#pragma once

#include "Document.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Render-time interpretation of a clip's color.* effect stack (see
// Document.h's ClipEffect/EffectParamValue and ROADMAP.md ticket F1.3). This
// is the EffectRegistry: a small, fixed table mapping each known
// "color.<name>" type string to a GPU-side dispatch id and its one numeric
// knob -- or, for the wheels and curves, the named knobs described further
// down. Adding a scalar knob later means adding one row to the registry
// (ColorEffects.cc) and one case to shader.metal's applyColorGrade switch --
// nothing here or in Document.h changes shape, and nothing upstream of this
// file (Operations.h, SetClipEffectsOperation, EditLog) is touched. The
// wheels and curves (F1.3, 18 September 2026) took the same road: their
// knobs are scalar params of the existing schema, so neither the document
// nor the journal format changed.
//
// This file only reads DocumentClip::effects; it has no write path and
// mutates nothing. LUTs remain out of scope (ROADMAP.md F1.3): an effect
// whose type is not one of the rows below is a silent no-op here, not an
// error, so a document carrying one (written by a later ticket, or opened
// from a newer build) still renders every effect this build understands
// instead of failing to render the clip at all. Document::Validate is the
// only place effect *acceptance* is decided (see ClipEffect::Validate in
// Document.cc, which enforces the "color." prefix generically, and
// ValidateColorEffect below for the two types whose knobs have a shape);
// this registry only decides which accepted types this build knows how to
// draw.

enum class ColorEffectKind : int32_t {
    Exposure = 0,
    Contrast = 1,
    Saturation = 2,
    Vibrance = 3,
    Temperature = 4,
    Tint = 5,
    Highlights = 6,
    Shadows = 7,
    // F1.3 -- ROADMAP.md, wheels and curves (18 September 2026). Several
    // knobs each, resolved into ResolvedColorGrade::data rather than
    // `amount`; see the wheels and curves section below.
    Wheels = 8,
    Curves = 9,
    // GR6 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. One knob, `degrees`,
    // resolved into a matrix in ResolvedColorGrade::data.
    Hue = 10,
    // GR8 -- six curves read along the hue, the luma or the saturation.
    HueCurves = 11,
    // GR16 -- a DaVinci Resolve node's primaries, as Resolve stores them.
    ResolvePrimaries = 12,
};

struct ColorEffectRegistryEntry {
    const char* type;      // Matches ClipEffect::type, e.g. "color.exposure".
    ColorEffectKind kind;  // GPU-side dispatch id (shader.metal switch).
    // The one numeric knob, read from ClipEffect::params. nullptr for
    // color.wheels and color.curves, whose knobs are named below.
    const char* param_name;
    float default_value;  // Used when the named param is absent.
    // GR1 -- where the stage sits in the canonical order (see
    // CanonicalColorStageIndex below); unique per type.
    int32_t canonical_rank;
};

// The registry: every color.* type this build knows how to render, in no
// particular order (lookup is by type string, not position).
const std::vector<ColorEffectRegistryEntry>& ColorEffectRegistry();

// Looks up one effect type ("color.exposure", ...). Returns nullptr for a
// type outside the table above -- callers must treat that as a no-op, not
// an error (see the file comment).
const ColorEffectRegistryEntry* FindColorEffectRegistryEntry(
    const std::string& type);

// The single explicit boundary where an exact EffectParamValue{num, den}
// becomes a float, mirroring how docs/reference/decisions-de-temps.md
// documents TimelineViewport as the sole time-to-pixel boundary. Every layer
// upstream of this call -- Document, EditLog, SetClipEffectsOperation -- keeps
// the value as an exact fraction (PHILOSOPHY.md principle 4); only this
// render-time resolve step (and, past it, the Metal kernel) ever sees a float.
// Returns 0 for a non-positive denominator rather than dividing by it:
// Document::Validate already rejects that shape for any document that reaches
// the renderer, so this is a defensive fallback for callers (tests, future
// tooling) that hand this function an unvalidated value directly.
float EffectParamValueToFloat(const EffectParamValue& value);

// One registry entry after crossing the float boundary above, ready to cross
// into Renderer.mm/shader.metal. `amount` is the one knob of the eight
// scalar effects; color.wheels and color.curves leave it at zero and carry
// their resolved knobs in ResolvedColorGrade::data instead, at
// [data_offset, data_offset + data_count).
struct ResolvedColorEffect {
    int32_t kind = static_cast<int32_t>(ColorEffectKind::Exposure);
    float amount = 0.0f;
    int32_t data_offset = 0;
    int32_t data_count = 0;
};

