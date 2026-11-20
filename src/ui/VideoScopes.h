#pragma once

#include "Document.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// UI-2026-08 -- video scopes are a local monitoring preference. They never
// enter Document because changing a diagnostic view must not change the edit.
enum class VideoScopeMode : int32_t {
    Off = 0,
    Waveform = 1,
    ParadeRgb = 2,
    Vectorscope = 3,
    // G5 -- docs/roadmap/GRADE-2026-09.md. The picture itself repainted by
    // what each pixel's level means (VideoScopeGuides below) rather than a
    // panel over it.
    FalseColor = 4,
    // The waveform of each channel drawn in its own colour over the one
    // width, as Resolve's colourised waveform: where R, G and B agree the
    // trace is white, a cast separates it. Appended rather than placed
    // after Waveform so a stored preference keeps its meaning.
    WaveformRgb = 5,
};

VideoScopeMode VideoScopeModeFromPreference(int32_t value);
size_t VideoScopeHistogramBinCount(VideoScopeMode mode);

// DEC42 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The displayed picture's
// RGB distribution, before diagnostic overlays and outside letterboxing.
// The renderer bins the display signal on the GPU; AppKit only draws the
// normalized bins under the Inspector's curves. This is local monitoring
// state, never part of a document.
inline constexpr size_t kVideoHistogramBins = 256;
using VideoHistogram = std::array<std::array<uint32_t, kVideoHistogramBins>, 3>;

// A common scale keeps the RGB channels comparable. An empty picture has
// no histogram, and produces no filled area.
std::array<double, kVideoHistogramBins> NormalizeVideoHistogramChannel(
    const VideoHistogram& histogram, size_t channel);

// PRD37 -- ROADMAP.md. Where Record draws the scope panel, in the drawable's
// pixels, y running down as in the fragment shader: decided here, drawn by
// shader.metal (scopeOverlay), which decides nothing. The panel keeps the
// monitor's bottom left corner it has had since UI-2026-08, but above
// `bottom_inset`, the strip of controls laid over the monitor's bottom edge:
// the scope selector sits there, and the QA of 3 October 2026 saw it cover
// the darkest fifth of a parade. Never taller than the room left above that
// strip; empty for a drawable with no room at all.
struct VideoScopePanel {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// From the drawable's edges, and between the panel and the strip.
inline constexpr double kVideoScopeMarginPixels = 12.0;

VideoScopePanel LayoutVideoScopePanel(double drawable_width,
                                      double drawable_height,
                                      double bottom_inset);

// G5 -- docs/roadmap/GRADE-2026-09.md. What the scopes mark, computed here
// and handed to shader.metal, which only draws: the graticule a colourist
// reads against, in the terms measure_color reports (ColorMeasure.h), so a
// number and a mark never disagree.
//
//  * Vectorscope: the six targets of 75 % colour bars (red, magenta, blue,
//    cyan, green, yellow), at the Cb and Cr the scope bins them at under
//    the output's luma weights, and the skin tone line at
//    kSkinLineDegrees from +Cb towards +Cr, as measure_color's hue.
//  * Waveform and parade: middle grey -- where scene 18 % grey lands on
//    the display signal. Under colour management that is the display
//    transform's answer (the ACES SDR view puts it near 0.40, not where a
//    Rec.709 camera would); without it there is no scene to anchor, and the
//    signal is read as a Rec.709 camera writes it (BT.709 OETF, 0.409).
//  * False colour, the ARRI convention: crushed and clipped as
//    measure_color counts them (a channel at its end code), near black and
//    near white on the display luma, and two bands a sixth of a stop either
//    side of middle grey and of one stop over it, where skin of a light
//    complexion sits when grey is right. Everything else is shown in grey
//    by its luma.
struct VideoScopeGuides {
    // Display luma, 0..1.
    float middle_grey = 0.0f;
    std::array<float, 2> grey_band{};
    std::array<float, 2> skin_band{};
    float near_black = 0.04f;
    float near_white = 0.97f;
    // A channel within this of 0 or 1 is crushed or clipped.
    float end_code = 0.5f / 1023.0f;
    // Cb, Cr of the 75 % bars, in the scope's -0.5..0.5 units.
    std::array<std::array<float, 2>, 6> targets{};
    // The skin tone line's direction: cos and sin of its angle.
    std::array<float, 2> skin_direction{};
    // Crushed, near black, grey, skin, near white, clipped: the ARRI
    // convention's hues, measurement codes rather than interface palette.
    std::array<std::array<float, 3>, 6> false_colors{};
};

// The guides for the monitor's presentation settings (the SDR view Record
// shows, see Renderer.mm).
bool BuildVideoScopeGuides(const ColorManagementSettings& presentation,
                           VideoScopeGuides& guides, std::string& error);

// The BT.709 OETF: a scene reflectance as a Rec.709 camera writes it.
double Rec709CameraSignal(double reflectance);
