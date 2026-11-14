#include "Stabilization.h"

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "Json.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {

using mcp_json::Value;

double Fraction(const EffectParamValue& value) {
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

// `path` averaged with a Gaussian of `sigma` frames, cut at three sigmas.
// Near an end of the clip the weights that fall outside are dropped and the
// rest renormalised: the first and last frames lean on the ones they have,
// which is what holds a shot's first frame near where it was composed.
std::vector<double> Smoothed(const std::vector<double>& path, double sigma) {
    const int64_t count = static_cast<int64_t>(path.size());
    const int64_t radius =
        std::max<int64_t>(1, static_cast<int64_t>(std::ceil(3.0 * sigma)));
    std::vector<double> weights(static_cast<size_t>(radius) + 1);
    for (int64_t offset = 0; offset <= radius; ++offset) {
        const double ratio = static_cast<double>(offset) / sigma;
        weights[static_cast<size_t>(offset)] = std::exp(-0.5 * ratio * ratio);
    }
    std::vector<double> smoothed(path.size());
    for (int64_t frame = 0; frame < count; ++frame) {
        double sum = 0.0;
        double total = 0.0;
        const int64_t first = std::max<int64_t>(0, frame - radius);
        const int64_t last = std::min<int64_t>(count - 1, frame + radius);
        for (int64_t other = first; other <= last; ++other) {
            const double weight =
                weights[static_cast<size_t>(std::llabs(other - frame))];
            sum += weight * path[static_cast<size_t>(other)];
            total += weight;
        }
        smoothed[static_cast<size_t>(frame)] = sum / total;
    }
    return smoothed;
}

// The smallest scale, over the one that fills, at which the room holds a
// position `base` moved by corrections between `low` and `high` (fractions
// of the frame at scale 1, so they grow with the scale as the picture
// does). `span` is the filled picture's size in frames at scale 1. Infinite
// when no scale does.
double ScaleHolding(double span, double base, double low, double high) {
    // base + high k <= (span k - 1) / 2  and  -(base + low k) <= the same.
    const auto bound = [&](double offset, double correction) {
        const double slope = span / 2.0 - correction;
        if (slope <= 1e-9) return std::numeric_limits<double>::infinity();
        return (offset + 0.5) / slope;
    };
    return std::max({1.0, bound(base, high), bound(-base, -low)});
}

struct Deviation {
    double rms = 0.0;
    double maximum = 0.0;
};

// Of the distances, in sequence pixels, a picture lies from where it should.
Deviation DeviationOf(const std::vector<double>& x,
                      const std::vector<double>& y) {
    Deviation deviation;
    double sum = 0.0;
    for (size_t frame = 0; frame < x.size(); ++frame) {
        const double squared = x[frame] * x[frame] + y[frame] * y[frame];
        sum += squared;
        deviation.maximum = std::max(deviation.maximum, std::sqrt(squared));
    }
    if (!x.empty())
        deviation.rms = std::sqrt(sum / static_cast<double>(x.size()));
    return deviation;
}

EffectParamValue Reduced(int64_t numerator, int64_t denominator) {
    const int64_t divisor =
        std::gcd(numerator < 0 ? -numerator : numerator, denominator);
    if (divisor == 0) return {0, 1};
    return {static_cast<int32_t>(numerator / divisor),
            static_cast<int32_t>(denominator / divisor)};
}

}  // namespace

bool StabilizationSettingsAreValid(const StabilizationSettings& settings,
                                   std::string& error) {
    const RationalTime& smoothing = settings.smoothing;
    if (smoothing.rate <= 0 || smoothing.value <= 0 ||
        smoothing > RationalTime(10, 1)) {
        error = "'smoothing' must be a time above zero, ten seconds at most";
        return false;
    }
    const EffectParamValue& scale = settings.max_scale;
    if (scale.den <= 0 || scale.num < scale.den || scale.num > 4 * scale.den) {
        error = "'max_scale' must lie in [1, 4]";
        return false;
    }
    const EffectParamValue& tolerance = settings.tolerance;
    if (tolerance.den <= 0 || tolerance.num <= 0 ||
        tolerance.num * 10 > tolerance.den) {
        error = "'tolerance' must lie in ]0, 1/10]";
        return false;
    }
    return true;
}

