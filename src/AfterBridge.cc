#include "AfterBridge.h"

#include "Json.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CUTMACHINE_AFTER_BRIDGE_SCRIPTS
#define CUTMACHINE_AFTER_BRIDGE_SCRIPTS "tools/after-bridge/scripts"
#endif

extern char** environ;

namespace {

using mcp_json::Value;

constexpr std::chrono::milliseconds kLaunchTimeout{10000};
constexpr std::chrono::milliseconds kScriptTimeout{190000};
constexpr std::chrono::milliseconds kMailboxTimeout{3000};

bool CheckKnownKeys(const Value& value, const std::vector<std::string>& allowed,
                    std::string& error) {
    if (!value.IsObject()) {
        error = "expected a JSON object";
        return false;
    }
    for (const auto& entry : value.AsObject()) {
        bool known = false;
        for (const std::string& key : allowed) {
            if (entry.first == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = "unknown field '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

bool RequiredString(const Value& value, const std::string& key,
                    std::string& output, std::string& error) {
    const Value* field = value.Find(key);
    if (field == nullptr || !field->IsString() || field->AsString().empty()) {
        error = "'" + key + "' must be a non-empty string";
        return false;
    }
    output = field->AsString();
    return true;
}

bool RequiredStringAllowEmpty(const Value& value, const std::string& key,
                              std::string& output, std::string& error) {
    const Value* field = value.Find(key);
    if (field == nullptr || !field->IsString()) {
        error = "'" + key + "' must be a string";
        return false;
    }
    output = field->AsString();
    return true;
}

bool OptionalString(const Value& value, const std::string& key,
                    std::string& output, std::string& error) {
    const Value* field = value.Find(key);
    if (field == nullptr) {
        output.clear();
        return true;
    }
    if (!field->IsString()) {
        error = "'" + key + "' must be a string";
        return false;
    }
    output = field->AsString();
    return true;
}

std::optional<AfterBridgeMethod> MethodFromName(const std::string& name) {
    if (name == "create_project") return AfterBridgeMethod::CreateProject;
    if (name == "open_project_copy") return AfterBridgeMethod::OpenProjectCopy;
    if (name == "describe_project") return AfterBridgeMethod::DescribeProject;
    if (name == "list_comps") return AfterBridgeMethod::ListComps;
    if (name == "list_footage") return AfterBridgeMethod::ListFootage;
    if (name == "describe_comp") return AfterBridgeMethod::DescribeComp;
    if (name == "render_frame") return AfterBridgeMethod::RenderFrame;
    if (name == "set_text") return AfterBridgeMethod::SetText;
    if (name == "set_property") return AfterBridgeMethod::SetProperty;
    if (name == "set_layer_state") return AfterBridgeMethod::SetLayerState;
    if (name == "replace_footage") return AfterBridgeMethod::ReplaceFootage;
    if (name == "import_footage") return AfterBridgeMethod::ImportFootage;
    if (name == "add_layer") return AfterBridgeMethod::AddLayer;
    if (name == "duplicate_layer") return AfterBridgeMethod::DuplicateLayer;
    if (name == "save_project") return AfterBridgeMethod::SaveProject;
    if (name == "read_comp") return AfterBridgeMethod::ReadComp;
    if (name == "build_motion_project") {
        return AfterBridgeMethod::BuildMotionProject;
    }
    return std::nullopt;
}

std::string ReadFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to read After bridge script '" + path.string() + "'";
        return {};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to finish reading After bridge script '" +
                path.string() + "'";
        return {};
    }
    return contents.str();
}

std::filesystem::path ScriptsDirectory() {
    const char* overridePath = std::getenv("CUTMACHINE_AFTER_BRIDGE_SCRIPTS");
    if (overridePath != nullptr && *overridePath != '\0') return overridePath;
    return CUTMACHINE_AFTER_BRIDGE_SCRIPTS;
}

std::string ScriptName(AfterBridgeMethod method) {
    switch (method) {
        case AfterBridgeMethod::CreateProject:
            return "create_project.jsx";
        case AfterBridgeMethod::OpenProjectCopy:
            return "open_project_copy.jsx";
        case AfterBridgeMethod::DescribeProject:
            return "describe_project.jsx";
        case AfterBridgeMethod::ListComps:
            return "list_comps.jsx";
        case AfterBridgeMethod::ListFootage:
            return "list_footage.jsx";
        case AfterBridgeMethod::DescribeComp:
            return "describe_comp.jsx";
        case AfterBridgeMethod::RenderFrame:
            return "render_frame.jsx";
        case AfterBridgeMethod::SetText:
            return "set_text.jsx";
        case AfterBridgeMethod::SetProperty:
            return "set_property.jsx";
        case AfterBridgeMethod::SetLayerState:
            return "set_layer_state.jsx";
        case AfterBridgeMethod::ReplaceFootage:
            return "replace_footage.jsx";
        case AfterBridgeMethod::ImportFootage:
            return "import_footage.jsx";
        case AfterBridgeMethod::AddLayer:
            return "add_layer.jsx";
        case AfterBridgeMethod::DuplicateLayer:
            return "duplicate_layer.jsx";
        case AfterBridgeMethod::SaveProject:
            return "save_project.jsx";
        // The same script builds a project by hand (File > Scripts), from
        // the description it is pointed at.
        case AfterBridgeMethod::BuildMotionProject:
            return "motion_import.jsx";
        case AfterBridgeMethod::ReadComp:
            return "read_comp.jsx";
    }
    return {};
}

std::string PercentEncode(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size() * 3);
    for (const unsigned char byte : text) {
        encoded.push_back('%');
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0f]);
    }
    return encoded;
}

