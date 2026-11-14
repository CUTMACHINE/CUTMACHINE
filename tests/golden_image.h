#pragma once

// FC9 -- docs/roadmap/FILMCRAFT-2026-10.md. A rendered image compared with a
// reference PNG under tests/fixtures/golden/, for the tests of a CPU render.
// Adapted from FilmCraft crates/testkit/src/golden.rs (MIT OR Apache-2.0,
// commit 09aaf9b).
//
// WHY THIS FILE EXISTS. A render was tested by a handful of pixels its
// author thought of, or by a fingerprint of the whole buffer. The first
// misses what nobody looked at; the second fails on the last bit a
// compiler changes from one machine to the other (PRODUIT-2026-10 PRD12),
// and then says nothing about where. Here the whole picture is compared
// with a reference a person has looked at, by three measures, against a
// tolerance the test declares (PHILOSOPHY.md principle 6):
//
//   psnr     over every sample of every channel, in dB, against full scale;
//            infinite when the images are the same.
//   max_abs  the largest difference of one sample, in code values of the
//            reference's depth (0-255 for 8 bits, 0-65535 for 16).
//   p99      the 99th percentile, over pixels, of each pixel's largest
//            channel difference, in the same code values: what 99 % of the
//            picture stays within, so that a few pixels at a rounding edge
//            do not hide a shift of the whole image, nor the reverse.
//
// The rendered image is quantized to the reference's depth before it is
// measured, the way the reference was when it was written: a render that
// has not changed compares at exactly zero on the machine that wrote it.
//
// On a failure the rendered image and a difference image (each channel's
// difference times eight, opaque) are written to the output directory, and
// the message names both paths. CUTMACHINE_BLESS=1 in the environment
// rewrites the references instead of comparing; CUTMACHINE_GOLDEN_OUTPUT
// moves the output directory. docs/reference/tests-golden.md says how to
// use it.
//
// PNG goes through FFmpeg's libavcodec, which the model already links: no
// new dependency.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// An image as a test compares it: interleaved samples in [0, 1], rows from
// the top, three channels (RGB) or four (RGBA, as the render stored it --
// premultiplied or not, the PNG keeps the numbers).
struct GoldenImage {
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 4;
    std::vector<float> samples;
};

// The depth a reference is stored at.
enum class GoldenDepth { Eight = 8, Sixteen = 16 };

GoldenImage GoldenImageFromFloat(int32_t width, int32_t height,
                                 int32_t channels,
                                 const std::vector<float>& samples);
GoldenImage GoldenImageFromBytes(int32_t width, int32_t height,
                                 int32_t channels,
                                 const std::vector<uint8_t>& samples);
GoldenImage GoldenImageFromWords(int32_t width, int32_t height,
                                 int32_t channels,
                                 const std::vector<uint16_t>& samples);

// Each sample clamped to [0, 1] and rounded to the nearest code value of
// `depth`, as a PNG of that depth stores it.
GoldenImage QuantizeGoldenImage(const GoldenImage& image, GoldenDepth depth);

struct GoldenDiff {
    double psnr = 0.0;
    double max_abs = 0.0;
    double p99 = 0.0;
    double mean_abs = 0.0;
    // Pixels with any channel different.
    size_t differing = 0;
};

// Both images in code values of `depth`, compared as they are (quantize
// them first). False, with `error`, when their sizes or channels differ.
bool DiffGoldenImages(const GoldenImage& expected, const GoldenImage& actual,
                      GoldenDepth depth, GoldenDiff& diff, std::string& error);

// |expected - actual| per channel, times `gain`, clamped; the alpha
// difference is folded into the three colours and the result is opaque
// RGBA, so a change of coverage alone still shows.
GoldenImage GoldenDifferenceImage(const GoldenImage& expected,
                                  const GoldenImage& actual, float gain = 8.0f);

// Every limit must hold. The default is exact.
struct GoldenTolerance {
    double min_psnr = std::numeric_limits<double>::infinity();
    double max_abs = 0.0;
    double p99 = 0.0;
};

bool GoldenToleranceAccepts(const GoldenTolerance& tolerance,
                            const GoldenDiff& diff);

std::string DescribeGoldenDiff(const GoldenDiff& diff);

bool WriteGoldenPng(const std::string& path, const GoldenImage& image,
                    GoldenDepth depth, std::string& error);
// The depth the file was stored at comes back in `depth`.
bool ReadGoldenPng(const std::string& path, GoldenImage& image,
                   GoldenDepth& depth, std::string& error);

// A reference larger than this is refused when blessed: render smaller.
inline constexpr size_t kGoldenMaxBytes = 64 * 1024;

struct GoldenCheck {
    // The reference is <reference_dir>/<name>.png; on a failure the output
    // directory receives <name>.actual.png and <name>.diff.png.
    std::string name;
    std::string reference_dir;
    std::string output_dir;
    GoldenDepth depth = GoldenDepth::Eight;
    GoldenTolerance tolerance;
};

struct GoldenResult {
    bool passed = false;
    // CUTMACHINE_BLESS was set: the reference was written, not compared.
    bool blessed = false;
    GoldenDiff diff;
    // What happened, in one paragraph: the measures, the tolerance, and on
    // a failure the paths of what was written.
    std::string message;
    std::string actual_path;
    std::string diff_path;
};

// CUTMACHINE_BLESS set to 1, true, yes or on.
bool GoldenBlessing();

GoldenResult CheckGoldenImage(const GoldenImage& actual,
                              const GoldenCheck& check);
