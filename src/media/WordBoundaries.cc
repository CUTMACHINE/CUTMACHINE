#include "WordBoundaries.h"

#include "WordAlignment.h"

#include <algorithm>
#include <exception>
#include <limits>

int64_t WordBoundaryThreshold(const SpeechOnsetReport& envelope,
                              const SpeechOnsetThresholds& thresholds) {
    return envelope.speech_level * thresholds.speech_ratio_percent / 100;
}

bool FrameIsVoiced(const SpeechOnsetReport& envelope, int64_t threshold,
                   const MediaRate& rate, int64_t frame) {
    if (frame < 0 || envelope.windows_per_second == 0 || rate.num <= 0 ||
        rate.den <= 0)
        return false;
    // Frame f covers [f, f+1) * den / num seconds; window k covers
    // [k, k+1) / wps. The windows touched are those whose start is before
    // the frame's end and whose end is after the frame's start.
    const __int128 wps = envelope.windows_per_second;
    const __int128 first =
        static_cast<__int128>(frame) * rate.den * wps / rate.num;
    __int128 last = (static_cast<__int128>(frame) + 1) * rate.den * wps;
    // Exclusive: the window that starts exactly at the frame's end is not
    // touched.
    last = (last + rate.num - 1) / rate.num;
    const __int128 count = static_cast<__int128>(envelope.levels.size());
    for (__int128 window = first; window < last && window < count; ++window) {
        if (envelope.levels[static_cast<size_t>(window)] >= threshold)
            return true;
    }
    return false;
}

int64_t FrameLevel(const SpeechOnsetReport& envelope, const MediaRate& rate,
                   int64_t frame) {
    if (frame < 0 || envelope.windows_per_second == 0 || rate.num <= 0 ||
        rate.den <= 0)
        return 0;
    const __int128 wps = envelope.windows_per_second;
    const __int128 first =
        static_cast<__int128>(frame) * rate.den * wps / rate.num;
    __int128 last = (static_cast<__int128>(frame) + 1) * rate.den * wps;
    last = (last + rate.num - 1) / rate.num;
    const __int128 count = static_cast<__int128>(envelope.levels.size());
    int64_t level = 0;
    for (__int128 window = first; window < last && window < count; ++window)
        level = std::max(level, envelope.levels[static_cast<size_t>(window)]);
    return level;
}

std::vector<VoiceWithoutWord> VoiceOutsideWords(
    const std::vector<WordBoundary>& boundaries,
    const SpeechOnsetReport& envelope, const SpeechOnsetThresholds& thresholds,
    const MediaRate& rate, int64_t firstFrame, int64_t endFrame,
    int64_t minimumFrames) {
    std::vector<VoiceWithoutWord> runs;
    if (firstFrame < 0) firstFrame = 0;
    if (endFrame <= firstFrame || minimumFrames < 1) return runs;
    const int64_t threshold = WordBoundaryThreshold(envelope, thresholds);
    // Frames a word covers: the aligner's frames widened to the voice, so a
    // drop tail is the word's, not a stray sound.
    const auto covered = [&](int64_t frame) {
        for (const WordBoundary& word : boundaries) {
            const int64_t start = std::min(word.start_frame, word.entry_frame);
            const int64_t end = std::max(word.end_frame, word.exit_frame);
            if (frame >= start && frame < end) return true;
            if (start > frame) break;
        }
        return false;
    };
    size_t nextWord = 0;
    int64_t runStart = -1;
    const auto close = [&](int64_t frame) {
        if (runStart < 0) return;
        if (frame - runStart >= minimumFrames) {
            VoiceWithoutWord run;
            run.first_frame = runStart;
            run.end_frame = frame;
            while (nextWord < boundaries.size() &&
                   boundaries[nextWord].end_frame <= runStart)
                ++nextWord;
            if (nextWord > 0) {
                run.has_previous = true;
                run.previous_word_index = nextWord - 1;
            }
            runs.push_back(run);
        }
        runStart = -1;
    };
    for (int64_t frame = firstFrame; frame < endFrame; ++frame) {
        const bool stray =
            FrameIsVoiced(envelope, threshold, rate, frame) && !covered(frame);
        if (stray && runStart < 0) runStart = frame;
        if (!stray) close(frame);
    }
    close(endFrame);
    return runs;
}

