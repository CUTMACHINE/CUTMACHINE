#pragma once

// GR3 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The colour wheels of the
// Inspector, as policy: where a wheel's puck sits for a clip's stack, what
// its rim is painted with, where each of its parts lies, and the stack a
// gesture gives. Portable, no framework: GradeWheelView.mm draws what this
// says and turns mouse events into calls here, the way MotionCanvas.h stands
// behind the visual editor of motion. One geometry serves the drawing and
// the hit test (LayoutGradeWheel), so the two cannot drift apart the way
// the timeline's drawn and clickable rectangles can.
//
// A wheel edits four knobs of the clip's first color.wheels stage
// (ColorEffects.h): <wheel>_r, <wheel>_g, <wheel>_b and <wheel>_master. The
// puck is the colour of the three channel knobs and the master bar is the
// master knob; nothing else of the stage is read or rewritten, so a stage an
// agent wrote keeps every number a gesture did not move.
//
// The puck's plane is the vectorscope's (VideoScopes.h): +x is +Cb, +y is
// +Cr, with the Rec.709 luma weights the saturation stage and measure_color
// use. A point of that plane is the channel triple of that chroma and of no
// luma -- so moving the puck rebalances the three channels around a constant
// weighted sum, as the colour balance controls of the application the
// timelines go to do, and the puck moves a neutral where the vectorscope
// shows it going. The part of the three knobs that is luma is not on the
// puck: it shows in the fields under the wheel, which hold the knobs as
// stored, and a gesture leaves it where it is.
//
// The gestures are that application's, from its manual (Primaries palette,
// "Color Balance Controls"): a drag anywhere in the ring moves the puck
// relative to where it was, as a trackball would; with Shift the puck jumps
// under the pointer; a double click resets the colour and not the master;
// with Command the drag sets the master; the reset control resets both.

#include "ColorEffects.h"
#include "Document.h"
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

enum class GradeWheel : int32_t { Lift = 0, Gamma = 1, Gain = 2, Offset = 3 };

struct GradeWheelSpec {
    GradeWheel wheel;
    const char* name;   // The knobs' prefix in color.wheels: "lift".
    const char* label;  // As the app names the wheel.
    // The chroma the rim stands for, in the knob's unit: a code value for
    // lift and offset, a power of two for gamma and gain. Past it the puck
    // stops; a channel knob reaches about twice that at the blue axis.
    double rim;
    // What one point of a master drag adds to the master knob.
    double master_step;
};

// In the order they are laid out: lift and gamma, then gain and offset.
inline const std::array<GradeWheelSpec, 4>& GradeWheels() {
    static const std::array<GradeWheelSpec, 4> kWheels{{
        {GradeWheel::Lift, "lift", "Lift", 0.15, 0.001},
        {GradeWheel::Gamma, "gamma", "Gamma", 0.30, 0.002},
        {GradeWheel::Gain, "gain", "Gain", 0.30, 0.002},
        {GradeWheel::Offset, "offset", "Offset", 0.15, 0.001},
    }};
    return kWheels;
}

// A knob is committed as a fraction of this, like every slider's value.
inline constexpr int32_t kWheelPrecision = 1000;

// A drag in the ring moves the puck by a quarter of the pointer's travel: a
// correction one would keep is a few thousandths on a channel, a point or
// two of puck, and the hand needs more room than that.
inline constexpr double kWheelDragGear = 0.25;

// The puck's move, in rims, for a pointer move of `points` in a ring of
// `radius` points.
inline double WheelDragDelta(double points, double radius) {
    return radius > 0.0 ? kWheelDragGear * points / radius : 0.0;
}

// The channel triple (r, g, b) of a chroma and of no luma, Rec.709 weights.
inline std::array<double, 3> WheelChannelsForChroma(double cb, double cr) {
    constexpr double kRed = 0.2126;
    constexpr double kGreen = 0.7152;
    constexpr double kBlue = 0.0722;
    const double red = 2.0 * (1.0 - kRed) * cr;
    const double blue = 2.0 * (1.0 - kBlue) * cb;
    return {red, -((kRed * red) + (kBlue * blue)) / kGreen, blue};
}

// The chroma (cb, cr) of a channel triple; its luma is left out.
inline std::array<double, 2> WheelChromaOfChannels(
    const std::array<double, 3>& channels) {
    constexpr double kRed = 0.2126;
    constexpr double kGreen = 0.7152;
    constexpr double kBlue = 0.0722;
    const double luma =
        (kRed * channels[0]) + (kGreen * channels[1]) + (kBlue * channels[2]);
    return {(channels[2] - luma) / (2.0 * (1.0 - kBlue)),
            (channels[0] - luma) / (2.0 * (1.0 - kRed))};
}

