#pragma once

#include "Operations.h"
#include "SpeechOnset.h"
#include "Transcription.h"

#include <cstdint>
#include <string>
#include <vector>

// AGENT-2026-09 A4 -- ROADMAP.md. Where to enter and leave on a word, read
// off the source's aligned words and its measured envelope together.
//
// The skill that drives interview edits has carried these rules for weeks:
// an aligner's start lands 0 to 2 frames after the attack but its end 3 to
// 10 frames before the voice drops, so an exit on the aligner's end clips
// the word; and a start that sits in silence wants the attack with two
// frames of air. On every measured run the agent applied them by hand, with
// Python over the transcript cache and the envelope cache, a dozen times a
// montage. That is arithmetic the engine can do (PHILOSOPHY.md principle 7),
// and here it does, per word, so that a boundary is a lookup.
//
// The voice is read on the source's own frame grid: a frame is voiced when
// any envelope window it touches reaches the source's speech level times
// SpeechOnsetThresholds::speech_ratio_percent -- the line the rest of the
// engine uses (SummarizeClipSpeechOnset), not a fixed decibel figure.
//
// The rules, per word, with s the aligner's start and e its exclusive end:
//   entry: if frame s is silent, the first voiced frame after it (the
//          attack) less `entry_air_frames`; if s is voiced, the first silent
//          frame before it, one more if that one is silent too; no silence
//          within `entry_search_frames` back, s itself.
//   exit:  if frame e-1 is voiced, the first silent frame at or after e (the
//          drop), never past the next word's start; if e-1 is already
//          silent, the last voiced frame plus one plus `exit_air_frames`,
//          never past e. A word with no voiced frame keeps s and e and says
//          so: it is misaligned, not a place to cut.
// Without an envelope every word keeps the aligner's frames and says so.
//
// Read-only and pure: no filesystem, no operation. What to do with a
// boundary stays the caller's.

struct WordBoundaryRules {
    // Silent frames looked for before a voiced start, before the start is
    // kept as it is.
    int64_t entry_search_frames = 10;
    // Frames looked ahead of a silent start for the attack.
    int64_t attack_search_frames = 25;
    // Air kept in front of an attack found ahead of a silent start.
    int64_t entry_air_frames = 2;
    // Air kept after the last voiced frame of a word whose aligner's end
    // already sits in silence.
    int64_t exit_air_frames = 3;
    // Frames looked ahead of a voiced end for the drop.
    int64_t drop_search_frames = 25;
};

struct WordBoundary {
    size_t index = 0;
    std::string text;
    // The aligner's frames on the source grid, end exclusive.
    int64_t start_frame = 0;
    int64_t end_frame = 0;
    // Where to cut: in at entry_frame, out before exit_frame.
    int64_t entry_frame = 0;
    int64_t exit_frame = 0;
    // "attack_ahead", "silence_before", "start_kept", "no_voice" or
    // "no_envelope".
    std::string entry_rule;
    // "drop_ahead", "next_word", "voice_before", "end_kept", "no_voice" or
    // "no_envelope".
    std::string exit_rule;
    // Whether any frame of [start_frame, end_frame) is voiced; true without
    // an envelope, since nothing says otherwise.
    bool voiced = true;
    // Frames between this word's end and the next word's start, as the
    // aligner has them; zero for the last word (`has_next` says which).
    int64_t gap_after_frames = 0;
    bool has_next = false;
};

// The level a window must reach to count as voice for these rules: the
// source's speech level times the thresholds' ratio.
int64_t WordBoundaryThreshold(const SpeechOnsetReport& envelope,
                              const SpeechOnsetThresholds& thresholds);

// Whether frame `frame` of a source at `rate` holds voice: any envelope
// window it touches reaches `threshold`. A frame outside the envelope is
// silent.
bool FrameIsVoiced(const SpeechOnsetReport& envelope, int64_t threshold,
                   const MediaRate& rate, int64_t frame);

// The loudest envelope window frame `frame` of a source at `rate` touches,
// or zero outside the envelope: what a caller reads to see the voice where
// a rule decided.
int64_t FrameLevel(const SpeechOnsetReport& envelope, const MediaRate& rate,
                   int64_t frame);

// One boundary per word of `transcript`, in order. `envelope` may be null:
// then every word keeps the aligner's frames ("no_envelope"). Refuses a
// transcript without a frame rate, an envelope of another source or of no
// grid, and a word whose times are not on the source's frame grid.
bool ResolveWordBoundaries(const Transcript& transcript,
                           const SpeechOnsetReport* envelope,
                           const SpeechOnsetThresholds& thresholds,
                           const WordBoundaryRules& rules,
                           std::vector<WordBoundary>& boundaries,
                           std::string& error);

// AGENT-2026-09 A18 -- ROADMAP.md. Voice the envelope hears where no word
// is: a run of voiced frames that no word's [entry, exit) covers, at least
// `minimumFrames` long, inside [firstFrame, endFrame). A "hmm" nobody
// wrote, a breath that carries, a word the transcriber dropped: the skill's
// "no sound outside words" check, which an agent ran as a Python scan of
// the envelope on a real montage.
struct VoiceWithoutWord {
    int64_t first_frame = 0;
    int64_t end_frame = 0;
    // The last word that ends at or before the run, when there is one.
    bool has_previous = false;
    size_t previous_word_index = 0;
};
std::vector<VoiceWithoutWord> VoiceOutsideWords(
    const std::vector<WordBoundary>& boundaries,
    const SpeechOnsetReport& envelope, const SpeechOnsetThresholds& thresholds,
    const MediaRate& rate, int64_t firstFrame, int64_t endFrame,
    int64_t minimumFrames);

