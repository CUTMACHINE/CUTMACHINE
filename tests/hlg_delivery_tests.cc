// BH3 -- docs/roadmap/BLANC-HLG-2026-10.md. The HLG delivery held to what
// DaVinci Resolve Studio 21.1 rendered of an S-Log3 chart on 20 October 2026
// (the author's settings: colour management v2, DaVinci WG/Intermediate, Rec.2020
// HLG, HDR 1000), and the OpenColorIO chain the export and the graphics use
// held to its CPU reference, HlgDeliverySignalOf.
#include "ColorEffects.h"
#include "ColorManagement.h"
#include "Document.h"
#include "HlgDelivery.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Describe(const RgbColor& color) {
    return "(" + std::to_string(color.red) + ", " +
           std::to_string(color.green) + ", " + std::to_string(color.blue) +
           ")";
}

double Worst(const RgbColor& a, const RgbColor& b) {
    return std::max({std::abs(a.red - b.red), std::abs(a.green - b.green),
                     std::abs(a.blue - b.blue)});
}

ColorManagementSettings HlgDelivery() {
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    settings.input_range = "full";
    settings.working_gamut = "acescct";
    settings.output_gamut = "rec2020";
    settings.output_transfer = "hlg";
    return settings;
}

// The chart's colour patches: linear Rec.2020 (from their S-Gamut3.Cine
// values) and Resolve's rendered R'G'B'.
struct Patch {
    const char* name;
    RgbColor rec2020;
    RgbColor resolve;
};
constexpr Patch kPatches[] = {
    {"rouge 0.18", {0.174953, 0.026923, 0.016368}, {0.3995, 0.1088, 0.0743}},
    {"rouge 0.72", {0.699813, 0.107694, 0.065471}, {0.6899, 0.2640, 0.1962}},
    {"rouge 2.88", {2.799251, 0.430776, 0.261884}, {0.8907, 0.5458, 0.4376}},
    {"vert 0.18", {0.053601, 0.218469, 0.026751}, {0.1687, 0.4184, 0.1063}},
    {"vert 0.72", {0.214404, 0.873877, 0.107005}, {0.3656, 0.7063, 0.2576}},
    {"vert 2.88", {0.857615, 3.495506, 0.428021}, {0.6542, 0.9028, 0.5488}},
    {"bleu 0.18", {0.015549, 0.005608, 0.193633}, {0.0821, 0.0358, 0.4734}},
    {"bleu 0.72", {0.062195, 0.022431, 0.774530}, {0.2175, 0.1419, 0.7500}},
    {"bleu 2.88", {0.248782, 0.089723, 3.098120}, {0.4894, 0.4264, 0.9332}},
    {"cyan 0.18", {0.038488, 0.153389, 0.185860}, {0.1388, 0.3464, 0.3907}},
    {"cyan 0.72", {0.153951, 0.613555, 0.743440}, {0.3139, 0.6373, 0.6804}},
    {"cyan 2.88", {0.615806, 2.454219, 2.973761}, {0.6105, 0.8504, 0.8812}},
    {"magenta 0.18", {0.155237, 0.001865, 0.169835}, {0.3762, 0.0142, 0.3955}},
    {"magenta 0.72", {0.620950, 0.007458, 0.679339}, {0.6671, 0.0922, 0.6885}},
    {"magenta 2.88", {2.483799, 0.029832, 2.717355}, {0.8743, 0.3480, 0.8929}},
    {"jaune 0.18", {0.190351, 0.189988, 0.013945}, {0.3875, 0.3843, 0.0650}},
    {"jaune 0.72", {0.761404, 0.759953, 0.055782}, {0.6743, 0.6740, 0.1804}},
    {"jaune 2.88", {3.045617, 3.039810, 0.223127}, {0.8747, 0.8791, 0.4376}},
    {"peau 0.18", {0.111482, 0.077172, 0.056864}, {0.2976, 0.2318, 0.1924}},
    {"peau 0.72", {0.445928, 0.308689, 0.227455}, {0.5813, 0.4804, 0.4085}},
    {"peau 2.88", {1.783711, 1.234756, 0.909821}, {0.8127, 0.7482, 0.6929}},
};

