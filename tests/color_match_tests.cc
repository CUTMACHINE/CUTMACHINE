// G2 -- docs/roadmap/GRADE-2026-09.md. The match itself: the closed-form
// guess on grading-space values whose difference is known by hand, the
// knobs it sets in the clip's own stages, and the refinement on a model of
// a display whose answer is known.
#include "ColorEffects.h"
#include "ColorMatch.h"
#include "ColorMeasure.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using Values = std::vector<std::array<float, 3>>;

// A deterministic spread of colours around mid grey: a ramp of lumas, each
// with its own small chroma, so percentiles, midtones and spread are all
// well defined.
Values Scene() {
    Values values;
    for (int index = 0; index < 400; ++index) {
        const float t = static_cast<float>(index) / 399.0f;
        const float base = 0.15f + 0.6f * t;
        const float swing = 0.04f * std::sin(static_cast<float>(index) * 0.7f);
        values.push_back({base + swing, base - 0.5f * swing, base - swing});
    }
    return values;
}

Values Offset(const Values& values, float red, float green, float blue) {
    Values out = values;
    for (auto& value : out) {
        value[0] += red;
        value[1] += green;
        value[2] += blue;
    }
    return out;
}

Values ScaleChroma(const Values& values, float factor) {
    Values out = values;
    for (auto& value : out) {
        const float luma =
            0.2126f * value[0] + 0.7152f * value[1] + 0.0722f * value[2];
        for (float& channel : value) channel = luma + (channel - luma) * factor;
    }
    return out;
}

// The guess applied as its moves: the factor, the cast, the exposure.
Values Guessed(const Values& values, const ColorMatchGuess& guess) {
    const Values scaled =
        ScaleChroma(values, static_cast<float>(guess.saturation_factor));
    return Offset(scaled, static_cast<float>(guess.offset + guess.exposure),
                  static_cast<float>(-guess.green + guess.exposure),
                  static_cast<float>(-guess.offset + guess.exposure));
}

bool Near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

ClipEffect Stage(const std::string& type, const std::string& knob,
                 EffectParamValue value) {
    ClipEffect effect;
    effect.type = type;
    effect.params[knob] = value;
    return effect;
}

}  // namespace

