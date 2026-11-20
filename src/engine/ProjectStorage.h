#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "EditLog.h"
#include "Json.h"
#include "Project.h"
#include "RunStep.h"

// A background snapshot must observe a complete generation while the same
// process continues editing the live project.
std::recursive_mutex& StoredProjectIoMutex();

// PERF5 -- docs/roadmap/PERF-2026-09.md. How many package commits this
// process has attempted, whatever the project: every write of a package goes
// through CommitStoredProjectAndLogs, which counts itself once it has tried,
// successful or not. A caller that keeps a parsed package (the MCP session's
// cache) reads it before parsing and trusts its copy only while it has not
// moved -- so it learns that something rewrote the package from storage,
// instead of guessing which command writes.
uint64_t StoredProjectCommitGeneration();

// DB2 -- ROADMAP.md. A save queues its exact JSON in the same file
// transaction as the montage. SQL delivery may fail without undoing that
// save; queued generations survive until a later open/save or explicit sync.
// The archive is a machine-local preference (ProjectArchive.h), never a
// project field. Disabling it leaves existing pending captures untouched.

struct StoredProjectArchiveReport {
    std::string database;
    bool enabled = false;
    uint64_t archived = 0;
    uint64_t pending = 0;
    std::string error_name;
    std::string message;
    // DEC44 -- docs/roadmap/DONNEES-2026-10.md. The daily copy of the
    // archive (ProjectArchive.h): what its folder holds and, after a
    // delivery, what this call copied. {"configured":false} without a folder;
    // null unless the caller asked (describeBackup). A copy that fails is
    // said here and on stderr; it never makes the delivery incomplete.
    mcp_json::Value backup;
};

// retry=false only inspects pending files. retry=true recovers the package
// transaction first, then delivers pending generations idempotently, then
// makes the day's copy of the archive when one is configured and due.
// describeBackup lists the copies' folder for `backup`: the tools ask, the
// window's check after each save does not, so that a synchronized folder
// never slows it.
StoredProjectArchiveReport StoredProjectArchiveStatus(
    const std::string& projectPath, bool retry = false,
    bool describeBackup = false);
mcp_json::Value StoredProjectArchiveReportJson(
    const StoredProjectArchiveReport& report);

// Explicit import of up to 100 existing packages, with per-project results.
// Each project is locked and validated; one refusal does not hide successes.
bool ImportStoredProjectsToArchive(const std::string& database,
                                   const std::vector<std::string>& projectPaths,
                                   mcp_json::Value& result,
                                   std::string& errorName,
                                   std::string& message);

class ProjectSessionLock {
public:
    ProjectSessionLock();
    ~ProjectSessionLock();
    ProjectSessionLock(const ProjectSessionLock&) = delete;
    ProjectSessionLock& operator=(const ProjectSessionLock&) = delete;

    bool Acquire(const std::string& projectPath, std::string& owner,
                 std::string& error);
    void Release();
    bool Held() const;

private:
    std::string lock_directory_;
    std::string token_;
};

struct CollectionIntegrityReport {
    uint64_t verified_media = 0;
    std::vector<std::string> missing_media;
    std::vector<std::string> modified_media;
    // FI13 -- the timelines whose look LUT is missing or no longer matches
    // its digest: they open, their renders refuse until the .cube is
    // imported again.
    std::vector<std::string> damaged_looks;
};

// Returns success when the manifest was parsed and checked. Integrity failures
// of collected rushes, and of the timelines' look LUTs (FI13), are reported in
// the result rather than as I/O/parser errors: the project still opens, and
// re-importing a damaged LUT repairs it.
bool VerifyPortableProject(const std::string& projectPath,
                           CollectionIntegrityReport& report,
                           std::string& error);

// PRD20 -- ROADMAP.md. A project path where nothing exists was refused as an
// "unsupported project package": the path was wrong, and the message sent
// its reader to look at a format.
//
// Whether nothing exists at `projectPath`: neither the path itself nor, when
// it names the project file of a folder, that folder. A folder that exists
// and is not a package is not missing: it keeps the refusal it had.
bool IsMissingProjectPath(const std::string& projectPath);
// "no project at '<path>': nothing exists at this path" -- what
// LoadStoredProject and the session lock answer for such a path.
std::string MissingProjectMessage(const std::string& projectPath);
// Whether `message` holds that sentence, whatever its callers wrapped around
// it. Engine functions report failures as sentences; this is what lets a
// surface name this one (IoError, as for any file that is not there) without
// every function in between learning a second channel -- the arrangement
// ffmpeg::RefusalName already has for a missing FFmpeg.
bool IsMissingProjectMessage(const std::string& message);

