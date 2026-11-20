#include "ToolFiles.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace tool_files {

namespace {

bool IsRegularFile(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool IsFolder(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

std::string Normal(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

// `path` read against the working directory when it is relative and the
// working directory is known.
std::string FromWorkingDirectory(const std::string& path,
                                 const std::string& workingDirectory) {
    const std::filesystem::path candidate(path);
    if (candidate.is_absolute() || workingDirectory.empty()) {
        return Normal(candidate);
    }
    return Normal(std::filesystem::path(workingDirectory) / candidate);
}

// The directories that stand for the executable's: its own and, for the
// binary of an application bundle (<x>.app/Contents/MacOS), the directory
// that holds the bundle -- build/ for build/CUTMACHINE.app.
std::vector<std::filesystem::path> ExecutableRoots(
    const std::string& executableDirectory) {
    std::vector<std::filesystem::path> roots;
    if (executableDirectory.empty()) {
        return roots;
    }
    std::filesystem::path directory =
        std::filesystem::path(executableDirectory).lexically_normal();
    if (!directory.has_filename()) {
        directory = directory.parent_path();
    }
    roots.push_back(directory);
    const std::filesystem::path contents = directory.parent_path();
    const std::filesystem::path bundle = contents.parent_path();
    if (directory.filename() == "MacOS" && contents.filename() == "Contents" &&
        bundle.extension() == ".app") {
        roots.push_back(bundle.parent_path());
    }
    return roots;
}

std::string QuotedList(const std::vector<std::string>& paths) {
    std::string text;
    for (const std::string& path : paths) {
        text += (text.empty() ? "'" : ", '") + path + "'";
    }
    return text;
}

}  // namespace

const char* OriginName(Origin origin, const char* variable) {
    switch (origin) {
        case Origin::kConfigured:
            return variable;
        case Origin::kExecutable:
            return "executable";
        case Origin::kBuild:
            return "build";
        case Origin::kWorkingDirectory:
            return "working_directory";
    }
    return variable;
}

File Locate(const Role& role, const std::string& configured,
            const std::string& build, const Context& context,
            std::string& problem) {
    const bool folder = role.kind == Kind::kFolder;
    std::function<bool(const std::string&)> present =
        folder ? context.is_folder : context.is_file;
    if (!present) {
        present = folder
                      ? std::function<bool(const std::string&)>(IsFolder)
                      : std::function<bool(const std::string&)>(IsRegularFile);
    }
    File file;
    if (!configured.empty()) {
        const std::string path =
            FromWorkingDirectory(configured, context.working_directory);
        file.origin = Origin::kConfigured;
        file.searched.push_back(path);
        if (present(path)) {
            file.path = path;
        } else {
            problem = std::string(role.variable) + " names '" + configured +
                      (folder ? "', which is not a folder"
                              : "', which is not a file");
        }
        return file;
    }
    const auto consider = [&](const std::string& candidate, Origin origin) {
        if (file.Found() || candidate.empty() ||
            std::find(file.searched.begin(), file.searched.end(), candidate) !=
                file.searched.end()) {
            return;
        }
        file.searched.push_back(candidate);
        if (present(candidate)) {
            file.path = candidate;
            file.origin = origin;
        }
    };
    for (const std::filesystem::path& root :
         ExecutableRoots(context.executable_directory)) {
        consider(Normal(root / role.beside_build), Origin::kExecutable);
    }
    consider(build.empty() ? std::string() : Normal(build), Origin::kBuild);
    consider(FromWorkingDirectory(role.from_working_directory,
                                  context.working_directory),
             Origin::kWorkingDirectory);
    if (!file.Found()) {
        problem = std::string(role.what) + " is at none of " +
                  QuotedList(file.searched);
    }
    return file;
}

std::string Refusal(const std::string& prefix,
                    const std::vector<std::string>& problems,
                    const std::vector<const Role*>& roles,
                    const std::string& settingsFile) {
    std::string text = prefix;
    for (size_t index = 0; index < problems.size(); ++index) {
        text += (index > 0 ? "; " : "") + problems[index];
    }
    std::vector<std::string> settings;
    settings.reserve(roles.size());
    for (const Role* role : roles) {
        settings.push_back(std::string(role->variable) + " to " + role->what);
    }
    text += ". Set " + Enumeration(settings) + ", in the environment";
    if (!settingsFile.empty()) {
        text += " or in '" + settingsFile + "'";
    }
    return text;
}

std::string CheckPaths(
    const std::string& prefix,
    const std::vector<std::pair<std::string, const Role*>>& paths,
    const std::string& settingsFile) {
    std::vector<std::string> problems;
    std::vector<const Role*> roles;
    for (const auto& [path, role] : paths) {
        const bool present =
            role->kind == Kind::kFolder ? IsFolder(path) : IsRegularFile(path);
        if (!path.empty() && present) {
            continue;
        }
        problems.push_back(std::string(role->what) + " is not at '" + path +
                           "'");
        roles.push_back(role);
    }
    return problems.empty() ? std::string()
                            : Refusal(prefix, problems, roles, settingsFile);
}

std::string Enumeration(const std::vector<std::string>& items) {
    std::string text;
    for (size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            text += index + 1 == items.size() ? " and " : ", ";
        }
        text += items[index];
    }
    return text;
}

}  // namespace tool_files
