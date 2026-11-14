// MA2 -- docs/roadmap/MOTION-AFTER-2026-09.md. See MotionAfterVerify.h.

#include "MotionAfterVerify.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <unordered_map>

namespace {

using mcp_json::Value;

// Four decimals, as the export writes its numbers: enough for a gap of a
// hundredth of a level, without float noise.
Value Num(double value) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    return Value::MakeDouble(std::round(value * 10000.0) / 10000.0);
}

// Who holds the codec's three allocations, whatever happens.
struct Decoder {
    AVCodecContext* context = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    ~Decoder() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
};

std::string FfmpegError(int status) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(status, text, sizeof text);
    return text;
}

// The nodes of one evaluated tree by path, with whether each is drawn: a
// node an opacity of 0 hides, its own or an ancestor's, shows nothing an
// approximation could change.
void IndexShown(
    const MotionEvaluatedNode& node, bool shown,
    std::unordered_map<std::string,
                       std::pair<const MotionEvaluatedNode*, bool>>& index) {
    const bool drawn = shown && node.opacity > 0.0;
    auto& slot = index[node.path];
    if (slot.first == nullptr || (!slot.second && drawn)) {
        slot = {&node, drawn};
    }
    for (const MotionEvaluatedNode& child : node.children) {
        IndexShown(child, drawn, index);
    }
}

bool Cuts(const MotionEvaluatedNode& node) {
    return std::any_of(node.filters.begin(), node.filters.end(),
                       [](const MotionFilter& filter) {
                           return filter.kind == MotionFilterKind::Slice &&
                                  filter.amplitude != 0.0;
                       });
}

std::string Numbered(const std::string& directory, const char* side,
                     int64_t frame) {
    char name[64];
    std::snprintf(name, sizeof name, "%s-%06lld.png", side,
                  static_cast<long long>(frame));
    return (std::filesystem::path(directory) / name).string();
}

bool ReadFile(const std::string& path, std::string& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file),
                 std::istreambuf_iterator<char>());
    return static_cast<bool>(file) || file.eof();
}

const char* VerdictName(MotionAfterVerdict verdict) {
    switch (verdict) {
        case MotionAfterVerdict::Within:
            return "within";
        case MotionAfterVerdict::Announced:
            return "announced";
        case MotionAfterVerdict::Unexplained:
            return "unexplained";
    }
    return "within";
}

}  // namespace

bool MotionAfterPngLuma(const std::string& png, int32_t& width, int32_t& height,
                        std::vector<float>& luma, std::string& error) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        error = "this FFmpeg has no PNG decoder";
        return false;
    }
    if (png.empty() || png.size() > static_cast<size_t>(INT32_MAX)) {
        error = "not a PNG of a size FFmpeg reads";
        return false;
    }
    Decoder decoder;
    decoder.context = avcodec_alloc_context3(codec);
    decoder.frame = av_frame_alloc();
    decoder.packet = av_packet_alloc();
    if (decoder.context == nullptr || decoder.frame == nullptr ||
        decoder.packet == nullptr) {
        error = "FFmpeg refused an allocation";
        return false;
    }
    int status = avcodec_open2(decoder.context, codec, nullptr);
    if (status >= 0) {
        status = av_new_packet(decoder.packet, static_cast<int>(png.size()));
    }
    if (status >= 0) {
        std::memcpy(decoder.packet->data, png.data(), png.size());
        status = avcodec_send_packet(decoder.context, decoder.packet);
    }
    if (status >= 0) {
        status = avcodec_send_packet(decoder.context, nullptr);
    }
    if (status >= 0) {
        status = avcodec_receive_frame(decoder.context, decoder.frame);
    }
    if (status < 0) {
        error = "PNG decoder: " + FfmpegError(status);
        return false;
    }
    const AVFrame& frame = *decoder.frame;
    const auto format = static_cast<AVPixelFormat>(frame.format);
    int channels = 0;
    bool wide = false;
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
        default: {
            const char* name = av_get_pix_fmt_name(format);
            error = std::string("PNG pixel format not read: ") +
                    (name != nullptr ? name : "unknown") +
                    " (RGB or RGBA, 8 or 16 bits)";
            return false;
        }
    }
    width = frame.width;
    height = frame.height;
    luma.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0F);
    for (int32_t y = 0; y < height; ++y) {
        const uint8_t* row =
            frame.data[0] + static_cast<ptrdiff_t>(y) * frame.linesize[0];
        for (int32_t x = 0; x < width; ++x) {
            uint32_t sample[4] = {0, 0, 0, 255};
            for (int channel = 0; channel < channels; ++channel) {
                const size_t at = static_cast<size_t>(x) * channels + channel;
                // 16 bits brought to 8, rounded: the measure is in 8-bit
                // levels whatever the file holds.
                sample[channel] =
                    wide ? (((static_cast<uint32_t>(row[at * 2]) << 8) |
                             row[at * 2 + 1]) +
                            128) /
                               257
                         : row[at];
            }
            // Over black, rounded to a level, then Pillow's "L": the
            // BT.601 weights in 16-bit fixed point, rounded -- the measure
            // the earlier numbers were taken with by hand
            // (MotionAfterVerify.h).
            uint32_t rgb[3];
            for (int channel = 0; channel < 3; ++channel) {
                rgb[channel] = (sample[channel] * sample[3] + 127) / 255;
            }
            const uint32_t level =
                (rgb[0] * 19595 + rgb[1] * 38470 + rgb[2] * 7471 + 0x8000) >>
                16;
            luma[static_cast<size_t>(y) * width + x] =
                static_cast<float>(level);
        }
    }
    return true;
}