// AGENT-2026-09 A43 -- ROADMAP.md. A word said twice that the transcript
// wrote once. "l'Horloge... l'Horloge du Rivage": whisper kept one word,
// the aligner stretched it over both utterances (1.16 s), and every run cut
// inside it and kept the stutter. An aligned word of at least
// `leastWordFrames` holding a hollow of at least `leastHollowFrames` frames
// under the speech line between two stretches of voice (the first of at
// least `leastVoiceFrames`, the second of at least `leastSecondFrames`) is
// a restart: the cut removes the first utterance whole, from the word's
// start to `restart_frame`, and the second starts clean, as an editor keeps
// the last take. Only words overlapping [firstFrame, endFrame) are read.
struct WordRestart {
    size_t word_index = 0;
    int64_t first_frame = 0;  // The aligned word's start.
    int64_t end_frame = 0;    // The aligned word's end, exclusive.
    int64_t hollow_first_frame = 0;
    int64_t restart_frame = 0;  // First voiced frame of the second utterance.
};
struct WordRestartRules {
    int64_t least_word_frames = 16;
    int64_t least_voice_frames = 3;
    int64_t least_hollow_frames = 3;
    int64_t least_second_frames = 5;
};
std::vector<WordRestart> FindWordRestarts(
    const Transcript& transcript, const SpeechOnsetReport& envelope,
    const SpeechOnsetThresholds& thresholds, const MediaRate& rate,
    int64_t firstFrame, int64_t endFrame,
    const WordRestartRules& rules = WordRestartRules{});

// AGENT-2026-09 A33, A47, A48 -- docs/roadmap/AGENT-2026-09.md. The frames
// a cut takes for a held stretch of voice [firstFrame, endFrame) -- a
// hesitation candidate (Hesitations.h) -- placed against the aligned words
// and the envelope.
//
// The hold the detector reports ends where the timbre starts moving again;
// the voice does not. On the second native run of the 26th, every cut made
// on a candidate's frames left the hold's decay behind it -- five frames of
// "souvent-euh" before "dormir", twelve of "rivaaage" before "tranquille"
// -- and the editor marked each as a "euh" still played. So the cut runs on
// through the voiced frames that follow the hold, across a break of at most
// `bridge_frames`, until the next aligned word starts (`voice_end_frame`).
// A word that straddles the hold's end stops it there.
//
// The cut never enters a neighbouring aligned word (A33, restored by A48
// ter): a word under way where the cut starts pushes the cut to its
// aligned end, and a word starting inside the cut ends it. A33 ter had
// let the cut into the previous word's tail; unbounded, a hold that began
// seven frames before the aligned end of "rivage" cut the word to "riva"
// (A48 bis), and bounded to two frames it still clipped "fenêtre" and
// "rangé" to the editor's ear. `keep_frames` frames of the hold's head
// stay for a stretched syllable inside its word, so the word still starts
// (A47); a stretch reaching the end of its word is its last vowel and only
// the decay past the aligned end goes. `withinWord` names the word a
// stretched syllable belongs to; it is never a neighbour.
//
// An aligned word wholly inside the hold is listed in `words_inside`: on
// the same run a "le" of one frame, placed by the aligner inside a
// six-frame hold after "fermer", was kept with two frames of hold
// around it and heard as the "euh" it was. `cut_including_words_inside` is
// the cut with those words taken along; the caller decides.
struct HeldCutRules {
    int64_t keep_frames = 0;
    int64_t bridge_frames = 1;
};
struct HeldCut {
    int64_t first_frame = 0;
    int64_t end_frame = 0;
    int64_t voice_end_frame = 0;
    // The cut; none when cut_end_frame <= cut_first_frame.
    int64_t cut_first_frame = 0;
    int64_t cut_end_frame = 0;
    bool clamped = false;
    std::vector<size_t> words_inside;
    int64_t cut_including_words_inside_first_frame = 0;
    int64_t cut_including_words_inside_end_frame = 0;
};
HeldCut ResolveHeldCut(const Transcript& transcript,
                       const SpeechOnsetReport* envelope,
                       const SpeechOnsetThresholds& thresholds,
                       const MediaRate& rate, int64_t firstFrame,
                       int64_t endFrame, const std::string& withinWord,
                       const HeldCutRules& rules = HeldCutRules{});

// AGENT-2026-09 A10 -- ROADMAP.md. Widens each range of a word removal,
// resolved by ResolveWordRemoval from the aligner's frames, to the voice
// the envelope hears: from the first word's entry to the last word's exit
// (ResolveWordBoundaries), never narrower than the aligner's range, never
// past the next kept word nor into the previous word's aligner frames, never
// towards a neighbour aligned on one or two frames (AX-2026-09: its voice is
// what lies there), and inside [clipStart, clipEnd). An aligner's
// end arrives 3 to 10 frames before the voice drops, so a removal cut on it
// left one or two frames of the word behind ("donc", on a real montage).
// Ranges that come to touch or overlap are merged. `operation.ranges` must
// hold one range per entry of `wordRanges`, in the same order. Returns the
// frames added, through `widenedFrames`.
bool WidenWordRemovalToVoice(
    const Transcript& transcript, const SpeechOnsetReport& envelope,
    const SpeechOnsetThresholds& thresholds, const WordBoundaryRules& rules,
    const std::vector<WordRange>& wordRanges, const RationalTime& clipStart,
    const RationalTime& clipEnd, RemoveWordsOperation& operation,
    int64_t& widenedFrames, std::string& error);
