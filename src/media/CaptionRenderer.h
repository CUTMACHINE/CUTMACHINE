#pragma once

// LG1 -- docs/roadmap/LEGENDES-2026-10.md. How the model reaches the one
// drawing of a carton. Measuring a line and tracing its glyphs need Core
// Text, which lives in cutmachine_motion_render, above the model; the export,
// the measuring tools and the Record monitor live below or beside it. The
// executable installs the renderer at startup (InstallCaptionRenderer,
// CaptionRender.h), as it installs the motion renderer
// (MotionFrameRenderer.h), and a surface that finds none says so.

#include "CaptionLayout.h"
#include "Document.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One carton to draw: its text as delivered (long lines already broken,
// WrapCaptionTextsForDelivery), its style, its clip's opacity.
struct CaptionCarton {
    std::string text;
    CaptionStyle style;
    EffectParamValue opacity{1, 1};
    // PA4 -- docs/roadmap/PARITE-2026-11.md. How many of the text's words
    // have started at the instant drawn (CaptionWordsSpoken), for a carton
    // whose words are timed and whose style animates them; -1 draws the
    // carton whole, whatever the style says. Under "highlight" the last
    // word started is drawn in the style's highlight colour; under
    // "reveal" only the words started are drawn, each where the whole
    // carton puts it, so a line never moves as it fills.
    int32_t words_spoken = -1;
};

struct CaptionDrawing {
    // The picture drawn, in pixels: a delivered frame, or the frame as the
    // Record monitor shows it.
    int32_t width = 0;
    int32_t height = 0;
    // The width the styles are authored at: the sequence's.
    int32_t sequence_width = 0;
    // LG4 -- the fill of the letters alone, opaque white, without outline,
    // shadow or box: where the letters are.
    bool glyphs_only = false;
};

// How one carton was drawn.
struct CaptionCartonLayout {
    caption_layout::Placement placement;
    // The family the glyphs were drawn in: the style's, or the fallback
    // when Core Text does not have it (decided on 8 October 2026: said, not
    // refused).
    std::string family;
    // The PostScript name of the face drawn.
    std::string face;
    bool substituted = false;
};

struct CaptionImage {
    int32_t width = 0;
    int32_t height = 0;
    // Premultiplied RGBA, 8 bits per channel, rows from the top. The values
    // are the style's colours as written, with no colour space conversion:
    // a caption colour is a code value of the delivered signal.
    std::vector<uint8_t> pixels;
    // One per carton, in drawing order.
    std::vector<CaptionCartonLayout> cartons;
};

// The family a carton falls back to when Core Text lacks its own: the one
// CaptionFontName gives the "system" style.
inline constexpr const char* kCaptionFallbackFamily = "Helvetica Neue";

struct CaptionRenderer {
    // The advance of each line of `lines` at the style's own size, in
    // sequence pixels, without decoration (caption_layout::MeasuredWidth
    // adds it).
    bool (*measure)(const CaptionStyle& style,
                    const std::vector<std::string>& lines,
                    std::vector<double>& advances,
                    std::string& error) = nullptr;
    // Draws each entry of `images` -- the cartons one picture shows, in
    // drawing order -- and hands it to `sink` with its index, one after the
    // other, so a whole film's pictures are never held at once. The fonts
    // are resolved once for the call. A false sink stops the drawing.
    bool (*draw)(const CaptionDrawing& drawing,
                 const std::vector<std::vector<CaptionCarton>>& images,
                 const std::function<bool(size_t, CaptionImage&)>& sink,
                 std::string& error) = nullptr;
    // Whether Core Text draws `family` itself rather than the fallback.
    bool (*has_family)(const std::string& family) = nullptr;
    // LG3 -- a picture as the PNG FFmpeg reads (ImageIO, as a motion frame
    // is written): straight alpha, the colours unchanged.
    bool (*encode_png)(const CaptionImage& image, std::string& png,
                       std::string& error) = nullptr;
};

namespace caption_renderer {
inline std::atomic<const CaptionRenderer*>& Slot() {
    static std::atomic<const CaptionRenderer*> renderer{nullptr};
    return renderer;
}
}  // namespace caption_renderer

inline void SetCaptionRenderer(const CaptionRenderer* renderer) {
    caption_renderer::Slot().store(renderer);
}

// Null when this process has no renderer (a test that links the model
// alone).
inline const CaptionRenderer* CurrentCaptionRenderer() {
    return caption_renderer::Slot().load();
}

inline constexpr const char* kNoCaptionRenderer =
    "this process has no caption renderer (Core Text): only the macOS "
    "cutmachine executable draws and measures captions, not a test binary "
    "nor the build without a window (headless/)";
