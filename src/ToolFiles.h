#pragma once

// GN6a -- docs/roadmap/GENERATIF-2026-10.md. Where the files of a program
// the engine starts but does not ship are looked for.
//
// MFA (MfaAlignment.h) was the first such program: a Python environment and
// a script, found by its variables, beside the executable, where the build
// installs them, or from the working directory (PRD21). The local voice-over
// (VoiceOver.h) needs the same search for its own environment, script and
// models. The order, the refusal that names every place looked at, and the
// rule that a variable naming nothing is a refusal rather than a reason to
// look further, are written once here; each program names its files and
// keeps its own refusal prefix and error name.
//
// The order, for each file, first hit wins:
//   1. its variable, as the caller read it (LocalEnv.h: the environment,
//      then the settings file). A relative value is read against the
//      working directory. A value that names nothing is a refusal.
//   2. beside the executable, and beside the bundle that holds it for the
//      binary of an application (<x>.app/Contents/MacOS).
//   3. where the build that made the binary installs it (absolute; an empty
//      one is skipped).
//   4. relative to the working directory.
// A place met twice is looked at once.
//
// Plain C++: no AppKit. The search reads the file system only through the
// predicates of Context, so a test gives its own.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tool_files {

// Where one file was found.
enum class Origin : std::uint8_t {
    kConfigured,        // its variable
    kExecutable,        // beside the executable, or the bundle that holds it
    kBuild,             // where the build that made the binary installs it
    kWorkingDirectory,  // relative to the working directory
};

// The variable's name for kConfigured, else "executable", "build" or
// "working_directory".
const char* OriginName(Origin origin, const char* variable);

// What is looked for: a regular file, or a folder.
enum class Kind : std::uint8_t { kFile, kFolder };

// One file of a program.
struct Role {
    // Its name in a sentence: "MFA's Python", "scripts/mfa_align.py".
    const char* what;
    // The variable that points at it.
    const char* variable;
    // Where it sits from a directory that stands for the executable's.
    const char* beside_build;
    // Where it sits from the working directory.
    const char* from_working_directory;
    Kind kind = Kind::kFile;
};

// Everything the search reads besides the variable and the build's path.
struct Context {
    // The directory of the running executable; empty when unknown.
    std::string executable_directory;
    // The working directory; empty: relative paths stay relative.
    std::string working_directory;
    // Whether a regular file (through symbolic links) is at a path. Null:
    // the file system.
    std::function<bool(const std::string&)> is_file;
    // Whether a folder is at a path. Null: the file system.
    std::function<bool(const std::string&)> is_folder;
};

struct File {
    // Where the file is. Empty when it was found nowhere.
    std::string path;
    Origin origin = Origin::kConfigured;
    // Every path looked at, in the order of the search.
    std::vector<std::string> searched;

    bool Found() const { return !path.empty(); }
};

// One file, by the order above. `problem` says why it was found nowhere --
// "<variable> names '<value>', which is not a file", or "<what> is at none
// of '<a>', '<b>'" -- and stays empty when it was found.
File Locate(const Role& role, const std::string& configured,
            const std::string& build, const Context& context,
            std::string& problem);

// `prefix`, the problems joined by "; ", then ". Set <variable> to <what>,
// ..., in the environment" and " or in '<settings file>'" when there is one.
std::string Refusal(const std::string& prefix,
                    const std::vector<std::string>& problems,
                    const std::vector<const Role*>& roles,
                    const std::string& settingsFile);

// For paths a caller filled itself: the refusal naming each one that is not
// there ("<what> is not at '<path>'"), empty when all are.
std::string CheckPaths(
    const std::string& prefix,
    const std::vector<std::pair<std::string, const Role*>>& paths,
    const std::string& settingsFile);

// "A", "A and B", "A, B and C".
std::string Enumeration(const std::vector<std::string>& items);

}  // namespace tool_files
