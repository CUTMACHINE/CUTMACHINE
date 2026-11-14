#include "ColorEffects.h"
#include "ColorManagement.h"
#include "ResolveContrastTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const Ulid kFirst = "01K90000000000000000000001";
const Ulid kSecond = "01K90000000000000000000002";
const Ulid kThird = "01K90000000000000000000003";

// FNV-1a, to pin a cube's bytes without committing ten megabytes.
uint64_t Fnv1a(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// PRD12 -- docs/roadmap/PRODUIT-2026-10.md. The eight scalar stages, as an
// unmanaged clip grade resolves them.
std::vector<ClipEffect> EightScalarStages() {
    return {
        {kFirst, "color.exposure", {{"amount", {1, 3}}}},
        {kSecond, "color.contrast", {{"amount", {1, 4}}}},
        {kThird, "color.saturation", {{"amount", {-1, 5}}}},
        {kFirst, "color.vibrance", {{"amount", {1, 2}}}},
        {kSecond, "color.temperature", {{"kelvin", {5200, 1}}}},
        {kThird, "color.tint", {{"amount", {1, 10}}}},
        {kFirst, "color.highlights", {{"amount", {3, 10}}}},
        {kSecond, "color.shadows", {{"amount", {1, 5}}}},
    };
}

// PRD12 -- what a cube's bytes depend on beyond the document. Where the
// compiler fuses a multiply and an add: under -ffp-contract=off the
// saturation, vibrance, highlights, shadows and temperature stages move by
// up to 3e-6. And the last bit of the system's powf, which the temperature
// stage calls in display space: 1.3e-7. Neither is fixed from one Mac to
// the next -- the runner of 1 October 2026 (AppleClang 15, macOS 14) wrote
// other bytes than the Mac the hash was taken on -- while exposure, tint and
// contrast alone are sensitive to neither. So the cube is pinned to a
// reference on every eighth lattice point, within a hundredth of a 10-bit
// step, and byte for byte only against a cube the same binary wrote.
constexpr size_t kCubeEdge = 65;
constexpr size_t kCubeHeaderLines = 4;
constexpr size_t kReferenceStride = 8;
constexpr double kReferenceTolerance = 1e-5;

// The cube's lattice lines, red moving fastest.
std::vector<std::string> CubeRows(const std::string& cube) {
    std::vector<std::string> rows;
    std::istringstream lines(cube);
    std::string line;
    for (size_t index = 0; std::getline(lines, line); ++index) {
        if (index >= kCubeHeaderLines) {
            rows.push_back(line);
        }
    }
    return rows;
}

// One reference line per sampled lattice point: its red, green and blue
// indices, then the cube's own line there.
std::string CubeReference(const std::string& cube) {
    const std::vector<std::string> rows = CubeRows(cube);
    std::ostringstream reference;
    for (size_t blue = 0; blue < kCubeEdge; blue += kReferenceStride) {
        for (size_t green = 0; green < kCubeEdge; green += kReferenceStride) {
            for (size_t red = 0; red < kCubeEdge; red += kReferenceStride) {
                const size_t index =
                    (blue * kCubeEdge * kCubeEdge) + (green * kCubeEdge) + red;
                reference << red << ' ' << green << ' ' << blue << ' '
                          << (index < rows.size() ? rows[index] : "missing")
                          << '\n';
            }
        }
    }
    return reference.str();
}

// Empty when every sampled point of `cube` is within the tolerance of the
// reference file; the first point that is not, otherwise.
std::string CompareCubeReference(const std::string& cube,
                                 const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return "cannot read " + path;
    }
    const std::vector<std::string> rows = CubeRows(cube);
    std::string line;
    size_t points = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        size_t red = 0;
        size_t green = 0;
        size_t blue = 0;
        std::array<double, 3> expected{};
        if (!(fields >> red >> green >> blue >> expected[0] >> expected[1] >>
              expected[2]) ||
            red >= kCubeEdge || green >= kCubeEdge || blue >= kCubeEdge) {
            return "unreadable reference line: " + line;
        }
        const size_t index =
            (blue * kCubeEdge * kCubeEdge) + (green * kCubeEdge) + red;
        std::istringstream written(index < rows.size() ? rows[index] : "");
        std::array<double, 3> actual{};
        if (!(written >> actual[0] >> actual[1] >> actual[2])) {
            return "no cube line at " + line;
        }
        for (size_t channel = 0; channel < 3; ++channel) {
            if (std::abs(actual[channel] - expected[channel]) >
                kReferenceTolerance) {
                return "lattice point " + std::to_string(red) + ' ' +
                       std::to_string(green) + ' ' + std::to_string(blue) +
                       " writes '" + rows[index] + "', the reference '" + line +
                       "'";
            }
        }
        ++points;
    }
    const size_t sampled =
        (kCubeEdge + kReferenceStride - 1) / kReferenceStride;
    return points == sampled * sampled * sampled
               ? std::string()
               : "the reference holds " + std::to_string(points) + " points";
}

ClipEffect Curves(std::map<std::string, EffectParamValue> params) {
    return {kFirst, "color.curves", std::move(params)};
}

bool Accepted(const ClipEffect& effect, std::string& error) {
    error.clear();
    return ValidateColorEffect(effect, error);
}

// The wheel formula of ColorEffects.h, written out again in double from the
// params, so that ColorEffects.cc is checked against the documented formula
// rather than against itself.
std::array<double, 3> WheelReference(
    const std::map<std::string, EffectParamValue>& params,
    const std::array<double, 3>& color) {
    const auto value = [&](const std::string& name) {
        const auto found = params.find(name);
        return found == params.end()
                   ? 0.0
                   : static_cast<double>(found->second.num) / found->second.den;
    };
    const std::array<const char*, 3> channels = {"r", "g", "b"};
    std::array<double, 3> result{};
    for (size_t channel = 0; channel < 3; ++channel) {
        const std::string c = channels[channel];
        const double lift = value("lift_" + c) + value("lift_master");
        const double gain =
            std::exp2(value("gain_" + c) + value("gain_master"));
        const double gamma =
            std::exp2(value("gamma_" + c) + value("gamma_master"));
        const double offset = value("offset_" + c) + value("offset_master");
        double y = ((gain - lift) * color[channel]) + lift + offset;
        if (y > 0.0) {
            y = std::pow(y, 1.0 / gamma);
        }
        result[channel] = y;
    }
    return result;
}

}  // namespace

// GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. color.resolve_primaries,
// held to the cubes DaVinci Resolve 21.1 exported on 25 October 2026
// (tests/fixtures/resolve_primaries, written by make_fixture.py).
EffectParamValue ParseFraction(const std::string& text) {
    const size_t slash = text.find('/');
    return {static_cast<int32_t>(std::stol(text.substr(0, slash))),
            static_cast<int32_t>(std::stol(text.substr(slash + 1)))};
}

struct ResolveCase {
    std::string name;
    ClipEffect effect;
    std::vector<std::array<float, 6>> points;
};

std::vector<ResolveCase> ReadResolveCubes() {
    std::vector<ResolveCase> cases;
    std::ifstream file(std::string(RESOLVE_PRIMARIES_FIXTURES_DIR) +
                       "/resolve_cubes.txt");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "case") {
            cases.push_back({});
            words >> cases.back().name;
            cases.back().effect = {kFirst, "color.resolve_primaries", {}};
        } else if (word == "param" && !cases.empty()) {
            std::string name;
            std::string value;
            words >> name >> value;
            cases.back().effect.params[name] = ParseFraction(value);
        } else if (word == "point" && !cases.empty()) {
            std::array<float, 6> point{};
            for (float& value : point) words >> value;
            cases.back().points.push_back(point);
        }
    }
    return cases;
}

ClipEffect ResolveStage(
    std::initializer_list<std::pair<const char*, EffectParamValue>> params) {
    ClipEffect effect{kFirst, "color.resolve_primaries", {}};
    for (const auto& [name, value] : params) effect.params[name] = value;
    return effect;
}

