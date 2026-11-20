#include "McpMotionClipTools.h"

#include "Document.h"
#include "FrameCapture.h"
#include "InterviewShort.h"
#include "MotionClip.h"
#include "MotionDocument.h"
#include "MotionFrameRenderer.h"
#include "MotionPlacement.h"
#include "MotionSafeArea.h"
#include "Operations.h"
#include "SpeechSelection.h"
#include "TempoGrid.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

bool Refuse(std::string& errorName, std::string& message,
            const std::string& name, std::string text) {
    errorName = name;
    message = std::move(text);
    return false;
}

bool Invalid(std::string& errorName, std::string& message, std::string text) {
    return Refuse(errorName, message, "ValidationFailed", std::move(text));
}

std::string Quoted(const std::string& text) {
    return "\"" + mcp_json::EscapeJsonString(text) + "\"";
}

std::string Described(const std::string& type, const std::string& text) {
    return "{\"type\":\"" + type + "\",\"description\":" + Quoted(text) + "}";
}

// PRD15 -- the same words as McpTools.cc's kTimeSchemaText.
const char kTime[] =
    "{\"type\":\"object\",\"description\":\"Exact time: value ticks of 1/rate "
    "s.\",\"properties\":{\"value\":{\"type\":\"integer\"},"
    "\"rate\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"value\","
    "\"rate\"],\"additionalProperties\":false}";

std::string Schema(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const std::vector<std::string>& required, const std::string& description) {
    std::string json =
        "{\"type\":\"object\",\"description\":" + Quoted(description) +
        ",\"properties\":{";
    for (size_t index = 0; index < properties.size(); ++index) {
        json += (index != 0 ? "," : "") + Quoted(properties[index].first) +
                ":" + properties[index].second;
    }
    json += "}";
    // PRD15 -- an empty `required` is left out, as McpTools.cc does.
    for (size_t index = 0; index < required.size(); ++index) {
        json +=
            (index != 0 ? "," : ",\"required\":[") + Quoted(required[index]);
    }
    if (!required.empty()) json += "]";
    return json + ",\"additionalProperties\":false}";
}

// Every key known, `timeline_id` included: these tools edit or read the
// timeline it names, like the rest of the catalog.
bool Known(const Value& args, const std::vector<std::string>& allowed,
           const std::string& path, std::string& message) {
    if (!args.IsObject()) {
        message = "'" + path + "' arguments must be a JSON object";
        return false;
    }
    for (const auto& entry : args.AsObject()) {
        if (entry.first != "timeline_id" &&
            std::find(allowed.begin(), allowed.end(), entry.first) ==
                allowed.end()) {
            message =
                "'" + path + "' has unknown argument '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

bool ReadTime(const Value& args, const std::string& key,
              const std::string& path, bool required, RationalTime& out,
              std::string& message) {
    const Value* field = args.Find(key);
    if (field == nullptr) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
        }
        return !required;
    }
    const Value* value = field->Find("value");
    const Value* rate = field->Find("rate");
    int64_t ticks = 0;
    int64_t perSecond = 0;
    if (!field->IsObject() || field->AsObject().size() != 2 ||
        value == nullptr || rate == nullptr || !value->AsInt64(ticks) ||
        !rate->AsInt64(perSecond) || perSecond <= 0 ||
        perSecond > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + "." + key +
                  "' must be {value, rate}: exact integers, rate positive";
        return false;
    }
    out = {ticks, static_cast<int32_t>(perSecond)};
    return true;
}

bool ReadId(const Value& args, const std::string& key, const std::string& path,
            const IdResolver& resolver, bool required, Ulid& out,
            std::string& message) {
    const Value* field = args.Find(key);
    if (field == nullptr) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
        }
        return !required;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    return resolver.Resolve(path + "." + key, field->AsString(), out, message);
}

bool Snapshot(McpBackend& backend, Document& document, std::string& errorName,
              std::string& message) {
    if (backend.SnapshotDocument(document, message)) {
        return true;
    }
    errorName = "IoError";
    return false;
}

// A composition by full id or unambiguous prefix, as McpMotionTools.cc
// names one: compositions are not in the timeline's ID resolver.
bool ResolveComposition(const Document& document, const std::string& input,
                        const DocumentComposition*& composition,
                        std::string& errorName, std::string& message) {
    const DocumentComposition* found = nullptr;
    size_t matches = 0;
    for (const DocumentComposition& candidate : document.compositions) {
        if (candidate.id == input) {
            composition = &candidate;
            return true;
        }
        if (!input.empty() &&
            candidate.id.compare(0, input.size(), input) == 0) {
            found = &candidate;
            ++matches;
        }
    }
    if (matches == 1) {
        composition = found;
        return true;
    }
    return Refuse(errorName, message,
                  matches > 1 ? "ValidationFailed" : "UnknownComposition",
                  matches > 1 ? "composition_id '" + input +
                                    "' matches several compositions"
                              : "unknown composition_id '" + input + "'");
}

// The applied result, with what the caller needs next: the clip's id and
// the props the instance now carries.
void Annotate(std::string& resultJson, const Ulid& clipId, const Value& props) {
    Value result;
    std::string error;
    if (!Value::Parse(resultJson, result, error) || !result.IsObject()) {
        result = Value::MakeObject();
    }
    result.Set("clip_id", Value::MakeString(clipId));
    result.Set("motion_props", props);
    resultJson = result.Dump();
}

