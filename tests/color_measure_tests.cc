// G1 -- docs/roadmap/GRADE-2026-09.md. The measure itself, on pictures
// whose numbers are known by hand: no FFmpeg, no file.
#include "ColorMeasure.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "TestSupport.h"

namespace {

bool Near(int64_t actual, int64_t expected, int64_t tolerance) {
    return std::llabs(actual - expected) <= tolerance;
}

std::string Show(int64_t value) { return std::to_string(value); }

using Picture = std::vector<std::array<float, 3>>;

Picture Uniform(int32_t width, int32_t height, std::array<float, 3> color) {
    return Picture(static_cast<size_t>(width) * height, color);
}

ColorMeasureChain Unmanaged(const std::vector<ClipEffect>& effects = {}) {
    ColorMeasureChain chain;
    chain.grade = ResolveColorGrade(effects, {});
    return chain;
}

ColorMeasureChain Managed(const std::vector<ClipEffect>& effects = {}) {
    ColorMeasureChain chain;
    chain.color.enabled = true;
    chain.color.input_gamut = "rec709";
    chain.color.input_transfer = "rec709";
    chain.color.working_gamut = "acescct";
    chain.color.output_gamut = "rec709";
    chain.color.output_transfer = "rec709";
    std::string error;
    Check(MakeOpenColorIoColorPath(chain.color, chain.path, error),
          "the managed colour path builds: " + error);
    chain.grade = ResolveColorGrade(effects, {});
    return chain;
}

ClipEffect Effect(const std::string& type,
                  std::map<std::string, EffectParamValue> params) {
    ClipEffect effect;
    effect.type = type;
    effect.params = std::move(params);
    return effect;
}

// A display colour of luma 0.5 whose chroma points at `degrees` on the
// vectorscope, built from the BT.709 definitions the measure reads back.
std::array<float, 3> AtHue(double degrees, double chroma) {
    const double radians = degrees * M_PI / 180.0;
    const double y = 0.5;
    const double cb = chroma * std::cos(radians);
    const double cr = chroma * std::sin(radians);
    const double red = y + 2.0 * (1.0 - 0.2126) * cr;
    const double blue = y + 2.0 * (1.0 - 0.0722) * cb;
    const double green = (y - 0.2126 * red - 0.0722 * blue) / 0.7152;
    return {static_cast<float>(red), static_cast<float>(green),
            static_cast<float>(blue)};
}

bool Measure(const Picture& picture, int32_t width, int32_t height,
             const ColorMeasureChain& chain, const ColorMeasureRegion& region,
             ColorPictureMeasure& whole, ColorPictureMeasure& inRegion,
             std::string& error) {
    return MeasureColorSignal(picture, width, height, chain, region, whole,
                              inRegion, error);
}

ColorMeasureRegion Region(EffectParamValue x, EffectParamValue y,
                          EffectParamValue width, EffectParamValue height) {
    ColorMeasureRegion region;
    region.present = true;
    region.x = x;
    region.y = y;
    region.width = width;
    region.height = height;
    return region;
}

}  // namespace

