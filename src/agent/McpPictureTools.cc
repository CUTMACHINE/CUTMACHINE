// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// pictures: shot quality, contact and cut sheets, cut inspection and review,
// frames, shot descriptions, stable windows and isolated frames. Moved out of
// McpTools.cc as they were; what they share with the other files of the catalog
// is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

// QC-2026-09 A4 -- the read-only half: which clips play a frame of a rush,
// and where. Answers the question an editor asks of a transcript or a
// contact sheet ("where is 1412 in the montage?") without asking them to
// subtract two times.
bool DispatchLocateSourceFrame(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id",
                                                      "source_frame"};
    if (!CheckKnownKeys(args, kAllowed, "locate_source_frame", message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    int64_t sourceFrame = 0;
    if (!ReadId(args, "media_id", "locate_source_frame", resolver, true,
                mediaId, message) ||
        !ReadInt64(args, "source_frame", "locate_source_frame", true, 0,
                   sourceFrame, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    std::vector<SourceFrameMatch> matches;
    if (!ResolveSourceFrame(document, mediaId, sourceFrame, matches, message))
        return Fail(errorName, message, message);
    resultJson = DescribeSourceFrameMatches(mediaId, sourceFrame, matches);
    return true;
}

bool DispatchListShotQuality(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "list_shot_quality", message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    // BENCH-2026-09 (B3) -- a cache this build cannot read is named under
    // unreadable_caches, never passed off as a source never measured, and
    // nothing here re-runs an analysis on its own. It no longer refuses the
    // whole view: one stale cache hid every other source's grades.
    std::map<Ulid, ShotQualityReport> reports;
    std::vector<Ulid> seen;
    std::vector<UnreadableShotQualityCache> unreadable;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (std::find(seen.begin(), seen.end(), clip.source_id) !=
                seen.end()) {
                continue;
            }
            seen.push_back(clip.source_id);
            ShotQualityReport report;
            const ShotQualityCacheRead read =
                backend.ReadSourceShotQuality(clip.source_id, report);
            if (read.state == ShotQualityCacheState::Loaded) {
                reports.emplace(clip.source_id, std::move(report));
            } else if (read.state == ShotQualityCacheState::Unreadable) {
                unreadable.push_back({clip.source_id, read});
            }
        }
    }
    resultJson =
        DescribeShotQualityForAgent(document, reports, ShotQualityThresholds{},
                                    ShotSegmentationSettings{}, unreadable);
    return true;
}

bool DispatchTimelineSheet(McpBackend& backend, const Value& args,
                           TimelineSheetKind kind, const std::string& toolName,
                           std::string& resultJson, std::string& errorName,
                           std::string& message) {
    static const std::vector<std::string> kAllowed = {"max_images", "graded"};
    if (!CheckKnownKeys(args, kAllowed, toolName, message))
        return Fail(errorName, message, message);
    // PRD39 -- ROADMAP.md. The engine's default, the one the command line
    // reads too (TimelineSheets.h).
    int32_t maximumImages = TimelineSheetSettings{}.maximum_images;
    // DEC29 -- docs/roadmap/GRADE-2026-09.md. The edit as it is delivered,
    // each cell graded with the sequence look, as read_frame graded shows
    // one clip.
    bool graded = false;
    if (!ReadInt32(args, "max_images", toolName, false, maximumImages,
                   maximumImages, message) ||
        !ReadBool(args, "graded", toolName, false, graded, message))
        return Fail(errorName, message, message);
    if (maximumImages <= 0 || maximumImages > 64)
        return Fail(errorName, message,
                    "'" + toolName + ".max_images' must be between 1 and 64");
    if (kind == TimelineSheetKind::Cuts && maximumImages < 2)
        return Fail(errorName, message,
                    "'cut_sheet.max_images' must be at least 2");

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    TimelineSheetSettings settings;
    settings.maximum_images = maximumImages;
    settings.graded = graded;
    TimelineSheetPlan plan;
    if (!BuildTimelineSheetPlan(document, kind, settings, plan, message))
        return Fail(errorName, message, message);
    // PRD39 -- nothing to show, as --contact-sheet and --cut-sheet say it:
    // a refusal, not an IoError from the render.
    if (plan.frames.empty())
        return Fail(errorName, message, EmptyTimelineSheetReason(plan));
    std::string jpeg;
    if (!backend.CaptureTimelineSheet(plan, settings, jpeg, message)) {
        errorName = "IoError";
        return false;
    }
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey,
                 Value::MakeString(SerializeTimelineSheetPlan(plan, graded)));
    resultJson = envelope.Dump();
    return true;
}

bool DispatchContactSheet(McpBackend& backend, const IdResolver&,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    return DispatchTimelineSheet(backend, args, TimelineSheetKind::Contact,
                                 "contact_sheet", resultJson, errorName,
                                 message);
}

// The derush sheet: rushes rather than an edit (BuildMediaSheetPlan). The
// backend renders it exactly as it renders a contact sheet.
bool DispatchMediaSheet(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id", "media_ids",
                                                      "max_images"};
    // DEC29 -- docs/roadmap/GRADE-2026-09.md. Said by name rather than as an
    // unknown key: the sheets of an edit take it, and a rush has nothing
    // for it to show.
    if (args.IsObject() && args.Find("graded") != nullptr)
        return Fail(errorName, message,
                    "'media_sheet' has no 'graded': a rush has no grade "
                    "until a clip places it; contact_sheet or read_frame "
                    "with graded show the clips");
    if (!CheckKnownKeys(args, kAllowed, "media_sheet", message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    std::vector<Ulid> mediaIds;
    if (!ReadId(args, "media_id", "media_sheet", resolver, false, mediaId,
                message) ||
        !ReadIdArray(args, "media_ids", "media_sheet", resolver, mediaIds,
                     message))
        return Fail(errorName, message, message);
    if (mediaId.empty() == mediaIds.empty())
        return Fail(errorName, message,
                    "'media_sheet' takes exactly one of 'media_id' or "
                    "'media_ids'");
    // PRD39 -- ROADMAP.md. One default, the engine's, for one rush as for
    // several and for --media-sheet alike: it was 12 for one rush here, and
    // 12 in every case on the command line.
    int32_t maximumImages = TimelineSheetSettings{}.maximum_images;
    if (!ReadInt32(args, "max_images", "media_sheet", false, maximumImages,
                   maximumImages, message))
        return Fail(errorName, message, message);
    if (maximumImages <= 0 || maximumImages > 64)
        return Fail(errorName, message,
                    "'media_sheet.max_images' must be between 1 and 64");
    if (!mediaId.empty()) mediaIds.push_back(mediaId);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    TimelineSheetSettings settings;
    settings.maximum_images = maximumImages;
    TimelineSheetPlan plan;
    if (!BuildMediaSheetPlan(document, mediaIds, settings, plan, message)) {
        if (message.rfind("unknown media_id", 0) == 0) {
            errorName = EditErrorName(EditError::UnknownMedia);
            return false;
        }
        return Fail(errorName, message, message);
    }
    std::string jpeg;
    if (!backend.CaptureTimelineSheet(plan, settings, jpeg, message)) {
        errorName = "IoError";
        return false;
    }
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey,
                 Value::MakeString(SerializeTimelineSheetPlan(plan)));
    resultJson = envelope.Dump();
    return true;
}

