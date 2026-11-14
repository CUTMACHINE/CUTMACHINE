// PA8 -- docs/roadmap/PARITE-2026-11.md. A clip's mask: the rule (the
// coverage at a point of the picture), and FFmpeg held to it -- the filter
// the export, read_frame and the sheets put in their chains draws, pixel
// by pixel, the coverage the rule gives, for each shape, with and without
// a feather, inverted, and on a picture that shows only part of its
// source.

#include "ClipMask.h"
#include "Ulid.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string Quote(const std::string& text) { return "'" + text + "'"; }

ClipMask Mask(const char* shape, EffectParamValue cx, EffectParamValue cy,
              EffectParamValue width, EffectParamValue height,
              EffectParamValue feather, bool inverted = false) {
    ClipMask mask;
    mask.shape = shape;
    mask.center_x = cx;
    mask.center_y = cy;
    mask.width = width;
    mask.height = height;
    mask.feather = feather;
    mask.inverted = inverted;
    return mask;
}

}  // namespace

int main() {
    // The rule.
    {
        const ClipMask none;
        Check(ClipMaskCoverage(none, 0.9, 0.1) == 1.0,
              "no mask keeps the whole picture");
        const ClipMask half =
            Mask(kClipMaskRectangle, {1, 4}, {1, 2}, {1, 2}, {1, 1}, {0, 1});
        Check(ClipMaskCoverage(half, 0.25, 0.5) == 1.0 &&
                  ClipMaskCoverage(half, 0.49, 0.99) == 1.0 &&
                  ClipMaskCoverage(half, 0.51, 0.5) == 0.0,
              "a rectangle on the left half keeps the left half, edge hard");
        const ClipMask reversed = Mask(kClipMaskRectangle, {1, 4}, {1, 2},
                                       {1, 2}, {1, 1}, {0, 1}, true);
        Check(ClipMaskCoverage(reversed, 0.25, 0.5) == 0.0 &&
                  ClipMaskCoverage(reversed, 0.75, 0.5) == 1.0,
              "inverted, it keeps the other half");
        const ClipMask round =
            Mask(kClipMaskEllipse, {1, 2}, {1, 2}, {1, 2}, {1, 1}, {1, 4});
        Check(
            ClipMaskCoverage(round, 0.5, 0.5) == 1.0 &&
                ClipMaskCoverage(round, 0.25, 0.5) == 0.0 &&
                std::fabs(ClipMaskCoverage(round, 0.28125, 0.5) - 0.5) < 1e-9 &&
                ClipMaskCoverage(round, 0.3125, 0.5) == 1.0 &&
                ClipMaskCoverage(round, 0.3, 0.02) == 0.0,
            "an ellipse fades inwards over its feather: nothing on its "
            "edge, half an eighth of the way in, whole a quarter in");
        Check(ClipMaskFilter(none, {0, 0, 320, 180, 320, 180}, "1").empty() &&
                  ClipMaskFilter(half, {0, 0, 0, 180, 320, 180}, "1").empty(),
              "no mask, or no picture, is no filter");
        const std::string spelled =
            ClipMaskFilter(half, {40, 20, 240, 140, 320, 180}, "7");
        Check(spelled ==
                  "split[mka7][mkb7];[mkb7]alphaextract,geq=lum='lum(X,Y)*"
                  "lte(max(abs((((40+(X+0.5)*240/W)/320-(1/4))/((1/2)/2))),"
                  "abs((((20+(Y+0.5)*140/H)/180-(1/2))/((1/1)/2)))),1)'"
                  "[mkm7];[mka7][mkm7]alphamerge",
              "the filter spells the rule on the source's coordinates: " +
                  spelled);
    }

    // FFmpeg held to the rule, on a white picture: what is left of the
    // white is the coverage.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-mask-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    struct Case {
        const char* name;
        ClipMask mask;
    };
    const std::vector<Case> cases = {
        {"a hard rectangle",
         Mask(kClipMaskRectangle, {1, 4}, {1, 2}, {1, 2}, {3, 4}, {0, 1})},
        {"a soft rectangle",
         Mask(kClipMaskRectangle, {1, 2}, {2, 5}, {3, 5}, {1, 2}, {1, 3})},
        {"a soft ellipse",
         Mask(kClipMaskEllipse, {1, 2}, {1, 2}, {1, 2}, {1, 1}, {1, 4})},
        {"an inverted ellipse off centre",
         Mask(kClipMaskEllipse, {3, 4}, {1, 4}, {2, 5}, {3, 5}, {1, 2}, true)},
    };
    // The picture: the whole 320x180 source, then a 240x140 part of it
    // starting at (40, 20), drawn at half its size.
    struct Picture {
        const char* name;
        std::string before;
        ClipMaskRegion region;
        int width;
        int height;
    };
    const std::vector<Picture> pictures = {
        {"the whole source", "", {0, 0, 320, 180, 320, 180}, 320, 180},
        {"a cropped part, scaled",
         "crop=240:140:40:20,scale=120:70,",
         {40, 20, 240, 140, 320, 180},
         120,
         70},
    };
    for (const Picture& picture : pictures) {
        for (const Case& item : cases) {
            const std::filesystem::path raw = directory / "mask.raw";
            const std::string command =
                Quote(FFMPEG_EXECUTABLE) +
                " -hide_banner -loglevel error -f lavfi -i "
                "color=c=white:s=320x180:r=25:d=0.1 -vf \"" +
                picture.before + "format=yuva444p16le," +
                ClipMaskFilter(item.mask, picture.region, "1") +
                ",premultiply=inplace=1,format=gray\" -frames:v 1 -f rawvideo "
                "-y " +
                Quote(raw.string());
            const std::string label =
                std::string(item.name) + " on " + picture.name;
            if (std::system(command.c_str()) != 0) {
                Check(false, label + ": FFmpeg refused the filter");
                continue;
            }
            std::ifstream file(raw, std::ios::binary);
            const std::string pixels((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
            if (pixels.size() !=
                static_cast<size_t>(picture.width) * picture.height) {
                Check(false, label + ": the picture has " +
                                 std::to_string(pixels.size()) + " bytes");
                continue;
            }
            // What a kept pixel reads on this grey plane: the brightest
            // value of the picture, which the masks here all reach.
            double white = 0.0;
            for (const char byte : pixels)
                white = std::max(white, static_cast<double>(
                                            static_cast<unsigned char>(byte)));
            double worst = 0.0;
            double full = 0.0;
            for (int y = 0; y < picture.height; ++y) {
                for (int x = 0; x < picture.width; ++x) {
                    const double u =
                        (picture.region.x +
                         (x + 0.5) * picture.region.width / picture.width) /
                        picture.region.source_width;
                    const double v =
                        (picture.region.y +
                         (y + 0.5) * picture.region.height / picture.height) /
                        picture.region.source_height;
                    const double expected = ClipMaskCoverage(item.mask, u, v);
                    full = std::max(full, expected);
                    const double read = static_cast<unsigned char>(
                        pixels[static_cast<size_t>(y) * picture.width + x]);
                    worst = std::max(worst, std::fabs(read - expected * white));
                }
            }
            // Two roundings stand between the rule and the byte read: the
            // alpha plane's, then the grey's. Measured at most 1.98.
            Check(full == 1.0 && white > 200.0 && worst <= 2.5,
                  label + ": FFmpeg draws the rule's coverage, at most " +
                      std::to_string(worst) + " of " + std::to_string(white) +
                      " away");
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return failures == 0 ? 0 : 1;
}
