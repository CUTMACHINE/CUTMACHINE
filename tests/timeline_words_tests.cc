// AGENT-2026-09 A24 -- docs/roadmap/AGENT-2026-09.md. The words of a montage
// projected from the rushes' alignments onto its audio clips: which words a
// clip covers and where they play, a word cut in two at an in or an out, the
// air at both ends, the voice no word covers, and the text of the whole.
#include "TimelineWords.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

constexpr int64_t kLoud = 1000;
constexpr int64_t kQuiet = 10;

const Ulid kSource = "01K40000000000000000000001";
const Ulid kOther = "01K40000000000000000000002";

SpeechOnsetReport Envelope(
    int64_t frames, const std::vector<std::pair<int64_t, int64_t>>& voiced) {
    SpeechOnsetReport report;
    report.media_id = kSource;
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    report.speech_level = kLoud;
    report.noise_floor = kQuiet;
    report.levels.assign(static_cast<size_t>(frames) * 2, kQuiet);
    for (const auto& [first, end] : voiced)
        for (int64_t frame = first; frame < end; ++frame) {
            report.levels[static_cast<size_t>(frame) * 2] = kLoud;
            report.levels[static_cast<size_t>(frame) * 2 + 1] = kLoud;
        }
    return report;
}

Transcript Words(const std::vector<std::pair<int64_t, int64_t>>& frames) {
    Transcript transcript;
    transcript.media_id = kSource;
    transcript.source_rate = {25, 1};
    transcript.word_timing = "nemo";
    int index = 0;
    for (const auto& [start, end] : frames) {
        TranscriptWord word;
        word.text = "w" + std::to_string(++index);
        word.start = {start, 25};
        word.end = {end, 25};
        transcript.words.push_back(word);
    }
    return transcript;
}

DocumentClip Clip(const Ulid& id, const Ulid& source, int64_t sourceIn,
                  int64_t frames, int64_t timelineIn) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.source_in = {sourceIn, 25};
    clip.duration = {frames, 25};
    clip.timeline_in = {timelineIn, 25};
    return clip;
}

}  // namespace

