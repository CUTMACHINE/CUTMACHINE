#pragma once

// GR8 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The hue curves of the
// Inspector, as policy: the points a curve shows, the one under the pointer,
// and the stack a gesture gives. Portable, no framework:
// GradeHueCurveView.mm draws what this says and turns mouse events into
// calls here, as GradeCurveView.mm does with GradeCurveModel.h, whose
// gestures these are (a click adds a point, with Shift on the curve; Option
// snaps to neutral; a right click removes).
//
// A curve of the clip's first color.hue_curves stage (ColorEffects.h) is
// none or 2 to kMaxColorCurvePoints points, neutral at y = 1/2. What differs
// from a tone curve:
//
//  * A curve without a point is a flat line, and one point would not be a
//    curve. The first click on one therefore places three: the point
//    clicked and an anchor either side of it, at neutral, so that the click
//    changes that colour and not its neighbours -- what the application the
//    timelines go to does from its six colour patches (its manual, "Six-
//    Vector Color Patches"). Along the luma or the saturation the anchors
//    are the two ends.
//  * A curve along the hue goes round. Its points stay in [0, 1), and a
//    point dragged to an edge stops there: it does not come back in at the
//    other.
//  * Any point may be removed. A curve left with one point is cleared, and a
//    stage left without a point is removed, so that a clip whose hue curves
//    were all reset reaches the export as one that never had any.
//
// The curve drawn is the one rendered: it is sampled through
// ResolveColorGrade and EvaluateHueCurve.

#include "ColorEffects.h"
#include "Document.h"
#include "GradeCurveModel.h"
#include "GradeWheelModel.h"
#include "InspectorGradeControls.h"
#include "UiTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui::inspector {

struct HueCurveSpec {
    const char* name;   // Its name in color.hue_curves: "hue_sat".
    const char* tab;    // What the bar's tab reads.
    const char* title;  // In full, for a person: the tab's tooltip.
    bool periodic;      // Read along the hue.
};

// In kHueCurveNames' order.
inline const std::array<HueCurveSpec, 6>& HueCurveSpecs() {
    static const std::array<HueCurveSpec, 6> kSpecs{{
        {"hue_hue", "T/T", "Teinte contre teinte", true},
        {"hue_sat", "T/S", "Teinte contre saturation", true},
        {"hue_lum", "T/L", "Teinte contre luma", true},
        {"lum_sat", "L/S", "Luma contre saturation", false},
        {"sat_sat", "S/S", "Saturation contre saturation", false},
        {"sat_lum", "S/L", "Saturation contre luma", false},
    }};
    return kSpecs;
}

inline const HueCurveSpec* FindHueCurveSpec(const std::string& name) {
    for (const HueCurveSpec& spec : HueCurveSpecs()) {
        if (name == spec.name) {
            return &spec;
        }
    }
    return nullptr;
}

// Half the width of what a first click, or a colour patch, changes: a
// twelfth of a turn either side, the distance from a primary to halfway to
// its neighbour.
inline constexpr double kHueCurveBand = 1.0 / 12.0;

// The hue of a channel triple, as a fraction of a turn: the vectorscope's
// angle over 360.
inline double HueOfChannels(const std::array<double, 3>& channels) {
    const std::array<double, 2> chroma = WheelChromaOfChannels(channels);
    const double turn =
        std::atan2(chroma[1], chroma[0]) / (2.0 * 3.14159265358979323846);
    return turn - std::floor(turn);
}

// The six patches of a curve along the hue: red, yellow, green, cyan, blue
// and magenta, each the colour it is painted with and the hue it stands at.
struct HueCurveVector {
    const char* label;
    std::array<float, 3> color;
    double hue;
};

inline const std::array<HueCurveVector, 6>& HueCurveVectors() {
    static const std::array<HueCurveVector, 6> kVectors = [] {
        std::array<HueCurveVector, 6> vectors{{
            {"Rouge", {1.0f, 0.0f, 0.0f}, 0.0},
            {"Jaune", {1.0f, 1.0f, 0.0f}, 0.0},
            {"Vert", {0.0f, 1.0f, 0.0f}, 0.0},
            {"Cyan", {0.0f, 1.0f, 1.0f}, 0.0},
            {"Bleu", {0.0f, 0.0f, 1.0f}, 0.0},
            {"Magenta", {1.0f, 0.0f, 1.0f}, 0.0},
        }};
        for (HueCurveVector& vector : vectors) {
            vector.hue = HueOfChannels(
                {vector.color[0], vector.color[1], vector.color[2]});
        }
        return vectors;
    }();
    return kVectors;
}

