// CH5 -- docs/roadmap/CHARTE-2026-11.md. A drag on the Charte panel's
// preview, as edits of the charter in the format shown only (DEC62): the
// anchors found in the frames the shipped pieces evaluate to, the steps,
// the bounds, the snapping to the safe zone and the centre lines, the
// logo's corner turned over, the resize, the name, the subtitle at its
// base or per format, and no edit for no change. The edits are checked by
// their paths and values, and laid into the charter by a local helper:
// writing them is EditMotionKit's (CH1), the round trip wave 2's.
// Portable: no framework, fake metrics, a neutral charter.

#include "CharterCanvas.h"
#include "CharterPreview.h"

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionKitEdit.h"
#include "MotionSafeArea.h"
#include "MotionTemplates.h"
#include "TestSupport.h"
#include "motion_fake_measurer.h"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Layout runs in single precision (Yoga): a thousandth of a pixel.
bool Near(double left, double right, double tolerance = 1e-3) {
    return std::fabs(left - right) <= tolerance;
}

using mcp_json::Value;
using ui::charter::AreaKind;
using ui::charter::Drag;
using ui::charter::DragContext;
using ui::charter::DragKind;
using ui::charter::HitArea;
using ui::charter::PiecePreview;
using ui::charter::PreviewFrames;
using ui::charter::Quad;

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

const char kKitText[] =
    R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
    R"("props":{"accent":"#1D6FA3","police":"Exemple Sans"}})";

MotionKitInfo Kit() {
    MotionKitInfo kit;
    std::string error;
    Check(ParseMotionKit("marque", kKitText, kit, error),
          "the test charter reads: " + error);
    return kit;
}

// The request evaluated as the executable's renderer evaluates it.
bool Evaluate(const MotionFrameRequest& request, MotionFrame& frame) {
    MotionComposition resolved;
    Value props;
    MotionError error;
    FakeMotionMeasurer measurer;
    const bool ok = ResolveMotionVariant(*request.composition, request.variant,
                                         request.format, request.props,
                                         resolved, props, error) &&
                    EvaluateMotionFrame(resolved, props, request.frame,
                                        measurer, frame, error);
    Check(ok, "the request evaluates: " + error.code + " -- " + error.message);
    if (ok) {
        ScaleMotionFrame(frame, request.scale);
    }
    return ok;
}

// A piece of the catalog previewed at frame `frameIndex`, rendered at
// `scale`, and the area of its anchor.
struct Shown {
    PiecePreview preview;
    MotionFrame frame;
    HitArea area;
    bool ok = false;
};

void Show(const MotionTemplateInfo& piece, size_t frameIndex, double scale,
          Shown& shown) {
    std::string error;
    shown.ok = ui::charter::BuildPieceRequest(
        Kit(), piece, PreviewFrames()[frameIndex], "logo.png",
        [](const std::string& path) { return path == "logo.png"; },
        shown.preview, error);
    Check(shown.ok, piece.name + " builds its request: " + error);
    if (!shown.ok) {
        return;
    }
    shown.ok = Evaluate(ui::charter::PieceFrameRequest(shown.preview, scale),
                        shown.frame);
    if (!shown.ok) {
        return;
    }
    shown.area.kind = AreaKind::Piece;
    shown.area.piece = piece.name;
    shown.area.format = shown.preview.format;
    shown.ok = ui::charter::AnchorQuad(shown.frame, piece.name, scale,
                                       shown.area.quad);
    Check(shown.ok, piece.name + " has its anchor at the rest frame");
}

bool QuadIs(const Quad& quad, double left, double top, double right,
            double bottom, double tolerance = 1e-3) {
    return Near(quad[0].first, left, tolerance) &&
           Near(quad[0].second, top, tolerance) &&
           Near(quad[1].first, right, tolerance) &&
           Near(quad[1].second, top, tolerance) &&
           Near(quad[2].first, right, tolerance) &&
           Near(quad[2].second, bottom, tolerance) &&
           Near(quad[3].first, left, tolerance) &&
           Near(quad[3].second, bottom, tolerance);
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
    return text.empty() ? "<none>" : text;
}

