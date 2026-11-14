#include "CaptionLayout.h"

#include <algorithm>
#include <cmath>

namespace caption_layout {

int64_t DrawnPixels(int32_t sequencePixels, int32_t width,
                    int32_t sequenceWidth) {
    if (sequencePixels <= 0 || width <= 0 || sequenceWidth <= 0) return 0;
    return ((static_cast<int64_t>(sequencePixels) * width) +
            (sequenceWidth / 2)) /
           sequenceWidth;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            return lines;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

double FontSize(const CaptionStyle& style, int32_t width,
                int32_t sequenceWidth) {
    if (width <= 0 || sequenceWidth <= 0) return 0.0;
    return static_cast<double>(std::max(1, style.font_size)) * width /
           sequenceWidth;
}

Placement Place(const CaptionStyle& style, int32_t width, int32_t height,
                int32_t sequenceWidth, const FontMetrics& metrics,
                const std::vector<double>& advances) {
    Placement placement;
    if (width <= 0 || height <= 0 || sequenceWidth <= 0) return placement;
    placement.scale = static_cast<double>(width) / sequenceWidth;
    placement.font_size = FontSize(style, width, sequenceWidth);
    if (style.outline_width > 0) {
        placement.outline = static_cast<double>(std::max<int64_t>(
            1, DrawnPixels(style.outline_width, width, sequenceWidth)));
    }
    if (style.shadow_offset > 0) {
        placement.shadow = static_cast<double>(std::max<int64_t>(
            1, DrawnPixels(style.shadow_offset, width, sequenceWidth)));
    }
    placement.box = !style.background_color.empty();
    if (placement.box) {
        placement.padding = static_cast<double>(DrawnPixels(
            std::max(0, style.background_padding), width, sequenceWidth));
    }
    placement.line_height = metrics.ascent + metrics.descent + metrics.leading;

    const size_t count = std::max<size_t>(1, advances.size());
    double widest = 0.0;
    for (const double advance : advances) widest = std::max(widest, advance);
    placement.block.width = widest;
    placement.block.height = placement.line_height * static_cast<double>(count);
    placement.block.x = (width - widest) / 2.0;
    const double margin = height / 20.0;
    if (style.vertical_position.num > 0 && style.vertical_position.den > 0) {
        placement.block.y =
            (static_cast<double>(height) * style.vertical_position.num /
             style.vertical_position.den) -
            (placement.block.height / 2.0);
    } else if (style.position == "top") {
        placement.block.y = margin;
    } else if (style.position == "center") {
        placement.block.y = (height - placement.block.height) / 2.0;
    } else {
        placement.block.y = height - placement.block.height - margin;
    }

    // drawtext's fix_bounds: everything the carton paints -- the outline
    // around the text, its shadow, the box -- is moved back inside the
    // picture, along each axis it fits on.
    const Rect& block = placement.block;
    double left = block.x - placement.outline;
    double top = block.y - placement.outline;
    double right = block.x + block.width + placement.outline + placement.shadow;
    double bottom =
        block.y + block.height + placement.outline + placement.shadow;
    if (placement.box) {
        left = std::min(left, block.x - placement.padding);
        top = std::min(top, block.y - placement.padding);
        right = std::max(right, block.x + block.width + placement.padding);
        bottom = std::max(bottom, block.y + block.height + placement.padding);
    }
    const auto inside = [](double low, double high, double limit) {
        if (high - low > limit) return 0.0;
        if (low < 0.0) return -low;
        if (high > limit) return limit - high;
        return 0.0;
    };
    placement.block.x += inside(left, right, width);
    placement.block.y += inside(top, bottom, height);

    if (placement.box) {
        placement.box_rect = {
            placement.block.x - placement.padding,
            placement.block.y - placement.padding,
            placement.block.width + (2.0 * placement.padding),
            placement.block.height + (2.0 * placement.padding)};
    }
    for (size_t index = 0; index < count; ++index) {
        const double advance = index < advances.size() ? advances[index] : 0.0;
        Line line;
        line.width = advance;
        line.x = placement.block.x + ((placement.block.width - advance) / 2.0);
        line.top = placement.block.y +
                   (placement.line_height * static_cast<double>(index));
        line.baseline = line.top + metrics.ascent;
        placement.lines.push_back(line);
    }
    return placement;
}

int64_t MeasuredWidth(double advance, const CaptionStyle& style) {
    return static_cast<int64_t>(std::ceil(std::max(0.0, advance))) +
           (2 * static_cast<int64_t>(std::max(0, style.outline_width))) +
           std::max(0, style.shadow_offset);
}

}  // namespace caption_layout
