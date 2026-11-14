#include "ResolveContrastTable.h"

#include "ColorManagement.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr double kResolveContrastShift = 0.0005;

// The four nodes nearest to `value` on an increasing axis, and their
// Lagrange weights; at the ends, the four first or last.
template <size_t N>
void CubicStencil(const std::array<float, N>& axis, float value, size_t& first,
                  std::array<double, 4>& weights) {
    const float clamped = std::clamp(value, axis.front(), axis.back());
    size_t above = static_cast<size_t>(
        std::upper_bound(axis.begin(), axis.end(), clamped) - axis.begin());
    above = std::clamp<size_t>(above, 2, N - 2);
    first = above - 2;
    for (size_t i = 0; i < 4; ++i) {
        double weight = 1.0;
        for (size_t j = 0; j < 4; ++j) {
            if (j == i) continue;
            weight *= (static_cast<double>(clamped) - axis[first + j]) /
                      (static_cast<double>(axis[first + i]) - axis[first + j]);
        }
        weights[i] = weight;
    }
}

// Catmull-Rom on `count` uniform samples over [0, end], the end slopes past
// the ends. The neighbour missing at either end is the straight line
// through the last two: a curve that is a line stays exactly one.
template <typename Sample>
double AlongSamples(Sample sample, int32_t count, double end, double u) {
    const int32_t last = count - 1;
    const double step = end / last;
    if (u <= 0.0) return sample(0) + (u / step) * (sample(1) - sample(0));
    if (u >= end) {
        return sample(last) +
               ((u - end) / step) * (sample(last) - sample(last - 1));
    }
    const double f = u / step;
    const int32_t i = std::min(static_cast<int32_t>(f), last - 1);
    const double t = f - i;
    const double p1 = sample(i);
    const double p2 = sample(i + 1);
    const double p0 = i > 0 ? sample(i - 1) : (2.0 * p1) - p2;
    const double p3 = i + 2 <= last ? sample(i + 2) : (2.0 * p2) - p1;
    return 0.5 * ((2.0 * p1) + ((p2 - p0) * t) +
                  ((2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t) +
                  ((3.0 * p1 - p0 - 3.0 * p2 + p3) * t * t * t));
}

double Hlg(double light) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (light < 0.0) return -Hlg(-light);
    if (light <= 1.0 / 12.0) return std::sqrt(3.0 * light);
    return a * std::log(12.0 * light - b) + c;
}

double HlgInverse(double code) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (code < 0.0) return -HlgInverse(-code);
    if (code <= 0.5) return code * code / 3.0;
    return (std::exp((code - c) / a) + b) / 12.0;
}

}  // namespace

float ResolveAwareSCurve(float contrast, float pivot, float x) {
    using resolve_contrast_table::kContrasts;
    using resolve_contrast_table::kCurves;
    using resolve_contrast_table::kPivots;
    size_t c0 = 0;
    size_t p0 = 0;
    std::array<double, 4> cw{};
    std::array<double, 4> pw{};
    CubicStencil(kContrasts, contrast, c0, cw);
    CubicStencil(kPivots, pivot, p0, pw);
    const auto sample = [&](int32_t index) {
        double value = 0.0;
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < 4; ++j) {
                const size_t curve = ((c0 + i) * kPivots.size()) + (p0 + j);
                value += cw[i] * pw[j] *
                         kCurves[(curve * kResolveContrastSamples) +
                                 static_cast<size_t>(index)];
            }
        }
        return value / 65535.0;
    };
    return static_cast<float>(
        AlongSamples(sample, kResolveContrastSamples, 1.0, x));
}

float ResolveContrastToHlg(float diValue) {
    return static_cast<float>(Hlg(DecodeDaVinciIntermediate(diValue) / 12.0));
}

float ResolveContrastFromHlg(float hlgCode) {
    return static_cast<float>(
        EncodeDaVinciIntermediate(HlgInverse(hlgCode) * 12.0));
}

bool ResolveContrastCurve(float contrast, float pivot, bool aware, bool sCurve,
                          std::array<float, 65>& samples, float& end) {
    if (contrast == 1.0f) return false;
    // The curve spans [0, T(1)], T the HLG code of the aware contrast: with
    // the aware tools, the codes of the values 0 to 1; without them the
    // values themselves, up to where the table, read through HLG, still
    // says what Resolve does above 1 (a gain of 1.2 takes a channel there).
    end = ResolveContrastToHlg(1.0f);
    const bool straight = contrast < 1.0f || !sCurve;
    const double slope = contrast <= 1.0f
                             ? contrast
                             : 1.0 / (2.0 - static_cast<double>(contrast));
    for (int32_t i = 0; i < kResolveContrastSamples; ++i) {
        const double u = end * i / (kResolveContrastSamples - 1);
        double value = 0.0;
        if (straight) {
            // Resolve takes 0.0005 off the value first (not below 0): the
            // contrast 0.7 lands within 0.014 of a 10-bit code with it, 0.37
            // without.
            value =
                ((std::max(u - kResolveContrastShift, 0.0) - pivot) * slope) +
                pivot;
        } else {
            // The table holds the aware curve on node values; its plain
            // curve, on HLG codes or on values alike, is the same read
            // through HLG.
            const float node = ResolveContrastFromHlg(static_cast<float>(u));
            value =
                ResolveContrastToHlg(ResolveAwareSCurve(contrast, pivot, node));
        }
        samples[static_cast<size_t>(i)] = static_cast<float>(value);
    }
    return true;
}

float ApplyResolveContrastCurve(const float* samples, float end, float u) {
    return static_cast<float>(AlongSamples(
        [samples](int32_t index) {
            return static_cast<double>(samples[index]);
        },
        kResolveContrastSamples, end, u));
}
