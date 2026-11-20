#pragma once

// What the monitor and the timeline draw, as a display list: rectangles,
// glyph runs and icons of the atlas (MetalRect, MetalText, MetalIcon), and
// the picture's grading, framing, masks and veils. Split out of Renderer.h,
// which imports AppKit, so that the list can be built in portable C++ and
// tested without a window (TimelineDrawing.h); Renderer.mm only draws it.

#include "ClipFraming.h"
#include "ColorEffects.h"
#include "Document.h"
#include "LookLut.h"
#include "MonitorCompare.h"
#include "UiTheme.h"
#include "VideoScopes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct MetalRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
    float bottom_red = 0.0f;
    float bottom_green = 0.0f;
    float bottom_blue = 0.0f;
    float bottom_alpha = 1.0f;

    MetalRect() = default;
    MetalRect(double itemX, double itemY, double itemWidth, double itemHeight,
              float itemRed, float itemGreen, float itemBlue,
              float itemAlpha = 1.0f)
        : x(itemX),
          y(itemY),
          width(itemWidth),
          height(itemHeight),
          red(itemRed),
          green(itemGreen),
          blue(itemBlue),
          alpha(itemAlpha),
          bottom_red(itemRed),
          bottom_green(itemGreen),
          bottom_blue(itemBlue),
          bottom_alpha(itemAlpha) {}
    MetalRect(double itemX, double itemY, double itemWidth, double itemHeight,
              const ui::theme::Color& topColor,
              const ui::theme::Color& bottomColor)
        : x(itemX),
          y(itemY),
          width(itemWidth),
          height(itemHeight),
          red(topColor.r),
          green(topColor.g),
          blue(topColor.b),
          alpha(topColor.a),
          bottom_red(bottomColor.r),
          bottom_green(bottomColor.g),
          bottom_blue(bottomColor.b),
          bottom_alpha(bottomColor.a) {}
};

enum class MetalFontFace { Sans, Mono };

struct MetalText {
    double x = 0.0;
    double y = 0.0;
    double max_width = 0.0;
    double point_size = 11.0;
    std::string text;
    ui::theme::Color color = ui::theme::kTextPrimary;
    MetalFontFace face = MetalFontFace::Mono;
    bool bold = false;
};

struct MetalIcon {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::string name;
    ui::theme::Color color = ui::theme::kTextPrimary;
};

using MetalDrawCommand = std::variant<MetalRect, MetalText, MetalIcon>;

// PA8 -- docs/roadmap/PARITE-2026-11.md. A clip's mask as the renderer is
// handed it: DocumentClip::mask's fractions turned into floats once, the
// shape as shader.metal counts it (0 none, 1 rectangle, 2 ellipse; PA15,
// 3 the people, whose coverage is the matte the slot is handed).
// PA15 -- docs/roadmap/PARITE-2026-11.md. The matte of the source frame a
// slot shows when its clip is cut out along its people (PersonMatte.h):
// 8-bit grey, rows packed, covering the whole displayed picture. The bytes
// belong to the caller and live until the render returns. None (null) for
// a slot whose mask is of shape 3 draws nothing of that clip: its matte is
// missing, and the export refuses it too.
struct MonitorMatte {
    const uint8_t* bytes = nullptr;
    int32_t width = 0;
    int32_t height = 0;
};

struct MonitorMask {
    int32_t shape = 0;
    float center_x = 0.5f;
    float center_y = 0.5f;
    float width = 1.0f;
    float height = 1.0f;
    float feather = 0.0f;
    bool inverted = false;
};

inline MonitorMask MonitorMaskOf(const ClipMask& mask) {
    MonitorMask out;
    if (mask.shape.empty()) return out;
    const auto fraction = [](const EffectParamValue& value) {
        return value.den > 0 ? static_cast<float>(value.num) /
                                   static_cast<float>(value.den)
                             : 0.0f;
    };
    out.shape = mask.shape == kClipMaskPerson    ? 3
                : mask.shape == kClipMaskEllipse ? 2
                                                 : 1;
    out.center_x = fraction(mask.center_x);
    out.center_y = fraction(mask.center_y);
    out.width = fraction(mask.width);
    out.height = fraction(mask.height);
    out.feather = fraction(mask.feather);
    out.inverted = mask.inverted;
    return out;
}

// R1 -- docs/roadmap/REEL-2026-09.md. The rectangle ClipFraming.h resolved,
// in the form the renderer draws: the part of the *displayed* picture to
// sample, as fractions of it, and the rectangle to draw it in, in sequence
// pixels. Fractions rather than pixels because the monitor may be decoding
// a proxy, which is the same picture at another size.
//
// This is the single step to floating point the lot's guard rail 2 allows,
// and it is at the render, in the one place the GPU is handed numbers. The
// rectangle itself was decided in exact integers.
struct MonitorFraming {
    float source_left = 0.0f;
    float source_top = 0.0f;
    float source_width = 1.0f;
    float source_height = 1.0f;
    int32_t x = 0;
    int32_t y = 0;
    // Zero means this frame carries no framing: the renderer then fits the
    // picture to the viewer the way it always has, byte for byte.
    int32_t width = 0;
    int32_t height = 0;
};