bool WidenWordRemovalToVoice(
    const Transcript& transcript, const SpeechOnsetReport& envelope,
    const SpeechOnsetThresholds& thresholds, const WordBoundaryRules& rules,
    const std::vector<WordRange>& wordRanges, const RationalTime& clipStart,
    const RationalTime& clipEnd, RemoveWordsOperation& operation,
    int64_t& widenedFrames, std::string& error) {
    widenedFrames = 0;
    if (operation.ranges.size() != wordRanges.size()) {
        error = "word removal ranges do not match the words named";
        return false;
    }
    std::vector<WordBoundary> boundaries;
    if (!ResolveWordBoundaries(transcript, &envelope, thresholds, rules,
                               boundaries, error))
        return false;
    const MediaRate rate = transcript.source_rate;
    std::vector<WordRemovalRange> widened;
    for (size_t index = 0; index < wordRanges.size(); ++index) {
        const WordRange& words = wordRanges[index];
        if (words.end_word_index >= boundaries.size() ||
            words.start_word_index > words.end_word_index) {
            error = "word range indices are out of bounds";
            return false;
        }
        WordRemovalRange range = operation.ranges[index];
        const RationalTime entry{
            boundaries[words.start_word_index].entry_frame * rate.den,
            rate.num};
        const RationalTime exit{
            boundaries[words.end_word_index].exit_frame * rate.den, rate.num};
        RationalTime start =
            entry < range.source_start ? entry : range.source_start;
        RationalTime end = exit > range.source_end ? exit : range.source_end;
        // AX-2026-09 -- nor into the voice of a neighbour the aligner made one
        // or two frames long (A45): its voice runs past those frames, and the
        // entry and exit rules handed it to the removed word. "pour", aligned
        // on frame 309, lost its voice twice to the removal of "justement",
        // aligned from 317 and widened back to 310. On that side the removal
        // keeps the aligner's own edge.
        constexpr int64_t kShortWordFrames = 2;
        const auto alignedShort = [&](size_t word) {
            return boundaries[word].end_frame - boundaries[word].start_frame <=
                   kShortWordFrames;
        };
        if (words.start_word_index > 0 &&
            alignedShort(words.start_word_index - 1) &&
            start < range.source_start)
            start = range.source_start;
        if (words.end_word_index + 1 < boundaries.size() &&
            alignedShort(words.end_word_index + 1) && end > range.source_end)
            end = range.source_end;
        // Never into the previous word's own frames as the aligner has
        // them: the frames backed over are silent, but a cut that overlaps
        // the aligner's range of a kept word would be reported as cutting
        // into it.
        if (words.start_word_index > 0) {
            const RationalTime previousEnd{
                boundaries[words.start_word_index - 1].end_frame * rate.den,
                rate.num};
            if (start < previousEnd && previousEnd < range.source_start)
                start = previousEnd;
        }
        if (start < clipStart) start = clipStart;
        if (end > clipEnd) end = clipEnd;
        if (!(start < end)) {
            error = "a widened range is empty";
            return false;
        }
        try {
            widenedFrames += end.sub(start).to_frames(rate.num, rate.den) -
                             range.source_end.sub(range.source_start)
                                 .to_frames(rate.num, rate.den);
        } catch (const std::exception& exception) {
            error = std::string("widened range has no frame count: ") +
                    exception.what();
            return false;
        }
        if (!widened.empty() && start <= widened.back().source_end) {
            if (end > widened.back().source_end)
                widened.back().source_end = end;
        } else {
            widened.push_back({start, end});
        }
    }
    operation.ranges = std::move(widened);
    return true;
}

namespace {

bool WordFrames(const TranscriptWord& word, const MediaRate& rate,
                int64_t& start, int64_t& end, std::string& error) {
    try {
        start = word.start.to_frames(rate.num, rate.den);
        end = word.end.to_frames(rate.num, rate.den);
        // A time that is not on the grid maps to the frame holding it: the
        // aligner's caches are frame-exact, Whisper's are not, and either
        // way the frame is what a cut can express.
    } catch (const std::exception& exception) {
        error = "word '" + word.text + "' has no frame: " + exception.what();
        return false;
    }
    if (end < start) end = start;
    return true;
}

}  // namespace

