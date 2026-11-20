#include "MotionLibraryBase.h"

#include "CanonicalHash.h"
#include "LocalEnv.h"
#include "UserDirectories.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;

using mcp_json::Value;

// 'CMLB': not the project archive's 'CMDB', so neither opens the other.
constexpr int kApplicationId = 0x434d4c42;
constexpr int kSchemaVersion = 1;
// A piece is a few kilobytes; past this a file is not one (as
// MotionTemplates.cc reads the folder).
constexpr uintmax_t kMaxFileBytes = uintmax_t{16} * 1024 * 1024;
constexpr int kMaxRowBytes = 16 * 1024 * 1024;
// A kit's temporary file older than this was left by a write that never
// finished (WriteKitFile).
constexpr time_t kStaleTemporarySeconds = 60 * 60;

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

// CH3 -- a BEGIN IMMEDIATE that rolls back unless it is committed: every
// way out of a kit write that is not the end leaves the base as it was.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() { Rollback(); }

    bool Begin(std::string& error) {
        open_ = Execute(db_, "BEGIN IMMEDIATE", error);
        return open_;
    }

    bool Commit(std::string& error) {
        if (!Execute(db_, "COMMIT", error)) {
            return false;
        }
        open_ = false;
        return true;
    }

    void Rollback() {
        if (open_) {
            std::string ignored;
            Execute(db_, "ROLLBACK", ignored);
            open_ = false;
        }
    }

private:
    sqlite3* db_;
    bool open_ = false;
};

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

// CH3 -- one version of a kit, as the base holds it; version 0 when the
// base holds none.
bool KitVersionRow(sqlite3* db, const std::string& name, int64_t version,
                   MotionKitVersion& row, std::string& error) {
    row = MotionKitVersion{};
    Statement statement(nullptr, sqlite3_finalize);
    // version 0 asks for the latest.
    if (!Prepare(db,
                 "SELECT version, content_json, source, created_at FROM "
                 "motion_library WHERE kind = 'kit' AND name = ?1 AND "
                 "(?2 = 0 OR version = ?2) ORDER BY version DESC LIMIT 1",
                 statement, error)) {
        return false;
    }
    Bind(statement.get(), 1, name);
    sqlite3_bind_int64(statement.get(), 2, version);
    const int code = sqlite3_step(statement.get());
    if (code == SQLITE_DONE) {
        return true;
    }
    if (code != SQLITE_ROW) {
        return Fail(db, error);
    }
    row.version = sqlite3_column_int64(statement.get(), 0);
    row.content = Column(statement.get(), 1);
    row.source = Column(statement.get(), 2);
    row.created_at = Column(statement.get(), 3);
    return true;
}

// ParseMotionKit's reason for refusing a stored version, empty when it
// reads under this binary.
std::string KitIssue(const std::string& name, const std::string& content) {
    MotionKitInfo kit;
    std::string reason;
    return ParseMotionKit(name, content, kit, reason) ? std::string() : reason;
}

std::string KitFilePath(const std::string& inbox, const std::string& name) {
    return inbox.empty()
               ? std::string()
               : (fs::path(MotionKitDirectory(inbox)) / (name + ".json"))
                     .string();
}

