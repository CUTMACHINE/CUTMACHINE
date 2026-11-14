#include "ColorEffects.h"

#include "ColorManagement.h"
#include "ResolveContrastTable.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <string_view>

namespace {

const std::vector<ColorEffectRegistryEntry>& BuildRegistry() {
    // clang-format off
    static const std::vector<ColorEffectRegistryEntry> registry = {
        // The last column is the stage's place in the canonical order
        // (GR1, ColorEffects.h), not its place in this table.
        // type                     kind                                param      default  rank
        {"color.exposure",          ColorEffectKind::Exposure,         "amount",  0.0f,    6},
        {"color.contrast",          ColorEffectKind::Contrast,         "amount",  0.0f,    8},
        {"color.saturation",        ColorEffectKind::Saturation,       "amount",  0.0f,    11},
        {"color.vibrance",          ColorEffectKind::Vibrance,         "amount",  0.0f,    3},
        // G7 -- the light the scene was lit by, as a raw converter's slider
        // reads (ColorEffects.h). 6500 is the neutral only without an
        // as-shot white balance: ResolveColorGrade takes the clip's.
        {"color.temperature",       ColorEffectKind::Temperature,      "kelvin",  6500.0f, 4},
        {"color.tint",              ColorEffectKind::Tint,             "amount",  0.0f,    5},
        {"color.highlights",        ColorEffectKind::Highlights,       "amount",  0.0f,    2},
        {"color.shadows",           ColorEffectKind::Shadows,          "amount",  0.0f,    1},
        // F1.3 -- several knobs each; see ColorEffects.h.
        {"color.wheels",            ColorEffectKind::Wheels,           nullptr,   0.0f,    7},
        {"color.curves",            ColorEffectKind::Curves,           nullptr,   0.0f,    9},
        // GR6 -- with the saturation, after the curves.
        {"color.hue",               ColorEffectKind::Hue,              "degrees", 0.0f,    10},
        // GR8 -- after the saturation, where a corrector node has them.
        {"color.hue_curves",        ColorEffectKind::HueCurves,        nullptr,   0.0f,    12},
        // GR16 -- an imported Resolve node is the base of the grade: what a
        // hand adds here comes after it, as a second node would.
        {"color.resolve_primaries", ColorEffectKind::ResolvePrimaries, nullptr,   0.0f,    0},
    };
    // clang-format on
    return registry;
}

// GR8 -- B - Y and R - Y to Cb and Cr with the Rec.709 weights: 2 (1 - Kb)
// and 2 (1 - Kr).
constexpr float kChromaBlue = 1.8556f;
constexpr float kChromaRed = 1.5748f;

float Luma(const std::array<float, 3>& color) {
    return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

float Smoothstep(float low, float high, float value) {
    const float t = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// F1.3 -- wheels and curves. Exact rational comparisons: the bounds and the
// "strictly increasing x" rule are decided on the fractions the document
// stores, never on their float images.
bool LessThan(const EffectParamValue& left, const EffectParamValue& right) {
    return static_cast<int64_t>(left.num) * right.den <
           static_cast<int64_t>(right.num) * left.den;
}

bool WithinUnit(const EffectParamValue& value, int32_t low, int32_t high) {
    return static_cast<int64_t>(value.num) >=
               static_cast<int64_t>(low) * value.den &&
           static_cast<int64_t>(value.num) <=
               static_cast<int64_t>(high) * value.den;
}

// Messages built inside loops, without a temporary per operator+.
std::string Join(std::initializer_list<std::string_view> parts) {
    std::string joined;
    for (const std::string_view part : parts) {
        joined += part;
    }
    return joined;
}

std::string FractionText(const EffectParamValue& value) {
    return std::to_string(value.num) + "/" + std::to_string(value.den);
}

double ExactValue(const EffectParamValue& value) {
    return value.den > 0 ? static_cast<double>(value.num) / value.den : 0.0;
}

double ParamOrZero(const ClipEffect& effect, const std::string& name) {
    const auto found = effect.params.find(name);
    return found == effect.params.end() ? 0.0 : ExactValue(found->second);
}

// A curve param name, <channel>_<x|y><index>, split. The index is written
// in decimal without a leading zero, so each point has one spelling.
struct CurveParamName {
    std::string channel;
    char axis = 'x';
    int32_t index = 0;
};

// `names` is the stage's curves: color.curves's channels, or
// color.hue_curves's six (GR8).
template <size_t N>
bool ParseCurveParamName(const std::string& name,
                         const std::array<const char*, N>& names,
                         CurveParamName& parsed) {
    const size_t underscore = name.rfind('_');
    if (underscore == std::string::npos || underscore + 2 > name.size()) {
        return false;
    }
    parsed.channel = name.substr(0, underscore);
    if (std::find(names.begin(), names.end(), parsed.channel) == names.end()) {
        return false;
    }
    parsed.axis = name[underscore + 1];
    if (parsed.axis != 'x' && parsed.axis != 'y') {
        return false;
    }
    const std::string digits = name.substr(underscore + 2);
    if (digits.empty() || digits.size() > 2 ||
        (digits.size() > 1 && digits.front() == '0') ||
        !std::all_of(digits.begin(), digits.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; })) {
        return false;
    }
    parsed.index = std::stoi(digits);
    return parsed.index < kMaxColorCurvePoints;
}

bool ValidateWheels(const ClipEffect& effect, std::string& error) {
    for (const auto& [name, value] : effect.params) {
        bool known = false;
        for (const char* wheel : kColorWheelNames) {
            for (const char* channel : kColorWheelChannels) {
                known = known ||
                        name == std::string(wheel) + "_" + std::string(channel);
            }
        }
        if (!known) {
            error = "color.wheels has an unknown parameter '" + name +
                    "' (expected lift_, gamma_, gain_ or offset_ followed by "
                    "r, g, b or master)";
            return false;
        }
        if (!WithinUnit(value, -kColorWheelLimit, kColorWheelLimit)) {
            error = "color.wheels parameter '" + name + "' (" +
                    FractionText(value) + ") is outside [-1, 1]";
            return false;
        }
    }
    return true;
}

bool ValidateCurves(const ClipEffect& effect, std::string& error) {
    for (const auto& param : effect.params) {
        CurveParamName parsed;
        if (!ParseCurveParamName(param.first, kColorCurveChannels, parsed)) {
            error = "color.curves has an unknown parameter '" + param.first +
                    "' (expected <channel>_x<i> or <channel>_y<i>, channel "
                    "master, r, g or b, i from 0 to " +
                    std::to_string(kMaxColorCurvePoints - 1) +
                    " without leading zero)";
            return false;
        }
    }
    for (const char* channel : kColorCurveChannels) {
        std::vector<ColorCurvePoint> points;
        if (!ReadColorCurveChannel(effect, channel, points, error)) {
            return false;
        }
    }
    return true;
}

// GR16 -- color.resolve_primaries's params and their bounds, which hold
// every value of the 46 117 grades read from the author's Resolve libraries on 20
// October 2026 (gamma reached +-3.998, the offset -5.9 and 8).
struct ResolvePrimariesBound {
    const char* wheel;
    bool master;
    int32_t low;
    int32_t high;
};
constexpr std::array<ResolvePrimariesBound, 4> kResolvePrimariesBounds = {{
    {"lift", true, -2, 2},
    {"gamma", true, -4, 4},
    {"gain", true, 0, 16},
    {"offset", false, -8, 8},
}};

EffectParamValue ParamOr(const ClipEffect& effect, const std::string& name,
                         EffectParamValue fallback) {
    const auto found = effect.params.find(name);
    return found == effect.params.end() ? fallback : found->second;
}

bool ValidateResolvePrimaries(const ClipEffect& effect, std::string& error) {
    for (const auto& [name, value] : effect.params) {
        int32_t low = 0;
        int32_t high = 0;
        bool known = true;
        // GR16 -- the contrast up to 2, Resolve's own end; the two project
        // settings it depends on are 0 or 1.
        if (name == "saturation" || name == "contrast") {
            high = 2;
        } else if (name == "temperature") {
            // GR16 -- the ranges of Resolve's panel (manual, p. 3146).
            low = -4000;
            high = 4000;
        } else if (name == "tint") {
            low = -100;
            high = 100;
        } else if (name == "lum_mix" || name == "pivot") {
            high = 1;
        } else if (name == "color_space_aware" || name == "contrast_s_curve" ||
                   name == "acescct") {
            if (value.den <= 0 || (value.num != 0 && value.num != value.den)) {
                error = "color.resolve_primaries parameter '" + name + "' (" +
                        FractionText(value) + ") is 0 or 1";
                return false;
            }
            high = 1;
        } else {
            known = false;
            for (const ResolvePrimariesBound& bound : kResolvePrimariesBounds) {
                for (const char* channel : kColorWheelChannels) {
                    if (!bound.master && std::string(channel) == "master") {
                        continue;
                    }
                    if (name == std::string(bound.wheel) + "_" + channel) {
                        known = true;
                        low = bound.low;
                        high = bound.high;
                    }
                }
            }
        }
        if (!known) {
            error = "color.resolve_primaries has an unknown parameter '" +
                    name +
                    "' (expected lift_, gamma_ or gain_ followed by r, g, b "
                    "or master, offset_ followed by r, g or b, saturation, "
                    "lum_mix, contrast, pivot, color_space_aware, "
                    "contrast_s_curve, temperature, tint or acescct)";
            return false;
        }
        if (!WithinUnit(value, low, high)) {
            error =
                Join({"color.resolve_primaries parameter '", name, "' (",
                      FractionText(value), ") is outside [",
                      std::to_string(low), ", ", std::to_string(high), "]"});
            return false;
        }
    }
    // GR16 -- the S-curve is a table that ends at kResolveContrastTableMost;
    // a straight contrast of 2 would have an infinite slope.
    const EffectParamValue contrast = ParamOr(effect, "contrast", {1, 1});
    const EffectParamValue sCurve = ParamOr(effect, "contrast_s_curve", {1, 1});
    if (sCurve.num != 0 && LessThan({9, 5}, contrast)) {
        error = "color.resolve_primaries contrast " + FractionText(contrast) +
                " is above 9/5, where the measured S-curve ends";
        return false;
    }
    if (sCurve.num == 0 && !LessThan(contrast, {2, 1})) {
        error =
            "color.resolve_primaries: a straight contrast of 2 has no "
            "slope (contrast_s_curve 0)";
        return false;
    }
    // GR16 -- in ACEScct the contrast was measured with the colour-space-
    // aware tools off, as every ACEScct project of the library has them
    // (14 November 2026); on, it moves by 40 to 150 codes from that, and
    // from the aware contrast of DaVinci YRGB.
    if (ParamOr(effect, "acescct", {0, 1}).num != 0 &&
        ParamOr(effect, "color_space_aware", {0, 1}).num != 0 &&
        contrast.num != contrast.den) {
        error = "color.resolve_primaries contrast " + FractionText(contrast) +
                " is not measured in ACEScct with the colour-space-aware "
                "tools (acescct 1, color_space_aware 1)";
        return false;
    }
    // 1 - L + L G is what the law divides by: zero or below, black would sit
    // at or above white. Decided on the fractions.
    for (const char* channel : kColorWheelChannels) {
        const std::string suffix = std::string("_") + channel;
        const EffectParamValue lift = ParamOr(effect, "lift" + suffix, {0, 1});
        const EffectParamValue gain = ParamOr(effect, "gain" + suffix, {1, 1});
        const __int128 ld = lift.den;
        const __int128 gd = gain.den;
        const __int128 numerator = ld * gd -
                                   static_cast<__int128>(lift.num) * gd +
                                   static_cast<__int128>(lift.num) * gain.num;
        if (ld <= 0 || gd <= 0 || numerator <= 0) {
            constexpr const char* kBlackAboveWhite =
                " put black at or above white (1 - lift + lift * gain must "
                "be positive)";
            error = Join({"color.resolve_primaries: lift", suffix, " ",
                          FractionText(lift), " and gain", suffix, " ",
                          FractionText(gain), kBlackAboveWhite});
            return false;
        }
    }
    return true;
}

// The per-channel values the wheel formula consumes (ColorEffects.h): the
// output at input 0, the output at input 1, then the gamma exponent 1/G,
// three floats each. GR2 -- the offset moves the two ends together, so it
// is added to both: L + O and K + O, slope K - L as before. False when
// every knob is neutral.
bool ResolveWheels(const ClipEffect& effect, std::vector<float>& data) {
    data.assign(9, 0.0f);
    bool neutral = true;
    for (size_t channel = 0; channel < 3; ++channel) {
        const std::string suffix =
            std::string("_") + kColorWheelChannels.at(channel);
        const double lift = ParamOrZero(effect, "lift" + suffix) +
                            ParamOrZero(effect, "lift_master");
        const double gain = ParamOrZero(effect, "gain" + suffix) +
                            ParamOrZero(effect, "gain_master");
        const double gamma = ParamOrZero(effect, "gamma" + suffix) +
                             ParamOrZero(effect, "gamma_master");
        const double offset = ParamOrZero(effect, "offset" + suffix) +
                              ParamOrZero(effect, "offset_master");
        neutral = neutral && lift == 0.0 && gain == 0.0 && gamma == 0.0 &&
                  offset == 0.0;
        data[channel] = static_cast<float>(lift + offset);
        data[3 + channel] = static_cast<float>(std::exp2(gain) + offset);
        data[6 + channel] = static_cast<float>(std::exp2(-gamma));
    }
    return !neutral;
}

// Four blocks, master, r, g, b: [n, x0, y0, m0, x1, y1, m1, ...]. A channel
// that does not read as a valid curve resolves to the identity; a document
// that reached the renderer went through ValidateColorEffect already, so
// this only protects callers handing in unvalidated effects. False when no
// channel has points.
bool ResolveCurves(const ClipEffect& effect, std::vector<float>& data) {
    data.clear();
    bool any = false;
    for (const char* channel : kColorCurveChannels) {
        std::vector<ColorCurvePoint> points;
        std::string ignored;
        if (!ReadColorCurveChannel(effect, channel, points, ignored)) {
            points.clear();
        }
        data.push_back(static_cast<float>(points.size()));
        if (points.empty()) {
            continue;
        }
        any = true;
        std::vector<float> x;
        std::vector<float> y;
        for (const ColorCurvePoint& point : points) {
            x.push_back(EffectParamValueToFloat(point.x));
            y.push_back(EffectParamValueToFloat(point.y));
        }
        const std::vector<float> tangents = MonotoneCurveTangents(x, y);
        for (size_t index = 0; index < points.size(); ++index) {
            data.push_back(x[index]);
            data.push_back(y[index]);
            data.push_back(tangents[index]);
        }
    }
    return any;
}

// GR8 -- six blocks, in kHueCurveNames' order, each [n, x0, y0, m0, ...] as a
// curve channel's, with two points more than the curve has: one before its
// first and one after its last, so that EvaluateColorCurve (and the
// shader's evaluateCurve) is asked inside the block for every x of [0, 1].
// A curve read along the hue goes round: the point before the first is the
// last one, a turn earlier, and its tangents are those of the closed curve.
// The others hold their end values past their ends. An empty curve is one
// zero. False when no curve has a point.
bool ResolveHueCurves(const ClipEffect& effect, std::vector<float>& data) {
    data.clear();
    bool any = false;
    for (const char* curve : kHueCurveNames) {
        std::vector<ColorCurvePoint> points;
        std::string ignored;
        if (!ReadHueCurve(effect, curve, points, ignored)) {
            points.clear();
        }
        if (points.empty()) {
            data.push_back(0.0f);
            continue;
        }
        any = true;
        std::vector<float> x;
        std::vector<float> y;
        for (const ColorCurvePoint& point : points) {
            x.push_back(EffectParamValueToFloat(point.x));
            y.push_back(EffectParamValueToFloat(point.y));
        }
        const size_t count = points.size();
        std::vector<float> wideX;
        std::vector<float> wideY;
        // What is kept of the wider list below: its first point and its
        // last are there only to give their neighbours a tangent.
        size_t skip = 0;
        if (IsHueIndexedCurve(curve)) {
            wideX = {x[count - 2] - 1.0f, x[count - 1] - 1.0f};
            wideY = {y[count - 2], y[count - 1]};
            wideX.insert(wideX.end(), x.begin(), x.end());
            wideY.insert(wideY.end(), y.begin(), y.end());
            wideX.insert(wideX.end(), {x[0] + 1.0f, x[1] + 1.0f});
            wideY.insert(wideY.end(), {y[0], y[1]});
            skip = 1;
        } else {
            wideX = {x[0] - 1.0f};
            wideY = {y[0]};
            wideX.insert(wideX.end(), x.begin(), x.end());
            wideY.insert(wideY.end(), y.begin(), y.end());
            wideX.push_back(x[count - 1] + 1.0f);
            wideY.push_back(y[count - 1]);
        }
        const std::vector<float> tangents = MonotoneCurveTangents(wideX, wideY);
        data.push_back(static_cast<float>(count + 2));
        for (size_t index = skip; index < skip + count + 2; ++index) {
            data.push_back(wideX[index]);
            data.push_back(wideY[index]);
            data.push_back(tangents[index]);
        }
    }
    return any;
}

}  // namespace

const std::vector<ColorEffectRegistryEntry>& ColorEffectRegistry() {
    return BuildRegistry();
}

const ColorEffectRegistryEntry* FindColorEffectRegistryEntry(
    const std::string& type) {
    for (const ColorEffectRegistryEntry& entry : ColorEffectRegistry()) {
        if (type == entry.type) return &entry;
    }
    return nullptr;
}

float EffectParamValueToFloat(const EffectParamValue& value) {
    if (value.den <= 0) return 0.0f;
    return static_cast<float>(value.num) / static_cast<float>(value.den);
}

bool ColorStackFits(const std::vector<ClipEffect>& effects,
                    std::string& error) {
    error.clear();
    // A white balance no stage of the stack is set to: none of them resolves
    // as neutral, so the count is the largest any media gives.
    ColorGradeContext context;
    context.as_shot_kelvin = kMinimumKelvin;
    const auto taken = [&](int32_t kelvin) {
        for (const ClipEffect& effect : effects) {
            if (effect.type != "color.temperature") continue;
            const auto param = effect.params.find("kelvin");
            if (param != effect.params.end() &&
                std::clamp(static_cast<int32_t>(std::lround(
                               EffectParamValueToFloat(param->second))),
                           kMinimumKelvin, kMaximumKelvin) == kelvin) {
                return true;
            }
        }
        return false;
    };
    while (taken(context.as_shot_kelvin) &&
           context.as_shot_kelvin < kMaximumKelvin) {
        ++context.as_shot_kelvin;
    }
    const ResolvedColorGrade grade = ResolveColorGrade(effects, context);
    if (grade.dropped_from < 0) {
        return true;
    }
    const auto index = static_cast<size_t>(grade.dropped_from);
    error = "the stack holds more than a clip's grade renders: stage " +
            std::to_string(index + 1) + " (" + effects[index].type +
            ") and those after it would be left out, on the monitor and in "
            "the export alike -- at most " +
            std::to_string(kMaxColorEffectsPerClip) + " stages and " +
            std::to_string(kMaxColorGradeData) +
            " resolved values per clip (a curve channel takes 1 + 3 per "
            "point)";
    return false;
}

int32_t CanonicalColorStageRank(const std::string& type) {
    const ColorEffectRegistryEntry* entry = FindColorEffectRegistryEntry(type);
    return entry == nullptr ? -1 : entry->canonical_rank;
}

bool IsCanonicalColorStack(const std::vector<ClipEffect>& effects) {
    int32_t last = -1;
    for (const ClipEffect& effect : effects) {
        const int32_t rank = CanonicalColorStageRank(effect.type);
        if (rank < 0) continue;
        if (rank < last) return false;
        last = rank;
    }
    return true;
}

size_t CanonicalColorStageIndex(const std::vector<ClipEffect>& effects,
                                const std::string& type) {
    const int32_t rank = CanonicalColorStageRank(type);
    if (rank < 0 || !IsCanonicalColorStack(effects)) return effects.size();
    for (size_t index = 0; index < effects.size(); ++index) {
        if (CanonicalColorStageRank(effects[index].type) > rank) return index;
    }
    return effects.size();
}

namespace {

// The points of one curve of a curve stage, in increasing x, checked as
// ColorEffects.h says. `stage` and `noun` only word the refusals: a channel
// of color.curves, a curve of color.hue_curves.
template <size_t N>
bool ReadCurvePoints(const ClipEffect& effect, const std::string& stage,
                     const std::string& noun,
                     const std::array<const char*, N>& names,
                     const std::string& channel,
                     std::vector<ColorCurvePoint>& points, std::string& error) {
    points.clear();
    std::map<int32_t, EffectParamValue> xs;
    std::map<int32_t, EffectParamValue> ys;
    for (const auto& [name, value] : effect.params) {
        CurveParamName parsed;
        if (!ParseCurveParamName(name, names, parsed) ||
            parsed.channel != channel) {
            continue;
        }
        if (value.den <= 0) {
            error = stage + " parameter '" + name +
                    "' has a non-positive denominator";
            return false;
        }
        (parsed.axis == 'x' ? xs : ys)[parsed.index] = value;
    }
    const std::string prefix = channel + "_";
    const size_t count = std::max(xs.size(), ys.size());
    for (int32_t index = 0; index < static_cast<int32_t>(count); ++index) {
        const std::string number = std::to_string(index);
        const bool hasX = xs.count(index) != 0;
        const bool hasY = ys.count(index) != 0;
        if (!hasX && !hasY) {
            error =
                Join({stage, " ", noun, " '", channel, "' has no point ",
                      number, ": points are numbered from 0 without a gap"});
            return false;
        }
        if (!hasX || !hasY) {
            error = Join({stage, " has ", prefix, hasX ? "x" : "y", number,
                          " without ", prefix, hasX ? "y" : "x", number});
            return false;
        }
    }
    if (count == 1) {
        error = stage + " " + noun + " '" + channel +
                "' has a single point; a curve takes none or at least two";
        return false;
    }
    for (int32_t index = 0; index < static_cast<int32_t>(count); ++index) {
        const std::string number = std::to_string(index);
        const EffectParamValue& x = xs.at(index);
        const EffectParamValue& y = ys.at(index);
        if (!WithinUnit(x, 0, 1) || !WithinUnit(y, 0, 1)) {
            const bool xOutside = !WithinUnit(x, 0, 1);
            error = Join({stage, " point ", prefix, xOutside ? "x" : "y",
                          number, " (", FractionText(xOutside ? x : y),
                          ") is outside [0, 1]"});
            return false;
        }
        if (index > 0 && !LessThan(xs.at(index - 1), x)) {
            error = Join({stage, " ", noun, " '", channel,
                          "' is not increasing in x: ", prefix, "x", number,
                          " (", FractionText(x), ") does not come after ",
                          prefix, "x", std::to_string(index - 1), " (",
                          FractionText(xs.at(index - 1)), ")"});
            return false;
        }
        // The renderer divides by the gap between two x as floats.
        if (index > 0 && !(EffectParamValueToFloat(xs.at(index - 1)) <
                           EffectParamValueToFloat(x))) {
            error = Join({stage, " ", noun, " '", channel, "': ", prefix, "x",
                          number, " is too close to ", prefix, "x",
                          std::to_string(index - 1), " to tell them apart"});
            return false;
        }
        points.push_back({x, y});
    }
    return true;
}

}  // namespace

bool ReadColorCurveChannel(const ClipEffect& effect, const std::string& channel,
                           std::vector<ColorCurvePoint>& points,
                           std::string& error) {
    return ReadCurvePoints(effect, "color.curves", "channel",
                           kColorCurveChannels, channel, points, error);
}

bool IsHueIndexedCurve(const std::string& curve) {
    return curve.rfind("hue_", 0) == 0;
}

bool ReadHueCurve(const ClipEffect& effect, const std::string& curve,
                  std::vector<ColorCurvePoint>& points, std::string& error) {
    if (!ReadCurvePoints(effect, "color.hue_curves", "curve", kHueCurveNames,
                         curve, points, error)) {
        return false;
    }
    // A hue of 1 is the hue 0 again: a point at both would give that hue
    // two values.
    if (IsHueIndexedCurve(curve) && !points.empty() &&
        !LessThan(points.back().x, {1, 1})) {
        error = "color.hue_curves curve '" + curve +
                "' has a point at x = 1, which is the hue 0 again: a hue "
                "lies in [0, 1)";
        points.clear();
        return false;
    }
    return true;
}

bool ValidateColorEffect(const ClipEffect& effect, std::string& error) {
    if (effect.type == "color.wheels") {
        return ValidateWheels(effect, error);
    }
    if (effect.type == "color.resolve_primaries") {
        return ValidateResolvePrimaries(effect, error);
    }
    if (effect.type == "color.curves") {
        return ValidateCurves(effect, error);
    }
    if (effect.type == "color.hue_curves") {
        // GR8 -- named params only, and every curve a curve.
        for (const auto& param : effect.params) {
            CurveParamName parsed;
            if (!ParseCurveParamName(param.first, kHueCurveNames, parsed)) {
                error = "color.hue_curves has an unknown parameter '" +
                        param.first +
                        "' (expected <curve>_x<i> or <curve>_y<i>, curve "
                        "hue_hue, hue_sat, hue_lum, lum_sat, sat_sat or "
                        "sat_lum, i from 0 to " +
                        std::to_string(kMaxColorCurvePoints - 1) +
                        " without leading zero)";
                return false;
            }
        }
        for (const char* curve : kHueCurveNames) {
            std::vector<ColorCurvePoint> points;
            if (!ReadHueCurve(effect, curve, points, error)) {
                return false;
            }
        }
        return true;
    }
    if (effect.type == "color.contrast") {
        // GR6 -- the pivot is a value of the picture.
        const auto pivot = effect.params.find("pivot");
        if (pivot != effect.params.end() && !WithinUnit(pivot->second, 0, 1)) {
            error = "color.contrast parameter 'pivot' (" +
                    FractionText(pivot->second) + ") is outside [0, 1]";
            return false;
        }
    }
    return true;
}

std::vector<float> MonotoneCurveTangents(const std::vector<float>& x,
                                         const std::vector<float>& y) {
    const size_t count = std::min(x.size(), y.size());
    std::vector<float> tangents(count, 0.0f);
    if (count < 2) {
        return tangents;
    }
    std::vector<double> secants(count - 1);
    for (size_t index = 0; index + 1 < count; ++index) {
        secants[index] = (static_cast<double>(y[index + 1]) - y[index]) /
                         (static_cast<double>(x[index + 1]) - x[index]);
    }
    std::vector<double> slopes(count, 0.0);
    slopes.front() = secants.front();
    slopes.back() = secants.back();
    for (size_t index = 1; index + 1 < count; ++index) {
        if (secants[index - 1] * secants[index] > 0.0) {
            slopes[index] = (secants[index - 1] + secants[index]) / 2.0;
        }
    }
    for (size_t index = 0; index + 1 < count; ++index) {
        if (secants[index] == 0.0) {
            slopes[index] = 0.0;
            slopes[index + 1] = 0.0;
            continue;
        }
        const double alpha = slopes[index] / secants[index];
        const double beta = slopes[index + 1] / secants[index];
        const double radius = (alpha * alpha) + (beta * beta);
        if (radius > 9.0) {
            const double scale = 3.0 / std::sqrt(radius);
            slopes[index] = scale * alpha * secants[index];
            slopes[index + 1] = scale * beta * secants[index];
        }
    }
    for (size_t index = 0; index < count; ++index) {
        tangents[index] = static_cast<float>(slopes[index]);
    }
    return tangents;
}

float EvaluateHueCurve(const float* block, float x) {
    return block[0] < 2.0f ? 0.5f : EvaluateColorCurve(block, x);
}

float EvaluateColorCurve(const float* block, float x) {
    const auto points = static_cast<int32_t>(block[0]);
    if (points < 2) {
        return x;
    }
    const auto count = static_cast<size_t>(points);
    const float* point = block + 1;
    const size_t last = (count - 1) * 3;
    if (x <= point[0]) {
        return point[1] + (point[2] * (x - point[0]));
    }
    if (x >= point[last]) {
        return point[last + 1] + (point[last + 2] * (x - point[last]));
    }
    size_t interval = 0;
    while (interval + 2 < count && x >= point[(interval + 1) * 3]) {
        ++interval;
    }
    const float* start = point + (interval * 3);
    const float width = start[3] - start[0];
    const float t = (x - start[0]) / width;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return ((2.0f * t3 - 3.0f * t2 + 1.0f) * start[1]) +
           ((t3 - 2.0f * t2 + t) * width * start[2]) +
           ((-2.0f * t3 + 3.0f * t2) * start[4]) +
           ((t3 - t2) * width * start[5]);
}

namespace {

using Matrix3 = std::array<std::array<double, 3>, 3>;

// Bradford's cone response, the chromatic adaptation every white balance
// here goes through.
const Matrix3 kBradford = {{{0.8951, 0.2664, -0.1614},
                            {-0.7502, 1.7135, 0.0367},
                            {0.0389, -0.0685, 1.0296}}};

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) {
    Matrix3 out{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            for (int k = 0; k < 3; ++k)
                out[row][column] += a[row][k] * b[k][column];
    return out;
}

Matrix3 Inverse(const Matrix3& m) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Matrix3 out{};
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return out;
}

// RGB to XYZ of each grading space's primaries: AP1 (ACES TB-2014-004),
// BT.709 and BT.2020 (their own white, D65).
Matrix3 RgbToXyz(ColorGradeSpace space) {
    switch (space) {
        case ColorGradeSpace::AcesCct:
            return {{{0.6624541811, 0.1340042065, 0.1561876870},
                     {0.2722287168, 0.6740817658, 0.0536895174},
                     {-0.0055746495, 0.0040607335, 1.0103391003}}};
        case ColorGradeSpace::LinearRec2020:
            return {{{0.6369580, 0.1446169, 0.1688810},
                     {0.2627002, 0.6779981, 0.0593017},
                     {0.0000000, 0.0280727, 1.0609851}}};
        case ColorGradeSpace::Display:
        case ColorGradeSpace::LinearRec709:
            break;
    }
    return {{{0.4124564, 0.3575761, 0.1804375},
             {0.2126729, 0.7151522, 0.0721750},
             {0.0193339, 0.1191920, 0.9503041}}};
}

// The chromaticity of a blackbody, by the cubic splines of Kim et al.
// (2002), 1667 K to 25000 K; the white it gives, at Y = 1.
std::array<double, 3> BlackbodyWhite(int32_t kelvin) {
    const double t =
        static_cast<double>(std::clamp(kelvin, kMinimumKelvin, kMaximumKelvin));
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double x =
        t <= 4000.0
            ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
            : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x;
    const double x3 = x2 * x;
    const double y =
        t <= 2222.0
            ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
        : t <= 4000.0
            ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
            : 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    return {x / y, 1.0, (1.0 - x - y) / y};
}

// GR16 -- RGB to XYZ from chromaticities {xr, yr, xg, yg, xb, yb, xw, yw}.
Matrix3 RgbToXyzFromPrimaries(const std::array<double, 8>& c) {
    Matrix3 primaries{};
    for (int column = 0; column < 3; ++column) {
        const double x = c[static_cast<size_t>(column) * 2];
        const double y = c[static_cast<size_t>(column) * 2 + 1];
        primaries[0][column] = x / y;
        primaries[1][column] = 1.0;
        primaries[2][column] = (1.0 - x - y) / y;
    }
    const std::array<double, 3> white = {c[6] / c[7], 1.0,
                                         (1.0 - c[6] - c[7]) / c[7]};
    const Matrix3 inverse = Inverse(primaries);
    Matrix3 out = primaries;
    for (int column = 0; column < 3; ++column) {
        double scale = 0.0;
        for (int k = 0; k < 3; ++k) scale += inverse[column][k] * white[k];
        for (int row = 0; row < 3; ++row) out[row][column] *= scale;
    }
    return out;
}

// GR16 -- DaVinci Wide Gamut's published primaries (Blackmagic Design,
// "DaVinci Wide Gamut / DaVinci Intermediate", 2020), white D65.
const std::array<double, 8> kDaVinciWideGamut = {
    0.8000, 0.3130, 0.1682, 0.9877, 0.0790, -0.1155, 0.3127, 0.3290};

// Linear light of a grading space to the linear light a Resolve node grades
// in: DaVinci Wide Gamut, or AP1 for a node graded in ACEScct (GR16, 14
// November 2026). Through XYZ, adapted by Bradford from the space's white to
// the node's (D65, or AP1's own): a grey stays a grey, and AP1 to AP1 is the
// identity.
Matrix3 ToResolveNodeGamut(ColorGradeSpace space, bool acescct) {
    const Matrix3& bradford = kBradford;
    const Matrix3 toXyz = RgbToXyz(space);
    const Matrix3 wideToXyz = acescct
                                  ? RgbToXyz(ColorGradeSpace::AcesCct)
                                  : RgbToXyzFromPrimaries(kDaVinciWideGamut);
    std::array<double, 3> source{};
    std::array<double, 3> target{};
    for (int row = 0; row < 3; ++row) {
        source[row] = toXyz[row][0] + toXyz[row][1] + toXyz[row][2];
        target[row] = wideToXyz[row][0] + wideToXyz[row][1] + wideToXyz[row][2];
    }
    Matrix3 scale{};
    for (int row = 0; row < 3; ++row) {
        double from = 0.0;
        double to = 0.0;
        for (int k = 0; k < 3; ++k) {
            from += bradford[row][k] * source[k];
            to += bradford[row][k] * target[k];
        }
        scale[row][row] = to / from;
    }
    const Matrix3 adaptation =
        Multiply(Inverse(bradford), Multiply(scale, bradford));
    return Multiply(Inverse(wideToXyz), Multiply(adaptation, toXyz));
}

// GR16 -- the white Resolve's temperature and tint aim at (measured on 6
// November 2026): Robertson's isotemperature lines (1968), as Wyszecki and
// Stiles print them -- the u of 325 mired included, misprinted 0.24702 for
// 0.24792, which Resolve keeps: without it, a temperature of 3000 to 3500
// misses its cube by 9 codes --, interpolated in mired, then moved by
// `offset` along the isotemperature line, as the DNG SDK does. XYZ at Y = 1.
struct RobertsonLine {
    double mired;
    double u;
    double v;
    double slope;
};
constexpr std::array<RobertsonLine, 31> kRobertsonLines = {{
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},
    {20, 0.18133, 0.26846, -0.26876},  {30, 0.18208, 0.27119, -0.28539},
    {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},
    {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},
    {80, 0.18740, 0.28668, -0.40955},  {90, 0.18880, 0.28997, -0.44278},
    {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901},
    {200, 0.21142, 0.32312, -1.0182},  {225, 0.21807, 0.32909, -1.2168},
    {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},
    {300, 0.24010, 0.34308, -2.0637},  {325, 0.24702, 0.34655, -2.4681},
    {350, 0.25591, 0.34951, -2.9641},  {375, 0.26400, 0.35200, -3.5814},
    {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},
    {500, 0.30505, 0.35907, -11.324},  {525, 0.31320, 0.35968, -15.628},
    {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},
    {600, 0.33724, 0.36051, -116.45},
}};

