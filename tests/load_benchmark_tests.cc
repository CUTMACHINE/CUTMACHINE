// PERF8 -- ROADMAP.md. The load side of the reference S8 built for saving:
// opening a project, and the tool calls of an MCP server, on the same
// interview-shaped fixture (tests/ProjectFixture.h: 405 rushes, 11
// timelines).
//
// PERF-2026-09 removed repeated work, not slow work, so what this holds is
// the work each step does -- package parses, media probes, media hashes,
// package files committed -- as counted by src/WorkCounters.h. A count is
// the same on a laptop, under a sanitizer and on the external
// drive projects are cut from; a duration is not. Durations are printed
// (median and maximum of the calls) and held only under the ten-second
// product ceiling S8 uses.
//
// Three ways to run it:
//   (no argument)           what ctest runs: builds the fixture under
//                           $TMPDIR, measures every step, asserts the
//                           bounds, removes the fixture.
//   --create-fixture <dir>  writes the fixture package into <dir> and prints
//                           its project path, so that it can be measured
//                           cold, after `sudo purge`, on the external volume.
//   --project <path>        measures the read-only steps once on an existing
//                           package: the fixture above, or a real project.
//                           Nothing is edited; the session lock is taken
//                           and released, as the editor does.

#include "Cli.h"
#include "EditLog.h"
#include "McpProjectBackend.h"
#include "McpSession.h"
#include "McpTools.h"
#include "ProjectFixture.h"
#include "ProjectRecovery.h"
#include "ProjectStorage.h"
#include "WorkCounters.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <limits.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using work_counters::Tally;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// One benchmarked step: the duration of each call, and the most work any
// single call did. The maximum, not the total, is what a bound holds: "an
// edit parses the package at most twice" must stay true of every edit.
struct Step {
    std::string name;
    std::vector<double> milliseconds;
    Tally most;
};

// `setup`, when given, runs before each call and is neither timed nor
// counted.
Step Measure(const std::string& name, int calls,
             const std::function<void(int)>& call,
             const std::function<void(int)>& setup = {}) {
    Step step{name, {}, {}};
    for (int index = 0; index < calls; ++index) {
        if (setup) {
            setup(index);
        }
        const Tally before = work_counters::Read();
        const Clock::time_point start = Clock::now();
        call(index);
        step.milliseconds.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start)
                .count());
        const Tally spent = work_counters::Since(before);
        step.most.package_loads =
            std::max(step.most.package_loads, spent.package_loads);
        step.most.media_probes =
            std::max(step.most.media_probes, spent.media_probes);
        step.most.media_hashes =
            std::max(step.most.media_hashes, spent.media_hashes);
        step.most.artifact_commits =
            std::max(step.most.artifact_commits, spent.artifact_commits);
    }
    return step;
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle]
                                  : (values[middle - 1] + values[middle]) / 2.0;
}

double Maximum(const std::vector<double>& values) {
    return values.empty() ? 0.0
                          : *std::max_element(values.begin(), values.end());
}

// scripts/perf_bench.sh reads this table by its columns (the step, then the
// median); keep the header and the step names stable.
void Print(const std::vector<Step>& steps) {
    std::printf("%-26s %5s %10s %10s %6s %6s %6s %7s\n", "step", "calls",
                "median_ms", "max_ms", "loads", "probes", "hashes", "commits");
    for (const Step& step : steps) {
        std::printf("%-26s %5zu %10.1f %10.1f %6ju %6ju %6ju %7ju\n",
                    step.name.c_str(), step.milliseconds.size(),
                    Median(step.milliseconds), Maximum(step.milliseconds),
                    static_cast<uintmax_t>(step.most.package_loads),
                    static_cast<uintmax_t>(step.most.media_probes),
                    static_cast<uintmax_t>(step.most.media_hashes),
                    static_cast<uintmax_t>(step.most.artifact_commits));
    }
}

