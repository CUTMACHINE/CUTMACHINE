// DEC24 -- SAVING_ROADMAP.md. A project journal written before S6 is
// migrated when the project opens, under the session lock.
//
// tests/fixtures/project_log/ holds such a journal, frozen: two applied
// entries and one undone, each carrying two whole projects, hex-encoded, in
// the version 2 format projects had then (ProjectVersionTwo.h). Beside it,
// the four states the entries move between, as this build writes a project
// (state-2 is the one the package stores). `--print-fixture <directory>`
// wrote them once; the bytes change only on purpose.
//
// The journal is put in a package this test makes, then migrated: written
// back in the delta form, a fraction of its size, every other file of the
// package byte for byte, and every undo and redo landing on the same states
// as before. Then what must not write: a second opening, a journal already
// compact, one whose change no delta expresses, a lock held by another
// process. And the cheap path an MCP session takes. `--measure` times an
// opening before and after on a package shaped like PRJI, by hand.
//
// Plain C++ and POSIX: no AppKit, no media.

#include "Cli.h"
#include "EditLog.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectFixture.h"
#include "ProjectStorage.h"
#include "ProjectVersionTwo.h"
#include "Ulid.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

#ifndef PROJECT_LOG_FIXTURES_DIR
#error "PROJECT_LOG_FIXTURES_DIR names tests/fixtures/project_log"
#endif

namespace {

namespace fs = std::filesystem;

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void WriteAll(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

const fs::path kFixtures = PROJECT_LOG_FIXTURES_DIR;
constexpr const char* kJournalFixture = "pre-s6.project-editlog.json";

std::string StateFile(int index) {
    return "state-" + std::to_string(index) + ".json";
}

// ---------------------------------------------------------------------
// The fixture, as it was written once.
// ---------------------------------------------------------------------

// An entry as a build before S6 wrote it: the operation carries the whole
// project after it, the inverse the whole project before, each in the
// version 2 format.
template <typename Operation>
std::string LegacyEntry(Operation operation, Operation inverse,
                        const Project& before, const Project& after) {
    operation.exact_project_result =
        ExactProjectState{project_version_two::VersionTwo(after), std::nullopt};
    inverse.exact_project_result = ExactProjectState{
        project_version_two::VersionTwo(before), std::nullopt};
    return "{\"op\":" + SerializeProjectOperation(operation) +
           ",\"inverse\":" + SerializeProjectOperation(inverse) + "}";
}

int PrintFixture(const fs::path& directory) {
    Project state = MakeInterviewShapedProject(3, 2, 2);
    state.name = "DEC24, un journal d'avant S6";
    state.active_timeline_id = state.timelines[0].id;
    const Ulid first = state.timelines[0].id;
    const Ulid second = state.timelines[1].id;
    const std::string firstName = state.timelines[0].name;
    const std::string secondName = state.timelines[1].name;

    // The states come from the operations themselves.
    std::vector<Project> states{state};
    ProjectEditLog log;
    EditError error = EditError::None;
    std::string message;
    const std::vector<ProjectOperation> operations = {
        RenameProjectItemOperation{second, "Montage court"},
        SetActiveProjectTimelineOperation{second},
        RenameProjectItemOperation{first, "Sélects"}};
    for (const ProjectOperation& operation : operations) {
        if (!log.Apply(state, operation, error, message)) {
            std::cerr << "cannot build the fixture: " << message << '\n';
            return 1;
        }
        states.push_back(state);
    }

    const std::string journal =
        "{\"version\":1,\"kind\":\"project\",\"applied\":[" +
        LegacyEntry(RenameProjectItemOperation{second, "Montage court"},
                    RenameProjectItemOperation{second, secondName}, states[0],
                    states[1]) +
        "," +
        LegacyEntry(SetActiveProjectTimelineOperation{second},
                    SetActiveProjectTimelineOperation{first}, states[1],
                    states[2]) +
        "],\"undone\":[" +
        LegacyEntry(RenameProjectItemOperation{first, "Sélects"},
                    RenameProjectItemOperation{first, firstName}, states[2],
                    states[3]) +
        "]}\n";
    fs::create_directories(directory);
    WriteAll(directory / kJournalFixture, journal);
    for (int index = 0; index < 4; ++index) {
        WriteAll(directory / StateFile(index), states[index].SaveToString());
    }
    std::cout << "fixture written to " << directory << '\n';
    return 0;
}

// ---------------------------------------------------------------------
// The package the journal is put in.
// ---------------------------------------------------------------------

struct Package {
    std::string project_path;
    fs::path directory;
    fs::path journal;
};

std::optional<Package> MakePackage(const fs::path& root,
                                   const std::string& name,
                                   const std::string& journal) {
    Project stored;
    std::string error;
    if (!Project::LoadFromString(ReadAll(kFixtures / StateFile(2)), stored,
                                 error)) {
        Check(false, "the stored state reads: " + error);
        return std::nullopt;
    }
    Package package;
    package.directory = root / name;
    if (!CreatePortableProject(package.directory.string(), stored,
                               package.project_path, error)) {
        Check(false, "the package is created: " + error);
        return std::nullopt;
    }
    package.journal = ProjectEditLogPathForProject(package.project_path);
    WriteAll(package.journal, journal);
    return package;
}

// Every file of the package folder and beside it, but the journal.
std::map<std::string, std::string> FilesBesideJournal(const Package& package) {
    std::map<std::string, std::string> files;
    const fs::path parent = package.directory.parent_path();
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(parent)) {
        if (entry.is_regular_file() && entry.path() != package.journal &&
            entry.path().string().rfind(package.directory.string(), 0) == 0) {
            files[entry.path().string()] = ReadAll(entry.path());
        }
    }
    return files;
}

bool LeftoversBeside(const Package& package) {
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(package.directory)) {
        const std::string name = entry.path().filename().string();
        if (Contains(name, ".tmp") || Contains(name, ".bak") ||
            Contains(name, ".lock")) {
            return true;
        }
    }
    return fs::exists(package.project_path + ".lock");
}

