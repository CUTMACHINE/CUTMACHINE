#pragma once

// MOTION-2026-09 M1 -- ROADMAP.md. Remotion's `spring()` vocabulary (mass,
// damping, stiffness, overshootClamping, same defaults), evaluated as the
// closed-form damped oscillator rather than by stepping frames. A stepped
// simulation drifts with the render rate; the closed form gives the same
// value at the same instant whether the composition renders at 25 or 50 fps.

#include "RationalTime.h"

#include <string>

struct MotionSpring {
    double from = 0.0;
    double to = 1.0;
    double mass = 1.0;
    double damping = 10.0;
    double stiffness = 100.0;
    bool overshoot_clamping = false;
};

// Rejects a physically meaningless configuration with a readable reason.
bool ValidateMotionSpring(const MotionSpring& spring, std::string& error);

// `elapsed` is the exact time since the spring started. It becomes seconds
// only here, as the argument of a physical law; a negative elapsed time holds
// the start value. Starting velocity is zero.
double EvaluateMotionSpring(const MotionSpring& spring, RationalTime elapsed);
