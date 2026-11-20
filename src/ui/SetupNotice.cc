#include "SetupNotice.h"

#include "FfmpegBinary.h"
#include "FfmpegCapabilities.h"
#include "Json.h"
#include "MfaAlignment.h"
#include "Transcription.h"
#include "VoiceOver.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace setup_notice {
namespace {

using mcp_json::Value;

// The menus a repair sends people to, as the menu bar spells them.
constexpr const char* kWhisperMenu = "CUTMACHINE ▸ Modèle Whisper…";
constexpr const char* kEngineMenu = "CUTMACHINE ▸ Moteur IA…";

// "A", "A et B", "A, B et C".
std::string Enumeration(const std::vector<std::string>& items) {
    std::string text;
    for (size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            text += index + 1 == items.size() ? " et " : ", ";
        }
        text += items[index];
    }
    return text;
}

std::string TextOf(const Value& object, const std::string& key) {
    const Value* value = object.Find(key);
    return value != nullptr && value->IsString() ? value->AsString()
                                                 : std::string();
}

// "transcribe_media, transcribe_timeline et inspect_cut refusent".
std::string ToolsRefuse(const std::vector<std::string>& tools) {
    if (tools.empty()) {
        return "ses outils refusent";
    }
    return Enumeration(tools) + (tools.size() == 1 ? " refuse" : " refusent");
}

// Where a machine-local setting goes, as a repair says it.
std::string WhereSettingsGo(const WindowFacts& facts) {
    return facts.settings_file.empty()
               ? std::string("dans l'environnement")
               : "dans l'environnement ou dans " + facts.settings_file;
}

// Where the report read a setting, from its origin name (SetupCheck.h).
std::string ReadFrom(const std::string& origin, const WindowFacts& facts) {
    if (origin !=
        setup::SettingOriginName(setup::SettingOrigin::kSettingsFile)) {
        return "dans l'environnement";
    }
    return facts.settings_file.empty() ? std::string("dans ses réglages")
                                       : "dans " + facts.settings_file;
}

Entry BaseEntry(const setup::Item& item) {
    Entry entry;
    entry.id = item.id;
    entry.title = ItemTitle(item.id);
    entry.state = item.state;
    entry.required = item.required;
    return entry;
}

// What each capability of FfmpegCapabilities.h prevents, in French, by its
// name. A capability added there and not here is still listed, by name.
struct CapabilityText {
    const char* name;
    const char* prevents;
};

constexpr CapabilityText kCapabilityTexts[] = {
    {"h264_videotoolbox",
     "un export H.264 ne peut pas prendre l'encodeur matériel, celui par "
     "défaut d'export_timeline et de render_motion_video"},
    {"hevc_videotoolbox",
     "un export HEVC ne peut pas prendre l'encodeur matériel, celui des "
     "préréglages de la fenêtre et d'export_timeline en HEVC"},
    {"libx264",
     "un export H.264 ne peut pas prendre l'encodeur logiciel "
     "(software: true)"},
    {"libx265",
     "un export HEVC ne peut pas prendre l'encodeur logiciel "
     "(software: true, et le préréglage master de la fenêtre)"},
    {"prores_ks",
     "les proxies ProRes ne se génèrent pas, et render_motion_video ne peut "
     "pas écrire de vidéo avec alpha"},
};

const char* CapabilityPrevents(const std::string& name) {
    for (const CapabilityText& text : kCapabilityTexts) {
        if (name == text.name) {
            return text.prevents;
        }
    }
    return nullptr;
}

// The configure options that bring a capability, from the engine's table:
// its words that are options ("--enable-libfreetype and
// --enable-libharfbuzz" gives two), so no English joins them here.
std::vector<std::string> ConfigureOptionsOf(const std::string& name) {
    std::vector<std::string> options;
    for (const ffmpeg::Capability& capability : ffmpeg::KnownCapabilities()) {
        if (name != capability.name) {
            continue;
        }
        const std::string words = capability.build_option;
        size_t start = 0;
        while (start < words.size()) {
            const size_t end = std::min(words.find(' ', start), words.size());
            const std::string word = words.substr(start, end - start);
            if (word.rfind("--", 0) == 0) {
                options.push_back(word);
            }
            start = end + 1;
        }
    }
    return options;
}

std::string FfmpegOriginText(const std::string& origin) {
    if (origin == ffmpeg::OriginName(ffmpeg::Origin::kConfigured)) {
        return std::string("réglé par ") + ffmpeg::kBinaryVariable;
    }
    if (origin == ffmpeg::OriginName(ffmpeg::Origin::kBundled)) {
        return "à côté de l'application";
    }
    return "trouvé dans le PATH";
}

