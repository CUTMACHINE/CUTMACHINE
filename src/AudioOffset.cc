#include "AudioOffset.h"

#include "FfmpegBinary.h"
#include "Fft.h"
#include "Json.h"
#include "ProcessLauncher.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

using Complex = std::complex<double>;
using mcp_json::Value;

size_t NextPowerOfTwo(size_t count) {
    size_t size = 1;
    while (size < count) {
        size <<= 1;
    }
    return size;
}

void InverseFft(std::vector<Complex>& data) {
    for (Complex& value : data) {
        value = std::conj(value);
    }
    fft::Forward(data);
    const double scale = 1.0 / static_cast<double>(data.size());
    for (Complex& value : data) {
        value = std::conj(value) * scale;
    }
}

// Where the sound gets louder, one value per `hop` samples: the rise of
// its log RMS (over 10 ms) across 10 ms, decays dropped. A rise is where a
// syllable starts, and a room's reverberation lengthens what follows it
// without moving it; the log makes a close and a distant microphone differ
// by an offset rather than a factor. Unlike the loudness itself, which
// stays alike over a whole word, rises are brief, so two unrelated takes do
// not line up by chance over seconds.
std::vector<double> Onsets(const std::vector<float>& samples, int32_t hop) {
    const int64_t count = static_cast<int64_t>(samples.size());
    std::vector<double> squares(static_cast<size_t>(count) + 1, 0.0);
    for (int64_t index = 0; index < count; ++index) {
        squares[index + 1] =
            squares[index] +
            static_cast<double>(samples[index]) * samples[index];
    }
    const int64_t values = count / hop;
    const int64_t half = int64_t{5} * hop;
    std::vector<double> loudness(static_cast<size_t>(values));
    for (int64_t value = 0; value < values; ++value) {
        const int64_t first = std::max<int64_t>(0, value * hop - half);
        const int64_t end = std::min<int64_t>(count, value * hop + half);
        const double energy =
            (squares[end] - squares[first]) / static_cast<double>(end - first);
        loudness[value] = std::log(std::sqrt(energy) + 1e-5);
    }
    std::vector<double> onsets(static_cast<size_t>(values), 0.0);
    for (int64_t value = 10; value < values; ++value) {
        onsets[value] = std::max(0.0, loudness[value] - loudness[value - 10]);
    }
    return onsets;
}

// Zero mean, unit variance. False for a constant envelope: nothing in it
// can be matched.
bool Standardize(std::vector<double>& values) {
    double mean = 0.0;
    for (const double value : values) {
        mean += value;
    }
    mean /= static_cast<double>(values.size());
    double variance = 0.0;
    for (const double value : values) {
        variance += (value - mean) * (value - mean);
    }
    variance /= static_cast<double>(values.size());
    if (variance < 1e-12) {
        return false;
    }
    const double scale = 1.0 / std::sqrt(variance);
    for (double& value : values) {
        value = (value - mean) * scale;
    }
    return true;
}

// The span [first, end) of the reference where other[n - lag] exists.
void OverlapAt(int64_t referenceCount, int64_t otherCount, int64_t lag,
               int64_t& first, int64_t& end) {
    first = std::max<int64_t>(0, lag);
    end = std::min<int64_t>(referenceCount, otherCount + lag);
}

// Pearson's correlation of reference[n] and other[n - lag] over their
// overlap.
double PearsonAt(const std::vector<double>& reference,
                 const std::vector<double>& other, int64_t lag) {
    int64_t first = 0;
    int64_t end = 0;
    OverlapAt(static_cast<int64_t>(reference.size()),
              static_cast<int64_t>(other.size()), lag, first, end);
    const double count = static_cast<double>(end - first);
    if (count < 2) {
        return 0.0;
    }
    double meanReference = 0.0;
    double meanOther = 0.0;
    for (int64_t index = first; index < end; ++index) {
        meanReference += reference[index];
        meanOther += other[index - lag];
    }
    meanReference /= count;
    meanOther /= count;
    double product = 0.0;
    double squaresReference = 0.0;
    double squaresOther = 0.0;
    for (int64_t index = first; index < end; ++index) {
        const double a = reference[index] - meanReference;
        const double b = other[index - lag] - meanOther;
        product += a * b;
        squaresReference += a * a;
        squaresOther += b * b;
    }
    if (squaresReference <= 0.0 || squaresOther <= 0.0) {
        return 0.0;
    }
    return product / std::sqrt(squaresReference * squaresOther);
}

