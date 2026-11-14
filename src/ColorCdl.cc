#include "ColorCdl.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>

namespace OCIO = OCIO_NAMESPACE;

namespace {

// The shader's constants (ColorEffects.cc, shader.metal): what a stage
// moves a value by is what it renders, float for float.
constexpr float kLogSlope = 17.52f;

// The CDL's and color.saturation's luma weights (Rec.709).
constexpr std::array<double, 3> kCdlLuma = {0.2126, 0.7152, 0.0722};

// Millionths: the step gain and gamma are rounded to on import.
constexpr int32_t kLogDenominator = 1000000;

const char* TypeOf(int32_t kind) {
    for (const ColorEffectRegistryEntry& entry : ColorEffectRegistry())
        if (static_cast<int32_t>(entry.kind) == kind) return entry.type;
    return "an unknown stage";
}

std::string Number(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(std::numeric_limits<double>::max_digits10);
    stream << value;
    return stream.str();
}

// The shortest spelling of `value` that reads back as it, for Resolve's
// strings and the error messages.
std::string Shortest(double value) {
    for (int digits = 1; digits <= 17; ++digits) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.*g", digits, value);
        if (std::strtod(text, nullptr) == value) return text;
    }
    return Number(value);
}

EffectParamValue Reduced(int64_t num, int64_t den) {
    const int64_t divisor = std::gcd(num, den);
    return {static_cast<int32_t>(num / divisor),
            static_cast<int32_t>(den / divisor)};
}

// A knob's value as the wheel's master and its channel, both in [-1, 1]:
// the channel alone when it fits, else the master takes the middle of the
// three channels' range.
bool SplitWheel(const std::array<double, 3>& values, double& master,
                std::array<double, 3>& channels) {
    master = 0.0;
    channels = values;
    const auto fits = [](double value) {
        return value >= -kColorWheelLimit && value <= kColorWheelLimit;
    };
    if (std::all_of(values.begin(), values.end(), fits)) return true;
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    master = (*low + *high) / 2.0;
    for (size_t channel = 0; channel < 3; ++channel)
        channels[channel] = values[channel] - master;
    return fits(master) && std::all_of(channels.begin(), channels.end(), fits);
}

// The same split on exact fractions, for the lift: the file's decimals
// stay exact, master and channel alike.
bool SplitExact(const std::array<EffectParamValue, 3>& values,
                EffectParamValue& master,
                std::array<EffectParamValue, 3>& channels) {
    const auto fits = [](const EffectParamValue& value) {
        return std::abs(static_cast<int64_t>(value.num)) <=
               static_cast<int64_t>(value.den) * kColorWheelLimit;
    };
    master = {0, 1};
    channels = values;
    if (std::all_of(values.begin(), values.end(), fits)) return true;
    const auto less = [](const EffectParamValue& left,
                         const EffectParamValue& right) {
        return static_cast<int64_t>(left.num) * right.den <
               static_cast<int64_t>(right.num) * left.den;
    };
    const auto [low, high] =
        std::minmax_element(values.begin(), values.end(), less);
    const int64_t den = int64_t{2} * low->den * high->den;
    const int64_t num = static_cast<int64_t>(low->num) * high->den +
                        static_cast<int64_t>(high->num) * low->den;
    const int64_t divisor = std::gcd(num, den);
    if (den / divisor > std::numeric_limits<int32_t>::max() ||
        std::abs(num / divisor) > std::numeric_limits<int32_t>::max())
        return false;
    master = Reduced(num, den);
    for (size_t channel = 0; channel < 3; ++channel) {
        const int64_t channelDen =
            static_cast<int64_t>(values[channel].den) * master.den;
        const int64_t channelNum =
            static_cast<int64_t>(values[channel].num) * master.den -
            static_cast<int64_t>(master.num) * values[channel].den;
        const int64_t reduce = std::gcd(channelNum, channelDen);
        if (channelDen / reduce > std::numeric_limits<int32_t>::max() ||
            std::abs(channelNum / reduce) > std::numeric_limits<int32_t>::max())
            return false;
        channels[channel] = Reduced(channelNum, channelDen);
    }
    return fits(master) && std::all_of(channels.begin(), channels.end(), fits);
}

