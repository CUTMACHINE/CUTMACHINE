#include "ShotQuality.h"

#include "Fft.h"

#include "ClipEnabled.h"
#include "FfmpegBinary.h"
#include "Json.h"
#include "SequenceFormat.h"
#include "Ulid.h"

#include <algorithm>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace {

using mcp_json::Value;

// Both metrics are accumulated in integers and divided once, at the end,
// through __int128. Nothing here ever holds an intermediate float: two runs
// over identical pixels produce identical integers on any host, which is
// what makes the cache file reproducible (PHILOSOPHY.md principle 6).
int64_t ScaleToMetric(__int128 numerator, __int128 denominator) {
    if (denominator <= 0) return 0;
    const __int128 scaled = numerator * kShotQualityMetricScale / denominator;
    if (scaled < 0) return 0;
    if (scaled > std::numeric_limits<int64_t>::max())
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(scaled);
}

bool ReadInt64Field(const Value& object, const std::string& key,
                    int64_t& output) {
    const Value* field = object.Find(key);
    return field != nullptr && field->AsInt64(output);
}

}  // namespace

const char* SharpnessGradeName(SharpnessGrade grade) {
    switch (grade) {
        case SharpnessGrade::Sharp:
            return "Sharp";
        case SharpnessGrade::Soft:
            return "Soft";
        case SharpnessGrade::Blurry:
            return "Blurry";
    }
    return "Sharp";
}

const char* SteadinessGradeName(SteadinessGrade grade) {
    switch (grade) {
        case SteadinessGrade::Steady:
            return "Steady";
        case SteadinessGrade::Moving:
            return "Moving";
        case SteadinessGrade::Shaky:
            return "Shaky";
    }
    return "Steady";
}

int64_t FrameSharpnessMetric(const uint8_t* luma, int32_t width,
                             int32_t height) {
    if (luma == nullptr || width < 3 || height < 3) return 0;
    // 3x3 Laplacian over the interior only: the border has no full
    // neighbourhood, and replicating edge pixels to fake one would inject a
    // zero response that drags the variance toward zero by an amount that
    // depends on the frame's aspect ratio rather than on its focus.
    const int64_t count =
        static_cast<int64_t>(width - 2) * static_cast<int64_t>(height - 2);
    __int128 sum = 0;
    __int128 sumOfSquares = 0;
    for (int32_t y = 1; y < height - 1; ++y) {
        const uint8_t* row = luma + static_cast<size_t>(y) * width;
        const uint8_t* above = row - width;
        const uint8_t* below = row + width;
        for (int32_t x = 1; x < width - 1; ++x) {
            const int32_t response = static_cast<int32_t>(above[x]) +
                                     static_cast<int32_t>(below[x]) +
                                     static_cast<int32_t>(row[x - 1]) +
                                     static_cast<int32_t>(row[x + 1]) -
                                     4 * static_cast<int32_t>(row[x]);
            sum += response;
            sumOfSquares += static_cast<__int128>(response) * response;
        }
    }
    // Population variance, kept exact: (n*sum2 - sum^2) / n^2 rather than
    // sum2/n - (sum/n)^2, which would round twice before subtracting.
    const __int128 numerator =
        static_cast<__int128>(count) * sumOfSquares - sum * sum;
    const __int128 denominator = static_cast<__int128>(count) * count;
    // Divided by 255^2 so the result is a fraction of full scale and does
    // not depend on the bit depth the analysis plane happens to use.
    return ScaleToMetric(numerator, denominator * 255 * 255);
}

int64_t FrameMotionMetric(const uint8_t* previous, const uint8_t* current,
                          int32_t width, int32_t height) {
    if (previous == nullptr || current == nullptr || width < 1 || height < 1)
        return 0;
    const int64_t count =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    __int128 total = 0;
    for (int64_t index = 0; index < count; ++index) {
        const int32_t difference = static_cast<int32_t>(current[index]) -
                                   static_cast<int32_t>(previous[index]);
        total += difference < 0 ? -difference : difference;
    }
    return ScaleToMetric(total, static_cast<__int128>(count) * 255);
}

namespace {

constexpr int32_t kMotionBlock = 64;

// One block's shift from `previous` to `current`, by phase correlation, with
// the normalised height of the correlation peak as its confidence.
bool BlockShift(const uint8_t* previous, const uint8_t* current, int32_t width,
                int32_t left, int32_t top, double& shiftX, double& shiftY,
                double& confidence) {
    using Complex = std::complex<double>;
    const int32_t n = kMotionBlock;
    std::vector<Complex> a(static_cast<size_t>(n) * n);
    std::vector<Complex> b(a.size());
    double meanA = 0.0;
    double meanB = 0.0;
    for (int32_t y = 0; y < n; ++y) {
        for (int32_t x = 0; x < n; ++x) {
            meanA += previous[((top + y) * width) + left + x];
            meanB += current[((top + y) * width) + left + x];
        }
    }
    meanA /= n * n;
    meanB /= n * n;
    for (int32_t y = 0; y < n; ++y) {
        const double wy = 0.5 - 0.5 * std::cos(2.0 * fft::kPi * y / (n - 1));
        for (int32_t x = 0; x < n; ++x) {
            const double wx =
                0.5 - 0.5 * std::cos(2.0 * fft::kPi * x / (n - 1));
            const size_t index = static_cast<size_t>(y) * n + x;
            a[index] =
                (previous[((top + y) * width) + left + x] - meanA) * wx * wy;
            b[index] =
                (current[((top + y) * width) + left + x] - meanB) * wx * wy;
        }
    }
    const auto transform = [&](std::vector<Complex>& plane) {
        std::vector<Complex> line(static_cast<size_t>(n));
        for (int32_t y = 0; y < n; ++y) {
            for (int32_t x = 0; x < n; ++x) line[x] = plane[y * n + x];
            fft::Forward(line);
            for (int32_t x = 0; x < n; ++x) plane[y * n + x] = line[x];
        }
        for (int32_t x = 0; x < n; ++x) {
            for (int32_t y = 0; y < n; ++y) line[y] = plane[y * n + x];
            fft::Forward(line);
            for (int32_t y = 0; y < n; ++y) plane[y * n + x] = line[y];
        }
    };
    transform(a);
    transform(b);
    // Cross-power spectrum of current against previous: its transform peaks
    // at minus the shift, since a forward transform stands in for the
    // inverse one and mirrors the plane.
    double energy = 0.0;
    for (size_t index = 0; index < a.size(); ++index) {
        Complex cross = b[index] * std::conj(a[index]);
        const double magnitude = std::abs(cross);
        energy += magnitude;
        a[index] = magnitude > 1e-9 ? cross / magnitude : Complex(0.0, 0.0);
    }
    if (energy <= 1e-6) return false;
    transform(a);
    size_t peak = 0;
    double best = -1.0;
    for (size_t index = 0; index < a.size(); ++index) {
        const double value = a[index].real();
        if (value > best) {
            best = value;
            peak = index;
        }
    }
    confidence = best / (static_cast<double>(n) * n);
    const int32_t peakX = static_cast<int32_t>(peak % n);
    const int32_t peakY = static_cast<int32_t>(peak / n);
    const auto at = [&](int32_t x, int32_t y) {
        return a[static_cast<size_t>(((y + n) % n) * n + ((x + n) % n))].real();
    };
    const auto refine = [](double left, double centre, double right) {
        const double denominator = left - 2.0 * centre + right;
        return std::abs(denominator) < 1e-12
                   ? 0.0
                   : std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
    };
    double x = peakX + refine(at(peakX - 1, peakY), best, at(peakX + 1, peakY));
    double y = peakY + refine(at(peakX, peakY - 1), best, at(peakX, peakY + 1));
    // Forward twice mirrors: index k holds shift -k.
    shiftX = x == 0.0 ? 0.0 : (x > n / 2.0 ? n - x : -x);
    shiftY = y == 0.0 ? 0.0 : (y > n / 2.0 ? n - y : -y);
    return true;
}

}  // namespace

