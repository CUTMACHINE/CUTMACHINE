#pragma once

// GN6a -- docs/roadmap/GENERATIF-2026-10.md. The engine runs the local
// voice-over model itself.
//
// A voice-over is generated on this machine by Qwen3-TTS (Apache 2.0),
// chosen by ear on 7 November 2026 against Chatterbox (DEC47): no service,
// no spending, no text leaving the machine. Two models do it. VoiceDesign
// makes a voice from a written description; it draws a new voice at every
// call, so a voice-over is said by Base, which clones a reference clip made
// once by VoiceDesign and keeps that voice from one take to the next (0.988
// speaker similarity between two cloned takes against 0.850 uncloned, on the
// bench in tools/voice-over-benchmark).
//
// As for MFA (MfaAlignment.h), the model stays in its own Python
// environment: the engine starts scripts/voice_over.py with that
// environment's Python, as a media task the caller can follow and cancel,
// and reads back the WAV and the report the script wrote. Three things are
// looked for, by ToolFiles.h's order:
//   - the environment's Python: CUTMACHINE_VOICE_PYTHON, then
//     <exe>/voice/env/bin/python, the build's, build/voice/env/bin/python;
//   - the script: CUTMACHINE_VOICE_SCRIPT, then <exe>/../scripts/
//     voice_over.py, the source tree's, scripts/voice_over.py;
//   - the models folder: CUTMACHINE_VOICE_MODELS, then <exe>/voice/models,
//     the build's, build/voice/models. It holds one folder per model, as
//     `hf download --local-dir` writes them (kVoiceDesignModel,
//     kVoiceCloneModel), each with its config.json and model.safetensors.
// Nothing is downloaded or installed. What is missing is the refusal
// VoiceOverNotFound, which names every place looked at.
//
// Nothing here asks for Apple: on Linux the same script runs on the CPU or
// a graphics card. Which voices may be cloned (only the engine's own, DEC49)
// is decided by the tools that call this (GN6b), not by the script.

#include "MediaTaskManager.h"
#include "ToolFiles.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The machine-local settings that point at each thing.
inline constexpr const char* kVoicePythonVariable = "CUTMACHINE_VOICE_PYTHON";
inline constexpr const char* kVoiceScriptVariable = "CUTMACHINE_VOICE_SCRIPT";
inline constexpr const char* kVoiceModelsVariable = "CUTMACHINE_VOICE_MODELS";

// The stable name surfaces give the refusal (CLI and MCP `error`).
inline constexpr const char* kVoiceOverNotFoundErrorName = "VoiceOverNotFound";

// The two model folders under the models folder: the Hugging Face
// repositories' names, under Qwen/.
inline constexpr const char* kVoiceDesignModel =
    "Qwen3-TTS-12Hz-1.7B-VoiceDesign";
inline constexpr const char* kVoiceCloneModel = "Qwen3-TTS-12Hz-1.7B-Base";

// The files of a model folder the script cannot run without.
const std::vector<std::string>& VoiceModelFiles();

struct VoiceOverSettings {
    // The interpreter of the model's environment, which runs the script.
    std::string python;
    // scripts/voice_over.py.
    std::string script;
    // The folder that holds the two model folders.
    std::string models;
    // Why the search found nothing usable; empty when it found everything.
    std::string refusal;

    // LocateVoiceOver(CurrentVoiceOverSearch()).Settings().
    static VoiceOverSettings FromEnvironment();
};

// One sentence naming everything the voice-over needs and cannot find, with
// the variables that point elsewhere; empty when nothing is missing. It
// starts with "voice-over model not found: " (VoiceOverRefusalName).
std::string MissingVoiceOverInstallation(const VoiceOverSettings& settings);

// Everything the search reads, so its order can be tested without touching
// the process.
struct VoiceOverSearch {
    // The three variables' values; empty when unset.
    std::string python_variable;
    std::string script_variable;
    std::string models_variable;
    // The directory of the running executable; empty when unknown.
    std::string executable_directory;
    // Where the build installs the three; an empty one is skipped.
    std::string build_python;
    std::string build_script;
    std::string build_models;
    // The working directory. Empty: relative paths stay relative.
    std::string working_directory;
    // The settings file a refusal tells the user to edit.
    std::string settings_file;
    // Whether a regular file, or a folder, is at a path. Null: the file
    // system.
    std::function<bool(const std::string&)> is_file;
    std::function<bool(const std::string&)> is_folder;
};

struct VoiceOverLocation {
    tool_files::File python;
    tool_files::File script;
    tool_files::File models;
    // The model folders (kVoiceDesignModel, kVoiceCloneModel) the models
    // folder lacks, or holds without one of VoiceModelFiles(). Empty when
    // the models folder was found nowhere: nothing is known of it.
    std::vector<std::string> missing_models;
    // "voice-over model not found: " and what is missing, then the
    // variables to set. Empty when everything was found.
    std::string refusal;

    bool Found() const { return refusal.empty(); }
    VoiceOverSettings Settings() const;
};

// The order above, applied to `search`: no environment.
VoiceOverLocation LocateVoiceOver(const VoiceOverSearch& search);

// What LocateVoiceOver reads for this process: the three variables (the
// environment, then the settings file), the executable's directory, the
// build's own paths and the working directory.
VoiceOverSearch CurrentVoiceOverSearch();

// kVoiceOverNotFoundErrorName when `message` holds a refusal of this file,
// else empty.
std::string VoiceOverRefusalName(const std::string& message);

// The languages the models say, by ISO 639-1 code, and the name Qwen3-TTS
// takes for each ("fr" -> "French"). Empty for a code it does not say.
std::string VoiceOverLanguageName(const std::string& code);
// The codes, in Qwen3-TTS's order.
std::vector<std::string> VoiceOverLanguageCodes();

enum class VoiceOverMode : std::uint8_t {
    // VoiceDesign: `text` said in a voice made from `description`.
    kDesign,
    // Base: `text` said in the voice of `reference_audio`, whose words are
    // `reference_text`.
    kClone,
};

struct VoiceOverRequest {
    VoiceOverMode mode = VoiceOverMode::kDesign;
    std::string text;
    // An ISO 639-1 code VoiceOverLanguageName knows.
    std::string language;
    std::string description;
    std::string reference_audio;
    std::string reference_text;
    // The sampling seed, recorded with the result.
    std::uint32_t seed = 0;
    // The WAV the script writes (PCM 24 bits, mono, at the model's rate),
    // and its JSON report.
    std::string output_path;
    std::string report_path;
};

// What the script reports of what it made.
struct VoiceOverResult {
    std::int64_t sample_rate = 0;
    std::int64_t samples = 0;
    // The model folder's name and the Hugging Face commit it was downloaded
    // at; the revision is empty when the folder does not say.
    std::string model;
    std::string revision;
    // "mps", "cuda" or "cpu".
    std::string device;
    std::uint32_t seed = 0;
};

// The arguments RunVoiceOver gives the interpreter, after the script.
// Separate so that what crosses to Python can be read in a test.
std::vector<std::string> VoiceOverArguments(const VoiceOverSettings& settings,
                                            const VoiceOverRequest& request);

// Runs the script for `request`. Refuses before starting anything a request
// the script could not serve: an unknown language, an empty text, a design
// without its description, a clone without its reference. Cancelled through
// `context`: the script and what it started stop together. On failure
// `error` carries the refusal, or the end of what the script printed on its
// error output.
bool RunVoiceOver(const VoiceOverSettings& settings,
                  const VoiceOverRequest& request, MediaTaskContext& context,
                  VoiceOverResult& result, std::string& error);
