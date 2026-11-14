// R2 -- docs/roadmap/REEL-2026-09.md. What an animated clip parameter is
// worth at a frame, and what its keys become when the clip is moved,
// trimmed or cut in two.
//
// Portable on purpose (guard rail 1 of the lot): the monitor, the export,
// the sheets and verify_export all reach this arithmetic, and a second
// answer to the same question would diverge without any of them failing.
// Everything here runs without AppKit, Metal or FFmpeg.

#include "ClipAnimation.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

const MediaRate kRate{25, 1};

ClipKeyframe Key(
    const std::string& id, int64_t frame, int32_t num, int32_t den,
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear) {
    ClipKeyframe keyframe;
    keyframe.id = id;
    keyframe.time = RationalTime(frame, 25);
    keyframe.value = EffectParamValue{num, den};
    keyframe.interpolation = interpolation;
    return keyframe;
}

ClipParameterAnimation Track(ClipAnimatedParameter parameter,
                             std::vector<ClipKeyframe> keyframes) {
    ClipParameterAnimation track;
    track.parameter = parameter;
    track.keyframes = std::move(keyframes);
    return track;
}

DocumentClip ZoomClip() {
    DocumentClip clip;
    clip.id = "01K90000000000000000000C01";
    clip.source_id = "01K90000000000000000000S01";
    clip.source_in = RationalTime(0, 25);
    clip.duration = RationalTime(25, 25);
    clip.timeline_in = RationalTime(50, 25);
    clip.animation = {Track(ClipAnimatedParameter::FramingScale,
                            {Key("01K90000000000000000000K01", 0, 1, 1),
                             Key("01K90000000000000000000K02", 12, 23, 20)})};
    return clip;
}

EffectParamValue SampledScale(const DocumentClip& clip, int64_t frame) {
    ClipSampledParameters sampled;
    std::string error;
    if (!SampleClipParameters(clip, frame, kRate, sampled, error)) {
        throw std::runtime_error("sampling failed: " + error);
    }
    return sampled.framing.scale;
}

double AsDouble(const EffectParamValue& value) {
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

}  // namespace

