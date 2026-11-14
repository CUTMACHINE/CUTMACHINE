#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertex_main(uint vertexId [[vertex_id]]) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0),
    };
    const float2 uvs[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 0.0), float2(0.0, 0.0),
    };
    VertexOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    out.uv = uvs[vertexId];
    return out;
}

float3 sampleYUV(float2 uv, texture2d<float> yTexture,
                 texture2d<float> uTexture, texture2d<float> vTexture,
                 sampler planeSampler, float redFromCr, float greenFromCb,
                 float greenFromCr, float blueFromCb, float sampleScale,
                 float yOffset, float yScale, float chromaOffset,
                 float chromaScale) {
    // R16 textures normalize against 65535 although FFmpeg stores 9-12 bit
    // planar samples in the low bits. sampleScale restores code/maxCode.
    const float yCode = yTexture.sample(planeSampler, uv).r * sampleScale;
    const float uCode = uTexture.sample(planeSampler, uv).r * sampleScale;
    const float vCode = vTexture.sample(planeSampler, uv).r * sampleScale;
    const float y = (yCode - yOffset) * yScale;
    const float cb = (uCode - chromaOffset) * chromaScale;
    const float cr = (vCode - chromaOffset) * chromaScale;

    return float3(y + redFromCr * cr, y + greenFromCb * cb + greenFromCr * cr,
                  y + blueFromCb * cb);
}

struct PresentationParameters {
    float left;
    float top;
    float width;
    float height;
    int quarterTurns;
    float opacity;
    int colorManagementEnabled;
    int inputGamut;
    int inputTransfer;
    int useAcescct;
    int inputIsRgb;
    int hasSourceAlpha;
    float redFromCr;
    float greenFromCb;
    float greenFromCr;
    float blueFromCb;
    float sampleScale;
    float yOffset;
    float yScale;
    float chromaOffset;
    float chromaScale;
    // R1 -- docs/roadmap/REEL-2026-09.md. The part of the *displayed*
    // picture to sample, as fractions of it: what ClipFraming.h named as
    // the source rectangle, so that a filled or cropped shot samples the
    // same region the export crops. 0,0,1,1 is the whole picture, which is
    // what an unframed clip is given.
    float sourceLeft;
    float sourceTop;
    float sourceWidth;
    float sourceHeight;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. The clip's mask (ClipMask.h):
    // 0 for none, 1 a rectangle, 2 an ellipse; its centre, width and height
    // as fractions of the displayed picture, its feather, and whether what
    // is outside the shape is what is kept.
    int maskShape;
    float maskCenterX;
    float maskCenterY;
    float maskWidth;
    float maskHeight;
    float maskFeather;
    int maskInverted;
};

// PA8 -- the mask's coverage where this fragment falls in the displayed
// picture: ClipMask.h's rule, ClipMaskCoverage, line for line, which
// tests/monitor_export_parity_tests.mm holds to the export's. The point is
// the one presentationUV samples, before the turn: the mask is placed on
// the picture as displayed.
float clipMaskCoverage(float2 outputUV, constant PresentationParameters& p) {
    if (p.maskShape == 0) return 1.0;
    const float2 displayUV =
        float2(p.sourceLeft, p.sourceTop) +
        ((outputUV - float2(p.left, p.top)) / float2(p.width, p.height)) *
            float2(p.sourceWidth, p.sourceHeight);
    const float2 offset =
        (displayUV - float2(p.maskCenterX, p.maskCenterY)) /
        (float2(p.maskWidth, p.maskHeight) * 0.5);
    const float distance = p.maskShape == 2
                               ? length(offset)
                               : max(abs(offset.x), abs(offset.y));
    const float inside =
        p.maskFeather > 0.0 ? clamp((1.0 - distance) / p.maskFeather, 0.0, 1.0)
                            : (distance <= 1.0 ? 1.0 : 0.0);
    return p.maskInverted != 0 ? 1.0 - inside : inside;
}