std::array<double, 3> RobertsonWhite(double kelvin, double offset) {
    const double mired = 1.0e6 / kelvin;
    size_t index = 0;
    while (index < kRobertsonLines.size() - 2 &&
           mired >= kRobertsonLines[index + 1].mired) {
        ++index;
    }
    const RobertsonLine& low = kRobertsonLines[index];
    const RobertsonLine& high = kRobertsonLines[index + 1];
    const double f = (high.mired - mired) / (high.mired - low.mired);
    double u = (low.u * f) + (high.u * (1.0 - f));
    double v = (low.v * f) + (high.v * (1.0 - f));
    // The direction of each isotemperature line, unit length, blended.
    const double lowLength = std::hypot(1.0, low.slope);
    const double highLength = std::hypot(1.0, high.slope);
    double du = (f / lowLength) + ((1.0 - f) / highLength);
    double dv =
        (low.slope * f / lowLength) + (high.slope * (1.0 - f) / highLength);
    const double length = std::hypot(du, dv);
    du /= length;
    dv /= length;
    u += du * offset;
    v += dv * offset;
    const double x = 1.5 * u / (u - (4.0 * v) + 2.0);
    const double y = v / (u - (4.0 * v) + 2.0);
    return {x / y, 1.0, (1.0 - x - y) / y};
}

