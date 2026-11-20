#pragma once
// G1 -- docs/roadmap/GRADE-2026-09.md. What a colourist reads on the scopes,
// measured on one frame of a clip as the export delivers it.
//
// A colourist balances with the scopes open: exposure first, then white
// balance, then saturation, and never by eye alone. The agent had nothing of
// the kind. The interview skill checked SATAVG by hand with FFmpeg, and
// read_frame shows the source without its grade, so a grade was set blind
// and seen only once exported. PHILOSOPHY.md principle 7: what a model would
// guess from a JPEG, the code measures.
//
// One frame of each clip goes through the chain the export gives that clip
// alone: graded, the sequence look at its stage, the output transform.
// Nothing is composited -- no opacity, fade, caption or clip above --
// because the question is the clip's own balance, which is what a grade
// changes. Every colour goes through the reference functions the export's
// cubes sample (OpenColorIoColorPath, ApplyResolvedColorGrade,
// EvaluateLookLut), never a second spelling of the chain. The source itself
// is decoded as the monitor decodes it, not through swscale's YCbCr
// conversion, which reads limited-range codes 0.385 % low (see the .cc).
//
// Nothing here judges. There is no "well exposed" and no target grey: a
// night scene has no business putting its median at 18 %. The measure gives
// the numbers a colourist reads; matching two shots (G2) computes from them.
//
// Every result is an integer times kColorMeasureScale, as ShotQuality.h's
// are, and the reply writes them as exact decimals.

#include "ColorEffects.h"
#include "ColorManagement.h"
#include "ColorMatch.h"
#include "Document.h"
#include "FrameCapture.h"
#include "Json.h"
#include "LookLut.h"
#include "RationalTime.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Hundredths: of a percent, of a stop, of an 8-bit chroma code, of a degree.
inline constexpr int64_t kColorMeasureScale = 100;

// The skin tone line of a vectorscope: the I axis of NTSC, 123 degrees from
// the +Cb axis towards +Cr, between red and yellow. Skin of every complexion
// sits on it under neutral light, which is why every vectorscope draws it.
inline constexpr int64_t kSkinLineDegrees = 123;

// Display-signal bands of the balance zones, in percent of full scale: the
// 25 % and 75 % lines of the waveform graticule (shader.metal's scopeGrid).
inline constexpr int64_t kColorShadowsBelowPercent = 25;
inline constexpr int64_t kColorHighlightsFromPercent = 75;

// At most how many clips one call measures: each costs a decode, and a
// whole reel belongs in several calls.
inline constexpr size_t kMaxColorMeasureClips = 64;

// A rectangle of the measured picture -- the part of the source the
// sequence shows -- as exact fractions of its width and height, top-left
// origin. A pixel belongs to it when its centre does.
struct ColorMeasureRegion {
    bool present = false;
    EffectParamValue x{0, 1};
    EffectParamValue y{0, 1};
    EffectParamValue width{1, 1};
    EffectParamValue height{1, 1};
};

bool ValidateColorMeasureRegion(const ColorMeasureRegion& region,
                                std::string& error);

// One band of the display signal, as the RGB parade shows it.
struct ColorZoneMeasure {
    int64_t pixels = 0;
    // Mean display signal per channel, percent of full scale.
    int64_t red = 0;
    int64_t green = 0;
    int64_t blue = 0;
    // red - blue: positive is warm, the side color.temperature raises.
    int64_t warmth = 0;
    // green - (red + blue) / 2: positive is green, negative magenta; the
    // opposite of color.tint, which raises magenta.
    int64_t green_magenta = 0;
};