bool ResolveWordBoundaries(const Transcript& transcript,
                           const SpeechOnsetReport* envelope,
                           const SpeechOnsetThresholds& thresholds,
                           const WordBoundaryRules& rules,
                           std::vector<WordBoundary>& boundaries,
                           std::string& error) {
    boundaries.clear();
    error.clear();
    const MediaRate rate = transcript.source_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "transcript '" + transcript.media_id + "' has no frame rate";
        return false;
    }
    if (envelope != nullptr) {
        if (envelope->media_id != transcript.media_id) {
            error = "speech envelope belongs to another source";
            return false;
        }
        if (envelope->windows_per_second == 0) {
            error = "speech envelope has no analysis grid";
            return false;
        }
    }
    if (rules.entry_search_frames < 0 || rules.attack_search_frames < 0 ||
        rules.entry_air_frames < 0 || rules.exit_air_frames < 0 ||
        rules.drop_search_frames < 0) {
        error = "word boundary rules must not be negative";
        return false;
    }
    const int64_t threshold =
        envelope != nullptr ? WordBoundaryThreshold(*envelope, thresholds) : 0;
    const auto voiced = [&](int64_t frame) {
        return FrameIsVoiced(*envelope, threshold, rate, frame);
    };

    std::vector<int64_t> starts(transcript.words.size());
    std::vector<int64_t> ends(transcript.words.size());
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        if (!WordFrames(transcript.words[index], rate, starts[index],
                        ends[index], error))
            return false;
    }

    boundaries.reserve(transcript.words.size());
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        WordBoundary boundary;
        boundary.index = index;
        boundary.text = transcript.words[index].text;
        boundary.start_frame = starts[index];
        boundary.end_frame = ends[index];
        boundary.entry_frame = starts[index];
        boundary.exit_frame = ends[index];
        boundary.has_next = index + 1 < transcript.words.size();
        boundary.gap_after_frames =
            boundary.has_next ? starts[index + 1] - ends[index] : 0;
        if (envelope == nullptr) {
            boundary.entry_rule = "no_envelope";
            boundary.exit_rule = "no_envelope";
            boundaries.push_back(std::move(boundary));
            continue;
        }
        const int64_t start = starts[index];
        const int64_t end = ends[index];
        bool anyVoiced = false;
        for (int64_t frame = start; frame < end && !anyVoiced; ++frame)
            anyVoiced = voiced(frame);
        boundary.voiced = anyVoiced;
        if (!anyVoiced) {
            boundary.entry_rule = "no_voice";
            boundary.exit_rule = "no_voice";
            boundaries.push_back(std::move(boundary));
            continue;
        }

        // Entry.
        if (!voiced(start)) {
            int64_t attack = -1;
            for (int64_t frame = start + 1;
                 frame <= start + rules.attack_search_frames && frame < end;
                 ++frame) {
                if (voiced(frame)) {
                    attack = frame;
                    break;
                }
            }
            if (attack >= 0) {
                boundary.entry_frame =
                    std::max(start, attack - rules.entry_air_frames);
                boundary.entry_rule = "attack_ahead";
            } else {
                boundary.entry_rule = "start_kept";
            }
        } else {
            int64_t entry = -1;
            for (int64_t frame = start - 1;
                 frame >= 0 && frame >= start - rules.entry_search_frames;
                 --frame) {
                if (!voiced(frame)) {
                    entry = frame;
                    if (frame - 1 >= 0 && !voiced(frame - 1)) entry = frame - 1;
                    break;
                }
            }
            if (entry >= 0) {
                boundary.entry_frame = entry;
                boundary.entry_rule = "silence_before";
            } else {
                boundary.entry_rule = "start_kept";
            }
        }

        // Exit.
        const int64_t nextStart = boundary.has_next
                                      ? starts[index + 1]
                                      : std::numeric_limits<int64_t>::max();
        if (end > start && voiced(end - 1)) {
            int64_t drop = -1;
            for (int64_t frame = end; frame <= end + rules.drop_search_frames;
                 ++frame) {
                if (frame >= nextStart) break;
                if (!voiced(frame)) {
                    drop = frame;
                    break;
                }
            }
            if (drop >= 0) {
                boundary.exit_frame = drop;
                boundary.exit_rule = "drop_ahead";
            } else if (nextStart <= end + rules.drop_search_frames) {
                boundary.exit_frame = std::max(end, nextStart);
                boundary.exit_rule = "next_word";
            } else {
                boundary.exit_rule = "end_kept";
            }
        } else {
            int64_t lastVoiced = -1;
            for (int64_t frame = end - 1; frame >= start; --frame) {
                if (voiced(frame)) {
                    lastVoiced = frame;
                    break;
                }
            }
            if (lastVoiced >= 0) {
                boundary.exit_frame =
                    std::min(end, lastVoiced + 1 + rules.exit_air_frames);
                boundary.exit_rule = "voice_before";
            } else {
                boundary.exit_rule = "end_kept";
            }
        }
        boundaries.push_back(std::move(boundary));
    }
    return true;
}

