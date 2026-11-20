#pragma once

// RM3 -- docs/roadmap/REGLES-2026-10.md. Where a carton ends, learned from
// delivered captions, inside the rule's bounds.
//
// The segmentation stays SegmentSubtitleWords: the same limits, the same
// reading pace, sentence ends and long pauses as hard breaks. What is
// learned is a correction to its hand-written boundary cost: one integer
// per feature of a junction (the words on either side, their families, the
// punctuation, the pause) and of a cue (its length), added to
// SubtitleRuleBoundaryCost. A model without weights is the rule, cue for
// cue. The lot's first rule -- the rule defines what is allowed, the model
// ranks inside it -- holds by construction: a cost never lifts a bound.
//
// Trained as a structured perceptron (Collins, 2002): the segmentation
// itself runs on each delivered track, and where its cues differ from the
// delivered ones, the weights move by a fixed step towards the delivered
// cues. The average over every step is kept. Integers throughout, so that
// two machines training on the same corpus get the same weights, the same
// fingerprint and the same cues (PRD12: the last bits of floating point are
// the compiler's).
//
// Admission, the lot's rules 2, 4 and 5: the forge installs a model only if,
// trained without them, it reproduces held-out groups (projects, films)
// better than the rule does, and does the same on an evaluation corpus it
// was never trained on. generate_captions uses the installed model and
// names it by its fingerprint. Without one, or with CUTMACHINE_MODELS=off,
// the rule decides alone. The document never depends on it: the cartons
// are written into it, never "ask the model".
//
// The forge scores the segmentation alone, under bounds drawn from each
// track's own cartons; generate_captions then measures widths, joins short
// cartons and snaps them to cuts. The forge's verdict is necessary, not
// sufficient: on 15 October 2026 a model trained on three films of the
// PRJA series won its held-out film here (48.9 % of cartons against
// 44.2 %) and lost through generate_captions (117 cartons to redo against
// 82). No model was admitted; the ticket's criterion is measured through
// generate_captions.
//
// A word gets weights of its own only if it appears in enough groups
// (vocabulary_minimum_groups): the weights are a table of common French
// words, not of what a client said.

#include "RationalTime.h"
#include "Subtitles.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// One delivered caption track, as the forge learns from it.
struct CaptionBreakTrack {
    // What held-out evaluation keeps together: a project, a film. Never
    // written into a model.
    std::string group;
    std::vector<SubtitleWord> words;
    // Where each delivered cue ends, as a word index; the last is
    // words.size().
    std::vector<size_t> cue_ends;
    // The corpus kept the cards' times, not the words': each word's time is
    // spread over its card by characters. A pause inside a card is then
    // unknown, so the pause features stay silent and the rule's pause terms
    // are neutral.
    bool timing_estimated = false;
    // The track's own bounds, set from its delivered cues: the house setting
    // an editor would choose, given to the rule and to the model alike.
    size_t maximum_characters = 42;
    RationalTime maximum_duration{5, 1};
};

struct CaptionBreakModel {
    // Feature name -> correction in the segmentation's cost units.
    std::map<std::string, int64_t> weights;
};

// What a corpus held, and what the forge left out of it.
struct CaptionBreakCorpusStats {
    size_t lines = 0;
    size_t cards = 0;
    size_t words = 0;
    // REGLES-2026-10, the author's decision of 12 October 2026: "elle plie //
    // la nappe" (made-up words), the article stays with its noun. A delivered
    // card that ends on a determiner gives it to the next card before
    // anything is learned.
    size_t article_breaks_moved = 0;
    // Cards the segmentation could never produce: a sentence end inside, a
    // long pause inside, no duration. Their tracks are cut there, and the
    // card left out.
    size_t cards_left_out = 0;
    // Words given with their times that fall in no card: not shown, so not
    // learned from.
    size_t words_left_out = 0;
};