int main() {
    const Values reference = Scene();
    const ColorMatchReading wanted = ReadColorMatch(reference);
    ColorMatchGuess guess;
    std::string error;

    // ---- the guess ------------------------------------------------------
    Check(SolveColorMatch(reference, reference, {}, guess, error) &&
              Near(guess.saturation_factor, 1.0, 1e-9) &&
              Near(guess.offset, 0.0, 1e-9) && Near(guess.green, 0.0, 1e-9) &&
              Near(guess.exposure, 0.0, 1e-9),
          "a picture matches itself with nothing: " + error);
    Check(SolveColorMatch(Offset(reference, -0.1f, -0.1f, -0.1f), reference, {},
                          guess, error) &&
              Near(guess.exposure, 0.1, 1e-6) && Near(guess.offset, 0.0, 1e-6),
          "a darker copy is an exposure of +0.1");
    Check(SolveColorMatch(Offset(reference, 0.02f, 0.0f, -0.02f), reference, {},
                          guess, error) &&
              Near(guess.offset, -0.02, 1e-6) && Near(guess.green, 0.0, 1e-6),
          "a warm copy is a cast of -0.02 on red and +0.02 on blue");
    Check(SolveColorMatch(Offset(reference, 0.0f, 0.03f, 0.0f), reference, {},
                          guess, error) &&
              Near(guess.green, 0.03, 1e-6),
          "a green copy is 0.03 off green");
    Check(SolveColorMatch(ScaleChroma(reference, 0.5f), reference, {}, guess,
                          error) &&
              Near(guess.saturation_factor, 2.0, 1e-4),
          "half the spread is a factor of two: " +
              std::to_string(guess.saturation_factor));
    const Values off =
        Offset(ScaleChroma(reference, 0.7f), 0.03f, -0.05f, -0.04f);
    Check(SolveColorMatch(off, reference, {}, guess, error),
          "a combined difference solves: " + error);
    const ColorMatchReading after = ReadColorMatch(Guessed(off, guess));
    Check(Near(after.median_luma, wanted.median_luma, 1e-4) &&
              Near(after.red_minus_blue, wanted.red_minus_blue, 1e-4) &&
              Near(after.green_magenta, wanted.green_magenta, 1e-4) &&
              Near(after.spread, wanted.spread, 1e-4),
          "applied, the guess lands on the reference in every number");

    ColorMatchAspects exposureOnly;
    exposureOnly.balance = false;
    exposureOnly.saturation = false;
    Check(SolveColorMatch(off, reference, exposureOnly, guess, error) &&
              guess.saturation_factor == 1.0 && guess.offset == 0.0 &&
              guess.green == 0.0 && guess.exposure != 0.0,
          "exposure alone touches nothing else");
    ColorMatchAspects none;
    none.exposure = none.balance = none.saturation = false;
    Check(!SolveColorMatch(off, reference, none, guess, error),
          "matching nothing is refused");
    Check(SolveColorMatch(Values(100, {0.4f, 0.4f, 0.4f}), reference, {}, guess,
                          error) &&
              guess.saturation_undefined && guess.saturation_factor == 1.0,
          "a grey clip has no colour to scale, and says so");
    Check(!SolveColorMatch({}, reference, {}, guess, error),
          "an empty picture is refused");

    // ---- the knobs ------------------------------------------------------
    const ColorGradeContext shot{ColorGradeSpace::AcesCct, 4500};
    const std::vector<ClipEffect> graded = {
        Stage("color.exposure", "amount", {-7, 10}),
        Stage("color.contrast", "amount", {1, 10}),
        Stage("color.temperature", "kelvin", {5200, 1})};
    const ColorMatchKnobs knobs = ReadColorMatchKnobs(graded, shot);
    Check(Near(knobs.exposure, -0.7, 1e-9) && Near(knobs.kelvin, 5200, 1e-9) &&
              knobs.tint == 0.0 && knobs.saturation == 0.0,
          "the knobs are the clip's first stage of each type");
    Check(Near(ReadColorMatchKnobs({}, shot).kelvin, 4500, 1e-9) &&
              Near(ReadColorMatchKnobs({}, {}).kelvin, 6500, 1e-9),
          "a clip without a temperature sits at its as-shot balance, or "
          "6500 K");

    ColorMatchGuess moves;
    moves.saturation_factor = 1.5;
    moves.offset = -0.01;
    moves.green = 0.02;
    moves.exposure = 0.05;
    const ColorMatchKnobs set = ApplyColorMatchGuess(knobs, moves, {}, shot);
    Check(Near(set.exposure, -0.7 + 0.05 * 17.52, 1e-9) &&
              Near(set.tint, 0.02 * 17.52 / 0.5, 1e-9) &&
              Near(set.saturation, 0.5, 1e-9),
          "amounts add, the factor multiplies");
    // The Kelvin found moves a mid grey's R - B by the guess's cast on top
    // of what 5200 K moves it by.
    const auto cast = [&](double kelvin) {
        const std::array<float, 3> grey = {0.4135884f, 0.4135884f, 0.4135884f};
        const auto moved = ApplyResolvedColorGrade(
            grey, ResolveColorGrade(
                      {Stage("color.temperature", "kelvin",
                             {static_cast<int32_t>(std::lround(kelvin)), 1})},
                      shot));
        return static_cast<double>(moved[0] - moved[2]);
    };
    Check(set.kelvin < 5200 && Near(cast(set.kelvin), cast(5200) - 0.02, 2e-4),
          "the Kelvin carries the cast: " + std::to_string(set.kelvin));

    std::vector<ColorMatchPlacement> placements;
    ColorMatchKnobs target = set;
    target.saturation = 0.25;
    const std::vector<ClipEffect> placed =
        PlaceColorMatch(graded, target, shot, &placements);
    Check(placed.size() == 5 && placed[1].type == "color.contrast" &&
              placed[1].params.at("amount").num == 1,
          "the other stages keep their place and values");
    const EffectParamValue exposure = placed[0].params.at("amount");
    Check(std::abs(static_cast<double>(exposure.num) / exposure.den -
                   set.exposure) < 0.0006 &&
              1000 % exposure.den == 0,
          "the clip's exposure is set, in thousandths");
    int adjusted = 0;
    int added = 0;
    for (const auto& placement : placements)
        (placement.adjusted ? adjusted : added) += 1;
    Check(adjusted == 2 && added == 2 && placed[3].type == "color.saturation" &&
              placed[4].type == "color.tint",
          "exposure and temperature adjusted; saturation, then tint, added");
    int exposures = 0;
    for (const ClipEffect& effect : placed)
        exposures += effect.type == "color.exposure" ? 1 : 0;
    Check(exposures == 1, "never a second stage of a type");
    Check(PlaceColorMatch({}, ReadColorMatchKnobs({}, shot), shot).empty(),
          "neutral knobs add nothing to a clip without stages");

    // ---- the refinement, on a display whose answer is known -------------
    // Each number moved by its knob and a little by the others, as a real
    // display does.
    const ColorMatchEvaluator model = [](const ColorMatchKnobs& value,
                                         ColorMatchDisplay& out, std::string&) {
        const double warm = 2000.0 * std::log(value.kelvin / 6500.0);
        out.luma_p50 = 5000.0 + 1000.0 * value.exposure + 0.05 * warm;
        out.has_midtones = true;
        out.warmth = warm * (1.0 + value.saturation);
        out.green_magenta = -300.0 * value.tint;
        out.saturation =
            1200.0 * (1.0 + value.saturation) + 0.2 * std::abs(warm);
        return true;
    };
    ColorMatchDisplay aim;
    aim.luma_p50 = 5600.0;
    aim.has_midtones = true;
    aim.warmth = 300.0;
    aim.green_magenta = -45.0;
    aim.saturation = 1500.0;
    ColorMatchKnobs tuned;
    Check(RefineColorMatch(aim, {}, true, model, tuned, error),
          "the refinement runs: " + error);
    ColorMatchDisplay landed;
    model(tuned, landed, error);
    Check(Near(landed.luma_p50, aim.luma_p50, 5) &&
              Near(landed.warmth, aim.warmth, 5) &&
              Near(landed.green_magenta, aim.green_magenta, 5) &&
              Near(landed.saturation, aim.saturation, 5),
          "coupled numbers land on the reference, one knob at a time: " +
              std::to_string(landed.luma_p50) + " " +
              std::to_string(landed.warmth) + " " +
              std::to_string(landed.green_magenta) + " " +
              std::to_string(landed.saturation));
    ColorMatchKnobs bounded;
    aim.warmth = 1e7;  // No Kelvin reaches it.
    Check(RefineColorMatch(aim, {}, true, model, bounded, error) &&
              bounded.kelvin <= kMaximumKelvin + 0.5,
          "an unreachable number stops the knob at its bound");

    // AGENT-2026-09 A16 -- the exposure proposed from measurements: the
    // clip's own exposure moved by what its scene median sits off the
    // target, kept within the tolerance, absent without a scene measure.
    {
        Document document;
        document.sources = {
            {"01K50000000000000000000001", "A.mov", {25, 1}, {1000, 25}}};
        DocumentClip graded;
        graded.id = "01K50000000000000000000011";
        graded.source_id = "01K50000000000000000000001";
        graded.source_in = {0, 25};
        graded.duration = {50, 25};
        graded.timeline_in = {0, 25};
        graded.effects = {Stage("color.exposure", "amount", {3, 10})};
        DocumentClip plain = graded;
        plain.id = "01K50000000000000000000012";
        plain.effects.clear();
        plain.timeline_in = {50, 25};
        DocumentClip dark = plain;
        dark.id = "01K50000000000000000000013";
        dark.timeline_in = {100, 25};
        document.sequence.tracks = {
            {"01K50000000000000000000002", "video", 0, {graded, plain, dark}}};
        const auto measured = [](const Ulid& clipId, bool scene, int64_t p50) {
            ColorClipMeasurement measurement;
            measurement.clip_id = clipId;
            measurement.source_id = "01K50000000000000000000001";
            measurement.picture.scene = scene;
            if (scene) measurement.picture.scene_p50 = p50;
            return measurement;
        };
        const std::vector<ColorClipMeasurement> measurements = {
            measured(graded.id, true, 197), measured(plain.id, true, 30),
            measured(dark.id, false, 0)};
        std::vector<ExposureProposal> proposals;
        Check(ProposeExposure(document, measurements, {}, proposals, error),
              "exposure proposals resolve: " + error);
        Check(proposals.size() == 3 && proposals[0].reason == "proposed" &&
                  proposals[0].correction_hundredths == -197 &&
                  Near(proposals[0].exposure_before, 0.3, 1e-9) &&
                  Near(proposals[0].exposure_after, -1.67, 1e-9),
              "a clip two stops over middle grey is brought back on its "
              "own exposure stage");
        const auto amount = [&]() {
            if (proposals.empty() || proposals[0].effects.empty() ||
                !proposals[0].effects[0].params.count("amount"))
                return std::string("(none)");
            const EffectParamValue& value =
                proposals[0].effects[0].params.at("amount");
            return std::to_string(value.num) + "/" + std::to_string(value.den);
        };
        Check(proposals[0].effects.size() == 1 &&
                  proposals[0].effects[0].type == "color.exposure" &&
                  Near(static_cast<double>(
                           proposals[0].effects[0].params.at("amount").num) /
                           proposals[0].effects[0].params.at("amount").den,
                       -1.67, 1e-9) &&
                  proposals[0].placements.size() == 1 &&
                  proposals[0].placements[0].adjusted,
              "the step carries the clip's stage adjusted, as an exact "
              "fraction: " +
                  amount());
        Check(proposals[1].reason == "within_tolerance" &&
                  proposals[1].effects.empty() &&
                  proposals[1].correction_hundredths == 0,
              "a clip within half a stop is left alone");
        Check(proposals[2].reason == "no_scene_measure" &&
                  !proposals[2].scene_p50_hundredths,
              "a clip without a scene measure gets no proposal and says why");
        ExposureProposalSettings aimed;
        aimed.target_hundredths = -50;
        aimed.tolerance_hundredths = 100;
        Check(
            ProposeExposure(document, measurements, aimed, proposals, error) &&
                proposals[0].correction_hundredths == -247 &&
                proposals[1].reason == "within_tolerance",
            "the target and the tolerance are the caller's");
        ColorMeasurePlan plan;
        plan.clip_ids = {graded.id, plain.id, dark.id};
        mcp_json::Value reply =
            ExposureProposalsValue(document, plan, ExposureProposalSettings{},
                                   measurements, proposals);
        Check(reply.Find("steps") &&
                  reply.Find("steps")->AsArray().size() == 1 &&
                  reply.Find("proposed")->Dump() == "1" &&
                  reply.Find("skipped")->Dump() == "2" &&
                  reply.Find("clips")->AsArray().size() == 3 &&
                  reply.Find("steps")->AsArray()[0].Find("tool")->AsString() ==
                      "set_clip_effects",
              "the reply carries one set_clip_effects step per proposal: " +
                  reply.Dump());
        // AGENT-2026-09 A19 -- where the montage sits as a whole.
        const mcp_json::Value* summary = reply.Find("summary");
        Check(summary != nullptr &&
                  summary->Find("clips_measured")->Dump() == "2" &&
                  summary->Find("median_stops")->Dump() == "1.97" &&
                  summary->Find("min_stops")->Dump() == "0.3" &&
                  summary->Find("max_stops")->Dump() == "1.97",
              "the reply summarises the measured medians: " + reply.Dump());
        ExposureProposalSettings negative;
        negative.tolerance_hundredths = -1;
        Check(!ProposeExposure(document, measurements, negative, proposals,
                               error),
              "a negative tolerance is refused");
        Check(!ProposeExposure(
                  document, {measured("01K50000000000000000000099", true, 0)},
                  {}, proposals, error),
              "a measurement of an unknown clip is refused");
    }

    // AGENT-2026-09 A20 -- the balance target of a face: the midtones'
    // chroma kept, turned onto the skin line, as the warmth and
    // green-magenta measure_color would then read.
    {
        const auto zone = [](double red, double green, double blue) {
            ColorZoneMeasure midtones;
            midtones.pixels = 100;
            midtones.red = static_cast<int64_t>(std::llround(red * 10000.0));
            midtones.green =
                static_cast<int64_t>(std::llround(green * 10000.0));
            midtones.blue = static_cast<int64_t>(std::llround(blue * 10000.0));
            midtones.warmth = midtones.red - midtones.blue;
            midtones.green_magenta =
                std::llround(static_cast<double>(2 * midtones.green -
                                                 midtones.red - midtones.blue) /
                             2.0);
            return midtones;
        };
        // What a Rec. 709 display reads of a colour: its luma, Cb, Cr.
        const auto hue = [](double red, double green, double blue) {
            const double luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            const double cb = (blue - luma) / (2.0 * (1.0 - 0.0722));
            const double cr = (red - luma) / (2.0 * (1.0 - 0.2126));
            double degrees = std::atan2(cr, cb) * 180.0 / M_PI;
            if (degrees < 0.0) degrees += 360.0;
            return degrees;
        };
        int64_t warmth = 0;
        int64_t greenMagenta = 0;
        // A face gone green under fluorescent light: its hue sits far from
        // the skin line.
        const double before = hue(0.55, 0.52, 0.38);
        Check(std::abs(before - 123.0) > 20.0,
              "the test's cast sits off the skin line: " +
                  std::to_string(before));
        Check(SkinLineBalanceTarget(zone(0.55, 0.52, 0.38), false, warmth,
                                    greenMagenta),
              "a face's midtones yield a balance target");
        // The target, read back: the same luma and chroma magnitude, the hue
        // on the skin line.
        const double luma = 0.2126 * 0.55 + 0.7152 * 0.52 + 0.0722 * 0.38;
        const double targetWarmth = static_cast<double>(warmth) / 10000.0;
        const double targetGreen = static_cast<double>(greenMagenta) / 10000.0;
        // red - blue = warmth, green - (red + blue) / 2 = gm, luma fixed:
        // three equations, three channels.
        // red = blue + warmth; green = blue + warmth / 2 + gm.
        const double blue = (luma - 0.2126 * targetWarmth -
                             0.7152 * (targetWarmth / 2.0 + targetGreen)) /
                            (0.2126 + 0.7152 + 0.0722);
        const double red = blue + targetWarmth;
        const double green = blue + targetWarmth / 2.0 + targetGreen;
        const double after = hue(red, green, blue);
        Check(
            std::abs(after - 123.0) < 0.5,
            "the target's hue lies on the skin line: " + std::to_string(after));
        const auto chroma = [](double r, double g, double b) {
            const double l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            return std::hypot((b - l) / (2.0 * (1.0 - 0.0722)),
                              (r - l) / (2.0 * (1.0 - 0.2126)));
        };
        Check(std::abs(chroma(red, green, blue) - chroma(0.55, 0.52, 0.38)) <
                  1e-3,
              "the target keeps the face's chroma");
        Check(warmth > 0 && greenMagenta < 0,
              "a skin target is warm and on the magenta side of green");
        Check(!SkinLineBalanceTarget(zone(0.5, 0.5, 0.5), false, warmth,
                                     greenMagenta),
              "a grey zone has no hue to turn");
        Check(!SkinLineBalanceTarget(ColorZoneMeasure{}, false, warmth,
                                     greenMagenta),
              "an empty zone has no target");
        int64_t warmth2020 = 0;
        int64_t green2020 = 0;
        Check(SkinLineBalanceTarget(zone(0.55, 0.52, 0.38), true, warmth2020,
                                    green2020) &&
                  warmth2020 != warmth,
              "the display's own weights decide the target");
    }

    // AGENT-2026-09 A20 -- the lift the highlights allow: whole when the
    // frame's p98 stays under the ceiling, cut to the room left otherwise,
    // never below zero; a lowering and an unmeasured frame pass as they are.
    {
        bool limited = true;
        Check(LimitGradeExposure(76, 151, 250, limited) == 76 && !limited,
              "a lift that keeps the whites under the ceiling passes whole");
        Check(LimitGradeExposure(180, 112, 250, limited) == 138 && limited,
              "a lift past the ceiling is cut to the room the highlights "
              "leave");
        Check(LimitGradeExposure(120, 270, 250, limited) == 0 && limited,
              "a frame already over the ceiling gets no lift at all");
        Check(LimitGradeExposure(-150, 290, 250, limited) == -150 && !limited,
              "a lowering is never limited");
        Check(LimitGradeExposure(180, std::nullopt, 250, limited) == 180 &&
                  !limited,
              "an unmeasured frame limits nothing");
    }

    // GR15 -- the ceiling of an HLG output: a lift stands while the frame
    // clips no more than it did, otherwise the largest that does not, in a
    // bounded number of renders; a lowering renders nothing.
    {
        int renders = 0;
        // The frame starts clipping once lifted past 0.6 stop, more with
        // every hundredth after.
        const DisplayClipEvaluator clipsPast60 =
            [&renders](int64_t correction, int64_t& clipped, std::string&) {
                ++renders;
                clipped = correction > 60 ? 20 + (correction - 60) : 20;
                return true;
            };
        int64_t allowed = 0;
        bool limited = true;
        std::string error;
        Check(LimitGradeExposureOnDisplay(-80, 20, clipsPast60, allowed,
                                          limited, error) &&
                  allowed == -80 && !limited && renders == 0,
              "a lowering is never limited and renders nothing");
        Check(LimitGradeExposureOnDisplay(50, 20, clipsPast60, allowed, limited,
                                          error) &&
                  allowed == 50 && !limited && renders == 1,
              "a lift that clips no more passes whole in one render");
        renders = 0;
        Check(LimitGradeExposureOnDisplay(150, 20, clipsPast60, allowed,
                                          limited, error) &&
                  limited && allowed <= 60 + kDisplayClipAllowance &&
                  allowed >=
                      60 + kDisplayClipAllowance - 2 * kDisplayCeilingStep &&
                  renders <= kDisplayCeilingRenders,
              "a lift that clips is cut to the largest that does not, in "
              "at most " +
                  std::to_string(kDisplayCeilingRenders) + " renders: " +
                  std::to_string(allowed) + " in " + std::to_string(renders));
        // A frame that clipped already: a few more specks are allowed.
        Check(LimitGradeExposureOnDisplay(65, 20, clipsPast60, allowed, limited,
                                          error) &&
                  allowed == 65 && !limited,
              "a speck more than the frame clipped is allowed");
        const DisplayClipEvaluator broken = [](int64_t, int64_t&,
                                               std::string& failure) {
            failure = "no picture";
            return false;
        };
        Check(!LimitGradeExposureOnDisplay(40, 0, broken, allowed, limited,
                                           error) &&
                  error == "no picture",
              "a render that fails refuses the lift");
    }

    // GR15 -- a rush whose faces all hit the ceiling: its vote is 0, and
    // says it was held back (on 6 November 2026 its clips came back
    // "within_tolerance", as if each face sat on its target). A rush whose
    // clips got what they asked is not.
    {
        const auto held = [](const Ulid& source, int64_t correction,
                             int64_t wanted) {
            GradeProposal proposal;
            proposal.source_id = source;
            proposal.anchor_used = "face";
            proposal.correction_hundredths = correction;
            proposal.wanted_hundredths = wanted;
            return proposal;
        };
        const Ulid rushA = "01K50000000000000000000041";
        const Ulid rushB = "01K50000000000000000000042";
        const std::map<Ulid, SourceGradeVote> votes = SourceGradeVotes(
            {held(rushA, 0, 80), held(rushA, 0, 75), held(rushA, 0, 95),
             held(rushB, 40, 40), held(rushB, -30, -30)});
        Check(votes.at(rushA).correction_hundredths == 0 &&
                  votes.at(rushA).wanted_hundredths == 80 &&
                  votes.at(rushA).limited,
              "a rush held back by its highlights votes 0 and says so");
        Check(votes.at(rushB).correction_hundredths ==
                      votes.at(rushB).wanted_hundredths &&
                  !votes.at(rushB).limited,
              "a rush whose clips got what they asked is not held back");
    }

    // AGENT-2026-09 A31 -- the vote of a rush: the median correction,
    // kelvin and tint of its clips read on a face or a patch; a rush with
    // none has no vote.
    {
        const auto anchored = [](const Ulid& source, int64_t correction,
                                 double kelvin, double tint) {
            GradeProposal proposal;
            proposal.source_id = source;
            proposal.anchor_used = "face";
            proposal.correction_hundredths = correction;
            proposal.knobs_after.kelvin = kelvin;
            proposal.knobs_after.tint = tint;
            return proposal;
        };
        const Ulid rushA = "01K50000000000000000000031";
        const Ulid rushB = "01K50000000000000000000032";
        GradeProposal faceless;
        faceless.source_id = rushB;
        faceless.anchor_used = "none";
        faceless.correction_hundredths = 300;
        const std::map<Ulid, SourceGradeVote> votes =
            SourceGradeVotes({anchored(rushA, 40, 6300.0, 0.2),
                              anchored(rushA, 100, 6400.0, 0.3),
                              anchored(rushA, -20, 6500.0, 0.1), faceless});
        Check(votes.size() == 1 && votes.count(rushA) == 1 &&
                  votes.at(rushA).count == 3 &&
                  votes.at(rushA).correction_hundredths == 40 &&
                  Near(votes.at(rushA).kelvin, 6400.0, 1e-9) &&
                  Near(votes.at(rushA).tint, 0.2, 1e-9),
              "a rush votes by the median of its anchored clips, and a rush "
              "with none has no vote");
    }

    // AGENT-2026-09 A20 -- the reply of propose_grade, from proposals made
    // by hand: one step per clip changed, the anchors and readings named.
    {
        Document document;
        document.color_management.enabled = true;
        DocumentClip faced;
        faced.id = "01K50000000000000000000021";
        faced.source_id = "01K50000000000000000000001";
        faced.source_in = {0, 25};
        faced.duration = {50, 25};
        faced.timeline_in = {0, 25};
        DocumentClip faceless = faced;
        faceless.id = "01K50000000000000000000022";
        faceless.timeline_in = {50, 25};
        document.sequence.tracks = {
            {"01K50000000000000000000002", "video", 0, {faced, faceless}}};
        GradeProposalPlan plan;
        plan.clip_ids = {faced.id, faceless.id};
        // AGENT-2026-09 A41 -- the full reading, which this test checks.
        plan.verbose = true;
        GradeProposal first;
        first.clip_id = faced.id;
        first.source_id = faced.source_id;
        first.anchor_used = "face";
        first.anchor_region.present = true;
        first.anchor_region.x = {1, 4};
        first.anchor_region.y = {1, 4};
        first.anchor_region.width = {1, 2};
        first.anchor_region.height = {1, 2};
        first.knobs_after.exposure = 0.45;
        first.knobs_after.kelvin = 5200;
        first.correction_hundredths = 45;
        first.balanced = true;
        first.effects = {Stage("color.exposure", "amount", {45, 100})};
        first.placements = {{"color.exposure", false},
                            {"color.temperature", false}};
        first.reason = "proposed";
        first.before.region.scene = true;
        first.before.region.scene_p50 = 25;
        first.before.region.skin_line_offset = -1800;
        first.before.region.zones[1].pixels = 10;
        first.before.region.zones[1].warmth = 300;
        first.before.picture.scene = true;
        first.before.picture.scene_p50 = -40;
        first.before.picture.scene_p98 = 151;
        first.after = first.before;
        first.after.region.scene_p50 = 70;
        first.after.region.skin_line_offset = 100;
        first.after.picture.scene_p50 = 5;
        first.after.picture.scene_p98 = 196;
        GradeProposal second;
        second.clip_id = faceless.id;
        second.source_id = faceless.source_id;
        second.anchor_used = "median";
        second.reason = "no_face_matched_montage";
        second.before.picture.scene = true;
        second.before.picture.scene_p50 = -90;
        second.after = second.before;
        second.after.picture.scene_p50 = 5;
        second.correction_hundredths = 95;
        second.knobs_after.exposure = 0.95;
        second.effects = {Stage("color.exposure", "amount", {95, 100})};
        second.placements = {{"color.exposure", false}};
        mcp_json::Value reply =
            GradeProposalsValue(document, plan, {first, second});
        const mcp_json::Value* clips = reply.Find("clips");
        Check(reply.Find("anchor")->AsString() == "face" &&
                  reply.Find("target_stops")->Dump() == "0.7" &&
                  reply.Find("highlight_ceiling_stops")->Dump() == "2.5" &&
                  reply.Find("proposed")->Dump() == "2" &&
                  reply.Find("steps")->AsArray().size() == 2 &&
                  clips != nullptr && clips->AsArray().size() == 2,
              "the reply carries one step per clip changed: " + reply.Dump());
        const mcp_json::Value& facedView = clips->AsArray()[0];
        Check(facedView.Find("anchor_used")->AsString() == "face" &&
                  facedView.Find("anchor_region") != nullptr &&
                  facedView.Find("balanced")->Dump() == "true" &&
                  facedView.Find("correction_stops")->Dump() == "0.45" &&
                  facedView.Find("before")->Find("anchor_stops")->Dump() ==
                      "0.25" &&
                  facedView.Find("after")->Find("anchor_stops")->Dump() ==
                      "0.7" &&
                  facedView.Find("before")
                          ->Find("skin_line_offset_degrees")
                          ->Dump() == "-18" &&
                  facedView.Find("after")->Find("frame_stops")->Dump() ==
                      "0.05" &&
                  facedView.Find("after")->Find("frame_p98_stops")->Dump() ==
                      "1.96" &&
                  facedView.Find("knobs")
                          ->Find("temperature_kelvin")
                          ->Find("after")
                          ->Dump() == "5200" &&
                  facedView.Find("placements")->AsArray().size() == 2,
              "a faced clip reports its anchor, before and after: " +
                  facedView.Dump());
        const mcp_json::Value& facelessView = clips->AsArray()[1];
        Check(facelessView.Find("anchor_used")->AsString() == "median" &&
                  facelessView.Find("reason")->AsString() ==
                      "no_face_matched_montage" &&
                  facelessView.Find("anchor_region") == nullptr &&
                  facelessView.Find("before")->Find("anchor_stops")->Dump() ==
                      "-0.9" &&
                  facelessView.Find("balanced")->Dump() == "false",
              "a faceless clip reads its frame and says it followed the "
              "montage: " +
                  facelessView.Dump());
        const mcp_json::Value* summary = reply.Find("summary");
        Check(summary != nullptr &&
                  summary->Find("clips_measured")->Dump() == "2" &&
                  summary->Find("frame_median_stops_after")->Dump() == "0.05",
              "the summary reads the frames after the proposals: " +
                  reply.Dump());
    }

    if (failures == 0) std::cout << "color_match: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
