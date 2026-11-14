// DB2 -- ROADMAP.md. File publication and SQL delivery are separate commits:
// neither a refusal nor a restart may lose a saved generation or duplicate it.

#include "Cli.h"
#include "EditLog.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "ProjectArchive.h"
#include "ProjectStorage.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using mcp_json::Value;
int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void Write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string Hex(const std::string& input) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    for (unsigned char byte : input) {
        output.push_back(kDigits[byte >> 4]);
        output.push_back(kDigits[byte & 15]);
    }
    return output;
}

std::string PendingName(const Ulid& id) {
    return ".cutmachine-archive-" + id + ".json";
}

class ArchiveEnvironment {
public:
    ArchiveEnvironment() {
        const char* before = std::getenv("CUTMACHINE_PROJECT_ARCHIVE");
        had_value_ = before != nullptr;
        if (before != nullptr) {
            before_ = before;
        }
    }
    ~ArchiveEnvironment() {
        if (had_value_) {
            setenv("CUTMACHINE_PROJECT_ARCHIVE", before_.c_str(), 1);
        } else {
            unsetenv("CUTMACHINE_PROJECT_ARCHIVE");
        }
    }
    void Set(const std::string& path) {
        setenv("CUTMACHINE_PROJECT_ARCHIVE", path.c_str(), 1);
    }

private:
    bool had_value_ = false;
    std::string before_;
};

Value Query(const std::string& database, const std::string& sql) {
    Value result;
    std::string error;
    std::string detail;
    Check(project_archive::Query(database, sql, 100, result, error, detail),
          "query archive: " + error + ": " + detail);
    return result;
}

std::string Count(const std::string& database) {
    const Value result =
        Query(database, "SELECT count(*) FROM project_snapshots");
    const Value* rows = result.Find("rows");
    return rows == nullptr ? std::string() : rows->Dump();
}

void AddTimeline(Project& project, ProjectEditLog& log) {
    EditError code = EditError::None;
    std::string error;
    Check(
        log.Apply(project,
                  AddProjectTimelineOperation{"Variante", 1080, 1920, {25, 1}},
                  code, error),
        "edit fixture through an operation: " + error);
}

}  // namespace

