// REMOTION-PARITY-2026-09 REM3 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// Paint, masks, effects and transitions drawn by the real painter: the
// corpus scenes C15 (a fade then a slide), C16 (an animated gradient), C17
// (a shadow, a blur, a bounded glow) and C18 (a circle then a polygon
// revealing a picture), plus a mask gradient and the italic face. Pixels
// are read where the descriptor says something is, and where it says
// nothing may be. macOS only: Core Text and Core Graphics draw.

#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionGeometry.h"
#include "MotionRender.h"
#include "Ulid.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

const std::string kFonts = MOTION_FONT_DIR;
const std::string kParity = MOTION_PARITY_FIXTURE_DIR;

std::string Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

MotionComposition Parse(const std::string& json, const std::string& label) {
    MotionComposition composition;
    MotionError error;
    Check(ParseMotionComposition(json, composition, error),
          label + " parses: " + error.code + " at " + error.path + " -- " +
              error.message);
    return composition;
}

bool Render(const MotionComposition& composition, int64_t frame,
            const std::string& directory, MotionRaster& raster,
            MotionFrame& evaluated, const std::string& label) {
    MotionCoreTextMeasurer measurer(directory);
    MotionError error;
    const bool ok = EvaluateMotionFrame(composition, mcp_json::Value(), frame,
                                        measurer, evaluated, error) &&
                    RenderMotionFrame(evaluated, measurer, raster, error);
    Check(ok, label + ": " + error.code + " at " + error.path + " -- " +
                  error.message);
    return ok;
}

// Straight colour of a premultiplied pixel, and its alpha, 0..255.
struct Pixel {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 0.0;
};

Pixel At(const MotionRaster& raster, int x, int y) {
    const uint8_t* rgba =
        &raster.pixels[(static_cast<size_t>(y) * raster.width + x) * 4];
    Pixel pixel;
    pixel.alpha = rgba[3];
    if (rgba[3] > 0) {
        pixel.red = rgba[0] * 255.0 / rgba[3];
        pixel.green = rgba[1] * 255.0 / rgba[3];
        pixel.blue = rgba[2] * 255.0 / rgba[3];
    }
    return pixel;
}

bool Is(const Pixel& pixel, double red, double green, double blue,
        double tolerance, double alpha = 255.0) {
    return std::abs(pixel.red - red) <= tolerance &&
           std::abs(pixel.green - green) <= tolerance &&
           std::abs(pixel.blue - blue) <= tolerance &&
           std::abs(pixel.alpha - alpha) <= tolerance;
}

std::string Text(const Pixel& pixel) {
    std::ostringstream out;
    out << "(" << pixel.red << ", " << pixel.green << ", " << pixel.blue << ", "
        << pixel.alpha << ")";
    return out.str();
}

const MotionEvaluatedNode* FindId(const MotionEvaluatedNode& node,
                                  const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = FindId(child, id)) {
            return found;
        }
    }
    return nullptr;
}

// C15 -- the fade mixes the two scenes without a dip; the slide shows each
// scene where the descriptor moved it.
void TestC15Painted() {
    const MotionComposition composition =
        Parse(Read(kParity + "/c15-fade-slide.json"), "C15");
    struct Expected {
        int64_t frame;
        int x;
        double red;
        double green;
        double blue;
        const char* what;
    };
    for (const Expected& expected : {
             Expected{19, 20, 228, 87, 46, "the first scene alone"},
             Expected{25, 20, 144.5, 88.5, 150, "half of each scene"},
             Expected{29, 20, 77.7, 89.7, 233.2, "a tenth of the first"},
             Expected{30, 20, 61, 90, 254, "the second scene alone"},
             Expected{45, 40, 61, 90, 254, "the second, pushed left"},
             Expected{45, 280, 77, 171, 161, "the third, half way in"},
             Expected{69, 20, 77, 171, 161, "the third scene alone"},
         }) {
        MotionRaster raster;
        MotionFrame frame;
        if (!Render(composition, expected.frame, std::string(), raster, frame,
                    "C15 frame " + std::to_string(expected.frame))) {
            continue;
        }
        const Pixel pixel = At(raster, expected.x, 20);
        Check(Is(pixel, expected.red, expected.green, expected.blue, 1.5),
              "C15 frame " + std::to_string(expected.frame) + " shows " +
                  expected.what + ", opaque: " + Text(pixel));
    }
}

// C16 -- the gradient across the frame, then turned; a radial spot fading
// to transparent over it.
void TestC16Painted() {
    const MotionComposition composition =
        Parse(Read(kParity + "/c16-animated-gradient.json"), "C16");
    MotionRaster at0;
    MotionFrame frame;
    if (!Render(composition, 0, std::string(), at0, frame, "C16 frame 0")) {
        return;
    }
    Check(Is(At(at0, 0, 170), 228, 87, 46, 3) &&
              Is(At(at0, 319, 170), 61, 90, 254, 3) &&
              Is(At(at0, 160, 170), 144.5, 88.5, 150, 3),
          "C16 paints the gradient from its first stop to its last: " +
              Text(At(at0, 0, 170)) + " " + Text(At(at0, 160, 170)) + " " +
              Text(At(at0, 319, 170)));
    // Pixel (50, 30) is sampled at (50.5, 30.5), 0.71 px from the spot's
    // centre: 3.5 % of the way to transparent, over the page's gradient at
    // x = 50.5.
    const double spot = 1.0 - std::hypot(0.5, 0.5) / 20.0;
    const double along = 50.5 / 320.0;
    const double page[3] = {228 + (61 - 228) * along, 87 + (90 - 87) * along,
                            46 + (254 - 46) * along};
    Check(Is(At(at0, 50, 30), 255 * spot + page[0] * (1 - spot),
             255 * spot + page[1] * (1 - spot),
             255 * spot + page[2] * (1 - spot), 4),
          "C16 paints the radial spot white at its centre, over the page: " +
              Text(At(at0, 50, 30)));
    MotionRaster at20;
    if (Render(composition, 20, std::string(), at20, frame, "C16 frame 20")) {
        Check(Is(At(at20, 300, 0), 77, 171, 161, 3) &&
                  Is(At(at20, 300, 179), 255, 210, 63, 3),
              "C16 ends on the vertical gradient: " + Text(At(at20, 300, 0)) +
                  " " + Text(At(at20, 300, 179)));
    }
}

