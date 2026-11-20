#pragma once

// MOTION-DESIGN-2026-09 -- the frames of one composition rendered on every
// core. A motion-blurred frame is several renders of the composition, and
// raster effects are computed on the CPU: rendered one after the other, a
// 36-second film with blur took minutes more than it needed. Each worker
// asks the installed renderer (MotionFrameRenderer.h) for its own frames,
// with its own copy of the request; the renderer builds its own measurer
// and bitmap per call, and the media readers are shared under their lock.

#include "MotionFrameRenderer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Renders each of `frames` (distinct composition frames) and writes its PNG
// to pathFor(frame). Stops at the first refusal, or when `cancel` turns
// true, and says which frame failed.
bool RenderMotionFramesToFiles(
    const MotionFrameRequest& base, const std::vector<int64_t>& frames,
    const std::function<std::string(int64_t)>& pathFor,
    const std::atomic_bool* cancel, std::string& error);
