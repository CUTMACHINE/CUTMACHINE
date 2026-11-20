#pragma once

// FC6 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/audio-dsp/src/ducking.rs (MIT OR Apache-2.0, commit 09aaf9b). The
// ducking a music should follow under a voice, proposed and not applied:
// where a voice track speaks, and the gain curve a music under it would
// take. Asked for under a voice-over in REEL-2026-09 (R2, the clip gain
// animated) and with the voice chain of MOTION-MARQUE-2026-10; the
// animated gain does not exist yet, so nothing here writes the document --
// the proposal is read, by the agent or by whoever mixes, until it does.
//
// What changes from FilmCraft: its times are seconds in floating point,
// its threshold an absolute level. Here every bound is a sample of the
// sound's own rate, as a RationalTime of that rate, and every duration in
// milliseconds must land on a whole sample (at 48 kHz, every one does): a
// setting that does not is refused, never rounded. The threshold sits over
// the rush's own noise floor, as the hollows of TimelineAudioReport.h do: a
// level in dBFS says nothing of a room, its distance over the floor does.
//
// The voice, in 10 ms frames: each frame's RMS level; the floor, a low
// percentile of the frames over -100 dBFS (digital silence and padding
// left out); speech, where the level rises `threshold_db` over the floor,
// held until it falls `hysteresis_db` under that, so that a word's own dips
// do not open and close it. Gaps shorter than `bridge_milliseconds` are
// closed, then ranges shorter than `minimum_speech_milliseconds` dropped (a
// cough, a door). The curve: the music at 0 dB, down `depth_db` from
// `attack_milliseconds` before each range to its start, held to its end,
// back up over `release_milliseconds` after it. Two ranges whose ramps
// would meet stay ducked between them. A ramp that would start before the
// sound does starts with it; one that would end after it ends with it.
//
// Portable, on samples already decoded.

#include "RationalTime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audio_dsp {

// The rate the CLI and the MCP decode a rush at before proposing.
inline constexpr int32_t kDuckingSampleRate = 48000;

struct DuckingSettings {
    // Over the floor, to enter speech; under that by the hysteresis, to
    // leave it.
    int64_t threshold_db = 10;
    int64_t hysteresis_db = 4;
    // The percentile of the frames' levels that is the floor.
    int64_t floor_percentile = 10;
    int64_t minimum_speech_milliseconds = 200;
    int64_t bridge_milliseconds = 300;
    // The music's gain under the voice, in dB down.
    int64_t depth_db = 12;
    int64_t attack_milliseconds = 150;
    int64_t release_milliseconds = 400;
};

struct DuckingRange {
    RationalTime start{0, 1};
    RationalTime end{0, 1};
};

// The curve between two points is a straight line in dB.
struct DuckingPoint {
    RationalTime time{0, 1};
    // 0, or minus the depth.
    int64_t gain_db = 0;
};

struct DuckingProposal {
    int32_t sample_rate = 48000;
    RationalTime duration{0, 1};
    // Absent when every frame is digital silence.
    bool silent = false;
    double floor_dbfs = -120.0;
    // The loudest frames (the 95th percentile), for the reader: how far
    // the voice stands over the floor.
    double speech_level_dbfs = -120.0;
    // The levels that open and close a range.
    double enter_dbfs = -120.0;
    double leave_dbfs = -120.0;
    std::vector<DuckingRange> speech;
    std::vector<DuckingPoint> curve;
};

// False with `error` for settings out of range, a rate that is not a whole
// number of samples every 10 ms, or a duration that is not a whole number
// of samples at `rate`. Callers check it before decoding a rush, so that a
// wrong setting is refused at once, not after an hour of sound.
bool ValidateDuckingSettings(const DuckingSettings& settings, int32_t rate,
                             std::string& error);

// False with `code` (InvalidInput) and `error` where
// ValidateDuckingSettings refuses, or for a duration that is not a whole
// number of samples at `rate`.
bool ProposeDucking(const std::vector<float>& samples, int32_t rate,
                    const DuckingSettings& settings, DuckingProposal& proposal,
                    std::string& code, std::string& error);

// The answer's body for the CLI and the MCP, without "ok": every time as
// {value, rate} at the sound's rate, levels in dBFS to the hundredth.
std::string DuckingProposalJson(const DuckingProposal& proposal,
                                const DuckingSettings& settings);

}  // namespace audio_dsp
