// HEADLESS-2026-10 -- docs/roadmap/HEADLESS-2026-10.md. The engine's
// command line and MCP server without AppKit, for a host that has no
// window: a Linux container, a CI runner, an agent's sandbox.
//
// H1 -- the commands are read by RunCliCommand (CliMain.h), the reading
// main.mm hands its command line to as well: this file began as a copy of
// it, and the copy had drifted by the day it was written. What main.mm
// supplies on top (CliHost) does not exist here: no renderer (Core Text,
// Core Graphics, Vision), so the tools that draw text or find faces refuse
// by name (MotionFrameRenderer.h, CaptionRenderer.h, FaceFinder.h); no
// local preferences, so strict timeline mode is the one --mcp-serve
// --require-timeline or open_project's require_timeline asks for.
#include "CliMain.h"
#include "CommandEnvelope.h"

#include <cstdio>
#include <optional>
#include <string>

int main(int argc, char* argv[]) {
    if (const std::optional<int> status =
            RunCliCommand(argc, argv, CliHost{})) {
        return *status;
    }
    // No command: the editor would open its window, on the project named if
    // one was.
    std::string output;
    const int status = FailCliCommand(
        "ValidationFailed",
        std::string(argc < 2 ? "this build has no window"
                             : "this build has no window to open '" +
                                   std::string(argv[1]) + "' in") +
            ": give a command (cutmachine --help lists them)",
        output, 2);
    std::fwrite(output.data(), 1, output.size(), stdout);
    return status;
}