bool RoundedLog(double value, EffectParamValue& fraction) {
    const double scaled = std::round(value * kLogDenominator);
    if (!std::isfinite(scaled) ||
        std::abs(scaled) > std::numeric_limits<int32_t>::max())
        return false;
    fraction = Reduced(static_cast<int64_t>(scaled), kLogDenominator);
    return true;
}

// A file's decimal exactly; a number with more than nine decimals -- one a
// fold computed, not one a person typed -- to the nearest billionth.
bool NearestFraction(double value, EffectParamValue& fraction) {
    if (DecimalFraction(value, fraction)) return true;
    const double scaled = std::round(value * 1e9);
    if (!std::isfinite(scaled) ||
        std::abs(scaled) > std::numeric_limits<int32_t>::max())
        return false;
    fraction = Reduced(static_cast<int64_t>(scaled), 1000000000);
    return true;
}

double Value(const EffectParamValue& fraction) {
    return static_cast<double>(fraction.num) / fraction.den;
}

}  // namespace

const char* ColorCdlSpaceId(ColorGradeSpace space) {
    switch (space) {
        case ColorGradeSpace::AcesCct:
            return "acescct";
        case ColorGradeSpace::LinearRec709:
            return "linear_rec709";
        case ColorGradeSpace::LinearRec2020:
            return "linear_rec2020";
        case ColorGradeSpace::Display:
            break;
    }
    return "display";
}

const char* ColorCdlSpaceName(ColorGradeSpace space) {
    switch (space) {
        case ColorGradeSpace::AcesCct:
            return "ACEScct";
        case ColorGradeSpace::LinearRec709:
            return "Linear Rec.709";
        case ColorGradeSpace::LinearRec2020:
            return "Linear Rec.2020";
        case ColorGradeSpace::Display:
            break;
    }
    return "Display";
}

bool ParseColorCdlSpace(const std::string& id, ColorGradeSpace& space) {
    for (ColorGradeSpace candidate :
         {ColorGradeSpace::AcesCct, ColorGradeSpace::LinearRec709,
          ColorGradeSpace::LinearRec2020, ColorGradeSpace::Display}) {
        if (id == ColorCdlSpaceId(candidate)) {
            space = candidate;
            return true;
        }
    }
    return false;
}

