#include "FfmpegPipe.h"

#include "FfmpegBinary.h"

#include <utility>

bool ReadFfmpegRawFrames(std::vector<std::string> arguments, size_t frameBytes,
                         const std::function<void(const uint8_t*)>& onFrame,
                         std::string& error, std::string* diagnostics) {
    if (arguments.empty() || frameBytes == 0) {
        error = "invalid FFmpeg frame reader arguments";
        return false;
    }
    std::vector<uint8_t> pending;
    process::Request request;
    request.arguments = std::move(arguments);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 65536;
    request.error_tail_bytes = 16384;
    request.on_output = [&](const char* bytes, size_t count) {
        pending.insert(pending.end(), bytes, bytes + count);
        size_t offset = 0;
        while (pending.size() - offset >= frameBytes) {
            onFrame(pending.data() + offset);
            offset += frameBytes;
        }
        pending.erase(pending.begin(), pending.begin() + offset);
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "FFmpeg pipes");
        return false;
    }
    if (diagnostics != nullptr) {
        *diagnostics = result.error_tail;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty() ? "FFmpeg decode failed"
                                          : result.error_tail;
        return false;
    }
    return true;
}