// What the rim is painted with at `angle` (radians from +x, counter-
// clockwise, y up): the colour the puck pushes a neutral toward there, at
// full strength around mid grey.
inline std::array<float, 3> WheelRimColor(double angle) {
    const std::array<double, 3> channels =
        WheelChannelsForChroma(std::cos(angle), std::sin(angle));
    const double largest = std::max(
        {std::abs(channels[0]), std::abs(channels[1]), std::abs(channels[2])});
    std::array<float, 3> color{};
    for (size_t channel = 0; channel < 3; ++channel) {
        color.at(channel) =
            static_cast<float>(0.5 + (0.45 * channels.at(channel) / largest));
    }
    return color;
}

inline std::string WheelKnobName(const GradeWheelSpec& spec, size_t channel) {
    return std::string(spec.name) + "_" + kColorWheelChannels.at(channel);
}

// The wheel's four knobs as the clip's first color.wheels stage holds them,
// r, g, b then master; zeros without a stage.
inline std::array<double, 4> ReadWheelKnobs(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec) {
    std::array<double, 4> knobs{};
    const ClipEffect* wheels = FindFirstEffect(effects, "color.wheels");
    if (wheels == nullptr) {
        return knobs;
    }
    for (size_t channel = 0; channel < 4; ++channel) {
        const auto found = wheels->params.find(WheelKnobName(spec, channel));
        if (found != wheels->params.end() && found->second.den > 0) {
            knobs.at(channel) = static_cast<double>(found->second.num) /
                                static_cast<double>(found->second.den);
        }
    }
    return knobs;
}

struct GradeWheelState {
    // r, g, b, master, as stored.
    std::array<double, 4> knobs{};
    // The puck, in rims from the centre: +x toward +Cb, +y toward +Cr. On
    // the rim when the stored colour lies past it.
    double x = 0.0;
    double y = 0.0;
    // How far the stored colour lies from the centre, in rims; above 1 for
    // a colour an agent wrote past what the wheel reaches.
    double reach = 0.0;
};

inline GradeWheelState ReadGradeWheel(const std::vector<ClipEffect>& effects,
                                      const GradeWheelSpec& spec) {
    GradeWheelState state;
    state.knobs = ReadWheelKnobs(effects, spec);
    const std::array<double, 2> chroma =
        WheelChromaOfChannels({state.knobs[0], state.knobs[1], state.knobs[2]});
    state.x = chroma[0] / spec.rim;
    state.y = chroma[1] / spec.rim;
    state.reach = std::hypot(state.x, state.y);
    if (state.reach > 1.0) {
        state.x /= state.reach;
        state.y /= state.reach;
    }
    return state;
}

// The stack with `values` written to the knobs `mask` names (r, g, b,
// master), each clamped to what a wheel knob takes and committed as an exact
// fraction; a knob at zero is removed, and so is a stage left without a
// knob. Without a stage, one is added at its canonical place (GR1) -- unless
// every value written is zero, which changes nothing.
inline std::vector<ClipEffect> WithWheelKnobs(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec,
    const std::array<double, 4>& values, const std::array<bool, 4>& mask) {
    std::array<EffectParamValue, 4> fractions{};
    bool anything = false;
    for (size_t channel = 0; channel < 4; ++channel) {
        if (!mask.at(channel)) {
            continue;
        }
        const double clamped = std::clamp(
            values.at(channel), -static_cast<double>(kColorWheelLimit),
            static_cast<double>(kColorWheelLimit));
        ReducedFraction(std::llround(clamped * kWheelPrecision),
                        kWheelPrecision, fractions.at(channel));
        anything = anything || fractions.at(channel).num != 0;
    }
    if (FindFirstEffect(effects, "color.wheels") == nullptr && !anything) {
        return effects;
    }
    std::vector<ClipEffect> result = effects;
    ClipEffect& stage = GradeStage(result, "color.wheels");
    for (size_t channel = 0; channel < 4; ++channel) {
        if (!mask.at(channel)) {
            continue;
        }
        const std::string name = WheelKnobName(spec, channel);
        if (fractions.at(channel).num == 0) {
            stage.params.erase(name);
        } else {
            stage.params[name] = fractions.at(channel);
        }
    }
    if (stage.params.empty()) {
        result.erase(result.begin() + (&stage - result.data()));
    }
    return result;
}

