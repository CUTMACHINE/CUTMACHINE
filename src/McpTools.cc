#include "McpTools.h"
#include "McpToolsInternal.h"

#include "AfterBridge.h"
#include "BoundaryAir.h"
#include "CaptionBreakModel.h"
#include "CaptionGeneration.h"
#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipGradeCopy.h"
#include "ClipSpeed.h"
#include "ColorCdl.h"
#include "ColorEffects.h"
#include "ColorMeasure.h"
#include "CutInspection.h"
#include "CutInspectionRender.h"
#include "CutReview.h"
#include "CutWords.h"
#include "Document.h"
#include "DocumentDelta.h"
#include "EditLog.h"
#include "EditPlan.h"
#include "Edl.h"
#include "EngineLog.h"
#include "FfmpegCapabilities.h"
#include "FontCatalog.h"
#include "Hesitations.h"
#include "LookLut.h"
#include "McpGenerationTools.h"
#include "McpMotionClipTools.h"
#include "McpMotionTools.h"
#include "MfaAlignment.h"
#include "MotionLibraryBase.h"
#include "Multicam.h"
#include "MusicStructure.h"
#include "PauseTightening.h"
#include "Procedures.h"
#include "ProjectArchive.h"
#include "RationalTime.h"
#include "SeparateSound.h"
#include "SequenceFormat.h"
#include "ShotDescriptions.h"
#include "SoundRender.h"
#include "SoundStem.h"
#include "SourceAddress.h"
#include "SpeechSelection.h"
#include "SubjectTracking.h"
#include "Subtitles.h"
#include "TempoGrid.h"
#include "TimelineCheck.h"
#include "TimelineSheets.h"
#include "TimelineStats.h"
#include "TimelineView.h"
#include "TimelineWords.h"
#include "TransitionRender.h"
#include "Ulid.h"
#include "WordBoundaries.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace mcp_tools {

std::string Esc(const std::string& text) {
    return mcp_json::EscapeJsonString(text);
}

std::string StringSchema(const std::string& description) {
    return "{\"type\":\"string\",\"description\":\"" + Esc(description) + "\"}";
}

// That an ID also takes a unique prefix or an alias is said once, in the
// server's instructions (McpServer.cc): it rode on 141 arguments.
std::string IdSchema(const std::string& description) {
    return description.empty() ? std::string("{\"type\":\"string\"}")
                               : StringSchema(description);
}

std::string IdArraySchema(const std::string& description) {
    return "{\"type\":\"array\",\"items\":{\"type\":\"string\"}" +
           (description.empty()
                ? std::string()
                : ",\"description\":\"" + Esc(description) + "\"") +
           "}";
}

std::string BoolSchema(const std::string& description) {
    return "{\"type\":\"boolean\",\"description\":\"" + Esc(description) +
           "\"}";
}

std::string IntSchema(const std::string& description) {
    return "{\"type\":\"integer\",\"description\":\"" + Esc(description) +
           "\"}";
}

std::string PositiveIntSchema(const std::string& description) {
    return "{\"type\":\"integer\",\"minimum\":1,\"description\":\"" +
           Esc(description) + "\"}";
}

std::string EnumSchema(const std::vector<std::string>& values,
                       const std::string& description) {
    std::string valuesJson = "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) valuesJson += ",";
        valuesJson += "\"" + values[index] + "\"";
    }
    valuesJson += "]";
    // PRD15 -- an empty description is left out rather than published.
    return "{\"type\":\"string\",\"enum\":" + valuesJson +
           (description.empty()
                ? std::string()
                : ",\"description\":\"" + Esc(description) + "\"") +
           "}";
}

const char kTimeSchemaText[] =
    "{\"type\":\"object\",\"description\":\"Exact time: value ticks of 1/rate "
    "s.\",\"properties\":{\"value\":{\"type\":\"integer\"},"
    "\"rate\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"value\","
    "\"rate\"],\"additionalProperties\":false}";

const char kFractionSchemaText[] =
    "{\"type\":\"object\",\"description\":\"Exact fraction num/den.\","
    "\"properties\":{\"num\":{\"type\":\"integer\"},"
    "\"den\":{\"type\":\"integer\"}},\"required\":[\"num\",\"den\"],"
    "\"additionalProperties\":false}";

std::string TimeSchema(const std::string& description) {
    std::string schema = kTimeSchemaText;
    const std::string generic = "\"description\":\"";
    const size_t at = schema.find(generic);
    schema.insert(at + generic.size(), Esc(description) + " ");
    return schema;
}

std::string FractionSchema(const std::string& description) {
    std::string schema = kFractionSchemaText;
    const std::string marker = "\"description\":\"";
    const size_t begin = schema.find(marker) + marker.size();
    const size_t end = schema.find("\"", begin);
    schema.replace(begin, end - begin, Esc(description));
    return schema;
}

const std::string kColorRegionSchema =
    std::string(
        "{\"type\":\"object\",\"description\":\"Rectangle of the "
        "measured picture (what the sequence shows of the clip), as exact "
        "fractions of its width and height from the top-left corner.\","
        "\"properties\":{\"x\":") +
    kFractionSchemaText + ",\"y\":" + kFractionSchemaText +
    ",\"width\":" + kFractionSchemaText + ",\"height\":" + kFractionSchemaText +
    "},\"required\":[\"x\",\"y\",\"width\",\"height\"],"
    "\"additionalProperties\":false}";

std::string ArraySchema(const std::string& itemSchema,
                        const std::string& description) {
    // PRD15 -- an empty description is left out rather than published.
    return "{\"type\":\"array\",\"items\":" + itemSchema +
           (description.empty()
                ? std::string()
                : ",\"description\":\"" + Esc(description) + "\"") +
           "}";
}

const char kCutWordsNote[] =
    " cut_words names, per source, the aligned words the cut removed "
    "(whole:false when cut into): targeted the words asked for, carried any "
    "other. A source without alignment is listed as unchecked, its words not "
    "checked.";

bool IsTimelineEditingTool(const std::string& name) {
    static const std::vector<std::string> kNames = {
        "insert_clip",
        "edit_source_range",
        "delete_timeline_range",
        "remove_clip",
        "clear_clip",
        "clear_clips",
        "trim_clip",
        "move_clip",
        "move_clips",
        "split_clip",
        "split_at_interval",
        "delete_gap",
        "detach_audio",
        "move_linked_clips",
        "trim_linked_clips",
        "shorten_linked_clip",
        "ripple_trim",
        "roll_edit",
        "slip_edit",
        // FC3 -- FILMCRAFT-2026-10.
        "slide_edit",
        "rate_stretch",
        "remove_linked_clips",
        "clear_linked_clips",
        "split_linked_clips",
        "paste_clips",
        "add_track",
        "import_srt",
        "remove_track",
        "set_track_lock",
        "set_track_sync_lock",
        "set_track_output",
        "update_sequence",
        "set_color_management",
        "conform_color_management",
        "add_bin",
        "remove_bin",
        "rename_bin",
        "move_bin",
        "set_media_bin",
        // G7 -- a media's as-shot white balance, a document edit like its
        // bin. G3 -- and the colour input it is read as.
        "set_media_white_balance",
        "set_media_color_input",
        // PL2 -- docs/roadmap/PLANS-2026-10.md. A media's described shots,
        // a document edit like its white balance.
        "describe_shots",
        // GN3 -- docs/roadmap/GENERATIF-2026-10.md. The provenance of what
        // a generation made joins the history of the timeline it names.
        "generate_media",
        // GN6b -- a voice or a voice-over the engine made, marked in the
        // history of the timeline it names, as generate_media is.
        "design_voice",
        "generate_voice_over",
        "add_marker",
        "remove_marker",
        "update_marker",
        // FC7 -- markers read from a CMX 3600 list, one undo step.
        "import_edl_markers",
        "add_transition",
        "remove_transition",
        "update_transition",
        "set_clip_link",
        // Out of lot (4 November 2026) -- LinkGroupRepair.h.
        "split_link_groups",
        // SYNCHRO-2026-10 SY2 -- a separate sound laid and linked.
        "sync_audio",
        // SN4 -- docs/roadmap/SON-2026-10.md. A rendered stem laid at its
        // timecode.
        "place_sound_stem",
        "set_clip_effects",
        // GR11 -- one clip's grade pasted onto others.
        "paste_clip_grade",
        // G4 -- a CDL in place of clips' primaries.
        "apply_cdl",
        "set_clip_opacity",
        // EF3 -- docs/roadmap/EFFETS-2026-10.md.
        "set_clip_blur",
        // PA8 -- docs/roadmap/PARITE-2026-11.md.
        "set_clip_mask",
        "set_clip_audio",
        "set_clip_speed",
        "set_clip_framing",
        // R2 -- docs/roadmap/REEL-2026-09.md.
        "set_clip_animation",
        // Out of lot -- Resolve's Enable/Disable Clip (29 October 2026).
        "set_clip_enabled",
        // CA3 -- docs/roadmap/CADRAGE-2026-10.md.
        "track_subject_framing",
        // PA7 -- docs/roadmap/PARITE-2026-11.md.
        "stabilize_clip",
        // R6 -- docs/roadmap/REEL-2026-09.md.
        "set_sequence_lut",
        "add_caption_style",
        "update_caption_style",
        "generate_captions",
        // PA5 -- a translated caption track.
        "translate_captions",
        "remove_caption_style",
        "set_clip_caption",
        "add_multicam_group",
        "set_multicam_active_angle",
        // F1.5 -- a cut through a group, decided on 8 October 2026.
        "split_multicam_group",
        "remove_multicam_group",
        "create_interview_short",
        "clean_disfluencies",
        "tighten_pauses",
        "trim_boundary_air",
        "close_junction_air",
        "remove_words",
        "remove_source_ranges",
        "conform_sequence",
        "undo",
        "redo",
    };
    return std::find(kNames.begin(), kNames.end(), name) != kNames.end() ||
           IsMotionEditingTool(name) || IsMotionClipEditingTool(name);
}

bool IsTimelineReadingTool(const std::string& name) {
    static const std::vector<std::string> kNames = {
        "describe",
        // SN7 -- docs/roadmap/SON-2026-10.md. Renders a stem on the cuts of
        // one timeline, read as it is now.
        "render_sound",
        "get_timeline_transcript",
        "timeline_stats",
        "list_shot_quality",
        "contact_sheet",
        "cut_sheet",
        "list_disfluencies",
        "list_speech_onsets",
        "locate_source_frame",
        "read_frame",
        "list_multicam_groups",
        // R7 -- docs/roadmap/REEL-2026-09.md. Places a source's beats on one
        // timeline, so it names that timeline like every other read.
        "list_clip_tempo_beats",
        // R8 -- the ranges to scan are a property of one timeline, so both
        // halves of the pass name theirs like every other timeline read.
        "list_isolated_frames",
        "analyze_isolated_frames",
        // R9 -- writes one timeline as a Resolve FCP7 XML, so it names its
        // timeline the way every other timeline read does.
        "export_resolve_xml",
        // FC7 -- the CMX 3600 lists of one timeline and of its markers.
        "export_edl",
        "export_edl_markers",
        // PA1 -- the OTIO and FCPXML files of one timeline.
        "export_otio",
        "export_fcpxml",
        // PA16 -- sends one timeline to Resolve.
        "send_to_resolve",
        // R1 -- the rectangle of one clip of one timeline.
        "list_clip_framing",
        // IA4 -- a window of one timeline, rendered as its export would be.
        "inspect_cut",
        // RC1 -- docs/roadmap/RACCORDS-2026-10.md. Every junction of one
        // timeline, rendered and measured.
        "review_cuts",
        // G1 -- docs/roadmap/GRADE-2026-09.md. Frames of one timeline's clips,
        // graded as its export grades them.
        "measure_color",
        // G2 -- two clips of one timeline, matched.
        "match_color",
        // AGENT-2026-09 A16 -- every clip of one timeline, exposed.
        "propose_exposure",
        // AGENT-2026-09 A20 -- every clip of one timeline, read on its face.
        "propose_grade",
        // AGENT-2026-09 A24 -- the montage's words from the alignments.
        "list_timeline_words",
        // G4 -- clips of one timeline, folded into CDLs.
        "export_cdl",
        // M5 -- docs/roadmap/MOTION-2026-09.md. Lays out every frame one
        // motion clip of one timeline shows.
        "check_motion_safe_area",
        // IA3 -- docs/roadmap/IA-2026-09.md. The blocks of one timeline's
        // transcript view, and a selection read from them.
        "list_speech_blocks",
        "inspect_speech_selection",
        // OR4 -- docs/roadmap/DONNEES-2026-10.md. One timeline judged
        // against a policy.
        "check_timeline",
        // MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. One timeline's journal,
        // grouped by why.
        "list_motifs",
        // PA9 -- one timeline's journal, as what changed since a state.
        "list_changes",
    };
    return std::find(kNames.begin(), kNames.end(), name) != kNames.end();
}

