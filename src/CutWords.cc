#include "CutWords.h"

#include "Json.h"

#include <algorithm>
#include <utility>

namespace {

using mcp_json::Value;

// One clip edge on the audio tracks of either document.
struct CoverageEdge {
    RationalTime time;
    int before = 0;
    int after = 0;
};

void CollectAudioEdges(const Document& document, bool isBefore,
                       std::map<Ulid, std::vector<CoverageEdge>>& edges) {
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips) {
            if (clip.duration.value <= 0) continue;
            std::vector<CoverageEdge>& list = edges[clip.source_id];
            list.push_back(
                {clip.source_in, isBefore ? 1 : 0, isBefore ? 0 : 1});
            list.push_back({clip.source_in.add(clip.duration),
                            isBefore ? -1 : 0, isBefore ? 0 : -1});
        }
    }
}

MediaRate RateOf(const Document& before, const Document& after,
                 const Ulid& sourceId) {
    if (const DocumentSource* source = before.FindSource(sourceId))
        return source->rate;
    if (const DocumentSource* source = after.FindSource(sourceId))
        return source->rate;
    return {0, 1};
}

int64_t FirstFrame(const RationalTime& time, const MediaRate& rate) {
    return time.to_frames(rate.num, rate.den);
}

int64_t EndFrame(const RationalTime& time, const MediaRate& rate) {
    const int64_t frame = time.to_frames(rate.num, rate.den);
    const RationalTime frameStart{frame * rate.den, rate.num};
    return frameStart.compare(time) < 0 ? frame + 1 : frame;
}

bool IsTargeted(const std::vector<WordRange>* ranges, size_t index) {
    if (ranges == nullptr) return false;
    return std::any_of(ranges->begin(), ranges->end(),
                       [&](const WordRange& range) {
                           return range.start_word_index <= index &&
                                  index <= range.end_word_index;
                       });
}

Value WordView(const CutWord& word, const MediaRate& rate) {
    Value entry = Value::MakeObject();
    entry.Set("word_index",
              Value::MakeInt(static_cast<int64_t>(word.word_index)));
    entry.Set("text", Value::MakeString(word.text));
    entry.Set("first_frame", Value::MakeInt(FirstFrame(word.start, rate)));
    entry.Set("end_frame", Value::MakeInt(EndFrame(word.end, rate)));
    entry.Set("whole", Value::MakeBool(word.whole));
    return entry;
}

}  // namespace

std::vector<RemovedSourceRange> RemovedAudioMaterial(const Document& before,
                                                     const Document& after) {
    std::map<Ulid, std::vector<CoverageEdge>> edges;
    CollectAudioEdges(before, true, edges);
    CollectAudioEdges(after, false, edges);

    std::vector<RemovedSourceRange> removed;
    for (auto& [sourceId, list] : edges) {
        std::stable_sort(
            list.begin(), list.end(),
            [](const CoverageEdge& left, const CoverageEdge& right) {
                return left.time.compare(right.time) < 0;
            });
        const MediaRate rate = RateOf(before, after, sourceId);
        int coveredBefore = 0;
        int coveredAfter = 0;
        size_t index = 0;
        while (index < list.size()) {
            const RationalTime at = list[index].time;
            while (index < list.size() && list[index].time.compare(at) == 0) {
                coveredBefore += list[index].before;
                coveredAfter += list[index].after;
                ++index;
            }
            // The counts now hold from `at` up to the next edge.
            if (index == list.size() || coveredAfter >= coveredBefore) continue;
            const RationalTime until = list[index].time;
            if (!removed.empty() && removed.back().source_id == sourceId &&
                removed.back().end.compare(at) == 0) {
                removed.back().end = until;
            } else {
                removed.push_back({sourceId, rate, at, until});
            }
        }
    }
    return removed;
}

