#include "Subtitles.h"

#include "ClipEnabled.h"
#include "ClipSpeed.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>

namespace {

std::string Trim(std::string value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

bool ParseTimestamp(std::string value, RationalTime& output) {
    value = Trim(std::move(value));
    if (value.size() != 12 || value[2] != ':' || value[5] != ':' ||
        (value[8] != ',' && value[8] != '.'))
        return false;
    constexpr size_t kDigitIndices[] = {0, 1, 3, 4, 6, 7, 9, 10, 11};
    for (size_t index : kDigitIndices)
        if (!std::isdigit(static_cast<unsigned char>(value[index])))
            return false;
    const int64_t hours = std::stoll(value.substr(0, 2));
    const int64_t minutes = std::stoll(value.substr(3, 2));
    const int64_t seconds = std::stoll(value.substr(6, 2));
    const int64_t milliseconds = std::stoll(value.substr(9, 3));
    if (minutes >= 60 || seconds >= 60) return false;
    output = {((hours * 60 + minutes) * 60 + seconds) * 1000 + milliseconds,
              1000};
    return true;
}

bool ParseTiming(const std::string& line, RationalTime& start,
                 RationalTime& end) {
    const size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    return ParseTimestamp(line.substr(0, arrow), start) &&
           ParseTimestamp(line.substr(arrow + 3), end) && end > start;
}

std::string FormatTimestamp(int64_t milliseconds) {
    int64_t value = milliseconds;
    const int64_t hours = value / 3600000;
    value %= 3600000;
    const int64_t minutes = value / 60000;
    value %= 60000;
    const int64_t seconds = value / 1000;
    value %= 1000;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2)
           << minutes << ':' << std::setw(2) << seconds << ',' << std::setw(3)
           << value;
    return output.str();
}

bool EndsSentence(const std::string& text) {
    if (text.empty()) return false;
    const char last = text.back();
    return last == '.' || last == '?' || last == '!';
}

bool JoinsWithoutSpace(const std::string& text) {
    if (text.empty()) return false;
    constexpr std::string_view kPunctuation = ".,!?;:%)]}";
    return kPunctuation.find(text.front()) != std::string_view::npos;
}

size_t Utf8CharacterCount(const std::string& text) {
    return static_cast<size_t>(std::count_if(
        text.begin(), text.end(),
        [](unsigned char character) { return (character & 0xC0) != 0x80; }));
}

std::string SyntaxWord(std::string text) {
    text = Trim(std::move(text));
    while (!text.empty() &&
           std::ispunct(static_cast<unsigned char>(text.front())) &&
           text.front() != '\'')
        text.erase(text.begin());
    while (!text.empty() &&
           std::ispunct(static_cast<unsigned char>(text.back())) &&
           text.back() != '\'')
        text.pop_back();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return text;
}

bool InWords(const std::string& word, const std::set<std::string_view>& words) {
    return words.find(word) != words.end();
}

bool EndsClause(const std::string& text) {
    if (text.empty()) return false;
    constexpr std::string_view kClausePunctuation = ",;:";
    return kClausePunctuation.find(text.back()) != std::string_view::npos;
}

// The word families the rule reads. RM3 shares them with the learned score
// (SubtitleWordFamilies), so both weigh the same evidence.
//
// RM3 -- "au" and "aux" are articles too, contracted with "à" as "du" and
// "des" are with "de": the author refused, blind on 15 October 2026, a carton
// ending on "aux" before its noun and one ending on "au" before its noun.
const std::set<std::string_view> kDeterminers = {
    "au",  "aux", "ce",   "cet",   "cette", "ces", "des", "du",    "la",   "l'",
    "le",  "les", "leur", "leurs", "ma",    "mes", "mon", "notre", "nos",  "sa",
    "ses", "son", "ta",   "tes",   "ton",   "un",  "une", "vos",   "votre"};
const std::set<std::string_view> kPrepositions = {
    "à",      "avec",     "chez",   "contre", "d'",    "dans", "de",
    "depuis", "derrière", "devant", "en",     "entre", "par",  "parmi",
    "pour",   "sans",     "sous",   "sur",    "vers"};
const std::set<std::string_view> kSubjectPronouns = {
    "c'", "ce",   "elle", "elles", "il", "ils", "je",
    "j'", "nous", "on",   "ça",    "tu", "vous"};
const std::set<std::string_view> kAuxiliaries = {
    "a",    "ai",      "allons",  "as",   "avaient", "avait",  "avons",  "avez",
    "doit", "doivent", "dois",    "est",  "étaient", "était",  "être",   "êtes",
    "ont",  "peut",    "peuvent", "peux", "sera",    "seront", "sommes", "sont",
    "suis", "va",      "vais",    "vas",  "vont"};
const std::set<std::string_view> kConjunctions = {"car", "donc", "et", "mais",
                                                  "ni",  "or",   "ou"};
const std::set<std::string_view> kModifiers = {
    "assez", "beaucoup", "bien",      "complètement", "moins", "petit",
    "plus",  "presque",  "tellement", "très",         "trop",  "vraiment"};
const std::set<std::string_view> kClitics = {
    "en", "l'", "leur", "lui", "m'", "me", "s'", "se", "t'", "te", "y"};
const std::set<std::string_view> kSubordinators = {
    "afin",    "comme", "dont",  "lequel", "lorsque", "où", "parce",
    "puisque", "qu'",   "quand", "que",    "qui",     "si"};

bool IsForbiddenSubtitleBoundary(const std::string& left,
                                 const std::string& right) {
    if (JoinsWithoutSpace(right)) return true;
    const std::string leftWord = SyntaxWord(left);
    const std::string rightWord = SyntaxWord(right);
    if (leftWord.empty() || rightWord.empty()) return false;
    return InWords(leftWord, kDeterminers) ||
           InWords(leftWord, kPrepositions) ||
           InWords(leftWord, kSubjectPronouns) ||
           InWords(leftWord, kAuxiliaries) || leftWord == "ne" ||
           leftWord == "n'" || InWords(leftWord, kConjunctions) ||
           InWords(leftWord, kModifiers) || InWords(leftWord, kClitics);
}

int64_t SubtitleBoundaryCost(const std::string& left, const std::string& right,
                             const RationalTime& gap) {
    const std::string rightWord = SyntaxWord(right);

    int64_t cost = 70;
    if (EndsClause(left))
        cost = 5;
    else if (InWords(rightWord, kConjunctions))
        cost = 15;
    else if (InWords(rightWord, kSubordinators))
        cost = 25;
    // The discount for a preposition opening the next cue never listed
    // "d'"; the shared family does, so it is left out here.
    else if (rightWord != "d'" && InWords(rightWord, kPrepositions))
        cost = 35;
    if (IsForbiddenSubtitleBoundary(left, right)) cost += 400;

    if (gap >= RationalTime{2, 5})
        cost = std::max<int64_t>(0, cost - 60);
    else if (gap >= RationalTime{6, 25})
        cost = std::max<int64_t>(0, cost - 30);
    else if (gap <= RationalTime{2, 25})
        cost += 30;
    return cost;
}

int64_t ReadingSpeedCost(size_t characters, const RationalTime& duration) {
    if (duration.value <= 0) return std::numeric_limits<int64_t>::max() / 8;
    const __int128 actual = static_cast<__int128>(characters) * duration.rate;
    const __int128 target =
        static_cast<__int128>(kSubtitleTargetCharactersPerSecond) *
        duration.value;
    const __int128 difference =
        actual > target ? actual - target : target - actual;
    // A squared relative error makes 18 CPS the centre of the optimization,
    // while deliberately leaving every individual cue unconstrained.
    // Scale before squaring and cap only the score, so hostile time values
    // cannot overflow __int128. The cap does not change any cue boundary:
    // a thousand-times miss is already dominated by every realistic option.
    constexpr __int128 kMaximumRelativeThousandths = 1000000;
    const __int128 relativeThousandths = std::min<__int128>(
        difference * 1000 / target, kMaximumRelativeThousandths);
    return static_cast<int64_t>(relativeThousandths * relativeThousandths *
                                400 / 1000000);
}

using TimedSubtitleWord = SubtitleWord;

size_t AppendedCharacterCount(size_t current, const std::string& text) {
    return current + ((!current || JoinsWithoutSpace(text)) ? 0 : 1) +
           Utf8CharacterCount(text);
}

size_t AppendedCharacterCountOf(const std::vector<SubtitleWord>& words,
                                size_t begin, size_t end) {
    size_t characters = 0;
    for (size_t index = begin; index < end; ++index)
        characters = AppendedCharacterCount(characters, words[index].text);
    return characters;
}

RationalTime CueDisplayEnd(const std::vector<TimedSubtitleWord>& words,
                           size_t end) {
    if (end < words.size() &&
        words[end].start.sub(words[end - 1].end) < kSubtitleCueMaximumGap)
        return words[end].start;
    return words[end - 1].end;
}

SubtitleCue CueOfWords(const std::vector<TimedSubtitleWord>& words,
                       size_t begin, size_t end) {
    std::string text;
    for (size_t index = begin; index < end; ++index) {
        if (!text.empty() && !JoinsWithoutSpace(words[index].text))
            text.push_back(' ');
        text += words[index].text;
    }
    const RationalTime cueEnd = CueDisplayEnd(words, end);
    return {words[begin].start, cueEnd.sub(words[begin].start), text};
}

// A word as two texts of it compare: trimmed, the typographic apostrophe
// read as the straight one, ASCII and Latin-1 letters in lower case.
std::string ComparableWord(const std::string& text) {
    std::string out;
    const std::string trimmed = Trim(text);
    for (size_t index = 0; index < trimmed.size(); ++index) {
        const auto byte = static_cast<unsigned char>(trimmed[index]);
        if (byte == 0xE2 && index + 2 < trimmed.size() &&
            static_cast<unsigned char>(trimmed[index + 1]) == 0x80 &&
            static_cast<unsigned char>(trimmed[index + 2]) == 0x99) {
            out.push_back('\'');
            index += 2;
        } else if (byte == 0xC3 && index + 1 < trimmed.size()) {
            const auto second = static_cast<unsigned char>(trimmed[index + 1]);
            out.push_back(static_cast<char>(byte));
            out.push_back(static_cast<char>(second >= 0x80 && second <= 0x9E &&
                                                    second != 0x97
                                                ? second + 0x20
                                                : second));
            ++index;
        } else {
            out.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    return out;
}

}  // namespace

bool ParseSrt(const std::string& contents, std::vector<SubtitleCue>& cues,
              std::string& error) {
    cues.clear();
    error.clear();
    std::string normalized;
    normalized.reserve(contents.size());
    for (char character : contents)
        if (character != '\r') normalized.push_back(character);
    if (normalized.compare(0, 3, "\xEF\xBB\xBF") == 0) normalized.erase(0, 3);

    std::istringstream input(normalized);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (Trim(line).empty()) continue;
        std::string timing = line;
        if (line.find("-->") == std::string::npos) {
            if (!std::all_of(line.begin(), line.end(),
                             [](unsigned char c) {
                                 return std::isdigit(c) || std::isspace(c);
                             }) ||
                !std::getline(input, timing)) {
                error = "invalid SRT cue index at line " +
                        std::to_string(lineNumber);
                return false;
            }
            ++lineNumber;
        }
        RationalTime start;
        RationalTime end;
        if (!ParseTiming(timing, start, end)) {
            error = "invalid SRT timing at line " + std::to_string(lineNumber);
            return false;
        }
        std::string text;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (Trim(line).empty()) break;
            if (!text.empty()) text.push_back('\n');
            text += line;
        }
        if (text.empty()) {
            error = "empty SRT cue at line " + std::to_string(lineNumber);
            return false;
        }
        if (!cues.empty() &&
            start < cues.back().timeline_in.add(cues.back().duration)) {
            error = "overlapping SRT cues require separate subtitle tracks";
            return false;
        }
        cues.push_back({start, end.sub(start), std::move(text)});
    }
    if (cues.empty()) {
        error = "SRT file contains no cues";
        return false;
    }
    return true;
}

