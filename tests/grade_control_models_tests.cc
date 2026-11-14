// GR3 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The colour wheels and the
// curve editor of the Inspector, as policy: every gesture is checked on the
// stack it gives, without a window.

#include "GradeCurveModel.h"
#include "GradeHueCurveModel.h"
#include "GradeWheelModel.h"
#include "GradeWorkspaceLayout.h"
#include "VideoScopes.h"

#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using ui::inspector::CurveCoordinate;
using ui::inspector::CurveEdit;
using ui::inspector::GradeWheel;
using ui::inspector::GradeWheels;
using ui::inspector::GradeWheelSpec;
using ui::inspector::GradeWheelState;
using ui::inspector::ReadGradeWheel;
using ui::inspector::ShownCurvePoints;

const GradeWheelSpec& Wheel(GradeWheel wheel) {
    return GradeWheels().at(static_cast<size_t>(wheel));
}

ClipEffect Stage(const std::string& type,
                 std::map<std::string, EffectParamValue> params) {
    ClipEffect effect;
    effect.type = type;
    effect.params = std::move(params);
    return effect;
}

// Every stage of a stack is one the document accepts.
bool Valid(const std::vector<ClipEffect>& effects, std::string& error) {
    for (const ClipEffect& effect : effects) {
        if (!ValidateColorEffect(effect, error)) {
            return false;
        }
    }
    return true;
}

std::string Types(const std::vector<ClipEffect>& effects) {
    std::string joined;
    for (const ClipEffect& effect : effects) {
        joined += effect.type + " ";
    }
    return joined;
}

double Luma(const std::array<double, 4>& knobs) {
    return (0.2126 * knobs[0]) + (0.7152 * knobs[1]) + (0.0722 * knobs[2]);
}

bool Same(const EffectParamValue& value, int32_t num, int32_t den) {
    return static_cast<int64_t>(value.num) * den ==
           static_cast<int64_t>(num) * value.den;
}