void TestResolvePrimaries() {
    // --- Validation ---------------------------------------------------
    std::string error;
    Check(ValidateColorEffect(ResolveStage({{"lift_r", {-3, 100}},
                                            {"lift_master", {-2, 100}},
                                            {"gamma_master", {399, 100}},
                                            {"gain_b", {97, 100}},
                                            {"offset_r", {8, 1}},
                                            {"saturation", {2, 1}},
                                            {"lum_mix", {0, 1}}}),
                              error),
          "color.resolve_primaries takes every knob Resolve stores, at the "
          "far ends the author's grades reach: " +
              error);
    const auto refuses = [&](const ClipEffect& effect, const char* words,
                             const std::string& what) {
        std::string message;
        Check(!ValidateColorEffect(effect, message) &&
                  message.find(words) != std::string::npos,
              what + " (" + message + ")");
    };
    refuses(ResolveStage({{"offset_master", {1, 10}}}), "unknown parameter",
            "Resolve's offset has no master: refused by name");
    // GR16 -- the temperature and the tint, within the ends of Resolve's
    // panel.
    Check(ValidateColorEffect(
              ResolveStage({{"temperature", {-4000, 1}}, {"tint", {100, 1}}}),
              error),
          "a temperature and a tint at the ends of Resolve's panel are "
          "taken: " +
              error);
    refuses(ResolveStage({{"temperature", {4001, 1}}}), "outside [-4000, 4000]",
            "a temperature past Resolve's panel is refused");
    refuses(ResolveStage({{"tint", {-201, 2}}}), "outside [-100, 100]",
            "a tint past Resolve's panel is refused");
    refuses(ResolveStage({{"contrast", {19, 10}}}), "above 9/5",
            "a contrast past the measured S-curve is refused");
    refuses(ResolveStage({{"contrast", {2, 1}}, {"contrast_s_curve", {0, 1}}}),
            "no slope", "a straight contrast of 2 is refused");
    refuses(ResolveStage({{"color_space_aware", {1, 2}}}), "is 0 or 1",
            "a project setting is 0 or 1");
    // GR16 -- in ACEScct (7 and 14 November 2026): everything the stage
    // takes was measured, the contrast with the colour-space-aware tools off
    // only.
    refuses(ResolveStage({{"acescct", {2, 1}}}), "is 0 or 1",
            "acescct is 0 or 1");
    Check(ValidateColorEffect(ResolveStage({{"acescct", {1, 1}},
                                            {"saturation", {3, 2}},
                                            {"gain_r", {6, 5}},
                                            {"contrast", {6, 5}},
                                            {"pivot", {2, 5}},
                                            {"contrast_s_curve", {1, 1}},
                                            {"temperature", {400, 1}},
                                            {"tint", {-20, 1}}}),
                              error),
          "in ACEScct, a saturation, a gain, a contrast, a temperature and a "
          "tint are taken: " +
              error);
    refuses(ResolveStage({{"acescct", {1, 1}},
                          {"color_space_aware", {1, 1}},
                          {"contrast", {6, 5}}}),
            "contrast 6/5 is not measured in ACEScct with the "
            "colour-space-aware tools",
            "in ACEScct, a contrast with the aware tools is refused by name");
    Check(ValidateColorEffect(ResolveStage({{"acescct", {1, 1}},
                                            {"color_space_aware", {1, 1}},
                                            {"temperature", {400, 1}}}),
                              error),
          "in ACEScct, the aware tools change nothing but the contrast: " +
              error);
    Check(ValidateColorEffect(ResolveStage({{"contrast", {9, 5}},
                                            {"pivot", {87, 200}},
                                            {"color_space_aware", {1, 1}},
                                            {"contrast_s_curve", {1, 1}}}),
                              error),
          "a contrast at the end of the table, its pivot and the project's "
          "settings are taken: " +
              error);
    refuses(ResolveStage({{"gamma_g", {41, 10}}}), "outside [-4, 4]",
            "a gamma past what Resolve stores is refused");
    refuses(ResolveStage({{"lum_mix", {3, 2}}}), "outside [0, 1]",
            "lum_mix lies in [0, 1]");
    refuses(ResolveStage({{"lift_b", {2, 1}}, {"gain_b", {0, 1}}}),
            "black at or above white",
            "a lift and a gain that put black above white are refused, on "
            "the fractions");
    Check(ValidateColorEffect(
              ResolveStage({{"lift_b", {1, 2}}, {"gain_b", {0, 1}}}), error),
          "a lift of 1/2 with a gain of 0 still leaves black below white");

    // --- Neutral ------------------------------------------------------
    Check(ResolveColorGrade(
              {ResolveStage({{"gain_r", {1, 1}}, {"lum_mix", {1, 2}}})}, {})
                  .count == 0,
          "a neutral color.resolve_primaries resolves to nothing, whatever "
          "its lum_mix");
    // GR16 -- a stored temperature and tint of 0 are Resolve's identity
    // (measured: the cube of such a node is the neutral one).
    Check(ResolveColorGrade(
              {ResolveStage({{"temperature", {0, 1}}, {"tint", {0, 1}}})}, {})
                  .count == 0,
          "a temperature and a tint of 0 resolve to nothing");
    {
        // GR16 -- the matrix ends the block, and only when it moves: a node
        // without it keeps the block it always had.
        const ResolvedColorGrade plain =
            ResolveColorGrade({ResolveStage({{"gain_r", {6, 5}}})}, {});
        const ResolvedColorGrade warmed = ResolveColorGrade(
            {ResolveStage({{"gain_r", {6, 5}}, {"temperature", {400, 1}}})},
            {});
        const ResolvedColorGrade both = ResolveColorGrade(
            {ResolveStage({{"contrast", {6, 5}}, {"tint", {-20, 1}}})}, {});
        const std::array<double, 9> matrix =
            ResolveWhiteBalanceMatrix(400.0, 0.0);
        bool tail = warmed.count == 1;
        for (size_t index = 0; tail && index < 9; ++index)
            tail = warmed.data[kResolvePrimariesDataSize + index] ==
                   static_cast<float>(matrix[index]);
        Check(
            plain.count == 1 &&
                plain.entries[0].data_count == kResolvePrimariesDataSize &&
                warmed.entries[0].data_count ==
                    kResolvePrimariesDataSize + kResolveWhiteBalanceDataSize &&
                tail && both.count == 1 &&
                both.entries[0].data_count ==
                    kResolvePrimariesContrastDataSize +
                        kResolveWhiteBalanceDataSize,
            "the temperature and tint matrix ends the block, after the "
            "contrast when there is one, and only when it moves");
        // A grey keeps its Rec.709 luma within 0.15 % at the panel's ends
        // (what the cubes show), and a warmer temperature lifts red over
        // blue.
        const std::array<double, 9> hot = ResolveWhiteBalanceMatrix(4000, 0);
        const double red = hot[0] + hot[1] + hot[2];
        const double green = hot[3] + hot[4] + hot[5];
        const double blue = hot[6] + hot[7] + hot[8];
        Check(red > green && green > blue &&
                  std::abs((0.2126 * red) + (0.7152 * green) + (0.0722 * blue) -
                           1.0) < 1.5e-3,
              "a temperature of 4000 warms a grey and keeps its luma");
    }
    {
        // GR16 -- the saturation turns about DaVinci YRGB's luma, or
        // Rec.709's in ACEScct: three weights in the block, after it.
        const ResolvedColorGrade yrgb =
            ResolveColorGrade({ResolveStage({{"saturation", {3, 2}}})}, {});
        const ResolvedColorGrade aces = ResolveColorGrade(
            {ResolveStage({{"saturation", {3, 2}}, {"acescct", {1, 1}}})}, {});
        Check(yrgb.count == 1 && aces.count == 1 && yrgb.data[37] == 0.21f &&
                  yrgb.data[38] == 0.70f && yrgb.data[39] == 0.09f &&
                  aces.data[37] == 0.2126f && aces.data[38] == 0.7152f &&
                  aces.data[39] == 0.0722f && yrgb.data[40] == 0.0f &&
                  aces.data[40] == 1.0f,
              "the saturation's luma weights follow the block's saturation: "
              "DaVinci YRGB's, or Rec.709's in ACEScct, and the block says "
              "which");
        // Y + 1.5 (x - Y), Y with each set of weights.
        const std::array<float, 3> bluish = {0.2f, 0.2f, 0.5f};
        const std::array<float, 3> a = ApplyResolvedColorGrade(bluish, yrgb);
        const std::array<float, 3> b = ApplyResolvedColorGrade(bluish, aces);
        const float lumaYrgb = (0.21f * 0.2f) + (0.70f * 0.2f) + (0.09f * 0.5f);
        const float luma709 =
            (0.2126f * 0.2f) + (0.7152f * 0.2f) + (0.0722f * 0.5f);
        Check(
            std::abs(a[0] - (lumaYrgb + (1.5f * (0.2f - lumaYrgb)))) < 1e-6f &&
                std::abs(b[0] - (luma709 + (1.5f * (0.2f - luma709)))) < 1e-6f,
            "a saturation of 1.5 turns about DaVinci YRGB's luma, or "
            "Rec.709's in ACEScct");
    }
    {
        // GR16 -- in ACEScct a negative value goes through the gamma
        // unchanged; in DaVinci YRGB it keeps its sign, -(|v|^e).
        const auto gammaOf = [](int32_t acescct) {
            return ResolveColorGrade(
                {ResolveStage({{"lift_r", {-1, 5}},
                               {"gamma_r", {1, 4}},
                               {"lum_mix", {0, 1}},
                               {"acescct", {acescct, 1}}})},
                {});
        };
        // On black, v = a = L G / (1 - L + L G) = -1/5 at a gain of 1.
        const float yrgb =
            ApplyResolvedColorGrade({0.0f, 0.5f, 0.5f}, gammaOf(0))[0];
        const float aces =
            ApplyResolvedColorGrade({0.0f, 0.5f, 0.5f}, gammaOf(1))[0];
        Check(std::abs(yrgb + std::pow(0.2f, 0.8f)) < 1e-6f &&
                  std::abs(aces + 0.2f) < 1e-6f,
              "a negative value keeps its sign through the gamma in DaVinci "
              "YRGB, and goes through unchanged in ACEScct");
        // GR16 -- the temperature and tint in ACEScct: on a grey, a
        // constant in log, the AP0 light of each channel multiplied by its
        // row sum to the power 2.4 (14 November 2026).
        const ResolvedColorGrade warm = ResolveColorGrade(
            {ResolveStage({{"temperature", {1000, 1}}, {"acescct", {1, 1}}})},
            {});
        const std::array<double, 9> m = ResolveWhiteBalanceMatrix(1000, 0);
        std::array<double, 3> gain{};
        for (size_t row = 0; row < 3; ++row) {
            for (size_t k = 0; k < 3; ++k) {
                gain.at(row) +=
                    kResolveAp0ToAp1.at((row * 3) + k) *
                    std::pow(m[k * 3] + m[(k * 3) + 1] + m[(k * 3) + 2], 2.4);
            }
        }
        double worst = 0.0;
        for (const float code : {0.3f, 0.4135884f, 0.6f}) {
            const std::array<float, 3> out =
                ApplyResolvedColorGrade({code, code, code}, warm);
            for (size_t row = 0; row < 3; ++row) {
                const double expected =
                    code + (std::log2(gain.at(row)) / 17.52);
                worst = std::max(worst, std::abs(out[row] - expected));
            }
        }
        Check(worst < 1e-5,
              "in ACEScct, a temperature moves a grey by a constant in log, "
              "its AP0 light by the row sums of its matrix to the power 2.4 "
              "(miss " +
                  std::to_string(worst) + ")");
    }

    // --- Against Resolve's own cubes ----------------------------------
    const std::vector<ResolveCase> cases = ReadResolveCubes();
    Check(cases.size() == 519 && std::all_of(cases.begin(), cases.end(),
                                             [](const ResolveCase& one) {
                                                 return one.points.size() ==
                                                        125;
                                             }),
          "the reference holds 519 cases of 125 lattice points each");
    // GR16 -- the worst miss of each folder of the measurement: the
    // primaries alone, then the contrast in each of the project settings,
    // then the temperature and the tint (6 November 2026), whose last two
    // folders say apart the nodes that carry a contrast.
    std::map<std::string, std::pair<double, std::string>> worstOf;
    for (const ResolveCase& one : cases) {
        std::string message;
        Check(ValidateColorEffect(one.effect, message),
              one.name + " is a valid stage: " + message);
        const ResolvedColorGrade grade = ResolveColorGrade({one.effect}, {});
        std::string folder = one.name.substr(0, one.name.find('/'));
        if ((folder == "chaine" || folder == "hasard" ||
             folder == "acescct-chaine" || folder == "acescct-hasard" ||
             folder == "acescct-temoin") &&
            one.effect.params.count("contrast") != 0)
            folder += " avec contraste";
        // GR16 -- in ACEScct, the two full nodes (every primary and a
        // saturation) apart from the single settings.
        if (folder == "acescct" &&
            one.name.find("noeud_type") != std::string::npos)
            folder += " nœud complet";
        for (const std::array<float, 6>& point : one.points) {
            const std::array<float, 3> out =
                ApplyResolvedColorGrade({point[0], point[1], point[2]}, grade);
            auto& [worst, where] = worstOf[folder];
            for (size_t channel = 0; channel < 3; ++channel) {
                const double miss = std::abs(
                    std::clamp(out[channel], 0.0f, 1.0f) - point[3 + channel]);
                if (miss > worst) {
                    worst = miss;
                    std::ostringstream at;
                    at << one.name << " at (" << point[0] << ", " << point[1]
                       << ", " << point[2] << ") channel " << channel
                       << ": Resolve " << point[3 + channel] << ", engine "
                       << out[channel];
                    where = at.str();
                }
            }
        }
    }
    for (const auto& [folder, worst] : worstOf) {
        std::cout << "Resolve primaries against Resolve 21.1's cubes, "
                  << folder << ": worst " << worst.first * 1023.0
                  << " of a 10-bit code (" << worst.second << ")\n";
    }
    Check(std::max(worstOf["yrgb"].first, worstOf["serie2"].first) <= 1e-4,
          "every lattice point of every case of the primaries lands within a "
          "tenth of a 10-bit code of Resolve's own cube");
    // GR16 -- the same thirteen nodes posed in a project in ACEScct (6
    // November 2026), the saturation about Rec.709's luma: each setting
    // alone within a tenth of a code. The two full nodes missed by 4 codes
    // where a channel went below 0 before the gamma, until the gamma passed
    // a negative value unchanged there (14 November 2026).
    Check(worstOf.count("acescct") == 1 && worstOf["acescct"].first <= 1e-4,
          "in ACEScct, every single setting of the primaries and the "
          "saturation lands within a tenth of a code of Resolve's cube");
    Check(worstOf.count("acescct nœud complet") == 1 &&
              worstOf["acescct nœud complet"].first <= 1e-4,
          "in ACEScct, a full node lands within a tenth of a code of "
          "Resolve's cube");
    // GR16 -- the temperature and the tint: alone, together, with every
    // other primary, and on the 12 nodes drawn at random without a
    // contrast, the measured law
    // lands within 0.05 of a code (the cubes give six decimals; a matrix
    // fitted freely to each one does no better than 0.02 to 0.03).
    double balance = 0.0;
    for (const char* folder :
         {"temperature", "teinte", "grille", "chaine", "hasard"}) {
        Check(worstOf.count(folder) == 1,
              std::string("the reference holds the folder ") + folder);
        balance = std::max(balance, worstOf[folder].first);
    }
    Check(balance <= 1e-4,
          "every node with a temperature or a tint and no contrast lands "
          "within a tenth of a code of Resolve's cube");
    // With a contrast, the contrast's own misses: a contrast below 1 at
    // Lum Mix 100 already misses Resolve's cube by 2.7 codes without any
    // temperature (gr15-facteurs/aware-recharge, never in this reference
    // until now), and the Lum Mix part of the contrast where a channel
    // goes below 0, which a warm or green node brings about more often.
    Check(std::max(worstOf["chaine avec contraste"].first,
                   worstOf["hasard avec contraste"].first) <= 4.0 / 1023,
          "with a contrast too, every point within four codes of Resolve");
    // GR16 -- with a contrast, what the measurement allowed: 0.75 of a code
    // in the author's setting (colour-space-aware tools on, Lum Mix 100 near
    // black the worst), 1.14 without the aware tools, 1.73 with the S-curve
    // switched off, a setting his projects do not use.
    Check(worstOf["aware-combi"].first <= 1.0 / 1023,
          "with the aware tools, every point within a code of Resolve");
    Check(worstOf["contraste-dwg"].first <= 1.5 / 1023,
          "without them, within a code and a half");
    Check(worstOf["contraste-lin"].first <= 2.0 / 1023,
          "with the S-curve switched off, within two codes");
    // GR16 -- in ACEScct (14 November 2026), the folders above posed again
    // in the probe project, then 24 nodes drawn at random for it alone
    // (acescct-temoin), never used to look for the laws. Without a
    // contrast, within two codes: the worst points are values above 100 in
    // linear light whose balanced channel nearly cancels, where Resolve's
    // floats round (p99 of the full cubes 0.035). The contrast alone lands
    // as in DaVinci YRGB, whose cubes it gives to the last digit. With a
    // contrast and a temperature, up to 24 codes: the temperature takes a
    // channel of a bright saturated colour far below 0 (to -15), and below 0
    // Resolve's contrast goes on along a slope 13 % flatter to 32 % steeper
    // than the table's first step (measured, gr16-acescct/extrap;
    // GRADE-RESOLVE-2026-10, "Ce qui reste"). Where no channel goes below
    // -0.05, 4 codes.
    const std::map<std::string, double> acescctMost = {
        {"acescct-temperature", 2.0},
        {"acescct-teinte", 2.0},
        {"acescct-grille", 2.0},
        {"acescct-chaine", 2.0},
        {"acescct-hasard", 2.0},
        {"acescct-temoin", 2.0},
        {"acescct-contraste", 1.5},
        {"acescct-contraste-lin", 2.0},
        {"acescct-chaine avec contraste", 24.0},
        {"acescct-hasard avec contraste", 24.0},
        {"acescct-temoin avec contraste", 24.0}};
    for (const auto& [folder, most] : acescctMost) {
        Check(
            worstOf.count(folder) == 1 && worstOf[folder].first <= most / 1023,
            "in ACEScct, " + folder + " lands within " + std::to_string(most) +
                " codes of Resolve's cubes");
    }

    // --- The contrast table, off its grid -----------------------------
    {
        std::ifstream file(std::string(RESOLVE_PRIMARIES_FIXTURES_DIR) +
                           "/contrast_held_out.txt");
        std::string line;
        double middle = 0.0;
        double far = 0.0;
        size_t curves = 0;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream words(line);
            float contrast = 0.0f;
            float pivot = 0.0f;
            words >> contrast >> pivot;
            ++curves;
            for (int32_t i = 0; i < kResolveContrastSamples; ++i) {
                float measured = 0.0f;
                words >> measured;
                if (measured <= 0.002f || measured >= 0.998f) continue;
                const double miss =
                    std::abs(ResolveAwareSCurve(contrast, pivot,
                                                static_cast<float>(i) / 64) -
                             measured);
                double& worst = pivot >= 0.25f && pivot <= 0.75f ? middle : far;
                worst = std::max(worst, miss);
            }
        }
        std::cout << "contrast table off its grid: worst " << middle * 1023
                  << " of a code for a pivot from 0.25 to 0.75, " << far * 1023
                  << " beyond\n";
        Check(curves == 16 && middle <= 0.15 / 1023 && far <= 5.0 / 1023,
              "16 curves measured off the grid: within 0.15 of a code for a "
              "pivot from 0.25 to 0.75, within 5 at the far pivots");
    }

    // --- Under colour management --------------------------------------
    for (const ColorGradeSpace space :
         {ColorGradeSpace::AcesCct, ColorGradeSpace::LinearRec709,
          ColorGradeSpace::LinearRec2020}) {
        const ResolvedColorGrade grade = ResolveColorGrade(
            {ResolveStage({{"saturation", {3, 2}}})}, {space, 0});
        bool ok = grade.count == 1;
        if (ok) {
            const float* block =
                grade.data.data() + grade.entries[0].data_offset;
            for (int row = 0; row < 3; ++row) {
                double toWide = 0.0;
                for (int column = 0; column < 3; ++column)
                    toWide += block[1 + row * 3 + column];
                ok = ok && std::abs(toWide - 1.0) < 1e-6;
                for (int column = 0; column < 3; ++column) {
                    double product = 0.0;
                    for (int k = 0; k < 3; ++k)
                        product +=
                            static_cast<double>(block[10 + row * 3 + k]) *
                            block[1 + k * 3 + column];
                    ok = ok &&
                         std::abs(product - (row == column ? 1.0 : 0.0)) < 1e-5;
                }
            }
        }
        Check(ok,
              "the way to DaVinci Wide Gamut keeps a grey grey, and the way "
              "back undoes it, in space " +
                  std::to_string(static_cast<int>(space)));
    }
    {
        // A grey through ACEScct: to DaVinci Intermediate, gain 2 on every
        // channel and on the master, and back.
        const ResolvedColorGrade grade =
            ResolveColorGrade({ResolveStage({{"gain_r", {2, 1}},
                                             {"gain_g", {2, 1}},
                                             {"gain_b", {2, 1}},
                                             {"gain_master", {2, 1}}})},
                              {ColorGradeSpace::AcesCct, 0});
        double miss = 0.0;
        for (const double code : {0.1, 0.3, 0.4135884, 0.55, 0.7}) {
            const double node =
                EncodeDaVinciIntermediate(DecodeAcesCct(code)) * 2.0;
            const double expected =
                EncodeAcesCct(DecodeDaVinciIntermediate(node));
            const std::array<float, 3> out = ApplyResolvedColorGrade(
                {static_cast<float>(code), static_cast<float>(code),
                 static_cast<float>(code)},
                grade);
            for (const float channel : out)
                miss = std::max(miss, std::abs(channel - expected));
        }
        Check(miss < 2e-5,
              "under ACEScct the stage grades DaVinci Intermediate, as a "
              "Resolve timeline in DaVinci Wide Gamut does (miss " +
                  std::to_string(miss) + ")");
    }
    {
        // GR16 -- a node graded in ACEScct grades ACEScct codes in AP1: an
        // engine grading in ACEScct hands its own values over, a linear one
        // goes through AP1 (a grey stays a grey) and ACEScct.
        const auto doubled = [](ColorGradeSpace space) {
            return ResolveColorGrade({ResolveStage({{"gain_r", {2, 1}},
                                                    {"gain_g", {2, 1}},
                                                    {"gain_b", {2, 1}},
                                                    {"gain_master", {2, 1}},
                                                    {"acescct", {1, 1}}})},
                                     {space, 0});
        };
        const ResolvedColorGrade aces = doubled(ColorGradeSpace::AcesCct);
        const ResolvedColorGrade linear =
            doubled(ColorGradeSpace::LinearRec709);
        double codes = 0.0;
        double light = 0.0;
        for (const double code : {0.1, 0.3, 0.4135884, 0.55}) {
            for (const float channel : ApplyResolvedColorGrade(
                     {static_cast<float>(code), static_cast<float>(code),
                      static_cast<float>(code)},
                     aces))
                codes = std::max(codes, std::abs(channel - (2.0 * code)));
            const double x = DecodeAcesCct(code);
            const double expected = DecodeAcesCct(2.0 * code);
            for (const float channel : ApplyResolvedColorGrade(
                     {static_cast<float>(x), static_cast<float>(x),
                      static_cast<float>(x)},
                     linear))
                light =
                    std::max(light, std::abs(channel - expected) / expected);
        }
        Check(codes < 1e-6 && light < 1e-4,
              "a node graded in ACEScct grades ACEScct codes, from an engine "
              "in ACEScct (miss " +
                  std::to_string(codes) + ") or in linear Rec.709 (relative " +
                  std::to_string(light) + ")");
    }
    Check(
        EncodeDaVinciIntermediate(0.18) > 0.33 &&
            EncodeDaVinciIntermediate(0.18) < 0.34 &&
            std::abs(
                DecodeDaVinciIntermediate(EncodeDaVinciIntermediate(0.001)) -
                0.001) < 1e-12 &&
            std::abs(DecodeDaVinciIntermediate(EncodeDaVinciIntermediate(5.0)) -
                     5.0) < 1e-9,
        "DaVinci Intermediate puts 18 % grey at 0.336, and decodes what it "
        "encodes on both of its branches");

    // --- In a stack ---------------------------------------------------
    const ClipEffect imported = ResolveStage({{"gamma_r", {1, 10}}});
    const ClipEffect exposure{kSecond, "color.exposure", {{"amount", {1, 2}}}};
    Check(CanonicalColorStageIndex({imported}, "color.exposure") == 1 &&
              CanonicalColorStageIndex({exposure}, "color.resolve_primaries") ==
                  0,
          "what a hand adds goes after an imported Resolve node, as a second "
          "node would");
    std::string cube;
    Check(BuildColorGradeCube(ResolveColorGrade({imported, exposure}, {}), cube,
                              error) &&
              !cube.empty(),
          "the export samples a stack with a Resolve node into its cube");
    const GpuClipGradeParameters packed =
        PackColorGradeForGpu(ResolveColorGrade({imported}, {}));
    Check(packed.count == 1 &&
              packed.entries[0].kind ==
                  static_cast<int32_t>(ColorEffectKind::ResolvePrimaries) &&
              packed.entries[0].data_count == kResolvePrimariesDataSize,
          "the monitor receives the stage and its block");
}

