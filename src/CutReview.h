#pragma once

// RC1 -- docs/roadmap/RACCORDS-2026-10.md. The engine watches and listens
// to every junction of a montage, for an agent that can do neither.
//
// WHY THIS FILE EXISTS. An agent perceives a montage by stills (read_frame,
// the sheets), by numbers, and by one window at a time (inspect_cut). It
// never sees the film run and never hears it: Claude takes neither sound
// nor video. What a viewer notices at a cut lives in time -- an ambience
// that jumps, a breath or a word cut short, two shots of one framing that
// jump, a movement that stops dead -- and nothing measured it over a whole
// montage. verify_export proves the delivery is the timeline; it says
// nothing of how the timeline is received.
//
// WHAT IS MEASURED, AND ON WHAT. Each junction is rendered by inspect_cut's
// own window render (CutInspectionRender.h: the export's graph, without
// loss), so what is measured is what the film shows and says there, every
// layer, grade, gain and fade included:
//
//   sound    over the mix, on either side of the cut: the ambience (a low
//            percentile of 10 ms levels, the room the viewer hears between
//            words), the voice (a high percentile), and the 30 ms that touch
//            the cut, against that side's ambience.
//   words    the words of the project's transcripts that the cut runs
//            through (TimelineTranscriptSpan::straddles_cut).
//   picture  the last frame before the cut against the first after it (mean
//            absolute difference and luma histogram distance, ShotQuality's
//            metrics), and the motion between the two frames on either side.
//
// WHAT IT IS NOT. Every signal has a declared threshold, and crossing one
// raises an ALERT TO EXAMINE, never a verdict on the montage (PHILOSOPHY.md
// principle 12: the engine judges the correct; whether a jump cut or a hard
// cut in the room tone is right is the author's). The thresholds below are
// first values, set from what the measures mean, and calibrated only on
// synthetic media: RACCORDS-2026-10 says what remains to measure on films.
//
// This header is portable: listing the junctions, planning their windows,
// the measures and the alerts. Rendering goes through a function the caller
// gives (a backend's InspectCut, or RenderCutInspection directly).

#include "CutInspection.h"
#include "CutInspectionRender.h"
#include "Document.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Bounds of a review.
inline constexpr int32_t kCutReviewMaxJunctions = 60;
inline constexpr int32_t kCutReviewMaxImagesPerSide = 4;
inline constexpr int32_t kCutReviewMaxBands = 12;
// Longest side of each picture of a band, and of the planes measured.
inline constexpr int32_t kCutReviewCellDimension = 240;
inline constexpr int32_t kCutReviewAnalysisDimension = 160;
// The sound measures: 10 ms windows, the percentiles of them that read the
// ambience and the voice, the stretch that touches the cut, and the least
// of a side worth a percentile.
inline constexpr int64_t kCutReviewLevelWindowMilliseconds = 10;
inline constexpr int32_t kCutReviewAmbiencePercentile = 10;
inline constexpr int32_t kCutReviewVoicePercentile = 90;
inline constexpr int64_t kCutReviewEdgeMilliseconds = 30;
inline constexpr int64_t kCutReviewMinimumSideMilliseconds = 300;

struct CutReviewThresholds {
    // The ambience of one side differs from the other's by at least this.
    // Two takes of one room usually sit within a few dB of each other; a
    // change of room, of microphone or of noise reduction is heard.
    int64_t ambience_jump_db = 6;
    // The voice of one side differs from the other's by at least this, when
    // both sides hold a voice (voice_over_ambience_db over their ambience).
    int64_t voice_jump_db = 6;
    int64_t voice_over_ambience_db = 10;
    // The 30 ms on one side of the cut stand this far over that side's
    // ambience, and are audible (at least edge_minimum_dbfs): the cut
    // falls inside a sound -- a word, a breath, a note -- not in the room.
    int64_t edge_over_ambience_db = 12;
    int64_t edge_minimum_dbfs = -50;
    // Pictures, in thousandths of full scale. Two frames across the cut
    // closer than these are one framing: a jump cut.
    int64_t jump_cut_difference_per_mille = 80;
    int64_t jump_cut_histogram_per_mille = 150;
    // A side moves when its two frames differ by at least moving; it is
    // still under still. A movement interrupted stops dead at a cut between
    // two frames of one scene (histogram distance under same_scene).
    int64_t moving_per_mille = 30;
    int64_t still_per_mille = 8;
    int64_t same_scene_histogram_per_mille = 250;
};

// One track that cuts at a junction.
struct TimelineJunctionCut {
    Ulid track_id;
    // "video" or "audio".
    std::string track_kind;
    Ulid outgoing_clip_id;
    Ulid incoming_clip_id;
    // Media ids, or composition ids for motion clips.
    Ulid outgoing_source_id;
    Ulid incoming_source_id;
    // The incoming clip reads its source on from the very frame the
    // outgoing one stopped at, both at speed 1: a cut nobody can see or hear
    // (a through edit). Its measures are given and raise no alert.
    bool continuous = false;
};

// A frame of the timeline where at least one visible video track or one
// audible audio track passes from a clip to the clip that touches it.
struct TimelineJunction {
    int64_t frame = 0;
    std::vector<TimelineJunctionCut> cuts;
    bool picture = false;
    bool sound = false;
    // Every cut of that kind here is continuous.
    bool picture_continuous = false;
    bool sound_continuous = false;
};

