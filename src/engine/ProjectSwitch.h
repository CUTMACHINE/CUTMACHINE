#pragma once

// DEC39 -- docs/roadmap/PHASES-2026-08.md. Changing project without going
// back to the welcome window: « Fichier › Ouvrir un projet… » and
// « Fichier › Nouveau projet… » in the editor, as Resolve's Project Manager
// changes project from inside the program.
//
// The editor holds one project for as long as its window is open: its
// lock, its journals, its decoders, the agents it relays (AX1) and its
// Agent panel all belong to that project, and nothing in the window was
// built to be torn down and built again while it runs. So the window does
// not swap its project in place. It checks the other project here, writes
// what it holds, and starts a new editor on the other project as it quits,
// once its own lock is released (AppDelegate's -applicationWillTerminate:).
// The person sees the window close and open again on the other project,
// without the welcome window in between.
//
// What is decided here, portable and tested: which project file a choice
// names, whether it is the one already open, whether it can be opened at
// all -- readable, and not held by another program -- and the command line
// that opens it. Checking first matters: the new editor, given a path it
// cannot open, says so and quits, which would leave nothing open.

#include <cstddef>
#include <string>
#include <vector>

namespace project_switch {

enum class Outcome {
    // The choice names the project the window holds: nothing to do.
    kAlreadyOpen,
    // Readable and free: the editor reopens on `project_path`.
    kOpen,
    // Not openable; `refusal` says why, in French, for the window.
    kRefused,
};

struct Plan {
    Outcome outcome = Outcome::kRefused;
    // The project file, a package folder resolved to the file it holds.
    std::string project_path;
    std::string refusal;
};

// The project file that `chosen` names: a package folder stands for the
// project.cutmachine.json it holds, as the welcome window's « Ouvrir un
// projet… » reads it (AppDelegate -resolvedProjectPath:); anything else is
// kept, normalized. main() reads its argument through it too: given the
// folder as is, the editor looked for manifest.json beside the package,
// said « Impossible d'ouvrir le projet » and quit (24 October 2026), when
// `cutmachine --help` promises the folder opens.
std::string ProjectFileOf(const std::string& chosen);

// The plan for opening `chosen` -- a project file or its package folder --
// from a window that holds `currentProjectPath`. Reads the project, and
// takes then gives back its session lock, to know that the new editor will
// open it; leaves nothing behind.
Plan PlanProjectSwitch(const std::string& currentProjectPath,
                       const std::string& chosen);

// Why the window cannot change project now, in French, or "" when it can.
// Quitting cancels an export and the window's background tasks (imports,
// analyses, proxies): a change of project asked for in passing must not
// throw them away, so it waits for them, or for the person to cancel them
// in the task bar.
std::string BusyRefusal(bool exportRunning, size_t runningTasks);

// The command line that opens the editor on `projectPath`: the executable,
// then the project, which is how main() reads a project to open.
std::vector<std::string> EditorArgumentsFor(const std::string& executable,
                                            const std::string& projectPath);

}  // namespace project_switch
