#include "FfmpegBinary.h"

#include "FfmpegCapabilities.h"
#include "LocalEnv.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <utility>

namespace ffmpeg {
namespace {

// The only place the binary is called by its name.
constexpr const char* kBinaryName = "ffmpeg";
constexpr const char* kNotFoundPrefix = "FFmpeg not found: ";
// PRD40 -- InputPresent's sentence, the one Export.cc already gave.
constexpr const char* kInputMissingPrefix = "source media is missing: ";
// What posix_spawnp searches when PATH is unset.
constexpr const char* kDefaultSearchPath = "/usr/bin:/bin";

bool IsExecutableFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           access(path.c_str(), X_OK) == 0;
}

std::string Absolute(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal().string();
}

// The first directory of `searchPath` that holds an executable `name`. An
// empty entry is the working directory, as for the shell.
std::string FindOnSearchPath(const std::string& name,
                             const std::string& searchPath) {
    size_t begin = 0;
    while (begin <= searchPath.size()) {
        size_t end = searchPath.find(':', begin);
        if (end == std::string::npos) {
            end = searchPath.size();
        }
        const std::string directory = searchPath.substr(begin, end - begin);
        const std::filesystem::path candidate =
            std::filesystem::path(directory.empty() ? "." : directory) / name;
        if (IsExecutableFile(candidate)) {
            return Absolute(candidate);
        }
        begin = end + 1;
    }
    return std::string();
}

std::string HowToConfigure(const Search& search) {
    std::string text = std::string("Set ") + kBinaryVariable +
                       " to the path of an FFmpeg binary, in the environment";
    if (!search.settings_file.empty()) {
        text += " or in '" + search.settings_file + "'";
    }
    return text;
}

std::string FindExecutableDirectory() {
    std::string executable;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        executable = buffer.c_str();
    }
#else
    std::string buffer(4096, '\0');
    const ssize_t length =
        readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length > 0) {
        executable = buffer.substr(0, static_cast<size_t>(length));
    }
#endif
    if (executable.empty()) {
        return std::string();
    }
    // Through symbolic links: a `cutmachine` linked into /usr/local/bin
    // ships its FFmpeg where the package is, not where the link is.
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::canonical(executable, error);
    return (error ? std::filesystem::path(executable) : canonical)
        .parent_path()
        .string();
}

}  // namespace

std::string ExecutableDirectory() {
    // The executable does not move while it runs: one look is all of it.
    static const std::string directory = FindExecutableDirectory();
    return directory;
}

const char* OriginName(Origin origin) {
    switch (origin) {
        case Origin::kConfigured:
            return kBinaryVariable;
        case Origin::kBundled:
            return "bundled";
        case Origin::kSearchPath:
            return "PATH";
    }
    return "PATH";
}

Location Locate(const Search& search) {
    Location location;
    if (!search.configured.empty()) {
        location.origin = Origin::kConfigured;
        const bool bareName = search.configured.find('/') == std::string::npos;
        if (bareName) {
            location.path =
                FindOnSearchPath(search.configured, search.search_path);
        } else if (IsExecutableFile(search.configured)) {
            location.path = Absolute(search.configured);
        }
        if (!location.Found()) {
            location.refusal =
                std::string(kNotFoundPrefix) + kBinaryVariable + " names '" +
                search.configured + "', which is " +
                (bareName ? "in none of the directories of PATH ('" +
                                search.search_path + "')"
                          : std::string("not an executable file")) +
                ". " + HowToConfigure(search);
        }
        return location;
    }
    if (!search.executable_directory.empty()) {
        const std::filesystem::path bundled =
            std::filesystem::path(search.executable_directory) / kBinaryName;
        if (IsExecutableFile(bundled)) {
            location.origin = Origin::kBundled;
            location.path = Absolute(bundled);
            return location;
        }
    }
    location.origin = Origin::kSearchPath;
    location.path = FindOnSearchPath(kBinaryName, search.search_path);
    if (!location.Found()) {
        location.refusal =
            std::string(kNotFoundPrefix) + kBinaryVariable +
            " is not set, there is no '" + kBinaryName +
            "' beside the executable" +
            (search.executable_directory.empty()
                 ? std::string()
                 : " (in '" + search.executable_directory + "')") +
            ", and none in the directories of PATH ('" + search.search_path +
            "'). " + HowToConfigure(search);
    }
    return location;
}

