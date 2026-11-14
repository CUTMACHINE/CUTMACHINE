#include "AudioDspFilters.h"

#include <algorithm>
#include <string>

namespace audio_dsp {

namespace {

const std::vector<std::string>& BandTypeNames() {
    static const std::vector<std::string> kNames = {
        "peaking", "low_shelf", "high_shelf", "high_pass", "low_pass"};
    return kNames;
}

BiquadType BandType(int index) {
    switch (index) {
        case 1:
            return BiquadType::kLowShelf;
        case 2:
            return BiquadType::kHighShelf;
        case 3:
            return BiquadType::kHighPass;
        case 4:
            return BiquadType::kLowPass;
        default:
            return BiquadType::kPeaking;
    }
}

std::string BandKey(int band, const char* name) {
    return "band" + std::to_string(band + 1) + "." + name;
}

// Butterworth: one section of Q 1/sqrt(2), or two of these Qs.
constexpr double kSecondOrderQ = 0.70710678118654752;
constexpr double kFourthOrderQ[2] = {0.54119610014619698, 1.3065629648763766};

}  // namespace

GlidingSection::GlidingSection(int32_t rate, int channels)
    : rate_(static_cast<double>(rate)),
      log_frequency_(std::log2(1000.0), RampSamples(rate)),
      gain_(0.0, RampSamples(rate)),
      q_(1.0, RampSamples(rate)),
      mix_(0.0, RampSamples(rate)),
      states_(static_cast<size_t>(channels)) {
    Design();
}

void GlidingSection::SetTargets(bool enabled, BiquadType type, double frequency,
                                double gainDb, double q, bool snap) {
    if (type != type_) {
        type_ = type;
        dirty_ = true;
    }
    log_frequency_.SetTarget(std::log2(std::max(frequency, 1.0)));
    gain_.SetTarget(gainDb);
    q_.SetTarget(q);
    mix_.SetTarget(enabled ? 1.0 : 0.0);
    if (snap) {
        log_frequency_.Snap();
        gain_.Snap();
        q_.Snap();
        mix_.Snap();
        Design();
        dirty_ = false;
        counter_ = 0;
    }
}

void GlidingSection::Advance() {
    log_frequency_.Next();
    gain_.Next();
    q_.Next();
    mix_.Next();
    if (log_frequency_.Moving() || gain_.Moving() || q_.Moving()) {
        if (counter_ == 0) {
            Design();
        }
        counter_ = (counter_ + 1) % kCoefficientInterval;
        dirty_ = true;
    } else if (dirty_) {
        // Landed: the targets' own coefficients, at once.
        Design();
        dirty_ = false;
        counter_ = 0;
    }
}

double GlidingSection::Process(int channel, double x) {
    BiquadState& state = states_[static_cast<size_t>(channel)];
    const double mix = mix_.Value();
    if (mix <= 0.0) {
        // Out of the chain: whatever it held is not heard again.
        state.Reset();
        return x;
    }
    const double y = state.Process(coefficients_, x);
    return mix >= 1.0 ? y : x + mix * (y - x);
}

void GlidingSection::Reset() {
    log_frequency_.Snap();
    gain_.Snap();
    q_.Snap();
    mix_.Snap();
    for (BiquadState& state : states_) {
        state.Reset();
    }
    Design();
    dirty_ = false;
    counter_ = 0;
}

double GlidingSection::TargetResponseDb(double frequency) const {
    if (mix_.Target() < 0.5) {
        return 0.0;
    }
    return BiquadCoefficients::Design(type_, rate_,
                                      std::exp2(log_frequency_.Target()),
                                      q_.Target(), gain_.Target())
        .MagnitudeDb(frequency, rate_);
}

void GlidingSection::Design() {
    coefficients_ = BiquadCoefficients::Design(
        type_, rate_, std::exp2(log_frequency_.Value()), q_.Value(),
        gain_.Value());
}

// ---------------------------------------------------------------------------

ParametricEq::ParametricEq(int32_t rate, int channels)
    : Effect(rate, channels), output_gain_(1.0, RampSamples(rate)) {
    for (GlidingSection& band : bands_) {
        band = GlidingSection(rate, channels);
    }
    InitializeParameters();
}

const std::vector<ParameterSpec>& ParametricEq::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = [] {
        static constexpr double kFrequencies[kBands] = {
            80.0, 250.0, 1000.0, 3000.0, 6000.0, 12000.0};
        std::vector<ParameterSpec> specs;
        for (int band = 0; band < kBands; ++band) {
            specs.push_back(
                {BandKey(band, "enabled"), 0.0, 1.0, 1.0, Unit::kToggle, {}});
            specs.push_back({BandKey(band, "type"), 0.0, 4.0, 0.0,
                             Unit::kChoice, BandTypeNames()});
            specs.push_back({BandKey(band, "frequency"),
                             20.0,
                             20000.0,
                             kFrequencies[band],
                             Unit::kHertz,
                             {}});
            specs.push_back(
                {BandKey(band, "gain"), -24.0, 24.0, 0.0, Unit::kDecibels, {}});
            specs.push_back({BandKey(band, "q"), 0.1, 18.0, 1.0, Unit::kQ, {}});
        }
        specs.push_back({"output_gain", -24.0, 24.0, 0.0, Unit::kDecibels, {}});
        return specs;
    }();
    return kSpecs;
}