// The portable half of -[AppDelegate loadDocumentAndSourcesHoldingLock]
// (src/app/main.mm), in the order it runs them, stopping where the editor turns
// to media and AppKit. Autosave recovery is not taken: the fixture has no
// autosave, and a real project with one would stop the editor on a dialog.
//
// What comes after this point in main.mm is not measured here: the
// per-source metadata decision PERF1 made (a rush the library describes is
// not probed), audio setup, and the decoders PERF2 opens behind the window.
// cutmachine_ui_smoke_tests guards those, on the real editor.
void OpenLikeTheEditor(const std::string& projectPath) {
    std::string error;
    ProjectSessionLock lock;
    std::string owner;
    Check(lock.Acquire(projectPath, owner, error),
          "the session lock is free: " + error);
    Check(RecoverTextArtifactTransaction(
              fs::path(projectPath).parent_path().string(), error),
          "no unrecoverable transaction: " + error);
    const ProjectRecoveryInfo recovery = ProjectRecovery::Inspect(projectPath);
    (void)recovery;
    CollectionIntegrityReport integrity;
    Check(VerifyPortableProject(projectPath, integrity, error),
          "the package verifies: " + error);
    Project project;
    Check(LoadStoredProject(projectPath, project, error),
          "the package loads: " + error);
    const Document document = project.MakeDocument(project.active_timeline_id);
    (void)document;
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : project.timelines) {
        const std::string logPath =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        if (!fs::exists(logPath)) {
            continue;
        }
        EditLog log;
        Check(EditLog::Load(logPath, log, editError, error),
              "a timeline journal loads: " + error);
    }
    const std::string projectLogPath =
        ProjectEditLogPathForProject(projectPath);
    if (fs::exists(projectLogPath)) {
        ProjectEditLog projectLog;
        Check(
            ProjectEditLog::Load(projectLogPath, projectLog, editError, error),
            "the project journal loads: " + error);
    }
}

mcp_json::Value Arguments(
    std::initializer_list<std::pair<std::string, mcp_json::Value>> members) {
    mcp_json::Value object = mcp_json::Value::MakeObject();
    for (const auto& member : members) {
        object.Set(member.first, member.second);
    }
    return object;
}

mcp_json::Value Time(int64_t value) {
    return Arguments({{"value", mcp_json::Value::MakeInt(value)},
                      {"rate", mcp_json::Value::MakeInt(25)}});
}

// An MCP session the way --mcp-serve runs one: McpSession in front of the
// catalog, over the project backend. McpSession stamps every answer with the
// project revision, which is where the reload after an edit happens.
struct McpServerSession {
    McpProjectBackend backend;
    McpToolRegistry registry;
    McpSession session;

    void Call(const std::string& tool, const mcp_json::Value& arguments) {
        const McpToolCallOutcome outcome =
            session.Call(registry, backend, tool, arguments);
        Check(outcome.ok, tool + " succeeds: " + outcome.error_name + " " +
                              outcome.message);
    }
};

// Journals with something in them, so that opening reads real entries: the
// timeline and project histories an afternoon of editing leaves behind.
void SeedJournals(const std::string& projectPath, const Project& fixture) {
    std::string output;
    for (int index = 0; index < 12; ++index) {
        AddMarkerOperation marker;
        marker.marker.name = "Repère " + std::to_string(index);
        marker.marker.time = {int64_t{25} * (index + 1), 25};
        Check(
            ApplyOperationCommand(projectPath, SerializeOperation(marker),
                                  output, fixture.timelines[index % 3].id) == 0,
            "seeded timeline edit: " + output);
    }
    for (int index = 0; index < 4; ++index) {
        const ProjectOperation rename{RenameProjectItemOperation{
            fixture.timelines[index].id, "Version " + std::to_string(index)}};
        Check(ApplyProjectOperationCommand(
                  projectPath, SerializeProjectOperation(rename), output) == 0,
              "seeded project edit: " + output);
    }
}

std::string CreateFixture(const fs::path& directory, const std::string& name,
                          Project& fixture) {
    fixture = MakeInterviewShapedProject();
    std::string error;
    std::string projectPath;
    Check(CreatePortableProject((directory / name).string(), fixture,
                                projectPath, error),
          "fixture package is created: " + error);
    SeedJournals(projectPath, fixture);
    return projectPath;
}

Step MeasureEditorOpen(const std::string& projectPath, int passes) {
    return Measure("editor_open_portable", passes,
                   [&](int) { OpenLikeTheEditor(projectPath); });
}

