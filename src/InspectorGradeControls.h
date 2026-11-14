#pragma once

// F2.2 -- ROADMAP.md Inspector. UI-facing metadata and pure conversion
// helpers for the eight F1.3 color.* grading knobs (ColorEffects.h's
// EffectRegistry). Plain C++, no AppKit -- following UiTheme.h's and
// PanelLayout.h's precedent -- so the float<->EffectParamValue boundary a
// slider UI needs (a drag position becoming a committed exact fraction) is
// directly unit-testable (tests/inspector_grade_controls_tests.cc) without
// a macOS toolchain.
//
// This header only *describes* the eight knobs for a slider UI (label,
// range, quantization step) and converts between a slider's float position
// and an exact EffectParamValue. It never mutates a document or clip, and
// never touches EditLog/Operations: the AppKit Inspector view
// (InspectorView.h/.mm, unverified in this sandbox -- see the F2.2 report)
// calls WithGradeControlValue to build the *new* effects vector for a
// clip, then submits it through SetClipEffectsOperation via
// EditLog::Apply -- the same path CLI/MCP already use for this operation
// (PHILOSOPHY.md principle 3, "aucune surface n'est privilégiée").
//
// F1.3 wheels and curves (18 September 2026) are sliders too, one per
// component, in the style of the eight above. Since GR4 and GR5
// (docs/roadmap/GRADE-RESOLVE-2026-10.md) the Inspector also draws the
// wheels and an editable curve (GradeWheelModel.h, GradeCurveModel.h): the
// sliders below stay, folded under them, as the bars that show each knob on
// its own. A wheel is four sliders (red, green, blue, master) per lift,
// gamma, gain and offset -- its sixteen params exactly. A curve
// channel is three sliders: the curve through (0, 0) and (1, 1) and three
// points at x = 1/4, 1/2 and 3/4, each slider setting how far its point
// sits above or below the diagonal. Curves an agent wrote with other points
// are shown (the value of the curve at those x) but not editable by these
// sliders: an edit would have to replace them, and nothing would say so.
// The curve editor takes them as they are. LUTs remain out of scope: the
// engine does not render them.

#include "ClipFraming.h"
#include "ColorEffects.h"
#include "Document.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace ui::inspector {

// One grading knob's UI presentation: the registry entry it reads/writes
// (`type`, matched against ColorEffectRegistry() at call time -- never
// duplicated here) plus the range and quantization a slider needs that the
// registry itself has no reason to carry (ColorEffects.h is a render-time
// concern; range/precision are a UI concern).
struct GradeControlSpec {
    const char* type;   // Matches ColorEffectRegistryEntry::type exactly.
    const char* label;  // French, matches the rest of the app's UI.
    float min_value;
    float max_value;
    // A committed value is quantized to the nearest 1/precision_den before
    // becoming an EffectParamValue, so num/den is always an exact fraction
    // -- never a lossy, unpredictable round-trip of the slider's raw float
    // (PHILOSOPHY.md principle 4: exact conversions, refused/quantized
    // deliberately rather than silently drifting). Temperature already
    // commits whole Kelvin -- see ColorEffects.cc's "kelvin" default and
    // its test ("integral values (kelvin) cross without a denominator") --
    // so its precision is 1, matching that convention exactly.
    int32_t precision_den;
    // F1.3 -- which knob of `type` this slider drives. nullptr for the eight
    // scalar effects (their one registry knob); a param name ("lift_r") for
    // color.wheels; a channel ("master", "r", "g", "b") for color.curves.
    const char* param;
    // color.curves only: which of the three grid points (kCurveGridX) this
    // slider moves; -1 otherwise.
    int32_t curve_point;
};

// F1.3 -- the Inspector's curve grid: the points a curve slider moves sit at
// these x, between fixed ends at (0, 0) and (1, 1).
inline constexpr std::array<int32_t, 3> kCurveGridQuarters = {1, 2, 3};