// A clip's resolved grade sits in a fixed-size slot because the Metal
// fragment shader consumes it as a plain array in a constant buffer, not a
// dynamically sized allocation. Stacks deeper than this are dropped, in
// document order, rather than reordered or silently merged; sixteen stages
// is generous for a grading stack limited to ten knob families with no
// duplication rule enforced.
inline constexpr int32_t kMaxColorEffectsPerClip = 16;

// F1.3 -- wheels and curves. The floats their stages resolve to share one
// pool per clip, sized so the whole grade still goes to the GPU through
// setFragmentBytes (Apple's limit for it is 4 KiB; see
// GpuClipGradeParameters). A full four-channel curve takes 196 floats and a
// wheel 9, so three full curves and every other stage fit. A stage that
// no longer fits ends the stack there, exactly like the depth limit above:
// the monitor and the export resolve the same truncated stack.
inline constexpr int32_t kMaxColorGradeData = 768;

struct ResolvedColorGrade {
    std::array<ResolvedColorEffect, kMaxColorEffectsPerClip> entries{};
    int32_t count = 0;
    std::array<float, kMaxColorGradeData> data{};
    int32_t data_count = 0;
    // GR7 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The index, in the
    // stack resolved, of the first stage the limits above left out, it and
    // every stage after it; -1 when the whole stack is here. Not sent to
    // the GPU: it is what lets a surface say so.
    int32_t dropped_from = -1;
};

// G7 -- docs/roadmap/GRADE-2026-09.md. Where a clip's grade acts, and the
// white balance its media was shot at: what color.temperature needs to be a
// colour temperature rather than a warm/cool slider labelled in Kelvin.
enum class ColorGradeSpace : int32_t {
    // No colour management: the display signal, BT.709 primaries, taken to
    // light by the BT.1886 2.4 power the Record monitor shows.
    Display = 0,
    // ACEScct code values, AP1 primaries.
    AcesCct = 1,
    // The linear working value of a managed project not grading in
    // ACEScct.
    LinearRec709 = 2,
    LinearRec2020 = 3,
};

struct ColorGradeContext {
    ColorGradeSpace space = ColorGradeSpace::Display;
    // LibraryMedia::white_balance_kelvin of the clip's media; 0 when
    // unknown, where kNeutralKelvin stands in.
    int32_t as_shot_kelvin = 0;
};

inline constexpr int32_t kNeutralKelvin = 6500;

// GR6 -- what color.contrast pivots about without a `pivot` of its own: 18 %
// middle grey as ACEScct encodes it, in every grading space, as it always
// did.
inline constexpr float kContrastPivot = 0.4135884f;
// The span of the Planckian locus approximation color.temperature uses (Kim
// et al., 2002). A kelvin outside it is taken at its end.
inline constexpr int32_t kMinimumKelvin = 1667;
inline constexpr int32_t kMaximumKelvin = 25000;

// The context of one clip of a document: its space from the project's colour
// management, its as-shot white balance from its media.
ColorGradeContext ColorGradeContextFor(const Document& document,
                                       const DocumentClip& clip);

// The neutral of color.temperature for a context: the as-shot white
// balance, or kNeutralKelvin when unknown.
int32_t ColorGradeNeutralKelvin(const ColorGradeContext& context);

// color.temperature's 3x3 matrix, row-major, on linear values in `space`'s
// primaries: the Bradford chromatic adaptation from the white of a
// blackbody at `sceneKelvin` to the white of one at `asShotKelvin` (both
// clamped to [kMinimumKelvin, kMaximumKelvin]). The camera balanced the
// picture for the as-shot light; a neutral lit by `sceneKelvin` then lands
// off neutral by exactly that adaptation's inverse, which this undoes. The
// identity when the two are equal.
std::array<float, 9> WhiteBalanceMatrix(int32_t sceneKelvin,
                                        int32_t asShotKelvin,
                                        ColorGradeSpace space);

