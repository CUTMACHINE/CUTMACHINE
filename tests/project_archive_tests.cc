// DB1 -- docs/roadmap/DONNEES-2026-10.md. An archive keeps exact project
// JSON across reopen/retry/concurrent capture, and SQL cannot edit it.
//
// DEC44 -- schema 3 stores each chunk once (ArchiveManifest.h): captures
// read back byte for byte through project_snapshots, nothing can be
// deleted, a schema 1 or 2 archive migrates without losing a byte -- even
// when the process dies midway -- and the daily copy is one consistent
// state, kept a week.

#include "ArchiveManifest.h"
#include "CanonicalHash.h"
#include "EditLog.h"
#include "ProjectArchive.h"
#include "ProjectFixture.h"
#include "Ulid.h"
#include "archive_schema_fixtures.h"

#include <signal.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"

namespace {

namespace fs = std::filesystem;
using mcp_json::Value;

std::string Field(const Value& value, const char* name) {
    const Value* field = value.Find(name);
    return field != nullptr && field->IsString() ? field->AsString()
                                                 : std::string();
}

bool Rows(const Value& result, size_t count) {
    const Value* rows = result.Find("rows");
    return rows != nullptr && rows->IsArray() &&
           rows->AsArray().size() == count;
}

const Value& Cell(const Value& result, size_t row, size_t column) {
    return result.Find("rows")->AsArray()[row].AsArray()[column];
}

fs::path Scratch(const std::string& name) {
    const fs::path directory =
        fs::temp_directory_path() /
        ("cutmachine-archive-" + name + "-" + GenerateUlid());
    fs::create_directories(directory);
    return directory;
}

// A native SQLite client, as any tool outside the engine opens the file.
struct Native {
    sqlite3* db = nullptr;
    explicit Native(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            throw std::runtime_error("cannot open " + path);
        }
    }
    ~Native() { sqlite3_close(db); }
    bool Exec(const std::string& sql) const {
        return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) ==
               SQLITE_OK;
    }
    int64_t Integer(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        int64_t value = -1;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) ==
                SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW) {
            value = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        return value;
    }
    std::string Text(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        std::string value;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) ==
                SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW &&
            sqlite3_column_text(statement, 0) != nullptr) {
            value.assign(reinterpret_cast<const char*>(
                             sqlite3_column_text(statement, 0)),
                         sqlite3_column_bytes(statement, 0));
        }
        sqlite3_finalize(statement);
        return value;
    }
};

std::string QueryText(const std::string& database, const std::string& sql) {
    Value result;
    std::string error;
    std::string message;
    if (!project_archive::Query(database, sql, 1, result, error, message) ||
        !Rows(result, 1) || !Cell(result, 0, 0).IsString()) {
        throw std::runtime_error("query failed: " + sql + ": " + message);
    }
    return Cell(result, 0, 0).AsString();
}

std::string Escape(const std::string& text) {
    std::string out;
    for (const char character : text) {
        out += character;
        if (character == '\'') out += '\'';
    }
    return out;
}

// One capture of a fabricated schema 1 or 2 archive.
struct OldCapture {
    std::string id;
    std::string project_id;
    std::string created_at;
    std::string document;
};

void InsertOld(const Native& native, const OldCapture& capture) {
    const bool ok = native.Exec(
        "INSERT INTO project_snapshots(snapshot_id,project_id,revision_hash,"
        "created_at,run_id,step,document_json) VALUES('" +
        capture.id + "','" + capture.project_id + "','" +
        CanonicalHash(capture.document) + "','" + capture.created_at +
        "','','save','" + Escape(capture.document) + "')");
    if (!ok) {
        throw std::runtime_error(std::string("fixture insert: ") +
                                 sqlite3_errmsg(native.db));
    }
}

