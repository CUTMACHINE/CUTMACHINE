// G5 -- docs/roadmap/GRADE-2026-09.md. The scope graticule and false colour,
// read back from the monitor's own passes (Renderer::RenderFramesToPixels):
// each level of a flat picture is painted the band VideoScopes.h gives it,
// and the vectorscope's targets, its skin tone line and the waveform's
// middle grey light up where the guides place them and nowhere near. What
// the screen shows is still the eye's to judge (VISUAL_QA_CHECKLIST.md);
// that the marks are where the numbers say is this test's.

#include "Renderer.h"
#include "VideoScopes.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cmath>
#include <iostream>
#include <numeric>
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

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

// A flat RGB picture at 8-bit codes, alpha opaque.
AVFrame* Flat(int red, int green, int blue) {
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_GBRAP;
    frame->width = 64;
    frame->height = 36;
    frame->color_range = AVCOL_RANGE_JPEG;
    av_frame_get_buffer(frame, 0);
    // GBRAP stores green, blue, red, alpha.
    const int values[4] = {green, blue, red, 255};
    for (int plane = 0; plane < 4; ++plane)
        for (int row = 0; row < frame->height; ++row)
            std::fill(frame->data[plane] + row * frame->linesize[plane],
                      frame->data[plane] + row * frame->linesize[plane] +
                          frame->width,
                      static_cast<uint8_t>(values[plane]));
    return frame;
}

bool Render(Renderer& renderer, int red, int green, int blue,
            VideoScopeMode scope, std::vector<float>& rgba,
            double bottomInset = 0.0) {
    AVFrame* frame = Flat(red, green, blue);
    TimelineRenderData data;
    data.display_sdr_preview = true;
    data.sequence_width = static_cast<int32_t>(kWidth);
    data.sequence_height = static_cast<int32_t>(kHeight);
    data.video_height = kHeight;
    data.video_scope = scope;
    data.video_scope_bottom_inset = bottomInset;
    data.video_rotation_degrees = {0};
    data.video_opacities = {1.0f};
    const bool rendered =
        renderer.RenderFramesToPixels({frame}, data, kWidth, kHeight, rgba);
    av_frame_free(&frame);
    return rendered;
}

std::array<float, 3> Pixel(const std::vector<float>& rgba, double x, double y) {
    const size_t column = static_cast<size_t>(std::lround(x));
    const size_t row = static_cast<size_t>(std::lround(y));
    const size_t at = (row * kWidth + column) * 4;
    return {rgba[at], rgba[at + 1], rgba[at + 2]};
}

float Brightest(const std::array<float, 3>& color) {
    return std::max({color[0], color[1], color[2]});
}

// The scope panel's rectangle, as VideoScopes.h lays it out for the shader
// (PRD37: no longer spelled twice), with no strip of controls under it.
using Panel = VideoScopePanel;
Panel ScopePanel() { return LayoutVideoScopePanel(kWidth, kHeight, 0.0); }

}  // namespace