bool FoldColorCdl(const ResolvedColorGrade& grade, ColorCdl& cdl,
                  std::string& error) {
    cdl = ColorCdl{};
    error.clear();
    std::array<double, 3> slope = {1.0, 1.0, 1.0};
    std::array<double, 3> offset = {0.0, 0.0, 0.0};
    bool powered = false;
    bool saturated = false;
    // One affine move per channel, composed where the order allows it.
    const auto affine = [&](int32_t kind, const std::array<double, 3>& a,
                            const std::array<double, 3>& b) {
        const bool identity = a == std::array<double, 3>{1.0, 1.0, 1.0} &&
                              b == std::array<double, 3>{0.0, 0.0, 0.0};
        if (identity) return true;
        if (powered) {
            error = std::string(TypeOf(kind)) +
                    " comes after a wheel's gamma, which a CDL applies last "
                    "in its slope, offset and power";
            return false;
        }
        std::array<double, 3> shift = b;
        if (saturated) {
            // After a saturation s, a move a * y + b with the same slope on
            // every channel is the move a * x + b' before it, with
            // b' = (b - (1 - s) * (w . b)) / s: scaling keeps luma where the
            // saturation pivots, and an offset's luma w . b passes through
            // it unscaled. A slope that differs by channel weighs the
            // channels differently from luma, and has no place before it.
            const double factor = cdl.saturation;
            if (a[0] != a[1] || a[1] != a[2]) {
                error = std::string(TypeOf(kind)) +
                        " scales the channels apart after color.saturation, "
                        "which a CDL applies last";
                return false;
            }
            const double luma =
                kCdlLuma[0] * b[0] + kCdlLuma[1] * b[1] + kCdlLuma[2] * b[2];
            const bool uniform = b[0] == b[1] && b[1] == b[2];
            if (!uniform && !(factor > 0.0)) {
                error = std::string(TypeOf(kind)) +
                        " moves the channels apart after a saturation of 0, "
                        "which no CDL undoes";
                return false;
            }
            if (!uniform) {
                for (size_t channel = 0; channel < 3; ++channel)
                    shift[channel] =
                        (b[channel] - (1.0 - factor) * luma) / factor;
            }
        }
        for (size_t channel = 0; channel < 3; ++channel) {
            slope[channel] *= a[channel];
            offset[channel] = a[channel] * offset[channel] + shift[channel];
        }
        return true;
    };
    for (int32_t index = 0; index < grade.count; ++index) {
        const ResolvedColorEffect& effect = grade.entries[index];
        const auto amount = static_cast<double>(effect.amount);
        switch (static_cast<ColorEffectKind>(effect.kind)) {
            case ColorEffectKind::Exposure: {
                const double lift = amount / static_cast<double>(kLogSlope);
                if (!affine(effect.kind, {1.0, 1.0, 1.0}, {lift, lift, lift}))
                    return false;
                break;
            }
            case ColorEffectKind::Contrast: {
                // GR6 -- about the stage's own pivot when it carries one.
                const bool own = effect.data_count == 1 &&
                                 effect.data_offset >= 0 &&
                                 effect.data_offset < grade.data_count;
                const double pivot = static_cast<double>(
                    own ? grade.data[static_cast<size_t>(effect.data_offset)]
                        : kContrastPivot);
                const double factor = 1.0 + amount;
                const double shift = pivot * (1.0 - factor);
                if (!affine(effect.kind, {factor, factor, factor},
                            {shift, shift, shift}))
                    return false;
                break;
            }
            case ColorEffectKind::Tint: {
                const double green =
                    -(amount * 0.5) / static_cast<double>(kLogSlope);
                if (!affine(effect.kind, {1.0, 1.0, 1.0}, {0.0, green, 0.0}))
                    return false;
                break;
            }
            case ColorEffectKind::Wheels: {
                if (effect.data_count != 9) break;
                const float* wheel = grade.data.data() + effect.data_offset;
                std::array<double, 3> a{};
                std::array<double, 3> b{};
                std::array<double, 3> exponent{};
                for (size_t channel = 0; channel < 3; ++channel) {
                    b[channel] = wheel[channel];
                    a[channel] = static_cast<double>(wheel[3 + channel]) -
                                 static_cast<double>(wheel[channel]);
                    exponent[channel] = wheel[6 + channel];
                }
                if (!affine(effect.kind, a, b)) return false;
                if (exponent != std::array<double, 3>{1.0, 1.0, 1.0}) {
                    if (powered) {
                        error =
                            "two color.wheels bend the values with a gamma, "
                            "and a CDL holds one power";
                        return false;
                    }
                    if (saturated) {
                        error =
                            "a color.wheels gamma comes after "
                            "color.saturation, which a CDL applies last";
                        return false;
                    }
                    cdl.power = exponent;
                    powered = true;
                }
                break;
            }
            case ColorEffectKind::Saturation:
                cdl.saturation *= 1.0 + amount;
                saturated = true;
                break;
            case ColorEffectKind::Vibrance:
            case ColorEffectKind::Highlights:
            case ColorEffectKind::Shadows:
                if (amount == 0.0) break;
                [[fallthrough]];
            case ColorEffectKind::Temperature:
            case ColorEffectKind::Curves:
            case ColorEffectKind::Hue:
            case ColorEffectKind::HueCurves:
            case ColorEffectKind::ResolvePrimaries:
                error = std::string(TypeOf(effect.kind)) +
                        " has no CDL form: a CDL holds a slope, an offset "
                        "and a power per channel, then a saturation";
                return false;
        }
    }
    for (size_t channel = 0; channel < 3; ++channel) {
        if (slope[channel] < 0.0) {
            error = "the stages fold into a negative slope (" +
                    Shortest(slope[channel]) + "), which a CDL forbids";
            return false;
        }
    }
    if (cdl.saturation < 0.0) {
        error =
            "the stages fold into a negative saturation, which a CDL "
            "forbids";
        return false;
    }
    cdl.slope = slope;
    cdl.offset = offset;
    return true;
}

