#pragma once
// G4 -- docs/roadmap/GRADE-2026-09.md. The ASC Color Decision List: the
// format primary corrections travel in between applications -- a slope, an
// offset and a power per channel, then one saturation, ten numbers applied as
//
//   y = (x * slope + offset) ^ power      per channel
//   y = luma + saturation * (y - luma)    Rec.709 luma weights
//
// This engine already grades with that formula. color.wheels is
// (K - L) * x + L + O, then the power 1/G: slope K - L, offset L + O, power
// 1/G (ColorEffects.h; O is the offset wheel, GR2). color.saturation is the
// CDL's saturation with the same Rec.709 weights, factor 1 + amount. Below
// zero, the wheel leaves a value out of its power, and so does
// OpenColorIO's no-clamp CDL (CDL_NO_CLAMP, the style it reads files in);
// the ASC's v1.2 style would clamp to [0, 1] first, which under ACEScct
// would cut the codes above 1.0. The two
// formulas were checked against each other on OpenColorIO's own processor
// (tests/color_cdl_tests.cc), negative values and values above 1 included.
// A CDL is therefore not a new kind of stage: it comes in as a color.wheels
// and a color.saturation, and goes out as what those and the other affine
// stages of a clip fold into.
//
// A CDL has no colour space of its own: its numbers mean something only on
// the values it was made on. The engine never guesses which (the fourth
// guard rail of LookLut.h): a CDL goes out labelled with the space the
// clip's grade acts in, and comes in only on a space its author declares.
//
// Files -- .cc, .ccc and .cdl -- are read and written by OpenColorIO, pinned
// with the rest of the colour pipeline, so no parser of this format lives
// here.

#include "ColorEffects.h"
#include "Document.h"
#include "Json.h"

#include <array>
#include <string>
#include <vector>

struct ColorCdl {
    // The ColorCorrection id; empty when the correction has none.
    std::string id;
    // Its first Description, for a person reading the file.
    std::string description;
    std::array<double, 3> slope = {1.0, 1.0, 1.0};
    std::array<double, 3> offset = {0.0, 0.0, 0.0};
    std::array<double, 3> power = {1.0, 1.0, 1.0};
    double saturation = 1.0;
};

// The values a clip's grade acts on, as apply_cdl's `space` declares them
// ("acescct", "linear_rec709", "linear_rec2020", or "display": the
// display-encoded signal, without colour management), and as a CDL's
// InputDescription names them for a person ("ACEScct", "Linear Rec.709",
// "Linear Rec.2020", "Display").
const char* ColorCdlSpaceId(ColorGradeSpace space);
const char* ColorCdlSpaceName(ColorGradeSpace space);
bool ParseColorCdlSpace(const std::string& id, ColorGradeSpace& space);

// The CDL a resolved grade folds into, as it renders: exposure, contrast and
// tint are per-channel affine moves, a wheel is one with a power after it,
// and saturation comes last. Moves compose while the order allows it. An
// affine move after a power has no place in slope-offset-power; after the
// saturation, a move with the same slope on every channel is carried before
// it (its offsets through the saturation's inverse, exactly), which is what
// a clip match_color placed a tint after its saturation needs, and a slope
// that differs by channel is not. Stages that resolve to nothing are
// skipped.
// Refused by the stage's type otherwise -- color.temperature (a white
// balance on linear light), color.curves, color.vibrance,
// color.highlights, color.shadows -- and when the fold gives a negative
// slope, which a CDL forbids.
bool FoldColorCdl(const ResolvedColorGrade& grade, ColorCdl& cdl,
                  std::string& error);

// The two stages a CDL becomes, as exact fractions a document carries: a
// color.wheels (lift = offset, gain = log2(slope + offset), gamma =
// -log2(power)) and a color.saturation (amount = saturation - 1), each
// omitted when neutral. The offset and the saturation are the file's
// decimals exactly (to the billionth past nine decimals, which only a
// computed CDL has); gain and gamma are logarithms, rounded to millionths,
// which moves a slope or a power by less than 1e-6 of itself. A wheel's
// knobs lie in [-1, 1]; a channel's value past that is split between the
// channel and the master, and refused by value when even that does not
// hold. Refused as well: slope + offset or power not positive, a negative
// saturation. `deviation` is the largest difference between the CDL and
// what the stages render, over the knobs.
bool ColorCdlStages(const ColorCdl& cdl, std::vector<ClipEffect>& stages,
                    double& deviation, std::string& error);

// A clip's stack with `stages` (ColorCdlStages's) in place of its primary:
// every color.exposure, color.contrast, color.tint, color.wheels and
// color.saturation stage removed -- a CDL is the whole primary, so keeping
// them would apply it twice -- and the CDL's stages put where the first of
// them stood, or after the stack when there was none. Their ids are the
// clip's first wheel's and first saturation's, when it had them. The types
// removed are listed in `replaced`, in stack order.
std::vector<ClipEffect> PlaceColorCdl(const std::vector<ClipEffect>& stack,
                                      const std::vector<ClipEffect>& stages,
                                      std::vector<std::string>* replaced);

