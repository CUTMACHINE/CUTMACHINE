// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// the properties of one clip: link, effects and grade, opacity, speed, framing
// and the tracking that drives it, animation and audio. Moved out of
// McpTools.cc as they were; what they share with the other files of the catalog
// is in McpToolsInternal.h.

#include "LinkGroupRepair.h"
#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

// Out of lot -- the author, 4 November 2026 (LinkGroupRepair.h). A timeline cut
// before #213 binds a block's stretches under one link; this splits it one
// group per place, the repair the window applies when it opens a timeline.
bool DispatchSplitLinkGroups(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    const std::string tool = "split_link_groups";
    if (!CheckKnownKeys(args, {"preview"}, tool, message)) {
        return Fail(errorName, message, message);
    }
    bool preview = false;
    if (!ReadBool(args, "preview", tool, false, preview, message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const LinkGroupRepair repair =
        PlanLinkGroupRepair(document, [] { return GenerateUlid(); });
    Value reply = Value::MakeObject();
    reply.Set("groups", Value::MakeInt(static_cast<int64_t>(repair.groups)));
    reply.Set("places", Value::MakeInt(static_cast<int64_t>(repair.places)));
    reply.Set("cleared", Value::MakeInt(static_cast<int64_t>(repair.cleared)));
    reply.Set("left_alone",
              Value::MakeInt(static_cast<int64_t>(repair.left_alone)));
    reply.Set("operations",
              Value::MakeInt(static_cast<int64_t>(repair.operations.size())));
    std::vector<BatchOperation> batches = LinkGroupRepairBatches(repair);
    if (preview || batches.empty()) {
        reply.Set("applied", Value::MakeBool(false));
        resultJson = reply.Dump();
        return true;
    }
    // More than kMaxBatchOperations links are more than one batch, so more
    // than one undo step; each batch answers its own delta.
    Value results = Value::MakeArray();
    for (BatchOperation& batch : batches) {
        std::string applied;
        if (!backend.ApplyOperation(Operation{std::move(batch)}, applied,
                                    errorName, message)) {
            return false;
        }
        Value result;
        std::string parseError;
        if (Value::Parse(applied, result, parseError)) {
            results.Push(std::move(result));
        }
    }
    reply.Set("applied", Value::MakeBool(true));
    reply.Set("results", std::move(results));
    resultJson = reply.Dump();
    return true;
}

bool DispatchSetClipLink(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "first_clip_id", "second_clip_id", "linked"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_link", message))
        return Fail(errorName, message, message);
    SetClipLinkOperation op;
    bool linked = true;
    if (!ReadId(args, "first_clip_id", "set_clip_link", resolver, true,
                op.first_clip_id, message) ||
        !ReadId(args, "second_clip_id", "set_clip_link", resolver, true,
                op.second_clip_id, message) ||
        !ReadBool(args, "linked", "set_clip_link", true, linked, message))
        return Fail(errorName, message, message);
    if (linked) {
        const Ulid group = GenerateUlid();
        op.first_group_id = group;
        op.second_group_id = group;
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetClipEffects(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "effects"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_effects", message))
        return Fail(errorName, message, message);
    SetClipEffectsOperation op;
    if (!ReadId(args, "clip_id", "set_clip_effects", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* effects = args.Find("effects");
    if (!effects || !effects->IsArray())
        return Fail(errorName, message,
                    "'set_clip_effects.effects' must be an array");
    static const std::vector<std::string> kEffectKeys = {"type", "params"};
    for (size_t index = 0; index < effects->AsArray().size(); ++index) {
        const Value& raw = effects->AsArray()[index];
        const std::string path =
            "set_clip_effects.effects[" + std::to_string(index) + "]";
        if (!CheckKnownKeys(raw, kEffectKeys, path, message))
            return Fail(errorName, message, message);
        ClipEffect effect;
        effect.id.clear();
        if (!ReadString(raw, "type", path, true, "", effect.type, message))
            return Fail(errorName, message, message);
        const Value* params = raw.Find("params");
        if (params) {
            if (!params->IsObject())
                return Fail(errorName, message,
                            "'" + path + ".params' must be an object");
            for (const auto& entry : params->AsObject()) {
                EffectParamValue value;
                if (!ReadFractionValue(entry.second,
                                       path + ".params." + entry.first,
                                       value.num, value.den, message))
                    return Fail(errorName, message, message);
                effect.params[entry.first] = value;
            }
        }
        op.effects.push_back(std::move(effect));
    }
    // GR7 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. A stack longer than a
    // clip's grade renders was cut short in silence; what builds one is
    // told, by the stage the picture would stop before.
    std::string overflow;
    if (!ColorStackFits(op.effects, overflow))
        return Fail(errorName, message, "set_clip_effects: " + overflow);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. One clip's grade
// pasted onto others, as the window's Copy and Paste of a grade does
// (ClipGradeCopy.h). One undo step.
bool DispatchPasteClipGrade(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    const std::string tool = "paste_clip_grade";
    if (!CheckKnownKeys(args, {"from_clip_id", "clip_ids"}, tool, message))
        return Fail(errorName, message, message);
    Ulid fromClipId;
    std::vector<Ulid> clipIds;
    if (!ReadId(args, "from_clip_id", tool, resolver, true, fromClipId,
                message) ||
        !ReadIdArray(args, "clip_ids", tool, resolver, clipIds, message))
        return Fail(errorName, message, message);
    if (clipIds.empty() || clipIds.size() > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'" + tool + ".clip_ids' names 1 to " +
                        std::to_string(kMaxBatchOperations) + " clips");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    ClipGradePaste paste;
    if (!BuildClipGradePaste(document, fromClipId, clipIds, paste, message)) {
        if (message.rfind("unknown clip_id", 0) == 0) {
            errorName = EditErrorName(EditError::UnknownClip);
            return false;
        }
        return Fail(errorName, message, message);
    }
    Value reply = Value::MakeObject();
    const auto ids = [](const std::vector<Ulid>& list) {
        Value array = Value::MakeArray();
        for (const Ulid& id : list) array.Push(Value::MakeString(id));
        return array;
    };
    reply.Set("pasted", ids(paste.pasted));
    reply.Set("unchanged", ids(paste.unchanged));
    Value skipped = Value::MakeArray();
    for (const ClipGradePaste::Skipped& item : paste.skipped) {
        Value entry = Value::MakeObject();
        entry.Set("clip_id", Value::MakeString(item.clip_id));
        entry.Set("reason", Value::MakeString(item.reason));
        skipped.Push(std::move(entry));
    }
    reply.Set("skipped", std::move(skipped));
    if (!paste.operation) {
        reply.Set("applied", Value::MakeBool(false));
        resultJson = reply.Dump();
        return true;
    }
    std::string applied;
    if (!backend.ApplyOperation(*paste.operation, applied, errorName, message))
        return false;
    reply.Set("applied", Value::MakeBool(true));
    Value result;
    std::string parseError;
    if (Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

bool DispatchSetClipOpacity(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "opacity"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_opacity", message))
        return Fail(errorName, message, message);
    SetClipOpacityOperation op;
    if (!ReadId(args, "clip_id", "set_clip_opacity", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* opacity = args.Find("opacity");
    if (!opacity || !ReadFractionValue(*opacity, "set_clip_opacity.opacity",
                                       op.opacity.num, op.opacity.den, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md.
bool DispatchSetClipBlur(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "blur"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_blur", message))
        return Fail(errorName, message, message);
    SetClipBlurOperation op;
    if (!ReadId(args, "clip_id", "set_clip_blur", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* blur = args.Find("blur");
    if (!blur || !ReadFractionValue(*blur, "set_clip_blur.blur", op.blur.num,
                                    op.blur.den, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// PA8 -- docs/roadmap/PARITE-2026-11.md. The shape that cuts a clip's
// picture out; `shape: "none"` removes it.
bool DispatchSetClipMask(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string tool = "set_clip_mask";
    static const std::vector<std::string> kAllowed = {
        "clip_id", "shape",  "center_x", "center_y",
        "width",   "height", "feather",  "inverted"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    SetClipMaskOperation op;
    std::string shape;
    if (!ReadId(args, "clip_id", tool, resolver, true, op.clip_id, message) ||
        !ReadString(args, "shape", tool, true, "", shape, message) ||
        !ReadBool(args, "inverted", tool, false, op.mask.inverted, message))
        return Fail(errorName, message, message);
    if (shape != "none" && shape != kClipMaskRectangle &&
        shape != kClipMaskEllipse)
        return Fail(
            errorName, message,
            "'" + tool + ".shape' must be 'rectangle', 'ellipse' or 'none'");
    const auto readFraction = [&](const char* key, EffectParamValue& value) {
        const Value* field = args.Find(key);
        if (field == nullptr || field->IsNull()) return true;
        return ReadFractionValue(*field, tool + "." + key, value.num, value.den,
                                 message);
    };
    if (!readFraction("center_x", op.mask.center_x) ||
        !readFraction("center_y", op.mask.center_y) ||
        !readFraction("width", op.mask.width) ||
        !readFraction("height", op.mask.height) ||
        !readFraction("feather", op.mask.feather))
        return Fail(errorName, message, message);
    if (shape == "none") {
        // Nothing else is said of a mask that is taken off.
        for (const char* key : {"center_x", "center_y", "width", "height",
                                "feather", "inverted"}) {
            if (args.Find(key) != nullptr)
                return Fail(errorName, message,
                            "'" + tool + "." + key +
                                "' places a mask, and shape 'none' removes "
                                "it");
        }
        op.mask = ClipMask{};
    } else {
        op.mask.shape = shape;
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// R3 -- docs/roadmap/REEL-2026-09.md.
bool DispatchSetClipSpeed(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "speed",
                                                      "speed_ramp"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_speed", message))
        return Fail(errorName, message, message);
    SetClipSpeedOperation op;
    if (!ReadId(args, "clip_id", "set_clip_speed", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* speed = args.Find("speed");
    const Value* ramp = args.Find("speed_ramp");
    if (ramp != nullptr && ramp->IsNull()) ramp = nullptr;
    if ((speed == nullptr) == (ramp == nullptr))
        return Fail(errorName, message,
                    "set_clip_speed takes 'speed' (one speed) or "
                    "'speed_ramp' (keys of speed), exactly one of the two");
    if (speed != nullptr &&
        !ReadFractionValue(*speed, "set_clip_speed.speed", op.speed.num,
                           op.speed.den, message))
        return Fail(errorName, message, message);
    // R3, criterion 4 -- the keys spelled as describe writes them back. An
    // id left out is generated: keys are what the author wrote, and the
    // operation that replays them has to name the same ones.
    if (ramp != nullptr) {
        if (!ramp->IsArray())
            return Fail(errorName, message,
                        "'set_clip_speed.speed_ramp' must be an array of keys");
        for (const Value& item : ramp->AsArray()) {
            if (!item.IsObject())
                return Fail(errorName, message,
                            "'set_clip_speed.speed_ramp' entries must be "
                            "objects");
            for (const auto& key : item.AsObject()) {
                if (key.first != "id" && key.first != "time" &&
                    key.first != "value" && key.first != "interpolation")
                    return Fail(errorName, message,
                                "'set_clip_speed.speed_ramp' has unknown key "
                                "'" +
                                    key.first + "'");
            }
            ClipKeyframe keyframe;
            if (!ReadString(item, "id", "set_clip_speed.speed_ramp", false, "",
                            keyframe.id, message))
                return Fail(errorName, message, message);
            if (keyframe.id.empty()) keyframe.id = GenerateUlid();
            if (!ReadTime(item, "time", "set_clip_speed.speed_ramp", true,
                          keyframe.time, message))
                return Fail(errorName, message, message);
            const Value* value = item.Find("value");
            if (value == nullptr ||
                !ReadFractionValue(*value, "set_clip_speed.speed_ramp.value",
                                   keyframe.value.num, keyframe.value.den,
                                   message))
                return Fail(errorName, message,
                            message.empty()
                                ? "'set_clip_speed.speed_ramp.value' is "
                                  "required"
                                : message);
            std::string interpolation;
            if (!ReadString(item, "interpolation", "set_clip_speed.speed_ramp",
                            false, "linear", interpolation, message))
                return Fail(errorName, message, message);
            if (!ParseKeyframeInterpolationName(interpolation,
                                                keyframe.interpolation) ||
                keyframe.interpolation == KeyframeInterpolation::CubicBezier)
                return Fail(errorName, message,
                            "'set_clip_speed.speed_ramp.interpolation' must "
                            "be 'linear' or 'hold': the source position is "
                            "the integral of the speed, and only those two "
                            "keep it exact; write a smooth ramp with more "
                            "linear keys");
            op.ramp.push_back(keyframe);
        }
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// R1 -- docs/roadmap/REEL-2026-09.md. A complete framing, like
// set_clip_audio's complete audible state: what the caller leaves out is
// the default, not "unchanged". A half-set framing read back later would
// be a rectangle nobody chose.
bool DispatchSetClipFraming(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_ids",  "mode",       "scale",    "position_x", "position_y",
        "crop_left", "crop_right", "crop_top", "crop_bottom"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_framing", message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    if (!ReadIdArray(args, "clip_ids", "set_clip_framing", resolver, clipIds,
                     message))
        return Fail(errorName, message, message);
    if (clipIds.empty())
        return Fail(errorName, message,
                    "'set_clip_framing.clip_ids' must contain at least one "
                    "clip");
    ClipFraming framing;
    std::string mode;
    if (!ReadString(args, "mode", "set_clip_framing", false, "fit", mode,
                    message))
        return Fail(errorName, message, message);
    if (mode == "fill") {
        framing.mode = FramingMode::Fill;
    } else if (mode != "fit") {
        return Fail(errorName, message,
                    "'set_clip_framing.mode' must be 'fit' or 'fill'");
    }
    const auto readFraction = [&](const char* key, EffectParamValue& value) {
        const Value* field = args.Find(key);
        if (field == nullptr) return true;
        return ReadFractionValue(*field, std::string("set_clip_framing.") + key,
                                 value.num, value.den, message);
    };
    if (!readFraction("scale", framing.scale) ||
        !readFraction("position_x", framing.position_x) ||
        !readFraction("position_y", framing.position_y) ||
        !readFraction("crop_left", framing.crop_left) ||
        !readFraction("crop_right", framing.crop_right) ||
        !readFraction("crop_top", framing.crop_top) ||
        !readFraction("crop_bottom", framing.crop_bottom))
        return Fail(errorName, message, message);
    SetClipFramingOperation op;
    for (const Ulid& clipId : clipIds) op.clips.push_back({clipId, framing});
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// Out of lot -- the author, 29 October 2026: Resolve's Enable/Disable Clip, the
// window's D. Every named clip takes the same state, in one undo step; the
// linked sound is a clip of its own, named or left as it plays.
bool DispatchSetClipEnabled(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids", "enabled"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_enabled", message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    bool enabled = true;
    if (args.Find("enabled") == nullptr)
        return Fail(errorName, message,
                    "'set_clip_enabled.enabled' is required: false disables "
                    "the clips, true enables them");
    if (!ReadIdArray(args, "clip_ids", "set_clip_enabled", resolver, clipIds,
                     message) ||
        !ReadBool(args, "enabled", "set_clip_enabled", true, enabled, message))
        return Fail(errorName, message, message);
    if (clipIds.empty())
        return Fail(errorName, message,
                    "'set_clip_enabled.clip_ids' must contain at least one "
                    "clip");
    SetClipEnabledOperation op;
    for (const Ulid& clipId : clipIds) op.clips.push_back({clipId, enabled});
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// R2 -- docs/roadmap/REEL-2026-09.md. The whole animation of a clip, like
// set_clip_framing's whole framing and for the same reason: what the caller
// leaves out is cleared, not kept. Adding a key, moving one and deleting
// one are therefore the same call, and the undo of each is byte-exact.
//
// Keyframe ids are the caller's when it gives them, as Keyframes.h has
// always required, because a journal line that replays has to name the same
// keys. When it does not, one is generated per clip: ids live in the
// document's single ID space, so the same list laid on thirty shots cannot
// carry the same thirty ids.
bool DispatchSetClipAnimation(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_ids", "animation"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_animation", message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    if (!ReadIdArray(args, "clip_ids", "set_clip_animation", resolver, clipIds,
                     message))
        return Fail(errorName, message, message);
    if (clipIds.empty())
        return Fail(errorName, message,
                    "'set_clip_animation.clip_ids' must contain at least one "
                    "clip");
    std::vector<ClipParameterAnimation> animation;
    bool explicitIds = false;
    const Value* entries = args.Find("animation");
    if (entries != nullptr && !entries->IsNull()) {
        if (!entries->IsArray())
            return Fail(errorName, message,
                        "'set_clip_animation.animation' must be an array of "
                        "parameter tracks");
        for (const Value& entry : entries->AsArray()) {
            if (!entry.IsObject())
                return Fail(errorName, message,
                            "'set_clip_animation.animation' entries must be "
                            "objects");
            for (const auto& key : entry.AsObject()) {
                if (key.first != "parameter" && key.first != "keyframes")
                    return Fail(errorName, message,
                                "'set_clip_animation.animation' has unknown "
                                "key '" +
                                    key.first + "'");
            }
            ClipParameterAnimation track;
            std::string parameter;
            if (!ReadString(entry, "parameter", "set_clip_animation.animation",
                            true, "", parameter, message))
                return Fail(errorName, message, message);
            if (!ParseClipAnimatedParameterName(parameter, track.parameter))
                return Fail(errorName, message,
                            "'set_clip_animation.animation.parameter' must be "
                            "one of opacity, framing.scale, "
                            "framing.position_x, framing.position_y");
            const Value* keys = entry.Find("keyframes");
            if (keys == nullptr || !keys->IsArray())
                return Fail(errorName, message,
                            "'set_clip_animation.animation.keyframes' must be "
                            "an array");
            for (const Value& item : keys->AsArray()) {
                if (!item.IsObject())
                    return Fail(errorName, message,
                                "'set_clip_animation.animation.keyframes' "
                                "entries must be objects");
                for (const auto& key : item.AsObject()) {
                    if (key.first != "id" && key.first != "time" &&
                        key.first != "value" && key.first != "interpolation" &&
                        key.first != "easing")
                        return Fail(errorName, message,
                                    "'set_clip_animation.animation.keyframes' "
                                    "has unknown key '" +
                                        key.first + "'");
                }
                ClipKeyframe keyframe;
                if (!ReadString(item, "id", "set_clip_animation.keyframes",
                                false, "", keyframe.id, message))
                    return Fail(errorName, message, message);
                if (!keyframe.id.empty()) explicitIds = true;
                if (!ReadTime(item, "time", "set_clip_animation.keyframes",
                              true, keyframe.time, message))
                    return Fail(errorName, message, message);
                const Value* value = item.Find("value");
                if (value == nullptr ||
                    !ReadFractionValue(
                        *value, "set_clip_animation.keyframes.value",
                        keyframe.value.num, keyframe.value.den, message))
                    return Fail(errorName, message,
                                message.empty()
                                    ? "'set_clip_animation.keyframes.value' is "
                                      "required"
                                    : message);
                std::string interpolation;
                if (!ReadString(item, "interpolation",
                                "set_clip_animation.keyframes", false, "linear",
                                interpolation, message))
                    return Fail(errorName, message, message);
                if (!ParseKeyframeInterpolationName(interpolation,
                                                    keyframe.interpolation))
                    return Fail(errorName, message,
                                "'set_clip_animation.keyframes.interpolation' "
                                "must be 'hold', 'linear' or 'cubic-bezier'");
                const Value* easing = item.Find("easing");
                if (easing != nullptr && !easing->IsNull()) {
                    if (!easing->IsArray() || easing->AsArray().size() != 4)
                        return Fail(errorName, message,
                                    "'set_clip_animation.keyframes.easing' is "
                                    "the four cubic-bezier control points, as "
                                    "exact fractions");
                    EffectParamValue* points[] = {
                        &keyframe.easing_x1, &keyframe.easing_y1,
                        &keyframe.easing_x2, &keyframe.easing_y2};
                    for (size_t index = 0; index < 4; ++index) {
                        if (!ReadFractionValue(
                                easing->AsArray()[index],
                                "set_clip_animation.keyframes.easing",
                                points[index]->num, points[index]->den,
                                message))
                            return Fail(errorName, message, message);
                    }
                }
                track.keyframes.push_back(keyframe);
            }
            animation.push_back(std::move(track));
        }
    }
    if (explicitIds && clipIds.size() > 1)
        return Fail(errorName, message,
                    "'set_clip_animation' cannot give the same keyframe ids "
                    "to several clips: leave 'id' out and one is generated "
                    "per clip, or make one call per clip");
    SetClipAnimationOperation op;
    for (const Ulid& clipId : clipIds) {
        std::vector<ClipParameterAnimation> forClip = animation;
        for (ClipParameterAnimation& track : forClip)
            for (ClipKeyframe& keyframe : track.keyframes)
                if (keyframe.id.empty()) keyframe.id = GenerateUlid();
        op.clips.push_back({clipId, std::move(forClip)});
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// CA3 -- docs/roadmap/CADRAGE-2026-10.md. A framing that follows its
// subject: the faces read every `every_frames` frames through the backend's
// finder (Vision on macOS; refused by name without one), or handed in as
// `detections`, which is how the rule is driven without Vision -- a test, a
// Linux session, a trajectory another tool measured. The result is R1 and
// R2's own operations in one batch, so it undoes in one step and replays
// without a finder.
bool DispatchTrackSubjectFraming(McpBackend& backend,
                                 const IdResolver& resolver, const Value& args,
                                 std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    const std::string tool = "track_subject_framing";
    static const std::vector<std::string> kAllowed = {
        "clip_ids",         "every_frames", "detections", "scale",
        "dead_zone_x",      "dead_zone_y",  "settle",     "max_speed",
        "max_acceleration", "target_x",     "target_y",   "gate",
        "tolerance",        "bridge_gap",   "hint"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, clipIds, message))
        return Fail(errorName, message, message);
    if (clipIds.empty())
        return Fail(errorName, message,
                    "'" + tool + ".clip_ids' must contain at least one clip");
    if (clipIds.size() * 2 > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'" + tool + "' follows at most " +
                        std::to_string(kMaxBatchOperations / 2) +
                        " clips in one call");
    int64_t everyFrames = 5;
    if (!ReadInt64(args, "every_frames", tool, false, 5, everyFrames, message))
        return Fail(errorName, message, message);
    if (everyFrames < 1 || everyFrames > 250)
        return Fail(errorName, message,
                    "'" + tool + ".every_frames' must lie in [1, 250]");
    SubjectTrackingSettings settings;
    std::optional<EffectParamValue> scale;
    const auto readFraction = [&](const char* key, EffectParamValue& value) {
        const Value* field = args.Find(key);
        if (field == nullptr || field->IsNull()) return true;
        return ReadFractionValue(*field, tool + "." + key, value.num, value.den,
                                 message);
    };
    EffectParamValue scaleValue{1, 1};
    if (args.Find("scale") != nullptr) {
        if (!readFraction("scale", scaleValue))
            return Fail(errorName, message, message);
        scale = scaleValue;
    }
    if (!readFraction("dead_zone_x", settings.dead_zone_x) ||
        !readFraction("dead_zone_y", settings.dead_zone_y) ||
        !readFraction("settle", settings.settle) ||
        !readFraction("max_speed", settings.max_speed) ||
        !readFraction("max_acceleration", settings.max_acceleration) ||
        !readFraction("target_x", settings.target_x) ||
        !readFraction("target_y", settings.target_y) ||
        !readFraction("gate", settings.gate) ||
        !readFraction("tolerance", settings.tolerance) ||
        !ReadTime(args, "bridge_gap", tool, false, settings.bridge_gap,
                  message))
        return Fail(errorName, message, message);
    if (const Value* hint = args.Find("hint");
        hint != nullptr && !hint->IsNull()) {
        message.clear();
        if (!hint->IsObject() ||
            !CheckKnownKeys(*hint, {"x", "y"}, tool + ".hint", message))
            return Fail(errorName, message,
                        message.empty() ? "'" + tool +
                                              ".hint' must be an object with "
                                              "'x' and 'y'"
                                        : message);
        const Value* x = hint->Find("x");
        const Value* y = hint->Find("y");
        if (x == nullptr || y == nullptr ||
            !ReadFractionValue(*x, tool + ".hint.x", settings.hint_x.num,
                               settings.hint_x.den, message) ||
            !ReadFractionValue(*y, tool + ".hint.y", settings.hint_y.num,
                               settings.hint_y.den, message))
            return Fail(errorName, message,
                        message.empty()
                            ? "'" + tool + ".hint' needs 'x' and 'y'"
                            : message);
        settings.has_hint = true;
    }
    if (!SubjectTrackingSettingsAreValid(settings, message))
        return Fail(errorName, message, "'" + tool + "': " + message);

    // The detections handed in, by clip.
    std::map<Ulid, std::vector<SubjectSample>> given;
    if (const Value* detections = args.Find("detections");
        detections != nullptr && !detections->IsNull()) {
        if (!detections->IsArray())
            return Fail(errorName, message,
                        "'" + tool +
                            ".detections' must be an array of {clip_id, "
                            "samples}");
        const std::string path = tool + ".detections";
        for (const Value& entry : detections->AsArray()) {
            if (!CheckKnownKeys(entry, {"clip_id", "samples"}, path, message))
                return Fail(errorName, message, message);
            Ulid clipId;
            if (!ReadId(entry, "clip_id", path, resolver, true, clipId,
                        message))
                return Fail(errorName, message, message);
            const Value* samples = entry.Find("samples");
            if (samples == nullptr || !samples->IsArray())
                return Fail(errorName, message,
                            "'" + path + ".samples' must be an array");
            std::vector<SubjectSample>& list = given[clipId];
            const std::string itemPath = path + ".samples";
            for (const Value& item : samples->AsArray()) {
                if (!CheckKnownKeys(item, {"time", "faces"}, itemPath, message))
                    return Fail(errorName, message, message);
                SubjectSample sample;
                if (!ReadTime(item, "time", itemPath, true, sample.time,
                              message))
                    return Fail(errorName, message, message);
                const Value* faces = item.Find("faces");
                if (faces != nullptr && !faces->IsNull()) {
                    if (!faces->IsArray())
                        return Fail(errorName, message,
                                    "'" + itemPath +
                                        ".faces' must be an array of "
                                        "regions");
                    for (const Value& face : faces->AsArray()) {
                        Value holder = Value::MakeObject();
                        holder.Set("face", face);
                        ColorMeasureRegion region;
                        if (!ReadColorRegion(holder, "face", itemPath, region,
                                             message))
                            return Fail(errorName, message, message);
                        sample.faces.push_back(region);
                    }
                }
                list.push_back(std::move(sample));
            }
        }
        for (const auto& entry : given)
            if (std::find(clipIds.begin(), clipIds.end(), entry.first) ==
                clipIds.end())
                return Fail(errorName, message,
                            "'" + path + "' names clip '" + entry.first +
                                "', which clip_ids does not");
    }

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    std::vector<Operation> operations;
    Value clips = Value::MakeArray();
    for (const Ulid& clipId : clipIds) {
        if (document.FindClip(clipId) == nullptr) {
            errorName = EditErrorName(EditError::UnknownClip);
            message = "unknown clip_id '" + clipId + "'";
            return false;
        }
        std::vector<SubjectSample> samples;
        const auto found = given.find(clipId);
        const bool read = found == given.end();
        if (read) {
            if (!backend.SampleClipFaces(clipId, everyFrames, samples,
                                         message)) {
                errorName = message.find("no face finder") != std::string::npos
                                ? EditErrorName(EditError::InvalidOperation)
                                : "IoError";
                return false;
            }
        } else {
            samples = found->second;
        }
        std::vector<Operation> forClip;
        SubjectTrackingResult result;
        bool modeChanged = false;
        if (!BuildSubjectTrackingOperations(document, clipId, samples, settings,
                                            scale, GenerateUlid, forClip,
                                            result, modeChanged, message)) {
            errorName = EditErrorName(EditError::InvalidOperation);
            return false;
        }
        for (Operation& operation : forClip)
            operations.push_back(std::move(operation));
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(clipId));
        item.Set("faces_from",
                 Value::MakeString(read ? "finder" : "detections"));
        item.Set("mode_set_to_fill", Value::MakeBool(modeChanged));
        Value tracking;
        std::string parseError;
        if (Value::Parse(SerializeSubjectTrackingResult(result), tracking,
                         parseError))
            item.Set("tracking", std::move(tracking));
        clips.Push(std::move(item));
    }
    std::string applied;
    if (!backend.ApplyOperation(
            Operation{BatchOperation{"cadrage suivi", std::move(operations)}},
            applied, errorName, message))
        return false;
    Value reply = Value::MakeObject();
    reply.Set("applied", Value::MakeBool(true));
    reply.Set("clips", std::move(clips));
    Value result;
    std::string parseError;
    if (Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

// PA7 -- docs/roadmap/PARITE-2026-11.md. A handheld shot steadied: the
// movement between every frame and the next is read from the rush, and the
// correction is laid as R1 and R2's own operations in one batch, as a
// followed framing is. It undoes in one step and replays without decoding.
bool DispatchStabilizeClip(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string tool = "stabilize_clip";
    static const std::vector<std::string> kAllowed = {
        "clip_ids", "smoothing", "scale", "max_scale", "tolerance"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    std::vector<Ulid> clipIds;
    if (!ReadIdArray(args, "clip_ids", tool, resolver, clipIds, message))
        return Fail(errorName, message, message);
    if (clipIds.empty())
        return Fail(errorName, message,
                    "'" + tool + ".clip_ids' must contain at least one clip");
    if (clipIds.size() * 2 > kMaxBatchOperations)
        return Fail(errorName, message,
                    "'" + tool + "' steadies at most " +
                        std::to_string(kMaxBatchOperations / 2) +
                        " clips in one call");
    StabilizationSettings settings;
    std::optional<EffectParamValue> scale;
    const auto readFraction = [&](const char* key, EffectParamValue& value) {
        const Value* field = args.Find(key);
        if (field == nullptr || field->IsNull()) return true;
        return ReadFractionValue(*field, tool + "." + key, value.num, value.den,
                                 message);
    };
    EffectParamValue scaleValue{1, 1};
    if (args.Find("scale") != nullptr && !args.Find("scale")->IsNull()) {
        if (!readFraction("scale", scaleValue))
            return Fail(errorName, message, message);
        scale = scaleValue;
    }
    if (!readFraction("max_scale", settings.max_scale) ||
        !readFraction("tolerance", settings.tolerance) ||
        !ReadTime(args, "smoothing", tool, false, settings.smoothing, message))
        return Fail(errorName, message, message);
    if (!StabilizationSettingsAreValid(settings, message))
        return Fail(errorName, message, "'" + tool + "': " + message);

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    std::vector<Operation> operations;
    Value clips = Value::MakeArray();
    for (const Ulid& clipId : clipIds) {
        if (document.FindClip(clipId) == nullptr) {
            errorName = EditErrorName(EditError::UnknownClip);
            message = "unknown clip_id '" + clipId + "'";
            return false;
        }
        std::vector<StabilizationSample> motion;
        if (!backend.SampleClipMotion(clipId, motion, message)) {
            // What the clip is, against what the disk gave.
            const bool refused =
                message.find("has no rush") != std::string::npos ||
                message.find("speed other than") != std::string::npos ||
                message.find("cadence") != std::string::npos;
            errorName = refused ? EditErrorName(EditError::InvalidOperation)
                                : "IoError";
            return false;
        }
        std::vector<Operation> forClip;
        StabilizationResult result;
        bool modeChanged = false;
        if (!BuildStabilizationOperations(document, clipId, motion, settings,
                                          scale, GenerateUlid, forClip, result,
                                          modeChanged, message)) {
            errorName = EditErrorName(EditError::InvalidOperation);
            return false;
        }
        for (Operation& operation : forClip)
            operations.push_back(std::move(operation));
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(clipId));
        item.Set("mode_set_to_fill", Value::MakeBool(modeChanged));
        Value stabilization;
        std::string parseError;
        if (Value::Parse(SerializeStabilizationResult(result), stabilization,
                         parseError))
            item.Set("stabilization", std::move(stabilization));
        clips.Push(std::move(item));
    }
    std::string applied;
    if (!backend.ApplyOperation(
            Operation{BatchOperation{"stabilisation", std::move(operations)}},
            applied, errorName, message))
        return false;
    Value reply = Value::MakeObject();
    reply.Set("applied", Value::MakeBool(true));
    reply.Set("corrected", Value::MakeString("translation"));
    reply.Set("clips", std::move(clips));
    Value result;
    std::string parseError;
    if (Value::Parse(applied, result, parseError))
        reply.Set("result", std::move(result));
    resultJson = reply.Dump();
    return true;
}

// R1 -- criterion 3, and guard rail 4: the engine measures, the author
// does not. For one clip at one instant it reports the rectangle its
// picture occupies in the sequence, the source region that fills it, and
// the frame edges it leaves uncovered -- the black bars, in pixels.
// Read-only: it changes nothing and it is the only way to know what a
// framing did without exporting it.
bool DispatchListClipFraming(McpBackend& backend, const IdResolver& resolver,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "offset"};
    if (!CheckKnownKeys(args, kAllowed, "list_clip_framing", message))
        return Fail(errorName, message, message);
    Ulid clipId;
    if (!ReadId(args, "clip_id", "list_clip_framing", resolver, true, clipId,
                message))
        return Fail(errorName, message, message);
    RationalTime offset{0, 1};
    if (!ReadTime(args, "offset", "list_clip_framing", false, offset, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        errorName = EditErrorName(EditError::UnknownClip);
        message = "unknown clip_id '" + clipId + "'";
        return false;
    }
    if (offset.value < 0 || offset > clip->duration) {
        return Fail(errorName, message,
                    "'list_clip_framing.offset' must land inside the clip");
    }
    // R2 -- docs/roadmap/REEL-2026-09.md. At that instant, which on an
    // animated clip is not the same rectangle as at any other: the values
    // are sampled by the one function every renderer calls, so what this
    // tool reports is what the monitor shows and the export writes.
    const MediaRate sequenceRate = document.sequence.frame_rate;
    ClipSampledParameters sampled;
    sampled.opacity = clip->opacity;
    sampled.framing = clip->framing;
    if (ClipHasAnimation(*clip) &&
        !SampleClipParameters(
            *clip, offset.to_frames(sequenceRate.num, sequenceRate.den),
            sequenceRate, sampled, message)) {
        return Fail(errorName, message, "clip '" + clipId + "': " + message);
    }
    ClipFramingGeometry geometry;
    int64_t framingWidth = 0;
    int64_t framingHeight = 0;
    if (!ClipDisplayedSize(document, *clip, framingWidth, framingHeight,
                           message) ||
        !ResolveClipFraming(sampled.framing, framingWidth, framingHeight,
                            document.sequence.width, document.sequence.height,
                            geometry, message)) {
        return Fail(errorName, message, "clip '" + clipId + "': " + message);
    }
    int64_t displayedWidth = 0;
    int64_t displayedHeight = 0;
    if (!ClipDisplayedSize(document, *clip, displayedWidth, displayedHeight,
                           message))
        return Fail(errorName, message, message);
    std::ostringstream output;
    output << "{\"ok\":true,\"clip_id\":\"" << Esc(clipId)
           << "\",\"source_id\":\"" << Esc(clip->source_id)
           << "\",\"offset\":{\"value\":" << offset.value
           << ",\"rate\":" << offset.rate
           << "},\"sequence\":{\"width\":" << document.sequence.width
           << ",\"height\":" << document.sequence.height
           << "},\"displayed_source\":{\"width\":" << displayedWidth
           << ",\"height\":" << displayedHeight << "},\"mode\":\""
           << (sampled.framing.mode == FramingMode::Fill ? "fill" : "fit")
           << "\",\"geometry\":" << SerializeClipFramingGeometry(geometry);
    // R2 -- and the values themselves, as exact fractions, so an agent can
    // read back what its curve is worth at that frame without evaluating
    // the curve itself (guard rail 4: the engine measures).
    if (ClipHasAnimation(*clip)) {
        output << ",\"sampled\":{\"scale\":{\"num\":"
               << sampled.framing.scale.num
               << ",\"den\":" << sampled.framing.scale.den
               << "},\"position_x\":{\"num\":" << sampled.framing.position_x.num
               << ",\"den\":" << sampled.framing.position_x.den
               << "},\"position_y\":{\"num\":" << sampled.framing.position_y.num
               << ",\"den\":" << sampled.framing.position_y.den
               << "},\"opacity\":{\"num\":" << sampled.opacity.num
               << ",\"den\":" << sampled.opacity.den << "}}";
    }
    // R3 -- which source frame that rectangle shows, through the speed
    // rule, so one answer covers "what is on screen here" entirely.
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source != nullptr && source->rate.num > 0 && source->rate.den > 0) {
        output << ",\"source_frame\":"
               << ClipSourceFrameAt(*clip, offset, source->rate);
    }
    output << '}';
    resultJson = output.str();
    return true;
}

bool DispatchSetClipAudio(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"clip_id", "gain_db",
                                                      "fade_in", "fade_out"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_audio", message))
        return Fail(errorName, message, message);
    SetClipAudioOperation op;
    if (!ReadId(args, "clip_id", "set_clip_audio", resolver, true, op.clip_id,
                message))
        return Fail(errorName, message, message);
    const Value* gain = args.Find("gain_db");
    if (!gain ||
        !ReadFractionValue(*gain, "set_clip_audio.gain_db", op.gain_db.num,
                           op.gain_db.den, message) ||
        !ReadTime(args, "fade_in", "set_clip_audio", true, op.fade_in,
                  message) ||
        !ReadTime(args, "fade_out", "set_clip_audio", true, op.fade_out,
                  message)) {
        return Fail(errorName, message, message);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

}  // namespace

void RegisterClipTools(const McpToolAdder& add) {
    add("set_clip_link",
        "Link two clips as an A/V pair, or clear an existing link between "
        "them.",
        SchemaBuilder()
            .Field("first_clip_id", IdSchema("First clip."), true)
            .Field("second_clip_id", IdSchema("Second clip."), true)
            .Field(
                "linked",
                BoolSchema("true to link (default), false to clear the link."),
                false)
            .Build("set_clip_link arguments"),
        DispatchSetClipLink);

    add("split_link_groups",
        "Split each link group of the timeline that binds clips at several "
        "places -- the shape a cut left before 30 October 2026, which made "
        "one click take a whole block -- one group per place, as one undo "
        "step. Members that overlap in time stay together (a picture and "
        "its sounds, a J or L cut); a clip alone at its place loses its "
        "link. The window does it when it opens a timeline. Answers groups, "
        "places, cleared, left_alone and the delta of each step.",
        SchemaBuilder()
            .Field("preview",
                   BoolSchema("true: count what would change, change "
                              "nothing; default false."),
                   false)
            .Build("split_link_groups arguments"),
        DispatchSplitLinkGroups);

    // F1.3 -- wheels and curves: the knobs, bounds and formulas an agent
    // needs, stated once in ColorEffects.h and enforced by
    // ValidateColorEffect; the bounds below are read from its constants.
    const std::string wheelLimit = std::to_string(kColorWheelLimit);

    add("set_clip_effects",
        "Replace a clip's entire color-grade effect stack; stages apply in "
        "order. Scalar stages take one exact param, not range-checked: "
        "color.exposure (amount, stops), color.contrast, color.saturation, "
        "color.vibrance, color.tint, color.highlights, color.shadows (amount, "
        "0 neutral), color.temperature (kelvin of the scene's light: the "
        "media's as-shot balance is neutral, 6500 if unknown; lower cools), "
        "color.hue (degrees about the grey axis, as measure_color's hue angle, "
        "positive from red toward yellow). color.contrast also takes pivot, in "
        "[0, 1], the value it holds still: out = (in - pivot) * (1 + amount) + "
        "pivot, 0.4136 (middle grey in ACEScct) when absent. color.wheels: "
        "params lift_r, lift_g, lift_b, lift_master and likewise gamma_*, "
        "gain_* and offset_*, each in [-" +
            wheelLimit + ", " + wheelLimit +
            "], absent = 0 = neutral; per channel L = lift_c + lift_master, "
            "K = 2^(gain_c + gain_master), G = 2^(gamma_c + gamma_master), "
            "O = offset_c + offset_master, out = ((K - L) * in + L + O)^(1/G) "
            "where positive: the offset moves every value of a channel by "
            "the same amount. color.curves: a "
            "master curve on R, G and B first, then r, g and b; point i of a "
            "channel is <channel>_x<i> with <channel>_y<i>, i from 0 without "
            "a gap, none or 2 to " +
            std::to_string(kMaxColorCurvePoints) +
            " points per channel, x strictly increasing, x and y in [0, 1]; "
            "monotone cubic that never overshoots, straight past the end "
            "points. color.hue_curves: six curves, points <curve>_x<i> and "
            "<curve>_y<i> as above, y = 1/2 neutral, none = unchanged: "
            "hue_hue, hue_sat, hue_lum read along the hue (x = measure_color's "
            "hue angle / 360, in [0, 1), the curve goes round), lum_sat along "
            "the luma, sat_sat and sat_lum along the saturation (0 grey, 1 a "
            "pure primary); *_hue turns the hue by (y - 1/2) of a turn, *_sat "
            "scales the saturation by 2y, *_lum adds (y - 1/2) to the luma. To "
            "change one colour, put its hue between two points at 1/2. "
            "color.resolve_primaries: an imported Resolve node. Stages act on "
            "ACEScct code values under colour management (on the linear value "
            "with a linear working space) and on the display signal without "
            "it.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to grade."), true)
            .Field("effects",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"type\":" +
                           StringSchema(
                               "Dotted knob family, e.g. 'color.exposure'.") +
                           ",\"params\":{\"type\":\"object\",\"description\":"
                           "\"Knob name to exact num/den value.\","
                           "\"additionalProperties\":" +
                           std::string(kFractionSchemaText) +
                           "}},\"required\":[\"type\"],"
                           "\"additionalProperties\":false}",
                       ""),
                   true)
            .Build("set_clip_effects arguments"),
        DispatchSetClipEffects);

    // GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md.
    add("paste_clip_grade",
        "Copy one clip's grade onto others, as a colourist copies and pastes a "
        "grade: the whole color.* stack of from_clip_id replaces the stack of "
        "each clip in clip_ids, knobs as they are (a color.temperature is the "
        "same move from each clip's own as-shot balance).",
        SchemaBuilder()
            .Field("from_clip_id", IdSchema("Clip whose grade is copied."),
                   true)
            .Field("clip_ids", IdArraySchema("Clips that take it, 1 to 256."),
                   true)
            .Build("paste_clip_grade arguments"),
        DispatchPasteClipGrade);

    add("set_clip_opacity",
        "Set a video clip's compositing opacity as an exact fraction.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Video clip to update."), true)
            .Field("opacity", kFractionSchemaText, true)
            .Build("set_clip_opacity arguments"),
        DispatchSetClipOpacity);

    add("set_clip_enabled",
        "Disable or enable clips, as Resolve's Disable Clip. A disabled clip "
        "keeps its place and settings and counts in edits and duration, but "
        "every render, analysis and check plays it as a hole (what lies under "
        "it, or black and silence). Disabling a picture leaves its linked "
        "sound playing: name the sound clip too to silence it. enabled true "
        "gives the film back exactly. describe and the delta write "
        "\"enabled\": false on a disabled clip.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Clips to disable or enable, all to the same "
                                 "state, on any track."),
                   true)
            .Field("enabled",
                   BoolSchema("false disables the clips, true enables them."),
                   true)
            .Build("set_clip_enabled arguments"),
        DispatchSetClipEnabled);

    // EF3 -- docs/roadmap/EFFETS-2026-10.md.
    add("set_clip_blur",
        "Blur a video clip's picture with a Gaussian blur. 'blur' is its "
        "standard deviation (sigma) in sequence pixels, an exact fraction from "
        "0 (none: removes the blur) to 200; 10 softens a 1080p shot into a "
        "background, 40 leaves only its shapes.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Video clip to blur."), true)
            .Field("blur", kFractionSchemaText, true)
            .Build("set_clip_blur arguments"),
        DispatchSetClipBlur);

    // PA8 -- docs/roadmap/PARITE-2026-11.md.
    add("set_clip_mask",
        "Cut a video clip's picture out with a shape: inside is kept, outside "
        "lets the tracks below show ('inverted' reverses it). For a split "
        "screen, a picture in a circle, a soft window. The shape is placed on "
        "the clip's picture, not on the frame: centre, width and height are "
        "fractions of the displayed rush (crops ignored), so the mask follows "
        "the picture when its framing moves or zooms. An 'ellipse' is the one "
        "inscribed in that rectangle. 'feather' softens the edge inwards, "
        "from 0 (hard) to 1 (a fade from the edge to the centre). The mask is "
        "a coverage the colours never see, mixed with what is below as an "
        "opacity is. shape 'none' removes it. list_clip_framing says where "
        "the picture lies; read_frame shows the result.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Video clip to mask."), true)
            .Field("shape",
                   EnumSchema({"rectangle", "ellipse", "none"},
                              "The shape; 'none' removes the mask."),
                   true)
            .Field("center_x",
                   FractionSchema("Centre across, as a fraction of the "
                                  "picture's width (default 1/2)."),
                   false)
            .Field("center_y",
                   FractionSchema("Centre down, as a fraction of the "
                                  "picture's height (default 1/2)."),
                   false)
            .Field("width",
                   FractionSchema("Width, as a fraction of the picture's "
                                  "(default 1/1; 1/2 keeps half of it)."),
                   false)
            .Field("height",
                   FractionSchema("Height, as a fraction of the picture's "
                                  "(default 1/1)."),
                   false)
            .Field("feather",
                   FractionSchema("Softness of the edge, inwards: 0 hard "
                                  "(default), 1 a fade to the centre."),
                   false)
            .Field("inverted",
                   BoolSchema("true keeps what is outside the shape."), false)
            .Build("set_clip_mask arguments"),
        DispatchSetClipMask);

    add("set_clip_speed",
        "Set a clip's speed, constant or ramped. 'speed' is one exact "
        "fraction: 1/2 is a half-speed slow motion, 2 plays twice as fast; the "
        "clip keeps its timeline duration and reads duration*speed of its "
        "source, no frame is interpolated, and on a linked clip the whole "
        "group follows, sound stretched with its pitch kept. 'speed_ramp' "
        "instead gives keys of speed over the clip's own time, the first and "
        "last held beyond them: the clip reads the integral of that curve.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to update."), true)
            .Field("speed", kFractionSchemaText, false)
            // R3, criterion 4 -- the interpolation is a closed set of two,
            // declared like set_clip_animation's (PRD15).
            .Field("speed_ramp",
                   ArraySchema(
                       std::string("{\"type\":\"object\",\"properties\":{"
                                   "\"time\":") +
                           kTimeSchemaText + ",\"value\":" +
                           kFractionSchemaText + ",\"interpolation\":" +
                           EnumSchema({KeyframeInterpolationName(
                                           KeyframeInterpolation::Linear),
                                       KeyframeInterpolationName(
                                           KeyframeInterpolation::Hold)},
                                      "") +
                           ",\"id\":{\"type\":\"string\"}},\"required\":"
                           "[\"time\",\"value\"]}",
                       "Keys of speed, in increasing time: {\"time\": "
                       "{\"value\", \"rate\"} in the clip's own time, "
                       "\"value\": the speed as {\"num\", \"den\"} (0 holds "
                       "the picture), \"interpolation\": \"linear\" or "
                       "\"hold\" up to the next key, \"id\": optional, "
                       "generated when absent}."),
                   false)
            .Build("set_clip_speed arguments"),
        DispatchSetClipSpeed);

    add("set_clip_framing",
        "Set how one or more video clips sit in the sequence frame. This is a "
        "complete framing: what you leave out is set to its default, not left "
        "as it was. Applied in the order rotation, crop, mode, scale, "
        "position; list_clip_framing reports the rectangle it produces.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Video clips to frame, all with the same "
                                 "framing."),
                   true)
            // PRD15 -- a closed set (DispatchSetClipFraming), so declared:
            // an enumeration takes any case (McpArguments.h).
            .Field("mode",
                   EnumSchema({"fit", "fill"},
                              "fit (default): the whole picture inside the "
                              "frame; fill: the frame covered, the overflow "
                              "cut."),
                   false)
            .Field("scale",
                   FractionSchema("Multiplies the mode's scale (default 1)."),
                   false)
            .Field("position_x",
                   FractionSchema("Moves the centre by this fraction of the "
                                  "frame, positive right (default 0)."),
                   false)
            .Field("position_y",
                   FractionSchema("Moves the centre by this fraction of the "
                                  "frame, positive down (default 0)."),
                   false)
            .Field("crop_left",
                   FractionSchema("Fraction of the source's left edge thrown "
                                  "away (default 0); likewise the other "
                                  "crops."),
                   false)
            .Field("crop_right", kFractionSchemaText, false)
            .Field("crop_top", kFractionSchemaText, false)
            .Field("crop_bottom", kFractionSchemaText, false)
            .Build("set_clip_framing arguments"),
        DispatchSetClipFraming);

    add("set_clip_animation",
        "Animate a video clip's opacity or its framing over the clip's own "
        "frames. It replaces the clip's whole animation: what you leave out is "
        "cleared, and an empty or absent 'animation' removes every key. Keys "
        "stay on the frames you put them on when the clip is moved, trimmed or "
        "cut; list_clip_framing reports what a frame is worth.",
        SchemaBuilder()
            .Field("clip_ids",
                   IdArraySchema("Video clips to animate, all with the same "
                                 "keys."),
                   true)
            // PRD15 -- the parameter and the interpolation are closed sets,
            // named by the engine (ClipAnimation.h) and declared so they take
            // any case; the rest of a track's shape stays in the description,
            // and the dispatcher still refuses a key it does not know.
            .Field("animation",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"parameter\":" +
                           EnumSchema(
                               {ClipAnimatedParameterName(
                                    ClipAnimatedParameter::Opacity),
                                ClipAnimatedParameterName(
                                    ClipAnimatedParameter::FramingScale),
                                ClipAnimatedParameterName(
                                    ClipAnimatedParameter::FramingPositionX),
                                ClipAnimatedParameterName(
                                    ClipAnimatedParameter::FramingPositionY)},
                               "") +
                           ",\"keyframes\":{\"type\":\"array\",\"items\":{"
                           "\"type\":\"object\",\"properties\":{"
                           "\"interpolation\":" +
                           EnumSchema({KeyframeInterpolationName(
                                           KeyframeInterpolation::Hold),
                                       KeyframeInterpolationName(
                                           KeyframeInterpolation::Linear),
                                       KeyframeInterpolationName(
                                           KeyframeInterpolation::CubicBezier)},
                                      "") +
                           "}}}}}",
                       "Parameter tracks. Each is {\"parameter\", "
                       "\"keyframes\": [{\"time\": {\"value\", \"rate\"}, "
                       "\"value\": {\"num\", \"den\"}, \"interpolation\", "
                       "\"easing\": the four cubic-bezier control points as "
                       "exact fractions, \"id\": optional, generated when "
                       "absent}]}."),
                   false)
            .Build("set_clip_animation arguments"),
        DispatchSetClipAnimation);

    // CA3 -- docs/roadmap/CADRAGE-2026-10.md.
    add("track_subject_framing",
        "Reframe video clips so their framing follows the subject's face, "
        "moving like an operator: still while the face is in the dead zone, "
        "then one limited move back. For a 16:9 rush in a 9:16 reel. The clip "
        "is put in 'fill' (scale and crops kept, or 'scale' given) and its "
        "position gets keys, as set_clip_animation would write them; its other "
        "keys are kept. One subject is followed: the face nearest 'hint' at "
        "first, or the largest, then the face nearest its last place; short "
        "holes in the detections are bridged, long ones hold the frame. Use it "
        "on shots where the subject moves; a subject that stays put needs a "
        "fixed set_clip_framing.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Video clips to follow."), true)
            .Field("every_frames",
                   PositiveIntSchema("Read the faces every N frames of the "
                                     "clip, plus its last frame (default "
                                     "5)."),
                   false)
            .Field("detections",
                   ArraySchema(
                       "{\"type\":\"object\",\"properties\":{\"clip_id\":"
                       "{\"type\":\"string\"},\"samples\":{\"type\":"
                       "\"array\",\"items\":{\"type\":\"object\","
                       "\"properties\":{\"time\":" +
                           std::string(kTimeSchemaText) +
                           ",\"faces\":{\"type\":\"array\",\"items\":" +
                           kColorRegionSchema + "}}}}}}",
                       "Faces you measured yourself instead of the finder: "
                       "per clip, samples at times local to the clip (zero "
                       "is its first frame), each with every face found, as "
                       "rectangles of the whole rush picture (rotation "
                       "applied), fractions from the top-left corner. An "
                       "empty 'faces' is a sample where nothing was found."),
                   false)
            .Field("scale", kFractionSchemaText, false)
            .Field("dead_zone_x",
                   FractionSchema("Half-width of the zone the face moves in "
                                  "without the frame moving, of the frame "
                                  "(default 1/10)."),
                   false)
            .Field("dead_zone_y",
                   FractionSchema("The same, down the frame (default 1/10)."),
                   false)
            .Field("settle",
                   FractionSchema("A move ends this close to the "
                                  "target (default 1/50)."),
                   false)
            .Field("max_speed",
                   FractionSchema("Fastest move, in frame widths per second "
                                  "(default 2/5)."),
                   false)
            .Field("max_acceleration",
                   FractionSchema("Acceleration, in frame widths per second "
                                  "squared (default 4/5)."),
                   false)
            .Field("target_x",
                   FractionSchema("Where the face sits across the frame "
                                  "(default 1/2)."),
                   false)
            .Field("target_y",
                   FractionSchema("Where the face sits down the frame "
                                  "(default 2/5)."),
                   false)
            .Field("gate",
                   FractionSchema("Farthest a face may move between two "
                                  "samples and stay the subject, in face "
                                  "widths (default 3/2)."),
                   false)
            .Field("tolerance",
                   FractionSchema("How far the keys may stray "
                                  "from the computed path, of the "
                                  "frame (default 1/1000)."),
                   false)
            .Field("bridge_gap",
                   TimeSchema("Longest hole in the detections bridged "
                              "(default 1 s)."),
                   false)
            .Field("hint",
                   "{\"type\":\"object\",\"description\":\"The point of the "
                   "rush picture nearest the face to follow when several "
                   "are found at first, as fractions.\",\"properties\":"
                   "{\"x\":" +
                       std::string(kFractionSchemaText) +
                       ",\"y\":" + kFractionSchemaText +
                       "},\"additionalProperties\":false}",
                   false)
            .Build("track_subject_framing arguments"),
        DispatchTrackSubjectFraming);

    add("stabilize_clip",
        "Steady handheld video clips. The movement between every frame and "
        "the next is read from the rush, the camera's path is smoothed, and "
        "the difference is written as keys on the framing's position, as "
        "set_clip_animation would write them: the clip is put in 'fill' and "
        "enlarged by the smallest scale that holds the corrections, up to "
        "max_scale; past it the corrections stop at the edge "
        "(frames_at_edge) rather than show a border. Only the translation is "
        "corrected: not the roll, the zoom or the rolling shutter. "
        "shake.before_rms_px and shake.left_rms_px say what it did, in "
        "sequence pixels; a subject that fills the frame and moves is read "
        "as the camera moving. Replaces the clip's position keys, so run it "
        "before track_subject_framing, not after. Refuses a clip with a "
        "speed, and a rush that is not at the sequence's cadence.",
        SchemaBuilder()
            .Field("clip_ids", IdArraySchema("Video clips to steady."), true)
            .Field("smoothing",
                   TimeSchema("How long the camera's path is averaged over, "
                              "each side (the Gaussian's deviation); default "
                              "1/2 s. Longer is steadier and costs more "
                              "zoom."),
                   false)
            .Field("scale",
                   FractionSchema("Scale to give the framing, over what "
                                  "fills the frame; taken as is. Omit to let "
                                  "the tool choose the smallest that holds "
                                  "the corrections."),
                   false)
            .Field("max_scale",
                   FractionSchema("The most the tool may enlarge the "
                                  "picture when it chooses; default 5/4."),
                   false)
            .Field("tolerance",
                   FractionSchema("How far the keys may stray from the "
                                  "computed path, as a fraction of the "
                                  "frame; default 1/5000."),
                   false)
            .Build("stabilize_clip arguments"),
        DispatchStabilizeClip);

    add("list_clip_framing",
        "Report, for one clip at one instant, the rectangle its picture "
        "occupies in the sequence frame, the source region read to fill it, "
        "and the frame edges it leaves uncovered in pixels -- the black bars, "
        "named rather than discovered in the delivery. Read-only.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to measure."), true)
            .Field("offset", kTimeSchemaText, false)
            .Build("list_clip_framing arguments"),
        DispatchListClipFraming);

    add("set_clip_audio",
        "Set an audio clip's exact gain in dB and its fade envelope.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Audio clip to update."), true)
            .Field("gain_db", kFractionSchemaText, true)
            .Field("fade_in", kTimeSchemaText, true)
            .Field("fade_out", kTimeSchemaText, true)
            .Build("set_clip_audio arguments"),
        DispatchSetClipAudio);
}
