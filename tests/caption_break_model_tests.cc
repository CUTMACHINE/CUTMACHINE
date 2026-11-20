// RM3 -- docs/roadmap/REGLES-2026-10.md. The learned carton boundary.

#include "CaptionBreakModel.h"
#include "Subtitles.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

#include "TestSupport.h"

namespace {

std::vector<SubtitleWord> Spoken() {
    // Words in milliseconds, with a real pause after "puits,".
    const std::vector<std::tuple<const char*, int64_t, int64_t>> spoken = {
        {"Sur", 0, 200},         {"la", 200, 320},
        {"colline,", 320, 800},  {"les", 1100, 1200},
        {"chèvres", 1200, 1260}, {"broutent", 1260, 1800},
        {"près", 1800, 1900},    {"du", 1900, 2200},
        {"grand", 2200, 2300},   {"puits,", 2300, 2700},
        {"mais", 3200, 3300},    {"le", 3300, 3400},
        {"chien", 3400, 3700},   {"dort", 3700, 3800},
        {"encore", 3800, 4000},  {"dehors.", 4000, 4700}};
    std::vector<SubtitleWord> words;
    for (const auto& [text, start, end] : spoken)
        words.push_back({text, {start, 1000}, {end, 1000}});
    return words;
}

std::vector<std::pair<size_t, size_t>> Ranges(
    const std::vector<SubtitleWord>& words,
    const SubtitleSegmentationLimits& limits) {
    std::vector<SubtitleCue> cues;
    std::vector<std::pair<size_t, size_t>> ranges;
    std::string error;
    Check(SegmentSubtitleWords(words, limits, cues, &ranges, error),
          "segmentation succeeds: " + error);
    return ranges;
}

// One delivered track per group: every sentence cut after "de", where the
// rule's +400 forbids it -- or, `beforePreposition`, before it, as the rule
// prefers.
std::string AfterPrepositionCorpus(bool beforePreposition = false,
                                   const std::string& prefix = "g") {
    const std::vector<std::string> subjects = {
        "Marie", "Paul",  "Inès", "Hugo",  "Léa", "Tom",
        "Nina",  "Sacha", "Jade", "Malik", "Luc", "Axel"};
    const std::vector<std::string> verbs = {"finir", "tourner", "monter",
                                            "écrire"};
    const std::vector<std::string> objects = {"plan.", "court.", "clip.",
                                              "récit."};
    // Several nouns before "de", so that what is learned is the
    // preposition, not one word in front of it.
    const std::vector<std::string> nouns = {"l'envie", "le besoin", "l'élan",
                                            "le temps", "la chance"};
    std::string corpus;
    for (size_t group = 0; group < subjects.size(); ++group) {
        corpus += "{\"group\": \"" + prefix + std::to_string(group) +
                  "\", \"frame_rate\": {\"num\": 25, \"den\": 1}, "
                  "\"cards\": [";
        int64_t frame = 100;
        for (size_t sentence = 0; sentence < 6; ++sentence) {
            const std::string& subject =
                subjects[(group + sentence) % subjects.size()];
            const std::string& noun = nouns[(group + (2 * sentence)) % 5];
            const std::string& verb = verbs[sentence % verbs.size()];
            const std::string& object = objects[(group + sentence) % 4];
            const std::string first = beforePreposition
                                          ? subject + " a " + noun
                                          : subject + " a " + noun + " de";
            const std::string second = beforePreposition
                                           ? "de " + verb + " un " + object
                                           : verb + " un " + object;
            if (sentence > 0) corpus += ", ";
            corpus += "{\"text\": \"" + first +
                      "\", \"start\": " + std::to_string(frame) +
                      ", \"duration\": 20}, ";
            corpus += "{\"text\": \"" + second +
                      "\", \"start\": " + std::to_string(frame + 20) +
                      ", \"duration\": 18}";
            frame += 48;
        }
        corpus += "]}\n";
    }
    return corpus;
}

}  // namespace

