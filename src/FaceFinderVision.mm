// AGENT-2026-09 A20 -- docs/roadmap/AGENT-2026-09.md. Vision's face
// rectangles as the engine's face finder (FaceFinder.h). Runs on the Mac,
// without any remote service; the picture handed in is the small
// display-referred frame measure_color measures, which is enough for a
// rectangle. Faces come back largest first, as regions of that picture with
// a top-left origin, where Vision's boxes have a bottom-left one.

#include "FaceFinder.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr int32_t kRegionDenominator = 10000;

EffectParamValue Fraction(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return {static_cast<int32_t>(std::llround(clamped * kRegionDenominator)),
            kRegionDenominator};
}

bool VisionFindFaces(int32_t width, int32_t height,
                     const std::vector<uint8_t>& rgb8,
                     std::vector<ColorMeasureRegion>& faces,
                     std::string& error) {
    faces.clear();
    error.clear();
    if (width <= 0 || height <= 0 ||
        rgb8.size() != static_cast<size_t>(width) * height * 3) {
        error = "the picture handed to the face finder does not match its size";
        return false;
    }
    @autoreleasepool {
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, rgb8.data(), rgb8.size(), nullptr);
        CGImageRef image = CGImageCreate(
            static_cast<size_t>(width), static_cast<size_t>(height), 8, 24,
            static_cast<size_t>(width) * 3, colorSpace,
            static_cast<CGBitmapInfo>(kCGImageAlphaNone) |
                kCGBitmapByteOrderDefault,
            provider, nullptr, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(colorSpace);
        if (image == nullptr) {
            error = "the face finder could not build an image";
            return false;
        }
        VNDetectFaceRectanglesRequest* request =
            [[VNDetectFaceRectanglesRequest alloc] init];
        VNImageRequestHandler* handler = [[VNImageRequestHandler alloc]
            initWithCGImage:image
                orientation:kCGImagePropertyOrientationUp
                    options:@{}];
        NSError* failure = nil;
        const BOOL performed = [handler performRequests:@[ request ]
                                                  error:&failure];
        CGImageRelease(image);
        // This library compiles without ARC: what alloc gave is released
        // here, once the results have been read.
        [handler release];
        if (!performed) {
            error =
                failure != nil
                    ? std::string([[failure localizedDescription] UTF8String])
                    : "the face finder failed";
            [request release];
            return false;
        }
        std::vector<std::pair<double, ColorMeasureRegion>> found;
        for (VNFaceObservation* observation in request.results) {
            const CGRect box = observation.boundingBox;
            ColorMeasureRegion region;
            region.present = true;
            region.x = Fraction(box.origin.x);
            region.y = Fraction(1.0 - box.origin.y - box.size.height);
            region.width = Fraction(box.size.width);
            region.height = Fraction(box.size.height);
            if (region.width.num <= 0 || region.height.num <= 0) continue;
            found.push_back({box.size.width * box.size.height, region});
        }
        std::stable_sort(found.begin(), found.end(),
                         [](const auto& left, const auto& right) {
                             return left.first > right.first;
                         });
        [request release];
        for (const auto& entry : found) faces.push_back(entry.second);
    }
    return true;
}

}  // namespace

void InstallFaceFinder() { SetFaceFinder(&VisionFindFaces); }