// a and b of Resolve's lift and gain (ColorEffects.h), and the exponent of
// its gamma.
void ResolveLiftGain(double lift, double gain, double& a, double& b) {
    const double denominator = 1.0 - lift + lift * gain;
    a = lift * gain / denominator;
    b = gain * (1.0 - lift) / denominator;
}

double ResolveGammaExponent(double gamma) {
    return gamma >= 0.0 ? 1.0 / (1.0 + gamma) : 1.0 - gamma;
}

double ParamOrDefault(const ClipEffect& effect, const std::string& name,
                      double fallback) {
    const auto found = effect.params.find(name);
    return found == effect.params.end() ? fallback : ExactValue(found->second);
}

// The kResolvePrimariesDataSize floats ColorEffects.h lists. False when the
// stage changes nothing.
bool ResolveResolvePrimaries(const ClipEffect& effect, ColorGradeSpace space,
                             std::vector<float>& data) {
    data.assign(kResolvePrimariesDataSize, 0.0f);
    data[0] = static_cast<float>(space);
    // GR16 -- a node graded in ACEScct grades ACEScct codes in AP1.
    const bool acescct = ParamOrDefault(effect, "acescct", 0.0) != 0.0;
    const Matrix3 to = ToResolveNodeGamut(space, acescct);
    const Matrix3 from = Inverse(to);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            data[1 + static_cast<size_t>(row) * 3 + column] =
                static_cast<float>(to[row][column]);
            data[10 + static_cast<size_t>(row) * 3 + column] =
                static_cast<float>(from[row][column]);
        }
    }
    constexpr std::array<double, 3> kLuma = {0.2126, 0.7152, 0.0722};
    bool neutral = true;
    double offsetLuma = 0.0;
    for (size_t channel = 0; channel < 4; ++channel) {
        const std::string suffix =
            std::string("_") + kColorWheelChannels.at(channel);
        double a = 0.0;
        double b = 1.0;
        ResolveLiftGain(ParamOrDefault(effect, "lift" + suffix, 0.0),
                        ParamOrDefault(effect, "gain" + suffix, 1.0), a, b);
        const double exponent =
            ResolveGammaExponent(ParamOrDefault(effect, "gamma" + suffix, 0.0));
        neutral = neutral && a == 0.0 && b == 1.0 && exponent == 1.0;
        if (channel == 3) {
            data[31] = static_cast<float>(a);
            data[32] = static_cast<float>(b);
            data[33] = static_cast<float>(exponent);
            continue;
        }
        const double offset = static_cast<double>(kResolveOffsetScale) *
                              ParamOrDefault(effect, "offset" + suffix, 0.0);
        neutral = neutral && offset == 0.0;
        offsetLuma += kLuma.at(channel) * offset;
        data[19 + channel] = static_cast<float>(a);
        data[22 + channel] = static_cast<float>(b);
        data[25 + channel] = static_cast<float>(offset);
        data[28 + channel] = static_cast<float>(exponent);
    }
    const double saturation = ParamOrDefault(effect, "saturation", 1.0);
    data[34] = static_cast<float>(offsetLuma);
    data[35] = static_cast<float>(ParamOrDefault(effect, "lum_mix", 1.0));
    data[36] = static_cast<float>(saturation);
    // GR16 -- the luma the saturation turns about: DaVinci YRGB's weights,
    // or Rec.709's in a project in ACEScct (measured on 7 November 2026).
    const std::array<float, 3> saturationLuma =
        acescct ? std::array<float, 3>{0.2126f, 0.7152f, 0.0722f}
                : std::array<float, 3>{0.21f, 0.70f, 0.09f};
    std::copy(saturationLuma.begin(), saturationLuma.end(), data.begin() + 37);
    data[40] = acescct ? 1.0f : 0.0f;
    // GR16 -- the contrast, when it moves: where it acts, then its curve
    // (ResolveContrastTable.h).
    std::array<float, kResolveContrastSamples> curve{};
    float end = 1.0f;
    const bool aware = ParamOrDefault(effect, "color_space_aware", 0.0) != 0.0;
    const bool contrasted = ResolveContrastCurve(
        static_cast<float>(ParamOrDefault(effect, "contrast", 1.0)),
        static_cast<float>(ParamOrDefault(effect, "pivot", kResolvePivot)),
        aware, ParamOrDefault(effect, "contrast_s_curve", 1.0) != 0.0, curve,
        end);
    if (contrasted) {
        data.resize(kResolvePrimariesContrastDataSize);
        data[41] = aware ? 2.0f : 1.0f;
        data[42] = end;
        std::copy(curve.begin(), curve.end(), data.begin() + 43);
    }
    // GR16 -- the temperature and the tint, last in the block, first in the
    // law: their matrix on the node's values (on AP0 light to the power
    // 1/2.4 in ACEScct, ApplyResolvePrimaries).
    const double temperature = ParamOrDefault(effect, "temperature", 0.0);
    const double tint = ParamOrDefault(effect, "tint", 0.0);
    const bool balanced = temperature != 0.0 || tint != 0.0;
    if (balanced) {
        const std::array<double, 9> matrix =
            ResolveWhiteBalanceMatrix(temperature, tint);
        for (const double value : matrix) {
            data.push_back(static_cast<float>(value));
        }
    }
    return balanced || contrasted || !neutral || saturation != 1.0;
}

}  // namespace

