// PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. What this machine lacks, said
// before a tool fails on it.
//
// Every item of the setup report in every state it can take, on a machine
// the test makes up (setup::Machine): its settings, its files, its FFmpeg
// diagnosis and the rest of what the report reads. Nothing here reads $HOME,
// the real key or the Keychain, and no FFmpeg is launched. Then the order MFA
// is looked for in (MfaAlignment.h), and the two pieces of the real
// machine that can be tried safely: where a setting comes from, on a
// settings file the test writes, and what is at a path.
//
// Plain C++ and POSIX: no AppKit, no FFmpeg, no network.

#include "FfmpegCapabilities.h"
#include "Json.h"
#include "LocalEnv.h"
#include "MfaAlignment.h"
#include "SetupCheck.h"
#include "Ulid.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using mcp_json::Value;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) {
        std::cout << "PASS: " << name << '\n';
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

constexpr const char* kSettingsFile = "/machine/.config/cutmachine/.env";
constexpr const char* kFfmpegPath = "/opt/ffmpeg/bin/ffmpeg";
constexpr const char* kModelPath = "/models/ggml-large-v3.bin";
constexpr const char* kHesitationsPath =
    "/models/ggml-large-v3-hesitations-s075.bin";
constexpr const char* kBackupFolder = "/Shared/OneDrive/Copies";

// A machine made of maps. Everything absent from them is absent from the
// machine.
struct FakeMachine {
    std::map<std::string, setup::Setting> settings;
    std::map<std::string, setup::FileStatus> files;
    ffmpeg::Diagnosis ffmpeg;
    MfaSearch mfa;
    std::set<std::string> directories;
    VoiceOverSearch voice_over;
    // AL4 -- the paths whose SHA-256 was verified, and the verdict.
    std::map<std::string, bool> verified_weights;

    void AddFile(const std::string& path, std::uintmax_t size = 1024,
                 bool readable = true) {
        files[path] = {true, readable, size};
    }

    void Set(const std::string& name, const std::string& value,
             setup::SettingOrigin origin) {
        settings[name] = {value, origin};
    }

