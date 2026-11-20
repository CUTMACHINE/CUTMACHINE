#include "AudioDspDynamics.h"

#include <algorithm>
#include <cmath>

namespace audio_dsp {

namespace {

// A one-pole's coefficient for a time constant; 0 (no memory) at 0 ms.
double Pole(double milliseconds, int32_t rate) {
    return milliseconds <= 0.0 ? 0.0
                               : std::exp(-1.0 / (milliseconds * 0.001 * rate));
}

double LevelDb(double amplitude) {
    return 20.0 * std::log10(std::max(amplitude, 1e-10));
}

constexpr double kButterworthQ = 0.70710678118654752;

}  // namespace

double CompressorReductionDb(double levelDb, double thresholdDb, double ratio,
                             double kneeDb) {
    const double slope = 1.0 / ratio - 1.0;
    const double over = levelDb - thresholdDb;
    if (kneeDb > 0.0 && 2.0 * std::abs(over) <= kneeDb) {
        const double into = over + kneeDb / 2.0;
        return slope * into * into / (2.0 * kneeDb);
    }
    return over > 0.0 ? slope * over : 0.0;
}

// ---------------------------------------------------------------------------

Compressor::Compressor(int32_t rate, int channels)
    : Effect(rate, channels),
      threshold_(-20.0, RampSamples(rate)),
      ratio_(4.0, RampSamples(rate)),
      knee_(6.0, RampSamples(rate)),
      makeup_(1.0, RampSamples(rate)) {
    InitializeParameters();
}

const std::vector<ParameterSpec>& Compressor::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = {
        {"threshold", -60.0, 0.0, -20.0, Unit::kDecibels, {}},
        {"ratio", 1.0, 30.0, 4.0, Unit::kRatio, {}},
        {"knee", 0.0, 24.0, 6.0, Unit::kDecibels, {}},
        {"attack", 0.1, 300.0, 10.0, Unit::kMilliseconds, {}},
        {"release", 5.0, 3000.0, 100.0, Unit::kMilliseconds, {}},
        {"makeup", -24.0, 24.0, 0.0, Unit::kDecibels, {}},
    };
    return kSpecs;
}

void Compressor::ApplyParameters(bool snap) {
    threshold_.SetTarget(Value("threshold"));
    ratio_.SetTarget(Value("ratio"));
    knee_.SetTarget(Value("knee"));
    makeup_.SetTarget(DbToGain(Value("makeup")));
    attack_ = Pole(Value("attack"), Rate());
    release_ = Pole(Value("release"), Rate());
    if (snap) {
        threshold_.Snap();
        ratio_.Snap();
        knee_.Snap();
        makeup_.Snap();
    }
}

void Compressor::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    for (size_t i = 0; i < frames; ++i) {
        const double threshold = threshold_.Next();
        const double ratio = ratio_.Next();
        const double knee = knee_.Next();
        const double makeup = makeup_.Next();
        double peak = 0.0;
        for (int channel = 0; channel < count; ++channel) {
            peak = std::max(
                peak, std::abs(static_cast<double>(channels[channel][i])));
        }
        const double target =
            CompressorReductionDb(LevelDb(peak), threshold, ratio, knee);
        // Down at the attack's pace, back up at the release's.
        const double pole = target < envelope_db_ ? attack_ : release_;
        envelope_db_ = pole * envelope_db_ + (1.0 - pole) * target;
        if (std::abs(envelope_db_) < 1e-12) {
            envelope_db_ = 0.0;
        }
        const double gain = DbToGain(envelope_db_) * makeup;
        for (int channel = 0; channel < count; ++channel) {
            channels[channel][i] =
                static_cast<float>(channels[channel][i] * gain);
        }
    }
}

void Compressor::Reset() {
    threshold_.Snap();
    ratio_.Snap();
    knee_.Snap();
    makeup_.Snap();
    envelope_db_ = 0.0;
}

// ---------------------------------------------------------------------------

DeEsser::DeEsser(int32_t rate, int channels)
    : Effect(rate, channels),
      detector_(rate, channels),
      log_frequency_(std::log2(6000.0), RampSamples(rate)),
      threshold_(-30.0, RampSamples(rate)),
      range_(8.0, RampSamples(rate)),
      attack_(Pole(1.0, rate)),
      release_(Pole(60.0, rate)),
      shelves_(static_cast<size_t>(channels)) {
    InitializeParameters();
}

const std::vector<ParameterSpec>& DeEsser::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = {
        {"frequency", 2000.0, 12000.0, 6000.0, Unit::kHertz, {}},
        {"threshold", -60.0, 0.0, -30.0, Unit::kDecibels, {}},
        {"reduction", 0.0, 24.0, 8.0, Unit::kDecibels, {}},
    };
    return kSpecs;
}

void DeEsser::ApplyParameters(bool snap) {
    detector_.SetTargets(true, BiquadType::kHighPass, Value("frequency"), 0.0,
                         kButterworthQ, snap);
    log_frequency_.SetTarget(std::log2(Value("frequency")));
    threshold_.SetTarget(Value("threshold"));
    range_.SetTarget(Value("reduction"));
    if (snap) {
        log_frequency_.Snap();
        threshold_.Snap();
        range_.Snap();
    }
}

