#pragma once

// R2 -- docs/roadmap/REEL-2026-09.md. What a clip's animated parameters are
// worth at one of its own frames, and what becomes of its keys when the clip
// is trimmed, moved or cut in two.
//
// Keyframes.h has been exact and tested since M1, and only the motion used
// it: no parameter of a clip could move. The two cut-down versions of the
// film measured on 20 September are built on that movement -- 20 shots of
// animated framing on one, 10 animated plus 2 fixed plus 3 shakes on the
// other -- and the relevé says why: "a punchy zoom on a strong beat, a slow
// push, a zoom-through from one shot to the next, a three-frame shake on an
// impact". It is the animation that carries the rhythm, not the cut alone.
//
// Why it is here rather than in Export.cc or Renderer.mm: the monitor, the
// export, the sheets (read_frame, contact_sheet, cut_sheet) and
// verify_export all have to show the same value at the same frame. Two
// answers to that question diverge without any test failing -- the trap the
// drawn and the clickable timeline already set (AGENTS.md), and guard rail 1
// of this lot. So the arithmetic lives once, in portable C++ with no AppKit,
// no Metal and no FFmpeg, and every surface calls it.
//
// THE RULE FOR TIME (criterion 1). A key's time is local to the clip: zero
// is the clip's first frame. A key therefore names a frame of the clip,
// counted from its head, and it stays on that frame:
//
//   * MOVED on the timeline (timeline_in changes, nothing else): nothing to
//     do. Local time is measured from the head, and the head moved with the
//     clip. A zoom stays on the same pictures and keeps its shape.
//   * HEAD TRIMMED (frames removed from or added to the head): every key
//     shifts by the same amount, in the opposite direction. The frame that
//     was local 10 and is now local 5 keeps its key. The zoom does not slide
//     off the gesture it was put on -- which is the one thing a cutter would
//     notice immediately.
//   * TAIL TRIMMED: nothing to do; times are counted from the head.
//   * SLIPPED (source_in changes, the clip's frames stay where they are):
//     nothing to do. A slip changes what the frames show, never where they
//     are; the animation was put on the beat, and the beat has not moved.
//   * CUT IN TWO at local offset s: the left piece keeps every key as it is,
//     the right piece keeps every key shifted by -s. Sampling either piece
//     at a given timeline frame gives the value the uncut clip gave there,
//     which is the property a cut must have and the one this ticket tests.
//
// AND KEYS THAT FALL OUTSIDE THE CLIP ARE KEPT, not dropped. Two reasons,
// and the second is the hard one:
//
//   1. A curve is shaped by its keys, including the ones just outside. Drop
//      the key three frames before the new head and the slope at the head
//      changes; the picture jumps where nothing was edited.
//   2. Dropping is not reversible. The inverse of a trim is a trim back, and
//      a trim back cannot invent a key the trim threw away. Apply -> undo
//      would stop being byte for byte, which is the whole of principle 6.
//
// So a key can sit at a negative local time or past the clip's last frame.
// Evaluation clamps at the outermost key on each side (Keyframes.h's
// KeyframeExtrapolation::Clamp), so nothing outside the clip is ever shown;
// it only shapes the curve inside.
//
// THE RULE FOR VALUE (criterion 2). The value is sampled from the whole
// frame number and nothing else. Output frame k of the clip is sampled at
// the exact rational time k / R_seq, never at a timestamp in seconds: this
// is ITW1's lesson, an expression evaluated in floating-point time moved a
// cut by one frame 133 times in 1400. The monitor, the export, the sheets
// and verify_export all ask this file for frame k and all get the same
// answer.
//
// WHERE EXACT BECOMES FLOATING POINT, AND WHY IT IS ALLOWED HERE (guard
// rail 2). Three steps, in this order:
//
//   1. the key's time and the sampled time are RationalTime and are compared
//      exactly -- a time never becomes a double, here as everywhere else
//      (principle 4);
//   2. the key's value and its easing control points are exact fractions in
//      the document, and are converted to double only to be handed to
//      KeyframeTrack::Evaluate. A cubic-bezier between two keys is not a
//      rational number of anything, so the evaluation itself is floating
//      point, as Keyframes.h decided at M1;
//   3. the double that comes out is quantised, once, back to an exact
//      fraction over kAnimationSampleDenominator, rounded to nearest with
//      ties away from zero -- and everything downstream is exact again. R1's
//      geometry is handed that fraction and works the way it always has.
//
// Why that is acceptable for a parameter and would not be for a time: a
// frame lost in a rounded time is a cut that no longer falls right, and it
// cannot be found again three hours later. A scale rounded to one
// ten-thousandth is a tenth of a pixel on a 1080-pixel frame, and R1 rounds
// the rectangle to whole pixels straight after. The error is bounded, it is
// in the one dimension where the output is whole pixels anyway, and -- the
// part that matters for guard rail 1 -- it is made in ONE place, so the
// monitor and the export round it identically instead of each rounding its
// own way.
//
// WHAT R2 DOES NOT ANIMATE, AND WHY IT IS MEASURED AND NOT AN OVERSIGHT.
// The crops (ClipFraming::crop_left and its three siblings) stay constant.
// An animated crop changes the source rectangle read frame by frame, and the
// export has no construction for that: `crop`'s width and height are
// expressions FFmpeg evaluates once, at configuration time -- measured, not
// read in the documentation -- so a per-frame crop would need a command
// stream in seconds, which is the one thing guard rail 2 forbids. Nothing in
// the corpus asks for it either: the 35 animated shots of the two versions
// measured are zooms, pushes and shakes, that is scale and position, plus
// the flashes, which are opacity. The day a film asks for a moving crop it
// is a ticket, with the export construction as its first question.
//
// Audio gain and the `color.*` knobs are out for the reason the ticket
// gives: R0 has not asked for them.