// A double as the shortest decimal that reads back as the same double, as
// an exact fraction of a power of ten -- what a CDL file wrote, when it
// wrote a decimal. False past nine decimals or past what int32_t holds.
bool DecimalFraction(double value, EffectParamValue& fraction);
// That shortest decimal, as text.
std::string ShortestDecimal(double value);

// --- Clips -----------------------------------------------------------------

// The CDL of each clip as it renders (FoldColorCdl on its resolved grade),
// to six decimals, id the clip's, description its media's file name, and
// the space their grades act in. Refused by clip: unknown, not on a video
// track, a motion clip (drawn by the engine, graded by no one), or a stage with
// no CDL form.
bool ClipColorCdls(const Document& document, const std::vector<Ulid>& clipIds,
                   std::vector<ColorCdl>& cdls, ColorGradeSpace& space,
                   std::string& error);

// What a clip's stack becomes with `cdl` in place of its primary
// (ColorCdlStages, then PlaceColorCdl). `declared` is the space the CDL was
// made on; it must be the one the clip's grade acts in -- a CDL made on
// display values means another picture on ACEScct codes, and no
// arithmetic here turns one into the other.
struct ClipCdlPlacement {
    std::vector<ClipEffect> effects;
    std::vector<std::string> replaced;
    double deviation = 0.0;
};
bool PlaceClipCdl(const Document& document, const Ulid& clipId,
                  const ColorCdl& cdl, ColorGradeSpace declared,
                  ClipCdlPlacement& placement, std::string& error);

// Resolve's TimelineItem.SetCDL takes the same ten numbers as strings,
// "r g b" per knob: the map a script hands it for `node` (1-based).
struct ResolveCdlMap {
    std::string node_index;
    std::string slope;
    std::string offset;
    std::string power;
    std::string saturation;
};
ResolveCdlMap ResolveSetCdlMap(const ColorCdl& cdl, int node);
// The inverse: those strings, three numbers each for slope, offset and power,
// one for the saturation. Resolve has no GetCDL to read them back (21.1,
// measured in GR12).
bool ParseResolveCdlMap(const ResolveCdlMap& map, ColorCdl& cdl,
                        std::string& error);

// GR12, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. DaVinci Resolve
// weighs a CDL's saturation with 0.21 R + 0.70 G + 0.09 B in a DaVinci YRGB
// project -- unmanaged, colour-managed, on a Rec.709 or a DaVinci Wide Gamut
// timeline alike (measured in Resolve Studio 21.1, 10 October 2026) -- and
// with the Rec.709 weights this CDL is made with only under ACEScct. A
// saturation s moves a channel by (1 - s) times the difference between the
// two lumas, which on values in [0, 1] is at most 0.0178 (blue against red
// and green). No single saturation converts exactly, so export_cdl warns
// instead: this is the largest gap, in 10-bit codes, rounded to a tenth --
// 1.8 for 1.1, 9.1 for 0.5 or 1.5, 18.2 for 0, the codes GR12 measured.
inline constexpr std::array<double, 3> kResolveYrgbSaturationWeights = {
    0.21, 0.70, 0.09};
double ResolveYrgbSaturationGapCodes(double saturation);

// --- Replies ---------------------------------------------------------------

// A CDL's ten numbers, each the shortest decimal that reads back as it.
mcp_json::Value ColorCdlValue(const ColorCdl& cdl);

// export_cdl's reply, the MCP tool's and --export-cdl's: the space, and per
// clip its CDL and the map Resolve's SetCDL takes for node `resolveNode`;
// `written` the file, or empty. GR12 -- a clip whose saturation is not 1
// carries `resolve_yrgb_gap_codes` (ResolveYrgbSaturationGapCodes), and the
// reply a `warning` naming them in `warning_clip_ids`.
mcp_json::Value ExportedCdlsValue(const std::vector<ColorCdl>& cdls,
                                  ColorGradeSpace space, int resolveNode,
                                  const std::string& written);

// --- Files, through OpenColorIO --------------------------------------------

// Every ColorCorrection of a .cc, .ccc or .cdl file, in file order, and the
// file's InputDescription (empty when it has none).
bool ReadColorCdlFile(const std::string& path, std::vector<ColorCdl>& cdls,
                      std::string& inputDescription, std::string& error);

// `cdls` written to `path` in the format its extension names (.cc holds
// exactly one), `inputDescription` as the file's InputDescription.
bool WriteColorCdlFile(const std::string& path,
                       const std::vector<ColorCdl>& cdls,
                       const std::string& inputDescription, std::string& error);
