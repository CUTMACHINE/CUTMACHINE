// S8 -- SAVING_ROADMAP.md. The save path, measured on a fixture shaped like
// the interview package it was first measured on (PRJI: 405 rushes, 11
// timelines, 33 project entries).
//
// PERF-2026-09's numbers were taken on real packages and could not be
// replayed: no reference project, no protocol. This builds the reference
// project in memory, drives the entry points an agent (CLI/MCP) and the
// editor (-commitProjectCandidate:) use, prints what each costs, and holds
// what does not depend on the machine -- the bytes each step writes -- to
// bounds the whole-snapshot journal broke by orders of magnitude.
//
// Timings are printed, and asserted only against a product ceiling as B10
// does: a machine under a sanitizer is not the external drive the editor
// actually saves to.

#include "Cli.h"
#include "EditLog.h"
#include "ProjectFixture.h"
#include "ProjectRecovery.h"
#include "ProjectStorage.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

uintmax_t FileBytes(const std::string& path) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    return error ? 0 : size;
}

std::string Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

struct OpenedProject {
    Project project;
    std::map<std::string, EditLog> timelineLogs;
    ProjectEditLog projectLog;
};

OpenedProject Open(const std::string& projectPath) {
    OpenedProject opened;
    std::string error;
    EditError editError = EditError::None;
    Check(LoadStoredProject(projectPath, opened.project, error),
          "package loads: " + error);
    for (const DocumentSequence& timeline : opened.project.timelines) {
        EditLog log;
        const std::string path =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        if (fs::exists(path))
            Check(EditLog::Load(path, log, editError, error),
                  "timeline journal loads: " + error);
        opened.timelineLogs[timeline.id] = std::move(log);
    }
    Check(ProjectEditLog::Load(ProjectEditLogPathForProject(projectPath),
                               opened.projectLog, editError, error),
          "project journal loads: " + error);
    return opened;
}

}  // namespace