ColorGradeContext ColorGradeContextFor(const Document& document,
                                       const DocumentClip& clip) {
    ColorGradeContext context;
    const ColorManagementSettings& color = document.color_management;
    if (color.enabled) {
        context.space =
            color.working_gamut == "rec709"    ? ColorGradeSpace::LinearRec709
            : color.working_gamut == "rec2020" ? ColorGradeSpace::LinearRec2020
                                               : ColorGradeSpace::AcesCct;
    }
    if (const LibraryMedia* media = document.FindLibraryMedia(clip.source_id))
        context.as_shot_kelvin = media->white_balance_kelvin;
    return context;
}

int32_t ColorGradeNeutralKelvin(const ColorGradeContext& context) {
    return context.as_shot_kelvin > 0 ? context.as_shot_kelvin : kNeutralKelvin;
}

std::array<float, 9> HueRotationMatrix(double degrees) {
    // To luma and chroma with the Rec.709 weights, the chroma turned, and
    // back: hue is the angle from +Cb toward +Cr, as the vectorscope and
    // measure_color count it.
    constexpr double kRed = 0.2126;
    constexpr double kGreen = 0.7152;
    constexpr double kBlue = 0.0722;
    const double cb = 2.0 * (1.0 - kBlue);
    const double cr = 2.0 * (1.0 - kRed);
    const Matrix3 toYcc = {{{kRed, kGreen, kBlue},
                            {-kRed / cb, -kGreen / cb, (1.0 - kBlue) / cb},
                            {(1.0 - kRed) / cr, -kGreen / cr, -kBlue / cr}}};
    const double angle = degrees * 3.14159265358979323846 / 180.0;
    const Matrix3 turn = {{{1.0, 0.0, 0.0},
                           {0.0, std::cos(angle), -std::sin(angle)},
                           {0.0, std::sin(angle), std::cos(angle)}}};
    const Matrix3 toRgb = Inverse(toYcc);
    const Matrix3 whole = Multiply(toRgb, Multiply(turn, toYcc));
    std::array<float, 9> matrix{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            matrix.at((row * 3) + column) =
                static_cast<float>(whole.at(row).at(column));
        }
    }
    return matrix;
}

