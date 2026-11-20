#include "Thumbnail.h"

#include "FfmpegBinary.h"
#include "FfmpegCapabilities.h"
#include "Ulid.h"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <vector>

bool GenerateMediaThumbnail(const std::string& inputPath,
                            const std::string& outputPath,
                            const RationalTime& duration,
                            const ThumbnailSettings& settings,
                            MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 || settings.width < 64 ||
        settings.height < 36) {
        error = "invalid thumbnail settings or source duration";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create thumbnail directory: " +
                filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    const long double seconds =
        static_cast<long double>(duration.value) / duration.rate;
    const double seek = static_cast<double>(std::min<long double>(
        10.0L, std::max<long double>(0.0L, seconds * 0.1L)));
    const std::string filter =
        "scale=" + std::to_string(settings.width) + ":" +
        std::to_string(settings.height) +
        ":force_original_aspect_ratio=decrease:flags=lanczos,pad=" +
        std::to_string(settings.width) + ":" + std::to_string(settings.height) +
        ":(ow-iw)/2:(oh-ih)/2:color=0x202124";
    // HEADLESS-2026-10 H2 -- VideoToolbox (HW-2026-08, below) is asked only
    // of a binary that has it: one without it (Linux, an FFmpeg configured
    // without it) does not decode in software instead, it refuses the whole
    // command ("Device creation failed"). Its encoders tell, from the probe
    // kept for the process: --enable-videotoolbox brings the decoder and the
    // h264_videotoolbox encoder together.
    std::string binary;
    std::string unresolved;
    const bool videoToolbox =
        ffmpeg::Executable(settings.ffmpeg_path, binary, unresolved) &&
        ffmpeg::ProbeBinary(binary).encoders.count("h264_videotoolbox") > 0;
    std::vector<std::string> storage = {settings.ffmpeg_path, "-hide_banner",
                                        "-loglevel", "error", "-nostdin"};
    if (videoToolbox) {
        // HW-2026-08 -- VideoToolbox decodes the poster frame instead of
        // the CPU. Measured on a 4K H.264 4:2:2 10-bit rush, warm cache, three
        // runs: same wall time (~0.4 s), CPU down from 0.85 s to 0.33 s, for a
        // byte-identical JPEG. The win is CPU headroom, not speed -- worth it
        // here because thumbnails are generated for a whole library at once,
        // in the background, while the user expects the app to stay
        // responsive.
        //
        // Deliberately NOT applied to ShotQuality.cc: that command decodes
        // every frame and downscales it, so the hardware path has to copy each
        // 4K frame back to system memory for the filter chain. Measured there:
        // 1.5x SLOWER in wall time for 45% less CPU, which is the wrong trade
        // for a batch analysis over a whole library.
        //
        // On a codec VideoToolbox cannot take, FFmpeg prints one line to
        // stderr and decodes in software anyway -- verified on mpeg4, same
        // bytes out, exit status 0. Success is decided by the exit status,
        // never by stderr content, so that warning is harmless.
        storage.insert(storage.end(), {"-hwaccel", "videotoolbox"});
    }
    storage.insert(
        storage.end(),
        {"-y", "-ss", std::to_string(seek), "-i", inputPath, "-map", "0:v:0",
         "-an", "-frames:v", "1", "-vf", filter, temporary.string()});

    // Standard output stays the process's own: the picture goes to a file,
    // and only the error output is read.
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 4096;
    request.error_tail_bytes = 8192;
    request.cancelled = [&context]() { return context.Cancelled(); };
    request.on_started = [&context]() {
        context.SetProgress(0.1, "Extraction de l'image");
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "thumbnail process pipe");
        return false;
    }
    if (result.cancelled || context.Cancelled()) {
        error = "thumbnail cancelled";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty() ? "FFmpeg thumbnail generation failed"
                                          : result.error_tail;
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "unable to install thumbnail: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    context.SetProgress(1.0, "Thumbnail prête");
    return true;
}
