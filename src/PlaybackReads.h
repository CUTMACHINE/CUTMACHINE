#pragma once

// PERF14 -- docs/roadmap/PERF-2026-09.md. What playback reads of a clip,
// worked out from the document: the source frames it shows (the bound of
// every read ahead of it, ReadAhead.h), and how many source frames a few
// seconds of it are. Plain C++ read from a document, no AppKit; main.mm
// only wires it. See tests/read_ahead_tests.cc.

#include "MonitorOcclusion.h"
#include "RationalTime.h"
#include "ReadAhead.h"
#include "Ulid.h"

#include <cstdint>
#include <optional>

class Document;
class Timeline;
struct DocumentClip;

namespace playback_reads {

// The first and last source frames `clip` shows, by the rule every surface
// decodes by (ClipSourceFrameAt, ClipSpeed.h). None when the clip is not
// read that way: a motion clip, which no decoder reads; a clip whose source
// the document does not hold; and a clip with a transition on either side,
// which a dissolve reads past its own range into its handles -- that one is
// read ahead as before, unbounded.
std::optional<read_ahead::SourceSpan> ClipReadSpan(const Document& document,
                                                   const DocumentClip& clip);

// The source frames `seconds` of `clip` played through cover, rounded up:
// its source's cadence times its speed. 0 for a clip whose source the
// document does not hold.
int64_t SourceFramesIn(const Document& document, const DocumentClip& clip,
                       int64_t seconds);

// The pictures of the library's media as their metadata describes them
// (size, rotation, a pixel format with alpha), for planning what to read:
// MonitorOcclusion decides what the monitor hides from the pictures in the
// frame cache, which a shot not yet decoded does not have. A wrong guess
// here moves a bound of the read-ahead, never the picture shown.
monitor_occlusion::PictureLookup PicturesFromLibrary(const Document& document);

// ClipReadSpan, ended where the picture of `clip` on `trackId` stops being
// seen within `lookahead` of `position` in `direction`: where an opaque shot
// above starts hiding the track (HiddenTracks with `pictures`). Under a
// cutaway the interview is not shown, and what was read of it ahead under
// the cutaway was read for nothing, at the moment the cutaway needed the
// disk: with four seconds asked ahead and only the out point to stop them,
// up to 124 MB a cutaway, and more late ticks than with no read-ahead at
// all (PERF14).
std::optional<read_ahead::SourceSpan> VisibleReadSpan(
    const Document& document, const Timeline& timeline, const Ulid& trackId,
    const DocumentClip& clip, RationalTime position, int direction,
    RationalTime lookahead, const monitor_occlusion::PictureLookup& pictures,
    bool zoomed);

// PERF17 -- the first source frame of the next piece of the same rush that
// `clip` cuts to on `trackId`, when playback forward from `position`
// reaches that cut within `lookahead`: the clip starting where `clip` ends,
// of the same source, no transition between them (a dissolve reads both
// pieces at once, through their handles, as before), and seen when it
// starts (no opaque shot above, `pictures`). Nothing otherwise, and
// nothing played backward. The decoder reading `clip` decodes on into it
// (read_ahead::FollowOnFill).
std::optional<int64_t> NextPieceFrame(
    const Document& document, const Timeline& timeline, const Ulid& trackId,
    const DocumentClip& clip, RationalTime position, int direction,
    RationalTime lookahead, const monitor_occlusion::PictureLookup& pictures,
    bool zoomed);

}  // namespace playback_reads