std::string Hostname() {
    std::array<char, 256> host{};
    gethostname(host.data(), host.size() - 1);
    return host.data();
}

// ---------------------------------------------------------------------
// The tests.
// ---------------------------------------------------------------------

void TestFixture(const std::string& journal) {
    Test("the fixture is a journal written before S6", [&] {
        Check(Contains(journal, "\"exact_project_hex\":\"") &&
                  !Contains(journal, "exact_project_delta"),
              "its entries carry whole projects, and no delta");
        // The version 2 format is in the hex: "project_version":2.
        Check(Contains(journal, "2270726f6a6563745f76657273696f6e223a32"),
              "and the projects in it are version 2 files");
        Check(journal.size() > 20000, "a few entries, tens of kilobytes: " +
                                          std::to_string(journal.size()));
    });
}

void TestMigration(const fs::path& root, const std::string& journal) {
    const std::optional<Package> made = MakePackage(root, "Film", journal);
    if (!made) return;
    const Package& package = *made;
    const std::map<std::string, std::string> before =
        FilesBesideJournal(package);

    ProjectEditLog log;
    ProjectEditLogMigration migration;
    std::string error;
    Test("opening migrates it, written back as deltas", [&] {
        Check(LoadProjectEditLogMigrating(package.project_path, log, migration,
                                          error),
              "the journal loads: " + error);
        Check(migration.found && migration.compacted == 3 &&
                  migration.rewritten && migration.rewrite_error.empty(),
              "three entries compacted, the journal rewritten: " +
                  std::to_string(migration.compacted) + " " +
                  migration.rewrite_error);
        Check(log.AppliedCount() == 2 && log.UndoneCount() == 1,
              "the same entries, applied and undone");
        const std::string written = ReadAll(package.journal);
        std::cout << "journal: " << journal.size() << " -> " << written.size()
                  << " bytes\n";
        Check(written == log.Serialize(), "the file is what was read");
        Check(!Contains(written, "\"exact_project_hex\":\"") &&
                  Contains(written, "exact_project_delta"),
              "every entry is a delta now");
        Check(migration.bytes_before == journal.size() &&
                  migration.bytes_after == written.size() &&
                  written.size() * 10 < journal.size(),
              "a fraction of its size: " + std::to_string(written.size()) +
                  " of " + std::to_string(journal.size()) + " bytes");
    });

    Test("nothing else of the package moves, and no lock stays", [&] {
        Check(FilesBesideJournal(package) == before,
              "the project, the timelines and their journals keep their "
              "bytes");
        Check(!LeftoversBeside(package),
              "no transaction leftover, and the lock taken for the write is "
              "released");
    });

    Test("every undo and redo lands where it did", [&] {
        Project project;
        Check(LoadStoredProject(package.project_path, project, error, false),
              "the project opens: " + error);
        ProjectEditLog reread;
        EditError editError = EditError::None;
        Check(ProjectEditLog::Load(package.journal.string(), reread, editError,
                                   error) &&
                  reread.CompactedOnRead() == 0,
              "the migrated journal has nothing left to convert");
        const auto at = [&](int index) {
            return project.SaveToString() ==
                   ReadAll(kFixtures / StateFile(index));
        };
        Check(at(2), "the package stores state 2");
        Check(reread.Undo(project, editError, error) && at(1),
              "undo: state 1 " + error);
        Check(reread.Undo(project, editError, error) && at(0),
              "undo: state 0 " + error);
        Check(reread.Redo(project, editError, error) && at(1),
              "redo: state 1 " + error);
        Check(reread.Redo(project, editError, error) && at(2),
              "redo: state 2 " + error);
        Check(reread.Redo(project, editError, error) && at(3),
              "redo of the undone entry: state 3 " + error);
    });

    Test("a second opening reads and writes nothing more", [&] {
        const std::string migrated = ReadAll(package.journal);
        const auto writtenAt = fs::last_write_time(package.journal);
        ProjectEditLog again;
        ProjectEditLogMigration second;
        Check(LoadProjectEditLogMigrating(package.project_path, again, second,
                                          error) &&
                  second.found && second.compacted == 0 && !second.rewritten,
              "nothing to compact: " + error);
        Check(ReadAll(package.journal) == migrated &&
                  fs::last_write_time(package.journal) == writtenAt,
              "the journal is not written again");
        ProjectEditLogMigration cheap;
        Check(MigrateProjectEditLog(package.project_path, cheap, error) &&
                  cheap.found && cheap.compacted == 0 && !cheap.rewritten,
              "nor by an MCP session's opening");
    });
}

