#include "MotionSpring.h"

#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Near critical damping both closed forms lose precision to a vanishing
// divisor; a fixed band routes them to the exact critical solution.
constexpr double kCriticalBand = 1e-9;

}  // namespace

bool ValidateMotionSpring(const MotionSpring& spring, std::string& error) {
    if (!std::isfinite(spring.from) || !std::isfinite(spring.to)) {
        error = "spring from/to must be finite numbers";
        return false;
    }
    if (!std::isfinite(spring.mass) || spring.mass <= 0.0) {
        error = "spring mass must be a positive number";
        return false;
    }
    if (!std::isfinite(spring.stiffness) || spring.stiffness <= 0.0) {
        error = "spring stiffness must be a positive number";
        return false;
    }
    if (!std::isfinite(spring.damping) || spring.damping < 0.0) {
        error = "spring damping must be zero or positive";
        return false;
    }
    return true;
}

double EvaluateMotionSpring(const MotionSpring& spring, RationalTime elapsed) {
    if (elapsed.rate <= 0 || elapsed.value <= 0) {
        return spring.from;
    }
    const double t = static_cast<double>(
        static_cast<long double>(elapsed.value) / elapsed.rate);
    const double initial = spring.from - spring.to;
    if (initial == 0.0) {
        return spring.to;
    }

    const double omega = std::sqrt(spring.stiffness / spring.mass);
    const double zeta =
        spring.damping / (2.0 * std::sqrt(spring.stiffness * spring.mass));

    double displacement = 0.0;
    if (zeta < 1.0 - kCriticalBand) {
        const double damped = omega * std::sqrt(1.0 - zeta * zeta);
        // With overshoot clamping the value stops at the target the first
        // time it reaches it; that instant is where the underdamped
        // displacement first crosses zero.
        if (spring.overshoot_clamping) {
            const double first_crossing =
                (kPi - std::atan2(damped, zeta * omega)) / damped;
            if (t >= first_crossing) {
                return spring.to;
            }
        }
        displacement =
            std::exp(-zeta * omega * t) *
            (initial * std::cos(damped * t) +
             (zeta * omega * initial / damped) * std::sin(damped * t));
    } else if (zeta <= 1.0 + kCriticalBand) {
        displacement = std::exp(-omega * t) * (initial + omega * initial * t);
    } else {
        const double root = std::sqrt(zeta * zeta - 1.0);
        const double fast = -omega * (zeta + root);
        const double slow = -omega * (zeta - root);
        const double slow_weight = initial * fast / (fast - slow);
        const double fast_weight = initial - slow_weight;
        displacement =
            fast_weight * std::exp(fast * t) + slow_weight * std::exp(slow * t);
    }
    return spring.to + displacement;
}
