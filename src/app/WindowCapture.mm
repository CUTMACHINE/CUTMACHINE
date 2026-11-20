#include "WindowCapture.h"

#import <QuartzCore/QuartzCore.h>

#include <cmath>

NSBitmapImageRep* DrawWindowOffscreen(
    NSWindow* window, const std::vector<WindowCaptureSurface>& surfaces,
    std::string& error) {
    error.clear();
    NSView* content = window.contentView;
    // The frame view draws the title bar; without one, the content alone.
    NSView* frameView = content.superview ?: content;
    if (frameView == nil || frameView.layer == nil ||
        NSIsEmptyRect(frameView.bounds)) {
        error = "the window has no layer to draw";
        return nil;
    }
    const CGFloat scale = window.backingScaleFactor;
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr
                      pixelsWide:std::lround(NSWidth(frameView.bounds) * scale)
                      pixelsHigh:std::lround(NSHeight(frameView.bounds) * scale)
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSCalibratedRGBColorSpace
                     bytesPerRow:0
                    bitsPerPixel:0];
    // Drawn in the window's colour space, as WindowServer receives it.
    if (bitmap != nil && window.colorSpace != nil)
        bitmap =
            [bitmap bitmapImageRepByRetaggingWithColorSpace:window.colorSpace];
    if (bitmap == nil) {
        error = "no bitmap could be made to draw the window in";
        return nil;
    }
    // Points: the context of a bitmap whose size is in points scales itself.
    bitmap.size = frameView.bounds.size;
    NSGraphicsContext* context =
        [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
    // What AppKit still has to draw is drawn now, not on the next turn.
    [frameView displayIfNeeded];
    NSMutableArray<CALayer*>* standIns = [NSMutableArray array];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (const WindowCaptureSurface& surface : surfaces) {
        NSView* view = surface.view;
        if (view == nil || view.layer == nil || surface.image == nullptr ||
            view.window != window || view.isHiddenOrHasHiddenAncestor)
            continue;
        // The drawable's picture stretched over the layer's bounds, as the
        // layer shows it; its subviews' layers stay above it.
        CALayer* standIn = [CALayer layer];
        standIn.frame = view.layer.bounds;
        standIn.contents = (__bridge id)surface.image;
        standIn.contentsGravity = kCAGravityResize;
        [view.layer insertSublayer:standIn atIndex:0];
        [standIns addObject:standIn];
    }
    [frameView.layer renderInContext:context.CGContext];
    for (CALayer* standIn in standIns) [standIn removeFromSuperlayer];
    [CATransaction commit];
    return bitmap;
}

CGRect WindowCaptureRect(NSView* view, CGFloat pixelsPerPoint) {
    NSWindow* window = view.window;
    NSView* content = window.contentView;
    NSView* frameView = content.superview ?: content;
    if (window == nil || frameView == nil || view.isHiddenOrHasHiddenAncestor)
        return CGRectNull;
    const NSRect inFrame = [view convertRect:view.bounds toView:frameView];
    const CGFloat top = frameView.isFlipped
                            ? NSMinY(inFrame)
                            : NSHeight(frameView.bounds) - NSMaxY(inFrame);
    return CGRectMake(std::round(NSMinX(inFrame) * pixelsPerPoint),
                      std::round(top * pixelsPerPoint),
                      std::round(NSWidth(inFrame) * pixelsPerPoint),
                      std::round(NSHeight(inFrame) * pixelsPerPoint));
}

CGImageRef WindowCaptureSrgb(CGImageRef image, CGRect rect, size_t width,
                             size_t height) {
    if (image == nullptr || width == 0 || height == 0) return nullptr;
    CGImageRef cropped = CGImageCreateWithImageInRect(image, rect);
    if (cropped == nullptr) return nullptr;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        nullptr, width, height, 8, 0, srgb,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(srgb);
    CGImageRef converted = nullptr;
    if (context != nullptr) {
        CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), cropped);
        converted = CGBitmapContextCreateImage(context);
        CGContextRelease(context);
    }
    CGImageRelease(cropped);
    return converted;
}

std::string EncodeWindowCapture(CGImageRef image, bool jpeg) {
    if (image == nullptr) return {};
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
    NSData* data =
        jpeg ? [bitmap representationUsingType:NSBitmapImageFileTypeJPEG
                                    properties:@{
                                        NSImageCompressionFactor : @0.9
                                    }]
             : [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                    properties:@{}];
    if (data == nil) return {};
    return std::string(static_cast<const char*>(data.bytes), data.length);
}
