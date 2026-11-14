#pragma once

// CA1 -- docs/roadmap/CADRAGE-2026-10.md. A framing that follows its
// subject, computed the way a camera operator would move, from the places a
// face was found in the clip.
//
// R1 made a clip's framing constant over the clip, and R2 let its position
// carry keys. Turning a 16:9 rush into a 9:16 reel -- the daily work of the
// reel -- then left one question to the author: where the face is at each
// frame. A person who walks across a fixed 9:16 window leaves it. This file
// answers that question, and answers it as keys on the framing's position
// (R2's own parameters), so nothing downstream learns anything new: the
// monitor, the export, the sheets and verify_export already sample those
// keys (ClipAnimation.h), and a document replays without the face finder
// that produced them.
//
// WHY A DEAD ZONE, AND WHY IT IS THE DEFAULT. A reframe that keeps the face
// exactly centred follows every nod and every jitter of the detector: the
// picture swims, and it reads as amateur at once. An operator holds the
// frame while the subject stays inside a safe zone around where it was
// composed, and only moves when the subject leaves it -- then makes one
// deliberate move, accelerating and braking, that brings the subject back
// to its place, and stops. That is the controller below, per axis:
//
//   * at rest, the frame does not move while the subject's distance to its
//     target place stays within `dead_zone` (a fraction of the frame);
//   * leaving it starts a MOVE whose goal is to put the subject back on its
//     target, not merely back at the zone's edge: an operator who only nudged
//     to the edge would creep after a walking subject forever;
//   * a move is speed- and acceleration-limited (`max_speed`,
//     `max_acceleration`, fractions of the frame per second and per second
//     squared) and brakes on the curve that stops it on the goal
//     (v = sqrt(2 a d)), so it does not overshoot and come back -- the
//     oscillation that reads as a shake;
//   * a move ends when the subject is within `settle` of its target, or
//     when the frame has gone past it -- a subject that stops dead while
//     the frame follows it at speed cannot be braked for in time -- and the
//     frame has then slowed to a stop at the acceleration bound. A move
//     never reverses: an overshoot the dead zone still holds is left where
//     it is, and only a subject outside the zone starts a new move. That is
//     what keeps a detector's jitter from turning into a tremble.
//
// The frame is first composed on the subject (frame 0 starts where the
// subject is), and is never asked to uncover the frame: the position stays
// inside the room the fill leaves on each side, and a subject the room
// cannot reach is held at the edge and counted (`frames_at_edge`) rather
// than reached by a black bar.
//
// SEVERAL FACES, AND GAPS. The choice is explicit, and it is one subject:
//
//   * the subject is chosen on the first sample where a face is found: the
//     face nearest `hint` when one is given (the caller read the frame and
//     named who to follow), the largest otherwise -- the person nearest the
//     camera, which is who a reel is about;
//   * afterwards it is FOLLOWED, not re-chosen: at each sample, the face
//     nearest the subject's last place, if it lies within `gate` face widths
//     of it. A second person walking past does not steal the frame because
//     it is larger, and a face that jumps further than a person can move
//     between two samples is a false detection, ignored;
//   * a sample with no face accepted is a GAP. A gap no longer than
//     `bridge_gap` is bridged: the subject's place is interpolated between
//     the detections on either side (in post-production one may anticipate;
//     a hole of a few samples is the detector blinking, not the subject
//     moving). A longer gap holds the frame where it was -- a camera that
//     loses its subject does not drift back to the centre -- and the subject
//     is chosen again, by the first-sample rule, when a face reappears;
//   * one isolated outlier is removed by a median of three over the
//     subject's consecutive detections, before anything else reads them.
//
// Two people the frame cannot hold together are not "followed" by framing
// their middle, which would show neither face: following the group is a
// cut or a wider format, not this tool's call.
//
// WHERE EXACT BECOMES FLOATING POINT (principle 4, guard rail 2 of REEL).
// Times stay RationalTime and are compared exactly; the fraction of the way
// a frame lies between two detections is the one dimensionless ratio
// Keyframes.h already allows to be a double. The controller runs in double,
// and what it produces is quantised once onto kAnimationSampleDenominator
// -- the denominator every sampled value of R2 lands on -- inside the room
// computed exactly. The keys are exact fractions in the document, so a
// replay never runs this arithmetic again: the controller is not on the
// rendering path, only on the authoring one.
//
// THE KEYS. The quantised path is reduced to the fewest linear keys that
// stay within `tolerance` of it (Ramer-Douglas-Peucker on whole frames): a
// still frame is two keys, a move a handful. An axis whose path never moves
// carries no key at all and gets its constant position instead.

