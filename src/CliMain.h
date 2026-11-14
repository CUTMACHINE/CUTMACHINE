#pragma once

// HEADLESS-2026-10 H1 -- docs/roadmap/HEADLESS-2026-10.md. The command line
// of every cutmachine executable, read in one place.
//
// main.mm read the commands, then opened the editor when none matched. The
// build without a window (HeadlessMain.cc) began as a copy of that reading,
// and the copy differed from it the day it was written: no --token for an
// HTTP server, no relay to the window that holds the project, a codec no
// encoder takes accepted, an option given twice taken. Both executables now
// hand their command line here and supply only what is theirs (CliHost):
// a command added to the table (CliCommands.h) is dispatched by both.
//
// Portable: nothing here reaches AppKit, Core Text or Vision.

#include <functional>
#include <optional>
#include <string>
#include <vector>

// What an executable supplies that the engine's portable code cannot. An
// empty member is a capability the executable does not have.
struct CliHost {
    // Called before the first command that may draw or look at a picture
    // (MotionFrameRenderer.h, CaptionRenderer.h, FaceFinder.h): Core Text,
    // Core Graphics and Vision in the macOS executable. Empty: the tools
    // that need one find none installed and refuse by name.
    std::function<void()> install_renderers;
    // The local preference for strict timeline mode (UiPreferences.h,
    // requiresExplicitTimelineForAgentEdits), read by --call-tool,
    // --prepare-edit-plan, --apply-edit-plan and --mcp-serve. Empty: off;
    // `--mcp-serve --require-timeline` and open_project's require_timeline
    // still turn it on.
    std::function<bool()> require_explicit_timeline;
    // The motion commands (CliSection::kMotion, MotionCli.h), which draw
    // with Core Text. Empty: each is refused as UnsupportedOperation.
    std::function<int(const std::string& command,
                      const std::vector<std::string>& arguments,
                      std::string& output)>
        motion_command;
};

// Runs the command `argv` names, writes its answer to stdout and returns its
// exit status. Returns nothing when the command line names no command --
// nothing at all, or one project path: the editor opens its window on it,
// and an executable without a window refuses. argv[2] may be rewritten (a
// package folder stands for its project file) and a --token value is
// overwritten where it sits (PRD13).
std::optional<int> RunCliCommand(int argc, char* argv[], const CliHost& host);
