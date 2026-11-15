#include "SetupCheck.h"

#include "FfmpegBinary.h"
#include "LocalEnv.h"
#include "ProjectArchive.h"
#include "Transcription.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>
#include <utility>

namespace setup {
namespace {

using mcp_json::Value;

// "A", "A and B", "A, B and C".
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

// Where a machine-local setting goes, as a repair says it.
std::string WhereSettingsGo(const std::string& settingsFile) {
    return settingsFile.empty()
               ? std::string("in the environment")
               : "in the environment or in '" + settingsFile + "'";
}

// Where a setting was read, as a sentence says it.
std::string ReadFrom(SettingOrigin origin, const std::string& settingsFile) {
    return origin == SettingOrigin::kSettingsFile
               ? "from '" + settingsFile + "'"
               : std::string("from the environment");
}

Item FfmpegItem(const Machine& machine) {
    Item item;
    item.id = ffmpeg::kDiagnosisKey;
    item.required = true;
    item.prevents =
        "the engine runs no FFmpeg: rushes are ingested without their sound "
        "level, thumbnails, waveforms or proxies, and the analyses of sound "
        "and picture, transcription, contact sheets, motion videos and every "
        "export refuse";
    const ffmpeg::Diagnosis diagnosis =
        machine.ffmpeg ? machine.ffmpeg() : ffmpeg::Diagnosis();
    const std::string variable = ffmpeg::kBinaryVariable;
    const std::string where = WhereSettingsGo(machine.settings_file);
    if (!diagnosis.location.Found()) {
        item.state = State::kMissing;
        // A machine that does not say where it looked has its refusal
        // empty: the item still says something.
        item.detail = diagnosis.location.refusal.empty()
                          ? std::string("FFmpeg not found")
                          : diagnosis.Detail();
        item.error = diagnosis.ErrorName();
        item.repair =
            "Install FFmpeg, then set " + variable + " to its path, " + where;
        return item;
    }
    const std::string& path = diagnosis.location.path;
    const std::string origin = ffmpeg::OriginName(diagnosis.location.origin);
    item.found.Set("path", Value::MakeString(path));
    item.found.Set("origin", Value::MakeString(origin));
    if (diagnosis.probe.answered && !diagnosis.probe.version.empty()) {
        item.found.Set("version", Value::MakeString(diagnosis.probe.version));
    }
    if (diagnosis.Complete()) {
        item.state = State::kOk;
        item.detail =
            "FFmpeg" +
            (diagnosis.probe.version.empty() ? std::string()
                                             : " " + diagnosis.probe.version) +
            " at '" + path + "' (" + origin +
            ") has every filter and encoder the engine uses";
        return item;
    }
    item.state = State::kIncomplete;
    item.detail = diagnosis.Detail();
    item.error = diagnosis.ErrorName();
    if (!diagnosis.probe.answered) {
        item.repair = "Check that '" + path + " -version' runs, or set " +
                      variable + " to an FFmpeg that does, " + where;
        return item;
    }
    std::vector<std::string> options;
    for (const ffmpeg::Capability* capability : diagnosis.missing) {
        Value row = Value::MakeObject();
        row.Set("kind", Value::MakeString(ffmpeg::KindName(capability->kind)));
        row.Set("name", Value::MakeString(capability->name));
        row.Set("prevents", Value::MakeString(capability->prevents));
        item.missing.Push(std::move(row));
        if (std::find(options.begin(), options.end(),
                      capability->build_option) == options.end()) {
            options.emplace_back(capability->build_option);
        }
    }
    item.repair = "Install an FFmpeg built with " + Enumeration(options) +
                  ", or set " + variable + " to one that is, " + where;
    return item;
}

Item WhisperModelItem(const Machine& machine) {
    Item item;
    item.id = kWhisperModelItem;
    item.prevents =
        "rushes cannot be transcribed: transcribe_media, "
        "transcribe_timeline and inspect_cut with transcribe refuse, as do "
        "--transcribe without a model path, --transcribe-timeline and "
        "--srt-from-media";
    item.tools = {"transcribe_media", "transcribe_timeline", "inspect_cut"};
    // DEC23 -- the window's transcription menus read this setting too, and
    // « CUTMACHINE > Modèle Whisper… » writes it: there is no other model.
    const std::string variable = kWhisperModelVariable;
    const std::string where = WhereSettingsGo(machine.settings_file);
    const Setting setting =
        machine.setting ? machine.setting(variable) : Setting();
    const auto fileAt = [&machine](const std::string& path) {
        return machine.file ? machine.file(path) : FileStatus();
    };
    std::string path;
    std::string reason;
    if (!ResolveWhisperModel(
            setting.value, machine.settings_file,
            [&fileAt](const std::string& candidate) {
                return fileAt(candidate).regular;
            },
            path, reason)) {
        item.state = State::kMissing;
        item.detail = reason;
        item.repair = "Set " + variable +
                      " to a local whisper.cpp ggml model file "
                      "(ggml-large-v3.bin, about 3 GB), " +
                      where;
        return item;
    }
    const FileStatus status = fileAt(path);
    const std::string from = ReadFrom(setting.origin, machine.settings_file);
    item.found.Set("path", Value::MakeString(path));
    item.found.Set("bytes", Value::MakeInt(static_cast<int64_t>(status.size)));
    item.found.Set("origin",
                   Value::MakeString(SettingOriginName(setting.origin)));
    if (!status.readable || status.size == 0) {
        item.state = State::kIncomplete;
        item.detail = "the Whisper model '" + path + "', " + from + ", " +
                      (!status.readable ? "cannot be read" : "is empty");
        item.repair = "Put a complete, readable ggml model file at '" + path +
                      "', or set " + variable + " to one, " + where;
        return item;
    }
    item.state = State::kOk;
    item.detail = "the Whisper model is '" + path + "' (" +
                  std::to_string(status.size) + " bytes), " + from;
    return item;
}

// AL4 -- docs/roadmap/ALIGNEMENT-2026-11.md. The s075 weights serve French
// verbatim only, from a setting of their own. Optional: without them, French
// verbatim decodes with the model and the default profile.
Item WhisperHesitationsModelItem(const Machine& machine) {
    Item item;
    item.id = kWhisperHesitationsModelItem;
    item.prevents =
        "French verbatim transcription decodes with the Whisper model and "
        "the default profile, not with fr-hesitations-v1 on the s075 weights";
    item.tools = {"transcribe_media", "transcribe_timeline"};
    const std::string variable = kWhisperHesitationsModelVariable;
    const std::string where = WhereSettingsGo(machine.settings_file);
    const std::string repair = "Set " + variable +
                               " to the evaluated s075 weights (SHA-256 " +
                               kFrenchHesitationsModelSha256 + "), " + where;
    const Setting setting =
        machine.setting ? machine.setting(variable) : Setting();
    if (setting.value.empty()) {
        item.state = State::kOptionalAbsent;
        item.detail = variable + " is not set";
        item.repair = repair;
        return item;
    }
    const std::string& path = setting.value;
    const FileStatus status = machine.file ? machine.file(path) : FileStatus();
    const std::string from = ReadFrom(setting.origin, machine.settings_file);
    item.found.Set("path", Value::MakeString(path));
    item.found.Set("bytes", Value::MakeInt(static_cast<int64_t>(status.size)));
    item.found.Set("origin",
                   Value::MakeString(SettingOriginName(setting.origin)));
    if (!status.regular || !status.readable || status.size == 0) {
        item.state = State::kIncomplete;
        item.detail = "the hesitations weights '" + path + "', " + from +
                      ", are not a readable, non-empty file";
        item.repair = repair;
        return item;
    }
    const std::optional<bool> evaluated =
        machine.hesitations_weights ? machine.hesitations_weights(path)
                                    : std::nullopt;
    item.found.Set("verified", Value::MakeBool(evaluated.has_value()));
    if (evaluated && !*evaluated) {
        item.found.Set("evaluated", Value::MakeBool(false));
        item.state = State::kIncomplete;
        item.detail = "'" + path + "', " + from +
                      ", is not the evaluated s075 weights: their SHA-256 "
                      "differs";
        item.repair = repair;
        return item;
    }
    item.state = State::kOk;
    if (evaluated) {
        item.found.Set("evaluated", Value::MakeBool(true));
    }
    item.detail = "the hesitations weights are '" + path + "' (" +
                  std::to_string(status.size) + " bytes), " + from +
                  (evaluated ? ", their SHA-256 verified"
                             : ", their SHA-256 not verified");
    return item;
}

Item MfaItem(const Machine& machine) {
    Item item;
    item.id = kMfaItem;
    item.prevents =
        "word boundaries are not aligned with MFA: align_words and "
        "--align-words refuse, and cuts stay on Whisper's boundaries";
    item.tools = {"align_words"};
    MfaSearch search = machine.mfa;
    if (!search.is_file) {
        search.is_file = [&machine](const std::string& path) {
            return machine.file && machine.file(path).regular;
        };
    }
    const MfaLocation location = LocateMfa(search);
    struct Part {
        const char* role;
        const char* variable;
        const char* what;
        const MfaFile* file;
    };
    const Part parts[] = {
        {"python", kMfaPythonVariable, "MFA's Python", &location.python},
        {"aligner", kMfaAlignerVariable, "MFA's mfa", &location.aligner},
        {"script", kMfaScriptVariable, "scripts/mfa_align.py",
         &location.script},
    };
    std::vector<std::string> found;
    std::vector<std::string> settings;
    for (const Part& part : parts) {
        if (part.file->Found()) {
            Value file = Value::MakeObject();
            file.Set("path", Value::MakeString(part.file->path));
            file.Set("origin", Value::MakeString(MfaOriginName(
                                   part.file->origin, part.variable)));
            item.found.Set(part.role, std::move(file));
            found.push_back(std::string(part.what) + " '" + part.file->path +
                            "'");
        } else {
            item.missing.Push(Value::MakeString(part.role));
            settings.push_back(std::string(part.variable) + " to " + part.what);
        }
    }
    if (location.Found()) {
        item.state = State::kOk;
        item.detail = Enumeration(found) + " were found; Python was not run";
        return item;
    }
    // Without its Python nothing of MFA runs; with it, a file short.
    item.state = location.python.Found() ? State::kIncomplete : State::kMissing;
    item.detail = location.refusal;
    item.error = kMfaNotFoundErrorName;
    item.repair = "Set " + Enumeration(settings) + ", " +
                  WhereSettingsGo(search.settings_file) +
                  ". Without a setting, a checkout's build/mfa/env and "
                  "scripts/ are found beside the executable, whatever the "
                  "working directory; the models go in build/mfa/models. "
                  "Nothing is downloaded or installed";
    return item;
}

Item AgentKeyItem(const Machine& machine) {
    Item item;
    item.id = kAgentKeyItem;
    item.prevents =
        "the window's agent panel cannot call Anthropic unless a key is "
        "saved in its own settings; agents that drive the engine over MCP "
        "bring their own";
    const std::string variable = kAgentKeyVariable;
    // Only whether it is set, and where: the value is never copied into the
    // report, not even in part.
    const Setting key = machine.setting ? machine.setting(variable) : Setting();
    const std::string keychain =
        ". The window's agent panel also reads a key saved in the Keychain, "
        "which this report does not consult";
    if (!key.value.empty()) {
        item.state = State::kOk;
        item.found.Set("variable", Value::MakeString(variable));
        item.found.Set("origin",
                       Value::MakeString(SettingOriginName(key.origin)));
        item.detail = variable + " is set, " +
                      ReadFrom(key.origin, machine.settings_file) +
                      "; its value is never shown" + keychain;
        return item;
    }
    item.state = State::kOptionalAbsent;
    item.detail = variable + " is set neither in the environment nor" +
                  (machine.settings_file.empty()
                       ? std::string(" in a settings file")
                       : " in '" + machine.settings_file + "'") +
                  keychain;
    item.repair =
        "Save a key in the agent panel's settings (it is kept in the "
        "Keychain), or set " +
        variable + " " + WhereSettingsGo(machine.settings_file);
    return item;
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The archive's daily copy, off
// the disk it lives on. Optional: without it nothing refuses, the archive
// is only as safe as this disk.
Item ArchiveBackupItem(const Machine& machine) {
    Item item;
    item.id = kArchiveBackupItem;
    item.prevents =
        "the project archive is copied nowhere: it lives only on this "
        "machine's disk";
    item.tools = {"backup_project_archive"};
    const std::string variable = project_archive::kBackupDirectoryVariable;
    const std::string where = WhereSettingsGo(machine.settings_file);
    const Setting setting =
        machine.setting ? machine.setting(variable) : Setting();
    const std::string kept = std::to_string(project_archive::kKeptBackups);
    if (setting.value.empty() || setting.value == "off") {
        item.state = State::kOptionalAbsent;
        item.detail = variable +
                      (setting.value.empty() ? " is not set" : " is off") +
                      ": no copy of the project archive is made";
        item.error = "NotConfigured";
        item.repair = "Set " + variable +
                      " to an existing absolute folder that a sync client "
                      "carries elsewhere " +
                      where +
                      "; saves then copy the archive there once a "
                      "day, keeping the " +
                      kept + " newest copies";
        return item;
    }
    Value found = Value::MakeObject();
    found.Set("directory", Value::MakeString(setting.value));
    found.Set("origin", Value::MakeString(SettingOriginName(setting.origin)));
    item.found = std::move(found);
    if (!std::filesystem::path(setting.value).is_absolute()) {
        item.state = State::kIncomplete;
        item.detail = variable + " is '" + setting.value +
                      "', not an absolute folder: no copy is made";
        item.error = "ValidationFailed";
        item.repair = "Set " + variable + " to an absolute folder " + where;
        return item;
    }
    if (!machine.directory || !machine.directory(setting.value)) {
        item.state = State::kIncomplete;
        item.detail = "the folder '" + setting.value + "' set by " + variable +
                      " " + ReadFrom(setting.origin, machine.settings_file) +
                      " does not exist (a sync client not mounted?): no "
                      "copy is made, and the folder is never created";
        item.error = "IoError";
        item.repair = "Mount or create '" + setting.value + "', or set " +
                      variable + " to another folder " + where;
        return item;
    }
    item.state = State::kOk;
    item.detail = "saves copy the project archive into '" + setting.value +
                  "' once a day, keeping the " + kept + " newest copies (" +
                  variable + " " +
                  ReadFrom(setting.origin, machine.settings_file) + ")";
    return item;
}

// GN6a -- docs/roadmap/GENERATIF-2026-10.md. The local voice-over model.
// Optional: nothing the engine requires depends on it. Absent when nothing
// of it is set up -- no Python found, no variable set --, incomplete when
// something is there but not all of it.
Item VoiceOverItem(const Machine& machine) {
    Item item;
    item.id = kVoiceOverItem;
    item.prevents =
        "no voice-over can be generated on this machine: the local "
        "Qwen3-TTS model (GN6) is not found";
    VoiceOverSearch search = machine.voice_over;
    if (!search.is_file) {
        search.is_file = [&machine](const std::string& path) {
            return machine.file && machine.file(path).regular;
        };
    }
    if (!search.is_folder) {
        search.is_folder = [&machine](const std::string& path) {
            return machine.directory && machine.directory(path);
        };
    }
    const VoiceOverLocation location = LocateVoiceOver(search);
    struct Part {
        const char* role;
        const char* variable;
        const char* what;
        const tool_files::File* file;
    };
    const Part parts[] = {
        {"python", kVoicePythonVariable, "the voice-over's Python",
         &location.python},
        {"script", kVoiceScriptVariable, "scripts/voice_over.py",
         &location.script},
        {"models", kVoiceModelsVariable, "the folder of the voice-over models",
         &location.models},
    };
    std::vector<std::string> found;
    std::vector<std::string> settings;
    for (const Part& part : parts) {
        if (part.file->Found()) {
            Value file = Value::MakeObject();
            file.Set("path", Value::MakeString(part.file->path));
            file.Set("origin", Value::MakeString(tool_files::OriginName(
                                   part.file->origin, part.variable)));
            item.found.Set(part.role, std::move(file));
            found.push_back(std::string(part.what) + " '" + part.file->path +
                            "'");
        } else {
            item.missing.Push(Value::MakeString(part.role));
            settings.push_back(std::string(part.variable) + " to " + part.what);
        }
    }
    for (const std::string& model : location.missing_models) {
        item.missing.Push(Value::MakeString(model));
    }
    if (location.Found()) {
        item.state = State::kOk;
        item.detail = Enumeration(found) +
                      " were found, with both models; Python was not run";
        return item;
    }
    const bool nothing =
        !location.python.Found() && search.python_variable.empty() &&
        search.script_variable.empty() && search.models_variable.empty();
    item.state = nothing ? State::kOptionalAbsent : State::kIncomplete;
    item.detail = location.refusal;
    item.error = kVoiceOverNotFoundErrorName;
    if (settings.empty()) {
        settings.push_back(std::string(kVoiceModelsVariable) +
                           " to the folder of the voice-over models");
    }
    item.repair = "Set " + Enumeration(settings) + ", " +
                  WhereSettingsGo(search.settings_file) +
                  ". Without a setting, a checkout's build/voice/env (a "
                  "Python with qwen-tts 0.1.1) and build/voice/models (the "
                  "folders " +
                  std::string(kVoiceDesignModel) + " and " + kVoiceCloneModel +
                  ", as `hf download --local-dir` writes them) are found "
                  "beside the executable, and scripts/ beside build/. "
                  "Nothing is downloaded or installed";
    return item;
}

Value ItemJson(const Item& item) {
    Value row = Value::MakeObject();
    row.Set("id", Value::MakeString(item.id));
    row.Set("state", Value::MakeString(StateName(item.state)));
    row.Set("required", Value::MakeBool(item.required));
    row.Set("prevents", Value::MakeString(item.prevents));
    if (!item.tools.empty()) {
        Value tools = Value::MakeArray();
        for (const std::string& tool : item.tools) {
            tools.Push(Value::MakeString(tool));
        }
        row.Set("tools", std::move(tools));
    }
    if (!item.found.IsNull()) {
        row.Set("found", item.found);
    }
    if (!item.missing.IsNull()) {
        row.Set("missing", item.missing);
    }
    row.Set("detail", Value::MakeString(item.detail));
    if (!item.error.empty()) {
        row.Set("error", Value::MakeString(item.error));
    }
    if (!item.repair.empty()) {
        row.Set("repair", Value::MakeString(item.repair));
    }
    return row;
}

// As local_env::Value reads it, and from where: the settings file never
// overwrites an exported variable, so a value the file gives too came from
// it, and any other one from the environment. Whether the file was loaded
// into this process already does not change the answer.
Setting ReadSetting(const std::string& name) {
    Setting setting;
    const std::string fromFile = local_env::FileValue(name.c_str());
    const char* exported = std::getenv(name.c_str());
    if (exported != nullptr && *exported != '\0') {
        setting.value = exported;
        setting.origin = !fromFile.empty() && fromFile == setting.value
                             ? SettingOrigin::kSettingsFile
                             : SettingOrigin::kEnvironment;
        return setting;
    }
    // The file only fills a variable that is unset: one exported empty
    // hides the file's value from the engine, and so from this report.
    if (exported == nullptr && !fromFile.empty()) {
        setting.value = fromFile;
        setting.origin = SettingOrigin::kSettingsFile;
    }
    return setting;
}

FileStatus ReadFileStatus(const std::string& path) {
    FileStatus status;
    std::error_code error;
    status.regular = std::filesystem::is_regular_file(path, error);
    if (status.regular) {
        status.readable = ::access(path.c_str(), R_OK) == 0;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        status.size = error ? 0 : size;
    }
    return status;
}

}  // namespace

const char* StateName(State state) {
    switch (state) {
        case State::kOk:
            return "ok";
        case State::kMissing:
            return "missing";
        case State::kIncomplete:
            return "incomplete";
        case State::kOptionalAbsent:
            return "optional_absent";
    }
    return "missing";
}

const char* SettingOriginName(SettingOrigin origin) {
    switch (origin) {
        case SettingOrigin::kEnvironment:
            return "environment";
        case SettingOrigin::kSettingsFile:
            return "settings_file";
        case SettingOrigin::kUnset:
            break;
    }
    return "";
}

bool Report::Ok() const { return MissingRequired() == nullptr; }

const Item* Report::MissingRequired() const {
    for (const Item& item : items) {
        if (item.required && item.state == State::kMissing) {
            return &item;
        }
    }
    return nullptr;
}

const Item* Report::Find(const std::string& id) const {
    for (const Item& item : items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

Machine CurrentMachine() {
    Machine machine;
    machine.setting = ReadSetting;
    machine.file = ReadFileStatus;
    machine.settings_file = local_env::LocalEnvFilePath();
    machine.ffmpeg = [] { return ffmpeg::Diagnose(); };
    machine.mfa = CurrentMfaSearch();
    machine.directory = [](const std::string& path) {
        std::error_code error;
        return std::filesystem::is_directory(path, error);
    };
    machine.voice_over = CurrentVoiceOverSearch();
    machine.hesitations_weights =
        [](const std::string& path) -> std::optional<bool> {
        bool evaluated = false;
        std::string error;
        if (!RecognizeWhisperDecodingModel(path, std::function<bool()>(),
                                           evaluated, error)) {
            return std::nullopt;
        }
        return evaluated;
    };
    return machine;
}

Report CheckSetup(const Machine& machine) {
    Report report;
    report.items.push_back(FfmpegItem(machine));
    report.items.push_back(WhisperModelItem(machine));
    report.items.push_back(WhisperHesitationsModelItem(machine));
    report.items.push_back(MfaItem(machine));
    report.items.push_back(AgentKeyItem(machine));
    report.items.push_back(ArchiveBackupItem(machine));
    report.items.push_back(VoiceOverItem(machine));
    return report;
}

Value ReportJson(const Report& report) {
    Value root = Value::MakeObject();
    root.Set("ok", Value::MakeBool(report.Ok()));
    if (const Item* missing = report.MissingRequired()) {
        root.Set("error", Value::MakeString(missing->error));
        root.Set("detail", Value::MakeString(missing->detail));
    }
    Value items = Value::MakeArray();
    for (const Item& item : report.items) {
        items.Push(ItemJson(item));
    }
    root.Set("items", std::move(items));
    return root;
}

}  // namespace setup