// Reads one corpus file, one delivered track per line:
//
//   {"group": "PRJA", "frame_rate": {"num": 25, "den": 1},
//    "cards": [{"text": "Vers midi, sur", "start": 90000, "duration": 11}]}
//
// `start` and `duration` are frames at `frame_rate`. Other keys are ignored.
// Word times are estimated within each card (timing_estimated), to a
// thousandth of a frame. Appends to `tracks`.
//
// A line may also give the words as they were said, in frames:
//
//   "words": [{"text": "Vers", "start": 0, "end": 3}, ...]
//
// The cards then only say where each word was shown: a word belongs to the
// card its middle falls in, and one that falls in none is left out. Their
// times are the words' own, pauses included.
bool ReadCaptionBreakCorpus(const std::string& path,
                            std::vector<CaptionBreakTrack>& tracks,
                            CaptionBreakCorpusStats& stats, std::string& error);

// The same, from text already read (tests, tools).
bool ParseCaptionBreakCorpus(const std::string& text,
                             std::vector<CaptionBreakTrack>& tracks,
                             CaptionBreakCorpusStats& stats,
                             std::string& error);

// The boundary costs SegmentSubtitleWords takes from this model for
// `words`: the rule's cost plus the corrections, one per junction.
std::vector<int64_t> CaptionBreakBoundaryCosts(
    const CaptionBreakModel& model, const std::vector<SubtitleWord>& words,
    bool timingEstimated);

// The correction the model adds to a cue of `words` words and `characters`
// characters.
int64_t CaptionBreakCueCost(const CaptionBreakModel& model, size_t words,
                            size_t characters);

// `limits` with this model's costs for `words`. `costs` must outlive the
// returned limits: they point into it.
SubtitleSegmentationLimits WithCaptionBreakModel(
    const CaptionBreakModel& model, const std::vector<SubtitleWord>& words,
    bool timingEstimated, SubtitleSegmentationLimits limits,
    std::vector<int64_t>& costs);

struct CaptionBreakTrainingOptions {
    int32_t epochs = 5;
    // How far one mistake moves a weight, in cost units.
    int64_t step = 10;
    // Held-out evaluation: each group falls in one of `folds` folds by its
    // name, and each fold is segmented by a model trained on the others.
    int32_t folds = 5;
    size_t vocabulary_minimum_groups = 10;
};

// Agreement with the delivered cues, summed over tracks.
struct CaptionBreakScores {
    int64_t delivered_boundaries = 0;
    int64_t predicted_boundaries = 0;
    int64_t matched_boundaries = 0;
    int64_t delivered_cues = 0;
    int64_t reproduced_cues = 0;
};

// REGLES-2026-10, 12 October 2026: the author chose between two cuts of the
// same words, one family at a time. `preferred` is the cut he chose; a score
// respects his choice when ending a cue there costs strictly less. Two hold
// as rules in his own words (`firm`: the article stays with its noun, the
// conjunction opens the next carton) and a model must respect them to be
// admitted; the other four are preferences that yield to balance between
// cartons, which a boundary cost alone does not see.
struct CaptionBreakDecision {
    std::string family;
    std::vector<std::string> words;
    // Junction indices: a cut after words[preferred] and after
    // words[other].
    size_t preferred = 0;
    size_t other = 0;
    bool firm = false;
};
const std::vector<CaptionBreakDecision>& CaptionBreakDecisions();

struct CaptionBreakDecisionResult {
    std::string family;
    bool firm = false;
    int64_t rule_preferred = 0;
    int64_t rule_other = 0;
    int64_t model_preferred = 0;
    int64_t model_other = 0;
};