std::array<float, 9> WhiteBalanceMatrix(int32_t sceneKelvin,
                                        int32_t asShotKelvin,
                                        ColorGradeSpace space) {
    const Matrix3& bradford = kBradford;
    const std::array<double, 3> source = BlackbodyWhite(sceneKelvin);
    const std::array<double, 3> target = BlackbodyWhite(asShotKelvin);
    Matrix3 scale{};
    for (int row = 0; row < 3; ++row) {
        double from = 0.0;
        double to = 0.0;
        for (int k = 0; k < 3; ++k) {
            from += bradford[row][k] * source[k];
            to += bradford[row][k] * target[k];
        }
        scale[row][row] = to / from;
    }
    const Matrix3 adaptation =
        Multiply(Inverse(bradford), Multiply(scale, bradford));
    const Matrix3 toXyz = RgbToXyz(space);
    const Matrix3 rgb = Multiply(Inverse(toXyz), Multiply(adaptation, toXyz));
    std::array<float, 9> out{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out[static_cast<size_t>(row) * 3 + static_cast<size_t>(column)] =
                static_cast<float>(rgb[row][column]);
    return out;
}

std::array<double, 9> ResolveWhiteBalanceMatrix(double temperature,
                                                double tint) {
    // GR16 -- measured on 6 November 2026: a Bradford adaptation, in
    // BT.709's primaries, from the white of 6500 K to the white of 6500 K
    // minus the temperature, moved by a ten-thousandth of the tint along
    // its isotemperature line (RobertsonWhite). Resolve goes back from the
    // cone response through Bradford's inverse rounded to four decimals:
    // with the exact inverse, every cube is missed by up to 0.19 of a code,
    // with these figures by 0.04 at most.
    const Matrix3 inverseBradford = {{{0.9870, -0.1471, 0.1600},
                                      {0.4323, 0.5184, 0.0493},
                                      {-0.0085, 0.0400, 0.9685}}};
    const Matrix3 toXyz = RgbToXyzFromPrimaries(
        {0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290});
    const std::array<double, 3> source = RobertsonWhite(6500.0, 0.0);
    const std::array<double, 3> target =
        RobertsonWhite(6500.0 - temperature, tint * 1.0e-4);
    Matrix3 gains{};
    for (int row = 0; row < 3; ++row) {
        double from = 0.0;
        double to = 0.0;
        for (int k = 0; k < 3; ++k) {
            from += kBradford[row][k] * source[k];
            to += kBradford[row][k] * target[k];
        }
        gains[row][row] = to / from;
    }
    const Matrix3 whole = Multiply(
        Inverse(toXyz),
        Multiply(inverseBradford, Multiply(gains, Multiply(kBradford, toXyz))));
    std::array<double, 9> out{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            out.at((row * 3) + column) = whole.at(row).at(column);
        }
    }
    return out;
}

