#include "MfaAlignment.h"

#include "FfmpegBinary.h"
#include "LocalEnv.h"
#include "ProcessLauncher.h"

#include <filesystem>
#include <utility>

// AX2 -- where the build installs MFA, and the script in its source tree
// (CMakeLists.txt). PRD21 -- a build that does not say has no such place,
// and the search skips that step: the working directory, which these used
// to be relative to, is a step of its own.
#ifndef CUTMACHINE_MFA_PYTHON_DEFAULT
#define CUTMACHINE_MFA_PYTHON_DEFAULT ""
#endif
#ifndef CUTMACHINE_MFA_ALIGNER_DEFAULT
#define CUTMACHINE_MFA_ALIGNER_DEFAULT ""
#endif
#ifndef CUTMACHINE_MFA_SCRIPT_DEFAULT
#define CUTMACHINE_MFA_SCRIPT_DEFAULT ""
#endif

namespace {

constexpr const char* kNotFoundPrefix = "MFA not found: ";

// The three files: their name in a sentence, their variable, and where each
// sits from a build directory and from the working directory.
const tool_files::Role kPython = {"MFA's Python", kMfaPythonVariable,
                                  "mfa/env/bin/python",
                                  "build/mfa/env/bin/python"};
const tool_files::Role kAligner = {"MFA's mfa", kMfaAlignerVariable,
                                   "mfa/env/bin/mfa", "build/mfa/env/bin/mfa"};
const tool_files::Role kScript = {"scripts/mfa_align.py", kMfaScriptVariable,
                                  "../scripts/mfa_align.py",
                                  "scripts/mfa_align.py"};

}  // namespace

MfaAlignmentSettings MfaAlignmentSettings::FromEnvironment() {
    return LocateMfa(CurrentMfaSearch()).Settings();
}

std::string MissingMfaInstallation(const MfaAlignmentSettings& settings) {
    if (!settings.refusal.empty()) {
        return settings.refusal;
    }
    // Settings a caller filled itself: each path is checked as it is.
    return tool_files::CheckPaths(kNotFoundPrefix,
                                  {{settings.python, &kPython},
                                   {settings.aligner, &kAligner},
                                   {settings.script, &kScript}},
                                  local_env::LocalEnvFilePath());
}

const char* MfaOriginName(MfaOrigin origin, const char* variable) {
    return tool_files::OriginName(origin, variable);
}

MfaAlignmentSettings MfaLocation::Settings() const {
    MfaAlignmentSettings settings;
    settings.python = python.path;
    settings.aligner = aligner.path;
    settings.script = script.path;
    settings.refusal = refusal;
    return settings;
}

MfaLocation LocateMfa(const MfaSearch& search) {
    tool_files::Context context;
    context.executable_directory = search.executable_directory;
    context.working_directory = search.working_directory;
    context.is_file = search.is_file;
    MfaLocation location;
    std::vector<std::string> problems;
    std::vector<const tool_files::Role*> roles;
    const auto locate = [&](const tool_files::Role& role,
                            const std::string& configured,
                            const std::string& build) {
        std::string problem;
        MfaFile file =
            tool_files::Locate(role, configured, build, context, problem);
        if (!problem.empty()) {
            problems.push_back(std::move(problem));
            roles.push_back(&role);
        }
        return file;
    };
    location.python =
        locate(kPython, search.python_variable, search.build_python);
    location.aligner =
        locate(kAligner, search.aligner_variable, search.build_aligner);
    location.script =
        locate(kScript, search.script_variable, search.build_script);
    if (!problems.empty()) {
        location.refusal = tool_files::Refusal(kNotFoundPrefix, problems, roles,
                                               search.settings_file);
    }
    return location;
}

MfaSearch CurrentMfaSearch() {
    MfaSearch search;
    search.python_variable = local_env::Value(kMfaPythonVariable);
    search.aligner_variable = local_env::Value(kMfaAlignerVariable);
    search.script_variable = local_env::Value(kMfaScriptVariable);
    search.executable_directory = ffmpeg::ExecutableDirectory();
    search.build_python = CUTMACHINE_MFA_PYTHON_DEFAULT;
    search.build_aligner = CUTMACHINE_MFA_ALIGNER_DEFAULT;
    search.build_script = CUTMACHINE_MFA_SCRIPT_DEFAULT;
    std::error_code error;
    const std::filesystem::path workingDirectory =
        std::filesystem::current_path(error);
    if (!error) {
        search.working_directory = workingDirectory.string();
    }
    search.settings_file = local_env::LocalEnvFilePath();
    return search;
}

std::string MfaRefusalName(const std::string& message) {
    return message.find(kNotFoundPrefix) != std::string::npos
               ? kMfaNotFoundErrorName
               : std::string();
}

bool RunMfaAlignment(const MfaAlignmentSettings& settings,
                     const std::string& projectPath,
                     const std::vector<std::string>& mediaIds,
                     const std::string& textsPath,
                     const std::string& workDirectory,
                     const std::string& reportPath, MediaTaskContext& context,
                     std::string& error) {
    std::vector<std::string> arguments = {settings.python, settings.script,
                                          projectPath};
    arguments.insert(arguments.end(), mediaIds.begin(), mediaIds.end());
    arguments.insert(arguments.end(), {"--work", workDirectory, "--report",
                                       reportPath, "--mfa", settings.aligner});
    if (!textsPath.empty())
        arguments.insert(arguments.end(), {"--text", textsPath});
    // FI10 -- docs/roadmap/FIABILITE-2026-10.md. The common launcher keeps
    // cancellation alive after stderr reaches EOF, through the final wait,
    // and stops MFA's workers along with the script.
    process::Request request;
    request.arguments = std::move(arguments);
    request.discard_input = true;
    request.standard_output = process::Stream::kDiscard;
    request.standard_error = process::Stream::kPipe;
    request.error_tail_bytes = 4096;
    request.cancelled = [&context] { return context.Cancelled(); };
    request.on_started = [&context] {
        context.SetProgress(0.0, "Alignement MFA");
    };
    const process::Result result = process::Run(request);
    if (!result.Started()) {
        error = "unable to start '" + settings.python +
                "': " + result.SystemErrorText();
        return false;
    }
    if (result.cancelled || context.Cancelled()) {
        error = "word alignment cancelled";
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty() ? "MFA alignment failed"
                                          : result.error_tail;
        return false;
    }
    return true;
}
