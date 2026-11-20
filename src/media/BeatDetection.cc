#include "BeatDetection.h"

#include "FfmpegBinary.h"
#include "Fft.h"
#include "Ulid.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr char kMagic[8] = {'C', 'M', 'B', 'E', 'A', 'T', '1', '\0'};
constexpr uint64_t kMaximumOnsets = 2ULL * 1000ULL * 1000ULL;
// The band envelope is a trailing section of the cache, after the tempo: a
// reader that predates it stops before it, and a cache that predates it
// simply ends there.
constexpr char kBandsTag[8] = {'C', 'M', 'B', 'A', 'N', 'D', '1', '\0'};
constexpr uint64_t kMaximumBandFrames = 64ULL * 1000ULL * 1000ULL;
constexpr double kLowBandTopHz = 150.0;
constexpr double kHighBandBottomHz = 4000.0;

int32_t CentiDecibels(double power) {
    // A floor well under anything an encoded track holds, so silence has a
    // number rather than minus infinity.
    return static_cast<int32_t>(
        std::lround(1000.0 * std::log10(std::max(power, 1e-12))));
}

// The single point where an analysis position becomes a RationalTime.
// `sampleIndex` is already an exact integer count of decoded PCM samples at
// `sampleRate` (a frame index times an integer hop size) — nothing here is
// ever a rounded float. TimelineViewport is documented in
// docs/reference/decisions-de-temps.md as the project's one time/pixel
// frontier; this is the audio-analysis equivalent,
// the one place a sample offset becomes a time.
RationalTime SampleIndexToTime(int64_t sampleIndex, int32_t sampleRate) {
    return RationalTime(sampleIndex, sampleRate);
}

// Peak-picks the novelty function into onsets, then estimates a tempo by
// autocorrelating it. See BeatDetection.h for the method and references.
BeatDetectionResult AnalyzeNovelty(const std::vector<float>& novelty,
                                   const std::vector<int64_t>& frameSampleIndex,
                                   uint32_t hopSize, uint32_t sampleRate,
                                   const BeatDetectionSettings& settings) {
    BeatDetectionResult result;
    const int64_t frameCount = static_cast<int64_t>(novelty.size());
    if (frameCount == 0) return result;
    const double hopSeconds =
        static_cast<double>(hopSize) / static_cast<double>(sampleRate);

    // --- Onset peak picking -------------------------------------------
    const int64_t localMaxRadius =
        std::max<int64_t>(1, std::llround(0.03 / hopSeconds));  // ~30ms
    const int64_t thresholdRadius =
        std::max<int64_t>(localMaxRadius, std::llround(0.5 / hopSeconds));
    const int64_t minimumGapFrames = std::max<int64_t>(
        1, std::llround(settings.minimum_onset_interval_seconds / hopSeconds));

    std::vector<double> prefixSum(frameCount + 1, 0.0);
    std::vector<double> prefixSumSquares(frameCount + 1, 0.0);
    float globalMax = 0.0f;
    for (int64_t i = 0; i < frameCount; ++i) {
        const double value = novelty[static_cast<size_t>(i)];
        prefixSum[i + 1] = prefixSum[i] + value;
        prefixSumSquares[i + 1] = prefixSumSquares[i] + value * value;
        globalMax = std::max(globalMax, novelty[static_cast<size_t>(i)]);
    }

    int64_t lastAccepted = std::numeric_limits<int64_t>::min() / 2;
    for (int64_t n = 0; n < frameCount; ++n) {
        const float here = novelty[static_cast<size_t>(n)];
        if (here <= 0.0f) continue;

        const int64_t maxLow = std::max<int64_t>(0, n - localMaxRadius);
        const int64_t maxHigh =
            std::min<int64_t>(frameCount - 1, n + localMaxRadius);
        bool isLocalMax = true;
        for (int64_t k = maxLow; k <= maxHigh && isLocalMax; ++k)
            if (k != n && novelty[static_cast<size_t>(k)] > here)
                isLocalMax = false;
        if (!isLocalMax) continue;

        const int64_t lowWindow = std::max<int64_t>(0, n - thresholdRadius);
        const int64_t highWindow =
            std::min<int64_t>(frameCount - 1, n + thresholdRadius);
        const int64_t windowCount = highWindow - lowWindow + 1;
        const double mean =
            (prefixSum[highWindow + 1] - prefixSum[lowWindow]) / windowCount;
        const double meanSquares =
            (prefixSumSquares[highWindow + 1] - prefixSumSquares[lowWindow]) /
            windowCount;
        const double variance = std::max(0.0, meanSquares - mean * mean);
        const double threshold =
            mean + static_cast<double>(settings.onset_sensitivity) *
                       std::sqrt(variance);
        if (static_cast<double>(here) <= threshold) continue;
        if (n - lastAccepted < minimumGapFrames) continue;

        lastAccepted = n;
        AudioOnset onset;
        onset.time = SampleIndexToTime(frameSampleIndex[static_cast<size_t>(n)],
                                       static_cast<int32_t>(sampleRate));
        onset.strength =
            globalMax > 0.0f ? std::clamp(here / globalMax, 0.0f, 1.0f) : 0.0f;
        result.onsets.push_back(onset);
    }

    // --- Tempo via autocorrelation of the novelty function --------------
    const int64_t minLagFrames = std::max<int64_t>(
        1, std::llround((60.0 / settings.maximum_bpm) / hopSeconds));
    const int64_t maxLagFrames =
        std::llround((60.0 / settings.minimum_bpm) / hopSeconds);
    const int64_t searchHighLag = std::min(maxLagFrames, frameCount - 1);
    if (result.onsets.size() >= 4 && searchHighLag > minLagFrames &&
        frameCount > 2 * minLagFrames) {
        double mean = 0.0;
        for (float value : novelty) mean += value;
        mean /= static_cast<double>(frameCount);
        std::vector<double> centered(static_cast<size_t>(frameCount));
        for (int64_t i = 0; i < frameCount; ++i)
            centered[static_cast<size_t>(i)] =
                static_cast<double>(novelty[static_cast<size_t>(i)]) - mean;

        double zeroLag = 0.0;
        for (double value : centered) zeroLag += value * value;
        zeroLag /= static_cast<double>(frameCount);

        double bestCorrelation = -std::numeric_limits<double>::infinity();
        int64_t bestLag = -1;
        for (int64_t lag = minLagFrames; lag <= searchHighLag; ++lag) {
            const int64_t count = frameCount - lag;
            double sum = 0.0;
            for (int64_t n = 0; n < count; ++n)
                sum += centered[static_cast<size_t>(n)] *
                       centered[static_cast<size_t>(n + lag)];
            sum /= static_cast<double>(count);
            if (sum > bestCorrelation) {
                bestCorrelation = sum;
                bestLag = lag;
            }
        }
        if (bestLag > 0) {
            const double periodSeconds =
                static_cast<double>(bestLag) * hopSeconds;
            result.bpm = 60.0 / periodSeconds;
            result.bpm_confidence =
                zeroLag > 1e-12
                    ? std::clamp(bestCorrelation / zeroLag, 0.0, 1.0)
                    : 0.0;
        }
    }

    return result;
}