bool DispatchInsert(McpBackend& backend, const IdResolver& resolver,
                    const Value& args, std::string& resultJson,
                    std::string& errorName, std::string& message) {
    const std::string path = "insert_motion_clip";
    InsertMotionClipOperation op;
    const Value* composition = args.Find("composition_id");
    if (!Known(args,
               {"track_id", "composition_id", "timeline_in", "source_in",
                "duration", "props", "sync_track_ids"},
               path, message) ||
        !ReadId(args, "track_id", path, resolver, true, op.track_id, message) ||
        !ReadTime(args, "timeline_in", path, true, op.timeline_in, message) ||
        !ReadTime(args, "source_in", path, false, op.source_in, message) ||
        !ReadTime(args, "duration", path, false, op.duration, message)) {
        return Invalid(errorName, message, message);
    }
    if (composition == nullptr || !composition->IsString()) {
        return Invalid(errorName, message,
                       "'" + path + ".composition_id' is required");
    }
    if (const Value* props = args.Find("props")) {
        if (!props->IsObject() && !props->IsNull()) {
            return Invalid(errorName, message,
                           "'" + path + ".props' must be an object");
        }
        if (props->IsObject() && !props->AsObject().empty()) {
            op.motion_props = *props;
        }
    }
    if (const Value* sync = args.Find("sync_track_ids")) {
        if (!sync->IsArray()) {
            return Invalid(errorName, message,
                           "'" + path + ".sync_track_ids' must be an array");
        }
        for (size_t index = 0; index < sync->AsArray().size(); ++index) {
            Ulid id;
            const Value& item = sync->AsArray()[index];
            if (!item.IsString() ||
                !resolver.Resolve(
                    path + ".sync_track_ids[" + std::to_string(index) + "]",
                    item.AsString(), id, message)) {
                return Invalid(errorName, message,
                               message.empty() ? "'" + path +
                                                     ".sync_track_ids' holds "
                                                     "track ids"
                                               : message);
            }
            op.sync_track_ids.push_back(id);
        }
    }
    Document document;
    const DocumentComposition* stored = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, composition->AsString(), stored,
                            errorName, message)) {
        return false;
    }
    op.composition_id = stored->id;
    // Duration omitted: the rest of the composition from source_in, which
    // the engine measures rather than the caller.
    if (args.Find("duration") == nullptr) {
        DocumentSource source;
        MotionError motion;
        if (!CompositionAsSource(*stored, source, motion)) {
            return Refuse(errorName, message, "InvalidMotion",
                          motion.code + ": " + motion.message);
        }
        try {
            op.duration = source.duration.sub(op.source_in);
        } catch (const std::exception& exception) {
            return Refuse(errorName, message, "ArithmeticError",
                          exception.what());
        }
    }
    op.clip_id = GenerateUlid();
    const Ulid clipId = op.clip_id;
    const Value props = op.motion_props;
    if (!backend.ApplyOperation(std::move(op), resultJson, errorName,
                                message)) {
        return false;
    }
    Annotate(resultJson, clipId, props);
    return true;
}

bool DispatchSetProps(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    const std::string path = "set_clip_motion_props";
    Ulid clipId;
    if (!Known(args, {"clip_id", "props", "replace"}, path, message) ||
        !ReadId(args, "clip_id", path, resolver, true, clipId, message)) {
        return Invalid(errorName, message, message);
    }
    const Value* props = args.Find("props");
    if (props == nullptr || !props->IsObject()) {
        return Invalid(errorName, message,
                       "'" + path + ".props' must be an object");
    }
    const Value* replace = args.Find("replace");
    if (replace != nullptr && !replace->IsBool()) {
        return Invalid(errorName, message,
                       "'" + path + ".replace' must be a boolean");
    }
    Document document;
    if (!Snapshot(backend, document, errorName, message)) {
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        return Refuse(errorName, message, "UnknownClip",
                      "unknown clip_id '" + clipId + "'");
    }
    Value merged;
    if (replace != nullptr && replace->AsBool()) {
        merged = props->AsObject().empty() ? Value() : *props;
    } else {
        merged = MergeMotionProps(clip->motion_props, *props);
    }
    if (!backend.ApplyOperation(SetClipMotionPropsOperation{clipId, merged},
                                resultJson, errorName, message)) {
        return false;
    }
    Annotate(resultJson, clipId, merged);
    return true;
}

bool ReadFontDirs(const Value& args, const std::string& path,
                  std::vector<std::string>& fonts, std::string& message) {
    const Value* directories = args.Find("font_dirs");
    if (directories == nullptr) {
        return true;
    }
    if (!directories->IsArray()) {
        message = "'" + path + ".font_dirs' must be an array";
        return false;
    }
    for (const Value& directory : directories->AsArray()) {
        if (!directory.IsString()) {
            message = "'" + path + ".font_dirs' holds directory paths";
            return false;
        }
        fonts.push_back(directory.AsString());
    }
    return true;
}

// The bound of one side in `margins`: whatever four sides a caller gives,
// a tenth of the frame stays inside in each direction.
constexpr int64_t kMaxSideMarginPercent = 45;

// PRD38 -- docs/roadmap/PRODUIT-2026-10b.md. The safe area a check applies:
// `zone` names one ("default" unless told otherwise), `margin_percent`
// keeps its old meaning -- every side of the default zone -- and `margins`
// sets the sides one by one instead, a side left out keeping the default
// 5 %. Mixing them is refused rather than resolved by a precedence the
// caller would have to guess.
//
// DEC34 -- docs/roadmap/MOTION-2026-09.md. `given` says whether the caller
// set any of the three. When it did not, a check of a clip on a timeline
// replaces the default zone with the timeline's (TimelineMotionSafeZone):
// social_vertical on a 9:16 timeline. Only the caller knows whether there
// is a timeline, so the choice is left to it.
bool ReadSafeZone(const Value& args, const std::string& path,
                  MotionSafeZone& zone, bool& given, std::string& message) {
    zone = DefaultMotionSafeZone();
    const Value* named = args.Find("zone");
    const Value* uniform = args.Find("margin_percent");
    const Value* sides = args.Find("margins");
    given = named != nullptr || uniform != nullptr || sides != nullptr;
    if (named != nullptr) {
        if (!named->IsString() ||
            !NamedMotionSafeZone(named->AsString(), zone)) {
            message = "'" + path +
                      ".zone' must be \"default\" or \"social_vertical\"";
            return false;
        }
    }
    if (uniform != nullptr) {
        int64_t margin = 0;
        if (!uniform->AsInt64(margin) || margin < 0 || margin > 25) {
            message =
                "'" + path + ".margin_percent' must be an integer from 0 to 25";
            return false;
        }
        if (zone.name != "default" || sides != nullptr) {
            message = "'" + path +
                      ".margin_percent' sets the default zone's margin: give "
                      "it without zone \"social_vertical\" and without margins";
            return false;
        }
        zone = DefaultMotionSafeZone(margin);
    }
    if (sides != nullptr) {
        if (named != nullptr) {
            message = "'" + path +
                      ".margins' replaces the zone side by side: give it "
                      "without zone";
            return false;
        }
        if (!sides->IsObject()) {
            message = "'" + path +
                      ".margins' must be an object of top, bottom, left "
                      "and right";
            return false;
        }
        for (const auto& entry : sides->AsObject()) {
            if (entry.first != "top" && entry.first != "bottom" &&
                entry.first != "left" && entry.first != "right") {
                message = "'" + path + ".margins' has unknown side '" +
                          entry.first + "'";
                return false;
            }
        }
        zone.name = "custom";
        const std::pair<const char*, MotionSafeMargin*> fields[] = {
            {"top", &zone.top},
            {"bottom", &zone.bottom},
            {"left", &zone.left},
            {"right", &zone.right}};
        for (const auto& [key, margin] : fields) {
            const Value* side = sides->Find(key);
            if (side == nullptr) {
                continue;
            }
            int64_t percent = 0;
            if (!side->AsInt64(percent) || percent < 0 ||
                percent > kMaxSideMarginPercent) {
                message = "'" + path + ".margins." + key +
                          "' must be an integer from 0 to " +
                          std::to_string(kMaxSideMarginPercent);
                return false;
            }
            *margin = {percent, 100};
        }
    }
    return true;
}

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

