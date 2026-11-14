#include "Proxy.h"

#include "FfmpegBinary.h"
#include "Ulid.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

bool GenerateMediaProxy(const std::string& inputPath,
                        const std::string& outputPath,
                        const RationalTime& duration,
                        const ProxySettings& settings,
                        MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 || settings.max_width < 64) {
        error = "invalid proxy settings or source duration";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error =
            "unable to create proxy directory: " + filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-noautorotate",
        "-i",
        inputPath,
        "-map",
        "0:v:0",
        "-an",
        "-vf",
        "scale='min(" + std::to_string(settings.max_width) +
            ",iw)':-2:flags=lanczos",
        "-c:v",
        "prores_ks",
        "-profile:v",
        "0",
        "-pix_fmt",
        "yuv422p10le",
        "-vendor",
        "apl0",
        "-progress",
        "pipe:1",
        "-nostats",
        temporary.string(),
    };

    const long double durationSeconds =
        static_cast<long double>(duration.value) / duration.rate;
    std::string progressBuffer;
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 4096;
    request.error_tail_bytes = 8192;
    request.cancelled = [&context]() { return context.Cancelled(); };
    request.on_output = [&](const char* bytes, size_t count) {
        progressBuffer.append(bytes, count);
        size_t newline = 0;
        while ((newline = progressBuffer.find('\n')) != std::string::npos) {
            const std::string line = progressBuffer.substr(0, newline);
            progressBuffer.erase(0, newline + 1);
            constexpr const char* prefix = "out_time_us=";
            if (line.rfind(prefix, 0) == 0) {
                try {
                    const int64_t microseconds =
                        std::stoll(line.substr(std::strlen(prefix)));
                    context.SetProgress(
                        durationSeconds <= 0.0L
                            ? 0.0
                            : static_cast<double>(microseconds / 1000000.0L /
                                                  durationSeconds),
                        "Encoding ProRes Proxy");
                } catch (...) {
                }
            }
        }
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "proxy process pipes");
        return false;
    }
    if (result.cancelled || context.Cancelled()) {
        error = "proxy cancelled";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty() ? "FFmpeg proxy generation failed"
                                          : result.error_tail;
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "unable to install proxy: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    context.SetProgress(1.0, "Proxy ready");
    return true;
}