bool LoadSrt(const std::string& path, std::vector<SubtitleCue>& cues,
             std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open SRT file";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read SRT file";
        return false;
    }
    return ParseSrt(contents.str(), cues, error);
}

AddTrackOperation BuildSubtitleTrackEdit(const std::vector<SubtitleCue>& cues,
                                         int32_t trackIndex,
                                         const Ulid& trackId) {
    AddTrackOperation operation;
    operation.track_id = trackId.empty() ? GenerateUlid() : trackId;
    operation.kind = "caption";
    operation.index = trackIndex;
    operation.sync_lock = false;
    for (const SubtitleCue& cue : cues) {
        DocumentClip clip;
        clip.source_id.clear();
        clip.source_in = {0, cue.timeline_in.rate};
        clip.timeline_in = cue.timeline_in;
        clip.duration = cue.duration;
        clip.include_audio = false;
        clip.caption_text = cue.text;
        operation.clips.push_back(std::move(clip));
    }
    return operation;
}

int64_t SrtMilliseconds(RationalTime time, bool* rounded) {
    if (time.rate <= 0)
        throw std::invalid_argument("RationalTime rate must be positive");
    if (time.value < 0)
        throw std::invalid_argument("SRT cannot hold a negative time");
    const __int128 scaled = static_cast<__int128>(time.value) * 1000;
    const __int128 remainder = scaled % time.rate;
    const __int128 nearest =
        scaled / time.rate + (2 * remainder >= time.rate ? 1 : 0);
    if (nearest > std::numeric_limits<int64_t>::max())
        throw std::overflow_error("SRT time overflows int64_t");
    if (rounded != nullptr) *rounded = remainder != 0;
    return static_cast<int64_t>(nearest);
}

