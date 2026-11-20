#pragma once

// PRD20 -- ROADMAP.md. The commands of the command line, described once.
//
// main() reads its arguments as a chain of `argc == N && argv[1] == "--x"`
// tests with a single refusal at its end, so every mistake got the same
// sentence -- "invalid command line; expected a project path or a supported
// --command" -- whether the command did not exist, lacked an argument or had
// one too many. It named neither the command nor what was missing, and
// `--help` could only point at the README.
//
// This table is what main() knows: each command's name and how it is called.
// The text of `--help`, the help of one command and every refusal of a
// command line are built from it, so a refusal always names the command and
// gives its usage. A command main() dispatches without an entry here is
// refused as unknown before it runs: the table cannot fall behind unseen.
//
// Plain C++: nothing here knows a project, AppKit or FFmpeg.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// The groups `--help` lists the commands under, in this order.
enum class CliSection {
    kAgents,
    kProject,
    kAnalyses,
    kColor,
    kEdits,
    kMulticam,
    kMotion,
    kDelivery,
    kConnectors,
    kMachine,
};

struct CliCommand {
    // As typed, with its two dashes: "--describe".
    const char* name;
    // What follows the name, on one line; empty for a command that takes
    // nothing. Its grammar is read by the functions below, so it is kept
    // regular: tokens separated by single spaces, required positional
    // arguments first as <name> (or '<name>' for one the shell must quote),
    // options and optional arguments after them in [brackets] or
    // (parentheses), "..." after what repeats.
    const char* arguments;
    // What the command does, in a few words.
    const char* summary;
    CliSection section;
};

// Every command main() dispatches, grouped by section.
const std::vector<CliCommand>& CliCommands();

// The command of that name, typed with or without its dashes ("--describe",
// "describe"); `-h` is `--help`. Null when there is none.
const CliCommand* FindCliCommand(const std::string& name);

// "usage: cutmachine --describe <project> [--timeline <id>]".
std::string CliUsageLine(const CliCommand& command);

// The required positional arguments of a command, as its usage names them:
// {"<project>", "<file>"} for --export.
std::vector<std::string> CliRequiredArguments(const CliCommand& command);

// The command a mistyped name most likely meant: the same name once dashes,
// underscores and case are put right, or the only command within two edits
// of it. Empty when there is no such command, or more than one.
std::string SuggestCliCommand(const std::string& name);

// "unknown command '--frobnicate'", the suggestion when there is one, and
// where the list is.
std::string UnknownCliCommandDetail(const std::string& name);

// Why `arguments` (what follows the name) cannot be this command's, in
// words that name the argument at fault when the usage is enough to tell:
// a required argument that is missing, an option the command does not have,
// an option left without its value, an option given twice that the usage
// does not let repeat (PRD39), an argument too many. Never empty: the
// caller has already established that the arguments do not fit.
std::string CliArgumentsMismatch(const CliCommand& command,
                                 const std::vector<std::string>& arguments);

// PRD39 -- ROADMAP.md. Why a command that reads its own options refused the
// one at `arguments[index]`, `arguments` being what follows its name: an
// option its usage does not have ("unknown --export option '--frob'"), one
// left without the value its usage gives it ("... needs a value"), one
// given again when its usage does not end its group with "..." ("... is
// given twice"), or else a value it does not take; a word that is no
// option there is an argument too many. The commands used to call all of
// these "unknown". Never empty.
std::string CliOptionRefusal(const CliCommand& command,
                             const std::vector<std::string>& arguments,
                             size_t index);

// The text of `cutmachine --help`.
std::string CliHelpText();
// The text of `cutmachine --help <command>`.
std::string CliCommandHelpText(const CliCommand& command);

// What a command line is, decided before anything runs.
struct CliPreflight {
    enum class Action {
        // Go on: the editor, or `command` with its arguments.
        kRun,
        // `text` goes to standard output and the process exits with 0.
        kPrint,
        // The command line is refused: `text` is the detail of a
        // ValidationFailed, exit status 2.
        kRefuse,
    };
    Action action = Action::kRun;
    // The command named, when the first argument is one. Null for the
    // editor (no argument, or a project path).
    const CliCommand* command = nullptr;
    std::string text;
};

// `arguments` is argv without the program name. Refuses only what the table
// alone proves wrong: a command that does not exist, one short of a
// required argument, one that takes a fixed number of arguments and got
// more, and a project path followed by anything. Options and their values
// are left to the command, which knows them.
//
// A lone argument without dashes is a project path, and the editor opens
// on it. `exists`, when given, says whether anything is at a path: a
// command typed without its dashes ("version", "help") where nothing bears
// that name is then refused as the command it meant, instead of opening a
// window on a project that is not there. Without `exists` nothing is looked
// up and such a word stays a path.
CliPreflight PreflightCommandLine(
    const std::vector<std::string>& arguments,
    const std::function<bool(const std::string&)>& exists = nullptr);