bool CheckKnownKeys(const Value& args, const std::vector<std::string>& allowed,
                    const std::string& path, std::string& message) {
    if (!args.IsObject()) {
        message = "'" + path + "' arguments must be a JSON object";
        return false;
    }
    for (const auto& entry : args.AsObject()) {
        if (entry.first == "timeline_id" &&
            (IsTimelineEditingTool(path) || IsTimelineReadingTool(path))) {
            continue;
        }
        if (std::find(allowed.begin(), allowed.end(), entry.first) ==
            allowed.end()) {
            message =
                "'" + path + "' has unknown argument '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

bool ReadString(const Value& args, const std::string& key,
                const std::string& path, bool required,
                const std::string& defaultValue, std::string& out,
                std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        out = defaultValue;
        return true;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    out = field->AsString();
    return true;
}

bool ReadBool(const Value& args, const std::string& key,
              const std::string& path, bool defaultValue, bool& out,
              std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        out = defaultValue;
        return true;
    }
    if (!field->IsBool()) {
        message = "'" + path + "." + key + "' must be a boolean";
        return false;
    }
    out = field->AsBool();
    return true;
}

bool ReadInt64(const Value& args, const std::string& key,
               const std::string& path, bool required, int64_t defaultValue,
               int64_t& out, std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        out = defaultValue;
        return true;
    }
    if (!field->IsNumber() || !field->AsInt64(out)) {
        message = "'" + path + "." + key +
                  "' must be an exact integer (no fraction, no exponent)";
        return false;
    }
    return true;
}

bool ReadInt32(const Value& args, const std::string& key,
               const std::string& path, bool required, int32_t defaultValue,
               int32_t& out, std::string& message) {
    int64_t wide = defaultValue;
    if (!ReadInt64(args, key, path, required, defaultValue, wide, message))
        return false;
    if (wide < std::numeric_limits<int32_t>::min() ||
        wide > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + "." + key + "' is out of 32-bit range";
        return false;
    }
    out = static_cast<int32_t>(wide);
    return true;
}

bool ReadTimeValue(const Value& field, const std::string& path,
                   RationalTime& out, std::string& message) {
    if (!field.IsObject()) {
        message = "'" + path + "' must be an object with 'value' and 'rate'";
        return false;
    }
    for (const auto& entry : field.AsObject()) {
        if (entry.first != "value" && entry.first != "rate") {
            message = "'" + path + "' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    const Value* valueField = field.Find("value");
    const Value* rateField = field.Find("rate");
    if (!valueField || !rateField) {
        message = "'" + path + "' requires both 'value' and 'rate'";
        return false;
    }
    int64_t value = 0;
    int64_t rate = 0;
    if (!valueField->IsNumber() || !valueField->AsInt64(value)) {
        message = "'" + path + ".value' must be an exact integer";
        return false;
    }
    if (!rateField->IsNumber() || !rateField->AsInt64(rate)) {
        message = "'" + path + ".rate' must be an exact integer";
        return false;
    }
    if (rate <= 0 || rate > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + ".rate' must be a positive 32-bit integer";
        return false;
    }
    out = RationalTime{value, static_cast<int32_t>(rate)};
    return true;
}

bool ReadTime(const Value& args, const std::string& key,
              const std::string& path, bool required, RationalTime& out,
              std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        return true;
    }
    return ReadTimeValue(*field, path + "." + key, out, message);
}

bool ReadFractionValue(const Value& field, const std::string& path,
                       int32_t& num, int32_t& den, std::string& message) {
    if (!field.IsObject()) {
        message = "'" + path + "' must be an object with 'num' and 'den'";
        return false;
    }
    for (const auto& entry : field.AsObject()) {
        if (entry.first != "num" && entry.first != "den") {
            message = "'" + path + "' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    const Value* numField = field.Find("num");
    const Value* denField = field.Find("den");
    if (!numField || !denField) {
        message = "'" + path + "' requires both 'num' and 'den'";
        return false;
    }
    int64_t numWide = 0;
    int64_t denWide = 0;
    if (!numField->IsNumber() || !numField->AsInt64(numWide)) {
        message = "'" + path + ".num' must be an exact integer";
        return false;
    }
    if (!denField->IsNumber() || !denField->AsInt64(denWide)) {
        message = "'" + path + ".den' must be an exact integer";
        return false;
    }
    if (denWide == 0 || denWide < std::numeric_limits<int32_t>::min() ||
        denWide > std::numeric_limits<int32_t>::max() ||
        numWide < std::numeric_limits<int32_t>::min() ||
        numWide > std::numeric_limits<int32_t>::max()) {
        message = "'" + path + "' must have a non-zero 32-bit 'den'";
        return false;
    }
    num = static_cast<int32_t>(numWide);
    den = static_cast<int32_t>(denWide);
    return true;
}

bool ReadId(const Value& args, const std::string& key, const std::string& path,
            const IdResolver& resolver, bool required, Ulid& out,
            std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        if (required) {
            message = "'" + path + "." + key + "' is required";
            return false;
        }
        return true;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    return resolver.Resolve(path + "." + key, field->AsString(), out, message);
}

bool ReadOptionalIdOrEmpty(const Value& args, const std::string& key,
                           const std::string& path, const IdResolver& resolver,
                           Ulid& out, std::string& message) {
    const Value* field = args.Find(key);
    if (!field) {
        out.clear();
        return true;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    if (field->AsString().empty()) {
        out.clear();
        return true;
    }
    return resolver.Resolve(path + "." + key, field->AsString(), out, message);
}

bool ReadIdArray(const Value& args, const std::string& key,
                 const std::string& path, const IdResolver& resolver,
                 std::vector<Ulid>& out, std::string& message) {
    const Value* field = args.Find(key);
    out.clear();
    if (!field) return true;
    if (!field->IsArray()) {
        message = "'" + path + "." + key + "' must be an array";
        return false;
    }
    for (size_t index = 0; index < field->AsArray().size(); ++index) {
        const Value& item = field->AsArray()[index];
        const std::string itemPath =
            path + "." + key + "[" + std::to_string(index) + "]";
        if (!item.IsString()) {
            message = "'" + itemPath + "' must be a string";
            return false;
        }
        Ulid resolved;
        if (!resolver.Resolve(itemPath, item.AsString(), resolved, message))
            return false;
        out.push_back(resolved);
    }
    return true;
}

bool Fail(std::string& errorName, std::string& message, std::string text) {
    errorName = "ValidationFailed";
    message = std::move(text);
    return false;
}

bool MeasureCutWords(McpBackend& backend, const Document& before,
                     const Operation& operation, const TargetedWords& targeted,
                     Value& view) {
    Document after = before;
    Operation dryRun = operation;
    Operation inverse = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!ApplyOperation(after, dryRun, inverse, error, message)) return false;
    const std::vector<RemovedSourceRange> removed =
        RemovedAudioMaterial(before, after);
    std::map<Ulid, Transcript> transcripts;
    for (const RemovedSourceRange& range : removed) {
        if (transcripts.count(range.source_id)) continue;
        Transcript words;
        std::string ignored;
        if (backend.ReadSourceTranscript(range.source_id, words, ignored))
            transcripts.emplace(range.source_id, std::move(words));
    }
    std::string parseError;
    return Value::Parse(
        SerializeCutWordsReport(CheckCutWords(removed, transcripts, targeted)),
        view, parseError);
}

bool ReadStringArray(const Value& args, const std::string& key,
                     const std::string& tool, std::vector<std::string>& out,
                     std::string& message) {
    out.clear();
    const Value* field = args.Find(key);
    if (field == nullptr) return true;
    if (!field->IsArray()) {
        message = "'" + tool + "." + key + "' must be an array of strings";
        return false;
    }
    for (const Value& item : field->AsArray()) {
        if (!item.IsString() || item.AsString().empty()) {
            message = "'" + tool + "." + key + "' must hold non-empty strings";
            return false;
        }
        out.push_back(item.AsString());
    }
    return true;
}

bool ReadColorRegion(const Value& args, const std::string& key,
                     const std::string& tool, ColorMeasureRegion& region,
                     std::string& message) {
    const Value* field = args.Find(key);
    if (field == nullptr) return true;
    const std::string path = tool + "." + key;
    static const std::vector<std::string> kRegionKeys = {"x", "y", "width",
                                                         "height"};
    if (!field->IsObject()) {
        message = "'" + path +
                  "' must be an object with 'x', 'y', 'width' and 'height'";
        return false;
    }
    if (!CheckKnownKeys(*field, kRegionKeys, path, message)) return false;
    region.present = true;
    EffectParamValue* parts[] = {&region.x, &region.y, &region.width,
                                 &region.height};
    for (size_t index = 0; index < kRegionKeys.size(); ++index) {
        const std::string partPath = path + "." + kRegionKeys[index];
        const Value* part = field->Find(kRegionKeys[index]);
        if (part == nullptr) {
            message = "'" + partPath + "' is required";
            return false;
        }
        if (!ReadFractionValue(*part, partPath, parts[index]->num,
                               parts[index]->den, message))
            return false;
    }
    if (!ValidateColorMeasureRegion(region, message)) {
        message = "'" + path + "': " + message;
        return false;
    }
    return true;
}

bool ReadAnalysisMediaIds(const Value& args, const std::string& tool,
                          const IdResolver& resolver, std::vector<Ulid>& out,
                          bool& batch, std::string& message) {
    Ulid single;
    std::vector<Ulid> many;
    const bool hasSingle = args.Find("media_id") != nullptr;
    const bool hasMany = args.Find("media_ids") != nullptr;
    if (hasSingle == hasMany) {
        message = "'" + tool + "' takes media_id or media_ids, one of them";
        return false;
    }
    if (hasSingle) {
        if (!ReadId(args, "media_id", tool, resolver, true, single, message))
            return false;
        out = {single};
        batch = false;
        return true;
    }
    if (!ReadIdArray(args, "media_ids", tool, resolver, many, message))
        return false;
    if (many.empty()) {
        message = "'" + tool + ".media_ids' must name at least one media";
        return false;
    }
    out = std::move(many);
    batch = true;
    return true;
}

bool RunAnalysisBatch(const std::vector<Ulid>& mediaIds, bool batch,
                      const std::string& tool,
                      const std::function<bool(const Ulid&, std::string& json,
                                               std::string& error)>& analyse,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    if (!batch) {
        std::string error;
        if (analyse(mediaIds.front(), resultJson, error)) return true;
        errorName = "IoError";
        message = error;
        return false;
    }
    Value results = Value::MakeArray();
    int64_t analysed = 0;
    for (const Ulid& mediaId : mediaIds) {
        Value entry = Value::MakeObject();
        entry.Set("media_id", Value::MakeString(mediaId));
        std::string json;
        std::string error;
        if (analyse(mediaId, json, error)) {
            Value parsed;
            std::string parseError;
            entry.Set("ok", Value::MakeBool(true));
            if (Value::Parse(json, parsed, parseError))
                entry.Set("result", std::move(parsed));
            ++analysed;
        } else {
            entry.Set("ok", Value::MakeBool(false));
            entry.Set("error", Value::MakeString(error));
        }
        results.Push(std::move(entry));
    }
    if (analysed == 0) {
        errorName = "IoError";
        message = "'" + tool + "' analysed none of the " +
                  std::to_string(mediaIds.size()) + " media: " + results.Dump();
        return false;
    }
    Value root = Value::MakeObject();
    root.Set("analyzed", Value::MakeInt(analysed));
    root.Set("failed",
             Value::MakeInt(static_cast<int64_t>(mediaIds.size()) - analysed));
    root.Set("results", std::move(results));
    resultJson = root.Dump();
    return true;
}

Value BeatTimeValue(const RationalTime& time) {
    Value result = Value::MakeObject();
    result.Set("value", Value::MakeInt(time.value));
    result.Set("rate", Value::MakeInt(time.rate));
    return result;
}

}  // namespace mcp_tools

using namespace mcp_tools;

namespace {

// PRD17 -- ROADMAP.md. The tools that act on one timeline and, without a
// timeline_id, take the active one: every edit and every read of a timeline,
// the timeline-scoped tools of McpToolRegistry::Call, a timeline transcribed
// and a Resolve timeline read in. What strict mode refuses unnamed.
bool TargetsOneTimeline(const std::string& name) {
    static const std::vector<std::string> kScoped = {
        "export_timeline",          "verify_export",
        "check_caption_legibility", "analyze_timeline_audio",
        "begin_intervention",       "prepare_edit_plan",
        "transcribe_timeline",      "import_resolve_timeline"};
    return IsTimelineEditingTool(name) || IsTimelineReadingTool(name) ||
           std::find(kScoped.begin(), kScoped.end(), name) != kScoped.end();
}

bool NamesTimeline(const Value& args) {
    const Value* value = args.IsObject() ? args.Find("timeline_id") : nullptr;
    return value != nullptr && value->IsString() && !value->AsString().empty();
}

// PRD40 -- ROADMAP.md. Strict mode keeps the agents of a series off each
// other's timelines; a call that names only a rush reads none of them, and
// four refusals of read_frame on a rush made an editor of the 3 October
// series name some timeline to look at a shot. Of the tools strict mode
// targets, read_frame alone can be pointed at a rush and nothing else.
// Those that take a media_id but place it on the timeline
// (locate_source_frame, list_clip_tempo_beats) read one; the rush readers
// (media_sheet, list_source_words, list_stable_windows, the audio
// analyses) target no timeline and were never refused.
bool ReadsOnlyARush(const std::string& name, const Value& args) {
    return name == "read_frame" && args.IsObject() &&
           args.Find("media_id") != nullptr && args.Find("clip_id") == nullptr;
}

// MM3 -- docs/roadmap/MOTION-MARQUE-2026-10.md. check_motion_safe_area
// pointed at a composition rendered alone reads no timeline either.
bool ReadsOnlyAComposition(const std::string& name, const Value& args) {
    return name == "check_motion_safe_area" && args.IsObject() &&
           args.Find("composition_id") != nullptr &&
           args.Find("clip_id") == nullptr;
}

// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. list_motifs pointed at the
// project's journal reads no timeline.
bool ReadsTheProjectJournal(const std::string& name, const Value& args) {
    const Value* journal = args.IsObject() ? args.Find("journal") : nullptr;
    return name == "list_motifs" && journal != nullptr && journal->IsString() &&
           journal->AsString() == "project";
}

bool StrictModeNeedsTimeline(const std::string& name, const Value& args) {
    return TargetsOneTimeline(name) && !NamesTimeline(args) &&
           !ReadsOnlyARush(name, args) && !ReadsOnlyAComposition(name, args) &&
           !ReadsTheProjectJournal(name, args);
}

McpToolClass ToolClassOf(const std::string& name);

// MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. The tools whose call may carry a
// `motif`: those that write an entry into a timeline's journal, undo and
// redo aside, which move entries and their motifs rather than write one.
// MOTIF6 -- and those that write one into the project's journal: a
// timeline created, cloned, declined, renamed, removed, made active or
// chosen.
bool TakesMotif(const std::string& name) {
    return (IsTimelineEditingTool(name) && name != "undo" && name != "redo") ||
           name == "apply_edit_plan" || name == "end_intervention" ||
           ToolClassOf(name) == McpToolClass::ProjectEdit;
}

// MOTIF6 -- DEC41. A choice between variants is recorded for what it says
// of the person's taste; without its reason, or without saying who chose,
// it would say nothing, or say it of the wrong one -- an agent's pick read
// later as the person's preference.
bool RequiresMotifWithAuthor(const std::string& name) {
    return name == "choose_timeline_variant";
}

// MOTIF2 -- a motif's marker named as every ID is named: an alias, a full
// ID or an unambiguous prefix, resolved against the timeline it edits.
bool ResolveMotifMarker(std::optional<EditMotif>& motif,
                        const IdResolver& resolver, const std::string& path,
                        std::string& message) {
    if (!motif || motif->marker_id.empty()) return true;
    Ulid marker;
    if (!resolver.Resolve(path + ".motif.marker_id", motif->marker_id, marker,
                          message)) {
        return false;
    }
    motif->marker_id = marker;
    return true;
}

bool DispatchUndo(McpBackend& backend, const IdResolver&, const Value& args,
                  std::string& resultJson, std::string& errorName,
                  std::string& message) {
    if (!CheckKnownKeys(args, {}, "undo", message))
        return Fail(errorName, message, message);
    return backend.Undo(resultJson, errorName, message);
}

bool DispatchRedo(McpBackend& backend, const IdResolver&, const Value& args,
                  std::string& resultJson, std::string& errorName,
                  std::string& message) {
    if (!CheckKnownKeys(args, {}, "redo", message))
        return Fail(errorName, message, message);
    return backend.Redo(resultJson, errorName, message);
}

// PL1 -- ROADMAP.md. A client whose intervention spans several tool calls
// -- Claude Code through MCP, a script through --call-tool -- names its
// start and its end; everything applied in between becomes one undo step.
// A pair rather than a batch tool: the agent keeps applying edits one call
// at a time and sees each result, which a list prepared in advance cannot
// give it, and preparing a list is what PL2 and PL3 are for. The token
// carries the start, so nothing is held between calls and an intervention
// never ended simply stays as separate steps.
bool DispatchBeginIntervention(McpBackend& backend, const IdResolver&,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {"timeline_id"}, "begin_intervention", message)) {
        return Fail(errorName, message, message);
    }
    return backend.BeginIntervention(resultJson, errorName, message);
}

bool DispatchEndIntervention(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {"intervention", "label"}, "end_intervention",
                        message)) {
        return Fail(errorName, message, message);
    }
    std::string intervention;
    std::string label;
    if (!ReadString(args, "intervention", "end_intervention", true, "",
                    intervention, message) ||
        !ReadString(args, "label", "end_intervention", true, "", label,
                    message)) {
        return Fail(errorName, message, message);
    }
    if (label.empty()) {
        return Fail(errorName, message,
                    "'end_intervention.label' must not be empty");
    }
    return backend.EndIntervention(intervention, label, resultJson, errorName,
                                   message);
}

