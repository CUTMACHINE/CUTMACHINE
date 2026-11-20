#include "PauseTightening.h"
#include "ClipSpeed.h"

#include "Json.h"
#include "Transcription.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

namespace {

using mcp_json::Value;

// One maximal run of windows under the speech threshold, as window indices
// into the envelope: [first, last).
struct Hollow {
    int64_t first = 0;
    int64_t last = 0;
};

void WriteTime(Value& parent, const std::string& key,
               const RationalTime& time) {
    Value entry = Value::MakeObject();
    entry.Set("value", Value::MakeInt(time.value));
    entry.Set("rate", Value::MakeInt(time.rate));
    parent.Set(key, std::move(entry));
}

// AGENT-2026-09 A17 -- the word's loudest window, or zero outside the
// envelope. A quiet one-frame "à" (IA8) rises well above the floor; a word
// the transcriber wrote and the aligner placed in the room tone does not.
int64_t WordPeak(const TranscriptWord& word,
                 const SpeechOnsetReport& envelope) {
    if (envelope.windows_per_second == 0) return 0;
    const int32_t grid = static_cast<int32_t>(envelope.windows_per_second);
    int64_t first = 0;
    int64_t end = 0;
    try {
        first = word.start.to_frames(grid);
        end = word.end.to_frames(grid);
        if (word.end.compare(RationalTime{end, grid}) != 0) ++end;
    } catch (const std::exception&) {
        return 0;
    }
    first = std::max<int64_t>(first, 0);
    end = std::min<int64_t>(end, static_cast<int64_t>(envelope.levels.size()));
    int64_t peak = 0;
    for (int64_t window = first; window < end; ++window)
        peak = std::max(peak, envelope.levels[static_cast<size_t>(window)]);
    return peak;
}

// The level `margin` dB above the noise floor, in the envelope's scale.
int64_t FloorLine(const SpeechOnsetReport& envelope, int64_t marginDb) {
    return static_cast<int64_t>(
        std::llround(static_cast<double>(envelope.noise_floor) *
                     std::pow(10.0, static_cast<double>(marginDb) / 20.0)));
}

}  // namespace

