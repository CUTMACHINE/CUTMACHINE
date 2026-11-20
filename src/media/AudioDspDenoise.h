#pragma once

// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/audio-dsp/src/effects/restoration.rs and effects/stft.rs (MIT OR
// Apache-2.0, commit 09aaf9b). Broadband noise taken down by spectral
// gating: a short-time Fourier transform (about 43 ms frames, 75 % overlap,
// square-root Hann windows on both sides, so that analysis then synthesis
// gives the sound back), and in every bin a noise floor followed by minimum
// statistics (Martin, 2001): the lowest of the smoothed power over the last
// 1.5 s, in eight sub-windows, corrected for its bias. Nothing has to be
// learned first, and the floor follows a room that changes.
//
// A bin that does not rise "threshold" dB above its floor is taken down by
// "reduction" dB. Gated bins one by one would sing (the "musical noise" of
// isolated bins opening and closing at random): each bin's gain opens fast
// and closes at the pace "smoothing" sets, then is averaged with its two
// neighbours in frequency.
//
// The transform is the engine's own (Fft.h), in double. The delay is one
// frame.

#include "AudioDsp.h"

#include <complex>
#include <vector>

namespace audio_dsp {

// "reduction" (0..40 dB, 0 gives the sound back, delayed by a frame),
// "threshold" (0..20 dB over the floor), "smoothing" (0..100 %).
class Denoise final : public Effect {
public:
    static constexpr int kSubWindows = 8;

    Denoise(int32_t rate, int channels);
    const char* Id() const override { return "denoise"; }
    const std::vector<ParameterSpec>& Parameters() const override;
    void Process(float* const* channels, size_t frames) override;
    size_t Latency() const override { return size_; }
    void Reset() override;
    // The frame length: 2048 samples up to 50 kHz, 4096 up to 100 kHz,
    // 8192 above.
    size_t FrameSize() const { return size_; }

protected:
    void ApplyParameters(bool snap) override;

private:
    struct Channel {
        std::vector<float> input;
        std::vector<double> output;
        std::vector<double> power;
        std::vector<double> minimum;
        // kSubWindows minima of earlier sub-windows, bin after bin.
        std::vector<double> history;
        std::vector<double> gains;
        std::vector<double> smoothed;
        size_t position = 0;
        size_t history_position = 0;
        size_t frames_in_sub_window = 0;
        bool started = false;
    };

    void Frame(Channel& channel);
    void ResetChannel(Channel& channel);

    size_t size_;
    size_t hop_;
    size_t bins_;
    size_t frames_per_sub_window_;
    std::vector<double> window_;
    std::vector<std::complex<double>> spectrum_;
    std::vector<Channel> channels_;
    Smoothed floor_;
    Smoothed threshold_;
    double release_ = 0.3;
};

}  // namespace audio_dsp
