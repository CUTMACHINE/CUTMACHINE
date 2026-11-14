#include "MotionLibraryBase.h"

#include "CanonicalHash.h"
#include "LocalEnv.h"
#include "UserDirectories.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;

// 'CMLB': not the project archive's 'CMDB', so neither opens the other.
constexpr int kApplicationId = 0x434d4c42;
constexpr int kSchemaVersion = 1;
// A piece is a few kilobytes; past this a file is not one (as
// MotionTemplates.cc reads the folder).
constexpr uintmax_t kMaxFileBytes = uintmax_t{16} * 1024 * 1024;
constexpr int kMaxRowBytes = 16 * 1024 * 1024;

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool Fail(sqlite3* db, std::string& error) {
    error = db != nullptr ? sqlite3_errmsg(db) : "cannot open the library";
    return false;
}

bool Execute(sqlite3* db, const char* sql, std::string& error) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK ||
           Fail(db, error);
}

bool Prepare(sqlite3* db, const char* sql, Statement& statement,
             std::string& error) {
    sqlite3_stmt* raw = nullptr;
    const int code = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    statement.reset(raw);
    return code == SQLITE_OK || Fail(db, error);
}

bool Scalar(sqlite3* db, const char* sql, int& value, std::string& error) {
    Statement statement(nullptr, sqlite3_finalize);
    if (!Prepare(db, sql, statement, error)) {
        return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Fail(db, error);
    }
    value = sqlite3_column_int(statement.get(), 0);
    return true;
}