// The statistics of one set of pixels: the whole picture, or the region.
struct ColorPictureMeasure {
    int64_t pixels = 0;
    // Display luma (the output's own weights), percent of full scale, at the
    // 2nd, 50th and 98th percentiles.
    int64_t luma_p02 = 0;
    int64_t luma_p50 = 0;
    int64_t luma_p98 = 0;
    // Percent of pixels with a channel on the top 10-bit code, and with
    // every channel on the bottom one.
    int64_t clipped = 0;
    int64_t crushed = 0;
    // Chroma magnitude in the unit of FFmpeg signalstats' SAT: 8-bit
    // limited-range code values from the neutral axis, 224 * |(Cb, Cr)|.
    // Same unit, not the same number: signalstats reads the encoded file,
    // whose chroma is subsampled.
    int64_t saturation_mean = 0;
    int64_t saturation_p95 = 0;
    // Vectorscope angle of the mean chroma, degrees from +Cb towards +Cr in
    // [0, 360), and its signed distance to the skin line: positive towards
    // yellow, negative towards red. Absent when the mean chroma is too close
    // to neutral for an angle to mean anything.
    std::optional<int64_t> hue;
    std::optional<int64_t> skin_line_offset;
    // Shadows, midtones, highlights.
    std::array<ColorZoneMeasure, 3> zones{};
    // Under colour management only: scene-linear luminance of the working
    // space, grade included and before the look, in stops from 18 % grey,
    // at the same percentiles. The unit color.exposure moves under ACEScct.
    // A percentile whose luminance is not above zero has no stop value.
    bool scene = false;
    std::optional<int64_t> scene_p02;
    std::optional<int64_t> scene_p50;
    std::optional<int64_t> scene_p98;
};

// The chain one clip goes through on its own, resolved from the document.
struct ColorMeasureChain {
    ColorManagementSettings color;
    // Built from `color` when it is enabled.
    OpenColorIoColorPath path;
    ResolvedColorGrade grade;
    LookLutStage look_stage = LookLutStage::None;
    std::shared_ptr<const LookLutSamples> look;
};

// Portable half: `signal` is the decoded source, full-range RGB in 0..1,
// row by row, `width` x `height`. Fills `picture`, and `inRegion` when the
// region is present.
bool MeasureColorSignal(const std::vector<std::array<float, 3>>& signal,
                        int32_t width, int32_t height,
                        const ColorMeasureChain& chain,
                        const ColorMeasureRegion& region,
                        ColorPictureMeasure& picture,
                        ColorPictureMeasure& inRegion, std::string& error);

struct ColorMeasurePlan {
    std::vector<Ulid> clip_ids;
    std::string position = "Middle";
    ColorMeasureRegion region;
};

struct ColorMeasureSettings {
    // Long edge of the measured picture. Enough pixels for a percentile to
    // be stable, few enough to decode and grade quickly.
    int32_t max_dimension = 480;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
};

struct ColorClipMeasurement {
    Ulid clip_id;
    Ulid source_id;
    RationalTime source_time;
    int32_t width = 0;
    int32_t height = 0;
    ColorPictureMeasure picture;
    ColorPictureMeasure region;
};

// Measures the plan's clips in `document`, whose media paths and look file
// resolve against `projectDirectory`. Refuses by name an unknown clip, a
// motion clip (drawn by the engine, never graded), a clip without video, and
// a position other than Start, Middle and End.
bool RenderColorMeasurement(const Document& document,
                            const std::string& projectDirectory,
                            const ColorMeasurePlan& plan,
                            const ColorMeasureSettings& settings,
                            std::vector<ColorClipMeasurement>& results,
                            std::string& error);

// G1b -- docs/roadmap/GRADE-2026-09.md. read_frame with `graded`: the
// frame at `sourceTime` of the clip, graded, with the sequence look, through
// the SDR display view -- what Record shows of this clip alone -- composed
// into the sequence frame when the clip is framed, as the plain read_frame
// composes it. Decoded and graded pixel for pixel as the measure is, so what
// an agent reads with measure_color and what it sees here are one picture;
// encoded by FrameCapture.h like every picture a model reads. Same refusals
// as the measure.
bool CaptureGradedClipFrame(const Document& document,
                            const std::string& projectDirectory,
                            const Ulid& clipId, const RationalTime& sourceTime,
                            const FrameCaptureSettings& settings,
                            std::string& jpegBytes, std::string& error);

// DEC29 -- docs/roadmap/GRADE-2026-09.md. One cell of a graded sheet
// (contact_sheet and cut_sheet with `graded`): a clip and the source time
// it shows there.
struct GradedSheetCell {
    Ulid clip_id;
    RationalTime source_time;
};