    // The machine reads this object's maps when the report runs, so a test
    // may change them between two reports.
    setup::Machine Make() const {
        setup::Machine machine;
        machine.setting = [this](const std::string& name) {
            const auto found = settings.find(name);
            return found == settings.end() ? setup::Setting() : found->second;
        };
        machine.file = [this](const std::string& path) {
            const auto found = files.find(path);
            return found == files.end() ? setup::FileStatus() : found->second;
        };
        machine.settings_file = kSettingsFile;
        machine.ffmpeg = [this] { return ffmpeg; };
        machine.mfa = mfa;
        machine.directory = [this](const std::string& path) {
            return directories.count(path) != 0;
        };
        machine.voice_over = voice_over;
        machine.hesitations_weights =
            [this](const std::string& path) -> std::optional<bool> {
            const auto found = verified_weights.find(path);
            if (found == verified_weights.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        return machine;
    }
};

ffmpeg::Diagnosis FfmpegNotFound() {
    ffmpeg::Diagnosis diagnosis;
    diagnosis.location.refusal =
        "FFmpeg not found: CUTMACHINE_FFMPEG is not set, there is no "
        "'ffmpeg' beside the executable, and none in the directories of PATH "
        "('/usr/bin:/bin'). Set CUTMACHINE_FFMPEG to the path of an FFmpeg "
        "binary, in the environment or in '/machine/.config/cutmachine/.env'";
    return diagnosis;
}

// A binary that answered its probe and has every capability but those of
// `lacking`.
ffmpeg::Diagnosis FfmpegFound(const std::set<std::string>& lacking = {}) {
    ffmpeg::Diagnosis diagnosis;
    diagnosis.location.path = kFfmpegPath;
    diagnosis.location.origin = ffmpeg::Origin::kConfigured;
    diagnosis.probe.path = kFfmpegPath;
    diagnosis.probe.answered = true;
    diagnosis.probe.version = "8.1";
    for (const ffmpeg::Capability& capability : ffmpeg::KnownCapabilities()) {
        if (lacking.count(capability.name) != 0) {
            diagnosis.missing.push_back(&capability);
            continue;
        }
        (capability.kind == ffmpeg::CapabilityKind::kFilter
             ? diagnosis.probe.filters
             : diagnosis.probe.encoders)
            .insert(capability.name);
    }
    return diagnosis;
}

// Where the made-up checkout installs MFA, and its build.
constexpr const char* kBuild = "/repo/build";
constexpr const char* kBuildPython = "/repo/build/mfa/env/bin/python";
constexpr const char* kBuildAligner = "/repo/build/mfa/env/bin/mfa";
constexpr const char* kSourceScript = "/repo/scripts/mfa_align.py";

MfaSearch CheckoutSearch() {
    MfaSearch search;
    search.executable_directory = kBuild;
    search.build_python = kBuildPython;
    search.build_aligner = kBuildAligner;
    search.build_script = kSourceScript;
    search.working_directory = "/somewhere/else";
    search.settings_file = kSettingsFile;
    return search;
}

void AddCheckoutMfa(FakeMachine& machine) {
    machine.AddFile(kBuildPython);
    machine.AddFile(kBuildAligner);
    machine.AddFile(kSourceScript);
}

// GN6a -- where the made-up checkout installs the voice-over model.
constexpr const char* kVoicePython = "/repo/build/voice/env/bin/python";
constexpr const char* kVoiceScript = "/repo/scripts/voice_over.py";
constexpr const char* kVoiceModels = "/repo/build/voice/models";

VoiceOverSearch VoiceCheckoutSearch() {
    VoiceOverSearch search;
    search.executable_directory = kBuild;
    search.working_directory = "/somewhere/else";
    search.settings_file = kSettingsFile;
    return search;
}

// The voice-over's files, but for `skip`.
void AddCheckoutVoice(FakeMachine& machine,
                      const std::set<std::string>& skip = {}) {
    if (skip.count(kVoicePython) == 0) {
        machine.AddFile(kVoicePython);
    }
    if (skip.count(kVoiceScript) == 0) {
        machine.AddFile(kVoiceScript);
    }
    machine.directories.insert(kVoiceModels);
    for (const char* folder : {kVoiceDesignModel, kVoiceCloneModel}) {
        for (const std::string& file : VoiceModelFiles()) {
            const std::string path =
                std::string(kVoiceModels) + "/" + folder + "/" + file;
            if (skip.count(path) == 0) {
                machine.AddFile(path);
            }
        }
    }
}

// The machine with everything: the report's ok case.
FakeMachine CompleteMachine() {
    FakeMachine machine;
    machine.ffmpeg = FfmpegFound();
    machine.Set("CUTMACHINE_WHISPER_MODEL", kModelPath,
                setup::SettingOrigin::kSettingsFile);
    machine.AddFile(kModelPath, 3095033483ULL);
    machine.Set("CUTMACHINE_WHISPER_HESITATIONS_MODEL", kHesitationsPath,
                setup::SettingOrigin::kSettingsFile);
    machine.AddFile(kHesitationsPath, 3095033483ULL);
    machine.verified_weights[kHesitationsPath] = true;
    machine.mfa = CheckoutSearch();
    AddCheckoutMfa(machine);
    machine.Set("ANTHROPIC_API_KEY", "sk-test-key-setup",
                setup::SettingOrigin::kEnvironment);
    machine.Set("CUTMACHINE_ARCHIVE_BACKUP_DIR", kBackupFolder,
                setup::SettingOrigin::kSettingsFile);
    machine.directories.insert(kBackupFolder);
    machine.voice_over = VoiceCheckoutSearch();
    AddCheckoutVoice(machine);
    return machine;
}

const setup::Item& ItemOf(const setup::Report& report, const std::string& id) {
    static const setup::Item kNone;
    const setup::Item* item = report.Find(id);
    Check(item != nullptr, "the report has an item " + id);
    return item != nullptr ? *item : kNone;
}

std::string FoundText(const setup::Item& item, const std::string& key) {
    const Value* value = item.found.Find(key);
    return value != nullptr && value->IsString() ? value->AsString()
                                                 : std::string();
}

std::string JsonText(const Value& value, const std::string& key) {
    const Value* found = value.Find(key);
    return found != nullptr && found->IsString() ? found->AsString()
                                                 : std::string();
}

constexpr size_t kItemCount = 7;

void TestReportShape() {
    const FakeMachine complete = CompleteMachine();
    const setup::Report report = setup::CheckSetup(complete.Make());
    const std::vector<std::string> expected = {
        ffmpeg::kDiagnosisKey,
        setup::kWhisperModelItem,
        setup::kWhisperHesitationsModelItem,
        setup::kMfaItem,
        setup::kAgentKeyItem,
        setup::kArchiveBackupItem,
        setup::kVoiceOverItem};
    std::vector<std::string> ids;
    ids.reserve(report.items.size());
    for (const setup::Item& item : report.items) {
        ids.push_back(item.id);
    }
    Check(ids == expected, "every item, in its stable order");
    Check(std::string(ffmpeg::kDiagnosisKey) == "ffmpeg",
          "the FFmpeg item is called ffmpeg");
    for (const setup::Item& item : report.items) {
        Check(item.state == setup::State::kOk,
              item.id + " is ok on the complete machine: " + item.detail);
        Check(item.required == (item.id == ffmpeg::kDiagnosisKey),
              item.id + ": only FFmpeg is required");
        Check(!item.prevents.empty() && !item.detail.empty(),
              item.id + " says what it prevents and what was found");
        Check(item.repair.empty() && item.error.empty(),
              item.id + " has nothing to repair");
    }
    Check(report.Ok() && report.MissingRequired() == nullptr,
          "the complete machine is ok");

    const Value json = setup::ReportJson(report);
    const Value* ok = json.Find("ok");
    const Value* items = json.Find("items");
    Check(ok != nullptr && ok->IsBool() && ok->AsBool() &&
              json.Find("error") == nullptr && items != nullptr &&
              items->IsArray() && items->AsArray().size() == kItemCount,
          "{\"ok\":true,\"items\":[every item]}: " + json.Dump());
    if (items != nullptr && items->IsArray() &&
        items->AsArray().size() == kItemCount) {
        const Value& first = items->AsArray()[0];
        Check(JsonText(first, "id") == "ffmpeg" &&
                  JsonText(first, "state") == "ok" &&
                  first.Find("required") != nullptr &&
                  first.Find("required")->AsBool() &&
                  first.Find("found") != nullptr &&
                  JsonText(*first.Find("found"), "path") == kFfmpegPath &&
                  JsonText(*first.Find("found"), "version") == "8.1" &&
                  first.Find("repair") == nullptr,
              "an item reads id, state, required, found: " + first.Dump());
        const Value& whisper = items->AsArray()[1];
        const Value* tools = whisper.Find("tools");
        Check(
            tools != nullptr && tools->IsArray() &&
                tools->AsArray().size() == 3 &&
                tools->AsArray()[0].AsString() == "transcribe_media",
            "the Whisper item lists the tools that need it: " + whisper.Dump());
        Check(items->AsArray()[0].Find("tools") == nullptr,
              "FFmpeg lists no tools: `required` says all of them need it");
    }
    Check(std::string(setup::StateName(setup::State::kOk)) == "ok" &&
              std::string(setup::StateName(setup::State::kMissing)) ==
                  "missing" &&
              std::string(setup::StateName(setup::State::kIncomplete)) ==
                  "incomplete" &&
              std::string(setup::StateName(setup::State::kOptionalAbsent)) ==
                  "optional_absent",
          "the four state names");
}

void TestFfmpegItem() {
    FakeMachine machine = CompleteMachine();

    machine.ffmpeg = FfmpegFound();
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "ffmpeg");
        Check(item.state == setup::State::kOk &&
                  FoundText(item, "path") == kFfmpegPath &&
                  FoundText(item, "origin") == "CUTMACHINE_FFMPEG" &&
                  FoundText(item, "version") == "8.1" &&
                  Contains(item.detail, "has every filter and encoder"),
              "ok: path, origin, version: " + item.detail);
    }