struct CaptionBreakTrainingReport {
    size_t tracks = 0;
    size_t groups = 0;
    size_t cues = 0;
    size_t words = 0;
    size_t vocabulary = 0;
    int32_t folds = 0;
    CaptionBreakScores rule_held_out;
    CaptionBreakScores model_held_out;
    std::vector<CaptionBreakDecisionResult> decisions;
    // The evaluation corpus, never trained on: the ticket's own criterion.
    // On 15 October 2026 a model trained on the author's Resolve corpus beat
    // the rule on its held-out projects (30.6 % of cartons against 21.8 %) and
    // lost on the PRJA series (199 cartons to redo against 82): the folds
    // shared the corpus's bias, Resolve's automatic segmentation.
    size_t evaluation_tracks = 0;
    size_t evaluation_groups = 0;
    CaptionBreakScores rule_evaluation;
    CaptionBreakScores model_evaluation;
    // Whether the model reproduces held-out groups better than the rule
    // (more delivered cues, and a better boundary F1), does the same on the
    // evaluation corpus when there is one, and respects the author's firm
    // choices. The forge installs only such a model, and only with an
    // evaluation.
    bool beats_rule = false;
};

// Held-out evaluation over `options.folds`, then the model trained on every
// track of `tracks` and scored on `evaluation`, which no fold trains on.
// Refused when fewer groups than folds.
bool TrainCaptionBreakModel(const std::vector<CaptionBreakTrack>& tracks,
                            const std::vector<CaptionBreakTrack>& evaluation,
                            const CaptionBreakTrainingOptions& options,
                            CaptionBreakModel& model,
                            CaptionBreakTrainingReport& report,
                            std::string& error);

// Agreement of the rule (`model` null) or of a model with the delivered
// cues of `tracks`.
CaptionBreakScores ScoreCaptionBreaks(
    const CaptionBreakModel* model,
    const std::vector<CaptionBreakTrack>& tracks);

// FNV-1a over the canonical text of the weights (CanonicalHash.h): it names
// the weights that cut a carton, it does not authenticate them.
std::string CaptionBreakModelFingerprint(const CaptionBreakModel& model);

// The model file: its weights, its fingerprint, and the report it was
// admitted (or not) on.
std::string SerializeCaptionBreakModel(
    const CaptionBreakModel& model, const CaptionBreakTrainingReport& report,
    const CaptionBreakTrainingOptions& options);
bool ParseCaptionBreakModel(const std::string& text, CaptionBreakModel& model,
                            bool& admitted, std::string& error);

// The report as the forge answers it.
std::string SerializeCaptionBreakReport(
    const CaptionBreakTrainingReport& report,
    const CaptionBreakTrainingOptions& options);

// Where models live: CUTMACHINE_MODELS when set ("off": nowhere), else
// "models" in the engine's data (UserDirectories.h). Empty when nowhere.
std::string CaptionModelsDirectory();

// The installed model: `<models>/caption_breaks.json`, admitted. False with
// an empty `error` when there is none (the rule decides); false with an
// error when the file is there but unreadable.
bool LoadInstalledCaptionBreakModel(CaptionBreakModel& model,
                                    std::string& fingerprint, std::string& path,
                                    std::string& error);

// Writes the model file under `<models>/candidates/`, named by its
// fingerprint, and -- when `install` and the report says it beats the rule
// -- as the installed model too. `installedPath` is left empty when it was
// not installed.
bool WriteCaptionBreakModel(const CaptionBreakModel& model,
                            const CaptionBreakTrainingReport& report,
                            const CaptionBreakTrainingOptions& options,
                            bool install, std::string& candidatePath,
                            std::string& installedPath, std::string& error);

// The forge as train_caption_breaks and --train-caption-breaks run it: the
// corpus files read, the model trained and measured, its file written as a
// candidate and, when `install` and it beats the rule, installed. Installing
// needs `evaluation` files, never trained on. The answer is the training
// report with the corpus counts, the fingerprint and the files written. A
// corpus that does not read is an IoError; a refusal of the options or of a
// corpus too small, a ValidationFailed.
bool RunCaptionBreakForge(const std::vector<std::string>& corpus,
                          const std::vector<std::string>& evaluation,
                          const CaptionBreakTrainingOptions& options,
                          bool install, std::string& resultJson,
                          std::string& errorName, std::string& message);