bool WriteSrt(const std::vector<SubtitleCue>& cues, const std::string& path,
              std::string& error, size_t* roundedTimestamps) {
    error.clear();
    if (roundedTimestamps != nullptr) *roundedTimestamps = 0;
    if (cues.empty()) {
        error = "timeline contains no visible subtitles";
        return false;
    }
    // Everything is formatted before the file is opened: a cue refused
    // halfway used to leave a truncated SRT behind, the cues before it
    // written and the rest missing, next to an error nobody reads twice.
    std::ostringstream text;
    size_t rounded = 0;
    try {
        for (size_t index = 0; index < cues.size(); ++index) {
            bool startRounded = false;
            bool endRounded = false;
            const int64_t start =
                SrtMilliseconds(cues[index].timeline_in, &startRounded);
            const int64_t end = SrtMilliseconds(
                cues[index].timeline_in.add(cues[index].duration), &endRounded);
            // ParseSrt refuses a cue that does not end after it starts; a
            // file this writer cannot read back is not written.
            if (end <= start) {
                error = "subtitle " + std::to_string(index + 1) +
                        " lasts less than the millisecond SRT counts in";
                return false;
            }
            if (startRounded) ++rounded;
            if (endRounded) ++rounded;
            text << index + 1 << '\n'
                 << FormatTimestamp(start) << " --> " << FormatTimestamp(end)
                 << '\n'
                 << cues[index].text << "\n\n";
        }
    } catch (const std::exception& exception) {
        error = std::string("subtitle time cannot be written in SRT: ") +
                exception.what();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create SRT file";
        return false;
    }
    output << text.str();
    if (!output) {
        error = "unable to write SRT file";
        return false;
    }
    if (roundedTimestamps != nullptr) *roundedTimestamps = rounded;
    return true;
}