    machine.ffmpeg = FfmpegNotFound();
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "ffmpeg");
        Check(item.state == setup::State::kMissing && item.required &&
                  item.error == "FfmpegNotFound" &&
                  item.detail == machine.ffmpeg.location.refusal &&
                  item.found.IsNull() &&
                  Contains(item.repair, "CUTMACHINE_FFMPEG") &&
                  Contains(item.repair, kSettingsFile),
              "missing: FFmpeg's own refusal, and the setting to repair it: " +
                  item.repair);
        Check(!report.Ok() && report.MissingRequired() == &item,
              "a missing FFmpeg fails the report");
        const Value json = setup::ReportJson(report);
        Check(json.Find("ok") != nullptr && !json.Find("ok")->AsBool() &&
                  JsonText(json, "error") == "FfmpegNotFound" &&
                  JsonText(json, "detail") == item.detail &&
                  json.Find("items") != nullptr &&
                  json.Find("items")->AsArray().size() == kItemCount,
              "ok:false carries FFmpeg's error and detail beside the items: " +
                  json.Dump().substr(0, 300));
    }

    machine.ffmpeg = FfmpegFound({"libx264", "libx265"});
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "ffmpeg");
        Check(item.state == setup::State::kIncomplete &&
                  item.error == "FfmpegCapabilityMissing" &&
                  Contains(item.detail, "lacks 2 of the 5") &&
                  item.missing.IsArray() && item.missing.AsArray().size() == 2,
              "incomplete: the capabilities it lacks: " + item.detail);
        if (item.missing.IsArray() && !item.missing.AsArray().empty()) {
            const Value& row = item.missing.AsArray()[0];
            Check(JsonText(row, "kind") == "encoder" &&
                      JsonText(row, "name") == "libx264" &&
                      Contains(JsonText(row, "prevents"), "H.264"),
                  "each with what its absence prevents: " + row.Dump());
        }
        Check(Contains(item.repair, "--enable-libx264") &&
                  Contains(item.repair, "--enable-libx265") &&
                  Contains(item.repair, "CUTMACHINE_FFMPEG"),
              "the build options to look for: " + item.repair);
        Check(report.Ok(),
              "an incomplete FFmpeg still edits and exports: the report is "
              "ok");
    }

    machine.ffmpeg = FfmpegFound();
    machine.ffmpeg.probe = ffmpeg::Probe();
    machine.ffmpeg.probe.path = kFfmpegPath;
    machine.ffmpeg.probe.failure = "'/opt/ffmpeg/bin/ffmpeg -filters' failed";
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "ffmpeg");
        Check(item.state == setup::State::kIncomplete &&
                  item.error == "FfmpegProbeFailed" &&
                  Contains(item.detail, "could not be probed") &&
                  FoundText(item, "path") == kFfmpegPath &&
                  item.found.Find("version") == nullptr &&
                  Contains(item.repair, "-version' runs"),
              "a binary that cannot be probed is incomplete: " + item.detail);
    }
}

void TestWhisperModelItem() {
    FakeMachine machine = CompleteMachine();
    const auto whisper = [&machine] {
        return ItemOf(setup::CheckSetup(machine.Make()), "whisper_model");
    };

    machine.settings.erase("CUTMACHINE_WHISPER_MODEL");
    {
        const setup::Item item = whisper();
        Check(item.state == setup::State::kMissing && !item.required &&
                  Contains(item.detail, "no Whisper model configured") &&
                  Contains(item.detail, kSettingsFile) &&
                  // DEC23 -- the window has no model of its own to mention.
                  !Contains(item.detail, "transcription menus") &&
                  Contains(item.repair, "CUTMACHINE_WHISPER_MODEL") &&
                  Contains(item.repair, kSettingsFile) && item.found.IsNull(),
              "unset: missing, with the setting and the file: " + item.detail);
    }

    machine.Set("CUTMACHINE_WHISPER_MODEL", "/nope",
                setup::SettingOrigin::kEnvironment);
    {
        const setup::Item item = whisper();
        Check(item.state == setup::State::kMissing &&
                  Contains(item.detail, "does not name a regular file") &&
                  Contains(item.detail, "'/nope'"),
              "a path to nothing: missing, as Transcription.h says it: " +
                  item.detail);
    }

    machine.Set("CUTMACHINE_WHISPER_MODEL", kModelPath,
                setup::SettingOrigin::kEnvironment);
    machine.AddFile(kModelPath, 3095033483ULL, /*readable=*/false);
    {
        const setup::Item item = whisper();
        Check(item.state == setup::State::kIncomplete &&
                  Contains(item.detail, "cannot be read") &&
                  FoundText(item, "path") == kModelPath && !item.repair.empty(),
              "unreadable: incomplete: " + item.detail);
    }

    machine.AddFile(kModelPath, 0);
    {
        const setup::Item item = whisper();
        Check(item.state == setup::State::kIncomplete &&
                  Contains(item.detail, "is empty"),
              "empty: incomplete: " + item.detail);
    }

    machine.AddFile(kModelPath, 3095033483ULL);
    {
        const setup::Item item = whisper();
        int64_t bytes = 0;
        const Value* size = item.found.Find("bytes");
        Check(item.state == setup::State::kOk &&
                  FoundText(item, "path") == kModelPath &&
                  FoundText(item, "origin") == "environment" &&
                  size != nullptr && size->AsInt64(bytes) &&
                  bytes == 3095033483LL &&
                  Contains(item.detail, "from the environment") &&
                  item.repair.empty(),
              "ok: the path, its size and where the setting came from: " +
                  item.detail);
    }

    machine.Set("CUTMACHINE_WHISPER_MODEL", kModelPath,
                setup::SettingOrigin::kSettingsFile);
    {
        const setup::Item item = whisper();
        Check(FoundText(item, "origin") == "settings_file" &&
                  Contains(item.detail, std::string("from '") + kSettingsFile),
              "from the settings file: " + item.detail);
    }
}

