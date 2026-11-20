#pragma once

// CH5 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel (CH6) shows the
// pieces a charter dresses -- the logo, the name, the card -- in 9:16, 16:9
// and 1:1, with the charter applied as add_motion_template applies it
// (ApplyMotionKit). What it renders, and at which frame, is decided here,
// where it is tested; the panel only hands the request to the renderer the
// executable installs (MotionFrameRenderer.h), off the main thread.
//
// Three facts of the real pieces shape it. A piece names its formats
// itself ("9x16" in the shipped ones, anything in a library piece), so the
// format is found by its size, never assumed. The logo fades in and out
// and a hit test skips a node at zero opacity, so the preview is taken at
// the middle of the piece, where everything a charter places is shown. And
// a missing image makes the render fail (MediaMissing), so a logo file the
// charter names and this machine lacks is replaced by a placeholder, and
// the panel says so rather than show nothing.
//
// Portable: no AppKit, no I/O -- whether an image exists is asked of the
// caller (tests/charter_preview_tests.cc).

#include "Json.h"
#include "MotionComposition.h"
#include "MotionFrameRenderer.h"
#include "MotionTemplates.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ui::charter {

// One size the panel previews.
struct PreviewFrame {
    int32_t width = 0;
    int32_t height = 0;
    // The name a format of that size usually goes by: "9x16", "16x9",
    // "1x1". The name a caption format created for it takes; a piece's own
    // name for that size is PieceFormatFor's.
    std::string name;
    // As the panel shows it, in French typography: "9:16", "16:9", "1:1".
    std::string label;
};

// 1080x1920, 1920x1080 and 1080x1080, in that order.
const std::vector<PreviewFrame>& PreviewFrames();

// The name `definition` (a piece's composition) gives its format of
// exactly `width` x `height`; empty when it declares none of that size, and
// the piece then has no preview at that size.
std::string PieceFormatFor(const mcp_json::Value& definition, int32_t width,
                           int32_t height);

// The frame a piece is previewed, hit-tested and dragged at: half its
// `duration`, rounded down. The shipped pieces fade in and out at both
// ends; their middle shows everything at full opacity, a spring settled.
int64_t RestFrame(const mcp_json::Value& definition);

// One piece, ready to render at one size. It owns the composition, because
// a MotionFrameRequest only points at one (PieceFrameRequest).
struct PiecePreview {
    std::string piece;
    PreviewFrame frame;
    // The piece's own name for that size (PieceFormatFor).
    std::string format;
    // The piece's composition with the charter applied (ApplyMotionKit),
    // every format kept: the request names the one to render.
    mcp_json::Value definition;
    // `definition`, parsed.
    MotionComposition composition;
    // What the charter set, and named without the piece having it.
    MotionKitApplication application;
    // Prop overrides for the render: each image prop whose file is missing,
    // set to the placeholder. Null when there is none.
    mcp_json::Value props;
    // Those props, by name, with the path the charter (or the piece) gave:
    // what the panel says it replaced.
    std::vector<std::pair<std::string, std::string>> substituted;
    // RestFrame(definition).
    int64_t rest_frame = 0;
};

// `piece` (a catalog entry: shipped or library) dressed by `kit` for
// `frame`: ApplyMotionKit on its definition, the format of that size, the
// rest frame. Every text prop an image node's `src` is bound to (the
// logo's `logo`) whose value `imageExists` refuses -- a relative path,
// which only a project's package resolves, included, as `imageExists`
// judges it -- is overridden in `props` by `placeholderImagePath` and
// listed in `substituted`. Refused, with a sentence for the panel: the
// piece has no format of that size, the charter withdraws it or does not
// fit it (ApplyMotionKit's reason), or the motion parser refuses the
// result.
bool BuildPieceRequest(
    const MotionKitInfo& kit, const MotionTemplateInfo& piece,
    const PreviewFrame& frame, const std::string& placeholderImagePath,
    const std::function<bool(const std::string&)>& imageExists,
    PiecePreview& out, std::string& error);

// The request for the renderer: `preview.composition`, its `format`, its
// `props`, its `rest_frame`, at `scale` of the frame's size (a preview is
// drawn smaller than 1080 pixels). Build it right before rendering: it
// points into `preview`, which must outlive the render and not move. The
// caller adds `font_directories` and, when it has one, `media_directory`.
// The evaluated frame the renderer returns is in pixels of that scale:
// AnchorQuad (CharterCanvas.h) takes the same `scale` back out.
MotionFrameRequest PieceFrameRequest(const PiecePreview& preview, double scale);

// The value of every prop of `preview` as the frame renders it, by name:
// the declared default (the charter's, once ApplyMotionKit set it), then
// the value of the preview's format, then `preview.props` (a placeholder).
// What a drag is judged against (DragContext::values, CharterCanvas.h).
mcp_json::Value PieceValues(const PiecePreview& preview);

}  // namespace ui::charter
