// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. The sound-processing library
// (AudioDsp.h) against its contract and against signals whose answer is
// known: the same output to the bit however the stream is cut into blocks,
// no allocation while processing, the identity at each effect's neutral
// setting, the latency an impulse measures, the biquads' response against
// the cookbook's formula, the compressor's static curve, no true peak over
// the limiter's ceiling as the meter (Loudness.h) reads it, a better
// signal-to-noise ratio out of the denoiser, the hum gone and the program
// kept by the de-hum. FFmpeg's own biquads are compared in
// audio_dsp_ffmpeg_tests.cc. Portable.

#include "AudioDsp.h"
#include "AudioDspDenoise.h"
#include "AudioDspDynamics.h"
#include "AudioDspFilters.h"
#include "AudioDspTruePeak.h"
#include "Loudness.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "TestSupport.h"

// Every allocation of the process is counted while `g_counting` is set: the
// contract is that Process, SetParameter and Reset make none.
namespace {
bool g_counting = false;
int64_t g_allocations = 0;
}  // namespace

void* operator new(size_t size) {
    if (g_counting) {
        ++g_allocations;
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

namespace {

using audio_dsp::CreateEffect;
using audio_dsp::Effect;
using audio_dsp::EffectIds;

std::string Text(double value) { return std::to_string(value); }

// The measures a reader of the run wants to see, passed or not.
void Report(const std::string& label, double value) {
    std::cout << label << ": " << value << '\n';
}

std::string Precise(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

constexpr int32_t kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// xorshift64*, uniform in [-1, 1).
class Random {
public:
    explicit Random(uint64_t seed) : state_(seed * 0x9E3779B97F4A7C15ull | 1) {}
    uint64_t Next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }
    float Uniform() {
        return static_cast<float>((Next() >> 40) /
                                  static_cast<double>(1ull << 24)) *
                   2.0f -
               1.0f;
    }

private:
    uint64_t state_;
};

std::vector<float> Noise(uint64_t seed, size_t count, float amplitude) {
    Random random(seed);
    std::vector<float> out(count);
    for (float& value : out) {
        value = random.Uniform() * amplitude;
    }
    return out;
}

std::vector<float> Sine(double frequency, double amplitude, size_t count,
                        double phase = 0.0, int32_t rate = kRate) {
    std::vector<float> out(count);
    for (size_t n = 0; n < count; ++n) {
        out[n] = static_cast<float>(
            amplitude *
            std::sin(2.0 * kPi * frequency * static_cast<double>(n) / rate +
                     phase));
    }
    return out;
}

// The amplitude of the `frequency` component of x[begin, end).
double ToneAmplitude(const std::vector<float>& x, size_t begin, size_t end,
                     double frequency, int32_t rate = kRate) {
    double re = 0.0;
    double im = 0.0;
    for (size_t n = begin; n < end; ++n) {
        const double w = 2.0 * kPi * frequency * static_cast<double>(n) / rate;
        re += x[n] * std::cos(w);
        im += x[n] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) /
           static_cast<double>(end - begin);
}

double Db(double ratio) { return 20.0 * std::log10(ratio); }

bool SameBits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

using Stereo = std::vector<std::vector<float>>;

void Run(Effect& effect, Stereo& signal, size_t begin, size_t length) {
    float* pointers[8];
    for (size_t channel = 0; channel < signal.size(); ++channel) {
        pointers[channel] = signal[channel].data() + begin;
    }
    effect.Process(pointers, length);
}

void RunWhole(Effect& effect, Stereo& signal) {
    Run(effect, signal, 0, signal[0].size());
}

// Settings under which each effect does something, and the change made
// mid-stream in the block test.
void Engage(Effect& effect) {
    const std::string id = effect.Id();
    if (id == "parametric_eq") {
        effect.SetParameter("band2.gain", 6.0);
        effect.SetParameter("band4.gain", -9.0);
        effect.SetParameter("band6.type", 2.0);
        effect.SetParameter("band6.gain", 4.0);
    } else if (id == "limiter") {
        effect.SetParameter("input_gain", 12.0);
    } else if (id == "de_esser") {
        effect.SetParameter("threshold", -40.0);
    }
}

void Tweak(Effect& effect) {
    const std::string id = effect.Id();
    if (id == "parametric_eq") {
        effect.SetParameter("band2.frequency", 400.0);
        effect.SetParameter("band3.enabled", 0.0);
        effect.SetParameter("band5.q", 4.0);
        effect.SetParameter("output_gain", -3.0);
    } else if (id == "high_pass") {
        effect.SetParameter("frequency", 200.0);
        effect.SetParameter("slope", 1.0);
    } else if (id == "compressor") {
        effect.SetParameter("threshold", -30.0);
        effect.SetParameter("ratio", 8.0);
        effect.SetParameter("makeup", 4.0);
    } else if (id == "de_esser") {
        effect.SetParameter("frequency", 4000.0);
        effect.SetParameter("reduction", 12.0);
    } else if (id == "limiter") {
        effect.SetParameter("ceiling", -6.0);
        effect.SetParameter("release", 20.0);
    } else if (id == "denoise") {
        effect.SetParameter("reduction", 24.0);
        effect.SetParameter("threshold", 3.0);
    } else if (id == "de_hum") {
        effect.SetParameter("frequency", 1.0);
        effect.SetParameter("harmonics", 7.0);
        effect.SetParameter("amount", 60.0);
    }
}

// The setting at which each effect gives the sound back.
void Neutral(Effect& effect) {
    const std::string id = effect.Id();
    if (id == "high_pass") {
        effect.SetParameter("enabled", 0.0);
    } else if (id == "compressor") {
        effect.SetParameter("ratio", 1.0);
    } else if (id == "de_esser") {
        effect.SetParameter("reduction", 0.0);
    } else if (id == "limiter") {
        effect.SetParameter("ceiling", 0.0);
    } else if (id == "denoise") {
        effect.SetParameter("reduction", 0.0);
    } else if (id == "de_hum") {
        effect.SetParameter("amount", 0.0);
    }
    // The parametric EQ's defaults are its neutral: every band at 0 dB.
}

void TestRegistry() {
    Check(EffectIds().size() == 7, "seven effects in the registry");
    for (const std::string& id : EffectIds()) {
        std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 2);
        Check(effect != nullptr && effect->Id() == id,
              id + " is built by its identifier");
        if (!effect) {
            continue;
        }
        for (const audio_dsp::ParameterSpec& spec : effect->Parameters()) {
            Check(effect->Parameter(spec.id) == spec.default_value,
                  id + "." + spec.id + " starts at its default");
            Check(spec.minimum <= spec.default_value &&
                      spec.default_value <= spec.maximum,
                  id + "." + spec.id + ": the default is in the range");
            Check(spec.unit != audio_dsp::Unit::kChoice ||
                      spec.choices.size() ==
                          static_cast<size_t>(spec.maximum) + 1,
                  id + "." + spec.id + ": a name for every choice");
        }
        Check(!effect->SetParameter("nope", 1.0) && !effect->Parameter("nope"),
              id + " refuses an unknown parameter");
    }
    std::unique_ptr<Effect> compressor = CreateEffect("compressor", kRate, 2);
    compressor->SetParameter("ratio", 1000.0);
    Check(compressor->Parameter("ratio") == 30.0, "a value is clamped");
    compressor->SetParameter("ratio", std::nan(""));
    Check(compressor->Parameter("ratio") == 4.0, "NaN is the default");
    Check(CreateEffect("nope", kRate, 2) == nullptr &&
              CreateEffect("compressor", 4000, 2) == nullptr &&
              CreateEffect("compressor", kRate, 0) == nullptr,
          "no effect for an unknown name, a rate or channels out of range");
}

// CHAQUE effet: the same output, to the bit, with blocks of 1, 64, 1000,
// sizes drawn at random (zero included) and the whole stream at once, a
// parameter changed at the same sample in each run.
void TestBlockIndependence() {
    const size_t length = 30000;
    const size_t changeAt = 11111;
    const Stereo source = {Noise(1, length, 0.7f), Noise(2, length, 0.7f)};
    for (const std::string& id : EffectIds()) {
        const auto run = [&](const std::function<size_t()>& sizes) {
            std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 2);
            Engage(*effect);
            Stereo signal = source;
            size_t position = 0;
            bool changed = false;
            while (position < length) {
                if (!changed && position == changeAt) {
                    Tweak(*effect);
                    changed = true;
                }
                size_t block = std::min(sizes(), length - position);
                if (!changed && position + block > changeAt) {
                    block = changeAt - position;
                }
                Run(*effect, signal, position, block);
                position += block;
            }
            return signal;
        };
        const Stereo whole = run([] { return SIZE_MAX; });
        bool finite = true;
        for (const std::vector<float>& channel : whole) {
            for (const float value : channel) {
                finite = finite && std::isfinite(value);
            }
        }
        Check(finite, id + ": a finite output");
        Check(!SameBits(whole[0], source[0]), id + " changes the sound");
        for (const size_t fixed : {size_t{1}, size_t{64}, size_t{1000}}) {
            const Stereo cut = run([fixed] { return fixed; });
            Check(SameBits(cut[0], whole[0]) && SameBits(cut[1], whole[1]),
                  id + ": blocks of " + std::to_string(fixed) +
                      " give the whole stream's output, to the bit");
        }
        Random random(5);
        const Stereo varied = run([&random] {
            switch (random.Next() % 4) {
                case 0:
                    return size_t{0};
                case 1:
                    return size_t{1};
                case 2:
                    return static_cast<size_t>(random.Next() % 64);
                default:
                    return static_cast<size_t>(random.Next() % 3000);
            }
        });
        Check(SameBits(varied[0], whole[0]) && SameBits(varied[1], whole[1]),
              id + ": blocks of varying size, empty ones included, give the "
                   "same output, to the bit");
    }
}

void TestNoAllocation() {
    for (const std::string& id : EffectIds()) {
        std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 2);
        Stereo signal = {Noise(3, 20000, 0.8f), Noise(4, 20000, 0.8f)};
        g_allocations = 0;
        g_counting = true;
        Engage(*effect);
        Run(*effect, signal, 0, 7000);
        Tweak(*effect);
        Run(*effect, signal, 7000, 13000);
        effect->Reset();
        Run(*effect, signal, 0, 5000);
        g_counting = false;
        Check(g_allocations == 0,
              id + ": Process, SetParameter and Reset allocate nothing (" +
                  std::to_string(g_allocations) + " allocations)");
    }
}

