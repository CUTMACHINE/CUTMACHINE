// Visual QA of 19 September 2026 (M4). Under colour management, a motion
// clip at 50 % opacity kept its white almost opaque on the monitor and in the
// export: both mixed it in the scene-linear working space, where display
// white sits near 16. The rule is now ColorManagement.h's
// OpenColorIoCompositeDisplayReferred -- mix in the SDR display view, come
// back through the inverse the motion frame already uses -- and the export
// graph follows it (motion_clip_export_tests). BH1 (BLANC-HLG-2026-10): for
// an HLG delivery that view is the HLG one, in graphic code values, so a
// white title reaches the HLG peak instead of the SDR view's white.
//
// The monitor cannot call the C++: shader.metal's compositeDisplayReferred
// carries its own copy, reading the two 65^3 LUTs Renderer.mm uploads. This
// compiles the real shader.metal, appends a kernel that calls the function
// with those LUTs, and compares it with the C++ on a grid of composites,
// layer colours and coverages, the way color_grade_shader_tests checks the
// grade. It needs a Metal device, not a window; without one it reports
// itself skipped.

#include "ColorEffects.h"
#include "ColorManagement.h"
#include "Document.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
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

// shader.metal's OutputParameters, as Renderer.mm fills it for the view.
struct ViewParameters {
    int32_t colorManagementEnabled;
    int32_t outputGamut;
    int32_t outputTransfer;
    int32_t scopeMode;
    uint32_t drawableWidth;
    uint32_t drawableHeight;
    uint32_t scopeSampleStep;
    uint32_t useAcescct;
    // R6 -- left at zero here: the view a motion layer is mixed in never
    // carries the sequence's look. The member is still spelled, because
    // this struct is handed to the shader whole.
    uint32_t lookLutEdge;
    // R6 -- zero likewise: the look's stage belongs to the delivery.
    uint32_t lookAfterDisplay;
    // PRD37 -- the scope panel's rectangle, zero: no scope in this view.
    float scopeLeft;
    float scopeTop;
    float scopeWidth;
    float scopeHeight;
};

// Appended to shader.metal: the source has no kernel of its own for this.
constexpr const char* kTestKernel = R"METAL(
kernel void cutmachine_test_composite_display_referred(
    device const float4* below [[buffer(0)]],
    device const float4* layer [[buffer(1)]],
    device float4* output [[buffer(2)]],
    constant OutputParameters& view [[buffer(3)]],
    constant uint& count [[buffer(4)]],
    texture3d<float> viewLut [[texture(0)]],
    texture3d<float> motionLut [[texture(1)]],
    sampler lutSampler [[sampler(0)]],
    uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    output[index] = float4(
        compositeDisplayReferred(below[index].rgb, layer[index].rgb,
                                 layer[index].a, view, viewLut, motionLut,
                                 lutSampler),
        1.0);
}
)METAL";

// Renderer.mm's UploadOpenColorIoLut, which is private to it.
id<MTLTexture> Upload(id<MTLDevice> device, const OpenColorIoLut3D& lut) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType3D;
    descriptor.pixelFormat = MTLPixelFormatRGBA32Float;
    descriptor.width = static_cast<NSUInteger>(lut.edge);
    descriptor.height = static_cast<NSUInteger>(lut.edge);
    descriptor.depth = static_cast<NSUInteger>(lut.edge);
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    const NSUInteger row = static_cast<NSUInteger>(lut.edge) * 4 * 4;
    [texture
        replaceRegion:MTLRegionMake3D(0, 0, 0, lut.edge, lut.edge, lut.edge)
          mipmapLevel:0
                slice:0
            withBytes:lut.rgba.data()
          bytesPerRow:row
        bytesPerImage:row * static_cast<NSUInteger>(lut.edge)];
    return texture;
}

double Distance(const RgbColor& a, const RgbColor& b) {
    return std::max({std::abs(a.red - b.red), std::abs(a.green - b.green),
                     std::abs(a.blue - b.blue)});
}