// ---------------------------------------------------------------------
// PL2/PL3 -- ROADMAP.md (PLAN-2026-09). Edit plans: prepared on a copy,
// then applied or discarded. The mode where the agent proposes and whoever
// drives decides, beside the direct mode every other tool here serves.
// ---------------------------------------------------------------------

// Why a tool cannot be a step of a plan, or nullptr when it can. Every tool
// that edits one timeline can, except those the copy cannot run.
const char* PlanStepRefusal(const std::string& name) {
    if (name == "undo" || name == "redo") {
        return "moves a history the plan's copy does not have";
    }
    if (name == "create_interview_short") {
        return "creates a timeline, which is a project edit; a plan edits "
               "one timeline";
    }
    // R6 -- half of it writes a .cube into the project package, which is a
    // project edit and not a timeline one, and the plan's copy has no
    // package to write into. Laying the look is one call, before or after
    // the plan.
    if (name == "set_sequence_lut") {
        return "copies a LUT into the project package, which a plan's copy "
               "does not have; set the sequence's look outside the plan";
    }
    // GN6b -- they ingest what they made into the library, which the
    // plan's copy cannot do (PlanBackend has no IngestMedia): a
    // generate_media step was spent on fal.ai before it failed there.
    if (name == "generate_media" || name == "design_voice" ||
        name == "generate_voice_over") {
        return "ingests a generated media into the library, which a plan's "
               "copy cannot; generate it before or after the plan";
    }
    if (!IsTimelineEditingTool(name)) {
        return "does not edit a timeline";
    }
    return nullptr;
}

const char kTimelineReadInPlan[] =
    "a step of a plan cannot read the timeline through the session: it "
    "would read the timeline before the plan, not the copy the steps edit";

// PL2 -- the backend a plan's steps are dispatched to. The ordinary tool
// code runs unchanged; only what it reads and what it applies are the
// copy's. Rush-level caches (transcripts, envelopes, picture quality) belong
// to a source, not to the timeline, so the copy reads them from the session.
// Word projection uses the draft and the session's caches (OR3). Other
// timeline-level reads, history, project edits and describe are refused by
// name rather than answered about the wrong document.
class PlanBackend final : public McpBackend {
public:
    PlanBackend(McpBackend& session, EditPlanDraft& draft)
        : session_(session), draft_(draft) {}

    // SN4 -- place_sound_stem reads its stem's header through the library's
    // path, relative to the package: a file of the project, like a rush.
    std::string PackageDirectory() const override {
        return session_.PackageDirectory();
    }

    bool SnapshotDocument(Document& document, std::string&) override {
        document = draft_.Current();
        return true;
    }

    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override {
        const Document before = draft_.Current();
        EditError error = EditError::None;
        // MOTIF2 -- the step's motif, or its rule's, rides on its members.
        std::vector<EntryMotif> motifs = EntryMotifsForCall(operation);
        if (!draft_.Apply(std::move(operation), error, message,
                          std::move(motifs))) {
            errorName = EditErrorName(error);
            return false;
        }
        DocumentDelta delta;
        ComputeDocumentDelta(before, draft_.Current(), delta);
        resultJson = "{\"ok\":true,\"doc_hash\":\"" +
                     TimelineRevision(draft_.Current()) +
                     "\",\"delta\":" + SerializeDocumentDelta(delta) + "}";
        return true;
    }

    bool ReadSourceTranscript(const Ulid& sourceId, Transcript& transcript,
                              std::string& message) override {
        return session_.ReadSourceTranscript(sourceId, transcript, message);
    }
    bool ReadMediaFileColor(const Ulid& mediaId, MediaFileColor& color,
                            std::string& message) override {
        return session_.ReadMediaFileColor(mediaId, color, message);
    }
    bool ReadSourceSpeechOnset(const Ulid& sourceId, SpeechOnsetReport& report,
                               std::string& message) override {
        return session_.ReadSourceSpeechOnset(sourceId, report, message);
    }
    ShotQualityCacheRead ReadSourceShotQuality(
        const Ulid& sourceId, ShotQualityReport& report) override {
        return session_.ReadSourceShotQuality(sourceId, report);
    }
    bool ReadSourceHesitations(const Ulid& sourceId, HesitationReport& report,
                               std::string& message) override {
        return session_.ReadSourceHesitations(sourceId, report, message);
    }
    bool ReadSourceAudioBeats(const Ulid& sourceId, BeatDetectionResult& beats,
                              std::string& message) override {
        return session_.ReadSourceAudioBeats(sourceId, beats, message);
    }
    bool ReadSourceSpeakers(const Ulid& sourceId, SpeakerDiarization& speakers,
                            std::string& message) override {
        return session_.ReadSourceSpeakers(sourceId, speakers, message);
    }

    bool ReadTimelineTranscript(std::string&, std::string& message) override {
        message = kTimelineReadInPlan;
        return false;
    }
    bool ReadTimelineWordSpans(std::vector<TimelineTranscriptSpan>& spans,
                               TimelineWordCheck& check,
                               std::string& message) override {
        return session_.ReadDocumentWordSpans(draft_.Current(), spans, check,
                                              message);
    }
    bool ApplyProjectEdit(ProjectOperation, std::string&,
                          std::string& errorName,
                          std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "a plan edits one timeline; project edits are not planned";
        return false;
    }
    bool Undo(std::string&, std::string& errorName,
              std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "a plan's copy has no history to undo";
        return false;
    }
    bool Redo(std::string& resultJson, std::string& errorName,
              std::string& message) override {
        return Undo(resultJson, errorName, message);
    }
    bool Describe(std::string&, std::string& message) override {
        message = "describe is not a step of a plan";
        return false;
    }

private:
    McpBackend& session_;
    EditPlanDraft& draft_;
};

// A step's own report, for the preview: what the tool answered, minus what
// the plan states once for all its steps (its delta and revision) and the
// canonical operation a range edit echoes, which the plan keeps itself.
Value StepReport(size_t index, const std::string& tool,
                 const std::string& resultJson) {
    Value report = Value::MakeObject();
    report.Set("step", Value::MakeInt(static_cast<int64_t>(index) + 1));
    report.Set("tool", Value::MakeString(tool));
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject()) {
        return report;
    }
    for (const auto& [key, value] : result.AsObject()) {
        if (key == "ok" || key == "delta" || key == "doc_hash" ||
            key == "operation") {
            continue;
        }
        report.Set(key, value);
    }
    return report;
}

// PL2 -- aliases resolve against one observation. Every step resolves its
// IDs and aliases with the resolver built on the timeline as it stood when
// the plan began: "remove A1, then A2" removes the two clips the caller
// saw, where re-observing between steps would have slid A2 onto the old A3.
// Geometry is still read from the copy, so each step edits what the steps
// before it left.
bool DispatchPrepareEditPlan(McpBackend& backend,
                             const std::vector<McpTool>& tools,
                             const std::vector<McpDispatchFn>& dispatch,
                             EditPlanStore& store, const Value& args,
                             std::string& resultJson, std::string& errorName,
                             std::string& message) {
    const std::string tool = "prepare_edit_plan";
    std::string label;
    if (!CheckKnownKeys(args, {"timeline_id", "label", "steps"}, tool,
                        message) ||
        !ReadString(args, "label", tool, true, "", label, message)) {
        return Fail(errorName, message, message);
    }
    const Value* steps = args.Find("steps");
    if (steps == nullptr || !steps->IsArray() || steps->AsArray().empty() ||
        steps->AsArray().size() > kMaxBatchOperations) {
        return Fail(errorName, message,
                    "'" + tool + ".steps' must hold 1 to " +
                        std::to_string(kMaxBatchOperations) + " steps");
    }
    Document base;
    if (!backend.SnapshotDocument(base, message)) {
        errorName = "IoError";
        return false;
    }
    // PRD17 -- ROADMAP.md. Refused before the steps run, at the store's
    // bound, which is per timeline: the agents of a series each keep theirs.
    const std::vector<Ulid> kept = store.PlanIdsForTimeline(base.sequence.id);
    if (kept.size() >= EditPlanStore::kMaxPlans) {
        std::string open;
        for (const Ulid& id : kept) open += (open.empty() ? "" : ", ") + id;
        errorName = kEditPlanLimitError;
        message = "this session already keeps " + std::to_string(kept.size()) +
                  " edit plans for timeline " + base.sequence.id +
                  " (at most " + std::to_string(EditPlanStore::kMaxPlans) +
                  " per timeline); apply or discard one first: " + open;
        return false;
    }
    const IdResolver observed(base);
    EditPlanDraft draft(base);
    PlanBackend copy(backend, draft);
    Value reports = Value::MakeArray();
    for (size_t index = 0; index < steps->AsArray().size(); ++index) {
        const Value& step = steps->AsArray()[index];
        const std::string path = tool + ".steps[" + std::to_string(index) + "]";
        std::string name;
        if (!CheckKnownKeys(step, {"tool", "arguments"}, path, message) ||
            !ReadString(step, "tool", path, true, "", name, message)) {
            return Fail(errorName, message, message);
        }
        const auto found = std::find_if(
            tools.begin(), tools.end(),
            [&](const McpTool& item) { return item.name == name; });
        if (found == tools.end()) {
            errorName = "UnknownTool";
            message = "step " + std::to_string(index + 1) + ": no such tool '" +
                      name + "'";
            return false;
        }
        if (const char* refusal = PlanStepRefusal(name)) {
            errorName = "UnsupportedOperation";
            message = "step " + std::to_string(index + 1) + ": '" + name +
                      "' cannot be a step of a plan: it " + refusal;
            return false;
        }
        const Value* given = step.Find("arguments");
        if (given != nullptr && !given->IsObject()) {
            return Fail(errorName, message,
                        "'" + path + ".arguments' must be a JSON object");
        }
        Value stepArguments = Value::MakeObject();
        // MOTIF2 -- a step says why like a call does.
        std::optional<EditMotif> stepMotif;
        if (given != nullptr) {
            for (const auto& [key, value] : given->AsObject()) {
                if (key == kMcpMotifKey) {
                    EditMotif motif;
                    if (!ParseMotifArgument(value, path + ".arguments.motif",
                                            motif, message)) {
                        return Fail(errorName, message, message);
                    }
                    stepMotif = std::move(motif);
                    if (!ResolveMotifMarker(stepMotif, observed,
                                            path + ".arguments", message)) {
                        return Fail(errorName, message, message);
                    }
                    continue;
                }
                if (key == kMcpAsyncKey || key == kMcpIdempotencyKey ||
                    key == kMcpExpectedRevisionKey) {
                    std::string refusal = "'" + path;
                    refusal += ".arguments." + key +
                               "': a step is not a call of its own; give it "
                               "to prepare_edit_plan itself";
                    return Fail(errorName, message, refusal);
                }
                if (key == "timeline_id") {
                    if (!value.IsString() ||
                        value.AsString() != base.sequence.id) {
                        return Fail(errorName, message,
                                    "'" + path +
                                        ".arguments.timeline_id' must be the "
                                        "plan's own timeline, '" +
                                        base.sequence.id + "'");
                    }
                    continue;
                }
                stepArguments.Set(key, value);
            }
        }
        std::string stepResult;
        const size_t toolIndex =
            static_cast<size_t>(std::distance(tools.begin(), found));
        copy.SetEditMotifs({stepMotif, {}});
        const bool stepApplied = dispatch[toolIndex](
            copy, observed, stepArguments, stepResult, errorName, message);
        copy.SetEditMotifs({});
        if (!stepApplied) {
            std::string context = "step " + std::to_string(index + 1);
            context += " (" + name + "): ";
            context += message;
            message = context + "; no plan was kept";
            Value failed = Value::MakeObject();
            failed.Set("failed_step",
                       Value::MakeInt(static_cast<int64_t>(index) + 1));
            failed.Set("tool", Value::MakeString(name));
            resultJson = failed.Dump();
            return false;
        }
        reports.Push(StepReport(index, name, stepResult));
    }

    EditPlan plan;
    if (!FinishEditPlan(draft, label, reports, plan, errorName, message)) {
        return false;
    }
    std::string answer = EditPlanAnswer(plan, "prepared");
    if (!store.Add(std::move(plan), errorName, message)) {
        return false;
    }
    resultJson = std::move(answer);
    return true;
}