// M5 -- docs/roadmap/MOTION-2026-09.md. Every composition frame an instance
// shows, evaluated with the real fonts (MotionFrameRequest::raster false)
// and checked against the safe area: `frameCount` frames of the sequence
// from `sourceIn`, each composition frame once, in order.
// MM3 -- docs/roadmap/MOTION-MARQUE-2026-10.md. The composition frames
// `unique`, each once, in order -- an instance's or a whole composition's.
bool CheckFrames(McpBackend& backend, const Document& document,
                 const Ulid& compositionId,
                 const MotionComposition& composition, const Value& props,
                 const std::set<int64_t>& unique,
                 const std::vector<std::string>& fonts,
                 MotionSafeAreaReport& report, std::string& errorName,
                 std::string& message) {
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      std::string("this process has no motion renderer "
                                  "(Core Text and Core Graphics) to measure "
                                  "the text with: ") +
                          kMotionRendererHolder);
    }
    const std::string directory = backend.MotionMediaDirectory();
    // REM2 -- the project's media and the compositions an instance shows.
    MotionReach reach;
    MotionError reachError;
    if (!MotionReachFor(document, compositionId, reach, reachError)) {
        return Refuse(errorName, message, reachError.code,
                      reachError.path + ": " + reachError.message);
    }
    bool first = true;
    for (const int64_t frame : unique) {
        MotionFrameRequest request;
        request.composition = &composition;
        request.props = props;
        request.frame = frame;
        request.media_directory = directory;
        request.reach = reach;
        // Registered once: the renderer keeps them for the process.
        if (first) {
            request.font_directories = fonts;
        }
        request.raster = false;
        first = false;
        MotionFrameImage image;
        MotionError error;
        if (!render(request, image, error)) {
            return Refuse(
                errorName, message, error.code,
                (error.path.empty() ? "" : error.path + ": ") + error.message);
        }
        CheckMotionFrameSafeArea(image.evaluated, report);
    }
    return true;
}

bool CheckInstance(McpBackend& backend, const Document& document,
                   const Ulid& compositionId,
                   const MotionComposition& composition, const Value& props,
                   const RationalTime& sourceIn, int64_t frameCount,
                   MediaRate rate, const std::vector<std::string>& fonts,
                   MotionSafeAreaReport& report, std::string& errorName,
                   std::string& message) {
    std::vector<int64_t> frames;
    try {
        frames = MotionClipFrames(composition, sourceIn, frameCount, rate);
    } catch (const std::exception& exception) {
        return Refuse(errorName, message, "ArithmeticError", exception.what());
    }
    return CheckFrames(backend, document, compositionId, composition, props,
                       std::set<int64_t>(frames.begin(), frames.end()), fonts,
                       report, errorName, message);
}

// The refusal an agent reads: which element, which edge, from which frame
// -- enough to shorten a text, lower a size or move a margin.
std::string DescribeIssues(const MotionSafeAreaReport& report) {
    std::string text;
    size_t shown = 0;
    for (const MotionSafeAreaIssue& issue : report.issues) {
        if (shown == 3) {
            text += "; and " + std::to_string(report.issues.size() - shown) +
                    " more";
            break;
        }
        text += shown == 0 ? "" : "; ";
        text += issue.text.empty() ? "a background" : "'" + issue.text + "'";
        if (!issue.node_id.empty()) {
            text += " (node " + issue.node_id + ")";
        }
        if (issue.kind == "text_overflows_box") {
            text += " is wider than its box";
        } else if (issue.kind == "word_split") {
            text += " is one word broken across two lines";
        } else {
            std::string edges;
            for (const std::string& edge : issue.edges) {
                edges += (edges.empty() ? "" : "/") + edge;
            }
            text += " crosses the " + edges + " edge";
        }
        text += " from composition frame " + std::to_string(issue.first_frame) +
                " to " + std::to_string(issue.last_frame);
        ++shown;
    }
    return text;
}

// DEC34 -- a zone the caller did not name is said to be the timeline's, so
// that a refusal in social_vertical is never read as a zone asked for.
std::string ZoneByDefaultNote(bool zoneGiven, const MotionSafeZone& zone) {
    if (zoneGiven || zone.name != "social_vertical") {
        return {};
    }
    return ", the default on a 9:16 timeline";
}

// `zoneGiven` false and social_vertical: the zone came from a 9:16
// timeline (DEC34), which the report says as `zone_chosen_by`. Any other
// report reads as it did before that decision.
Value ReportValue(const MotionSafeAreaReport& report, bool zoneGiven = true) {
    Value value;
    std::string error;
    Value::Parse(SerializeMotionSafeAreaReport(report), value, error);
    if (!ZoneByDefaultNote(zoneGiven, report.zone).empty() &&
        value.IsObject()) {
        value.Set("zone_chosen_by", Value::MakeString("timeline_9x16"));
    }
    return value;
}

