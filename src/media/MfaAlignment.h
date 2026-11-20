#pragma once

// AX2 -- docs/roadmap/AX-2026-09.md. The engine runs the aligner itself.
//
// The word boundaries every cut is placed on come from a forced aligner
// (ITW3, WordAlignment.h): Whisper gives the text, the aligner the timing.
// Until AX2 the alignment was a Python script in the monter-une-itw skill
// that an agent ran through its shell: a step outside the engine, known only
// to an agent that had read the skill, and one that wrote the project's
// caches without the session lock the engine holds.
//
// ALIGNEMENT-2026-11 -- docs/roadmap/ALIGNEMENT-2026-11.md. The aligner is
// Montreal Forced Aligner, which replaced NeMo Forced Aligner: against
// C-PROM, 70 minutes of French checked by hand, NeMo's boundaries were 73 ms
// from the reference on average and MFA's 31 ms. MFA stays in its conda
// environment (Kaldi, kalpy): the engine starts scripts/mfa_align.py with
// that environment's Python, under the lock it holds, as a media task the
// caller can follow and cancel, and reads back what the script installed.
// The script runs the environment's `mfa` on the models beside it
// (<environment>/../models). What is missing for it to run is named, file
// by file.
//
// PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. Where those files are looked
// for. Each used to be its variable or else one path: the build tree that
// compiled the binary, baked in by CMake, or outside CMake a path relative
// to the working directory. A binary off its build tree -- a worktree's, or
// one whose build/ was rebuilt elsewhere -- found nothing, and the refusal
// named the one path it had tried.
//
// The order, for each of the three files, first hit wins:
//   1. its variable (CUTMACHINE_MFA_PYTHON, CUTMACHINE_MFA_ALIGNER,
//      CUTMACHINE_MFA_SCRIPT), in the environment, then in the settings
//      file (LocalEnv.h). A value that names no file is a refusal, never a
//      reason to look further: as for CUTMACHINE_FFMPEG, someone who set a
//      path must not get another one without being told.
//   2. beside the executable: <dir>/mfa/env/bin/python,
//      <dir>/mfa/env/bin/mfa and <dir>/../scripts/mfa_align.py, which are a
//      checkout's build/ and scripts/ whatever the working directory. The
//      application's binary lives in <x>.app/Contents/MacOS: the directory
//      holding the bundle is looked in too, and for build/CUTMACHINE.app
//      that is build/.
//   3. where the build that made this binary installs them (CMakeLists.txt,
//      absolute): what a binary copied off its build tree -- the app dragged
//      to /Applications -- still finds.
//   4. the working directory: build/mfa/env/bin/python,
//      build/mfa/env/bin/mfa and scripts/mfa_align.py.
//
// Nothing is downloaded or installed: the self-contained package (PRD3) is
// postponed. A file found nowhere is the refusal MfaNotFound, which names
// every place it was looked for and the variable to set.
//
// GN6a -- docs/roadmap/GENERATIF-2026-10.md. The order and the refusal are
// ToolFiles.h's, shared with the local voice-over (VoiceOver.h); what is
// MFA's here is the three files, their variables and the refusal's name.

#include "MediaTaskManager.h"
#include "ToolFiles.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The machine-local settings that point at each file.
inline constexpr const char* kMfaPythonVariable = "CUTMACHINE_MFA_PYTHON";
inline constexpr const char* kMfaAlignerVariable = "CUTMACHINE_MFA_ALIGNER";
inline constexpr const char* kMfaScriptVariable = "CUTMACHINE_MFA_SCRIPT";

// The stable name surfaces give the refusal (CLI and MCP `error`).
inline constexpr const char* kMfaNotFoundErrorName = "MfaNotFound";

struct MfaAlignmentSettings {
    // The interpreter of MFA's environment, which runs the script.
    std::string python;
    // That environment's `mfa`.
    std::string aligner;
    // scripts/mfa_align.py.
    std::string script;
    // PRD21 -- why the search found no file for one of the three: the
    // sentence of MfaLocation::refusal. Empty when it found all three.
    std::string refusal;

    // LocateMfa(CurrentMfaSearch()): each file where the search above
    // finds it, and the refusal when one is found nowhere.
    static MfaAlignmentSettings FromEnvironment();
};

// One sentence naming every file the alignment needs and cannot find, with
// the variable that points elsewhere; empty when nothing is missing. It
// starts with "MFA not found: " (MfaRefusalName).
std::string MissingMfaInstallation(const MfaAlignmentSettings& settings);

// PRD21 -- where one file was found: its variable, beside the executable
// (or the bundle that holds it), where the build installs it, or relative
// to the working directory.
using MfaOrigin = tool_files::Origin;

// The variable's name for kConfigured (as ffmpeg::OriginName names
// CUTMACHINE_FFMPEG), else "executable", "build" or "working_directory".
const char* MfaOriginName(MfaOrigin origin, const char* variable);

// Everything the search reads, so its order can be tested without touching
// the process.
struct MfaSearch {
    // The three variables' values; empty when unset.
    std::string python_variable;
    std::string aligner_variable;
    std::string script_variable;
    // The directory of the running executable; empty when unknown.
    std::string executable_directory;
    // Where the build installs the three (step 3); an empty one is skipped.
    std::string build_python;
    std::string build_aligner;
    std::string build_script;
    // The working directory: steps 4, and a relative variable, are read
    // against it. Empty: they stay relative.
    std::string working_directory;
    // The settings file a refusal tells the user to edit.
    std::string settings_file;
    // Whether a file is at a path. Null: the file system (a regular file,
    // through symbolic links).
    std::function<bool(const std::string&)> is_file;
};

// Where the file is (empty when found nowhere), its origin, and every path
// looked at in the order of the search.
using MfaFile = tool_files::File;

struct MfaLocation {
    MfaFile python;
    MfaFile aligner;
    MfaFile script;
    // "MFA not found: " and, for each file found nowhere, where it was
    // looked for, then the variables to set and where. Empty when all three
    // were found.
    std::string refusal;

    bool Found() const { return refusal.empty(); }
    // The three paths, and the refusal.
    MfaAlignmentSettings Settings() const;
};

// The order above, applied to `search`: no environment.
MfaLocation LocateMfa(const MfaSearch& search);

// What LocateMfa reads for this process: the three variables (the
// environment, then the settings file), the executable's directory, the
// build's own paths and the working directory.
MfaSearch CurrentMfaSearch();

// kMfaNotFoundErrorName when `message` holds a refusal from LocateMfa,
// else empty: what lets a surface name the refusal, as ffmpeg::RefusalName
// does FFmpeg's, without every function in between learning a second
// channel.
std::string MfaRefusalName(const std::string& message);

// Runs the script on `mediaIds` of the project at `projectPath` (a resolved
// project.cutmachine.json). `workDirectory` holds the 16 kHz WAVs and MFA's
// corpus, output and working files: keep it off exFAT. `textsPath`, when not
// empty, is a JSON object {media id: text} replacing a rush's Whisper text.
// The script writes its summary to `reportPath` (aligned, skipped,
// uncovered_speech, mfa_ms).
//
// Cancelled through `context`: the script and everything it started (FFmpeg,
// MFA and its workers) are stopped together. On failure `error` carries the end
// of what the script printed on its error output.
bool RunMfaAlignment(const MfaAlignmentSettings& settings,
                     const std::string& projectPath,
                     const std::vector<std::string>& mediaIds,
                     const std::string& textsPath,
                     const std::string& workDirectory,
                     const std::string& reportPath, MediaTaskContext& context,
                     std::string& error);