Step MeasureCliEdits(const std::string& name, const std::string& projectPath,
                     int calls, int64_t firstFrame) {
    return Measure(name, calls, [&](int index) {
        AddMarkerOperation marker;
        marker.marker.name = name + " " + std::to_string(index);
        marker.marker.time = {firstFrame + int64_t{25} * index, 25};
        std::string output;
        Check(ApplyOperationCommand(projectPath, SerializeOperation(marker),
                                    output) == 0,
              "CLI timeline edit: " + output);
    });
}

// The editor saving one timeline edit, the way S8 drives
// -commitProjectCandidate: -- the package commit only, with the edited
// timeline's journal as the partial write set (B10). Reading the project
// back for each call is setup, neither timed nor counted.
Step MeasureEditorCommits(const std::string& name,
                          const std::string& projectPath, int calls,
                          int64_t firstFrame) {
    Project candidate;
    std::map<std::string, EditLog> logs;
    ProjectEditLog projectLog;
    return Measure(
        name, calls,
        [&](int) {
            std::string error;
            Check(CommitStoredProjectAndLogs(projectPath, candidate, logs,
                                             projectLog, error),
                  "editor commit: " + error);
        },
        [&](int index) {
            std::string error;
            EditError editError = EditError::None;
            Project project;
            Check(LoadStoredProject(projectPath, project, error),
                  "the project reloads: " + error);
            const Ulid timeline = project.active_timeline_id;
            EditLog log;
            Check(EditLog::Load(
                      TimelineEditLogPathForProject(projectPath, timeline), log,
                      editError, error),
                  "the timeline journal loads: " + error);
            projectLog = ProjectEditLog{};
            Check(
                ProjectEditLog::Load(ProjectEditLogPathForProject(projectPath),
                                     projectLog, editError, error),
                "the project journal loads: " + error);
            Document document = project.MakeDocument(timeline);
            AddMarkerOperation marker;
            marker.marker.name = name + " " + std::to_string(index);
            marker.marker.time = {firstFrame + int64_t{25} * index, 25};
            Check(log.Apply(document, marker, editError, error),
                  "the editor edit applies: " + error);
            Check(project.CommitDocument(timeline, document, error),
                  "the editor edit stages: " + error);
            candidate = std::move(project);
            logs = {{timeline, std::move(log)}};
        });
}

// The MCP calls that change nothing, shared by the ctest run and --project.
void MeasureMcpReads(const std::string& projectPath, int readCalls,
                     McpServerSession& mcp, std::vector<Step>& steps) {
    steps.push_back(Measure("mcp_open_project", 1, [&](int) {
        mcp.Call(
            "open_project",
            Arguments({{"path", mcp_json::Value::MakeString(projectPath)}}));
    }));
    steps.push_back(Measure("mcp_read_timeline_stats", readCalls, [&](int) {
        mcp.Call("timeline_stats", mcp_json::Value::MakeObject());
    }));
    steps.push_back(Measure("mcp_read_describe", 3, [&](int) {
        mcp.Call("describe", mcp_json::Value::MakeObject());
    }));
}

int MeasureExistingProject(const std::string& projectPath) {
    Project project;
    std::string error;
    Check(LoadStoredProject(projectPath, project, error),
          "the project loads: " + error);
    std::printf("PERF8 load benchmark: %zu rushes, %zu timelines, %s\n",
                project.rushes.size(), project.timelines.size(),
                projectPath.c_str());
    std::vector<Step> steps{MeasureEditorOpen(projectPath, 1)};
    {
        McpServerSession mcp;
        MeasureMcpReads(projectPath, 5, mcp, steps);
    }
    Print(steps);
    return 0;
}

// The per-user temporary directory macOS keeps on the system volume,
// whatever TMPDIR says. HEADLESS-2026-10 H2 -- elsewhere, /tmp: there is no
// such directory to ask for.
fs::path InternalTemporaryDirectory() {
#if defined(__APPLE__)
    char buffer[PATH_MAX];
    const size_t length =
        confstr(_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof(buffer));
    return length > 0 && length <= sizeof(buffer) ? fs::path(buffer)
                                                  : fs::path("/tmp");
#else
    return fs::path("/tmp");
#endif
}