// Display order matches ColorEffectKind (ColorEffects.h), which itself
// matches BuildRegistry()'s declaration order in ColorEffects.cc. Ranges
// below are read off shader.metal's own comments for each knob (e.g.
// applyContrast: "+/-1 doubles/removes the slope"). The temperature's
// 2000-12000 K spans tungsten to shade, the lights a scene is lit by; the
// engine takes 1667-25000 K (G7, ColorEffects.h).
//
// F1.3 -- wheel and curve sliders cover part of what the engine accepts
// (every wheel knob lies in [-1, 1]): a lift of 0.5 already moves black
// halfway up, a gain or gamma of 1 doubles. No slider below shares the
// exposure's or the opacity's range, which the UI smoke test finds them by.
// GR2 -- the offset wheel's four knobs follow the gain's.
// GR6 -- the contrast's pivot follows the contrast and the hue rotation
// closes the scalar knobs. The pivot's slider stops short of 0 and 1, which
// the engine takes: a pivot there is a lift or a gain under another name,
// and the range [0, 1] is the opacity's, which the smoke test finds it by.
inline const std::vector<GradeControlSpec>& GradeControls() {
    // clang-format off
    static const std::vector<GradeControlSpec> kControls{{
        {"color.exposure",    "Exposition",    -2.0f,     2.0f, 1000, nullptr, -1},
        {"color.contrast",    "Contraste",     -1.0f,     1.0f, 1000, nullptr, -1},
        {"color.contrast",    "Pivot",          0.05f,    0.95f, 1000, "pivot", -1},
        {"color.saturation",  "Saturation",    -1.0f,     1.0f, 1000, nullptr, -1},
        {"color.vibrance",    "Vibrance",      -1.0f,     1.0f, 1000, nullptr, -1},
        {"color.temperature", "Température", 2000.0f, 12000.0f,    1, nullptr, -1},
        {"color.tint",        "Teinte",        -1.0f,     1.0f, 1000, nullptr, -1},
        {"color.highlights",  "Hautes lum.",   -1.0f,     1.0f, 1000, nullptr, -1},
        {"color.shadows",     "Ombres",        -1.0f,     1.0f, 1000, nullptr, -1},
        {"color.hue",         "Rot. teintes", -180.0f,   180.0f,   10, nullptr, -1},
        {"color.wheels",      "Lift rouge",    -0.5f,     0.5f, 1000, "lift_r", -1},
        {"color.wheels",      "Lift vert",     -0.5f,     0.5f, 1000, "lift_g", -1},
        {"color.wheels",      "Lift bleu",     -0.5f,     0.5f, 1000, "lift_b", -1},
        {"color.wheels",      "Lift maître",   -0.5f,     0.5f, 1000, "lift_master", -1},
        {"color.wheels",      "Gamma rouge",   -1.0f,     1.0f, 1000, "gamma_r", -1},
        {"color.wheels",      "Gamma vert",    -1.0f,     1.0f, 1000, "gamma_g", -1},
        {"color.wheels",      "Gamma bleu",    -1.0f,     1.0f, 1000, "gamma_b", -1},
        {"color.wheels",      "Gamma maître",  -1.0f,     1.0f, 1000, "gamma_master", -1},
        {"color.wheels",      "Gain rouge",    -1.0f,     1.0f, 1000, "gain_r", -1},
        {"color.wheels",      "Gain vert",     -1.0f,     1.0f, 1000, "gain_g", -1},
        {"color.wheels",      "Gain bleu",     -1.0f,     1.0f, 1000, "gain_b", -1},
        {"color.wheels",      "Gain maître",   -1.0f,     1.0f, 1000, "gain_master", -1},
        {"color.wheels",      "Offset rouge",  -0.5f,     0.5f, 1000, "offset_r", -1},
        {"color.wheels",      "Offset vert",   -0.5f,     0.5f, 1000, "offset_g", -1},
        {"color.wheels",      "Offset bleu",   -0.5f,     0.5f, 1000, "offset_b", -1},
        {"color.wheels",      "Offset maître", -0.5f,     0.5f, 1000, "offset_master", -1},
        {"color.curves",      "Maître ombres", -0.25f,   0.25f, 1000, "master", 0},
        {"color.curves",      "Maître médiums",-0.25f,   0.25f, 1000, "master", 1},
        {"color.curves",      "Maître hautes", -0.25f,   0.25f, 1000, "master", 2},
        {"color.curves",      "Rouge ombres",  -0.25f,   0.25f, 1000, "r", 0},
        {"color.curves",      "Rouge médiums", -0.25f,   0.25f, 1000, "r", 1},
        {"color.curves",      "Rouge hautes",  -0.25f,   0.25f, 1000, "r", 2},
        {"color.curves",      "Vert ombres",   -0.25f,   0.25f, 1000, "g", 0},
        {"color.curves",      "Vert médiums",  -0.25f,   0.25f, 1000, "g", 1},
        {"color.curves",      "Vert hautes",   -0.25f,   0.25f, 1000, "g", 2},
        {"color.curves",      "Bleu ombres",   -0.25f,   0.25f, 1000, "b", 0},
        {"color.curves",      "Bleu médiums",  -0.25f,   0.25f, 1000, "b", 1},
        {"color.curves",      "Bleu hautes",   -0.25f,   0.25f, 1000, "b", 2},
    }};
    // clang-format on
    return kControls;
}

