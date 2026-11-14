// FC9 -- docs/roadmap/FILMCRAFT-2026-10.md. See golden_image.h. Adapted
// from FilmCraft crates/testkit/src/golden.rs (MIT OR Apache-2.0, commit
// 09aaf9b): the three measures, the tolerance, the amplified difference
// image and the blessing variable are theirs; the PNG codec is FFmpeg's,
// and the depths and the quantization are ours.

#include "golden_image.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {

double CodeMax(GoldenDepth depth) {
    return depth == GoldenDepth::Sixteen ? 65535.0 : 255.0;
}

bool ValidShape(int32_t width, int32_t height, int32_t channels,
                size_t samples) {
    return width > 0 && height > 0 && (channels == 3 || channels == 4) &&
           samples == static_cast<size_t>(width) * height * channels;
}

std::string FfmpegError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

AVPixelFormat PixelFormatFor(int32_t channels, GoldenDepth depth) {
    if (depth == GoldenDepth::Sixteen) {
        return channels == 4 ? AV_PIX_FMT_RGBA64BE : AV_PIX_FMT_RGB48BE;
    }
    return channels == 4 ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
}

}  // namespace

GoldenImage GoldenImageFromFloat(int32_t width, int32_t height,
                                 int32_t channels,
                                 const std::vector<float>& samples) {
    return {width, height, channels, samples};
}

GoldenImage GoldenImageFromBytes(int32_t width, int32_t height,
                                 int32_t channels,
                                 const std::vector<uint8_t>& samples) {
    GoldenImage image{width, height, channels, {}};
    image.samples.reserve(samples.size());
    for (uint8_t value : samples) image.samples.push_back(value / 255.0f);
    return image;
}

GoldenImage GoldenImageFromWords(int32_t width, int32_t height,
                                 int32_t channels,
                                 const std::vector<uint16_t>& samples) {
    GoldenImage image{width, height, channels, {}};
    image.samples.reserve(samples.size());
    for (uint16_t value : samples) image.samples.push_back(value / 65535.0f);
    return image;
}

namespace {

// The code value a PNG of `depth` stores for `sample`: clamped, nearest.
// NaN reads as 0 rather than as whatever a cast makes of it.
uint32_t Code(float sample, GoldenDepth depth) {
    const double scale = CodeMax(depth);
    const double clamped =
        std::isnan(sample) ? 0.0 : std::clamp<double>(sample, 0.0, 1.0);
    return static_cast<uint32_t>(std::floor(clamped * scale + 0.5));
}

}  // namespace

GoldenImage QuantizeGoldenImage(const GoldenImage& image, GoldenDepth depth) {
    GoldenImage quantized = image;
    const double scale = CodeMax(depth);
    for (float& sample : quantized.samples) {
        sample = static_cast<float>(Code(sample, depth) / scale);
    }
    return quantized;
}

bool DiffGoldenImages(const GoldenImage& expected, const GoldenImage& actual,
                      GoldenDepth depth, GoldenDiff& diff, std::string& error) {
    if (!ValidShape(expected.width, expected.height, expected.channels,
                    expected.samples.size()) ||
        !ValidShape(actual.width, actual.height, actual.channels,
                    actual.samples.size())) {
        error = "image malformée (taille, canaux ou nombre d'échantillons)";
        return false;
    }
    if (expected.width != actual.width || expected.height != actual.height ||
        expected.channels != actual.channels) {
        error = "la référence fait " + std::to_string(expected.width) + "x" +
                std::to_string(expected.height) + "x" +
                std::to_string(expected.channels) + ", le rendu " +
                std::to_string(actual.width) + "x" +
                std::to_string(actual.height) + "x" +
                std::to_string(actual.channels);
        return false;
    }
    const double scale = CodeMax(depth);
    const size_t channels = static_cast<size_t>(expected.channels);
    const size_t pixels = expected.samples.size() / channels;
    std::vector<double> pixelMax(pixels, 0.0);
    double sum = 0.0;
    double squares = 0.0;
    diff = {};
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        for (size_t channel = 0; channel < channels; ++channel) {
            const size_t index = pixel * channels + channel;
            // In code values: both images are quantized, so this is an
            // integer up to float rounding, which std::round removes.
            const double difference = std::round(
                std::fabs(static_cast<double>(expected.samples[index]) -
                          actual.samples[index]) *
                scale);
            sum += difference;
            squares += difference * difference;
            pixelMax[pixel] = std::max(pixelMax[pixel], difference);
        }
        diff.max_abs = std::max(diff.max_abs, pixelMax[pixel]);
        if (pixelMax[pixel] > 0.0) ++diff.differing;
    }
    // The smallest value at or under which 99 % of the pixels fall.
    const size_t rank = (pixels * 99 + 99) / 100;
    std::nth_element(pixelMax.begin(), pixelMax.begin() + (rank - 1),
                     pixelMax.end());
    diff.p99 = pixelMax[rank - 1];
    const double count = static_cast<double>(pixels * channels);
    diff.mean_abs = sum / count;
    const double mse = squares / count / (scale * scale);
    diff.psnr = mse == 0.0 ? std::numeric_limits<double>::infinity()
                           : 10.0 * std::log10(1.0 / mse);
    return true;
}

