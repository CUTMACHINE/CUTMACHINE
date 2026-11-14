#include "WhisperModelSetting.h"

#include "LocalEnv.h"
#include "Transcription.h"

#include <string>

namespace whisper_model_setting {
namespace {

std::string FileName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

Choice SaveChosenModel(const std::string& path) {
    Choice choice;
    choice.path = path;
    const local_env::WriteOutcome written =
        local_env::WriteFileValue(kWhisperModelVariable, path);
    choice.saved = written.written;
    choice.error = written.error;
    choice.environment_wins = written.environment_wins;
    choice.environment_value = written.environment_value;
    return choice;
}

std::string ChoiceStatus(const Choice& choice) {
    const std::string name = FileName(choice.path);
    if (!choice.saved) {
        return "Modèle Whisper · " + name +
               ", pour cette transcription seulement : il n'a pas pu être "
               "enregistré (" +
               choice.error + ")";
    }
    if (choice.environment_wins) {
        return "Modèle Whisper · " + name + " enregistré, mais " +
               kWhisperModelVariable + " de l'environnement l'emporte : " +
               (choice.environment_value.empty()
                    ? std::string("vide")
                    : FileName(choice.environment_value));
    }
    return "Modèle Whisper · " + name + ", pour la fenêtre et les agents";
}

Migration MigrateWindowPreference(const std::string& preference) {
    Migration migration;
    if (preference.empty()) {
        return migration;
    }
    if (!local_env::FileValue(kWhisperModelVariable).empty()) {
        migration.outcome = MigrationOutcome::kEngineSettingKept;
        return migration;
    }
    const local_env::WriteOutcome written =
        local_env::WriteFileValue(kWhisperModelVariable, preference);
    if (!written.written) {
        migration.outcome = MigrationOutcome::kFailed;
        migration.forget_preference = false;
        migration.error = written.error;
        return migration;
    }
    migration.outcome = MigrationOutcome::kMigrated;
    return migration;
}

const char* MigrationOutcomeName(MigrationOutcome outcome) {
    switch (outcome) {
        case MigrationOutcome::kNothing:
            return "nothing";
        case MigrationOutcome::kMigrated:
            return "migrated";
        case MigrationOutcome::kEngineSettingKept:
            return "engine_setting_kept";
        case MigrationOutcome::kFailed:
            return "failed";
    }
    return "nothing";
}

}  // namespace whisper_model_setting
