// G6 -- docs/roadmap/GRADE-2026-09.md. The Record monitor's comparison,
// without a GPU: where the split puts the reference, and a still's planes
// on the way in from the monitor and out to a file and back.
#include "MonitorCompare.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

int main() {
    // ---- the split -------------------------------------------------------
    Check(ClampSplitPosition(0.3) == 0.3 && ClampSplitPosition(0.0) == 0.0 &&
              ClampSplitPosition(1.0) == 1.0,
          "a position in range is kept");
    Check(ClampSplitPosition(-0.1) == 0.5 && ClampSplitPosition(1.5) == 0.5 &&
              ClampSplitPosition(std::nan("")) == 0.5,
          "anything else is the middle");
    const ReferenceSplit half = ReferenceSplitFor(1080, 1920, 0.5);
    Check(half.width == 540 && half.height == 1920 && half.source_width == 0.5,
          "half a portrait frame is 540 columns of it, and half the still");
    const ReferenceSplit third = ReferenceSplitFor(1920, 1080, 1.0 / 3.0);
    Check(third.width == 640 &&
              third.source_width * 1920.0 == static_cast<double>(third.width),
          "the still is sampled on the same whole columns it is drawn on, "
          "neither stretched nor moved");
    Check(ReferenceSplitFor(1920, 1080, 0.0).width == 0,
          "at the left edge the reference has no column");
    Check(ReferenceSplitFor(0, 1080, 0.5).width == 0, "no frame, no split");

    // ---- a still ---------------------------------------------------------
    // Two pixels read back from the monitor: an orange and a translucent
    // blue, alpha made opaque.
    const std::vector<float> rgba = {1.0f, 0.5f, 0.0f, 1.0f,
                                     0.0f, 0.2f, 0.8f, 0.4f};
    const ReferenceStill still = ReferenceStillFromRgba(rgba, 2, 1);
    Check(still.width == 2 && still.rgba.size() == 8 && still.rgba[0] == 255 &&
              still.rgba[1] == 128 && still.rgba[2] == 0 &&
              still.rgba[3] == 255 && still.rgba[5] == 51 &&
              still.rgba[6] == 204 && still.rgba[7] == 255,
          "the monitor's values in 8 bits, alpha opaque");
    Check(ReferenceFits(still, 2, 1) && !ReferenceFits(still, 4, 1) &&
              !ReferenceFits(ReferenceStill{}, 0, 0),
          "a still fits only the sequence size it was grabbed at");
    Check(ReferenceStillFromRgba({0.0f}, 2, 1).width == 0,
          "too few samples make no still");

    if (failures == 0) std::cout << "monitor_compare: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
