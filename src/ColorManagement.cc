#include "ColorManagement.h"

#include "ColorEffects.h"
#include "Document.h"
#include "HlgDelivery.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange) {
    if (bitDepth < 8 || bitDepth > 16)
        throw std::invalid_argument("YUV bit depth must be between 8 and 16");
    const uint32_t maximumCode =
        bitDepth == 16 ? 65535u : ((1u << bitDepth) - 1u);
    YuvCodeParameters result;
    result.sample_scale = bitDepth > 8 ? 65535.0f / maximumCode : 1.0f;
    if (fullRange) {
        result.chroma_offset =
            static_cast<float>(1u << (bitDepth - 1)) / maximumCode;
        return result;
    }
    const uint32_t shift = static_cast<uint32_t>(bitDepth - 8);
    result.y_offset = static_cast<float>(16u << shift) / maximumCode;
    result.y_scale = static_cast<float>(maximumCode) / (219u << shift);
    result.chroma_offset = static_cast<float>(128u << shift) / maximumCode;
    result.chroma_scale = static_cast<float>(maximumCode) / (224u << shift);
    return result;
}

YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant) {
    if (bt2020NonConstant) return {1.4746f, -0.164553f, -0.571353f, 1.8814f};
    return {};
}

bool ResolveYuvFullRange(const ColorManagementSettings& settings,
                         YuvRangeTag tag, bool rgb) {
    // MOTION-2026-09 M4 -- an RGB picture has no YCbCr range of its own,
    // the rule the export already follows (Export.cc).
    if (rgb) return true;
    if (settings.input_range != "auto") return settings.input_range == "full";
    return tag == YuvRangeTag::Full ||
           (tag == YuvRangeTag::Unspecified &&
            settings.input_transfer == "sony_slog3");
}

bool ResolveYuvBt2020Matrix(const ColorManagementSettings& settings,
                            bool taggedBt2020NonConstant) {
    if (settings.input_ycbcr_matrix != "auto")
        return settings.input_ycbcr_matrix == "bt2020_ncl";
    return taggedBt2020NonConstant;
}

