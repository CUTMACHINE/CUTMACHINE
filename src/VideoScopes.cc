#include "VideoScopes.h"

#include "ColorManagement.h"
#include "ColorMeasure.h"

#include <algorithm>
#include <cmath>

VideoScopeMode VideoScopeModeFromPreference(int32_t value) {
    if (value < static_cast<int32_t>(VideoScopeMode::Off) ||
        value > static_cast<int32_t>(VideoScopeMode::WaveformRgb))
        return VideoScopeMode::Off;
    return static_cast<VideoScopeMode>(value);
}

size_t VideoScopeHistogramBinCount(VideoScopeMode mode) {
    switch (mode) {
        case VideoScopeMode::Waveform:
            return 256u * 128u;
        case VideoScopeMode::ParadeRgb:
            return 3u * 128u * 128u;
        case VideoScopeMode::WaveformRgb:
            return 3u * 256u * 128u;
        case VideoScopeMode::Vectorscope:
            return 256u * 256u;
        case VideoScopeMode::Off:
        case VideoScopeMode::FalseColor:
            return 1u;
    }
    return 1u;
}

std::array<double, kVideoHistogramBins> NormalizeVideoHistogramChannel(
    const VideoHistogram& histogram, size_t channel) {
    std::array<double, kVideoHistogramBins> levels{};
    if (channel >= histogram.size()) {
        return levels;
    }
    uint32_t peak = 0;
    for (const auto& bins : histogram) {
        for (const uint32_t count : bins) {
            peak = std::max(peak, count);
        }
    }
    if (peak == 0) {
        return levels;
    }
    for (size_t bin = 0; bin < levels.size(); ++bin) {
        levels[bin] = static_cast<double>(histogram[channel][bin]) / peak;
    }
    return levels;
}

VideoScopePanel LayoutVideoScopePanel(double drawable_width,
                                      double drawable_height,
                                      double bottom_inset) {
    VideoScopePanel panel;
    if (!std::isfinite(drawable_width) || !std::isfinite(drawable_height) ||
        drawable_width <= 0.0 || drawable_height <= 0.0)
        return panel;
    const double inset = std::isfinite(bottom_inset)
                             ? std::clamp(bottom_inset, 0.0, drawable_height)
                             : 0.0;
    // The proportions shader.metal has drawn since UI-2026-08: up to 42 %
    // of the width and 520 pixels, 38 % of the height, a 0.62 aspect.
    const double room =
        drawable_height - inset - (2.0 * kVideoScopeMarginPixels);
    const double width = std::min(drawable_width * 0.42, 520.0);
    const double height =
        std::min({drawable_height * 0.38, width * 0.62, room});
    if (width <= 0.0 || height <= 0.0) return panel;
    panel.x = kVideoScopeMarginPixels;
    panel.y = drawable_height - inset - kVideoScopeMarginPixels - height;
    panel.width = width;
    panel.height = height;
    return panel;
}

double Rec709CameraSignal(double reflectance) {
    if (reflectance < 0.018) return 4.5 * reflectance;
    return 1.099 * std::pow(reflectance, 0.45) - 0.099;
}

namespace {

constexpr double kMiddleGrey = 0.18;
// A sixth of a stop either side: the width of ARRI's grey band (38-42 %)
// around a Rec.709 camera's 41 %.
constexpr double kBandStops = 1.0 / 6.0;

}  // namespace

bool BuildVideoScopeGuides(const ColorManagementSettings& presentation,
                           VideoScopeGuides& guides, std::string& error) {
    guides = VideoScopeGuides{};
    error.clear();
    const bool rec2020 =
        presentation.enabled && presentation.output_gamut == "rec2020";
    // The luma weights the scope kernel bins with (shader.metal).
    const std::array<double, 3> weights =
        rec2020 ? std::array<double, 3>{0.2627, 0.6780, 0.0593}
                : std::array<double, 3>{0.2126, 0.7152, 0.0722};
    const auto luma = [&](double red, double green, double blue) {
        return weights[0] * red + weights[1] * green + weights[2] * blue;
    };

    // Scene reflectance to display luma, as the monitor renders it.
    const auto displayed = [&](double reflectance, float& out) {
        if (!presentation.enabled) {
            out = static_cast<float>(Rec709CameraSignal(reflectance));
            return true;
        }
        RgbColor display;
        if (!OpenColorIoDisplayColor(presentation,
                                     {reflectance, reflectance, reflectance},
                                     display, error))
            return false;
        out =
            static_cast<float>(luma(display.red, display.green, display.blue));
        return true;
    };
    const double low = std::exp2(-kBandStops);
    const double high = std::exp2(kBandStops);
    if (!displayed(kMiddleGrey, guides.middle_grey) ||
        !displayed(kMiddleGrey * low, guides.grey_band[0]) ||
        !displayed(kMiddleGrey * high, guides.grey_band[1]) ||
        !displayed(2.0 * kMiddleGrey * low, guides.skin_band[0]) ||
        !displayed(2.0 * kMiddleGrey * high, guides.skin_band[1])) {
        error = "unable to place middle grey on the display: " + error;
        return false;
    }

    // Red, magenta, blue, cyan, green, yellow at 75 %, clockwise from red as
    // a vectorscope's graticule names them.
    const std::array<std::array<double, 3>, 6> bars = {{{0.75, 0.0, 0.0},
                                                        {0.75, 0.0, 0.75},
                                                        {0.0, 0.0, 0.75},
                                                        {0.0, 0.75, 0.75},
                                                        {0.0, 0.75, 0.0},
                                                        {0.75, 0.75, 0.0}}};
    for (size_t index = 0; index < bars.size(); ++index) {
        const auto& bar = bars[index];
        const double y = luma(bar[0], bar[1], bar[2]);
        guides.targets[index] = {
            static_cast<float>((bar[2] - y) / (2.0 * (1.0 - weights[2]))),
            static_cast<float>((bar[0] - y) / (2.0 * (1.0 - weights[0])))};
    }
    const double skin = static_cast<double>(kSkinLineDegrees) * M_PI / 180.0;
    guides.skin_direction = {static_cast<float>(std::cos(skin)),
                             static_cast<float>(std::sin(skin))};
    guides.false_colors = {{
        {0.55f, 0.18f, 0.75f},  // Crushed: purple.
        {0.15f, 0.35f, 0.95f},  // Near black: blue.
        {0.20f, 0.80f, 0.25f},  // Middle grey: green.
        {0.95f, 0.50f, 0.70f},  // One stop over: pink.
        {0.95f, 0.90f, 0.15f},  // Near white: yellow.
        {0.95f, 0.12f, 0.12f},  // Clipped: red.
    }};
    return true;
}