GlobalMotion EstimateGlobalMotion(const uint8_t* previous,
                                  const uint8_t* current, int32_t width,
                                  int32_t height) {
    GlobalMotion motion;
    if (width < 3 * kMotionBlock || height < 3 * kMotionBlock) return motion;
    constexpr double kMinimumConfidence = 0.08;
    struct Shift {
        double centre_x;
        double centre_y;
        double dx;
        double dy;
    };
    std::vector<Shift> shifts;
    for (int32_t row = 0; row < 3; ++row) {
        for (int32_t column = 0; column < 3; ++column) {
            const double centreX = width * (column + 1) / 4.0;
            const double centreY = height * (row + 1) / 4.0;
            const int32_t left =
                std::clamp(static_cast<int32_t>(centreX) - kMotionBlock / 2, 0,
                           width - kMotionBlock);
            const int32_t top =
                std::clamp(static_cast<int32_t>(centreY) - kMotionBlock / 2, 0,
                           height - kMotionBlock);
            double dx = 0.0;
            double dy = 0.0;
            double confidence = 0.0;
            if (BlockShift(previous, current, width, left, top, dx, dy,
                           confidence) &&
                confidence >= kMinimumConfidence) {
                shifts.push_back({left + kMotionBlock / 2.0 - width / 2.0,
                                  top + kMotionBlock / 2.0 - height / 2.0, dx,
                                  dy});
            }
        }
    }
    motion.blocks = static_cast<int32_t>(shifts.size());
    if (shifts.size() < 4) return motion;
    // d = t + k * r, least squares over the trusted blocks.
    double meanX = 0.0, meanY = 0.0, meanRx = 0.0, meanRy = 0.0;
    for (const Shift& shift : shifts) {
        meanX += shift.dx;
        meanY += shift.dy;
        meanRx += shift.centre_x;
        meanRy += shift.centre_y;
    }
    const double count = static_cast<double>(shifts.size());
    meanX /= count;
    meanY /= count;
    meanRx /= count;
    meanRy /= count;
    double numerator = 0.0;
    double denominator = 0.0;
    for (const Shift& shift : shifts) {
        const double rx = shift.centre_x - meanRx;
        const double ry = shift.centre_y - meanRy;
        numerator += (shift.dx - meanX) * rx + (shift.dy - meanY) * ry;
        denominator += rx * rx + ry * ry;
    }
    motion.zoom = denominator > 0.0 ? numerator / denominator : 0.0;
    motion.translation_x = meanX - motion.zoom * meanRx;
    motion.translation_y = meanY - motion.zoom * meanRy;
    return motion;
}

std::optional<int64_t> CameraSpeed(const ShotQualityReport& report,
                                   const ShotQualitySample& sample,
                                   int32_t displayWidth,
                                   int32_t displayHeight) {
    if (!report.has_global_motion || sample.motion_blocks < 4 ||
        displayWidth <= 0 || displayHeight <= 0 ||
        report.samples_per_second == 0)
        return std::nullopt;
    constexpr double kReferenceWidth = 1080.0;
    const double horizontal = static_cast<double>(sample.translation_x) /
                              kShotQualityMetricScale * kReferenceWidth;
    const double vertical = static_cast<double>(sample.translation_y) /
                            kShotQualityMetricScale * kReferenceWidth *
                            displayHeight / displayWidth;
    return static_cast<int64_t>(std::llround(std::hypot(horizontal, vertical) *
                                             report.samples_per_second));
}

