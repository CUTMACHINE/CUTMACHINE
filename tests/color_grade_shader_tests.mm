// F1.3 -- ROADMAP.md, wheels and curves (18 September 2026). The grade has
// one definition, ColorEffects.cc's ApplyResolvedColorGrade: the export
// samples it into its cubes, managed or not. The monitor cannot call it --
// shader.metal carries its own copy of every stage, and of the
// GradeEntry/ClipGradeParameters layout the renderer fills. Until this test,
// nothing compared the two: a wrong sign or a shifted field would only have
// shown on screen, and nothing of the Phase 2 has been looked at there.
//
// This compiles the real shader.metal with the options Renderer.mm uses,
// appends a compute kernel that calls its applyColorGrade, feeds it what
// PackColorGradeForGpu packs (exactly what Renderer.mm sends), and compares
// every stage -- the eight scalar ones, the wheels and the curves, alone and
// stacked -- with the C++ on a grid of values that spans ACEScct code values
// below 0 and above 1. It needs a Metal device, not a window; without one it
// reports itself skipped.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ColorEffects.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
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

constexpr int kSkipped = 77;

// Appended to shader.metal: the source has no kernel of its own for this.
constexpr const char* kTestKernel = R"METAL(
kernel void cutmachine_test_apply_color_grade(
    device const float4* input [[buffer(0)]],
    device float4* output [[buffer(1)]],
    constant ClipGradeParameters& grade [[buffer(2)]],
    constant uint& count [[buffer(3)]],
    uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = float4(applyColorGrade(input[index].rgb, grade), 1.0);
}
)METAL";

ClipEffect Effect(const std::string& type,
                  std::map<std::string, EffectParamValue> params) {
    return {GenerateUlid(), type, std::move(params)};
}

// The curve every curve case below starts from: an S on the master, a lift
// in the red toe, a blue shoulder, and a green that goes down then up.
std::map<std::string, EffectParamValue> CurvePoints() {
    return {
        {"master_x0", {0, 1}}, {"master_y0", {0, 1}}, {"master_x1", {1, 4}},
        {"master_y1", {1, 5}}, {"master_x2", {1, 2}}, {"master_y2", {1, 2}},
        {"master_x3", {3, 4}}, {"master_y3", {4, 5}}, {"master_x4", {1, 1}},
        {"master_y4", {1, 1}}, {"r_x0", {1, 10}},     {"r_y0", {3, 20}},
        {"r_x1", {1, 1}},      {"r_y1", {1, 1}},      {"g_x0", {0, 1}},
        {"g_y0", {1, 10}},     {"g_x1", {2, 5}},      {"g_y1", {1, 20}},
        {"g_x2", {7, 10}},     {"g_y2", {3, 4}},      {"g_x3", {1, 1}},
        {"g_y3", {9, 10}},     {"b_x0", {0, 1}},      {"b_y0", {0, 1}},
        {"b_x1", {3, 5}},      {"b_y1", {7, 10}},     {"b_x2", {17, 20}},
        {"b_y2", {7, 10}},     {"b_x3", {1, 1}},      {"b_y3", {19, 20}},
    };
}

// GR8 -- every hue curve at once: a hue turned, one drained, one lit, the
// shadows desaturated, the pale colours lifted, the vivid ones darkened.
std::map<std::string, EffectParamValue> HueCurves() {
    return {
        {"hue_hue_x0", {1, 20}},  {"hue_hue_y0", {1, 2}},
        {"hue_hue_x1", {3, 20}},  {"hue_hue_y1", {13, 20}},
        {"hue_hue_x2", {1, 4}},   {"hue_hue_y2", {1, 2}},
        {"hue_sat_x0", {2, 5}},   {"hue_sat_y0", {1, 2}},
        {"hue_sat_x1", {1, 2}},   {"hue_sat_y1", {1, 5}},
        {"hue_sat_x2", {3, 5}},   {"hue_sat_y2", {1, 2}},
        {"hue_lum_x0", {7, 10}},  {"hue_lum_y0", {1, 2}},
        {"hue_lum_x1", {4, 5}},   {"hue_lum_y1", {3, 5}},
        {"hue_lum_x2", {19, 20}}, {"hue_lum_y2", {9, 20}},
        {"lum_sat_x0", {1, 10}},  {"lum_sat_y0", {1, 5}},
        {"lum_sat_x1", {1, 2}},   {"lum_sat_y1", {1, 2}},
        {"lum_sat_x2", {9, 10}},  {"lum_sat_y2", {2, 5}},
        {"sat_sat_x0", {0, 1}},   {"sat_sat_y0", {7, 10}},
        {"sat_sat_x1", {1, 1}},   {"sat_sat_y1", {1, 2}},
        {"sat_lum_x0", {1, 5}},   {"sat_lum_y0", {1, 2}},
        {"sat_lum_x1", {1, 1}},   {"sat_lum_y1", {2, 5}},
    };
}

