#include "LocalEnv.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace local_env {
namespace {

std::string Trim(const std::string& value) {
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// One `KEY=value` line of the file. False for a blank line, a comment or a
// line without a key.
bool ParseLine(const std::string& line, std::string& key, std::string& value) {
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') return false;
    if (trimmed.rfind("export ", 0) == 0) trimmed = Trim(trimmed.substr(7));
    const size_t equals = trimmed.find('=');
    if (equals == std::string::npos) return false;
    key = Trim(trimmed.substr(0, equals));
    value = Trim(trimmed.substr(equals + 1));
    if (key.empty()) return false;
    if (value.size() >= 2 && value.front() == value.back() &&
        (value.front() == '\'' || value.front() == '"')) {
        value = value.substr(1, value.size() - 2);
    }
    return true;
}

void LoadDotEnvFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) return;
    std::string line;
    std::string key;
    std::string value;
    while (std::getline(input, line)) {
        if (!ParseLine(line, key, value)) continue;
        // overwrite=0: a variable already in the environment wins over the
        // file, so a one-off override in front of a command works.
        ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

// DEC23 -- a value as a line of the file carries it, read back by ParseLine
// to the same bytes. Plain when nothing in it would be lost or misread by a
// shell sourcing the file; otherwise in single quotes, which ParseLine
// strips and a shell keeps literal; in double quotes when the value holds a
// single quote, which ParseLine strips just the same.
std::string Encoded(const std::string& value) {
    constexpr const char* kShellSpecial = " \t\"'`$\\#;&|<>()*?[]{}~!";
    if (value.find_first_of(kShellSpecial) == std::string::npos) return value;
    if (value.find('\'') == std::string::npos) return "'" + value + "'";
    return "\"" + value + "\"";
}

std::string ErrnoText(const std::string& what, const std::string& path) {
    return what + " '" + path + "': " + std::strerror(errno);
}

// Writes `content` to `target` through a temporary file beside it, renamed
// over it once complete and synced. `mode` is the new file's.
bool ReplaceFile(const std::filesystem::path& target,
                 const std::string& content, mode_t mode, std::string& error) {
    const std::filesystem::path temporary =
        target.string() + ".tmp-" + std::to_string(::getpid());
    const int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (descriptor < 0) {
        error = ErrnoText("cannot create", temporary.string());
        return false;
    }
    // open() applies the umask: the mode is set again, exactly.
    bool ok = ::fchmod(descriptor, mode) == 0;
    size_t done = 0;
    while (ok && done < content.size()) {
        const ssize_t wrote =
            ::write(descriptor, content.data() + done, content.size() - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            ok = false;
            break;
        }
        done += static_cast<size_t>(wrote);
    }
    ok = ok && ::fsync(descriptor) == 0;
    if (!ok) error = ErrnoText("cannot write", temporary.string());
    if (::close(descriptor) != 0 && ok) {
        ok = false;
        error = ErrnoText("cannot write", temporary.string());
    }
    if (ok && ::rename(temporary.c_str(), target.c_str()) != 0) {
        ok = false;
        error = ErrnoText("cannot replace", target.string());
    }
    if (!ok) ::unlink(temporary.c_str());
    return ok;
}

}  // namespace

std::string LocalEnvFilePath() {
    if (const char* explicitPath = std::getenv(kEnvFileVariable);
        explicitPath && *explicitPath)
        return explicitPath;
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/cutmachine/.env";
    return std::string();
}

void LoadLocalEnvFileIfPresent() {
    const std::string path = LocalEnvFilePath();
    if (!path.empty()) LoadDotEnvFile(path);
}

std::string Value(const char* name) {
    LoadLocalEnvFileIfPresent();
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string FileValue(const char* name) {
    const std::string path = LocalEnvFilePath();
    if (path.empty()) return std::string();
    std::ifstream input(path);
    std::string line;
    std::string key;
    std::string value;
    while (std::getline(input, line)) {
        // The first line that sets it: LoadDotEnvFile never overwrites, so
        // that is the one the environment receives.
        if (ParseLine(line, key, value) && key == name) return value;
    }
    return std::string();
}

WriteOutcome WriteFileValue(const char* name, const std::string& value) {
    WriteOutcome outcome;
    const std::string variable = name != nullptr ? name : "";
    if (variable.empty() ||
        variable.find_first_of("= \t\r\n#") != std::string::npos) {
        outcome.error = "not a setting name: '" + variable + "'";
        return outcome;
    }
    if (value.empty() ||
        value.find_first_of(std::string("\r\n\0", 3)) != std::string::npos) {
        outcome.error = value.empty() ? variable + " cannot be set empty"
                                      : variable +
                                            " cannot hold a line break: "
                                            "the settings file has one "
                                            "setting a line";
        return outcome;
    }
    const std::string path = LocalEnvFilePath();
    if (path.empty()) {
        outcome.error = std::string("no settings file: neither ") +
                        kEnvFileVariable + " nor HOME is set";
        return outcome;
    }

    // A link (a dotfiles checkout, say) is written through: what it points
    // to is the file, and the link stays a link.
    std::error_code fileError;
    std::filesystem::path target = path;
    if (std::filesystem::is_symlink(target, fileError)) {
        target = std::filesystem::canonical(target, fileError);
        if (fileError) {
            outcome.error = "the settings file '" + path +
                            "' is a link to nothing: " + fileError.message();
            return outcome;
        }
    }

    // The lines as they are, the one that sets the variable replaced.
    std::vector<std::string> lines;
    std::string previous;
    bool replaced = false;
    mode_t mode = S_IRUSR | S_IWUSR;
    struct stat status {};
    if (::stat(target.c_str(), &status) == 0) {
        if (!S_ISREG(status.st_mode)) {
            outcome.error =
                "the settings file '" + target.string() + "' is not a file";
            return outcome;
        }
        mode = status.st_mode & 07777;
        std::ifstream input(target, std::ios::binary);
        if (!input) {
            outcome.error = ErrnoText("cannot read", target.string());
            return outcome;
        }
        std::string line;
        std::string key;
        std::string current;
        while (std::getline(input, line)) {
            if (!replaced && ParseLine(line, key, current) && key == variable) {
                previous = current;
                const std::string trimmed = Trim(line);
                const bool exported = trimmed.rfind("export ", 0) == 0;
                line = (exported ? "export " : "") + variable + "=" +
                       Encoded(value);
                replaced = true;
            }
            lines.push_back(line);
        }
    } else if (errno != ENOENT) {
        outcome.error = ErrnoText("cannot read", target.string());
        return outcome;
    } else {
        std::filesystem::create_directories(target.parent_path(), fileError);
        if (fileError) {
            outcome.error = "cannot create the folder '" +
                            target.parent_path().string() +
                            "': " + fileError.message();
            return outcome;
        }
    }
    if (!replaced) lines.push_back(variable + "=" + Encoded(value));

    std::string content;
    for (const std::string& line : lines) content += line + "\n";
    if (!ReplaceFile(target, content, mode, outcome.error)) return outcome;
    outcome.written = true;

    // This process reads it too, unless the environment carries a value
    // the file did not give: that one wins, as it always has.
    const char* exported = std::getenv(variable.c_str());
    if (exported == nullptr || (!previous.empty() && previous == exported)) {
        ::setenv(variable.c_str(), value.c_str(), /*overwrite=*/1);
    } else if (value != exported) {
        outcome.environment_wins = true;
        outcome.environment_value = exported;
    }
    return outcome;
}

}  // namespace local_env