// C17 -- the effects write inside their domain and nowhere else; the glow
// is the colour it names; the same frame twice is the same bytes.
void TestC17Painted() {
    const MotionComposition composition =
        Parse(Read(kParity + "/c17-shadow-blur-glow.json"), "C17");
    MotionRaster raster;
    MotionRaster again;
    MotionFrame frame;
    MotionFrame second;
    if (!Render(composition, 20, std::string(), raster, frame,
                "C17 frame 20") ||
        !Render(composition, 20, std::string(), again, second, "C17 again")) {
        return;
    }
    Check(raster.pixels == again.pixels,
          "C17 renders its effects to the same bytes twice");
    const MotionEvaluatedNode* title = FindId(frame.root, "title");
    if (title == nullptr || !title->has_filter_domain) {
        Check(false, "C17 has a title with a domain");
        return;
    }
    const MotionBox& domain = title->filter_domain;
    bool outsideClear = true;
    int inside = 0;
    for (int y = 0; y < raster.height; ++y) {
        for (int x = 0; x < raster.width; ++x) {
            const bool within =
                x >= domain.x - 1 && x <= domain.x + domain.width + 1 &&
                y >= domain.y - 1 && y <= domain.y + domain.height + 1;
            const double alpha = At(raster, x, y).alpha;
            if (!within && alpha != 0.0) {
                outsideClear = false;
            }
            inside += within && alpha > 0.0 ? 1 : 0;
        }
    }
    Check(outsideClear && inside > 0,
          "C17 writes pixels inside its domain only");
    // Just left of the title's box: the glow, yellow over nothing.
    const Pixel halo =
        At(raster, static_cast<int>(title->box.x) - 4,
           static_cast<int>(title->box.y + title->box.height / 2.0));
    Check(halo.alpha > 20 && halo.red > 200 && halo.green > 170 &&
              halo.blue < 120,
          "C17 lays a yellow glow around the title: " + Text(halo));
    // PRD5 -- docs/roadmap/PRODUIT-2026-10.md. Frame 0 is evaluated into a
    // frame of its own: `title` points into `frame`, and evaluating into
    // `frame` again frees the nodes it points to. The halo is read where the
    // title sits at frame 20, as before.
    MotionRaster at0;
    MotionFrame bareFrame;
    if (Render(composition, 0, std::string(), at0, bareFrame, "C17 frame 0")) {
        const Pixel bare =
            At(at0, static_cast<int>(title->box.x) - 4,
               static_cast<int>(title->box.y + title->box.height / 2.0));
        Check(bare.alpha < 20,
              "C17 has no glow at frame 0, where it is 0 wide: " + Text(bare));
    }
}

// C18 -- the picture shows inside the circle, then inside the polygon.
void TestC18Painted(const std::filesystem::path& directory) {
    const MotionComposition composition =
        Parse(Read(kParity + "/c18-circle-polygon-mask.json"), "C18");
    struct Expected {
        int64_t frame;
        bool shown;
    };
    for (const Expected& expected : {Expected{0, false}, Expected{20, true},
                                     Expected{35, false}, Expected{45, true}}) {
        MotionRaster raster;
        MotionFrame frame;
        if (!Render(composition, expected.frame, directory.string(), raster,
                    frame, "C18 frame " + std::to_string(expected.frame))) {
            continue;
        }
        // (550, 225): the red half of the picture, 150 px right of the
        // circle's centre, right of the polygon's edge at frame 35.
        const Pixel pixel = At(raster, 550, 225);
        Check(expected.shown ? Is(pixel, 255, 0, 0, 1) : pixel.alpha == 0.0,
              "C18 frame " + std::to_string(expected.frame) +
                  (expected.shown ? " shows" : " hides") +
                  " the picture there: " + Text(pixel));
    }
}

// REMOTION-PARITY-2026-09 REM4 -- C19: the card drawn through its
// projection. Points are placed with the descriptor's quad: inside it the
// card's colour, well past its far edge nothing, below it the shadow its
// own plane casts.
void TestC19Painted() {
    const MotionComposition composition =
        Parse(Read(kParity + "/c19-perspective-card.json"), "C19");
    MotionRaster raster;
    MotionRaster again;
    MotionFrame frame;
    MotionFrame second;
    if (!Render(composition, 20, std::string(), raster, frame,
                "C19 frame 20") ||
        !Render(composition, 20, std::string(), again, second, "C19 again")) {
        return;
    }
    Check(raster.pixels == again.pixels, "C19 renders the same bytes twice");
    const MotionEvaluatedNode* card = FindId(frame.root, "card");
    if (card == nullptr || !card->has_quad) {
        Check(false, "C19 has a card with a quad");
        return;
    }
    // A point of the quad by its parameters: u across, v down.
    const auto inside = [&](double u, double v, double& x, double& y) {
        const auto& q = card->quad;
        const double topX = q[0].first + (q[1].first - q[0].first) * u;
        const double topY = q[0].second + (q[1].second - q[0].second) * u;
        const double bottomX = q[3].first + (q[2].first - q[3].first) * u;
        const double bottomY = q[3].second + (q[2].second - q[3].second) * u;
        x = topX + (bottomX - topX) * v;
        y = topY + (bottomY - topY) * v;
    };
    double x = 0.0;
    double y = 0.0;
    inside(0.1, 0.5, x, y);
    const Pixel face = At(raster, static_cast<int>(x), static_cast<int>(y));
    Check(Is(face, 77, 171, 161, 3),
          "C19 shows the card's colour inside its projected corners: " +
              Text(face));
    const double farX =
        std::max(card->quad[1].first, card->quad[2].first) + 40.0;
    const double midY = (card->quad[1].second + card->quad[2].second) / 2.0;
    const Pixel beyond =
        At(raster, static_cast<int>(farX), static_cast<int>(midY));
    Check(beyond.alpha == 0.0,
          "C19 paints nothing well past the card's far edge: " + Text(beyond));
    inside(0.3, 1.0, x, y);
    const Pixel shadow =
        At(raster, static_cast<int>(x), static_cast<int>(y) + 8);
    Check(shadow.alpha > 20 && shadow.red < 40 && shadow.green < 40,
          "C19 casts its shadow below the card, in its own plane: " +
              Text(shadow));
    // PRD5 -- its own frame too: `card` and the `inside` closure still point
    // into `frame`. Nothing reads them past this line today; evaluating into
    // `frame` here would leave them dangling for whoever adds a check below.
    MotionRaster flat;
    MotionFrame flatFrame;
    if (Render(composition, 0, std::string(), flat, flatFrame, "C19 frame 0")) {
        Check(flat.pixels != raster.pixels,
              "C19 moves between frame 0 and frame 20");
    }
}