bool ColorCdlStages(const ColorCdl& cdl, std::vector<ClipEffect>& stages,
                    double& deviation, std::string& error) {
    stages.clear();
    deviation = 0.0;
    error.clear();
    std::array<double, 3> gains{};
    std::array<double, 3> gammas{};
    for (size_t channel = 0; channel < 3; ++channel) {
        const double white = cdl.slope[channel] + cdl.offset[channel];
        if (!(white > 0.0) || !std::isfinite(white)) {
            error = "slope + offset (" + Shortest(white) +
                    ") must be positive: the wheel's gain is the output at "
                    "input 1, a power of two";
            return false;
        }
        if (!(cdl.power[channel] > 0.0) || !std::isfinite(cdl.power[channel])) {
            error =
                "a power must be positive, got " + Shortest(cdl.power[channel]);
            return false;
        }
        gains[channel] = std::log2(white);
        gammas[channel] = -std::log2(cdl.power[channel]);
    }
    if (!(cdl.saturation >= 0.0) || !std::isfinite(cdl.saturation)) {
        error = "a saturation must be zero or more, got " +
                Shortest(cdl.saturation);
        return false;
    }

    ClipEffect wheels;
    wheels.type = "color.wheels";
    const auto put = [&](const std::string& name,
                         const EffectParamValue& value) {
        if (value.num != 0) wheels.params[name] = value;
    };
    const auto triple = [](const std::array<double, 3>& values) {
        return Shortest(values[0]) + ", " + Shortest(values[1]) + ", " +
               Shortest(values[2]);
    };
    const auto pastReach = [&](const char* wheel,
                               const std::array<double, 3>& values) {
        error = std::string("the CDL's ") + wheel + " (" + triple(values) +
                ") lies past what color.wheels reaches, -1 to 1 on a channel "
                "plus -1 to 1 on the master";
        return false;
    };
    // The lift is the offset, exact.
    std::array<EffectParamValue, 3> offsets{};
    for (size_t channel = 0; channel < 3; ++channel) {
        if (!NearestFraction(cdl.offset[channel], offsets[channel])) {
            error = "the CDL's offset " + Shortest(cdl.offset[channel]) +
                    " has no exact fraction";
            return false;
        }
    }
    EffectParamValue liftMaster;
    std::array<EffectParamValue, 3> lifts{};
    if (!SplitExact(offsets, liftMaster, lifts))
        return pastReach("lift", cdl.offset);
    // Gain and gamma are logarithms, rounded.
    const auto logs = [&](const char* wheel,
                          const std::array<double, 3>& values) {
        double master = 0.0;
        std::array<double, 3> channels{};
        if (!SplitWheel(values, master, channels))
            return pastReach(wheel, values);
        for (size_t channel = 0; channel < 3; ++channel) {
            EffectParamValue fraction;
            if (!RoundedLog(channels[channel], fraction))
                return pastReach(wheel, values);
            put(std::string(wheel) + "_" + kColorWheelChannels[channel],
                fraction);
        }
        EffectParamValue fraction;
        if (!RoundedLog(master, fraction)) return pastReach(wheel, values);
        put(std::string(wheel) + "_master", fraction);
        return true;
    };
    for (size_t channel = 0; channel < 3; ++channel)
        put(std::string("lift_") + kColorWheelChannels[channel],
            lifts[channel]);
    put("lift_master", liftMaster);
    if (!logs("gain", gains) || !logs("gamma", gammas)) return false;
    // What the rounded knobs render, against the file.
    const auto param = [&](const std::string& name) {
        const auto found = wheels.params.find(name);
        return found == wheels.params.end() ? 0.0 : Value(found->second);
    };
    for (size_t channel = 0; channel < 3; ++channel) {
        const std::string suffix =
            std::string("_") + kColorWheelChannels[channel];
        const double lift = param("lift" + suffix) + param("lift_master");
        const double white =
            std::exp2(param("gain" + suffix) + param("gain_master"));
        const double power =
            std::exp2(-(param("gamma" + suffix) + param("gamma_master")));
        deviation = std::max({deviation, std::abs(lift - cdl.offset[channel]),
                              std::abs((white - lift) - cdl.slope[channel]),
                              std::abs(power - cdl.power[channel])});
    }
    if (!wheels.params.empty()) stages.push_back(std::move(wheels));
    if (cdl.saturation != 1.0) {
        ClipEffect saturation;
        saturation.type = "color.saturation";
        EffectParamValue factor;
        if (!NearestFraction(cdl.saturation, factor) ||
            factor.num - static_cast<int64_t>(factor.den) <
                -std::numeric_limits<int32_t>::max()) {
            error = "the CDL's saturation " + Shortest(cdl.saturation) +
                    " has no exact fraction";
            return false;
        }
        // amount = saturation - 1, on the fraction: 1.15 is 3/20 exactly.
        saturation.params["amount"] =
            Reduced(static_cast<int64_t>(factor.num) - factor.den, factor.den);
        stages.push_back(std::move(saturation));
    }
    return true;
}