// place_motion_clip reads the anchor from exactly one of clip_id, span_id,
// block_id and at.
//
// A beat of the music: `beat` names it as list_tempo_grid numbers it, and
// the engine answers the frame it is heard on (TempoBeatFramesOnTimeline).
// The instance then starts there as an `at: "start"` one does, `offset`
// later; a music heard twice takes `beat.clip_id` to say which playing.
bool ReadBeatAnchor(McpBackend& backend, const IdResolver& resolver,
                    const Value& beat, const std::string& path,
                    MotionPlacementRequest& request, std::string& errorName,
                    std::string& message) {
    const std::string field = path + ".beat";
    if (!beat.IsObject() ||
        !Known(beat,
               {"media_id", "index", "source_in", "source_out", "clip_id"},
               field, message)) {
        return Invalid(
            errorName, message,
            message.empty() ? "'" + field + "' must be an object" : message);
    }
    Ulid mediaId;
    Ulid clipId;
    int64_t index = 0;
    const Value* indexValue = beat.Find("index");
    if (!ReadId(beat, "media_id", field, resolver, true, mediaId, message) ||
        !ReadId(beat, "clip_id", field, resolver, false, clipId, message)) {
        return Invalid(errorName, message, message);
    }
    if (indexValue == nullptr || !indexValue->AsInt64(index)) {
        return Invalid(errorName, message,
                       "'" + field +
                           ".index' is required: a beat index of "
                           "list_tempo_grid");
    }
    Document document;
    if (!Snapshot(backend, document, errorName, message)) return false;
    const DocumentSource* source = document.FindSource(mediaId);
    if (source == nullptr) {
        return Refuse(errorName, message, "UnknownMedia",
                      "unknown media_id '" + mediaId + "'");
    }
    RationalTime windowIn{0, 1};
    RationalTime windowOut = source->duration;
    if (!ReadTime(beat, "source_in", field, false, windowIn, message) ||
        !ReadTime(beat, "source_out", field, false, windowOut, message)) {
        return Invalid(errorName, message, message);
    }
    BeatDetectionResult analysis;
    if (!backend.ReadSourceAudioBeats(mediaId, analysis, message)) {
        return Invalid(errorName, message, message);
    }
    const TempoGrid grid =
        FitTempoGrid(analysis.onsets, windowIn, windowOut, TempoGridSettings{},
                     TempoBarSettings{});
    std::vector<int64_t> frames;
    if (!TempoBeatFramesOnTimeline(document, grid, mediaId, index, frames,
                                   message)) {
        return Refuse(errorName, message, "UnknownBeat", message);
    }
    if (!clipId.empty()) {
        const DocumentClip* clip = document.FindClip(clipId);
        TempoGridClipBeats placed;
        frames.clear();
        if (clip == nullptr || clip->source_id != mediaId ||
            !ProjectTempoGridOntoClip(grid, *clip, document.sequence.frame_rate,
                                      placed, message)) {
            return Refuse(
                errorName, message, "UnknownClip",
                "clip '" + clipId + "' does not play media '" + mediaId + "'");
        }
        for (const TempoGridSequenceBeat& placedBeat : placed.beats)
            if (placedBeat.index == index) frames.push_back(placedBeat.frame);
    }
    if (frames.empty()) {
        return Refuse(
            errorName, message, "UnknownBeat",
            "beat " + std::to_string(index) + " of media '" + mediaId +
                "' is not heard on this timeline" +
                (clipId.empty() ? "" : " through clip '" + clipId + "'"));
    }
    if (frames.size() > 1) {
        std::string listed;
        for (const int64_t frame : frames)
            listed += (listed.empty() ? "" : ", ") + std::to_string(frame);
        return Refuse(errorName, message, "AmbiguousBeat",
                      "beat " + std::to_string(index) + " is heard on frames " +
                          listed + ": name the playing with beat.clip_id");
    }
    const MediaRate rate = document.sequence.frame_rate;
    request.anchor = MotionAnchor::Start;
    request.offset =
        RationalTime{frames.front() * rate.den, rate.num}.add(request.offset);
    return true;
}

bool ReadAnchor(McpBackend& backend, const IdResolver& resolver,
                const Value& args, const std::string& path,
                MotionPlacementRequest& request, std::string& errorName,
                std::string& message) {
    const Value* clip = args.Find("clip_id");
    const Value* span = args.Find("span_id");
    // IA3 -- a block of speech (list_speech_blocks): a whole sentence, the
    // passage a carton goes over.
    const Value* block = args.Find("block_id");
    const Value* at = args.Find("at");
    const Value* beat = args.Find("beat");
    const int given =
        static_cast<int>(clip != nullptr) + static_cast<int>(span != nullptr) +
        static_cast<int>(block != nullptr) + static_cast<int>(at != nullptr) +
        static_cast<int>(beat != nullptr);
    if (given != 1) {
        return Invalid(errorName, message,
                       "'" + path +
                           "' takes exactly one anchor: clip_id, span_id, "
                           "block_id, beat or at");
    }
    if (beat != nullptr) {
        return ReadBeatAnchor(backend, resolver, *beat, path, request,
                              errorName, message);
    }
    if (args.Find("end_span_id") != nullptr && span == nullptr) {
        return Invalid(errorName, message,
                       "'" + path + ".end_span_id' needs span_id");
    }
    if (args.Find("end_block_id") != nullptr && block == nullptr) {
        return Invalid(errorName, message,
                       "'" + path + ".end_block_id' needs block_id");
    }
    const bool passage = span != nullptr || block != nullptr;
    if (args.Find("force") != nullptr && !passage) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".force' only applies to span_id or "
                           "block_id");
    }
    const Value* revision = args.Find("transcript_revision");
    if (revision != nullptr && !passage) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".transcript_revision' only applies to span_id or "
                           "block_id");
    }
    if (clip != nullptr) {
        request.anchor = MotionAnchor::Clip;
        if (!ReadId(args, "clip_id", path, resolver, true, request.clip_id,
                    message)) {
            return Invalid(errorName, message, message);
        }
        return true;
    }
    if (at != nullptr) {
        if (!at->IsString() ||
            (at->AsString() != "start" && at->AsString() != "end")) {
            return Invalid(errorName, message,
                           "'" + path + ".at' must be \"start\" or \"end\"");
        }
        request.anchor =
            at->AsString() == "start" ? MotionAnchor::Start : MotionAnchor::End;
        return true;
    }
    const Value* first = span != nullptr ? span : block;
    const Value* end =
        args.Find(span != nullptr ? "end_span_id" : "end_block_id");
    const Value* force = args.Find("force");
    if (!first->IsString() || (end != nullptr && !end->IsString()) ||
        (force != nullptr && !force->IsBool())) {
        return Invalid(errorName, message,
                       "'" + path +
                           "' takes span ids of get_timeline_transcript or "
                           "block ids of list_speech_blocks as strings, "
                           "force as a boolean");
    }
    // IA3 -- docs/roadmap/IA-2026-09.md. A span or block id is a position in
    // one reading of the transcript: the reading comes with it, and an id
    // read before an edit is refused instead of anchoring on another
    // passage.
    if (revision == nullptr || !revision->IsString()) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".transcript_revision' is required with span_id "
                           "or block_id: pass the one get_timeline_transcript "
                           "or list_speech_blocks returned");
    }
    std::vector<TimelineTranscriptSpan> spans;
    if (!backend.ReadTimelineTranscriptSpans(spans, message)) {
        errorName = "IoError";
        return false;
    }
    const std::string current = TranscriptViewRevision(spans);
    if (revision->AsString() != current) {
        return Refuse(errorName, message, kStaleSelectionError,
                      StaleSelectionMessage(revision->AsString(), current));
    }
    std::string firstSpanId = first->AsString();
    std::string lastSpanId =
        end != nullptr ? end->AsString() : first->AsString();
    if (block != nullptr) {
        const std::vector<SpeechBlock> blocks = BuildSpeechBlocks(spans);
        const auto findBlock = [&](const std::string& id) {
            return std::find_if(blocks.begin(), blocks.end(),
                                [&](const SpeechBlock& candidate) {
                                    return candidate.block_id == id;
                                });
        };
        const auto firstBlock = findBlock(first->AsString());
        const auto lastBlock =
            end != nullptr ? findBlock(end->AsString()) : firstBlock;
        if (firstBlock == blocks.end() || lastBlock == blocks.end()) {
            return Refuse(errorName, message, "UnknownBlock",
                          "unknown block id '" +
                              (firstBlock == blocks.end() ? first->AsString()
                                                          : end->AsString()) +
                              "': read list_speech_blocks again");
        }
        if (lastBlock < firstBlock) {
            return Invalid(errorName, message,
                           "'" + path + ".end_block_id' comes before block_id");
        }
        firstSpanId = firstBlock->first_span_id;
        lastSpanId = lastBlock->last_span_id;
    }
    const auto find = [&](const std::string& id) {
        return std::find_if(spans.begin(), spans.end(),
                            [&](const TimelineTranscriptSpan& candidate) {
                                return candidate.span_id == id;
                            });
    };
    const auto firstSpan = find(firstSpanId);
    const auto lastSpan = find(lastSpanId);
    if (firstSpan == spans.end() || lastSpan == spans.end()) {
        return Refuse(
            errorName, message, "UnknownSpan",
            "unknown span id '" +
                (firstSpan == spans.end() ? firstSpanId : lastSpanId) +
                "': read get_timeline_transcript again");
    }
    if (lastSpan < firstSpan) {
        return Invalid(errorName, message,
                       "'" + path + ".end_span_id' comes before span_id");
    }
    const bool forced = force != nullptr && force->AsBool();
    for (auto it = firstSpan; it <= lastSpan; ++it) {
        if (it->likely_hallucinated && !forced) {
            return Invalid(errorName, message,
                           "span " + it->span_id +
                               " has words but no measured speech group: "
                               "check the rush and pass force:true only to "
                               "override");
        }
    }
    request.anchor = MotionAnchor::Span;
    request.span_in = firstSpan->timeline_in;
    request.span_source_id = firstSpan->source_id;
    // A block is a passage: without a duration, the instance covers it
    // whole, as an end id does.
    if (end != nullptr ||
        (block != nullptr && args.Find("duration") == nullptr)) {
        request.span_end = lastSpan->timeline_in.add(lastSpan->duration);
    }
    return true;
}