bool ListStableWindows(const ShotQualityReport& report, MediaRate sourceRate,
                       int32_t displayWidth, int32_t displayHeight,
                       const StableWindowSettings& settings,
                       std::vector<StableWindow>& windows, std::string& error,
                       RushStillness* stillness) {
    windows.clear();
    error.clear();
    if (stillness != nullptr) *stillness = RushStillness{};
    if (!report.has_global_motion) {
        error =
            "the shot quality cache predates camera motion; re-run "
            "analyze_shot_quality";
        return false;
    }
    if (sourceRate.num <= 0 || sourceRate.den <= 0 ||
        report.samples_per_second == 0) {
        error = "invalid source rate or analysis grid";
        return false;
    }
    const size_t count = report.samples.size();
    // Interval k joins sample k-1 to sample k.
    std::vector<bool> stable(count, false);
    std::vector<int64_t> speeds(count, 0);
    const auto sharpEnough = [&](const ShotQualitySample& sample) {
        return static_cast<__int128>(sample.sharpness) * 100 >=
               static_cast<__int128>(report.median_sharpness) *
                   settings.minimum_sharpness_percent;
    };
    std::vector<int64_t> measuredSpeeds;
    int64_t unmeasured = 0;
    int64_t tooFast = 0;
    int64_t tooMuchZoom = 0;
    int64_t tooSoft = 0;
    for (size_t index = 1; index < count; ++index) {
        const ShotQualitySample& sample = report.samples[index];
        const std::optional<int64_t> speed =
            CameraSpeed(report, sample, displayWidth, displayHeight);
        if (!speed) {
            ++unmeasured;
            continue;
        }
        speeds[index] = *speed;
        measuredSpeeds.push_back(*speed);
        const int64_t zoomPerMille = std::llabs(sample.zoom) *
                                     report.samples_per_second * 1000 /
                                     kShotQualityMetricScale;
        const bool fast = *speed > settings.maximum_speed;
        const bool zooming = zoomPerMille > settings.maximum_zoom_per_mille;
        const bool soft =
            !sharpEnough(report.samples[index - 1]) || !sharpEnough(sample);
        tooFast += fast ? 1 : 0;
        tooMuchZoom += zooming ? 1 : 0;
        tooSoft += soft ? 1 : 0;
        stable[index] = !fast && !zooming && !soft;
    }
    RationalTime longest{0, static_cast<int32_t>(report.samples_per_second)};
    if (stillness != nullptr) {
        const auto measured = static_cast<int64_t>(measuredSpeeds.size());
        stillness->measured_intervals = measured;
        stillness->unmeasured_intervals = unmeasured;
        stillness->median_speed = ShotQualityPercentile(measuredSpeeds, 50);
        if (measured > 0) {
            stillness->too_fast_percent = tooFast * 100 / measured;
            stillness->too_much_zoom_percent = tooMuchZoom * 100 / measured;
            stillness->too_soft_percent = tooSoft * 100 / measured;
        }
    }
    for (size_t begin = 1; begin < count;) {
        if (!stable[begin]) {
            ++begin;
            continue;
        }
        size_t end = begin;
        while (end + 1 < count && stable[end + 1]) ++end;
        const RationalTime start = report.samples[begin - 1].time;
        const RationalTime stop = report.samples[end].time;
        const RationalTime minimum{settings.minimum_duration_milliseconds,
                                   1000};
        if (longest < stop.sub(start)) longest = stop.sub(start);
        if (!(stop.sub(start) < minimum)) {
            StableWindow window;
            window.start = start;
            window.end = stop;
            // Frames wholly inside the window: the first one starting at or
            // after its start, up to the one containing its end.
            const __int128 startNumerator =
                static_cast<__int128>(start.value) * sourceRate.num;
            const __int128 startDenominator =
                static_cast<__int128>(start.rate) * sourceRate.den;
            window.first_frame = static_cast<int64_t>(
                (startNumerator + startDenominator - 1) / startDenominator);
            window.end_frame = stop.to_frames(sourceRate.num, sourceRate.den);
            std::vector<int64_t> spans(speeds.begin() + begin,
                                       speeds.begin() + end + 1);
            window.peak_speed = *std::max_element(spans.begin(), spans.end());
            window.median_speed = ShotQualityPercentile(spans, 50);
            int64_t lowest = std::numeric_limits<int64_t>::max();
            for (size_t index = begin - 1; index <= end; ++index) {
                lowest =
                    std::min(lowest, report.median_sharpness > 0
                                         ? report.samples[index].sharpness *
                                               100 / report.median_sharpness
                                         : 0);
            }
            window.lowest_sharpness_percent = lowest;
            windows.push_back(window);
        }
        begin = end + 1;
    }
    if (stillness != nullptr) stillness->longest_still = longest;
    return true;
}

int64_t FrameHistogramDistanceMetric(const uint8_t* previous,
                                     const uint8_t* current, int32_t width,
                                     int32_t height) {
    if (previous == nullptr || current == nullptr || width < 1 || height < 1)
        return 0;
    const int64_t count =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    // 256 / kShotQualityHistogramBins is exact, so every byte maps into
    // range and the compiler turns the division into a shift.
    constexpr int32_t kBinWidth = 256 / kShotQualityHistogramBins;
    int64_t previousBins[kShotQualityHistogramBins] = {};
    int64_t currentBins[kShotQualityHistogramBins] = {};
    for (int64_t index = 0; index < count; ++index) {
        ++previousBins[previous[index] / kBinWidth];
        ++currentBins[current[index] / kBinWidth];
    }
    __int128 total = 0;
    for (int32_t bin = 0; bin < kShotQualityHistogramBins; ++bin) {
        const int64_t difference = currentBins[bin] - previousBins[bin];
        total += difference < 0 ? -difference : difference;
    }
    // Both histograms hold exactly `count` entries, so what one bin gains
    // another loses and the absolute differences sum to twice the number of
    // pixels that moved. Dividing by 2*count therefore reads directly as
    // "the fraction of pixels that changed luma bin", and reaches full scale
    // only when the two planes share no bin at all.
    return ScaleToMetric(total, static_cast<__int128>(count) * 2);
}

int64_t ShotQualityPercentile(std::vector<int64_t> values, int percent) {
    if (values.empty()) return 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    std::sort(values.begin(), values.end());
    // Nearest-rank on the lower side: index = floor(p * (n-1) / 100). With
    // integer arithmetic this is exact and stable, and for p=50 on an even
    // count it deliberately picks the lower of the two middle values rather
    // than averaging them -- an averaged median would be the one place in
    // this file where a value not actually measured gets reported.
    const int64_t index = static_cast<int64_t>(percent) *
                          static_cast<int64_t>(values.size() - 1) / 100;
    return values[static_cast<size_t>(index)];
}

bool SummarizeClipShotQuality(const DocumentClip& clip,
                              const ShotQualityReport& report,
                              const ShotQualityThresholds& thresholds,
                              ClipShotQuality& summary, std::string& error) {
    error.clear();
    if (report.media_id != clip.source_id) {
        error = "shot quality report does not match the clip source";
        return false;
    }
    if (report.samples_per_second == 0) {
        error = "shot quality report has no analysis rate";
        return false;
    }
    const RationalTime clipEnd = clip.source_in.add(clip.duration);
    std::vector<int64_t> sharpness;
    std::vector<int64_t> motion;
    for (const ShotQualitySample& sample : report.samples) {
        if (sample.time.compare(clip.source_in) < 0) continue;
        if (sample.time.compare(clipEnd) >= 0) break;
        sharpness.push_back(sample.sharpness);
        motion.push_back(sample.motion);
    }
    if (sharpness.empty()) {
        error = "clip is shorter than one analysis sample interval";
        return false;
    }

    summary = ClipShotQuality{};
    summary.clip_id = clip.id;
    summary.source_id = clip.source_id;
    summary.samples = static_cast<int32_t>(sharpness.size());
    summary.sharpness_median = ShotQualityPercentile(sharpness, 50);
    summary.sharpness_worst = ShotQualityPercentile(sharpness, 10);
    summary.motion_median = ShotQualityPercentile(motion, 50);
    summary.motion_peak = ShotQualityPercentile(motion, 90);
    summary.source_median_sharpness = report.median_sharpness;

    // Graded on the worst decile, not the median: a shot that racks focus
    // halfway through reads healthy at p50 and is still unusable.
    if (report.median_sharpness > 0) {
        const __int128 ratio = static_cast<__int128>(summary.sharpness_worst) *
                               100 / report.median_sharpness;
        if (ratio < thresholds.blurry_ratio_percent)
            summary.sharpness = SharpnessGrade::Blurry;
        else if (ratio < thresholds.soft_ratio_percent)
            summary.sharpness = SharpnessGrade::Soft;
    }
    summary.source_median_motion = report.median_motion;
    // Same shape as the sharpness grade, in the other direction. The floor
    // keeps a locked-off tripod from grading its own sensor noise.
    const int64_t movingAt =
        std::max(thresholds.motion_floor,
                 report.median_motion * thresholds.moving_ratio_percent / 100);
    const int64_t shakyAt =
        std::max(thresholds.motion_floor,
                 report.median_motion * thresholds.shaky_ratio_percent / 100);
    if (summary.motion_peak >= shakyAt)
        summary.steadiness = SteadinessGrade::Shaky;
    else if (summary.motion_peak >= movingAt)
        summary.steadiness = SteadinessGrade::Moving;

    summary.clean = summary.sharpness == SharpnessGrade::Sharp &&
                    summary.steadiness == SteadinessGrade::Steady;
    return true;
}