const Step& Find(const std::vector<Step>& steps, const std::string& name) {
    const auto found =
        std::find_if(steps.begin(), steps.end(),
                     [&](const Step& step) { return step.name == name; });
    Check(found != steps.end(), "step " + name + " was measured");
    return *found;
}

// The work each step may do. Every bound is what the step does today, so
// that any added parse, probe or rewrite fails here with the step's name.
// When a change removes work, lower the bound in the same commit.
void Bound(const Step& step, uint64_t loads, uint64_t commits,
           const std::string& why) {
    const Tally& most = step.most;
    Check(most.package_loads <= loads, step.name + " parses the package " +
                                           std::to_string(most.package_loads) +
                                           " times, bound " +
                                           std::to_string(loads) + ": " + why);
    Check(most.media_probes == 0 && most.media_hashes == 0,
          step.name +
              " reads media: the fixture's library describes every "
              "rush and no rush is collected into the package, so "
              "nothing here has a reason to open one (PERF1)");
    Check(most.artifact_commits <= commits,
          step.name + " commits " + std::to_string(most.artifact_commits) +
              " files, bound " + std::to_string(commits) + ": " + why);
    Check(Median(step.milliseconds) < 10000.0,
          step.name + " stays under the ten-second product ceiling");
}

void HoldBounds(const std::vector<Step>& steps) {
    Bound(Find(steps, "editor_open_portable"), 2, 0,
          "VerifyPortableProject parses the package to list the rushes the "
          "manifest might fingerprint, then the editor parses it again");
    Bound(Find(steps, "cli_edit_add_marker"), 1, 3,
          "one parse; the project file, the edited timeline and its journal "
          "are rewritten, never the other ten timelines (PERF6)");
    Bound(Find(steps, "editor_commit"), 0, 3,
          "the editor commits what it holds; same three files (PERF6)");
    Bound(Find(steps, "mcp_open_project"), 1, 0,
          "opening parses once, and that parse is the session's cache");
    Bound(Find(steps, "mcp_read_timeline_stats"), 0, 0,
          "a read answers from the session's cache (PERF5)");
    Bound(Find(steps, "mcp_read_describe"), 0, 0,
          "describe answers from the session's cache like every other read "
          "(PERF5)");
    Bound(Find(steps, "mcp_edit_add_marker"), 0, 3,
          "the edit starts from the session's cache and leaves the committed "
          "project in it (PERF5); the three files of the edit");
    Bound(Find(steps, "mcp_read_after_edit"), 0, 0,
          "the edit's revision stamp already refilled the cache");
    Bound(Find(steps, "mcp_project_edit"), 0, 3,
          "a project edit starts from the session's cache and leaves the "
          "committed project in it (PERF5; it parsed twice: the command, "
          "then the revision stamp); the project file, the manifest and the "
          "project journal");
    const uint64_t editFiles = 3;
    Bound(Find(steps, "mcp_undo"), 0, editFiles,
          "undo goes through the cache like an edit (PERF5; it parsed "
          "twice); the files of an edit");
    Bound(Find(steps, "mcp_redo"), 0, editFiles,
          "redo goes through the cache like an edit (PERF5; it parsed "
          "twice); the files of an edit");
    Bound(Find(steps, "mcp_read_after_undo"), 0, 0,
          "the undo left the project it committed in the cache");
    Bound(Find(steps, "mcp_analysis"), 0, 0,
          "an analysis reads the session's cache and, committing nothing, "
          "leaves it (PERF5; it parsed twice: the command, then the "
          "revision stamp after the cache was dropped)");
    Bound(Find(steps, "mcp_read_after_analysis"), 0, 0,
          "the analysis left the cache as it found it");
    Bound(Find(steps, "cli_edit_after_agent"), 1, 3,
          "an agent's edits change nothing for the command line: one "
          "parse, the same three files");
    Bound(Find(steps, "editor_commit_after_agent"), 0, 3,
          "nor for the editor: the same three files, without parsing the "
          "package");
}

}  // namespace