bool FormatVtt(const std::vector<SubtitleCue>& cues, std::string& text,
               std::string& error, size_t* roundedTimestamps) {
    error.clear();
    if (roundedTimestamps != nullptr) *roundedTimestamps = 0;
    if (cues.empty()) {
        error = "timeline contains no visible subtitles";
        return false;
    }
    // SRT's clock with WebVTT's decimal point; the rounding is SRT's own.
    const auto stamp = [](int64_t milliseconds) {
        std::string value = FormatTimestamp(milliseconds);
        value[value.size() - 4] = '.';
        return value;
    };
    std::ostringstream output;
    output << "WEBVTT\n\n";
    size_t rounded = 0;
    try {
        for (size_t index = 0; index < cues.size(); ++index) {
            bool startRounded = false;
            bool endRounded = false;
            const int64_t start =
                SrtMilliseconds(cues[index].timeline_in, &startRounded);
            const int64_t end = SrtMilliseconds(
                cues[index].timeline_in.add(cues[index].duration), &endRounded);
            if (end <= start) {
                error = "subtitle " + std::to_string(index + 1) +
                        " lasts less than the millisecond WebVTT counts in";
                return false;
            }
            std::string escaped;
            bool lineHasText = false;
            for (const char character : cues[index].text) {
                if (character == '\r') continue;
                if (character == '\n') {
                    if (!lineHasText) {
                        error = "subtitle " + std::to_string(index + 1) +
                                " holds an empty line, which ends a WebVTT "
                                "cue";
                        return false;
                    }
                    lineHasText = false;
                    escaped.push_back('\n');
                    continue;
                }
                lineHasText = true;
                if (character == '&')
                    escaped += "&amp;";
                else if (character == '<')
                    escaped += "&lt;";
                else if (character == '>')
                    escaped += "&gt;";
                else
                    escaped.push_back(character);
            }
            if (!lineHasText) {
                error = "subtitle " + std::to_string(index + 1) +
                        " ends on an empty line, which ends a WebVTT cue";
                return false;
            }
            if (startRounded) ++rounded;
            if (endRounded) ++rounded;
            output << index + 1 << '\n'
                   << stamp(start) << " --> " << stamp(end) << '\n'
                   << escaped << "\n\n";
        }
    } catch (const std::exception& exception) {
        error = std::string("subtitle time cannot be written in WebVTT: ") +
                exception.what();
        return false;
    }
    text = output.str();
    if (roundedTimestamps != nullptr) *roundedTimestamps = rounded;
    return true;
}