bool ReadPlanId(const Value& args, const std::string& tool, Ulid& planId,
                std::string& message) {
    return CheckKnownKeys(args, {"plan_id"}, tool, message) &&
           ReadString(args, "plan_id", tool, true, "", planId, message);
}

bool RefuseUnknownPlan(const Ulid& planId, std::string& errorName,
                       std::string& message) {
    errorName = kUnknownPlanError;
    message = "no open edit plan '" + planId +
              "' in this session: it was applied or discarded, or prepared "
              "in another session";
    return false;
}

// PL3 -- whoever drives decides. Over MCP the client applies its own plans:
// this mode asks nothing of an autonomous agent that the direct mode does
// not. What it guarantees is what is applied: the plan's own members, to the
// timeline they were prepared on, at the revision they were prepared on.
// MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. Why the timeline is as it is:
// its journal's entries in effect, grouped by motif.
// PA9 -- docs/roadmap/PARITE-2026-11.md. What the timeline became since a
// state the caller holds (ReadTimelineChanges, EditLog.h).
bool DispatchListChanges(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string tool = "list_changes";
    std::string since;
    // `timeline_id` is read by McpToolRegistry::Call, like every read's.
    if (!CheckKnownKeys(args, {"since", "timeline_id"}, tool, message) ||
        !ReadString(args, "since", tool, false, "", since, message)) {
        return Fail(errorName, message, message);
    }
    // Left out: the state the timeline is in, to give next time.
    const bool hashed =
        since.empty() || (since.size() == 16 &&
                          std::all_of(since.begin(), since.end(), [](char c) {
                              return (c >= '0' && c <= '9') ||
                                     (c >= 'a' && c <= 'f');
                          }));
    if (!hashed) {
        return Fail(errorName, message,
                    "'list_changes.since' is a doc_hash, as an edit result "
                    "or an earlier list_changes names one: 16 hexadecimal "
                    "digits");
    }
    return backend.ReadTimelineChangesJson(since, resultJson, errorName,
                                           message);
}

bool DispatchListMotifs(McpBackend& backend, const IdResolver&,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    const std::string tool = "list_motifs";
    std::string reasonName;
    std::string rule;
    std::string journal;
    if (!CheckKnownKeys(args, {"reason", "rule", "journal"}, tool, message) ||
        !ReadString(args, "reason", tool, false, "", reasonName, message) ||
        !ReadString(args, "rule", tool, false, "", rule, message) ||
        !ReadString(args, "journal", tool, false, "timeline", journal,
                    message)) {
        return Fail(errorName, message, message);
    }
    if (journal != "timeline" && journal != "project") {
        return Fail(errorName, message,
                    "'list_motifs.journal' must be timeline or project");
    }
    // MOTIF6 -- the project's journal belongs to no timeline: one named
    // with it would be a question this answer does not address.
    if (journal == "project" && args.Find("timeline_id") != nullptr) {
        return Fail(errorName, message,
                    "'list_motifs.timeline_id' names a timeline's journal; "
                    "journal: project reads the project's, which has none");
    }
    std::optional<MotifReason> reason;
    if (!reasonName.empty()) {
        MotifReason parsed = MotifReason::Unstated;
        if (!ParseMotifReason(reasonName, parsed)) {
            return Fail(errorName, message,
                        "'list_motifs.reason' is not a motif reason");
        }
        reason = parsed;
    }
    if (journal == "project") {
        return backend.ReadProjectMotifs(reason, rule, resultJson, errorName,
                                         message);
    }
    return backend.ReadTimelineMotifs(reason, rule, resultJson, errorName,
                                      message);
}

bool DispatchApplyEditPlan(McpBackend& backend, EditPlanStore& store,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    Ulid planId;
    bool verbose = false;
    if (!CheckKnownKeys(args, {"plan_id", "verbose"}, "apply_edit_plan",
                        message) ||
        !ReadString(args, "plan_id", "apply_edit_plan", true, "", planId,
                    message) ||
        !ReadBool(args, "verbose", "apply_edit_plan", false, verbose,
                  message)) {
        return Fail(errorName, message, message);
    }
    EditPlan plan;
    if (!store.Find(planId, plan)) {
        return RefuseUnknownPlan(planId, errorName, message);
    }
    if (!backend.SelectTimelineForEdit(plan.timeline_id, errorName, message)) {
        return false;
    }
    struct TimelineScope {
        McpBackend& backend;
        ~TimelineScope() { backend.EndTimelineEdit(); }
    };
    const TimelineScope scope{backend};

    Document current;
    if (!backend.SnapshotDocument(current, message)) {
        errorName = "IoError";
        return false;
    }
    std::string refusal;
    if (!CheckEditPlanApplicable(plan, current, refusal, errorName, message)) {
        resultJson = refusal;
        return false;
    }
    // One check-and-apply, so a gesture landing between the check above and
    // the write is refused rather than overwritten (the app's adapter runs
    // both on its main thread).
    // MOTIF2 -- the steps' motifs on their members, the call's on the
    // rest.
    struct MotifMembers {
        McpBackend& backend;
        McpBackend::EditMotifs saved;
        ~MotifMembers() { backend.SetEditMotifs(saved); }
    };
    const MotifMembers restore{backend, backend.CurrentEditMotifs()};
    McpBackend::EditMotifs motifs = restore.saved;
    motifs.members = plan.motifs;
    backend.SetEditMotifs(motifs);
    std::string applied;
    if (!backend.ApplyPreparedOperation(plan.batch, current.SaveToString(),
                                        applied, errorName, message)) {
        if (errorName == "StaleDocument") {
            errorName = kStaleRevisionError;
            message = "the timeline changed while plan '" + plan.plan_id +
                      "' was being applied; nothing was applied: prepare it "
                      "again on the current timeline";
        }
        return false;
    }
    store.Remove(plan.plan_id);

    Value answer;
    std::string parseError;
    Value::Parse(EditPlanAnswer(plan, "applied"), answer, parseError);
    Document after;
    std::string ignored;
    if (backend.SnapshotDocument(after, ignored)) {
        answer.Set("doc_hash", Value::MakeString(TimelineRevision(after)));
    }
    if (!verbose) {
        // AX-2026-09 -- the plan applied is the plan previewed: the same
        // members on the same revision, or it was refused above. Its delta
        // and step reports are the ones prepare_edit_plan already gave, and
        // repeating them cost as much again as the preview itself (a
        // six-step plan: 41 KB to prepare, 43 KB to apply).
        Value compact = Value::MakeObject();
        for (const char* key :
             {"ok", "status", "plan_id", "timeline_id", "timeline_name",
              "label", "operation_count", "base_doc_hash", "result_doc_hash",
              "doc_hash", "duration_after"}) {
            if (const Value* value = answer.Find(key)) compact.Set(key, *value);
        }
        compact.Set("delta_as_prepared", Value::MakeBool(true));
        resultJson = compact.Dump();
        return true;
    }
    Value appliedResult;
    if (Value::Parse(applied, appliedResult, parseError) &&
        appliedResult.IsObject()) {
        for (const char* key : {"delta", "cut_words"}) {
            if (const Value* value = appliedResult.Find(key)) {
                answer.Set(key, *value);
            }
        }
    }
    resultJson = answer.Dump();
    return true;
}

bool DispatchDiscardEditPlan(EditPlanStore& store, const Value& args,
                             std::string& resultJson, std::string& errorName,
                             std::string& message) {
    Ulid planId;
    if (!ReadPlanId(args, "discard_edit_plan", planId, message)) {
        return Fail(errorName, message, message);
    }
    EditPlan plan;
    if (!store.Find(planId, plan) || !store.Remove(planId)) {
        return RefuseUnknownPlan(planId, errorName, message);
    }
    Value answer = Value::MakeObject();
    answer.Set("ok", Value::MakeBool(true));
    answer.Set("status", Value::MakeString("discarded"));
    answer.Set("plan_id", Value::MakeString(plan.plan_id));
    answer.Set("timeline_id", Value::MakeString(plan.timeline_id));
    answer.Set("label", Value::MakeString(plan.label));
    resultJson = answer.Dump();
    return true;
}

// IA5 -- ROADMAP.md. get_task, cancel_task and list_tasks are answered by the
// server's session (McpSession.h) before the catalog is reached. Reaching
// this means there is no server: `--call-tool` or the chat panel, where every
// tool runs to its end within the call and no task outlives it.
bool DispatchTaskWithoutServer(McpBackend&, const IdResolver&, const Value&,
                               std::string&, std::string& errorName,
                               std::string& message) {
    errorName = "UnsupportedOperation";
    message =
        "background tasks live in a running MCP server (--mcp-serve); "
        "here every tool runs to its end within the call";
    return false;
}

// Inserts one property at the head of a schema built by SchemaBuilder.
void InjectSchemaProperty(std::string& schema, const std::string& name,
                          const std::string& propertySchema) {
    const std::string marker = "\"properties\":{";
    const size_t position = schema.find(marker);
    if (position == std::string::npos) {
        return;
    }
    const size_t insertion = position + marker.size();
    const bool hasProperties =
        insertion < schema.size() && schema[insertion] != '}';
    schema.insert(insertion, "\"" + name + "\":" + propertySchema +
                                 (hasProperties ? "," : ""));
}

// PRD15 -- ROADMAP.md. Every schema builder of the catalog opens an
// inputSchema with a description of its own, "<tool> arguments" or "<tool>
// takes no arguments": 7 KB that told an agent nothing the tool's
// description does not. Removed at registration, only in those two
// spellings, so a root description that says something stays.
std::string WithoutBoilerplateRootDescription(const std::string& tool,
                                              const std::string& schema) {
    const std::string prefix = "{\"type\":\"object\",";
    for (const std::string& boilerplate :
         {tool + " arguments", tool + " takes no arguments"}) {
        const std::string member =
            "\"description\":\"" + Esc(boilerplate) + "\",";
        if (schema.compare(0, prefix.size(), prefix) == 0 &&
            schema.compare(prefix.size(), member.size(), member) == 0) {
            return prefix + schema.substr(prefix.size() + member.size());
        }
    }
    return schema;
}