// What a schema 2 archive of a busy day looks like: a project of several
// timelines saved again and again, one timeline edited each time, and a
// few captures no current serializer writes.
std::vector<OldCapture> BusyDay(size_t saves) {
    std::vector<OldCapture> captures;
    Project project = MakeInterviewShapedProject(40, 6, 12);
    for (size_t save = 0; save < saves; ++save) {
        project.timelines[save % project.timelines.size()].name =
            "Version " + std::to_string(save);
        char created[32];
        std::snprintf(created, sizeof(created),
                      "2026-10-23T15:%02zu:%02zu.%03zuZ", (save / 60) % 60,
                      save % 60, save % 1000);
        captures.push_back(
            {GenerateUlid(), project.id, created, project.SaveToString()});
    }
    // Another writer's whitespace, timelines as strings (project version
    // 2), escapes, a JSON that is no project: kept to the byte.
    const std::string other = GenerateUlid();
    captures.push_back({GenerateUlid(), other, "2026-10-05T12:48:20.331Z",
                        "{ \"id\" : \"" + other +
                            "\", \"timelines\" : [ \"{\\\"version\\\":7}\" ] "
                            ", \"name\":\"\xc3\x89milie \\u00e9\\n\" }"});
    captures.push_back(
        {GenerateUlid(), other, "2026-10-05T12:48:21.000Z", "[1,2,3]"});
    return captures;
}

std::string MakeOldArchive(const fs::path& directory, int version,
                           const std::vector<OldCapture>& captures) {
    const std::string database = (directory / "projects.sqlite").string();
    Native native(database);
    if (!native.Exec(archive_fixtures::kSchemaOne) ||
        (version == 2 && !native.Exec(archive_fixtures::kSchemaOneToTwo))) {
        throw std::runtime_error("fixture schema");
    }
    native.Exec("BEGIN");
    for (const OldCapture& capture : captures) {
        InsertOld(native, capture);
    }
    native.Exec("COMMIT");
    return database;
}

