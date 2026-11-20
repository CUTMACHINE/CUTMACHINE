#include "ProjectArchive.h"

#include "ArchiveManifest.h"
#include "CanonicalHash.h"
#include "LocalEnv.h"
#include "RunStep.h"
#include "UserDirectories.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace project_archive {

std::string DefaultDatabasePath() {
    if (const char* path = std::getenv("CUTMACHINE_PROJECT_ARCHIVE");
        path != nullptr && *path != '\0') {
        return std::strcmp(path, "off") == 0 ? std::string() : path;
    }
    // HEADLESS-2026-10 H4 -- Application Support on macOS (UserDirectories.h).
    const std::string directory = user_directories::ApplicationData();
    return directory.empty()
               ? std::string()
               : (std::filesystem::path(directory) / "projects.sqlite")
                     .string();
}

namespace {

namespace fs = std::filesystem;
using mcp_json::Value;
constexpr int kApplicationId = 0x434d4442;  // CMDB, distinct from other DBs.
constexpr int kSchemaVersion = 3;
constexpr int kMaxSqlBytes = 64 * 1024;
constexpr int kMaxDocumentBytes = 64 * 1024 * 1024;
// Decompressed chunks a connection keeps: captures of one project share
// most of theirs, so a query over many reads each chunk once.
constexpr size_t kChunkCacheBytes = size_t{64} * 1024 * 1024;

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

// DB3 -- schema 1 to 2: run receipts, linked to their capture.
constexpr char kSchemaOneToTwo[] =
    "CREATE TABLE project_run_steps ("
    "step_id TEXT PRIMARY KEY NOT NULL, run_id TEXT NOT NULL,"
    "stage TEXT NOT NULL, project_id TEXT NOT NULL,"
    "timeline_id TEXT NOT NULL, snapshot_id TEXT NOT NULL "
    "UNIQUE "
    "REFERENCES project_snapshots(snapshot_id),"
    "base_revision TEXT NOT NULL, result_revision TEXT NOT NULL,"
    "receipt_json TEXT NOT NULL "
    "CHECK(json_valid(receipt_json)));"
    "CREATE INDEX run_steps_by_run ON project_run_steps"
    "(run_id,stage,step_id);"
    "CREATE TRIGGER run_steps_no_replace BEFORE INSERT ON "
    "project_run_steps WHEN EXISTS(SELECT 1 FROM "
    "project_run_steps "
    "WHERE step_id=NEW.step_id) BEGIN SELECT "
    "RAISE(ABORT,'run steps are immutable'); END;"
    "CREATE TRIGGER run_steps_no_update BEFORE UPDATE ON "
    "project_run_steps BEGIN SELECT "
    "RAISE(ABORT,'run steps are immutable'); END;"
    "CREATE TRIGGER run_steps_no_delete BEFORE DELETE ON "
    "project_run_steps BEGIN SELECT "
    "RAISE(ABORT,'run steps are immutable'); END;"
    "PRAGMA user_version=2;";

// DEC44 -- schema 3: chunks once each, captures as manifests. Every table
// refuses updates, deletions and a second row under the same key, from any
// SQLite client; a capture cannot name a chunk the archive does not hold.
// The objects take names of their own so that a migration can build them
// beside the schema 2 table they replace.
constexpr char kSchemaThreeTables[] =
    "CREATE TABLE archive_chunks ("
    "digest TEXT PRIMARY KEY NOT NULL "
    "CHECK(length(digest)=64 AND digest NOT GLOB '*[^0-9a-f]*'),"
    "bytes INTEGER NOT NULL CHECK(typeof(bytes)='integer' AND bytes>=0),"
    "codec TEXT NOT NULL CHECK(codec='zstd'),"
    "content BLOB NOT NULL CHECK(typeof(content)='blob'));"
    "CREATE TRIGGER archive_chunks_no_replace BEFORE INSERT ON archive_chunks "
    "WHEN EXISTS(SELECT 1 FROM archive_chunks WHERE digest=NEW.digest) "
    "BEGIN SELECT RAISE(ABORT,'archive chunks are immutable'); END;"
    "CREATE TRIGGER archive_chunks_no_update BEFORE UPDATE ON archive_chunks "
    "BEGIN SELECT RAISE(ABORT,'archive chunks are immutable'); END;"
    "CREATE TRIGGER archive_chunks_no_delete BEFORE DELETE ON archive_chunks "
    "BEGIN SELECT RAISE(ABORT,'archive chunks are immutable'); END;"
    "CREATE TABLE archive_snapshots ("
    "snapshot_id TEXT PRIMARY KEY NOT NULL,"
    "project_id TEXT NOT NULL,"
    "revision_hash TEXT NOT NULL,"
    "created_at TEXT NOT NULL DEFAULT "
    "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
    "run_id TEXT NOT NULL, step TEXT NOT NULL,"
    "manifest_json TEXT NOT NULL CHECK(json_valid(manifest_json)));"
    "CREATE INDEX archive_snapshots_by_project ON archive_snapshots"
    "(project_id,created_at,snapshot_id);"
    "CREATE INDEX archive_snapshots_by_run ON archive_snapshots(run_id,step);"
    "CREATE TRIGGER archive_snapshots_no_replace BEFORE INSERT ON "
    "archive_snapshots WHEN EXISTS(SELECT 1 FROM archive_snapshots "
    "WHERE snapshot_id=NEW.snapshot_id) BEGIN SELECT "
    "RAISE(ABORT,'project snapshots are immutable'); END;"
    "CREATE TRIGGER archive_snapshots_need_chunks BEFORE INSERT ON "
    "archive_snapshots WHEN NOT EXISTS(SELECT 1 FROM archive_chunks WHERE "
    "digest=json_extract(NEW.manifest_json,'$.rest')) OR EXISTS(SELECT 1 "
    "FROM json_each(NEW.manifest_json,'$.parts') AS part WHERE NOT "
    "EXISTS(SELECT 1 FROM archive_chunks WHERE "
    "digest=json_extract(part.value,'$.digest'))) BEGIN SELECT "
    "RAISE(ABORT,'a project snapshot names stored chunks only'); END;"
    "CREATE TRIGGER archive_snapshots_no_update BEFORE UPDATE ON "
    "archive_snapshots BEGIN SELECT "
    "RAISE(ABORT,'project snapshots are immutable'); END;"
    "CREATE TRIGGER archive_snapshots_no_delete BEFORE DELETE ON "
    "archive_snapshots BEGIN SELECT "
    "RAISE(ABORT,'project snapshots are immutable'); END;";

std::string RunStepsTable(const std::string& name) {
    return "CREATE TABLE " + name +
           " (step_id TEXT PRIMARY KEY NOT NULL, run_id TEXT NOT NULL,"
           "stage TEXT NOT NULL, project_id TEXT NOT NULL,"
           "timeline_id TEXT NOT NULL, snapshot_id TEXT NOT NULL UNIQUE "
           "REFERENCES archive_snapshots(snapshot_id),"
           "base_revision TEXT NOT NULL, result_revision TEXT NOT NULL,"
           "receipt_json TEXT NOT NULL CHECK(json_valid(receipt_json)));";
}

// The same guards schema 2 had, on the table that now names
// archive_snapshots.
constexpr char kRunStepGuards[] =
    "CREATE INDEX run_steps_by_run ON project_run_steps"
    "(run_id,stage,step_id);"
    "CREATE TRIGGER run_steps_no_replace BEFORE INSERT ON project_run_steps "
    "WHEN EXISTS(SELECT 1 FROM project_run_steps WHERE step_id=NEW.step_id) "
    "BEGIN SELECT RAISE(ABORT,'run steps are immutable'); END;"
    "CREATE TRIGGER run_steps_no_update BEFORE UPDATE ON project_run_steps "
    "BEGIN SELECT RAISE(ABORT,'run steps are immutable'); END;"
    "CREATE TRIGGER run_steps_no_delete BEFORE DELETE ON project_run_steps "
    "BEGIN SELECT RAISE(ABORT,'run steps are immutable'); END;";

// What a caller's SQL has always read, under the same name and columns.
constexpr char kSnapshotsView[] =
    "CREATE VIEW project_snapshots(snapshot_id,project_id,revision_hash,"
    "created_at,run_id,step,document_json) AS SELECT snapshot_id,project_id,"
    "revision_hash,created_at,run_id,step,archive_document(manifest_json) "
    "FROM archive_snapshots;";

bool Refuse(const char* name, const std::string& detail, std::string& errorName,
            std::string& message) {
    errorName = name;
    message = detail;
    return false;
}

bool SqlError(sqlite3* db, std::string& errorName, std::string& message) {
    return Refuse("ArchiveError", sqlite3_errmsg(db), errorName, message);
}

bool Execute(sqlite3* db, const char* sql, std::string& errorName,
             std::string& message) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK ||
           SqlError(db, errorName, message);
}

