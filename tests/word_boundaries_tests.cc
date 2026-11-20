// AGENT-2026-09 A4 -- docs/roadmap/AGENT-2026-09.md. The skill's rules for
// entering and leaving on a word, now in the engine: read against a
// synthetic envelope where every frame's voice is known, so each rule is
// checked on the case it was written for.
#include "WordBoundaries.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

constexpr int64_t kLoud = 1000;
constexpr int64_t kQuiet = 10;

// A 25 fps source with a 50-window grid: two windows per frame. `voiced`
// lists the frames that carry voice; every other frame is room tone.
SpeechOnsetReport Envelope(int64_t frames, const std::vector<int64_t>& voiced) {
    SpeechOnsetReport report;
    report.media_id = "01K40000000000000000000001";
    report.windows_per_second = 50;
    report.decode_sample_rate = 16000;
    report.speech_level = kLoud;
    report.noise_floor = kQuiet;
    report.levels.assign(static_cast<size_t>(frames) * 2, kQuiet);
    for (int64_t frame : voiced) {
        report.levels[static_cast<size_t>(frame) * 2] = kLoud;
        report.levels[static_cast<size_t>(frame) * 2 + 1] = kLoud;
    }
    return report;
}

Transcript Words(const std::vector<std::pair<int64_t, int64_t>>& frames) {
    Transcript transcript;
    transcript.media_id = "01K40000000000000000000001";
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

std::vector<int64_t> Range(int64_t first, int64_t end) {
    std::vector<int64_t> frames;
    for (int64_t frame = first; frame < end; ++frame) frames.push_back(frame);
    return frames;
}

std::vector<int64_t> Join(std::vector<int64_t> left,
                          const std::vector<int64_t>& right) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

}  // namespace

