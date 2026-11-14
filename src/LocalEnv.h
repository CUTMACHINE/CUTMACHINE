#pragma once

// Machine-local settings, shared by every surface (ALPHA-2026-08).
//
// Extracted verbatim from ChatLlmClient.cc, which has read the API key this
// way since F2.4. It moved here the moment a second setting needed the same
// treatment: the Whisper model path (Transcription.h). Both are the same kind
// of value -- machine-local, not project truth. A model path must never enter
// a project file, or reopening the montage on another Mac would carry a path
// that does not exist there (PHILOSOPHY.md: the same project must produce the
// same montage everywhere).
//
// Why a dotenv file and not NSUserDefaults, where UiPreferences.h keeps
// interface state: the headless surfaces need this. `--transcribe` and
// `--mcp-serve` are the same binary, but Cli.cc and the model library are
// deliberately AppKit-free, and a value only reachable through Foundation
// would be invisible to them. A file both a human and a CLI can read is the
// only thing that serves the app, the CLI and the agent at once.
//
// The real environment always wins over the file: LoadDotEnv never overwrites
// a variable that is already set, so a one-off `CUTMACHINE_WHISPER_MODEL=…`
// in front of a command overrides the configured value without editing
// anything.

#include <string>

namespace local_env {

// The variable that replaces the settings file's path: tests point it at a
// file of their own, so that none reads or writes the user's.
inline constexpr const char* kEnvFileVariable = "CUTMACHINE_ENV_FILE";

// $CUTMACHINE_ENV_FILE if set, else ~/.config/cutmachine/.env. Missing files
// are not an error: an unconfigured install is a normal state, and every
// caller has to handle an absent value anyway.
void LoadLocalEnvFileIfPresent();

// The path LoadLocalEnvFileIfPresent() reads, for error messages that tell
// the user which file to edit rather than making them guess.
std::string LocalEnvFilePath();

// Loads the file if needed, then reads one variable. Empty when unset.
std::string Value(const char* name);

// PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. What the file itself sets `name`
// to, read without touching the environment; empty when it does not set it.
// Once the file is loaded, a value it supplied cannot be told from one that
// was exported: this is how the setup report (SetupCheck.h) says where a
// setting comes from. The engine reads Value(), never this.
std::string FileValue(const char* name);

// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. The window's menu writes the
// Whisper model where the engine reads it, so the settings file is no longer
// only read. What a write did to this process.
struct WriteOutcome {
    // The file now sets `name` to the value.
    bool written = false;
    // Why not, when it does not.
    std::string error;
    // A value exported to this process, other than the one the file used to
    // give, keeps winning (LoadDotEnv never overwrites): this process goes on
    // reading `environment_value`, and so do the processes it starts.
    bool environment_wins = false;
    std::string environment_value;
};

// Sets `name` to `value` in the settings file, as FileValue reads it back:
// the first line that sets it is replaced, `export ` kept; with none, a line
// is appended. Every other line stays as it was, comments and later
// duplicates included. The file and its folder are created when missing, a
// new file readable by its owner only (it may hold ANTHROPIC_API_KEY); an
// existing one keeps its mode, and a symbolic link is written through, never
// replaced. The new content goes to a temporary file in the same folder,
// then renamed over the old, so a failed write leaves the old file whole.
//
// This process then reads the new value too, unless the environment holds
// one of its own (WriteOutcome::environment_wins). A value with a line break
// or a NUL, and an empty one, are refused: the file could not carry them, or
// would carry "unset".
WriteOutcome WriteFileValue(const char* name, const std::string& value);

}  // namespace local_env