// Pure, read-only interpretation of one clip's effect stack: looks up each
// entry's type in the registry (skipping anything unregistered, see the
// file comment), converts its one param to a float -- or, for color.wheels
// and color.curves, the per-channel values their formulas below consume --
// and preserves stack order. Never mutates `effects` and never touches
// Operations.h/EditLog -- this runs at render time only, the same boundary
// TransformColorForOutput in ColorManagement.h occupies for the display/export
// transform.
//
// G7 -- `context` says where the stack acts and what the clip was shot at;
// only color.temperature reads it (see the wheels and curves section).
ResolvedColorGrade ResolveColorGrade(const std::vector<ClipEffect>& effects,
                                     const ColorGradeContext& context);

// GR7 -- whether the monitor and the export render the whole of `effects`.
// They resolve the same stack and stop at the same stage when it holds more
// than kMaxColorEffectsPerClip stages or kMaxColorGradeData values; until
// this ticket nothing said so. False with, in `error`, the stage the picture
// stops before. The white balance is counted as if it were never neutral: a
// stack that fits on one media fits on every other.
//
// The operation itself does not refuse such a stack: undoing an edit of a
// clip that carried one before this check existed has to put it back.
// Refusing is for what builds a stack -- set_clip_effects does -- and the
// Inspector says it of the clip it shows.
bool ColorStackFits(const std::vector<ClipEffect>& effects, std::string& error);

// --- Canonical order (GR1 -- docs/roadmap/GRADE-RESOLVE-2026-10.md) -------
//
// A stack is an ordered list and renders in its order, whatever it is: an
// agent may write any. But a control that adds a stage has to put it
// somewhere, and "after the others" made the picture depend on which slider
// was touched first -- exposure then contrast and contrast then exposure
// read the same in the panel and render two pictures. The order below is
// the one a colour corrector node applies its tools in, in the application
// the timelines go to, kept to the stages this build has: shadow and
// highlight recovery, vibrance, white balance, exposure, the wheels, the
// contrast, the curves, the saturation.
//
// The rank of a registered type, -1 for any other.
int32_t CanonicalColorStageRank(const std::string& type);

// Whether the registered stages of `effects` come in non-decreasing rank.
// Anything unregistered is skipped: it renders nothing.
bool IsCanonicalColorStack(const std::vector<ClipEffect>& effects);

// Where a new stage of `type` goes in `effects`: before the first stage of a
// greater rank when the stack is canonical, after the last one otherwise --
// a stack written in another order is never reordered, which would change a
// picture without saying so, and its new stage goes where it always went.
size_t CanonicalColorStageIndex(const std::vector<ClipEffect>& effects,
                                const std::string& type);

