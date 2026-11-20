#include "InspectorGradeControls.h"
#include "InspectorKeys.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"

int main() {
    using ui::inspector::ClampToGradeControlRange;
    using ui::inspector::CurrentClipOpacity;
    using ui::inspector::CurrentGradeControlValue;
    using ui::inspector::FindGradeControlSpec;
    using ui::inspector::GradeControls;
    using ui::inspector::IsGradeControlEditable;
    using ui::inspector::QuantizeClipOpacity;
    using ui::inspector::QuantizeGradeControlValue;
    using ui::inspector::WithGradeControlValue;

    // --- G7: the temperature sits at the clip's as-shot white balance ---
    {
        const auto* temperature = FindGradeControlSpec("color.temperature");
        Check(temperature != nullptr &&
                  CurrentGradeControlValue({}, *temperature, 4500) == 4500.0f &&
                  ui::inspector::NeutralGradeControlValue(*temperature, 4500) ==
                      4500.0f &&
                  ui::inspector::NeutralGradeControlValue(*temperature) ==
                      6500.0f,
              "without a stage, the temperature slider shows the as-shot "
              "balance, 6500 K when unknown");
    }

    // --- Visual QA of 19 September 2026: neutral, reset, pictures only ---
    {
        const auto* exposure = FindGradeControlSpec("color.exposure");
        const auto* lift = FindGradeControlSpec("color.wheels", "lift_r");
        Check(exposure != nullptr && lift != nullptr &&
                  ui::inspector::NeutralGradeControlValue(*exposure) ==
                      CurrentGradeControlValue({}, *exposure) &&
                  ui::inspector::NeutralGradeControlValue(*lift) == 0.0f,
              "a slider's neutral is what an ungraded clip shows");
        std::vector<ClipEffect> effects;
        if (exposure != nullptr && lift != nullptr) {
            effects = WithGradeControlValue(effects, *exposure, 0.5f);
            effects = WithGradeControlValue(effects, *lift, 0.1f);
        }
        ClipEffect other;
        other.type = "transform.crop";
        effects.push_back(other);
        const std::vector<ClipEffect> reset =
            ui::inspector::WithoutGrade(effects);
        Check(reset.size() == 1 && reset.front().type == "transform.crop",
              "resetting removes every graded knob and keeps other effects");
        Check(ui::inspector::IsGradableTrackKind("video") &&
                  !ui::inspector::IsGradableTrackKind("audio") &&
                  !ui::inspector::IsGradableTrackKind("caption"),
              "only a picture is graded");
        DocumentClip clip;
        clip.effects = reset;
        std::optional<ui::inspector::GradePreview> preview;
        Check(&ui::inspector::DisplayedEffects(clip, preview) == &clip.effects,
              "without a drag the monitor draws the clip's own grade");
        preview = ui::inspector::GradePreview{GenerateUlid(), effects};
        Check(&ui::inspector::DisplayedEffects(clip, preview) == &clip.effects,
              "another clip's preview leaves this one alone");
        preview->clip_id = clip.id;
        Check(&ui::inspector::DisplayedEffects(clip, preview) ==
                  &preview->effects,
              "a drag on this clip is drawn before it is committed");
    }

    // --- Spec table shape -----------------------------------------------
    Check(GradeControls().size() == 10 + 16 + 12,
          "ten scalar sliders, sixteen wheel sliders, twelve curve sliders");
    size_t scalars = 0;
    for (const auto& spec : GradeControls()) {
        Check(FindColorEffectRegistryEntry(spec.type) != nullptr,
              std::string("every GradeControlSpec type resolves in "
                          "ColorEffectRegistry(): ") +
                  spec.type);
        if (spec.param == nullptr) {
            ++scalars;
            Check(
                FindGradeControlSpec(spec.type) == &spec,
                std::string("lookup returns the exact spec for ") + spec.type);
        } else {
            Check(FindGradeControlSpec(spec.type, spec.param,
                                       spec.curve_point) == &spec,
                  std::string("lookup by knob returns the exact spec for ") +
                      spec.label);
        }
        Check(spec.min_value < spec.max_value,
              std::string("range is non-degenerate for ") + spec.type);
        Check(spec.precision_den > 0,
              std::string("precision_den is positive for ") + spec.type);
        // The UI smoke test (main.mm) finds the opacity and exposure
        // sliders by their ranges.
        if (spec.param != nullptr) {
            const bool opacityRange =
                spec.min_value == 0.0f && spec.max_value == 1.0f;
            const bool exposureRange =
                spec.min_value == -2.0f && spec.max_value == 2.0f;
            Check(!opacityRange && !exposureRange,
                  std::string("no wheel or curve slider takes the opacity's "
                              "or the exposure's range: ") +
                      spec.label);
        }
    }
    // GR6 -- the eight of F1.3 and the hue rotation; the contrast's pivot is
    // a knob with a name, counted with the wheels' and the curves'.
    Check(scalars == 9, "one slider per scalar effect");
    Check(FindGradeControlSpec("color.curve") == nullptr &&
              FindGradeControlSpec("") == nullptr,
          "unregistered knob types miss cleanly");
    Check(std::string(ui::inspector::GradeControlSectionTitle(
              *FindGradeControlSpec("color.wheels"))) !=
                  ui::inspector::GradeControlSectionTitle(
                      *FindGradeControlSpec("color.exposure")) &&
              std::string(ui::inspector::GradeControlSectionTitle(
                  *FindGradeControlSpec("color.curves"))) !=
                  ui::inspector::GradeControlSectionTitle(
                      *FindGradeControlSpec("color.wheels")),
          "wheels and curves each get their own section");

    // --- Clamping ----------------------------------------------------------
    {
        const ui::inspector::GradeControlSpec* contrast =
            FindGradeControlSpec("color.contrast");
        Check(contrast != nullptr, "color.contrast is registered");
        Check(ClampToGradeControlRange(*contrast, 5.0f) == 1.0f,
              "clamps above max_value");
        Check(ClampToGradeControlRange(*contrast, -5.0f) == -1.0f,
              "clamps below min_value");
        Check(ClampToGradeControlRange(*contrast, 0.25f) == 0.25f,
              "in-range values pass through unchanged");
    }

    // --- The exact float -> EffectParamValue boundary ----------------------
    {
        const EffectParamValue opacity = QuantizeClipOpacity(0.4254f);
        Check(opacity.num == 425 && opacity.den == 1000,
              "clip opacity quantizes deterministically to 1/1000");
        Check(CurrentClipOpacity(opacity) == 0.425f,
              "clip opacity reads back through the render float boundary");
        Check(QuantizeClipOpacity(-1.0f).num == 0 &&
                  QuantizeClipOpacity(2.0f).num == 1000,
              "clip opacity clamps to the canonical [0, 1] range");
    }
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. The « Flou » slider's value
    // crosses into the document at a tenth of a pixel, reduced.
    {
        const EffectParamValue blur = ui::inspector::QuantizeClipBlur(12.34f);
        Check(blur.num == 123 && blur.den == 10,
              "a blur quantizes to a tenth of a pixel: 12.34 is 123/10");
        const EffectParamValue whole = ui::inspector::QuantizeClipBlur(10.0f);
        Check(whole.num == 10 && whole.den == 1,
              "a whole blur is stored reduced, as set_clip_blur stores it");
        const EffectParamValue none = ui::inspector::QuantizeClipBlur(0.02f);
        Check(none.num == 0 && none.den == 1,
              "a slider at rest removes the blur, spelled 0/1 so the file "
              "omits it");
        Check(ui::inspector::QuantizeClipBlur(-3.0f).num == 0 &&
                  ui::inspector::QuantizeClipBlur(500.0f).num ==
                      static_cast<int32_t>(
                          ui::inspector::kInspectorBlurMaximum) &&
                  ui::inspector::kInspectorBlurMaximum <= kMaxClipBlur,
              "the slider clamps to its range, within the engine's bound");
        Check(ui::inspector::CurrentClipBlur({25, 2}) == 12.5f &&
                  ui::inspector::CurrentClipBlur({0, 1}) == 0.0f &&
                  ui::inspector::CurrentClipBlur({1, 0}) == 0.0f,
              "a stored blur reads back through the render float boundary");
        const ui::inspector::OwnEditRefresh pendingBlur =
            ui::inspector::RefreshAfterOwnEdit(true, {}, false, {}, true);
        const ui::inspector::OwnEditRefresh settledBlur =
            ui::inspector::RefreshAfterOwnEdit(true, {}, false, {});
        Check(!pendingBlur.rewrite_blur && pendingBlur.rewrite_opacity &&
                  settledBlur.rewrite_blur,
              "after its own edit the panel keeps a « Flou » whose commit "
              "is pending, and shows it again otherwise");
    }
    {
        const ui::inspector::GradeControlSpec* exposure =
            FindGradeControlSpec("color.exposure");
        const EffectParamValue value =
            QuantizeGradeControlValue(*exposure, 0.35f);
        Check(value.den == 1000, "exposure quantizes to its declared 1/1000");
        Check(value.num == 350, "0.35 becomes an exact 350/1000");
        Check(EffectParamValueToFloat(value) == 0.35f,
              "reading the committed value back reproduces the exact same "
              "float the slider produced -- no drift across the boundary");
    }
    {
        // Temperature commits whole Kelvin (den == 1), matching
        // ColorEffects.cc's own "kelvin" convention exactly.
        const ui::inspector::GradeControlSpec* temperature =
            FindGradeControlSpec("color.temperature");
        const EffectParamValue value =
            QuantizeGradeControlValue(*temperature, 5600.4f);
        Check(value.den == 1, "temperature commits without a denominator");
        Check(value.num == 5600, "rounds to the nearest whole Kelvin");
    }
    {
        // A value dragged back to a previous position quantizes to the
        // identical fraction both times -- required for undo/redo and for
        // canonical-JSON determinism (PHILOSOPHY.md principle 6): the same
        // slider gesture must produce byte-identical committed state.
        const ui::inspector::GradeControlSpec* saturation =
            FindGradeControlSpec("color.saturation");
        const EffectParamValue first =
            QuantizeGradeControlValue(*saturation, -0.6f);
        const EffectParamValue second =
            QuantizeGradeControlValue(*saturation, -0.6f);
        Check(first.num == second.num && first.den == second.den,
              "quantizing the same float twice is deterministic");
    }
    {
        // An out-of-range slider position (defensive: a caller bug, or a
        // control that briefly overshoots during a drag) still commits an
        // exact, in-range fraction rather than an out-of-range one.
        const ui::inspector::GradeControlSpec* vibrance =
            FindGradeControlSpec("color.vibrance");
        const EffectParamValue value =
            QuantizeGradeControlValue(*vibrance, 50.0f);
        Check(EffectParamValueToFloat(value) == vibrance->max_value,
              "an out-of-range slider value is clamped before quantizing");
    }

    // --- Reading the current value, including registry defaults -----------
    {
        const ui::inspector::GradeControlSpec* contrast =
            FindGradeControlSpec("color.contrast");
        Check(CurrentGradeControlValue({}, *contrast) == 0.0f,
              "no effects at all falls back to the registry default");
        const std::vector<ClipEffect> effects = {
            {GenerateUlid(), "color.contrast", {{"amount", {-250, 1000}}}},
        };
        Check(CurrentGradeControlValue(effects, *contrast) == -0.25f,
              "an existing effect's value crosses the exact->float "
              "boundary the same way ColorEffects.h does");
        const std::vector<ClipEffect> missingParam = {
            {GenerateUlid(), "color.contrast", {}},
        };
        Check(CurrentGradeControlValue(missingParam, *contrast) == 0.0f,
              "an effect present but missing its param falls back to the "
              "registry default, same rule as ResolveColorGrade");
    }

    // --- WithGradeControlValue: the pure effects-vector builder ------------
    {
        const ui::inspector::GradeControlSpec* exposure =
            FindGradeControlSpec("color.exposure");
        const std::vector<ClipEffect> updated =
            WithGradeControlValue({}, *exposure, 0.5f);
        Check(updated.size() == 1 && updated[0].type == "color.exposure",
              "a knob absent from an empty stack is appended");
        Check(EffectParamValueToFloat(updated[0].params.at("amount")) == 0.5f,
              "the appended effect carries the quantized committed value");
    }
    {
        // Updating an existing knob edits in place: same id, same position,
        // every other effect untouched -- SetClipEffectsOperation replaces
        // the whole vector, so preserving identity/order here matters for
        // anything else (future tooling, a second Inspector row) that
        // might key off effect id or stack position.
        const Ulid exposureId = GenerateUlid();
        const Ulid contrastId = GenerateUlid();
        const std::vector<ClipEffect> effects = {
            {exposureId, "color.exposure", {{"amount", {100, 1000}}}},
            {contrastId, "color.contrast", {{"amount", {-200, 1000}}}},
        };
        const ui::inspector::GradeControlSpec* exposure =
            FindGradeControlSpec("color.exposure");
        const std::vector<ClipEffect> updated =
            WithGradeControlValue(effects, *exposure, -0.8f);
        Check(updated.size() == 2, "updating in place does not add a stage");
        Check(updated[0].id == exposureId && updated[1].id == contrastId,
              "identity and stack order are preserved across an update");
        Check(EffectParamValueToFloat(updated[0].params.at("amount")) == -0.8f,
              "the targeted knob's value changed");
        Check(EffectParamValueToFloat(updated[1].params.at("amount")) == -0.2f,
              "every other knob's value is untouched");
    }
    {
        // The input vector itself is never mutated -- WithGradeControlValue
        // is a pure function, matching ResolveColorGrade's own contract.
        const std::vector<ClipEffect> original = {
            {GenerateUlid(), "color.tint", {{"amount", {0, 1000}}}},
        };
        const std::vector<ClipEffect> before = original;
        const ui::inspector::GradeControlSpec* tint =
            FindGradeControlSpec("color.tint");
        (void)WithGradeControlValue(original, *tint, 0.9f);
        Check(original.size() == before.size() &&
                  original[0].params.at("amount").num ==
                      before[0].params.at("amount").num,
              "the caller's effects vector is left untouched");
    }

    // --- F1.3 wheels
    // ----------------------------------------------------------
    {
        const ui::inspector::GradeControlSpec* gainBlue =
            FindGradeControlSpec("color.wheels", "gain_b");
        const ui::inspector::GradeControlSpec* liftRed =
            FindGradeControlSpec("color.wheels", "lift_r");
        Check(gainBlue != nullptr && liftRed != nullptr,
              "wheel sliders are found by their param");
        if (gainBlue != nullptr && liftRed != nullptr) {
            const Ulid exposureId = GenerateUlid();
            std::vector<ClipEffect> effects = {
                {exposureId, "color.exposure", {{"amount", {1, 2}}}}};
            effects = WithGradeControlValue(effects, *gainBlue, 0.3f);
            effects = WithGradeControlValue(effects, *liftRed, -0.05f);
            Check(effects.size() == 2 && effects[0].id == exposureId &&
                      effects[1].type == "color.wheels" &&
                      effects[1].params.size() == 2 &&
                      effects[1].params.at("gain_b").num == 300 &&
                      effects[1].params.at("gain_b").den == 1000 &&
                      effects[1].params.at("lift_r").num == -50,
                  "two wheel sliders write two params of one wheel stage");
            Check(CurrentGradeControlValue(effects, *gainBlue) == 0.3f &&
                      CurrentGradeControlValue(
                          effects, *FindGradeControlSpec("color.wheels",
                                                         "gamma_g")) == 0.0f,
                  "wheel sliders read their param, zero when absent");
            std::string error;
            Check(ValidateColorEffect(effects[1], error),
                  "what the wheel sliders write validates: " + error);
            Check(IsGradeControlEditable(effects, *gainBlue),
                  "wheel sliders are always editable");
        }
    }

    // --- F1.3 curves
    // ----------------------------------------------------------
    {
        const ui::inspector::GradeControlSpec* redMids =
            FindGradeControlSpec("color.curves", "r", 1);
        const ui::inspector::GradeControlSpec* redShadows =
            FindGradeControlSpec("color.curves", "r", 0);
        const ui::inspector::GradeControlSpec* masterHighs =
            FindGradeControlSpec("color.curves", "master", 2);
        Check(redMids != nullptr && redShadows != nullptr &&
                  masterHighs != nullptr,
              "curve sliders are found by channel and grid point");
        if (redMids != nullptr && redShadows != nullptr &&
            masterHighs != nullptr) {
            std::vector<ClipEffect> effects =
                WithGradeControlValue({}, *redMids, 0.1f);
            Check(effects.size() == 1 && effects[0].type == "color.curves" &&
                      effects[0].params.size() == 10 &&
                      effects[0].params.at("r_x2").num == 1 &&
                      effects[0].params.at("r_x2").den == 2 &&
                      effects[0].params.at("r_y2").num == 3 &&
                      effects[0].params.at("r_y2").den == 5 &&
                      effects[0].params.at("r_y1").num == 1 &&
                      effects[0].params.at("r_y1").den == 4,
                  "a curve slider writes its channel as the five grid "
                  "points, as reduced exact fractions");
            std::string error;
            Check(ValidateColorEffect(effects[0], error),
                  "what the curve sliders write validates: " + error);
            const auto shown = ApplyResolvedColorGrade(
                {0.5f, 0.5f, 0.5f}, ResolveColorGrade(effects, {}));
            Check(std::abs(shown[0] - 0.6f) < 1e-6f && shown[1] == 0.5f,
                  "the renderer draws the red curve 0.1 above the diagonal "
                  "at 1/2, the other channels untouched");
            Check(std::abs(CurrentGradeControlValue(effects, *redMids) - 0.1f) <
                          1e-6f &&
                      CurrentGradeControlValue(effects, *redShadows) == 0.0f,
                  "curve sliders read their point back");
            effects = WithGradeControlValue(effects, *redShadows, -0.05f);
            Check(effects[0].params.at("r_y1").num == 1 &&
                      effects[0].params.at("r_y1").den == 5 &&
                      effects[0].params.at("r_y2").num == 3,
                  "a second slider keeps the first one's point");
            effects = WithGradeControlValue(effects, *masterHighs, 0.02f);
            effects = WithGradeControlValue(effects, *redShadows, 0.0f);
            effects = WithGradeControlValue(effects, *redMids, 0.0f);
            Check(effects.size() == 1 && effects[0].params.count("r_x0") == 0 &&
                      effects[0].params.size() == 10 &&
                      effects[0].params.count("master_y3") == 1,
                  "back at the diagonal, a channel's points are removed and "
                  "the other channels kept");
        }
        // A curve written elsewhere, off the grid: shown, not editable.
        const std::vector<ClipEffect> agentCurve = {{GenerateUlid(),
                                                     "color.curves",
                                                     {{"g_x0", {0, 1}},
                                                      {"g_y0", {1, 10}},
                                                      {"g_x1", {1, 1}},
                                                      {"g_y1", {9, 10}}}}};
        const ui::inspector::GradeControlSpec* greenMids =
            FindGradeControlSpec("color.curves", "g", 1);
        const ui::inspector::GradeControlSpec* blueMids =
            FindGradeControlSpec("color.curves", "b", 1);
        if (greenMids != nullptr && blueMids != nullptr) {
            Check(!IsGradeControlEditable(agentCurve, *greenMids) &&
                      IsGradeControlEditable(agentCurve, *blueMids),
                  "a channel off the grid is not editable; an empty one is");
            Check(std::abs(CurrentGradeControlValue(agentCurve, *greenMids)) <
                          1e-6f &&
                      std::abs(CurrentGradeControlValue(
                                   agentCurve, *FindGradeControlSpec(
                                                   "color.curves", "g", 0)) -
                               0.05f) < 1e-6f,
                  "an off-grid curve shows its value at the grid points");
            const std::vector<ClipEffect> untouched =
                WithGradeControlValue(agentCurve, *greenMids, 0.2f);
            const auto same = [](const ClipEffect& left,
                                 const ClipEffect& right) {
                if (left.params.size() != right.params.size()) {
                    return false;
                }
                for (const auto& [name, value] : left.params) {
                    const auto other = right.params.find(name);
                    if (other == right.params.end() ||
                        other->second.num != value.num ||
                        other->second.den != value.den) {
                        return false;
                    }
                }
                return true;
            };
            Check(untouched.size() == 1 && same(untouched[0], agentCurve[0]),
                  "a slider cannot rewrite a curve it did not write");
        }
    }

    // --- GR1: the same sliders give the same stack, whatever the order ---
    {
        const std::vector<std::pair<const char*, const char*>> gestures = {
            {"color.saturation", nullptr}, {"color.curves", "master"},
            {"color.contrast", nullptr},   {"color.wheels", "offset_r"},
            {"color.exposure", nullptr},   {"color.temperature", nullptr},
            {"color.shadows", nullptr}};
        const auto spec = [](const std::pair<const char*, const char*>& name) {
            return name.second == nullptr
                       ? FindGradeControlSpec(name.first)
                       : FindGradeControlSpec(
                             name.first, name.second,
                             std::string(name.first) == "color.curves" ? 1
                                                                       : -1);
        };
        const auto types = [](const std::vector<ClipEffect>& effects) {
            std::string joined;
            for (const ClipEffect& effect : effects) {
                joined += effect.type + " ";
            }
            return joined;
        };
        std::vector<size_t> order(gestures.size());
        for (size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::string first;
        bool same = true;
        bool found = true;
        size_t permutations = 0;
        do {
            std::vector<ClipEffect> effects;
            for (const size_t index : order) {
                const auto* control = spec(gestures[index]);
                found = found && control != nullptr;
                if (control != nullptr) {
                    effects = WithGradeControlValue(
                        effects, *control,
                        std::string(control->type) == "color.temperature"
                            ? 5000.0f
                            : 0.1f);
                }
            }
            const std::string stack = types(effects);
            if (permutations == 0) {
                first = stack;
            }
            same = same && stack == first;
            ++permutations;
        } while (std::next_permutation(order.begin(), order.end()));
        Check(found && same && permutations == 5040 &&
                  first ==
                      "color.shadows color.temperature color.exposure "
                      "color.wheels color.contrast color.curves "
                      "color.saturation ",
              "seven controls touched in any of their 5040 orders give one "
              "stack, in the canonical order: " +
                  first);

        // A stack written in another order keeps it, and takes a new stage
        // at its end.
        const auto* exposure = FindGradeControlSpec("color.exposure");
        const auto* shadows = FindGradeControlSpec("color.shadows");
        const auto* contrast = FindGradeControlSpec("color.contrast");
        if (exposure != nullptr && shadows != nullptr && contrast != nullptr) {
            std::vector<ClipEffect> agent;
            agent.push_back({"", "color.saturation", {{"amount", {1, 5}}}});
            agent.push_back({"", "color.tint", {{"amount", {1, 10}}}});
            agent = WithGradeControlValue(agent, *shadows, 0.2f);
            Check(types(agent) == "color.saturation color.tint color.shadows ",
                  "a stack in another order is not reordered: its new stage "
                  "goes last: " +
                      types(agent));
            // An effect of another family stays where it is.
            std::vector<ClipEffect> mixed;
            mixed.push_back({"", "transform.crop", {}});
            mixed = WithGradeControlValue(mixed, *contrast, 0.2f);
            mixed = WithGradeControlValue(mixed, *exposure, 0.2f);
            Check(
                types(mixed) == "transform.crop color.exposure color.contrast ",
                "an exposure set after a contrast goes before it: " +
                    types(mixed));
        }
    }

    // --- GR6: the contrast's pivot and the hue rotation ---
    {
        const auto* contrast = FindGradeControlSpec("color.contrast");
        const auto* pivot = FindGradeControlSpec("color.contrast", "pivot");
        const auto* hue = FindGradeControlSpec("color.hue");
        Check(contrast != nullptr && contrast->param == nullptr &&
                  pivot != nullptr && hue != nullptr,
              "the contrast keeps its slider, and has one for its pivot; the "
              "hue rotation has its own");
        if (contrast != nullptr && pivot != nullptr && hue != nullptr) {
            Check(CurrentGradeControlValue({}, *pivot) == kContrastPivot &&
                      ui::inspector::NeutralGradeControlValue(*pivot) ==
                          kContrastPivot &&
                      !(pivot->min_value == 0.0f && pivot->max_value == 1.0f),
                  "without a pivot the slider shows the one a contrast "
                  "pivots about; its range is not the opacity's");
            std::vector<ClipEffect> effects =
                WithGradeControlValue({}, *contrast, 0.2f);
            effects = WithGradeControlValue(effects, *pivot, 0.3f);
            std::string error;
            Check(effects.size() == 1 &&
                      effects[0].params.at("pivot").num == 300 &&
                      effects[0].params.at("pivot").den == 1000 &&
                      effects[0].params.at("amount").num == 200 &&
                      ValidateColorEffect(effects[0], error) &&
                      std::abs(CurrentGradeControlValue(effects, *pivot) -
                               0.3f) < 1e-6f &&
                      std::abs(CurrentGradeControlValue(effects, *contrast) -
                               0.2f) < 1e-6f,
                  "the pivot's slider writes the pivot of the one contrast "
                  "stage, and leaves its amount: " +
                      error);
            // Back at the default, the knob goes: the stage is the one a
            // clip had before the knob existed.
            const std::vector<ClipEffect> reset = WithGradeControlValue(
                effects, *pivot,
                ui::inspector::NeutralGradeControlValue(*pivot));
            Check(reset.size() == 1 && reset[0].params.count("pivot") == 0 &&
                      reset[0].params.count("amount") == 1,
                  "a pivot back at its default is removed, not written");
            Check(WithGradeControlValue({}, *pivot, kContrastPivot).empty() &&
                      WithGradeControlValue(
                          WithGradeControlValue({}, *pivot, 0.6f), *pivot,
                          kContrastPivot)
                          .empty(),
                  "a pivot alone, reset, leaves no stage");
            const std::vector<ClipEffect> turned =
                WithGradeControlValue({}, *hue, 37.52f);
            Check(turned.size() == 1 && turned[0].type == "color.hue" &&
                      turned[0].params.at("degrees").num == 375 &&
                      turned[0].params.at("degrees").den == 10 &&
                      ui::inspector::WithoutGrade(turned).empty(),
                  "the hue rotation commits tenths of a degree, and the "
                  "reset takes it away");
        }
    }

    // --- GR2: the offset wheel's sliders ---
    {
        const auto* offset = FindGradeControlSpec("color.wheels", "offset_b");
        const auto* master =
            FindGradeControlSpec("color.wheels", "offset_master");
        Check(offset != nullptr && master != nullptr,
              "the offset wheel has its sliders");
        if (offset != nullptr && master != nullptr) {
            std::vector<ClipEffect> effects =
                WithGradeControlValue({}, *offset, -0.04f);
            effects = WithGradeControlValue(effects, *master, 0.02f);
            std::string error;
            Check(effects.size() == 1 && effects[0].type == "color.wheels" &&
                      effects[0].params.at("offset_b").num == -40 &&
                      effects[0].params.at("offset_b").den == 1000 &&
                      effects[0].params.at("offset_master").num == 20 &&
                      ValidateColorEffect(effects[0], error) &&
                      std::abs(CurrentGradeControlValue(effects, *offset) +
                               0.04f) < 1e-6f,
                  "an offset slider writes its knob of the one wheel stage: " +
                      error);
        }
    }

    // --- PRD37: what the panel shows again after one of its own edits ---
    {
        const auto* exposure = FindGradeControlSpec("color.exposure");
        const auto* gain = FindGradeControlSpec("color.wheels", "gain_master");
        Check(exposure != nullptr && gain != nullptr,
              "the exposure and the gain master have their sliders");
        if (exposure != nullptr && gain != nullptr) {
            const std::vector<ClipEffect> graded =
                WithGradeControlValue({}, *exposure, 0.5f);
            ClipEffect blur;
            blur.type = "blur";
            Check(ui::inspector::CanResetGrade(true, graded) &&
                      !ui::inspector::CanResetGrade(true, {}) &&
                      !ui::inspector::CanResetGrade(true, {blur}) &&
                      !ui::inspector::CanResetGrade(false, graded),
                  "« Réinitialiser l'étalonnage » takes a click on a graded "
                  "picture only");

            const size_t gainIndex =
                static_cast<size_t>(gain - GradeControls().data());
            std::vector<bool> pending(GradeControls().size(), false);
            pending[gainIndex] = true;
            const ui::inspector::OwnEditRefresh refresh =
                ui::inspector::RefreshAfterOwnEdit(true, graded, true, pending);
            size_t rewritten = 0;
            for (bool rewrite : refresh.rewrite_sliders) rewritten += rewrite;
            Check(
                refresh.reset_enabled &&
                    refresh.rewrite_sliders.size() == GradeControls().size() &&
                    !refresh.rewrite_sliders[gainIndex] &&
                    rewritten == GradeControls().size() - 1 &&
                    !refresh.rewrite_opacity,
                "after its own edit the panel shows the stack again, the "
                "reset button on, except the controls whose commit is "
                "still pending");

            const ui::inspector::OwnEditRefresh settled =
                ui::inspector::RefreshAfterOwnEdit(true, {}, false, {});
            Check(!settled.reset_enabled && settled.rewrite_opacity &&
                      std::all_of(settled.rewrite_sliders.begin(),
                                  settled.rewrite_sliders.end(),
                                  [](bool rewrite) { return rewrite; }),
                  "with nothing pending, every control is shown again, and "
                  "the reset is off on an ungraded clip");
        }
    }

    // The author, 29 October 2026 -- zoom and position in the Inspector.
    {
        using ui::inspector::FramingControl;
        const EffectParamValue one =
            ui::inspector::FramingScaleFromSlider(0.0f);
        const EffectParamValue doubled =
            ui::inspector::FramingScaleFromSlider(1.0f);
        const EffectParamValue most =
            ui::inspector::FramingScaleFromSlider(9.0f);
        Check(one.num == 1 && one.den == 1 && doubled.num == 2 &&
                  doubled.den == 1 && most.num == 8 && most.den == 1,
              "the zoom slider runs in stops, 1 in the middle, reduced "
              "terms, held to 8");
        Check(std::abs(ui::inspector::FramingSliderFromScale({23, 20}) -
                       static_cast<float>(std::log2(1.15))) < 1e-6f,
              "a stored 23/20 sits where 1.15 does");

        const EffectParamValue right = ui::inspector::FramingPositionFromPixels(
            FramingControl::PositionX, 120.0f, 1920);
        const EffectParamValue up = ui::inspector::FramingPositionFromPixels(
            FramingControl::PositionY, 108.0f, 1080);
        Check(right.num == 1 && right.den == 16 && up.num == -1 && up.den == 10,
              "position is whole sequence pixels, and Y counts upward as "
              "in Resolve while the document runs down");
        Check(ui::inspector::FramingPixelsFromPosition(
                  FramingControl::PositionY, {-1, 10}, 1080) == 108.0f,
              "the document's -1/10 reads back as 108 px up");

        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        framing.crop_left = {1, 10};
        const ClipFraming zoomed = ui::inspector::WithFramingControl(
            framing, FramingControl::Scale, std::log2(1.5f), 1920, 1080);
        Check(zoomed.mode == FramingMode::Fill && zoomed.scale.num == 3 &&
                  zoomed.scale.den == 2 && zoomed.crop_left.num == 1 &&
                  zoomed.crop_left.den == 10 &&
                  ui::inspector::SameFraming(
                      ui::inspector::WithFramingControl(
                          zoomed, FramingControl::Scale, 0.0f, 1920, 1080),
                      framing),
              "a slider moves its own field and keeps the others; back to "
              "neutral is the framing it left");

        DocumentClip clip;
        ClipParameterAnimation push;
        push.parameter = ClipAnimatedParameter::FramingScale;
        push.keyframes.push_back(ClipKeyframe{});
        clip.animation.push_back(push);
        Check(ui::inspector::IsFramingControlAnimated(clip,
                                                      FramingControl::Scale) &&
                  !ui::inspector::IsFramingControlAnimated(
                      clip, FramingControl::PositionX),
              "a keyframed zoom is left to its animation, the position is "
              "not");

        DocumentTrack video;
        video.kind = "video";
        DocumentTrack audio;
        audio.kind = "audio";
        DocumentClip carton;
        carton.caption_group_id = "01CAPTION";
        Check(ui::inspector::CanFrameClip(DocumentClip{}, &video) &&
                  !ui::inspector::CanFrameClip(DocumentClip{}, &audio) &&
                  !ui::inspector::CanFrameClip(carton, &video) &&
                  !ui::inspector::CanFrameClip(DocumentClip{}, nullptr),
              "a picture clip of a video track is framed, sound and "
              "cartons are not");

        std::optional<ui::inspector::FramingPreview> preview =
            ui::inspector::FramingPreview{clip.id, zoomed, std::nullopt};
        DocumentClip other;
        const std::optional<DocumentClip> previewed =
            ui::inspector::PreviewedClip(clip, preview);
        Check(previewed &&
                  ui::inspector::SameFraming(previewed->framing, zoomed) &&
                  previewed->animation.size() == clip.animation.size() &&
                  !ui::inspector::PreviewedClip(other, preview),
              "the monitor shows the dragged framing on its clip only, its "
              "keys kept");
        preview->animation = std::vector<ClipParameterAnimation>{};
        Check(ui::inspector::PreviewedClip(clip, preview)->animation.empty(),
              "and the keys a keyed row's drag would commit");
    }

    // R2c -- docs/roadmap/REEL-2026-09.md. The diamond of a row, the value
    // an animated slider shows, and what a click or a release commits.
    {
        using ui::inspector::KeyedControl;
        const MediaRate rate{25, 1};
        DocumentClip clip;
        clip.id = "01K9R2C0000000000000000C01";
        clip.source_id = "01K9R2C0000000000000000S01";
        clip.duration = RationalTime(50, 25);
        clip.timeline_in = RationalTime(100, 25);
        clip.opacity = {1, 2};
        Check(ui::inspector::ClipLocalFrame(clip, RationalTime(110, 25),
                                            rate) == 10 &&
                  ui::inspector::ClipLocalFrame(clip, RationalTime(90, 25),
                                                rate) == -10,
              "the playhead's frame counts from the clip's head");

        // Unkeyed: the diamond is free on the clip's frames only, and the
        // slider shows the constant.
        ui::inspector::KeyedRowState state = ui::inspector::KeyedRowStateAt(
            clip, KeyedControl::Opacity, true, 10, rate, 1920, 1080);
        Check(state.keyable && !state.animated && !state.key_here &&
                  !state.previous_frame && !state.next_frame &&
                  state.slider_value == 0.5f,
              "an unkeyed opacity: diamond free, slider on the constant");
        Check(!ui::inspector::KeyedRowStateAt(clip, KeyedControl::Opacity, true,
                                              50, rate, 1920, 1080)
                      .keyable &&
                  !ui::inspector::KeyedRowStateAt(clip, KeyedControl::Opacity,
                                                  true, -1, rate, 1920, 1080)
                       .keyable &&
                  !ui::inspector::KeyedRowStateAt(clip, KeyedControl::Opacity,
                                                  false, 10, rate, 1920, 1080)
                       .keyable,
              "no key off the clip, nor on a row that does not edit it");

        // First click: one key holding the value shown, nothing moves.
        Operation operation;
        bool posed = false;
        std::string error;
        Check(ui::inspector::KeyToggleOperation(
                  clip, KeyedControl::Opacity, 10, rate,
                  "01K9R2C0000000000000000K01", operation, posed, error) &&
                  posed &&
                  std::holds_alternative<SetClipAnimationOperation>(operation),
              "the first click poses a key: " + error);
        DocumentClip keyed = clip;
        if (const auto* set =
                std::get_if<SetClipAnimationOperation>(&operation)) {
            keyed.animation = set->clips.at(0).animation;
        }
        Check(keyed.animation.size() == 1 &&
                  keyed.animation[0].keyframes.size() == 1 &&
                  keyed.animation[0].keyframes[0].value.num == 1 &&
                  keyed.animation[0].keyframes[0].value.den == 2 &&
                  keyed.animation[0].keyframes[0].time.compare(
                      RationalTime(10, 25)) == 0,
              "one key, on frame 10, holding the 1/2 shown");
        state = ui::inspector::KeyedRowStateAt(keyed, KeyedControl::Opacity,
                                               true, 10, rate, 1920, 1080);
        Check(state.animated && state.key_here && state.slider_value == 0.5f,
              "the diamond is lit on its frame");

        // Elsewhere, the slider keys the frame it is let go on.
        std::vector<ClipParameterAnimation> animation;
        Check(ui::inspector::KeyedValueAnimation(
                  keyed, KeyedControl::Opacity, 30, rate,
                  ui::inspector::KeyedValueFromSlider(KeyedControl::Opacity,
                                                      0.0f, 1920, 1080),
                  "01K9R2C0000000000000000K02", animation, error) &&
                  animation.size() == 1 && animation[0].keyframes.size() == 2,
              "a released slider on frame 30 adds the second key: " + error);
        keyed.animation = animation;
        state = ui::inspector::KeyedRowStateAt(keyed, KeyedControl::Opacity,
                                               true, 20, rate, 1920, 1080);
        Check(state.animated && !state.key_here && state.previous_frame == 10 &&
                  state.next_frame == 30 &&
                  std::abs(state.slider_value - 0.25f) < 1e-6f,
              "half way, the slider shows 1/4 and the arrows lead to 10 and "
              "30");

        // A key removed: the other stays. The last one removed: its value
        // goes into the field, in the same undo.
        Check(ui::inspector::KeyToggleOperation(
                  keyed, KeyedControl::Opacity, 30, rate,
                  "01K9R2C0000000000000000K03", operation, posed, error) &&
                  !posed &&
                  std::holds_alternative<SetClipAnimationOperation>(operation),
              "the diamond on a key removes it: " + error);
        DocumentClip last = keyed;
        last.animation[0].keyframes.pop_back();
        last.animation[0].keyframes[0].value = {4, 5};
        Check(ui::inspector::KeyToggleOperation(
                  last, KeyedControl::Opacity, 10, rate,
                  "01K9R2C0000000000000000K04", operation, posed, error) &&
                  !posed,
              "the last key is removed: " + error);
        const auto* batch = std::get_if<BatchOperation>(&operation);
        Check(batch != nullptr && batch->operations.size() == 2 &&
                  std::holds_alternative<SetClipAnimationOperation>(
                      batch->operations[0]) &&
                  std::get<SetClipAnimationOperation>(batch->operations[0])
                      .clips.at(0)
                      .animation.empty() &&
                  std::holds_alternative<SetClipOpacityOperation>(
                      batch->operations[1]) &&
                  std::get<SetClipOpacityOperation>(batch->operations[1])
                          .opacity.num == 4,
              "and its 4/5 goes into the opacity field, one batch");
        last.animation[0].keyframes[0].value = {1, 2};
        Check(ui::inspector::KeyToggleOperation(
                  last, KeyedControl::Opacity, 10, rate,
                  "01K9R2C0000000000000000K05", operation, posed, error) &&
                  std::holds_alternative<SetClipAnimationOperation>(operation),
              "no batch when the field already holds the key's value");

        // A framing row keys its own field, in the slider's terms.
        clip.framing.scale = {1, 1};
        Check(ui::inspector::KeyToggleOperation(
                  clip, KeyedControl::Scale, 0, rate,
                  "01K9R2C0000000000000000K06", operation, posed, error) &&
                  posed,
              "the zoom's diamond poses a key: " + error);
        const EffectParamValue zoom = ui::inspector::KeyedValueFromSlider(
            KeyedControl::Scale, std::log2(1.5f), 1920, 1080);
        const EffectParamValue up = ui::inspector::KeyedValueFromSlider(
            KeyedControl::PositionY, 108.0f, 1920, 1080);
        Check(zoom.num == 3 && zoom.den == 2 && up.num == -1 && up.den == 10,
              "a zoom of 1.5 and 108 px up, as the unkeyed rows commit them");
        DocumentClip zoomed = clip;
        zoomed.animation = {};
        Check(ui::inspector::KeyedValueAnimation(
                  clip, KeyedControl::Scale, 0, rate, zoom,
                  "01K9R2C0000000000000000K07", zoomed.animation, error),
              "a zoom keyed: " + error);
        Check(
            std::abs(ui::inspector::KeyedRowStateAt(zoomed, KeyedControl::Scale,
                                                    true, 40, rate, 1920, 1080)
                         .slider_value -
                     std::log2(1.5f)) < 1e-4f,
            "and its slider shows it on every frame");
        Operation field = ui::inspector::FieldOperation(
            clip, ClipAnimatedParameter::FramingPositionX, {1, 8});
        Check(std::holds_alternative<SetClipFramingOperation>(field) &&
                  std::get<SetClipFramingOperation>(field)
                          .clips.at(0)
                          .framing.position_x.den == 8,
              "a framing key's value goes into the framing field");
    }

    if (failures == 0) {
        std::cout << "PASS: inspector grade controls\n";
    } else {
        std::cout << "FAIL: " << failures << " check(s) failed\n";
    }
    return failures == 0 ? 0 : 1;
}
