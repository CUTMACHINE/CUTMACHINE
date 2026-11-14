#include "PauseTightening.h"

#include "EditLog.h"
#include "Json.h"
#include "Operations.h"

#include <iostream>
#include <string>
#include <vector>

// QC-2026-09 A2. Le cœur est pur : une enveloppe synthétique, aucun FFmpeg,
// aucun modèle. Ce qui est épinglé ici, c'est ce que le ticket promet — les
// creux internes se referment, les bords ne bougent pas, l'arrondi ne mord
// jamais sur la parole — et la réversibilité, puisque la fonction produit une
// opération et rien d'autre.

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr int64_t kRoom = 20000;
constexpr int64_t kVoice = 200000;
const char kSourceId[] = "01K30000000000000000000001";

// Enveloppe à 50 fenêtres/s décrite par une suite (niveau, nombre de
// fenêtres), pour que chaque cas se lise comme la forme d'onde qu'il
// représente.
SpeechOnsetReport Envelope(
    const std::vector<std::pair<int64_t, int64_t>>& blocks) {
    SpeechOnsetReport report;
    report.media_id = kSourceId;
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    for (const auto& block : blocks)
        for (int64_t index = 0; index < block.second; ++index)
            report.levels.push_back(block.first);
    report.speech_level = SpeechLevelPercentile(report.levels, 90);
    report.noise_floor = SpeechLevelPercentile(report.levels, 5);
    return report;
}

// IA8 -- des mots placés comme les place un aligneur forcé : bornes en images
// de la source, dans l'ordre, sans chevauchement.
Transcript AlignedWords(const std::vector<TranscriptWord>& words,
                        const std::string& timing = "nemo") {
    Transcript transcript;
    transcript.media_id = kSourceId;
    transcript.source_rate = {25, 1};
    transcript.word_timing = timing;
    transcript.words = words;
    return transcript;
}

bool Overlaps(const WordRemovalRange& range, const TranscriptWord& word) {
    return range.source_start < word.end && word.start < range.source_end;
}

DocumentClip Clip(RationalTime sourceIn, RationalTime duration) {
    DocumentClip clip;
    clip.id = "01K30000000000000000000020";
    clip.source_id = kSourceId;
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = RationalTime{0, 25};
    return clip;
}

}  // namespace

