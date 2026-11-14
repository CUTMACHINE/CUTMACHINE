#include "LookLut.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>

namespace {

// The .cube grammar this build reads, kept deliberately narrow: a keyword
// per line, then the triplets. Nothing here interprets a keyword it does
// not know -- an unknown one is refused, because a file carrying it is a
// file whose meaning this build does not have.
bool ReadFloats(std::istringstream& line, int count, double* values) {
    for (int index = 0; index < count; ++index) {
        if (!(line >> values[index])) return false;
        if (!std::isfinite(values[index])) return false;
    }
    std::string extra;
    return !(line >> extra);
}

}  // namespace

bool IsLookLutDigest(const std::string& digest) {
    if (digest.size() != 64) return false;
    return std::all_of(digest.begin(), digest.end(), [](unsigned char letter) {
        return (letter >= '0' && letter <= '9') ||
               (letter >= 'a' && letter <= 'f');
    });
}

std::string LookLutPackagePath(const std::string& digest) {
    return "Luts/" + digest + ".cube";
}

bool ResolveLookLutStage(const SequenceLut& lut,
                         const ColorManagementSettings& color,
                         LookLutStage& stage, std::string& error) {
    stage = LookLutStage::None;
    error.clear();
    if (lut.digest.empty()) return true;
    if (!IsLookLutDigest(lut.digest)) {
        error = "the look LUT's digest is not a SHA-256";
        return false;
    }
    if (!color.enabled) {
        if (!lut.space.empty()) {
            error = "the look LUT declares the space '" + lut.space +
                    "', which only exists under colour management: a "
                    "sequence without it applies the LUT to the delivered "
                    "signal, so leave the space out";
            return false;
        }
        stage = LookLutStage::Display;
        return true;
    }
    // Guard rail 4: under colour management the engine does not get to
    // decide what a LUT was computed on. Without a declaration there is no
    // honest stage to put it at, so it is refused rather than applied to
    // whichever value happens to be at hand.
    if (lut.space.empty()) {
        error =
            "the look LUT declares no space, and colour management is on: "
            "declare the space it expects ('" +
            std::string(kLookLutSpaceWorkingLog) + "' or '" +
            std::string(kLookLutSpaceDisplay) + "')";
        return false;
    }
    if (lut.space == kLookLutSpaceDisplay) {
        // R6 -- a display LUT belongs on the delivered display signal, and
        // there is one only when the delivery is Rec.709 SDR.
        if (color.output_gamut != "rec709" ||
            color.output_transfer != "rec709") {
            error = "the look LUT expects a Rec.709 display signal ('" +
                    std::string(kLookLutSpaceDisplay) +
                    "') but this project delivers " + color.output_gamut +
                    " / " + color.output_transfer +
                    ": there is no Rec.709 signal to lay it on; deliver in "
                    "Rec.709, or use a LUT computed on '" +
                    std::string(kLookLutSpaceWorkingLog) + "'";
            return false;
        }
        stage = LookLutStage::DeliveredDisplay;
        return true;
    }
    if (lut.space != kLookLutSpaceWorkingLog) {
        error = "unsupported look LUT space '" + lut.space +
                "': under colour management this build applies a look LUT "
                "on the '" +
                std::string(kLookLutSpaceWorkingLog) +
                "' code values of the working space, or on the delivered "
                "signal of a Rec.709 SDR delivery ('" +
                std::string(kLookLutSpaceDisplay) + "'), and nowhere else";
        return false;
    }
    if (color.working_gamut != kLookLutSpaceWorkingLog) {
        error = "the look LUT expects '" +
                std::string(kLookLutSpaceWorkingLog) +
                "' but the working space is '" + color.working_gamut +
                "': there is no log stage to lay it at";
        return false;
    }
    stage = LookLutStage::WorkingLog;
    return true;
}