// PRD15 -- ROADMAP.md. The one place a tool's class is declared, by name,
// from what its dispatcher was read to do (the backend calls it makes, the
// files it writes, the program it drives). A tool missing here stays
// Unclassified and tests/mcp_tools_tests.cc fails; so does a name here that
// no tool has. The export tools are declared here without their code being
// touched.
const std::vector<std::pair<std::string, McpToolClass>>& ToolClassTable() {
    using C = McpToolClass;
    static const std::vector<std::pair<std::string, McpToolClass>> kTable = {
        // Reads. The pictures and measures render through temporary files
        // the call removes; nothing is left behind.
        {"describe", C::Read},
        {"list_motifs", C::Read},
        {"list_changes", C::Read},
        {"get_timeline_transcript", C::Read},
        {"list_speech_blocks", C::Read},
        {"inspect_speech_selection", C::Read},
        {"list_source_words", C::Read},
        // PA3 -- docs/roadmap/PARITE-2026-11.md.
        {"list_speakers", C::Read},
        {"list_timeline_words", C::Read},
        {"list_disfluencies", C::Read},
        {"locate_source_frame", C::Read},
        {"list_shot_quality", C::Read},
        {"timeline_stats", C::Read},
        {"check_timeline", C::Read},
        {"contact_sheet", C::Read},
        {"cut_sheet", C::Read},
        {"media_sheet", C::Read},
        {"inspect_cut", C::Read},
        {"review_cuts", C::Read},
        {"read_frame", C::Read},
        {"measure_color", C::Read},
        {"match_color", C::Read},
        {"propose_exposure", C::Read},
        {"list_speech_onsets", C::Read},
        {"list_audio_beats", C::Read},
        {"list_tempo_grid", C::Read},
        {"list_music_structure", C::Read},
        {"list_clip_tempo_beats", C::Read},
        {"list_stable_windows", C::Read},
        // PL3 -- docs/roadmap/PLANS-2026-10.md.
        {"search_shots", C::Read},
        {"list_isolated_frames", C::Read},
        {"analyze_timeline_audio", C::Read},
        {"measure_audio_offset", C::Read},
        // FC6 -- decodes one rush and writes nothing.
        {"propose_ducking", C::Read},
        {"check_caption_legibility", C::Read},
        {"verify_export", C::Read},
        {"get_editor_view", C::Read},
        {"get_video_output", C::Read},
        // A local preference is persisted, outside the project's journal.
        {"set_video_output", C::Unjournaled},
        // Draws the window into memory; no file is written.
        {"capture_editor_window", C::Read},
        {"list_timelines", C::Read},
        {"list_multicam_groups", C::Read},
        {"list_clip_framing", C::Read},
        // Hands out a token and writes nothing (McpBackend.h).
        {"begin_intervention", C::Read},
        {"get_task", C::Read},
        {"list_tasks", C::Read},
        {"list_motion_compositions", C::Read},
        {"describe_motion_composition", C::Read},
        {"list_motion_templates", C::Read},
        {"list_fonts", C::Read},
        // PRD18 -- the procedures compiled into the binary (Procedures.h).
        {"get_procedure", C::Read},
        {"query_project_archive", C::Read},
        {"project_archive_status", C::Read},
        {"get_run_step", C::Read},
        {"get_edit_plan", C::Read},
        {"commit_run_step", C::JournaledEdit},
        {"render_motion_frame", C::Read},
        {"check_motion_safe_area", C::Read},
        // What this server holds: the open project and its lock, the plans
        // it keeps, the tasks it runs.
        {"open_project", C::Session},
        {"prepare_edit_plan", C::Session},
        {"discard_edit_plan", C::Session},
        {"cancel_task", C::Session},
        // Caches beside the project (kCacheWritingAnalyses).
        {"transcribe_media", C::Analysis},
        {"transcribe_timeline", C::Analysis},
        {"analyze_speech_onset", C::Analysis},
        {"analyze_shot_quality", C::Analysis},
        {"analyze_audio_beats", C::Analysis},
        {"analyze_hesitations", C::Analysis},
        {"analyze_isolated_frames", C::Analysis},
        {"align_transcript", C::Analysis},
        {"align_words", C::Analysis},
        {"analyze_speakers", C::Analysis},
        // The timeline's journal: ApplyOperation, ApplyPreparedOperation,
        // undo, redo, the regrouping of an intervention, and a Resolve
        // timeline read in as operations appended to that journal.
        {"insert_clip", C::JournaledEdit},
        {"edit_source_range", C::JournaledEdit},
        {"delete_timeline_range", C::JournaledEdit},
        {"remove_clip", C::JournaledEdit},
        {"clear_clip", C::JournaledEdit},
        {"clear_clips", C::JournaledEdit},
        {"trim_clip", C::JournaledEdit},
        {"move_clip", C::JournaledEdit},
        {"move_clips", C::JournaledEdit},
        {"split_clip", C::JournaledEdit},
        {"split_at_interval", C::JournaledEdit},
        {"delete_gap", C::JournaledEdit},
        {"detach_audio", C::JournaledEdit},
        {"move_linked_clips", C::JournaledEdit},
        {"trim_linked_clips", C::JournaledEdit},
        {"shorten_linked_clip", C::JournaledEdit},
        {"ripple_trim", C::JournaledEdit},
        {"roll_edit", C::JournaledEdit},
        {"slip_edit", C::JournaledEdit},
        {"slide_edit", C::JournaledEdit},
        {"rate_stretch", C::JournaledEdit},
        {"remove_linked_clips", C::JournaledEdit},
        {"clear_linked_clips", C::JournaledEdit},
        {"split_linked_clips", C::JournaledEdit},
        {"paste_clips", C::JournaledEdit},
        {"add_track", C::JournaledEdit},
        {"import_srt", C::JournaledEdit},
        {"import_edl_markers", C::JournaledEdit},
        {"remove_track", C::JournaledEdit},
        {"set_track_lock", C::JournaledEdit},
        {"set_track_sync_lock", C::JournaledEdit},
        {"set_track_output", C::JournaledEdit},
        {"update_sequence", C::JournaledEdit},
        {"set_color_management", C::JournaledEdit},
        {"add_bin", C::JournaledEdit},
        {"remove_bin", C::JournaledEdit},
        {"rename_bin", C::JournaledEdit},
        {"move_bin", C::JournaledEdit},
        {"set_media_bin", C::JournaledEdit},
        // DEC32 (PRD7) -- an ingest is one IngestMediaOperation.
        {"ingest_media", C::JournaledEdit},
        {"add_marker", C::JournaledEdit},
        {"remove_marker", C::JournaledEdit},
        {"update_marker", C::JournaledEdit},
        {"add_transition", C::JournaledEdit},
        {"remove_transition", C::JournaledEdit},
        {"update_transition", C::JournaledEdit},
        {"set_clip_link", C::JournaledEdit},
        {"split_link_groups", C::JournaledEdit},
        {"sync_audio", C::JournaledEdit},
        {"place_sound_stem", C::JournaledEdit},
        {"render_sound", C::FileWrite},
        {"set_clip_effects", C::JournaledEdit},
        {"paste_clip_grade", C::JournaledEdit},
        {"set_clip_opacity", C::JournaledEdit},
        {"set_clip_blur", C::JournaledEdit},
        {"set_clip_mask", C::JournaledEdit},
        {"set_clip_speed", C::JournaledEdit},
        {"set_clip_framing", C::JournaledEdit},
        {"set_clip_animation", C::JournaledEdit},
        {"set_clip_enabled", C::JournaledEdit},
        // CA3 -- reads faces, then lays R1 and R2's operations in one batch.
        {"track_subject_framing", C::JournaledEdit},
        // PA7 -- reads the rush, then lays the same two operations.
        {"stabilize_clip", C::JournaledEdit},
        {"set_clip_audio", C::JournaledEdit},
        {"add_caption_style", C::JournaledEdit},
        {"generate_captions", C::JournaledEdit},
        {"translate_captions", C::JournaledEdit},
        // RM3 -- writes model files in the engine's data folder and, when
        // installed, changes how generate_captions cuts: nothing undoes it.
        {"train_caption_breaks", C::Unjournaled},
        {"update_caption_style", C::JournaledEdit},
        {"remove_caption_style", C::JournaledEdit},
        {"set_clip_caption", C::JournaledEdit},
        {"add_multicam_group", C::JournaledEdit},
        {"set_multicam_active_angle", C::JournaledEdit},
        {"split_multicam_group", C::JournaledEdit},
        {"remove_multicam_group", C::JournaledEdit},
        {"clean_disfluencies", C::JournaledEdit},
        {"tighten_pauses", C::JournaledEdit},
        {"trim_boundary_air", C::JournaledEdit},
        {"close_junction_air", C::JournaledEdit},
        {"remove_words", C::JournaledEdit},
        {"remove_source_ranges", C::JournaledEdit},
        {"set_media_white_balance", C::JournaledEdit},
        {"apply_cdl", C::JournaledEdit},
        {"set_media_color_input", C::JournaledEdit},
        // PL2 -- docs/roadmap/PLANS-2026-10.md.
        {"describe_shots", C::JournaledEdit},
        // A read by default; apply:true sets each proposal as its own
        // operation (DispatchProposeGrade).
        {"propose_grade", C::JournaledEdit},
        {"conform_sequence", C::JournaledEdit},
        {"conform_color_management", C::JournaledEdit},
        {"undo", C::JournaledEdit},
        {"redo", C::JournaledEdit},
        {"end_intervention", C::JournaledEdit},
        {"apply_edit_plan", C::JournaledEdit},
        {"add_motion_composition", C::JournaledEdit},
        {"add_motion_template", C::JournaledEdit},
        {"remove_motion_composition", C::JournaledEdit},
        {"edit_motion_composition", C::JournaledEdit},
        {"humanize_motion_composition", C::JournaledEdit},
        // MB7 -- reads After, adds the composition as add_motion_composition.
        {"import_after_comp", C::JournaledEdit},
        {"insert_motion_clip", C::JournaledEdit},
        {"place_motion_clip", C::JournaledEdit},
        {"set_clip_motion_props", C::JournaledEdit},
        {"import_resolve_timeline", C::JournaledEdit},
        // The .cube copied into the package is addressed by its digest;
        // what the timeline gains is the operation.
        {"set_sequence_lut", C::JournaledEdit},
        // The project's journal (ApplyProjectEdit).
        {"add_timeline", C::ProjectEdit},
        {"remove_timeline", C::ProjectEdit},
        {"clone_timeline", C::ProjectEdit},
        // MOTIF6 -- a choice, journaled with its motif.
        {"choose_timeline_variant", C::ProjectEdit},
        {"rename_timeline", C::ProjectEdit},
        {"set_timeline_bin", C::ProjectEdit},
        {"set_active_timeline", C::ProjectEdit},
        {"create_timeline_variants", C::ProjectEdit},
        {"create_interview_short", C::ProjectEdit},
        // A file at the caller's path: export_cdl writes over one in place.
        {"export_timeline", C::FileWrite},
        {"archive_project", C::FileWrite},
        {"sync_project_archive", C::FileWrite},
        {"import_project_archive", C::FileWrite},
        {"backup_project_archive", C::FileWrite},
        {"export_timelines", C::FileWrite},
        {"render_motion_video", C::FileWrite},
        {"export_resolve_xml", C::FileWrite},
        {"export_edl", C::FileWrite},
        {"export_edl_markers", C::FileWrite},
        {"export_otio", C::FileWrite},
        {"export_fcpxml", C::FileWrite},
        {"export_cdl", C::FileWrite},
        // A package created, a piece written to this machine's motion
        // library: no journal holds them.
        {"create_project", C::Unjournaled},
        {"save_motion_template", C::Unjournaled},
        // After Effects jobs that open no undo group and save nothing
        // (tools/after-bridge/scripts).
        {"after_describe_project", C::ExternalRead},
        {"after_list_comps", C::ExternalRead},
        {"after_list_footage", C::ExternalRead},
        {"after_describe_comp", C::ExternalRead},
        // After Effects changed, saved or made to render.
        {"after_create_project", C::External},
        {"after_open_project_copy", C::External},
        {"after_render_frame", C::External},
        {"after_set_text", C::External},
        {"after_set_property", C::External},
        {"after_set_layer_state", C::External},
        {"after_replace_footage", C::External},
        {"after_import_footage", C::External},
        {"after_add_layer", C::External},
        {"after_duplicate_layer", C::External},
        {"after_save_project", C::External},
        {"export_motion_after", C::External},
        // MA2 -- After made to render each frame it checks.
        {"verify_after_export", C::External},
        // PA16 -- a timeline built in the project open in Resolve.
        {"send_to_resolve", C::External},
        // GENERATIF-2026-10 GN2 -- a proposal is only this session's; a
        // generation is paid to fal.ai, then ingested and marked.
        {"propose_generation", C::Session},
        {"generate_media", C::External},
        // GN6b -- a WAV written into the package, then ingested and
        // marked: two operations of the timeline's journal, nothing
        // outside this machine.
        {"design_voice", C::JournaledEdit},
        {"generate_voice_over", C::JournaledEdit},
    };
    return kTable;
}

McpToolClass ToolClassOf(const std::string& name) {
    for (const auto& [toolName, toolClass] : ToolClassTable()) {
        if (toolName == name) return toolClass;
    }
    return McpToolClass::Unclassified;
}

}  // namespace

bool McpToolTargetsOneTimeline(const std::string& name) {
    return TargetsOneTimeline(name);
}

bool McpCallNeedsTimeline(const std::string& name,
                          const mcp_json::Value& arguments) {
    return StrictModeNeedsTimeline(name, arguments);
}

std::string McpTimelineRequiredMessage(McpBackend& backend,
                                       const std::string& tool) {
    std::string message =
        "strict timeline mode: '" + tool +
        "' edits or reads one timeline and must name it with timeline_id, "
        "since the agents sharing this project each work on their own";
    Project project;
    std::string ignored;
    if (backend.SnapshotProject(project, ignored)) {
        message += "; the project's timelines: " +
                   Exporter::DescribeTimelines(project);
    }
    return message;
}

const char* McpToolClassName(McpToolClass toolClass) {
    switch (toolClass) {
        case McpToolClass::Read:
            return "read";
        case McpToolClass::Session:
            return "session";
        case McpToolClass::Analysis:
            return "analysis";
        case McpToolClass::JournaledEdit:
            return "journaled_edit";
        case McpToolClass::ProjectEdit:
            return "project_edit";
        case McpToolClass::FileWrite:
            return "file_write";
        case McpToolClass::Unjournaled:
            return "unjournaled";
        case McpToolClass::ExternalRead:
            return "external_read";
        case McpToolClass::External:
            return "external";
        case McpToolClass::Unclassified:
            break;
    }
    return "unclassified";
}

std::vector<std::string> McpToolClassTableNames() {
    std::vector<std::string> names;
    for (const auto& entry : ToolClassTable()) names.push_back(entry.first);
    return names;
}

