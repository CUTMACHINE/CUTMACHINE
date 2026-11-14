#include "HlgDelivery.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

// BH3 -- Resolve's HLG signal for each grey of the 20 October 2026 chart:
// scene reflectance (the S-Log3 code of the band, decoded) and the G' of the
// rendered band. Resolve holds 0.99615 from 16.6x white up; the last two
// points are raised to 0.99845 and 1.0 so the curve stays strictly
// increasing, which the inverse a graphic goes through needs (BH1).
struct CurvePoint {
    double reflectance;
    double signal;
};
constexpr CurvePoint kResolveGreys[] = {
    {0.002510, 0.01346},  {0.003395, 0.01801},  {0.004871, 0.02712},
    {0.006643, 0.03621},  {0.009152, 0.04985},  {0.012759, 0.06854},
    {0.017623, 0.08902},  {0.024425, 0.11403},  {0.034051, 0.14246},
    {0.046866, 0.17430},  {0.065374, 0.21297},  {0.089907, 0.25618},
    {0.124776, 0.30622},  {0.173424, 0.36420},  {0.239632, 0.42902},
    {0.332742, 0.50294},  {0.460582, 0.57458},  {0.641818, 0.63940},
    {0.884940, 0.69625},  {1.229610, 0.74856},  {1.707025, 0.79747},
    {2.347457, 0.83954},  {3.255391, 0.87706},  {4.513001, 0.91118},
    {6.254956, 0.94043},  {8.667796, 0.96431},  {12.009899, 0.98364},
    {16.639156, 0.99615}, {23.051291, 0.99845}, {31.932950, 1.0},
};
constexpr size_t kPointCount = sizeof(kResolveGreys) / sizeof(kResolveGreys[0]);

// Fritsch-Carlson slopes of the signal against the logarithm of reflectance:
// a monotone cubic through the measured points, smooth between the half-stop
// steps of the chart and never overshooting them.
struct MonotoneCurve {
    double log[kPointCount];
    double slope[kPointCount];
};

MonotoneCurve BuildCurve() {
    MonotoneCurve curve{};
    double secant[kPointCount - 1];
    for (size_t i = 0; i < kPointCount; ++i) {
        curve.log[i] = std::log(kResolveGreys[i].reflectance);
    }
    for (size_t i = 0; i + 1 < kPointCount; ++i) {
        secant[i] = (kResolveGreys[i + 1].signal - kResolveGreys[i].signal) /
                    (curve.log[i + 1] - curve.log[i]);
    }
    curve.slope[0] = secant[0];
    curve.slope[kPointCount - 1] = secant[kPointCount - 2];
    for (size_t i = 1; i + 1 < kPointCount; ++i) {
        curve.slope[i] = secant[i - 1] * secant[i] <= 0.0
                             ? 0.0
                             : (secant[i - 1] + secant[i]) / 2.0;
    }
    for (size_t i = 0; i + 1 < kPointCount; ++i) {
        const double alpha = curve.slope[i] / secant[i];
        const double beta = curve.slope[i + 1] / secant[i];
        const double norm = alpha * alpha + beta * beta;
        if (norm > 9.0) {
            const double tau = 3.0 / std::sqrt(norm);
            curve.slope[i] = tau * alpha * secant[i];
            curve.slope[i + 1] = tau * beta * secant[i];
        }
    }
    return curve;
}

const MonotoneCurve& Curve() {
    static const MonotoneCurve curve = BuildCurve();
    return curve;
}

// BT.2408: the HLG reference white, 75 % of the signal, is reflectance 1.0.
double Bt2408SceneScale() {
    static const double scale = DecodeHlg(0.75);
    return scale;
}

