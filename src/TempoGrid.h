#pragma once

#include "BeatDetection.h"
#include "Document.h"
#include "RationalTime.h"

#include <cstdint>
#include <string>
#include <vector>

// R7 -- docs/roadmap/REEL-2026-09.md. An editorial beat grid, derived from
// the onsets DetectAudioBeats already cached.
//
// Why this is not BeatDetection.h's tempo. That estimate autocorrelates the
// novelty function over the whole source and reports the strongest lag: on
// the first film's title it answered 60.8 BPM for a track at 121.0 -- the
// half-time reading, and 0.5 % off on top, which is about eight frames of
// drift over a minute. Its onsets were right (median -8 ms against the
// audible attacks); only the periodicity was wrong. So this module never
// re-detects anything and never decodes audio: it takes the cached onsets
// and fits a period and a phase to them.
//
// Method, and why it is this one. Candidate periods come from clustering
// inter-onset intervals -- Dixon, "Automatic Extraction of Tempo and Beat
// From Expressive Performances", Journal of New Music Research 30(1), 2001.
// Each candidate is then refined by alternating a circular-mean phase
// estimate (the resultant length of circular statistics, which is what
// `coherence` below reports) with a weighted least-squares fit of the
// onsets against their integer beat indices -- the standard refinement of
// an induced tempo, as in Ellis, "Beat Tracking by Dynamic Programming",
// Journal of New Music Research 36(1), 2007. The fit runs on the strong
// onsets of the most stable stretch of the analysis window, which is what
// worked by hand on 19 September 2026 (coherence 0.81, median residual
// 5 ms) and what an average over a whole track does not do.
//
// What does NOT decide the metrical level: low-frequency energy. Measured
// on that same title, the steepest slope of the bass energy arrives 88 ms
// after the audible attack -- two frames late at 25 i/s, on every cut that
// trusts it. A third-party grid fitted that way on another job put the
// phase 0.2 s out: an off-beat bass note read as the kick, and the track
// read in half-time. The onsets this module consumes are broadband spectral
// flux, and `strength` is the broadband novelty peak; the phase is settled
// on those, never on a band-limited envelope.
//
// Exactness. A period and a phase are positions, so they are integer counts
// of samples at the onsets' own decode rate -- the same exact domain
// AudioOnset::time already lives in (PHILOSOPHY.md principle 4). Residuals
// are published in samples too; milliseconds appear only where something is
// displayed. Doubles exist here for scores -- coherence, BPM, support
// ratios -- and never become a position.
//
// No cache of its own, deliberately. The expensive half of this work is the
// onset detection, which BeatDetection.cc already caches; fitting a grid on
// top of a few hundred cached onsets costs microseconds. A second cache
// file would be one more thing to be stale, for no measurable gain.
//
// R7 adds no operation. The author places the cuts; the grid says where the
// beats are. Nothing in this file writes to a Document.

// Thresholds and search bounds. Tolerances are fractions of the fitted
// period rather than durations, so they need no sample rate and stay exact.
struct TempoGridSettings {
    // Metrical search range. The half-time reading of a 121 BPM track
    // (60.5) is deliberately inside it: it has to be rejected by the
    // arbitration below, on evidence, not hidden by a bound.
    double minimum_bpm = 60.0;
    double maximum_bpm = 200.0;
    // Tempo preference among the metrical levels of one candidate. A
    // log-normal resonance curve, as used for tempo induction since
    // Parncutt, "A Perceptual Model of Pulse Salience", Music Perception
    // 11(4), 1994: the level closest to `preferred_bpm` wins a tie, the
    // arbitration below breaks the ones that matter.
    double preferred_bpm = 120.0;
    double preference_octaves = 0.8;
    // Below this many onsets in the window there is nothing to fit.
    int32_t minimum_onsets = 16;
    // An onset plays a beat when it sits within this fraction of the period
    // of it. An eighth of a beat is 62 ms at 121 BPM -- wide enough for the
    // 11.6 ms analysis hop and for a human drummer, narrow enough that an
    // off-beat note never counts as the beat.
    int32_t match_tolerance_num = 1;
    int32_t match_tolerance_den = 8;
    // Refusal: the median absolute residual may not exceed this fraction of
    // the period. A 24th of a beat is 20 ms at 121 BPM, twice the 10 ms the
    // ticket asks a grid to hold, so a grid that passes has room to spare.
    int32_t maximum_residual_num = 1;
    int32_t maximum_residual_den = 24;
    // Refusal: circular coherence of the onsets around the grid. Onsets
    // spread uniformly over a quarter of a beat still score 0.64; spread
    // over half a beat they score 0.00. 0.60 is therefore the line between
    // "played to a grid" and "not on a grid at all", and a free-tempo take
    // falls far below it.
    double minimum_coherence = 0.60;
    // Refusal: share of the window's beats that must carry an onset.
    int32_t minimum_beat_support_percent = 55;
    // Arbitration. A level is read in half-time when this share of its
    // midpoints is played...
    int32_t subdivision_support_percent = 55;
    // ...and those midpoints are not systematically weaker than its beats.
    // An off-beat bass sits well under 70 % of the kick's novelty; the
    // other half of a half-time reading sits at 100 % of it. The same ratio
    // reads the other way for double-time: when one beat out of two is
    // systematically weaker, the level is a subdivision.
    int32_t weak_level_strength_percent = 70;
    // How many unplayed beats in a row end the stable stretch the fit runs
    // on. Two lets a fill or a held note through and stops at a break.
    int32_t stable_section_gap = 2;
};

