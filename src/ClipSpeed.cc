#include "ClipSpeed.h"

#include "ClipAnimation.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace {

int64_t Gcd(int64_t left, int64_t right) {
    while (right != 0) {
        const int64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left < 0 ? -left : left;
}

int64_t Checked(__int128 value, const char* what) {
    if (value < std::numeric_limits<int64_t>::min() ||
        value > std::numeric_limits<int64_t>::max()) {
        throw std::overflow_error(std::string(what) + " overflows int64_t");
    }
    return static_cast<int64_t>(value);
}

// value/rate with `factor` applied to the numerator and `divisor` to the
// denominator, kept exact. The spelling is left alone while it fits: a
// reduction would change the rate a trim stores, and the document writes
// value and rate as they are.
RationalTime Apply(const RationalTime& time, int64_t factor, int64_t divisor) {
    if (time.rate <= 0) {
        throw std::invalid_argument("RationalTime rate must be positive");
    }
    __int128 value = static_cast<__int128>(time.value) * factor;
    __int128 rate = static_cast<__int128>(time.rate) * divisor;
    if (rate > std::numeric_limits<int32_t>::max() ||
        value < std::numeric_limits<int64_t>::min() ||
        value > std::numeric_limits<int64_t>::max()) {
        const int64_t common =
            Gcd(Checked(value < 0 ? -value : value, "clip speed numerator"),
                Checked(rate, "clip speed rate"));
        if (common > 1) {
            value /= common;
            rate /= common;
        }
    }
    if (rate <= 0 || rate > std::numeric_limits<int32_t>::max()) {
        throw std::overflow_error("clip speed rate overflows int32_t");
    }
    return {Checked(value, "clip speed time"), static_cast<int32_t>(rate)};
}

// Criterion 4 -- an exact fraction for a ramp's integral. 128 bits because
// the parabola squares a time; reduced after every step and checked, so a
// product that would not fit is an overflow_error and never a wrapped
// value. Nothing here goes through a double.
struct Exact {
    __int128 num = 0;
    __int128 den = 1;
};

__int128 Magnitude(__int128 value) { return value < 0 ? -value : value; }

__int128 Gcd128(__int128 left, __int128 right) {
    left = Magnitude(left);
    right = Magnitude(right);
    while (right != 0) {
        const __int128 remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

__int128 Times128(__int128 left, __int128 right) {
    __int128 result = 0;
    if (__builtin_mul_overflow(left, right, &result)) {
        throw std::overflow_error("clip speed ramp integral overflows");
    }
    return result;
}

__int128 Plus128(__int128 left, __int128 right) {
    __int128 result = 0;
    if (__builtin_add_overflow(left, right, &result)) {
        throw std::overflow_error("clip speed ramp integral overflows");
    }
    return result;
}

Exact Make(__int128 num, __int128 den) {
    if (den == 0) {
        throw std::invalid_argument("clip speed ramp: zero denominator");
    }
    if (den < 0) {
        num = -num;
        den = -den;
    }
    const __int128 common = Gcd128(num, den);
    if (common > 1) {
        num /= common;
        den /= common;
    }
    if (num == 0) {
        den = 1;
    }
    return {num, den};
}

Exact Sum(const Exact& left, const Exact& right) {
    const __int128 common = Gcd128(left.den, right.den);
    const __int128 leftScale = right.den / common;
    const __int128 rightScale = left.den / common;
    return Make(
        Plus128(Times128(left.num, leftScale), Times128(right.num, rightScale)),
        Times128(left.den, leftScale));
}

Exact Difference(const Exact& left, const Exact& right) {
    return Sum(left, {-right.num, right.den});
}

Exact Product(const Exact& left, const Exact& right) {
    // Cross-reduced first, so the products stay as small as the result.
    const __int128 first = Gcd128(left.num, right.den);
    const __int128 second = Gcd128(right.num, left.den);
    const __int128 a = first > 1 ? first : 1;
    const __int128 b = second > 1 ? second : 1;
    return Make(Times128(left.num / a, right.num / b),
                Times128(left.den / b, right.den / a));
}

Exact Quotient(const Exact& left, const Exact& right) {
    if (right.num == 0) {
        throw std::invalid_argument("clip speed ramp: division by zero");
    }
    return Product(left, {right.den, right.num});
}

int Sign(const Exact& value) {
    return value.num < 0 ? -1 : (value.num > 0 ? 1 : 0);
}

Exact OfTime(const RationalTime& time) {
    if (time.rate <= 0) {
        throw std::invalid_argument("RationalTime rate must be positive");
    }
    return Make(time.value, time.rate);
}

Exact OfValue(const EffectParamValue& value) {
    return Make(value.num, value.den);
}

// Towards -infinity, as RationalTime::to_frames floors.
int64_t Floor(const Exact& value) {
    __int128 quotient = value.num / value.den;
    if (value.num < 0 && value.num % value.den != 0) {
        --quotient;
    }
    return Checked(quotient, "clip speed ramp frame");
}

// The one place a ramp's integral becomes a RationalTime: reduced, then
// refused when its denominator has no int32_t spelling. Never rounded.
RationalTime AsTime(const Exact& value) {
    if (value.den > std::numeric_limits<int32_t>::max()) {
        throw std::overflow_error(
            "clip speed ramp: the source time has no exact RationalTime "
            "spelling (its denominator overflows int32_t)");
    }
    return {Checked(value.num, "clip speed ramp time"),
            static_cast<int32_t>(value.den)};
}

// The integral of a ramp's speed from its first key to local time `x`. The
// speed is the first key's before it and the last key's after it (R2's
// clamp); between two keys it is held, or it travels in a straight line,
// whose integral over the first `d` of a segment [a, b] is
//
//     v_a d + (v_b - v_a) d^2 / (2 (b - a))
//
// -- a parabola, exact at any rational `d`. Differences of this function
// are every integral the clip needs.
Exact RampPrimitive(const std::vector<ClipKeyframe>& ramp, const Exact& x) {
    const Exact first = OfTime(ramp.front().time);
    if (Sign(Difference(x, first)) <= 0) {
        return Product(OfValue(ramp.front().value), Difference(x, first));
    }
    Exact area;
    for (size_t index = 0; index + 1 < ramp.size(); ++index) {
        const Exact a = OfTime(ramp[index].time);
        const Exact b = OfTime(ramp[index + 1].time);
        const bool inside = Sign(Difference(x, b)) < 0;
        const Exact d = Difference(inside ? x : b, a);
        const Exact from = OfValue(ramp[index].value);
        Exact piece = Product(from, d);
        if (ramp[index].interpolation == KeyframeInterpolation::Linear) {
            const Exact rise = Difference(OfValue(ramp[index + 1].value), from);
            piece = Sum(piece, Quotient(Product(rise, Product(d, d)),
                                        Product({2, 1}, Difference(b, a))));
        }
        area = Sum(area, piece);
        if (inside) {
            return area;
        }
    }
    return Sum(area, Product(OfValue(ramp.back().value),
                             Difference(x, OfTime(ramp.back().time))));
}

// The integral between two of the clip's local times.
Exact RampIntegral(const DocumentClip& clip, const RationalTime& from,
                   const RationalTime& to) {
    return Difference(RampPrimitive(clip.speed_ramp, OfTime(to)),
                      RampPrimitive(clip.speed_ramp, OfTime(from)));
}

bool Faster(const EffectParamValue& left, const EffectParamValue& right) {
    return static_cast<int64_t>(left.num) * right.den >
           static_cast<int64_t>(right.num) * left.den;
}

}  // namespace

bool ClipSpeedIsValid(const EffectParamValue& speed, std::string& error) {
    if (speed.num < 0 || speed.den <= 0) {
        error =
            "clip speed must be a positive fraction, or 0/1 to hold a frame; "
            "a reverse is not a speed";
        return false;
    }
    if (speed.num > kMaxClipSpeedTerm || speed.den > kMaxClipSpeedTerm) {
        error = "clip speed terms must stay at or below " +
                std::to_string(kMaxClipSpeedTerm);
        return false;
    }
    return true;
}

bool ClipSpeedRampIsValid(const std::vector<ClipKeyframe>& ramp,
                          std::string& error) {
    if (ramp.empty()) {
        return true;
    }
    if (ramp.size() < 2) {
        error =
            "a speed ramp needs at least two keys; one key is a constant "
            "speed, which is set as 'speed'";
        return false;
    }
    if (ramp.size() > kMaxClipKeyframes) {
        error = "a speed ramp carries at most " +
                std::to_string(kMaxClipKeyframes) + " keys";
        return false;
    }
    std::set<Ulid> ids;
    for (size_t index = 0; index < ramp.size(); ++index) {
        const ClipKeyframe& key = ramp[index];
        const std::string at = "speed ramp key " + std::to_string(index);
        if (key.id.empty() || !ids.insert(key.id).second) {
            error = at + " needs an id of its own";
            return false;
        }
        if (key.time.rate <= 0) {
            error = at + " has a time whose rate is not positive";
            return false;
        }
        if (index > 0 && !(ramp[index - 1].time < key.time)) {
            error = at + " is not after the key before it";
            return false;
        }
        std::string speedError;
        if (!ClipSpeedIsValid(key.value, speedError)) {
            error = at + ": " + speedError;
            return false;
        }
        if (key.interpolation != KeyframeInterpolation::Hold &&
            key.interpolation != KeyframeInterpolation::Linear) {
            error = at +
                    " is a cubic-bezier: a speed ramp's keys are 'hold' or "
                    "'linear', because the source position is the integral "
                    "of the speed and only those two keep it an exact "
                    "fraction (ClipSpeed.h); write a smooth ramp with more "
                    "linear keys";
            return false;
        }
    }
    return true;
}

RationalTime ScaleBySpeed(const RationalTime& time,
                          const EffectParamValue& speed) {
    if (IsUnitSpeed(speed)) {
        return time;
    }
    return Apply(time, speed.num, speed.den);
}

RationalTime UnscaleBySpeed(const RationalTime& sourceTime,
                            const EffectParamValue& speed) {
    if (IsUnitSpeed(speed)) {
        return sourceTime;
    }
    return Apply(sourceTime, speed.den, speed.num);
}

RationalTime ClipSourceAdvanceBetween(const DocumentClip& clip,
                                      const RationalTime& from,
                                      const RationalTime& to) {
    if (!ClipHasSpeedRamp(clip)) {
        return ScaleBySpeed(to.sub(from), clip.speed);
    }
    return AsTime(RampIntegral(clip, from, to));
}

RationalTime ClipSourceAdvance(const DocumentClip& clip,
                               const RationalTime& offset) {
    if (!ClipHasSpeedRamp(clip)) {
        return ScaleBySpeed(offset, clip.speed);
    }
    return AsTime(RampIntegral(clip, RationalTime{0, 1}, offset));
}

RationalTime ClipSourceSpan(const DocumentClip& clip) {
    return ClipSourceAdvance(clip, clip.duration);
}

RationalTime ClipSourceEnd(const DocumentClip& clip) {
    return clip.source_in.add(ClipSourceSpan(clip));
}

RationalTime ClipSourceTimeAt(const DocumentClip& clip,
                              const RationalTime& offset) {
    return clip.source_in.add(ClipSourceAdvance(clip, offset));
}

int64_t ClipSourceFrameAt(const DocumentClip& clip, const RationalTime& offset,
                          const MediaRate& sourceRate) {
    if (ClipHasSpeedRamp(clip)) {
        if (sourceRate.num <= 0 || sourceRate.den <= 0) {
            throw std::invalid_argument("source rate must be positive");
        }
        const Exact position =
            Sum(OfTime(clip.source_in),
                RampIntegral(clip, RationalTime{0, 1}, offset));
        return Floor(Product(position, Make(sourceRate.num, sourceRate.den)));
    }
    return ClipSourceTimeAt(clip, offset)
        .to_frames(sourceRate.num, sourceRate.den);
}

std::vector<int64_t> ClipSourceFrames(const DocumentClip& clip, int64_t first,
                                      int64_t count,
                                      const MediaRate& sequenceRate,
                                      const MediaRate& sourceRate) {
    std::vector<int64_t> frames;
    if (count <= 0) {
        return frames;
    }
    frames.reserve(static_cast<size_t>(count));
    for (int64_t frame = first; frame < first + count; ++frame) {
        frames.push_back(ClipSourceFrameAt(
            clip, RationalTime{frame * sequenceRate.den, sequenceRate.num},
            sourceRate));
    }
    return frames;
}

EffectParamValue ClipPeakSpeed(const DocumentClip& clip) {
    if (!ClipHasSpeedRamp(clip)) {
        return clip.speed;
    }
    EffectParamValue peak = clip.speed_ramp.front().value;
    for (const ClipKeyframe& key : clip.speed_ramp) {
        if (Faster(key.value, peak)) {
            peak = key.value;
        }
    }
    return peak;
}

int64_t ClipFirstLocalFrameShowing(const DocumentClip& clip,
                                   int64_t sourceFrame, int64_t frames,
                                   const MediaRate& sequenceRate,
                                   const MediaRate& sourceRate) {
    const auto shown = [&](int64_t frame) {
        return ClipSourceFrameAt(
            clip, RationalTime{frame * sequenceRate.den, sequenceRate.num},
            sourceRate);
    };
    // The rule never goes back -- no speed is negative -- so the first
    // frame at or past `sourceFrame` is found by halves.
    int64_t low = 0;
    int64_t high = frames;
    while (low < high) {
        const int64_t middle = low + (high - low) / 2;
        if (shown(middle) < sourceFrame) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < frames && shown(low) == sourceFrame ? low : -1;
}

void ShiftClipSpeedRampHead(DocumentClip& clip, const RationalTime& advance) {
    if (clip.speed_ramp.empty() || advance.value == 0) {
        return;
    }
    for (ClipKeyframe& key : clip.speed_ramp) {
        key.time = key.time.sub(advance);
    }
}

RationalTime SourceFrameStart(const RationalTime& time, const MediaRate& rate) {
    if (time.rate <= 0 || rate.num <= 0 || rate.den <= 0) {
        return time;
    }
    return {time.to_frames(rate.num, rate.den) * rate.den, rate.num};
}

bool IsOnSourceFrameBoundary(const RationalTime& time, const MediaRate& rate) {
    if (time.rate <= 0 || rate.num <= 0 || rate.den <= 0) {
        return false;
    }
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    return numerator % denominator == 0;
}

MediaRate SpeedAdjustedSourceRate(const MediaRate& sourceRate,
                                  const EffectParamValue& speed) {
    if (IsUnitSpeed(speed)) {
        return sourceRate;
    }
    __int128 num = static_cast<__int128>(sourceRate.num) * speed.num;
    __int128 den = static_cast<__int128>(sourceRate.den) * speed.den;
    const int64_t common = Gcd(Checked(num, "effective source rate"),
                               Checked(den, "effective source rate"));
    if (common > 1) {
        num /= common;
        den /= common;
    }
    if (num <= 0 || den <= 0 || num > std::numeric_limits<int32_t>::max() ||
        den > std::numeric_limits<int32_t>::max()) {
        throw std::overflow_error("effective source rate overflows int32_t");
    }
    return {static_cast<int32_t>(num), static_cast<int32_t>(den)};
}

std::string ClipSpeedRefusal(const DocumentClip& clip,
                             const std::string& what) {
    const std::string speed = ClipHasSpeedRamp(clip)
                                  ? std::string("a ramp")
                                  : std::to_string(clip.speed.num) + "/" +
                                        std::to_string(clip.speed.den);
    return what + " refuses clip '" + clip.id + "': its speed is " + speed +
           ", and this tool only maps source time onto a clip at 1/1 (R3, "
           "docs/roadmap/REEL-2026-09.md)";
}

std::string SpeedAtempoChain(const EffectParamValue& speed) {
    if (IsUnitSpeed(speed) || speed.num <= 0 || speed.den <= 0) {
        return std::string();
    }
    int64_t num = speed.num;
    int64_t den = speed.den;
    std::string chain;
    // atempo refuses a factor outside [0.5, 2] on older builds and below 0.5
    // on every build. Halving or doubling by exact fractions keeps each link
    // inside the window and the product exact.
    while (num * 2 < den) {
        chain += ",atempo=1/2";
        num *= 2;
    }
    while (num > den * 2) {
        chain += ",atempo=2";
        den *= 2;
    }
    const int64_t common = Gcd(num, den);
    chain += ",atempo=" + std::to_string(num / common) + '/' +
             std::to_string(den / common);
    return chain;
}