bool SaveAudioBeats(const std::filesystem::path& destination,
                    const BeatDetectionResult& result, uint32_t sampleRate,
                    std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create beat detection directory: " +
                filesystemError.message();
        return false;
    }
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.filename().string() + ".partial-" + GenerateUlid());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(kMagic, sizeof(kMagic));
    output.write(reinterpret_cast<const char*>(&sampleRate),
                 sizeof(sampleRate));
    const uint64_t onsetCount = result.onsets.size();
    output.write(reinterpret_cast<const char*>(&onsetCount),
                 sizeof(onsetCount));
    for (const AudioOnset& onset : result.onsets) {
        const int64_t sampleValue = onset.time.value;
        output.write(reinterpret_cast<const char*>(&sampleValue),
                     sizeof(sampleValue));
        output.write(reinterpret_cast<const char*>(&onset.strength),
                     sizeof(onset.strength));
    }
    const uint8_t hasBpm = result.bpm.has_value() ? 1 : 0;
    output.write(reinterpret_cast<const char*>(&hasBpm), sizeof(hasBpm));
    const double bpmValue = result.bpm.value_or(0.0);
    output.write(reinterpret_cast<const char*>(&bpmValue), sizeof(bpmValue));
    output.write(reinterpret_cast<const char*>(&result.bpm_confidence),
                 sizeof(result.bpm_confidence));
    if (result.bands) {
        const AudioBandEnvelope& bands = *result.bands;
        output.write(kBandsTag, sizeof(kBandsTag));
        output.write(reinterpret_cast<const char*>(&bands.sample_rate),
                     sizeof(bands.sample_rate));
        output.write(reinterpret_cast<const char*>(&bands.hop_size),
                     sizeof(bands.hop_size));
        output.write(reinterpret_cast<const char*>(&bands.window_size),
                     sizeof(bands.window_size));
        const uint64_t frames = bands.full_centi_db.size();
        output.write(reinterpret_cast<const char*>(&frames), sizeof(frames));
        for (const std::vector<int32_t>* band :
             {&bands.low_centi_db, &bands.high_centi_db, &bands.full_centi_db})
            output.write(
                reinterpret_cast<const char*>(band->data()),
                static_cast<std::streamsize>(frames * sizeof(int32_t)));
    }
    output.close();
    if (!output) {
        error = "unable to write beat detection cache";
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        error = "unable to install beat detection cache: " +
                filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

}  // namespace

