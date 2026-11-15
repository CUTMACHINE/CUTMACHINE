// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. One Whisper model setting,
// the engine's.
//
// The settings file is written now (local_env::WriteFileValue): by the
// window's « Modèle Whisper… », and once to migrate the window's old
// preference (WhisperModelSetting.h). Every write here goes to a settings
// file under a temporary folder of the suite's own: CUTMACHINE_ENV_FILE
// points there, and HOME too, so that a path that went wrong still could
// not reach the user's ~/.config.
//
// Plain C++ and POSIX: no AppKit, no Whisper model, no network.

#include "LocalEnv.h"
#include "MediaTaskManager.h"
#include "Transcription.h"
#include "Ulid.h"
#include "WhisperModelSetting.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using whisper_model_setting::MigrationOutcome;

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

std::string ReadAll(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void WriteAll(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

mode_t ModeOf(const fs::path& path) {
    struct stat status {};
    ::stat(path.c_str(), &status);
    return status.st_mode & 07777;
}

std::string Getenv(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "<unset>";
}

// The settings file of one test, under the suite's root.
fs::path UseSettingsFile(const fs::path& path) {
    ::setenv(local_env::kEnvFileVariable, path.c_str(), 1);
    return path;
}

constexpr const char* kName = "CUTMACHINE_DEC23_TEST";

void TestWriteFileValue(const fs::path& root) {
    Test("a missing file and folder are created, the owner's alone", [&] {
        const fs::path settings =
            UseSettingsFile(root / "new" / "cutmachine" / ".env");
        ::unsetenv(kName);
        const local_env::WriteOutcome outcome =
            local_env::WriteFileValue(kName, "/models/ggml-large-v3.bin");
        Check(outcome.written && outcome.error.empty(),
              "written: " + outcome.error);
        Check(ReadAll(settings) ==
                  std::string(kName) + "=/models/ggml-large-v3.bin\n",
              "one line: " + ReadAll(settings));
        Check(ModeOf(settings) == 0600, "readable by its owner only: " +
                                            std::to_string(ModeOf(settings)));
        Check(local_env::FileValue(kName) == "/models/ggml-large-v3.bin",
              "FileValue reads it back");
        Check(Getenv(kName) == "/models/ggml-large-v3.bin" &&
                  !outcome.environment_wins,
              "and this process reads it: " + Getenv(kName));
        Check(!fs::exists(settings.string() + ".tmp-" +
                          std::to_string(::getpid())),
              "no temporary file is left");
    });

    Test("the line that sets it is replaced, every other kept", [&] {
        const fs::path settings = UseSettingsFile(root / "kept" / ".env");
        const std::string before =
            "# Mes réglages\n"
            "ANTHROPIC_API_KEY=sk-not-a-key\n"
            "\n"
            "export " +
            std::string(kName) +
            "=\"/old/model.bin\"\n"
            "CUTMACHINE_FFMPEG=/opt/ffmpeg\n" +
            kName + "=/second/model.bin\n";
        WriteAll(settings, before);
        ::chmod(settings.c_str(), 0640);
        // The value LoadDotEnv put in this process, from the file.
        ::setenv(kName, "/old/model.bin", 1);
        const local_env::WriteOutcome outcome =
            local_env::WriteFileValue(kName, "/new/model.bin");
        Check(outcome.written, "written: " + outcome.error);
        const std::string expected =
            "# Mes réglages\n"
            "ANTHROPIC_API_KEY=sk-not-a-key\n"
            "\n"
            "export " +
            std::string(kName) +
            "=/new/model.bin\n"
            "CUTMACHINE_FFMPEG=/opt/ffmpeg\n" +
            kName + "=/second/model.bin\n";
        Check(ReadAll(settings) == expected,
              "the first setting line replaced, `export` kept, the rest "
              "byte for byte: " +
                  ReadAll(settings));
        Check(ModeOf(settings) == 0640, "the file keeps its mode");
        Check(local_env::FileValue(kName) == "/new/model.bin" &&
                  Getenv(kName) == "/new/model.bin" &&
                  !outcome.environment_wins,
              "the file's value was this process's: it follows");
        Check(local_env::FileValue("ANTHROPIC_API_KEY") == "sk-not-a-key",
              "the key beside it is untouched");
    });

    Test("a path with spaces or quotes reads back the same", [&] {
        const fs::path settings = UseSettingsFile(root / "quoted" / ".env");
        for (const std::string& value :
             {std::string("/Volumes/T7 Shield/models/ggml-large-v3.bin"),
              std::string("/Users/me/l'atelier/ggml.bin"),
              std::string("/models/$HOME/#1 (v3).bin"), std::string(" lead"),
              std::string("\"quoted\"")}) {
            ::unsetenv(kName);
            const local_env::WriteOutcome outcome =
                local_env::WriteFileValue(kName, value);
            Check(outcome.written && local_env::FileValue(kName) == value,
                  "'" + value + "' reads back: " + local_env::FileValue(kName) +
                      " -- " + ReadAll(settings));
            // As a new process would: the file loaded into an empty
            // environment.
            ::unsetenv(kName);
            Check(local_env::Value(kName) == value,
                  "'" + value + "' loads back: " + local_env::Value(kName));
        }
        Check(Contains(ReadAll(settings), "='\"quoted\"'"),
              "quotes are kept inside quotes: " + ReadAll(settings));
    });

    Test("an empty value or a line break is refused, the file untouched", [&] {
        const fs::path settings = UseSettingsFile(root / "refused" / ".env");
        WriteAll(settings, std::string(kName) + "=/kept.bin\n");
        for (const std::string& value :
             {std::string(), std::string("/a.bin\nEVIL=1"),
              std::string("/a.bin\r"), std::string("/a\0b", 4)}) {
            const local_env::WriteOutcome outcome =
                local_env::WriteFileValue(kName, value);
            Check(!outcome.written && !outcome.error.empty(),
                  "refused, with a reason: " + outcome.error);
        }
        Check(ReadAll(settings) == std::string(kName) + "=/kept.bin\n",
              "the file is as it was: " + ReadAll(settings));
        Check(!local_env::WriteFileValue("BAD NAME", "/x").written &&
                  !local_env::WriteFileValue("", "/x").written,
              "a name the file cannot carry is refused");
    });

    Test("a link is written through and stays a link", [&] {
        const fs::path real = root / "dotfiles" / "cutmachine.env";
        WriteAll(real, "# dotfiles\n");
        const fs::path link = root / "linked" / ".env";
        fs::create_directories(link.parent_path());
        fs::create_symlink(real, link);
        UseSettingsFile(link);
        ::unsetenv(kName);
        const local_env::WriteOutcome outcome =
            local_env::WriteFileValue(kName, "/linked/model.bin");
        Check(outcome.written, "written: " + outcome.error);
        Check(fs::is_symlink(link), "the link is still a link");
        Check(ReadAll(real) ==
                  "# dotfiles\n" + std::string(kName) + "=/linked/model.bin\n",
              "the file it points to has the line: " + ReadAll(real));
    });

    Test("an exported value keeps winning, and is said", [&] {
        const fs::path settings = UseSettingsFile(root / "exported" / ".env");
        WriteAll(settings, std::string(kName) + "=/file/model.bin\n");
        ::setenv(kName, "/exported/model.bin", 1);
        const local_env::WriteOutcome outcome =
            local_env::WriteFileValue(kName, "/chosen/model.bin");
        Check(outcome.written &&
                  local_env::FileValue(kName) == "/chosen/model.bin",
              "the file has the choice");
        Check(outcome.environment_wins &&
                  outcome.environment_value == "/exported/model.bin" &&
                  Getenv(kName) == "/exported/model.bin",
              "the environment's value stays, and the outcome names it");

        // Exported empty hides the file's value from the engine: it still
        // does.
        ::setenv(kName, "", 1);
        const local_env::WriteOutcome hidden =
            local_env::WriteFileValue(kName, "/again/model.bin");
        Check(hidden.written && hidden.environment_wins &&
                  hidden.environment_value.empty() && Getenv(kName).empty(),
              "an empty export still wins");
        ::unsetenv(kName);
    });

    Test("a folder that cannot be written leaves the file whole", [&] {
        if (::geteuid() == 0) {
            std::cout << "SKIP: root writes anywhere\n";
            return;
        }
        const fs::path folder = root / "locked";
        const fs::path settings = UseSettingsFile(folder / ".env");
        WriteAll(settings, std::string(kName) + "=/kept.bin\n");
        ::chmod(folder.c_str(), 0500);
        const local_env::WriteOutcome outcome =
            local_env::WriteFileValue(kName, "/new.bin");
        ::chmod(folder.c_str(), 0700);
        Check(!outcome.written && Contains(outcome.error, "cannot create"),
              "refused, named: " + outcome.error);
        Check(ReadAll(settings) == std::string(kName) + "=/kept.bin\n",
              "the old file is whole");
    });
}

void TestChoice(const fs::path& root) {
    const char* variable = kWhisperModelVariable;
    Test("« Modèle Whisper… » writes the engine's setting", [&] {
        const fs::path settings = UseSettingsFile(root / "choice" / ".env");
        WriteAll(settings, "# réglages\n");
        ::unsetenv(variable);
        const whisper_model_setting::Choice choice =
            whisper_model_setting::SaveChosenModel(
                "/Volumes/T7/models/ggml-large-v3.bin");
        Check(choice.saved && choice.error.empty() && !choice.environment_wins,
              "saved: " + choice.error);
        Check(ReadAll(settings) ==
                  "# réglages\nCUTMACHINE_WHISPER_MODEL="
                  "/Volumes/T7/models/ggml-large-v3.bin\n",
              "as CUTMACHINE_WHISPER_MODEL: " + ReadAll(settings));
        // What every tool resolves now, the window's menus included.
        const fs::path model = root / "choice" / "ggml-tiny.bin";
        WriteAll(model, "ggml");
        Check(whisper_model_setting::SaveChosenModel(model.string()).saved,
              "a second choice replaces the first");
        std::string path;
        std::string reason;
        Check(ResolveConfiguredWhisperModel(path, reason) &&
                  path == model.string(),
              "the tools resolve the window's choice: " + path + reason);
        Check(whisper_model_setting::ChoiceStatus(choice) ==
                  "Modèle Whisper · ggml-large-v3.bin, pour la fenêtre et "
                  "les agents",
              "the window's line: " +
                  whisper_model_setting::ChoiceStatus(choice));
        ::unsetenv(variable);
    });

    Test("the window's line when the choice does not take", [&] {
        whisper_model_setting::Choice choice;
        choice.path = "/m/ggml-base.bin";
        choice.saved = true;
        choice.environment_wins = true;
        choice.environment_value = "/elsewhere/ggml-large-v3.bin";
        Check(whisper_model_setting::ChoiceStatus(choice) ==
                  "Modèle Whisper · ggml-base.bin enregistré, mais "
                  "CUTMACHINE_WHISPER_MODEL de l'environnement l'emporte : "
                  "ggml-large-v3.bin",
              "the environment wins: " +
                  whisper_model_setting::ChoiceStatus(choice));
        choice.saved = false;
        choice.environment_wins = false;
        choice.error = "cannot create '/x/.env.tmp-1': Permission denied";
        Check(whisper_model_setting::ChoiceStatus(choice) ==
                  "Modèle Whisper · ggml-base.bin, pour cette transcription "
                  "seulement : il n'a pas pu être enregistré (cannot create "
                  "'/x/.env.tmp-1': Permission denied)",
              "not saved: " + whisper_model_setting::ChoiceStatus(choice));
    });
}

void TestMigration(const fs::path& root) {
    const char* variable = kWhisperModelVariable;
    using whisper_model_setting::MigrateWindowPreference;

    Test("no preference, nothing migrated", [&] {
        const fs::path settings = UseSettingsFile(root / "none" / ".env");
        const auto migration = MigrateWindowPreference("");
        Check(migration.outcome == MigrationOutcome::kNothing &&
                  migration.forget_preference && !fs::exists(settings),
              "nothing written");
    });

    Test("the preference becomes the engine's setting, once", [&] {
        const fs::path settings = UseSettingsFile(root / "migrate" / ".env");
        WriteAll(settings, "ANTHROPIC_API_KEY=sk-not-a-key\n");
        ::unsetenv(variable);
        const auto migration =
            MigrateWindowPreference("/Users/me/whisper/ggml-medium.bin");
        Check(
            migration.outcome == MigrationOutcome::kMigrated &&
                migration.forget_preference && migration.error.empty(),
            std::string("migrated: ") +
                whisper_model_setting::MigrationOutcomeName(migration.outcome) +
                " " + migration.error);
        Check(
            ReadAll(settings) ==
                "ANTHROPIC_API_KEY=sk-not-a-key\n"
                "CUTMACHINE_WHISPER_MODEL=/Users/me/whisper/ggml-medium.bin\n",
            "the file has it: " + ReadAll(settings));
        // Run again (the preference was not removed, say): the file has a
        // model now, which stays.
        const std::string after = ReadAll(settings);
        Check(MigrateWindowPreference("/Users/me/whisper/ggml-medium.bin")
                          .outcome == MigrationOutcome::kEngineSettingKept &&
                  ReadAll(settings) == after,
              "a second run changes nothing");
        ::unsetenv(variable);
    });

    Test("a model the engine already has stays", [&] {
        const fs::path settings = UseSettingsFile(root / "kept-model" / ".env");
        const std::string before =
            "CUTMACHINE_WHISPER_MODEL=/models/ggml-large-v3.bin\n";
        WriteAll(settings, before);
        const auto migration =
            MigrateWindowPreference("/Users/me/whisper/ggml-medium.bin");
        Check(migration.outcome == MigrationOutcome::kEngineSettingKept &&
                  migration.forget_preference,
              "the engine's setting is kept, the preference forgotten");
        Check(ReadAll(settings) == before, "the file is byte for byte");
    });

    Test("only the file decides: an exported model is a one-off", [&] {
        const fs::path settings = UseSettingsFile(root / "one-off" / ".env");
        ::setenv(variable, "/tmp/one-off.bin", 1);
        const auto migration =
            MigrateWindowPreference("/Users/me/whisper/ggml-medium.bin");
        Check(migration.outcome == MigrationOutcome::kMigrated &&
                  local_env::FileValue(variable) ==
                      "/Users/me/whisper/ggml-medium.bin" &&
                  Getenv(variable) == "/tmp/one-off.bin",
              "migrated into the file; the export still wins here");
        ::unsetenv(variable);
    });

    Test("a file that cannot be written keeps the preference", [&] {
        if (::geteuid() == 0) {
            std::cout << "SKIP: root writes anywhere\n";
            return;
        }
        const fs::path folder = root / "read-only";
        UseSettingsFile(folder / ".env");
        fs::create_directories(folder);
        ::chmod(folder.c_str(), 0500);
        const auto migration =
            MigrateWindowPreference("/Users/me/whisper/ggml-medium.bin");
        ::chmod(folder.c_str(), 0700);
        Check(migration.outcome == MigrationOutcome::kFailed &&
                  !migration.forget_preference && !migration.error.empty(),
              "failed, named, and the next launch tries again: " +
                  migration.error);
    });
}

// AL4 -- « Transcrire les hésitations (français)… » runs on the
// hesitations weights or refuses, saying why and what to set. A three-byte
// file stands for the weights (the SHA-256 of "abc", FIPS 180-2).
void TestHesitations(const fs::path& root) {
    using whisper_model_setting::HesitationsFacts;
    using whisper_model_setting::HesitationsProblem;
    using whisper_model_setting::HesitationsWeightsProblem;
    using whisper_model_setting::RefuseHesitations;
    const fs::path evaluated = root / "weights" / "evaluated.bin";
    const fs::path other = root / "weights" / "other.bin";
    WriteAll(evaluated, "abc");
    WriteAll(other, "abd");
    const ScopedEvaluatedWhisperModelForTesting identity(
        3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    Test("the hesitations action knows what refuses before reading", [&] {
        HesitationsFacts facts;
        facts.settings_file = "/machine/.config/cutmachine/.env";
        Check(HesitationsWeightsProblem(facts) == HesitationsProblem::kUnset,
              "unset");
        facts.path = (root / "nowhere.bin").string();
        Check(
            HesitationsWeightsProblem(facts) == HesitationsProblem::kUnreadable,
            "a path to nothing");
        facts.path = other.string();
        facts.readable = true;
        Check(
            HesitationsWeightsProblem(facts) == HesitationsProblem::kUnverified,
            "a file not read yet: the task decides");
        facts.evaluated = false;
        Check(HesitationsWeightsProblem(facts) ==
                  HesitationsProblem::kOtherWeights,
              "other weights");
        facts.evaluated = true;
        Check(HesitationsWeightsProblem(facts) == HesitationsProblem::kNone,
              "the evaluated weights");
    });

    Test("each refusal says why and what to set", [&] {
        HesitationsFacts facts;
        facts.settings_file = "/machine/.config/cutmachine/.env";
        const auto unset = RefuseHesitations(HesitationsProblem::kUnset, facts);
        facts.path = "/models/elsewhere.bin";
        const auto unreadable =
            RefuseHesitations(HesitationsProblem::kUnreadable, facts);
        const auto weights =
            RefuseHesitations(HesitationsProblem::kOtherWeights, facts);
        for (const auto& refusal : {unset, unreadable, weights}) {
            Check(refusal.title == "Transcription des hésitations impossible" &&
                      Contains(refusal.detail,
                               "Réglez CUTMACHINE_WHISPER_HESITATIONS_MODEL") &&
                      Contains(refusal.detail,
                               "/machine/.config/cutmachine/.env") &&
                      Contains(refusal.detail, "5b05a0870a122a83"),
                  "the setting, the file and the weights to look for: " +
                      refusal.detail);
        }
        Check(Contains(unset.detail, "ne sont pas réglés"), unset.detail);
        Check(Contains(unreadable.detail, "/models/elsewhere.bin") &&
                  Contains(unreadable.detail, "pas un fichier lisible"),
              unreadable.detail);
        Check(Contains(weights.detail, "n'est pas les poids s075 évalués"),
              weights.detail);
        Check(
            RefuseHesitations(HesitationsProblem::kNone, facts).detail.empty(),
            "nothing to refuse, nothing said");
    });

    Test("this process's facts come from the engine's setting", [&] {
        ::unsetenv(kWhisperHesitationsModelVariable);
        Check(whisper_model_setting::CurrentHesitationsFacts().path.empty(),
              "unset");
        ::setenv(kWhisperHesitationsModelVariable, other.c_str(), 1);
        const HesitationsFacts facts =
            whisper_model_setting::CurrentHesitationsFacts();
        Check(facts.path == other.string() && facts.readable &&
                  !facts.evaluated.has_value(),
              "a readable file, not read by the main thread");
        ::unsetenv(kWhisperHesitationsModelVariable);
    });

    Test("the task reads the weights and refuses others", [&] {
        MediaTaskManager manager;
        const auto ready = [&](const HesitationsFacts& facts,
                               std::string& error) {
            bool result = false;
            manager.Enqueue(
                MediaTaskKind::Transcription, "hesitations",
                [&](MediaTaskContext& context, std::string&) {
                    result = whisper_model_setting::HesitationsWeightsReady(
                        facts, context, error);
                    return true;
                });
            Check(manager.WaitForIdle(30000), "the check terminates");
            return result;
        };
        HesitationsFacts facts;
        facts.path = evaluated.string();
        facts.readable = true;
        std::string error;
        Check(ready(facts, error) && error.empty(),
              "the evaluated weights: ready: " + error);
        facts.path = other.string();
        Check(!ready(facts, error) &&
                  Contains(error, "n'est pas les poids s075 évalués"),
              "other weights of the same size: refused, said: " + error);
        facts = HesitationsFacts();
        Check(!ready(facts, error) && Contains(error, "ne sont pas réglés"),
              "unset: refused: " + error);
    });
}

}  // namespace

int main() {
    try {
        const fs::path root = fs::canonical(fs::temp_directory_path()) /
                              (GenerateUlid() + "-whisper-model-setting");
        fs::create_directories(root / "home");
        // Neither the user's settings file nor their home: a write that
        // went to the wrong place would land here.
        ::setenv("HOME", (root / "home").c_str(), 1);
        UseSettingsFile(root / "unset" / ".env");

        TestWriteFileValue(root);
        TestChoice(root);
        TestMigration(root);
        TestHesitations(root);

        Check(!fs::exists(root / "home" / ".config"),
              "nothing was written under the made-up home");
        std::error_code ignored;
        fs::remove_all(root, ignored);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All Whisper model setting tests passed\n";
    return 0;
}