// --- Wheels and curves (F1.3 -- ROADMAP.md, 18 September 2026) -----------
//
// Both are ordinary stages of the stack and act where every other stage
// acts, on the value applyColorGrade receives (shader.metal,
// fragment_working): ACEScct code values under colour management with the
// ACEScct working space, the scene-linear value under colour management
// with a linear working space, and the display-encoded signal (0..1,
// clamped after the whole stack) without colour management. Like the other
// stages they are not recalibrated per space: a lift of 0.05 adds 0.05 to
// an ACEScct code value or to a display signal alike.
//
// color.temperature -- G7, docs/roadmap/GRADE-2026-09.md (25 September
// 2026). One param, `kelvin`: the colour temperature of the light the scene
// was lit by, as a raw converter's slider reads. The stage adapts the white
// of a blackbody at that temperature to the white of one at the clip's
// as-shot white balance (ColorGradeContext), by Bradford, on linear light:
// the ACEScct stage decodes to linear AP1 and encodes back, a linear working
// space is used as it is, and without colour management the display signal
// is taken to light by the 2.4 power and back. At the as-shot value, or
// absent, the stage is neutral and resolves to nothing; lower cools the
// picture, higher warms it. It replaces a warm/cool offset of log2(K / 6500)
// * 0.6 that only its name tied to a temperature.
//
// color.contrast -- GR6, docs/roadmap/GRADE-RESOLVE-2026-10.md. Two params:
// `amount`, and since GR6 an optional `pivot` in [0, 1], the value the
// contrast holds still: y = (x - pivot) * (1 + amount) + pivot. Absent, the
// pivot is kContrastPivot and the stage resolves exactly as it did before
// the knob existed. A darker picture wants a lower pivot, so that raising
// the contrast does not crush its shadows; a lighter one a higher pivot.
//
// color.hue -- GR6. One param, `degrees`: every hue turned by that angle
// about the grey axis, luma kept (Rec.709 weights, as color.saturation).
// The angle is the vectorscope's and measure_color's, counted from +Cb
// toward +Cr: a positive rotation takes red toward yellow. It is a matrix
// on the values the stack acts on, computed here (HueRotationMatrix) and
// handed to the shader like the white balance's. A multiple of 360 resolves
// to nothing.
//
// color.wheels -- the lift, gamma, gain and offset wheels. Each wheel is a
// colour plus a master: sixteen scalar params named <wheel>_<channel>, wheel
// in {lift, gamma, gain, offset}, channel in {r, g, b, master}. Every param
// is optional, 0 (neutral) when absent, and must lie in [-1, 1]. For each
// channel c the colour and the master add, then
//
//   L = lift_c + lift_master          the output at input 0, before O
//   K = 2^(gain_c + gain_master)      the output at input 1, before O
//   G = 2^(gamma_c + gamma_master)
//   O = offset_c + offset_master
//   y = (K - L) * x + L + O
//   y = y^(1/G) where y > 0, y unchanged elsewhere
//
// which is the lift/gamma/gain of a Grade node with its black point at 0
// and white point at 1: lift moves the black end and holds the white one,
// gain moves the white end and holds the black one, gamma bends the values
// between and holds both. Gain and gamma are powers of two so that 0 is
// neutral, +1 doubles, -1 halves, and neither reaches zero within the
// bounds. Under ACEScct the two held ends are code values 0 and 1 (linear
// -0.0069 and 222.9; black is code 0.0729), as in any application grading
// in ACEScct.
//
// The offset (GR2 -- docs/roadmap/GRADE-RESOLVE-2026-10.md) moves both ends
// together: every value of a channel by the same amount, which on log codes
// is the printer light of a film grade. The stage stays one ASC CDL, slope
// K - L, offset L + O, power 1/G (ColorCdl.h), and resolves to the same nine
// values as before -- L + O, K + O and 1/G -- so neither the shader nor the
// export reads anything new. Until G7 a coloured offset was spelled
// color.temperature plus color.tint, and no fourth wheel was needed; since
// color.temperature became a chromatic adaptation, nothing else spells it.
// Its master is color.exposure's move (amount / 17.52) under another name,
// kept because the wheel is where a hand looks for it.
//
// color.curves -- one curve per channel: master on R, G and B first, then r,
// g and b on their own channel. A curve is a list of points, params
// <channel>_x<i> and <channel>_y<i> for i = 0, 1, 2... without a gap,
// channel in {master, r, g, b}. A channel has no point (identity) or 2 to
// kMaxColorCurvePoints; both coordinates of a point are present; x and y
// lie in [0, 1]; x strictly increases with i. Validation refuses anything
// else, a curve that goes back in x included.
//
// Between points the curve is the monotone cubic Hermite interpolant of
// Fritsch and Carlson (1980). With secants d_k, a point's tangent is the
// mean of the secants on either side, or zero where they differ in sign or
// either is zero; the end tangents are the end secants; both tangents of a
// flat interval are zero. Then, interval by interval in increasing x, where
// a = m_k / d_k and b = m_(k+1) / d_k give a^2 + b^2 > 9, both are scaled
// by 3 / sqrt(a^2 + b^2). On every interval the curve then stays between
// its two end values: it never overshoots a point, and a rising run of
// points gives a rising curve. Past the first and last points it goes on
// as a straight line along the end tangent, so values outside [0, 1]
// (ACEScct and linear light have them) keep their order instead of being
// clipped.
//
// color.hue_curves -- GR8, docs/roadmap/GRADE-RESOLVE-2026-10.md. Six
// curves that change one part of a colour according to another, each a list
// of points like a color.curves channel: params <curve>_x<i> and
// <curve>_y<i>, none or 2 to kMaxColorCurvePoints points, x strictly
// increasing, x and y in [0, 1]. Without a point a curve changes nothing.
//
//   hue_hue   along the hue        turns the hue by (y - 1/2) of a turn
//   hue_sat   along the hue        scales the saturation by 2y
//   hue_lum   along the hue        adds (y - 1/2) to the luma
//   lum_sat   along the luma       scales the saturation by 2y
//   sat_sat   along the saturation scales the saturation by 2y
//   sat_lum   along the saturation adds (y - 1/2) to the luma
//
// so y = 1/2 is neutral on all six. The pixel is read once, as it enters
// the stage, with the Rec.709 weights of color.saturation and color.hue:
// luma Y, chroma (Cb, Cr), the hue their angle counted from +Cb toward +Cr
// as a fraction of a turn -- the vectorscope's and measure_color's angle
// divided by 360, so that a hue measured on a picture is the x to put a
// point at -- the saturation twice the chroma's length, clamped to 1 (a
// pure primary sits at 1), and the luma clamped to [0, 1] to be read along.
// All six curves are read at those values, then applied together: the hue
// turned, the chroma scaled by the product of the three factors, the luma
// moved by the sum of the two offsets. hue_lum fades to nothing below a
// saturation of kHueCurveLumaFade: a grey has no hue to be lit by.
//
// A curve along the hue goes round: it is periodic, its last point followed
// by its first a turn later, and x = 1 is refused as the hue 0 again. The
// other three hold their first value before their first point and their
// last after their last. Between points the interpolation is color.curves's
// monotone cubic, which never overshoots a point.
//
// color.resolve_primaries -- GR16, docs/roadmap/GRADE-RESOLVE-2026-10.md
// (DEC46, 25 October 2026). The primaries of one DaVinci Resolve corrector
// node, at the values Resolve stores (ResolveGrade.h reads them), rendered
// as Resolve 21.1 renders them. color.wheels is not that law, and changing
// it would change every project graded with it; an imported grade gets its
// own stage instead. Params, all optional:
//
//   lift_<c>, gamma_<c>, gain_<c>   c in {r, g, b, master}; 0, 0, 1 neutral
//   offset_<c>                      c in {r, g, b}; 0 neutral
//   saturation                      1 neutral (Resolve's panel shows 50)
//   lum_mix                         in [0, 1]; 1 when absent (the panel's 100)
//   contrast                        1 neutral; up to 9/5 with the S-curve,
//                                   below 2 without it
//   pivot                           in [0, 1]; kResolvePivot when absent
//   color_space_aware               0 or 1, the project's setting; 0 absent
//                                   (measured at HDR 1000 working luminance)
//   contrast_s_curve                0 or 1, the project's setting; 1 absent
//   acescct                         0 or 1: the node was graded in a project
//                                   in ACEScct; 0 absent
//
// The law was measured on 25 October 2026 with grades written into .drx
// files and Resolve's own ExportLUT cubes, to 0.05 of a 10-bit code on every
// lattice point of 80 cubes. Per channel, x the value, L, G, g, O its lift,
// gain, gamma and offset:
//
//   a = L G / (1 - L + L G),  b = G (1 - L) / (1 - L + L G)
//   v = a + b x + kResolveOffsetScale O
//   c = v^e, e = 1 / (1 + g) for g >= 0 and 1 - g below; a negative v
//       keeps its sign, -(|v|^e)
//
// The masters are Resolve's Y channel: the same law on the luma of x
// (Rec.709 weights), the offsets counted by their luma, gives a target luma
// T, and every channel gets lum_mix (T - Y(c)). At lum_mix 1 a colour that
// moves one channel keeps the picture's luma; a master alone moves the luma
// alone. Then the saturation s about the luma of the result, with the
// weights DaVinci YRGB uses (0.21, 0.70, 0.09, GR12): Y + s (out - Y).
//
// Resolve grades on its timeline's values, DaVinci Intermediate in DaVinci
// Wide Gamut for the projects measured in DaVinci YRGB and Color Managed
// v2. The stage does the same: under colour management it takes the working
// value to linear light, to DaVinci Wide Gamut (Bradford from AP1's white),
// to DaVinci Intermediate, applies the law, and goes back; without colour
// management it applies the law to the display signal, as an unmanaged
// Resolve project does.
//
// In a project in ACEScct (`acescct` 1, measured on 14 November 2026), the
// node's values are ACEScct codes in AP1: the stage takes the working value
// there instead (as it is, when the engine grades in ACEScct), and the law
// changes in three places. A negative v goes through the gamma unchanged
// (c = v), the masters' target too; the saturation turns about Rec.709's
// weights (0.2126, 0.7152, 0.0722); the temperature and tint act in
// ACES2065-1 (AP0) light taken to a 2.4 power (below). The contrast is the
// one without the colour-space-aware tools, on the node's values, and the
// working luminance changes nothing; with the tools on, it follows another
// law, never measured, which the stage refuses by name.
//
// The contrast comes after the primaries' Y part and before the saturation,
// as the curve ResolveContrastTable.h describes, and its own Y part follows
// the same lum_mix. Without the colour-space-aware tools it acts on the
// node's values: the channels, then one luma restoration towards the
// contrasted target luma. With them, Resolve applies it in two steps: the
// primaries and their Y part as above, then the contrast and its Y part
// (Rec.709 weights) on HLG codes, HLG(DaVinci Intermediate^-1(x) / 12).
// Measured within 0.75 of a 10-bit code with the tools on and 1.73 without,
// Lum Mix 100 near black the worst.
//
//   temperature                     in [-4000, 4000], 0 neutral
//   tint                            in [-100, 100], 0 neutral
//
// GR16, measured on 6 November 2026: temperature and tint act first, on the
// node's values, before the lift, gamma and gain, as a 3x3 matrix that
// ResolveWhiteBalanceMatrix gives. The rest of the law then reads the
// balanced values, its Y part included. In ACEScct (14 November 2026) the
// same matrix acts on p = AP0(light)^(1/2.4), a negative keeping its sign:
// the node's codes to linear AP1, to AP0 (kResolveAp1ToAp0), to that power,
// the matrix, back to the power 2.4, to AP1 and to ACEScct codes. A grey
// moves by a constant in log: its AP0 light is multiplied, channel by
// channel, by the matrix's row sums to the power 2.4.
//
// A wheel stage whose knobs are all neutral, a curve stage without a point
// and a neutral color.resolve_primaries resolve to nothing, so such a clip
// reaches the export exactly as a clip without the stage.
inline constexpr int32_t kColorWheelLimit = 1;
inline constexpr int32_t kMaxColorCurvePoints = 16;
inline constexpr std::array<const char*, 4> kColorWheelNames = {
    "lift", "gamma", "gain", "offset"};
