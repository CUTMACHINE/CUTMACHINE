#pragma once

// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/audio-dsp/src/lib.rs, smooth.rs and biquad.rs (MIT OR Apache-2.0,
// commit 09aaf9b). The engine's own sound processing: until now every
// treatment of a sound was an FFmpeg filter in the export's graph (volume,
// afade, amix, alimiter), so playback and export could not share one, and
// nothing could be measured before a file was written. The voice chain and
// the ducking asked for on 9 October (MOTION-MARQUE-2026-10) need effects
// the engine runs itself.
//
// Nothing here is wired to the export or to playback: one mixer for both is
// the author's decision to take (MOTION-MARQUE-2026-10), and this is the library
// it will be built on.
//
// The contract every effect keeps, and tests/audio_dsp_tests.cc checks:
// - built with its sample rate and channel count, which allocates; then
//   Process, SetParameter and Reset never allocate;
// - Process works in place on planar float blocks (one pointer per channel,
//   as many as the effect was built with), of any length, zero included;
// - the output does not depend on how the stream is cut into blocks, to
//   the bit: every state, every parameter ramp and every periodic update
//   advances per sample, never per block;
// - Latency() is the delay, in samples, between a sample in and its
//   processed self out, and an impulse says the same;
// - parameters are addressed by a stable identifier, carry a range, a
//   unit and a default, and move to a new value over a linear ramp of
//   kParameterRampMilliseconds, sample by sample (a choice or a toggle
//   switches at once, or through the effect's own cross-fade);
// - recursive state under 1e-30 is set to zero, so a long silence after a
//   sound never leaves the processor in denormal arithmetic (no CPU flag
//   needed, so the same code is exact on every machine that runs it).
//
// Portable: plain C++, no framework, in cutmachine_model.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace audio_dsp {

// A parameter's unit, for display and for building a control.
enum class Unit {
    kNone,
    kDecibels,
    kHertz,
    kMilliseconds,
    kPercent,
    kQ,
    kRatio,
    // 0 or 1.
    kToggle,
    // An index into ParameterSpec::choices.
    kChoice,
};

struct ParameterSpec {
    // Stable: what a caller, a saved setting or an automation names.
    std::string id;
    double minimum = 0.0;
    double maximum = 1.0;
    double default_value = 0.0;
    Unit unit = Unit::kNone;
    // For kChoice: the names of the values, in index order.
    std::vector<std::string> choices;

    // Clamped into [minimum, maximum], rounded for a toggle or a choice;
    // NaN is the default.
    double Sanitize(double value) const;
};

// The length of every parameter ramp.
inline constexpr double kParameterRampMilliseconds = 20.0;

// A value moving linearly to its target over a fixed number of samples.
class Smoothed {
public:
    Smoothed() = default;
    Smoothed(double value, int64_t rampSamples);

    void SetTarget(double target);
    // Straight to the target.
    void Snap();
    // One sample on; the value after it.
    double Next();
    double Value() const { return current_; }
    double Target() const { return target_; }
    bool Moving() const { return remaining_ > 0; }

private:
    double current_ = 0.0;
    double target_ = 0.0;
    double step_ = 0.0;
    int64_t remaining_ = 0;
    int64_t ramp_ = 0;
};

// Samples of a ramp of kParameterRampMilliseconds at `rate`.
int64_t RampSamples(int32_t rate);

// Zero under 1e-30: recursive state never reaches denormal numbers.
inline double Flush(double value) {
    return std::abs(value) < 1e-30 ? 0.0 : value;
}

inline double DbToGain(double db) { return std::pow(10.0, db / 20.0); }

class Effect {
public:
    Effect(int32_t rate, int channels);
    virtual ~Effect() = default;
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;

    // The registry's name for the effect (CreateEffect).
    virtual const char* Id() const = 0;
    virtual const std::vector<ParameterSpec>& Parameters() const = 0;

    // Sets a parameter by identifier, clamped into its range, moving there
    // from the next sample processed. False for an unknown identifier.
    bool SetParameter(std::string_view id, double value);
    // The value last set (the target, not where a ramp is).
    std::optional<double> Parameter(std::string_view id) const;

    // `channels` holds Channels() pointers, each to `frames` samples.
    virtual void Process(float* const* channels, size_t frames) = 0;
    virtual size_t Latency() const { return 0; }
    // Every state back to silence; ramps land on their targets.
    virtual void Reset() = 0;

    int32_t Rate() const { return rate_; }
    int Channels() const { return channels_; }

protected:
    // Called once the derived effect's table exists: the defaults, applied
    // with every ramp landed.
    void InitializeParameters();
    // The parameter values changed: the derived effect sets its ramps'
    // targets (snap: straight there).
    virtual void ApplyParameters(bool snap) = 0;
    double Value(std::string_view id) const;
    // By position in Parameters(): what an effect with many parameters
    // reads, without building their names.
    double ValueAt(size_t index) const { return values_[index]; }
    bool On(std::string_view id) const { return Value(id) >= 0.5; }
    int Index(std::string_view id) const { return static_cast<int>(Value(id)); }

private:
    int32_t rate_;
    int channels_;
    std::vector<double> values_;
};

// The effects CreateEffect builds, by identifier.
const std::vector<std::string>& EffectIds();
// Null for an unknown identifier, a rate under 8000 Hz or above 384000 Hz,
// or a channel count outside 1..8.
std::unique_ptr<Effect> CreateEffect(std::string_view id, int32_t rate,
                                     int channels);

// RBJ Audio EQ Cookbook (Robert Bristow-Johnson) second-order sections.
enum class BiquadType {
    kPeaking,
    kLowShelf,
    kHighShelf,
    kHighPass,
    kLowPass,
    kNotch,
};

// Normalised (a0 = 1) coefficients.
struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    // The cookbook design: `frequency` clamped to [1 Hz, 0.49 rate], `q`
    // to at least 0.001; `gainDb` serves the peaking and shelving types
    // (a shelf's q is the cookbook's shelf q: 1/sqrt(2) is slope 1). A
    // peaking or shelving section at 0 dB is the identity, exactly.
    static BiquadCoefficients Design(BiquadType type, double rate,
                                     double frequency, double q, double gainDb);
    // |H(e^jw)| at `frequency`, in dB, from the coefficients.
    double MagnitudeDb(double frequency, double rate) const;
};

// One section's memory, transposed direct form II, in double.
struct BiquadState {
    double z1 = 0.0;
    double z2 = 0.0;

    double Process(const BiquadCoefficients& c, double x) {
        const double y = c.b0 * x + z1;
        z1 = Flush(c.b1 * x - c.a1 * y + z2);
        z2 = Flush(c.b2 * x - c.a2 * y);
        return y;
    }
    void Reset() {
        z1 = 0.0;
        z2 = 0.0;
    }
};

}  // namespace audio_dsp
