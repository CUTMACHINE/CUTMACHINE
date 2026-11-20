// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// the edits of clips on a timeline: insertion and overwrite, removal, trims,
// moves, splits, the edits of linked clips, paste, and multicam groups. Moved
// out of McpTools.cc as they were; what they share with the other files of the
// catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

// QC-2026-09 A4 -- the same field on split_clip, trim_clip and ripple_trim:
// the frame of the rush the caller means, which the engine converts into a
// timeline position or a delta.
std::string SourceFrameSchema() {
    return IntSchema(
        "Frame of the clip's own source, in its own rate: at a head the first "
        "frame played, at a tail the last (inclusive). Prefer it when you "
        "decided on the rush.");
}

bool ReadEdge(const Value& args, const std::string& key,
              const std::string& path, TrimEdge& out, std::string& message) {
    std::string text;
    if (!ReadString(args, key, path, true, "", text, message)) return false;
    if (text == "Head") {
        out = TrimEdge::Head;
        return true;
    }
    if (text == "Tail") {
        out = TrimEdge::Tail;
        return true;
    }
    message = "'" + path + "." + key + "' must be 'Head' or 'Tail'";
    return false;
}

// ---------------------------------------------------------------------
// Per-tool dispatch. Each function validates, resolves IDs, builds the
// matching Operations.h struct, and ends by handing it to `backend` --
// which is EditLog::Apply (in-memory backend) or ApplyOperationCommand
// (project-file backend), never a hand-rolled mutation.
// ---------------------------------------------------------------------

bool DispatchInsertClip(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "track_id",    "source_id",      "source_in", "duration",
        "timeline_in", "sync_track_ids", "speed"};
    if (!CheckKnownKeys(args, kAllowed, "insert_clip", message))
        return Fail(errorName, message, message);
    const bool syncTracksExplicit = args.Find("sync_track_ids") != nullptr;
    InsertClipOperation op;
    if (!ReadId(args, "track_id", "insert_clip", resolver, true, op.track_id,
                message) ||
        !ReadId(args, "source_id", "insert_clip", resolver, true, op.source_id,
                message) ||
        !ReadTime(args, "source_in", "insert_clip", true, op.source_in,
                  message) ||
        !ReadTime(args, "duration", "insert_clip", true, op.duration,
                  message) ||
        !ReadTime(args, "timeline_in", "insert_clip", true, op.timeline_in,
                  message) ||
        !ReadIdArray(args, "sync_track_ids", "insert_clip", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    // R4 -- docs/roadmap/REEL-2026-09.md. A clip laid at a speed; 0/1 holds
    // the frame source_in falls in, from that frame's start (a held frame is
    // a whole frame, and Document::Validate keeps a non-unit speed on one).
    if (const Value* speed = args.Find("speed")) {
        if (!ReadFractionValue(*speed, "insert_clip.speed", op.speed.num,
                               op.speed.den, message)) {
            return Fail(errorName, message, message);
        }
        if (IsFreezeSpeed(op.speed)) {
            op.speed = {0, 1};
            Document document;
            if (!backend.SnapshotDocument(document, message)) {
                errorName = "IoError";
                return false;
            }
            if (const DocumentSource* source =
                    document.FindSource(op.source_id)) {
                op.source_in = SourceFrameStart(op.source_in, source->rate);
            }
        }
    }

    // QC-2026-09 C1 -- an omitted sync_track_ids used to ripple only
    // track_id and desync any A/V-linked partner living elsewhere without
    // any signal at all. Surface it the same way ripple_trim does: apply as
    // requested, then flag tracks that hold material at or after
    // timeline_in and were not asked to follow.
    std::vector<Ulid> warningTrackIds;
    if (!syncTracksExplicit) {
        Document document;
        if (!backend.SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        // AGENT-2026-09 A34 -- tried: every other sync-locked track
        // following by default, as ripple_trim now does. Withdrawn the same
        // day: an insert is how a cutaway lands on its own track and how a
        // rush's sound is laid beside its picture, and both pushed the
        // speech tracks by the inserted length (the native run of the 26th
        // saw its picture track slide under the sound it was laying). An
        // insert moves nothing else and says which tracks hold material
        // downstream; sync_track_ids names the tracks that must follow.
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.id == op.track_id) continue;
            if (std::any_of(track.clips.begin(), track.clips.end(),
                            [&](const DocumentClip& clip) {
                                return clip.timeline_in >= op.timeline_in;
                            }))
                warningTrackIds.push_back(track.id);
        }
    }

    std::string appliedJson;
    if (!backend.ApplyOperation(op, appliedJson, errorName, message))
        return false;
    Value result;
    std::string parseError;
    if (!Value::Parse(appliedJson, result, parseError) || !result.IsObject())
        result = Value::MakeObject();
    if (!warningTrackIds.empty()) {
        // PRD15 -- in English, as the engine's other answers to an agent;
        // it was the catalog's one French sentence outside examples.
        result.Set(
            "warning",
            Value::MakeString("sync_track_ids omitted: clips downstream on the "
                              "tracks in warning_track_ids were not shifted"));
        Value tracks = Value::MakeArray();
        for (const Ulid& trackId : warningTrackIds)
            tracks.Push(Value::MakeString(trackId));
        result.Set("warning_track_ids", std::move(tracks));
    }
    resultJson = result.Dump();
    return true;
}

// ADOBE-CLI-2026-09 -- reuse the monitor's exact-track edits, including
// preview validation and reserved split/insert identities. Applying them
// against another snapshot would silently discard intervening mouse edits.
bool DispatchPreparedRangeEdit(McpBackend& backend, const Document& document,
                               DeleteGapOperation operation, bool preview,
                               Value response, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    Document candidate = document;
    EditLog log;
    EditError error = EditError::None;
    if (!log.Apply(candidate, Operation{std::move(operation)}, error,
                   message)) {
        errorName = EditErrorName(error);
        return false;
    }
    const Operation& prepared = log.AppliedEntries().back().op;
    Value canonical;
    std::string parseError;
    if (!Value::Parse(SerializeOperation(prepared), canonical, parseError)) {
        return Fail(errorName, message, parseError);
    }
    Value affected = Value::MakeArray();
    for (const auto& state :
         std::get<DeleteGapOperation>(prepared).exact_track_result) {
        affected.Push(Value::MakeString(state.track_id));
    }
    response.Set("ok", Value::MakeBool(true));
    response.Set("preview", Value::MakeBool(preview));
    response.Set("timeline_id", Value::MakeString(document.sequence.id));
    response.Set("affected_track_ids", std::move(affected));
    response.Set("operation", std::move(canonical));
    if (!preview) {
        std::string applied;
        if (!backend.ApplyPreparedOperation(prepared, document.SaveToString(),
                                            applied, errorName, message)) {
            return false;
        }
        // PRD40 -- ROADMAP.md. The doc_hash and the delta every edit
        // returns: delete_timeline_range and edit_source_range dropped them.
        Value appliedView;
        if (Value::Parse(applied, appliedView, parseError) &&
            appliedView.IsObject()) {
            for (const char* key : {"doc_hash", "delta"}) {
                if (const Value* value = appliedView.Find(key)) {
                    response.Set(key, *value);
                }
            }
        }
    }
    resultJson = response.Dump();
    return true;
}

// PRD14 -- ROADMAP.md. What a refusal about a track can offer instead: the
// unlocked tracks of `kind`, each by alias and ID, in describe's order. An
// agent told only that `track_id` was required went back to describe for a
// ULID to copy; told which tracks would do, it sends the call again.
std::string UnlockedTracksOnOffer(const Document& document,
                                  const std::string& kind) {
    const std::vector<const DocumentTrack*> tracks =
        TracksInDescribeOrder(document.sequence);
    std::string offer;
    for (size_t ordinal = 0; ordinal < tracks.size(); ++ordinal) {
        const DocumentTrack& track = *tracks[ordinal];
        if (track.kind != kind || track.locked) continue;
        const std::string alias = TrackAlias(tracks, ordinal);
        offer += offer.empty() ? "" : ", ";
        offer += alias.empty() ? track.id : alias + " (" + track.id + ")";
    }
    if (offer.empty()) {
        return "this timeline has no unlocked " + kind +
               " track (add_track adds one, set_track_lock unlocks one)";
    }
    return "the unlocked " + kind + " tracks of this timeline are " + offer;
}

// Why `id` cannot be the target of `kind` the caller meant it to be, or
// empty when it can.
std::string TrackTargetProblem(const Document& document, const Ulid& id,
                               const std::string& kind) {
    const DocumentTrack* track = document.FindTrack(id);
    if (track == nullptr) return "names no track of this timeline";
    if (track->kind != kind) {
        return std::string("names ") + (track->kind == "audio" ? "an " : "a ") +
               track->kind + " track";
    }
    if (track->locked) return "names a locked track";
    return std::string();
}

