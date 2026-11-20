// CH6 -- docs/roadmap/CHARTE-2026-11.md. The round trip of a gesture on the
// Charte panel, wave 2: the outline drawn while the mouse is down
// (DragOutline), the edits written at the release (DragEdits), the charter
// they make (EditMotionKit, then ParseMotionKit of its written form), and
// the piece dressed by that charter and evaluated again (BuildPieceRequest,
// the evaluation the executable's renderer runs, with a stand-in measurer)
// -- the anchor lands where the outline said, in the format dragged, and
// the other formats keep their place (DEC62). A subtitle dragged writes the
// base height without caption formats and the format's with them, both
// accepted by EditMotionKit and read back by MotionKitCaptionStyle.
// Portable: no framework, no I/O, a neutral charter.

#include "CharterCanvas.h"
#include "CharterPanelModel.h"
#include "CharterPreview.h"

#include "Json.h"
#include "MotionCanvas.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionKitEdit.h"
#include "MotionTemplates.h"
#include "TestSupport.h"
#include "motion_fake_measurer.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using mcp_json::Value;
using ui::charter::AreaKind;
using ui::charter::Drag;
using ui::charter::DragContext;
using ui::charter::DragKind;
using ui::charter::HitArea;
using ui::charter::PiecePreview;
using ui::charter::PreviewFrame;
using ui::charter::PreviewFrames;
using ui::charter::Quad;

// The preview is drawn at a quarter of the frame, as a panel of about 270
// points shows a 9:16 frame on a Retina screen.
constexpr double kScale = 0.25;
// A whole step of a % prop is 0.5 % of 1080 or 1920 pixels; the layout runs
// in single precision. What the outline says and what the charter then
// draws agree to a hundredth of a pixel.
constexpr double kTolerance = 0.01;

const char kKitText[] =
    R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
    R"("props":{"accent":"#1D6FA3","police":"Exemple Sans"},)"
    R"("captions":{"font_family":"Exemple Sans","font_size":56,)"
    R"("color":"#FFFFFF","vertical_position":{"num":3,"den":4}}})";

Value Json(const std::string& text) {
    Value value;
    std::string error;
    Check(Value::Parse(text, value, error), "test JSON parses: " + error);
    return value;
}

const MotionTemplateInfo* Find(const std::vector<MotionTemplateInfo>& pieces,
                               const std::string& name) {
    for (const MotionTemplateInfo& piece : pieces) {
        if (piece.name == name) {
            return &piece;
        }
    }
    return nullptr;
}

// The charter as the panel holds it after a read: the tree of its file
// and what ParseMotionKit makes of it.
struct Charter {
    Value file;
    MotionKitInfo kit;
};

bool Read(const Value& file, Charter& charter) {
    charter.file = file;
    std::string error;
    const bool ok =
        ParseMotionKit("marque", DumpMotionKit(file), charter.kit, error);
    Check(ok, "the charter reads: " + error);
    return ok;
}

// The anchor of `piece` dressed by `kit` in frame `frameIndex`, evaluated
// as the executable's renderer evaluates a PieceFrameRequest.
struct Shown {
    PiecePreview preview;
    HitArea area;
    bool ok = false;
};

void Show(const MotionKitInfo& kit, const MotionTemplateInfo& piece,
          size_t frameIndex, Shown& shown) {
    std::string error;
    shown.ok = ui::charter::BuildPieceRequest(
        kit, piece, PreviewFrames()[frameIndex], "logo.png",
        [](const std::string& path) { return path == "logo.png"; },
        shown.preview, error);
    Check(shown.ok, piece.name + " builds its request: " + error);
    if (!shown.ok) {
        return;
    }
    const MotionFrameRequest request =
        ui::charter::PieceFrameRequest(shown.preview, kScale);
    MotionComposition resolved;
    Value props;
    MotionError motionError;
    FakeMotionMeasurer measurer;
    MotionFrame frame;
    shown.ok = ResolveMotionVariant(*request.composition, request.variant,
                                    request.format, request.props, resolved,
                                    props, motionError) &&
               EvaluateMotionFrame(resolved, props, request.frame, measurer,
                                   frame, motionError);
    Check(shown.ok, "the request evaluates: " + motionError.code + " -- " +
                        motionError.message);
    if (!shown.ok) {
        return;
    }
    ScaleMotionFrame(frame, request.scale);
    shown.area.kind = AreaKind::Piece;
    shown.area.piece = piece.name;
    shown.area.format = shown.preview.format;
    shown.ok =
        ui::charter::AnchorQuad(frame, piece.name, kScale, shown.area.quad);
    Check(shown.ok, piece.name + " has its anchor at the rest frame");
}

