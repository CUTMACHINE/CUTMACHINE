#include "PackageFiles.h"

#include "WorkCounters.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace {

bool ReadFileContents(const std::string& path, std::string& contents,
                      std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        message = "unable to open '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        message = "unable to read '" + path + "'";
        return false;
    }
    contents = buffer.str();
    return true;
}

bool WriteFileSynchronized(const std::filesystem::path& path,
                           const std::string& contents, std::string& message) {
    const int descriptor =
        open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (descriptor < 0) {
        message = "unable to create '" + path.string() + "'";
        return false;
    }
    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + written,
                                    contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int savedError = errno;
            close(descriptor);
            message = "unable to write '" + path.string() +
                      "': " + std::strerror(savedError);
            return false;
        }
        written += static_cast<size_t>(count);
    }
    // fsync is the portable baseline. macOS may still defer drive-cache
    // flushing, so request F_FULLFSYNC there when the filesystem supports it.
#ifdef F_FULLFSYNC
    if (fcntl(descriptor, F_FULLFSYNC) != 0 && fsync(descriptor) != 0) {
#else
    if (fsync(descriptor) != 0) {
#endif
        const int savedError = errno;
        close(descriptor);
        message = "unable to synchronize '" + path.string() +
                  "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        message =
            "unable to close '" + path.string() + "': " + std::strerror(errno);
        return false;
    }
    return true;
}

bool SyncDirectory(const std::filesystem::path& path, std::string& message) {
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        message = "unable to open directory '" + path.string() +
                  "': " + std::strerror(errno);
        return false;
    }
    if (fsync(descriptor) != 0) {
        const int savedError = errno;
        close(descriptor);
        message = "unable to synchronize directory '" + path.string() +
                  "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        message = "unable to close directory '" + path.string() +
                  "': " + std::strerror(errno);
        return false;
    }
    return true;
}

void RemoveIfPresent(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

bool Rename(const std::filesystem::path& from, const std::filesystem::path& to,
            std::string& message) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (!error) return true;
    message = "unable to rename '" + from.string() + "' to '" + to.string() +
              "': " + error.message();
    return false;
}

struct CommitArtifact {
    std::filesystem::path path;
    std::string contents;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool existed = false;
    bool backed_up = false;
    bool committed = false;
    bool remove = false;
    bool unchanged = false;
};

std::string HexEncode(const std::string& input) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        output.push_back(kDigits[byte >> 4]);
        output.push_back(kDigits[byte & 0xf]);
    }
    return output;
}