std::vector<SourceShot> DetectSourceShots(
    const ShotQualityReport& report, const ShotSegmentationSettings& settings) {
    std::vector<SourceShot> shots;
    if (report.samples.empty() || report.samples_per_second == 0) return shots;
    const int32_t rate = static_cast<int32_t>(report.samples_per_second);

    // The two absolute conditions. They do not separate a cut from a camera
    // move -- see ShotSegmentationSettings, where the measurements are --
    // they only stop the local ratio below from firing on a neighbourhood
    // that barely moved at all.
    const int64_t histogramAt =
        std::max(settings.cut_histogram_floor,
                 report.median_histogram_distance *
                     settings.cut_histogram_ratio_percent / 100);
    const int64_t window =
        std::max<int64_t>(1, settings.cut_local_window_samples);

    // Sample 0 always opens a shot: the source starts somewhere, and its
    // two change metrics are zero by construction rather than by
    // measurement, so it can never be a detected boundary itself. For the
    // same reason it is left out of every neighbourhood below.
    std::vector<size_t> starts = {0};
    std::vector<int64_t> neighbourhood;
    for (size_t index = 1; index < report.samples.size(); ++index) {
        const ShotQualitySample& sample = report.samples[index];
        if (sample.motion < settings.cut_motion_floor) continue;
        if (sample.histogram_distance < histogramAt) continue;

        // The condition that does the actual work: a cut is instantaneous,
        // so it stands alone; a camera move has inertia, so its neighbours
        // moved nearly as much as it did.
        const size_t reach = static_cast<size_t>(window);
        const size_t first = index > reach ? index - reach : 1;
        const size_t limit = std::min(report.samples.size(), index + reach + 1);
        neighbourhood.clear();
        for (size_t other = first; other < limit; ++other)
            if (other != index)
                neighbourhood.push_back(report.samples[other].motion);
        // A median of zero means nothing around this sample moved at all,
        // and no multiple of zero is reachable. That is not a reason to
        // reject: a spike in a perfectly still neighbourhood is the clearest
        // boundary there is, so the ratio passes rather than dividing.
        const int64_t baseline = ShotQualityPercentile(neighbourhood, 50);
        if (baseline > 0 && static_cast<__int128>(sample.motion) * 100 <
                                static_cast<__int128>(baseline) *
                                    settings.cut_motion_local_percent)
            continue;
        // Too close to the previous boundary: dropped, not merged into it.
        // Keeping the earlier one is what makes the pass single-sweep and
        // its output independent of how many candidates cluster inside the
        // window -- a flash frame produces one dropped candidate whether it
        // lasted one sample or three.
        if (static_cast<int64_t>(index - starts.back()) <
            settings.minimum_samples)
            continue;
        starts.push_back(index);
    }
    // The tail gets the same minimum as everything else. Without this a cut
    // near the very end of a source leaves a stub shot that no rule
    // upstream would have allowed anywhere else in it.
    if (starts.size() > 1 &&
        static_cast<int64_t>(report.samples.size() - starts.back()) <
            settings.minimum_samples)
        starts.pop_back();

    shots.reserve(starts.size());
    for (size_t shotIndex = 0; shotIndex < starts.size(); ++shotIndex) {
        const size_t first = starts[shotIndex];
        const size_t limit = shotIndex + 1 < starts.size()
                                 ? starts[shotIndex + 1]
                                 : report.samples.size();
        SourceShot shot;
        shot.first_sample = static_cast<int32_t>(first);
        shot.sample_count = static_cast<int32_t>(limit - first);
        shot.start = RationalTime{static_cast<int64_t>(first), rate};
        shot.end = RationalTime{static_cast<int64_t>(limit), rate};

        std::vector<int64_t> sharpness;
        std::vector<int64_t> motion;
        sharpness.reserve(limit - first);
        size_t keyframe = first;
        for (size_t index = first; index < limit; ++index) {
            sharpness.push_back(report.samples[index].sharpness);
            // A shot's first sample measures its change against the *last
            // sample of the previous shot*, which for a detected boundary is
            // the cut itself. Counting it would put the cut's own spike into
            // the shot's motion median and report every shot as moving.
            if (index > first) motion.push_back(report.samples[index].motion);
            // Strictly greater, so a tie keeps the earliest sample and two
            // runs over the same media pick the same frame.
            if (report.samples[index].sharpness >
                report.samples[keyframe].sharpness)
                keyframe = index;
        }
        shot.keyframe_sample = static_cast<int32_t>(keyframe);
        shot.keyframe = RationalTime{static_cast<int64_t>(keyframe), rate};
        shot.median_sharpness = ShotQualityPercentile(std::move(sharpness), 50);
        shot.median_motion = ShotQualityPercentile(std::move(motion), 50);
        shots.push_back(shot);
    }
    return shots;
}

// Cache layout, written strictly and read tolerantly (the same split
// Transcription.cc uses). Sample k's time is {k, samples_per_second} by
// construction, so the index is the timestamp and only the three metrics
// are stored -- parallel integer arrays rather than an array of objects,
// which keeps a one-hour rush's cache in the low hundreds of kilobytes and
// still reads plainly in a text editor. There is no float literal anywhere
// in the file.
std::string SerializeShotQuality(const ShotQualityReport& report) {
    std::ostringstream output;
    output << "{\"version\":" << (report.has_global_motion ? 4 : 3)
           << ",\"media_id\":\"" << mcp_json::EscapeJsonString(report.media_id)
           << "\",\"samples_per_second\":" << report.samples_per_second
           << ",\"analysis_width\":" << report.analysis_width
           << ",\"analysis_height\":" << report.analysis_height
           << ",\"median_sharpness\":" << report.median_sharpness
           << ",\"median_motion\":" << report.median_motion
           << ",\"median_histogram_distance\":"
           << report.median_histogram_distance << ",\"sharpness\":[";
    for (size_t index = 0; index < report.samples.size(); ++index) {
        if (index) output << ',';
        output << report.samples[index].sharpness;
    }
    output << "],\"motion\":[";
    for (size_t index = 0; index < report.samples.size(); ++index) {
        if (index) output << ',';
        output << report.samples[index].motion;
    }
    output << "],\"histogram\":[";
    for (size_t index = 0; index < report.samples.size(); ++index) {
        if (index) output << ',';
        output << report.samples[index].histogram_distance;
    }
    output << ']';
    if (report.has_global_motion) {
        const auto series = [&](const char* key, auto member) {
            output << ",\"" << key << "\":[";
            for (size_t index = 0; index < report.samples.size(); ++index) {
                if (index) output << ',';
                output << report.samples[index].*member;
            }
            output << ']';
        };
        series("translation_x", &ShotQualitySample::translation_x);
        series("translation_y", &ShotQualitySample::translation_y);
        series("zoom", &ShotQualitySample::zoom);
        series("motion_blocks", &ShotQualitySample::motion_blocks);
    }
    output << '}';
    return output.str();
}