// AL4 -- the s075 weights for French verbatim: optional, and verified.
void TestWhisperHesitationsModelItem() {
    FakeMachine machine = CompleteMachine();
    const auto hesitations = [&machine] {
        return ItemOf(setup::CheckSetup(machine.Make()),
                      "whisper_hesitations_model");
    };
    {
        const setup::Item item = hesitations();
        const Value* verified = item.found.Find("verified");
        const Value* evaluated = item.found.Find("evaluated");
        Check(item.state == setup::State::kOk && !item.required &&
                  FoundText(item, "path") == kHesitationsPath &&
                  verified != nullptr && verified->IsBool() &&
                  verified->AsBool() && evaluated != nullptr &&
                  evaluated->IsBool() && evaluated->AsBool() &&
                  Contains(item.detail, "SHA-256 verified") &&
                  item.repair.empty(),
              "the evaluated weights: ok, verified: " + item.detail);
    }

    machine.verified_weights.clear();
    {
        const setup::Item item = hesitations();
        const Value* verified = item.found.Find("verified");
        Check(item.state == setup::State::kOk && verified != nullptr &&
                  verified->IsBool() && !verified->AsBool() &&
                  item.found.Find("evaluated") == nullptr &&
                  Contains(item.detail, "not verified"),
              "weights not verified: ok, and said: " + item.detail);
    }

    machine.verified_weights[kHesitationsPath] = false;
    {
        const setup::Item item = hesitations();
        Check(
            item.state == setup::State::kIncomplete &&
                Contains(item.detail, "not the evaluated s075 weights") &&
                Contains(item.repair, "CUTMACHINE_WHISPER_HESITATIONS_MODEL") &&
                Contains(item.repair, "5b05a0870a122a83"),
            "other weights: incomplete, with the digest to look for: " +
                item.detail);
    }

    machine.files.erase(kHesitationsPath);
    {
        const setup::Item item = hesitations();
        Check(item.state == setup::State::kIncomplete &&
                  Contains(item.detail, "not a readable, non-empty file"),
              "a path to nothing: incomplete: " + item.detail);
    }

    machine.settings.erase("CUTMACHINE_WHISPER_HESITATIONS_MODEL");
    {
        const setup::Item item = hesitations();
        Check(item.state == setup::State::kOptionalAbsent && !item.required &&
                  item.found.IsNull() && Contains(item.repair, kSettingsFile),
              "unset: optional and absent: " + item.detail);
        Check(
            ItemOf(setup::CheckSetup(machine.Make()), "whisper_model").state ==
                setup::State::kOk,
            "the model does not depend on it");
    }
}

void TestMfaItem() {
    FakeMachine machine = CompleteMachine();
    const auto mfa = [&machine] {
        return ItemOf(setup::CheckSetup(machine.Make()), "mfa");
    };

    {
        const setup::Item item = mfa();
        const Value* python = item.found.Find("python");
        Check(item.state == setup::State::kOk &&
                  item.tools == std::vector<std::string>{"align_words"} &&
                  python != nullptr &&
                  JsonText(*python, "path") == kBuildPython &&
                  JsonText(*python, "origin") == "executable" &&
                  item.missing.IsNull() &&
                  Contains(item.detail, "Python was not run"),
              "ok: the three files and where each was found: " +
                  item.found.Dump());
    }

    machine.files.erase(kSourceScript);
    {
        const setup::Item item = mfa();
        Check(item.state == setup::State::kIncomplete &&
                  item.error == "MfaNotFound" && item.missing.IsArray() &&
                  item.missing.AsArray().size() == 1 &&
                  item.missing.AsArray()[0].AsString() == "script" &&
                  item.found.Find("python") != nullptr &&
                  Contains(item.detail, "scripts/mfa_align.py is at none of") &&
                  Contains(item.repair, "CUTMACHINE_MFA_SCRIPT") &&
                  !Contains(item.repair, "CUTMACHINE_MFA_PYTHON"),
              "its Python without the script: incomplete, the script "
              "named: " +
                  item.detail);
    }

    machine.files.erase(kBuildPython);
    {
        const setup::Item item = mfa();
        Check(item.state == setup::State::kMissing &&
                  item.error == "MfaNotFound" &&
                  Contains(item.detail, "MFA not found: ") &&
                  Contains(item.detail, kBuildPython) &&
                  Contains(item.detail,
                           "/somewhere/else/build/mfa/env/bin/python") &&
                  Contains(item.detail, "CUTMACHINE_MFA_PYTHON") &&
                  Contains(item.detail, kSettingsFile) &&
                  Contains(item.repair, "Nothing is downloaded"),
              "no Python: missing, with every place it was looked for: " +
                  item.detail);
    }

    // A variable that names nothing is a refusal: the checkout's files,
    // there beside the executable, are not taken instead.
    AddCheckoutMfa(machine);
    machine.mfa.python_variable = "/nowhere/python";
    {
        const setup::Item item = mfa();
        Check(item.state == setup::State::kMissing &&
                  Contains(item.detail,
                           "CUTMACHINE_MFA_PYTHON names '/nowhere/python', "
                           "which is not a file") &&
                  item.found.Find("python") == nullptr &&
                  item.found.Find("aligner") != nullptr,
              "a variable to nothing is not looked past: " + item.detail);
    }
}