// Read-only means nothing else is said: the other hints mean something only
// for a tool that changes its environment. Undo is what makes an edit
// non-destructive, which leaves out the project's journal (the `undo` tool
// does not reach it), files written over, and what no journal keeps; an
// analysis only replaces a cache the engine recomputes from the rushes, and
// a session's plans and tasks are the server's own. Only another program or
// a server is an open world. idempotentHint is never claimed: a journaled
// edit adds a step each time, and an analysis or an export run twice works
// twice -- the server's own instructions say never to start an analysis
// again.
std::string McpToolAnnotationsJson(McpToolClass toolClass) {
    switch (toolClass) {
        case McpToolClass::Read:
            return "{\"readOnlyHint\":true,\"openWorldHint\":false}";
        case McpToolClass::Session:
        case McpToolClass::Analysis:
        case McpToolClass::JournaledEdit:
            return "{\"destructiveHint\":false,\"openWorldHint\":false}";
        case McpToolClass::ProjectEdit:
        case McpToolClass::FileWrite:
        case McpToolClass::Unjournaled:
            return "{\"openWorldHint\":false}";
        case McpToolClass::ExternalRead:
            return "{\"readOnlyHint\":true}";
        case McpToolClass::External:
        case McpToolClass::Unclassified:
            break;
    }
    return "{}";
}

std::string McpToolListingJson(const McpTool& tool) {
    return "{\"name\":\"" + Esc(tool.name) + "\",\"description\":\"" +
           Esc(tool.description) +
           "\",\"inputSchema\":" + tool.input_schema_json +
           ",\"annotations\":" + McpToolAnnotationsJson(tool.tool_class) + "}";
}

std::string McpToolsListingJson(const std::vector<McpTool>& tools) {
    std::string listing = "[";
    for (size_t index = 0; index < tools.size(); ++index) {
        if (index) listing += ",";
        listing += McpToolListingJson(tools[index]);
    }
    return listing + "]";
}

McpToolRegistry::McpToolRegistry(const McpToolProfile& profile)
    : profile_(&profile),
      generation_proposals_(
          std::make_shared<generation::GenerationProposalStore>()) {
    // PROFIL-2026-11 PF1 -- the domain of the tools `add` is given, set
    // before each file registers its own (McpToolProfiles.h).
    std::string domain = mcp_domain::kSession;
    const auto add = [&](std::string name, std::string description,
                         std::string schema, McpDispatchFn fn) {
        // PL3 -- a read names its timeline the same way, and never
        // activates it.
        const bool reads = IsTimelineReadingTool(name);
        if (IsTimelineEditingTool(name) || reads) {
            const std::string marker = "\"properties\":{";
            const size_t position = schema.find(marker);
            if (position != std::string::npos) {
                const size_t insertion = position + marker.size();
                const bool hasProperties =
                    insertion < schema.size() && schema[insertion] != '}';
                // PRD15 -- short: on 117 tools. Strict mode, which refuses an
                // edit without one, says so in its own refusal.
                schema.insert(
                    insertion,
                    "\"timeline_id\":" +
                        StringSchema(reads ? "Timeline to read, left "
                                             "inactive; default: the active "
                                             "one."
                                           : "Timeline to edit; default: the "
                                             "active one.") +
                        (hasProperties ? "," : ""));
            }
        }
        // PRD15 -- the root's "<tool> arguments", on all 168 tools, said
        // nothing the tool's own description does not.
        schema = WithoutBoilerplateRootDescription(name, schema);
        // PRD14 -- ROADMAP.md. The one place the catalog's tolerance is
        // installed (McpArguments.h). The rules read from the schema put
        // every call into the tool's own vocabulary before its dispatcher
        // runs, a rush's other name included; the schema no longer says so
        // on each rush argument, since the call reaches the tool the same
        // way under either name. Wrapping the dispatcher rather than Call
        // makes it hold for the steps of a plan too, which reach dispatch_
        // directly.
        McpArgumentRules rules(schema);
        if (!rules.Empty()) {
            fn = [rules, tool = name, dispatch = std::move(fn)](
                     McpBackend& backend, const IdResolver& resolver,
                     const Value& arguments, std::string& resultJson,
                     std::string& errorName, std::string& message) {
                Value normalized;
                if (!rules.Normalize(tool, arguments, normalized, message)) {
                    errorName = "ValidationFailed";
                    return false;
                }
                return dispatch(backend, resolver, normalized, resultJson,
                                errorName, message);
            };
        }
        argument_rules_.push_back(std::move(rules));
        tools_.push_back(
            {std::move(name), std::move(description), std::move(schema)});
        tools_.back().domain = domain;
        dispatch_.push_back(std::move(fn));
    };

    // DECOUPAGE-2026-10 DC2 -- the catalog's tools by domain, one
    // file each (McpToolsInternal.h).
    const auto registerDomain = [&](const char* name, auto registerTools) {
        domain = name;
        registerTools(add);
        domain = mcp_domain::kSession;
    };
    registerDomain(mcp_domain::kEdit, RegisterEditTools);
    registerDomain(mcp_domain::kSequence, RegisterSequenceTools);
    registerDomain(mcp_domain::kClip, RegisterClipTools);
    registerDomain(mcp_domain::kCaption, RegisterCaptionTools);
    registerDomain(mcp_domain::kProject, RegisterProjectTools);
    registerDomain(mcp_domain::kSpeech, RegisterSpeechTools);
    registerDomain(mcp_domain::kPicture, RegisterPictureTools);
    registerDomain(mcp_domain::kColor, RegisterColorTools);
    registerDomain(mcp_domain::kSound, RegisterSoundTools);
    registerDomain(mcp_domain::kDelivery, RegisterDeliveryTools);
    registerDomain(mcp_domain::kAfter, RegisterAfterTools);

    // PRD18 -- the delta of a reversal, said where it applies (it left the
    // server's instructions).
    add("undo",
        "Undo the timeline's most recently applied operation. Returns a delta: "
        "the only way to learn what the reversal put back.",
        SchemaBuilder().Build("undo takes no arguments"), DispatchUndo);

    add("redo",
        "Redo the timeline's most recently undone operation. Returns a delta "
        "of what it applied again.",
        SchemaBuilder().Build("redo takes no arguments"), DispatchRedo);

    add("begin_intervention",
        "Mark the start of an intervention on a timeline, before its first "
        "edit. Every edit applied to that timeline until end_intervention "
        "becomes one undo step, so a person reverts the whole intervention "
        "with a single Undo. Returns an `intervention` token to pass to "
        "end_intervention. Writes nothing: an intervention never ended stays "
        "as separate undo steps.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Timeline the intervention edits; defaults to "
                                "the active timeline unless strict mode is "
                                "enabled."),
                   false)
            .Build("begin_intervention arguments"),
        DispatchBeginIntervention);

    add("end_intervention",
        "Group every edit applied to the intervention's timeline since "
        "begin_intervention into one labelled undo step. The montage is not "
        "changed, only its history. Refused, with the history left as it "
        "was, when that history was undone below the start of the "
        "intervention or rewritten under it, or when it holds more than 256 "
        "operations. Returns `grouped`, the number of steps folded.",
        SchemaBuilder()
            .Field("intervention",
                   StringSchema("The token begin_intervention returned."), true)
            .Field("label",
                   StringSchema("What the intervention did, in a few words, "
                                "for whoever reads the history."),
                   true)
            .Build("end_intervention arguments"),
        DispatchEndIntervention);

    // PL3 -- ROADMAP.md. Edit plans. Engine first (EditPlan.h), then the CLI
    // (--prepare-edit-plan), then these. The store is this catalog's own:
    // one catalog serves one session.
    const std::string planStepSchema =
        SchemaBuilder()
            .Field("tool",
                   StringSchema("A timeline-editing tool of this catalog."),
                   true)
            .Field("arguments",
                   "{\"type\":\"object\",\"description\":\"The tool's own "
                   "arguments, as it takes them directly.\"}",
                   false)
            .Build("One step of the plan");
    add("prepare_edit_plan",
        "Prepare edits on a private copy of one timeline without changing the "
        "project: nothing is saved, journalled or activated until "
        "apply_edit_plan. Each step names a timeline-editing tool of this "
        "catalog and its arguments; the steps run in order on the copy, and "
        "every ID and alias they name resolves once, against the timeline as "
        "it stands now -- removing A1 then A2 removes the two clips you saw, "
        "never the old A3. A step cannot address what an earlier step creates. "
        "The answer is the plan measured: durations, the delta from the "
        "current timeline (its created IDs are the ones applied) and each "
        "step's report. The plan stays in this session until applied or "
        "discarded.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Timeline the plan edits; defaults to the "
                                "active timeline unless strict mode is "
                                "enabled. Preparing never activates it."),
                   false)
            .Field("label",
                   StringSchema("What the plan does, in a few words: the "
                                "history's name for it once applied."),
                   true)
            .Field(
                "steps",
                ArraySchema(planStepSchema, "1 to 256 ordered timeline edits."),
                true)
            .Build("prepare_edit_plan arguments"),
        [this](McpBackend& backend, const IdResolver&, const Value& args,
               std::string& resultJson, std::string& errorName,
               std::string& message) {
            return DispatchPrepareEditPlan(backend, tools_, dispatch_,
                                           edit_plans_, args, resultJson,
                                           errorName, message);
        });

    // OR1, OR2 -- export the exact prepared members before a graph persists
    // its request. The session's plan store may disappear on restart.
    add("get_edit_plan",
        "Read a prepared plan as plan_json, the engine's serialized plan "
        "with its exact operations and generated IDs. Store it before "
        "commit_run_step so a server restart cannot change the proposed edit. "
        "Writes nothing; discard_edit_plan may then release the session copy.",
        SchemaBuilder()
            .Field("plan_id", StringSchema("Prepared plan ULID."), true)
            .Build("get_edit_plan arguments"),
        [this](McpBackend&, const IdResolver&, const Value& args,
               std::string& resultJson, std::string& errorName,
               std::string& message) {
            Ulid id;
            EditPlan plan;
            if (!ReadPlanId(args, "get_edit_plan", id, message)) {
                return Fail(errorName, message, message);
            }
            if (!edit_plans_.Find(id, plan)) {
                return RefuseUnknownPlan(id, errorName, message);
            }
            Value answer = Value::MakeObject();
            answer.Set("ok", Value::MakeBool(true));
            answer.Set("plan_json", Value::MakeString(SerializeEditPlan(plan)));
            resultJson = answer.Dump();
            return true;
        });

    add("commit_run_step",
        "Commit one durable montage step: its serialized edit plan (optional "
        "for an observation), journals, receipt and archive capture in the "
        "same package transaction. request_json is a JSON object encoded as a "
        "string, with full ULIDs run_id, step_id, timeline_id; stage "
        "analysis|selection|assembly|finishing|control|delivery; base_revision "
        "of the whole project; optional plan_json and observations_json "
        "strings. An unavailable SQL archive keeps its capture pending and the "
        "saved receipt usable. Does not activate another timeline.",
        SchemaBuilder()
            .Field("request_json",
                   StringSchema("Serialized RunStepRequest, at most 16 MiB."),
                   true)
            .Build("commit_run_step arguments"),
        [](McpBackend& backend, const IdResolver&, const Value& args,
           std::string& resultJson, std::string& errorName,
           std::string& message) {
            std::string json;
            RunStepRequest request;
            if (!CheckKnownKeys(args, {"request_json"}, "commit_run_step",
                                message) ||
                !ReadString(args, "request_json", "commit_run_step", true, "",
                            json, message) ||
                !ParseRunStepRequest(json, request, message)) {
                return Fail(errorName, message, message);
            }
            return backend.CommitRunStep(request, resultJson, errorName,
                                         message);
        });

    add("get_run_step",
        "Read a saved run step receipt by its full step_id ULID. found:false "
        "means no committed step. matches_current compares the saved project "
        "and exact journals, including the undo cursor, with the receipt; "
        "false means stop and observe the intervening edit. Returns the "
        "receipt_json and current project revision; a missing receipt also "
        "returns history_hashes by timeline and project_history_hash for "
        "preparing a run request. Writes no montage.",
        SchemaBuilder()
            .Field("step_id", StringSchema("Full step ULID."), true)
            .Build("get_run_step arguments"),
        [](McpBackend& backend, const IdResolver&, const Value& args,
           std::string& resultJson, std::string& errorName,
           std::string& message) {
            std::string id;
            if (!CheckKnownKeys(args, {"step_id"}, "get_run_step", message) ||
                !ReadString(args, "step_id", "get_run_step", true, "", id,
                            message)) {
                return Fail(errorName, message, message);
            }
            return backend.ReadRunStep(id, resultJson, errorName, message);
        });

    add("apply_edit_plan",
        "Apply a prepared plan to its timeline as one undo step named by the "
        "plan's label: the same operations and the same created IDs as the "
        "preview. Whoever drives decides when to apply: an MCP client may "
        "apply its own plans. An applied plan is no longer kept. The answer is "
        "short, `delta_as_prepared: true`: the delta and step reports are "
        "exactly the ones prepare_edit_plan returned, so patch your model with "
        "those.",
        SchemaBuilder()
            .Field("plan_id",
                   StringSchema("The plan_id prepare_edit_plan returned."),
                   true)
            .Field("verbose",
                   BoolSchema("true: answer with the whole preview again, "
                              "delta and step reports included, as measured "
                              "while applying; default false."),
                   false)
            .Build("apply_edit_plan arguments"),
        [this](McpBackend& backend, const IdResolver&, const Value& args,
               std::string& resultJson, std::string& errorName,
               std::string& message) {
            return DispatchApplyEditPlan(backend, edit_plans_, args, resultJson,
                                         errorName, message);
        });

    add("discard_edit_plan",
        "Forget a prepared plan. The project is untouched.",
        SchemaBuilder()
            .Field("plan_id",
                   StringSchema("The plan_id prepare_edit_plan returned."),
                   true)
            .Build("discard_edit_plan arguments"),
        [this](McpBackend&, const IdResolver&, const Value& args,
               std::string& resultJson, std::string& errorName,
               std::string& message) {
            return DispatchDiscardEditPlan(edit_plans_, args, resultJson,
                                           errorName, message);
        });

    // IA5 -- ROADMAP.md. Long analyses as tasks the server keeps, and the
    // arguments that make a lost answer safe to ask again.
    add("get_task",
        "Read a background task started with async:true: its state (queued, "
        "running, succeeded, failed, cancelled), progress, and once finished "
        "the tool's own result or refusal. A task survives the connection "
        "that started it: after a disconnect, call this with the task_id "
        "instead of starting the analysis again. `stale: true` means the "
        "inputs it read changed before its result was installed: the result "
        "describes the earlier state.",
        SchemaBuilder()
            .Field("task_id", StringSchema("The task_id async:true returned."),
                   true)
            .Field("wait_seconds",
                   IntSchema("Wait up to this many seconds (0-30, default 0) "
                             "for the task to finish before answering."),
                   false)
            .Build("get_task arguments"),
        DispatchTaskWithoutServer);
    add("cancel_task",
        "Cancel a background task. A queued task never starts; a running one "
        "stops at its next check and nothing it had not installed is "
        "installed. Answers once the task has stopped, or after 10 s with "
        "cancel_requested:true if it has not. A transcription still loading "
        "its model (detail 'Starting') stops once the load ends, 5 to 30 s.",
        SchemaBuilder()
            .Field("task_id", StringSchema("The task_id async:true returned."),
                   true)
            .Build("cancel_task arguments"),
        DispatchTaskWithoutServer);
    add("list_tasks",
        "List this server's background tasks, most recent first, with their "
        "state and progress but without their results.",
        SchemaBuilder().Build("list_tasks takes no arguments"),
        DispatchTaskWithoutServer);

    // MOTION-2026-09 M3 -- compositions (McpMotionTools.cc). Registered
    // before the IA5 marking below, so their edits take idempotency_key and
    // expected_revision like every other timeline edit.
    registerDomain(mcp_domain::kMotion, RegisterMotionTools);
    // M4 -- motion clips (McpMotionClipTools.cc), under the same marking.
    registerDomain(mcp_domain::kMotionClip, RegisterMotionClipTools);
    // GENERATIF-2026-10 GN2 -- paid generation (McpGenerationTools.cc):
    // proposed for free, spent only on a proposal the person accepted.
    registerDomain(mcp_domain::kGeneration, [&](const McpToolAdder& adder) {
        RegisterGenerationTools(adder, *generation_proposals_);
    });
    // GN6b -- the local voice-over (McpVoiceTools.cc): free, on this
    // machine, made only when the person asks (DEC47, DEC48).
    registerDomain(mcp_domain::kVoice, RegisterVoiceTools);

    // MOTIF4 -- docs/roadmap/MOTIF-2026-10.md.
    {
        std::vector<std::string> reasons = MotifReasonNames();
        add("list_motifs",
            "Why the timeline is as it is: its journal's entries in effect, "
            "grouped by motif (reason, rule, params). Each group lists its "
            "entries (rank in the history, author, note, marker) and what "
            "each member touched (clip, source ranges cut, trims); "
            "`operations_without_motif` counts what nobody explained. "
            "Filter with `reason` or `rule` (e.g. rule tighten_pauses). "
            "journal: project reads the project's journal instead: "
            "timelines created, cloned, declined, and the choices between "
            "them (choose_timeline_variant), each told by the timelines it "
            "names. Every edit takes `motif` {reason, note, author, "
            "marker_id}: reason one of the enumeration below (other needs a "
            "note), note one line, author agent (default), person or "
            "procedure, marker_id the marker it answers. tighten_pauses, "
            "clean_disfluencies, trim_boundary_air, close_junction_air, "
            "generate_captions and conform_sequence record their own "
            "reason, rule and params: give them only a note.",
            SchemaBuilder()
                .Field("reason", EnumSchema(reasons, ""), false)
                .Field("rule",
                       StringSchema("The tool whose rule cut, e.g. "
                                    "tighten_pauses."),
                       false)
                .Field("journal",
                       EnumSchema({"timeline", "project"},
                                  "Whose journal: a timeline's (default) or "
                                  "the project's, which takes no "
                                  "timeline_id."),
                       false)
                .Build("list_motifs arguments"),
            DispatchListMotifs);
    }

    // PA9 -- docs/roadmap/PARITE-2026-11.md.
    add("list_changes",
        "What the timeline became since a state you hold: give the "
        "`doc_hash` of the last edit result you saw, and read one delta "
        "from that state to now (clips created, changed, removed, shifted, "
        "as every edit's delta) with the journal's entries in between. The "
        "delta has no word for a marker renamed, a style's settings or a "
        "track locked: `changed` is then true over an empty delta, and the "
        "entries' types say what to read again. Call "
        "it at the start of a turn, before trusting the model of the "
        "timeline you kept: the person may have edited in the window since. "
        "The journal keeps no author, so the entries after your own last "
        "one are someone else's. `undone: true` means the entries were "
        "undone, not applied: the timeline went back. `found: false` means "
        "the journal no longer leads to that state (undone, then other "
        "edits; or further than 500 entries): read the timeline again with "
        "describe. `doc_hash` in the answer is the state to give next time; "
        "without `since`, the answer is that state alone.",
        SchemaBuilder()
            .Field("since",
                   StringSchema("The doc_hash of the state you last saw: 16 "
                                "hexadecimal digits. Left out: only the "
                                "current state is named."),
                   false)
            .Build("list_changes arguments"),
        DispatchListChanges);

    // PRD16 -- ROADMAP.md. The exports too: a series of films is an hour of
    // FFmpeg, and an export task works on the project as it was asked on
    // (McpSession.h).
    static const std::vector<std::string> kBackgroundTools = {
        "transcribe_media",     "transcribe_timeline",
        "analyze_speech_onset", "analyze_shot_quality",
        "analyze_audio_beats",  "analyze_isolated_frames",
        "align_words",          "export_timeline",
        "export_timelines",     "render_motion_video",
        "measure_audio_offset", "propose_ducking",
        "analyze_speakers"};
    static const std::vector<std::string> kCacheWritingAnalyses = {
        "transcribe_media",        "transcribe_timeline",
        "align_transcript",        "align_words",
        "analyze_speech_onset",    "analyze_shot_quality",
        "analyze_audio_beats",     "analyze_hesitations",
        "analyze_isolated_frames", "analyze_speakers"};
    static const std::vector<std::string> kProjectMutations = {
        "create_project",
        "ingest_media",
        "add_timeline",
        "remove_timeline",
        "clone_timeline",
        "rename_timeline",
        "set_timeline_bin",
        "create_timeline_variants",
        "set_active_timeline",
        "end_intervention",
        "import_resolve_timeline",
        "choose_timeline_variant"};
    // PL3 -- a plan call changes what the session keeps, and applying one
    // changes the project: a lost answer is replayed rather than run again,
    // which would keep a second plan or find the applied one gone.
    static const std::vector<std::string> kEditPlanCalls = {
        "prepare_edit_plan", "apply_edit_plan", "discard_edit_plan"};
    // PRD40 -- ROADMAP.md. An export's answer is lost like an edit's, and
    // an hour of FFmpeg is the last thing to run twice: sent again under
    // its key, the call answers its first answer -- for a task, the same
    // task_id (McpSession.h). They take expected_revision with it, as every
    // tool that takes a key does.
    static const std::vector<std::string> kReplayedExports = {
        "export_timeline", "export_timelines"};
    const auto listed = [](const std::vector<std::string>& names,
                           const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    // PRD15 -- ROADMAP.md. These three, with timeline_id, ride on a hundred
    // tools each and weighed 41 KB of the catalog, which the app's agent
    // panel sends with every turn. Their use is told once in the server's
    // instructions (McpServer.cc); each description keeps what a caller without
    // them --
    // `--call-tool`, the panel -- needs to read it alone.
    for (McpTool& tool : tools_) {
        tool.background = listed(kBackgroundTools, tool.name);
        tool.writes_analysis_cache = listed(kCacheWritingAnalyses, tool.name);
        tool.idempotent = IsTimelineEditingTool(tool.name) ||
                          listed(kProjectMutations, tool.name) ||
                          listed(kEditPlanCalls, tool.name) ||
                          listed(kReplayedExports, tool.name) ||
                          tool.writes_analysis_cache;
        tool.tool_class = ToolClassOf(tool.name);
        if (tool.idempotent && tool.name != "create_project") {
            InjectSchemaProperty(
                tool.input_schema_json, kMcpExpectedRevisionKey,
                StringSchema("Refused unless the project is at this "
                             "revision."));
        }
        if (tool.idempotent) {
            InjectSchemaProperty(
                tool.input_schema_json, kMcpIdempotencyKey,
                StringSchema("Replays this call's first answer if sent "
                             "again."));
        }
        // MOTIF2 -- short, on every edit: its vocabulary is told once, in
        // list_motifs' description and schema. Not in the server's
        // instructions, which a client keeps to 2,048 characters and which
        // had no room left.
        // MOTIF6 -- a tool that requires it declares its own.
        if (TakesMotif(tool.name) && !RequiresMotifWithAuthor(tool.name)) {
            InjectSchemaProperty(tool.input_schema_json, kMcpMotifKey,
                                 "{\"type\":\"object\",\"description\":\"Why "
                                 "this edit: {reason, note, author, "
                                 "marker_id}; see list_motifs.\"}");
        }
        if (tool.background) {
            InjectSchemaProperty(
                tool.input_schema_json, kMcpAsyncKey,
                BoolSchema("true: run in the background, answering at once "
                           "with a task_id for get_task; for work over a "
                           "minute. Server only."));
        }
    }

    // PROFIL-2026-11 PF1 -- the profile chooses last, from the catalog as
    // every surface sees it: what it serves is described and marked as the
    // full catalog has it. The three tables stay index-aligned.
    catalog_size_ = tools_.size();
    if (!profile_->everything) {
        std::vector<McpTool> tools;
        std::vector<McpDispatchFn> dispatch;
        std::vector<McpArgumentRules> rules;
        for (size_t index = 0; index < tools_.size(); ++index) {
            if (!McpToolProfileServes(*profile_, tools_[index].domain,
                                      tools_[index].name)) {
                withheld_.push_back(tools_[index].name);
                continue;
            }
            tools.push_back(std::move(tools_[index]));
            dispatch.push_back(std::move(dispatch_[index]));
            rules.push_back(std::move(argument_rules_[index]));
        }
        tools_ = std::move(tools);
        dispatch_ = std::move(dispatch);
        argument_rules_ = std::move(rules);
    }
}