bool ResolvePauseTightening(const DocumentClip& clip,
                            const SpeechOnsetReport& envelope,
                            const Transcript* words,
                            const PauseTighteningSettings& settings,
                            const MediaRate& sourceRate,
                            const std::vector<Ulid>& syncTrackIds,
                            RemoveWordsOperation& operation,
                            PauseTighteningReport& report, std::string& error) {
    error.clear();
    operation = RemoveWordsOperation{};
    report = PauseTighteningReport{};
    if (ClipHasSpeed(clip)) {
        error = ClipSpeedRefusal(clip, "tighten_pauses");
        return false;
    }
    if (clip.source_id != envelope.media_id) {
        error = "speech envelope belongs to another source";
        return false;
    }
    if (envelope.windows_per_second == 0 || envelope.levels.empty()) {
        error = "speech envelope has no analysis grid";
        return false;
    }
    if (sourceRate.num <= 0 || sourceRate.den <= 0) {
        error = "source frame rate is invalid";
        return false;
    }
    if (settings.minimum_gap_milliseconds <= 0) {
        error = "minimum gap must be a positive duration";
        return false;
    }
    if (settings.keep_frames < 0) {
        error = "frames to keep cannot be negative";
        return false;
    }
    if (settings.speech_ratio_percent <= 0 ||
        settings.speech_ratio_percent >= 100) {
        error = "speech ratio must be a percentage strictly between 0 and 100";
        return false;
    }
    if (words != nullptr && words->media_id != clip.source_id) {
        error = "transcript belongs to another source";
        return false;
    }
    // Whisper's own boundaries drift and sit on silence; see the IA8 note in
    // the header for why only an aligner's are allowed to veto a cut.
    const bool wordsAligned =
        words != nullptr && words->word_timing != "whisper";
    report.words_checked = wordsAligned;
    // AGENT-2026-09 A17 -- the line under which an aligned word is a word
    // nobody said, and the floor it is counted from, published.
    if (settings.unheard_margin_db < 0 || settings.unheard_margin_db > 30) {
        error = "unheard_margin_db must be between 0 and 30";
        return false;
    }
    const int64_t floorLine = FloorLine(envelope, settings.unheard_margin_db);
    report.floor_dbfs = SpeechLevelDbfs(envelope.noise_floor);
    if (wordsAligned) {
        report.word_timing = words->word_timing;
    } else {
        report.words_unchecked_reason =
            words == nullptr
                ? "no transcript is cached for this source"
                : "only Whisper word boundaries are cached; they are not "
                  "precise enough to protect a word, a forced alignment is";
    }

    const int32_t grid = static_cast<int32_t>(envelope.windows_per_second);
    const RationalTime clipStart = clip.source_in;
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    // Floor at the head and ceiling at the tail, so the analysed span never
    // reaches past the material the clip actually plays.
    int64_t firstWindow = clipStart.to_frames(grid);
    int64_t lastWindow = clipEnd.to_frames(grid);
    if (clipEnd.compare(RationalTime{lastWindow, grid}) != 0) ++lastWindow;
    if (firstWindow < 0) firstWindow = 0;
    if (lastWindow > static_cast<int64_t>(envelope.levels.size()))
        lastWindow = static_cast<int64_t>(envelope.levels.size());
    if (lastWindow <= firstWindow) {
        error = "clip holds no analysis window";
        return false;
    }

    // The clip's own speech level, never the source's: a rush holding one
    // loud take and one quiet one would otherwise measure the quiet clip
    // against the loud one and find it silent from end to end. Same choice,
    // and same reason, as SummarizeClipSpeechOnset.
    std::vector<int64_t> inside(
        envelope.levels.begin() + static_cast<size_t>(firstWindow),
        envelope.levels.begin() + static_cast<size_t>(lastWindow));
    const int64_t speechLevel = SpeechLevelPercentile(inside, 90);
    const int64_t threshold = speechLevel * settings.speech_ratio_percent / 100;

    std::vector<Hollow> hollows;
    int64_t runStart = -1;
    for (int64_t window = firstWindow; window < lastWindow; ++window) {
        const bool silent =
            envelope.levels[static_cast<size_t>(window)] < threshold;
        if (silent && runStart < 0) runStart = window;
        if (!silent && runStart >= 0) {
            hollows.push_back({runStart, window});
            runStart = -1;
        }
    }
    if (runStart >= 0) hollows.push_back({runStart, lastWindow});

    // Head and tail air belong to SpeechOnset.h's question, not this one.
    // They are measured and reported so a caller sees what was left standing.
    if (!hollows.empty() && hollows.front().first == firstWindow) {
        report.head_air =
            RationalTime{hollows.front().last - firstWindow, grid};
        hollows.erase(hollows.begin());
    }
    if (!hollows.empty() && hollows.back().last == lastWindow) {
        report.tail_air = RationalTime{lastWindow - hollows.back().first, grid};
        hollows.pop_back();
    }

    const RationalTime minimumGap{settings.minimum_gap_milliseconds, 1000};
    const RationalTime keep{settings.keep_frames * sourceRate.den,
                            sourceRate.num};
    // Split between the two ends rather than left at one: the splice then
    // sits in the deepest part of the hollow, as far from the decay of the
    // previous word and from the attack of the next as the budget allows.
    // Odd frames go to the head, where a trailing sibilant lives.
    const int64_t headFrames = (settings.keep_frames + 1) / 2;
    const int64_t tailFrames = settings.keep_frames / 2;
    const RationalTime headKeep{headFrames * sourceRate.den, sourceRate.num};
    const RationalTime tailKeep{tailFrames * sourceRate.den, sourceRate.num};

    RationalTime removedTotal{0, sourceRate.num};
    // One pause, in source time, judged and closed on its own. A hollow is
    // one call; a hollow split around aligned words is one call per part.
    const auto closeGap = [&](const RationalTime& rawStart,
                              const RationalTime& rawEnd) {
        if (rawEnd.sub(rawStart).compare(minimumGap) < 0) {
            ++report.skipped_short;
            return;
        }
        // Inward on both sides: a boundary the envelope places between two
        // frames resolves inside the silence, never over the speech next to
        // it. RoundToSourceFrame is the codebase's one explicit rounding rule
        // for exactly this kind of continuous-to-frame snap.
        const RationalTime start = RoundToSourceFrame(rawStart, sourceRate,
                                                      /*roundUp=*/true);
        const RationalTime end = RoundToSourceFrame(rawEnd, sourceRate,
                                                    /*roundUp=*/false);
        if (start.compare(end) >= 0) {
            ++report.skipped_short;
            return;
        }
        const RationalTime cutStart = start.add(headKeep);
        const RationalTime cutEnd = end.sub(tailKeep);
        if (cutStart.compare(cutEnd) >= 0) {
            // The hollow is already at or under the length asked for. Not a
            // failure: the clip is already as tight as the request wants.
            ++report.skipped_already_tight;
            return;
        }
        if (cutStart.compare(clipStart) < 0 || cutEnd.compare(clipEnd) > 0) {
            // Rounding pushed a boundary outside the clip. ApplyOperation
            // would refuse the whole operation for it, so drop this one
            // pause rather than lose the others with it.
            ++report.skipped_short;
            return;
        }
        operation.ranges.push_back({cutStart, cutEnd});
        const RationalTime removed = cutEnd.sub(cutStart);
        report.pauses.push_back({rawStart, rawEnd, removed});
        removedTotal = removedTotal.add(removed);
        // Reported in the same domain as the pauses that make it up, so a
        // caller adding the entries up gets the published total back.
        report.removed_total = removedTotal;
    };

    // Hollows are in order and aligned words are ordered and do not overlap
    // (LoadWordAlignment checks both), so one cursor serves every hollow. It
    // only passes words that end before the current hollow starts: a word
    // with a stop consonant inside can hold two hollows.
    size_t firstWord = 0;
    for (const Hollow& hollow : hollows) {
        const RationalTime rawStart{hollow.first, grid};
        const RationalTime rawEnd{hollow.last, grid};
        if (!wordsAligned) {
            closeGap(rawStart, rawEnd);
            continue;
        }
        while (firstWord < words->words.size() &&
               words->words[firstWord].end.compare(rawStart) <= 0)
            ++firstWord;
        // The parts of the hollow no aligned word covers, left to right.
        RationalTime partStart = rawStart;
        bool holdsWord = false;
        for (size_t index = firstWord; index < words->words.size(); ++index) {
            const TranscriptWord& word = words->words[index];
            if (word.start.compare(rawEnd) >= 0) break;
            // AGENT-2026-09 A17 -- heard, unless every window of the word
            // sits within the margin of the source's noise floor.
            const int64_t peak = WordPeak(word, envelope);
            const bool heard = peak > floorLine;
            const int64_t peakDbfs = SpeechLevelDbfs(peak);
            if (!heard && settings.close_unheard) {
                report.closed_over_words.push_back(
                    {word.text, word.start, word.end, false, peakDbfs});
                continue;
            }
            // AGENT-2026-09 A33 -- only the audible windows of the word are
            // kept out of the hollow: the aligned span trimmed, inside the
            // hollow, to the windows above the floor line. A word whose
            // whole share of the hollow sits at the floor is an aligner's
            // end running into the silence, and the hollow closes over it.
            // An unheard word (A17) is kept whole unless close_unheard: its
            // trimming would close it as a matter of course.
            RationalTime protectedStart = word.start;
            RationalTime protectedEnd = word.end;
            if (heard) {
                int64_t protectFirst =
                    (word.start.compare(partStart) > 0 ? word.start : partStart)
                        .to_frames(grid);
                int64_t protectEnd =
                    (word.end.compare(rawEnd) < 0 ? word.end : rawEnd)
                        .to_frames(grid);
                const int64_t last =
                    static_cast<int64_t>(envelope.levels.size());
                protectFirst = std::clamp<int64_t>(protectFirst, 0, last);
                protectEnd =
                    std::clamp<int64_t>(protectEnd, protectFirst, last);
                // Only an edge of the word that lies inside the hollow is
                // released: a hollow the word spans whole is a stop
                // consonant inside it, and stays a word.
                if (word.end.compare(rawEnd) < 0)
                    while (
                        protectEnd > protectFirst &&
                        envelope.levels[static_cast<size_t>(protectEnd - 1)] <=
                            floorLine)
                        --protectEnd;
                if (word.start.compare(rawStart) > 0)
                    while (protectFirst < protectEnd &&
                           envelope.levels[static_cast<size_t>(protectFirst)] <=
                               floorLine)
                        ++protectFirst;
                if (protectFirst >= protectEnd) {
                    report.closed_over_words.push_back(
                        {word.text, word.start, word.end, heard, peakDbfs});
                    continue;
                }
                protectedStart = RationalTime{protectFirst, grid};
                protectedEnd = RationalTime{protectEnd, grid};
            }
            holdsWord = true;
            report.kept_words.push_back({word.text, word.start, word.end, heard,
                                         peakDbfs, protectedStart,
                                         protectedEnd});
            if (protectedStart.compare(partStart) > 0)
                closeGap(partStart, protectedStart);
            if (protectedEnd.compare(partStart) > 0) partStart = protectedEnd;
        }
        if (partStart.compare(rawEnd) < 0) closeGap(partStart, rawEnd);
        if (holdsWord) ++report.hollows_holding_words;
    }

    operation.clip_id = clip.id;
    operation.gap_padding = RationalTime{0, sourceRate.num};
    operation.sync_track_ids = syncTrackIds;
    return true;
}

