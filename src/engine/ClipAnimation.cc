#include "ClipAnimation.h"

#include "ClipSpeed.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace {

// An exact fraction as a double. One division, and it is the only place a
// key's value or easing point becomes floating point.
double AsDouble(const EffectParamValue& value) {
    if (value.den == 0) {
        return 0.0;
    }
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

// Step 3 of the boundary described in the header: the sampled double back to
// an exact fraction over kAnimationSampleDenominator, to nearest, ties away
// from zero, then reduced so that a value that lands on a simple fraction is
// spelled simply -- 1.15 comes back as 23/20 and not as 11500/10000.
EffectParamValue Quantize(double value) {
    const double scaled = value * kAnimationSampleDenominator;
    const double rounded =
        scaled < 0.0 ? -std::floor(-scaled + 0.5) : std::floor(scaled + 0.5);
    int64_t numerator = static_cast<int64_t>(rounded);
    int64_t denominator = kAnimationSampleDenominator;
    int64_t left = numerator < 0 ? -numerator : numerator;
    int64_t right = denominator;
    while (right != 0) {
        const int64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    if (left > 1) {
        numerator /= left;
        denominator /= left;
    }
    if (numerator == 0) {
        denominator = 1;
    }
    EffectParamValue result;
    result.num = static_cast<int32_t>(numerator);
    result.den = static_cast<int32_t>(denominator);
    return result;
}

// The clip's frame `k` as an exact time: k frames at num/den per second is
// k*den/num seconds. Nothing here goes through a double.
RationalTime FrameTime(int64_t frame, const MediaRate& rate) {
    return RationalTime(frame * rate.den, rate.num);
}

bool ValueIsInside(const EffectParamValue& value, int32_t magnitude) {
    const __int128 limit = static_cast<__int128>(magnitude) * value.den;
    const __int128 number = value.num;
    return number <= limit && number >= -limit;
}

bool KeyframeValueIsValid(ClipAnimatedParameter parameter,
                          const EffectParamValue& value, std::string& error) {
    if (value.den <= 0) {
        error =
            "a keyframe value must be a fraction with a positive "
            "denominator";
        return false;
    }
    switch (parameter) {
        case ClipAnimatedParameter::Opacity:
            if (value.num < 0 || !ValueIsInside(value, 1)) {
                error = "an animated opacity stays in [0, 1]";
                return false;
            }
            return true;
        case ClipAnimatedParameter::FramingScale:
            if (value.num <= 0) {
                error =
                    "an animated framing scale must be positive; a zero, a "
                    "negative or a mirror is not a scale";
                return false;
            }
            if (!ValueIsInside(value, kMaxAnimatedScale)) {
                error = "an animated framing scale stays at or under " +
                        std::to_string(kMaxAnimatedScale);
                return false;
            }
            return true;
        case ClipAnimatedParameter::FramingPositionX:
        case ClipAnimatedParameter::FramingPositionY:
            if (!ValueIsInside(value, kMaxAnimatedOffset)) {
                error =
                    "an animated framing position moves the picture more "
                    "than " +
                    std::to_string(kMaxAnimatedOffset) + " frames off centre";
                return false;
            }
            return true;
    }
    error = "unknown animated parameter";
    return false;
}

// The easing points Keyframes.h accepts: x in [0, 1] keeps the curve a
// function of time, y is free so an overshoot stays expressible.
bool EasingIsValid(const ClipKeyframe& keyframe, std::string& error) {
    if (keyframe.interpolation != KeyframeInterpolation::CubicBezier) {
        return true;
    }
    const EffectParamValue xs[] = {keyframe.easing_x1, keyframe.easing_x2};
    for (const EffectParamValue& value : xs) {
        if (value.den <= 0 || value.num < 0 || !ValueIsInside(value, 1)) {
            error =
                "a cubic-bezier control point's x stays in [0, 1], so the "
                "curve stays a function of time";
            return false;
        }
    }
    const EffectParamValue ys[] = {keyframe.easing_y1, keyframe.easing_y2};
    for (const EffectParamValue& value : ys) {
        if (value.den <= 0 || !ValueIsInside(value, kMaxAnimatedScale)) {
            error = "a cubic-bezier control point's y stays at or under " +
                    std::to_string(kMaxAnimatedScale) + " in magnitude";
            return false;
        }
    }
    return true;
}

// The track Keyframes.h evaluates, built from the exact keys. This is step 2
// of the boundary: it is the only conversion of a key to a double.
bool BuildTrack(const ClipParameterAnimation& animation, KeyframeTrack& track,
                std::string& error) {
    for (const ClipKeyframe& source : animation.keyframes) {
        Keyframe keyframe;
        keyframe.id = source.id;
        keyframe.time = source.time;
        keyframe.value = AsDouble(source.value);
        keyframe.interpolation = source.interpolation;
        keyframe.easing.x1 = AsDouble(source.easing_x1);
        keyframe.easing.y1 = AsDouble(source.easing_y1);
        keyframe.easing.x2 = AsDouble(source.easing_x2);
        keyframe.easing.y2 = AsDouble(source.easing_y2);
        const KeyframeResult added = track.Add(keyframe);
        if (!added) {
            error = std::string("keyframe track of ") +
                    ClipAnimatedParameterName(animation.parameter) +
                    " is invalid: " + KeyframeErrorName(added.error);
            return false;
        }
    }
    return true;
}

}  // namespace

const char* ClipAnimatedParameterName(ClipAnimatedParameter parameter) {
    switch (parameter) {
        case ClipAnimatedParameter::Opacity:
            return "opacity";
        case ClipAnimatedParameter::FramingScale:
            return "framing.scale";
        case ClipAnimatedParameter::FramingPositionX:
            return "framing.position_x";
        case ClipAnimatedParameter::FramingPositionY:
            return "framing.position_y";
    }
    return "";
}

bool ParseClipAnimatedParameterName(const std::string& name,
                                    ClipAnimatedParameter& parameter) {
    const ClipAnimatedParameter all[] = {
        ClipAnimatedParameter::Opacity, ClipAnimatedParameter::FramingScale,
        ClipAnimatedParameter::FramingPositionX,
        ClipAnimatedParameter::FramingPositionY};
    for (ClipAnimatedParameter candidate : all) {
        if (name == ClipAnimatedParameterName(candidate)) {
            parameter = candidate;
            return true;
        }
    }
    return false;
}

const char* KeyframeInterpolationName(KeyframeInterpolation interpolation) {
    switch (interpolation) {
        case KeyframeInterpolation::Hold:
            return "hold";
        case KeyframeInterpolation::Linear:
            return "linear";
        case KeyframeInterpolation::CubicBezier:
            return "cubic-bezier";
    }
    return "";
}

bool ParseKeyframeInterpolationName(const std::string& name,
                                    KeyframeInterpolation& interpolation) {
    const KeyframeInterpolation all[] = {KeyframeInterpolation::Hold,
                                         KeyframeInterpolation::Linear,
                                         KeyframeInterpolation::CubicBezier};
    for (KeyframeInterpolation candidate : all) {
        if (name == KeyframeInterpolationName(candidate)) {
            interpolation = candidate;
            return true;
        }
    }
    return false;
}

const ClipParameterAnimation* FindClipAnimation(
    const DocumentClip& clip, ClipAnimatedParameter parameter) {
    for (const ClipParameterAnimation& entry : clip.animation) {
        if (entry.parameter == parameter) {
            return &entry;
        }
    }
    return nullptr;
}

bool ClipAnimates(const DocumentClip& clip, ClipAnimatedParameter parameter) {
    return FindClipAnimation(clip, parameter) != nullptr;
}

bool ClipAnimationIsValid(const std::vector<ClipParameterAnimation>& animation,
                          std::string& error) {
    std::set<int> parameters;
    std::set<Ulid> ids;
    for (const ClipParameterAnimation& entry : animation) {
        const std::string what = ClipAnimatedParameterName(entry.parameter);
        if (what.empty()) {
            error = "unknown animated parameter";
            return false;
        }
        if (!parameters.insert(static_cast<int>(entry.parameter)).second) {
            error = "parameter '" + what + "' is animated twice";
            return false;
        }
        // R2c -- docs/roadmap/REEL-2026-09.md. One key is allowed, and holds
        // its value over the whole clip. R2 refused it -- a constant has a
        // field, and two spellings of one state is what the format rules of
        // this project exist to prevent -- until the Inspector posed keys
        // the way Resolve does: the first click on the diamond poses one
        // key, the second one is born where the value next changes. The author
        // chose the single key over a first key held by the panel alone,
        // which no agent, no undo and no other machine would have seen (31
        // October 2026). Motion compositions already allowed it.
        if (entry.keyframes.empty()) {
            error = "parameter '" + what + "' carries no key";
            return false;
        }
        if (entry.keyframes.size() > kMaxClipKeyframes) {
            error = "parameter '" + what + "' carries more than " +
                    std::to_string(kMaxClipKeyframes) + " keys";
            return false;
        }
        for (std::size_t index = 0; index < entry.keyframes.size(); ++index) {
            const ClipKeyframe& keyframe = entry.keyframes[index];
            if (keyframe.id.empty()) {
                error = "a keyframe of '" + what + "' carries no id";
                return false;
            }
            if (!ids.insert(keyframe.id).second) {
                error = "keyframe id '" + keyframe.id + "' is used twice";
                return false;
            }
            if (keyframe.time.rate <= 0) {
                error = "a keyframe of '" + what +
                        "' carries a time with a non-positive rate";
                return false;
            }
            if (index > 0 &&
                entry.keyframes[index - 1].time.compare(keyframe.time) >= 0) {
                error = "the keys of '" + what +
                        "' must be written in strictly increasing time";
                return false;
            }
            if (keyframe.interpolation != KeyframeInterpolation::Hold &&
                keyframe.interpolation != KeyframeInterpolation::Linear &&
                keyframe.interpolation != KeyframeInterpolation::CubicBezier) {
                error = "a keyframe of '" + what +
                        "' carries an unknown interpolation";
                return false;
            }
            if (!KeyframeValueIsValid(entry.parameter, keyframe.value, error)) {
                error.insert(0, "keyframe of '" + what + "': ");
                return false;
            }
            if (!EasingIsValid(keyframe, error)) {
                error.insert(0, "keyframe of '" + what + "': ");
                return false;
            }
        }
    }
    error.clear();
    return true;
}

bool SampleClipParameters(const DocumentClip& clip, int64_t localFrame,
                          const MediaRate& sequenceRate,
                          ClipSampledParameters& sampled, std::string& error) {
    sampled.opacity = clip.opacity;
    sampled.framing = clip.framing;
    if (clip.animation.empty()) {
        error.clear();
        return true;
    }
    if (sequenceRate.num <= 0 || sequenceRate.den <= 0) {
        error = "the sequence has no usable cadence to sample a keyframe at";
        return false;
    }
    if (!ClipAnimationIsValid(clip.animation, error)) {
        return false;
    }
    const RationalTime time = FrameTime(localFrame, sequenceRate);
    for (const ClipParameterAnimation& entry : clip.animation) {
        KeyframeTrack track;
        if (!BuildTrack(entry, track, error)) {
            return false;
        }
        // Clamp on both sides: outside the keyed range the outermost key's
        // value holds. An extrapolated slope would run a scale to zero or a
        // position off the frame on a clip somebody merely lengthened.
        const KeyframeEvaluation evaluation = track.Evaluate(time);
        if (!evaluation) {
            error =
                std::string("keyframe track of ") +
                ClipAnimatedParameterName(entry.parameter) +
                " cannot be evaluated: " + KeyframeErrorName(evaluation.error);
            return false;
        }
        const EffectParamValue value = Quantize(evaluation.value);
        if (!KeyframeValueIsValid(entry.parameter, value, error)) {
            error.insert(0, std::string("the sampled value of ") +
                                ClipAnimatedParameterName(entry.parameter) +
                                " is out of range: ");
            return false;
        }
        switch (entry.parameter) {
            case ClipAnimatedParameter::Opacity:
                sampled.opacity = value;
                break;
            case ClipAnimatedParameter::FramingScale:
                sampled.framing.scale = value;
                break;
            case ClipAnimatedParameter::FramingPositionX:
                sampled.framing.position_x = value;
                break;
            case ClipAnimatedParameter::FramingPositionY:
                sampled.framing.position_y = value;
                break;
        }
    }
    error.clear();
    return true;
}

bool ResolveClipFramingAtFrame(const Document& document,
                               const DocumentClip& clip, int64_t localFrame,
                               int64_t frameWidth, int64_t frameHeight,
                               ClipFramingGeometry& geometry,
                               EffectParamValue& opacity, std::string& error) {
    ClipSampledParameters sampled;
    if (!SampleClipParameters(clip, localFrame, document.sequence.frame_rate,
                              sampled, error)) {
        return false;
    }
    opacity = sampled.opacity;
    int64_t sourceWidth = 0;
    int64_t sourceHeight = 0;
    if (!ClipDisplayedSize(document, clip, sourceWidth, sourceHeight, error)) {
        return false;
    }
    return ResolveClipFraming(sampled.framing, sourceWidth, sourceHeight,
                              frameWidth, frameHeight, geometry, error);
}

bool ClipFrameCount(const DocumentClip& clip, const MediaRate& sequenceRate,
                    int64_t& frames, std::string& error) {
    frames = 0;
    if (sequenceRate.num <= 0 || sequenceRate.den <= 0) {
        error = "the sequence has no usable cadence";
        return false;
    }
    const __int128 numerator =
        static_cast<__int128>(clip.duration.value) * sequenceRate.num;
    const __int128 denominator =
        static_cast<__int128>(clip.duration.rate) * sequenceRate.den;
    if (denominator == 0 || numerator % denominator != 0) {
        error =
            "the clip's duration is not a whole number of sequence frames, "
            "so its animation cannot be sampled frame by frame";
        return false;
    }
    frames = static_cast<int64_t>(numerator / denominator);
    error.clear();
    return true;
}

void ShiftClipAnimationHead(DocumentClip& clip, const RationalTime& advance) {
    // R3, criterion 4 -- a speed ramp's keys are clip keys too, under the
    // same rule; every site that moves a head already calls this.
    ShiftClipSpeedRampHead(clip, advance);
    if (clip.animation.empty() || advance.value == 0) {
        return;
    }
    for (ClipParameterAnimation& entry : clip.animation) {
        for (ClipKeyframe& keyframe : entry.keyframes) {
            keyframe.time = keyframe.time.sub(advance);
        }
    }
}

int64_t ClipKeyframeFrame(const ClipKeyframe& keyframe,
                          const MediaRate& sequenceRate) {
    const __int128 numerator =
        static_cast<__int128>(keyframe.time.value) * sequenceRate.num;
    const __int128 denominator =
        static_cast<__int128>(keyframe.time.rate) * sequenceRate.den;
    if (denominator <= 0) {
        return 0;
    }
    // Toward the past for a key before the clip's head too.
    __int128 quotient = numerator / denominator;
    if (numerator % denominator != 0 && numerator < 0) {
        --quotient;
    }
    return static_cast<int64_t>(quotient);
}

const ClipKeyframe* FindClipKeyframeAt(const DocumentClip& clip,
                                       ClipAnimatedParameter parameter,
                                       int64_t localFrame,
                                       const MediaRate& sequenceRate) {
    const ClipParameterAnimation* entry = FindClipAnimation(clip, parameter);
    if (entry == nullptr) {
        return nullptr;
    }
    for (const ClipKeyframe& keyframe : entry->keyframes) {
        if (ClipKeyframeFrame(keyframe, sequenceRate) == localFrame) {
            return &keyframe;
        }
    }
    return nullptr;
}

std::optional<int64_t> PreviousClipKeyframeFrame(
    const DocumentClip& clip, ClipAnimatedParameter parameter,
    int64_t localFrame, const MediaRate& sequenceRate) {
    std::optional<int64_t> previous;
    if (const ClipParameterAnimation* entry =
            FindClipAnimation(clip, parameter)) {
        for (const ClipKeyframe& keyframe : entry->keyframes) {
            const int64_t frame = ClipKeyframeFrame(keyframe, sequenceRate);
            if (frame < localFrame) {
                previous = frame;
            }
        }
    }
    return previous;
}

std::optional<int64_t> NextClipKeyframeFrame(const DocumentClip& clip,
                                             ClipAnimatedParameter parameter,
                                             int64_t localFrame,
                                             const MediaRate& sequenceRate) {
    if (const ClipParameterAnimation* entry =
            FindClipAnimation(clip, parameter)) {
        for (const ClipKeyframe& keyframe : entry->keyframes) {
            const int64_t frame = ClipKeyframeFrame(keyframe, sequenceRate);
            if (frame > localFrame) {
                return frame;
            }
        }
    }
    return std::nullopt;
}

bool ClipAnimationWithKey(const DocumentClip& clip,
                          ClipAnimatedParameter parameter, int64_t localFrame,
                          const MediaRate& sequenceRate,
                          const EffectParamValue& value, const Ulid& id,
                          std::vector<ClipParameterAnimation>& animation,
                          std::string& error) {
    if (sequenceRate.num <= 0 || sequenceRate.den <= 0) {
        error = "the sequence has no usable cadence to place a key at";
        return false;
    }
    if (!KeyframeValueIsValid(parameter, value, error)) {
        error.insert(0, std::string("key of ") +
                            ClipAnimatedParameterName(parameter) + ": ");
        return false;
    }
    animation = clip.animation;
    const RationalTime time = FrameTime(localFrame, sequenceRate);
    const auto entry =
        std::find_if(animation.begin(), animation.end(),
                     [&](const ClipParameterAnimation& candidate) {
                         return candidate.parameter == parameter;
                     });
    if (entry == animation.end()) {
        ClipParameterAnimation added;
        added.parameter = parameter;
        ClipKeyframe keyframe;
        keyframe.id = id;
        keyframe.time = time;
        keyframe.value = value;
        added.keyframes.push_back(keyframe);
        const auto after =
            std::find_if(animation.begin(), animation.end(),
                         [&](const ClipParameterAnimation& candidate) {
                             return static_cast<int>(candidate.parameter) >
                                    static_cast<int>(parameter);
                         });
        animation.insert(after, std::move(added));
        error.clear();
        return true;
    }
    std::vector<ClipKeyframe>& keyframes = entry->keyframes;
    for (ClipKeyframe& keyframe : keyframes) {
        if (ClipKeyframeFrame(keyframe, sequenceRate) == localFrame) {
            keyframe.value = value;
            error.clear();
            return true;
        }
    }
    const auto after = std::find_if(keyframes.begin(), keyframes.end(),
                                    [&](const ClipKeyframe& keyframe) {
                                        return keyframe.time.compare(time) > 0;
                                    });
    // The curve of the segment it cuts, or a straight line for a first key.
    ClipKeyframe keyframe =
        after == keyframes.begin() ? ClipKeyframe{} : *std::prev(after);
    keyframe.id = id;
    keyframe.time = time;
    keyframe.value = value;
    keyframes.insert(after, keyframe);
    error.clear();
    return true;
}

bool ClipAnimationWithoutKey(const DocumentClip& clip,
                             ClipAnimatedParameter parameter,
                             int64_t localFrame, const MediaRate& sequenceRate,
                             std::vector<ClipParameterAnimation>& animation,
                             std::string& error) {
    animation = clip.animation;
    const auto entry =
        std::find_if(animation.begin(), animation.end(),
                     [&](const ClipParameterAnimation& candidate) {
                         return candidate.parameter == parameter;
                     });
    if (entry != animation.end()) {
        std::vector<ClipKeyframe>& keyframes = entry->keyframes;
        const auto key = std::find_if(
            keyframes.begin(), keyframes.end(),
            [&](const ClipKeyframe& keyframe) {
                return ClipKeyframeFrame(keyframe, sequenceRate) == localFrame;
            });
        if (key != keyframes.end()) {
            keyframes.erase(key);
            if (keyframes.empty()) {
                animation.erase(entry);
            }
            error.clear();
            return true;
        }
    }
    error = std::string("clip '") + clip.id + "' has no key of " +
            ClipAnimatedParameterName(parameter) + " on its frame " +
            std::to_string(localFrame);
    return false;
}

std::string ClipAnimationRefusal(const DocumentClip& clip,
                                 const std::string& what) {
    return what + " does not carry the animated parameters of clip '" +
           clip.id +
           "' (R2): it would be delivered as a still, without the movement "
           "the edit is built on";
}