ResolvedColorGrade ResolveColorGrade(const std::vector<ClipEffect>& effects,
                                     const ColorGradeContext& context) {
    ResolvedColorGrade grade;
    for (size_t position = 0; position < effects.size(); ++position) {
        const ClipEffect& effect = effects[position];
        const ColorEffectRegistryEntry* entry =
            FindColorEffectRegistryEntry(effect.type);
        if (entry == nullptr) {
            continue;  // Unregistered "color.*" type: silent no-op.
        }
        // GR7 -- every way out of this loop before its end leaves a stage
        // out; it is recorded once, at the first, and cleared below when
        // the stage turns out to be neutral or to fit.
        grade.dropped_from = static_cast<int32_t>(position);
        if (grade.count >= kMaxColorEffectsPerClip) {
            break;
        }
        ResolvedColorEffect resolved;
        resolved.kind = static_cast<int32_t>(entry->kind);
        if (entry->kind == ColorEffectKind::Temperature) {
            // G7 -- the space, then the adaptation's matrix; neutral at the
            // as-shot white balance.
            const int32_t neutral = ColorGradeNeutralKelvin(context);
            int32_t kelvin = neutral;
            const auto param = effect.params.find(entry->param_name);
            if (param != effect.params.end())
                kelvin = static_cast<int32_t>(
                    std::lround(EffectParamValueToFloat(param->second)));
            kelvin = std::clamp(kelvin, kMinimumKelvin, kMaximumKelvin);
            if (kelvin == std::clamp(neutral, kMinimumKelvin, kMaximumKelvin)) {
                grade.dropped_from = -1;
                continue;
            }
            constexpr int32_t kSize = 10;
            if (kSize > kMaxColorGradeData - grade.data_count) break;
            const std::array<float, 9> matrix =
                WhiteBalanceMatrix(kelvin, neutral, context.space);
            resolved.amount = static_cast<float>(kelvin);
            resolved.data_offset = grade.data_count;
            resolved.data_count = kSize;
            grade.data[static_cast<size_t>(grade.data_count)] =
                static_cast<float>(context.space);
            std::copy(matrix.begin(), matrix.end(),
                      grade.data.begin() + grade.data_count + 1);
            grade.data_count += kSize;
        } else if (entry->kind == ColorEffectKind::Wheels ||
                   entry->kind == ColorEffectKind::Curves ||
                   entry->kind == ColorEffectKind::HueCurves ||
                   entry->kind == ColorEffectKind::ResolvePrimaries) {
            // F1.3 -- a neutral stage resolves to nothing (ColorEffects.h).
            // GR16 -- the Resolve stage also needs where it acts.
            std::vector<float> data;
            const bool active =
                entry->kind == ColorEffectKind::Wheels
                    ? ResolveWheels(effect, data)
                : entry->kind == ColorEffectKind::Curves
                    ? ResolveCurves(effect, data)
                : entry->kind == ColorEffectKind::HueCurves
                    ? ResolveHueCurves(effect, data)
                    : ResolveResolvePrimaries(effect, context.space, data);
            if (!active) {
                grade.dropped_from = -1;
                continue;
            }
            const auto size = static_cast<int32_t>(data.size());
            if (size > kMaxColorGradeData - grade.data_count) {
                break;  // The pool is full: the stack ends here.
            }
            resolved.data_offset = grade.data_count;
            resolved.data_count = size;
            std::copy(data.begin(), data.end(),
                      grade.data.begin() + grade.data_count);
            grade.data_count += size;
        } else if (entry->kind == ColorEffectKind::Hue) {
            // GR6 -- the rotation as a matrix; a whole turn is no stage.
            const double degrees = ParamOrZero(effect, entry->param_name);
            if (std::fmod(degrees, 360.0) == 0.0) {
                grade.dropped_from = -1;
                continue;
            }
            constexpr int32_t kSize = 9;
            if (kSize > kMaxColorGradeData - grade.data_count) break;
            const std::array<float, 9> matrix = HueRotationMatrix(degrees);
            resolved.amount = static_cast<float>(degrees);
            resolved.data_offset = grade.data_count;
            resolved.data_count = kSize;
            std::copy(matrix.begin(), matrix.end(),
                      grade.data.begin() + grade.data_count);
            grade.data_count += kSize;
        } else {
            resolved.amount = entry->default_value;
            const auto param = effect.params.find(entry->param_name);
            if (param != effect.params.end()) {
                resolved.amount = EffectParamValueToFloat(param->second);
            }
            // GR6 -- a contrast with its own pivot carries it in the pool;
            // without one it resolves as it always did.
            const auto pivot = effect.params.find("pivot");
            if (entry->kind == ColorEffectKind::Contrast &&
                pivot != effect.params.end()) {
                if (grade.data_count >= kMaxColorGradeData) break;
                resolved.data_offset = grade.data_count;
                resolved.data_count = 1;
                grade.data[static_cast<size_t>(grade.data_count)] =
                    EffectParamValueToFloat(pivot->second);
                grade.data_count += 1;
            }
        }
        grade.entries[static_cast<size_t>(grade.count)] = resolved;
        ++grade.count;
        grade.dropped_from = -1;
    }
    return grade;
}

