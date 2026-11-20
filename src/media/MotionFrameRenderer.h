#pragma once

// MOTION-2026-09 M3 -- ROADMAP.md. How a surface of the model reaches the
// motion renderer. Layout and rasterization need Core Text and Core Graphics,
// which live in cutmachine_motion_render, above the model; the MCP tools live
// in the model. The executable that links both installs the renderer here at
// startup (InstallMotionFrameRenderer, MotionCli.h), and a tool that finds
// none says so instead of guessing a layout without the fonts.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MotionFrameRequest {
    const MotionComposition* composition = nullptr;
    // M6 -- a version and a format the composition declares, or empty.
    std::string variant;
    std::string format;
    // Prop overrides for this frame only, or null.
    mcp_json::Value props;
    int64_t frame = 0;
    // Relative image sources resolve here: a project's package.
    std::string media_directory;
    // Registered for the process before measuring
    // (RegisterMotionFontDirectory).
    std::vector<std::string> font_directories;
    // M5 -- false evaluates the frame with the real fonts and stops there:
    // no raster, no PNG. What a check of the layout needs, a hundred frames
    // at a time.
    bool raster = true;
    // REMOTION-PARITY-2026-09 REM2 -- the project's media and the
    // compositions an instance may show (MotionReachFor).
    MotionReach reach;
    // MOTION-DESIGN-2026-09 -- drawn at this fraction of the composition's
    // size (ScaleMotionFrame): what a sequence smaller than the
    // composition shows, rendered at the size it shows it.
    double scale = 1.0;
    // PERF11 -- docs/roadmap/PERF-2026-09.md. Set by a caller that renders
    // several frames at the same time (MotionParallelRender.h): the order
    // in which they decode their video pictures, and this frame's rank in
    // it. The caller leaves the turn itself once the frame is rendered, so
    // a renderer that ignores these blocks nobody.
    MotionMediaTurns* media_turns = nullptr;
    size_t media_turn = 0;
};

struct MotionFrameImage {
    // The frame as PNG bytes, and its geometry as DumpMotionFrame writes it:
    // both from the one evaluation, so the boxes describe the pixels.
    std::string png;
    std::string geometry;
    int32_t width = 0;
    int32_t height = 0;
    // M5 -- the evaluated tree itself, which geometry spells as JSON.
    MotionFrame evaluated;
};

using MotionFrameRenderer = bool (*)(const MotionFrameRequest& request,
                                     MotionFrameImage& image,
                                     MotionError& error);

namespace motion_frame_renderer {
inline std::atomic<MotionFrameRenderer>& Slot() {
    static std::atomic<MotionFrameRenderer> renderer{nullptr};
    return renderer;
}
}  // namespace motion_frame_renderer

inline void SetMotionFrameRenderer(MotionFrameRenderer renderer) {
    motion_frame_renderer::Slot().store(renderer);
}

// Null when this process has no renderer (a test that links the model
// alone).
inline MotionFrameRenderer CurrentMotionFrameRenderer() {
    return motion_frame_renderer::Slot().load();
}

// HEADLESS-2026-10 -- where a renderer is, said by every refusal for the
// want of one. "Use the cutmachine executable" was written for a test
// binary, and told the build without a window (headless/), itself named
// cutmachine, to use itself.
inline constexpr const char* kMotionRendererHolder =
    "only the macOS cutmachine executable has one, not a test binary nor "
    "the build without a window (headless/)";