void TestWheels() {
    using namespace ui::inspector;
    const GradeWheelSpec& lift = Wheel(GradeWheel::Lift);
    const GradeWheelSpec& gain = Wheel(GradeWheel::Gain);
    const GradeWheelSpec& offset = Wheel(GradeWheel::Offset);
    std::string error;

    // --- the plane of the puck ---
    {
        const std::array<double, 3> channels =
            WheelChannelsForChroma(0.03, -0.07);
        const std::array<double, 2> chroma = WheelChromaOfChannels(channels);
        Check(std::abs(chroma[0] - 0.03) < 1e-12 &&
                  std::abs(chroma[1] + 0.07) < 1e-12 &&
                  std::abs(Luma({channels[0], channels[1], channels[2], 0.0})) <
                      1e-12,
              "a chroma becomes a channel triple of no luma, and comes back");
        // Pure red sits where the vectorscope puts it: 103 degrees from +Cb.
        const std::array<double, 2> red =
            WheelChromaOfChannels({1.0, 0.0, 0.0});
        const double angle = std::atan2(red[1], red[0]);
        Check(std::abs((angle * 180.0 / 3.14159265358979) - 102.9) < 0.2,
              "red lies at the vectorscope's angle");
        const std::array<float, 3> rim = WheelRimColor(angle);
        const std::array<float, 3> opposite =
            WheelRimColor(angle + 3.14159265358979);
        Check(rim[0] > rim[1] && rim[0] > rim[2] && opposite[0] < opposite[1] &&
                  opposite[0] < opposite[2],
              "the rim is red where the puck pushes toward red, cyan across");
        bool inside = true;
        for (int step = 0; step < 360; ++step) {
            for (const float channel : WheelRimColor(step * 0.0174533)) {
                inside = inside && channel >= 0.0f && channel <= 1.0f;
            }
        }
        Check(inside, "every rim colour is a colour");
    }

    // --- reading ---
    {
        const GradeWheelState empty = ReadGradeWheel({}, lift);
        Check(empty.x == 0.0 && empty.y == 0.0 && empty.reach == 0.0 &&
                  empty.knobs == std::array<double, 4>{},
              "a clip without a wheel shows the puck at the centre");
        Check(WithWheelPuckMoved({}, lift, 0.0, 0.0).empty() &&
                  WithWheelMasterMoved({}, lift, 0.0).empty() &&
                  WithWheelReset({}, lift).empty(),
              "a gesture that moves nothing adds no stage");
    }

    // --- the puck ---
    {
        const std::vector<ClipEffect> graded = {
            Stage("color.exposure", {{"amount", {1, 2}}}),
            Stage("color.contrast", {{"amount", {1, 5}}})};
        const std::vector<ClipEffect> pushed =
            WithWheelPuck(graded, lift, 0.5, -0.25);
        Check(Types(pushed) == "color.exposure color.wheels color.contrast ",
              "a wheel's stage goes to its canonical place: " + Types(pushed));
        const GradeWheelState state = ReadGradeWheel(pushed, lift);
        Check(std::abs(state.x - 0.5) < 0.01 && std::abs(state.y + 0.25) < 0.01,
              "the puck reads back where it was put, to the knobs' step");
        Check(std::abs(Luma(state.knobs)) < 0.001 && state.knobs[3] == 0.0 &&
                  pushed[1].params.count("lift_master") == 0 &&
                  pushed[1].params.size() == 3,
              "the puck writes the three channels around a constant luma, "
              "and nothing else");
        Check(Valid(pushed, error),
              "the stack is one the document accepts: " + error);
        // The other wheels are not read.
        Check(ReadGradeWheel(pushed, gain).reach == 0.0,
              "another wheel's puck stays at its centre");
        // Past the rim, the puck stops on it.
        const GradeWheelState far =
            ReadGradeWheel(WithWheelPuck({}, offset, 3.0, 0.0), offset);
        Check(far.reach < 1.01 && far.x > 0.98 && std::abs(far.y) < 0.01,
              "a target past the rim is brought back onto it");
    }

    // --- relative moves, on a stage an agent wrote ---
    {
        // A lift with a luma part, which is not on the puck.
        const std::vector<ClipEffect> agent = {
            Stage("color.wheels", {{"lift_r", {1, 10}},
                                   {"lift_g", {1, 10}},
                                   {"lift_b", {1, 10}},
                                   {"gamma_master", {1, 20}}})};
        Check(ReadGradeWheel(agent, lift).reach < 1e-9,
              "three equal channels are luma: the puck is at the centre");
        const std::vector<ClipEffect> moved =
            WithWheelPuckMoved(agent, lift, 0.2, 0.1);
        const GradeWheelState state = ReadGradeWheel(moved, lift);
        Check(std::abs(state.x - 0.2) < 0.01 &&
                  std::abs(state.y - 0.1) < 0.01 &&
                  std::abs(Luma(state.knobs) - 0.1) < 0.001,
              "a drag moves the chroma and leaves the luma where it was");
        Check(moved.size() == 1 &&
                  Same(moved[0].params.at("gamma_master"), 1, 20),
              "and leaves the other wheels' knobs alone");
        const std::vector<ClipEffect> returned =
            WithWheelPuckMoved(agent, lift, 0.0, 0.0);
        Check(returned.size() == 1 &&
                  returned[0].params.size() == agent[0].params.size() &&
                  Same(returned[0].params.at("lift_r"), 1, 10) &&
                  Same(returned[0].params.at("lift_g"), 1, 10) &&
                  Same(returned[0].params.at("lift_b"), 1, 10),
              "a drag that comes back to its start gives back the stack");
        // A colour past the rim is not pulled in by a gesture that asks
        // nothing.
        const std::vector<ClipEffect> beyond = {
            Stage("color.wheels", {{"lift_b", {9, 10}}})};
        const GradeWheelState shown = ReadGradeWheel(beyond, lift);
        const std::vector<ClipEffect> still =
            WithWheelPuckMoved(beyond, lift, 0.0, 0.0);
        Check(shown.reach > 1.0 &&
                  std::abs(std::hypot(shown.x, shown.y) - 1.0) < 1e-9 &&
                  Same(still[0].params.at("lift_b"), 9, 10),
              "a colour written past the rim shows on it, and a drag of "
              "nothing leaves it there");
    }

    // --- the master ---
    {
        const std::vector<ClipEffect> raised =
            WithWheelMasterMoved({}, lift, 0.05);
        Check(raised.size() == 1 &&
                  Same(raised[0].params.at("lift_master"), 1, 20) &&
                  raised[0].params.size() == 1 && Valid(raised, error),
              "the master bar writes the master knob alone: " + error);
        Check(WithWheelMasterMoved(raised, lift, -0.05).empty(),
              "a wheel brought back to neutral leaves no stage");
        const std::vector<ClipEffect> pinned =
            WithWheelMasterMoved(raised, lift, 5.0);
        Check(Same(pinned[0].params.at("lift_master"), 1, 1),
              "a knob stops at what a wheel takes");
    }

    // --- resets ---
    {
        std::vector<ClipEffect> effects = WithWheelPuck({}, gain, 0.4, 0.4);
        effects = WithWheelMasterMoved(effects, gain, 0.2);
        effects = WithWheelMasterMoved(effects, lift, 0.1);
        const std::vector<ClipEffect> colour =
            WithWheelColourReset(effects, gain);
        Check(
            ReadGradeWheel(colour, gain).reach == 0.0 &&
                std::abs(ReadGradeWheel(colour, gain).knobs[3] - 0.2) < 1e-12 &&
                colour[0].params.count("gain_r") == 0,
            "a double click resets the colour and keeps the master");
        const std::vector<ClipEffect> both = WithWheelReset(effects, gain);
        Check(both.size() == 1 && both[0].params.size() == 1 &&
                  Same(both[0].params.at("lift_master"), 1, 10),
              "the reset control resets the wheel, and only that wheel");
        Check(WithWheelReset(both, lift).empty(),
              "the last wheel reset takes the stage with it");
    }

    // --- the fields ---
    {
        double knob = 0.0;
        Check(WheelShownValue(gain, 0.0) == 1.0 &&
                  WheelShownValue(gain, 1.0) == 2.0 &&
                  WheelShownValue(lift, 0.05) == 0.05,
              "the gain reads as the factor it is, the others as stored");
        Check(WheelKnobForShown(gain, 2.0, knob) && knob == 1.0 &&
                  WheelKnobForShown(gain, 1.0, knob) && knob == 0.0 &&
                  !WheelKnobForShown(gain, 0.0, knob) &&
                  !WheelKnobForShown(gain, -1.0, knob) &&
                  WheelKnobForShown(offset, -0.03, knob) && knob == -0.03 &&
                  !WheelKnobForShown(lift, std::nan(""), knob),
              "a typed gain is a positive factor; a typed lift is the knob");
        const std::vector<ClipEffect> typed =
            WithWheelKnob({}, offset, 1, 0.035);
        Check(typed.size() == 1 && Same(typed[0].params.at("offset_g"), 7, 200),
              "a field commits its knob as an exact fraction");
    }

    // --- geometry ---
    {
        Check(WheelDragDelta(40.0, 50.0) == 0.2 &&
                  WheelDragDelta(40.0, 0.0) == 0.0,
              "a drag moves the puck by a quarter of the pointer's travel");
        const GradeWheelLayout layout = LayoutGradeWheel(134.0);
        const double centre = layout.ring.x + (layout.ring.width / 2.0);
        Check(std::abs(centre - 67.0) < 1e-9 &&
                  layout.ring.y >= layout.title.y + layout.title.height &&
                  layout.fields[0].y >= layout.ring.y + layout.ring.height &&
                  layout.master.y >=
                      layout.fields[0].y + layout.fields[0].height &&
                  layout.height == GradeWheelHeight() &&
                  layout.fields[3].x + layout.fields[3].width <= 134.0 + 1e-9 &&
                  layout.fields[1].x >=
                      layout.fields[0].x + layout.fields[0].width,
              "the ring is centred under its title, the fields and the "
              "master bar under it, none over another");
        const double travel = WheelTravelRadius(layout.ring);
        const double middle = layout.ring.y + (layout.ring.height / 2.0);
        const std::array<double, 2> top =
            WheelPointInRims(layout.ring, centre, middle - travel);
        const std::array<double, 2> drawn =
            WheelPointInRing(layout.ring, 0.5, -0.25);
        const std::array<double, 2> read =
            WheelPointInRims(layout.ring, drawn[0], drawn[1]);
        Check(travel > 0.0 && travel < layout.ring.width / 2.0 &&
                  std::abs(top[0]) < 1e-9 && std::abs(top[1] - 1.0) < 1e-9 &&
                  std::abs(read[0] - 0.5) < 1e-9 &&
                  std::abs(read[1] + 0.25) < 1e-9 &&
                  WheelDiscContains(layout.ring, centre, layout.ring.y + 1.0) &&
                  !WheelDiscContains(layout.ring, layout.ring.x + 1.0,
                                     layout.ring.y + 1.0),
              "up is +Cr, one rim is the inside of the painted ring, the "
              "puck is drawn where it is read, and the square's corner is "
              "outside the disc");
        // A stage carries an identifier: the same gesture made twice from
        // nothing gives two stages, and a gesture on a stage keeps its own.
        const std::vector<ClipEffect> one =
            WithWheelMasterMoved({}, lift, 0.05);
        Check(
            SameGradeStack(one, std::vector<ClipEffect>(one)) &&
                SameGradeStack({}, {}) && !SameGradeStack(one, {}) &&
                SameGradeStack(one, WithWheelPuckMoved(one, lift, 0.0, 0.0)) &&
                !SameGradeStack(one, WithWheelMasterMoved(one, lift, 0.001)) &&
                !SameGradeStack(one, WithWheelMasterMoved({}, lift, 0.05)),
            "two stacks are the same edit when their stages, and every "
            "knob's exact value, are");
        const GradeWheelGridLayout grid = LayoutGradeWheelGrid(276.0);
        Check(grid.cells[1].x >= grid.cells[0].x + grid.cells[0].width &&
                  grid.cells[2].y >= grid.cells[0].y + grid.cells[0].height &&
                  grid.cells[3].x + grid.cells[3].width <= 276.0 + 1e-9 &&
                  grid.height >= grid.cells[3].y + grid.cells[3].height,
              "four wheels in two rows of two");
    }
}

