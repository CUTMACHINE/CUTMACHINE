#include "FrameCapture.h"

#include "ClipAnimation.h"
#include "ClipBlur.h"
#include "ClipMask.h"
#include "ClipSpeed.h"
#include "ColorManagement.h"
#include "Export.h"
#include "FfmpegBinary.h"
#include "PersonMatte.h"
#include "Ulid.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

// FFmpeg's -ss takes seconds. Every other time in this project stays
// rational, and this is the one place a RationalTime has to become a decimal
// string -- so it is done exactly, by long division, rather than through a
// double. A frame boundary that would land on 3.9999999 with floating point
// lands on 4.0 here, which is the difference between the frame asked for and
// the one before it.
std::string FfmpegSeekSeconds(const RationalTime& time) {
    if (time.rate <= 0) return "0";
    const bool negative = time.value < 0;
    const int64_t magnitude = negative ? -time.value : time.value;
    const int64_t whole = magnitude / time.rate;
    int64_t remainder = magnitude % time.rate;
    std::ostringstream output;
    if (negative) output << '-';
    output << whole;
    if (remainder == 0) return output.str();
    output << '.';
    // Nine digits is past any timebase this project accepts, and the loop
    // stops early once the division terminates.
    for (int digit = 0; digit < 9 && remainder != 0; ++digit) {
        remainder *= 10;
        output << static_cast<char>('0' + remainder / time.rate);
        remainder %= time.rate;
    }
    return output.str();
}

namespace {

bool RunFfmpeg(const std::vector<std::string>& arguments, std::string& error) {
    process::Request request;
    request.arguments = arguments;
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "FFmpeg pipes");
        return false;
    }
    if (!result.Succeeded()) {
        error = "FFmpeg could not render the requested frame";
        return false;
    }
    return true;
}

}  // namespace

bool ApplyClipFramingToCapture(const Document& document, const Ulid& clipId,
                               const RationalTime& sourceTime,
                               FrameCaptureSettings& settings,
                               std::string& message) {
    if (clipId.empty()) return true;
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr ||
        (!ClipHasFraming(*clip) && !ClipHasAnimation(*clip) &&
         !ClipHasBlur(*clip) && !ClipHasMask(*clip))) {
        return true;
    }
    // R2 -- the clip's own frame number for that source time: the source
    // offset unscaled by the clip's speed is the local timeline offset
    // (ClipSpeed.h owns that conversion), and its whole frame is what
    // ClipAnimation.h samples at.
    ClipSampledParameters sampled;
    sampled.framing = clip->framing;
    if (ClipHasAnimation(*clip)) {
        int64_t localFrame = 0;
        try {
            if (ClipHasSpeedRamp(*clip)) {
                // R3, criterion 4 -- a ramp has no inverse in closed form:
                // the first of the clip's frames that shows this source
                // frame, by the rule itself.
                const DocumentSource* source =
                    document.FindSource(clip->source_id);
                int64_t frames = 0;
                if (source == nullptr ||
                    !ClipFrameCount(*clip, document.sequence.frame_rate, frames,
                                    message)) {
                    message = "clip '" + clip->id +
                              "': its ramp cannot be read back to a frame";
                    return false;
                }
                localFrame = ClipFirstLocalFrameShowing(
                    *clip,
                    sourceTime.to_frames(source->rate.num, source->rate.den),
                    frames, document.sequence.frame_rate, source->rate);
                if (localFrame < 0) {
                    message = "clip '" + clip->id +
                              "': no frame of its ramp shows that source "
                              "frame, so its animated values cannot be "
                              "sampled";
                    return false;
                }
            } else {
                const RationalTime offset = UnscaleBySpeed(
                    sourceTime.sub(clip->source_in), clip->speed);
                localFrame = offset.to_frames(document.sequence.frame_rate.num,
                                              document.sequence.frame_rate.den);
            }
        } catch (const std::exception& exception) {
            message = std::string("clip '") + clip->id +
                      "': the frame asked for is not a whole frame of the "
                      "clip, so its animated values cannot be sampled (" +
                      exception.what() + ')';
            return false;
        }
        if (!SampleClipParameters(*clip, localFrame,
                                  document.sequence.frame_rate, sampled,
                                  message)) {
            message = "clip '" + clip->id + "': " + message;
            return false;
        }
    }
    int64_t sourceWidth = 0;
    int64_t sourceHeight = 0;
    if (!ClipDisplayedSize(document, *clip, sourceWidth, sourceHeight, message))
        return false;
    int64_t frameWidth = 0;
    int64_t frameHeight = 0;
    BoundedFrameSize(document.sequence.width, document.sequence.height,
                     settings.max_dimension, frameWidth, frameHeight);
    ClipFramingGeometry framing;
    if (!ResolveClipFramingReduced(sampled.framing, sourceWidth, sourceHeight,
                                   document.sequence.width,
                                   document.sequence.height, frameWidth,
                                   frameHeight, framing, message)) {
        message = "clip '" + clip->id + "': " + message;
        return false;
    }
    // EF3 -- the blur in this frame's pixels (ClipBlur.h).
    if (!ClipBlurFilter(*clip, frameWidth, frameHeight, document.sequence.width,
                        document.sequence.height, settings.blur_filter,
                        message))
        return false;
    const LibraryMedia* media = document.FindLibraryMedia(clip->source_id);
    settings.blur_rgb =
        media != nullptr && IsRgbPixelFormat(media->pixel_format);
    // PA8 -- the mask on the region of the source this frame's picture
    // shows (ClipMask.h).
    settings.mask_filter = ClipMaskFilter(
        clip->mask,
        ClipMaskRegion{framing.source_x, framing.source_y, framing.source_width,
                       framing.source_height, sourceWidth, sourceHeight},
        "0");
    // PA15 -- the matte of the source frame read, found in the package.
    settings.matte_path.clear();
    if (ClipHasPersonMask(*clip)) {
        if (settings.package_directory.empty() || media == nullptr) {
            message = "clip '" + clip->id +
                      "' is cut out along its people, and its matte is kept "
                      "in a package this read was not told of";
            return false;
        }
        person_matte::Info info;
        if (!person_matte::MatteForMedia(document, settings.package_directory,
                                         clip->source_id, settings.matte_path,
                                         info, message)) {
            message = "clip '" + clip->id + "': " + message;
            settings.matte_path.clear();
            return false;
        }
        settings.matte_frame =
            person_matte::SourceFrameAt(sourceTime, media->rate);
        settings.matte_inverted = clip->mask.inverted;
    }
    settings.framed = true;
    settings.framing = framing;
    settings.source_width = sourceWidth;
    settings.source_height = sourceHeight;
    settings.frame_width = frameWidth;
    settings.frame_height = frameHeight;
    return true;
}