bool SameQuad(const Quad& left, const Quad& right) {
    for (size_t corner = 0; corner < left.size(); ++corner) {
        if (std::fabs(left[corner].first - right[corner].first) > kTolerance ||
            std::fabs(left[corner].second - right[corner].second) >
                kTolerance) {
            return false;
        }
    }
    return true;
}

std::string Describe(const Quad& quad) {
    std::string text;
    for (const auto& [x, y] : quad) {
        text += "(" + std::to_string(x) + ", " + std::to_string(y) + ") ";
    }
    return text;
}

std::string Describe(const std::vector<MotionKitEdit>& edits) {
    std::string text;
    for (const MotionKitEdit& edit : edits) {
        text +=
            edit.path + "=" + (edit.value ? edit.value->Dump() : "<rm>") + " ";
    }
    return text;
}

// A gesture, before it knows where the anchor is: a move by (dx, dy), or a
// resize by handle `handle` (MotionSelectionHandles' order) by (dx, dy).
struct Gesture {
    DragKind kind = DragKind::Move;
    int handle = -1;
    double dx = 0.0;
    double dy = 0.0;
};

Gesture Move(double dx, double dy) { return {DragKind::Move, -1, dx, dy}; }

Gesture Resize(int handle, double dx, double dy) {
    return {DragKind::Resize, handle, dx, dy};
}

Drag MoveOf(const HitArea& area, double dx, double dy) {
    Drag drag;
    drag.area = area;
    drag.kind = DragKind::Move;
    const double x = (area.quad[0].first + area.quad[2].first) / 2.0;
    const double y = (area.quad[0].second + area.quad[2].second) / 2.0;
    drag.start = {x, y};
    drag.current = {x + dx, y + dy};
    return drag;
}

Drag ResizeOf(const HitArea& area, int handle, double dx, double dy) {
    Drag drag;
    drag.area = area;
    drag.kind = DragKind::Resize;
    drag.handle = handle;
    drag.start =
        MotionSelectionHandles(area.quad.data())[static_cast<size_t>(handle)];
    drag.current = {drag.start.first + dx, drag.start.second + dy};
    return drag;
}

// `drag` on the logo shown in frame `frameIndex` of `charter`: the edits
// all go to that format, the charter they make is written and read back,
// the logo it dresses lands on the outline in that frame, and every other
// frame keeps its logo where it was.
void CheckLogoGesture(const std::vector<MotionTemplateInfo>& shipped,
                      const Charter& charter, size_t frameIndex,
                      const Gesture& made, const std::string& label) {
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    Shown before;
    Show(charter.kit, *logo, frameIndex, before);
    if (!before.ok) {
        return;
    }
    const Drag gesture =
        made.kind == DragKind::Move
            ? MoveOf(before.area, made.dx, made.dy)
            : ResizeOf(before.area, made.handle, made.dx, made.dy);
    // Cmd held on the panel: no snapping, the steps and bounds alone.
    DragContext context =
        ui::charter::PieceDragContext(before.preview, charter.file, kScale);
    context.snap_distance = 0.0;
    const Quad outline = ui::charter::DragOutline(gesture, context);
    const std::vector<MotionKitEdit> edits =
        ui::charter::DragEdits(gesture, context);
    Check(!SameQuad(outline, before.area.quad),
          label + ": the outline has left the anchor: " + Describe(outline));
    const std::string prefix =
        "pieces.logo.formats." + before.preview.format + ".";
    bool perFormat = !edits.empty();
    for (const MotionKitEdit& edit : edits) {
        perFormat = perFormat &&
                    edit.path.compare(0, prefix.size(), prefix) == 0 &&
                    edit.value.has_value();
    }
    Check(perFormat, label + ": every edit is the dragged format's (" + prefix +
                         "*): " + Describe(edits));

    Value written;
    bool changed = false;
    MotionKitError error;
    Check(
        EditMotionKit("marque", charter.file, edits, written, changed, error) &&
            changed,
        label + ": EditMotionKit takes the edits: " + error.code + " " +
            error.path + " " + error.message);
    Charter after;
    if (!Read(written, after)) {
        return;
    }
    Shown landed;
    Show(after.kit, *logo, frameIndex, landed);
    Check(landed.ok && SameQuad(landed.area.quad, outline),
          label +
              ": the logo dressed by the written charter lands on the "
              "outline: drawn " +
              Describe(landed.area.quad) + "vs outline " + Describe(outline));
    for (size_t other = 0; other < PreviewFrames().size(); ++other) {
        if (other == frameIndex) {
            continue;
        }
        Shown was;
        Shown now;
        Show(charter.kit, *logo, other, was);
        Show(after.kit, *logo, other, now);
        Check(was.ok && now.ok && SameQuad(was.area.quad, now.area.quad),
              label + ": the logo in " + PreviewFrames()[other].label +
                  " keeps its place: " + Describe(was.area.quad) + "vs " +
                  Describe(now.area.quad));
    }
}

