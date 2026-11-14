#pragma once

#include "Document.h"
#include "SpeechOnset.h"
#include "Transcription.h"
#include "Ulid.h"
#include "WordBoundaries.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// AGENT-2026-09 A24 -- docs/roadmap/AGENT-2026-09.md. The words of a montage,
// read from the rushes' alignments instead of transcribed again.
//
// On the second run of the 25 September bench the agent wrote a script that
// walked the timeline file, opened each source's alignment and printed, clip
// by clip, the words the clip covers with their frames -- then another that
// scanned the envelope of every clip for voice outside those words. Both
// are the skill's own checks (no clip without a word, no word cut in two,
// no sound nobody wrote, no air at the joins), and both were built by hand
// because the engine only answered per source (list_source_words) or by
// transcribing the montage anew (transcribe_timeline, 5 to 30 s of model
// load each time and a text that drifts from the aligned words).
//
// This reads the same alignments and envelopes once per source and projects
// them onto every audio clip of the sequence: sound lives on audio tracks
// (Document.h), so the words a listener hears are theirs. Pure: the caller
// loads the sources (the MCP backend does) and hands them in.

struct TimelineWordSource {
    Transcript transcript;
    bool has_envelope = false;
    SpeechOnsetReport envelope;
    // ResolveWordBoundaries over `transcript`, with the envelope when there
    // is one.
    std::vector<WordBoundary> boundaries;
};

struct TimelineWord {
    size_t index = 0;
    std::string text;
    // The aligner's frames on the source grid, end exclusive.
    int64_t start_frame = 0;
    int64_t end_frame = 0;
    // False when the clip covers only part of the word.
    bool whole = true;
    bool voiced = true;
    // The sequence frame the word starts on -- the clip's own in when the
    // word began before it.
    int64_t timeline_frame = 0;
};

struct TimelineClipWords {
    Ulid clip_id;
    Ulid track_id;
    int32_t track_index = 0;
    Ulid source_id;
    // Sequence grid: the first frame the clip plays on and the frame after
    // its last.
    int64_t timeline_in_frame = 0;
    int64_t timeline_end_frame = 0;
    // Source grid, when checked: the first source frame the clip reads and
    // the frame after the last.
    int64_t source_in_frame = 0;
    int64_t source_end_frame = 0;
    // False when the source has no transcript to read; the reason says why.
    bool checked = false;
    std::string unchecked_reason;
    std::string word_timing;
    std::vector<TimelineWord> words;
    // A word straddles the clip's in, or its out, and the frames the clip
    // drops carry voice (with the envelope; without it, any straddle): a
    // cut into a word.
    bool head_cut = false;
    bool tail_cut = false;
    // AGENT-2026-09 A48. With the envelope: the clip's in, or its out, lands
    // inside loud voice -- the frame before the in and the in itself both
    // at the source's speech level, or the last frame and the one after.
    // No word straddles the cut (the aligner ends one word where the next
    // starts), but the voice does: "Alors, le portail" cut at "le" was
    // marked by the editor as an odd cut, the "-ors" of "Alors" chopped
    // against "le" at three decibels above the speech level.
    bool head_in_voice = false;
    bool tail_in_voice = false;
    // With the source's envelope: silent frames from the clip's in to its
    // first voice, and from its last voice to its out; `silent` when no
    // frame of the clip carries voice.
    bool has_envelope = false;
    bool silent = false;
    int64_t head_air_frames = 0;
    int64_t tail_air_frames = 0;
    // Voice inside the clip that no word covers (VoiceOutsideWords), in
    // source frames.
    std::vector<VoiceWithoutWord> stray;
};

// Every clip of every audio track of `document`, tracks in index order and
// clips in time order, with the words of `sources` (keyed by source id) the
// clip covers. A clip whose source is not in `sources` is listed unchecked.
// Refuses a sequence without a frame rate.
bool ResolveTimelineWords(const Document& document,
                          const std::map<Ulid, TimelineWordSource>& sources,
                          std::vector<TimelineClipWords>& clips,
                          std::string& error);

// The montage as text: the words of each checked clip in order, clips
// separated by " | ", an unchecked clip shown as "[?]".
std::string TimelineWordsText(const std::vector<TimelineClipWords>& clips);
