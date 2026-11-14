// LG1 -- docs/roadmap/LEGENDES-2026-10.md. Where a carton goes: the rule the
// export, Record and the measuring tools share (CaptionLayout.h), pinned
// without Core Text. The font is described by its measures only, so every
// number below can be worked out by hand.

#include "CaptionLayout.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// A font 100 px tall at the drawn size: 80 up, 20 down, no leading.
caption_layout::FontMetrics Metrics(double scale = 1.0) {
    return {80.0 * scale, 20.0 * scale, 0.0};
}

}  // namespace

int main() {
    using caption_layout::Place;
    using caption_layout::Placement;

    // Sizes authored in sequence pixels follow the drawn width, rounded to
    // the nearest pixel as drawtext was given them.
    Check(caption_layout::DrawnPixels(4, 1080, 1080) == 4, "same size");
    Check(caption_layout::DrawnPixels(4, 540, 1080) == 2, "half size");
    Check(caption_layout::DrawnPixels(3, 540, 1080) == 2, "1.5 rounds up");
    Check(caption_layout::DrawnPixels(0, 540, 1080) == 0, "none stays none");
    Check(Near(caption_layout::FontSize(CaptionStyle{}, 540, 1080), 24.0),
          "the font size scales exactly");

    Check(caption_layout::SplitLines("a\n\nb").size() == 3,
          "an empty line is a line");
    Check(caption_layout::SplitLines("").size() == 1, "no text is one line");

    // Bottom: a twentieth of the height under the block; the lines centred
    // each on its own width.
    {
        CaptionStyle style;
        const Placement placement =
            Place(style, 1000, 2000, 1000, Metrics(), {400.0, 200.0});
        Check(Near(placement.line_height, 100.0), "line height");
        Check(Near(placement.block.height, 200.0), "two lines");
        Check(Near(placement.block.y, 2000.0 - 200.0 - 100.0),
              "bottom margin is h/20: " + std::to_string(placement.block.y));
        Check(Near(placement.block.x, 300.0) &&
                  Near(placement.block.width, 400.0),
              "block centred on its widest line");
        Check(placement.lines.size() == 2 &&
                  Near(placement.lines[1].x, 400.0) &&
                  Near(placement.lines[1].top, 1800.0) &&
                  Near(placement.lines[1].baseline, 1880.0),
              "second line centred, a line height down");
        Check(!placement.box && placement.outline == 0.0 &&
                  placement.shadow == 0.0,
              "no decoration asked, none drawn");
    }
    // Top and centre.
    {
        CaptionStyle style;
        style.position = "top";
        Check(Near(Place(style, 1000, 2000, 1000, Metrics(), {10.0}).block.y,
                   100.0),
              "top margin");
        style.position = "center";
        Check(Near(Place(style, 1000, 2000, 1000, Metrics(), {10.0}).block.y,
                   950.0),
              "centred block");
    }
    // ITW4 -- a custom position places the block's centre, so one line and
    // two stay centred on the same height.
    {
        CaptionStyle style;
        style.vertical_position = {3, 4};
        const Placement one = Place(style, 1000, 2000, 1000, Metrics(), {10.0});
        const Placement two =
            Place(style, 1000, 2000, 1000, Metrics(), {10.0, 10.0});
        Check(Near(one.block.y + (one.block.height / 2.0), 1500.0) &&
                  Near(two.block.y + (two.block.height / 2.0), 1500.0),
              "same centre for one and two lines");
    }
    // The block the style places is the text: the box grows around it by
    // its padding, and does not move it.
    {
        CaptionStyle style;
        style.background_color = "#000000@0.6";
        style.background_padding = 20;
        const Placement boxed =
            Place(style, 1000, 2000, 1000, Metrics(), {400.0});
        CaptionStyle bare;
        const Placement plain =
            Place(bare, 1000, 2000, 1000, Metrics(), {400.0});
        Check(boxed.box && Near(boxed.block.y, plain.block.y),
              "the box does not move the text");
        Check(Near(boxed.box_rect.x, 280.0) && Near(boxed.box_rect.y, 1780.0) &&
                  Near(boxed.box_rect.width, 440.0) &&
                  Near(boxed.box_rect.height, 140.0),
              "padding on every side");
    }
    // Outline and shadow: whole pixels, at least one when asked for.
    {
        CaptionStyle style;
        style.outline_width = 1;
        style.shadow_offset = 3;
        const Placement small = Place(style, 270, 480, 1080, Metrics(), {1.0});
        Check(Near(small.outline, 1.0) && Near(small.shadow, 1.0),
              "a quarter size keeps one pixel of each");
        const Placement large =
            Place(style, 2160, 3840, 1080, Metrics(), {1.0});
        Check(Near(large.outline, 2.0) && Near(large.shadow, 6.0),
              "double size doubles them");
    }
    // fix_bounds: a block pushed out of the picture comes back inside, with
    // its outline and its shadow.
    {
        CaptionStyle style;
        style.vertical_position = {1, 1};
        style.outline_width = 4;
        style.shadow_offset = 2;
        const Placement placement =
            Place(style, 1000, 1000, 1000, Metrics(), {200.0});
        Check(Near(placement.block.y + placement.block.height + 4.0 + 2.0,
                   1000.0),
              "bottom edge back at the picture's: " +
                  std::to_string(placement.block.y));
        style.vertical_position = {0, 1};
        style.position = "top";
        // A line wider than the picture keeps its centre: it cannot fit.
        const Placement wide =
            Place(style, 1000, 1000, 1000, Metrics(), {1200.0});
        Check(Near(wide.block.x, -100.0), "too wide to fit: left centred");
    }
    // What a line measures for wrapping: its advance rounded up, the outline
    // on both sides, the shadow, in sequence pixels.
    {
        CaptionStyle style;
        style.outline_width = 4;
        style.shadow_offset = 2;
        Check(caption_layout::MeasuredWidth(100.2, style) == 111,
              "101 + 8 + 2");
        Check(caption_layout::MeasuredWidth(100.0, CaptionStyle{}) == 100,
              "bare advance");
    }

    return failures == 0 ? 0 : 1;
}
