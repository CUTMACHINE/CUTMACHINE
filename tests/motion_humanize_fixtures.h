#pragma once

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. What the humanize suites
// build their compositions from.

#include "MotionEvaluate.h"

#include <cstdint>
#include <string>
#include <vector>

// A row of digit strips as the v6 generator wrote one: HH:MM:SS:FF of the
// frames from `first`, one column per character, each `width` wide; a
// digit column a strip of the ten digits stepped every frame. `text` is the
// style its texts carry.
inline std::string TimecodeRow(int64_t first, int64_t frames, double width,
                               int64_t broken = -1,
                               const std::string& text =
                                   R"("style": {"fontFamily": "Fake",
                                      "fontSize": 20, "lineHeight": 1.0})") {
    std::vector<std::string> shown;
    for (int64_t frame = 0; frame < frames; ++frame) {
        shown.push_back(MotionTimecodeText(first + frame, 25, 1));
    }
    const std::string w = std::to_string(width);
    std::string columns;
    for (size_t position = 0; position < 11; ++position) {
        if (!columns.empty()) {
            columns += ",";
        }
        const char first_char = shown[0][position];
        if (first_char < '0' || first_char > '9') {
            columns +=
                R"({"type": "box", "style": {"height": 20, "width": )" + w +
                R"(, "flexShrink": 0}, "children": [{"type": "text", )" + text +
                R"(, "content": ")" + std::string(1, first_char) + R"("}]})";
            continue;
        }
        std::string keys = "[";
        std::string values = "[";
        for (int64_t frame = 0; frame < frames; ++frame) {
            int digit = shown[static_cast<size_t>(frame)][position] - '0';
            if (frame == broken && position == 10) {
                digit = (digit + 1) % 10;
            }
            keys += (frame > 0 ? "," : "") + std::to_string(frame);
            values += (frame > 0 ? "," : "") + std::to_string(-digit * 20);
        }
        keys += "]";
        values += "]";
        std::string digits;
        for (int digit = 0; digit < 10; ++digit) {
            digits +=
                (digit > 0 ? "," : "") +
                std::string(
                    R"({"type": "box", "style": {"height": 20, "flexShrink": 0}, "children": [{"type": "text", )") +
                text + R"(, "content": ")" + std::to_string(digit) + R"("}]})";
        }
        columns +=
            R"({"type": "box", "style": {"height": 20, "width": )" + w +
            R"(, "overflow": "hidden", "flexShrink": 0, "flexDirection": "column"},
               "children": [{"type": "box", "style": {"flexDirection": "column",
               "flexShrink": 0, "height": 200, "alignSelf": "flex-start"},
               "animate": {"translateY": {"interpolate": {"frames": )" +
            keys + R"(, "values": )" + values +
            R"(, "easing": "step-end"}}}, "children": [)" + digits + "]}]}";
    }
    return R"({"type": "box", "style": {"position": "absolute", "left": 30,
               "top": 150, "height": 20}, "children": [)" +
           columns + "]}";
}