GoldenImage GoldenDifferenceImage(const GoldenImage& expected,
                                  const GoldenImage& actual, float gain) {
    GoldenImage image{expected.width, expected.height, 4, {}};
    const size_t channels = static_cast<size_t>(expected.channels);
    const size_t pixels = static_cast<size_t>(expected.width) * expected.height;
    image.samples.reserve(pixels * 4);
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto amplified = [&](size_t channel) {
            const size_t index = pixel * channels + channel;
            return std::min(1.0f, std::fabs(expected.samples[index] -
                                            actual.samples[index]) *
                                      gain);
        };
        const float alpha = channels == 4 ? amplified(3) : 0.0f;
        for (size_t channel = 0; channel < 3; ++channel) {
            image.samples.push_back(std::max(amplified(channel), alpha));
        }
        image.samples.push_back(1.0f);
    }
    return image;
}

bool GoldenToleranceAccepts(const GoldenTolerance& tolerance,
                            const GoldenDiff& diff) {
    return diff.psnr >= tolerance.min_psnr &&
           diff.max_abs <= tolerance.max_abs && diff.p99 <= tolerance.p99;
}

std::string DescribeGoldenDiff(const GoldenDiff& diff) {
    char text[200];
    if (std::isinf(diff.psnr)) {
        std::snprintf(text, sizeof(text),
                      "PSNR infini, écart max %.0f, p99 %.0f, moyen %.4f, "
                      "%zu pixel(s) différent(s)",
                      diff.max_abs, diff.p99, diff.mean_abs, diff.differing);
    } else {
        std::snprintf(text, sizeof(text),
                      "PSNR %.2f dB, écart max %.0f, p99 %.0f, moyen %.4f, "
                      "%zu pixel(s) différent(s)",
                      diff.psnr, diff.max_abs, diff.p99, diff.mean_abs,
                      diff.differing);
    }
    return text;
}

