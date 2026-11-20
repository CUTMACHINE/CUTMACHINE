#pragma once

#include "Document.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ALPHA-2026-08 -- Read-only semantic view for agent-driven interview edits.
// Every selectable span carries source-exact times copied from the transcript;
// the model selects ranges but never computes an edit boundary.

// One selectable stretch of speech on the audible timeline. `span_id` is the
// only handle a caller is meant to use: the exact times are published so a
// human reading the view can check them, not so a model can retype them.
// ResolveTranscriptSpanRange is what turns ids back into times, and it is
// the only intended path from one to the other (PHILOSOPHY.md principle 7).
struct TimelineTranscriptSpan {
    std::string span_id;  // "S1", "S2", ... in timeline order.
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    std::string text;
    // B6 -- ROADMAP.md. True when at least one word in this span crossed an
    // edit boundary. ITW3 attributes such a word to the clip that plays its
    // start, or, when its start was cut away, to the clip that plays the
    // rest of it.
    bool straddles_cut = false;
    // B12 -- inherited from the source transcript cache. Selectable text is
    // still shown for diagnosis, but editing tools must require an explicit
    // force override before trusting words no measured speech group supports.
    bool likely_hallucinated = false;
    // A9 -- visible warning that measured speech contains substantially more
    // material than this inference returned. Unlike hallucination, this does
    // not make an existing span unsafe to cut; it warns that spans are absent.
    bool likely_incomplete = false;
};

// ITW3 -- ROADMAP.md. A word the source transcript places in the montage but
// that no reading of the montage's own audio contains. On the ITW-2026-09
// interview, NeMo put an unheard "pour" on the edge of a clip; projecting
// its words onto the edit meant cross-checking them by hand against two
// transcribe_timeline readings. Such a word is reported here and left out of
// the spans, so it reaches neither the agent's view nor the SRT.
struct UnheardWord {
    Ulid clip_id;
    Ulid source_id;
    std::string text;
    RationalTime source_in;
    RationalTime timeline_in;
    // The part the montage plays, so a caption gap can be matched to it.
    RationalTime duration;
};

// BENCH-2026-09 -- ROADMAP.md. Whether a reading of the montage writes the
// same word as the transcript. On a real run "mélangé" was dropped as
// unheard because the verbatim reading wrote "mélangés".
//
// Both texts are folded first: case (accented capitals included), ASCII and
// typographic punctuation, apostrophes and hyphens never count, and the two
// latter split a word ("l'homme" is "l" and "homme"). Two words are the same
// when they share a folded token; they are inflections of each other when a
// token of each is one stem of at least three letters followed by nothing,
// "e", "s", "es" or "x" (mélangé/mélangés/mélangée, grand/grandes,
// beau/beaux). Nothing else is tolerated: accents still count (mangé is not
// mange), and so does any other ending (quel/quelle, travail/travaux), so a
// short or merely similar word is never taken for another (le/les, de/des).
enum class HeardWordMatch : uint8_t { None, Exact, Inflected };
HeardWordMatch MatchHeardWord(const std::string& written,
                              const std::string& heard);

// A word kept only because a reading writes it with another inflection.
// Reported so the tolerance stays visible.
struct InflectedWord {
    Ulid clip_id;
    std::string text;
    std::string heard_as;
    RationalTime timeline_in;
};

// BENCH-2026-09 -- ROADMAP.md. A word the forced aligner placed in material
// the edit removed, right against a clip edge, while every reading of the
// montage audio hears it there. On a real run two short words were lost this
// way: NeMo had put them in a hesitation and a closed pause that were cut,
// the montage sound still said them, and no report named them.
//
// Such a word is reported, never put back into the spans: its only position
// is the aligner's, inside removed material, and the readings' timestamps
// drift by up to 1.5 s. Choosing a place and a carton for it is an edit, and
// stays the caller's.
struct CutHeardWord {
    // The clip whose edge the word sits against.
    Ulid clip_id;
    Ulid source_id;
    std::string text;
    // Where the aligner put it, in the removed material.
    RationalTime source_in;
    RationalTime duration;
    // The clip edge on the timeline: where the montage plays around it.
    RationalTime timeline_in;
    // How each reading of the montage writes it, one entry per reading.
    std::vector<std::string> heard_as;
};

struct TimelineWordCheck {
    // Readings of the montage audio whose cache still matches the current
    // audible arrangement (TimelineTranscriptCachePath). Zero means nothing
    // was checked: no word is dropped on the strength of no evidence.
    int32_t readings = 0;
    std::vector<UnheardWord> unheard;
    std::vector<InflectedWord> inflected;
    std::vector<CutHeardWord> cut_but_heard;
    // Who placed the word boundaries of each source used ("whisper", or the
    // aligner method from WordAlignment.h).
    std::vector<std::pair<Ulid, std::string>> word_timing;
};