// F1.3 -- the Inspector groups the sliders under one header per family.
inline const char* GradeControlSectionTitle(const GradeControlSpec& spec) {
    const std::string type = spec.type;
    if (type == "color.wheels") {
        return "Roues (lift, gamma, gain, offset)";
    }
    if (type == "color.curves") {
        return "Courbes";
    }
    return "Correction colorimétrique";
}

// The first slider of `type`: the one slider of each scalar effect, the
// first of the wheels or curves.
inline const GradeControlSpec* FindGradeControlSpec(const std::string& type) {
    for (const GradeControlSpec& spec : GradeControls())
        if (type == spec.type) return &spec;
    return nullptr;
}

// F1.3 -- one wheel or curve slider, by its knob.
inline const GradeControlSpec* FindGradeControlSpec(const std::string& type,
                                                    const std::string& param,
                                                    int32_t curvePoint = -1) {
    for (const GradeControlSpec& spec : GradeControls()) {
        if (type == spec.type && spec.param != nullptr && param == spec.param &&
            curvePoint == spec.curve_point) {
            return &spec;
        }
    }
    return nullptr;
}

// Clamps to [min_value, max_value]. Every conversion below goes through
// this first so an out-of-range slider position (or a caller passing a
// stray float) never becomes a stored value the knob's own range wouldn't
// already cover.
inline float ClampToGradeControlRange(const GradeControlSpec& spec,
                                      float value) {
    return std::clamp(value, spec.min_value, spec.max_value);
}

// The exact float->EffectParamValue boundary for one knob, mirroring
// ColorEffects.h's EffectParamValueToFloat on the way in: rounds to the
// nearest 1/precision_den so the stored fraction is exact by construction,
// not a truncation of whatever bit pattern the slider produced. Reading it
// straight back through EffectParamValueToFloat reproduces the same
// quantized value on every read -- no further drift on repeated
// load/save, which is what makes this the correct side of the exact<->
// float boundary (PHILOSOPHY.md principle 4) rather than the wrong one.
inline EffectParamValue QuantizeGradeControlValue(const GradeControlSpec& spec,
                                                  float sliderValue) {
    const float clamped = ClampToGradeControlRange(spec, sliderValue);
    const int32_t den = spec.precision_den > 0 ? spec.precision_den : 1;
    const double scaled = static_cast<double>(clamped) * den;
    const int32_t num = static_cast<int32_t>(std::lround(scaled));
    return EffectParamValue{num, den};
}

// ALPHA-2026-08 -- Inspector percentages cross into the canonical model at a
// fixed 1/1000 boundary. Keeping this pure lets the portable UI policy tests
// cover the same value AppKit submits through SetClipOpacityOperation.
inline EffectParamValue QuantizeClipOpacity(float sliderValue) {
    const float clamped = std::clamp(sliderValue, 0.0f, 1.0f);
    return {static_cast<int32_t>(
                std::lround(static_cast<double>(clamped) * 1000.0)),
            1000};
}