void DeEsser::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    const auto rate = static_cast<double>(Rate());
    for (size_t i = 0; i < frames; ++i) {
        detector_.Advance();
        const double frequency = std::exp2(log_frequency_.Next());
        const double threshold = threshold_.Next();
        const double range = range_.Next();
        double peak = 0.0;
        for (int channel = 0; channel < count; ++channel) {
            peak = std::max(peak, std::abs(detector_.Process(
                                      channel, channels[channel][i])));
        }
        // A peak follower on the band: fast up, slow down.
        const double pole = peak > level_ ? attack_ : release_;
        level_ = Flush(pole * level_ + (1.0 - pole) * peak);
        const double over = LevelDb(level_) - threshold;
        reduction_db_ = over > 0.0 ? -std::min(range, 0.75 * over) : 0.0;
        const BiquadCoefficients shelf =
            BiquadCoefficients::Design(BiquadType::kHighShelf, rate, frequency,
                                       kButterworthQ, reduction_db_);
        for (int channel = 0; channel < count; ++channel) {
            channels[channel][i] = static_cast<float>(
                shelves_[static_cast<size_t>(channel)].Process(
                    shelf, channels[channel][i]));
        }
    }
}

void DeEsser::Reset() {
    detector_.Reset();
    log_frequency_.Snap();
    threshold_.Snap();
    range_.Snap();
    level_ = 0.0;
    reduction_db_ = 0.0;
    for (BiquadState& shelf : shelves_) {
        shelf.Reset();
    }
}

// ---------------------------------------------------------------------------

Limiter::Limiter(int32_t rate, int channels)
    : Effect(rate, channels),
      input_gain_(1.0, RampSamples(rate)),
      ceiling_(DbToGain(-1.0), RampSamples(rate)),
      box_(
          std::max<size_t>(1, static_cast<size_t>(std::llround(0.005 * rate)))),
      window_(box_ + 2 * kTruePeakDelay),
      latency_(box_ + 2 * kTruePeakDelay - 1),
      detectors_(static_cast<size_t>(channels)),
      delay_(static_cast<size_t>(channels),
             std::vector<float>(latency_ + 1, 0.0f)),
      queue_index_(window_ + 1, 0),
      queue_gain_(window_ + 1, 1.0),
      box_values_(box_, 1.0),
      box_sum_(static_cast<double>(box_)) {
    InitializeParameters();
}

const std::vector<ParameterSpec>& Limiter::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = {
        {"input_gain", 0.0, 24.0, 0.0, Unit::kDecibels, {}},
        {"ceiling", -24.0, 0.0, -1.0, Unit::kDecibels, {}},
        {"release", 1.0, 1000.0, 60.0, Unit::kMilliseconds, {}},
    };
    return kSpecs;
}

void Limiter::ApplyParameters(bool snap) {
    input_gain_.SetTarget(DbToGain(Value("input_gain")));
    ceiling_.SetTarget(DbToGain(Value("ceiling")));
    release_ = Pole(Value("release"), Rate());
    if (snap) {
        input_gain_.Snap();
        ceiling_.Snap();
    }
}

void Limiter::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    const size_t length = latency_ + 1;
    const size_t capacity = queue_index_.size();
    for (size_t i = 0; i < frames; ++i) {
        const double inputGain = input_gain_.Next();
        // Aimed a hair under the ceiling (2^-20, 0.00001 dB): the samples
        // are rounded to float after the gain, which could otherwise put a
        // peak one rounding over it.
        const double ceiling = ceiling_.Next() * (1.0 - 0x1p-20);
        // The peak of the sample kTruePeakDelay behind this one.
        double peak = 0.0;
        for (int channel = 0; channel < count; ++channel) {
            const auto x = static_cast<float>(channels[channel][i] * inputGain);
            delay_[static_cast<size_t>(channel)][write_] = x;
            peak = std::max(peak,
                            detectors_[static_cast<size_t>(channel)].Push(x));
        }
        const double asked = peak > ceiling ? ceiling / peak : 1.0;
        const int64_t k = time_ - kTruePeakDelay;
        // Sliding minimum over the last window_ asks.
        while (queue_size_ > 0 &&
               queue_gain_[(queue_head_ + queue_size_ - 1) % capacity] >=
                   asked) {
            --queue_size_;
        }
        queue_index_[(queue_head_ + queue_size_) % capacity] = k;
        queue_gain_[(queue_head_ + queue_size_) % capacity] = asked;
        ++queue_size_;
        while (queue_index_[queue_head_] + static_cast<int64_t>(window_) <= k) {
            queue_head_ = (queue_head_ + 1) % capacity;
            --queue_size_;
        }
        const double held = queue_gain_[queue_head_];
        // Down at once, back up at the release's pace.
        released_ = held < released_
                        ? held
                        : released_ + (held - released_) * (1.0 - release_);
        box_sum_ += released_ - box_values_[box_position_];
        box_values_[box_position_] = released_;
        box_position_ = (box_position_ + 1) % box_;
        ++time_;
        // A sum kept by additions drifts; summed again now and then, on a
        // count of samples, never of blocks.
        if (time_ % 65536 == 0) {
            box_sum_ = 0.0;
            for (const double value : box_values_) {
                box_sum_ += value;
            }
        }
        gain_ = std::min(1.0, box_sum_ / static_cast<double>(box_));
        const size_t read = (write_ + 1) % length;
        for (int channel = 0; channel < count; ++channel) {
            const double y = delay_[static_cast<size_t>(channel)][read] * gain_;
            channels[channel][i] =
                static_cast<float>(std::clamp(y, -ceiling, ceiling));
        }
        write_ = read;
    }
}

void Limiter::Reset() {
    input_gain_.Snap();
    ceiling_.Snap();
    for (TruePeakDetector& detector : detectors_) {
        detector.Reset();
    }
    for (std::vector<float>& line : delay_) {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    write_ = 0;
    queue_head_ = 0;
    queue_size_ = 0;
    std::fill(box_values_.begin(), box_values_.end(), 1.0);
    box_position_ = 0;
    box_sum_ = static_cast<double>(box_);
    released_ = 1.0;
    gain_ = 1.0;
    time_ = 0;
}

double Limiter::GainReductionDb() const { return 20.0 * std::log10(gain_); }

}  // namespace audio_dsp