int main() {
    Document document;
    document.sequence.frame_rate = {25, 1};
    // Two speech clips of one rush, out of time order on purpose, and a
    // clip of a rush nobody transcribed on a second track.
    DocumentTrack speech;
    speech.id = "01K40000000000000000000010";
    speech.kind = "audio";
    speech.index = 0;
    speech.clips = {Clip("01K40000000000000000000022", kSource, 300, 44, 50),
                    Clip("01K40000000000000000000021", kSource, 100, 50, 0),
                    Clip("01K40000000000000000000025", kSource, 300, 32, 94)};
    DocumentTrack music;
    music.id = "01K40000000000000000000011";
    music.kind = "audio";
    music.index = 1;
    music.clips = {Clip("01K40000000000000000000023", kOther, 0, 90, 0)};
    DocumentTrack video;
    video.id = "01K40000000000000000000012";
    video.kind = "video";
    video.clips = {Clip("01K40000000000000000000024", kSource, 100, 50, 0)};
    document.sequence.tracks = {music, video, speech};

    // w1 straddles the first clip's in (100), w3 its out (150); w4 and w5
    // sit inside the second clip (300..340), followed by four voiced frames
    // no word covers.
    TimelineWordSource source;
    source.transcript =
        Words({{90, 105}, {110, 120}, {140, 152}, {300, 310}, {320, 336}});
    source.has_envelope = true;
    source.envelope = Envelope(400, {{90, 105},
                                     {110, 120},
                                     {140, 152},
                                     {300, 310},
                                     {320, 330},
                                     {338, 342}});
    const SpeechOnsetThresholds thresholds;
    const WordBoundaryRules rules;
    std::string error;
    Check(ResolveWordBoundaries(source.transcript, &source.envelope, thresholds,
                                rules, source.boundaries, error),
          "the fixture's boundaries resolve: " + error);
    std::map<Ulid, TimelineWordSource> sources;
    sources[kSource] = source;

    std::vector<TimelineClipWords> clips;
    Check(ResolveTimelineWords(document, sources, clips, error),
          "the montage's words resolve: " + error);
    Check(clips.size() == 4, "every audio clip is listed, video left out");
    if (clips.size() == 4) {
        const TimelineClipWords& first = clips[0];
        Check(first.clip_id == "01K40000000000000000000021" &&
                  first.track_index == 0 && first.timeline_in_frame == 0 &&
                  first.timeline_end_frame == 50 &&
                  first.source_in_frame == 100 &&
                  first.source_end_frame == 150 && first.checked,
              "clips come track by track, in time order, on both grids");
        Check(first.words.size() == 3 && first.words[0].text == "w1" &&
                  !first.words[0].whole && first.words[0].timeline_frame == 0 &&
                  first.words[1].whole && first.words[1].timeline_frame == 10 &&
                  first.words[2].text == "w3" && !first.words[2].whole &&
                  first.words[2].timeline_frame == 40,
              "a clip lists the words it covers, whole or not, where they "
              "play");
        Check(first.head_cut && first.tail_cut,
              "a word straddling the in or the out is a cut into a word");
        Check(first.has_envelope && !first.silent &&
                  first.head_air_frames == 0 && first.tail_air_frames == 0,
              "voice on the first and last frame leaves no air");
        Check(first.stray.empty(), "no stray voice inside the first clip");
        // AGENT-2026-09 A48 -- voice on 99 and 100, on 149 and 150: both
        // cuts land inside continuous voice.
        Check(first.head_in_voice && first.tail_in_voice,
              "a cut with voice on both sides is a cut inside voice");

        const TimelineClipWords& second = clips[1];
        Check(second.clip_id == "01K40000000000000000000022" &&
                  second.timeline_in_frame == 50 &&
                  second.timeline_end_frame == 94 && second.words.size() == 2 &&
                  second.words[0].whole &&
                  second.words[0].timeline_frame == 50 &&
                  second.words[1].timeline_frame == 70 && !second.head_cut &&
                  !second.tail_cut,
              "whole words project to the sequence frame they start on");
        Check(!second.head_in_voice && !second.tail_in_voice,
              "a cut placed in silence on either side is not inside voice");
        // Voice at half the speech level (-6 dB) on one side is not loud:
        // the flag needs the speech level on both sides.
        TimelineWordSource quieter = source;
        quieter.envelope.levels[99 * 2] = quieter.envelope.levels[99 * 2 + 1] =
            kLoud / 2;
        std::map<Ulid, TimelineWordSource> quieterSources;
        quieterSources[kSource] = quieter;
        std::vector<TimelineClipWords> quieterClips;
        Check(ResolveTimelineWords(document, quieterSources, quieterClips,
                                   error) &&
                  quieterClips.size() == 4 && !quieterClips[0].head_in_voice &&
                  quieterClips[0].tail_in_voice && quieterClips[0].head_cut,
              "a cut against voice under the speech level is not inside "
              "loud voice, though still a cut into the word");
        Check(second.head_air_frames == 0 && second.tail_air_frames == 2,
              "the air is counted from the last voice to the out, stray "
              "voice included: " +
                  std::to_string(second.tail_air_frames));
        Check(second.stray.size() == 1 && second.stray[0].first_frame == 338 &&
                  second.stray[0].end_frame == 342 &&
                  second.stray[0].has_previous &&
                  second.stray[0].previous_word_index == 4,
              "voice no word covers is named after the word before it");

        const TimelineClipWords& third = clips[2];
        Check(third.clip_id == "01K40000000000000000000025" &&
                  third.words.size() == 2 && !third.words[1].whole &&
                  !third.tail_cut && third.tail_air_frames == 2,
              "dropping the silent end of an aligned word is not a cut into "
              "it, and the air is counted from the voice");

        const TimelineClipWords& other = clips[3];
        Check(!other.checked && other.unchecked_reason == "no_transcript" &&
                  other.track_index == 1 && other.words.empty(),
              "a clip of an untranscribed rush is listed unchecked");
    }
    Check(
        TimelineWordsText(clips) == "w1 w2 w3 | w4 w5 | w4 w5 | [?]",
        "the montage reads as text, clip by clip: " + TimelineWordsText(clips));

    // Without an envelope the words still project; the air is unknown.
    TimelineWordSource bare;
    bare.transcript = source.transcript;
    Check(ResolveWordBoundaries(bare.transcript, nullptr, thresholds, rules,
                                bare.boundaries, error),
          "boundaries resolve without an envelope");
    sources[kSource] = bare;
    Check(ResolveTimelineWords(document, sources, clips, error) &&
              clips.size() == 4 && !clips[0].has_envelope &&
              clips[0].words.size() == 3 && clips[1].stray.empty() &&
              clips[2].tail_cut,
          "without an envelope the words project and nothing is measured");

    Document unrated = document;
    unrated.sequence.frame_rate = {0, 1};
    Check(!ResolveTimelineWords(unrated, sources, clips, error),
          "a sequence without a frame rate is refused");

    if (failures == 0) std::cout << "timeline_words: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