// A mask gradient: coverage falls from left to right, nothing outside the
// box.
void TestMaskPainted() {
    const MotionComposition composition = Parse(
        R"J({"width":120,"height":40,"fps":25,"duration":1,"root":{"type":"fill",)J"
        R"J("children":[{"type":"box","style":{"position":"absolute","left":10,)J"
        R"J("top":10,"width":100,"height":20,"backgroundColor":"#ff0000",)J"
        R"J("maskImage":"linear-gradient(to right, #000 0%, transparent 100%)"}}]}})J",
        "mask");
    MotionRaster raster;
    MotionFrame frame;
    if (!Render(composition, 0, std::string(), raster, frame, "mask")) {
        return;
    }
    const double left = At(raster, 12, 20).alpha;
    const double middle = At(raster, 60, 20).alpha;
    const double right = At(raster, 107, 20).alpha;
    Check(left > 230 && std::abs(middle - 127.5) < 6 && right < 20 &&
              At(raster, 60, 20).red > 250,
          "a mask gradient fades the box from left to right: " +
              std::to_string(left) + " " + std::to_string(middle) + " " +
              std::to_string(right));
}

// The italic of a family, or FontMissing: never a slanted roman.
void TestItalic() {
    MotionCoreTextMeasurer measurer;
    MotionTextStyle style;
    style.font_family = "Helvetica Neue";
    style.font_size = 40;
    std::string upright;
    std::string italic;
    MotionError error;
    Check(measurer.ResolveFontName(style, upright, error),
          "Helvetica Neue resolves: " + error.message);
    style.italic = true;
    Check(measurer.ResolveFontName(style, italic, error) &&
              italic.find("Italic") != std::string::npos && italic != upright,
          "fontStyle italic takes the family's italic face: " + italic);
    // FI4 -- ROADMAP.md. This build-generated family has only one upright
    // face; an italic installed by the user must not change the oracle.
    Check(RegisterMotionFontDirectory(MOTION_UPRIGHT_FONT_DIR, error),
          "the private upright font registers: " + error.message);
    style.font_family = "CUTMACHINE Test Upright";
    style.font_weight = 600;
    style.italic = false;
    Check(measurer.ResolveFontName(style, upright, error) &&
              upright == "CUTMACHINETestUpright",
          "the private family resolves its real upright face: " + upright);
    style.italic = true;
    MotionTextLayout layout;
    Check(
        !measurer.LayoutText("Titre", style, -1.0, layout, error) &&
            error.code == "FontMissing" &&
            error.message.find("no italic face") != std::string::npos,
        "a family without an italic is refused, not slanted: " + error.message);
    MotionComposition composition = Parse(
        R"({"width":200,"height":60,"fps":25,"duration":1,"root":{"type":"fill",)"
        R"("children":[{"type":"text","content":"Titre","style":{)"
        R"("fontFamily":"Helvetica Neue","fontStyle":"italic","fontSize":40}}]}})",
        "italic");
    MotionFrame frame;
    MotionRaster raster;
    if (Render(composition, 0, std::string(), raster, frame, "italic")) {
        Check(DumpMotionFrame(frame).find("\"fontStyle\":\"italic\"") !=
                  std::string::npos,
              "the descriptor says a text is italic");
    }
}

// MOTION-MARQUE-2026-10 MM2 -- the figures a font shows, applied by Core
// Text (OpenType lnum, onum, tnum, pnum) to the layout and to the drawing
// alike, inherited as CSS inherits it. Baskerville's default figures are
// lining; old-style ones are narrower. Georgia has only one kind.
void TestNumericVariant() {
    MotionComposition refused;
    MotionError error;
    Check(!ParseMotionComposition(
              R"({"width":200,"height":60,"fps":25,"duration":1,)"
              R"("root":{"type":"text","content":"1",)"
              R"("style":{"fontVariantNumeric":"lining-nums oldstyle-nums"}}})",
              refused, error) &&
              error.code == "InvalidValue",
          "lining and old-style figures at once are refused: " + error.code);
    Check(!ParseMotionComposition(
              R"({"width":200,"height":60,"fps":25,"duration":1,)"
              R"("root":{"type":"text","content":"1",)"
              R"("style":{"fontVariantNumeric":"slashed-zero"}}})",
              refused, error),
          "a variant the engine does not apply is refused");

    const auto width = [](const std::string& variant, MotionFrame& frame) {
        const std::string inherited =
            variant.empty() ? std::string()
                            : R"(,"fontVariantNumeric":")" + variant + "\"";
        MotionComposition composition = Parse(
            R"({"width":800,"height":120,"fps":25,"duration":1,)"
            R"("root":{"type":"fill","style":{"fontFamily":"Baskerville",)"
            R"("fontSize":60)" +
                inherited +
                R"(},"children":[{"type":"text","content":"1 point 2031"}]}})",
            "figures " + variant);
        MotionRaster raster;
        if (!Render(composition, 0, std::string(), raster, frame,
                    "figures " + variant)) {
            return -1.0;
        }
        return frame.root.children.empty() ||
                       frame.root.children[0].text_layout.lines.empty()
                   ? -1.0
                   : frame.root.children[0].text_layout.lines[0].width;
    };
    MotionFrame plain;
    MotionFrame oldstyle;
    const double plainWidth = width(std::string(), plain);
    const double oldstyleWidth = width("oldstyle-nums tabular-nums", oldstyle);
    Check(plainWidth > 0.0 && oldstyleWidth > 0.0 &&
              oldstyleWidth < plainWidth - 10.0,
          "old-style figures, inherited from the box, set narrower: " +
              std::to_string(plainWidth) + " / " +
              std::to_string(oldstyleWidth));
    Check(DumpMotionFrame(oldstyle).find(
              "\"fontVariantNumeric\":\"oldstyle-nums tabular-nums\"") !=
                  std::string::npos &&
              DumpMotionFrame(plain).find("fontVariantNumeric") ==
                  std::string::npos,
          "the descriptor says which figures a text shows, and only then");

    MotionCoreTextMeasurer measurer;
    MotionTextStyle style;
    style.font_family = "Baskerville";
    style.font_size = 60;
    style.numeric_figures = "oldstyle";
    const bool changes = measurer.NumericVariantChanges("1 point", style);
    const bool letters = measurer.NumericVariantChanges("point", style);
    style.font_family = "Georgia";
    style.numeric_figures = "lining";
    const bool same = measurer.NumericVariantChanges("1 point", style);
    Check(changes && !letters && !same,
          "a variant changes a text only where it has figures the font sets "
          "otherwise");
}