bool DetectAudioBeats(const std::string& inputPath,
                      const std::string& outputPath,
                      const RationalTime& duration,
                      const BeatDetectionSettings& settings,
                      MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 ||
        settings.decode_sample_rate == 0 ||
        settings.decode_sample_rate >
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        !fft::IsPowerOfTwo(settings.fft_size) || settings.hop_size == 0 ||
        settings.hop_size > settings.fft_size ||
        settings.minimum_onset_interval_seconds < 0.0 ||
        settings.minimum_bpm <= 0.0 ||
        settings.maximum_bpm <= settings.minimum_bpm) {
        error = "invalid beat detection settings or source duration";
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
    // STFT state. `accum` holds decoded samples not yet consumed into a
    // frame; frames overlap, so only `hop_size` samples are dropped off the
    // front after each one. `nextFrameStart` is the absolute sample index
    // (at decode_sample_rate) of accum[0], carried as an exact integer.
    std::vector<double> hannWindow(settings.fft_size);
    for (uint32_t i = 0; i < settings.fft_size; ++i)
        hannWindow[i] =
            0.5 - 0.5 * std::cos(2.0 * fft::kPi * i /
                                 static_cast<double>(settings.fft_size - 1));
    std::vector<float> accum;
    int64_t nextFrameStart = 0;
    std::vector<double> previousMagnitude;
    std::vector<float> novelty;
    std::vector<int64_t> frameSampleIndex;
    AudioBandEnvelope bands;
    bands.sample_rate = settings.decode_sample_rate;
    bands.hop_size = settings.hop_size;
    bands.window_size = settings.fft_size;
    const double binHz = static_cast<double>(settings.decode_sample_rate) /
                         static_cast<double>(settings.fft_size);

    auto processReadySTFTFrames = [&]() {
        while (accum.size() >= settings.fft_size) {
            std::vector<std::complex<double>> spectrum(settings.fft_size);
            for (uint32_t i = 0; i < settings.fft_size; ++i)
                spectrum[i] = accum[i] * hannWindow[i];
            fft::Forward(spectrum);
            std::vector<double> magnitude(settings.fft_size / 2 + 1);
            double low = 0.0;
            double high = 0.0;
            double full = 0.0;
            for (size_t k = 0; k < magnitude.size(); ++k) {
                magnitude[k] = std::abs(spectrum[k]);
                const double power = magnitude[k] * magnitude[k];
                const double hz = static_cast<double>(k) * binHz;
                full += power;
                if (k > 0 && hz < kLowBandTopHz) low += power;
                if (hz >= kHighBandBottomHz) high += power;
            }
            bands.low_centi_db.push_back(CentiDecibels(low));
            bands.high_centi_db.push_back(CentiDecibels(high));
            bands.full_centi_db.push_back(CentiDecibels(full));
            if (!previousMagnitude.empty()) {
                double flux = 0.0;
                for (size_t k = 0; k < magnitude.size(); ++k) {
                    const double diff = magnitude[k] - previousMagnitude[k];
                    if (diff > 0.0) flux += diff;
                }
                novelty.push_back(static_cast<float>(flux));
                frameSampleIndex.push_back(nextFrameStart);
            }
            previousMagnitude = std::move(magnitude);
            accum.erase(accum.begin(), accum.begin() + settings.hop_size);
            nextFrameStart += settings.hop_size;
        }
    };

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
            accum.push_back(std::isfinite(sample) ? sample : 0.0f);
            ++processedSamples;
        }
        pendingBytes.erase(pendingBytes.begin(), pendingBytes.begin() + offset);
        processReadySTFTFrames();
        context.SetProgress(
            expectedSamples <= 0.0L
                ? 0.0
                : static_cast<double>(processedSamples / expectedSamples),
            "Analyse des beats");
    };
    const process::Result launch = ffmpeg::Run(std::move(request));
    if (!launch.Started()) {
        error = ffmpeg::StartError(launch, "beat detection process pipes");
        return false;
    }
    if (launch.cancelled || context.Cancelled()) {
        error = "beat detection cancelled";
        return false;
    }
    if (!launch.Succeeded()) {
        error = launch.error_tail.empty()
                    ? "FFmpeg beat detection analysis failed"
                    : launch.error_tail;
        return false;
    }
    if (novelty.empty()) {
        error = "audio stream too short for beat detection";
        return false;
    }

    BeatDetectionResult result =
        AnalyzeNovelty(novelty, frameSampleIndex, settings.hop_size,
                       settings.decode_sample_rate, settings);
    result.bands = std::move(bands);
    if (!SaveAudioBeats(outputPath, result, settings.decode_sample_rate, error))
        return false;
    context.SetProgress(1.0, "Beats prêts");
    return true;
}