std::vector<ClipEffect> PlaceColorCdl(const std::vector<ClipEffect>& stack,
                                      const std::vector<ClipEffect>& stages,
                                      std::vector<std::string>* replaced) {
    if (replaced != nullptr) replaced->clear();
    const auto primary = [](const std::string& type) {
        return type == "color.exposure" || type == "color.contrast" ||
               type == "color.tint" || type == "color.wheels" ||
               type == "color.saturation";
    };
    std::vector<ClipEffect> placed;
    std::vector<ClipEffect> incoming = stages;
    bool inserted = false;
    Ulid wheelId;
    Ulid saturationId;
    for (const ClipEffect& effect : stack) {
        if (effect.type == "color.wheels" && wheelId.empty())
            wheelId = effect.id;
        if (effect.type == "color.saturation" && saturationId.empty())
            saturationId = effect.id;
    }
    for (ClipEffect& effect : incoming) {
        if (effect.type == "color.wheels" && !wheelId.empty())
            effect.id = wheelId;
        if (effect.type == "color.saturation" && !saturationId.empty())
            effect.id = saturationId;
    }
    for (const ClipEffect& effect : stack) {
        if (!primary(effect.type)) {
            placed.push_back(effect);
            continue;
        }
        if (replaced != nullptr) replaced->push_back(effect.type);
        if (!inserted) {
            placed.insert(placed.end(), incoming.begin(), incoming.end());
            inserted = true;
        }
    }
    if (!inserted)
        placed.insert(placed.end(), incoming.begin(), incoming.end());
    return placed;
}

bool DecimalFraction(double value, EffectParamValue& fraction) {
    if (!std::isfinite(value)) return false;
    const std::string text = Shortest(value);
    // "%g" spellings: an optional sign, digits, a point, digits, maybe an
    // exponent.
    char* end = nullptr;
    errno = 0;
    const double check = std::strtod(text.c_str(), &end);
    if (errno != 0 || check != value) return false;
    const size_t exponentAt = text.find_first_of("eE");
    const std::string mantissa = text.substr(0, exponentAt);
    int exponent = exponentAt == std::string::npos
                       ? 0
                       : std::atoi(text.c_str() + exponentAt + 1);
    const bool negative = !mantissa.empty() && mantissa[0] == '-';
    std::string digits;
    int decimals = 0;
    bool afterPoint = false;
    for (char character : mantissa) {
        if (character == '.') {
            afterPoint = true;
        } else if (character >= '0' && character <= '9') {
            digits += character;
            if (afterPoint) ++decimals;
        }
    }
    decimals -= exponent;
    int64_t num = 0;
    for (char digit : digits) {
        num = num * 10 + (digit - '0');
        if (num > std::numeric_limits<int32_t>::max() * int64_t{10})
            return false;
    }
    int64_t den = 1;
    while (decimals < 0) {
        num *= 10;
        ++decimals;
        if (num > std::numeric_limits<int32_t>::max()) return false;
    }
    if (decimals > 9) return false;
    for (int step = 0; step < decimals; ++step) den *= 10;
    if (negative) num = -num;
    const int64_t divisor = std::gcd(num, den);
    num /= divisor;
    den /= divisor;
    if (num > std::numeric_limits<int32_t>::max() ||
        num < -std::numeric_limits<int32_t>::max() ||
        den > std::numeric_limits<int32_t>::max())
        return false;
    fraction = {static_cast<int32_t>(num), static_cast<int32_t>(den)};
    return true;
}

std::string ShortestDecimal(double value) { return Shortest(value); }