// At its neutral setting each effect returns its input delayed by
// Latency(): to the bit, except the denoiser, whose transform and its
// inverse give the samples back within 1e-6.
void TestNeutralIsIdentity() {
    const size_t length = 24000;
    // Under the limiter's 0 dBTP, true peaks included.
    const Stereo source = {Noise(6, length, 0.5f), Noise(7, length, 0.4f)};
    for (const std::string& id : EffectIds()) {
        std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 2);
        Neutral(*effect);
        effect->Reset();
        Stereo signal = source;
        RunWhole(*effect, signal);
        const size_t latency = effect->Latency();
        double worst = 0.0;
        bool exact = true;
        for (size_t channel = 0; channel < 2; ++channel) {
            for (size_t n = 0; n < length; ++n) {
                const float expected =
                    n < latency ? 0.0f : source[channel][n - latency];
                const float got = signal[channel][n];
                exact =
                    exact && std::memcmp(&expected, &got, sizeof(float)) == 0;
                worst = std::max(worst,
                                 std::abs(static_cast<double>(got) - expected));
            }
        }
        if (id == "denoise") {
            Check(worst < 1e-6,
                  "denoise at 0 dB gives the sound back within 1e-6, a "
                  "frame late: " +
                      std::to_string(worst));
        } else {
            Check(exact, id + " at its neutral setting is the identity" +
                             (latency > 0 ? " delayed by its latency" : "") +
                             ", to the bit (worst " + std::to_string(worst) +
                             ")");
        }
    }
}

