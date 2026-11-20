#pragma once

// R1 -- docs/roadmap/REEL-2026-09.md. Where a clip's picture lands in the
// sequence frame, and which part of the source is read to fill it.
//
// A 16:9 rush in a 1080x1920 sequence used to come out with black bars,
// because the export reduced every source to fit (`scale=...:
// force_original_aspect_ratio=decrease`) and centred it
// (`overlay=x=(W-w)/2:y=(H-h)/2`), while the monitor did the same sum of
// its own (`fitScale`, Renderer.mm). The first film measured reframed 15 of
// its 31 shots -- a zoom of 1.1 to 1.45 and an offset of up to 100 px, to
// push a ceiling, a light stand, a reflector or a poster out of frame.
//
// Why it is here rather than in Export.cc or Renderer.mm: the monitor, the
// export, the sheets (read_frame, contact_sheet, cut_sheet) and
// verify_export all have to place the same rectangle. Two answers to that
// question diverge without any test failing -- the trap the drawn and the
// clickable timeline already set (AGENTS.md), and guard rail 1 of this lot.
// So the arithmetic lives once, in portable C++ with no AppKit, no Metal
// and no FFmpeg, and every surface calls it.
//
// THE ORDER, fixed and applied in this sequence:
//
//   1. the source's display rotation. Everything below is in *displayed*
//      pixels: a quarter turn in the file's display matrix swaps width and
//      height first (DisplayDimensions, SequenceFormat.h). Nobody
//      re-derives it -- FFmpeg's decoder applies the matrix on its own and
//      the monitor applies `quarterTurns` in the shader -- so this function
//      is handed the displayed size and never the stored one.
//   2. the crop: fractions of each edge of that displayed picture are
//      thrown away, leaving the rectangle actually read.
//   3. the mode: `fit` scales that rectangle until it is contained by the
//      frame, `fill` until it contains the frame.
//   4. the scale: an exact fraction on top of the mode's scale, so 1/1 is
//      "exactly fitted" or "exactly filled".
//   5. the position: the centre of the result moves by a fraction of the
//      sequence frame, positive x right and positive y down.
//
// THE RECTANGLE. With a displayed source Wd x Hd, a sequence frame W x H,
// crops l, r, t, b, a scale k and a position (px, py):
//
//     sw = Wd * (1 - l - r)          the cropped source, in source pixels
//     sh = Hd * (1 - t - b)
//     s0 = min(W/sw, H/sh)  in fit  |  max(W/sw, H/sh)  in fill
//     s  = s0 * k
//     dw = sw * s                    the picture, in sequence pixels
//     dh = sh * s
//     left   = W * (1/2 + px) - dw/2
//     top    = H * (1/2 + py) - dh/2
//     right  = left + dw
//     bottom = top + dh
//
// Every line above is exact rational arithmetic. The four edges -- not the
// size and the origin separately -- are then rounded once, to the nearest
// whole sequence pixel, ties away from zero. Rounding the edges is what
// keeps two pictures that met exactly from being handed a one-pixel gap.
//
// The part of that rectangle inside [0, W] x [0, H] is what is drawn; what
// falls outside is what `fill` cuts. The source rectangle read is the
// preimage of the drawn part, rounded to whole source pixels and then to an
// even grid: a subsampled plane (yuv420p, yuv422p) cannot be cut at an odd
// column, and FFmpeg's `crop` rounds it down behind the caller's back. The
// engine does that rounding itself, once, so every surface reads the same
// rectangle. (A 4:1:1 source would need a multiple of four; none exists in
// the corpus, and that is a ticket the day one does.)
//
// WHAT THE ENGINE MEASURES AND THE AUTHOR DOES NOT (guard rail 4). The
// caller chooses the scale and the position; it never computes a rectangle.
// This function hands back the rectangle, the source region it reads, and
// the frame edges the picture leaves uncovered -- the black bars, named in
// pixels, rather than discovered in the delivery.
//
// WHAT R1 DOES NOT DO. No keyframes: the framing is constant over the clip.
// R2 is where an animated framing goes, and it goes into the fields
// themselves (Document.h) -- each becomes sampleable in the clip's own
// local time and this function is handed the values sampled at the frame
// being rendered. Nothing below changes: a rectangle per frame is still one
// call of ResolveClipFraming per frame.

#include "Document.h"

#include <cstdint>
#include <string>

// The bound each term of a framing fraction stays inside. Large enough for
// any scale or offset an edit expresses, small enough that the products of
// the rule above stay far inside a 128-bit intermediate.
inline constexpr int32_t kMaxFramingTerm = 100000;

// Each fraction in its smallest terms. Set before the framing is stored,
// for the reason set_clip_speed reduces a speed: 22/20 and 11/10 are the
// same zoom and must be the same bytes, and a framing spelled entirely in
// its own defaults (`scale: 2/2`, `crop_left: 0/4`) must become the default
// the file omits rather than a key an older reader would drop.
ClipFraming ReduceClipFraming(ClipFraming framing);

// Field-level validity: positive denominators, a positive scale, crops
// inside [0, 1) whose two ends leave a picture, an offset that stays within
// a few frames of the centre. Says nothing about whether the result lands
// on screen -- that needs the source, and ResolveClipFraming answers it.
bool ClipFramingIsValid(const ClipFraming& framing, std::string& error);