bool DispatchCutSheet(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    return DispatchTimelineSheet(backend, args, TimelineSheetKind::Cuts,
                                 "cut_sheet", resultJson, errorName, message);
}

// IA4 -- docs/roadmap/IA-2026-09.md. The engine resolves the window and the
// frames (CutInspection.h); the backend renders them through the export's
// graph and reads them back. What the agent receives is the report as text
// beside the pictures, like a sheet.
bool DispatchInspectCut(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "at_tail",    "position", "before", "after",
        "images",  "transcribe", "language", "band"};
    if (!CheckKnownKeys(args, kAllowed, "inspect_cut", message))
        return Fail(errorName, message, message);
    const bool hasClip = args.Find("clip_id") != nullptr;
    const bool hasPosition = args.Find("position") != nullptr;
    if (hasClip == hasPosition)
        return Fail(errorName, message,
                    "'inspect_cut' takes exactly one of 'clip_id' or "
                    "'position'");
    CutInspectionRequest request;
    CutInspectionRenderSettings settings;
    RationalTime position{0, 1};
    if ((hasClip && !ReadId(args, "clip_id", "inspect_cut", resolver, true,
                            request.clip_id, message)) ||
        !ReadBool(args, "at_tail", "inspect_cut", false, request.at_tail,
                  message) ||
        (hasPosition &&
         !ReadTime(args, "position", "inspect_cut", true, position, message)) ||
        !ReadTime(args, "before", "inspect_cut", false, request.before,
                  message) ||
        !ReadTime(args, "after", "inspect_cut", false, request.after,
                  message) ||
        !ReadInt32(args, "images", "inspect_cut", false, request.images,
                   request.images, message) ||
        !ReadBool(args, "transcribe", "inspect_cut", false, settings.transcribe,
                  message) ||
        !ReadString(args, "language", "inspect_cut", false, "auto",
                    settings.language, message) ||
        // RC2 -- docs/roadmap/RACCORDS-2026-10.md.
        !ReadBool(args, "band", "inspect_cut", false, settings.cut_band,
                  message))
        return Fail(errorName, message, message);
    if (hasPosition) request.position = position;
    if (hasPosition && args.Find("at_tail") != nullptr)
        return Fail(errorName, message,
                    "'inspect_cut.at_tail' belongs with 'clip_id'");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    CutInspectionPlan plan;
    if (!PlanCutInspection(document, request, plan, message))
        return Fail(errorName, message, message);
    CutInspectionResult result;
    if (!backend.InspectCut(plan, settings, result, message)) {
        errorName = "IoError";
        return false;
    }
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey,
                 Value::MakeString(EncodeBase64(result.jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(SerializeCutInspection(
                                       plan, result.audio, result.words)));
    resultJson = envelope.Dump();
    return true;
}

// RC1 -- docs/roadmap/RACCORDS-2026-10.md. Every junction of the timeline,
// or those named, rendered by the backend's InspectCut one window at a time
// and measured by the engine (CutReview.h); the bands come back stacked in
// one picture beside the report. The project is read once before and once
// after: an edit in between would mix two montages in one report.
bool DispatchReviewCuts(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "positions", "clip_ids", "side", "images_per_side", "max_bands"};
    if (!CheckKnownKeys(args, kAllowed, "review_cuts", message))
        return Fail(errorName, message, message);
    CutReviewRequest request;
    std::vector<Ulid> clipIds;
    if (!ReadIdArray(args, "clip_ids", "review_cuts", resolver, clipIds,
                     message) ||
        !ReadTime(args, "side", "review_cuts", false, request.side, message) ||
        !ReadInt32(args, "images_per_side", "review_cuts", false,
                   request.images_per_side, request.images_per_side, message) ||
        !ReadInt32(args, "max_bands", "review_cuts", false, request.max_bands,
                   request.max_bands, message))
        return Fail(errorName, message, message);
    std::vector<RationalTime> positions;
    if (const Value* field = args.Find("positions")) {
        if (!field->IsArray())
            return Fail(errorName, message,
                        "'review_cuts.positions' must be an array");
        for (size_t index = 0; index < field->AsArray().size(); ++index) {
            RationalTime position;
            if (!ReadTimeValue(
                    field->AsArray()[index],
                    "review_cuts.positions[" + std::to_string(index) + "]",
                    position, message))
                return Fail(errorName, message, message);
            positions.push_back(position);
        }
    }
    Document document;
    std::string revision;
    if (!backend.SnapshotDocument(document, message) ||
        !backend.Revision(revision, message)) {
        errorName = "IoError";
        return false;
    }
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0)
        return Fail(errorName, message, "the timeline has no frame rate");
    for (size_t index = 0; index < positions.size(); ++index) {
        const RationalTime& position = positions[index];
        const int64_t frame = position.to_frames(rate.num, rate.den);
        if (RationalTime{frame * rate.den, rate.num} != position)
            return Fail(errorName, message,
                        "'review_cuts.positions[" + std::to_string(index) +
                            "]' is not a whole frame of the timeline");
        request.frames.push_back(frame);
    }
    for (const Ulid& clipId : clipIds) {
        const DocumentClip* found = nullptr;
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "video" && track.kind != "audio") continue;
            for (const DocumentClip& clip : track.clips) {
                if (clip.id == clipId) found = &clip;
            }
        }
        if (found == nullptr)
            return Fail(errorName, message,
                        "clip '" + clipId +
                            "' is on no video or audio track of this "
                            "timeline");
        const int64_t frame = found->timeline_in.to_frames(rate.num, rate.den);
        if (RationalTime{frame * rate.den, rate.num} != found->timeline_in)
            return Fail(errorName, message,
                        "clip '" + clipId +
                            "' does not start on a frame of the timeline");
        request.frames.push_back(frame);
    }
    CutReviewReport report;
    std::vector<std::string> bands;
    const CutInspectionRenderer render =
        [&backend](const CutInspectionPlan& plan,
                   const CutInspectionRenderSettings& settings,
                   CutInspectionResult& result, std::string& error) {
            return backend.InspectCut(plan, settings, result, error);
        };
    if (!ReviewCuts(document, request, render, report, bands, message)) {
        // A request the engine refuses is the caller's; a render that fails
        // is the machine's.
        errorName = message.find("the window of the junction") == 0
                        ? "IoError"
                        : "ValidationFailed";
        return false;
    }
    std::string after;
    if (!backend.Revision(after, message)) {
        errorName = "IoError";
        return false;
    }
    if (after != revision)
        return Fail(errorName, message,
                    "the project changed during the review: its report would "
                    "mix two montages; ask again");
    const std::string text = SerializeCutReview(report);
    if (bands.empty()) {
        resultJson = text;
        return true;
    }
    std::string jpeg;
    if (!StackInspectionPictures(bands, std::string(), jpeg, message)) {
        errorName = "IoError";
        return false;
    }
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(text));
    resultJson = envelope.Dump();
    return true;
}