// `edits` are exactly these paths and values, in this order.
bool EditsAre(const std::vector<MotionKitEdit>& edits,
              const std::vector<std::pair<std::string, std::string>>& want) {
    if (edits.size() != want.size()) {
        return false;
    }
    for (size_t index = 0; index < edits.size(); ++index) {
        if (edits[index].path != want[index].first || !edits[index].value ||
            edits[index].value->Dump() != want[index].second ||
            edits[index].insert) {
            return false;
        }
    }
    return true;
}

// `file` with each edit's value set at its path, parents created: the
// shape EditMotionKit gives the file, without its judgement.
Value Laid(Value file, const std::vector<MotionKitEdit>& edits) {
    const auto lay = [](auto& self, Value& at, const std::string& path,
                        const Value& value) -> void {
        const size_t dot = path.find('.');
        const std::string key = path.substr(0, dot);
        if (dot == std::string::npos) {
            at.Set(key, value);
            return;
        }
        const Value* existing = at.Find(key);
        Value child = existing != nullptr && existing->IsObject()
                          ? *existing
                          : Value::MakeObject();
        self(self, child, path.substr(dot + 1), value);
        at.Set(key, std::move(child));
    };
    for (const MotionKitEdit& edit : edits) {
        if (edit.value) {
            lay(lay, file, edit.path, *edit.value);
        }
    }
    return file;
}

Drag MoveOf(const HitArea& area, double dx, double dy, bool fine = false) {
    Drag drag;
    drag.area = area;
    drag.kind = DragKind::Move;
    const double x = (area.quad[0].first + area.quad[2].first) / 2.0;
    const double y = (area.quad[0].second + area.quad[2].second) / 2.0;
    drag.start = {x, y};
    drag.current = {x + dx, y + dy};
    drag.fine = fine;
    return drag;
}

void CheckAnchors(const std::vector<MotionTemplateInfo>& shipped) {
    Check(ui::charter::AnchorId("logo") == "logo" &&
              ui::charter::AnchorId("nom") == "bloc" &&
              ui::charter::AnchorId("carton").empty() &&
              ui::charter::AnchorId("bandeau").empty(),
          "the logo is dragged by its image, the name by its block, the "
          "card not at all");
    // The ids are the shipped pieces' own.
    for (const char* name : {"logo", "nom"}) {
        const MotionTemplateInfo* piece = Find(shipped, name);
        Check(piece != nullptr &&
                  piece->definition.Dump().find(
                      "\"id\":\"" + ui::charter::AnchorId(name) + "\"") !=
                      std::string::npos,
              std::string(name) + " carries the anchor's author id");
        if (piece == nullptr) {
            continue;
        }
        for (size_t frame = 0; frame < PreviewFrames().size(); ++frame) {
            Shown shown;
            Show(*piece, frame, 0.5, shown);
            Check(shown.ok, std::string(name) + " is anchored in " +
                                PreviewFrames()[frame].label);
        }
    }

    // The logo in 9:16: 18 % of 1 080 wide, its image 2:1, in the top right
    // corner, 14 % of the width from the right and 15 % of the height from
    // the top -- at any scale of the render.
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    for (const double scale : {1.0, 0.5, 0.3}) {
        Shown shown;
        Show(*logo, 0, scale, shown);
        Check(shown.ok && QuadIs(shown.area.quad, 734.4, 288.0, 928.8, 385.2),
              "the 9:16 logo's anchor, in composition pixels at scale " +
                  std::to_string(scale) + ": " + Describe(shown.area.quad));
    }
    {
        Shown shown;
        Show(*logo, 1, 0.5, shown);
        Check(shown.ok && QuadIs(shown.area.quad, 1612.8, 64.8, 1804.8, 160.8),
              "the 16:9 logo: 10 % wide, 6 % margins: " +
                  Describe(shown.area.quad));
    }

    // Not shown: no anchor.
    Shown faded;
    Show(*logo, 0, 1.0, faded);
    MotionFrameRequest first =
        ui::charter::PieceFrameRequest(faded.preview, 1.0);
    first.frame = 0;
    MotionFrame frame;
    Quad quad{};
    Check(Evaluate(first, frame) &&
              !ui::charter::AnchorQuad(frame, "logo", 1.0, quad),
          "at zero opacity the logo has no anchor");
    Check(!ui::charter::AnchorQuad(faded.frame, "carton", 1.0, quad),
          "a piece without an anchor has none");
}

