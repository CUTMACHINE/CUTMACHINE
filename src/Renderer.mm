#include "Renderer.h"

#include "CaptionOverlayLayout.h"

#include "ColorManagement.h"
#include "MetalUiAtlas.h"

#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

struct Renderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    id<MTLRenderPipelineState> workingPipeline = nil;
    id<MTLRenderPipelineState> outputPipeline = nil;
    id<MTLComputePipelineState> scopePipeline = nil;
    id<MTLComputePipelineState> curveHistogramPipeline = nil;
    id<MTLRenderPipelineState> solidPipeline = nil;
    id<MTLRenderPipelineState> textPipeline = nil;
    id<MTLSamplerState> sampler = nil;
    MetalUiAtlas uiAtlas;
    id<MTLTexture> workingTexture = nil;
    id<MTLTexture> ocioInputLut = nil;
    id<MTLTexture> ocioOutputLut = nil;
    std::string ocioLutKey;
    // G3 -- the input LUTs of media that declare their own, by
    // OpenColorIoCacheKey: built on first sight and kept, a few at most.
    std::map<std::string, id<MTLTexture>> mediaInputLuts;
    // G6 -- the reference still's texture, and the still it was made from,
    // held so its address is not reused while the texture stands for it.
    id<MTLTexture> referenceTexture = nil;
    std::shared_ptr<const ReferenceStill> referenceSource;
    // G5 -- shader.metal's ScopeGuideParameters, float for float, for the
    // presentation they were built for.
    struct ScopeGuides {
        float middleGrey = 0.0f;
        float greyLow = 0.0f;
        float greyHigh = 0.0f;
        float skinLow = 0.0f;
        float skinHigh = 0.0f;
        float nearBlack = 0.0f;
        float nearWhite = 0.0f;
        float endCode = 0.0f;
        float skinCos = 0.0f;
        float skinSin = 0.0f;
        float targets[12] = {};
        float falseColors[18] = {};
    } scopeGuides;
    std::string scopeGuideKey;
    // MOTION-2026-09 M4 -- display-referred sRGB to the working space, for
    // the motion frames (ColorManagement.h); built on first use.
    id<MTLTexture> ocioMotionLut = nil;
    std::string ocioMotionLutKey;
    // Visual QA of 19 September 2026 -- the view ocioMotionLut inverts
    // (the SDR one, or the HLG one for an HLG presentation, BH1), forward,
    // and the pipeline that mixes a motion frame in it
    // (compositeDisplayReferred in shader.metal).
    id<MTLTexture> ocioMotionViewLut = nil;
    // R6 -- docs/roadmap/REEL-2026-09.md. The sequence's look, uploaded
    // when its digest changes. `lookLutFallback` is a 2^3 texture bound
    // whenever there is no look: the fragment shader never samples it
    // (lookLutEdge is then zero), but a texture argument has to be bound.
    id<MTLTexture> lookLut = nil;
    std::string lookLutKey;
    id<MTLTexture> lookLutFallback = nil;
    id<MTLRenderPipelineState> displayReferredPipeline = nil;
    // R5 -- docs/roadmap/REEL-2026-09.md. A colour dip under colour
    // management, mixed in the same SDR view as a motion frame.
    id<MTLRenderPipelineState> solidDisplayReferredPipeline = nil;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred layer: drawn alone
    // into `blurSource` (decodeLayerPipeline), blurred into `blurResult` by
    // `blurKernel`, then laid by blurredWorkingPipeline, or mixed in the
    // graphics view by blurredDisplayReferredPipeline. The textures are the
    // size of the last blurred rectangle, the kernel of its sigma, both
    // made again only when that changes. `blurSupported` is false on a GPU
    // Metal Performance Shaders does not run on, where a blurred layer is
    // drawn sharp and said so once.
    id<MTLRenderPipelineState> decodeLayerPipeline = nil;
    id<MTLRenderPipelineState> blurredWorkingPipeline = nil;
    id<MTLRenderPipelineState> blurredDisplayReferredPipeline = nil;
    id<MTLTexture> blurSource = nil;
    id<MTLTexture> blurResult = nil;
    MPSImageGaussianBlur* blurKernel = nil;
    float blurKernelSigma = -1.0f;
    bool blurSupported = false;
    bool blurUnsupportedReported = false;
    id<MTLBuffer> scopeHistogram = nil;
    size_t scopeHistogramBins = 0;
    id<MTLBuffer> curveHistogramBuffer = nil;
    VideoHistogram curveHistogram{};
    std::vector<std::array<id<MTLTexture>, 4>> planes;
    std::vector<std::array<int, 4>> textureWidths;
    std::vector<std::array<int, 4>> textureHeights;
    std::vector<int> textureFormats;
    CGColorSpaceRef sdrColorSpace = nullptr;
    CGColorSpaceRef hlgColorSpace = nullptr;
    // VQA5 -- CaptureNextPresent.
    bool captureArmed = false;
    CGImageRef presentCapture = nullptr;

    ~Impl() {
        if (sdrColorSpace) CGColorSpaceRelease(sdrColorSpace);
        if (hlgColorSpace) CGColorSpaceRelease(hlgColorSpace);
        if (presentCapture) CGImageRelease(presentCapture);
    }
};

namespace {

// XR keeps ten useful bits for HLG while remaining usable for SDR projects.
// The layer and RenderFramesToPixels' target share it, and with it the
// output pipeline.
constexpr MTLPixelFormat kOutputPixelFormat = MTLPixelFormatBGRA10_XR;

// R6 -- one upload for every 3D LUT the renderer holds: the OpenColorIO
// pair, the motion transforms and the sequence's imported look, which is
// the only one whose edge is not 65.
id<MTLTexture> UploadLut3D(id<MTLDevice> device, int32_t edge,
                           const std::vector<float>& rgba) {
    if (!device || edge <= 1 ||
        rgba.size() != static_cast<size_t>(edge) * edge * edge * 4)
        return nil;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType3D;
    descriptor.pixelFormat = MTLPixelFormatRGBA32Float;
    descriptor.width = static_cast<NSUInteger>(edge);
    descriptor.height = static_cast<NSUInteger>(edge);
    descriptor.depth = static_cast<NSUInteger>(edge);
    descriptor.mipmapLevelCount = 1;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) return nil;
    const NSUInteger bytesPerRow =
        static_cast<NSUInteger>(edge) * 4 * sizeof(float);
    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, edge, edge, edge)
               mipmapLevel:0
                     slice:0
                 withBytes:rgba.data()
               bytesPerRow:bytesPerRow
             bytesPerImage:bytesPerRow * static_cast<NSUInteger>(edge)];
    return texture;
}

id<MTLTexture> UploadOpenColorIoLut(id<MTLDevice> device,
                                    const OpenColorIoLut3D& lut) {
    return UploadLut3D(device, lut.edge, lut.rgba);
}

}  // namespace

Renderer::Renderer() : impl_(new Impl()) {}
Renderer::~Renderer() { delete impl_; }

