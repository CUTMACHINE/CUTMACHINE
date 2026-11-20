#include "VoiceOver.h"

#include "FfmpegBinary.h"
#include "Json.h"
#include "LocalEnv.h"
#include "ProcessLauncher.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

// GN6a -- where the build installs the model's environment and models, and
// the script in its source tree (CMakeLists.txt). A build that does not say
// has no such place, and the search skips that step.
#ifndef CUTMACHINE_VOICE_PYTHON_DEFAULT
#define CUTMACHINE_VOICE_PYTHON_DEFAULT ""
#endif
#ifndef CUTMACHINE_VOICE_SCRIPT_DEFAULT
#define CUTMACHINE_VOICE_SCRIPT_DEFAULT ""
#endif
#ifndef CUTMACHINE_VOICE_MODELS_DEFAULT
#define CUTMACHINE_VOICE_MODELS_DEFAULT ""
#endif

namespace {

constexpr const char* kNotFoundPrefix = "voice-over model not found: ";

const tool_files::Role kPython = {"the voice-over's Python",
                                  kVoicePythonVariable, "voice/env/bin/python",
                                  "build/voice/env/bin/python"};
const tool_files::Role kScript = {"scripts/voice_over.py", kVoiceScriptVariable,
                                  "../scripts/voice_over.py",
                                  "scripts/voice_over.py"};
const tool_files::Role kModels = {
    "the folder of the voice-over models", kVoiceModelsVariable, "voice/models",
    "build/voice/models", tool_files::Kind::kFolder};

// Qwen3-TTS's languages (its config's codec_language_id), by ISO 639-1
// code, in its order.
const std::pair<const char*, const char*> kLanguages[] = {
    {"zh", "Chinese"},  {"en", "English"},    {"de", "German"},
    {"it", "Italian"},  {"pt", "Portuguese"}, {"es", "Spanish"},
    {"ja", "Japanese"}, {"ko", "Korean"},     {"fr", "French"},
    {"ru", "Russian"},
};

bool IsRegularFile(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// The model folders `models` lacks, or holds short of a file.
std::vector<std::string> MissingModels(
    const std::string& models,
    const std::function<bool(const std::string&)>& isFile) {
    std::vector<std::string> missing;
    for (const char* folder : {kVoiceDesignModel, kVoiceCloneModel}) {
        for (const std::string& file : VoiceModelFiles()) {
            const std::string path =
                (std::filesystem::path(models) / folder / file).string();
            if (!isFile(path)) {
                missing.emplace_back(folder);
                break;
            }
        }
    }
    return missing;
}

// "the folder of the voice-over models '<dir>' lacks A and B (each needs
// config.json, ...)".
std::string MissingModelsProblem(const std::string& models,
                                 const std::vector<std::string>& missing) {
    std::vector<std::string> files;
    for (const std::string& file : VoiceModelFiles()) {
        files.push_back(file);
    }
    return std::string(kModels.what) + " '" + models + "' lacks " +
           tool_files::Enumeration(missing) + " (each needs " +
           tool_files::Enumeration(files) + ")";
}

}  // namespace

const std::vector<std::string>& VoiceModelFiles() {
    static const std::vector<std::string> files = {
        "config.json", "model.safetensors",
        "speech_tokenizer/model.safetensors"};
    return files;
}

VoiceOverSettings VoiceOverSettings::FromEnvironment() {
    return LocateVoiceOver(CurrentVoiceOverSearch()).Settings();
}

std::string MissingVoiceOverInstallation(const VoiceOverSettings& settings) {
    if (!settings.refusal.empty()) {
        return settings.refusal;
    }
    // Settings a caller filled itself: each path is checked as it is.
    std::string refusal = tool_files::CheckPaths(kNotFoundPrefix,
                                                 {{settings.python, &kPython},
                                                  {settings.script, &kScript},
                                                  {settings.models, &kModels}},
                                                 local_env::LocalEnvFilePath());
    if (!refusal.empty()) {
        return refusal;
    }
    const std::vector<std::string> missing =
        MissingModels(settings.models, IsRegularFile);
    if (missing.empty()) {
        return std::string();
    }
    return tool_files::Refusal(kNotFoundPrefix,
                               {MissingModelsProblem(settings.models, missing)},
                               {&kModels}, local_env::LocalEnvFilePath());
}

VoiceOverSettings VoiceOverLocation::Settings() const {
    VoiceOverSettings settings;
    settings.python = python.path;
    settings.script = script.path;
    settings.models = models.path;
    settings.refusal = refusal;
    return settings;
}

VoiceOverLocation LocateVoiceOver(const VoiceOverSearch& search) {
    tool_files::Context context;
    context.executable_directory = search.executable_directory;
    context.working_directory = search.working_directory;
    context.is_file = search.is_file;
    context.is_folder = search.is_folder;
    VoiceOverLocation location;
    std::vector<std::string> problems;
    std::vector<const tool_files::Role*> roles;
    const auto locate = [&](const tool_files::Role& role,
                            const std::string& configured,
                            const std::string& build) {
        std::string problem;
        tool_files::File file =
            tool_files::Locate(role, configured, build, context, problem);
        if (!problem.empty()) {
            problems.push_back(std::move(problem));
            roles.push_back(&role);
        }
        return file;
    };
    location.python =
        locate(kPython, search.python_variable, search.build_python);
    location.script =
        locate(kScript, search.script_variable, search.build_script);
    location.models =
        locate(kModels, search.models_variable, search.build_models);
    if (location.models.Found()) {
        const std::function<bool(const std::string&)> isFile =
            search.is_file
                ? search.is_file
                : std::function<bool(const std::string&)>(IsRegularFile);
        location.missing_models = MissingModels(location.models.path, isFile);
        if (!location.missing_models.empty()) {
            problems.push_back(MissingModelsProblem(location.models.path,
                                                    location.missing_models));
            roles.push_back(&kModels);
        }
    }
    if (!problems.empty()) {
        location.refusal = tool_files::Refusal(kNotFoundPrefix, problems, roles,
                                               search.settings_file);
    }
    return location;
}

VoiceOverSearch CurrentVoiceOverSearch() {
    VoiceOverSearch search;
    search.python_variable = local_env::Value(kVoicePythonVariable);
    search.script_variable = local_env::Value(kVoiceScriptVariable);
    search.models_variable = local_env::Value(kVoiceModelsVariable);
    search.executable_directory = ffmpeg::ExecutableDirectory();
    search.build_python = CUTMACHINE_VOICE_PYTHON_DEFAULT;
    search.build_script = CUTMACHINE_VOICE_SCRIPT_DEFAULT;
    search.build_models = CUTMACHINE_VOICE_MODELS_DEFAULT;
    std::error_code error;
    const std::filesystem::path workingDirectory =
        std::filesystem::current_path(error);
    if (!error) {
        search.working_directory = workingDirectory.string();
    }
    search.settings_file = local_env::LocalEnvFilePath();
    return search;
}

std::string VoiceOverRefusalName(const std::string& message) {
    return message.find(kNotFoundPrefix) != std::string::npos
               ? kVoiceOverNotFoundErrorName
               : std::string();
}

std::string VoiceOverLanguageName(const std::string& code) {
    for (const auto& [iso, name] : kLanguages) {
        if (code == iso) {
            return name;
        }
    }
    return std::string();
}

std::vector<std::string> VoiceOverLanguageCodes() {
    std::vector<std::string> codes;
    for (const auto& language : kLanguages) {
        codes.emplace_back(language.first);
    }
    return codes;
}

std::vector<std::string> VoiceOverArguments(const VoiceOverSettings& settings,
                                            const VoiceOverRequest& request) {
    const bool design = request.mode == VoiceOverMode::kDesign;
    const std::string model = (std::filesystem::path(settings.models) /
                               (design ? kVoiceDesignModel : kVoiceCloneModel))
                                  .string();
    std::vector<std::string> arguments = {
        settings.python,
        settings.script,
        design ? "design" : "clone",
        "--model",
        model,
        "--text",
        request.text,
        "--language",
        VoiceOverLanguageName(request.language),
        "--seed",
        std::to_string(request.seed),
        "--out",
        request.output_path,
        "--report",
        request.report_path};
    if (design) {
        arguments.insert(arguments.end(),
                         {"--description", request.description});
    } else {
        arguments.insert(arguments.end(),
                         {"--reference", request.reference_audio,
                          "--reference-text", request.reference_text});
    }
    return arguments;
}

namespace {

// What the request lacks for the script to serve it; empty when nothing.
std::string RequestProblem(const VoiceOverRequest& request) {
    if (request.text.empty()) {
        return "the text to say is empty";
    }
    if (VoiceOverLanguageName(request.language).empty()) {
        std::string codes;
        for (const std::string& code : VoiceOverLanguageCodes()) {
            codes += (codes.empty() ? "" : ", ") + code;
        }
        return "the voice-over models do not say the language '" +
               request.language + "' (they say " + codes + ")";
    }
    if (request.mode == VoiceOverMode::kDesign && request.description.empty()) {
        return "a voice is made from a description, and it is empty";
    }
    if (request.mode == VoiceOverMode::kClone &&
        (request.reference_audio.empty() || request.reference_text.empty())) {
        return "a voice is cloned from a reference clip and its words";
    }
    if (request.output_path.empty() || request.report_path.empty()) {
        return "the voice-over needs a WAV path and a report path";
    }
    return std::string();
}

bool ReadReport(const std::string& path, VoiceOverResult& result,
                std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "the voice-over script wrote no report at '" + path + "'";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    mcp_json::Value report;
    std::string parseError;
    if (!mcp_json::Value::Parse(text, report, parseError) ||
        !report.IsObject()) {
        error = "the voice-over report is not a JSON object: " + parseError;
        return false;
    }
    const auto integer = [&report](const char* key, std::int64_t& value) {
        const mcp_json::Value* field = report.Find(key);
        return field != nullptr && field->AsInt64(value);
    };
    const auto text_of = [&report](const char* key) {
        const mcp_json::Value* field = report.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };
    std::int64_t seed = 0;
    if (!integer("sample_rate", result.sample_rate) ||
        !integer("samples", result.samples) || !integer("seed", seed) ||
        result.sample_rate <= 0 || result.samples <= 0 || seed < 0 ||
        seed > UINT32_MAX) {
        error =
            "the voice-over report lacks a positive sample_rate, "
            "samples or seed: " +
            text;
        return false;
    }
    result.seed = static_cast<std::uint32_t>(seed);
    result.model = text_of("model");
    result.revision = text_of("revision");
    result.device = text_of("device");
    return true;
}

}  // namespace

bool RunVoiceOver(const VoiceOverSettings& settings,
                  const VoiceOverRequest& request, MediaTaskContext& context,
                  VoiceOverResult& result, std::string& error) {
    if (const std::string problem = RequestProblem(request); !problem.empty()) {
        error = problem;
        return false;
    }
    // A report or a WAV left by an earlier run must not pass for this one.
    std::error_code removeError;
    std::filesystem::remove(request.report_path, removeError);
    std::filesystem::remove(request.output_path, removeError);

    process::Request launch;
    launch.arguments = VoiceOverArguments(settings, request);
    launch.discard_input = true;
    launch.standard_output = process::Stream::kDiscard;
    launch.standard_error = process::Stream::kPipe;
    launch.error_tail_bytes = 4096;
    launch.cancelled = [&context] { return context.Cancelled(); };
    launch.on_started = [&context] { context.SetProgress(0.0, "Voix off"); };
    const process::Result outcome = process::Run(launch);
    if (!outcome.Started()) {
        error = "unable to start '" + settings.python +
                "': " + outcome.SystemErrorText();
        return false;
    }
    if (outcome.cancelled || context.Cancelled()) {
        error = "voice-over cancelled";
        return false;
    }
    if (!outcome.Succeeded()) {
        error = outcome.error_tail.empty() ? "the voice-over script failed"
                                           : outcome.error_tail;
        return false;
    }
    if (!ReadReport(request.report_path, result, error)) {
        return false;
    }
    std::error_code sizeError;
    const std::uintmax_t bytes =
        std::filesystem::file_size(request.output_path, sizeError);
    if (sizeError || bytes == 0) {
        error = "the voice-over script reported a WAV it did not write at '" +
                request.output_path + "'";
        return false;
    }
    return true;
}
