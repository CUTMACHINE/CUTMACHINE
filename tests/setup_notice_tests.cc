// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. What the window says of
// the setup report (SetupNotice.h): the welcome screen's line, and the
// diagnosis in French, entry by entry, with the window's own two settings
// (its Whisper model, a key in the Keychain) taken into account.
//
// The reports are the engine's own, made by setup::CheckSetup on a machine
// the test makes up, so a change in what the report carries shows here.
// Nothing reads $HOME, preferences, the Keychain or a real FFmpeg.
//
// Plain C++: no AppKit, no FFmpeg, no network.

#include "FfmpegCapabilities.h"
#include "Json.h"
#include "MfaAlignment.h"
#include "SetupCheck.h"
#include "SetupNotice.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using mcp_json::Value;
using setup::State;
using setup_notice::Entry;
using setup_notice::WindowFacts;

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

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

constexpr const char* kSettingsFile = "/machine/.config/cutmachine/.env";
// What the window shows for it.
constexpr const char* kShownSettingsFile = "~/.config/cutmachine/.env";
constexpr const char* kFfmpegPath = "/opt/ffmpeg/bin/ffmpeg";
constexpr const char* kModelPath = "/models/ggml-large-v3.bin";
constexpr const char* kHesitationsPath =
    "/models/ggml-large-v3-hesitations-s075.bin";
constexpr const char* kBackupFolder = "/Shared/OneDrive/Copies";
// Not a key: as plain as the other suites' "sk-test-key".
constexpr const char* kKeyValue = "sk-test-key-notice";
constexpr const char* kWelcomeFfmpeg =
    "FFmpeg introuvable : les vignettes, les formes d'onde et l'export vidéo "
    "ne fonctionneront pas.";

constexpr const char* kBuildPython = "/repo/build/mfa/env/bin/python";
constexpr const char* kBuildAligner = "/repo/build/mfa/env/bin/mfa";
constexpr const char* kSourceScript = "/repo/scripts/mfa_align.py";

// A machine made of maps, as setup_check_tests makes one.
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

    setup::Report Report() const {
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
        return setup::CheckSetup(machine);
    }
};

ffmpeg::Diagnosis FfmpegNotFound() {
    ffmpeg::Diagnosis diagnosis;
    diagnosis.location.refusal =
        "FFmpeg not found: CUTMACHINE_FFMPEG is not set, there is no "
        "'ffmpeg' beside the executable, and none in the directories of PATH "
        "('/usr/bin:/bin:/usr/sbin:/sbin'). Set CUTMACHINE_FFMPEG to the "
        "path of an FFmpeg binary, in the environment or in "
        "'/machine/.config/cutmachine/.env'";
    return diagnosis;
}