namespace {

// DeserializeShotQuality, also handing back the version the cache declares
// (0 when it declares none) so a refusal can say which one it met.
bool DeserializeVersionedShotQuality(const std::string& json,
                                     ShotQualityReport& report,
                                     int64_t& version, std::string& error) {
    error.clear();
    version = 0;
    Value root;
    std::string parseError;
    if (!Value::Parse(json, root, parseError) || !root.IsObject()) {
        error = "malformed shot quality cache: " + parseError;
        return false;
    }
    // Older caches are refused rather than migrated, and the reason has not
    // changed between the two bumps: a cache is cheap to rebuild (ten
    // seconds for a seven-minute rush), while inventing a metric that was
    // never measured would be the silent fallback this project refuses
    // everywhere else. Version 1 stored no median_motion, because motion was
    // graded against an absolute threshold measurement later showed to be
    // wrong. Version 2 stored no histogram distance, so DetectSourceShots
    // could not tell one of its two conditions from the other and would have
    // had to call every whip pan a cut.
    // ITW6 -- version 4 adds global motion. A version 3 cache still grades
    // sharpness, steadiness and shots; it only lacks what version 4 added,
    // and says so through has_global_motion rather than inventing zeros.
    if (!ReadInt64Field(root, "version", version) ||
        (version != 3 && version != 4)) {
        error = (version == 1 || version == 2)
                    ? "shot quality cache predates shot segmentation; "
                      "re-run the analysis"
                    : "unsupported shot quality cache version";
        return false;
    }
    ShotQualityReport parsed;
    const Value* mediaId = root.Find("media_id");
    if (mediaId == nullptr || !mediaId->IsString()) {
        error = "shot quality cache has no media_id";
        return false;
    }
    parsed.media_id = mediaId->AsString();
    int64_t rate = 0;
    int64_t width = 0;
    int64_t height = 0;
    if (!ReadInt64Field(root, "samples_per_second", rate) || rate <= 0 ||
        rate > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "analysis_width", width) || width <= 0 ||
        width > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "analysis_height", height) || height <= 0 ||
        height > std::numeric_limits<int32_t>::max()) {
        error = "shot quality cache has invalid analysis settings";
        return false;
    }
    parsed.samples_per_second = static_cast<uint32_t>(rate);
    parsed.analysis_width = static_cast<int32_t>(width);
    parsed.analysis_height = static_cast<int32_t>(height);
    if (!ReadInt64Field(root, "median_sharpness", parsed.median_sharpness) ||
        parsed.median_sharpness < 0 ||
        !ReadInt64Field(root, "median_motion", parsed.median_motion) ||
        parsed.median_motion < 0 ||
        !ReadInt64Field(root, "median_histogram_distance",
                        parsed.median_histogram_distance) ||
        parsed.median_histogram_distance < 0) {
        error =
            "shot quality cache has no median sharpness, motion or "
            "histogram distance";
        return false;
    }
    const Value* sharpness = root.Find("sharpness");
    const Value* motion = root.Find("motion");
    const Value* histogram = root.Find("histogram");
    if (sharpness == nullptr || !sharpness->IsArray() || motion == nullptr ||
        !motion->IsArray() || histogram == nullptr || !histogram->IsArray() ||
        sharpness->AsArray().size() != motion->AsArray().size() ||
        sharpness->AsArray().size() != histogram->AsArray().size()) {
        error = "shot quality cache metric arrays are missing or mismatched";
        return false;
    }
    parsed.has_global_motion = version == 4;
    const Value* translationX = root.Find("translation_x");
    const Value* translationY = root.Find("translation_y");
    const Value* zoom = root.Find("zoom");
    const Value* blocks = root.Find("motion_blocks");
    const auto sameLength = [&](const Value* series) {
        return series != nullptr && series->IsArray() &&
               series->AsArray().size() == sharpness->AsArray().size();
    };
    if (parsed.has_global_motion &&
        (!sameLength(translationX) || !sameLength(translationY) ||
         !sameLength(zoom) || !sameLength(blocks))) {
        error = "shot quality cache global motion arrays are mismatched";
        return false;
    }
    parsed.samples.reserve(sharpness->AsArray().size());
    for (size_t index = 0; index < sharpness->AsArray().size(); ++index) {
        ShotQualitySample sample;
        if (!sharpness->AsArray()[index].AsInt64(sample.sharpness) ||
            !motion->AsArray()[index].AsInt64(sample.motion) ||
            !histogram->AsArray()[index].AsInt64(sample.histogram_distance) ||
            sample.sharpness < 0 || sample.motion < 0 ||
            sample.histogram_distance < 0) {
            error = "shot quality cache contains an invalid metric";
            return false;
        }
        if (parsed.has_global_motion) {
            int64_t blockCount = 0;
            if (!translationX->AsArray()[index].AsInt64(sample.translation_x) ||
                !translationY->AsArray()[index].AsInt64(sample.translation_y) ||
                !zoom->AsArray()[index].AsInt64(sample.zoom) ||
                !blocks->AsArray()[index].AsInt64(blockCount) ||
                blockCount < 0 || blockCount > 9) {
                error = "shot quality cache contains an invalid global motion";
                return false;
            }
            sample.motion_blocks = static_cast<int32_t>(blockCount);
        }
        sample.time =
            RationalTime{static_cast<int64_t>(index),
                         static_cast<int32_t>(parsed.samples_per_second)};
        parsed.samples.push_back(sample);
    }
    report = std::move(parsed);
    return true;
}

}  // namespace

bool DeserializeShotQuality(const std::string& json, ShotQualityReport& report,
                            std::string& error) {
    int64_t version = 0;
    return DeserializeVersionedShotQuality(json, report, version, error);
}

ShotQualityCacheRead ReadShotQualityCache(const std::string& path,
                                          ShotQualityReport& report) {
    ShotQualityCacheRead read;
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        read.state = ShotQualityCacheState::Absent;
        read.detail = "unable to open shot quality cache";
        return read;
    }
    // From here on a file is there: whatever stops it from loading makes it
    // unreadable, never absent.
    read.state = ShotQualityCacheState::Unreadable;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        read.detail = "unable to open shot quality cache";
        return read;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        read.detail = "unable to read shot quality cache";
        return read;
    }
    if (DeserializeVersionedShotQuality(buffer.str(), report,
                                        read.found_version, read.detail)) {
        read.state = ShotQualityCacheState::Loaded;
    }
    return read;
}

