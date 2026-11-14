// FC9 -- docs/roadmap/FILMCRAFT-2026-10.md. The comparison of images itself
// (tests/golden_image.h): PNG read back as written at both depths, the
// three measures on differences whose answer is known, the files a failure
// leaves, and CUTMACHINE_BLESS.

#include "golden_image.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef GOLDEN_OUTPUT_DIR
#error "GOLDEN_OUTPUT_DIR names where the tests may write"
#endif

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "FAIL: " << name << ": threw: " << e.what() << '\n';
    }
}

// A ramp over x and y, a constant blue and an alpha that varies: every
// channel carries something a decoder could swap or drop.
GoldenImage Ramp(int32_t width, int32_t height, int32_t channels) {
    std::vector<uint16_t> words;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            words.push_back(static_cast<uint16_t>(x * 65535 / (width - 1)));
            words.push_back(static_cast<uint16_t>(y * 65535 / (height - 1)));
            words.push_back(12345);
            if (channels == 4) {
                words.push_back(static_cast<uint16_t>(65535 - x * 1000));
            }
        }
    }
    return GoldenImageFromWords(width, height, channels, words);
}

std::string Scratch(const std::string& leaf) {
    const std::filesystem::path path =
        std::filesystem::path(GOLDEN_OUTPUT_DIR) / "self-test" / leaf;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path.string();
}

}  // namespace