void CheckLogoRoundTrip(const std::vector<MotionTemplateInfo>& shipped) {
    Charter charter;
    if (!Read(Json(kKitText), charter)) {
        return;
    }
    // Each format, moved: left and down in 9:16 (its own margins), right
    // and up in 16:9 and 1:1 (the piece's defaults until now).
    CheckLogoGesture(shipped, charter, 0, Move(-100, 96),
                     "9:16 moved left and down");
    CheckLogoGesture(shipped, charter, 1, Move(-240, 120),
                     "16:9 moved left and down");
    CheckLogoGesture(shipped, charter, 2, Move(-60, 300),
                     "1:1 moved across to the bottom");
    // The corner turned over: dragged to the bottom left quadrant of 9:16.
    CheckLogoGesture(shipped, charter, 0, Move(-700, 1300),
                     "9:16 moved to the bottom left corner");
    // Resized by its bottom right corner, and by its left edge.
    CheckLogoGesture(shipped, charter, 0, Resize(2, 80, 0),
                     "9:16 resized by its bottom right corner");
    CheckLogoGesture(shipped, charter, 1, Resize(7, -120, 0),
                     "16:9 resized by its left edge");

    // A charter that already set the 9:16 place keeps everything else of
    // its file: the gesture changes the values it moved, one line each.
    Charter placed;
    if (!Read(Json(R"({"format":"brand-kit.v1","description":"Charte.",)"
                   R"("props":{"police":"Exemple Sans"},)"
                   R"("pieces":{"logo":{"logo":"logo.png","formats":)"
                   R"({"9x16":{"marge_haut":20.50}}}}})"),
              placed)) {
        return;
    }
    CheckLogoGesture(shipped, placed, 0, Move(-100, 0),
                     "9:16 with a place already set");
    Shown before;
    Show(placed.kit, *Find(shipped, "logo"), 0, before);
    DragContext context =
        ui::charter::PieceDragContext(before.preview, placed.file, kScale);
    context.snap_distance = 0.0;
    Drag horizontal = MoveOf(before.area, -100, 0);
    Value written;
    bool changed = false;
    MotionKitError error;
    EditMotionKit("marque", placed.file,
                  ui::charter::DragEdits(horizontal, context), written, changed,
                  error);
    const Value* top =
        FindMotionKitPath(written, "pieces.logo.formats.9x16.marge_haut");
    Check(top != nullptr && top->Dump() == "20.50",
          "a horizontal drag leaves the top margin's literal as written: " +
              (top != nullptr ? top->Dump() : std::string("<none>")));
}

