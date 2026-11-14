// MOTION-2026-09 M2 -- ROADMAP.md. Core Text measurement and Core Graphics
// rendering of evaluated motion frames. macOS only: this suite exercises the
// frameworks the portable motion suite fakes.

#include "Document.h"
#include "MotionCli.h"
#include "MotionComposition.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionHumanize.h"
#include "MotionRender.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"
#include "motion_humanize_fixtures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
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

const std::string kFonts = MOTION_FONT_DIR;
// MW7 -- a variable font of its own, registered by the test that needs it.
const std::string kVariableFonts = MOTION_VARIABLE_FONT_DIR;

MotionTextStyle Fira(double weight, double size) {
    MotionTextStyle style;
    style.font_family = "Fira Sans Condensed";
    style.font_weight = weight;
    style.font_size = size;
    return style;
}

bool EvaluateJson(const std::string& json, MotionCoreTextMeasurer& measurer,
                  MotionFrame& frame, const std::string& label,
                  int64_t index = 0) {
    MotionComposition composition;
    MotionError error;
    const bool ok = ParseMotionComposition(json, composition, error) &&
                    EvaluateMotionFrame(composition, mcp_json::Value(), index,
                                        measurer, frame, error);
    Check(ok, label + ": " + error.code + " at " + error.path + " -- " +
                  error.message);
    return ok;
}

bool Render(const std::string& json, MotionRaster& raster,
            const std::string& label, MotionFrame* evaluated = nullptr) {
    MotionCoreTextMeasurer measurer;
    MotionFrame frame;
    if (!EvaluateJson(json, measurer, frame, label)) {
        return false;
    }
    MotionError error;
    const bool ok = RenderMotionFrame(frame, measurer, raster, error);
    Check(ok, label + " renders: " + error.message);
    if (evaluated != nullptr) {
        *evaluated = frame;
    }
    return ok;
}

const uint8_t* Pixel(const MotionRaster& raster, int x, int y) {
    return &raster.pixels[(static_cast<size_t>(y) * raster.width + x) * 4];
}

// Rows holding a pixel brighter than half on the red channel, grouped into
// bands: the same measurement taken on the After Effects render.
struct Band {
    int top;
    int bottom;
    int left;
    int right;
};

std::vector<Band> Bands(const MotionRaster& raster) {
    std::vector<Band> bands;
    bool open = false;
    Band current{};
    for (int y = 0; y < raster.height; ++y) {
        int left = -1;
        int right = -1;
        for (int x = 0; x < raster.width; ++x) {
            if (Pixel(raster, x, y)[0] > 128) {
                if (left < 0) {
                    left = x;
                }
                right = x;
            }
        }
        if (left >= 0) {
            if (!open) {
                current = {y, y, left, right};
                open = true;
            } else {
                current.bottom = y;
                current.left = std::min(current.left, left);
                current.right = std::max(current.right, right);
            }
        } else if (open) {
            bands.push_back(current);
            open = false;
        }
    }
    if (open) {
        bands.push_back(current);
    }
    return bands;
}

std::string TitleCard(int width, int height, double top, double gap,
                      double size, double lineHeight) {
    char json[1600];
    std::snprintf(json, sizeof json,
                  R"({"width": %d, "height": %d, "fps": 25, "duration": 10,
  "root": {"type": "fill", "style": {"backgroundColor": "#000"}, "children": [
    {"type": "box", "style": {"position": "absolute", "top": %.4f, "left": 0, "right": 0,
      "flexDirection": "column", "alignItems": "center", "gap": %.4f,
      "fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": %.4f,
      "lineHeight": "%.4fpx", "textTransform": "uppercase", "textAlign": "center", "color": "#fff"},
     "children": [
       {"type": "text", "content": "Courez avec nous tous\npour la troisième\ncourse du matin"},
       {"type": "text", "content": "18 mars 2031"}]}]}})",
                  width, height, top, gap, size, lineHeight);
    return json;
}

void TestFontResolution() {
    MotionCoreTextMeasurer measurer;
    MotionError error;
    std::string file;
    Check(measurer.ResolveFontFile(Fira(500, 40), file, error) &&
              file.find("tests/fixtures/fonts/FiraSansCondensed-Medium.ttf") !=
                  std::string::npos,
          "a registered font wins over any installed copy of the same face: " +
              file);
    for (int weight : {300, 400, 500}) {
        measurer.ResolveFontFile(Fira(weight, 40), file, error);
        Check(file.find("-Medium.ttf") != std::string::npos,
              "CSS weight matching sends " + std::to_string(weight) +
                  " to the Medium face");
    }
    for (int weight : {550, 600, 900}) {
        measurer.ResolveFontFile(Fira(weight, 40), file, error);
        Check(file.find("-SemiBold.ttf") != std::string::npos,
              "CSS weight matching sends " + std::to_string(weight) +
                  " to the SemiBold face");
    }
    MotionTextStyle missing = Fira(500, 40);
    missing.font_family = "Nonexistent Motion Sans";
    MotionTextLayout layout;
    Check(!measurer.LayoutText("x", missing, -1.0, layout, error) &&
              error.code == "FontMissing",
          "an unknown family is refused, never substituted");
    Check(!measurer.LayoutText("汉字", Fira(500, 40), -1.0, layout, error) &&
              error.code == "FontMissing",
          "missing glyphs cannot silently use another font");
}

// MOTION-WEB-2026-10 MW7 -- ROADMAP.md. A variable font is drawn at the
// weight a style asks. Outfit (google/fonts, tests/fixtures/fonts/outfit) is
// one file whose nine named instances all carry the OS/2 weight class of the
// default one, Thin: read from there, every weight matched the instance Core
// Text lists first, Regular. Measured on the picture: the ink of one line,
// and its advance, grow with the weight asked, between named instances too
// (650), and stop at the ends of the axis as CSS clamps them.
void TestVariableFontWeight() {
    MotionError error;
    if (!RegisterMotionFontDirectory(kVariableFonts, error)) {
        Check(false, "variable fixture font: " + error.message);
        return;
    }
    const auto card = [](int weight) {
        return R"({"width": 900, "height": 140, "fps": 25, "duration": 1,
          "root": {"type": "fill", "children": [
            {"type": "text", "content": "Hamburgefonstiv",
             "style": {"position": "absolute", "left": 10, "top": 10,
                       "fontFamily": "Outfit", "fontWeight": )" +
               std::to_string(weight) + R"(, "fontSize": 80,
                       "color": "#ffffff"}}]}})";
    };
    struct Drawn {
        int weight = 0;
        double ink = 0.0;
        double advance = 0.0;
    };
    std::vector<Drawn> drawn;
    for (int weight : {50, 100, 300, 400, 600, 650, 800, 900, 950}) {
        MotionRaster raster;
        MotionFrame frame;
        if (!Render(card(weight), raster, "Outfit at " + std::to_string(weight),
                    &frame) ||
            frame.root.children.empty()) {
            return;
        }
        Drawn line;
        line.weight = weight;
        for (size_t at = 3; at < raster.pixels.size(); at += 4) {
            line.ink += raster.pixels[at] / 255.0;
        }
        line.advance = frame.root.children.front().text_layout.width;
        drawn.push_back(line);
    }
    const auto at = [&](int weight) -> const Drawn& {
        return *std::find_if(drawn.begin(), drawn.end(), [&](const Drawn& d) {
            return d.weight == weight;
        });
    };
    const std::vector<int> rising = {100, 300, 400, 600, 650, 800, 900};
    for (size_t index = 1; index < rising.size(); ++index) {
        const Drawn& lighter = at(rising[index - 1]);
        const Drawn& heavier = at(rising[index]);
        Check(heavier.ink > lighter.ink * 1.01 &&
                  heavier.advance > lighter.advance + 0.5,
              "Outfit " + std::to_string(heavier.weight) + " is heavier than " +
                  std::to_string(lighter.weight) + ": ink " +
                  std::to_string(heavier.ink) + " vs " +
                  std::to_string(lighter.ink) + ", advance " +
                  std::to_string(heavier.advance) + " vs " +
                  std::to_string(lighter.advance));
    }
    Check(at(50).ink == at(100).ink && at(950).ink == at(900).ink,
          "a weight beyond the axis is drawn at its end, as CSS clamps it: " +
              std::to_string(at(50).ink) + " / " + std::to_string(at(100).ink) +
              ", " + std::to_string(at(950).ink) + " / " +
              std::to_string(at(900).ink));

    MotionCoreTextMeasurer measurer;
    MotionTextStyle style;
    style.font_family = "Outfit";
    style.font_size = 80;
    style.font_weight = 600;
    std::string file;
    Check(measurer.ResolveFontFile(style, file, error) &&
              file.find("tests/fixtures/fonts/outfit/Outfit[wght].ttf") !=
                  std::string::npos,
          "the weight comes from the fixture's one variable file: " + file);
    style.italic = true;
    MotionTextLayout layout;
    Check(!measurer.LayoutText("x", style, -1.0, layout, error) &&
              error.code == "FontMissing",
          "a variable family without an italic still refuses to slant");
}

