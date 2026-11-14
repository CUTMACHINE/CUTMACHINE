#pragma once

// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. The true-peak interpolator of
// Loudness.cc (MOTION-MARQUE-2026-10 MM1), moved here so that the meter,
// the offline limiter of NormalizeLoudness and the streaming limiter of
// AudioDspDynamics.h read the peaks between samples through the same
// filter: a limiter checked against a meter that interpolates otherwise
// could pass its own test and fail the meter's. Loudness.cc keeps its
// outputs bit for bit: the taps are computed by the same expressions, in
// the same order (tests/audio_dsp_tests.cc compares the two detectors).
//
// Four times oversampling by a Kaiser-windowed sinc (beta 9), 32 taps a
// phase.

#include <array>
#include <cstddef>
#include <vector>

namespace audio_dsp {

inline constexpr int kTruePeakPhases = 4;
inline constexpr int kTruePeakTaps = 32;
// Phase p of the value computed at input n stands between n - 16 and the
// sample after it: the detector answers for the sample this far behind.
inline constexpr int kTruePeakDelay =
    (kTruePeakPhases * kTruePeakTaps) / (2 * kTruePeakPhases);

// kTruePeakPhases * kTruePeakTaps coefficients; tap j of phase p is at
// j * kTruePeakPhases + p.
const std::vector<double>& TruePeakInterpolator();

// One channel's true peak, sample by sample: Push(x[n]) answers for sample
// n - kTruePeakDelay the largest magnitude among that sample and the
// oversampled values from it to the next -- what Loudness.cc's envelope
// holds for it. Silence stands before the first sample. Never allocates.
class TruePeakDetector {
public:
    double Push(float x);
    void Reset();

private:
    std::array<float, kTruePeakTaps> history_{};
    size_t position_ = 0;
};

}  // namespace audio_dsp
