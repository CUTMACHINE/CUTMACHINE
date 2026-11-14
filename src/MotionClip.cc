#include "MotionClip.h"

#include "CanonicalHash.h"
#include "MotionDocument.h"

#include <stdexcept>
#include <utility>

namespace {

using mcp_json::Value;

bool Fail(MotionError& error, const std::string& code, const std::string& path,
          const std::string& message) {
    error = {code, path, message};
    return false;
}

std::string Described(const MotionError& error) {
    return error.code + (error.path.empty() ? "" : " at " + error.path) + ": " +
           error.message;
}

}  // namespace

DocumentSource CompositionAsSource(const Ulid& compositionId,
                                   const MotionComposition& composition) {
    DocumentSource source;
    source.id = compositionId;
    source.rate = {composition.fps_numerator, composition.fps_denominator};
    source.duration = MotionFrameTime(composition, composition.duration);
    return source;
}

bool CompositionAsSource(const DocumentComposition& composition,
                         DocumentSource& source, MotionError& error) {
    MotionComposition parsed;
    if (!ParseDocumentComposition(composition, parsed, error)) {
        return false;
    }
    source = CompositionAsSource(composition.id, parsed);
    return true;
}

std::optional<DocumentSource> ClipSource(const Document& document,
                                         const DocumentClip& clip) {
    if (!IsMotionClip(clip)) {
        const DocumentSource* source = document.FindSource(clip.source_id);
        return source != nullptr ? std::optional<DocumentSource>(*source)
                                 : std::nullopt;
    }
    const DocumentComposition* composition =
        document.FindComposition(clip.composition_id);
    DocumentSource source;
    MotionError error;
    if (composition == nullptr ||
        !CompositionAsSource(*composition, source, error)) {
        return std::nullopt;
    }
    return source;
}

bool ValidateMotionInstanceProps(const MotionComposition& composition,
                                 const Value& props, MotionError& error) {
    if (props.IsNull()) {
        return true;
    }
    if (!props.IsObject() || props.AsObject().empty()) {
        return Fail(error, "InvalidValue", "motion_props",
                    "instance props are null or a non-empty object of prop "
                    "values");
    }
    for (const auto& [name, value] : props.AsObject()) {
        const std::string path = "motion_props." + name;
        const MotionPropDefinition* definition = nullptr;
        for (const MotionPropDefinition& prop : composition.props) {
            if (prop.name == name) {
                definition = &prop;
                break;
            }
        }
        if (definition == nullptr) {
            return Fail(error, "UnknownProp", path,
                        "the composition declares no prop '" + name + "'");
        }
        if (value.IsNull()) {
            return Fail(error, "PropTypeMismatch", path,
                        "an instance prop has a value: leave it out to use "
                        "the composition's");
        }
        if (!ValidateMotionPropValue(*definition, value, path, error)) {
            return false;
        }
    }
    return true;
}

bool ValidateMotionClip(const DocumentTrack& track, const DocumentClip& clip,
                        const DocumentComposition* stored,
                        const MotionComposition* composition,
                        std::string& error) {
    const std::string who = "motion clip ('" + clip.id + "') ";
    if (!IsMotionClip(clip)) {
        error = "clip ('" + clip.id +
                "') has motion_props without a composition_id";
        return false;
    }
    if (!IsValidUlid(clip.composition_id)) {
        error =
            who + "has invalid composition_id '" + clip.composition_id + "'";
        return false;
    }
    if (!clip.source_id.empty()) {
        error = who + "names both a composition and source_id '" +
                clip.source_id + "'";
        return false;
    }
    if (track.kind != "video") {
        error = who + "must be on a video track, not '" + track.kind + "'";
        return false;
    }
    // Colour effects read a camera signal the monitor and the export decode
    // through the project's input transform; a composition is drawn in
    // display sRGB (Export.cc), which no effect of the stack was tuned for.
    if (!clip.effects.empty()) {
        error = who + "carries no colour effects";
        return false;
    }
    if (stored == nullptr || composition == nullptr) {
        error = who + "references unknown composition_id '" +
                clip.composition_id + "'";
        return false;
    }
    MotionError motion;
    if (!ValidateMotionInstanceProps(*composition, clip.motion_props, motion)) {
        error = who + Described(motion);
        return false;
    }
    const RationalTime length =
        MotionFrameTime(*composition, composition->duration);
    try {
        if (clip.source_in.value < 0 ||
            clip.source_in.add(clip.duration) > length) {
            error = who + "has source_in + duration outside the " +
                    std::to_string(composition->duration) +
                    " frames of composition '" + stored->id + "'";
            return false;
        }
    } catch (const std::exception& exception) {
        error =
            who + "has invalid rational time arithmetic: " + exception.what();
        return false;
    }
    return true;
}