int main(int argc, char** argv) try {
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    if (arguments.size() == 2 && arguments[0] == "--create-fixture") {
        Project fixture;
        const std::string projectPath =
            CreateFixture(arguments[1], "PERF8.cutmachine-project", fixture);
        std::printf("%s\n", projectPath.c_str());
        return 0;
    }
    if (arguments.size() == 2 && arguments[0] == "--project") {
        return MeasureExistingProject(fs::absolute(arguments[1]).string());
    }
    if (!arguments.empty()) {
        std::cerr << "usage: cutmachine_load_benchmark_tests "
                     "[--create-fixture <dir> | --project <path>]\n";
        return 2;
    }

    const fs::path directory = fs::temp_directory_path() /
                               ("cutmachine-load-benchmark-" + GenerateUlid());
    fs::create_directories(directory);
    Project fixture;
    const std::string projectPath =
        CreateFixture(directory, "PERF8.cutmachine-project", fixture);

    constexpr int kOpenPasses = 5;
    constexpr int kCalls = 10;
    std::vector<Step> steps{MeasureEditorOpen(projectPath, kOpenPasses)};
    // Before any agent has edited the project: the plain CLI path, and the
    // editor's.
    steps.push_back(
        MeasureCliEdits("cli_edit_add_marker", projectPath, kCalls, 3000));
    steps.push_back(
        MeasureEditorCommits("editor_commit", projectPath, kCalls, 5000));
    {
        McpServerSession mcp;
        MeasureMcpReads(projectPath, kCalls, mcp, steps);
        steps.push_back(Measure("mcp_edit_add_marker", kCalls, [&](int index) {
            mcp.Call("add_marker",
                     Arguments({{"name", mcp_json::Value::MakeString(
                                             "MCP " + std::to_string(index))},
                                {"time", Time(1000 + int64_t{25} * index)}}));
        }));
        // A read straight after an edit: the edit dropped the backend's
        // cache, and whoever reloads it pays here or in the edit itself.
        steps.push_back(Measure(
            "mcp_read_after_edit", kCalls,
            [&](int) {
                mcp.Call("timeline_stats", mcp_json::Value::MakeObject());
            },
            [&](int index) {
                mcp.Call(
                    "add_marker",
                    Arguments({{"name", mcp_json::Value::MakeString(
                                            "Suivi " + std::to_string(index))},
                               {"time", Time(2000 + int64_t{25} * index)}}));
            }));
        // PERF5 -- the rest of what a session does under the lock: a project
        // edit, undo and redo start from the cache and leave the committed
        // project in it; an analysis reads the cache and leaves it.
        steps.push_back(Measure("mcp_project_edit", kCalls, [&](int index) {
            mcp.Call(
                "rename_timeline",
                Arguments({{"timeline_id", mcp_json::Value::MakeString(
                                               fixture.active_timeline_id)},
                           {"name", mcp_json::Value::MakeString(
                                        "Montage " + std::to_string(index))}}));
        }));
        steps.push_back(Measure("mcp_undo", kCalls, [&](int) {
            mcp.Call("undo", mcp_json::Value::MakeObject());
        }));
        steps.push_back(Measure("mcp_redo", kCalls, [&](int) {
            mcp.Call("redo", mcp_json::Value::MakeObject());
        }));
        steps.push_back(Measure(
            "mcp_read_after_undo", kCalls,
            [&](int) {
                mcp.Call("timeline_stats", mcp_json::Value::MakeObject());
            },
            [&](int) { mcp.Call("undo", mcp_json::Value::MakeObject()); }));
        steps.push_back(Measure("mcp_analysis", kCalls, [&](int) {
            mcp.Call("align_transcript", mcp_json::Value::MakeObject());
        }));
        steps.push_back(Measure("mcp_read_after_analysis", kCalls, [&](int) {
            mcp.Call("timeline_stats", mcp_json::Value::MakeObject());
        }));
    }
    // The same CLI edit once an agent has edited the project through MCP.
    steps.push_back(
        MeasureCliEdits("cli_edit_after_agent", projectPath, kCalls, 4000));
    steps.push_back(MeasureEditorCommits("editor_commit_after_agent",
                                         projectPath, kCalls, 6000));

    std::printf(
        "PERF8 load benchmark: %zu rushes, %zu timelines, %d open passes, "
        "%d calls per MCP/CLI step\n",
        fixture.rushes.size(), fixture.timelines.size(), kOpenPasses, kCalls);
    Print(steps);
    HoldBounds(steps);

    fs::remove_all(directory);
    std::cout << "Load benchmark passed\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
}