// Garde-fou 4 of the lot: the author frames, the engine measures. The
// engine never guesses which beat opens a bar -- it numbers bars only once
// the author has said where one starts and how many beats it holds.
struct TempoBarSettings {
    int32_t beats_per_bar = 0;  // 0: undeclared, no bar numbering.
    bool has_origin = false;
    RationalTime origin{0, 1};  // The declared downbeat, in source time.
};

enum class TempoGridStatus {
    Fitted,
    TooFewOnsets,
    NoPeriodFound,
    FreeTempo,  // Fitted, then refused: see TempoGridResiduals.
};

const char* TempoGridStatusName(TempoGridStatus status);

struct TempoGridBeat {
    // Beat index counted from sample 0 of the source, so two windows of the
    // same track number the same beat the same way.
    int64_t index = 0;
    int64_t sample = 0;  // Exact position, at TempoGrid::sample_rate.
    bool matched = false;
    // Signed onset minus beat, in samples. Zero when unmatched.
    int64_t residual_samples = 0;
    // 1-based, and zero when the author declared no origin.
    int32_t bar = 0;
    int32_t beat_in_bar = 0;
};

struct TempoGridResiduals {
    int64_t total_beats = 0;
    int64_t matched_beats = 0;
    int64_t median_absolute_samples = 0;
    int64_t percentile90_absolute_samples = 0;
    int64_t maximum_absolute_samples = 0;
};

struct TempoGrid {
    TempoGridStatus status = TempoGridStatus::TooFewOnsets;
    // Why a refused grid still carries numbers: a free-tempo track is
    // refused *with* its residuals, so the author sees what was measured
    // instead of a grid forced onto it.
    std::string refusal;

    int32_t sample_rate = 0;
    int64_t period_samples = 0;
    int64_t phase_samples = 0;  // Sample of beat index 0; 0 <= it < period.
    int64_t window_first_sample = 0;
    int64_t window_last_sample = 0;
    // Beat index range covered by the window, and by the stable stretch the
    // period and phase were fitted on.
    int64_t first_index = 0;
    int64_t last_index = 0;
    int64_t stable_first_index = 0;
    int64_t stable_last_index = 0;

    // Circular coherence at the subdivision the music is actually played
    // in, and that subdivision (1: on the beat, 2: eighths, 4: sixteenths).
    // Measuring at the beat alone would punish a track whose off-beats are
    // played -- which is most of them.
    double coherence = 0.0;
    int32_t subdivision = 1;
    int32_t beat_support_percent = 0;
    int32_t subdivision_support_percent = 0;

    // Arbitration trace: the clustered candidate before the metrical
    // arbitration, and how many times it was doubled or halved.
    int64_t candidate_period_samples = 0;
    int32_t doublings = 0;
    int32_t halvings = 0;

    int32_t beats_per_bar = 0;
    bool has_origin = false;
    int64_t origin_index = 0;
    // How far the declared origin sat from the beat it was snapped to.
    int64_t origin_snap_samples = 0;

    TempoGridResiduals residuals;
    std::vector<TempoGridBeat> beats;  // Empty unless status is Fitted.

    bool accepted() const { return status == TempoGridStatus::Fitted; }
    // Display only: never use this to compute a position.
    double bpm() const;
};