bool IsPortableProjectV2(const std::string& projectPath);
bool CreatePortableProject(const std::string& destinationDirectory,
                           const Project& project, std::string& projectPath,
                           std::string& error);
bool LoadStoredProject(const std::string& projectPath, Project& project,
                       std::string& error, bool deliverArchive = true);
bool CommitStoredProject(const std::string& projectPath, const Project& project,
                         std::string& error);
// B10 -- ROADMAP.md. timelineLogs is a partial write set: an omitted existing
// journal stays byte-identical on disk, while a newly added timeline receives
// an empty journal. This lets project-only edits avoid reading every timeline
// history before an otherwise unrelated commit.
//
// PERF5 -- `storedBefore`, when given, is the project as stored before this
// edit, which the caller already holds: what compares the commit with the
// state before it reads that instead of parsing the package once more.
bool CommitStoredProjectAndLogs(
    const std::string& projectPath, const Project& project,
    const std::map<std::string, EditLog>& timelineLogs,
    const ProjectEditLog& projectLog, std::string& error,
    const Project* storedBefore = nullptr, bool deliverArchive = true,
    const RunStepReceipt* runStep = nullptr);

// OR2 -- reuse a lock already owned by this process, under the I/O mutex;
// otherwise acquire one before recovering or reading a transaction.
bool AcquireStoredProjectLock(const std::string& projectPath,
                              ProjectSessionLock& lock, std::string& errorName,
                              std::string& message);

// DEC24 -- SAVING_ROADMAP.md. A project journal written before S6 carries
// two whole projects per entry: 189 MB for PRJI's 33 changes of active
// timeline. Reading it converts each entry into a delta, in memory
// (CompactLegacyProjectEntry), and only the next project operation used to
// write that form back -- so the editor paid the 3 to 4 s of conversion at
// every opening of a project nobody reorganised (PERF-2026-09, 24
// September). Decided by the author on 23 October 2026: the journal is migrated
// at the opening, under the session lock. A binary from before S6 could
// not read it any more, nor does it read the version 3 project file beside
// it.
struct ProjectEditLogMigration {
    // The project has a journal.
    bool found = false;
    // Entries written before S6 that were read as deltas.
    size_t compacted = 0;
    // The journal was written back in that form, through the package's
    // commit transaction.
    bool rewritten = false;
    uint64_t bytes_before = 0;
    uint64_t bytes_after = 0;
    // Why it was not, when entries were compacted: the lock held by another
    // process, an I/O error. The journal read is still good, in memory, and
    // the next opening tries again.
    std::string rewrite_error;
};

// The project journal of `projectPath`, read as ProjectEditLog::Load reads
// it, and written back at once when some of its entries predate S6. The
// session lock is taken first -- reused when this process holds it, as the
// editor and an MCP session do at the opening -- so that what is written is
// what was read; without it the journal is read and nothing is written.
// Nothing else of the package is touched: the project, the timelines and
// their journals keep their bytes, and so does a journal that is already in
// the delta form, or whose entries cannot be expressed as deltas.
//
// No journal is an empty one. False, with `error`, only when the journal
// cannot be read: the same failure ProjectEditLog::Load gives.
bool LoadProjectEditLogMigrating(const std::string& projectPath,
                                 ProjectEditLog& log,
                                 ProjectEditLogMigration& migration,
                                 std::string& error);

// The same migration for a caller that does not keep the journal (an MCP
// session opening a project). A journal without a whole project in it --
// no `"exact_project_hex":"` -- is not parsed: an opening that never read
// the journal does not start paying for it.
bool MigrateProjectEditLog(const std::string& projectPath,
                           ProjectEditLogMigration& migration,
                           std::string& error);

struct PortableProjectResult {
    std::string directory_path;
    std::string project_path;
    uint64_t media_count = 0;
    uint64_t media_bytes = 0;
    // FI12 -- the journals copied as they were (a timeline's ID, or
    // "project"), because their rewrite did not replay exactly: undoing
    // there can bring back a path of the source. Empty in the usual case.
    std::vector<std::string> verbatim_histories;
};

// Creates an autonomous project directory without mutating the source. The
// destination must not exist. All original media must be readable; derived
// caches are deliberately excluded and will be regenerated by the receiver.
// FI12 -- the journals travel rewritten like the state, so that undo and
// redo in the copy stay inside it; what history alone names (a logo
// replaced since) is copied when it can be read. A rush version relinked
// away is not.
bool CollectPortableProject(const std::string& sourceProjectPath,
                            const std::string& destinationDirectory,
                            PortableProjectResult& result, std::string& error);