ffmpeg::Diagnosis FfmpegFound(
    const std::set<std::string>& lacking = {},
    ffmpeg::Origin origin = ffmpeg::Origin::kConfigured) {
    ffmpeg::Diagnosis diagnosis;
    diagnosis.location.path = kFfmpegPath;
    diagnosis.location.origin = origin;
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

MfaSearch CheckoutSearch() {
    MfaSearch search;
    search.executable_directory = "/repo/build";
    search.build_python = kBuildPython;
    search.build_aligner = kBuildAligner;
    search.build_script = kSourceScript;
    search.working_directory = "/somewhere/else";
    search.settings_file = kSettingsFile;
    return search;
}

// GN6a -- the voice-over model installed in the made-up checkout, but for
// the files in `skip`.
constexpr const char* kVoiceModels = "/repo/build/voice/models";

void AddCheckoutVoice(FakeMachine& machine,
                      const std::set<std::string>& skip = {}) {
    machine.voice_over.executable_directory = "/repo/build";
    machine.voice_over.working_directory = "/somewhere/else";
    machine.voice_over.settings_file = kSettingsFile;
    machine.AddFile("/repo/build/voice/env/bin/python");
    machine.AddFile("/repo/scripts/voice_over.py");
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

// Everything there: FFmpeg, the model by the variable, MFA, the key by the
// variable, the voice-over model, and whatever else the report reads.
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
    machine.AddFile(kBuildPython);
    machine.AddFile(kBuildAligner);
    machine.AddFile(kSourceScript);
    machine.Set("ANTHROPIC_API_KEY", kKeyValue,
                setup::SettingOrigin::kEnvironment);
    machine.Set("CUTMACHINE_ARCHIVE_BACKUP_DIR", kBackupFolder,
                setup::SettingOrigin::kSettingsFile);
    machine.directories.insert(kBackupFolder);
    AddCheckoutVoice(machine);
    return machine;
}

// FFmpeg only: what a new Mac with FFmpeg installed and nothing else has.
FakeMachine FfmpegOnlyMachine() {
    FakeMachine machine;
    machine.ffmpeg = FfmpegFound();
    machine.mfa = CheckoutSearch();
    return machine;
}

WindowFacts Facts() {
    WindowFacts facts;
    facts.settings_file = kShownSettingsFile;
    return facts;
}

Entry EntryOf(const setup::Report& report, const WindowFacts& facts,
              const std::string& id) {
    for (const Entry& entry : setup_notice::Entries(report, facts)) {
        if (entry.id == id) {
            return entry;
        }
    }
    Check(false, "the diagnosis has an entry " + id);
    return Entry();
}

std::string Lines(const Entry& entry) {
    std::string text;
    for (const std::string& line : entry.found) {
        text += line + "\n";
    }
    return text;
}

void TestLabelsAndTitles() {
    Check(std::string(setup_notice::StateLabel(State::kOk)) == "prêt" &&
              std::string(setup_notice::StateLabel(State::kMissing)) ==
                  "absent" &&
              std::string(setup_notice::StateLabel(State::kIncomplete)) ==
                  "incomplet" &&
              std::string(setup_notice::StateLabel(State::kOptionalAbsent)) ==
                  "facultatif, absent",
          "the four states, in plain words");
    Check(
        setup_notice::ItemTitle(ffmpeg::kDiagnosisKey) == "FFmpeg" &&
            setup_notice::ItemTitle(setup::kWhisperModelItem) ==
                "Modèle Whisper" &&
            setup_notice::ItemTitle(setup::kWhisperHesitationsModelItem) ==
                "Poids des hésitations" &&
            setup_notice::ItemTitle(setup::kMfaItem) == "MFA" &&
            setup_notice::ItemTitle(setup::kAgentKeyItem) == "Clé de l'agent" &&
            setup_notice::ItemTitle(setup::kArchiveBackupItem) ==
                "Copie de l'archive" &&
            setup_notice::ItemTitle(setup::kVoiceOverItem) == "Voix off",
        "every item's name");
    Check(setup_notice::ItemTitle("gpu") == "gpu",
          "an item this version does not know keeps its identifier");
}

void TestWelcomeLine() {
    FakeMachine machine = FfmpegOnlyMachine();
    Check(setup_notice::WelcomeLine(machine.Report()).empty(),
          "FFmpeg there, nothing else: no line -- only the required counts");
    Check(StartsWith(setup_notice::Summary(machine.Report()),
                     "Rien de requis ne manque"),
          "and the summary says nothing required is missing");

    machine.ffmpeg = FfmpegNotFound();
    Check(setup_notice::WelcomeLine(machine.Report()) == kWelcomeFfmpeg,
          "no FFmpeg: the line names it and what stops: " +
              setup_notice::WelcomeLine(machine.Report()));
    Check(setup_notice::Summary(machine.Report()) == kWelcomeFfmpeg,
          "the summary is that line");

    // An FFmpeg without libx265 edits and exports.
    machine.ffmpeg = FfmpegFound({"libx265"});
    Check(setup_notice::WelcomeLine(machine.Report()).empty(),
          "an incomplete FFmpeg puts no line on the welcome screen");
    machine.ffmpeg = FfmpegFound();
    machine.ffmpeg.probe = ffmpeg::Probe();
    machine.ffmpeg.probe.path = kFfmpegPath;
    Check(setup_notice::WelcomeLine(machine.Report()).empty(),
          "neither does one that cannot be probed");

    Check(setup_notice::WelcomeLine(setup::CheckSetup(setup::Machine())) ==
              kWelcomeFfmpeg,
          "a machine that says nothing: the same line");

    // A required item this version cannot word still gets a line.
    setup::Report report;
    setup::Item gpu;
    gpu.id = "gpu";
    gpu.required = true;
    gpu.state = State::kMissing;
    report.items.push_back(gpu);
    Check(setup_notice::WelcomeLine(report) ==
              "gpu introuvable : CUTMACHINE ne peut ni monter ni exporter.",
          "an unknown required item: " + setup_notice::WelcomeLine(report));
}

void TestFfmpegEntry() {
    FakeMachine machine = CompleteMachine();
    const WindowFacts facts = Facts();

    machine.ffmpeg = FfmpegNotFound();
    Entry entry = EntryOf(machine.Report(), facts, "ffmpeg");
    Check(entry.state == State::kMissing && entry.required &&
              entry.title == "FFmpeg" &&
              Lines(entry) == "FFmpeg introuvable.\n" &&
              StartsWith(entry.prevents, "Sans lui, ") &&
              Contains(entry.prevents, "vignettes") &&
              Contains(entry.prevents, "formes d'onde") &&
              Contains(entry.prevents, "export vidéo"),
          "absent: what stops without it: " + entry.prevents);
    Check(StartsWith(entry.repair, "installer FFmpeg") &&
              Contains(entry.repair, "CUTMACHINE_FFMPEG") &&
              Contains(entry.repair, kShownSettingsFile) &&
              Contains(entry.repair, "Finder") &&
              Contains(entry.repair, "PATH"),
          "and the repair names the setting, the file and the Finder's "
          "PATH: " +
              entry.repair);

    machine.ffmpeg = FfmpegFound();
    entry = EntryOf(machine.Report(), facts, "ffmpeg");
    Check(entry.state == State::kOk &&
              Lines(entry) ==
                  "FFmpeg 8.1 : /opt/ffmpeg/bin/ffmpeg, réglé par "
                  "CUTMACHINE_FFMPEG.\n" &&
              entry.prevents.empty() && entry.repair.empty(),
          "ready: version, path and where it came from: " + Lines(entry));
    machine.ffmpeg = FfmpegFound({}, ffmpeg::Origin::kSearchPath);
    Check(Contains(Lines(EntryOf(machine.Report(), facts, "ffmpeg")),
                   ", trouvé dans le PATH."),
          "found on PATH");
    machine.ffmpeg = FfmpegFound({}, ffmpeg::Origin::kBundled);
    Check(Contains(Lines(EntryOf(machine.Report(), facts, "ffmpeg")),
                   ", à côté de l'application."),
          "found beside the application");

    machine.ffmpeg = FfmpegFound({"libx264", "libx265"});
    entry = EntryOf(machine.Report(), facts, "ffmpeg");
    Check(entry.state == State::kIncomplete &&
              Contains(Lines(entry),
                       "Il lui manque libx264 (encodeur) et libx265 "
                       "(encodeur).") &&
              StartsWith(entry.prevents, "Sans libx264, un export H.264") &&
              Contains(entry.prevents, " ; sans libx265, un export HEVC") &&
              Contains(entry.prevents, "Le reste fonctionne."),
          "incomplete: what it lacks and what each stops: " + Lines(entry) +
              entry.prevents);
    Check(Contains(entry.repair,
                   "compilé avec --enable-libx264 et --enable-libx265") &&
              Contains(entry.repair, "CUTMACHINE_FFMPEG") &&
              !Contains(entry.repair, " and "),
          "the configure options, from the engine's table, joined in "
          "French: " +
              entry.repair);

    machine.ffmpeg = FfmpegFound();
    machine.ffmpeg.probe = ffmpeg::Probe();
    machine.ffmpeg.probe.path = kFfmpegPath;
    machine.ffmpeg.probe.failure = "'/opt/ffmpeg/bin/ffmpeg -filters' failed";
    entry = EntryOf(machine.Report(), facts, "ffmpeg");
    Check(entry.state == State::kIncomplete &&
              Contains(Lines(entry), "FFmpeg : /opt/ffmpeg/bin/ffmpeg") &&
              Contains(Lines(entry), "Il ne répond pas") &&
              Contains(entry.repair,
                       "/opt/ffmpeg/bin/ffmpeg -version » "
                       "fonctionne"),
          "a binary that does not answer: " + Lines(entry) + entry.repair);
}

// DEC23 -- one setting, the engine's, which the window's menus read too and
// « CUTMACHINE ▸ Modèle Whisper… » writes: the entry no longer has a window
// model to count, nor two readers to tell apart.
void TestWhisperEntry() {
    FakeMachine machine = CompleteMachine();
    const WindowFacts facts = Facts();
    const auto whisper = [&] {
        return EntryOf(machine.Report(), facts, "whisper_model");
    };

    // Unset.
    machine.settings.erase("CUTMACHINE_WHISPER_MODEL");
    Entry entry = whisper();
    Check(entry.state == State::kMissing && entry.found.size() == 1 &&
              entry.found[0] ==
                  "CUTMACHINE_WHISPER_MODEL ne désigne aucun modèle : "
                  "transcribe_media, transcribe_timeline et inspect_cut "
                  "refusent.",
          "unset: absent, said once: " + Lines(entry));
    Check(StartsWith(entry.prevents, "Sans modèle, rien ne se transcrit") &&
              Contains(entry.prevents,
                       "les menus de transcription de la "
                       "fenêtre demandent un fichier") &&
              entry.repair ==
                  "choisir un modèle ggml de whisper.cpp (ggml-large-v3.bin, "
                  "environ 3 Go) par CUTMACHINE ▸ Modèle Whisper…, qui "
                  "l'écrit dans CUTMACHINE_WHISPER_MODEL, ou l'y indiquer "
                  "soi-même dans l'environnement ou dans "
                  "~/.config/cutmachine/.env.",
          "the repair: the menu writes the variable: " + entry.repair);
    Check(!Contains(Lines(entry), "Menus de la fenêtre") &&
              !Contains(Lines(entry), "Agents et ligne de commande"),
          "no second reader to tell apart: " + Lines(entry));

    // Set, from the settings file, then from the environment.
    machine.Set("CUTMACHINE_WHISPER_MODEL", kModelPath,
                setup::SettingOrigin::kSettingsFile);
    entry = whisper();
    Check(entry.state == State::kOk && entry.found.size() == 1 &&
              entry.found[0] ==
                  "/models/ggml-large-v3.bin, par CUTMACHINE_WHISPER_MODEL "
                  "dans ~/.config/cutmachine/.env : il sert aux menus de la "
                  "fenêtre, aux agents et à la ligne de commande." &&
              entry.prevents.empty() && entry.repair.empty(),
          "ready, where it was read, and that it serves everyone: " +
              Lines(entry));
    machine.Set("CUTMACHINE_WHISPER_MODEL", kModelPath,
                setup::SettingOrigin::kEnvironment);
    Check(Contains(whisper().found[0],
                   "par CUTMACHINE_WHISPER_MODEL dans l'environnement :"),
          "or from the environment");

    // The model there, but empty or unreadable.
    machine.AddFile(kModelPath, 0);
    entry = whisper();
    Check(entry.state == State::kIncomplete &&
              entry.found[0] ==
                  "/models/ggml-large-v3.bin, par CUTMACHINE_WHISPER_MODEL, "
                  "est vide." &&
              !entry.repair.empty(),
          "the model empty: incomplete: " + Lines(entry));
    machine.AddFile(kModelPath, 3095033483ULL, /*readable=*/false);
    entry = whisper();
    Check(entry.state == State::kIncomplete &&
              Contains(entry.found[0], "ne peut pas être lu."),
          "unreadable: incomplete: " + Lines(entry));
}

void TestAgentKeyEntry() {
    FakeMachine machine = CompleteMachine();
    WindowFacts facts = Facts();
    const auto key = [&] {
        return EntryOf(machine.Report(), facts, "agent_key");
    };

    machine.settings.erase("ANTHROPIC_API_KEY");
    Entry entry = key();
    Check(entry.state == State::kOptionalAbsent && !entry.required &&
              Contains(Lines(entry), "Aucune clé") &&
              StartsWith(entry.prevents, "Sans clé, le panneau Agent") &&
              Contains(entry.repair, "CUTMACHINE ▸ Moteur IA…") &&
              Contains(entry.repair, "Trousseau") &&
              Contains(entry.repair, "ANTHROPIC_API_KEY") &&
              Contains(entry.repair, kShownSettingsFile),
          "no key anywhere: optional, and both ways to give one: " +
              entry.repair);

    facts.agent_key_saved = true;
    entry = key();
    Check(entry.state == State::kOk &&
              Lines(entry) ==
                  "Une clé est enregistrée dans le Trousseau (CUTMACHINE ▸ "
                  "Moteur IA…).\n" &&
              entry.repair.empty(),
          "a key in the Keychain alone makes it ready: " + Lines(entry));

    facts.agent_key_saved = false;
    machine.Set("ANTHROPIC_API_KEY", kKeyValue,
                setup::SettingOrigin::kEnvironment);
    entry = key();
    Check(entry.state == State::kOk &&
              Lines(entry) ==
                  "ANTHROPIC_API_KEY est réglée dans l'environnement ; sa "
                  "valeur n'est jamais affichée.\n",
          "the variable alone: " + Lines(entry));

    facts.agent_key_saved = true;
    machine.Set("ANTHROPIC_API_KEY", kKeyValue,
                setup::SettingOrigin::kSettingsFile);
    entry = key();
    Check(entry.state == State::kOk && entry.found.size() == 2 &&
              Contains(entry.found[1], "dans ~/.config/cutmachine/.env"),
          "both, the file named: " + Lines(entry));
    const setup::Report report = machine.Report();
    Check(
        !Contains(setup_notice::DiagnosticText(report, facts), "sk-test") &&
            !Contains(setup_notice::ClipboardText(report, facts), "sk-test") &&
            !Contains(setup_notice::ClipboardText(report, facts), "key-notice"),
        "the key's value is in neither the text nor what is copied");
}

void TestMfaEntry() {
    FakeMachine machine = CompleteMachine();
    const WindowFacts facts = Facts();

    Entry entry = EntryOf(machine.Report(), facts, "mfa");
    Check(entry.state == State::kOk && Contains(Lines(entry), "trouvés") &&
              entry.repair.empty(),
          "ready: " + Lines(entry));

    machine.files.erase(kSourceScript);
    entry = EntryOf(machine.Report(), facts, "mfa");
    Check(entry.state == State::kIncomplete &&
              Lines(entry) == "Introuvable : scripts/mfa_align.py.\n" &&
              StartsWith(entry.prevents, "Sans MFA, ") &&
              Contains(entry.prevents, "align_words refuse,") &&
              StartsWith(entry.repair,
                         "régler CUTMACHINE_MFA_SCRIPT, dans "
                         "l'environnement ou dans "
                         "~/.config/cutmachine/.env.") &&
              !Contains(entry.repair, "CUTMACHINE_MFA_PYTHON"),
          "its script missing: only its variable: " + Lines(entry) +
              entry.repair);

    machine.files.clear();
    entry = EntryOf(machine.Report(), facts, "mfa");
    Check(entry.state == State::kMissing &&
              Lines(entry) ==
                  "Introuvable : le Python de MFA, l'exécutable mfa et "
                  "scripts/mfa_align.py.\n" &&
              Contains(entry.repair,
                       "CUTMACHINE_MFA_PYTHON, CUTMACHINE_MFA_ALIGNER et "
                       "CUTMACHINE_MFA_SCRIPT") &&
              Contains(entry.repair, "Rien n'est téléchargé"),
          "nothing of it: absent, the three variables: " + entry.repair);
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md.
void TestArchiveBackupEntry() {
    FakeMachine machine = CompleteMachine();
    const WindowFacts facts = Facts();
    Entry entry = EntryOf(machine.Report(), facts, "archive_backup");
    Check(entry.state == State::kOk && Contains(Lines(entry), kBackupFolder) &&
              Contains(Lines(entry), "une fois par jour") &&
              entry.repair.empty(),
          "a folder: ready, named: " + Lines(entry));
    machine.directories.clear();
    entry = EntryOf(machine.Report(), facts, "archive_backup");
    Check(entry.state == State::kIncomplete &&
              Contains(Lines(entry), "n'existe pas") &&
              Contains(entry.repair, "CUTMACHINE_ARCHIVE_BACKUP_DIR") &&
              Contains(entry.repair, kShownSettingsFile),
          "a missing folder: incomplete: " + Lines(entry));
    machine.Set("CUTMACHINE_ARCHIVE_BACKUP_DIR", "off",
                setup::SettingOrigin::kSettingsFile);
    entry = EntryOf(machine.Report(), facts, "archive_backup");
    Check(entry.state == State::kOptionalAbsent &&
              Contains(Lines(entry), "copiée nulle part") &&
              StartsWith(entry.prevents, "Sans copie, "),
          "off: optional: " + Lines(entry));
}

void TestText() {
    FakeMachine machine = FfmpegOnlyMachine();
    machine.ffmpeg = FfmpegNotFound();
    WindowFacts facts = Facts();
    facts.build = "0.1.0, commit abc1234";
    const setup::Report report = machine.Report();
    const std::vector<setup_notice::Run> runs =
        setup_notice::DiagnosticRuns(report, facts);
    const std::string text = setup_notice::DiagnosticText(report, facts);

    std::string joined;
    std::vector<std::string> headings;
    for (const setup_notice::Run& run : runs) {
        joined += run.text;
        if (run.heading) {
            headings.push_back(run.text);
        }
    }
    Check(joined == text, "the text is the runs end to end");
    const std::string optional = "facultatif, absent\n";
    const std::vector<std::string> expected = {
        "FFmpeg : absent\n",
        "Modèle Whisper : absent\n",
        "Poids des hésitations : " + optional,
        "MFA : absent\n",
        "Clé de l'agent : " + optional,
        "Copie de l'archive : " + optional,
        "Voix off : " + optional};
    Check(headings == expected,
          "one heading per item, in the report's order, its state in "
          "plain words: " +
              text);
    Check(StartsWith(text, std::string("CUTMACHINE 0.1.0, commit abc1234\n") +
                               kWelcomeFfmpeg + "\n\nFFmpeg : absent\n"),
          "the build first, then the summary: " + text.substr(0, 200));
    Check(Contains(text, "\nÀ faire : installer FFmpeg") &&
              Contains(text, "\nSans lui, les rushes"),
          "an entry that is not ready says what stops and what to do");
    Check(Contains(text,
                   "Après une modification de "
                   "~/.config/cutmachine/.env, relancer CUTMACHINE"),
          "and the text ends on the settings file being read once");

    // The engine's English stays in the engine's report.
    for (const setup::Item& item : report.items) {
        Check(item.detail.empty() || !Contains(text, item.detail),
              item.id +
                  ": the engine's sentence is not in the window's "
                  "text: " +
                  item.detail);
        Check(item.repair.empty() || !Contains(text, item.repair),
              item.id + ": nor its repair");
        Check(!Contains(text, item.prevents), item.id +
                                                  ": nor what it says "
                                                  "it prevents");
    }
    Check(!Contains(text, "not found") && !Contains(text, "Without"),
          "no English in the window's text");

    const std::string copied = setup_notice::ClipboardText(report, facts);
    const std::string marker =
        "\nRapport du moteur (cutmachine --check-setup) :\n";
    Check(StartsWith(copied, text) && Contains(copied, marker),
          "what is copied is the text, then the engine's report");
    const size_t json = copied.find(marker);
    Value parsed;
    std::string error;
    const bool read =
        json != std::string::npos &&
        Value::Parse(copied.substr(json + marker.size()), parsed, error);
    const Value* items = read ? parsed.Find("items") : nullptr;
    Check(read && items != nullptr && items->IsArray() &&
              items->AsArray().size() == report.items.size() &&
              parsed.Find("ok") != nullptr && !parsed.Find("ok")->AsBool(),
          "and that report is the JSON of --check-setup: " + error);

    // Everything ready: no closing word about the settings file.
    FakeMachine complete = CompleteMachine();
    const std::string ready =
        setup_notice::DiagnosticText(complete.Report(), Facts());
    Check(StartsWith(ready, "Rien de requis ne manque") &&
              !Contains(ready, "À faire") && !Contains(ready, "relancer") &&
              Contains(ready, "Clé de l'agent : prêt\n"),
          "everything ready: no repair, no word on the file: " + ready);
    // AL4 -- the hesitations weights say whether their digest is verified.
    Check(Contains(ready, "Poids des hésitations : prêt\n") &&
              Contains(ready, "dont l'empreinte est vérifiée"),
          "the hesitations weights, verified: " + ready);
    complete.verified_weights.clear();
    Check(Contains(setup_notice::DiagnosticText(complete.Report(), Facts()),
                   "son empreinte n'a pas encore été vérifiée"),
          "and not yet verified, said so");
}

// GN6a -- the local voice-over model, worded from the report's roles and
// model folders.
void TestVoiceOver() {
    Entry entry = EntryOf(CompleteMachine().Report(), Facts(), "voice_over");
    Check(entry.title == "Voix off" && entry.state == State::kOk &&
              entry.found.size() == 1 &&
              StartsWith(entry.found[0], "Le Python de la voix off") &&
              entry.repair.empty(),
          "installed: ready, nothing to do: " + Lines(entry));

    entry = EntryOf(FfmpegOnlyMachine().Report(), Facts(), "voice_over");
    Check(entry.state == State::kOptionalAbsent &&
              entry.found ==
                  std::vector<std::string>{"La voix off locale n'est pas "
                                           "installée sur cette machine."} &&
              StartsWith(entry.prevents, "Sans elle, aucune voix off") &&
              Contains(entry.repair, "build/voice/env") &&
              Contains(entry.repair, "CUTMACHINE_VOICE_MODELS") &&
              Contains(entry.repair, kShownSettingsFile),
          "not installed: optional, and how to install it: " + Lines(entry));

    FakeMachine machine = CompleteMachine();
    machine.files.erase(std::string(kVoiceModels) + "/" + kVoiceCloneModel +
                        "/config.json");
    entry = EntryOf(machine.Report(), Facts(), "voice_over");
    Check(entry.state == State::kIncomplete &&
              entry.found ==
                  std::vector<std::string>{"Modèle incomplet ou absent : "
                                           "Qwen3-TTS-12Hz-1.7B-Base."} &&
              Contains(entry.repair, "régler CUTMACHINE_VOICE_MODELS, "),
          "a model short of a file: the model named, its variable: " +
              Lines(entry));
}

void TestUnknownItem() {
    setup::Report report = FfmpegOnlyMachine().Report();
    setup::Item gpu;
    gpu.id = "gpu";
    gpu.state = State::kMissing;
    gpu.detail = "no GPU at all";
    gpu.repair = "Install a GPU";
    report.items.push_back(gpu);
    const Entry entry = EntryOf(report, Facts(), "gpu");
    const std::string text = setup_notice::DiagnosticText(report, Facts());
    Check(entry.title == "gpu" && entry.found.empty() &&
              entry.prevents.empty() && entry.repair.empty() &&
              Contains(text, "\ngpu : absent\n") &&
              !Contains(text, "Install a GPU"),
          "an unknown item: its identifier and its state, nothing "
          "translated: " +
              text);
}

}  // namespace

int main() {
    Test("labels and titles", TestLabelsAndTitles);
    Test("welcome line", TestWelcomeLine);
    Test("ffmpeg entry", TestFfmpegEntry);
    Test("whisper entry", TestWhisperEntry);
    Test("agent key entry", TestAgentKeyEntry);
    Test("mfa entry", TestMfaEntry);
    Test("archive backup entry", TestArchiveBackupEntry);
    Test("text", TestText);
    Test("voice-over", TestVoiceOver);
    Test("unknown item", TestUnknownItem);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "setup notice tests passed\n";
    return 0;
}
