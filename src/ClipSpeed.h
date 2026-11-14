#pragma once

// R3 -- docs/roadmap/REEL-2026-09.md. A clip's constant speed, and the one
// rule that says which source frame an exported, monitored or sheeted frame
// shows.
//
// A fashion film shot at 50 or 100 i/s lives on this: the first film
// measured put 31 shots out of 31 at 1/2, the two cut-down versions 32 of
// 113 and 27 of 92. Every one of them is a rush at twice the sequence
// cadence, shown frame for frame -- no interpolation, no ramp, no freeze.
//
// Why it is here rather than in Timeline.cc: the monitor, the export, the
// sheets (read_frame, contact_sheet, cut_sheet) and verify_export all have
// to name the same source frame. Two answers to that question diverge
// without any test failing -- the trap the drawn and the clickable timeline
// already set (AGENTS.md). So the arithmetic lives once, in portable C++
// with no AppKit and no FFmpeg, and every surface calls it.
//
// THE RULE. A clip at speed `p/q`, opened on `source_in`, on a track of a
// sequence running at `R_seq`, reading a source running at `R_src`. Its
// output frame `k` (k = 0 is the clip's first frame) shows the source frame
//
//     floor( ( source_in + (p/q) * (k / R_seq) ) * R_src )
//
// Everything before the floor is exact rational arithmetic; the floor is the
// single rounding, and it goes towards -infinity, which is what
// RationalTime::to_frames already does everywhere else in the engine.
//
// What it gives, on the cases the ticket names:
//
//   1/2 on a 50 i/s rush in a 25 i/s sequence -> k, k+1, k+2 ...
//       each source frame exactly once;
//   1/1 on the same rush                      -> 2k, one source frame in two;
//   1/2 on a 25 i/s rush                      -> floor(k/2), each source
//       frame twice -- a duplicate, never an interpolation;
//   2/1 on a 50 i/s rush                      -> 4k;
//   2/5 on a 25 i/s rush                      -> floor(2k/5), each source
//       frame 2.5 times on average, and the floor is what decides which of
//       them is seen twice and which three times.
//
// The speed is exact (EffectParamValue, a num/den pair, like opacity and
// audio_gain_db) and the times are exact (RationalTime). The single step to
// floating point is at the render, as ROADMAP.md's crosscutting guard rail
// asks.
//
// WHY source_in MUST LAND ON A SOURCE FRAME. The export cannot be handed
// this rule frame by frame: it hands FFmpeg the clip as a stream reopened at
// an effective cadence (SpeedAdjustedSourceRate below) and lets `fps` pick.
// That picks source frame `floor(k * p * R_src / (q * R_seq))` counted from
// the frame the seek landed on, which equals the rule above exactly when
// `source_in * R_src` is a whole number -- floor(F + x) = F + floor(x) holds
// for an integer F and nothing weaker. Document::Validate therefore refuses
// a non-unit speed on a clip whose source_in falls inside a source frame,
// and an edit that would move it there is refused rather than exported one
// frame away from what the monitor shows.
//
// A RAMP (criterion 4, 11 October 2026). A ramp replaces the constant `p/q`
// above by keys of speed over the clip's own time (DocumentClip::speed_ramp),
// and the source position becomes the integral of that curve from the clip's
// head:
//
//     floor( ( source_in + integral_0^(k / R_seq) v(t) dt ) * R_src )
//
// Between two keys the speed is held or travels in a straight line, and
// nothing else: the integral of a line is a parabola, whose value at a
// rational time is a rational number, so the rule above stays exact up to
// its one floor. A cubic-bezier between two keys is refused, not
// approximated: its integral at a given time needs the root of a cubic,
// which is no fraction of anything, and a double there would be a second
// rounding the monitor and the export could make differently. A smooth ramp
// is written with more straight keys. Before the first key and after the
// last the speed is the outermost key's, as R2 clamps (ClipAnimation.h), and
// the keys follow R2's rule for time: local to the clip, shifted by a head
// trim, kept when they fall outside it.
//
// The integral is computed here, in ClipSourceFrameAt and the functions
// beside it, and nowhere else. No FFmpeg expression evaluates it: `fps` can
// resample a constant cadence and nothing more, so a ramp is exported as the
// explicit per-frame list ClipSourceFrames gives (Export.cc spells it).
// ITW1 is the reason: an expression FFmpeg evaluates in double is an
// expression that moves a cut by one frame, 133 times in 1400.
//
// A ramp is picture only, like a freeze. `atempo` stretches a sound by one
// factor, and no chain of constant factors follows a speed that changes
// every frame: a ramp is refused on a sound clip and on a clip linked to
// one, rather than playing its sound at a speed the picture does not have
// (criterion 2). And unlike a constant speed it does not ask source_in to
// fall on a source frame: the export seeks to the start of the first frame
// the list names and counts from there, so a head trim inside a ramp may
// open the clip anywhere in a frame.

