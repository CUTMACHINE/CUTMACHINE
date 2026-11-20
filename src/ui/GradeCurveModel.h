#pragma once

// GR3 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The curve editor of the
// Inspector, as policy: the points a channel shows, the one under the
// pointer, and the stack a gesture gives. Portable, no framework:
// GradeCurveView.mm draws what this says and turns mouse events into calls
// here (see GradeWheelModel.h, its sibling).
//
// A channel of the clip's first color.curves stage (ColorEffects.h) is a
// list of 2 to kMaxColorCurvePoints points, or none: the identity. The
// editor shows an empty channel as the two ends of the diagonal, (0, 0) and
// (1, 1), and writes nothing until a gesture moves away from it; a channel
// brought back to those two ends is written as empty again, and a stage left
// without a point is removed, so that a clip whose curves were all reset
// reaches the export as one that never had any.
//
// A point an agent wrote keeps its exact fraction until a gesture moves that
// point: the others are rewritten as they were read. What a gesture places
// is a fraction of kCurvePrecision. The curve drawn is the one rendered --
// both go through ResolveColorGrade and EvaluateColorCurve -- so no second
// interpolation lives here.
//
// The gestures are those of the application the timelines go to, from its
// manual (Curves, "Adjusting Curves Using the Mouse"): a click adds a point
// where it lands, and the curve follows; with Shift the point is added on
// the curve, which keeps its shape; dragging with Option snaps the point to
// the neutral diagonal; a right click on a point removes it. The two end
// points move like any other -- the black point and the white point, a lift
// and a gain by the curve -- but are not removed.
//
// What the engine refuses, the model refuses first, by name: a seventeenth
// point on a channel, and a point moved past its neighbour (it stops one
// step short of it instead).

#include "ColorEffects.h"
#include "Document.h"
#include "GradeWheelModel.h"
#include "InspectorGradeControls.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace ui::inspector {

// A coordinate is committed as a fraction of this.
inline constexpr int32_t kCurvePrecision = 1000;

inline double CurveCoordinate(const EffectParamValue& value) {
    return value.den > 0
               ? static_cast<double>(value.num) / static_cast<double>(value.den)
               : 0.0;
}

// `value` in [0, 1], as the exact fraction the editor commits.
inline EffectParamValue CurveFraction(double value) {
    EffectParamValue fraction{0, 1};
    ReducedFraction(std::llround(std::clamp(value, 0.0, 1.0) * kCurvePrecision),
                    kCurvePrecision, fraction);
    return fraction;
}

// The points the editor shows for a channel: its own, or the two ends of
// the diagonal when it has none (or does not read as a curve, which only an
// unvalidated stack can).
inline std::vector<ColorCurvePoint> ShownCurvePoints(
    const std::vector<ClipEffect>& effects, const std::string& channel) {
    std::vector<ColorCurvePoint> points;
    std::string ignored;
    const ClipEffect* curves = FindFirstEffect(effects, "color.curves");
    if (curves != nullptr) {
        if (!ReadColorCurveChannel(*curves, channel, points, ignored)) {
            points.clear();
        }
    }
    if (points.empty()) {
        points = {{{0, 1}, {0, 1}}, {{1, 1}, {1, 1}}};
    }
    return points;
}

// Whether the channel holds points of its own.
inline bool HasCurvePoints(const std::vector<ClipEffect>& effects,
                           const std::string& channel) {
    std::vector<ColorCurvePoint> points;
    std::string ignored;
    const ClipEffect* curves = FindFirstEffect(effects, "color.curves");
    return curves != nullptr &&
           ReadColorCurveChannel(*curves, channel, points, ignored) &&
           !points.empty();
}

// One channel alone, as the renderer resolves it: its points taken as a
// master curve through ResolveColorGrade. Nothing when the channel has no
// point.
inline ResolvedColorGrade ResolveCurveChannel(
    const std::vector<ClipEffect>& effects, const std::string& channel) {
    const ClipEffect* curves = FindFirstEffect(effects, "color.curves");
    if (curves == nullptr) {
        return {};
    }
    ClipEffect alone{curves->id, "color.curves", {}};
    const std::string prefix = channel + "_";
    for (const auto& [name, value] : curves->params) {
        if (name.rfind(prefix, 0) == 0) {
            alone.params["master_" + name.substr(prefix.size())] = value;
        }
    }
    // A curve reads no context: only color.temperature does.
    return ResolveColorGrade({alone}, {});
}