MotionAfterFrameGap MotionAfterCompareLuma(const std::vector<float>& engine,
                                           const std::vector<float>& after) {
    MotionAfterFrameGap gap;
    const size_t count = std::min(engine.size(), after.size());
    if (count == 0) {
        return gap;
    }
    double sum = 0.0;
    size_t over = 0;
    for (size_t index = 0; index < count; ++index) {
        const double difference =
            std::fabs(static_cast<double>(engine[index]) - after[index]);
        sum += difference;
        gap.max = std::max(gap.max, difference);
        if (difference > 8.0) {
            ++over;
        }
    }
    gap.mean = sum / static_cast<double>(count);
    gap.over = static_cast<double>(over) / static_cast<double>(count);
    return gap;
}

std::vector<size_t> MotionAfterNotesShown(const MotionAfterExportReport& report,
                                          const MotionFrame& frame) {
    // The frame and each instant of its shutter: an approximation shows if
    // it shows at any of them.
    std::vector<std::unordered_map<std::string,
                                   std::pair<const MotionEvaluatedNode*, bool>>>
        trees(1 + frame.motion_blur.size());
    IndexShown(frame.root, true, trees[0]);
    for (size_t index = 0; index < frame.motion_blur.size(); ++index) {
        IndexShown(frame.motion_blur[index], true, trees[index + 1]);
    }
    std::set<size_t> shown;
    for (const MotionAfterNoteScope& scope : report.scopes) {
        if (scope.note >= report.notes.size() || shown.count(scope.note) != 0) {
            continue;
        }
        for (const auto& tree : trees) {
            const auto found = tree.find(scope.path);
            if (found == tree.end() || !found->second.second) {
                continue;
            }
            if (!scope.slice || Cuts(*found->second.first)) {
                shown.insert(scope.note);
                break;
            }
        }
    }
    return {shown.begin(), shown.end()};
}

bool VerifyMotionAfterFrames(const MotionAfterVerifyRequest& request,
                             MotionFrameRenderer render,
                             const MotionAfterFrameSource& after,
                             std::vector<MotionAfterFrameCheck>& checks,
                             MotionError& error) {
    checks.clear();
    if (render == nullptr || request.engine.composition == nullptr) {
        error = {"UnsupportedOperation", "",
                 std::string("no motion renderer in this process: ") +
                     kMotionRendererHolder};
        return false;
    }
    for (const int64_t frame : request.frames) {
        MotionAfterFrameCheck check;
        check.frame = frame;
        check.engine_png = Numbered(request.directory, "moteur", frame);
        check.after_png = Numbered(request.directory, "after", frame);
        std::error_code ignored;
        if (std::filesystem::exists(check.engine_png, ignored) ||
            std::filesystem::exists(check.after_png, ignored)) {
            error = {"IoError", "",
                     "refusing to overwrite the pictures of frame " +
                         std::to_string(frame) + " in '" + request.directory +
                         "'"};
            return false;
        }
        MotionFrameRequest engine = request.engine;
        engine.frame = frame;
        MotionFrameImage image;
        if (!render(engine, image, error)) {
            return false;
        }
        {
            std::ofstream file(check.engine_png,
                               std::ios::binary | std::ios::trunc);
            file << image.png;
            if (!file) {
                error = {"IoError", "",
                         "cannot write '" + check.engine_png + "'"};
                return false;
            }
        }
        std::string code;
        std::string message;
        if (!after(frame, check.after_png, code, message)) {
            error = {code.empty() ? "AfterRenderFailed" : code, "",
                     "After, frame " + std::to_string(frame) + ": " + message};
            return false;
        }
        std::string bytes;
        if (!ReadFile(check.after_png, bytes)) {
            error = {"IoError", "",
                     "After wrote no picture at '" + check.after_png + "'"};
            return false;
        }
        int32_t engineWidth = 0;
        int32_t engineHeight = 0;
        int32_t afterWidth = 0;
        int32_t afterHeight = 0;
        std::vector<float> engineLuma;
        std::vector<float> afterLuma;
        std::string decodeError;
        if (!MotionAfterPngLuma(image.png, engineWidth, engineHeight,
                                engineLuma, decodeError)) {
            error = {
                "DecodeFailed", "",
                "engine, frame " + std::to_string(frame) + ": " + decodeError};
            return false;
        }
        if (!MotionAfterPngLuma(bytes, afterWidth, afterHeight, afterLuma,
                                decodeError)) {
            error = {
                "DecodeFailed", "",
                "After, frame " + std::to_string(frame) + ": " + decodeError};
            return false;
        }
        if (afterWidth != engineWidth || afterHeight != engineHeight) {
            error = {"ValidationFailed", "",
                     "After renders " + std::to_string(afterWidth) + "x" +
                         std::to_string(afterHeight) + ", the engine " +
                         std::to_string(engineWidth) + "x" +
                         std::to_string(engineHeight) +
                         ": comp_id is not the composition's own"};
            return false;
        }
        check.gap = MotionAfterCompareLuma(engineLuma, afterLuma);
        check.notes = MotionAfterNotesShown(request.report, image.evaluated);
        check.verdict = check.gap.mean <= request.tolerance
                            ? MotionAfterVerdict::Within
                        : check.notes.empty() ? MotionAfterVerdict::Unexplained
                                              : MotionAfterVerdict::Announced;
        checks.push_back(std::move(check));
    }
    return true;
}