inline constexpr std::array<const char*, 4> kColorWheelChannels = {
    "r", "g", "b", "master"};
inline constexpr std::array<const char*, 4> kColorCurveChannels = {
    "master", "r", "g", "b"};
// GR8 -- color.hue_curves's six, in the order they resolve and apply.
inline constexpr std::array<const char*, 6> kHueCurveNames = {
    "hue_hue", "hue_sat", "hue_lum", "lum_sat", "sat_sat", "sat_lum"};
inline constexpr float kHueCurveLumaFade = 0.1f;
// GR16 -- what one unit of a stored Resolve offset adds to a value, measured
// (0.1832 +- 0.0001), and the floats one color.resolve_primaries resolves
// to: the space, the matrices to and from DaVinci Wide Gamut, then per
// channel a, b, the scaled offset and the exponent, the master's a, b and
// exponent, the offsets' luma, lum_mix, the saturation and the three luma
// weights it turns about (DaVinci YRGB's, or Rec.709's in ACEScct), then 1
// for a node graded in ACEScct, 0 otherwise: under colour management the
// matrices then go to and from AP1, and the node's values are ACEScct codes
// rather than DaVinci Intermediate.
inline constexpr float kResolveOffsetScale = 0.1832f;
inline constexpr int32_t kResolvePrimariesDataSize = 41;
// GR16 -- with a contrast, the block goes on: where it acts (1 the node's
// values, without the colour-space-aware tools; 2 HLG codes, with them),
// the end of the domain its curve spans, and the curve's 65 values
// (ResolveContrastTable.h). Resolve's pivot is 0.435 when none is stored.
inline constexpr int32_t kResolvePrimariesContrastDataSize = 108;
inline constexpr float kResolvePivot = 0.435f;
// GR16 -- with a temperature or a tint, the block ends with the nine floats
// of their matrix, row-major, after the contrast when there is one (50 or
// 117 floats): a node without them resolves to exactly the block it always
// did.
inline constexpr int32_t kResolveWhiteBalanceDataSize = 9;
// GR16 -- in ACEScct, where the temperature and tint act: linear AP1 to
// ACES2065-1 and back (ACES reference matrices, the ones shader.metal
// carries), and the power AP0 light is taken to (measured on 14 November
// 2026: 2.35 and 2.45 miss a grey by 0.01 stop, 2.4 by 0.0002).
inline constexpr std::array<float, 9> kResolveAp1ToAp0 = {
    0.6954522414f, 0.1406786965f,  0.1638690622f, 0.0447945634f, 0.8596711185f,
    0.0955343182f, -0.0055258826f, 0.0040252103f, 1.0015006723f};