#include "Document.h"
#include "RationalTime.h"

#include <cstdint>
#include <string>
#include <vector>

// The speed a clip has when nothing set one: play the source through at the
// sequence cadence. A clip at unit speed is written, exported and monitored
// exactly as it was before this ticket, byte for byte.
inline bool IsUnitSpeed(const EffectParamValue& speed) {
    return speed.num == speed.den;
}

// Criterion 4 -- whether the clip's speed is a ramp. A ramped clip keeps its
// constant `speed` at 1/1 (Document::Validate): the keys are its speed.
inline bool ClipHasSpeedRamp(const DocumentClip& clip) {
    return !clip.speed_ramp.empty();
}

// Whether the clip plays its source at anything but 1/1, ramp included.
// Every tool that maps source time onto a timeline only at 1/1 asks this
// one, so a ramp is refused wherever a constant speed already was.
inline bool ClipHasSpeed(const DocumentClip& clip) {
    return !IsUnitSpeed(clip.speed) || ClipHasSpeedRamp(clip);
}

// R4 -- docs/roadmap/REEL-2026-09.md, decided by the author on 8 October 2026. A
// freeze frame is a picture clip at speed 0/1: THE RULE below names
// floor(source_in * R_src) for every output frame, the one frame held for
// the clip's whole duration, and the clip reads no span of its source. No
// new kind of clip: everything that already names a frame through this file
// names the held one. Picture only: a held picture has no sound to hold,
// so a freeze is refused on an audio track and on a clip linked to one.
inline bool IsFreezeSpeed(const EffectParamValue& speed) {
    return speed.num == 0 && speed.den > 0;
}
inline bool ClipIsFreeze(const DocumentClip& clip) {
    return IsFreezeSpeed(clip.speed);
}

// The bounds a document may carry. Positive, or 0/1, the freeze (R4): a
// negative speed is a reverse, which is a different feature (the source
// would have to be read backwards, which neither the decoder path nor `fps`
// does). The magnitudes keep the effective cadence handed to FFmpeg a small
// rational.
inline constexpr int32_t kMaxClipSpeedTerm = 1000;

bool ClipSpeedIsValid(const EffectParamValue& speed, std::string& error);

// Criterion 4 -- the keys a ramp may carry: none (no ramp), or at least two
// (one key is a constant speed, which belongs in `speed`), at most
// kMaxClipKeyframes, ids present and distinct, times strictly increasing,
// values inside ClipSpeedIsValid's bounds, and each key held or linear --
// a cubic-bezier is refused by name, for the reason the header gives. Says
// nothing about the source: Document::Validate checks the span the ramp
// reads against it.
bool ClipSpeedRampIsValid(const std::vector<ClipKeyframe>& ramp,
                          std::string& error);

// `time` multiplied by `speed`, exactly. The identity -- the same rate, the
// same value -- at unit speed, so that a project that sets no speed keeps
// its bytes.
RationalTime ScaleBySpeed(const RationalTime& time,
                          const EffectParamValue& speed);

// The inverse: the timeline span a source span covers at `speed`. Refuses
// nothing; the caller checks what it gets back the way it checks any other
// exact time.
RationalTime UnscaleBySpeed(const RationalTime& sourceTime,
                            const EffectParamValue& speed);

// The source time a clip reads between its own local times `from` and `to`:
// the integral of its speed over that stretch, ScaleBySpeed(to - from) at a
// constant speed. Negative when `to` is before `from`. Throws
// std::overflow_error when a ramp's integral has no RationalTime spelling
// (a denominator past int32_t): refused, never rounded.
RationalTime ClipSourceAdvanceBetween(const DocumentClip& clip,
                                      const RationalTime& from,
                                      const RationalTime& to);