bool ClipColorCdls(const Document& document, const std::vector<Ulid>& clipIds,
                   std::vector<ColorCdl>& cdls, ColorGradeSpace& space,
                   std::string& error) {
    cdls.clear();
    error.clear();
    space = ColorGradeSpace::Display;
    for (size_t index = 0; index < clipIds.size(); ++index) {
        const Ulid& clipId = clipIds[index];
        const DocumentClip* clip = document.FindClip(clipId);
        const DocumentTrack* track = document.FindTrackForClip(clipId);
        if (clip == nullptr) {
            error = "unknown clip_id '" + clipId + "'";
            return false;
        }
        if (track == nullptr || track->kind != "video" || IsMotionClip(*clip)) {
            error = "clip '" + clipId +
                    "' is not a picture a grade acts on: only a clip of a "
                    "video track, not a motion clip, carries a CDL";
            return false;
        }
        const ColorGradeContext context = ColorGradeContextFor(document, *clip);
        if (index == 0) space = context.space;
        ColorCdl cdl;
        if (!FoldColorCdl(ResolveColorGrade(clip->effects, context), cdl,
                          error)) {
            error = "clip '" + clipId + "': " + error;
            return false;
        }
        // Six decimals, as colourists' tools write a CDL: the fold runs on
        // the renderer's floats, and its last digits are theirs, not the
        // grade's (a contrast of 1/5 folds to 1.2000000029802322).
        const auto sixDecimals = [](double& value) {
            value = std::round(value * 1e6) / 1e6;
            if (value == 0.0) value = 0.0;  // No "-0".
        };
        for (size_t channel = 0; channel < 3; ++channel) {
            sixDecimals(cdl.slope[channel]);
            sixDecimals(cdl.offset[channel]);
            sixDecimals(cdl.power[channel]);
        }
        sixDecimals(cdl.saturation);
        cdl.id = clipId;
        if (const LibraryMedia* media =
                document.FindLibraryMedia(clip->source_id))
            cdl.description = media->filename;
        cdls.push_back(std::move(cdl));
    }
    return true;
}