// The points a curve holds; none when it has none (or does not read as a
// curve, which only an unvalidated stack can).
inline std::vector<ColorCurvePoint> HueCurvePoints(
    const std::vector<ClipEffect>& effects, const std::string& curve) {
    std::vector<ColorCurvePoint> points;
    std::string ignored;
    const ClipEffect* stage = FindFirstEffect(effects, "color.hue_curves");
    if (stage == nullptr || !ReadHueCurve(*stage, curve, points, ignored)) {
        points.clear();
    }
    return points;
}

// One curve as the renderer resolves it: the stage with that curve alone,
// and where its block starts in the pool. `block` is null without a point.
struct ResolvedHueCurve {
    ResolvedColorGrade grade;
    const float* block = nullptr;
};

inline void ResolveHueCurve(const std::vector<ClipEffect>& effects,
                            const std::string& curve,
                            ResolvedHueCurve& resolved) {
    resolved.block = nullptr;
    const ClipEffect* stage = FindFirstEffect(effects, "color.hue_curves");
    if (stage == nullptr) {
        return;
    }
    ClipEffect alone{stage->id, "color.hue_curves", {}};
    const std::string prefix = curve + "_";
    for (const auto& [name, value] : stage->params) {
        if (name.rfind(prefix, 0) == 0) {
            alone.params[name] = value;
        }
    }
    resolved.grade = ResolveColorGrade({alone}, {});
    if (resolved.grade.count != 1) {
        return;
    }
    // The curves before it are empty: one zero each.
    const float* block =
        resolved.grade.data.data() + resolved.grade.entries[0].data_offset;
    for (const char* name : kHueCurveNames) {
        if (curve == name) {
            resolved.block = block;
            return;
        }
        block += 1 + (3 * static_cast<ptrdiff_t>(block[0]));
    }
}

// The curve at `x`, as the renderer evaluates it; 1/2 without a point.
inline double HueCurveValueAt(const std::vector<ClipEffect>& effects,
                              const std::string& curve, double x) {
    ResolvedHueCurve resolved;
    ResolveHueCurve(effects, curve, resolved);
    return resolved.block == nullptr
               ? 0.5
               : EvaluateHueCurve(resolved.block, static_cast<float>(x));
}

// The curve at `count` evenly spaced x from 0 to 1, for the view to stroke.
inline std::vector<double> SampleHueCurve(
    const std::vector<ClipEffect>& effects, const std::string& curve,
    int32_t count) {
    std::vector<double> samples(static_cast<size_t>(std::max(count, 0)), 0.5);
    ResolvedHueCurve resolved;
    ResolveHueCurve(effects, curve, resolved);
    if (resolved.block == nullptr) {
        return samples;
    }
    for (int32_t index = 0; index < count; ++index) {
        const float x = count > 1 ? static_cast<float>(index) /
                                        static_cast<float>(count - 1)
                                  : 0.0f;
        // The last sample of a curve that goes round is the first again.
        samples[static_cast<size_t>(index)] = EvaluateHueCurve(
            resolved.block, IsHueIndexedCurve(curve) && x >= 1.0f ? 0.0f : x);
    }
    return samples;
}

// The stack with `points` as the curve. Fewer than two is no curve: it is
// cleared, and a stage left without a point goes.
inline std::vector<ClipEffect> WithHueCurvePoints(
    const std::vector<ClipEffect>& effects, const std::string& curve,
    const std::vector<ColorCurvePoint>& points) {
    const bool none = points.size() < 2;
    if (none && FindFirstEffect(effects, "color.hue_curves") == nullptr) {
        return effects;
    }
    std::vector<ClipEffect> result = effects;
    ClipEffect& stage = GradeStage(result, "color.hue_curves");
    SetCurveChannelPoints(stage, curve,
                          none ? std::vector<ColorCurvePoint>{} : points);
    if (stage.params.empty()) {
        result.erase(result.begin() + (&stage - result.data()));
    }
    return result;
}

// The largest x a point of the curve may take: one step short of 1 along
// the hue, where 1 is 0 again.
inline double HueCurveMaxX(const std::string& curve) {
    return IsHueIndexedCurve(curve) ? 1.0 - (1.0 / kCurvePrecision) : 1.0;
}