// The channel's curve at `x`, as the renderer evaluates it.
inline double CurveValueAt(const std::vector<ClipEffect>& effects,
                           const std::string& channel, double x) {
    const auto input = static_cast<float>(x);
    return ApplyResolvedColorGrade({input, input, input},
                                   ResolveCurveChannel(effects, channel))[0];
}

// The curve at `count` evenly spaced x from 0 to 1, for the view to stroke.
inline std::vector<double> SampleCurve(const std::vector<ClipEffect>& effects,
                                       const std::string& channel,
                                       int32_t count) {
    const ResolvedColorGrade grade = ResolveCurveChannel(effects, channel);
    std::vector<double> samples;
    for (int32_t index = 0; index < count; ++index) {
        const auto x = count > 1 ? static_cast<float>(index) /
                                       static_cast<float>(count - 1)
                                 : 0.0f;
        samples.push_back(ApplyResolvedColorGrade({x, x, x}, grade)[0]);
    }
    return samples;
}

// The params of one channel of a color.curves stage replaced by `points`,
// numbered from 0 without a gap, as the stage is read.
inline void SetCurveChannelPoints(ClipEffect& stage, const std::string& channel,
                                  const std::vector<ColorCurvePoint>& points) {
    const std::string prefix = channel + "_";
    for (auto it = stage.params.begin(); it != stage.params.end();) {
        it = it->first.rfind(prefix, 0) == 0 ? stage.params.erase(it)
                                             : std::next(it);
    }
    for (size_t index = 0; index < points.size(); ++index) {
        const std::string number = std::to_string(index);
        std::string name = prefix;
        name.append("x").append(number);
        stage.params[name] = points[index].x;
        name = prefix;
        name.append("y").append(number);
        stage.params[name] = points[index].y;
    }
}

// The stack with `points` as the channel's curve. The identity -- no point,
// or the two ends of the diagonal alone -- is written as an empty channel.
inline std::vector<ClipEffect> WithCurvePoints(
    const std::vector<ClipEffect>& effects, const std::string& channel,
    const std::vector<ColorCurvePoint>& points) {
    const auto is = [](const EffectParamValue& value, int32_t whole) {
        return value.den > 0 && static_cast<int64_t>(value.num) ==
                                    static_cast<int64_t>(whole) * value.den;
    };
    const bool identity =
        points.empty() ||
        (points.size() == 2 && is(points[0].x, 0) && is(points[0].y, 0) &&
         is(points[1].x, 1) && is(points[1].y, 1));
    if (identity && FindFirstEffect(effects, "color.curves") == nullptr) {
        return effects;
    }
    std::vector<ClipEffect> result = effects;
    ClipEffect& stage = GradeStage(result, "color.curves");
    SetCurveChannelPoints(stage, channel,
                          identity ? std::vector<ColorCurvePoint>{} : points);
    if (stage.params.empty()) {
        result.erase(result.begin() + (&stage - result.data()));
    }
    return result;
}

