#pragma once

#include "Document.h"
#include "RationalTime.h"
#include "Transcription.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ITW2 -- ROADMAP.md. Hesitations found in the signal, not in the words.
//
// On the ITW-2026-09 interview, whisper large-v3 in verbatim wrote none of
// the fourteen hesitations the editor finally had cut. tighten_pauses closes
// silences only, and the skill's "sound outside words" check, with its eight
// frames of margin after each word, hid an "euh" stuck to the word before.
// They came in three families (the words quoted here are made up):
//
//   - a detached "euh": a vowel held 0.4 to 0.8 s, flat or slowly falling;
//   - a stretched syllable: "la maa...rée" (17 frames of held vowel),
//     "un-euuh pont" (9 frames), "tables-euh";
//   - a murmur between two words, 25 to 33 dB under the speech.
//
// What separated them from speech needed no transcript: how stable the
// timbre is. Speech changes its spectral envelope every few tens of
// milliseconds; a held vowel does not. Measured as the mean distance between
// successive MFCC vectors over +/-50 ms, it split six confirmed hesitations
// from normal speech with no overlap. A hold under 200 ms inside a word is
// the normal prosody of a syllable and is not reported.
//
// AGENT-2026-09 A48 -- docs/roadmap/AGENT-2026-09.md. Between two aligned
// words the floor is lower. The editor marked twelve "euh" on the export of
// the second native run of the 26th; the detector had seen five. The rest
// were holds of 100 to 190 ms sitting after a word's aligned end or before
// the next word's start ("fenêtre-euh ouverte", "jardin,-euh puis",
// "cheval-euh sans"), under the 200 ms floor, and two long holds the
// detector had split in two on a break of 30 to 40 ms in the stability
// ("souvent-euh dormir", "rivaaage"). A break of one frame no longer ends
// a hold, and a hold no word claims counts from 100 ms: on those four
// rushes that lists 30 holds inside the montage instead of 6, eleven of the
// twelve marks among them.
//
// Measured on the four rushes that montage plays: 20 candidates, 15 inside
// the stretch of each rush the montage uses, and 11 of those 15 fall in
// frames the editor's final cut removed -- "la maa...rée", the "euh" of
// "un-euh pont" and the one after "tables" among them. The other four are
// still played: false alarms or hesitations kept, which only listening
// tells apart.
//
// Measurement only: this lists candidates with their numbers and the words
// around them. Which to cut is the caller's decision, made by listening.

struct HesitationSettings {
    // A voiced stretch is held when its timbre stability falls under this
    // share of the rush's median voiced stability. Relative, because the
    // absolute figure depends on the MFCC analysis: the roadmap's 5.5-6.3
    // against 8.8-10.3 came from another window size than this file's, whose
    // speech medians sit near 33. The ratio carried over: "la maa...rée"
    // held at 0.68 of its rush's median here, the confirmed hesitations at
    // 0.58-0.66 there, normal speech at 0.93-1.08.
    int64_t held_stability_percent = 76;
    // Inside a word (a stretched syllable) or without a transcript.
    int64_t minimum_held_milliseconds = 200;
    // Between two aligned words (a filler): A48.
    int64_t minimum_gap_held_milliseconds = 100;
    // A break in the hold no longer than this joins its two sides: one
    // frame at 25 fps.
    int64_t bridge_milliseconds = 40;
    // A held stretch must be voice: within this many dB of the rush's speech
    // level (90th percentile of window levels).
    int64_t held_level_range_db = 18;
    // A murmur sits this far under the speech level, and above the floor.
    int64_t murmur_min_db_below_speech = 22;
    int64_t murmur_max_db_below_speech = 36;
    int64_t minimum_murmur_milliseconds = 150;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
};

struct HesitationCandidate {
    // "filler" between words, "stretched_syllable" inside one, "murmur"
    // between words, or "held" when there is no transcript to place it.
    std::string family;
    // Source time on the 10 ms analysis grid, [start, end).
    RationalTime start{0, 100};
    RationalTime end{0, 100};
    // Mean timbre stability over the stretch, and its level against the
    // rush's speech level, both times 1000 / in whole dB.
    int64_t stability_milli = 0;
    int64_t level_db_below_speech = 0;
    std::string previous_word;
    std::string next_word;
    // For a stretched syllable, the word it stretches.
    std::string within_word;
};

struct HesitationReport {
    std::string media_id;
    int32_t windows_per_second = 100;
    int64_t speech_level_db = -120;
    int64_t floor_level_db = -120;
    // Median stability of the rush's voiced windows, times 1000: the scale
    // held_stability_percent applies to.
    int64_t median_stability_milli = 0;
    // Per 10 ms window: stability times 1000 (0 where unvoiced), level dBFS.
    std::vector<int64_t> stability_milli;
    std::vector<int64_t> level_db;
    std::vector<HesitationCandidate> candidates;
};

// Pure core: mono PCM at 16 kHz in, per-window stability and level out.
void MeasureTimbreStability(const std::vector<float>& samples,
                            std::vector<double>& stability,
                            std::vector<double>& levelDb);

// Pure core: candidates from the measurements, placed against `words` (the
// source's aligned transcript, possibly empty).
void DetectHesitations(const std::vector<double>& stability,
                       const std::vector<double>& levelDb,
                       const std::vector<TranscriptWord>& words,
                       const HesitationSettings& settings,
                       HesitationReport& report);

// Decodes `inputPath`'s audio, measures and detects, and writes the cache.
bool GenerateHesitationReport(const std::string& inputPath,
                              const std::string& outputPath,
                              const std::string& mediaId,
                              const std::vector<TranscriptWord>& words,
                              const HesitationSettings& settings,
                              HesitationReport& report, std::string& error);

bool LoadHesitationReport(const std::string& path, HesitationReport& report,
                          std::string& error);

std::string SerializeHesitationReport(const HesitationReport& report,
                                      bool includeWindows);

// The project-facing pair every backend uses: the cache lives at
// `<project dir>/.cutmachine/hesitations/<media_id>.json`, and the words
// placing each candidate are the source's aligned words (LoadSourceWords).
// A source without a transcript is still analysed; its candidates are
// "held".
bool AnalyzeProjectSourceHesitations(const std::filesystem::path& projectPath,
                                     const std::vector<LibraryMedia>& library,
                                     const Ulid& sourceId,
                                     const HesitationSettings& settings,
                                     HesitationReport& report,
                                     std::string& error);
bool ReadProjectSourceHesitations(const std::filesystem::path& projectPath,
                                  const Ulid& sourceId,
                                  HesitationReport& report, std::string& error);