bool ClipFramePositionTime(const DocumentClip& clip,
                           const std::string& position, RationalTime& time) {
    if (position == "Start") {
        time = clip.source_in;
    } else if (position == "Middle") {
        time = ClipSourceTimeAt(
            clip, RationalTime{clip.duration.value / 2, clip.duration.rate});
    } else if (position == "End") {
        const RationalTime back{clip.duration.rate / 4 + 1, clip.duration.rate};
        const RationalTime end = ClipSourceEnd(clip);
        time = end.compare(back) > 0
                   ? (ClipHasSpeedRamp(clip)
                          ? ClipSourceTimeAt(clip, clip.duration.sub(back))
                          : end.sub(ScaleBySpeed(back, clip.speed)))
                   : clip.source_in;
    } else {
        return false;
    }
    return true;
}

bool CaptureSourceFrame(const std::string& inputPath, const RationalTime& time,
                        const FrameCaptureSettings& settings,
                        std::string& jpegBytes, std::string& error) {
    error.clear();
    jpegBytes.clear();
    if (time.rate <= 0 || time.value < 0) {
        error = "frame time must be a non-negative exact position";
        return false;
    }
    if (settings.max_dimension < 16 || settings.jpeg_quality < 2 ||
        settings.jpeg_quality > 31) {
        error = "invalid frame capture settings";
        return false;
    }
    // PRD40 -- the rush offline is said by its path, before FFmpeg fails on
    // it without saying which file it could not open.
    if (!ffmpeg::InputPresent(inputPath, error)) {
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() /
        ("cutmachine-frame-" + GenerateUlid() + ".jpg");

    // `scale` with -1 on one axis keeps the aspect ratio and lets FFmpeg pick
    // the other dimension; `force_original_aspect_ratio=decrease` bounds the
    // long edge whichever way the frame is oriented, so a vertical rush is
    // bounded on its height and a horizontal one on its width.
    const std::string bound = std::to_string(settings.max_dimension);
    const std::filesystem::path lutPath =
        std::filesystem::temp_directory_path() /
        ("cutmachine-frame-" + GenerateUlid() + ".cube");
    // R1 -- a framed clip is composed into the sequence frame the caller
    // already reduced to the bound; an unframed read keeps the single
    // bounding scale this capture has always used.
    std::string filter;
    if (settings.framed) {
        const std::string crop = ClipFramingCropFilter(
            settings.framing, settings.source_width, settings.source_height);
        if (!crop.empty()) filter = crop + ',';
        filter += "scale=w=" + std::to_string(settings.framing.width) +
                  ":h=" + std::to_string(settings.framing.height);
        // EF3 -- the blur on the picture alone, before the frame's black
        // is laid around it (ClipBlur.h).
        if (!settings.blur_filter.empty()) {
            filter += std::string(",format=") +
                      (settings.blur_rgb ? "gbrap16le" : "yuva444p16le") + ',' +
                      settings.blur_filter;
        }
        // PA8 -- the mask after the blur, then the picture brought onto
        // the black it will be padded with: a JPEG carries no alpha.
        if (!settings.mask_filter.empty()) {
            filter += std::string(",format=") +
                      (settings.blur_rgb ? "gbrap16le" : "yuva444p16le") + ',' +
                      settings.mask_filter + ",premultiply=inplace=1";
        }
        // PA15 -- the matte of the frame, the picture's second input, read
        // through the crop and the scale the picture went through.
        if (!settings.matte_path.empty()) {
            const std::string matteChain =
                (crop.empty() ? std::string() : crop + ",") +
                "scale=w=" + std::to_string(settings.framing.width) +
                ":h=" + std::to_string(settings.framing.height);
            filter += std::string(",format=") +
                      (settings.blur_rgb ? "gbrap16le" : "yuva444p16le") + ',' +
                      person_matte::MatteFilter(
                          1, settings.source_width, settings.source_height,
                          matteChain, settings.matte_inverted, "0") +
                      ",premultiply=inplace=1";
        }
        filter += ",pad=" + std::to_string(settings.frame_width) + ':' +
                  std::to_string(settings.frame_height) + ':' +
                  std::to_string(settings.framing.x) + ':' +
                  std::to_string(settings.framing.y) + ":black";
    } else {
        filter = "scale=w=" + bound + ":h=" + bound +
                 ":force_original_aspect_ratio=decrease";
    }
    if (settings.color_management.enabled) {
        std::string cube;
        if (!BuildOpenColorIoCube(
                ColorManagementForSdrPreview(settings.color_management), cube,
                error)) {
            error = "unable to build the frame colour transform: " + error;
            return false;
        }
        std::ofstream lut(lutPath, std::ios::binary | std::ios::trunc);
        lut << cube;
        lut.close();
        if (!lut) {
            std::filesystem::remove(lutPath, filesystemError);
            error = "unable to create the frame colour transform";
            return false;
        }
        filter += ',' + ColorLutFilter(settings.color_management,
                                       lutPath.string(), "bt709");
    }
    filter += ',';
    filter += kJpegFromVideoFilter;
    std::vector<std::string> arguments = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        // Before -i: seeks by keyframe first, which is what makes this fast
        // on a long rush. The frame returned is still the one containing the
        // requested time, because -ss before the input is followed by exact
        // decoding up to it in current FFmpeg.
        "-ss",
        FfmpegSeekSeconds(time),
        "-i",
        inputPath,
    };
    // PA15 -- the matte of the frame as a second input: the graph then
    // names its two inputs and its output.
    const std::filesystem::path mattePicture =
        std::filesystem::temp_directory_path() /
        ("cutmachine-frame-matte-" + GenerateUlid() + ".pgm");
    if (!settings.matte_path.empty()) {
        if (!person_matte::WriteFramePgm(settings.matte_path,
                                         settings.matte_frame,
                                         mattePicture.string(), error)) {
            std::filesystem::remove(lutPath, filesystemError);
            return false;
        }
        arguments.insert(arguments.end(),
                         {"-i", mattePicture.string(), "-filter_complex",
                          "[0:v:0]" + filter + "[out]", "-map", "[out]"});
    } else {
        arguments.insert(arguments.end(), {"-map", "0:v:0", "-vf", filter});
    }
    arguments.insert(arguments.end(), {"-an", "-frames:v", "1", "-q:v",
                                       std::to_string(settings.jpeg_quality),
                                       "-y", temporary.string()});
    const bool rendered = RunFfmpeg(arguments, error);
    std::filesystem::remove(lutPath, filesystemError);
    std::filesystem::remove(mattePicture, filesystemError);
    if (!rendered) {
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::ifstream input(temporary, std::ios::binary);
    if (!input) {
        std::filesystem::remove(temporary, filesystemError);
        error = "unable to read the rendered frame";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    input.close();
    std::filesystem::remove(temporary, filesystemError);
    jpegBytes = buffer.str();
    if (jpegBytes.empty()) {
        error = "the source produced no frame at that position";
        return false;
    }
    return true;
}

bool EncodeDisplayPictureAsJpeg(const std::vector<std::array<float, 3>>& rgb,
                                int64_t width, int64_t height,
                                const FrameCaptureSettings& settings,
                                std::string& jpegBytes, std::string& error) {
    error.clear();
    jpegBytes.clear();
    if (width <= 0 || height <= 0 ||
        rgb.size() != static_cast<size_t>(width) * height) {
        error = "the picture does not match its size";
        return false;
    }
    if (settings.jpeg_quality < 2 || settings.jpeg_quality > 31) {
        error = "invalid frame capture settings";
        return false;
    }
    std::error_code filesystemError;
    const std::string stem = "cutmachine-graded-" + GenerateUlid();
    const std::filesystem::path raw =
        std::filesystem::temp_directory_path() / (stem + ".rgb");
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() / (stem + ".jpg");
    {
        // 16 bits per channel, little-endian: the display signal loses
        // nothing a JPEG could keep.
        std::string bytes;
        bytes.resize(rgb.size() * 6);
        size_t offset = 0;
        for (const std::array<float, 3>& pixel : rgb) {
            for (float channel : pixel) {
                const auto code = static_cast<uint16_t>(
                    std::lround(std::clamp(channel, 0.0f, 1.0f) * 65535.0f));
                bytes[offset++] = static_cast<char>(code & 0xff);
                bytes[offset++] = static_cast<char>(code >> 8);
            }
        }
        std::ofstream output(raw, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            std::filesystem::remove(raw, filesystemError);
            error = "unable to write the graded picture";
            return false;
        }
    }
    std::string filter;
    if (settings.framed) {
        filter = "pad=" + std::to_string(settings.frame_width) + ':' +
                 std::to_string(settings.frame_height) + ':' +
                 std::to_string(settings.framing.x) + ':' +
                 std::to_string(settings.framing.y) + ":black,";
    }
    // RGB in, so no source matrix to name: only the JPEG's own.
    filter += "scale=out_color_matrix=bt601:out_range=pc,format=yuvj420p";
    const std::vector<std::string> arguments = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb48le",
        "-s",
        std::to_string(width) + 'x' + std::to_string(height),
        "-i",
        raw.string(),
        "-frames:v",
        "1",
        "-vf",
        filter,
        "-q:v",
        std::to_string(settings.jpeg_quality),
        "-y",
        temporary.string(),
    };
    const bool rendered = RunFfmpeg(arguments, error);
    std::filesystem::remove(raw, filesystemError);
    if (!rendered) {
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::ifstream input(temporary, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    input.close();
    std::filesystem::remove(temporary, filesystemError);
    jpegBytes = buffer.str();
    if (jpegBytes.empty()) {
        error = "FFmpeg produced no picture";
        return false;
    }
    return true;
}

std::string EncodeBase64(const std::string& bytes) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    size_t index = 0;
    while (index + 2 < bytes.size()) {
        const uint32_t triple =
            (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index]))
             << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 1]))
             << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 2]));
        output.push_back(kAlphabet[(triple >> 18) & 0x3f]);
        output.push_back(kAlphabet[(triple >> 12) & 0x3f]);
        output.push_back(kAlphabet[(triple >> 6) & 0x3f]);
        output.push_back(kAlphabet[triple & 0x3f]);
        index += 3;
    }
    const size_t remaining = bytes.size() - index;
    if (remaining == 1) {
        const uint32_t value =
            static_cast<uint32_t>(static_cast<unsigned char>(bytes[index]));
        output.push_back(kAlphabet[(value >> 2) & 0x3f]);
        output.push_back(kAlphabet[(value << 4) & 0x3f]);
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2) {
        const uint32_t value =
            (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index]))
             << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 1]));
        output.push_back(kAlphabet[(value >> 10) & 0x3f]);
        output.push_back(kAlphabet[(value >> 4) & 0x3f]);
        output.push_back(kAlphabet[(value << 2) & 0x3f]);
        output.push_back('=');
    }
    return output;
}