// The subtitle: dragged without caption formats, the base height; with
// them, the 9:16 format's. Each edit is written by EditMotionKit and read
// back as the caption style of each size.
void CheckCaptionRoundTrip() {
    Charter charter;
    if (!Read(Json(kKitText), charter)) {
        return;
    }
    const PreviewFrame& vertical = PreviewFrames()[0];
    const PreviewFrame& wide = PreviewFrames()[1];
    const auto drag = [](const PreviewFrame& frame, const std::string& format,
                         double dy) {
        Drag gesture;
        gesture.area.kind = AreaKind::Captions;
        gesture.area.format = format;
        // A block of 600 x 80 pixels centred at 75 % of the height.
        const double centre = frame.height * 0.75;
        gesture.area.quad = {{{240, centre - 40},
                              {840, centre - 40},
                              {840, centre + 40},
                              {240, centre + 40}}};
        gesture.kind = DragKind::Move;
        gesture.start = {540, centre};
        gesture.current = {540, centre + dy};
        return gesture;
    };
    const auto contextFor = [](const Charter& on, const PreviewFrame& frame) {
        DragContext context;
        context.frame_width = frame.width;
        context.frame_height = frame.height;
        context.kit = on.file;
        Check(
            ui::charter::CaptionDragContextFor(on.kit, frame, context.captions),
            "the charter gives a subtitle in " + frame.label);
        return context;
    };
    const auto heightOf = [](const MotionKitInfo& kit,
                             const PreviewFrame& frame) {
        CaptionStyle style;
        std::string format;
        std::vector<std::string> set;
        std::string error;
        if (!MotionKitCaptionStyle(kit, frame.width, frame.height, style,
                                   format, set, error)) {
            return std::string("<refused: ") + error + ">";
        }
        return std::to_string(style.vertical_position.num) + "/" +
               std::to_string(style.vertical_position.den);
    };

    // Base: up by 15 % of 1920 pixels, 75 % to 60 %, every size at once.
    DragContext context = contextFor(charter, vertical);
    std::vector<MotionKitEdit> edits =
        ui::charter::DragEdits(drag(vertical, "", -288), context);
    Check(edits.size() == 1 && edits[0].path == "captions.vertical_position",
          "without formats, the base height: " + Describe(edits));
    Value written;
    bool changed = false;
    MotionKitError error;
    Check(
        EditMotionKit("marque", charter.file, edits, written, changed, error) &&
            changed,
        "EditMotionKit takes the base height: " + error.message);
    Charter base;
    if (Read(written, base)) {
        Check(
            heightOf(base.kit, vertical) == "3/5" &&
                heightOf(base.kit, wide) == "3/5",
            "the base height is every size's: " + heightOf(base.kit, vertical) +
                ", " + heightOf(base.kit, wide));
    }

    // Per format: the panel's action first, then the drag in 9:16.
    const std::vector<MotionKitEdit> formats = ui::charter::WithBrandFormat(
        charter.file,
        ui::charter::CaptionFormatEdits(charter.file, PreviewFrames()));
    Check(formats.size() == 3,
          "one caption format per preview size: " + Describe(formats));
    Check(EditMotionKit("marque", charter.file, formats, written, changed,
                        error) &&
              changed,
          "EditMotionKit takes the formats: " + error.message);
    Charter sized;
    if (!Read(written, sized)) {
        return;
    }
    Check(heightOf(sized.kit, vertical) == "3/4" &&
              heightOf(sized.kit, wide) == "3/4",
          "the formats change nothing yet: " + heightOf(sized.kit, vertical));
    context = contextFor(sized, vertical);
    Check(context.captions.has_formats && context.captions.format == "9x16",
          "the 9:16 drag knows its format: " + context.captions.format);
    edits = ui::charter::DragEdits(drag(vertical, "9x16", -288), context);
    Check(edits.size() == 1 &&
              edits[0].path == "captions.formats.9x16.vertical_position",
          "with formats, the format of the size dragged: " + Describe(edits));
    Check(EditMotionKit("marque", sized.file, edits, written, changed, error) &&
              changed,
          "EditMotionKit takes the per-format height: " + error.message);
    Charter perFormat;
    if (Read(written, perFormat)) {
        Check(heightOf(perFormat.kit, vertical) == "3/5" &&
                  heightOf(perFormat.kit, wide) == "3/4",
              "9:16 moves, 16:9 keeps the base: " +
                  heightOf(perFormat.kit, vertical) + ", " +
                  heightOf(perFormat.kit, wide));
    }
}

}  // namespace

int main() {
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    if (Find(shipped, "logo") == nullptr) {
        Check(false, "logo is shipped");
        return 1;
    }
    CheckLogoRoundTrip(shipped);
    CheckCaptionRoundTrip();
    if (failures == 0) std::cout << "charter round trip tests passed\n";
    return failures == 0 ? 0 : 1;
}