// REMOTION-PARITY-2026-09 REM6 -- a photo stored on its side, as a phone
// writes it: 80 x 40 pixels, red left and blue right, with an EXIF
// orientation of 6 (turn a quarter clockwise to show). Shown, it is 40 x 80,
// red on top.
bool WriteTurnedJpeg(const std::string& path) {
    constexpr int kStoredWidth = 80;
    constexpr int kStoredHeight = 40;
    std::vector<uint8_t> pixels(static_cast<size_t>(kStoredWidth) *
                                kStoredHeight * 4);
    for (int y = 0; y < kStoredHeight; ++y) {
        for (int x = 0; x < kStoredWidth; ++x) {
            uint8_t* pixel =
                &pixels[(static_cast<size_t>(y) * kStoredWidth + x) * 4];
            pixel[0] = x < kStoredWidth / 2 ? 255 : 0;
            pixel[2] = x < kStoredWidth / 2 ? 0 : 255;
            pixel[3] = 255;
        }
    }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        pixels.data(), kStoredWidth, kStoredHeight, 8, kStoredWidth * 4, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
    CGImageRef image =
        context != nullptr ? CGBitmapContextCreateImage(context) : nullptr;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
        static_cast<CFIndex>(path.size()), false);
    CGImageDestinationRef destination =
        url != nullptr ? CGImageDestinationCreateWithURL(
                             url, CFSTR("public.jpeg"), 1, nullptr)
                       : nullptr;
    bool written = false;
    if (image != nullptr && destination != nullptr) {
        const int orientation = 6;
        const double quality = 1.0;
        CFNumberRef orientationValue =
            CFNumberCreate(nullptr, kCFNumberIntType, &orientation);
        CFNumberRef qualityValue =
            CFNumberCreate(nullptr, kCFNumberDoubleType, &quality);
        const void* keys[] = {kCGImagePropertyOrientation,
                              kCGImageDestinationLossyCompressionQuality};
        const void* values[] = {orientationValue, qualityValue};
        CFDictionaryRef properties = CFDictionaryCreate(
            nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(destination, image, properties);
        written = CGImageDestinationFinalize(destination);
        CFRelease(properties);
        CFRelease(qualityValue);
        CFRelease(orientationValue);
    }
    if (destination != nullptr) CFRelease(destination);
    if (url != nullptr) CFRelease(url);
    if (image != nullptr) CGImageRelease(image);
    if (context != nullptr) CGContextRelease(context);
    CGColorSpaceRelease(space);
    return written;
}

// The photo measured and drawn upright: contained in a square, it is a
// portrait column, red above blue, with nothing either side of it.
void TestTurnedPhoto(const std::filesystem::path& directory) {
    Check(WriteTurnedJpeg((directory / "turned.jpg").string()),
          "the turned photo is written");
    MotionCoreTextMeasurer measurer(directory.string());
    MotionError error;
    double width = 0.0;
    double height = 0.0;
    Check(measurer.ImageSize("turned.jpg", width, height, error) &&
              width == 40.0 && height == 80.0,
          "the turned photo measures as shown, 40 x 80: " +
              std::to_string(width) + " x " + std::to_string(height));
    const MotionComposition composition =
        Parse(R"({"width":100,"height":100,"fps":25,"duration":1,"root":{)"
              R"("type":"fill","children":[{"type":"image","src":"turned.jpg",)"
              R"("style":{"width":100,"height":100,"objectFit":"contain"}}]}})",
              "turned photo");
    MotionRaster raster;
    MotionFrame frame;
    if (!Render(composition, 0, directory.string(), raster, frame,
                "turned photo")) {
        return;
    }
    const Pixel top = At(raster, 50, 20);
    const Pixel bottom = At(raster, 50, 80);
    const Pixel side = At(raster, 10, 50);
    Check(Is(top, 255, 0, 0, 12) && Is(bottom, 0, 0, 255, 12) &&
              side.alpha == 0.0,
          "the turned photo is drawn upright, red above blue, a column in "
          "the square: " +
              Text(top) + " " + Text(bottom) + " " + Text(side));
}

// MOTION-DESIGN-2026-09 -- motion blur, blend modes, grain. A box moving
// 20 px a frame under a 180-degree shutter of four instants covers what it
// crosses in quarters; a screen of orange over blue lightens both; the same
// grain twice is the same bytes, and another phase is another grain.
void TestMotionDesignTools() {
    MotionComposition blur = Parse(
        R"({"width":120,"height":20,"fps":25,"duration":10,)"
        R"("motionBlur":{"samples":4,"shutterAngle":180},)"
        R"("root":{"type":"fill","children":[{"type":"box","style":{)"
        R"("position":"absolute","left":0,"top":0,"width":10,"height":20,)"
        R"("backgroundColor":"#FFFFFF"},"animate":{"translateX":{)"
        R"("interpolate":{"frames":[0,9],"values":[0,180]}}}}]}})",
        "motion blur");
    MotionRaster raster;
    MotionFrame frame;
    if (Render(blur, 2, kFonts, raster, frame, "motion blur")) {
        // Instants at 2, 2.125, 2.25, 2.375: the box at 40, 42.5, 45, 47.5.
        const Pixel first = At(raster, 41, 10);
        const Pixel all = At(raster, 48, 10);
        const Pixel last = At(raster, 56, 10);
        const Pixel none = At(raster, 58, 10);
        Check(
            frame.motion_blur.size() == 3 && std::abs(first.alpha - 64) <= 2 &&
                all.alpha == 255 && std::abs(last.alpha - 64) <= 2 &&
                none.alpha == 0,
            "motion blur averages the shutter's four instants: " + Text(first) +
                " " + Text(all) + " " + Text(last) + " " + Text(none));
    }
    MotionComposition blend =
        Parse(R"({"width":20,"height":20,"fps":25,"duration":1,"root":{"type":)"
              R"("fill","style":{"backgroundColor":"#1F4FFF"},"children":[{)"
              R"("type":"box","style":{"width":20,"height":20,)"
              R"("backgroundColor":"#FF5A1F","mixBlendMode":"screen"}}]}})",
              "screen");
    if (Render(blend, 0, kFonts, raster, frame, "screen")) {
        // screen(a, b) = a + b - a b, per channel.
        const auto screen = [](double a, double b) {
            return 255.0 * (a / 255 + b / 255 - (a / 255) * (b / 255));
        };
        Check(Is(At(raster, 10, 10), screen(0xFF, 0x1F), screen(0x5A, 0x4F),
                 screen(0x1F, 0xFF), 2),
              "screen lightens orange over blue: " + Text(At(raster, 10, 10)));
    }
    const auto grainy = [&](const char* phase) {
        const std::string json =
            std::string(
                "{\"width\":64,\"height\":64,\"fps\":25,"
                "\"duration\":1,\"root\":{\"type\":\"fill\","
                "\"style\":{\"backgroundColor\":\"#808080\","
                "\"filter\":\"grain(0.2 2px 5 ") +
            phase + ")\"}}}";
        const MotionComposition grain = Parse(json, "grain");
        MotionRaster out;
        MotionFrame evaluated;
        Render(grain, 0, kFonts, out, evaluated, "grain");
        return out.pixels;
    };
    const std::vector<uint8_t> once = grainy("3");
    const std::vector<uint8_t> again = grainy("3.7");
    const std::vector<uint8_t> other = grainy("4");
    double sum = 0.0;
    for (size_t index = 0; index < once.size(); index += 4) {
        sum += once[index];
    }
    const double mean = sum / (once.size() / 4.0);
    Check(!once.empty() && once == again && once != other &&
              std::abs(mean - 128.0) < 6.0 && once[0] != once[4 * 40],
          "grain is seeded, whole phases only, centred on the picture: mean " +
              std::to_string(mean));
    MotionComposition refused;
    MotionError error;
    Check(!ParseMotionComposition(
              R"({"width":8,"height":8,"fps":25,"duration":1,)"
              R"("motionBlur":{"samples":1},"root":{"type":"fill"}})",
              refused, error) &&
              !ParseMotionComposition(
                  R"({"width":8,"height":8,"fps":25,"duration":1,)"
                  R"J("root":{"type":"fill","style":{"filter":"grain(2)"}}})J",
                  refused, error) &&
              !ParseMotionComposition(
                  R"({"width":8,"height":8,"fps":25,"duration":1,)"
                  R"("root":{"type":"fill","style":{"mixBlendMode":"glow"}}})",
                  refused, error),
          "one instant, a grain past full scale, an unknown blend are refused");
}

