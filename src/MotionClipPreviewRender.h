#pragma once

// MOTION-2026-09 M4 -- ROADMAP.md. The macOS half of the monitor's motion
// pictures: the frame a MotionPreviewRequest names, drawn by Core Text and
// Core Graphics as the export draws it (the same evaluation, through
// ResolveMotionVariant with no version or format, then MotionRender.h), and
// laid out for Renderer.mm (MotionClipPreview.h). Kept in the motion
// library, beside the renderer the export reaches through
// MotionFrameRenderer.h, so that no framework enters the model.

#include "MotionClipPreview.h"
#include "MotionComposition.h"

#include <string>

// REMOTION-PARITY-2026-09 REM2 -- `document` gives what the composition
// reaches: the project's media and the compositions its instances show.
bool RenderMotionPreview(const MotionPreviewRequest& request,
                         const Document& document,
                         const std::string& mediaDirectory,
                         MotionPreviewImage& image, MotionError& error);