// An impulse comes out where Latency() says: at its neutral setting, its
// largest output sample; engaged, a causal effect's first non-zero one.
void TestLatency() {
    const size_t length = 20000;
    const size_t at = 1000;
    for (const std::string& id : EffectIds()) {
        for (const bool engaged : {false, true}) {
            std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 1);
            if (engaged) {
                Engage(*effect);
            } else {
                Neutral(*effect);
            }
            effect->Reset();
            Stereo signal = {std::vector<float>(length, 0.0f)};
            signal[0][at] = 0.5f;
            RunWhole(*effect, signal);
            size_t largest = 0;
            size_t first = length;
            for (size_t n = 0; n < length; ++n) {
                if (std::abs(signal[0][n]) > std::abs(signal[0][largest])) {
                    largest = n;
                }
                if (first == length && signal[0][n] != 0.0f) {
                    first = n;
                }
            }
            const size_t expected = at + effect->Latency();
            if (!engaged) {
                Check(largest == expected,
                      id + ": the impulse comes out " +
                          std::to_string(largest - at) +
                          " samples later, its declared latency " +
                          std::to_string(effect->Latency()));
            } else if (id != "denoise") {
                // The denoiser's gains spread an impulse over its frame:
                // its delay is measured at the neutral setting only.
                Check(first == expected,
                      id + " engaged: nothing comes out before its latency (" +
                          std::to_string(first) + " for " +
                          std::to_string(expected) + ")");
            }
        }
    }
    std::unique_ptr<Effect> limiter = CreateEffect("limiter", kRate, 2);
    Check(limiter->Latency() == 240 + 2 * audio_dsp::kTruePeakDelay - 1,
          "the limiter's delay is 5 ms and the detector's two half-lengths");
    std::unique_ptr<Effect> denoise = CreateEffect("denoise", kRate, 2);
    Check(denoise->Latency() == 2048, "the denoiser is one frame late");
}