std::string SerializePauseTighteningReport(
    const PauseTighteningReport& report) {
    Value root = Value::MakeObject();
    root.Set("closed_count",
             Value::MakeInt(static_cast<int64_t>(report.pauses.size())));
    WriteTime(root, "removed_total", report.removed_total);
    root.Set("skipped_short", Value::MakeInt(report.skipped_short));
    root.Set("skipped_already_tight",
             Value::MakeInt(report.skipped_already_tight));
    WriteTime(root, "head_air", report.head_air);
    WriteTime(root, "tail_air", report.tail_air);
    root.Set("words_checked", Value::MakeBool(report.words_checked));
    if (report.words_checked) {
        root.Set("word_timing", Value::MakeString(report.word_timing));
    } else {
        root.Set("words_unchecked_reason",
                 Value::MakeString(report.words_unchecked_reason));
    }
    root.Set("hollows_holding_words",
             Value::MakeInt(report.hollows_holding_words));
    Value kept = Value::MakeArray();
    // AGENT-2026-09 A17 -- the floor the margin counts from, and what the
    // hollow was closed over, when asked.
    if (report.words_checked)
        root.Set("floor_dbfs", Value::MakeInt(report.floor_dbfs));
    if (!report.closed_over_words.empty()) {
        Value closedOver = Value::MakeArray();
        for (const PauseKeptWord& word : report.closed_over_words) {
            Value entry = Value::MakeObject();
            entry.Set("text", Value::MakeString(word.text));
            WriteTime(entry, "start", word.start);
            WriteTime(entry, "end", word.end);
            entry.Set("peak_dbfs", Value::MakeInt(word.peak_dbfs));
            entry.Set("reason",
                      Value::MakeString(word.heard ? "aligned_past_the_voice"
                                                   : "unheard"));
            closedOver.Push(std::move(entry));
        }
        root.Set("closed_over_words", std::move(closedOver));
    }
    for (const PauseKeptWord& word : report.kept_words) {
        Value entry = Value::MakeObject();
        entry.Set("text", Value::MakeString(word.text));
        WriteTime(entry, "start", word.start);
        WriteTime(entry, "end", word.end);
        entry.Set("peak_dbfs", Value::MakeInt(word.peak_dbfs));
        if (!word.heard) entry.Set("heard", Value::MakeBool(false));
        WriteTime(entry, "protected_start", word.protected_start);
        WriteTime(entry, "protected_end", word.protected_end);
        kept.Push(std::move(entry));
    }
    root.Set("kept_words", std::move(kept));
    Value closed = Value::MakeArray();
    for (const TightenedPause& pause : report.pauses) {
        Value entry = Value::MakeObject();
        WriteTime(entry, "gap_start", pause.gap_start);
        WriteTime(entry, "gap_end", pause.gap_end);
        WriteTime(entry, "removed", pause.removed);
        closed.Push(std::move(entry));
    }
    root.Set("closed", std::move(closed));
    return root.Dump();
}