bool DispatchReadFrame(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "media_id", "source_in", "position", "graded"};
    if (!CheckKnownKeys(args, kAllowed, "read_frame", message))
        return Fail(errorName, message, message);
    const bool hasClip = args.Find("clip_id") != nullptr;
    const bool hasMedia = args.Find("media_id") != nullptr;
    if (hasClip == hasMedia)
        return Fail(errorName, message,
                    "'read_frame' takes exactly one of 'clip_id' or "
                    "'media_id'");
    // G1b -- docs/roadmap/GRADE-2026-09.md. The frame as the sequence
    // delivers the clip: its grade and the sequence look.
    bool graded = false;
    if (!ReadBool(args, "graded", "read_frame", false, graded, message))
        return Fail(errorName, message, message);
    if (graded && hasMedia)
        return Fail(errorName, message,
                    "'read_frame.graded' needs a 'clip_id': a media has no "
                    "grade until a clip places it");

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    Ulid sourceId;
    RationalTime time;
    // R1 -- docs/roadmap/REEL-2026-09.md. Set when the frame belongs to a
    // clip: the backend then composes it as the sequence does.
    Ulid framedClipId;
    if (hasClip) {
        Ulid clipId;
        std::string position;
        if (!ReadId(args, "clip_id", "read_frame", resolver, true, clipId,
                    message) ||
            !ReadString(args, "position", "read_frame", false, "Middle",
                        position, message))
            return Fail(errorName, message, message);
        if (args.Find("source_in") != nullptr)
            return Fail(errorName, message,
                        "'read_frame.source_in' belongs with 'media_id'; a "
                        "clip is addressed by 'position' instead");
        const DocumentClip* clip = document.FindClip(clipId);
        if (clip == nullptr) {
            errorName = EditErrorName(EditError::UnknownClip);
            message = "unknown clip_id '" + clipId + "'";
            return false;
        }
        sourceId = clip->source_id;
        framedClipId = clip->id;
        // The same trade create_interview_short makes: a named place in the
        // shot, resolved by the engine (FrameCapture.h).
        if (!ClipFramePositionTime(*clip, position, time)) {
            return Fail(errorName, message,
                        "'read_frame.position' must be 'Start', 'Middle' or "
                        "'End'");
        }
        // MOTION-2026-09 M4 -- a motion clip has no media to decode: it is
        // shown as the engine renders it at that instant of the timeline.
        if (IsMotionClip(*clip)) {
            return RenderMotionClipFrame(
                backend, clipId,
                clip->timeline_in.add(time.sub(clip->source_in)), {},
                resultJson, errorName, message);
        }
    } else {
        if (!ReadId(args, "media_id", "read_frame", resolver, true, sourceId,
                    message) ||
            !ReadTime(args, "source_in", "read_frame", true, time, message))
            return Fail(errorName, message, message);
    }

    const LibraryMedia* media = document.FindLibraryMedia(sourceId);
    if (media && media->metadata_complete && !media->has_video) {
        errorName = EditErrorName(EditError::InvalidOperation);
        message = "media_id '" + sourceId +
                  "' is audio-only; read_frame requires a video stream";
        return false;
    }
    // The frame that time falls in, from its first instant: a time inside
    // a frame would be read as the next one (SourceFrameStart, ClipSpeed.h).
    if (const DocumentSource* source = document.FindSource(sourceId)) {
        time = SourceFrameStart(time, source->rate);
    }

    std::string jpeg;
    const bool captured =
        graded
            ? backend.CaptureGradedClipFrame(framedClipId, time, jpeg, message)
            : backend.CaptureSourceFrame(sourceId, framedClipId, time, jpeg,
                                         message);
    if (!captured) {
        errorName = "IoError";
        return false;
    }
    Value described = Value::MakeObject();
    described.Set("source_id", Value::MakeString(sourceId));
    if (graded) described.Set("graded", Value::MakeBool(true));
    Value at = Value::MakeObject();
    at.Set("value", Value::MakeInt(time.value));
    at.Set("rate", Value::MakeInt(time.rate));
    described.Set("source_in", std::move(at));
    described.Set("bytes", Value::MakeInt(static_cast<int64_t>(jpeg.size())));

    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(described.Dump()));
    resultJson = envelope.Dump();
    return true;
}

bool DispatchAnalyzeShotQuality(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id", "media_ids"};
    if (!CheckKnownKeys(args, kAllowed, "analyze_shot_quality", message))
        return Fail(errorName, message, message);
    std::vector<Ulid> mediaIds;
    bool batch = false;
    if (!ReadAnalysisMediaIds(args, "analyze_shot_quality", resolver, mediaIds,
                              batch, message))
        return Fail(errorName, message, message);
    return RunAnalysisBatch(
        mediaIds, batch, "analyze_shot_quality",
        [&](const Ulid& mediaId, std::string& json, std::string& error) {
            return backend.AnalyzeSourceShotQuality(mediaId, json, error);
        },
        resultJson, errorName, message);
}