bool McpToolRegistry::Offers(const std::string& toolName) const {
    return std::any_of(tools_.begin(), tools_.end(), [&](const McpTool& tool) {
        return tool.name == toolName;
    });
}

namespace {

// Unwraps the reserved envelope (McpTools.h) a dispatcher returns when it
// has a picture, so no caller has to know the convention: the outcome ends
// up with the tool's own result text and the image beside it.
void LiftImagePayload(McpToolCallOutcome& outcome) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(outcome.result_json, parsed, parseError) ||
        !parsed.IsObject()) {
        return;
    }
    const Value* data = parsed.Find(kMcpImageDataKey);
    const Value* mime = parsed.Find(kMcpImageMimeKey);
    if (data == nullptr || !data->IsString() || mime == nullptr ||
        !mime->IsString()) {
        return;
    }
    const Value* text = parsed.Find(kMcpImageTextKey);
    if (text == nullptr || !text->IsString()) return;
    outcome.image_base64 = data->AsString();
    outcome.image_mime = mime->AsString();
    outcome.result_json = text->AsString();
}

// B2 -- ROADMAP.md. This is the sole serializer for tool-call refusals.
// Dispatchers keep reporting the engine's existing stable code separately;
// every surface then receives the same machine-readable payload.
McpToolCallOutcome FinalizeOutcome(McpToolCallOutcome outcome) {
    if (outcome.ok) {
        LiftImagePayload(outcome);
        return outcome;
    }
    if (outcome.error_name.empty()) outcome.error_name = "InvalidOperation";
    if (outcome.message.empty()) outcome.message = "tool call failed";
    // PRD1 -- a tool that needed FFmpeg and had none says so by name,
    // whichever code its dispatcher gave the sentence it relayed.
    if (const std::string named = ffmpeg::RefusalName(outcome.message);
        !named.empty()) {
        outcome.error_name = named;
    }
    // PRD21 -- and so does align_words when MFA is found nowhere: the
    // dispatcher only relays the sentence, which it used to call
    // ValidationFailed.
    if (const std::string named = MfaRefusalName(outcome.message);
        !named.empty()) {
        outcome.error_name = named;
    }
    // PA3 -- and analyze_speakers when its model is found nowhere.
    if (const std::string named = DiarizationRefusalName(outcome.message);
        !named.empty()) {
        outcome.error_name = named;
    }
    Value partial;
    std::string parseError;
    const bool hasPartial =
        !outcome.result_json.empty() &&
        Value::Parse(outcome.result_json, partial, parseError) &&
        partial.IsObject();
    outcome.result_json =
        "{\"ok\":false,\"error\":\"" + Esc(outcome.error_name) +
        "\",\"detail\":\"" + Esc(outcome.message) + "\"" +
        (hasPartial ? ",\"result\":" + partial.Dump() : "") + "}";
    outcome.image_base64.clear();
    outcome.image_mime.clear();
    return outcome;
}