// The response measured on a sine through the section against the
// cookbook's transfer function, and the points the cookbook fixes.
void TestBiquadResponse() {
    using audio_dsp::BiquadCoefficients;
    using audio_dsp::BiquadState;
    using audio_dsp::BiquadType;
    // Four seconds, measured over the last two: a whole number of periods
    // of every frequency used, long after the transient (a notch of Q 30
    // at 50 Hz rings for a fifth of a second).
    const auto measured = [](const BiquadCoefficients& c, double frequency) {
        const size_t length = static_cast<size_t>(kRate) * 4;
        std::vector<float> x = Sine(frequency, 0.5, length);
        BiquadState state;
        for (float& value : x) {
            value = static_cast<float>(state.Process(c, value));
        }
        return Db(ToneAmplitude(x, length / 2, length, frequency) / 0.5);
    };
    const double butterworth = 1.0 / std::sqrt(2.0);
    struct Case {
        BiquadType type;
        double frequency;
        double q;
        double gain;
        std::string name;
    };
    const std::vector<Case> cases = {
        {BiquadType::kPeaking, 1000.0, 1.0, 6.0, "peaking +6 dB at 1 kHz"},
        {BiquadType::kPeaking, 300.0, 4.0, -12.0, "peaking -12 dB at 300 Hz"},
        {BiquadType::kLowShelf, 200.0, butterworth, 9.0, "low shelf +9 dB"},
        {BiquadType::kHighShelf, 5000.0, butterworth, -9.0, "high shelf -9 dB"},
        {BiquadType::kHighPass, 100.0, butterworth, 0.0, "high-pass 100 Hz"},
        {BiquadType::kLowPass, 2000.0, butterworth, 0.0, "low-pass 2 kHz"},
        {BiquadType::kNotch, 50.0, 30.0, 0.0, "notch 50 Hz"},
    };
    for (const Case& c : cases) {
        const BiquadCoefficients coefficients =
            BiquadCoefficients::Design(c.type, kRate, c.frequency, c.q, c.gain);
        double worst = 0.0;
        for (const double f :
             {40.0, 100.0, 250.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0}) {
            if (c.type == BiquadType::kNotch && f == 100.0) {
                continue;
            }
            worst =
                std::max(worst, std::abs(measured(coefficients, f) -
                                         coefficients.MagnitudeDb(f, kRate)));
        }
        Report(c.name + ", measured against the formula (dB)", worst);
        Check(worst < 0.001,
              c.name +
                  ": the measured response is the formula's within "
                  "0.001 dB: " +
                  Text(worst));
    }
    const auto at = [](BiquadType type, double f0, double q, double gain,
                       double f) {
        return BiquadCoefficients::Design(type, kRate, f0, q, gain)
            .MagnitudeDb(f, kRate);
    };
    Check(std::abs(at(BiquadType::kPeaking, 1000.0, 1.0, 6.0, 1000.0) - 6.0) <
              1e-9,
          "a peaking section has its gain at its centre");
    Check(std::abs(at(BiquadType::kHighPass, 2000.0, butterworth, 0.0, 2000.0) +
                   3.0103) < 0.001 &&
              std::abs(
                  at(BiquadType::kLowPass, 2000.0, butterworth, 0.0, 2000.0) +
                  3.0103) < 0.001,
          "Butterworth high-pass and low-pass are 3 dB down at the cutoff");
    Check(
        std::abs(at(BiquadType::kLowShelf, 200.0, butterworth, 9.0, 200.0) -
                 4.5) < 0.01 &&
            std::abs(at(BiquadType::kLowShelf, 200.0, butterworth, 9.0, 10.0) -
                     9.0) < 0.05,
        "a shelf has half its gain at its frequency, all of it beyond");
    Check(at(BiquadType::kLowPass, 2000.0, butterworth, 0.0, 8000.0) < -22.0,
          "12 dB an octave past the cutoff");
    Check(measured(BiquadCoefficients::Design(BiquadType::kNotch, kRate, 50.0,
                                              30.0, 0.0),
                   50.0) < -40.0,
          "a notch takes its frequency 40 dB down");

    // The EQ and the high-pass report the response of their sections.
    std::unique_ptr<Effect> eq = CreateEffect("parametric_eq", kRate, 1);
    eq->SetParameter("band3.gain", 6.0);
    eq->SetParameter("output_gain", -2.0);
    const double eqAt =
        static_cast<audio_dsp::ParametricEq&>(*eq).ResponseDb(1000.0);
    Check(
        std::abs(eqAt - 4.0) < 1e-9,
        "the EQ's response is its bands' and its output gain's: " + Text(eqAt));
    std::unique_ptr<Effect> highPass = CreateEffect("high_pass", kRate, 1);
    highPass->SetParameter("slope", 1.0);
    const double steep =
        static_cast<audio_dsp::HighPass&>(*highPass).ResponseDb(40.0);
    Check(std::abs(steep - 10.0 * std::log10(1.0 / (1.0 + std::pow(2.0, 8)))) <
              0.05,
          "the 24 dB high-pass is a fourth-order Butterworth, an octave "
          "under: " +
              Text(steep));
}