bool HexDecode(const std::string& input, std::string& output) {
    if (input.size() % 2 != 0) return false;
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    output.clear();
    output.reserve(input.size() / 2);
    for (size_t index = 0; index < input.size(); index += 2) {
        const int high = digit(input[index]);
        const int low = digit(input[index + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::filesystem::path CommonDirectory(
    const std::vector<CommitArtifact>& artifacts) {
    std::filesystem::path common =
        std::filesystem::absolute(artifacts.front().path).parent_path();
    for (size_t index = 1; index < artifacts.size(); ++index) {
        const std::filesystem::path parent =
            std::filesystem::absolute(artifacts[index].path).parent_path();
        while (!common.empty()) {
            const std::filesystem::path relative =
                parent.lexically_relative(common);
            if (!relative.empty() && *relative.begin() != "..") break;
            if (parent == common) break;
            common = common.parent_path();
        }
    }
    return common;
}

bool IsWithinDirectory(const std::filesystem::path& directory,
                       const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalDirectory =
        std::filesystem::weakly_canonical(directory, error);
    if (error) return false;
    const std::filesystem::path canonicalParent =
        std::filesystem::weakly_canonical(path.parent_path(), error);
    if (error) return false;
    const std::filesystem::path relative =
        canonicalParent.lexically_relative(canonicalDirectory);
    return canonicalParent == canonicalDirectory ||
           (!relative.empty() && *relative.begin() != "..");
}

bool IsTransactionLeftover(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    const size_t marker = filename.rfind(".cutmachine-");
    if (marker == std::string::npos) return false;
    const std::string suffix = filename.substr(marker + 12);
    if (suffix.size() != 30 || suffix[26] != '.') return false;
    for (size_t index = 0; index < 26; ++index)
        if (!std::isalnum(static_cast<unsigned char>(suffix[index])))
            return false;
    return suffix.substr(27) == "tmp" || suffix.substr(27) == "bak";
}

bool RecoverTransaction(const std::filesystem::path& directory,
                        std::string& message) {
    const std::filesystem::path marker = directory / ".cutmachine-transaction";
    std::error_code existsError;
    if (!std::filesystem::exists(marker, existsError)) {
        if (existsError) {
            message = "unable to inspect transaction marker: " +
                      existsError.message();
            return false;
        }
        return true;
    }
    std::string journal;
    if (!ReadFileContents(marker.string(), journal, message)) return false;
    std::istringstream input(journal);
    std::string version;
    std::string nonce;
    if (!std::getline(input, version) ||
        version != "CUTMACHINE_TRANSACTION_V1" || !std::getline(input, nonce) ||
        nonce.size() != 38 || nonce.rfind(".cutmachine-", 0) != 0) {
        message = "invalid interrupted transaction marker";
        return false;
    }
    for (size_t index = 12; index < nonce.size(); ++index)
        if (!std::isalnum(static_cast<unsigned char>(nonce[index]))) {
            message = "invalid interrupted transaction marker";
            return false;
        }
    struct RecoveryEntry {
        std::filesystem::path path;
        bool existed = false;
    };
    std::vector<RecoveryEntry> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 3 || line[1] != ' ') {
            message = "invalid interrupted transaction entry";
            return false;
        }
        std::string decoded;
        if ((line[0] != '0' && line[0] != '1') ||
            !HexDecode(line.substr(2), decoded)) {
            message = "invalid interrupted transaction entry";
            return false;
        }
        const std::filesystem::path relative(decoded);
        if (relative.empty() || relative.is_absolute()) {
            message = "unsafe interrupted transaction entry";
            return false;
        }
        for (const auto& component : relative)
            if (component == "..") {
                message = "unsafe interrupted transaction entry";
                return false;
            }
        const std::filesystem::path path = directory / relative;
        if (!IsWithinDirectory(directory, path)) {
            message = "unsafe interrupted transaction entry";
            return false;
        }
        entries.push_back({path, line[0] == '1'});
    }
    for (auto item = entries.rbegin(); item != entries.rend(); ++item) {
        const std::filesystem::path backup =
            item->path.string() + nonce + ".bak";
        const std::filesystem::path temporary =
            item->path.string() + nonce + ".tmp";
        std::error_code backupError;
        if (std::filesystem::exists(backup, backupError)) {
            RemoveIfPresent(item->path);
            if (!Rename(backup, item->path, message)) return false;
        } else if (backupError) {
            message = "unable to inspect transaction backup: " +
                      backupError.message();
            return false;
        } else if (!item->existed) {
            RemoveIfPresent(item->path);
        }
        RemoveIfPresent(temporary);
        if (!SyncDirectory(item->path.parent_path(), message)) return false;
    }
    if (!std::filesystem::remove(marker, existsError) || existsError) {
        message = "unable to remove recovered transaction marker: " +
                  existsError.message();
        return false;
    }
    return SyncDirectory(directory, message);
}

void RemoveTransactionLeftovers(const std::filesystem::path& directory) {
    std::error_code error;
    for (const std::filesystem::path& scan :
         {directory, directory / "Timelines"}) {
        if (!std::filesystem::is_directory(scan, error)) {
            error.clear();
            continue;
        }
        for (std::filesystem::directory_iterator item(scan, error), end;
             !error && item != end; item.increment(error)) {
            if (item->is_regular_file(error) &&
                IsTransactionLeftover(item->path()))
                RemoveIfPresent(item->path());
            error.clear();
        }
    }
}

// PERF-2026-09. A generation is durable once its bytes are on disk -- and an
// artifact whose bytes are already exactly there is, by definition, already
// durable. Every commit hands us the whole package (project file, manifest,
// each timeline, each journal) even when one timeline moved, and each byte
// rewritten costs an F_FULLFSYNC plus two directory syncs: on an external
// USB/exFAT volume that is tens of milliseconds apiece, paid on the thread
// the editor draws from. Comparing first turns those into one cheap read.
// This weakens no guarantee in SAVING_ROADMAP.md S2: what is not rewritten
// is not changed, so it needs neither the journal nor a rollback entry.
bool ArtifactMatchesDisk(const CommitArtifact& artifact, bool exists) {
    if (artifact.remove) return !exists;
    if (!exists) return false;
    std::string existing;
    std::string ignored;
    return ReadFileContents(artifact.path.string(), existing, ignored) &&
           existing == artifact.contents;
}

// S2 -- SAVING_ROADMAP.md. Every byte and the rollback marker reach durable
// storage before an existing canonical destination is moved.
bool CommitArtifacts(std::vector<CommitArtifact> artifacts,
                     std::string& message) {
    if (artifacts.empty()) return true;
    const std::filesystem::path directory = CommonDirectory(artifacts);
    if (directory.empty() || directory == directory.root_path()) {
        message = "transaction artifacts do not share a safe directory";
        return false;
    }
    for (const CommitArtifact& artifact : artifacts)
        if (!IsWithinDirectory(directory,
                               std::filesystem::absolute(artifact.path))) {
            message = "transaction artifact escapes its package directory";
            return false;
        }
    if (!RecoverTransaction(directory, message)) return false;
    RemoveTransactionLeftovers(directory);
    // Recovery first: until it has run, what is on disk may still be a
    // half-committed generation, and comparing against that would skip an
    // artifact that does need rewriting.
    for (CommitArtifact& artifact : artifacts) {
        std::error_code existsError;
        artifact.existed = std::filesystem::exists(artifact.path, existsError);
        if (existsError) {
            message = "unable to inspect '" + artifact.path.string() +
                      "': " + existsError.message();
            return false;
        }
        artifact.unchanged = ArtifactMatchesDisk(artifact, artifact.existed);
    }
    artifacts.erase(std::remove_if(artifacts.begin(), artifacts.end(),
                                   [](const CommitArtifact& artifact) {
                                       return artifact.unchanged;
                                   }),
                    artifacts.end());
    if (artifacts.empty()) return true;
    work_counters::Count(work_counters::Work::kArtifactCommit,
                         artifacts.size());
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    const std::filesystem::path marker = directory / ".cutmachine-transaction";
    const std::filesystem::path markerTemporary =
        marker.string() + nonce + ".tmp";
    const auto cleanupTemporary = [&] {
        for (const CommitArtifact& artifact : artifacts)
            RemoveIfPresent(artifact.temporary);
    };
    const auto rollback = [&] {
        std::string ignored;
        for (auto item = artifacts.rbegin(); item != artifacts.rend(); ++item) {
            if (item->committed) RemoveIfPresent(item->path);
            if (item->backed_up) Rename(item->backup, item->path, ignored);
        }
        cleanupTemporary();
    };
    for (CommitArtifact& artifact : artifacts) {
        artifact.temporary = artifact.path.string() + nonce + ".tmp";
        artifact.backup = artifact.path.string() + nonce + ".bak";
        if (!artifact.remove &&
            !WriteFileSynchronized(artifact.temporary, artifact.contents,
                                   message)) {
            cleanupTemporary();
            return false;
        }
    }
    std::ostringstream journal;
    journal << "CUTMACHINE_TRANSACTION_V1\n" << nonce << '\n';
    for (const CommitArtifact& artifact : artifacts) {
        const std::filesystem::path relative =
            std::filesystem::absolute(artifact.path)
                .lexically_relative(directory);
        journal << (artifact.existed ? '1' : '0') << ' '
                << HexEncode(relative.generic_string()) << '\n';
    }
    if (!WriteFileSynchronized(markerTemporary, journal.str(), message) ||
        !Rename(markerTemporary, marker, message) ||
        !SyncDirectory(directory, message)) {
        cleanupTemporary();
        RemoveIfPresent(markerTemporary);
        RemoveIfPresent(marker);
        return false;
    }
    for (CommitArtifact& artifact : artifacts) {
        if (artifact.existed) {
            if (!Rename(artifact.path, artifact.backup, message)) {
                rollback();
                return false;
            }
            artifact.backed_up = true;
            if (!SyncDirectory(artifact.path.parent_path(), message)) {
                rollback();
                return false;
            }
        }
    }
    for (CommitArtifact& artifact : artifacts) {
        if (artifact.remove) continue;
        if (!Rename(artifact.temporary, artifact.path, message)) {
            rollback();
            return false;
        }
        artifact.committed = true;
        if (!SyncDirectory(artifact.path.parent_path(), message)) {
            rollback();
            return false;
        }
    }
    std::error_code markerError;
    if (!std::filesystem::remove(marker, markerError) || markerError) {
        message =
            "unable to remove transaction marker: " + markerError.message();
        rollback();
        return false;
    }
    if (!SyncDirectory(directory, message)) {
        std::string ignored;
        if (WriteFileSynchronized(markerTemporary, journal.str(), ignored) &&
            Rename(markerTemporary, marker, ignored))
            SyncDirectory(directory, ignored);
        rollback();
        return false;
    }
    for (const CommitArtifact& artifact : artifacts)
        if (artifact.backed_up) RemoveIfPresent(artifact.backup);
    RemoveTransactionLeftovers(directory);
    return true;
}

}  // namespace

bool ReadTextFile(const std::string& path, std::string& contents,
                  std::string& message) {
    return ReadFileContents(path, contents, message);
}

bool WriteDurableFile(const std::filesystem::path& path,
                      const std::string& contents, std::string& message) {
    return WriteFileSynchronized(path, contents, message);
}

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId) {
    return projectPath + ".timeline-" + timelineId + ".editlog.json";
}