int main() {
    const SpeechOnsetThresholds thresholds;
    const WordBoundaryRules rules;
    std::vector<WordBoundary> boundaries;
    std::string error;

    // ---- the frame grid ------------------------------------------------
    {
        const SpeechOnsetReport envelope = Envelope(10, {3});
        const int64_t threshold = WordBoundaryThreshold(envelope, thresholds);
        Check(threshold == kLoud * thresholds.speech_ratio_percent / 100,
              "the threshold is the source's speech level times the ratio");
        Check(FrameIsVoiced(envelope, threshold, {25, 1}, 3) &&
                  !FrameIsVoiced(envelope, threshold, {25, 1}, 2) &&
                  !FrameIsVoiced(envelope, threshold, {25, 1}, 4),
              "a frame is voiced by the windows it covers, not its "
              "neighbours'");
        Check(!FrameIsVoiced(envelope, threshold, {25, 1}, 10) &&
                  !FrameIsVoiced(envelope, threshold, {25, 1}, -1),
              "a frame outside the envelope is silent");
        // At 30000/1001 the frames and the 50-window grid do not align:
        // frame 3 spans 100.1 to 133.5 ms, windows 5 (100-120) and 6.
        SpeechOnsetReport ntsc = envelope;
        ntsc.levels.assign(20, kQuiet);
        ntsc.levels[5] = kLoud;
        Check(FrameIsVoiced(ntsc, threshold, {30000, 1001}, 3) &&
                  !FrameIsVoiced(ntsc, threshold, {30000, 1001}, 4),
              "a fractional frame rate reads the windows it touches");
    }

    // ---- entry: a start in silence takes the attack, two frames early --
    {
        // The aligner says the word starts at 10; the voice arrives at 14.
        const SpeechOnsetReport envelope = Envelope(40, Range(14, 20));
        Check(ResolveWordBoundaries(Words({{10, 20}}), &envelope, thresholds,
                                    rules, boundaries, error),
              "a start in silence resolves: " + error);
        Check(boundaries.size() == 1 && boundaries[0].entry_frame == 12 &&
                  boundaries[0].entry_rule == "attack_ahead",
              "entry is the attack less two frames of air");
        Check(boundaries[0].voiced, "the word is voiced");
        // The attack right after the start keeps the start.
        const SpeechOnsetReport close = Envelope(40, Range(11, 20));
        ResolveWordBoundaries(Words({{10, 20}}), &close, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].entry_frame == 10 &&
                  boundaries[0].entry_rule == "attack_ahead",
              "the air never reaches before the aligner's start");
    }

    // ---- entry: a voiced start backs up to the silence before it --------
    {
        // Voice from 8: the aligner's 10 is late; 7 and 6 are silent.
        const SpeechOnsetReport envelope = Envelope(40, Range(8, 20));
        ResolveWordBoundaries(Words({{10, 20}}), &envelope, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].entry_frame == 6 &&
                  boundaries[0].entry_rule == "silence_before",
              "entry backs up to the first silent frame, and one more when "
              "that one is silent too: " +
                  std::to_string(boundaries[0].entry_frame));
        // Only one silent frame before the voice: the one before it is
        // voice from another word.
        const SpeechOnsetReport tight =
            Envelope(40, Join(Range(0, 7), Range(8, 20)));
        ResolveWordBoundaries(Words({{10, 20}}), &tight, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].entry_frame == 7,
              "one silent frame is taken alone when its neighbour is voiced");
        // Voice all the way back: no silence within ten frames.
        const SpeechOnsetReport continuous = Envelope(40, Range(0, 20));
        ResolveWordBoundaries(Words({{10, 20}}), &continuous, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].entry_frame == 10 &&
                  boundaries[0].entry_rule == "start_kept",
              "without silence within reach the aligner's start is kept");
    }

    // ---- exit: a voiced end runs on to the drop -------------------------
    {
        // The aligner ends the word at 20; the voice drops at 26.
        const SpeechOnsetReport envelope = Envelope(40, Range(10, 26));
        ResolveWordBoundaries(Words({{10, 20}}), &envelope, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].exit_frame == 26 &&
                  boundaries[0].exit_rule == "drop_ahead",
              "exit is the first silent frame after the aligner's end");
        // The next word starts before the drop: never beyond its start.
        ResolveWordBoundaries(Words({{10, 20}, {23, 30}}), &envelope,
                              thresholds, rules, boundaries, error);
        Check(boundaries.size() == 2 && boundaries[0].exit_frame == 23 &&
                  boundaries[0].exit_rule == "next_word",
              "exit never passes the next word's start");
        Check(boundaries[0].has_next && boundaries[0].gap_after_frames == 3 &&
                  !boundaries[1].has_next &&
                  boundaries[1].gap_after_frames == 0,
              "the aligner's gap to the next word is reported");
        // Voice that never drops within reach and no next word.
        const SpeechOnsetReport endless = Envelope(60, Range(10, 60));
        ResolveWordBoundaries(Words({{10, 20}}), &endless, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].exit_frame == 20 &&
                  boundaries[0].exit_rule == "end_kept",
              "without a drop within reach the aligner's end is kept");
    }

    // ---- exit: an end already in silence pulls back to the voice --------
    {
        // Voice from 10 to 15; the aligner's end at 20 sits in silence.
        const SpeechOnsetReport envelope = Envelope(40, Range(10, 15));
        ResolveWordBoundaries(Words({{10, 20}}), &envelope, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].exit_frame == 18 &&
                  boundaries[0].exit_rule == "voice_before",
              "exit is the last voiced frame plus three frames of air: " +
                  std::to_string(boundaries[0].exit_frame));
        const SpeechOnsetReport late = Envelope(40, Range(10, 19));
        ResolveWordBoundaries(Words({{10, 20}}), &late, thresholds, rules,
                              boundaries, error);
        Check(boundaries[0].exit_frame == 20,
              "the air never reaches past the aligner's end");
    }

    // ---- a word with no voice at all -----------------------------------
    {
        const SpeechOnsetReport envelope = Envelope(40, Range(30, 40));
        ResolveWordBoundaries(Words({{10, 20}}), &envelope, thresholds, rules,
                              boundaries, error);
        Check(!boundaries[0].voiced && boundaries[0].entry_frame == 10 &&
                  boundaries[0].exit_frame == 20 &&
                  boundaries[0].entry_rule == "no_voice" &&
                  boundaries[0].exit_rule == "no_voice",
              "a word the envelope does not hear keeps its frames and says "
              "so");
    }

    // ---- without an envelope -------------------------------------------
    {
        Check(ResolveWordBoundaries(Words({{10, 20}, {22, 30}}), nullptr,
                                    thresholds, rules, boundaries, error),
              "words resolve without an envelope: " + error);
        Check(boundaries.size() == 2 && boundaries[0].entry_frame == 10 &&
                  boundaries[0].exit_frame == 20 &&
                  boundaries[0].entry_rule == "no_envelope" &&
                  boundaries[1].exit_rule == "no_envelope" &&
                  boundaries[0].gap_after_frames == 2,
              "without an envelope every word keeps the aligner's frames");
    }

    // ---- refusals -------------------------------------------------------
    {
        SpeechOnsetReport other = Envelope(40, Range(10, 20));
        other.media_id = "01K40000000000000000000002";
        Check(!ResolveWordBoundaries(Words({{10, 20}}), &other, thresholds,
                                     rules, boundaries, error) &&
                  error.find("another source") != std::string::npos,
              "an envelope of another source is refused: " + error);
        SpeechOnsetReport gridless = Envelope(40, Range(10, 20));
        gridless.windows_per_second = 0;
        Check(!ResolveWordBoundaries(Words({{10, 20}}), &gridless, thresholds,
                                     rules, boundaries, error),
              "an envelope without a grid is refused");
        Transcript rateless = Words({{10, 20}});
        rateless.source_rate = {0, 1};
        Check(!ResolveWordBoundaries(rateless, nullptr, thresholds, rules,
                                     boundaries, error) &&
                  error.find("frame rate") != std::string::npos,
              "a transcript without a frame rate is refused: " + error);
        WordBoundaryRules negative;
        negative.entry_air_frames = -1;
        Check(!ResolveWordBoundaries(Words({{10, 20}}), nullptr, thresholds,
                                     negative, boundaries, error),
              "negative rules are refused");
    }

    // ---- the level a caller reads ---------------------------------------
    {
        const SpeechOnsetReport envelope = Envelope(10, {3});
        Check(FrameLevel(envelope, {25, 1}, 3) == kLoud &&
                  FrameLevel(envelope, {25, 1}, 2) == kQuiet &&
                  FrameLevel(envelope, {25, 1}, 10) == 0,
              "a frame's level is its loudest window, zero outside");
    }

    // ---- widening a removal to the voice ---------------------------------
    {
        // Three words; the aligner ends the second at 30 while its voice
        // runs to 34 and the third word starts at 36: removing the second
        // word on the aligner's frames leaves four voiced frames behind.
        const SpeechOnsetReport envelope = Envelope(
            60, Join(Range(10, 20), Join(Range(22, 34), Range(36, 50))));
        const Transcript words = Words({{10, 20}, {22, 30}, {36, 50}});
        RemoveWordsOperation removal;
        removal.ranges = {{{22, 25}, {30, 25}}};
        int64_t widened = 0;
        Check(WidenWordRemovalToVoice(words, envelope, thresholds, rules,
                                      {{1, 1}}, {0, 25}, {60, 25}, removal,
                                      widened, error),
              "a removal widens: " + error);
        Check(removal.ranges.size() == 1 &&
                  removal.ranges[0].source_start == RationalTime{20, 25} &&
                  removal.ranges[0].source_end == RationalTime{34, 25} &&
                  widened == 6,
              "the range runs from the silence before the word to its drop: " +
                  std::to_string(removal.ranges[0].source_start.value) +
                  " to " + std::to_string(removal.ranges[0].source_end.value));
        // Never past the next kept word, never outside the clip.
        RemoveWordsOperation tail;
        tail.ranges = {{{36, 25}, {50, 25}}};
        Check(WidenWordRemovalToVoice(words, envelope, thresholds, rules,
                                      {{2, 2}}, {0, 25}, {48, 25}, tail,
                                      widened, error) &&
                  tail.ranges[0].source_end == RationalTime{48, 25} &&
                  tail.ranges[0].source_start == RationalTime{34, 25},
              "a widened range stays inside the clip");
        // Two adjacent words removed as two ranges come to touch: one
        // range.
        RemoveWordsOperation both;
        both.ranges = {{{10, 25}, {20, 25}}, {{22, 25}, {30, 25}}};
        Check(WidenWordRemovalToVoice(words, envelope, thresholds, rules,
                                      {{0, 0}, {1, 1}}, {0, 25}, {60, 25}, both,
                                      widened, error) &&
                  both.ranges.size() == 1 &&
                  both.ranges[0].source_start == RationalTime{8, 25} &&
                  both.ranges[0].source_end == RationalTime{34, 25},
              "ranges that come to touch are merged, the first backed up "
              "to the silence before it: " +
                  std::to_string(both.ranges.size()) + " from " +
                  std::to_string(both.ranges[0].source_start.value));
        // AX-2026-09 -- "pour" aligned on one frame, then continuous voice
        // up to "justement": that voice is the short word's, the removal of
        // the long one keeps the aligner's edge on its side.
        const SpeechOnsetReport continuous =
            Envelope(60, Join(Range(10, 20), Range(22, 42)));
        const Transcript shortBefore = Words({{10, 20}, {22, 23}, {30, 40}});
        RemoveWordsOperation justement;
        justement.ranges = {{{30, 25}, {40, 25}}};
        Check(WidenWordRemovalToVoice(shortBefore, continuous, thresholds,
                                      rules, {{2, 2}}, {0, 25}, {60, 25},
                                      justement, widened, error) &&
                  justement.ranges[0].source_start == RationalTime{30, 25} &&
                  justement.ranges[0].source_end >= RationalTime{40, 25},
              "no widening towards a word aligned on one frame: " +
                  std::to_string(justement.ranges[0].source_start.value));
        const Transcript shortAfter = Words({{10, 20}, {22, 30}, {37, 38}});
        RemoveWordsOperation before;
        before.ranges = {{{22, 25}, {30, 25}}};
        Check(WidenWordRemovalToVoice(shortAfter, continuous, thresholds, rules,
                                      {{1, 1}}, {0, 25}, {60, 25}, before,
                                      widened, error) &&
                  before.ranges[0].source_end == RationalTime{30, 25},
              "nor after it, towards one aligned on one frame: " +
                  std::to_string(before.ranges[0].source_end.value));
        RemoveWordsOperation mismatched;
        mismatched.ranges = {{{10, 25}, {20, 25}}};
        Check(!WidenWordRemovalToVoice(words, envelope, thresholds, rules,
                                       {{0, 0}, {1, 1}}, {0, 25}, {60, 25},
                                       mismatched, widened, error),
              "ranges and words must match one to one");
    }

    // ---- voice where no word is ------------------------------------------
    {
        // Two words, a "hmm" of 8 frames between them that nobody wrote,
        // and a 2-frame click after the second that is too short to count.
        const SpeechOnsetReport envelope =
            Envelope(80, Join(Join(Range(10, 20), Range(26, 34)),
                              Join(Range(40, 50), Range(55, 57))));
        const Transcript words = Words({{10, 20}, {40, 50}});
        std::vector<WordBoundary> bounds;
        Check(ResolveWordBoundaries(words, &envelope, thresholds, rules, bounds,
                                    error),
              "boundaries resolve for the stray-voice case: " + error);
        const std::vector<VoiceWithoutWord> stray =
            VoiceOutsideWords(bounds, envelope, thresholds, {25, 1}, 0, 80, 4);
        Check(stray.size() == 1 && stray[0].first_frame == 26 &&
                  stray[0].end_frame == 34 && stray[0].has_previous &&
                  stray[0].previous_word_index == 0,
              "a voiced run between two words that no word covers is "
              "reported after the word before it: " +
                  std::to_string(stray.size()));
        Check(VoiceOutsideWords(bounds, envelope, thresholds, {25, 1}, 0, 80, 1)
                      .size() == 2,
              "the minimum length decides what counts");
        Check(
            VoiceOutsideWords(bounds, envelope, thresholds, {25, 1}, 35, 80, 4)
                .empty(),
            "only the frames asked for are scanned");
    }

    // AGENT-2026-09 A43 -- a word said twice that the transcript wrote once:
    // a long aligned word with a hollow between two stretches of voice is a
    // restart; a word without the hollow, or too short, is not.
    {
        const Transcript twice = Words({{10, 40}, {45, 60}, {62, 70}});
        const SpeechOnsetReport envelope =
            Envelope(100, Join(Join(Range(10, 19), Range(23, 40)),
                               Join(Range(45, 60), Range(62, 70))));
        const std::vector<WordRestart> restarts =
            FindWordRestarts(twice, envelope, thresholds, {25, 1}, 0, 100);
        Check(restarts.size() == 1 && restarts[0].word_index == 0 &&
                  restarts[0].first_frame == 10 &&
                  restarts[0].hollow_first_frame == 19 &&
                  restarts[0].restart_frame == 23 &&
                  restarts[0].end_frame == 40,
              "a hollow inside a long aligned word marks the restart of its "
              "second utterance");
        Check(FindWordRestarts(twice, envelope, thresholds, {25, 1}, 41, 100)
                  .empty(),
              "only words overlapping the range are read");
        const SpeechOnsetReport held = Envelope(100, Range(10, 40));
        Check(
            FindWordRestarts(twice, held, thresholds, {25, 1}, 0, 100).empty(),
            "a long word voiced throughout is a stretch, not a restart");
    }

    // AGENT-2026-09 A48 -- the cut of a held stretch runs on through the
    // voice that follows it, up to the next aligned word; a word the
    // aligner placed inside the hold is named, with the cut that takes it
    // along.
    {
        // w1 [10,20), w2 [40,50), w3 [52,60); voice on 10..23, a one-frame
        // dip, 25..30, then 40..59. A hold of three frames after w1.
        const Transcript spoken = Words({{10, 20}, {40, 50}, {52, 60}});
        const SpeechOnsetReport decaying = Envelope(
            100, Join(Join(Range(10, 24), Range(25, 31)), Range(40, 60)));
        HeldCut held =
            ResolveHeldCut(spoken, &decaying, thresholds, {25, 1}, 20, 23, "");
        Check(held.first_frame == 20 && held.end_frame == 23 &&
                  held.voice_end_frame == 31 && held.cut_first_frame == 20 &&
                  held.cut_end_frame == 31 && !held.clamped &&
                  held.words_inside.empty(),
              "the cut runs to the end of the voice after the hold, across "
              "a dip of one frame: " +
                  std::to_string(held.voice_end_frame));
        const SpeechOnsetReport glued = Envelope(100, Range(10, 60));
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 20, 23, "");
        Check(held.voice_end_frame == 40 && held.cut_end_frame == 40 &&
                  !held.clamped,
              "voice running on to the next aligned word stops there");
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 20, 45, "");
        Check(held.voice_end_frame == 45 && held.cut_first_frame == 20 &&
                  held.cut_end_frame == 40 && held.clamped,
              "a word under way at the hold's end takes no extension and "
              "ends the cut where it starts");
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 38, 51, "");
        Check(held.words_inside.size() == 1 && held.words_inside[0] == 1 &&
                  held.cut_first_frame == 38 && held.cut_end_frame == 40 &&
                  held.clamped && held.voice_end_frame == 52 &&
                  held.cut_including_words_inside_first_frame == 38 &&
                  held.cut_including_words_inside_end_frame == 52,
              "a word wholly inside the hold is named; the cut keeps it, "
              "the alternative takes it along to the next word");
        held = ResolveHeldCut(spoken, nullptr, thresholds, {25, 1}, 20, 23, "");
        Check(held.voice_end_frame == 23 && held.cut_end_frame == 23,
              "without an envelope the cut is the hold itself");
        HeldCutRules stretched;
        stretched.keep_frames = 2;
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 12, 20, "w1",
                              stretched);
        Check(held.cut_first_frame == 20 && held.cut_end_frame == 40 &&
                  !held.clamped,
              "a stretch reaching the end of its word keeps the whole word: "
              "only the decay past its aligned end goes");
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 8, 16, "w1",
                              stretched);
        Check(held.cut_first_frame == 12 && held.cut_end_frame == 16,
              "the frames kept are the word's own head, not the hold's, "
              "when the hold begins before the word");
        // A48 ter -- a filler that begins seven frames before the previous
        // word's aligned end starts at that end.
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 13, 23, "");
        Check(held.cut_first_frame == 20 && held.cut_end_frame == 40 &&
                  held.clamped,
              "the previous word keeps its whole aligned range: " +
                  std::to_string(held.cut_first_frame));
        held = ResolveHeldCut(spoken, &glued, thresholds, {25, 1}, 12, 16, "w1",
                              stretched);
        Check(held.voice_end_frame == 16 && held.cut_first_frame == 14 &&
                  held.cut_end_frame == 16,
              "a hold that ends before its word does is followed by the "
              "rest of the word, not by a decay");
        Transcript loose = spoken;
        loose.word_timing = "whisper";
        held = ResolveHeldCut(loose, &glued, thresholds, {25, 1}, 20, 45, "");
        Check(held.cut_first_frame == 20 && held.cut_end_frame == 45 &&
                  !held.clamped,
              "only aligned words clamp the cut");
    }

    if (failures == 0) std::cout << "word_boundaries: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
