#pragma once

// VFR-2026-11 -- docs/roadmap/VFR-2026-11.md. The source grid: how a frame
// number names a picture of a rush.
//
// THE RULE (ClipSpeed.h) gives a frame number, floor(source time * R). This
// file says which decoded picture that number is, the same way on every
// surface that decodes one: frame k of a source is the picture on screen
// k/R after its first picture, as FFmpeg's `fps` filter resamples a stream
// anchored on its first picture with its timestamps kept -- the latest
// picture whose time, in steps of 1/R and rounded to the nearest step (a
// half away from zero), is k or less. On a constant-rate file that is
// simply its k-th picture. On a phone's variable-rate file, whose pictures
// drift around the 1/30 s grid and sometimes leave a hole, it is the
// picture nearest k/R, so the picture stays with the sound, which is placed
// by time.
//
// R is the rate the source is counted at (SourceCountingRate), chosen once
// at ingest and stored with the media. A phone counted at its average rate
// (30.004, 29.95...) put THE RULE's floor one picture before the sound on
// most frames of a 30 fps timeline: the nominal rate is the grid the camera
// recorded on.
//
// Before this ticket the export read each clip from where its `-ss` landed
// and renumbered from the first picture it kept: a picture that landed more
// than half a step late made the whole clip one picture early (VF1). The
// verification already read the grid this way; the export and the
// verification now share SourceGridReadFor below.

#include "Document.h"

#include <cstdint>
#include <string>
#include <vector>

// VF2 -- the rate a video stream's frames are counted at. `average` is the
// stream's frame count over its duration, `nominal` the base rate its
// timestamps are written on (FFmpeg's avg_frame_rate and r_frame_rate). The
// nominal rate is taken only for a variable-rate stream whose average stays
// within 2 % of it: the average is then an artefact of the jitter and the
// holes, not a cadence. An average that is already a standard rate (24000/
// 1001, 25, 30000/1001...) is kept, so that a genuine 23.976 file is never
// counted at 24 because its container says 24.
MediaRate SourceCountingRate(MediaRate average, MediaRate nominal);

// Whether `rate` is one of the cadences cameras and editors use: 24000/1001,
// 24, 25, 30000/1001, 30, 48000/1001, 48, 50, 60000/1001, 60, 100,
// 120000/1001, 120 and 240 (compared as exact fractions).
bool IsStandardFrameRate(MediaRate rate);

// The grid step a picture falls on: (pts - originPts) * timeBase * rate,
// rounded to the nearest integer with halves away from zero, exactly as
// FFmpeg's `fps` filter (round=near) and av_rescale_q_rnd(AV_ROUND_NEAR_INF)
// compute it. `timeBase` is the stream's, as num/den seconds per tick.
int64_t SourceGridStep(int64_t pts, int64_t originPts, MediaRate timeBase,
                       MediaRate rate);

// The smallest pts whose grid step is `step` or more: where a decode that
// wants frame `step` can stop at the first picture at or after it.
int64_t SourceGridFirstPts(int64_t step, int64_t originPts, MediaRate timeBase,
                           MediaRate rate);

// The presentation timestamp of the first picture a decode of the whole
// stream yields, in the stream's own time base: the grid's origin, an
// integer FFmpeg subtracts exactly (`setpts=PTS-<pts>`), never a time in
// seconds it would divide by the time base in double (ITW1). False, with
// `error`, when the file has no picture to decode.
bool FirstPicturePts(const std::string& path, int64_t& pts, std::string& error);

// How FFmpeg reads frames `first` to `last` (inclusive) of a source on its
// grid. `input_arguments` go before `-i <path>`: a seek a second ahead of
// `first`, and timestamps kept as the file has them, so that where the seek
// lands decides only how much is decoded for nothing, never which picture
// is which. `filter` starts the source's chain: it anchors the grid on the
// first picture, resamples at `rate`, and drops every frame before `first`,
// so the chain's first frame is grid frame `first`, at time base 1/rate.
// With `boundDuration`, a `-t` also bounds the read after the seek to the
// frames up to `last`, with a second of margin; the caller still trims the
// chain to the frames it shows.
struct SourceGridRead {
    std::vector<std::string> input_arguments;
    std::string filter;
};
SourceGridRead SourceGridReadFor(int64_t firstPicturePts, MediaRate rate,
                                 int64_t first, int64_t last,
                                 bool boundDuration);

// The seek SourceGridReadFor uses: `kSourceGridSeekLeadSeconds` before
// frame `first`, never before the start of the file.
inline constexpr double kSourceGridSeekLeadSeconds = 1.0;