bool Execute(sqlite3* db, const std::string& sql, std::string& errorName,
             std::string& message) {
    return Execute(db, sql.c_str(), errorName, message);
}

bool Prepare(sqlite3* db, const std::string& sql, Statement& statement,
             std::string& errorName, std::string& message,
             const char** tail = nullptr) {
    sqlite3_stmt* raw = nullptr;
    const int code = sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, tail);
    statement.reset(raw);
    return code == SQLITE_OK || SqlError(db, errorName, message);
}

std::string Text(sqlite3_stmt* statement, int column) {
    const auto* bytes = sqlite3_column_text(statement, column);
    return bytes == nullptr
               ? std::string()
               : std::string(reinterpret_cast<const char*>(bytes),
                             sqlite3_column_bytes(statement, column));
}

bool Bind(sqlite3* db, sqlite3_stmt* statement, int index,
          std::string_view value, std::string& errorName,
          std::string& message) {
    return sqlite3_bind_text64(statement, index, value.data(), value.size(),
                               SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK ||
           SqlError(db, errorName, message);
}

// DEC44 -- the chunks a connection has read, decompressed and checked
// against their digest. Shared pointers: a document being put together
// keeps its chunks even if the cache is emptied meanwhile.
using Chunk = std::shared_ptr<const std::string>;

struct ChunkCache {
    std::unordered_map<std::string, Chunk> chunks;
    size_t bytes = 0;
};

// `code` is the SQLite result that stopped the read, SQLITE_OK otherwise:
// a query interrupted by its budget says so, rather than "chunk missing".
bool LoadChunk(sqlite3* db, ChunkCache& cache, const std::string& digest,
               Chunk& chunk, int& code, std::string& message) {
    code = SQLITE_OK;
    if (const auto found = cache.chunks.find(digest);
        found != cache.chunks.end()) {
        chunk = found->second;
        return true;
    }
    sqlite3_stmt* raw = nullptr;
    code = sqlite3_prepare_v2(
        db, "SELECT bytes,codec,content FROM archive_chunks WHERE digest=?", -1,
        &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    if (code != SQLITE_OK) {
        message = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(raw, 1, digest.data(), static_cast<int>(digest.size()),
                      SQLITE_TRANSIENT);
    code = sqlite3_step(raw);
    if (code != SQLITE_ROW) {
        message = code == SQLITE_DONE
                      ? "archive chunk " + digest + " is missing"
                      : std::string(sqlite3_errmsg(db));
        if (code == SQLITE_DONE) code = SQLITE_OK;
        return false;
    }
    code = SQLITE_OK;
    const int64_t bytes = sqlite3_column_int64(raw, 0);
    if (Text(raw, 1) != kArchiveCodec || bytes < 0 ||
        static_cast<uint64_t>(bytes) > kMaxArchiveChunkBytes) {
        message = "archive chunk " + digest + " has an unknown codec or size";
        return false;
    }
    const void* frame = sqlite3_column_blob(raw, 2);
    const int frameBytes = sqlite3_column_bytes(raw, 2);
    std::string content;
    if (!DecompressArchiveChunk(
            std::string_view(static_cast<const char*>(frame),
                             static_cast<size_t>(frameBytes)),
            static_cast<size_t>(bytes), content, message)) {
        message = "archive chunk " + digest + ": " + message;
        return false;
    }
    // The digest is the address: a chunk changed under it is refused.
    if (ArchiveDigest(content) != digest) {
        message = "archive chunk " + digest + " does not match its digest";
        return false;
    }
    chunk = std::make_shared<const std::string>(std::move(content));
    if (cache.bytes + chunk->size() > kChunkCacheBytes) {
        cache.chunks.clear();
        cache.bytes = 0;
    }
    if (chunk->size() <= kChunkCacheBytes) {
        cache.chunks.emplace(digest, chunk);
        cache.bytes += chunk->size();
    }
    return true;
}

bool AssembleCapture(sqlite3* db, ChunkCache& cache,
                     const std::string& manifestJson, std::string& document,
                     int& code, std::string& message) {
    code = SQLITE_OK;
    ArchiveManifest manifest;
    if (!ParseArchiveManifest(manifestJson, manifest, message)) {
        return false;
    }
    Chunk rest;
    if (!LoadChunk(db, cache, manifest.rest, rest, code, message)) {
        return false;
    }
    std::vector<Chunk> parts;
    std::vector<std::string_view> views;
    parts.reserve(manifest.parts.size());
    views.reserve(manifest.parts.size());
    for (const ArchivePart& part : manifest.parts) {
        Chunk chunk;
        if (!LoadChunk(db, cache, part.digest, chunk, code, message)) {
            return false;
        }
        views.emplace_back(*chunk);
        parts.push_back(std::move(chunk));
    }
    return AssembleArchiveDocument(manifest, *rest, views, document, message);
}

std::string ValueText(sqlite3_value* value) {
    const auto* bytes = sqlite3_value_text(value);
    return bytes == nullptr ? std::string()
                            : std::string(reinterpret_cast<const char*>(bytes),
                                          sqlite3_value_bytes(value));
}

void ReportFunctionError(sqlite3_context* context, int code,
                         const std::string& message) {
    if (code == SQLITE_INTERRUPT) {
        sqlite3_result_error_code(context, SQLITE_INTERRUPT);
        return;
    }
    sqlite3_result_error(context, message.c_str(), -1);
}

// archive_document(manifest_json): the capture's document, as TEXT.
void ArchiveDocumentFunction(sqlite3_context* context, int,
                             sqlite3_value** values) {
    if (sqlite3_value_type(values[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }
    auto& cache = *static_cast<ChunkCache*>(sqlite3_user_data(context));
    std::string document;
    std::string message;
    int code = SQLITE_OK;
    if (!AssembleCapture(sqlite3_context_db_handle(context), cache,
                         ValueText(values[0]), document, code, message)) {
        ReportFunctionError(context, code, message);
        return;
    }
    sqlite3_result_text64(context, document.data(), document.size(),
                          SQLITE_TRANSIENT, SQLITE_UTF8);
}

// archive_chunk(digest): one chunk, as TEXT -- a timeline across captures
// without putting each whole document back together.
void ArchiveChunkFunction(sqlite3_context* context, int,
                          sqlite3_value** values) {
    if (sqlite3_value_type(values[0]) == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }
    auto& cache = *static_cast<ChunkCache*>(sqlite3_user_data(context));
    const std::string digest = ValueText(values[0]);
    if (!IsArchiveDigest(digest)) {
        sqlite3_result_error(context, "archive_chunk takes a SHA-256 digest",
                             -1);
        return;
    }
    Chunk chunk;
    std::string message;
    int code = SQLITE_OK;
    if (!LoadChunk(sqlite3_context_db_handle(context), cache, digest, chunk,
                   code, message)) {
        ReportFunctionError(context, code, message);
        return;
    }
    sqlite3_result_text64(context, chunk->data(), chunk->size(),
                          SQLITE_TRANSIENT, SQLITE_UTF8);
}

void DeleteCache(void* cache) { delete static_cast<ChunkCache*>(cache); }

// Innocuous and deterministic: they only read immutable rows, and
// trusted_schema=OFF lets a view use nothing else.
bool RegisterFunctions(sqlite3* db) {
    constexpr int kFlags =
        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS;
    return sqlite3_create_function_v2(db, "archive_document", 1, kFlags,
                                      new ChunkCache(), ArchiveDocumentFunction,
                                      nullptr, nullptr,
                                      DeleteCache) == SQLITE_OK &&
           sqlite3_create_function_v2(db, "archive_chunk", 1, kFlags,
                                      new ChunkCache(), ArchiveChunkFunction,
                                      nullptr, nullptr,
                                      DeleteCache) == SQLITE_OK;
}

bool Open(const std::string& path, bool write, Database& db,
          std::string& errorName, std::string& message) {
    if (path.empty() || path == ":memory:" ||
        path.find('\0') != std::string::npos) {
        return Refuse("ValidationFailed", "database must name a local file",
                      errorName, message);
    }
    sqlite3* raw = nullptr;
    const int mode = write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                           : SQLITE_OPEN_READONLY;
    const int code = sqlite3_open_v2(path.c_str(), &raw,
                                     mode | SQLITE_OPEN_FULLMUTEX, nullptr);
    db.reset(raw);
    if (code != SQLITE_OK) {
        return Refuse(
            "ArchiveError",
            raw != nullptr ? sqlite3_errmsg(raw) : "cannot open archive",
            errorName, message);
    }
    sqlite3_busy_timeout(raw, 5000);
    sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, kMaxDocumentBytes);
    sqlite3_limit(raw, SQLITE_LIMIT_SQL_LENGTH, kMaxSqlBytes);
    sqlite3_limit(raw, SQLITE_LIMIT_COLUMN, 128);
    sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    if (!RegisterFunctions(raw)) {
        return SqlError(raw, errorName, message);
    }
    return Execute(raw, "PRAGMA trusted_schema=OFF", errorName, message) &&
           (!write || Execute(raw,
                              "PRAGMA fullfsync=ON; PRAGMA synchronous=FULL; "
                              "PRAGMA foreign_keys=ON",
                              errorName, message));
}

bool Scalar(sqlite3* db, const char* sql, int64_t& value,
            std::string& errorName, std::string& message) {
    Statement statement(nullptr, sqlite3_finalize);
    if (!Prepare(db, sql, statement, errorName, message)) {
        return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return SqlError(db, errorName, message);
    }
    value = sqlite3_column_int64(statement.get(), 0);
    return true;
}

bool Scalar(sqlite3* db, const char* sql, int& value, std::string& errorName,
            std::string& message) {
    int64_t wide = 0;
    if (!Scalar(db, sql, wide, errorName, message)) {
        return false;
    }
    value = static_cast<int>(wide);
    return true;
}

bool Versions(sqlite3* db, int& application, int& version,
              std::string& errorName, std::string& message) {
    return Scalar(db, "PRAGMA application_id", application, errorName,
                  message) &&
           Scalar(db, "PRAGMA user_version", version, errorName, message);
}

struct Rollback {
    sqlite3* db;
    ~Rollback() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
};

// DEC44 -- the chunks of a capture, each stored once: a digest already
// there is reused, after checking that it names as many bytes.
class ChunkStore {
public:
    explicit ChunkStore(sqlite3* db) : db_(db) {}

    bool Prepare(std::string& errorName, std::string& message) {
        return project_archive::Prepare(
                   db_, "SELECT bytes FROM archive_chunks WHERE digest=?",
                   find_, errorName, message) &&
               project_archive::Prepare(
                   db_,
                   "INSERT INTO archive_chunks(digest,bytes,codec,content) "
                   "VALUES(?,?,?,?)",
                   insert_, errorName, message) &&
               project_archive::Prepare(
                   db_,
                   "INSERT INTO archive_snapshots(snapshot_id,project_id,"
                   "revision_hash,created_at,run_id,step,manifest_json) "
                   "VALUES(?,?,?,coalesce(?,strftime('%Y-%m-%dT%H:%M:%fZ',"
                   "'now')),?,?,?) RETURNING created_at",
                   snapshot_, errorName, message);
    }

    bool Store(const ArchiveSplit& split, std::string& errorName,
               std::string& message) {
        std::set<std::string_view> seen;
        if (!Ensure(split.manifest.rest, split.rest, seen, errorName,
                    message)) {
            return false;
        }
        for (size_t index = 0; index < split.parts.size(); ++index) {
            if (!Ensure(split.manifest.parts[index].digest, split.parts[index],
                        seen, errorName, message)) {
                return false;
            }
        }
        return true;
    }

    // `createdAt` empty: now.
    bool InsertSnapshot(const std::string& id, const std::string& projectId,
                        const std::string& revision,
                        const std::string& createdAt, const std::string& runId,
                        const std::string& step, const std::string& manifest,
                        std::string& created, std::string& errorName,
                        std::string& message) {
        sqlite3_stmt* statement = snapshot_.get();
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        if (!Bind(db_, statement, 1, id, errorName, message) ||
            !Bind(db_, statement, 2, projectId, errorName, message) ||
            !Bind(db_, statement, 3, revision, errorName, message) ||
            (!createdAt.empty() &&
             !Bind(db_, statement, 4, createdAt, errorName, message)) ||
            !Bind(db_, statement, 5, runId, errorName, message) ||
            !Bind(db_, statement, 6, step, errorName, message) ||
            !Bind(db_, statement, 7, manifest, errorName, message)) {
            return false;
        }
        if (sqlite3_step(statement) != SQLITE_ROW) {
            return SqlError(db_, errorName, message);
        }
        created = Text(statement, 0);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            return SqlError(db_, errorName, message);
        }
        sqlite3_reset(statement);
        return true;
    }

    size_t inserted() const { return inserted_; }

private:
    bool Ensure(const std::string& digest, std::string_view bytes,
                std::set<std::string_view>& seen, std::string& errorName,
                std::string& message) {
        if (!seen.insert(digest).second) {
            return true;
        }
        sqlite3_stmt* find = find_.get();
        sqlite3_reset(find);
        if (!Bind(db_, find, 1, digest, errorName, message)) {
            return false;
        }
        const int code = sqlite3_step(find);
        if (code == SQLITE_ROW) {
            const int64_t stored = sqlite3_column_int64(find, 0);
            sqlite3_reset(find);
            if (stored != static_cast<int64_t>(bytes.size())) {
                return Refuse(
                    "ArchiveError",
                    "archive chunk " + digest + " is stored with another size",
                    errorName, message);
            }
            return true;
        }
        if (code != SQLITE_DONE) {
            return SqlError(db_, errorName, message);
        }
        sqlite3_reset(find);
        std::string frame;
        if (!CompressArchiveChunk(bytes, frame, message)) {
            errorName = "ArchiveError";
            return false;
        }
        sqlite3_stmt* insert = insert_.get();
        sqlite3_reset(insert);
        if (!Bind(db_, insert, 1, digest, errorName, message) ||
            sqlite3_bind_int64(insert, 2, static_cast<int64_t>(bytes.size())) !=
                SQLITE_OK ||
            !Bind(db_, insert, 3, kArchiveCodec, errorName, message) ||
            sqlite3_bind_blob64(insert, 4, frame.data(), frame.size(),
                                SQLITE_TRANSIENT) != SQLITE_OK) {
            return SqlError(db_, errorName, message);
        }
        if (sqlite3_step(insert) != SQLITE_DONE) {
            return SqlError(db_, errorName, message);
        }
        sqlite3_reset(insert);
        ++inserted_;
        return true;
    }

    sqlite3* db_;
    Statement find_{nullptr, sqlite3_finalize};
    Statement insert_{nullptr, sqlite3_finalize};
    Statement snapshot_{nullptr, sqlite3_finalize};
    size_t inserted_ = 0;
};

// DEC44 -- schema 2 to 3, inside the caller's transaction. Each capture is
// cut, its chunks stored, its manifest inserted with its own date, and the
// document put back together from what the database now holds and compared
// with the old bytes before anything is retired. A refusal rolls everything
// back; a process killed midway leaves SQLite's journal to do the same. The
// run receipts move to a table that names archive_snapshots, unchanged.
bool MigrateTwoToThree(sqlite3* db, std::string& errorName,
                       std::string& message) {
    int64_t before = 0;
    if (!Scalar(db, "SELECT count(*) FROM project_snapshots", before, errorName,
                message) ||
        !Execute(db, kSchemaThreeTables, errorName, message)) {
        return false;
    }
    ChunkStore store(db);
    Statement rows(nullptr, sqlite3_finalize);
    if (!store.Prepare(errorName, message) ||
        !Prepare(db,
                 "SELECT snapshot_id,project_id,revision_hash,created_at,"
                 "run_id,step,document_json FROM project_snapshots "
                 "ORDER BY rowid",
                 rows, errorName, message)) {
        return false;
    }
    ChunkCache verified;
    int code = SQLITE_OK;
    while ((code = sqlite3_step(rows.get())) == SQLITE_ROW) {
        const std::string id = Text(rows.get(), 0);
        const std::string document = Text(rows.get(), 6);
        const ArchiveSplit split = SplitArchiveDocument(document);
        const std::string manifest = SerializeArchiveManifest(split.manifest);
        std::string created;
        if (!store.Store(split, errorName, message) ||
            !store.InsertSnapshot(id, Text(rows.get(), 1), Text(rows.get(), 2),
                                  Text(rows.get(), 3), Text(rows.get(), 4),
                                  Text(rows.get(), 5), manifest, created,
                                  errorName, message)) {
            message = "capture " + id + ": " + message;
            return false;
        }
        std::string rebuilt;
        int readCode = SQLITE_OK;
        if (!AssembleCapture(db, verified, manifest, rebuilt, readCode,
                             message) ||
            rebuilt != document) {
            return Refuse("ArchiveMigrationFailed",
                          "capture " + id +
                              " does not read back as its schema 2 bytes" +
                              (message.empty() ? "" : ": " + message),
                          errorName, message);
        }
    }
    if (code != SQLITE_DONE) {
        return SqlError(db, errorName, message);
    }
    rows.reset();
    int64_t after = 0;
    if (!Scalar(db, "SELECT count(*) FROM archive_snapshots", after, errorName,
                message)) {
        return false;
    }
    if (after != before) {
        return Refuse("ArchiveMigrationFailed",
                      "schema 3 holds " + std::to_string(after) +
                          " captures, schema 2 held " + std::to_string(before),
                      errorName, message);
    }
    return Execute(db,
                   RunStepsTable("project_run_steps_v3") +
                       "INSERT INTO project_run_steps_v3 SELECT step_id,"
                       "run_id,stage,project_id,timeline_id,snapshot_id,"
                       "base_revision,result_revision,receipt_json FROM "
                       "project_run_steps ORDER BY rowid;"
                       // DROP TABLE fires no DELETE trigger: the rows went
                       // across first, and the new table guards them.
                       "DROP TABLE project_run_steps;"
                       "ALTER TABLE project_run_steps_v3 RENAME TO "
                       "project_run_steps;" +
                       kRunStepGuards + "DROP TABLE project_snapshots;" +
                       kSnapshotsView + "PRAGMA user_version=3;",
                   errorName, message);
}

// `create`: a writer, which creates an empty archive and migrates an older
// schema. A reader consults schema 1, 2 or 3 as it is.
bool Schema(sqlite3* db, bool create, bool& migrated, std::string& errorName,
            std::string& message) {
    migrated = false;
    int application = 0;
    int version = 0;
    if (!Versions(db, application, version, errorName, message)) {
        return false;
    }
    if (application == kApplicationId && version == kSchemaVersion) {
        return true;
    }
    if (application == kApplicationId && (version == 1 || version == 2)) {
        if (!create) {
            return true;
        }
        // DB3 -- v1 gains its run receipts, then DEC44 moves both.
        if (version == 1 && !Execute(db, kSchemaOneToTwo, errorName, message)) {
            return false;
        }
        migrated = true;
        return MigrateTwoToThree(db, errorName, message);
    }
    if (create && application == 0 && version == 0) {
        int tables = 0;
        if (!Scalar(db, "SELECT count(*) FROM sqlite_schema", tables, errorName,
                    message)) {
            return false;
        }
        if (tables == 0) {
            return Execute(db,
                           std::string(kSchemaThreeTables) +
                               RunStepsTable("project_run_steps") +
                               kRunStepGuards + kSnapshotsView +
                               "PRAGMA application_id=1129137218;"
                               "PRAGMA user_version=3;",
                           errorName, message);
        }
    }
    return Refuse("ArchiveSchemaMismatch",
                  "not a supported CUTMACHINE archive (schema 1 to 3)",
                  errorName, message);
}

// Gives the file back the pages a migration freed, when they are more than
// half of it. Best effort: another connection may hold the archive, and a
// VACUUM that cannot run changes nothing.
void Compact(sqlite3* db) {
    std::string ignoredName;
    std::string ignored;
    int64_t pages = 0;
    int64_t free = 0;
    if (Scalar(db, "PRAGMA page_count", pages, ignoredName, ignored) &&
        Scalar(db, "PRAGMA freelist_count", free, ignoredName, ignored) &&
        free * 2 > pages && free > 1024) {
        sqlite3_exec(db, "VACUUM", nullptr, nullptr, nullptr);
    }
}

// A writer's connection on an archive of the current schema: created when
// empty, migrated in a transaction of its own when older, then compacted.
bool OpenWriter(const std::string& path, Database& db, std::string& errorName,
                std::string& message) {
    if (!Open(path, true, db, errorName, message)) {
        return false;
    }
    int application = 0;
    int version = 0;
    if (!Versions(db.get(), application, version, errorName, message)) {
        return false;
    }
    if (application == kApplicationId && version == kSchemaVersion) {
        return true;
    }
    // A migration writes a few tens of MB and reads the whole archive:
    // keep its pages in memory, so readers are locked out only at commit.
    sqlite3_exec(db.get(), "PRAGMA cache_size=-131072", nullptr, nullptr,
                 nullptr);
    bool migrated = false;
    {
        if (!Execute(db.get(), "BEGIN IMMEDIATE", errorName, message)) {
            return false;
        }
        const Rollback rollback{db.get()};
        if (!Schema(db.get(), true, migrated, errorName, message) ||
            !Execute(db.get(), "COMMIT", errorName, message)) {
            return false;
        }
    }
    sqlite3_exec(db.get(), "PRAGMA cache_size=-2000", nullptr, nullptr,
                 nullptr);
    if (migrated) {
        Compact(db.get());
    }
    return true;
}

int Authorize(void*, int action, const char*, const char*, const char*,
              const char*) {
    switch (action) {
        case SQLITE_SELECT:
        case SQLITE_READ:
        case SQLITE_FUNCTION:
        case SQLITE_RECURSIVE:
            return SQLITE_OK;
        default:
            return SQLITE_DENY;
    }
}

struct QueryBudget {
    int calls = 0;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
};

int StopExpensiveQuery(void* context) {
    auto& budget = *static_cast<QueryBudget*>(context);
    return (++budget.calls > 2000 ||
            std::chrono::steady_clock::now() > budget.deadline)
               ? 1
               : 0;
}

bool CaptureImpl(const std::string& databasePath, const Project& project,
                 const CaptureContext& context, bool importing, Value& result,
                 std::string& errorName, std::string& message) {
    errorName.clear();
    message.clear();
    if (!project.Validate(message)) {
        errorName = "ValidationFailed";
        return false;
    }
    Ulid id =
        context.snapshot_id.empty() ? GenerateUlid() : context.snapshot_id;
    if (!IsValidUlid(id) || context.run_id.size() > 1024 ||
        context.step.size() > 1024) {
        return Refuse("ValidationFailed",
                      "snapshot_id must be a ULID; run_id and step are "
                      "limited to 1024 bytes each",
                      errorName, message);
    }
    const std::string json = project.SaveToString();
    RunStepReceipt runStep;
    if (!context.run_step_json.empty() &&
        (!ParseRunStepReceipt(context.run_step_json, runStep, message) ||
         runStep.snapshot_id != id || runStep.project_id != project.id ||
         runStep.result_revision != CanonicalHash(json) ||
         runStep.run_id != context.run_id || runStep.stage != context.step)) {
        return Refuse("ValidationFailed",
                      "capture and run receipt disagree: " + message, errorName,
                      message);
    }
    if (json.size() > kMaxDocumentBytes - 4096) {
        return Refuse("ArchiveLimit", "project JSON exceeds archive limit",
                      errorName, message);
    }
    // DEC44 -- cut before taking the lock: the transaction only looks up
    // digests and compresses the chunks the archive does not hold yet.
    const ArchiveSplit split = SplitArchiveDocument(json);
    const std::string manifest = SerializeArchiveManifest(split.manifest);
    Database db(nullptr, sqlite3_close);
    if (!OpenWriter(databasePath, db, errorName, message) ||
        !Execute(db.get(), "BEGIN IMMEDIATE", errorName, message)) {
        return false;
    }
    const Rollback rollback{db.get()};
    int application = 0;
    int version = 0;
    if (!Versions(db.get(), application, version, errorName, message)) {
        return false;
    }
    if (application != kApplicationId || version != kSchemaVersion) {
        return Refuse("ArchiveSchemaMismatch",
                      "the archive changed schema while it was opened",
                      errorName, message);
    }
    Statement statement(nullptr, sqlite3_finalize);
    if (importing) {
        if (!Prepare(db.get(),
                     "SELECT snapshot_id FROM archive_snapshots "
                     "WHERE project_id=? AND manifest_json=? "
                     "AND run_id='' AND step='import' LIMIT 1",
                     statement, errorName, message) ||
            !Bind(db.get(), statement.get(), 1, project.id, errorName,
                  message) ||
            !Bind(db.get(), statement.get(), 2, manifest, errorName, message)) {
            return false;
        }
        const int found = sqlite3_step(statement.get());
        if (found == SQLITE_ROW) {
            id = Text(statement.get(), 0);
        } else if (found != SQLITE_DONE) {
            return SqlError(db.get(), errorName, message);
        }
        statement.reset();
    }
    if (!Prepare(db.get(),
                 "SELECT project_id,revision_hash,created_at,run_id,step,"
                 "manifest_json FROM archive_snapshots WHERE snapshot_id=?",
                 statement, errorName, message) ||
        !Bind(db.get(), statement.get(), 1, id, errorName, message)) {
        return false;
    }
    const int code = sqlite3_step(statement.get());
    const bool replayed = code == SQLITE_ROW;
    const std::string revision = CanonicalHash(json);
    std::string created;
    if (replayed) {
        // The same manifest is the same bytes: every chunk is named by the
        // SHA-256 of its content, at the same place.
        if (Text(statement.get(), 0) != project.id ||
            Text(statement.get(), 1) != revision ||
            Text(statement.get(), 3) != context.run_id ||
            Text(statement.get(), 4) != context.step ||
            Text(statement.get(), 5) != manifest) {
            return Refuse("SnapshotIdReused",
                          "snapshot_id already names different data", errorName,
                          message);
        }
        created = Text(statement.get(), 2);
    } else {
        if (code != SQLITE_DONE) {
            return SqlError(db.get(), errorName, message);
        }
        statement.reset();
        ChunkStore store(db.get());
        if (!store.Prepare(errorName, message) ||
            !store.Store(split, errorName, message) ||
            !store.InsertSnapshot(id, project.id, revision, "", context.run_id,
                                  context.step, manifest, created, errorName,
                                  message)) {
            return false;
        }
    }
    statement.reset();
    if (!context.run_step_json.empty()) {
        if (!Prepare(db.get(),
                     "SELECT receipt_json FROM project_run_steps WHERE "
                     "step_id=?",
                     statement, errorName, message) ||
            !Bind(db.get(), statement.get(), 1, runStep.step_id, errorName,
                  message)) {
            return false;
        }
        const int found = sqlite3_step(statement.get());
        if (found == SQLITE_ROW) {
            if (Text(statement.get(), 0) != context.run_step_json) {
                return Refuse("RunStepIdReused",
                              "step_id already names "
                              "different run data",
                              errorName, message);
            }
        } else if (found == SQLITE_DONE) {
            statement.reset();
            if (!Prepare(
                    db.get(),
                    "INSERT INTO project_run_steps(step_id,run_id,stage,"
                    "project_id,timeline_id,snapshot_id,base_revision,"
                    "result_revision,receipt_json) VALUES(?,?,?,?,?,?,?,?,?)",
                    statement, errorName, message)) {
                return false;
            }
            const std::string* values[] = {
                &runStep.step_id,       &runStep.run_id,
                &runStep.stage,         &runStep.project_id,
                &runStep.timeline_id,   &runStep.snapshot_id,
                &runStep.base_revision, &runStep.result_revision,
                &context.run_step_json};
            for (int index = 0; index < 9; ++index) {
                if (!Bind(db.get(), statement.get(), index + 1, *values[index],
                          errorName, message)) {
                    return false;
                }
            }
            if (sqlite3_step(statement.get()) != SQLITE_DONE) {
                return SqlError(db.get(), errorName, message);
            }
        } else {
            return SqlError(db.get(), errorName, message);
        }
        statement.reset();
    }
    if (!Execute(db.get(), "COMMIT", errorName, message)) {
        return false;
    }
    result = Value::MakeObject();
    result.Set("snapshot_id", Value::MakeString(id));
    result.Set("project_id", Value::MakeString(project.id));
    result.Set("revision_hash", Value::MakeString(revision));
    result.Set("created_at", Value::MakeString(created));
    result.Set("run_id", Value::MakeString(context.run_id));
    result.Set("step", Value::MakeString(context.step));
    result.Set("replayed", Value::MakeBool(replayed));
    return true;
}

// --- DEC44: the daily copy --------------------------------------------

bool IsDatedCopy(const std::string& name) {
    const std::string prefix = kBackupPrefix;
    constexpr char kSuffix[] = ".sqlite";
    const size_t suffix = sizeof(kSuffix) - 1;
    return name.size() == prefix.size() + 8 + suffix &&
           name.compare(0, prefix.size(), prefix) == 0 &&
           name.compare(name.size() - suffix, suffix, kSuffix) == 0 &&
           name.substr(prefix.size(), 8).find_first_not_of("0123456789") ==
               std::string::npos;
}

// The dated copies of `directory`, newest first: the date is in the name,
// so the name orders them.
std::vector<std::string> DatedCopies(const fs::path& directory,
                                     std::error_code& error) {
    std::vector<std::string> names;
    for (fs::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        const std::string name = it->path().filename().string();
        std::error_code statusError;
        if (IsDatedCopy(name) &&
            fs::is_regular_file(fs::symlink_status(it->path(), statusError))) {
            names.push_back(name);
        }
    }
    std::sort(names.rbegin(), names.rend());
    return names;
}

bool CheckBackupDirectory(const std::string& directory, std::string& errorName,
                          std::string& message) {
    if (directory.empty()) {
        return Refuse("NotConfigured",
                      std::string("no copy of the project archive is made: "
                                  "set ") +
                          kBackupDirectoryVariable + " to a folder in '" +
                          local_env::LocalEnvFilePath() + "'",
                      errorName, message);
    }
    if (!fs::path(directory).is_absolute()) {
        return Refuse("ValidationFailed",
                      std::string(kBackupDirectoryVariable) +
                          " must be an absolute folder, or off",
                      errorName, message);
    }
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        return Refuse("IoError",
                      "the backup folder '" + directory +
                          "' does not exist; it is never created, in case "
                          "its sync client is not mounted",
                      errorName, message);
    }
    return true;
}

// Durable on disk before the rename publishes it. F_FULLFSYNC is macOS's
// flush to the medium, as SQLite's fullfsync; elsewhere fsync is.
bool SyncPath(const std::string& path, std::string& message) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        message = "cannot open '" + path + "': " + std::strerror(errno);
        return false;
    }
#if defined(__APPLE__)
    bool ok = ::fcntl(descriptor, F_FULLFSYNC) == 0 || ::fsync(descriptor) == 0;
#else
    bool ok = ::fsync(descriptor) == 0;
#endif
    if (!ok) {
        message = "cannot sync '" + path + "': " + std::strerror(errno);
    }
    ::close(descriptor);
    return ok;
}

