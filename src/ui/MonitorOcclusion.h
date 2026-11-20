#pragma once

// PERF13 -- docs/roadmap/PERF-2026-09.md. The layers of the Record monitor
// an opaque shot above hides entirely, which playback then neither decodes
// nor asks for.
//
// On the external drive projects are cut from (USB, exFAT, 41 MB/s for one
// reader), one 4K XAVC S-I stream takes 31 MB/s. Under a cutaway on V2 the
// monitor went on decoding the interview on V1: two streams, the drive
// saturated for the whole cutaway, and the picture on screen stuttered for
// the sake of one nobody could see. On the interview measured on 4 October
// 2026, nine cutaways cover 34.6 s of 73.
//
// THE RULE, the most cautious that still says yes to a plain cutaway. A
// visible video track is hidden at a position when a visible video track
// above it holds a layer the renderer (Renderer.mm, fragment_working) draws
// with an alpha of exactly one on every pixel of the frame:
//   - the only layer of its track: no dissolve under way, and no transition
//     of that track -- dissolve or colour dip -- within a frame of the
//     position;
//   - a media clip: a motion clip's alpha is only known once it is drawn;
//   - its opacity exactly 1/1, and, keyed, exactly 1/1 as sampled at this
//     frame (ClipAnimation.h), the two the renderer multiplies;
//   - a picture the frame cache holds right now, without an alpha channel.
//     A layer with nothing to draw is held, then dropped
//     (PlaybackPresentation.h): the layer under it must then be there. An
//     offline layer has nothing to draw;
//   - a rectangle that covers the frame. Framed or keyed, the one
//     ClipFraming.h resolves at this frame says `covers_frame` -- the
//     renderer draws exactly that rectangle. Otherwise the renderer fits the
//     picture, which covers the frame only when the picture, turned upright,
//     has exactly the frame's aspect;
//   - the monitor fitted to its view. Zoomed, an unframed picture is drawn
//     at its own size and may overflow a framed one above it.
// A clip has no blend mode (only a motion layer has one, inside its
// composition), so normal blending holds by construction. Anything the rule
// cannot decide -- a framing that does not resolve, a keyed value that does
// not sample, a transition it cannot place -- leaves the track below shown.
//
// Exactly the same image: the working pass blends source over destination,
// so where the top layer's alpha is one the colour beneath is overwritten
// whatever it was, and while the monitor is fitted no layer draws outside
// the frame. Checked pixel for pixel on the GPU by
// tests/monitor_occlusion_render_tests.mm.
//
// Plain C++ with no AppKit, like DecoderPool.h; main.mm only wires it. See
// tests/monitor_occlusion_tests.cc.

#include "RationalTime.h"
#include "Ulid.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>

class Document;
class Timeline;

namespace monitor_occlusion {

// A picture of a source the frame cache holds right now, as the renderer
// would draw it: its size once turned upright and whether it carries an
// alpha channel.
struct Picture {
    int64_t width = 0;
    int64_t height = 0;
    bool alpha = false;
};

// A picture `width` x `height` as stored, displayed with a counterclockwise
// rotation of `rotationDegrees`: the quarter turns Renderer.mm applies, the
// nearest to the angle, swap its sides.
Picture UprightPicture(int64_t width, int64_t height, int32_t rotationDegrees,
                       bool alpha);

// What the monitor would draw for `source` now, its frame `frame` asked
// for; nothing when it has nothing to draw (no picture cached, the media
// offline). main.mm answers from the frame cache.
using PictureLookup =
    std::function<std::optional<Picture>(const Ulid& source, int64_t frame)>;

// The visible video tracks hidden at `position`: every one below the
// highest whose layer covers the frame. Empty when the monitor is zoomed.
std::set<Ulid> HiddenTracks(const Document& document, const Timeline& timeline,
                            RationalTime position, const PictureLookup& picture,
                            bool zoomed);

// For each of `hidden` -- HiddenTracks at `position` -- the first sequence
// frame within `lookahead` of playback in the sense of `direction` at which
// it is shown again, so that main.mm asks its decoder for that frame ahead,
// as it does for a cut. A track hidden through the whole window has no
// entry. At speed n the window spans n times `lookahead`, the same
// wall-clock warning; it stops at either end of the timeline.
std::map<Ulid, RationalTime> NextShown(
    const Document& document, const Timeline& timeline, RationalTime position,
    int direction, RationalTime lookahead, const std::set<Ulid>& hidden,
    const PictureLookup& picture, bool zoomed);

}  // namespace monitor_occlusion