double DecodeSonySLog3(double signal) {
    constexpr double breakpoint = 171.2102946929 / 1023.0;
    if (signal >= breakpoint)
        return std::pow(10.0, (signal * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
    return (signal * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
}

double EncodeAcesCct(double linearAp1) {
    if (linearAp1 <= 0.0078125)
        return linearAp1 * 10.5402377416545 + 0.0729055341958355;
    return (std::log2(linearAp1) + 9.72) / 17.52;
}

double DecodeAcesCct(double acesCct) {
    constexpr double breakpoint = 0.155251141552511;
    if (acesCct <= breakpoint)
        return (acesCct - 0.0729055341958355) / 10.5402377416545;
    return std::exp2(acesCct * 17.52 - 9.72);
}

double EncodeDaVinciIntermediate(double linear) {
    if (linear <= 0.00262409) return linear * 10.44426855;
    return (std::log2(linear + 0.0075) + 7.0) * 0.07329248;
}

double DecodeDaVinciIntermediate(double code) {
    if (code <= 0.02740668) return code / 10.44426855;
    return std::exp2(code / 0.07329248 - 7.0) - 0.0075;
}

double EncodeHlg(double sceneLinear) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (sceneLinear <= 0.0) return 0.0;
    if (sceneLinear <= 1.0 / 12.0) return std::sqrt(3.0 * sceneLinear);
    return a * std::log(12.0 * sceneLinear - b) + c;
}

double DecodeHlg(double signal) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (signal <= 0.5) return signal * signal / 3.0;
    return (std::exp((signal - c) / a) + b) / 12.0;
}

double HlgSceneReflectionScale() { return DecodeHlg(0.75) / 0.9; }

namespace {

RgbColor Multiply(RgbColor value, const double matrix[3][3]) {
    return {
        value.red * matrix[0][0] + value.green * matrix[0][1] +
            value.blue * matrix[0][2],
        value.red * matrix[1][0] + value.green * matrix[1][1] +
            value.blue * matrix[1][2],
        value.red * matrix[2][0] + value.green * matrix[2][1] +
            value.blue * matrix[2][2],
    };
}

// BT.2087 -- linear Rec.709 primaries to linear Rec.2020, and its inverse.
constexpr double kRec709ToRec2020[3][3] = {{0.6274039, 0.3292830, 0.0433131},
                                           {0.0690973, 0.9195404, 0.0113623},
                                           {0.0163914, 0.0880133, 0.8955953}};
constexpr double kRec2020ToRec709[3][3] = {{1.6604910, -0.5876411, -0.0728499},
                                           {-0.1245505, 1.1328999, -0.0083494},
                                           {-0.0181508, -0.1005789, 1.1187297}};

// BH1 -- one transfer for both ends: a graphic's code value is read as an
// HLG signal, so a neutral passes the conversion unchanged.
RgbColor ConvertThroughHlgLight(RgbColor value, const double matrix[3][3]) {
    const auto light = [](double signal) {
        return DecodeHlg(std::clamp(signal, 0.0, 1.0));
    };
    const RgbColor converted = Multiply(
        {light(value.red), light(value.green), light(value.blue)}, matrix);
    const auto encode = [](double scene) {
        return std::clamp(EncodeHlg(std::max(scene, 0.0)), 0.0, 1.0);
    };
    return {encode(converted.red), encode(converted.green),
            encode(converted.blue)};
}

}  // namespace

bool GraphicsPlacedInHlg(const ColorManagementSettings& settings) {
    return settings.enabled && settings.output_transfer == "hlg";
}

RgbColor GraphicsCodeToHlgSignal(RgbColor code) {
    return ConvertThroughHlgLight(code, kRec709ToRec2020);
}

RgbColor HlgSignalToGraphicsCode(RgbColor signal) {
    return ConvertThroughHlgLight(signal, kRec2020ToRec709);
}

namespace {

double DecodeRec709(double signal) {
    const double positive = std::max(signal, 0.0);
    return positive >= 0.081 ? std::pow((positive + 0.099) / 1.099, 1.0 / 0.45)
                             : positive / 4.5;
}

double EncodeRec709(double linear) {
    linear = std::max(linear, 0.0);
    return std::clamp(
        linear >= 0.018 ? 1.099 * std::pow(linear, 0.45) - 0.099 : 4.5 * linear,
        0.0, 1.0);
}

RgbColor DecodeTransfer(RgbColor value, const std::string& transfer) {
    auto decode = [&](double channel) {
        if (transfer == "linear") return channel;
        if (transfer == "sony_slog3") return DecodeSonySLog3(channel);
        // ITW5 -- BT.2100 HLG camera signal, back to scene reflectance: the
        // inverse of the HLG delivery encoding, so HLG in and out round-trip.
        if (transfer == "hlg")
            return DecodeHlg(channel) / HlgSceneReflectionScale();
        return DecodeRec709(channel);
    };
    return {decode(value.red), decode(value.green), decode(value.blue)};
}

RgbColor SourceToAp1(RgbColor rgb, const std::string& gamut) {
    RgbColor ap0;
    if (gamut == "sony_sgamut3_cine") {
        static constexpr double matrix[3][3] = {
            {0.6387886672, 0.2723514337, 0.0888598991},
            {-0.0039159060, 1.0880732309, -0.0841573249},
            {-0.0299072021, -0.0264325799, 1.0563397820},
        };
        ap0 = Multiply(rgb, matrix);
    } else if (gamut == "sony_sgamut3") {
        static constexpr double matrix[3][3] = {
            {0.7529825954, 0.1433702162, 0.1036471884},
            {0.0217076974, 1.0153188355, -0.0370265329},
            {-0.0094160527, 0.0033704179, 1.0060456349},
        };
        ap0 = Multiply(rgb, matrix);
    } else {
        static constexpr double rec709ToXyz[3][3] = {
            {0.412391, 0.357584, 0.180481},
            {0.212639, 0.715169, 0.072192},
            {0.019331, 0.119195, 0.950532},
        };
        static constexpr double rec2020ToXyz[3][3] = {
            {0.636958, 0.144617, 0.168881},
            {0.262700, 0.677998, 0.059302},
            {0.000000, 0.028073, 1.060985},
        };
        static constexpr double d65ToD60[3][3] = {
            {1.013030, 0.006105, -0.014971},
            {0.007698, 0.998165, -0.005032},
            {-0.002841, 0.004685, 0.924507},
        };
        static constexpr double xyzToAp0[3][3] = {
            {1.049811, 0.000000, -0.000097},
            {-0.495903, 1.373313, 0.098240},
            {0.000000, 0.000000, 0.991252},
        };
        const RgbColor xyz =
            Multiply(rgb, gamut == "rec2020" ? rec2020ToXyz : rec709ToXyz);
        ap0 = Multiply(Multiply(xyz, d65ToD60), xyzToAp0);
    }
    static constexpr double ap0ToAp1[3][3] = {
        {1.4514393161, -0.2365107469, -0.2149285693},
        {-0.0765537734, 1.1762296998, -0.0996759264},
        {0.0083161484, -0.0060324498, 0.9977163014},
    };
    return Multiply(ap0, ap0ToAp1);
}

RgbColor Ap1ToOutput(RgbColor ap1, const std::string& gamut) {
    static constexpr double ap1ToXyzD60[3][3] = {
        {0.6624541811, 0.1340042065, 0.1561876870},
        {0.2722287168, 0.6740817658, 0.0536895174},
        {-0.0055746495, 0.0040607335, 1.0103391003},
    };
    static constexpr double d60ToD65[3][3] = {
        {0.9872240087, -0.0061132286, 0.0159532883},
        {-0.0075983718, 1.0018614847, 0.0053300358},
        {0.0030725771, -0.0050959615, 1.0816806031},
    };
    static constexpr double xyzToRec2020[3][3] = {
        {1.716651, -0.355671, -0.253366},
        {-0.666684, 1.616481, 0.015769},
        {0.017640, -0.042771, 0.942103},
    };
    static constexpr double xyzToRec709[3][3] = {
        {3.240970, -1.537383, -0.498611},
        {-0.969244, 1.875968, 0.041555},
        {0.055630, -0.203977, 1.056972},
    };
    const RgbColor xyz = Multiply(Multiply(ap1, ap1ToXyzD60), d60ToD65);
    return Multiply(xyz, gamut == "rec2020" ? xyzToRec2020 : xyzToRec709);
}

}  // namespace

RgbColor MapLinearOutputToDisplay(const ColorManagementSettings& settings,
                                  RgbColor linearOutput) {
    const bool hlg = settings.output_transfer == "hlg";
    const double maximum = hlg ? 1.0 / HlgSceneReflectionScale() : 1.0;
    const double knee = hlg ? 0.9 : 0.72;
    const RgbColor weights = settings.output_gamut == "rec2020"
                                 ? RgbColor{0.2627, 0.6780, 0.0593}
                                 : RgbColor{0.2126, 0.7152, 0.0722};
    const double luminance = linearOutput.red * weights.red +
                             linearOutput.green * weights.green +
                             linearOutput.blue * weights.blue;
    if (luminance > knee) {
        const double shoulder =
            maximum -
            (maximum - knee) * std::exp(-(luminance - knee) / (maximum - knee));
        const double scale = shoulder / luminance;
        linearOutput.red *= scale;
        linearOutput.green *= scale;
        linearOutput.blue *= scale;
    }

    // Compress chroma around luminance instead of clipping channels
    // independently. The ratio describes how far a colour is from neutral as
    // a fraction of the available RGB cube in that direction. A smooth knee
    // begins before the boundary so saturated highlights retain hue detail.
    const double mappedLuminance = linearOutput.red * weights.red +
                                   linearOutput.green * weights.green +
                                   linearOutput.blue * weights.blue;
    const double positiveRoom = std::max(maximum - mappedLuminance, 1e-9);
    const double negativeRoom = std::max(mappedLuminance, 1e-9);
    const auto excursion = [&](double channel) {
        const double chroma = channel - mappedLuminance;
        return chroma >= 0.0 ? chroma / positiveRoom : -chroma / negativeRoom;
    };
    const double ratio =
        std::max({excursion(linearOutput.red), excursion(linearOutput.green),
                  excursion(linearOutput.blue)});
    constexpr double kChromaKnee = 0.75;
    constexpr double kChromaLimit = 0.98;
    if (ratio > kChromaKnee) {
        const double compressed =
            kChromaKnee + (kChromaLimit - kChromaKnee) *
                              (1.0 - std::exp(-(ratio - kChromaKnee) /
                                              (kChromaLimit - kChromaKnee)));
        const double scale = compressed / ratio;
        linearOutput.red =
            mappedLuminance + (linearOutput.red - mappedLuminance) * scale;
        linearOutput.green =
            mappedLuminance + (linearOutput.green - mappedLuminance) * scale;
        linearOutput.blue =
            mappedLuminance + (linearOutput.blue - mappedLuminance) * scale;
    }
    return {std::clamp(linearOutput.red, 0.0, maximum),
            std::clamp(linearOutput.green, 0.0, maximum),
            std::clamp(linearOutput.blue, 0.0, maximum)};
}

RgbColor TransformColorForOutput(const ColorManagementSettings& settings,
                                 RgbColor signal) {
    if (!settings.enabled)
        return {std::clamp(signal.red, 0.0, 1.0),
                std::clamp(signal.green, 0.0, 1.0),
                std::clamp(signal.blue, 0.0, 1.0)};
    RgbColor output =
        Ap1ToOutput(SourceToAp1(DecodeTransfer(signal, settings.input_transfer),
                                settings.input_gamut),
                    settings.output_gamut);
    output = MapLinearOutputToDisplay(settings, output);
    if (settings.output_transfer == "hlg") {
        const double scale = HlgSceneReflectionScale();
        return {
            std::clamp(EncodeHlg(std::max(0.0, output.red) * scale), 0.0, 1.0),
            std::clamp(EncodeHlg(std::max(0.0, output.green) * scale), 0.0,
                       1.0),
            std::clamp(EncodeHlg(std::max(0.0, output.blue) * scale), 0.0,
                       1.0)};
    }
    return {EncodeRec709(output.red), EncodeRec709(output.green),
            EncodeRec709(output.blue)};
}

ColorManagementSettings ColorManagementForSdrPreview(
    const ColorManagementSettings& settings) {
    ColorManagementSettings preview = settings;
    preview.output_gamut = "rec709";
    preview.output_transfer = "rec709";
    return preview;
}

namespace {

const char* OpenColorIoSourceSpace(const ColorManagementSettings& settings) {
    if (settings.input_transfer == "sony_slog3" &&
        settings.input_gamut == "sony_sgamut3_cine")
        return "S-Log3 S-Gamut3.Cine";
    if (settings.input_transfer == "sony_slog3" &&
        settings.input_gamut == "sony_sgamut3")
        return "S-Log3 S-Gamut3";
    if (settings.input_transfer == "rec709" && settings.input_gamut == "rec709")
        return "Camera Rec.709";
    if (settings.input_transfer == "linear" && settings.input_gamut == "rec709")
        return "Linear Rec.709 (sRGB)";
    if (settings.input_transfer == "linear" &&
        settings.input_gamut == "rec2020")
        return "Linear Rec.2020";
    throw std::runtime_error(
        "unsupported OCIO input gamut/transfer combination");
}

const char* OpenColorIoWorkingSpace(const ColorManagementSettings& settings) {
    if (settings.working_gamut == "acescct") return "ACEScg";
    if (settings.working_gamut == "rec709") return "Linear Rec.709 (sRGB)";
    if (settings.working_gamut == "rec2020") return "Linear Rec.2020";
    throw std::runtime_error("unsupported OCIO working gamut");
}

struct OpenColorIoDisplayView {
    const char* display;
    const char* view;
};

// An SDR delivery's display and view in the studio config. An HLG delivery
// has none: it is DisplayTransform's own chain (BH3).
OpenColorIoDisplayView ResolveOpenColorIoDisplayView(
    const ColorManagementSettings& settings) {
    if (settings.output_gamut == "rec2020")
        return {"Rec.1886 Rec.2020 - Display", "ACES 1.0 - SDR Video"};
    return {"Rec.1886 Rec.709 - Display", "ACES 1.0 - SDR Video"};
}

OCIO::ConstConfigRcPtr OpenColorIoConfig() {
    static const OCIO::ConstConfigRcPtr config =
        OCIO::Config::CreateFromBuiltinConfig(kOpenColorIoConfig);
    return config;
}

// The float a half-float's bits encode: the input of entry `bits` of a
// half-domain Lut1DTransform.
float HalfBitsToFloat(uint16_t bits) {
    const uint32_t exponent = (bits >> 10) & 0x1fu;
    const uint32_t mantissa = bits & 0x3ffu;
    float magnitude = 0.0f;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        magnitude = mantissa != 0 ? std::nanf("") : HUGE_VALF;
    } else {
        magnitude = std::ldexp(static_cast<float>(mantissa | 0x400u),
                               static_cast<int>(exponent) - 25);
    }
    return (bits & 0x8000u) != 0 ? -magnitude : magnitude;
}

// A per-channel function as a half-domain Lut1DTransform: one entry for
// every half float, so a linear input of any size reads its own value.
// Infinities read the largest finite half; not-a-number reads zero.
OCIO::Lut1DTransformRcPtr HalfDomainLut(double (*function)(double)) {
    constexpr unsigned long kLength = 65536;
    constexpr float kLargestHalf = 65504.0f;
    OCIO::Lut1DTransformRcPtr lut = OCIO::Lut1DTransform::Create(kLength, true);
    for (unsigned long index = 0; index < kLength; ++index) {
        float input = HalfBitsToFloat(static_cast<uint16_t>(index));
        if (std::isnan(input)) {
            input = 0.0f;
        }
        input = std::clamp(input, -kLargestHalf, kLargestHalf);
        const float value = static_cast<float>(function(input));
        lut->setValue(index, value, value, value);
    }
    return lut;
}

OCIO::MatrixTransformRcPtr MatrixTransformOf(const ColorMatrix3& matrix) {
    const double m44[16] = {matrix[0][0], matrix[0][1], matrix[0][2], 0.0,
                            matrix[1][0], matrix[1][1], matrix[1][2], 0.0,
                            matrix[2][0], matrix[2][1], matrix[2][2], 0.0,
                            0.0,          0.0,          0.0,          1.0};
    OCIO::MatrixTransformRcPtr transform = OCIO::MatrixTransform::Create();
    transform->setMatrix(m44);
    return transform;
}

// BH3 -- docs/roadmap/BLANC-HLG-2026-10.md. The HLG delivery, from `source`
// to the delivered signal: HlgDeliverySignalOf (HlgDelivery.h) step for step,
// in place of the studio config's ACES 1.1 HDR view.
OCIO::GroupTransformRcPtr HlgDeliveryTransform(const char* source) {
    static const OCIO::Lut1DTransformRcPtr tone =
        HalfDomainLut(HlgDeliveryTone);
    static const OCIO::Lut1DTransformRcPtr encode =
        HalfDomainLut(HlgDeliveryEncode);
    OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
    OCIO::ColorSpaceTransformRcPtr toRec2020 =
        OCIO::ColorSpaceTransform::Create();
    toRec2020->setSrc(source);
    toRec2020->setDst("Linear Rec.2020");
    group->appendTransform(toRec2020);
    group->appendTransform(MatrixTransformOf(Rec2020ToDaVinciWideGamut()));
    group->appendTransform(tone->createEditableCopy());
    group->appendTransform(MatrixTransformOf(DaVinciWideGamutToRec2020()));
    group->appendTransform(encode->createEditableCopy());
    return group;
}

// The display side of every managed path, from `source`: the SDR view of the
// studio config, or the HLG delivery.
OCIO::TransformRcPtr DisplayTransform(const ColorManagementSettings& settings,
                                      const char* source) {
    if (settings.output_transfer == "hlg") {
        if (settings.output_gamut != "rec2020") {
            throw std::runtime_error("HLG output requires Rec.2020");
        }
        return HlgDeliveryTransform(source);
    }
    const OpenColorIoDisplayView view = ResolveOpenColorIoDisplayView(settings);
    OCIO::DisplayViewTransformRcPtr display =
        OCIO::DisplayViewTransform::Create();
    display->setSrc(source);
    display->setDisplay(view.display);
    display->setView(view.view);
    return display;
}

OCIO::Lut1DTransformRcPtr Rec709DecodeTransform() {
    constexpr unsigned long kLength = 4096;
    OCIO::Lut1DTransformRcPtr transform =
        OCIO::Lut1DTransform::Create(kLength, false);
    transform->setInterpolation(OCIO::INTERP_LINEAR);
    for (unsigned long index = 0; index < kLength; ++index) {
        const float signal = static_cast<float>(index) / (kLength - 1);
        const float linear = static_cast<float>(DecodeRec709(signal));
        transform->setValue(index, linear, linear, linear);
    }
    return transform;
}

// Rec.2020 sources the studio config has no camera space for: decoded by a
// 1D transfer LUT, then named as Linear Rec.2020.
bool NeedsRec2020Rec709InputTransform(const ColorManagementSettings& settings) {
    return settings.input_gamut == "rec2020" &&
           (settings.input_transfer == "rec709" ||
            settings.input_transfer == "hlg");
}

// ITW5 -- ROADMAP.md. The ITW-2026-09 rushes were tagged BT.2020 HLG
// (arib-std-b67) and the pipeline had no such input: they were approximated as
// Rec.2020 primaries with a Rec.709 curve.
OCIO::Lut1DTransformRcPtr Rec2020DecodeTransform(
    const ColorManagementSettings& settings) {
    if (settings.input_transfer != "hlg") return Rec709DecodeTransform();
    constexpr unsigned long kLength = 4096;
    OCIO::Lut1DTransformRcPtr transform =
        OCIO::Lut1DTransform::Create(kLength, false);
    transform->setInterpolation(OCIO::INTERP_LINEAR);
    const double scale = HlgSceneReflectionScale();
    for (unsigned long index = 0; index < kLength; ++index) {
        const double signal = static_cast<double>(index) / (kLength - 1);
        const float linear = static_cast<float>(DecodeHlg(signal) / scale);
        transform->setValue(index, linear, linear, linear);
    }
    return transform;
}

OCIO::ConstProcessorRcPtr OpenColorIoInputProcessor(
    const ColorManagementSettings& settings) {
    const OCIO::ConstConfigRcPtr config = OpenColorIoConfig();
    if (!NeedsRec2020Rec709InputTransform(settings)) {
        return config->getProcessor(OpenColorIoSourceSpace(settings),
                                    OpenColorIoWorkingSpace(settings));
    }
    // COLOR-2026-08 -- the ACES studio config has linear Rec.2020 but no
    // camera Rec.2020 alias. Preserve the legacy document combination by
    // decoding its Rec.709 transfer before the named gamut conversion.
    OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
    group->appendTransform(Rec2020DecodeTransform(settings));
    OCIO::ColorSpaceTransformRcPtr gamut = OCIO::ColorSpaceTransform::Create();
    gamut->setSrc("Linear Rec.2020");
    gamut->setDst(OpenColorIoWorkingSpace(settings));
    group->appendTransform(gamut);
    return config->getProcessor(group);
}

void SampleOpenColorIoProcessor(const OCIO::ConstProcessorRcPtr& processor,
                                OpenColorIoLut3D& output) {
    output.edge = kOpenColorIoLutEdge;
    const size_t texelCount =
        static_cast<size_t>(output.edge) * output.edge * output.edge;
    output.rgba.resize(texelCount * 4);
    const OCIO::ConstCPUProcessorRcPtr cpu =
        processor->getDefaultCPUProcessor();
    for (int32_t blue = 0; blue < output.edge; ++blue) {
        for (int32_t green = 0; green < output.edge; ++green) {
            for (int32_t red = 0; red < output.edge; ++red) {
                float pixel[4] = {static_cast<float>(red) / (output.edge - 1),
                                  static_cast<float>(green) / (output.edge - 1),
                                  static_cast<float>(blue) / (output.edge - 1),
                                  1.0f};
                cpu->applyRGBA(pixel);
                const size_t offset =
                    (static_cast<size_t>(blue * output.edge * output.edge +
                                         green * output.edge + red)) *
                    4;
                std::copy(pixel, pixel + 4, output.rgba.begin() + offset);
            }
        }
    }
}

OCIO::ConstProcessorRcPtr OpenColorIoOutputProcessor(
    const ColorManagementSettings& settings) {
    const char* lutInputSpace = settings.working_gamut == "acescct"
                                    ? "ACEScct"
                                    : OpenColorIoWorkingSpace(settings);
    return OpenColorIoConfig()->getProcessor(
        DisplayTransform(settings, lutInputSpace));
}

OCIO::ConstProcessorRcPtr OpenColorIoCombinedProcessor(
    const ColorManagementSettings& settings) {
    if (NeedsRec2020Rec709InputTransform(settings)) {
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        group->appendTransform(Rec2020DecodeTransform(settings));
        group->appendTransform(DisplayTransform(settings, "Linear Rec.2020"));
        return OpenColorIoConfig()->getProcessor(group);
    }
    return OpenColorIoConfig()->getProcessor(
        DisplayTransform(settings, OpenColorIoSourceSpace(settings)));
}

}  // namespace