bool DispatchEditSourceRange(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    const std::string tool = "edit_source_range";
    if (!CheckKnownKeys(
            args,
            {"source_id", "track_id", "source_in", "source_out", "timeline_in",
             "timeline_out", "audio_track_ids", "mode", "preview"},
            tool, message)) {
        return Fail(errorName, message, message);
    }
    // PRD14 -- read before the arguments rather than after: a refusal names
    // what this timeline offers.
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    Ulid sourceId;
    Ulid trackId;
    std::vector<Ulid> audioTrackIds;
    std::string mode;
    bool preview = false;
    if (!ReadId(args, "source_id", tool, resolver, true, sourceId, message)) {
        return Fail(errorName, message, message);
    }
    // A track that is missing, or named by something that resolves to
    // nothing ("V1"), is answered with the tracks that would do.
    if (!ReadId(args, "track_id", tool, resolver, true, trackId, message)) {
        return Fail(errorName, message,
                    message + ": " + UnlockedTracksOnOffer(document, "video"));
    }
    if (!ReadIdArray(args, "audio_track_ids", tool, resolver, audioTrackIds,
                     message)) {
        return Fail(errorName, message,
                    message + ": " + UnlockedTracksOnOffer(document, "audio"));
    }
    if (args.Find("mode") == nullptr) {
        return Fail(errorName, message,
                    "'edit_source_range.mode' is required: 'Insert' or "
                    "'Overwrite'");
    }
    if (!ReadString(args, "mode", tool, true, "", mode, message) ||
        !ReadBool(args, "preview", tool, false, preview, message)) {
        return Fail(errorName, message, message);
    }
    if (mode != "Insert" && mode != "Overwrite") {
        return Fail(errorName, message,
                    "'edit_source_range.mode' must be 'Insert' or 'Overwrite'");
    }
    // PRD14 -- the engine answers a wrong target with one sentence for
    // every cause ("requires an unlocked video target and unlocked audio
    // targets"). Said here per argument, with the tracks that would do.
    if (const std::string problem =
            TrackTargetProblem(document, trackId, "video");
        !problem.empty()) {
        return Fail(errorName, message,
                    "'edit_source_range.track_id' " + problem + ": " +
                        UnlockedTracksOnOffer(document, "video"));
    }
    for (size_t index = 0; index < audioTrackIds.size(); ++index) {
        const std::string field = "'edit_source_range.audio_track_ids[" +
                                  std::to_string(index) + "]' ";
        if (std::find(audioTrackIds.begin(), audioTrackIds.begin() + index,
                      audioTrackIds[index]) != audioTrackIds.begin() + index) {
            return Fail(errorName, message,
                        field +
                            "names a track already listed: each audio "
                            "target is named once");
        }
        if (const std::string problem =
                TrackTargetProblem(document, audioTrackIds[index], "audio");
            !problem.empty()) {
            return Fail(errorName, message,
                        field + problem + ": " +
                            UnlockedTracksOnOffer(document, "audio"));
        }
    }
    SourceRecordEditPoints points;
    const std::pair<const char*, std::optional<RationalTime>*> fields[] = {
        {"source_in", &points.source_in},
        {"source_out", &points.source_out},
        {"timeline_in", &points.timeline_in},
        {"timeline_out", &points.timeline_out}};
    for (const auto& [key, point] : fields) {
        if (args.Find(key) != nullptr) {
            RationalTime time;
            if (!ReadTime(args, key, tool, true, time, message)) {
                return Fail(errorName, message, message);
            }
            *point = time;
        }
    }
    const DocumentSource* source = document.FindSource(sourceId);
    if (source == nullptr) {
        return Fail(errorName, message,
                    "'edit_source_range.source_id' must identify a source");
    }
    // PRD14 -- the two halves of the engine's "a positive source duration
    // and a Record in or out are required", which named no argument and no
    // form. The three-point rule itself is ResolveSourceRecordEditRange's
    // and is unchanged: these are its first refusal, said for a caller.
    if (source->duration.rate <= 0 || source->duration.value <= 0) {
        return Fail(errorName, message,
                    "'edit_source_range.source_id': rush '" + sourceId +
                        "' has no duration on record, so no range of it can "
                        "be laid on a timeline");
    }
    if (!points.timeline_in && !points.timeline_out) {
        return Fail(
            errorName, message,
            "'edit_source_range' needs a Record point: give timeline_in or "
            "timeline_out, plus source_in and source_out to take less than "
            "the whole rush, each as an exact time {value, rate} (integers, "
            "rate in ticks per second) -- the start of this timeline is "
            "{\"value\":0,\"rate\":" +
                std::to_string(document.sequence.frame_rate.num) + "}");
    }
    SourceRecordEditRange range;
    if (!ResolveSourceRecordEditRange(source->duration, points, range,
                                      message)) {
        return Fail(errorName, message, message);
    }
    try {
        auto operation = TimelineSourceEditOperation(
            document, sourceId, range.source_in, range.source_out,
            range.timeline_in, trackId, mode == "Insert", audioTrackIds);
        if (!operation) {
            return Fail(errorName, message,
                        "Source range requires an unlocked video target and "
                        "unlocked audio targets");
        }
        Value response = Value::MakeObject();
        response.Set("mode", Value::MakeString(mode));
        const auto addTime = [&](const std::string& name, RationalTime time) {
            Value value = Value::MakeObject();
            value.Set("value", Value::MakeInt(time.value));
            value.Set("rate", Value::MakeInt(time.rate));
            response.Set(name, std::move(value));
        };
        addTime("source_in", range.source_in);
        addTime("source_out", range.source_out);
        addTime("timeline_in", range.timeline_in);
        addTime("timeline_out",
                range.timeline_in.add(range.source_out.sub(range.source_in)));
        return DispatchPreparedRangeEdit(
            backend, document, std::move(*operation), preview,
            std::move(response), resultJson, errorName, message);
    } catch (const std::exception& exception) {
        return Fail(errorName, message, exception.what());
    }
}