bool Renderer::Initialize(NSView* view) {
    impl_->device = MTLCreateSystemDefaultDevice();
    impl_->queue = [impl_->device newCommandQueue];
    if (!impl_->device || !impl_->queue) {
        std::fprintf(stderr, "Unable to initialize Metal device/queue\n");
        return false;
    }

    // VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. BT.709 primaries and
    // white, decoded by a pure 2.4 gamma: the reference monitor's picture,
    // and the one ColorSync shows for a Rec.709-A file (colr 1-2-1, gama
    // 2.4; Export.h). The named kCGColorSpaceITUR_709 decodes with the
    // inverse camera curve instead, about 1.96: Record showed the midtones
    // 12 to 19 codes lighter than a player reading the same values at 2.4
    // or as sRGB, exactly as QuickTime does with a 1-1-1 file.
    {
        const CGFloat white[3] = {0.95047, 1.0, 1.08883};
        const CGFloat black[3] = {0.0, 0.0, 0.0};
        const CGFloat gamma[3] = {2.4, 2.4, 2.4};
        // BT.709 RGB to XYZ, columns R, G, B.
        const CGFloat toXyz[9] = {0.4124, 0.2126, 0.0193, 0.3576, 0.7152,
                                  0.1192, 0.1805, 0.0722, 0.9505};
        impl_->sdrColorSpace =
            CGColorSpaceCreateCalibratedRGB(white, black, gamma, toXyz);
    }
    impl_->hlgColorSpace =
        CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_HLG);
    if (view) {
        impl_->layer = [CAMetalLayer layer];
        impl_->layer.device = impl_->device;
        impl_->layer.pixelFormat = kOutputPixelFormat;
        impl_->layer.framebufferOnly = YES;
        // While a splitter is being dragged, keep the last drawable's aspect
        // instead of letting Core Animation stretch it to intermediate
        // bounds.
        impl_->layer.contentsGravity = kCAGravityResizeAspect;
        impl_->layer.colorspace = impl_->sdrColorSpace;
        impl_->layer.contentsScale =
            view.window.backingScaleFactor
                ?: NSScreen.mainScreen.backingScaleFactor;
        [view setWantsLayer:YES];
        view.layer = impl_->layer;
        Resize(view.bounds);
    }

    NSError* error = nil;
    NSString* shaderPath =
        [NSString stringWithUTF8String:CUTMACHINE_SHADER_PATH];
    if (![[NSFileManager defaultManager] fileExistsAtPath:shaderPath]) {
        NSString* bundled = [[NSBundle mainBundle] pathForResource:@"shader"
                                                            ofType:@"metal"];
        if (bundled.length > 0) shaderPath = bundled;
    }
    NSString* shaderSource =
        [NSString stringWithContentsOfFile:shaderPath
                                  encoding:NSUTF8StringEncoding
                                     error:&error];
    if (!shaderSource) {
        std::fprintf(stderr, "Unable to read %s: %s\n", shaderPath.UTF8String,
                     error.localizedDescription.UTF8String);
        return false;
    }
    id<MTLLibrary> library = [impl_->device newLibraryWithSource:shaderSource
                                                         options:nil
                                                           error:&error];
    if (!library) {
        std::fprintf(stderr, "Unable to compile %s: %s\n",
                     shaderPath.UTF8String,
                     error.localizedDescription.UTF8String);
        return false;
    }

    id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> fragment =
        [library newFunctionWithName:@"fragment_working"];
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    impl_->workingPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->workingPipeline) {
        std::fprintf(stderr, "Unable to create Metal working pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }
    // Reads the composite through framebuffer fetch and writes the mix
    // itself: no fixed-function blending.
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_working_display_referred"];
    descriptor.colorAttachments[0].blendingEnabled = NO;
    impl_->displayReferredPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->displayReferredPipeline) {
        std::fprintf(stderr,
                     "Unable to create Metal display-referred pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred layer's passes:
    // drawn alone into a texture of its own, unblended; laid in the
    // graphics view, unblended, as the display-referred pipeline above; laid
    // over the composite, blended, as the working pipeline.
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_blurred_display_referred"];
    impl_->blurredDisplayReferredPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_decode_layer"];
    impl_->decodeLayerPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_working_blurred"];
    descriptor.colorAttachments[0].blendingEnabled = YES;
    impl_->blurredWorkingPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->blurredDisplayReferredPipeline || !impl_->decodeLayerPipeline ||
        !impl_->blurredWorkingPipeline) {
        std::fprintf(stderr,
                     "Unable to create the blurred layer pipelines: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }
    impl_->blurSupported = MPSSupportsMTLDevice(impl_->device);

    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_output"];
    descriptor.colorAttachments[0].pixelFormat = kOutputPixelFormat;
    descriptor.colorAttachments[0].blendingEnabled = NO;
    impl_->outputPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->outputPipeline) {
        std::fprintf(stderr, "Unable to create Metal output pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    id<MTLFunction> scopeFunction =
        [library newFunctionWithName:@"compute_video_scope"];
    impl_->scopePipeline =
        [impl_->device newComputePipelineStateWithFunction:scopeFunction
                                                     error:&error];
    if (!impl_->scopePipeline) {
        std::fprintf(stderr, "Unable to create video scope pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    id<MTLFunction> histogramFunction =
        [library newFunctionWithName:@"compute_curve_histogram"];
    impl_->curveHistogramPipeline =
        [impl_->device newComputePipelineStateWithFunction:histogramFunction
                                                     error:&error];
    if (impl_->curveHistogramPipeline == nil) {
        std::fprintf(stderr, "Unable to create curve histogram pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_solid"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_solid"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor =
        MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    impl_->solidPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->solidPipeline) {
        std::fprintf(stderr, "Unable to create solid pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }
    // R5 -- reads the composite through framebuffer fetch and writes the
    // mix itself, as the display-referred pipeline above does.
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_solid_display_referred"];
    descriptor.colorAttachments[0].blendingEnabled = NO;
    impl_->solidDisplayReferredPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->solidDisplayReferredPipeline) {
        std::fprintf(stderr, "Unable to create the colour dip pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_solid"];
    descriptor.colorAttachments[0].blendingEnabled = YES;

    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_text"];
    descriptor.fragmentFunction =
        [library newFunctionWithName:@"fragment_text"];
    impl_->textPipeline =
        [impl_->device newRenderPipelineStateWithDescriptor:descriptor
                                                      error:&error];
    if (!impl_->textPipeline) {
        std::fprintf(stderr, "Unable to create text pipeline: %s\n",
                     error.localizedDescription.UTF8String);
        return false;
    }

    MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->sampler =
        [impl_->device newSamplerStateWithDescriptor:samplerDescriptor];
    return impl_->sampler != nil && impl_->uiAtlas.Initialize(impl_->device);
}

void Renderer::Resize(NSRect bounds) {
    if (!impl_->layer) {
        return;
    }
    // Only the drawable is ours. This CAMetalLayer is the view's *backing*
    // layer (Initialize sets view.layer), so AppKit owns its frame and
    // expresses it in the superview's coordinates -- while every caller here
    // passes view.bounds, whose origin is always (0,0). Assigning that as the
    // layer frame teleported the surface to the top-left of its enclosing
    // view: harmless for the monitors, which do sit at their panel's origin,
    // but it lifted the timeline out of the bottom pane and drew it behind
    // the monitors, visible through the gap between them.
    const CGFloat scale = impl_->layer.contentsScale;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    impl_->layer.drawableSize =
        CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
    [CATransaction commit];
}

bool Renderer::RenderFrames(const std::vector<AVFrame*>& frames,
                            const TimelineRenderData& timeline) {
    if (!impl_->layer) return false;
    if (!Render(frames, timeline, 0, 0, nullptr)) return false;
    if (impl_->captureArmed) {
        impl_->captureArmed = false;
        // The drawable's size, so that the scale Render reads from the
        // layer lays out the pixels it has just presented.
        const CGSize drawable = impl_->layer.drawableSize;
        const auto width = static_cast<uint32_t>(drawable.width);
        const auto height = static_cast<uint32_t>(drawable.height);
        std::vector<float> rgba;
        if (width > 0 && height > 0 &&
            Render(frames, timeline, width, height, &rgba)) {
            // The values the layer hands the display, in the colour space
            // the present has just given it.
            NSData* bytes = [NSData dataWithBytes:rgba.data()
                                           length:rgba.size() * sizeof(float)];
            CGDataProviderRef provider =
                CGDataProviderCreateWithCFData((__bridge CFDataRef)bytes);
            CGImageRef image = CGImageCreate(
                width, height, 32, 128, static_cast<size_t>(width) * 16,
                impl_->layer.colorspace,
                kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents |
                    kCGBitmapByteOrder32Little,
                provider, nullptr, false, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            if (impl_->presentCapture) CGImageRelease(impl_->presentCapture);
            impl_->presentCapture = image;
        }
    }
    return true;
}

void Renderer::CaptureNextPresent() {
    impl_->captureArmed = true;
    if (impl_->presentCapture) CGImageRelease(impl_->presentCapture);
    impl_->presentCapture = nullptr;
}

CGImageRef Renderer::TakePresentCapture() {
    impl_->captureArmed = false;
    CGImageRef image = impl_->presentCapture;
    impl_->presentCapture = nullptr;
    return image;
}

bool Renderer::RenderFramesToPixels(const std::vector<AVFrame*>& frames,
                                    const TimelineRenderData& timeline,
                                    uint32_t width, uint32_t height,
                                    std::vector<float>& rgba) {
    if (width == 0 || height == 0) return false;
    return Render(frames, timeline, width, height, &rgba);
}

const VideoHistogram& Renderer::CurveHistogram() const {
    return impl_->curveHistogram;
}

bool Renderer::Render(const std::vector<AVFrame*>& frames,
                      const TimelineRenderData& timeline, uint32_t width,
                      uint32_t height, std::vector<float>* rgba) {
    if (!impl_->workingPipeline || !impl_->outputPipeline ||
        !impl_->scopePipeline || !impl_->solidPipeline ||
        !impl_->textPipeline || !impl_->queue) {
        return false;
    }

    if (impl_->planes.size() < frames.size()) {
        impl_->planes.resize(frames.size());
        impl_->textureWidths.resize(frames.size(), {});
        impl_->textureHeights.resize(frames.size(), {});
        impl_->textureFormats.resize(frames.size(), AV_PIX_FMT_NONE);
    }
    for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const AVFrame* frame = frames[frameIndex];
        if (!frame) continue;
        const AVPixelFormat pixelFormat =
            static_cast<AVPixelFormat>(frame->format);
        const AVPixFmtDescriptor* pixel = av_pix_fmt_desc_get(pixelFormat);
        if (frame->width <= 0 || frame->height <= 0 || !pixel ||
            pixel->nb_components < 3 ||
            (pixel->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                             AV_PIX_FMT_FLAG_HWACCEL)) ||
            pixel->comp[0].plane == pixel->comp[1].plane ||
            pixel->comp[0].plane == pixel->comp[2].plane ||
            pixel->comp[1].plane == pixel->comp[2].plane ||
            pixel->comp[0].depth != pixel->comp[1].depth ||
            pixel->comp[0].depth != pixel->comp[2].depth ||
            pixel->comp[0].depth < 8 || pixel->comp[0].depth > 16 ||
            !frame->data[0] || !frame->data[1] || !frame->data[2]) {
            const char* name = av_get_pix_fmt_name(pixelFormat);
            std::fprintf(stderr,
                         "Unsupported planar AVFrame passed to renderer: %s\n",
                         name ? name : "unknown");
            return false;
        }
        const bool hasAlpha =
            pixel->nb_components >= 4 && (pixel->flags & AV_PIX_FMT_FLAG_ALPHA);
        if (hasAlpha && (pixel->comp[3].plane < 0 || pixel->comp[3].plane > 3 ||
                         pixel->comp[3].depth != pixel->comp[0].depth ||
                         !frame->data[pixel->comp[3].plane])) {
            std::fprintf(stderr,
                         "Unsupported alpha plane passed to renderer\n");
            return false;
        }
        std::array<int, 4> planeWidths = {
            frame->width, AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w),
            AV_CEIL_RSHIFT(frame->width, pixel->log2_chroma_w), frame->width};
        std::array<int, 4> planeHeights = {
            frame->height, AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h),
            AV_CEIL_RSHIFT(frame->height, pixel->log2_chroma_h), frame->height};
        if (impl_->textureWidths[frameIndex] != planeWidths ||
            impl_->textureHeights[frameIndex] != planeHeights ||
            impl_->textureFormats[frameIndex] != frame->format) {
            const int planeCount = hasAlpha ? 4 : 3;
            for (int plane = 0; plane < planeCount; ++plane) {
                MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:
                        pixel->comp[0].depth > 8 ? MTLPixelFormatR16Unorm
                                                 : MTLPixelFormatR8Unorm
                                                 width:planeWidths[plane]
                                                height:planeHeights[plane]
                                             mipmapped:NO];
                textureDescriptor.storageMode = MTLStorageModeShared;
                textureDescriptor.usage = MTLTextureUsageShaderRead;
                impl_->planes[frameIndex][plane] =
                    [impl_->device newTextureWithDescriptor:textureDescriptor];
                if (!impl_->planes[frameIndex][plane]) {
                    std::fprintf(
                        stderr, "Unable to allocate Metal layer %zu plane %d\n",
                        frameIndex, plane);
                    return false;
                }
            }
            impl_->textureWidths[frameIndex] = planeWidths;
            impl_->textureHeights[frameIndex] = planeHeights;
            impl_->textureFormats[frameIndex] = frame->format;
        }

        const int planeCount = hasAlpha ? 4 : 3;
        for (int plane = 0; plane < planeCount; ++plane) {
            if (frame->linesize[plane] <= 0) {
                std::fprintf(stderr,
                             "Unsupported negative/zero linesize for layer %zu "
                             "plane %d: %d\n",
                             frameIndex, plane, frame->linesize[plane]);
                return false;
            }
            const MTLRegion region =
                MTLRegionMake2D(0, 0, planeWidths[plane], planeHeights[plane]);
            [impl_->planes[frameIndex][plane]
                replaceRegion:region
                  mipmapLevel:0
                    withBytes:frame->data[plane]
                  bytesPerRow:static_cast<NSUInteger>(frame->linesize[plane])];
        }
    }

    const ColorManagementSettings presentationColor =
        timeline.display_sdr_preview
            ? ColorManagementForSdrPreview(timeline.color_management)
            : timeline.color_management;
    if (presentationColor.enabled) {
        const std::string lutKey = OpenColorIoCacheKey(presentationColor);
        if (lutKey != impl_->ocioLutKey) {
            OpenColorIoLutPair luts;
            std::string error;
            if (!BuildOpenColorIoLuts(presentationColor, luts, error)) {
                std::fprintf(stderr,
                             "Unable to prepare OpenColorIO Metal LUTs: %s\n",
                             error.c_str());
                return false;
            }
            id<MTLTexture> inputLut =
                UploadOpenColorIoLut(impl_->device, luts.input_to_working);
            id<MTLTexture> outputLut =
                UploadOpenColorIoLut(impl_->device, luts.working_to_display);
            if (!inputLut || !outputLut) {
                std::fprintf(stderr,
                             "Unable to allocate OpenColorIO Metal LUTs\n");
                return false;
            }
            impl_->ocioInputLut = inputLut;
            impl_->ocioOutputLut = outputLut;
            impl_->ocioLutKey = lutKey;
        }
    }
    // G3 -- docs/roadmap/GRADE-2026-09.md. A frame whose media declares
    // its own input is decoded with that input's range and matrix, and
    // reaches the working space through that input's LUT.
    std::vector<ColorManagementSettings> frameColors(frames.size(),
                                                     presentationColor);
    std::vector<id<MTLTexture>> frameInputLuts(frames.size(), nil);
    for (size_t frameIndex = 0; frameIndex < frames.size() &&
                                frameIndex < timeline.video_color_inputs.size();
         ++frameIndex) {
        const MediaColorInput& input = timeline.video_color_inputs[frameIndex];
        if (!DeclaresColorInput(input) && !input.graphics) continue;
        frameColors[frameIndex] = WithMediaColorInput(presentationColor, input);
        // DEC31 -- a graphic is decoded with its own matrix and range and
        // reaches the working space through the motion LUT, not an input
        // LUT of its own.
        if (!presentationColor.enabled || input.graphics) continue;
        const std::string key = OpenColorIoCacheKey(frameColors[frameIndex]);
        if (key == impl_->ocioLutKey) continue;
        auto found = impl_->mediaInputLuts.find(key);
        if (found == impl_->mediaInputLuts.end()) {
            OpenColorIoLut3D lut;
            std::string error;
            if (!BuildOpenColorIoInputLut(frameColors[frameIndex], lut,
                                          error)) {
                std::fprintf(stderr,
                             "Unable to prepare a media's input LUT: %s\n",
                             error.c_str());
                return false;
            }
            id<MTLTexture> texture = UploadOpenColorIoLut(impl_->device, lut);
            if (!texture) {
                std::fprintf(stderr,
                             "Unable to allocate a media's input LUT\n");
                return false;
            }
            found = impl_->mediaInputLuts.emplace(key, texture).first;
        }
        frameInputLuts[frameIndex] = found->second;
    }
    // R5 -- a colour dip is mixed in that view too, so a timeline with
    // one needs the same pair of transforms as one with a motion frame.
    const bool anyDisplayReferred =
        std::any_of(timeline.video_display_referred.begin(),
                    timeline.video_display_referred.end(),
                    [](uint8_t value) { return value != 0; }) ||
        std::any_of(timeline.video_veils.begin(), timeline.video_veils.end(),
                    [](const MonitorVeil& veil) { return veil.alpha > 0.0f; });
    if (presentationColor.enabled && anyDisplayReferred) {
        const std::string lutKey = OpenColorIoCacheKey(presentationColor);
        if (lutKey != impl_->ocioMotionLutKey) {
            OpenColorIoLut3D lut;
            std::string error;
            if (!BuildOpenColorIoDisplayReferredWorkingLut(presentationColor,
                                                           lut, error)) {
                std::fprintf(stderr,
                             "Unable to prepare the motion Metal LUT: %s\n",
                             error.c_str());
                return false;
            }
            // The forward view: what BuildOpenColorIoDisplayReferredWorking*
            // inverts -- the SDR view, or for an HLG presentation the HLG
            // view in graphic code values (BH1).
            OpenColorIoLutPair view;
            if (!BuildOpenColorIoGraphicsViewLuts(presentationColor, view,
                                                  error)) {
                std::fprintf(stderr,
                             "Unable to prepare the motion view LUT: %s\n",
                             error.c_str());
                return false;
            }
            id<MTLTexture> motionLut = UploadOpenColorIoLut(impl_->device, lut);
            id<MTLTexture> viewLut =
                UploadOpenColorIoLut(impl_->device, view.working_to_display);
            if (!motionLut || !viewLut) {
                std::fprintf(stderr,
                             "Unable to allocate the motion Metal LUTs\n");
                return false;
            }
            impl_->ocioMotionLut = motionLut;
            impl_->ocioMotionViewLut = viewLut;
            impl_->ocioMotionLutKey = lutKey;
        }
    }
    // R6 -- docs/roadmap/REEL-2026-09.md. The sequence's look, uploaded
    // when its digest changes and left alone otherwise. The samples were
    // read and checked by LoadSequenceLookLut; nothing here parses a file
    // or decides where the look applies.
    if (timeline.look_lut.size > 1 &&
        timeline.look_lut_digest != impl_->lookLutKey) {
        id<MTLTexture> look = UploadLut3D(impl_->device, timeline.look_lut.size,
                                          timeline.look_lut.rgba);
        if (!look) {
            std::fprintf(stderr, "Unable to allocate the look LUT texture\n");
            return false;
        }
        impl_->lookLut = look;
        impl_->lookLutKey = timeline.look_lut_digest;
    }
    if (!impl_->lookLutFallback) {
        impl_->lookLutFallback =
            UploadLut3D(impl_->device, 2, std::vector<float>(2 * 2 * 2 * 4));
        if (!impl_->lookLutFallback) {
            std::fprintf(stderr, "Unable to allocate the look LUT binding\n");
            return false;
        }
    }
    const bool hasLook = timeline.look_lut.size > 1 && impl_->lookLut != nil &&
                         timeline.look_lut_digest == impl_->lookLutKey;
    const bool hlgOutput =
        presentationColor.enabled && presentationColor.output_transfer == "hlg";
    id<CAMetalDrawable> drawable = nil;
    id<MTLTexture> target = nil;
    if (rgba == nullptr) {
        impl_->layer.colorspace =
            hlgOutput ? impl_->hlgColorSpace : impl_->sdrColorSpace;
        impl_->layer.wantsExtendedDynamicRangeContent = hlgOutput;
        // The output pass already writes HLG-encoded values and the layer's
        // HLG color space carries that transfer function. CAEDRMetadata
        // belongs to the separate linear-EDR presentation path; attaching it
        // here makes the system tone-map an encoded signal again and burns
        // the monitor image.
        impl_->layer.EDRMetadata = nil;

        drawable = [impl_->layer nextDrawable];
        if (!drawable) {
            std::fprintf(stderr, "CAMetalLayer returned no drawable\n");
            return false;
        }
        target = drawable.texture;
    } else {
        MTLTextureDescriptor* targetDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:kOutputPixelFormat
                                         width:width
                                        height:height
                                     mipmapped:NO];
        targetDescriptor.storageMode = MTLStorageModePrivate;
        targetDescriptor.usage = MTLTextureUsageRenderTarget;
        target = [impl_->device newTextureWithDescriptor:targetDescriptor];
        if (!target) {
            std::fprintf(stderr, "Unable to allocate the offscreen target\n");
            return false;
        }
    }

    id<MTLCommandBuffer> commandBuffer = [impl_->queue commandBuffer];
    if (!impl_->workingTexture || impl_->workingTexture.width != target.width ||
        impl_->workingTexture.height != target.height) {
        MTLTextureDescriptor* workingDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:target.width
                                        height:target.height
                                     mipmapped:NO];
        workingDescriptor.storageMode = MTLStorageModePrivate;
        workingDescriptor.usage =
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        impl_->workingTexture =
            [impl_->device newTextureWithDescriptor:workingDescriptor];
        if (!impl_->workingTexture) {
            std::fprintf(stderr, "Unable to allocate ACES working texture\n");
            return false;
        }
    }

    MTLRenderPassDescriptor* workingPass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    workingPass.colorAttachments[0].texture = impl_->workingTexture;
    workingPass.colorAttachments[0].loadAction = MTLLoadActionClear;
    workingPass.colorAttachments[0].storeAction = MTLStoreActionStore;
    workingPass.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    id<MTLRenderCommandEncoder> workingEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:workingPass];

    const double scale = impl_->layer ? impl_->layer.contentsScale : 1.0;
    const double videoHeight = std::clamp(timeline.video_height * scale, 0.0,
                                          static_cast<double>(target.height));
    if (videoHeight > 0.0) {
        [workingEncoder
            setViewport:MTLViewport{0.0, 0.0, static_cast<double>(target.width),
                                    videoHeight, 0.0, 1.0}];
        [workingEncoder setRenderPipelineState:impl_->workingPipeline];
        struct PresentationParameters {
            float left;
            float top;
            float width;
            float height;
            int32_t quarterTurns;
            float opacity;
            int32_t colorManagementEnabled;
            int32_t inputGamut;
            int32_t inputTransfer;
            int32_t useAcescct;
            int32_t inputIsRgb;
            int32_t hasSourceAlpha;
            float redFromCr;
            float greenFromCb;
            float greenFromCr;
            float blueFromCb;
            float sampleScale;
            float yOffset;
            float yScale;
            float chromaOffset;
            float chromaScale;
            // R1 -- shader.metal's PresentationParameters: the part of the
            // displayed picture to sample.
            float sourceLeft;
            float sourceTop;
            float sourceWidth;
            float sourceHeight;
            // PA8 -- shader.metal's PresentationParameters: the clip's
            // mask.
            int32_t maskShape;
            float maskCenterX;
            float maskCenterY;
            float maskWidth;
            float maskHeight;
            float maskFeather;
            int32_t maskInverted;
        } parameters = {};
        const auto gamut = [](const std::string& value) -> int32_t {
            if (value == "sony_sgamut3_cine") return 1;
            if (value == "sony_sgamut3") return 2;
            if (value == "rec2020") return 3;
            return 0;
        };
        parameters.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
        parameters.inputGamut = gamut(presentationColor.input_gamut);
        parameters.inputTransfer =
            presentationColor.input_transfer == "sony_slog3"
                ? 1
                : (presentationColor.input_transfer == "linear" ? 2 : 0);
        parameters.useAcescct =
            presentationColor.working_gamut == "acescct" ? 1 : 0;
        for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
            const AVFrame* frame = frames[frameIndex];
            if (!frame) continue;
            const AVPixFmtDescriptor* pixel =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
            const int depth = pixel->comp[0].depth;
            parameters.inputIsRgb =
                (pixel->flags & AV_PIX_FMT_FLAG_RGB) != 0 ? 1 : 0;
            parameters.hasSourceAlpha =
                pixel->nb_components >= 4 &&
                        (pixel->flags & AV_PIX_FMT_FLAG_ALPHA)
                    ? 1
                    : 0;
            // G1 -- the reading ColorManagement.h writes once, for the
            // colour measure to decode as this does.
            const YuvRangeTag rangeTag =
                frame->color_range == AVCOL_RANGE_JPEG ? YuvRangeTag::Full
                : frame->color_range == AVCOL_RANGE_UNSPECIFIED
                    ? YuvRangeTag::Unspecified
                    : YuvRangeTag::Limited;
            const bool fullRange = ResolveYuvFullRange(
                frameColors[frameIndex], rangeTag, parameters.inputIsRgb != 0);
            const YuvCodeParameters code =
                BuildYuvCodeParameters(depth, fullRange);
            parameters.sampleScale = code.sample_scale;
            parameters.yOffset = code.y_offset;
            parameters.yScale = code.y_scale;
            parameters.chromaOffset = code.chroma_offset;
            parameters.chromaScale = code.chroma_scale;
            const bool bt2020Matrix = ResolveYuvBt2020Matrix(
                frameColors[frameIndex],
                frame->colorspace == AVCOL_SPC_BT2020_NCL);
            const YuvMatrixParameters matrix =
                BuildYuvMatrixParameters(bt2020Matrix);
            parameters.redFromCr = matrix.red_from_cr;
            parameters.greenFromCb = matrix.green_from_cb;
            parameters.greenFromCr = matrix.green_from_cr;
            parameters.blueFromCb = matrix.blue_from_cb;
            for (NSUInteger component = 0; component < 3; ++component)
                [workingEncoder
                    setFragmentTexture:
                        impl_->planes[frameIndex][pixel->comp[component].plane]
                               atIndex:component];
            id<MTLTexture> alphaTexture =
                parameters.hasSourceAlpha
                    ? impl_->planes[frameIndex][pixel->comp[3].plane]
                    : nil;
            [workingEncoder setFragmentTexture:alphaTexture atIndex:3];
            // MOTION-2026-09 M4 -- a motion frame reaches the working space
            // through its own transform, as in the export.
            const bool displayReferred =
                frameIndex < timeline.video_display_referred.size() &&
                timeline.video_display_referred[frameIndex] != 0;
            if (presentationColor.enabled)
                [workingEncoder
                    setFragmentTexture:displayReferred
                                           ? impl_->ocioMotionLut
                                           : (frameInputLuts[frameIndex]
                                                  ? frameInputLuts[frameIndex]
                                                  : impl_->ocioInputLut)
                               atIndex:4];
            // Visual QA of 19 September 2026 -- managed, a motion frame is
            // mixed in the SDR view (compositeDisplayReferred), or for an
            // HLG presentation in the HLG view (BH1), as in the export;
            // unmanaged, the working space already is the display.
            const bool mixInView = presentationColor.enabled &&
                                   displayReferred &&
                                   impl_->ocioMotionViewLut != nil;
            struct ViewParameters {  // shader.metal's OutputParameters
                int32_t colorManagementEnabled;
                int32_t outputGamut;
                int32_t outputTransfer;
                int32_t scopeMode;
                uint32_t drawableWidth;
                uint32_t drawableHeight;
                uint32_t scopeSampleStep;
                uint32_t useAcescct;
                // R6 -- left at zero: this is the view a motion layer is
                // mixed in, without the look. The look belongs to the
                // delivery and is laid once, on the finished composite.
                uint32_t lookLutEdge;
                uint32_t lookAfterDisplay;
                // PRD37 -- no scope here; spelled because the struct goes
                // to the shader whole.
                float scopeLeft;
                float scopeTop;
                float scopeWidth;
                float scopeHeight;
            } view = {};
            if (mixInView) {
                view.colorManagementEnabled = 1;
                view.useAcescct =
                    ColorManagementForSdrPreview(presentationColor)
                                .working_gamut == "acescct"
                        ? 1u
                        : 0u;
                [workingEncoder
                    setRenderPipelineState:impl_->displayReferredPipeline];
                [workingEncoder setFragmentTexture:impl_->ocioMotionViewLut
                                           atIndex:5];
                [workingEncoder setFragmentBytes:&view
                                          length:sizeof(view)
                                         atIndex:2];
            } else {
                [workingEncoder setRenderPipelineState:impl_->workingPipeline];
            }
            const int32_t degrees =
                frameIndex < timeline.video_rotation_degrees.size()
                    ? timeline.video_rotation_degrees[frameIndex]
                    : 0;
            const int32_t turns =
                static_cast<int32_t>(std::lround(degrees / 90.0));
            parameters.quarterTurns = ((turns % 4) + 4) % 4;
            const double displayedWidth =
                parameters.quarterTurns % 2 ? frame->height : frame->width;
            const double displayedHeight =
                parameters.quarterTurns % 2 ? frame->width : frame->height;
            const double viewportAspect =
                static_cast<double>(target.width) / videoHeight;
            const double sequenceAspect =
                timeline.sequence_width > 0 && timeline.sequence_height > 0
                    ? static_cast<double>(timeline.sequence_width) /
                          timeline.sequence_height
                    : viewportAspect;
            double canvasWidth = target.width;
            double canvasHeight = videoHeight;
            if (sequenceAspect > viewportAspect) {
                canvasHeight = canvasWidth / sequenceAspect;
            } else {
                canvasWidth = canvasHeight * sequenceAspect;
            }
            // R1 -- docs/roadmap/REEL-2026-09.md. A framed clip is placed
            // at the rectangle ClipFraming.h resolved, mapped from sequence
            // pixels into the canvas: no scale of this file's own, so the
            // monitor and the export cannot answer differently (guard rail
            // 1). A clip that frames nothing keeps the fit below, pixel for
            // pixel, which is why the monitor of every existing project is
            // unchanged.
            const MonitorFraming framing =
                frameIndex < timeline.video_framings.size()
                    ? timeline.video_framings[frameIndex]
                    : MonitorFraming{};
            parameters.sourceLeft = framing.source_left;
            parameters.sourceTop = framing.source_top;
            parameters.sourceWidth =
                framing.width > 0 ? framing.source_width : 1.0f;
            parameters.sourceHeight =
                framing.width > 0 ? framing.source_height : 1.0f;
            // EF3 -- how many drawable pixels one sequence pixel of this
            // picture covers, on each axis: what carries a blur's sigma from
            // the document's pixels into the monitor's. Zero where the
            // sequence has no size.
            double perSequencePixelX = 0.0;
            double perSequencePixelY = 0.0;
            if (framing.width > 0 && framing.height > 0 &&
                timeline.sequence_width > 0 && timeline.sequence_height > 0) {
                // The zoom magnifies the sequence frame here rather than the
                // source: a framed clip's picture is defined against the
                // frame, and at zoom 0 the canvas is the one computed above,
                // so the two agree where they overlap.
                double framedCanvasWidth = canvasWidth;
                double framedCanvasHeight = canvasHeight;
                if (timeline.video_zoom > 0.0) {
                    framedCanvasWidth =
                        timeline.sequence_width * timeline.video_zoom;
                    framedCanvasHeight =
                        timeline.sequence_height * timeline.video_zoom;
                }
                const double pixelsPerUnitX =
                    framedCanvasWidth / timeline.sequence_width;
                const double pixelsPerUnitY =
                    framedCanvasHeight / timeline.sequence_height;
                perSequencePixelX = pixelsPerUnitX;
                perSequencePixelY = pixelsPerUnitY;
                const double canvasLeft =
                    (static_cast<double>(target.width) - framedCanvasWidth) /
                    2.0;
                const double canvasTop =
                    (videoHeight - framedCanvasHeight) / 2.0;
                parameters.width = static_cast<float>(
                    (framing.width * pixelsPerUnitX) / target.width);
                parameters.height = static_cast<float>(
                    (framing.height * pixelsPerUnitY) / videoHeight);
                parameters.left = static_cast<float>(
                    (canvasLeft + (framing.x * pixelsPerUnitX)) / target.width);
                parameters.top = static_cast<float>(
                    (canvasTop + (framing.y * pixelsPerUnitY)) / videoHeight);
            } else {
                double contentWidth = 0.0;
                double contentHeight = 0.0;
                if (timeline.video_zoom > 0.0 &&
                    timeline.video_zoom_frames_sequence &&
                    timeline.sequence_width > 0 &&
                    timeline.sequence_height > 0) {
                    // DEC42 -- Record zooms the sequence frame: the picture
                    // fits the zoomed frame as it fits the frame unzoomed.
                    const ui::caption_overlay::PictureSize fitted =
                        ui::caption_overlay::FitPictureInFrame(
                            displayedWidth, displayedHeight,
                            timeline.sequence_width * timeline.video_zoom,
                            timeline.sequence_height * timeline.video_zoom);
                    contentWidth = fitted.width;
                    contentHeight = fitted.height;
                } else if (timeline.video_zoom > 0.0) {
                    contentWidth = displayedWidth * timeline.video_zoom;
                    contentHeight = displayedHeight * timeline.video_zoom;
                } else {
                    // Kdenlive-style free resize: derive both dimensions from
                    // one scale so changing only the viewer width can never
                    // stretch or mirror the video content.
                    const double fitScale =
                        std::min(canvasWidth / displayedWidth,
                                 canvasHeight / displayedHeight);
                    contentWidth = displayedWidth * fitScale;
                    contentHeight = displayedHeight * fitScale;
                }
                parameters.width = static_cast<float>(
                    contentWidth / static_cast<double>(target.width));
                parameters.height =
                    static_cast<float>(contentHeight / videoHeight);
                parameters.left = (1.0f - parameters.width) * 0.5f;
                parameters.top = (1.0f - parameters.height) * 0.5f;
                // EF3 -- the picture fitted to the sequence frame, as the
                // export fits it, against what it covers here.
                if (timeline.sequence_width > 0 &&
                    timeline.sequence_height > 0) {
                    const ui::caption_overlay::PictureSize inSequence =
                        ui::caption_overlay::FitPictureInFrame(
                            displayedWidth, displayedHeight,
                            timeline.sequence_width, timeline.sequence_height);
                    if (inSequence.width > 0.0 && inSequence.height > 0.0) {
                        perSequencePixelX = contentWidth / inSequence.width;
                        perSequencePixelY = contentHeight / inSequence.height;
                    }
                }
            }
            parameters.opacity =
                frameIndex < timeline.video_opacities.size()
                    ? std::clamp(timeline.video_opacities[frameIndex], 0.0f,
                                 1.0f)
                    : 1.0f;
            // PA8 -- docs/roadmap/PARITE-2026-11.md. The mask, handed over
            // as the framing is: the renderer evaluates the rule, it does
            // not read the document.
            const MonitorMask mask = frameIndex < timeline.video_masks.size()
                                         ? timeline.video_masks[frameIndex]
                                         : MonitorMask{};
            parameters.maskShape = mask.shape;
            parameters.maskCenterX = mask.center_x;
            parameters.maskCenterY = mask.center_y;
            parameters.maskWidth = mask.width;
            parameters.maskHeight = mask.height;
            parameters.maskFeather = mask.feather;
            parameters.maskInverted = mask.inverted ? 1 : 0;
            // Renderer.mm never interprets a grade entry's `kind`: it sends
            // what ColorEffects.h packs, the byte layout shader.metal's
            // ClipGradeParameters mirrors (checked on the GPU by
            // tests/color_grade_shader_tests.mm).
            const GpuClipGradeParameters gradeParameters =
                frameIndex < timeline.video_color_grades.size()
                    ? PackColorGradeForGpu(
                          timeline.video_color_grades[frameIndex])
                    : GpuClipGradeParameters{};
            // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred layer is
            // drawn in three passes, and the composite's pass is split
            // around the first two: Metal Performance Shaders encode on the
            // command buffer, never inside a render pass. The picture is
            // drawn alone, ungraded and opaque, into a texture the size of
            // the rectangle it covers here; blurred there by a Gaussian of
            // its sigma carried into those pixels, the texture's edges
            // extended (the export's `gblur` extends the scaled picture's,
            // ClipBlur.h); then graded and laid with its opacity, or mixed
            // in the graphics view, as a sharp layer is. A sharp layer
            // keeps the single draw below, and every project that blurs
            // nothing its passes and its pixels.
            const float blur = frameIndex < timeline.video_blurs.size()
                                   ? timeline.video_blurs[frameIndex]
                                   : 0.0f;
            bool drawnBlurred = false;
            if (blur > 0.0f && !impl_->blurSupported &&
                !impl_->blurUnsupportedReported) {
                impl_->blurUnsupportedReported = true;
                std::fprintf(stderr,
                             "Metal Performance Shaders do not run on this "
                             "GPU: Record draws blurred clips sharp\n");
            }
            const double rectWidth =
                static_cast<double>(parameters.width) * target.width;
            const double rectHeight =
                static_cast<double>(parameters.height) * videoHeight;
            if (blur > 0.0f && impl_->blurSupported && rectWidth >= 1.0 &&
                rectHeight >= 1.0 && perSequencePixelX > 0.0 &&
                perSequencePixelY > 0.0) {
                // A picture zoomed far past the viewer is blurred at a
                // reduced size, its sigma reduced with it, rather than in a
                // texture the GPU cannot hold.
                constexpr double kLargestSide = 4096.0;
                const double reduction = std::min(
                    1.0, kLargestSide / std::max(rectWidth, rectHeight));
                const NSUInteger textureWidth = static_cast<NSUInteger>(
                    std::max(1.0, std::ceil(rectWidth * reduction)));
                const NSUInteger textureHeight = static_cast<NSUInteger>(
                    std::max(1.0, std::ceil(rectHeight * reduction)));
                // The kernel is round; the two axes scale alike but for the
                // rounding of the picture's rectangle, and take their mean.
                const float sigma = static_cast<float>(
                    blur * 0.5 *
                    ((perSequencePixelX * textureWidth / rectWidth) +
                     (perSequencePixelY * textureHeight / rectHeight)));
                if (!impl_->blurSource ||
                    impl_->blurSource.width != textureWidth ||
                    impl_->blurSource.height != textureHeight) {
                    MTLTextureDescriptor* blurDescriptor = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:
                            MTLPixelFormatRGBA16Float
                                                     width:textureWidth
                                                    height:textureHeight
                                                 mipmapped:NO];
                    blurDescriptor.storageMode = MTLStorageModePrivate;
                    blurDescriptor.usage =
                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                    impl_->blurSource =
                        [impl_->device newTextureWithDescriptor:blurDescriptor];
                    blurDescriptor.usage =
                        MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    impl_->blurResult =
                        [impl_->device newTextureWithDescriptor:blurDescriptor];
                }
                if (impl_->blurKernelSigma != sigma || !impl_->blurKernel) {
                    impl_->blurKernel = [[MPSImageGaussianBlur alloc]
                        initWithDevice:impl_->device
                                 sigma:sigma];
                    impl_->blurKernel.edgeMode = MPSImageEdgeModeClamp;
                    impl_->blurKernelSigma = sigma;
                }
                if (impl_->blurSource && impl_->blurResult &&
                    impl_->blurKernel) {
                    [workingEncoder endEncoding];
                    MTLRenderPassDescriptor* decodePass =
                        [MTLRenderPassDescriptor renderPassDescriptor];
                    decodePass.colorAttachments[0].texture = impl_->blurSource;
                    decodePass.colorAttachments[0].loadAction =
                        MTLLoadActionDontCare;
                    decodePass.colorAttachments[0].storeAction =
                        MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> decoder = [commandBuffer
                        renderCommandEncoderWithDescriptor:decodePass];
                    [decoder setViewport:MTLViewport{
                                             0.0, 0.0,
                                             static_cast<double>(textureWidth),
                                             static_cast<double>(textureHeight),
                                             0.0, 1.0}];
                    [decoder setRenderPipelineState:impl_->decodeLayerPipeline];
                    for (NSUInteger component = 0; component < 3; ++component)
                        [decoder setFragmentTexture:
                                     impl_->planes[frameIndex]
                                                  [pixel->comp[component].plane]
                                            atIndex:component];
                    [decoder setFragmentTexture:alphaTexture atIndex:3];
                    // The whole texture is the rectangle.
                    auto whole = parameters;
                    whole.left = 0.0f;
                    whole.top = 0.0f;
                    whole.width = 1.0f;
                    whole.height = 1.0f;
                    [decoder setFragmentBytes:&whole
                                       length:sizeof(whole)
                                      atIndex:0];
                    [decoder setFragmentSamplerState:impl_->sampler atIndex:0];
                    [decoder drawPrimitives:MTLPrimitiveTypeTriangle
                                vertexStart:0
                                vertexCount:6];
                    [decoder endEncoding];
                    [impl_->blurKernel encodeToCommandBuffer:commandBuffer
                                               sourceTexture:impl_->blurSource
                                          destinationTexture:impl_->blurResult];
                    MTLRenderPassDescriptor* resume =
                        [MTLRenderPassDescriptor renderPassDescriptor];
                    resume.colorAttachments[0].texture = impl_->workingTexture;
                    resume.colorAttachments[0].loadAction = MTLLoadActionLoad;
                    resume.colorAttachments[0].storeAction =
                        MTLStoreActionStore;
                    workingEncoder = [commandBuffer
                        renderCommandEncoderWithDescriptor:resume];
                    [workingEncoder
                        setViewport:MTLViewport{
                                        0.0, 0.0,
                                        static_cast<double>(target.width),
                                        videoHeight, 0.0, 1.0}];
                    [workingEncoder setFragmentTexture:impl_->blurResult
                                               atIndex:0];
                    if (mixInView) {
                        [workingEncoder
                            setRenderPipelineState:
                                impl_->blurredDisplayReferredPipeline];
                        [workingEncoder setFragmentTexture:impl_->ocioMotionLut
                                                   atIndex:4];
                        [workingEncoder
                            setFragmentTexture:impl_->ocioMotionViewLut
                                       atIndex:5];
                        [workingEncoder setFragmentBytes:&view
                                                  length:sizeof(view)
                                                 atIndex:2];
                    } else {
                        [workingEncoder
                            setRenderPipelineState:impl_->
                                                   blurredWorkingPipeline];
                        if (presentationColor.enabled)
                            [workingEncoder
                                setFragmentTexture:
                                    displayReferred
                                        ? impl_->ocioMotionLut
                                        : (frameInputLuts[frameIndex]
                                               ? frameInputLuts[frameIndex]
                                               : impl_->ocioInputLut)
                                           atIndex:4];
                    }
                    [workingEncoder setFragmentBytes:&parameters
                                              length:sizeof(parameters)
                                             atIndex:0];
                    [workingEncoder setFragmentBytes:&gradeParameters
                                              length:sizeof(gradeParameters)
                                             atIndex:1];
                    [workingEncoder setFragmentSamplerState:impl_->sampler
                                                    atIndex:0];
                    [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                       vertexStart:0
                                       vertexCount:6];
                    [workingEncoder
                        setRenderPipelineState:impl_->workingPipeline];
                    drawnBlurred = true;
                }
            }
            if (!drawnBlurred) {
                [workingEncoder setFragmentBytes:&parameters
                                          length:sizeof(parameters)
                                         atIndex:0];
                [workingEncoder setFragmentBytes:&gradeParameters
                                          length:sizeof(gradeParameters)
                                         atIndex:1];
                [workingEncoder setFragmentSamplerState:impl_->sampler
                                                atIndex:0];
                [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                   vertexStart:0
                                   vertexCount:6];
            }
            // R5 -- docs/roadmap/REEL-2026-09.md. The colour dip over this
            // layer, painted straight after it and before the layer above,
            // because a transition belongs to one track. The rectangle is
            // the one this frame was just drawn into, so the veil covers
            // the picture and not the letterbox beside it -- which is what
            // the export does too, where `fade` acts on the picture before
            // it is placed in the frame. The alpha is the exact fraction
            // TransitionRender.h computed; nothing here derives a curve.
            const MonitorVeil veil = frameIndex < timeline.video_veils.size()
                                         ? timeline.video_veils[frameIndex]
                                         : MonitorVeil{};
            if (veil.alpha > 0.0f) {
                struct VeilParameters {
                    float rect[4];
                    float topColor[4];
                    float bottomColor[4];
                    float drawableSize[2];
                    float padding[2];
                    int32_t colorManagementEnabled;
                    int32_t outputTransfer;
                    int32_t colorPadding[2];
                } dip = {};
                dip.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
                dip.outputTransfer = hlgOutput ? 1 : 0;
                dip.drawableSize[0] = static_cast<float>(target.width);
                dip.drawableSize[1] = static_cast<float>(videoHeight);
                dip.rect[0] =
                    parameters.left * static_cast<float>(target.width);
                dip.rect[1] = parameters.top * static_cast<float>(videoHeight);
                dip.rect[2] =
                    parameters.width * static_cast<float>(target.width);
                dip.rect[3] =
                    parameters.height * static_cast<float>(videoHeight);
                const float alpha = std::clamp(veil.alpha, 0.0f, 1.0f);
                for (int channel = 0; channel < 3; ++channel) {
                    const float value = channel == 0   ? veil.red
                                        : channel == 1 ? veil.green
                                                       : veil.blue;
                    dip.topColor[channel] = value;
                    dip.bottomColor[channel] = value;
                }
                dip.topColor[3] = alpha;
                dip.bottomColor[3] = alpha;
                // Managed, the colour is mixed in the SDR view, as the
                // export does (fragment_solid_display_referred); the same
                // pair of transforms a motion frame is mixed through.
                const bool dipInView = presentationColor.enabled &&
                                       impl_->ocioMotionLut != nil &&
                                       impl_->ocioMotionViewLut != nil;
                if (dipInView) {
                    struct ViewParameters {  // shader.metal's OutputParameters
                        int32_t colorManagementEnabled;
                        int32_t outputGamut;
                        int32_t outputTransfer;
                        int32_t scopeMode;
                        uint32_t drawableWidth;
                        uint32_t drawableHeight;
                        uint32_t scopeSampleStep;
                        uint32_t useAcescct;
                        // R6 -- zero: the SDR view is not the delivery, and
                        // the look is laid once on the finished composite.
                        uint32_t lookLutEdge;
                        uint32_t lookAfterDisplay;
                        // PRD37 -- no scope here either.
                        float scopeLeft;
                        float scopeTop;
                        float scopeWidth;
                        float scopeHeight;
                    } view = {};
                    view.colorManagementEnabled = 1;
                    view.useAcescct =
                        ColorManagementForSdrPreview(presentationColor)
                                    .working_gamut == "acescct"
                            ? 1u
                            : 0u;
                    [workingEncoder
                        setRenderPipelineState:impl_->
                                               solidDisplayReferredPipeline];
                    [workingEncoder setFragmentTexture:impl_->ocioMotionLut
                                               atIndex:4];
                    [workingEncoder setFragmentTexture:impl_->ocioMotionViewLut
                                               atIndex:5];
                    [workingEncoder setFragmentSamplerState:impl_->sampler
                                                    atIndex:0];
                    [workingEncoder setFragmentBytes:&view
                                              length:sizeof(view)
                                             atIndex:2];
                } else {
                    [workingEncoder
                        setRenderPipelineState:impl_->solidPipeline];
                }
                [workingEncoder setVertexBytes:&dip
                                        length:sizeof(dip)
                                       atIndex:0];
                [workingEncoder setFragmentBytes:&dip
                                          length:sizeof(dip)
                                         atIndex:0];
                [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                                   vertexStart:0
                                   vertexCount:6];
                [workingEncoder setRenderPipelineState:impl_->workingPipeline];
            }
        }
    }
    [workingEncoder
        setViewport:MTLViewport{0.0, 0.0, static_cast<double>(target.width),
                                static_cast<double>(target.height), 0.0, 1.0}];
    struct SolidParameters {
        float rect[4];
        float topColor[4];
        float bottomColor[4];
        float drawableSize[2];
        float padding[2];
        int32_t colorManagementEnabled;
        int32_t outputTransfer;
        int32_t colorPadding[2];
    } solid = {};
    solid.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
    solid.outputTransfer = hlgOutput ? 1 : 0;
    solid.drawableSize[0] = static_cast<float>(target.width);
    solid.drawableSize[1] = static_cast<float>(target.height);

    struct TextParameters {
        float rect[4];
        float uv[4];
        float color[4];
        float drawableSize[2];
        float padding[2];
        int32_t colorManagementEnabled;
        int32_t outputTransfer;
        int32_t colorPadding[2];
    } text = {};
    text.drawableSize[0] = static_cast<float>(target.width);
    text.drawableSize[1] = static_cast<float>(target.height);
    text.colorManagementEnabled = presentationColor.enabled ? 1 : 0;
    text.outputTransfer = hlgOutput ? 1 : 0;

    const auto drawRect = [&](const MetalRect& item) {
        double left = item.x;
        double itemTop = item.y;
        double width = item.width;
        double height = item.height;
        if (width < 0.0) {
            left += width;
            width = -width;
        }
        if (height < 0.0) {
            itemTop += height;
            height = -height;
        }
        if (width <= 0.0 || height <= 0.0) return;
        solid.rect[0] = static_cast<float>(left * scale);
        solid.rect[1] = static_cast<float>(itemTop * scale);
        solid.rect[2] = static_cast<float>(width * scale);
        solid.rect[3] = static_cast<float>(height * scale);
        solid.topColor[0] = item.red;
        solid.topColor[1] = item.green;
        solid.topColor[2] = item.blue;
        solid.topColor[3] = item.alpha;
        solid.bottomColor[0] = item.bottom_red;
        solid.bottomColor[1] = item.bottom_green;
        solid.bottomColor[2] = item.bottom_blue;
        solid.bottomColor[3] = item.bottom_alpha;
        [workingEncoder setRenderPipelineState:impl_->solidPipeline];
        [workingEncoder setVertexBytes:&solid length:sizeof(solid) atIndex:0];
        [workingEncoder setFragmentBytes:&solid length:sizeof(solid) atIndex:0];
        [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:0
                           vertexCount:6];
    };
    const auto drawAtlasQuad = [&](const MetalAtlasQuad& quad) {
        text.rect[0] = static_cast<float>(quad.x * scale);
        text.rect[1] = static_cast<float>(quad.y * scale);
        text.rect[2] = static_cast<float>(quad.width * scale);
        text.rect[3] = static_cast<float>(quad.height * scale);
        text.uv[0] = quad.u0;
        text.uv[1] = quad.v0;
        text.uv[2] = quad.u1;
        text.uv[3] = quad.v1;
        text.color[0] = quad.color.r;
        text.color[1] = quad.color.g;
        text.color[2] = quad.color.b;
        text.color[3] = quad.color.a;
        [workingEncoder setRenderPipelineState:impl_->textPipeline];
        [workingEncoder setVertexBytes:&text length:sizeof(text) atIndex:0];
        [workingEncoder setFragmentBytes:&text length:sizeof(text) atIndex:0];
        [workingEncoder setFragmentTexture:impl_->uiAtlas.texture() atIndex:0];
        [workingEncoder setFragmentSamplerState:impl_->sampler atIndex:0];
        [workingEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                           vertexStart:0
                           vertexCount:6];
    };

    std::vector<MetalAtlasQuad> textQuads;
    for (const MetalDrawCommand& command : timeline.overlays) {
        if (const MetalRect* rectangle = std::get_if<MetalRect>(&command)) {
            drawRect(*rectangle);
        } else if (const MetalText* textItem =
                       std::get_if<MetalText>(&command)) {
            textQuads.clear();
            impl_->uiAtlas.AppendTextQuads(*textItem, textQuads);
            for (const MetalAtlasQuad& quad : textQuads) drawAtlasQuad(quad);
        } else if (const MetalIcon* icon = std::get_if<MetalIcon>(&command)) {
            MetalAtlasQuad quad;
            if (impl_->uiAtlas.IconQuad(*icon, quad)) drawAtlasQuad(quad);
        }
    }
    [workingEncoder endEncoding];

    const size_t scopeBins = VideoScopeHistogramBinCount(timeline.video_scope);
    if (!impl_->scopeHistogram || impl_->scopeHistogramBins < scopeBins) {
        impl_->scopeHistogram =
            [impl_->device newBufferWithLength:scopeBins * sizeof(uint32_t)
                                       options:MTLResourceStorageModePrivate];
        impl_->scopeHistogramBins = scopeBins;
    }
    if (!impl_->scopeHistogram) {
        std::fprintf(stderr, "Unable to allocate video scope histogram\n");
        return false;
    }

    const VideoScopePanel scopePanel = LayoutVideoScopePanel(
        static_cast<double>(target.width), static_cast<double>(target.height),
        timeline.video_scope_bottom_inset);
    struct OutputParameters {
        int32_t colorManagementEnabled;
        int32_t outputGamut;
        int32_t outputTransfer;
        int32_t scopeMode;
        uint32_t drawableWidth;
        uint32_t drawableHeight;
        uint32_t scopeSampleStep;
        uint32_t useAcescct;
        // R6 -- zero for a timeline without a look.
        uint32_t lookLutEdge;
        // R6 -- one when the look is laid on the delivered signal
        // (LookLutStage::DeliveredDisplay), zero on the working log values.
        uint32_t lookAfterDisplay;
        // PRD37 -- the scope panel, in drawable pixels, as VideoScopes.h
        // lays it out: the shader draws in it and decides nothing.
        float scopeLeft;
        float scopeTop;
        float scopeWidth;
        float scopeHeight;
    } outputParameters = {
        presentationColor.enabled ? 1 : 0,
        presentationColor.output_gamut == "rec2020" ? 1 : 0,
        hlgOutput ? 1 : 0,
        static_cast<int32_t>(timeline.video_scope),
        static_cast<uint32_t>(target.width),
        static_cast<uint32_t>(target.height),
        2,
        presentationColor.working_gamut == "acescct" ? 1u : 0u,
        hasLook ? static_cast<uint32_t>(timeline.look_lut.size) : 0u,
        timeline.look_lut_stage == LookLutStage::DeliveredDisplay ? 1u : 0u,
        static_cast<float>(scopePanel.x),
        static_cast<float>(scopePanel.y),
        static_cast<float>(scopePanel.width),
        static_cast<float>(scopePanel.height),
    };

    id<MTLBlitCommandEncoder> scopeClear = [commandBuffer blitCommandEncoder];
    [scopeClear fillBuffer:impl_->scopeHistogram
                     range:NSMakeRange(0, scopeBins * sizeof(uint32_t))
                     value:0];
    [scopeClear endEncoding];
    if (timeline.video_scope != VideoScopeMode::Off) {
        id<MTLComputeCommandEncoder> scopeEncoder =
            [commandBuffer computeCommandEncoder];
        [scopeEncoder setComputePipelineState:impl_->scopePipeline];
        [scopeEncoder setTexture:impl_->workingTexture atIndex:0];
        if (presentationColor.enabled)
            [scopeEncoder setTexture:impl_->ocioOutputLut atIndex:1];
        [scopeEncoder
            setTexture:hasLook ? impl_->lookLut : impl_->lookLutFallback
               atIndex:2];
        [scopeEncoder setSamplerState:impl_->sampler atIndex:0];
        [scopeEncoder setBuffer:impl_->scopeHistogram offset:0 atIndex:0];
        [scopeEncoder setBytes:&outputParameters
                        length:sizeof(outputParameters)
                       atIndex:1];
        const NSUInteger threadWidth =
            impl_->scopePipeline.threadExecutionWidth;
        const NSUInteger threadHeight = std::max<NSUInteger>(
            1,
            impl_->scopePipeline.maxTotalThreadsPerThreadgroup / threadWidth);
        const MTLSize threads = MTLSizeMake(threadWidth, threadHeight, 1);
        const MTLSize grid =
            MTLSizeMake((target.width + outputParameters.scopeSampleStep - 1) /
                            outputParameters.scopeSampleStep,
                        (target.height + outputParameters.scopeSampleStep - 1) /
                            outputParameters.scopeSampleStep,
                        1);
        [scopeEncoder dispatchThreads:grid threadsPerThreadgroup:threads];
        [scopeEncoder endEncoding];
    }

    // DEC42 -- bin the same transformed signal as the scopes, in the
    // visible sequence frame. Diagnostic overlays and the reference split
    // are added later, and the viewer's letterboxing is excluded.
    if (timeline.curve_histogram) {
        if (impl_->curveHistogramBuffer == nil) {
            impl_->curveHistogramBuffer = [impl_->device
                newBufferWithLength:sizeof(VideoHistogram)
                            options:MTLResourceStorageModeShared];
        }
        if (impl_->curveHistogramBuffer == nil) {
            return false;
        }
        id<MTLBlitCommandEncoder> clear = [commandBuffer blitCommandEncoder];
        [clear fillBuffer:impl_->curveHistogramBuffer
                    range:NSMakeRange(0, sizeof(VideoHistogram))
                    value:0];
        [clear endEncoding];
        const auto canvas = ui::caption_overlay::Compute(
            timeline.sequence_width, timeline.sequence_height, target.width,
            videoHeight, timeline.video_zoom);
        const std::array<float, 4> rectangle = {
            static_cast<float>(canvas.canvas_x),
            static_cast<float>(canvas.canvas_y),
            static_cast<float>(canvas.canvas_x + canvas.canvas_width),
            static_cast<float>(canvas.canvas_y + canvas.canvas_height)};
        id<MTLComputeCommandEncoder> histogram =
            [commandBuffer computeCommandEncoder];
        [histogram setComputePipelineState:impl_->curveHistogramPipeline];
        [histogram setTexture:impl_->workingTexture atIndex:0];
        if (presentationColor.enabled) {
            [histogram setTexture:impl_->ocioOutputLut atIndex:1];
        }
        [histogram setTexture:hasLook ? impl_->lookLut : impl_->lookLutFallback
                      atIndex:2];
        [histogram setSamplerState:impl_->sampler atIndex:0];
        [histogram setBuffer:impl_->curveHistogramBuffer offset:0 atIndex:0];
        [histogram setBytes:&outputParameters
                     length:sizeof(outputParameters)
                    atIndex:1];
        [histogram setBytes:rectangle.data()
                     length:sizeof(rectangle)
                    atIndex:2];
        const NSUInteger threadWidth =
            impl_->curveHistogramPipeline.threadExecutionWidth;
        const NSUInteger threadHeight = std::max<NSUInteger>(
            1, impl_->curveHistogramPipeline.maxTotalThreadsPerThreadgroup /
                   threadWidth);
        [histogram dispatchThreads:MTLSizeMake((target.width + 1) / 2,
                                               (target.height + 1) / 2, 1)
             threadsPerThreadgroup:MTLSizeMake(threadWidth, threadHeight, 1)];
        [histogram endEncoding];
    }

    // G5 -- docs/roadmap/GRADE-2026-09.md. The graticule's marks and the
    // false-colour bands, rebuilt when the presentation changes.
    const std::string guideKey =
        std::string(presentationColor.enabled ? "managed|" : "display|") +
        (presentationColor.enabled ? OpenColorIoCacheKey(presentationColor)
                                   : std::string());
    if (guideKey != impl_->scopeGuideKey) {
        VideoScopeGuides guides;
        std::string error;
        if (!BuildVideoScopeGuides(presentationColor, guides, error)) {
            std::fprintf(stderr, "Unable to place the scope guides: %s\n",
                         error.c_str());
            return false;
        }
        Impl::ScopeGuides& out = impl_->scopeGuides;
        out.middleGrey = guides.middle_grey;
        out.greyLow = guides.grey_band[0];
        out.greyHigh = guides.grey_band[1];
        out.skinLow = guides.skin_band[0];
        out.skinHigh = guides.skin_band[1];
        out.nearBlack = guides.near_black;
        out.nearWhite = guides.near_white;
        out.endCode = guides.end_code;
        out.skinCos = guides.skin_direction[0];
        out.skinSin = guides.skin_direction[1];
        for (size_t index = 0; index < guides.targets.size(); ++index) {
            out.targets[index * 2] = guides.targets[index][0];
            out.targets[index * 2 + 1] = guides.targets[index][1];
        }
        for (size_t index = 0; index < guides.false_colors.size(); ++index)
            for (size_t channel = 0; channel < 3; ++channel)
                out.falseColors[index * 3 + channel] =
                    guides.false_colors[index][channel];
        impl_->scopeGuideKey = guideKey;
    }

    // G6 -- docs/roadmap/GRADE-2026-09.md. The reference still, uploaded
    // when it changes, and its rectangle: the left part of the sequence
    // frame, placed by the canvas a framed clip is placed by above.
    struct ReferenceParameters {  // shader.metal's
        float left;
        float top;
        float right;
        float bottom;
        float sourceWidth;
        float clipBottom;
        int32_t enabled;
    } referenceParameters = {};
    const ReferenceStill* still = timeline.reference_still.get();
    if (still != nullptr && ReferenceFits(*still, timeline.sequence_width,
                                          timeline.sequence_height)) {
        if (timeline.reference_still != impl_->referenceSource) {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                             width:static_cast<NSUInteger>(
                                                       still->width)
                                            height:static_cast<NSUInteger>(
                                                       still->height)
                                         mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture =
                [impl_->device newTextureWithDescriptor:descriptor];
            if (!texture) {
                std::fprintf(stderr,
                             "Unable to allocate the reference texture\n");
                return false;
            }
            [texture
                replaceRegion:MTLRegionMake2D(0, 0, still->width, still->height)
                  mipmapLevel:0
                    withBytes:still->rgba.data()
                  bytesPerRow:static_cast<NSUInteger>(still->width) * 4];
            impl_->referenceTexture = texture;
            impl_->referenceSource = timeline.reference_still;
        }
        const ReferenceSplit split =
            ReferenceSplitFor(timeline.sequence_width, timeline.sequence_height,
                              timeline.reference_position);
        const double viewportAspect =
            static_cast<double>(target.width) / std::max(videoHeight, 1.0);
        const double sequenceAspect =
            static_cast<double>(timeline.sequence_width) /
            timeline.sequence_height;
        double canvasWidth = target.width;
        double canvasHeight = videoHeight;
        if (timeline.video_zoom > 0.0) {
            canvasWidth = timeline.sequence_width * timeline.video_zoom;
            canvasHeight = timeline.sequence_height * timeline.video_zoom;
        } else if (sequenceAspect > viewportAspect) {
            canvasHeight = canvasWidth / sequenceAspect;
        } else {
            canvasWidth = canvasHeight * sequenceAspect;
        }
        const double canvasLeft =
            (static_cast<double>(target.width) - canvasWidth) / 2.0;
        const double canvasTop = (videoHeight - canvasHeight) / 2.0;
        // Unclipped, so the still maps onto the frame even when a zoom
        // puts part of the frame outside the viewer; clipped to the video
        // area's rows when drawn.
        referenceParameters.left = static_cast<float>(canvasLeft);
        referenceParameters.top = static_cast<float>(canvasTop);
        referenceParameters.right = static_cast<float>(
            canvasLeft + canvasWidth * split.width / timeline.sequence_width);
        referenceParameters.bottom =
            static_cast<float>(canvasTop + canvasHeight);
        referenceParameters.sourceWidth =
            static_cast<float>(split.source_width);
        referenceParameters.clipBottom = static_cast<float>(videoHeight);
        referenceParameters.enabled = split.width > 0 && videoHeight > 0.0;
    }

    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder
        setViewport:MTLViewport{0.0, 0.0, static_cast<double>(target.width),
                                static_cast<double>(target.height), 0.0, 1.0}];
    [encoder setRenderPipelineState:impl_->outputPipeline];
    [encoder setFragmentTexture:impl_->workingTexture atIndex:0];
    if (presentationColor.enabled)
        [encoder setFragmentTexture:impl_->ocioOutputLut atIndex:1];
    [encoder
        setFragmentTexture:hasLook ? impl_->lookLut : impl_->lookLutFallback
                   atIndex:2];
    [encoder setFragmentSamplerState:impl_->sampler atIndex:0];
    [encoder setFragmentBuffer:impl_->scopeHistogram offset:0 atIndex:1];
    [encoder setFragmentBytes:&outputParameters
                       length:sizeof(outputParameters)
                      atIndex:0];
    [encoder setFragmentBytes:&impl_->scopeGuides
                       length:sizeof(impl_->scopeGuides)
                      atIndex:2];
    [encoder setFragmentBytes:&referenceParameters
                       length:sizeof(referenceParameters)
                      atIndex:3];
    [encoder setFragmentTexture:referenceParameters.enabled != 0
                                    ? impl_->referenceTexture
                                    : impl_->workingTexture
                        atIndex:3];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6];

    [encoder endEncoding];
    id<MTLBuffer> readback = nil;
    const NSUInteger readbackRow = static_cast<NSUInteger>(width) * 8;
    if (rgba != nullptr) {
        readback =
            [impl_->device newBufferWithLength:readbackRow * height
                                       options:MTLResourceStorageModeShared];
        if (!readback) {
            std::fprintf(stderr, "Unable to allocate the readback buffer\n");
            return false;
        }
        id<MTLBlitCommandEncoder> copy = [commandBuffer blitCommandEncoder];
        [copy copyFromTexture:target
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:readbackRow
            destinationBytesPerImage:readbackRow * height];
        [copy endEncoding];
    } else {
        [commandBuffer presentDrawable:drawable];
    }
    [commandBuffer commit];

    // Keep the synchronous path while the cache/renderer boundary is measured.
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status == MTLCommandBufferStatusError) {
        std::fprintf(stderr, "Metal command failed: %s\n",
                     commandBuffer.error.localizedDescription.UTF8String);
        return false;
    }
    if (timeline.curve_histogram) {
        std::memcpy(&impl_->curveHistogram,
                    impl_->curveHistogramBuffer.contents,
                    sizeof(VideoHistogram));
    } else {
        impl_->curveHistogram = {};
    }
    if (rgba != nullptr) {
        // BGRA10_XR: blue, green, red, alpha, each ten bits at the top of
        // sixteen; 384 codes 0.0 and 894 codes 1.0.
        const auto* words = static_cast<const uint16_t*>(readback.contents);
        const size_t pixels = static_cast<size_t>(width) * height;
        rgba->resize(pixels * 4);
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            const auto decode = [&](size_t component) {
                return (static_cast<float>(words[(pixel * 4) + component] >>
                                           6) -
                        384.0f) /
                       510.0f;
            };
            (*rgba)[(pixel * 4) + 0] = decode(2);
            (*rgba)[(pixel * 4) + 1] = decode(1);
            (*rgba)[(pixel * 4) + 2] = decode(0);
            (*rgba)[(pixel * 4) + 3] = decode(3);
        }
    }
    return true;
}