void TestCompressor() {
    using audio_dsp::CompressorReductionDb;
    Check(CompressorReductionDb(-30.0, -20.0, 4.0, 0.0) == 0.0 &&
              CompressorReductionDb(0.0, -20.0, 4.0, 0.0) == -15.0,
          "hard knee: nothing under, 4:1 over");
    Check(std::abs(CompressorReductionDb(-25.0, -20.0, 4.0, 10.0)) < 1e-12 &&
              std::abs(CompressorReductionDb(-15.0, -20.0, 4.0, 10.0) -
                       CompressorReductionDb(-15.0, -20.0, 4.0, 0.0)) < 1e-12,
          "the soft knee meets both lines at its edges");
    Check(CompressorReductionDb(-10.0, -20.0, 1.0, 6.0) == 0.0,
          "ratio 1 reduces nothing");
    // Constant levels: the output settles on the curve, makeup added.
    for (const double level : {-40.0, -20.0, -10.0, -2.0}) {
        std::unique_ptr<Effect> effect = CreateEffect("compressor", kRate, 2);
        effect->SetParameter("threshold", -20.0);
        effect->SetParameter("ratio", 4.0);
        effect->SetParameter("knee", 0.0);
        effect->SetParameter("makeup", 3.0);
        effect->Reset();
        const auto value = static_cast<float>(audio_dsp::DbToGain(level));
        Stereo signal = {std::vector<float>(kRate, value),
                         std::vector<float>(kRate, -value)};
        RunWhole(*effect, signal);
        const double out = Db(std::abs(signal[0][kRate - 1]));
        const double expected =
            level + CompressorReductionDb(level, -20.0, 4.0, 0.0) + 3.0;
        Check(std::abs(out - expected) < 0.01,
              "compressor at " + Text(level) + " dBFS settles at " +
                  Text(expected) + ": " + Text(out));
        Check(signal[0][kRate - 1] == -signal[1][kRate - 1],
              "one gain for both channels");
    }
    // Attack: one time constant takes 63 % of the reduction, in dB.
    std::unique_ptr<Effect> effect = CreateEffect("compressor", kRate, 1);
    effect->SetParameter("threshold", -20.0);
    effect->SetParameter("ratio", 20.0);
    effect->SetParameter("knee", 0.0);
    effect->SetParameter("attack", 10.0);
    effect->Reset();
    Stereo signal = {std::vector<float>(4800, 1.0f)};
    RunWhole(*effect, signal);
    const double full = CompressorReductionDb(0.0, -20.0, 20.0, 0.0);
    const double atTau = Db(signal[0][479]);
    Check(std::abs(atTau / full - (1.0 - std::exp(-1.0))) < 0.01,
          "one attack time constant takes 63 % of the reduction: " +
              Text(atTau / full));
}