#include "ClipFraming.h"
#include "Document.h"
#include "Keyframes.h"
#include "RationalTime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The denominator every sampled value lands on (step 3 above). Ten thousand:
// fine enough that the quantisation is under a tenth of a pixel on any frame
// this engine delivers, coarse enough that the numerator of a scale or an
// offset stays inside kMaxFramingTerm (ClipFraming.h), which is what R1's
// validator accepts.
inline constexpr int32_t kAnimationSampleDenominator = 10000;

// The largest magnitude a key may carry. The scale shares R1's practical
// ceiling; ten times "exactly filled" is already an unreadable blow-up, and
// it is what keeps kAnimationSampleDenominator * value inside
// kMaxFramingTerm. The offset shares R1's own eight-frames rule.
inline constexpr int32_t kMaxAnimatedScale = 10;
inline constexpr int32_t kMaxAnimatedOffset = 8;

// Keys per parameter. A bound exists so that a document cannot ask the
// export for a filter expression megabytes long; 4096 is two and a half
// minutes of a key on every frame at 25 i/s, far past anything an author
// writes by hand or an agent writes on a beat grid.
inline constexpr std::size_t kMaxClipKeyframes = 4096;

// The name a parameter carries in a document, an operation and a tool.
const char* ClipAnimatedParameterName(ClipAnimatedParameter parameter);
bool ParseClipAnimatedParameterName(const std::string& name,
                                    ClipAnimatedParameter& parameter);

// The interpolation's name, spelled as CSS spells it, in the document, in
// the journal and in a tool answer.
const char* KeyframeInterpolationName(KeyframeInterpolation interpolation);
bool ParseKeyframeInterpolationName(const std::string& name,
                                    KeyframeInterpolation& interpolation);

// Whether this clip drives that parameter.
bool ClipAnimates(const DocumentClip& clip, ClipAnimatedParameter parameter);
const ClipParameterAnimation* FindClipAnimation(
    const DocumentClip& clip, ClipAnimatedParameter parameter);

// Field-level validity: one entry per parameter, at least one key in an
// entry (a single key holds its value over the whole clip, R2c), ids present
// and distinct, times strictly increasing, values and easings inside the
// bounds above. Says nothing about whether the result lands on screen --
// that needs the source, and ResolveClipFramingAtFrame answers it.
bool ClipAnimationIsValid(const std::vector<ClipParameterAnimation>& animation,
                          std::string& error);

// R2c -- docs/roadmap/REEL-2026-09.md. Keys edited one at a time, at a frame
// of the clip, the way the Inspector's diamond and an animated slider edit
// them. Each answer is the clip's complete new animation, the one
// SetClipAnimation carries, so a key posed or removed is one operation and
// one undo. A key belongs to the frame its time falls in: `localFrame`
// covers [localFrame, localFrame + 1) in the sequence's cadence, so a key an
// agent wrote between two frames is found on the frame it is shown on.

