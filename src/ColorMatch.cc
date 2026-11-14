#include "ColorMatch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace {

// ColorEffects.cc's constants: an amount moves the grading space by
// amount / 17.52, and color.tint moves green by amount * 0.5 / 17.52.
constexpr double kLogSlope = 17.52;
constexpr double kTintStrength = 0.5;
// The luma color.saturation preserves.
constexpr double kRed = 0.2126;
constexpr double kGreen = 0.7152;
constexpr double kBlue = 0.0722;
// Below this mean distance to luma there is no colour for a factor to
// scale: a thousandth of the grading range.
constexpr double kMinimumChroma = 1e-3;
constexpr int32_t kAmountDenominator = 1000;

double Luma(const std::array<float, 3>& value) {
    return kRed * value[0] + kGreen * value[1] + kBlue * value[2];
}

// Nearest rank, as ColorMeasure.cc reads its percentiles.
double Percentile(const std::vector<double>& sorted, int64_t percent) {
    const size_t count = sorted.size();
    size_t rank = static_cast<size_t>(
        (static_cast<uint64_t>(percent) * count + 99) / 100);
    rank = std::clamp<size_t>(rank, 1, count);
    return sorted[rank - 1];
}

// The midtones: luma between the 25th and 75th percentiles, ends included.
std::vector<size_t> Midtones(const std::vector<std::array<float, 3>>& values,
                             double& median) {
    std::vector<double> lumas;
    lumas.reserve(values.size());
    for (const auto& value : values) lumas.push_back(Luma(value));
    std::vector<double> sorted = lumas;
    std::sort(sorted.begin(), sorted.end());
    median = Percentile(sorted, 50);
    const double low = Percentile(sorted, 25);
    const double high = Percentile(sorted, 75);
    std::vector<size_t> selected;
    for (size_t index = 0; index < lumas.size(); ++index) {
        if (lumas[index] >= low && lumas[index] <= high)
            selected.push_back(index);
    }
    return selected;
}

// The mean distance to luma once the midtones' mean cast is taken away: how
// spread the colours are around the picture's balance. The saturation
// factor scales it exactly; a cast, which moves every value by the same
// vector, does not touch it.
double Spread(const std::vector<std::array<float, 3>>& values,
              const std::vector<size_t>& midtones) {
    std::array<double, 3> cast{};
    for (size_t index : midtones) {
        const double luma = Luma(values[index]);
        for (int channel = 0; channel < 3; ++channel)
            cast[channel] += values[index][channel] - luma;
    }
    for (double& channel : cast)
        channel /= static_cast<double>(std::max<size_t>(midtones.size(), 1));
    double sum = 0.0;
    for (const auto& value : values) {
        const double luma = Luma(value);
        double squared = 0.0;
        for (int channel = 0; channel < 3; ++channel) {
            const double away = value[channel] - luma - cast[channel];
            squared += away * away;
        }
        sum += std::sqrt(squared);
    }
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

EffectParamValue Thousandths(double value) {
    const int64_t num = std::llround(value * kAmountDenominator);
    const int64_t divisor = std::gcd(num < 0 ? -num : num,
                                     static_cast<int64_t>(kAmountDenominator));
    const int64_t reduced = divisor == 0 ? 1 : divisor;
    return {static_cast<int32_t>(num / reduced),
            static_cast<int32_t>(kAmountDenominator / reduced)};
}

double ToDouble(const EffectParamValue& value) {
    return value.den == 0 ? 0.0
                          : static_cast<double>(value.num) /
                                static_cast<double>(value.den);
}

// A mid grey where the grade acts: ACEScct's code for 18 %, 18 % itself in
// a linear working space, a mid display signal without management.
float MidGrey(ColorGradeSpace space) {
    switch (space) {
        case ColorGradeSpace::AcesCct:
            return 0.4135884f;
        case ColorGradeSpace::LinearRec709:
        case ColorGradeSpace::LinearRec2020:
            return 0.18f;
        case ColorGradeSpace::Display:
            break;
    }
    return 0.5f;
}

// R - B of a mid grey once a temperature stage at `kelvin` has acted.
double TemperatureCast(double kelvin, const ColorGradeContext& context) {
    const float grey = MidGrey(context.space);
    const ResolvedColorGrade grade = ResolveColorGrade(
        {{"",
          "color.temperature",
          {{"kelvin", {static_cast<int32_t>(std::lround(kelvin)), 1}}}}},
        context);
    const std::array<float, 3> moved =
        ApplyResolvedColorGrade({grey, grey, grey}, grade);
    return static_cast<double>(moved[0] - moved[2]);
}

// A knob's neutral value.
double Neutral(const std::string& type, const ColorGradeContext& context) {
    return type == "color.temperature"
               ? static_cast<double>(ColorGradeNeutralKelvin(context))
               : 0.0;
}

const char* KnobName(const std::string& type) {
    return type == "color.temperature" ? "kelvin" : "amount";
}

}  // namespace