// The point of the channel within (`reachX`, `reachY`) of (x, y), the
// nearest when several are; -1 when none is. The reaches are in the curve's
// units: the view converts its grab distance in points.
inline int32_t HitCurvePoint(const std::vector<ClipEffect>& effects,
                             const std::string& channel, double x, double y,
                             double reachX, double reachY) {
    const std::vector<ColorCurvePoint> points =
        ShownCurvePoints(effects, channel);
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

// What a gesture on a curve gives: the stack, the point the gesture holds
// (its index in the channel as the stack now has it; -1 when it holds
// none), and, when it was refused, why -- in the app's language, for the
// view to show. A refused gesture returns the stack unchanged.
struct CurveEdit {
    std::vector<ClipEffect> effects;
    int32_t index = -1;
    std::string refusal;
};

// A point added at x: at y, or on the curve when `keepShape` (the Shift
// click). On an x a point already holds, that point is the one held and
// nothing is added.
inline CurveEdit WithCurvePointAdded(const std::vector<ClipEffect>& effects,
                                     const std::string& channel, double x,
                                     double y, bool keepShape) {
    std::vector<ColorCurvePoint> points = ShownCurvePoints(effects, channel);
    const EffectParamValue fx = CurveFraction(x);
    const double placed = CurveCoordinate(fx);
    size_t at = 0;
    while (at < points.size() &&
           CurveCoordinate(points[at].x) < placed - 0.5 / kCurvePrecision) {
        ++at;
    }
    if (at < points.size() && std::abs(CurveCoordinate(points[at].x) - placed) <
                                  0.5 / kCurvePrecision) {
        return {effects, static_cast<int32_t>(at), {}};
    }
    if (points.size() >= static_cast<size_t>(kMaxColorCurvePoints)) {
        return {effects, -1,
                "Une courbe porte " + std::to_string(kMaxColorCurvePoints) +
                    " points au plus."};
    }
    const EffectParamValue fy =
        CurveFraction(keepShape ? CurveValueAt(effects, channel, placed) : y);
    points.insert(points.begin() + static_cast<ptrdiff_t>(at), {fx, fy});
    return {WithCurvePoints(effects, channel, points),
            static_cast<int32_t>(at),
            {}};
}

// The point `index` moved to (x, y); onto the diagonal at its x when `snap`
// (the Option drag). It stays between its neighbours, one step short of
// each, and within [0, 1].
inline CurveEdit WithCurvePointMoved(const std::vector<ClipEffect>& effects,
                                     const std::string& channel, int32_t index,
                                     double x, double y, bool snap) {
    std::vector<ColorCurvePoint> points = ShownCurvePoints(effects, channel);
    if (index < 0 || static_cast<size_t>(index) >= points.size()) {
        return {effects, -1, {}};
    }
    const auto at = static_cast<size_t>(index);
    constexpr double kStep = 1.0 / kCurvePrecision;
    // The neighbours may sit off the editor's steps (an agent's 1/3): the
    // bound is the first step past them.
    const double low =
        at == 0
            ? 0.0
            : std::floor(CurveCoordinate(points[at - 1].x) * kCurvePrecision +
                         1e-9) /
                      kCurvePrecision +
                  kStep;
    const double high =
        at + 1 == points.size()
            ? 1.0
            : std::ceil(CurveCoordinate(points[at + 1].x) * kCurvePrecision -
                        1e-9) /
                      kCurvePrecision -
                  kStep;
    if (low > high) {
        // No step is free between the neighbours: the point keeps its x.
        x = CurveCoordinate(points[at].x);
        points[at].y = CurveFraction(snap ? x : y);
    } else {
        points[at].x = CurveFraction(std::clamp(x, low, high));
        points[at].y = CurveFraction(snap ? CurveCoordinate(points[at].x) : y);
    }
    std::vector<ClipEffect> moved = WithCurvePoints(effects, channel, points);
    // Back on the identity, the channel is empty again and shows its two
    // ends: the point held is still the one at this place.
    return {std::move(moved), index, {}};
}

// The point `index` removed (the right click). The two ends stay.
inline CurveEdit WithCurvePointRemoved(const std::vector<ClipEffect>& effects,
                                       const std::string& channel,
                                       int32_t index) {
    std::vector<ColorCurvePoint> points = ShownCurvePoints(effects, channel);
    if (index < 0 || static_cast<size_t>(index) >= points.size()) {
        return {effects, -1, {}};
    }
    if (index == 0 || static_cast<size_t>(index) + 1 == points.size()) {
        return {effects, -1,
                "Les deux extrémités d'une courbe se déplacent, elles ne se "
                "retirent pas."};
    }
    points.erase(points.begin() + index);
    return {WithCurvePoints(effects, channel, points), -1, {}};
}

// One channel back to the identity.
inline std::vector<ClipEffect> WithCurveChannelReset(
    const std::vector<ClipEffect>& effects, const std::string& channel) {
    return WithCurvePoints(effects, channel, {});
}

// Every channel back to the identity: the stage goes.
inline std::vector<ClipEffect> WithCurvesReset(
    const std::vector<ClipEffect>& effects) {
    std::vector<ClipEffect> result = effects;
    for (const char* channel : kColorCurveChannels) {
        result = WithCurvePoints(result, channel, {});
    }
    return result;
}

// The curve of `from` given to `to` as well, point for point.
inline std::vector<ClipEffect> WithCurveChannelCopied(
    const std::vector<ClipEffect>& effects, const std::string& from,
    const std::string& to) {
    if (from == to) {
        return effects;
    }
    return WithCurvePoints(effects, to,
                           HasCurvePoints(effects, from)
                               ? ShownCurvePoints(effects, from)
                               : std::vector<ColorCurvePoint>{});
}

// Three points added on the curve at 1/4, 1/2 and 3/4 -- shadows, midtones
// and highlights to hold or to move -- where the channel has room for them.
// A channel still on the identity gets the five points of the diagonal, the
// one case where the identity is written out: the anchors are the point.
inline CurveEdit WithCurveAnchors(const std::vector<ClipEffect>& effects,
                                  const std::string& channel) {
    std::vector<ColorCurvePoint> points = ShownCurvePoints(effects, channel);
    for (const int32_t quarter : kCurveGridQuarters) {
        const double x = static_cast<double>(quarter) / 4.0;
        const bool taken = std::any_of(
            points.begin(), points.end(), [x](const ColorCurvePoint& point) {
                return std::abs(CurveCoordinate(point.x) - x) <
                       0.5 / kCurvePrecision;
            });
        if (taken) {
            continue;
        }
        if (points.size() >= static_cast<size_t>(kMaxColorCurvePoints)) {
            return {effects, -1,
                    "Une courbe porte " + std::to_string(kMaxColorCurvePoints) +
                        " points au plus."};
        }
        const ColorCurvePoint anchor{
            CurveFraction(x), CurveFraction(CurveValueAt(effects, channel, x))};
        points.insert(std::find_if(points.begin(), points.end(),
                                   [x](const ColorCurvePoint& point) {
                                       return CurveCoordinate(point.x) > x;
                                   }),
                      anchor);
    }
    // Written as they are, identity included.
    std::vector<ClipEffect> result = effects;
    SetCurveChannelPoints(GradeStage(result, "color.curves"), channel, points);
    return {std::move(result), -1, {}};
}

// --- Geometry ---------------------------------------------------------------

// The editor in a block `width` wide, rows from the top: a bar of controls
// (the channels, the anchors, the reset), then the graph. The graph keeps a
// margin on every side: its end points sit on its corners, and a point
// drawn half outside its view is cut (seen on the first captures).
struct GradeCurveLayout {
    GradeRect bar;
    GradeRect graph;
    double height = 0.0;
};

inline GradeCurveLayout LayoutGradeCurve(double width) {
    using namespace ui::theme;
    constexpr double kMargin = kGradeCurvePointRadius + kSpaceXxs;
    GradeCurveLayout layout;
    layout.bar = {0.0, 0.0, width, kGradeCurveBarHeight};
    layout.graph = {kMargin, kGradeCurveBarHeight + kSpaceXs + kMargin,
                    std::max(0.0, width - (2.0 * kMargin)),
                    kGradeCurveGraphHeight};
    layout.height = layout.graph.y + layout.graph.height + kMargin;
    return layout;
}

// A point of the graph in the curve's units: x from the left, y from the
// bottom, both 0 to 1 across the graph, not clamped.
inline std::array<double, 2> CurvePointInUnits(const GradeRect& graph,
                                               double px, double py) {
    if (graph.width <= 0.0 || graph.height <= 0.0) {
        return {0.0, 0.0};
    }
    return {(px - graph.x) / graph.width,
            ((graph.y + graph.height) - py) / graph.height};
}

// And back: where (x, y) of the curve is drawn.
inline std::array<double, 2> CurvePointInGraph(const GradeRect& graph, double x,
                                               double y) {
    return {graph.x + (x * graph.width),
            graph.y + graph.height - (y * graph.height)};
}

}  // namespace ui::inspector