bool DispatchDeleteTimelineRange(McpBackend& backend,
                                 const IdResolver& resolver, const Value& args,
                                 std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    const std::string tool = "delete_timeline_range";
    if (!CheckKnownKeys(
            args, {"in", "out", "track_ids", "ripple", "followers", "preview"},
            tool, message)) {
        return Fail(errorName, message, message);
    }
    RationalTime in;
    RationalTime out;
    std::vector<Ulid> trackIds;
    bool ripple = false;
    bool preview = false;
    std::string followers;
    if (!ReadTime(args, "in", tool, true, in, message) ||
        !ReadTime(args, "out", tool, true, out, message) ||
        !ReadIdArray(args, "track_ids", tool, resolver, trackIds, message) ||
        !ReadBool(args, "ripple", tool, false, ripple, message) ||
        !ReadString(args, "followers", tool, false, "cut", followers,
                    message) ||
        !ReadBool(args, "preview", tool, false, preview, message)) {
        return Fail(errorName, message, message);
    }
    if (followers != "cut" && followers != "continuous") {
        return Fail(errorName, message,
                    "'delete_timeline_range.followers' must be 'cut' or "
                    "'continuous'");
    }
    if (in.value < 0 || out <= in) {
        return Fail(errorName, message,
                    "Record range requires 0 <= in < out (out is exclusive)");
    }
    if (trackIds.empty() ||
        std::set<Ulid>(trackIds.begin(), trackIds.end()).size() !=
            trackIds.size()) {
        return Fail(errorName, message,
                    "'delete_timeline_range.track_ids' must contain distinct "
                    "target tracks");
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    for (const auto& id : trackIds) {
        if (document.FindTrack(id) == nullptr) {
            return Fail(
                errorName, message,
                "'delete_timeline_range.track_ids' must identify tracks");
        }
    }
    try {
        auto operation = TimelineRangeDeleteOperation(
            document, in, out, ripple, trackIds,
            followers == "continuous" ? RangeDeleteFollowers::Continuous
                                      : RangeDeleteFollowers::Cut);
        if (!operation) {
            return Fail(errorName, message,
                        "Record range changes no unlocked target or "
                        "synchronized track");
        }
        Value response = Value::MakeObject();
        response.Set("ripple", Value::MakeBool(ripple));
        response.Set("followers", Value::MakeString(followers));
        response.Set("in", *args.Find("in"));
        response.Set("out", *args.Find("out"));
        return DispatchPreparedRangeEdit(
            backend, document, std::move(*operation), preview,
            std::move(response), resultJson, errorName, message);
    } catch (const std::exception& exception) {
        return Fail(errorName, message, exception.what());
    }
}

bool DispatchRemoveClip(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id",
                                                      "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "remove_clip", message))
        return Fail(errorName, message, message);
    RemoveClipOperation op;
    if (!ReadId(args, "clip_id", "remove_clip", resolver, true, op.clip_id,
                message) ||
        !ReadIdArray(args, "sync_track_ids", "remove_clip", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchClearClip(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id"};
    if (!CheckKnownKeys(args, kAllowed, "clear_clip", message))
        return Fail(errorName, message, message);
    ClearClipOperation op;
    if (!ReadId(args, "clip_id", "clear_clip", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchClearClips(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "clear_clips", message))
        return Fail(errorName, message, message);
    ClearClipsOperation op;
    if (!ReadIdArray(args, "clip_ids", "clear_clips", resolver, op.clip_ids,
                     message))
        return Fail(errorName, message, message);
    if (op.clip_ids.empty()) {
        message = "'clear_clips.clip_ids' must contain at least one clip";
        return Fail(errorName, message, message);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// QC-2026-09 A4 -- shared by trim_clip and ripple_trim: read the edge, then
// either the delta the caller measured or the source frame it wants that
// edge to land on. Exactly one, never both, and never a precedence rule --
// two answers to one question is how a montage ends up a frame out with
// nobody able to say where.
bool ReadTrimAmount(McpBackend& backend, const Value& args,
                    const std::string& tool, const Ulid& clipId, TrimEdge edge,
                    RationalTime& delta, std::string& errorName,
                    std::string& message) {
    const bool hasDelta = args.Find("delta") != nullptr;
    const bool hasSourceFrame = args.Find("source_frame") != nullptr;
    if (hasDelta == hasSourceFrame) {
        message = "'" + tool +
                  "' takes either 'delta' or 'source_frame', and exactly one "
                  "of them";
        return Fail(errorName, message, message);
    }
    if (hasDelta)
        return ReadTime(args, "delta", tool, true, delta, message)
                   ? true
                   : Fail(errorName, message, message);
    int64_t sourceFrame = 0;
    if (!ReadInt64(args, "source_frame", tool, true, 0, sourceFrame, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    if (ResolveClipSourceFrameTrim(document, clipId, sourceFrame, edge, delta,
                                   message)) {
        return true;
    }
    errorName = EditErrorName(EditError::InvalidOperation);
    return false;
}

bool DispatchTrimClip(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "edge",
                                                      "delta", "source_frame"};
    if (!CheckKnownKeys(args, kAllowed, "trim_clip", message))
        return Fail(errorName, message, message);
    TrimClipOperation op;
    if (!ReadId(args, "clip_id", "trim_clip", resolver, true, op.clip_id,
                message) ||
        !ReadEdge(args, "edge", "trim_clip", op.edge, message))
        return Fail(errorName, message, message);
    if (!ReadTrimAmount(backend, args, "trim_clip", op.clip_id, op.edge,
                        op.delta, errorName, message))
        return false;
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveClip(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "track_id",
                                                      "timeline_in"};
    if (!CheckKnownKeys(args, kAllowed, "move_clip", message))
        return Fail(errorName, message, message);
    MoveClipOperation op;
    if (!ReadId(args, "clip_id", "move_clip", resolver, true, op.clip_id,
                message) ||
        !ReadId(args, "track_id", "move_clip", resolver, true, op.track_id,
                message) ||
        !ReadTime(args, "timeline_in", "move_clip", true, op.timeline_in,
                  message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSplitClip(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "timeline_position", "source_frame"};
    if (!CheckKnownKeys(args, kAllowed, "split_clip", message))
        return Fail(errorName, message, message);
    SplitClipOperation op;
    if (!ReadId(args, "clip_id", "split_clip", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    // QC-2026-09 A4 -- a cut is decided in the rush ("just after he says the
    // name"), and the timeline position is a conversion. Naming the source
    // frame lets the engine do it; naming both would be two answers to one
    // question, so it is refused rather than resolved by precedence.
    const bool hasPosition = args.Find("timeline_position") != nullptr;
    const bool hasSourceFrame = args.Find("source_frame") != nullptr;
    if (hasPosition == hasSourceFrame)
        return Fail(errorName, message,
                    "'split_clip' takes either 'timeline_position' or "
                    "'source_frame', and exactly one of them");
    if (hasPosition) {
        if (!ReadTime(args, "timeline_position", "split_clip", true,
                      op.timeline_position, message))
            return Fail(errorName, message, message);
        return backend.ApplyOperation(op, resultJson, errorName, message);
    }
    int64_t sourceFrame = 0;
    if (!ReadInt64(args, "source_frame", "split_clip", true, 0, sourceFrame,
                   message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    if (!ResolveClipSourceFramePosition(document, op.clip_id, sourceFrame,
                                        op.timeline_position, message)) {
        errorName = EditErrorName(EditError::InvalidOperation);
        return false;
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSplitAtInterval(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "interval"};
    if (!CheckKnownKeys(args, kAllowed, "split_at_interval", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    RationalTime interval;
    if (!ReadId(args, "clip_id", "split_at_interval", resolver, true, clipId,
                message) ||
        !ReadTime(args, "interval", "split_at_interval", true, interval,
                  message))
        return Fail(errorName, message, message);

    // Its own snapshot: dispatch functions receive only the ID resolver, and
    // giving all 38 of them a Document parameter to serve the one tool that
    // resolves against clip geometry is the wrong trade. Nothing mutates
    // between McpToolRegistry::Call's snapshot and this one.
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }

    SplitClipAtPositionsOperation op;
    if (!ResolveIntervalSplits(document, clipId, interval, op, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchDeleteGap(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "track_id", "gap_start", "gap_duration", "linked_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "delete_gap", message))
        return Fail(errorName, message, message);
    DeleteGapOperation op;
    if (!ReadId(args, "track_id", "delete_gap", resolver, true, op.track_id,
                message) ||
        !ReadTime(args, "gap_start", "delete_gap", true, op.gap_start,
                  message) ||
        !ReadTime(args, "gap_duration", "delete_gap", true, op.gap_duration,
                  message) ||
        !ReadIdArray(args, "linked_track_ids", "delete_gap", resolver,
                     op.linked_track_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchDetachAudio(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"video_clip_id",
                                                      "audio_track_id"};
    if (!CheckKnownKeys(args, kAllowed, "detach_audio", message))
        return Fail(errorName, message, message);
    DetachAudioOperation op;
    if (!ReadId(args, "video_clip_id", "detach_audio", resolver, true,
                op.video_clip_id, message) ||
        !ReadId(args, "audio_track_id", "detach_audio", resolver, true,
                op.audio_track_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveLinkedClips(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id", "moves"};
    if (!CheckKnownKeys(args, kAllowed, "move_linked_clips", message))
        return Fail(errorName, message, message);
    MoveLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "move_linked_clips", resolver, true,
                op.link_group_id, message))
        return Fail(errorName, message, message);
    const Value* moves = args.Find("moves");
    if (!moves || !moves->IsArray() || moves->AsArray().empty())
        return Fail(errorName, message,
                    "'move_linked_clips.moves' must be a non-empty array");
    for (size_t index = 0; index < moves->AsArray().size(); ++index) {
        const Value& item = moves->AsArray()[index];
        const std::string path =
            "move_linked_clips.moves[" + std::to_string(index) + "]";
        static const std::vector<std::string> kMoveKeys = {
            "clip_id", "track_id", "timeline_in"};
        if (!CheckKnownKeys(item, kMoveKeys, path, message))
            return Fail(errorName, message, message);
        LinkedClipMove move;
        if (!ReadId(item, "clip_id", path, resolver, true, move.clip_id,
                    message) ||
            !ReadId(item, "track_id", path, resolver, true, move.track_id,
                    message) ||
            !ReadTime(item, "timeline_in", path, true, move.timeline_in,
                      message))
            return Fail(errorName, message, message);
        op.moves.push_back(move);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveClips(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"moves"};
    if (!CheckKnownKeys(args, kAllowed, "move_clips", message))
        return Fail(errorName, message, message);
    MoveClipsOperation op;
    const Value* moves = args.Find("moves");
    if (!moves || !moves->IsArray() || moves->AsArray().size() < 2)
        return Fail(errorName, message,
                    "'move_clips.moves' must contain at least two items");
    for (size_t index = 0; index < moves->AsArray().size(); ++index) {
        const Value& item = moves->AsArray()[index];
        const std::string path =
            "move_clips.moves[" + std::to_string(index) + "]";
        static const std::vector<std::string> kMoveKeys = {
            "clip_id", "track_id", "timeline_in"};
        if (!CheckKnownKeys(item, kMoveKeys, path, message))
            return Fail(errorName, message, message);
        LinkedClipMove move;
        if (!ReadId(item, "clip_id", path, resolver, true, move.clip_id,
                    message) ||
            !ReadId(item, "track_id", path, resolver, true, move.track_id,
                    message) ||
            !ReadTime(item, "timeline_in", path, true, move.timeline_in,
                      message))
            return Fail(errorName, message, message);
        op.moves.push_back(move);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchTrimLinkedClips(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id", "trims"};
    if (!CheckKnownKeys(args, kAllowed, "trim_linked_clips", message))
        return Fail(errorName, message, message);
    TrimLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "trim_linked_clips", resolver, true,
                op.link_group_id, message))
        return Fail(errorName, message, message);
    const Value* trims = args.Find("trims");
    if (!trims || !trims->IsArray() || trims->AsArray().empty())
        return Fail(errorName, message,
                    "'trim_linked_clips.trims' must be a non-empty array");
    for (size_t index = 0; index < trims->AsArray().size(); ++index) {
        const Value& item = trims->AsArray()[index];
        const std::string path =
            "trim_linked_clips.trims[" + std::to_string(index) + "]";
        static const std::vector<std::string> kTrimKeys = {"clip_id", "edge",
                                                           "delta"};
        if (!CheckKnownKeys(item, kTrimKeys, path, message))
            return Fail(errorName, message, message);
        LinkedClipTrim trim;
        if (!ReadId(item, "clip_id", path, resolver, true, trim.clip_id,
                    message) ||
            !ReadEdge(item, "edge", path, trim.edge, message) ||
            !ReadTime(item, "delta", path, true, trim.delta, message))
            return Fail(errorName, message, message);
        op.trims.push_back(trim);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchShortenLinkedClip(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "edge", "amount", "unit", "preview"};
    if (!CheckKnownKeys(args, kAllowed, "shorten_linked_clip", message))
        return Fail(errorName, message, message);

    Ulid clipId;
    TrimEdge edge = TrimEdge::Tail;
    int64_t amount = 0;
    std::string unit;
    bool preview = false;
    if (!ReadId(args, "clip_id", "shorten_linked_clip", resolver, true, clipId,
                message) ||
        !ReadEdge(args, "edge", "shorten_linked_clip", edge, message) ||
        !ReadInt64(args, "amount", "shorten_linked_clip", true, 0, amount,
                   message) ||
        !ReadString(args, "unit", "shorten_linked_clip", true, "", unit,
                    message) ||
        !ReadBool(args, "preview", "shorten_linked_clip", false, preview,
                  message))
        return Fail(errorName, message, message);
    if (amount <= 0)
        return Fail(errorName, message,
                    "'shorten_linked_clip.amount' must be positive");

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* anchor = document.FindClip(clipId);
    if (!anchor)
        return Fail(errorName, message,
                    "'shorten_linked_clip.clip_id' is not a clip");
    if (anchor->link_group_id.empty())
        return Fail(errorName, message,
                    "'shorten_linked_clip.clip_id' is not A/V-linked");

    RationalTime magnitude;
    if (unit == "Seconds") {
        magnitude = {amount, 1};
    } else if (unit == "Frames") {
        if (document.sequence.frame_rate.num <= 0 ||
            document.sequence.frame_rate.den <= 0)
            return Fail(errorName, message,
                        "the sequence frame rate must be positive");
        const __int128 ticks =
            static_cast<__int128>(amount) * document.sequence.frame_rate.den;
        if (ticks > std::numeric_limits<int64_t>::max())
            return Fail(errorName, message,
                        "'shorten_linked_clip.amount' overflows RationalTime");
        magnitude = {static_cast<int64_t>(ticks),
                     document.sequence.frame_rate.num};
    } else {
        return Fail(errorName, message,
                    "'shorten_linked_clip.unit' must be 'Frames' or 'Seconds'");
    }
    if (edge == TrimEdge::Tail) magnitude.value = -magnitude.value;

    TrimLinkedClipsOperation operation;
    operation.link_group_id = anchor->link_group_id;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.link_group_id == operation.link_group_id)
                operation.trims.push_back({clip.id, edge, magnitude});
        }
    }
    if (operation.trims.size() < 2)
        return Fail(errorName, message,
                    "the linked group has fewer than two members");

    if (!preview)
        return backend.ApplyOperation(operation, resultJson, errorName,
                                      message);

    Document candidate = document;
    EditLog previewLog;
    EditError previewError = EditError::None;
    std::string previewMessage;
    Operation previewOperation = operation;
    if (!previewLog.Apply(candidate, previewOperation, previewError,
                          previewMessage)) {
        errorName = EditErrorName(previewError);
        message = previewMessage;
        return false;
    }
    resultJson = "{\"ok\":true,\"preview\":true,\"operation\":" +
                 SerializeOperation(operation) + "}";
    return true;
}

bool DispatchRippleTrim(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id",         "edge",          "delta", "source_frame",
        "linked_clip_ids", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "ripple_trim", message))
        return Fail(errorName, message, message);
    const bool linkedIdsExplicit = args.Find("linked_clip_ids") != nullptr;
    const bool syncTracksExplicit = args.Find("sync_track_ids") != nullptr;
    RippleTrimOperation op;
    if (!ReadId(args, "clip_id", "ripple_trim", resolver, true, op.clip_id,
                message) ||
        !ReadEdge(args, "edge", "ripple_trim", op.edge, message) ||
        !ReadIdArray(args, "linked_clip_ids", "ripple_trim", resolver,
                     op.linked_clip_ids, message) ||
        !ReadIdArray(args, "sync_track_ids", "ripple_trim", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    if (!ReadTrimAmount(backend, args, "ripple_trim", op.clip_id, op.edge,
                        op.delta, errorName, message))
        return false;

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* anchor = document.FindClip(op.clip_id);
    if (!anchor) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + op.clip_id + "'";
        return false;
    }

    // QC-2026-09 B5 -- absence means the editing default (the whole A/V
    // group); a present array, including [], is an intentional subset.
    //
    // ITW7 -- ROADMAP.md. "The whole group" used to mean every clip sharing
    // the link id, and remove_words leaves one id on all the fragments of a
    // pair: lengthening the tail of the last fragment lengthened all of them,
    // on both tracks, and was refused as an Overlap. The partner of an edge
    // is, on each other track, the member whose same edge sits nearest; the
    // rule lives in the engine (LinkedEdgePartnerIds), shared with the
    // boundary-air tools.
    if (!linkedIdsExplicit) {
        op.linked_clip_ids = LinkedEdgePartnerIds(document, *anchor, op.edge);
    }

    std::vector<Ulid> warningTrackIds;
    if (!syncTracksExplicit) {
        std::vector<Ulid> affectedTrackIds;
        const auto addAffectedTrack = [&](const Ulid& clipId) {
            const DocumentTrack* track = document.FindTrackForClip(clipId);
            if (track &&
                std::find(affectedTrackIds.begin(), affectedTrackIds.end(),
                          track->id) == affectedTrackIds.end())
                affectedTrackIds.push_back(track->id);
        };
        addAffectedTrack(anchor->id);
        for (const Ulid& linkedId : op.linked_clip_ids)
            addAffectedTrack(linkedId);
        const RationalTime boundary =
            op.edge == TrimEdge::Head
                ? anchor->timeline_in
                : anchor->timeline_in.add(anchor->duration);
        // AGENT-2026-09 A34 -- absent, the sync tracks are the editing
        // default every other ripple tool uses (BoundarySyncTrackIds): every
        // other sync-locked track follows, as the Document's sync_lock
        // promises. This tool alone moved nothing else and only warned; on
        // the third run the cutaway track fell ten frames out of place and
        // the warning went unread. The warning now names only the tracks
        // whose sync lock is off and that hold clips downstream.
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (std::find(affectedTrackIds.begin(), affectedTrackIds.end(),
                          track.id) != affectedTrackIds.end())
                continue;
            if (track.sync_lock) {
                op.sync_track_ids.push_back(track.id);
                continue;
            }
            if (std::any_of(track.clips.begin(), track.clips.end(),
                            [&](const DocumentClip& clip) {
                                return clip.timeline_in >= boundary;
                            }))
                warningTrackIds.push_back(track.id);
        }
    }

    // AGENT-2026-09 A12 -- lengthening an edge onto frames the neighbouring
    // clip already plays from the same source repeats them on screen: what
    // was wanted at that junction is a roll. Said, never refused -- the
    // repeat may be wanted.
    Value repeats;
    const DocumentTrack* anchorTrack = document.FindTrackForClip(anchor->id);
    const bool lengthens = (op.edge == TrimEdge::Tail && op.delta.value > 0) ||
                           (op.edge == TrimEdge::Head && op.delta.value < 0);
    if (anchorTrack != nullptr && lengthens) {
        try {
            const RationalTime anchorEnd =
                anchor->timeline_in.add(anchor->duration);
            const DocumentClip* neighbour = nullptr;
            for (const DocumentClip& other : anchorTrack->clips) {
                if (other.id == anchor->id ||
                    other.source_id != anchor->source_id)
                    continue;
                if (op.edge == TrimEdge::Tail) {
                    if (other.timeline_in >= anchorEnd &&
                        (neighbour == nullptr ||
                         other.timeline_in < neighbour->timeline_in))
                        neighbour = &other;
                } else if (other.timeline_in.add(other.duration) <=
                               anchor->timeline_in &&
                           (neighbour == nullptr ||
                            other.timeline_in > neighbour->timeline_in)) {
                    neighbour = &other;
                }
            }
            if (neighbour != nullptr) {
                const RationalTime sourceEnd =
                    anchor->source_in.add(anchor->duration);
                const RationalTime neighbourEnd =
                    neighbour->source_in.add(neighbour->duration);
                RationalTime from{0, 1};
                RationalTime to{0, 1};
                if (op.edge == TrimEdge::Tail) {
                    from = std::max(neighbour->source_in, sourceEnd);
                    to = std::min(sourceEnd.add(op.delta), neighbourEnd);
                } else {
                    from = std::max(anchor->source_in.add(op.delta),
                                    neighbour->source_in);
                    to = std::min(anchor->source_in, neighbourEnd);
                }
                if (from < to) {
                    const auto timeValue = [](const RationalTime& time) {
                        Value value = Value::MakeObject();
                        value.Set("value", Value::MakeInt(time.value));
                        value.Set("rate", Value::MakeInt(time.rate));
                        return value;
                    };
                    repeats = Value::MakeObject();
                    repeats.Set("clip_id", Value::MakeString(neighbour->id));
                    repeats.Set("from", timeValue(from));
                    repeats.Set("to", timeValue(to));
                    repeats.Set(
                        "hint",
                        Value::MakeString("these source frames now play twice, "
                                          "here and in that clip; roll_edit "
                                          "moves a junction without repeating "
                                          "any"));
                }
            }
        } catch (const std::exception&) {
            // An overflow in the arithmetic is the operation's to refuse.
        }
    }

    Value cutWords;
    const bool measured = MeasureCutWords(backend, document, op, {}, cutWords);
    std::string appliedJson;
    if (!backend.ApplyOperation(op, appliedJson, errorName, message))
        return false;
    Value result;
    std::string parseError;
    if (!Value::Parse(appliedJson, result, parseError) || !result.IsObject())
        result = Value::MakeObject();
    if (measured) result.Set("cut_words", std::move(cutWords));
    if (repeats.IsObject()) result.Set("repeats_source", std::move(repeats));
    Value alsoCut = Value::MakeArray();
    std::vector<Ulid> reported;
    for (const Ulid& linkedId : op.linked_clip_ids) {
        if (linkedId == op.clip_id ||
            std::find(reported.begin(), reported.end(), linkedId) !=
                reported.end())
            continue;
        reported.push_back(linkedId);
        alsoCut.Push(Value::MakeString(linkedId));
    }
    result.Set("also_cut", std::move(alsoCut));
    if (!warningTrackIds.empty()) {
        // PRD15 -- in English, as the engine's other answers to an agent;
        // it was the catalog's one French sentence outside examples.
        result.Set(
            "warning",
            Value::MakeString("sync_track_ids omitted: clips downstream on the "
                              "tracks in warning_track_ids were not shifted"));
        Value tracks = Value::MakeArray();
        for (const Ulid& trackId : warningTrackIds)
            tracks.Push(Value::MakeString(trackId));
        result.Set("warning_track_ids", std::move(tracks));
    }
    resultJson = result.Dump();
    return true;
}

bool DispatchRollEdit(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"pairs", "delta"};
    if (!CheckKnownKeys(args, kAllowed, "roll_edit", message))
        return Fail(errorName, message, message);
    RollEditOperation op;
    if (!ReadTime(args, "delta", "roll_edit", true, op.delta, message))
        return Fail(errorName, message, message);
    const Value* pairs = args.Find("pairs");
    if (!pairs || !pairs->IsArray() || pairs->AsArray().empty())
        return Fail(errorName, message,
                    "'roll_edit.pairs' must be a non-empty array");
    for (size_t index = 0; index < pairs->AsArray().size(); ++index) {
        const Value& item = pairs->AsArray()[index];
        const std::string path =
            "roll_edit.pairs[" + std::to_string(index) + "]";
        static const std::vector<std::string> kPairKeys = {"left_clip_id",
                                                           "right_clip_id"};
        if (!CheckKnownKeys(item, kPairKeys, path, message))
            return Fail(errorName, message, message);
        RollEditPair pair;
        if (!ReadId(item, "left_clip_id", path, resolver, true,
                    pair.left_clip_id, message) ||
            !ReadId(item, "right_clip_id", path, resolver, true,
                    pair.right_clip_id, message))
            return Fail(errorName, message, message);
        op.pairs.push_back(pair);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. `linked_clip_ids` follows
// slide_edit's convention (QC-2026-09 B5): absent, each named clip brings
// its partner on every other track (SlipClipsWithLinkedPartners), so a slip
// keeps picture and sound in sync; present, even [], it is the caller's
// exact choice, and [] slips the named clips alone, the recalage of one
// track against the other. Before 24 October 2026 nothing was added: a
// picture slipped alone left its sound reading other frames, without a
// word, where slide_edit and the window's slip took the partner along.
bool DispatchSlipEdit(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids", "delta",
                                                      "linked_clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "slip_edit", message))
        return Fail(errorName, message, message);
    const bool linkedIdsExplicit = args.Find("linked_clip_ids") != nullptr;
    SlipEditOperation op;
    std::vector<Ulid> linked;
    if (!ReadTime(args, "delta", "slip_edit", true, op.delta, message))
        return Fail(errorName, message, message);
    if (!ReadIdArray(args, "clip_ids", "slip_edit", resolver, op.clip_ids,
                     message) ||
        !ReadIdArray(args, "linked_clip_ids", "slip_edit", resolver, linked,
                     message))
        return Fail(errorName, message, message);
    if (op.clip_ids.empty())
        return Fail(errorName, message,
                    "'slip_edit.clip_ids' must be a non-empty array");
    if (!linkedIdsExplicit) {
        Document document;
        if (!backend.SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        op.clip_ids =
            SlipClipsWithLinkedPartners(document, std::move(op.clip_ids));
    }
    for (const Ulid& id : linked)
        if (std::find(op.clip_ids.begin(), op.clip_ids.end(), id) ==
            op.clip_ids.end())
            op.clip_ids.push_back(id);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. The clips named and one exact
// delta. `linked_clip_ids` follows ripple_trim's convention (QC-2026-09 B5):
// absent, each named clip brings its partner on every other track
// (LinkedEdgePartnerIds), so a slide keeps picture and sound together;
// present, even [], it is the caller's exact choice. Before the review of
// 18 October 2026 nothing was added, and sliding a picture alone moved it
// off its sound without a word, while rate_stretch took the whole group.
bool DispatchSlideEdit(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids", "delta",
                                                      "linked_clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "slide_edit", message))
        return Fail(errorName, message, message);
    const bool linkedIdsExplicit = args.Find("linked_clip_ids") != nullptr;
    SlideEditOperation op;
    std::vector<Ulid> linked;
    if (!ReadTime(args, "delta", "slide_edit", true, op.delta, message))
        return Fail(errorName, message, message);
    if (!ReadIdArray(args, "clip_ids", "slide_edit", resolver, op.clip_ids,
                     message) ||
        !ReadIdArray(args, "linked_clip_ids", "slide_edit", resolver, linked,
                     message))
        return Fail(errorName, message, message);
    if (op.clip_ids.empty())
        return Fail(errorName, message,
                    "'slide_edit.clip_ids' must be a non-empty array");
    if (!linkedIdsExplicit) {
        Document document;
        if (!backend.SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        // FC3 (window) -- the rule the window's slide asks too
        // (Operations.h); an unknown clip is the operation's to refuse.
        op.clip_ids =
            SlideClipsWithLinkedPartners(document, std::move(op.clip_ids));
    }
    for (const Ulid& id : linked)
        if (std::find(op.clip_ids.begin(), op.clip_ids.end(), id) ==
            op.clip_ids.end())
            op.clip_ids.push_back(id);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// FC3 -- the new duration, not a delta: "make this shot last three
// seconds" is what a stretch is asked for, and the speed follows from it.
//
// DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. `ripple: true` is the stretch
// of Resolve's Trim mode (RateStretchOperation::ripple). Its sync tracks,
// absent, are the editing default ripple_trim takes (AGENT-2026-09 A34):
// every sync-locked track that holds no member of the group; a track whose
// sync lock is off and that holds clips after the clip's end is named in a
// warning, never moved. Present, even [], they are the caller's choice.
bool DispatchRateStretch(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "edge", "duration", "ripple", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "rate_stretch", message))
        return Fail(errorName, message, message);
    const bool syncTracksExplicit = args.Find("sync_track_ids") != nullptr;
    RateStretchOperation op;
    if (!ReadId(args, "clip_id", "rate_stretch", resolver, true, op.clip_id,
                message) ||
        !ReadTime(args, "duration", "rate_stretch", true, op.duration,
                  message) ||
        !ReadBool(args, "ripple", "rate_stretch", false, op.ripple, message) ||
        !ReadIdArray(args, "sync_track_ids", "rate_stretch", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    if (args.Find("edge") != nullptr &&
        !ReadEdge(args, "edge", "rate_stretch", op.edge, message))
        return Fail(errorName, message, message);
    if (syncTracksExplicit && !op.ripple)
        return Fail(errorName, message,
                    "'rate_stretch.sync_track_ids' only says which tracks "
                    "follow a ripple: set 'ripple' to true or leave it out");
    std::vector<Ulid> warningTrackIds;
    if (op.ripple && !syncTracksExplicit) {
        Document document;
        if (!backend.SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        const DocumentClip* anchor = document.FindClip(op.clip_id);
        if (anchor != nullptr) {
            std::vector<Ulid> memberTrackIds;
            for (const DocumentTrack& track : document.sequence.tracks)
                for (const DocumentClip& clip : track.clips)
                    if (clip.id == anchor->id ||
                        (!anchor->link_group_id.empty() &&
                         clip.link_group_id == anchor->link_group_id))
                        memberTrackIds.push_back(track.id);
            const RationalTime end = anchor->timeline_in.add(anchor->duration);
            for (const DocumentTrack& track : document.sequence.tracks) {
                if (std::find(memberTrackIds.begin(), memberTrackIds.end(),
                              track.id) != memberTrackIds.end())
                    continue;
                if (track.sync_lock) {
                    op.sync_track_ids.push_back(track.id);
                    continue;
                }
                if (std::any_of(track.clips.begin(), track.clips.end(),
                                [&](const DocumentClip& clip) {
                                    return clip.timeline_in >= end;
                                }))
                    warningTrackIds.push_back(track.id);
            }
        }
        // An unknown clip is the operation's to refuse, by its name.
    }
    std::string appliedJson;
    if (!backend.ApplyOperation(op, appliedJson, errorName, message))
        return false;
    if (warningTrackIds.empty()) {
        resultJson = std::move(appliedJson);
        return true;
    }
    Value result;
    std::string parseError;
    if (!Value::Parse(appliedJson, result, parseError) || !result.IsObject())
        result = Value::MakeObject();
    result.Set("warning",
               Value::MakeString("sync_track_ids omitted: clips downstream on "
                                 "the tracks in warning_track_ids were not "
                                 "shifted"));
    Value tracks = Value::MakeArray();
    for (const Ulid& trackId : warningTrackIds)
        tracks.Push(Value::MakeString(trackId));
    result.Set("warning_track_ids", std::move(tracks));
    resultJson = result.Dump();
    return true;
}

bool DispatchRemoveLinkedClips(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "link_group_id", "clip_ids", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "remove_linked_clips", message))
        return Fail(errorName, message, message);
    RemoveLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "remove_linked_clips", resolver, true,
                op.link_group_id, message) ||
        !ReadIdArray(args, "clip_ids", "remove_linked_clips", resolver,
                     op.clip_ids, message) ||
        !ReadIdArray(args, "sync_track_ids", "remove_linked_clips", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchClearLinkedClips(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"link_group_id",
                                                      "clip_ids"};
    if (!CheckKnownKeys(args, kAllowed, "clear_linked_clips", message))
        return Fail(errorName, message, message);
    ClearLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "clear_linked_clips", resolver, true,
                op.link_group_id, message) ||
        !ReadIdArray(args, "clip_ids", "clear_linked_clips", resolver,
                     op.clip_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSplitLinkedClips(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "link_group_id", "clip_ids", "timeline_position", "sync_track_ids"};
    if (!CheckKnownKeys(args, kAllowed, "split_linked_clips", message))
        return Fail(errorName, message, message);
    SplitLinkedClipsOperation op;
    if (!ReadId(args, "link_group_id", "split_linked_clips", resolver, true,
                op.link_group_id, message) ||
        !ReadIdArray(args, "clip_ids", "split_linked_clips", resolver,
                     op.clip_ids, message) ||
        !ReadTime(args, "timeline_position", "split_linked_clips", true,
                  op.timeline_position, message) ||
        !ReadIdArray(args, "sync_track_ids", "split_linked_clips", resolver,
                     op.sync_track_ids, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchPasteClips(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clips", "overwrite"};
    if (!CheckKnownKeys(args, kAllowed, "paste_clips", message))
        return Fail(errorName, message, message);
    PasteClipsOperation op;
    if (!ReadBool(args, "overwrite", "paste_clips", false, op.overwrite,
                  message))
        return Fail(errorName, message, message);
    const Value* clips = args.Find("clips");
    if (!clips || !clips->IsArray() || clips->AsArray().empty())
        return Fail(errorName, message,
                    "'paste_clips.clips' must be a non-empty array");

    struct Item {
        std::string label;
        Ulid track_id;
        Ulid source_id;
        RationalTime source_in;
        RationalTime duration;
        RationalTime timeline_in;
        std::string link_group;
        bool anchor = false;
    };
    std::vector<Item> items;
    static const std::vector<std::string> kItemKeys = {
        "label",    "track_id",    "source_id",  "source_in",
        "duration", "timeline_in", "link_group", "anchor"};
    for (size_t index = 0; index < clips->AsArray().size(); ++index) {
        const Value& raw = clips->AsArray()[index];
        const std::string path =
            "paste_clips.clips[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(raw, kItemKeys, path, message))
            return Fail(errorName, message, message);
        Item item;
        if (!ReadString(raw, "label", path, true, "", item.label, message) ||
            !ReadId(raw, "track_id", path, resolver, true, item.track_id,
                    message) ||
            !ReadId(raw, "source_id", path, resolver, true, item.source_id,
                    message) ||
            !ReadTime(raw, "source_in", path, true, item.source_in, message) ||
            !ReadTime(raw, "duration", path, true, item.duration, message) ||
            !ReadTime(raw, "timeline_in", path, true, item.timeline_in,
                      message) ||
            !ReadString(raw, "link_group", path, false, "", item.link_group,
                        message) ||
            !ReadBool(raw, "anchor", path, false, item.anchor, message))
            return Fail(errorName, message, message);
        if (item.label.empty())
            return Fail(errorName, message,
                        "'" + path + ".label' must not be empty");
        items.push_back(std::move(item));
    }

    std::map<std::string, Ulid> labelToCopiedId;
    for (const Item& item : items) {
        if (labelToCopiedId.count(item.label))
            return Fail(errorName, message,
                        "'paste_clips.clips' has a duplicate label '" +
                            item.label + "'");
        labelToCopiedId[item.label] = GenerateUlid();
    }
    std::map<std::string, Ulid> linkGroupUlid;
    std::map<std::string, std::string> linkGroupAnchorLabel;
    for (const Item& item : items) {
        if (item.link_group.empty()) continue;
        if (!linkGroupUlid.count(item.link_group))
            linkGroupUlid[item.link_group] = GenerateUlid();
        if (item.anchor || !linkGroupAnchorLabel.count(item.link_group))
            linkGroupAnchorLabel[item.link_group] = item.label;
    }
    for (const Item& item : items) {
        PastedClip clip;
        clip.copied_clip_id = labelToCopiedId[item.label];
        clip.track_id = item.track_id;
        clip.source_id = item.source_id;
        clip.source_in = item.source_in;
        clip.duration = item.duration;
        clip.timeline_in = item.timeline_in;
        if (!item.link_group.empty()) {
            clip.copied_link_group_id = linkGroupUlid[item.link_group];
            clip.copied_sync_anchor_clip_id =
                labelToCopiedId[linkGroupAnchorLabel[item.link_group]];
        }
        op.clips.push_back(std::move(clip));
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// F1.5 -- ROADMAP.md. Multicam groups. Ids resolve like every other tool's;
// Multicam.h builds the operation, states what an angle does to the picture
// and measures, on a dry run, what the result reports beside the backend's.
bool ApplyMulticamEdit(McpBackend& backend, const Document& before,
                       Operation operation, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    Value report;
    const bool described = DescribeMulticamEdit(before, operation, report);
    if (!backend.ApplyOperation(std::move(operation), resultJson, errorName,
                                message)) {
        return false;
    }
    if (!described) {
        return true;
    }
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject()) {
        result = Value::MakeObject();
    }
    for (const auto& [key, value] : report.AsObject()) {
        result.Set(key, value);
    }
    resultJson = result.Dump();
    return true;
}

bool DispatchAddMulticamGroup(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    const std::string tool = "add_multicam_group";
    if (!CheckKnownKeys(args, {"angles", "name", "active_clip_id"}, tool,
                        message)) {
        return Fail(errorName, message, message);
    }
    const Value* angles = args.Find("angles");
    if (angles == nullptr || !angles->IsArray()) {
        return Fail(
            errorName, message,
            "'" + tool + ".angles' must be an array of {clip_id, name}");
    }
    std::vector<MulticamAngleRequest> requests;
    for (size_t index = 0; index < angles->AsArray().size(); ++index) {
        const Value& item = angles->AsArray()[index];
        const std::string path =
            tool + ".angles[" + std::to_string(index) + "]";
        MulticamAngleRequest request;
        if (!CheckKnownKeys(item, {"clip_id", "name"}, path, message) ||
            !ReadId(item, "clip_id", path, resolver, true, request.clip_id,
                    message) ||
            !ReadString(item, "name", path, false, "", request.name, message)) {
            return Fail(errorName, message, message);
        }
        requests.push_back(std::move(request));
    }
    std::string name;
    Ulid activeClip;
    if (!ReadString(args, "name", tool, false, "", name, message) ||
        !ReadId(args, "active_clip_id", tool, resolver, false, activeClip,
                message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    AddMulticamGroupOperation op;
    EditError error = EditError::None;
    if (!BuildAddMulticamGroup(document, name, requests, activeClip, op, error,
                               message)) {
        errorName = EditErrorName(error);
        return false;
    }
    return ApplyMulticamEdit(backend, document, std::move(op), resultJson,
                             errorName, message);
}

bool DispatchSetMulticamActiveAngle(McpBackend& backend,
                                    const IdResolver& resolver,
                                    const Value& args, std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    const std::string tool = "set_multicam_active_angle";
    if (!CheckKnownKeys(args, {"group_id", "angle_id"}, tool, message)) {
        return Fail(errorName, message, message);
    }
    if (args.Find("angle_id") == nullptr) {
        return Fail(errorName, message,
                    "'" + tool +
                        ".angle_id' is required; pass \"\" with group_id "
                        "to clear the active angle");
    }
    Ulid groupId;
    Ulid angleOrClip;
    if (!ReadId(args, "group_id", tool, resolver, false, groupId, message) ||
        !ReadOptionalIdOrEmpty(args, "angle_id", tool, resolver, angleOrClip,
                               message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    SetMulticamActiveAngleOperation op;
    EditError error = EditError::None;
    if (!BuildSetMulticamActiveAngle(document, groupId, angleOrClip, op, error,
                                     message)) {
        errorName = EditErrorName(error);
        return false;
    }
    return ApplyMulticamEdit(backend, document, op, resultJson, errorName,
                             message);
}

// F1.5 -- decided on 8 October 2026 (ROADMAP.md). A cut through a group
// (Multicam.h, BuildSplitMulticamGroup): one batch, one undo. The answer
// names the second group, its angles and the one it shows.
bool DispatchSplitMulticamGroup(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    const std::string tool = "split_multicam_group";
    if (!CheckKnownKeys(args,
                        {"group_id", "timeline_position", "active_angle_id"},
                        tool, message)) {
        return Fail(errorName, message, message);
    }
    Ulid groupId;
    Ulid activeAngle;
    RationalTime position;
    if (!ReadId(args, "group_id", tool, resolver, true, groupId, message) ||
        !ReadTime(args, "timeline_position", tool, true, position, message) ||
        !ReadId(args, "active_angle_id", tool, resolver, false, activeAngle,
                message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    BatchOperation batch;
    MulticamCut cut;
    EditError error = EditError::None;
    if (!BuildSplitMulticamGroup(document, groupId, position, activeAngle,
                                 batch, cut, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    if (!backend.ApplyOperation(batch, resultJson, errorName, message)) {
        return false;
    }
    Value result;
    std::string parseError;
    if (Value::Parse(resultJson, result, parseError) && result.IsObject()) {
        result.Set("group_id", Value::MakeString(cut.group_id));
        Value angles = Value::MakeArray();
        for (const Ulid& angle : cut.angle_ids) {
            angles.Push(Value::MakeString(angle));
        }
        result.Set("angle_ids", std::move(angles));
        result.Set("active_angle_id", Value::MakeString(cut.active_angle_id));
        resultJson = result.Dump();
    }
    return true;
}

bool DispatchRemoveMulticamGroup(McpBackend& backend,
                                 const IdResolver& resolver, const Value& args,
                                 std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    const std::string tool = "remove_multicam_group";
    RemoveMulticamGroupOperation op;
    if (!CheckKnownKeys(args, {"group_id"}, tool, message) ||
        !ReadId(args, "group_id", tool, resolver, true, op.group_id, message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return ApplyMulticamEdit(backend, document, op, resultJson, errorName,
                             message);
}

bool DispatchListMulticamGroups(McpBackend& backend, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "list_multicam_groups", message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    resultJson = DescribeMulticamGroups(document.sequence).Dump();
    return true;
}

}  // namespace

void RegisterEditTools(const McpToolAdder& add) {
    // AX-2026-09 -- the pointer to paste_clips is there because a run opened
    // a third video track for want of an overwrite. PRD14: the lot that
    // found it stays here; a description is read by an agent on every
    // load, and tests/mcp_tools_tests.cc refuses a ticket name in one.
    add("insert_clip",
        "Insert a clip from a source into a track at an exact timeline "
        "position, rippling later clips on that track forward. Without "
        "sync_track_ids only this track ripples; the answer names the linked "
        "tracks left behind. To lay a clip over what a track already holds "
        "without moving anything else -- replacing a cutaway, adding one to a "
        "filled cutaway track -- use paste_clips with overwrite: true instead. "
        "`speed` lays it at a constant speed (default 1/1); 0/1 is a freeze "
        "frame: the frame source_in falls in, held for the whole duration, on "
        "a video track. A still image (a PNG, a JPEG, a TIFF) is a media of "
        "one frame: lay it with speed 0/1 and source_in 0 to hold it as long "
        "as wanted, its alpha kept; declare a logo a graphic with "
        "set_media_color_input.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Destination track."), true)
            .Field("source_id", IdSchema("Source media to read from."), true)
            .Field("source_in", kTimeSchemaText, true)
            .Field("duration", kTimeSchemaText, true)
            .Field("timeline_in", kTimeSchemaText, true)
            .Field("speed", kFractionSchemaText, false)
            .Field("sync_track_ids",
                   IdArraySchema("Additional tracks whose downstream clips "
                                 "must follow the same ripple -- e.g. the "
                                 "track holding the A/V-linked partner of "
                                 "what is being pushed forward."),
                   false)
            .Build("insert_clip arguments"),
        DispatchInsertClip);

    // PRD14 -- the points are named by their arguments: "a Record point"
    // alone left an agent to guess that it meant timeline_in.
    add("edit_source_range",
        "Insert or overwrite a Source range on an unlocked video track, with "
        "optional linked audio targets. Use explicit Source/Record in and "
        "exclusive out points. Missing source bounds (source_in, source_out) "
        "use the full rush. Three-point duration arithmetic is exact; "
        "conflicting four-point durations are refused. Insert opens every "
        "unlocked sync-locked track; overwrite affects only the targets.",
        SchemaBuilder()
            .Field("source_id", IdSchema("Source rush."), true)
            .Field("track_id", IdSchema("Unlocked destination video track."),
                   true)
            .Field("audio_track_ids",
                   IdArraySchema("Unlocked audio tracks to receive linked "
                                 "source audio; omitted means video only."),
                   false)
            .Field("mode",
                   EnumSchema({"Insert", "Overwrite"},
                              "Source-to-Record edit mode."),
                   true)
            .Field("source_in", kTimeSchemaText, false)
            .Field("source_out", kTimeSchemaText, false)
            .Field("timeline_in", kTimeSchemaText, false)
            .Field("timeline_out", kTimeSchemaText, false)
            .Field("preview",
                   BoolSchema("Validate and return the prepared operation "
                              "without editing; default false."),
                   false)
            .Build("edit_source_range arguments"),
        DispatchEditSourceRange);

    add("delete_timeline_range",
        "Clear a Record in/out range on explicit target tracks, splitting "
        "clips at its exact boundaries. Out is exclusive. Default clear leaves "
        "a gap; ripple=true extracts the range and closes time on targets and "
        "unlocked sync-locked followers. Locked tracks remain untouched.",
        SchemaBuilder()
            .Field("in", kTimeSchemaText, true)
            .Field("out", kTimeSchemaText, true)
            .Field("track_ids",
                   IdArraySchema(
                       "Distinct target track IDs; at least one is required."),
                   true)
            .Field("ripple",
                   BoolSchema("Extract and close time instead of leaving a "
                              "gap; default false."),
                   false)
            .Field("followers",
                   EnumSchema(
                       {"cut", "continuous"},
                       "With ripple, how untargeted sync-locked tracks "
                       "follow. 'cut' (default) removes the same range from "
                       "them, so a cutaway spanning it loses frames in the "
                       "middle of the shot. 'continuous' moves them through "
                       "the same time change but keeps each clip's pictures "
                       "continuous from its head, shortening it at its tail: "
                       "a cutaway over removed words stays whole and ends "
                       "with them."),
                   false)
            .Field("preview",
                   BoolSchema("Validate and return the prepared operation "
                              "without editing; default false."),
                   false)
            .Build("delete_timeline_range arguments"),
        DispatchDeleteTimelineRange);

    add("remove_clip",
        "Remove a clip and ripple later clips on its track backward to close "
        "the gap.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to remove."), true)
            .Field("sync_track_ids",
                   IdArraySchema("Additional tracks whose downstream clips "
                                 "must follow the same ripple."),
                   false)
            .Build("remove_clip arguments"),
        DispatchRemoveClip);

    add("clear_clip",
        "Remove a clip in place, leaving a gap instead of rippling later "
        "clips.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to clear."), true)
            .Build("clear_clip arguments"),
        DispatchClearClip);

    add("clear_clips",
        "Remove an arbitrary selection of clips atomically in place, leaving "
        "gaps instead of rippling later clips.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Clips to clear."), true)
            .Build("clear_clips arguments"),
        DispatchClearClips);

    add("trim_clip",
        "Trim one edge of a clip, changing only that clip's own extent.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to trim."), true)
            .Field("edge", EnumSchema({"Head", "Tail"}, "Which edge to trim."),
                   true)
            .Field("delta", kTimeSchemaText, false)
            .Field("source_frame", SourceFrameSchema(), false)
            .Build("trim_clip arguments"),
        DispatchTrimClip);

    add("move_clip",
        "Move a clip to a new track and/or timeline position, overwriting "
        "anything it lands on top of.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to move."), true)
            .Field("track_id", IdSchema("Destination track."), true)
            .Field("timeline_in", kTimeSchemaText, true)
            .Build("move_clip arguments"),
        DispatchMoveClip);

    add("move_clips", "Move an arbitrary selection of clips atomically.",
        SchemaBuilder()
            .Field("moves",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
                           IdSchema("") + ",\"track_id\":" + IdSchema("") +
                           ",\"timeline_in\":" + std::string(kTimeSchemaText) +
                           "},\"required\":[\"clip_id\",\"track_id\","
                           "\"timeline_in\"],\"additionalProperties\":false}",
                       "One exact destination per selected clip."),
                   true)
            .Build("move_clips arguments"),
        DispatchMoveClips);

    add("split_clip", "Split one clip into two, strictly inside it.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to split."), true)
            .Field("timeline_position", kTimeSchemaText, false)
            .Field("source_frame", SourceFrameSchema(), false)
            .Build("split_clip arguments"),
        DispatchSplitClip);

    add("split_at_interval",
        "Cut one clip repeatedly at a regular spacing -- every N seconds -- in "
        "a single call. interval {value:3, rate:1} cuts every three seconds, "
        "on the sequence's frames. Cuts the clip's A/V-linked partners at the "
        "same positions.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to subdivide."), true)
            .Field("interval", kTimeSchemaText, true)
            .Build("split_at_interval arguments"),
        DispatchSplitAtInterval);

    add("delete_gap",
        "Remove a gap of exact duration starting at an exact position on a "
        "track, rippling everything after it backward. Optionally applies "
        "the same shift to other tracks.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track containing the gap."), true)
            .Field("gap_start", kTimeSchemaText, true)
            .Field("gap_duration", kTimeSchemaText, true)
            .Field(
                "linked_track_ids",
                IdArraySchema("Other tracks to ripple by the same duration."),
                false)
            .Build("delete_gap arguments"),
        DispatchDeleteGap);

    add("detach_audio",
        "Split a video clip's audio into its own clip on an audio track, "
        "linked back to the video clip in sync.",
        SchemaBuilder()
            .Field("video_clip_id",
                   IdSchema("Video clip to detach audio from."), true)
            .Field("audio_track_id",
                   IdSchema("Destination audio track for the detached clip."),
                   true)
            .Build("detach_audio arguments"),
        DispatchDetachAudio);

    add("move_linked_clips",
        "Move every member of one A/V-linked group in a single atomic "
        "gesture, one destination per member.",
        SchemaBuilder()
            .Field("link_group_id",
                   IdSchema("Shared link group of the clips being moved."),
                   true)
            .Field("moves",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
                           IdSchema("") + ",\"track_id\":" + IdSchema("") +
                           ",\"timeline_in\":" + std::string(kTimeSchemaText) +
                           "},\"required\":[\"clip_id\",\"track_id\","
                           "\"timeline_in\"],\"additionalProperties\":false}",
                       "One destination per linked member."),
                   true)
            .Build("move_linked_clips arguments"),
        DispatchMoveLinkedClips);

    add("trim_linked_clips",
        "Trim every member of one A/V-linked group in a single atomic "
        "gesture, one edge/delta per member.",
        SchemaBuilder()
            .Field("link_group_id",
                   IdSchema("Shared link group of the clips being trimmed."),
                   true)
            .Field("trims",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
                           IdSchema("") +
                           ",\"edge\":" + EnumSchema({"Head", "Tail"}, "") +
                           ",\"delta\":" + std::string(kTimeSchemaText) +
                           "},\"required\":[\"clip_id\",\"edge\",\"delta\"],"
                           "\"additionalProperties\":false}",
                       "One edge/delta per linked member."),
                   true)
            .Build("trim_linked_clips arguments"),
        DispatchTrimLinkedClips);

    add("shorten_linked_clip",
        "Shorten one A/V-linked plan by an editing amount. Prefer it to "
        "trim_linked_clips to remove N frames or seconds: the engine finds the "
        "members and the sign.",
        SchemaBuilder()
            .Field("clip_id",
                   IdSchema("Any video or audio member of the linked plan."),
                   true)
            .Field("edge", EnumSchema({"Head", "Tail"}, "Edge to shorten."),
                   true)
            .Field("amount", PositiveIntSchema("Positive editing amount."),
                   true)
            .Field("unit", EnumSchema({"Frames", "Seconds"}, "Amount unit."),
                   true)
            .Field("preview",
                   BoolSchema("Validate without modifying the project; "
                              "default false."),
                   false)
            .Build("shorten_linked_clip arguments"),
        DispatchShortenLinkedClip);

    add("ripple_trim",
        "Trim one clip's edge and, by default, its facing linked partner on "
        "each other track, and shift downstream clips by the same amount, "
        "every other sync-locked track with them; the answer warns of a track "
        "with its sync lock off that holds clips downstream." +
            std::string(kCutWordsNote),
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip whose edge is trimmed."), true)
            .Field("edge", EnumSchema({"Head", "Tail"}, "Which edge to trim."),
                   true)
            .Field("delta", kTimeSchemaText, false)
            .Field("source_frame", SourceFrameSchema(), false)
            .Field("linked_clip_ids",
                   IdArraySchema("Other clips in the same link group to trim "
                                 "together with clip_id."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema("Tracks that follow the ripple instead of "
                                 "every other sync-locked track."),
                   false)
            .Build("ripple_trim arguments"),
        DispatchRippleTrim);

    add("roll_edit",
        "Slide a shared cut between contiguous clip pairs without changing "
        "total sequence duration. Also moves the boundary between two adjacent "
        "caption clips: each keeps its text and no source is read.",
        SchemaBuilder()
            .Field(
                "pairs",
                ArraySchema(
                    "{\"type\":\"object\",\"properties\":{\"left_clip_id\":" +
                        IdSchema("") + ",\"right_clip_id\":" + IdSchema("") +
                        "},\"required\":[\"left_clip_id\","
                        "\"right_clip_id\"],\"additionalProperties\":"
                        "false}",
                    "Contiguous clip pairs sharing the rolled cut."),
                true)
            .Field("delta", kTimeSchemaText, true)
            .Build("roll_edit arguments"),
        DispatchRollEdit);

    add("slip_edit",
        "Slide the source window under one clip (or several linked clips) "
        "without moving its timeline position or duration.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema(
                       "Clip(s) to slip; more than one requires they share "
                       "a link group."),
                   true)
            .Field("delta", kTimeSchemaText, true)
            .Field("linked_clip_ids",
                   IdArraySchema(
                       "Linked clips that slip too. Omit it for the default: "
                       "each named clip's partner on every other track. [] "
                       "slips the named clips alone, to re-sync one track "
                       "against the other."),
                   false)
            .Build("slip_edit arguments"),
        DispatchSlipEdit);

    // FC3 -- docs/roadmap/FILMCRAFT-2026-10.md.
    add("slide_edit",
        "Move one clip (or several linked clips) by delta between the two "
        "clips that touch it on its track: it keeps its source range and "
        "duration, the clip before it lengthens or shortens by its tail and "
        "the clip after it by its head, so nothing else on the track moves.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Clip(s) to slide."), true)
            .Field("delta", kTimeSchemaText, true)
            .Field("linked_clip_ids",
                   IdArraySchema(
                       "Linked clips that slide too. Omit it for the default: "
                       "each named clip's partner on every other track. [] "
                       "slides the named clips alone, off their sound."),
                   false)
            .Build("slide_edit arguments"),
        DispatchSlideEdit);

    add("rate_stretch",
        "Give a clip a new timeline duration while it keeps reading the same "
        "source range: its constant speed becomes source span / duration, as "
        "an exact fraction.",
        SchemaBuilder()
            .Field("clip_id",
                   IdSchema("Clip to stretch; its link group follows."), true)
            .Field("duration", kTimeSchemaText, true)
            .Field("edge",
                   EnumSchema({"Head", "Tail"},
                              "The edge that moves; default Tail (the head "
                              "stays where it is). With ripple the head "
                              "stays either way."),
                   false)
            .Field("ripple",
                   BoolSchema("Move every later clip by the change of length "
                              "instead of refusing an overlap or leaving a "
                              "gap. Default false."),
                   false)
            .Field("sync_track_ids",
                   IdArraySchema("The other tracks that follow the ripple. "
                                 "Omit it for every sync-locked track; [] "
                                 "moves only the clip's own tracks."),
                   false)
            .Build("rate_stretch arguments"),
        DispatchRateStretch);

    add("remove_linked_clips",
        "Remove every member of one A/V-linked group, rippling each affected "
        "and explicitly synchronized track.",
        SchemaBuilder()
            .Field("link_group_id", IdSchema("Shared link group."), true)
            .Field("clip_ids",
                   IdArraySchema("Every member of the link group to remove."),
                   true)
            .Field("sync_track_ids",
                   IdArraySchema("Additional tracks whose downstream clips "
                                 "must follow the same ripple."),
                   false)
            .Build("remove_linked_clips arguments"),
        DispatchRemoveLinkedClips);

    add("clear_linked_clips",
        "Remove every member of one A/V-linked group in place, leaving gaps "
        "instead of rippling.",
        SchemaBuilder()
            .Field("link_group_id", IdSchema("Shared link group."), true)
            .Field("clip_ids",
                   IdArraySchema("Every member of the link group to clear."),
                   true)
            .Build("clear_linked_clips arguments"),
        DispatchClearLinkedClips);

    add("split_linked_clips",
        "Split every member of one A/V-linked group at the same timeline "
        "position in one atomic cut, producing two new link groups and "
        "optionally cutting synchronized tracks there too.",
        SchemaBuilder()
            .Field("link_group_id", IdSchema("Shared link group."), true)
            .Field("clip_ids",
                   IdArraySchema("Every member of the link group to split."),
                   true)
            .Field("timeline_position", kTimeSchemaText, true)
            .Field("sync_track_ids",
                   IdArraySchema("Additional tracks to cut at the same exact "
                                 "timeline position."),
                   false)
            .Build("split_linked_clips arguments"),
        DispatchSplitLinkedClips);

    add("paste_clips",
        "Paste a batch of clips (optionally A/V-linked to each other) onto one "
        "or more tracks at exact positions, either inserted or overwriting "
        "what is underneath.",
        SchemaBuilder()
            .Field(
                "clips",
                ArraySchema(
                    "{\"type\":\"object\",\"properties\":{\"label\":" +
                        StringSchema(
                            "Caller-chosen token unique within this call, "
                            "used only to correlate 'link_group' members.") +
                        ",\"track_id\":" + IdSchema("") +
                        ",\"source_id\":" + IdSchema("") +
                        ",\"source_in\":" + std::string(kTimeSchemaText) +
                        ",\"duration\":" + std::string(kTimeSchemaText) +
                        ",\"timeline_in\":" + std::string(kTimeSchemaText) +
                        ",\"link_group\":" +
                        StringSchema(
                            "Optional token shared by clips in this call "
                            "that should become one new A/V link group.") +
                        ",\"anchor\":" +
                        BoolSchema("Marks this item as its link_group's sync "
                                   "anchor; defaults to the first member.") +
                        "},\"required\":[\"label\",\"track_id\",\"source_id\","
                        "\"source_in\",\"duration\",\"timeline_in\"],"
                        "\"additionalProperties\":false}",
                    "Clips to paste."),
                true)
            .Field("overwrite",
                   BoolSchema("true: lay the clips over what is there, cut "
                              "away under them, nothing shifting -- how to "
                              "replace a clip or lay one on a filled track; "
                              "default: refused on an occupied range."),
                   false)
            .Build("paste_clips arguments"),
        DispatchPasteClips);

    const std::string multicamAngleSchema =
        "{\"type\":\"object\",\"properties\":{\"clip_id\":" +
        IdSchema("Clip already on a video track, one camera's take.") +
        ",\"name\":" +
        StringSchema("Angle name; defaults to 'Angle N' in list order.") +
        "},\"required\":[\"clip_id\"],\"additionalProperties\":false}";

    add("add_multicam_group",
        "Group clips already placed on video tracks, one per synchronised "
        "camera, as the angles of one multicam group. While an angle is "
        "active, the other angles' clips are off screen everywhere the "
        "timeline is shown or measured; what lies underneath shows where the "
        "active clip does not cover. With no active angle, every angle "
        "composites as an ordinary clip. Pictures only: audio clips, linked or "
        "not, keep playing, so mute the audio tracks that must not be heard. "
        "Returns group_id, angle_ids in order, the group and the clips it "
        "hides.",
        SchemaBuilder()
            .Field("angles",
                   ArraySchema(multicamAngleSchema,
                               "At least two angles, one clip each."),
                   true)
            .Field("name",
                   StringSchema("Group name; defaults to 'Multicam N'."), false)
            .Field("active_clip_id",
                   IdSchema("One of the angle clips, to make its angle "
                            "active at once; omit to leave none active."),
                   false)
            .Build("add_multicam_group arguments"),
        DispatchAddMulticamGroup);

    add("split_multicam_group",
        "Cut a multicam group at timeline_position: every angle that spans it "
        "is split there, and what follows becomes a second group that can show "
        "another angle (Resolve's multicam cut). The group keeps the angles "
        "before the cut; the second takes the right pieces and the angles "
        "wholly after it, and shows the angle that was active, or "
        "active_angle_id (an angle of the group). Sound is not cut: it does "
        "not follow the angle.",
        SchemaBuilder()
            .Field("group_id", IdSchema("The multicam group."), true)
            .Field("timeline_position", kTimeSchemaText, true)
            .Field("active_angle_id",
                   IdSchema("An angle of the group for the second group to "
                            "show; default: the active one."),
                   false)
            .Build("split_multicam_group arguments"),
        DispatchSplitMulticamGroup);

    add("set_multicam_active_angle",
        "Choose which angle of a multicam group is on screen. Pictures only. "
        "Returns the group and the clips now hidden.",
        SchemaBuilder()
            .Field("angle_id",
                   IdSchema("Angle to activate, or the clip it shows; \"\" "
                            "clears the group's active angle."),
                   true)
            .Field("group_id",
                   IdSchema("Multicam group; required to clear, optional "
                            "otherwise."),
                   false)
            .Build("set_multicam_active_angle arguments"),
        DispatchSetMulticamActiveAngle);

    add("remove_multicam_group",
        "Dissolve a multicam group. Its clips stay where they are and "
        "composite as ordinary clips again; nothing else changes.",
        SchemaBuilder()
            .Field("group_id", IdSchema("Multicam group to dissolve."), true)
            .Build("remove_multicam_group arguments"),
        DispatchRemoveMulticamGroup);

    add("list_multicam_groups",
        "List the timeline's multicam groups: each angle's clip, track and "
        "extent, which angle is active and which clips multicam takes off "
        "screen.",
        SchemaBuilder().Build("list_multicam_groups arguments"),
        DispatchListMulticamGroups);
}