int main() {
    std::string error;
    ColorPictureMeasure whole;
    ColorPictureMeasure inRegion;
    const ColorMeasureRegion none;

    // ---- a mid grey without colour management -------------------------
    Check(Measure(Uniform(8, 4, {0.5f, 0.5f, 0.5f}), 8, 4, Unmanaged(), none,
                  whole, inRegion, error),
          "a grey picture measures: " + error);
    Check(whole.pixels == 32, "every pixel is measured");
    Check(whole.luma_p02 == 5000 && whole.luma_p50 == 5000 &&
              whole.luma_p98 == 5000,
          "grey 0.5 reads 50 % at every percentile: " + Show(whole.luma_p50));
    Check(whole.clipped == 0 && whole.crushed == 0,
          "nothing of a grey is clipped or crushed");
    Check(whole.saturation_mean == 0 && whole.saturation_p95 == 0,
          "a grey has no chroma");
    Check(whole.zones[1].pixels == 32 && whole.zones[0].pixels == 0 &&
              whole.zones[2].pixels == 0,
          "50 % is a midtone");
    Check(whole.zones[1].red == 5000 && whole.zones[1].warmth == 0 &&
              whole.zones[1].green_magenta == 0,
          "a grey is balanced");
    Check(!whole.scene, "no scene reading without colour management");
    Check(!whole.hue, "a neutral picture has no hue");

    Check(Measure(Uniform(4, 4, {1.0f, 1.0f, 1.0f}), 4, 4, Unmanaged(), none,
                  whole, inRegion, error) &&
              whole.clipped == 10000 && whole.luma_p50 == 10000 &&
              whole.zones[2].pixels == 16,
          "white is clipped everywhere, and a highlight");
    Check(Measure(Uniform(4, 4, {0.0f, 0.0f, 0.0f}), 4, 4, Unmanaged(), none,
                  whole, inRegion, error) &&
              whole.crushed == 10000 && whole.clipped == 0,
          "black is crushed everywhere");

    // ---- a saturated primary ------------------------------------------
    // BT.709 red: Y' = 0.2126, Cb = -0.2126 / 1.8556, Cr = 0.7874 / 1.5748
    // = 0.5, so |(Cb, Cr)| = 0.512959 and 224 times it is 114.90; its angle
    // is 102.91 degrees, 20.09 short of the skin line.
    const ColorMeasureRegion all = Region({0, 1}, {0, 1}, {1, 1}, {1, 1});
    Check(Measure(Uniform(4, 4, {1.0f, 0.0f, 0.0f}), 4, 4, Unmanaged(), all,
                  whole, inRegion, error),
          "a red picture measures: " + error);
    Check(whole.luma_p50 == 2126,
          "red's luma is 21.26 %: " + Show(whole.luma_p50));
    Check(Near(whole.saturation_mean, 11490, 2),
          "red's chroma is 114.90 codes: " + Show(whole.saturation_mean));
    Check(whole.clipped == 10000, "a channel on its top code is clipped");
    Check(inRegion.hue && Near(*inRegion.hue, 10291, 2),
          "red sits at 102.91 degrees: " +
              (inRegion.hue ? Show(*inRegion.hue) : std::string("none")));
    Check(
        inRegion.skin_line_offset && Near(*inRegion.skin_line_offset, -2009, 2),
        "red is 20.09 degrees on the red side of the skin line");

    // ---- the skin line's sign -----------------------------------------
    Check(Measure(Uniform(2, 2, AtHue(123.0, 0.1)), 2, 2, Unmanaged(), all,
                  whole, inRegion, error) &&
              inRegion.skin_line_offset &&
              Near(*inRegion.skin_line_offset, 0, 2),
          "a colour at 123 degrees is on the skin line");
    Check(Measure(Uniform(2, 2, AtHue(140.0, 0.1)), 2, 2, Unmanaged(), all,
                  whole, inRegion, error) &&
              inRegion.skin_line_offset &&
              Near(*inRegion.skin_line_offset, 1700, 2),
          "towards yellow is positive");
    Check(Measure(Uniform(2, 2, AtHue(110.0, 0.1)), 2, 2, Unmanaged(), all,
                  whole, inRegion, error) &&
              inRegion.skin_line_offset &&
              Near(*inRegion.skin_line_offset, -1300, 2),
          "towards red is negative");

    // ---- the grade is measured, not the source ------------------------
    Check(
        Measure(Uniform(4, 4, {1.0f, 0.0f, 0.0f}), 4, 4,
                Unmanaged({Effect("color.saturation", {{"amount", {-1, 1}}})}),
                none, whole, inRegion, error) &&
            whole.saturation_mean <= 1 && Near(whole.luma_p50, 2126, 1),
        "a full desaturation leaves red's luma and no chroma: " +
            Show(whole.saturation_mean));
    Check(Measure(Uniform(4, 4, {0.5f, 0.5f, 0.5f}), 4, 4,
                  Unmanaged({Effect("color.exposure", {{"amount", {1, 1}}})}),
                  none, whole, inRegion, error) &&
              Near(whole.luma_p50, 5571, 1),
          "exposure 1 adds 1/17.52 to the display signal: " +
              Show(whole.luma_p50));
    Check(Measure(Uniform(4, 4, {1.0f, 1.0f, 1.0f}), 4, 4,
                  Unmanaged({Effect("color.exposure", {{"amount", {1, 1}}})}),
                  none, whole, inRegion, error) &&
              whole.luma_p50 == 10000 && whole.clipped == 10000,
          "the grade's result is clamped, as its cube clamps it");

    // ---- balance by zone ----------------------------------------------
    const Picture zoned = {
        {0.1f, 0.1f, 0.1f}, {0.5f, 0.5f, 0.5f}, {0.95f, 0.9f, 0.8f}};
    Check(Measure(zoned, 3, 1, Unmanaged(), none, whole, inRegion, error),
          "a three-zone picture measures: " + error);
    Check(whole.zones[0].pixels == 1 && whole.zones[1].pixels == 1 &&
              whole.zones[2].pixels == 1,
          "one pixel per zone");
    Check(whole.zones[2].red == 9500 && whole.zones[2].blue == 8000 &&
              whole.zones[2].warmth == 1500,
          "warm highlights read as red minus blue");
    Check(whole.zones[2].green_magenta == 250,
          "green minus the mean of red and blue: " +
              Show(whole.zones[2].green_magenta));

    // ---- a region -----------------------------------------------------
    Picture halves(8, {1.0f, 0.0f, 0.0f});
    for (size_t row = 0; row < 2; ++row) {
        halves[row * 4 + 2] = {0.0f, 0.0f, 1.0f};
        halves[row * 4 + 3] = {0.0f, 0.0f, 1.0f};
    }
    Check(Measure(halves, 4, 2, Unmanaged(),
                  Region({0, 1}, {0, 1}, {1, 2}, {1, 1}), whole, inRegion,
                  error) &&
              inRegion.pixels == 4 && inRegion.hue &&
              Near(*inRegion.hue, 10291, 2),
          "the left half is the red one");
    Check(Measure(halves, 4, 2, Unmanaged(),
                  Region({1, 2}, {0, 1}, {1, 2}, {1, 1}), whole, inRegion,
                  error) &&
              inRegion.pixels == 4 && inRegion.hue &&
              Near(*inRegion.hue, 35476, 2),
          "the right half is the blue one, at 354.76 degrees: " +
              (inRegion.hue ? Show(*inRegion.hue) : std::string("none")));
    Check(Measure(halves, 4, 2, Unmanaged(),
                  Region({1, 3}, {0, 1}, {1, 3}, {1, 1}), whole, inRegion,
                  error) &&
              inRegion.pixels == 4,
          "a pixel belongs to the region when its centre does");
    Check(whole.pixels == 8, "the whole picture is still measured");
    Check(!Measure(halves, 4, 2, Unmanaged(),
                   Region({0, 1}, {0, 1}, {1, 10}, {1, 1}), whole, inRegion,
                   error),
          "a region holding no pixel centre is refused");

    std::string refusal;
    Check(!ValidateColorMeasureRegion(Region({0, 1}, {0, 1}, {0, 1}, {1, 1}),
                                      refusal),
          "an empty region is refused");
    Check(!ValidateColorMeasureRegion(Region({1, 2}, {0, 1}, {2, 3}, {1, 1}),
                                      refusal),
          "a region past the right edge is refused");
    Check(!ValidateColorMeasureRegion(Region({-1, 4}, {0, 1}, {1, 2}, {1, 1}),
                                      refusal),
          "a region before the left edge is refused");
    Check(!ValidateColorMeasureRegion(Region({1, -2}, {0, 1}, {1, 2}, {1, 1}),
                                      refusal),
          "a negative denominator is refused");
    Check(ValidateColorMeasureRegion(Region({1, 2}, {1, 2}, {1, 2}, {1, 2}),
                                     refusal),
          "the bottom-right quarter is a region: " + refusal);
    Check(!Measure(Uniform(2, 2, {0.5f, 0.5f, 0.5f}), 3, 2, Unmanaged(), none,
                   whole, inRegion, error),
          "a picture that does not match its size is refused");

    // ---- a look, laid where the chain lays it -------------------------
    auto constant = std::make_shared<LookLutSamples>();
    constant->size = 2;
    constant->rgba.assign(size_t{8} * 4, 0.25f);  // Eight RGBA entries.
    for (size_t index = 3; index < constant->rgba.size(); index += 4)
        constant->rgba[index] = 1.0f;
    ColorMeasureChain looked = Unmanaged();
    looked.look_stage = LookLutStage::Display;
    looked.look = constant;
    Check(Measure(Uniform(4, 4, {0.9f, 0.1f, 0.4f}), 4, 4, looked, none, whole,
                  inRegion, error) &&
              Near(whole.luma_p50, 2500, 1),
          "a look that makes everything 0.25 reads 25 %: " +
              Show(whole.luma_p50));

    // ---- colour management: the scene, in stops ------------------------
    // 18 % grey through the BT.709 camera curve: 1.099 * 0.18^0.45 - 0.099.
    const float grey18 =
        static_cast<float>(1.099 * std::pow(0.18, 0.45) - 0.099);
    ColorMeasureChain managed = Managed();
    Check(Measure(Uniform(4, 4, {grey18, grey18, grey18}), 4, 4, managed, none,
                  whole, inRegion, error),
          "a managed grey measures: " + error);
    Check(whole.scene && whole.scene_p50 && Near(*whole.scene_p50, 0, 3),
          "18 % grey reads 0 stops: " +
              (whole.scene_p50 ? Show(*whole.scene_p50) : std::string("none")));
    Check(whole.saturation_mean <= 5,
          "a managed grey stays neutral: " + Show(whole.saturation_mean));
    const int64_t ungradedLuma = whole.luma_p50;
    const int64_t ungradedStops = whole.scene_p50.value_or(-100000);
    ColorMeasureChain brighter =
        Managed({Effect("color.exposure", {{"amount", {1, 1}}})});
    Check(Measure(Uniform(4, 4, {grey18, grey18, grey18}), 4, 4, brighter, none,
                  whole, inRegion, error) &&
              whole.scene_p50 && Near(*whole.scene_p50 - ungradedStops, 100, 1),
          "exposure 1 under ACEScct moves the scene one stop: " +
              (whole.scene_p50 ? Show(*whole.scene_p50) : std::string("none")));
    Check(whole.luma_p50 > ungradedLuma,
          "and the delivered picture brightens with it");

    // The working-log look sits between the LUT coordinates and the display
    // transform: a constant look is the display transform of that constant.
    ColorMeasureChain managedLook = Managed();
    managedLook.look_stage = LookLutStage::WorkingLog;
    managedLook.look = constant;
    const std::array<float, 3> expected =
        OpenColorIoPathDisplay(managedLook.path, {0.25f, 0.25f, 0.25f});
    const int64_t expectedLuma = std::llround(
        (0.2126 * expected[0] + 0.7152 * expected[1] + 0.0722 * expected[2]) *
        10000.0);
    Check(Measure(Uniform(4, 4, {0.9f, 0.1f, 0.4f}), 4, 4, managedLook, none,
                  whole, inRegion, error) &&
              Near(whole.luma_p50, expectedLuma, 1),
          "the working-log look feeds the display transform: " +
              Show(whole.luma_p50) + " against " + Show(expectedLuma));
    Check(whole.scene, "the scene is read before the look");

    // ---- determinism --------------------------------------------------
    ColorPictureMeasure first;
    ColorPictureMeasure second;
    Check(Measure(zoned, 3, 1, brighter, none, first, inRegion, error) &&
              Measure(zoned, 3, 1, brighter, none, second, inRegion, error) &&
              first.luma_p50 == second.luma_p50 &&
              first.saturation_mean == second.saturation_mean &&
              first.scene_p50 == second.scene_p50 &&
              first.zones[2].warmth == second.zones[2].warmth,
          "two measures of one picture are identical");

    if (failures == 0) std::cout << "color_measure: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
