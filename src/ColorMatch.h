#pragma once
// G2 -- docs/roadmap/GRADE-2026-09.md. Matching a shot to a reference shot,
// the way a colourist matches a scene to its hero shot: exposure, then
// balance, then saturation, and nothing else. Resolve's own shot match is
// described by colourists as a starting point that holds on primaries and
// not on curves; this stays inside that frame. The engine computes the
// correction, the caller decides whether it lands (match_color returns a
// set_clip_effects step, for prepare_edit_plan or for a direct edit).
//
// The match sets the clip's own four knobs -- its color.saturation,
// color.temperature, color.tint and color.exposure, the first stage of each
// type, which is the one the Inspector shows -- and adds, after the stack,
// a stage for a knob the clip does not have yet. It never leaves two stages
// of a type the Inspector would not both show. (G7 changed color.temperature
// into a chromatic adaptation on linear light, which no longer adds to an
// offset: the first version of this file folded a correction into the
// clip's stages by exact algebra, and had to append a second stage whenever
// the algebra did not hold.)
//
// A first guess is solved in closed form where the knobs act -- the ACEScct
// code values under colour management with ACEScct, the linear working
// values with another working space, the display signal without colour
// management -- on each clip's values after its own grade:
//
//   * saturation: the mean distance to luma once the midtones' mean cast is
//     taken away, which color.saturation's factor scales;
//   * balance, on the midtones (luma between the 25th and 75th
//     percentiles): the mean of R - B and of G - (R + B) / 2, the cast,
//     aimed at after the factor has scaled the clip's own;
//   * exposure: the median luma, which a uniform offset moves by itself.
//
// Luma here is the one color.saturation uses (BT.709 weights). That guess is
// then settled on the display by RefineColorMatch, against measure_color's
// own numbers.
//
// Portable: no AppKit, no Metal, no FFmpeg (tests/color_match_tests.cc).

#include "ColorEffects.h"
#include "Document.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

struct ColorMatchAspects {
    bool exposure = true;
    bool balance = true;
    bool saturation = true;
};

// What the match reads of one set of grading-space values.
struct ColorMatchReading {
    int64_t pixels = 0;
    double median_luma = 0.0;
    double red_minus_blue = 0.0;  // Midtone mean.
    double green_magenta = 0.0;   // Midtone mean of G - (R + B) / 2.
    double spread = 0.0;          // See Spread in the .cc.
};

ColorMatchReading ReadColorMatch(
    const std::vector<std::array<float, 3>>& values);

// The closed-form guess, as moves of the grading space: a saturation factor,
// then a cast -- `offset` up on red and down on blue, `green` down on green
// -- then a uniform `exposure` offset.
struct ColorMatchGuess {
    double saturation_factor = 1.0;
    double offset = 0.0;
    double green = 0.0;
    double exposure = 0.0;
    // Neither picture has colour enough for a factor to mean anything.
    bool saturation_undefined = false;
};

// Solves the guess that takes `clip` to `reference`, both the grading-space
// values of the pixels to compare (a region, or the whole picture), each
// after its own clip's grade.
bool SolveColorMatch(const std::vector<std::array<float, 3>>& clip,
                     const std::vector<std::array<float, 3>>& reference,
                     const ColorMatchAspects& aspects, ColorMatchGuess& guess,
                     std::string& error);

// The clip's four knobs, as the stages hold them: amounts for exposure, tint
// and saturation, Kelvin for the temperature.
struct ColorMatchKnobs {
    double exposure = 0.0;
    double kelvin = static_cast<double>(kNeutralKelvin);
    double tint = 0.0;
    double saturation = 0.0;
};

// What the clip's stack holds now: the first stage of each type, and the
// neutral (0, or the as-shot white balance) for a type it has not.
ColorMatchKnobs ReadColorMatchKnobs(const std::vector<ClipEffect>& stack,
                                    const ColorGradeContext& context);

// The knobs that apply `guess` on top of `knobs`: amounts added, factors
// multiplied, and the Kelvin whose adaptation moves a mid grey's R - B by
// the cast the guess asks for on top of the clip's own, found by bisection
// over [kMinimumKelvin, kMaximumKelvin] in `context`.
ColorMatchKnobs ApplyColorMatchGuess(const ColorMatchKnobs& knobs,
                                     const ColorMatchGuess& guess,
                                     const ColorMatchAspects& aspects,
                                     const ColorGradeContext& context);

// Where each knob went in the clip's stack.
struct ColorMatchPlacement {
    std::string type;
    // The clip's own stage of that type was set; or one was added after the
    // stack, the clip having none.
    bool adjusted = false;
};

// The clip's stack with `knobs` in it, as exact fractions a step carries:
// thousandths for the three amounts (the Inspector's step), whole Kelvin.
// A knob the clip has no stage for, and that stays neutral, adds nothing.
std::vector<ClipEffect> PlaceColorMatch(
    const std::vector<ClipEffect>& stack, const ColorMatchKnobs& knobs,
    const ColorGradeContext& context,
    std::vector<ColorMatchPlacement>* placements = nullptr);

// What the refinement below reads of a rendered picture: measure_color's
// numbers, in its hundredths (ColorMeasure.h) -- the median display luma,
// the display midtones' warmth and green-magenta, the mean saturation.
struct ColorMatchDisplay {
    double luma_p50 = 0.0;
    bool has_midtones = false;
    double warmth = 0.0;
    double green_magenta = 0.0;
    double saturation = 0.0;
};

// Renders the clip with candidate knobs and reads it.
using ColorMatchEvaluator = std::function<bool(
    const ColorMatchKnobs&, ColorMatchDisplay&, std::string&)>;

// The closed form is exact on a flat picture. On a real one the midtones
// and spread it reads in the grading space are not the display numbers
// measure_color reports, and on two shots of different content they can
// disagree -- measured on real shots, a saturation solved up in the grading
// space came out down on the display. A colourist matches what the output
// scopes show, so the knobs are then refined against those numbers: one at
// a time, in cycles, each by secant steps on the number it moves most (the
// temperature on the logarithm of its Kelvin), within sane bounds, each
// step rendering the whole picture. A joint Newton step was tried and
// diverged: on the display a warm cast is also chroma, which couples the
// temperature to the saturation. The best state seen is kept. Two parts of
// one scene (the same light, two cameras or two moments) land on the
// reference to a few hundredths; two unrelated contents have no right
// answer, and the reply's residual says how far the best effort stayed.
bool RefineColorMatch(const ColorMatchDisplay& target,
                      const ColorMatchAspects& aspects, bool saturationDefined,
                      const ColorMatchEvaluator& evaluate,
                      ColorMatchKnobs& knobs, std::string& error);
