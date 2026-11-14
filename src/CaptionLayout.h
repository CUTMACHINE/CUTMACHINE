#pragma once

// LG1 -- docs/roadmap/LEGENDES-2026-10.md. Where a carton goes in a picture,
// from its style and the measures of its font: the one rule the export, the
// Record monitor and the measuring tools draw by.
//
// A caption used to be laid out by three engines: FFmpeg's drawtext in the
// file, two AppKit text fields in Record, drawtext again on a black frame to
// measure a line. They disagreed on the outline (AppKit strokes both sides
// of the glyph edge), on the box (Record placed the box, the file the text)
// and on the font (fontconfig does not see Adobe Fonts). The rule is now
// written once, here, portable and tested; Core Text only measures the lines
// and traces the glyphs where this says (CaptionRender.mm).
//
// The meaning of a style is the one the export always gave it, drawtext's:
// sizes are authored in sequence pixels and follow the drawn width; the
// lines are centred, each on its own width; a line is as tall as the font's
// ascent, descent and leading; `position` keeps a twentieth of the height as
// margin, `vertical_position` places the centre of the block; the outline
// lies outside the glyph, the shadow is the glyph and its outline moved down
// and right, the box surrounds the text with its padding on every side; the
// block the style places is the text, not the box; and the whole is moved
// back inside the picture when it would leave it.

#include "Document.h"

#include <cstdint>
#include <string>
#include <vector>

namespace caption_layout {

// Of the font as drawn, in pixels, all positive: the descent goes down.
struct FontMetrics {
    double ascent = 0.0;
    double descent = 0.0;
    double leading = 0.0;
};

// One line, origin at the picture's top left. `x` is where its pen starts,
// `baseline` the height its glyphs stand on.
struct Line {
    double x = 0.0;
    double top = 0.0;
    double width = 0.0;
    double baseline = 0.0;
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct Placement {
    // Drawn pixels per sequence pixel.
    double scale = 0.0;
    double font_size = 0.0;
    // Drawn pixels, zero when the style has none. The outline and the
    // shadow are whole pixels, at least one when the style asks for any,
    // as drawtext drew them.
    double outline = 0.0;
    double shadow = 0.0;
    double padding = 0.0;
    double line_height = 0.0;
    // The text block, every line included.
    Rect block;
    bool box = false;
    Rect box_rect;
    std::vector<Line> lines;
};

// A size the style authors in sequence pixels, drawn `width` pixels wide:
// rounded to the nearest pixel, as drawtext was given it.
int64_t DrawnPixels(int32_t sequencePixels, int32_t width,
                    int32_t sequenceWidth);

// The text's lines, as delivered: one per '\n', an empty one kept.
std::vector<std::string> SplitLines(const std::string& text);

// `advances` are the lines' widths, in drawn pixels, at the font size this
// returns in `font_size` (the style's, scaled): one per line of SplitLines.
// `metrics` are the font's at that size.
Placement Place(const CaptionStyle& style, int32_t width, int32_t height,
                int32_t sequenceWidth, const FontMetrics& metrics,
                const std::vector<double>& advances);

// The font size a style draws at in a picture `width` pixels wide.
double FontSize(const CaptionStyle& style, int32_t width,
                int32_t sequenceWidth);

// The width a line takes in sequence pixels, decoration included: its
// advance at the style's own size, the outline on both sides, the shadow.
// What WrapCaptionLines compares with the limit, and what generate_captions
// publishes as rendered_width.
int64_t MeasuredWidth(double advance, const CaptionStyle& style);

}  // namespace caption_layout
