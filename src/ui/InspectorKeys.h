#pragma once

// R2c -- docs/roadmap/REEL-2026-09.md. A clip's keys in the Inspector, where
// Resolve puts them: a diamond beside the opacity, the zoom and the two
// positions, arrows to the key before and after the playhead, and on a
// keyed parameter a slider that keys the frame it is moved on. Policy only,
// plain C++ like InspectorGradeControls.h: the view shows what
// KeyedRowStateAt says and hands back the operations built here.
//
// An animated parameter shows the value the clip has on the playhead's
// frame, sampled by ClipAnimation.h: the value Record shows and the export
// delivers there. The constant the clip still carries is not shown, since a
// key replaces it on every frame.

#include "ClipAnimation.h"
#include "InspectorGradeControls.h"
#include "Operations.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ui::inspector {

// The four rows that carry a diamond, in the panel's order.
enum class KeyedControl { Opacity, Scale, PositionX, PositionY };

inline constexpr std::array<KeyedControl, 4> kKeyedControls = {
    KeyedControl::Opacity, KeyedControl::Scale, KeyedControl::PositionX,
    KeyedControl::PositionY};

inline ClipAnimatedParameter KeyedParameter(KeyedControl control) {
    switch (control) {
        case KeyedControl::Opacity:
            return ClipAnimatedParameter::Opacity;
        case KeyedControl::Scale:
            return ClipAnimatedParameter::FramingScale;
        case KeyedControl::PositionX:
            return ClipAnimatedParameter::FramingPositionX;
        case KeyedControl::PositionY:
            return ClipAnimatedParameter::FramingPositionY;
    }
    return ClipAnimatedParameter::Opacity;
}

inline KeyedControl KeyedControlFor(FramingControl control) {
    switch (control) {
        case FramingControl::Scale:
            return KeyedControl::Scale;
        case FramingControl::PositionX:
            return KeyedControl::PositionX;
        case FramingControl::PositionY:
            return KeyedControl::PositionY;
    }
    return KeyedControl::Scale;
}

inline FramingControl FramingControlFor(KeyedControl control) {
    return control == KeyedControl::PositionX   ? FramingControl::PositionX
           : control == KeyedControl::PositionY ? FramingControl::PositionY
                                                : FramingControl::Scale;
}

// The value of `parameter` among sampled parameters, or among the constants
// a clip carries.
inline EffectParamValue ParameterValue(const ClipSampledParameters& sampled,
                                       ClipAnimatedParameter parameter) {
    switch (parameter) {
        case ClipAnimatedParameter::Opacity:
            return sampled.opacity;
        case ClipAnimatedParameter::FramingScale:
            return sampled.framing.scale;
        case ClipAnimatedParameter::FramingPositionX:
            return sampled.framing.position_x;
        case ClipAnimatedParameter::FramingPositionY:
            return sampled.framing.position_y;
    }
    return sampled.opacity;
}

// The slider's position for a row, from the parameters on a frame.
inline float KeyedSliderValue(KeyedControl control,
                              const ClipSampledParameters& sampled,
                              int32_t width, int32_t height) {
    if (control == KeyedControl::Opacity) {
        return CurrentClipOpacity(sampled.opacity);
    }
    return FramingSliderValue(sampled.framing, FramingControlFor(control),
                              width, height);
}

// The parameter value a slider position stands for, in the terms the
// unkeyed row commits: an opacity in thousandths, a framing field reduced.
inline EffectParamValue KeyedValueFromSlider(KeyedControl control,
                                             float sliderValue, int32_t width,
                                             int32_t height) {
    if (control == KeyedControl::Opacity) {
        return QuantizeClipOpacity(sliderValue);
    }
    const ClipFraming framing = WithFramingControl(
        ClipFraming{}, FramingControlFor(control), sliderValue, width, height);
    ClipSampledParameters sampled;
    sampled.framing = framing;
    return ParameterValue(sampled, KeyedParameter(control));
}

// The playhead's frame in the clip: zero on its first frame, negative before
// it, past its count after it.
inline int64_t ClipLocalFrame(const DocumentClip& clip, RationalTime position,
                              const MediaRate& rate) {
    if (rate.num <= 0 || rate.den <= 0 || position.rate <= 0) return 0;
    return position.sub(clip.timeline_in).to_frames(rate.num, rate.den);
}

struct KeyedRowState {
    // The diamond can be pressed: the row edits this clip, and the playhead
    // is on one of its frames. A key is never posed off the clip.
    bool keyable = false;
    bool animated = false;
    bool key_here = false;
    std::optional<int64_t> previous_frame;
    std::optional<int64_t> next_frame;
    // The value on the playhead's frame, and where it puts the slider.
    EffectParamValue value{1, 1};
    float slider_value = 0.0f;
};

