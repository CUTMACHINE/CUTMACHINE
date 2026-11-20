#pragma once

// MOTION-MARQUE-2026-10 MM1 -- docs/roadmap/MOTION-MARQUE-2026-10.md. A
// sound delivered at a loudness, measured and limited as a broadcast meter
// does it: ITU-R BS.1770-4 for the loudness (K-weighting, 400 ms blocks
// every 100 ms, gated at -70 LUFS then 10 LU under the ungated mean) and
// its true peak (the signal oversampled four times), EBU R 128's units.
//
// The films of a brand job once went through FFmpeg by hand after
// render_motion_video, whose mix sums its sounds without a limiter (the
// first render peaked at +1.9 dBFS): gain, limiter, -16 LUFS, -1 dBTP. This
// does the same in the engine, on the engine's own mix (MotionAudio.h),
// before any codec touches it.
//
// Portable: plain C++ on float samples. The sound-processing work the author
// asked for then (a voice chain, ducking) needs this meter and this
// limiter too; they are written for any rate, though the motion mix is
// always 48 kHz.

#include <cstdint>
#include <optional>
#include <vector>

struct LoudnessMeasure {
    // Integrated loudness, LUFS; absent when every block falls under the
    // absolute gate (silence).
    std::optional<double> integrated_lufs;
    // The highest sample of the signal oversampled four times, in dBTP;
    // -infinity for silence.
    double true_peak_dbtp = 0.0;
    // The highest sample as it is, in dBFS.
    double sample_peak_dbfs = 0.0;
};

// Two channels of the same length at `rate` Hz, each weighted 1 (BS.1770's
// left and right).
LoudnessMeasure MeasureLoudness(const std::vector<float>& left,
                                const std::vector<float>& right, int32_t rate);

// The true peak alone, in dBTP: the measure a limiter is checked against.
double TruePeakDb(const std::vector<float>& left,
                  const std::vector<float>& right);

struct LoudnessTarget {
    double integrated_lufs = -16.0;
    // The ceiling the true peak stays under, dBTP.
    double true_peak_dbtp = -1.0;
};

struct LoudnessResult {
    LoudnessMeasure before;
    LoudnessMeasure after;
    // The gain applied before limiting, and the most the limiter took off
    // on top of it, both in dB (the second 0 or negative).
    double gain_db = 0.0;
    double limited_db = 0.0;
    // Silence: nothing to bring to a loudness, the samples are untouched.
    bool silent = false;
};

// Brings the two channels to `target`: a gain to the integrated loudness,
// then a limiter that keeps every oversampled peak under the ceiling -- a
// look-ahead of 5 ms, so that the gain is down before a peak arrives, and a
// release of 100 ms, both channels together. The limiter takes loudness
// off a dense mix, so the gain is set again until the loudness is within
// 0.05 LU of the target, or the limiter cannot give it back (a target too
// loud for the ceiling: `after` says where it ended).
void NormalizeLoudness(std::vector<float>& left, std::vector<float>& right,
                       int32_t rate, const LoudnessTarget& target,
                       LoudnessResult& result);
