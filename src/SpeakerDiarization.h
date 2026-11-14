#pragma once

// PA3 -- docs/roadmap/PARITE-2026-11.md. Who speaks when, in one rush.
//
// Nothing in the engine could tell two voices apart: an interview with its
// interviewer, two people on two cameras, a street interview where four
// students answer in turn were all one stream of words. This file gives
// each stretch of speech of a rush a voice, a number, and from there each
// word its voice.
//
// THE MODEL, AND WHAT IT WAS MEASURED AT. A speaker embedding model
// (WeSpeaker ResNet34-LM, trained on VoxCeleb, run by sherpa-onnx; both
// Apache 2.0) gives a vector to each 1.5 s window of speech, one window
// every 0.25 s; the vectors are gathered into as many groups as the caller
// says there are voices (scripts/diarize.py, which says why the usual
// pipeline was set aside). On 12 November 2026, on fifteen dialogues made
// of two real French voices each with every turn known: 5.9 % of the time
// given to the wrong voice on average, 4.2 % at the median, a change of
// voice found within 0.25 s, 25 % on one pair of very close voices; 4.1 %
// with three voices. That is a measure on voices spliced together, without
// the pauses a real conversation has: no real two-voice rush with a known
// answer was at hand.
//
// THE COUNT IS GIVEN, NEVER GUESSED: guessed, it was wrong on five
// dialogues of fifteen. The caller says how many voices the rush holds.
// What the engine gives back to judge it is `similarity`, how alike the
// mean vectors of two voices are, per mille: voices told apart measured 380
// to 800, one voice asked for two 877, the pairs the model confused 850 to
// 900. It measures and does not judge (principle 7): from 850 up the two
// groups are not to be relied on, and the answer says the number, not a
// verdict.
//
// A NUMBER, NOT A NAME. A voice is 0, 1, 2 in order of first appearance in
// its rush. Nothing here says who it is, and voice 0 of one rush is not
// voice 0 of another: naming a voice, or matching it across rushes, is the
// author's reading of the words.
//
// As for MFA (MfaAlignment.h) and the local voice-over (VoiceOver.h), the
// model stays in its own Python environment: the engine starts
// scripts/diarize.py with that environment's Python, as a media task the
// caller can follow and cancel, and reads back the report it wrote. Three
// things are looked for, by ToolFiles.h's order:
//   - the environment's Python: CUTMACHINE_DIARIZATION_PYTHON, then
//     <exe>/diarization/env/bin/python, the build's,
//     build/diarization/env/bin/python;
//   - the script: CUTMACHINE_DIARIZATION_SCRIPT, then
//     <exe>/../scripts/diarize.py, the source tree's, scripts/diarize.py;
//   - the embedding model: CUTMACHINE_DIARIZATION_MODEL, then
//     <exe>/diarization/models/<kSpeakerEmbeddingModel>, the build's,
//     build/diarization/models/<kSpeakerEmbeddingModel>.
// Nothing is downloaded or installed. What is missing is the refusal
// DiarizationNotFound, which names every place looked at.
//
// The result is a cache artifact, not document state:
// `.cutmachine/speakers/<media id>.json`, beside the transcripts.

#include "MediaTaskManager.h"
#include "RationalTime.h"
#include "ToolFiles.h"
#include "Transcription.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The machine-local settings that point at each thing.
inline constexpr const char* kDiarizationPythonVariable =
    "CUTMACHINE_DIARIZATION_PYTHON";
inline constexpr const char* kDiarizationScriptVariable =
    "CUTMACHINE_DIARIZATION_SCRIPT";
inline constexpr const char* kDiarizationModelVariable =
    "CUTMACHINE_DIARIZATION_MODEL";

// The stable name surfaces give the refusal (CLI and MCP `error`).
inline constexpr const char* kDiarizationNotFoundErrorName =
    "DiarizationNotFound";

// The embedding model's file, as sherpa-onnx's release names it.
inline constexpr const char* kSpeakerEmbeddingModel =
    "wespeaker_en_voxceleb_resnet34_LM.onnx";

// The most voices a rush may be said to hold.
inline constexpr int32_t kMaxDiarizedSpeakers = 8;

// The rate the model reads sound at, and the cache counts its times in.
inline constexpr int32_t kDiarizationSampleRate = 16000;

struct DiarizationSettings {
    std::string python;
    std::string script;
    std::string model;
    // Why the search found nothing usable; empty when it found everything.
    std::string refusal;

    // LocateDiarization(CurrentDiarizationSearch()).Settings().
    static DiarizationSettings FromEnvironment();
};

