#include "CaptionBreakModel.h"

#include "CanonicalHash.h"
#include "Json.h"
#include "UserDirectories.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace {

using mcp_json::Value;

constexpr const char* kModelKind = "cutmachine.caption_breaks";
constexpr int64_t kModelVersion = 1;
constexpr const char* kInstalledName = "caption_breaks.json";

// Between the rule's "no pause" (80 ms) and "short pause" (240 ms) terms:
// the gap the rule is given where the corpus does not know it.
constexpr RationalTime kNeutralGap{4, 25};

uint64_t Fnv(const std::string& text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

// The word as a lexical feature names it: the rule's syntax word, with the
// typographic apostrophe of edited cards read as the transcript's, and
// Latin-1 capitals lowered too ("À", "Être").
std::string LexicalWord(const std::string& text) {
    std::string straight;
    straight.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (index + 2 < text.size() &&
            static_cast<unsigned char>(text[index]) == 0xE2 &&
            static_cast<unsigned char>(text[index + 1]) == 0x80 &&
            static_cast<unsigned char>(text[index + 2]) == 0x99) {
            straight.push_back('\'');
            index += 2;
        } else {
            straight.push_back(text[index]);
        }
    }
    std::string word = SubtitleSyntaxWord(straight);
    for (size_t index = 0; index + 1 < word.size(); ++index) {
        const unsigned char second = word[index + 1];
        if (static_cast<unsigned char>(word[index]) == 0xC3 && second >= 0x80 &&
            second <= 0x9E && second != 0x97) {
            word[index + 1] = static_cast<char>(second + 0x20);
        }
    }
    return word;
}