void TestAgentKeyItem() {
    FakeMachine machine = CompleteMachine();
    // Not a key: as plain as the other suites' "sk-test-key", so that a
    // scan for secrets does not take it for one.
    const std::string value = "sk-test-key-setup";

    machine.Set("ANTHROPIC_API_KEY", value, setup::SettingOrigin::kEnvironment);
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "agent_key");
        const std::string json = setup::ReportJson(report).Dump();
        Check(item.state == setup::State::kOk && !item.required &&
                  FoundText(item, "origin") == "environment" &&
                  FoundText(item, "variable") == "ANTHROPIC_API_KEY" &&
                  Contains(item.detail, "Keychain") && item.tools.empty(),
              "set in the environment: ok, and where: " + item.detail);
        Check(!Contains(json, value) && !Contains(json, "sk-test") &&
                  !Contains(json, "key-setup"),
              "the key's value appears nowhere in the report, nor a part "
              "of it");
    }

    machine.Set("ANTHROPIC_API_KEY", value,
                setup::SettingOrigin::kSettingsFile);
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "agent_key");
        Check(FoundText(item, "origin") == "settings_file" &&
                  Contains(item.detail, kSettingsFile) &&
                  !Contains(setup::ReportJson(report).Dump(), "sk-test"),
              "set in the settings file: " + item.detail);
    }

    machine.settings.erase("ANTHROPIC_API_KEY");
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "agent_key");
        Check(item.state == setup::State::kOptionalAbsent &&
                  item.found.IsNull() &&
                  Contains(item.detail, "does not consult") &&
                  Contains(item.repair, "ANTHROPIC_API_KEY") &&
                  Contains(item.repair, kSettingsFile),
              "unset: optional, and the Keychain is said not to be read: " +
                  item.detail);
        Check(report.Ok(), "no key does not fail the report");
    }
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The archive's daily copy: a
// folder that exists, else nothing is copied, and the report never fails
// for it.
void TestArchiveBackupItem() {
    FakeMachine machine = CompleteMachine();
    {
        const setup::Item item =
            ItemOf(setup::CheckSetup(machine.Make()), "archive_backup");
        Check(item.state == setup::State::kOk &&
                  FoundText(item, "directory") == kBackupFolder &&
                  FoundText(item, "origin") == "settings_file" &&
                  Contains(item.detail, "once a day") &&
                  Contains(item.detail, "7 newest") &&
                  item.tools ==
                      std::vector<std::string>{"backup_project_archive"},
              "a folder that exists: ok: " + item.detail);
    }
    machine.directories.clear();
    {
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "archive_backup");
        Check(item.state == setup::State::kIncomplete &&
                  item.error == "IoError" &&
                  Contains(item.detail, "never created") &&
                  Contains(item.repair, kBackupFolder) && report.Ok(),
              "a folder that is not there: incomplete, never a failure: " +
                  item.detail);
    }
    machine.Set("CUTMACHINE_ARCHIVE_BACKUP_DIR", "relative/Copies",
                setup::SettingOrigin::kEnvironment);
    machine.directories.insert("relative/Copies");
    {
        const setup::Item item =
            ItemOf(setup::CheckSetup(machine.Make()), "archive_backup");
        Check(item.state == setup::State::kIncomplete &&
                  item.error == "ValidationFailed",
              "a relative folder is refused: " + item.detail);
    }
    for (const char* value : {"", "off"}) {
        machine.Set("CUTMACHINE_ARCHIVE_BACKUP_DIR", value,
                    setup::SettingOrigin::kSettingsFile);
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "archive_backup");
        Check(item.state == setup::State::kOptionalAbsent &&
                  item.error == "NotConfigured" && item.found.IsNull() &&
                  Contains(item.repair, "CUTMACHINE_ARCHIVE_BACKUP_DIR") &&
                  Contains(item.repair, kSettingsFile) && report.Ok(),
              std::string("unset or off: optional, no copy, ok report (") +
                  value + "): " + item.detail);
    }
}

