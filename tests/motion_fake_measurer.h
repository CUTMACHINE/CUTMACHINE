#pragma once

// REMOTION-PARITY-2026-09 -- the fake metrics of the portable motion suites:
// every glyph half as wide as the font size, lines 1.2 times as tall, the
// baseline at 0.8. What makes a box computable by hand in a test.

#include "MotionEvaluate.h"

#include <sstream>
#include <string>
#include <vector>

class FakeMotionMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override {
        if (style.font_family == "Missing Sans") {
            error = {"FontMissing", "", "font 'Missing Sans' is not installed"};
            return false;
        }
        const double lineHeight =
            style.line_height > 0.0 ? style.line_height : style.font_size * 1.2;
        const double advance = style.font_size * 0.5 + style.letter_spacing;
        layout = MotionTextLayout{};
        std::stringstream stream(text);
        std::string line;
        std::vector<std::pair<std::string, double>> lines;
        std::vector<std::vector<MotionTextCluster>> clusters;
        while (std::getline(stream, line, '\n')) {
            clusters.emplace_back();
            double x = 0.0;
            for (size_t index = 0; index < line.size(); ++index) {
                if ((static_cast<unsigned char>(line[index]) & 0xC0) == 0x80) {
                    clusters.back().back().length += 1;
                    continue;
                }
                clusters.back().push_back({index, 1, x, advance});
                x += advance;
            }
            lines.emplace_back(line, x);
            layout.width = std::max(layout.width, x);
        }
        const double frame = max_width >= 0.0 ? max_width : layout.width;
        for (size_t index = 0; index < lines.size(); ++index) {
            MotionTextLine placed;
            placed.text = lines[index].first;
            placed.clusters = clusters[index];
            placed.width = lines[index].second;
            placed.height = lineHeight;
            placed.y = lineHeight * static_cast<double>(index);
            placed.x = style.text_align == "center"
                           ? (frame - placed.width) / 2.0
                       : style.text_align == "right" ? frame - placed.width
                                                     : 0.0;
            placed.baseline = placed.y + style.font_size * 0.8;
            layout.lines.push_back(placed);
        }
        layout.height = lineHeight * static_cast<double>(lines.size());
        return true;
    }

    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override {
        if (source != "logo.png") {
            error = {"MediaMissing", "", "no media '" + source + "'"};
            return false;
        }
        width = 400.0;
        height = 200.0;
        return true;
    }
};