namespace {

// GR16 -- whether a color.resolve_primaries block of `count` floats ends
// with a temperature and tint matrix.
bool ResolveBlockBalanced(int32_t count) {
    return count == kResolvePrimariesDataSize + kResolveWhiteBalanceDataSize ||
           count ==
               kResolvePrimariesContrastDataSize + kResolveWhiteBalanceDataSize;
}

// The data of one wheel or curve stage, or nullptr when the entry does not
// describe a well-formed block of the pool (only a hand-built grade can).
const float* StageData(const ResolvedColorGrade& grade,
                       const ResolvedColorEffect& effect) {
    if (effect.data_offset < 0 || effect.data_count <= 0 ||
        effect.data_offset > grade.data_count - effect.data_count ||
        grade.data_count > kMaxColorGradeData) {
        return nullptr;
    }
    const float* data = grade.data.data() + effect.data_offset;
    if (static_cast<ColorEffectKind>(effect.kind) == ColorEffectKind::Wheels) {
        return effect.data_count == 9 ? data : nullptr;
    }
    if (static_cast<ColorEffectKind>(effect.kind) ==
        ColorEffectKind::Temperature) {
        return effect.data_count == 10 ? data : nullptr;
    }
    if (static_cast<ColorEffectKind>(effect.kind) ==
        ColorEffectKind::ResolvePrimaries) {
        // GR16 -- the primaries, with or without a contrast, with or
        // without a temperature and tint.
        const int32_t base =
            effect.data_count - (ResolveBlockBalanced(effect.data_count)
                                     ? kResolveWhiteBalanceDataSize
                                     : 0);
        return base == kResolvePrimariesDataSize ||
                       base == kResolvePrimariesContrastDataSize
                   ? data
                   : nullptr;
    }
    // GR6 -- a contrast's pivot, a hue rotation's matrix.
    if (static_cast<ColorEffectKind>(effect.kind) ==
        ColorEffectKind::Contrast) {
        return effect.data_count == 1 ? data : nullptr;
    }
    if (static_cast<ColorEffectKind>(effect.kind) == ColorEffectKind::Hue) {
        return effect.data_count == 9 ? data : nullptr;
    }
    // Curves: four blocks of a channel's points. GR8 -- hue curves: six, of
    // two points more each.
    const bool hueCurves =
        static_cast<ColorEffectKind>(effect.kind) == ColorEffectKind::HueCurves;
    const int32_t blocks = hueCurves ? 6 : 4;
    const int32_t most = kMaxColorCurvePoints + (hueCurves ? 2 : 0);
    int32_t used = 0;
    for (int32_t block = 0; block < blocks; ++block) {
        if (used >= effect.data_count) {
            return nullptr;
        }
        const float points = data[used];
        if (!std::isfinite(points) || points < 0.0f ||
            points > static_cast<float>(most)) {
            return nullptr;
        }
        used += 1 + (3 * static_cast<int32_t>(points));
    }
    return used == effect.data_count ? data : nullptr;
}

float SignedPower(float value, float exponent) {
    return value < 0.0f ? -std::pow(-value, exponent)
                        : std::pow(value, exponent);
}

// GR16 -- a Resolve gamma: a negative value keeps its sign in DaVinci YRGB
// and Color Managed v2, and goes through unchanged in ACEScct (measured on
// 14 November 2026, where the mirrored power missed full nodes by 4 codes).
float ResolveGammaPower(float value, float exponent, bool acescct) {
    return acescct && value < 0.0f ? value : SignedPower(value, exponent);
}

std::array<float, 3> MultiplyRows(const float* matrix,
                                  const std::array<float, 3>& value) {
    std::array<float, 3> out{};
    for (size_t row = 0; row < 3; ++row) {
        out[row] = (matrix[row * 3] * value[0]) +
                   (matrix[(row * 3) + 1] * value[1]) +
                   (matrix[(row * 3) + 2] * value[2]);
    }
    return out;
}

// GR16 -- color.resolve_primaries, as shader.metal's applyResolvePrimaries,
// step for step: to the node's values under colour management (DaVinci
// Intermediate in DaVinci Wide Gamut, or ACEScct in AP1 for a node graded
// in ACEScct, block[40]), Resolve's law (ColorEffects.h), and back. `count`
// is the block's size: with a contrast from
// kResolvePrimariesContrastDataSize on, with a temperature and tint when
// ResolveBlockBalanced says so.
std::array<float, 3> ApplyResolvePrimaries(std::array<float, 3> color,
                                           const float* block, int32_t count) {
    const bool contrasted = count >= kResolvePrimariesContrastDataSize;
    const auto space =
        static_cast<ColorGradeSpace>(static_cast<int32_t>(block[0]));
    const bool acescct = block[40] != 0.0f;
    // Under colour management the working value goes to the node's values;
    // an engine grading in ACEScct already holds a node's in ACEScct.
    const bool converted = space != ColorGradeSpace::Display &&
                           !(acescct && space == ColorGradeSpace::AcesCct);
    std::array<float, 3> node = color;
    if (converted) {
        std::array<float, 3> light = color;
        if (space == ColorGradeSpace::AcesCct) {
            for (float& channel : light)
                channel = static_cast<float>(DecodeAcesCct(channel));
        }
        const std::array<float, 3> wide = MultiplyRows(block + 1, light);
        for (size_t row = 0; row < 3; ++row) {
            node[row] = static_cast<float>(
                acescct ? EncodeAcesCct(wide[row])
                        : EncodeDaVinciIntermediate(wide[row]));
        }
    }
    if (ResolveBlockBalanced(count)) {
        // The temperature and tint come first, on the node's values; in
        // ACEScct, on AP0 light taken to the power 1/2.4.
        const float* matrix = block + count - kResolveWhiteBalanceDataSize;
        if (acescct) {
            std::array<float, 3> light{};
            for (size_t row = 0; row < 3; ++row)
                light[row] = static_cast<float>(DecodeAcesCct(node[row]));
            std::array<float, 3> power =
                MultiplyRows(kResolveAp1ToAp0.data(), light);
            for (float& channel : power)
                channel = SignedPower(channel, 1.0f / kResolveAcesBalancePower);
            power = MultiplyRows(matrix, power);
            for (float& channel : power)
                channel = SignedPower(channel, kResolveAcesBalancePower);
            light = MultiplyRows(kResolveAp0ToAp1.data(), power);
            for (size_t row = 0; row < 3; ++row)
                node[row] = static_cast<float>(EncodeAcesCct(light[row]));
        } else {
            node = MultiplyRows(matrix, node);
        }
    }
    std::array<float, 3> out{};
    for (size_t channel = 0; channel < 3; ++channel) {
        out[channel] = ResolveGammaPower(
            block[19 + channel] + (block[22 + channel] * node[channel]) +
                block[25 + channel],
            block[28 + channel], acescct);
    }
    const float target = ResolveGammaPower(
        block[31] + (block[32] * Luma(node)) + block[34], block[33], acescct);
    const float lumMix = block[35];
    const int32_t mode = contrasted ? static_cast<int32_t>(block[41]) : 0;
    const float* curve = block + 43;
    const float end = contrasted ? block[42] : 1.0f;
    if (mode == 1) {
        // Without the aware tools the contrast acts on each channel before
        // the luma is taken back, toward the contrast of the masters' luma.
        for (float& channel : out)
            channel = ApplyResolveContrastCurve(curve, end, channel);
        const float aim = ApplyResolveContrastCurve(curve, end, target);
        const float shift = lumMix * (aim - Luma(out));
        for (float& channel : out) channel += shift;
    } else {
        const float shift = lumMix * (target - Luma(out));
        for (float& channel : out) channel += shift;
    }
    if (mode == 2) {
        // With them, after the masters' luma: on HLG codes, with a luma of
        // its own taken back the same way.
        std::array<float, 3> code{};
        for (size_t channel = 0; channel < 3; ++channel)
            code[channel] = ResolveContrastToHlg(out[channel]);
        const float aim = ApplyResolveContrastCurve(curve, end, Luma(code));
        for (float& channel : code)
            channel = ApplyResolveContrastCurve(curve, end, channel);
        const float shift = lumMix * (aim - Luma(code));
        for (size_t channel = 0; channel < 3; ++channel)
            out[channel] = ResolveContrastFromHlg(code[channel] + shift);
    }
    if (block[36] != 1.0f) {
        // DaVinci YRGB's weights, or Rec.709's in ACEScct (block[37..39]).
        const float luma =
            (block[37] * out[0]) + (block[38] * out[1]) + (block[39] * out[2]);
        for (float& channel : out)
            channel = luma + (block[36] * (channel - luma));
    }
    if (!converted) return out;
    std::array<float, 3> wide{};
    for (size_t channel = 0; channel < 3; ++channel)
        wide[channel] = static_cast<float>(
            acescct ? DecodeAcesCct(out[channel])
                    : DecodeDaVinciIntermediate(out[channel]));
    color = MultiplyRows(block + 10, wide);
    if (space == ColorGradeSpace::AcesCct) {
        for (float& channel : color)
            channel = static_cast<float>(EncodeAcesCct(channel));
    }
    return color;
}

}  // namespace