double Rms(const std::vector<double>& values) {
    double energy = 0.0;
    for (const double value : values) {
        energy += value * value;
    }
    return values.empty()
               ? 0.0
               : std::sqrt(energy / static_cast<double>(values.size()));
}

// GCC-PHAT in one window: the shift s in [0, 2 * margin] that best lines
// segment[i] up with search[i + s]. False when either is silent.
bool PhaseCorrelationPeak(const std::vector<double>& segment,
                          const std::vector<double>& search, int64_t margin,
                          int64_t& shift) {
    if (Rms(segment) < 1e-4 || Rms(search) < 1e-4) {
        return false;
    }
    const size_t size = NextPowerOfTwo(search.size());
    std::vector<Complex> a(size);
    std::vector<Complex> b(size);
    std::copy(segment.begin(), segment.end(), a.begin());
    std::copy(search.begin(), search.end(), b.begin());
    fft::Forward(a);
    fft::Forward(b);
    double largest = 0.0;
    for (size_t index = 0; index < size; ++index) {
        a[index] = std::conj(a[index]) * b[index];
        largest = std::max(largest, std::abs(a[index]));
    }
    const double floor = largest * 1e-12;
    for (Complex& value : a) {
        const double magnitude = std::abs(value);
        value = magnitude > floor ? value / magnitude : Complex(0.0, 0.0);
    }
    InverseFft(a);
    double best = -std::numeric_limits<double>::infinity();
    for (int64_t candidate = 0; candidate <= 2 * margin; ++candidate) {
        if (a[candidate].real() > best) {
            best = a[candidate].real();
            shift = candidate;
        }
    }
    return true;
}

// a / b rounded to the nearest integer, halves away from zero.
int64_t RoundedQuotient(int64_t a, int64_t b) {
    const bool negative = (a < 0) != (b < 0);
    const int64_t magnitude =
        (std::llabs(a) * 2 + std::llabs(b)) / (std::llabs(b) * 2);
    return negative ? -magnitude : magnitude;
}

double Rounded(double value) { return std::round(value * 1000.0) / 1000.0; }

Value Samples(int64_t value, int32_t rate) {
    Value time = Value::MakeObject();
    time.Set("value", Value::MakeInt(value));
    time.Set("rate", Value::MakeInt(rate));
    return time;
}

}  // namespace