namespace {

bool EncodePng(const GoldenImage& image, GoldenDepth depth, std::string& bytes,
               std::string& error) {
    if (!ValidShape(image.width, image.height, image.channels,
                    image.samples.size())) {
        error = "image malformée (taille, canaux ou nombre d'échantillons)";
        return false;
    }
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        error = "ce FFmpeg n'a pas d'encodeur PNG";
        return false;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    const auto release = [&] {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    };
    if (context == nullptr || frame == nullptr || packet == nullptr) {
        error = "allocation FFmpeg refusée";
        release();
        return false;
    }
    const AVPixelFormat format = PixelFormatFor(image.channels, depth);
    context->width = image.width;
    context->height = image.height;
    context->pix_fmt = format;
    context->time_base = {1, 1};
    int status = avcodec_open2(context, codec, nullptr);
    if (status >= 0) {
        frame->format = format;
        frame->width = image.width;
        frame->height = image.height;
        status = av_frame_get_buffer(frame, 0);
    }
    if (status < 0) {
        error = "encodeur PNG : " + FfmpegError(status);
        release();
        return false;
    }
    const size_t channels = static_cast<size_t>(image.channels);
    for (int32_t y = 0; y < image.height; ++y) {
        uint8_t* row =
            frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
        for (size_t x = 0; x < static_cast<size_t>(image.width); ++x) {
            for (size_t channel = 0; channel < channels; ++channel) {
                const uint32_t code = Code(
                    image.samples[(static_cast<size_t>(y) * image.width + x) *
                                      channels +
                                  channel],
                    depth);
                const size_t at = x * channels + channel;
                if (depth == GoldenDepth::Sixteen) {
                    // RGB48BE / RGBA64BE: big-endian, as PNG stores it.
                    row[at * 2] = static_cast<uint8_t>(code >> 8);
                    row[at * 2 + 1] = static_cast<uint8_t>(code & 0xFF);
                } else {
                    row[at] = static_cast<uint8_t>(code);
                }
            }
        }
    }
    status = avcodec_send_frame(context, frame);
    if (status >= 0) status = avcodec_send_frame(context, nullptr);
    if (status >= 0) status = avcodec_receive_packet(context, packet);
    if (status < 0) {
        error = "encodeur PNG : " + FfmpegError(status);
        release();
        return false;
    }
    bytes.assign(reinterpret_cast<const char*>(packet->data),
                 static_cast<size_t>(packet->size));
    release();
    return true;
}

bool DecodePng(const std::string& bytes, GoldenImage& image, GoldenDepth& depth,
               std::string& error) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        error = "ce FFmpeg n'a pas de décodeur PNG";
        return false;
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    const auto release = [&] {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    };
    if (context == nullptr || frame == nullptr || packet == nullptr) {
        error = "allocation FFmpeg refusée";
        release();
        return false;
    }
    int status = avcodec_open2(context, codec, nullptr);
    if (status >= 0)
        status = av_new_packet(packet, static_cast<int>(bytes.size()));
    if (status >= 0) {
        std::memcpy(packet->data, bytes.data(), bytes.size());
        status = avcodec_send_packet(context, packet);
    }
    if (status >= 0) status = avcodec_send_packet(context, nullptr);
    if (status >= 0) status = avcodec_receive_frame(context, frame);
    if (status < 0) {
        error = "décodeur PNG : " + FfmpegError(status);
        release();
        return false;
    }
    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    int32_t channels = 0;
    bool wide = false;
    bool grey = false;
    switch (format) {
        case AV_PIX_FMT_RGB24:
            channels = 3;
            break;
        case AV_PIX_FMT_RGBA:
            channels = 4;
            break;
        case AV_PIX_FMT_RGB48BE:
            channels = 3;
            wide = true;
            break;
        case AV_PIX_FMT_RGBA64BE:
            channels = 4;
            wide = true;
            break;
        case AV_PIX_FMT_GRAY8:
            channels = 3;
            grey = true;
            break;
        case AV_PIX_FMT_GRAY16BE:
            channels = 3;
            grey = true;
            wide = true;
            break;
        default: {
            const char* name = av_get_pix_fmt_name(format);
            error = std::string("format PNG non lu : ") +
                    (name ? name : "inconnu") +
                    " (RGB ou RGBA, 8 ou 16 bits, ou gris)";
            release();
            return false;
        }
    }
    depth = wide ? GoldenDepth::Sixteen : GoldenDepth::Eight;
    const double scale = CodeMax(depth);
    const int32_t stored = grey ? 1 : channels;
    image = {frame->width, frame->height, channels, {}};
    image.samples.reserve(static_cast<size_t>(frame->width) * frame->height *
                          channels);
    for (int32_t y = 0; y < frame->height; ++y) {
        const uint8_t* row =
            frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0];
        for (int32_t x = 0; x < frame->width; ++x) {
            for (int32_t channel = 0; channel < channels; ++channel) {
                const size_t at = static_cast<size_t>(x) * stored +
                                  (grey ? 0 : static_cast<size_t>(channel));
                const uint32_t code =
                    wide ? (static_cast<uint32_t>(row[at * 2]) << 8) |
                               row[at * 2 + 1]
                         : row[at];
                image.samples.push_back(static_cast<float>(code / scale));
            }
        }
    }
    release();
    return true;
}

}  // namespace

bool WriteGoldenPng(const std::string& path, const GoldenImage& image,
                    GoldenDepth depth, std::string& error) {
    std::string bytes;
    if (!EncodePng(image, depth, bytes, error)) return false;
    std::error_code code;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), code);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "écriture impossible : " + path;
        return false;
    }
    return true;
}