void TestCurves() {
    using namespace ui::inspector;
    std::string error;

    // --- an empty channel ---
    {
        const std::vector<ColorCurvePoint> shown = ShownCurvePoints({}, "r");
        Check(shown.size() == 2 && Same(shown[0].x, 0, 1) &&
                  Same(shown[0].y, 0, 1) && Same(shown[1].x, 1, 1) &&
                  Same(shown[1].y, 1, 1) && !HasCurvePoints({}, "r"),
              "a channel without a point shows the two ends of the diagonal");
        const std::vector<double> samples = SampleCurve({}, "master", 5);
        Check(samples.size() == 5 && samples[0] == 0.0 && samples[2] == 0.5 &&
                  samples[4] == 1.0,
              "and draws as the diagonal");
    }

    // --- adding ---
    const std::vector<ClipEffect> graded = {
        Stage("color.contrast", {{"amount", {1, 5}}}),
        Stage("color.saturation", {{"amount", {1, 10}}})};
    const CurveEdit added =
        WithCurvePointAdded(graded, "master", 0.5, 0.6, false);
    {
        Check(added.index == 1 && added.refusal.empty() &&
                  Types(added.effects) ==
                      "color.contrast color.curves color.saturation " &&
                  Valid(added.effects, error),
              "a click adds a point, the stage at its canonical place: " +
                  Types(added.effects) + error);
        const std::vector<ColorCurvePoint> points =
            ShownCurvePoints(added.effects, "master");
        Check(points.size() == 3 && Same(points[1].x, 1, 2) &&
                  Same(points[1].y, 3, 5) &&
                  std::abs(CurveValueAt(added.effects, "master", 0.5) - 0.6) <
                      1e-6,
              "the curve passes where the click landed");
        // With Shift, the point lands on the curve.
        const double before = CurveValueAt(added.effects, "master", 0.25);
        const CurveEdit kept =
            WithCurvePointAdded(added.effects, "master", 0.25, 0.9, true);
        double worst = 0.0;
        for (int step = 0; step <= 20; ++step) {
            const double x = step / 20.0;
            worst = std::max(
                worst, std::abs(CurveValueAt(kept.effects, "master", x) -
                                CurveValueAt(added.effects, "master", x)));
        }
        Check(kept.index == 1 &&
                  std::abs(CurveCoordinate(
                               ShownCurvePoints(kept.effects, "master")[1].y) -
                           before) < 0.0006 &&
                  worst < 0.01,
              "a Shift click adds a point on the curve, which keeps its "
              "shape (worst " +
                  std::to_string(worst) + ")");
        // On a point that is there, nothing is added.
        const CurveEdit again =
            WithCurvePointAdded(added.effects, "master", 0.5, 0.1, false);
        Check(again.index == 1 &&
                  ShownCurvePoints(again.effects, "master").size() == 3 &&
                  Same(ShownCurvePoints(again.effects, "master")[1].y, 3, 5),
              "a click at a point's x holds that point");
    }

    // --- the seventeenth point ---
    {
        std::vector<ClipEffect> full;
        for (int index = 1; index <= 14; ++index) {
            full = WithCurvePointAdded(full, "g", index / 15.0, index / 15.0,
                                       false)
                       .effects;
        }
        Check(ShownCurvePoints(full, "g").size() == 16 && Valid(full, error),
              "sixteen points are a curve: " + error);
        const CurveEdit refused =
            WithCurvePointAdded(full, "g", 0.99, 0.5, false);
        Check(refused.index == -1 &&
                  refused.refusal == "Une courbe porte 16 points au plus." &&
                  ShownCurvePoints(refused.effects, "g").size() == 16,
              "a seventeenth is refused, by name, and the stack is as it "
              "was: " +
                  refused.refusal);
    }

    // --- moving ---
    {
        const CurveEdit moved =
            WithCurvePointMoved(added.effects, "master", 1, 0.3, 0.45, false);
        const std::vector<ColorCurvePoint> points =
            ShownCurvePoints(moved.effects, "master");
        Check(moved.index == 1 && Same(points[1].x, 3, 10) &&
                  Same(points[1].y, 9, 20) && Valid(moved.effects, error),
              "a drag moves the point: " + error);
        // It does not pass a neighbour, nor leave the graph.
        const std::vector<ColorCurvePoint> past = ShownCurvePoints(
            WithCurvePointMoved(added.effects, "master", 1, 1.7, 1.4, false)
                .effects,
            "master");
        Check(Same(past[1].x, 999, 1000) && Same(past[1].y, 1, 1) &&
                  Valid(WithCurvePointMoved(added.effects, "master", 1, 1.7,
                                            1.4, false)
                            .effects,
                        error),
              "a point stops one step short of its neighbour, and at the "
              "top of the graph: " +
                  error);
        const std::vector<ColorCurvePoint> low = ShownCurvePoints(
            WithCurvePointMoved(added.effects, "master", 1, -3.0, -2.0, false)
                .effects,
            "master");
        Check(Same(low[1].x, 1, 1000) && Same(low[1].y, 0, 1),
              "and one step after the one before it");
        // With Option, onto the diagonal.
        const std::vector<ClipEffect> snapped =
            WithCurvePointMoved(added.effects, "master", 1, 0.4, 0.9, true)
                .effects;
        Check(Same(ShownCurvePoints(snapped, "master")[1].y, 2, 5),
              "an Option drag snaps the point to the neutral diagonal");
        // The black point: a lift by the curve.
        const std::vector<ClipEffect> black =
            WithCurvePointMoved({}, "b", 0, 0.1, 0.0, false).effects;
        const std::vector<ColorCurvePoint> ends = ShownCurvePoints(black, "b");
        Check(ends.size() == 2 && Same(ends[0].x, 1, 10) &&
                  Same(ends[0].y, 0, 1) && HasCurvePoints(black, "b") &&
                  Valid(black, error),
              "the black point moves like any other: " + error);
        Check(
            WithCurvePointMoved(black, "b", 0, 0.0, 0.0, false).effects.empty(),
            "and, back at its corner, leaves no stage");
    }

    // --- removing ---
    {
        const CurveEdit removed =
            WithCurvePointRemoved(added.effects, "master", 1);
        Check(removed.refusal.empty() &&
                  Types(removed.effects) == "color.contrast color.saturation ",
              "a right click removes the point; a curve back on the "
              "diagonal leaves no stage: " +
                  Types(removed.effects));
        const CurveEdit end = WithCurvePointRemoved(added.effects, "master", 2);
        Check(!end.refusal.empty() &&
                  ShownCurvePoints(end.effects, "master").size() == 3,
              "an end point is not removed: " + end.refusal);
    }

    // --- what an agent wrote stays as written ---
    {
        const std::vector<ClipEffect> agent = {
            Stage("color.curves", {{"g_x0", {0, 1}},
                                   {"g_y0", {0, 1}},
                                   {"g_x1", {1, 3}},
                                   {"g_y1", {2, 5}},
                                   {"g_x2", {1, 2}},
                                   {"g_y2", {1, 2}},
                                   {"g_x3", {1, 1}},
                                   {"g_y3", {1, 1}}})};
        const std::vector<ClipEffect> moved =
            WithCurvePointMoved(agent, "g", 3, 1.0, 0.9, false).effects;
        const std::vector<ColorCurvePoint> points =
            ShownCurvePoints(moved, "g");
        Check(points.size() == 4 && points[1].x.num == 1 &&
                  points[1].x.den == 3 && points[1].y.num == 2 &&
                  points[1].y.den == 5 && Same(points[3].y, 9, 10),
              "a point a gesture did not move keeps its exact fraction");
        // A neighbour off the editor's steps: the first step past it.
        const std::vector<ClipEffect> close =
            WithCurvePointMoved(agent, "g", 2, 0.1, 0.5, false).effects;
        Check(Same(ShownCurvePoints(close, "g")[2].x, 334, 1000) &&
                  Valid(close, error),
              "a point stops at the first step past a neighbour at 1/3: " +
                  error);
        // The other channels are not touched.
        const std::vector<ClipEffect> red =
            WithCurvePointAdded(agent, "r", 0.5, 0.4, false).effects;
        Check(
            red.size() == 1 && ShownCurvePoints(red, "g").size() == 4 &&
                ShownCurvePoints(red, "r").size() == 3 && Valid(red, error),
            "a gesture on a channel leaves the others as they were: " + error);
        // Reset, copy.
        Check(WithCurveChannelReset(red, "g").size() == 1 &&
                  !HasCurvePoints(WithCurveChannelReset(red, "g"), "g") &&
                  HasCurvePoints(WithCurveChannelReset(red, "g"), "r") &&
                  WithCurvesReset(red).empty(),
              "a channel is reset alone; all of them, and the stage goes");
        const std::vector<ClipEffect> copied =
            WithCurveChannelCopied(agent, "g", "b");
        const std::vector<ColorCurvePoint> blue = ShownCurvePoints(copied, "b");
        Check(
            blue.size() == 4 && blue[1].x.den == 3 && blue[1].y.den == 5 &&
                ShownCurvePoints(copied, "g").size() == 4 &&
                Valid(copied, error) &&
                !HasCurvePoints(WithCurveChannelCopied(copied, "r", "b"), "b"),
            "a channel is copied point for point; an empty one clears: " +
                error);
    }

    // --- anchors ---
    {
        const CurveEdit plain = WithCurveAnchors({}, "master");
        const std::vector<ColorCurvePoint> five =
            ShownCurvePoints(plain.effects, "master");
        Check(plain.refusal.empty() && five.size() == 5 &&
                  Same(five[1].x, 1, 4) && Same(five[1].y, 1, 4) &&
                  Same(five[3].x, 3, 4) &&
                  HasCurvePoints(plain.effects, "master") &&
                  Valid(plain.effects, error),
              "the anchors on a neutral curve are the diagonal's quarters: " +
                  error);
        const CurveEdit bent = WithCurveAnchors(added.effects, "master");
        const std::vector<ColorCurvePoint> points =
            ShownCurvePoints(bent.effects, "master");
        double worst = 0.0;
        for (int step = 0; step <= 20; ++step) {
            const double x = step / 20.0;
            worst = std::max(
                worst, std::abs(CurveValueAt(bent.effects, "master", x) -
                                CurveValueAt(added.effects, "master", x)));
        }
        Check(points.size() == 5 && Same(points[2].y, 3, 5) && worst < 0.01 &&
                  Valid(bent.effects, error),
              "on a bent curve the anchors land on it, where no point "
              "already is (worst " +
                  std::to_string(worst) + "): " + error);
    }

    // --- the point under the pointer ---
    {
        Check(HitCurvePoint(added.effects, "master", 0.51, 0.58, 0.03, 0.03) ==
                      1 &&
                  HitCurvePoint(added.effects, "master", 0.7, 0.7, 0.03,
                                0.03) == -1 &&
                  HitCurvePoint(added.effects, "master", 0.01, 0.0, 0.03,
                                0.03) == 0 &&
                  HitCurvePoint({}, "r", 0.99, 0.99, 0.03, 0.03) == 1,
              "a point is held from within its reach, the ends of a neutral "
              "curve included");
        std::vector<ClipEffect> two =
            WithCurvePointAdded({}, "r", 0.50, 0.5, false).effects;
        two = WithCurvePointAdded(two, "r", 0.52, 0.52, false).effects;
        Check(HitCurvePoint(two, "r", 0.515, 0.515, 0.03, 0.03) == 2,
              "of two points in reach, the nearest is held");
    }

    // --- geometry ---
    {
        const GradeCurveLayout layout = LayoutGradeCurve(276.0);
        const double quarter = layout.graph.width / 4.0;
        const std::array<double, 2> unit =
            CurvePointInUnits(layout.graph, layout.graph.x + quarter,
                              layout.graph.y + (layout.graph.height / 2.0));
        const std::array<double, 2> back =
            CurvePointInGraph(layout.graph, unit[0], unit[1]);
        Check(std::abs(unit[0] - 0.25) < 1e-9 &&
                  std::abs(unit[1] - 0.5) < 1e-9 &&
                  std::abs(back[0] - (layout.graph.x + quarter)) < 1e-9 &&
                  layout.graph.y >= layout.bar.y + layout.bar.height,
              "the graph maps to the curve's units and back, under its bar");
        const double radius = ui::theme::kGradeCurvePointRadius;
        Check(
            layout.graph.x >= radius &&
                layout.graph.x + layout.graph.width <= 276.0 - radius &&
                layout.graph.y >= layout.bar.y + layout.bar.height + radius &&
                layout.height >= layout.graph.y + layout.graph.height + radius,
            "a point on a corner of the graph is drawn whole in the view");
        const std::array<double, 2> corner =
            CurvePointInGraph(layout.graph, 0.0, 0.0);
        Check(corner[1] == layout.graph.y + layout.graph.height,
              "black is at the bottom left");
    }
}