const std::vector<std::pair<SubtitleWordFamily, const char*>>& FamilyNames() {
    static const std::vector<std::pair<SubtitleWordFamily, const char*>>
        kNames = {{kSubtitleDeterminer, "determiner"},
                  {kSubtitlePreposition, "preposition"},
                  {kSubtitleSubjectPronoun, "subject_pronoun"},
                  {kSubtitleAuxiliary, "auxiliary"},
                  {kSubtitleNegation, "negation"},
                  {kSubtitleConjunction, "conjunction"},
                  {kSubtitleModifier, "modifier"},
                  {kSubtitleClitic, "clitic"},
                  {kSubtitleSubordinator, "subordinator"}};
    return kNames;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

// The features of the junction after words[index]. A word's own features
// are named only when `known` says it is in the vocabulary (training), or
// always (scoring: a word outside it has no weight anyway).
template <typename Known>
std::vector<std::string> JunctionFeatures(
    const std::vector<SubtitleWord>& words, size_t index, bool timingEstimated,
    const Known& known) {
    const std::string& left = words[index].text;
    const std::string& right = words[index + 1].text;
    std::vector<std::string> features = {"bias"};
    const std::string leftWord = LexicalWord(left);
    const std::string rightWord = LexicalWord(right);
    if (!leftWord.empty() && known(leftWord))
        features.push_back("L:" + leftWord);
    if (!rightWord.empty() && known(rightWord))
        features.push_back("R:" + rightWord);
    const uint32_t leftFamilies = SubtitleWordFamilies(left);
    const uint32_t rightFamilies = SubtitleWordFamilies(right);
    for (const auto& [family, name] : FamilyNames()) {
        if (leftFamilies & family)
            features.push_back(std::string("Lf:") + name);
        if (rightFamilies & family)
            features.push_back(std::string("Rf:") + name);
    }
    if (EndsWith(left, "...") || EndsWith(left, "\xE2\x80\xA6"))
        features.push_back("Lp:ellipsis");
    else if (EndsWith(left, ","))
        features.push_back("Lp:comma");
    else if (EndsWith(left, ";") || EndsWith(left, ":"))
        features.push_back("Lp:colon");
    if (!timingEstimated) {
        const RationalTime gap = words[index + 1].start.sub(words[index].end);
        if (gap >= RationalTime{2, 5})
            features.push_back("gap:long");
        else if (gap >= RationalTime{6, 25})
            features.push_back("gap:pause");
        else if (gap > RationalTime{2, 25})
            features.push_back("gap:short");
        else
            features.push_back("gap:none");
    }
    return features;
}

std::string CharactersBucket(size_t characters) {
    static const std::vector<std::pair<size_t, const char*>> kBuckets = {
        {4, "chars:1-4"},    {8, "chars:5-8"},    {12, "chars:9-12"},
        {16, "chars:13-16"}, {20, "chars:17-20"}, {24, "chars:21-24"},
        {28, "chars:25-28"}, {32, "chars:29-32"}, {42, "chars:33-42"}};
    for (const auto& [limit, name] : kBuckets)
        if (characters <= limit) return name;
    return "chars:43+";
}

std::vector<std::string> CueFeatures(size_t words, size_t characters) {
    std::vector<std::string> features = {CharactersBucket(characters)};
    if (words == 1) features.push_back("words:1");
    return features;
}

int64_t RuleCost(const std::vector<SubtitleWord>& words, size_t index,
                 bool timingEstimated) {
    const RationalTime gap = timingEstimated
                                 ? kNeutralGap
                                 : words[index + 1].start.sub(words[index].end);
    return SubtitleRuleBoundaryCost(words[index].text, words[index + 1].text,
                                    gap);
}

int64_t WeightOf(const CaptionBreakModel& model, const std::string& name) {
    const auto found = model.weights.find(name);
    return found == model.weights.end() ? 0 : found->second;
}

// Half away from zero: the averaged weight as an integer.
int64_t RoundedQuotient(int64_t numerator, int64_t denominator) {
    if (numerator >= 0) return (numerator + (denominator / 2)) / denominator;
    return -((-numerator + (denominator / 2)) / denominator);
}

// "91.2" from 912 thousandths, integer arithmetic only.
std::string Percent(int64_t part, int64_t whole) {
    if (whole <= 0) return "0";
    const int64_t tenths = RoundedQuotient(part * 1000, whole);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

// F1 in tenths of a percent, for comparing two scorings exactly.
int64_t F1Tenths(const CaptionBreakScores& scores) {
    const int64_t denominator =
        scores.delivered_boundaries + scores.predicted_boundaries;
    if (denominator <= 0) return 0;
    return RoundedQuotient(2000 * scores.matched_boundaries, denominator);
}

bool SegmentTrack(const CaptionBreakModel* model,
                  const CaptionBreakTrack& track, std::vector<size_t>& ends,
                  std::string& error) {
    SubtitleSegmentationLimits limits;
    limits.maximum_characters = track.maximum_characters;
    limits.maximum_duration = track.maximum_duration;
    std::vector<int64_t> costs;
    if (model != nullptr) {
        limits = WithCaptionBreakModel(*model, track.words,
                                       track.timing_estimated, limits, costs);
    } else if (track.timing_estimated) {
        // The rule, but without the pauses the corpus does not know.
        costs.reserve(track.words.size());
        for (size_t index = 0; index + 1 < track.words.size(); ++index)
            costs.push_back(RuleCost(track.words, index, true));
        limits.boundary_costs = &costs;
    }
    std::vector<SubtitleCue> cues;
    std::vector<std::pair<size_t, size_t>> ranges;
    if (!SegmentSubtitleWords(track.words, limits, cues, &ranges, error))
        return false;
    ends.clear();
    for (const auto& range : ranges) ends.push_back(range.second);
    return true;
}

void AddScores(const CaptionBreakTrack& track,
               const std::vector<size_t>& predicted,
               CaptionBreakScores& scores) {
    const std::set<size_t> delivered(track.cue_ends.begin(),
                                     std::prev(track.cue_ends.end()));
    const std::set<size_t> found(predicted.begin(), std::prev(predicted.end()));
    scores.delivered_boundaries += static_cast<int64_t>(delivered.size());
    scores.predicted_boundaries += static_cast<int64_t>(found.size());
    for (const size_t end : found)
        if (delivered.count(end) != 0) ++scores.matched_boundaries;
    std::set<std::pair<size_t, size_t>> deliveredCues;
    size_t begin = 0;
    for (const size_t end : track.cue_ends) {
        deliveredCues.emplace(begin, end);
        begin = end;
    }
    scores.delivered_cues += static_cast<int64_t>(deliveredCues.size());
    begin = 0;
    for (const size_t end : predicted) {
        if (deliveredCues.count({begin, end}) != 0) ++scores.reproduced_cues;
        begin = end;
    }
}

// The learner's view of one track: each junction's features as indices,
// and the rule's cost there.
struct PreparedTrack {
    const CaptionBreakTrack* track = nullptr;
    std::vector<std::vector<int>> junctions;
    std::vector<int64_t> rule;
};

class FeatureSpace {
public:
    int Index(const std::string& name) {
        const auto [found, inserted] =
            indices_.emplace(name, static_cast<int>(names_.size()));
        if (inserted) names_.push_back(name);
        return found->second;
    }
    const std::vector<std::string>& names() const { return names_; }

private:
    std::unordered_map<std::string, int> indices_;
    std::vector<std::string> names_;
};

std::set<std::string> Vocabulary(
    const std::vector<const CaptionBreakTrack*>& tracks, size_t minimumGroups) {
    std::map<std::string, std::set<std::string>> groups;
    for (const CaptionBreakTrack* track : tracks)
        for (const SubtitleWord& word : track->words) {
            const std::string lexical = LexicalWord(word.text);
            if (!lexical.empty()) groups[lexical].insert(track->group);
        }
    std::set<std::string> vocabulary;
    for (const auto& [word, seen] : groups)
        if (seen.size() >= minimumGroups) vocabulary.insert(word);
    return vocabulary;
}

// The averaged structured perceptron over `tracks`.
CaptionBreakModel Train(const std::vector<const CaptionBreakTrack*>& tracks,
                        const CaptionBreakTrainingOptions& options,
                        size_t* vocabularySize) {
    const std::set<std::string> vocabulary =
        Vocabulary(tracks, options.vocabulary_minimum_groups);
    if (vocabularySize != nullptr) *vocabularySize = vocabulary.size();
    const auto known = [&](const std::string& word) {
        return vocabulary.count(word) != 0;
    };
    FeatureSpace space;
    std::vector<PreparedTrack> prepared;
    prepared.reserve(tracks.size());
    for (const CaptionBreakTrack* track : tracks) {
        PreparedTrack entry;
        entry.track = track;
        for (size_t index = 0; index + 1 < track->words.size(); ++index) {
            std::vector<int> ids;
            for (const std::string& name : JunctionFeatures(
                     track->words, index, track->timing_estimated, known))
                ids.push_back(space.Index(name));
            entry.junctions.push_back(std::move(ids));
            entry.rule.push_back(
                RuleCost(track->words, index, track->timing_estimated));
        }
        prepared.push_back(std::move(entry));
    }
    // Every cue feature has an index before the first segmentation.
    std::vector<int> bucketIds(64, -1);
    for (size_t characters = 0; characters < bucketIds.size(); ++characters)
        bucketIds[characters] = space.Index(CharactersBucket(characters));
    const int singleWordId = space.Index("words:1");
    const auto cueIds = [&](size_t words, size_t characters) {
        std::vector<int> ids = {
            characters < bucketIds.size()
                ? bucketIds[characters]
                : space.Index(CharactersBucket(characters))};
        if (words == 1) ids.push_back(singleWordId);
        return ids;
    };

    std::vector<int64_t> weights(space.names().size(), 0);
    std::vector<int64_t> accumulated(space.names().size(), 0);
    int64_t counter = 1;
    const auto grow = [&]() {
        weights.resize(space.names().size(), 0);
        accumulated.resize(space.names().size(), 0);
    };
    for (int32_t epoch = 0; epoch < options.epochs; ++epoch) {
        // A fixed order per epoch, the same on every machine.
        std::vector<std::pair<uint64_t, size_t>> order;
        for (size_t index = 0; index < prepared.size(); ++index)
            order.emplace_back(
                Fnv(prepared[index].track->group + "#" + std::to_string(index) +
                    "#" + std::to_string(epoch)),
                index);
        std::sort(order.begin(), order.end());
        for (const auto& [unused, trackIndex] : order) {
            const PreparedTrack& entry = prepared[trackIndex];
            const CaptionBreakTrack& track = *entry.track;
            std::vector<int64_t> costs(entry.rule.size());
            for (size_t index = 0; index < costs.size(); ++index) {
                int64_t cost = entry.rule[index];
                for (const int id : entry.junctions[index]) cost += weights[id];
                costs[index] = cost;
            }
            SubtitleSegmentationLimits limits;
            limits.maximum_characters = track.maximum_characters;
            limits.maximum_duration = track.maximum_duration;
            limits.boundary_costs = &costs;
            limits.cue_cost = [&](size_t begin, size_t end, size_t characters) {
                int64_t cost = 0;
                for (const int id : cueIds(end - begin, characters))
                    cost +=
                        id < static_cast<int>(weights.size()) ? weights[id] : 0;
                return cost;
            };
            std::vector<SubtitleCue> cues;
            std::vector<std::pair<size_t, size_t>> ranges;
            std::string error;
            if (!SegmentSubtitleWords(track.words, limits, cues, &ranges,
                                      error)) {
                ++counter;
                continue;
            }
            std::vector<size_t> predicted;
            predicted.reserve(ranges.size());
            for (const auto& range : ranges) predicted.push_back(range.second);
            if (predicted != track.cue_ends) {
                // Costs are minimised: what the segmentation chose and the
                // delivery did not costs more, and the reverse less.
                std::map<int, int64_t> delta;
                const auto count = [&](const std::vector<size_t>& ends,
                                       int64_t sign) {
                    size_t begin = 0;
                    for (const size_t end : ends) {
                        for (const int id :
                             cueIds(end - begin, SubtitleCueCharacters(
                                                     track.words, begin, end)))
                            delta[id] += sign;
                        if (end < track.words.size())
                            for (const int id : entry.junctions[end - 1])
                                delta[id] += sign;
                        begin = end;
                    }
                };
                count(predicted, 1);
                count(track.cue_ends, -1);
                grow();
                for (const auto& [id, change] : delta) {
                    if (change == 0) continue;
                    weights[id] += options.step * change;
                    accumulated[id] += counter * options.step * change;
                }
            }
            ++counter;
        }
    }
    grow();
    CaptionBreakModel model;
    for (size_t id = 0; id < weights.size(); ++id) {
        const int64_t averaged =
            RoundedQuotient((weights[id] * counter) - accumulated[id], counter);
        if (averaged != 0) model.weights[space.names()[id]] = averaged;
    }
    return model;
}

// Spread a card's words over its frames by characters, in thousandths of a
// frame: an estimate, kept as such (CaptionBreakTrack::timing_estimated).
RationalTime MilliFrameTime(int64_t milliFrames, int32_t num, int32_t den) {
    return RationalTime{milliFrames * den, num * 1000};
}

std::vector<std::string> SplitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (const char character : text) {
        if (character == ' ' || character == '\n' || character == '\r' ||
            character == '\t') {
            if (!current.empty()) words.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    return words;
}

// A delivered cue the segmentation could produce under some bounds: no
// sentence end and no long pause inside, a duration.
bool CueIsReachable(const std::vector<SubtitleWord>& words, size_t begin,
                    size_t end) {
    for (size_t index = begin; index + 1 < end; ++index) {
        if (SubtitleEndsSentence(words[index].text)) return false;
        if (words[index + 1].start.sub(words[index].end) >=
            kSubtitleCueMaximumGap)
            return false;
    }
    return SubtitleCueDisplayEnd(words, end).sub(words[begin].start) >
           RationalTime{0, 1};
}

// One delivered track as the forge learns from it: articles given to their
// noun, then cut into runs of reachable cues, each with its own bounds.
void AddTrack(const std::string& group, std::vector<SubtitleWord> words,
              std::vector<size_t> ends, bool timingEstimated,
              std::vector<CaptionBreakTrack>& tracks,
              CaptionBreakCorpusStats& stats) {
    for (size_t cue = 0; cue + 1 < ends.size();) {
        const size_t begin = cue == 0 ? 0 : ends[cue - 1];
        bool moved = false;
        while (ends[cue] > begin &&
               (SubtitleWordFamilies(words[ends[cue] - 1].text) &
                kSubtitleDeterminer) != 0 &&
               words[ends[cue]].start.sub(words[ends[cue] - 1].end) <
                   kSubtitleCueMaximumGap) {
            --ends[cue];
            moved = true;
        }
        if (moved) ++stats.article_breaks_moved;
        if (ends[cue] == begin) {
            ends.erase(ends.begin() + static_cast<std::ptrdiff_t>(cue));
            continue;
        }
        ++cue;
    }
    size_t runBegin = 0;
    std::vector<size_t> runEnds;
    const auto flush = [&]() {
        if (runEnds.empty()) return;
        CaptionBreakTrack track;
        track.group = group;
        track.timing_estimated = timingEstimated;
        track.words.assign(
            words.begin() + static_cast<std::ptrdiff_t>(runBegin),
            words.begin() + static_cast<std::ptrdiff_t>(runEnds.back()));
        size_t longest = 1;
        RationalTime slowest{5, 1};
        size_t begin = 0;
        for (const size_t end : runEnds) {
            const size_t local = end - runBegin;
            longest = std::max(
                longest, SubtitleCueCharacters(track.words, begin, local));
            if (local - begin > 1) {
                const RationalTime duration =
                    SubtitleCueDisplayEnd(track.words, local)
                        .sub(track.words[begin].start);
                if (duration > slowest) slowest = duration;
            }
            track.cue_ends.push_back(local);
            begin = local;
        }
        track.maximum_characters = longest;
        track.maximum_duration = slowest;
        tracks.push_back(std::move(track));
        runEnds.clear();
    };
    size_t begin = 0;
    for (const size_t end : ends) {
        if (CueIsReachable(words, begin, end)) {
            if (runEnds.empty()) runBegin = begin;
            runEnds.push_back(end);
        } else {
            ++stats.cards_left_out;
            flush();
        }
        begin = end;
    }
    flush();
}

bool ReadInt(const Value& object, const char* key, int64_t& output) {
    const Value* field = object.Find(key);
    return field != nullptr && field->IsNumber() && field->AsInt64(output);
}

void Append(Value& object, const char* key, const CaptionBreakScores& scores) {
    Value entry = Value::MakeObject();
    entry.Set("delivered_boundaries",
              Value::MakeInt(scores.delivered_boundaries));
    entry.Set("predicted_boundaries",
              Value::MakeInt(scores.predicted_boundaries));
    entry.Set("matched_boundaries", Value::MakeInt(scores.matched_boundaries));
    entry.Set("boundary_precision_percent",
              Value::MakeRawNumber(Percent(scores.matched_boundaries,
                                           scores.predicted_boundaries)));
    entry.Set("boundary_recall_percent",
              Value::MakeRawNumber(Percent(scores.matched_boundaries,
                                           scores.delivered_boundaries)));
    entry.Set("boundary_f1_percent",
              Value::MakeRawNumber(Percent(F1Tenths(scores), 1000)));
    entry.Set("delivered_cues", Value::MakeInt(scores.delivered_cues));
    entry.Set("reproduced_cues", Value::MakeInt(scores.reproduced_cues));
    entry.Set("reproduced_percent",
              Value::MakeRawNumber(
                  Percent(scores.reproduced_cues, scores.delivered_cues)));
    object.Set(key, std::move(entry));
}

std::string CutText(const std::vector<std::string>& words, size_t after) {
    std::string text;
    for (size_t index = 0; index < words.size(); ++index) {
        if (index > 0) text += " ";
        text += words[index];
        if (index == after) text += " //";
    }
    return text;
}

Value DecisionsValue(const std::vector<CaptionBreakDecisionResult>& results) {
    Value list = Value::MakeArray();
    const std::vector<CaptionBreakDecision>& cases = CaptionBreakDecisions();
    for (size_t index = 0; index < results.size() && index < cases.size();
         ++index) {
        const CaptionBreakDecisionResult& result = results[index];
        Value item = Value::MakeObject();
        item.Set("family", Value::MakeString(result.family));
        item.Set("firm", Value::MakeBool(result.firm));
        item.Set("chosen", Value::MakeString(CutText(cases[index].words,
                                                     cases[index].preferred)));
        item.Set("other", Value::MakeString(
                              CutText(cases[index].words, cases[index].other)));
        item.Set("rule_respects",
                 Value::MakeBool(result.rule_preferred < result.rule_other));
        item.Set("model_respects",
                 Value::MakeBool(result.model_preferred < result.model_other));
        item.Set("rule_costs",
                 Value::MakeString(std::to_string(result.rule_preferred) +
                                   " / " + std::to_string(result.rule_other)));
        item.Set("model_costs",
                 Value::MakeString(std::to_string(result.model_preferred) +
                                   " / " + std::to_string(result.model_other)));
        list.Push(std::move(item));
    }
    return list;
}

Value TrainingValue(const CaptionBreakTrainingReport& report,
                    const CaptionBreakTrainingOptions& options) {
    Value training = Value::MakeObject();
    training.Set("tracks", Value::MakeInt(static_cast<int64_t>(report.tracks)));
    training.Set("groups", Value::MakeInt(static_cast<int64_t>(report.groups)));
    training.Set("cues", Value::MakeInt(static_cast<int64_t>(report.cues)));
    training.Set("words", Value::MakeInt(static_cast<int64_t>(report.words)));
    training.Set("vocabulary",
                 Value::MakeInt(static_cast<int64_t>(report.vocabulary)));
    training.Set("vocabulary_minimum_groups",
                 Value::MakeInt(
                     static_cast<int64_t>(options.vocabulary_minimum_groups)));
    training.Set("epochs", Value::MakeInt(options.epochs));
    training.Set("step", Value::MakeInt(options.step));
    training.Set("folds", Value::MakeInt(report.folds));
    return training;
}

Value EvaluationValue(const CaptionBreakTrainingReport& report) {
    Value evaluation = Value::MakeObject();
    evaluation.Set(
        "tracks",
        Value::MakeInt(static_cast<int64_t>(report.evaluation_tracks)));
    evaluation.Set(
        "groups",
        Value::MakeInt(static_cast<int64_t>(report.evaluation_groups)));
    Append(evaluation, "rule", report.rule_evaluation);
    Append(evaluation, "model", report.model_evaluation);
    return evaluation;
}

Value WeightsValue(const CaptionBreakModel& model) {
    Value weights = Value::MakeObject();
    for (const auto& [name, weight] : model.weights)
        weights.Set(name, Value::MakeInt(weight));
    return weights;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text,
               std::string& error) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << text;
        if (!output) {
            error = "cannot write '" + temporary.string() + "'";
            return false;
        }
    }
    std::error_code renamed;
    std::filesystem::rename(temporary, path, renamed);
    if (renamed) {
        error = "cannot write '" + path.string() + "': " + renamed.message();
        return false;
    }
    return true;
}

}  // namespace