Entry FfmpegEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    const std::string variable = ffmpeg::kBinaryVariable;
    const std::string where = WhereSettingsGo(facts);
    const std::string path = TextOf(item.found, "path");
    if (path.empty()) {
        entry.found.emplace_back("FFmpeg introuvable.");
        if (item.state == setup::State::kOk) {
            return entry;
        }
        entry.prevents =
            "Sans lui, les rushes s'importent sans niveau sonore, vignettes, "
            "formes d'onde ni proxies, et les analyses du son et de l'image, "
            "la transcription, les planches, les vidéos de motion et tout "
            "export vidéo refusent.";
        entry.repair =
            "installer FFmpeg, puis indiquer son chemin dans " + variable +
            ", " + where +
            ". Ouverte depuis le Finder, l'application ne voit pas le PATH du "
            "terminal : un FFmpeg qui s'y trouve doit lui être indiqué ainsi.";
        return entry;
    }
    const std::string version = TextOf(item.found, "version");
    entry.found.push_back(
        "FFmpeg" + (version.empty() ? std::string() : " " + version) + " : " +
        path + ", " + FfmpegOriginText(TextOf(item.found, "origin")) + ".");
    if (item.state == setup::State::kOk) {
        return entry;
    }
    if (item.error == ffmpeg::kProbeFailedErrorName ||
        !item.missing.IsArray() || item.missing.AsArray().empty()) {
        entry.found.emplace_back(
            "Il ne répond pas : ses filtres et ses encodeurs n'ont pas pu "
            "être lus.");
        entry.prevents =
            "Sans réponse, rien n'est vérifié avant un lancement : un export "
            "peut échouer en cours de route.";
        entry.repair = "vérifier que « " + path +
                       " -version » fonctionne dans un terminal, ou indiquer "
                       "dans " +
                       variable + " un FFmpeg qui répond, " + where + ".";
        return entry;
    }
    std::vector<std::string> names;
    std::vector<std::string> clauses;
    std::vector<std::string> options;
    for (const Value& row : item.missing.AsArray()) {
        const std::string name = TextOf(row, "name");
        const bool filter = TextOf(row, "kind") ==
                            ffmpeg::KindName(ffmpeg::CapabilityKind::kFilter);
        names.push_back(name + (filter ? " (filtre)" : " (encodeur)"));
        if (const char* prevents = CapabilityPrevents(name)) {
            clauses.push_back("sans " + name + ", " + prevents);
        }
        for (const std::string& option : ConfigureOptionsOf(name)) {
            if (std::find(options.begin(), options.end(), option) ==
                options.end()) {
                options.push_back(option);
            }
        }
    }
    entry.found.push_back("Il lui manque " + Enumeration(names) + ".");
    if (!clauses.empty()) {
        std::string prevents;
        for (const std::string& clause : clauses) {
            prevents += (prevents.empty() ? "" : " ; ") + clause;
        }
        // "sans libx265, ..." opens the sentence.
        prevents[0] = 'S';
        entry.prevents = prevents + ". Le reste fonctionne.";
    }
    entry.repair = (options.empty() ? std::string("installer un FFmpeg complet")
                                    : "installer un FFmpeg compilé avec " +
                                          Enumeration(options)) +
                   ", ou indiquer dans " + variable + " un FFmpeg qui l'est, " +
                   where + ".";
    return entry;
}

// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. One setting, the engine's:
// the window's menus read it as the tools do, and its menu writes it. The
// entry used to say which of two models served which reader.
Entry WhisperEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    const std::string variable = kWhisperModelVariable;
    const std::string menu = kWhisperMenu;
    const std::string path = TextOf(item.found, "path");
    int64_t bytes = 0;
    const Value* size = item.found.Find("bytes");
    if (size != nullptr) {
        size->AsInt64(bytes);
    }
    if (item.state == setup::State::kOk) {
        entry.found.push_back(
            path + ", par " + variable + " " +
            ReadFrom(TextOf(item.found, "origin"), facts) +
            " : il sert aux menus de la fenêtre, aux agents et à la ligne de "
            "commande.");
        return entry;
    }
    if (item.state == setup::State::kIncomplete && !path.empty()) {
        entry.found.push_back(
            path + ", par " + variable + ", " +
            (bytes == 0 ? "est vide." : "ne peut pas être lu."));
    } else {
        entry.found.push_back(variable + " ne désigne aucun modèle : " +
                              ToolsRefuse(item.tools) + ".");
    }
    entry.prevents =
        "Sans modèle, rien ne se transcrit : " + ToolsRefuse(item.tools) +
        ", et les menus de transcription de la fenêtre demandent un fichier.";
    entry.repair =
        "choisir un modèle ggml de whisper.cpp (ggml-large-v3.bin, environ "
        "3 Go) par " +
        menu + ", qui l'écrit dans " + variable +
        ", ou l'y indiquer soi-même " + WhereSettingsGo(facts) + ".";
    return entry;
}