// The limiter's own detector is the meter's: the same peak, to the bit.
// Then a sound driven 12 dB into it, in blocks of every size, never has a
// true peak over the ceiling as Loudness.h measures it.
void TestLimiter() {
    {
        std::vector<float> x = Noise(11, 30000, 0.9f);
        x[5000] = 1.7f;
        audio_dsp::TruePeakDetector detector;
        double most = 0.0;
        for (const float value : x) {
            most = std::max(most, detector.Push(value));
        }
        for (int n = 0; n < audio_dsp::kTruePeakDelay; ++n) {
            most = std::max(most, detector.Push(0.0f));
        }
        const double meter = TruePeakDb(x, x);
        Check(20.0 * std::log10(most) == meter,
              "the streaming detector reads the meter's true peak, to the "
              "bit: " +
                  Text(20.0 * std::log10(most)) + " and " + Text(meter));
    }
    for (const double ceilingDb : {-1.0, -6.0}) {
        std::unique_ptr<Effect> limiter = CreateEffect("limiter", kRate, 2);
        limiter->SetParameter("ceiling", ceilingDb);
        limiter->SetParameter("input_gain", 12.0);
        limiter->Reset();
        Random random(7);
        std::vector<float> left;
        std::vector<float> right;
        for (int block = 0; block < 300; ++block) {
            const size_t length = 1 + random.Next() % 700;
            Stereo signal = {std::vector<float>(length),
                             std::vector<float>(length)};
            for (size_t n = 0; n < length; ++n) {
                // Noise, a tone, and impulses between samples' worth.
                const double t = static_cast<double>(left.size() + n);
                signal[0][n] =
                    random.Uniform() * 0.5f +
                    static_cast<float>(
                        0.4 * std::sin(2.0 * kPi * 11025.0 * t / kRate + 0.7));
                signal[1][n] = random.Uniform() * 0.3f;
            }
            if (block % 7 == 0) {
                signal[0][length / 2] = 4.0f;
                signal[1][0] = -3.0f;
            }
            Run(*limiter, signal, 0, length);
            left.insert(left.end(), signal[0].begin(), signal[0].end());
            right.insert(right.end(), signal[1].begin(), signal[1].end());
        }
        const double peak = TruePeakDb(left, right);
        Report("limiter true peak out, ceiling " + Text(ceilingDb), peak);
        Check(peak <= ceilingDb, "no true peak over the " + Text(ceilingDb) +
                                     " dBTP ceiling: " + Precise(peak));
        Check(peak > ceilingDb - 1.0,
              "the limiter brings the peaks to the ceiling, not far under: " +
                  Text(peak));
    }
    // A loud sine settles at the ceiling, smoothly.
    std::unique_ptr<Effect> limiter = CreateEffect("limiter", kRate, 1);
    limiter->SetParameter("ceiling", -6.0);
    limiter->Reset();
    Stereo signal = {Sine(200.0, 1.0, kRate)};
    RunWhole(*limiter, signal);
    float most = 0.0f;
    for (size_t n = kRate / 2; n < static_cast<size_t>(kRate); ++n) {
        most = std::max(most, std::abs(signal[0][n]));
    }
    Check(std::abs(Db(most) + 6.0) < 0.3,
          "a sine 6 dB over settles at the ceiling: " + Text(Db(most)));
}