std::array<float, 3> ApplyResolvedColorGrade(std::array<float, 3> color,
                                             const ResolvedColorGrade& grade) {
    constexpr float kLogSlope = 17.52f;
    for (int32_t index = 0; index < grade.count; ++index) {
        const ResolvedColorEffect& effect = grade.entries[index];
        const float amount = effect.amount;
        switch (static_cast<ColorEffectKind>(effect.kind)) {
            case ColorEffectKind::Exposure:
                for (float& channel : color) {
                    channel += amount / kLogSlope;
                }
                break;
            case ColorEffectKind::Contrast: {
                // GR6 -- about its own pivot when it has one.
                const float* own = StageData(grade, effect);
                const float pivot = own != nullptr ? own[0] : kContrastPivot;
                for (float& channel : color) {
                    channel = (channel - pivot) * (1.0f + amount) + pivot;
                }
                break;
            }
            case ColorEffectKind::HueCurves: {
                // GR8 -- as shader.metal's applyHueCurves, step for step.
                const float* block = StageData(grade, effect);
                if (block == nullptr) break;
                constexpr float kTurn = 6.2831853f;
                const float luma = Luma(color);
                const float cb = (color[2] - luma) / kChromaBlue;
                const float cr = (color[0] - luma) / kChromaRed;
                const float chroma = std::sqrt((cb * cb) + (cr * cr));
                float hue = chroma > 0.0f ? std::atan2(cr, cb) / kTurn : 0.0f;
                hue -= std::floor(hue);
                const float saturation = std::clamp(chroma * 2.0f, 0.0f, 1.0f);
                const float level = std::clamp(luma, 0.0f, 1.0f);
                const auto next = [](const float* current) {
                    return current + 1 +
                           (3 * static_cast<ptrdiff_t>(current[0]));
                };
                const float hueShift = EvaluateHueCurve(block, hue) - 0.5f;
                block = next(block);
                const float satByHue = EvaluateHueCurve(block, hue) * 2.0f;
                block = next(block);
                const float lumByHue = EvaluateHueCurve(block, hue) - 0.5f;
                block = next(block);
                const float satByLum = EvaluateHueCurve(block, level) * 2.0f;
                block = next(block);
                const float satBySat =
                    EvaluateHueCurve(block, saturation) * 2.0f;
                block = next(block);
                const float lumBySat =
                    EvaluateHueCurve(block, saturation) - 0.5f;
                const float angle = hueShift * kTurn;
                const float cosine = std::cos(angle);
                const float sine = std::sin(angle);
                const float factor = satByHue * satByLum * satBySat;
                const float turnedCb = ((cb * cosine) - (cr * sine)) * factor;
                const float turnedCr = ((cb * sine) + (cr * cosine)) * factor;
                const float lifted =
                    luma +
                    (lumByHue *
                     std::clamp(saturation / kHueCurveLumaFade, 0.0f, 1.0f)) +
                    lumBySat;
                color[0] = lifted + (kChromaRed * turnedCr);
                color[2] = lifted + (kChromaBlue * turnedCb);
                color[1] =
                    (lifted - (0.2126f * color[0]) - (0.0722f * color[2])) /
                    0.7152f;
                break;
            }
            case ColorEffectKind::Hue: {
                // GR6 -- the rotation's matrix, as shader.metal's applyHue.
                const float* matrix = StageData(grade, effect);
                if (matrix == nullptr) break;
                const std::array<float, 3> input = color;
                for (int row = 0; row < 3; ++row) {
                    color[static_cast<size_t>(row)] =
                        matrix[row * 3] * input[0] +
                        matrix[1 + row * 3] * input[1] +
                        matrix[2 + row * 3] * input[2];
                }
                break;
            }
            case ColorEffectKind::Saturation: {
                const float luma = Luma(color);
                for (float& channel : color) {
                    channel = luma + (channel - luma) * (1.0f + amount);
                }
                break;
            }
            case ColorEffectKind::Vibrance: {
                const float luma = Luma(color);
                const auto [minimum, maximum] =
                    std::minmax_element(color.begin(), color.end());
                const float average = (color[0] + color[1] + color[2]) / 3.0f;
                const float saturation =
                    std::clamp((*maximum - *minimum) /
                                   std::max(std::abs(average), 0.0001f),
                               0.0f, 1.0f);
                const float weight = 1.0f - saturation;
                for (float& channel : color) {
                    channel += weight * amount * (channel - luma);
                }
                break;
            }
            case ColorEffectKind::Temperature: {
                // G7 -- to linear light, the adaptation, and back, as
                // shader.metal's applyTemperature.
                const float* block = StageData(grade, effect);
                if (block == nullptr) break;
                const auto space = static_cast<ColorGradeSpace>(
                    static_cast<int32_t>(block[0]));
                std::array<float, 3> light = color;
                for (float& channel : light) {
                    if (space == ColorGradeSpace::AcesCct) {
                        channel = static_cast<float>(DecodeAcesCct(channel));
                    } else if (space == ColorGradeSpace::Display) {
                        channel = std::copysign(
                            std::pow(std::abs(channel), 2.4f), channel);
                    }
                }
                for (int row = 0; row < 3; ++row) {
                    color[static_cast<size_t>(row)] =
                        block[1 + row * 3] * light[0] +
                        block[2 + row * 3] * light[1] +
                        block[3 + row * 3] * light[2];
                }
                for (float& channel : color) {
                    if (space == ColorGradeSpace::AcesCct) {
                        channel = static_cast<float>(EncodeAcesCct(channel));
                    } else if (space == ColorGradeSpace::Display) {
                        channel = std::copysign(
                            std::pow(std::abs(channel), 1.0f / 2.4f), channel);
                    }
                }
                break;
            }
            case ColorEffectKind::Tint:
                color[1] -= (amount * 0.5f) / kLogSlope;
                break;
            case ColorEffectKind::Highlights: {
                const float mask = Smoothstep(0.4f, 0.9f, Luma(color));
                for (float& channel : color) {
                    channel -= amount * mask * 0.5f;
                }
                break;
            }
            case ColorEffectKind::Shadows: {
                const float mask = 1.0f - Smoothstep(0.05f, 0.5f, Luma(color));
                for (float& channel : color) {
                    channel += amount * mask * 0.5f;
                }
                break;
            }
            case ColorEffectKind::Wheels: {
                // Lift L, gain K, then the exponent 1/G (ColorEffects.h).
                const float* wheel = StageData(grade, effect);
                if (wheel == nullptr) {
                    break;
                }
                for (size_t channel = 0; channel < 3; ++channel) {
                    float value = (color[channel] *
                                   (wheel[3 + channel] - wheel[channel])) +
                                  wheel[channel];
                    if (value > 0.0f) {
                        value = std::pow(value, wheel[6 + channel]);
                    }
                    color[channel] = value;
                }
                break;
            }
            case ColorEffectKind::ResolvePrimaries: {
                const float* block = StageData(grade, effect);
                if (block != nullptr)
                    color =
                        ApplyResolvePrimaries(color, block, effect.data_count);
                break;
            }
            case ColorEffectKind::Curves: {
                // Master on all three channels, then r, g and b.
                const float* block = StageData(grade, effect);
                if (block == nullptr) {
                    break;
                }
                const auto next = [](const float* current) {
                    return current + 1 +
                           (3 * static_cast<ptrdiff_t>(current[0]));
                };
                for (float& channel : color) {
                    channel = EvaluateColorCurve(block, channel);
                }
                for (float& channel : color) {
                    block = next(block);
                    channel = EvaluateColorCurve(block, channel);
                }
                break;
            }
        }
    }
    return color;
}

bool BuildColorGradeCube(const ResolvedColorGrade& grade, std::string& cube,
                         std::string& error) {
    cube.clear();
    error.clear();
    if (grade.count < 0 || grade.count > kMaxColorEffectsPerClip) {
        error = "invalid resolved clip grade depth";
        return false;
    }
    for (int32_t index = 0; index < grade.count; ++index) {
        if (!std::isfinite(grade.entries[index].amount)) {
            error = "non-finite clip grade amount";
            return false;
        }
    }
    if (grade.data_count < 0 || grade.data_count > kMaxColorGradeData ||
        !std::all_of(grade.data.begin(), grade.data.begin() + grade.data_count,
                     [](float value) { return std::isfinite(value); })) {
        error = "non-finite clip grade wheel or curve";
        return false;
    }
    constexpr int32_t kEdge = 65;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "TITLE \"CUTMACHINE clip grade\"\n"
           << "LUT_3D_SIZE " << kEdge << '\n'
           << "DOMAIN_MIN 0.0 0.0 0.0\n"
           << "DOMAIN_MAX 1.0 1.0 1.0\n"
           << std::fixed << std::setprecision(10);
    // .cube stores red as the fastest-moving axis, matching the OCIO LUT.
    for (int32_t blue = 0; blue < kEdge; ++blue) {
        for (int32_t green = 0; green < kEdge; ++green) {
            for (int32_t red = 0; red < kEdge; ++red) {
                const auto color = ApplyResolvedColorGrade(
                    {static_cast<float>(red) / (kEdge - 1),
                     static_cast<float>(green) / (kEdge - 1),
                     static_cast<float>(blue) / (kEdge - 1)},
                    grade);
                if (!std::all_of(color.begin(), color.end(), [](float channel) {
                        return std::isfinite(channel);
                    })) {
                    error = "clip grade produces a non-finite color";
                    return false;
                }
                output << std::clamp(color[0], 0.0f, 1.0f) << ' '
                       << std::clamp(color[1], 0.0f, 1.0f) << ' '
                       << std::clamp(color[2], 0.0f, 1.0f) << '\n';
            }
        }
    }
    cube = output.str();
    return true;
}

GpuClipGradeParameters PackColorGradeForGpu(const ResolvedColorGrade& grade) {
    GpuClipGradeParameters packed{};
    packed.count = std::clamp(grade.count, 0, kMaxColorEffectsPerClip);
    const int32_t data = std::clamp(grade.data_count, 0, kMaxColorGradeData);
    for (int32_t index = 0; index < packed.count; ++index) {
        const ResolvedColorEffect& effect =
            grade.entries[static_cast<size_t>(index)];
        GpuColorGradeEntry& entry = packed.entries[index];
        entry = {effect.kind, effect.amount, effect.data_offset,
                 effect.data_count};
        const auto kind = static_cast<ColorEffectKind>(effect.kind);
        const bool carriesData =
            kind == ColorEffectKind::Wheels ||
            kind == ColorEffectKind::Curves || kind == ColorEffectKind::Hue ||
            kind == ColorEffectKind::HueCurves ||
            kind == ColorEffectKind::ResolvePrimaries ||
            (kind == ColorEffectKind::Contrast && effect.data_count != 0);
        if (carriesData && StageData(grade, effect) == nullptr) {
            // The shader trusts every offset it is given; a stage that
            // does not describe its block is sent as a no-op instead.
            entry = {-1, 0.0f, 0, 0};
        }
    }
    std::copy(grade.data.begin(), grade.data.begin() + data, packed.data);
    return packed;
}
