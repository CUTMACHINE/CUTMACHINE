#pragma once

#include "Transcription.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ITW3 -- ROADMAP.md. Word boundaries from a forced aligner, read by the
// engine instead of beside it.
//
// The word-alignment bench (docs/WORD_ALIGNMENT_DIAGNOSTIC.md) settled the
// split: Whisper supplies the text, NeMo Forced Aligner the boundaries, which
// cut the boundary error from 296 ms to 60 ms on the pilot. The skill's
// mfa_align.py already writes that result as a cache beside the Whisper one,
// but nothing in the engine read it: create_interview_short, remove_words,
// get_timeline_transcript and the SRT export all kept cutting on Whisper, so
// on the ITW-2026-09 interview every boundary was placed by hand.
//
// The cache is the bench manifest (schema_version 1): one file per media and
// aligned text under `.cutmachine/word-alignments/<media_id>/`, words as
// half-open [frame_start, frame_end) on the source frame grid. It replaces
// the Whisper words only when it still describes the same text -- the letters
// of both word lists agree, or the alignment says its text deliberately
// replaced Whisper's. A transcript regenerated after the alignment therefore
// falls back to Whisper instead of wearing stale boundaries.
//
// Read-only. Nothing here writes either cache.

struct ForcedAlignedWord {
    std::string text;
    int64_t frame_start = 0;
    int64_t frame_end = 0;
};

struct WordAlignment {
    std::string path;
    std::string media_id;
    MediaRate source_rate{0, 1};
    // "mfa", "nemo" or "gold": any aligner the bench scores. "cutmachine"
    // (Whisper's own boundaries) is refused, since it would replace Whisper
    // with itself.
    std::string method;
    std::string model;
    int64_t audio_duration_frames = 0;
    // The aligner was given text other than the Whisper cache's, because the
    // cache left measured speech without words. Its words then win outright.
    bool text_override = false;
    std::vector<ForcedAlignedWord> words;
};

// Parses and validates one manifest: required fields, words positive,
// ordered, non-overlapping and inside the audio duration.
bool LoadWordAlignment(const std::string& path, WordAlignment& alignment,
                       std::string& error);

// True when `alignment` may stand in for `transcript`'s words. `reason`
// says why not otherwise.
bool WordAlignmentMatchesTranscript(const WordAlignment& alignment,
                                    const Transcript& transcript,
                                    std::string& reason);

// Replaces the transcript's words with the aligned ones and records the
// provenance in Transcript::word_timing. Metadata (model, verbatim, speech
// assessment) is kept: it still describes where the text came from.
void ApplyWordAlignment(const WordAlignment& alignment, Transcript& transcript);

// Applies the newest alignment in `alignmentDirectory` (one media's
// `word-alignments/<media_id>`) that matches `transcript`. Returns whether
// one was applied; a missing directory or no match leaves it untouched.
bool ApplyNewestWordAlignment(const std::filesystem::path& alignmentDirectory,
                              Transcript& transcript);

// ALIGNEMENT-2026-11 -- docs/roadmap/ALIGNEMENT-2026-11.md. Whether a
// transcript's words carry a forced aligner's boundaries (Transcript::
// word_timing): "mfa" since MFA replaced NeMo, "nemo" for the caches NeMo
// wrote before, "gold" for a hand-placed one. The rules that read an
// aligner's word ranges (restarts, a cut held inside a word) apply to all
// three, never to Whisper's own.
bool IsForcedAlignedTiming(const std::string& wordTiming);

// The one reader of a source's words for word-level tools: the Whisper cache
// at `<cacheDirectory>/transcripts/<media_id>.json`, with the newest matching
// alignment under `<cacheDirectory>/word-alignments/<media_id>/` applied.
// `cacheDirectory` is the project's `.cutmachine` directory. An alignment
// that does not match is ignored, never an error: Whisper words remain a
// usable answer.
bool LoadSourceWords(const std::filesystem::path& cacheDirectory,
                     const std::string& mediaId, Transcript& transcript,
                     std::string& error);