bool ReadGoldenPng(const std::string& path, GoldenImage& image,
                   GoldenDepth& depth, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "lecture impossible : " + path;
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    if (!DecodePng(bytes, image, depth, error)) {
        error = path + " : " + error;
        return false;
    }
    return true;
}

bool GoldenBlessing() {
    const char* value = std::getenv("CUTMACHINE_BLESS");
    if (value == nullptr) return false;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

GoldenResult CheckGoldenImage(const GoldenImage& actual,
                              const GoldenCheck& check) {
    GoldenResult result;
    const std::string reference =
        (std::filesystem::path(check.reference_dir) / (check.name + ".png"))
            .lexically_normal()
            .string();
    const GoldenImage rendered = QuantizeGoldenImage(actual, check.depth);
    std::string error;

    if (GoldenBlessing()) {
        if (!WriteGoldenPng(reference, rendered, check.depth, error)) {
            result.message = "CUTMACHINE_BLESS : " + error;
            return result;
        }
        std::error_code code;
        const uintmax_t size = std::filesystem::file_size(reference, code);
        if (code || size > kGoldenMaxBytes) {
            result.message = reference + " : " + std::to_string(size) +
                             " octets, au-delà des " +
                             std::to_string(kGoldenMaxBytes) +
                             " permis à une référence ; rendre plus petit";
            return result;
        }
        result.passed = true;
        result.blessed = true;
        result.diff.psnr = std::numeric_limits<double>::infinity();
        result.message =
            "référence réécrite (CUTMACHINE_BLESS) : " + reference + ", " +
            std::to_string(size) + " octets ; regarde-la avant de la committer";
        return result;
    }

    const char* moved = std::getenv("CUTMACHINE_GOLDEN_OUTPUT");
    const std::filesystem::path output =
        moved && *moved ? std::filesystem::path(moved)
                        : std::filesystem::path(check.output_dir);
    const auto writeActual = [&] {
        result.actual_path = (output / (check.name + ".actual.png")).string();
        std::string ignored;
        if (!WriteGoldenPng(result.actual_path, rendered, check.depth,
                            ignored)) {
            result.actual_path.clear();
        }
    };

    GoldenImage expected;
    GoldenDepth stored = check.depth;
    if (!ReadGoldenPng(reference, expected, stored, error)) {
        writeActual();
        result.message = "référence absente ou illisible (" + error +
                         ") ; CUTMACHINE_BLESS=1 l'écrit" +
                         (result.actual_path.empty()
                              ? std::string()
                              : " ; rendu écrit dans " + result.actual_path);
        return result;
    }
    if (stored != check.depth) {
        result.message = reference + " est en " +
                         std::to_string(static_cast<int>(stored)) +
                         " bits, le test la compare en " +
                         std::to_string(static_cast<int>(check.depth)) +
                         " ; CUTMACHINE_BLESS=1 la réécrit";
        return result;
    }
    if (!DiffGoldenImages(expected, rendered, check.depth, result.diff,
                          error)) {
        writeActual();
        result.message = reference + " : " + error;
        return result;
    }
    char limits[160];
    std::snprintf(limits, sizeof(limits),
                  "tolérance : PSNR >= %.1f dB, écart max <= %.0f, p99 <= %.0f",
                  check.tolerance.min_psnr, check.tolerance.max_abs,
                  check.tolerance.p99);
    if (GoldenToleranceAccepts(check.tolerance, result.diff)) {
        result.passed = true;
        result.message = check.name + " : " + DescribeGoldenDiff(result.diff) +
                         " (" + limits + ")";
        return result;
    }
    writeActual();
    result.diff_path = (output / (check.name + ".diff.png")).string();
    if (!WriteGoldenPng(result.diff_path,
                        GoldenDifferenceImage(expected, rendered),
                        GoldenDepth::Eight, error)) {
        result.diff_path.clear();
    }
    result.message =
        reference + " : " + DescribeGoldenDiff(result.diff) + " (" + limits +
        ") ; rendu : " +
        (result.actual_path.empty() ? "non écrit" : result.actual_path) +
        ", différence x8 : " +
        (result.diff_path.empty() ? "non écrite" : result.diff_path);
    return result;
}