bool LoadShotQuality(const std::string& path, ShotQualityReport& report,
                     std::string& error) {
    const ShotQualityCacheRead read = ReadShotQualityCache(path, report);
    error = read.detail;
    return read.state == ShotQualityCacheState::Loaded;
}

bool GenerateShotQuality(const std::string& inputPath,
                         const std::string& outputPath,
                         const std::string& mediaId,
                         const RationalTime& duration,
                         const ShotQualitySettings& settings,
                         MediaTaskContext& context, std::string& error) {
    error.clear();
    if (duration.rate <= 0 || duration.value <= 0 ||
        settings.samples_per_second == 0 ||
        settings.samples_per_second > std::numeric_limits<int32_t>::max() ||
        settings.analysis_width < 3 || settings.analysis_height < 3) {
        error = "invalid shot quality settings or source duration";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create shot quality directory: " +
                filesystemError.message();
        return false;
    }

    // `fps` resamples onto the analysis grid, so sample k is the frame the
    // filter emitted for time k/samples_per_second regardless of the source's
    // own frame rate -- which is what lets ShotQualitySample::time be
    // reconstructed from the array index alone. `format=gray` gives a
    // tightly packed 8-bit plane, so the analysis functions need no stride.
    const std::string filter =
        "fps=" + std::to_string(settings.samples_per_second) +
        ",scale=" + std::to_string(settings.analysis_width) + ":" +
        std::to_string(settings.analysis_height) +
        ":flags=bilinear,format=gray";
    std::vector<std::string> storage = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-i",
        inputPath,
        "-map",
        "0:v:0",
        "-an",
        "-sn",
        "-vf",
        filter,
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "pipe:1",
    };
    const size_t frameBytes = static_cast<size_t>(settings.analysis_width) *
                              static_cast<size_t>(settings.analysis_height);
    ShotQualityReport report;
    report.media_id = mediaId;
    report.has_global_motion = true;
    report.samples_per_second = settings.samples_per_second;
    report.analysis_width = settings.analysis_width;
    report.analysis_height = settings.analysis_height;
    std::vector<uint8_t> previousFrame;
    std::vector<char> pendingBytes;
    const long double expectedFrames =
        static_cast<long double>(duration.value) / duration.rate *
        settings.samples_per_second;
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 65536;
    request.error_tail_bytes = 8192;
    request.cancelled = [&context]() { return context.Cancelled(); };
    request.on_output = [&](const char* bytes, size_t count) {
        pendingBytes.insert(pendingBytes.end(), bytes, bytes + count);
        size_t offset = 0;
        while (pendingBytes.size() - offset >= frameBytes) {
            const uint8_t* frame =
                reinterpret_cast<const uint8_t*>(pendingBytes.data() + offset);
            ShotQualitySample sample;
            sample.time =
                RationalTime{static_cast<int64_t>(report.samples.size()),
                             static_cast<int32_t>(settings.samples_per_second)};
            sample.sharpness = FrameSharpnessMetric(
                frame, settings.analysis_width, settings.analysis_height);
            sample.motion = previousFrame.empty()
                                ? 0
                                : FrameMotionMetric(previousFrame.data(), frame,
                                                    settings.analysis_width,
                                                    settings.analysis_height);
            sample.histogram_distance =
                previousFrame.empty()
                    ? 0
                    : FrameHistogramDistanceMetric(previousFrame.data(), frame,
                                                   settings.analysis_width,
                                                   settings.analysis_height);
            if (!previousFrame.empty()) {
                const GlobalMotion global = EstimateGlobalMotion(
                    previousFrame.data(), frame, settings.analysis_width,
                    settings.analysis_height);
                sample.motion_blocks = global.blocks;
                sample.translation_x = static_cast<int64_t>(std::llround(
                    global.translation_x / settings.analysis_width *
                    kShotQualityMetricScale));
                sample.translation_y = static_cast<int64_t>(std::llround(
                    global.translation_y / settings.analysis_height *
                    kShotQualityMetricScale));
                sample.zoom = static_cast<int64_t>(
                    std::llround(global.zoom * kShotQualityMetricScale));
            }
            report.samples.push_back(sample);
            previousFrame.assign(frame, frame + frameBytes);
            offset += frameBytes;
        }
        pendingBytes.erase(pendingBytes.begin(), pendingBytes.begin() + offset);
        context.SetProgress(
            expectedFrames <= 0.0L
                ? 0.0
                : static_cast<double>(report.samples.size() / expectedFrames),
            "Analyse image");
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "shot quality process pipes");
        return false;
    }
    if (result.cancelled || context.Cancelled()) {
        error = "shot quality analysis cancelled";
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty()
                    ? "FFmpeg shot quality analysis failed"
                    : result.error_tail;
        return false;
    }
    if (report.samples.empty()) {
        error = "video stream produced no analysis samples";
        return false;
    }
    std::vector<int64_t> allSharpness;
    std::vector<int64_t> allMotion;
    std::vector<int64_t> allHistogram;
    allSharpness.reserve(report.samples.size());
    allMotion.reserve(report.samples.size());
    allHistogram.reserve(report.samples.size());
    for (size_t index = 0; index < report.samples.size(); ++index) {
        allSharpness.push_back(report.samples[index].sharpness);
        // The first sample has no predecessor, so its zero is an absence of
        // measurement rather than a still frame; counting it would drag the
        // median down on a short source. Both change metrics share that.
        if (index > 0) {
            allMotion.push_back(report.samples[index].motion);
            allHistogram.push_back(report.samples[index].histogram_distance);
        }
    }
    report.median_sharpness =
        ShotQualityPercentile(std::move(allSharpness), 50);
    report.median_motion = ShotQualityPercentile(std::move(allMotion), 50);
    report.median_histogram_distance =
        ShotQualityPercentile(std::move(allHistogram), 50);

    // Written through a neighbouring temporary and renamed, so a cancelled
    // or crashed run never leaves a half-parsed cache behind for the next
    // reader to trust.
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string json = SerializeShotQuality(report);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, filesystemError);
            error = "unable to write shot quality cache";
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "unable to publish shot quality cache: " +
                filesystemError.message();
        return false;
    }
    return true;
}