// `editable` is whether the row edits this clip at all: the opacity of a
// clip on a video track, the framing of a clip CanFrameClip accepts.
inline KeyedRowState KeyedRowStateAt(const DocumentClip& clip,
                                     KeyedControl control, bool editable,
                                     int64_t localFrame, const MediaRate& rate,
                                     int32_t width, int32_t height) {
    KeyedRowState state;
    const ClipAnimatedParameter parameter = KeyedParameter(control);
    std::string error;
    int64_t frames = 0;
    const bool inside = ClipFrameCount(clip, rate, frames, error) &&
                        localFrame >= 0 && localFrame < frames;
    state.keyable = editable && inside;
    state.animated = ClipAnimates(clip, parameter);
    state.key_here =
        FindClipKeyframeAt(clip, parameter, localFrame, rate) != nullptr;
    state.previous_frame =
        PreviousClipKeyframeFrame(clip, parameter, localFrame, rate);
    state.next_frame = NextClipKeyframeFrame(clip, parameter, localFrame, rate);
    ClipSampledParameters sampled;
    if (!SampleClipParameters(clip, localFrame, rate, sampled, error)) {
        sampled.opacity = clip.opacity;
        sampled.framing = clip.framing;
    }
    state.value = ParameterValue(sampled, parameter);
    state.slider_value = KeyedSliderValue(control, sampled, width, height);
    return state;
}

// The operation that writes `value` into the constant field of `parameter`.
inline Operation FieldOperation(const DocumentClip& clip,
                                ClipAnimatedParameter parameter,
                                const EffectParamValue& value) {
    if (parameter == ClipAnimatedParameter::Opacity) {
        return SetClipOpacityOperation{clip.id, value};
    }
    ClipFraming framing = clip.framing;
    if (parameter == ClipAnimatedParameter::FramingScale) {
        framing.scale = value;
    } else if (parameter == ClipAnimatedParameter::FramingPositionX) {
        framing.position_x = value;
    } else {
        framing.position_y = value;
    }
    SetClipFramingOperation operation;
    operation.clips.push_back({clip.id, ReduceClipFraming(framing)});
    return operation;
}

// The diamond pressed on frame `localFrame`. Without a key there, a key
// `id` holding the value the frame shows, so that the picture does not move.
// With one, the key goes; when it was the parameter's last, its value goes
// into the field in the same undo, so the picture does not move either
// (Resolve keeps the value when the last key is deleted). `posed` says which.
inline bool KeyToggleOperation(const DocumentClip& clip, KeyedControl control,
                               int64_t localFrame, const MediaRate& rate,
                               const Ulid& id, Operation& operation,
                               bool& posed, std::string& error) {
    const ClipAnimatedParameter parameter = KeyedParameter(control);
    std::vector<ClipParameterAnimation> animation;
    if (const ClipKeyframe* key =
            FindClipKeyframeAt(clip, parameter, localFrame, rate)) {
        const EffectParamValue held = key->value;
        const ClipParameterAnimation* entry =
            FindClipAnimation(clip, parameter);
        const bool last = entry != nullptr && entry->keyframes.size() == 1;
        if (!ClipAnimationWithoutKey(clip, parameter, localFrame, rate,
                                     animation, error)) {
            return false;
        }
        SetClipAnimationOperation set;
        set.clips.push_back({clip.id, std::move(animation)});
        ClipSampledParameters constants;
        constants.opacity = clip.opacity;
        constants.framing = clip.framing;
        if (last && !SameFraction(held, ParameterValue(constants, parameter))) {
            BatchOperation batch;
            batch.label = "Inspecteur : clé retirée";
            batch.operations.emplace_back(std::move(set));
            batch.operations.push_back(FieldOperation(clip, parameter, held));
            operation = std::move(batch);
        } else {
            operation = std::move(set);
        }
        posed = false;
        error.clear();
        return true;
    }
    ClipSampledParameters sampled;
    if (!SampleClipParameters(clip, localFrame, rate, sampled, error) ||
        !ClipAnimationWithKey(clip, parameter, localFrame, rate,
                              ParameterValue(sampled, parameter), id, animation,
                              error)) {
        return false;
    }
    SetClipAnimationOperation set;
    set.clips.push_back({clip.id, std::move(animation)});
    operation = std::move(set);
    posed = true;
    return true;
}

// A keyed row's slider let go on frame `localFrame`: the key there takes the
// value, or a key `id` is added there -- Resolve keys an animated parameter
// wherever it is changed. The animation is handed back too, for the monitor
// to show it while the hand still holds the slider.
inline bool KeyedValueAnimation(const DocumentClip& clip, KeyedControl control,
                                int64_t localFrame, const MediaRate& rate,
                                const EffectParamValue& value, const Ulid& id,
                                std::vector<ClipParameterAnimation>& animation,
                                std::string& error) {
    return ClipAnimationWithKey(clip, KeyedParameter(control), localFrame, rate,
                                value, id, animation, error);
}

// The clip as the monitor shows it while an Inspector drag previews it:
// the framing it would commit, and the keys it would commit when the
// dragged row is keyed. Empty when nothing previews this clip.
inline std::optional<DocumentClip> PreviewedClip(
    const DocumentClip& clip, const std::optional<FramingPreview>& preview) {
    if (!preview || preview->clip_id != clip.id) return std::nullopt;
    DocumentClip shown = clip;
    shown.framing = preview->framing;
    if (preview->animation) shown.animation = *preview->animation;
    return shown;
}

}  // namespace ui::inspector