// The stack with the wheel's puck brought to (x, y), in rims. The puck does
// not leave the ring: a target past the rim is brought back onto it -- or
// onto the circle the stored colour already lies on, when an agent wrote it
// past the rim, so that a gesture never pulls it in by more than it asked.
// Only the chroma moves: the three channel knobs take the channel triple of
// the difference, their luma part stays.
inline std::vector<ClipEffect> WithWheelPuck(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec,
    double x, double y) {
    const std::array<double, 4> knobs = ReadWheelKnobs(effects, spec);
    const std::array<double, 2> chroma =
        WheelChromaOfChannels({knobs[0], knobs[1], knobs[2]});
    const double limit =
        std::max(1.0, std::hypot(chroma[0], chroma[1]) / spec.rim);
    const double distance = std::hypot(x, y);
    if (distance > limit) {
        x *= limit / distance;
        y *= limit / distance;
    }
    const std::array<double, 3> move = WheelChannelsForChroma(
        (x * spec.rim) - chroma[0], (y * spec.rim) - chroma[1]);
    return WithWheelKnobs(
        effects, spec,
        {knobs[0] + move[0], knobs[1] + move[1], knobs[2] + move[2], 0.0},
        {true, true, true, false});
}

// The same, relative to where the stored colour is: what a drag in the ring
// gives, `effects` being the stack the drag started from.
inline std::vector<ClipEffect> WithWheelPuckMoved(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec,
    double dx, double dy) {
    const std::array<double, 4> knobs = ReadWheelKnobs(effects, spec);
    const std::array<double, 2> chroma =
        WheelChromaOfChannels({knobs[0], knobs[1], knobs[2]});
    return WithWheelPuck(effects, spec, (chroma[0] / spec.rim) + dx,
                         (chroma[1] / spec.rim) + dy);
}

// The stack with the master knob moved by `delta`.
inline std::vector<ClipEffect> WithWheelMasterMoved(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec,
    double delta) {
    const std::array<double, 4> knobs = ReadWheelKnobs(effects, spec);
    return WithWheelKnobs(effects, spec, {0.0, 0.0, 0.0, knobs[3] + delta},
                          {false, false, false, true});
}

// One knob set to `value`: what a field under the wheel commits.
inline std::vector<ClipEffect> WithWheelKnob(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec,
    size_t channel, double value) {
    std::array<double, 4> values{};
    std::array<bool, 4> mask{};
    values.at(channel) = value;
    mask.at(channel) = true;
    return WithWheelKnobs(effects, spec, values, mask);
}

// The colour back to neutral, the master kept: a double click in the ring.
inline std::vector<ClipEffect> WithWheelColourReset(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec) {
    return WithWheelKnobs(effects, spec, {}, {true, true, true, false});
}

// The colour and the master back to neutral: the reset control.
inline std::vector<ClipEffect> WithWheelReset(
    const std::vector<ClipEffect>& effects, const GradeWheelSpec& spec) {
    return WithWheelKnobs(effects, spec, {}, {true, true, true, true});
}

// What a field shows for a knob. The gain is stored as a power of two, 0
// neutral; it reads as the factor it is, 1.00 neutral, as in the
// application the timelines go to. The other three read as stored.
inline double WheelShownValue(const GradeWheelSpec& spec, double knob) {
    return spec.wheel == GradeWheel::Gain ? std::exp2(knob) : knob;
}

// The knob for a value typed in a field; false for one that names no knob
// (a gain factor that is not positive, a number that is not one).
inline bool WheelKnobForShown(const GradeWheelSpec& spec, double shown,
                              double& knob) {
    if (!std::isfinite(shown)) {
        return false;
    }
    if (spec.wheel != GradeWheel::Gain) {
        knob = shown;
        return true;
    }
    if (shown <= 0.0) {
        return false;
    }
    knob = std::log2(shown);
    return true;
}

// Where the master bar's mark sits for a master knob: -1 at its left end, 0
// at its middle, 1 at its right end. The bar spans twice the rim's chroma
// either side -- what a master is moved by in practice -- and the mark
// stops at its ends for a knob beyond.
inline double WheelMasterFraction(const GradeWheelSpec& spec, double master) {
    return std::clamp(master / (2.0 * spec.rim), -1.0, 1.0);
}

// --- Geometry ---------------------------------------------------------------