void CheckHitTest() {
    HitArea under;
    under.piece = "nom";
    under.quad = {{{100, 100}, {500, 100}, {500, 300}, {100, 300}}};
    HitArea over;
    over.piece = "logo";
    over.quad = {{{400, 200}, {600, 200}, {600, 400}, {400, 400}}};
    HitArea flat;
    flat.piece = "vide";
    flat.quad = {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}};
    const std::vector<HitArea> areas{flat, under, over};
    const HitArea* hit = ui::charter::HitTest(areas, 450, 250);
    Check(hit != nullptr && hit->piece == "logo",
          "where two areas overlap, the later one, drawn over, is hit");
    hit = ui::charter::HitTest(areas, 150, 150);
    Check(hit != nullptr && hit->piece == "nom", "the name alone is hit");
    hit = ui::charter::HitTest(areas, 500, 100);
    Check(hit != nullptr && hit->piece == "nom", "an edge counts as inside");
    Check(ui::charter::HitTest(areas, 50, 50) == nullptr,
          "outside every area: nothing");
    Check(ui::charter::HitTest(areas, 0, 0) == nullptr,
          "a box of no area is never hit");
    HitArea turned;
    turned.piece = "tourne";
    turned.quad = {{{300, 0}, {400, 100}, {300, 200}, {200, 100}}};
    Check(ui::charter::HitTest({turned}, 300, 100) != nullptr &&
              ui::charter::HitTest({turned}, 210, 10) == nullptr,
          "a turned box is hit inside its sides, not in its bounds' corners");
}