ColorMatchReading ReadColorMatch(
    const std::vector<std::array<float, 3>>& values) {
    ColorMatchReading reading;
    reading.pixels = static_cast<int64_t>(values.size());
    if (values.empty()) return reading;
    const std::vector<size_t> midtones = Midtones(values, reading.median_luma);
    double redMinusBlue = 0.0;
    double greenMagenta = 0.0;
    for (size_t index : midtones) {
        const auto& value = values[index];
        redMinusBlue += value[0] - value[2];
        greenMagenta += value[1] - (value[0] + value[2]) / 2.0;
    }
    const double count = static_cast<double>(midtones.size());
    reading.red_minus_blue = redMinusBlue / count;
    reading.green_magenta = greenMagenta / count;
    reading.spread = Spread(values, midtones);
    return reading;
}

bool SolveColorMatch(const std::vector<std::array<float, 3>>& clip,
                     const std::vector<std::array<float, 3>>& reference,
                     const ColorMatchAspects& aspects, ColorMatchGuess& guess,
                     std::string& error) {
    guess = ColorMatchGuess{};
    error.clear();
    if (clip.empty() || reference.empty()) {
        error = "nothing to compare: a picture or a region holds no pixel";
        return false;
    }
    if (!aspects.exposure && !aspects.balance && !aspects.saturation) {
        error = "nothing to match: name exposure, balance or saturation";
        return false;
    }
    const ColorMatchReading target = ReadColorMatch(reference);
    const ColorMatchReading source = ReadColorMatch(clip);
    const bool scaled = aspects.saturation && target.spread >= kMinimumChroma &&
                        source.spread >= kMinimumChroma;
    guess.saturation_undefined = aspects.saturation && !scaled;
    const double k = scaled ? target.spread / source.spread : 1.0;
    if (scaled) guess.saturation_factor = k;
    // After the factor has scaled the clip's own cast, the offsets land it
    // on the reference's.
    if (aspects.balance) {
        guess.offset =
            (target.red_minus_blue - k * source.red_minus_blue) / 2.0;
        guess.green = k * source.green_magenta - target.green_magenta;
    }
    // The factor keeps luma; the offsets move it by their weighted sum.
    if (aspects.exposure) {
        const double shift =
            kRed * guess.offset - kBlue * guess.offset - kGreen * guess.green;
        guess.exposure = target.median_luma - (source.median_luma + shift);
    }
    return true;
}

ColorMatchKnobs ReadColorMatchKnobs(const std::vector<ClipEffect>& stack,
                                    const ColorGradeContext& context) {
    ColorMatchKnobs knobs;
    knobs.kelvin = Neutral("color.temperature", context);
    const auto first = [&](const char* type, double& out) {
        for (const ClipEffect& effect : stack) {
            if (effect.type != type) continue;
            const auto param = effect.params.find(KnobName(type));
            out = param != effect.params.end() ? ToDouble(param->second)
                                               : Neutral(type, context);
            return;
        }
    };
    first("color.exposure", knobs.exposure);
    first("color.temperature", knobs.kelvin);
    first("color.tint", knobs.tint);
    first("color.saturation", knobs.saturation);
    return knobs;
}

