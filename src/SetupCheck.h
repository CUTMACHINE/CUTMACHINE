#pragma once

// PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. What this machine lacks, said
// before a tool fails on it.
//
// A new machine could ask `--check-ffmpeg` (PRD2), and nothing else.
// Whether the Whisper model was configured, whether MFA could be found,
// whether a model key was there came out one tool at a time -- the first
// transcribe_media, the first align_words -- each in the words of its own
// refusal.
//
// The report is a list of items, each the thing a family of tools needs:
//   ffmpeg           the binary FfmpegBinary.h resolves, probed by PRD2's
//                    diagnosis; the one item required to edit and export.
//   whisper_model    CUTMACHINE_WHISPER_MODEL, resolved as Transcription.h
//                    resolves it, and the file there readable and not empty.
//                    The window's menus use it too since DEC23.
//   whisper_hesitations_model
//                    CUTMACHINE_WHISPER_HESITATIONS_MODEL, the s075 weights
//                    French verbatim decodes with (AL4), and whether their
//                    SHA-256 is the evaluated one. Optional.
//   mfa              the three files of MFA's installation, searched as
//                    MfaAlignment.h searches them. Python is not started.
//   agent_key        ANTHROPIC_API_KEY: present or not, and from where.
//                    Never its value, nor any part of it.
//   archive_backup   CUTMACHINE_ARCHIVE_BACKUP_DIR, the folder the project
//                    archive is copied into once a day (DEC44,
//                    ProjectArchive.h), and that folder there. Optional.
//   voice_over       the local voice-over model (GN6a): its Python, its
//                    script and its models folder, searched as VoiceOver.h
//                    searches them, each model folder short of no file.
//                    Python is not started. Optional.
//
// Nothing here resolves anything its own way: each item asks the function
// its tools ask, so the report cannot say "found" where the tool will then
// say "not found".
//
// Everything that depends on the machine comes in through Machine, so a test
// gives another one and never reads $HOME, the real key or the Keychain.
// `cutmachine --check-setup` (Cli.h) and the window's notice at launch both
// call CheckSetup(CurrentMachine()).
//
// Plain C++: no AppKit.

#include "FfmpegCapabilities.h"
#include "Json.h"
#include "MfaAlignment.h"
#include "VoiceOver.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace setup {

// The stable identifiers of the items, in the order a report lists them.
// The first is ffmpeg::kDiagnosisKey, "ffmpeg", the key PRD2's diagnosis
// already goes by.
inline constexpr const char* kWhisperModelItem = "whisper_model";
inline constexpr const char* kWhisperHesitationsModelItem =
    "whisper_hesitations_model";
inline constexpr const char* kMfaItem = "mfa";
inline constexpr const char* kAgentKeyItem = "agent_key";
inline constexpr const char* kArchiveBackupItem = "archive_backup";
inline constexpr const char* kVoiceOverItem = "voice_over";

// The setting that carries the key of the window's agent panel
// (ChatLlmClient.h).
inline constexpr const char* kAgentKeyVariable = "ANTHROPIC_API_KEY";

enum class State : std::uint8_t {
    // Present and usable.
    kOk,
    // Absent: the tools it serves refuse.
    kMissing,
    // Present, but short of something: an FFmpeg without a filter or an
    // encoder the engine uses, or one that cannot be probed; a model file
    // that is empty or cannot be read; MFA's Python without its aligner or
    // its script.
    kIncomplete,
    // Absent, and nothing the engine requires depends on it.
    kOptionalAbsent,
};

// "ok", "missing", "incomplete", "optional_absent".
const char* StateName(State state);

struct Item {
    std::string id;
    State state = State::kMissing;
    // Needed to edit and export. Only FFmpeg is.
    bool required = false;
    // What cannot be done without it, as a clause that follows "Without
    // it, ".
    std::string prevents;
    // The catalog's tools that refuse without it, when that set is short
    // and exact. FFmpeg lists none: every tool that reads or writes media
    // needs it, which is what `required` says.
    std::vector<std::string> tools;
    // What was found -- paths, version, where they came from -- as an
    // object. Null when nothing was.
    mcp_json::Value found;
    // What it lacks, as an array: FFmpeg's missing filters and encoders,
    // MFA's files found nowhere, the voice-over's files found nowhere and
    // the model folders it lacks. Null when it lacks nothing.
    mcp_json::Value missing;
    // One sentence: what was found, or what is wrong.
    std::string detail;
    // The name of the refusal its tools give while it is not ok
    // (FfmpegNotFound, MfaNotFound...); empty when they have none of their
    // own.
    std::string error;
    // How to repair it, naming the setting and the file. Empty when ok.
    std::string repair;
};

struct Report {
    std::vector<Item> items;

    // No required item is missing: this machine can edit and export. An
    // incomplete FFmpeg -- one without libx265, say -- edits and exports,
    // and its item says what it cannot do.
    bool Ok() const;
    // The first required item that is missing; null when Ok().
    const Item* MissingRequired() const;
    // The item of that id; null when there is none.
    const Item* Find(const std::string& id) const;
};

// Where a machine-local setting was read.
enum class SettingOrigin : std::uint8_t { kUnset, kEnvironment, kSettingsFile };

// "environment" or "settings_file"; "" for kUnset.
const char* SettingOriginName(SettingOrigin origin);

struct Setting {
    // As the engine reads it (LocalEnv.h): the environment, then the
    // settings file. Empty when unset.
    std::string value;
    SettingOrigin origin = SettingOrigin::kUnset;
};

// What is at a path.
struct FileStatus {
    // A regular file, through symbolic links.
    bool regular = false;
    bool readable = false;
    std::uintmax_t size = 0;
};

// Everything the report reads from the machine.
struct Machine {
    // One machine-local setting, and where it came from.
    std::function<Setting(const std::string& name)> setting;
    // What is at a path.
    std::function<FileStatus(const std::string& path)> file;
    // The settings file a repair names (LocalEnv.h).
    std::string settings_file;
    // The binary the engine would run, as FfmpegCapabilities.h diagnoses
    // it. Unset: nothing is found.
    std::function<ffmpeg::Diagnosis()> ffmpeg;
    // MFA's search, with everything it reads. A null `is_file` is `file`
    // above.
    MfaSearch mfa;
    // DEC44 -- whether a folder exists at a path. Unset: none does.
    std::function<bool(const std::string& path)> directory;
    // GN6a -- the voice-over's search, with everything it reads. A null
    // `is_file` is `file` above, a null `is_folder` is `directory`.
    VoiceOverSearch voice_over;
    // AL4 -- whether the file at a path is the evaluated s075 weights, or no
    // value when that was not verified. Unset: never verified.
    std::function<std::optional<bool>(const std::string& path)>
        hesitations_weights;
};

// This process's machine: its environment and settings file, the file
// system, ffmpeg::Diagnose(), CurrentMfaSearch() and CurrentVoiceOverSearch().
Machine CurrentMachine();

// The report for `machine`, its items in the order above. On the current
// machine it probes FFmpeg (three short launches, remembered per binary),
// may read the Keychain once, and reads the hesitations weights once per
// process to verify them (about 10 s for 3 GB, WhisperDecodingProfile.h):
// call it off the main thread.
Report CheckSetup(const Machine& machine);

// {"ok":…,"items":[…]}. When !Ok(), the missing required item's `error` and
// `detail` beside them, as every refusal of the engine carries its own.
mcp_json::Value ReportJson(const Report& report);

}  // namespace setup