// The rectangle, and everything derived from it, in whole pixels.
struct ClipFramingGeometry {
    // The source region read, in displayed source pixels: what a crop
    // filter is given, on the even grid described above.
    int64_t source_x = 0;
    int64_t source_y = 0;
    int64_t source_width = 0;
    int64_t source_height = 0;
    // Where that region is drawn, in sequence pixels. Always inside the
    // frame: this is the picture after `fill` has cut what overran.
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    int64_t height = 0;
    // The whole picture before the frame clipped it, in sequence pixels.
    // Negative, or past the frame, exactly as far as the framing pushes it.
    int64_t full_x = 0;
    int64_t full_y = 0;
    int64_t full_width = 0;
    int64_t full_height = 0;
    // R2 -- docs/roadmap/REEL-2026-09.md. The crop alone, in displayed
    // source pixels: what the crop fractions keep of the source, before
    // the frame clipped anything. `source_*` above is the preimage of what
    // is *drawn* and therefore moves with an animated scale or position;
    // this one depends only on the crops, which R2 does not animate, so it
    // is the constant `crop` an animated clip's filter chain is built on.
    // Exposed here rather than worked out again in Export.cc, because two
    // answers to the same rectangle is exactly what guard rail 1 forbids.
    int64_t crop_x = 0;
    int64_t crop_y = 0;
    int64_t crop_width = 0;
    int64_t crop_height = 0;
    // The frame edges the picture does not reach, in sequence pixels: the
    // black bars, zero on an edge it covers.
    int64_t uncovered_left = 0;
    int64_t uncovered_top = 0;
    int64_t uncovered_right = 0;
    int64_t uncovered_bottom = 0;
    // No uncovered edge at all: nothing of the frame is left black by this
    // clip. What criterion 1 of the ticket asks about.
    bool covers_frame = false;
    // The whole displayed source is read and drawn over the whole frame
    // with nothing cut and nothing left over -- the case where a consumer
    // may keep the filter chain it had before this ticket.
    bool is_identity = false;
};

// THE RULE, as a rectangle. `sourceWidth` and `sourceHeight` are the
// *displayed* size of the source (step 1 above, already applied by the
// caller); `frameWidth` and `frameHeight` are the frame the picture lands
// in -- the sequence for the monitor and the export, the thumbnail grid for
// verify_export, which is the same frame at another size.
//
// Fails, with a message naming what, when the framing is invalid, when a
// dimension is not positive, or when the framing leaves nothing on screen.
bool ResolveClipFraming(const ClipFraming& framing, int64_t sourceWidth,
                        int64_t sourceHeight, int64_t frameWidth,
                        int64_t frameHeight, ClipFramingGeometry& geometry,
                        std::string& error);

// The sequence frame reduced to fit a square bound, aspect kept: the frame
// a sheet cell, a read_frame answer or a verification thumbnail composes
// in. Written once because three surfaces shrink the same frame, and a
// framing resolved against two different rectangles is two rectangles.
void BoundedFrameSize(int64_t frameWidth, int64_t frameHeight, int64_t bound,
                      int64_t& width, int64_t& height);

// VQA2 -- the same framing drawn into a reduced copy of the sequence frame
// (a sheet cell, a read_frame answer, a verification thumbnail): resolved at
// the sequence's own size, so the source region read is exactly the one the
// monitor and the export read, then only the rectangles drawn are scaled to
// `frameWidth` x `frameHeight`, each edge rounded once. Resolving at the
// reduced size instead rounds the edges there, and the region read moves:
// 80 source pixels wide on a sheet against the export's 82, for a quarter
// cropped off a 320-pixel rush filled into 1080x1920 (agentic QA of 23
// September 2026), a 13x enlargement where the difference shows.
bool ResolveClipFramingReduced(const ClipFraming& framing, int64_t sourceWidth,
                               int64_t sourceHeight, int64_t sequenceWidth,
                               int64_t sequenceHeight, int64_t frameWidth,
                               int64_t frameHeight,
                               ClipFramingGeometry& geometry,
                               std::string& error);

// The displayed size of the media a clip reads, rotation applied. Fails
// when the clip names no library media, when that media was never probed
// (a version-1 source), or when its display matrix is not a right angle --
// none of which an exact rectangle can be built on.
bool ClipDisplayedSize(const Document& document, const DocumentClip& clip,
                       int64_t& width, int64_t& height, std::string& error);

// The one call every consumer makes: the clip's rectangle in a frame of
// `frameWidth` x `frameHeight`. Pass the sequence's own size for the
// monitor, the export and the sheets; pass the thumbnail size for a
// verification that works on thumbnails.
bool ResolveClipFramingForClip(const Document& document,
                               const DocumentClip& clip, int64_t frameWidth,
                               int64_t frameHeight,
                               ClipFramingGeometry& geometry,
                               std::string& error);

// The geometry as the JSON the reading tool reports (criterion 3): the
// rectangle in the sequence, the source region it reads, and the uncovered
// edges. Written here so the CLI and the MCP tool cannot drift into
// describing the same rectangle differently.
std::string SerializeClipFramingGeometry(const ClipFramingGeometry& geometry);

// The FFmpeg fragment that reads `geometry.source_*` out of a decoded
// source, for the export, the sheets and the verification. Empty when the
// whole source is read, so a clip that crops nothing keeps the filter chain
// it had before this ticket.
std::string ClipFramingCropFilter(const ClipFramingGeometry& geometry,
                                  int64_t sourceWidth, int64_t sourceHeight);