bool MeasureAudioOffset(const std::vector<float>& reference,
                        const std::vector<float>& other,
                        const AudioOffsetSettings& settings,
                        AudioOffsetReport& report, std::string& code,
                        std::string& error) {
    report = {};
    code.clear();
    error.clear();
    if (settings.sample_rate <= 0 || settings.envelope_hop <= 0 ||
        settings.frame_num <= 0 || settings.frame_den <= 0 ||
        settings.window <= 0 || settings.search_margin <= 0) {
        code = "InvalidInput";
        error = "the measure's settings must be positive";
        return false;
    }
    const int64_t referenceCount = static_cast<int64_t>(reference.size());
    const int64_t otherCount = static_cast<int64_t>(other.size());
    if (std::min(referenceCount, otherCount) < settings.minimum_overlap) {
        code = "NoOverlap";
        error = "one sound is shorter than the shortest overlap measured (" +
                std::to_string(settings.minimum_overlap) + " samples)";
        return false;
    }

    // Coarse: every lag at once, on the onsets.
    const int32_t hop = settings.envelope_hop;
    std::vector<double> referenceOnsets = Onsets(reference, hop);
    std::vector<double> otherOnsets = Onsets(other, hop);
    const std::vector<double> referenceRises = referenceOnsets;
    const std::vector<double> otherRises = otherOnsets;
    if (!Standardize(referenceOnsets) || !Standardize(otherOnsets)) {
        code = "WeakCorrelation";
        error = "one sound's loudness never changes: nothing to match";
        return false;
    }
    const int64_t na = static_cast<int64_t>(referenceOnsets.size());
    const int64_t nb = static_cast<int64_t>(otherOnsets.size());
    const size_t size = NextPowerOfTwo(static_cast<size_t>(na + nb - 1));
    std::vector<Complex> a(size);
    std::vector<Complex> b(size);
    std::copy(referenceOnsets.begin(), referenceOnsets.end(), a.begin());
    std::copy(otherOnsets.begin(), otherOnsets.end(), b.begin());
    fft::Forward(a);
    fft::Forward(b);
    for (size_t index = 0; index < size; ++index) {
        a[index] *= std::conj(b[index]);
    }
    InverseFft(a);
    // Each lag's sum over its overlap, divided by the overlap's square
    // root: unrelated sounds give every lag the same spread whatever its
    // overlap, and a true match grows with it.
    const int64_t minimumOverlap = settings.minimum_overlap / hop;
    std::vector<double> scores(static_cast<size_t>(na + nb - 1),
                               -std::numeric_limits<double>::infinity());
    std::vector<double> spread;
    int64_t bestLag = 0;
    double best = -std::numeric_limits<double>::infinity();
    for (int64_t lag = -(nb - 1); lag <= na - 1; ++lag) {
        int64_t first = 0;
        int64_t end = 0;
        OverlapAt(na, nb, lag, first, end);
        if (end - first < minimumOverlap) {
            continue;
        }
        const size_t index = lag >= 0 ? static_cast<size_t>(lag)
                                      : size - static_cast<size_t>(-lag);
        const double score =
            a[index].real() / std::sqrt(static_cast<double>(end - first));
        scores[static_cast<size_t>(lag + nb - 1)] = score;
        spread.push_back(score);
        if (score > best) {
            best = score;
            bestLag = lag;
        }
    }
    if (spread.empty()) {
        code = "NoOverlap";
        error = "no lag leaves the two sounds " +
                std::to_string(settings.minimum_overlap) + " samples in common";
        return false;
    }
    // The level chance reaches: the scores' median and their spread about
    // it (the median absolute deviation, as a standard deviation), which a
    // single true peak does not move.
    const auto middle =
        spread.begin() + static_cast<std::ptrdiff_t>(spread.size() / 2);
    std::nth_element(spread.begin(), middle, spread.end());
    const double median = *middle;
    for (double& value : spread) {
        value = std::abs(value - median);
    }
    std::nth_element(spread.begin(), middle, spread.end());
    const double deviation = std::max(1.4826 * *middle, 1e-12);
    double second = -std::numeric_limits<double>::infinity();
    for (int64_t lag = -(nb - 1); lag <= na - 1; ++lag) {
        if (std::llabs(lag - bestLag) > settings.peak_exclusion) {
            second =
                std::max(second, scores[static_cast<size_t>(lag + nb - 1)]);
        }
    }
    report.significance = Rounded((best - median) / deviation);
    report.peak_ratio = Rounded(
        second - median > 1e-12 ? (best - median) / (second - median) : 1e3);
    report.correlation =
        Rounded(PearsonAt(referenceRises, otherRises, bestLag));
    if (report.significance < settings.minimum_significance) {
        code = "WeakCorrelation";
        error = "the best lag stands " + std::to_string(report.significance) +
                " deviations above chance, under " +
                std::to_string(settings.minimum_significance) +
                ": not one take, or too little of it";
        return false;
    }
    if (report.peak_ratio < settings.minimum_peak_ratio) {
        code = "AmbiguousPeak";
        error = "another lag matches almost as well (peak ratio " +
                std::to_string(report.peak_ratio) +
                "): a periodic sound, a tick or a loop, cannot be placed";
        return false;
    }

    // Fine: windows spread over the overlap, each placed to the sample.
    const int64_t coarse = bestLag * hop;
    int64_t overlapFirst = 0;
    int64_t overlapEnd = 0;
    OverlapAt(referenceCount, otherCount, coarse, overlapFirst, overlapEnd);
    const int64_t overlap = overlapEnd - overlapFirst;
    if (overlap < settings.minimum_overlap) {
        code = "NoOverlap";
        error = "the two sounds overlap by " + std::to_string(overlap) +
                " samples, under " + std::to_string(settings.minimum_overlap);
        return false;
    }
    const int64_t window = std::min(settings.window, overlap);
    std::vector<int64_t> starts;
    if (overlap >= 3 * window) {
        for (int part = 0; part < 3; ++part) {
            const int64_t centre = overlapFirst + overlap * (2 * part + 1) / 6;
            starts.push_back(std::clamp(centre - window / 2, overlapFirst,
                                        overlapEnd - window));
        }
    } else {
        starts.push_back(overlapFirst + (overlap - window) / 2);
    }
    const int64_t margin = settings.search_margin;
    for (const int64_t start : starts) {
        std::vector<double> segment(static_cast<size_t>(window));
        for (int64_t index = 0; index < window; ++index) {
            segment[index] = reference[start + index];
        }
        std::vector<double> search(static_cast<size_t>(window + 2 * margin),
                                   0.0);
        const int64_t searchFirst = start - coarse - margin;
        for (int64_t index = 0; index < window + 2 * margin; ++index) {
            const int64_t source = searchFirst + index;
            if (source >= 0 && source < otherCount) {
                search[index] = other[source];
            }
        }
        int64_t shift = 0;
        if (!PhaseCorrelationPeak(segment, search, margin, shift)) {
            continue;
        }
        if (shift == 0 || shift == 2 * margin) {
            code = "ClockDrift";
            error = "a window finds the other sound " + std::to_string(margin) +
                    " samples or more from the rest: the clocks drift too "
                    "far for one offset";
            return false;
        }
        report.windows.push_back({start, coarse + margin - shift});
    }
    if (report.windows.empty()) {
        code = "WeakCorrelation";
        error = "every window of the overlap is silent in one sound";
        return false;
    }
    report.offset = report.windows[report.windows.size() / 2].offset;
    if (report.windows.size() >= 2) {
        const AudioOffsetWindow& head = report.windows.front();
        const AudioOffsetWindow& tail = report.windows.back();
        const int64_t moved = tail.offset - head.offset;
        const int64_t span = tail.at - head.at;
        report.drift_ppm = Rounded(static_cast<double>(moved) * 1e6 /
                                   static_cast<double>(span));
        report.drift_samples = RoundedQuotient(moved * overlap, span);
    }
    OverlapAt(referenceCount, otherCount, report.offset, report.overlap_first,
              report.overlap_end);
    // Half a frame, exactly: |drift| > rate * num / (2 * den).
    if (std::llabs(report.drift_samples) * 2 * settings.frame_den >
        static_cast<int64_t>(settings.sample_rate) * settings.frame_num) {
        code = "ClockDrift";
        error = "the clocks drift by " + std::to_string(report.drift_samples) +
                " samples over the overlap (" +
                std::to_string(report.drift_ppm) +
                " ppm), more than half a frame: one offset would be wrong at "
                "the end of the take";
        return false;
    }
    return true;
}