// PRD40 -- ROADMAP.md. The server's instructions promise a delta from every
// edit, and three tools of the 3 October series answered without one
// (generate_captions, close_junction_air, delete_timeline_range): their
// editors read the whole timeline again to learn what each had done. Every
// answer of a journaled edit -- the class table's word, not a list kept
// here -- is checked on its way out, and one that changed the timeline
// without saying how gets the delta the timeline's two states give: the
// diff DocumentDelta.h computes for every backend, so it cannot disagree
// with the edit it observed. An answer that carries its own keeps it, and
// so does one that chose to leave it out (apply_edit_plan's
// delta_as_prepared). Nothing is added when nothing changed: a preview, a
// proposal, an intervention folded.
void AttachMissingDelta(McpBackend& backend, const Document& before,
                        std::string& resultJson) {
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject() ||
        result.Find("delta") != nullptr ||
        result.Find("delta_as_prepared") != nullptr) {
        return;
    }
    Document after;
    std::string snapshotError;
    if (!backend.SnapshotDocument(after, snapshotError) ||
        after.SaveToString() == before.SaveToString()) {
        return;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(before, after, delta);
    Value deltaView;
    if (!Value::Parse(SerializeDocumentDelta(delta), deltaView, parseError)) {
        return;
    }
    result.Set("delta", std::move(deltaView));
    resultJson = result.Dump();
}

}  // namespace

bool McpToolRegistry::NormalizeArguments(const std::string& toolName,
                                         const mcp_json::Value& arguments,
                                         mcp_json::Value& normalized,
                                         std::string& message) const {
    for (size_t index = 0; index < tools_.size(); ++index) {
        if (tools_[index].name == toolName)
            return argument_rules_[index].Normalize(toolName, arguments,
                                                    normalized, message);
    }
    normalized = arguments;
    return true;
}

// PRD35 -- ROADMAP.md. The one place every surface's tool calls pass --
// the server, the app's relay, the chat panel -- so the one place each
// leaves its line in the engine log of a server or the app (EngineLog.h):
// the tool, how it ended, how long it took and the timeline it named, never
// its arguments nor its result. A process that keeps no log (the CLI, the
// tests) pays one atomic read.
McpToolCallOutcome McpToolRegistry::Call(
    McpBackend& backend, const std::string& toolName,
    const mcp_json::Value& arguments) const {
    if (!engine_log::Active()) {
        return CallTool(backend, toolName, arguments);
    }
    const engine_log::Clock::time_point started = engine_log::Clock::now();
    McpToolCallOutcome outcome = CallTool(backend, toolName, arguments);
    const Value* timeline =
        arguments.IsObject() ? arguments.Find("timeline_id") : nullptr;
    engine_log::RecordToolCall(
        toolName, outcome.ok ? std::string() : outcome.error_name, started,
        timeline != nullptr && timeline->IsString() ? timeline->AsString()
                                                    : std::string());
    return outcome;
}

McpToolCallOutcome McpToolRegistry::CallTool(
    McpBackend& backend, const std::string& toolName,
    const mcp_json::Value& arguments) const {
    McpToolCallOutcome outcome;
    size_t toolIndex = tools_.size();
    for (size_t index = 0; index < tools_.size(); ++index) {
        if (tools_[index].name == toolName) {
            toolIndex = index;
            break;
        }
    }
    if (toolIndex == tools_.size()) {
        outcome.ok = false;
        // PROFIL-2026-11 PF1 -- a tool the catalog has and the profile left
        // out is not a typo: say which profile, and how to have the tool.
        if (std::find(withheld_.begin(), withheld_.end(), toolName) !=
            withheld_.end()) {
            outcome.error_name = "ToolNotInProfile";
            outcome.message =
                "'" + toolName + "' is not served by this server's profile '" +
                profile_->name + "' (" + std::to_string(tools_.size()) +
                " of " + std::to_string(catalog_size_) +
                " tools: " + profile_->summary +
                "); start the server without --profile to "
                "have every tool";
        } else {
            outcome.error_name = "UnknownTool";
            outcome.message = "no such tool '" + toolName + "'";
        }
        return FinalizeOutcome(std::move(outcome));
    }

    // MCP allows omitting "arguments" for a no-argument tool; treat Null as
    // an empty object rather than a type error.
    static const Value kEmptyObject = Value::MakeObject();
    const Value& effectiveArguments =
        arguments.IsNull() ? kEmptyObject : arguments;
    if (!effectiveArguments.IsObject()) {
        outcome.ok = false;
        outcome.error_name = "ValidationFailed";
        outcome.message = "'" + toolName + "' arguments must be a JSON object";
        return FinalizeOutcome(std::move(outcome));
    }

    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md, DEC41. Where a motif is the
    // point of the call, it is required, and so is who decided. Checked on
    // the call as made, before the motif below is set on the backend and
    // the call made again without it.
    if (RequiresMotifWithAuthor(toolName) &&
        !backend.CurrentEditMotifs().motif) {
        const Value* given = effectiveArguments.Find(kMcpMotifKey);
        if (given == nullptr || !given->IsObject() ||
            given->Find("author") == nullptr) {
            outcome.ok = false;
            outcome.error_name = "ValidationFailed";
            outcome.message =
                "'" + toolName +
                ".motif' is required, with its author: a choice is recorded "
                "with why it was made and who made it (person when the "
                "person chose, agent when it was left to you)";
            return FinalizeOutcome(std::move(outcome));
        }
    }

    // MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. Why the call edits: read
    // here for every tool, so that no dispatcher learns the argument, set
    // on the backend for the call and cleared after it. The call goes on
    // without it.
    if (const Value* given = effectiveArguments.Find(kMcpMotifKey)) {
        if (!TakesMotif(toolName)) {
            outcome.ok = false;
            outcome.error_name = "ValidationFailed";
            outcome.message = "'" + toolName +
                              ".motif': a motif rides on an edit of a "
                              "timeline's journal, and " +
                              toolName + " writes none";
            return FinalizeOutcome(std::move(outcome));
        }
        EditMotif motif;
        if (!ParseMotifArgument(*given, toolName + ".motif", motif,
                                outcome.message)) {
            outcome.ok = false;
            outcome.error_name = "ValidationFailed";
            return FinalizeOutcome(std::move(outcome));
        }
        Value withoutMotif = Value::MakeObject();
        for (const auto& [key, value] : effectiveArguments.AsObject()) {
            if (key != kMcpMotifKey) withoutMotif.Set(key, value);
        }
        struct MotifScope {
            McpBackend& backend;
            ~MotifScope() { backend.SetEditMotifs({}); }
        };
        backend.SetEditMotifs({std::move(motif), {}});
        const MotifScope scope{backend};
        return CallTool(backend, toolName, withoutMotif);
    }

    // IA5 -- a running server's session consumes these before calling here
    // (McpSession.h). Reaching the catalog with one means there is no server
    // to keep the task, the answer to replay or the revision to compare.
    for (const char* key :
         {kMcpAsyncKey, kMcpIdempotencyKey, kMcpExpectedRevisionKey}) {
        if (effectiveArguments.Find(key) != nullptr) {
            outcome.ok = false;
            outcome.error_name = "UnsupportedOperation";
            outcome.message = "'" + toolName + "." + key +
                              "' needs a running MCP server (--mcp-serve); "
                              "--call-tool and the chat panel run each call "
                              "to its end";
            return FinalizeOutcome(std::move(outcome));
        }
    }

    // IA7 -- a server may run before any project exists. Opening or creating
    // one needs no snapshot to resolve IDs against; everything else needs a
    // project and is refused, by name, until there is one. PRD18 -- nor does
    // reading a procedure, which an agent does before it has a project.
    // MOTION-AFTER-2026-09 MA10 -- nor do the After Effects tools: the bridge
    // addresses an .aep and reads or writes no Document (RunAfterRequest),
    // and refusing them sent an agent to drive After by AppleScript.
    const bool afterTool = toolName.rfind("after_", 0) == 0;
    if (toolName == "create_project" || toolName == "open_project" ||
        toolName == "get_procedure" || afterTool ||
        // RM3 -- the forge reads corpus files, never a project.
        toolName == "train_caption_breaks" ||
        toolName == "query_project_archive" ||
        toolName == "import_project_archive" ||
        toolName == "backup_project_archive" ||
        ((toolName == "archive_project" ||
          toolName == "project_archive_status" ||
          toolName == "sync_project_archive") &&
         backend.HasOpenProject())) {
        const Document noDocument;
        const IdResolver noIds(noDocument);
        outcome.ok = dispatch_[toolIndex](backend, noIds, effectiveArguments,
                                          outcome.result_json,
                                          outcome.error_name, outcome.message);
        return FinalizeOutcome(std::move(outcome));
    }
    if (!backend.HasOpenProject()) {
        outcome.ok = false;
        outcome.error_name = "NoProjectOpen";
        outcome.message =
            "no project is open in this server: call "
            "create_project or open_project first";
        return FinalizeOutcome(std::move(outcome));
    }

    // PRD17 -- ROADMAP.md. Strict mode: an edit or a read of "the active
    // timeline" is refused, by name and with the timelines there are -- in a
    // series the active timeline is nobody's. Reads included: PL3 left them
    // free because a read cannot damage the montage, but an agent that reads
    // another's film decides its own edits on it.
    if (backend.RequiresExplicitTimeline()) {
        // PRD40 -- on the arguments as the dispatcher will read them, so a
        // rush named source_id is a rush too; a call the rules refuse is
        // refused by its dispatcher, under its own name.
        Value normalized;
        std::string ignored;
        if (!NormalizeArguments(toolName, effectiveArguments, normalized,
                                ignored)) {
            normalized = effectiveArguments;
        }
        if (StrictModeNeedsTimeline(toolName, normalized)) {
            outcome.ok = false;
            outcome.error_name = "TimelineRequired";
            outcome.message = McpTimelineRequiredMessage(backend, toolName);
            return FinalizeOutcome(std::move(outcome));
        }
    }

    const bool timelineEditing = IsTimelineEditingTool(toolName);
    // PRD17 -- import_resolve_timeline writes into the timeline it names, or
    // the active one, like an edit.
    const bool timelineScoped =
        timelineEditing || toolName == "export_timeline" ||
        toolName == "verify_export" || toolName == "check_caption_legibility" ||
        toolName == "analyze_timeline_audio" ||
        toolName == "begin_intervention" ||
        toolName == "import_resolve_timeline";
    // PL3 -- ROADMAP.md. Reads, and the preparation of a plan, select their
    // timeline without activating it. A plan named with no timeline_id goes
    // through SelectTimelineForEdit(""), which never switches timelines and
    // is where strict mode refuses an edit that does not name its timeline:
    // a plan is bound to the timeline chosen here.
    const bool timelineRead =
        IsTimelineReadingTool(toolName) || toolName == "prepare_edit_plan";
    if (timelineRead) {
        std::string timelineId;
        if (const Value* value = effectiveArguments.Find("timeline_id")) {
            if (!value->IsString() || value->AsString().empty()) {
                outcome.ok = false;
                outcome.error_name = "ValidationFailed";
                outcome.message =
                    "'" + toolName + ".timeline_id' must be a non-empty string";
                return FinalizeOutcome(std::move(outcome));
            }
            timelineId = value->AsString();
        }
        const bool selected =
            timelineId.empty() && toolName == "prepare_edit_plan"
                ? backend.SelectTimelineForEdit(timelineId, outcome.error_name,
                                                outcome.message)
                : backend.SelectTimelineForRead(timelineId, outcome.error_name,
                                                outcome.message);
        if (!selected) {
            outcome.ok = false;
            return FinalizeOutcome(std::move(outcome));
        }
    }
    if (timelineScoped) {
        // PRD16 -- ROADMAP.md. An export that names no timeline, or one the
        // project lacks, is answered with the timelines there are, name and
        // ID, for the caller to correct in one turn.
        const auto refuseNamingTimelines = [&] {
            Project project;
            std::string ignored;
            if (toolName == "export_timeline" &&
                backend.SnapshotProject(project, ignored)) {
                outcome.message += "; the project's timelines: " +
                                   Exporter::DescribeTimelines(project);
            }
            outcome.ok = false;
            return FinalizeOutcome(std::move(outcome));
        };
        std::string timelineId;
        if (const Value* value = effectiveArguments.Find("timeline_id")) {
            if (!value->IsString() || value->AsString().empty()) {
                outcome.error_name = "ValidationFailed";
                outcome.message =
                    "'" + toolName + ".timeline_id' must be a non-empty string";
                return refuseNamingTimelines();
            }
            timelineId = value->AsString();
        }
        if (!backend.SelectTimelineForEdit(timelineId, outcome.error_name,
                                           outcome.message)) {
            return refuseNamingTimelines();
        }
    }

    Document document;
    std::string snapshotError;
    if (!backend.SnapshotDocument(document, snapshotError)) {
        if (timelineScoped || timelineRead) {
            backend.EndTimelineEdit();
        }
        outcome.ok = false;
        outcome.error_name = "IoError";
        outcome.message = snapshotError;
        return FinalizeOutcome(std::move(outcome));
    }
    const IdResolver resolver(document);

    // MOTIF2 -- the motif's marker, resolved on the timeline the call edits.
    if (McpBackend::EditMotifs motifs = backend.CurrentEditMotifs();
        motifs.motif && !motifs.motif->marker_id.empty()) {
        if (!ResolveMotifMarker(motifs.motif, resolver, toolName,
                                outcome.message)) {
            if (timelineScoped || timelineRead) backend.EndTimelineEdit();
            outcome.ok = false;
            outcome.error_name = "ValidationFailed";
            return FinalizeOutcome(std::move(outcome));
        }
        backend.SetEditMotifs(std::move(motifs));
    }
    outcome.ok = dispatch_[toolIndex](backend, resolver, effectiveArguments,
                                      outcome.result_json, outcome.error_name,
                                      outcome.message);
    // PRD40 -- read while the call's timeline is still the one selected.
    if (outcome.ok &&
        tools_[toolIndex].tool_class == McpToolClass::JournaledEdit) {
        AttachMissingDelta(backend, document, outcome.result_json);
    }
    if (timelineScoped || timelineRead) {
        backend.EndTimelineEdit();
    }
    return FinalizeOutcome(std::move(outcome));
}
