#pragma once

// PA7 -- docs/roadmap/PARITE-2026-11.md. A handheld shot steadied, written
// as keys on the clip's framing.
//
// The engine already measured how the camera moves between two pictures
// (EstimateGlobalMotion, ShotQuality.h: phase correlation on a grid of
// blocks, then the translation and zoom that explain them), but a few times
// a second, to grade a rush. Steadying a shot asks for it between every
// frame and the next. From there the rule is short:
//
//   * the camera's PATH is the sum of those movements: where the picture
//     has drifted to at each frame, as a fraction of the source;
//   * the path a steady operator would have made is that path SMOOTHED, a
//     Gaussian average over `smoothing` seconds on each side -- a pan and a
//     walk stay, the tremble around them goes;
//   * the CORRECTION is the difference: at each frame the picture is moved
//     by what separates the path it took from the path it should have
//     taken.
//
// And it is written as CA1 wrote a followed framing (SubjectTracking.h):
// the clip in `fill`, and keys on the framing's position. Nothing
// downstream learns anything new -- the monitor, the export, the sheets and
// verify_export already sample those keys -- and a document replays without
// decoding a frame. The analysis is on the authoring path only.
//
// THE ZOOM IT COSTS. A picture moved off centre uncovers an edge of the
// frame unless it is larger than the frame. The scale is chosen as the
// smallest that holds every correction, up to `max_scale`; past it the
// corrections are held at the edge of the room and counted
// (`frames_at_edge`): a shot that shakes more than the zoom allows is
// steadied less, never bordered in black. A `scale` given is taken as is.
//
// WHAT IT DOES NOT DO, and says:
//
//   * only the translation is corrected. The zoom between frames is
//     measured and not written (a clip that animates its scale has no fixed
//     room to move in); the rotation is not measured at all, nor the
//     rolling shutter's wobble. A shot that rolls keeps its roll.
//   * the measure is the picture's, not the camera's. It is nine blocks,
//     and the ones that move otherwise than most are left out
//     (GlobalMotionFit::Consensus, ShotQuality.h): a subject crossing part
//     of the frame is not read as the camera, and the frames where blocks
//     were left out are counted (`frames_blocks_set_aside`). A subject that
//     fills most of the frame and moves is still read as the camera.
//   * a frame the estimator could not measure (a flat picture, a movement
//     past half a block) counts as no movement, and is counted
//     (`frames_unmeasured`).
//
// WHERE EXACT BECOMES FLOATING POINT (principle 4). The path, its average
// and the correction are doubles; what they produce is quantised once onto
// kAnimationSampleDenominator inside the room, and reduced to keys that are
// exact fractions in the document. A replay never runs this arithmetic.

#include "Document.h"
#include "Operations.h"
#include "SubjectTracking.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// How the picture moved from the previous frame of the clip to this one, as
// fractions of the displayed source: positive x to the right, positive y
// downwards. The first frame of a clip has no previous one and is not read.
struct StabilizationSample {
    bool measured = false;
    double translation_x = 0.0;
    double translation_y = 0.0;
    // Measured, reported, not corrected: the change of scale.
    double zoom = 0.0;
    // Blocks of the measure left out for moving otherwise than the rest.
    int32_t blocks_set_aside = 0;
};

struct StabilizationSettings {
    // The standard deviation of the average the path is smoothed with, in
    // seconds: the longer, the steadier, and the more zoom it costs.
    RationalTime smoothing{1, 2};
    // The most the picture may be enlarged to make room for the
    // corrections, over the scale that fills the frame.
    EffectParamValue max_scale{5, 4};
    // How far the keys may stray from the computed path, as a fraction of
    // the frame: 1/5000 is a fifth of a pixel on a 1080-wide frame, two
    // steps of the grid the keys are written on. A correction is a movement
    // of a few pixels; the 1/1000 a followed framing allows would put a
    // pixel of the tremble back. The price is a key on nearly every frame.
    EffectParamValue tolerance{1, 5000};
};

bool StabilizationSettingsAreValid(const StabilizationSettings& settings,
                                   std::string& error);

struct StabilizationInput {
    // The clip as a moved framing reads it (PrepareFollowedFraming): its
    // `samples` are not read.
    SubjectTrackingInput clip;
    // One per frame of the clip, in order; the first is not read.
    std::vector<StabilizationSample> motion;
    // True when the caller gave the scale: it is taken as is.
    bool scale_given = false;
};

struct StabilizationResult {
    SubjectTrackingAxis x;
    SubjectTrackingAxis y;
    int64_t frames = 0;
    int64_t frames_unmeasured = 0;
    // Measured frames where part of the picture moved otherwise than the
    // rest and was left out of the measure: a subject crossing the frame.
    int64_t frames_blocks_set_aside = 0;
    // The scale the framing is given, and whether this computation chose
    // it; `scale_needed` is the one that would have held every correction
    // (above `scale` when max_scale stopped it).
    EffectParamValue scale{1, 1};
    bool scale_chosen = false;
    double scale_needed = 1.0;
    // In sequence pixels. How far the picture strayed from its smoothed
    // path before (root mean square and at most), and how far it still
    // does once the keys are laid -- what the edge of the room and the
    // keys' tolerance left.
    double shake_before_rms = 0.0;
    double shake_before_max = 0.0;
    double shake_after_rms = 0.0;
    double shake_after_max = 0.0;
    // The largest change of scale between two frames, which is not
    // corrected.
    double zoom_max = 0.0;
};

// THE RULE, as a pure function: no document, no FFmpeg. Fails with a named
// reason when the motion does not hold one sample per frame, when the clip
// has fewer than three frames, or when the framing is not in `fill`.
bool ComputeStabilization(StabilizationInput input,
                          const StabilizationSettings& settings,
                          StabilizationResult& result, std::string& error);

// The computation laid on a clip of `document` as the two operations a
// followed framing is made of (LayFollowedFraming): reversible, journaled,
// replayed without an analysis. The clip's own position stays the centre
// the corrections move around; its position keys are replaced.
bool BuildStabilizationOperations(
    const Document& document, const Ulid& clipId,
    const std::vector<StabilizationSample>& motion,
    const StabilizationSettings& settings,
    const std::optional<EffectParamValue>& scale,
    const std::function<Ulid()>& newId, std::vector<Operation>& operations,
    StabilizationResult& result, bool& modeChanged, std::string& error);

std::string SerializeStabilizationResult(const StabilizationResult& result);

// The analysis picture a clip's frames are reduced to before two of them
// are compared: 768 on the long edge of the displayed source, 432 on the
// other, so that a pixel of it is about two and a half of an HD rush and
// the estimator's sub-pixel reading lands under one.
void StabilizationAnalysisSize(int64_t displayedWidth, int64_t displayedHeight,
                               int32_t& width, int32_t& height);

// Reads the movement between every frame of `clipId` and the next: one
// FFmpeg run over the clip's source range, each frame reduced to the
// analysis picture in grey, each pair handed to EstimateGlobalMotion.
// Refuses, by name: a motion clip, a clip with a speed, and a clip whose
// source is not at the sequence's cadence -- frames repeated or dropped to
// fit another cadence read as a stop and a jump, which a correction would
// turn into a tremble.
bool SampleClipMotion(const Document& document,
                      const std::string& projectDirectory, const Ulid& clipId,
                      const std::string& ffmpegPath,
                      std::vector<StabilizationSample>& motion,
                      std::string& error);