void ParametricEq::ApplyParameters(bool snap) {
    // Five parameters a band, in the table's order, then the output gain.
    for (int band = 0; band < kBands; ++band) {
        const size_t first = static_cast<size_t>(band) * 5;
        bands_[static_cast<size_t>(band)].SetTargets(
            ValueAt(first) >= 0.5,
            BandType(static_cast<int>(ValueAt(first + 1))), ValueAt(first + 2),
            ValueAt(first + 3), ValueAt(first + 4), snap);
    }
    output_gain_.SetTarget(DbToGain(ValueAt(kBands * 5)));
    if (snap) {
        output_gain_.Snap();
    }
}

void ParametricEq::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    for (size_t i = 0; i < frames; ++i) {
        for (GlidingSection& band : bands_) {
            band.Advance();
        }
        const double gain = output_gain_.Next();
        for (int channel = 0; channel < count; ++channel) {
            double x = channels[channel][i];
            for (GlidingSection& band : bands_) {
                x = band.Process(channel, x);
            }
            channels[channel][i] = static_cast<float>(x * gain);
        }
    }
}

void ParametricEq::Reset() {
    for (GlidingSection& band : bands_) {
        band.Reset();
    }
    output_gain_.Snap();
}

double ParametricEq::ResponseDb(double frequency) const {
    double db = 20.0 * std::log10(output_gain_.Target());
    for (const GlidingSection& band : bands_) {
        db += band.TargetResponseDb(frequency);
    }
    return db;
}

// ---------------------------------------------------------------------------

HighPass::HighPass(int32_t rate, int channels) : Effect(rate, channels) {
    for (GlidingSection& section : sections_) {
        section = GlidingSection(rate, channels);
    }
    InitializeParameters();
}

const std::vector<ParameterSpec>& HighPass::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = {
        {"enabled", 0.0, 1.0, 1.0, Unit::kToggle, {}},
        {"frequency", 10.0, 500.0, 80.0, Unit::kHertz, {}},
        {"slope", 0.0, 1.0, 0.0, Unit::kChoice, {"12db", "24db"}},
    };
    return kSpecs;
}

void HighPass::ApplyParameters(bool snap) {
    const bool enabled = On("enabled");
    const bool steep = Index("slope") == 1;
    const double frequency = Value("frequency");
    sections_[0].SetTargets(enabled, BiquadType::kHighPass, frequency, 0.0,
                            steep ? kFourthOrderQ[0] : kSecondOrderQ, snap);
    sections_[1].SetTargets(enabled && steep, BiquadType::kHighPass, frequency,
                            0.0, kFourthOrderQ[1], snap);
}

void HighPass::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    for (size_t i = 0; i < frames; ++i) {
        for (GlidingSection& section : sections_) {
            section.Advance();
        }
        for (int channel = 0; channel < count; ++channel) {
            double x = channels[channel][i];
            for (GlidingSection& section : sections_) {
                x = section.Process(channel, x);
            }
            channels[channel][i] = static_cast<float>(x);
        }
    }
}

void HighPass::Reset() {
    for (GlidingSection& section : sections_) {
        section.Reset();
    }
}

double HighPass::ResponseDb(double frequency) const {
    return sections_[0].TargetResponseDb(frequency) +
           sections_[1].TargetResponseDb(frequency);
}

// ---------------------------------------------------------------------------

DeHum::DeHum(int32_t rate, int channels)
    : Effect(rate, channels), amount_(1.0, RampSamples(rate)) {
    for (GlidingSection& notch : notches_) {
        notch = GlidingSection(rate, channels);
    }
    InitializeParameters();
}

const std::vector<ParameterSpec>& DeHum::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = {
        {"frequency", 0.0, 1.0, 0.0, Unit::kChoice, {"50hz", "60hz"}},
        {"harmonics", 1.0, 10.0, 4.0, Unit::kNone, {}},
        {"q", 1.0, 100.0, 30.0, Unit::kQ, {}},
        {"amount", 0.0, 100.0, 100.0, Unit::kPercent, {}},
    };
    return kSpecs;
}

void DeHum::ApplyParameters(bool snap) {
    const double fundamental = Index("frequency") == 1 ? 60.0 : 50.0;
    const int harmonics = static_cast<int>(std::lround(Value("harmonics")));
    const double q = Value("q");
    for (int k = 0; k < kMaxHarmonics; ++k) {
        const double frequency = fundamental * (k + 1);
        const bool active =
            k < harmonics && frequency < 0.45 * static_cast<double>(Rate());
        notches_[static_cast<size_t>(k)].SetTargets(active, BiquadType::kNotch,
                                                    frequency, 0.0, q, snap);
    }
    amount_.SetTarget(Value("amount") / 100.0);
    if (snap) {
        amount_.Snap();
    }
}

void DeHum::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    for (size_t i = 0; i < frames; ++i) {
        for (GlidingSection& notch : notches_) {
            notch.Advance();
        }
        const double amount = amount_.Next();
        for (int channel = 0; channel < count; ++channel) {
            const double dry = channels[channel][i];
            double wet = dry;
            for (GlidingSection& notch : notches_) {
                wet = notch.Process(channel, wet);
            }
            const double out =
                amount >= 1.0
                    ? wet
                    : (amount <= 0.0 ? dry : dry + amount * (wet - dry));
            channels[channel][i] = static_cast<float>(out);
        }
    }
}

void DeHum::Reset() {
    for (GlidingSection& notch : notches_) {
        notch.Reset();
    }
    amount_.Snap();
}

}  // namespace audio_dsp
