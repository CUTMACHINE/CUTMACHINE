#pragma once

#include "Document.h"
#include "Operations.h"
#include "RationalTime.h"
#include "SpeechOnset.h"
#include "Transcription.h"

#include <cstdint>
#include <string>
#include <vector>

// QC-2026-09 A2 -- closes the silences inside a clip without reading a word.
//
// The measured case: two equivalent gestures removed 6.6 s of internal
// silence from a single montage, and neither needed the transcript. That
// matters beyond the time saved, because the transcript is the expensive
// artifact and the unreliable one -- an interview cut that only has to be
// tightened should not have to wait for Whisper, nor inherit its word
// boundaries. The speech envelope (SpeechOnset.h) already says where the
// voice is; a hollow in it is a pause whatever was said around it.
//
// Same division of labour as Transcription.h's ResolveWordRemoval, for the
// same reason (PHILOSOPHY.md principle 7): the caller states an intent --
// "no pause longer than half a second, leave six frames of it" -- and this
// resolves the exact source ranges. It produces a RemoveWordsOperation
// rather than an operation of its own, because that operation already does
// precisely what is needed and does it reversibly: cut exact spans out of a
// clip's source range, ripple-close each cut, carry the A/V pair through
// Q4b's linked_clip_ids, and shift sync-locked tracks by the same delta.
// Adding a second operation that ripple-closes source spans would be a
// second implementation of one thing.
//
// IA8 -- ROADMAP.md. Reading no word had a price. At 320 ms and 4 frames,
// on an interview whose words a forced aligner had placed, five whole words
// went: an « à », two « Et », two « et ». Each lasted one frame and sat
// against a pause; in two cases the silences either side were each under the
// threshold, and the envelope read silence, word and silence as one hollow.
// A short, quiet word falls under the speech line (SpeechOnset.h) however the
// line is drawn, so this is not a threshold to tune. When aligned words are
// cached, a hollow that holds one is split around it and each remaining part
// is judged as a hollow of its own -- closed if it is still a pause, left
// otherwise. Whisper's boundaries are not trusted for this: they drift by up
// to 1.5 s and land on silence, so they would keep real pauses open while
// still missing the one-frame words that were lost. Without an alignment the
// tool closes pauses exactly as before and its report says the words went
// unchecked.
//
// Deliberately internal pauses only. A hollow that touches the clip's head
// or its tail is not a pause between two phrases; it is lead-in or run-out,
// which SpeechOnset.h measures and a head ripple trim removes. Cutting it
// here would silently do a different edit from the one asked for, and would
// fight the tool that owns that question. Head and tail air are reported, so
// a caller sees what was left rather than assuming there was none.

// The defaults are 320 ms and 4 frames since 4 November 2026 (decided by
// the author): the setting the interview procedure used to pass explicitly on
// every call. 400 ms and 6 frames, the former defaults, left the film
// slack: four hollows closed per shot, 36 to 44 left as "too short". 250 ms
// and 3 frames cut a hollow inside a word ("notre"), whose stop consonant
// leaves a real gap in the envelope; 300 to 320 ms and 4 frames kept the
// words. Since IA8 an aligned word is protected as well.
struct PauseTighteningSettings {
    // Below this, a hollow is a breath or the space between two words in one
    // phrase. Removing those is what makes an interview sound clipped rather
    // than tight, so the default sits above ordinary sentence-internal
    // pauses and below the hesitation an editor would cut by hand.
    int64_t minimum_gap_milliseconds = 320;
    // What is left of each pause, in whole frames of the clip's own source
    // rate. Frames rather than milliseconds because this is the unit the
    // decision is actually made in, and because the cut has to land on a
    // frame either way. Zero butts the two phrases together, which almost
    // always reads as a mistake.
    int64_t keep_frames = 4;
    // Speech/silence line, as a percentage of the clip's own 90th
    // percentile. Same measurement and same reasoning as
    // SpeechOnsetThresholds::speech_ratio_percent, and deliberately the same
    // number: a hollow this calls silence must be a hollow that one calls
    // silence too, or two tools would disagree about the same audio.
    int64_t speech_ratio_percent = 25;
    // AGENT-2026-09 A17 -- ROADMAP.md. An aligned word whose every window
    // sits at the source's noise floor (within 3 dB of it) was written by
    // the transcriber and placed by the aligner, but not spoken: on a real
    // montage, unspoken "Et"s held nine pauses open and the agent closed
    // them by hand. IA8's guard stays -- a quiet one-frame "à" is well above
    // the floor -- but such a word is reported as unheard, and with this
    // flag the hollow is closed over it, the word going to
    // `closed_over_words`.
    bool close_unheard = false;
    // How far above the source's noise floor a word's loudest window may
    // sit and still count as unheard, in dB. 3 dB is room tone; on a real
    // rush the "Et"s nobody said peaked 5 to 8 dB above the floor, 6 to
    // 8 dB under the speech line, so a caller who has read the levels may
    // widen it. Every kept word carries its peak for that.
    int64_t unheard_margin_db = 3;
};

