// G6 -- docs/roadmap/GRADE-2026-09.md. The reference still, the whole loop
// through the monitor's own passes: Record is grabbed
// (Renderer::RenderFramesToPixels, as main.mm grabs it), the picture then
// changes, and the split shows on its left what Record showed and on its
// right what it shows now -- without colour management and with it. A first
// version sent the still back through the working space like a motion
// frame, and this test measured it 0.078 off under S-Log3 to Rec.709: the
// ACES SDR view's inverse does not hold saturated colours. What the eye
// makes of it stays VISUAL_QA_CHECKLIST.md's.

#include "MonitorCompare.h"
#include "Renderer.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cmath>
#include <iostream>
#include <memory>
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

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 180;

AVFrame* Flat(int red, int green, int blue) {
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_GBRAP;
    frame->width = 64;
    frame->height = 36;
    frame->color_range = AVCOL_RANGE_JPEG;
    av_frame_get_buffer(frame, 0);
    const int values[4] = {green, blue, red, 255};
    for (int plane = 0; plane < 4; ++plane)
        for (int row = 0; row < frame->height; ++row)
            std::fill(frame->data[plane] + row * frame->linesize[plane],
                      frame->data[plane] + row * frame->linesize[plane] +
                          frame->width,
                      static_cast<uint8_t>(values[plane]));
    return frame;
}

TimelineRenderData Data(const ColorManagementSettings& color) {
    TimelineRenderData data;
    data.color_management = color;
    data.display_sdr_preview = true;
    data.sequence_width = static_cast<int32_t>(kWidth);
    data.sequence_height = static_cast<int32_t>(kHeight);
    data.video_height = kHeight;
    data.video_rotation_degrees = {0};
    data.video_opacities = {1.0f};
    return data;
}

std::array<float, 3> Pixel(const std::vector<float>& rgba, uint32_t x,
                           uint32_t y) {
    const size_t at = (static_cast<size_t>(y) * kWidth + x) * 4;
    return {rgba[at], rgba[at + 1], rgba[at + 2]};
}

double Gap(const std::array<float, 3>& left,
           const std::array<float, 3>& right) {
    return std::max({std::abs(left[0] - right[0]), std::abs(left[1] - right[1]),
                     std::abs(left[2] - right[2])});
}

void Loop(Renderer& renderer, const ColorManagementSettings& color,
          const std::string& label) {
    // Record, grabbed.
    std::vector<float> grabbed;
    {
        std::vector<AVFrame*> frames = {Flat(200, 90, 40)};
        const bool rendered = renderer.RenderFramesToPixels(
            frames, Data(color), kWidth, kHeight, grabbed);
        av_frame_free(&frames[0]);
        Check(rendered, label + ": Record renders");
    }
    auto still = std::make_shared<ReferenceStill>(
        ReferenceStillFromRgba(grabbed, kWidth, kHeight));
    Check(ReferenceFits(*still, kWidth, kHeight),
          label + ": the grab is a still of the sequence's size");

    // The picture changes; the split shows both.
    std::vector<AVFrame*> frames = {Flat(60, 120, 210)};
    TimelineRenderData data = Data(color);
    data.reference_still = still;
    data.reference_position = 0.5;
    std::vector<float> split;
    Check(renderer.RenderFramesToPixels(frames, data, kWidth, kHeight, split),
          label + ": the split renders");
    for (AVFrame*& frame : frames) av_frame_free(&frame);
    std::vector<float> live;
    {
        std::vector<AVFrame*> alone = {Flat(60, 120, 210)};
        renderer.RenderFramesToPixels(alone, Data(color), kWidth, kHeight,
                                      live);
        av_frame_free(&alone[0]);
    }
    const auto shownLeft = Pixel(split, kWidth / 4, kHeight / 2);
    const auto grabbedLeft = Pixel(grabbed, kWidth / 4, kHeight / 2);
    const auto shownRight = Pixel(split, kWidth * 3 / 4, kHeight / 2);
    const auto liveRight = Pixel(live, kWidth * 3 / 4, kHeight / 2);
    const double reference = Gap(shownLeft, grabbedLeft);
    const double current = Gap(shownRight, liveRight);
    std::cout << label << ": reference within " << reference << ", live within "
              << current << '\n';
    Check(reference < 0.003,
          label + ": the left half shows what Record showed, within " +
              std::to_string(reference));
    Check(current < 1e-4,
          label + ": the right half is the live picture, untouched: " +
              std::to_string(current));
    Check(Gap(shownLeft, shownRight) > 0.1,
          label + ": and the two halves differ");
    // The seam at the split's column: left of it the still, right of it the
    // live picture.
    Check(Gap(Pixel(split, kWidth / 2 - 1, kHeight / 2), grabbedLeft) < 0.003 &&
              Gap(Pixel(split, kWidth / 2, kHeight / 2), liveRight) < 1e-4,
          label + ": the seam falls on the split's column");
}

// CF1 -- the second screen keeps the composite, not Record's inspection
// aids. Compare the GPU's actual pixels with the clean Record baseline.
void CleanFeed(Renderer& renderer, const ColorManagementSettings& color) {
    std::vector<AVFrame*> frames = {Flat(60, 120, 210)};
    TimelineRenderData baseline = Data(color);
    baseline.video_opacities = {0.7f};
    baseline.video_framings = {
        MonitorFraming{0.0f, 0.0f, 1.0f, 1.0f, 40, 20, 240, 140}};
    std::vector<float> wanted;
    Check(renderer.RenderFramesToPixels(frames, baseline, kWidth, kHeight,
                                        wanted),
          "CF1: the clean composite renders");
    TimelineRenderData inspected = baseline;
    inspected.video_zoom = 4.0;
    inspected.video_scope = VideoScopeMode::Waveform;
    inspected.video_scope_bottom_inset = 24.0;
    inspected.reference_still = std::make_shared<ReferenceStill>(
        ReferenceStillFromRgba(wanted, kWidth, kHeight));
    inspected.overlays.emplace_back(MetalRect{
        0.0, 0.0, kWidth, kHeight, ui::theme::kAccent, ui::theme::kAccent});
    const TimelineRenderData clean = CleanFeedRenderData(inspected, kHeight);
    std::vector<float> output;
    Check(renderer.RenderFramesToPixels(frames, clean, kWidth, kHeight, output),
          "CF1: the clean feed renders");
    Check(output == wanted,
          "CF1: every output pixel equals Record without its inspection aids");
    Check(inspected.video_zoom == 4.0 && inspected.reference_still &&
              !inspected.overlays.empty(),
          "CF1: cleaning the feed leaves Record's own view intact");
    av_frame_free(&frames[0]);
}

}  // namespace

int main() {
    @autoreleasepool {
        if (MTLCreateSystemDefaultDevice() == nil) {
            std::cout << "SKIP: no Metal device\n";
            return 77;
        }
        Renderer renderer;
        Check(renderer.Initialize(nil), "the renderer starts without a view");
        if (failures != 0) return 1;
        Loop(renderer, ColorManagementSettings{}, "unmanaged");
        CleanFeed(renderer, ColorManagementSettings{});
        ColorManagementSettings managed;
        managed.enabled = true;
        managed.input_gamut = "sony_sgamut3_cine";
        managed.input_transfer = "sony_slog3";
        Loop(renderer, managed, "S-Log3 to Rec.709");
        CleanFeed(renderer, managed);
    }
    if (failures == 0)
        std::cout << "monitor_compare_render: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