// A machine that says nothing at all: every item absent, and FFmpeg, the
// one required, fails the report.
// GN6a -- the local voice-over model: absent when nothing of it is set up,
// incomplete when part of it is, the missing roles and model folders named.
void TestVoiceOverItem() {
    std::vector<std::string> missing;
    const auto missingOf = [&missing](const setup::Item& item) {
        missing.clear();
        if (item.missing.IsArray()) {
            for (const Value& value : item.missing.AsArray()) {
                missing.push_back(value.IsString() ? value.AsString() : "?");
            }
        }
        return missing;
    };
    {
        FakeMachine machine = CompleteMachine();
        const setup::Item item =
            ItemOf(setup::CheckSetup(machine.Make()), "voice_over");
        const Value* python = item.found.Find("python");
        Check(item.state == setup::State::kOk && !item.required &&
                  python != nullptr &&
                  JsonText(*python, "path") == kVoicePython &&
                  JsonText(*python, "origin") == "executable" &&
                  item.tools.empty(),
              "complete: ok, found beside the executable, no tool yet: " +
                  item.detail);
    }
    {
        FakeMachine machine = CompleteMachine();
        machine.files.clear();
        machine.directories.clear();
        const setup::Report report = setup::CheckSetup(machine.Make());
        const setup::Item& item = ItemOf(report, "voice_over");
        Check(item.state == setup::State::kOptionalAbsent &&
                  item.error == "VoiceOverNotFound" &&
                  missingOf(item) ==
                      std::vector<std::string>{"python", "script", "models"} &&
                  item.repair.find("build/voice/env") != std::string::npos &&
                  item.repair.find("qwen-tts 0.1.1") != std::string::npos,
              "nothing installed: optional, absent, the three roles named: " +
                  item.repair);
    }
    {
        FakeMachine machine = CompleteMachine();
        machine.files.erase(kVoiceScript);
        const setup::Item item =
            ItemOf(setup::CheckSetup(machine.Make()), "voice_over");
        Check(item.state == setup::State::kIncomplete &&
                  missingOf(item) == std::vector<std::string>{"script"} &&
                  item.repair.find("Set CUTMACHINE_VOICE_SCRIPT to "
                                   "scripts/voice_over.py,") == 0,
              "the script missing: incomplete, only it named: " + item.repair);
    }
    {
        FakeMachine machine = CompleteMachine();
        machine.files.erase(std::string(kVoiceModels) + "/" +
                            kVoiceDesignModel + "/model.safetensors");
        const setup::Item item =
            ItemOf(setup::CheckSetup(machine.Make()), "voice_over");
        Check(item.state == setup::State::kIncomplete &&
                  missingOf(item) ==
                      std::vector<std::string>{kVoiceDesignModel} &&
                  item.detail.find("lacks Qwen3-TTS-12Hz-1.7B-VoiceDesign") !=
                      std::string::npos &&
                  item.repair.find("Set CUTMACHINE_VOICE_MODELS") == 0,
              "a model short of its weights: incomplete, the model named: " +
                  item.detail);
    }
    {
        FakeMachine machine = CompleteMachine();
        machine.voice_over.python_variable = "/nowhere/python";
        const setup::Item item =
            ItemOf(setup::CheckSetup(machine.Make()), "voice_over");
        Check(item.state == setup::State::kIncomplete &&
                  item.detail.find("CUTMACHINE_VOICE_PYTHON names "
                                   "'/nowhere/python', which is not a file") !=
                      std::string::npos,
              "a variable to nothing is not 'absent': " + item.detail);
    }
}

void TestEmptyMachine() {
    const setup::Report report = setup::CheckSetup(setup::Machine());
    Check(report.items.size() == kItemCount,
          "an empty machine still lists every item");
    Check(!report.Ok() && report.MissingRequired() != nullptr &&
              report.MissingRequired()->id == "ffmpeg",
          "and FFmpeg missing fails it");
    for (const setup::Item& item : report.items) {
        Check(item.state != setup::State::kOk,
              item.id + " is not ok on an empty machine");
    }
}

// --- MFA's search ------------------------------------------------------

struct FakeFiles {
    std::set<std::string> paths;
    std::function<bool(const std::string&)> Predicate() const {
        return
            [this](const std::string& path) { return paths.count(path) != 0; };
    }
};

void TestMfaSearchOrder() {
    FakeFiles files;
    MfaSearch search = CheckoutSearch();
    search.executable_directory = "/app/bin";
    search.working_directory = "/work";
    search.is_file = files.Predicate();
    const std::string besidePython = "/app/bin/mfa/env/bin/python";
    const std::string besideScript = "/app/scripts/mfa_align.py";
    const std::string workingPython = "/work/build/mfa/env/bin/python";
    const std::string configuredPython = "/opt/mfa/bin/python";
    files.paths = {configuredPython, besidePython, kBuildPython, workingPython};

    search.python_variable = configuredPython;
    MfaLocation location = LocateMfa(search);
    Check(location.python.path == configuredPython &&
              location.python.origin == MfaOrigin::kConfigured &&
              location.python.searched ==
                  std::vector<std::string>{configuredPython},
          "1. the variable first, and nothing else looked at");

    search.python_variable.clear();
    location = LocateMfa(search);
    Check(location.python.path == besidePython &&
              location.python.origin == MfaOrigin::kExecutable,
          "2. then beside the executable: " + location.python.path);

    files.paths.erase(besidePython);
    location = LocateMfa(search);
    Check(location.python.path == kBuildPython &&
              location.python.origin == MfaOrigin::kBuild,
          "3. then where the build installs it: " + location.python.path);

    files.paths.erase(kBuildPython);
    location = LocateMfa(search);
    Check(location.python.path == workingPython &&
              location.python.origin == MfaOrigin::kWorkingDirectory &&
              location.python.searched ==
                  std::vector<std::string>{besidePython, kBuildPython,
                                           workingPython},
          "4. then the working directory, after the three others");

    files.paths.erase(workingPython);
    location = LocateMfa(search);
    Check(!location.python.Found() && !location.Found() &&
              location.python.searched.size() == 3 &&
              Contains(location.refusal,
                       "MFA's Python is at none of '" + besidePython + "', '" +
                           kBuildPython + "', '" + workingPython + "'"),
          "found nowhere: the refusal lists the places in order: " +
              location.refusal);
    Check(MfaRefusalName(location.refusal) == "MfaNotFound" &&
              MfaRefusalName("FFmpeg not found: whatever").empty(),
          "the refusal is named MfaNotFound, and nothing else is");

    // The script sits in scripts/, beside build/: one level up.
    files.paths = {besideScript};
    location = LocateMfa(search);
    Check(location.script.path == besideScript &&
              location.script.origin == MfaOrigin::kExecutable,
          "the script beside the executable is <dir>/../scripts: " +
              location.script.path);

    // A relative variable reads against the working directory.
    search.script_variable = "tools/align.py";
    files.paths = {"/work/tools/align.py"};
    location = LocateMfa(search);
    Check(location.script.path == "/work/tools/align.py" &&
              location.script.origin == MfaOrigin::kConfigured,
          "a relative variable is read from the working directory: " +
              location.script.path);
    search.script_variable.clear();

    // A variable that names nothing is a refusal, never a reason to look
    // further, even with the file where the search would find it.
    search.aligner_variable = "/nowhere/mfa";
    files.paths = {"/app/bin/mfa/env/bin/mfa"};
    location = LocateMfa(search);
    Check(!location.aligner.Found() &&
              Contains(location.refusal,
                       "CUTMACHINE_MFA_ALIGNER names '/nowhere/mfa', "
                       "which is not a file") &&
              Contains(location.refusal,
                       "Set CUTMACHINE_MFA_PYTHON to MFA's Python, "
                       "CUTMACHINE_MFA_ALIGNER to MFA's mfa and "
                       "CUTMACHINE_MFA_SCRIPT to "
                       "scripts/mfa_align.py, in the environment or in '" +
                           std::string(kSettingsFile) + "'"),
          "a variable to nothing is a refusal: " + location.refusal);
}