int main() {
    // The suite plays both sides of the variable itself.
    ::unsetenv("CUTMACHINE_BLESS");
    ::unsetenv("CUTMACHINE_GOLDEN_OUTPUT");

    Test(
        "a PNG reads back the samples it was written with, 8 and 16 bits, "
        "RGB and RGBA",
        [] {
            const std::string dir = Scratch("png");
            for (int32_t channels : {3, 4}) {
                for (GoldenDepth depth :
                     {GoldenDepth::Eight, GoldenDepth::Sixteen}) {
                    const GoldenImage image =
                        QuantizeGoldenImage(Ramp(37, 21, channels), depth);
                    const std::string path =
                        dir + "/ramp" + std::to_string(channels) + "-" +
                        std::to_string(static_cast<int>(depth)) + ".png";
                    std::string error;
                    GoldenImage read;
                    GoldenDepth readDepth = GoldenDepth::Eight;
                    Check(WriteGoldenPng(path, image, depth, error) &&
                              ReadGoldenPng(path, read, readDepth, error),
                          "write and read " + path + ": " + error);
                    Check(readDepth == depth && read.width == 37 &&
                              read.height == 21 && read.channels == channels &&
                              read.samples == image.samples,
                          path + " reads back sample for sample");
                }
            }
        });

    Test("sixteen bits keep a step that eight bits round away", [] {
        const GoldenImage fine =
            GoldenImageFromWords(1, 1, 3, {1000, 1001, 65535});
        const GoldenImage wide =
            QuantizeGoldenImage(fine, GoldenDepth::Sixteen);
        const GoldenImage narrow =
            QuantizeGoldenImage(fine, GoldenDepth::Eight);
        Check(wide.samples[0] != wide.samples[1] &&
                  narrow.samples[0] == narrow.samples[1],
              "1000 and 1001 of 65535 differ at 16 bits and not at 8");
        const GoldenImage outside = QuantizeGoldenImage(
            GoldenImageFromFloat(1, 1, 3, {-0.25f, 1.5f, std::nanf("")}),
            GoldenDepth::Eight);
        Check(outside.samples == std::vector<float>{0.0f, 1.0f, 0.0f},
              "a sample outside [0, 1] is clamped, NaN reads as 0");
    });

    Test("identical images measure nothing and pass an exact tolerance", [] {
        const GoldenImage image =
            QuantizeGoldenImage(Ramp(10, 10, 4), GoldenDepth::Eight);
        GoldenDiff diff;
        std::string error;
        Check(DiffGoldenImages(image, image, GoldenDepth::Eight, diff, error),
              error);
        Check(
            std::isinf(diff.psnr) && diff.max_abs == 0.0 && diff.p99 == 0.0 &&
                diff.mean_abs == 0.0 && diff.differing == 0,
            "PSNR infinite, everything else zero: " + DescribeGoldenDiff(diff));
        Check(GoldenToleranceAccepts(GoldenTolerance{}, diff),
              "the default tolerance is exact, and accepts it");
    });

    Test("one sample off: the measures FilmCraft's definitions give", [] {
        const GoldenImage expected =
            QuantizeGoldenImage(Ramp(10, 10, 4), GoldenDepth::Eight);
        GoldenImage actual = expected;
        // Pixel (3, 2), green, ten levels up.
        actual.samples[(2 * 10 + 3) * 4 + 1] += 10.0f / 255.0f;
        GoldenDiff diff;
        std::string error;
        Check(
            DiffGoldenImages(expected, actual, GoldenDepth::Eight, diff, error),
            error);
        // One sample of 400 off by 10: mse = 100 / 400 levels^2.
        const double psnr = 10.0 * std::log10(255.0 * 255.0 / (100.0 / 400.0));
        Check(diff.max_abs == 10.0 && diff.differing == 1 && diff.p99 == 0.0 &&
                  std::fabs(diff.mean_abs - 10.0 / 400.0) < 1e-12 &&
                  std::fabs(diff.psnr - psnr) < 1e-9,
              "max 10, one pixel, p99 0 (1 % of 100 pixels), mean 10/400, "
              "PSNR " +
                  std::to_string(psnr) + ": " + DescribeGoldenDiff(diff));
        // A second pixel off: 2 % of the picture, so the 99th percentile
        // now lands on one of them.
        actual.samples[(7 * 10 + 8) * 4 + 3] -= 4.0f / 255.0f;
        Check(DiffGoldenImages(expected, actual, GoldenDepth::Eight, diff,
                               error) &&
                  diff.differing == 2 && diff.max_abs == 10.0 &&
                  diff.p99 == 4.0,
              "two pixels of 100: p99 is the smaller of the two, 4: " +
                  DescribeGoldenDiff(diff));
        GoldenTolerance tolerance;
        tolerance.min_psnr = 30.0;
        tolerance.max_abs = 10.0;
        tolerance.p99 = 4.0;
        Check(GoldenToleranceAccepts(tolerance, diff),
              "a tolerance at the measures accepts them");
        tolerance.p99 = 3.0;
        Check(!GoldenToleranceAccepts(tolerance, diff),
              "one level under the p99 refuses them");
        GoldenDiff shape;
        Check(!DiffGoldenImages(
                  expected,
                  QuantizeGoldenImage(Ramp(10, 11, 4), GoldenDepth::Eight),
                  GoldenDepth::Eight, shape, error) &&
                  error.find("10x10x4") != std::string::npos,
              "images of different sizes are not compared: " + error);
    });

    Test(
        "a failure writes the render and an amplified difference, and names "
        "both",
        [] {
            const std::string references = Scratch("fail-references");
            const std::string output = Scratch("fail-output");
            const GoldenImage expected =
                QuantizeGoldenImage(Ramp(10, 10, 3), GoldenDepth::Eight);
            std::string error;
            Check(WriteGoldenPng(references + "/ramp.png", expected,
                                 GoldenDepth::Eight, error),
                  error);
            GoldenImage actual = expected;
            actual.samples[(4 * 10 + 5) * 3 + 0] =
                expected.samples[(4 * 10 + 5) * 3 + 0] + 10.0f / 255.0f;
            GoldenCheck check{
                "ramp", references, output, GoldenDepth::Eight, {}};
            GoldenResult result = CheckGoldenImage(actual, check);
            Check(
                !result.passed && !result.blessed &&
                    result.actual_path == output + "/ramp.actual.png" &&
                    result.diff_path == output + "/ramp.diff.png" &&
                    result.message.find(result.actual_path) !=
                        std::string::npos &&
                    result.message.find(result.diff_path) != std::string::npos,
                "an exact check of a changed render fails and names its "
                "files: " +
                    result.message);
            GoldenImage written;
            GoldenImage difference;
            GoldenDepth depth = GoldenDepth::Eight;
            Check(
                ReadGoldenPng(result.actual_path, written, depth, error) &&
                    written.samples ==
                        QuantizeGoldenImage(actual, GoldenDepth::Eight).samples,
                "the render written is the render compared: " + error);
            Check(ReadGoldenPng(result.diff_path, difference, depth, error) &&
                      difference.channels == 4,
                  "the difference reads back as RGBA: " + error);
            bool elsewhere = true;
            for (size_t pixel = 0; pixel < 100; ++pixel) {
                if (pixel == 45) continue;
                elsewhere = elsewhere &&
                            difference.samples[pixel * 4] == 0.0f &&
                            difference.samples[pixel * 4 + 3] == 1.0f;
            }
            Check(std::lround(difference.samples[45 * 4] * 255.0f) == 80 &&
                      difference.samples[45 * 4 + 1] == 0.0f && elsewhere,
                  "ten levels show as eighty, in their channel, and "
                  "nothing else shows");

            GoldenCheck lenient = check;
            lenient.tolerance = {30.0, 10.0, 0.0};
            result = CheckGoldenImage(actual, lenient);
            Check(result.passed && result.actual_path.empty() &&
                      result.message.find("écart max 10") != std::string::npos,
                  "within its tolerance it passes, and still says by how "
                  "much: " +
                      result.message);

            ::setenv("CUTMACHINE_GOLDEN_OUTPUT", (output + "/moved").c_str(),
                     1);
            result = CheckGoldenImage(actual, check);
            ::unsetenv("CUTMACHINE_GOLDEN_OUTPUT");
            Check(result.actual_path == output + "/moved/ramp.actual.png" &&
                      std::filesystem::exists(result.actual_path),
                  "CUTMACHINE_GOLDEN_OUTPUT moves what a failure writes");

            check.name = "absente";
            result = CheckGoldenImage(actual, check);
            Check(!result.passed &&
                      result.message.find("CUTMACHINE_BLESS=1") !=
                          std::string::npos &&
                      std::filesystem::exists(result.actual_path),
                  "a missing reference fails, says how to write it, and "
                  "leaves the render to look at: " +
                      result.message);
        });

    Test(
        "CUTMACHINE_BLESS rewrites the reference, which then compares "
        "exactly",
        [] {
            const std::string references = Scratch("bless-references");
            const std::string output = Scratch("bless-output");
            const GoldenImage image = Ramp(16, 8, 4);
            GoldenCheck check{
                "rampe", references, output, GoldenDepth::Sixteen, {}};
            ::setenv("CUTMACHINE_BLESS", "1", 1);
            GoldenResult result = CheckGoldenImage(image, check);
            ::unsetenv("CUTMACHINE_BLESS");
            Check(result.passed && result.blessed &&
                      std::filesystem::exists(references + "/rampe.png") &&
                      result.message.find("réécrite") != std::string::npos,
                  "blessing writes the reference: " + result.message);
            result = CheckGoldenImage(image, check);
            Check(result.passed && !result.blessed &&
                      std::isinf(result.diff.psnr),
                  "the blessed reference compares at zero: " + result.message);
            GoldenImage changed = image;
            changed.samples[0] += 3.0f / 65535.0f;
            result = CheckGoldenImage(changed, check);
            Check(!result.passed && result.diff.max_abs == 3.0,
                  "three 16-bit codes are three codes off: " + result.message);
            check.depth = GoldenDepth::Eight;
            result = CheckGoldenImage(image, check);
            Check(!result.passed &&
                      result.message.find("16 bits") != std::string::npos,
                  "a 16-bit reference is not compared at 8 bits: " +
                      result.message);
            ::setenv("CUTMACHINE_BLESS", "non", 1);
            Check(!GoldenBlessing(), "only 1, true, yes or on bless");
            ::setenv("CUTMACHINE_BLESS", "Yes", 1);
            Check(GoldenBlessing(), "in any case");
            ::unsetenv("CUTMACHINE_BLESS");
        });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