int main() {
    @autoreleasepool {
        if (MTLCreateSystemDefaultDevice() == nil) {
            std::cout << "SKIP: no Metal device\n";
            return 77;
        }
        Renderer renderer;
        Check(renderer.Initialize(nil), "the renderer starts without a view");
        VideoScopeGuides guides;
        std::string error;
        Check(BuildVideoScopeGuides(ColorManagementSettings{}, guides, error),
              "the unmanaged guides build: " + error);
        if (failures != 0) return 1;

        // DEC42 -- the curve histogram reads the actual displayed RGB
        // signal, including the grade and look, before the scope overlay.
        // It excludes the viewer's black letterbox and forgets old frames.
        {
            AVFrame* frame = Flat(128, 64, 32);
            TimelineRenderData data;
            data.display_sdr_preview = true;
            data.sequence_width = 640;
            data.sequence_height = 360;
            data.video_height = 480;
            data.curve_histogram = true;
            std::vector<float> rgba;
            Check(renderer.RenderFramesToPixels({frame}, data, 640, 480, rgba),
                  "the curve histogram renders with letterboxing");
            const VideoHistogram plain = renderer.CurveHistogram();
            const auto total = [](const auto& bins) {
                return std::accumulate(bins.begin(), bins.end(), uint32_t{0});
            };
            const auto peak = [](const auto& bins) {
                return std::distance(
                    bins.begin(), std::max_element(bins.begin(), bins.end()));
            };
            Check(total(plain[0]) == 640u * 360u / 4u &&
                      total(plain[1]) == total(plain[0]) &&
                      total(plain[2]) == total(plain[0]) && plain[0][0] == 0 &&
                      std::abs(peak(plain[0]) - 128) <= 1 &&
                      std::abs(peak(plain[1]) - 64) <= 1 &&
                      std::abs(peak(plain[2]) - 32) <= 1,
                  "RGB bins contain the picture, excluding both black bars");
            data.video_scope = VideoScopeMode::FalseColor;
            Check(
                renderer.RenderFramesToPixels({frame}, data, 640, 480, rgba) &&
                    renderer.CurveHistogram() == plain,
                "false colour does not enter the curve histogram");
            data.video_scope = VideoScopeMode::Off;
            data.video_color_grades = {ResolveColorGrade(
                {{GenerateUlid(), "color.exposure", {{"amount", {1, 1}}}}},
                ColorGradeContext{})};
            Check(renderer.RenderFramesToPixels({frame}, data, 640, 480, rgba),
                  "a graded picture and its histogram render");
            Check(peak(renderer.CurveHistogram()[0]) > peak(plain[0]) &&
                      peak(renderer.CurveHistogram()[1]) > peak(plain[1]),
                  "the histogram follows the grade seen on Record");
            data.video_color_grades.clear();
            data.look_lut.size = 2;
            for (int blue = 0; blue < 2; ++blue)
                for (int green = 0; green < 2; ++green)
                    for (int red = 0; red < 2; ++red) {
                        data.look_lut.rgba.push_back(1.0f - red);
                        data.look_lut.rgba.push_back(1.0f - green);
                        data.look_lut.rgba.push_back(1.0f - blue);
                        data.look_lut.rgba.push_back(1.0f);
                    }
            data.look_lut_digest = "curve-histogram-inversion";
            data.look_lut_stage = LookLutStage::DeliveredDisplay;
            Check(
                renderer.RenderFramesToPixels({frame}, data, 640, 480, rgba) &&
                    std::abs(peak(renderer.CurveHistogram()[2]) - 223) <= 1,
                "the histogram follows the delivered look");
            data.curve_histogram = false;
            Check(
                renderer.RenderFramesToPixels({frame}, data, 640, 480, rgba) &&
                    renderer.CurveHistogram() == VideoHistogram{},
                "a disabled histogram clears the preceding frame");
            av_frame_free(&frame);
        }

        // ---- false colour: each level its band ---------------------------
        const auto band = [&](int code, size_t expected,
                              const std::string& label) {
            std::vector<float> rgba;
            if (!Render(renderer, code, code, code, VideoScopeMode::FalseColor,
                        rgba)) {
                Check(false, label + ": the monitor renders");
                return;
            }
            const auto shown = Pixel(rgba, kWidth / 2.0, kHeight / 2.0);
            const auto& color = guides.false_colors[expected];
            const double gap = std::max({std::abs(shown[0] - color[0]),
                                         std::abs(shown[1] - color[1]),
                                         std::abs(shown[2] - color[2])});
            Check(gap < 0.01,
                  label + " is painted its band: gap " + std::to_string(gap));
        };
        const auto code = [](float level) {
            return static_cast<int>(std::lround(level * 255.0f));
        };
        band(0, 0, "black");
        band(3, 1, "near black");
        band(code(guides.middle_grey), 2, "middle grey");
        band(code((guides.skin_band[0] + guides.skin_band[1]) / 2.0f), 3,
             "one stop over grey");
        band(250, 4, "near white");
        band(255, 5, "white");
        {
            std::vector<float> rgba;
            Check(Render(renderer, 180, 180, 180, VideoScopeMode::FalseColor,
                         rgba),
                  "a level of no band renders");
            const auto shown = Pixel(rgba, kWidth / 2.0, kHeight / 2.0);
            Check(std::abs(shown[0] - shown[1]) < 1e-3 &&
                      std::abs(shown[0] - 180.0f / 255.0f * 0.8f) < 0.01,
                  "a level of no band is its luma in grey, dimmed: " +
                      std::to_string(shown[0]));
        }
        {
            std::vector<float> rgba;
            Check(
                Render(renderer, 200, 30, 30, VideoScopeMode::FalseColor, rgba),
                "a saturated red renders");
            const auto shown = Pixel(rgba, kWidth / 2.0, kHeight / 2.0);
            Check(std::abs(shown[0] - shown[2]) < 1e-3,
                  "false colour paints by level, never the hue");
        }

        // ---- the vectorscope's marks -------------------------------------
        const Panel panel = ScopePanel();
        const auto at = [&](double u, double v) {
            return std::array<double, 2>{panel.x + u * panel.width,
                                         panel.y + v * panel.height};
        };
        {
            std::vector<float> rgba;
            Check(Render(renderer, 0, 0, 0, VideoScopeMode::Vectorscope, rgba),
                  "the vectorscope renders");
            const char* names[6] = {"red",  "magenta", "blue",
                                    "cyan", "green",   "yellow"};
            for (size_t index = 0; index < 6; ++index) {
                const double u = guides.targets[index][0] + 0.5;
                const double v = 0.5 - guides.targets[index][1];
                // On the box's right edge, and a quarter of the way back
                // to the centre -- clear of the box, the axes and the
                // circle the 75 % targets sit near.
                const auto edge = at(u + 0.02, v);
                const auto outside =
                    at(0.5 + (u - 0.5) * 0.75, 0.5 + (v - 0.5) * 0.75);
                const float lit = Brightest(Pixel(rgba, edge[0], edge[1]));
                const float dark =
                    Brightest(Pixel(rgba, outside[0], outside[1]));
                Check(lit > 0.25f && dark < 0.1f,
                      std::string("the ") + names[index] +
                          " target is drawn where the guides put it: " +
                          std::to_string(lit) + " on, " + std::to_string(dark) +
                          " beside");
            }
            const auto skin = at(0.5 + 0.3 * guides.skin_direction[0],
                                 0.5 - 0.3 * guides.skin_direction[1]);
            const auto across = at(0.5 + 0.3 * guides.skin_direction[0] + 0.04,
                                   0.5 - 0.3 * guides.skin_direction[1]);
            Check(Brightest(Pixel(rgba, skin[0], skin[1])) > 0.25f &&
                      Brightest(Pixel(rgba, across[0], across[1])) < 0.1f,
                  "the skin tone line runs at 123 degrees");
        }

        // ---- the waveform's middle grey ----------------------------------
        {
            std::vector<float> rgba;
            Check(Render(renderer, 0, 0, 0, VideoScopeMode::Waveform, rgba),
                  "the waveform renders");
            // On a dash (the second half of each 48th of the width), at
            // middle grey, and a tenth of the height above.
            const double u = 20.75 / 48.0;
            const auto grey = at(u, 1.0 - guides.middle_grey);
            const auto above = at(u, 1.0 - guides.middle_grey - 0.1);
            Check(Brightest(Pixel(rgba, grey[0], grey[1])) > 0.25f &&
                      Brightest(Pixel(rgba, above[0], above[1])) < 0.1f,
                  "middle grey is marked at " +
                      std::to_string(guides.middle_grey));
        }

        // ---- the RGB waveform: each channel in its own colour -----------
        // A red picture: down one column of the panel, the brightest red
        // sits higher than the brightest green, each nearly alone in its
        // colour, and green and blue share their row.
        {
            std::vector<float> rgba;
            Check(Render(renderer, 200, 30, 30, VideoScopeMode::WaveformRgb,
                         rgba),
                  "the RGB waveform renders");
            const double x = panel.x + panel.width * 0.3;
            double redRow = -1.0, greenRow = -1.0;
            float redPeak = 0.0f, greenPeak = 0.0f;
            for (double y = panel.y; y < panel.y + panel.height; y += 1.0) {
                const auto color = Pixel(rgba, x, y);
                if (color[0] > redPeak) redPeak = color[0], redRow = y;
                if (color[1] > greenPeak) greenPeak = color[1], greenRow = y;
            }
            const auto red = Pixel(rgba, x, redRow);
            const auto green = Pixel(rgba, x, greenRow);
            Check(redPeak > 0.5f && greenPeak > 0.5f && redRow < greenRow &&
                      red[1] < 0.3f && red[2] < 0.3f && green[0] < 0.3f &&
                      std::abs(green[1] - green[2]) < 0.05f,
                  "red traced above green and blue, each in its colour: red "
                  "peak at row " +
                      std::to_string(redRow) + ", green at " +
                      std::to_string(greenRow));
        }

        // ---- PRD37: above the controls along the monitor's bottom --------
        // A picture at 50 % grey: the panel darkens what it covers to its
        // trace, black here except the graticule, and leaves the rest of
        // the picture as it is.
        {
            constexpr double kInset = 56.0;
            std::vector<float> rgba;
            Check(Render(renderer, 128, 128, 128, VideoScopeMode::ParadeRgb,
                         rgba, kInset),
                  "the parade renders above a strip of controls");
            const VideoScopePanel raised =
                LayoutVideoScopePanel(kWidth, kHeight, kInset);
            const Panel bare = ScopePanel();
            // A column inside the red channel, in a row of the old panel's
            // foot that the raised one leaves; and, inside the raised one, a
            // row clear of the trace (at 50 %) and of every graticule line.
            const double x = bare.x + bare.width * 0.1;
            const double strip = kHeight - kInset / 2.0;
            const double inside = raised.y + raised.height * 0.7;
            const float picture = Brightest(Pixel(rgba, x, strip));
            const float panel = Brightest(Pixel(rgba, x, inside));
            Check(raised.y + raised.height <= kHeight - kInset &&
                      std::abs(picture - 128.0f / 255.0f) < 0.02f &&
                      panel < picture - 0.2f,
                  "the panel is drawn above the strip and the strip shows "
                  "the picture: " +
                      std::to_string(picture) + " in the strip, " +
                      std::to_string(panel) + " in the panel");
        }
    }
    if (failures == 0) std::cout << "video_scope_render: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
