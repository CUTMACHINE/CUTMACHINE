// G4 -- docs/roadmap/GRADE-2026-09.md. The CDL, held to OpenColorIO's own
// processor: the stages a CDL becomes render what OpenColorIO's no-clamp CDL
// renders, negative values and values above 1 included; a stack folds into
// a CDL that renders what the stack renders; files go out and come back.
#include "ColorCdl.h"
#include "ColorEffects.h"

#include <OpenColorIO/OpenColorIO.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

ClipEffect Stage(const std::string& type,
                 std::map<std::string, EffectParamValue> params) {
    ClipEffect effect;
    effect.type = type;
    effect.params = std::move(params);
    return effect;
}

// Values from below black to above white on every channel, as ACEScct
// codes and linear light have them.
std::vector<std::array<float, 3>> Samples() {
    std::vector<std::array<float, 3>> samples;
    std::mt19937 random(7);
    std::uniform_real_distribution<float> value(-0.25f, 1.4f);
    for (int index = 0; index < 4000; ++index)
        samples.push_back({value(random), value(random), value(random)});
    return samples;
}

// OpenColorIO's no-clamp CDL on one value.
std::array<float, 3> Ocio(const ColorCdl& cdl, std::array<float, 3> color) {
    const OCIO::CDLTransformRcPtr transform = OCIO::CDLTransform::Create();
    transform->setStyle(OCIO::CDL_NO_CLAMP);
    transform->setSlope(cdl.slope.data());
    transform->setOffset(cdl.offset.data());
    transform->setPower(cdl.power.data());
    transform->setSat(cdl.saturation);
    const OCIO::ConstCPUProcessorRcPtr cpu = OCIO::Config::CreateRaw()
                                                 ->getProcessor(transform)
                                                 ->getDefaultCPUProcessor();
    cpu->applyRGB(color.data());
    return color;
}

// The largest gap, over the samples, between OpenColorIO's CDL and a grade.
double Gap(const ColorCdl& cdl, const ResolvedColorGrade& grade) {
    double largest = 0.0;
    for (const auto& sample : Samples()) {
        const auto expected = Ocio(cdl, sample);
        const auto actual = ApplyResolvedColorGrade(sample, grade);
        for (size_t channel = 0; channel < 3; ++channel)
            largest = std::max(
                largest, static_cast<double>(
                             std::abs(expected[channel] - actual[channel])));
    }
    return largest;
}

ResolvedColorGrade Resolved(const std::vector<ClipEffect>& stack) {
    return ResolveColorGrade(stack, {ColorGradeSpace::AcesCct, 0});
}

}  // namespace

