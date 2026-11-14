#pragma once

// IA3 -- docs/roadmap/IA-2026-09.md. Choosing ideas, not cues, and never on
// a stale reading.
//
// The spans of the transcript view (InterviewShort.h) are subtitle cues of
// about 42 characters, and their aliases S1, S2... are recomputed on every
// read. Two failures came of that on a real delivery (QC-2026-09b; the
// words here are made up): a film that opened on "...le four à bois" -- a
// selection that started mid-sentence on a cue edge -- and one that kept
// "on travaille pareil" after the sentence that "pareil" referred to had
// been cut. And nothing stopped an S12 read before an edit,
// or from another timeline, from naming another passage once the view had
// moved.
//
// This header answers the three:
//  - blocks: runs of consecutive spans that belong together, cut where a
//    sentence ends, where the speaker pauses longer than a breath or where
//    the material stops being one stretch of one source. A block says when
//    its text opens or closes mid-sentence;
//  - a revision: the fingerprint of the view a selection was read from. Every
//    tool that takes a span or block id requires it, and a selection read
//    from any other view is refused as stale, never re-resolved;
//  - a selection's context: for each selected segment, what the same rush
//    says right before and right after it, and whether the selection keeps
//    it. The engine reports; the model judges references, causality and
//    transitions. No pronoun becomes a deterministic refusal.
//
// Times stay the engine's: a segment resolves through
// ResolveTranscriptSpanRange, the same path span runs already take.

#include "InterviewShort.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The fingerprint of a transcript view: a hash over every span's id, source,
// exact times, text and reservations. It is not a document revision on
// purpose. An edit that moves no span (a grade, a title) leaves every id
// designating the same passage, so it leaves the revision alone; an edit, a
// transcription or an alignment that moves one span, or renumbers them,
// changes it. Two timelines with the same spans share it, and their ids do
// designate the same passages.
std::string TranscriptViewRevision(
    const std::vector<TimelineTranscriptSpan>& spans);

// Whether a span's text ends a sentence: a final '.', '?', '!' or '…',
// closing quotes and brackets aside.
bool TextEndsSentence(const std::string& text);

// Whether a text starts on a lowercase letter, leading punctuation aside:
// Whisper capitalises the start of a sentence, so "le four à bois" is the
// middle of one.
bool TextStartsLowercase(const std::string& text);

struct SpeechBlock {
    // "B1", "B2"... in timeline order, valid for one view revision.
    std::string block_id;
    std::string first_span_id;
    std::string last_span_id;
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    std::string text;
    // The text starts mid-sentence (a lowercase letter, or a span of the
    // same stretch right before it that ends no sentence), and the block
    // played before it in the view is not the start of that sentence. A
    // jump cut inside a sentence does not set it; a film that opens on
    // "le four à bois" does.
    bool opens_mid_sentence = false;
    // The last span ends no sentence, and the block played after it is not
    // the rest of it.
    bool closes_mid_sentence = false;
    bool straddles_cut = false;
    bool likely_hallucinated = false;
    bool likely_incomplete = false;
};

// Groups the view's spans (timeline order, as BuildTimelineTranscriptSpans
// returns them) into blocks. Consecutive spans share a block when they come
// from one source, the second starts within kSubtitleCueMaximumGap after the
// first in that source, and the first does not end a sentence. That gap is
// the one ResolveTranscriptSpanRange tolerates, so any block, and any run of
// blocks that crosses no longer pause, resolves to one exact range.
std::vector<SpeechBlock> BuildSpeechBlocks(
    const std::vector<TimelineTranscriptSpan>& spans);

// One requested segment: a run of spans or a run of blocks, never both.
// The end ids are optional (a run of one).
struct SpeechSegmentRequest {
    std::string span_id;
    std::string end_span_id;
    std::string block_id;
    std::string end_block_id;
};

// What the rush says next to a segment, as the view shows it: the nearest
// span of the same source before it (after it), with the rest of that span's
// block on the far side, so a sentence is read whole.
struct SpeechNeighbour {
    bool present = false;
    std::string block_id;
    // The span right against the segment.
    std::string span_id;
    std::string text;
    // Source time between that span and the segment.
    RationalTime gap;
    // Some segment of the selection keeps that span.
    bool kept = false;
    // The segment played right before (after) this one ends (starts) with
    // it: the viewer hears it just before (after).
    bool heard_next_to_it = false;
};

// A segment's mid-sentence flags read the film the selection builds: what
// plays around a segment is the segments next to it in the selection.
struct SpeechSelectionSegment {
    std::string first_span_id;
    std::string last_span_id;
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    std::string text;
    bool opens_mid_sentence = false;
    bool closes_mid_sentence = false;
    bool straddles_cut = false;
    bool likely_hallucinated = false;
    bool likely_incomplete = false;
    SpeechNeighbour before;
    SpeechNeighbour after;
    // Other segments (by index) that replay some of the same material.
    std::vector<size_t> overlaps;
};

enum class SpeechSelectionError : uint8_t { None, Stale, Invalid };

// The error name a stale selection is refused with, on every surface.
inline constexpr char kStaleSelectionError[] = "StaleSelection";

// Resolves a selection read from the view of `revision` against the current
// view `spans`. A different revision is refused as Stale before any id is
// looked at: a positional id is only meaningful in the view it was read
// from. `path` prefixes each error ("create_interview_short.segments").
bool ResolveSpeechSelection(const std::vector<TimelineTranscriptSpan>& spans,
                            const std::string& revision,
                            const std::vector<SpeechSegmentRequest>& requests,
                            const std::string& path,
                            std::vector<SpeechSelectionSegment>& segments,
                            SpeechSelectionError& kind, std::string& error);

// The message a stale revision gets, shared by every tool that checks one.
std::string StaleSelectionMessage(const std::string& given,
                                  const std::string& current);

std::string SerializeSpeechBlocks(const Ulid& timelineId,
                                  const std::string& revision,
                                  const std::vector<SpeechBlock>& blocks);

std::string SerializeSpeechSelection(
    const Ulid& timelineId, const std::string& revision,
    const std::vector<SpeechSelectionSegment>& segments);