// GR8 -- the hue curves.
void TestHueCurves() {
    using namespace ui::inspector;
    std::string error;

    // --- the table ---
    {
        bool ordered = HueCurveSpecs().size() == kHueCurveNames.size();
        for (size_t index = 0; ordered && index < kHueCurveNames.size();
             ++index) {
            ordered = std::string(HueCurveSpecs().at(index).name) ==
                          kHueCurveNames.at(index) &&
                      HueCurveSpecs().at(index).periodic ==
                          IsHueIndexedCurve(kHueCurveNames.at(index));
        }
        Check(ordered && FindHueCurveSpec("hue_sat") != nullptr &&
                  FindHueCurveSpec("master") == nullptr,
              "the six curves, in the engine's order, the first three along "
              "the hue");
        const auto& vectors = HueCurveVectors();
        Check(std::abs(vectors[0].hue - (102.9 / 360.0)) < 0.001 &&
                  vectors[0].hue < vectors[1].hue &&
                  vectors[1].hue < vectors[2].hue &&
                  vectors[2].hue < vectors[3].hue &&
                  vectors[3].hue < vectors[4].hue && vectors[5].hue < 0.2,
              "red stands at the vectorscope's 103 degrees, then yellow, "
              "green, cyan and blue, and magenta past the seam");
    }

    // --- an empty curve ---
    Check(HueCurvePoints({}, "hue_sat").empty() &&
              HueCurveValueAt({}, "hue_sat", 0.3) == 0.5 &&
              SampleHueCurve({}, "hue_sat", 3) ==
                  std::vector<double>{0.5, 0.5, 0.5},
          "a curve without a point is a flat line at neutral");

    // --- the first click: the point and its two anchors ---
    const std::vector<ClipEffect> graded = {
        Stage("color.saturation", {{"amount", {1, 10}}})};
    const CurveEdit first =
        WithHueCurvePointAdded(graded, "hue_sat", 0.3, 0.2, false);
    {
        const std::vector<ColorCurvePoint> points =
            HueCurvePoints(first.effects, "hue_sat");
        Check(
            first.index == 1 && points.size() == 3 &&
                Same(points[0].x, 217, 1000) && Same(points[0].y, 1, 2) &&
                Same(points[1].x, 3, 10) && Same(points[1].y, 1, 5) &&
                Same(points[2].x, 383, 1000) && Same(points[2].y, 1, 2) &&
                Types(first.effects) == "color.saturation color.hue_curves " &&
                Valid(first.effects, error),
            "a click on a flat curve places its point between two anchors "
            "at neutral, the stage after the saturation: " +
                Types(first.effects) + error);
        Check(std::abs(HueCurveValueAt(first.effects, "hue_sat", 0.3) - 0.2) <
                      1e-6 &&
                  HueCurveValueAt(first.effects, "hue_sat", 0.7) == 0.5 &&
                  HueCurvePoints(first.effects, "hue_hue").empty(),
              "the curve passes there, stays neutral away from it, and the "
              "other curves are untouched");
        // Near the seam the anchors go round.
        const CurveEdit seam =
            WithHueCurvePointAdded({}, "hue_hue", 0.02, 0.7, false);
        const std::vector<ColorCurvePoint> round =
            HueCurvePoints(seam.effects, "hue_hue");
        Check(seam.index == 0 && round.size() == 3 && Same(round[0].x, 1, 50) &&
                  Same(round[1].x, 103, 1000) && Same(round[2].x, 937, 1000) &&
                  Same(round[2].y, 1, 2) && Valid(seam.effects, error),
              "a click near hue 0 puts one anchor the other side of the "
              "seam: " +
                  error);
        // Along the luma, the anchors are the ends.
        const CurveEdit tone =
            WithHueCurvePointAdded({}, "lum_sat", 0.2, 0.1, false);
        const std::vector<ColorCurvePoint> ends =
            HueCurvePoints(tone.effects, "lum_sat");
        Check(
            tone.index == 1 && ends.size() == 3 && Same(ends[0].x, 0, 1) &&
                Same(ends[0].y, 1, 2) && Same(ends[2].x, 1, 1) &&
                Valid(tone.effects, error),
            "along the luma the first click is held by the two ends: " + error);
        const CurveEdit atEnd =
            WithHueCurvePointAdded({}, "sat_lum", 1.0, 0.3, false);
        const std::vector<ColorCurvePoint> two =
            HueCurvePoints(atEnd.effects, "sat_lum");
        Check(atEnd.index == 1 && two.size() == 2 && Same(two[1].y, 3, 10) &&
                  Same(two[0].y, 1, 2),
              "a first click on an end moves that end");
    }

    // --- more points, on the curve with Shift ---
    {
        const double before = HueCurveValueAt(first.effects, "hue_sat", 0.26);
        const CurveEdit kept =
            WithHueCurvePointAdded(first.effects, "hue_sat", 0.26, 0.9, true);
        Check(kept.index == 1 &&
                  HueCurvePoints(kept.effects, "hue_sat").size() == 4 &&
                  std::abs(CurveCoordinate(
                               HueCurvePoints(kept.effects, "hue_sat")[1].y) -
                           before) < 0.0006,
              "a Shift click adds its point on the curve");
        const CurveEdit again =
            WithHueCurvePointAdded(first.effects, "hue_sat", 0.3, 0.9, false);
        Check(again.index == 1 &&
                  HueCurvePoints(again.effects, "hue_sat").size() == 3 &&
                  Same(HueCurvePoints(again.effects, "hue_sat")[1].y, 1, 5),
              "a click at a point's x holds that point");
        std::vector<ClipEffect> full;
        for (int index = 0; index < kMaxColorCurvePoints; ++index) {
            full = WithHueCurvePointAdded(full, "hue_lum",
                                          0.05 + (index * 0.06), 0.5, false)
                       .effects;
        }
        // The first click placed three: sixteen are reached before the loop
        // ends, and the rest were refused.
        const CurveEdit refused =
            WithHueCurvePointAdded(full, "hue_lum", 0.999, 0.5, false);
        Check(HueCurvePoints(full, "hue_lum").size() == 16 &&
                  refused.index == -1 &&
                  refused.refusal == "Une courbe porte 16 points au plus." &&
                  Valid(full, error),
              "a seventeenth point is refused, by name: " + refused.refusal +
                  error);
    }

    // --- moving ---
    {
        const std::vector<ColorCurvePoint> moved = HueCurvePoints(
            WithHueCurvePointMoved(first.effects, "hue_sat", 1, 0.9, 1.4, false)
                .effects,
            "hue_sat");
        Check(Same(moved[1].x, 382, 1000) && Same(moved[1].y, 1, 1),
              "a point stops one step short of its neighbour and at the top "
              "of the graph");
        const std::vector<ColorCurvePoint> snapped = HueCurvePoints(
            WithHueCurvePointMoved(first.effects, "hue_sat", 1, 0.31, 0.9, true)
                .effects,
            "hue_sat");
        Check(Same(snapped[1].x, 31, 100) && Same(snapped[1].y, 1, 2),
              "an Option drag snaps the point to neutral");
        const std::vector<ClipEffect> edge =
            WithHueCurvePointMoved(first.effects, "hue_sat", 2, 1.7, 0.5, false)
                .effects;
        Check(Same(HueCurvePoints(edge, "hue_sat")[2].x, 999, 1000) &&
                  Valid(edge, error),
              "the last point of a curve along the hue stops short of 1, "
              "which is 0 again: " +
                  error);
    }

    // --- removing ---
    {
        const CurveEdit two =
            WithHueCurvePointRemoved(first.effects, "hue_sat", 0);
        Check(HueCurvePoints(two.effects, "hue_sat").size() == 2 &&
                  Valid(two.effects, error),
              "any point is removed, an anchor included: " + error);
        const CurveEdit none =
            WithHueCurvePointRemoved(two.effects, "hue_sat", 0);
        Check(Types(none.effects) == "color.saturation ",
              "a curve left with one point is cleared, and the stage goes: " +
                  Types(none.effects));
        Check(Types(WithHueCurveReset(first.effects, "hue_sat")) ==
                  "color.saturation ",
              "a reset clears the curve");
    }

    // --- the colour patches ---
    {
        const CurveEdit patch = WithHueCurveVector({}, "hue_hue", 4);
        const std::vector<ColorCurvePoint> points =
            HueCurvePoints(patch.effects, "hue_hue");
        const double blue = HueCurveVectors()[4].hue;
        Check(patch.refusal.empty() && points.size() == 3 && patch.index >= 0 &&
                  std::abs(CurveCoordinate(
                               points[static_cast<size_t>(patch.index)].x) -
                           blue) < 0.001 &&
                  Same(points[0].y, 1, 2) && Same(points[1].y, 1, 2) &&
                  Same(points[2].y, 1, 2) && Valid(patch.effects, error),
              "the blue patch places three points at neutral around blue, "
              "and holds the middle one: " +
                  error);
        const CurveEdit onCurve =
            WithHueCurveVector(first.effects, "hue_sat", 0);
        double worst = 0.0;
        for (int step = 0; step < 50; ++step) {
            const double x = step / 50.0;
            worst = std::max(
                worst, std::abs(HueCurveValueAt(onCurve.effects, "hue_sat", x) -
                                HueCurveValueAt(first.effects, "hue_sat", x)));
        }
        Check(HueCurvePoints(onCurve.effects, "hue_sat").size() > 3 &&
                  worst < 0.02,
              "on a curve already bent the patch's points land on it (worst " +
                  std::to_string(worst) + ")");
        Check(SameGradeStack(WithHueCurveVector({}, "lum_sat", 0).effects, {}),
              "a curve that is not along the hue has no patch");
    }

    // --- the point under the pointer, and the geometry ---
    {
        Check(HitHueCurvePoint(first.effects, "hue_sat", 0.31, 0.22, 0.03,
                               0.05) == 1 &&
                  HitHueCurvePoint(first.effects, "hue_sat", 0.6, 0.5, 0.03,
                                   0.05) == -1 &&
                  HitHueCurvePoint({}, "hue_sat", 0.3, 0.5, 0.03, 0.05) == -1,
              "a point is held from within its reach; a flat curve has none");
        const GradeHueCurveLayout layout = LayoutGradeHueCurve(276.0);
        const double radius = ui::theme::kGradeCurvePointRadius;
        Check(
            layout.patches[0].y >= layout.bar.y + layout.bar.height &&
                layout.patches[5].x + layout.patches[5].width <
                    layout.reset.x &&
                layout.graph.y >=
                    layout.patches[0].y + layout.patches[0].height + radius &&
                layout.graph.x >= radius &&
                layout.height >= layout.graph.y + layout.graph.height + radius,
            "the bar, the six patches and the reset, then the graph with "
            "room for a point on its edge");
    }
}