bool LoadAudioBeats(const std::string& path, BeatDetectionResult& result,
                    std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open beat detection cache";
        return false;
    }
    char magic[sizeof(kMagic)]{};
    uint32_t sampleRate = 0;
    uint64_t onsetCount = 0;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&sampleRate), sizeof(sampleRate));
    input.read(reinterpret_cast<char*>(&onsetCount), sizeof(onsetCount));
    if (!input || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        sampleRate == 0 ||
        sampleRate >
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        onsetCount > kMaximumOnsets) {
        error = "invalid beat detection cache header";
        return false;
    }
    BeatDetectionResult parsed;
    parsed.onsets.reserve(static_cast<size_t>(onsetCount));
    for (uint64_t i = 0; i < onsetCount; ++i) {
        int64_t sampleValue = 0;
        float strength = 0.0f;
        input.read(reinterpret_cast<char*>(&sampleValue), sizeof(sampleValue));
        input.read(reinterpret_cast<char*>(&strength), sizeof(strength));
        if (!input || sampleValue < 0 ||
            (!parsed.onsets.empty() &&
             sampleValue <= parsed.onsets.back().time.value) ||
            !std::isfinite(strength) || strength < 0.0f || strength > 1.0f) {
            error = "invalid beat detection cache onset";
            return false;
        }
        AudioOnset onset;
        onset.time =
            RationalTime(sampleValue, static_cast<int32_t>(sampleRate));
        onset.strength = strength;
        parsed.onsets.push_back(onset);
    }
    uint8_t hasBpm = 0;
    double bpmValue = 0.0;
    double bpmConfidence = 0.0;
    input.read(reinterpret_cast<char*>(&hasBpm), sizeof(hasBpm));
    input.read(reinterpret_cast<char*>(&bpmValue), sizeof(bpmValue));
    input.read(reinterpret_cast<char*>(&bpmConfidence), sizeof(bpmConfidence));
    if (!input || (hasBpm != 0 && hasBpm != 1) ||
        !std::isfinite(bpmConfidence) || bpmConfidence < 0.0 ||
        bpmConfidence > 1.0) {
        error = "invalid beat detection cache tempo";
        return false;
    }
    if (hasBpm == 1) {
        if (!std::isfinite(bpmValue) || bpmValue <= 0.0 || bpmValue > 1000.0) {
            error = "invalid beat detection cache tempo";
            return false;
        }
        parsed.bpm = bpmValue;
    }
    parsed.bpm_confidence = bpmConfidence;
    char tag[sizeof(kBandsTag)]{};
    input.read(tag, sizeof(tag));
    if (input && std::memcmp(tag, kBandsTag, sizeof(kBandsTag)) == 0) {
        AudioBandEnvelope bands;
        uint64_t frames = 0;
        input.read(reinterpret_cast<char*>(&bands.sample_rate),
                   sizeof(bands.sample_rate));
        input.read(reinterpret_cast<char*>(&bands.hop_size),
                   sizeof(bands.hop_size));
        input.read(reinterpret_cast<char*>(&bands.window_size),
                   sizeof(bands.window_size));
        input.read(reinterpret_cast<char*>(&frames), sizeof(frames));
        if (!input || bands.sample_rate != sampleRate || bands.hop_size == 0 ||
            bands.window_size < bands.hop_size || frames > kMaximumBandFrames) {
            error = "invalid beat detection cache band envelope";
            return false;
        }
        for (std::vector<int32_t>* band :
             {&bands.low_centi_db, &bands.high_centi_db,
              &bands.full_centi_db}) {
            band->resize(static_cast<size_t>(frames));
            input.read(reinterpret_cast<char*>(band->data()),
                       static_cast<std::streamsize>(frames * sizeof(int32_t)));
        }
        if (!input) {
            error = "invalid beat detection cache band envelope";
            return false;
        }
        parsed.bands = std::move(bands);
    }
    result = std::move(parsed);
    return true;
}