int Run() {
    ArchiveEnvironment environment;
    const fs::path root = fs::temp_directory_path() /
                          ("cutmachine-archive-delivery-" + GenerateUlid());
    fs::create_directory(root);
    const std::string database = (root / "data/projects.sqlite").string();
    environment.Set(database);
    std::string error;
    std::string path;
    Project project("Automatic archive");
    ProjectEditLog log;
    const std::string original = project.SaveToString();
    Check(CreatePortableProject((root / "film").string(), project, path, error),
          "creation saves and archives: " + error);
    Check(Count(database) == "[[1]]" &&
              StoredProjectArchiveStatus(path).pending == 0,
          "creation delivers one capture after package publication");
    AddTimeline(project, log);
    const std::string edited = project.SaveToString();
    Check(CommitStoredProjectAndLogs(path, project, {}, log, error),
          "save archives the next complete project: " + error);
    const Value snapshots = Query(
        database,
        "SELECT document_json FROM project_snapshots ORDER BY snapshot_id");
    const Value* rows = snapshots.Find("rows");
    Check(rows != nullptr && rows->AsArray().size() == 2 &&
              rows->AsArray()[0].AsArray()[0].AsString() == original &&
              rows->AsArray()[1].AsArray()[0].AsString() == edited,
          "two saves retain exact canonical JSON including every timeline");

    // SQL cannot open a directory. Both saved states must remain in the
    // package until delivery is repaired, including across a normal reopen.
    const fs::path unavailable = root / "unavailable.sqlite";
    fs::create_directory(unavailable);
    environment.Set(unavailable.string());
    Project waiting("Deferred archive");
    ProjectEditLog waitingLog;
    std::string waitingPath;
    const std::string waitingOriginal = waiting.SaveToString();
    Check(CreatePortableProject((root / "waiting").string(), waiting,
                                waitingPath, error),
          "archive failure does not report a failed package creation");
    AddTimeline(waiting, waitingLog);
    Check(
        CommitStoredProjectAndLogs(waitingPath, waiting, {}, waitingLog, error),
        "archive failure does not report a failed edit");
    Project reopened;
    Check(LoadStoredProject(waitingPath, reopened, error) &&
              reopened.SaveToString() == waiting.SaveToString() &&
              StoredProjectArchiveStatus(waitingPath).pending == 2,
          "reopen keeps both pending saves and the latest montage");
    const auto refused = StoredProjectArchiveStatus(waitingPath, true);
    Check(refused.pending == 2 && refused.error_name == "ArchiveError",
          "a delivery refusal is inspectable without losing the save");

    // Simulate death after the SQL commit but before outbox deletion.
    fs::remove(unavailable);
    fs::path firstPending;
    for (const auto& item : fs::directory_iterator(root / "waiting")) {
        if (item.path().filename().string().rfind(".cutmachine-archive-", 0) ==
                0 &&
            (firstPending.empty() || item.path() < firstPending)) {
            firstPending = item.path();
        }
    }
    Project delivered;
    Check(!firstPending.empty() &&
              Project::LoadFromString(Read(firstPending), delivered, error),
          "oldest pending generation is a canonical project");
    Value capture;
    std::string errorName;
    const Ulid firstId = firstPending.stem().string().substr(20);
    Check(project_archive::Capture(unavailable.string(), delivered,
                                   {firstId, "", "save"}, capture, errorName,
                                   error),
          "simulate SQL committed before process termination: " + error);
    const auto caughtUp = StoredProjectArchiveStatus(waitingPath, true);
    Check(caughtUp.error_name.empty() && caughtUp.pending == 0 &&
              caughtUp.archived == 2 && Count(unavailable.string()) == "[[2]]",
          "retry reuses the delivered ULID and archives the other saved state");
    Check(StoredProjectArchiveStatus(waitingPath, true).archived == 0 &&
              Count(unavailable.string()) == "[[2]]",
          "another sync does not create extra revisions");
    const Value recovered = Query(
        unavailable.string(),
        "SELECT document_json FROM project_snapshots ORDER BY snapshot_id");
    const auto* recoveredRows = recovered.Find("rows");
    Check(recoveredRows != nullptr && recoveredRows->AsArray().size() == 2 &&
              recoveredRows->AsArray()[0].AsArray()[0].AsString() ==
                  waitingOriginal,
          "catch-up preserves the older full JSON, not just the current one");

    // The existing package transaction restores the old generation and
    // removes a partially published outbox entry before SQL can observe it.
    environment.Set("off");
    Project rollback("Rollback witness");
    std::string rollbackPath;
    Check(CreatePortableProject((root / "rollback").string(), rollback,
                                rollbackPath, error),
          "create interrupted-commit fixture");
    const std::string rollbackJson = rollback.SaveToString();
    const Ulid interruptedId = GenerateUlid();
    const std::string pending = PendingName(interruptedId);
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    Write(rollbackPath + nonce + ".bak", rollbackJson);
    Write(rollbackPath, "partially published project");
    Write(root / "rollback" / pending, project.SaveToString());
    Write(root / "rollback/.cutmachine-transaction",
          "CUTMACHINE_TRANSACTION_V1\n" + nonce + "\n1 " +
              Hex("project.cutmachine.json") + "\n0 " + Hex(pending) + "\n");
    environment.Set(database);
    Check(LoadStoredProject(rollbackPath, reopened, error) &&
              reopened.SaveToString() == rollbackJson &&
              !fs::exists(root / "rollback" / pending) &&
              Count(database) == "[[2]]",
          "uncommitted outbox is rolled back before automatic delivery");

    // A committed generation whose process died before starting SQL is
    // delivered on open. Disabling capture preserves this pending JSON.
    const Ulid committedId = GenerateUlid();
    const fs::path committedPending =
        root / "rollback" / PendingName(committedId);
    Write(committedPending, rollbackJson);
    environment.Set("off");
    Check(LoadStoredProject(rollbackPath, reopened, error) &&
              fs::exists(committedPending),
          "off preserves existing pending generations");
    environment.Set(database);
    Check(LoadStoredProject(rollbackPath, reopened, error) &&
              !fs::exists(committedPending) && Count(database) == "[[3]]",
          "open delivers a committed generation left before SQL started");

    // A failed file save cannot publish a capture of a refused edit.
    Project candidate = rollback;
    ProjectEditLog candidateLog;
    AddTimeline(candidate, candidateLog);
    const fs::path timelineDirectory = root / "rollback/Timelines";
    const fs::path heldTimelines = root / "held-timelines";
    fs::rename(timelineDirectory, heldTimelines);
    fs::create_directory_symlink(heldTimelines, timelineDirectory);
    Check(!CommitStoredProjectAndLogs(rollbackPath, candidate, {}, candidateLog,
                                      error) &&
              Count(database) == "[[3]]" &&
              StoredProjectArchiveStatus(rollbackPath).pending == 0,
          "failed file publication creates neither an SQL row nor an outbox");
    fs::remove(timelineDirectory);
    fs::rename(heldTimelines, timelineDirectory);

    // Foreign files are never adopted as pending captures, and symbolic
    // links or invalid canonical projects are refused without being deleted.
    const fs::path malformed = root / "rollback" / PendingName(GenerateUlid());
    Write(malformed, "{}");
    auto invalid = StoredProjectArchiveStatus(rollbackPath, true);
    Check(invalid.error_name == "ValidationFailed" && fs::exists(malformed),
          "invalid pending data stays available for diagnosis");
    fs::remove(malformed);
    const fs::path linked = root / "rollback" / PendingName(GenerateUlid());
    fs::create_symlink(fs::path(path), linked);
    invalid = StoredProjectArchiveStatus(rollbackPath, true);
    Check(invalid.error_name == "IoError" && fs::is_symlink(linked),
          "pending delivery never follows a symbolic link");
    fs::remove(linked);

    // Import is independent of a running editor, preserves the source, and
    // reports locked/missing projects separately from successful captures.
    environment.Set("off");
    const std::string importedDatabase =
        (root / "import/projects.sqlite").string();
    const std::string beforeImport = Read(path);
    Value imported;
    Check(
        ImportStoredProjectsToArchive(
            importedDatabase,
            {(root / "film").string(), (root / "missing").string()}, imported,
            errorName, error) &&
            !imported.Find("complete")->AsBool() &&
            imported.Find("imported")->Dump() == "1" &&
            imported.Find("refused")->Dump() == "1" &&
            Read(path) == beforeImport,
        "partial import reports each outcome and does not change source JSON");
    Check(ImportStoredProjectsToArchive(importedDatabase, {path}, imported,
                                        errorName, error) &&
              Count(importedDatabase) == "[[1]]" &&
              imported.Dump().find("\"replayed\":true") != std::string::npos,
          "identical reimport reuses its durable capture");
    ProjectSessionLock held;
    std::string owner;
    Check(held.Acquire(path, owner, error), "hold imported project's lock");
    Check(ImportStoredProjectsToArchive(importedDatabase, {path}, imported,
                                        errorName, error) &&
              imported.Find("refused")->Dump() == "1" &&
              imported.Dump().find("ProjectLocked") != std::string::npos,
          "import does not read through another writer's lock");
    held.Release();

    // The registry exposes maintenance outside timeline selection, and a
    // server with no project can import a collection and query its archive.
    McpToolRegistry registry;
    McpProjectBackend backend(path, true);
    auto outcome =
        registry.Call(backend, "project_archive_status", Value::MakeObject());
    Check(outcome.ok &&
              outcome.result_json.find("\"pending\":0") != std::string::npos,
          "MCP status does not need a selected timeline");
    outcome =
        registry.Call(backend, "sync_project_archive", Value::MakeObject());
    Check(outcome.ok, "MCP sync does not need a selected timeline");
    McpProjectBackend noProject;
    Value args = Value::MakeObject();
    args.Set("database", Value::MakeString(importedDatabase));
    Value projectPaths = Value::MakeArray();
    projectPaths.Push(Value::MakeString(path));
    args.Set("projects", std::move(projectPaths));
    outcome = registry.Call(noProject, "import_project_archive", args);
    Check(outcome.ok && outcome.result_json.find("\"complete\":true") !=
                            std::string::npos,
          "MCP import works before a project is opened");
    environment.Set(importedDatabase);
    args = Value::MakeObject();
    args.Set("sql",
             Value::MakeString("SELECT count(*) FROM project_snapshots"));
    outcome = registry.Call(noProject, "query_project_archive", args);
    Check(outcome.ok &&
              outcome.result_json.find("\"rows\":[[1]]") != std::string::npos,
          "MCP query can use the configured default archive");

    // A second live process owns the package. Do not recover its current
    // file transaction or deliver an outbox it has only partly published.
    environment.Set("off");
    Project foreign("Foreign writer witness");
    std::string foreignPath;
    Check(CreatePortableProject((root / "foreign").string(), foreign,
                                foreignPath, error),
          "create cross-process delivery fixture");
    const fs::path foreignPending =
        root / "foreign" / PendingName(GenerateUlid());
    Write(foreignPending, foreign.SaveToString());
    const std::string foreignDatabase = (root / "foreign.sqlite").string();
    environment.Set(foreignDatabase);
    int ready[2];
    int done[2];
    const bool pipesReady = pipe(ready) == 0 && pipe(done) == 0;
    Check(pipesReady, "create lock-owner synchronization pipes");
    if (pipesReady) {
        const pid_t child = fork();
        Check(child >= 0, "fork a competing lock owner");
        if (child == 0) {
            close(ready[0]);
            close(done[1]);
            ProjectSessionLock foreignLock;
            std::string who;
            std::string detail;
            const char acquired =
                foreignLock.Acquire(foreignPath, who, detail) ? '1' : '0';
            (void)write(ready[1], &acquired, 1);
            char release;
            (void)read(done[0], &release, 1);
            foreignLock.Release();
            _exit(0);
        }
        close(ready[1]);
        close(done[0]);
        if (child > 0) {
            char acquired = '0';
            Check(read(ready[0], &acquired, 1) == 1 && acquired == '1',
                  "the other process holds the package lock");
            const fs::path marker = root / "foreign/.cutmachine-transaction";
            Write(marker, "an in-progress writer owns this marker");
            const auto blocked = StoredProjectArchiveStatus(foreignPath, true);
            Check(blocked.error_name == "ProjectLocked" &&
                      blocked.pending == 1 && fs::exists(marker) &&
                      fs::exists(foreignPending) &&
                      !fs::exists(foreignDatabase),
                  "delivery leaves a live foreign writer's transaction "
                  "untouched");
            const char release = '1';
            (void)write(done[1], &release, 1);
            int status = 0;
            Check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0,
                  "competing writer releases its lock");
            fs::remove(marker);
            const auto deliveredAfterRelease =
                StoredProjectArchiveStatus(foreignPath, true);
            Check(deliveredAfterRelease.error_name.empty() &&
                      deliveredAfterRelease.pending == 0 &&
                      Count(foreignDatabase) == "[[1]]",
                  "delivery proceeds only after the foreign writer releases");
        }
        close(ready[0]);
        close(done[1]);
    }

    // DEC44 -- docs/roadmap/DONNEES-2026-10.md. With a folder set, a save
    // makes the day's copy of the archive, once a day; a folder that cannot
    // take it is said in the status and never makes the delivery
    // incomplete.
    {
        const std::string copiedDatabase =
            (root / "copied/projects.sqlite").string();
        environment.Set(copiedDatabase);
        const fs::path backups = root / "OneDrive" / "Copies";
        fs::create_directories(backups);
        setenv("CUTMACHINE_ARCHIVE_BACKUP_DIR", backups.c_str(), 1);
        Project copied("Copied archive");
        ProjectEditLog copiedLog;
        std::string copiedPath;
        Check(CreatePortableProject((root / "copied-film").string(), copied,
                                    copiedPath, error),
              "creation with a backup folder: " + error);
        const fs::path copy =
            backups / project_archive::BackupFileName(std::time(nullptr));
        Check(fs::exists(copy), "the first save made the day's copy");
        const auto copiedAt = fs::last_write_time(copy);
        AddTimeline(copied, copiedLog);
        Check(CommitStoredProjectAndLogs(copiedPath, copied, {}, copiedLog,
                                         error),
              "a second save: " + error);
        Check(fs::last_write_time(copy) == copiedAt,
              "a second save the same day copies nothing");
        Check(StoredProjectArchiveStatus(copiedPath).backup.IsNull(),
              "the window's check after a save does not list the folder");
        const Value status = StoredProjectArchiveReportJson(
            StoredProjectArchiveStatus(copiedPath, false, true));
        const Value* backup = status.Find("backup");
        Check(backup != nullptr && backup->Find("configured") != nullptr &&
                  backup->Find("configured")->AsBool() &&
                  backup->Find("latest") != nullptr &&
                  backup->Find("latest")->AsString() ==
                      copy.filename().string() &&
                  !backup->Find("due")->AsBool(),
              "the status names today's copy: " + status.Dump());
        setenv("CUTMACHINE_ARCHIVE_BACKUP_DIR", (root / "unmounted").c_str(),
               1);
        AddTimeline(copied, copiedLog);
        Check(CommitStoredProjectAndLogs(copiedPath, copied, {}, copiedLog,
                                         error),
              "a save with an unmounted folder still saves: " + error);
        const Value refused = StoredProjectArchiveReportJson(
            StoredProjectArchiveStatus(copiedPath, false, true));
        const Value* refusedBackup = refused.Find("backup");
        Check(refused.Find("complete")->AsBool() && refusedBackup != nullptr &&
                  refusedBackup->Find("error") != nullptr &&
                  refusedBackup->Find("error")->AsString() == "IoError" &&
                  !fs::exists(root / "unmounted") &&
                  Count(copiedDatabase) == "[[3]]",
              "an unmounted folder is said, never created, and the "
              "captures are all delivered: " +
                  refused.Dump());
        setenv("CUTMACHINE_ARCHIVE_BACKUP_DIR", "off", 1);
    }

    fs::remove_all(root);
    return failures == 0 ? 0 : 1;
}

int main() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: archive delivery exception: " << error.what()
                  << '\n';
        return 1;
    }
}