// Fits a grid to the cached onsets inside [windowIn, windowOut). The window
// may be the whole source; a passage is what an author uses when a track
// changes tempo between sections.
TempoGrid FitTempoGrid(const std::vector<AudioOnset>& onsets,
                       const RationalTime& windowIn,
                       const RationalTime& windowOut,
                       const TempoGridSettings& settings,
                       const TempoBarSettings& bars);

// One beat of the grid, placed in the sequence.
struct TempoGridSequenceBeat {
    int64_t index = 0;
    int32_t bar = 0;
    int32_t beat_in_bar = 0;
    RationalTime source_time{0, 1};
    RationalTime timeline_time{0, 1};
    int64_t frame = 0;
    // Exact distance from the start of `frame` to the beat. This is the
    // error the author takes by cutting on that frame.
    RationalTime frame_offset{0, 1};
    // The same distance in samples at the grid's rate, floored. `offset`
    // above is the exact value; this one is for reading.
    int64_t frame_offset_samples = 0;
};

struct TempoGridClipBeats {
    Ulid clip_id;
    Ulid source_id;
    RationalTime source_in{0, 1};
    RationalTime timeline_in{0, 1};
    RationalTime duration{0, 1};
    std::vector<TempoGridSequenceBeat> beats;
};

// Criterion 4 of the ticket: two clips of one track cut together keep one
// continuous grid, or the discontinuity is named in samples.
struct TempoGridJunction {
    Ulid before_clip_id;
    Ulid after_clip_id;
    bool continuous = false;
    // Signed phase jump of the grid across the cut, in samples, reduced to
    // (-period/2, period/2]. Zero means the beats run straight through.
    int64_t sample_offset = 0;
    // Whole beats of the track skipped (or repeated, when negative) at the
    // cut. 96 beats between two identical positions of a 4-beat bar is a
    // continuous phrase; 97 is not.
    int64_t beat_offset = 0;
    // beat_offset modulo beats_per_bar; 0 when no bar was declared, and 0
    // when the cut lands on the same beat of the bar.
    int32_t bar_phase_offset = 0;
};

// The one place a beat becomes a frame number. Everything else in R7 stays
// in exact time and calls this. Returns false when `rate` is unusable.
bool TempoBeatToSequenceFrame(const RationalTime& timelineTime,
                              const MediaRate& rate, int64_t& frame,
                              RationalTime& offset, std::string& error);

// Maps a source position of the grid's media to its position on the
// timeline, through the clip that carries it.
//
// R3 (vitesse de plan) enters HERE and nowhere else: once DocumentClip
// carries an exact speed fraction, the elapsed source time below is divided
// by it before it is added to timeline_in, and ProjectTempoGridOntoClip's
// range test follows. R3 does not exist yet, so nothing scales today --
// this comment marks the single line that changes when it does.
RationalTime TempoSourceToTimeline(const DocumentClip& clip,
                                   const RationalTime& sourceTime);

// Places every beat of `grid` that the clip shows. Returns false, with a
// reason, when the grid was refused or the clip does not read the media.
bool ProjectTempoGridOntoClip(const TempoGrid& grid, const DocumentClip& clip,
                              const MediaRate& sequenceRate,
                              TempoGridClipBeats& out, std::string& error);

// Where beat `beatIndex` of `mediaId` is heard on `document`'s timeline: the
// sequence frame -- the one that contains the beat, as
// ProjectTempoGridOntoClip names it -- of every clip of that media on a track
// that is not muted, ascending and once each (a picture and its linked
// sound hear a beat on the same frame). Empty when no clip plays it. Fails
// on a refused grid, or a beat index the grid does not hold.
//
// What lets a title land on a beat without the agent turning a beat into a
// time: it names the beat list_tempo_grid gave it, the engine answers the
// frame (PHILOSOPHY.md principle 7).
bool TempoBeatFramesOnTimeline(const Document& document, const TempoGrid& grid,
                               const Ulid& mediaId, int64_t beatIndex,
                               std::vector<int64_t>& frames,
                               std::string& error);

// Junctions between consecutive clips of `clips`, which the caller passes
// in timeline order. Only pairs that share a cut -- the second starts where
// the first ends -- produce a junction.
std::vector<TempoGridJunction> TempoGridJunctions(
    const TempoGrid& grid, const std::vector<DocumentClip>& clips);