bool WriteVtt(const std::vector<SubtitleCue>& cues, const std::string& path,
              std::string& error, size_t* roundedTimestamps) {
    std::string text;
    size_t rounded = 0;
    if (roundedTimestamps != nullptr) *roundedTimestamps = 0;
    if (!FormatVtt(cues, text, error, &rounded)) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create WebVTT file";
        return false;
    }
    output << text;
    if (!output) {
        error = "unable to write WebVTT file";
        return false;
    }
    if (roundedTimestamps != nullptr) *roundedTimestamps = rounded;
    return true;
}

bool SaveSrt(const Document& document, const std::string& path,
             std::string& error) {
    error.clear();
    std::vector<const DocumentClip*> ordered;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "caption" && track.visible)
            for (const DocumentClip& clip : track.clips)
                // A disabled carton is not delivered (ClipEnabled.h).
                if (ClipIsSeen(document.sequence, clip))
                    ordered.push_back(&clip);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const DocumentClip* left, const DocumentClip* right) {
                         return left->timeline_in < right->timeline_in;
                     });
    std::vector<SubtitleCue> cues;
    cues.reserve(ordered.size());
    for (const DocumentClip* clip : ordered)
        cues.push_back({clip->timeline_in, clip->duration, clip->caption_text});
    return WriteSrt(cues, path, error);
}

std::vector<const DocumentClip*> ActiveSubtitles(const Document& document,
                                                 RationalTime position) {
    std::vector<const DocumentClip*> result;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "caption" || !track.visible) continue;
        for (const DocumentClip& clip : track.clips)
            // A disabled carton is not shown (ClipEnabled.h).
            if (position >= clip.timeline_in &&
                position < clip.timeline_in.add(clip.duration) &&
                ClipIsSeen(document.sequence, clip))
                result.push_back(&clip);
    }
    return result;
}