// The same from the clip's head: what a head trim of `offset` adds to
// source_in, and what a cut at `offset` gives the right piece. At a constant
// speed it is ScaleBySpeed(offset, speed), spelled the way it always was.
RationalTime ClipSourceAdvance(const DocumentClip& clip,
                               const RationalTime& offset);

// The source a clip consumes: the advance over its whole duration.
RationalTime ClipSourceSpan(const DocumentClip& clip);

// source_in + the span above: the exclusive end of the source range read.
RationalTime ClipSourceEnd(const DocumentClip& clip);

// The source time the clip reads `offset` into its own timeline span.
RationalTime ClipSourceTimeAt(const DocumentClip& clip,
                              const RationalTime& offset);

// THE RULE, as a frame number: the source frame shown `offset` into the
// clip. Every surface that decodes a picture goes through here. On a ramp
// the integral is never spelled as a RationalTime on the way: it is
// floored straight from the exact fraction, so no denominator limit can
// refuse a frame the document holds.
int64_t ClipSourceFrameAt(const DocumentClip& clip, const RationalTime& offset,
                          const MediaRate& sourceRate);

// Criterion 4 -- THE RULE for `count` consecutive output frames of the
// clip from its local frame `first`, at the sequence cadence: the explicit
// list the export spells for a ramp, and nothing but ClipSourceFrameAt
// called once per frame, so the list and the monitor cannot disagree.
std::vector<int64_t> ClipSourceFrames(const DocumentClip& clip, int64_t first,
                                      int64_t count,
                                      const MediaRate& sequenceRate,
                                      const MediaRate& sourceRate);

// The fastest the clip plays its source: its constant speed, or the
// largest key of its ramp. The read-ahead sizes its decoding by it
// (PlaybackReads.cc), which has to be enough at every frame.
EffectParamValue ClipPeakSpeed(const DocumentClip& clip);

// The clip's first local frame showing `sourceFrame`, or -1 when none in
// [0, frames) does. The inverse of THE RULE, by search: on a slow motion
// several frames show the same source frame, and the first is the one a
// still taken "at that source time" was asked for (FrameCapture.cc).
int64_t ClipFirstLocalFrameShowing(const DocumentClip& clip,
                                   int64_t sourceFrame, int64_t frames,
                                   const MediaRate& sequenceRate,
                                   const MediaRate& sourceRate);

// R2's rule for time, applied to a ramp's keys: the head moved by `advance`
// of local time, so every key moves the other way and stays on its frame.
void ShiftClipSpeedRampHead(DocumentClip& clip, const RationalTime& advance);

// The start of the source frame the rule names, as an exact time: what a
// single-frame read hands FFmpeg's `-ss`. An accurate seek keeps the first
// frame at or after the time it is given, so the rule's own time, which lands
// inside a frame wherever the speed or the two cadences put it there (source
// frame 19.5 at 1/2), showed the next frame where the monitor and the export
// show this one (agentic QA of 23 September 2026: cut_sheet, 1/2 on a
// 25 i/s rush). `rate` must be positive; the time comes back unchanged
// otherwise.
RationalTime SourceFrameStart(const RationalTime& time, const MediaRate& rate);

// Whether `time` falls exactly on a frame boundary of a source at `rate`.
bool IsOnSourceFrameBoundary(const RationalTime& time, const MediaRate& rate);

// The cadence a source has to be read at for the rule above to fall out of a
// plain resampling: R_src * p / q. At 1/2 a 50 i/s rush is read as a 25 i/s
// one, which is exactly what a constant slow motion is.
MediaRate SpeedAdjustedSourceRate(const MediaRate& sourceRate,
                                  const EffectParamValue& speed);

// R3, criterion 3 and guard rail 5 of the lot. The refusal a tool gives
// when it maps a source time onto a timeline (or back) and cannot do it
// exactly on a clip whose speed is not 1/1. A named refusal, never a result
// that is quietly one frame out: B3's lesson is that `applied: true` over a
// wrong answer is worse than no answer.
std::string ClipSpeedRefusal(const DocumentClip& clip, const std::string& what);

// The `atempo` chain that stretches a linked clip's sound by the same
// factor, pitch kept (atempo resamples in time, not in frequency). Written
// as exact fractions, each factor inside atempo's own [0.5, 2] window so the
// chain holds on every FFmpeg build; their product is the speed exactly.
// Empty at unit speed.
std::string SpeedAtempoChain(const EffectParamValue& speed);
