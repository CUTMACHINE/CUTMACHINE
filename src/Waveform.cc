#include "Waveform.h"

#include "FfmpegBinary.h"
#include "Ulid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace {

constexpr char kMagic[8] = {'C', 'M', 'W', 'A', 'V', 'E', '1', '\0'};
constexpr uint64_t kMaximumPeaks = 24ULL * 60ULL * 60ULL * 200ULL;

bool SaveAudioWaveform(const std::filesystem::path& destination,
                       const AudioWaveform& waveform, std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error =
            "unable to create waveform directory: " + filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.filename().string() + ".partial-" + GenerateUlid());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const uint64_t count = waveform.peaks.size();
    output.write(kMagic, sizeof(kMagic));
    output.write(reinterpret_cast<const char*>(&waveform.peaks_per_second),
                 sizeof(waveform.peaks_per_second));
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    if (!waveform.peaks.empty())
        output.write(reinterpret_cast<const char*>(waveform.peaks.data()),
                     static_cast<std::streamsize>(waveform.peaks.size() *
                                                  sizeof(float)));
    output.close();
    if (!output) {
        error = "unable to write waveform cache";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error =
            "unable to install waveform cache: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

}  // namespace

bool GenerateAudioWaveform(const std::string& inputPath,
                           const std::string& outputPath,
                           const RationalTime& duration,
                           const WaveformSettings& settings,
                           MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 ||
        settings.peaks_per_second == 0 ||
        settings.decode_sample_rate < settings.peaks_per_second ||
        settings.decode_sample_rate % settings.peaks_per_second != 0) {
        error = "invalid waveform settings or source duration";
        return false;
    }
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-i",
        inputPath,
        "-map",
        "0:a:0",
        "-vn",
        "-ac",
        "1",
        "-ar",
        std::to_string(settings.decode_sample_rate),
        "-f",
        "f32le",
        "pipe:1",
    };
    AudioWaveform waveform;
    waveform.peaks_per_second = settings.peaks_per_second;
    const uint32_t samplesPerPeak =
        settings.decode_sample_rate / settings.peaks_per_second;
    uint32_t samplesInPeak = 0;
    float peak = 0.0f;
    uint64_t processedSamples = 0;
    std::vector<char> pendingBytes;
    const long double expectedSamples =
        static_cast<long double>(duration.value) / duration.rate *
        settings.decode_sample_rate;
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 8192;
    request.error_tail_bytes = 8192;
    request.cancelled = [&context]() { return context.Cancelled(); };
    request.on_output = [&](const char* bytes, size_t count) {
        pendingBytes.insert(pendingBytes.end(), bytes, bytes + count);
        size_t offset = 0;
        while (pendingBytes.size() - offset >= sizeof(float)) {
            float sample = 0.0f;
            std::memcpy(&sample, pendingBytes.data() + offset, sizeof(sample));
            offset += sizeof(sample);
            if (std::isfinite(sample)) {
                peak = std::max(peak, std::min(1.0f, std::abs(sample)));
            }
            ++samplesInPeak;
            ++processedSamples;
            if (samplesInPeak == samplesPerPeak) {
                waveform.peaks.push_back(peak);
                samplesInPeak = 0;
                peak = 0.0f;
            }
        }
        pendingBytes.erase(pendingBytes.begin(), pendingBytes.begin() + offset);
        context.SetProgress(
            expectedSamples <= 0.0L
                ? 0.0
                : static_cast<double>(processedSamples / expectedSamples),
            "Analyse audio");
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "waveform process pipes");
        return false;
    }
    if (result.cancelled || context.Cancelled()) {
        error = "waveform cancelled";
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty() ? "FFmpeg waveform analysis failed"
                                          : result.error_tail;
        return false;
    }
    if (samplesInPeak != 0) waveform.peaks.push_back(peak);
    if (waveform.peaks.empty()) {
        error = "audio stream produced no waveform samples";
        return false;
    }
    if (!SaveAudioWaveform(outputPath, waveform, error)) return false;
    context.SetProgress(1.0, "Waveform prête");
    return true;
}

bool LoadAudioWaveform(const std::string& path, AudioWaveform& waveform,
                       std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open waveform cache";
        return false;
    }
    char magic[sizeof(kMagic)]{};
    uint32_t peaksPerSecond = 0;
    uint64_t count = 0;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&peaksPerSecond),
               sizeof(peaksPerSecond));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!input || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        peaksPerSecond == 0 || peaksPerSecond > 1000 || count == 0 ||
        count > kMaximumPeaks) {
        error = "invalid waveform cache header";
        return false;
    }
    AudioWaveform parsed;
    parsed.peaks_per_second = peaksPerSecond;
    parsed.peaks.resize(static_cast<size_t>(count));
    input.read(
        reinterpret_cast<char*>(parsed.peaks.data()),
        static_cast<std::streamsize>(parsed.peaks.size() * sizeof(float)));
    if (!input) {
        error = "truncated waveform cache";
        return false;
    }
    for (float peak : parsed.peaks) {
        if (!std::isfinite(peak) || peak < 0.0f || peak > 1.0f) {
            error = "invalid waveform peak";
            return false;
        }
    }
    waveform = std::move(parsed);
    return true;
}
