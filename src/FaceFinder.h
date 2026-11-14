#pragma once

// AGENT-2026-09 A20 -- docs/roadmap/AGENT-2026-09.md. Where the faces are in
// a picture, for a grade that reads the skin rather than the frame.
//
// A colourist exposes and balances an interview on the face: the median of
// the frame measures the walls, and the two runs of the 25th showed it --
// one delivered at +1.9 stops was a little bright, the same scene at +0.5
// a little dark, the face somewhere in between both times. The engine can
// find the face itself: on macOS, Vision's face rectangles (FaceFinderVision
// .mm), installed at startup like the motion frame renderer
// (MotionFrameRenderer.h). The model (this header, ColorMeasure.cc) only
// knows the slot, so a test installs a finder of its own and a process
// without one says so instead of guessing a region.
//
// The finder reads the display-referred picture measure_color measures,
// 8 bits per channel, and returns each face as a region of that picture,
// largest first. Nothing here is an operation, and nothing here judges
// whose face it is.

#include "ColorMeasure.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

using FaceFinder = bool (*)(int32_t width, int32_t height,
                            const std::vector<uint8_t>& rgb8,
                            std::vector<ColorMeasureRegion>& faces,
                            std::string& error);

namespace face_finder {
inline std::atomic<FaceFinder>& Slot() {
    static std::atomic<FaceFinder> finder{nullptr};
    return finder;
}
}  // namespace face_finder

inline void SetFaceFinder(FaceFinder finder) {
    face_finder::Slot().store(finder);
}

// Null when this process has no finder (a test that links the model alone).
inline FaceFinder CurrentFaceFinder() { return face_finder::Slot().load(); }

// Installs Vision's face rectangles as the finder (FaceFinderVision.mm).
void InstallFaceFinder();