bool ParseLookLutCube(const std::string& text, LookLutSamples& output,
                      std::string& error) {
    output = LookLutSamples{};
    error.clear();
    int32_t size = 0;
    double domainMin[3] = {0.0, 0.0, 0.0};
    double domainMax[3] = {1.0, 1.0, 1.0};
    std::vector<float> rgba;
    size_t triplets = 0;
    size_t expected = 0;
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    std::string raw;
    int64_t lineNumber = 0;
    const auto fail = [&](const std::string& what) {
        error = "look LUT line " + std::to_string(lineNumber) + ": " + what;
        output = LookLutSamples{};
        return false;
    };
    while (std::getline(stream, raw)) {
        ++lineNumber;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const size_t first = raw.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (raw[first] == '#') continue;
        std::istringstream line(raw);
        line.imbue(std::locale::classic());
        std::string keyword;
        line >> keyword;
        if (keyword == "TITLE") continue;
        if (keyword == "LUT_1D_SIZE") {
            return fail(
                "this is a 1D LUT; a sequence look is a 3D .cube "
                "(LUT_3D_SIZE)");
        }
        if (keyword == "LUT_3D_SIZE") {
            if (size != 0) return fail("LUT_3D_SIZE is declared twice");
            int64_t declared = 0;
            if (!(line >> declared)) return fail("LUT_3D_SIZE has no size");
            std::string extra;
            if (line >> extra) return fail("LUT_3D_SIZE takes one number");
            if (declared < kMinLookLutSize || declared > kMaxLookLutSize) {
                return fail("LUT_3D_SIZE " + std::to_string(declared) +
                            " is outside " + std::to_string(kMinLookLutSize) +
                            ".." + std::to_string(kMaxLookLutSize));
            }
            if (!rgba.empty()) {
                return fail("LUT_3D_SIZE comes after the values");
            }
            size = static_cast<int32_t>(declared);
            expected = static_cast<size_t>(size) * size * size;
            rgba.reserve(expected * 4);
            continue;
        }
        if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX") {
            double* target = keyword == "DOMAIN_MIN" ? domainMin : domainMax;
            if (!ReadFloats(line, 3, target)) {
                return fail(keyword + " takes three finite numbers");
            }
            continue;
        }
        // Anything left has to be a triplet, which means the keyword is
        // really its first number.
        double values[3] = {0.0, 0.0, 0.0};
        char* end = nullptr;
        values[0] = std::strtod(keyword.c_str(), &end);
        if (end == keyword.c_str() || *end != '\0' ||
            !std::isfinite(values[0])) {
            return fail("unknown keyword '" + keyword + "'");
        }
        if (!ReadFloats(line, 2, values + 1)) {
            return fail("a value line holds three finite numbers");
        }
        if (size == 0) return fail("a value comes before LUT_3D_SIZE");
        if (triplets == expected) {
            return fail("more values than LUT_3D_SIZE " + std::to_string(size) +
                        " asks for");
        }
        for (const double value : values) {
            // Refused rather than clamped: FFmpeg's lut3d keeps it in float
            // and clamps it in 16 bits, so a file leaving [0, 1] is a file
            // the monitor and the export would read differently.
            if (value < 0.0 || value > 1.0) {
                return fail("value " + std::to_string(value) +
                            " is outside [0, 1]");
            }
            rgba.push_back(static_cast<float>(value));
        }
        rgba.push_back(1.0f);
        ++triplets;
    }
    if (size == 0) {
        error = "look LUT: no LUT_3D_SIZE";
        return false;
    }
    for (int channel = 0; channel < 3; ++channel) {
        if (domainMin[channel] != 0.0 || domainMax[channel] != 1.0) {
            error =
                "look LUT: only the 0..1 domain is read, because FFmpeg's "
                "lut3d and a texture lookup do not agree on any other one";
            return false;
        }
    }
    if (triplets != expected) {
        error = "look LUT: " + std::to_string(triplets) + " values for the " +
                std::to_string(expected) + " LUT_3D_SIZE " +
                std::to_string(size) + " asks for";
        return false;
    }
    output.size = size;
    output.rgba = std::move(rgba);
    return true;
}

std::array<float, 3> EvaluateLookLut(const LookLutSamples& lut,
                                     std::array<float, 3> color) {
    if (lut.size < kMinLookLutSize ||
        lut.rgba.size() !=
            static_cast<size_t>(lut.size) * lut.size * lut.size * 4) {
        return color;
    }
    const int32_t last = lut.size - 1;
    double weight[3] = {0.0, 0.0, 0.0};
    int32_t low[3] = {0, 0, 0};
    for (int channel = 0; channel < 3; ++channel) {
        const double position =
            std::clamp(static_cast<double>(color[channel]), 0.0, 1.0) * last;
        const double floor = std::floor(position);
        low[channel] = static_cast<int32_t>(
            std::min<double>(floor, static_cast<double>(last - 1)));
        weight[channel] = position - low[channel];
    }
    std::array<double, 3> mixed{0.0, 0.0, 0.0};
    for (int corner = 0; corner < 8; ++corner) {
        const int32_t red = low[0] + ((corner & 1) != 0 ? 1 : 0);
        const int32_t green = low[1] + ((corner & 2) != 0 ? 1 : 0);
        const int32_t blue = low[2] + ((corner & 4) != 0 ? 1 : 0);
        const double share = ((corner & 1) != 0 ? weight[0] : 1.0 - weight[0]) *
                             ((corner & 2) != 0 ? weight[1] : 1.0 - weight[1]) *
                             ((corner & 4) != 0 ? weight[2] : 1.0 - weight[2]);
        if (share == 0.0) continue;
        const size_t offset =
            ((static_cast<size_t>(blue) * lut.size + green) * lut.size + red) *
            4;
        for (int channel = 0; channel < 3; ++channel)
            mixed[channel] += share * lut.rgba[offset + channel];
    }
    return {static_cast<float>(mixed[0]), static_cast<float>(mixed[1]),
            static_cast<float>(mixed[2])};
}