inline constexpr std::array<float, 9> kResolveAp0ToAp1 = {
    1.4514393161f,  -0.2365107469f, -0.2149285693f,
    -0.0765537734f, 1.1762296998f,  -0.0996759264f,
    0.0083161484f,  -0.0060324498f, 0.9977163014f};
inline constexpr float kResolveAcesBalancePower = 2.4f;

// GR16 -- the 3x3 matrix, row-major, that a Resolve node's stored
// temperature and tint make on the node's values; the identity at 0 and 0.
std::array<double, 9> ResolveWhiteBalanceMatrix(double temperature,
                                                double tint);

struct ColorCurvePoint {
    EffectParamValue x;
    EffectParamValue y;
};

// One channel of a color.curves effect, in increasing x, checked as the
// comment above says. Empty for a channel without points. Only reads the
// params of `channel`; ValidateColorEffect checks the rest.
bool ReadColorCurveChannel(const ClipEffect& effect, const std::string& channel,
                           std::vector<ColorCurvePoint>& points,
                           std::string& error);

// The type-specific half of effect validation, run by Document::Validate on
// every effect. color.wheels, color.curves and color.resolve_primaries (GR16)
// accept their named params only, within their bounds; every other type is
// accepted unchanged, so the scalar effects keep the open param maps they
// always had -- but for color.contrast's `pivot` (GR6), which must lie in [0,
// 1] when it is given.
bool ValidateColorEffect(const ClipEffect& effect, std::string& error);