int main(int argc, char** argv) {
    // PRD12 -- the reference of the eight scalar stages, written again.
    if (argc == 2 && std::string(argv[1]) == "--print-eight-stage-reference") {
        std::string cube;
        std::string error;
        if (!BuildColorGradeCube(ResolveColorGrade(EightScalarStages(), {}),
                                 cube, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << CubeReference(cube);
        return 0;
    }
    // --- Registry shape -----------------------------------------------
    Check(ColorEffectRegistry().size() == 13,
          "the eight F1.3 scalar knobs, the wheels, the curves, the hue "
          "rotation, the hue curves and the Resolve primaries");
    for (const ColorEffectRegistryEntry& entry : ColorEffectRegistry()) {
        const std::string type = entry.type;
        Check(type.rfind("color.", 0) == 0,
              "every registered type carries the color. prefix: " + type);
        Check(FindColorEffectRegistryEntry(type) == &entry,
              "lookup returns the exact registered entry for " + type);
    }
    Check(FindColorEffectRegistryEntry("color.exposure") != nullptr &&
              FindColorEffectRegistryEntry("color.contrast") != nullptr &&
              FindColorEffectRegistryEntry("color.saturation") != nullptr &&
              FindColorEffectRegistryEntry("color.vibrance") != nullptr &&
              FindColorEffectRegistryEntry("color.temperature") != nullptr &&
              FindColorEffectRegistryEntry("color.tint") != nullptr &&
              FindColorEffectRegistryEntry("color.highlights") != nullptr &&
              FindColorEffectRegistryEntry("color.shadows") != nullptr,
          "all eight ROADMAP.md F1.3 knob names resolve");
    Check(
        FindColorEffectRegistryEntry("color.wheels") != nullptr &&
            FindColorEffectRegistryEntry("color.wheels")->param_name ==
                nullptr &&
            FindColorEffectRegistryEntry("color.curves") != nullptr &&
            FindColorEffectRegistryEntry("color.curves")->param_name == nullptr,
        "wheels and curves are registered, with named knobs of their own");
    Check(FindColorEffectRegistryEntry("color.curve") == nullptr &&
              FindColorEffectRegistryEntry("brightness") == nullptr &&
              FindColorEffectRegistryEntry("") == nullptr,
          "types outside the registry (future knobs, garbage) miss cleanly");

    // --- GR1: the canonical order ---------------------------------------
    {
        const std::array<const char*, 13> order = {"color.resolve_primaries",
                                                   "color.shadows",
                                                   "color.highlights",
                                                   "color.vibrance",
                                                   "color.temperature",
                                                   "color.tint",
                                                   "color.exposure",
                                                   "color.wheels",
                                                   "color.contrast",
                                                   "color.curves",
                                                   "color.hue",
                                                   "color.saturation",
                                                   "color.hue_curves"};
        bool ranked = true;
        for (size_t index = 0; index < order.size(); ++index) {
            ranked = ranked && CanonicalColorStageRank(order.at(index)) ==
                                   static_cast<int32_t>(index);
        }
        Check(ranked && CanonicalColorStageRank("color.lut") == -1 &&
                  CanonicalColorStageRank("transform.crop") == -1,
              "every registered stage has its own canonical rank, in the "
              "order a corrector node applies its tools; nothing else has "
              "one");
        const auto stack = [](std::initializer_list<const char*> types) {
            std::vector<ClipEffect> effects;
            for (const char* type : types) {
                effects.push_back({kFirst, type, {}});
            }
            return effects;
        };
        Check(IsCanonicalColorStack({}) &&
                  IsCanonicalColorStack(
                      stack({"color.exposure", "color.contrast"})) &&
                  IsCanonicalColorStack(stack(
                      {"color.wheels", "color.wheels", "color.saturation"})) &&
                  IsCanonicalColorStack(stack(
                      {"color.exposure", "color.lut", "color.contrast"})) &&
                  !IsCanonicalColorStack(
                      stack({"color.contrast", "color.exposure"})),
              "a stack is canonical when its registered stages never go "
              "back in rank; an unregistered one is skipped");
        const std::vector<ClipEffect> graded =
            stack({"color.exposure", "color.contrast", "color.saturation"});
        Check(CanonicalColorStageIndex(graded, "color.wheels") == 1 &&
                  CanonicalColorStageIndex(graded, "color.shadows") == 0 &&
                  CanonicalColorStageIndex(graded, "color.curves") == 2 &&
                  CanonicalColorStageIndex(graded, "color.saturation") == 3 &&
                  CanonicalColorStageIndex({}, "color.curves") == 0,
              "a new stage goes before the first stage of a greater rank");
        Check(CanonicalColorStageIndex(
                  stack({"color.contrast", "color.exposure"}),
                  "color.shadows") == 2 &&
                  CanonicalColorStageIndex(graded, "color.lut") == 3,
              "a stack in another order, or an unregistered type, takes its "
              "new stage at the end, as before");
    }

    // --- The exact -> float boundary -----------------------------------
    Check(EffectParamValueToFloat({35, 100}) == 0.35f,
          "num/den crosses to float exactly for a clean fraction");
    Check(EffectParamValueToFloat({-5, 100}) == -0.05f,
          "negative numerators cross correctly");
    Check(EffectParamValueToFloat({5600, 1}) == 5600.0f,
          "integral values (kelvin) cross without a denominator");
    Check(EffectParamValueToFloat({1, 0}) == 0.0f,
          "a non-positive denominator resolves to 0 rather than dividing by "
          "it (Document::Validate rejects this shape before it ever "
          "reaches a validated document, this is a defensive fallback)");
    Check(EffectParamValueToFloat({1, -3}) == 0.0f,
          "a negative denominator resolves to 0 the same way");

    // --- ResolveColorGrade: order, defaults, unknown-type skipping ------
    {
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.exposure", {{"amount", {35, 100}}}},
            {kSecond, "color.temperature", {{"kelvin", {5600, 1}}}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects, {});
        Check(grade.count == 2, "both known effects resolve");
        Check(grade.entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::Exposure) &&
                  grade.entries[0].amount == 0.35f,
              "first entry resolves exposure in document order");
        Check(grade.entries[1].kind ==
                      static_cast<int32_t>(ColorEffectKind::Temperature) &&
                  grade.entries[1].amount == 5600.0f,
              "second entry resolves temperature, stack order preserved");
    }
    {
        // A param map missing its expected key falls back to the registry
        // default rather than treating the effect as absent.
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.contrast", {}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects, {});
        Check(grade.count == 1 && grade.entries[0].amount == 0.0f,
              "a missing 'amount' param falls back to the registry default");
    }
    {
        // Types outside the registry (future curves/wheels/LUT, or a typo)
        // are silent no-ops, not errors, and do not consume a slot.
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.exposure", {{"amount", {10, 100}}}},
            {kSecond, "color.curve", {{"points", {1, 1}}}},
            {kThird, "color.contrast", {{"amount", {20, 100}}}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects, {});
        Check(grade.count == 2,
              "an unregistered color.* type is skipped, not counted");
        Check(grade.entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::Exposure) &&
                  grade.entries[1].kind ==
                      static_cast<int32_t>(ColorEffectKind::Contrast),
              "surrounding known effects keep their relative order across "
              "a skipped unknown one");
    }
    {
        // Depth beyond kMaxColorEffectsPerClip is dropped, not reordered.
        std::vector<ClipEffect> effects;
        for (int32_t i = 0; i < kMaxColorEffectsPerClip + 5; ++i) {
            effects.push_back(
                {GenerateUlid(), "color.exposure", {{"amount", {i, 1}}}});
        }
        const ResolvedColorGrade grade = ResolveColorGrade(effects, {});
        Check(grade.count == kMaxColorEffectsPerClip,
              "a stack deeper than kMaxColorEffectsPerClip is truncated");
        Check(grade.entries[0].amount == 0.0f &&
                  grade.entries[kMaxColorEffectsPerClip - 1].amount ==
                      static_cast<float>(kMaxColorEffectsPerClip - 1),
              "truncation keeps the leading entries in document order");
    }
    {
        const ResolvedColorGrade grade = ResolveColorGrade({}, {});
        Check(grade.count == 0, "an empty effect stack resolves to no-op");
    }

    // F1.3 -- the offline reference must retain the monitor's visual
    // directions and stage order before its sampled LUT reaches FFmpeg.
    const auto apply = [](const std::string& type, const std::string& knob,
                          EffectParamValue value, std::array<float, 3> color) {
        return ApplyResolvedColorGrade(
            color, ResolveColorGrade({{kFirst, type, {{knob, value}}}}, {}));
    };
    const std::array<float, 3> middle{0.5f, 0.5f, 0.5f};
    const auto exposure = apply("color.exposure", "amount", {2, 1}, middle);
    Check(exposure[0] > middle[0] && exposure[1] > middle[1] &&
              exposure[2] > middle[2],
          "positive exposure brightens all channels");
    Check(apply("color.contrast", "amount", {1, 2}, {0.2f, 0.2f, 0.2f})[0] <
                  0.2f &&
              apply("color.contrast", "amount", {1, 2}, {0.8f, 0.8f, 0.8f})[0] >
                  0.8f,
          "contrast separates shadows from highlights");
    const auto desaturated =
        apply("color.saturation", "amount", {-1, 1}, {0.3f, 0.5f, 0.7f});
    Check(std::abs(desaturated[0] - desaturated[2]) < 0.00001f,
          "minus one saturation removes chroma");
    const auto lowChroma =
        apply("color.vibrance", "amount", {1, 1}, {0.45f, 0.5f, 0.55f});
    const auto highChroma =
        apply("color.vibrance", "amount", {1, 1}, {0.1f, 0.5f, 0.9f});
    Check(lowChroma[2] - lowChroma[0] > 0.1f &&
              std::abs(highChroma[0] - 0.1f) < 0.00001f,
          "vibrance raises muted color and protects saturated color");
    const auto warmer = apply("color.temperature", "kelvin", {8500, 1}, middle);
    Check(warmer[0] > middle[0] && warmer[2] < middle[2],
          "higher input kelvin compensates toward warm color");
    Check(apply("color.tint", "amount", {1, 1}, middle)[1] < middle[1],
          "positive tint shifts away from green");
    Check(apply("color.highlights", "amount", {1, 1}, {0.8f, 0.8f, 0.8f})[0] <
                  0.8f &&
              std::abs(apply("color.highlights", "amount", {1, 1},
                             {0.1f, 0.1f, 0.1f})[0] -
                       0.1f) < 0.00001f,
          "highlight recovery leaves shadows alone");
    Check(apply("color.shadows", "amount", {1, 1}, {0.1f, 0.1f, 0.1f})[0] >
                  0.1f &&
              std::abs(apply("color.shadows", "amount", {1, 1},
                             {0.8f, 0.8f, 0.8f})[0] -
                       0.8f) < 0.00001f,
          "shadow lift leaves highlights alone");
    const auto exposureThenContrast = ApplyResolvedColorGrade(
        {0.3f, 0.3f, 0.3f},
        ResolveColorGrade(
            {
                {kFirst, "color.exposure", {{"amount", {2, 1}}}},
                {kSecond, "color.contrast", {{"amount", {1, 2}}}},
            },
            {}));
    const auto contrastThenExposure = ApplyResolvedColorGrade(
        {0.3f, 0.3f, 0.3f},
        ResolveColorGrade(
            {
                {kFirst, "color.contrast", {{"amount", {1, 2}}}},
                {kSecond, "color.exposure", {{"amount", {2, 1}}}},
            },
            {}));
    Check(std::abs(exposureThenContrast[0] - contrastThenExposure[0]) > 0.04f,
          "reordering noncommuting stages changes the result");

    // --- G7 color.temperature: a chromatic adaptation ------------------------
    {
        const std::array<ColorGradeSpace, 4> spaces = {
            ColorGradeSpace::Display, ColorGradeSpace::AcesCct,
            ColorGradeSpace::LinearRec709, ColorGradeSpace::LinearRec2020};
        for (ColorGradeSpace space : spaces) {
            const std::array<float, 9> same =
                WhiteBalanceMatrix(4500, 4500, space);
            bool identity = true;
            for (int index = 0; index < 9; ++index)
                identity = identity &&
                           std::abs(same[index] -
                                    (index % 4 == 0 ? 1.0f : 0.0f)) < 1e-6f;
            Check(identity,
                  "the scene lit at the as-shot balance is the "
                  "identity");
        }
        const auto temperature = [&](int32_t kelvin, ColorGradeContext context,
                                     std::array<float, 3> color) {
            return ApplyResolvedColorGrade(
                color,
                ResolveColorGrade(
                    {{kFirst, "color.temperature", {{"kelvin", {kelvin, 1}}}}},
                    context));
        };
        const ColorGradeContext shot5600{ColorGradeSpace::AcesCct, 5600};
        Check(
            ResolveColorGrade(
                {{kFirst, "color.temperature", {{"kelvin", {5600, 1}}}}},
                shot5600)
                        .count == 0 &&
                ResolveColorGrade({{kFirst, "color.temperature", {}}}, shot5600)
                        .count == 0,
            "at the as-shot balance, or without a kelvin, the stage is "
            "neutral and resolves to nothing");
        const std::array<float, 3> grey = {0.4135884f, 0.4135884f, 0.4135884f};
        const auto cooler = temperature(3200, shot5600, grey);
        const auto warmer = temperature(8000, shot5600, grey);
        Check(cooler[2] > cooler[0] && warmer[0] > warmer[2],
              "a lower kelvin cools, a higher one warms");
        // The camera balanced for 5600 K; a grey lit by 3200 K was recorded
        // by the inverse adaptation. Telling the stage 3200 K brings it back
        // to the grey it was.
        for (ColorGradeSpace space :
             {ColorGradeSpace::LinearRec709, ColorGradeSpace::LinearRec2020,
              ColorGradeSpace::AcesCct}) {
            const std::array<float, 9> recorded =
                WhiteBalanceMatrix(5600, 3200, space);
            const float g = 0.18f;
            std::array<float, 3> tinted = {
                (recorded[0] + recorded[1] + recorded[2]) * g,
                (recorded[3] + recorded[4] + recorded[5]) * g,
                (recorded[6] + recorded[7] + recorded[8]) * g};
            const bool log = space == ColorGradeSpace::AcesCct;
            if (log) {
                for (float& channel : tinted)
                    channel = static_cast<float>(EncodeAcesCct(channel));
            }
            std::array<float, 3> fixed =
                temperature(3200, {space, 5600}, tinted);
            if (log) {
                for (float& channel : fixed)
                    channel = static_cast<float>(DecodeAcesCct(channel));
            }
            Check(std::abs(tinted[0] - tinted[2]) > 1e-3f &&
                      std::abs(fixed[0] - g) < 2e-5f &&
                      std::abs(fixed[1] - g) < 2e-5f &&
                      std::abs(fixed[2] - g) < 2e-5f,
                  "a grey lit by tungsten under a 5600 K balance comes back "
                  "grey at 3200 K");
        }
        // Without a known balance, 6500 K stands in.
        Check(ResolveColorGrade(
                  {{kFirst, "color.temperature", {{"kelvin", {6500, 1}}}}}, {})
                      .count == 0,
              "unknown balance: 6500 K is neutral");
    }

    // --- F1.3 wheels and curves: neutral by default -------------------------
    // The eight scalar stages sample into the same cube as before wheels and
    // curves existed (taken from the 4063ced sources, at -O2 and at -O0
    // under ASan alike): a clip without a wheel or a curve exports the same
    // file. G7 (25 September 2026) changed what color.temperature does -- a
    // chromatic adaptation, no longer an offset -- and with it this cube, on
    // purpose. PRD12: pinned to a reference within a tolerance, since its
    // last bits are the compiler's and the system's (CompareCubeReference).
    // `--print-eight-stage-reference` writes the reference again.
    {
        const std::vector<ClipEffect> scalars = EightScalarStages();
        std::string scalarCube;
        std::string cube;
        std::string error;
        const bool built = BuildColorGradeCube(ResolveColorGrade(scalars, {}),
                                               scalarCube, error);
        const std::string mismatch =
            built ? CompareCubeReference(scalarCube,
                                         std::string(COLOR_GRADE_FIXTURES_DIR) +
                                             "/eight_scalar_stages.txt")
                  : error;
        Check(built && scalarCube.size() == 10710466 && mismatch.empty(),
              "the eight scalar stages write the cube they wrote before "
              "wheels and curves: " +
                  mismatch);
        // Exposure, contrast and tint are exact on any machine: no fused
        // multiply-add changes them, and they call no libm function.
        Check(BuildColorGradeCube(
                  ResolveColorGrade(
                      {{kFirst, "color.contrast", {{"amount", {1, 4}}}}}, {}),
                  cube, error) &&
                  Fnv1a(cube) == 0xe2529ac6d2a1918dULL,
              "the contrast stage of the unmanaged export golden writes the "
              "same cube");

        std::vector<ClipEffect> withNeutral = scalars;
        withNeutral.insert(withNeutral.begin() + 3, {kThird,
                                                     "color.wheels",
                                                     {{"lift_master", {0, 1}},
                                                      {"gain_r", {1, 10}},
                                                      {"gain_g", {2, 20}},
                                                      {"gain_b", {1, 10}},
                                                      {"gain_master", {-1, 10}},
                                                      {"gamma_b", {0, 7}}}});
        withNeutral.push_back({kFirst, "color.curves", {}});
        const ResolvedColorGrade neutral = ResolveColorGrade(withNeutral, {});
        // Only the temperature's matrix block is in the pool.
        Check(
            neutral.count == 8 &&
                neutral.data_count == ResolveColorGrade(scalars, {}).data_count,
            "a wheel stage that sums to neutral and a curve stage without "
            "points resolve to nothing");
        Check(BuildColorGradeCube(neutral, cube, error) && cube == scalarCube,
              "so the clip exports as if they were absent");
    }

    // --- Wheels -----------------------------------------------------------
    {
        const std::map<std::string, EffectParamValue> params = {
            {"lift_r", {1, 20}},         {"lift_b", {-1, 25}},
            {"lift_master", {1, 50}},    {"gamma_g", {1, 4}},
            {"gamma_master", {-1, 10}},  {"gain_r", {-1, 5}},
            {"gain_b", {3, 10}},         {"gain_master", {1, 8}},
            {"offset_g", {3, 100}},      {"offset_b", {-1, 40}},
            {"offset_master", {1, 100}},
        };
        const ResolvedColorGrade grade =
            ResolveColorGrade({{kFirst, "color.wheels", params}}, {});
        Check(grade.count == 1 &&
                  grade.entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::Wheels) &&
                  grade.entries[0].data_offset == 0 &&
                  grade.entries[0].data_count == 9 && grade.data_count == 9,
              "a wheel stage resolves to nine values in the pool");
        double worst = 0.0;
        for (const double value :
             {-0.2, -0.01, 0.0, 0.0729, 0.18, 0.4136, 0.55, 0.9, 1.0, 1.3}) {
            const std::array<double, 3> input = {value, value * 0.9 + 0.02,
                                                 value * 1.1 - 0.01};
            const auto actual = ApplyResolvedColorGrade(
                {static_cast<float>(input[0]), static_cast<float>(input[1]),
                 static_cast<float>(input[2])},
                grade);
            const auto expected = WheelReference(params, input);
            for (size_t channel = 0; channel < 3; ++channel) {
                worst = std::max(worst,
                                 std::abs(actual[channel] - expected[channel]));
            }
        }
        Check(worst < 2e-6,
              "the wheels compute the documented lift/gamma/gain/offset "
              "formula, worst " +
                  std::to_string(worst));
    }
    {
        const auto apply = [](const std::string& knob, EffectParamValue value,
                              float input) {
            return ApplyResolvedColorGrade(
                {input, input, input},
                ResolveColorGrade({{kFirst, "color.wheels", {{knob, value}}}},
                                  {}))[0];
        };
        Check(std::abs(apply("lift_master", {1, 10}, 0.0f) - 0.1f) < 1e-6f &&
                  std::abs(apply("lift_master", {1, 10}, 1.0f) - 1.0f) < 1e-6f,
              "lift moves the black end and holds the white one");
        Check(std::abs(apply("gain_master", {1, 1}, 1.0f) - 2.0f) < 1e-6f &&
                  apply("gain_master", {1, 1}, 0.0f) == 0.0f &&
                  std::abs(apply("gain_master", {-1, 1}, 0.5f) - 0.25f) < 1e-6f,
              "gain doubles or halves the white end and holds the black one");
        Check(
            std::abs(apply("gamma_master", {1, 1}, 0.25f) - 0.5f) < 1e-6f &&
                apply("gamma_master", {1, 1}, 0.0f) == 0.0f &&
                std::abs(apply("gamma_master", {1, 1}, 1.0f) - 1.0f) < 1e-6f &&
                apply("gamma_master", {1, 1}, -0.1f) == -0.1f,
            "gamma bends the midtones, holds both ends and leaves "
            "non-positive values alone");
        const auto red = ApplyResolvedColorGrade(
            {0.5f, 0.5f, 0.5f},
            ResolveColorGrade(
                {{kFirst,
                  "color.wheels",
                  {{"gain_r", {1, 2}}, {"gain_master", {-1, 2}}}}},
                {}));
        Check(std::abs(red[0] - 0.5f) < 1e-6f &&
                  std::abs(red[1] - 0.5f * std::exp2(-0.5f)) < 1e-6f,
              "a wheel's colour and master add before the formula");
        // GR2 -- the offset wheel.
        Check(
            std::abs(apply("offset_master", {1, 10}, 0.0f) - 0.1f) < 1e-6f &&
                std::abs(apply("offset_master", {1, 10}, 0.4f) - 0.5f) <
                    1e-6f &&
                std::abs(apply("offset_master", {1, 10}, 1.0f) - 1.1f) < 1e-6f,
            "the offset moves both ends, and every value between, by the "
            "same amount");
        const auto tinted = ApplyResolvedColorGrade(
            {0.3f, 0.3f, 0.3f},
            ResolveColorGrade({{kFirst,
                                "color.wheels",
                                {{"offset_r", {1, 20}},
                                 {"offset_b", {-1, 20}},
                                 {"offset_master", {1, 50}}}}},
                              {}));
        Check(std::abs(tinted[0] - 0.37f) < 1e-6f &&
                  std::abs(tinted[1] - 0.32f) < 1e-6f &&
                  std::abs(tinted[2] - 0.27f) < 1e-6f,
              "the offset's colour and master add, channel by channel");
        // With a lift and a gain, the slope stays K - L: the offset is added
        // to both ends.
        const ResolvedColorGrade both =
            ResolveColorGrade({{kFirst,
                                "color.wheels",
                                {{"lift_master", {1, 10}},
                                 {"gain_master", {1, 1}},
                                 {"offset_master", {-1, 20}}}}},
                              {});
        Check(
            std::abs(ApplyResolvedColorGrade({0.0f, 0.0f, 0.0f}, both)[0] -
                     0.05f) < 1e-6f &&
                std::abs(ApplyResolvedColorGrade({1.0f, 1.0f, 1.0f}, both)[0] -
                         1.95f) < 1e-6f &&
                both.entries[0].data_count == 9,
            "an offset under a lift and a gain moves both of their ends, "
            "in the same nine values");
        Check(
            ResolveColorGrade({{kFirst,
                                "color.wheels",
                                {{"offset_r", {1, 10}},
                                 {"offset_master", {-1, 10}},
                                 {"offset_g", {0, 1}}}}},
                              {})
                        .count == 1 &&
                ResolveColorGrade(
                    {{kFirst, "color.wheels", {{"offset_master", {0, 1}}}}}, {})
                        .count == 0,
            "an offset on one channel is a stage; a neutral one resolves to "
            "nothing");
    }
    {
        std::string error;
        Check(Accepted({kFirst,
                        "color.wheels",
                        {{"lift_r", {-1, 1}}, {"gain_master", {1, 1}}}},
                       error),
              "wheel knobs at their bounds are accepted: " + error);
        Check(!Accepted({kFirst, "color.wheels", {{"gain_g", {101, 100}}}},
                        error) &&
                  error.find("outside [-1, 1]") != std::string::npos,
              "a wheel knob past its bound is refused: " + error);
        Check(
            !Accepted({kFirst, "color.wheels", {{"pivot_r", {1, 10}}}},
                      error) &&
                error.find("unknown parameter 'pivot_r'") != std::string::npos,
            "an unknown wheel knob is refused rather than ignored: " + error);
        Check(Accepted({kFirst,
                        "color.wheels",
                        {{"offset_r", {1, 10}}, {"offset_master", {-1, 1}}}},
                       error) &&
                  !Accepted({kFirst, "color.wheels", {{"offset_b", {-11, 10}}}},
                            error) &&
                  error.find("outside [-1, 1]") != std::string::npos,
              "GR2: the offset wheel's knobs are accepted within [-1, 1]: " +
                  error);
        Check(Accepted({kFirst, "color.exposure", {{"stops", {9, 1}}}}, error),
              "the scalar stages keep their open param maps");
    }

    // --- GR8: the hue curves ------------------------------------------------
    {
        const auto stage = [](std::map<std::string, EffectParamValue> params) {
            return ClipEffect{kFirst, "color.hue_curves", std::move(params)};
        };
        const auto luma = [](const std::array<float, 3>& color) {
            return (0.2126 * color[0]) + (0.7152 * color[1]) +
                   (0.0722 * color[2]);
        };
        const auto chroma = [&](const std::array<float, 3>& color) {
            const double y = luma(color);
            return std::hypot((color[0] - y) / 1.5748, (color[2] - y) / 1.8556);
        };
        // The hue as a fraction of a turn, from +Cb toward +Cr.
        const auto hue = [&](const std::array<float, 3>& color) {
            const double y = luma(color);
            double turn =
                std::atan2((color[0] - y) / 1.5748, (color[2] - y) / 1.8556) /
                6.283185307179586;
            return turn - std::floor(turn);
        };
        // A band of a twelfth of a turn either side of `centre`, in
        // thousandths.
        const auto band = [](const std::string& curve, int32_t centre,
                             EffectParamValue value) {
            return std::map<std::string, EffectParamValue>{
                {curve + "_x0", {centre - 83, 1000}}, {curve + "_y0", {1, 2}},
                {curve + "_x1", {centre, 1000}},      {curve + "_y1", value},
                {curve + "_x2", {centre + 83, 1000}}, {curve + "_y2", {1, 2}}};
        };
        const auto apply = [](const ClipEffect& effect,
                              std::array<float, 3> color) {
            return ApplyResolvedColorGrade(color,
                                           ResolveColorGrade({effect}, {}));
        };
        const std::array<float, 3> orange = {0.8f, 0.45f, 0.2f};
        const std::array<float, 3> blue = {0.2f, 0.3f, 0.8f};
        const auto at = static_cast<int32_t>(std::lround(hue(orange) * 1000));
        std::string error;

        // Validation.
        Check(Accepted(stage(band("hue_sat", at, {0, 1})), error) &&
                  Accepted(stage({}), error),
              "a band on a hue curve, and a stage without a point, are "
              "accepted: " +
                  error);
        Check(
            !Accepted(stage({{"hue_x0", {0, 1}}}), error) &&
                error.find("unknown parameter 'hue_x0'") != std::string::npos &&
                !Accepted(stage({{"master_x0", {0, 1}}}), error),
            "a param that names no hue curve is refused: " + error);
        Check(!Accepted(stage({{"hue_sat_x0", {1, 2}}, {"hue_sat_y0", {0, 1}}}),
                        error) &&
                  error.find("color.hue_curves curve 'hue_sat' has a single "
                             "point") != std::string::npos,
              "a curve of one point is refused, by the stage's name: " + error);
        Check(!Accepted(stage({{"hue_hue_x0", {0, 1}},
                               {"hue_hue_y0", {1, 2}},
                               {"hue_hue_x1", {1, 1}},
                               {"hue_hue_y1", {1, 2}}}),
                        error) &&
                  error.find("x = 1, which is the hue 0 again") !=
                      std::string::npos &&
                  Accepted(stage({{"sat_sat_x0", {0, 1}},
                                  {"sat_sat_y0", {1, 2}},
                                  {"sat_sat_x1", {1, 1}},
                                  {"sat_sat_y1", {1, 2}}}),
                           error),
              "a hue of 1 is the hue 0 again, refused on a curve along the "
              "hue only: " +
                  error);

        // Neutral.
        Check(ResolveColorGrade({stage({})}, {}).count == 0,
              "a stage without a point resolves to nothing");
        const ClipEffect flat = stage(band("hue_hue", 500, {1, 2}));
        const ResolvedColorGrade flatGrade = ResolveColorGrade({flat}, {});
        const std::array<float, 3> same =
            ApplyResolvedColorGrade(orange, flatGrade);
        Check(flatGrade.count == 1 &&
                  flatGrade.entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::HueCurves) &&
                  flatGrade.entries[0].data_count == 1 + (3 * 5) + 5 &&
                  std::abs(same[0] - orange[0]) < 1e-6f &&
                  std::abs(same[1] - orange[1]) < 1e-6f &&
                  std::abs(same[2] - orange[2]) < 1e-6f,
              "a curve at 1/2 changes nothing; three points resolve to five, "
              "the five other curves to one zero each");

        // Hue against saturation: one colour drained, the others kept.
        const ClipEffect drained = stage(band("hue_sat", at, {0, 1}));
        const std::array<float, 3> grey = apply(drained, orange);
        const std::array<float, 3> kept = apply(drained, blue);
        Check(chroma(grey) < 1e-3 && std::abs(luma(grey) - luma(orange)) < 1e-6,
              "hue_sat at 0 on a colour's hue drains it, luma kept (chroma " +
                  std::to_string(chroma(grey)) + ")");
        Check(std::abs(kept[0] - blue[0]) < 1e-6f &&
                  std::abs(kept[1] - blue[1]) < 1e-6f &&
                  std::abs(kept[2] - blue[2]) < 1e-6f,
              "and leaves a colour outside the band as it was");
        const std::array<float, 3> doubled =
            apply(stage(band("hue_sat", at, {1, 1})), orange);
        Check(std::abs(chroma(doubled) - (2.0 * chroma(orange))) < 2e-3 &&
                  std::abs(hue(doubled) - hue(orange)) < 1e-4,
              "hue_sat at 1 doubles its saturation and keeps its hue");

        // Hue against hue: a quarter turn.
        const std::array<float, 3> turned =
            apply(stage(band("hue_hue", at, {3, 4})), orange);
        double moved = hue(turned) - hue(orange) - 0.25;
        moved -= std::round(moved);
        Check(std::abs(moved) < 2e-3 &&
                  std::abs(chroma(turned) - chroma(orange)) < 1e-6 &&
                  std::abs(luma(turned) - luma(orange)) < 1e-6,
              "hue_hue at 3/4 turns the colour a quarter turn, chroma and "
              "luma kept (off by " +
                  std::to_string(moved) + " of a turn)");

        // Hue against luma: a colour is lit, a grey is not.
        const ClipEffect lit = stage(band("hue_lum", at, {7, 10}));
        const std::array<float, 3> brighter = apply(lit, orange);
        const std::array<float, 3> stillGrey = apply(lit, {0.4f, 0.4f, 0.4f});
        Check(std::abs(luma(brighter) - luma(orange) - 0.2) < 1e-3 &&
                  std::abs(chroma(brighter) - chroma(orange)) < 1e-6 &&
                  std::abs(stillGrey[0] - 0.4f) < 1e-6f &&
                  std::abs(stillGrey[2] - 0.4f) < 1e-6f,
              "hue_lum adds to a colour's luma and leaves a grey, which has "
              "no hue, alone");

        // The curve goes round: across the seam at hue 0 it is continuous.
        const ClipEffect seam = stage({{"hue_sat_x0", {20, 1000}},
                                       {"hue_sat_y0", {1, 4}},
                                       {"hue_sat_x1", {100, 1000}},
                                       {"hue_sat_y1", {1, 2}},
                                       {"hue_sat_x2", {950, 1000}},
                                       {"hue_sat_y2", {1, 2}}});
        const ResolvedColorGrade seamGrade = ResolveColorGrade({seam}, {});
        const float* block =
            seamGrade.data.data() + seamGrade.entries[0].data_offset + 1;
        const float before = EvaluateHueCurve(block, 0.9999f);
        const float after = EvaluateHueCurve(block, 0.0001f);
        const float middle = EvaluateHueCurve(block, 0.985f);
        Check(std::abs(before - after) < 1e-3f && before > 0.25f &&
                  before < 0.5f && middle > before &&
                  std::abs(EvaluateHueCurve(block, 0.02f) - 0.25f) < 1e-6f &&
                  std::abs(EvaluateHueCurve(block, 0.5f) - 0.5f) < 1e-6f,
              "a curve along the hue goes round: the same value either side "
              "of hue 0, between its last point and its first (" +
                  std::to_string(before) + ", " + std::to_string(after) + ")");

        // Luma against saturation: the shadows drained, the rest kept, and
        // the ends held past the points.
        const ClipEffect shadows = stage({{"lum_sat_x0", {1, 10}},
                                          {"lum_sat_y0", {0, 1}},
                                          {"lum_sat_x1", {1, 2}},
                                          {"lum_sat_y1", {1, 2}}});
        const std::array<float, 3> dark = {0.09f, 0.03f, 0.02f};
        const std::array<float, 3> bright = {1.2f, 0.9f, 0.7f};
        Check(chroma(apply(shadows, dark)) < 1e-6 &&
                  std::abs(chroma(apply(shadows, bright)) - chroma(bright)) <
                      1e-6 &&
                  std::abs(chroma(apply(shadows, orange)) - chroma(orange)) <
                      1e-6,
              "lum_sat drains below its first point, which holds, and keeps "
              "what lies past its last");

        // Saturation against saturation: a vibrance.
        const ClipEffect vibrance = stage({{"sat_sat_x0", {0, 1}},
                                           {"sat_sat_y0", {1, 1}},
                                           {"sat_sat_x1", {1, 1}},
                                           {"sat_sat_y1", {1, 2}}});
        const std::array<float, 3> pale = {0.5f, 0.45f, 0.42f};
        const std::array<float, 3> primary = {1.0f, 0.0f, 0.0f};
        Check(chroma(apply(vibrance, pale)) > 1.8 * chroma(pale) &&
                  std::abs(chroma(apply(vibrance, primary)) - chroma(primary)) <
                      1e-3,
              "sat_sat lifts a pale colour and leaves a pure one");

        // Saturation against luma: the vivid colours darkened.
        const ClipEffect dense = stage({{"sat_lum_x0", {0, 1}},
                                        {"sat_lum_y0", {1, 2}},
                                        {"sat_lum_x1", {1, 1}},
                                        {"sat_lum_y1", {1, 4}}});
        Check(std::abs(luma(apply(dense, primary)) - luma(primary) + 0.25) <
                      1e-3 &&
                  std::abs(luma(apply(dense, {0.4f, 0.4f, 0.4f})) - 0.4) < 1e-6,
              "sat_lum darkens a pure colour by its curve and leaves a grey");

        // Sixteen points on every curve fit the pool, with the rest.
        std::map<std::string, EffectParamValue> full;
        for (const char* curve : kHueCurveNames) {
            for (int point = 0; point < kMaxColorCurvePoints; ++point) {
                const std::string number = std::to_string(point);
                full[std::string(curve) + "_x" + number] = {point, 16};
                full[std::string(curve) + "_y" + number] = {1, 2};
            }
        }
        const ResolvedColorGrade fullGrade =
            ResolveColorGrade({stage(full)}, {});
        Check(Accepted(stage(full), error) && fullGrade.count == 1 &&
                  fullGrade.entries[0].data_count == 6 * (1 + (3 * 18)) &&
                  ColorStackFits({stage(full)}, error),
              "six curves of sixteen points take 330 values: " + error);

        // What the export's cube makes of a narrow band: trilinear between
        // 65 samples a side, against the stage itself.
        std::string cube;
        const ResolvedColorGrade narrow = ResolveColorGrade({drained}, {});
        Check(BuildColorGradeCube(narrow, cube, error),
              "a hue curve builds its cube: " + error);
        std::vector<float> table;
        {
            std::istringstream lines(cube);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty() || (line[0] != '-' && line[0] != '.' &&
                                     (line[0] < '0' || line[0] > '9'))) {
                    continue;
                }
                std::istringstream values(line);
                float r = 0.0f;
                float g = 0.0f;
                float b = 0.0f;
                values >> r >> g >> b;
                table.insert(table.end(), {r, g, b});
            }
        }
        constexpr int kEdge = 65;
        Check(table.size() == static_cast<size_t>(kEdge * kEdge * kEdge * 3),
              "the cube holds 65 samples a side");
        if (table.size() == static_cast<size_t>(kEdge * kEdge * kEdge * 3)) {
            const auto sample = [&](int r, int g, int b, int channel) {
                return table[static_cast<size_t>(
                    ((((b * kEdge) + g) * kEdge + r) * 3) + channel)];
            };
            const auto interpolate = [&](const std::array<float, 3>& color) {
                std::array<float, 3> out{};
                std::array<int, 3> low{};
                std::array<float, 3> part{};
                for (size_t axis = 0; axis < 3; ++axis) {
                    const float scaled = color[axis] * (kEdge - 1);
                    low[axis] = std::min(static_cast<int>(scaled), kEdge - 2);
                    part[axis] = scaled - static_cast<float>(low[axis]);
                }
                for (int channel = 0; channel < 3; ++channel) {
                    float sum = 0.0f;
                    for (int corner = 0; corner < 8; ++corner) {
                        const int dr = corner & 1;
                        const int dg = (corner >> 1) & 1;
                        const int db = (corner >> 2) & 1;
                        sum += (dr != 0 ? part[0] : 1.0f - part[0]) *
                               (dg != 0 ? part[1] : 1.0f - part[1]) *
                               (db != 0 ? part[2] : 1.0f - part[2]) *
                               sample(low[0] + dr, low[1] + dg, low[2] + db,
                                      channel);
                    }
                    out[static_cast<size_t>(channel)] = sum;
                }
                return out;
            };
            std::mt19937 random(11);
            std::uniform_real_distribution<float> value(0.0f, 1.0f);
            double worst = 0.0;
            double total = 0.0;
            constexpr int kSamples = 20000;
            for (int index = 0; index < kSamples; ++index) {
                const std::array<float, 3> color = {
                    value(random), value(random), value(random)};
                const std::array<float, 3> exact =
                    ApplyResolvedColorGrade(color, narrow);
                const std::array<float, 3> cubed = interpolate(color);
                for (size_t channel = 0; channel < 3; ++channel) {
                    const double gap =
                        std::abs(std::clamp(exact[channel], 0.0f, 1.0f) -
                                 cubed[channel]);
                    worst = std::max(worst, gap);
                    total += gap;
                }
            }
            std::cout << "hue curve in the export's cube: worst " << worst
                      << ", mean " << total / (kSamples * 3.0) << " over "
                      << kSamples << " colours\n";
            // Measured 0.0107 and 0.00009 when written: the worst is where
            // the band's edge crosses a cell near the grey axis. The bound
            // is the tolerance the monitor and the export are held to.
            Check(worst < 0.02 && total / (kSamples * 3.0) < 0.0005,
                  "a band a sixth of a turn wide survives the 65-sample cube "
                  "(worst " +
                      std::to_string(worst) + ")");
        }
    }

    // --- GR7: a stack longer than a clip's grade renders -------------------
    {
        std::string error;
        std::vector<ClipEffect> stack;
        for (int index = 0; index < kMaxColorEffectsPerClip; ++index) {
            stack.push_back(
                {kFirst, "color.exposure", {{"amount", {index + 1, 100}}}});
        }
        Check(ColorStackFits(stack, error) && error.empty() &&
                  ResolveColorGrade(stack, {}).dropped_from == -1 &&
                  ColorStackFits({}, error),
              "sixteen stages are a stack the grade renders whole: " + error);
        stack.push_back({kSecond, "color.transform", {}});
        stack.push_back({kSecond, "color.contrast", {{"amount", {1, 10}}}});
        stack.push_back({kThird, "color.saturation", {{"amount", {1, 10}}}});
        Check(
            !ColorStackFits(stack, error) &&
                ResolveColorGrade(stack, {}).dropped_from == 17 &&
                ResolveColorGrade(stack, {}).count == kMaxColorEffectsPerClip &&
                error.find("stage 18 (color.contrast)") != std::string::npos &&
                error.find("at most 16 stages and 768") != std::string::npos,
            "a seventeenth stage is left out, and named: " + error);
        // The pool: a full curve stage takes 196 values, the fourth does
        // not fit.
        std::map<std::string, EffectParamValue> full;
        for (const char* channel : kColorCurveChannels) {
            for (int point = 0; point < kMaxColorCurvePoints; ++point) {
                const std::string number = std::to_string(point);
                full[std::string(channel) + "_x" + number] = {point, 15};
                full[std::string(channel) + "_y" + number] = {point, 15};
            }
        }
        std::vector<ClipEffect> curves(3, {kFirst, "color.curves", full});
        Check(ColorStackFits(curves, error),
              "three full curve stages fit: " + error);
        curves.push_back({kSecond, "color.curves", full});
        Check(!ColorStackFits(curves, error) &&
                  error.find("stage 4 (color.curves)") != std::string::npos,
              "the fourth does not, and is named: " + error);
        // A white balance counts whatever the media it lands on.
        std::vector<ClipEffect> balanced(
            15, {kFirst, "color.exposure", {{"amount", {1, 10}}}});
        balanced.push_back(
            {kSecond, "color.temperature", {{"kelvin", {6500, 1}}}});
        balanced.push_back({kThird, "color.tint", {{"amount", {1, 10}}}});
        Check(ResolveColorGrade(balanced, {}).dropped_from == -1 &&
                  !ColorStackFits(balanced, error) &&
                  error.find("stage 17 (color.tint)") != std::string::npos,
              "a white balance neutral on one media is counted: the stack "
              "has to fit on every media: " +
                  error);
        // A neutral stage takes no room.
        std::vector<ClipEffect> padded(
            16, {kFirst, "color.exposure", {{"amount", {1, 10}}}});
        padded.insert(padded.begin(), {kSecond, "color.wheels", {}});
        padded.insert(padded.begin(),
                      {kThird, "color.hue", {{"degrees", {0, 1}}}});
        Check(ColorStackFits(padded, error),
              "neutral wheels and a hue at zero take no stage: " + error);
    }

    // --- GR6: the contrast's pivot ------------------------------------------
    {
        const auto contrast = [](std::map<std::string, EffectParamValue> params,
                                 float input) {
            return ApplyResolvedColorGrade(
                {input, input, input},
                ResolveColorGrade(
                    {{kFirst, "color.contrast", std::move(params)}}, {}))[0];
        };
        Check(
            std::abs(contrast({{"amount", {1, 2}}, {"pivot", {1, 4}}}, 0.25f) -
                     0.25f) < 1e-7f &&
                std::abs(
                    contrast({{"amount", {1, 2}}, {"pivot", {1, 4}}}, 0.45f) -
                    0.55f) < 1e-6f &&
                std::abs(
                    contrast({{"amount", {-1, 2}}, {"pivot", {3, 4}}}, 0.25f) -
                    0.5f) < 1e-6f,
            "a contrast with a pivot holds that value and scales the "
            "others about it");
        const ResolvedColorGrade plain = ResolveColorGrade(
            {{kFirst, "color.contrast", {{"amount", {1, 2}}}}}, {});
        const ResolvedColorGrade own =
            ResolveColorGrade({{kFirst,
                                "color.contrast",
                                {{"amount", {1, 2}}, {"pivot", {1, 4}}}}},
                              {});
        Check(plain.count == 1 && plain.data_count == 0 &&
                  plain.entries[0].data_count == 0 && own.count == 1 &&
                  own.entries[0].data_count == 1 && own.data_count == 1 &&
                  own.data[0] == 0.25f &&
                  std::abs(ApplyResolvedColorGrade({kContrastPivot, 0.2f, 0.9f},
                                                   plain)[0] -
                           kContrastPivot) < 1e-7f,
              "without a pivot the stage resolves as it always did, about "
              "middle grey; with one it carries it in the pool");
        std::string error;
        Check(Accepted({kFirst,
                        "color.contrast",
                        {{"amount", {1, 2}}, {"pivot", {1, 1}}}},
                       error) &&
                  !Accepted({kFirst,
                             "color.contrast",
                             {{"amount", {1, 2}}, {"pivot", {11, 10}}}},
                            error) &&
                  error.find("'pivot' (11/10) is outside [0, 1]") !=
                      std::string::npos,
              "a pivot is a value of the picture, in [0, 1]: " + error);
    }

    // --- GR6: the hue rotation ----------------------------------------------
    {
        const auto hue = [](EffectParamValue degrees) {
            return ResolveColorGrade(
                {{kFirst, "color.hue", {{"degrees", degrees}}}}, {});
        };
        Check(hue({0, 1}).count == 0 && hue({360, 1}).count == 0 &&
                  hue({-720, 1}).count == 0 && hue({1, 10}).count == 1 &&
                  hue({1, 10}).entries[0].kind ==
                      static_cast<int32_t>(ColorEffectKind::Hue) &&
                  hue({1, 10}).entries[0].data_count == 9,
              "a whole turn is no stage; anything else is nine values");
        const auto luma = [](const std::array<float, 3>& color) {
            return (0.2126 * color[0]) + (0.7152 * color[1]) +
                   (0.0722 * color[2]);
        };
        const auto angle = [&](const std::array<float, 3>& color) {
            const double y = luma(color);
            return std::atan2((color[0] - y) / 1.5748,
                              (color[2] - y) / 1.8556) *
                   180.0 / 3.14159265358979;
        };
        const auto chroma = [&](const std::array<float, 3>& color) {
            const double y = luma(color);
            return std::hypot((color[0] - y) / 1.5748, (color[2] - y) / 1.8556);
        };
        double worstAngle = 0.0;
        double worstLuma = 0.0;
        double worstChroma = 0.0;
        for (const std::array<float, 3>& color :
             {std::array<float, 3>{0.8f, 0.2f, 0.1f},
              std::array<float, 3>{0.1f, 0.6f, 0.3f},
              std::array<float, 3>{0.2f, 0.25f, 0.9f},
              std::array<float, 3>{1.2f, -0.05f, 0.4f}}) {
            for (const int degrees : {30, 90, -45, 200}) {
                const std::array<float, 3> turned =
                    ApplyResolvedColorGrade(color, hue({degrees, 1}));
                double moved = angle(turned) - angle(color) - degrees;
                moved -= 360.0 * std::round(moved / 360.0);
                worstAngle = std::max(worstAngle, std::abs(moved));
                worstLuma =
                    std::max(worstLuma, std::abs(luma(turned) - luma(color)));
                worstChroma = std::max(
                    worstChroma, std::abs(chroma(turned) - chroma(color)));
            }
        }
        Check(worstAngle < 1e-3 && worstLuma < 1e-6 && worstChroma < 1e-6,
              "a rotation moves every hue by its angle, counted from +Cb "
              "toward +Cr, and keeps luma and chroma (angle " +
                  std::to_string(worstAngle) + ", luma " +
                  std::to_string(worstLuma) + ")");
        const std::array<float, 3> grey =
            ApplyResolvedColorGrade({0.4f, 0.4f, 0.4f}, hue({77, 1}));
        Check(std::abs(grey[0] - 0.4f) < 1e-6f &&
                  std::abs(grey[1] - 0.4f) < 1e-6f &&
                  std::abs(grey[2] - 0.4f) < 1e-6f,
              "a grey has no hue to turn");
        const std::array<float, 3> yellowed =
            ApplyResolvedColorGrade({1.0f, 0.0f, 0.0f}, hue({72, 1}));
        Check(yellowed[0] > yellowed[2] && yellowed[1] > yellowed[2] &&
                  yellowed[1] > 0.1f,
              "a positive rotation takes red toward yellow");
        const std::array<float, 3> back = ApplyResolvedColorGrade(
            {0.7f, 0.3f, 0.2f},
            ResolveColorGrade({{kFirst, "color.hue", {{"degrees", {55, 1}}}},
                               {kSecond, "color.hue", {{"degrees", {-55, 1}}}}},
                              {}));
        Check(std::abs(back[0] - 0.7f) < 1e-6f &&
                  std::abs(back[1] - 0.3f) < 1e-6f &&
                  std::abs(back[2] - 0.2f) < 1e-6f,
              "a rotation and its opposite give the picture back");
        const std::array<float, 9> identity = HueRotationMatrix(0.0);
        Check(std::abs(identity[0] - 1.0f) < 1e-7f &&
                  std::abs(identity[4] - 1.0f) < 1e-7f &&
                  std::abs(identity[8] - 1.0f) < 1e-7f &&
                  std::abs(identity[1]) < 1e-7f &&
                  std::abs(identity[5]) < 1e-7f,
              "no rotation is the identity");
    }

    // --- Curves: validation ------------------------------------------------
    {
        std::string error;
        Check(Accepted(Curves({{"master_x0", {0, 1}},
                               {"master_y0", {0, 1}},
                               {"master_x1", {1, 2}},
                               {"master_y1", {3, 5}},
                               {"master_x2", {1, 1}},
                               {"master_y2", {1, 1}},
                               {"b_x0", {0, 1}},
                               {"b_y0", {1, 10}},
                               {"b_x1", {1, 1}},
                               {"b_y1", {9, 10}}}),
                       error),
              "a master curve and a blue curve are accepted: " + error);
        Check(!Accepted(Curves({{"r_x0", {1, 2}},
                                {"r_y0", {1, 2}},
                                {"r_x1", {1, 4}},
                                {"r_y1", {1, 4}}}),
                        error) &&
                  error.find("not increasing in x") != std::string::npos,
              "a curve that goes back in x is refused: " + error);
        Check(
            !Accepted(Curves({{"r_x0", {1, 2}},
                              {"r_y0", {1, 2}},
                              {"r_x1", {2, 4}},
                              {"r_y1", {3, 4}}}),
                      error) &&
                error.find("not increasing in x") != std::string::npos,
            "two points on the same x are refused, compared exactly: " + error);
        Check(!Accepted(Curves({{"b_x0", {1, 3}},
                                {"b_y0", {1, 2}},
                                {"b_x1", {333333334, 1000000000}},
                                {"b_y1", {1, 2}}}),
                        error) &&
                  error.find("too close") != std::string::npos,
              "two x the renderer cannot tell apart are refused: " + error);
        Check(!Accepted(Curves({{"g_x0", {1, 2}}, {"g_y0", {1, 2}}}), error) &&
                  error.find("single point") != std::string::npos,
              "a one-point curve is refused: " + error);
        Check(!Accepted(Curves({{"g_x0", {0, 1}},
                                {"g_y0", {0, 1}},
                                {"g_x2", {1, 1}},
                                {"g_y2", {1, 1}}}),
                        error) &&
                  error.find("no point 1") != std::string::npos,
              "a gap in the point numbers is refused: " + error);
        Check(
            !Accepted(
                Curves({{"g_x0", {0, 1}}, {"g_y0", {0, 1}}, {"g_x1", {1, 1}}}),
                error) &&
                error.find("g_x1 without g_y1") != std::string::npos,
            "a point without its y is refused: " + error);
        Check(!Accepted(Curves({{"master_x0", {0, 1}},
                                {"master_y0", {-1, 10}},
                                {"master_x1", {1, 1}},
                                {"master_y1", {1, 1}}}),
                        error) &&
                  error.find("outside [0, 1]") != std::string::npos,
              "a point outside the unit square is refused: " + error);
        Check(!Accepted(Curves({{"red_x0", {0, 1}}}), error) &&
                  !Accepted(Curves({{"r_x00", {0, 1}}}), error) &&
                  !Accepted(Curves({{"r_x16", {0, 1}}}), error) &&
                  !Accepted(Curves({{"r_z0", {0, 1}}}), error) &&
                  error.find("unknown parameter") != std::string::npos,
              "names outside <channel>_<x|y><0..15> are refused: " + error);
        std::map<std::string, EffectParamValue> sixteen;
        for (int32_t index = 0; index < kMaxColorCurvePoints; ++index) {
            sixteen["r_x" + std::to_string(index)] = {index, 15};
            sixteen["r_y" + std::to_string(index)] = {index, 15};
        }
        Check(Accepted(Curves(sixteen), error),
              "sixteen points are accepted: " + error);
    }

    // --- Curves: interpolation --------------------------------------------
    {
        // Through every point, and monotone without overshoot on random
        // rising data and on data that rises then falls.
        std::mt19937 random(20260918);
        std::uniform_int_distribution<int32_t> step(1, 40);
        double worstOvershoot = 0.0;
        double worstMiss = 0.0;
        bool monotone = true;
        for (int32_t trial = 0; trial < 200; ++trial) {
            const int32_t count = 2 + (trial % (kMaxColorCurvePoints - 1));
            std::vector<int32_t> xs(static_cast<size_t>(count));
            std::vector<int32_t> ys(static_cast<size_t>(count));
            int32_t x = 0;
            int32_t y = 0;
            for (int32_t index = 0; index < count; ++index) {
                x += step(random);
                const bool rising = trial % 2 == 0 || index < count / 2;
                y += (rising ? 1 : -1) *
                     (step(random) % 3 == 0 ? 0 : step(random));
                xs[static_cast<size_t>(index)] = x;
                ys[static_cast<size_t>(index)] = y;
            }
            const int32_t low = *std::min_element(ys.begin(), ys.end());
            const int32_t high = *std::max_element(ys.begin(), ys.end());
            std::map<std::string, EffectParamValue> params;
            for (int32_t index = 0; index < count; ++index) {
                params["master_x" + std::to_string(index)] = {
                    xs[static_cast<size_t>(index)], x};
                params["master_y" + std::to_string(index)] = {
                    ys[static_cast<size_t>(index)] - low,
                    std::max(1, high - low)};
            }
            std::string error;
            if (!Accepted(Curves(params), error)) {
                Check(false, "random curve validates: " + error);
                continue;
            }
            const ResolvedColorGrade grade =
                ResolveColorGrade({Curves(params)}, {});
            const float* block =
                grade.data.data() + grade.entries[0].data_offset;
            for (int32_t index = 0; index < count; ++index) {
                const float px = block[1 + (index * 3)];
                const float py = block[2 + (index * 3)];
                worstMiss = std::max(worstMiss,
                                     static_cast<double>(std::abs(
                                         EvaluateColorCurve(block, px) - py)));
            }
            for (int32_t index = 0; index + 1 < count; ++index) {
                const float x0 = block[1 + (index * 3)];
                const float x1 = block[4 + (index * 3)];
                const float y0 = block[2 + (index * 3)];
                const float y1 = block[5 + (index * 3)];
                float previous = y0;
                for (int32_t sample = 1; sample <= 64; ++sample) {
                    const float at = x0 + ((x1 - x0) * sample / 64.0f);
                    const float value = EvaluateColorCurve(block, at);
                    worstOvershoot = std::max(
                        {worstOvershoot,
                         static_cast<double>(value - std::max(y0, y1)),
                         static_cast<double>(std::min(y0, y1) - value)});
                    if ((y1 >= y0 && value < previous - 1e-6f) ||
                        (y1 < y0 && value > previous + 1e-6f)) {
                        monotone = false;
                    }
                    previous = value;
                }
            }
        }
        Check(worstMiss < 1e-6, "the curve goes through every point, worst " +
                                    std::to_string(worstMiss));
        Check(worstOvershoot < 1e-6,
              "no interval overshoots its two points, worst " +
                  std::to_string(worstOvershoot));
        Check(monotone, "each interval follows the direction of its points");
        std::cout << "curves: worst miss " << worstMiss << ", worst overshoot "
                  << worstOvershoot << '\n';
    }
    {
        // A steep step: a plain Catmull-Rom or natural spline would ring
        // below 0.2 and above 0.8 here.
        const ResolvedColorGrade grade =
            ResolveColorGrade({Curves({
                                  {"master_x0", {0, 1}},
                                  {"master_y0", {1, 5}},
                                  {"master_x1", {2, 5}},
                                  {"master_y1", {1, 5}},
                                  {"master_x2", {1, 2}},
                                  {"master_y2", {4, 5}},
                                  {"master_x3", {1, 1}},
                                  {"master_y3", {4, 5}},
                              })},
                              {});
        const float* block = grade.data.data();
        bool flat = true;
        for (int32_t sample = 0; sample <= 100; ++sample) {
            const float at = sample / 100.0f;
            const float value = EvaluateColorCurve(block, at);
            flat = flat && value >= 0.2f - 1e-6f && value <= 0.8f + 1e-6f &&
                   (at > 0.4f || std::abs(value - 0.2f) < 1e-6f) &&
                   (at < 0.5f || std::abs(value - 0.8f) < 1e-6f);
        }
        Check(flat, "flat runs stay flat on either side of a steep step");
        // Past the ends: a straight line along the end tangent (zero here).
        Check(std::abs(EvaluateColorCurve(block, -0.3f) - 0.2f) < 1e-6f &&
                  std::abs(EvaluateColorCurve(block, 1.4f) - 0.8f) < 1e-6f,
              "past a flat end the curve stays level");
    }
    {
        const ResolvedColorGrade grade =
            ResolveColorGrade({Curves({
                                  {"r_x0", {1, 10}},
                                  {"r_y0", {1, 5}},
                                  {"r_x1", {1, 2}},
                                  {"r_y1", {3, 5}},
                                  {"r_x2", {9, 10}},
                                  {"r_y2", {7, 10}},
                              })},
                              {});
        // Blocks: master (no point), then r.
        const float* red = grade.data.data() + 1;
        const float slopeEnd = red[1 + 8];
        const float slopeStart = red[1 + 2];
        Check(std::abs(EvaluateColorCurve(red, 1.2f) -
                       (0.7f + slopeEnd * 0.3f)) < 1e-6f &&
                  std::abs(EvaluateColorCurve(red, 0.0f) -
                           (0.2f - slopeStart * 0.1f)) < 1e-6f &&
                  slopeEnd > 0.0f && slopeStart > 0.0f,
              "past the end points the curve goes on along its end "
              "tangents, keeping the order of out-of-range values");
        const auto graded = ApplyResolvedColorGrade({0.5f, 0.5f, 0.5f}, grade);
        Check(std::abs(graded[0] - 0.6f) < 1e-6f && graded[1] == 0.5f &&
                  graded[2] == 0.5f,
              "a red curve moves the red channel only");
    }
    {
        // Master first, then the channel's own curve.
        const std::map<std::string, EffectParamValue> master = {
            {"master_x0", {0, 1}}, {"master_y0", {0, 1}},
            {"master_x1", {1, 2}}, {"master_y1", {7, 10}},
            {"master_x2", {1, 1}}, {"master_y2", {1, 1}}};
        std::map<std::string, EffectParamValue> both = master;
        both.insert({{"g_x0", {0, 1}},
                     {"g_y0", {1, 10}},
                     {"g_x1", {1, 1}},
                     {"g_y1", {1, 1}}});
        const auto masterOnly = ApplyResolvedColorGrade(
            {0.5f, 0.5f, 0.3f}, ResolveColorGrade({Curves(master)}, {}));
        const auto combined = ApplyResolvedColorGrade(
            {0.5f, 0.5f, 0.3f}, ResolveColorGrade({Curves(both)}, {}));
        Check(std::abs(masterOnly[0] - 0.7f) < 1e-6f &&
                  std::abs(combined[1] - (0.1f + 0.9f * 0.7f)) < 1e-6f &&
                  combined[0] == masterOnly[0] && combined[2] == masterOnly[2],
              "the master curve runs before the channel curves");
    }

    // --- The pool and the stack limits -------------------------------------
    {
        std::map<std::string, EffectParamValue> full;
        for (const char* channel : kColorCurveChannels) {
            for (int32_t index = 0; index < kMaxColorCurvePoints; ++index) {
                full[std::string(channel) + "_x" + std::to_string(index)] = {
                    index, kMaxColorCurvePoints - 1};
                full[std::string(channel) + "_y" + std::to_string(index)] = {
                    index, kMaxColorCurvePoints};
            }
        }
        const std::vector<ClipEffect> effects = {
            {kFirst, "color.wheels", {{"lift_r", {1, 10}}}},
            Curves(full),
            Curves(full),
            Curves(full),
            {kSecond, "color.exposure", {{"amount", {1, 1}}}},
            Curves(full),
            {kThird, "color.contrast", {{"amount", {1, 1}}}},
        };
        const ResolvedColorGrade grade = ResolveColorGrade(effects, {});
        Check(grade.count == 5 && grade.data_count == 9 + (3 * 196) &&
                  grade.entries[4].kind ==
                      static_cast<int32_t>(ColorEffectKind::Exposure),
              "a stage that no longer fits the pool ends the stack there, "
              "like the depth limit");
    }

    // --- What Renderer.mm sends ---------------------------------------------
    {
        Check(offsetof(GpuClipGradeParameters, entries) == 16 &&
                  offsetof(GpuClipGradeParameters, data) == 16 + (16 * 16) &&
                  sizeof(GpuClipGradeParameters) == 16 + (16 * 16) + (768 * 4),
              "the packed grade has shader.metal's ClipGradeParameters "
              "layout");
        const ResolvedColorGrade grade = ResolveColorGrade(
            {
                {kFirst, "color.exposure", {{"amount", {1, 2}}}},
                {kSecond, "color.wheels", {{"gain_b", {1, 4}}}},
            },
            {});
        const GpuClipGradeParameters packed = PackColorGradeForGpu(grade);
        Check(packed.count == 2 && packed.entries[0].amount == 0.5f &&
                  packed.entries[1].kind ==
                      static_cast<int32_t>(ColorEffectKind::Wheels) &&
                  packed.entries[1].data_count == 9 &&
                  packed.data[5] == grade.data[5],
              "the packed grade carries every entry and the pool");
        ResolvedColorGrade malformed = grade;
        malformed.entries[1].data_offset = 700;
        Check(PackColorGradeForGpu(malformed).entries[1].kind == -1,
              "a stage pointing outside its pool is sent as a no-op");
    }

    ResolvedColorGrade overflowing;
    overflowing.count = 2;
    overflowing.entries[0] = {static_cast<int32_t>(ColorEffectKind::Exposure),
                              std::numeric_limits<float>::max()};
    overflowing.entries[1] = {static_cast<int32_t>(ColorEffectKind::Contrast),
                              std::numeric_limits<float>::max()};
    std::string impossibleCube = "stale output";
    std::string cubeError;
    Check(
        !BuildColorGradeCube(overflowing, impossibleCube, cubeError) &&
            impossibleCube.empty() &&
            cubeError.find("non-finite color") != std::string::npos,
        "an extreme model-authored stack cannot write NaN into an export LUT");

    TestResolvePrimaries();

    if (failures == 0) {
        std::cout << "PASS: color effects registry\n";
    }
    return failures == 0 ? 0 : 1;
}
