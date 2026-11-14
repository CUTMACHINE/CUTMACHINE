#pragma once

#include "Document.h"
#include "SpeechOnset.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// ITW8 -- ROADMAP.md. Measurements of the montage's own sound: the mix a
// player hears, assembled exactly as transcribe_timeline assembles it (the
// export's gains, fades, mix and limiter), without exporting anything.
//
// Two things made this necessary on the ITW-2026-09 interview. No tool
// rendered or measured the assembled sound, so the agent rebuilt it with
// FFmpeg from the clip bounds for every check of hollows, level and sync.
// And the skill looked for hollows under a fixed -30 dBFS: the room there sat
// near -25 dBFS, so the same montage had no hollow at -30, one at -25 and
// seven at -22. An absolute threshold says nothing about pacing. A hollow
// here is measured against the rush that plays it, from its own speech
// onset analysis: a window closer to the rush's noise floor than to its
// speech level. A margin in dB over the floor does not carry across rooms
// either: on that interview the floors sat 25 dB under the voice, and only a
// 12 to 16 dB margin found its breaths.

struct TimelineAudioSettings {
    // Where the hollow threshold sits between the playing rush's noise floor
    // (0) and its speech level (100), in dB. A window under it is hollow.
    int64_t dip_position_percent = 50;
    // Hollows shorter than this are breaths, not pacing.
    int64_t minimum_dip_milliseconds = 250;
    // The opening stretch whose level is reported on its own.
    int64_t head_milliseconds = 1500;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
};

struct TimelineAudioDip {
    RationalTime start{0, 1};
    RationalTime end{0, 1};
    // Highest threshold of the rushes playing, gain applied; absent in a
    // gap where no audio clip plays.
    std::optional<int64_t> threshold_dbfs;
    int64_t level_dbfs = -120;  // Mean over the hollow.
    // "hollow" inside playing audio, or "gap" where nothing plays.
    std::string kind;
};

struct TimelineAudioReport {
    RationalTime duration{0, 1};
    // Summary of FFmpeg's ebur128 over the whole mix.
    std::optional<double> integrated_lufs;
    std::optional<double> true_peak_dbfs;
    std::optional<double> loudness_range_lu;
    int64_t head_level_dbfs = -120;
    int64_t head_peak_dbfs = -120;
    // Time before the first sustained window above its rush's threshold:
    // the air at the head of the film.
    std::optional<RationalTime> head_air;
    std::vector<TimelineAudioDip> dips;
    RationalTime longest_dip{0, 1};
    // 10 ms windows analysed, and those whose rushes have no noise floor
    // (no speech onset analysis), which cannot hold a hollow.
    int64_t windows = 0;
    int64_t unmeasured_windows = 0;
    std::vector<Ulid> sources_without_floor;
};

// `floors` maps source ids to their speech onset analysis; the noise floor
// is all this reads. `projectPath` resolves relative media paths.
bool AnalyzeTimelineAudio(const Document& document,
                          const std::filesystem::path& projectPath,
                          const std::map<Ulid, SpeechOnsetReport>& floors,
                          const TimelineAudioSettings& settings,
                          TimelineAudioReport& report, std::string& error);

std::string SerializeTimelineAudioReport(const TimelineAudioReport& report,
                                         MediaRate rate);

// PA2 -- docs/roadmap/PARITE-2026-11.md. The loudness of a file's first
// audio stream as a broadcast meter reads it (FFmpeg's ebur128, the meter
// AnalyzeTimelineAudio reads the mix with): what a delivery is brought to,
// and what a delivered file is judged on.
struct FileLoudness {
    // Absent for silence: every block under the meter's absolute gate.
    std::optional<double> integrated_lufs;
    // Absent for digital silence, whose peak has no logarithm.
    std::optional<double> true_peak_dbfs;
    std::optional<double> loudness_range_lu;
};

// False with `error` when the file cannot be read or has no audio stream.
// `ffmpegPath` empty: the binary FfmpegBinary.h resolves.
bool MeasureFileLoudness(const std::string& path, const std::string& ffmpegPath,
                         FileLoudness& loudness, std::string& error);
