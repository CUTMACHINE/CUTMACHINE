// LG1 -- docs/roadmap/LEGENDES-2026-10.md. A carton as Core Text draws it:
// the letters, their outline, their shadow and their box land where
// CaptionLayout.h says, in the colours the style writes, and a family Core
// Text lacks is drawn in the fallback and said to be.
//
// The letters are located by the renderer's own mask (glyphs only), so the
// checks hold whatever the font's design: a pixel well inside a letter is
// the fill, a pixel a few pixels outside it the outline, a pixel where the
// letter would be once moved by the shadow offset the shadow.
//
// CUTMACHINE_CAPTION_RENDER_OUT=<dir> writes the pictures as PNG, to look.

#include "CaptionRender.h"
#include "CaptionRenderer.h"
#include "MotionComposition.h"
#include "MotionRender.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

struct Picture {
    CaptionImage image;
    bool drawn = false;
};

Picture DrawOne(const CaptionDrawing& drawing,
                const std::vector<CaptionCarton>& cartons) {
    Picture picture;
    std::string error;
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    const bool ok = renderer->draw(
        drawing, {cartons},
        [&](size_t, CaptionImage& image) {
            picture.image = std::move(image);
            picture.drawn = true;
            return true;
        },
        error);
    Check(ok && picture.drawn, "draw: " + error);
    return picture;
}

void Save(const Picture& picture, const std::string& name) {
    const char* directory = std::getenv("CUTMACHINE_CAPTION_RENDER_OUT");
    if (directory == nullptr || !picture.drawn) return;
    MotionRaster raster;
    raster.width = picture.image.width;
    raster.height = picture.image.height;
    raster.pixels = picture.image.pixels;
    const std::string path =
        (std::filesystem::path(directory) / (name + ".png")).string();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    MotionError error;
    WriteMotionPng(raster, path, error);
}

struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
};

Rgba At(const CaptionImage& image, int x, int y) {
    const size_t offset =
        4 * ((static_cast<size_t>(y) * static_cast<size_t>(image.width)) +
             static_cast<size_t>(x));
    return {image.pixels[offset], image.pixels[offset + 1],
            image.pixels[offset + 2], image.pixels[offset + 3]};
}

// Chessboard distance of every pixel to the nearest pixel of the letters
// (mask alpha over half).
std::vector<int> Distances(const CaptionImage& mask) {
    const int width = mask.width;
    const int height = mask.height;
    std::vector<int> distance(static_cast<size_t>(width) * height,
                              std::numeric_limits<int>::max());
    std::deque<int> queue;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (At(mask, x, y).a > 127) {
                distance[(y * width) + x] = 0;
                queue.push_back((y * width) + x);
            }
        }
    }
    while (!queue.empty()) {
        const int at = queue.front();
        queue.pop_front();
        const int x = at % width;
        const int y = at / width;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const int next = (ny * width) + nx;
                if (distance[next] > distance[at] + 1) {
                    distance[next] = distance[at] + 1;
                    queue.push_back(next);
                }
            }
        }
    }
    return distance;
}

// Inside a letter: the distance to the outside, the same way.
std::vector<int> Depths(const CaptionImage& mask) {
    CaptionImage inverse = mask;
    for (size_t index = 3; index < inverse.pixels.size(); index += 4)
        inverse.pixels[index] = static_cast<uint8_t>(255 - mask.pixels[index]);
    return Distances(inverse);
}

// MW7 -- the coverage of a picture, in pixels: its alpha summed.
double Ink(const std::vector<uint8_t>& pixels) {
    double ink = 0.0;
    for (size_t index = 3; index < pixels.size(); index += 4) {
        ink += pixels[index] / 255.0;
    }
    return ink;
}

struct MotionLine {
    double ink = 0.0;
    double advance = 0.0;
};

