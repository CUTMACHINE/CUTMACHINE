// MOTION-MARQUE-2026-10 MM1 -- the loudness meter and the true-peak limiter
// (Loudness.h) on signals whose answer is known: EBU Tech 3341's sine
// cases and its gating case, a peak between two samples, and a dense mix
// brought to -16 LUFS under -1 dBTP. Portable.

#include "Loudness.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

constexpr int32_t kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// A sine of `dbfs` peak on both channels.
void Sine(double frequency, double dbfs, double seconds,
          std::vector<float>& left, std::vector<float>& right,
          double phase = 0.0) {
    const double amplitude = std::pow(10.0, dbfs / 20.0);
    const auto count = static_cast<size_t>(seconds * kRate);
    for (size_t n = 0; n < count; ++n) {
        const auto value = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * n / kRate + phase));
        left.push_back(value);
        right.push_back(value);
    }
}

std::string Text(double value) { return std::to_string(value); }

void TestSines() {
    std::vector<float> left;
    std::vector<float> right;
    Sine(1000.0, -23.0, 20.0, left, right);
    LoudnessMeasure measure = MeasureLoudness(left, right, kRate);
    Check(measure.integrated_lufs.has_value() &&
              std::abs(*measure.integrated_lufs + 23.0) < 0.1,
          "Tech 3341 case 1: a 1 kHz sine at -23 dBFS reads -23 LUFS: " +
              Text(measure.integrated_lufs.value_or(0.0)));
    Check(std::abs(measure.true_peak_dbtp + 23.0) < 0.1 &&
              std::abs(measure.sample_peak_dbfs + 23.0) < 0.1,
          "its true peak is its peak");
    left.clear();
    right.clear();
    Sine(1000.0, -33.0, 20.0, left, right);
    measure = MeasureLoudness(left, right, kRate);
    Check(measure.integrated_lufs.has_value() &&
              std::abs(*measure.integrated_lufs + 33.0) < 0.1,
          "Tech 3341 case 2: -33 dBFS reads -33 LUFS: " +
              Text(measure.integrated_lufs.value_or(0.0)));
}

void TestGating() {
    // Tech 3341 case 3: 10 s at -36, 60 s at -23, 10 s at -36 dBFS: the
    // quiet parts fall under the relative gate.
    std::vector<float> left;
    std::vector<float> right;
    Sine(1000.0, -36.0, 10.0, left, right);
    Sine(1000.0, -23.0, 60.0, left, right);
    Sine(1000.0, -36.0, 10.0, left, right);
    const LoudnessMeasure measure = MeasureLoudness(left, right, kRate);
    Check(measure.integrated_lufs.has_value() &&
              std::abs(*measure.integrated_lufs + 23.0) < 0.1,
          "Tech 3341 case 3: the gate leaves the quiet parts out: " +
              Text(measure.integrated_lufs.value_or(0.0)));
    std::vector<float> silence(static_cast<size_t>(kRate) * 5, 0.0f);
    Check(!MeasureLoudness(silence, silence, kRate).integrated_lufs,
          "silence has no integrated loudness");
}

void TestTruePeak() {
    // A sine at a quarter of the rate, sampled 45 degrees off its crests:
    // every sample at 0.707 of a peak that falls between them.
    std::vector<float> left;
    std::vector<float> right;
    Sine(kRate / 4.0, -6.0, 1.0, left, right, kPi / 4.0);
    const LoudnessMeasure measure = MeasureLoudness(left, right, kRate);
    Check(std::abs(measure.sample_peak_dbfs + 9.01) < 0.05 &&
              std::abs(measure.true_peak_dbtp + 6.0) < 0.2,
          "a peak between two samples is found: samples " +
              Text(measure.sample_peak_dbfs) + " dBFS, true peak " +
              Text(measure.true_peak_dbtp) + " dBTP");
}

// A dense mix: a few sines and a seeded noise, loud enough to clip.
void Dense(std::vector<float>& left, std::vector<float>& right) {
    uint64_t seed = 42;
    const auto noise = [&seed]() {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((seed >> 33) & 0xffffff) / 0x800000 - 1.0;
    };
    const size_t count = static_cast<size_t>(kRate) * 12;
    for (size_t n = 0; n < count; ++n) {
        const double t = static_cast<double>(n) / kRate;
        // A beat every half second: transients the limiter has to catch.
        const double beat = std::exp(-std::fmod(t, 0.5) * 30.0);
        const double tone = 0.4 * std::sin(2.0 * kPi * 110.0 * t) +
                            0.3 * std::sin(2.0 * kPi * 440.0 * t + 0.3);
        left.push_back(static_cast<float>(tone + 0.9 * beat * noise()));
        right.push_back(static_cast<float>(tone * 0.8 + 0.9 * beat * noise()));
    }
}

void TestNormalize() {
    std::vector<float> left;
    std::vector<float> right;
    Dense(left, right);
    LoudnessResult result;
    std::vector<float> softLeft = left;
    std::vector<float> softRight = right;
    NormalizeLoudness(softLeft, softRight, kRate, LoudnessTarget{-16.0, -1.0},
                      result);
    Check(result.before.true_peak_dbtp > 0.0 && result.gain_db < 0.0 &&
              result.after.integrated_lufs.has_value() &&
              std::abs(*result.after.integrated_lufs + 16.0) <= 0.05 &&
              result.after.true_peak_dbtp <= -1.0 + 1e-3,
          "a mix that clips is brought down to -16 LUFS: " +
              Text(result.before.true_peak_dbtp) + " dBTP before");
    // Pushed to -9 LUFS, its beats would pass the ceiling: the limiter
    // holds them, and the gain is set again for what it took.
    NormalizeLoudness(left, right, kRate, LoudnessTarget{-9.0, -1.0}, result);
    Check(result.after.integrated_lufs.has_value() &&
              std::abs(*result.after.integrated_lufs + 9.0) <= 0.1 &&
              result.after.true_peak_dbtp <= -1.0 + 1e-3 &&
              result.limited_db < -1.0,
          "a dense mix reaches -9 LUFS under -1 dBTP, the limiter at work: " +
              Text(result.after.integrated_lufs.value_or(0.0)) + " LUFS, " +
              Text(result.after.true_peak_dbtp) + " dBTP, limited " +
              Text(result.limited_db) + " dB");
    // A quiet sine needs gain only: nothing to limit.
    std::vector<float> quietLeft;
    std::vector<float> quietRight;
    Sine(1000.0, -40.0, 10.0, quietLeft, quietRight);
    NormalizeLoudness(quietLeft, quietRight, kRate, LoudnessTarget{-16.0, -1.0},
                      result);
    Check(result.after.integrated_lufs.has_value() &&
              std::abs(*result.after.integrated_lufs + 16.0) < 0.05 &&
              result.limited_db > -0.01,
          "a quiet sine is raised, not limited: " +
              Text(result.after.integrated_lufs.value_or(0.0)));
    std::vector<float> silence(static_cast<size_t>(kRate) * 2, 0.0f);
    std::vector<float> silence2 = silence;
    NormalizeLoudness(silence, silence2, kRate, LoudnessTarget{}, result);
    Check(result.silent && silence[100] == 0.0f, "silence is left as it is");
}

}  // namespace

int main() {
    TestSines();
    TestGating();
    TestTruePeak();
    TestNormalize();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "loudness: ok\n";
    return 0;
}