bool ParseCaptionBreakCorpus(const std::string& text,
                             std::vector<CaptionBreakTrack>& tracks,
                             CaptionBreakCorpusStats& stats,
                             std::string& error) {
    std::istringstream input(text);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const std::string where = "corpus line " + std::to_string(lineNumber);
        Value root;
        std::string parseError;
        if (!Value::Parse(line, root, parseError) || !root.IsObject()) {
            error = where + ": not a JSON object" +
                    (parseError.empty() ? "" : " (" + parseError + ")");
            return false;
        }
        const Value* group = root.Find("group");
        const Value* rate = root.Find("frame_rate");
        const Value* cards = root.Find("cards");
        int64_t num = 0;
        int64_t den = 0;
        if (group == nullptr || !group->IsString() ||
            group->AsString().empty()) {
            error = where + ": 'group' must name a project or a film";
            return false;
        }
        if (rate == nullptr || !rate->IsObject() ||
            !ReadInt(*rate, "num", num) || !ReadInt(*rate, "den", den) ||
            num <= 0 || den <= 0 ||
            num > std::numeric_limits<int32_t>::max() / 1000 || den > 100000) {
            error = where +
                    ": 'frame_rate' must be {\"num\", \"den\"}, "
                    "positive integers";
            return false;
        }
        if (cards == nullptr || !cards->IsArray()) {
            error = where + ": 'cards' must be an array";
            return false;
        }
        ++stats.lines;
        struct Card {
            int64_t start = 0;
            int64_t duration = 0;
            std::vector<std::string> words;
        };
        std::vector<Card> read;
        for (const Value& card : cards->AsArray()) {
            const Value* cardText = card.Find("text");
            Card entry;
            if (!card.IsObject() || cardText == nullptr ||
                !cardText->IsString() || !ReadInt(card, "start", entry.start) ||
                !ReadInt(card, "duration", entry.duration) || entry.start < 0 ||
                entry.duration < 0 || entry.start > (int64_t{1} << 32) ||
                entry.duration > (int64_t{1} << 24)) {
                error = where +
                        ": each card needs 'text', and 'start' and "
                        "'duration' in frames";
                return false;
            }
            entry.words = SplitWords(cardText->AsString());
            if (!entry.words.empty()) read.push_back(std::move(entry));
        }
        const auto frameTime = [&](int64_t milliFrames) {
            return MilliFrameTime(milliFrames, static_cast<int32_t>(num),
                                  static_cast<int32_t>(den));
        };
        std::vector<SubtitleWord> words;
        std::vector<size_t> ends;
        const Value* spoken = root.Find("words");
        if (spoken != nullptr) {
            if (!spoken->IsArray()) {
                error = where + ": 'words' must be an array";
                return false;
            }
            // Each word to the card its middle falls in, cards in time
            // order; a word never goes back to an earlier card.
            std::stable_sort(
                read.begin(), read.end(),
                [](const Card& a, const Card& b) { return a.start < b.start; });
            size_t card = 0;
            size_t lastCard = read.size();
            for (const Value& word : spoken->AsArray()) {
                const Value* wordText = word.Find("text");
                int64_t from = 0;
                int64_t to = 0;
                if (!word.IsObject() || wordText == nullptr ||
                    !wordText->IsString() || wordText->AsString().empty() ||
                    !ReadInt(word, "start", from) ||
                    !ReadInt(word, "end", to) || from < 0 || to < from ||
                    to > (int64_t{1} << 32)) {
                    error = where +
                            ": each word needs 'text', and 'start' and 'end' "
                            "in frames, in order";
                    return false;
                }
                const int64_t middle = from + to;  // doubled
                while (card < read.size() &&
                       2 * (read[card].start + read[card].duration) <= middle)
                    ++card;
                if (card >= read.size() || 2 * read[card].start > middle) {
                    ++stats.words_left_out;
                    continue;
                }
                if (lastCard != read.size() && card != lastCard)
                    ends.push_back(words.size());
                lastCard = card;
                words.push_back({wordText->AsString(), frameTime(from * 1000),
                                 frameTime(to * 1000)});
                ++stats.words;
            }
            if (!words.empty()) ends.push_back(words.size());
            stats.cards += ends.size();
        } else {
            for (const Card& card : read) {
                ++stats.cards;
                int64_t total = static_cast<int64_t>(card.words.size()) - 1;
                for (const std::string& word : card.words)
                    total += static_cast<int64_t>(SubtitleCharacterCount(word));
                int64_t prefix = 0;
                for (const std::string& word : card.words) {
                    const int64_t length =
                        static_cast<int64_t>(SubtitleCharacterCount(word));
                    const int64_t from =
                        (card.start * 1000) +
                        ((card.duration * 1000 * prefix) / total);
                    const int64_t to =
                        (card.start * 1000) +
                        ((card.duration * 1000 * (prefix + length)) / total);
                    words.push_back({word, frameTime(from), frameTime(to)});
                    prefix += length + 1;
                }
                ends.push_back(words.size());
                stats.words += card.words.size();
            }
        }
        if (!ends.empty())
            AddTrack(group->AsString(), std::move(words), std::move(ends),
                     spoken == nullptr, tracks, stats);
    }
    return true;
}

