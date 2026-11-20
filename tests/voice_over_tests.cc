// GN6a -- docs/roadmap/GENERATIF-2026-10.md. The engine's launcher of the
// local voice-over model.
//
// Where the model's Python, script and models folder are looked for (the
// order of ToolFiles.h, shared with MFA), the refusal that names what is
// missing, the languages, what the engine hands the script, and a run on
// /bin/sh standing in for Python: the report read back, a stale report not
// taken for a new one, a failure's error output kept, cancellation.
//
// Plain C++ and POSIX: no AppKit, no FFmpeg, no model, no network.

#include "MediaTaskManager.h"
#include "VoiceOver.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"

namespace {

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
}

void WriteScript(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/sh\n" << body << '\n';
    chmod(path.c_str(), 0755);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

constexpr const char* kSettingsFile = "/machine/.config/cutmachine/.env";

// A file system made of two sets.
struct FakeFiles {
    std::set<std::string> files;
    std::set<std::string> folders;

    void AddModels(const std::string& models,
                   const std::vector<std::string>& skip = {}) {
        folders.insert(models);
        for (const char* folder : {kVoiceDesignModel, kVoiceCloneModel}) {
            for (const std::string& file : VoiceModelFiles()) {
                const std::string path = models + "/" + folder + "/" + file;
                bool skipped = false;
                for (const std::string& entry : skip) {
                    skipped = skipped || entry == path;
                }
                if (!skipped) {
                    files.insert(path);
                }
            }
        }
    }

    void Apply(VoiceOverSearch& search) const {
        search.is_file = [this](const std::string& path) {
            return files.count(path) != 0;
        };
        search.is_folder = [this](const std::string& path) {
            return folders.count(path) != 0;
        };
    }
};

VoiceOverSearch AppSearch() {
    VoiceOverSearch search;
    search.executable_directory = "/app/bin";
    search.working_directory = "/work";
    search.build_python = "/repo/build/voice/env/bin/python";
    search.build_script = "/repo/scripts/voice_over.py";
    search.build_models = "/repo/build/voice/models";
    search.settings_file = kSettingsFile;
    return search;
}

void TestSearchOrder() {
    Test("the order: variable, executable, build, working directory", [] {
        FakeFiles fake;
        VoiceOverSearch search = AppSearch();
        fake.Apply(search);
        const std::string configured = "/opt/voice/bin/python";
        const std::string beside = "/app/bin/voice/env/bin/python";
        const std::string working = "/work/build/voice/env/bin/python";
        fake.files = {configured, beside, search.build_python, working};

        search.python_variable = configured;
        VoiceOverLocation location = LocateVoiceOver(search);
        Check(location.python.path == configured &&
                  location.python.origin == tool_files::Origin::kConfigured &&
                  location.python.searched.size() == 1,
              "1. the variable, and nothing else looked at");

        search.python_variable.clear();
        location = LocateVoiceOver(search);
        Check(location.python.path == beside &&
                  location.python.origin == tool_files::Origin::kExecutable,
              "2. beside the executable: " + location.python.path);

        fake.files.erase(beside);
        location = LocateVoiceOver(search);
        Check(location.python.path == search.build_python &&
                  location.python.origin == tool_files::Origin::kBuild,
              "3. where the build installs it: " + location.python.path);

        fake.files.erase(search.build_python);
        location = LocateVoiceOver(search);
        Check(
            location.python.path == working &&
                location.python.origin == tool_files::Origin::kWorkingDirectory,
            "4. the working directory: " + location.python.path);

        fake.files.erase(working);
        location = LocateVoiceOver(search);
        Check(!location.Found() &&
                  location.refusal.rfind("voice-over model not found: ", 0) ==
                      0 &&
                  Contains(location.refusal,
                           "the voice-over's Python is at none of '" + beside +
                               "', '/repo/build/voice/env/bin/python', '" +
                               working + "'") &&
                  Contains(location.refusal,
                           "Set CUTMACHINE_VOICE_PYTHON to the voice-over's "
                           "Python, CUTMACHINE_VOICE_SCRIPT to "
                           "scripts/voice_over.py and CUTMACHINE_VOICE_MODELS "
                           "to the folder of the voice-over models, in the "
                           "environment or in '" +
                               std::string(kSettingsFile) + "'"),
              "found nowhere: every place, then the variables: " +
                  location.refusal);
        Check(VoiceOverRefusalName(location.refusal) == "VoiceOverNotFound" &&
                  VoiceOverRefusalName("MFA not found: x").empty(),
              "the refusal is named VoiceOverNotFound, and nothing else is");
    });

    Test("the script sits in scripts/, beside build/", [] {
        FakeFiles fake;
        VoiceOverSearch search = AppSearch();
        fake.Apply(search);
        fake.files = {"/app/scripts/voice_over.py"};
        const VoiceOverLocation location = LocateVoiceOver(search);
        Check(location.script.path == "/app/scripts/voice_over.py" &&
                  location.script.origin == tool_files::Origin::kExecutable,
              "<dir>/../scripts/voice_over.py: " + location.script.path);
    });

    Test("the models are a folder, and a variable to nothing is a refusal", [] {
        FakeFiles fake;
        VoiceOverSearch search = AppSearch();
        fake.Apply(search);
        // A regular file where the folder should be does not count.
        fake.files = {"/app/bin/voice/models"};
        VoiceOverLocation location = LocateVoiceOver(search);
        Check(!location.models.Found() && location.models.searched.size() == 3,
              "a file named models is not the models folder");

        search.models_variable = "/nowhere/models";
        fake.folders = {"/app/bin/voice/models"};
        location = LocateVoiceOver(search);
        Check(!location.models.Found() &&
                  Contains(location.refusal,
                           "CUTMACHINE_VOICE_MODELS names "
                           "'/nowhere/models', which is not a folder"),
              "a variable naming no folder is refused, even with one "
              "where the search would look: " +
                  location.refusal);
    });

    Test("a models folder short of a model is refused by its name", [] {
        FakeFiles fake;
        VoiceOverSearch search = AppSearch();
        fake.Apply(search);
        const std::string models = "/app/bin/voice/models";
        fake.files = {"/app/bin/voice/env/bin/python",
                      "/app/scripts/voice_over.py"};
        fake.AddModels(models, {models + "/" + kVoiceCloneModel +
                                "/speech_tokenizer/model.safetensors"});
        VoiceOverLocation location = LocateVoiceOver(search);
        Check(location.models.Found() &&
                  location.missing_models ==
                      std::vector<std::string>{kVoiceCloneModel} &&
                  Contains(location.refusal,
                           "the folder of the voice-over models '" + models +
                               "' lacks Qwen3-TTS-12Hz-1.7B-Base (each needs "
                               "config.json, model.safetensors and "
                               "speech_tokenizer/model.safetensors)") &&
                  Contains(location.refusal,
                           "Set CUTMACHINE_VOICE_MODELS to the folder of the "
                           "voice-over models"),
              "the missing model is named, and only the models variable: " +
                  location.refusal);

        fake.AddModels(models);
        location = LocateVoiceOver(search);
        const VoiceOverSettings settings = location.Settings();
        Check(location.Found() && location.missing_models.empty() &&
                  settings.python == "/app/bin/voice/env/bin/python" &&
                  settings.script == "/app/scripts/voice_over.py" &&
                  settings.models == models && settings.refusal.empty(),
              "complete: found, no refusal: " + location.refusal);
    });

    Test("the app's binary finds the build/ that holds its bundle", [] {
        FakeFiles fake;
        VoiceOverSearch search;
        search.executable_directory =
            "/repo/build/CUTMACHINE.app/Contents/MacOS";
        search.working_directory = "/";
        fake.Apply(search);
        fake.files = {"/repo/build/voice/env/bin/python",
                      "/repo/scripts/voice_over.py"};
        fake.AddModels("/repo/build/voice/models");
        const VoiceOverLocation location = LocateVoiceOver(search);
        Check(location.Found() &&
                  location.python.path == "/repo/build/voice/env/bin/python" &&
                  location.models.path == "/repo/build/voice/models",
              "build/CUTMACHINE.app finds build/voice: " + location.refusal);
    });
}

void TestLanguages() {
    Test("the ten languages, by ISO code", [] {
        Check(VoiceOverLanguageName("fr") == "French" &&
                  VoiceOverLanguageName("en") == "English" &&
                  VoiceOverLanguageName("es") == "Spanish",
              "fr, en and es are French, English and Spanish");
        Check(VoiceOverLanguageName("FR").empty() &&
                  VoiceOverLanguageName("French").empty() &&
                  VoiceOverLanguageName("nl").empty(),
              "only lower-case ISO codes the models say");
        const std::vector<std::string> codes = VoiceOverLanguageCodes();
        Check(
            codes.size() == 10 && codes.front() == "zh" && codes.back() == "ru",
            "ten codes, in Qwen3-TTS's order");
    });
}

VoiceOverSettings ScriptSettings() {
    VoiceOverSettings settings;
    settings.python = "/env/bin/python";
    settings.script = "/repo/scripts/voice_over.py";
    settings.models = "/repo/build/voice/models";
    return settings;
}

void TestArguments() {
    Test("what crosses to Python, for a design and for a clone", [] {
        VoiceOverRequest request;
        request.mode = VoiceOverMode::kDesign;
        request.text = "Le 21 mars, à 10 h.";
        request.language = "fr";
        request.description = "Une voix chaude";
        request.seed = 42;
        request.output_path = "/p/Sound/voice.wav";
        request.report_path = "/p/Sound/voice.json";
        std::vector<std::string> arguments =
            VoiceOverArguments(ScriptSettings(), request);
        Check(
            arguments ==
                std::vector<std::string>{
                    "/env/bin/python", "/repo/scripts/voice_over.py", "design",
                    "--model",
                    "/repo/build/voice/models/"
                    "Qwen3-TTS-12Hz-1.7B-VoiceDesign",
                    "--text", "Le 21 mars, à 10 h.", "--language", "French",
                    "--seed", "42", "--out", "/p/Sound/voice.wav", "--report",
                    "/p/Sound/voice.json", "--description", "Une voix chaude"},
            "design: the VoiceDesign folder, the language's name, the "
            "description");

        request.mode = VoiceOverMode::kClone;
        request.reference_audio = "/p/Sound/reference.wav";
        request.reference_text = "Prenez le temps.";
        arguments = VoiceOverArguments(ScriptSettings(), request);
        Check(arguments.size() == 19 && arguments[2] == "clone" &&
                  arguments[4] ==
                      "/repo/build/voice/models/Qwen3-TTS-12Hz-1.7B-Base" &&
                  arguments[15] == "--reference" &&
                  arguments[16] == "/p/Sound/reference.wav" &&
                  arguments[17] == "--reference-text" &&
                  arguments[18] == "Prenez le temps.",
              "clone: the Base folder, the reference and its words");
    });
}

VoiceOverRequest ValidRequest(const std::filesystem::path& root) {
    VoiceOverRequest request;
    request.mode = VoiceOverMode::kDesign;
    request.text = "Bonjour, et bienvenue.";
    request.language = "fr";
    request.description = "Une voix chaude";
    request.seed = 42;
    request.output_path = (root / "voice.wav").string();
    request.report_path = (root / "voice.json").string();
    return request;
}

// Runs `request` on a manager and returns the task's snapshot and result.
MediaTaskSnapshot Run(const VoiceOverSettings& settings,
                      const VoiceOverRequest& request,
                      VoiceOverResult& result) {
    MediaTaskManager manager(1);
    return RunMediaTaskAndWait(
        manager, MediaTaskKind::VoiceOver, "voice fixture",
        [&](MediaTaskContext& context, std::string& error) {
            return RunVoiceOver(settings, request, context, result, error);
        });
}

void TestRequestRefusals(const std::filesystem::path& root) {
    Test("a request the script cannot serve starts nothing", [&] {
        VoiceOverSettings settings = ScriptSettings();
        settings.python = (root / "never-started").string();
        struct Case {
            const char* name;
            std::function<void(VoiceOverRequest&)> spoil;
            const char* says;
        };
        const Case cases[] = {
            {"empty text", [](VoiceOverRequest& r) { r.text.clear(); },
             "the text to say is empty"},
            {"unknown language", [](VoiceOverRequest& r) { r.language = "nl"; },
             "do not say the language 'nl' (they say zh, en, de"},
            {"design without description",
             [](VoiceOverRequest& r) { r.description.clear(); },
             "made from a description"},
            {"clone without reference",
             [](VoiceOverRequest& r) {
                 r.mode = VoiceOverMode::kClone;
                 r.reference_text = "Mots.";
             },
             "cloned from a reference clip and its words"},
        };
        for (const Case& item : cases) {
            VoiceOverRequest request = ValidRequest(root);
            item.spoil(request);
            VoiceOverResult result;
            const MediaTaskSnapshot snapshot = Run(settings, request, result);
            Check(snapshot.state == MediaTaskState::Failed &&
                      Contains(snapshot.error, item.says) &&
                      !Contains(snapshot.error, "unable to start"),
                  std::string(item.name) + ": " + snapshot.error);
        }
    });
}

void TestRun(const std::filesystem::path& root) {
    // A shell standing in for Python: it records its arguments, then
    // writes the WAV and the report where they tell it to.
    const auto script = root / "voice_over.sh";
    const auto received = root / "received.txt";
    WriteScript(script,
                "for argument in \"$@\"; do printf '%s\\n' \"$argument\"; "
                "done > '" +
                    received.string() +
                    "'\n"
                    "while [ $# -gt 0 ]; do\n"
                    "  case \"$1\" in\n"
                    "    --out) out=\"$2\"; shift;;\n"
                    "    --report) report=\"$2\"; shift;;\n"
                    "  esac\n"
                    "  shift\n"
                    "done\n"
                    "printf 'RIFFfake' > \"$out\"\n"
                    "printf '{\"sample_rate\":24000,\"samples\":48000,"
                    "\"model\":\"Qwen3-TTS-12Hz-1.7B-VoiceDesign\","
                    "\"revision\":\"5ecdb67327fd37bb2e042aab12ff7391903235d3\","
                    "\"device\":\"cpu\",\"seed\":42}' > \"$report\"");
    VoiceOverSettings settings = ScriptSettings();
    settings.python = "/bin/sh";
    settings.script = script.string();

    Test("the report comes back, and the text crosses intact", [&] {
        VoiceOverRequest request = ValidRequest(root);
        request.text = "Le 21 mars — « on vous attend ».";
        VoiceOverResult result;
        const MediaTaskSnapshot snapshot = Run(settings, request, result);
        Check(snapshot.state == MediaTaskState::Succeeded,
              "the run succeeds: " + snapshot.error);
        Check(
            result.sample_rate == 24000 && result.samples == 48000 &&
                result.model == "Qwen3-TTS-12Hz-1.7B-VoiceDesign" &&
                result.revision == "5ecdb67327fd37bb2e042aab12ff7391903235d3" &&
                result.device == "cpu" && result.seed == 42,
            "every field of the report is read");
        Check(Contains(ReadText(received),
                       "--text\nLe 21 mars — « on vous attend ».\n"),
              "the text reaches the script byte for byte");
    });

    Test("a report left by an earlier run is not taken for this one", [&] {
        const auto silent = root / "silent.sh";
        WriteScript(silent, "exit 0");
        VoiceOverSettings quiet = settings;
        quiet.script = silent.string();
        VoiceOverRequest request = ValidRequest(root);
        std::ofstream(request.report_path)
            << "{\"sample_rate\":24000,\"samples\":1,\"seed\":42}";
        std::ofstream(request.output_path) << "old";
        VoiceOverResult result;
        const MediaTaskSnapshot snapshot = Run(quiet, request, result);
        Check(snapshot.state == MediaTaskState::Failed &&
                  Contains(snapshot.error, "wrote no report"),
              "the stale report was removed first: " + snapshot.error);
    });

    Test("a report without its fields, or without its WAV, fails", [&] {
        const auto partial = root / "partial.sh";
        WriteScript(partial,
                    "while [ $# -gt 0 ]; do\n"
                    "  case \"$1\" in --report) report=\"$2\"; shift;; esac\n"
                    "  shift\n"
                    "done\n"
                    "printf '{\"sample_rate\":24000,\"samples\":0,"
                    "\"seed\":1}' > \"$report\"");
        VoiceOverSettings other = settings;
        other.script = partial.string();
        VoiceOverResult result;
        MediaTaskSnapshot snapshot = Run(other, ValidRequest(root), result);
        Check(snapshot.state == MediaTaskState::Failed &&
                  Contains(snapshot.error, "lacks a positive sample_rate"),
              "zero samples is refused: " + snapshot.error);

        const auto noWav = root / "no-wav.sh";
        WriteScript(noWav,
                    "while [ $# -gt 0 ]; do\n"
                    "  case \"$1\" in --report) report=\"$2\"; shift;; esac\n"
                    "  shift\n"
                    "done\n"
                    "printf '{\"sample_rate\":24000,\"samples\":10,"
                    "\"seed\":1}' > \"$report\"");
        other.script = noWav.string();
        snapshot = Run(other, ValidRequest(root), result);
        Check(snapshot.state == MediaTaskState::Failed &&
                  Contains(snapshot.error, "did not write"),
              "a WAV reported but absent is refused: " + snapshot.error);
    });

    Test("the script's failure and its error output reach the caller", [&] {
        const auto failing = root / "failing.sh";
        WriteScript(failing,
                    "echo 'voice_over.py: the model returned no sound' >&2; "
                    "exit 1");
        VoiceOverSettings other = settings;
        other.script = failing.string();
        VoiceOverResult result;
        const MediaTaskSnapshot snapshot =
            Run(other, ValidRequest(root), result);
        Check(snapshot.state == MediaTaskState::Failed &&
                  Contains(snapshot.error, "the model returned no sound"),
              "the error tail is the error: " + snapshot.error);
    });

    Test("a running voice-over can be cancelled", [&] {
        const auto ready = root / "voice-ready";
        const auto slow = root / "slow.sh";
        WriteScript(slow,
                    "echo ready > '" + ready.string() + "'\nexec sleep 6");
        VoiceOverSettings other = settings;
        other.script = slow.string();
        const VoiceOverRequest request = ValidRequest(root);
        VoiceOverResult result;
        MediaTaskManager manager(1);
        const auto id = manager.Enqueue(
            MediaTaskKind::VoiceOver, "voice fixture",
            [&](MediaTaskContext& context, std::string& error) {
                return RunVoiceOver(other, request, context, result, error);
            });
        const auto start = std::chrono::steady_clock::now();
        while (!std::filesystem::exists(ready) && SecondsSince(start) < 3.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(std::filesystem::exists(ready), "the script started");
        Check(manager.Cancel(id), "cancellation was requested");
        Check(manager.WaitForIdle(4000), "the cancelled task finishes");
        const auto tasks = manager.Snapshot();
        Check(tasks.size() == 1 &&
                  tasks[0].state == MediaTaskState::Cancelled &&
                  tasks[0].kind == MediaTaskKind::VoiceOver &&
                  std::string(MediaTaskKindName(tasks[0].kind)) == "VoiceOver",
              "a cancelled voice-over says cancelled, under its own kind");
        Check(SecondsSince(start) < 5.0,
              "the sleeping script was stopped, not waited for");
    });
}

void TestCallerSettings(const std::filesystem::path& root) {
    Test("settings a caller filled are checked as they are", [&] {
        VoiceOverSettings settings;
        settings.python = (root / "absent" / "python").string();
        settings.script = (root / "absent.py").string();
        settings.models = (root / "models").string();
        std::filesystem::create_directories(root / "models");
        std::string refusal = MissingVoiceOverInstallation(settings);
        Check(VoiceOverRefusalName(refusal) == "VoiceOverNotFound" &&
                  Contains(refusal, "the voice-over's Python is not at '" +
                                        settings.python + "'") &&
                  Contains(refusal, "scripts/voice_over.py is not at '"),
              "absent paths are named: " + refusal);

        WriteScript(settings.python, "exit 0");
        WriteScript(settings.script, "exit 0");
        refusal = MissingVoiceOverInstallation(settings);
        Check(Contains(refusal,
                       "lacks Qwen3-TTS-12Hz-1.7B-VoiceDesign and "
                       "Qwen3-TTS-12Hz-1.7B-Base"),
              "an empty models folder lacks both models: " + refusal);

        for (const char* folder : {kVoiceDesignModel, kVoiceCloneModel}) {
            for (const std::string& file : VoiceModelFiles()) {
                const auto path = root / "models" / folder / file;
                std::filesystem::create_directories(path.parent_path());
                std::ofstream(path) << "x";
            }
        }
        Check(MissingVoiceOverInstallation(settings).empty(),
              "complete on disk: nothing missing");
    });
}

}  // namespace

int main() {
    std::error_code ignored;
    const std::filesystem::path root =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("cutmachine-voice-over-" + std::to_string(getpid()));
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    // The machine's own settings must not decide this suite's answers.
    const std::filesystem::path settings = root / "settings.env";
    std::ofstream(settings) << "# GN6a test settings\n";
    setenv("CUTMACHINE_ENV_FILE", settings.c_str(), 1);
    unsetenv(kVoicePythonVariable);
    unsetenv(kVoiceScriptVariable);
    unsetenv(kVoiceModelsVariable);

    TestSearchOrder();
    TestLanguages();
    TestArguments();
    TestRequestRefusals(root);
    TestRun(root);
    TestCallerSettings(root);

    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All voice-over tests passed\n";
    return 0;
}