// The cells of a graded sheet, each the picture read_frame graded gives of
// its clip at its source time -- graded, with the sequence look, through the
// SDR view Record uses, by the same chain pixel for pixel -- composed into a
// cell of `cellWidth` x `cellHeight` the way the plain sheet composes it: a
// framed clip in the rectangle its framing names in the sequence frame
// reduced to `bound`, any other fitted to that frame and centred, black
// around. Display RGB in 0..1, row by row, one picture per cell, in order.
// Same refusals as the graded frame, by clip.
bool RenderGradedSheetCells(
    const Document& document, const std::string& projectDirectory,
    const std::vector<GradedSheetCell>& cells, int64_t cellWidth,
    int64_t cellHeight, int32_t bound, const std::string& ffmpegPath,
    std::vector<std::vector<std::array<float, 3>>>& pictures,
    std::string& error);

// The reply of measure_color and --measure-color, written once so the two
// surfaces cannot describe the same measure differently.
mcp_json::Value ColorMeasurementsValue(
    const Document& document, const ColorMeasurePlan& plan,
    const std::vector<ColorClipMeasurement>& results);

// G2 -- docs/roadmap/GRADE-2026-09.md. match_color and --match-color: the
// clip matched to a reference clip (ColorMatch.h), read from one frame of
// each, as measure_color reads them. A region on either side compares that
// part only: a face to a face, a grey card to a grey card.
struct ColorMatchPlan {
    Ulid clip_id;
    Ulid reference_clip_id;
    std::string position = "Middle";
    std::string reference_position = "Middle";
    ColorMeasureRegion region;
    ColorMeasureRegion reference_region;
    ColorMatchAspects aspects;
};

struct ColorMatchResult {
    // "acescct", "linear" or "display": where the correction was solved.
    std::string grading_space;
    // The clip's four knobs before and after the match, as its stages hold
    // them (the after exactly what `effects` carries).
    ColorMatchKnobs knobs_before;
    ColorMatchKnobs knobs_after;
    // The balance wanted a temperature past what the knob reaches
    // (kMinimumKelvin to kMaximumKelvin).
    bool kelvin_limited = false;
    bool saturation_undefined = false;
    // The clip's whole stack with the correction in it, and where each of
    // its knobs went.
    std::vector<ClipEffect> effects;
    std::vector<ColorMatchPlacement> placements;
    // measure_color's numbers for the reference, the clip as it is, and the
    // clip as `effects` would render it.
    ColorClipMeasurement reference;
    ColorClipMeasurement before;
    ColorClipMeasurement after;
};

// Reads both clips, solves and places the correction. Changes nothing: the
// caller hands the step to prepare_edit_plan or to set_clip_effects. Same
// refusals as the measure.
bool RenderColorMatch(const Document& document,
                      const std::string& projectDirectory,
                      const ColorMatchPlan& plan,
                      const ColorMeasureSettings& settings,
                      ColorMatchResult& result, std::string& error);

mcp_json::Value ColorMatchValue(const Document& document,
                                const ColorMatchPlan& plan,
                                const ColorMatchResult& result);

// AGENT-2026-09 A16 -- docs/roadmap/AGENT-2026-09.md. propose_exposure:
// every clip measured at once and, for each one whose scene median sits
// further than `tolerance` from `target` (stops from 18 % grey), the clip's
// stack with its exposure moved by the difference, placed as match_color
// places a correction. On the first run by the MCP server the model set the
// colour management and graded nothing: 25 clips at +1.4 to +2.4 stops,
// which the human verdict called overexposed. The tool decides nothing: a
// scene wanted dark keeps a proposal its caller is free to drop.
struct ExposureProposalSettings {
    // Where a clip's scene median should land, in hundredths of a stop
    // from 18 % grey; 0 is middle grey.
    int64_t target_hundredths = 0;
    // A clip within this of the target is left alone: half a stop.
    int64_t tolerance_hundredths = 50;
};

struct ExposureProposal {
    Ulid clip_id;
    // measure_color's scene median before, in hundredths of a stop; absent
    // without colour management or on a black picture.
    std::optional<int64_t> scene_p50_hundredths;
    // What the step adds to the clip's exposure, hundredths of a stop.
    int64_t correction_hundredths = 0;
    double exposure_before = 0.0;
    double exposure_after = 0.0;
    // The clip's whole stack with the correction in it, empty when nothing
    // is proposed.
    std::vector<ClipEffect> effects;
    std::vector<ColorMatchPlacement> placements;
    // "proposed", "within_tolerance" or "no_scene_measure".
    std::string reason;
};