// Kit names are ASCII (IsMotionPieceName): folding them is ASCII's.
std::string FoldCase(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// The entries of kits/ as the folder spells them, whatever they are.
std::vector<std::string> KitDirectoryEntries(const std::string& inbox) {
    std::vector<std::string> entries;
    if (inbox.empty()) {
        return entries;
    }
    std::error_code failure;
    for (fs::directory_iterator entry(MotionKitDirectory(inbox), failure), end;
         !failure && entry != end; entry.increment(failure)) {
        entries.push_back(entry->path().filename().string());
    }
    return entries;
}

// CH3 (review) -- whether kits/ has an entry spelled exactly <name>.json,
// byte for byte. A disk that ignores case (macOS's by default) opens
// kits/marque.json for kits/Marque.json: rule R only reads, and a write
// only renames over, the file of that very name.
bool KitFileEntryExists(const std::string& inbox, const std::string& name) {
    const std::vector<std::string> entries = KitDirectoryEntries(inbox);
    return std::find(entries.begin(), entries.end(), name + ".json") !=
           entries.end();
}

// CH3 (review) -- another kit that `name` would share a file with on a disk
// that ignores case: a name of the base, or an entry of kits/, equal to it
// once folded but spelled otherwise. `other` receives it, empty when there
// is none. False only when the base fails.
bool KitNameCollision(sqlite3* db, const std::string& inbox,
                      const std::string& name, std::string& other,
                      std::string& error) {
    other.clear();
    Statement statement(nullptr, sqlite3_finalize);
    if (!Prepare(db,
                 "SELECT name FROM motion_library WHERE kind = 'kit' AND "
                 "lower(name) = lower(?1) AND name <> ?1 LIMIT 1",
                 statement, error)) {
        return false;
    }
    Bind(statement.get(), 1, name);
    const int code = sqlite3_step(statement.get());
    if (code == SQLITE_ROW) {
        other = "the kit '" + Column(statement.get(), 0) + "' of the library";
        return true;
    }
    if (code != SQLITE_DONE) {
        return Fail(db, error);
    }
    const std::string exact = name + ".json";
    const std::string folded = FoldCase(exact);
    for (const std::string& entry : KitDirectoryEntries(inbox)) {
        if (entry != exact && FoldCase(entry) == folded) {
            other = (fs::path(MotionKitDirectory(inbox)) / entry).string();
            return true;
        }
    }
    return true;
}

// What reading kits/<name>.json gave: nothing there, its bytes, or a reason
// it is not a file a charter can be.
enum class KitFileRead : uint8_t { Absent, Read, Refused };

// Without following a link, and without blocking on a pipe: rule R's steps
// 1 and 2 (MotionLibraryBase.h).
KitFileRead ReadKitFileBytes(const std::string& path, std::string& content,
                             std::string& reason) {
    content.clear();
    reason.clear();
    const auto failed = [&](const char* what) {
        reason = std::string(what) + ": " + std::strerror(errno);
        return KitFileRead::Refused;
    };
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return errno == ENOENT || errno == ENOTDIR ? KitFileRead::Absent
                                                   : failed("unreadable");
    }
    if (S_ISLNK(info.st_mode)) {
        reason =
            "not a regular file but a symbolic link: the engine replaces the "
            "file by a rename, which would replace the link and not its "
            "target";
        return KitFileRead::Refused;
    }
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        return errno == ENOENT ? KitFileRead::Absent : failed("unreadable");
    }
    if (::fstat(descriptor, &info) != 0) {
        const KitFileRead read = failed("unreadable");
        ::close(descriptor);
        return read;
    }
    if (!S_ISREG(info.st_mode) ||
        static_cast<uintmax_t>(info.st_size) > kMaxFileBytes) {
        ::close(descriptor);
        reason = S_ISREG(info.st_mode) ? "larger than a kit can be"
                                       : "not a regular file";
        return KitFileRead::Refused;
    }
    char buffer[65536];
    while (true) {
        const ssize_t got = ::read(descriptor, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            const KitFileRead read = failed("unreadable");
            ::close(descriptor);
            return read;
        }
        if (got == 0) {
            break;
        }
        content.append(buffer, static_cast<size_t>(got));
        if (content.size() > kMaxFileBytes) {
            ::close(descriptor);
            content.clear();
            reason = "larger than a kit can be";
            return KitFileRead::Refused;
        }
    }
    ::close(descriptor);
    return KitFileRead::Read;
}

// Rule R (MotionLibraryBase.h) on kits/<name>.json, inside the caller's
// transaction: `latest` receives the latest version after it. False only
// when the base fails.
bool ImportKitFile(sqlite3* db, const std::string& inbox,
                   const std::string& name, MotionKitFileState& state,
                   std::string& issue, MotionKitVersion& latest,
                   std::vector<MotionLibraryIssue>& imported,
                   std::string& error) {
    state = MotionKitFileState::Absent;
    issue.clear();
    if (!KitVersionRow(db, name, 0, latest, error)) {
        return false;
    }
    const std::string path = KitFilePath(inbox, name);
    // Step 1: no entry of that exact name is no file, even where the disk
    // would open another kit's under it.
    if (path.empty() || !KitFileEntryExists(inbox, name)) {
        return true;
    }
    std::string content;
    switch (ReadKitFileBytes(path, content, issue)) {
        case KitFileRead::Absent:
            return true;
        case KitFileRead::Refused:
            state = MotionKitFileState::Diverged;
            return true;
        case KitFileRead::Read:
            break;
    }
    if (latest.version > 0 && content == latest.content) {
        state = MotionKitFileState::Matches;
        return true;
    }
    MotionKitInfo kit;
    if (!ParseMotionKit(name, content, kit, issue)) {
        state = MotionKitFileState::Diverged;
        return true;
    }
    const int64_t next = latest.version + 1;
    if (!Insert(db, "kit", name, next, content, path, error) ||
        !KitVersionRow(db, name, 0, latest, error)) {
        return false;
    }
    imported.push_back({path, "kit " + name + " v" + std::to_string(next)});
    state = MotionKitFileState::Matches;
    return true;
}