Entry MfaEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    if (item.state == setup::State::kOk) {
        entry.found.emplace_back(
            "Le Python de MFA, son exécutable mfa et scripts/mfa_align.py "
            "sont trouvés ; Python n'a pas été lancé.");
        return entry;
    }
    struct Part {
        const char* role;
        const char* what;
        const char* variable;
    };
    constexpr Part kParts[] = {
        {"python", "le Python de MFA", kMfaPythonVariable},
        {"aligner", "l'exécutable mfa", kMfaAlignerVariable},
        {"script", "scripts/mfa_align.py", kMfaScriptVariable},
    };
    std::vector<std::string> missing;
    std::vector<std::string> variables;
    for (const Part& part : kParts) {
        bool lacking = !item.missing.IsArray();
        if (item.missing.IsArray()) {
            for (const Value& role : item.missing.AsArray()) {
                lacking = lacking ||
                          (role.IsString() && role.AsString() == part.role);
            }
        }
        if (lacking) {
            missing.emplace_back(part.what);
            variables.emplace_back(part.variable);
        }
    }
    entry.found.push_back("Introuvable : " + Enumeration(missing) + ".");
    entry.prevents =
        "Sans MFA, les frontières de mots ne sont pas alignées : " +
        ToolsRefuse(item.tools) +
        ", et les coupes restent sur les frontières de Whisper.";
    entry.repair = "régler " + Enumeration(variables) + ", " +
                   WhereSettingsGo(facts) +
                   ". Sans réglage, un checkout trouve ces fichiers à côté de "
                   "l'exécutable (build/mfa/env, scripts/), et les modèles "
                   "dans build/mfa/models. Rien n'est téléchargé ni "
                   "installé.";
    return entry;
}

Entry AgentKeyEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    const std::string variable = setup::kAgentKeyVariable;
    // The key's value is in neither the report nor the facts: only whether
    // it is set, and where.
    const bool configured = item.state == setup::State::kOk;
    if (facts.agent_key_saved) {
        entry.found.push_back(std::string("Une clé est enregistrée dans le ") +
                              "Trousseau (" + kEngineMenu + ").");
    }
    if (configured) {
        entry.found.push_back(variable + " est réglée " +
                              ReadFrom(TextOf(item.found, "origin"), facts) +
                              " ; sa valeur n'est jamais affichée.");
    }
    if (facts.agent_key_saved || configured) {
        entry.state = setup::State::kOk;
        return entry;
    }
    entry.found.push_back("Aucune clé, ni dans le Trousseau ni dans " +
                          variable + ".");
    entry.prevents =
        "Sans clé, le panneau Agent ne peut pas appeler Anthropic ; un agent "
        "qui pilote le moteur par MCP apporte la sienne.";
    entry.repair = std::string("enregistrer une clé par ") + kEngineMenu +
                   " (elle est gardée dans le Trousseau), ou régler " +
                   variable + " " + WhereSettingsGo(facts) + ".";
    return entry;
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The folder comes from the
// report's structured `found`, the sentences are the window's.
Entry ArchiveBackupEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    const std::string directory = TextOf(item.found, "directory");
    const std::string where =
        facts.settings_file.empty()
            ? std::string("dans l'environnement")
            : "dans l'environnement ou dans " + facts.settings_file;
    if (item.state == setup::State::kOk) {
        entry.found.emplace_back(
            "Les sauvegardes copient l'archive des projets une fois par jour "
            "dans " +
            directory + ", et gardent les sept copies les plus récentes.");
        return entry;
    }
    if (item.state == setup::State::kOptionalAbsent) {
        entry.found.emplace_back(
            "CUTMACHINE_ARCHIVE_BACKUP_DIR n'est pas réglé : l'archive des "
            "projets n'est copiée nulle part.");
    } else {
        entry.found.emplace_back("Le dossier " + directory +
                                 " n'existe pas ou n'est pas un chemin "
                                 "absolu : aucune copie n'est faite.");
    }
    entry.prevents =
        "Sans copie, l'archive des projets ne vit que sur le disque de cette "
        "machine.";
    entry.repair =
        "régler CUTMACHINE_ARCHIVE_BACKUP_DIR sur un dossier "
        "absolu existant, synchronisé ailleurs (OneDrive), " +
        where + ".";
    return entry;
}