void CheckLogoDrag(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    Shown shown;
    Show(*logo, 0, 0.5, shown);
    if (!shown.ok) {
        return;
    }
    const Value kitFile = Json(kKitText);
    // No snapping first: the steps and the bounds alone.
    DragContext context =
        ui::charter::PieceDragContext(shown.preview, kitFile, 0.5);
    Check(context.frame_width == 1080 && context.frame_height == 1920 &&
              context.safe_zone.name == "social_vertical" &&
              Near(context.snap_distance, 16.0) &&
              context.values.Find("marge_droite") != nullptr &&
              context.values.Find("marge_droite")->Dump() == "14",
          "the drag context of the 9:16 logo: its size, the social zone, 8 "
          "points at half a pixel each, the format's values");
    context.snap_distance = 0.0;
    const HitArea& area = shown.area;

    // 100 pixels to the left: marge_droite, from 14 to 23.26, at 0.5.
    auto edits = ui::charter::DragEdits(MoveOf(area, -100, 0), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_droite", "23.5"}}),
          "a move left writes the right margin of 9:16 only, by 0.5: " +
              Describe(edits));
    Check(
        QuadIs(ui::charter::DragOutline(MoveOf(area, -100, 0), context),
               1080 - 253.8 - 194.4, 288.0, 1080 - 253.8, 385.2),
        "the outline is the box the written margin gives: " +
            Describe(ui::charter::DragOutline(MoveOf(area, -100, 0), context)));
    edits = ui::charter::DragEdits(MoveOf(area, -100, 0, true), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_droite", "23.3"}}),
          "with Option, by 0.1: " + Describe(edits));
    const Value laid = Laid(kitFile, edits);
    const Value* written =
        FindMotionKitPath(laid, "pieces.logo.formats.9x16.marge_droite");
    Check(written != nullptr && written->Dump() == "23.3" &&
              FindMotionKitPath(laid, "pieces.logo.marge_droite") == nullptr,
          "laid into the charter, the value is under the 9:16 format, not "
          "for every format: " +
              laid.Dump());

    // Down 96 pixels: marge_haut from 15 to 20, the right margin untouched.
    edits = ui::charter::DragEdits(MoveOf(area, 0, 96), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_haut", "20"}}),
          "a move down writes the top margin alone: " + Describe(edits));

    // No change: no edit.
    Check(ui::charter::DragEdits(MoveOf(area, 0, 0), context).empty(),
          "a click without a move writes nothing");
    Check(ui::charter::DragEdits(MoveOf(area, 1, 2), context).empty(),
          "a move shorter than half a step writes nothing");
    Check(QuadIs(ui::charter::DragOutline(MoveOf(area, 0, 0), context), 734.4,
                 288.0, 928.8, 385.2),
          "and its outline is the anchor");

    // Past the right edge: clamped to 0.
    edits = ui::charter::DragEdits(MoveOf(area, 300, 0), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_droite", "0"}}),
          "a margin under its minimum is clamped to it: " + Describe(edits));

    // Into the left half: the corner turns over, the left margin is
    // computed, the right one left as it was.
    edits = ui::charter::DragEdits(MoveOf(area, -500, 0), context);
    Check(
        EditsAre(edits, {{"pieces.logo.formats.9x16.coin", R"("haut-gauche")"},
                         {"pieces.logo.formats.9x16.marge_gauche", "21.5"}}),
        "into the left half the corner becomes haut-gauche, its margin "
        "computed: " +
            Describe(edits));
    Check(
        QuadIs(ui::charter::DragOutline(MoveOf(area, -500, 0), context), 232.2,
               288.0, 426.6, 385.2),
        "the outline sits at the new corner's margins: " +
            Describe(ui::charter::DragOutline(MoveOf(area, -500, 0), context)));
    edits = ui::charter::DragEdits(MoveOf(area, -500, 1000), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.coin", R"("bas-gauche")"},
                           {"pieces.logo.formats.9x16.marge_gauche", "21.5"},
                           {"pieces.logo.formats.9x16.marge_bas", "28"}}),
          "into the bottom left quarter: bas-gauche, both its margins: " +
              Describe(edits));

    // A resize by the top right corner: the bottom left corner holds.
    Drag resize = MoveOf(area, 100, 0);
    resize.kind = DragKind::Resize;
    resize.handle = 1;
    edits = ui::charter::DragEdits(resize, context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.largeur", "27.5"},
                           {"pieces.logo.formats.9x16.marge_droite", "4.5"},
                           {"pieces.logo.formats.9x16.marge_haut", "12.5"}}),
          "a corner resize writes the width, and the margins of the box "
          "the opposite corner holds: " +
              Describe(edits));
    Check(QuadIs(ui::charter::DragOutline(resize, context), 1080 - 48.6 - 297,
                 240.0, 1080 - 48.6, 240.0 + 148.5),
          "its outline keeps the image's aspect: " +
              Describe(ui::charter::DragOutline(resize, context)));
    resize.handle = 7;
    resize.current = {resize.start.first - 100, resize.start.second};
    edits = ui::charter::DragEdits(resize, context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.largeur", "27.5"}}),
          "the left handle grows the width; the right edge and the top "
          "hold, so no margin moves: " +
              Describe(edits));
    resize.handle = 4;
    Check(ui::charter::DragEdits(resize, context).empty(),
          "the top handle changes nothing");
    resize.handle = 6;
    Check(ui::charter::DragEdits(resize, context).empty(),
          "nor does the bottom one");
}