bool SubtitleCuesForClip(const Transcript& transcript, const DocumentClip& clip,
                         std::vector<SubtitleCue>& cues, std::string& error) {
    cues.clear();
    error.clear();
    if (clip.source_id.empty() || transcript.media_id != clip.source_id) {
        error = "transcript does not match clip source";
        return false;
    }

    // R3 -- docs/roadmap/REEL-2026-09.md, guard rail 5. A cue is a source
    // time placed on the timeline; on a clip that is not at 1/1 that
    // placement stretches, and a cue written at the speed it was spoken
    // would drift away from the picture. Refused by name.
    if (ClipHasSpeed(clip)) {
        error = ClipSpeedRefusal(clip, "subtitles");
        return false;
    }
    const RationalTime sourceEnd = clip.source_in.add(clip.duration);
    std::vector<TimedSubtitleWord> words;
    words.reserve(transcript.words.size());
    for (const TranscriptWord& word : transcript.words) {
        if (word.end <= clip.source_in || word.start >= sourceEnd) continue;
        const RationalTime sourceStart =
            word.start < clip.source_in ? clip.source_in : word.start;
        const RationalTime clippedEnd =
            word.end > sourceEnd ? sourceEnd : word.end;
        const RationalTime timelineStart =
            clip.timeline_in.add(sourceStart.sub(clip.source_in));
        const RationalTime timelineEnd =
            clip.timeline_in.add(clippedEnd.sub(clip.source_in));
        std::string text = Trim(word.text);
        if (text.empty() || timelineEnd <= timelineStart) continue;
        words.push_back({std::move(text), timelineStart, timelineEnd});
    }
    if (words.empty()) {
        error = "transcript has no words inside clip source range";
        return false;
    }

    return SegmentSubtitleWords(words, {}, cues, nullptr, error);
}

size_t SubtitleCharacterCount(const std::string& text) {
    return Utf8CharacterCount(text);
}

bool SubtitleEndsSentence(const std::string& text) {
    return EndsSentence(text);
}

bool SubtitleOpensSentenceAfter(const std::string& previous) {
    if (previous.empty()) return true;
    const bool ellipsis = previous.size() >= 2 &&
                          previous.compare(previous.size() - 2, 2, "..") == 0;
    return EndsSentence(previous) && !ellipsis;
}

std::string SubtitleWithCapital(const std::string& word) {
    std::string text = word;
    size_t index = 0;
    // Past an opening quote or bracket: ", ', (, [, and « with the space
    // French sets after it.
    while (index < text.size()) {
        const unsigned char character = text[index];
        if (character == '"' || character == '\'' || character == '(' ||
            character == '[' || character == ' ') {
            ++index;
        } else if (character == 0xC2 && index + 1 < text.size() &&
                   (static_cast<unsigned char>(text[index + 1]) == 0xAB ||
                    static_cast<unsigned char>(text[index + 1]) == 0xA0)) {
            index += 2;
        } else {
            break;
        }
    }
    if (index >= text.size()) return text;
    const unsigned char first = text[index];
    if (first >= 'a' && first <= 'z') {
        text[index] = static_cast<char>(first - 'a' + 'A');
    } else if (first == 0xC3 && index + 1 < text.size()) {
        // U+00E0 to U+00FE are the capitals U+00C0 to U+00DE plus 0x20,
        // except the division sign.
        const unsigned char second = text[index + 1];
        if (second >= 0xA0 && second <= 0xBE && second != 0xB7)
            text[index + 1] = static_cast<char>(second - 0x20);
    } else if (first == 0xC5 && index + 1 < text.size() &&
               static_cast<unsigned char>(text[index + 1]) == 0x93) {
        text[index + 1] = static_cast<char>(0x92);  // œ -> Œ
    }
    return text;
}

int64_t SubtitleRuleBoundaryCost(const std::string& left,
                                 const std::string& right,
                                 const RationalTime& gap) {
    return SubtitleBoundaryCost(left, right, gap);
}

uint32_t SubtitleWordFamilies(const std::string& word) {
    const std::string syntax = SyntaxWord(word);
    if (syntax.empty()) return 0;
    uint32_t families = 0;
    if (InWords(syntax, kDeterminers)) families |= kSubtitleDeterminer;
    if (InWords(syntax, kPrepositions)) families |= kSubtitlePreposition;
    if (InWords(syntax, kSubjectPronouns)) families |= kSubtitleSubjectPronoun;
    if (InWords(syntax, kAuxiliaries)) families |= kSubtitleAuxiliary;
    if (syntax == "ne" || syntax == "n'") families |= kSubtitleNegation;
    if (InWords(syntax, kConjunctions)) families |= kSubtitleConjunction;
    if (InWords(syntax, kModifiers)) families |= kSubtitleModifier;
    if (InWords(syntax, kClitics)) families |= kSubtitleClitic;
    if (InWords(syntax, kSubordinators)) families |= kSubtitleSubordinator;
    return families;
}

std::string SubtitleSyntaxWord(const std::string& text) {
    return SyntaxWord(text);
}