int main() {
    const fs::path directory = fs::temp_directory_path() /
                               ("cutmachine-save-benchmark-" + GenerateUlid());
    fs::create_directories(directory);
    const Project fixture = MakeInterviewShapedProject();
    std::string error;
    std::string projectPath;
    Check(CreatePortableProject(
              (directory / "Interviews.cutmachine-project").string(), fixture,
              projectPath, error),
          "fixture package is created: " + error);
    const uintmax_t projectBytes = FileBytes(projectPath);

    // 1. Thirty-three project operations through the CLI, the way an agent
    // renames and switches timelines.
    std::vector<Ulid> timelineIds;
    for (const DocumentSequence& timeline : fixture.timelines)
        timelineIds.push_back(timeline.id);
    constexpr int kProjectOperations = 33;
    double slowestProjectOperation = 0.0;
    const Clock::time_point projectStart = Clock::now();
    for (int index = 0; index < kProjectOperations; ++index) {
        const Ulid& target = timelineIds[index % timelineIds.size()];
        const ProjectOperation operation =
            index % 3 == 2
                ? ProjectOperation{SetActiveProjectTimelineOperation{target}}
                : ProjectOperation{RenameProjectItemOperation{
                      target, "Version " + std::to_string(index)}};
        const Clock::time_point start = Clock::now();
        std::string output;
        Check(
            ApplyProjectOperationCommand(
                projectPath, SerializeProjectOperation(operation), output) == 0,
            "project operation " + std::to_string(index) + ": " + output);
        slowestProjectOperation =
            std::max(slowestProjectOperation, MillisecondsSince(start));
    }
    const double projectOperationsTotal = MillisecondsSince(projectStart);
    const std::string projectLogPath =
        ProjectEditLogPathForProject(projectPath);
    const std::string journal = Read(projectLogPath);

    // What those entries weighed before S6: both whole projects, hex-encoded.
    Project loaded;
    Check(LoadStoredProject(projectPath, loaded, error),
          "package reloads: " + error);
    const uintmax_t wholeSnapshotJournal =
        static_cast<uintmax_t>(kProjectOperations) * 4 *
        loaded.SaveToString().size();

    // 2. One timeline edit through the CLI -- the MCP backend's path, which
    // since B10 reads and rewrites the project journal on every edit.
    AddMarkerOperation marker;
    marker.marker.name = "bench";
    marker.marker.time = {0, 25};
    const Clock::time_point cliStart = Clock::now();
    std::string output;
    Check(ApplyOperationCommand(projectPath, SerializeOperation(marker),
                                output) == 0,
          "timeline edit through the CLI: " + output);
    const double cliTimelineEdit = MillisecondsSince(cliStart);
    Check(Read(projectLogPath) == journal,
          "a timeline edit leaves the project journal byte-identical");

    // 3. The editor saving one timeline edit: autosave envelope, then the
    // package commit, as -commitProjectCandidate: does.
    const Clock::time_point reopenStart = Clock::now();
    OpenedProject opened = Open(projectPath);
    const double reopen = MillisecondsSince(reopenStart);
    Check(opened.projectLog.AppliedCount() ==
              static_cast<size_t>(kProjectOperations),
          "every project operation is journaled");

    Document document = opened.project.MakeActiveDocument();
    EditLog stagedLog = opened.timelineLogs[opened.project.active_timeline_id];
    EditError editError = EditError::None;
    AddMarkerOperation second;
    second.marker.name = "bench 2";
    second.marker.time = {25, 25};
    Check(stagedLog.Apply(document, second, editError, error),
          "editor edit applies: " + error);
    Project candidate = opened.project;
    Check(candidate.CommitDocument(opened.project.active_timeline_id, document,
                                   error),
          "editor edit commits to the project: " + error);
    std::map<std::string, EditLog> logs = opened.timelineLogs;
    logs[opened.project.active_timeline_id] = stagedLog;

    const Clock::time_point autosaveStart = Clock::now();
    Check(ProjectRecovery::WriteAutosave(projectPath, candidate, logs,
                                         opened.projectLog, error),
          "autosave: " + error);
    const double autosave = MillisecondsSince(autosaveStart);
    const Clock::time_point commitStart = Clock::now();
    Check(CommitStoredProjectAndLogs(projectPath, candidate, logs,
                                     opened.projectLog, error),
          "package commit: " + error);
    const double commit = MillisecondsSince(commitStart);
    Check(ProjectRecovery::DiscardAutosave(projectPath, error),
          "autosave discard: " + error);

    std::printf(
        "S8 save benchmark: %zu rushes, %zu timelines, project file %ju bytes\n"
        "  project operation (CLI)    %8.1f ms average, %8.1f ms slowest\n"
        "  project journal, %d ops   %10ju bytes (whole snapshots: %ju)\n"
        "  timeline edit (CLI/MCP)    %8.1f ms\n"
        "  reopen project + journals  %8.1f ms\n"
        "  editor save of one edit    %8.1f ms (autosave %.1f, commit %.1f)\n",
        fixture.rushes.size(), fixture.timelines.size(), projectBytes,
        projectOperationsTotal / kProjectOperations, slowestProjectOperation,
        kProjectOperations, static_cast<uintmax_t>(journal.size()),
        wholeSnapshotJournal, cliTimelineEdit, reopen, autosave + commit,
        autosave, commit);

    Check(journal.size() < projectBytes,
          "33 project entries weigh less than one copy of the project");
    Check(static_cast<uintmax_t>(journal.size()) * 100 < wholeSnapshotJournal,
          "the project journal is at least a hundred times smaller than "
          "whole snapshots");
    Check(cliTimelineEdit < 10000.0 && autosave + commit < 10000.0,
          "a timeline edit is saved in under ten seconds");

    fs::remove_all(directory);
    std::cout << "Save benchmark passed\n";
    return 0;
}