void CheckLogoSnap(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    Shown shown;
    Show(*logo, 0, 0.5, shown);
    if (!shown.ok) {
        return;
    }
    const DragContext context =
        ui::charter::PieceDragContext(shown.preview, Json(kKitText), 0.5);
    const HitArea& area = shown.area;
    // The social zone of 9:16: x 64.8 to 940, y 268.8 to 1 248. Snapping
    // reaches 16 composition pixels (8 points at half a pixel each).

    // The right edge 3.2 pixels short of the rail's line: onto it, and the
    // margin rounded up so that the logo stays clear of the rail.
    auto edits = ui::charter::DragEdits(MoveOf(area, 8, 0), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_droite", "13"}}),
          "the right edge snaps to the rail's line, the margin rounded "
          "inward to 13: " +
              Describe(edits));
    const Quad outline = ui::charter::DragOutline(MoveOf(area, 8, 0), context);
    Check(outline[1].first <= 940.0 + 1e-9,
          "the snapped edge never ends past the line: " + Describe(outline));
    DragContext loose = context;
    loose.snap_distance = 0.0;
    Check(EditsAre(ui::charter::DragEdits(MoveOf(area, 8, 0), loose),
                   {{"pieces.logo.formats.9x16.marge_droite", "13.5"}}),
          "without snapping, the same move rounds to the nearest step");
    // 20 pixels short: out of reach.
    Check(EditsAre(ui::charter::DragEdits(MoveOf(area, -9, 0), context),
                   {{"pieces.logo.formats.9x16.marge_droite", "15"}}),
          "a line out of reach does not snap");

    // The top edge 4.2 pixels under the zone's top: onto it, 14 % exactly.
    edits = ui::charter::DragEdits(MoveOf(area, 0, -15), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_haut", "14"}}),
          "the top edge snaps to the zone's top: " + Describe(edits));

    // The left edge 4.4 pixels right of the centre line: onto it.
    edits = ui::charter::DragEdits(MoveOf(area, -190, 0), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.marge_droite", "32"}}),
          "the left edge snaps to the centre line: " + Describe(edits));
    Check(
        Near(ui::charter::DragOutline(MoveOf(area, -190, 0), context)[0].first,
             540.0),
        "the outline's left edge is on the centre line");

    // Snapped to the zone's left edge, the left margin rounds up, inward.
    edits = ui::charter::DragEdits(MoveOf(area, -664, 0), context);
    Check(
        EditsAre(edits, {{"pieces.logo.formats.9x16.coin", R"("haut-gauche")"},
                         {"pieces.logo.formats.9x16.marge_gauche", "6"}}),
        "the left edge snaps to the zone's left, 6 %: " + Describe(edits));

    // Resized by the top right corner toward the rail: the moving edge
    // snaps, the width and the right margin keep it inside the line.
    Drag resize = MoveOf(area, 8, 0);
    resize.kind = DragKind::Resize;
    resize.handle = 1;
    edits = ui::charter::DragEdits(resize, context);
    Check(EditsAre(edits, {{"pieces.logo.formats.9x16.largeur", "19"},
                           {"pieces.logo.formats.9x16.marge_droite", "13"},
                           {"pieces.logo.formats.9x16.marge_haut", "14.5"}}),
          "a resize snaps its moving edge to the rail: " + Describe(edits));
    Check(ui::charter::DragOutline(resize, context)[1].first <= 940.0 + 1e-9,
          "and the resized box ends inside it: " +
              Describe(ui::charter::DragOutline(resize, context)));
}