std::vector<WordRestart> FindWordRestarts(
    const Transcript& transcript, const SpeechOnsetReport& envelope,
    const SpeechOnsetThresholds& thresholds, const MediaRate& rate,
    int64_t firstFrame, int64_t endFrame, const WordRestartRules& rules) {
    std::vector<WordRestart> restarts;
    if (rate.num <= 0 || rate.den <= 0 || envelope.windows_per_second == 0)
        return restarts;
    const int64_t threshold = WordBoundaryThreshold(envelope, thresholds);
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        const TranscriptWord& word = transcript.words[index];
        const int64_t first = word.start.to_frames(rate.num, rate.den);
        const int64_t end = word.end.to_frames(rate.num, rate.den);
        if (end <= firstFrame || first >= endFrame) continue;
        if (end - first < rules.least_word_frames) continue;
        int64_t voiced = 0;
        int64_t hollow = 0;
        int64_t hollowStart = -1;
        int64_t restart = -1;
        for (int64_t frame = first; frame < end && restart < 0; ++frame) {
            if (FrameIsVoiced(envelope, threshold, rate, frame)) {
                if (hollow >= rules.least_hollow_frames &&
                    voiced >= rules.least_voice_frames)
                    restart = frame;
                else if (hollow > 0)
                    hollow = 0;
                ++voiced;
            } else {
                if (hollow == 0) hollowStart = frame;
                ++hollow;
            }
        }
        if (restart < 0 || end - restart < rules.least_second_frames) continue;
        restarts.push_back({index, first, end, hollowStart, restart});
    }
    return restarts;
}