// The shader's LUT read (ocioLutCoordinates, then linear filtering of the
// 3D texture): clamped to the cube, texel centres, trilinear.
RgbColor SampleLut(const OpenColorIoLut3D& lut, const RgbColor& value) {
    const int edge = lut.edge;
    const auto texel = [edge](double v) {
        return std::clamp(v, 0.0, 1.0) * (edge - 1);
    };
    const double x = texel(value.red);
    const double y = texel(value.green);
    const double z = texel(value.blue);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = std::min(x0 + 1, edge - 1);
    const int y1 = std::min(y0 + 1, edge - 1);
    const int z1 = std::min(z0 + 1, edge - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    const double fz = z - z0;
    RgbColor result;
    double* const channels[3] = {&result.red, &result.green, &result.blue};
    for (int channel = 0; channel < 3; ++channel) {
        const auto at = [&](int r, int g, int b) {
            return static_cast<double>(
                lut.rgba[((((static_cast<size_t>(b) * edge) + g) * edge) + r) *
                             4 +
                         channel]);
        };
        const double c00 = at(x0, y0, z0) * (1 - fx) + at(x1, y0, z0) * fx;
        const double c10 = at(x0, y1, z0) * (1 - fx) + at(x1, y1, z0) * fx;
        const double c01 = at(x0, y0, z1) * (1 - fx) + at(x1, y0, z1) * fx;
        const double c11 = at(x0, y1, z1) * (1 - fx) + at(x1, y1, z1) * fx;
        *channels[channel] = ((c00 * (1 - fy) + c10 * fy) * (1 - fz)) +
                             ((c01 * (1 - fy) + c11 * fy) * fz);
    }
    return result;
}

// compositeDisplayReferred in C++, with the same two LUTs: what the shader
// must compute, up to float rounding and the GPU's filtering weights.
RgbColor EmulatedComposite(const OpenColorIoLut3D& view,
                           const OpenColorIoLut3D& motion, bool acescct,
                           const RgbColor& below, const RgbColor& layer,
                           double coverage) {
    const RgbColor coded =
        acescct ? RgbColor{EncodeAcesCct(below.red), EncodeAcesCct(below.green),
                           EncodeAcesCct(below.blue)}
                : below;
    // displaySignal bounds the view to what it shows.
    const RgbColor sampled = SampleLut(view, coded);
    const RgbColor shown{std::clamp(sampled.red, 0.0, 1.0),
                         std::clamp(sampled.green, 0.0, 1.0),
                         std::clamp(sampled.blue, 0.0, 1.0)};
    return SampleLut(motion,
                     {shown.red + ((layer.red - shown.red) * coverage),
                      shown.green + ((layer.green - shown.green) * coverage),
                      shown.blue + ((layer.blue - shown.blue) * coverage)});
}

ColorManagementSettings Settings(const std::string& outputTransfer) {
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    settings.working_gamut = "acescct";
    settings.output_gamut = outputTransfer == "hlg" ? "rec2020" : "rec709";
    settings.output_transfer = outputTransfer;
    return settings;
}

void CheckCase(id<MTLDevice> device, id<MTLComputePipelineState> pipeline,
               id<MTLCommandQueue> queue, id<MTLSamplerState> sampler,
               const std::string& label,
               const ColorManagementSettings& settings) {
    std::string error;
    const ColorManagementSettings sdr = ColorManagementForSdrPreview(settings);
    // Where a graphic is placed, and where the result is judged: the SDR
    // view, or the HLG delivery itself (BH1).
    const ColorManagementSettings seen =
        GraphicsPlacedInHlg(settings) ? settings : sdr;
    OpenColorIoLutPair view;
    OpenColorIoLut3D motion;
    if (!BuildOpenColorIoGraphicsViewLuts(settings, view, error) ||
        !BuildOpenColorIoDisplayReferredWorkingLut(settings, motion, error)) {
        Check(false, label + ": the LUTs build: " + error);
        return;
    }

    // Composites: camera signals of the project taken to the working space,
    // from crushed black to a bright highlight, neutral and mildly tinted.
    // A saturated highlight whose working value leaves the gamut (a channel
    // below zero) is left out: there the two 65^3 LUTs' interpolation, not
    // the rule, sets the error, and the export reads the same cubes. Layers:
    // the corners of the display cube and a few chart colours. Coverages: an
    // antialiased edge, the QA's 50 %, three quarters and opaque.
    std::vector<RgbColor> belows;
    for (const double level : {0.0, 0.08, 0.2, 0.41, 0.6, 0.8}) {
        for (const RgbColor tint : {RgbColor{1, 1, 1}, RgbColor{1, 0.85, 0.7},
                                    RgbColor{0.7, 0.85, 1}}) {
            RgbColor working;
            if (!OpenColorIoWorkingColor(
                    settings, ResolvedColorGrade{},
                    {level * tint.red, level * tint.green, level * tint.blue},
                    working, error)) {
                Check(false, label + ": a camera signal converts: " + error);
                return;
            }
            belows.push_back(working);
        }
    }
    const std::vector<RgbColor> layers = {
        {1, 1, 1},     {0, 0, 0},       {1, 0, 0},
        {0, 1, 0},     {0, 0, 1},       {0.9, 0, 0.494},
        {1, 0.824, 0}, {0.5, 0.5, 0.5}, {0.2, 0.4, 0.6}};
    const std::vector<float> coverages = {0.1f, 0.5f, 0.75f, 1.0f};
    std::vector<std::array<float, 4>> belowData;
    std::vector<std::array<float, 4>> layerData;
    for (const RgbColor& below : belows) {
        for (const RgbColor& layer : layers) {
            for (const float coverage : coverages) {
                belowData.push_back({static_cast<float>(below.red),
                                     static_cast<float>(below.green),
                                     static_cast<float>(below.blue), 1.0f});
                layerData.push_back({static_cast<float>(layer.red),
                                     static_cast<float>(layer.green),
                                     static_cast<float>(layer.blue), coverage});
            }
        }
    }
    const auto count = static_cast<uint32_t>(belowData.size());
    const NSUInteger bytes = belowData.size() * sizeof(belowData[0]);
    id<MTLBuffer> belowBuffer =
        [device newBufferWithBytes:belowData.data()
                            length:bytes
                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> layerBuffer =
        [device newBufferWithBytes:layerData.data()
                            length:bytes
                           options:MTLResourceStorageModeShared];
    id<MTLBuffer> output =
        [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    ViewParameters parameters = {};
    parameters.colorManagementEnabled = 1;
    parameters.useAcescct = sdr.working_gamut == "acescct" ? 1u : 0u;

    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:belowBuffer offset:0 atIndex:0];
    [encoder setBuffer:layerBuffer offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3];
    [encoder setBytes:&count length:sizeof(count) atIndex:4];
    [encoder setTexture:Upload(device, view.working_to_display) atIndex:0];
    [encoder setTexture:Upload(device, motion) atIndex:1];
    [encoder setSamplerState:sampler atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(
                                  std::min<NSUInteger>(
                                      64,
                                      pipeline.maxTotalThreadsPerThreadgroup),
                                  1, 1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];

    // Compared where the result is seen: through the view the graphic is
    // placed in. A working value near white is a large number, whose LUT
    // error means nothing.
    const auto* results =
        static_cast<const std::array<float, 4>*>(output.contents);
    double worst = 0.0;
    double worstEmulated = 0.0;
    double worstHalfWhiteOverBlack = 0.0;
    std::string worstCase;
    const bool acescct = parameters.useAcescct != 0;
    for (uint32_t index = 0; index < count; ++index) {
        const RgbColor below{belowData[index][0], belowData[index][1],
                             belowData[index][2]};
        const RgbColor layer{layerData[index][0], layerData[index][1],
                             layerData[index][2]};
        const double coverage = layerData[index][3];
        RgbColor expected;
        RgbColor expectedShown;
        RgbColor emulatedShown;
        RgbColor gpuShown;
        if (!OpenColorIoCompositeDisplayReferred(settings, below, layer,
                                                 coverage, expected, error) ||
            !OpenColorIoDisplayColor(seen, expected, expectedShown, error) ||
            !OpenColorIoDisplayColor(
                seen,
                EmulatedComposite(view.working_to_display, motion, acescct,
                                  below, layer, coverage),
                emulatedShown, error) ||
            !OpenColorIoDisplayColor(
                seen, {results[index][0], results[index][1], results[index][2]},
                gpuShown, error)) {
            Check(false, label + ": the reference computes: " + error);
            return;
        }
        worstEmulated =
            std::max(worstEmulated, Distance(gpuShown, emulatedShown));
        const double distance = Distance(gpuShown, expectedShown);
        if (distance > worst) {
            worst = distance;
            worstCase = "below (" + std::to_string(below.red) + ", " +
                        std::to_string(below.green) + ", " +
                        std::to_string(below.blue) + "), layer (" +
                        std::to_string(layer.red) + ", " +
                        std::to_string(layer.green) + ", " +
                        std::to_string(layer.blue) + ") at " +
                        std::to_string(coverage);
        }
        if (index < layers.size() * coverages.size() && layer.red == 1.0 &&
            layer.green == 1.0 && coverage == 0.5f) {
            // Over crushed black, a white layer at 50 % is mid-grey in the
            // view, not white: the defect the QA saw.
            worstHalfWhiteOverBlack = std::max({std::abs(gpuShown.red - 0.5),
                                                std::abs(gpuShown.green - 0.5),
                                                std::abs(gpuShown.blue - 0.5)});
        }
    }
    // The logic: the shader computes what the same LUTs give in C++.
    Check(worstEmulated <= 2e-3,
          label +
              ": the shader mixes in the view as the C++ does with the "
              "same LUTs, worst " +
              std::to_string(worstEmulated));
    // The precision: against the exact OpenColorIO rule, the two 65^3 LUTs
    // cost at most this near the faces of the cube (a pure primary at 10 %
    // over a dark blue), well under a code value elsewhere.
    Check(worst <= 0.03,
          label + ": the monitor's mix stays near the exact rule, worst " +
              std::to_string(worst) + " for " + worstCase);
    Check(worstHalfWhiteOverBlack <= 0.02,
          label + ": white at 50 % over black shows as mid-grey, off by " +
              std::to_string(worstHalfWhiteOverBlack));
    std::cout << label << ": " << count << " composites, worst "
              << worstEmulated << " against the same LUTs, " << worst
              << " against the exact rule\n";
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
        if (shader == nil) return 1;
        NSString* source = [shader stringByAppendingString:@(kTestKernel)];
        id<MTLLibrary> library = [device newLibraryWithSource:source
                                                      options:nil
                                                        error:&error];
        Check(library != nil,
              std::string("shader.metal compiles: ") +
                  (error != nil ? error.localizedDescription.UTF8String : ""));
        if (library == nil) return 1;
        Check([library
                  newFunctionWithName:@"fragment_working_display_referred"] !=
                  nil,
              "the monitor's display-referred fragment is in the library");
        id<MTLFunction> function = [library
            newFunctionWithName:@"cutmachine_test_composite_display_referred"];
        id<MTLComputePipelineState> pipeline =
            function == nil
                ? nil
                : [device newComputePipelineStateWithFunction:function
                                                        error:&error];
        Check(pipeline != nil, "the test kernel builds");
        if (pipeline == nil) return 1;
        // Renderer.mm's sampler: linear filtering, clamped to the edge.
        MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDescriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> sampler =
            [device newSamplerStateWithDescriptor:samplerDescriptor];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        CheckCase(device, pipeline, queue, sampler, "S-Log3 to Rec.709",
                  Settings("rec709"));
        // An HLG delivery: a graphic is placed in the HLG view itself, its
        // white at the peak (BH1), and mixed there.
        CheckCase(device, pipeline, queue, sampler, "S-Log3 to HLG",
                  Settings("hlg"));
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() { return Run(); }