#include "ClipAnimation.h"
#include "ColorMeasure.h"
#include "Document.h"
#include "Operations.h"
#include "RationalTime.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// One sampled instant of a clip: its time local to the clip (zero is the
// clip's first frame, as R2's keys count it) and every face found there, as
// regions of the displayed source picture -- top-left origin, fractions of
// its width and height, rotation applied -- which is what FaceFinder.h
// returns. An empty list is a sample where nothing was found.
struct SubjectSample {
    RationalTime time;
    std::vector<ColorMeasureRegion> faces;
};

// The operator's choices. Every default is a prudent value listed in the
// lot as "à trancher par l'auteur"; every one can be set per call.
struct SubjectTrackingSettings {
    // Half-extent of the zone the subject may move in without the frame
    // moving, as a fraction of the frame on that axis: 1/10 is 108 px either
    // side on a 1080-wide frame.
    EffectParamValue dead_zone_x{1, 10};
    EffectParamValue dead_zone_y{1, 10};
    // A move ends when the subject is this close to its target.
    EffectParamValue settle{1, 50};
    // The fastest the frame travels, in frames per second on its axis: 2/5
    // crosses a whole frame width in two and a half seconds.
    EffectParamValue max_speed{2, 5};
    // How fast it gets there and stops again, in frames per second squared:
    // 4/5 reaches full speed in half a second.
    EffectParamValue max_acceleration{4, 5};
    // Where the subject's face sits in the frame: centred across, and a
    // little above the middle, which leaves the headroom of a portrait.
    EffectParamValue target_x{1, 2};
    EffectParamValue target_y{2, 5};
    // The longest hole in the detections that is bridged by interpolation.
    RationalTime bridge_gap{1, 1};
    // The furthest a face may move between two samples and still be the
    // subject, in widths of the subject's face.
    EffectParamValue gate{3, 2};
    // How far the keys may stray from the computed path, as a fraction of
    // the frame: 1/1000 is about one pixel on a 1080-wide frame.
    EffectParamValue tolerance{1, 1000};
    // Who to follow when several faces are found at first: the face nearest
    // this point of the displayed source picture.
    bool has_hint = false;
    EffectParamValue hint_x{1, 2};
    EffectParamValue hint_y{1, 2};
};

bool SubjectTrackingSettingsAreValid(const SubjectTrackingSettings& settings,
                                     std::string& error);

// What the pure computation needs, and nothing it would have to fetch.
struct SubjectTrackingInput {
    // The displayed source picture, rotation applied (ClipDisplayedSize).
    int64_t source_width = 0;
    int64_t source_height = 0;
    // The sequence frame.
    int64_t frame_width = 0;
    int64_t frame_height = 0;
    // The framing the keys move: `fill`, at its scale and crops. Its own
    // position is ignored -- the keys replace it.
    ClipFraming framing;
    // The cadence the clip's frames are counted at, and how many there are.
    MediaRate rate{25, 1};
    int64_t frame_count = 0;
    std::vector<SubjectSample> samples;
};

// One axis of the result.
struct SubjectTrackingAxis {
    // False: the frame never moves on this axis, and `constant` is its
    // position. True: `keyframes` drive it (ids left empty for the caller).
    bool animated = false;
    EffectParamValue constant{0, 1};
    std::vector<ClipKeyframe> keyframes;
    // The room the fill leaves on each side, the bound of the position.
    EffectParamValue room{0, 1};
    // How many deliberate moves the operator made.
    int64_t moves = 0;
    // Frames where the subject's place lay beyond the room: held at the edge.
    int64_t frames_at_edge = 0;
    // The position at every frame, on kAnimationSampleDenominator, before
    // the keys reduced it. What the keys sample to stays within `tolerance`.
    std::vector<int64_t> path;
};

struct SubjectTrackingResult {
    SubjectTrackingAxis x;
    SubjectTrackingAxis y;
    int64_t samples = 0;
    int64_t samples_with_subject = 0;
    // Times the subject was chosen again after a gap longer than bridge_gap.
    int64_t reacquired = 0;
    // The face chosen first, as the finder gave it.
    ColorMeasureRegion first_subject;
};