// One proposal per measurement, in order. Refuses a measurement of a clip
// the document does not hold and a negative tolerance.
bool ProposeExposure(const Document& document,
                     const std::vector<ColorClipMeasurement>& measurements,
                     const ExposureProposalSettings& settings,
                     std::vector<ExposureProposal>& proposals,
                     std::string& error);

// The reply of propose_exposure: one line per clip and, under `steps`, a
// set_clip_effects step per proposal, ready for prepare_edit_plan.
mcp_json::Value ExposureProposalsValue(
    const Document& document, const ColorMeasurePlan& plan,
    const ExposureProposalSettings& settings,
    const std::vector<ColorClipMeasurement>& measurements,
    const std::vector<ExposureProposal>& proposals);

// AGENT-2026-09 A20 -- docs/roadmap/AGENT-2026-09.md. propose_grade: the
// exposure and the white balance of every clip, read where a colourist
// reads them. `anchor` says where: "face", the largest face the engine
// finds in the frame (FaceFinder.h); "neutral", a white or grey patch the
// caller names as `region`, the same on every clip; "median", the whole
// frame, exposure only. Exposure: the anchor's scene median is brought to
// `target` (stops from 18 % grey), when it sits further than `tolerance`
// from it. Balance: the anchor's midtones are brought onto the skin line
// (face) or to neutral (grey), with the clip's own temperature and tint,
// refined against the rendered picture as match_color refines. A clip
// whose face is not found gets its exposure from the montage, brought to
// the median the found faces left the frames at, and no balance.
// A lift stops where the frame's whites would go: the frame's scene p98
// is kept under `highlight_ceiling` (2.5 stops over 18 % grey is diffuse
// white), the correction cut to what the highlights leave room for. Seen
// on the bench of the 25th: a prosthetic read as a face, 1.1 stop under,
// lifted the frame 1.8 stops and sent a white shirt to 87 % -- nothing
// clipped under the ACES view, and it was blown all the same.
// Nothing is applied: the reply carries one set_clip_effects step per
// proposal, and says for each clip what it read and why.
struct GradeProposalPlan {
    std::vector<Ulid> clip_ids;
    std::string position = "Middle";
    std::string anchor = "face";
    ColorMeasureRegion region;
    bool exposure = true;
    bool balance = true;
    // Hundredths of a stop: where the anchor's median should land. On the
    // two runs of the 25th a face at about +0.7 stop read right.
    int64_t target_hundredths = 70;
    int64_t tolerance_hundredths = 25;
    int64_t highlight_ceiling_hundredths = 250;
    // GR15 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Under an HLG output the
    // ceiling is what the output clips, not a scene stop: a lift stands as
    // long as no more of the rendered frame lands on the top code than
    // before (LimitGradeExposureOnDisplay). On 6 November 2026 an interview
    // whose windows already sat at 5.3 stops got no lift at all under the
    // 2.5-stop rule, while the face at +1.5 its editor chose clipped
    // nothing. propose_grade sets it for an HLG sequence unless the caller
    // names a highlight_ceiling_stops, which keeps the scene rule.
    bool display_ceiling = false;
    // AGENT-2026-09 A31 -- one grade per rush. Read per clip, the faces of
    // one take came out from -0.35 to -1.09 stop apart on the third run
    // and the light jumped at every straight cut; the agent computed the
    // median per rush by hand. The anchored clips of a source vote by the
    // median of their corrections and balances, and every clip of the
    // source gets it, each within its own highlight ceiling.
    bool per_source = true;
    // AGENT-2026-09 A41 -- the full reading of every clip in the reply; off,
    // the anchor and frame stops and the skin line offset only.
    bool verbose = false;
};