// CH3 (review) -- what a write cut short between its temporary file and the
// rename leaves behind (a crash, a kill): `.<name>.json.tmp-*` of this kit
// only -- the names allow no '.', so no other kit's prefix is this one --
// and only once an hour old, so that another process's write in progress
// is never taken from it. A regular file only; a link is left alone.
void RemoveStaleKitTemporaries(const fs::path& directory,
                               const std::string& name) {
    const std::string prefix = "." + name + ".json.tmp-";
    const time_t limit = ::time(nullptr) - kStaleTemporarySeconds;
    std::vector<fs::path> stale;
    std::error_code failure;
    for (fs::directory_iterator entry(directory, failure), end;
         !failure && entry != end; entry.increment(failure)) {
        const std::string filename = entry->path().filename().string();
        struct stat info {};
        if (filename.compare(0, prefix.size(), prefix) == 0 &&
            ::lstat(entry->path().c_str(), &info) == 0 &&
            S_ISREG(info.st_mode) && info.st_mtime < limit) {
            stale.push_back(entry->path());
        }
    }
    for (const fs::path& path : stale) {
        ::unlink(path.c_str());
    }
}

// Writes `content` as kits/<name>.json: a temporary file beside it, synced,
// renamed over it. Never leaves the temporary file behind. The file keeps
// the permissions it had (a rename would give it the temporary file's);
// a hard link to it is not kept, since the rename puts another file under
// the name.
bool WriteKitFile(const std::string& inbox, const std::string& name,
                  const std::string& content, std::string& error) {
    static std::atomic<uint64_t> sequence{0};
    const fs::path directory = MotionKitDirectory(inbox);
    std::error_code failure;
    fs::create_directories(directory, failure);
    if (failure) {
        error =
            "unable to create " + directory.string() + ": " + failure.message();
        return false;
    }
    RemoveStaleKitTemporaries(directory, name);
    const fs::path target = directory / (name + ".json");
    const fs::path temporary =
        directory / ("." + name + ".json.tmp-" + std::to_string(::getpid()) +
                     "-" + std::to_string(++sequence));
    const auto failed = [&](const std::string& what) {
        error = what + " " + temporary.string() + ": " + std::strerror(errno);
        return false;
    };
    // R found the file a regular one (or none): its mode, without following
    // anything.
    struct stat previous {};
    const bool kept =
        ::lstat(target.c_str(), &previous) == 0 && S_ISREG(previous.st_mode);
    const int descriptor =
        ::open(temporary.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return failed("unable to create");
    }
    bool written = true;
    if (kept && ::fchmod(descriptor, previous.st_mode & 0777) != 0) {
        written = failed("unable to set the mode of");
    }
    size_t offset = 0;
    while (written && offset < content.size()) {
        const ssize_t put = ::write(descriptor, content.data() + offset,
                                    content.size() - offset);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            written = failed("unable to write");
            break;
        }
        offset += static_cast<size_t>(put);
    }
    // The base syncs with F_FULLFSYNC (PRAGMA fullfsync); the file does
    // the same where the system tells the two apart.
#if defined(__APPLE__)
    if (written && ::fcntl(descriptor, F_FULLFSYNC) != 0 &&
        ::fsync(descriptor) != 0) {
        written = failed("unable to sync");
    }
#else
    if (written && ::fsync(descriptor) != 0) {
        written = failed("unable to sync");
    }
#endif
    if (::close(descriptor) != 0 && written) {
        written = failed("unable to close");
    }
    if (written && ::rename(temporary.c_str(), target.c_str()) != 0) {
        error = "unable to replace " + target.string() + ": " +
                std::strerror(errno);
        written = false;
    }
    if (!written) {
        ::unlink(temporary.c_str());
        return false;
    }
    // The rename itself, durable: best effort, the file is whole either way.
    const int folder = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (folder >= 0) {
        ::fsync(folder);
        ::close(folder);
    }
    return true;
}