// PA7 -- docs/roadmap/PARITE-2026-11.md. What a framing that moves inside a
// filled picture is made of, shared with the stabilisation
// (Stabilization.h), which moves the same position for another reason.
//
// The size of the filled picture in frames (`span`, 1 fills exactly) and
// the room it leaves on each side, on kAnimationSampleDenominator.
struct FollowedFramingRoom {
    double span_x = 1.0;
    double span_y = 1.0;
    int64_t room_x = 0;
    int64_t room_y = 0;
};
FollowedFramingRoom FramingRoomOf(const SubjectTrackingInput& input);

// One axis from its position at every frame, as a fraction of the frame:
// quantised, held inside `roomNumerator`, then reduced to the fewest linear
// keys within `tolerance` (both on kAnimationSampleDenominator).
bool FillPositionAxis(const std::vector<double>& positions,
                      int64_t roomNumerator, int64_t tolerance,
                      const MediaRate& rate, SubjectTrackingAxis& axis,
                      std::string& error);

// The clip as a moved framing reads it: its displayed size, the sequence
// frame, its frame count, and its framing put in `fill` at `scale` when
// given. Refuses a motion clip (`what` ends the sentence: "follow a face
// in"), a clip without a probed picture and a clip that animates its scale.
bool PrepareFollowedFraming(const Document& document, const Ulid& clipId,
                            const std::optional<EffectParamValue>& scale,
                            const std::string& what,
                            SubjectTrackingInput& input, bool& modeChanged,
                            std::string& error);

// The two axes laid on the clip as a SetClipFraming and a SetClipAnimation
// that replaces its position keys and keeps its others, after every frame
// was resolved through the call every renderer makes: an edge uncovered is
// a refusal.
bool LayFollowedFraming(const Document& document, const Ulid& clipId,
                        ClipFraming framing, const SubjectTrackingAxis& x,
                        const SubjectTrackingAxis& y,
                        const std::function<Ulid()>& newId,
                        std::vector<Operation>& operations, std::string& error);

// THE RULE, as a pure function: no document, no FFmpeg, no finder. Fails
// with a named reason when no sample holds a face, when the samples are not
// in strictly increasing time, or when the fill leaves no picture beyond the
// frame on either axis (a framing that fits cannot be panned).
bool ComputeSubjectTracking(const SubjectTrackingInput& input,
                            const SubjectTrackingSettings& settings,
                            SubjectTrackingResult& result, std::string& error);

// CA2 -- the computation laid on a clip of `document` as operations: a
// SetClipFraming that puts the clip in `fill` (at `scale` when given; its
// crops kept) with the constant positions, and a SetClipAnimation that
// replaces the clip's position keys and keeps every other track it carries.
// Both existing operations, so the result is reversible, journaled and
// replayed exactly as a hand-written framing is, without a face finder.
//
// Refuses a motion clip, a clip without a probed picture, and a clip that
// animates its scale (the room the keys move in would change under them).
// Before handing anything back it resolves the clip's rectangle at every
// one of its frames with the new framing and keys, through the one call
// every renderer makes, and refuses if any frame would leave an edge
// uncovered: the engine says so rather than the delivery.
bool BuildSubjectTrackingOperations(
    const Document& document, const Ulid& clipId,
    const std::vector<SubjectSample>& samples,
    const SubjectTrackingSettings& settings,
    const std::optional<EffectParamValue>& scale,
    const std::function<Ulid()>& newId, std::vector<Operation>& operations,
    SubjectTrackingResult& result, bool& modeChanged, std::string& error);

// CA3 -- the result as the JSON the tool reports.
std::string SerializeSubjectTrackingResult(const SubjectTrackingResult& result);

// CA3 -- the clip local frames the sampler reads: every `everyFrames`-th
// frame from the first, plus the last one so the end of the clip is seen.
std::vector<int64_t> SubjectSampleFrames(int64_t frameCount,
                                         int64_t everyFrames);

// CA3 -- reads the faces of `clipId` at those frames, through the face finder
// the executable installed (FaceFinder.h): each frame decoded from its
// source, the whole displayed picture reduced to `maxDimension` on its long
// edge. A process without a finder -- the build without a window, a test --
// refuses by name before decoding anything.
bool SampleClipFaces(const Document& document,
                     const std::string& projectDirectory, const Ulid& clipId,
                     int64_t everyFrames, int32_t maxDimension,
                     const std::string& ffmpegPath,
                     std::vector<SubjectSample>& samples, std::string& error);
