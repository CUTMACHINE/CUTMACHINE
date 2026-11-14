#pragma once

// ITW4 -- ROADMAP.md. Where the sequence frame sits in the Record monitor:
// the canvas the cartons, the motion editor's selection and the window
// test's measures are laid on.
//
// It used to place the cartons themselves too -- font size, outline,
// anchor -- for the two AppKit text fields Record drew them with. Since LG5
// (docs/roadmap/LEGENDES-2026-10.md) Record shows the caption renderer's
// picture, which CaptionLayout.h lays out for the monitor and the export
// alike; only the frame is left here. Portable and tested; main.mm only
// turns it into view frames.

#include <cstdint>

namespace ui::caption_overlay {

struct Layout {
    // The sequence frame inside the monitor view, origin top-left, in view
    // points. The renderer fits the sequence aspect ratio and centres it,
    // or, zoomed, draws it at a fixed size, centred too.
    double canvas_x = 0.0;
    double canvas_y = 0.0;
    double canvas_width = 0.0;
    double canvas_height = 0.0;
    // Sequence pixels to view points.
    double scale = 0.0;
};

// `zoomPoints` is a zoomed monitor's view points per sequence pixel (its
// magnification over the backing scale); zero fits the frame to the view.
// Visual QA of 19 September 2026 -- zoomed, the renderer keeps the frame's
// size when the view changes, and a caption fitted to the view grew and
// shrank without it.
inline Layout Compute(int32_t sequenceWidth, int32_t sequenceHeight,
                      double viewWidth, double viewHeight,
                      double zoomPoints = 0.0) {
    Layout layout;
    if (sequenceWidth <= 0 || sequenceHeight <= 0 || viewWidth <= 0.0 ||
        viewHeight <= 0.0)
        return layout;
    const double sequenceAspect =
        static_cast<double>(sequenceWidth) / sequenceHeight;
    layout.canvas_width = viewWidth;
    layout.canvas_height = viewHeight;
    if (zoomPoints > 0.0) {
        layout.canvas_width = sequenceWidth * zoomPoints;
        layout.canvas_height = sequenceHeight * zoomPoints;
    } else if (sequenceAspect > viewWidth / viewHeight) {
        layout.canvas_height = viewWidth / sequenceAspect;
    } else {
        layout.canvas_width = viewHeight * sequenceAspect;
    }
    layout.canvas_x = (viewWidth - layout.canvas_width) / 2.0;
    layout.canvas_y = (viewHeight - layout.canvas_height) / 2.0;
    layout.scale = layout.canvas_height / sequenceHeight;
    return layout;
}

// DEC42 -- docs/roadmap/QA-AGENTIQUE-2026-09.md (case 107a). The size of a
// picture that frames nothing (no ClipFraming), `pictureWidth` x
// `pictureHeight` once upright, in a sequence frame drawn `canvasWidth` x
// `canvasHeight` -- Compute's canvas, zoomed or not: fitted into it, its
// aspect kept, centred by the caller. Zoomed, Record used to draw such a
// picture at its own pixels times the zoom -- the rush, not the sequence --
// while a framed clip and the cartons sat on the frame: a 4K rush in an HD
// timeline covered four times the frame the cartons were laid on. Resolve's
// viewer zooms the timeline's frame; so does Record now (decided on 23
// October 2026). The Source monitor keeps zooming the rush.
struct PictureSize {
    double width = 0.0;
    double height = 0.0;
};

inline PictureSize FitPictureInFrame(double pictureWidth, double pictureHeight,
                                     double canvasWidth, double canvasHeight) {
    PictureSize size;
    if (pictureWidth <= 0.0 || pictureHeight <= 0.0 || canvasWidth <= 0.0 ||
        canvasHeight <= 0.0)
        return size;
    const double scale =
        canvasWidth / pictureWidth < canvasHeight / pictureHeight
            ? canvasWidth / pictureWidth
            : canvasHeight / pictureHeight;
    size.width = pictureWidth * scale;
    size.height = pictureHeight * scale;
    return size;
}

}  // namespace ui::caption_overlay