// AL4 -- the s075 weights for French verbatim, from the report's `found`:
// their path and whether their SHA-256 was verified.
Entry HesitationsModelEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    const std::string path = TextOf(item.found, "path");
    const std::string where =
        facts.settings_file.empty()
            ? std::string("dans l'environnement")
            : "dans l'environnement ou dans " + facts.settings_file;
    if (item.state == setup::State::kOk) {
        const mcp_json::Value* evaluated = item.found.Find("evaluated");
        entry.found.emplace_back(
            "Le français verbatim se transcrit avec " + path +
            (evaluated != nullptr && evaluated->IsBool()
                 ? ", dont l'empreinte est vérifiée."
                 : " ; son empreinte n'a pas encore été vérifiée."));
        return entry;
    }
    if (item.state == setup::State::kOptionalAbsent) {
        entry.found.emplace_back(
            "CUTMACHINE_WHISPER_HESITATIONS_MODEL n'est pas réglé.");
    } else {
        entry.found.emplace_back(path +
                                 " n'est pas un fichier lisible des poids "
                                 "s075 évalués.");
    }
    entry.prevents =
        "Sans eux, le français verbatim se transcrit avec le modèle Whisper "
        "et le profil default, sans fr-hesitations-v1.";
    entry.repair =
        "régler CUTMACHINE_WHISPER_HESITATIONS_MODEL sur les poids s075, " +
        where + ".";
    return entry;
}

// GN6a -- the local voice-over model, from the report's structured
// `missing`: the roles found nowhere ("python", "script", "models") and the
// model folders the models folder lacks.
Entry VoiceOverEntry(const setup::Item& item, const WindowFacts& facts) {
    Entry entry = BaseEntry(item);
    if (item.state == setup::State::kOk) {
        entry.found.emplace_back(
            "Le Python de la voix off, scripts/voice_over.py et ses deux "
            "modèles (VoiceDesign et Base) sont trouvés ; Python n'a pas été "
            "lancé.");
        return entry;
    }
    struct Part {
        const char* role;
        const char* what;
        const char* variable;
    };
    constexpr Part kParts[] = {
        {"python", "le Python de la voix off", kVoicePythonVariable},
        {"script", "scripts/voice_over.py", kVoiceScriptVariable},
        {"models", "le dossier des modèles", kVoiceModelsVariable},
    };
    std::vector<std::string> missing;
    std::vector<std::string> models;
    std::vector<std::string> variables;
    if (item.missing.IsArray()) {
        for (const Value& value : item.missing.AsArray()) {
            if (!value.IsString()) {
                continue;
            }
            bool role = false;
            for (const Part& part : kParts) {
                if (value.AsString() == part.role) {
                    missing.emplace_back(part.what);
                    variables.emplace_back(part.variable);
                    role = true;
                }
            }
            if (!role) {
                models.push_back(value.AsString());
            }
        }
    }
    if (item.state == setup::State::kOptionalAbsent) {
        entry.found.emplace_back(
            "La voix off locale n'est pas installée sur cette machine.");
    } else {
        if (!missing.empty()) {
            entry.found.push_back("Introuvable : " + Enumeration(missing) +
                                  ".");
        }
        if (!models.empty()) {
            entry.found.push_back(
                "Modèle incomplet ou absent : " + Enumeration(models) + ".");
            variables.emplace_back(kVoiceModelsVariable);
        }
    }
    if (variables.empty()) {
        variables = {kVoicePythonVariable, kVoiceScriptVariable,
                     kVoiceModelsVariable};
    }
    entry.prevents =
        "Sans elle, aucune voix off ne se génère sur cette machine "
        "(Qwen3-TTS, en local).";
    entry.repair =
        "installer un Python avec qwen-tts 0.1.1 dans "
        "build/voice/env et les dossiers " +
        std::string(kVoiceDesignModel) + " et " + kVoiceCloneModel +
        " dans build/voice/models, ou régler " + Enumeration(variables) + ", " +
        WhereSettingsGo(facts) + ". Rien n'est téléchargé ni installé.";
    return entry;
}

}  // namespace

const char* StateLabel(setup::State state) {
    switch (state) {
        case setup::State::kOk:
            return "prêt";
        case setup::State::kMissing:
            return "absent";
        case setup::State::kIncomplete:
            return "incomplet";
        case setup::State::kOptionalAbsent:
            return "facultatif, absent";
    }
    return "absent";
}

