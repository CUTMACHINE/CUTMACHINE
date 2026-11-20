#pragma once

// CH5 -- docs/roadmap/CHARTE-2026-11.md. Dragging the logo, the name or the
// subtitle on the Charte panel's preview edits the charter, in the format
// shown only (DEC62): moving the logo in 9:16 writes
// `pieces.logo.formats.9x16.*`, and 16:9 keeps its own place. This is the
// policy of that gesture: what a click grabs, the outline drawn while the
// mouse is down, and the edits written when it is released (DEC61: one
// version per gesture, none during it).
//
// It is not REM5 (MotionCanvas.h). The visual editor of a composition
// writes CSS into a clip's definition (translateX, width) and would
// overwrite the bindings of a shipped piece ({"prop": "largeur"}); here a
// gesture becomes values of the props the piece already declares --
// margins, a width, a corner -- the only words a charter has for where a
// piece sits. As in REM5, the clickable box is the evaluated frame's, the
// one the renderer drew (constraint 6: no second clickable geometry).
//
// Portable: no AppKit, no I/O (tests/charter_canvas_tests.cc).

#include "CharterPreview.h"
#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionKitEdit.h"
#include "MotionSafeArea.h"
#include "MotionTemplates.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ui::charter {

// A point in composition pixels, rows from the top: x, y.
using Point = std::pair<double, double>;
// A box: top-left, top-right, bottom-right, bottom-left, as MotionHit
// spells one (a plain array, so that MotionNodeQuad can fill .data()).
using Quad = std::array<Point, 4>;

// What a hit area is: one piece's anchor, or the subtitle (a caption style,
// not a piece: its box comes from the caption layout the panel draws by,
// caption_layout::Place, not from a motion frame).
enum class AreaKind { Piece, Captions };

struct HitArea {
    AreaKind kind = AreaKind::Piece;
    // The piece; empty for the subtitle.
    std::string piece;
    // The piece's own name for the format shown (PieceFormatFor); for the
    // subtitle, the charter's caption format of that size, empty when it
    // declares none.
    std::string format;
    // Composition pixels of the frame shown.
    Quad quad{};
};

// The author id of the node a piece is dragged by: "logo" for the piece
// `logo` (its image), "bloc" for `nom` (the box of both lines). Empty for
// a piece that is not dragged (the card fills the frame).
std::string AnchorId(const std::string& piece);

// Where the anchor of `piece` is drawn in `evaluated` (the frame the
// renderer returned for PieceFrameRequest at `scale`), in composition
// pixels: the node found by its author id, through the transforms of its
// ancestors and its own as the painter applies them (MotionGeometry.h),
// then divided by `scale`. By id, not by ULID: a piece's composition
// carries no identities. False when the piece has no anchor, or the node
// is not shown (zero opacity included).
bool AnchorQuad(const MotionFrame& evaluated, const std::string& piece,
                double scale, Quad& quad);

// The area under (x, y), composition pixels: the last of `areas` that
// contains it, as the later one is drawn over the earlier; null for none.
const HitArea* HitTest(const std::vector<HitArea>& areas, double x, double y);

enum class DragKind { Move, Resize };

// One gesture, mouse down to now.
struct Drag {
    // What was grabbed, its quad as it was at mouse down.
    HitArea area;
    DragKind kind = DragKind::Move;
    // Resize: the handle grabbed, in MotionSelectionHandles' order
    // (corners from the top-left, then the edges' middles from the top).
    int handle = -1;
    Point start{};
    Point current{};
    // The Option key: a finer step.
    bool fine = false;
};

// The subtitle's side of a drag.
struct CaptionDragContext {
    // The charter declares `captions.formats`.
    bool has_formats = false;
    // Its caption format of the frame's size; empty when it declares none.
    std::string format;
    // Where the block's centre is now, a whole percentage of the height
    // (CaptionHeightPercent of MediaPanelModel.h).
    int32_t percent = 0;
};

// The step of a % prop, and with the Option key.
inline constexpr double kPercentStep = 0.5;
inline constexpr double kFinePercentStep = 0.1;
// How near, in points of the preview, an edge snaps to a line.
inline constexpr double kSnapPoints = 8.0;