CutWordsReport CheckCutWords(const std::vector<RemovedSourceRange>& removed,
                             const std::map<Ulid, Transcript>& transcripts,
                             const TargetedWords& targeted) {
    CutWordsReport report;
    for (const RemovedSourceRange& range : removed) {
        if (report.sources.empty() ||
            report.sources.back().source_id != range.source_id) {
            SourceCutWords source;
            source.source_id = range.source_id;
            source.source_rate = range.source_rate;
            report.sources.push_back(std::move(source));
        }
        report.sources.back().removed.push_back(range);
    }

    for (SourceCutWords& source : report.sources) {
        const auto found = transcripts.find(source.source_id);
        if (found == transcripts.end()) {
            source.unchecked_reason = "no transcript is cached for this source";
        } else if (found->second.word_timing == "whisper") {
            // See the header: a word named from Whisper's boundaries would
            // be a guess reported as a measurement.
            source.unchecked_reason =
                "only Whisper word boundaries are cached; a forced alignment "
                "is needed to name the words a cut touches";
        } else {
            source.checked = true;
            source.word_timing = found->second.word_timing;
        }
        if (!source.checked) {
            report.checked = false;
            continue;
        }
        const auto targetedFound = targeted.find(source.source_id);
        const std::vector<WordRange>* targetedRanges =
            targetedFound == targeted.end() ? nullptr : &targetedFound->second;
        const std::vector<TranscriptWord>& words = found->second.words;
        for (size_t index = 0; index < words.size(); ++index) {
            const TranscriptWord& word = words[index];
            bool touched = false;
            bool whole = false;
            for (const RemovedSourceRange& range : source.removed) {
                if (!(range.start < word.end) || !(word.start < range.end))
                    continue;
                touched = true;
                // Ranges are merged, so a word removed whole lies inside one.
                whole = whole ||
                        (range.start <= word.start && word.end <= range.end);
            }
            if (!touched) {
                // AGENT-2026-09 A45 -- a word aligned on one or two frames
                // next to a cut: its voice likely runs into the cut.
                if (source.source_rate.num > 0 && source.source_rate.den > 0) {
                    const MediaRate& rate = source.source_rate;
                    const int64_t first = FirstFrame(word.start, rate);
                    const int64_t end = EndFrame(word.end, rate);
                    if (end - first <= 2) {
                        for (const RemovedSourceRange& range : source.removed) {
                            const int64_t cutFirst =
                                FirstFrame(range.start, rate);
                            const int64_t cutEnd = EndFrame(range.end, rate);
                            if ((end <= cutFirst && cutFirst - end <= 3) ||
                                (first >= cutEnd && first - cutEnd <= 3)) {
                                source.at_risk.push_back({index, word.text,
                                                          word.start, word.end,
                                                          false});
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            CutWord cut{index, word.text, word.start, word.end, whole};
            (IsTargeted(targetedRanges, index) ? source.targeted
                                               : source.carried)
                .push_back(std::move(cut));
        }
    }
    return report;
}

std::string SerializeCutWordsReport(const CutWordsReport& report) {
    Value root = Value::MakeObject();
    root.Set("checked", Value::MakeBool(report.checked));
    Value sources = Value::MakeArray();
    for (const SourceCutWords& source : report.sources) {
        const MediaRate rate = source.source_rate;
        const bool framed = rate.num > 0 && rate.den > 0;
        Value entry = Value::MakeObject();
        entry.Set("source_id", Value::MakeString(source.source_id));
        Value removed = Value::MakeArray();
        for (const RemovedSourceRange& range : source.removed) {
            if (!framed) continue;
            Value frames = Value::MakeObject();
            frames.Set("first_frame",
                       Value::MakeInt(FirstFrame(range.start, rate)));
            frames.Set("end_frame", Value::MakeInt(EndFrame(range.end, rate)));
            removed.Push(std::move(frames));
        }
        entry.Set("removed", std::move(removed));
        entry.Set("checked", Value::MakeBool(source.checked));
        if (!source.checked) {
            entry.Set("reason", Value::MakeString(source.unchecked_reason));
        } else {
            entry.Set("word_timing", Value::MakeString(source.word_timing));
            Value targeted = Value::MakeArray();
            Value carried = Value::MakeArray();
            if (framed) {
                for (const CutWord& word : source.targeted)
                    targeted.Push(WordView(word, rate));
                for (const CutWord& word : source.carried)
                    carried.Push(WordView(word, rate));
            }
            entry.Set("targeted", std::move(targeted));
            entry.Set("carried", std::move(carried));
            if (!source.at_risk.empty()) {
                Value atRisk = Value::MakeArray();
                for (const CutWord& word : source.at_risk)
                    atRisk.Push(WordView(word, rate));
                entry.Set("at_risk", std::move(atRisk));
            }
        }
        sources.Push(std::move(entry));
    }
    root.Set("sources", std::move(sources));
    return root.Dump();
}