bool BuildInvocation(const AfterBridgeRequest& request,
                     const std::filesystem::path& responsePath,
                     AfterBridgeInvocation& invocation, std::string& error) {
    const std::filesystem::path directory = ScriptsDirectory();
    const std::string common = ReadFile(directory / "common.jsx", error);
    if (!error.empty()) return false;
    const std::string scriptName = ScriptName(request.method);
    const std::string handler = ReadFile(directory / scriptName, error);
    if (!error.empty()) return false;

    // The request is already strict JSON, but embedding arbitrary path bytes
    // directly in JavaScript would make U+2028 and source metacharacters part
    // of the program. Percent-encoding every UTF-8 byte keeps data and source
    // separate; eval only sees the validated JSON object after decoding.
    const std::string requestJson = SerializeAfterBridgeRequest(request);
    std::ostringstream job;
    job << "var CUTMACHINE_AFTER_REQUEST = eval(\"(\" + "
           "decodeURIComponent(\""
        << PercentEncode(requestJson) << "\") + \")\");\n"
        << "var CUTMACHINE_AFTER_RESPONSE_PATH = decodeURIComponent(\""
        << PercentEncode(responsePath.string()) << "\");\n"
        << common << '\n'
        << handler
        << '\n'
        // MA11 -- After keeps a script's globals for the session: a request
        // left behind made motion_import.jsx, run by hand afterwards, take
        // itself for the bridge and build nothing.
        << "try {\n"
           "    CUTMACHINE_AFTER.execute(CUTMACHINE_AFTER_REQUEST, "
           "CUTMACHINE_AFTER_HANDLER);\n"
           "} finally {\n"
           "    CUTMACHINE_AFTER_REQUEST = undefined;\n"
           "}\n";
    invocation = {request, scriptName, job.str(), responsePath.string()};
    return true;
}

bool RequiredArgumentString(const Value& arguments, const std::string& key,
                            std::string& error) {
    std::string ignored;
    return RequiredString(arguments, key, ignored, error);
}

bool RequiredArgumentStringAllowEmpty(const Value& arguments,
                                      const std::string& key,
                                      std::string& error) {
    std::string ignored;
    return RequiredStringAllowEmpty(arguments, key, ignored, error);
}

bool IsAfterPropertyValue(const Value& value) {
    if (value.IsString() || value.IsBool() || value.IsNumber()) return true;
    if (!value.IsArray()) return false;
    return std::all_of(
        value.AsArray().begin(), value.AsArray().end(), [](const Value& item) {
            return item.IsString() || item.IsBool() || item.IsNumber();
        });
}

bool OptionalArgumentBool(const Value& arguments, const std::string& key,
                          std::string& error) {
    const Value* field = arguments.Find(key);
    if (field == nullptr || field->IsBool()) return true;
    error = "'" + key + "' must be a boolean";
    return false;
}

bool OptionalArgumentInteger(const Value& arguments, const std::string& key,
                             std::string& error) {
    const Value* field = arguments.Find(key);
    int64_t ignored = 0;
    if (field == nullptr || (field->IsNumber() && field->AsInt64(ignored))) {
        return true;
    }
    error = "'" + key + "' must be an exact integer";
    return false;
}

bool RequiredArgumentInteger(const Value& arguments, const std::string& key,
                             std::string& error) {
    const Value* field = arguments.Find(key);
    int64_t ignored = 0;
    if (field != nullptr && field->IsNumber() && field->AsInt64(ignored))
        return true;
    error = "'" + key + "' must be an exact integer";
    return false;
}