// The frame of the clip a key falls on (rounded toward the past).
int64_t ClipKeyframeFrame(const ClipKeyframe& keyframe,
                          const MediaRate& sequenceRate);

// The key of `parameter` on frame `localFrame`, or null.
const ClipKeyframe* FindClipKeyframeAt(const DocumentClip& clip,
                                       ClipAnimatedParameter parameter,
                                       int64_t localFrame,
                                       const MediaRate& sequenceRate);

// The frame of the nearest key of `parameter` strictly before, or strictly
// after, `localFrame` -- where the Inspector's arrows take the playhead.
std::optional<int64_t> PreviousClipKeyframeFrame(
    const DocumentClip& clip, ClipAnimatedParameter parameter,
    int64_t localFrame, const MediaRate& sequenceRate);
std::optional<int64_t> NextClipKeyframeFrame(const DocumentClip& clip,
                                             ClipAnimatedParameter parameter,
                                             int64_t localFrame,
                                             const MediaRate& sequenceRate);

// The animation with `parameter` holding `value` at `localFrame`: the key
// already there takes the value, keeping its identity, time and curve, or a
// key `id` is added on the frame's exact time. A new key travels to the
// next one as the key before it did, so a segment cut in two keeps its
// curve on both halves; a first key travels linearly, Resolve's default. A
// parameter that had no key gets an entry, in the order of the parameters.
// Refused when the value is outside the parameter's bounds.
bool ClipAnimationWithKey(const DocumentClip& clip,
                          ClipAnimatedParameter parameter, int64_t localFrame,
                          const MediaRate& sequenceRate,
                          const EffectParamValue& value, const Ulid& id,
                          std::vector<ClipParameterAnimation>& animation,
                          std::string& error);

// The animation without the key of `parameter` on `localFrame`; the
// parameter's entry goes with its last key. Refused when there is none.
bool ClipAnimationWithoutKey(const DocumentClip& clip,
                             ClipAnimatedParameter parameter,
                             int64_t localFrame, const MediaRate& sequenceRate,
                             std::vector<ClipParameterAnimation>& animation,
                             std::string& error);

// What a clip's animated fields are worth at one of its own frames. The
// fields the clip does not animate keep the constants it carries, so this is
// also the right answer for a clip that animates nothing.
struct ClipSampledParameters {
    EffectParamValue opacity{1, 1};
    ClipFraming framing;
};

// THE RULE, as a value. `localFrame` is the clip's own output frame, zero
// for its first; `sequenceRate` is the cadence those frames are counted at.
// Negative and past-the-end frames are answered, clamped, because a
// transition's handles read a few frames on either side.
bool SampleClipParameters(const DocumentClip& clip, int64_t localFrame,
                          const MediaRate& sequenceRate,
                          ClipSampledParameters& sampled, std::string& error);

// R1's geometry, handed the values sampled at `localFrame`. This is the one
// call an animated clip's consumers make, and it is deliberately the only
// bridge between the two tickets: the rectangle is still resolved by
// ResolveClipFraming and by nothing else (guard rail 1).
bool ResolveClipFramingAtFrame(const Document& document,
                               const DocumentClip& clip, int64_t localFrame,
                               int64_t frameWidth, int64_t frameHeight,
                               ClipFramingGeometry& geometry,
                               EffectParamValue& opacity, std::string& error);

// How many output frames the clip has, at the sequence's cadence. Refuses a
// duration that does not land on a whole frame rather than rounding it.
bool ClipFrameCount(const DocumentClip& clip, const MediaRate& sequenceRate,
                    int64_t& frames, std::string& error);

// The clip's head moved by `advance` of its own local time -- positive when
// frames were removed from the head, negative when they were given back.
// Shifts every key the other way, which is the rule written above. Does
// nothing to a clip that carries no key, so every call site may make it
// unconditionally. A speed ramp's keys move with them (ClipSpeed.h).
void ShiftClipAnimationHead(DocumentClip& clip, const RationalTime& advance);

// The refusal a surface gives when it cannot honour an animated clip. Named,
// never a silent still frame: guard rail 5's lesson is that `applied: true`
// over a wrong answer is worse than no answer.
std::string ClipAnimationRefusal(const DocumentClip& clip,
                                 const std::string& what);
