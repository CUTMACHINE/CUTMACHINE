#include "Keyframes.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

bool TimeLess(const Keyframe& keyframe, const RationalTime& time) {
    return keyframe.time < time;
}

bool TimeLessKeyframe(const Keyframe& left, const Keyframe& right) {
    return left.time < right.time;
}

// Returns the exact (position - left) / (right - left) rational converted to
// long double only after cancellation into a dimensionless fraction.
long double InterpolationFraction(const RationalTime& position,
                                  const RationalTime& left,
                                  const RationalTime& right) {
    const __int128 fromLeft =
        static_cast<__int128>(position.value) * left.rate -
        static_cast<__int128>(left.value) * position.rate;
    const __int128 span = static_cast<__int128>(right.value) * left.rate -
                          static_cast<__int128>(left.value) * right.rate;
    const __int128 numerator = fromLeft * right.rate;
    const __int128 denominator = span * position.rate;
    return static_cast<long double>(numerator) /
           static_cast<long double>(denominator);
}

}  // namespace

const char* KeyframeErrorName(KeyframeError error) {
    switch (error) {
        case KeyframeError::None:
            return "none";
        case KeyframeError::InvalidId:
            return "invalid_id";
        case KeyframeError::InvalidTime:
            return "invalid_time";
        case KeyframeError::InvalidInterpolation:
            return "invalid_interpolation";
        case KeyframeError::NonFiniteValue:
            return "non_finite_value";
        case KeyframeError::DuplicateId:
            return "duplicate_id";
        case KeyframeError::DuplicateTime:
            return "duplicate_time";
        case KeyframeError::NotFound:
            return "not_found";
        case KeyframeError::EmptyTrack:
            return "empty_track";
        case KeyframeError::NumericOverflow:
            return "numeric_overflow";
    }
    return "unknown";
}

KeyframeResult KeyframeTrack::Validate(const Keyframe& keyframe) {
    if (!IsValidUlid(keyframe.id)) {
        return {KeyframeError::InvalidId};
    }
    if (keyframe.time.rate <= 0) {
        return {KeyframeError::InvalidTime};
    }
    if (keyframe.interpolation != KeyframeInterpolation::Hold &&
        keyframe.interpolation != KeyframeInterpolation::Linear &&
        keyframe.interpolation != KeyframeInterpolation::CubicBezier) {
        return {KeyframeError::InvalidInterpolation};
    }
    if (keyframe.interpolation == KeyframeInterpolation::CubicBezier) {
        const KeyframeEasing& easing = keyframe.easing;
        if (!std::isfinite(easing.x1) || !std::isfinite(easing.y1) ||
            !std::isfinite(easing.x2) || !std::isfinite(easing.y2) ||
            easing.x1 < 0.0 || easing.x1 > 1.0 || easing.x2 < 0.0 ||
            easing.x2 > 1.0) {
            return {KeyframeError::InvalidInterpolation};
        }
    }
    if (!std::isfinite(keyframe.value)) {
        return {KeyframeError::NonFiniteValue};
    }
    return {};
}

KeyframeResult KeyframeTrack::Add(Keyframe keyframe) {
    const KeyframeResult validation = Validate(keyframe);
    if (!validation) {
        return validation;
    }
    if (std::any_of(keyframes_.begin(), keyframes_.end(),
                    [&](const Keyframe& existing) {
                        return existing.id == keyframe.id;
                    })) {
        return {KeyframeError::DuplicateId};
    }

    const auto insertion = std::lower_bound(
        keyframes_.begin(), keyframes_.end(), keyframe, TimeLessKeyframe);
    if (insertion != keyframes_.end() && insertion->time == keyframe.time) {
        return {KeyframeError::DuplicateTime};
    }
    keyframes_.insert(insertion, std::move(keyframe));
    return {};
}

KeyframeResult KeyframeTrack::Remove(const Ulid& id) {
    const auto found = std::find_if(
        keyframes_.begin(), keyframes_.end(),
        [&](const Keyframe& keyframe) { return keyframe.id == id; });
    if (found == keyframes_.end()) {
        return {KeyframeError::NotFound};
    }
    keyframes_.erase(found);
    return {};
}