bool ReadCaptionBreakCorpus(const std::string& path,
                            std::vector<CaptionBreakTrack>& tracks,
                            CaptionBreakCorpusStats& stats,
                            std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot read corpus '" + path + "'";
        return false;
    }
    std::ostringstream text;
    text << input.rdbuf();
    if (!ParseCaptionBreakCorpus(text.str(), tracks, stats, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

std::vector<int64_t> CaptionBreakBoundaryCosts(
    const CaptionBreakModel& model, const std::vector<SubtitleWord>& words,
    bool timingEstimated) {
    std::vector<int64_t> costs;
    if (words.size() < 2) return costs;
    costs.reserve(words.size() - 1);
    const auto any = [](const std::string&) { return true; };
    for (size_t index = 0; index + 1 < words.size(); ++index) {
        int64_t cost = RuleCost(words, index, timingEstimated);
        for (const std::string& name :
             JunctionFeatures(words, index, timingEstimated, any))
            cost += WeightOf(model, name);
        costs.push_back(cost);
    }
    return costs;
}

int64_t CaptionBreakCueCost(const CaptionBreakModel& model, size_t words,
                            size_t characters) {
    int64_t cost = 0;
    for (const std::string& name : CueFeatures(words, characters))
        cost += WeightOf(model, name);
    return cost;
}

SubtitleSegmentationLimits WithCaptionBreakModel(
    const CaptionBreakModel& model, const std::vector<SubtitleWord>& words,
    bool timingEstimated, SubtitleSegmentationLimits limits,
    std::vector<int64_t>& costs) {
    costs = CaptionBreakBoundaryCosts(model, words, timingEstimated);
    limits.boundary_costs = &costs;
    const CaptionBreakModel* held = &model;
    limits.cue_cost = [held](size_t begin, size_t end, size_t characters) {
        return CaptionBreakCueCost(*held, end - begin, characters);
    };
    return limits;
}

const std::vector<CaptionBreakDecision>& CaptionBreakDecisions() {
    static const std::vector<CaptionBreakDecision> kDecisions = {
        {"article", {"elle", "plie", "la", "nappe"}, 1, 2, true},
        {"preposition",
         {"il", "a", "l'envie", "de", "monter", "un", "clip"},
         3,
         2},
        {"subject_pronoun", {"mais", "tu", "vois", "que"}, 0, 1},
        {"auxiliary", {"ils", "ont", "promis", "de", "venir"}, 2, 1},
        {"conjunction",
         {"ces", "murs", "étaient", "humides", "mais", "le", "vent", "tourne"},
         3,
         4,
         true},
        {"long_group", {"sous", "deux", "grands", "arbres"}, 0, 2}};
    return kDecisions;
}

CaptionBreakScores ScoreCaptionBreaks(
    const CaptionBreakModel* model,
    const std::vector<CaptionBreakTrack>& tracks) {
    CaptionBreakScores scores;
    for (const CaptionBreakTrack& track : tracks) {
        std::vector<size_t> predicted;
        std::string error;
        if (!SegmentTrack(model, track, predicted, error)) {
            // Counted as finding nothing: no boundary, no cue.
            predicted = {track.words.size()};
        }
        AddScores(track, predicted, scores);
    }
    return scores;
}

bool TrainCaptionBreakModel(const std::vector<CaptionBreakTrack>& tracks,
                            const std::vector<CaptionBreakTrack>& evaluation,
                            const CaptionBreakTrainingOptions& options,
                            CaptionBreakModel& model,
                            CaptionBreakTrainingReport& report,
                            std::string& error) {
    report = {};
    if (options.epochs < 1 || options.epochs > 100 || options.step < 1 ||
        options.step > 1000 || options.folds < 2 || options.folds > 50) {
        error = "epochs must lie in 1..100, step in 1..1000, folds in 2..50";
        return false;
    }
    std::set<std::string> groups;
    for (const CaptionBreakTrack& track : tracks) {
        groups.insert(track.group);
        report.cues += track.cue_ends.size();
        report.words += track.words.size();
    }
    report.tracks = tracks.size();
    report.groups = groups.size();
    report.folds = options.folds;
    if (groups.size() < static_cast<size_t>(options.folds)) {
        error = "the corpus has " + std::to_string(groups.size()) +
                " groups, fewer than the " + std::to_string(options.folds) +
                " folds held out in turn";
        return false;
    }
    for (int32_t fold = 0; fold < options.folds; ++fold) {
        std::vector<const CaptionBreakTrack*> training;
        std::vector<CaptionBreakTrack> heldOut;
        for (const CaptionBreakTrack& track : tracks) {
            if (static_cast<int32_t>(Fnv(track.group) %
                                     static_cast<uint64_t>(options.folds)) ==
                fold)
                heldOut.push_back(track);
            else
                training.push_back(&track);
        }
        if (heldOut.empty()) continue;
        const CaptionBreakModel foldModel = Train(training, options, nullptr);
        const CaptionBreakScores rule = ScoreCaptionBreaks(nullptr, heldOut);
        const CaptionBreakScores learned =
            ScoreCaptionBreaks(&foldModel, heldOut);
        for (const auto& [sum, part] :
             {std::pair<CaptionBreakScores*, const CaptionBreakScores*>{
                  &report.rule_held_out, &rule},
              {&report.model_held_out, &learned}}) {
            sum->delivered_boundaries += part->delivered_boundaries;
            sum->predicted_boundaries += part->predicted_boundaries;
            sum->matched_boundaries += part->matched_boundaries;
            sum->delivered_cues += part->delivered_cues;
            sum->reproduced_cues += part->reproduced_cues;
        }
    }
    std::vector<const CaptionBreakTrack*> all;
    all.reserve(tracks.size());
    for (const CaptionBreakTrack& track : tracks) all.push_back(&track);
    model = Train(all, options, &report.vocabulary);
    const auto wins = [](const CaptionBreakScores& learned,
                         const CaptionBreakScores& rule) {
        return learned.reproduced_cues > rule.reproduced_cues &&
               F1Tenths(learned) > F1Tenths(rule);
    };
    report.beats_rule = wins(report.model_held_out, report.rule_held_out);
    if (!evaluation.empty()) {
        std::set<std::string> evaluated;
        for (const CaptionBreakTrack& track : evaluation)
            evaluated.insert(track.group);
        report.evaluation_tracks = evaluation.size();
        report.evaluation_groups = evaluated.size();
        report.rule_evaluation = ScoreCaptionBreaks(nullptr, evaluation);
        report.model_evaluation = ScoreCaptionBreaks(&model, evaluation);
        if (!wins(report.model_evaluation, report.rule_evaluation))
            report.beats_rule = false;
    }
    const CaptionBreakModel rule;
    for (const CaptionBreakDecision& decision : CaptionBreakDecisions()) {
        std::vector<SubtitleWord> words;
        for (size_t index = 0; index < decision.words.size(); ++index)
            words.push_back(
                {decision.words[index],
                 RationalTime{static_cast<int64_t>(index) * 3, 10},
                 RationalTime{static_cast<int64_t>(index + 1) * 3, 10}});
        const std::vector<int64_t> ruleCosts =
            CaptionBreakBoundaryCosts(rule, words, true);
        const std::vector<int64_t> modelCosts =
            CaptionBreakBoundaryCosts(model, words, true);
        report.decisions.push_back(
            {decision.family, decision.firm, ruleCosts[decision.preferred],
             ruleCosts[decision.other], modelCosts[decision.preferred],
             modelCosts[decision.other]});
        if (decision.firm &&
            modelCosts[decision.preferred] >= modelCosts[decision.other])
            report.beats_rule = false;
    }
    return true;
}

std::string CaptionBreakModelFingerprint(const CaptionBreakModel& model) {
    Value canonical = Value::MakeObject();
    canonical.Set("kind", Value::MakeString(kModelKind));
    canonical.Set("version", Value::MakeInt(kModelVersion));
    canonical.Set("weights", WeightsValue(model));
    return CanonicalHash(canonical.Dump());
}

std::string SerializeCaptionBreakModel(
    const CaptionBreakModel& model, const CaptionBreakTrainingReport& report,
    const CaptionBreakTrainingOptions& options) {
    Value root = Value::MakeObject();
    root.Set("kind", Value::MakeString(kModelKind));
    root.Set("version", Value::MakeInt(kModelVersion));
    root.Set("fingerprint",
             Value::MakeString(CaptionBreakModelFingerprint(model)));
    root.Set("admitted", Value::MakeBool(report.beats_rule));
    root.Set("training", TrainingValue(report, options));
    Value heldOut = Value::MakeObject();
    Append(heldOut, "rule", report.rule_held_out);
    Append(heldOut, "model", report.model_held_out);
    root.Set("held_out", std::move(heldOut));
    if (report.evaluation_tracks > 0)
        root.Set("evaluation", EvaluationValue(report));
    root.Set("decisions", DecisionsValue(report.decisions));
    root.Set("weights", WeightsValue(model));
    return root.Dump() + "\n";
}

bool ParseCaptionBreakModel(const std::string& text, CaptionBreakModel& model,
                            bool& admitted, std::string& error) {
    model = {};
    admitted = false;
    Value root;
    std::string parseError;
    if (!Value::Parse(text, root, parseError) || !root.IsObject()) {
        error = "the caption break model is not a JSON object";
        return false;
    }
    const Value* kind = root.Find("kind");
    const Value* version = root.Find("version");
    int64_t versionNumber = 0;
    if (kind == nullptr || !kind->IsString() ||
        kind->AsString() != kModelKind || version == nullptr ||
        !version->AsInt64(versionNumber) || versionNumber != kModelVersion) {
        error = std::string("not a ") + kModelKind + " model of version " +
                std::to_string(kModelVersion);
        return false;
    }
    const Value* weights = root.Find("weights");
    if (weights == nullptr || !weights->IsObject()) {
        error = "the caption break model has no 'weights' object";
        return false;
    }
    for (const auto& [name, value] : weights->AsObject()) {
        int64_t weight = 0;
        if (!value.IsNumber() || !value.AsInt64(weight) || weight < -1000000 ||
            weight > 1000000) {
            error = "weight '" + name + "' is not an integer within 10^6";
            return false;
        }
        model.weights[name] = weight;
    }
    const Value* fingerprint = root.Find("fingerprint");
    if (fingerprint == nullptr || !fingerprint->IsString() ||
        fingerprint->AsString() != CaptionBreakModelFingerprint(model)) {
        error =
            "the caption break model's fingerprint does not match its "
            "weights";
        return false;
    }
    const Value* admittedValue = root.Find("admitted");
    admitted = admittedValue != nullptr && admittedValue->IsBool() &&
               admittedValue->AsBool();
    return true;
}

std::string SerializeCaptionBreakReport(
    const CaptionBreakTrainingReport& report,
    const CaptionBreakTrainingOptions& options) {
    Value root = Value::MakeObject();
    root.Set("training", TrainingValue(report, options));
    Value heldOut = Value::MakeObject();
    Append(heldOut, "rule", report.rule_held_out);
    Append(heldOut, "model", report.model_held_out);
    root.Set("held_out", std::move(heldOut));
    if (report.evaluation_tracks > 0)
        root.Set("evaluation", EvaluationValue(report));
    root.Set("beats_rule", Value::MakeBool(report.beats_rule));
    root.Set("decisions", DecisionsValue(report.decisions));
    return root.Dump();
}

std::string CaptionModelsDirectory() {
    if (const char* configured = std::getenv("CUTMACHINE_MODELS")) {
        std::string value = configured;
        if (value == "off") return {};
        if (!value.empty()) return value;
    }
    const std::string data = user_directories::ApplicationData();
    if (data.empty()) return {};
    return (std::filesystem::path(data) / "models").string();
}

bool LoadInstalledCaptionBreakModel(CaptionBreakModel& model,
                                    std::string& fingerprint, std::string& path,
                                    std::string& error) {
    model = {};
    fingerprint.clear();
    path.clear();
    error.clear();
    const std::string directory = CaptionModelsDirectory();
    if (directory.empty()) return false;
    const std::filesystem::path file =
        std::filesystem::path(directory) / kInstalledName;
    std::error_code missing;
    if (!std::filesystem::exists(file, missing)) return false;
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    bool admitted = false;
    if (!input || !ParseCaptionBreakModel(text.str(), model, admitted, error)) {
        if (error.empty()) error = "cannot read '" + file.string() + "'";
        error = file.string() + ": " + error;
        model = {};
        return false;
    }
    if (!admitted) {
        error = file.string() + ": installed without beating the rule";
        model = {};
        return false;
    }
    fingerprint = CaptionBreakModelFingerprint(model);
    path = file.string();
    return true;
}

bool WriteCaptionBreakModel(const CaptionBreakModel& model,
                            const CaptionBreakTrainingReport& report,
                            const CaptionBreakTrainingOptions& options,
                            bool install, std::string& candidatePath,
                            std::string& installedPath, std::string& error) {
    candidatePath.clear();
    installedPath.clear();
    const std::string directory = CaptionModelsDirectory();
    if (directory.empty()) {
        error =
            "models are off (CUTMACHINE_MODELS=off) or there is no home "
            "folder to keep them in";
        return false;
    }
    const std::string text = SerializeCaptionBreakModel(model, report, options);
    const std::filesystem::path candidate =
        std::filesystem::path(directory) / "candidates" /
        ("caption_breaks-" + CaptionBreakModelFingerprint(model) + ".json");
    if (!WriteFile(candidate, text, error)) return false;
    candidatePath = candidate.string();
    if (install && report.beats_rule) {
        const std::filesystem::path installed =
            std::filesystem::path(directory) / kInstalledName;
        if (!WriteFile(installed, text, error)) return false;
        installedPath = installed.string();
    }
    return true;
}

bool RunCaptionBreakForge(const std::vector<std::string>& corpus,
                          const std::vector<std::string>& evaluation,
                          const CaptionBreakTrainingOptions& options,
                          bool install, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    if (install && evaluation.empty()) {
        errorName = "ValidationFailed";
        message =
            "installing a model needs an evaluation corpus it was not "
            "trained on, the ticket's own criterion: folds of the "
            "training corpus share its bias";
        return false;
    }
    std::vector<CaptionBreakTrack> tracks;
    CaptionBreakCorpusStats stats;
    for (const std::string& path : corpus) {
        if (!ReadCaptionBreakCorpus(path, tracks, stats, message)) {
            errorName = "IoError";
            return false;
        }
    }
    std::vector<CaptionBreakTrack> evaluated;
    CaptionBreakCorpusStats evaluationStats;
    for (const std::string& path : evaluation) {
        if (!ReadCaptionBreakCorpus(path, evaluated, evaluationStats,
                                    message)) {
            errorName = "IoError";
            return false;
        }
    }
    CaptionBreakModel model;
    CaptionBreakTrainingReport report;
    if (!TrainCaptionBreakModel(tracks, evaluated, options, model, report,
                                message)) {
        errorName = "ValidationFailed";
        return false;
    }
    Value root;
    std::string parseError;
    if (!Value::Parse(SerializeCaptionBreakReport(report, options), root,
                      parseError)) {
        errorName = "InternalError";
        message = parseError;
        return false;
    }
    Value read = Value::MakeObject();
    read.Set("lines", Value::MakeInt(static_cast<int64_t>(stats.lines)));
    read.Set("cards", Value::MakeInt(static_cast<int64_t>(stats.cards)));
    read.Set("words", Value::MakeInt(static_cast<int64_t>(stats.words)));
    read.Set("article_breaks_moved",
             Value::MakeInt(static_cast<int64_t>(stats.article_breaks_moved)));
    read.Set("cards_left_out",
             Value::MakeInt(static_cast<int64_t>(stats.cards_left_out)));
    read.Set("words_left_out",
             Value::MakeInt(static_cast<int64_t>(stats.words_left_out)));
    root.Set("corpus", std::move(read));
    root.Set("fingerprint",
             Value::MakeString(CaptionBreakModelFingerprint(model)));
    if (CaptionModelsDirectory().empty()) {
        if (install) {
            errorName = "ValidationFailed";
            message =
                "models are off (CUTMACHINE_MODELS=off): nothing can be "
                "installed";
            return false;
        }
        root.Set("installed", Value::MakeBool(false));
        resultJson = root.Dump();
        return true;
    }
    std::string candidatePath;
    std::string installedPath;
    if (!WriteCaptionBreakModel(model, report, options, install, candidatePath,
                                installedPath, message)) {
        errorName = "IoError";
        return false;
    }
    root.Set("candidate_path", Value::MakeString(candidatePath));
    root.Set("installed", Value::MakeBool(!installedPath.empty()));
    if (!installedPath.empty())
        root.Set("installed_path", Value::MakeString(installedPath));
    resultJson = root.Dump();
    return true;
}