int main() {
    const MediaRate kPal{25, 1};
    PauseTighteningSettings defaults;
    std::string error;
    Check(defaults.minimum_gap_milliseconds == 320 && defaults.keep_frames == 4,
          "les défauts sont 320 ms et 4 images, le réglage de la procédure "
          "d'interview");

    // --- Un creux interne se referme ---------------------------------------
    // 1 s de parole, 1 s de silence, 1 s de parole. À 25 i/s le creux fait 25
    // images ; on en garde 4 (le défaut), réparties 2/2, donc 21 images
    // partent.
    {
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, nullptr,
                                     defaults, kPal, {}, operation, report,
                                     error),
              "un plan à un creux se résout : " + error);
        Check(operation.ranges.size() == 1,
              "un seul creux donne une seule plage");
        Check(operation.clip_id == "01K30000000000000000000020",
              "l'opération vise bien le plan demandé");
        if (operation.ranges.size() == 1) {
            Check(operation.ranges[0].source_start == RationalTime{27, 25},
                  "la coupe démarre 2 images après le début du creux");
            Check(operation.ranges[0].source_end == RationalTime{48, 25},
                  "et s'arrête 2 images avant sa fin");
        }
        Check(report.pauses.size() == 1 &&
                  report.removed_total == RationalTime{21, 25},
              "le rapport annonce les 21 images retirées");
        Check(report.head_air == RationalTime{0, 50} &&
                  report.tail_air == RationalTime{0, 50},
              "un plan qui entre et sort sur la parole n'a pas d'air aux "
              "bords");
    }

    // --- Les bords ne sont pas la question de cet outil ---------------------
    {
        const SpeechOnsetReport envelope = Envelope({{kRoom, 40},
                                                     {kVoice, 50},
                                                     {kRoom, 50},
                                                     {kVoice, 50},
                                                     {kRoom, 30}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(Clip({0, 25}, {110, 25}), envelope,
                                     nullptr, defaults, kPal, {}, operation,
                                     report, error),
              "un plan bordé de silence se résout : " + error);
        Check(operation.ranges.size() == 1,
              "seul le creux interne est coupé : l'air de tête et de queue "
              "relève d'un rognage, pas d'un ripple interne");
        Check(report.head_air == RationalTime{40, 50} &&
                  report.tail_air == RationalTime{30, 50},
              "l'air laissé aux bords est mesuré et publié");
        if (operation.ranges.size() == 1) {
            Check(operation.ranges[0].source_start == RationalTime{47, 25},
                  "la coupe interne démarre après les 2 images gardées");
        }
    }

    // --- Un souffle n'est pas une hésitation --------------------------------
    {
        // 200 ms de creux : sous les 320 ms par défaut.
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 10}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(Clip({0, 25}, {55, 25}), envelope, nullptr,
                                     defaults, kPal, {}, operation, report,
                                     error),
              "un plan sans hésitation se résout : " + error);
        Check(operation.ranges.empty() && report.skipped_short == 1,
              "un creux plus court que le seuil est compté, pas coupé");

        // 6 images gardées (240 ms) : plus que le creux de 200 ms.
        PauseTighteningSettings aggressive = defaults;
        aggressive.minimum_gap_milliseconds = 150;
        aggressive.keep_frames = 6;
        RemoveWordsOperation cut;
        PauseTighteningReport second;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {55, 25}), envelope, nullptr,
                                   aggressive, kPal, {}, cut, second, error) &&
                cut.ranges.empty() && second.skipped_already_tight == 1,
            "abaisser le seuil ne coupe pas un creux déjà plus court que ce "
            "qu'on garde : " +
                error);
    }

    // --- L'arrondi ne mord jamais sur la parole -----------------------------
    {
        // Le creux commence à la fenêtre 51 et finit à la 99 : ni l'une ni
        // l'autre ne tombe sur une image de 25 i/s (2 fenêtres = 1 image).
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 51}, {kRoom, 48}, {kVoice, 50}});
        PauseTighteningSettings noKeep = defaults;
        noKeep.keep_frames = 0;
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, nullptr,
                                     noKeep, kPal, {}, operation, report,
                                     error) &&
                  operation.ranges.size() == 1,
              "un creux mal aligné se résout quand même : " + error);
        if (operation.ranges.size() == 1) {
            // 51/50 s arrondi vers le haut donne l'image 26 ; 99/50 s arrondi
            // vers le bas donne l'image 49. Les deux tombent à l'intérieur du
            // silence, jamais sur la parole qui l'encadre.
            Check(operation.ranges[0].source_start == RationalTime{26, 25},
                  "le début de coupe s'arrondit vers l'intérieur du silence");
            Check(operation.ranges[0].source_end == RationalTime{49, 25},
                  "la fin de coupe aussi");
        }
    }

    // --- Un mot bref entre deux silences n'est pas avalé (IA8) ---------------
    // Le cas relevé au bench : 200 ms de silence, un « à » d'une image resté
    // sous la ligne de parole, 200 ms de silence. L'enveloppe n'y voit qu'un
    // creux de 440 ms, au-dessus des 320 ms du skill.
    {
        const SpeechOnsetReport envelope = Envelope(
            {{kVoice, 50}, {kRoom, 10}, {kRoom, 2}, {kRoom, 10}, {kVoice, 50}});
        PauseTighteningSettings skill = defaults;
        skill.minimum_gap_milliseconds = 320;
        skill.keep_frames = 4;
        const TranscriptWord word{"à", {30, 25}, {31, 25}};
        const Transcript aligned = AlignedWords(
            {{"Et", {10, 25}, {24, 25}}, word, {"demain", {37, 25}, {60, 25}}});

        RemoveWordsOperation blind;
        PauseTighteningReport blindReport;
        Check(ResolvePauseTightening(Clip({0, 25}, {61, 25}), envelope, nullptr,
                                     skill, kPal, {}, blind, blindReport,
                                     error) &&
                  blind.ranges.size() == 1 && Overlaps(blind.ranges[0], word),
              "sans mots, le creux est lu d'un bloc et la coupe prend le mot "
              "(le défaut que le rapport doit au moins avouer)");
        Check(!blindReport.words_checked &&
                  blindReport.words_unchecked_reason.find("no transcript") !=
                      std::string::npos,
              "sans transcript, le rapport dit que les mots n'ont pas été "
              "vérifiés");

        RemoveWordsOperation guarded;
        PauseTighteningReport report;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {61, 25}), envelope, &aligned,
                                   skill, kPal, {}, guarded, report, error),
            "avec des mots alignés, le plan se résout : " + error);
        bool overlap = false;
        for (const WordRemovalRange& range : guarded.ranges)
            overlap = overlap || Overlaps(range, word);
        Check(!overlap, "aucun intervalle retiré ne recouvre le mot");
        Check(guarded.ranges.empty() && report.skipped_short == 2,
              "de part et d'autre du mot, deux silences sous le seuil restent "
              "en place");
        Check(report.words_checked && report.word_timing == "nemo" &&
                  report.hollows_holding_words == 1 &&
                  report.kept_words.size() == 1 &&
                  report.kept_words[0].text == "à",
              "le rapport nomme le mot gardé et l'aligneur qui l'a placé");

        // AGENT-2026-09 A17 -- un mot que l'aligneur a posé dans le fond de
        // salle n'a pas été dit : il est signalé « non entendu », gardé par
        // défaut, et le creux se ferme par-dessus avec close_unheard. Un mot
        // bas mais au-dessus du plancher reste entendu et protégé.
        {
            const int64_t kQuiet = kRoom * 3;
            const SpeechOnsetReport unheardEnvelope = Envelope({{kVoice, 50},
                                                                {kRoom, 30},
                                                                {kVoice, 50},
                                                                {kQuiet, 2},
                                                                {kVoice, 50}});
            const Transcript unheardWords =
                AlignedWords({{"Alors", {10, 25}, {24, 25}},
                              {"Et", {30, 25}, {33, 25}},
                              {"demain", {40, 25}, {65, 25}},
                              {"à", {65, 25}, {66, 25}},
                              {"plus", {66, 25}, {90, 25}}});
            RemoveWordsOperation guardedStill;
            PauseTighteningReport unheardReport;
            Check(ResolvePauseTightening(
                      Clip({0, 25}, {91, 25}), unheardEnvelope, &unheardWords,
                      skill, kPal, {}, guardedStill, unheardReport, error) &&
                      guardedStill.ranges.empty() &&
                      unheardReport.kept_words.size() == 1 &&
                      unheardReport.kept_words[0].text == "Et" &&
                      !unheardReport.kept_words[0].heard,
                  "par défaut le mot posé dans le fond de salle garde le "
                  "creux ouvert, mais le rapport le dit non entendu");
            PauseTighteningSettings closing = skill;
            closing.close_unheard = true;
            RemoveWordsOperation closed;
            PauseTighteningReport closedReport;
            Check(ResolvePauseTightening(
                      Clip({0, 25}, {91, 25}), unheardEnvelope, &unheardWords,
                      closing, kPal, {}, closed, closedReport, error) &&
                      closed.ranges.size() == 1 &&
                      closedReport.closed_over_words.size() == 1 &&
                      closedReport.closed_over_words[0].text == "Et" &&
                      closedReport.kept_words.empty(),
                  "avec close_unheard, le creux se ferme par-dessus le mot non "
                  "entendu et le rapport le nomme");
            Check(
                std::string(closedReport.closed_over_words.empty()
                                ? ""
                                : SerializePauseTighteningReport(closedReport))
                        .find("\"closed_over_words\"") != std::string::npos,
                "le rapport sérialisé porte les mots recouverts");
            // Le plancher et la crête de chaque mot sont publiés, et la
            // marge est celle de l'appelant : à 3 dB le mot au plancher est
            // le seul non entendu ; à 12 dB un mot bas y passerait aussi.
            Check(unheardReport.floor_dbfs ==
                          SpeechLevelDbfs(unheardEnvelope.noise_floor) &&
                      unheardReport.kept_words[0].peak_dbfs ==
                          SpeechLevelDbfs(kRoom),
                  "le rapport donne le plancher du rush et la crête du mot");
            PauseTighteningSettings wide = skill;
            wide.unheard_margin_db = 31;
            Check(!ResolvePauseTightening(
                      Clip({0, 25}, {91, 25}), unheardEnvelope, &unheardWords,
                      wide, kPal, {}, closed, closedReport, error),
                  "une marge hors bornes est refusée");
        }

        // AGENT-2026-09 A30 -- un mot entendu dont l'alignement déborde
        // dans le creux (la fin NeMo court dans le silence qu'elle précède)
        // ne protège que ses fenêtres au-dessus du plancher : le creux se
        // ferme, et le rapport nomme le mot recouvert avec sa raison.
        {
            const SpeechOnsetReport overflowEnvelope =
                Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
            const Transcript overflowWords = AlignedWords(
                {{"Rivage", {10, 25}, {30, 25}}, {"donc", {55, 25}, {70, 25}}});
            RemoveWordsOperation closedOver;
            PauseTighteningReport overflowReport;
            Check(ResolvePauseTightening(
                      Clip({0, 25}, {75, 25}), overflowEnvelope, &overflowWords,
                      skill, kPal, {}, closedOver, overflowReport, error) &&
                      closedOver.ranges.size() == 1 &&
                      overflowReport.closed_over_words.size() == 1 &&
                      overflowReport.closed_over_words[0].text == "Rivage" &&
                      overflowReport.closed_over_words[0].heard &&
                      overflowReport.kept_words.empty() &&
                      overflowReport.hollows_holding_words == 0,
                  "un alignement qui déborde au plancher ne tient pas le "
                  "creux ouvert : " +
                      error);
            Check(SerializePauseTighteningReport(overflowReport)
                          .find("aligned_past_the_voice") != std::string::npos,
                  "le rapport dit pourquoi le mot a été recouvert");
            // La part audible d'un tel mot, elle, reste protégée : cinq
            // fenêtres sous la ligne de parole mais au-dessus du plancher,
            // avant le silence.
            const int64_t kQuiet = kRoom * 2;
            const SpeechOnsetReport partEnvelope = Envelope(
                {{kVoice, 50}, {kQuiet, 5}, {kRoom, 45}, {kVoice, 50}});
            RemoveWordsOperation partly;
            PauseTighteningReport partReport;
            Check(ResolvePauseTightening(Clip({0, 25}, {75, 25}), partEnvelope,
                                         &overflowWords, skill, kPal, {},
                                         partly, partReport, error) &&
                      partly.ranges.size() == 1 &&
                      partReport.kept_words.size() == 1 &&
                      partReport.kept_words[0].protected_end ==
                          RationalTime{55, 50} &&
                      partly.ranges[0].source_start.compare(
                          RationalTime{55, 50}) >= 0,
                  "les fenêtres audibles du mot restent hors de la coupe : " +
                      error);
        }

        const Transcript whisper = AlignedWords(
            {{"Et", {10, 25}, {24, 25}}, word, {"demain", {37, 25}, {60, 25}}},
            "whisper");
        RemoveWordsOperation whisperCut;
        PauseTighteningReport whisperReport;
        Check(ResolvePauseTightening(Clip({0, 25}, {61, 25}), envelope,
                                     &whisper, skill, kPal, {}, whisperCut,
                                     whisperReport, error) &&
                  whisperCut.ranges.size() == 1 &&
                  !whisperReport.words_checked &&
                  whisperReport.words_unchecked_reason.find("Whisper") !=
                      std::string::npos,
              "des bornes Whisper ne protègent rien, et le rapport le dit");

        Transcript foreign = aligned;
        foreign.media_id = "01K30000000000000000000009";
        Check(
            !ResolvePauseTightening(Clip({0, 25}, {61, 25}), envelope, &foreign,
                                    skill, kPal, {}, guarded, report, error) &&
                error.find("another source") != std::string::npos,
            "des mots d'une autre source sont refusés");
    }

    // --- Une pause longue d'un seul côté du mot se referme quand même -------
    {
        // 800 ms de silence, le mot d'une image, 200 ms de silence.
        const SpeechOnsetReport envelope = Envelope(
            {{kVoice, 50}, {kRoom, 40}, {kRoom, 2}, {kRoom, 10}, {kVoice, 50}});
        PauseTighteningSettings skill = defaults;
        skill.minimum_gap_milliseconds = 320;
        skill.keep_frames = 4;
        const TranscriptWord word{"et", {45, 25}, {46, 25}};
        const Transcript aligned =
            AlignedWords({{"alors", {5, 25}, {24, 25}},
                          word,
                          {"ensuite", {52, 25}, {70, 25}}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(
            ResolvePauseTightening(Clip({0, 25}, {76, 25}), envelope, &aligned,
                                   skill, kPal, {}, operation, report, error),
            "le plan se résout : " + error);
        Check(operation.ranges.size() == 1 &&
                  !Overlaps(operation.ranges[0], word),
              "seul le long silence se referme, sans toucher le mot");
        if (operation.ranges.size() == 1) {
            // Silence de l'image 25 à l'image 45 ; 2 images gardées de chaque
            // côté.
            Check(operation.ranges[0].source_start == RationalTime{27, 25} &&
                      operation.ranges[0].source_end == RationalTime{43, 25},
                  "la coupe garde son air contre le mot comme contre la "
                  "parole");
        }
        Check(report.pauses.size() == 1 && report.skipped_short == 1 &&
                  report.pauses[0].gap_end == RationalTime{45, 25},
              "le rapport publie la pause refermée, bornée par le mot, et "
              "compte le silence court laissé");
    }

    // --- Un creux à l'intérieur d'un mot reste en place ----------------------
    {
        // Une occlusive laisse 400 ms de creux dans l'enveloppe, à l'intérieur
        // d'un mot que l'aligneur a placé d'un bord à l'autre.
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 20}, {kVoice, 50}});
        const Transcript aligned =
            AlignedWords({{"notre", {20, 25}, {40, 25}}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(Clip({0, 25}, {60, 25}), envelope,
                                     &aligned, defaults, kPal, {}, operation,
                                     report, error) &&
                  operation.ranges.empty() && report.hollows_holding_words == 1,
              "un creux couvert par un mot n'est pas une pause");
    }

    // --- Refus plutôt qu'invention -----------------------------------------
    {
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        DocumentClip foreign = Clip({0, 25}, {75, 25});
        foreign.source_id = "01K30000000000000000000009";
        Check(!ResolvePauseTightening(foreign, envelope, nullptr, defaults,
                                      kPal, {}, operation, report, error) &&
                  error.find("another source") != std::string::npos,
              "une enveloppe d'une autre source est refusée : " + error);

        SpeechOnsetReport empty;
        empty.media_id = kSourceId;
        Check(!ResolvePauseTightening(Clip({0, 25}, {75, 25}), empty, nullptr,
                                      defaults, kPal, {}, operation, report,
                                      error),
              "une enveloppe sans grille d'analyse est refusée");

        PauseTighteningSettings negative = defaults;
        negative.keep_frames = -1;
        Check(!ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope,
                                      nullptr, negative, kPal, {}, operation,
                                      report, error),
              "garder un nombre négatif d'images est refusé");

        Check(!ResolvePauseTightening(Clip({5000, 25}, {75, 25}), envelope,
                                      nullptr, defaults, kPal, {}, operation,
                                      report, error),
              "un plan hors de l'enveloppe est refusé, pas déclaré serré");
    }

    // --- Réversibilité ------------------------------------------------------
    // La promesse du ticket : « le tout en une opération réversible ». On la
    // vérifie comme AGENTS.md l'exige, octet à octet après annulation, et sur
    // la paire A/V liée, qui est ce que Q4b a rendu possible.
    {
        Document document;
        document.sources = {{kSourceId, "rush.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture = Clip({0, 25}, {75, 25});
        picture.link_group_id = "01K30000000000000000000005";
        DocumentClip sound = picture;
        sound.id = "01K30000000000000000000021";
        document.sequence.tracks = {
            {"01K30000000000000000000002", "video", 0, {picture}},
            {"01K30000000000000000000003", "audio", 1, {sound}},
        };
        const std::string before = document.SaveToString();

        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        Check(ResolvePauseTightening(picture, envelope, nullptr, defaults, kPal,
                                     {}, operation, report, error),
              "le plan image se résout : " + error);
        operation.linked_clip_ids =
            LinkedClipIdsCoveringRanges(document, picture, operation.ranges);
        Check(operation.linked_clip_ids.size() == 1 &&
                  operation.linked_clip_ids[0] == "01K30000000000000000000021",
              "le son lié est emporté par la même coupe");

        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(log.Apply(document, operation, editError, message),
              "l'opération s'applique : " + message);
        Check(document.SaveToString() != before, "le montage a bien changé");
        const DocumentTrack* video =
            document.FindTrack("01K30000000000000000000002");
        const DocumentTrack* audio =
            document.FindTrack("01K30000000000000000000003");
        Check(video != nullptr && audio != nullptr &&
                  video->clips.size() == audio->clips.size(),
              "image et son sont découpés en autant de fragments");
        Check(log.Undo(document, editError, message),
              "l'opération s'annule : " + message);
        Check(document.SaveToString() == before,
              "l'annulation rend un document octet pour octet identique");
        Check(log.Redo(document, editError, message),
              "et se rétablit : " + message);
    }

    // --- Sérialisation du rapport ------------------------------------------
    {
        const SpeechOnsetReport envelope =
            Envelope({{kVoice, 50}, {kRoom, 50}, {kVoice, 50}});
        RemoveWordsOperation operation;
        PauseTighteningReport report;
        ResolvePauseTightening(Clip({0, 25}, {75, 25}), envelope, nullptr,
                               defaults, kPal, {}, operation, report, error);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(SerializePauseTighteningReport(report),
                                     parsed, parseError) &&
                  parsed.IsObject(),
              "le rapport est du JSON valide : " + parseError);
        const mcp_json::Value* closed = parsed.Find("closed");
        Check(closed != nullptr && closed->IsArray() &&
                  closed->AsArray().size() == 1,
              "il publie le creux refermé");
        const mcp_json::Value* checked = parsed.Find("words_checked");
        const mcp_json::Value* reason = parsed.Find("words_unchecked_reason");
        Check(checked != nullptr && checked->IsBool() && !checked->AsBool() &&
                  reason != nullptr && reason->IsString(),
              "il dit que les mots n'ont pas été vérifiés, et pourquoi");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "Tous les tests de resserrement des silences passent\n";
    return 0;
}