// Points, rows from the top.
struct GradeRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool Contains(double px, double py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// One wheel in a cell `width` wide: its title with the reset control at its
// right, the ring centred under it, the four fields (r, g, b, master), and
// the master bar.
struct GradeWheelLayout {
    GradeRect title;
    GradeRect reset;
    GradeRect ring;
    std::array<GradeRect, 4> fields;
    GradeRect master;
    double height = 0.0;
};

inline GradeWheelLayout LayoutGradeWheel(double width) {
    using namespace ui::theme;
    GradeWheelLayout layout;
    layout.title = {0.0, 0.0, std::max(0.0, width - kGradeWheelTitleHeight),
                    kGradeWheelTitleHeight};
    layout.reset = {std::max(0.0, width - kGradeWheelTitleHeight), 0.0,
                    kGradeWheelTitleHeight, kGradeWheelTitleHeight};
    double y = kGradeWheelTitleHeight + kSpaceXxs;
    layout.ring = {std::max(0.0, (width - kGradeWheelDiameter) / 2.0), y,
                   kGradeWheelDiameter, kGradeWheelDiameter};
    y += kGradeWheelDiameter + kSpaceXs;
    const double field = std::max(0.0, (width - (3.0 * kSpaceXxs)) / 4.0);
    for (size_t index = 0; index < 4; ++index) {
        layout.fields.at(index) = {
            static_cast<double>(index) * (field + kSpaceXxs), y, field,
            kGradeWheelFieldHeight};
    }
    y += kGradeWheelFieldHeight + kSpaceXs;
    layout.master = {0.0, y, width, kGradeWheelMasterHeight};
    layout.height = y + kGradeWheelMasterHeight;
    return layout;
}

// The height every wheel cell takes, whatever its width.
inline double GradeWheelHeight() { return LayoutGradeWheel(0.0).height; }

// The four wheels in two rows of two, in a block `width` wide.
struct GradeWheelGridLayout {
    std::array<GradeRect, 4> cells;
    double height = 0.0;
};

inline GradeWheelGridLayout LayoutGradeWheelGrid(double width) {
    using namespace ui::theme;
    GradeWheelGridLayout layout;
    const double cell = std::max(0.0, (width - kSpaceS) / 2.0);
    const double height = GradeWheelHeight();
    for (size_t index = 0; index < 4; ++index) {
        const size_t column = index % 2;
        const size_t row = index / 2;
        layout.cells.at(index) = {
            static_cast<double>(column) * (cell + kSpaceS),
            static_cast<double>(row) * (height + kSpaceM), cell, height};
    }
    layout.height = (2.0 * height) + kSpaceM;
    return layout;
}

// How far from the centre the puck travels, in points: to the inner edge of
// the painted ring, its own radius left out, so that at one rim it touches
// the ring without covering it. `ring` is LayoutGradeWheel's.
inline double WheelTravelRadius(const GradeRect& ring) {
    return std::max(0.0, (ring.width / 2.0) - ui::theme::kGradeWheelRingWidth -
                             ui::theme::kGradeWheelPuckRadius);
}

// Where a point of the ring's square lies on the wheel, in rims: +x right,
// +y up, the centre at (0, 0).
inline std::array<double, 2> WheelPointInRims(const GradeRect& ring, double px,
                                              double py) {
    const double travel = WheelTravelRadius(ring);
    if (travel <= 0.0) {
        return {0.0, 0.0};
    }
    const double half = ring.width / 2.0;
    return {(px - (ring.x + half)) / travel, ((ring.y + half) - py) / travel};
}

// And back: where the puck is drawn for a position in rims.
inline std::array<double, 2> WheelPointInRing(const GradeRect& ring, double x,
                                              double y) {
    const double travel = WheelTravelRadius(ring);
    const double half = ring.width / 2.0;
    return {ring.x + half + (x * travel), ring.y + half - (y * travel)};
}

// Whether a point is in the disc the ring bounds, the painted ring
// included: the whole disc takes the drag, not the puck alone.
inline bool WheelDiscContains(const GradeRect& ring, double px, double py) {
    const double half = ring.width / 2.0;
    return std::hypot(px - (ring.x + half), py - (ring.y + half)) <= half;
}

// Whether two stacks are the same edit: what tells a gesture that changed
// nothing, which commits nothing.
inline bool SameGradeStack(const std::vector<ClipEffect>& left,
                           const std::vector<ClipEffect>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const ClipEffect& a = left[index];
        const ClipEffect& b = right[index];
        if (a.id != b.id || a.type != b.type ||
            a.params.size() != b.params.size()) {
            return false;
        }
        for (const auto& [name, value] : a.params) {
            const auto other = b.params.find(name);
            if (other == b.params.end() || other->second.num != value.num ||
                other->second.den != value.den) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace ui::inspector