int main() {
    // A model without weights is the rule, cost for cost and cue for cue.
    {
        const std::vector<SubtitleWord> words = Spoken();
        const CaptionBreakModel empty;
        const std::vector<int64_t> costs =
            CaptionBreakBoundaryCosts(empty, words, false);
        bool same = costs.size() == words.size() - 1;
        for (size_t index = 0; same && index < costs.size(); ++index)
            same = costs[index] ==
                   SubtitleRuleBoundaryCost(
                       words[index].text, words[index + 1].text,
                       words[index + 1].start.sub(words[index].end));
        Check(same, "an empty model's boundary costs are the rule's");
        SubtitleSegmentationLimits limits;
        limits.maximum_characters = 20;
        std::vector<int64_t> held;
        const SubtitleSegmentationLimits withModel =
            WithCaptionBreakModel(empty, words, false, limits, held);
        Check(Ranges(words, limits) == Ranges(words, withModel),
              "an empty model cuts the same cartons as the rule");

        std::vector<int64_t> wrong(words.size(), 0);
        limits.boundary_costs = &wrong;
        std::vector<SubtitleCue> cues;
        std::string error;
        Check(!SegmentSubtitleWords(words, limits, cues, nullptr, error) &&
                  !error.empty(),
              "boundary costs that are not one per junction are refused");
    }

    // The families the rule reads.
    Check((SubtitleWordFamilies("En") & kSubtitlePreposition) != 0 &&
              (SubtitleWordFamilies("en") & kSubtitleClitic) != 0 &&
              (SubtitleWordFamilies("la,") & kSubtitleDeterminer) != 0 &&
              (SubtitleWordFamilies("qu'") & kSubtitleSubordinator) != 0 &&
              SubtitleWordFamilies("caméra") == 0,
          "word families follow the rule's lists, punctuation aside");

    // REGLES-2026-10, 12 October 2026: the rule respects three of the author's
    // six choices (article, auxiliary, conjunction). The pronoun is a tie.
    {
        const CaptionBreakModel rule;
        std::vector<std::string> respected;
        for (const CaptionBreakDecision& decision : CaptionBreakDecisions()) {
            std::vector<SubtitleWord> words;
            for (size_t index = 0; index < decision.words.size(); ++index)
                words.push_back({decision.words[index],
                                 {static_cast<int64_t>(index) * 3, 10},
                                 {static_cast<int64_t>(index + 1) * 3, 10}});
            const std::vector<int64_t> costs =
                CaptionBreakBoundaryCosts(rule, words, true);
            if (costs[decision.preferred] < costs[decision.other])
                respected.push_back(decision.family);
        }
        Check(respected == std::vector<std::string>{"article", "auxiliary",
                                                    "conjunction"},
              "the rule respects the article, auxiliary and conjunction "
              "choices, not the preposition, pronoun and long group ones");
    }

    // The corpus: word times spread over each card, articles given to their
    // noun, unreachable cards left out and the track cut there.
    {
        const std::string corpus =
            "{\"group\": \"p\", \"frame_rate\": {\"num\": 25, \"den\": 1}, "
            "\"extra\": 1, \"cards\": ["
            "{\"text\": \" Elle plie la\", \"start\": 0, \"duration\": 25}, "
            "{\"text\": \"nappe\\nce matin,\", \"start\": 25, \"duration\": "
            "25}, "
            "{\"text\": \"il gèle. Demain\", \"start\": 50, \"duration\": "
            "25},"
            " {\"text\": \"tu sors.\", \"start\": 75, \"duration\": 25}, "
            "{\"text\": \"Et hop.\", \"start\": 100, \"duration\": 25}]}\n";
        std::vector<CaptionBreakTrack> tracks;
        CaptionBreakCorpusStats stats;
        std::string error;
        Check(ParseCaptionBreakCorpus(corpus, tracks, stats, error),
              "a corpus line parses: " + error);
        Check(stats.cards == 5 && stats.article_breaks_moved == 1 &&
                  stats.cards_left_out == 1 && tracks.size() == 2,
              "one article moved, one card with a sentence end inside left "
              "out, the track cut in two");
        if (tracks.size() == 2) {
            const CaptionBreakTrack& first = tracks[0];
            Check(first.words.size() == 6 &&
                      first.cue_ends == std::vector<size_t>{2, 6} &&
                      first.words[2].text == "la" && first.timing_estimated,
                  "« Elle plie // la nappe ce matin, »");
            // "Elle" holds 4 of the card's 12 characters, spaces included.
            Check(first.words[0].start == RationalTime{0, 25000} &&
                      first.words[0].end ==
                          RationalTime{25 * 1000 * 4 / 12, 25000},
                  "a word's time is its share of the card, in thousandths of "
                  "a frame");
            // "la nappe ce matin," in characters.
            Check(first.maximum_characters == 18,
                  "the bounds come from the delivered cards");
            Check(tracks[1].cue_ends == std::vector<size_t>{2, 4},
                  "the cards after the left-out one form their own track");
        }
        std::vector<CaptionBreakTrack> none;
        Check(!ParseCaptionBreakCorpus("{\"group\": \"p\", \"cards\": []}\n",
                                       none, stats, error) &&
                  error.find("frame_rate") != std::string::npos,
              "a line without a frame rate is refused by name");
    }

    // Words given as said: each goes to the card its middle falls in, the
    // pauses are known, a word in no card is left out.
    {
        const std::string corpus =
            "{\"group\": \"p\", \"frame_rate\": {\"num\": 25, \"den\": 1}, "
            "\"cards\": [{\"text\": \"Sur la colline,\", \"start\": 0, "
            "\"duration\": 20}, {\"text\": \"tu as vu l'orage\", "
            "\"start\": 30, \"duration\": 25}], \"words\": ["
            "{\"text\": \"Sur\", \"start\": 0, \"end\": 4}, "
            "{\"text\": \"la\", \"start\": 4, \"end\": 7}, "
            "{\"text\": \"colline,\", \"start\": 7, \"end\": 18}, "
            "{\"text\": \"euh\", \"start\": 21, \"end\": 28}, "
            "{\"text\": \"tu\", \"start\": 30, \"end\": 33}, "
            "{\"text\": \"as\", \"start\": 33, \"end\": 35}, "
            "{\"text\": \"vu\", \"start\": 35, \"end\": 38}, "
            "{\"text\": \"l'orage\", \"start\": 38, \"end\": 52}]}\n";
        std::vector<CaptionBreakTrack> tracks;
        CaptionBreakCorpusStats stats;
        std::string error;
        Check(ParseCaptionBreakCorpus(corpus, tracks, stats, error) &&
                  tracks.size() == 1 && stats.words == 7 &&
                  stats.words_left_out == 1 && stats.cards == 2,
              "words go to their cards, the one in no card is left out: " +
                  error);
        if (tracks.size() == 1) {
            Check(!tracks[0].timing_estimated &&
                      tracks[0].cue_ends == std::vector<size_t>{3, 7} &&
                      tracks[0].words[3].start == RationalTime{30, 25} &&
                      tracks[0].words[2].end == RationalTime{18, 25},
                  "the words keep their own times, pauses included");
        }
    }

    // Training: a corpus that always cuts after "de" teaches it, held out.
    {
        std::vector<CaptionBreakTrack> tracks;
        CaptionBreakCorpusStats stats;
        std::string error;
        Check(ParseCaptionBreakCorpus(AfterPrepositionCorpus(), tracks, stats,
                                      error),
              "the synthetic corpus parses: " + error);
        CaptionBreakTrainingOptions options;
        options.folds = 3;
        options.vocabulary_minimum_groups = 2;
        CaptionBreakModel model;
        CaptionBreakTrainingReport report;
        Check(TrainCaptionBreakModel(tracks, {}, options, model, report, error),
              "training succeeds: " + error);
        Check(report.groups == 12 && report.folds == 3 &&
                  report.model_held_out.reproduced_cues >
                      report.rule_held_out.reproduced_cues &&
                  report.beats_rule,
              "the model reproduces held-out groups the rule does not");
        bool preposition = false;
        for (const CaptionBreakDecisionResult& result : report.decisions)
            if (result.family == "preposition")
                preposition = result.model_preferred < result.model_other &&
                              result.rule_preferred > result.rule_other;
        Check(preposition,
              "after training, « l'envie de // monter » costs less than "
              "« l'envie // de monter »");

        // The evaluation corpus is the ticket's own criterion: delivered cuts
        // the rule already makes. The model wins its held-out folds and
        // loses there, so it does not beat the rule.
        {
            std::vector<CaptionBreakTrack> ruleLike;
            Check(ParseCaptionBreakCorpus(AfterPrepositionCorpus(true, "e"),
                                          ruleLike, stats, error),
                  "the evaluation corpus parses: " + error);
            CaptionBreakModel judged;
            CaptionBreakTrainingReport evaluated;
            Check(TrainCaptionBreakModel(tracks, ruleLike, options, judged,
                                         evaluated, error) &&
                      evaluated.model_held_out.reproduced_cues >
                          evaluated.rule_held_out.reproduced_cues &&
                      evaluated.evaluation_groups == 12 &&
                      evaluated.model_evaluation.reproduced_cues <
                          evaluated.rule_evaluation.reproduced_cues &&
                      !evaluated.beats_rule,
                  "a model that loses on the evaluation corpus does not "
                  "beat the rule, whatever its folds say");
        }

        CaptionBreakModel again;
        CaptionBreakTrainingReport second;
        Check(
            TrainCaptionBreakModel(tracks, {}, options, again, second, error) &&
                CaptionBreakModelFingerprint(again) ==
                    CaptionBreakModelFingerprint(model) &&
                !model.weights.empty(),
            "training twice gives the same weights");

        const std::string file =
            SerializeCaptionBreakModel(model, report, options);
        CaptionBreakModel read;
        bool admitted = false;
        Check(ParseCaptionBreakModel(file, read, admitted, error) &&
                  read.weights == model.weights && admitted,
              "the model file reads back: " + error);
        std::string tampered = file;
        const std::string fingerprint0 = CaptionBreakModelFingerprint(model);
        const size_t at = tampered.find(fingerprint0);
        if (at != std::string::npos)
            tampered.replace(at, fingerprint0.size(), "0123456789abcdef");
        Check(at != std::string::npos &&
                  !ParseCaptionBreakModel(tampered, read, admitted, error) &&
                  error.find("fingerprint") != std::string::npos,
              "weights that do not match their fingerprint are refused");

        // The forge's files, in a models folder of the test's own.
        const std::filesystem::path folder =
            std::filesystem::temp_directory_path() /
            ("cutmachine-models-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(folder);
        ::setenv("CUTMACHINE_MODELS", folder.c_str(), 1);
        CaptionBreakModel installed;
        std::string fingerprint;
        std::string path;
        Check(!LoadInstalledCaptionBreakModel(installed, fingerprint, path,
                                              error) &&
                  error.empty(),
              "no model installed: the rule decides, without an error");
        std::string candidate;
        std::string installedPath;
        CaptionBreakTrainingReport losing = report;
        losing.beats_rule = false;
        Check(WriteCaptionBreakModel(model, losing, options, true, candidate,
                                     installedPath, error) &&
                  !candidate.empty() && installedPath.empty() &&
                  !LoadInstalledCaptionBreakModel(installed, fingerprint, path,
                                                  error),
              "a model that does not beat the rule stays a candidate");
        Check(WriteCaptionBreakModel(model, report, options, true, candidate,
                                     installedPath, error) &&
                  !installedPath.empty() &&
                  LoadInstalledCaptionBreakModel(installed, fingerprint, path,
                                                 error) &&
                  fingerprint == CaptionBreakModelFingerprint(model) &&
                  installed.weights == model.weights,
              "a model that beats the rule is installed and named by its "
              "fingerprint: " +
                  error);
        ::setenv("CUTMACHINE_MODELS", "off", 1);
        Check(!LoadInstalledCaptionBreakModel(installed, fingerprint, path,
                                              error) &&
                  error.empty() && CaptionModelsDirectory().empty(),
              "CUTMACHINE_MODELS=off: no model, the rule decides");
        std::filesystem::remove_all(folder);
    }

    if (failures == 0) std::cout << "caption break model tests passed\n";
    return failures == 0 ? 0 : 1;
}
