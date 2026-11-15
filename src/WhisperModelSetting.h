#pragma once

// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. One Whisper model setting:
// the engine's.
//
// The window's transcription menus kept a model of their own, in a
// preference (CUTMACHINEWhisperModelPath), while the CLI, the MCP server and
// the agent panel read CUTMACHINE_WHISPER_MODEL (Transcription.h,
// LocalEnv.h). Two settings for one file: a model chosen in the window was
// unknown to the agents, the setup report had to say which reader used which
// (SetupNotice.h), and a new Mac had to be told twice. Decided by the author on
// 23 October 2026: the engine's setting is the only one. The window reads it as
// every tool does, « CUTMACHINE ▸ Modèle Whisper… » writes it in the
// settings file, and the preference serves once more, to migrate.
//
// What the window does is decided here; main.mm only shows the panel, reads
// and removes the preference, and puts the status line on screen.
//
// Plain C++: no AppKit. tests/whisper_model_setting_tests.cc, which points
// CUTMACHINE_ENV_FILE at a file of its own: no test reads or writes the
// user's settings.

#include <cstdint>
#include <optional>
#include <string>

class MediaTaskContext;

namespace whisper_model_setting {

// What « Modèle Whisper… » did with the file it was given.
struct Choice {
    // The path, as chosen.
    std::string path;
    // Written in the settings file as CUTMACHINE_WHISPER_MODEL.
    bool saved = false;
    // Why not, in the engine's words, when it was not.
    std::string error;
    // The environment carries another model, which this process and the
    // tools it starts keep reading (LocalEnv.h).
    bool environment_wins = false;
    std::string environment_value;
};

// Writes `path` as the engine's model (local_env::WriteFileValue). The
// caller has the path from a file panel: whether a model is there is the
// transcription's question, as for a path written by hand.
Choice SaveChosenModel(const std::string& path);

// The window's line once the choice is made, in French:
// « Modèle Whisper · ggml-large-v3.bin, pour la fenêtre et les agents »;
// the variable that wins when the environment has one; the error when the
// file could not be written, the model then serving this transcription only.
std::string ChoiceStatus(const Choice& choice);

enum class MigrationOutcome : std::uint8_t {
    // No preference: nothing to migrate.
    kNothing,
    // The settings file did not set the model: the preference's path is
    // now CUTMACHINE_WHISPER_MODEL there.
    kMigrated,
    // The settings file sets a model already: it stays, it is the engine's.
    // The preference's path is dropped, the same one or not.
    kEngineSettingKept,
    // The file could not be written: the preference stays, for the next
    // launch to try again.
    kFailed,
};

struct Migration {
    MigrationOutcome outcome = MigrationOutcome::kNothing;
    // The caller removes the preference: true for every outcome but kFailed.
    bool forget_preference = true;
    std::string error;
};

// The window's old preference, its value read by the caller ("" when it has
// none), migrated once. Only the settings file decides whether the engine
// already has a model: a variable exported to this process is a one-off and
// is not written anywhere. A path whose file is gone is migrated as it is:
// it was chosen, and the menus ask for another when the tools refuse it.
Migration MigrateWindowPreference(const std::string& preference);

// "nothing", "migrated", "engine_setting_kept", "failed".
const char* MigrationOutcomeName(MigrationOutcome outcome);

// AL4 -- docs/roadmap/ALIGNEMENT-2026-11.md. « Transcrire les hésitations
// (français)… » asks for the euh: it names fr-hesitations-v1 and runs on the
// hesitations weights (CUTMACHINE_WHISPER_HESITATIONS_MODEL), never on a
// silent `default`. Without them it refuses, and says why and what to set.
enum class HesitationsProblem : std::uint8_t {
    // The evaluated weights: transcribe.
    kNone,
    // A readable file this process has not read yet: the task reads it
    // (HesitationsWeightsReady), the main thread does not.
    kUnverified,
    // The setting is not set.
    kUnset,
    // It names no readable, non-empty file.
    kUnreadable,
    // It names other weights than the evaluated ones.
    kOtherWeights,
};

// What the window knows of the hesitations weights without reading them.
struct HesitationsFacts {
    // The setting's value; empty when unset.
    std::string path;
    // A regular, readable, non-empty file there.
    bool readable = false;
    // The evaluated weights or not, when this process already knows it
    // (KnownWhisperDecodingModel).
    std::optional<bool> evaluated;
    // The settings file the refusal names (LocalEnv.h); empty: the
    // environment only.
    std::string settings_file;
};

// This process's facts: the setting, the file there, what is known of it.
HesitationsFacts CurrentHesitationsFacts();

HesitationsProblem HesitationsWeightsProblem(const HesitationsFacts& facts);

// The alert of a refusal, in French: its title, then why and what to set.
struct HesitationsRefusal {
    std::string title;
    std::string detail;
};
HesitationsRefusal RefuseHesitations(HesitationsProblem problem,
                                     const HesitationsFacts& facts);

// The same decision in the task, where the weights can be read: false, with
// the refusal's detail as `error`, unless `facts.path` holds the evaluated
// weights. A cancelled read says so instead.
bool HesitationsWeightsReady(const HesitationsFacts& facts,
                             MediaTaskContext& context, std::string& error);

}  // namespace whisper_model_setting
