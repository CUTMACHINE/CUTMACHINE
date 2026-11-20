#pragma once

#include "RenderData.h"

#import <AppKit/AppKit.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct AVFrame;

class Renderer {
public:
    Renderer();
    ~Renderer();

    // A nil view prepares a renderer without a layer, for
    // RenderFramesToPixels only.
    bool Initialize(NSView* view);
    void Resize(NSRect bounds);
    // Frames are ordered bottom-to-top and composited in that order. Null
    // entries are timeline holes and reveal lower tracks.
    bool RenderFrames(const std::vector<AVFrame*>& frames,
                      const TimelineRenderData& timeline);
    // Visual QA of 19 September 2026 -- the picture RenderFrames presents,
    // drawn by the same passes into a `width` x `height` target at one pixel
    // per point and read back: red, green, blue and alpha per pixel, top row
    // first, the values the layer hands the display. Lets a test put the
    // monitor's whole chain beside the exported file.
    bool RenderFramesToPixels(const std::vector<AVFrame*>& frames,
                              const TimelineRenderData& timeline,
                              uint32_t width, uint32_t height,
                              std::vector<float>& rgba);
    // VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The window drawn
    // without the screen (WindowCapture.h) needs the picture this layer
    // shows, and the drawable cannot be read back (framebufferOnly). Armed,
    // the next RenderFrames that presents draws the same frames and display
    // list a second time, into a target of the drawable's size, and keeps
    // it. Unarmed, it costs one test per present.
    void CaptureNextPresent();
    // The kept picture as an image in the layer's colour space (+1 for the
    // caller), or null when nothing has presented since it was armed.
    CGImageRef TakePresentCapture();

    // The latest successful render's displayed RGB distribution. Empty
    // when that render did not request one.
    const VideoHistogram& CurveHistogram() const;

private:
    bool Render(const std::vector<AVFrame*>& frames,
                const TimelineRenderData& timeline, uint32_t width,
                uint32_t height, std::vector<float>* rgba);

    struct Impl;
    Impl* impl_;
};