// R8 -- docs/roadmap/REEL-2026-09.md. The two halves of the isolated-frame
// pass: one that measures and one that only reads, sharing a backend entry
// point so the ranges they work on cannot differ.
bool DispatchScanIsolatedFrames(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, bool analyze,
                                const std::string& tool,
                                std::string& resultJson, std::string& errorName,
                                std::string& message) {
    if (!CheckKnownKeys(args, {"media_ids"}, tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> mediaIds;
    if (!ReadIdArray(args, "media_ids", tool, resolver, mediaIds, message))
        return Fail(errorName, message, message);
    if (args.Find("media_ids") != nullptr && mediaIds.empty())
        return Fail(errorName, message,
                    "'" + tool + ".media_ids' names no media");
    return backend.ScanIsolatedFrames(analyze, mediaIds, resultJson, message)
               ? true
               : Fail(errorName, message, message);
}

bool DispatchAnalyzeIsolatedFrames(McpBackend& backend,
                                   const IdResolver& resolver,
                                   const Value& args, std::string& resultJson,
                                   std::string& errorName,
                                   std::string& message) {
    return DispatchScanIsolatedFrames(backend, resolver, args, true,
                                      "analyze_isolated_frames", resultJson,
                                      errorName, message);
}

bool DispatchListIsolatedFrames(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    return DispatchScanIsolatedFrames(backend, resolver, args, false,
                                      "list_isolated_frames", resultJson,
                                      errorName, message);
}

// ITW6 -- ROADMAP.md. Where a rush holds still, in its own frames.
// PL2 -- docs/roadmap/PLANS-2026-10.md. Now, in UTC, as a described shot
// records it. Read here, at the surface: the operation carries the text, so
// replaying the journal gives the same document whatever the clock says.
std::string ShotTimestampNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

bool ReadClosedName(const Value& object, const std::string& key,
                    const std::string& path, bool required,
                    const std::vector<std::string>& names, std::string& out,
                    std::string& message) {
    if (!ReadString(object, key, path, required, "", out, message))
        return false;
    if (!required && object.Find(key) == nullptr) return true;
    if (std::find(names.begin(), names.end(), out) == names.end()) {
        std::string known;
        for (const std::string& name : names)
            known += (known.empty() ? "" : ", ") + name;
        message = "'" + path + "." + key + "' takes one of: " + known;
        return false;
    }
    return true;
}

// One entry of describe_shots.shots, into `shot`. The author, its kind and
// the timestamp come from the call, not from the entry.
bool ReadShotEntry(const Value& item, const std::string& path,
                   ShotDescription& shot, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "shot_id", "source_in", "duration", "size",    "subjects", "action",
        "place",   "movement",  "quality",  "reserve", "keywords"};
    if (!item.IsObject()) {
        message = "'" + path + "' must be an object";
        return false;
    }
    for (const auto& entry : item.AsObject()) {
        if (std::find(kAllowed.begin(), kAllowed.end(), entry.first) ==
            kAllowed.end()) {
            message = "'" + path + "' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    std::string id;
    if (!ReadString(item, "shot_id", path, false, "", id, message))
        return false;
    shot.id = id.empty() ? GenerateUlid() : id;
    if (!ReadTime(item, "source_in", path, true, shot.source_in, message) ||
        !ReadTime(item, "duration", path, true, shot.duration, message))
        return false;
    std::string size;
    std::string movement;
    std::string quality;
    if (!ReadClosedName(item, "size", path, true, ShotSizeNames(), size,
                        message) ||
        !ReadClosedName(item, "movement", path, true, CameraMovementNames(),
                        movement, message) ||
        !ReadClosedName(item, "quality", path, false, ShotQualityNames(),
                        quality, message))
        return false;
    ParseShotSize(size, shot.size);
    ParseCameraMovement(movement, shot.movement);
    shot.quality = ShotQuality::Good;
    if (!quality.empty()) ParseShotQuality(quality, shot.quality);
    if (!ReadStringArray(item, "subjects", path, shot.subjects, message) ||
        !ReadStringArray(item, "keywords", path, shot.keywords, message) ||
        !ReadString(item, "action", path, false, "", shot.action, message) ||
        !ReadString(item, "place", path, false, "", shot.place, message) ||
        !ReadString(item, "reserve", path, false, "", shot.reserve, message))
        return false;
    return true;
}

Value ShotValue(const LibraryMedia& media, const ShotDescription& shot) {
    Value item = Value::MakeObject();
    item.Set("shot_id", Value::MakeString(shot.id));
    item.Set("media_id", Value::MakeString(media.id));
    item.Set("filename", Value::MakeString(media.filename));
    item.Set("source_in", BeatTimeValue(shot.source_in));
    item.Set("duration", BeatTimeValue(shot.duration));
    item.Set("size", Value::MakeString(ShotSizeName(shot.size)));
    Value subjects = Value::MakeArray();
    for (const std::string& subject : shot.subjects)
        subjects.Push(Value::MakeString(subject));
    item.Set("subjects", std::move(subjects));
    item.Set("action", Value::MakeString(shot.action));
    item.Set("place", Value::MakeString(shot.place));
    item.Set("movement", Value::MakeString(CameraMovementName(shot.movement)));
    item.Set("quality", Value::MakeString(ShotQualityName(shot.quality)));
    if (!shot.reserve.empty())
        item.Set("reserve", Value::MakeString(shot.reserve));
    Value keywords = Value::MakeArray();
    for (const std::string& keyword : shot.keywords)
        keywords.Push(Value::MakeString(keyword));
    item.Set("keywords", std::move(keywords));
    item.Set("author_kind",
             Value::MakeString(ShotAuthorKindName(shot.author_kind)));
    item.Set("author", Value::MakeString(shot.author));
    item.Set("described_at", Value::MakeString(shot.described_at));
    // PL3 -- where to look, in the arguments the two tools take: the
    // middle frame of the stretch for read_frame, the rush for media_sheet.
    if (media.rate.num > 0 && media.rate.den > 0) {
        const int64_t first =
            shot.source_in.to_frames(media.rate.num, media.rate.den);
        const int64_t end = shot.source_in.add(shot.duration)
                                .to_frames(media.rate.num, media.rate.den);
        const int64_t middle =
            first + (std::max<int64_t>(end - first, 1) - 1) / 2;
        Value look = Value::MakeObject();
        Value frame = Value::MakeObject();
        frame.Set("media_id", Value::MakeString(media.id));
        frame.Set("source_in", BeatTimeValue(RationalTime{
                                   middle * media.rate.den, media.rate.num}));
        look.Set("read_frame", std::move(frame));
        Value sheet = Value::MakeObject();
        sheet.Set("media_id", Value::MakeString(media.id));
        look.Set("media_sheet", std::move(sheet));
        item.Set("look", std::move(look));
    }
    return item;
}

// PL2 -- docs/roadmap/PLANS-2026-10.md. Describe stretches of one rush once,
// keep them in the document. One operation, one undo step.
bool DispatchDescribeShots(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string tool = "describe_shots";
    static const std::vector<std::string> kAllowed = {
        "media_id", "shots",       "remove_shot_ids",
        "author",   "author_kind", "described_at"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    std::string author;
    std::string authorKind;
    std::string describedAt;
    std::vector<std::string> removed;
    if (!ReadId(args, "media_id", tool, resolver, true, mediaId, message) ||
        !ReadString(args, "author", tool, false, "", author, message) ||
        !ReadClosedName(args, "author_kind", tool, false, ShotAuthorKindNames(),
                        authorKind, message) ||
        !ReadString(args, "described_at", tool, false, ShotTimestampNow(),
                    describedAt, message) ||
        !ReadStringArray(args, "remove_shot_ids", tool, removed, message))
        return Fail(errorName, message, message);
    std::vector<ShotDescription> written;
    if (const Value* shots = args.Find("shots")) {
        if (!shots->IsArray())
            return Fail(errorName, message,
                        "'" + tool + ".shots' must be an array");
        if (shots->AsArray().size() > kMaxShotsPerMedia)
            return Fail(errorName, message,
                        "'" + tool + ".shots' holds more than " +
                            std::to_string(kMaxShotsPerMedia) + " entries");
        ShotAuthorKind kind = ShotAuthorKind::Model;
        if (!authorKind.empty()) ParseShotAuthorKind(authorKind, kind);
        for (size_t index = 0; index < shots->AsArray().size(); ++index) {
            ShotDescription shot;
            if (!ReadShotEntry(shots->AsArray()[index],
                               tool + ".shots[" + std::to_string(index) + "]",
                               shot, message))
                return Fail(errorName, message, message);
            shot.author_kind = kind;
            shot.author = author;
            shot.described_at = describedAt;
            written.push_back(std::move(shot));
        }
    }
    if (written.empty() && removed.empty())
        return Fail(errorName, message,
                    "'" + tool + "' needs shots to write or remove_shot_ids");
    if (!written.empty() && author.empty())
        return Fail(errorName, message,
                    "'" + tool +
                        ".author' is required: the model's name, or the "
                        "person's, who wrote these descriptions");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(mediaId);
    if (media == nullptr) {
        errorName = EditErrorName(EditError::UnknownMedia);
        message = "unknown media_id '" + mediaId + "'";
        return false;
    }
    std::vector<ShotDescription> merged;
    if (!MergeShotDescriptions(*media, written, removed, merged, message))
        return Fail(errorName, message, "'" + tool + "': " + message);
    std::string applied;
    if (!backend.ApplyOperation(SetMediaShotsOperation{mediaId, merged},
                                applied, errorName, message))
        return false;
    Value reply = Value::MakeObject();
    reply.Set("applied", Value::MakeBool(true));
    reply.Set("media_id", Value::MakeString(mediaId));
    Value ids = Value::MakeArray();
    for (const ShotDescription& shot : written)
        ids.Push(Value::MakeString(shot.id));
    reply.Set("written_shot_ids", std::move(ids));
    Value gone = Value::MakeArray();
    for (const std::string& id : removed) gone.Push(Value::MakeString(id));
    reply.Set("removed_shot_ids", std::move(gone));
    reply.Set("described_shots",
              Value::MakeInt(static_cast<int64_t>(merged.size())));
    Value result;
    std::string parseError;
    if (Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

bool ReadClosedNames(const Value& args, const std::string& key,
                     const std::string& path,
                     const std::vector<std::string>& names,
                     std::vector<std::string>& out, std::string& message) {
    if (!ReadStringArray(args, key, path, out, message)) return false;
    for (size_t index = 0; index < out.size(); ++index) {
        if (std::find(names.begin(), names.end(), out[index]) != names.end())
            continue;
        std::string known;
        for (const std::string& name : names)
            known += (known.empty() ? "" : ", ") + name;
        message = "'" + path + "." + key + "[" + std::to_string(index) +
                  "]' takes one of: " + known;
        return false;
    }
    return true;
}

// PL3 -- docs/roadmap/PLANS-2026-10.md. The described shots that answer a
// question, read from the document: no picture is decoded, no model asked.
bool DispatchSearchShots(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string tool = "search_shots";
    static const std::vector<std::string> kAllowed = {
        "media_ids", "sizes",       "movements",    "qualities",
        "text",      "author_kind", "min_duration", "limit"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    ShotQuery query;
    std::vector<std::string> sizes;
    std::vector<std::string> movements;
    std::vector<std::string> qualities;
    std::string authorKind;
    int32_t limit = 50;
    if (!ReadIdArray(args, "media_ids", tool, resolver, query.media_ids,
                     message) ||
        !ReadClosedNames(args, "sizes", tool, ShotSizeNames(), sizes,
                         message) ||
        !ReadClosedNames(args, "movements", tool, CameraMovementNames(),
                         movements, message) ||
        !ReadClosedNames(args, "qualities", tool, ShotQualityNames(), qualities,
                         message) ||
        !ReadClosedName(args, "author_kind", tool, false, ShotAuthorKindNames(),
                        authorKind, message) ||
        !ReadString(args, "text", tool, false, "", query.text, message) ||
        !ReadTime(args, "min_duration", tool, false, query.minimum_duration,
                  message) ||
        !ReadInt32(args, "limit", tool, false, limit, limit, message))
        return Fail(errorName, message, message);
    if (limit < 1 || limit > 500)
        return Fail(errorName, message,
                    "'" + tool + ".limit' must be between 1 and 500");
    for (const std::string& name : sizes) {
        ShotSize value;
        ParseShotSize(name, value);
        query.sizes.push_back(value);
    }
    for (const std::string& name : movements) {
        CameraMovement value;
        ParseCameraMovement(name, value);
        query.movements.push_back(value);
    }
    for (const std::string& name : qualities) {
        ShotQuality value;
        ParseShotQuality(name, value);
        query.qualities.push_back(value);
    }
    if (!authorKind.empty()) {
        ShotAuthorKind value;
        ParseShotAuthorKind(authorKind, value);
        query.author_kind = value;
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    for (const Ulid& mediaId : query.media_ids) {
        if (document.FindLibraryMedia(mediaId) == nullptr) {
            errorName = EditErrorName(EditError::UnknownMedia);
            message = "unknown media_id '" + mediaId + "'";
            return false;
        }
    }
    const std::vector<ShotMatch> matches = SearchShots(document.library, query);
    Value reply = Value::MakeObject();
    reply.Set("total", Value::MakeInt(static_cast<int64_t>(matches.size())));
    Value shots = Value::MakeArray();
    for (size_t index = 0;
         index < matches.size() && index < static_cast<size_t>(limit); ++index)
        shots.Push(ShotValue(*matches[index].media, *matches[index].shot));
    reply.Set("shots", std::move(shots));
    // PL4 -- what the search could not see: a rush with a picture and no
    // description answers nothing, whatever it shows.
    Value undescribed = Value::MakeArray();
    for (const LibraryMedia& media : document.library) {
        if (!query.media_ids.empty() &&
            std::find(query.media_ids.begin(), query.media_ids.end(),
                      media.id) == query.media_ids.end())
            continue;
        if ((media.metadata_complete && !media.has_video) ||
            !media.shots.empty())
            continue;
        Value item = Value::MakeObject();
        item.Set("media_id", Value::MakeString(media.id));
        item.Set("filename", Value::MakeString(media.filename));
        undescribed.Push(std::move(item));
    }
    reply.Set("undescribed_media", std::move(undescribed));
    resultJson = reply.Dump();
    return true;
}

bool DispatchListStableWindows(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    const std::string tool = "list_stable_windows";
    if (!CheckKnownKeys(args,
                        {"media_id", "max_speed", "max_zoom_per_mille",
                         "min_sharpness_percent", "min_duration_ms"},
                        tool, message))
        return Fail(errorName, message, message);
    Ulid mediaId;
    StableWindowSettings settings;
    if (!ReadId(args, "media_id", tool, resolver, true, mediaId, message) ||
        !ReadInt64(args, "max_speed", tool, false, settings.maximum_speed,
                   settings.maximum_speed, message) ||
        !ReadInt64(args, "max_zoom_per_mille", tool, false,
                   settings.maximum_zoom_per_mille,
                   settings.maximum_zoom_per_mille, message) ||
        !ReadInt64(args, "min_sharpness_percent", tool, false,
                   settings.minimum_sharpness_percent,
                   settings.minimum_sharpness_percent, message) ||
        !ReadInt64(args, "min_duration_ms", tool, false,
                   settings.minimum_duration_milliseconds,
                   settings.minimum_duration_milliseconds, message))
        return Fail(errorName, message, message);
    if (settings.maximum_speed < 0 || settings.maximum_zoom_per_mille < 0 ||
        settings.minimum_sharpness_percent < 0 ||
        settings.minimum_duration_milliseconds < 0)
        return Fail(errorName, message,
                    "'list_stable_windows' limits must not be negative");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(mediaId);
    int32_t displayWidth = 0;
    int32_t displayHeight = 0;
    if (media == nullptr || !media->has_video ||
        !DisplayDimensions(*media, displayWidth, displayHeight)) {
        return Fail(errorName, message,
                    "media_id '" + mediaId + "' has no displayable video");
    }
    ShotQualityReport report;
    const ShotQualityCacheRead read =
        backend.ReadSourceShotQuality(mediaId, report);
    if (read.state == ShotQualityCacheState::Unreadable) {
        // BENCH-2026-09 (B3) -- the same refusal as list_shot_quality.
        const std::vector<UnreadableShotQualityCache> unreadable = {
            {mediaId, read}};
        errorName = kUnreadableCacheError;
        message = DescribeUnreadableShotQualityDetail(unreadable);
        resultJson = DescribeUnreadableShotQualityCaches(unreadable);
        return false;
    }
    if (read.state != ShotQualityCacheState::Loaded) {
        errorName = "IoError";
        message = "no shot quality analysis for '" + mediaId +
                  "'; run analyze_shot_quality: " + read.detail;
        return false;
    }
    std::vector<StableWindow> windows;
    RushStillness stillness;
    if (!ListStableWindows(report, media->rate, displayWidth, displayHeight,
                           settings, windows, message, &stillness))
        return Fail(errorName, message, message);
    Value root = Value::MakeObject();
    root.Set("media_id", Value::MakeString(mediaId));
    // 6 November 2026 -- an empty list alone read as "unusable"; see
    // RushStillness.
    Value rush = Value::MakeObject();
    rush.Set("median_speed", Value::MakeInt(stillness.median_speed));
    rush.Set("too_fast_percent", Value::MakeInt(stillness.too_fast_percent));
    rush.Set("too_much_zoom_percent",
             Value::MakeInt(stillness.too_much_zoom_percent));
    rush.Set("too_soft_percent", Value::MakeInt(stillness.too_soft_percent));
    rush.Set("longest_still", BeatTimeValue(stillness.longest_still));
    rush.Set("measured_intervals",
             Value::MakeInt(stillness.measured_intervals));
    rush.Set("unmeasured_intervals",
             Value::MakeInt(stillness.unmeasured_intervals));
    root.Set("rush", std::move(rush));
    Value list = Value::MakeArray();
    int64_t undescribedWindows = 0;
    for (const StableWindow& window : windows) {
        Value item = Value::MakeObject();
        item.Set("first_frame", Value::MakeInt(window.first_frame));
        item.Set("end_frame", Value::MakeInt(window.end_frame));
        Value sourceIn = Value::MakeObject();
        sourceIn.Set("value",
                     Value::MakeInt(window.first_frame * media->rate.den));
        sourceIn.Set("rate", Value::MakeInt(media->rate.num));
        item.Set("source_in", std::move(sourceIn));
        Value duration = Value::MakeObject();
        duration.Set("value",
                     Value::MakeInt((window.end_frame - window.first_frame) *
                                    media->rate.den));
        duration.Set("rate", Value::MakeInt(media->rate.num));
        item.Set("duration", std::move(duration));
        item.Set("median_speed", Value::MakeInt(window.median_speed));
        item.Set("peak_speed", Value::MakeInt(window.peak_speed));
        item.Set("lowest_sharpness_percent",
                 Value::MakeInt(window.lowest_sharpness_percent));
        // PL4 -- docs/roadmap/PLANS-2026-10.md. The descriptions this
        // window already has; empty, it is still to describe.
        const RationalTime windowIn{window.first_frame * media->rate.den,
                                    media->rate.num};
        const RationalTime windowEnd{window.end_frame * media->rate.den,
                                     media->rate.num};
        Value shotIds = Value::MakeArray();
        for (const ShotDescription* shot :
             ShotsOverlapping(*media, windowIn, windowEnd))
            shotIds.Push(Value::MakeString(shot->id));
        if (shotIds.AsArray().empty()) ++undescribedWindows;
        item.Set("shot_ids", std::move(shotIds));
        list.Push(std::move(item));
    }
    root.Set("windows", std::move(list));
    root.Set("undescribed_windows", Value::MakeInt(undescribedWindows));
    resultJson = root.Dump();
    return true;
}

}  // namespace

void RegisterPictureTools(const McpToolAdder& add) {
    add("locate_source_frame",
        "Find where a frame of a rush is playing on the timeline: which clip, "
        "on which track, and at what timeline position. A frame can be on the "
        "timeline more than once -- a picture and its detached sound, or one "
        "rush used twice -- so every match is listed rather than one being "
        "chosen. Use this rather than computing a timeline position from a "
        "clip's source_in by hand.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush the frame belongs to."), true)
            .Field("source_frame",
                   IntSchema("Frame index in that media's own frame rate."),
                   true)
            .Build("locate_source_frame arguments"),
        DispatchLocateSourceFrame);

    add("list_shot_quality",
        "Report measured picture quality for every video clip on the timeline: "
        "whether each is Sharp/Soft/Blurry and Steady/Moving/Shaky, with the "
        "numbers behind each grade. Use it to keep soft or unsteady shots out "
        "of a cut. Clips whose source has not been analysed are listed under "
        "\"unanalyzed\" and are not a pass: they are unknown. An old "
        "unreadable cache is listed in \"unreadable_caches\" with the call "
        "that rebuilds it. \"sources\" lists, per analysed source, the shots "
        "detected inside it with their source-domain start, end and sharpest "
        "keyframe -- what the rushes hold, including takes the timeline never "
        "used.",
        SchemaBuilder().Build("list_shot_quality arguments"),
        DispatchListShotQuality);

    add("contact_sheet",
        "Look at the edit as one JPEG grid. Samples the middle frame of each "
        "visible shot across the whole timeline; clips hidden under a higher "
        "visible video track are omitted. The document display colour "
        "transform is applied, so log rushes are shown as the edit presents "
        "them, but without each clip's grade or the sequence look. Exact clip, "
        "source and timeline addresses accompany every cell.",
        SchemaBuilder()
            .Field("max_images",
                   PositiveIntSchema("Maximum cells (default 24, maximum "
                                     "64). Longer edits are sampled evenly."),
                   false)
            .Field("graded",
                   BoolSchema("Show each clip graded, with the sequence "
                              "look, through the SDR view Record uses. "
                              "Defaults to false."),
                   false)
            .Build("contact_sheet arguments"),
        DispatchContactSheet);

    add("cut_sheet",
        "Look at every sampled visible cut as adjacent before/after frames in "
        "one JPEG grid. Each pair is the last sequence frame before the cut "
        "and the first at it, after higher-track compositing and through the "
        "document display colour transform. Exact clip, source and cut "
        "addresses accompany every cell.",
        SchemaBuilder()
            .Field("max_images",
                   PositiveIntSchema("Maximum cells, kept in complete pairs "
                                     "(default 24, maximum 64)."),
                   false)
            .Field("graded",
                   BoolSchema("Show each clip graded, with the sequence "
                              "look, through the SDR view Record uses. "
                              "Defaults to false."),
                   false)
            .Build("cut_sheet arguments"),
        DispatchCutSheet);

    add("media_sheet",
        "Look at rushes rather than an edit, as one JPEG grid: the derush of a "
        "shoot, with nothing on a timeline. One media_id gives max_images "
        "frames spread across the take, each the middle of an equal part of "
        "it; media_ids gives the middle frame of each rush, sampled evenly "
        "past max_images. Cells take the first rush's own shape, shown through "
        "the project's colour transform, ungraded. Every cell names its media "
        "and exact source_in, which read_frame and an insert take as they are.",
        SchemaBuilder()
            .Field("media_id", IdSchema("One rush, seen across its length."),
                   false)
            .Field("media_ids",
                   IdArraySchema("Several rushes, one frame each."), false)
            .Field("max_images",
                   PositiveIntSchema("Maximum cells (default 24, maximum 64)."),
                   false)
            .Build("media_sheet arguments"),
        DispatchMediaSheet);

    // IA4 -- docs/roadmap/IA-2026-09.md.
    add("inspect_cut",
        "Look at the montage AS IT IS DELIVERED around one cut: a short window "
        "is rendered by the export's own graph, sound included, and read back. "
        "Returns the chosen frames as one JPEG strip (the last frame before "
        "the cut and the first at it always among them) with their exact "
        "frames and times, the mix's level frame by frame (dBFS) and on either "
        "side of the cut, the words the montage is expected to say in the "
        "window, and `alerts` to examine: a word a cut runs through, and -- "
        "with `transcribe` and a local Whisper model configured -- words "
        "expected but not heard in a fresh reading of the window's mix, or "
        "heard but not expected. Use it instead of read_frame or cut_sheet "
        "when what matters is what the film shows and says there, not what the "
        "rush holds. Name the cut by `clip_id` (its head, or its tail with "
        "`at_tail`) or by a timeline `position`; `before` and `after` say how "
        "much of the montage to render on either side (1 s each by default, at "
        "most 5 s, whole frames of the timeline). An HLG delivery is shown "
        "through the SDR view, as Record shows it.",
        SchemaBuilder()
            .Field("clip_id",
                   IdSchema("Clip whose head (or tail, with at_tail) is the "
                            "cut to inspect."),
                   false)
            .Field("at_tail",
                   BoolSchema("Inspect the clip's tail instead of its head."),
                   false)
            .Field("position", kTimeSchemaText, false)
            .Field("before", kTimeSchemaText, false)
            .Field("after", kTimeSchemaText, false)
            .Field("images",
                   PositiveIntSchema("Frames returned as pictures, 2 to 12 "
                                     "(default 6)."),
                   false)
            .Field("transcribe",
                   BoolSchema("Read the window's mix with the local Whisper "
                              "model and compare it with the expected words. "
                              "Refused when no model is configured."),
                   false)
            .Field("language",
                   StringSchema("Language of the reading, ISO 639-1, or "
                                "'auto' (default)."),
                   false)
            .Field("band",
                   BoolSchema("Show the pictures as a cut band: the frames "
                              "before the cut, a red bar, the frames from "
                              "it, and the window's waveform under them with "
                              "the cut in red."),
                   false)
            .Build("inspect_cut arguments"),
        DispatchInspectCut);

    // RC1 -- docs/roadmap/RACCORDS-2026-10.md.
    add("review_cuts",
        "Watch and listen to every junction of the montage -- where a clip of "
        "a visible video track or an audible audio track meets the next one -- "
        "as the film delivers it: each window is rendered by inspect_cut's "
        "export graph and measured. Measures the sound (ambience, voice, the "
        "30 ms at the cut), the words it crosses and the picture on either "
        "side. A measure past its declared threshold (reported in "
        "`thresholds`) raises an alert TO EXAMINE, never a verdict: "
        "ambience_jump, voice_jump, sound_cut_live (a word, breath or note cut "
        "short), word_crosses_cut, jump_cut, motion_interrupted. A through "
        "edit (the source read on, speed 1) raises none. Returns the report "
        "and one picture of bands, alerts first; each junction names its "
        "`band`. Call it before handing a montage over, after the last edit; "
        "then inspect_cut (with transcribe) on what it flags. Every junction "
        "by default (at most 60; the rest listed in not_reviewed). Renders one "
        "window per junction: seconds each.",
        SchemaBuilder()
            .Field("positions",
                   ArraySchema(kTimeSchemaText,
                               "Timeline positions of the junctions to "
                               "review, whole frames."),
                   false)
            .Field("clip_ids",
                   IdArraySchema("Clips whose heads are the junctions to "
                                 "review."),
                   false)
            .Field("side",
                   TimeSchema("Montage rendered on either side of each cut, "
                              "whole frames; 1 s by default, 5 s at most."),
                   false)
            .Field("images_per_side",
                   PositiveIntSchema("Pictures of each band on either side "
                                     "of the cut, 1 to 4 (default 3)."),
                   false)
            .Field("max_bands",
                   IntSchema("Bands in the picture, 0 to 12 (default 6); "
                             "junctions with alerts first."),
                   false)
            .Build("review_cuts arguments"),
        DispatchReviewCuts);

    add("read_frame",
        "Look at one frame: the picture itself, for what no measurement tells "
        "(a cutaway of someone talking over other dialogue reads as a sync "
        "error). Shown through the project's colour management, without the "
        "clip's grade or the sequence look unless graded.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to look at."), false)
            .Field("position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Which part of the clip to show. Defaults to "
                              "Middle. Only with clip_id."),
                   false)
            .Field("media_id",
                   IdSchema("Library media to look at, for a shot that is "
                            "not on the timeline yet."),
                   false)
            .Field("source_in", kTimeSchemaText, false)
            .Field("graded",
                   BoolSchema("Only with clip_id: show the clip graded, with "
                              "the sequence look, through the SDR view "
                              "Record uses. Defaults to false."),
                   false)
            .Build("read_frame arguments"),
        DispatchReadFrame);

    add("analyze_shot_quality",
        "Measure a source's picture quality and cache the result, so "
        "list_shot_quality has something to report. Decodes the whole media, "
        "so it takes seconds to a minute depending on length; the result is "
        "cached and only needs redoing when the media changes.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Media to analyse."), false)
            .Field("media_ids",
                   IdArraySchema("Media to analyse in one call, in place of "
                                 "media_id: one line per media under "
                                 "`results`, each with its own report or "
                                 "error."),
                   false)
            .Build("analyze_shot_quality arguments"),
        DispatchAnalyzeShotQuality);

    // PL2 -- docs/roadmap/PLANS-2026-10.md.
    add("describe_shots",
        "Write down, once, what stretches of one rush show, so that no later "
        "edit has to look at them again: search_shots then finds a cutaway in "
        "the document instead of in contact sheets. Look first (media_sheet, "
        "read_frame, list_stable_windows), then give one entry per stretch, in "
        "the rush's own time. Free fields short, concrete, in the project's "
        "language. The descriptions live in the project file and are kept by "
        "every timeline.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush described."), true)
            .Field(
                "shots",
                ArraySchema(
                    std::string("{\"type\":\"object\",\"properties\":{") +
                        "\"shot_id\":" +
                        StringSchema("Existing description to replace; "
                                     "omit for a new one.") +
                        ",\"source_in\":" + kTimeSchemaText +
                        ",\"duration\":" + kTimeSchemaText + ",\"size\":" +
                        EnumSchema(ShotSizeNames(), "Shot size.") +
                        ",\"subjects\":" +
                        ArraySchema("{\"type\":\"string\"}",
                                    "Who or what is seen.") +
                        ",\"action\":" + StringSchema("What happens.") +
                        ",\"place\":" + StringSchema("Where.") +
                        ",\"movement\":" +
                        EnumSchema(CameraMovementNames(), "Camera movement.") +
                        ",\"quality\":" +
                        EnumSchema(ShotQualityNames(), "good by default.") +
                        ",\"reserve\":" +
                        StringSchema("What is wrong with it, if "
                                     "anything.") +
                        ",\"keywords\":" +
                        ArraySchema("{\"type\":\"string\"}",
                                    "Free search words.") +
                        "},\"required\":[\"source_in\",\"duration\","
                        "\"size\",\"movement\"],"
                        "\"additionalProperties\":false}",
                    "Stretches to describe."),
                false)
            .Field("remove_shot_ids",
                   ArraySchema("{\"type\":\"string\"}",
                               "Descriptions of this rush to drop."),
                   false)
            .Field("author",
                   StringSchema("Who wrote the descriptions: the model's name, "
                                "or the person's."),
                   false)
            .Field("author_kind",
                   EnumSchema(ShotAuthorKindNames(), "model by default."),
                   false)
            .Field("described_at",
                   StringSchema("YYYY-MM-DDTHH:MM:SSZ, UTC; now by default."),
                   false)
            .Build("describe_shots arguments"),
        DispatchDescribeShots);

    // PL3 -- docs/roadmap/PLANS-2026-10.md.
    add("search_shots",
        "Find described shots (describe_shots) without looking at a single "
        "frame: filter on the closed fields (sizes, movements, qualities, "
        "author_kind, min_duration, media_ids; any of the values within one "
        "filter, every filter at once) and on words of text, each of which "
        "must appear in the subjects, action, place, keywords or reserve, "
        "ignoring case and accents (\"velo\" finds \"vélos\"). Answers in "
        "library order, never ranked, with each shot's exact source_in and "
        "duration to cut from and `look`: the read_frame and media_sheet "
        "arguments that show it. undescribed_media lists the rushes in scope "
        "that have a picture and no description: the search cannot see "
        "them, describe them once.",
        SchemaBuilder()
            .Field("media_ids",
                   IdArraySchema("Rushes to search; all by "
                                 "default."),
                   false)
            .Field("sizes",
                   ArraySchema(EnumSchema(ShotSizeNames(), ""),
                               "Shot sizes accepted."),
                   false)
            .Field("movements",
                   ArraySchema(EnumSchema(CameraMovementNames(), ""),
                               "Camera movements accepted."),
                   false)
            .Field("qualities",
                   ArraySchema(EnumSchema(ShotQualityNames(), ""),
                               "Qualities accepted."),
                   false)
            .Field("author_kind",
                   EnumSchema(ShotAuthorKindNames(),
                              "Only descriptions by a model, or by a person."),
                   false)
            .Field("text",
                   StringSchema("Words that must all appear in the free "
                                "fields."),
                   false)
            .Field("min_duration", kTimeSchemaText, false)
            .Field("limit",
                   IntSchema("Shots returned, 1 to 500 (default 50); total "
                             "counts them all."),
                   false)
            .Build("search_shots arguments"),
        DispatchSearchShots);

    add("list_stable_windows",
        "List where a rush holds still: stretches whose camera moves no "
        "faster than max_speed (pixels per second of a frame 1080 wide, "
        "absolute, measured by phase correlation), zooms no more than "
        "max_zoom_per_mille per second, and stays at least "
        "min_sharpness_percent of the rush's median sharpness, for at least "
        "min_duration_ms. Windows are in the rush's own frames and come with "
        "an exact source_in and duration to cut from. Use it to choose "
        "cutaways under a \"no moving shot\" rule. Needs analyze_shot_quality, "
        "run since camera motion was measured. `rush` sums up the whole rush, "
        "window or not: no window says the camera moves or the picture "
        "softens, not that the shot is unusable. Whether a move is meant (a "
        "follow that keeps its subject, or a take's end that drops it) shows "
        "on the picture, not in these numbers.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Rush to search."), true)
            .Field("max_speed",
                   IntSchema("Camera speed limit in px/s at 1080 width "
                             "(default 80: handheld drift passes, a pan "
                             "does not)."),
                   false)
            .Field("max_zoom_per_mille",
                   IntSchema("Zoom limit per second, in thousandths of scale "
                             "(default 150; the estimate reads parallax as "
                             "zoom, so handheld footage sits near 50)."),
                   false)
            .Field("min_sharpness_percent",
                   IntSchema("Least sharpness against the rush's median "
                             "(default 72)."),
                   false)
            .Field("min_duration_ms",
                   IntSchema("Shortest window listed (default 1200)."), false)
            .Build("list_stable_windows arguments"),
        DispatchListStableWindows);

    add("list_isolated_frames",
        "Report the single frames the rushes hold that their neighbours do not "
        "-- a studio flash, a black frame, a corrupted one -- over the source "
        "ranges this timeline actually reads, at each clip's speed, transition "
        "handles included. Measured frame by frame; list_shot_quality samples "
        "four times a second and cannot see any of this. Each frame is "
        "reported with its deviation in luma levels, the level it returned to, "
        "and the clips that play it. A flash can also be where a shot is meant "
        "to start. Media with no scan, or whose scan does not cover every "
        "range this timeline uses, are listed under \"unscanned\" and are not "
        "a pass: run analyze_isolated_frames.",
        SchemaBuilder()
            .Field("media_ids",
                   IdArraySchema("Rushes to report whole, whether the "
                                 "timeline uses them or not."),
                   false)
            .Build("list_isolated_frames arguments"),
        DispatchListIsolatedFrames);

    add("analyze_isolated_frames",
        "Measure every frame of the source ranges this timeline reads and "
        "cache the result, so list_isolated_frames has something to report. "
        "Decodes each rush up to the last frame the timeline uses, so it costs "
        "seconds per rush rather than the whole media; the result is cached "
        "and only needs redoing when the edit reaches frames it does not "
        "cover. A whole 4K rush (media_ids) takes about a quarter of its "
        "duration. This analysis does not edit the project.",
        SchemaBuilder()
            .Field("media_ids",
                   IdArraySchema("Rushes to measure whole, whether the "
                                 "timeline uses them or not."),
                   false)
            .Build("analyze_isolated_frames arguments"),
        DispatchAnalyzeIsolatedFrames);
}