Search CurrentSearch() {
    // The settings file only ever adds variables the environment lacks, and
    // what it adds stays for the life of the process: one read is all of it.
    static std::once_flag settingsLoaded;
    std::call_once(settingsLoaded, local_env::LoadLocalEnvFileIfPresent);

    Search search;
    if (const char* configured = std::getenv(kBinaryVariable)) {
        search.configured = configured;
    }
    search.executable_directory = ExecutableDirectory();
    const char* searchPath = std::getenv("PATH");
    search.search_path =
        searchPath != nullptr ? searchPath : kDefaultSearchPath;
    search.settings_file = local_env::LocalEnvFilePath();
    return search;
}

Location Resolved() {
    static std::mutex mutex;
    static bool remembered = false;
    static Search rememberedSearch;
    static Location rememberedLocation;

    Search search = CurrentSearch();
    const std::lock_guard<std::mutex> lock(mutex);
    if (remembered && rememberedSearch == search) {
        return rememberedLocation;
    }
    Location location = Locate(search);
    // Only an answer is kept: a machine where FFmpeg was just installed
    // should not need the app restarted to notice.
    remembered = location.Found();
    if (remembered) {
        rememberedSearch = std::move(search);
        rememberedLocation = location;
    }
    return location;
}

bool Executable(const std::string& requested, std::string& path,
                std::string& error) {
    if (!requested.empty()) {
        path = requested;
        return true;
    }
    Location location = Resolved();
    if (!location.Found()) {
        error = std::move(location.refusal);
        return false;
    }
    path = std::move(location.path);
    return true;
}

process::Result Run(process::Request request) {
    if (request.arguments.empty()) {
        request.arguments.emplace_back();
    }
    std::string path;
    std::string error;
    if (!Executable(request.arguments.front(), path, error)) {
        process::Result refused;
        refused.stage = process::Stage::kRefused;
        refused.refusal = std::move(error);
        return refused;
    }
    request.arguments.front() = std::move(path);
    // PRD2 -- a filter or an encoder this binary lacks is refused here, by
    // name, rather than left for FFmpeg to fail on.
    if (!Require(request.arguments.front(), request.arguments, error)) {
        process::Result refused;
        refused.stage = process::Stage::kRefused;
        refused.refusal = std::move(error);
        return refused;
    }
    return process::Run(request);
}

std::string StartError(const process::Result& result,
                       const std::string& pipes) {
    switch (result.stage) {
        case process::Stage::kRefused:
            return result.refusal;
        case process::Stage::kPipes:
            return "unable to create " + pipes + ": " +
                   result.SystemErrorText();
        case process::Stage::kSpawn:
        case process::Stage::kRan:
            break;
    }
    return "unable to start FFmpeg: " + result.SystemErrorText();
}

bool InputPresent(const std::string& path, std::string& error) {
    std::error_code statusError;
    if (std::filesystem::is_regular_file(path, statusError) && !statusError) {
        return true;
    }
    error = std::string(kInputMissingPrefix) + path;
    return false;
}

std::string RefusalName(const std::string& message) {
    if (message.find(kNotFoundPrefix) != std::string::npos) {
        return kNotFoundErrorName;
    }
    if (message.find(kInputMissingPrefix) != std::string::npos) {
        return kInputMissingErrorName;
    }
    if (message.find(kCapabilityMissingPrefix) != std::string::npos) {
        return kCapabilityMissingErrorName;
    }
    return std::string();
}

}  // namespace ffmpeg