void TestMfaSearchFromTheCheckout() {
    FakeFiles files;
    files.paths = {kBuildPython, kBuildAligner, kSourceScript};

    // The checkout's own binary, build/cutmachine, run from anywhere.
    MfaSearch search;
    search.executable_directory = kBuild;
    search.working_directory = "/tmp";
    search.settings_file = kSettingsFile;
    search.is_file = files.Predicate();
    MfaLocation location = LocateMfa(search);
    Check(location.Found() && location.python.path == kBuildPython &&
              location.aligner.path == kBuildAligner &&
              location.script.path == kSourceScript &&
              location.python.origin == MfaOrigin::kExecutable &&
              location.script.origin == MfaOrigin::kExecutable,
          "build/cutmachine finds build/mfa/env and scripts/ from /tmp: " +
              location.refusal);

    // Its build's own paths are the same places: looked at once.
    search.build_python = kBuildPython;
    files.paths.erase(kBuildPython);
    location = LocateMfa(search);
    Check(location.python.searched ==
              std::vector<std::string>{kBuildPython,
                                       "/tmp/build/mfa/env/bin/python"},
          "a place met twice is looked at once");

    // The application's binary sits three levels down, in the bundle.
    files.paths = {kBuildPython, kBuildAligner, kSourceScript};
    search = MfaSearch();
    search.executable_directory = "/repo/build/CUTMACHINE.app/Contents/MacOS";
    search.working_directory = "/";
    search.is_file = files.Predicate();
    location = LocateMfa(search);
    Check(location.Found() && location.python.path == kBuildPython &&
              location.script.path == kSourceScript &&
              location.python.origin == MfaOrigin::kExecutable &&
              location.python.searched.size() == 2 &&
              location.python.searched[0] ==
                  "/repo/build/CUTMACHINE.app/Contents/MacOS/mfa/env/bin/"
                  "python",
          "the app's binary finds the build/ that holds its bundle: " +
              location.refusal);

    // The bundle copied elsewhere finds what its build installed.
    search.executable_directory = "/Applications/CUTMACHINE.app/Contents/MacOS";
    search.build_python = kBuildPython;
    search.build_aligner = kBuildAligner;
    search.build_script = kSourceScript;
    location = LocateMfa(search);
    Check(location.Found() && location.python.origin == MfaOrigin::kBuild &&
              location.script.origin == MfaOrigin::kBuild,
          "a copy in /Applications still finds its build's MFA: " +
              location.refusal);

    // No executable directory, no build: the working directory, as before.
    files.paths = {"/repo/build/mfa/env/bin/python",
                   "/repo/build/mfa/env/bin/mfa", "/repo/scripts/mfa_align.py"};
    search = MfaSearch();
    search.working_directory = "/repo";
    search.is_file = files.Predicate();
    location = LocateMfa(search);
    Check(location.Found() &&
              location.python.origin == MfaOrigin::kWorkingDirectory,
          "the working directory still serves: " + location.refusal);

    Check(std::string(
              MfaOriginName(MfaOrigin::kConfigured, kMfaPythonVariable)) ==
                  "CUTMACHINE_MFA_PYTHON" &&
              std::string(MfaOriginName(MfaOrigin::kExecutable, "")) ==
                  "executable" &&
              std::string(MfaOriginName(MfaOrigin::kBuild, "")) == "build" &&
              std::string(MfaOriginName(MfaOrigin::kWorkingDirectory, "")) ==
                  "working_directory",
          "the origin names");
}

void TestMissingMfaInstallation() {
    MfaAlignmentSettings settings;
    settings.refusal = "MFA not found: made up";
    Check(MissingMfaInstallation(settings) == settings.refusal,
          "a search's refusal is the answer");

    // Settings filled by hand are checked as they are.
    settings = MfaAlignmentSettings();
    settings.python = "/nowhere/python";
    settings.aligner = "/nowhere/mfa";
    settings.script = "/nowhere/mfa_align.py";
    const std::string missing = MissingMfaInstallation(settings);
    Check(MfaRefusalName(missing) == "MfaNotFound" &&
              Contains(missing, "'/nowhere/python'") &&
              Contains(missing, "CUTMACHINE_MFA_PYTHON") &&
              Contains(missing, "CUTMACHINE_MFA_ALIGNER") &&
              Contains(missing, "CUTMACHINE_MFA_SCRIPT"),
          "hand-filled paths to nothing: the same named refusal: " + missing);
}

// --- The real machine, where it can be tried safely --------------------