int main() {
    Test("a value is sampled from the whole frame number, exactly", [] {
        const DocumentClip clip = ZoomClip();
        Check(SampledScale(clip, 0).num == 1 && SampledScale(clip, 0).den == 1,
              "frame 0 is the first key's value, spelled 1/1");
        const EffectParamValue end = SampledScale(clip, 12);
        Check(end.num == 23 && end.den == 20,
              "frame 12 is the last key's value, reduced to 23/20");
        // Linear between them: frame 6 is exactly halfway, and the
        // quantisation lands on a spelling that reduces.
        const EffectParamValue middle = SampledScale(clip, 6);
        Check(std::abs(AsDouble(middle) - 1.075) < 1e-9,
              "frame 6 is halfway, 1.075");
        Check(middle.den <= kAnimationSampleDenominator,
              "and it is an exact fraction over the sampling denominator");
    });

    Test("outside the keyed range the outermost key holds", [] {
        const DocumentClip clip = ZoomClip();
        Check(AsDouble(SampledScale(clip, -4)) == 1.0,
              "before the first key the value clamps");
        Check(std::abs(AsDouble(SampledScale(clip, 40)) - 1.15) < 1e-9,
              "after the last key it clamps too, not extrapolates");
    });

    Test("a cubic-bezier is the CSS curve, sampled at whole frames", [] {
        DocumentClip clip = ZoomClip();
        ClipKeyframe first = Key("01K90000000000000000000K01", 0, 1, 1,
                                 KeyframeInterpolation::CubicBezier);
        // cubic-bezier(0.4, 0, 0.2, 1) -- the usual ease-in-out.
        first.easing_x1 = EffectParamValue{2, 5};
        first.easing_y1 = EffectParamValue{0, 1};
        first.easing_x2 = EffectParamValue{1, 5};
        first.easing_y2 = EffectParamValue{1, 1};
        clip.animation[0].keyframes[0] = first;
        const double start = AsDouble(SampledScale(clip, 0));
        const double quarter = AsDouble(SampledScale(clip, 3));
        const double middle = AsDouble(SampledScale(clip, 6));
        const double end = AsDouble(SampledScale(clip, 12));
        Check(std::abs(start - 1.0) < 1e-9 && std::abs(end - 1.15) < 1e-9,
              "the two ends are the keys themselves");
        Check(quarter > start && quarter < middle && middle < end,
              "the curve is monotonic between them");
        // An ease-in-out is slower than linear at a quarter of the way and
        // symmetric about the middle: that is what makes it an ease and not
        // a straight line.
        Check(quarter < 1.0 + (0.15 * 0.25),
              "and it starts slower than a straight line (" +
                  std::to_string(quarter) + ')');
        // cubic-bezier(0.4, 0, 0.2, 1) is not symmetric: at half the
        // duration it is already past half the travel (0.775 of it, by the
        // curve's own arithmetic), which is exactly what makes a zoom feel
        // like it lands rather than arrives.
        Check(middle > 1.10 && middle < 1.13,
              "and it is past halfway at half the duration (" +
                  std::to_string(middle) +
                  ", a straight line would be "
                  "1.075)");
    });

    Test("a track is refused rather than half-read", [] {
        std::string error;
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::FramingScale, {})}, error),
              "a parameter without a key is refused");
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::FramingScale,
                         {Key("01K90000000000000000000K02", 12, 1, 1),
                          Key("01K90000000000000000000K01", 0, 1, 1)})},
                  error),
              "keys out of order are refused, not sorted");
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::FramingScale,
                         {Key("01K90000000000000000000K01", 0, 1, 1),
                          Key("01K90000000000000000000K01", 12, 1, 1)})},
                  error),
              "a repeated key id is refused");
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::Opacity,
                         {Key("01K90000000000000000000K01", 0, 0, 1),
                          Key("01K90000000000000000000K02", 12, 3, 2)})},
                  error),
              "an opacity above 1 is refused");
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::FramingScale,
                         {Key("01K90000000000000000000K01", 0, 0, 1),
                          Key("01K90000000000000000000K02", 12, 1, 1)})},
                  error),
              "a scale of zero is refused: it is not a scale");
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::FramingScale,
                         {Key("01K90000000000000000000K01", 0, 1, 1),
                          Key("01K90000000000000000000K02", 12, 2, 1)}),
                   Track(ClipAnimatedParameter::FramingScale,
                         {Key("01K90000000000000000000K03", 0, 1, 1),
                          Key("01K90000000000000000000K04", 12, 2, 1)})},
                  error),
              "the same parameter animated twice is refused");
        ClipKeyframe eased = Key("01K90000000000000000000K01", 0, 1, 1,
                                 KeyframeInterpolation::CubicBezier);
        eased.easing_x1 = EffectParamValue{3, 2};
        Check(!ClipAnimationIsValid(
                  {Track(ClipAnimatedParameter::FramingScale,
                         {eased, Key("01K90000000000000000000K02", 12, 2, 1)})},
                  error),
              "a control point outside [0, 1] in x is refused: the curve "
              "would stop being a function of time");
    });

    // CRITERION 1. The three edits the ticket asks about, each against the
    // same rule: a key stays on the frame of the clip it was put on.
    Test("moving the clip leaves every key where it was", [] {
        DocumentClip clip = ZoomClip();
        const std::vector<ClipParameterAnimation> before = clip.animation;
        // A move changes timeline_in and nothing else; the head did not
        // move relative to the clip, so ShiftClipAnimationHead is never
        // called and the keys are untouched.
        clip.timeline_in = RationalTime(200, 25);
        Check(clip.animation[0].keyframes[0].time.compare(
                  before[0].keyframes[0].time) == 0 &&
                  clip.animation[0].keyframes[1].time.compare(
                      before[0].keyframes[1].time) == 0,
              "the keys are the same times");
        Check(AsDouble(SampledScale(clip, 6)) == 1.075,
              "and the clip's sixth frame is worth what it was worth");
    });

    Test("trimming the head takes the keys with it", [] {
        DocumentClip clip = ZoomClip();
        // Five frames off the head: the frame that was local 12 is now
        // local 7, and its key has to be there.
        const RationalTime advance(5, 25);
        clip.source_in = clip.source_in.add(advance);
        clip.duration = clip.duration.sub(advance);
        clip.timeline_in = clip.timeline_in.add(advance);
        ShiftClipAnimationHead(clip, advance);
        Check(AsDouble(SampledScale(clip, 7)) == 1.15,
              "the end of the zoom is now the clip's seventh frame");
        Check(AsDouble(SampledScale(clip, 1)) == AsDouble([&] {
                  DocumentClip original = ZoomClip();
                  return SampledScale(original, 6);
              }()),
              "and every frame in between still shows what it showed");
        // The first key is now before the head and is KEPT: it still
        // shapes the slope at the head, and a trim back must restore the
        // same bytes.
        Check(clip.animation[0].keyframes.size() == 2,
              "the key pushed outside the clip is kept, not dropped");
        Check(clip.animation[0].keyframes[0].time.value == -5,
              "at its negative local time");
        ShiftClipAnimationHead(clip, RationalTime(-5, 25));
        Check(clip.animation[0].keyframes[0].time.value == 0 &&
                  clip.animation[0].keyframes[1].time.value == 12,
              "trimming back restores the times exactly: apply -> undo is "
              "byte for byte");
    });

    Test("cutting the clip in two keeps the curve continuous", [] {
        const DocumentClip original = ZoomClip();
        const RationalTime cut(5, 25);
        DocumentClip left = original;
        left.duration = cut;
        DocumentClip right = original;
        right.id = "01K90000000000000000000C02";
        right.source_in = original.source_in.add(cut);
        right.duration = original.duration.sub(cut);
        right.timeline_in = original.timeline_in.add(cut);
        ShiftClipAnimationHead(right, cut);
        for (int64_t frame = 0; frame < 25; ++frame) {
            const DocumentClip& piece = frame < 5 ? left : right;
            const int64_t local = frame < 5 ? frame : frame - 5;
            Check(AsDouble(SampledScale(piece, local)) ==
                      AsDouble(SampledScale(original, frame)),
                  "frame " + std::to_string(frame) +
                      " of the cut pair shows what the uncut clip showed");
        }
        Check(right.animation[0].keyframes[0].time.value == -5 &&
                  right.animation[0].keyframes[1].time.value == 7,
              "the right piece's keys are the same keys, shifted by the cut");
        Check(left.animation[0].keyframes.size() == 2,
              "and the left piece keeps the key that is now past its end, so "
              "a join restores the same bytes");
    });

    Test("a sampled value is exact, and the rounding happens once", [] {
        DocumentClip clip = ZoomClip();
        // A value whose linear interpolation is not a short decimal: the
        // quantisation has to land on the same fraction every time, on
        // every machine, or the monitor and the export would each round
        // their own way (guard rail 1).
        clip.animation[0].keyframes[1].value = EffectParamValue{7, 3};
        const EffectParamValue first = SampledScale(clip, 5);
        const EffectParamValue second = SampledScale(clip, 5);
        Check(first.num == second.num && first.den == second.den,
              "the same frame gives the same fraction, twice");
        Check(first.den <= kAnimationSampleDenominator && first.den > 0,
              "and the fraction is over the sampling denominator or a "
              "divisor of it (" +
                  std::to_string(first.num) + '/' + std::to_string(first.den) +
                  ')');
        Check(std::abs(AsDouble(first) - (1.0 + ((7.0 / 3.0 - 1.0) * 5 / 12))) <
                  1.0 / kAnimationSampleDenominator,
              "within one quantum of the exact interpolation");
    });

    Test("the frame count of a clip is exact or refused", [] {
        DocumentClip clip = ZoomClip();
        int64_t frames = 0;
        std::string error;
        Check(ClipFrameCount(clip, kRate, frames, error) && frames == 25,
              "a whole number of frames: " + error);
        clip.duration = RationalTime(1, 3);
        Check(!ClipFrameCount(clip, kRate, frames, error),
              "a duration that is not a whole frame is refused, not rounded");
    });

    Test("opacity is sampled like any other parameter", [] {
        DocumentClip clip;
        clip.id = "01K90000000000000000000C03";
        clip.duration = RationalTime(4, 25);
        clip.animation = {Track(ClipAnimatedParameter::Opacity,
                                {Key("01K90000000000000000000K10", 0, 1, 1),
                                 Key("01K90000000000000000000K11", 4, 0, 1)})};
        ClipSampledParameters sampled;
        std::string error;
        Check(SampleClipParameters(clip, 2, kRate, sampled, error) &&
                  sampled.opacity.num == 1 && sampled.opacity.den == 2,
              "the flash is at half after two of its four frames: " + error);
        Check(SampleClipParameters(clip, 0, kRate, sampled, error) &&
                  sampled.opacity.num == sampled.opacity.den,
              "and opaque on its first");
    });

    Test("a clip that animates nothing keeps its constants", [] {
        DocumentClip clip;
        clip.id = "01K90000000000000000000C04";
        clip.opacity = EffectParamValue{1, 2};
        clip.framing.scale = EffectParamValue{11, 10};
        ClipSampledParameters sampled;
        std::string error;
        Check(SampleClipParameters(clip, 7, kRate, sampled, error) &&
                  sampled.opacity.num == 1 && sampled.opacity.den == 2 &&
                  sampled.framing.scale.num == 11 &&
                  sampled.framing.scale.den == 10,
              "the fields themselves, untouched: " + error);
    });

    // R2c -- docs/roadmap/REEL-2026-09.md. The first click on the
    // Inspector's diamond poses one key: it holds its value on every frame.
    Test("a single key is valid and holds its value over the clip", [] {
        DocumentClip clip = ZoomClip();
        clip.framing.scale = EffectParamValue{1, 1};
        clip.animation = {Track(ClipAnimatedParameter::FramingScale,
                                {Key("01K90000000000000000000K01", 12, 6, 5)})};
        std::string error;
        Check(ClipAnimationIsValid(clip.animation, error),
              "one key is accepted: " + error);
        for (const int64_t frame :
             {int64_t{-3}, int64_t{0}, int64_t{12}, int64_t{24}, int64_t{40}}) {
            Check(SampledScale(clip, frame).num == 6 &&
                      SampledScale(clip, frame).den == 5,
                  "and holds 6/5 on frame " + std::to_string(frame) +
                      ", over the constant 1");
        }
    });

    Test("a key belongs to the frame its time falls in", [] {
        DocumentClip clip = ZoomClip();
        // An agent's key between frames 4 and 5, at 4.5 frames.
        ClipKeyframe between = Key("01K90000000000000000000K03", 0, 11, 10);
        between.time = RationalTime(9, 50);
        clip.animation[0].keyframes.insert(
            clip.animation[0].keyframes.begin() + 1, between);
        Check(ClipKeyframeFrame(between, kRate) == 4,
              "4.5 frames falls on frame 4");
        ClipKeyframe before = Key("01K90000000000000000000K04", 0, 1, 1);
        before.time = RationalTime(-1, 50);
        Check(ClipKeyframeFrame(before, kRate) == -1,
              "half a frame before the head falls on frame -1");
        const ClipKeyframe* found = FindClipKeyframeAt(
            clip, ClipAnimatedParameter::FramingScale, 4, kRate);
        Check(found != nullptr && found->id == between.id,
              "and is found there");
        Check(FindClipKeyframeAt(clip, ClipAnimatedParameter::FramingScale, 5,
                                 kRate) == nullptr,
              "not on the next frame");
        Check(FindClipKeyframeAt(clip, ClipAnimatedParameter::Opacity, 0,
                                 kRate) == nullptr,
              "nor on a parameter the clip does not animate");
        Check(
            PreviousClipKeyframeFrame(clip, ClipAnimatedParameter::FramingScale,
                                      4, kRate) == 0 &&
                NextClipKeyframeFrame(clip, ClipAnimatedParameter::FramingScale,
                                      4, kRate) == 12,
            "the keys before and after frame 4 are on 0 and 12");
        Check(!PreviousClipKeyframeFrame(
                  clip, ClipAnimatedParameter::FramingScale, 0, kRate) &&
                  !NextClipKeyframeFrame(
                      clip, ClipAnimatedParameter::FramingScale, 12, kRate),
              "and there is none before the first or after the last");
    });

    Test("a key is posed, moved to a value and removed at a frame", [] {
        const DocumentClip clip = ZoomClip();
        std::vector<ClipParameterAnimation> animation;
        std::string error;
        // On a frame between the two keys: a new key, in order, with the
        // curve of the key before it.
        DocumentClip eased = clip;
        eased.animation[0].keyframes[0].interpolation =
            KeyframeInterpolation::CubicBezier;
        eased.animation[0].keyframes[0].easing_x1 = EffectParamValue{2, 5};
        Check(ClipAnimationWithKey(eased, ClipAnimatedParameter::FramingScale,
                                   6, kRate, EffectParamValue{5, 4},
                                   "01K90000000000000000000K09", animation,
                                   error) &&
                  animation.size() == 1 && animation[0].keyframes.size() == 3,
              "a key added between the two: " + error);
        if (animation.size() == 1 && animation[0].keyframes.size() == 3) {
            const ClipKeyframe& added = animation[0].keyframes[1];
            Check(added.id == "01K90000000000000000000K09" &&
                      added.time.compare(RationalTime(6, 25)) == 0 &&
                      added.value.num == 5 && added.value.den == 4,
                  "on the frame's exact time, with its value");
            Check(added.interpolation == KeyframeInterpolation::CubicBezier &&
                      added.easing_x1.num == 2 && added.easing_x1.den == 5,
                  "travelling on as the key before it did");
        }
        // On a key's frame: the key takes the value and keeps the rest.
        Check(
            ClipAnimationWithKey(clip, ClipAnimatedParameter::FramingScale, 12,
                                 kRate, EffectParamValue{3, 2},
                                 "01K90000000000000000000K10", animation,
                                 error) &&
                animation[0].keyframes.size() == 2 &&
                animation[0].keyframes[1].id == "01K90000000000000000000K02" &&
                animation[0].keyframes[1].value.num == 3,
            "the key on frame 12 takes 3/2, its identity kept: " + error);
        // Before the first key: a first key, travelling linearly.
        DocumentClip late = clip;
        late.animation[0].keyframes[0].time = RationalTime(3, 25);
        late.animation[0].keyframes[0].interpolation =
            KeyframeInterpolation::Hold;
        Check(ClipAnimationWithKey(late, ClipAnimatedParameter::FramingScale, 0,
                                   kRate, EffectParamValue{1, 1},
                                   "01K90000000000000000000K11", animation,
                                   error) &&
                  animation[0].keyframes.front().id ==
                      "01K90000000000000000000K11" &&
                  animation[0].keyframes.front().interpolation ==
                      KeyframeInterpolation::Linear,
              "a new first key travels linearly: " + error);
        // A parameter without keys gets an entry, in the parameters' order.
        Check(
            ClipAnimationWithKey(clip, ClipAnimatedParameter::Opacity, 3, kRate,
                                 EffectParamValue{1, 2},
                                 "01K90000000000000000000K12", animation,
                                 error) &&
                animation.size() == 2 &&
                animation[0].parameter == ClipAnimatedParameter::Opacity &&
                animation[0].keyframes.size() == 1 &&
                animation[1].parameter == ClipAnimatedParameter::FramingScale,
            "the opacity's first key comes before the scale's entry: " + error);
        Check(ClipAnimationIsValid(animation, error),
              "and the animation is valid: " + error);
        Check(!ClipAnimationWithKey(clip, ClipAnimatedParameter::Opacity, 3,
                                    kRate, EffectParamValue{3, 2},
                                    "01K90000000000000000000K13", animation,
                                    error),
              "an opacity above 1 is refused");
        // Removed: the key goes, then the entry with its last key.
        Check(ClipAnimationWithoutKey(clip, ClipAnimatedParameter::FramingScale,
                                      12, kRate, animation, error) &&
                  animation.size() == 1 && animation[0].keyframes.size() == 1,
              "the key on 12 goes, one is left: " + error);
        DocumentClip single = clip;
        single.animation[0].keyframes.pop_back();
        Check(
            ClipAnimationWithoutKey(single, ClipAnimatedParameter::FramingScale,
                                    0, kRate, animation, error) &&
                animation.empty(),
            "the last key takes its parameter's entry with it: " + error);
        Check(
            !ClipAnimationWithoutKey(clip, ClipAnimatedParameter::FramingScale,
                                     5, kRate, animation, error) &&
                error.find("frame 5") != std::string::npos,
            "no key on frame 5: refused by name: " + error);
    });

    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all clip animation tests passed\n";
    return 0;
}