// The exposure correction the frame's highlights allow, hundredths of a
// stop: `wanted` as it is when it lowers the picture or when the frame's
// scene p98 (`framePeak`, absent when unmeasured) stays under `ceiling`
// once lifted; otherwise the lift that reaches the ceiling, never below
// zero, and `limited` set. Pure, so the rule is tested without a frame.
int64_t LimitGradeExposure(int64_t wanted, std::optional<int64_t> framePeak,
                           int64_t ceiling, bool& limited);

// GR15 -- the ceiling of an HLG output. `clippedAt` renders the clip with
// `correction` hundredths of a stop more exposure and gives the share of
// its frame on the top code (ColorPictureMeasure::clipped, hundredths of a
// percent). A lowering passes as it is. A lift passes whole when the frame
// clips no more than `clippedBefore` (plus kDisplayClipAllowance, a few
// specks of a specular highlight); otherwise `limited` is set and
// `allowed` is the largest lift found that does not, by bisection to
// kDisplayCeilingStep, in at most kDisplayCeilingRenders renders. Pure but
// for the renders `clippedAt` makes, so the rule is tested without a frame.
inline constexpr int64_t kDisplayClipAllowance = 5;
inline constexpr int64_t kDisplayCeilingStep = 5;
inline constexpr int kDisplayCeilingRenders = 6;
using DisplayClipEvaluator = std::function<bool(
    int64_t correction, int64_t& clipped, std::string& error)>;
bool LimitGradeExposureOnDisplay(int64_t wanted, int64_t clippedBefore,
                                 const DisplayClipEvaluator& clippedAt,
                                 int64_t& allowed, bool& limited,
                                 std::string& error);

struct GradeProposal {
    Ulid clip_id;
    Ulid source_id;
    // "face", "neutral", "median" or "none".
    std::string anchor_used;
    ColorMeasureRegion anchor_region;
    ColorMatchKnobs knobs_before;
    ColorMatchKnobs knobs_after;
    int64_t correction_hundredths = 0;
    // GR15 -- what the clip's own reading asked before the highlight
    // ceiling cut it: equal to correction_hundredths when nothing was cut.
    // The vote of a rush reads it, so a rush whose faces all hit the
    // ceiling says so instead of voting a plain 0.
    int64_t wanted_hundredths = 0;
    bool balanced = false;
    std::vector<ClipEffect> effects;
    std::vector<ColorMatchPlacement> placements;
    // "proposed", "within_tolerance", "no_face", "no_face_matched_montage",
    // "no_scene_measure", "neutral_anchor", "per_source" or
    // "limited_by_highlights".
    std::string reason;
    // Why a clip could not be read ("unreadable"), when it could not.
    std::string error;
    ColorClipMeasurement before;
    ColorClipMeasurement after;
};

// The vote of one source's anchored clips: the median correction, kelvin
// and tint of the clips read on a face or a patch.
struct SourceGradeVote {
    int64_t correction_hundredths = 0;
    // GR15 -- the median of what the clips wanted before the ceiling, and
    // whether it is more than the vote: then the rush was held back by its
    // highlights, and each of its clips says so.
    int64_t wanted_hundredths = 0;
    bool limited = false;
    double kelvin = 0.0;
    double tint = 0.0;
    size_t count = 0;
};
std::map<Ulid, SourceGradeVote> SourceGradeVotes(
    const std::vector<GradeProposal>& proposals);

// The display midtones a face's mean colour would have on the skin line at
// the same saturation: the balance target of a face anchor, in
// measure_color's units (hundredths of a percent). False when the midtones
// are too neutral to have a hue -- nothing to rotate.
bool SkinLineBalanceTarget(const ColorZoneMeasure& midtones,
                           bool rec2020Display, int64_t& warmth,
                           int64_t& greenMagenta);

// Reads every clip of the plan, finds its anchor, solves and places the
// corrections. Changes nothing. Refuses a face anchor in a process without
// a face finder, a neutral anchor without a region, and what the measure
// refuses.
bool RenderGradeProposals(const Document& document,
                          const std::string& projectDirectory,
                          const GradeProposalPlan& plan,
                          const ColorMeasureSettings& settings,
                          std::vector<GradeProposal>& proposals,
                          std::string& error);

mcp_json::Value GradeProposalsValue(
    const Document& document, const GradeProposalPlan& plan,
    const std::vector<GradeProposal>& proposals);