bool ComputeStabilization(StabilizationInput input,
                          const StabilizationSettings& settings,
                          StabilizationResult& result, std::string& error) {
    result = StabilizationResult{};
    error.clear();
    if (!StabilizationSettingsAreValid(settings, error)) return false;
    SubjectTrackingInput& clip = input.clip;
    if (clip.source_width <= 0 || clip.source_height <= 0 ||
        clip.frame_width <= 0 || clip.frame_height <= 0) {
        error = "the source and the sequence frame must have a size";
        return false;
    }
    if (clip.rate.num <= 0 || clip.rate.den <= 0 || clip.frame_count < 3) {
        error = "a clip of fewer than three frames has no path to steady";
        return false;
    }
    if (clip.framing.mode != FramingMode::Fill) {
        error =
            "a steadied framing moves inside what 'fill' cuts: a framing "
            "that fits has no room to move in";
        return false;
    }
    if (!ClipFramingIsValid(clip.framing, error)) return false;
    const size_t count = static_cast<size_t>(clip.frame_count);
    if (input.motion.size() != count) {
        error = "the motion holds " + std::to_string(input.motion.size()) +
                " samples for a clip of " + std::to_string(count) + " frames";
        return false;
    }
    result.frames = clip.frame_count;

    // The path: where the picture has drifted to, in fractions of the
    // source.
    std::vector<double> pathX(count, 0.0);
    std::vector<double> pathY(count, 0.0);
    for (size_t frame = 1; frame < count; ++frame) {
        const StabilizationSample& sample = input.motion[frame];
        pathX[frame] = pathX[frame - 1];
        pathY[frame] = pathY[frame - 1];
        if (!sample.measured || !std::isfinite(sample.translation_x) ||
            !std::isfinite(sample.translation_y)) {
            ++result.frames_unmeasured;
            continue;
        }
        pathX[frame] += sample.translation_x;
        pathY[frame] += sample.translation_y;
        if (std::isfinite(sample.zoom))
            result.zoom_max = std::max(result.zoom_max, std::fabs(sample.zoom));
    }
    if (result.frames_unmeasured == clip.frame_count - 1) {
        error =
            "no movement could be measured between the clip's frames: "
            "nothing to steady";
        return false;
    }
    const double framesPerSecond =
        static_cast<double>(clip.rate.num) / static_cast<double>(clip.rate.den);
    const double sigma = std::max(
        0.5, framesPerSecond * static_cast<double>(settings.smoothing.value) /
                 static_cast<double>(settings.smoothing.rate));
    const std::vector<double> smoothX = Smoothed(pathX, sigma);
    const std::vector<double> smoothY = Smoothed(pathY, sigma);

    // The correction, in frames at scale 1: a fraction of the source is
    // that fraction of the picture as drawn, which is `span` frames wide
    // for what the crops keep of it.
    ClipFraming unit = clip.framing;
    unit.scale = {1, 1};
    SubjectTrackingInput atUnit = clip;
    atUnit.framing = unit;
    const FollowedFramingRoom unitRoom = FramingRoomOf(atUnit);
    const double keptX = 1.0 - Fraction(clip.framing.crop_left) -
                         Fraction(clip.framing.crop_right);
    const double keptY = 1.0 - Fraction(clip.framing.crop_top) -
                         Fraction(clip.framing.crop_bottom);
    std::vector<double> correctionX(count);
    std::vector<double> correctionY(count);
    for (size_t frame = 0; frame < count; ++frame) {
        correctionX[frame] =
            (smoothX[frame] - pathX[frame]) * unitRoom.span_x / keptX;
        correctionY[frame] =
            (smoothY[frame] - pathY[frame]) * unitRoom.span_y / keptY;
    }
    const auto [lowX, highX] =
        std::minmax_element(correctionX.begin(), correctionX.end());
    const auto [lowY, highY] =
        std::minmax_element(correctionY.begin(), correctionY.end());
    const double baseX = Fraction(clip.framing.position_x);
    const double baseY = Fraction(clip.framing.position_y);

    // The scale: the smallest that holds every correction, the clip's own
    // at least, max_scale at most; or the one the caller gave.
    const double own = Fraction(clip.framing.scale);
    result.scale_needed =
        std::max({own, ScaleHolding(unitRoom.span_x, baseX, *lowX, *highX),
                  ScaleHolding(unitRoom.span_y, baseY, *lowY, *highY)});
    if (!input.scale_given) {
        const double ceiling = std::max(own, Fraction(settings.max_scale));
        // A thousandth above what is needed, on thousandths: the room is
        // floored onto the sample grid, and the last correction must fit.
        const double wanted = std::min(ceiling, result.scale_needed * 1.001);
        const int64_t thousandths =
            static_cast<int64_t>(std::ceil(wanted * 1000.0 - 1e-9));
        const EffectParamValue chosen = Reduced(thousandths, 1000);
        if (Fraction(chosen) > own + 1e-12) {
            clip.framing.scale = chosen;
            result.scale_chosen = true;
        }
    }
    result.scale = clip.framing.scale;
    const double scale = Fraction(clip.framing.scale);
    const FollowedFramingRoom room = FramingRoomOf(clip);
    if (room.room_x == 0 && room.room_y == 0) {
        error =
            "the filled picture does not overrun the frame on either axis: "
            "a correction has no room to move in (raise 'scale' or "
            "'max_scale')";
        return false;
    }
    const double maxRoomX = static_cast<double>(room.room_x) /
                            static_cast<double>(kAnimationSampleDenominator);
    const double maxRoomY = static_cast<double>(room.room_y) /
                            static_cast<double>(kAnimationSampleDenominator);
    std::vector<double> positionX(count);
    std::vector<double> positionY(count);
    for (size_t frame = 0; frame < count; ++frame) {
        positionX[frame] = baseX + correctionX[frame] * scale;
        positionY[frame] = baseY + correctionY[frame] * scale;
        if (std::fabs(positionX[frame]) > maxRoomX + 1e-9)
            ++result.x.frames_at_edge;
        if (std::fabs(positionY[frame]) > maxRoomY + 1e-9)
            ++result.y.frames_at_edge;
    }
    const int64_t tolerance = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::floor(Fraction(settings.tolerance) *
                          static_cast<double>(kAnimationSampleDenominator))));
    if (!FillPositionAxis(positionX, room.room_x, tolerance, clip.rate,
                          result.x, error) ||
        !FillPositionAxis(positionY, room.room_y, tolerance, clip.rate,
                          result.y, error))
        return false;

    // What it did, in sequence pixels: how far the picture strayed from
    // its smoothed path, and how far it still does behind the keys --
    // sampled between them as the renderers do, linearly.
    const auto laid = [&](const SubjectTrackingAxis& axis) {
        std::vector<double> values(count);
        if (!axis.animated) {
            std::fill(values.begin(), values.end(), Fraction(axis.constant));
            return values;
        }
        size_t next = 0;
        for (size_t frame = 0; frame < count; ++frame) {
            const RationalTime time(static_cast<int64_t>(frame) * clip.rate.den,
                                    clip.rate.num);
            while (next + 1 < axis.keyframes.size() &&
                   axis.keyframes[next + 1].time <= time)
                ++next;
            const ClipKeyframe& from = axis.keyframes[next];
            if (next + 1 >= axis.keyframes.size() || time <= from.time) {
                values[frame] = Fraction(from.value);
                continue;
            }
            const ClipKeyframe& to = axis.keyframes[next + 1];
            const RationalTime span = to.time.sub(from.time);
            const RationalTime into = time.sub(from.time);
            const double progress =
                (static_cast<double>(into.value) / into.rate) /
                (static_cast<double>(span.value) / span.rate);
            values[frame] =
                Fraction(from.value) +
                (Fraction(to.value) - Fraction(from.value)) * progress;
        }
        return values;
    };
    const std::vector<double> laidX = laid(result.x);
    const std::vector<double> laidY = laid(result.y);
    const double width = static_cast<double>(clip.frame_width);
    const double height = static_cast<double>(clip.frame_height);
    std::vector<double> beforeX(count), beforeY(count);
    std::vector<double> afterX(count), afterY(count);
    for (size_t frame = 0; frame < count; ++frame) {
        beforeX[frame] = correctionX[frame] * scale * width;
        beforeY[frame] = correctionY[frame] * scale * height;
        afterX[frame] = (positionX[frame] - laidX[frame]) * width;
        afterY[frame] = (positionY[frame] - laidY[frame]) * height;
    }
    const Deviation before = DeviationOf(beforeX, beforeY);
    const Deviation after = DeviationOf(afterX, afterY);
    result.shake_before_rms = before.rms;
    result.shake_before_max = before.maximum;
    result.shake_after_rms = after.rms;
    result.shake_after_max = after.maximum;
    return true;
}