bool DispatchPlace(McpBackend& backend, const IdResolver& resolver,
                   const Value& args, std::string& resultJson,
                   std::string& errorName, std::string& message) {
    const std::string path = "place_motion_clip";
    MotionPlacementRequest request;
    RationalTime duration{0, 1};
    MotionSafeZone zone;
    bool zoneGiven = false;
    std::vector<std::string> fonts;
    const Value* composition = args.Find("composition_id");
    const Value* allow = args.Find("allow_layout_issues");
    if (!Known(
            args,
            {"composition_id", "clip_id", "span_id", "end_span_id", "block_id",
             "end_block_id", "force", "transcript_revision", "at", "beat",
             "offset", "duration", "track_id", "props", "zone",
             "margin_percent", "margins", "allow_layout_issues", "font_dirs"},
            path, message) ||
        !ReadTime(args, "offset", path, false, request.offset, message) ||
        !ReadTime(args, "duration", path, false, duration, message) ||
        !ReadId(args, "track_id", path, resolver, false, request.track_id,
                message) ||
        !ReadSafeZone(args, path, zone, zoneGiven, message) ||
        !ReadFontDirs(args, path, fonts, message)) {
        return Invalid(errorName, message, message);
    }
    if (composition == nullptr || !composition->IsString()) {
        return Invalid(errorName, message,
                       "'" + path + ".composition_id' is required");
    }
    if (allow != nullptr && !allow->IsBool()) {
        return Invalid(errorName, message,
                       "'" + path + ".allow_layout_issues' must be a boolean");
    }
    Value props;
    if (const Value* given = args.Find("props")) {
        if (!given->IsObject() && !given->IsNull()) {
            return Invalid(errorName, message,
                           "'" + path + ".props' must be an object");
        }
        if (given->IsObject() && !given->AsObject().empty()) {
            props = *given;
        }
    }
    if (args.Find("duration") != nullptr) {
        request.duration = duration;
    }
    if (args.Find("duration") != nullptr &&
        (args.Find("end_span_id") != nullptr ||
         args.Find("end_block_id") != nullptr)) {
        return Invalid(
            errorName, message,
            "'" + path + "' takes a duration or an end id, not both");
    }
    if (!ReadAnchor(backend, resolver, args, path, request, errorName,
                    message)) {
        return false;
    }
    Document document;
    const DocumentComposition* stored = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, composition->AsString(), stored,
                            errorName, message)) {
        return false;
    }
    MotionComposition parsed;
    MotionError motion;
    if (!ParseDocumentComposition(*stored, parsed, motion) ||
        !ValidateMotionInstanceProps(parsed, props, motion)) {
        return Refuse(
            errorName, message, motion.code,
            (motion.path.empty() ? "" : motion.path + ": ") + motion.message);
    }
    request.composition_id = stored->id;
    MotionPlacement placement;
    if (!ResolveMotionPlacement(document, request, placement, errorName,
                                message)) {
        return false;
    }
    // DEC34 -- no zone given: the timeline's, social_vertical in 9:16.
    if (!zoneGiven) {
        zone = TimelineMotionSafeZone(document.sequence.width,
                                      document.sequence.height);
    }

    // The check comes before the edit: a text that does not fit is refused,
    // not posed and reported.
    const MediaRate rate = document.sequence.frame_rate;
    const int64_t frameCount = static_cast<int64_t>(
        (static_cast<__int128>(placement.duration.value) * rate.num) /
        (static_cast<__int128>(placement.duration.rate) * rate.den));
    MotionSafeAreaReport report;
    report.zone = zone;
    if (!CheckInstance(backend, document, stored->id, parsed, props,
                       RationalTime{0, 1}, frameCount, rate, fonts, report,
                       errorName, message)) {
        return false;
    }
    const bool allowed = allow != nullptr && allow->AsBool();
    if (!report.issues.empty() && !allowed) {
        const bool outside =
            std::any_of(report.issues.begin(), report.issues.end(),
                        [](const MotionSafeAreaIssue& issue) {
                            return issue.kind == "outside_safe_area";
                        });
        return Refuse(errorName, message,
                      outside ? "OutsideSafeArea" : "TextDoesNotFit",
                      "checked against the " + zone.name + " safe area (" +
                          DescribeMotionSafeZone(zone) + ")" +
                          ZoneByDefaultNote(zoneGiven, zone) + ": " +
                          DescribeIssues(report) +
                          ". Shorten the text, lower its size or move it "
                          "(props or edit_motion_composition), or pass "
                          "allow_layout_issues:true to pose it as it is");
    }

    InsertMotionClipOperation insert;
    insert.track_id = placement.track_id;
    insert.composition_id = stored->id;
    insert.source_in = {0, 1};
    insert.duration = placement.duration;
    insert.motion_props = props;
    insert.clip_id = GenerateUlid();
    const Ulid clipId = insert.clip_id;
    // InsertMotionClip ripples what follows on its track. A clip placed in
    // a free range must move nothing, so when something follows it is
    // inserted past the track's last clip and moved into the range, one
    // undo step.
    const DocumentTrack* track = document.FindTrack(placement.track_id);
    RationalTime trackEnd{0, 1};
    bool follows = false;
    for (const DocumentClip& clip : track->clips) {
        const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
        if (trackEnd < clipEnd) {
            trackEnd = clipEnd;
        }
        if (!(clip.timeline_in < placement.timeline_in)) {
            follows = true;
        }
    }
    bool applied = false;
    if (!follows) {
        insert.timeline_in = placement.timeline_in;
        applied = backend.ApplyOperation(std::move(insert), resultJson,
                                         errorName, message);
    } else {
        insert.timeline_in = trackEnd;
        MoveClipOperation move;
        move.clip_id = clipId;
        move.track_id = placement.track_id;
        move.timeline_in = placement.timeline_in;
        std::vector<Operation> operations;
        operations.emplace_back(std::move(insert));
        operations.emplace_back(std::move(move));
        applied =
            backend.ApplyOperation(BatchOperation{path, std::move(operations)},
                                   resultJson, errorName, message);
    }
    if (!applied) {
        return false;
    }
    Annotate(resultJson, clipId, props);
    Value result;
    std::string error;
    if (Value::Parse(resultJson, result, error) && result.IsObject()) {
        result.Set("composition_id", Value::MakeString(stored->id));
        result.Set("track_id", Value::MakeString(placement.track_id));
        result.Set("timeline_in", TimeValue(placement.timeline_in));
        result.Set("duration", TimeValue(placement.duration));
        result.Set("composition_duration",
                   TimeValue(placement.composition_duration));
        result.Set("safe_area", ReportValue(report, zoneGiven));
        resultJson = result.Dump();
    }
    return true;
}