// MOTION-DESIGN-2026-09 -- morph, mattes, dashes, goo, composition grain
// and rendering at scale.
void TestMotionDesignTools2() {
    // A square morphing into a diamond: half-way, its top vertex is half-way.
    const MotionComposition morph =
        Parse(R"({"width":200,"height":200,"fps":25,"duration":11,"root":{)"
              R"("type":"fill","children":[{"type":"path","style":{)"
              R"("position":"absolute","left":100,"top":100,"fill":"#FFFFFF"},)"
              R"("d":"M -60 -60 L 60 -60 L 60 60 L -60 60 Z","animate":{"d":{)"
              R"("interpolate":{"frames":[0,10],"values":[)"
              R"("M -60 -60 L 60 -60 L 60 60 L -60 60 Z",)"
              R"("M 0 -90 L 90 0 L 0 90 L -90 0 Z"]}}}}]}})",
              "morph");
    MotionRaster raster;
    MotionFrame frame;
    if (Render(morph, 5, kFonts, raster, frame, "morph")) {
        // Corner (-60,-60) moves to (0,-90): at t = 1/2, (-30,-75), so the
        // pixel (100-30, 100-73) is inside and (100-30, 100-80) is not.
        Check(At(raster, 70, 28).alpha > 200 && At(raster, 70, 20).alpha == 0,
              "a morph half-way draws the half-way shape: " +
                  Text(At(raster, 70, 28)) + " " + Text(At(raster, 70, 20)));
    }
    MotionComposition refused;
    MotionError error;
    Check(!ParseMotionComposition(
              R"({"width":8,"height":8,"fps":25,"duration":2,"root":{)"
              R"("type":"path","d":"M0 0 L4 0 L4 4 Z","animate":{"d":{)"
              R"("interpolate":{"frames":[0,1],"values":["M0 0 L4 0 L4 4 Z",)"
              R"("M0 0 L4 0 L4 4 L0 4 Z"]}}}}})",
              refused, error) &&
              error.code == "InvalidAnimation",
          "a morph between two structures is refused: " + error.code);
    // A white square as a matte over red: inside red stays, outside goes.
    const MotionComposition matte = Parse(
        R"({"width":40,"height":20,"fps":25,"duration":1,"root":{"type":)"
        R"("fill","style":{"isolation":"isolate"},"children":[{"type":"box",)"
        R"("style":{"position":"absolute","left":0,"top":0,"width":40,)"
        R"("height":20,"backgroundColor":"#FF0000"}},{"type":"box","style":{)"
        R"("position":"absolute","left":0,"top":0,"width":20,"height":20,)"
        R"("backgroundColor":"#FFFFFF","mixBlendMode":"mask-in"}}]}})",
        "matte");
    if (Render(matte, 0, kFonts, raster, frame, "matte")) {
        Check(Is(At(raster, 10, 10), 255, 0, 0, 1) &&
                  At(raster, 30, 10).alpha == 0,
              "mask-in keeps what lies under the matte, inside it only: " +
                  Text(At(raster, 10, 10)) + " " + Text(At(raster, 30, 10)));
    }
    // Dashes 10 on, 10 off along a horizontal line.
    const MotionComposition dash = Parse(
        R"({"width":60,"height":10,"fps":25,"duration":1,"root":{"type":)"
        R"("fill","children":[{"type":"path","style":{"position":)"
        R"("absolute","left":0,"top":0,"fill":"none","stroke":"#FFFFFF",)"
        R"("strokeWidth":4,"strokeDasharray":"10 10"},"d":"M0 5 L60 5"}]}})",
        "dash");
    if (Render(dash, 0, kFonts, raster, frame, "dash")) {
        Check(At(raster, 5, 5).alpha > 200 && At(raster, 15, 5).alpha == 0 &&
                  At(raster, 25, 5).alpha > 200,
              "a dashed stroke alternates on and off");
    }
    // Goo of 8 px: two discs 4 px apart melt; the same far apart do not.
    const auto gooey = [&](int gap) {
        const std::string json =
            "{\"width\":200,\"height\":60,\"fps\":25,\"duration\":1,"
            "\"root\":{\"type\":\"fill\",\"style\":{\"filter\":\"goo(8px)\"},"
            "\"children\":[{\"type\":\"box\",\"style\":{\"position\":"
            "\"absolute\",\"left\":20,\"top\":10,\"width\":40,\"height\":40,"
            "\"borderRadius\":20,\"backgroundColor\":\"#FFFFFF\"}},{\"type\":"
            "\"box\",\"style\":{\"position\":\"absolute\",\"left\":" +
            std::to_string(60 + gap) +
            ",\"top\":10,\"width\":40,\"height\":40,\"borderRadius\":20,"
            "\"backgroundColor\":\"#FFFFFF\"}}]}}";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, "goo"), 0, kFonts, out, evaluated, "goo");
        return At(out, 60 + gap / 2, 30).alpha;
    };
    Check(gooey(4) > 200 && gooey(60) == 0,
          "goo melts near shapes together and leaves far ones apart");
    // Composition grain: seeded, a new grain every frame.
    const MotionComposition grain = Parse(
        R"({"width":32,"height":32,"fps":25,"duration":3,)"
        R"("grain":{"amount":0.2,"seed":4},"root":{"type":"fill","style":{)"
        R"("backgroundColor":"#808080"}}})",
        "grain");
    MotionRaster first;
    MotionRaster again;
    MotionRaster next;
    Render(grain, 1, kFonts, first, frame, "grain");
    Render(grain, 1, kFonts, again, frame, "grain");
    Render(grain, 2, kFonts, next, frame, "grain");
    Check(first.pixels == again.pixels && first.pixels != next.pixels,
          "composition grain is seeded and new every frame");
    // Rendering at scale: half the size, the box at half its place.
    const MotionComposition box = Parse(
        R"({"width":200,"height":100,"fps":25,"duration":1,"root":{"type":)"
        R"("fill","children":[{"type":"box","style":{"position":"absolute",)"
        R"("left":100,"top":40,"width":60,"height":20,)"
        R"("backgroundColor":"#FFFFFF"}}]}})",
        "scale");
    MotionCoreTextMeasurer measurer(kFonts);
    MotionFrame evaluated;
    MotionRaster half;
    if (EvaluateMotionFrame(box, mcp_json::Value(), 0, measurer, evaluated,
                            error)) {
        ScaleMotionFrame(evaluated, 0.5);
        RenderMotionFrame(evaluated, measurer, half, error);
    }
    Check(half.width == 100 && half.height == 50 &&
              At(half, 60, 25).alpha == 255 && At(half, 45, 25).alpha == 0,
          "a frame drawn at half scale is half the size, its boxes at half "
          "their place");
    // Slices: a white bar whose left edge moves row by row, by whole
    // pixels; the pattern changes with the whole part of the phase only.
    const auto sliced = [&](const std::string& phase) {
        const std::string json =
            "{\"width\":200,\"height\":80,\"fps\":25,\"duration\":1,"
            "\"root\":{\"type\":\"fill\",\"children\":[{\"type\":"
            "\"box\",\"style\":{\"position\":\"absolute\",\"left\":60,"
            "\"top\":0,\"width\":80,\"height\":80,\"backgroundColor\":"
            "\"#FFFFFF\",\"filter\":\"slice(30px 8px 7 " +
            phase + ")\"}}]}}";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, "slice"), 0, kFonts, out, evaluated, "slice");
        std::vector<int> edges;
        for (int y = 0; y < out.height; ++y) {
            int edge = -1;
            for (int x = 0; x < out.width && edge < 0; ++x) {
                edge = At(out, x, y).alpha > 128 ? x : -1;
            }
            edges.push_back(edge);
        }
        return edges;
    };
    const std::vector<int> zero = sliced("0");
    int moved = 0;
    int kept = 0;
    for (const int edge : zero) {
        moved += edge != 60 && edge >= 30 && edge <= 90 ? 1 : 0;
        kept += edge == 60 ? 1 : 0;
    }
    Check(moved > 8 && kept > 8,
          "slice moves some rows sideways and leaves others in place: " +
              std::to_string(moved) + " moved, " + std::to_string(kept) +
              " kept");
    Check(sliced("0.6") == zero && sliced("1") != zero,
          "slice changes its pattern with the whole part of the phase");
}

// MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. What the shutter does to a
// grain whose phase moves: a still grey box, grain(0.3) with its phase
// animated one unit a frame. From a whole phase at the frame, every
// instant of a 360-degree shutter has the same whole part, the same grain,
// and the blurred frame is the sharp one. Half a unit later the shutter
// crosses a whole phase and averages two grains: the contract holds only
// as far as the phase's whole part stays put over the shutter.
void TestGrainUnderShutter() {
    const auto grainy = [&](bool blurred, const char* from, const char* to) {
        const std::string json =
            std::string(R"({"width":32,"height":32,"fps":25,"duration":10,)") +
            (blurred ? R"("motionBlur":{"samples":4,"shutterAngle":360},)"
                     : "") +
            R"("root":{"type":"fill","style":{"backgroundColor":"#808080"},)"
            R"("animate":{"filter":{"interpolate":{"frames":[0,9],)"
            R"("easing":"linear","values":["grain(0.3 1px 5 )" +
            from + R"J()","grain(0.3 1px 5 )J" + to + R"J()"]}}}}})J";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, "grain under the shutter"), 3, kFonts, out,
               evaluated, "grain under the shutter");
        return out.pixels;
    };
    Check(!grainy(false, "0", "9").empty() &&
              grainy(true, "0", "9") == grainy(false, "0", "9"),
          "a phase whole at the frame keeps one grain over the shutter");
    Check(grainy(true, "0.5", "9.5") != grainy(false, "0.5", "9.5"),
          "a phase that crosses a whole number under the shutter mixes two "
          "grains");
    // A phase interpolated in floating point lands a rounding short of a
    // whole number on some frames: from 0 to 22 over 22 frames, frame 15
    // is 14.999999999999998, and its floor showed frame 14's grain again
    // -- on 3.7 % of the frames of such animations between 100 and 1000
    // frames long. A phase within a few ulps of a whole number is that
    // number.
    const auto at = [&](int64_t frame, bool blurred) {
        const std::string json =
            std::string(R"({"width":32,"height":32,"fps":25,"duration":23,)") +
            (blurred ? R"("motionBlur":{"samples":4,"shutterAngle":360},)"
                     : "") +
            R"("root":{"type":"fill","style":{"backgroundColor":"#808080"},)"
            R"("animate":{"filter":{"interpolate":{"frames":[0,22],)"
            R"J("easing":"linear","values":["grain(0.3 1px 5 0)",)J"
            R"J("grain(0.3 1px 5 22)"]}}}}})J";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, "grain a frame"), frame, kFonts, out, evaluated,
               "grain a frame");
        return out.pixels;
    };
    Check(!at(15, false).empty() && at(15, false) != at(14, false) &&
              at(15, true) == at(15, false),
          "a phase one rounding short of 15 at frame 15 shows grain 15, "
          "sharp under the shutter");
}