void CheckLogoOtherFormats(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    Shown shown;
    Show(*logo, 1, 0.5, shown);
    if (!shown.ok) {
        return;
    }
    DragContext context =
        ui::charter::PieceDragContext(shown.preview, Json(kKitText), 0.5);
    Check(context.safe_zone.name == "default",
          "a 16:9 frame is judged against the default zone");
    context.snap_distance = 0.0;
    const auto edits =
        ui::charter::DragEdits(MoveOf(shown.area, -100, 0), context);
    Check(EditsAre(edits, {{"pieces.logo.formats.16x9.marge_droite", "11"}}),
          "in 16:9 the drag writes the 16:9 format, by % of 1 920: " +
              Describe(edits));

    // A piece whose format has another name: its own name in the path.
    HitArea custom = shown.area;
    custom.format = "paysage";
    Check(EditsAre(ui::charter::DragEdits(MoveOf(custom, -100, 0), context),
                   {{"pieces.logo.formats.paysage.marge_droite", "11"}}),
          "the path names the piece's own format");
    custom.format.clear();
    Check(ui::charter::DragEdits(MoveOf(custom, -100, 0), context).empty(),
          "no format: no edit, never one for every format");

    // A motion-kit.v1 charter is made a brand's first.
    context.kit =
        Json(R"({"format":"motion-kit.v1","description":"Ancienne charte.",)"
             R"("props":{"accent":"#1D6FA3"}})");
    Check(EditsAre(ui::charter::DragEdits(MoveOf(shown.area, -100, 0), context),
                   {{"format", R"("brand-kit.v1")"},
                    {"pieces.logo.formats.16x9.marge_droite", "11"}}),
          "a motion-kit.v1 charter is upgraded in the same batch");
}

void CheckNameDrag(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* nom = Find(shipped, "nom");
    Shown shown;
    Show(*nom, 0, 0.5, shown);
    if (!shown.ok) {
        return;
    }
    // The block: 9 % from the left, 36 % of the height from the bottom.
    Check(Near(shown.area.quad[0].first, 97.2, 0.05) &&
              Near(shown.area.quad[2].first, 982.8, 0.05) &&
              Near(shown.area.quad[2].second, 1228.8),
          "the name's block spans from marge to marge, hauteur from the "
          "bottom: " +
              Describe(shown.area.quad));
    DragContext context =
        ui::charter::PieceDragContext(shown.preview, Json(kKitText), 0.5);
    context.snap_distance = 0.0;
    auto edits = ui::charter::DragEdits(MoveOf(shown.area, 54, -96), context);
    Check(
        EditsAre(edits, {{"pieces.nom.formats.9x16.marge", "14"},
                         {"pieces.nom.formats.9x16.hauteur", "41"}}),
        "the name moved writes marge and hauteur of 9:16: " + Describe(edits));
    const Quad outline =
        ui::charter::DragOutline(MoveOf(shown.area, 54, -96), context);
    Check(Near(outline[0].first, 151.2) && Near(outline[1].first, 928.8) &&
              Near(outline[2].second, 1132.8),
          "its right edge follows its left: " + Describe(outline));
    Drag resize = MoveOf(shown.area, 54, -96);
    resize.kind = DragKind::Resize;
    resize.handle = 2;
    Check(ui::charter::DragEdits(resize, context).empty(),
          "the name is not resized");

    // Snapping: the bottom edge onto the zone's bottom line (1 248, 35 %
    // of the height from the bottom); across, the pointer did not move,
    // and nothing snaps there.
    context.snap_distance = 16.0;
    edits = ui::charter::DragEdits(MoveOf(shown.area, 0, 10), context);
    Check(EditsAre(edits, {{"pieces.nom.formats.9x16.hauteur", "35"}}),
          "the block's bottom snaps to the zone's bottom: " + Describe(edits));
    Check(ui::charter::DragOutline(MoveOf(shown.area, 0, 10), context)[2]
                  .second <= 1248.0 + 1e-9,
          "and does not end below it");
}

void CheckCaptionDrag() {
    HitArea area;
    area.kind = AreaKind::Captions;
    area.quad = {{{240, 1500}, {840, 1500}, {840, 1572}, {240, 1572}}};
    DragContext context;
    context.frame_width = 1080;
    context.frame_height = 1920;
    context.kit =
        Json(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
             R"("captions":{"font_family":"Exemple Sans"}})");
    context.captions.has_formats = false;
    context.captions.percent = 80;

    // 96 pixels up, 5 % of the height: 75, written 3/4, for every size.
    auto edits = ui::charter::DragEdits(MoveOf(area, 30, -96), context);
    Check(EditsAre(edits,
                   {{"captions.vertical_position", R"({"num":3,"den":4})"}}),
          "without caption formats the subtitle edits the base height, as a "
          "reduced fraction, vertically only: " +
              Describe(edits));
    Check(
        QuadIs(ui::charter::DragOutline(MoveOf(area, 30, -96), context), 240,
               1404, 840, 1476),
        "its outline is centred at 75 % of the height: " +
            Describe(ui::charter::DragOutline(MoveOf(area, 30, -96), context)));
    edits = ui::charter::DragEdits(MoveOf(area, 0, -192, true), context);
    Check(EditsAre(edits,
                   {{"captions.vertical_position", R"({"num":7,"den":10})"}}),
          "whole percentages, even with Option: " + Describe(edits));
    Check(ui::charter::DragEdits(MoveOf(area, 0, 9), context).empty(),
          "less than half a percent: nothing");
    Check(EditsAre(ui::charter::DragEdits(MoveOf(area, 0, 2000), context),
                   {{"captions.vertical_position", R"({"num":99,"den":100})"}}),
          "never past 99 %");

    // With caption formats: that size's format, or nothing.
    context.captions.has_formats = true;
    context.captions.format = "vertical";
    Check(EditsAre(ui::charter::DragEdits(MoveOf(area, 0, -96), context),
                   {{"captions.formats.vertical.vertical_position",
                     R"({"num":3,"den":4})"}}),
          "with formats, the format of that size, by its own name");
    context.captions.format.clear();
    Check(ui::charter::DragEdits(MoveOf(area, 0, -96), context).empty(),
          "formats and none of that size: nothing");

    // A value already there: no edit.
    context.captions.has_formats = false;
    context.kit =
        Json(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
             R"("captions":{"vertical_position":{"num":3,"den":4}}})");
    Check(ui::charter::DragEdits(MoveOf(area, 0, -96), context).empty(),
          "the height the charter already has writes nothing");

    // The context the panel builds from a charter.
    MotionKitInfo kit;
    std::string error;
    Check(ParseMotionKit(
              "marque",
              R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
              R"("captions":{"font_family":"Exemple Sans",)"
              R"("vertical_position":{"num":3,"den":4}}})",
              kit, error),
          "a charter with a base height reads: " + error);
    ui::charter::CaptionDragContext captions;
    Check(
        ui::charter::CaptionDragContextFor(kit, PreviewFrames()[1], captions) &&
            !captions.has_formats && captions.format.empty() &&
            captions.percent == 75,
        "a base height of 3/4 is 75 % in every size");
    Check(ParseMotionKit(
              "marque",
              R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
              R"("captions":{"font_family":"Exemple Sans","formats":)"
              R"({"vertical":{"width":1080,"height":1920,)"
              R"("vertical_position":{"num":4,"den":5}}}}})",
              kit, error),
          "a charter with a 9:16 caption format reads: " + error);
    Check(
        ui::charter::CaptionDragContextFor(kit, PreviewFrames()[0], captions) &&
            captions.has_formats && captions.format == "vertical" &&
            captions.percent == 80,
        "in 9:16 its format, by name, at 80 %");
    Check(!ui::charter::CaptionDragContextFor(kit, PreviewFrames()[1],
                                              captions) &&
              captions.has_formats && captions.format.empty(),
          "in 16:9 no caption style at all: nothing to drag");
    Check(ParseMotionKit("marque", kKitText, kit, error) &&
              !ui::charter::CaptionDragContextFor(kit, PreviewFrames()[0],
                                                  captions),
          "a charter without captions has no subtitle to drag");
}

}  // namespace

int main() {
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    if (Find(shipped, "logo") == nullptr || Find(shipped, "nom") == nullptr) {
        Check(false, "logo and nom are shipped");
        return 1;
    }
    CheckAnchors(shipped);
    CheckHitTest();
    CheckLogoDrag(shipped);
    CheckLogoSnap(shipped);
    CheckLogoOtherFormats(shipped);
    CheckNameDrag(shipped);
    CheckCaptionDrag();
    if (failures == 0) std::cout << "charter canvas tests passed\n";
    return failures == 0 ? 0 : 1;
}
