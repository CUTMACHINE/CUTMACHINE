#pragma once

// MA2 -- docs/roadmap/MOTION-AFTER-2026-09.md. An After Effects project the
// bridge built from a composition (export_motion_after), checked against the
// engine frame by frame instead of by eye. Until now every .aep delivered was
// checked by a script written again each time: frames rendered by After,
// frames rendered by the engine, the mean luma gap of each, and the frames
// over the line explained by hand ("the three others are slice() glitches").
//
// Here the explanation is the exporter's own: its report says what After
// gets instead (MotionAfterExportReport::notes), and since MA2 which node
// each note is about (MotionAfterNoteScope). A frame over the tolerance is
// "announced" when a note about a node drawn at that frame names it, and
// "unexplained" otherwise -- what the check is for: a defect of the export
// (MA13 was one) stays visible instead of being drowned in the
// approximations everybody already knows. A note about the whole
// composition (the motion blur of a fade, the grain) cannot say which frames
// it explains; it is listed beside the unexplained frames, never counted as
// explaining one.
//
// The measure is the one used by hand since 29 September, so that
// its tolerance (0.5) and every number measured since stay comparable: the
// mean, over every pixel, of the absolute difference of luma as Pillow's
// convert("L") gives it (BT.601 weights in 16-bit fixed point, rounded to a
// level), each picture over black, with the largest difference and the share
// of pixels more than 8 levels apart beside it. Rounding each luma to a level
// matters at the edge: on the teaser v7, frame 512 (a grid of one-pixel
// strokes, After and Core Graphics anti-aliasing them differently) is at
// 0.48 so measured, at 0.501 with unrounded BT.709 luma. Both pictures are
// PNG files kept in the caller's directory, to be looked at.
//
// Portable: FFmpeg's PNG decoder, which the model links. The engine frames
// come from the renderer the executable installs (MotionFrameRenderer.h),
// After's from a callback: the bridge's render_frame in the tool, a fake in
// the tests.

#include "Json.h"
#include "MotionAfterExport.h"
#include "MotionEvaluate.h"
#include "MotionFrameRenderer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The luma of a PNG, over black: one whole level per pixel, 0 to 255, rows
// from the top. 8 or 16 bits, RGB or RGBA (straight alpha, as PNG stores
// it). Refused with a message for anything else.
bool MotionAfterPngLuma(const std::string& png, int32_t& width, int32_t& height,
                        std::vector<float>& luma, std::string& error);

struct MotionAfterFrameGap {
    // Mean absolute difference of luma over the frame, in levels.
    double mean = 0.0;
    // The largest difference of one pixel.
    double max = 0.0;
    // The share of pixels more than 8 levels apart, 0 to 1.
    double over = 0.0;
};

// Two lumas of the same size compared.
MotionAfterFrameGap MotionAfterCompareLuma(const std::vector<float>& engine,
                                           const std::vector<float>& after);

// The notes of `report` that show in `frame`: those about a node drawn
// there (in the tree, and not hidden by its opacity or an ancestor's), a
// slice() only while it cuts, at the frame or at one of its shutter
// instants. Indices into report.notes, in order, each once.
std::vector<size_t> MotionAfterNotesShown(const MotionAfterExportReport& report,
                                          const MotionFrame& frame);

enum class MotionAfterVerdict : uint8_t {
    Within,       // at or under the tolerance
    Announced,    // over it, and a note about a node drawn there names it
    Unexplained,  // over it, and nothing the report says about a node does
};

struct MotionAfterFrameCheck {
    int64_t frame = 0;
    MotionAfterFrameGap gap;
    MotionAfterVerdict verdict = MotionAfterVerdict::Within;
    // The notes shown at that frame (MotionAfterNotesShown).
    std::vector<size_t> notes;
    std::string engine_png;
    std::string after_png;
};

// Writes frame `frame` of the After composition to `path`, a PNG that does
// not exist yet; on a refusal, `code` and `message` say why.
using MotionAfterFrameSource =
    std::function<bool(int64_t frame, const std::string& path,
                       std::string& code, std::string& message)>;

struct MotionAfterVerifyRequest {
    // The composition, its props, fonts, reach and media as the export had
    // them; `frame` is set for each frame checked.
    MotionFrameRequest engine;
    // The exporter's report for the same composition and options.
    MotionAfterExportReport report;
    // Frames of the composition, each within it, in the order checked.
    std::vector<int64_t> frames;
    // In levels of mean luma difference.
    double tolerance = 0.5;
    // An existing directory: each frame's two pictures go there,
    // moteur-NNNNNN.png and after-NNNNNN.png, never over a file.
    std::string directory;
};

// Renders every frame of `request` on both sides and compares them. False,
// with `error`, when a frame cannot be rendered on either side, when a file
// is already there, or when After's picture is not the composition's size.
bool VerifyMotionAfterFrames(const MotionAfterVerifyRequest& request,
                             MotionFrameRenderer render,
                             const MotionAfterFrameSource& after,
                             std::vector<MotionAfterFrameCheck>& checks,
                             MotionError& error);

// The result verify_after_export gives: counts by verdict, the median and
// the worst gap, every frame, the notes the announced frames name, and the
// notes about the whole composition.
void MotionAfterVerifyFields(const MotionAfterVerifyRequest& request,
                             const std::vector<MotionAfterFrameCheck>& checks,
                             mcp_json::Value& result);
