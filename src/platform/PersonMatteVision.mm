// PA15 -- docs/roadmap/PARITE-2026-11.md. Vision's person segmentation as
// the engine's segmenter (PersonMatte.h). Runs on the Mac, without any
// remote service. One session is one media: its frames go through a
// VNSequenceRequestHandler, so Vision sees them as a sequence and smooths
// the matte from one frame to the next, as it does for a camera feed.
//
// Measured on 13 November 2026, on a 540 x 960 frame of the bench rush:
// "fast" 35 ms a frame and a 192 x 256 matte, "balanced" 30 ms and
// 384 x 512, "accurate" 73 ms and 1512 x 2016, the hair finest. The matte
// covers the whole picture handed in, stretched to the model's 3:4.

#include "PersonMatte.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <cstring>

namespace {

struct VisionSession {
    VNSequenceRequestHandler* handler = nil;
    VNGeneratePersonSegmentationRequest* request = nil;
};

void* OpenVision(const std::string& quality, std::string& error) {
    @autoreleasepool {
        auto* session = new VisionSession;
        // This library compiles without ARC: what alloc gives is released
        // in CloseVision.
        session->handler = [[VNSequenceRequestHandler alloc] init];
        session->request = [[VNGeneratePersonSegmentationRequest alloc] init];
        if (session->handler == nil || session->request == nil) {
            [session->handler release];
            [session->request release];
            delete session;
            error = "Vision's person segmentation is not available";
            return nullptr;
        }
        session->request.qualityLevel =
            quality == person_matte::kQualityFast
                ? VNGeneratePersonSegmentationRequestQualityLevelFast
            : quality == person_matte::kQualityBalanced
                ? VNGeneratePersonSegmentationRequestQualityLevelBalanced
                : VNGeneratePersonSegmentationRequestQualityLevelAccurate;
        session->request.outputPixelFormat = kCVPixelFormatType_OneComponent8;
        return session;
    }
}

bool SegmentWithVision(void* opaque, int32_t width, int32_t height,
                       const uint8_t* rgb8, int32_t& matteWidth,
                       int32_t& matteHeight, std::vector<uint8_t>& matte,
                       std::string& error) {
    auto* session = static_cast<VisionSession*>(opaque);
    if (session == nullptr || width <= 0 || height <= 0 || rgb8 == nullptr) {
        error = "the picture handed to the segmenter has no size";
        return false;
    }
    @autoreleasepool {
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferCreate(kCFAllocatorDefault, static_cast<size_t>(width),
                                static_cast<size_t>(height),
                                kCVPixelFormatType_32BGRA, nullptr,
                                &buffer) != kCVReturnSuccess ||
            buffer == nullptr) {
            error = "the segmenter could not make a picture buffer";
            return false;
        }
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (int32_t y = 0; y < height; ++y) {
            const uint8_t* in = rgb8 + static_cast<size_t>(y) * width * 3;
            uint8_t* out = base + static_cast<size_t>(y) * stride;
            for (int32_t x = 0; x < width; ++x) {
                out[4 * x + 0] = in[3 * x + 2];
                out[4 * x + 1] = in[3 * x + 1];
                out[4 * x + 2] = in[3 * x + 0];
                out[4 * x + 3] = 255;
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        NSError* failure = nil;
        const BOOL performed =
            [session->handler performRequests:@[ session->request ]
                              onCVPixelBuffer:buffer
                                        error:&failure];
        CVPixelBufferRelease(buffer);
        if (!performed) {
            error = failure != nil
                        ? std::string(failure.localizedDescription.UTF8String)
                        : "Vision's person segmentation failed";
            return false;
        }
        VNPixelBufferObservation* observation =
            session->request.results.firstObject;
        CVPixelBufferRef mask =
            observation != nil ? observation.pixelBuffer : nullptr;
        if (mask == nullptr) {
            error = "Vision gave no matte for this picture";
            return false;
        }
        CVPixelBufferLockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);
        matteWidth = static_cast<int32_t>(CVPixelBufferGetWidth(mask));
        matteHeight = static_cast<int32_t>(CVPixelBufferGetHeight(mask));
        const auto* bytes =
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(mask));
        const size_t maskStride = CVPixelBufferGetBytesPerRow(mask);
        matte.resize(static_cast<size_t>(matteWidth) * matteHeight);
        for (int32_t y = 0; y < matteHeight; ++y)
            std::memcpy(matte.data() + static_cast<size_t>(y) * matteWidth,
                        bytes + static_cast<size_t>(y) * maskStride,
                        static_cast<size_t>(matteWidth));
        CVPixelBufferUnlockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);
    }
    return true;
}

void CloseVision(void* opaque) {
    auto* session = static_cast<VisionSession*>(opaque);
    if (session == nullptr) return;
    [session->handler release];
    [session->request release];
    delete session;
}

const person_matte::Segmenter kVisionSegmenter{&OpenVision, &SegmentWithVision,
                                               &CloseVision};

}  // namespace

namespace person_matte {

void InstallVisionSegmenter() { SetSegmenter(&kVisionSegmenter); }

}  // namespace person_matte