int Run() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::cout << "SKIP: no Metal device\n";
            return kSkipped;
        }
        NSError* error = nil;
        NSString* shader =
            [NSString stringWithContentsOfFile:@CUTMACHINE_SHADER_SOURCE
                                      encoding:NSUTF8StringEncoding
                                         error:&error];
        Check(shader != nil, "shader.metal is readable");
        if (shader == nil) {
            return 1;
        }
        NSString* kernel = @(kTestKernel);
        NSString* source = [shader stringByAppendingString:kernel];
        // options:nil, as Renderer.mm compiles it: fast math included.
        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                      options:nil
                                                        error:&error];
        Check(library != nil,
              std::string("shader.metal compiles: ") +
                  (error != nil ? error.localizedDescription.UTF8String : ""));
        if (library == nil) {
            return 1;
        }
        Check([library newFunctionWithName:@"fragment_working"] != nil,
              "fragment_working is still in the library");
        id<MTLFunction> function =
            [library newFunctionWithName:@"cutmachine_test_apply_color_grade"];
        Check(function != nil, "the test kernel is in the library");
        if (function == nil) {
            return 1;
        }
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        Check(pipeline != nil, "the test kernel builds");
        if (pipeline == nil) {
            return 1;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];

        // 14 steps per channel from -0.2 to 1.3: ACEScct below black and
        // above its 1.0 code, display signals, and the curve ends.
        std::vector<std::array<float, 4>> colors;
        for (int red = 0; red < 14; ++red) {
            for (int green = 0; green < 14; ++green) {
                for (int blue = 0; blue < 14; ++blue) {
                    colors.push_back({-0.2f + (red * 1.5f / 13.0f),
                                      -0.2f + (green * 1.5f / 13.0f),
                                      -0.2f + (blue * 1.5f / 13.0f), 1.0f});
                }
            }
        }
        const auto count = static_cast<uint32_t>(colors.size());
        const NSUInteger bytes = colors.size() * sizeof(colors[0]);
        id<MTLBuffer> input =
            [device newBufferWithBytes:colors.data()
                                length:bytes
                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> output =
            [device newBufferWithLength:bytes
                                options:MTLResourceStorageModeShared];

        std::map<std::string, EffectParamValue> wheels = {
            {"lift_r", {1, 20}},       {"lift_b", {-1, 25}},
            {"lift_master", {1, 50}},  {"gamma_g", {1, 4}},
            {"gamma_master", {-1, 5}}, {"gain_r", {-1, 5}},
            {"gain_b", {3, 10}},       {"gain_master", {1, 8}},
        };
        // GR16 -- a node like the ones the author saves in Resolve: balls and
        // dials together, an offset, a saturation, Lum Mix at its default.
        std::map<std::string, EffectParamValue> resolveNode = {
            {"gain_r", {53, 50}},     {"gain_g", {51, 50}},
            {"gain_b", {97, 100}},    {"gain_master", {51, 50}},
            {"gamma_r", {1, 50}},     {"gamma_g", {1, 20}},
            {"gamma_b", {2, 25}},     {"gamma_master", {1, 20}},
            {"lift_r", {-3, 100}},    {"lift_g", {-1, 50}},
            {"lift_b", {-1, 100}},    {"lift_master", {-1, 50}},
            {"offset_r", {-1, 20}},   {"offset_b", {1, 25}},
            {"saturation", {11, 10}},
        };
        const std::vector<std::pair<std::string, std::vector<ClipEffect>>>
            cases = {
                {"exposure", {Effect("color.exposure", {{"amount", {3, 2}}})}},
                {"contrast", {Effect("color.contrast", {{"amount", {2, 5}}})}},
                {"saturation",
                 {Effect("color.saturation", {{"amount", {-3, 5}}})}},
                {"vibrance", {Effect("color.vibrance", {{"amount", {7, 10}}})}},
                {"temperature",
                 {Effect("color.temperature", {{"kelvin", {4300, 1}}})}},
                {"tint", {Effect("color.tint", {{"amount", {-2, 5}}})}},
                {"highlights",
                 {Effect("color.highlights", {{"amount", {1, 2}}})}},
                {"shadows", {Effect("color.shadows", {{"amount", {-2, 5}}})}},
                {"wheels", {Effect("color.wheels", wheels)}},
                // GR8 -- the hue curves: one alone, then all six, then in a
                // stack.
                {"hue against saturation",
                 {Effect("color.hue_curves", {{"hue_sat_x0", {1, 5}},
                                              {"hue_sat_y0", {1, 2}},
                                              {"hue_sat_x1", {3, 10}},
                                              {"hue_sat_y1", {1, 10}},
                                              {"hue_sat_x2", {2, 5}},
                                              {"hue_sat_y2", {1, 2}}})}},
                {"six hue curves", {Effect("color.hue_curves", HueCurves())}},
                {"hue curves after an exposure and a contrast",
                 {Effect("color.exposure", {{"amount", {1, 2}}}),
                  Effect("color.contrast", {{"amount", {1, 5}}}),
                  Effect("color.hue_curves", HueCurves())}},
                // GR6 -- a contrast about its own pivot, a hue rotation.
                {"contrast about a pivot",
                 {Effect("color.contrast",
                         {{"amount", {2, 5}}, {"pivot", {1, 4}}})}},
                {"hue rotation",
                 {Effect("color.hue", {{"degrees", {-675, 10}}})}},
                {"hue, then a contrast about a pivot, then saturation",
                 {Effect("color.hue", {{"degrees", {120, 1}}}),
                  Effect("color.contrast",
                         {{"amount", {-1, 4}}, {"pivot", {3, 5}}}),
                  Effect("color.saturation", {{"amount", {1, 5}}})}},
                // GR2 -- the offset wheel, alone and under the others:
                // the same nine values reach the shader.
                {"offset wheel",
                 {Effect("color.wheels", {{"offset_r", {3, 100}},
                                          {"offset_b", {-1, 20}},
                                          {"offset_master", {1, 50}}})}},
                {"wheels with an offset",
                 {Effect("color.wheels", {{"lift_r", {1, 20}},
                                          {"gain_b", {3, 10}},
                                          {"gamma_master", {-1, 5}},
                                          {"offset_g", {-1, 25}},
                                          {"offset_master", {1, 10}}})}},
                {"wheels at their bounds",
                 {Effect("color.wheels", {{"lift_master", {-1, 1}},
                                          {"gain_g", {1, 1}},
                                          {"gain_master", {1, 1}},
                                          {"gamma_b", {-1, 1}},
                                          {"gamma_master", {-1, 1}}})}},
                {"curves", {Effect("color.curves", CurvePoints())}},
                {"red curve alone",
                 {Effect("color.curves", {{"r_x0", {1, 5}},
                                          {"r_y0", {1, 10}},
                                          {"r_x1", {1, 2}},
                                          {"r_y1", {7, 10}},
                                          {"r_x2", {4, 5}},
                                          {"r_y2", {3, 4}}})}},
                {"resolve primaries",
                 {Effect("color.resolve_primaries", resolveNode)}},
                {"resolve primaries, lum mix half, then an exposure",
                 {Effect("color.resolve_primaries", {{"gamma_master", {1, 4}},
                                                     {"gain_r", {13, 10}},
                                                     {"lum_mix", {1, 2}}}),
                  Effect("color.exposure", {{"amount", {1, 2}}})}},
                {"full stack",
                 {Effect("color.exposure", {{"amount", {1, 3}}}),
                  Effect("color.wheels", wheels),
                  Effect("color.contrast", {{"amount", {1, 5}}}),
                  Effect("color.curves", CurvePoints()),
                  Effect("color.saturation", {{"amount", {1, 4}}}),
                  Effect("color.temperature", {{"kelvin", {5600, 1}}})}},
            };

        // G7 -- the white balance acts on linear light, so each grading
        // space takes it to light and back its own way: every one is run,
        // against as-shot balances on either side of the scene's.
        std::vector<
            std::tuple<std::string, std::vector<ClipEffect>, ColorGradeContext>>
            runs;
        for (const auto& [name, effects] : cases)
            runs.push_back({name, effects, {}});
        const std::vector<std::pair<std::string, ColorGradeContext>> spaces = {
            {"display, shot at 5600 K", {ColorGradeSpace::Display, 5600}},
            {"ACEScct, shot at 4500 K", {ColorGradeSpace::AcesCct, 4500}},
            {"linear BT.709, unknown balance",
             {ColorGradeSpace::LinearRec709, 0}},
            {"linear BT.2020, shot at 3200 K",
             {ColorGradeSpace::LinearRec2020, 3200}},
        };
        for (const auto& [space, context] : spaces) {
            runs.push_back(
                {"temperature 3000 K, " + space,
                 {Effect("color.temperature", {{"kelvin", {3000, 1}}})},
                 context});
            runs.push_back(
                {"temperature 9000 K, " + space,
                 {Effect("color.temperature", {{"kelvin", {9000, 1}}})},
                 context});
            // GR16 -- to DaVinci Intermediate and back, from every space.
            runs.push_back({"resolve primaries, " + space,
                            {Effect("color.resolve_primaries", resolveNode)},
                            context});
            // With a contrast: the S-curve on HLG codes (aware tools), on
            // the node's values (without), and a straight one.
            std::map<std::string, EffectParamValue> aware = resolveNode;
            aware["contrast"] = {6, 5};
            aware["pivot"] = {2, 5};
            aware["color_space_aware"] = {1, 1};
            runs.push_back({"resolve primaries, aware contrast, " + space,
                            {Effect("color.resolve_primaries", aware)},
                            context});
            std::map<std::string, EffectParamValue> plain = aware;
            plain["color_space_aware"] = {0, 1};
            plain["contrast"] = {3, 2};
            runs.push_back({"resolve primaries, contrast, " + space,
                            {Effect("color.resolve_primaries", plain)},
                            context});
            std::map<std::string, EffectParamValue> straight = plain;
            straight["contrast_s_curve"] = {0, 1};
            straight["contrast"] = {7, 10};
            runs.push_back({"resolve primaries, straight contrast, " + space,
                            {Effect("color.resolve_primaries", straight)},
                            context});
            // GR16 -- a temperature and a tint: their matrix ends the
            // block, alone and after a contrast.
            // GR16 -- a node graded in ACEScct: its saturation about
            // Rec.709's luma, three weights read from the block.
            std::map<std::string, EffectParamValue> aces = resolveNode;
            aces["acescct"] = {1, 1};
            aces["saturation"] = {3, 2};
            runs.push_back({"resolve primaries, ACEScct saturation, " + space,
                            {Effect("color.resolve_primaries", aces)},
                            context});
            // GR16 -- in ACEScct (14 November 2026): the node's own codes,
            // its gamma passing the negative values its lifts make, the
            // temperature and tint on AP0 light to the power 1/2.4, then a
            // contrast without the aware tools.
            std::map<std::string, EffectParamValue> acesFull = aces;
            acesFull["temperature"] = {-1500, 1};
            acesFull["tint"] = {30, 1};
            acesFull["contrast"] = {13, 10};
            acesFull["pivot"] = {3, 10};
            runs.push_back(
                {"resolve primaries, ACEScct contrast, temperature and tint, " +
                     space,
                 {Effect("color.resolve_primaries", acesFull)},
                 context});
            std::map<std::string, EffectParamValue> balanced = resolveNode;
            balanced["temperature"] = {-1500, 1};
            balanced["tint"] = {30, 1};
            runs.push_back({"resolve primaries, temperature and tint, " + space,
                            {Effect("color.resolve_primaries", balanced)},
                            context});
            std::map<std::string, EffectParamValue> both = aware;
            both["temperature"] = {2500, 1};
            both["tint"] = {-40, 1};
            runs.push_back(
                {"resolve primaries, aware contrast, temperature and tint, " +
                     space,
                 {Effect("color.resolve_primaries", both)},
                 context});
        }

        for (const auto& [name, effects, context] : runs) {
            const ResolvedColorGrade grade =
                ResolveColorGrade(effects, context);
            Check(grade.count == static_cast<int32_t>(effects.size()),
                  name + ": every stage resolves");
            const GpuClipGradeParameters packed = PackColorGradeForGpu(grade);

            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder =
                [commands computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:output offset:0 atIndex:1];
            // setBytes, as Renderer.mm's setFragmentBytes: the same 4 KiB
            // limit, the same bytes.
            [encoder setBytes:&packed length:sizeof(packed) atIndex:2];
            [encoder setBytes:&count length:sizeof(count) atIndex:3];
            const NSUInteger width = pipeline.threadExecutionWidth;
            [encoder dispatchThreadgroups:MTLSizeMake(
                                              (count + width - 1) / width, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            Check(commands.status == MTLCommandBufferStatusCompleted,
                  name + ": the GPU ran the grade");

            const auto* shown =
                static_cast<const std::array<float, 4>*>(output.contents);
            double worst = 0.0;
            bool finite = true;
            std::string where;
            for (size_t index = 0; index < colors.size(); ++index) {
                const std::array<float, 3> expected = ApplyResolvedColorGrade(
                    {colors[index][0], colors[index][1], colors[index][2]},
                    grade);
                for (size_t channel = 0; channel < 3; ++channel) {
                    const double actual = shown[index][channel];
                    finite = finite && std::isfinite(actual);
                    // Fast math: relative to the value once it passes 1.
                    const double distance =
                        std::abs(actual - expected[channel]) /
                        std::max(1.0, std::abs(double(expected[channel])));
                    if (distance > worst) {
                        worst = distance;
                        where = "(" + std::to_string(colors[index][0]) + ", " +
                                std::to_string(colors[index][1]) + ", " +
                                std::to_string(colors[index][2]) +
                                ") channel " + std::to_string(channel) +
                                ": GPU " + std::to_string(actual) + ", C++ " +
                                std::to_string(expected[channel]);
                    }
                }
            }
            Check(finite, name + ": the GPU grade stays finite");
            std::ostringstream agree;
            agree << name << ": shader.metal and ColorEffects.cc agree, worst "
                  << worst << " at " << where;
            // G7 -- the white balance in ACEScct decodes codes up to 1.3,
            // linear light near 8500, and its matrix subtracts channels
            // hundreds apart: the GPU's fast exp2 and log2 differ there by
            // about 1e-4 of the value. No camera signal reaches those codes;
            // every other stage, and this one in the other spaces, keeps the
            // 1e-5 bound. GR16 -- color.resolve_primaries goes the same way
            // to light, then to DaVinci Intermediate's log2 and back: under
            // ACEScct its worst is 1.4e-4, at a code of 0.95 (linear 120).
            // With a contrast it also goes through HLG's log and exp: 9.7e-4
            // of the value under ACEScct, at a code of 1.3 (linear 8500)
            // that comes out at -5.5, and 1.6e-5 in linear BT.709 -- 0.016
            // of a 10-bit code. A temperature and a tint (6 November 2026)
            // multiply those differences by their matrix: 5.2e-4 under
            // ACEScct, at a code of 1.18 that comes out at -3.7. A saturation
            // of 1.5 (the ACEScct run, 7 November 2026) stretches them the
            // same way: 5.7e-4 under ACEScct at a code of 1.07 that comes out
            // at -2.5, 2.0e-5 in linear BT.2020 at a light of 1.3. A node
            // graded in ACEScct (14 November 2026) holds ACEScct codes from
            // every space, and its temperature and tint go to light and back
            // through pow: 4.6e-4 of the value at most, in linear BT.709 at
            // a light of 1.18 that comes out at -3.6, and 2.9e-4 on the
            // display signal at a code of 1.3.
            const bool acesNode =
                name.find("ACEScct saturation") != std::string::npos ||
                name.find("ACEScct contrast") != std::string::npos;
            const bool stretched = acesNode;
            const bool balanced =
                name.find("temperature and tint") != std::string::npos ||
                stretched;
            const bool extremeLight =
                context.space == ColorGradeSpace::AcesCct &&
                (name.rfind("temperature", 0) == 0 ||
                 name.rfind("resolve primaries", 0) == 0);
            const bool contrasted =
                name.find("contrast,") != std::string::npos &&
                name.rfind("resolve primaries", 0) == 0;
            Check(worst < (acesNode                    ? 1e-3
                           : extremeLight              ? (contrasted ? 2e-3
                                                          : balanced ? 1e-3
                                                                     : 5e-4)
                           : (contrasted || stretched) ? 3e-5
                                                       : 1e-5),
                  agree.str());
            std::cout << "shader parity, " << name << ": worst " << worst
                      << '\n';
        }
    }
    if (failures == 0) {
        std::cout << "PASS: color grade shader parity\n";
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() {
    try {
        return Run();
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
    }
    return 1;
}
