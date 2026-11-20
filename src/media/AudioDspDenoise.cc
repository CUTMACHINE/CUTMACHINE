#include "AudioDspDenoise.h"

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audio_dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
// The smoothing of each bin's power from frame to frame.
constexpr double kPowerSmoothing = 0.8;
// The minimum of a smoothed periodogram sits under the noise's mean; this
// brings it back up (about 4 dB).
constexpr double kMinimumBias = 2.5;
// The window minima are searched over.
constexpr double kSearchSeconds = 1.5;
// How fast a bin's gain opens, per frame.
constexpr double kOpening = 0.7;

size_t FrameSizeFor(int32_t rate) {
    if (rate <= 50000) {
        return 2048;
    }
    return rate <= 100000 ? 4096 : 8192;
}

}  // namespace

Denoise::Denoise(int32_t rate, int channels)
    : Effect(rate, channels),
      size_(FrameSizeFor(rate)),
      hop_(size_ / 4),
      bins_(size_ / 2 + 1),
      frames_per_sub_window_(std::max<size_t>(
          1, static_cast<size_t>(std::llround(kSearchSeconds * rate /
                                              static_cast<double>(hop_) /
                                              kSubWindows)))),
      window_(size_),
      spectrum_(size_),
      channels_(static_cast<size_t>(channels)),
      floor_(1.0, RampSamples(rate)),
      threshold_(1.0, RampSamples(rate)) {
    // Square roots of a periodic Hann: their product sums to 2 at a
    // quarter-frame hop.
    for (size_t i = 0; i < size_; ++i) {
        window_[i] =
            std::sqrt(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                           static_cast<double>(size_)));
    }
    for (Channel& channel : channels_) {
        channel.input.assign(size_, 0.0f);
        channel.output.assign(size_, 0.0);
        channel.power.assign(bins_, 0.0);
        channel.minimum.assign(bins_, 0.0);
        channel.history.assign(bins_ * kSubWindows, 0.0);
        channel.gains.assign(bins_, 1.0);
        channel.smoothed.assign(bins_, 1.0);
        ResetChannel(channel);
    }
    InitializeParameters();
}

const std::vector<ParameterSpec>& Denoise::Parameters() const {
    static const std::vector<ParameterSpec> kSpecs = {
        {"reduction", 0.0, 40.0, 12.0, Unit::kDecibels, {}},
        {"threshold", 0.0, 20.0, 6.0, Unit::kDecibels, {}},
        {"smoothing", 0.0, 100.0, 50.0, Unit::kPercent, {}},
    };
    return kSpecs;
}

void Denoise::ApplyParameters(bool snap) {
    floor_.SetTarget(DbToGain(-Value("reduction")));
    threshold_.SetTarget(std::pow(10.0, Value("threshold") / 10.0));
    // 0 %: a gate that closes in a couple of frames; 100 %: in thirty.
    release_ = 0.6 - 0.57 * Value("smoothing") / 100.0;
    if (snap) {
        floor_.Snap();
        threshold_.Snap();
    }
}

void Denoise::ResetChannel(Channel& channel) {
    std::fill(channel.input.begin(), channel.input.end(), 0.0f);
    std::fill(channel.output.begin(), channel.output.end(), 0.0);
    std::fill(channel.power.begin(), channel.power.end(), 0.0);
    std::fill(channel.minimum.begin(), channel.minimum.end(),
              std::numeric_limits<double>::infinity());
    std::fill(channel.history.begin(), channel.history.end(),
              std::numeric_limits<double>::infinity());
    std::fill(channel.gains.begin(), channel.gains.end(), 1.0);
    std::fill(channel.smoothed.begin(), channel.smoothed.end(), 1.0);
    channel.position = 0;
    channel.history_position = 0;
    channel.frames_in_sub_window = 0;
    channel.started = false;
}

void Denoise::Reset() {
    floor_.Snap();
    threshold_.Snap();
    for (Channel& channel : channels_) {
        ResetChannel(channel);
    }
}

void Denoise::Process(float* const* channels, size_t frames) {
    const int count = Channels();
    for (size_t i = 0; i < frames; ++i) {
        floor_.Next();
        threshold_.Next();
        for (int index = 0; index < count; ++index) {
            Channel& channel = channels_[static_cast<size_t>(index)];
            channel.input[size_ - hop_ + channel.position] = channels[index][i];
            const double y = channel.output[channel.position];
            channels[index][i] = static_cast<float>(y);
            if (++channel.position == hop_) {
                channel.position = 0;
                std::copy(channel.output.begin() + static_cast<long>(hop_),
                          channel.output.end(), channel.output.begin());
                std::fill(channel.output.end() - static_cast<long>(hop_),
                          channel.output.end(), 0.0);
                Frame(channel);
                std::copy(channel.input.begin() + static_cast<long>(hop_),
                          channel.input.end(), channel.input.begin());
            }
        }
    }
}

void Denoise::Frame(Channel& channel) {
    for (size_t i = 0; i < size_; ++i) {
        spectrum_[i] = {channel.input[i] * window_[i], 0.0};
    }
    fft::Forward(spectrum_);
    const double floor = floor_.Value();
    const double threshold = threshold_.Value();
    const bool rotate =
        ++channel.frames_in_sub_window == frames_per_sub_window_;
    for (size_t k = 0; k < bins_; ++k) {
        const double p = std::norm(spectrum_[k]);
        // Flushed like every recursive state: after a long digital silence
        // the smoothed power would otherwise decay into subnormals, and the
        // minimum and its history with it.
        channel.power[k] =
            Flush(channel.started ? kPowerSmoothing * channel.power[k] +
                                        (1.0 - kPowerSmoothing) * p
                                  : p);
        channel.minimum[k] = std::min(channel.minimum[k], channel.power[k]);
        double lowest = channel.minimum[k];
        for (size_t u = 0; u < kSubWindows; ++u) {
            lowest = std::min(lowest, channel.history[u * bins_ + k]);
        }
        const double noise = lowest * kMinimumBias;
        const double target =
            channel.power[k] > noise * threshold ? 1.0 : floor;
        double& gain = channel.gains[k];
        gain = Flush(gain +
                     (target - gain) * (target > gain ? kOpening : release_));
        if (rotate) {
            channel.history[channel.history_position * bins_ + k] =
                channel.minimum[k];
            channel.minimum[k] = std::numeric_limits<double>::infinity();
        }
    }
    channel.started = true;
    if (rotate) {
        channel.history_position = (channel.history_position + 1) % kSubWindows;
        channel.frames_in_sub_window = 0;
    }
    for (size_t k = 0; k < bins_; ++k) {
        const double before = channel.gains[k == 0 ? 0 : k - 1];
        const double after = channel.gains[std::min(k + 1, bins_ - 1)];
        channel.smoothed[k] =
            0.25 * before + 0.5 * channel.gains[k] + 0.25 * after;
    }
    // The same real gain on a bin and its mirror keeps the spectrum
    // Hermitian, so the inverse is real; it is taken as the forward
    // transform of the conjugate, whose real part is the inverse's.
    for (size_t k = 0; k < size_; ++k) {
        const size_t bin = k < bins_ ? k : size_ - k;
        spectrum_[k] = std::conj(spectrum_[k]) * channel.smoothed[bin];
    }
    fft::Forward(spectrum_);
    const double scale = 1.0 / (2.0 * static_cast<double>(size_));
    for (size_t i = 0; i < size_; ++i) {
        channel.output[i] += spectrum_[i].real() * window_[i] * scale;
    }
}

}  // namespace audio_dsp