namespace {

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

// Timeline span left visible once every clip on a higher-index video track
// is subtracted. Export.cc composites video tracks in ascending index order,
// each over the accumulated result, so a higher index is literally on top --
// this mirrors that rule rather than guessing at it. All arithmetic stays in
// RationalTime, so a clip covered to the frame reports exactly zero.
//
// F1.5 -- ROADMAP.md. An inactive multicam angle is on screen nowhere and
// covers nothing (Multicam.h), the rule Export.cc applies. Nor is, nor does,
// a disabled clip (ClipEnabled.h).
RationalTime VisibleDuration(const Document& document,
                             const DocumentTrack& track,
                             const DocumentClip& clip) {
    const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
    if (!ClipIsSeen(document.sequence, clip)) {
        return RationalTime{0, clip.duration.rate > 0 ? clip.duration.rate : 1};
    }
    std::vector<std::pair<RationalTime, RationalTime>> gaps;
    gaps.emplace_back(clip.timeline_in, clipEnd);
    for (const DocumentTrack& above : document.sequence.tracks) {
        if (above.kind != "video" || above.index <= track.index) continue;
        if (!above.visible) continue;
        for (const DocumentClip& cover : above.clips) {
            // MOTION-2026-09 M4 -- a motion clip is drawn with its alpha over
            // the shot: a title does not take it off screen.
            if (!ClipIsSeen(document.sequence, cover) || IsMotionClip(cover)) {
                continue;
            }
            const RationalTime coverStart = cover.timeline_in;
            const RationalTime coverEnd = coverStart.add(cover.duration);
            std::vector<std::pair<RationalTime, RationalTime>> next;
            for (const auto& span : gaps) {
                if (coverEnd.compare(span.first) <= 0 ||
                    coverStart.compare(span.second) >= 0) {
                    next.push_back(span);
                    continue;
                }
                if (span.first.compare(coverStart) < 0)
                    next.emplace_back(span.first, coverStart);
                if (span.second.compare(coverEnd) > 0)
                    next.emplace_back(coverEnd, span.second);
            }
            gaps = std::move(next);
        }
    }
    RationalTime visible{0, clip.duration.rate > 0 ? clip.duration.rate : 1};
    for (const auto& span : gaps)
        visible = visible.add(span.second.sub(span.first));
    return visible;
}

}  // namespace

namespace {

// BENCH-2026-09 (B3) -- each unreadable cache, machine-readable: shared by
// the refusal of list_stable_windows and the timeline views that list them.
Value UnreadableCachesValue(
    const std::vector<UnreadableShotQualityCache>& caches) {
    Value list = Value::MakeArray();
    for (const UnreadableShotQualityCache& cache : caches) {
        Value item = Value::MakeObject();
        item.Set("source_id", Value::MakeString(cache.source_id));
        item.Set("cache", Value::MakeString("shot_quality"));
        item.Set("found_version", cache.read.found_version > 0
                                      ? Value::MakeInt(cache.read.found_version)
                                      : Value::MakeNull());
        Value readable = Value::MakeArray();
        for (const int64_t version : kReadableShotQualityVersions) {
            readable.Push(Value::MakeInt(version));
        }
        item.Set("readable_versions", std::move(readable));
        item.Set("reason", Value::MakeString(cache.read.detail));
        Value arguments = Value::MakeObject();
        arguments.Set("media_id", Value::MakeString(cache.source_id));
        Value remedy = Value::MakeObject();
        remedy.Set("tool", Value::MakeString("analyze_shot_quality"));
        remedy.Set("arguments", std::move(arguments));
        remedy.Set("cli", Value::MakeString("--shot-quality"));
        item.Set("remedy", std::move(remedy));
        list.Push(std::move(item));
    }
    return list;
}

}  // namespace