// One sentence naming everything the diarization needs and cannot find,
// with the variables that point elsewhere; empty when nothing is missing.
// It starts with "diarization model not found: ".
std::string MissingDiarizationInstallation(const DiarizationSettings& settings);

// Everything the search reads, so its order can be tested without touching
// the process.
struct DiarizationSearch {
    std::string python_variable;
    std::string script_variable;
    std::string model_variable;
    std::string executable_directory;
    std::string build_python;
    std::string build_script;
    std::string build_model;
    std::string working_directory;
    std::string settings_file;
    // Whether a regular file is at a path. Null: the file system.
    std::function<bool(const std::string&)> is_file;
};

struct DiarizationLocation {
    tool_files::File python;
    tool_files::File script;
    tool_files::File model;
    std::string refusal;

    bool Found() const { return refusal.empty(); }
    DiarizationSettings Settings() const;
};

DiarizationLocation LocateDiarization(const DiarizationSearch& search);
DiarizationSearch CurrentDiarizationSearch();

// kDiarizationNotFoundErrorName when `message` holds a refusal of this
// file, else empty.
std::string DiarizationRefusalName(const std::string& message);

// One stretch of one voice, in the rush's own time.
struct SpeakerSegment {
    RationalTime start;
    RationalTime end;
    int32_t speaker = 0;
};

// How alike two voices' mean vectors are, per mille.
struct SpeakerSimilarity {
    int32_t first = 0;
    int32_t second = 0;
    int32_t per_mille = 0;
};

// What `.cutmachine/speakers/<media id>.json` holds.
struct SpeakerDiarization {
    std::string media_id;
    // How many voices the caller said the rush holds, and how many of them
    // were given at least one stretch.
    int32_t speakers = 0;
    int32_t speakers_heard = 0;
    // In order, never overlapping.
    std::vector<SpeakerSegment> segments;
    std::vector<SpeakerSimilarity> similarity;
    // "words": the stretches of speech came from the rush's transcript;
    // "loudness": from the sound's level, the rush having no transcript.
    std::string speech_from;
    // What made it: "sherpa-onnx", its version, the model file's name.
    std::string engine;
    std::string engine_version;
    std::string embedding;
};

std::string SerializeSpeakerDiarization(const SpeakerDiarization& diarization);
bool DeserializeSpeakerDiarization(const std::string& json,
                                   SpeakerDiarization& diarization,
                                   std::string& error);
// False with `error` when the file is absent or is not a cache this build
// reads.
bool LoadSpeakerDiarization(const std::string& path,
                            SpeakerDiarization& diarization,
                            std::string& error);

// The script's report (times in samples at kDiarizationSampleRate) as a
// diarization of `mediaId`. False with `error` when the report is not the
// one scripts/diarize.py writes: a segment out of order, a voice outside
// the count, a time past the sound.
bool ReadDiarizationReport(const std::string& json, const std::string& mediaId,
                           SpeakerDiarization& diarization, std::string& error);

// The stretches where the transcript has someone speaking: its words, in
// order, joined across the gaps shorter than `gap`. A window of the model
// never crosses from one to the next: a voice changes in a pause far more
// often than inside a word.
struct SpeechStretch {
    RationalTime start;
    RationalTime end;
};
std::vector<SpeechStretch> SpeechStretchesOfWords(
    const std::vector<TranscriptWord>& words, const RationalTime& gap);

// Those stretches as the script reads them: "[[start, end], ...]" in whole
// samples at kDiarizationSampleRate, rounded outwards.
std::string SpeechStretchesJson(const std::vector<SpeechStretch>& stretches);

// The voice that speaks over most of [start, end): its number, or -1 when
// no stretch of any voice overlaps it. Ties go to the earlier voice.
int32_t SpeakerOfSpan(const SpeakerDiarization& diarization,
                      const RationalTime& start, const RationalTime& end);

// How long each voice speaks, by its number, in seconds as a rational.
std::vector<RationalTime> SpeakerDurations(
    const SpeakerDiarization& diarization);

struct DiarizationRequest {
    // PCM 16 bits, mono, kDiarizationSampleRate: made by the caller.
    std::string audio_path;
    // "[[start, end], ...]" in samples, or empty to let the script find
    // the speech by its level.
    std::string speech_path;
    int32_t speakers = 0;
    std::string report_path;
};

// The command line RunDiarization starts, for a test to read.
std::vector<std::string> DiarizationArguments(
    const DiarizationSettings& settings, const DiarizationRequest& request);

// Runs the script and leaves its report at `report_path`. Cancelled through
// `context`. On failure `error` carries the end of what the script printed
// on its error output.
bool RunDiarization(const DiarizationSettings& settings,
                    const DiarizationRequest& request,
                    MediaTaskContext& context, std::string& error);
