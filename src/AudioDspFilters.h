#pragma once

// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/audio-dsp/src/effects/eq.rs and effects/restoration.rs (the de-hum)
// (MIT OR Apache-2.0, commit 09aaf9b). The linear effects: an EQ of six
// parametric bands, a high-pass, and a comb of notches on the mains hum and
// its harmonics. All of them are cascades of the RBJ sections of AudioDsp.h.
//
// A section's frequency moves on a log scale, its gain and its Q linearly,
// every one over the parameter ramp; while one moves, the coefficients are
// designed again every kCoefficientInterval samples, counted per sample so
// that the cut into blocks never shows. Enabling or disabling a band, the
// high-pass or a notch cross-fades it in or out over the same ramp.

#include "AudioDsp.h"

#include <array>
#include <vector>

namespace audio_dsp {

inline constexpr int kCoefficientInterval = 16;

// One section whose frequency, gain and Q glide.
class GlidingSection {
public:
    GlidingSection() = default;
    GlidingSection(int32_t rate, int channels);

    void SetTargets(bool enabled, BiquadType type, double frequency,
                    double gainDb, double q, bool snap);
    // One sample on, for every channel at once: the parameters, then, when
    // due, the coefficients.
    void Advance();
    // `x` of channel `channel` through the section, with its cross-fade.
    double Process(int channel, double x);
    void Reset();
    // The section as its targets set it, in dB at `frequency`; 0 when it
    // is to be disabled.
    double TargetResponseDb(double frequency) const;

private:
    void Design();

    double rate_ = 48000.0;
    BiquadType type_ = BiquadType::kPeaking;
    Smoothed log_frequency_;
    Smoothed gain_;
    Smoothed q_;
    Smoothed mix_;
    BiquadCoefficients coefficients_;
    int counter_ = 0;
    bool dirty_ = false;
    std::vector<BiquadState> states_;
};

// Six bands; band n's parameters are "band<n>.enabled", ".type" (peaking,
// low_shelf, high_shelf, high_pass, low_pass), ".frequency", ".gain" and
// ".q", n from 1. Then "output_gain". Every band at 0 dB is the identity,
// to the bit.
class ParametricEq final : public Effect {
public:
    static constexpr int kBands = 6;

    ParametricEq(int32_t rate, int channels);
    const char* Id() const override { return "parametric_eq"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    void Reset() override;
    // The response of the bands as set (targets, not ramps), in dB.
    double ResponseDb(double frequency) const;

protected:
    void ApplyParameters(bool snap) override;

private:
    std::array<GlidingSection, kBands> bands_;
    Smoothed output_gain_;
};

// A Butterworth high-pass of 12 or 24 dB per octave ("slope": 12db, 24db),
// "frequency" from 10 to 500 Hz. "enabled" off is its neutral setting: a
// high-pass at any frequency changes a sound.
class HighPass final : public Effect {
public:
    HighPass(int32_t rate, int channels);
    const char* Id() const override { return "high_pass"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    void Reset() override;
    double ResponseDb(double frequency) const;

protected:
    void ApplyParameters(bool snap) override;

private:
    // The two sections of a fourth order; the second is faded out at 12.
    std::array<GlidingSection, 2> sections_;
};

// Notches on the mains frequency ("frequency": 50hz, 60hz) and its first
// "harmonics" multiples (1 to 10), of quality "q"; "amount" from 0 to 100 %
// mixes the result with the dry sound (0 is the identity). A harmonic at or
// above 0.45 of the rate is left out.
class DeHum final : public Effect {
public:
    static constexpr int kMaxHarmonics = 10;

    DeHum(int32_t rate, int channels);
    const char* Id() const override { return "de_hum"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    void Reset() override;

protected:
    void ApplyParameters(bool snap) override;

private:
    std::array<GlidingSection, kMaxHarmonics> notches_;
    Smoothed amount_;
};

}  // namespace audio_dsp