bool ValidateAfterArguments(AfterBridgeMethod method, const Value& arguments,
                            std::string& error) {
    if (!arguments.IsObject()) {
        error = "'arguments' must be a JSON object";
        return false;
    }
    std::vector<std::string> allowed;
    switch (method) {
        case AfterBridgeMethod::CreateProject:
            allowed = {"project_path"};
            if (!RequiredArgumentString(arguments, "project_path", error))
                return false;
            break;
        case AfterBridgeMethod::OpenProjectCopy:
            allowed = {"source_project_path", "working_project_path"};
            if (!RequiredArgumentString(arguments, "source_project_path",
                                        error) ||
                !RequiredArgumentString(arguments, "working_project_path",
                                        error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::DescribeProject:
        case AfterBridgeMethod::ListComps:
        case AfterBridgeMethod::ListFootage:
            break;
        case AfterBridgeMethod::ReadComp:
            allowed = {"comp_id"};
            if (!RequiredArgumentString(arguments, "comp_id", error))
                return false;
            break;
        case AfterBridgeMethod::DescribeComp:
            allowed = {"comp_id", "layer_id"};
            if (!RequiredArgumentString(arguments, "comp_id", error))
                return false;
            if (arguments.Find("layer_id") != nullptr &&
                !RequiredArgumentString(arguments, "layer_id", error))
                return false;
            break;
        case AfterBridgeMethod::RenderFrame:
            allowed = {"comp_id", "frame", "image_path"};
            if (!RequiredArgumentString(arguments, "comp_id", error) ||
                !RequiredArgumentInteger(arguments, "frame", error) ||
                !RequiredArgumentString(arguments, "image_path", error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::SetText:
            allowed = {"comp_id", "layer_id", "text", "expected", "save"};
            if (!RequiredArgumentString(arguments, "comp_id", error) ||
                !RequiredArgumentString(arguments, "layer_id", error) ||
                !RequiredArgumentStringAllowEmpty(arguments, "text", error) ||
                (arguments.Find("expected") != nullptr &&
                 !RequiredArgumentStringAllowEmpty(arguments, "expected",
                                                   error)) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::SetProperty: {
            allowed = {"comp_id", "layer_id", "property_path", "value",
                       "frame",   "expected", "save"};
            if (!RequiredArgumentString(arguments, "comp_id", error) ||
                !RequiredArgumentString(arguments, "layer_id", error) ||
                !OptionalArgumentInteger(arguments, "frame", error) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            const Value* path = arguments.Find("property_path");
            if (path == nullptr || !path->IsArray() ||
                path->AsArray().empty()) {
                error = "'property_path' must be a non-empty string array";
                return false;
            }
            for (const Value& segment : path->AsArray()) {
                if (!segment.IsString() || segment.AsString().empty()) {
                    error = "'property_path' must contain non-empty strings";
                    return false;
                }
            }
            const Value* value = arguments.Find("value");
            if (value == nullptr || !IsAfterPropertyValue(*value)) {
                error = "'value' must be a scalar or a flat array of scalars";
                return false;
            }
            const Value* expected = arguments.Find("expected");
            if (expected != nullptr && !IsAfterPropertyValue(*expected)) {
                error =
                    "'expected' must be a scalar or a flat array of scalars";
                return false;
            }
            break;
        }
        case AfterBridgeMethod::SetLayerState:
            allowed = {"comp_id",   "layer_id",    "enabled",
                       "locked",    "start_frame", "in_frame",
                       "out_frame", "expected",    "save"};
            if (!RequiredArgumentString(arguments, "comp_id", error) ||
                !RequiredArgumentString(arguments, "layer_id", error) ||
                !OptionalArgumentBool(arguments, "enabled", error) ||
                !OptionalArgumentBool(arguments, "locked", error) ||
                !OptionalArgumentInteger(arguments, "start_frame", error) ||
                !OptionalArgumentInteger(arguments, "in_frame", error) ||
                !OptionalArgumentInteger(arguments, "out_frame", error) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            if (arguments.Find("enabled") == nullptr &&
                arguments.Find("locked") == nullptr &&
                arguments.Find("start_frame") == nullptr &&
                arguments.Find("in_frame") == nullptr &&
                arguments.Find("out_frame") == nullptr) {
                error = "set_layer_state requires at least one changed field";
                return false;
            }
            if (const Value* expected = arguments.Find("expected")) {
                if (!expected->IsObject() || expected->AsObject().empty()) {
                    error =
                        "'expected' must be a non-empty object of layer "
                        "states";
                    return false;
                }
                if (!OptionalArgumentBool(*expected, "enabled", error) ||
                    !OptionalArgumentBool(*expected, "locked", error) ||
                    !OptionalArgumentInteger(*expected, "start_frame", error) ||
                    !OptionalArgumentInteger(*expected, "in_frame", error) ||
                    !OptionalArgumentInteger(*expected, "out_frame", error) ||
                    !CheckKnownKeys(*expected,
                                    {"enabled", "locked", "start_frame",
                                     "in_frame", "out_frame"},
                                    error)) {
                    return false;
                }
            }
            break;
        case AfterBridgeMethod::ReplaceFootage:
            allowed = {"footage_id", "media_path", "save"};
            if (!RequiredArgumentString(arguments, "footage_id", error) ||
                !RequiredArgumentString(arguments, "media_path", error) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::ImportFootage:
            allowed = {"media_path", "folder_name", "save"};
            if (!RequiredArgumentString(arguments, "media_path", error) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            if (arguments.Find("folder_name") != nullptr &&
                !RequiredArgumentString(arguments, "folder_name", error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::AddLayer:
            allowed = {"comp_id",  "item_id",   "index", "start_frame",
                       "in_frame", "out_frame", "save"};
            if (!RequiredArgumentString(arguments, "comp_id", error) ||
                !RequiredArgumentString(arguments, "item_id", error) ||
                !OptionalArgumentInteger(arguments, "index", error) ||
                !OptionalArgumentInteger(arguments, "start_frame", error) ||
                !OptionalArgumentInteger(arguments, "in_frame", error) ||
                !OptionalArgumentInteger(arguments, "out_frame", error) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::DuplicateLayer:
            allowed = {"comp_id", "layer_id", "save"};
            if (!RequiredArgumentString(arguments, "comp_id", error) ||
                !RequiredArgumentString(arguments, "layer_id", error) ||
                !OptionalArgumentBool(arguments, "save", error)) {
                return false;
            }
            break;
        case AfterBridgeMethod::SaveProject: {
            allowed = {"project_path"};
            const Value* path = arguments.Find("project_path");
            if (path != nullptr &&
                (!path->IsString() || path->AsString().empty())) {
                error =
                    "'project_path' must be a non-empty string when present";
                return false;
            }
            break;
        }
        case AfterBridgeMethod::BuildMotionProject:
            allowed = {"description_path", "project_path"};
            if (!RequiredArgumentString(arguments, "description_path", error) ||
                !RequiredArgumentString(arguments, "project_path", error)) {
                return false;
            }
            break;
    }
    return CheckKnownKeys(arguments, allowed, error);
}

// An empty `requiredExtension` accepts any media; otherwise the lowercased
// extension must match exactly, so a render target cannot silently become a
// format After writes differently than the caller expects.
bool NormalizeAbsolutePath(AfterBridgeRequest& request, const std::string& key,
                           std::string_view requiredExtension, bool mustExist,
                           bool mustNotExist, std::string& errorName,
                           std::string& message) {
    const Value* field = request.arguments.Find(key);
    if (field == nullptr) return true;
    std::filesystem::path path(field->AsString());
    if (!path.is_absolute()) {
        errorName = "ValidationFailed";
        message = "'" + key + "' must be absolute";
        return false;
    }
    if (!requiredExtension.empty()) {
        std::string extension = path.extension().string();
        for (char& character : extension) {
            if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        }
        if (extension != requiredExtension) {
            errorName = "ValidationFailed";
            message =
                "'" + key + "' must end in " + std::string(requiredExtension);
            return false;
        }
    }
    path = path.lexically_normal();
    const std::filesystem::path parent = path.parent_path();
    std::error_code statusError;
    if (!std::filesystem::is_directory(parent, statusError)) {
        errorName = "IoError";
        message = "directory does not exist: '" + parent.string() + "'";
        return false;
    }
    if (mustExist && !std::filesystem::is_regular_file(path, statusError)) {
        errorName = "IoError";
        message = "file does not exist: '" + path.string() + "'";
        return false;
    }
    if (mustNotExist && std::filesystem::exists(path, statusError)) {
        errorName = requiredExtension == ".aep" ? "ProjectExists" : "IoError";
        message = "refusing to overwrite existing file '" + path.string() + "'";
        return false;
    }
    request.arguments.Set(key, Value::MakeString(path.string()));
    return true;
}

bool ValidateLocalPaths(AfterBridgeRequest& request, std::string& errorName,
                        std::string& message) {
    switch (request.method) {
        case AfterBridgeMethod::CreateProject:
            return NormalizeAbsolutePath(request, "project_path", ".aep", false,
                                         true, errorName, message);
        case AfterBridgeMethod::OpenProjectCopy: {
            if (!NormalizeAbsolutePath(request, "source_project_path", ".aep",
                                       true, false, errorName, message) ||
                !NormalizeAbsolutePath(request, "working_project_path", ".aep",
                                       false, true, errorName, message)) {
                return false;
            }
            const std::string& source =
                request.arguments.Find("source_project_path")->AsString();
            const std::string& working =
                request.arguments.Find("working_project_path")->AsString();
            if (source == working) {
                errorName = "ValidationFailed";
                message = "'working_project_path' must differ from the source";
                return false;
            }
            return true;
        }
        case AfterBridgeMethod::ReplaceFootage:
        case AfterBridgeMethod::ImportFootage:
            return NormalizeAbsolutePath(request, "media_path", "", true, false,
                                         errorName, message);
        // A still is written by After, never overwritten: numbering the frames
        // is what makes a before/after comparison possible at all.
        case AfterBridgeMethod::RenderFrame:
            return NormalizeAbsolutePath(request, "image_path", ".png", false,
                                         true, errorName, message);
        case AfterBridgeMethod::SaveProject:
            return NormalizeAbsolutePath(request, "project_path", ".aep", false,
                                         false, errorName, message);
        // The description is read, the project written once: like
        // create_project, never over an existing file.
        case AfterBridgeMethod::BuildMotionProject:
            return NormalizeAbsolutePath(request, "description_path", ".json",
                                         true, false, errorName, message) &&
                   NormalizeAbsolutePath(request, "project_path", ".aep", false,
                                         true, errorName, message);
        default:
            return true;
    }
}

bool PrepareWorkingCopy(const AfterBridgeRequest& request,
                        std::string& errorName, std::string& message) {
    if (request.method != AfterBridgeMethod::OpenProjectCopy) return true;
    const std::filesystem::path source(
        request.arguments.Find("source_project_path")->AsString());
    const std::filesystem::path working(
        request.arguments.Find("working_project_path")->AsString());
    std::error_code error;
    if (!std::filesystem::copy_file(
            source, working, std::filesystem::copy_options::none, error)) {
        errorName = "IoError";
        message = "unable to create After Effects working copy '" +
                  working.string() + "': " + error.message();
        return false;
    }
    return true;
}

class ResponseMailbox {
public:
    bool Create(std::string& error) {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "cutmachine-after.XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            error = "mkdtemp() failed: " + std::string(std::strerror(errno));
            return false;
        }
        directory_ = created;
        response_path_ = directory_ / "response.json";
        return true;
    }

    ~ResponseMailbox() {
        std::error_code ignored;
        if (!response_path_.empty())
            std::filesystem::remove(response_path_.string() + ".tmp", ignored);
        if (!response_path_.empty())
            std::filesystem::remove(response_path_, ignored);
        if (!directory_.empty()) std::filesystem::remove(directory_, ignored);
    }

    const std::filesystem::path& response_path() const {
        return response_path_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path response_path_;
};

bool IsAfterEffectsApplication(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    if (path.extension() != ".app" ||
        name.find("After Effects") == std::string::npos ||
        name.find("Render Engine") != std::string::npos) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(
               path / "Contents/MacOS/After Effects", error) &&
           std::filesystem::is_regular_file(path / "Contents/Resources/ae.sdef",
                                            error);
}

bool FindAfterEffectsApplication(std::filesystem::path& application,
                                 std::string& errorName, std::string& message) {
    const char* overridePath = std::getenv("CUTMACHINE_AFTER_EFFECTS_APP");
    if (overridePath != nullptr && *overridePath != '\0') {
        application = std::filesystem::path(overridePath).lexically_normal();
        if (IsAfterEffectsApplication(application)) return true;
        errorName = "AfterConfigurationError";
        message =
            "CUTMACHINE_AFTER_EFFECTS_APP does not name an After Effects "
            "application bundle";
        return false;
    }

    std::vector<std::filesystem::path> candidates;
    std::error_code iteratorError;
    std::filesystem::directory_iterator applications("/Applications",
                                                     iteratorError);
    if (!iteratorError) {
        for (const auto& entry : applications) {
            const std::filesystem::path path = entry.path();
            if (IsAfterEffectsApplication(path)) candidates.push_back(path);
            std::error_code typeError;
            if (!entry.is_directory(typeError) ||
                path.filename().string().find("Adobe After Effects") != 0) {
                continue;
            }
            std::filesystem::directory_iterator children(path, typeError);
            if (typeError) continue;
            for (const auto& child : children) {
                if (IsAfterEffectsApplication(child.path()))
                    candidates.push_back(child.path());
            }
        }
    }
    if (candidates.empty()) {
        errorName = "AfterNotInstalled";
        message = "Adobe After Effects is not installed in /Applications";
        return false;
    }
    std::sort(candidates.begin(), candidates.end());
    application = candidates.back();
    return true;
}

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
};

bool RunProcess(const std::vector<std::string>& arguments,
                std::chrono::milliseconds timeout, ProcessResult& result,
                std::string& error) {
    if (arguments.empty()) {
        error = "empty process invocation";
        return false;
    }
    int outputPipe[2] = {-1, -1};
    if (::pipe(outputPipe) != 0) {
        error = "pipe() failed: " + std::string(std::strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        ::close(outputPipe[0]);
        ::close(outputPipe[1]);
        error = "posix_spawn_file_actions_init() failed";
        return false;
    }
    posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outputPipe[1]);

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    pid_t child = -1;
    const int spawnError = posix_spawn(&child, arguments[0].c_str(), &actions,
                                       nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(outputPipe[1]);
    if (spawnError != 0) {
        ::close(outputPipe[0]);
        error = "unable to start '" + arguments[0] +
                "': " + std::string(std::strerror(spawnError));
        return false;
    }

    const int flags = ::fcntl(outputPipe[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(outputPipe[0], F_SETFL, flags | O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool exited = false;
    bool eof = false;
    while (!exited || !eof) {
        char buffer[4096];
        while (true) {
            const ssize_t count = ::read(outputPipe[0], buffer, sizeof(buffer));
            if (count > 0) {
                result.output.append(buffer, static_cast<size_t>(count));
                continue;
            }
            if (count == 0) eof = true;
            break;
        }
        if (!exited) {
            const pid_t waitResult = ::waitpid(child, &status, WNOHANG);
            if (waitResult == child) exited = true;
        }
        if (exited && eof) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            ::kill(child, SIGTERM);
            ::poll(nullptr, 0, 100);
            if (::waitpid(child, &status, WNOHANG) == 0) ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            exited = true;
            break;
        }
        pollfd descriptor{outputPipe[0], POLLIN | POLLHUP, 0};
        ::poll(&descriptor, 1, 20);
    }
    ::close(outputPipe[0]);
    if (!result.timed_out && WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (!result.timed_out && WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);
    return true;
}

std::string EscapeAppleScriptString(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        if (character == '\\' || character == '"') escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

bool NativeAfterRunner(const AfterBridgeInvocation& invocation,
                       std::string& responseJson, std::string& errorName,
                       std::string& message) {
    std::filesystem::path application;
    if (!FindAfterEffectsApplication(application, errorName, message))
        return false;

    // `open` is idempotent for an already running application. Calling it on
    // every request covers both launch states without process-name heuristics.
    ProcessResult launch;
    std::string processError;
    if (!RunProcess({"/usr/bin/open", "-g", application.string()},
                    kLaunchTimeout, launch, processError)) {
        errorName = "AfterLaunchFailed";
        message = processError;
        return false;
    }
    if (launch.timed_out || launch.exit_code != 0) {
        errorName = launch.timed_out ? "AfterTimeout" : "AfterLaunchFailed";
        message = launch.timed_out ? "timed out while launching After Effects"
                                   : launch.output;
        return false;
    }

    std::ostringstream appleScript;
    appleScript << "on run argv\n"
                << "set jsxSource to item 1 of argv\n"
                << "with timeout of 180 seconds\n"
                << "tell application \""
                << EscapeAppleScriptString(application.string()) << "\"\n"
                << "launch\n"
                << "DoScript jsxSource override true\n"
                << "end tell\n"
                << "end timeout\n"
                << "end run";
    ProcessResult script;
    if (!RunProcess({"/usr/bin/osascript", "-e", appleScript.str(),
                     invocation.job_script},
                    kScriptTimeout, script, processError)) {
        errorName = "AfterExecutionFailed";
        message = processError;
        return false;
    }
    if (script.timed_out) {
        errorName = "AfterTimeout";
        message = "After Effects did not finish the JSX job within 180 seconds";
        return false;
    }
    if (script.exit_code != 0) {
        if (script.output.find("-1743") != std::string::npos) {
            errorName = "AfterAutomationDenied";
            message = script.output;
        } else if (script.output.find("-1712") != std::string::npos) {
            errorName = "AfterTimeout";
            message =
                "After Effects did not answer the JSX job. Close modal "
                "dialogs and check Settings > Scripting & Expressions.";
        } else {
            errorName = "AfterExecutionFailed";
            message = script.output.empty()
                          ? "After Effects rejected the JSX job"
                          : script.output;
        }
        return false;
    }
    // After Effects 26 acknowledges DoScript with the scalar value 1,
    // despite ae.sdef declaring a text result. The JSX response therefore
    // travels through one private, per-invocation mailbox. Poll briefly after
    // the Apple Event returns because After may finish a queued script just
    // after sending that acknowledgement.
    const std::filesystem::path responsePath(invocation.response_path);
    const auto deadline = std::chrono::steady_clock::now() + kMailboxTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code statusError;
        if (std::filesystem::is_regular_file(responsePath, statusError) &&
            std::filesystem::file_size(responsePath, statusError) > 0) {
            std::string readError;
            responseJson = ReadFile(responsePath, readError);
            if (!readError.empty()) {
                errorName = "AfterExecutionFailed";
                message = readError;
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    errorName = "AfterFileAccessDenied";
    message =
        "After Effects did not write the bridge response. Enable Settings > "
        "Scripting & Expressions > Allow Scripts to Write Files and Access "
        "Network, then retry.";
    return false;
}

}  // namespace

const char* AfterBridgeMethodName(AfterBridgeMethod method) {
    switch (method) {
        case AfterBridgeMethod::CreateProject:
            return "create_project";
        case AfterBridgeMethod::OpenProjectCopy:
            return "open_project_copy";
        case AfterBridgeMethod::DescribeProject:
            return "describe_project";
        case AfterBridgeMethod::ListComps:
            return "list_comps";
        case AfterBridgeMethod::ListFootage:
            return "list_footage";
        case AfterBridgeMethod::DescribeComp:
            return "describe_comp";
        case AfterBridgeMethod::RenderFrame:
            return "render_frame";
        case AfterBridgeMethod::SetText:
            return "set_text";
        case AfterBridgeMethod::SetProperty:
            return "set_property";
        case AfterBridgeMethod::SetLayerState:
            return "set_layer_state";
        case AfterBridgeMethod::ReplaceFootage:
            return "replace_footage";
        case AfterBridgeMethod::ImportFootage:
            return "import_footage";
        case AfterBridgeMethod::AddLayer:
            return "add_layer";
        case AfterBridgeMethod::DuplicateLayer:
            return "duplicate_layer";
        case AfterBridgeMethod::SaveProject:
            return "save_project";
        case AfterBridgeMethod::BuildMotionProject:
            return "build_motion_project";
        case AfterBridgeMethod::ReadComp:
            return "read_comp";
    }
    return "";
}

bool ParseAfterBridgeRequest(const std::string& json,
                             AfterBridgeRequest& request, std::string& error) {
    Value root;
    if (!Value::Parse(json, root, error)) return false;
    if (!CheckKnownKeys(root, {"schema", "method", "arguments"}, error))
        return false;
    std::string schema;
    std::string methodName;
    if (!RequiredString(root, "schema", schema, error) ||
        !RequiredString(root, "method", methodName, error)) {
        return false;
    }
    if (schema != kAfterRequestSchema) {
        error = "unsupported After request schema '" + schema + "'";
        return false;
    }
    const std::optional<AfterBridgeMethod> method = MethodFromName(methodName);
    if (!method) {
        error = "unknown After method '" + methodName + "'";
        return false;
    }
    const Value* arguments = root.Find("arguments");
    if (arguments == nullptr) {
        error = "'arguments' is required";
        return false;
    }
    if (!ValidateAfterArguments(*method, *arguments, error)) return false;
    AfterBridgeRequest parsed;
    parsed.method = *method;
    parsed.arguments = *arguments;
    request = std::move(parsed);
    error.clear();
    return true;
}

std::string SerializeAfterBridgeRequest(const AfterBridgeRequest& request) {
    Value root = Value::MakeObject();
    root.Set("schema", Value::MakeString(kAfterRequestSchema));
    root.Set("method",
             Value::MakeString(AfterBridgeMethodName(request.method)));
    root.Set("arguments", request.arguments);
    return root.Dump();
}

bool ParseAfterBridgeResponse(const std::string& json,
                              AfterBridgeResponse& response,
                              std::string& error) {
    Value root;
    if (!Value::Parse(json, root, error)) return false;
    if (!CheckKnownKeys(root,
                        {"schema", "ok", "project_name", "path", "item_count",
                         "after_version", "result", "error", "detail"},
                        error)) {
        return false;
    }
    std::string schema;
    if (!RequiredString(root, "schema", schema, error)) return false;
    if (schema != kAfterResponseSchema) {
        error = "unsupported After response schema '" + schema + "'";
        return false;
    }
    const Value* ok = root.Find("ok");
    if (ok == nullptr || !ok->IsBool()) {
        error = "'ok' must be a boolean";
        return false;
    }
    AfterBridgeResponse parsed;
    parsed.ok = ok->AsBool();
    if (parsed.ok) {
        if (!RequiredString(root, "project_name", parsed.project_name, error) ||
            !RequiredStringAllowEmpty(root, "path", parsed.path, error) ||
            !OptionalString(root, "after_version", parsed.after_version,
                            error)) {
            return false;
        }
        const Value* itemCount = root.Find("item_count");
        if (itemCount != nullptr) {
            int64_t count = 0;
            if (!itemCount->IsNumber() || !itemCount->AsInt64(count) ||
                count < 0) {
                error = "'item_count' must be a non-negative integer";
                return false;
            }
            parsed.item_count = count;
        }
        const Value* result = root.Find("result");
        if (result != nullptr) {
            if (!result->IsObject()) {
                error = "'result' must be a JSON object";
                return false;
            }
            parsed.result = *result;
        }
        if (root.Find("error") != nullptr || root.Find("detail") != nullptr) {
            error = "a successful After response cannot contain an error";
            return false;
        }
    } else {
        if (!RequiredString(root, "error", parsed.error, error) ||
            !RequiredString(root, "detail", parsed.detail, error)) {
            return false;
        }
        if (root.Find("project_name") != nullptr ||
            root.Find("path") != nullptr ||
            root.Find("item_count") != nullptr ||
            root.Find("after_version") != nullptr ||
            root.Find("result") != nullptr) {
            error = "a failed After response cannot contain project fields";
            return false;
        }
    }
    response = std::move(parsed);
    error.clear();
    return true;
}

std::string SerializeAfterBridgeResponse(const AfterBridgeResponse& response) {
    Value root = Value::MakeObject();
    root.Set("schema", Value::MakeString(kAfterResponseSchema));
    root.Set("ok", Value::MakeBool(response.ok));
    if (response.ok) {
        root.Set("project_name", Value::MakeString(response.project_name));
        root.Set("path", Value::MakeString(response.path));
        if (response.item_count)
            root.Set("item_count", Value::MakeInt(*response.item_count));
        if (!response.after_version.empty())
            root.Set("after_version",
                     Value::MakeString(response.after_version));
        if (!response.result.IsNull()) root.Set("result", response.result);
    } else {
        root.Set("error", Value::MakeString(response.error));
        root.Set("detail", Value::MakeString(response.detail));
    }
    return root.Dump();
}

bool RunAfterBridgeRequest(const std::string& requestJson,
                           std::string& responseJson, std::string& errorName,
                           std::string& message,
                           const AfterBridgeRunner& runner) {
    AfterBridgeRequest request;
    if (!ParseAfterBridgeRequest(requestJson, request, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    if (!ValidateLocalPaths(request, errorName, message)) return false;

    ResponseMailbox mailbox;
    if (!mailbox.Create(message)) {
        errorName = "IoError";
        return false;
    }
    AfterBridgeInvocation invocation;
    if (!BuildInvocation(request, mailbox.response_path(), invocation,
                         message)) {
        errorName = "IoError";
        return false;
    }
    if (!PrepareWorkingCopy(request, errorName, message)) return false;
    std::string rawResponse;
    const AfterBridgeRunner effectiveRunner =
        runner ? runner : AfterBridgeRunner(NativeAfterRunner);
    if (!effectiveRunner(invocation, rawResponse, errorName, message))
        return false;

    AfterBridgeResponse response;
    if (!ParseAfterBridgeResponse(rawResponse, response, message)) {
        errorName = "AfterProtocolError";
        message = "invalid after-response.v2: " + message;
        return false;
    }
    responseJson = SerializeAfterBridgeResponse(response);
    if (!response.ok) {
        errorName = response.error;
        message = response.detail;
        return false;
    }
    errorName.clear();
    message.clear();
    return true;
}