// The point of the curve within reach of (x, y), the nearest when several
// are; -1 when none is.
inline int32_t HitHueCurvePoint(const std::vector<ClipEffect>& effects,
                                const std::string& curve, double x, double y,
                                double reachX, double reachY) {
    const std::vector<ColorCurvePoint> points = HueCurvePoints(effects, curve);
    int32_t hit = -1;
    double best = 0.0;
    for (size_t index = 0; index < points.size(); ++index) {
        const double dx = std::abs(CurveCoordinate(points[index].x) - x);
        const double dy = std::abs(CurveCoordinate(points[index].y) - y);
        if (dx > reachX || dy > reachY || reachX <= 0.0 || reachY <= 0.0) {
            continue;
        }
        const double distance = std::hypot(dx / reachX, dy / reachY);
        if (hit < 0 || distance < best) {
            hit = static_cast<int32_t>(index);
            best = distance;
        }
    }
    return hit;
}

// `points` with one more at x, kept in increasing x; the index it lands at,
// or that of the point already holding that x, which is then left as it is.
inline int32_t InsertHueCurvePoint(std::vector<ColorCurvePoint>& points,
                                   double x, double y) {
    const EffectParamValue fx = CurveFraction(x);
    const double placed = CurveCoordinate(fx);
    size_t at = 0;
    while (at < points.size() &&
           CurveCoordinate(points[at].x) < placed - (0.5 / kCurvePrecision)) {
        ++at;
    }
    if (at < points.size() && std::abs(CurveCoordinate(points[at].x) - placed) <
                                  0.5 / kCurvePrecision) {
        return static_cast<int32_t>(at);
    }
    points.insert(points.begin() + static_cast<ptrdiff_t>(at),
                  {fx, CurveFraction(y)});
    return static_cast<int32_t>(at);
}

// A point added at x: at y, or on the curve when `keepShape` (the Shift
// click). On a curve without a point, the point and its two anchors at
// neutral (see the file comment). On an x a point already holds, that point
// is the one held and nothing is added.
inline CurveEdit WithHueCurvePointAdded(const std::vector<ClipEffect>& effects,
                                        const std::string& curve, double x,
                                        double y, bool keepShape) {
    std::vector<ColorCurvePoint> points = HueCurvePoints(effects, curve);
    const bool periodic = IsHueIndexedCurve(curve);
    x = std::clamp(x, 0.0, HueCurveMaxX(curve));
    y = std::clamp(keepShape ? HueCurveValueAt(effects, curve, x) : y, 0.0,
                   1.0);
    const bool first = points.empty();
    if (!first) {
        const double placed = CurveCoordinate(CurveFraction(x));
        for (size_t index = 0; index < points.size(); ++index) {
            if (std::abs(CurveCoordinate(points[index].x) - placed) <
                0.5 / kCurvePrecision) {
                return {effects, static_cast<int32_t>(index), {}};
            }
        }
        if (points.size() >= static_cast<size_t>(kMaxColorCurvePoints)) {
            return {effects, -1,
                    "Une courbe porte " + std::to_string(kMaxColorCurvePoints) +
                        " points au plus."};
        }
    } else if (periodic) {
        // The anchors go round with the hue.
        for (const double anchor : {x - kHueCurveBand, x + kHueCurveBand}) {
            InsertHueCurvePoint(points, anchor - std::floor(anchor), 0.5);
        }
    } else {
        InsertHueCurvePoint(points, 0.0, 0.5);
        InsertHueCurvePoint(points, 1.0, 0.5);
    }
    const int32_t index = InsertHueCurvePoint(points, x, y);
    // On an end of a new curve along the luma or the saturation, the end
    // takes the value clicked.
    points[static_cast<size_t>(index)].y = CurveFraction(y);
    return {WithHueCurvePoints(effects, curve, points), index, {}};
}

// The point `index` moved to (x, y); to neutral at its x when `snap` (the
// Option drag). It stays between its neighbours, one step short of each, and
// within the curve's x.
inline CurveEdit WithHueCurvePointMoved(const std::vector<ClipEffect>& effects,
                                        const std::string& curve, int32_t index,
                                        double x, double y, bool snap) {
    std::vector<ColorCurvePoint> points = HueCurvePoints(effects, curve);
    if (index < 0 || static_cast<size_t>(index) >= points.size()) {
        return {effects, -1, {}};
    }
    const auto at = static_cast<size_t>(index);
    constexpr double kStep = 1.0 / kCurvePrecision;
    const double low =
        at == 0 ? 0.0
                : (std::floor(
                       (CurveCoordinate(points[at - 1].x) * kCurvePrecision) +
                       1e-9) /
                   kCurvePrecision) +
                      kStep;
    const double high =
        at + 1 == points.size()
            ? HueCurveMaxX(curve)
            : (std::ceil((CurveCoordinate(points[at + 1].x) * kCurvePrecision) -
                         1e-9) /
               kCurvePrecision) -
                  kStep;
    if (low <= high) {
        points[at].x = CurveFraction(std::clamp(x, low, high));
    }
    points[at].y = CurveFraction(snap ? 0.5 : y);
    return {WithHueCurvePoints(effects, curve, points), index, {}};
}

