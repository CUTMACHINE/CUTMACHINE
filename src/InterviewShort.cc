#include "InterviewShort.h"

#include "ClipEnabled.h"
#include "Json.h"
#include "Subtitles.h"
#include "TimelineTranscription.h"
#include "Transcription.h"
#include "WordAlignment.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <vector>

namespace {

mcp_json::Value TimeValue(const RationalTime& time) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("value", mcp_json::Value::MakeInt(time.value));
    value.Set("rate", mcp_json::Value::MakeInt(time.rate));
    return value;
}

bool ReadTimeValue(const mcp_json::Value* field, RationalTime& time) {
    if (field == nullptr || !field->IsObject()) return false;
    const mcp_json::Value* value = field->Find("value");
    const mcp_json::Value* rate = field->Find("rate");
    int64_t valueOut = 0;
    int64_t rateOut = 0;
    if (value == nullptr || rate == nullptr || !value->AsInt64(valueOut) ||
        !rate->AsInt64(rateOut) || rateOut <= 0 ||
        rateOut > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    time = RationalTime{valueOut, static_cast<int32_t>(rateOut)};
    return true;
}

mcp_json::Value WordCheckValue(const TimelineWordCheck& check) {
    mcp_json::Value heard = mcp_json::Value::MakeObject();
    heard.Set("montage_readings", mcp_json::Value::MakeInt(check.readings));
    mcp_json::Value unheard = mcp_json::Value::MakeArray();
    for (const UnheardWord& word : check.unheard) {
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("text", mcp_json::Value::MakeString(word.text));
        item.Set("clip_id", mcp_json::Value::MakeString(word.clip_id));
        item.Set("source_id", mcp_json::Value::MakeString(word.source_id));
        item.Set("source_in", TimeValue(word.source_in));
        item.Set("timeline_in", TimeValue(word.timeline_in));
        item.Set("duration", TimeValue(word.duration));
        unheard.Push(std::move(item));
    }
    heard.Set("unheard_words", std::move(unheard));
    mcp_json::Value inflected = mcp_json::Value::MakeArray();
    for (const InflectedWord& word : check.inflected) {
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("text", mcp_json::Value::MakeString(word.text));
        item.Set("heard_as", mcp_json::Value::MakeString(word.heard_as));
        item.Set("clip_id", mcp_json::Value::MakeString(word.clip_id));
        item.Set("timeline_in", TimeValue(word.timeline_in));
        inflected.Push(std::move(item));
    }
    heard.Set("inflected_matches", std::move(inflected));
    mcp_json::Value cut = mcp_json::Value::MakeArray();
    for (const CutHeardWord& word : check.cut_but_heard) {
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("text", mcp_json::Value::MakeString(word.text));
        item.Set("reason",
                 mcp_json::Value::MakeString("aligned_in_removed_range"));
        item.Set("clip_id", mcp_json::Value::MakeString(word.clip_id));
        item.Set("source_id", mcp_json::Value::MakeString(word.source_id));
        item.Set("source_in", TimeValue(word.source_in));
        item.Set("duration", TimeValue(word.duration));
        item.Set("timeline_in", TimeValue(word.timeline_in));
        mcp_json::Value spellings = mcp_json::Value::MakeArray();
        for (const std::string& spelling : word.heard_as) {
            spellings.Push(mcp_json::Value::MakeString(spelling));
        }
        item.Set("heard_as", std::move(spellings));
        cut.Push(std::move(item));
    }
    heard.Set("cut_but_heard", std::move(cut));
    return heard;
}

struct AudibleClip {
    const DocumentClip* clip = nullptr;
    const Transcript* transcript = nullptr;
    std::vector<std::pair<size_t, bool>> selected_words;
};

struct WordCandidate {
    size_t clip_index = 0;
    size_t word_index = 0;
    RationalTime source_start;
    RationalTime source_end;
    RationalTime timeline_start;
    RationalTime timeline_end;
    RationalTime overlap;
};

bool PositiveOverlap(const RationalTime& leftStart, const RationalTime& leftEnd,
                     const RationalTime& rightStart,
                     const RationalTime& rightEnd) {
    return leftStart < rightEnd && rightStart < leftEnd;
}

// Two fragments belong to the same played occurrence when their timeline
// intervals overlap, or when they meet at an edit while carrying disjoint
// portions of the source word. Reusing the complete word in two consecutive
// clips therefore remains two occurrences, while a word cut in half does not.
bool SamePlayedOccurrence(const WordCandidate& left,
                          const WordCandidate& right) {
    if (PositiveOverlap(left.timeline_start, left.timeline_end,
                        right.timeline_start, right.timeline_end))
        return true;
    const bool touches = left.timeline_end == right.timeline_start ||
                         right.timeline_end == left.timeline_start;
    return touches && !PositiveOverlap(left.source_start, left.source_end,
                                       right.source_start, right.source_end);
}

// ITW3 -- a word counts as heard when a reading of the montage audio writes
// the same word (MatchHeardWord) within this distance of where the source
// words place it. Wide on purpose: Whisper's own timestamps drift by up to
// 1.5 s on these rushes, and a check that is too strict would drop words
// that were spoken.
constexpr int64_t kHeardToleranceSeconds = 2;

void AppendUtf8(uint32_t codePoint, std::string& text) {
    if (codePoint < 0x80) {
        text.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// Spaces, apostrophes and hyphens: they end a token.
bool SplitsTokens(uint32_t codePoint) {
    if (codePoint < 0x80) {
        return std::isspace(static_cast<int>(codePoint)) != 0 ||
               codePoint == '\'' || codePoint == '-' || codePoint == '`';
    }
    return codePoint == 0x00A0 || codePoint == 0x00B4 || codePoint == 0x02BC ||
           (codePoint >= 0x2000 && codePoint <= 0x200B) ||
           (codePoint >= 0x2010 && codePoint <= 0x2015) ||
           codePoint == 0x2018 || codePoint == 0x2019 || codePoint == 0x201B ||
           codePoint == 0x2032 || codePoint == 0x202F || codePoint == 0x205F ||
           codePoint == 0x3000 || codePoint == 0xFEFF;
}

// Punctuation and symbols that never belong to a word: dropped.
bool IsWordPunctuation(uint32_t codePoint) {
    if (codePoint < 0x80) {
        return std::isalnum(static_cast<int>(codePoint)) == 0;
    }
    return (codePoint >= 0x00A1 && codePoint <= 0x00BF) ||
           codePoint == 0x00D7 || codePoint == 0x00F7 ||
           (codePoint >= 0x200C && codePoint <= 0x200F) ||
           (codePoint >= 0x2016 && codePoint <= 0x205E);
}

// Lowercase for the Latin letters French writes: ASCII, Latin-1 (À to Þ),
// Œ and Ÿ. Any other code point is kept as it is.
uint32_t LowercaseLetter(uint32_t codePoint) {
    if (codePoint >= 'A' && codePoint <= 'Z') {
        return codePoint + 0x20;
    }
    if (codePoint >= 0x00C0 && codePoint <= 0x00DE && codePoint != 0x00D7) {
        return codePoint + 0x20;
    }
    if (codePoint == 0x0152) {
        return 0x0153;
    }
    if (codePoint == 0x0178) {
        return 0x00FF;
    }
    return codePoint;
}

// AGENT-2026-09 A13 -- the French words for a number written in digits, as
// a reading writes them and as the folding splits them (a hyphen splits a
// token, so "quatre-vingt-dix" is three). A digit token is a cardinal
// ("21"), or an ordinal with its ending ("3ème", "3e", "1er", "1re",
// "2nd"). On a real montage whisper read "3ème" back as "troisième" and
// the word was dropped from its caption as unheard. Numbers above 999 are
// left as digits: a year is read many ways.
constexpr std::array<const char*, 17> kUnits = {
    "zéro",  "un",     "deux",     "trois",  "quatre", "cinq",
    "six",   "sept",   "huit",     "neuf",   "dix",    "onze",
    "douze", "treize", "quatorze", "quinze", "seize"};

void AppendNumberUnder100(int64_t value, std::vector<std::string>& out) {
    if (value < 17) {
        out.push_back(kUnits[static_cast<size_t>(value)]);
        return;
    }
    if (value < 20) {
        out.push_back("dix");
        out.push_back(kUnits[static_cast<size_t>(value - 10)]);
        return;
    }
    static const std::array<const char*, 7> kTens = {
        "vingt",    "trente",   "quarante", "cinquante",
        "soixante", "soixante", "quatre"};
    const int64_t tens = value / 10;
    const int64_t rest = value % 10;
    if (tens == 8 || tens == 9) {
        out.push_back("quatre");
        const int64_t after = value - 80;
        if (after == 0) {
            out.push_back("vingts");
            return;
        }
        out.push_back("vingt");
        AppendNumberUnder100(after, out);
        return;
    }
    out.push_back(kTens[static_cast<size_t>(tens - 2)]);
    if (tens == 7) {
        // 70 is soixante-dix, 71 soixante et onze, 72 soixante-douze.
        const int64_t after = value - 60;
        if (after == 11) out.push_back("et");
        AppendNumberUnder100(after, out);
        return;
    }
    if (rest == 0) return;
    if (rest == 1) out.push_back("et");
    out.push_back(kUnits[static_cast<size_t>(rest)]);
}

void AppendCardinal(int64_t value, std::vector<std::string>& out) {
    if (value >= 100) {
        const int64_t hundreds = value / 100;
        const int64_t rest = value % 100;
        if (hundreds > 1) out.push_back(kUnits[static_cast<size_t>(hundreds)]);
        out.push_back(hundreds > 1 && rest == 0 ? "cents" : "cent");
        if (rest == 0) return;
        AppendNumberUnder100(rest, out);
        return;
    }
    AppendNumberUnder100(value, out);
}

bool ExpandNumberToken(const std::string& token,
                       std::vector<std::string>& out) {
    size_t digits = 0;
    while (digits < token.size() &&
           std::isdigit(static_cast<unsigned char>(token[digits])) != 0)
        ++digits;
    if (digits == 0 || digits > 3) return false;
    const int64_t value = std::stoll(token.substr(0, digits));
    const std::string ending = token.substr(digits);
    if (ending.empty()) {
        AppendCardinal(value, out);
        return true;
    }
    const bool feminine = ending == "re" || ending == "ère" || ending == "ere";
    const bool ordinal = feminine || ending == "e" || ending == "er" ||
                         ending == "ème" || ending == "eme" || ending == "es";
    if (value == 2 && (ending == "nd" || ending == "nde")) {
        out.push_back(ending == "nde" ? "seconde" : "second");
        return true;
    }
    if (!ordinal) return false;
    if (value == 1) {
        out.push_back(feminine ? "première" : "premier");
        return true;
    }
    std::vector<std::string> words;
    AppendCardinal(value, words);
    std::string& last = words.back();
    if (last == "cinq") {
        last = "cinquième";
    } else if (last == "neuf") {
        last = "neuvième";
    } else if (last == "vingts" || last == "cents") {
        last.pop_back();
        last += "ième";
    } else {
        if (!last.empty() && last.back() == 'e') last.pop_back();
        last += "ième";
    }
    out.insert(out.end(), words.begin(), words.end());
    return true;
}

// BENCH-2026-09 -- folded tokens of a written word: lowercase, without
// punctuation, split on spaces, apostrophes and hyphens (see
// MatchHeardWord). A byte that is not valid UTF-8 is kept as a letter so no
// text disappears from the comparison.
std::vector<std::string> FoldedTokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(std::move(current));
        }
        current.clear();
    };
    for (size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        size_t length = 1;
        uint32_t codePoint = lead;
        if (lead >= 0xC0 && lead < 0xE0) {
            length = 2;
            codePoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead < 0xF0) {
            length = 3;
            codePoint = lead & 0x0F;
        } else if (lead >= 0xF0 && lead < 0xF8) {
            length = 4;
            codePoint = lead & 0x07;
        } else if (lead >= 0x80) {
            length = 0;
        }
        for (size_t offset = 1; length > 1 && offset < length; ++offset) {
            const auto next =
                index + offset < text.size()
                    ? static_cast<unsigned char>(text[index + offset])
                    : 0;
            if ((next & 0xC0) != 0x80) {
                length = 0;
                break;
            }
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        if (length == 0) {
            current.push_back(text[index]);
            ++index;
            continue;
        }
        index += length;
        if (SplitsTokens(codePoint)) {
            flush();
        } else if (!IsWordPunctuation(codePoint)) {
            AppendUtf8(LowercaseLetter(codePoint), current);
        }
    }
    flush();
    // AGENT-2026-09 A13 -- a number written in digits is folded to the
    // words a reading writes for it ("3ème" to "troisième"), so the two
    // spellings are one word.
    std::vector<std::string> expanded;
    for (const std::string& token : tokens) {
        if (!ExpandNumberToken(token, expanded)) expanded.push_back(token);
    }
    return expanded;
}

size_t CodePointCount(const std::string& text, size_t bytes) {
    size_t count = 0;
    for (size_t index = 0; index < bytes && index < text.size(); ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// The endings the inflection rule strips: gender and number, nothing else.
constexpr std::array<const char*, 5> kInflectionEndings = {"", "e", "s", "es",
                                                           "x"};
constexpr size_t kInflectionStemLetters = 3;

// AGENT-2026-09 A13 -- the plural of -al: "spécial" read back "spéciaux"
// dropped a caption word on a real montage. Only that ending pair, on a
// stem of three letters: "travail"/"travaux" (-ail) still count as two
// words, as the table in interview_short_tests.cc records.
bool PluralOfAl(const std::string& left, const std::string& right) {
    const auto ends = [](const std::string& text, const std::string& ending) {
        return text.size() > ending.size() &&
               text.compare(text.size() - ending.size(), ending.size(),
                            ending) == 0;
    };
    const std::string* singular = nullptr;
    const std::string* plural = nullptr;
    if (ends(left, "al") && ends(right, "aux")) {
        singular = &left;
        plural = &right;
    } else if (ends(right, "al") && ends(left, "aux")) {
        singular = &right;
        plural = &left;
    } else {
        return false;
    }
    const size_t stemBytes = singular->size() - 2;
    return CodePointCount(*singular, stemBytes) >= kInflectionStemLetters &&
           plural->size() == stemBytes + 3 &&
           plural->compare(0, stemBytes, *singular, 0, stemBytes) == 0;
}

bool InflectedTokens(const std::string& left, const std::string& right) {
    if (PluralOfAl(left, right)) return true;
    for (const char* leftEnding : kInflectionEndings) {
        const std::string ending(leftEnding);
        if (left.size() < ending.size() ||
            left.compare(left.size() - ending.size(), ending.size(), ending) !=
                0) {
            continue;
        }
        const size_t stemBytes = left.size() - ending.size();
        if (CodePointCount(left, stemBytes) < kInflectionStemLetters) {
            continue;
        }
        for (const char* rightEnding : kInflectionEndings) {
            const std::string other(rightEnding);
            if (right.size() == stemBytes + other.size() &&
                right.compare(0, stemBytes, left, 0, stemBytes) == 0 &&
                right.compare(stemBytes, other.size(), other) == 0) {
                return true;
            }
        }
    }
    return false;
}

HeardWordMatch MatchFoldedTokens(const std::vector<std::string>& written,
                                 const std::vector<std::string>& heard) {
    HeardWordMatch best = HeardWordMatch::None;
    for (const std::string& left : written) {
        for (const std::string& right : heard) {
            if (left == right) {
                return HeardWordMatch::Exact;
            }
            if (best == HeardWordMatch::None && InflectedTokens(left, right)) {
                best = HeardWordMatch::Inflected;
            }
        }
    }
    return best;
}

// One word as the montage plays it or a reading writes it.
struct TimedTokens {
    RationalTime start;
    std::vector<std::string> tokens;
    std::string text;
};

// Each token of `words` as its own entry, in order, with its word's start
// and text.
std::vector<TimedTokens> SplitTokens(const std::vector<TimedTokens>& words) {
    std::vector<TimedTokens> split;
    for (const TimedTokens& word : words) {
        for (const std::string& token : word.tokens) {
            split.push_back({word.start, {token}, word.text});
        }
    }
    return split;
}

// BENCH-2026-09 -- the reading entries a montage entry accounts for (words
// or single tokens). Pairs of a montage entry and a reading entry that agree
// (MatchHeardWord) and lie within `tolerance` of each other are chained so
// that both orders are kept, and the longest chain wins: a longest common
// subsequence over time-plausible pairs, computed as a longest increasing
// subsequence. A reading entry outside the chain is one the montage does not
// explain.
std::vector<bool> AccountedReadingWords(const std::vector<TimedTokens>& placed,
                                        const std::vector<TimedTokens>& heard,
                                        const RationalTime& tolerance) {
    std::vector<std::pair<size_t, size_t>> pairs;
    size_t low = 0;
    for (size_t left = 0; left < placed.size(); ++left) {
        while (low < heard.size() &&
               placed[left].start.sub(heard[low].start) > tolerance) {
            ++low;
        }
        std::vector<size_t> matches;
        for (size_t right = low;
             right < heard.size() &&
             heard[right].start.sub(placed[left].start) <= tolerance;
             ++right) {
            if (MatchFoldedTokens(placed[left].tokens, heard[right].tokens) !=
                HeardWordMatch::None) {
                matches.push_back(right);
            }
        }
        // Descending, so a strictly increasing chain takes one per word.
        for (auto match = matches.rbegin(); match != matches.rend(); ++match) {
            pairs.emplace_back(left, *match);
        }
    }
    std::vector<size_t> tails;
    std::vector<size_t> previous(pairs.size(),
                                 std::numeric_limits<size_t>::max());
    for (size_t pair = 0; pair < pairs.size(); ++pair) {
        const auto position =
            std::lower_bound(tails.begin(), tails.end(), pairs[pair].second,
                             [&](size_t tail, size_t value) {
                                 return pairs[tail].second < value;
                             });
        if (position != tails.begin()) {
            previous[pair] = *(position - 1);
        }
        if (position == tails.end()) {
            tails.push_back(pair);
        } else {
            *position = pair;
        }
    }
    std::vector<bool> accounted(heard.size(), false);
    if (tails.empty()) {
        return accounted;
    }
    for (size_t pair = tails.back(); pair != std::numeric_limits<size_t>::max();
         pair = previous[pair]) {
        accounted[pairs[pair].second] = true;
    }
    return accounted;
}

size_t FindRoot(std::vector<size_t>& parents, size_t index) {
    while (parents[index] != index) {
        parents[index] = parents[parents[index]];
        index = parents[index];
    }
    return index;
}

// BENCH-2026-09 -- how far from a clip edge, in source time, a removed word
// can still be one the aligner misplaced. The two words lost on the bench run
// sat in a hesitation and a closed pause right against the kept material;
// the reach matches the reading tolerance so the two stay one scale.
constexpr int64_t kRemovedWordReachSeconds = 2;

RationalTime Distance(const RationalTime& left, const RationalTime& right) {
    return left < right ? right.sub(left) : left.sub(right);
}

// BENCH-2026-09 -- words the aligner put in removed material next to a clip
// edge that every reading still hears there (see CutHeardWord). `played`
// says whether a source word overlaps any audible clip; `audibleClips` hold
// only the words kept after the heard check.
template <typename Played>
void FindCutHeardWords(const std::vector<AudibleClip>& audibleClips,
                       const std::map<Ulid, Transcript>& transcripts,
                       const std::vector<std::vector<TimedTokens>>& heardWords,
                       const Played& played, const RationalTime& tolerance,
                       std::vector<CutHeardWord>& found) {
    found.clear();
    if (heardWords.empty()) {
        return;
    }
    std::vector<TimedTokens> placed;
    for (const AudibleClip& audible : audibleClips) {
        const DocumentClip& clip = *audible.clip;
        for (const auto& selected : audible.selected_words) {
            const TranscriptWord& word =
                audible.transcript->words[selected.first];
            const RationalTime sourceStart =
                word.start < clip.source_in ? clip.source_in : word.start;
            placed.push_back(
                {clip.timeline_in.add(sourceStart.sub(clip.source_in)),
                 FoldedTokens(word.text), word.text});
        }
    }
    std::stable_sort(placed.begin(), placed.end(),
                     [](const TimedTokens& left, const TimedTokens& right) {
                         return left.start < right.start;
                     });
    // Aligned token by token: Whisper writes "à la" or "et créer" as one
    // word, and on the bench run a kept "la" explained the whole "à la" and
    // hid the "à" the aligner had put in the cut.
    const std::vector<TimedTokens> placedTokens = SplitTokens(placed);
    std::vector<std::vector<TimedTokens>> heardTokens;
    std::vector<std::vector<bool>> unexplained;
    for (const std::vector<TimedTokens>& words : heardWords) {
        heardTokens.push_back(SplitTokens(words));
        std::vector<bool> accounted =
            AccountedReadingWords(placedTokens, heardTokens.back(), tolerance);
        accounted.flip();
        unexplained.push_back(std::move(accounted));
    }

    struct Candidate {
        size_t clip_index = 0;
        Ulid source_id;
        const TranscriptWord* word = nullptr;
        std::vector<std::string> tokens;
        RationalTime distance;
        RationalTime edge;
    };
    const RationalTime reach{kRemovedWordReachSeconds, 1};
    std::vector<Candidate> candidates;
    for (const auto& entry : transcripts) {
        const std::vector<TranscriptWord>& words = entry.second.words;
        for (size_t wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
            if (played(entry.first, wordIndex)) {
                continue;
            }
            const TranscriptWord& word = words[wordIndex];
            std::optional<Candidate> best;
            for (size_t clipIndex = 0; clipIndex < audibleClips.size();
                 ++clipIndex) {
                const DocumentClip& clip = *audibleClips[clipIndex].clip;
                if (clip.source_id != entry.first) {
                    continue;
                }
                const RationalTime clipEnd = clip.source_in.add(clip.duration);
                Candidate candidate;
                if (word.start >= clipEnd) {
                    candidate.distance = word.start.sub(clipEnd);
                    candidate.edge = clip.timeline_in.add(clip.duration);
                } else if (word.end <= clip.source_in) {
                    candidate.distance = clip.source_in.sub(word.end);
                    candidate.edge = clip.timeline_in;
                } else {
                    continue;
                }
                if (candidate.distance > reach) {
                    continue;
                }
                if (best && (best->distance < candidate.distance ||
                             (best->distance == candidate.distance &&
                              best->edge <= candidate.edge))) {
                    continue;
                }
                candidate.clip_index = clipIndex;
                best = std::move(candidate);
            }
            if (!best) {
                continue;
            }
            best->tokens = FoldedTokens(word.text);
            if (best->tokens.empty()) {
                continue;
            }
            best->source_id = entry.first;
            best->word = &word;
            candidates.push_back(std::move(*best));
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& left, const Candidate& right) {
                         if (left.edge != right.edge) {
                             return left.edge < right.edge;
                         }
                         return left.distance < right.distance;
                     });

    // Every reading must hear each token of the word near the edge, in
    // tokens the montage's own words leave unexplained. One reading token
    // explains one removed token at most.
    for (const Candidate& candidate : candidates) {
        std::vector<std::vector<size_t>> picks;
        for (size_t reading = 0; reading < heardTokens.size(); ++reading) {
            const std::vector<TimedTokens>& tokens = heardTokens[reading];
            const RationalTime windowStart = candidate.edge.sub(tolerance);
            const auto first = std::lower_bound(
                tokens.begin(), tokens.end(), windowStart,
                [](const TimedTokens& token, const RationalTime& start) {
                    return token.start < start;
                });
            std::vector<size_t> chosen;
            for (const std::string& token : candidate.tokens) {
                std::optional<size_t> pick;
                for (auto index = static_cast<size_t>(first - tokens.begin());
                     index < tokens.size() &&
                     tokens[index].start.sub(candidate.edge) <= tolerance;
                     ++index) {
                    if (!unexplained[reading][index] ||
                        std::find(chosen.begin(), chosen.end(), index) !=
                            chosen.end() ||
                        MatchFoldedTokens({token}, tokens[index].tokens) ==
                            HeardWordMatch::None) {
                        continue;
                    }
                    if (!pick ||
                        Distance(tokens[index].start, candidate.edge) <
                            Distance(tokens[*pick].start, candidate.edge)) {
                        pick = index;
                    }
                }
                if (!pick) {
                    break;
                }
                chosen.push_back(*pick);
            }
            if (chosen.size() != candidate.tokens.size()) {
                break;
            }
            picks.push_back(std::move(chosen));
        }
        if (picks.size() != heardTokens.size()) {
            continue;
        }
        CutHeardWord word;
        word.clip_id = audibleClips[candidate.clip_index].clip->id;
        word.source_id = candidate.source_id;
        word.text = candidate.word->text;
        word.source_in = candidate.word->start;
        word.duration = candidate.word->end.sub(candidate.word->start);
        word.timeline_in = candidate.edge;
        for (size_t reading = 0; reading < picks.size(); ++reading) {
            for (const size_t index : picks[reading]) {
                unexplained[reading][index] = false;
            }
            word.heard_as.push_back(
                heardTokens[reading][picks[reading].front()].text);
        }
        found.push_back(std::move(word));
    }
}

}  // namespace

namespace {

bool BuildTimelineTranscriptSpansImpl(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    bool wordByWord, std::vector<TimelineTranscriptSpan>& spans,
    std::string& error, TimelineWordCheck* check) {
    spans.clear();
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::map<Ulid, Transcript> transcripts;
    std::vector<AudibleClip> audibleClips;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip says nothing in the film (ClipEnabled.h).
            if (!ClipIsHeard(clip)) continue;
            auto found = transcripts.find(clip.source_id);
            if (found == transcripts.end()) {
                Transcript transcript;
                std::string loadError;
                if (!LoadAudioTranscript(
                        (transcriptDirectory / (clip.source_id + ".json"))
                            .string(),
                        transcript, loadError))
                    continue;
                ApplyNewestWordAlignment(transcriptDirectory.parent_path() /
                                             "word-alignments" / clip.source_id,
                                         transcript);
                found =
                    transcripts.emplace(clip.source_id, std::move(transcript))
                        .first;
            }
            audibleClips.push_back({&clip, &found->second, {}});
        }
    }

    if (check != nullptr) {
        *check = {};
        for (const auto& entry : transcripts)
            check->word_timing.emplace_back(entry.first,
                                            entry.second.word_timing);
    }

    // B6 -- assign every played word occurrence to exactly one clip before
    // cue segmentation. Segmenting each clip independently duplicates a word
    // that crosses a cut; filtering each independently can also drop it.
    //
    // ITW3 -- ROADMAP.md. The clip is the one that plays the word's start,
    // so a subtitle shows a word when it begins to be heard. A word whose
    // start no clip plays was cut into: it goes to the clip that plays the
    // rest of it. The greatest-overlap rule this replaces showed a word cut
    // between two contiguous clips on the second one, frames after it began.
    const auto startIsPlayed = [&](const Ulid& sourceId,
                                   const RationalTime& start) {
        return std::any_of(audibleClips.begin(), audibleClips.end(),
                           [&](const AudibleClip& audible) {
                               return audible.clip->source_id == sourceId &&
                                      audible.clip->source_in <= start &&
                                      start < audible.clip->source_in.add(
                                                  audible.clip->duration);
                           });
    };
    using WordKey = std::pair<Ulid, size_t>;
    std::map<WordKey, std::vector<WordCandidate>> candidatesByWord;
    std::map<WordKey, bool> startPlayed;
    for (size_t clipIndex = 0; clipIndex < audibleClips.size(); ++clipIndex) {
        const DocumentClip& clip = *audibleClips[clipIndex].clip;
        const Transcript& transcript = *audibleClips[clipIndex].transcript;
        const RationalTime clipEnd = clip.source_in.add(clip.duration);
        for (size_t wordIndex = 0; wordIndex < transcript.words.size();
             ++wordIndex) {
            const TranscriptWord& word = transcript.words[wordIndex];
            if (word.end <= clip.source_in || word.start >= clipEnd) continue;
            const WordKey key{clip.source_id, wordIndex};
            auto played = startPlayed.find(key);
            if (played == startPlayed.end())
                played =
                    startPlayed
                        .emplace(key, startIsPlayed(clip.source_id, word.start))
                        .first;
            const bool containsStart = clip.source_in <= word.start;
            if (played->second && !containsStart) continue;
            const RationalTime sourceStart =
                word.start < clip.source_in ? clip.source_in : word.start;
            const RationalTime sourceEnd =
                word.end > clipEnd ? clipEnd : word.end;
            if (sourceEnd <= sourceStart) continue;
            const RationalTime timelineStart =
                clip.timeline_in.add(sourceStart.sub(clip.source_in));
            const RationalTime timelineEnd =
                clip.timeline_in.add(sourceEnd.sub(clip.source_in));
            candidatesByWord[{clip.source_id, wordIndex}].push_back(
                {clipIndex, wordIndex, sourceStart, sourceEnd, timelineStart,
                 timelineEnd, sourceEnd.sub(sourceStart)});
        }
    }

    for (auto& entry : candidatesByWord) {
        std::vector<WordCandidate>& candidates = entry.second;
        std::vector<size_t> parents(candidates.size());
        std::iota(parents.begin(), parents.end(), 0);
        for (size_t left = 0; left < candidates.size(); ++left) {
            for (size_t right = left + 1; right < candidates.size(); ++right) {
                if (!SamePlayedOccurrence(candidates[left], candidates[right]))
                    continue;
                const size_t leftRoot = FindRoot(parents, left);
                const size_t rightRoot = FindRoot(parents, right);
                if (leftRoot != rightRoot) parents[rightRoot] = leftRoot;
            }
        }
        std::map<size_t, std::vector<size_t>> clusters;
        for (size_t index = 0; index < candidates.size(); ++index)
            clusters[FindRoot(parents, index)].push_back(index);
        for (const auto& cluster : clusters) {
            // Only clips that play the start, or all the clips that play a
            // cut-into word, reach this point: the earliest one wins.
            size_t winner = cluster.second.front();
            for (const size_t index : cluster.second) {
                if (candidates[index].timeline_start <
                    candidates[winner].timeline_start) {
                    winner = index;
                }
            }
            const WordCandidate& chosen = candidates[winner];
            const AudibleClip& audible = audibleClips[chosen.clip_index];
            const TranscriptWord& word =
                audible.transcript->words[chosen.word_index];
            const RationalTime clipEnd =
                audible.clip->source_in.add(audible.clip->duration);
            const bool straddles =
                word.start < audible.clip->source_in || word.end > clipEnd;
            audibleClips[chosen.clip_index].selected_words.push_back(
                {chosen.word_index, straddles});
        }
    }

    // ITW3 -- keep only the words a reading of the montage audio contains.
    std::vector<Transcript> readings;
    TimelineAudioPlan plan;
    std::string planError;
    const std::filesystem::path cacheDirectory =
        transcriptDirectory.parent_path();
    if (BuildTimelineAudioPlan(
            document, cacheDirectory.parent_path() / "project.cutmachine.json",
            plan, planError)) {
        for (const bool verbatim : {false, true}) {
            Transcript reading;
            std::string readingError;
            if (LoadAudioTranscript(
                    TimelineTranscriptCachePath(cacheDirectory,
                                                document.sequence.id, verbatim)
                        .string(),
                    reading, readingError) &&
                reading.media_id == plan.cache_identity)
                readings.push_back(std::move(reading));
        }
    }
    if (check != nullptr)
        check->readings = static_cast<int32_t>(readings.size());
    if (!readings.empty()) {
        const RationalTime tolerance{kHeardToleranceSeconds, 1};
        // Each reading's words, folded once and in time order.
        std::vector<std::vector<TimedTokens>> heardWords;
        for (const Transcript& reading : readings) {
            std::vector<TimedTokens> words;
            for (const TranscriptWord& word : reading.words) {
                std::vector<std::string> tokens = FoldedTokens(word.text);
                if (!tokens.empty()) {
                    words.push_back({word.start, std::move(tokens), word.text});
                }
            }
            std::stable_sort(
                words.begin(), words.end(),
                [](const TimedTokens& left, const TimedTokens& right) {
                    return left.start < right.start;
                });
            heardWords.push_back(std::move(words));
        }
        // The best agreement any reading gives within the tolerance, and the
        // spelling of the first inflected one.
        const auto heard = [&](const std::vector<std::string>& tokens,
                               const RationalTime& timelineStart,
                               std::string& heardAs) {
            HeardWordMatch best = HeardWordMatch::None;
            for (const std::vector<TimedTokens>& words : heardWords) {
                for (const TimedTokens& word : words) {
                    if (word.start.sub(timelineStart) > tolerance) break;
                    if (timelineStart.sub(word.start) > tolerance) continue;
                    const HeardWordMatch match =
                        MatchFoldedTokens(tokens, word.tokens);
                    if (match == HeardWordMatch::Exact) {
                        return match;
                    }
                    if (match == HeardWordMatch::Inflected &&
                        best == HeardWordMatch::None) {
                        best = match;
                        heardAs = word.text;
                    }
                }
            }
            return best;
        };
        for (AudibleClip& audible : audibleClips) {
            const DocumentClip& clip = *audible.clip;
            const RationalTime clipEnd = clip.source_in.add(clip.duration);
            std::vector<std::pair<size_t, bool>> kept;
            for (const auto& selected : audible.selected_words) {
                const TranscriptWord& word =
                    audible.transcript->words[selected.first];
                const RationalTime sourceStart =
                    word.start < clip.source_in ? clip.source_in : word.start;
                const RationalTime sourceEnd =
                    word.end > clipEnd ? clipEnd : word.end;
                const RationalTime timelineStart =
                    clip.timeline_in.add(sourceStart.sub(clip.source_in));
                const std::vector<std::string> tokens = FoldedTokens(word.text);
                // A word with no letters (a lone "...") cannot be checked.
                std::string heardAs;
                const HeardWordMatch match =
                    tokens.empty() ? HeardWordMatch::Exact
                                   : heard(tokens, timelineStart, heardAs);
                if (match != HeardWordMatch::None) {
                    kept.push_back(selected);
                    if (match == HeardWordMatch::Inflected &&
                        check != nullptr) {
                        check->inflected.push_back(
                            {clip.id, word.text, heardAs, timelineStart});
                    }
                } else if (check != nullptr) {
                    check->unheard.push_back(
                        {clip.id, clip.source_id, word.text, sourceStart,
                         timelineStart, sourceEnd.sub(sourceStart)});
                }
            }
            audible.selected_words = std::move(kept);
        }
        if (check != nullptr) {
            std::stable_sort(
                check->unheard.begin(), check->unheard.end(),
                [](const UnheardWord& left, const UnheardWord& right) {
                    return left.timeline_in < right.timeline_in;
                });
            std::stable_sort(
                check->inflected.begin(), check->inflected.end(),
                [](const InflectedWord& left, const InflectedWord& right) {
                    return left.timeline_in < right.timeline_in;
                });
            FindCutHeardWords(
                audibleClips, transcripts, heardWords,
                [&](const Ulid& sourceId, size_t wordIndex) {
                    return candidatesByWord.count({sourceId, wordIndex}) != 0;
                },
                tolerance, check->cut_but_heard);
        }
    }

    for (AudibleClip& audible : audibleClips) {
        const DocumentClip& clip = *audible.clip;
        std::sort(audible.selected_words.begin(), audible.selected_words.end());
        Transcript selected = *audible.transcript;
        selected.words.clear();
        for (const auto& word : audible.selected_words)
            selected.words.push_back(audible.transcript->words[word.first]);
        if (selected.words.empty()) continue;
        std::vector<SubtitleCue> cues;
        std::string cueError;
        const bool built =
            wordByWord ? WordSubtitleCuesForClip(selected, clip, cues, cueError)
                       : SubtitleCuesForClip(selected, clip, cues, cueError);
        if (!built) continue;
        for (const SubtitleCue& cue : cues) {
            TimelineTranscriptSpan span;
            span.source_id = clip.source_id;
            const RationalTime offset = cue.timeline_in.sub(clip.timeline_in);
            span.source_in = clip.source_in.add(offset);
            span.duration = cue.duration;
            span.timeline_in = cue.timeline_in;
            span.text = cue.text;
            span.likely_hallucinated = audible.transcript->likely_hallucinated;
            span.likely_incomplete = audible.transcript->likely_incomplete;
            const RationalTime cueEnd = span.source_in.add(span.duration);
            for (const auto& selectedWord : audible.selected_words) {
                if (!selectedWord.second) continue;
                const TranscriptWord& word =
                    audible.transcript->words[selectedWord.first];
                if (word.start < cueEnd && word.end > span.source_in) {
                    span.straddles_cut = true;
                    break;
                }
            }
            spans.push_back(std::move(span));
        }
    }
    std::stable_sort(spans.begin(), spans.end(),
                     [](const TimelineTranscriptSpan& left,
                        const TimelineTranscriptSpan& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    for (size_t index = 0; index < spans.size(); ++index)
        spans[index].span_id = "S" + std::to_string(index + 1);
    if (spans.empty()) {
        error = "no cached transcript spans for the active audio tracks";
        return false;
    }
    error.clear();
    return true;
}

}  // namespace

HeardWordMatch MatchHeardWord(const std::string& written,
                              const std::string& heard) {
    return MatchFoldedTokens(FoldedTokens(written), FoldedTokens(heard));
}

bool BuildTimelineTranscriptSpans(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::vector<TimelineTranscriptSpan>& spans, std::string& error,
    TimelineWordCheck* check) {
    return BuildTimelineTranscriptSpansImpl(document, transcriptDirectory,
                                            false, spans, error, check);
}

bool BuildTimelineTranscriptWordSpans(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::vector<TimelineTranscriptSpan>& spans, std::string& error,
    TimelineWordCheck* check) {
    return BuildTimelineTranscriptSpansImpl(document, transcriptDirectory, true,
                                            spans, error, check);
}

std::string SerializeTimelineTranscriptSpans(
    const Document& document, const std::vector<TimelineTranscriptSpan>& spans,
    const TimelineWordCheck* check) {
    mcp_json::Value list = mcp_json::Value::MakeArray();
    for (const TimelineTranscriptSpan& span : spans) {
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("span_id", mcp_json::Value::MakeString(span.span_id));
        value.Set("source_id", mcp_json::Value::MakeString(span.source_id));
        value.Set("source_in", TimeValue(span.source_in));
        value.Set("duration", TimeValue(span.duration));
        value.Set("timeline_in", TimeValue(span.timeline_in));
        value.Set(
            "approx_seconds",
            mcp_json::Value::MakeDouble(
                static_cast<double>(span.duration.value) / span.duration.rate));
        value.Set("text", mcp_json::Value::MakeString(span.text));
        value.Set("straddles_cut",
                  mcp_json::Value::MakeBool(span.straddles_cut));
        value.Set("likely_hallucinated",
                  mcp_json::Value::MakeBool(span.likely_hallucinated));
        value.Set("likely_incomplete",
                  mcp_json::Value::MakeBool(span.likely_incomplete));
        list.Push(std::move(value));
    }
    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("timeline_id", mcp_json::Value::MakeString(document.sequence.id));
    root.Set("timeline_name",
             mcp_json::Value::MakeString(document.sequence.name));
    root.Set("spans", std::move(list));
    if (check != nullptr) {
        mcp_json::Value timing = mcp_json::Value::MakeObject();
        for (const auto& entry : check->word_timing)
            timing.Set(entry.first, mcp_json::Value::MakeString(entry.second));
        root.Set("word_timing", std::move(timing));
        root.Set("word_check", WordCheckValue(*check));
    }
    return root.Dump();
}

std::string SerializeTimelineWordCheck(const TimelineWordCheck& check) {
    return WordCheckValue(check).Dump();
}

std::vector<CaptionSpeechGap> FindCaptionSpeechGaps(
    const std::vector<std::pair<RationalTime, RationalTime>>& cartons,
    const TimelineWordCheck& check) {
    std::vector<CaptionSpeechGap> gaps;
    const auto overlaps = [](const RationalTime& start, const RationalTime& end,
                             const RationalTime& gapStart,
                             const RationalTime& gapEnd) {
        return start < gapEnd && gapStart < end;
    };
    for (size_t index = 0; index + 1 < cartons.size(); ++index) {
        const RationalTime gapStart =
            cartons[index].first.add(cartons[index].second);
        const RationalTime gapEnd = cartons[index + 1].first;
        if (gapEnd <= gapStart) {
            continue;
        }
        CaptionSpeechGap gap;
        gap.after = index;
        gap.timeline_in = gapStart;
        gap.duration = gapEnd.sub(gapStart);
        for (const UnheardWord& word : check.unheard) {
            if (overlaps(word.timeline_in, word.timeline_in.add(word.duration),
                         gapStart, gapEnd)) {
                gap.words.push_back({word.text, word.timeline_in, "unheard"});
            }
        }
        for (const CutHeardWord& word : check.cut_but_heard) {
            if (gapStart <= word.timeline_in && word.timeline_in <= gapEnd) {
                gap.words.push_back(
                    {word.text, word.timeline_in, "aligned_in_removed_range"});
            }
        }
        if (gap.words.empty()) {
            continue;
        }
        std::stable_sort(
            gap.words.begin(), gap.words.end(),
            [](const CaptionGapWord& left, const CaptionGapWord& right) {
                return left.timeline_in < right.timeline_in;
            });
        gaps.push_back(std::move(gap));
    }
    return gaps;
}

std::string SerializeCaptionSpeechGaps(
    const std::vector<CaptionSpeechGap>& gaps) {
    mcp_json::Value list = mcp_json::Value::MakeArray();
    for (const CaptionSpeechGap& gap : gaps) {
        mcp_json::Value item = mcp_json::Value::MakeObject();
        item.Set("after_carton",
                 mcp_json::Value::MakeInt(static_cast<int64_t>(gap.after)));
        item.Set("timeline_in", TimeValue(gap.timeline_in));
        item.Set("duration", TimeValue(gap.duration));
        item.Set("approx_seconds", mcp_json::Value::MakeDouble(
                                       static_cast<double>(gap.duration.value) /
                                       gap.duration.rate));
        mcp_json::Value words = mcp_json::Value::MakeArray();
        for (const CaptionGapWord& word : gap.words) {
            mcp_json::Value entry = mcp_json::Value::MakeObject();
            entry.Set("text", mcp_json::Value::MakeString(word.text));
            entry.Set("timeline_in", TimeValue(word.timeline_in));
            entry.Set("reason", mcp_json::Value::MakeString(word.reason));
            words.Push(std::move(entry));
        }
        item.Set("words", std::move(words));
        list.Push(std::move(item));
    }
    return list.Dump();
}

bool ParseTimelineTranscriptSpans(const std::string& json,
                                  std::vector<TimelineTranscriptSpan>& spans,
                                  std::string& error) {
    spans.clear();
    mcp_json::Value root;
    std::string parseError;
    if (!mcp_json::Value::Parse(json, root, parseError) || !root.IsObject()) {
        error = "malformed transcript span view: " + parseError;
        return false;
    }
    const mcp_json::Value* list = root.Find("spans");
    if (list == nullptr || !list->IsArray()) {
        error = "transcript span view has no spans";
        return false;
    }
    for (const mcp_json::Value& value : list->AsArray()) {
        TimelineTranscriptSpan span;
        const mcp_json::Value* spanId = value.Find("span_id");
        const mcp_json::Value* sourceId = value.Find("source_id");
        const mcp_json::Value* text = value.Find("text");
        const mcp_json::Value* straddlesCut = value.Find("straddles_cut");
        const mcp_json::Value* likelyHallucinated =
            value.Find("likely_hallucinated");
        const mcp_json::Value* likelyIncomplete =
            value.Find("likely_incomplete");
        if (spanId == nullptr || !spanId->IsString() || sourceId == nullptr ||
            !sourceId->IsString() ||
            !ReadTimeValue(value.Find("source_in"), span.source_in) ||
            !ReadTimeValue(value.Find("duration"), span.duration) ||
            !ReadTimeValue(value.Find("timeline_in"), span.timeline_in)) {
            error = "transcript span view contains an invalid span";
            return false;
        }
        span.span_id = spanId->AsString();
        span.source_id = sourceId->AsString();
        if (text != nullptr && text->IsString()) span.text = text->AsString();
        if (straddlesCut != nullptr) {
            if (!straddlesCut->IsBool()) {
                error =
                    "transcript span view contains an invalid "
                    "straddles_cut flag";
                return false;
            }
            span.straddles_cut = straddlesCut->AsBool();
        }
        if (likelyHallucinated != nullptr) {
            if (!likelyHallucinated->IsBool()) {
                error =
                    "transcript span view contains an invalid "
                    "likely_hallucinated flag";
                return false;
            }
            span.likely_hallucinated = likelyHallucinated->AsBool();
        }
        if (likelyIncomplete != nullptr) {
            if (!likelyIncomplete->IsBool()) {
                error =
                    "transcript span view contains an invalid "
                    "likely_incomplete flag";
                return false;
            }
            span.likely_incomplete = likelyIncomplete->AsBool();
        }
        spans.push_back(std::move(span));
    }
    error.clear();
    return true;
}

bool DescribeTimelineTranscriptForAgent(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::string& json, std::string& error) {
    std::vector<TimelineTranscriptSpan> spans;
    TimelineWordCheck check;
    if (!BuildTimelineTranscriptSpans(document, transcriptDirectory, spans,
                                      error, &check)) {
        return false;
    }
    json = SerializeTimelineTranscriptSpans(document, spans, &check);
    error.clear();
    return true;
}

bool ResolveTranscriptSpanRange(
    const std::vector<TimelineTranscriptSpan>& spans,
    const std::string& startSpanId, const std::string& endSpanId,
    Ulid& sourceId, RationalTime& sourceIn, RationalTime& duration,
    std::string& error) {
    const auto find = [&](const std::string& id) {
        return std::find_if(spans.begin(), spans.end(),
                            [&](const TimelineTranscriptSpan& span) {
                                return span.span_id == id;
                            });
    };
    const auto first = find(startSpanId);
    if (first == spans.end()) {
        error = "unknown span_id '" + startSpanId + "'";
        return false;
    }
    auto last = first;
    if (!endSpanId.empty()) {
        last = find(endSpanId);
        if (last == spans.end()) {
            error = "unknown end_span_id '" + endSpanId + "'";
            return false;
        }
        if (last < first) {
            error = "end_span_id '" + endSpanId +
                    "' comes before start span_id '" + startSpanId + "'";
            return false;
        }
    }
    // Every span in the run must belong to one source and butt against its
    // predecessor. Anything else is a gap the caller did not name, and
    // stitching over it would invent an edit.
    //
    // PRD40 -- ROADMAP.md. Every break of the run is named, with the
    // segments that would select it: the first one alone was, and an editor
    // of the 3 October series needed three refusals (8, then 14, then 16
    // segments) to find the breaks of one selection a turn at a time.
    std::vector<std::string> breaks;
    std::string pieces;
    auto piece = first;
    const auto closePiece = [&](auto end) {
        pieces += (pieces.empty() ? "'" : ", '") + piece->span_id + "'";
        if (end != piece) pieces += " to '" + end->span_id + "'";
    };
    for (auto span = first; span != last; ++span) {
        const auto next = span + 1;
        if (next->source_id != span->source_id) {
            breaks.push_back("spans '" + span->span_id + "' and '" +
                             next->span_id +
                             "' come from different sources and cannot be "
                             "merged");
            closePiece(span);
            piece = next;
            continue;
        }
        // Two cues are usually flush, but a cue also breaks on a pause
        // between words, and that pause then belongs to neither of them. A
        // run that steps over one of those is still one continuous stretch
        // of the source -- merging it takes the breath along, which is what
        // a cut through a sentence has to do. Refusing it, as an earlier
        // version of this function did, made ordinary sentences
        // unselectable: measured on a real interview, three of eight
        // editorial beats were rejected over gaps of one and two frames.
        //
        // What must stay refused is a run that steps over a real editorial
        // gap, because merging that would silently splice out whatever sits
        // between. The line between the two is the cue segmenter's own
        // maximum gap, taken from the same constant so the two cannot drift.
        const RationalTime gap =
            next->source_in.sub(span->source_in.add(span->duration));
        if (gap.compare(RationalTime{0, 1}) < 0 ||
            gap.compare(kSubtitleCueMaximumGap) >= 0) {
            breaks.push_back("spans '" + span->span_id + "' and '" +
                             next->span_id +
                             "' are separated by more than a breath");
            closePiece(span);
            piece = next;
        }
    }
    if (!breaks.empty()) {
        closePiece(last);
        error.clear();
        for (const std::string& item : breaks) {
            error += (error.empty() ? "" : "; ") + item;
        }
        error += " -- select the run as separate segments: " + pieces;
        return false;
    }
    sourceId = first->source_id;
    sourceIn = first->source_in;
    duration = last->source_in.add(last->duration).sub(first->source_in);
    error.clear();
    return true;
}