// A copy in progress names its temporary file; one older than this was
// left by a process that died.
constexpr auto kAbandonedCopy = std::chrono::hours(1);

}  // namespace

bool Capture(const std::string& databasePath, const Project& project,
             const CaptureContext& context, Value& result,
             std::string& errorName, std::string& message) {
    return CaptureImpl(databasePath, project, context, false, result, errorName,
                       message);
}

bool CaptureImport(const std::string& databasePath, const Project& project,
                   Value& result, std::string& errorName,
                   std::string& message) {
    return CaptureImpl(databasePath, project, {"", "", "import"}, true, result,
                       errorName, message);
}

bool Query(const std::string& databasePath, const std::string& sql,
           size_t maxRows, Value& result, std::string& errorName,
           std::string& message) {
    errorName.clear();
    message.clear();
    if (sql.empty() || sql.size() > kMaxSqlBytes ||
        sql.find('\0') != std::string::npos || maxRows == 0 ||
        maxRows > kMaxQueryRows) {
        return Refuse("ValidationFailed",
                      "query needs SQL of at most 64 KiB and max_rows "
                      "between 1 and 1000",
                      errorName, message);
    }
    Database db(nullptr, sqlite3_close);
    bool migrated = false;
    if (!Open(databasePath, false, db, errorName, message) ||
        !Schema(db.get(), false, migrated, errorName, message)) {
        return false;
    }
    QueryBudget budget;
    sqlite3_progress_handler(db.get(), 1000, StopExpensiveQuery, &budget);
    sqlite3_set_authorizer(db.get(), Authorize, nullptr);
    Statement statement(nullptr, sqlite3_finalize);
    const char* tail = nullptr;
    if (!Prepare(db.get(), sql, statement, errorName, message, &tail)) {
        return false;
    }
    if (!statement || sqlite3_stmt_readonly(statement.get()) == 0 ||
        sqlite3_column_count(statement.get()) == 0) {
        return Refuse("ValidationFailed", "query must be one read-only SELECT",
                      errorName, message);
    }
    Statement extra(nullptr, sqlite3_finalize);
    if (!Prepare(db.get(), tail, extra, errorName, message)) {
        return false;
    }
    if (extra) {
        return Refuse("ValidationFailed", "query must contain one statement",
                      errorName, message);
    }
    Value columns = Value::MakeArray();
    const int count = sqlite3_column_count(statement.get());
    for (int column = 0; column < count; ++column) {
        columns.Push(
            Value::MakeString(sqlite3_column_name(statement.get(), column)));
    }
    size_t bytes = columns.Dump().size();
    Value rows = Value::MakeArray();
    bool truncated = false;
    int code = SQLITE_OK;
    while ((code = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (rows.AsArray().size() == maxRows) {
            truncated = true;
            break;
        }
        Value row = Value::MakeArray();
        for (int column = 0; column < count; ++column) {
            switch (sqlite3_column_type(statement.get(), column)) {
                case SQLITE_NULL:
                    row.Push(Value::MakeNull());
                    break;
                case SQLITE_INTEGER:
                    row.Push(Value::MakeInt(
                        sqlite3_column_int64(statement.get(), column)));
                    break;
                case SQLITE_FLOAT: {
                    const double value =
                        sqlite3_column_double(statement.get(), column);
                    if (!std::isfinite(value)) {
                        return Refuse("ValidationFailed",
                                      "query returned a non-finite number",
                                      errorName, message);
                    }
                    row.Push(Value::MakeDouble(value));
                    break;
                }
                case SQLITE_TEXT:
                    row.Push(Value::MakeString(Text(statement.get(), column)));
                    break;
                default:
                    return Refuse("ValidationFailed",
                                  "query returned a BLOB; select text instead",
                                  errorName, message);
            }
        }
        bytes += row.Dump().size();
        if (bytes > kMaxQueryBytes) {
            return Refuse("ArchiveLimit", "query result exceeds 4 MiB",
                          errorName, message);
        }
        rows.Push(std::move(row));
    }
    if (!truncated && code != SQLITE_DONE) {
        if (code == SQLITE_INTERRUPT ||
            sqlite3_errcode(db.get()) == SQLITE_INTERRUPT) {
            return Refuse("ArchiveLimit", "query exceeded execution budget",
                          errorName, message);
        }
        return SqlError(db.get(), errorName, message);
    }
    result = Value::MakeObject();
    result.Set("columns", std::move(columns));
    result.Set("rows", std::move(rows));
    result.Set("truncated", Value::MakeBool(truncated));
    return true;
}

std::string BackupDirectory() {
    const std::string value = local_env::Value(kBackupDirectoryVariable);
    return value == "off" ? std::string() : value;
}

std::string BackupFileName(std::time_t now) {
    std::tm local{};
    localtime_r(&now, &local);
    char date[16] = {};
    std::strftime(date, sizeof(date), "%Y%m%d", &local);
    return std::string(kBackupPrefix) + date + ".sqlite";
}

bool Backup(const std::string& database, const std::string& directory,
            BackupMode mode, std::time_t now, BackupReport& report) {
    report = BackupReport{};
    report.database = database;
    report.directory = directory;
    if (!CheckBackupDirectory(directory, report.error_name, report.message)) {
        return false;
    }
    std::error_code error;
    if (database.empty() || !fs::is_regular_file(database, error)) {
        return Refuse("ArchiveError",
                      database.empty()
                          ? std::string("the project archive is off")
                          : "no project archive at '" + database + "'",
                      report.error_name, report.message);
    }
    const std::string name = BackupFileName(now);
    const fs::path target = fs::path(directory) / name;
    report.path = target.string();
    if (mode == BackupMode::kIfDue && fs::exists(target, error)) {
        report.kept = DatedCopies(directory, error).size();
        return true;
    }
    // The copy is of the current schema, and of a file without the pages
    // a migration left free.
    {
        Database writer(nullptr, sqlite3_close);
        if (!OpenWriter(database, writer, report.error_name, report.message)) {
            return false;
        }
        Compact(writer.get());
    }
    // The temporary name is the lock: one process copies at a time.
    const fs::path temporary = fs::path(directory) / ("." + name + ".tmp");
    int descriptor = -1;
    for (int attempt = 0; attempt < 2 && descriptor < 0; ++attempt) {
        descriptor = ::open(temporary.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (descriptor >= 0 || errno != EEXIST) {
            break;
        }
        const auto modified = fs::last_write_time(temporary, error);
        if (error ||
            fs::file_time_type::clock::now() - modified <
                std::chrono::duration_cast<fs::file_time_type::clock::duration>(
                    kAbandonedCopy)) {
            report.message = "another process is copying the archive";
            return true;
        }
        fs::remove(temporary, error);
    }
    if (descriptor < 0) {
        return Refuse("IoError",
                      "cannot create '" + temporary.string() +
                          "': " + std::strerror(errno),
                      report.error_name, report.message);
    }
    ::close(descriptor);
    struct RemoveUnlessPublished {
        fs::path path;
        bool published = false;
        ~RemoveUnlessPublished() {
            if (!published) {
                std::error_code ignored;
                fs::remove(path, ignored);
            }
        }
    } cleanup{temporary};
    {
        // VACUUM INTO reads in one transaction: the copy is one state of
        // the archive, whatever writers do meanwhile, and holds no free
        // page.
        Database source(nullptr, sqlite3_close);
        Statement vacuum(nullptr, sqlite3_finalize);
        if (!Open(database, false, source, report.error_name, report.message) ||
            !Prepare(source.get(), "VACUUM INTO ?", vacuum, report.error_name,
                     report.message) ||
            !Bind(source.get(), vacuum.get(), 1, temporary.string(),
                  report.error_name, report.message)) {
            return false;
        }
        if (sqlite3_step(vacuum.get()) != SQLITE_DONE) {
            return SqlError(source.get(), report.error_name, report.message);
        }
    }
    if (!SyncPath(temporary.string(), report.message)) {
        report.error_name = "IoError";
        return false;
    }
    {
        Database copy(nullptr, sqlite3_close);
        Statement check(nullptr, sqlite3_finalize);
        int application = 0;
        int version = 0;
        if (!Open(temporary.string(), false, copy, report.error_name,
                  report.message) ||
            !Versions(copy.get(), application, version, report.error_name,
                      report.message) ||
            !Prepare(copy.get(), "PRAGMA quick_check", check, report.error_name,
                     report.message)) {
            return false;
        }
        if (application != kApplicationId || version != kSchemaVersion ||
            sqlite3_step(check.get()) != SQLITE_ROW ||
            Text(check.get(), 0) != "ok") {
            return Refuse("ArchiveError",
                          "the copy did not read back as a sound archive",
                          report.error_name, report.message);
        }
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        return Refuse(
            "IoError",
            "cannot publish '" + target.string() + "': " + std::strerror(errno),
            report.error_name, report.message);
    }
    cleanup.published = true;
    report.performed = true;
    if (!SyncPath(directory, report.message)) {
        report.error_name = "IoError";
        return false;
    }
    std::vector<std::string> copies = DatedCopies(directory, error);
    for (size_t index = kKeptBackups; index < copies.size(); ++index) {
        // Only older than today's: a copy dated later (a clock set back)
        // is left alone.
        if (copies[index] >= name) {
            continue;
        }
        fs::remove(fs::path(directory) / copies[index], error);
        if (!error) {
            report.removed.push_back(copies[index]);
        }
    }
    report.kept = DatedCopies(directory, error).size();
    return true;
}

Value BackupReportJson(const BackupReport& report) {
    Value result = Value::MakeObject();
    result.Set("ok", Value::MakeBool(report.error_name.empty()));
    result.Set("database", Value::MakeString(report.database));
    result.Set("directory", Value::MakeString(report.directory));
    result.Set("path", Value::MakeString(report.path));
    result.Set("performed", Value::MakeBool(report.performed));
    Value removed = Value::MakeArray();
    for (const std::string& name : report.removed) {
        removed.Push(Value::MakeString(name));
    }
    result.Set("removed", std::move(removed));
    result.Set("kept", Value::MakeInt(static_cast<int64_t>(report.kept)));
    if (!report.error_name.empty()) {
        result.Set("error", Value::MakeString(report.error_name));
    }
    if (!report.message.empty()) {
        result.Set("detail", Value::MakeString(report.message));
    }
    return result;
}

BackupState InspectBackups(const std::string& directory, std::time_t now) {
    BackupState state;
    state.directory = directory;
    state.configured = !directory.empty();
    if (!CheckBackupDirectory(directory, state.error_name, state.message)) {
        return state;
    }
    std::error_code error;
    const std::vector<std::string> copies = DatedCopies(directory, error);
    if (error) {
        state.error_name = "IoError";
        state.message = "cannot list the backup folder: " + error.message();
        return state;
    }
    state.latest = copies.empty() ? std::string() : copies.front();
    state.due = state.latest != BackupFileName(now);
    return state;
}

Value BackupStateJson(const BackupState& state) {
    Value result = Value::MakeObject();
    result.Set("configured", Value::MakeBool(state.configured));
    result.Set("directory", Value::MakeString(state.directory));
    result.Set("latest", Value::MakeString(state.latest));
    result.Set("due", Value::MakeBool(state.due));
    if (!state.error_name.empty()) {
        result.Set("error", Value::MakeString(state.error_name));
        result.Set("detail", Value::MakeString(state.message));
    }
    return result;
}

}  // namespace project_archive
