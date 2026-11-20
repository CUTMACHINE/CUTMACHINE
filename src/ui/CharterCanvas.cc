// CH5 -- docs/roadmap/CHARTE-2026-11.md. A drag on the Charte panel's
// preview, as values of the props a piece already declares
// (CharterCanvas.h). One function says where a release lands -- the box,
// and the values that put it there -- so that the outline drawn while the
// mouse is down is the box the written values give, never a second guess.

#include "CharterCanvas.h"

#include "CharterPanelModel.h"
#include "Document.h"
#include "MediaPanelModel.h"
#include "MotionComposition.h"
#include "MotionGeometry.h"
#include "MotionTemplates.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ui::charter {

namespace {

using mcp_json::Value;
using media_panel::CaptionHeightPercent;
using media_panel::kCaptionHeightPercentMax;
using media_panel::kCaptionHeightPercentMin;

constexpr double kEpsilon = 1e-9;

// The first node with author id `id` that is shown, and the transform of
// the space it sits in; null when there is none, or every copy sits at
// zero opacity or under an ancestor that does. The evaluated tree of the
// composition itself: a piece's anchor is never inside an instance.
const MotionEvaluatedNode* FindShown(const MotionEvaluatedNode& node,
                                     const MotionHomography& parent,
                                     const std::string& id,
                                     MotionHomography& found) {
    if (node.opacity <= 0.0) {
        return nullptr;
    }
    if (node.id == id) {
        found = parent;
        return &node;
    }
    const MotionHomography full = parent.After(MotionNodeMapping(node));
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* hit =
                FindShown(child, full, id, found)) {
            return hit;
        }
    }
    return nullptr;
}

// An axis-aligned box in composition pixels.
struct Box {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    double Width() const { return right - left; }
    double Height() const { return bottom - top; }
};

Box BoundsOf(const Quad& quad) {
    Box box{quad[0].first, quad[0].second, quad[0].first, quad[0].second};
    for (const Point& point : quad) {
        box.left = std::min(box.left, point.first);
        box.right = std::max(box.right, point.first);
        box.top = std::min(box.top, point.second);
        box.bottom = std::max(box.bottom, point.second);
    }
    return box;
}

Quad QuadOf(const Box& box) {
    return {Point{box.left, box.top}, Point{box.right, box.top},
            Point{box.right, box.bottom}, Point{box.left, box.bottom}};
}

// `value` onto the grid of `step` by `round` -- through the step's inverse
// when it is whole, as MotionKitNumber rounds, so that both agree on every
// value a drag writes. No grid when `step` is not positive.
double OnGrid(double value, double step,
              const std::function<double(double)>& round) {
    if (!(step > 0.0) || !std::isfinite(step)) {
        return value;
    }
    const double inverse = 1.0 / step;
    const double whole = std::round(inverse);
    if (whole >= 1.0 && std::abs(inverse - whole) < kEpsilon) {
        return round(value * whole) / whole;
    }
    return round(value / step) * step;
}

double Nearest(double value, double step) {
    return OnGrid(value, step, [](double x) { return std::round(x); });
}

// Floor and ceiling forgive the last bits of a product: 12.5 at 0.5 is on
// the grid even when 12.5 * 2 lands a hair above 25.
double Below(double value, double step) {
    return OnGrid(value, step, [](double x) { return std::floor(x + 1e-7); });
}

double Above(double value, double step) {
    return OnGrid(value, step, [](double x) { return std::ceil(x - 1e-7); });
}