ColorMatrix3 Inverse(const ColorMatrix3& m) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    ColorMatrix3 inverse{};
    inverse[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inverse[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inverse[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inverse[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    inverse[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inverse[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inverse[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inverse[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inverse[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return inverse;
}

ColorMatrix3 Multiply(const ColorMatrix3& a, const ColorMatrix3& b) {
    ColorMatrix3 product{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            for (int k = 0; k < 3; ++k) {
                product[row][column] += a[row][k] * b[k][column];
            }
        }
    }
    return product;
}

// RGB to XYZ from the chromaticities of the primaries and the white point
// (SMPTE RP 177).
ColorMatrix3 RgbToXyz(const double primaries[3][2], const double white[2]) {
    ColorMatrix3 columns{};
    for (int column = 0; column < 3; ++column) {
        const double x = primaries[column][0];
        const double y = primaries[column][1];
        columns[0][column] = x / y;
        columns[1][column] = 1.0;
        columns[2][column] = (1.0 - x - y) / y;
    }
    const double whiteXyz[3] = {white[0] / white[1], 1.0,
                                (1.0 - white[0] - white[1]) / white[1]};
    const ColorMatrix3 inverse = Inverse(columns);
    double scale[3] = {};
    for (int row = 0; row < 3; ++row) {
        for (int k = 0; k < 3; ++k) {
            scale[row] += inverse[row][k] * whiteXyz[k];
        }
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            columns[row][column] *= scale[column];
        }
    }
    return columns;
}

constexpr double kD65[2] = {0.3127, 0.3290};
constexpr double kRec2020Primaries[3][2] = {
    {0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
// Blackmagic Design, DaVinci Wide Gamut Intermediate (2020).
constexpr double kDaVinciWideGamutPrimaries[3][2] = {
    {0.8000, 0.3130}, {0.1682, 0.9877}, {0.0790, -0.1155}};

RgbColor Apply(const ColorMatrix3& m, RgbColor value) {
    return {m[0][0] * value.red + m[0][1] * value.green + m[0][2] * value.blue,
            m[1][0] * value.red + m[1][1] * value.green + m[1][2] * value.blue,
            m[2][0] * value.red + m[2][1] * value.green + m[2][2] * value.blue};
}

}  // namespace

double HlgDeliverySignal(double reflectance) {
    if (!(reflectance > 0.0)) {
        return 0.0;
    }
    const CurvePoint& first = kResolveGreys[0];
    const CurvePoint& last = kResolveGreys[kPointCount - 1];
    // Under the darkest grey the signal falls linearly to black, as Resolve's
    // three darkest greys do (about 5.4 signal per unit of reflectance).
    if (reflectance <= first.reflectance) {
        return first.signal * reflectance / first.reflectance;
    }
    if (reflectance >= last.reflectance) {
        return last.signal;
    }
    const MonotoneCurve& curve = Curve();
    const double u = std::log(reflectance);
    const size_t upper = static_cast<size_t>(
        std::upper_bound(curve.log, curve.log + kPointCount, u) - curve.log);
    const size_t i = upper - 1;
    const double h = curve.log[i + 1] - curve.log[i];
    const double t = (u - curve.log[i]) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * kResolveGreys[i].signal +
           (t3 - 2.0 * t2 + t) * h * curve.slope[i] +
           (-2.0 * t3 + 3.0 * t2) * kResolveGreys[i + 1].signal +
           (t3 - t2) * h * curve.slope[i + 1];
}

double HlgDeliveryTone(double reflectance) {
    if (!(reflectance > 0.0)) {
        return reflectance;
    }
    return DecodeHlg(HlgDeliverySignal(reflectance)) / Bt2408SceneScale();
}

double HlgDeliveryEncode(double reflectance) {
    return std::clamp(
        EncodeHlg(std::max(reflectance, 0.0) * Bt2408SceneScale()), 0.0, 1.0);
}

const ColorMatrix3& Rec2020ToDaVinciWideGamut() {
    static const ColorMatrix3 matrix =
        Multiply(Inverse(RgbToXyz(kDaVinciWideGamutPrimaries, kD65)),
                 RgbToXyz(kRec2020Primaries, kD65));
    return matrix;
}

const ColorMatrix3& DaVinciWideGamutToRec2020() {
    static const ColorMatrix3 matrix = Inverse(Rec2020ToDaVinciWideGamut());
    return matrix;
}

RgbColor HlgDeliverySignalOf(RgbColor rec2020Linear) {
    const RgbColor wide = Apply(Rec2020ToDaVinciWideGamut(), rec2020Linear);
    const RgbColor toned{HlgDeliveryTone(wide.red), HlgDeliveryTone(wide.green),
                         HlgDeliveryTone(wide.blue)};
    const RgbColor back = Apply(DaVinciWideGamutToRec2020(), toned);
    return {HlgDeliveryEncode(back.red), HlgDeliveryEncode(back.green),
            HlgDeliveryEncode(back.blue)};
}