void TestTextModel() {
    MotionCoreTextMeasurer measurer;
    MotionError error;
    MotionTextStyle style = Fira(500, 60);
    style.line_height = 72.0;
    style.text_transform = "uppercase";
    MotionTextLayout layout;
    Check(
        measurer.LayoutText("journées\nà venir", style, -1.0, layout, error) &&
            layout.lines.size() == 2 && layout.lines[0].text == "JOURNÉES" &&
            layout.lines[1].text == "À VENIR",
        "uppercase maps accented letters and keeps explicit line breaks");
    Check(layout.lines.size() == 2 &&
              std::fabs(layout.lines[1].baseline - layout.lines[0].baseline -
                        72.0) < 1e-9 &&
              layout.lines[0].baseline > 0.0 &&
              layout.lines[0].baseline < 72.0 &&
              std::fabs(layout.height - 144.0) < 1e-9,
          "each line box is line-height tall and holds its baseline");

    MotionTextLayout trailing;
    MotionTextLayout plain;
    style.text_transform = "none";
    measurer.LayoutText("abc   ", style, -1.0, trailing, error);
    measurer.LayoutText("abc", style, -1.0, plain, error);
    Check(std::fabs(trailing.width - plain.width) < 1e-9,
          "trailing spaces do not widen a line, as in CSS");

    MotionTextLayout wrapped;
    Check(measurer.LayoutText("une phrase assez longue pour passer à la ligne",
                              style, 300.0, wrapped, error) &&
              wrapped.lines.size() > 1 &&
              std::all_of(wrapped.lines.begin(), wrapped.lines.end(),
                          [](const MotionTextLine& line) {
                              return line.width <= 300.0 + 1e-6;
                          }),
          "a constrained width wraps text within it");

    style.text_align = "center";
    MotionTextLayout centred;
    measurer.LayoutText("abc", style, 400.0, centred, error);
    Check(!centred.lines.empty() &&
              std::fabs(centred.lines[0].x -
                        (400.0 - centred.lines[0].width) / 2.0) < 1e-9,
          "centred lines are centred within the available width");
}

// Reference measurements of a title card, in both formats, at threshold 128.
// The card was fitted on 13 September 2026 against a real one rendered by
// After Effects 26 -- font, size, line pitch, first baseline, date baseline --
// and landed every edge of every band within 2 px of it. Its text has since
// been replaced by an invented one, which After never rendered: the numbers
// below are the engine's own render of 14 November 2026 with the same font
// and geometry. They hold the engine to itself, within the same 2 px.
void TestReferenceTitleCards() {
    struct Case {
        const char* name;
        int width;
        int height;
        double scale;
        double first_baseline;
        int expected[5][4];
    };
    // Size and pitch were fitted once on the 9:16 card (Medium, 61.17 px at
    // After's 120 % scale; 60 px pitch). The 16:9 card is the same intent at
    // After's 195.99 % scale, with nothing refitted.
    const double baseSize = 73.4 / 1.2;
    const double basePitch = 60.0;
    const Case cases[] = {
        {"9:16",
         1080,
         1920,
         1.2,
         835.0,
         {{783, 835, 158, 922},
          {839, 850, 725, 741},
          {855, 907, 247, 834},
          {927, 979, 268, 809},
          {1057, 1109, 338, 736}}},
        {"16:9",
         1920,
         1080,
         1.959882,
         338.0,
         {{253, 339, 336, 1585},
          {344, 364, 1262, 1289},
          {371, 456, 482, 1441},
          {489, 574, 515, 1400},
          {702, 787, 630, 1281}}},
    };
    for (const Case& reference : cases) {
        const double size = baseSize * reference.scale;
        const double pitch = basePitch * reference.scale;
        // CSS stacking: last title baseline to date baseline is one line box
        // plus the gap.
        const double gap =
            (reference.expected[4][1] - reference.expected[3][1]) - pitch;

        // Read where the first baseline lands, then place the card: the
        // geometry comes from the engine, not from measuring pixels.
        MotionCoreTextMeasurer probeMeasurer;
        MotionFrame probe;
        if (!EvaluateJson(TitleCard(reference.width, reference.height, 0.0, gap,
                                    size, pitch),
                          probeMeasurer, probe, reference.name)) {
            continue;
        }
        const MotionEvaluatedNode& title = probe.root.children[0].children[0];
        const double top = reference.first_baseline -
                           (title.box.y + title.text_layout.lines[0].baseline);

        MotionRaster raster;
        if (!Render(TitleCard(reference.width, reference.height, top, gap, size,
                              pitch),
                    raster, reference.name)) {
            continue;
        }
        const std::vector<Band> bands = Bands(raster);
        int worst = 0;
        std::string measured;
        for (const Band& band : bands) {
            measured += " " + std::to_string(band.top) + "-" +
                        std::to_string(band.bottom) + " x" +
                        std::to_string(band.left) + "-" +
                        std::to_string(band.right);
        }
        if (bands.size() == 5) {
            for (size_t index = 0; index < 5; ++index) {
                const int edges[4] = {bands[index].top, bands[index].bottom,
                                      bands[index].left, bands[index].right};
                for (int edge = 0; edge < 4; ++edge) {
                    worst = std::max(worst,
                                     std::abs(edges[edge] -
                                              reference.expected[index][edge]));
                }
            }
        }
        Check(bands.size() == 5 && worst <= 2,
              std::string(reference.name) +
                  " title card matches its reference within 2 px "
                  "(worst " +
                  std::to_string(worst) + " px):" + measured);
        std::cout << "reference " << reference.name << ": worst deviation "
                  << worst << " px\n";
    }
}

void TestDrawing() {
    MotionRaster raster;
    if (Render(R"({"width": 100, "height": 100, "fps": 25, "duration": 1,
      "root": {"type": "fill", "children": [
        {"type": "box", "style": {"position": "absolute", "left": 10, "top": 10, "width": 30, "height": 30,
                                  "backgroundColor": "#ff0000", "borderRadius": 15}},
        {"type": "box", "style": {"position": "absolute", "left": 60, "top": 10, "width": 30, "height": 30,
                                  "backgroundColor": "#00ff00", "opacity": 0.5}},
        {"type": "box", "style": {"position": "absolute", "left": 10, "top": 60, "width": 20, "height": 20,
                                  "backgroundColor": "#0000ff", "translateX": 50}}]}})",
               raster, "primitives")) {
        const uint8_t* centre = Pixel(raster, 25, 25);
        const uint8_t* corner = Pixel(raster, 11, 11);
        const uint8_t* half = Pixel(raster, 75, 25);
        const uint8_t* moved = Pixel(raster, 70, 70);
        const uint8_t* vacated = Pixel(raster, 20, 70);
        Check(centre[0] == 255 && centre[3] == 255,
              "a background fills its box");
        Check(corner[3] == 0, "a border radius leaves the corner empty");
        Check(std::abs(half[3] - 128) <= 1 && std::abs(half[1] - 128) <= 1,
              "opacity composites the element at half alpha");
        Check(moved[2] == 255 && vacated[3] == 0,
              "translateX moves the drawing, not the layout box");
    }

    MotionRaster rotated;
    if (Render(R"({"width": 100, "height": 100, "fps": 25, "duration": 1,
      "root": {"type": "fill", "children": [
        {"type": "box", "style": {"position": "absolute", "left": 20, "top": 45, "width": 60, "height": 10,
                                  "backgroundColor": "#ffffff", "rotate": 90}}]}})",
               rotated, "rotation")) {
        Check(
            Pixel(rotated, 50, 25)[3] == 255 && Pixel(rotated, 25, 50)[3] == 0,
            "rotate turns the drawing around the centre of its box");
    }

    MotionRaster first;
    MotionRaster second;
    const std::string text = TitleCard(1080, 1920, 700, 58, 73.4, 72);
    if (Render(text, first, "determinism 1") &&
        Render(text, second, "determinism 2")) {
        Check(first.pixels == second.pixels,
              "rendering the same frame twice gives the same pixels");
    }
}

