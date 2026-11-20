#pragma once

#include "RationalTime.h"
#include "Ulid.h"

#include <cstddef>
#include <string>
#include <vector>

enum class KeyframeInterpolation {
    Hold,
    Linear,
    // MOTION-2026-09 M1 -- ROADMAP.md. CSS `cubic-bezier(x1, y1, x2, y2)`
    // easing, so a motion curve means what a model already knows it means.
    CubicBezier,
};

// Control points of a CSS cubic-bezier easing; read only for CubicBezier.
// x1 and x2 must lie in [0, 1], which keeps the curve a function of time.
struct KeyframeEasing {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 1.0;
    double y2 = 1.0;
};

struct Keyframe {
    Ulid id;
    RationalTime time;
    double value = 0.0;

    // Interpolation applies from this keyframe to the following one.
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
    KeyframeEasing easing;
};

// Behaviour outside the keyed range. Extend continues the straight line
// through the two outermost keyframes, ignoring their easing; a Hold outer
// segment has no slope and therefore clamps.
enum class KeyframeExtrapolation {
    Clamp,
    Extend,
};

enum class KeyframeError {
    None,
    InvalidId,
    InvalidTime,
    InvalidInterpolation,
    NonFiniteValue,
    DuplicateId,
    DuplicateTime,
    NotFound,
    EmptyTrack,
    NumericOverflow,
};

const char* KeyframeErrorName(KeyframeError error);

struct KeyframeResult {
    KeyframeError error = KeyframeError::None;

    explicit operator bool() const { return error == KeyframeError::None; }
};

struct KeyframeEvaluation {
    KeyframeError error = KeyframeError::None;
    double value = 0.0;

    explicit operator bool() const { return error == KeyframeError::None; }
};

// A deterministic, single-numeric-parameter keyframe track.
//
// Time ordering and comparisons are exact. Linear interpolation converts only
// the resulting dimensionless rational fraction to floating point; it never
// converts a RationalTime position to seconds. Values are finite doubles by
// design, because image/audio parameter evaluation is ultimately floating
// point. CanonicalValueString provides a locale-independent, round-trippable
// spelling for future document serialization.
class KeyframeTrack {
public:
    const std::vector<Keyframe>& keyframes() const { return keyframes_; }
    std::size_t size() const { return keyframes_.size(); }
    bool empty() const { return keyframes_.empty(); }

    // IDs are supplied by the caller so a serialized operation can replay the
    // same identity. Two keyframes may not occupy the same exact rational time.
    KeyframeResult Add(Keyframe keyframe);
    KeyframeResult Remove(const Ulid& id);

    // Identity is immutable; an update replaces all mutable fields atomically.
    KeyframeResult Update(const Ulid& id, RationalTime time, double value,
                          KeyframeInterpolation interpolation,
                          KeyframeEasing easing = {});

    // Before/after the keyed range, evaluation clamps to the endpoint value
    // unless the caller asks for the range to be extended on that side.
    KeyframeEvaluation Evaluate(
        RationalTime time,
        KeyframeExtrapolation left = KeyframeExtrapolation::Clamp,
        KeyframeExtrapolation right = KeyframeExtrapolation::Clamp) const;

    static KeyframeResult Validate(const Keyframe& keyframe);
    static std::string CanonicalValueString(double value);

    // Eased progress for a linear progress in [0, 1]. Bisection over the
    // monotonic x(t) makes the result independent of any tolerance choice.
    static double EvaluateCubicBezier(const KeyframeEasing& easing,
                                      double progress);

private:
    std::vector<Keyframe> keyframes_;
};