void TestCaptureAndQuery() {
    const fs::path directory = Scratch("capture");
    const std::string database = (directory / "projects.sqlite").string();
    Project base("Portrait d’Émilie");
    ProjectEditLog preparation;
    std::string setupError;
    EditError setupCode = EditError::None;
    Check(
        preparation.Apply(
            base, AddProjectTimelineOperation{"Variante", 1080, 1920, {25, 1}},
            setupCode, setupError),
        "create a second timeline");
    const std::string original = base.SaveToString();
    project_archive::CaptureContext context{GenerateUlid(), "run-1",
                                            "selection"};
    Value capture;
    std::string error;
    std::string message;
    Check(project_archive::Capture(database, base, context, capture, error,
                                   message),
          "capture: " + message);
    Check(Field(capture, "snapshot_id") == context.snapshot_id &&
              Field(capture, "revision_hash") == CanonicalHash(original) &&
              !Field(capture, "created_at").empty() &&
              !capture.Find("replayed")->AsBool(),
          "capture has an identity, revision, date and context");
    Value result;
    Check(project_archive::Query(
              database,
              "SELECT document_json,json_extract(document_json,'$.name'),"
              "json_array_length(document_json,'$.timelines') "
              "FROM project_snapshots",
              10, result, error, message) &&
              Rows(result, 1) && Cell(result, 0, 0).AsString() == original &&
              Cell(result, 0, 1).AsString() == base.name &&
              Cell(result, 0, 2).Dump() == "2",
          "reopen retains every canonical JSON byte and supports JSON SQL");
    Check(project_archive::Query(database, "SELECT * FROM project_snapshots",
                                 10, result, error, message) &&
              result.Find("columns")->Dump() ==
                  "[\"snapshot_id\",\"project_id\",\"revision_hash\","
                  "\"created_at\",\"run_id\",\"step\",\"document_json\"]",
          "project_snapshots keeps its seven columns: " + message);
    Project restored;
    Check(Project::LoadFromString(original, restored, message) &&
              restored.SaveToString() == original,
          "archived full project can be read back by the engine");
    Value retry;
    Check(project_archive::Capture(database, base, context, retry, error,
                                   message) &&
              retry.Find("replayed")->AsBool() &&
              Field(retry, "created_at") == Field(capture, "created_at"),
          "retry across new connections returns the first capture");
    auto otherContext = context;
    otherContext.step = "finition";
    Check(!project_archive::Capture(database, base, otherContext, retry, error,
                                    message) &&
              error == "SnapshotIdReused",
          "an existing ID cannot be reused with another context");

    Project edited = base;
    ProjectEditLog log;
    EditError editError = EditError::None;
    Check(log.Apply(
              edited,
              RenameProjectItemOperation{base.active_timeline_id, "Film monté"},
              editError, message),
          "prepare a real revised project");
    Check(!project_archive::Capture(database, edited, context, retry, error,
                                    message) &&
              error == "SnapshotIdReused",
          "an existing ID cannot overwrite another revision");
    otherContext.snapshot_id = GenerateUlid();
    Check(project_archive::Capture(database, edited, otherContext, retry, error,
                                   message),
          "capture the new revision");
    Check(base.SaveToString() == original, "archiving never edits the project");
    // A view has no rowid: the order is the caller's to state.
    Check(project_archive::Query(database,
                                 "SELECT document_json FROM project_snapshots "
                                 "ORDER BY step='finition'",
                                 10, result, error, message) &&
              Rows(result, 2) && Cell(result, 0, 0).AsString() == original &&
              Cell(result, 1, 0).AsString() == edited.SaveToString(),
          "a new revision adds a capture and retains the earlier one");
    {
        Native native(database);
        Check(native.Integer("PRAGMA user_version") == 3 &&
                  native.Integer("SELECT count(*) FROM archive_snapshots") == 2,
              "a new archive is schema 3");
        // The renamed timeline is a new chunk; the rest and the other
        // timeline are the ones the first capture stored.
        Check(native.Integer("SELECT count(*) FROM archive_chunks") == 4,
              "the second capture added one chunk: " +
                  std::to_string(
                      native.Integer("SELECT count(*) FROM archive_chunks")));
    }

    for (const char* sql :
         {"DELETE FROM project_snapshots", "DELETE FROM archive_chunks",
          "UPDATE project_snapshots SET step='lost'",
          "DROP TABLE project_snapshots", "PRAGMA user_version=2",
          "PRAGMA wal_checkpoint", "ATTACH ':memory:' AS other",
          "SELECT 1; SELECT 2", "SELECT load_extension('/tmp/no.so')",
          "WITH x AS (SELECT 1) DELETE FROM archive_snapshots RETURNING *"}) {
        Check(
            !project_archive::Query(database, sql, 10, result, error, message),
            std::string("query refuses: ") + sql);
    }
    Check(project_archive::Query(database,
                                 "WITH x AS (SELECT 9223372036854775807 AS n) "
                                 "SELECT n,n,NULL,1.25 FROM x; -- comment",
                                 10, result, error, message) &&
              Rows(result, 1) &&
              Cell(result, 0, 0).Dump() == "9223372036854775807" &&
              result.Find("columns")->AsArray().size() == 4,
          "WITH, comments, duplicate columns and exact int64 values work");
    Check(project_archive::Query(database, "SELECT * FROM project_snapshots", 1,
                                 result, error, message) &&
              Rows(result, 1) && result.Find("truncated")->AsBool(),
          "row cap is explicit");
    Check(!project_archive::Query(
              database,
              "WITH RECURSIVE x(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM x) "
              "SELECT sum(n) FROM x",
              10, result, error, message) &&
              error == "ArchiveLimit",
          "unbounded computation is interrupted");
    Check(!project_archive::Query(database, "SELECT printf('%.*c',4194305,'a')",
                                  10, result, error, message) &&
              error == "ArchiveLimit",
          "large output is refused");
    Check(!project_archive::Query(database, "SELECT x'01'", 10, result, error,
                                  message),
          "binary output is refused");
    Check(
        !project_archive::Query(database, "SELECT content FROM archive_chunks",
                                10, result, error, message),
        "a compressed chunk is binary, and refused as such");
    Check(!project_archive::Query(database, "SELECT 1e999", 10, result, error,
                                  message),
          "non-finite numeric output is refused");
    Check(!project_archive::Query(database, "SELECT 1", 0, result, error,
                                  message),
          "invalid row cap is refused");
    const auto missing = directory / "missing.sqlite";
    Check(!project_archive::Query(missing.string(), "SELECT 1", 1, result,
                                  error, message) &&
              !fs::exists(missing),
          "a query never creates a missing database");

    // archive_chunk reads one chunk: here every version of the renamed
    // timeline, without putting whole documents back together.
    Check(project_archive::Query(
              database,
              "SELECT json_extract(archive_chunk(part.value->>'digest'),"
              "'$.name') FROM archive_snapshots, json_each(manifest_json,"
              "'$.parts') AS part WHERE part.value->>'id'='" +
                  base.active_timeline_id + "' ORDER BY step='finition'",
              10, result, error, message) &&
              Rows(result, 2) && Cell(result, 1, 0).AsString() == "Film monté",
          "archive_chunk gives a timeline across captures: " + message);
    Check(project_archive::Query(
              database,
              "SELECT archive_document(manifest_json)=document_json FROM "
              "archive_snapshots JOIN project_snapshots USING(snapshot_id)",
              10, result, error, message) &&
              Rows(result, 2) && Cell(result, 0, 0).Dump() == "1",
          "archive_document is what the view reads: " + message);
    Check(!project_archive::Query(database, "SELECT archive_chunk('abc')", 1,
                                  result, error, message),
          "archive_chunk takes a digest");

    // Even a SQLite client outside the engine cannot update archived rows.
    {
        Native native(database);
        for (const char* sql :
             {"DELETE FROM project_snapshots", "DELETE FROM archive_snapshots",
              "DELETE FROM archive_chunks", "UPDATE archive_chunks SET bytes=0",
              "UPDATE archive_snapshots SET step='lost'",
              "INSERT OR REPLACE INTO archive_snapshots SELECT * FROM "
              "archive_snapshots",
              "INSERT OR REPLACE INTO archive_chunks SELECT * FROM "
              "archive_chunks"}) {
            Check(!native.Exec(sql),
                  std::string("schema enforces immutability for native SQL "
                              "clients too: ") +
                      sql);
        }
        ArchiveManifest dangling;
        dangling.bytes = 2;
        dangling.rest = ArchiveDigest("{}");
        Check(!native.Exec("INSERT INTO archive_snapshots(snapshot_id,"
                           "project_id,revision_hash,run_id,step,"
                           "manifest_json) VALUES('" +
                           GenerateUlid() + "','p','h','','','" +
                           SerializeArchiveManifest(dangling) + "')"),
              "a capture naming a chunk the archive lacks is refused");
        Check(native.Integer("SELECT count(*) FROM archive_snapshots") == 2,
              "and nothing was added");
    }

    std::atomic_int concurrentFailures{0};
    std::vector<std::thread> workers;
    workers.reserve(6);
    for (int index = 0; index < 6; ++index) {
        workers.emplace_back([&] {
            Value answer;
            std::string name;
            std::string detail;
            if (!project_archive::Capture(database, base, context, answer, name,
                                          detail) ||
                !answer.Find("replayed")->AsBool()) {
                ++concurrentFailures;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Check(concurrentFailures == 0,
          "concurrent retries are one durable capture");
    Check(project_archive::Query(database,
                                 "SELECT count(*) FROM project_snapshots", 1,
                                 result, error, message) &&
              Cell(result, 0, 0).Dump() == "2",
          "refusals and retries leave both archived states intact");
    workers.clear();
    for (int index = 0; index < 6; ++index) {
        workers.emplace_back([&] {
            Value answer;
            std::string name;
            std::string detail;
            if (!project_archive::CaptureImport(database, base, answer, name,
                                                detail)) {
                ++concurrentFailures;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Check(concurrentFailures == 0 &&
              project_archive::Query(database,
                                     "SELECT count(*) FROM project_snapshots "
                                     "WHERE step='import'",
                                     1, result, error, message) &&
              Cell(result, 0, 0).Dump() == "1",
          "simultaneous identical imports share one durable row");
    Value import;
    Check(project_archive::CaptureImport(database, edited, import, error,
                                         message) &&
              !import.Find("replayed")->AsBool(),
          "another state imports as a new row");

    // The digest is the address: a chunk changed behind the triggers' back
    // is refused, never read as another document.
    {
        Native native(database);
        Check(native.Exec("DROP TRIGGER archive_chunks_no_update") &&
                  native.Exec("UPDATE archive_chunks SET content=x'00' "
                              "WHERE rowid=1"),
              "a client can still drop a trigger");
    }
    Check(!project_archive::Query(database,
                                  "SELECT document_json FROM project_snapshots",
                                  10, result, error, message) &&
              error == "ArchiveError",
          "a damaged chunk is refused: " + message);
    fs::remove_all(directory);
}

void TestSchemaRefusals() {
    const fs::path directory = Scratch("schemas");
    const std::string database = (directory / "projects.sqlite").string();
    Project base("Refusals");
    Value result;
    std::string error;
    std::string message;
    Check(project_archive::Capture(database, base, {}, result, error, message),
          "capture: " + message);
    {
        Native native(database);
        native.Exec("PRAGMA user_version=999");
    }
    Check(
        !project_archive::Capture(database, base, {}, result, error, message) &&
            error == "ArchiveSchemaMismatch",
        "unknown schema is not overwritten");
    Check(!project_archive::Query(database, "SELECT 1", 1, result, error,
                                  message) &&
              error == "ArchiveSchemaMismatch",
          "unknown schema is not read as current");
    const std::string foreign = (directory / "foreign.sqlite").string();
    {
        Native native(foreign);
        native.Exec("CREATE TABLE unrelated(value TEXT)");
    }
    Check(
        !project_archive::Capture(foreign, base, {}, result, error, message) &&
            error == "ArchiveSchemaMismatch",
        "a foreign database is left alone");
    fs::remove_all(directory);
}

// Every capture of `captures` reads back, through the view, as the bytes
// schema 1 or 2 held, with its own date and revision.
void CheckMigrated(const std::string& database,
                   const std::vector<OldCapture>& captures,
                   const std::string& what) {
    for (const OldCapture& capture : captures) {
        Value result;
        std::string error;
        std::string message;
        const bool read = project_archive::Query(
            database,
            "SELECT document_json,created_at,revision_hash,project_id,step "
            "FROM project_snapshots WHERE snapshot_id='" +
                capture.id + "'",
            1, result, error, message);
        Check(read && Rows(result, 1) &&
                  Cell(result, 0, 0).AsString() == capture.document &&
                  Cell(result, 0, 1).AsString() == capture.created_at &&
                  Cell(result, 0, 2).AsString() ==
                      CanonicalHash(capture.document) &&
                  Cell(result, 0, 3).AsString() == capture.project_id &&
                  Cell(result, 0, 4).AsString() == "save",
              what + ": capture " + capture.id +
                  " reads back to the byte: " + message);
    }
}

void TestMigrationFromTwo() {
    const fs::path directory = Scratch("migrate-two");
    const std::vector<OldCapture> captures = BusyDay(60);
    const std::string database = MakeOldArchive(directory, 2, captures);
    // A run receipt linked to one of them.
    const std::string stepId = GenerateUlid();
    {
        Native native(database);
        Check(native.Exec("INSERT INTO project_run_steps VALUES('" + stepId +
                          "','run','selection','" + captures[3].project_id +
                          "','tl','" + captures[3].id +
                          "','base','result','{\"receipt\":true}')"),
              "fixture receipt");
    }
    const uintmax_t before = fs::file_size(database);
    Value result;
    std::string error;
    std::string message;
    Check(project_archive::Query(database,
                                 "SELECT count(*) FROM project_snapshots", 1,
                                 result, error, message) &&
              Cell(result, 0, 0).Dump() == std::to_string(captures.size()),
          "a reader consults schema 2 as it is");
    {
        Native native(database);
        Check(native.Integer("PRAGMA user_version") == 2,
              "and does not migrate it");
    }
    Project next = MakeInterviewShapedProject(4, 2, 2);
    Check(project_archive::Capture(database, next, {}, result, error, message),
          "a writer migrates, then captures: " + message);
    {
        Native native(database);
        Check(native.Integer("PRAGMA user_version") == 3 &&
                  native.Integer("SELECT count(*) FROM archive_snapshots") ==
                      static_cast<int64_t>(captures.size() + 1) &&
                  native.Text("SELECT type FROM sqlite_schema WHERE "
                              "name='project_snapshots'") == "view",
              "schema 3, every capture, and the view in the table's place");
        Check(native.Integer("SELECT count(*) FROM sqlite_schema WHERE "
                             "type='trigger'") == 10,
              "chunks, snapshots and run steps keep their guards");
        Check(native.Integer("PRAGMA foreign_key_check") == -1,
              "the receipts name captures that exist");
        Check(native.Integer("PRAGMA freelist_count") * 2 <
                  native.Integer("PRAGMA page_count"),
              "the freed pages went back to the disk");
    }
    CheckMigrated(database, captures, "schema 2");
    Check(project_archive::Query(
              database,
              "SELECT r.receipt_json,s.document_json FROM project_run_steps r "
              "JOIN project_snapshots s USING(snapshot_id) WHERE r.step_id='" +
                  stepId + "'",
              1, result, error, message) &&
              Rows(result, 1) &&
              Cell(result, 0, 0).AsString() == "{\"receipt\":true}" &&
              Cell(result, 0, 1).AsString() == captures[3].document,
          "the receipt keeps its link to its capture: " + message);
    const uintmax_t after = fs::file_size(database);
    Check(after * 10 < before,
          "the archive shrinks: " + std::to_string(before) + " -> " +
              std::to_string(after) + " bytes");
    {
        Native native(database);
        Check(!native.Exec("DELETE FROM project_run_steps") &&
                  !native.Exec("INSERT INTO project_run_steps SELECT * FROM "
                               "project_run_steps"),
              "the moved receipts are as immutable as before");
    }
    fs::remove_all(directory);
}

void TestMigrationFromOne() {
    const fs::path directory = Scratch("migrate-one");
    const std::vector<OldCapture> captures = BusyDay(8);
    const std::string database = MakeOldArchive(directory, 1, captures);
    Project next("After schema 1");
    Value result;
    std::string error;
    std::string message;
    Check(
        project_archive::CaptureImport(database, next, result, error, message),
        "an import migrates schema 1 too: " + message);
    {
        Native native(database);
        Check(native.Integer("PRAGMA user_version") == 3 &&
                  native.Integer("SELECT count(*) FROM project_run_steps") == 0,
              "schema 1 becomes schema 3, with its receipts table");
    }
    CheckMigrated(database, captures, "schema 1");
    fs::remove_all(directory);
}

// A process killed in the middle of the migration: SQLite's journal gives
// the archive back as schema 2, whole, and the next writer migrates it.
void TestInterruptedMigration() {
    const fs::path directory = Scratch("interrupted");
    const std::vector<OldCapture> captures = BusyDay(400);
    const std::string database = MakeOldArchive(directory, 2, captures);
    const std::string journal = database + "-journal";
    const pid_t child = fork();
    if (child == 0) {
        Project next("Killed migration");
        Value result;
        std::string error;
        std::string message;
        project_archive::Capture(database, next, {}, result, error, message);
        _exit(0);
    }
    // Wait for the migration's transaction to write, then kill it.
    bool started = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ignored;
        if (fs::exists(journal, ignored) &&
            fs::file_size(journal, ignored) > 0) {
            started = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
    Check(started && WIFSIGNALED(status),
          "the migration was killed while its journal was hot");
    {
        Native native(database);
        Check(native.Integer("PRAGMA user_version") == 2 &&
                  native.Text("PRAGMA integrity_check") == "ok" &&
                  native.Integer("SELECT count(*) FROM project_snapshots") ==
                      static_cast<int64_t>(captures.size()) &&
                  native.Integer("SELECT count(*) FROM sqlite_schema WHERE "
                                 "name LIKE 'archive_%'") == 0,
              "rolled back: schema 2, whole, nothing of schema 3");
    }
    CheckMigrated(database, captures, "after the kill");
    Project next("After the kill");
    Value result;
    std::string error;
    std::string message;
    Check(project_archive::Capture(database, next, {}, result, error, message),
          "the next writer migrates: " + message);
    CheckMigrated(database, captures, "migrated after the kill");
    fs::remove_all(directory);
}

void WriteFile(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
}

std::time_t Day(int year, int month, int day) {
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = 12;
    local.tm_isdst = -1;
    return std::mktime(&local);
}

void TestBackup() {
    const fs::path directory = Scratch("backup");
    const fs::path folder = directory / "OneDrive" / "Copies";
    const std::string database = (directory / "projects.sqlite").string();
    Project project = MakeInterviewShapedProject(10, 3, 4);
    Value result;
    std::string error;
    std::string message;
    Check(
        project_archive::Capture(database, project, {}, result, error, message),
        "capture: " + message);
    const std::time_t today = Day(2026, 10, 24);
    Check(project_archive::BackupFileName(today) ==
              "CUTMACHINE_projects_20261024.sqlite",
          "a copy is named by its local date");

    project_archive::BackupReport report;
    Check(!project_archive::Backup(
              database, "", project_archive::BackupMode::kNow, today, report) &&
              report.error_name == "NotConfigured",
          "no folder, no copy");
    Check(!project_archive::Backup(database, "relative/Copies",
                                   project_archive::BackupMode::kNow, today,
                                   report) &&
              report.error_name == "ValidationFailed",
          "a relative folder is refused");
    Check(!project_archive::Backup(database, folder.string(),
                                   project_archive::BackupMode::kNow, today,
                                   report) &&
              report.error_name == "IoError" && !fs::exists(folder),
          "a missing folder is refused and never created");
    fs::create_directories(folder);
    const project_archive::BackupState before =
        project_archive::InspectBackups(folder.string(), today);
    Check(before.configured && before.latest.empty() && before.due,
          "an empty folder: a copy is due");

    Check(project_archive::Backup(database, folder.string(),
                                  project_archive::BackupMode::kIfDue, today,
                                  report) &&
              report.performed &&
              fs::path(report.path).filename() ==
                  "CUTMACHINE_projects_20261024.sqlite" &&
              report.kept == 1,
          "the day's first copy: " + report.message);
    Check(
        QueryText(report.path, "SELECT document_json FROM project_snapshots") ==
            project.SaveToString(),
        "the copy is an archive the engine reads");
    Check(!fs::exists(folder / ".CUTMACHINE_projects_20261024.sqlite.tmp"),
          "no temporary file is left");
    const auto firstWrite = fs::last_write_time(report.path);
    Check(project_archive::Backup(database, folder.string(),
                                  project_archive::BackupMode::kIfDue, today,
                                  report) &&
              !report.performed &&
              fs::last_write_time(report.path) == firstWrite,
          "at most one automatic copy a day");
    project.timelines[1].name = "Later that day";
    Check(
        project_archive::Capture(database, project, {}, result, error, message),
        "capture later: " + message);
    Check(project_archive::Backup(database, folder.string(),
                                  project_archive::BackupMode::kNow, today,
                                  report) &&
              report.performed,
          "an explicit copy writes today's again");
    Check(QueryText(report.path,
                    "SELECT count(*)||'' FROM project_snapshots") == "2",
          "and holds the later capture");
    const project_archive::BackupState after =
        project_archive::InspectBackups(folder.string(), today);
    Check(after.latest == "CUTMACHINE_projects_20261024.sqlite" && !after.due,
          "status: today's copy, nothing due");

    // A week kept: older dated copies go, oldest first; a later date (a
    // clock set back) and other files stay.
    for (int day = 10; day <= 23; ++day) {
        WriteFile(folder / ("CUTMACHINE_projects_202610" + std::to_string(day) +
                            ".sqlite"),
                  "older");
    }
    WriteFile(folder / "CUTMACHINE_projects_20261231.sqlite", "future");
    WriteFile(folder / "Projets IDAA.resolve_20260829.diskdb", "resolve");
    WriteFile(folder / "CUTMACHINE_projects_2026102.sqlite", "not a date");
    Check(project_archive::Backup(database, folder.string(),
                                  project_archive::BackupMode::kNow, today,
                                  report),
          "copy with older ones around: " + report.message);
    std::vector<std::string> left;
    for (const auto& entry : fs::directory_iterator(folder)) {
        left.push_back(entry.path().filename().string());
    }
    std::sort(left.begin(), left.end());
    std::vector<std::string> expected = {
        "CUTMACHINE_projects_2026102.sqlite",
        "CUTMACHINE_projects_20261019.sqlite",
        "CUTMACHINE_projects_20261020.sqlite",
        "CUTMACHINE_projects_20261021.sqlite",
        "CUTMACHINE_projects_20261022.sqlite",
        "CUTMACHINE_projects_20261023.sqlite",
        "CUTMACHINE_projects_20261024.sqlite",
        "CUTMACHINE_projects_20261231.sqlite",
        "Projets IDAA.resolve_20260829.diskdb"};
    std::sort(expected.begin(), expected.end());
    std::string listing;
    for (const std::string& name : left) listing += name + " ";
    Check(left == expected && report.removed.size() == 9 &&
              report.removed.front() == "CUTMACHINE_projects_20261018.sqlite",
          "seven dated copies kept, the newest, and nothing else touched: " +
              listing);

    // A copy in progress elsewhere, then one abandoned an hour ago.
    const fs::path temporary =
        folder / ".CUTMACHINE_projects_20261025.sqlite.tmp";
    WriteFile(temporary, "");
    const std::time_t tomorrow = Day(2026, 10, 25);
    Check(project_archive::Backup(database, folder.string(),
                                  project_archive::BackupMode::kIfDue, tomorrow,
                                  report) &&
              !report.performed && fs::exists(temporary) &&
              !fs::exists(folder / "CUTMACHINE_projects_20261025.sqlite"),
          "another process copying: nothing written");
    fs::last_write_time(
        temporary, fs::file_time_type::clock::now() - std::chrono::hours(2));
    Check(project_archive::Backup(database, folder.string(),
                                  project_archive::BackupMode::kIfDue, tomorrow,
                                  report) &&
              report.performed && !fs::exists(temporary),
          "an abandoned temporary file is replaced: " + report.message);

    // One state of the archive, whatever writers do during the copy.
    std::atomic_bool stop{false};
    std::atomic_int written{0};
    std::thread writer([&] {
        Project busy = project;
        while (!stop) {
            busy.timelines[0].name = "Busy " + std::to_string(written.load());
            Value answer;
            std::string name;
            std::string detail;
            if (project_archive::Capture(database, busy, {}, answer, name,
                                         detail)) {
                ++written;
            }
        }
    });
    bool consistent = true;
    for (int round = 0; round < 5; ++round) {
        if (!project_archive::Backup(database, folder.string(),
                                     project_archive::BackupMode::kNow,
                                     tomorrow, report) ||
            !report.performed) {
            consistent = false;
            continue;
        }
        Value rows;
        consistent = consistent &&
                     project_archive::Query(
                         report.path,
                         "SELECT count(*), sum(length(document_json)) FROM "
                         "project_snapshots",
                         1, rows, error, message) &&
                     Rows(rows, 1) &&
                     Native(report.path).Text("PRAGMA integrity_check") == "ok";
    }
    stop = true;
    writer.join();
    Check(consistent && written > 0,
          "copies taken while an agent writes read back whole (" +
              std::to_string(written.load()) +
              " captures meanwhile): " + message);

    // A schema 2 archive is migrated before it is copied.
    const fs::path old = Scratch("backup-old");
    const std::vector<OldCapture> captures = BusyDay(5);
    const std::string oldDatabase = MakeOldArchive(old, 2, captures);
    Check(project_archive::Backup(oldDatabase, folder.string(),
                                  project_archive::BackupMode::kNow, tomorrow,
                                  report),
          "copy of a schema 2 archive: " + report.message);
    Check(Native(report.path).Integer("PRAGMA user_version") == 3 &&
              Native(oldDatabase).Integer("PRAGMA user_version") == 3,
          "the archive, then its copy, are schema 3");
    CheckMigrated(report.path, captures, "the copy of a migrated archive");
    fs::remove_all(old);
    fs::remove_all(directory);
}

}  // namespace

int main() {
    Test("capture, query and immutability", TestCaptureAndQuery);
    Test("schema refusals", TestSchemaRefusals);
    Test("migration from schema 2", TestMigrationFromTwo);
    Test("migration from schema 1", TestMigrationFromOne);
    Test("a migration killed midway", TestInterruptedMigration);
    Test("the daily copy", TestBackup);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