ColorMatchKnobs ApplyColorMatchGuess(const ColorMatchKnobs& knobs,
                                     const ColorMatchGuess& guess,
                                     const ColorMatchAspects& aspects,
                                     const ColorGradeContext& context) {
    ColorMatchKnobs out = knobs;
    if (aspects.exposure) out.exposure += guess.exposure * kLogSlope;
    if (aspects.saturation && !guess.saturation_undefined)
        out.saturation =
            (1.0 + knobs.saturation) * guess.saturation_factor - 1.0;
    if (aspects.balance) {
        out.tint += guess.green * kLogSlope / kTintStrength;
    }
    // No cast asked, no Kelvin to look for: a bisection would land a Kelvin
    // or so off the clip's own and add a stage for nothing.
    if (aspects.balance && guess.offset != 0.0) {
        // The Kelvin whose adaptation gives a mid grey the cast the clip's
        // own temperature gives it, plus the guess's: R - B grows with the
        // Kelvin, so a bisection on its logarithm finds it.
        const double wanted =
            TemperatureCast(knobs.kelvin, context) + 2.0 * guess.offset;
        double low = std::log(static_cast<double>(kMinimumKelvin));
        double high = std::log(static_cast<double>(kMaximumKelvin));
        for (int step = 0; step < 40; ++step) {
            const double middle = (low + high) / 2.0;
            if (TemperatureCast(std::exp(middle), context) < wanted)
                low = middle;
            else
                high = middle;
        }
        out.kelvin = std::exp((low + high) / 2.0);
    }
    return out;
}

std::vector<ClipEffect> PlaceColorMatch(
    const std::vector<ClipEffect>& stack, const ColorMatchKnobs& knobs,
    const ColorGradeContext& context,
    std::vector<ColorMatchPlacement>* placements) {
    std::vector<ClipEffect> result = stack;
    if (placements != nullptr) placements->clear();
    const auto place = [&](const char* type, const EffectParamValue& value,
                           bool neutral) {
        for (ClipEffect& effect : result) {
            if (effect.type != type) continue;
            effect.params[KnobName(type)] = value;
            if (placements != nullptr) placements->push_back({type, true});
            return;
        }
        if (neutral) return;
        ClipEffect added;
        added.type = type;
        added.params[KnobName(type)] = value;
        result.push_back(std::move(added));
        if (placements != nullptr) placements->push_back({type, false});
    };
    // In the order the correction acts: the factor, the cast, the exposure.
    const EffectParamValue saturation = Thousandths(knobs.saturation);
    place("color.saturation", saturation, saturation.num == 0);
    const auto kelvin = static_cast<int32_t>(std::lround(
        std::clamp(knobs.kelvin, static_cast<double>(kMinimumKelvin),
                   static_cast<double>(kMaximumKelvin))));
    place("color.temperature", {kelvin, 1},
          kelvin == ColorGradeNeutralKelvin(context));
    const EffectParamValue tint = Thousandths(knobs.tint);
    place("color.tint", tint, tint.num == 0);
    const EffectParamValue exposure = Thousandths(knobs.exposure);
    place("color.exposure", exposure, exposure.num == 0);
    return result;
}