// MA7 -- grain and slices belong to the node: moved by whole pixels, it
// shows the same pattern at the same place on it, even half out of the
// frame. They were counted from the corner of the layer of its effects,
// which the frame's left or top edge cuts: a node sliding in from the left
// had its grain fixed to the frame, then caught up with it when its edge
// came in.
void TestPatternsFollowTheNode() {
    const auto painted = [&](const std::string& filter, int left, int top) {
        const std::string json =
            R"({"width":60,"height":60,"fps":25,"duration":1,"root":{"type":)"
            R"("fill","children":[{"type":"box","style":{"position":)"
            R"("absolute","left":)" +
            std::to_string(left) + R"(,"top":)" + std::to_string(top) +
            R"(,"width":40,"height":40,"backgroundColor":"#808080",)"
            R"("filter":")" +
            filter + "\"}}]}}";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, filter), 0, kFonts, out, evaluated, filter);
        return out;
    };
    // The node's rows 10 to 30 and its columns from `first` to `last`,
    // wherever it was drawn.
    const auto same = [](const MotionRaster& a, int ax, int ay,
                         const MotionRaster& b, int bx, int by, int first,
                         int last) {
        if (a.width != 60 || b.width != 60) {
            return false;
        }
        for (int y = 10; y < 30; ++y) {
            for (int x = first; x < last; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    const auto at = [&](const MotionRaster& r, int px, int py) {
                        return r.pixels[(static_cast<size_t>(py) * r.width +
                                         static_cast<size_t>(px)) *
                                            4 +
                                        static_cast<size_t>(channel)];
                    };
                    if (at(a, ax + x, ay + y) != at(b, bx + x, by + y)) {
                        return false;
                    }
                }
            }
        }
        return true;
    };
    const MotionRaster inside = painted("grain(0.5 1px 3)", 10, 10);
    const MotionRaster leftOut = painted("grain(0.5 1px 3)", -10, 10);
    const MotionRaster topOut = painted("grain(0.5 1px 3)", 10, -10);
    Check(same(inside, 10, 10, leftOut, -10, 10, 10, 30) &&
              same(inside, 10, 10, topOut, 10, -10, 10, 30),
          "the grain of a node half out of the frame is the grain of the "
          "node inside it");
    const MotionRaster sliced = painted("slice(6px 4px 9)", 10, 10);
    const MotionRaster slicedOut = painted("slice(6px 4px 9)", 10, -10);
    const MotionRaster plain = painted("grain(0 1px 0)", 10, 10);
    // Every column a row may be shifted to, the box's edges included.
    Check(!same(sliced, 10, 10, plain, 10, 10, -8, 48) &&
              same(sliced, 10, 10, slicedOut, 10, -10, -8, 48),
          "the slices of a node half out of the frame are those of the node "
          "inside it");
}

// MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. The three track mattes, by
// the plain path and by the depth path, over a red bar from 0 to 40 px: a
// white matte over its left half (mask-in, mask-out), or from 20 to 60 px
// (atop).
void TestMattes() {
    const auto matte = [&](const std::string& mode, const std::string& extra) {
        const bool atop = mode == "atop";
        const std::string json =
            R"({"width":60,"height":20,"fps":25,"duration":1,"root":{"type":)"
            R"("fill","style":{"isolation":"isolate"},"children":[{"type":)"
            R"("box","style":{"position":"absolute","left":0,"top":0,)"
            R"("width":40,"height":20,"backgroundColor":"#FF0000"}},)"
            R"({"type":"box","style":{"position":"absolute","left":)" +
            std::string(atop ? "20" : "0") + R"(,"top":0,"width":)" +
            std::string(atop ? "40" : "20") +
            R"(,"height":20,"backgroundColor":"#FFFFFF","mixBlendMode":")" +
            mode + "\"" + extra + "}}]}}";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, mode), 0, kFonts, out, evaluated, mode);
        return out;
    };
    MotionRaster in = matte("mask-in", "");
    Check(in.width == 60 && Is(At(in, 10, 10), 255, 0, 0, 1) &&
              At(in, 30, 10).alpha == 0 && At(in, 50, 10).alpha == 0,
          "mask-in keeps the red under the matte only: " +
              Text(At(in, 10, 10)) + " " + Text(At(in, 30, 10)));
    const MotionRaster out = matte("mask-out", "");
    Check(out.width == 60 && At(out, 10, 10).alpha == 0 &&
              Is(At(out, 30, 10), 255, 0, 0, 1) && At(out, 50, 10).alpha == 0,
          "mask-out keeps the red outside the matte only: " +
              Text(At(out, 10, 10)) + " " + Text(At(out, 30, 10)));
    MotionRaster atop = matte("atop", "");
    Check(atop.width == 60 && Is(At(atop, 10, 10), 255, 0, 0, 1) &&
              Is(At(atop, 30, 10), 255, 255, 255, 1) &&
              At(atop, 50, 10).alpha == 0,
          "atop paints the matte over the red only: " + Text(At(atop, 10, 10)) +
              " " + Text(At(atop, 30, 10)) + " " + Text(At(atop, 50, 10)));
    // A matte turned in depth goes by the depth path: drawn flat into a
    // layer of its own, then laid by its mode. Its mode applied inside that
    // layer as well, over nothing, left no matte at all: a mask-in then
    // erased the whole group, an atop painted nothing.
    in = matte("mask-in", R"(,"rotateY":30)");
    Check(in.width == 60 && Is(At(in, 10, 10), 255, 0, 0, 1) &&
              At(in, 0, 10).alpha == 0 && At(in, 30, 10).alpha == 0,
          "mask-in turned in depth keeps the red under its foreshortened "
          "box: " +
              Text(At(in, 10, 10)) + " " + Text(At(in, 0, 10)) + " " +
              Text(At(in, 30, 10)));
    atop = matte("atop", R"(,"rotateY":30)");
    Check(atop.width == 60 && Is(At(atop, 10, 10), 255, 0, 0, 1) &&
              Is(At(atop, 35, 10), 255, 255, 255, 1) &&
              At(atop, 50, 10).alpha == 0,
          "atop turned in depth paints the matte over the red only: " +
              Text(At(atop, 10, 10)) + " " + Text(At(atop, 35, 10)) + " " +
              Text(At(atop, 50, 10)));
}

}  // namespace