std::string ProjectEditLogPathForProject(const std::string& projectPath) {
    return projectPath + ".project-editlog.json";
}

bool LoadProjectEditLogIfAny(const std::string& projectPath,
                             ProjectEditLog& log, EditError& error,
                             std::string& message) {
    const std::string path = ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (existsError) {
        error = EditError::IoError;
        message = "unable to inspect project edit log '" + path +
                  "': " + existsError.message();
        return false;
    }
    if (!exists) return true;
    std::string json;
    return ReadFileContents(path, json, message) &&
           ProjectEditLog::Deserialize(json, log, error, message);
}

bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message) {
    std::vector<CommitArtifact> prepared;
    prepared.reserve(artifacts.size());
    for (const auto& artifact : artifacts)
        prepared.push_back({artifact.first, artifact.second});
    return CommitArtifacts(std::move(prepared), message);
}

bool RecoverTextArtifactTransaction(const std::string& directory,
                                    std::string& message) {
    if (!RecoverTransaction(std::filesystem::absolute(directory), message))
        return false;
    RemoveTransactionLeftovers(std::filesystem::absolute(directory));
    message.clear();
    return true;
}

bool CommitTextArtifactsAndRemove(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    const std::vector<std::string>& removals, std::string& message) {
    std::vector<CommitArtifact> prepared;
    prepared.reserve(artifacts.size() + removals.size());
    for (const auto& artifact : artifacts)
        prepared.push_back({artifact.first, artifact.second});
    for (const std::string& removal : removals) {
        CommitArtifact artifact;
        artifact.path = removal;
        artifact.remove = true;
        prepared.push_back(std::move(artifact));
    }
    return CommitArtifacts(std::move(prepared), message);
}