// MM3 -- a composition rendered alone (render_motion_video, the CLI),
// version and format resolved as a render resolves them: every frame of it.
bool CheckComposition(McpBackend& backend, const Value& args,
                      const std::string& path, const MotionSafeZone& zone,
                      bool atRest, const std::vector<std::string>& fonts,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    const Value* id = args.Find("composition_id");
    std::string variant;
    std::string format;
    Value overrides;
    for (const char* key : {"variant", "format"}) {
        const Value* field = args.Find(key);
        if (field != nullptr && !field->IsString()) {
            return Invalid(errorName, message,
                           "'" + path + "." + key + "' must be a string");
        }
        if (field != nullptr) {
            (std::string(key) == "variant" ? variant : format) =
                field->AsString();
        }
    }
    if (const Value* props = args.Find("props")) {
        if (!props->IsObject()) {
            return Invalid(errorName, message,
                           "'" + path + ".props' must be an object");
        }
        overrides = *props;
    }
    if (id == nullptr || !id->IsString()) {
        return Invalid(errorName, message,
                       "'" + path + ".composition_id' must be a string");
    }
    Document document;
    const DocumentComposition* stored = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id->AsString(), stored, errorName,
                            message)) {
        return false;
    }
    MotionComposition parsed;
    MotionComposition resolved;
    Value props;
    MotionError error;
    if (!ParseDocumentComposition(*stored, parsed, error) ||
        !ResolveMotionVariant(parsed, variant, format, overrides, resolved,
                              props, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    std::set<int64_t> frames;
    for (int64_t frame = 0; frame < resolved.duration; ++frame) {
        frames.insert(frame);
    }
    MotionSafeAreaReport report;
    report.zone = zone;
    report.at_rest_only = atRest;
    // The check reads each frame itself, never its shutter's instants.
    resolved.motion_blur_samples = 0;
    if (!CheckFrames(backend, document, stored->id, resolved, props, frames,
                     fonts, report, errorName, message)) {
        return false;
    }
    Value result = ReportValue(report);
    result.Set("composition_id", Value::MakeString(stored->id));
    result.Set("variant", variant.empty() ? Value::MakeNull()
                                          : Value::MakeString(variant));
    result.Set("format",
               format.empty() ? Value::MakeNull() : Value::MakeString(format));
    result.Set("width", Value::MakeInt(resolved.width));
    result.Set("height", Value::MakeInt(resolved.height));
    resultJson = result.Dump();
    return true;
}

bool DispatchCheckSafeArea(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string path = "check_motion_safe_area";
    Ulid clipId;
    MotionSafeZone zone;
    bool zoneGiven = false;
    std::vector<std::string> fonts;
    if (!Known(args,
               {"clip_id", "composition_id", "props", "variant", "format",
                "zone", "margin_percent", "margins", "at_rest", "font_dirs"},
               path, message) ||
        !ReadSafeZone(args, path, zone, zoneGiven, message) ||
        !ReadFontDirs(args, path, fonts, message)) {
        return Invalid(errorName, message, message);
    }
    bool atRest = false;
    if (const Value* rest = args.Find("at_rest")) {
        if (!rest->IsBool()) {
            return Invalid(errorName, message,
                           "'" + path + ".at_rest' must be a boolean");
        }
        atRest = rest->AsBool();
    }
    const bool byClip = args.Find("clip_id") != nullptr;
    const bool byComposition = args.Find("composition_id") != nullptr;
    if (byClip == byComposition) {
        return Invalid(errorName, message,
                       "'" + path +
                           "' takes exactly one of clip_id (a motion clip "
                           "of a timeline) and composition_id (a "
                           "composition rendered alone)");
    }
    // DEC34 -- a composition checked alone has no timeline: without a
    // zone it keeps the default one, whatever its size. A vertical film is
    // not always a social one (a screen at a fair), and the decision of 23
    // October 2026 names a 9:16 timeline.
    if (byComposition) {
        return CheckComposition(backend, args, path, zone, atRest, fonts,
                                resultJson, errorName, message);
    }
    if (args.Find("props") != nullptr || args.Find("variant") != nullptr ||
        args.Find("format") != nullptr) {
        return Invalid(errorName, message,
                       "'" + path +
                           "': a clip names its props; props, variant and "
                           "format go with composition_id");
    }
    if (!ReadId(args, "clip_id", path, resolver, true, clipId, message)) {
        return Invalid(errorName, message, message);
    }
    Document document;
    MotionClipInstant instant;
    if (!Snapshot(backend, document, errorName, message)) {
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        return Refuse(errorName, message, "UnknownClip",
                      "unknown clip_id '" + clipId + "'");
    }
    if (!MotionClipAt(document, clipId, clip->timeline_in, instant, errorName,
                      message)) {
        return false;
    }
    // DEC34 -- no zone given: the timeline's, social_vertical in 9:16.
    if (!zoneGiven) {
        zone = TimelineMotionSafeZone(document.sequence.width,
                                      document.sequence.height);
    }
    const MediaRate rate = document.sequence.frame_rate;
    const int64_t frameCount = static_cast<int64_t>(
        (static_cast<__int128>(clip->duration.value) * rate.num) /
        (static_cast<__int128>(clip->duration.rate) * rate.den));
    MotionSafeAreaReport report;
    report.zone = zone;
    report.at_rest_only = atRest;
    if (!CheckInstance(backend, document, instant.composition->id,
                       instant.parsed, clip->motion_props, clip->source_in,
                       frameCount, rate, fonts, report, errorName, message)) {
        return false;
    }
    Value result = ReportValue(report, zoneGiven);
    result.Set("clip_id", Value::MakeString(clip->id));
    result.Set("composition_id", Value::MakeString(instant.composition->id));
    // Checked in the composition's own frame. The export fits a composition
    // of another size into the timeline's frame, and applies the clip's
    // framing on top: both are named rather than folded in.
    const int32_t width = instant.parsed.width;
    const int32_t height = instant.parsed.height;
    if (width != document.sequence.width ||
        height != document.sequence.height || ClipHasFraming(*clip)) {
        result.Set(
            "note",
            Value::MakeString(
                "checked in the composition's own " + std::to_string(width) +
                "x" + std::to_string(height) + " frame; the timeline is " +
                std::to_string(document.sequence.width) + "x" +
                std::to_string(document.sequence.height) +
                (ClipHasFraming(*clip)
                     ? " and the clip's framing is not applied"
                     : ", into which the composition is fitted")));
    }
    resultJson = result.Dump();
    return true;
}

}  // namespace