// MOTION-DESIGN-2026-09 -- mesh(): a black-to-white ramp whose middle
// grid line moves from 50 % to 80 %. Each column must show the ramp where
// After's Mesh Warp puts it: along each cell, a cubic Bézier from the
// vertex to its neighbour, handles a third of the resting cell away.
void TestMeshWarpPainted() {
    const auto ramp = [&](const std::string& filter) {
        const std::string json =
            R"({"width":200,"height":100,"fps":25,"duration":1,"root":)"
            R"({"type":"fill","children":[{"type":"box","style":)"
            R"({"position":"absolute","left":0,"top":0,"width":200,)"
            R"("height":100,"backgroundImage":)"
            R"J("linear-gradient(90deg, #000000 0%, #FFFFFF 100%)")J" +
            (filter.empty() ? std::string()
                            : ",\"filter\":\"" + filter + "\"") +
            "}}]}}";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, "mesh"), 0, kFonts, out, evaluated, "mesh");
        return out;
    };
    const auto bezier = [](double p0, double p1, double p2, double p3,
                           double t) {
        const double s = 1.0 - t;
        return s * s * s * p0 + 3.0 * s * s * t * p1 + 3.0 * s * t * t * p2 +
               t * t * t * p3;
    };
    // The resting column a warped column shows, found along the Bézier.
    const auto source = [&](double x) {
        const bool left = x < 160.0;
        const double p0 = left ? 0.0 : 160.0;
        const double p3 = left ? 160.0 : 200.0;
        const double third = 100.0 / 3.0;
        double low = 0.0;
        double high = 1.0;
        for (int step = 0; step < 60; ++step) {
            const double middle = (low + high) / 2.0;
            (bezier(p0, p0 + third, p3 - third, p3, middle) < x ? low : high) =
                middle;
        }
        return (left ? 0.0 : 100.0) + 100.0 * (low + high) / 2.0;
    };
    const MotionRaster warped =
        ramp("mesh(1 2, 0 0, 80% 0, 100% 0, 0 100%, 80% 100%, 100% 100%)");
    double worst = 0.0;
    int worstAt = -1;
    for (int x = 0; warped.width == 200 && x < 200; ++x) {
        const double expected = 255.0 * source(x + 0.5) / 200.0;
        const double error = std::abs(At(warped, x, 50).red - expected);
        if (error > worst) {
            worst = error;
            worstAt = x;
        }
    }
    Check(warped.width == 200 && worst <= 2.5,
          "mesh() stretches a cell along After's Béziers: worst column " +
              std::to_string(worstAt) + " off by " + std::to_string(worst));
    const MotionRaster plain = ramp("");
    const MotionRaster resting =
        ramp("mesh(1 2, 0 0, 50% 0, 100% 0, 0 100%, 50% 100%, 100% 100%)");
    double drift = 0.0;
    for (int x = 0; plain.width == 200 && resting.width == 200 && x < 200;
         ++x) {
        drift = std::max(
            drift, std::abs(At(plain, x, 50).red - At(resting, x, 50).red));
    }
    Check(
        plain.width == 200 && drift <= 1.5,
        "a mesh at rest leaves the picture as it is: " + std::to_string(drift));

    // A bar overhanging the meshed box above and below, as an accent does a
    // tight line: moving the whole grid 20 px right must take all of it
    // along, not leave its overhang where it was.
    const auto overhang = [&](const std::string& mesh) {
        const std::string json =
            R"({"width":200,"height":100,"fps":25,"duration":1,"root":)"
            R"({"type":"fill","style":{"backgroundColor":"#000000"},)"
            R"("children":[{"type":"box","style":{"position":"absolute",)"
            R"("left":50,"top":30,"width":100,"height":40,"filter":")" +
            mesh +
            R"("},"children":[{"type":"box","style":{"position":"absolute",)"
            R"("left":10,"top":-20,"width":20,"height":80,)"
            R"("backgroundColor":"#FFFFFF"}}]}]}})";
        MotionRaster out;
        MotionFrame evaluated;
        Render(Parse(json, "mesh overhang"), 0, kFonts, out, evaluated,
               "mesh overhang");
        return out;
    };
    const MotionRaster moved =
        overhang("mesh(1 1, 20px 0, 120px 0, 20px 40px, 120px 40px)");
    Check(moved.width == 200 && At(moved, 90, 15).red > 200 &&
              At(moved, 90, 85).red > 200 && At(moved, 90, 50).red > 200,
          "mesh() carries what overhangs its box along with the edge");
    Check(moved.width == 200 && At(moved, 65, 15).red < 30 &&
              At(moved, 65, 85).red < 30 && At(moved, 65, 50).red < 30,
          "mesh() leaves nothing of the overhang behind");
    const MotionRaster still =
        overhang("mesh(1 1, 0 0, 100% 0, 0 100%, 100% 100%)");
    Check(still.width == 200 && At(still, 70, 15).red > 200 &&
              At(still, 70, 85).red > 200 && At(still, 90, 15).red < 30,
          "a mesh at rest leaves the overhang as it is");
}

int main() {
    MotionError error;
    if (!RegisterMotionFontDirectory(kFonts, error)) {
        std::cerr << "FAIL: fixture fonts: " << error.message << '\n';
        return 1;
    }
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-motion-paint-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    // C08's picture: a 2:1 red mark with a transparent left half.
    MotionRaster logo;
    logo.width = 400;
    logo.height = 200;
    logo.pixels.assign(400 * 200 * 4, 0);
    for (int y = 0; y < logo.height; ++y) {
        for (int x = 200; x < logo.width; ++x) {
            uint8_t* pixel =
                &logo.pixels[(static_cast<size_t>(y) * logo.width + x) * 4];
            pixel[0] = 255;
            pixel[3] = 255;
        }
    }
    Check(WriteMotionPng(logo, (directory / "logo.png").string(), error),
          "the picture is written: " + error.message);
    TestC15Painted();
    TestC16Painted();
    TestC17Painted();
    TestC18Painted(directory);
    TestC19Painted();
    TestMaskPainted();
    TestItalic();
    TestNumericVariant();
    TestTurnedPhoto(directory);
    TestMotionDesignTools();
    TestMotionDesignTools2();
    TestGrainUnderShutter();
    TestPatternsFollowTheNode();
    TestMattes();
    TestMeshWarpPainted();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures != 0) {
        std::cerr << failures << " motion paint assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion paint\n";
    return 0;
}