KeyframeResult KeyframeTrack::Update(const Ulid& id, RationalTime time,
                                     double value,
                                     KeyframeInterpolation interpolation,
                                     KeyframeEasing easing) {
    const auto found = std::find_if(
        keyframes_.begin(), keyframes_.end(),
        [&](const Keyframe& keyframe) { return keyframe.id == id; });
    if (found == keyframes_.end()) {
        return {KeyframeError::NotFound};
    }

    Keyframe replacement{id, time, value, interpolation, easing};
    const KeyframeResult validation = Validate(replacement);
    if (!validation) {
        return validation;
    }
    if (std::any_of(keyframes_.begin(), keyframes_.end(),
                    [&](const Keyframe& keyframe) {
                        return keyframe.id != id && keyframe.time == time;
                    })) {
        return {KeyframeError::DuplicateTime};
    }

    // Validation precedes mutation, making a rejected update atomic.
    keyframes_.erase(found);
    const auto insertion = std::lower_bound(
        keyframes_.begin(), keyframes_.end(), replacement, TimeLessKeyframe);
    keyframes_.insert(insertion, std::move(replacement));
    return {};
}

KeyframeEvaluation KeyframeTrack::Evaluate(RationalTime time,
                                           KeyframeExtrapolation left,
                                           KeyframeExtrapolation right) const {
    if (time.rate <= 0) {
        return {KeyframeError::InvalidTime, 0.0};
    }
    if (keyframes_.empty()) {
        return {KeyframeError::EmptyTrack, 0.0};
    }

    // Straight-line continuation through two keyframes, used only outside the
    // keyed range. The fraction stays exact until it is dimensionless.
    const auto extend = [&](const Keyframe& from,
                            const Keyframe& to) -> KeyframeEvaluation {
        if (from.interpolation == KeyframeInterpolation::Hold) {
            return {KeyframeError::None,
                    time < from.time ? from.value : to.value};
        }
        const long double fraction =
            InterpolationFraction(time, from.time, to.time);
        const double value = static_cast<double>(
            static_cast<long double>(from.value) +
            (static_cast<long double>(to.value) - from.value) * fraction);
        if (!std::isfinite(value)) {
            return {KeyframeError::NumericOverflow, 0.0};
        }
        return {KeyframeError::None, value};
    };

    const auto right_key =
        std::lower_bound(keyframes_.begin(), keyframes_.end(), time, TimeLess);
    if (right_key == keyframes_.begin()) {
        if (right_key->time != time && left == KeyframeExtrapolation::Extend &&
            keyframes_.size() > 1) {
            return extend(keyframes_[0], keyframes_[1]);
        }
        return {KeyframeError::None, right_key->value};
    }
    if (right_key == keyframes_.end()) {
        if (right == KeyframeExtrapolation::Extend && keyframes_.size() > 1) {
            return extend(keyframes_[keyframes_.size() - 2], keyframes_.back());
        }
        return {KeyframeError::None, keyframes_.back().value};
    }
    if (right_key->time == time) {
        return {KeyframeError::None, right_key->value};
    }

    const Keyframe& left_key = *(right_key - 1);
    if (left_key.interpolation == KeyframeInterpolation::Hold) {
        return {KeyframeError::None, left_key.value};
    }
    long double fraction =
        InterpolationFraction(time, left_key.time, right_key->time);
    if (left_key.interpolation == KeyframeInterpolation::CubicBezier) {
        fraction =
            EvaluateCubicBezier(left_key.easing, static_cast<double>(fraction));
    }
    const long double interpolated =
        static_cast<long double>(left_key.value) +
        (static_cast<long double>(right_key->value) - left_key.value) *
            fraction;
    const double value = static_cast<double>(interpolated);
    if (!std::isfinite(value)) {
        return {KeyframeError::NumericOverflow, 0.0};
    }
    return {KeyframeError::None, value};
}

double KeyframeTrack::EvaluateCubicBezier(const KeyframeEasing& easing,
                                          double progress) {
    if (progress <= 0.0) {
        return 0.0;
    }
    if (progress >= 1.0) {
        return 1.0;
    }
    const auto bezier = [](double p1, double p2, double t) {
        const double inverse = 1.0 - t;
        return 3.0 * inverse * inverse * t * p1 + 3.0 * inverse * t * t * p2 +
               t * t * t;
    };
    // x(t) is monotonic because x1 and x2 are validated into [0, 1]. A fixed
    // number of halvings reaches double resolution and never depends on a
    // convergence threshold, so every platform performs the same steps.
    double low = 0.0;
    double high = 1.0;
    for (int step = 0; step < 64; ++step) {
        const double middle = 0.5 * (low + high);
        if (bezier(easing.x1, easing.x2, middle) < progress) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return bezier(easing.y1, easing.y2, 0.5 * (low + high));
}

std::string KeyframeTrack::CanonicalValueString(double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("keyframe value must be finite");
    }
    if (value == 0.0) {
        return "0";
    }

    char buffer[64];
    const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("cannot format canonical keyframe value");
    }
    return std::string(buffer, result.ptr);
}
