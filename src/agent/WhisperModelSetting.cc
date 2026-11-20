#include "WhisperModelSetting.h"

#include "LocalEnv.h"
#include "Transcription.h"

#include <filesystem>
#include <fstream>
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

HesitationsFacts CurrentHesitationsFacts() {
    HesitationsFacts facts;
    facts.path = ConfiguredWhisperHesitationsModel();
    facts.settings_file = local_env::LocalEnvFilePath();
    if (facts.path.empty()) {
        return facts;
    }
    std::error_code error;
    const bool regular = std::filesystem::is_regular_file(facts.path, error);
    const std::uintmax_t size =
        regular ? std::filesystem::file_size(facts.path, error) : 0;
    facts.readable = regular && !error && size > 0 &&
                     std::ifstream(facts.path, std::ios::binary).good();
    if (facts.readable) {
        facts.evaluated = KnownWhisperDecodingModel(facts.path);
    }
    return facts;
}

HesitationsProblem HesitationsWeightsProblem(const HesitationsFacts& facts) {
    if (facts.path.empty()) {
        return HesitationsProblem::kUnset;
    }
    if (!facts.readable) {
        return HesitationsProblem::kUnreadable;
    }
    if (!facts.evaluated) {
        return HesitationsProblem::kUnverified;
    }
    return *facts.evaluated ? HesitationsProblem::kNone
                            : HesitationsProblem::kOtherWeights;
}

HesitationsRefusal RefuseHesitations(HesitationsProblem problem,
                                     const HesitationsFacts& facts) {
    HesitationsRefusal refusal;
    refusal.title = "Transcription des hésitations impossible";
    const std::string variable = kWhisperHesitationsModelVariable;
    const std::string where =
        facts.settings_file.empty()
            ? std::string("dans l'environnement")
            : "dans l'environnement ou dans " + facts.settings_file;
    const std::string repair =
        " Réglez " + variable + " sur les poids s075 évalués (SHA-256 " +
        std::string(kFrenchHesitationsModelSha256).substr(0, 16) + "…), " +
        where +
        ". « Transcrire avec Whisper… » transcrit sans eux, mais sans les "
        "« euh ».";
    switch (problem) {
        case HesitationsProblem::kNone:
        case HesitationsProblem::kUnverified:
            refusal.title.clear();
            return refusal;
        case HesitationsProblem::kUnset:
            refusal.detail =
                "Les poids des hésitations ne sont pas réglés : " + variable +
                " n'est pas défini." + repair;
            return refusal;
        case HesitationsProblem::kUnreadable:
            refusal.detail = variable + " désigne " + facts.path +
                             ", qui n'est pas un fichier lisible." + repair;
            return refusal;
        case HesitationsProblem::kOtherWeights:
            refusal.detail = facts.path + ", que désigne " + variable +
                             ", n'est pas les poids s075 évalués : son "
                             "empreinte diffère." +
                             repair;
            return refusal;
    }
    return refusal;
}

bool HesitationsWeightsReady(const HesitationsFacts& facts,
                             MediaTaskContext& context, std::string& error) {
    error.clear();
    HesitationsFacts read = facts;
    if (HesitationsWeightsProblem(read) == HesitationsProblem::kUnverified) {
        bool evaluated = false;
        if (!RecognizeWhisperDecodingModel(read.path, context, evaluated,
                                           error)) {
            if (context.Cancelled()) {
                return false;
            }
            read.readable = false;
        } else {
            read.evaluated = evaluated;
        }
    }
    const HesitationsProblem problem = HesitationsWeightsProblem(read);
    if (problem == HesitationsProblem::kNone) {
        error.clear();
        return true;
    }
    error = RefuseHesitations(problem, read).detail;
    return false;
}

}  // namespace whisper_model_setting
