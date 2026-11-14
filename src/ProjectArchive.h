#pragma once

// DB1 -- docs/roadmap/DONNEES-2026-10.md. Keep complete canonical project
// states for later SQL/JSON analysis, independently of the undo journal.
// Archiving never edits a project. Callers choose a local database and a
// capture boundary; ProjectStorage implements automatic capture (DB2).
// A supplied snapshot ULID makes a retry durable across server restarts:
// the same bytes and context return the first capture, different ones fail.
//
// DEC44 -- schema 3, "git in SQLite". A capture is a manifest of SHA-256
// digests (ArchiveManifest.h); its chunks -- each timeline, each
// composition, the rest of the document -- live once each in
// archive_chunks, compressed. project_snapshots is a view that gives every
// capture back as the document_json it was, byte for byte, so a caller's
// SQL reads as before. A writer migrates a schema 1 or 2 archive in one
// transaction, checking every capture against its old bytes before the old
// table goes. Nothing is ever deleted: triggers refuse it, chunks included.

#include "Json.h"
#include "Project.h"
#include "Ulid.h"

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace project_archive {

inline constexpr size_t kMaxQueryRows = 1000;
inline constexpr size_t kMaxQueryBytes = size_t{4} * 1024 * 1024;

// DB2 -- machine-local archive preference, independent of document storage.
// CUTMACHINE_PROJECT_ARCHIVE overrides Application Support (macOS) or
// ~/.local/share/cutmachine (elsewhere, UserDirectories.h); "off" disables.
std::string DefaultDatabasePath();

struct CaptureContext {
    Ulid snapshot_id;  // Empty generates an ID; supply one for retries.
    std::string run_id;
    std::string step;
    // DB3 -- optional canonical RunStepReceipt, inserted with its snapshot.
    std::string run_step_json;
};

// One transaction: create/check the archive schema and insert a capture.
// No replacement of an existing row, even if its revision hash matches.
// DEC44 -- an archive of an older schema is migrated first, in a
// transaction of its own, then compacted (VACUUM) when that succeeds.
bool Capture(const std::string& databasePath, const Project& project,
             const CaptureContext& context, mcp_json::Value& result,
             std::string& errorName, std::string& message);

// DB2 -- importing the same canonical state again reuses its import row.
// Compare bytes, not only the revision hash; lookup and insertion share a
// transaction so simultaneous imports cannot create duplicate captures.
// DEC44 -- the bytes are compared through the manifest: the same chunk
// digests at the same offsets are the same document.
bool CaptureImport(const std::string& databasePath, const Project& project,
                   mcp_json::Value& result, std::string& errorName,
                   std::string& message);

// One read-only SELECT (WITH allowed), including SQLite's JSON functions.
// Opens an existing archive without creating it. Returns column names and
// row arrays, retaining duplicate column names and integer precision. Row,
// byte and execution limits keep an agent's query bounded. No SQL writes,
// ATTACH, PRAGMA, extension loading or second statement are allowed.
// DEC44 -- the connection carries archive_document(manifest_json) and
// archive_chunk(digest), which project_snapshots and a caller's SQL use to
// read a schema 3 archive. A schema 1 or 2 archive is read as it is.
bool Query(const std::string& databasePath, const std::string& sql,
           size_t maxRows, mcp_json::Value& result, std::string& errorName,
           std::string& message);

// DEC44 -- the daily copy of the archive, off the disk it lives on.
//
// Set once in the settings file (LocalEnv.h, as the Whisper model, DEC23):
// CUTMACHINE_ARCHIVE_BACKUP_DIR names an existing absolute folder, which a
// sync client such as OneDrive may carry elsewhere; the engine only writes
// a file there and talks to no service. Unset or "off": no copy.
inline constexpr const char* kBackupDirectoryVariable =
    "CUTMACHINE_ARCHIVE_BACKUP_DIR";
// Copies are named CUTMACHINE_projects_<YYYYMMDD>.sqlite, the local date,
// beside the Resolve database backups of the same folder.
inline constexpr const char* kBackupPrefix = "CUTMACHINE_projects_";
// Dated copies kept: a week. The archive refuses deletions by itself, so a
// newer copy holds everything an older one does; older copies only guard
// against damage done outside the engine, and a week leaves the time to
// notice it. About 20 MB each on 24 October 2026.
inline constexpr size_t kKeptBackups = 7;

// The configured folder: "" when unset or "off".
std::string BackupDirectory();

// "CUTMACHINE_projects_20261024.sqlite" for `now` in local time.
std::string BackupFileName(std::time_t now);

enum class BackupMode {
    // The automatic copy: only when today has none yet.
    kIfDue,
    // An explicit copy: today's is written again.
    kNow,
};

struct BackupReport {
    std::string database;
    std::string directory;
    // Today's copy, written or found.
    std::string path;
    // This call wrote it.
    bool performed = false;
    // Older dated copies removed beyond kKeptBackups.
    std::vector<std::string> removed;
    // Dated copies in the folder after the call.
    size_t kept = 0;
    std::string error_name;
    std::string message;
};

// Writes today's copy of `database` into `directory`: VACUUM INTO a
// temporary file of that folder, synced, checked, then renamed, so that the
// dated name never holds a half-written base, even while agents write; the
// archive is migrated first if it is of an older schema. Then removes the
// dated copies beyond kKeptBackups, oldest first. The folder must exist: it
// is never created, since a missing one is usually a sync client not
// mounted. Another process already copying is not an error: nothing is
// written.
bool Backup(const std::string& database, const std::string& directory,
            BackupMode mode, std::time_t now, BackupReport& report);

mcp_json::Value BackupReportJson(const BackupReport& report);

// What a status report says of the copies without writing any.
struct BackupState {
    bool configured = false;
    std::string directory;
    // The newest dated copy's file name; empty when there is none.
    std::string latest;
    // Today has no copy yet.
    bool due = false;
    std::string error_name;
    std::string message;
};

BackupState InspectBackups(const std::string& directory, std::time_t now);
mcp_json::Value BackupStateJson(const BackupState& state);

}  // namespace project_archive
