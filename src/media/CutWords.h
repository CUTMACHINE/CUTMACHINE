#pragma once

#include "Document.h"
#include "RationalTime.h"
#include "Transcription.h"
#include "Ulid.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// IA9 -- ROADMAP.md. The words an edit took with it.
//
// clean_disfluencies names the hesitations it removes. Every other tool that
// cuts sound -- tighten_pauses, trim_boundary_air, close_junction_air,
// remove_source_ranges, ripple_trim -- answered with bounds and clip ids, so
// an agent learnt that a word had gone only by rereading the whole text, and
// a one-letter preposition does not show on a reread. On the 16 September
// bench five of them went that way and were found by diffing text by hand.
//
// The removed material is read from the documents, not from the operation:
// per source, the stretches the audio tracks cover fewer times after the
// edit than before. That holds for every operation without restating what
// each one does -- a ripple trim, the fragments RemoveWords leaves, and the
// sync-locked tracks that follow a cut alike -- and a rush used twice only
// counts as lost where one of its copies is. Video tracks are left out:
// sound lives exclusively on audio tracks (Document.h), so picture trimmed
// from a speech rush takes no word the listener hears.
//
// Words are only named from a forced alignment. Whisper's boundaries drift
// by up to 1.5 s, so "this word was touched" read from them would be a guess
// presented as a measurement; without an alignment the source is reported as
// unchecked, with its removed frames, rather than as an empty word list.
//
// Pure: no filesystem. Callers load the words (LoadSourceWords or the MCP
// backend) for the sources this returns.

struct RemovedSourceRange {
    Ulid source_id;
    // The source's own frame grid, for reporting in frames.
    MediaRate source_rate{0, 1};
    RationalTime start;
    RationalTime end;
};

// Merged, ordered ranges per source, sources in id order.
std::vector<RemovedSourceRange> RemovedAudioMaterial(const Document& before,
                                                     const Document& after);

struct CutWord {
    size_t word_index = 0;
    std::string text;
    RationalTime start;
    RationalTime end;
    // False when the cut only took part of the word.
    bool whole = false;
};

struct SourceCutWords {
    Ulid source_id;
    MediaRate source_rate{0, 1};
    std::vector<RemovedSourceRange> removed;
    bool checked = false;
    std::string word_timing;
    std::string unchecked_reason;
    // Words the caller named (remove_words ranges, the hesitations
    // clean_disfluencies found), and every other word the cut touched.
    std::vector<CutWord> targeted;
    std::vector<CutWord> carried;
    // AGENT-2026-09 A45 -- a word the cut did not touch but that the
    // aligner made one or two frames long, within three frames of a cut:
    // its voice runs past those frames (a "pour" aligned on one frame lost
    // its voice to the removal of "justement" on a real montage), and the
    // report could not say so.
    std::vector<CutWord> at_risk;
};

struct CutWordsReport {
    // True when every source that lost sound had aligned words to check.
    bool checked = true;
    std::vector<SourceCutWords> sources;
};

// Inclusive word index ranges a caller asked to remove, per source.
using TargetedWords = std::map<Ulid, std::vector<WordRange>>;

// `transcripts` holds the words of the sources that have any; a source of
// `removed` absent from it has no transcript.
CutWordsReport CheckCutWords(const std::vector<RemovedSourceRange>& removed,
                             const std::map<Ulid, Transcript>& transcripts,
                             const TargetedWords& targeted);

// Frames are counted on each source's own grid: a range or word starts on
// the frame holding its start and ends after the frame holding its end.
std::string SerializeCutWordsReport(const CutWordsReport& report);