int64_t MotionClipFrame(const MotionComposition& composition,
                        const RationalTime& sourceTime) {
    return sourceTime.to_frames(composition.fps_numerator,
                                composition.fps_denominator);
}

std::vector<int64_t> MotionClipFrames(const MotionComposition& composition,
                                      const RationalTime& sourceIn,
                                      int64_t frameCount, MediaRate rate) {
    std::vector<int64_t> frames;
    if (frameCount <= 0) {
        return frames;
    }
    frames.reserve(static_cast<size_t>(frameCount));
    for (int64_t index = 0; index < frameCount; ++index) {
        // index/rate seconds after sourceIn, exactly.
        const RationalTime offset{index * rate.den, rate.num};
        frames.push_back(MotionClipFrame(composition, sourceIn.add(offset)));
    }
    return frames;
}

bool MotionClipAt(const Document& document, const Ulid& clipId,
                  const RationalTime& timelineTime, MotionClipInstant& instant,
                  std::string& errorName, std::string& message) {
    const auto refuse = [&](const char* name, std::string text) {
        errorName = name;
        message = std::move(text);
        return false;
    };
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        return refuse("UnknownClip", "unknown clip_id '" + clipId + "'");
    }
    if (!IsMotionClip(*clip)) {
        return refuse("InvalidOperation",
                      "clip_id '" + clipId +
                          "' shows a media, not a composition: read_frame "
                          "shows it");
    }
    const DocumentComposition* stored =
        document.FindComposition(clip->composition_id);
    if (stored == nullptr) {
        return refuse("UnknownComposition",
                      "unknown composition_id '" + clip->composition_id + "'");
    }
    MotionError error;
    if (!ParseDocumentComposition(*stored, instant.parsed, error)) {
        return refuse("InvalidMotion", Described(error));
    }
    try {
        const RationalTime offset = timelineTime.sub(clip->timeline_in);
        if (timelineTime.rate <= 0 || offset.value < 0 ||
            offset >= clip->duration) {
            return refuse("InvalidTimelineIn",
                          "time " + std::to_string(timelineTime.value) + "/" +
                              std::to_string(timelineTime.rate) +
                              " is outside motion clip '" + clipId +
                              "', which runs " +
                              std::to_string(clip->timeline_in.value) + "/" +
                              std::to_string(clip->timeline_in.rate) + " for " +
                              std::to_string(clip->duration.value) + "/" +
                              std::to_string(clip->duration.rate));
        }
        instant.source_time = clip->source_in.add(offset);
    } catch (const std::exception& exception) {
        return refuse("ArithmeticError", exception.what());
    }
    instant.clip = clip;
    instant.composition = stored;
    instant.frame = MotionClipFrame(instant.parsed, instant.source_time);
    return true;
}

Value MergeMotionProps(const Value& current, const Value& changes) {
    Value merged = Value::MakeObject();
    if (current.IsObject()) {
        for (const auto& [name, value] : current.AsObject()) {
            const Value* change = changes.Find(name);
            if (change == nullptr) {
                merged.Set(name, value);
            } else if (!change->IsNull()) {
                merged.Set(name, *change);
            }
        }
    }
    if (changes.IsObject()) {
        for (const auto& [name, value] : changes.AsObject()) {
            if (current.Find(name) == nullptr && !value.IsNull()) {
                merged.Set(name, value);
            }
        }
    }
    return merged.AsObject().empty() ? Value() : merged;
}

std::string MotionClipPictureKey(const DocumentComposition& composition,
                                 const Value& props, int32_t width,
                                 int32_t height) {
    return MotionClipPictureKey(composition, props, width, height,
                                std::string());
}

std::string MotionClipPictureKey(const DocumentComposition& composition,
                                 const Value& props, int32_t width,
                                 int32_t height, const std::string& reach) {
    // Without a reach the text is the one M4 hashed: a composition that
    // reaches nothing keeps the key it had.
    const std::string text = composition.definition.Dump() + '\n' +
                             props.Dump() + '\n' + std::to_string(width) + 'x' +
                             std::to_string(height) +
                             (reach.empty() ? std::string() : '\n' + reach);
    // Two hashes and the length: a collision would show another picture,
    // and nothing downstream could tell.
    return composition.id + ':' + CanonicalHash(text) + ':' +
           CanonicalHash(text + '\x1f') + ':' + std::to_string(text.size());
}