void TestWithoutWriting(const fs::path& root, const std::string& journal) {
    std::string error;

    Test("no journal is an empty one", [&] {
        const std::optional<Package> package =
            MakePackage(root, "Sans journal", journal);
        if (!package) return;
        fs::remove(package->journal);
        ProjectEditLog log;
        ProjectEditLogMigration migration;
        Check(LoadProjectEditLogMigrating(package->project_path, log, migration,
                                          error) &&
                  !migration.found && log.AppliedCount() == 0 &&
                  !fs::exists(package->journal),
              "loaded empty, nothing created: " + error);
    });

    Test("a journal that does not read is refused, as before", [&] {
        const std::optional<Package> package =
            MakePackage(root, "Illisible", "{\"version\":1,\"kind\":");
        if (!package) return;
        ProjectEditLog log;
        ProjectEditLogMigration migration;
        Check(!LoadProjectEditLogMigrating(package->project_path, log,
                                           migration, error) &&
                  !error.empty() &&
                  ReadAll(package->journal) == "{\"version\":1,\"kind\":",
              "refused, untouched: " + error);
    });

    Test("a change no delta expresses keeps its whole projects", [&] {
        // Two timelines swapped: a delta cannot say a reordering, so S6
        // keeps such an entry whole, and DEC24 has nothing to write.
        Project before;
        Check(Project::LoadFromString(ReadAll(kFixtures / StateFile(2)), before,
                                      error),
              "state 2 reads: " + error);
        Project after = before;
        std::swap(after.timelines[0], after.timelines[1]);
        const std::string reordered =
            "{\"version\":1,\"kind\":\"project\",\"applied\":[" +
            LegacyEntry(
                SetActiveProjectTimelineOperation{before.active_timeline_id},
                SetActiveProjectTimelineOperation{before.active_timeline_id},
                before, after) +
            "],\"undone\":[]}\n";
        const std::optional<Package> package =
            MakePackage(root, "Réordonné", reordered);
        if (!package) return;
        ProjectEditLog log;
        ProjectEditLogMigration migration;
        Check(LoadProjectEditLogMigrating(package->project_path, log, migration,
                                          error) &&
                  migration.compacted == 0 && !migration.rewritten &&
                  ReadAll(package->journal) == reordered,
              "read, not rewritten: " + error);
    });

    Test("a lock held by another process: read, not written", [&] {
        const std::optional<Package> package =
            MakePackage(root, "Tenu ailleurs", journal);
        if (!package) return;
        // The parent of this test -- CTest, a shell -- is alive, on this
        // host, and is not this process.
        const fs::path lock = package->project_path + ".lock";
        fs::create_directory(lock);
        WriteAll(lock / "owner", "01K50000000000000000000024\n" + Hostname() +
                                     "\n" + std::to_string(getppid()) +
                                     "\nqa\n");
        ProjectEditLog log;
        ProjectEditLogMigration migration;
        Check(LoadProjectEditLogMigrating(package->project_path, log, migration,
                                          error),
              "the journal still loads: " + error);
        Check(migration.compacted == 3 && !migration.rewritten &&
                  Contains(migration.rewrite_error, "session lock") &&
                  log.AppliedCount() == 2,
              "compacted in memory, not written: " + migration.rewrite_error);
        Check(
            ReadAll(package->journal) == journal && fs::exists(lock / "owner"),
            "the journal and the other process's lock are as they were");
        fs::remove_all(lock);
    });

    Test("under this process's own lock, as the editor opens", [&] {
        const std::optional<Package> package =
            MakePackage(root, "Editeur", journal);
        if (!package) return;
        ProjectSessionLock held;
        std::string owner;
        Check(held.Acquire(package->project_path, owner, error),
              "the window's lock: " + error);
        ProjectEditLog log;
        ProjectEditLogMigration migration;
        Check(LoadProjectEditLogMigrating(package->project_path, log, migration,
                                          error) &&
                  migration.rewritten,
              "migrated under it: " + error + migration.rewrite_error);
        Check(held.Held() && fs::exists(package->project_path + ".lock"),
              "and the lock is still the window's");
        held.Release();
    });

    Test("an MCP session's opening migrates a journal before S6", [&] {
        const std::optional<Package> package =
            MakePackage(root, "Session", journal);
        if (!package) return;
        ProjectEditLogMigration migration;
        Check(MigrateProjectEditLog(package->project_path, migration, error) &&
                  migration.compacted == 3 && migration.rewritten &&
                  !Contains(ReadAll(package->journal),
                            "\"exact_project_hex\":\""),
              "migrated: " + error + migration.rewrite_error);
    });
}