int main() {
    // ---- the stages a CDL becomes render OpenColorIO's CDL --------------
    const std::vector<ColorCdl> cdls = {
        {"a",
         "",
         {1.05, 0.98, 0.9},
         {-0.02, 0.01, 0.03},
         {1.1, 1.0, 0.92},
         1.15},
        {"b", "", {0.8, 0.8, 0.8}, {0.05, 0.05, 0.05}, {1.0, 1.0, 1.0}, 0.7},
        {"c", "", {1.3, 1.2, 1.25}, {-0.1, 0.0, 0.02}, {0.8, 0.85, 1.3}, 1.0},
        {"d", "", {2.2, 0.6, 1.0}, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, 2.0},
    };
    for (const ColorCdl& cdl : cdls) {
        std::vector<ClipEffect> stages;
        double deviation = 1.0;
        std::string error;
        const bool made = ColorCdlStages(cdl, stages, deviation, error);
        Check(made && deviation < 2e-6,
              "CDL " + cdl.id + " becomes stages within 2e-6: " + error + " (" +
                  std::to_string(deviation) + ")");
        if (!made) continue;
        for (const ClipEffect& stage : stages)
            Check(ValidateColorEffect(stage, error),
                  "CDL " + cdl.id + " gives a valid " + stage.type + ": " +
                      error);
        const double gap = Gap(cdl, Resolved(stages));
        std::cout << "CDL " << cdl.id << ": stages within " << deviation
                  << ", render within " << gap << " of OpenColorIO\n";
        Check(gap < 1e-4, "CDL " + cdl.id +
                              " renders as OpenColorIO's no-clamp CDL, "
                              "below 0 and above 1 alike: " +
                              std::to_string(gap));
    }
    {
        std::vector<ClipEffect> stages;
        double deviation = 0.0;
        std::string error;
        Check(ColorCdlStages(cdls[0], stages, deviation, error) &&
                  stages.size() == 2 && stages[0].type == "color.wheels" &&
                  stages[0].params.at("lift_r").num == -1 &&
                  stages[0].params.at("lift_r").den == 50 &&
                  stages[1].params.at("amount").num == 3 &&
                  stages[1].params.at("amount").den == 20,
              "the offset and the saturation are the file's decimals exactly");
        Check(ColorCdlStages(ColorCdl{}, stages, deviation, error) &&
                  stages.empty(),
              "an identity CDL is no stage");
        Check(ColorCdlStages(cdls[3], stages, deviation, error) &&
                  stages[0].params.count("gain_master") == 1,
              "a gain past a channel's reach is shared with the master");
        ColorCdl far = ColorCdl{};
        far.slope = {8.0, 1.0, 1.0};
        Check(
            !ColorCdlStages(far, stages, deviation, error) &&
                error.find("gain") != std::string::npos,
            "a gain past the master's reach too is refused by value: " + error);
        ColorCdl dark = ColorCdl{};
        dark.slope = {0.1, 1.0, 1.0};
        dark.offset = {-0.2, 0.0, 0.0};
        Check(!ColorCdlStages(dark, stages, deviation, error),
              "slope + offset at or under 0 is no gain");
        ColorCdl flat = ColorCdl{};
        flat.power = {0.0, 1.0, 1.0};
        Check(!ColorCdlStages(flat, stages, deviation, error),
              "a power of 0 is refused");
    }

    // ---- a stack folds into the CDL it renders --------------------------
    const std::vector<ClipEffect> stack = {
        Stage("color.exposure", {{"amount", {3, 10}}}),
        Stage("color.contrast", {{"amount", {1, 5}}}),
        Stage("color.wheels", {{"lift_b", {1, 50}}, {"gain_r", {1, 10}}}),
        Stage("color.saturation", {{"amount", {1, 4}}}),
        // match_color's order: a tint and an exposure after the saturation.
        Stage("color.tint", {{"amount", {-2, 5}}}),
        Stage("color.exposure", {{"amount", {-1, 10}}}),
    };
    ColorCdl folded;
    std::string error;
    Check(
        FoldColorCdl(Resolved(stack), folded, error),
        "exposure, contrast, a wheel, saturation, then a tint fold: " + error);
    const double foldGap = Gap(folded, Resolved(stack));
    std::cout << "fold: renders within " << foldGap << " of the stack\n";
    Check(foldGap < 1e-4,
          "the fold renders what the stack renders, a tint after the "
          "saturation carried before it: " +
              std::to_string(foldGap));
    // A gamma, the power a CDL applies last in its slope, offset and power,
    // with a saturation after it.
    const std::vector<ClipEffect> bent = {
        Stage("color.exposure", {{"amount", {-1, 5}}}),
        Stage("color.wheels", {{"lift_master", {-1, 50}},
                               {"gamma_g", {1, 10}},
                               {"gamma_master", {-1, 20}}}),
        Stage("color.saturation", {{"amount", {-1, 5}}})};
    ColorCdl bentCdl;
    Check(FoldColorCdl(Resolved(bent), bentCdl, error) &&
              Gap(bentCdl, Resolved(bent)) < 1e-4,
          "an exposure, a wheel with its gamma, then a saturation fold: " +
              error);
    // GR2 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The offset wheel keeps
    // the stage one CDL: slope K - L, offset L + O, power 1/G.
    {
        const std::vector<ClipEffect> printer = {
            Stage("color.wheels", {{"lift_master", {1, 20}},
                                   {"gain_r", {1, 10}},
                                   {"gamma_master", {1, 10}},
                                   {"offset_r", {3, 100}},
                                   {"offset_b", {-1, 50}},
                                   {"offset_master", {1, 100}}}),
            Stage("color.saturation", {{"amount", {1, 10}}})};
        ColorCdl cdl;
        const bool foldedOffset = FoldColorCdl(Resolved(printer), cdl, error);
        Check(foldedOffset &&
                  std::abs(cdl.slope[0] - (std::exp2(0.1) - 0.05)) < 1e-6 &&
                  std::abs(cdl.slope[1] - 0.95) < 1e-6 &&
                  std::abs(cdl.offset[0] - 0.09) < 1e-6 &&
                  std::abs(cdl.offset[1] - 0.06) < 1e-6 &&
                  std::abs(cdl.offset[2] - 0.04) < 1e-6 &&
                  std::abs(cdl.power[0] - std::exp2(-0.1)) < 1e-6,
              "an offset wheel folds into the CDL's offset, the slope kept: " +
                  error);
        Check(foldedOffset && Gap(cdl, Resolved(printer)) < 1e-4,
              "and that CDL renders what the wheel renders");
    }
    {
        std::vector<ClipEffect> stages;
        double deviation = 0.0;
        ColorCdl again;
        Check(ColorCdlStages(folded, stages, deviation, error) &&
                  FoldColorCdl(Resolved(stages), again, error) &&
                  std::abs(again.slope[0] - folded.slope[0]) < 1e-5 &&
                  std::abs(again.offset[1] - folded.offset[1]) < 1e-5 &&
                  std::abs(again.power[2] - folded.power[2]) < 1e-5 &&
                  std::abs(again.saturation - folded.saturation) < 1e-6,
              "a fold comes back from its stages: " + error);
    }

    const auto refused = [&](const std::vector<ClipEffect>& effects,
                             const std::string& named,
                             const std::string& label) {
        ColorCdl ignored;
        std::string reason;
        Check(!FoldColorCdl(Resolved(effects), ignored, reason) &&
                  reason.find(named) != std::string::npos,
              label + ": " + reason);
    };
    refused({Stage("color.temperature", {{"kelvin", {3200, 1}}})},
            "color.temperature", "a white balance has no CDL form");
    refused({Stage("color.curves", {{"master_x0", {0, 1}},
                                    {"master_y0", {1, 10}},
                                    {"master_x1", {1, 1}},
                                    {"master_y1", {1, 1}}})},
            "color.curves", "a curve has none either");
    refused({Stage("color.wheels", {{"gamma_r", {1, 10}}}),
             Stage("color.exposure", {{"amount", {1, 10}}})},
            "color.exposure", "an offset after a gamma is refused");
    refused({Stage("color.wheels", {{"gamma_r", {1, 10}}}),
             Stage("color.wheels", {{"gamma_g", {1, 10}}})},
            "two color.wheels", "two gammas are refused");
    refused({Stage("color.saturation", {{"amount", {1, 2}}}),
             Stage("color.wheels", {{"gain_r", {1, 10}}})},
            "color.wheels", "a per-channel gain after saturation is refused");
    refused({Stage("color.contrast", {{"amount", {-2, 1}}})}, "negative slope",
            "a contrast below -1 is a negative slope");
    // GR6 -- docs/roadmap/GRADE-RESOLVE-2026-10.md.
    refused({Stage("color.hue", {{"degrees", {30, 1}}})}, "color.hue",
            "a hue rotation mixes the channels: no CDL form");
    // GR8.
    refused({Stage("color.hue_curves", {{"hue_sat_x0", {1, 5}},
                                        {"hue_sat_y0", {1, 2}},
                                        {"hue_sat_x1", {2, 5}},
                                        {"hue_sat_y1", {1, 4}}})},
            "color.hue_curves", "a hue curve has no CDL form");
    {
        // A contrast about its own pivot is still one affine move: slope
        // 1 + amount, offset pivot * -amount.
        const std::vector<ClipEffect> pivoted = {
            Stage("color.contrast", {{"amount", {1, 4}}, {"pivot", {3, 10}}})};
        ColorCdl cdl;
        const bool folded = FoldColorCdl(Resolved(pivoted), cdl, error);
        Check(folded && std::abs(cdl.slope[0] - 1.25) < 1e-6 &&
                  std::abs(cdl.offset[1] + 0.075) < 1e-6 &&
                  cdl.power[2] == 1.0 && Gap(cdl, Resolved(pivoted)) < 1e-4,
              "a contrast folds about its own pivot: " + error);
        ColorCdl whole;
        Check(FoldColorCdl(
                  Resolved({Stage("color.hue", {{"degrees", {360, 1}}})}),
                  whole, error) &&
                  whole.slope[0] == 1.0,
              "a whole turn of hue resolves to nothing, and folds: " + error);
    }
    {
        ColorCdl neutral;
        Check(
            FoldColorCdl(
                Resolved({Stage("color.temperature", {{"kelvin", {6500, 1}}})}),
                neutral, error) &&
                neutral.slope[0] == 1.0 && neutral.saturation == 1.0,
            "a neutral white balance resolves to nothing, and folds: " + error);
    }

    // ---- in place of the clip's primary ---------------------------------
    {
        const std::vector<ClipEffect> clip = {
            Stage("color.temperature", {{"kelvin", {5200, 1}}}),
            Stage("color.exposure", {{"amount", {1, 10}}}),
            Stage("color.curves", {}), Stage("color.wheels", {}),
            Stage("color.saturation", {{"amount", {1, 10}}})};
        std::vector<ClipEffect> stages;
        double deviation = 0.0;
        ColorCdlStages(cdls[0], stages, deviation, error);
        std::vector<std::string> replaced;
        const std::vector<ClipEffect> placed =
            PlaceColorCdl(clip, stages, &replaced);
        Check(placed.size() == 4 && placed[0].type == "color.temperature" &&
                  placed[1].type == "color.wheels" &&
                  placed[2].type == "color.saturation" &&
                  placed[3].type == "color.curves" &&
                  placed[1].id == clip[3].id && placed[2].id == clip[4].id &&
                  replaced.size() == 3,
              "the CDL replaces the primary where it began, and keeps the "
              "clip's wheel and saturation ids");
        Check(PlaceColorCdl({clip[0]}, stages, nullptr).size() == 3,
              "a clip without a primary takes the CDL after its stack");
    }

    // ---- decimals -------------------------------------------------------
    EffectParamValue fraction;
    Check(DecimalFraction(1.05, fraction) && fraction.num == 21 &&
              fraction.den == 20,
          "1.05 is 21/20");
    Check(DecimalFraction(-0.0001, fraction) && fraction.num == -1 &&
              fraction.den == 10000,
          "-0.0001 is -1/10000");
    Check(DecimalFraction(3.0, fraction) && fraction.num == 3 &&
              fraction.den == 1,
          "3 is 3/1");
    Check(!DecimalFraction(1e-12, fraction), "past nine decimals, refused");
    Check(!DecimalFraction(1.0 / 3.0, fraction), "a third is no decimal");

    // ---- files, through OpenColorIO -------------------------------------
    const auto directory = std::filesystem::temp_directory_path() /
                           ("cutmachine-cdl-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    std::vector<ColorCdl> written = {cdls[0], cdls[2]};
    written[0].description = "C0001.MP4";
    const std::string collection = (directory / "look.ccc").string();
    Check(WriteColorCdlFile(collection, written, "ACEScct", error),
          "a .ccc is written: " + error);
    std::vector<ColorCdl> read;
    std::string input;
    Check(ReadColorCdlFile(collection, read, input, error) &&
              read.size() == 2 && read[0].id == "a" && read[1].id == "c" &&
              read[0].description == "C0001.MP4" && input == "ACEScct" &&
              read[0].slope == written[0].slope &&
              read[0].offset == written[0].offset &&
              read[1].power == written[1].power &&
              read[0].saturation == written[0].saturation,
          "it reads back as written, ids, description and space included: " +
              error + " / " + input);
    const std::string single = (directory / "one.CC").string();
    Check(WriteColorCdlFile(single, {cdls[1]}, "Display", error) &&
              ReadColorCdlFile(single, read, input, error) &&
              read.size() == 1 && read[0].saturation == 0.7 &&
              input == "Display",
          "a .cc holds one, and says its space: " + error);
    Check(!WriteColorCdlFile(single, written, "", error),
          "a .cc of two is refused");
    Check(!WriteColorCdlFile((directory / "look.cube").string(), written, "",
                             error),
          "a .cube is not a CDL");
    // A decision list as another application writes it.
    const auto list = directory / "shot.cdl";
    std::ofstream(list) << R"(<?xml version="1.0" encoding="UTF-8"?>
<ColorDecisionList xmlns="urn:ASC:CDL:v1.01">
  <ColorDecision>
    <ColorCorrection id="A001C003">
      <SOPNode>
        <Slope>1.0200 0.9900 0.9700</Slope>
        <Offset>0.0100 0.0000 -0.0150</Offset>
        <Power>1.0000 1.0200 1.0000</Power>
      </SOPNode>
      <SatNode>
        <Saturation>0.9500</Saturation>
      </SatNode>
    </ColorCorrection>
  </ColorDecision>
</ColorDecisionList>
)";
    Check(ReadColorCdlFile(list.string(), read, input, error) &&
              read.size() == 1 && read[0].id == "A001C003" &&
              read[0].offset[2] == -0.015 && read[0].saturation == 0.95 &&
              input.empty(),
          "a hand-written .cdl reads: " + error);
    Check(!ReadColorCdlFile((directory / "missing.ccc").string(), read, input,
                            error),
          "a missing file is refused");
    std::filesystem::remove_all(directory);

    // ---- Resolve's SetCDL ----------------------------------------------
    const ResolveCdlMap map = ResolveSetCdlMap(cdls[0], 1);
    Check(map.node_index == "1" && map.slope == "1.05 0.98 0.9" &&
              map.offset == "-0.02 0.01 0.03" && map.power == "1.1 1 0.92" &&
              map.saturation == "1.15",
          "SetCDL's strings are the shortest decimals: " + map.slope + " / " +
              map.power);

    ColorGradeSpace space;
    Check(ParseColorCdlSpace("acescct", space) &&
              space == ColorGradeSpace::AcesCct &&
              ParseColorCdlSpace("linear_rec2020", space) &&
              space == ColorGradeSpace::LinearRec2020 &&
              !ParseColorCdlSpace("ACEScct", space) &&
              std::string(ColorCdlSpaceName(ColorGradeSpace::AcesCct)) ==
                  "ACEScct",
          "a space is declared by its id, and named for a person");

    // ---- GR12, DEC30: the saturation DaVinci YRGB weighs otherwise -----
    // The gaps GR12 measured in Resolve 21.1: 1.8 codes for 1.1, 9 for 0.5
    // or 1.5, 18 for 0; none for 1.
    Check(ResolveYrgbSaturationGapCodes(1.0) == 0.0 &&
              ResolveYrgbSaturationGapCodes(1.1) == 1.8 &&
              ResolveYrgbSaturationGapCodes(0.5) == 9.1 &&
              ResolveYrgbSaturationGapCodes(1.5) == 9.1 &&
              ResolveYrgbSaturationGapCodes(0.0) == 18.2,
          "GR12: the YRGB gap is the one measured in Resolve: " +
              std::to_string(ResolveYrgbSaturationGapCodes(1.1)) + ", " +
              std::to_string(ResolveYrgbSaturationGapCodes(0.0)));
    {
        ColorCdl neutral;
        neutral.id = "01K30000000000000000000101";
        ColorCdl saturated = cdls[0];
        saturated.id = "01K30000000000000000000102";
        const std::string plain =
            ExportedCdlsValue({neutral}, ColorGradeSpace::Display, 1, "")
                .Dump();
        const std::string warned =
            ExportedCdlsValue({neutral, saturated}, ColorGradeSpace::Display, 1,
                              "")
                .Dump();
        Check(plain.find("warning") == std::string::npos &&
                  plain.find("resolve_yrgb_gap_codes") == std::string::npos,
              "GR12: a saturation of 1 carries no warning: " + plain);
        Check(warned.find("\"warning\":\"the clips in warning_clip_ids") !=
                      std::string::npos &&
                  warned.find("\"warning_clip_ids\":[\"01K30000000000000000000"
                              "102\"]") != std::string::npos &&
                  warned.find("\"resolve_yrgb_gap_codes\":2.7") !=
                      std::string::npos &&
                  warned.find("up to 2.7 10-bit codes") != std::string::npos,
              "GR12: a saturation of 1.15 is named, with its gap: " + warned);
    }

    if (failures == 0) std::cout << "color_cdl: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