// What a drag is judged against, for the frame and the piece grabbed.
struct DragContext {
    int32_t frame_width = 0;
    int32_t frame_height = 0;
    // TimelineMotionSafeZone(frame_width, frame_height): the zone a film of
    // that size is checked against, social_vertical in 9:16.
    MotionSafeZone safe_zone;
    // kSnapPoints in composition pixels at the preview's scale.
    double snap_distance = 0.0;
    // The piece's prop declarations (PiecePreview::composition.props):
    // type, unit, min, max.
    std::vector<MotionPropDefinition> props;
    // The values in effect in that format, by prop name: the piece's
    // defaults, its format's values, the charter's, as rendered.
    mcp_json::Value values;
    // The charter file as read (a JSON tree, MotionKitEdit.h).
    mcp_json::Value kit;
    CaptionDragContext captions;
};

// The box where the grabbed thing would land if released now, snapped and
// stepped as DragEdits will write it. No edit: what the panel draws while
// the mouse is down.
Quad DragOutline(const Drag& drag, const DragContext& context);

// The edits a release writes; empty when nothing would change (every value
// equal to `context.values`, or the drag too short to cross a step). The
// rules:
//
// - Every piece edit goes to `pieces.<p>.formats.<format>.<prop>`
//   (MotionKitPiecePath with the area's format): the frame shown only.
//   A motion-kit.v1 charter gets `format: "brand-kit.v1"` first, as
//   `pieces` is a field of a brand's charter. Only props whose value
//   changes are written.
// - A % prop moves by kPercentStep (kFinePercentStep with `fine`) and is
//   clamped to its declared min and max. The logo's marge_gauche,
//   marge_droite and largeur are % of the frame's width, marge_haut and
//   marge_bas % of its height (CSS percentages, as the piece binds them).
// - An edge of the box within `snap_distance` of an edge of the safe zone,
//   or of the frame's centre line on that axis, goes onto it (the nearest
//   line per axis); the value that places it is then rounded toward the
//   inside (up, for the margin on that edge's side), so a snapped edge
//   never ends past the line it snapped to -- the inside of the zone, or
//   for the centre line the side the box lies on. Other margins round to
//   the nearest step. An axis the pointer did not move neither snaps nor
//   writes: a horizontal drag writes no vertical margin, even one off the
//   step, and does not pull the box onto a line it started near.
// - The logo, moved: on release `coin` becomes the quadrant of the box's
//   centre ("haut-gauche" when it is left of and above the middle...), and
//   the margins of that corner are computed from the box: marge_gauche or
//   marge_droite from the left or right edge, marge_haut or marge_bas from
//   the top or bottom edge. The margins of the other sides are left as
//   they are. Resized by a corner or the left or right handle: `largeur`
//   is the new width (the image keeps its aspect), the opposite corner
//   holds (for the left or right handle, the opposite edge and the side
//   of the corner, top or bottom), only the moving edge snaps, and the
//   margins follow as for a move; the top and bottom handles change
//   nothing.
// - The name, moved: `marge` from the box's left edge (the block spans
//   from `marge` to `marge` short of the right edge, so its right edge
//   follows, and only the left edge snaps across), `hauteur` from its
//   bottom edge. Not resized.
// - The subtitle moves vertically only, and does not snap: its block's
//   centre, as a whole percentage of the height from 1 to 99 (no finer
//   step), written as a reduced fraction {"num", "den"} (75 is 3/4, as
//   WithCaptionPlacement of MediaPanelModel.h writes it). Without
//   `captions.formats` in the charter it edits
//   `captions.vertical_position`, every size at once: a first caption
//   format would restrict the other sizes (MotionKitCaptionStyle refuses a
//   size no format has), so only an explicit action creates formats
//   (CaptionFormatEdits, CharterPanelModel.h). With formats, it edits that
//   size's `captions.formats.<f>.vertical_position`; none of that size,
//   nothing.
std::vector<MotionKitEdit> DragEdits(const Drag& drag,
                                     const DragContext& context);

// The context of a drag on a piece of `preview`, shown at `pointsPerPixel`
// view points per composition pixel: the frame's size and safe zone, the
// snap distance, the piece's props and their values (PieceValues) and the
// charter file `kitFile`. `captions` is left empty: a piece is not the
// subtitle.
DragContext PieceDragContext(const PiecePreview& preview,
                             const mcp_json::Value& kitFile,
                             double pointsPerPixel);

// The subtitle's side of a drag on `frame`: whether `kit` declares caption
// formats, its format of that size, and the block's height as the
// Inspector reads it (CaptionHeightPercent). False when the charter gives
// no caption style at that size (no captions, or formats and none of that
// size): there is then no subtitle to drag there.
bool CaptionDragContextFor(const MotionKitInfo& kit, const PreviewFrame& frame,
                           CaptionDragContext& out);

}  // namespace ui::charter