void TestPngAndImages() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-motion-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    MotionRaster square;
    square.width = 40;
    square.height = 20;
    square.pixels.assign(40 * 20 * 4, 0);
    for (size_t index = 0; index < square.pixels.size(); index += 4) {
        square.pixels[index] = 255;
        square.pixels[index + 3] = 255;
    }
    const std::string png = (directory / "square.png").string();
    MotionError error;
    Check(WriteMotionPng(square, png, error),
          "a PNG is written: " + error.message);
    Check(!WriteMotionPng(square, png, error) && error.code == "IoError",
          "an existing PNG is never overwritten");
    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    Check(WriteMotionPng(square, "relative.png", error),
          "a bare filename writes to the current directory");
    std::filesystem::current_path(previous);
    const std::string invalid = (directory / "invalid.png").string();
    MotionRaster truncated = square;
    truncated.pixels.resize(4);
    Check(!WriteMotionPng(truncated, invalid, error) &&
              error.code == "InvalidValue" && !std::filesystem::exists(invalid),
          "an incomplete raster is refused before writing");
    const std::string dangling = (directory / "dangling.png").string();
    std::filesystem::create_symlink("absent.png", dangling);
    Check(!WriteMotionPng(square, dangling, error) &&
              !std::filesystem::exists(directory / "absent.png"),
          "a dangling symlink cannot redirect a frame write");

    MotionCoreTextMeasurer measurer(directory.string());
    double width = 0.0;
    double height = 0.0;
    Check(measurer.ImageSize("square.png", width, height, error) &&
              width == 40.0 && height == 20.0,
          "an image source resolves against the media directory");

    MotionFrame frame;
    if (EvaluateJson(R"({"width": 100, "height": 100, "fps": 25, "duration": 1,
          "root": {"type": "fill", "children": [
            {"type": "image", "src": "square.png", "style": {"position": "absolute", "left": 0, "top": 0,
                                                             "width": 100, "height": 100, "objectFit": "contain"}}]}})",
                     measurer, frame, "image")) {
        MotionRaster raster;
        if (RenderMotionFrame(frame, measurer, raster, error)) {
            Check(Pixel(raster, 50, 50)[0] == 255 &&
                      Pixel(raster, 50, 10)[3] == 0 &&
                      Pixel(raster, 50, 90)[3] == 0,
                  "contain letterboxes a wide image inside a square box");
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void TestCli() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-motion-cli-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    const std::string composition = (directory / "card.json").string();
    {
        std::ofstream output(composition);
        output << TitleCard(1080, 1920, 700, 58, 73.4, 72);
    }
    std::string output;
    Check(DescribeMotionFrameCommand({composition, "0", "--font-dir", kFonts},
                                     output) == 0 &&
              output.rfind("{\"width\":1080,\"height\":1920,\"frame\":0,", 0) ==
                  0 &&
              output.find("\"baseline\":") != std::string::npos &&
              output.find("\"words\":[{\"text\":\"COUREZ\"") !=
                  std::string::npos &&
              output.find("\"chars\":[{\"text\":\"C\"") != std::string::npos,
          "describe prints the evaluated geometry, words and characters "
          "included: " +
              output.substr(0, 120));
    const std::string image = (directory / "card-0.png").string();
    Check(RenderMotionFrameCommand(
              {composition, "0", image, "--font-dir", kFonts}, output) == 0 &&
              std::filesystem::exists(image) &&
              output.find("\"ok\":true") != std::string::npos,
          "render writes the PNG and reports it: " + output);
    Check(RenderMotionFrameCommand({composition, "0", image}, output) == 1 &&
              output.find("refusing to overwrite") != std::string::npos,
          "render refuses to overwrite a frame");
    // PRD39 -- ROADMAP.md. A command line refused exits with status 2, as
    // every command's does; these exited 1.
    Check(DescribeMotionFrameCommand({composition, "zéro"}, output) == 2 &&
              output.find("ValidationFailed") != std::string::npos,
          "a non-integer frame is refused as a command line");
    Check(DescribeMotionFrameCommand({composition, "0", "--speed", "2"},
                                     output) == 2 &&
              output.find("unknown --describe-motion-frame option '--speed'") !=
                  std::string::npos,
          "an unknown option is refused by name: " + output);
    Check(DescribeMotionFrameCommand({composition, "10"}, output) == 1 &&
              output.find("FrameOutOfRange") != std::string::npos,
          "a frame past the duration is refused");

    // M6 -- a version in a format, by name.
    const std::string versioned = (directory / "versions.json").string();
    {
        std::ofstream file(versioned);
        file << R"({"width": 1080, "height": 1920, "fps": 25, "duration": 10,
          "props": {"date": {"type": "text", "default": "1er mai"},
                    "corps": {"type": "number", "default": 60, "min": 10, "max": 300}},
          "formats": {"16x9": {"width": 1920, "height": 1080, "props": {"corps": 110}}},
          "variants": {"juin": {"props": {"date": "29 juin"}}},
          "root": {"type": "fill", "style": {"justifyContent": "center", "alignItems": "center"},
            "children": [{"type": "text", "content": {"prop": "date"},
              "style": {"fontFamily": "Fira Sans Condensed", "fontSize": {"prop": "corps"}}}]}})";
    }
    Check(DescribeMotionFrameCommand({versioned, "0", "--variant", "juin",
                                      "--format", "16x9", "--font-dir", kFonts},
                                     output) == 0 &&
              output.rfind("{\"width\":1920,\"height\":1080,", 0) == 0 &&
              output.find("\"text\":\"29 juin\"") != std::string::npos &&
              output.find("\"fontSize\":110") != std::string::npos,
          "--variant and --format describe that version in that format: " +
              output.substr(0, 160));
    Check(
        DescribeMotionFrameCommand({versioned, "0", "--variant", "juillet"},
                                   output) == 1 &&
            output.find("\"error\":\"UnknownVariant\"") != std::string::npos &&
            output.find("declared: juin") != std::string::npos,
        "an unknown version is refused by name: " + output);
    Check(RenderMotionFrameCommand(
              {versioned, "0", (directory / "carre.png").string(), "--format",
               "carre"},
              output) == 1 &&
              output.find("\"error\":\"UnknownFormat\"") != std::string::npos &&
              !std::filesystem::exists(directory / "carre.png"),
          "an unknown format is refused before anything is written: " + output);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// M1b -- units, paths, clips and outlines are drawn where the evaluated
// frame says they are: each check reads the exposed geometry, then looks for
// ink in the raster, the same comparison as the reference title card.
struct Ink {
    bool any = false;
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

// Pixels whose channel `channel` is above half, optionally only where
// another channel is low (to tell red from white).
Ink InkIn(const MotionRaster& raster, int x0, int y0, int x1, int y1,
          int channel = 0, int low = -1) {
    Ink ink;
    for (int y = std::max(0, y0); y < std::min(raster.height, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(raster.width, x1); ++x) {
            const uint8_t* pixel = Pixel(raster, x, y);
            if (pixel[channel] <= 128 || (low >= 0 && pixel[low] > 64)) {
                continue;
            }
            if (!ink.any) {
                ink = {true, x, x, y, y};
            }
            ink.left = std::min(ink.left, x);
            ink.right = std::max(ink.right, x);
            ink.top = std::min(ink.top, y);
            ink.bottom = std::max(ink.bottom, y);
        }
    }
    return ink;
}

Ink InkAll(const MotionRaster& raster, int channel = 0, int low = -1) {
    return InkIn(raster, 0, 0, raster.width, raster.height, channel, low);
}

std::string Words(const std::string& units, const std::string& extra) {
    return R"({"width": 900, "height": 200, "fps": 25, "duration": 20,
  "root": {"type": "fill", "style": {"backgroundColor": "#000", "padding": 40,
    "fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": 80, "color": "#fff"},
   "children": [{"type": "text", "content": "UN DEUX TROIS", "units": ")" +
           units + R"(")" + extra + "}]}}";
}

// MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The highlighter, drawn
// by Core Text's layout: under DEUX and TROIS, the space between them is
// red, the space before DEUX is not; at progress 0 nothing red is drawn.
void TestHighlightRendering() {
    const auto card = [](const std::string& progress) {
        return R"({"width": 900, "height": 200, "fps": 25, "duration": 20,
  "root": {"type": "fill", "style": {"backgroundColor": "#000", "padding": 40,
    "fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": 80, "color": "#fff"},
   "children": [{"type": "text", "content": "UN DEUX TROIS",
     "highlights": [{"unit": "word", "start": 1, "count": 2, "color": "#ff0000",
                     "progress": )" +
               progress + "}]}]}}";
    };
    MotionRaster raster;
    MotionFrame frame;
    if (!Render(card("1"), raster, "highlight", &frame)) {
        return;
    }
    const MotionEvaluatedNode& text = frame.root.children[0];
    Check(text.words.size() == 3 && text.highlights.size() == 1,
          "three words, one band");
    if (text.words.size() != 3 || text.highlights.size() != 1) {
        return;
    }
    const MotionHighlightBand& band = text.highlights[0];
    const auto near = [](double left, double right) {
        return std::fabs(left - right) < 1e-6;
    };
    const auto gap = [&](size_t before) {
        const MotionTextSpan& left = text.words[before];
        const MotionTextSpan& right = text.words[before + 1];
        return static_cast<int>(text.box.x +
                                (left.x + left.width + right.x) / 2.0);
    };
    const int middle =
        static_cast<int>(text.box.y + band.box.y + band.box.height / 2.0);
    const uint8_t* between = Pixel(raster, gap(1), middle);
    const uint8_t* before = Pixel(raster, gap(0), middle);
    Check(between[0] > 200 && between[1] < 60 && between[2] < 60,
          "the space between DEUX and TROIS is under the stroke");
    Check(before[0] < 60 && before[1] < 60 && before[2] < 60,
          "the space before DEUX is not");
    Check(near(band.box.x, text.words[1].x) &&
              near(band.box.x + band.box.width,
                   text.words[2].x + text.words[2].width) &&
              near(band.painted, band.box.width),
          "the band runs from DEUX's left to TROIS's right");
    MotionRaster none;
    if (Render(card("0"), none, "highlight at 0")) {
        bool red = false;
        for (int y = 0; y < none.height && !red; ++y) {
            for (int x = 0; x < none.width && !red; ++x) {
                const uint8_t* pixel = Pixel(none, x, y);
                red = pixel[0] > 200 && pixel[1] < 60;
            }
        }
        Check(!red, "at progress 0 nothing red is drawn");
    }
}

void TestUnitRendering() {
    const std::string cascade = Words(
        "word",
        R"(, "animate": {"opacity": {"interpolate": {"frames": [0, 1], "values": [0, 1]}}},
            "stagger": {"each": 2})");
    MotionCoreTextMeasurer measurer;
    MotionComposition composition;
    MotionError error;
    MotionFrame frame;
    MotionRaster raster;
    if (!ParseMotionComposition(cascade, composition, error) ||
        !EvaluateMotionFrame(composition, mcp_json::Value(), 3, measurer, frame,
                             error) ||
        !RenderMotionFrame(frame, measurer, raster, error)) {
        Check(false, "word cascade renders: " + error.message);
        return;
    }
    const MotionEvaluatedNode& text = frame.root.children[0];
    Check(text.words.size() == 3 && text.units.size() == 3,
          "the word cascade exposes three words");
    if (text.words.size() != 3) {
        return;
    }
    // At frame 3 the first two words are in, the third has not started.
    const auto box = [&](size_t index) {
        const MotionTextSpan& word = text.words[index];
        return std::make_pair(text.box.x + word.x, text.box.y + word.y);
    };
    const Ink all = InkAll(raster);
    const MotionTextSpan& second = text.words[1];
    const double secondRight = box(1).first + second.width;
    Check(all.any && std::abs(all.left - box(0).first) <= 8 &&
              all.right <= secondRight + 2 &&
              !InkIn(raster, static_cast<int>(box(2).first), 0, raster.width,
                     raster.height)
                   .any,
          "ink covers the visible words and stops before the third word's "
          "box (ink " +
              std::to_string(all.left) + "-" + std::to_string(all.right) +
              ", second word ends at " + std::to_string(secondRight) + ")");
    for (size_t index = 0; index < 2; ++index) {
        const MotionTextSpan& word = text.words[index];
        const Ink inside =
            InkIn(raster, static_cast<int>(box(index).first) - 2,
                  static_cast<int>(box(index).second),
                  static_cast<int>(box(index).first + word.width) + 2,
                  static_cast<int>(box(index).second + word.height));
        Check(inside.any && inside.left >= box(index).first - 2 &&
                  inside.right <= box(index).first + word.width + 2,
              "word " + std::to_string(index) +
                  " is drawn inside its exposed box");
    }

    // Characters: at frame 2 with one frame each, U and N are in (the space
    // is not a unit), D is not.
    MotionFrame chars;
    MotionRaster charRaster;
    MotionComposition charComposition;
    if (ParseMotionComposition(
            Words(
                "char",
                R"(, "animate": {"opacity": {"interpolate": {"frames": [0, 1], "values": [0, 1]}}},
                      "stagger": {"each": 1})"),
            charComposition, error) &&
        EvaluateMotionFrame(charComposition, mcp_json::Value(), 2, measurer,
                            chars, error) &&
        RenderMotionFrame(chars, measurer, charRaster, error)) {
        const MotionEvaluatedNode& node = chars.root.children[0];
        const MotionTextSpan& n = node.chars[1];
        const MotionTextSpan& d = node.chars[2];
        const Ink ink = InkAll(charRaster);
        Check(ink.any && ink.right <= node.box.x + n.x + n.width + 2 &&
                  ink.right > node.box.x + n.x &&
                  !InkIn(charRaster, static_cast<int>(node.box.x + d.x), 0,
                         charRaster.width, charRaster.height)
                       .any,
              "the character cascade has drawn U and N, not D");
    }

    // A range of one word in red: red ink only inside that word's box.
    MotionFrame ranged;
    MotionRaster rangeRaster;
    MotionComposition rangeComposition;
    if (ParseMotionComposition(
            Words(
                "word",
                R"(, "ranges": [{"unit": "word", "start": 1, "count": 1, "style": {"color": "#ff0000"}}])"),
            rangeComposition, error) &&
        EvaluateMotionFrame(rangeComposition, mcp_json::Value(), 0, measurer,
                            ranged, error) &&
        RenderMotionFrame(ranged, measurer, rangeRaster, error)) {
        const MotionEvaluatedNode& node = ranged.root.children[0];
        const MotionTextSpan& word = node.words[1];
        const Ink red = InkAll(rangeRaster, 0, 1);
        const Ink white =
            InkIn(rangeRaster, 0, 0, rangeRaster.width, rangeRaster.height, 1);
        const double left = node.box.x + word.x;
        const double right = left + word.width;
        Check(red.any && red.left >= left - 2 && red.right <= right + 2 &&
                  white.any &&
                  !InkIn(rangeRaster, static_cast<int>(left) + 2, 0,
                         static_cast<int>(right) - 2, rangeRaster.height, 1)
                       .any,
              "the ranged word is red inside its box, the rest white");
    }
}

void TestPathRendering() {
    const auto line = [](const std::string& trim) {
        return R"({"width": 200, "height": 100, "fps": 25, "duration": 1,
  "root": {"type": "fill", "children": [{"type": "path", "d": "M20 50 L180 50",
    "style": {"position": "absolute", "left": 0, "top": 0, "fill": "none", "stroke": "#ffffff",
              "strokeWidth": 10, "trimEnd": )" +
               trim + "}}]}}";
    };
    for (const char* trim : {"0", "0.5", "1"}) {
        MotionRaster raster;
        MotionFrame frame;
        if (!Render(line(trim), raster, std::string("trim ") + trim, &frame)) {
            continue;
        }
        const MotionPathPaint& paint = frame.root.children[0].path_paint;
        const Ink ink = InkAll(raster);
        if (!paint.has_bounds) {
            Check(!ink.any, std::string("trim ") + trim + " draws nothing");
            continue;
        }
        Check(ink.any && std::abs(ink.left - paint.bounds.x) <= 1 &&
                  std::abs(ink.right + 1 -
                           (paint.bounds.x + paint.bounds.width)) <= 1 &&
                  std::abs(ink.top - 45) <= 1 && std::abs(ink.bottom - 54) <= 1,
              std::string("trimEnd ") + trim +
                  " draws the stroke over the exposed bounds (ink " +
                  std::to_string(ink.left) + "-" + std::to_string(ink.right) +
                  ")");
    }

    MotionRaster clipped;
    if (Render(R"J({"width": 100, "height": 100, "fps": 25, "duration": 1,
      "root": {"type": "fill", "style": {"backgroundColor": "#ffffff",
        "clipPath": "polygon(0 0, 100% 0, 0 100%)"}}})J",
               clipped, "clip-path")) {
        Check(Pixel(clipped, 10, 10)[3] == 255 &&
                  Pixel(clipped, 40, 40)[3] == 255 &&
                  Pixel(clipped, 60, 60)[3] == 0 &&
                  Pixel(clipped, 90, 90)[3] == 0,
              "clip-path keeps the triangle above the diagonal");
    }

    MotionRaster repeated;
    MotionFrame copies;
    if (Render(R"({"width": 300, "height": 100, "fps": 25, "duration": 1,
      "root": {"type": "fill", "style": {"flexDirection": "row"}, "children": [
        {"type": "repeat", "count": 3, "cycle": {"backgroundColor": ["#ff0000", "#0000ff"]},
         "template": {"type": "box", "style": {"width": 100, "height": 100}}}]}})",
               repeated, "repeat", &copies)) {
        bool alternating = copies.root.children.size() == 3;
        for (size_t index = 0; alternating && index < 3; ++index) {
            const MotionBox& box = copies.root.children[index].box;
            const uint8_t* centre =
                Pixel(repeated, static_cast<int>(box.x + box.width / 2),
                      static_cast<int>(box.y + box.height / 2));
            alternating = index % 2 == 0 ? centre[0] == 255 && centre[2] == 0
                                         : centre[2] == 255 && centre[0] == 0;
        }
        Check(alternating,
              "three repeated boxes alternate red, blue, red where exposed");
    }
}

void TestTextOutline() {
    const auto letter = [](const std::string& paint) {
        return R"({"width": 300, "height": 300, "fps": 25, "duration": 1,
  "root": {"type": "fill", "style": {"backgroundColor": "#000", "alignItems": "center",
    "justifyContent": "center", "fontFamily": "Fira Sans Condensed", "fontWeight": 600,
    "fontSize": 240, )" +
               paint + R"(}, "children": [{"type": "text", "content": "I"}]}})";
    };
    MotionRaster filled;
    MotionRaster outlined;
    if (!Render(letter(R"("color": "#fff")"), filled, "filled I") ||
        !Render(letter(R"("color": "transparent", "textStroke": "4px #fff")"),
                outlined, "outlined I")) {
        return;
    }
    const Ink solid = InkAll(filled);
    const Ink outline = InkAll(outlined);
    const int middleX = (solid.left + solid.right) / 2;
    const int middleY = (solid.top + solid.bottom) / 2;
    Check(solid.any && outline.any &&
              Pixel(filled, middleX, middleY)[0] == 255 &&
              Pixel(outlined, middleX, middleY)[0] < 64 &&
              std::abs(outline.left - solid.left) <= 3 &&
              std::abs(outline.right - solid.right) <= 3 &&
              std::abs(outline.top - solid.top) <= 3 &&
              std::abs(outline.bottom - solid.bottom) <= 3,
          "textStroke outlines the glyph: its edges within 3 px of the "
          "filled glyph, its stem empty");
}

// Counter-test of M0 (ROADMAP.md, MOTION-2026-09): two pieces of the corpus,
// transposed with neutral texts and colours, must render from the format.
// The geometry each check needs is read from the evaluated frame.
bool RenderFixture(const std::string& name, int64_t index,
                   const std::string& props, MotionFrame& frame,
                   MotionRaster& raster) {
    std::ifstream input(std::string(MOTION_FIXTURE_DIR) + "/" + name);
    std::stringstream json;
    json << input.rdbuf();
    mcp_json::Value overrides;
    std::string message;
    if (!props.empty()) {
        mcp_json::Value::Parse(props, overrides, message);
    }
    MotionCoreTextMeasurer measurer;
    MotionComposition composition;
    MotionError error;
    const bool ok = ParseMotionComposition(json.str(), composition, error) &&
                    EvaluateMotionFrame(composition, overrides, index, measurer,
                                        frame, error) &&
                    RenderMotionFrame(frame, measurer, raster, error);
    Check(ok, name + " frame " + std::to_string(index) + ": " + error.code +
                  " at " + error.path + " -- " + error.message);
    return ok;
}

const MotionEvaluatedNode* Node(const MotionEvaluatedNode& node,
                                const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = Node(child, id)) {
            return found;
        }
    }
    return nullptr;
}

bool ColorNear(const uint8_t* pixel, int red, int green, int blue) {
    return std::abs(pixel[0] - red) <= 2 && std::abs(pixel[1] - green) <= 2 &&
           std::abs(pixel[2] - blue) <= 2 && pixel[3] == 255;
}

void TestCounterPieces() {
    MotionFrame credit;
    MotionRaster creditRaster;
    if (RenderFixture("credit-palette.json", 30, "", credit, creditRaster)) {
        const MotionEvaluatedNode* name = Node(credit.root, "nom");
        const MotionEvaluatedNode* role = Node(credit.root, "fonction");
        Check(
            name != nullptr && role != nullptr &&
                std::fabs(role->box.x - name->box.x - 28.0) < 1e-6 &&
                ColorNear(Pixel(creditRaster, static_cast<int>(name->box.x) + 3,
                                static_cast<int>(name->box.y) + 3),
                          0x4D, 0xAB, 0xA1) &&
                ColorNear(Pixel(creditRaster, static_cast<int>(role->box.x) + 3,
                                static_cast<int>(role->box.y) + 3),
                          255, 255, 255),
            "the credit's boxes are painted in the palette colour and "
            "white where the frame exposes them");
        if (name != nullptr) {
            const MotionEvaluatedNode& text = name->children[0];
            const Ink ink =
                InkIn(creditRaster, static_cast<int>(name->box.x),
                      static_cast<int>(name->box.y),
                      static_cast<int>(name->box.x + name->box.width),
                      static_cast<int>(name->box.y + name->box.height));
            Check(ink.any && ink.left >= text.box.x - 2 &&
                      ink.right <= text.box.x + text.box.width + 2,
                  "the name is drawn inside its measured box, inside the "
                  "box it sizes");
        }
    }

    MotionFrame start;
    MotionRaster startRaster;
    if (RenderFixture("question-plein-ecran.json", 0, "", start, startRaster)) {
        Check(!InkAll(startRaster, 0, 1).any,
              "the question's wipe starts off screen");
    }
    MotionFrame full;
    MotionRaster fullRaster;
    if (RenderFixture("question-plein-ecran.json", 30, "", full, fullRaster)) {
        const bool covered =
            ColorNear(Pixel(fullRaster, 2, 2), 0xE5, 0x39, 0x35) &&
            ColorNear(Pixel(fullRaster, 1917, 2), 0xE5, 0x39, 0x35) &&
            ColorNear(Pixel(fullRaster, 2, 1077), 0xE5, 0x39, 0x35) &&
            ColorNear(Pixel(fullRaster, 1917, 1077), 0xE5, 0x39, 0x35);
        const MotionEvaluatedNode* question = Node(full.root, "question");
        bool allIn = question != nullptr && question->units.size() == 11;
        for (size_t index = 0; allIn && index < question->units.size();
             ++index) {
            allIn = question->units[index].opacity == 1.0;
        }
        Check(covered && allIn,
              "at frame 30 the colour covers the frame and every word is in");
    }
    MotionFrame entering;
    MotionRaster enteringRaster;
    if (RenderFixture("question-plein-ecran.json", 9, "", entering,
                      enteringRaster)) {
        const MotionEvaluatedNode* wipe = Node(entering.root, "volet");
        const MotionEvaluatedNode* question = Node(entering.root, "question");
        bool clipped = wipe != nullptr && wipe->has_clip &&
                       wipe->clip.size() == 4 && question != nullptr;
        for (int y = 0; clipped && y < 1080; y += 4) {
            // Left edge of the colour at this row, from the exposed polygon;
            // the white border runs 10 px to its left.
            const double t = y / 1080.0;
            const double edge = wipe->clip[0].first +
                                (wipe->clip[3].first - wipe->clip[0].first) * t;
            const Ink beyond = InkIn(enteringRaster, 0, y,
                                     static_cast<int>(edge) - 14, y + 1, 1);
            clipped = !beyond.any;
        }
        Check(clipped && question->units.back().opacity == 0.0 &&
                  question->units.front().opacity == 1.0,
              "while the wipe enters, words cascade and nothing is drawn "
              "left of the exposed polygon");
    }
}

// REMOTION-PARITY-2026-09 REM1 -- the portable parity suite fixes exact
// descriptors. This half crosses the platform boundary: every current-corpus
// scene is evaluated with Core Text, painted with Core Graphics, and painted
// a second time byte-for-byte identically. No browser or Remotion runtime is
// involved.
bool LoadParityFixture(const std::string& name,
                       MotionComposition& composition) {
    std::ifstream input(std::string(MOTION_FIXTURE_DIR) + "/parity/" + name);
    std::stringstream json;
    json << input.rdbuf();
    MotionError error;
    const bool ok = input.good() || input.eof();
    const bool parsed =
        ok && ParseMotionComposition(json.str(), composition, error);
    Check(parsed, "parity fixture " + name + ": " + error.code + " at " +
                      error.path + " -- " + error.message);
    return parsed;
}

bool RenderParityFrame(const MotionComposition& composition,
                       const mcp_json::Value& props, int64_t index,
                       MotionCoreTextMeasurer& measurer, MotionRaster& raster,
                       const std::string& label) {
    MotionFrame frame;
    MotionError error;
    const bool ok = EvaluateMotionFrame(composition, props, index, measurer,
                                        frame, error) &&
                    RenderMotionFrame(frame, measurer, raster, error);
    Check(ok, label + ": " + error.code + " at " + error.path + " -- " +
                  error.message);
    return ok;
}

void TestParityCorpus() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-motion-parity-" + GenerateUlid());
    std::filesystem::create_directories(directory);

    // A 2:1 red mark with a transparent left half. It distinguishes source
    // alpha from a merely transparent node, and makes contain/cover crops
    // observable without committing a binary fixture.
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
    MotionError error;
    Check(WriteMotionPng(logo, (directory / "logo.png").string(), error),
          "C08 synthetic alpha logo is written: " + error.message);
    MotionCoreTextMeasurer measurer(directory.string());

    struct Scene {
        const char* fixture;
        std::vector<int64_t> controls;
        bool changes;
    };
    const Scene scenes[] = {
        {"c01-centered-card.json", {0, 24, 49}, false},
        {"c02-staggered-credit.json", {0, 4, 12, 30}, true},
        {"c03-word-cascade.json", {0, 4, 8}, true},
        {"c04-series-timescale.json", {0, 9, 18}, true},
        {"c05-repeat-palette.json", {0, 2, 6}, true},
        {"c06-path-trim.json", {0, 10, 20}, true},
        {"c07-polygon-clip.json", {0, 5, 10}, true},
        {"c08-logo-fit.json", {0, 12, 24}, false},
        {"c09-font-control.json", {0, 12, 24}, false},
        {"c10-variants-formats.json", {0, 24, 49}, false},
    };
    for (const Scene& scene : scenes) {
        MotionComposition composition;
        if (!LoadParityFixture(scene.fixture, composition)) {
            continue;
        }
        std::vector<MotionRaster> controls;
        for (int64_t index : scene.controls) {
            MotionRaster raster;
            if (RenderParityFrame(composition, mcp_json::Value(), index,
                                  measurer, raster,
                                  std::string(scene.fixture) + " frame " +
                                      std::to_string(index))) {
                controls.push_back(std::move(raster));
            }
        }
        if (controls.size() != scene.controls.size()) {
            continue;
        }
        MotionRaster repeated;
        const size_t witness = scene.controls.size() / 2;
        if (RenderParityFrame(composition, mcp_json::Value(),
                              scene.controls[witness], measurer, repeated,
                              std::string(scene.fixture) + " repeated")) {
            Check(controls[witness].pixels == repeated.pixels,
                  std::string(scene.fixture) +
                      " paints identical bytes on two successive passes");
        }
        if (scene.changes) {
            Check(controls.front().pixels != controls.back().pixels,
                  std::string(scene.fixture) +
                      " changes pixels across its control event");
        }
    }

    MotionComposition imageComposition;
    if (LoadParityFixture("c08-logo-fit.json", imageComposition)) {
        MotionRaster raster;
        if (RenderParityFrame(imageComposition, mcp_json::Value(), 0, measurer,
                              raster, "C08 alpha witness")) {
            Check(Pixel(raster, 100, 225)[3] == 0 &&
                      Pixel(raster, 250, 225)[0] == 255 &&
                      Pixel(raster, 250, 225)[3] == 255,
                  "C08 contain preserves transparent and opaque source pixels");
            Check(Pixel(raster, 520, 225)[3] == 0 &&
                      std::abs(Pixel(raster, 650, 225)[0] - 128) <= 1 &&
                      std::abs(Pixel(raster, 650, 225)[3] - 128) <= 1,
                  "C08 cover crops the same alpha source and applies 50% "
                  "opacity");
        }
    }

    MotionComposition centered;
    if (LoadParityFixture("c01-centered-card.json", centered)) {
        for (const char* format : {"9x16", "16x9"}) {
            MotionComposition resolved;
            mcp_json::Value props;
            MotionRaster raster;
            if (ResolveMotionVariant(centered, "", format, mcp_json::Value(),
                                     resolved, props, error) &&
                RenderParityFrame(resolved, props, 24, measurer, raster,
                                  std::string("C01 ") + format)) {
                Check((std::string(format) == "9x16" && raster.width == 1080 &&
                       raster.height == 1920) ||
                          (std::string(format) == "16x9" &&
                           raster.width == 1920 && raster.height == 1080),
                      std::string("C01 paints the declared ") + format +
                          " canvas");
            } else {
                Check(false, std::string("C01 resolves ") + format + ": " +
                                 error.message);
            }
        }
    }

    MotionComposition deliveries;
    if (LoadParityFixture("c10-variants-formats.json", deliveries)) {
        int rendered = 0;
        for (const char* variant : {"alpha", "beta", "gamma"}) {
            for (const char* format : {"9x16", "16x9", "1x1"}) {
                MotionComposition resolved;
                mcp_json::Value props;
                MotionRaster raster;
                if (ResolveMotionVariant(deliveries, variant, format,
                                         mcp_json::Value(), resolved, props,
                                         error) &&
                    RenderParityFrame(
                        resolved, props, 24, measurer, raster,
                        std::string("C10 ") + variant + " x " + format)) {
                    ++rendered;
                } else {
                    Check(false, std::string("C10 resolves ") + variant +
                                     " x " + format + ": " + error.message);
                }
            }
        }
        Check(rendered == 9, "C10 paints all nine version-format deliveries");
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// ---------------------------------------------------------------------------
// M6 -- ROADMAP.md. The ticket's validation: three versions x two formats
// from one composition of a project, six files, each read back at one frame
// whose ink is compared with what --describe-motion-frame says of the same
// version in the same format; an unknown version refused. The composition
// moves its date five pixels a frame, so a file off by one frame fails.

const std::string kFfmpeg = FFMPEG_EXECUTABLE;

std::string FfprobePath() {
    const std::string name = "ffmpeg";
    if (kFfmpeg.size() >= name.size() &&
        kFfmpeg.compare(kFfmpeg.size() - name.size(), name.size(), name) == 0) {
        return kFfmpeg.substr(0, kFfmpeg.size() - name.size()) + "ffprobe";
    }
    return "ffprobe";
}

std::string Quote(const std::string& text) { return "'" + text + "'"; }

std::string Capture(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
        output.append(buffer, count);
    }
    pclose(pipe);
    return output;
}

// Frame `index` of a video, decoded to RGBA with the BT.709 matrix the
// encoder was told about.
bool DecodeFrame(const std::string& video, int64_t index, int width, int height,
                 MotionRaster& raster) {
    const std::string raw = video + ".rgba";
    const std::string command =
        Quote(kFfmpeg) + " -hide_banner -loglevel error -y -i " + Quote(video) +
        " -vf " +
        Quote("select=eq(n\\," + std::to_string(index) +
              "),scale=in_color_matrix=bt709:in_range=tv,format=rgba") +
        " -frames:v 1 -f rawvideo " + Quote(raw) + " 2>&1";
    const std::string errors = Capture(command);
    std::ifstream input(raw, std::ios::binary);
    raster.width = width;
    raster.height = height;
    raster.pixels.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
    std::error_code ignored;
    std::filesystem::remove(raw, ignored);
    const bool ok = raster.pixels.size() == static_cast<size_t>(width) *
                                                static_cast<size_t>(height) * 4;
    Check(ok, "frame " + std::to_string(index) + " of " + video +
                  " decodes: " + errors);
    return ok;
}

// Ink of premultiplied red above half, by bands of rows: straight alpha
// would otherwise count faint antialiased edges as full ink.
std::vector<Band> PremultipliedBands(const MotionRaster& raster) {
    MotionRaster premultiplied = raster;
    for (size_t at = 0; at + 3 < premultiplied.pixels.size(); at += 4) {
        premultiplied.pixels[at] = static_cast<uint8_t>(
            (premultiplied.pixels[at] * premultiplied.pixels[at + 3] + 127) /
            255);
    }
    return Bands(premultiplied);
}

double Number(const mcp_json::Value* value) {
    double number = 0.0;
    if (value != nullptr) {
        MotionNumber(*value, number);
    }
    return number;
}

struct DescribedLine {
    std::string text;
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double baseline = 0.0;
};

// Text lines of a described frame, in frame pixels, moved by the
// translations of their node and its ancestors.
void CollectLines(const mcp_json::Value& node, double dx, double dy,
                  std::vector<DescribedLine>& lines) {
    if (const mcp_json::Value* transform = node.Find("transform")) {
        dx += Number(transform->Find("translateX"));
        dy += Number(transform->Find("translateY"));
    }
    const mcp_json::Value* box = node.Find("box");
    const double x =
        box != nullptr && box->IsArray() && box->AsArray().size() == 4
            ? Number(&box->AsArray()[0])
            : 0.0;
    const double y =
        box != nullptr && box->IsArray() && box->AsArray().size() == 4
            ? Number(&box->AsArray()[1])
            : 0.0;
    const mcp_json::Value* text = node.Find("text");
    if (const mcp_json::Value* laid =
            text != nullptr ? text->Find("lines") : nullptr) {
        for (const mcp_json::Value& line : laid->AsArray()) {
            const mcp_json::Value* lineBox = line.Find("box");
            if (lineBox == nullptr || lineBox->AsArray().size() != 4) {
                continue;
            }
            const auto& edges = lineBox->AsArray();
            DescribedLine described;
            described.text = line.Find("text")->AsString();
            described.left = x + dx + Number(&edges[0]);
            described.top = y + dy + Number(&edges[1]);
            described.right = described.left + Number(&edges[2]);
            described.bottom = described.top + Number(&edges[3]);
            described.baseline = y + dy + Number(line.Find("baseline"));
            lines.push_back(described);
        }
    }
    if (const mcp_json::Value* children = node.Find("children")) {
        for (const mcp_json::Value& child : children->AsArray()) {
            CollectLines(child, dx, dy, lines);
        }
    }
}

// Every band of ink sits in its described line and on its baseline, and
// spans most of it: the wrong version, the wrong format or the wrong frame
// each move one of these.
void CompareWithDescription(const std::string& label,
                            const MotionRaster& raster,
                            const std::string& described) {
    mcp_json::Value frame;
    std::string error;
    if (!mcp_json::Value::Parse(described, frame, error)) {
        Check(false, label + ": the description parses: " + error);
        return;
    }
    std::vector<DescribedLine> lines;
    CollectLines(*frame.Find("root"), 0.0, 0.0, lines);
    const std::vector<Band> bands = PremultipliedBands(raster);
    std::string measured;
    for (const Band& band : bands) {
        measured +=
            " " + std::to_string(band.top) + "-" + std::to_string(band.bottom) +
            " x" + std::to_string(band.left) + "-" + std::to_string(band.right);
    }
    if (bands.size() != lines.size()) {
        Check(false, label + ": " + std::to_string(bands.size()) +
                         " bands of ink for " + std::to_string(lines.size()) +
                         " described lines:" + measured);
        return;
    }
    for (size_t index = 0; index < lines.size(); ++index) {
        const Band& band = bands[index];
        const DescribedLine& line = lines[index];
        const double width = band.right - band.left + 1;
        Check(std::abs(band.bottom + 1 - line.baseline) <= 2.0 &&
                  band.top >= line.top - 2.0 && band.left >= line.left - 2.0 &&
                  band.right + 1 <= line.right + 2.0 &&
                  width >= 0.8 * (line.right - line.left),
              label + ": line '" + line.text + "' [" +
                  std::to_string(line.left) + ", " + std::to_string(line.top) +
                  ", " + std::to_string(line.right) + ", " +
                  std::to_string(line.bottom) + "] baseline " +
                  std::to_string(line.baseline) + " holds its ink:" + measured);
    }
}

bool FileCount(const std::filesystem::path& directory, size_t& count) {
    count = 0;
    std::error_code status;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, status)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return !status;
}

void TestVersionsAndFormatsVideo() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-motion-video-" + GenerateUlid());
    std::filesystem::create_directories(directory / "out");
    const std::filesystem::path out = directory / "out";

    // One composition, stored in a project.
    std::ifstream fixture(std::string(MOTION_FIXTURE_DIR) +
                          "/carte-versions.json");
    std::stringstream json;
    json << fixture.rdbuf();
    mcp_json::Value definition;
    std::string message;
    Check(mcp_json::Value::Parse(json.str(), definition, message),
          "the versioned card parses: " + message);
    Document document;
    document.sequence.id = "01K90000000000000000000001";
    document.sources = {
        {"01K90000000000000000000002", "rush.mov", {25, 1}, {1000, 25}}};
    document.sequence.tracks = {{"01K90000000000000000000003",
                                 "video",
                                 0,
                                 {{"01K90000000000000000000004",
                                   "01K90000000000000000000002",
                                   {0, 25},
                                   {50, 25},
                                   {0, 25}}}}};
    const Ulid id = "01K90000000000000000000010";
    document.compositions = {
        {id, "Carte déclinée", WithDefinitionIdentities(definition)}};
    std::string project;
    if (!CreatePortableProject((directory / "Versions").string(),
                               Project::FromDocument(document, "Versions"),
                               project, message)) {
        Check(false, "the project saves: " + message);
        return;
    }
    const std::string pattern = (out / "carte_{variant}_{format}.mov").string();
    const std::vector<std::string> common = {"--composition", id, "--font-dir",
                                             kFonts};
    const auto command = [&](std::vector<std::string> arguments,
                             std::string& output) {
        arguments.insert(arguments.end(), common.begin(), common.end());
        return RenderMotionVideoCommand(arguments, output);
    };
    size_t files = 0;

    // Refused before anything is written.
    std::string output;
    Check(
        command({project, pattern, "--alpha", "--variants", "avril,juin"},
                output) == 1 &&
            output.find("\"error\":\"UnknownVariant\"") != std::string::npos &&
            output.find("declared: avril, mai, mars") != std::string::npos &&
            FileCount(out, files) && files == 0,
        "an unknown version is refused by name, nothing written: " + output);
    Check(command({project, pattern, "--alpha", "--formats", "carre"},
                  output) == 1 &&
              output.find("\"error\":\"UnknownFormat\"") != std::string::npos,
          "an unknown format is refused by name: " + output);
    Check(command({project, (out / "carte_{variant}.mov").string(), "--alpha"},
                  output) == 1 &&
              output.find("\"error\":\"InvalidExport\"") != std::string::npos &&
              output.find("written twice") != std::string::npos &&
              FileCount(out, files) && files == 0,
          "an output that does not name the format is refused: " + output);
    Check(command({project, (out / "carte_{variant}_{format}.mp4").string(),
                   "--alpha"},
                  output) == 1 &&
              output.find(".mov") != std::string::npos,
          "alpha is refused outside a .mov: " + output);
    {
        std::ofstream existing(out / "carte_mars_16x9.mov");
        existing << "not a video";
    }
    Check(command({project, pattern, "--alpha"}, output) == 1 &&
              output.find("already exists") != std::string::npos &&
              FileCount(out, files) && files == 1,
          "a destination that exists stops the batch before the first "
          "encode: " +
              output);
    std::filesystem::remove(out / "carte_mars_16x9.mov");

    // Preview: the plan, and nothing written.
    Check(command({project, pattern, "--alpha", "--preview"}, output) == 0 &&
              output.find("\"ok\":true,\"preview\":true,\"total\":6,"
                          "\"completed\":0") != std::string::npos &&
              output.find("\"status\":\"planned\"") != std::string::npos &&
              FileCount(out, files) && files == 0,
          "a preview plans six files and writes none: " + output);

    // Three versions x two formats, ProRes 4444 with alpha.
    const auto started = std::chrono::steady_clock::now();
    const int status = command({project, pattern, "--alpha"}, output);
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    Check(
        status == 0 && output.find("\"ok\":true,\"preview\":false,\"total\":6,"
                                   "\"completed\":6") != std::string::npos,
        "six files are rendered and reported: " + output);
    std::cout << "M6: 3 versions x 2 formats, 20 frames each, ProRes 4444, "
              << seconds << " s\n";
    mcp_json::Value report;
    mcp_json::Value::Parse(output, report, message);
    const mcp_json::Value* outputs = report.Find("outputs");
    Check(outputs != nullptr && outputs->IsArray() &&
              outputs->AsArray().size() == 6,
          "the report lists the six files");
    const char* const variants[] = {"avril", "mai", "mars"};
    const char* const formats[] = {"9x16", "16x9"};
    const int64_t readFrame = 7;
    size_t item = 0;
    for (const char* variant : variants) {
        for (const char* format : formats) {
            const std::string label =
                std::string(variant) + " in " + std::string(format);
            const std::string file = (out / ("carte_" + std::string(variant) +
                                             "_" + format + ".mov"))
                                         .string();
            const bool tall = std::string(format) == "9x16";
            const int width = tall ? 1080 : 1920;
            const int height = tall ? 1920 : 1080;
            if (outputs != nullptr && item < outputs->AsArray().size()) {
                const mcp_json::Value& line = outputs->AsArray()[item];
                Check(line.Find("variant")->AsString() == variant &&
                          line.Find("format")->AsString() == format &&
                          line.Find("path")->AsString() == file &&
                          line.Find("status")->AsString() == "complete",
                      label + ": the report names its file in order: " +
                          line.Dump());
            }
            ++item;
            const std::string probe = Capture(
                Quote(FfprobePath()) +
                " -v error -select_streams v:0 -show_entries "
                "stream=codec_name,profile,pix_fmt,width,height,nb_frames,"
                "r_frame_rate,color_space -of compact=p=0 " +
                Quote(file));
            Check(probe.find("codec_name=prores") != std::string::npos &&
                      probe.find("profile=4444") != std::string::npos &&
                      probe.find("pix_fmt=yuva444") != std::string::npos &&
                      probe.find("width=" + std::to_string(width) + "|") !=
                          std::string::npos &&
                      probe.find("height=" + std::to_string(height) + "|") !=
                          std::string::npos &&
                      probe.find("r_frame_rate=25/1") != std::string::npos &&
                      probe.find("nb_frames=20") != std::string::npos &&
                      probe.find("color_space=bt709") != std::string::npos,
                  label +
                      ": ProRes 4444 with alpha, the format's size, 20 "
                      "frames at 25/1: " +
                      probe);
            MotionRaster decoded;
            std::string described;
            if (!DecodeFrame(file, readFrame, width, height, decoded) ||
                DescribeMotionFrameCommand(
                    {project, std::to_string(readFrame), "--composition", id,
                     "--variant", variant, "--format", format, "--font-dir",
                     kFonts},
                    described) != 0) {
                Check(false,
                      label + ": frame read and described: " + described);
                continue;
            }
            // The description itself says the version's date at the
            // format's type size, so a resolution both sides shared wrongly
            // would still fail here.
            const std::string date =
                std::string(variant) == "avril" ? "1er avril"
                : std::string(variant) == "mai" ? "27 mai"
                                                : "22 mars 2031";
            Check(described.find("\"content\":\"" + date + "\"") !=
                          std::string::npos &&
                      described.find(std::string("\"fontSize\":") +
                                     (tall ? "72," : "110,")) !=
                          std::string::npos,
                  label + ": the version's date at the format's size");
            CompareWithDescription(label, decoded, described);
            uint8_t opaque = 0;
            for (size_t at = 3; at < decoded.pixels.size(); at += 4) {
                opaque = std::max(opaque, decoded.pixels[at]);
            }
            Check(Pixel(decoded, 0, 0)[3] == 0 && opaque == 255,
                  label + ": transparent around opaque text");
        }
    }

    // Without alpha: H.264 over black, the timeline delivery's codec.
    const std::string mp4 = (out / "carte_mai_16x9.mp4").string();
    Check(command({project, (out / "carte_{variant}_{format}.mp4").string(),
                   "--variants", "mai", "--formats", "16x9"},
                  output) == 0 &&
              output.find("\"codec\":\"h264\"") != std::string::npos,
          "one version in one format, as H.264: " + output);
    const std::string probe = Capture(
        Quote(FfprobePath()) +
        " -v error -select_streams v:0 -show_entries "
        "stream=codec_name,pix_fmt,width,height,nb_frames,r_frame_rate -of "
        "compact=p=0 " +
        Quote(mp4));
    Check(probe.find("codec_name=h264") != std::string::npos &&
              probe.find("pix_fmt=yuv420p") != std::string::npos &&
              probe.find("nb_frames=20") != std::string::npos,
          "H.264 4:2:0, 20 frames: " + probe);
    MotionRaster flat;
    std::string described;
    if (DecodeFrame(mp4, readFrame, 1920, 1080, flat) &&
        DescribeMotionFrameCommand(
            {project, std::to_string(readFrame), "--composition", id,
             "--variant", "mai", "--format", "16x9", "--font-dir", kFonts},
            described) == 0) {
        CompareWithDescription("mai in 16x9, H.264", flat, described);
        Check(Pixel(flat, 0, 0)[0] < 20 && Pixel(flat, 0, 0)[3] == 255,
              "the H.264 card is flattened over black");
    }
    Check(command({project, (out / "carte_{variant}_{format}.mp4").string(),
                   "--variants", "mai", "--formats", "16x9"},
                  output) == 1 &&
              output.find("already exists") != std::string::npos,
          "rendering again is refused without --overwrite");
    Check(command({project, (out / "carte_{variant}_{format}.mp4").string(),
                   "--variants", "mai", "--formats", "16x9", "--overwrite"},
                  output) == 0,
          "and replaces the file with it: " + output);
    size_t leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(out)) {
        if (entry.path().string().find(".part") != std::string::npos) {
            ++leftovers;
        }
    }
    Check(FileCount(out, files) && files == 7 && leftovers == 0,
          "seven files and no temporary one are left");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// Principle 9: measured before any real-time preview is promised. Printed,
// not asserted -- timing belongs to the machine, not to correctness.
void MeasureCost() {
    for (const auto& format :
         {std::make_pair(1080, 1920), std::make_pair(1920, 1080)}) {
        const std::string json = TitleCard(format.first, format.second,
                                           format.second * 0.35, 58, 73.4, 72);
        MotionComposition composition;
        MotionError error;
        if (!ParseMotionComposition(json, composition, error)) {
            continue;
        }
        MotionCoreTextMeasurer measurer;
        std::vector<double> evaluate;
        std::vector<double> render;
        for (int run = 0; run < 25; ++run) {
            MotionFrame frame;
            MotionRaster raster;
            const auto start = std::chrono::steady_clock::now();
            EvaluateMotionFrame(composition, mcp_json::Value(), 0, measurer,
                                frame, error);
            const auto evaluated = std::chrono::steady_clock::now();
            RenderMotionFrame(frame, measurer, raster, error);
            const auto rendered = std::chrono::steady_clock::now();
            evaluate.push_back(
                std::chrono::duration<double, std::milli>(evaluated - start)
                    .count());
            render.push_back(
                std::chrono::duration<double, std::milli>(rendered - evaluated)
                    .count());
        }
        std::sort(evaluate.begin(), evaluate.end());
        std::sort(render.begin(), render.end());
        std::printf(
            "cost %dx%d: evaluate median %.2f ms, render median %.2f ms\n",
            format.first, format.second, evaluate[12], render[12]);
    }
}

}  // namespace

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. The humanize pass proved as
// the tools prove it: every frame of both definitions rendered with the real
// fonts. A timecode of digit strips (Menlo, as the v6 wrote it), a box that
// only placed a text, and letters placed by the engine's own advances fold
// without a pixel changing; a letter moved a third of a pixel is seen.
void TestHumanizeProof() {
    InstallMotionFrameRenderer();
    const std::string fira =
        R"("fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": 40)";
    // Where the engine puts the letters of one word.
    MotionComposition measured;
    MotionError error;
    MotionCoreTextMeasurer measurer;
    MotionFrame frame;
    const std::string word =
        R"({"width": 400, "height": 200, "fps": 25, "duration": 1, "root":
            {"type": "fill", "children": [{"type": "text", "content": "COUPER",
             "style": {)" +
        fira + "}}]}}";
    if (!ParseMotionComposition(word, measured, error) ||
        !EvaluateMotionFrame(measured, mcp_json::Value(), 0, measurer, {},
                             frame, error)) {
        Check(false, "the word is measured: " + error.message);
        return;
    }
    const MotionEvaluatedNode& text = frame.root.children[0];
    const auto letters = [&](double moved) {
        std::string list;
        for (size_t index = 0; index < text.chars.size(); ++index) {
            char left[64];
            std::snprintf(
                left, sizeof(left), "%.17g",
                60.0 + text.chars[index].x + (index == 3 ? moved : 0.0));
            list +=
                std::string(index > 0 ? "," : "") +
                R"({"type": "text", "content": ")" + text.chars[index].text +
                R"(", "style": {"position": "absolute", "left": )" + left +
                R"(, "top": 80, )" + fira +
                R"(}, "animate": {"opacity": {"interpolate": {"frames": [)" +
                std::to_string(index * 2) + "," +
                std::to_string(index * 2 + 1) +
                R"(], "values": [0, 1], "easing": "step-end"}}}})";
        }
        return list;
    };
    const auto card = [&](double moved) {
        return R"({"width": 400, "height": 200, "fps": 25, "duration": 14,
            "root": {"type": "fill", "style": {"backgroundColor": "#1b1b1b",
              "color": "#ffffff"}, "children": [
              {"type": "sequence", "from": 0, "duration": 14, "children": [)" +
               TimecodeRow(0, 14, 11, -1,
                           R"("style": {"fontFamily": "Menlo",
                              "fontSize": 18, "lineHeight": 1.0})") +
               R"(,
                {"type": "box", "style": {"position": "absolute", "left": 40,
                  "top": 20}, "animate": {"opacity": {"interpolate":
                  {"frames": [0, 6], "values": [0, 1]}}},
                 "children": [{"type": "text", "content": "Titre",
                   "style": {)" +
               fira + "}}]}," + letters(moved) + "]}]}}";
    };
    mcp_json::Value written;
    std::string parseError;
    if (!mcp_json::Value::Parse(card(0.0), written, parseError)) {
        Check(false, "the card parses: " + parseError);
        return;
    }
    mcp_json::Value human;
    MotionHumanizeReport report;
    MotionComposition before;
    MotionComposition after;
    if (!HumanizeMotionDefinition(written, measurer, {}, human, report,
                                  error) ||
        !ParseMotionComposition(written, before, error) ||
        !ParseMotionComposition(human, after, error)) {
        Check(false, "the card humanizes: " + error.path + " " + error.message);
        return;
    }
    Check(report.timecodes == 1 && report.wrappers == 1 && report.words == 1 &&
              report.letters == 6,
          "a timecode, a wrapper and a word of six letters fold: " +
              std::to_string(report.timecodes) + " " +
              std::to_string(report.wrappers) + " " +
              std::to_string(report.words) + " " +
              std::to_string(report.letters));
    std::vector<int64_t> differing;
    Check(CompareMotionRenders(MotionFrameRequest{}, before, after, differing,
                               error) &&
              differing.empty(),
          "rendered, the fourteen frames of both are the same pixels: " +
              std::to_string(differing.size()) + " differ " + error.message);

    MotionComposition moved;
    if (!ParseMotionComposition(card(0.3), moved, error)) {
        Check(false, "the moved card parses: " + error.message);
        return;
    }
    Check(CompareMotionRenders(MotionFrameRequest{}, before, moved, differing,
                               error) &&
              !differing.empty() && differing.front() == 7,
          "a letter a third of a pixel away is seen from the frame it shows: " +
              (differing.empty() ? std::string("none")
                                 : std::to_string(differing.front())));
}

int main() {
    MotionError error;
    if (!RegisterMotionFontDirectory(kFonts, error)) {
        std::cerr << "FAIL: fixture fonts: " << error.message << '\n';
        return 1;
    }
    TestFontResolution();
    TestVariableFontWeight();
    TestTextModel();
    TestReferenceTitleCards();
    TestDrawing();
    TestPngAndImages();
    TestCli();
    TestUnitRendering();
    TestHighlightRendering();
    TestPathRendering();
    TestTextOutline();
    TestCounterPieces();
    TestParityCorpus();
    TestVersionsAndFormatsVideo();
    TestHumanizeProof();
    MeasureCost();
    if (failures != 0) {
        std::cerr << failures << " motion render assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion render\n";
    return 0;
}
