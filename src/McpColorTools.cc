// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// colour: measures, white balance, CDL, the input of a media, matching,
// exposure and grade proposals, conforming and the sequence look. Moved out of
// McpTools.cc as they were; what they share with the other files of the catalog
// is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

// R6 -- docs/roadmap/REEL-2026-09.md. One look for the whole timeline.
// With a path, the .cube is copied into the project package under its
// digest and laid on the sequence; without one, the sequence's look is
// cleared -- the complete replacement state every other "set" of this lot
// uses, so the inverse of either is the previous reference exactly.
bool DispatchSetSequenceLut(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    const std::string tool = "set_sequence_lut";
    if (!CheckKnownKeys(args, {"path", "space"}, tool, message))
        return Fail(errorName, message, message);
    std::string path;
    std::string space;
    if (!ReadString(args, "path", tool, false, "", path, message) ||
        !ReadString(args, "space", tool, false, "", space, message))
        return Fail(errorName, message, message);
    if (path.empty()) {
        if (!space.empty())
            return Fail(errorName, message,
                        "'set_sequence_lut.space' without a path: leave both "
                        "out to clear the sequence's look");
        return backend.ApplyOperation(SetSequenceLutOperation{}, resultJson,
                                      errorName, message);
    }
    return backend.ImportSequenceLut(path, space, resultJson, errorName,
                                     message);
}

// G1 -- docs/roadmap/GRADE-2026-09.md. The scopes, measured (see
// ColorMeasure.h). The clips are checked here first, so an unknown or a
// motion clip answers with the error names every other clip tool uses.
bool DispatchMeasureColor(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids", "position",
                                                      "region"};
    if (!CheckKnownKeys(args, kAllowed, "measure_color", message))
        return Fail(errorName, message, message);
    ColorMeasurePlan plan;
    if (!ReadIdArray(args, "clip_ids", "measure_color", resolver, plan.clip_ids,
                     message) ||
        !ReadString(args, "position", "measure_color", false, "Middle",
                    plan.position, message))
        return Fail(errorName, message, message);
    if (plan.clip_ids.empty())
        return Fail(errorName, message,
                    "'measure_color.clip_ids' must name at least one clip");
    if (plan.clip_ids.size() > kMaxColorMeasureClips)
        return Fail(errorName, message,
                    "'measure_color.clip_ids' names at most " +
                        std::to_string(kMaxColorMeasureClips) +
                        " clips; measure a longer list in several calls");
    if (plan.position != "Start" && plan.position != "Middle" &&
        plan.position != "End")
        return Fail(errorName, message,
                    "'measure_color.position' must be 'Start', 'Middle' or "
                    "'End'");
    if (!ReadColorRegion(args, "region", "measure_color", plan.region, message))
        return Fail(errorName, message, message);

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    for (const Ulid& clipId : plan.clip_ids) {
        const DocumentClip* clip = document.FindClip(clipId);
        if (clip == nullptr) {
            errorName = EditErrorName(EditError::UnknownClip);
            message = "unknown clip_id '" + clipId + "'";
            return false;
        }
        if (IsMotionClip(*clip)) {
            errorName = EditErrorName(EditError::InvalidOperation);
            message = "clip '" + clipId +
                      "' is a motion clip: it is drawn by the engine and "
                      "carries no grade to measure";
            return false;
        }
    }
    std::vector<ColorClipMeasurement> results;
    if (!backend.MeasureClipColors(plan, results, message)) {
        errorName = "IoError";
        return false;
    }
    resultJson = ColorMeasurementsValue(document, plan, results).Dump();
    return true;
}