// The point `index` removed (the right click). A curve left with one point
// is cleared.
inline CurveEdit WithHueCurvePointRemoved(
    const std::vector<ClipEffect>& effects, const std::string& curve,
    int32_t index) {
    std::vector<ColorCurvePoint> points = HueCurvePoints(effects, curve);
    if (index < 0 || static_cast<size_t>(index) >= points.size()) {
        return {effects, -1, {}};
    }
    points.erase(points.begin() + index);
    return {WithHueCurvePoints(effects, curve, points), -1, {}};
}

// One curve back to neutral.
inline std::vector<ClipEffect> WithHueCurveReset(
    const std::vector<ClipEffect>& effects, const std::string& curve) {
    return WithHueCurvePoints(effects, curve, {});
}

// A colour patch: three points on a curve along the hue, at the vector's hue
// and a band either side, each on the curve as it is -- so the picture does
// not change, and the middle one is there to be moved. The index returned is
// the middle point's. Where a point already stands, it is kept.
inline CurveEdit WithHueCurveVector(const std::vector<ClipEffect>& effects,
                                    const std::string& curve, size_t vector) {
    if (!IsHueIndexedCurve(curve) || vector >= HueCurveVectors().size()) {
        return {effects, -1, {}};
    }
    std::vector<ColorCurvePoint> points = HueCurvePoints(effects, curve);
    const double hue = HueCurveVectors().at(vector).hue;
    int32_t middle = -1;
    for (const double offset : {-kHueCurveBand, kHueCurveBand, 0.0}) {
        const double at = std::min(hue + offset - std::floor(hue + offset),
                                   HueCurveMaxX(curve));
        const bool taken = std::any_of(
            points.begin(), points.end(), [at](const ColorCurvePoint& point) {
                return std::abs(CurveCoordinate(point.x) -
                                CurveCoordinate(CurveFraction(at))) <
                       0.5 / kCurvePrecision;
            });
        if (!taken &&
            points.size() >= static_cast<size_t>(kMaxColorCurvePoints)) {
            return {effects, -1,
                    "Une courbe porte " + std::to_string(kMaxColorCurvePoints) +
                        " points au plus."};
        }
        middle = InsertHueCurvePoint(points, at,
                                     HueCurveValueAt(effects, curve, at));
    }
    // The middle point was inserted last: its index is the one to hold.
    return {WithHueCurvePoints(effects, curve, points), middle, {}};
}

// --- Geometry ---------------------------------------------------------------

// The editor in a block `width` wide, rows from the top: the bar that
// chooses the curve, a row of six colour patches and the reset, the graph.
struct GradeHueCurveLayout {
    GradeRect bar;
    std::array<GradeRect, 6> patches;
    GradeRect reset;
    GradeRect graph;
    double height = 0.0;
};

inline GradeHueCurveLayout LayoutGradeHueCurve(double width) {
    using namespace ui::theme;
    constexpr double kMargin = kGradeCurvePointRadius + kSpaceXxs;
    GradeHueCurveLayout layout;
    layout.bar = {0.0, 0.0, width, kGradeCurveBarHeight};
    double y = kGradeCurveBarHeight + kSpaceXs;
    for (size_t index = 0; index < layout.patches.size(); ++index) {
        layout.patches.at(index) = {kMargin + (static_cast<double>(index) *
                                               (kGradeHuePatchSize + kSpaceS)),
                                    y, kGradeHuePatchSize, kGradeHuePatchSize};
    }
    layout.reset = {std::max(0.0, width - kGradeCurveBarHeight), y,
                    kGradeCurveBarHeight, kGradeHuePatchSize};
    y += kGradeHuePatchSize + kSpaceXs + kMargin;
    layout.graph = {kMargin, y, std::max(0.0, width - (2.0 * kMargin)),
                    kGradeHueCurveGraphHeight};
    layout.height = y + kGradeHueCurveGraphHeight + kMargin;
    return layout;
}

}  // namespace ui::inspector
