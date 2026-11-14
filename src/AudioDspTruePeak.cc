#include "AudioDspTruePeak.h"

#include <algorithm>
#include <cmath>

namespace audio_dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

const std::vector<double>& TruePeakInterpolator() {
    static const std::vector<double> taps = [] {
        const int length = kTruePeakPhases * kTruePeakTaps;
        const double beta = 9.0;
        const auto bessel = [](double x) {
            double sum = 1.0;
            double term = 1.0;
            for (int k = 1; k < 50; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        std::vector<double> out(static_cast<size_t>(length));
        const double centre = (length - 1) / 2.0;
        for (int k = 0; k < length; ++k) {
            const double t = (k - centre) / kTruePeakPhases;
            const double sinc = t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
            const double r = 2.0 * k / (length - 1) - 1.0;
            const double window =
                bessel(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) /
                bessel(beta);
            out[static_cast<size_t>(k)] = sinc * window;
        }
        return out;
    }();
    return taps;
}

double TruePeakDetector::Push(float x) {
    const std::vector<double>& h = TruePeakInterpolator();
    history_[position_] = x;
    // history_[(position_ - j) mod taps] is x[n - j].
    const auto at = [this](int j) {
        return history_[(position_ + kTruePeakTaps - static_cast<size_t>(j)) %
                        kTruePeakTaps];
    };
    double most = std::abs(static_cast<double>(at(kTruePeakDelay)));
    for (int p = 0; p < kTruePeakPhases; ++p) {
        double acc = 0.0;
        for (int j = 0; j < kTruePeakTaps; ++j) {
            acc += h[static_cast<size_t>(j) * kTruePeakPhases +
                     static_cast<size_t>(p)] *
                   at(j);
        }
        most = std::max(most, std::abs(acc));
    }
    position_ = (position_ + 1) % kTruePeakTaps;
    return most;
}

void TruePeakDetector::Reset() {
    history_.fill(0.0f);
    position_ = 0;
}

}  // namespace audio_dsp