bool PlaceClipCdl(const Document& document, const Ulid& clipId,
                  const ColorCdl& cdl, ColorGradeSpace declared,
                  ClipCdlPlacement& placement, std::string& error) {
    placement = ClipCdlPlacement{};
    error.clear();
    const DocumentClip* clip = document.FindClip(clipId);
    const DocumentTrack* track = document.FindTrackForClip(clipId);
    if (clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    if (track == nullptr || track->kind != "video" || IsMotionClip(*clip)) {
        error = "clip '" + clipId +
                "' is not a picture a grade acts on: only a clip of a video "
                "track, not a motion clip, takes a CDL";
        return false;
    }
    const ColorGradeSpace space = ColorGradeContextFor(document, *clip).space;
    if (space != declared) {
        error = std::string("the CDL was made on ") +
                ColorCdlSpaceName(declared) + " values, and clip '" + clipId +
                "' is graded on " + ColorCdlSpaceName(space) +
                " values: its numbers would mean another picture there";
        return false;
    }
    std::vector<ClipEffect> stages;
    if (!ColorCdlStages(cdl, stages, placement.deviation, error)) return false;
    placement.effects =
        PlaceColorCdl(clip->effects, stages, &placement.replaced);
    if (placement.effects.size() >
        static_cast<size_t>(kMaxColorEffectsPerClip)) {
        error = "clip '" + clipId + "' would hold more than " +
                std::to_string(kMaxColorEffectsPerClip) +
                " stages, the most the renderer draws";
        return false;
    }
    for (const ClipEffect& effect : placement.effects) {
        if (!ValidateColorEffect(effect, error)) {
            error = "clip '" + clipId + "': " + error;
            return false;
        }
    }
    return true;
}

bool ParseResolveCdlMap(const ResolveCdlMap& map, ColorCdl& cdl,
                        std::string& error) {
    cdl = ColorCdl{};
    error.clear();
    const auto numbers = [&](const std::string& name, const std::string& text,
                             double* values, size_t count) {
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        for (size_t index = 0; index < count; ++index) {
            if (!(stream >> values[index]) || !std::isfinite(values[index])) {
                error = "'" + name + "' takes " + std::to_string(count) +
                        (count == 1 ? " number" : " numbers") +
                        " separated by spaces, as Resolve writes it; got '" +
                        text + "'";
                return false;
            }
        }
        std::string rest;
        if (stream >> rest) {
            error = "'" + name + "' takes " + std::to_string(count) +
                    (count == 1 ? " number" : " numbers") + "; got '" + text +
                    "'";
            return false;
        }
        return true;
    };
    return numbers("slope", map.slope, cdl.slope.data(), 3) &&
           numbers("offset", map.offset, cdl.offset.data(), 3) &&
           numbers("power", map.power, cdl.power.data(), 3) &&
           numbers("saturation", map.saturation, &cdl.saturation, 1);
}

ResolveCdlMap ResolveSetCdlMap(const ColorCdl& cdl, int node) {
    const auto triple = [](const std::array<double, 3>& values) {
        return Shortest(values[0]) + " " + Shortest(values[1]) + " " +
               Shortest(values[2]);
    };
    return {std::to_string(node), triple(cdl.slope), triple(cdl.offset),
            triple(cdl.power), Shortest(cdl.saturation)};
}

mcp_json::Value ColorCdlValue(const ColorCdl& cdl) {
    using mcp_json::Value;
    const auto triple = [](const std::array<double, 3>& values) {
        Value list = Value::MakeArray();
        for (double value : values)
            list.Push(Value::MakeRawNumber(Shortest(value)));
        return list;
    };
    Value value = Value::MakeObject();
    value.Set("slope", triple(cdl.slope));
    value.Set("offset", triple(cdl.offset));
    value.Set("power", triple(cdl.power));
    value.Set("saturation", Value::MakeRawNumber(Shortest(cdl.saturation)));
    return value;
}

double ResolveYrgbSaturationGapCodes(double saturation) {
    // The Rec.709 weights of the ASC, the ones FoldColorCdl folds with.
    constexpr std::array<double, 3> kRec709 = {0.2126, 0.7152, 0.0722};
    double above = 0.0;
    double below = 0.0;
    for (size_t channel = 0; channel < 3; ++channel) {
        const double difference =
            kResolveYrgbSaturationWeights[channel] - kRec709[channel];
        (difference > 0.0 ? above : below) += difference;
    }
    // The largest |difference . y| over y in [0, 1]^3.
    const double luma = std::max(above, -below);
    const double codes = std::abs(1.0 - saturation) * luma * 1023.0;
    return std::round(codes * 10.0) / 10.0;
}

mcp_json::Value ExportedCdlsValue(const std::vector<ColorCdl>& cdls,
                                  ColorGradeSpace space, int resolveNode,
                                  const std::string& written) {
    using mcp_json::Value;
    Value reply = Value::MakeObject();
    reply.Set("space", Value::MakeString(ColorCdlSpaceId(space)));
    reply.Set("input_description", Value::MakeString(ColorCdlSpaceName(space)));
    Value clips = Value::MakeArray();
    Value warned = Value::MakeArray();
    double widest = 0.0;
    for (const ColorCdl& cdl : cdls) {
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(cdl.id));
        item.Set("media", Value::MakeString(cdl.description));
        item.Set("cdl", ColorCdlValue(cdl));
        // GR12, DEC30 -- the saturation Resolve weighs otherwise in YRGB.
        if (cdl.saturation != 1.0) {
            const double gap = ResolveYrgbSaturationGapCodes(cdl.saturation);
            item.Set("resolve_yrgb_gap_codes",
                     Value::MakeRawNumber(Shortest(gap)));
            warned.Push(Value::MakeString(cdl.id));
            widest = std::max(widest, gap);
        }
        const ResolveCdlMap map = ResolveSetCdlMap(cdl, resolveNode);
        Value resolve = Value::MakeObject();
        resolve.Set("NodeIndex", Value::MakeString(map.node_index));
        resolve.Set("Slope", Value::MakeString(map.slope));
        resolve.Set("Offset", Value::MakeString(map.offset));
        resolve.Set("Power", Value::MakeString(map.power));
        resolve.Set("Saturation", Value::MakeString(map.saturation));
        item.Set("resolve_set_cdl", std::move(resolve));
        clips.Push(std::move(item));
    }
    reply.Set("clips", std::move(clips));
    reply.Set("written",
              written.empty() ? Value::MakeNull() : Value::MakeString(written));
    if (!warned.AsArray().empty()) {
        reply.Set(
            "warning",
            Value::MakeString(
                "the clips in warning_clip_ids have a saturation other than "
                "1: in a DaVinci YRGB project, managed or not, Resolve weighs "
                "a CDL's saturation 0.21 R + 0.70 G + 0.09 B rather than the "
                "Rec.709 weights this CDL is made with (as it does under "
                "ACEScct), so there they render up to " +
                Shortest(widest) +
                " 10-bit codes apart (resolve_yrgb_gap_codes, per clip). No "
                "CDL saturation converts exactly; a saturation of 1 renders "
                "alike in both."));
        reply.Set("warning_clip_ids", std::move(warned));
    }
    return reply;
}