// White noise and tone bursts (on half a second, off half a second): the
// noise between the bursts goes down by at least 10 dB, the tone stays
// within 1 dB, and the ratio of the tone to what is left of the noise rises
// by at least 6 dB.
void TestDenoise() {
    std::unique_ptr<Effect> effect = CreateEffect("denoise", kRate, 1);
    effect->SetParameter("reduction", 20.0);
    effect->Reset();
    const size_t latency = effect->Latency();
    const size_t length = static_cast<size_t>(kRate) * 8;
    const size_t half = kRate / 2;
    const std::vector<float> noise = Noise(21, length, 0.02f);
    const std::vector<float> tone = Sine(1000.0, 0.3, length);
    const auto on = [](size_t n) { return (n / half) % 2 == 1; };
    std::vector<float> input(length);
    for (size_t n = 0; n < length; ++n) {
        input[n] = noise[n] + (on(n) ? tone[n] : 0.0f);
    }
    Stereo signal = {input};
    RunWhole(*effect, signal);
    const std::vector<float>& out = signal[0];
    const auto rms = [](const std::vector<float>& x, size_t begin, size_t end) {
        double sum = 0.0;
        for (size_t n = begin; n < end; ++n) {
            sum += static_cast<double>(x[n]) * x[n];
        }
        return std::sqrt(sum / static_cast<double>(end - begin));
    };
    // After the warm-up: segment 14 is noise alone, 15 a burst; a quarter
    // of a segment off each edge, latency compensated.
    const size_t offBegin = 14 * half + half / 4;
    const size_t onBegin = 15 * half + half / 4;
    const size_t span = half / 2;
    const double reduced =
        Db(rms(out, offBegin + latency, offBegin + latency + span) /
           rms(input, offBegin, offBegin + span));
    Report("denoise: noise alone (dB)", reduced);
    Check(reduced < -10.0,
          "the noise alone goes at least 10 dB down: " + Text(reduced));
    const double kept = Db(ToneAmplitude(out, onBegin + latency,
                                         onBegin + latency + span, 1000.0) /
                           0.3);
    Report("denoise: tone (dB)", kept);
    Check(std::abs(kept) < 1.0, "the tone stays within 1 dB: " + Text(kept));
    // What is left once the tone is taken out, before and after.
    const auto residual = [&](const std::vector<float>& x, size_t begin,
                              double phaseShift) {
        double sum = 0.0;
        for (size_t n = 0; n < span; ++n) {
            const double clean =
                0.3 *
                std::sin(2.0 * kPi * 1000.0 *
                         static_cast<double>(begin + n - phaseShift) / kRate);
            const double rest = x[begin + n] - clean;
            sum += rest * rest;
        }
        return std::sqrt(sum / static_cast<double>(span));
    };
    const double signalRms = 0.3 / std::sqrt(2.0);
    const double before = Db(signalRms / residual(input, onBegin, 0.0));
    const double after = Db(signalRms / residual(out, onBegin + latency,
                                                 static_cast<double>(latency)));
    Report("denoise: SNR before (dB)", before);
    Report("denoise: SNR after (dB)", after);
    Check(after - before > 6.0,
          "the signal-to-noise ratio rises by at least 6 dB: " + Text(before) +
              " to " + Text(after));
}

// Hum at 50 Hz and 150 Hz under a 1 kHz program: each harmonic at least
// 30 dB down, the program within 0.1 dB; set to 60 Hz, 50 Hz is left.
void TestDeHum() {
    const size_t length = static_cast<size_t>(kRate) * 2;
    const std::vector<float> h1 = Sine(50.0, 0.2, length, 0.1);
    const std::vector<float> h3 = Sine(150.0, 0.2, length, 0.1);
    const std::vector<float> program = Sine(1000.0, 0.2, length);
    std::vector<float> input(length);
    for (size_t n = 0; n < length; ++n) {
        input[n] = h1[n] + h3[n] + program[n];
    }
    std::unique_ptr<Effect> effect = CreateEffect("de_hum", kRate, 1);
    Stereo signal = {input};
    RunWhole(*effect, signal);
    const size_t tail = length / 2;
    const double at50 = Db(ToneAmplitude(signal[0], tail, length, 50.0) / 0.2);
    const double at150 =
        Db(ToneAmplitude(signal[0], tail, length, 150.0) / 0.2);
    const double at1k =
        Db(ToneAmplitude(signal[0], tail, length, 1000.0) / 0.2);
    Report("de-hum: 50 Hz (dB)", at50);
    Report("de-hum: 150 Hz (dB)", at150);
    Report("de-hum: 1 kHz (dB)", at1k);
    Check(at50 < -30.0 && at150 < -30.0,
          "50 Hz and its third harmonic at least 30 dB down: " + Text(at50) +
              ", " + Text(at150));
    Check(std::abs(at1k) < 0.1, "1 kHz within 0.1 dB: " + Text(at1k));
    std::unique_ptr<Effect> sixty = CreateEffect("de_hum", kRate, 1);
    sixty->SetParameter("frequency", 1.0);
    sixty->Reset();
    Stereo hum = {h1};
    RunWhole(*sixty, hum);
    Check(Db(ToneAmplitude(hum[0], tail, length, 50.0) / 0.2) > -1.0,
          "the 60 Hz comb leaves 50 Hz alone");
}