inline MonitorFraming MonitorFramingOf(const ClipFramingGeometry& geometry,
                                       int64_t displayedWidth,
                                       int64_t displayedHeight) {
    MonitorFraming framing;
    if (displayedWidth <= 0 || displayedHeight <= 0 || geometry.width <= 0 ||
        geometry.height <= 0)
        return framing;
    framing.source_left = static_cast<float>(
        static_cast<double>(geometry.source_x) / displayedWidth);
    framing.source_top = static_cast<float>(
        static_cast<double>(geometry.source_y) / displayedHeight);
    framing.source_width = static_cast<float>(
        static_cast<double>(geometry.source_width) / displayedWidth);
    framing.source_height = static_cast<float>(
        static_cast<double>(geometry.source_height) / displayedHeight);
    framing.x = static_cast<int32_t>(geometry.x);
    framing.y = static_cast<int32_t>(geometry.y);
    framing.width = static_cast<int32_t>(geometry.width);
    framing.height = static_cast<int32_t>(geometry.height);
    return framing;
}

// R2 -- docs/roadmap/REEL-2026-09.md. An animated clip as the export draws
// it: the constant crop scaled onto the whole picture's rectangle, which
// the frame then cuts (Export.cc cannot re-crop per frame, `crop` takes its
// size once). The drawn rectangle is the same as above; what differs is
// which part of the source lands in it. MonitorFramingOf reads the preimage
// ClipFraming.h puts on the even grid a subsampled crop needs, and on a
// zoomed picture that snap moved Record a whole pixel away from the file
// (monitor_export_parity_tests, 2.5x zoom: 0.4 source pixel). Here the
// preimage is the exact one the export's scale produces.
inline MonitorFraming MonitorFramingOfWholePicture(
    const ClipFramingGeometry& geometry, int64_t displayedWidth,
    int64_t displayedHeight) {
    MonitorFraming framing =
        MonitorFramingOf(geometry, displayedWidth, displayedHeight);
    if (framing.width <= 0 || geometry.full_width <= 0 ||
        geometry.full_height <= 0)
        return framing;
    const double perPixelX =
        static_cast<double>(geometry.crop_width) / geometry.full_width;
    const double perPixelY =
        static_cast<double>(geometry.crop_height) / geometry.full_height;
    framing.source_left = static_cast<float>(
        (geometry.crop_x + ((geometry.x - geometry.full_x) * perPixelX)) /
        displayedWidth);
    framing.source_top = static_cast<float>(
        (geometry.crop_y + ((geometry.y - geometry.full_y) * perPixelY)) /
        displayedHeight);
    framing.source_width =
        static_cast<float>((geometry.width * perPixelX) / displayedWidth);
    framing.source_height =
        static_cast<float>((geometry.height * perPixelY) / displayedHeight);
    return framing;
}

// R5 -- docs/roadmap/REEL-2026-09.md. The flat colour a "dip_to_color" of
// the layer's own track lays over it, already in the renderer's terms: the
// sRGB channels of the document's #rrggbb, and the exact fraction
// TransitionRender.h computed turned into a float once. Renderer.mm never
// works out a fade curve; it paints what it is handed, over the picture
// rectangle it has just drawn, so the veil covers the shot and not the
// letterbox around it.
struct MonitorVeil {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;
};

