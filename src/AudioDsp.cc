#include "AudioDsp.h"

#include "AudioDspDenoise.h"
#include "AudioDspDynamics.h"
#include "AudioDspFilters.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace audio_dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

double ParameterSpec::Sanitize(double value) const {
    if (std::isnan(value)) {
        return default_value;
    }
    const double clamped = std::clamp(value, minimum, maximum);
    if (unit == Unit::kToggle || unit == Unit::kChoice) {
        return std::round(clamped);
    }
    return clamped;
}

Smoothed::Smoothed(double value, int64_t rampSamples)
    : current_(value),
      target_(value),
      ramp_(std::max<int64_t>(0, rampSamples)) {}

void Smoothed::SetTarget(double target) {
    if (target == target_) {
        return;
    }
    target_ = target;
    if (ramp_ == 0) {
        current_ = target;
        remaining_ = 0;
        return;
    }
    remaining_ = ramp_;
    step_ = (target - current_) / static_cast<double>(ramp_);
}

void Smoothed::Snap() {
    current_ = target_;
    remaining_ = 0;
}

double Smoothed::Next() {
    if (remaining_ > 0) {
        --remaining_;
        // The last step lands on the target itself, not on a sum of steps.
        current_ = remaining_ == 0 ? target_ : current_ + step_;
    }
    return current_;
}

int64_t RampSamples(int32_t rate) {
    return static_cast<int64_t>(
        std::llround(kParameterRampMilliseconds * rate / 1000.0));
}

Effect::Effect(int32_t rate, int channels) : rate_(rate), channels_(channels) {}

void Effect::InitializeParameters() {
    values_.clear();
    for (const ParameterSpec& spec : Parameters()) {
        values_.push_back(spec.default_value);
    }
    ApplyParameters(true);
}

bool Effect::SetParameter(std::string_view id, double value) {
    const std::vector<ParameterSpec>& specs = Parameters();
    for (size_t index = 0; index < specs.size(); ++index) {
        if (specs[index].id == id) {
            values_[index] = specs[index].Sanitize(value);
            ApplyParameters(false);
            return true;
        }
    }
    return false;
}

std::optional<double> Effect::Parameter(std::string_view id) const {
    const std::vector<ParameterSpec>& specs = Parameters();
    for (size_t index = 0; index < specs.size(); ++index) {
        if (specs[index].id == id) {
            return values_[index];
        }
    }
    return std::nullopt;
}

double Effect::Value(std::string_view id) const {
    // Only ever asked for the effect's own identifiers.
    return Parameter(id).value_or(0.0);
}

const std::vector<std::string>& EffectIds() {
    static const std::vector<std::string> kIds = {
        "parametric_eq", "high_pass", "compressor", "de_esser",
        "limiter",       "denoise",   "de_hum"};
    return kIds;
}

std::unique_ptr<Effect> CreateEffect(std::string_view id, int32_t rate,
                                     int channels) {
    if (rate < 8000 || rate > 384000 || channels < 1 || channels > 8) {
        return nullptr;
    }
    if (id == "parametric_eq") {
        return std::make_unique<ParametricEq>(rate, channels);
    }
    if (id == "high_pass") {
        return std::make_unique<HighPass>(rate, channels);
    }
    if (id == "compressor") {
        return std::make_unique<Compressor>(rate, channels);
    }
    if (id == "de_esser") {
        return std::make_unique<DeEsser>(rate, channels);
    }
    if (id == "limiter") {
        return std::make_unique<Limiter>(rate, channels);
    }
    if (id == "denoise") {
        return std::make_unique<Denoise>(rate, channels);
    }
    if (id == "de_hum") {
        return std::make_unique<DeHum>(rate, channels);
    }
    return nullptr;
}

BiquadCoefficients BiquadCoefficients::Design(BiquadType type, double rate,
                                              double frequency, double q,
                                              double gainDb) {
    const double f = std::clamp(frequency, 1.0, rate * 0.49);
    const double quality = std::max(q, 1e-3);
    const double w0 = 2.0 * kPi * f / rate;
    const double sn = std::sin(w0);
    const double cs = std::cos(w0);
    const double alpha = sn / (2.0 * quality);
    const double a = std::pow(10.0, gainDb / 40.0);
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;
    switch (type) {
        case BiquadType::kPeaking:
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cs;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha / a;
            break;
        case BiquadType::kLowShelf: {
            const double k = 2.0 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1.0) - (a - 1.0) * cs + k);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cs);
            b2 = a * ((a + 1.0) - (a - 1.0) * cs - k);
            a0 = (a + 1.0) + (a - 1.0) * cs + k;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cs);
            a2 = (a + 1.0) + (a - 1.0) * cs - k;
            break;
        }
        case BiquadType::kHighShelf: {
            const double k = 2.0 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1.0) + (a - 1.0) * cs + k);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cs);
            b2 = a * ((a + 1.0) + (a - 1.0) * cs - k);
            a0 = (a + 1.0) - (a - 1.0) * cs + k;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cs);
            a2 = (a + 1.0) - (a - 1.0) * cs - k;
            break;
        }
        case BiquadType::kHighPass:
            b0 = (1.0 + cs) / 2.0;
            b1 = -(1.0 + cs);
            b2 = (1.0 + cs) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case BiquadType::kLowPass:
            b0 = (1.0 - cs) / 2.0;
            b1 = 1.0 - cs;
            b2 = (1.0 - cs) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
        case BiquadType::kNotch:
            b0 = 1.0;
            b1 = -2.0 * cs;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cs;
            a2 = 1.0 - alpha;
            break;
    }
    // At 0 dB a peaking or shelving section has b == a term for term
    // before the division, so b0 is 1 and the state never leaves zero.
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double BiquadCoefficients::MagnitudeDb(double frequency, double rate) const {
    const double w = 2.0 * kPi * frequency / rate;
    const double c1 = std::cos(w);
    const double s1 = std::sin(w);
    const double c2 = std::cos(2.0 * w);
    const double s2 = std::sin(2.0 * w);
    const double nr = b0 + b1 * c1 + b2 * c2;
    const double ni = -(b1 * s1 + b2 * s2);
    const double dr = 1.0 + a1 * c1 + a2 * c2;
    const double di = -(a1 * s1 + a2 * s2);
    return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

}  // namespace audio_dsp