// Sibilance over the threshold is taken down; a low voice is not touched.
void TestDeEsser() {
    const size_t length = static_cast<size_t>(kRate);
    std::unique_ptr<Effect> effect = CreateEffect("de_esser", kRate, 1);
    effect->SetParameter("threshold", -30.0);
    effect->SetParameter("reduction", 10.0);
    effect->Reset();
    Stereo hiss = {Sine(8000.0, 0.5, length)};
    RunWhole(*effect, hiss);
    const double down =
        Db(ToneAmplitude(hiss[0], length / 2, length, 8000.0) / 0.5);
    Report("de-esser: 8 kHz (dB)", down);
    // The shelf at 6 kHz has most of its 10 dB at 8 kHz.
    Check(
        down < -6.0 && down > -10.5,
        "a loud 8 kHz is taken down, by at most the reduction: " + Text(down));
    effect->Reset();
    Stereo voice = {Sine(300.0, 0.5, length)};
    RunWhole(*effect, voice);
    const double kept =
        Db(ToneAmplitude(voice[0], length / 2, length, 300.0) / 0.5);
    Check(std::abs(kept) < 0.05, "a 300 Hz voice is left: " + Text(kept));
}

// A burst at full scale and beyond, then silence for 40 s: the output stays
// finite and no sample is a denormal.
void TestDenormals() {
    for (const std::string& id : EffectIds()) {
        std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 2);
        Engage(*effect);
        Stereo loud = {Noise(3, 24000, 4.0f), Noise(4, 24000, 4.0f)};
        loud[0][100] = 1e6f;
        loud[1][200] = -1e6f;
        RunWhole(*effect, loud);
        bool clean = true;
        for (int second = 0; second < 40; ++second) {
            Stereo silence = {std::vector<float>(kRate, 0.0f),
                              std::vector<float>(kRate, 0.0f)};
            RunWhole(*effect, silence);
            for (const std::vector<float>& channel : silence) {
                for (const float value : channel) {
                    clean = clean && std::isfinite(value) &&
                            std::fpclassify(value) != FP_SUBNORMAL;
                }
            }
        }
        Check(clean, id + ": a silence after a sound has no denormal");
    }
}

void TestReset() {
    for (const std::string& id : EffectIds()) {
        std::unique_ptr<Effect> effect = CreateEffect(id, kRate, 2);
        Engage(*effect);
        effect->Reset();
        const Stereo source = {Noise(8, 6000, 0.5f), Noise(9, 6000, 0.5f)};
        Stereo first = source;
        RunWhole(*effect, first);
        Stereo other = {Noise(10, 3000, 0.9f), Noise(11, 3000, 0.9f)};
        RunWhole(*effect, other);
        effect->Reset();
        Stereo second = source;
        RunWhole(*effect, second);
        Check(SameBits(first[0], second[0]) && SameBits(first[1], second[1]),
              id + ": Reset gives the first run's output again");
    }
}

}  // namespace

int main() {
    TestRegistry();
    TestBlockIndependence();
    TestNoAllocation();
    TestNeutralIsIdentity();
    TestLatency();
    TestBiquadResponse();
    TestCompressor();
    TestLimiter();
    TestDenoise();
    TestDeHum();
    TestDeEsser();
    TestDenormals();
    TestReset();
    if (failures != 0) {
        std::cerr << failures << " audio DSP check(s) failed\n";
        return 1;
    }
    std::cout << "audio DSP tests passed\n";
    return 0;
}