float3 decodeTransfer(float3 signal, int transfer) {
    if (transfer == 2) return signal;
    if (transfer == 1) {
        // Sony's published full-range formula. 420/1023 maps to 18% scene
        // reflection and the linear branch changes at 171.2102946929/1023.
        const float3 high =
            pow(10.0, (signal * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
        const float3 low =
            (signal * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
        return select(low, high, signal >= 171.2102946929 / 1023.0);
    }
    const float3 positive = max(signal, 0.0);
    return select(positive / 4.5, pow((positive + 0.099) / 1.099, 1.0 / 0.45),
                  positive >= 0.081);
}

float3 mulRows(float3 value, float3 row0, float3 row1, float3 row2) {
    return float3(dot(value, row0), dot(value, row1), dot(value, row2));
}

float3 sourceToAP1(float3 rgb, int gamut) {
    float3 ap0;
    if (gamut == 1) {  // ACES reference CSC: S-Gamut3.Cine to ACES2065-1.
        ap0 = mulRows(rgb, float3(0.6387886672, 0.2723514337, 0.0888598991),
                      float3(-0.0039159060, 1.0880732309, -0.0841573249),
                      float3(-0.0299072021, -0.0264325799, 1.0563397820));
    } else if (gamut == 2) {  // ACES reference CSC: S-Gamut3 to ACES2065-1.
        ap0 = mulRows(rgb, float3(0.7529825954, 0.1433702162, 0.1036471884),
                      float3(0.0217076974, 1.0153188355, -0.0370265329),
                      float3(-0.0094160527, 0.0033704179, 1.0060456349));
    } else {
        float3 xyzD65;
        if (gamut == 3) {
            xyzD65 = mulRows(rgb, float3(0.636958, 0.144617, 0.168881),
                             float3(0.262700, 0.677998, 0.059302),
                             float3(0.000000, 0.028073, 1.060985));
        } else {
            xyzD65 = mulRows(rgb, float3(0.412391, 0.357584, 0.180481),
                             float3(0.212639, 0.715169, 0.072192),
                             float3(0.019331, 0.119195, 0.950532));
        }
        const float3 xyzD60 =
            mulRows(xyzD65, float3(1.013030, 0.006105, -0.014971),
                    float3(0.007698, 0.998165, -0.005032),
                    float3(-0.002841, 0.004685, 0.924507));
        ap0 = mulRows(xyzD60, float3(1.049811, 0.000000, -0.000097),
                      float3(-0.495903, 1.373313, 0.098240),
                      float3(0.000000, 0.000000, 0.991252));
    }
    // ACES2065-1 (AP0) to the AP1 primaries shared by ACEScg/ACEScct.
    return mulRows(ap0, float3(1.4514393161, -0.2365107469, -0.2149285693),
                   float3(-0.0765537734, 1.1762296998, -0.0996759264),
                   float3(0.0083161484, -0.0060324498, 0.9977163014));
}

float3 linearAP1ToACEScct(float3 value) {
    const float3 high = (log2(value) + 9.72) / 17.52;
    const float3 low = value * 10.5402377416545 + 0.0729055341958355;
    return select(low, high, value > 0.0078125);
}

float3 acesCctToLinearAP1(float3 value) {
    constexpr float breakpoint = 0.155251141552511;
    const float3 high = exp2(value * 17.52 - 9.72);
    const float3 low = (value - 0.0729055341958355) / 10.5402377416545;
    return select(low, high, value > breakpoint);
}

float3 ap1ToOutput(float3 ap1, int outputGamut) {
    const float3 xyzD60 =
        mulRows(ap1, float3(0.6624541811, 0.1340042065, 0.1561876870),
                float3(0.2722287168, 0.6740817658, 0.0536895174),
                float3(-0.0055746495, 0.0040607335, 1.0103391003));
    const float3 xyzD65 =
        mulRows(xyzD60, float3(0.9872240087, -0.0061132286, 0.0159532883),
                float3(-0.0075983718, 1.0018614847, 0.0053300358),
                float3(0.0030725771, -0.0050959615, 1.0816806031));
    if (outputGamut == 1) {
        return mulRows(xyzD65, float3(1.716651, -0.355671, -0.253366),
                       float3(-0.666684, 1.616481, 0.015769),
                       float3(0.017640, -0.042771, 0.942103));
    }
    return mulRows(xyzD65, float3(3.240970, -1.537383, -0.498611),
                   float3(-0.969244, 1.875968, 0.041555),
                   float3(0.055630, -0.203977, 1.056972));
}

float3 mapLinearOutputToDisplay(float3 color, int outputGamut,
                                int outputTransfer) {
    const bool hlg = outputTransfer == 1;
    const float maximum = hlg ? (1.0 / 0.2944028442) : 1.0;
    const float knee = hlg ? 0.9 : 0.72;
    const float3 weights = outputGamut == 1 ? float3(0.2627, 0.6780, 0.0593)
                                            : float3(0.2126, 0.7152, 0.0722);
    const float luminance = dot(color, weights);
    if (luminance > knee) {
        const float shoulder =
            maximum -
            (maximum - knee) * exp(-(luminance - knee) / (maximum - knee));
        color *= shoulder / luminance;
    }

    const float mappedLuminance = dot(color, weights);
    const float positiveRoom = max(maximum - mappedLuminance, 1e-9);
    const float negativeRoom = max(mappedLuminance, 1e-9);
    const float3 chroma = color - mappedLuminance;
    const float3 excursion =
        select(-chroma / negativeRoom, chroma / positiveRoom, chroma >= 0.0);
    const float ratio = max(excursion.r, max(excursion.g, excursion.b));
    constexpr float chromaKnee = 0.75;
    constexpr float chromaLimit = 0.98;
    if (ratio > chromaKnee) {
        const float compressed =
            chromaKnee +
            (chromaLimit - chromaKnee) *
                (1.0 - exp(-(ratio - chromaKnee) / (chromaLimit - chromaKnee)));
        color = mappedLuminance + chroma * (compressed / ratio);
    }
    return clamp(color, 0.0, maximum);
}

float3 encodeOutput(float3 linear, int transfer) {
    linear = max(linear, 0.0);
    if (transfer == 1) {
        constexpr float a = 0.17883277;
        constexpr float b = 0.28466892;
        constexpr float c = 0.55991073;
        return clamp(select(sqrt(3.0 * linear), a * log(12.0 * linear - b) + c,
                            linear > (1.0 / 12.0)),
                     0.0, 1.0);
    }
    return clamp(select(4.5 * linear, 1.099 * pow(linear, 0.45) - 0.099,
                        linear >= 0.018),
                 0.0, 1.0);
}

// --- Creative color grading (F1.3) -----------------------------------
//
// Interprets a clip's resolved color.* effect stack (Document.h's
// ClipEffect/EffectParamValue, ColorEffects.h's EffectRegistry). Every knob
// below takes a plain float `amount` -- crossing from the document's exact
// num/den fraction to a float happens exactly once, in
// ColorEffects.cc's EffectParamValueToFloat, before this file ever sees the
// value (see that file's comment, and the description of TimelineViewport
// in docs/reference/decisions-de-temps.md as the analogous time-to-pixel
// boundary).
//
// GradeEntry/ClipGradeParameters mirror ColorEffects.h's
// GpuColorGradeEntry/GpuClipGradeParameters, which PackColorGradeForGpu
// fills from a ResolvedColorGrade, the same way PresentationParameters/
// OutputParameters below mirror their Renderer.mm counterparts: Metal
// Shading Language cannot include a C++ header. Since the wheels and curves
// (F1.3, 18 September 2026) tests/color_grade_shader_tests.mm compiles this
// file, runs applyColorGrade on the GPU with what PackColorGradeForGpu
// packs, and compares it with ColorEffects.cc's ApplyResolvedColorGrade:
// the layout and the arithmetic of every stage below are checked there.
struct GradeEntry {
    int kind;
    float amount;
    // F1.3 -- wheels and curves: their resolved knobs in `data`.
    int dataOffset;
    int dataCount;
};

struct ClipGradeParameters {
    int count;
    // Plain scalars, not int3: unlike the trailing vector padding fields
    // elsewhere in this file (PresentationParameters::padding,
    // SolidParameters::colorPadding), this padding sits *before* more
    // fields. A vector type's larger alignment would shift where `entries`
    // starts relative to ColorEffects.h's plain `int32_t padding[3]`, so
    // both sides use an array of scalars to guarantee identical byte
    // offsets.
    int padding[3];
    // Matches ColorEffects.h's kMaxColorEffectsPerClip.
    GradeEntry entries[16];
    // Matches ColorEffects.h's kMaxColorGradeData.
    float data[768];
};

// ACEScct's log branch is (log2(linearAP1)+9.72)/17.52 (see
// linearAP1ToACEScct above), so adding a constant to an ACEScct-encoded
// channel is exactly a *2^(constant*17.52) multiply in linear light. Several
// knobs below use this to stay in log space rather than round-tripping
// through linearAP1ToACEScct/acesCctToLinearAP1 a second time.
constant float kAcesCctLogSlope = 17.52;
// linearAP1ToACEScct(0.18), i.e. 18% middle grey in ACEScct code value.
constant float kAcesCctMiddleGrey = 0.4135884;
constant float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float3 applyExposure(float3 color, float amount) {
    // amount is in stops. Adding amount/17.52 to the log-encoded code is the
    // log-space equivalent of CIExposureAdjust's inputEV, i.e. linear *=
    // 2^amount (see kAcesCctLogSlope above). The affine low-code branch near
    // black is a close, not exact, match; the deviation stays confined to
    // sub-visible shadow detail.
    return color + amount / kAcesCctLogSlope;
}

float3 applyContrast(float3 color, float amount, float pivot) {
    // Pivots around ACEScct's encoding of 18% middle grey so contrast moves
    // shadows and highlights apart without shifting exposure. amount == 0 is
    // a no-op; +/-1 doubles/removes the slope, mirroring CIColorControls'
    // inputContrast convention (its neutral value 1.0 corresponds to our 0).
    // GR6 -- docs/roadmap/GRADE-RESOLVE-2026-10.md: a stage may carry its
    // own pivot; applyColorGrade hands kAcesCctMiddleGrey for one that does
    // not, which is what this always pivoted about.
    return (color - pivot) * (1.0 + amount) + pivot;
}

float3 applySaturation(float3 color, float amount) {
    // Rec.709 luma weights, mirroring CIColorControls' inputSaturation:
    // amount == 0 is a no-op, -1 fully desaturates, +1 doubles the existing
    // saturation.
    const float luma = dot(color, kLumaWeights);
    return mix(float3(luma), color, 1.0 + amount);
}

float3 applyVibrance(float3 color, float amount) {
    // A selective saturation boost that protects already-saturated pixels
    // (and, by extension, skin tones) more than the flat applySaturation
    // move above -- the same intent as CIVibrance, hand-written because
    // there is no single closed-form matrix for it (unlike exposure/
    // contrast/saturation/temperature/tint above and below, which do).
    // existingSaturation is a cheap proxy: the spread between the brightest
    // and dimmest channel, relative to their average.
    const float luma = dot(color, kLumaWeights);
    const float channelMax = max(color.r, max(color.g, color.b));
    const float channelMin = min(color.r, min(color.g, color.b));
    const float average = (color.r + color.g + color.b) / 3.0;
    const float existingSaturation =
        clamp((channelMax - channelMin) / max(abs(average), 1e-4), 0.0, 1.0);
    const float weight = 1.0 - existingSaturation;
    return mix(color, mix(float3(luma), color, 1.0 + amount), weight);
}

// G7 -- docs/roadmap/GRADE-2026-09.md. The white balance: the Bradford
// adaptation ColorEffects.cc's WhiteBalanceMatrix computed, applied on
// linear light. block[0] is the grading space (ColorGradeSpace): 1 decodes
// ACEScct to linear AP1 and back, 0 takes the display signal to light by
// the 2.4 power and back, 2 and 3 are linear already. block[1..9] is the
// matrix, row-major. ApplyResolvedColorGrade is the same arithmetic.
float3 applyTemperature(float3 color, constant float* block) {
    const int space = int(block[0]);
    float3 light = color;
    if (space == 1) {
        light = acesCctToLinearAP1(light);
    } else if (space == 0) {
        light = sign(light) * pow(abs(light), float3(2.4));
    }
    const float3 adapted =
        float3(block[1] * light.r + block[2] * light.g + block[3] * light.b,
               block[4] * light.r + block[5] * light.g + block[6] * light.b,
               block[7] * light.r + block[8] * light.g + block[9] * light.b);
    if (space == 1) return linearAP1ToACEScct(adapted);
    if (space == 0) return sign(adapted) * pow(abs(adapted), float3(1.0 / 2.4));
    return adapted;
}

float3 applyTint(float3 color, float amount) {
    // Positive amount shifts toward magenta (down green); negative toward
    // green -- the conventional sign of a tint slider paired with the
    // temperature axis above.
    constexpr float kStrength = 0.5;
    color.g -= (amount * kStrength) / kAcesCctLogSlope;
    return color;
}

float3 applyHighlights(float3 color, float amount) {
    // No single built-in composes inline here the way CIColorControls does
    // for exposure/contrast/saturation, so this is a hand-written luminance
    // mask: positive amount recovers (darkens) the brightest code values,
    // negative boosts them. The mask fades in above the midtones so shadows
    // and midtones are untouched.
    const float luma = dot(color, kLumaWeights);
    const float mask = smoothstep(0.4, 0.9, luma);
    return color - amount * mask * 0.5;
}

float3 applyShadows(float3 color, float amount) {
    // Mirror of applyHighlights: positive amount lifts (brightens) the
    // darkest code values, negative crushes them, fading out above the
    // midtones.
    const float luma = dot(color, kLumaWeights);
    const float mask = 1.0 - smoothstep(0.05, 0.5, luma);
    return color + amount * mask * 0.5;
}

// GR6 -- color.hue: every hue turned about the grey axis. block[0..8] is the
// matrix ColorEffects.cc's HueRotationMatrix computed, row-major, applied to
// the value as it is; ApplyResolvedColorGrade is the same arithmetic.
float3 applyHue(float3 color, constant float* block) {
    return float3(block[0] * color.r + block[1] * color.g + block[2] * color.b,
                  block[3] * color.r + block[4] * color.g + block[5] * color.b,
                  block[6] * color.r + block[7] * color.g + block[8] * color.b);
}

// F1.3 -- wheels and curves. The formulas are written once, with their
// reasons, in ColorEffects.h; ColorEffects.cc resolves the knobs into the
// values below and computes the same arithmetic for the export.
//
// Lift L, gain K, then the gamma exponent 1/G, three floats each:
// y = (K - L) * x + L, then y^(1/G) where y > 0.
float3 applyWheels(float3 color, constant float* wheel) {
    const float3 lift = float3(wheel[0], wheel[1], wheel[2]);
    const float3 gain = float3(wheel[3], wheel[4], wheel[5]);
    const float3 power = float3(wheel[6], wheel[7], wheel[8]);
    const float3 graded = color * (gain - lift) + lift;
    return select(graded, pow(max(graded, 0.0), power), graded > 0.0);
}

// One channel's block, [n, x0, y0, m0, x1, ...]: the Fritsch-Carlson
// monotone cubic between points, the end tangent's straight line past them,
// the identity without points (ColorEffects.cc's EvaluateColorCurve).
float evaluateCurve(constant float* block, float x) {
    const int count = int(block[0]);
    if (count < 2) return x;
    constant float* points = block + 1;
    const int last = (count - 1) * 3;
    if (x <= points[0]) return points[1] + points[2] * (x - points[0]);
    if (x >= points[last])
        return points[last + 1] + points[last + 2] * (x - points[last]);
    int interval = 0;
    while (interval + 2 < count && x >= points[(interval + 1) * 3])
        ++interval;
    constant float* start = points + interval * 3;
    const float width = start[3] - start[0];
    const float t = (x - start[0]) / width;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * start[1] +
           (t3 - 2.0 * t2 + t) * width * start[2] +
           (-2.0 * t3 + 3.0 * t2) * start[4] + (t3 - t2) * width * start[5];
}

// Four blocks: master on R, G and B, then r, g and b on their channel.
float3 applyCurves(float3 color, constant float* block) {
    color = float3(evaluateCurve(block, color.r),
                   evaluateCurve(block, color.g),
                   evaluateCurve(block, color.b));
    block += 1 + 3 * int(block[0]);
    color.r = evaluateCurve(block, color.r);
    block += 1 + 3 * int(block[0]);
    color.g = evaluateCurve(block, color.g);
    block += 1 + 3 * int(block[0]);
    color.b = evaluateCurve(block, color.b);
    return color;
}

// GR8 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. color.hue_curves: six
// curves read along the hue, the luma or the saturation of the pixel as it
// enters the stage, then applied together (ColorEffects.h has the table and
// the reasons). Six blocks as a curve channel's, each with a point before
// its first and after its last so that evaluateCurve is asked inside it; an
// empty one is neutral. ColorEffects.cc's ApplyResolvedColorGrade is the
// same arithmetic, step for step.
float hueCurve(constant float* block, float x) {
    return block[0] < 2.0 ? 0.5 : evaluateCurve(block, x);
}

float3 applyHueCurves(float3 color, constant float* block) {
    constexpr float kTurn = 6.2831853;
    constexpr float kChromaBlue = 1.8556;
    constexpr float kChromaRed = 1.5748;
    constexpr float kLumaFade = 0.1;
    const float luma = dot(color, kLumaWeights);
    const float cb = (color.b - luma) / kChromaBlue;
    const float cr = (color.r - luma) / kChromaRed;
    const float chroma = sqrt(cb * cb + cr * cr);
    float hue = chroma > 0.0 ? atan2(cr, cb) / kTurn : 0.0;
    hue -= floor(hue);
    const float saturation = clamp(chroma * 2.0, 0.0, 1.0);
    const float level = clamp(luma, 0.0, 1.0);
    const float hueShift = hueCurve(block, hue) - 0.5;
    block += 1 + 3 * int(block[0]);
    const float satByHue = hueCurve(block, hue) * 2.0;
    block += 1 + 3 * int(block[0]);
    const float lumByHue = hueCurve(block, hue) - 0.5;
    block += 1 + 3 * int(block[0]);
    const float satByLum = hueCurve(block, level) * 2.0;
    block += 1 + 3 * int(block[0]);
    const float satBySat = hueCurve(block, saturation) * 2.0;
    block += 1 + 3 * int(block[0]);
    const float lumBySat = hueCurve(block, saturation) - 0.5;
    const float angle = hueShift * kTurn;
    const float cosine = cos(angle);
    const float sine = sin(angle);
    const float factor = satByHue * satByLum * satBySat;
    const float turnedCb = (cb * cosine - cr * sine) * factor;
    const float turnedCr = (cb * sine + cr * cosine) * factor;
    const float lifted =
        luma + lumByHue * clamp(saturation / kLumaFade, 0.0, 1.0) + lumBySat;
    const float red = lifted + kChromaRed * turnedCr;
    const float blue = lifted + kChromaBlue * turnedCb;
    return float3(red, (lifted - 0.2126 * red - 0.0722 * blue) / 0.7152, blue);
}

// GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. color.resolve_primaries: a
// DaVinci Resolve node's primaries as Resolve 21.1 renders them, the law and
// the block ColorEffects.h describes. Under colour management the value goes
// to DaVinci Intermediate in DaVinci Wide Gamut, where Resolve grades, and
// back. ColorEffects.cc's ApplyResolvePrimaries is the same arithmetic, step
// for step.
float3 encodeDaVinciIntermediate(float3 value) {
    const float3 high = (log2(value + 0.0075) + 7.0) * 0.07329248;
    return select(value * 10.44426855, high, value > 0.00262409);
}

float3 decodeDaVinciIntermediate(float3 value) {
    const float3 high = exp2(value / 0.07329248 - 7.0) - 0.0075;
    return select(value / 10.44426855, high, value > 0.02740668);
}

float signedPower(float value, float exponent) {
    return value < 0.0 ? -pow(-value, exponent) : pow(value, exponent);
}

// GR16 -- the contrast of a Resolve node (ResolveContrastTable.h): its curve
// is 65 values on [0, end], read by Catmull-Rom (the neighbour missing at an
// end on the line through the last two), along the end slopes past the
// ends; ResolveContrast.cc's ApplyResolveContrastCurve is the same.
float resolveContrastCurve(constant float* samples, float end, float u) {
    const float step = end / 64.0;
    if (u <= 0.0) return samples[0] + (u / step) * (samples[1] - samples[0]);
    if (u >= end)
        return samples[64] + ((u - end) / step) * (samples[64] - samples[63]);
    const float f = u / step;
    const int i = min(int(f), 63);
    const float t = f - float(i);
    const float p1 = samples[i];
    const float p2 = samples[i + 1];
    const float p0 = i > 0 ? samples[i - 1] : 2.0 * p1 - p2;
    const float p3 = i + 2 <= 64 ? samples[i + 2] : 2.0 * p2 - p1;
    return 0.5 * (2.0 * p1 + (p2 - p0) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                  (3.0 * p1 - p0 - 3.0 * p2 + p3) * t * t * t);
}

// The HLG code of a node's value for the aware contrast, and back: linear
// light divided by 12, BT.2100 HLG, a negative value keeping its sign.
float resolveToHlg(float value) {
    const float light = decodeDaVinciIntermediate(float3(value)).r / 12.0;
    const float magnitude = abs(light);
    const float code = magnitude <= 1.0 / 12.0
                           ? sqrt(3.0 * magnitude)
                           : 0.17883277 * log(12.0 * magnitude - 0.28466892) +
                                 0.55991073;
    return light < 0.0 ? -code : code;
}

float resolveFromHlg(float code) {
    const float magnitude = abs(code);
    const float light =
        magnitude <= 0.5
            ? magnitude * magnitude / 3.0
            : (exp((magnitude - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
    return encodeDaVinciIntermediate(float3((code < 0.0 ? -light : light) *
                                            12.0)).r;
}

float3 applyResolvePrimaries(float3 color, constant float* block, int count) {
    const bool contrasted = count >= 104;
    const int space = int(block[0]);
    const bool managed = space != 0;
    float3 node = color;
    if (managed) {
        float3 light = space == 1 ? acesCctToLinearAP1(color) : color;
        node = encodeDaVinciIntermediate(
            float3(block[1] * light.r + block[2] * light.g + block[3] * light.b,
                   block[4] * light.r + block[5] * light.g + block[6] * light.b,
                   block[7] * light.r + block[8] * light.g +
                       block[9] * light.b));
    }
    if (count == 46 || count == 113) {
        // The temperature and tint, last in the block, first on the node's
        // values.
        constant float* m = block + count - 9;
        node = float3(m[0] * node.r + m[1] * node.g + m[2] * node.b,
                      m[3] * node.r + m[4] * node.g + m[5] * node.b,
                      m[6] * node.r + m[7] * node.g + m[8] * node.b);
    }
    float3 out;
    for (int c = 0; c < 3; ++c) {
        out[c] = signedPower(block[19 + c] + block[22 + c] * node[c] +
                                 block[25 + c],
                             block[28 + c]);
    }
    const float target = signedPower(
        block[31] + block[32] * dot(node, kLumaWeights) + block[34], block[33]);
    const float lumMix = block[35];
    const int mode = contrasted ? int(block[37]) : 0;
    constant float* curve = block + 39;
    const float end = contrasted ? block[38] : 1.0;
    if (mode == 1) {
        // Without the aware tools: on each channel, then the luma taken back
        // toward the contrast of the masters' luma.
        out = float3(resolveContrastCurve(curve, end, out.r),
                     resolveContrastCurve(curve, end, out.g),
                     resolveContrastCurve(curve, end, out.b));
        out += lumMix * (resolveContrastCurve(curve, end, target) -
                         dot(out, kLumaWeights));
    } else {
        out += lumMix * (target - dot(out, kLumaWeights));
    }
    if (mode == 2) {
        // With them: after the masters' luma, on HLG codes, with its own.
        float3 code = float3(resolveToHlg(out.r), resolveToHlg(out.g),
                             resolveToHlg(out.b));
        const float aim =
            resolveContrastCurve(curve, end, dot(code, kLumaWeights));
        code = float3(resolveContrastCurve(curve, end, code.r),
                      resolveContrastCurve(curve, end, code.g),
                      resolveContrastCurve(curve, end, code.b));
        code += lumMix * (aim - dot(code, kLumaWeights));
        out = float3(resolveFromHlg(code.r), resolveFromHlg(code.g),
                     resolveFromHlg(code.b));
    }
    if (block[36] != 1.0) {
        const float luma = dot(out, float3(0.21, 0.70, 0.09));
        out = luma + block[36] * (out - luma);
    }
    if (!managed) return out;
    const float3 wide = decodeDaVinciIntermediate(out);
    const float3 back =
        float3(block[10] * wide.r + block[11] * wide.g + block[12] * wide.b,
               block[13] * wide.r + block[14] * wide.g + block[15] * wide.b,
               block[16] * wide.r + block[17] * wide.g + block[18] * wide.b);
    return space == 1 ? linearAP1ToACEScct(back) : back;
}

// Applies one clip's resolved grade stack in order. Entries whose kind isn't
// one of the thirteen above are left untouched -- exactly the registry's
// "unregistered color.* type is a no-op" rule (ColorEffects.h), so a stack
// containing a future LUT entry still renders every stage this build
// understands instead of failing to render the clip at all.
float3 applyColorGrade(float3 color, constant ClipGradeParameters& grade) {
    for (int i = 0; i < grade.count; ++i) {
        const float amount = grade.entries[i].amount;
        switch (grade.entries[i].kind) {
            case 0:
                color = applyExposure(color, amount);
                break;
            case 1:
                color = applyContrast(
                    color, amount,
                    grade.entries[i].dataCount > 0
                        ? grade.data[grade.entries[i].dataOffset]
                        : kAcesCctMiddleGrey);
                break;
            case 2:
                color = applySaturation(color, amount);
                break;
            case 3:
                color = applyVibrance(color, amount);
                break;
            case 4:
                color = applyTemperature(
                    color, grade.data + grade.entries[i].dataOffset);
                break;
            case 5:
                color = applyTint(color, amount);
                break;
            case 6:
                color = applyHighlights(color, amount);
                break;
            case 7:
                color = applyShadows(color, amount);
                break;
            case 8:
                color = applyWheels(color,
                                    grade.data + grade.entries[i].dataOffset);
                break;
            case 9:
                color = applyCurves(color,
                                    grade.data + grade.entries[i].dataOffset);
                break;
            case 10:
                color = applyHue(color,
                                 grade.data + grade.entries[i].dataOffset);
                break;
            case 11:
                color = applyHueCurves(
                    color, grade.data + grade.entries[i].dataOffset);
                break;
            case 12:
                color = applyResolvePrimaries(
                    color, grade.data + grade.entries[i].dataOffset,
                    grade.entries[i].dataCount);
                break;
            default:
                break;
        }
    }
    return color;
}

bool presentationUV(float2 outputUV, float left, float top, float width,
                    float height, int quarterTurns, float2 sourceOrigin,
                    float2 sourceSize, thread float2& codedUV) {
    if (outputUV.x < left || outputUV.x > left + width || outputUV.y < top ||
        outputUV.y > top + height)
        return false;
    // R1 -- the quad covers the rectangle the framing put on screen, and
    // the picture is sampled out of the region the framing named: the same
    // crop-then-place the export writes as `crop`, `scale` and `overlay`.
    // The rotation comes after, so the region is expressed in the picture
    // as displayed, exactly as ClipFraming.h resolves it.
    const float2 displayUV =
        sourceOrigin +
        ((outputUV - float2(left, top)) / float2(width, height)) * sourceSize;
    switch (quarterTurns) {
        case 1:
            codedUV = float2(1.0 - displayUV.y, displayUV.x);
            break;
        case 2:
            codedUV = float2(1.0 - displayUV.x, 1.0 - displayUV.y);
            break;
        case 3:
            codedUV = float2(displayUV.y, 1.0 - displayUV.x);
            break;
        default:
            codedUV = displayUV;
            break;
    }
    return true;
}

// The texture coordinates of a 3D LUT of `edge` samples per side: the
// input clamped to the cube's 0..1 domain, then the texel centres. With a
// linear sampler this is trilinear interpolation on the cube's grid, which
// is what FFmpeg's `lut3d=interp=trilinear` computes on the same file.
float3 lutCoordinates(float3 value, float edge) {
    return (clamp(value, 0.0, 1.0) * (edge - 1.0) + 0.5) / edge;
}

float3 ocioLutCoordinates(float3 value) {
    return lutCoordinates(value, 65.0);
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. A layer's picture as decoded, in
// its own signal: RGB as stored, or YCbCr through its matrix and range,
// unclamped. What fragment_working grades, and what a blurred layer is
// blurred in before it is (fragment_decode_layer).
float3 layerSignal(float2 codedUV, texture2d<float> yTexture,
                   texture2d<float> uTexture, texture2d<float> vTexture,
                   sampler planeSampler,
                   constant PresentationParameters& parameters) {
    if (parameters.inputIsRgb != 0) {
        return float3(yTexture.sample(planeSampler, codedUV).r,
                      uTexture.sample(planeSampler, codedUV).r,
                      vTexture.sample(planeSampler, codedUV).r) *
               parameters.sampleScale;
    }
    return sampleYUV(codedUV, yTexture, uTexture, vTexture, planeSampler,
                     parameters.redFromCr, parameters.greenFromCb,
                     parameters.greenFromCr, parameters.blueFromCb,
                     parameters.sampleScale, parameters.yOffset,
                     parameters.yScale, parameters.chromaOffset,
                     parameters.chromaScale);
}

// EF3 -- a layer's signal into the working composite: the input transform
// and the grade, as fragment_working has always applied them, written once
// so that a blurred layer (fragment_working_blurred) is graded alike.
float3 workingColor(float3 color, texture3d<float> ocioInputLut,
                    sampler planeSampler,
                    constant PresentationParameters& parameters,
                    constant ClipGradeParameters& grade) {
    if (parameters.colorManagementEnabled != 0) {
        // COLOR-2026-08 -- OCIO samples are stored with red, green, then blue
        // as the texture x/y/z coordinates. The LUT output is the selected
        // linear working space (ACEScg for the ACEScct grading path).
        color =
            ocioInputLut.sample(planeSampler, ocioLutCoordinates(color)).rgb;
        if (parameters.useAcescct != 0) {
            // Creative operations belong between these two calls. Composite
            // storage remains scene-linear AP1 so alpha blending is correct.
            color = linearAP1ToACEScct(color);
            color = applyColorGrade(color, grade);
            color = acesCctToLinearAP1(color);
        } else {
            // Color-managed but not working in ACEScct: grade the
            // scene-linear AP1 signal directly. Every knob above still
            // composes (mix/dot/log2 are meaningful on any float3), just
            // without the log-domain calibration the ACEScct branch gets.
            color = applyColorGrade(color, grade);
        }
    } else {
        // No color management: grade the clamped display-encoded signal
        // directly, same as a non-ACES editor would. Composing here too
        // (rather than only in the ACEScct branch) keeps grading usable
        // with ColorManagementSettings::enabled at its default of false.
        color = clamp(applyColorGrade(color, grade), 0.0, 1.0);
    }
    return color;
}

fragment float4 fragment_working(VertexOut in [[stage_in]],
                                 texture2d<float> yTexture [[texture(0)]],
                                 texture2d<float> uTexture [[texture(1)]],
                                 texture2d<float> vTexture [[texture(2)]],
                                 texture2d<float> alphaTexture [[texture(3)]],
                                 texture3d<float> ocioInputLut [[texture(4)]],
                                 sampler planeSampler [[sampler(0)]],
                                 constant PresentationParameters& parameters
                                 [[buffer(0)]],
                                 constant ClipGradeParameters& grade
                                 [[buffer(1)]]) {
    float2 codedUV;
    if (!presentationUV(in.uv, parameters.left, parameters.top,
                        parameters.width, parameters.height,
                        parameters.quarterTurns,
                        float2(parameters.sourceLeft, parameters.sourceTop),
                        float2(parameters.sourceWidth, parameters.sourceHeight),
                        codedUV))
        return float4(0.0);
    const float3 color = workingColor(
        layerSignal(codedUV, yTexture, uTexture, vTexture, planeSampler,
                    parameters),
        ocioInputLut, planeSampler, parameters, grade);
    const float sourceAlpha =
        parameters.hasSourceAlpha != 0
            ? alphaTexture.sample(planeSampler, codedUV).r *
                  parameters.sampleScale
            : 1.0;
    return float4(color,
                  clamp(sourceAlpha * parameters.opacity *
                            clipMaskCoverage(in.uv, parameters),
                        0.0, 1.0));
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred layer, first pass: its
// picture, cropped and turned as the framing says, drawn alone into a
// texture the size of the rectangle it covers on the monitor (the whole
// texture is that rectangle: left 0, top 0, width and height 1). Its own
// signal and its own alpha, ungraded and opaque: the blur comes next
// (Renderer.mm, Metal Performance Shaders), then fragment_working_blurred
// grades it and lays it with the layer's opacity, as the export blurs the
// picture before its grade (ClipBlur.h).
fragment float4 fragment_decode_layer(
    VertexOut in [[stage_in]], texture2d<float> yTexture [[texture(0)]],
    texture2d<float> uTexture [[texture(1)]],
    texture2d<float> vTexture [[texture(2)]],
    texture2d<float> alphaTexture [[texture(3)]],
    sampler planeSampler [[sampler(0)]],
    constant PresentationParameters& parameters [[buffer(0)]]) {
    float2 codedUV;
    if (!presentationUV(in.uv, parameters.left, parameters.top,
                        parameters.width, parameters.height,
                        parameters.quarterTurns,
                        float2(parameters.sourceLeft, parameters.sourceTop),
                        float2(parameters.sourceWidth, parameters.sourceHeight),
                        codedUV))
        return float4(0.0);
    const float sourceAlpha =
        parameters.hasSourceAlpha != 0
            ? alphaTexture.sample(planeSampler, codedUV).r *
                  parameters.sampleScale
            : 1.0;
    return float4(layerSignal(codedUV, yTexture, uTexture, vTexture,
                              planeSampler, parameters),
                  sourceAlpha);
}

// EF3 -- where a blurred layer's texture lies on the monitor: the
// rectangle the layer covers, sampled across it. The crop and the turn are
// already in the texture.
bool blurredLayerUV(float2 outputUV, constant PresentationParameters& p,
                    thread float2& uv) {
    if (outputUV.x < p.left || outputUV.x > p.left + p.width ||
        outputUV.y < p.top || outputUV.y > p.top + p.height)
        return false;
    uv = (outputUV - float2(p.left, p.top)) / float2(p.width, p.height);
    return true;
}

// EF3 -- a blurred layer, last pass: the blurred signal graded into the
// working composite, as fragment_working grades a sharp one.
fragment float4 fragment_working_blurred(
    VertexOut in [[stage_in]], texture2d<float> blurred [[texture(0)]],
    texture3d<float> ocioInputLut [[texture(4)]],
    sampler planeSampler [[sampler(0)]],
    constant PresentationParameters& parameters [[buffer(0)]],
    constant ClipGradeParameters& grade [[buffer(1)]]) {
    float2 uv;
    if (!blurredLayerUV(in.uv, parameters, uv)) return float4(0.0);
    const float4 signal = blurred.sample(planeSampler, uv);
    return float4(
        workingColor(signal.rgb, ocioInputLut, planeSampler, parameters, grade),
        clamp(signal.a * parameters.opacity *
                  clipMaskCoverage(in.uv, parameters),
              0.0, 1.0));
}

struct OutputParameters {
    int colorManagementEnabled;
    int outputGamut;
    int outputTransfer;
    int scopeMode;
    uint drawableWidth;
    uint drawableHeight;
    uint scopeSampleStep;
    uint useAcescct;
    // R6 -- docs/roadmap/REEL-2026-09.md. Samples per side of the
    // sequence's look LUT, or zero for a timeline without one -- which is
    // every timeline that had none before this ticket, drawn byte for byte
    // as it was. The size travels because an imported .cube is any size
    // between 2 and 65, unlike the project's own OCIO cubes.
    uint lookLutEdge;
    // R6 -- non-zero when the look expects the delivered display signal
    // (LookLutStage::DeliveredDisplay): it is then laid after the display
    // transform, where a LUT declared "rec709" was written to act.
    uint lookAfterDisplay;
    // PRD37 -- ROADMAP.md. The scope panel in drawable pixels, as
    // VideoScopes.h lays it out (LayoutVideoScopePanel): above the controls
    // along the monitor's bottom edge. Drawn here, decided there.
    float scopeLeft;
    float scopeTop;
    float scopeWidth;
    float scopeHeight;
};

// Visual QA of 19 September 2026 -- the signal is bounded to what a
// delivery holds, managed or not. The display cube returns up to 1.06 for a
// bright input, which the exported file clips at 1.0: unbounded, the monitor
// and its scopes showed above white what no file carries
// (cutmachine_monitor_export_parity_tests).
float3 displaySignal(float3 working, constant OutputParameters& parameters,
                     texture3d<float> ocioOutputLut, sampler textureSampler) {
    if (parameters.colorManagementEnabled == 0) return clamp(working, 0.0, 1.0);
    const float3 lutInput =
        parameters.useAcescct != 0 ? linearAP1ToACEScct(working) : working;
    return clamp(
        ocioOutputLut.sample(textureSampler, ocioLutCoordinates(lutInput)).rgb,
        0.0, 1.0);
}

// R6 -- docs/roadmap/REEL-2026-09.md. The same signal with the sequence's
// look laid on it, at the one stage LookLut.h names and the export's graph
// uses: under colour management, on the ACEScct code values, between the
// encode and the display cube; without it, on the delivered signal itself.
//
// Kept beside displaySignal rather than inside it because the motion path
// asks displaySignal for a round trip through the SDR *view*
// (compositeDisplayReferred), which is not the delivery and must not carry
// the look -- it would then be applied twice, and at a stage that is not
// the film's.
float3 displaySignalWithLook(float3 working,
                             constant OutputParameters& parameters,
                             texture3d<float> ocioOutputLut,
                             texture3d<float> lookLut,
                             sampler textureSampler) {
    if (parameters.lookLutEdge == 0)
        return displaySignal(working, parameters, ocioOutputLut,
                             textureSampler);
    const float edge = float(parameters.lookLutEdge);
    if (parameters.colorManagementEnabled == 0 ||
        parameters.lookAfterDisplay != 0) {
        // Without colour management the composite already is the delivered
        // signal; with it, the look waits for the display transform.
        const float3 delivered =
            parameters.colorManagementEnabled == 0
                ? working
                : displaySignal(working, parameters, ocioOutputLut,
                                textureSampler);
        return clamp(lookLut.sample(textureSampler,
                                    lutCoordinates(delivered, edge))
                         .rgb,
                     0.0, 1.0);
    }
    // MEASURED, NOT DEDUCED (cutmachine_monitor_export_parity_tests). The
    // scene-linear value is clamped at zero BEFORE the log encode, because
    // that is where the export clamps: FFmpeg's lut1d divides its input by
    // DOMAIN_MAX and never subtracts DOMAIN_MIN, so the shaper's domain
    // starts at linear zero and every negative value reads as ACEScct
    // 0.0729 (ColorManagement.cc's ShaperText). Encoding first instead
    // reaches ACEScct 0 to 0.0729, a range the export cannot produce.
    // Without a look that gap is invisible -- the display transform renders
    // all of it as black -- and with one it is not: this test measured the
    // darkest patch 0.031 apart in blue, a lifted black on the file and a
    // crushed one on the monitor. displaySignal is left alone, so a project
    // without a look is drawn exactly as it was.
    const float3 lutInput = parameters.useAcescct != 0
                                ? linearAP1ToACEScct(max(working, 0.0))
                                : working;
    const float3 looked =
        lookLut.sample(textureSampler, lutCoordinates(lutInput, edge)).rgb;
    return clamp(
        ocioOutputLut.sample(textureSampler, ocioLutCoordinates(looked)).rgb,
        0.0, 1.0);
}

constant uint kScopeWaveformWidth = 256;
constant uint kScopeWaveformHeight = 128;
constant uint kScopeParadeWidth = 128;
constant uint kScopeParadeHeight = 128;
constant uint kScopeVectorSize = 256;

kernel void compute_video_scope(texture2d<float, access::read> workingTexture
                                [[texture(0)]],
                                texture3d<float> ocioOutputLut [[texture(1)]],
                                texture3d<float> lookLut [[texture(2)]],
                                sampler textureSampler [[sampler(0)]],
                                device atomic_uint* histogram [[buffer(0)]],
                                constant OutputParameters& parameters
                                [[buffer(1)]],
                                uint2 gid [[thread_position_in_grid]]) {
    const uint2 source = gid * parameters.scopeSampleStep;
    if (source.x >= parameters.drawableWidth ||
        source.y >= parameters.drawableHeight || parameters.scopeMode == 0)
        return;
    // R6 -- the scopes measure what is delivered, look included: a
    // waveform that ignored the sequence's LUT would answer a question
    // nobody asked.
    const float3 signal = displaySignalWithLook(
        workingTexture.read(source).rgb, parameters, ocioOutputLut, lookLut,
        textureSampler);
    if (parameters.scopeMode == 1) {
        const float3 weights = parameters.outputGamut == 1
                                   ? float3(0.2627, 0.6780, 0.0593)
                                   : float3(0.2126, 0.7152, 0.0722);
        const uint x =
            min(kScopeWaveformWidth - 1, source.x * kScopeWaveformWidth /
                                             max(parameters.drawableWidth, 1u));
        const uint y = min(kScopeWaveformHeight - 1,
                           uint(clamp(dot(signal, weights), 0.0, 1.0) *
                                float(kScopeWaveformHeight - 1)));
        atomic_fetch_add_explicit(&histogram[y * kScopeWaveformWidth + x], 1u,
                                  memory_order_relaxed);
    } else if (parameters.scopeMode == 5) {
        // The colourised waveform: each channel binned over the full width,
        // one plane per channel, laid out as the parade's.
        const uint x =
            min(kScopeWaveformWidth - 1, source.x * kScopeWaveformWidth /
                                             max(parameters.drawableWidth, 1u));
        for (uint channel = 0; channel < 3; ++channel) {
            const uint y = min(kScopeWaveformHeight - 1,
                               uint(clamp(signal[channel], 0.0, 1.0) *
                                    float(kScopeWaveformHeight - 1)));
            const uint index =
                channel * kScopeWaveformWidth * kScopeWaveformHeight +
                y * kScopeWaveformWidth + x;
            atomic_fetch_add_explicit(&histogram[index], 1u,
                                      memory_order_relaxed);
        }
    } else if (parameters.scopeMode == 2) {
        const uint x =
            min(kScopeParadeWidth - 1, source.x * kScopeParadeWidth /
                                           max(parameters.drawableWidth, 1u));
        for (uint channel = 0; channel < 3; ++channel) {
            const uint y = min(kScopeParadeHeight - 1,
                               uint(clamp(signal[channel], 0.0, 1.0) *
                                    float(kScopeParadeHeight - 1)));
            const uint index =
                channel * kScopeParadeWidth * kScopeParadeHeight +
                y * kScopeParadeWidth + x;
            atomic_fetch_add_explicit(&histogram[index], 1u,
                                      memory_order_relaxed);
        }
    } else if (parameters.scopeMode == 3) {
        const float3 weights = parameters.outputGamut == 1
                                   ? float3(0.2627, 0.6780, 0.0593)
                                   : float3(0.2126, 0.7152, 0.0722);
        const float y = dot(signal, weights);
        const float cb = (signal.b - y) / (2.0 * (1.0 - weights.b));
        const float cr = (signal.r - y) / (2.0 * (1.0 - weights.r));
        const uint xBin =
            min(kScopeVectorSize - 1,
                uint(clamp(cb + 0.5, 0.0, 1.0) * float(kScopeVectorSize - 1)));
        const uint yBin =
            min(kScopeVectorSize - 1,
                uint(clamp(cr + 0.5, 0.0, 1.0) * float(kScopeVectorSize - 1)));
        atomic_fetch_add_explicit(&histogram[yBin * kScopeVectorSize + xBin],
                                  1u, memory_order_relaxed);
    }
}

// DEC42 -- the curve editor measures the visible sequence frame's display
// signal, before diagnostic overlays, in 256 bins per RGB channel. The
// sample step matches the scopes and only these 3 KiB reach the CPU.
kernel void compute_curve_histogram(
    texture2d<float, access::read> workingTexture [[texture(0)]],
    texture3d<float> ocioOutputLut [[texture(1)]],
    texture3d<float> lookLut [[texture(2)]],
    sampler textureSampler [[sampler(0)]],
    device atomic_uint* histogram [[buffer(0)]],
    constant OutputParameters& parameters [[buffer(1)]],
    constant float4& rectangle [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint2 source = gid * 2;
    const float2 centre = float2(source) + 0.5;
    if (source.x >= parameters.drawableWidth ||
        source.y >= parameters.drawableHeight || centre.x < rectangle.x ||
        centre.y < rectangle.y || centre.x >= rectangle.z ||
        centre.y >= rectangle.w)
        return;
    const float3 signal = displaySignalWithLook(
        workingTexture.read(source).rgb, parameters, ocioOutputLut, lookLut,
        textureSampler);
    for (uint channel = 0; channel < 3; ++channel) {
        const uint bin = min(255u, uint(clamp(signal[channel], 0.0, 1.0) * 255));
        atomic_fetch_add_explicit(&histogram[channel * 256 + bin], 1u,
                                  memory_order_relaxed);
    }
}

// G5 -- docs/roadmap/GRADE-2026-09.md. What the graticule marks and what
// false colour paints, computed once on the CPU (VideoScopes.h) so the marks
// say what measure_color says. Floats only, so the C++ twin in Renderer.mm
// lays out the same.
struct ScopeGuideParameters {
    float middleGrey;
    float greyLow;
    float greyHigh;
    float skinLow;
    float skinHigh;
    float nearBlack;
    float nearWhite;
    float endCode;
    float skinCos;
    float skinSin;
    float targets[12];      // Cb, Cr of the six 75 % bars.
    float falseColors[18];  // Crushed ... clipped, RGB each.
};

// Distance from `point` to the segment from `start` to `end`.
float segmentDistance(float2 point, float2 start, float2 end) {
    const float2 along = end - start;
    const float t =
        clamp(dot(point - start, along) / max(dot(along, along), 1e-8), 0.0,
              1.0);
    return length(point - (start + t * along));
}

float scopeGrid(float2 uv, int mode, constant ScopeGuideParameters& guides) {
    if (mode == 3) {
        const float radius = length((uv - 0.5) * 2.0);
        const float circle = 1.0 - smoothstep(0.008, 0.018, abs(radius - 0.75));
        const float axes = max(1.0 - smoothstep(0.002, 0.006, abs(uv.x - 0.5)),
                               1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.5)));
        // G5 -- a box around each 75 % bar, where the histogram bins it
        // (Cb right, Cr up), and the skin tone line from the centre.
        float targets = 0.0;
        for (int index = 0; index < 6; ++index) {
            const float2 target =
                float2(guides.targets[index * 2] + 0.5,
                       0.5 - guides.targets[index * 2 + 1]);
            const float2 offset = abs(uv - target);
            const float edge = max(offset.x, offset.y);
            targets = max(targets,
                          1.0 - smoothstep(0.002, 0.006, abs(edge - 0.02)));
        }
        const float skin =
            1.0 - smoothstep(0.002, 0.006,
                             segmentDistance(uv, float2(0.5),
                                             float2(0.5) + 0.5 *
                                                 float2(guides.skinCos,
                                                        -guides.skinSin)));
        return max(max(circle, axes) * 0.18, max(targets, skin) * 0.42);
    }
    const float horizontal =
        max(1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.25)),
            max(1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.5)),
                1.0 - smoothstep(0.002, 0.006, abs(uv.y - 0.75))));
    // G5 -- middle grey, dashed so it reads apart from the quarter lines.
    const float dash = step(0.5, fract(uv.x * 48.0));
    const float grey =
        (1.0 - smoothstep(0.002, 0.006, abs(uv.y - (1.0 - guides.middleGrey)))) *
        dash;
    return max(horizontal * 0.16, grey * 0.42);
}

// G5 -- the ARRI convention on the delivered signal: crushed and clipped by
// a channel at its end code, as measure_color counts them; then the display
// luma's bands; everything else in grey by its luma, dimmed.
float3 falseColor(float3 signal, float3 weights,
                  constant ScopeGuideParameters& guides) {
    const float top = max(signal.r, max(signal.g, signal.b));
    const float luma = dot(signal, weights);
    int band = -1;
    if (top <= guides.endCode) {
        band = 0;
    } else if (top >= 1.0 - guides.endCode) {
        band = 5;
    } else if (luma <= guides.nearBlack) {
        band = 1;
    } else if (luma >= guides.greyLow && luma <= guides.greyHigh) {
        band = 2;
    } else if (luma >= guides.skinLow && luma <= guides.skinHigh) {
        band = 3;
    } else if (luma >= guides.nearWhite) {
        band = 4;
    }
    if (band < 0) return float3(luma * 0.8);
    return float3(guides.falseColors[band * 3], guides.falseColors[band * 3 + 1],
                  guides.falseColors[band * 3 + 2]);
}

float4 scopeOverlay(float4 base, float2 pixel, device atomic_uint* histogram,
                    constant OutputParameters& parameters,
                    constant ScopeGuideParameters& guides) {
    if (parameters.scopeMode == 0 || parameters.scopeMode == 4) return base;
    const float panelWidth = parameters.scopeWidth;
    const float panelHeight = parameters.scopeHeight;
    if (panelWidth <= 0.0 || panelHeight <= 0.0) return base;
    const float2 origin = float2(parameters.scopeLeft, parameters.scopeTop);
    if (pixel.x < origin.x || pixel.y < origin.y ||
        pixel.x >= origin.x + panelWidth || pixel.y >= origin.y + panelHeight)
        return base;
    const float2 uv = (pixel - origin) / float2(panelWidth, panelHeight);
    float3 trace = float3(0.0);
    if (parameters.scopeMode == 1) {
        const uint x = min(kScopeWaveformWidth - 1,
                           uint(uv.x * float(kScopeWaveformWidth)));
        const uint y =
            min(kScopeWaveformHeight - 1,
                uint((1.0 - uv.y) * float(kScopeWaveformHeight - 1)));
        const uint count = atomic_load_explicit(
            &histogram[y * kScopeWaveformWidth + x], memory_order_relaxed);
        trace = float3(1.0 - exp(-float(count) * 0.18));
    } else if (parameters.scopeMode == 5) {
        const uint x = min(kScopeWaveformWidth - 1,
                           uint(uv.x * float(kScopeWaveformWidth)));
        const uint y =
            min(kScopeWaveformHeight - 1,
                uint((1.0 - uv.y) * float(kScopeWaveformHeight - 1)));
        for (uint channel = 0; channel < 3; ++channel) {
            const uint index =
                channel * kScopeWaveformWidth * kScopeWaveformHeight +
                y * kScopeWaveformWidth + x;
            trace[channel] =
                1.0 - exp(-float(atomic_load_explicit(&histogram[index],
                                                      memory_order_relaxed)) *
                          0.18);
        }
    } else if (parameters.scopeMode == 2) {
        const uint channel = min(2u, uint(uv.x * 3.0));
        const float localX = fract(uv.x * 3.0);
        const uint x =
            min(kScopeParadeWidth - 1, uint(localX * float(kScopeParadeWidth)));
        const uint y = min(kScopeParadeHeight - 1,
                           uint((1.0 - uv.y) * float(kScopeParadeHeight - 1)));
        const uint index = channel * kScopeParadeWidth * kScopeParadeHeight +
                           y * kScopeParadeWidth + x;
        const float intensity =
            1.0 - exp(-float(atomic_load_explicit(&histogram[index],
                                                  memory_order_relaxed)) *
                      0.18);
        trace[channel] = intensity;
    } else {
        const uint x =
            min(kScopeVectorSize - 1, uint(uv.x * float(kScopeVectorSize)));
        const uint y = min(kScopeVectorSize - 1,
                           uint((1.0 - uv.y) * float(kScopeVectorSize - 1)));
        const uint count = atomic_load_explicit(
            &histogram[y * kScopeVectorSize + x], memory_order_relaxed);
        trace = float3(0.65, 1.0, 0.78) * (1.0 - exp(-float(count) * 0.12));
    }
    const float grid = scopeGrid(uv, parameters.scopeMode, guides);
    const float3 panel = max(trace, float3(grid));
    return float4(mix(base.rgb, panel, 0.88), base.a);
}

// Visual QA of 19 September 2026 -- the rule of ColorManagement.h's
// OpenColorIoCompositeDisplayReferred: a display-referred layer is mixed in
// the SDR display view, where its colours were authored, not in the working
// space where a translucent white stays white. `below` goes out through
// `viewLut` (the SDR view, read like fragment_output reads its LUT), the
// layer is mixed there by `coverage`, and the mix comes back through
// `motionLut`, the inverse of that view.
float3 compositeDisplayReferred(float3 below, float3 layer, float coverage,
                                constant OutputParameters& view,
                                texture3d<float> viewLut,
                                texture3d<float> motionLut,
                                sampler lutSampler) {
    const float3 shown = displaySignal(below, view, viewLut, lutSampler);
    const float3 mixed = mix(shown, layer, coverage);
    return motionLut.sample(lutSampler, ocioLutCoordinates(mixed)).rgb;
}

// A motion frame on a colour-managed monitor. It reads the composite under
// it (framebuffer fetch, Apple GPUs) and replaces it, blending disabled;
// where the frame covers nothing the composite is returned untouched.
fragment float4 fragment_working_display_referred(
    VertexOut in [[stage_in]], float4 below [[color(0)]],
    texture2d<float> redTexture [[texture(0)]],
    texture2d<float> greenTexture [[texture(1)]],
    texture2d<float> blueTexture [[texture(2)]],
    texture2d<float> alphaTexture [[texture(3)]],
    texture3d<float> motionLut [[texture(4)]],
    texture3d<float> viewLut [[texture(5)]],
    sampler planeSampler [[sampler(0)]],
    constant PresentationParameters& parameters [[buffer(0)]],
    constant OutputParameters& view [[buffer(2)]]) {
    float2 codedUV;
    if (!presentationUV(in.uv, parameters.left, parameters.top,
                        parameters.width, parameters.height,
                        parameters.quarterTurns,
                        float2(parameters.sourceLeft, parameters.sourceTop),
                        float2(parameters.sourceWidth, parameters.sourceHeight),
                        codedUV))
        return below;
    const float sourceAlpha =
        parameters.hasSourceAlpha != 0
            ? alphaTexture.sample(planeSampler, codedUV).r *
                  parameters.sampleScale
            : 1.0;
    const float coverage = clamp(sourceAlpha * parameters.opacity *
                                     clipMaskCoverage(in.uv, parameters),
                                 0.0, 1.0);
    if (coverage <= 0.0) return below;
    // DEC31, BH2 -- a media declared a graphic may be YCbCr (a ProRes 4444
    // logo); its code values become display RGB as fragment_working reads
    // a camera's, before they are mixed. A motion frame is planar RGB.
    const float3 layer =
        parameters.inputIsRgb != 0
            ? float3(redTexture.sample(planeSampler, codedUV).r,
                     greenTexture.sample(planeSampler, codedUV).r,
                     blueTexture.sample(planeSampler, codedUV).r) *
                  parameters.sampleScale
            : clamp(sampleYUV(codedUV, redTexture, greenTexture, blueTexture,
                              planeSampler, parameters.redFromCr,
                              parameters.greenFromCb, parameters.greenFromCr,
                              parameters.blueFromCb, parameters.sampleScale,
                              parameters.yOffset, parameters.yScale,
                              parameters.chromaOffset, parameters.chromaScale),
                    0.0, 1.0);
    return float4(compositeDisplayReferred(below.rgb, layer, coverage, view,
                                           viewLut, motionLut, planeSampler),
                  below.a);
}

// EF3 -- a blurred layer that is mixed in the graphics view (a media
// declared a graphic, under colour management), last pass: the blurred
// signal mixed as fragment_working_display_referred mixes a sharp one.
fragment float4 fragment_blurred_display_referred(
    VertexOut in [[stage_in]], float4 below [[color(0)]],
    texture2d<float> blurred [[texture(0)]],
    texture3d<float> motionLut [[texture(4)]],
    texture3d<float> viewLut [[texture(5)]],
    sampler planeSampler [[sampler(0)]],
    constant PresentationParameters& parameters [[buffer(0)]],
    constant OutputParameters& view [[buffer(2)]]) {
    float2 uv;
    if (!blurredLayerUV(in.uv, parameters, uv)) return below;
    const float4 signal = blurred.sample(planeSampler, uv);
    const float coverage = clamp(signal.a * parameters.opacity *
                                     clipMaskCoverage(in.uv, parameters),
                                 0.0, 1.0);
    if (coverage <= 0.0) return below;
    const float3 layer = parameters.inputIsRgb != 0
                             ? signal.rgb
                             : clamp(signal.rgb, 0.0, 1.0);
    return float4(compositeDisplayReferred(below.rgb, layer, coverage, view,
                                           viewLut, motionLut, planeSampler),
                  below.a);
}

// G6 -- docs/roadmap/GRADE-2026-09.md. Where the reference still is drawn,
// in drawable pixels, and the part of it sampled there (MonitorCompare.h).
struct ReferenceParameters {
    float left;
    float top;
    float right;
    float bottom;
    float sourceWidth;
    // The video area's last row: a zoom past the viewer draws no still
    // below it.
    float clipBottom;
    int enabled;
};

fragment float4 fragment_output(VertexOut in [[stage_in]],
                                texture2d<float> workingTexture [[texture(0)]],
                                texture3d<float> ocioOutputLut [[texture(1)]],
                                texture3d<float> lookLut [[texture(2)]],
                                texture2d<float> referenceTexture
                                [[texture(3)]],
                                sampler textureSampler [[sampler(0)]],
                                device atomic_uint* histogram [[buffer(1)]],
                                constant OutputParameters& parameters
                                [[buffer(0)]],
                                constant ScopeGuideParameters& guides
                                [[buffer(2)]],
                                constant ReferenceParameters& reference
                                [[buffer(3)]]) {
    float4 working = workingTexture.sample(textureSampler, in.uv);
    float4 output =
        float4(displaySignalWithLook(working.rgb, parameters, ocioOutputLut,
                                     lookLut, textureSampler),
               working.a);
    // G6 -- the reference is what Record showed: drawn as it is, never
    // through the working space and back.
    const float2 pixel = in.position.xy;
    if (reference.enabled != 0 && pixel.x >= reference.left &&
        pixel.x < reference.right && pixel.y >= reference.top &&
        pixel.y < reference.bottom && pixel.y < reference.clipBottom) {
        const float2 uv = float2(
            (pixel.x - reference.left) / (reference.right - reference.left) *
                reference.sourceWidth,
            (pixel.y - reference.top) / (reference.bottom - reference.top));
        output = float4(referenceTexture.sample(textureSampler, uv).rgb, 1.0);
    }
    if (parameters.scopeMode == 4) {
        const float3 weights = parameters.outputGamut == 1
                                   ? float3(0.2627, 0.6780, 0.0593)
                                   : float3(0.2126, 0.7152, 0.0722);
        output.rgb = falseColor(output.rgb, weights, guides);
    }
    return scopeOverlay(output, in.position.xy, histogram, parameters, guides);
}

struct SolidParameters {
    float4 rect;
    float4 topColor;
    float4 bottomColor;
    float2 drawableSize;
    float2 padding;
    int colorManagementEnabled;
    int outputTransfer;
    int2 colorPadding;
};

struct SolidVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex SolidVertexOut vertex_solid(uint vertexId [[vertex_id]],
                                   constant SolidParameters& parameters
                                   [[buffer(0)]]) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 pixel =
        parameters.rect.xy + corners[vertexId] * parameters.rect.zw;
    const float2 ndc = float2(pixel.x / parameters.drawableSize.x * 2.0 - 1.0,
                              1.0 - pixel.y / parameters.drawableSize.y * 2.0);
    SolidVertexOut out;
    out.position = float4(ndc, 0.0, 1.0);
    out.color =
        mix(parameters.topColor, parameters.bottomColor, corners[vertexId].y);
    return out;
}

float4 uiWorkingColor(float4 color, int colorManagementEnabled,
                      int outputTransfer) {
    float3 rgb = color.rgb;
    if (colorManagementEnabled != 0) {
        // UI colours are authored in sRGB. Put them in the same linear AP1
        // working buffer as video so translucent timeline shapes blend in
        // linear light rather than in encoded HLG.
        rgb =
            select(rgb / 12.92, pow((rgb + 0.055) / 1.055, 2.4), rgb > 0.04045);
        rgb = sourceToAP1(rgb, 0);
        if (outputTransfer == 1) {
            // The output pass maps scene reflection 0.9 to signal HLG 0.75.
            rgb *= 0.9;
        }
    }
    return float4(rgb, color.a);
}

fragment float4 fragment_solid(SolidVertexOut in [[stage_in]],
                               constant SolidParameters& parameters
                               [[buffer(0)]]) {
    return uiWorkingColor(in.color, parameters.colorManagementEnabled,
                          parameters.outputTransfer);
}

// R5 -- docs/roadmap/REEL-2026-09.md. A colour dip on a colour-managed
// monitor. Its colour is a graphic colour, the code value the author wrote,
// so it is mixed by the rule a motion layer follows
// (OpenColorIoCompositeDisplayReferred): the composite under it goes out
// through the SDR view, the colour is mixed there by the veil's strength,
// and the mix comes back through the view's inverse. uiWorkingColor, which
// a timeline shape uses, would send a white dip through the tone curve of
// the view and leave it grey. Reads the composite by framebuffer fetch, as
// fragment_working_display_referred does; where the veil is nil the
// composite is returned untouched.
fragment float4 fragment_solid_display_referred(
    SolidVertexOut in [[stage_in]], float4 below [[color(0)]],
    texture3d<float> motionLut [[texture(4)]],
    texture3d<float> viewLut [[texture(5)]],
    sampler lutSampler [[sampler(0)]],
    constant OutputParameters& view [[buffer(2)]]) {
    const float coverage = clamp(in.color.a, 0.0, 1.0);
    if (coverage <= 0.0) return below;
    return float4(compositeDisplayReferred(below.rgb, in.color.rgb, coverage,
                                           view, viewLut, motionLut,
                                           lutSampler),
                  below.a);
}

struct TextParameters {
    float4 rect;
    float4 uv;
    float4 color;
    float2 drawableSize;
    float2 padding;
    int colorManagementEnabled;
    int outputTransfer;
    int2 colorPadding;
};

struct TextVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex TextVertexOut vertex_text(uint vertexId [[vertex_id]],
                                 constant TextParameters& parameters
                                 [[buffer(0)]]) {
    const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    const float2 pixel =
        parameters.rect.xy + corners[vertexId] * parameters.rect.zw;
    TextVertexOut out;
    out.position =
        float4(pixel.x / parameters.drawableSize.x * 2.0 - 1.0,
               1.0 - pixel.y / parameters.drawableSize.y * 2.0, 0.0, 1.0);
    out.uv = mix(parameters.uv.xy, parameters.uv.zw, corners[vertexId]);
    return out;
}

fragment float4 fragment_text(TextVertexOut in [[stage_in]],
                              constant TextParameters& parameters [[buffer(0)]],
                              texture2d<float> atlas [[texture(0)]],
                              sampler textureSampler [[sampler(0)]]) {
    const float coverage = atlas.sample(textureSampler, in.uv).r;
    float4 color = parameters.color;
    color.a *= coverage;
    return uiWorkingColor(color, parameters.colorManagementEnabled,
                          parameters.outputTransfer);
}