bool RefineColorMatch(const ColorMatchDisplay& target,
                      const ColorMatchAspects& aspects, bool saturationDefined,
                      const ColorMatchEvaluator& evaluate,
                      ColorMatchKnobs& knobs, std::string& error) {
    // Five hundredths of measure_color's unit (0.05 % of luma, 0.05 of a
    // chroma code); a few secant steps per knob, each a render of the whole
    // picture.
    constexpr double kTolerance = 5.0;
    constexpr int kSteps = 5;
    constexpr int kCycles = 3;
    // The temperature is moved on the logarithm of its Kelvin, which the
    // cast follows far more evenly than the Kelvin itself.
    struct Variables {
        double exposure;
        double logKelvin;
        double tint;
        double saturation;
    };
    const auto toKnobs = [](const Variables& value) {
        return ColorMatchKnobs{value.exposure, std::exp(value.logKelvin),
                               value.tint, value.saturation};
    };
    // Where each knob may go: sane grading, not a secant's extrapolation,
    // nor a closed form read on two contents that share nothing.
    const double coolest = std::log(static_cast<double>(kMinimumKelvin));
    const double warmest = std::log(static_cast<double>(kMaximumKelvin));
    Variables current{
        std::clamp(knobs.exposure, -8.0, 8.0),
        std::clamp(std::log(std::max(knobs.kelvin, 1.0)), coolest, warmest),
        std::clamp(knobs.tint, -4.0, 4.0),
        std::clamp(knobs.saturation, -1.0, 2.0)};
    ColorMatchDisplay shown;
    if (!evaluate(toKnobs(current), shown, error)) return false;

    const bool balance =
        aspects.balance && target.has_midtones && shown.has_midtones;
    const bool saturation = aspects.saturation && saturationDefined &&
                            target.saturation > 0.0 && shown.saturation > 0.0;
    // What every step is judged by, whatever knob it moved: the sum of the
    // active numbers' distances to the reference.
    const auto cost = [&](const ColorMatchDisplay& display) {
        double total = 0.0;
        if (aspects.exposure)
            total += std::abs(display.luma_p50 - target.luma_p50);
        if (balance)
            total += std::abs(display.warmth - target.warmth) +
                     std::abs(display.green_magenta - target.green_magenta);
        if (saturation)
            total += std::abs(display.saturation - target.saturation);
        return total;
    };
    Variables best = current;
    double bestCost = cost(shown);
    const auto solve = [&](double Variables::*member, double probe, double low,
                           double high, double ColorMatchDisplay::*number) {
        double x0 = current.*member;
        double f0 = shown.*number - target.*number;
        if (std::abs(f0) <= kTolerance) return true;
        double x1 = std::clamp(x0 + probe, low, high);
        for (int step = 0; step < kSteps; ++step) {
            Variables trial = current;
            trial.*member = x1;
            ColorMatchDisplay rendered;
            if (!evaluate(toKnobs(trial), rendered, error)) return false;
            const double f1 = rendered.*number - target.*number;
            if (std::abs(f1) < std::abs(shown.*number - target.*number)) {
                current = trial;
                shown = rendered;
            }
            if (cost(rendered) < bestCost) {
                bestCost = cost(rendered);
                best = trial;
            }
            if (std::abs(f1) <= kTolerance || f1 == f0) break;
            const double next = x1 - f1 * (x1 - x0) / (f1 - f0);
            x0 = x1;
            f0 = f1;
            x1 = std::clamp(next, low, high);
            if (x1 == x0) break;
        }
        return true;
    };
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        if (aspects.exposure && !solve(&Variables::exposure, 0.1, -8.0, 8.0,
                                       &ColorMatchDisplay::luma_p50))
            return false;
        if (balance && (!solve(&Variables::logKelvin, 0.05, coolest, warmest,
                               &ColorMatchDisplay::warmth) ||
                        !solve(&Variables::tint, 0.05, -4.0, 4.0,
                               &ColorMatchDisplay::green_magenta)))
            return false;
        // Under -1 the factor turns negative and inverts colours.
        if (saturation && !solve(&Variables::saturation, 0.05, -1.0, 2.0,
                                 &ColorMatchDisplay::saturation))
            return false;
        const auto near = [&](double ColorMatchDisplay::*number) {
            return std::abs(shown.*number - target.*number) <= kTolerance;
        };
        if ((!aspects.exposure || near(&ColorMatchDisplay::luma_p50)) &&
            (!balance || (near(&ColorMatchDisplay::warmth) &&
                          near(&ColorMatchDisplay::green_magenta))) &&
            (!saturation || near(&ColorMatchDisplay::saturation)))
            break;
    }
    knobs = toKnobs(best);
    return true;
}