// GR6 -- color.hue's 3x3 matrix, row-major: the rotation of every hue by
// `degrees` about the grey axis, Rec.709 luma kept. The identity at 0.
std::array<float, 9> HueRotationMatrix(double degrees);

// GR8 -- one curve of a color.hue_curves effect, as ReadColorCurveChannel
// reads a channel; a curve along the hue also refuses a point at x = 1.
bool ReadHueCurve(const ClipEffect& effect, const std::string& curve,
                  std::vector<ColorCurvePoint>& points, std::string& error);

// Whether a curve of kHueCurveNames is read along the hue, and so periodic.
bool IsHueIndexedCurve(const std::string& curve);

// Evaluates one resolved hue curve, a block as EvaluateColorCurve's; one
// without points is neutral, 1/2. shader.metal's hueCurve is the same.
float EvaluateHueCurve(const float* block, float x);

// Fritsch-Carlson tangents for points already in strictly increasing x.
std::vector<float> MonotoneCurveTangents(const std::vector<float>& x,
                                         const std::vector<float>& y);

// Evaluates one resolved curve channel, a block [n, x0, y0, m0, x1, ...] of
// ResolvedColorGrade::data; n < 2 is the identity. shader.metal's
// evaluateCurve is the same arithmetic.
float EvaluateColorCurve(const float* block, float x);

// F1.3 -- offline export samples the same ordered grading math into a .cube
// so FFmpeg grades each visible source before timeline compositing. This
// reference is display-referred, matching the monitor without project color
// management; scene-linear managed export has a separate transform order.
std::array<float, 3> ApplyResolvedColorGrade(std::array<float, 3> color,
                                             const ResolvedColorGrade& grade);
bool BuildColorGradeCube(const ResolvedColorGrade& grade, std::string& cube,
                         std::string& error);

// Mirrors shader.metal's GradeEntry/ClipGradeParameters byte for byte: Metal
// Shading Language cannot include this header, so the shader keeps its own
// copy and tests/color_grade_shader_tests.mm checks the two against each
// other on the GPU. Renderer.mm sends exactly what PackColorGradeForGpu
// returns.
struct GpuColorGradeEntry {
    int32_t kind;
    float amount;
    int32_t data_offset;
    int32_t data_count;
};

struct GpuClipGradeParameters {
    int32_t count;
    // Plain scalars: see shader.metal's ClipGradeParameters::padding.
    int32_t padding[3];
    GpuColorGradeEntry entries[kMaxColorEffectsPerClip];
    float data[kMaxColorGradeData];
};

// setFragmentBytes takes at most 4 KiB.
static_assert(sizeof(GpuClipGradeParameters) <= 4096,
              "the clip grade no longer fits setFragmentBytes");

GpuClipGradeParameters PackColorGradeForGpu(const ResolvedColorGrade& grade);