bool RenderMotionClipFrame(McpBackend& backend, const Ulid& clipId,
                           const RationalTime& timelineTime,
                           const std::vector<std::string>& fontDirectories,
                           std::string& resultJson, std::string& errorName,
                           std::string& message) {
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      std::string("this process has no motion renderer "
                                  "(Core Text and Core Graphics): ") +
                          kMotionRendererHolder);
    }
    Document document;
    MotionClipInstant instant;
    if (!Snapshot(backend, document, errorName, message) ||
        !MotionClipAt(document, clipId, timelineTime, instant, errorName,
                      message)) {
        return false;
    }
    MotionFrameRequest request;
    request.composition = &instant.parsed;
    request.props = instant.clip->motion_props;
    request.frame = instant.frame;
    request.media_directory = backend.MotionMediaDirectory();
    request.font_directories = fontDirectories;
    MotionFrameImage image;
    MotionError error;
    if (!MotionReachFor(document, instant.composition->id, request.reach,
                        error) ||
        !render(request, image, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    // Which frame of which composition this is, then its geometry, so that
    // an agent reads where the clip's composition stands at that instant.
    Value described = Value::MakeObject();
    described.Set("clip_id", Value::MakeString(instant.clip->id));
    described.Set("composition_id", Value::MakeString(instant.composition->id));
    described.Set("frame", Value::MakeInt(instant.frame));
    described.Set("motion_props", instant.clip->motion_props);
    Value geometry;
    std::string parseError;
    if (Value::Parse(image.geometry, geometry, parseError)) {
        described.Set("geometry", std::move(geometry));
    }
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(image.png)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/png"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(described.Dump()));
    resultJson = envelope.Dump();
    return true;
}

bool IsMotionClipEditingTool(const std::string& name) {
    return name == "insert_motion_clip" || name == "set_clip_motion_props" ||
           name == "place_motion_clip";
}

void RegisterMotionClipTools(const McpToolAdder& add) {
    add("insert_motion_clip",
        "Pose a motion composition of the project (a title, a name on screen, "
        "a card: list_motion_compositions) on a video track, as a clip whose "
        "pictures the engine renders. Inserted like insert_clip: clips at or "
        "after timeline_in on its track move later by its duration, and tracks "
        "in sync_track_ids with them. The clip's time follows the "
        "composition's: source_in is a time of the composition (default 0), "
        "duration defaults to the rest of the composition, and the clip cannot "
        "run past its last frame. `props` gives this instance its prop values, "
        "checked against the composition's typed props. The clip then trims, "
        "cuts, moves and takes opacity and dissolves like any other. Returns "
        "the new clip_id.",
        Schema({{"track_id", Described("string", "Video track to pose it on.")},
                {"composition_id", Described("string", "Composition to show.")},
                {"timeline_in", kTime},
                {"source_in", kTime},
                {"duration", kTime},
                {"props", Described("object",
                                    "Prop values of this instance, over the "
                                    "composition's defaults.")},
                {"sync_track_ids",
                 "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
                 "\"description\":" +
                     Quoted("Other tracks that ripple with the insertion.") +
                     "}"}},
               {"track_id", "composition_id", "timeline_in"},
               "insert_motion_clip arguments"),
        DispatchInsert);

    // M5 -- docs/roadmap/MOTION-2026-09.md. PRD38 -- and the zone of a
    // vertical social video, by name, or a margin per side. DEC34 -- the
    // default of a clip on a 9:16 timeline is the social one.
    const std::string zone =
        "{\"type\":\"string\",\"enum\":[\"default\",\"social_vertical\"],"
        "\"description\":" +
        Quoted(
            "default: margin_percent per side; social_vertical: Reels/TikTok "
            "(14 % top, 35 % bottom, 6 % left, right edge at 940/1080). Unset: "
            "social_vertical on a 9:16 timeline, else default.") +
        "}";
    const std::string margin = Described(
        "integer",
        "Margin of the default zone on each side, in % of the frame's width "
        "and height (default 5: EBU R 95's graphics-safe area).");
    const std::string margins =
        "{\"type\":\"object\",\"description\":" +
        Quoted(
            "Instead of zone: each side's margin in % of the frame's width "
            "(left, right) or height (top, bottom), integers from 0 to "
            "45; a side left out keeps 5.") +
        ",\"properties\":{\"top\":{\"type\":\"integer\"},\"bottom\":{"
        "\"type\":\"integer\"},\"left\":{\"type\":\"integer\"},\"right\":{"
        "\"type\":\"integer\"}},\"additionalProperties\":false}";
    const std::string fontDirs =
        "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"description\":" +
        Quoted(
            "Font directories to register for this process first, as "
            "render_motion_frame takes them.") +
        "}";
    add("place_motion_clip",
        "Pose an instance of a motion composition where you name it, and let "
        "the engine compute the range and the track: on a clip, a transcribed "
        "passage, a beat of the music, or the start or the end of the film "
        "(at). `offset` moves the start later into the anchor. Duration, "
        "unless given: the passage named, else the composition up to the end "
        "of the anchor's shot (a jump cut on the same source does not end it), "
        "or of the film for `at`. Track, unless given: the lowest video track "
        "above the anchor that is free over the range; nothing on the timeline "
        "moves. Every frame is laid out with the real fonts and checked "
        "against the safe area before it is posed. Returns clip_id, track_id, "
        "timeline_in, duration and the safe-area report. To see it over the "
        "picture, inspect_cut on the anchor clip.",
        Schema(
            {{"composition_id", Described("string", "Composition to pose.")},
             {"clip_id", Described("string", "Anchor: this clip.")},
             {"span_id",
              Described("string",
                        "Anchor: this span of get_timeline_transcript; the "
                        "instance starts on its first word.")},
             {"end_span_id",
              Described("string",
                        "Last span of the passage, inclusive: the "
                        "instance lasts until its last word ends.")},
             {"block_id",
              Described("string",
                        "Anchor: this block of list_speech_blocks. Without "
                        "a duration the instance covers the whole block.")},
             {"end_block_id",
              Described("string", "Last block of the passage, inclusive.")},
             {"force", Described("boolean",
                                 "Accept a passage marked "
                                 "likely_hallucinated.")},
             {"transcript_revision",
              Described(
                  "string",
                  "Required with span_id or block_id: the "
                  "transcript_revision of the reading the ids come from.")},
             {"at",
              "{\"type\":\"string\",\"enum\":[\"start\",\"end\"],"
              "\"description\":" +
                  Quoted("Anchor: the film's first or last frame.") + "}"},
             {"beat",
              "{\"type\":\"object\",\"description\":" +
                  Quoted("Anchor: a beat of the music, by the index "
                         "list_tempo_grid gives it, with that call's "
                         "source_in/source_out if it had a window; clip_id for "
                         "a music heard twice. The engine finds the frame it "
                         "is heard on.") +
                  ",\"properties\":{\"media_id\":{\"type\":\"string\"},"
                  "\"index\":{\"type\":\"integer\"},\"source_in\":" +
                  std::string(kTime) + ",\"source_out\":" + kTime +
                  ",\"clip_id\":{\"type\":\"string\"}},\"required\":["
                  "\"media_id\",\"index\"],\"additionalProperties\":false}"},
             {"offset", kTime},
             {"duration", kTime},
             {"track_id", Described("string", "Video track to use instead.")},
             {"props", Described("object",
                                 "Prop values of this instance, over the "
                                 "composition's defaults.")},
             {"zone", zone},
             {"margin_percent", margin},
             {"margins", margins},
             {"allow_layout_issues",
              Described("boolean",
                        "Pose it even if the check finds issues (a "
                        "deliberate bleed); the report still lists "
                        "them.")},
             {"font_dirs", fontDirs}},
            {"composition_id"}, "place_motion_clip arguments"),
        DispatchPlace);

    add("check_motion_safe_area",
        "Check every frame a motion clip shows -- or, with composition_id, "
        "every frame of a composition rendered alone (render_motion_video), in "
        "a version and a format -- against the safe area (5 % per side by "
        "default; zone social_vertical for a vertical video on Reels or "
        "TikTok, the default of a clip on a 9:16 timeline), with its props and "
        "the real fonts: which text or image leaves it (the edges crossed, the "
        "first and last composition frames, its bounds in pixels), and which "
        "word is too long for its box and was broken between two letters "
        "(word_split). Read-only; run it after set_clip_motion_props or "
        "edit_motion_composition.",
        Schema(
            {{"clip_id",
              Described("string", "Motion clip to check (or composition_id).")},
             {"composition_id",
              Described("string",
                        "Composition rendered alone to check (or "
                        "clip_id).")},
             {"props", Described("object",
                                 "With composition_id: prop values over "
                                 "the version's and the format's.")},
             {"variant",
              Described("string", "With composition_id: the version.")},
             {"format", Described("string",
                                  "With composition_id: the format "
                                  "(its size and props).")},
             {"zone", zone},
             {"margin_percent", margin},
             {"margins", margins},
             {"at_rest",
              Described("boolean",
                        "Count only what is outside while it stands "
                        "still: an element crossing the edge as it moves "
                        "in or out is a passage, counted in in_motion. "
                        "Each issue says at_rest either way.")},
             {"font_dirs", fontDirs}},
            {}, "check_motion_safe_area arguments"),
        DispatchCheckSafeArea);

    add("set_clip_motion_props",
        "Set prop values of one motion clip -- this instance only, not the "
        "composition's defaults. Merged into the values the clip already has: "
        "a null value removes one, so the composition's default applies again; "
        "replace:true sets exactly `props`.",
        Schema({{"clip_id", Described("string", "Motion clip to change.")},
                {"props", Described("object", "Prop values to set.")},
                {"replace",
                 Described("boolean",
                           "Replace every instance prop instead of merging "
                           "(default false).")}},
               {"clip_id", "props"}, "set_clip_motion_props arguments"),
        DispatchSetProps);
}