// `--measure <directory> [rushes timelines entries]`: what an opening
// costs before and after, on a package shaped like PRJI (405 rushes,
// timelines, a journal of changes of active timeline written before S6).
// Not run by CTest: it builds hundreds of megabytes, and a duration only
// says something on the machine and the disk it was taken on.
int Measure(const fs::path& directory, size_t rushes, size_t timelines,
            size_t entries) {
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - start)
            .count();
    };
    Project state = MakeInterviewShapedProject(rushes, timelines, 24);
    std::string journal = "{\"version\":1,\"kind\":\"project\",\"applied\":[";
    for (size_t index = 0; index < entries; ++index) {
        const Project before = state;
        const Ulid previous = state.active_timeline_id;
        const Ulid next = state.timelines[(index + 1) % timelines].id;
        state.active_timeline_id = next;
        journal += (index == 0 ? "" : ",") +
                   LegacyEntry(SetActiveProjectTimelineOperation{next},
                               SetActiveProjectTimelineOperation{previous},
                               before, state);
    }
    journal += "],\"undone\":[]}\n";
    std::string error;
    Package package;
    package.directory = directory / "Mesure";
    if (!CreatePortableProject(package.directory.string(), state,
                               package.project_path, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    package.journal = ProjectEditLogPathForProject(package.project_path);
    WriteAll(package.journal, journal);
    std::cout << "journal before S6: " << journal.size() << " bytes, "
              << entries << " entries\n";
    journal.clear();
    journal.shrink_to_fit();

    for (int pass = 0; pass < 2; ++pass) {
        ProjectEditLog log;
        EditError editError = EditError::None;
        const auto start = Clock::now();
        ProjectEditLog::Load(package.journal.string(), log, editError, error);
        std::cout << "pass " << pass << ", every opening before DEC24 ("
                  << log.CompactedOnRead()
                  << " entries converted): " << since(start) << " ms\n";
    }
    ProjectEditLog log;
    ProjectEditLogMigration migration;
    auto start = Clock::now();
    LoadProjectEditLogMigrating(package.project_path, log, migration, error);
    std::cout << "the opening that migrates: " << since(start) << " ms, "
              << migration.bytes_before << " -> " << migration.bytes_after
              << " bytes" << (migration.rewritten ? "" : ", NOT rewritten")
              << migration.rewrite_error << "\n";
    for (int pass = 0; pass < 3; ++pass) {
        ProjectEditLog again;
        ProjectEditLogMigration next;
        start = Clock::now();
        LoadProjectEditLogMigrating(package.project_path, again, next, error);
        std::cout << "an opening after it: " << since(start) << " ms\n";
    }
    start = Clock::now();
    ProjectEditLogMigration cheap;
    MigrateProjectEditLog(package.project_path, cheap, error);
    std::cout << "an MCP session's opening after it: " << since(start)
              << " ms\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--print-fixture") {
        return PrintFixture(argv[2]);
    }
    if ((argc == 3 || argc == 6) && std::string(argv[1]) == "--measure") {
        const auto number = [&](int index, size_t fallback) {
            return argc == 6 ? std::stoul(argv[index]) : fallback;
        };
        return Measure(argv[2], number(3, 405), number(4, 11), number(5, 33));
    }
    try {
        const fs::path root = fs::canonical(fs::temp_directory_path()) /
                              (GenerateUlid() + "-project-log-migration");
        fs::create_directories(root);
        const std::string journal = ReadAll(kFixtures / kJournalFixture);
        TestFixture(journal);
        TestMigration(root, journal);
        TestWithoutWriting(root, journal);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All project log migration tests passed\n";
    return 0;
}