namespace {

const char* WriteFormatFor(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    if (!extension.empty()) extension.erase(0, 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    for (int index = 0; index < OCIO::GroupTransform::GetNumWriteFormats();
         ++index) {
        if (extension == OCIO::GroupTransform::GetFormatExtensionByIndex(index))
            return OCIO::GroupTransform::GetFormatNameByIndex(index);
    }
    return nullptr;
}

std::string ChildValue(const OCIO::FormatMetadata& metadata, const char* name) {
    for (int index = 0; index < metadata.getNumChildrenElements(); ++index) {
        const OCIO::FormatMetadata& child = metadata.getChildElement(index);
        if (std::string(child.getElementName()) == name)
            return child.getElementValue();
    }
    return "";
}

}  // namespace

bool ReadColorCdlFile(const std::string& path, std::vector<ColorCdl>& cdls,
                      std::string& inputDescription, std::string& error) {
    cdls.clear();
    inputDescription.clear();
    error.clear();
    try {
        const OCIO::GroupTransformRcPtr group =
            OCIO::CDLTransform::CreateGroupFromFile(path.c_str());
        inputDescription =
            ChildValue(group->getFormatMetadata(), "InputDescription");
        for (int index = 0; index < group->getNumTransforms(); ++index) {
            const auto transform =
                OCIO::DynamicPtrCast<const OCIO::CDLTransform>(
                    group->getTransform(index));
            if (!transform) continue;
            ColorCdl cdl;
            const char* id = transform->getFormatMetadata().getAttributeValue(
                OCIO::METADATA_ID);
            cdl.id = id != nullptr ? id : "";
            cdl.description = ChildValue(transform->getFormatMetadata(),
                                         OCIO::METADATA_DESCRIPTION);
            transform->getSlope(cdl.slope.data());
            transform->getOffset(cdl.offset.data());
            transform->getPower(cdl.power.data());
            cdl.saturation = transform->getSat();
            if (inputDescription.empty())
                inputDescription = ChildValue(transform->getFormatMetadata(),
                                              "InputDescription");
            cdls.push_back(std::move(cdl));
        }
    } catch (const std::exception& exception) {
        error = "unable to read the CDL '" + path + "': " + exception.what();
        return false;
    }
    if (cdls.empty()) {
        error = "the CDL '" + path + "' holds no ColorCorrection";
        return false;
    }
    return true;
}

bool WriteColorCdlFile(const std::string& path,
                       const std::vector<ColorCdl>& cdls,
                       const std::string& inputDescription,
                       std::string& error) {
    error.clear();
    const char* format = WriteFormatFor(path);
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (format == nullptr ||
        (extension != ".cc" && extension != ".ccc" && extension != ".cdl")) {
        error = "a CDL is written as .cc, .ccc or .cdl, not '" + path + "'";
        return false;
    }
    if (cdls.empty() || (extension == ".cc" && cdls.size() != 1)) {
        error = extension == ".cc"
                    ? "a .cc holds exactly one correction; write a .ccc"
                    : "no correction to write";
        return false;
    }
    try {
        const OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
        if (!inputDescription.empty() && extension != ".cc")
            group->getFormatMetadata().addChildElement(
                "InputDescription", inputDescription.c_str());
        for (const ColorCdl& cdl : cdls) {
            const OCIO::CDLTransformRcPtr transform =
                OCIO::CDLTransform::Create();
            transform->setStyle(OCIO::CDL_NO_CLAMP);
            if (!cdl.id.empty())
                transform->getFormatMetadata().addAttribute(OCIO::METADATA_ID,
                                                            cdl.id.c_str());
            if (!cdl.description.empty())
                transform->getFormatMetadata().addChildElement(
                    OCIO::METADATA_DESCRIPTION, cdl.description.c_str());
            if (!inputDescription.empty() && extension == ".cc")
                transform->getFormatMetadata().addChildElement(
                    "InputDescription", inputDescription.c_str());
            transform->setSlope(cdl.slope.data());
            transform->setOffset(cdl.offset.data());
            transform->setPower(cdl.power.data());
            transform->setSat(cdl.saturation);
            group->appendTransform(transform);
        }
        std::ostringstream text;
        text.imbue(std::locale::classic());
        group->write(OCIO::Config::CreateRaw(), format, text);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text.str();
        output.close();
        if (!output) {
            error = "unable to write '" + path + "'";
            return false;
        }
    } catch (const std::exception& exception) {
        error = "unable to write the CDL '" + path + "': " + exception.what();
        return false;
    }
    return true;
}