// The entries of kits/ that may be kit files: `<name>.json`, not hidden,
// anything but a folder (a link or a pipe is one R finds diverged), sorted.
std::vector<fs::path> KitFiles(const fs::path& directory) {
    std::vector<fs::path> files;
    std::error_code failure;
    for (fs::directory_iterator entry(directory, failure), end;
         !failure && entry != end; entry.increment(failure)) {
        const fs::path& path = entry->path();
        const std::string filename = path.filename().string();
        std::error_code ignored;
        if (filename.empty() || filename.front() == '.' ||
            path.extension() != ".json" ||
            entry->symlink_status(ignored).type() == fs::file_type::directory) {
            continue;
        }
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The kit names of kits/ (IsMotionPieceName), sorted.
std::vector<std::string> KitFileNames(const std::string& inbox) {
    std::vector<std::string> names;
    if (inbox.empty()) {
        return names;
    }
    for (const fs::path& path : KitFiles(MotionKitDirectory(inbox))) {
        if (IsMotionPieceName(path.stem().string())) {
            names.push_back(path.stem().string());
        }
    }
    return names;
}

// A file of the inbox, read whole and judged by the reader the shelf uses.
struct InboxFile {
    std::string kind;
    std::string name;
    std::string path;
    std::string content;
};

// CH3 -- a kit file of the inbox, read before the base is locked: to know
// whether there is anything to import, and for the shelf when the base
// cannot be used. Rule R reads it again under the lock.
struct InboxKit {
    std::string name;
    std::string path;
    std::string content;
    // ParseMotionKit's (or the file's) reason, empty when it reads.
    std::string reason;
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
               std::vector<InboxFile>& files, std::vector<InboxKit>& kits,
               std::vector<MotionLibraryIssue>& issues) {
    if (inbox.empty()) {
        return;
    }
    for (const fs::path& path : JsonFiles(inbox)) {
        const std::string name = path.stem().string();
        const auto issue = [&](std::string message) {
            issues.push_back({path.string(), std::move(message)});
        };
        if (!IsMotionPieceName(name)) {
            issue("'" + name +
                  "' is not a piece name: letters, digits, '-' and '_'");
            continue;
        }
        if (std::any_of(shipped.begin(), shipped.end(),
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
            issue(sizeFailure ? "unreadable: " + sizeFailure.message()
                              : std::string("larger than a piece can be"));
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
        if (!ParseMotionPiece(name, content, pieceInfo, reason)) {
            issue(reason);
            continue;
        }
        files.push_back({"piece", name, path.string(), std::move(content)});
    }
    for (const fs::path& path : KitFiles(MotionKitDirectory(inbox))) {
        const std::string name = path.stem().string();
        if (!IsMotionPieceName(name)) {
            issues.push_back(
                {path.string(), "'" + name +
                                    "' is not a kit name: letters, digits, '-' "
                                    "and '_'"});
            continue;
        }
        InboxKit kit{name, path.string(), std::string(), std::string()};
        const KitFileRead read =
            ReadKitFileBytes(kit.path, kit.content, kit.reason);
        if (read == KitFileRead::Absent) {
            continue;
        }
        MotionKitInfo parsed;
        if (read == KitFileRead::Read &&
            !ParseMotionKit(name, kit.content, parsed, kit.reason)) {
            kit.content.clear();
        }
        kits.push_back(std::move(kit));
    }
}

// The inbox's piece files the base does not hold yet, each as the next
// version of its name (MB10), and its kit files by rule R (CH3), in one
// transaction: two servers reading at once import once. A kit file R finds
// diverged lands in `diverged`.
bool Import(sqlite3* db, const std::string& inbox,
            const std::vector<InboxFile>& files,
            const std::vector<InboxKit>& kits,
            std::vector<MotionLibraryIssue>& imported,
            std::vector<MotionLibraryIssue>& diverged, std::string& error) {
    if (files.empty() && kits.empty()) {
        return true;
    }
    Transaction transaction(db);
    if (!transaction.Begin(error)) {
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
            return false;
        }
        if (!seen) {
            done.push_back({file.path, file.kind + " " + file.name + " v" +
                                           std::to_string(latest + 1)});
        }
    }
    std::vector<MotionLibraryIssue> refused;
    for (const InboxKit& kit : kits) {
        MotionKitFileState state = MotionKitFileState::Absent;
        std::string issue;
        MotionKitVersion latest;
        if (!ImportKitFile(db, inbox, kit.name, state, issue, latest, done,
                           error)) {
            return false;
        }
        if (state == MotionKitFileState::Diverged) {
            refused.push_back({kit.path, issue});
        }
    }
    if (!transaction.Commit(error)) {
        return false;
    }
    std::move(done.begin(), done.end(), std::back_inserter(imported));
    std::move(refused.begin(), refused.end(), std::back_inserter(diverged));
    return true;
}

bool KitError(MotionKitError& error, const char* code, std::string path,
              std::string message) {
    error = MotionKitError{code, std::move(path), std::move(message)};
    return false;
}

// Whether the base exists, so that reading a library that has none does
// not leave one behind.
bool BaseExists(const std::string& base) {
    std::error_code absent;
    return !base.empty() && fs::exists(base, absent);
}

// The new tree of a kit write, given the latest version R left (MotionKit
// Version, version 0 when there is none); false with the refusal.
using KitProducer = std::function<bool(sqlite3*, const MotionKitVersion&,
                                       Value&, MotionKitError&)>;

// The write order of MotionLibraryBase.h, shared by the save, the edit and
// the restore: R, KitFileDiverged, UnknownKit (`mustExist`), StaleKit, the
// new tree, no duplicate, then insert + file + COMMIT.
bool WriteKitVersion(const MotionLibraryLocation& location,
                     const std::string& name, int64_t expected, bool mustExist,
                     const std::string& source, const KitProducer& produce,
                     MotionKitSaved& saved, MotionKitError& error) {
    saved = MotionKitSaved{};
    error = MotionKitError{};
    saved.path = KitFilePath(location.inbox, name);
    Database db(nullptr, sqlite3_close);
    std::string reason;
    const auto ioFailed = [&](const std::string& path,
                              const std::string& what) {
        saved.imported.clear();
        return KitError(error, kMotionKitIoError, path, what + reason);
    };
    if (!Open(location.base, db, reason)) {
        return ioFailed(location.base, "the library base cannot be used: ");
    }
    Transaction transaction(db.get());
    if (!transaction.Begin(reason)) {
        return ioFailed(location.base, "the library base is busy: ");
    }
    // Before R: a name another kit's file would answer to is refused
    // whatever this disk does with case, so that a library reads the same
    // on every disk.
    std::string other;
    if (!KitNameCollision(db.get(), location.inbox, name, other, reason)) {
        return ioFailed(location.base, "the library base failed: ");
    }
    if (!other.empty()) {
        transaction.Rollback();
        return KitError(error, kMotionKitInvalidKit, "",
                        "'" + name + "' differs only by case from " + other +
                            ": on a disk that ignores case, as macOS's does "
                            "by default, both would be one file; give this "
                            "kit another name");
    }
    MotionKitFileState state = MotionKitFileState::Absent;
    std::string fileIssue;
    MotionKitVersion latest;
    if (!ImportKitFile(db.get(), location.inbox, name, state, fileIssue, latest,
                       saved.imported, reason)) {
        return ioFailed(location.base, "the library base failed: ");
    }
    if (state == MotionKitFileState::Diverged) {
        transaction.Rollback();
        return KitError(error, kMotionKitFileDiverged, saved.path,
                        saved.path + " no longer reads as a charter (" +
                            fileIssue +
                            "): the engine does not overwrite it; fix the "
                            "file, or move it away to write again");
    }
    if (mustExist && latest.version == 0) {
        if (!transaction.Commit(reason)) {
            return ioFailed(location.base, "the library base failed: ");
        }
        return KitError(error, kMotionKitUnknownKit, "",
                        "no kit '" + name + "' in the library");
    }
    if (expected != latest.version) {
        if (!transaction.Commit(reason)) {
            return ioFailed(location.base, "the library base failed: ");
        }
        saved.version = latest.version;
        saved.content = latest.content;
        return KitError(
            error, kMotionKitStaleKit, "",
            "kit '" + name + "' is at version " +
                std::to_string(latest.version) + ", not " +
                std::to_string(expected) +
                (latest.version == 0 ? std::string(": it no longer exists")
                                     : std::string(": read it again")) +
                ", nothing was written");
    }
    Value tree;
    if (!produce(db.get(), latest, tree, error)) {
        // What R imported stays: it is the file, whatever this write was.
        std::string ignored;
        transaction.Commit(ignored);
        return false;
    }
    const std::string content = DumpMotionKit(tree);
    Value stored;
    std::string ignored;
    const bool same = latest.version > 0 &&
                      Value::Parse(latest.content, stored, ignored) &&
                      DumpMotionKit(stored) == content;
    if (same) {
        // The file holds the stored bytes, so that the next R matches.
        if (state == MotionKitFileState::Absent && !location.inbox.empty() &&
            !WriteKitFile(location.inbox, name, latest.content, reason)) {
            return ioFailed(saved.path, "");
        }
        if (!transaction.Commit(reason)) {
            return ioFailed(location.base, "the library base failed: ");
        }
        saved.version = latest.version;
        saved.content = latest.content;
        return true;
    }
    if (!Insert(db.get(), "kit", name, latest.version + 1, content, source,
                reason)) {
        return ioFailed(location.base, "the library base failed: ");
    }
    if (!location.inbox.empty() &&
        !WriteKitFile(location.inbox, name, content, reason)) {
        return ioFailed(saved.path, "");
    }
    if (!transaction.Commit(reason)) {
        // The file is one version ahead of the base: the next R imports it.
        return ioFailed(location.base,
                        "the library base failed after the file was "
                        "written (the next read imports it): ");
    }
    saved.version = latest.version + 1;
    saved.created = true;
    saved.content = content;
    return true;
}

// R alone, for a read: the state of kits/<name>.json and the latest
// version after it. `opened` false when there is no base and no file, and
// nothing was created.
bool ReadKit(const MotionLibraryLocation& location, const std::string& name,
             Database& db, MotionKitFileState& state, std::string& fileIssue,
             MotionKitVersion& latest,
             std::vector<MotionLibraryIssue>& imported, bool& opened,
             MotionKitError& error) {
    opened = false;
    state = MotionKitFileState::Absent;
    // No base and no entry of that exact name: nothing to read, and no base
    // left behind (a file of another case is another kit's).
    if (!BaseExists(location.base) &&
        !KitFileEntryExists(location.inbox, name)) {
        return true;
    }
    std::string reason;
    if (!Open(location.base, db, reason)) {
        return KitError(error, kMotionKitIoError, location.base,
                        "the library base cannot be used: " + reason);
    }
    Transaction transaction(db.get());
    if (!transaction.Begin(reason) ||
        !ImportKitFile(db.get(), location.inbox, name, state, fileIssue, latest,
                       imported, reason) ||
        !transaction.Commit(reason)) {
        imported.clear();
        return KitError(error, kMotionKitIoError, location.base,
                        "the library base failed: " + reason);
    }
    opened = true;
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
    std::vector<InboxKit> kits;
    ReadInbox(location.inbox, shipped, files, kits, contents.issues);
    // What the files said before the base was locked: the reasons the
    // shelf gives when R cannot run.
    const auto unreadKits = [&]() {
        for (const InboxKit& kit : kits) {
            if (!kit.reason.empty()) {
                contents.issues.push_back({kit.path, kit.reason});
            }
        }
    };
    // Reading an empty library leaves no base behind.
    const bool importable =
        !files.empty() ||
        std::any_of(kits.begin(), kits.end(),
                    [](const InboxKit& kit) { return kit.reason.empty(); });
    if (!importable && !BaseExists(location.base)) {
        unreadKits();
        return;
    }
    Database db(nullptr, sqlite3_close);
    std::string error;
    if (!Open(location.base, db, error)) {
        // The shelf still lists what the inbox holds, unversioned.
        contents.issues.push_back(
            {location.base, "the library base cannot be used: " + error});
        for (const InboxFile& file : files) {
            MotionTemplateInfo piece;
            ParseMotionPiece(file.name, file.content, piece, error);
            piece.path = file.path;
            contents.pieces.push_back(std::move(piece));
        }
        for (const InboxKit& file : kits) {
            if (file.reason.empty()) {
                MotionKitInfo kit;
                ParseMotionKit(file.name, file.content, kit, error);
                kit.path = file.path;
                contents.kits.push_back(std::move(kit));
            }
        }
        unreadKits();
        return;
    }
    std::vector<MotionLibraryIssue> diverged;
    if (!Import(db.get(), location.inbox, files, kits, contents.imported,
                diverged, error)) {
        // What the base holds is still listed; the files wait for the next
        // read.
        contents.issues.push_back(
            {location.inbox, "the inbox could not be imported: " + error});
        unreadKits();
    } else {
        std::move(diverged.begin(), diverged.end(),
                  std::back_inserter(contents.issues));
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

bool ListMotionKitNames(const MotionLibraryLocation& location,
                        std::vector<std::string>& names,
                        MotionKitError& error) {
    names.clear();
    error = MotionKitError{};
    const std::vector<std::string> files = KitFileNames(location.inbox);
    if (files.empty() && !BaseExists(location.base)) {
        return true;
    }
    Database db(nullptr, sqlite3_close);
    std::string reason;
    if (!Open(location.base, db, reason)) {
        return KitError(error, kMotionKitIoError, location.base,
                        "the library base cannot be used: " + reason);
    }
    std::vector<std::string> found = files;
    {
        Transaction transaction(db.get());
        if (!transaction.Begin(reason)) {
            return KitError(error, kMotionKitIoError, location.base,
                            "the library base is busy: " + reason);
        }
        std::vector<MotionLibraryIssue> imported;
        for (const std::string& name : files) {
            MotionKitFileState state = MotionKitFileState::Absent;
            std::string issue;
            MotionKitVersion latest;
            if (!ImportKitFile(db.get(), location.inbox, name, state, issue,
                               latest, imported, reason)) {
                return KitError(error, kMotionKitIoError, location.base,
                                "the library base failed: " + reason);
            }
        }
        Statement statement(nullptr, sqlite3_finalize);
        if (!Prepare(db.get(),
                     "SELECT DISTINCT name FROM motion_library WHERE kind = "
                     "'kit'",
                     statement, reason)) {
            return KitError(error, kMotionKitIoError, location.base,
                            "the library base failed: " + reason);
        }
        int code = SQLITE_ROW;
        while ((code = sqlite3_step(statement.get())) == SQLITE_ROW) {
            found.push_back(Column(statement.get(), 0));
        }
        statement.reset();
        if (code != SQLITE_DONE || !transaction.Commit(reason)) {
            if (code != SQLITE_DONE) {
                Fail(db.get(), reason);
            }
            return KitError(error, kMotionKitIoError, location.base,
                            "the library base failed: " + reason);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    names = std::move(found);
    return true;
}

bool ReadMotionKitState(const MotionLibraryLocation& location,
                        const std::string& name, MotionKitState& state,
                        MotionKitError& error) {
    state = MotionKitState{};
    error = MotionKitError{};
    if (!IsMotionPieceName(name)) {
        return KitError(error, kMotionKitUnknownKit, "",
                        "'" + name +
                            "' is not a kit name: letters, digits, '-' and "
                            "'_', 64 at most");
    }
    state.path = KitFilePath(location.inbox, name);
    Database db(nullptr, sqlite3_close);
    bool opened = false;
    if (!ReadKit(location, name, db, state.file, state.file_issue, state.latest,
                 state.imported, opened, error)) {
        return false;
    }
    state.kit.name = name;
    if (state.latest.version == 0 &&
        state.file != MotionKitFileState::Diverged) {
        return KitError(error, kMotionKitUnknownKit, "",
                        "no kit '" + name + "' in the library");
    }
    if (state.latest.version > 0) {
        MotionKitInfo kit;
        std::string reason;
        if (ParseMotionKit(name, state.latest.content, kit, reason)) {
            kit.version = state.latest.version;
            state.kit = std::move(kit);
        } else {
            state.latest.issue = reason;
        }
    }
    return true;
}

bool ListMotionKitVersions(const MotionLibraryLocation& location,
                           const std::string& name, size_t limit,
                           std::vector<MotionKitVersion>& versions,
                           MotionKitError& error) {
    versions.clear();
    error = MotionKitError{};
    if (!IsMotionPieceName(name)) {
        return KitError(error, kMotionKitUnknownKit, "",
                        "'" + name +
                            "' is not a kit name: letters, digits, '-' and "
                            "'_', 64 at most");
    }
    Database db(nullptr, sqlite3_close);
    MotionKitFileState state = MotionKitFileState::Absent;
    std::string fileIssue;
    MotionKitVersion latest;
    std::vector<MotionLibraryIssue> imported;
    bool opened = false;
    if (!ReadKit(location, name, db, state, fileIssue, latest, imported, opened,
                 error)) {
        return false;
    }
    if (opened && latest.version > 0) {
        Statement statement(nullptr, sqlite3_finalize);
        std::string reason;
        if (!Prepare(db.get(),
                     "SELECT version, content_json, source, created_at FROM "
                     "motion_library WHERE kind = 'kit' AND name = ?1 ORDER "
                     "BY version DESC LIMIT ?2",
                     statement, reason)) {
            return KitError(error, kMotionKitIoError, location.base,
                            "the library base failed: " + reason);
        }
        Bind(statement.get(), 1, name);
        sqlite3_bind_int64(statement.get(), 2,
                           limit == 0 ? -1 : static_cast<int64_t>(limit));
        int code = SQLITE_ROW;
        while ((code = sqlite3_step(statement.get())) == SQLITE_ROW) {
            MotionKitVersion row;
            row.version = sqlite3_column_int64(statement.get(), 0);
            row.content = Column(statement.get(), 1);
            row.source = Column(statement.get(), 2);
            row.created_at = Column(statement.get(), 3);
            row.issue = KitIssue(name, row.content);
            versions.push_back(std::move(row));
        }
        if (code != SQLITE_DONE) {
            Fail(db.get(), reason);
            versions.clear();
            return KitError(error, kMotionKitIoError, location.base,
                            "the library base failed: " + reason);
        }
    }
    if (versions.empty()) {
        return KitError(error, kMotionKitUnknownKit, "",
                        "no version of kit '" + name + "' in the library");
    }
    return true;
}

bool SaveMotionKitVersion(const MotionLibraryLocation& location,
                          const std::string& name, const mcp_json::Value& file,
                          int64_t expected_version, const std::string& source,
                          MotionKitSaved& saved, MotionKitError& error) {
    saved = MotionKitSaved{};
    error = MotionKitError{};
    if (!IsMotionPieceName(name)) {
        return KitError(error, kMotionKitInvalidKit, "",
                        "'" + name +
                            "' is not a kit name: letters, digits, '-' and "
                            "'_', 64 at most");
    }
    MotionKitInfo kit;
    std::string reason;
    if (!ParseMotionKit(name, DumpMotionKit(file), kit, reason)) {
        return KitError(error, kMotionKitInvalidKit, "", reason);
    }
    return WriteKitVersion(
        location, name, expected_version, false, source,
        [&](sqlite3*, const MotionKitVersion&, Value& tree, MotionKitError&) {
            tree = file;
            return true;
        },
        saved, error);
}

bool EditMotionKitVersion(const MotionLibraryLocation& location,
                          const std::string& name,
                          const std::vector<MotionKitEdit>& edits,
                          int64_t expected_version, const std::string& source,
                          MotionKitSaved& saved, MotionKitError& error) {
    saved = MotionKitSaved{};
    error = MotionKitError{};
    if (!IsMotionPieceName(name)) {
        return KitError(error, kMotionKitInvalidKit, "",
                        "'" + name +
                            "' is not a kit name: letters, digits, '-' and "
                            "'_', 64 at most");
    }
    return WriteKitVersion(
        location, name, expected_version, true, source,
        [&](sqlite3*, const MotionKitVersion& latest, Value& tree,
            MotionKitError& refused) {
            Value file;
            std::string reason;
            if (!Value::Parse(latest.content, file, reason)) {
                return KitError(refused, kMotionKitInvalidKit, "",
                                "version " + std::to_string(latest.version) +
                                    " of kit '" + name + "' is not JSON (" +
                                    reason + "): restore an older one");
            }
            bool changed = false;
            return EditMotionKit(name, file, edits, tree, changed, refused);
        },
        saved, error);
}

bool RestoreMotionKitVersion(const MotionLibraryLocation& location,
                             const std::string& name, int64_t version,
                             int64_t expected_version, MotionKitSaved& saved,
                             MotionKitError& error) {
    saved = MotionKitSaved{};
    error = MotionKitError{};
    if (!IsMotionPieceName(name)) {
        return KitError(error, kMotionKitInvalidKit, "",
                        "'" + name +
                            "' is not a kit name: letters, digits, '-' and "
                            "'_', 64 at most");
    }
    return WriteKitVersion(
        location, name, expected_version, true,
        "restore v" + std::to_string(version),
        [&](sqlite3* db, const MotionKitVersion&, Value& tree,
            MotionKitError& refused) {
            MotionKitVersion row;
            std::string reason;
            if (version <= 0) {
                return KitError(refused, kMotionKitUnknownKit, "",
                                "kit '" + name + "' has no version " +
                                    std::to_string(version));
            }
            if (!KitVersionRow(db, name, version, row, reason)) {
                return KitError(refused, kMotionKitIoError, location.base,
                                "the library base failed: " + reason);
            }
            if (row.version != version) {
                return KitError(refused, kMotionKitUnknownKit, "",
                                "kit '" + name + "' has no version " +
                                    std::to_string(version));
            }
            MotionKitInfo kit;
            if (!Value::Parse(row.content, tree, reason) ||
                !ParseMotionKit(name, DumpMotionKit(tree), kit, reason)) {
                return KitError(
                    refused, kMotionKitInvalidKit, "",
                    "version " + std::to_string(version) + " of kit '" + name +
                        "' no longer reads as a charter: " + reason);
            }
            return true;
        },
        saved, error);
}