inline float CurrentClipOpacity(const EffectParamValue& opacity) {
    return opacity.den > 0 ? static_cast<float>(opacity.num) /
                                 static_cast<float>(opacity.den)
                           : 1.0f;
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. The Inspector's « Flou »: the
// clip's Gaussian blur in sequence pixels, a slider from 0 to
// kInspectorBlurMaximum. Half the engine's bound (kMaxClipBlur): past 100 a
// 1080p shot is a wash of colour, and a slider twice as long would give the
// range people use -- a few pixels to a few tens -- half the travel. A blur
// set higher by a tool is shown as it is and kept until the slider moves.
// Crosses into the document at a tenth of a pixel, reduced, so the stored
// fraction is exact by construction, as the opacity's thousandth is.
inline constexpr float kInspectorBlurMaximum = 100.0f;

inline EffectParamValue QuantizeClipBlur(float sliderValue) {
    const float clamped = std::clamp(sliderValue, 0.0f, kInspectorBlurMaximum);
    int32_t num =
        static_cast<int32_t>(std::lround(static_cast<double>(clamped) * 10.0));
    int32_t den = 10;
    const int32_t common = std::gcd(num, den);
    if (common > 1) {
        num /= common;
        den /= common;
    }
    return {num, den};
}

inline float CurrentClipBlur(const EffectParamValue& blur) {
    return blur.den > 0
               ? static_cast<float>(blur.num) / static_cast<float>(blur.den)
               : 0.0f;
}

// F1.3 -- an exact fraction, reduced, or false when it does not fit the
// document's 32-bit fields.
inline bool ReducedFraction(int64_t num, int64_t den, EffectParamValue& out) {
    if (den <= 0) {
        return false;
    }
    const int64_t divisor = std::gcd(num < 0 ? -num : num, den);
    if (divisor > 1) {
        num /= divisor;
        den /= divisor;
    }
    if (num < INT32_MIN || num > INT32_MAX || den > INT32_MAX) {
        return false;
    }
    out = {static_cast<int32_t>(num), static_cast<int32_t>(den)};
    return true;
}

// F1.3 -- the Inspector's curve grid. A channel is on the grid when it has
// exactly its five points: (0, 0), (1/4, y1), (1/2, y2), (3/4, y3), (1, 1),
// compared exactly. `deviations` receives y - x at the three inner points.
inline bool ReadCurveGrid(const ClipEffect& effect, const std::string& channel,
                          std::array<EffectParamValue, 3>& deviations) {
    std::vector<ColorCurvePoint> points;
    std::string error;
    if (!ReadColorCurveChannel(effect, channel, points, error) ||
        points.size() != 5) {
        return false;
    }
    const auto equals = [](const EffectParamValue& value, int32_t num,
                           int32_t den) {
        return static_cast<int64_t>(value.num) * den ==
               static_cast<int64_t>(num) * value.den;
    };
    for (int32_t index = 0; index < 5; ++index) {
        if (!equals(points[static_cast<size_t>(index)].x, index, 4)) {
            return false;
        }
    }
    if (!equals(points.front().y, 0, 1) || !equals(points.back().y, 1, 1)) {
        return false;
    }
    for (size_t index = 0; index < 3; ++index) {
        // y - x, x = n/4, as one exact fraction.
        const EffectParamValue& y = points[index + 1].y;
        if (!ReducedFraction(
                (static_cast<int64_t>(y.num) * 4) -
                    (static_cast<int64_t>(kCurveGridQuarters.at(index)) *
                     y.den),
                static_cast<int64_t>(y.den) * 4, deviations.at(index))) {
            return false;
        }
    }
    return true;
}

inline const ClipEffect* FindFirstEffect(const std::vector<ClipEffect>& effects,
                                         const std::string& type) {
    for (const ClipEffect& effect : effects) {
        if (effect.type == type) {
            return &effect;
        }
    }
    return nullptr;
}

// GR1 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The stage of `type` a
// control writes to: the clip's first, or a new one at its canonical place.
// The reference stays valid until `effects` changes again.
inline ClipEffect& AddGradeStage(std::vector<ClipEffect>& effects,
                                 const std::string& type) {
    ClipEffect added;
    added.type = type;
    const size_t index = CanonicalColorStageIndex(effects, type);
    return *effects.insert(effects.begin() + static_cast<ptrdiff_t>(index),
                           std::move(added));
}

inline ClipEffect& GradeStage(std::vector<ClipEffect>& effects,
                              const std::string& type) {
    for (ClipEffect& effect : effects) {
        if (effect.type == type) {
            return effect;
        }
    }
    return AddGradeStage(effects, type);
}

// Whether a slider can edit the clip as it is: always, except a curve
// channel that already has points off the Inspector's grid (see the file
// comment).
inline bool IsGradeControlEditable(const std::vector<ClipEffect>& effects,
                                   const GradeControlSpec& spec) {
    if (std::string(spec.type) != "color.curves" || spec.param == nullptr) {
        return true;
    }
    const ClipEffect* curves = FindFirstEffect(effects, spec.type);
    if (curves == nullptr) {
        return true;
    }
    std::vector<ColorCurvePoint> points;
    std::string error;
    std::array<EffectParamValue, 3> deviations{};
    return (ReadColorCurveChannel(*curves, spec.param, points, error) &&
            points.empty()) ||
           ReadCurveGrid(*curves, spec.param, deviations);
}

// Reads a knob's current value out of a clip's effect stack, honoring the
// registry's default (ColorEffects.h) when the clip carries no entry for
// this type -- the same rule ResolveColorGrade applies at render time,
// kept consistent here so a freshly opened Inspector's slider position
// always matches what the renderer is already drawing. A wheel knob reads
// its param (0 when absent); a curve slider reads how far the channel's
// curve, as the renderer evaluates it, sits from the diagonal at its x.
// G7 -- `neutralKelvin` is what color.temperature sits at on a clip
// without the stage: the clip's as-shot white balance
// (ColorGradeNeutralKelvin), 6500 K when unknown.
inline float CurrentGradeControlValue(const std::vector<ClipEffect>& effects,
                                      const GradeControlSpec& spec,
                                      int32_t neutralKelvin = kNeutralKelvin) {
    const std::string type = spec.type;
    if (type == "color.wheels" && spec.param != nullptr) {
        const ClipEffect* wheels = FindFirstEffect(effects, type);
        if (wheels == nullptr) {
            return 0.0f;
        }
        const auto param = wheels->params.find(spec.param);
        return param == wheels->params.end()
                   ? 0.0f
                   : EffectParamValueToFloat(param->second);
    }
    if (type == "color.curves" && spec.param != nullptr) {
        const ClipEffect* curves = FindFirstEffect(effects, type);
        if (curves == nullptr || spec.curve_point < 0 ||
            spec.curve_point >= 3) {
            return 0.0f;
        }
        std::array<EffectParamValue, 3> deviations{};
        if (ReadCurveGrid(*curves, spec.param, deviations)) {
            return EffectParamValueToFloat(
                deviations.at(static_cast<size_t>(spec.curve_point)));
        }
        // Off the grid: the curve as drawn, through its channel alone.
        ClipEffect channel{curves->id, type, {}};
        const std::string prefix = std::string(spec.param) + "_";
        for (const auto& [name, value] : curves->params) {
            if (name.rfind(prefix, 0) == 0) {
                channel.params["master_" + name.substr(prefix.size())] = value;
            }
        }
        const float x = static_cast<float>(kCurveGridQuarters.at(
                            static_cast<size_t>(spec.curve_point))) /
                        4.0f;
        // A curve reads no context: only color.temperature does.
        const ResolvedColorGrade grade = ResolveColorGrade({channel}, {});
        const std::array<float, 3> shown =
            ApplyResolvedColorGrade({x, x, x}, grade);
        return shown[0] - x;
    }
    if (type == "color.contrast" && spec.param != nullptr) {
        // GR6 -- the pivot: the stage's own, or the one a contrast without
        // one pivots about.
        const ClipEffect* contrast = FindFirstEffect(effects, type);
        if (contrast == nullptr) {
            return kContrastPivot;
        }
        const auto param = contrast->params.find(spec.param);
        return param == contrast->params.end()
                   ? kContrastPivot
                   : EffectParamValueToFloat(param->second);
    }
    const ColorEffectRegistryEntry* entry =
        FindColorEffectRegistryEntry(spec.type);
    const float fallback =
        type == "color.temperature"
            ? static_cast<float>(neutralKelvin)
            : (entry != nullptr ? entry->default_value : 0.0f);
    for (const ClipEffect& effect : effects) {
        if (effect.type != spec.type) {
            continue;
        }
        if (entry == nullptr || entry->param_name == nullptr) {
            return fallback;
        }
        const auto param = effect.params.find(entry->param_name);
        if (param == effect.params.end()) {
            return fallback;
        }
        return EffectParamValueToFloat(param->second);
    }
    return fallback;
}

// Pure function: returns a *new* effects vector with `spec`'s knob set to
// `sliderValue` (quantized per QuantizeGradeControlValue), leaving every
// other effect and the stack's order untouched. Updates the first existing
// entry of `spec.type` in place if present, otherwise adds a new ClipEffect
// at its canonical place (GR1, CanonicalColorStageIndex in ColorEffects.h):
// the stack no longer depends on which control was touched first. Never
// mutates `effects` and never touches EditLog/Operations
// -- the caller (an AppKit control's action) wraps the result in
// SetClipEffectsOperation and submits it through EditLog::Apply, exactly
// like every other edit in this codebase (PHILOSOPHY.md principle 2/3).
//
// F1.3 -- a wheel slider writes its one param. A curve slider rewrites its
// channel as the five grid points, the other two inner points kept; back
// at the diagonal on all three, the channel's points are removed, which is
// the identity. A curve channel IsGradeControlEditable refuses is returned
// unchanged.
// Visual QA of 19 September 2026 -- nothing brought a slider back to where
// it started, and resetting 32 of them by hand was the complaint. The value
// a slider shows on an ungraded clip: its knob's neutral.
inline float NeutralGradeControlValue(const GradeControlSpec& spec,
                                      int32_t neutralKelvin = kNeutralKelvin) {
    return CurrentGradeControlValue({}, spec, neutralKelvin);
}

// The clip with every grading stage removed, in one edit; any other effect
// is kept, in order. GR8 -- every stage the engine renders, not only those a
// slider drives: the hue curves have an editor and no slider.
inline std::vector<ClipEffect> WithoutGrade(
    const std::vector<ClipEffect>& effects) {
    std::vector<ClipEffect> kept;
    for (const ClipEffect& effect : effects) {
        if (FindColorEffectRegistryEntry(effect.type) == nullptr) {
            kept.push_back(effect);
        }
    }
    return kept;
}

// Grading is for pictures: on an audio or caption track the Inspector's
// grade sliders are shown but not editable (visual QA: an audio clip showed
// all of them live).
inline bool IsGradableTrackKind(const std::string& kind) {
    return kind == "video";
}

// PRD37 -- ROADMAP.md. Whether « Réinitialiser l'étalonnage » takes a
// click: the clip takes a grade and carries a stage of one.
inline bool CanResetGrade(bool gradable,
                          const std::vector<ClipEffect>& effects) {
    return gradable && WithoutGrade(effects).size() != effects.size();
}

// PRD37 -- what the panel shows again once one of its own edits has been
// applied, or refused. The QA of 3 October 2026 saw it show the stack as
// it was at the last selection: « Réinitialiser l'étalonnage » off on a
// graded clip, a master bar in the middle under a typed 1.50, a wheel that
// did not follow its sliders nor they it. Everything is read again from the
// clip's stack, except a control whose own commit is still pending: it
// keeps what the hand left, and that commit lands on the stack read here.
// Nothing pending is dropped. The full reload a new selection makes drops
// every pending commit, which is right for a clip one has left and wrong
// here: of two sliders moved within the debounce, the first commit would
// take the second away.
struct OwnEditRefresh {
    bool reset_enabled = false;
    bool rewrite_opacity = false;
    // EF3 -- the « Flou » slider, by the same rule as the opacity's.
    bool rewrite_blur = false;
    // One per GradeControls() entry.
    std::vector<bool> rewrite_sliders;
};

inline OwnEditRefresh RefreshAfterOwnEdit(
    bool gradable, const std::vector<ClipEffect>& effects, bool opacityPending,
    const std::vector<bool>& slidersPending, bool blurPending = false) {
    OwnEditRefresh refresh;
    refresh.reset_enabled = CanResetGrade(gradable, effects);
    refresh.rewrite_opacity = !opacityPending;
    refresh.rewrite_blur = !blurPending;
    refresh.rewrite_sliders.assign(GradeControls().size(), true);
    for (size_t index = 0; index < refresh.rewrite_sliders.size() &&
                           index < slidersPending.size();
         ++index) {
        refresh.rewrite_sliders[index] = !slidersPending[index];
    }
    return refresh;
}

// Visual QA of 19 September 2026 -- a grade slider's value, shown by the
// Record monitor while it is dragged and before the commit that ends the
// drag. Belongs to the interface only: never written to the document.
struct GradePreview {
    Ulid clip_id;
    std::vector<ClipEffect> effects;
};

// The effects the monitor draws `clip` with: the preview when it is this
// clip's, its own otherwise.
inline const std::vector<ClipEffect>& DisplayedEffects(
    const DocumentClip& clip, const std::optional<GradePreview>& preview) {
    return preview && preview->clip_id == clip.id ? preview->effects
                                                  : clip.effects;
}

inline std::vector<ClipEffect> WithGradeControlValue(
    const std::vector<ClipEffect>& effects, const GradeControlSpec& spec,
    float sliderValue) {
    const ColorEffectRegistryEntry* entry =
        FindColorEffectRegistryEntry(spec.type);
    const bool curve =
        std::string(spec.type) == "color.curves" && spec.param != nullptr;
    const std::string paramName =
        spec.param != nullptr
            ? spec.param
            : (entry != nullptr && entry->param_name != nullptr
                   ? entry->param_name
                   : "amount");
    const EffectParamValue quantized =
        QuantizeGradeControlValue(spec, sliderValue);
    if (curve && (!IsGradeControlEditable(effects, spec) ||
                  spec.curve_point < 0 || spec.curve_point >= 3)) {
        return effects;
    }

    std::vector<ClipEffect> result = effects;
    ClipEffect* target = nullptr;
    for (ClipEffect& effect : result) {
        if (effect.type == spec.type) {
            target = &effect;
            break;
        }
    }
    // GR6 -- a pivot brought back to the default is no pivot: the stage
    // then renders, byte for byte, as one that never had the knob, and one
    // left without any knob goes.
    const bool defaultPivot =
        std::string(spec.type) == "color.contrast" && spec.param != nullptr &&
        quantized.num == QuantizeGradeControlValue(spec, kContrastPivot).num;
    if (defaultPivot) {
        if (target == nullptr) {
            return result;
        }
        target->params.erase(paramName);
        if (target->params.empty()) {
            result.erase(result.begin() + (target - result.data()));
        }
        return result;
    }
    if (target == nullptr) {
        target = &AddGradeStage(result, spec.type);
    }
    if (!curve) {
        target->params[paramName] = quantized;
        return result;
    }

    std::array<EffectParamValue, 3> deviations{};
    if (!ReadCurveGrid(*target, paramName, deviations)) {
        deviations.fill({0, 1});
    }
    deviations.at(static_cast<size_t>(spec.curve_point)) = quantized;
    // y = n/4 + deviation at the three inner points, checked before any
    // param is touched.
    std::array<EffectParamValue, 3> inner{};
    for (size_t index = 0; index < 3; ++index) {
        const EffectParamValue& deviation = deviations.at(index);
        if (!ReducedFraction(
                (static_cast<int64_t>(kCurveGridQuarters.at(index)) *
                 deviation.den) +
                    (static_cast<int64_t>(deviation.num) * 4),
                static_cast<int64_t>(deviation.den) * 4, inner.at(index))) {
            return effects;
        }
    }
    const std::string prefix = paramName + "_";
    for (auto it = target->params.begin(); it != target->params.end();) {
        it = it->first.rfind(prefix, 0) == 0 ? target->params.erase(it)
                                             : std::next(it);
    }
    if (std::all_of(
            deviations.begin(), deviations.end(),
            [](const EffectParamValue& value) { return value.num == 0; })) {
        return result;
    }
    for (int32_t index = 0; index < 5; ++index) {
        const std::string number = std::to_string(index);
        std::string xName = prefix;
        xName.append("x").append(number);
        std::string yName = prefix;
        yName.append("y").append(number);
        EffectParamValue x{0, 1};
        ReducedFraction(index, 4, x);
        target->params[xName] = x;
        target->params[yName] = index == 0 ? EffectParamValue{0, 1}
                                : index == 4
                                    ? EffectParamValue{1, 1}
                                    : inner.at(static_cast<size_t>(index - 1));
    }
    return result;
}

// The author, 29 October 2026 -- a clip's zoom and position, set in the
// Inspector as in Resolve's Transform, on top of R1's framing
// (ClipFraming.h): the same SetClipFramingOperation set_clip_framing
// writes, so the monitor, the export and the sheets read one rectangle.
enum class FramingControl { Scale, PositionX, PositionY };

// The zoom slider runs in stops, so 1.0 sits in the middle and halving
// and doubling are the same travel: 1/8 to 8.
inline constexpr float kFramingScaleSliderMin = -3.0f;
inline constexpr float kFramingScaleSliderMax = 3.0f;

inline EffectParamValue ReducedFraction(int64_t num, int64_t den) {
    if (den < 0) num = -num, den = -den;
    const int64_t divisor = std::gcd(num < 0 ? -num : num, den);
    if (divisor > 1) num /= divisor, den /= divisor;
    return {static_cast<int32_t>(num), static_cast<int32_t>(den)};
}

// A zoom from the slider, to the thousandth, as the opacity is.
inline EffectParamValue FramingScaleFromSlider(float stops) {
    const double clamped =
        std::clamp(stops, kFramingScaleSliderMin, kFramingScaleSliderMax);
    const int64_t num = std::max<int64_t>(
        1, static_cast<int64_t>(std::lround(std::exp2(clamped) * 1000.0)));
    return ReducedFraction(num, 1000);
}

inline float FramingSliderFromScale(const EffectParamValue& scale) {
    const double value = scale.den > 0 && scale.num > 0
                             ? static_cast<double>(scale.num) / scale.den
                             : 1.0;
    return std::clamp(static_cast<float>(std::log2(value)),
                      kFramingScaleSliderMin, kFramingScaleSliderMax);
}

// Whole sequence pixels from the centre, as Resolve's Position X and Y,
// and like Resolve the Y counts upward: the document's position_y runs
// down (ClipFraming.h), so the panel flips it at this boundary and
// nowhere else. The slider covers one frame either way.
inline EffectParamValue FramingPositionFromPixels(FramingControl control,
                                                  float pixels,
                                                  int32_t frameSize) {
    if (frameSize <= 0) return {0, 1};
    const double bound = static_cast<double>(frameSize);
    int64_t whole = static_cast<int64_t>(
        std::lround(std::clamp(static_cast<double>(pixels), -bound, bound)));
    if (control == FramingControl::PositionY) whole = -whole;
    return ReducedFraction(whole, frameSize);
}

inline float FramingPixelsFromPosition(FramingControl control,
                                       const EffectParamValue& position,
                                       int32_t frameSize) {
    if (position.den <= 0 || frameSize <= 0) return 0.0f;
    const double pixels =
        static_cast<double>(position.num) * frameSize / position.den;
    return static_cast<float>(control == FramingControl::PositionY ? -pixels
                                                                   : pixels);
}

// The slider's value for `control`: stops for the zoom, pixels otherwise.
inline float FramingSliderValue(const ClipFraming& framing,
                                FramingControl control, int32_t width,
                                int32_t height) {
    switch (control) {
        case FramingControl::Scale:
            return FramingSliderFromScale(framing.scale);
        case FramingControl::PositionX:
            return FramingPixelsFromPosition(control, framing.position_x,
                                             width);
        case FramingControl::PositionY:
            return FramingPixelsFromPosition(control, framing.position_y,
                                             height);
    }
    return 0.0f;
}

// `framing` with one control moved to the slider's value, in its smallest
// terms (ReduceClipFraming), every other field kept.
inline ClipFraming WithFramingControl(ClipFraming framing,
                                      FramingControl control, float sliderValue,
                                      int32_t width, int32_t height) {
    switch (control) {
        case FramingControl::Scale:
            framing.scale = FramingScaleFromSlider(sliderValue);
            break;
        case FramingControl::PositionX:
            framing.position_x =
                FramingPositionFromPixels(control, sliderValue, width);
            break;
        case FramingControl::PositionY:
            framing.position_y =
                FramingPositionFromPixels(control, sliderValue, height);
            break;
    }
    return ReduceClipFraming(framing);
}

inline bool SameFraction(const EffectParamValue& left,
                         const EffectParamValue& right) {
    return static_cast<int64_t>(left.num) * right.den ==
           static_cast<int64_t>(right.num) * left.den;
}

inline bool SameFraming(const ClipFraming& left, const ClipFraming& right) {
    return left.mode == right.mode && SameFraction(left.scale, right.scale) &&
           SameFraction(left.position_x, right.position_x) &&
           SameFraction(left.position_y, right.position_y) &&
           SameFraction(left.crop_left, right.crop_left) &&
           SameFraction(left.crop_right, right.crop_right) &&
           SameFraction(left.crop_top, right.crop_top) &&
           SameFraction(left.crop_bottom, right.crop_bottom);
}

// Where a double click brings the control back: zoom 1, centred.
inline float NeutralFramingSliderValue(FramingControl) { return 0.0f; }

// A keyframed zoom or position is sampled over the static value at every
// frame (ClipAnimation.h): its slider shows and keys the playhead's frame
// instead of the constant (InspectorKeys.h, R2c).
inline bool IsFramingControlAnimated(const DocumentClip& clip,
                                     FramingControl control) {
    const ClipAnimatedParameter parameter =
        control == FramingControl::Scale ? ClipAnimatedParameter::FramingScale
        : control == FramingControl::PositionX
            ? ClipAnimatedParameter::FramingPositionX
            : ClipAnimatedParameter::FramingPositionY;
    for (const ClipParameterAnimation& animation : clip.animation)
        if (animation.parameter == parameter && !animation.keyframes.empty())
            return true;
    return false;
}

// What ApplySetClipFraming accepts: a picture clip of a video track. A
// motion clip is laid out at the sequence size and a carton is drawn in
// the frame; neither is scaled into it.
inline bool CanFrameClip(const DocumentClip& clip, const DocumentTrack* track) {
    return track != nullptr && track->kind == "video" && !IsMotionClip(clip) &&
           clip.caption_group_id.empty();
}

// While a framing slider moves, the monitor shows the framing it would
// commit, as GradePreview does for the grade; nothing is applied. R2c -- on
// a keyed row, the keys it would commit (InspectorKeys.h, PreviewedClip).
struct FramingPreview {
    Ulid clip_id;
    ClipFraming framing;
    std::optional<std::vector<ClipParameterAnimation>> animation;
};

}  // namespace ui::inspector