std::string DescribeShotQualityForAgent(
    const Document& document, const std::map<Ulid, ShotQualityReport>& reports,
    const ShotQualityThresholds& thresholds,
    const ShotSegmentationSettings& segmentation,
    const std::vector<UnreadableShotQualityCache>& unreadable) {
    Value graded = Value::MakeArray();
    Value ungraded = Value::MakeArray();
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            // MOTION-2026-09 M4 -- a composition is drawn, not filmed: it
            // has no sharpness or camera motion to measure.
            if (IsMotionClip(clip)) {
                Value entry = Value::MakeObject();
                entry.Set("clip_id", Value::MakeString(clip.id));
                entry.Set("composition_id",
                          Value::MakeString(clip.composition_id));
                entry.Set("track_id", Value::MakeString(track.id));
                entry.Set("reason",
                          Value::MakeString("motion clip: a composition is "
                                            "drawn, not filmed, and has no "
                                            "shot quality"));
                ungraded.Push(std::move(entry));
                continue;
            }
            const auto found = reports.find(clip.source_id);
            ClipShotQuality summary;
            std::string error;
            if (found == reports.end() ||
                !SummarizeClipShotQuality(clip, found->second, thresholds,
                                          summary, error)) {
                Value entry = Value::MakeObject();
                entry.Set("clip_id", Value::MakeString(clip.id));
                entry.Set("source_id", Value::MakeString(clip.source_id));
                entry.Set("track_id", Value::MakeString(track.id));
                const bool cacheUnreadable =
                    std::any_of(unreadable.begin(), unreadable.end(),
                                [&](const UnreadableShotQualityCache& cache) {
                                    return cache.source_id == clip.source_id;
                                });
                entry.Set("reason",
                          Value::MakeString(
                              found != reports.end() ? error
                              : cacheUnreadable
                                  ? "the cached shot quality analysis of this "
                                    "source cannot be read by this build: see "
                                    "unreadable_caches"
                                  : "no cached shot quality analysis for this "
                                    "source"));
                ungraded.Push(std::move(entry));
                continue;
            }
            Value entry = Value::MakeObject();
            entry.Set("clip_id", Value::MakeString(summary.clip_id));
            entry.Set("source_id", Value::MakeString(summary.source_id));
            entry.Set("track_id", Value::MakeString(track.id));
            entry.Set("timeline_in", TimeValue(clip.timeline_in));
            entry.Set("duration", TimeValue(clip.duration));
            entry.Set("samples", Value::MakeInt(summary.samples));
            entry.Set("sharpness",
                      Value::MakeString(SharpnessGradeName(summary.sharpness)));
            entry.Set(
                "steadiness",
                Value::MakeString(SteadinessGradeName(summary.steadiness)));
            const RationalTime visible = VisibleDuration(document, track, clip);
            const bool fullyCovered = visible.compare(RationalTime{0, 1}) == 0;
            entry.Set("clean", Value::MakeBool(summary.clean));
            entry.Set("visible", TimeValue(visible));
            entry.Set("fully_covered", Value::MakeBool(fullyCovered));
            // What a caller should actually act on: measured badly *and*
            // still on screen. Kept as its own field so "clean" keeps
            // meaning "measured well" and nothing else.
            entry.Set("needs_attention",
                      Value::MakeBool(!summary.clean && !fullyCovered));
            entry.Set("sharpness_median",
                      Value::MakeInt(summary.sharpness_median));
            entry.Set("sharpness_worst",
                      Value::MakeInt(summary.sharpness_worst));
            entry.Set("source_median_sharpness",
                      Value::MakeInt(summary.source_median_sharpness));
            entry.Set("motion_median", Value::MakeInt(summary.motion_median));
            entry.Set("motion_peak", Value::MakeInt(summary.motion_peak));
            entry.Set("source_median_motion",
                      Value::MakeInt(summary.source_median_motion));
            // ITW6 -- the camera itself, in absolute terms, beside the
            // relative change above: px/s at 1080 width and zoom per second.
            const ShotQualityReport& report = found->second;
            const LibraryMedia* media =
                document.FindLibraryMedia(clip.source_id);
            int32_t displayWidth = 0;
            int32_t displayHeight = 0;
            std::vector<int64_t> speeds;
            int64_t zoomPeak = 0;
            if (media != nullptr &&
                DisplayDimensions(*media, displayWidth, displayHeight)) {
                const RationalTime clipEnd = clip.source_in.add(clip.duration);
                for (const ShotQualitySample& sample : report.samples) {
                    if (sample.time < clip.source_in ||
                        !(sample.time < clipEnd))
                        continue;
                    if (const auto speed = CameraSpeed(
                            report, sample, displayWidth, displayHeight)) {
                        speeds.push_back(*speed);
                        zoomPeak = std::max<int64_t>(
                            zoomPeak, std::llabs(sample.zoom) *
                                          report.samples_per_second * 1000 /
                                          kShotQualityMetricScale);
                    }
                }
            }
            Value camera = Value::MakeObject();
            camera.Set("measured", Value::MakeBool(!speeds.empty()));
            if (!speeds.empty()) {
                camera.Set("speed_median",
                           Value::MakeInt(ShotQualityPercentile(speeds, 50)));
                camera.Set("speed_peak",
                           Value::MakeInt(ShotQualityPercentile(speeds, 90)));
                camera.Set("zoom_peak_per_mille_per_second",
                           Value::MakeInt(zoomPeak));
            }
            entry.Set("camera", std::move(camera));
            graded.Push(std::move(entry));
        }
    }

    // What the rushes hold, as opposed to what the timeline already uses.
    // Keyed by source rather than by clip because a shot is a property of
    // the media: two clips cut from the same take share these, and a caller
    // looking for an unused shot has no clip to hang the question on.
    Value sources = Value::MakeArray();
    for (const auto& entry : reports) {
        const ShotQualityReport& report = entry.second;
        Value item = Value::MakeObject();
        item.Set("source_id", Value::MakeString(entry.first));
        item.Set("samples_per_second",
                 Value::MakeInt(report.samples_per_second));
        item.Set("median_sharpness", Value::MakeInt(report.median_sharpness));
        item.Set("median_motion", Value::MakeInt(report.median_motion));
        item.Set("median_histogram_distance",
                 Value::MakeInt(report.median_histogram_distance));
        Value shotList = Value::MakeArray();
        for (const SourceShot& shot : DetectSourceShots(report, segmentation)) {
            Value described = Value::MakeObject();
            // Source-domain times, so these can be used as a clip's
            // source_in and duration without conversion.
            described.Set("start", TimeValue(shot.start));
            described.Set("end", TimeValue(shot.end));
            described.Set("duration", TimeValue(shot.end.sub(shot.start)));
            // The frame to pull when one still has to stand for the shot.
            described.Set("keyframe", TimeValue(shot.keyframe));
            described.Set("samples", Value::MakeInt(shot.sample_count));
            described.Set("median_sharpness",
                          Value::MakeInt(shot.median_sharpness));
            described.Set("median_motion", Value::MakeInt(shot.median_motion));
            shotList.Push(std::move(described));
        }
        item.Set("shots", std::move(shotList));
        sources.Push(std::move(item));
    }

    Value limits = Value::MakeObject();
    limits.Set("blurry_ratio_percent",
               Value::MakeInt(thresholds.blurry_ratio_percent));
    limits.Set("soft_ratio_percent",
               Value::MakeInt(thresholds.soft_ratio_percent));
    limits.Set("moving_ratio_percent",
               Value::MakeInt(thresholds.moving_ratio_percent));
    limits.Set("shaky_ratio_percent",
               Value::MakeInt(thresholds.shaky_ratio_percent));
    limits.Set("motion_floor", Value::MakeInt(thresholds.motion_floor));

    Value cuts = Value::MakeObject();
    cuts.Set("cut_motion_floor", Value::MakeInt(segmentation.cut_motion_floor));
    cuts.Set("cut_histogram_floor",
             Value::MakeInt(segmentation.cut_histogram_floor));
    cuts.Set("cut_histogram_ratio_percent",
             Value::MakeInt(segmentation.cut_histogram_ratio_percent));
    cuts.Set("cut_motion_local_percent",
             Value::MakeInt(segmentation.cut_motion_local_percent));
    cuts.Set("cut_local_window_samples",
             Value::MakeInt(segmentation.cut_local_window_samples));
    cuts.Set("minimum_samples", Value::MakeInt(segmentation.minimum_samples));

    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(document.sequence.id));
    root.Set("timeline_name", Value::MakeString(document.sequence.name));
    root.Set("metric_scale", Value::MakeInt(kShotQualityMetricScale));
    root.Set("thresholds", std::move(limits));
    root.Set("segmentation", std::move(cuts));
    root.Set("clips", std::move(graded));
    root.Set("unanalyzed", std::move(ungraded));
    root.Set("unreadable_caches", UnreadableCachesValue(unreadable));
    root.Set("sources", std::move(sources));
    return root.Dump();
}

std::string DescribeUnreadableShotQualityDetail(
    const std::vector<UnreadableShotQualityCache>& caches) {
    std::string readable;
    for (const int64_t version : kReadableShotQualityVersions) {
        readable += (readable.empty() ? "" : ", ") + std::to_string(version);
    }
    std::string detail = caches.size() == 1
                             ? "a shot quality cache cannot be read by this "
                               "build: "
                             : std::to_string(caches.size()) +
                                   " shot quality caches cannot be read by "
                                   "this build: ";
    for (size_t index = 0; index < caches.size(); ++index) {
        const UnreadableShotQualityCache& cache = caches[index];
        if (index > 0) {
            detail += "; ";
        }
        detail += "source '" + cache.source_id + "' (" +
                  (cache.read.found_version > 0
                       ? "version " + std::to_string(cache.read.found_version)
                       : std::string("no version")) +
                  ", " + cache.read.detail + ")";
    }
    detail += ". This build reads versions " + readable +
              ". Run analyze_shot_quality on each source listed (CLI "
              "--shot-quality); nothing was re-analysed.";
    return detail;
}

std::string DescribeUnreadableShotQualityCaches(
    const std::vector<UnreadableShotQualityCache>& caches) {
    Value root = Value::MakeObject();
    root.Set("unreadable_caches", UnreadableCachesValue(caches));
    return root.Dump();
}