bool BuildStabilizationOperations(
    const Document& document, const Ulid& clipId,
    const std::vector<StabilizationSample>& motion,
    const StabilizationSettings& settings,
    const std::optional<EffectParamValue>& scale,
    const std::function<Ulid()>& newId, std::vector<Operation>& operations,
    StabilizationResult& result, bool& modeChanged, std::string& error) {
    operations.clear();
    StabilizationInput input;
    if (!PrepareFollowedFraming(document, clipId, scale, "steady", input.clip,
                                modeChanged, error))
        return false;
    input.motion = motion;
    input.scale_given = scale.has_value();
    if (!ComputeStabilization(input, settings, result, error)) {
        error = "clip '" + clipId + "': " + error;
        return false;
    }
    ClipFraming framing = input.clip.framing;
    framing.scale = result.scale;
    return LayFollowedFraming(document, clipId, framing, result.x, result.y,
                              newId, operations, error);
}

std::string SerializeStabilizationResult(const StabilizationResult& result) {
    const auto fraction = [](const EffectParamValue& value) {
        Value item = Value::MakeObject();
        item.Set("num", Value::MakeInt(value.num));
        item.Set("den", Value::MakeInt(value.den));
        return item;
    };
    const auto axis = [&](const SubjectTrackingAxis& from) {
        Value item = Value::MakeObject();
        item.Set("animated", Value::MakeBool(from.animated));
        item.Set("keys",
                 Value::MakeInt(static_cast<int64_t>(from.keyframes.size())));
        item.Set("frames_at_edge", Value::MakeInt(from.frames_at_edge));
        item.Set("room", fraction(from.room));
        return item;
    };
    // Pixels to the hundredth: what the measure is worth.
    const auto pixels = [](double value) {
        return Value::MakeDouble(std::round(value * 100.0) / 100.0);
    };
    Value json = Value::MakeObject();
    json.Set("frames", Value::MakeInt(result.frames));
    json.Set("frames_unmeasured", Value::MakeInt(result.frames_unmeasured));
    json.Set("scale", fraction(result.scale));
    json.Set("scale_chosen", Value::MakeBool(result.scale_chosen));
    json.Set("scale_needed",
             Value::MakeDouble(std::isfinite(result.scale_needed)
                                   ? std::round(result.scale_needed * 1000.0) /
                                         1000.0
                                   : 0.0));
    json.Set("scale_needed_is_finite",
             Value::MakeBool(std::isfinite(result.scale_needed)));
    Value shake = Value::MakeObject();
    shake.Set("before_rms_px", pixels(result.shake_before_rms));
    shake.Set("before_max_px", pixels(result.shake_before_max));
    shake.Set("left_rms_px", pixels(result.shake_after_rms));
    shake.Set("left_max_px", pixels(result.shake_after_max));
    json.Set("shake", std::move(shake));
    json.Set("zoom_not_corrected_max_percent",
             Value::MakeDouble(std::round(result.zoom_max * 10000.0) / 100.0));
    json.Set("x", axis(result.x));
    json.Set("y", axis(result.y));
    return json.Dump();
}

void StabilizationAnalysisSize(int64_t displayedWidth, int64_t displayedHeight,
                               int32_t& width, int32_t& height) {
    if (displayedWidth > displayedHeight) {
        width = 768;
        height = 432;
    } else if (displayedWidth < displayedHeight) {
        width = 432;
        height = 768;
    } else {
        width = 576;
        height = 576;
    }
}