// G7 -- docs/roadmap/GRADE-2026-09.md. The as-shot white balance of one
// or more media: a value, or what each file's camera wrote. One undo step.
bool DispatchSetMediaWhiteBalance(McpBackend& backend,
                                  const IdResolver& resolver, const Value& args,
                                  std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_ids", "kelvin",
                                                      "from_camera"};
    if (!CheckKnownKeys(args, kAllowed, "set_media_white_balance", message))
        return Fail(errorName, message, message);
    std::vector<Ulid> mediaIds;
    if (!ReadIdArray(args, "media_ids", "set_media_white_balance", resolver,
                     mediaIds, message))
        return Fail(errorName, message, message);
    if (mediaIds.empty() || mediaIds.size() > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'set_media_white_balance.media_ids' names 1 to " +
                        std::to_string(kMaxBatchOperations) + " media");
    bool fromCamera = false;
    if (!ReadBool(args, "from_camera", "set_media_white_balance", false,
                  fromCamera, message))
        return Fail(errorName, message, message);
    const Value* kelvinField = args.Find("kelvin");
    if (fromCamera == (kelvinField != nullptr))
        return Fail(errorName, message,
                    "'set_media_white_balance' takes either 'kelvin' or "
                    "'from_camera': true");
    int64_t kelvin = 0;
    if (kelvinField != nullptr &&
        (!kelvinField->IsNumber() || !kelvinField->AsInt64(kelvin)))
        return Fail(errorName, message,
                    "'set_media_white_balance.kelvin' must be an integer, 0 "
                    "for unknown");

    std::vector<Operation> operations;
    Value read = Value::MakeArray();
    for (const Ulid& mediaId : mediaIds) {
        int32_t value = static_cast<int32_t>(kelvin);
        if (fromCamera) {
            MediaFileColor color;
            if (!backend.ReadMediaFileColor(mediaId, color, message)) {
                errorName = "IoError";
                return false;
            }
            value = color.white_balance_kelvin;
            Value item = Value::MakeObject();
            item.Set("media_id", Value::MakeString(mediaId));
            item.Set("white_balance_kelvin",
                     value > 0 ? Value::MakeInt(value) : Value::MakeNull());
            read.Push(std::move(item));
            // A file whose camera says nothing leaves the media as it is.
            if (value == 0) continue;
        }
        operations.push_back(SetMediaWhiteBalanceOperation{mediaId, value});
    }
    Value reply = Value::MakeObject();
    if (fromCamera) reply.Set("read", std::move(read));
    if (operations.empty()) {
        reply.Set("applied", Value::MakeBool(false));
        resultJson = reply.Dump();
        return true;
    }
    const Operation operation =
        operations.size() == 1
            ? operations.front()
            : Operation{
                  BatchOperation{"balance de tournage", std::move(operations)}};
    std::string applied;
    if (!backend.ApplyOperation(operation, applied, errorName, message))
        return false;
    mcp_json::Value result;
    std::string parseError;
    reply.Set("applied", Value::MakeBool(true));
    if (mcp_json::Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

// G4 -- docs/roadmap/GRADE-2026-09.md.
Value EffectStackValue(const std::vector<ClipEffect>& effects) {
    Value list = Value::MakeArray();
    for (const ClipEffect& effect : effects) {
        Value item = Value::MakeObject();
        item.Set("type", Value::MakeString(effect.type));
        Value params = Value::MakeObject();
        for (const auto& [name, fraction] : effect.params) {
            Value exact = Value::MakeObject();
            exact.Set("num", Value::MakeInt(fraction.num));
            exact.Set("den", Value::MakeInt(fraction.den));
            params.Set(name, std::move(exact));
        }
        item.Set("params", std::move(params));
        list.Push(std::move(item));
    }
    return list;
}

bool DispatchExportCdl(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    const std::string tool = "export_cdl";
    if (!CheckKnownKeys(args, {"clip_ids", "path", "resolve_node"}, tool,
                        message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    std::string path;
    int64_t node = 1;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, clipIds, message) ||
        !ReadString(args, "path", tool, false, "", path, message) ||
        !ReadInt64(args, "resolve_node", tool, false, 1, node, message))
        return Fail(errorName, message, message);
    if (clipIds.empty() || clipIds.size() > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'" + tool + ".clip_ids' names 1 to " +
                        std::to_string(kMaxBatchOperations) + " clips");
    if (node < 1 || node > 1000)
        return Fail(errorName, message,
                    "'" + tool + ".resolve_node' is a node number, from 1");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    std::vector<ColorCdl> cdls;
    ColorGradeSpace space = ColorGradeSpace::Display;
    if (!ClipColorCdls(document, clipIds, cdls, space, message))
        return Fail(errorName, message, message);
    if (!path.empty() &&
        !WriteColorCdlFile(path, cdls, ColorCdlSpaceName(space), message)) {
        errorName = "IoError";
        return false;
    }
    resultJson =
        ExportedCdlsValue(cdls, space, static_cast<int>(node), path).Dump();
    return true;
}

bool DispatchApplyCdl(McpBackend& backend, const IdResolver& resolver,
                      const Value& args, std::string& resultJson,
                      std::string& errorName, std::string& message) {
    const std::string tool = "apply_cdl";
    if (!CheckKnownKeys(args,
                        {"clip_ids", "path", "id", "cdl", "space", "preview"},
                        tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    std::string path;
    std::string id;
    std::string spaceName;
    bool preview = false;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, clipIds, message) ||
        !ReadString(args, "path", tool, false, "", path, message) ||
        !ReadString(args, "id", tool, false, "", id, message) ||
        !ReadString(args, "space", tool, true, "", spaceName, message) ||
        !ReadBool(args, "preview", tool, false, preview, message))
        return Fail(errorName, message, message);
    if (clipIds.empty() || clipIds.size() > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'" + tool + ".clip_ids' names 1 to " +
                        std::to_string(kMaxBatchOperations) + " clips");
    ColorGradeSpace space;
    if (!ParseColorCdlSpace(spaceName, space))
        return Fail(errorName, message,
                    "'" + tool +
                        ".space' is the values the CDL was made on: acescct, "
                        "linear_rec709, linear_rec2020 or display");
    const Value* inline_ = args.Find("cdl");
    if (path.empty() == (inline_ == nullptr))
        return Fail(
            errorName, message,
            "'" + tool + "' takes a CDL either from 'path' or as 'cdl'");
    if (!id.empty() && path.empty())
        return Fail(errorName, message,
                    "'" + tool +
                        ".id' names a correction of the file at "
                        "'path'");

    ColorCdl cdl;
    std::string inputDescription;
    if (!path.empty()) {
        std::vector<ColorCdl> cdls;
        if (!ReadColorCdlFile(path, cdls, inputDescription, message)) {
            errorName = "IoError";
            return false;
        }
        std::vector<std::string> ids;
        for (const ColorCdl& candidate : cdls) ids.push_back(candidate.id);
        const auto named =
            std::find_if(cdls.begin(), cdls.end(),
                         [&](const ColorCdl& item) { return item.id == id; });
        if (!id.empty() && named == cdls.end())
            return Fail(errorName, message,
                        "the file holds no correction '" + id + "'");
        if (id.empty() && cdls.size() > 1) {
            std::string list;
            for (const std::string& item : ids)
                list += (list.empty() ? "" : ", ") + item;
            return Fail(errorName, message,
                        "the file holds " + std::to_string(cdls.size()) +
                            " corrections; name one with 'id': " + list);
        }
        cdl = id.empty() ? cdls.front() : *named;
    } else {
        if (inline_ == nullptr || !inline_->IsObject())
            return Fail(errorName, message, "'" + tool + ".cdl' is an object");
        ResolveCdlMap map;
        map.saturation = "1";
        for (const auto& [key, value] : inline_->AsObject()) {
            std::string* field = key == "slope"        ? &map.slope
                                 : key == "offset"     ? &map.offset
                                 : key == "power"      ? &map.power
                                 : key == "saturation" ? &map.saturation
                                                       : nullptr;
            if (field == nullptr || !value.IsString())
                return Fail(errorName, message,
                            "'" + tool +
                                ".cdl' takes slope, offset, power and "
                                "saturation as Resolve's strings, e.g. "
                                "\"1.05 0.98 0.9\"");
            *field = value.AsString();
        }
        if (!ParseResolveCdlMap(map, cdl, message))
            return Fail(errorName, message, "'" + tool + ".cdl': " + message);
    }

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    std::vector<Operation> operations;
    Value clips = Value::MakeArray();
    Value steps = Value::MakeArray();
    double deviation = 0.0;
    for (const Ulid& clipId : clipIds) {
        ClipCdlPlacement placement;
        if (!PlaceClipCdl(document, clipId, cdl, space, placement, message))
            return Fail(errorName, message, message);
        deviation = std::max(deviation, placement.deviation);
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(clipId));
        Value replaced = Value::MakeArray();
        for (const std::string& type : placement.replaced)
            replaced.Push(Value::MakeString(type));
        item.Set("replaced", std::move(replaced));
        clips.Push(std::move(item));
        Value step = Value::MakeObject();
        step.Set("tool", Value::MakeString("set_clip_effects"));
        Value arguments = Value::MakeObject();
        arguments.Set("clip_id", Value::MakeString(clipId));
        arguments.Set("effects", EffectStackValue(placement.effects));
        step.Set("arguments", std::move(arguments));
        steps.Push(std::move(step));
        operations.push_back(
            SetClipEffectsOperation{clipId, std::move(placement.effects)});
    }
    Value reply = Value::MakeObject();
    reply.Set("space", Value::MakeString(ColorCdlSpaceId(space)));
    if (!path.empty()) {
        reply.Set("id", Value::MakeString(cdl.id));
        reply.Set("file_input_description",
                  inputDescription.empty()
                      ? Value::MakeNull()
                      : Value::MakeString(inputDescription));
    }
    reply.Set("cdl", ColorCdlValue(cdl));
    reply.Set("deviation", Value::MakeRawNumber(ShortestDecimal(deviation)));
    reply.Set("clips", std::move(clips));
    if (preview) {
        reply.Set("applied", Value::MakeBool(false));
        reply.Set("steps", std::move(steps));
        resultJson = reply.Dump();
        return true;
    }
    const Operation operation =
        operations.size() == 1
            ? operations.front()
            : Operation{BatchOperation{"CDL", std::move(operations)}};
    std::string applied;
    if (!backend.ApplyOperation(operation, applied, errorName, message))
        return false;
    mcp_json::Value result;
    std::string parseError;
    reply.Set("applied", Value::MakeBool(true));
    if (mcp_json::Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

Value ColorInputValue(const MediaColorInput& input) {
    if (input.graphics) {
        // DEC31 -- a graphic, read with its own matrix and range.
        Value value = Value::MakeObject();
        value.Set("graphics", Value::MakeBool(true));
        value.Set("ycbcr_matrix", Value::MakeString(input.ycbcr_matrix));
        value.Set("range", Value::MakeString(input.range));
        return value;
    }
    if (!DeclaresColorInput(input)) return Value::MakeNull();
    Value value = Value::MakeObject();
    value.Set("gamut", Value::MakeString(input.gamut));
    value.Set("transfer", Value::MakeString(input.transfer));
    value.Set("ycbcr_matrix", Value::MakeString(input.ycbcr_matrix));
    value.Set("range", Value::MakeString(input.range));
    return value;
}

// G3 -- docs/roadmap/GRADE-2026-09.md. The colour input one or more media
// are read as: a declared one, the one each file declares, or the
// project's. One undo step.
bool DispatchSetMediaColorInput(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    const std::string tool = "set_media_color_input";
    static const std::vector<std::string> kAllowed = {
        "media_ids",   "input_gamut", "input_transfer", "input_ycbcr_matrix",
        "input_range", "from_file",   "follow_project", "graphics"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> mediaIds;
    if (!ReadIdArray(args, "media_ids", tool, resolver, mediaIds, message))
        return Fail(errorName, message, message);
    if (mediaIds.empty() || mediaIds.size() > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'" + tool + ".media_ids' names 1 to " +
                        std::to_string(kMaxBatchOperations) + " media");
    bool fromFile = false;
    bool followProject = false;
    // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A logo or a still,
    // placed as authored: declared here, never guessed at ingest.
    bool graphics = false;
    MediaColorInput input;
    if (!ReadBool(args, "from_file", tool, false, fromFile, message) ||
        !ReadBool(args, "follow_project", tool, false, followProject,
                  message) ||
        !ReadBool(args, "graphics", tool, false, graphics, message) ||
        !ReadString(args, "input_gamut", tool, false, "", input.gamut,
                    message) ||
        !ReadString(args, "input_transfer", tool, false, "", input.transfer,
                    message) ||
        !ReadString(args, "input_ycbcr_matrix", tool, false, "auto",
                    input.ycbcr_matrix, message) ||
        !ReadString(args, "input_range", tool, false, "auto", input.range,
                    message))
        return Fail(errorName, message, message);
    const bool curve = args.Find("input_gamut") != nullptr ||
                       args.Find("input_transfer") != nullptr;
    const bool reading = args.Find("input_ycbcr_matrix") != nullptr ||
                         args.Find("input_range") != nullptr;
    const bool declared = curve || (reading && !graphics);
    if (static_cast<int>(declared) + static_cast<int>(fromFile) +
            static_cast<int>(followProject) + static_cast<int>(graphics) !=
        1)
        return Fail(errorName, message,
                    "'" + tool +
                        "' takes one of: input_gamut and input_transfer "
                        "(with input_ycbcr_matrix and input_range if the "
                        "file's own tags are wrong), graphics: true (with "
                        "input_ycbcr_matrix and input_range likewise), "
                        "from_file: true, or follow_project: true");
    if (graphics) {
        input.graphics = true;
        std::string inputError;
        if (!ValidateColorInputNames("rec709", "rec709", input.ycbcr_matrix,
                                     input.range, inputError))
            return Fail(errorName, message, "'" + tool + "': " + inputError);
    }
    if (declared) {
        if (input.gamut.empty() || input.transfer.empty())
            return Fail(errorName, message,
                        "'" + tool +
                            "' declares an input by both input_gamut and "
                            "input_transfer");
        std::string inputError;
        if (!ValidateColorInputNames(input.gamut, input.transfer,
                                     input.ycbcr_matrix, input.range,
                                     inputError))
            return Fail(errorName, message, "'" + tool + "': " + inputError);
        if (!IsSupportedColorInput(input.gamut, input.transfer))
            return Fail(errorName, message,
                        "'" + tool +
                            "': the pinned OpenColorIO config reads "
                            "no " +
                            input.transfer + " picture in " + input.gamut);
    }

    std::vector<Operation> operations;
    Value read = Value::MakeArray();
    for (const Ulid& mediaId : mediaIds) {
        MediaColorInput value = followProject ? MediaColorInput{} : input;
        if (fromFile) {
            MediaFileColor color;
            if (!backend.ReadMediaFileColor(mediaId, color, message)) {
                errorName = "IoError";
                return false;
            }
            Value item = Value::MakeObject();
            item.Set("media_id", Value::MakeString(mediaId));
            item.Set("color_input", ColorInputValue(color.input));
            read.Push(std::move(item));
            // A file that declares nothing leaves the media as it is, as
            // set_media_white_balance leaves it.
            if (!DeclaresColorInput(color.input)) continue;
            value = color.input;
        }
        operations.push_back(SetMediaColorInputOperation{mediaId, value});
    }
    Value reply = Value::MakeObject();
    if (fromFile) reply.Set("read", std::move(read));
    if (operations.empty()) {
        reply.Set("applied", Value::MakeBool(false));
        resultJson = reply.Dump();
        return true;
    }
    const Operation operation =
        operations.size() == 1
            ? operations.front()
            : Operation{BatchOperation{"entrée couleur des médias",
                                       std::move(operations)}};
    std::string applied;
    if (!backend.ApplyOperation(operation, applied, errorName, message))
        return false;
    mcp_json::Value result;
    std::string parseError;
    reply.Set("applied", Value::MakeBool(true));
    if (mcp_json::Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

// G2 -- docs/roadmap/GRADE-2026-09.md. A clip matched to a reference clip
// (ColorMatch.h). Read-only: the reply carries the step that would apply it.
bool DispatchMatchColor(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "reference_clip_id", "position", "reference_position",
        "region",  "reference_region",  "match"};
    if (!CheckKnownKeys(args, kAllowed, "match_color", message))
        return Fail(errorName, message, message);
    ColorMatchPlan plan;
    if (!ReadId(args, "clip_id", "match_color", resolver, true, plan.clip_id,
                message) ||
        !ReadId(args, "reference_clip_id", "match_color", resolver, true,
                plan.reference_clip_id, message) ||
        !ReadString(args, "position", "match_color", false, "Middle",
                    plan.position, message) ||
        !ReadString(args, "reference_position", "match_color", false, "Middle",
                    plan.reference_position, message) ||
        !ReadColorRegion(args, "region", "match_color", plan.region, message) ||
        !ReadColorRegion(args, "reference_region", "match_color",
                         plan.reference_region, message))
        return Fail(errorName, message, message);
    for (const std::string* position :
         {&plan.position, &plan.reference_position}) {
        if (*position != "Start" && *position != "Middle" && *position != "End")
            return Fail(errorName, message,
                        "'match_color' positions must be 'Start', 'Middle' "
                        "or 'End'");
    }
    if (const Value* match = args.Find("match")) {
        if (!match->IsArray() || match->AsArray().empty())
            return Fail(errorName, message,
                        "'match_color.match' must be a non-empty array of "
                        "'exposure', 'balance' and 'saturation'");
        plan.aspects = {false, false, false};
        for (const Value& item : match->AsArray()) {
            const std::string name = item.IsString() ? item.AsString() : "";
            if (name == "exposure") {
                plan.aspects.exposure = true;
            } else if (name == "balance") {
                plan.aspects.balance = true;
            } else if (name == "saturation") {
                plan.aspects.saturation = true;
            } else {
                return Fail(errorName, message,
                            "'match_color.match' takes 'exposure', "
                            "'balance' and 'saturation'");
            }
        }
    }

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    for (const Ulid* clipId : {&plan.clip_id, &plan.reference_clip_id}) {
        const DocumentClip* clip = document.FindClip(*clipId);
        if (clip == nullptr) {
            errorName = EditErrorName(EditError::UnknownClip);
            message = "unknown clip_id '" + *clipId + "'";
            return false;
        }
        if (IsMotionClip(*clip)) {
            errorName = EditErrorName(EditError::InvalidOperation);
            message = "clip '" + *clipId +
                      "' is a motion clip: it is drawn by the engine and "
                      "carries no grade to match";
            return false;
        }
    }
    ColorMatchResult result;
    if (!backend.MatchClipColor(plan, result, message)) {
        errorName = "IoError";
        return false;
    }
    resultJson = ColorMatchValue(document, plan, result).Dump();
    return true;
}

// AGENT-2026-09 A16 -- ROADMAP.md. The exposure of every clip proposed in
// one call (ColorMeasure.h). A stop count comes as a JSON number, which
// mcp_json keeps as text: read here to hundredths of a stop.
bool ReadStops(const Value& args, const std::string& key,
               const std::string& tool, int64_t fallback, int64_t& hundredths,
               std::string& message) {
    hundredths = fallback;
    const Value* field = args.Find(key);
    if (field == nullptr) return true;
    if (!field->IsNumber()) {
        message = "'" + tool + "." + key + "' must be a number of stops";
        return false;
    }
    std::istringstream stream(field->Dump());
    stream.imbue(std::locale::classic());
    double stops = 0.0;
    if (!(stream >> stops) || !std::isfinite(stops) || std::abs(stops) > 10.0) {
        message = "'" + tool + "." + key +
                  "' must be a number of stops between -10 and 10";
        return false;
    }
    hundredths = static_cast<int64_t>(std::llround(stops * 100.0));
    return true;
}

bool DispatchProposeExposure(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    const std::string tool = "propose_exposure";
    static const std::vector<std::string> kAllowed = {
        "clip_ids", "position", "target_stops", "tolerance_stops"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    ColorMeasurePlan plan;
    ExposureProposalSettings settings;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, plan.clip_ids,
                     message) ||
        !ReadString(args, "position", tool, false, "Middle", plan.position,
                    message) ||
        !ReadStops(args, "target_stops", tool, settings.target_hundredths,
                   settings.target_hundredths, message) ||
        !ReadStops(args, "tolerance_stops", tool, settings.tolerance_hundredths,
                   settings.tolerance_hundredths, message))
        return Fail(errorName, message, message);
    if (plan.position != "Start" && plan.position != "Middle" &&
        plan.position != "End")
        return Fail(errorName, message,
                    "'" + tool +
                        ".position' must be 'Start', 'Middle' or "
                        "'End'");
    if (settings.tolerance_hundredths < 0)
        return Fail(errorName, message,
                    "'" + tool + ".tolerance_stops' must not be negative");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    if (!document.color_management.enabled) {
        errorName = EditErrorName(EditError::InvalidOperation);
        message = tool +
                  " needs colour management: the scene exposure is measured "
                  "in the working space (set_color_management, or "
                  "conform_color_management for what the rushes declare)";
        return false;
    }
    if (plan.clip_ids.empty()) {
        // Every video clip of the timeline, in track then time order; a
        // motion clip is drawn by the engine and carries no grade.
        std::vector<const DocumentTrack*> tracks;
        for (const DocumentTrack& track : document.sequence.tracks)
            if (track.kind == "video") tracks.push_back(&track);
        std::stable_sort(
            tracks.begin(), tracks.end(),
            [](const DocumentTrack* left, const DocumentTrack* right) {
                return left->index < right->index;
            });
        // DEC31 -- nor a graphic under colour management, which takes no
        // grade.
        for (const DocumentTrack* track : tracks)
            for (const DocumentClip& clip : track->clips)
                if (!IsMotionClip(clip) &&
                    !(document.color_management.enabled &&
                      document.IsGraphicsMedia(clip.source_id)))
                    plan.clip_ids.push_back(clip.id);
        if (plan.clip_ids.empty())
            return Fail(errorName, message,
                        "'" + tool +
                            "': the timeline holds no video clip to "
                            "measure");
    } else {
        for (const Ulid& clipId : plan.clip_ids) {
            const DocumentClip* clip = document.FindClip(clipId);
            if (clip == nullptr) {
                errorName = EditErrorName(EditError::UnknownClip);
                message = "unknown clip_id '" + clipId + "'";
                return false;
            }
            if (IsMotionClip(*clip)) {
                errorName = EditErrorName(EditError::InvalidOperation);
                message = "clip '" + clipId +
                          "' is a motion clip: it is drawn by the engine and "
                          "carries no grade to expose";
                return false;
            }
        }
    }
    // Measured by measure_color's own path, a bounded list at a time.
    std::vector<ColorClipMeasurement> measurements;
    for (size_t first = 0; first < plan.clip_ids.size();
         first += kMaxColorMeasureClips) {
        ColorMeasurePlan chunk = plan;
        chunk.clip_ids.assign(
            plan.clip_ids.begin() + static_cast<std::ptrdiff_t>(first),
            plan.clip_ids.begin() +
                static_cast<std::ptrdiff_t>(std::min(
                    plan.clip_ids.size(), first + kMaxColorMeasureClips)));
        std::vector<ColorClipMeasurement> results;
        if (!backend.MeasureClipColors(chunk, results, message)) {
            errorName = "IoError";
            return false;
        }
        measurements.insert(measurements.end(), results.begin(), results.end());
    }
    std::vector<ExposureProposal> proposals;
    std::string error;
    if (!ProposeExposure(document, measurements, settings, proposals, error))
        return Fail(errorName, message, error);
    resultJson = ExposureProposalsValue(document, plan, settings, measurements,
                                        proposals)
                     .Dump();
    return true;
}

// AGENT-2026-09 A20 -- ROADMAP.md. Every clip's exposure and balance read on
// the face, or on a patch (ColorMeasure.h).
bool DispatchProposeGrade(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    const std::string tool = "propose_grade";
    static const std::vector<std::string> kAllowed = {
        "clip_ids",        "position",
        "anchor",          "region",
        "aspects",         "target_stops",
        "tolerance_stops", "highlight_ceiling_stops",
        "per_source",      "apply",
        "verbose"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    GradeProposalPlan plan;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, plan.clip_ids,
                     message) ||
        !ReadString(args, "position", tool, false, "Middle", plan.position,
                    message) ||
        !ReadString(args, "anchor", tool, false, "face", plan.anchor,
                    message) ||
        !ReadColorRegion(args, "region", tool, plan.region, message))
        return Fail(errorName, message, message);
    if (plan.anchor != "face" && plan.anchor != "neutral" &&
        plan.anchor != "median")
        return Fail(errorName, message,
                    "'" + tool +
                        ".anchor' must be 'face', 'neutral' or "
                        "'median'");
    if (plan.anchor == "neutral" && !plan.region.present)
        return Fail(errorName, message,
                    "'" + tool +
                        "' with a neutral anchor needs 'region', the "
                        "white or grey patch, as fractions of the picture");
    if (plan.position != "Start" && plan.position != "Middle" &&
        plan.position != "End")
        return Fail(errorName, message,
                    "'" + tool +
                        ".position' must be 'Start', 'Middle' or "
                        "'End'");
    if (const Value* aspects = args.Find("aspects")) {
        if (!aspects->IsArray() || aspects->AsArray().empty())
            return Fail(errorName, message,
                        "'" + tool +
                            ".aspects' must be a non-empty array of "
                            "'exposure' and 'balance'");
        plan.exposure = false;
        plan.balance = false;
        for (const Value& item : aspects->AsArray()) {
            const std::string name = item.IsString() ? item.AsString() : "";
            if (name == "exposure") {
                plan.exposure = true;
            } else if (name == "balance") {
                plan.balance = true;
            } else {
                return Fail(errorName, message,
                            "'" + tool +
                                ".aspects' takes 'exposure' and "
                                "'balance'");
            }
        }
    }
    // The balance of a whole frame means nothing: the median anchor
    // exposes only.
    if (plan.anchor == "median") plan.balance = false;
    // GR15 -- the face's default and the ceiling under an HLG output are
    // set once the document is read; what the caller names stays.
    const bool targetGiven = args.Find("target_stops") != nullptr;
    const bool ceilingGiven = args.Find("highlight_ceiling_stops") != nullptr;
    const int64_t defaultTarget = plan.anchor == "face" ? 70 : 0;
    if (!ReadStops(args, "target_stops", tool, defaultTarget,
                   plan.target_hundredths, message) ||
        !ReadStops(args, "tolerance_stops", tool, plan.tolerance_hundredths,
                   plan.tolerance_hundredths, message) ||
        !ReadStops(args, "highlight_ceiling_stops", tool,
                   plan.highlight_ceiling_hundredths,
                   plan.highlight_ceiling_hundredths, message))
        return Fail(errorName, message, message);
    if (plan.tolerance_hundredths < 0)
        return Fail(errorName, message,
                    "'" + tool + ".tolerance_stops' must not be negative");
    if (args.Find("per_source") != nullptr &&
        !ReadBool(args, "per_source", tool, false, plan.per_source, message))
        return Fail(errorName, message, message);
    bool apply = false;
    if (!ReadBool(args, "apply", tool, false, apply, message) ||
        !ReadBool(args, "verbose", tool, false, plan.verbose, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    if (!document.color_management.enabled) {
        errorName = EditErrorName(EditError::InvalidOperation);
        message = tool +
                  " needs colour management: the exposure is measured in "
                  "the working space (set_color_management, or "
                  "conform_color_management for what the rushes declare)";
        return false;
    }
    // GR15 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. On 6 November 2026,
    // on an HDR screen, the author chose an interview's face at +1.5 stops (60
    // % of the HLG signal, the median of 17 interviews he delivered in HLG from
    // Resolve) over +0.7 (48 %). In SDR the face stays at +0.7.
    if (document.color_management.output_transfer == "hlg") {
        if (!targetGiven && plan.anchor == "face") plan.target_hundredths = 150;
        if (!ceilingGiven) plan.display_ceiling = true;
    }
    if (plan.clip_ids.empty()) {
        std::vector<const DocumentTrack*> tracks;
        for (const DocumentTrack& track : document.sequence.tracks)
            if (track.kind == "video") tracks.push_back(&track);
        std::stable_sort(
            tracks.begin(), tracks.end(),
            [](const DocumentTrack* left, const DocumentTrack* right) {
                return left->index < right->index;
            });
        // DEC31 -- nor a graphic under colour management, which takes no
        // grade.
        for (const DocumentTrack* track : tracks)
            for (const DocumentClip& clip : track->clips)
                if (!IsMotionClip(clip) &&
                    !(document.color_management.enabled &&
                      document.IsGraphicsMedia(clip.source_id)))
                    plan.clip_ids.push_back(clip.id);
        if (plan.clip_ids.empty())
            return Fail(errorName, message,
                        "'" + tool +
                            "': the timeline holds no video clip to "
                            "read");
    } else {
        for (const Ulid& clipId : plan.clip_ids) {
            const DocumentClip* clip = document.FindClip(clipId);
            if (clip == nullptr) {
                errorName = EditErrorName(EditError::UnknownClip);
                message = "unknown clip_id '" + clipId + "'";
                return false;
            }
            if (IsMotionClip(*clip)) {
                errorName = EditErrorName(EditError::InvalidOperation);
                message = "clip '" + clipId +
                          "' is a motion clip: it is drawn by the engine and "
                          "carries no grade to read";
                return false;
            }
        }
    }
    if (plan.clip_ids.size() > 4 * kMaxColorMeasureClips)
        return Fail(errorName, message,
                    "'" + tool + "' reads at most " +
                        std::to_string(4 * kMaxColorMeasureClips) +
                        " clips in one call");
    std::vector<GradeProposal> proposals;
    if (!backend.ProposeClipGrades(plan, proposals, message)) {
        errorName = "IoError";
        return false;
    }
    Value reply = GradeProposalsValue(document, plan, proposals);
    if (!apply) {
        resultJson = reply.Dump();
        return true;
    }
    // AGENT-2026-09 A41 -- apply:true sets each proposed stack in turn, one
    // reversible operation per clip; the steps leave the reply, which the
    // native run of the 26th found too big to read in one piece.
    int64_t applied = 0;
    for (const GradeProposal& proposal : proposals) {
        if (proposal.effects.empty()) continue;
        std::string json;
        std::string name;
        std::string detail;
        SetClipEffectsOperation operation;
        operation.clip_id = proposal.clip_id;
        operation.effects = proposal.effects;
        if (!backend.ApplyOperation(operation, json, name, detail)) {
            errorName = name;
            message = "clip '" + proposal.clip_id + "' refused (" +
                      std::to_string(applied) +
                      " applied before it, each its own undo step): " + detail;
            return false;
        }
        ++applied;
    }
    // Applied, the reply keeps one line per clip: the second native run of
    // the 26th still read 32 KB after the steps had gone.
    Value appliedReply = Value::MakeObject();
    for (const auto& [key, value] : reply.AsObject())
        if (key != "steps" && key != "clips") appliedReply.Set(key, value);
    Value compact = Value::MakeArray();
    for (const GradeProposal& proposal : proposals) {
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(proposal.clip_id));
        item.Set("anchor_used", Value::MakeString(proposal.anchor_used));
        item.Set("reason", Value::MakeString(proposal.reason));
        item.Set("correction_hundredths",
                 Value::MakeInt(proposal.correction_hundredths));
        item.Set("balanced", Value::MakeBool(proposal.balanced));
        item.Set("applied", Value::MakeBool(!proposal.effects.empty()));
        if (!proposal.error.empty())
            item.Set("error", Value::MakeString(proposal.error));
        compact.Push(std::move(item));
    }
    appliedReply.Set("clips", std::move(compact));
    appliedReply.Set("applied", Value::MakeInt(applied));
    resultJson = appliedReply.Dump();
    return true;
}

bool DispatchConformColorManagement(McpBackend& backend, const IdResolver&,
                                    const Value& args, std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    const std::string tool = "conform_color_management";
    if (!CheckKnownKeys(args, {"preview"}, tool, message))
        return Fail(errorName, message, message);
    bool preview = false;
    if (!ReadBool(args, "preview", tool, false, preview, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    ColorInputProposal proposal;
    if (!ResolveColorInput(document.library, proposal, message))
        return Fail(errorName, message, message);
    Value root = Value::MakeObject();
    root.Set("media_considered",
             Value::MakeInt(static_cast<int64_t>(proposal.media_considered)));
    root.Set("media_agreeing",
             Value::MakeInt(static_cast<int64_t>(proposal.media_agreeing)));
    Value declarations = Value::MakeArray();
    for (const auto& [declaration, count] : proposal.declarations) {
        Value item = Value::MakeObject();
        item.Set("tags", Value::MakeString(declaration));
        item.Set("media", Value::MakeInt(static_cast<int64_t>(count)));
        declarations.Push(std::move(item));
    }
    root.Set("declarations", std::move(declarations));
    if (proposal.input_transfer.empty()) {
        root.Set("determined", Value::MakeBool(false));
        root.Set("applied", Value::MakeBool(false));
        root.Set("note",
                 Value::MakeString("most media declare no input and their tags "
                                   "name none (untagged footage, a camera "
                                   "log the metadata does not prove): set it "
                                   "with set_color_management, or per media "
                                   "with set_media_color_input"));
        resultJson = root.Dump();
        return true;
    }
    ColorManagementSettings settings = document.color_management;
    settings.enabled = settings.enabled || proposal.enable;
    settings.input_transfer = proposal.input_transfer;
    settings.input_gamut = proposal.input_gamut;
    settings.input_ycbcr_matrix = proposal.input_ycbcr_matrix;
    settings.input_range = proposal.input_range;
    Value proposed = Value::MakeObject();
    proposed.Set("enabled", Value::MakeBool(settings.enabled));
    proposed.Set("input_gamut", Value::MakeString(settings.input_gamut));
    proposed.Set("input_transfer", Value::MakeString(settings.input_transfer));
    proposed.Set("input_ycbcr_matrix",
                 Value::MakeString(settings.input_ycbcr_matrix));
    proposed.Set("input_range", Value::MakeString(settings.input_range));
    root.Set("determined", Value::MakeBool(true));
    root.Set("proposed", std::move(proposed));
    const ColorManagementSettings& current = document.color_management;
    const bool already =
        current.enabled == settings.enabled &&
        current.input_transfer == settings.input_transfer &&
        current.input_gamut == settings.input_gamut &&
        current.input_ycbcr_matrix == settings.input_ycbcr_matrix &&
        current.input_range == settings.input_range;
    root.Set("already_conformed", Value::MakeBool(already));
    if (preview || already) {
        root.Set("applied", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    std::string applied;
    if (!backend.ApplyOperation(SetColorManagementOperation{settings}, applied,
                                errorName, message))
        return false;
    root.Set("applied", Value::MakeBool(true));
    resultJson = root.Dump();
    return true;
}

}  // namespace

void RegisterColorTools(const McpToolAdder& add) {
    // G1 -- docs/roadmap/GRADE-2026-09.md.
    add("measure_color",
        "Measure what a colourist reads on the scopes, on one frame of each "
        "clip, as the export delivers that clip on its own: decoded, graded, "
        "the sequence look, the output transform (no opacity, fade, caption or "
        "clip above). Grade by these numbers, not by eye, and remeasure after "
        "each change. display: luma_percent p2/p50/p98 (% of full scale), "
        "clipped_percent, crushed_percent, saturation (FFmpeg signalstats "
        "SAT), zones (shadows under 25 %, highlights from 75 %) with warmth = "
        "red - blue (positive warm) and green_magenta = green - (red + blue) / "
        "2 (positive green, the opposite of color.tint). scene, under colour "
        "management only: stops_from_middle_grey, the unit of color.exposure. "
        "region, when given: the same on that rectangle, plus hue_degrees and "
        "skin_line_offset_degrees (positive towards yellow). No target: a "
        "night scene need not sit at 18 %.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Clips of the timeline to measure, 1 to 64. "
                                 "Each costs one decode."),
                   true)
            .Field("position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Which frame of each clip, as read_frame names "
                              "it. Defaults to Middle."),
                   false)
            .Field("region", kColorRegionSchema, false)
            .Build("measure_color arguments"),
        DispatchMeasureColor);

    // G7 -- docs/roadmap/GRADE-2026-09.md.
    add("set_media_white_balance",
        "Set the white balance each media was shot at, in Kelvin: what "
        "color.temperature is neutral at for every clip of it. Ingest reads "
        "the as-shot value from Sony cameras (describe lists it as a source's "
        "white_balance_kelvin). kelvin sets it by hand (1000 to 50000), 0 "
        "clears it: unknown, where 6500 K stands in.",
        SchemaBuilder()
            .Field("media_ids", IdArraySchema("Media to set, 1 to 256."), true)
            .Field("kelvin",
                   IntSchema("The as-shot white balance; 0 for unknown."),
                   false)
            .Field("from_camera",
                   BoolSchema("Read it again from each file, as ingest does, "
                              "for media ingested before."),
                   false)
            .Build("set_media_white_balance arguments"),
        DispatchSetMediaWhiteBalance);

    // G4 -- docs/roadmap/GRADE-2026-09.md.
    add("export_cdl",
        "Write clips' primary grades as ASC CDLs -- slope, offset and power "
        "per channel, then a saturation -- the format corrections travel in "
        "between applications. Each clip's grade is folded as it renders: "
        "color.exposure, color.contrast, color.tint, color.wheels (a wheel's "
        "gamma is the CDL's power) and color.saturation compose into one CDL "
        "where their order allows it. A stage with no CDL form "
        "(color.temperature, color.curves, color.vibrance, color.highlights, "
        "color.shadows) refuses the clip by name; nothing is dropped silently. "
        "The numbers mean something only on the values the grade acts on: "
        "space says which (acescct under colour management with ACEScct), and "
        "the file's InputDescription says it too. path writes a .cc (one "
        "clip), .ccc or .cdl, each correction's id the clip's. resolve_set_cdl "
        "is the map DaVinci Resolve's TimelineItem.SetCDL takes, for node "
        "resolve_node (default 1): valid when that node grades the same "
        "values. A saturation other than 1 is warned: a DaVinci YRGB project "
        "weighs it otherwise, and resolve_yrgb_gap_codes says by how many "
        "10-bit codes. Changes nothing in the project.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Clips to fold, 1 to 256."), true)
            .Field("path",
                   StringSchema("A .cc, .ccc or .cdl file to write; omit to "
                                "only read the numbers. A server takes a "
                                "relative path from the project's folder."),
                   false)
            .Field("resolve_node",
                   IntSchema("Node number in resolve_set_cdl; default 1."),
                   false)
            .Build("export_cdl arguments"),
        DispatchExportCdl);

    add("apply_cdl",
        "Apply an ASC CDL to clips, in place of their primary grade: each "
        "clip's color.exposure, color.contrast, color.tint, color.wheels and "
        "color.saturation stages are replaced by a color.wheels (lift = "
        "offset, gain = log2(slope + offset), gamma = -log2(power)) and a "
        "color.saturation, where the first of them stood; other stages keep "
        "their place. The CDL comes from path (.cc, .ccc or .cdl, read by "
        "OpenColorIO; id names one correction of a file holding several) or as "
        "cdl, the strings Resolve's TimelineItem.SetCDL takes. space must be "
        "the values the clips are graded on. Offset and saturation land "
        "exactly; gain and gamma are logarithms rounded to millionths, and "
        "deviation says by how much the knobs moved. A slope, power or "
        "saturation a wheel cannot reach is refused by value. preview returns "
        "the set_clip_effects steps instead.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Clips to grade, 1 to 256."), true)
            .Field("space",
                   EnumSchema({"acescct", "linear_rec709", "linear_rec2020",
                               "display"},
                              "The values the CDL was made on."),
                   true)
            .Field("path", StringSchema("A .cc, .ccc or .cdl file."), false)
            .Field("id",
                   StringSchema("The correction of the file to apply, by "
                                "its id; needed when it holds several."),
                   false)
            .Field("cdl",
                   SchemaBuilder()
                       .Field("slope", StringSchema("\"r g b\""), true)
                       .Field("offset", StringSchema("\"r g b\""), true)
                       .Field("power", StringSchema("\"r g b\""), true)
                       .Field("saturation",
                              StringSchema("One number; default \"1\"."), false)
                       .Build("a CDL, as Resolve's SetCDL takes it"),
                   false)
            .Field("preview",
                   BoolSchema("Return the steps instead of applying them; "
                              "default false."),
                   false)
            .Build("apply_cdl arguments"),
        DispatchApplyCdl);

    // G3 -- docs/roadmap/GRADE-2026-09.md.
    add("set_media_color_input",
        "Set the colour input each media is read as, in place of the project's "
        "(set_color_management): a Rec.709 or HLG phone clip in an S-Log3 "
        "project, an S-Log3 rush in an HLG one. Only read when the project "
        "manages colour, a graphic aside. Ingest declares it from the file "
        "(describe: color_input); a media without one follows the project. "
        "Under colour management a graphic takes no grade.",
        SchemaBuilder()
            .Field("media_ids", IdArraySchema("Media to set, 1 to 256."), true)
            .Field("input_gamut",
                   EnumSchema({"rec709", "sony_sgamut3_cine", "sony_sgamut3",
                               "rec2020"},
                              "The media's primaries."),
                   false)
            .Field("input_transfer",
                   EnumSchema({"rec709", "sony_slog3", "linear", "hlg"},
                              "The media's transfer curve."),
                   false)
            .Field("input_ycbcr_matrix",
                   EnumSchema({"auto", "bt709", "bt2020_ncl"},
                              "Defaults to auto: the file's own tag."),
                   false)
            .Field("input_range",
                   EnumSchema({"auto", "full", "limited"},
                              "Defaults to auto: the file's own tag."),
                   false)
            .Field("from_file",
                   BoolSchema("Read it again from each file, as ingest does, "
                              "for media ingested before."),
                   false)
            .Field("follow_project",
                   BoolSchema("Read the media with the project's input."),
                   false)
            .Field("graphics",
                   BoolSchema("Place the media as a graphic (a logo, a "
                              "packshot, a still): its code values are what it "
                              "shows, as a motion title's colours (a white "
                              "logo reaches white); ingest never sets it."),
                   false)
            .Build("set_media_color_input arguments"),
        DispatchSetMediaColorInput);

    // G2 -- docs/roadmap/GRADE-2026-09.md.
    add("match_color",
        "Match a clip to a reference clip the way a colourist matches a scene "
        "to its hero shot: exposure, then balance, then saturation, and "
        "nothing else (no curve, no look). Solved on one frame of each clip, "
        "after its own grade. Applies nothing: returns one set_clip_effects "
        "step, the clip's own exposure, temperature, tint and saturation knobs "
        "adjusted in its stack. measures gives measure_color's numbers for the "
        "reference, the clip before and the clip after. Match a face to a face "
        "or a grey card to a grey card with region and reference_region. "
        "kelvin_limited says the balance wanted a temperature past 1667 or "
        "25000 K, where color.temperature stops; saturation_undefined that a "
        "picture has no colour to scale. Check the result with read_frame "
        "graded.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to correct."), true)
            .Field("reference_clip_id",
                   IdSchema("Clip to match it to; left as it is."), true)
            .Field("position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Frame of the clip, as read_frame names it. "
                              "Defaults to Middle."),
                   false)
            .Field("reference_position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Frame of the reference. Defaults to Middle."),
                   false)
            .Field("region", kColorRegionSchema, false)
            .Field("reference_region", kColorRegionSchema, false)
            .Field("match",
                   ArraySchema(EnumSchema({"exposure", "balance", "saturation"},
                                          "An aspect to match."),
                               "What to match; defaults to all three."),
                   false)
            .Build("match_color arguments"),
        DispatchMatchColor);

    add("propose_exposure",
        "Propose the exposure of every video clip of the timeline (or of "
        "clip_ids) in one call, under colour management: each clip is measured "
        "as measure_color measures it, and one whose scene median sits further "
        "than tolerance_stops from target_stops gets a set_clip_effects step "
        "under `steps` with its own exposure moved by the difference, the rest "
        "of its grade kept; `clips` says for each what was measured and why it "
        "got a step or not (proposed, within_tolerance, no_scene_measure). "
        "Applies nothing; returns set_clip_effects `steps`.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Clips to expose; omitted, every video clip "
                                 "of the timeline that is not a motion "
                                 "clip."),
                   false)
            .Field("position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Which frame of each clip to measure; Middle "
                              "by default."),
                   false)
            .Field("target_stops",
                   "{\"type\":\"number\",\"description\":\"Where the scene "
                   "median should land, in stops from 18 % grey; 0 by "
                   "default.\"}",
                   false)
            .Field("tolerance_stops",
                   "{\"type\":\"number\",\"description\":\"A clip within "
                   "this of the target gets no step, in stops; 0.5 by "
                   "default.\"}",
                   false)
            .Build("propose_exposure arguments"),
        DispatchProposeExposure);

    // AX-2026-09 -- `verbose` exists because the knobs and steps of 65
    // clips weighed 57 KB; PRD14 keeps the lot's name here. PRD15 -- the
    // figure, the defaults and why 0.7 live in their arguments' descriptions
    // (verbose, tolerance_stops, highlight_ceiling_stops, target_stops): the
    // tool's own ran past the 2,048 characters Claude Code reads.
    add("propose_grade",
        "Propose the exposure and the white balance of every video clip of the "
        "timeline (or of clip_ids), read where a colourist reads them, under "
        "colour management. Balance puts the anchor's midtones on the skin "
        "line (face) or neutral (patch). A clip whose face is not found gets "
        "its exposure from the montage (the median the found faces left the "
        "frames at) and no balance. `clips` gives each reading and why, with "
        "anchor_region, the face found: on a made-up or masked subject, the "
        "make-up -- read_frame a surprising proposal. Prefer this to "
        "propose_exposure on an interview.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Clips to read; omitted, every video clip "
                                 "of the timeline that is not a motion "
                                 "clip."),
                   false)
            .Field("position",
                   EnumSchema({"Start", "Middle", "End"},
                              "Which frame of each clip to read; Middle by "
                              "default."),
                   false)
            .Field("anchor",
                   EnumSchema({"face", "neutral", "median"},
                              "Where each clip is read: its largest face "
                              "(default), a white or grey patch given as "
                              "region, or the whole frame (exposure only)."),
                   false)
            .Field("region", kColorRegionSchema, false)
            .Field("aspects",
                   ArraySchema(EnumSchema({"exposure", "balance"}, ""),
                               "What to propose; both by default."),
                   false)
            // PRD15 -- why 0.7 moved here from the tool's description,
            // which ran past the 2,048 characters a client reads.
            .Field("target_stops",
                   "{\"type\":\"number\",\"description\":\"Where the anchor's "
                   "scene median should land, in stops from 18 % grey; 0.7 by "
                   "default for a face (right on two films), 1.5 when the "
                   "sequence delivers HLG (the face chosen on an HDR screen), "
                   "0 otherwise.\"}",
                   false)
            .Field("tolerance_stops",
                   "{\"type\":\"number\",\"description\":\"A clip within "
                   "this of the target gets no exposure change, in stops; "
                   "0.25 by default.\"}",
                   false)
            .Field("highlight_ceiling_stops",
                   "{\"type\":\"number\",\"description\":\"How far over "
                   "18 % grey the frame's scene p98 may go once lifted, in "
                   "stops; 2.5 (diffuse white) by default. A lift past it "
                   "is cut to what the highlights leave room for. Left out "
                   "when the sequence delivers HLG, the ceiling is the "
                   "output itself: a lift stands while no more of the "
                   "rendered frame lands on the top code than before, and "
                   "is cut by a few renders to the largest that does "
                   "(the reply says highlight_ceiling: display_clipping); "
                   "given, it keeps this scene rule.\"}",
                   false)
            .Field("apply",
                   BoolSchema("true: set each proposed stack on its clip at "
                              "once, one undo step per clip, and leave the "
                              "steps out of the reply; default false, nothing "
                              "is applied."),
                   false)
            .Field("verbose",
                   BoolSchema("true: the full reading of every clip, knobs and "
                              "steps included; default false."),
                   false)
            .Field("per_source",
                   BoolSchema("true (default): one grade per rush, voted by "
                              "its clips, so a cut inside a take does not "
                              "jump; false: each clip on its own reading."),
                   false)
            .Build("propose_grade arguments"),
        DispatchProposeGrade);

    add("conform_color_management",
        "Set the project's colour management input -- shared by every "
        "timeline, and read for every media that declares no input of its "
        "own (set_media_color_input) -- to what the project's rushes "
        "declare, and report what it read. Each media counts for its own "
        "declared input, else for its tags. HLG or S-Log3 enables "
        "management with that input; Rec.709 sets the rec709 input and "
        "leaves management as it is. Untagged footage without a "
        "declaration says nothing usable: the result says so and changes "
        "nothing, and set_color_management is the way. Working and output "
        "settings are kept. Mixed tags are listed in \"declarations\"; the "
        "most common input wins. preview reports without applying.",
        SchemaBuilder()
            .Field("preview",
                   BoolSchema("Report the proposal without applying it; "
                              "default false."),
                   false)
            .Build("conform_color_management arguments"),
        DispatchConformColorManagement);

    // R6 -- docs/roadmap/REEL-2026-09.md. The look of the whole timeline,
    // in one place instead of on every clip.
    add("set_sequence_lut",
        "Give the selected timeline its look: a .cube LUT applied once for the "
        "whole sequence, after every clip's own color.* grade and before the "
        "output transform. Declare the space the LUT expects: with colour "
        "management on, 'acescct' for a LUT computed on the log values of the "
        "ACEScct working space, or 'rec709' for a LUT written for a Rec.709 "
        "display signal, laid on the delivered signal.",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of the .cube file to import. "
                                "Leave it out to clear the look."),
                   false)
            // PRD15 -- the two spaces LookLut.cc accepts, by its own names.
            .Field("space",
                   EnumSchema({kLookLutSpaceWorkingLog, kLookLutSpaceDisplay},
                              "The space the LUT expects, under colour "
                              "management; left out without it."),
                   false)
            .Build("set_sequence_lut arguments"),
        DispatchSetSequenceLut);
}