struct TimelineRenderData {
    double video_height = 0.0;
    // Zero fits the image to the viewer. Positive values are native-pixel
    // magnifications: 1.0 = 100%, 2.0 = 200%.
    double video_zoom = 0.0;
    // DEC42 -- docs/roadmap/QA-AGENTIQUE-2026-09.md (case 107a). Record's
    // zoom magnifies the sequence frame for every clip, framed or not
    // (CaptionOverlayLayout.h, FitPictureInFrame); the Source monitor,
    // false, keeps magnifying the rush's own pixels.
    bool video_zoom_frames_sequence = false;
    int32_t sequence_width = 1920;
    int32_t sequence_height = 1080;
    ColorManagementSettings color_management;
    // Local monitor policy only. The document and export keep their requested
    // output transform; this switches the drawable to a Rec.709 SDR preview.
    bool display_sdr_preview = false;
    VideoScopeMode video_scope = VideoScopeMode::Off;
    // DEC42 -- read back only the small RGB distribution, when the curve
    // editor is visible. No second render or full-frame CPU copy.
    bool curve_histogram = false;
    // PRD37 -- ROADMAP.md. The drawable's pixels along its bottom edge that
    // the monitor's controls cover: the scope panel is laid out above them
    // (VideoScopes.h, LayoutVideoScopePanel). Zero for a view without any,
    // the panel then drawn where it always was.
    double video_scope_bottom_inset = 0.0;
    std::vector<int32_t> video_rotation_degrees;
    std::vector<float> video_opacities;
    // Parallel to the frames passed to RenderFrames: the resolved color.*
    // grade stack (F1.3, see ColorEffects.h) for the clip each frame came
    // from. A slot beyond this vector's size, or a default-constructed
    // (count == 0) entry, renders with no grading applied. Renderer.mm is a
    // read-only consumer here -- it never looks at DocumentClip/ClipEffect
    // itself, only this already-resolved, already-float form.
    std::vector<ResolvedColorGrade> video_color_grades;
    // MOTION-2026-09 M4 -- parallel to the frames too: non-zero for a motion
    // frame, display-referred sRGB, which reaches the working space through
    // the inverse of the display view rather than the camera's input
    // transform (ColorManagement.h). Beyond its size, a camera frame.
    std::vector<uint8_t> video_display_referred;
    // G3 -- docs/roadmap/GRADE-2026-09.md. Parallel to the frames too: the
    // colour input each frame's media declares (Document::ColorManagementFor
    // resolves it), read in place of the project's. Beyond this vector's
    // size, or undeclared, the frame follows the project, exactly as before
    // this ticket.
    std::vector<MediaColorInput> video_color_inputs;
    // G6 -- docs/roadmap/GRADE-2026-09.md. The reference still, drawn as it
    // is over the left `reference_position` of the frame by the output pass
    // (MonitorCompare.h); null for none, which is every monitor that shows
    // none, drawn as it was. Shared rather than copied: the renderer keeps
    // it for as long as it keeps its texture.
    std::shared_ptr<const ReferenceStill> reference_still;
    double reference_position = 0.5;
    // R1 -- parallel to the frames too: where each one's clip put its
    // picture in the sequence frame (ClipFraming.h, through
    // MonitorFramingOf above). A slot beyond this vector's size, or one
    // whose width is zero, is a clip that frames nothing and is fitted to
    // the viewer exactly as before this ticket. Renderer.mm never resolves
    // a framing itself: it is handed the rectangle, like the grades above.
    std::vector<MonitorFraming> video_framings;
    // R5 -- parallel to the frames too: the colour dip over each one, and
    // nothing for a slot beyond this vector's size or one whose alpha is
    // zero, which is every frame of every project that dips nowhere.
    std::vector<MonitorVeil> video_veils;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. Parallel to the frames too:
    // each one's Gaussian blur, its sigma in sequence pixels
    // (DocumentClip::blur turned into a float once). Zero, or a slot beyond
    // this vector's size, is a sharp layer, drawn exactly as before this
    // ticket. A blurred layer is drawn alone into a texture the size of its
    // rectangle, blurred there with its edges extended, then graded and laid
    // with its opacity: the picture the export blurs with `gblur`
    // (ClipBlur.h), within a gap tests/monitor_export_parity_tests.mm bounds.
    std::vector<float> video_blurs;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. Parallel to the frames too:
    // each one's mask (MonitorMaskOf above). A slot beyond this vector's
    // size, or one without a shape, is a whole picture, drawn exactly as
    // before this ticket. The mask scales the layer's coverage where its
    // opacity does, in the fragment that lays it (`clipMaskCoverage`,
    // shader.metal), on the rule ClipMask.h gives the export.
    std::vector<MonitorMask> video_masks;
    // PA15 -- parallel to the frames as well: the matte of each slot whose
    // mask is of shape 3 (MonitorMatte above), sampled where the shape's
    // rule would be evaluated.
    std::vector<MonitorMatte> video_mattes;
    // R6 -- docs/roadmap/REEL-2026-09.md. The sequence's look LUT, already
    // read from the project package and checked against its digest
    // (LookLutStorage.h): the renderer resolves nothing itself, as it
    // resolves neither a framing nor a fade curve. A zero `size` is a
    // timeline without a look, drawn exactly as it was before this ticket.
    // The digest is the upload's cache key -- the samples cross to the GPU
    // when the look changes, not every frame.
    LookLutSamples look_lut;
    std::string look_lut_digest;
    // Where the look is laid, as LoadSequenceLookLut resolved it: on the
    // working log values, or on the delivered signal of a Rec.709 SDR
    // delivery. The renderer applies the stage it is handed.
    LookLutStage look_lut_stage = LookLutStage::None;
    // Ordered overlay commands. Text and icons stay in the same display list
    // as rectangles so the playhead and selection strokes preserve their
    // exact z-order instead of being forced into a second overlay pass.
    std::vector<MetalDrawCommand> overlays;
};

// CF1 -- the same composite on a second display, fitted in full, with the
// viewer's inspection aids removed. Colour and the clip's framing remain
// those of Record. Captions are drawn by its separate CaptionRenderer.
inline TimelineRenderData CleanFeedRenderData(TimelineRenderData data,
                                              double height) {
    data.video_height = height;
    data.video_zoom = 0.0;
    data.video_scope = VideoScopeMode::Off;
    data.curve_histogram = false;
    data.video_scope_bottom_inset = 0.0;
    data.reference_still.reset();
    data.overlays.clear();
    return data;
}