std::string ItemTitle(const std::string& id) {
    if (id == ffmpeg::kDiagnosisKey) {
        return "FFmpeg";
    }
    if (id == setup::kWhisperModelItem) {
        return "Modèle Whisper";
    }
    if (id == setup::kWhisperHesitationsModelItem) {
        return "Poids des hésitations";
    }
    if (id == setup::kMfaItem) {
        return "MFA";
    }
    if (id == setup::kAgentKeyItem) {
        return "Clé de l'agent";
    }
    if (id == setup::kArchiveBackupItem) {
        return "Copie de l'archive";
    }
    if (id == setup::kVoiceOverItem) {
        return "Voix off";
    }
    return id;
}

std::vector<Entry> Entries(const setup::Report& report,
                           const WindowFacts& facts) {
    std::vector<Entry> entries;
    entries.reserve(report.items.size());
    for (const setup::Item& item : report.items) {
        if (item.id == ffmpeg::kDiagnosisKey) {
            entries.push_back(FfmpegEntry(item, facts));
        } else if (item.id == setup::kWhisperModelItem) {
            entries.push_back(WhisperEntry(item, facts));
        } else if (item.id == setup::kWhisperHesitationsModelItem) {
            entries.push_back(HesitationsModelEntry(item, facts));
        } else if (item.id == setup::kMfaItem) {
            entries.push_back(MfaEntry(item, facts));
        } else if (item.id == setup::kAgentKeyItem) {
            entries.push_back(AgentKeyEntry(item, facts));
        } else if (item.id == setup::kArchiveBackupItem) {
            entries.push_back(ArchiveBackupEntry(item, facts));
        } else if (item.id == setup::kVoiceOverItem) {
            entries.push_back(VoiceOverEntry(item, facts));
        } else {
            // An item this version cannot word: its name and state, and the
            // engine's sentences in the copied report.
            entries.push_back(BaseEntry(item));
        }
    }
    return entries;
}

std::string WelcomeLine(const setup::Report& report) {
    const setup::Item* missing = report.MissingRequired();
    if (missing == nullptr) {
        return std::string();
    }
    if (missing->id == ffmpeg::kDiagnosisKey) {
        return "FFmpeg introuvable : les vignettes, les formes d'onde et "
               "l'export vidéo ne fonctionneront pas.";
    }
    // Required means needed to edit and export (SetupCheck.h).
    return ItemTitle(missing->id) +
           " introuvable : CUTMACHINE ne peut ni monter ni exporter.";
}

std::string Summary(const setup::Report& report) {
    const std::string line = WelcomeLine(report);
    return line.empty()
               ? "Rien de requis ne manque : CUTMACHINE peut monter et "
                 "exporter."
               : line;
}

std::vector<Run> DiagnosticRuns(const setup::Report& report,
                                const WindowFacts& facts) {
    std::vector<Run> runs;
    std::string head;
    if (!facts.build.empty()) {
        head += "CUTMACHINE " + facts.build + "\n";
    }
    head += Summary(report) + "\n";
    runs.push_back({head, false});
    bool repairs = false;
    for (const Entry& entry : Entries(report, facts)) {
        runs.push_back({"\n", false});
        runs.push_back(
            {entry.title + " : " + StateLabel(entry.state) + "\n", true});
        std::string body;
        for (const std::string& line : entry.found) {
            body += line + "\n";
        }
        if (!entry.prevents.empty()) {
            body += entry.prevents + "\n";
        }
        if (!entry.repair.empty()) {
            body += "À faire : " + entry.repair + "\n";
            repairs = true;
        }
        if (!body.empty()) {
            runs.push_back({body, false});
        }
    }
    // The engine reads the settings file once a value is needed, and never
    // again for a value it already has (LocalEnv.h): an edit is seen by the
    // next launch.
    if (repairs && !facts.settings_file.empty()) {
        runs.push_back({"\nAprès une modification de " + facts.settings_file +
                            ", relancer CUTMACHINE : une valeur déjà lue "
                            "n'est pas relue.\n",
                        false});
    }
    return runs;
}

std::string DiagnosticText(const setup::Report& report,
                           const WindowFacts& facts) {
    std::string text;
    for (const Run& run : DiagnosticRuns(report, facts)) {
        text += run.text;
    }
    return text;
}

std::string ClipboardText(const setup::Report& report,
                          const WindowFacts& facts) {
    return DiagnosticText(report, facts) +
           "\nRapport du moteur (cutmachine --check-setup) :\n" +
           setup::ReportJson(report).Dump() + "\n";
}

}  // namespace setup_notice
