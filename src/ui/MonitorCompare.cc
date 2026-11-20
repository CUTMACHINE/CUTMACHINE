#include "MonitorCompare.h"

#include <algorithm>
#include <cmath>

double ClampSplitPosition(double position) {
    if (!std::isfinite(position) || position < 0.0 || position > 1.0)
        return 0.5;
    return position;
}

ReferenceSplit ReferenceSplitFor(int32_t sequenceWidth, int32_t sequenceHeight,
                                 double position) {
    ReferenceSplit split;
    if (sequenceWidth <= 0 || sequenceHeight <= 0) return split;
    const double clamped = ClampSplitPosition(position);
    split.width = static_cast<int32_t>(std::lround(clamped * sequenceWidth));
    split.height = sequenceHeight;
    split.source_width =
        static_cast<double>(split.width) / static_cast<double>(sequenceWidth);
    return split;
}

namespace {

uint8_t Code(float value) {
    return static_cast<uint8_t>(
        std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

ReferenceStill ReferenceStillFromRgba(const std::vector<float>& rgba,
                                      int32_t width, int32_t height) {
    ReferenceStill still;
    if (width <= 0 || height <= 0 ||
        rgba.size() < static_cast<size_t>(width) * height * 4)
        return still;
    still.width = width;
    still.height = height;
    still.rgba.resize(still.ByteSize());
    for (size_t index = 0; index < still.rgba.size(); ++index)
        still.rgba[index] = index % 4 == 3 ? 255 : Code(rgba[index]);
    return still;
}

bool ReferenceFits(const ReferenceStill& still, int32_t sequenceWidth,
                   int32_t sequenceHeight) {
    return still.width > 0 && still.width == sequenceWidth &&
           still.height == sequenceHeight &&
           still.rgba.size() == still.ByteSize();
}
