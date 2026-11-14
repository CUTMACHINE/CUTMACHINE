#include "TimelineWords.h"

#include <algorithm>

namespace {

// Frames are counted on a grid as CutWords counts them: a range starts on
// the frame holding its start and ends after the frame holding its end.
int64_t FirstFrame(const RationalTime& time, const MediaRate& rate) {
    return time.to_frames(rate.num, rate.den);
}

int64_t EndFrame(const RationalTime& time, const MediaRate& rate) {
    const int64_t frame = time.to_frames(rate.num, rate.den);
    const RationalTime frameStart{frame * rate.den, rate.num};
    return time.sub(frameStart).value == 0 ? frame : frame + 1;
}

bool ValidRate(const MediaRate& rate) { return rate.num > 0 && rate.den > 0; }

}  // namespace

bool ResolveTimelineWords(const Document& document,
                          const std::map<Ulid, TimelineWordSource>& sources,
                          std::vector<TimelineClipWords>& clips,
                          std::string& error) {
    clips.clear();
    error.clear();
    const MediaRate sequenceRate = document.sequence.frame_rate;
    if (!ValidRate(sequenceRate)) {
        error = "the sequence has no frame rate";
        return false;
    }
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "audio") tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    const SpeechOnsetThresholds thresholds;
    for (const DocumentTrack* track : tracks) {
        std::vector<const DocumentClip*> ordered;
        for (const DocumentClip& clip : track->clips) ordered.push_back(&clip);
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [](const DocumentClip* left, const DocumentClip* right) {
                return left->timeline_in.sub(right->timeline_in).value < 0;
            });
        for (const DocumentClip* clip : ordered) {
            TimelineClipWords item;
            item.clip_id = clip->id;
            item.track_id = track->id;
            item.track_index = track->index;
            item.source_id = clip->source_id;
            item.timeline_in_frame =
                FirstFrame(clip->timeline_in, sequenceRate);
            item.timeline_end_frame =
                EndFrame(clip->timeline_in.add(clip->duration), sequenceRate);
            const auto found = sources.find(clip->source_id);
            if (found == sources.end()) {
                item.unchecked_reason = "no_transcript";
                clips.push_back(std::move(item));
                continue;
            }
            const TimelineWordSource& source = found->second;
            const MediaRate rate = source.transcript.source_rate;
            if (!ValidRate(rate)) {
                item.unchecked_reason = "no_source_rate";
                clips.push_back(std::move(item));
                continue;
            }
            item.checked = true;
            item.word_timing = source.transcript.word_timing;
            const int64_t in = FirstFrame(clip->source_in, rate);
            const int64_t end =
                EndFrame(clip->source_in.add(clip->duration), rate);
            item.source_in_frame = in;
            item.source_end_frame = end;
            for (const WordBoundary& boundary : source.boundaries) {
                if (boundary.end_frame <= in || boundary.start_frame >= end)
                    continue;
                TimelineWord word;
                word.index = boundary.index;
                word.text = boundary.text;
                word.start_frame = boundary.start_frame;
                word.end_frame = boundary.end_frame;
                word.whole =
                    boundary.start_frame >= in && boundary.end_frame <= end;
                word.voiced = boundary.voiced;
                // Where the word starts on the sequence: its own start, or
                // the clip's in for a word that began before it.
                const RationalTime wordStart{
                    std::max(boundary.start_frame, in) * rate.den, rate.num};
                RationalTime offset = wordStart.sub(clip->source_in);
                if (offset.value < 0) offset = RationalTime{0, 1};
                word.timeline_frame = clip->timeline_in.add(offset).to_frames(
                    sequenceRate.num, sequenceRate.den);
                item.words.push_back(std::move(word));
            }
            if (!item.words.empty()) {
                item.head_cut = item.words.front().start_frame < in;
                item.tail_cut = item.words.back().end_frame > end;
            }
            if (source.has_envelope) {
                item.has_envelope = true;
                const int64_t threshold =
                    WordBoundaryThreshold(source.envelope, thresholds);
                // A cut is into a word only when the frames it drops carry
                // voice: an aligner's end can run past the drop into the
                // silence (measured: "Rivage," ending 4 frames after the
                // voice fell to the floor), and cutting there cuts nothing.
                const auto voicedIn = [&](int64_t first, int64_t last) {
                    for (int64_t frame = first; frame < last; ++frame)
                        if (FrameIsVoiced(source.envelope, threshold, rate,
                                          frame))
                            return true;
                    return false;
                };
                if (item.head_cut)
                    item.head_cut =
                        voicedIn(item.words.front().start_frame, in);
                if (item.tail_cut)
                    item.tail_cut = voicedIn(end, item.words.back().end_frame);
                // A48 -- loud on both sides: at the speech level (its 90th
                // percentile) less one decibel. Voice alone flags most of
                // the cuts of a fast speaker (45 of 80 edges on the marked
                // montage); at the speech level it flagged nine, the one
                // the editor marked loudest of all, three decibels above.
                constexpr int64_t kLoudPercent = 89;
                const int64_t loud =
                    source.envelope.speech_level * kLoudPercent / 100;
                const auto loudAt = [&](int64_t frame) {
                    return FrameIsVoiced(source.envelope, loud, rate, frame);
                };
                item.head_in_voice = in > 0 && loudAt(in - 1) && loudAt(in);
                item.tail_in_voice = end > in && loudAt(end - 1) && loudAt(end);
                int64_t first = in;
                while (first < end &&
                       !FrameIsVoiced(source.envelope, threshold, rate, first))
                    ++first;
                if (first == end) {
                    item.silent = true;
                    item.head_air_frames = end - in;
                    item.tail_air_frames = 0;
                } else {
                    item.head_air_frames = first - in;
                    int64_t last = end;
                    while (last > in &&
                           !FrameIsVoiced(source.envelope, threshold, rate,
                                          last - 1))
                        --last;
                    item.tail_air_frames = end - last;
                }
                // At least 160 ms, as list_source_words scans a rush.
                const int64_t minimumFrames =
                    (16 * static_cast<int64_t>(rate.num) +
                     100 * static_cast<int64_t>(rate.den) - 1) /
                    (100 * static_cast<int64_t>(rate.den));
                item.stray = VoiceOutsideWords(
                    source.boundaries, source.envelope, thresholds, rate, in,
                    end, std::max<int64_t>(1, minimumFrames));
            }
            clips.push_back(std::move(item));
        }
    }
    return true;
}

std::string TimelineWordsText(const std::vector<TimelineClipWords>& clips) {
    std::string text;
    for (const TimelineClipWords& clip : clips) {
        if (!text.empty()) text += " | ";
        if (!clip.checked) {
            text += "[?]";
            continue;
        }
        bool first = true;
        for (const TimelineWord& word : clip.words) {
            if (!first) text += ' ';
            first = false;
            text += word.text;
        }
    }
    return text;
}