size_t SubtitleCueCharacters(const std::vector<SubtitleWord>& words,
                             size_t begin, size_t end) {
    return AppendedCharacterCountOf(words, begin, std::min(end, words.size()));
}

RationalTime SubtitleCueDisplayEnd(const std::vector<SubtitleWord>& words,
                                   size_t end) {
    return CueDisplayEnd(words, end);
}

bool SegmentSubtitleWords(const std::vector<SubtitleWord>& words,
                          const SubtitleSegmentationLimits& limits,
                          std::vector<SubtitleCue>& cues,
                          std::vector<std::pair<size_t, size_t>>* ranges,
                          std::string& error) {
    cues.clear();
    if (ranges != nullptr) ranges->clear();
    if (words.empty()) {
        error = "no words to segment";
        return false;
    }
    if (limits.boundary_costs != nullptr &&
        limits.boundary_costs->size() != words.size() - 1) {
        error = "boundary costs must give one cost per junction between words";
        return false;
    }
    // The segmentation is global rather than greedy: every possible cue is
    // scored against reading pace and the syntax/air at its outgoing edge.
    // Sentence ends and long pauses remain hard boundaries; 42 characters
    // and five seconds are safety rails, not the reading-speed target.
    const size_t kMaximumCharacters = limits.maximum_characters;
    const RationalTime kMaximumDuration = limits.maximum_duration;
    const bool widthLimited = limits.width && limits.maximum_width > 0;
    const int64_t kUnreachable = std::numeric_limits<int64_t>::max() / 4;
    std::vector<int64_t> costs(words.size() + 1, kUnreachable);
    std::vector<size_t> next(words.size(), words.size());
    costs.back() = 0;
    for (size_t reverse = words.size(); reverse > 0; --reverse) {
        const size_t begin = reverse - 1;
        size_t characters = 0;
        for (size_t end = begin + 1; end <= words.size(); ++end) {
            characters =
                AppendedCharacterCount(characters, words[end - 1].text);
            const RationalTime displayEnd = CueDisplayEnd(words, end);
            const RationalTime duration = displayEnd.sub(words[begin].start);
            const bool singleWord = end == begin + 1;
            if (!singleWord && (characters > kMaximumCharacters ||
                                duration > kMaximumDuration ||
                                (widthLimited && limits.width(begin, end) >
                                                     limits.maximum_width)))
                break;
            if (costs[end] != kUnreachable) {
                int64_t boundaryCost = 0;
                if (end < words.size()) {
                    boundaryCost =
                        limits.boundary_costs != nullptr
                            ? (*limits.boundary_costs)[end - 1]
                            : SubtitleBoundaryCost(
                                  words[end - 1].text, words[end].text,
                                  words[end].start.sub(words[end - 1].end));
                }
                int64_t speedCost = ReadingSpeedCost(characters, duration);
                if (limits.cue_cost)
                    speedCost += limits.cue_cost(begin, end, characters);
                const int64_t candidate = costs[end] + boundaryCost + speedCost;
                // On equal evidence, keep the longer syntactic unit. This
                // avoids gratuitous one-word flashes without hiding a better
                // timing or syntax boundary.
                if (candidate < costs[begin] ||
                    (candidate == costs[begin] && end > next[begin])) {
                    costs[begin] = candidate;
                    next[begin] = end;
                }
            }
            const bool sentenceEnd = EndsSentence(words[end - 1].text);
            const bool longGap = end < words.size() &&
                                 words[end].start.sub(words[end - 1].end) >=
                                     kSubtitleCueMaximumGap;
            if (sentenceEnd || longGap) break;
        }
    }
    if (costs.front() == kUnreachable) {
        error = "transcript cannot be segmented into readable subtitle cues";
        return false;
    }

    for (size_t begin = 0; begin < words.size();) {
        const size_t end = next[begin];
        if (end <= begin || end > words.size()) {
            error = "subtitle segmentation produced an invalid boundary";
            cues.clear();
            if (ranges != nullptr) ranges->clear();
            return false;
        }
        cues.push_back(CueOfWords(words, begin, end));
        if (ranges != nullptr) ranges->emplace_back(begin, end);
        begin = end;
    }
    return true;
}