std::string AudioOffsetReportJson(const AudioOffsetReport& report,
                                  const AudioOffsetSettings& settings) {
    const int32_t rate = settings.sample_rate;
    Value result = Value::MakeObject();
    result.Set("offset", Samples(report.offset, rate));
    Value frames = Value::MakeObject();
    frames.Set("frames", Value::MakeInt(RoundedQuotient(
                             report.offset * settings.frame_den,
                             static_cast<int64_t>(rate) * settings.frame_num)));
    frames.Set("frame_rate",
               Value::MakeString(std::to_string(settings.frame_den) + "/" +
                                 std::to_string(settings.frame_num)));
    frames.Set("rounding", Value::MakeString("nearest"));
    result.Set("offset_frames", std::move(frames));
    Value overlap = Value::MakeObject();
    overlap.Set("start", Samples(report.overlap_first, rate));
    overlap.Set("duration",
                Samples(report.overlap_end - report.overlap_first, rate));
    result.Set("overlap", std::move(overlap));
    Value confidence = Value::MakeObject();
    confidence.Set("correlation", Value::MakeDouble(report.correlation));
    confidence.Set("significance", Value::MakeDouble(report.significance));
    confidence.Set("peak_ratio", Value::MakeDouble(report.peak_ratio));
    result.Set("confidence", std::move(confidence));
    Value windows = Value::MakeArray();
    for (const AudioOffsetWindow& window : report.windows) {
        Value item = Value::MakeObject();
        item.Set("at", Samples(window.at, rate));
        item.Set("offset", Samples(window.offset, rate));
        windows.Push(std::move(item));
    }
    result.Set("windows", std::move(windows));
    Value drift = Value::MakeObject();
    drift.Set("ppm", Value::MakeDouble(report.drift_ppm));
    drift.Set("samples", Value::MakeInt(report.drift_samples));
    drift.Set("limit_samples",
              Value::MakeInt(static_cast<int64_t>(rate) * settings.frame_num /
                             (2 * settings.frame_den)));
    result.Set("drift", std::move(drift));
    return result.Dump();
}

