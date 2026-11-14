#pragma once

// MOTION-2026-09 M6 -- ROADMAP.md. The macOS half of a motion video: every
// frame evaluated, drawn by Core Graphics and handed to FFmpeg as a PNG
// sequence, one file of the batch at a time. MotionVideo.h plans the batch
// and the encoding; this only draws what the plan asks for.

#include "MotionVideo.h"

#include <string>

// Plans the batch (PlanMotionVideos), then evaluates every frame of every
// render before the first is encoded, so a missing font or a refused text
// stops the batch while nothing is written. With `preview`, stops there and
// reports what would be written. Otherwise renders each file in turn,
// through a temporary PNG sequence that is removed after it, and
// Exporter::Run. A failure then keeps the files already complete and lists
// them in `report`, with error.code ExportFailed.
bool RenderMotionVideos(const MotionVideoRequest& request, std::string& report,
                        MotionError& error);