void CheckGreyCurve() {
    // Resolve put the chart's 18 % grey band (0.1734) at 0.3642 and the next
    // (0.2396) at 0.4290: 18 % itself sits between, where BT.2408 puts it.
    const double grey = HlgDeliverySignal(0.18);
    Check(grey > 0.368 && grey < 0.376,
          "18 % grey sits at Resolve's 37 % of the HLG signal, not the ACES "
          "view's 30 %: " +
              std::to_string(grey));
    Check(std::abs(HlgDeliverySignal(0.173424) - 0.36420) < 1e-9 &&
              std::abs(HlgDeliverySignal(1.229610) - 0.74856) < 1e-9 &&
              std::abs(HlgDeliverySignal(0.012759) - 0.06854) < 1e-9,
          "the curve passes through Resolve's measured greys");
    Check(HlgDeliverySignal(0.0) == 0.0 && HlgDeliverySignal(-1.0) == 0.0,
          "black and below deliver zero");
    Check(HlgDeliverySignal(31.93295) == 1.0 && HlgDeliverySignal(100.0) == 1.0,
          "the peak is reached at 32x white");
    // Strictly increasing from deep black to past the peak: the inverse a
    // graphic goes through (BH1) needs it.
    double previous = 0.0;
    bool increasing = true;
    bool encodes = true;
    for (int step = 0; step <= 2000; ++step) {
        const double reflectance = 1e-4 * std::pow(10.0, step * 5.6 / 2000.0);
        const double signal = HlgDeliverySignal(reflectance);
        if (reflectance < 31.93295 && !(signal > previous)) {
            increasing = false;
        }
        previous = signal;
        if (std::abs(HlgDeliveryEncode(HlgDeliveryTone(reflectance)) - signal) >
            1e-9) {
            encodes = false;
        }
    }
    Check(increasing, "the delivered signal rises with every reflectance");
    Check(encodes,
          "the tone curve encoded at BT.2408 gives the delivered signal");
    Check(std::abs(HlgDeliveryEncode(1.0) - 0.75) < 1e-12,
          "BT.2408: reflectance 1.0 is the HLG reference white, 75 %");
    Check(HlgDeliveryTone(-0.25) == -0.25,
          "a negative value passes the tone curve unchanged");
}

void CheckGamut() {
    const ColorMatrix3& in = Rec2020ToDaVinciWideGamut();
    const ColorMatrix3& out = DaVinciWideGamutToRec2020();
    double worst = 0.0;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += out[row][k] * in[k][column];
            }
            worst =
                std::max(worst, std::abs(sum - (row == column ? 1.0 : 0.0)));
        }
        const double white = in[row][0] + in[row][1] + in[row][2];
        Check(std::abs(white - 1.0) < 1e-9,
              "Rec.2020 and DaVinci Wide Gamut share their D65 white");
    }
    Check(worst < 1e-12, "the two gamut matrices invert each other");
    for (const double reflectance : {0.002, 0.18, 0.9, 4.0, 40.0}) {
        const RgbColor signal =
            HlgDeliverySignalOf({reflectance, reflectance, reflectance});
        Check(Worst(signal, {HlgDeliverySignal(reflectance),
                             HlgDeliverySignal(reflectance),
                             HlgDeliverySignal(reflectance)}) < 1e-9,
              "a neutral delivers the grey curve");
    }
}

void CheckColourPatches() {
    double worst = 0.0;
    double worstSkin = 0.0;
    double ordered = 0.0;
    for (const Patch& patch : kPatches) {
        const RgbColor delivered = HlgDeliverySignalOf(patch.rec2020);
        const double error = Worst(delivered, patch.resolve);
        worst = std::max(worst, error);
        if (std::string(patch.name).rfind("peau", 0) == 0) {
            worstSkin = std::max(worstSkin, error);
        }
        ordered += error;
    }
    std::cout << "colour patches against Resolve: worst " << worst << ", skin "
              << worstSkin << ", mean "
              << ordered / static_cast<double>(std::size(kPatches)) << '\n';
    Check(worstSkin < 0.015,
          "skin within 1.5 points of Resolve: " + std::to_string(worstSkin));
    Check(worst < 0.10, "every chart patch within 10 points of Resolve: " +
                            std::to_string(worst));
}