// One pause that was closed, in the clip's own source time domain. Published
// so a caller can see what the cut actually did without diffing the
// document -- the operation itself carries only the ranges to remove.
struct TightenedPause {
    RationalTime gap_start;
    RationalTime gap_end;
    RationalTime removed;
};

// IA8 -- an aligned word found inside a hollow, and therefore kept.
struct PauseKeptWord {
    std::string text;
    RationalTime start;
    RationalTime end;
    // AGENT-2026-09 A17 -- false when every window of the word sits within
    // `unheard_margin_db` of the source's noise floor: the aligner placed a
    // word nobody said. `peak_dbfs` is the word's loudest window, so a
    // caller can judge the margin on a number.
    bool heard = true;
    int64_t peak_dbfs = -120;
    // AGENT-2026-09 A33 -- the part of the word the hollow left alone: its
    // aligned span trimmed to the windows above the floor line. An aligner's
    // end runs into the silence it precedes (measured: "Rivage," aligned
    // 4 frames past the drop), and protecting the whole aligned span kept
    // that silence -- 320/4 closed 5 hollows on 23 clips of a real montage.
    RationalTime protected_start{0, 1};
    RationalTime protected_end{0, 1};
};

struct PauseTighteningReport {
    std::vector<TightenedPause> pauses;
    RationalTime removed_total{0, 1};
    // Hollows found and left alone, by reason. Separated because they say
    // different things: `skipped_short` is the healthy majority, while a
    // large `skipped_already_tight` means the clip has been through this
    // once already and a caller lowering `keep_frames` would only churn it.
    int32_t skipped_short = 0;
    int32_t skipped_already_tight = 0;
    // Air at the clip's own edges, measured and not touched. See the header
    // comment: this is SpeechOnset.h's question, and answering it here would
    // do a different edit from the one asked for.
    RationalTime head_air{0, 1};
    RationalTime tail_air{0, 1};
    // IA8 -- whether aligned words guarded the cut, which aligner placed
    // them, and why not otherwise. A report that says nothing about words
    // would read as "no word was touched" when none was looked at.
    bool words_checked = false;
    std::string word_timing;
    std::string words_unchecked_reason;
    // Hollows that held at least one aligned word, and those words.
    int32_t hollows_holding_words = 0;
    std::vector<PauseKeptWord> kept_words;
    // AGENT-2026-09 A17 -- with close_unheard, the unheard words a hollow
    // was closed over; and the source's noise floor the margin counts from.
    std::vector<PauseKeptWord> closed_over_words;
    int64_t floor_dbfs = -120;
};

// ---------------------------------------------------------------------
// Pure core. No FFmpeg, no filesystem -- what tests exercise directly.
// ---------------------------------------------------------------------

// Resolves the pauses inside `clip`'s own source range against `envelope`
// into the exact RemoveWordsOperation ApplyOperation can execute.
// `sourceRate` is the frame grid every produced boundary lands on, and the
// grid `keep_frames` is counted in; it is the source's own MediaRate.
//
// Rounding is one-directional on purpose: a gap's start rounds forward and
// its end rounds back, so a boundary the envelope places between two frames
// always resolves *inside* the silence. The alternative -- rounding to the
// nearest -- would occasionally take a frame of the consonant that opens the
// next word, which is the one error this must not make.
//
// `words` is the source's transcript as LoadSourceWords reads it, or null
// when none is cached. Only a forced alignment (`word_timing` other than
// "whisper") protects words; see the IA8 note above.
//
// Fails, rather than inventing a cut, when the envelope or the words belong
// to another source, the envelope has no analysis grid, or when the settings
// are not usable. A clip
// with no pause worth closing is a success with an empty `operation.ranges`
// -- a caller must check that before applying, because ApplyOperation
// rejects an empty range list.
bool ResolvePauseTightening(const DocumentClip& clip,
                            const SpeechOnsetReport& envelope,
                            const Transcript* words,
                            const PauseTighteningSettings& settings,
                            const MediaRate& sourceRate,
                            const std::vector<Ulid>& syncTrackIds,
                            RemoveWordsOperation& operation,
                            PauseTighteningReport& report, std::string& error);

// The report as the agent- and CLI-facing JSON both surfaces return.
std::string SerializePauseTighteningReport(const PauseTighteningReport& report);
