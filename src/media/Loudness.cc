#include "Loudness.h"

#include "AudioDspTruePeak.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;

// A second-order section, transposed direct form II, in double: the
// K-weighting's two stages.
struct Biquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;

    double Process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// BS.1770's K-weighting at any rate: the high shelf of the head's effect
// and the RLB high-pass, from the analogue prototypes (the 48 kHz
// coefficients of the recommendation come out of these to their printed
// digits).
void KWeighting(int32_t rate, Biquad& shelf, Biquad& highPass) {
    {
        const double f0 = 1681.974450955533;
        const double gain = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / rate);
        const double vh = std::pow(10.0, gain / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / rate);
        const double a0 = 1.0 + k / q + k * k;
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        highPass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

std::optional<double> Integrated(const std::vector<float>& left,
                                 const std::vector<float>& right,
                                 int32_t rate) {
    const size_t length = std::min(left.size(), right.size());
    // Each sample's K-weighted power, both channels, summed from the start.
    std::vector<double> sums(length + 1, 0.0);
    Biquad shelfL;
    Biquad passL;
    Biquad shelfR;
    Biquad passR;
    KWeighting(rate, shelfL, passL);
    KWeighting(rate, shelfR, passR);
    for (size_t n = 0; n < length; ++n) {
        const double l = passL.Process(shelfL.Process(left[n]));
        const double r = passR.Process(shelfR.Process(right[n]));
        sums[n + 1] = sums[n] + l * l + r * r;
    }
    // Blocks of 400 ms every 100 ms.
    const size_t step = static_cast<size_t>(rate) / 10;
    const size_t block = 4 * step;
    if (step == 0 || length < block) {
        return std::nullopt;
    }
    std::vector<double> powers;
    for (size_t start = 0; start + block <= length; start += step) {
        powers.push_back((sums[start + block] - sums[start]) /
                         static_cast<double>(block));
    }
    const auto loudness = [](double power) {
        return -0.691 + 10.0 * std::log10(power);
    };
    // Absolute gate at -70 LUFS, then a relative gate 10 LU under the
    // loudness of what passed it.
    double sum = 0.0;
    size_t count = 0;
    for (const double power : powers) {
        if (power > 0.0 && loudness(power) > -70.0) {
            sum += power;
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    const double relative = loudness(sum / static_cast<double>(count)) - 10.0;
    sum = 0.0;
    count = 0;
    for (const double power : powers) {
        if (power > 0.0 && loudness(power) > -70.0 &&
            loudness(power) > relative) {
            sum += power;
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return loudness(sum / static_cast<double>(count));
}

// Four times oversampling by a Kaiser-windowed sinc, 32 taps a phase: the
// samples between the samples, where a reconstructed (or encoded) signal
// peaks above what the samples show. FC5 (FILMCRAFT-2026-10) moved the
// filter to AudioDspTruePeak.h, shared with the streaming limiter.
constexpr int kPhases = audio_dsp::kTruePeakPhases;
constexpr int kTaps = audio_dsp::kTruePeakTaps;

// The largest magnitude of one channel's oversampled values that fall
// between sample m and the next, its own included, for every m: what a
// limiter must keep under its ceiling at m.
std::vector<double> PeakEnvelope(const std::vector<float>& x) {
    const std::vector<double>& h = audio_dsp::TruePeakInterpolator();
    const size_t length = x.size();
    // Phase p of output n stands at n - (centre - p) / kPhases: shifted by
    // `delay` samples, it falls in [m, m + 1).
    const int delay = (kPhases * kTaps) / (2 * kPhases);
    std::vector<double> envelope(length, 0.0);
    for (size_t m = 0; m < length; ++m) {
        double most = std::abs(static_cast<double>(x[m]));
        const size_t n = m + static_cast<size_t>(delay);
        for (int p = 0; p < kPhases; ++p) {
            double acc = 0.0;
            for (int j = 0; j < kTaps; ++j) {
                if (n < static_cast<size_t>(j)) {
                    break;
                }
                const size_t at = n - static_cast<size_t>(j);
                if (at < length) {
                    acc += h[static_cast<size_t>(j) * kPhases +
                             static_cast<size_t>(p)] *
                           x[at];
                }
            }
            most = std::max(most, std::abs(acc));
        }
        envelope[m] = most;
    }
    return envelope;
}

double ToDb(double amplitude) {
    return amplitude > 0.0 ? 20.0 * std::log10(amplitude)
                           : -std::numeric_limits<double>::infinity();
}

// Gain applied, then a look-ahead limiter at `ceiling` (linear) driven by
// the oversampled peaks, both channels together. Returns the lowest gain
// the limiter set.
double Limit(const std::vector<double>& envelope, double gain, double ceiling,
             int32_t rate, std::vector<float>& left,
             std::vector<float>& right) {
    const size_t length = envelope.size();
    const size_t window =
        std::max<size_t>(1, static_cast<size_t>(rate) * 5 / 1000);
    const double release =
        1.0 - std::exp(-1.0 / (0.100 * static_cast<double>(rate)));
    // The gain each sample needs, then the lowest over the window ahead of
    // it: the gain is down before the peak.
    std::vector<double> needed(length);
    for (size_t n = 0; n < length; ++n) {
        const double peak = envelope[n] * gain;
        needed[n] = peak > ceiling ? ceiling / peak : 1.0;
    }
    std::vector<double> ahead(length, 1.0);
    {
        // A monotone queue: the minimum of needed[n .. n + window - 1].
        std::vector<size_t> queue;
        size_t head = 0;
        for (size_t i = length; i-- > 0;) {
            while (queue.size() > head && needed[queue.back()] >= needed[i]) {
                queue.pop_back();
            }
            queue.push_back(i);
            while (queue[head] >= i + window) {
                ++head;
            }
            ahead[i] = needed[queue[head]];
        }
    }
    // A slow return to unity, never above what the window ahead allows,
    // then the average over the window behind: each sample's gain is a
    // mean of values that are all at most what it needs.
    double held = 1.0;
    std::vector<double> released(length);
    for (size_t n = 0; n < length; ++n) {
        held = std::min(ahead[n], held + (1.0 - held) * release);
        released[n] = held;
    }
    double lowest = 1.0;
    double running = 0.0;
    for (size_t n = 0; n < length; ++n) {
        running += released[n];
        if (n >= window) {
            running -= released[n - window];
        }
        // Before a whole window, the missing values count as unity, as if
        // the signal had been silent before it began.
        const size_t counted = std::min(n + 1, window);
        const double smooth =
            (running + static_cast<double>(window - counted)) /
            static_cast<double>(window);
        const double applied = std::min(smooth, needed[n]);
        lowest = std::min(lowest, applied);
        left[n] = static_cast<float>(left[n] * gain * applied);
        right[n] = static_cast<float>(right[n] * gain * applied);
    }
    return lowest;
}

}  // namespace

double TruePeakDb(const std::vector<float>& left,
                  const std::vector<float>& right) {
    double most = 0.0;
    for (const std::vector<float>* channel : {&left, &right}) {
        for (const double value : PeakEnvelope(*channel)) {
            most = std::max(most, value);
        }
    }
    return ToDb(most);
}

LoudnessMeasure MeasureLoudness(const std::vector<float>& left,
                                const std::vector<float>& right, int32_t rate) {
    LoudnessMeasure measure;
    measure.integrated_lufs = Integrated(left, right, rate);
    measure.true_peak_dbtp = TruePeakDb(left, right);
    double most = 0.0;
    for (const std::vector<float>* channel : {&left, &right}) {
        for (const float value : *channel) {
            most = std::max(most, std::abs(static_cast<double>(value)));
        }
    }
    measure.sample_peak_dbfs = ToDb(most);
    return measure;
}

void NormalizeLoudness(std::vector<float>& left, std::vector<float>& right,
                       int32_t rate, const LoudnessTarget& target,
                       LoudnessResult& result) {
    result = LoudnessResult{};
    const size_t length = std::min(left.size(), right.size());
    left.resize(length);
    right.resize(length);
    result.before = MeasureLoudness(left, right, rate);
    if (!result.before.integrated_lufs.has_value()) {
        result.silent = true;
        result.after = result.before;
        return;
    }
    // The oversampled peaks scale with the gain: computed once.
    std::vector<double> envelope = PeakEnvelope(left);
    {
        const std::vector<double> other = PeakEnvelope(right);
        for (size_t n = 0; n < length; ++n) {
            envelope[n] = std::max(envelope[n], other[n]);
        }
    }
    const double ceilingDb = target.true_peak_dbtp;
    double gainDb = target.integrated_lufs - *result.before.integrated_lufs;
    std::vector<float> outLeft;
    std::vector<float> outRight;
    for (int pass = 0; pass < 8; ++pass) {
        gainDb = std::min(gainDb, 60.0);
        const double gain = std::pow(10.0, gainDb / 20.0);
        // The limiter's ceiling, lowered by what the smoothing let through.
        double limit = ceilingDb;
        double lowest = 1.0;
        for (int trial = 0; trial < 6; ++trial) {
            outLeft = left;
            outRight = right;
            lowest = Limit(envelope, gain, std::pow(10.0, limit / 20.0), rate,
                           outLeft, outRight);
            const double peak = TruePeakDb(outLeft, outRight);
            if (peak <= ceilingDb + 1e-3) {
                break;
            }
            limit -= peak - ceilingDb + 0.01;
        }
        result.gain_db = gainDb;
        result.limited_db = ToDb(lowest);
        const std::optional<double> reached =
            Integrated(outLeft, outRight, rate);
        if (!reached.has_value()) {
            break;
        }
        const double miss = target.integrated_lufs - *reached;
        if (std::abs(miss) <= 0.05) {
            break;
        }
        gainDb += miss;
    }
    left = std::move(outLeft);
    right = std::move(outRight);
    result.after = MeasureLoudness(left, right, rate);
}