// The OpenColorIO chain the export samples (working value to the delivered
// signal) against the CPU reference, through the studio config's own
// ACEScg to linear Rec.2020.
void CheckOpenColorIoChain() {
    const ColorManagementSettings settings = HlgDelivery();
    const OCIO::ConstConfigRcPtr config =
        OCIO::Config::CreateFromBuiltinConfig(kOpenColorIoConfig);
    const OCIO::ConstCPUProcessorRcPtr toRec2020 =
        config->getProcessor("ACEScg", "Linear Rec.2020")
            ->getDefaultCPUProcessor();
    const RgbColor working[] = {
        {0.0, 0.0, 0.0},    {0.004, 0.004, 0.004}, {0.18, 0.18, 0.18},
        {0.9, 0.9, 0.9},    {6.0, 6.0, 6.0},       {40.0, 40.0, 40.0},
        {0.30, 0.20, 0.15}, {0.05, 0.02, 0.40},    {1.2, 0.1, 0.05},
        {0.02, 0.35, 0.03}, {2.0, 1.6, 1.1},       {0.5, 0.0, 0.5},
    };
    double worst = 0.0;
    for (const RgbColor& color : working) {
        float pixel[4] = {static_cast<float>(color.red),
                          static_cast<float>(color.green),
                          static_cast<float>(color.blue), 1.0f};
        toRec2020->applyRGBA(pixel);
        const RgbColor reference =
            HlgDeliverySignalOf({pixel[0], pixel[1], pixel[2]});
        RgbColor display;
        std::string error;
        Check(OpenColorIoDisplayColor(settings, color, display, error),
              "the HLG delivery evaluates: " + error);
        const double distance = Worst(display, reference);
        worst = std::max(worst, distance);
        if (distance > 2e-3) {
            Check(false, "OpenColorIO chain " + Describe(display) +
                             " against the reference " + Describe(reference) +
                             " for " + Describe(color));
        }
    }
    std::cout << "OpenColorIO chain against HlgDeliverySignalOf: worst "
              << worst << '\n';
    // End to end: the chart's 18 % grey S-Log3 code, decoded, read at
    // Resolve's level and not at the ACES view's 0.303.
    RgbColor greyWorking;
    RgbColor greyDisplay;
    std::string error;
    const double code = 420.0 / 1023.0;
    Check(
        OpenColorIoWorkingColor(settings, ResolvedColorGrade{},
                                {code, code, code}, greyWorking, error) &&
            OpenColorIoDisplayColor(settings, greyWorking, greyDisplay, error),
        "an S-Log3 grey goes through the delivery: " + error);
    Check(greyDisplay.green > 0.365 && greyDisplay.green < 0.380,
          "an S-Log3 18 % grey is delivered at about 37 % of the HLG signal: " +
              Describe(greyDisplay));
}

// BH1 still holds through the new delivery: a graphic's code value goes to
// the working space by the inverse and comes back as its HLG signal.
void CheckGraphicsRoundTrip() {
    const ColorManagementSettings settings = HlgDelivery();
    for (const RgbColor& code :
         {RgbColor{0.5, 0.5, 0.5},
          RgbColor{224.0 / 255.0, 112.0 / 255.0, 96.0 / 255.0},
          RgbColor{0.05, 0.05, 0.05}}) {
        RgbColor working;
        RgbColor shown;
        std::string error;
        Check(OpenColorIoDisplayReferredWorkingColor(settings, code, working,
                                                     error) &&
                  OpenColorIoDisplayColor(settings, working, shown, error),
              "the graphics path evaluates: " + error);
        const RgbColor signal = GraphicsCodeToHlgSignal(code);
        Check(Worst(shown, signal) < 4e-3,
              "a graphic comes back as the HLG signal of its code value: " +
                  Describe(shown) + " against " + Describe(signal));
    }
    RgbColor working;
    RgbColor white;
    std::string error;
    Check(OpenColorIoDisplayReferredWorkingColor(settings, {1.0, 1.0, 1.0},
                                                 working, error) &&
              OpenColorIoDisplayColor(settings, working, white, error) &&
              white.red > 0.99 && white.green > 0.99 && white.blue > 0.99,
          "a white title still reaches the peak: " + Describe(white));
}

}  // namespace

int main() {
    try {
        CheckGreyCurve();
        CheckGamut();
        CheckColourPatches();
        CheckOpenColorIoChain();
        CheckGraphicsRoundTrip();
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "hlg_delivery_tests passed\n";
    return EXIT_SUCCESS;
}