void MotionAfterVerifyFields(const MotionAfterVerifyRequest& request,
                             const std::vector<MotionAfterFrameCheck>& checks,
                             mcp_json::Value& result) {
    int64_t within = 0;
    int64_t announced = 0;
    int64_t unexplained = 0;
    std::vector<double> means;
    const MotionAfterFrameCheck* worst = nullptr;
    Value frames = Value::MakeArray();
    std::set<size_t> named;
    for (const MotionAfterFrameCheck& check : checks) {
        switch (check.verdict) {
            case MotionAfterVerdict::Within:
                ++within;
                break;
            case MotionAfterVerdict::Announced:
                ++announced;
                break;
            case MotionAfterVerdict::Unexplained:
                ++unexplained;
                break;
        }
        means.push_back(check.gap.mean);
        if (worst == nullptr || check.gap.mean > worst->gap.mean) {
            worst = &check;
        }
        Value entry = Value::MakeObject();
        entry.Set("frame", Value::MakeInt(check.frame));
        entry.Set("mean", Num(check.gap.mean));
        entry.Set("max", Num(check.gap.max));
        entry.Set("over_8", Num(check.gap.over));
        entry.Set("verdict", Value::MakeString(VerdictName(check.verdict)));
        if (check.verdict == MotionAfterVerdict::Announced) {
            Value notes = Value::MakeArray();
            for (const size_t note : check.notes) {
                notes.Push(Value::MakeString(request.report.notes[note]));
                named.insert(note);
            }
            entry.Set("notes", notes);
        }
        frames.Push(std::move(entry));
    }
    result.Set("frames_checked",
               Value::MakeInt(static_cast<int64_t>(checks.size())));
    result.Set("tolerance", Num(request.tolerance));
    result.Set("within", Value::MakeInt(within));
    result.Set("announced", Value::MakeInt(announced));
    result.Set("unexplained", Value::MakeInt(unexplained));
    if (worst != nullptr) {
        std::sort(means.begin(), means.end());
        const size_t middle = means.size() / 2;
        const double median = means.size() % 2 == 1
                                  ? means[middle]
                                  : (means[middle - 1] + means[middle]) / 2.0;
        result.Set("median_mean", Num(median));
        Value top = Value::MakeObject();
        top.Set("frame", Value::MakeInt(worst->frame));
        top.Set("mean", Num(worst->gap.mean));
        result.Set("worst", top);
    }
    // The notes the announced frames name, and the notes about the whole
    // composition, which may explain an unexplained frame but are never
    // counted as explaining one.
    Value explaining = Value::MakeArray();
    for (const size_t note : named) {
        explaining.Push(Value::MakeString(request.report.notes[note]));
    }
    result.Set("announced_by", explaining);
    std::set<size_t> scoped;
    for (const MotionAfterNoteScope& scope : request.report.scopes) {
        scoped.insert(scope.note);
    }
    Value whole = Value::MakeArray();
    for (size_t note = 0; note < request.report.notes.size(); ++note) {
        if (scoped.count(note) == 0) {
            whole.Push(Value::MakeString(request.report.notes[note]));
        }
    }
    result.Set("composition_notes", whole);
    result.Set("directory", Value::MakeString(request.directory));
    result.Set("frames", frames);
}