void Bind(sqlite3_stmt* statement, int index, const std::string& text) {
    sqlite3_bind_text(statement, index, text.data(),
                      static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

std::string Column(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    return text != nullptr
               ? std::string(reinterpret_cast<const char*>(text),
                             static_cast<size_t>(
                                 sqlite3_column_bytes(statement, index)))
               : std::string();
}

// The base, created with its schema on first use. Versions never change
// once written: the triggers refuse an update and a delete.
bool Open(const std::string& path, Database& db, std::string& error) {
    if (path.empty()) {
        error = "no home directory: the library has no base";
        return false;
    }
    std::error_code ignored;
    fs::create_directories(fs::path(path).parent_path(), ignored);
    sqlite3* raw = nullptr;
    const int code = sqlite3_open_v2(
        path.c_str(), &raw,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    db.reset(raw);
    if (code != SQLITE_OK) {
        return Fail(raw, error);
    }
    sqlite3_busy_timeout(raw, 5000);
    sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, kMaxRowBytes);
    sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    if (!Execute(raw,
                 "PRAGMA trusted_schema=OFF; PRAGMA fullfsync=ON; "
                 "PRAGMA synchronous=FULL",
                 error)) {
        return false;
    }
    int application = 0;
    int version = 0;
    int tables = 0;
    if (!Scalar(raw, "PRAGMA application_id", application, error) ||
        !Scalar(raw, "PRAGMA user_version", version, error) ||
        !Scalar(raw, "SELECT count(*) FROM sqlite_schema", tables, error)) {
        return false;
    }
    if (application == kApplicationId && version == kSchemaVersion) {
        return true;
    }
    if (application != 0 || version != 0 || tables != 0) {
        error = "'" + path + "' is not a CUTMACHINE motion library";
        return false;
    }
    return Execute(raw,
                   "BEGIN IMMEDIATE;"
                   "CREATE TABLE motion_library ("
                   "kind TEXT NOT NULL CHECK(kind IN ('piece','kit')),"
                   "name TEXT NOT NULL,"
                   "version INTEGER NOT NULL CHECK(version >= 1),"
                   "content_json TEXT NOT NULL CHECK(json_valid(content_json)),"
                   "content_hash TEXT NOT NULL,"
                   "source TEXT NOT NULL,"
                   "created_at TEXT NOT NULL DEFAULT "
                   "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
                   "PRIMARY KEY (kind, name, version));"
                   "CREATE TRIGGER library_no_update BEFORE UPDATE ON "
                   "motion_library BEGIN SELECT "
                   "RAISE(ABORT,'library versions are immutable'); END;"
                   "CREATE TRIGGER library_no_delete BEFORE DELETE ON "
                   "motion_library BEGIN SELECT "
                   "RAISE(ABORT,'library versions are immutable'); END;"
                   "PRAGMA application_id=1129139266; PRAGMA user_version=1;"
                   "COMMIT;",
                   error);
}

// The highest version of a name, 0 when there is none; and whether one of
// its versions has exactly `hash`.
bool Versions(sqlite3* db, const std::string& kind, const std::string& name,
              const std::string& hash, int64_t& latest, bool& seen,
              std::string& error) {
    Statement statement(nullptr, sqlite3_finalize);
    if (!Prepare(db,
                 "SELECT coalesce(max(version),0), "
                 "coalesce(max(content_hash = ?3),0) FROM motion_library "
                 "WHERE kind = ?1 AND name = ?2",
                 statement, error)) {
        return false;
    }
    Bind(statement.get(), 1, kind);
    Bind(statement.get(), 2, name);
    Bind(statement.get(), 3, hash);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Fail(db, error);
    }
    latest = sqlite3_column_int64(statement.get(), 0);
    seen = sqlite3_column_int(statement.get(), 1) != 0;
    return true;
}

bool Insert(sqlite3* db, const std::string& kind, const std::string& name,
            int64_t version, const std::string& content,
            const std::string& source, std::string& error) {
    Statement statement(nullptr, sqlite3_finalize);
    if (!Prepare(db,
                 "INSERT INTO motion_library "
                 "(kind, name, version, content_json, content_hash, source) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                 statement, error)) {
        return false;
    }
    Bind(statement.get(), 1, kind);
    Bind(statement.get(), 2, name);
    sqlite3_bind_int64(statement.get(), 3, version);
    Bind(statement.get(), 4, content);
    Bind(statement.get(), 5, CanonicalHash(content));
    Bind(statement.get(), 6, source);
    return sqlite3_step(statement.get()) == SQLITE_DONE || Fail(db, error);
}

// A file of the inbox, read whole and judged by the reader the shelf uses.
struct InboxFile {
    std::string kind;
    std::string name;
    std::string path;
    std::string content;
};

std::vector<fs::path> JsonFiles(const fs::path& directory) {
    std::vector<fs::path> files;
    std::error_code failure;
    for (fs::directory_iterator entry(directory, failure), end;
         !failure && entry != end; entry.increment(failure)) {
        const fs::path& path = entry->path();
        const std::string filename = path.filename().string();
        std::error_code ignored;
        if (filename.empty() || filename.front() == '.' ||
            path.extension() != ".json" ||
            !fs::is_regular_file(path, ignored)) {
            continue;
        }
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    return files;
}

void ReadInbox(const std::string& inbox,
               const std::vector<MotionTemplateInfo>& shipped,
               std::vector<InboxFile>& files,
               std::vector<MotionLibraryIssue>& issues) {
    if (inbox.empty()) {
        return;
    }
    for (const char* kind : {"piece", "kit"}) {
        const bool piece = std::string(kind) == "piece";
        const fs::path directory =
            piece ? fs::path(inbox) : fs::path(inbox) / "kits";
        for (const fs::path& path : JsonFiles(directory)) {
            const std::string name = path.stem().string();
            const auto issue = [&](std::string message) {
                issues.push_back({path.string(), std::move(message)});
            };
            if (!IsMotionPieceName(name)) {
                issue("'" + name + "' is not a " + kind +
                      " name: letters, digits, '-' and '_'");
                continue;
            }
            if (piece && std::any_of(shipped.begin(), shipped.end(),
                                     [&](const MotionTemplateInfo& info) {
                                         return info.name == name;
                                     })) {
                issue("'" + name +
                      "' is the name of a template CUTMACHINE ships: rename "
                      "the file, a library piece never replaces a shipped "
                      "one");
                continue;
            }
            std::error_code sizeFailure;
            const uintmax_t size = fs::file_size(path, sizeFailure);
            if (sizeFailure || size > kMaxFileBytes) {
                issue(sizeFailure
                          ? "unreadable: " + sizeFailure.message()
                          : std::string("larger than a ") + kind + " can be");
                continue;
            }
            std::ifstream stream(path, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
            if (!stream.good() && !stream.eof()) {
                issue("unreadable");
                continue;
            }
            std::string reason;
            MotionTemplateInfo pieceInfo;
            MotionKitInfo kitInfo;
            if (piece ? !ParseMotionPiece(name, content, pieceInfo, reason)
                      : !ParseMotionKit(name, content, kitInfo, reason)) {
                issue(reason);
                continue;
            }
            files.push_back({kind, name, path.string(), std::move(content)});
        }
    }
}

// The inbox's files the base does not hold yet, each as the next version of
// its name, in one transaction: two servers reading at once import once.
bool Import(sqlite3* db, const std::vector<InboxFile>& files,
            std::vector<MotionLibraryIssue>& imported, std::string& error) {
    if (files.empty()) {
        return true;
    }
    if (!Execute(db, "BEGIN IMMEDIATE", error)) {
        return false;
    }
    std::vector<MotionLibraryIssue> done;
    for (const InboxFile& file : files) {
        int64_t latest = 0;
        bool seen = false;
        if (!Versions(db, file.kind, file.name, CanonicalHash(file.content),
                      latest, seen, error) ||
            (!seen && !Insert(db, file.kind, file.name, latest + 1,
                              file.content, file.path, error))) {
            std::string ignored;
            Execute(db, "ROLLBACK", ignored);
            return false;
        }
        if (!seen) {
            done.push_back({file.path, file.kind + " " + file.name + " v" +
                                           std::to_string(latest + 1)});
        }
    }
    if (!Execute(db, "COMMIT", error)) {
        std::string ignored;
        Execute(db, "ROLLBACK", ignored);
        return false;
    }
    std::move(done.begin(), done.end(), std::back_inserter(imported));
    return true;
}

}  // namespace

MotionLibraryLocation CurrentMotionLibraryLocation() {
    MotionLibraryLocation location;
    const std::string configured =
        local_env::Value("CUTMACHINE_MOTION_LIBRARY");
    if (!configured.empty()) {
        location.inbox = configured;
        location.base = (fs::path(configured) / "library.sqlite").string();
        return location;
    }
    const std::string home = user_directories::Home();
    if (home.empty()) {
        return location;
    }
    location.inbox =
        (fs::path(home) / ".local/share/cutmachine/motion-library").string();
    // HEADLESS-2026-10 H4 -- Application Support on macOS
    // (UserDirectories.h).
    location.base = (fs::path(user_directories::ApplicationData(
                         home, user_directories::kThisSystem)) /
                     "library.sqlite")
                        .string();
    return location;
}

void ReadMotionLibraryBase(const MotionLibraryLocation& location,
                           const std::vector<MotionTemplateInfo>& shipped,
                           MotionLibraryContents& contents) {
    contents = MotionLibraryContents{};
    std::vector<InboxFile> files;
    ReadInbox(location.inbox, shipped, files, contents.issues);
    // Reading an empty library leaves no base behind.
    std::error_code absent;
    if (files.empty() && !location.base.empty() &&
        !fs::exists(location.base, absent)) {
        return;
    }
    Database db(nullptr, sqlite3_close);
    std::string error;
    if (Open(location.base, db, error) &&
        !Import(db.get(), files, contents.imported, error)) {
        // What the base holds is still listed; the files wait for the next
        // read.
        contents.issues.push_back(
            {location.inbox, "the inbox could not be imported: " + error});
    } else if (!db || !error.empty()) {
        // The shelf still lists what the inbox holds, unversioned.
        contents.issues.push_back(
            {location.base, "the library base cannot be used: " + error});
        for (const InboxFile& file : files) {
            if (file.kind == "piece") {
                MotionTemplateInfo piece;
                ParseMotionPiece(file.name, file.content, piece, error);
                piece.path = file.path;
                contents.pieces.push_back(std::move(piece));
            } else {
                MotionKitInfo kit;
                ParseMotionKit(file.name, file.content, kit, error);
                kit.path = file.path;
                contents.kits.push_back(std::move(kit));
            }
        }
        return;
    }
    Statement statement(nullptr, sqlite3_finalize);
    if (!Prepare(db.get(),
                 "SELECT kind, name, version, content_json FROM motion_library "
                 "AS row WHERE version = (SELECT max(version) FROM "
                 "motion_library WHERE kind = row.kind AND name = row.name) "
                 "ORDER BY kind, name",
                 statement, error)) {
        contents.issues.push_back({location.base, error});
        return;
    }
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const std::string kind = Column(statement.get(), 0);
        const std::string name = Column(statement.get(), 1);
        const int64_t version = sqlite3_column_int64(statement.get(), 2);
        const std::string content = Column(statement.get(), 3);
        std::string where = location.base;
        where += " (";
        where += kind;
        where += ' ';
        where += name;
        where += " v";
        where += std::to_string(version);
        where += ')';
        std::string reason;
        if (kind == "piece") {
            MotionTemplateInfo piece;
            if (std::any_of(shipped.begin(), shipped.end(),
                            [&](const MotionTemplateInfo& info) {
                                return info.name == name;
                            })) {
                contents.issues.push_back(
                    {where, "the name of a template CUTMACHINE ships"});
            } else if (!ParseMotionPiece(name, content, piece, reason)) {
                contents.issues.push_back({where, reason});
            } else {
                piece.version = version;
                contents.pieces.push_back(std::move(piece));
            }
        } else {
            MotionKitInfo kit;
            if (!ParseMotionKit(name, content, kit, reason)) {
                contents.issues.push_back({where, reason});
            } else {
                kit.version = version;
                contents.kits.push_back(std::move(kit));
            }
        }
    }
}

bool FindMotionLibraryKit(const MotionLibraryLocation& location,
                          const std::string& name, MotionKitInfo& kit,
                          std::string& error) {
    std::vector<MotionTemplateInfo> shipped;
    if (!MotionTemplateCatalog(shipped, error)) {
        return false;
    }
    MotionLibraryContents contents;
    ReadMotionLibraryBase(location, shipped, contents);
    std::string known;
    for (const MotionKitInfo& candidate : contents.kits) {
        if (candidate.name == name) {
            kit = candidate;
            return true;
        }
        known += (known.empty() ? "" : ", ") + candidate.name;
    }
    // A file of that name that is not a kit is the likelier reason than a
    // typo: say what is wrong with it.
    for (const MotionLibraryIssue& issue : contents.issues) {
        if (fs::path(issue.path).stem() == name) {
            known += "; " + issue.path + " is not a kit: " + issue.message;
        }
    }
    error = "unknown kit '" + name + "' in " + location.base +
            "; known: " + (known.empty() ? "none" : known);
    return false;
}

bool SaveMotionPieceVersion(const MotionLibraryLocation& location,
                            const MotionTemplateInfo& piece, bool overwrite,
                            int64_t& version, bool& existed,
                            std::string& error) {
    version = 0;
    existed = false;
    Database db(nullptr, sqlite3_close);
    if (!Open(location.base, db, error) ||
        !Execute(db.get(), "BEGIN IMMEDIATE", error)) {
        return false;
    }
    const std::string content = DumpMotionPiece(piece);
    int64_t latest = 0;
    bool seen = false;
    std::string ignored;
    if (!Versions(db.get(), "piece", piece.name, CanonicalHash(content), latest,
                  seen, error)) {
        Execute(db.get(), "ROLLBACK", ignored);
        return false;
    }
    existed = latest > 0;
    if (existed && !overwrite) {
        Execute(db.get(), "ROLLBACK", ignored);
        error = "a piece named '" + piece.name + "' is in the library (v" +
                std::to_string(latest) +
                "): pass overwrite to save the next version, the others are "
                "kept";
        return false;
    }
    if (!Insert(db.get(), "piece", piece.name, latest + 1, content,
                "save_motion_template", error) ||
        !Execute(db.get(), "COMMIT", error)) {
        Execute(db.get(), "ROLLBACK", ignored);
        return false;
    }
    version = latest + 1;
    return true;
}