bool GroupSubtitleWords(const std::vector<SubtitleWord>& words,
                        const std::vector<std::string>& cartons,
                        const SubtitleSegmentationLimits& limits,
                        std::vector<SubtitleCue>& cues,
                        std::vector<std::pair<size_t, size_t>>* ranges,
                        std::string& error) {
    cues.clear();
    if (ranges != nullptr) ranges->clear();
    std::vector<std::pair<size_t, size_t>> found;
    size_t next = 0;
    for (size_t carton = 0; carton < cartons.size(); ++carton) {
        const std::string name = "carton " + std::to_string(carton) + " (\"" +
                                 Trim(cartons[carton]) + "\")";
        std::istringstream input(cartons[carton]);
        std::string word;
        const size_t begin = next;
        while (input >> word) {
            if (next >= words.size()) {
                error = name + " goes past the montage's last word";
                return false;
            }
            if (ComparableWord(word) != ComparableWord(words[next].text)) {
                error = name + " has '" + word + "' where the montage says '" +
                        words[next].text + "' (word " + std::to_string(next) +
                        ")";
                return false;
            }
            ++next;
        }
        if (next == begin) {
            error = "carton " + std::to_string(carton) + " is empty";
            return false;
        }
        for (size_t index = begin; index + 1 < next; ++index) {
            if (EndsSentence(words[index].text)) {
                error = name + " crosses a sentence end after '" +
                        words[index].text + "'";
                return false;
            }
            if (words[index + 1].start.sub(words[index].end) >=
                kSubtitleCueMaximumGap) {
                error = name + " crosses a pause of 0.7 s or more after '" +
                        words[index].text + "'";
                return false;
            }
        }
        if (next - begin > 1 &&
            (AppendedCharacterCountOf(words, begin, next) >
                 limits.maximum_characters ||
             CueDisplayEnd(words, next).sub(words[begin].start) >
                 limits.maximum_duration)) {
            error = name + " is longer than " +
                    std::to_string(limits.maximum_characters) +
                    " characters or 5 s";
            return false;
        }
        found.emplace_back(begin, next);
    }
    if (next < words.size()) {
        error = "the cartons stop at word " + std::to_string(next) + " of " +
                std::to_string(words.size()) + " ('" + words[next].text +
                "'): every word of the montage belongs to a carton";
        return false;
    }
    for (const auto& [begin, end] : found) {
        cues.push_back(CueOfWords(words, begin, end));
        if (ranges != nullptr) ranges->emplace_back(begin, end);
    }
    return true;
}

bool WordSubtitleCuesForClip(const Transcript& transcript,
                             const DocumentClip& clip,
                             std::vector<SubtitleCue>& cues,
                             std::string& error) {
    cues.clear();
    error.clear();
    if (clip.source_id.empty() || transcript.media_id != clip.source_id) {
        error = "transcript does not match clip source";
        return false;
    }

    // R3 -- docs/roadmap/REEL-2026-09.md, guard rail 5. A cue is a source
    // time placed on the timeline; on a clip that is not at 1/1 that
    // placement stretches, and a cue written at the speed it was spoken
    // would drift away from the picture. Refused by name.
    if (ClipHasSpeed(clip)) {
        error = ClipSpeedRefusal(clip, "subtitles");
        return false;
    }
    const RationalTime sourceEnd = clip.source_in.add(clip.duration);
    for (const TranscriptWord& word : transcript.words) {
        if (word.end <= clip.source_in || word.start >= sourceEnd) continue;
        const RationalTime sourceStart =
            word.start < clip.source_in ? clip.source_in : word.start;
        const RationalTime clippedEnd =
            word.end > sourceEnd ? sourceEnd : word.end;
        const RationalTime timelineStart =
            clip.timeline_in.add(sourceStart.sub(clip.source_in));
        const RationalTime timelineEnd =
            clip.timeline_in.add(clippedEnd.sub(clip.source_in));
        const std::string text = Trim(word.text);
        if (text.empty() || timelineEnd <= timelineStart) continue;
        cues.push_back({timelineStart, timelineEnd.sub(timelineStart), text});
    }
    if (cues.empty()) {
        error = "transcript has no words inside clip source range";
        return false;
    }
    return true;
}