const MotionPropDefinition* PropNamed(const DragContext& context,
                                      const std::string& name) {
    for (const MotionPropDefinition& prop : context.props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

double Clamp(double value, const MotionPropDefinition& prop) {
    if (prop.minimum) {
        value = std::max(value, *prop.minimum);
    }
    if (prop.maximum) {
        value = std::min(value, *prop.maximum);
    }
    return value;
}

std::optional<double> NumberIn(const Value& values, const std::string& name) {
    const Value* value = values.Find(name);
    double number = 0.0;
    if (value == nullptr || !value->IsNumber() ||
        !MotionNumber(*value, number)) {
        return std::nullopt;
    }
    return number;
}

std::string TextIn(const Value& values, const std::string& name) {
    const Value* value = values.Find(name);
    return value != nullptr && value->IsString() ? value->AsString()
                                                 : std::string();
}

// A line an edge snaps to, on one axis: `inside` is the side of it that is
// inside the safe zone (+1 toward larger coordinates); the centre line has
// no inside of its own, and takes the side the box lies on.
struct Line {
    double at = 0.0;
    int inside = 0;
    bool centre = false;
};

std::vector<Line> LinesOn(double size, const MotionSafeMargin& low,
                          const MotionSafeMargin& high) {
    const auto fraction = [](const MotionSafeMargin& margin) {
        return margin.den > 0 ? static_cast<double>(margin.num) /
                                    static_cast<double>(margin.den)
                              : 0.0;
    };
    return {{size * fraction(low), 1, false},
            {size - size * fraction(high), -1, false},
            {size / 2.0, 0, true}};
}

// An edge that moves with the gesture: at `base + k * d` for an offset d
// of the pointer. `leading` is the left or top edge of the box.
struct Edge {
    double base = 0.0;
    double k = 1.0;
    bool leading = true;
};

// Which line an axis snapped to, and the side a rounding must keep the
// snapped edge on.
struct Snap {
    bool snapped = false;
    double line = 0.0;
    int inside = 0;
    bool leading = true;
};

// The pointer's offset `d` on one axis, moved so that the edge nearest a
// line within `distance` lies on it; `d` itself when none is that near, or
// when the pointer did not move on that axis -- a vertical drag must not
// pull the box sideways onto a line it merely started near.
double Snapped(const std::vector<Edge>& edges, const std::vector<Line>& lines,
               double d, double distance, Snap& snap) {
    snap = Snap{};
    if (!(distance > 0.0) || d == 0.0) {
        return d;
    }
    double best = distance;
    double offset = d;
    for (const Edge& edge : edges) {
        if (edge.k == 0.0) {
            continue;
        }
        const double at = edge.base + edge.k * d;
        for (const Line& line : lines) {
            const double away = std::abs(at - line.at);
            if (away <= best && (!snap.snapped || away < best)) {
                best = away;
                offset = d + (line.at - at) / edge.k;
                snap.snapped = true;
                snap.line = line.at;
                snap.leading = edge.leading;
                snap.inside =
                    line.centre ? (edge.leading ? 1 : -1) : line.inside;
            }
        }
    }
    return offset;
}

// A margin (or a width) onto the grid: to the nearest step, or, when its
// axis snapped, to the step that keeps the snapped edge on the inside of
// the line -- `edgeAt` says where that edge lands for a candidate value.
// Then clamped to the prop's bounds.
double Rounded(double value, double step, const Snap& snap,
               const std::function<double(double)>& edgeAt,
               const MotionPropDefinition& prop) {
    double chosen = Nearest(value, step);
    if (snap.snapped) {
        const double below = Below(value, step);
        const double above = Above(value, step);
        const auto inside = [&](double candidate) {
            return (edgeAt(candidate) - snap.line) * snap.inside >= -1e-6;
        };
        const bool belowInside = inside(below);
        const bool aboveInside = inside(above);
        if (belowInside != aboveInside) {
            chosen = belowInside ? below : above;
        }
    }
    return Clamp(chosen, prop);
}

bool Changed(const std::optional<double>& current, double value) {
    return !current || std::abs(*current - value) > kEpsilon;
}

// Where a release lands: the box, and the values that differ from those in
// effect, in the order they are written.
struct Landing {
    Quad quad{};
    std::vector<std::pair<std::string, Value>> props;
    // The subtitle's: the path and the fraction.
    std::string caption_path;
    std::optional<Value> caption;
};

bool IsLeft(const std::string& corner) {
    return corner.size() >= 6 &&
           corner.compare(corner.size() - 6, 6, "gauche") == 0;
}

bool IsTop(const std::string& corner) { return corner.rfind("haut", 0) == 0; }

std::string CornerName(bool left, bool top) {
    return std::string(top ? "haut" : "bas") + (left ? "-gauche" : "-droite");
}

// The logo's box for a corner, the margins of that corner (% of the width
// across, of the height down) and a size, as the piece lays it out.
Box LogoBox(bool left, bool top, double across, double down, double width,
            double height, double frameWidth, double frameHeight) {
    Box box;
    if (left) {
        box.left = across * frameWidth / 100.0;
        box.right = box.left + width;
    } else {
        box.right = frameWidth - across * frameWidth / 100.0;
        box.left = box.right - width;
    }
    if (top) {
        box.top = down * frameHeight / 100.0;
        box.bottom = box.top + height;
    } else {
        box.bottom = frameHeight - down * frameHeight / 100.0;
        box.top = box.bottom - height;
    }
    return box;
}

void LandLogo(const Drag& drag, const DragContext& context, Landing& landing) {
    const double frameWidth = context.frame_width;
    const double frameHeight = context.frame_height;
    const Box start = BoundsOf(drag.area.quad);
    const double step = drag.fine ? kFinePercentStep : kPercentStep;
    const double dx = drag.current.first - drag.start.first;
    const double dy = drag.current.second - drag.start.second;
    const std::vector<Line> across =
        LinesOn(frameWidth, context.safe_zone.left, context.safe_zone.right);
    const std::vector<Line> down =
        LinesOn(frameHeight, context.safe_zone.top, context.safe_zone.bottom);
    std::string corner = TextIn(context.values, "coin");
    if (corner.empty()) {
        corner = "haut-droite";
    }
    const bool left = IsLeft(corner);
    const bool top = IsTop(corner);

    Box target = start;
    Snap snapX;
    Snap snapY;
    // Which edges did not move, for a resize: the margins on their side
    // stay as they are.
    std::optional<bool> heldLeft;
    std::optional<bool> heldTop;
    const std::optional<double> width = NumberIn(context.values, "largeur");
    double largeur = width.value_or(0.0);
    bool resized = false;
    if (drag.kind == DragKind::Move) {
        const double offsetX =
            Snapped({{start.left, 1.0, true}, {start.right, 1.0, false}},
                    across, dx, context.snap_distance, snapX);
        const double offsetY =
            Snapped({{start.top, 1.0, true}, {start.bottom, 1.0, false}}, down,
                    dy, context.snap_distance, snapY);
        target = {start.left + offsetX, start.top + offsetY,
                  start.right + offsetX, start.bottom + offsetY};
        if (offsetX == 0.0 && !snapX.snapped) {
            heldLeft = left;
        }
        if (offsetY == 0.0 && !snapY.snapped) {
            heldTop = top;
        }
    } else {
        const int handle = drag.handle;
        const bool corners = handle >= 0 && handle <= 3;
        if (!corners && handle != 5 && handle != 7) {
            landing.quad = drag.area.quad;
            return;
        }
        const bool movingRight = handle == 1 || handle == 2 || handle == 5;
        const bool holdTop = corners ? (handle == 2 || handle == 3) : top;
        const double offset = Snapped(
            {{movingRight ? start.right : start.left, 1.0, !movingRight}},
            across, dx, context.snap_distance, snapX);
        double newWidth = std::max(
            1.0, movingRight ? start.Width() + offset : start.Width() - offset);
        if (const MotionPropDefinition* prop = PropNamed(context, "largeur")) {
            largeur = Rounded(
                newWidth / frameWidth * 100.0, step, snapX,
                [&](double candidate) {
                    const double w = candidate * frameWidth / 100.0;
                    return movingRight ? start.left + w : start.right - w;
                },
                *prop);
            newWidth = std::max(1.0, largeur * frameWidth / 100.0);
            resized = true;
        }
        const double newHeight = start.Width() > 0.0
                                     ? start.Height() * newWidth / start.Width()
                                     : start.Height();
        target.left = movingRight ? start.left : start.right - newWidth;
        target.right = target.left + newWidth;
        target.top = holdTop ? start.top : start.bottom - newHeight;
        target.bottom = target.top + newHeight;
        heldLeft = movingRight;
        heldTop = holdTop;
        // The width is rounded; the margin of the moving edge's side, if
        // it is the corner's, is rounded by the same snap below, so that
        // the edge stays inside the line it snapped to.
    }

    // The quadrant of the box's centre; on a middle line, the side it had.
    // A piece whose `coin` does not take that corner keeps its own.
    const double centreX = (target.left + target.right) / 2.0;
    const double centreY = (target.top + target.bottom) / 2.0;
    bool newLeft = centreX < frameWidth / 2.0 - kEpsilon   ? true
                   : centreX > frameWidth / 2.0 + kEpsilon ? false
                                                           : left;
    bool newTop = centreY < frameHeight / 2.0 - kEpsilon   ? true
                  : centreY > frameHeight / 2.0 + kEpsilon ? false
                                                           : top;
    const MotionPropDefinition* cornerProp = PropNamed(context, "coin");
    if (cornerProp == nullptr ||
        std::find(cornerProp->values.begin(), cornerProp->values.end(),
                  CornerName(newLeft, newTop)) == cornerProp->values.end()) {
        newLeft = left;
        newTop = top;
    }
    const std::string acrossName = newLeft ? "marge_gauche" : "marge_droite";
    const std::string downName = newTop ? "marge_haut" : "marge_bas";
    const std::optional<double> acrossNow =
        NumberIn(context.values, acrossName);
    const std::optional<double> downNow = NumberIn(context.values, downName);
    double acrossValue = acrossNow.value_or(0.0);
    double downValue = downNow.value_or(0.0);
    const MotionPropDefinition* acrossProp = PropNamed(context, acrossName);
    const MotionPropDefinition* downProp = PropNamed(context, downName);
    const bool acrossHeld =
        heldLeft && *heldLeft == newLeft && newLeft == left && acrossNow;
    const bool downHeld =
        heldTop && *heldTop == newTop && newTop == top && downNow;
    if (acrossProp != nullptr && !acrossHeld) {
        const double real =
            newLeft ? target.left / frameWidth * 100.0
                    : (frameWidth - target.right) / frameWidth * 100.0;
        acrossValue = Rounded(
            real, step, snapX,
            [&](double candidate) {
                const Box box = LogoBox(newLeft, newTop, candidate, downValue,
                                        target.Width(), target.Height(),
                                        frameWidth, frameHeight);
                return snapX.leading ? box.left : box.right;
            },
            *acrossProp);
    }
    if (downProp != nullptr && !downHeld) {
        const double real =
            newTop ? target.top / frameHeight * 100.0
                   : (frameHeight - target.bottom) / frameHeight * 100.0;
        downValue = Rounded(
            real, step, snapY,
            [&](double candidate) {
                const Box box = LogoBox(newLeft, newTop, acrossValue, candidate,
                                        target.Width(), target.Height(),
                                        frameWidth, frameHeight);
                return snapY.leading ? box.top : box.bottom;
            },
            *downProp);
    }
    landing.quad =
        QuadOf(LogoBox(newLeft, newTop, acrossValue, downValue, target.Width(),
                       target.Height(), frameWidth, frameHeight));

    if (const std::string newCorner = CornerName(newLeft, newTop);
        newCorner != corner) {
        landing.props.emplace_back("coin", Value::MakeString(newCorner));
    }
    if (resized && Changed(width, largeur)) {
        landing.props.emplace_back("largeur", MotionKitNumber(largeur, 0.0));
    }
    if (acrossProp != nullptr && Changed(acrossNow, acrossValue)) {
        landing.props.emplace_back(acrossName,
                                   MotionKitNumber(acrossValue, 0.0));
    }
    if (downProp != nullptr && Changed(downNow, downValue)) {
        landing.props.emplace_back(downName, MotionKitNumber(downValue, 0.0));
    }
}

void LandName(const Drag& drag, const DragContext& context, Landing& landing) {
    landing.quad = drag.area.quad;
    if (drag.kind != DragKind::Move) {
        return;
    }
    const double frameWidth = context.frame_width;
    const double frameHeight = context.frame_height;
    const Box start = BoundsOf(drag.area.quad);
    const double step = drag.fine ? kFinePercentStep : kPercentStep;
    const double dx = drag.current.first - drag.start.first;
    const double dy = drag.current.second - drag.start.second;
    // Across, only the left edge snaps: the block's right edge mirrors it
    // (`marge` short of the frame's), and nothing is drawn there -- the
    // bands sit on the left -- so a snap on it would jump for no visible
    // reason.
    Snap snapX;
    Snap snapY;
    const double offsetX = Snapped(
        {{start.left, 1.0, true}},
        LinesOn(frameWidth, context.safe_zone.left, context.safe_zone.right),
        dx, context.snap_distance, snapX);
    const double offsetY = Snapped(
        {{start.top, 1.0, true}, {start.bottom, 1.0, false}},
        LinesOn(frameHeight, context.safe_zone.top, context.safe_zone.bottom),
        dy, context.snap_distance, snapY);
    const std::optional<double> margeNow = NumberIn(context.values, "marge");
    const std::optional<double> hauteurNow =
        NumberIn(context.values, "hauteur");
    double marge = margeNow.value_or((start.left / frameWidth) * 100.0);
    double hauteur =
        hauteurNow.value_or((frameHeight - start.bottom) / frameHeight * 100.0);
    const MotionPropDefinition* margeProp = PropNamed(context, "marge");
    const MotionPropDefinition* hauteurProp = PropNamed(context, "hauteur");
    const double height = start.Height();
    if (margeProp != nullptr && (offsetX != 0.0 || snapX.snapped)) {
        marge = Rounded(
            (start.left + offsetX) / frameWidth * 100.0, step, snapX,
            [&](double candidate) { return candidate * frameWidth / 100.0; },
            *margeProp);
    }
    if (hauteurProp != nullptr && (offsetY != 0.0 || snapY.snapped)) {
        hauteur = Rounded(
            (frameHeight - (start.bottom + offsetY)) / frameHeight * 100.0,
            step, snapY,
            [&](double candidate) {
                const double bottom =
                    frameHeight - candidate * frameHeight / 100.0;
                return snapY.leading ? bottom - height : bottom;
            },
            *hauteurProp);
    }
    Box box;
    box.left = marge * frameWidth / 100.0;
    box.right = std::max(box.left, frameWidth - box.left);
    box.bottom = frameHeight - hauteur * frameHeight / 100.0;
    box.top = box.bottom - height;
    landing.quad = QuadOf(box);
    if (margeProp != nullptr && Changed(margeNow, marge)) {
        landing.props.emplace_back("marge", MotionKitNumber(marge, 0.0));
    }
    if (hauteurProp != nullptr && Changed(hauteurNow, hauteur)) {
        landing.props.emplace_back("hauteur", MotionKitNumber(hauteur, 0.0));
    }
}

// A fraction {num, den} as a charter's captions write vertical_position,
// read as a whole percentage; nullopt for anything else.
std::optional<int32_t> PercentOf(const Value* fraction) {
    if (fraction == nullptr || !fraction->IsObject()) {
        return std::nullopt;
    }
    int64_t num = 0;
    int64_t den = 0;
    const Value* numValue = fraction->Find("num");
    const Value* denValue = fraction->Find("den");
    if (numValue == nullptr || denValue == nullptr || !numValue->AsInt64(num) ||
        !denValue->AsInt64(den) || den <= 0 || num * 100 % den != 0) {
        return std::nullopt;
    }
    return static_cast<int32_t>(num * 100 / den);
}

void LandCaptions(const Drag& drag, const DragContext& context,
                  Landing& landing) {
    landing.quad = drag.area.quad;
    const CaptionDragContext& captions = context.captions;
    if (drag.kind != DragKind::Move || context.frame_height <= 0 ||
        (captions.has_formats && captions.format.empty())) {
        return;
    }
    const double dy = drag.current.second - drag.start.second;
    const int32_t percent = static_cast<int32_t>(std::clamp<int64_t>(
        std::lround(captions.percent + dy / context.frame_height * 100.0),
        kCaptionHeightPercentMin, kCaptionHeightPercentMax));
    if (percent == captions.percent) {
        return;
    }
    const Box start = BoundsOf(drag.area.quad);
    const double shift = percent * context.frame_height / 100.0 -
                         (start.top + start.bottom) / 2.0;
    landing.quad = QuadOf(
        {start.left, start.top + shift, start.right, start.bottom + shift});
    const std::string path = MotionKitCaptionPath(
        captions.has_formats ? captions.format : std::string(),
        "vertical_position");
    if (PercentOf(FindMotionKitPath(context.kit, path)) == percent) {
        return;
    }
    const int32_t divisor = std::gcd(percent, 100);
    Value fraction = Value::MakeObject();
    fraction.Set("num", Value::MakeInt(percent / divisor));
    fraction.Set("den", Value::MakeInt(100 / divisor));
    landing.caption_path = path;
    landing.caption = std::move(fraction);
}

Landing Land(const Drag& drag, const DragContext& context) {
    Landing landing;
    landing.quad = drag.area.quad;
    if (drag.start == drag.current || context.frame_width <= 0 ||
        context.frame_height <= 0) {
        return landing;
    }
    if (drag.area.kind == AreaKind::Captions) {
        LandCaptions(drag, context, landing);
    } else if (drag.area.piece == "logo") {
        LandLogo(drag, context, landing);
    } else if (drag.area.piece == "nom") {
        LandName(drag, context, landing);
    }
    return landing;
}

// Twice the signed area of `quad`; zero for a box that covers nothing.
double SignedArea(const Quad& quad) {
    double area = 0.0;
    for (size_t corner = 0; corner < quad.size(); ++corner) {
        const Point& from = quad[corner];
        const Point& to = quad[(corner + 1) % quad.size()];
        area += from.first * to.second - to.first * from.second;
    }
    return area;
}

// Inside a convex quad of either winding, edges included.
bool Contains(const Quad& quad, double x, double y) {
    if (std::abs(SignedArea(quad)) <= kEpsilon) {
        return false;
    }
    int side = 0;
    for (size_t corner = 0; corner < quad.size(); ++corner) {
        const Point& from = quad[corner];
        const Point& to = quad[(corner + 1) % quad.size()];
        const double cross = (to.first - from.first) * (y - from.second) -
                             (to.second - from.second) * (x - from.first);
        if (std::abs(cross) <= kEpsilon) {
            continue;
        }
        const int sign = cross > 0.0 ? 1 : -1;
        if (side == 0) {
            side = sign;
        } else if (sign != side) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string AnchorId(const std::string& piece) {
    if (piece == "logo") {
        return "logo";
    }
    if (piece == "nom") {
        return "bloc";
    }
    return {};
}

bool AnchorQuad(const MotionFrame& evaluated, const std::string& piece,
                double scale, Quad& quad) {
    const std::string id = AnchorId(piece);
    if (id.empty() || !(scale > 0.0)) {
        return false;
    }
    MotionHomography parent;
    const MotionEvaluatedNode* node =
        FindShown(evaluated.root, MotionHomography{}, id, parent);
    if (node == nullptr) {
        return false;
    }
    const MotionHomography full = parent.After(MotionNodeMapping(*node));
    const double xs[] = {node->box.x, node->box.x + node->box.width,
                         node->box.x + node->box.width, node->box.x};
    const double ys[] = {node->box.y, node->box.y,
                         node->box.y + node->box.height,
                         node->box.y + node->box.height};
    Quad found{};
    for (size_t corner = 0; corner < found.size(); ++corner) {
        double x = 0.0;
        double y = 0.0;
        if (!full.Apply(xs[corner], ys[corner], x, y)) {
            return false;
        }
        found[corner] = {x / scale, y / scale};
    }
    quad = found;
    return true;
}

const HitArea* HitTest(const std::vector<HitArea>& areas, double x, double y) {
    for (auto area = areas.rbegin(); area != areas.rend(); ++area) {
        if (Contains(area->quad, x, y)) {
            return &*area;
        }
    }
    return nullptr;
}

Quad DragOutline(const Drag& drag, const DragContext& context) {
    return Land(drag, context).quad;
}

std::vector<MotionKitEdit> DragEdits(const Drag& drag,
                                     const DragContext& context) {
    const Landing landing = Land(drag, context);
    std::vector<MotionKitEdit> edits;
    if (drag.area.kind == AreaKind::Captions) {
        if (landing.caption) {
            edits.push_back({landing.caption_path, landing.caption, false});
        }
    } else if (!drag.area.format.empty()) {
        for (const auto& [prop, value] : landing.props) {
            edits.push_back(
                {MotionKitPiecePath(drag.area.piece, drag.area.format, prop),
                 value, false});
        }
    }
    return WithBrandFormat(context.kit, std::move(edits));
}

DragContext PieceDragContext(const PiecePreview& preview,
                             const mcp_json::Value& kitFile,
                             double pointsPerPixel) {
    DragContext context;
    context.frame_width = preview.frame.width;
    context.frame_height = preview.frame.height;
    context.safe_zone =
        TimelineMotionSafeZone(preview.frame.width, preview.frame.height);
    context.snap_distance =
        pointsPerPixel > 0.0 ? kSnapPoints / pointsPerPixel : 0.0;
    context.props = preview.composition.props;
    context.values = PieceValues(preview);
    context.kit = kitFile;
    return context;
}

bool CaptionDragContextFor(const MotionKitInfo& kit, const PreviewFrame& frame,
                           CaptionDragContext& out) {
    out = CaptionDragContext{};
    const Value* formats =
        kit.captions.IsObject() ? kit.captions.Find("formats") : nullptr;
    out.has_formats = formats != nullptr && formats->IsObject() &&
                      !formats->AsObject().empty();
    CaptionStyle style;
    std::vector<std::string> set;
    std::string error;
    if (!MotionKitCaptionStyle(kit, frame.width, frame.height, style,
                               out.format, set, error)) {
        out.format.clear();
        return false;
    }
    out.percent = CaptionHeightPercent(style);
    return true;
}

}  // namespace ui::charter