// MW7 -- one line of white motion text on a transparent picture, as the
// motion renderer lays it out and draws it.
MotionLine DrawMotionLine(const std::string& text, const std::string& family,
                          int weight, int size) {
    const std::string json =
        R"({"width": 1000, "height": 600, "fps": 25, "duration": 1,
        "root": {"type": "fill", "children": [{"type": "text", "content": ")" +
        text +
        R"(", "style": {"position": "absolute", "left": 10, "top": 10,
        "fontFamily": ")" +
        family + R"(", "fontWeight": )" + std::to_string(weight) +
        R"(, "fontSize": )" + std::to_string(size) +
        R"(, "color": "#ffffff"}}]}})";
    MotionLine line;
    MotionComposition composition;
    MotionCoreTextMeasurer measurer;
    MotionFrame frame;
    MotionRaster raster;
    MotionError error;
    const bool ok = ParseMotionComposition(json, composition, error) &&
                    EvaluateMotionFrame(composition, mcp_json::Value(), 0,
                                        measurer, frame, error) &&
                    RenderMotionFrame(frame, measurer, raster, error) &&
                    !frame.root.children.empty();
    Check(ok, "motion line in " + family + " at " + std::to_string(weight) +
                  ": " + error.code + " " + error.message);
    if (ok) {
        line.ink = Ink(raster.pixels);
        line.advance = frame.root.children.front().text_layout.width;
    }
    return line;
}

}  // namespace

int main() {
    InstallCaptionRenderer();
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    if (renderer == nullptr) {
        std::cerr << "FAIL: no caption renderer installed\n";
        return 1;
    }

    CaptionDrawing drawing;
    drawing.width = 1000;
    drawing.height = 600;
    drawing.sequence_width = 1000;

    CaptionStyle style;
    style.font_size = 200;
    style.position = "center";
    style.color = "#ffffff";
    style.outline_width = 10;
    style.outline_color = "#ff0000";
    style.shadow_offset = 24;
    style.shadow_color = "#0000ff";
    const CaptionCarton carton{"HIH", style, {1, 1}};

    CaptionDrawing maskDrawing = drawing;
    maskDrawing.glyphs_only = true;
    const Picture mask = DrawOne(maskDrawing, {carton});
    const Picture styled = DrawOne(drawing, {carton});
    Save(mask, "mask");
    Save(styled, "styled");
    if (!mask.drawn || !styled.drawn) return 1;

    Check(styled.image.cartons.size() == 1 &&
              styled.image.cartons[0].family == "Helvetica Neue" &&
              !styled.image.cartons[0].substituted,
          "the system style draws in Helvetica Neue, not a substitute");

    const std::vector<int> outside = Distances(mask.image);
    const std::vector<int> inside = Depths(mask.image);
    const int width = drawing.width;
    int fills = 0;
    int fillsRight = 0;
    int outlines = 0;
    int outlinesRight = 0;
    int shadows = 0;
    int shadowsRight = 0;
    for (int y = 0; y < drawing.height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Rgba pixel = At(styled.image, x, y);
            const int at = (y * width) + x;
            // Deep in a letter: the fill, whatever lies under it.
            if (inside[at] >= 6) {
                ++fills;
                fillsRight += pixel.r == 255 && pixel.g == 255 &&
                              pixel.b == 255 && pixel.a == 255;
            }
            // Three to six pixels out of a letter (under 8.5 px along a
            // diagonal): inside the 10 px outline, which covers the shadow.
            if (outside[at] >= 3 && outside[at] <= 6) {
                ++outlines;
                outlinesRight += pixel.r == 255 && pixel.g == 0 &&
                                 pixel.b == 0 && pixel.a == 255;
            }
            // Out of reach of any letter and its outline, where a letter
            // lies once moved 24 px down and right: the shadow.
            const int sx = x - 24;
            const int sy = y - 24;
            if (outside[at] > 12 && sx >= 0 && sy >= 0 &&
                inside[(sy * width) + sx] >= 6) {
                ++shadows;
                shadowsRight += pixel.r == 0 && pixel.g == 0 &&
                                pixel.b == 255 && pixel.a == 255;
            }
        }
    }
    Check(fills > 500 && fillsRight == fills,
          "fill: " + std::to_string(fillsRight) + "/" + std::to_string(fills));
    Check(outlines > 500 && outlinesRight == outlines,
          "outline outside the letters: " + std::to_string(outlinesRight) +
              "/" + std::to_string(outlines));
    Check(shadows > 200 && shadowsRight == shadows,
          "shadow down and right: " + std::to_string(shadowsRight) + "/" +
              std::to_string(shadows));
    // Nothing outside the letters, the outline and the shadow.
    int stray = 0;
    for (int y = 0; y < drawing.height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int sx = std::max(0, x - 24);
            const int sy = std::max(0, y - 24);
            const int shadowed =
                std::min(outside[(y * width) + x], outside[(sy * width) + sx]);
            if (shadowed > 12 && At(styled.image, x, y).a != 0) ++stray;
        }
    }
    Check(stray == 0, "ink far from the letters: " + std::to_string(stray));

    // The letters sit where the layout says.
    {
        const caption_layout::Placement& placement =
            styled.image.cartons[0].placement;
        int top = drawing.height;
        int bottom = -1;
        int left = width;
        int right = -1;
        for (int y = 0; y < drawing.height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (At(mask.image, x, y).a <= 127) continue;
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
        const caption_layout::Line& line = placement.lines.front();
        Check(bottom <= std::lround(line.baseline) + 1 &&
                  bottom >= std::lround(line.baseline) - 2,
              "capitals stand on the baseline: " + std::to_string(bottom) +
                  " vs " + std::to_string(line.baseline));
        Check(std::abs((left + right + 1) - width) <= 4,
              "the line is centred: " + std::to_string(left) + ".." +
                  std::to_string(right));
        Check(std::fabs((placement.block.y + placement.block.height / 2.0) -
                        drawing.height / 2.0) < 1e-6,
              "the block is centred");
    }

    // The box: the colour all around the text, padding included, under
    // nothing else.
    {
        CaptionStyle boxed;
        boxed.font_size = 100;
        boxed.position = "center";
        boxed.background_color = "#00ff00";
        boxed.background_padding = 30;
        const Picture picture = DrawOne(drawing, {{"Mot", boxed, {1, 1}}});
        Save(picture, "box");
        const caption_layout::Rect& rect =
            picture.image.cartons[0].placement.box_rect;
        const Rgba corner = At(picture.image, static_cast<int>(rect.x) + 2,
                               static_cast<int>(rect.y) + 2);
        const Rgba beyond = At(picture.image, static_cast<int>(rect.x) - 2,
                               static_cast<int>(rect.y) - 2);
        Check(corner.g == 255 && corner.r == 0 && corner.a == 255,
              "box corner is the box colour");
        Check(beyond.a == 0, "nothing outside the box");
        Check(std::fabs(rect.width -
                        (picture.image.cartons[0].placement.block.width +
                         60.0)) < 1e-6,
              "padding on both sides");
    }

    // The clip's opacity multiplies every colour.
    {
        CaptionStyle plain;
        plain.font_size = 200;
        plain.position = "center";
        const Picture half = DrawOne(drawing, {{"HIH", plain, {1, 2}}});
        int deepest = 0;
        int alpha = -1;
        for (size_t at = 0; at < inside.size(); ++at) {
            if (inside[at] > deepest) {
                deepest = inside[at];
                alpha = half.image.pixels[(4 * at) + 3];
            }
        }
        Check(alpha >= 127 && alpha <= 128,
              "half opacity: alpha " + std::to_string(alpha));
    }

    // A family Core Text lacks is drawn in the fallback, and said.
    {
        CaptionStyle missing;
        missing.font_family = "Famille Absente LG1";
        const Picture picture = DrawOne(drawing, {{"Texte", missing, {1, 1}}});
        Check(picture.drawn && picture.image.cartons[0].substituted &&
                  picture.image.cartons[0].family == "Helvetica Neue",
              "missing family falls back to Helvetica Neue");
        Check(!renderer->has_family("Famille Absente LG1") &&
                  renderer->has_family("Helvetica Neue"),
              "has_family");
    }
    // A family Adobe Fonts activates, when this Mac has one: drawn in it,
    // where drawtext drew Verdana.
    if (renderer->has_family("Poppins")) {
        CaptionStyle poppins;
        poppins.font_family = "Poppins";
        const Picture picture = DrawOne(drawing, {{"Texte", poppins, {1, 1}}});
        Check(!picture.image.cartons[0].substituted &&
                  picture.image.cartons[0].face.rfind("Poppins", 0) == 0,
              "Poppins drawn in Poppins: " + picture.image.cartons[0].face);
    } else {
        std::cout << "Poppins not on this Mac: Adobe Fonts check skipped\n";
    }

    // MOTION-WEB-2026-10 MW7 -- ROADMAP.md. A caption in a variable family
    // is drawn at the 400 of its weight axis: a caption style that names no
    // weight takes the regular face, as a motion text at 400 does --
    // not the file's default instance (Outfit's is Thin), nor whichever
    // instance Core Text lists first. Measured against motion texts drawn
    // in the same file at 100, 400 and 900.
    {
        MotionError registered;
        Check(RegisterMotionFontDirectory(MOTION_VARIABLE_FONT_DIR, registered),
              "variable fixture font: " + registered.message);
        CaptionStyle outfit;
        outfit.font_family = "Outfit";
        outfit.font_size = 200;
        outfit.position = "center";
        const Picture letters = DrawOne(maskDrawing, {{"HIH", outfit, {1, 1}}});
        Save(letters, "outfit");
        double ink = 0.0;
        double advance = 0.0;
        if (letters.drawn && !letters.image.cartons.empty()) {
            ink = Ink(letters.image.pixels);
            advance = letters.image.cartons[0].placement.lines[0].width;
            Check(letters.image.cartons[0].family == "Outfit" &&
                      !letters.image.cartons[0].substituted,
                  "Outfit drawn in Outfit: " + letters.image.cartons[0].face);
        }
        const MotionLine thin = DrawMotionLine("HIH", "Outfit", 100, 200);
        const MotionLine regular = DrawMotionLine("HIH", "Outfit", 400, 200);
        const MotionLine black = DrawMotionLine("HIH", "Outfit", 900, 200);
        Check(std::fabs(advance - regular.advance) < 1e-3 * regular.advance,
              "the caption's advance is motion's at 400: " +
                  std::to_string(advance) + " vs " +
                  std::to_string(regular.advance) +
                  " (100: " + std::to_string(thin.advance) +
                  ", 900: " + std::to_string(black.advance) + ")");
        Check(std::fabs(ink - regular.ink) < 0.02 * regular.ink &&
                  thin.ink < 0.8 * ink && black.ink > 1.2 * ink,
              "the caption's ink is motion's at 400: " + std::to_string(ink) +
                  " vs " + std::to_string(regular.ink) +
                  " (100: " + std::to_string(thin.ink) +
                  ", 900: " + std::to_string(black.ink) + ")");

        // Out of lot, 2026-11-04 -- a style that names a weight is drawn at
        // it, as a motion text at that weight is.
        CaptionStyle heavy = outfit;
        heavy.font_weight = 900;
        const Picture bold = DrawOne(maskDrawing, {{"HIH", heavy, {1, 1}}});
        double boldInk = 0.0;
        double boldAdvance = 0.0;
        if (bold.drawn && !bold.image.cartons.empty()) {
            boldInk = Ink(bold.image.pixels);
            boldAdvance = bold.image.cartons[0].placement.lines[0].width;
        }
        Check(std::fabs(boldAdvance - black.advance) < 1e-3 * black.advance &&
                  std::fabs(boldInk - black.ink) < 0.02 * black.ink,
              "a caption at 900 is motion's at 900: advance " +
                  std::to_string(boldAdvance) + " vs " +
                  std::to_string(black.advance) + ", ink " +
                  std::to_string(boldInk) + " vs " + std::to_string(black.ink));
    }
    // The same in a family of static faces: Helvetica Neue's medium face is
    // its own file, taken at 500 as CSS would take it.
    {
        CaptionStyle medium;
        medium.font_family = "Helvetica Neue";
        medium.font_weight = 500;
        const Picture picture = DrawOne(drawing, {{"Texte", medium, {1, 1}}});
        CaptionStyle plain;
        plain.font_family = "Helvetica Neue";
        const Picture regular = DrawOne(drawing, {{"Texte", plain, {1, 1}}});
        Check(picture.drawn && !picture.image.cartons[0].substituted &&
                  picture.image.cartons[0].face == "HelveticaNeue-Medium" &&
                  regular.image.cartons[0].face == "HelveticaNeue",
              "Helvetica Neue at 500 is its medium face: " +
                  picture.image.cartons[0].face + ", at 400 " +
                  regular.image.cartons[0].face);
    }

    // Measuring at the sequence size and drawing at another agree: Core
    // Text scales advances linearly.
    {
        CaptionStyle plain;
        plain.font_size = 60;
        std::vector<double> advances;
        std::string error;
        Check(
            renderer->measure(plain, {"Bonjour à tous", ""}, advances, error) &&
                advances.size() == 2 && advances[0] > 100.0 &&
                advances[1] == 0.0,
            "measure: " + error);
        CaptionDrawing doubled = drawing;
        doubled.width = 2000;
        doubled.height = 1200;
        const Picture picture =
            DrawOne(doubled, {{"Bonjour à tous", plain, {1, 1}}});
        const double drawn = picture.image.cartons[0].placement.lines[0].width;
        Check(!advances.empty() &&
                  std::fabs(drawn - (2.0 * advances[0])) < 0.01 * drawn,
              "double size, double advance: " + std::to_string(drawn) + " vs " +
                  std::to_string(advances.empty() ? 0 : advances[0]));
    }

    // Several cartons in one picture are drawn in the order given, each in
    // its own style.
    {
        CaptionStyle top;
        top.position = "top";
        top.color = "#ff0000";
        CaptionStyle bottom;
        bottom.color = "#00ff00";
        const Picture picture =
            DrawOne(drawing, {{"Haut", top, {1, 1}}, {"Bas", bottom, {1, 1}}});
        Check(picture.image.cartons.size() == 2 &&
                  picture.image.cartons[0].placement.block.y <
                      picture.image.cartons[1].placement.block.y,
              "two cartons, each placed by its style");
    }

    // PA4 -- docs/roadmap/PARITE-2026-11.md. A carton whose words are
    // animated: the word being said in its own colour, or the words drawn
    // one after the other, the line standing where it stands full.
    {
        CaptionStyle plain;
        plain.font_size = 160;
        plain.position = "center";
        plain.color = "#ffffff";
        plain.outline_width = 6;
        plain.outline_color = "#ff0000";
        CaptionStyle lit = plain;
        lit.word_animation = kCaptionWordAnimationHighlight;
        lit.highlight_color = "#00ff00";
        CaptionStyle revealed = plain;
        revealed.word_animation = kCaptionWordAnimationReveal;
        const std::string text = "H H H";
        const auto draw = [&](const CaptionStyle& style, int32_t spoken) {
            CaptionCarton carton{text, style, {1, 1}};
            carton.words_spoken = spoken;
            return DrawOne(drawing, {carton});
        };
        CaptionDrawing lettersOnly = drawing;
        lettersOnly.glyphs_only = true;
        const Picture letters = DrawOne(lettersOnly, {{text, plain, {1, 1}}});
        const Picture whole = draw(plain, -1);
        Save(whole, "words-whole");
        // The three words, as the columns their letters ink.
        std::vector<std::pair<int, int>> words;
        bool inked = false;
        for (int x = 0; x < drawing.width; ++x) {
            bool column = false;
            for (int y = 0; y < drawing.height && !column; ++y)
                column = At(letters.image, x, y).a > 0;
            if (column && !inked) words.emplace_back(x, x);
            if (column) words.back().second = x;
            inked = column;
        }
        Check(words.size() == 3,
              "three words inked apart: " + std::to_string(words.size()));
        if (words.size() == 3) {
            const std::vector<int> depth = Depths(letters.image);
            const auto same = [&](const Picture& a, const Picture& b, int from,
                                  int until) {
                for (int y = 0; y < drawing.height; ++y) {
                    for (int x = from; x < until; ++x) {
                        const Rgba p = At(a.image, x, y);
                        const Rgba q = At(b.image, x, y);
                        if (p.r != q.r || p.g != q.g || p.b != q.b ||
                            p.a != q.a)
                            return false;
                    }
                }
                return true;
            };
            // How many pixels deep in word `word` are `r`,`g`,`b`, of how
            // many.
            const auto deep = [&](const Picture& picture, size_t word, int r,
                                  int g, int b) {
                int right = 0;
                int all = 0;
                for (int y = 0; y < drawing.height; ++y) {
                    for (int x = words[word].first; x <= words[word].second;
                         ++x) {
                        if (depth[(y * drawing.width) + x] < 4) continue;
                        const Rgba pixel = At(picture.image, x, y);
                        ++all;
                        right += pixel.r == r && pixel.g == g && pixel.b == b &&
                                 pixel.a == 255;
                    }
                }
                return std::make_pair(right, all);
            };
            const auto all = [](const std::pair<int, int>& count) {
                return count.second > 200 && count.first == count.second;
            };

            // A carton that is not timed is drawn whole under any style.
            Check(same(draw(lit, -1), whole, 0, drawing.width) &&
                      same(draw(revealed, -1), whole, 0, drawing.width),
                  "an untimed carton is drawn whole whatever its style");
            // Highlight: nothing lit before the first word, then the last
            // word started, and only its letters change.
            Check(same(draw(lit, 0), whole, 0, drawing.width),
                  "before the first word, nothing is lit");
            const Picture second = draw(lit, 2);
            Save(second, "words-highlight-2");
            Check(all(deep(second, 0, 255, 255, 255)) &&
                      all(deep(second, 1, 0, 255, 0)) &&
                      all(deep(second, 2, 255, 255, 255)),
                  "the second word is lit, the others keep the fill");
            Check(same(second, whole, 0, words[1].first - 1) &&
                      same(second, whole, words[1].second + 2, drawing.width),
                  "the words beside it are the whole carton's, pixel for "
                  "pixel");
            bool outlineKept = true;
            for (int y = 0; y < drawing.height; ++y) {
                for (int x = words[1].first; x <= words[1].second; ++x) {
                    if (At(letters.image, x, y).a != 0) continue;
                    const Rgba p = At(second.image, x, y);
                    const Rgba q = At(whole.image, x, y);
                    outlineKept = outlineKept && p.r == q.r && p.g == q.g &&
                                  p.b == q.b && p.a == q.a;
                }
            }
            Check(outlineKept, "around the lit word the outline is unchanged");
            Check(all(deep(draw(lit, 3), 2, 0, 255, 0)),
                  "the last word stays lit to the carton's end");
            Check(second.image.cartons[0].placement.lines[0].x ==
                      whole.image.cartons[0].placement.lines[0].x,
                  "a lit carton stands where the whole one does");

            // Reveal: only the words started, each where the whole puts it.
            const Picture none = draw(revealed, 0);
            bool empty = true;
            for (int y = 0; y < drawing.height && empty; ++y)
                for (int x = 0; x < drawing.width && empty; ++x)
                    empty = At(none.image, x, y).a == 0;
            Check(empty, "before the first word, no letter is drawn");
            const Picture one = draw(revealed, 1);
            Save(one, "words-reveal-1");
            const int outline = 6;
            Check(same(one, whole, 0, words[0].second + outline + 1),
                  "the first word is the whole carton's, pixel for pixel");
            bool rest = true;
            for (int y = 0; y < drawing.height && rest; ++y)
                for (int x = words[1].first - outline; x < drawing.width; ++x)
                    rest = rest && At(one.image, x, y).a == 0;
            Check(rest, "the words not yet said are not drawn");
            Check(one.image.cartons[0].placement.lines[0].x ==
                          whole.image.cartons[0].placement.lines[0].x &&
                      one.image.cartons[0].placement.block.width ==
                          whole.image.cartons[0].placement.block.width,
                  "a carton that fills stands where it stands full");
            Check(same(draw(revealed, 3), whole, 0, drawing.width),
                  "every word said, the carton is the whole one");
        }
        // Two lines: the count runs on from one line to the next.
        const std::string lines = "H H\nH";
        CaptionCarton upper{lines, revealed, {1, 1}};
        upper.words_spoken = 2;
        CaptionCarton full{lines, revealed, {1, 1}};
        full.words_spoken = 3;
        const Picture two = DrawOne(drawing, {upper});
        const Picture three = DrawOne(drawing, {full});
        const double secondTop = three.image.cartons[0].placement.lines[1].top;
        bool lower = false;
        bool lowerFull = false;
        for (int y = static_cast<int>(secondTop) + 12; y < drawing.height;
             ++y) {
            for (int x = 0; x < drawing.width; ++x) {
                lower = lower || At(two.image, x, y).a != 0;
                lowerFull = lowerFull || At(three.image, x, y).a != 0;
            }
        }
        Check(!lower && lowerFull,
              "the third word, on the second line, waits for its turn");
    }

    return failures == 0 ? 0 : 1;
}
