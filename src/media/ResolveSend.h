#pragma once

// PA16 -- docs/roadmap/PARITE-2026-11.md. Send the montage to DaVinci
// Resolve in one gesture.
//
// Until now a montage reached Resolve through two routes that each carried
// half of it (ResolveExport.h, ResolveXmlExport.h), and through a shell: the
// agent exported a descriptor, then ran sidecar/resolve_bridge.py itself.
// This is the gesture whole: the descriptor of the selected timeline
// (ResolveExport.h, v3), written where it can be read again, handed to the
// bridge, and the bridge's report -- what was built, what was read back,
// what did not travel -- returned as the answer.
//
// The engine does not speak to Resolve itself: Resolve's scripting module
// is a Python library loaded from Resolve's own bundle, and external
// scripting is a Studio feature. The bridge stays the one place that knows
// Resolve's API; the engine knows the montage.
//
// A Resolve with a dialog open answers nothing and never returns (measured
// 29 September and 6 November 2026), so the bridge runs under a time limit:
// past it the bridge is stopped and the answer says Resolve stopped
// answering. The timeline it had started stays in Resolve, named.

#include "Document.h"
#include "ResolveExport.h"

#include <string>

// The environment variables that point at the bridge's Python and at the
// bridge itself, read like every other tool path (LocalEnv.h).
inline constexpr const char* kResolvePythonVariable =
    "CUTMACHINE_RESOLVE_PYTHON";
inline constexpr const char* kResolveBridgeVariable =
    "CUTMACHINE_RESOLVE_BRIDGE";
// Where the descriptors are written; unset: the engine's data folder,
// resolve-send/. The test environment points it into the build tree.
inline constexpr const char* kResolveSendDirectoryVariable =
    "CUTMACHINE_RESOLVE_SEND_DIR";

struct ResolveBridgeSettings {
    std::string python;
    std::string bridge;
    // Why the bridge was found nowhere; empty when it was.
    std::string refusal;
};

// The variables first, then beside the executable, the build's source tree,
// the working directory. Python: its variable, else /usr/bin/python3, else
// `python3` on the PATH -- the bridge needs nothing but the standard library
// and Resolve's own module.
ResolveBridgeSettings LocateResolveBridge();

struct ResolveSendRequest {
    ResolveSendOptions options;
    // The timeline's name in Resolve; empty: the sequence's own.
    std::string name;
    // Write the descriptor and stop: nothing is sent.
    bool dry_run = false;
    // Where the descriptor is written; empty: CUTMACHINE_RESOLVE_SEND_DIR,
    // else the engine's data folder, resolve-send/.
    std::string directory;
    int timeout_seconds = 600;
};

// Builds the descriptor of `document` (its media paths made absolute
// against `packageDirectory`), writes it, runs the bridge unless dry_run,
// and answers the report as JSON: the timeline and project it landed in,
// what was set, `verified` and the `mismatches` of the read back, and
// `not_sent`, the descriptor's losses. Error names: InvalidExport (the
// montage cannot be described), IoError, ResolveBridgeMissing,
// ResolveSendFailed (the bridge refused, its sentence in the message),
// ResolveTimeout.
bool SendTimelineToResolve(Document document,
                           const std::string& packageDirectory,
                           const ResolveSendRequest& request,
                           const ResolveBridgeSettings& settings,
                           std::string& resultJson, std::string& errorName,
                           std::string& error);

// PA16 -- what the window says of a send, in French: the status line, and
// when there is more to read (what did not travel, a difference read back,
// a refusal) the title and text of the dialog that shows it. Built from
// SendTimelineToResolve's answer, or from its error. Portable, so that
// main.mm only wires it.
struct ResolveSendNotice {
    std::string status;
    // Empty when the status line says it all.
    std::string title;
    std::string text;
    bool failed = false;
};
ResolveSendNotice ResolveSendNoticeFor(bool sent, const std::string& resultJson,
                                       const std::string& errorName,
                                       const std::string& error);

// --send-resolve: the same from a stored project.
int SendResolveCommand(const std::string& projectPath,
                       const std::string& timelineId,
                       const ResolveSendRequest& request, std::string& output);