bool BuildOpenColorIoLuts(const ColorManagementSettings& settings,
                          OpenColorIoLutPair& output, std::string& error) {
    output = {};
    error.clear();
    try {
        SampleOpenColorIoProcessor(OpenColorIoInputProcessor(settings),
                                   output.input_to_working);
        SampleOpenColorIoProcessor(OpenColorIoOutputProcessor(settings),
                                   output.working_to_display);
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    output = {};
    return false;
}

bool BuildOpenColorIoInputLut(const ColorManagementSettings& settings,
                              OpenColorIoLut3D& output, std::string& error) {
    output = {};
    error.clear();
    try {
        SampleOpenColorIoProcessor(OpenColorIoInputProcessor(settings), output);
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    output = {};
    return false;
}

namespace {

std::string CubeText(const OpenColorIoLut3D& lut, const char* title) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "TITLE \"" << title << "\"\n"
           << "LUT_3D_SIZE " << lut.edge << '\n'
           << "DOMAIN_MIN 0.0 0.0 0.0\n"
           << "DOMAIN_MAX 1.0 1.0 1.0\n"
           << std::fixed << std::setprecision(10);
    for (size_t offset = 0; offset < lut.rgba.size(); offset += 4)
        stream << lut.rgba[offset] << ' ' << lut.rgba[offset + 1] << ' '
               << lut.rgba[offset + 2] << '\n';
    return stream.str();
}

}  // namespace

namespace {

// F1.3 -- linear composite. The two per-colour paths below are the one
// definition of what a managed clip goes through: the export's cubes sample
// them and the tests evaluate them, so the two cannot drift apart.
struct WorkingPath {
    OCIO::ConstCPUProcessorRcPtr input;
    bool acescct = false;
};

WorkingPath MakeWorkingPath(const ColorManagementSettings& settings) {
    return {OpenColorIoInputProcessor(settings)->getDefaultCPUProcessor(),
            settings.working_gamut == "acescct"};
}

// Same order as fragment_working: input to the linear working space, the
// grade in ACEScct (or on the linear value for another working space), and
// back to the linear value the working texture stores and blends.
std::array<float, 3> ToWorking(const WorkingPath& path,
                               const ResolvedColorGrade& grade,
                               const std::array<float, 3>& signal) {
    float pixel[4] = {signal[0], signal[1], signal[2], 1.0f};
    path.input->applyRGBA(pixel);
    std::array<float, 3> color{pixel[0], pixel[1], pixel[2]};
    if (grade.count == 0) {
        return color;
    }
    if (!path.acescct) {
        return ApplyResolvedColorGrade(color, grade);
    }
    for (float& channel : color) {
        channel = static_cast<float>(EncodeAcesCct(channel));
    }
    color = ApplyResolvedColorGrade(color, grade);
    for (float& channel : color) {
        channel = static_cast<float>(DecodeAcesCct(channel));
    }
    return color;
}

struct DisplayPath {
    OCIO::ConstCPUProcessorRcPtr display;
    bool acescct = false;
};

DisplayPath MakeDisplayPath(const ColorManagementSettings& settings) {
    return {OpenColorIoOutputProcessor(settings)->getDefaultCPUProcessor(),
            settings.working_gamut == "acescct"};
}

// Same as displaySignal in shader.metal: ACEScct when grading in ACEScct,
// coordinates clamped to the LUT's 0..1 domain, then the display transform.
// Split in two because a look declared on the working log values is laid
// between the halves (G1 -- docs/roadmap/GRADE-2026-09.md).
std::array<float, 3> ToDisplayCoordinates(const DisplayPath& path,
                                          const std::array<float, 3>& working) {
    std::array<float, 3> coordinates{};
    for (int channel = 0; channel < 3; ++channel) {
        const double value =
            path.acescct ? EncodeAcesCct(working[channel]) : working[channel];
        coordinates[channel] = static_cast<float>(std::clamp(value, 0.0, 1.0));
    }
    return coordinates;
}

std::array<float, 3> FromDisplayCoordinates(
    const DisplayPath& path, const std::array<float, 3>& coordinates) {
    float pixel[4] = {coordinates[0], coordinates[1], coordinates[2], 1.0f};
    path.display->applyRGBA(pixel);
    return {std::clamp(pixel[0], 0.0f, 1.0f), std::clamp(pixel[1], 0.0f, 1.0f),
            std::clamp(pixel[2], 0.0f, 1.0f)};
}

std::array<float, 3> ToDisplay(const DisplayPath& path,
                               const std::array<float, 3>& working) {
    return FromDisplayCoordinates(path, ToDisplayCoordinates(path, working));
}

bool IsFinite(const std::array<float, 3>& color) {
    return std::isfinite(color[0]) && std::isfinite(color[1]) &&
           std::isfinite(color[2]);
}

// FFmpeg's lut1d divides its input by DOMAIN_MAX but never subtracts
// DOMAIN_MIN (measured on FFmpeg 8.1: a negative minimum shifted every
// lookup by that amount), so the shaper starts at linear zero. The monitor
// still separates ACEScct 0 to 0.073 (linear -0.0069 to 0); here those
// negative values read as zero, and the ACES display transforms render all
// of them as black. The upper bound is ACEScct 1.0, where the monitor's LUT
// coordinates clamp. Uniform samples of a logarithm are coarsest just above
// the ACEScct toe (linear 0.0078): measured there, linear interpolation
// between these samples stays within 1.3e-3 of ACEScct.
std::string ShaperText() {
    const double maximum = DecodeAcesCct(1.0);
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "TITLE \"CUTMACHINE scene-linear to ACEScct shaper\"\n"
           << "LUT_1D_SIZE " << kOpenColorIoShaperSize << '\n'
           << std::fixed << std::setprecision(10) << "DOMAIN_MIN 0.0 0.0 0.0\n"
           << "DOMAIN_MAX " << maximum << ' ' << maximum << ' ' << maximum
           << '\n';
    for (int32_t index = 0; index < kOpenColorIoShaperSize; ++index) {
        const double value = EncodeAcesCct(
            maximum * index / static_cast<double>(kOpenColorIoShaperSize - 1));
        stream << value << ' ' << value << ' ' << value << '\n';
    }
    return stream.str();
}

}  // namespace

bool OpenColorIoWorkingColor(const ColorManagementSettings& settings,
                             const ResolvedColorGrade& grade, RgbColor signal,
                             RgbColor& working, std::string& error) {
    error.clear();
    try {
        const std::array<float, 3> color = ToWorking(
            MakeWorkingPath(settings), grade,
            {static_cast<float>(signal.red), static_cast<float>(signal.green),
             static_cast<float>(signal.blue)});
        if (!IsFinite(color)) {
            error = "graded colour path produces a non-finite colour";
            return false;
        }
        working = {color[0], color[1], color[2]};
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return false;
}

bool OpenColorIoDisplayColor(const ColorManagementSettings& settings,
                             RgbColor working, RgbColor& display,
                             std::string& error) {
    error.clear();
    try {
        const std::array<float, 3> color = ToDisplay(
            MakeDisplayPath(settings),
            {static_cast<float>(working.red), static_cast<float>(working.green),
             static_cast<float>(working.blue)});
        display = {color[0], color[1], color[2]};
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return false;
}

struct OpenColorIoColorPathState {
    WorkingPath working;
    DisplayPath display;
};

bool MakeOpenColorIoColorPath(const ColorManagementSettings& settings,
                              OpenColorIoColorPath& path, std::string& error) {
    error.clear();
    try {
        path.state = std::make_shared<const OpenColorIoColorPathState>(
            OpenColorIoColorPathState{MakeWorkingPath(settings),
                                      MakeDisplayPath(settings)});
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    path.state.reset();
    return false;
}

std::array<float, 3> OpenColorIoPathWorking(
    const OpenColorIoColorPath& path, const ResolvedColorGrade& grade,
    const std::array<float, 3>& signal) {
    return ToWorking(path.state->working, grade, signal);
}

std::array<float, 3> OpenColorIoPathCoordinates(
    const OpenColorIoColorPath& path, const std::array<float, 3>& working) {
    return ToDisplayCoordinates(path.state->display, working);
}

std::array<float, 3> OpenColorIoPathDisplay(
    const OpenColorIoColorPath& path, const std::array<float, 3>& coordinates) {
    return FromDisplayCoordinates(path.state->display, coordinates);
}

bool BuildOpenColorIoWorkingCube(const ColorManagementSettings& settings,
                                 const ResolvedColorGrade& grade,
                                 std::string& output, std::string& error) {
    output.clear();
    error.clear();
    try {
        const WorkingPath path = MakeWorkingPath(settings);
        OpenColorIoLut3D lut;
        lut.edge = kOpenColorIoLutEdge;
        lut.rgba.resize(static_cast<size_t>(lut.edge) * lut.edge * lut.edge *
                        4);
        const float step = 1.0f / static_cast<float>(lut.edge - 1);
        size_t offset = 0;
        for (int32_t blue = 0; blue < lut.edge; ++blue) {
            for (int32_t green = 0; green < lut.edge; ++green) {
                for (int32_t red = 0; red < lut.edge; ++red) {
                    const std::array<float, 3> color =
                        ToWorking(path, grade,
                                  {static_cast<float>(red) * step,
                                   static_cast<float>(green) * step,
                                   static_cast<float>(blue) * step});
                    if (!IsFinite(color)) {
                        error =
                            "graded colour path produces a non-finite colour";
                        return false;
                    }
                    std::copy(
                        color.begin(), color.end(),
                        lut.rgba.begin() + static_cast<std::ptrdiff_t>(offset));
                    lut.rgba[offset + 3] = 1.0f;
                    offset += 4;
                }
            }
        }
        output = CubeText(lut, "CUTMACHINE OpenColorIO working-space clip");
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    output.clear();
    return false;
}

namespace {

// MOTION-2026-09 M4 -- display-referred signal to the working space: the
// inverse of the SDR display view, from its display to the linear working
// space the other clips reach (ColorManagement.h says why). BH1 -- for an
// HLG delivery, the inverse of the HLG delivery (BH3), which
// DisplayReferredSignal feeds with the graphic's HLG signal.
OCIO::ConstCPUProcessorRcPtr DisplayReferredInputProcessor(
    const ColorManagementSettings& settings) {
    const OCIO::TransformRcPtr transform = DisplayTransform(
        GraphicsPlacedInHlg(settings) ? settings
                                      : ColorManagementForSdrPreview(settings),
        OpenColorIoWorkingSpace(settings));
    transform->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
    return OpenColorIoConfig()
        ->getProcessor(transform)
        ->getDefaultCPUProcessor();
}

// BH1 -- what the inverse view reads for a graphic's code value: the value
// itself through the SDR view, its HLG signal for an HLG delivery.
void DisplayReferredSignal(const ColorManagementSettings& settings,
                           float pixel[4]) {
    if (!GraphicsPlacedInHlg(settings)) {
        return;
    }
    const RgbColor signal =
        GraphicsCodeToHlgSignal({pixel[0], pixel[1], pixel[2]});
    pixel[0] = static_cast<float>(signal.red);
    pixel[1] = static_cast<float>(signal.green);
    pixel[2] = static_cast<float>(signal.blue);
}

bool SampleDisplayReferred(const ColorManagementSettings& settings,
                           OpenColorIoLut3D& lut, std::string& error) {
    const OCIO::ConstCPUProcessorRcPtr processor =
        DisplayReferredInputProcessor(settings);
    lut.edge = kOpenColorIoLutEdge;
    lut.rgba.assign(static_cast<size_t>(lut.edge) * lut.edge * lut.edge * 4,
                    0.0f);
    const float step = 1.0f / static_cast<float>(lut.edge - 1);
    size_t offset = 0;
    for (int32_t blue = 0; blue < lut.edge; ++blue) {
        for (int32_t green = 0; green < lut.edge; ++green) {
            for (int32_t red = 0; red < lut.edge; ++red) {
                float pixel[4] = {static_cast<float>(red) * step,
                                  static_cast<float>(green) * step,
                                  static_cast<float>(blue) * step, 1.0f};
                DisplayReferredSignal(settings, pixel);
                processor->applyRGBA(pixel);
                if (!IsFinite({pixel[0], pixel[1], pixel[2]})) {
                    error =
                        "inverse display transform produces a non-finite "
                        "colour";
                    return false;
                }
                std::copy(
                    pixel, pixel + 3,
                    lut.rgba.begin() + static_cast<std::ptrdiff_t>(offset));
                lut.rgba[offset + 3] = 1.0f;
                offset += 4;
            }
        }
    }
    return true;
}

}  // namespace

bool OpenColorIoDisplayReferredWorkingColor(
    const ColorManagementSettings& settings, RgbColor signal, RgbColor& working,
    std::string& error) {
    error.clear();
    try {
        float pixel[4] = {static_cast<float>(signal.red),
                          static_cast<float>(signal.green),
                          static_cast<float>(signal.blue), 1.0f};
        DisplayReferredSignal(settings, pixel);
        DisplayReferredInputProcessor(settings)->applyRGBA(pixel);
        if (!IsFinite({pixel[0], pixel[1], pixel[2]})) {
            error = "inverse display transform produces a non-finite colour";
            return false;
        }
        working = {pixel[0], pixel[1], pixel[2]};
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return false;
}

bool OpenColorIoCompositeDisplayReferred(
    const ColorManagementSettings& settings, RgbColor below, RgbColor layer,
    double coverage, RgbColor& working, std::string& error) {
    error.clear();
    if (!(coverage > 0.0)) {
        working = below;
        return true;
    }
    const double amount = std::min(coverage, 1.0);
    RgbColor shown;
    const bool hlg = GraphicsPlacedInHlg(settings);
    if (!OpenColorIoDisplayColor(
            hlg ? settings : ColorManagementForSdrPreview(settings), below,
            shown, error)) {
        return false;
    }
    // What the view shows: a highlight above white is seen as white, and the
    // layer mixes with that.
    shown = {std::clamp(shown.red, 0.0, 1.0), std::clamp(shown.green, 0.0, 1.0),
             std::clamp(shown.blue, 0.0, 1.0)};
    // BH1 -- in an HLG delivery the layer is mixed in its own code values.
    if (hlg) {
        shown = HlgSignalToGraphicsCode(shown);
    }
    const RgbColor mixed{shown.red + ((layer.red - shown.red) * amount),
                         shown.green + ((layer.green - shown.green) * amount),
                         shown.blue + ((layer.blue - shown.blue) * amount)};
    return OpenColorIoDisplayReferredWorkingColor(settings, mixed, working,
                                                  error);
}

bool BuildOpenColorIoDisplayReferredWorkingLut(
    const ColorManagementSettings& settings, OpenColorIoLut3D& output,
    std::string& error) {
    output = {};
    error.clear();
    try {
        if (SampleDisplayReferred(settings, output, error)) {
            return true;
        }
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    output = {};
    return false;
}

bool BuildOpenColorIoDisplayReferredWorkingCube(
    const ColorManagementSettings& settings, std::string& output,
    std::string& error) {
    output.clear();
    OpenColorIoLut3D lut;
    if (!BuildOpenColorIoDisplayReferredWorkingLut(settings, lut, error)) {
        return false;
    }
    output = CubeText(lut, "CUTMACHINE OpenColorIO display-referred clip");
    return true;
}

namespace {

// BH1 -- an HLG view's samples read back as graphic code values.
void MapHlgSamplesToGraphicsCode(OpenColorIoLut3D& lut) {
    for (size_t offset = 0; offset + 2 < lut.rgba.size(); offset += 4) {
        const RgbColor code = HlgSignalToGraphicsCode(
            {lut.rgba[offset], lut.rgba[offset + 1], lut.rgba[offset + 2]});
        lut.rgba[offset] = static_cast<float>(code.red);
        lut.rgba[offset + 1] = static_cast<float>(code.green);
        lut.rgba[offset + 2] = static_cast<float>(code.blue);
    }
}

bool BuildDisplayCubes(const ColorManagementSettings& settings,
                       bool graphicsCode, std::string& shaper,
                       std::string& cube, std::string& error) {
    shaper.clear();
    cube.clear();
    error.clear();
    try {
        OpenColorIoLut3D lut;
        SampleOpenColorIoProcessor(OpenColorIoOutputProcessor(settings), lut);
        for (size_t offset = 0; offset < lut.rgba.size(); offset += 4) {
            for (size_t channel = 0; channel < 3; ++channel) {
                float& value = lut.rgba[offset + channel];
                if (!std::isfinite(value)) {
                    error = "display transform produces a non-finite colour";
                    return false;
                }
                value = std::clamp(value, 0.0f, 1.0f);
            }
        }
        if (graphicsCode) {
            MapHlgSamplesToGraphicsCode(lut);
        }
        cube = CubeText(lut, graphicsCode
                                 ? "CUTMACHINE OpenColorIO graphics view"
                                 : "CUTMACHINE OpenColorIO display transform");
        if (settings.working_gamut == "acescct") {
            shaper = ShaperText();
        }
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    shaper.clear();
    cube.clear();
    return false;
}

}  // namespace

bool BuildOpenColorIoDisplayCubes(const ColorManagementSettings& settings,
                                  std::string& shaper, std::string& cube,
                                  std::string& error) {
    return BuildDisplayCubes(settings, false, shaper, cube, error);
}

bool BuildOpenColorIoGraphicsViewCubes(const ColorManagementSettings& settings,
                                       std::string& shaper, std::string& cube,
                                       std::string& error) {
    if (GraphicsPlacedInHlg(settings)) {
        return BuildDisplayCubes(settings, true, shaper, cube, error);
    }
    return BuildDisplayCubes(ColorManagementForSdrPreview(settings), false,
                             shaper, cube, error);
}

bool BuildOpenColorIoGraphicsViewLuts(const ColorManagementSettings& settings,
                                      OpenColorIoLutPair& output,
                                      std::string& error) {
    if (!GraphicsPlacedInHlg(settings)) {
        return BuildOpenColorIoLuts(ColorManagementForSdrPreview(settings),
                                    output, error);
    }
    if (!BuildOpenColorIoLuts(settings, output, error)) {
        return false;
    }
    for (size_t offset = 0; offset + 2 < output.working_to_display.rgba.size();
         offset += 4) {
        for (size_t channel = 0; channel < 3; ++channel) {
            float& value = output.working_to_display.rgba[offset + channel];
            value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        }
    }
    MapHlgSamplesToGraphicsCode(output.working_to_display);
    return true;
}

std::string ColorInputScaleOptions(const ColorManagementSettings& settings) {
    std::string options;
    if (settings.input_range != "auto") {
        options += std::string("in_range=") +
                   (settings.input_range == "full" ? "full" : "tv") + ':';
    }
    if (settings.input_ycbcr_matrix != "auto") {
        options +=
            std::string("in_color_matrix=") +
            (settings.input_ycbcr_matrix == "bt2020_ncl" ? "bt2020" : "bt709") +
            ':';
    }
    return options;
}

ColorManagementSettings ResolveUntaggedColorInput(
    const ColorManagementSettings& settings, const std::string& colorRange,
    const std::string& colorSpace) {
    // av_color_range_name and av_color_space_name, which ingest stores, say
    // "unknown" for an unspecified tag. An empty one is a document that
    // never recorded the tag, not a file without one: it is left to the
    // graph, which reads the file's own.
    const auto untagged = [](const std::string& tag) {
        return tag == "unknown" || tag == "reserved";
    };
    ColorManagementSettings resolved = settings;
    if (settings.input_range == "auto" && untagged(colorRange)) {
        resolved.input_range =
            ResolveYuvFullRange(settings, YuvRangeTag::Unspecified, false)
                ? "full"
                : "limited";
    }
    if (settings.input_ycbcr_matrix == "auto" && untagged(colorSpace)) {
        resolved.input_ycbcr_matrix =
            ResolveYuvBt2020Matrix(settings, false) ? "bt2020_ncl" : "bt709";
    }
    return resolved;
}

std::string ColorLutFilter(const ColorManagementSettings& settings,
                           const std::string& lutPath,
                           const std::string& outputMatrix) {
    return "scale=" + ColorInputScaleOptions(settings) +
           "out_range=full,format=gbrp16le,lut3d=file='" + lutPath +
           "':interp=trilinear,scale=out_range=tv:out_color_matrix=" +
           outputMatrix;
}

bool BuildOpenColorIoCube(const ColorManagementSettings& settings,
                          std::string& output, std::string& error) {
    output.clear();
    error.clear();
    try {
        OpenColorIoLut3D lut;
        SampleOpenColorIoProcessor(OpenColorIoCombinedProcessor(settings), lut);
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << "TITLE \"CUTMACHINE OpenColorIO display transform\"\n"
               << "LUT_3D_SIZE " << lut.edge << '\n'
               << "DOMAIN_MIN 0.0 0.0 0.0\n"
               << "DOMAIN_MAX 1.0 1.0 1.0\n"
               << std::fixed << std::setprecision(10);
        for (size_t offset = 0; offset < lut.rgba.size(); offset += 4)
            stream << lut.rgba[offset] << ' ' << lut.rgba[offset + 1] << ' '
                   << lut.rgba[offset + 2] << '\n';
        output = stream.str();
        return true;
    } catch (const OCIO::Exception& exception) {
        error = exception.what();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return false;
}

std::string OpenColorIoCacheKey(const ColorManagementSettings& settings) {
    std::ostringstream output;
    output << kOpenColorIoConfig << '|' << settings.input_gamut << '|'
           << settings.input_transfer << '|' << settings.working_gamut << '|'
           << settings.output_gamut << '|' << settings.output_transfer;
    return output.str();
}