HeldCut ResolveHeldCut(const Transcript& transcript,
                       const SpeechOnsetReport* envelope,
                       const SpeechOnsetThresholds& thresholds,
                       const MediaRate& rate, int64_t firstFrame,
                       int64_t endFrame, const std::string& withinWord,
                       const HeldCutRules& rules) {
    HeldCut cut;
    cut.first_frame = firstFrame;
    cut.end_frame = endFrame;
    cut.voice_end_frame = endFrame;
    cut.cut_first_frame = firstFrame;
    cut.cut_end_frame = firstFrame;
    cut.cut_including_words_inside_first_frame = firstFrame;
    cut.cut_including_words_inside_end_frame = firstFrame;
    if (endFrame <= firstFrame || rate.num <= 0 || rate.den <= 0) return cut;
    struct Placed {
        size_t index;
        int64_t first;
        int64_t end;
    };
    std::vector<Placed> words;
    for (size_t index = 0; index < transcript.words.size(); ++index) {
        const TranscriptWord& word = transcript.words[index];
        if (!withinWord.empty() && word.text == withinWord) continue;
        words.push_back({index, word.start.to_frames(rate.num, rate.den),
                         word.end.to_frames(rate.num, rate.den)});
    }
    const auto inside = [&](const Placed& word) {
        return word.first >= firstFrame && word.end <= endFrame;
    };
    for (const Placed& word : words)
        if (inside(word)) cut.words_inside.push_back(word.index);
    // A stretched syllable keeps the head of its own word, not of the hold:
    // a hold that begins in the previous word's tail would keep that tail
    // and take the word's attack. And a hold that ends before its word
    // does is followed by the rest of that word ("la maa...rée"), not by
    // a decay: no extension then.
    int64_t withinStart = firstFrame;
    int64_t withinEnd = endFrame;
    bool withinFound = false;
    bool wordGoesOn = false;
    if (!withinWord.empty()) {
        for (const TranscriptWord& word : transcript.words) {
            const int64_t wordFirst = word.start.to_frames(rate.num, rate.den);
            const int64_t wordEnd = word.end.to_frames(rate.num, rate.den);
            if (word.text != withinWord || wordFirst >= endFrame ||
                wordEnd <= firstFrame)
                continue;
            withinStart = std::max(withinStart, wordFirst);
            withinEnd = wordEnd;
            withinFound = true;
            wordGoesOn = wordEnd > endFrame;
            break;
        }
    }
    // The voice that follows the hold, up to the next aligned word. A word
    // that straddles the hold's end is that next word already under way.
    if (envelope != nullptr && envelope->windows_per_second > 0 &&
        !wordGoesOn) {
        int64_t limit = std::numeric_limits<int64_t>::max();
        bool straddled = false;
        for (const Placed& word : words) {
            if (word.first >= endFrame)
                limit = std::min(limit, word.first);
            else if (word.end > endFrame)
                straddled = true;
        }
        if (!straddled) {
            const int64_t threshold =
                WordBoundaryThreshold(*envelope, thresholds);
            int64_t last = endFrame;
            int64_t gap = 0;
            for (int64_t frame = endFrame; frame < limit; ++frame) {
                if (FrameIsVoiced(*envelope, threshold, rate, frame)) {
                    last = frame + 1;
                    gap = 0;
                } else if (++gap > rules.bridge_frames) {
                    break;
                }
            }
            cut.voice_end_frame = last;
        }
    }
    // The clamp to the aligned words (A33), run with every word and then
    // without the words inside the hold.
    const auto clamp = [&](bool withInside, int64_t& cutFirst,
                           int64_t& cutEnd) {
        const int64_t keep =
            std::min(rules.keep_frames, endFrame - firstFrame - 1);
        cutFirst = withinStart + std::max<int64_t>(0, keep);
        // A48 ter -- a stretch that reaches the end of its word is that
        // word's last vowel: the word keeps its whole aligned range and
        // only the decay past it goes. Two frames taken off the end of
        // "utilisé" were still a clipped word to the editor's ear.
        if (withinFound && !wordGoesOn)
            cutFirst = std::max(cutFirst, withinEnd);
        cutEnd = cut.voice_end_frame;
        bool clamped = false;
        if (!IsForcedAlignedTiming(transcript.word_timing)) return clamped;
        // Whether a word is under way where the cut starts is read against
        // the cut's own start, not against a start a previous word has
        // already pushed: the next word would otherwise pass for one under
        // way and push the cut past itself.
        const int64_t origin = cutFirst;
        for (const Placed& word : words) {
            if (!withInside && inside(word)) continue;
            if (word.end <= cutFirst || word.first >= cutEnd) continue;
            // Never inside an aligned word (A33, restored by A48 ter). A33
            // ter had let the cut into the previous word's tail because
            // an aligner runs that word over a "euh" whisper did not
            // write ("souvent-euh") and the cut came back empty; since
            // A48 the cut runs on to the next word anyway, so the two
            // frames of "euh" the aligner glued to the word are the price
            // of a word never clipped -- two frames taken off "fenêtre"
            // and "rangé" were what the editor marked. A word that
            // starts inside the cut is the next word: the cut stops where
            // it starts.
            if (word.first < origin + 2) {
                if (word.end > cutFirst) {
                    cutFirst = word.end;
                    clamped = true;
                }
            } else {
                cutEnd = std::min(cutEnd, word.first);
                clamped = true;
            }
        }
        return clamped;
    };
    cut.clamped = clamp(true, cut.cut_first_frame, cut.cut_end_frame);
    clamp(false, cut.cut_including_words_inside_first_frame,
          cut.cut_including_words_inside_end_frame);
    return cut;
}