// The junctions of the document's timeline, ascending. A clip after a gap
// starts no junction (analyze_timeline_audio and check_timeline report
// gaps). A junction off the frame grid of the sequence is left out and
// counted in `offGrid`. Refuses a timeline without a frame rate.
bool ListTimelineJunctions(const Document& document,
                           std::vector<TimelineJunction>& junctions,
                           int64_t& offGrid, std::string& error);

struct CutReviewRequest {
    // The junctions to review, by their frame on the timeline; each must be
    // one. Empty: every junction, the first kCutReviewMaxJunctions of them.
    std::vector<int64_t> frames;
    // Montage rendered on either side of each cut: whole frames, at most
    // kCutInspectionMaxSideSeconds.
    RationalTime side{1, 1};
    // Pictures of each band on either side of the cut, the frame just
    // before and the frame at the cut among them.
    int32_t images_per_side = 3;
    // Bands in the picture: the junctions with alerts first, in timeline
    // order, then the others.
    int32_t max_bands = 6;
    CutReviewThresholds thresholds;
};

// The frames a band shows on either side of a cut, ascending: `perSide`
// frames spread over [first, cut) ending at cut - 1, and over [cut, end)
// starting at cut. Fewer when the window holds fewer.
std::vector<int64_t> CutBandFrames(int64_t first, int64_t cut, int64_t end,
                                   int32_t perSide);

// The window of one junction: PlanCutInspection's, with `shown` the band's
// frames and the four the picture measures read (cut - 2 to cut + 1).
bool PlanJunctionReview(const Document& document,
                        const TimelineJunction& junction,
                        const CutReviewRequest& request,
                        CutInspectionPlan& plan,
                        std::vector<int64_t>& bandFrames, std::string& error);

struct JunctionSoundMeasures {
    bool measured = false;
    std::optional<int64_t> ambience_before_dbfs;
    std::optional<int64_t> ambience_after_dbfs;
    std::optional<int64_t> voice_before_dbfs;
    std::optional<int64_t> voice_after_dbfs;
    std::optional<int64_t> edge_before_dbfs;
    std::optional<int64_t> edge_after_dbfs;
};

struct JunctionPictureMeasures {
    bool measured = false;
    // Frame cut - 1 against frame cut.
    std::optional<int64_t> difference_per_mille;
    std::optional<int64_t> histogram_per_mille;
    // Frame cut - 2 against cut - 1, and cut against cut + 1.
    std::optional<int64_t> motion_before_per_mille;
    std::optional<int64_t> motion_after_per_mille;
};

// `samples` is the window's mix, interleaved stereo float from the window's
// first frame (CutInspectionResult::samples). A side shorter than
// kCutReviewMinimumSideMilliseconds has no ambience or voice.
bool MeasureJunctionSound(const std::vector<float>& samples, int32_t sampleRate,
                          const CutInspectionPlan& plan,
                          JunctionSoundMeasures& measures, std::string& error);

// `planes` are grey planes of every frame of `plan.shown`, in its order.
bool MeasureJunctionPicture(const std::vector<std::vector<uint8_t>>& planes,
                            int32_t width, int32_t height,
                            const CutInspectionPlan& plan,
                            JunctionPictureMeasures& measures,
                            std::string& error);

struct CutReviewAlert {
    // ambience_jump, voice_jump, sound_cut_live, word_crosses_cut,
    // jump_cut, motion_interrupted.
    std::string kind;
    // What was measured, in one sentence.
    std::string finding;
};

struct JunctionReview {
    TimelineJunction junction;
    CutInspectionPlan plan;
    JunctionSoundMeasures sound;
    JunctionPictureMeasures picture;
    // The words the cut runs through, from the project's transcripts.
    std::vector<CutInspectionWord> words_across;
    // Why no word could be read, when the project has no transcript.
    std::string words_unavailable;
    std::vector<CutReviewAlert> alerts;
    // Its band's place in the picture, from 0; absent when it is not there.
    std::optional<int32_t> band;
};

// The alerts the measures raise against the thresholds. Sound alerts only
// for a junction where sound cuts, picture alerts only where picture cuts,
// neither for a continuous cut; a word the cut runs through always.
void JudgeJunction(const CutReviewThresholds& thresholds,
                   JunctionReview& review);

struct CutReviewReport {
    Ulid timeline_id;
    MediaRate rate{25, 1};
    CutReviewThresholds thresholds;
    int64_t junctions_total = 0;
    int64_t off_grid = 0;
    std::vector<JunctionReview> reviews;
    // Junctions left out by the kCutReviewMaxJunctions bound.
    std::vector<int64_t> not_reviewed;
    // Frames of the junctions whose bands the picture holds, top to bottom.
    std::vector<int64_t> bands;
};

using CutInspectionRenderer = std::function<bool(
    const CutInspectionPlan&, const CutInspectionRenderSettings&,
    CutInspectionResult&, std::string&)>;

// Renders, measures and judges each junction asked for, and keeps the band
// pictures the report names in `bands`, in that order (one JPEG each, all
// of one width, for StackInspectionPictures).
bool ReviewCuts(const Document& document, const CutReviewRequest& request,
                const CutInspectionRenderer& render, CutReviewReport& report,
                std::vector<std::string>& bandJpegs, std::string& error);

// The report as the agent reads it.
std::string SerializeCutReview(const CutReviewReport& report);