void TestCurveHistogram() {
    const VideoHistogram empty{};
    Check(NormalizeVideoHistogramChannel(empty, 0) ==
              std::array<double, kVideoHistogramBins>{},
          "a picture without samples draws no histogram");
    VideoHistogram histogram{};
    histogram[0][0] = 100;
    histogram[0][255] = 20;
    histogram[1][128] = 50;
    const auto red = NormalizeVideoHistogramChannel(histogram, 0);
    const auto green = NormalizeVideoHistogramChannel(histogram, 1);
    const auto blue = NormalizeVideoHistogramChannel(histogram, 2);
    Check(red.front() == 1.0 && red.back() == 0.2 && green[128] == 0.5 &&
              blue == std::array<double, kVideoHistogramBins>{},
          "black and white stay in the end bins, channels share one scale");
    Check(NormalizeVideoHistogramChannel(histogram, 3) ==
              std::array<double, kVideoHistogramBins>{},
          "an invalid channel draws no distribution");
}

void TestColorWorkspaceLayout() {
    using namespace ui::inspector;
    const std::array<std::vector<double>, 4> heights = {
        std::vector<double>{24.0}, std::vector<double>{22.0, 420.0},
        std::vector<double>{22.0, 240.0, 220.0},
        std::vector<double>{24.0, 24.0}};
    const auto wide = LayoutGradeWorkspace(1000.0, heights);
    Check(wide.columns && wide.groups[1][0].y == wide.groups[2][0].y &&
              wide.groups[1][0].x == 0.0 &&
              wide.groups[2][0].x > wide.groups[1][0].width &&
              wide.groups[2][0].x + wide.groups[2][0].width == 1000.0,
          "Colour places wheels and curves beside each other after the reset");
    const auto& lastWheel = wide.groups[1].back();
    const auto& lastCurve = wide.groups[2].back();
    Check(
        wide.groups[3][0].y > lastWheel.y + lastWheel.height &&
            wide.groups[3][0].y > lastCurve.y + lastCurve.height,
        "secondary controls begin below both columns, including unfolded rows");
    const auto narrow = LayoutGradeWorkspace(300.0, heights);
    Check(!narrow.columns && narrow.groups[1][0].width == 300.0 &&
              narrow.groups[2][0].width == 300.0 &&
              narrow.groups[2][0].y >
                  narrow.groups[1].back().y + narrow.groups[1].back().height,
          "a narrow Colour panel keeps both editors at its full width");
    const auto empty = LayoutGradeWorkspace(-1.0, {});
    Check(!empty.columns && empty.height >= 0.0,
          "an empty or not-yet-sized panel has bounded geometry");
}

}  // namespace

int main() {
    TestWheels();
    TestCurves();
    TestHueCurves();
    TestCurveHistogram();
    TestColorWorkspaceLayout();
    if (failures == 0) {
        std::cout << "PASS: grade control models\n";
    } else {
        std::cout << "FAIL: " << failures << " check(s) failed\n";
    }
    return failures == 0 ? 0 : 1;
}