// The word check as the agent reads it: readings, unheard words, words kept
// through an inflection and words heard although aligned into a cut.
std::string SerializeTimelineWordCheck(const TimelineWordCheck& check);

// BENCH-2026-09 -- ROADMAP.md. A stretch between two consecutive cartons
// during which the montage speaks. The carton that lost "mélangé" left the
// caption track empty for 0.84 s while it was said, and finding that took a
// word-by-word diff against both readings.
//
// Speech here is word positions, not the signal: an unheard word the check
// left out ("unheard"), or a word heard although aligned into a cut whose
// clip edge falls in the gap ("aligned_in_removed_range"). A word the
// cartons were made from is never in a gap: generate_captions no longer
// moves a carton edge past one (DEC18, docs/roadmap/BENCH-2026-09.md), and
// the reason that named such a word, "carton_edge_on_cut", went with it.
struct CaptionGapWord {
    std::string text;
    RationalTime timeline_in;
    std::string reason;
};

struct CaptionSpeechGap {
    // Index of the carton before the gap; the next one follows it.
    size_t after = 0;
    RationalTime timeline_in;
    RationalTime duration;
    std::vector<CaptionGapWord> words;
};

// `cartons` are (timeline_in, duration) in timeline order; `check` the word
// check of the spans they were made from.
std::vector<CaptionSpeechGap> FindCaptionSpeechGaps(
    const std::vector<std::pair<RationalTime, RationalTime>>& cartons,
    const TimelineWordCheck& check);

std::string SerializeCaptionSpeechGaps(
    const std::vector<CaptionSpeechGap>& gaps);

// Collects the spans of every audible audio track, in timeline order, from
// the transcripts cached under `transcriptDirectory` (the project's
// `.cutmachine/transcripts`), with any matching word alignment applied. When
// `check` is given it receives the unheard words left out and the word
// timing provenance.
bool BuildTimelineTranscriptSpans(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::vector<TimelineTranscriptSpan>& spans, std::string& error,
    TimelineWordCheck* check = nullptr);

// WORD-ALIGNMENT-2026-09 -- same audible-word attribution as the agent view,
// with one span per word so a Resolve review track exposes every boundary.
bool BuildTimelineTranscriptWordSpans(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::vector<TimelineTranscriptSpan>& spans, std::string& error,
    TimelineWordCheck* check = nullptr);

std::string SerializeTimelineTranscriptSpans(
    const Document& document, const std::vector<TimelineTranscriptSpan>& spans,
    const TimelineWordCheck* check = nullptr);

// Reads back what SerializeTimelineTranscriptSpans wrote. Exists so a
// backend that can only hand out the rendered JSON view can still answer a
// structured span lookup without a second cache path; see McpBackend.h's
// default ReadTimelineTranscriptSpans.
bool ParseTimelineTranscriptSpans(const std::string& json,
                                  std::vector<TimelineTranscriptSpan>& spans,
                                  std::string& error);

bool DescribeTimelineTranscriptForAgent(
    const Document& document, const std::filesystem::path& transcriptDirectory,
    std::string& json, std::string& error);

// Resolves an inclusive run of span ids into the one exact source range it
// covers. This is the deterministic computation PHILOSOPHY.md principle 7
// asks for on the editorial path, and the counterpart of what
// ResolveWordRemoval already does on the cleanup path: the caller names
// *which spans* (semantic, agent-sized), this resolves *which frames*
// (exact, code-guaranteed).
//
// Taking a run rather than a single id is what lets a caller express an idea
// that outlives one span. Spans are cut for subtitle legibility -- grouped by
// timing and French syntax around an average 18 CPS target, with sentence
// ends and long pauses as hard boundaries. This remains a display convention,
// not an editorial unit, so a single sentence can span several cues. Merging
// them here, from ids, keeps that regrouping in code instead of asking a model
// to add two timecodes together.
//
// Refuses, rather than repairing, a run whose spans are not all from one
// source or are not contiguous in that source: a gap between two spans is
// silence or another speaker, and quietly swallowing it would produce a cut
// nobody asked for. `endSpanId` may be empty, meaning a run of one.
bool ResolveTranscriptSpanRange(
    const std::vector<TimelineTranscriptSpan>& spans,
    const std::string& startSpanId, const std::string& endSpanId,
    Ulid& sourceId, RationalTime& sourceIn, RationalTime& duration,
    std::string& error);
