#pragma once

// VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The window drawn without the
// screen.
//
// Why. Agents wrote this interface blind: Phase 2 in a sandbox without
// AppKit or Metal, and since then the only picture of the whole window was
// ScreenCaptureKit's, which wants the window in front and visible (a covered
// window hands back its last composited image, VQA2) and which a Claude Code
// session cannot use at all, lacking the screen-recording permission. AppKit
// can draw a window off screen, but a CAMetalLayer's content is not drawn
// that way: Record, Source and the timeline came out transparent (measured
// on 3 October 2026, PRD28).
//
// How. Each Metal surface's renderer draws the picture it presents a second
// time, into memory (Renderer::CaptureNextPresent). While Core Animation
// renders the window's layer tree into a bitmap (renderInContext:), that
// picture is the lowest sublayer of each surface's layer: above the layer's
// own (unrendered) drawable and below its subviews' layers -- the caption
// fields over Record, the motion canvas -- which is where the layer shows it
// on screen. The frame view's tree is rendered, so the title bar is in the
// picture and its geometry is that of a ScreenCaptureKit capture without
// shadow. The stand-ins leave in the same transaction they came in: nothing
// of them reaches the screen. cacheDisplayInRect: with a stand-in subview
// gave the same pixels (8 October 2026), at the cost of touching the view
// hierarchy.
//
// What it cannot hold: what WindowServer adds while compositing. On macOS
// 26 that is the glass -- the blurred and dimmed scroll edges (backdrop,
// portal and chameleon layers of NSScrollPocket), the materials behind the
// search field and the title bar -- and the dimming of secondary label
// colours, which come out brighter than on screen. Measured on 8 October
// 2026 against ScreenCaptureKit, window composited: the timeline 100 % of
// pixels within 4 codes, Record 99.7 %, Source 99.6 %, the whole content
// 95.05 % within 8; the rest is that glass.

#import <AppKit/AppKit.h>

#include <string>
#include <vector>

// A Metal surface of the window and the picture its renderer presents,
// borrowed for the call.
struct WindowCaptureSurface {
    NSView* view = nil;
    CGImageRef image = nullptr;
};

// The whole window at its backing scale, title bar included, each surface's
// picture laid in its view. A surface hidden, or in another window, is left
// out. Null, with `error`, when the window cannot be drawn.
NSBitmapImageRep* DrawWindowOffscreen(
    NSWindow* window, const std::vector<WindowCaptureSurface>& surfaces,
    std::string& error);

// Where `view` sits in a capture of its window `pixelsPerPoint` pixels per
// point, in the image's pixels from its top-left corner. Empty for a view
// hidden or in no window.
CGRect WindowCaptureRect(NSView* view, CGFloat pixelsPerPoint);

// `rect` of `image` (its pixels, from the top-left corner), resampled to
// `width` x `height` and converted to sRGB: 8 bits per channel, RGBA,
// alpha premultiplied. The space the comparisons and the agent's picture
// are made in, whatever the display. +1 for the caller; null on failure.
CGImageRef WindowCaptureSrgb(CGImageRef image, CGRect rect, size_t width,
                             size_t height);

// `image` as JPEG (quality 0.9: the interface's small text stays legible)
// or PNG. Empty on failure.
std::string EncodeWindowCapture(CGImageRef image, bool jpeg);