class ScopedVariable {
public:
    ScopedVariable(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) {
            had_ = true;
            previous_ = previous;
        }
        if (value != nullptr) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedVariable() {
        if (had_) {
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    std::string name_;
    bool had_ = false;
    std::string previous_;
};

void TestCurrentMachine(const std::filesystem::path& root) {
    const std::filesystem::path settings = root / "settings.env";
    {
        std::ofstream file(settings);
        file << "# made up for the test\n"
                "CUTMACHINE_SETUP_TEST_FILE=from-file\n"
                "export CUTMACHINE_SETUP_TEST_SAME=\"same\"\n"
                "CUTMACHINE_SETUP_TEST_BOTH=file-value\n"
                "CUTMACHINE_SETUP_TEST_EMPTY=hidden\n"
                "CUTMACHINE_SETUP_TEST_TWICE=first\n"
                "CUTMACHINE_SETUP_TEST_TWICE=second\n";
    }
    // The settings file is this one, never the one under $HOME.
    const ScopedVariable file("CUTMACHINE_ENV_FILE", settings.c_str());
    const ScopedVariable environment("CUTMACHINE_SETUP_TEST_ENV", "from-env");
    const ScopedVariable same("CUTMACHINE_SETUP_TEST_SAME", "same");
    const ScopedVariable both("CUTMACHINE_SETUP_TEST_BOTH", "env-value");
    const ScopedVariable empty("CUTMACHINE_SETUP_TEST_EMPTY", "");
    const ScopedVariable fileOnly("CUTMACHINE_SETUP_TEST_FILE", nullptr);

    Check(local_env::FileValue("CUTMACHINE_SETUP_TEST_TWICE") == "first" &&
              local_env::FileValue("CUTMACHINE_SETUP_TEST_SAME") == "same" &&
              local_env::FileValue("CUTMACHINE_SETUP_TEST_ENV").empty(),
          "FileValue reads the file as LoadDotEnv does: the first line wins, "
          "quotes and export are dropped");

    const setup::Machine machine = setup::CurrentMachine();
    Check(machine.settings_file == settings.string(),
          "the machine's settings file is CUTMACHINE_ENV_FILE's");
    const auto read = [&machine](const char* name) {
        return machine.setting(name);
    };
    setup::Setting setting = read("CUTMACHINE_SETUP_TEST_ENV");
    Check(setting.value == "from-env" &&
              setting.origin == setup::SettingOrigin::kEnvironment,
          "exported only: from the environment");
    setting = read("CUTMACHINE_SETUP_TEST_FILE");
    Check(setting.value == "from-file" &&
              setting.origin == setup::SettingOrigin::kSettingsFile,
          "in the file only: from the settings file");
    setting = read("CUTMACHINE_SETUP_TEST_SAME");
    Check(setting.value == "same" &&
              setting.origin == setup::SettingOrigin::kSettingsFile,
          "the file's value, loaded or exported alike: the settings file");
    setting = read("CUTMACHINE_SETUP_TEST_BOTH");
    Check(setting.value == "env-value" &&
              setting.origin == setup::SettingOrigin::kEnvironment,
          "exported over the file: the environment wins, as for the engine");
    setting = read("CUTMACHINE_SETUP_TEST_EMPTY");
    Check(
        setting.value.empty() && setting.origin == setup::SettingOrigin::kUnset,
        "exported empty: the file's value is hidden, as from the engine");
    // Once the file is loaded into the process, the answers stay the same.
    local_env::LoadLocalEnvFileIfPresent();
    setting = read("CUTMACHINE_SETUP_TEST_FILE");
    Check(setting.value == "from-file" &&
              setting.origin == setup::SettingOrigin::kSettingsFile,
          "loaded into the environment, still from the settings file");
    setting = read("CUTMACHINE_SETUP_TEST_EMPTY");
    Check(setting.value.empty(), "loaded, the empty export still hides it");
    ::unsetenv("CUTMACHINE_SETUP_TEST_FILE");
    ::unsetenv("CUTMACHINE_SETUP_TEST_TWICE");

    const std::filesystem::path model = root / "model.bin";
    { std::ofstream(model) << "ggml"; }
    const std::filesystem::path emptyModel = root / "empty.bin";
    { std::ofstream output(emptyModel); }
    const std::filesystem::path locked = root / "locked.bin";
    { std::ofstream(locked) << "ggml"; }
    ::chmod(locked.c_str(), 0);
    setup::FileStatus status = machine.file(model.string());
    Check(status.regular && status.readable && status.size == 4,
          "a file: regular, readable, its size");
    status = machine.file(emptyModel.string());
    Check(status.regular && status.size == 0, "an empty file: size 0");
    status = machine.file(root.string());
    Check(!status.regular, "a directory is not a model file");
    status = machine.file((root / "absent.bin").string());
    Check(!status.regular, "nothing there");
    status = machine.file(locked.string());
    // Root reads anything: the check only holds for a user.
    Check(::geteuid() == 0 || (status.regular && !status.readable),
          "a file without permission is not readable");
    ::chmod(locked.c_str(), 0600);
    Check(machine.mfa.settings_file == settings.string(),
          "MFA's search names the same settings file");
}

}  // namespace

int main() {
    try {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-setup-check-tests");
        std::filesystem::create_directories(root);

        Test("report shape", TestReportShape);
        Test("ffmpeg item", TestFfmpegItem);
        Test("whisper_model item", TestWhisperModelItem);
        Test("whisper_hesitations_model item", TestWhisperHesitationsModelItem);
        Test("mfa item", TestMfaItem);
        Test("agent_key item", TestAgentKeyItem);
        Test("archive_backup item", TestArchiveBackupItem);
        Test("voice_over item", TestVoiceOverItem);
        Test("empty machine", TestEmptyMachine);
        Test("MFA search order", TestMfaSearchOrder);
        Test("MFA from the checkout and the app", TestMfaSearchFromTheCheckout);
        Test("MissingMfaInstallation", TestMissingMfaInstallation);
        Test("current machine", [&root] { TestCurrentMachine(root); });

        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    } catch (const std::exception& failure) {
        // The temporary directory and its files are real: a file system
        // that refuses them throws.
        std::cerr << "FAIL: the setup check tests threw: " << failure.what()
                  << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "setup check tests passed\n";
    return 0;
}
