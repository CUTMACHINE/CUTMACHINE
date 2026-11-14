#pragma once

// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/audio-dsp/src/effects/dynamics.rs (MIT OR Apache-2.0, commit
// 09aaf9b). The dynamics of a voice chain: a compressor, a de-esser and a
// true-peak limiter. Detection is linked across channels -- the loudest
// drives one gain for all -- so a stereo image does not move.

#include "AudioDsp.h"
#include "AudioDspFilters.h"
#include "AudioDspTruePeak.h"

#include <vector>

namespace audio_dsp {

// The gain a compressor's static curve takes off a level, in dB (0 or
// negative): nothing under `threshold - knee / 2`, (1 / ratio - 1) times
// the excess above `threshold + knee / 2`, and a quadratic between. At
// ratio 1 it is 0 everywhere, exactly.
double CompressorReductionDb(double levelDb, double thresholdDb, double ratio,
                             double kneeDb);

// Feed-forward, peak detector. "threshold" (-60..0 dB), "ratio" (1..30),
// "knee" (0..24 dB), "attack" (0.1..300 ms), "release" (5..3000 ms),
// "makeup" (-24..24 dB). Ratio 1 and makeup 0 are the identity.
class Compressor final : public Effect {
public:
    Compressor(int32_t rate, int channels);
    const char* Id() const override { return "compressor"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    void Reset() override;
    // The reduction applied to the last sample, in dB.
    double GainReductionDb() const { return envelope_db_; }

protected:
    void ApplyParameters(bool snap) override;

private:
    Smoothed threshold_;
    Smoothed ratio_;
    Smoothed knee_;
    Smoothed makeup_;
    double attack_ = 0.0;
    double release_ = 0.0;
    double envelope_db_ = 0.0;
};

// Sibilance taken down where it is: the sound above "frequency"
// (2000..12000 Hz) is watched through a Butterworth high-pass, its peak
// level followed (1 ms up, 60 ms down), and while it stands above
// "threshold" (-60..0 dB) a high shelf at that frequency takes it down, four
// to one, by at most "reduction" (0..24 dB). A dynamic shelf rather than a
// band split: a high band taken off the input does not add back to it in
// phase, and a split that reduces a band also shifts the rest. The shelf
// is designed again every sample; at 0 dB it is the identity, so a
// reduction of 0 returns the sound as it came, to the bit.
class DeEsser final : public Effect {
public:
    DeEsser(int32_t rate, int channels);
    const char* Id() const override { return "de_esser"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    void Reset() override;
    double GainReductionDb() const { return reduction_db_; }

protected:
    void ApplyParameters(bool snap) override;

private:
    GlidingSection detector_;
    Smoothed log_frequency_;
    Smoothed threshold_;
    Smoothed range_;
    double attack_ = 0.0;
    double release_ = 0.0;
    double level_ = 0.0;
    double reduction_db_ = 0.0;
    std::vector<BiquadState> shelves_;
};

// A look-ahead limiter on the true peak: "input_gain" (0..24 dB), "ceiling"
// (-24..0 dBTP), "release" (1..1000 ms). Each sample's peak, oversampled
// four times through the meter's own filter (AudioDspTruePeak.h), asks for
// the gain that brings it under the ceiling; that gain holds over every
// sample whose interpolation reads the peak (the filter's half-length on
// each side), a one-pole lets it back up, and a moving average 5 ms long
// smooths it -- each average a mean of values that all keep the peak under
// the ceiling, so the gain is down before the peak arrives. The samples
// are then clipped at the ceiling. The gains aim 2^-20 (0.00001 dB) under
// the ceiling, so that the rounding of the samples to float cannot put a
// peak over it. The delay is the 5 ms plus the
// detector's two half-lengths.
class Limiter final : public Effect {
public:
    Limiter(int32_t rate, int channels);
    const char* Id() const override { return "limiter"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    size_t Latency() const override { return latency_; }
    void Reset() override;
    double GainReductionDb() const;

protected:
    void ApplyParameters(bool snap) override;

private:
    Smoothed input_gain_;
    Smoothed ceiling_;
    double release_ = 0.0;
    size_t box_ = 1;
    size_t window_ = 1;
    size_t latency_ = 0;
    std::vector<TruePeakDetector> detectors_;
    // Each channel's gained input, `latency_ + 1` long.
    std::vector<std::vector<float>> delay_;
    size_t write_ = 0;
    // The sliding minimum of the gains asked: a ring of (index, gain), its
    // gains increasing from head to tail.
    std::vector<int64_t> queue_index_;
    std::vector<double> queue_gain_;
    size_t queue_head_ = 0;
    size_t queue_size_ = 0;
    std::vector<double> box_values_;
    size_t box_position_ = 0;
    double box_sum_ = 0.0;
    double released_ = 1.0;
    double gain_ = 1.0;
    int64_t time_ = 0;
};

}  // namespace audio_dsp
