#pragma once

// PRD1 -- docs/roadmap/PRODUIT-2026-10.md. Which FFmpeg the engine runs, and
// the one way it runs it.
//
// Until this ticket every settings struct defaulted its `ffmpeg_path` to the
// bare name "ffmpeg", which posix_spawnp looked up on PATH at each launch.
// That fails silently where it matters: an app opened from the Finder gets
// launchd's PATH, where FFmpeg is not, and the only thing said was "unable
// to start FFmpeg: No such file or directory". It also meant two launches of
// one export could find two different binaries.
//
// The order, first hit wins:
//   1. CUTMACHINE_FFMPEG -- the real environment, then the machine-local
//      settings file (LocalEnv.h). A value that does not name an executable
//      is a refusal, never a reason to look further: someone who configured
//      a binary must not get another one without being told.
//   2. An `ffmpeg` beside the running executable, for a package that ships
//      its own (PRD3).
//   3. The directories of PATH.
//
// The answer is an absolute path, kept for the life of the process. It is
// remembered against the two values that decide it (CUTMACHINE_FFMPEG and
// PATH) rather than unconditionally: several suites, and the window's own
// smoke run, put FFmpeg's directory on PATH after the process has started,
// and a memo that ignored its inputs would hand them the answer from before.
// In a process that leaves its environment alone that is one search.
//
// A caller that names a binary itself -- every test passes the one CMake
// found -- keeps it: an explicit path is used as given and never resolved.
// An empty `ffmpeg_path` is what means "the resolved binary", and
// ffmpeg::Run is where that is settled, so no caller can forget to.
//
// Plain C++ and POSIX: no AppKit.

#include "ProcessLauncher.h"

#include <string>

namespace ffmpeg {

// The machine-local setting that names the binary.
inline constexpr const char* kBinaryVariable = "CUTMACHINE_FFMPEG";

// The stable name surfaces give the refusal below (CLI and MCP `error`).
inline constexpr const char* kNotFoundErrorName = "FfmpegNotFound";

enum class Origin {
    kConfigured,  // CUTMACHINE_FFMPEG
    kBundled,     // beside the executable
    kSearchPath,  // PATH
};

// "CUTMACHINE_FFMPEG", "bundled" or "PATH".
const char* OriginName(Origin origin);

// Everything the resolution reads, so the order can be tested without
// touching the process's environment.
struct Search {
    // CUTMACHINE_FFMPEG; empty when unset.
    std::string configured;
    // The directory of the running executable; empty when unknown.
    std::string executable_directory;
    // PATH, colon-separated.
    std::string search_path;
    // The settings file a refusal tells the user to edit.
    std::string settings_file;

    bool operator==(const Search& other) const {
        return configured == other.configured &&
               executable_directory == other.executable_directory &&
               search_path == other.search_path &&
               settings_file == other.settings_file;
    }
};

struct Location {
    // Absolute. Empty when nothing was found.
    std::string path;
    Origin origin = Origin::kSearchPath;
    // Why nothing was found: where it looked and which variable to set.
    // Starts with "FFmpeg not found: ".
    std::string refusal;

    bool Found() const { return !path.empty(); }
};

// The order above, applied to `search`. No memo, no environment.
Location Locate(const Search& search);

// What Locate reads for this process: its environment, the settings file
// and its own executable's directory.
Search CurrentSearch();

// The directory of the running executable, through symbolic links; empty
// when it cannot be known. Looked up once. PRD21 -- MFA is looked for
// beside it too (MfaAlignment.h), and the setup report says what it found
// there (SetupCheck.h).
std::string ExecutableDirectory();

// Locate(CurrentSearch()), searched once and remembered while the two
// variables keep their values. A failure is not remembered.
Location Resolved();

// The binary a launch runs: `requested` as given when the caller named one,
// else the resolved one. False with the named refusal when there is none.
bool Executable(const std::string& requested, std::string& path,
                std::string& error);

// process::Run for FFmpeg. `request.arguments[0]` is the binary the caller
// chose, or empty for the resolved one. With no binary to run, nothing is
// started and the result is Stage::kRefused, its `refusal` the sentence of
// Location::refusal. PRD2 -- the same when the binary lacks a filter or an
// encoder the arguments use (FfmpegCapabilities.h).
process::Result Run(process::Request request);

// The sentence for a launch that did not start: the refusal, "unable to
// create <pipes>: <errno>" or "unable to start FFmpeg: <errno>". `pipes`
// is the caller's own name for what it could not create, as each wrote it
// before there was one launcher.
std::string StartError(const process::Result& result, const std::string& pipes);

// PRD40 -- ROADMAP.md. A media FFmpeg is about to be handed that is not
// there: a package copied without the folder of its rushes, a disk not
// mounted. FFmpeg could only fail, and the caller learned "FFmpeg could not
// render the requested frame" while the server's own error output named the
// file. Each function that hands FFmpeg a rush checks it first; the name
// below is what every surface then gives the refusal, path included.
inline constexpr const char* kInputMissingErrorName = "MediaMissing";

// True when `path` names a regular file. Otherwise false, with
// "source media is missing: <path>" in `error` -- the export's sentence,
// which it has always given.
bool InputPresent(const std::string& path, std::string& error);

// The stable error name a message carries, or empty: kNotFoundErrorName
// when `message` holds a refusal from Locate, PRD2's
// FfmpegCapabilityMissing when it holds one from Require, and PRD40's
// MediaMissing when it holds one from InputPresent. Engine functions report
// failures as sentences; this is what lets a surface name these without
// every function in between learning a second channel.
std::string RefusalName(const std::string& message);

}  // namespace ffmpeg