bool ParseAudioOffsetChannel(const std::string& name,
                             AudioOffsetChannel& channel) {
    if (name == "mix") {
        channel = AudioOffsetChannel::Mix;
    } else if (name == "left") {
        channel = AudioOffsetChannel::Left;
    } else if (name == "right") {
        channel = AudioOffsetChannel::Right;
    } else {
        return false;
    }
    return true;
}

bool DecodeAudioForOffset(const std::string& path, AudioOffsetChannel channel,
                          int32_t sampleRate,
                          const std::function<bool()>& cancelled,
                          std::vector<float>& samples, std::string& error) {
    samples.clear();
    error.clear();
    std::vector<std::string> arguments = {
        "",   "-hide_banner", "-loglevel", "error", "-nostdin", "-i",
        path, "-map",         "0:a:0",     "-vn",   "-sn"};
    if (channel == AudioOffsetChannel::Mix) {
        arguments.insert(arguments.end(), {"-ac", "1"});
    } else {
        arguments.insert(
            arguments.end(),
            {"-af", channel == AudioOffsetChannel::Left ? "pan=mono|c0=c0"
                                                        : "pan=mono|c0=c1"});
    }
    arguments.insert(arguments.end(), {"-ar", std::to_string(sampleRate), "-f",
                                       "f32le", "pipe:1"});
    std::vector<char> pending;
    process::Request request;
    request.arguments = std::move(arguments);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 65536;
    request.error_tail_bytes = 4096;
    request.cancelled = cancelled;
    request.on_output = [&](const char* bytes, size_t count) {
        pending.insert(pending.end(), bytes, bytes + count);
        const size_t whole = pending.size() / sizeof(float);
        const size_t first = samples.size();
        samples.resize(first + whole);
        std::memcpy(samples.data() + first, pending.data(),
                    whole * sizeof(float));
        pending.erase(pending.begin(),
                      pending.begin() +
                          static_cast<std::ptrdiff_t>(whole * sizeof(float)));
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "audio offset process pipes");
        return false;
    }
    if (result.cancelled || (cancelled && cancelled())) {
        error = "audio offset measure cancelled";
        return false;
    }
    if (!result.Succeeded()) {
        error = result.error_tail.empty() ? "FFmpeg could not decode '" + path +
                                                "' for the audio offset"
                                          : result.error_tail;
        return false;
    }
    return true;
}
