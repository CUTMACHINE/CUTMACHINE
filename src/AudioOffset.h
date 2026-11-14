#pragma once

// SYNCHRO-2026-10 SY1 -- docs/roadmap/SYNCHRO-2026-10.md. How far the sound
// of one recording sits from another's: a recorder's mix against the sound
// a camera took of the same take, or two cameras on one take. Read only: it
// says where, it places nothing (SY2 does).
//
// Two passes, written from the problem, not from any implementation read:
// - coarse, on where each sound gets louder (the rise of its log RMS, one
//   value per millisecond): a close microphone and a distant one rise
//   together where their waveforms, filtered and reverberated differently,
//   do not. The whole lag range is searched by one FFT correlation; its
//   peak gives the region, its height above what chance reaches over all
//   lags, and against the best peak elsewhere, gives the confidence.
// - fine, on the samples themselves, around that region, in a few windows
//   spread over the overlap (head, middle, tail): a cross-correlation
//   weighted by phase only (GCC-PHAT), which keeps the direct sound's arrival
//   sharp under room reverberation and does not care what each microphone's
//   frequency response is. Each window gives an offset exact to the sample;
//   their spread over the overlap is the drift between the two clocks.
//
// No value is guessed: no overlap, a correlation too weak, a peak another
// one matches (a clock's tick, a music loop, a tone) or a drift of more than
// half a frame over the overlap are refusals by name.
//
// Plain C++; the files are decoded by an FFmpeg process, as --speech-onset
// decodes a rush, so the engine needs no decoder of its own here.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AudioOffsetSettings {
    // The rate both sounds are given at, and the unit of every offset.
    int32_t sample_rate = 48000;
    // Samples per envelope value: 48 is one millisecond at 48 kHz.
    int32_t envelope_hop = 48;
    // Shortest overlap a lag may leave, in samples (2 s at 48 kHz).
    int64_t minimum_overlap = 96000;
    // The fine windows' length, and how far each searches around the coarse
    // lag (50 ms: wide enough for the drift a window may show).
    int64_t window = int64_t{8} * 48000;
    int64_t search_margin = 2400;
    // How far the coarse peak must stand above chance (the scores' median,
    // in median absolute deviations), below which the two sounds are not
    // taken for one take.
    double minimum_significance = 10.0;
    // The coarse peak against the best one at least `peak_exclusion`
    // envelope values away (both above the median), below which the lag is
    // ambiguous.
    double minimum_peak_ratio = 1.5;
    int64_t peak_exclusion = 100;
    // A frame of the picture, as num/den seconds: the drift over the
    // overlap may not exceed half of it. 1/25 by default.
    int64_t frame_num = 1;
    int64_t frame_den = 25;
};

struct AudioOffsetWindow {
    // The window's first sample on the reference.
    int64_t at = 0;
    // The offset this window measured.
    int64_t offset = 0;
};

struct AudioOffsetReport {
    // Where the other sound's first sample falls on the reference, in
    // samples: reference[n] is heard as other[n - offset]. Negative when the
    // other started first.
    int64_t offset = 0;
    // The two sounds' common span, on the reference.
    int64_t overlap_first = 0;
    int64_t overlap_end = 0;
    // The onsets' correlation at the lag found (Pearson, -1 to 1): a
    // reading, not a criterion.
    double correlation = 0.0;
    // The coarse peak above chance, in deviations.
    double significance = 0.0;
    // The coarse peak's height over the best one away from it.
    double peak_ratio = 0.0;
    std::vector<AudioOffsetWindow> windows;
    // The clocks' drift, from the first window to the last: parts per
    // million, and samples over the whole overlap.
    double drift_ppm = 0.0;
    int64_t drift_samples = 0;
};

// One channel as the measure takes it: the left, the right, or their mean.
enum class AudioOffsetChannel : uint8_t { Mix, Left, Right };

// "mix", "left" or "right", as the CLI and the MCP take a channel. False for
// any other name.
bool ParseAudioOffsetChannel(const std::string& name,
                             AudioOffsetChannel& channel);

// The channel each of the two rushes is read on.
struct AudioOffsetChannels {
    AudioOffsetChannel reference = AudioOffsetChannel::Mix;
    AudioOffsetChannel other = AudioOffsetChannel::Mix;
};

// The first audio stream of `path`, one channel of it, as 32-bit floats at
// `sampleRate`, decoded by the resolved FFmpeg (FfmpegBinary.h) the way
// --speech-onset reads a rush. A recorder may hold the lavalier on one
// channel and the boom on the other: the measure takes the one it is told.
// `cancelled` is polled while FFmpeg runs.
bool DecodeAudioForOffset(const std::string& path, AudioOffsetChannel channel,
                          int32_t sampleRate,
                          const std::function<bool()>& cancelled,
                          std::vector<float>& samples, std::string& error);

// False with `code` (NoOverlap, WeakCorrelation, AmbiguousPeak, ClockDrift,
// InvalidInput) and `error` when the offset cannot be told. `reference` and
// `other` are mono, at settings.sample_rate.
bool MeasureAudioOffset(const std::vector<float>& reference,
                        const std::vector<float>& other,
                        const AudioOffsetSettings& settings,
                        AudioOffsetReport& report, std::string& code,
                        std::string& error);

// The answer's body for the CLI and the MCP, without "ok": every time in
// samples at settings.sample_rate as {value, rate}, and the offset in whole
// frames of `frame` (rounded to the nearest, said) for reading only.
std::string AudioOffsetReportJson(const AudioOffsetReport& report,
                                  const AudioOffsetSettings& settings);
