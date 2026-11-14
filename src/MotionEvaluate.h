#pragma once

// MOTION-2026-09 M1 -- ROADMAP.md. Evaluates one frame of a composition into a
// laid-out tree: bindings resolved, sequences in local time, animations
// applied, geometry computed by Yoga (flexbox). A frame is a pure function of
// composition, props and frame number. Text and image sizes come from an
// injected measurer -- fake metrics in portable tests, Core Text from M2 -- so
// the policy stays testable without macOS. Nothing here draws pixels.
//
// M1b adds what a motion needs past boxes and lines: text cut into lines,
// words and characters after layout (cascades and style ranges), vector
// paths with trims, clip-path polygons, repeats and series, and sequences
// whose local time runs at a speed of their own. All of it is exposed in the
// evaluated frame, so a caller reads the box of a word or the bounds of a
// trimmed path instead of measuring pixels.

#include "MotionComposition.h"
#include "MotionMedia.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct MotionTextStyle {
    std::string font_family = "system-ui";
    double font_weight = 400.0;
    double font_size = 16.0;
    // Resolved pixels; 0 means the font's normal line height.
    double line_height = 0.0;
    double letter_spacing = 0.0;
    std::string text_transform = "none";
    std::string text_align = "left";
    MotionColor color;
    // M1b -- CSS word-spacing: added to every space, on top of the letter
    // spacing.
    double word_spacing = 0.0;
    // M1b -- -webkit-text-stroke: an outline centred on the glyph edges.
    double stroke_width = 0.0;
    MotionColor stroke_color;
    // Off when the text is cut into characters: a ligature would draw two
    // characters as one glyph, which a per-character cascade cannot split.
    // An engine setting, not a style: never serialised.
    bool ligatures = true;
    // REMOTION-PARITY-2026-09 REM3 -- the family's italic face, never a
    // slanted roman: a family without one is FontMissing.
    bool italic = false;
    // MOTION-MARQUE-2026-10 MM2 -- font-variant-numeric: "lining" or
    // "oldstyle" figures, "tabular" or "proportional" widths; empty, the
    // font's own (ParseMotionNumericVariant).
    std::string numeric_figures;
    std::string numeric_spacing;
};

// M1b -- one grapheme cluster of a laid-out line: bytes [start, start +
// length) of the line's text, its advance measured from the line's x.
struct MotionTextCluster {
    size_t start = 0;
    size_t length = 0;
    double x = 0.0;
    double width = 0.0;
};

// Positions are relative to the text node's border box. A measurer fills
// `clusters` for every line: words and characters are cut from them.
struct MotionTextLine {
    std::string text;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double baseline = 0.0;
    std::vector<MotionTextCluster> clusters;
};

struct MotionTextLayout {
    double width = 0.0;
    double height = 0.0;
    std::vector<MotionTextLine> lines;
};

class MotionMeasurer {
public:
    virtual ~MotionMeasurer() = default;

    // Lays out text. A negative max_width means unconstrained; otherwise
    // lines are aligned within it. A font that cannot be resolved must fail
    // with FontMissing rather than measure a substitute.
    virtual bool LayoutText(const std::string& text,
                            const MotionTextStyle& style, double max_width,
                            MotionTextLayout& layout, MotionError& error) = 0;

    virtual bool ImageSize(const std::string& source, double& width,
                           double& height, MotionError& error) = 0;

    // REMOTION-PARITY-2026-09 REM2 -- a video's or a sound's media: `src` as
    // an image's source is written, or `media`, a project media's ULID. A
    // measurer that knows no media refuses both (MediaMissing), which is
    // what a composition without video or sound never asks.
    virtual bool MediaInfo(const std::string& src, const std::string& media,
                           MotionMediaInfo& info, MotionError& error);

    // REM2 -- the whole sound of that media, 48 kHz stereo (MotionAudio.h
    // mixes it).
    virtual bool MediaSound(const std::string& src, const std::string& media,
                            std::shared_ptr<const MotionPcm>& pcm,
                            MotionError& error);

    // MOTION-AFTER-2026-09 -- the PostScript name of the face `style`
    // resolves to, which an After Effects text layer is set by. A measurer
    // that resolves no font file refuses (FontMissing).
    virtual bool FontPostScriptName(const MotionTextStyle& style,
                                    std::string& name, MotionError& error);

    // MOTION-WEB-2026-10 MW7 -- whether the face `style` resolves to is an
    // instance of a variable font, at which `weight` of its wght axis, and
    // whether the font names that instance (`named`) or the axis was set
    // between its named instances. After Effects is given a PostScript name
    // and no axis, so its export says so. A measurer that resolves no font
    // file answers no.
    virtual bool FontVariableWeight(const MotionTextStyle& style, int& weight,
                                    bool& named);

    // MOTION-MARQUE-2026-10 MM2 -- whether `text` set in `style` shows
    // other glyphs, or other advances, than in the font's default figures:
    // what a reader that cannot apply font-variant-numeric (After Effects)
    // must say. A measurer that cannot tell answers yes for a text with a
    // digit and a variant.
    virtual bool NumericVariantChanges(const std::string& text,
                                       const MotionTextStyle& style);
};

// REM2 -- the compositions of a project an instance may name, parsed once,
// by ULID. Owned by whoever evaluates; the evaluator only reads it.
struct MotionLibrary {
    std::map<std::string, MotionComposition> compositions;
};

// REM2 -- what an evaluation may reach beyond the composition itself.
struct MotionEvaluationContext {
    // Null: an instance is refused (UnknownComposition).
    const MotionLibrary* library = nullptr;
    // The ULID of the composition evaluated, when it has one: an instance of
    // itself, however deep, is a cycle.
    std::string composition_id;
};

// REM2 -- what an evaluation of a composition of a project reaches beyond
// it: the compositions its instances show, parsed, and the project's media
// by ULID (MotionReachFor, MotionDocument.h). Empty for a composition read
// from a file, which can then name neither.
struct MotionReach {
    std::string composition_id;
    std::shared_ptr<const MotionLibrary> library;
    std::map<std::string, std::string> media_paths;

    MotionEvaluationContext Context() const {
        return {library.get(), composition_id};
    }
};

// REM2 -- instances nest at most this deep: a cycle the document would have
// refused cannot recurse forever in a composition read from a file.
inline constexpr int kMotionInstanceDepth = 8;

struct MotionBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// M1b -- a word or a character after layout, positioned like a line:
// relative to the text node's border box, as tall as its line box. Its width
// is its advance, letter spacing included.
struct MotionTextSpan {
    std::string text;
    size_t line = 0;
    // For a character, the word it belongs to.
    size_t word = 0;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double baseline = 0.0;
};

// M1b -- how one unit is painted: the text's colour, then the ranges that
// cover it, then its own animation, delayed by the stagger. Transforms apply
// around the centre of the unit's box, like a node's.
struct MotionTextUnitState {
    size_t index = 0;  // into lines, words or chars, by the unit kind
    int64_t delay = 0;
    double opacity = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double rotate = 0.0;
    MotionColor color;
    bool has_background = false;
    MotionColor background;
};

// MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. One band of a
// highlight (MotionTextHighlight), on one line: `box` is the whole band,
// relative to the text node's border box like a span; `painted` is the width
// filled from its left at this frame, the highlight's progress spread over
// its bands in reading order.
struct MotionHighlightBand {
    size_t highlight = 0;  // into the text node's highlights
    size_t line = 0;
    MotionBox box;
    double painted = 0.0;
    MotionColor color;
};

// M1b -- a path as drawn: viewBox applied, trimmed, in composition pixels
// before the node's transforms.
struct MotionPathPaint {
    MotionPath geometry;
    // Length of the whole path before trimming, in pixels.
    double length = 0.0;
    bool has_bounds = false;
    MotionBox bounds;
    bool has_fill = false;
    MotionColor fill;
    bool has_stroke = false;
    MotionColor stroke;
    double stroke_width = 0.0;
    std::string linecap = "butt";
    std::string linejoin = "miter";
    std::string fill_rule = "nonzero";
    double trim_start = 0.0;
    double trim_end = 1.0;
    // MOTION-DESIGN-2026-09 -- stroke dashes, in the node's pixels.
    std::vector<double> dash;
    double dash_offset = 0.0;
};
// REM2 -- what a video or an audio node plays at this instant. Media time
// is exact: (startFrom + local * playbackRate) / fps, where local is the
// node's own time in frames of its composition. Outside [startFrom,
// min(endAt, the media's end)) the node shows and plays nothing: `state`
// is then "before" or "after".
struct MotionMediaState {
    std::string src;
    std::string media;
    std::string state = "playing";
    int64_t time_numerator = 0;
    int64_t time_denominator = 1;
    // The picture shown, counted at the media's own cadence from its first
    // picture: the one that contains the media time. Video only.
    int64_t frame = 0;
    bool muted = false;
    double volume = 1.0;
};

// REM2 -- an instance: the composition it shows, the frame of that
// composition it is at (a fraction when a timeScale or another cadence
// makes it one), and how that composition's canvas maps into the instance's
// box -- by objectFit, like an image. Outside the composition's duration
// nothing is shown: `state` is "before" or "after".
struct MotionInstanceState {
    std::string composition_id;
    std::string state = "playing";
    int64_t local_numerator = 0;
    int64_t local_denominator = 1;
    int32_t width = 0;
    int32_t height = 0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double origin_x = 0.0;
    double origin_y = 0.0;
};

// REM3 -- a gradient resolved in composition pixels, before the node's
// transforms: for a linear gradient, the gradient line from (x0, y0) to
// (x1, y1); for a radial one, the centre (x0, y0) and the radii (x1, y1).
// Stops at their positions, CSS's fix-ups done.
struct MotionResolvedGradient {
    MotionGradientKind kind = MotionGradientKind::Linear;
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    std::vector<std::pair<double, MotionColor>> stops;
};

// REM3 -- a clip-path shape resolved in composition pixels, before
// transforms: a polygon's vertices, a circle's or an ellipse's centre and
// radii, an inset's rectangle and corner radius.
struct MotionResolvedClip {
    MotionClipKind kind = MotionClipKind::None;
    std::vector<std::pair<double, double>> points;
    double cx = 0.0;
    double cy = 0.0;
    double rx = 0.0;
    double ry = 0.0;
    MotionBox rect;
    double round = 0.0;
};

// REM3 -- a transition of a series at this instant: which one, how far
// through (the eased or sprung progress), and its window in the series'
// frames.
struct MotionTransitionState {
    bool active = false;
    std::string uid;
    MotionPresentation presentation = MotionPresentation::Fade;
    MotionDirection direction = MotionDirection::FromRight;
    double progress = 0.0;
    int64_t from = 0;
    int64_t to = 0;
};

struct MotionEvaluatedNode {
    // Structural path, the same spelling as parse errors. Diagnostic only:
    // `uid` is the address.
    std::string path;
    std::string id;
    // M3 -- the document node this box comes from, the ULID an edit names.
    // The copies of a repeat share their template's.
    std::string uid;
    MotionNodeType type = MotionNodeType::Box;
    // Layout box in composition pixels, before transforms.
    MotionBox box;
    double opacity = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double rotate = 0.0;
    bool has_background = false;
    MotionColor background;
    double border_radius = 0.0;
    std::string text;
    MotionTextStyle text_style;
    MotionTextLayout text_layout;
    std::string image_source;
    std::string object_fit = "fill";
    // M1b -- clip-path polygon in composition pixels, before transforms; it
    // moves with the node and clips its descendants.
    bool has_clip = false;
    std::vector<std::pair<double, double>> clip;
    // M1b -- words and characters of a text node, and the units it is
    // painted by when it has units or ranges (empty otherwise).
    std::vector<MotionTextSpan> words;
    std::vector<MotionTextSpan> chars;
    MotionTextUnit unit_kind = MotionTextUnit::None;
    std::vector<MotionTextUnitState> units;
    // MB3 -- the bands of its highlights, under the glyphs.
    std::vector<MotionHighlightBand> highlights;
    MotionPathPaint path_paint;
    // M1b -- local time of a sequence or series, in its own frames (a
    // fraction when a timeScale makes it one).
    bool has_local_frame = false;
    int64_t local_numerator = 0;
    int64_t local_denominator = 1;
    // MOTION-DESIGN-2026-09 -- CSS mix-blend-mode; empty or "normal": none.
    std::string blend_mode;
    // MOTION-DESIGN-2026-09 -- CSS isolation: isolate.
    bool isolate = false;
    // REM2 -- CSS overflow: hidden clips the descendants to the border box,
    // rounded by border_radius.
    bool overflow_hidden = false;
    // M5b -- the node's `decor`: the safe-area check skips it and its
    // descendants, and says so (MotionSafeArea.h).
    bool decor = false;
    // REM3 -- a background gradient (under the content, over the
    // background colour), a mask gradient (its alpha multiplies what the
    // node paints, nothing outside its box), every clip-path shape, and the
    // raster effects with the frame pixels they may write.
    bool has_gradient = false;
    MotionResolvedGradient gradient;
    bool has_mask = false;
    MotionResolvedGradient mask;
    MotionResolvedClip clip_shape;
    std::vector<MotionFilter> filters;
    bool has_filter_domain = false;
    MotionBox filter_domain;
    // REM3 -- a sequence shown by a transition: "exiting" or "entering",
    // at which progress, added into its opacity and translation; a fade's
    // entering scene adds to the exiting one (plus-lighter), a wipe's or an
    // iris' is clipped by `transition_clip`.
    std::string transition_role;
    double transition_progress = 0.0;
    bool blend_plus_lighter = false;
    MotionResolvedClip transition_clip;
    // REM3 -- on a series: the transition running now, if any.
    MotionTransitionState transition;
    // REMOTION-PARITY-2026-09 REM4 -- 2.5D. The node's turns about its
    // horizontal and vertical axes (degrees), the perspective it gives its
    // own children (0: none), and, when it turns in depth, the transform
    // that places it in its parent's space: a 4x4 matrix, row-major, from
    // points of its plane (x, y, 0, 1) -- translate, rotateX, rotateY,
    // rotate, scale around the centre of the box, then the parent's
    // perspective about the parent's centre. `quad` is where its box's
    // corners land in the frame (top-left, top-right, bottom-right,
    // bottom-left), every ancestor applied.
    double rotate_x = 0.0;
    double rotate_y = 0.0;
    double perspective = 0.0;
    bool has_3d = false;
    double matrix3d[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool has_quad = false;
    std::pair<double, double> quad[4];
    MotionMediaState media;
    MotionInstanceState instance;
    // REM2 -- the evaluated root of an instance, in the coordinates of its
    // own canvas (`instance` maps them into this node's box). Kept apart
    // from `children`, whose boxes are in this composition's pixels. Empty
    // when nothing is shown.
    std::vector<MotionEvaluatedNode> instance_root;
    std::vector<MotionEvaluatedNode> children;
};

struct MotionFrame {
    int32_t width = 0;
    int32_t height = 0;
    int64_t frame = 0;
    MotionEvaluatedNode root;
    // MOTION-DESIGN-2026-09 -- with the composition's motionBlur, the other
    // instants of the shutter, in order, each a whole tree evaluated at its
    // exact fractional frame: the painter averages them with `root`, which
    // stays the frame itself (what the descriptor and a click read).
    std::vector<MotionEvaluatedNode> motion_blur;
    // MOTION-DESIGN-2026-09 -- the composition's grain, painted once over
    // the finished frame (0: none), cell size in frame pixels.
    double grain_amount = 0.0;
    double grain_size = 1.0;
    int64_t grain_seed = 0;
};

// MOTION-DESIGN-2026-09 -- the frame drawn at `scale` of its size: the root
// and every shutter instant scaled about the top-left corner, sizes and
// effect domains recomputed. Layout is unchanged (it happened at full
// size); what is painted is vector, so it is drawn sharp at the new size.
void ScaleMotionFrame(MotionFrame& frame, double scale);

// `props` is a JSON object of overrides (or null). Unknown props, wrong types
// and out-of-range values are refused, never coerced.
bool EvaluateMotionFrame(const MotionComposition& composition,
                         const mcp_json::Value& props, int64_t frame,
                         MotionMeasurer& measurer, MotionFrame& output,
                         MotionError& error);

// REM2 -- the same, with the project's other compositions for instances.
bool EvaluateMotionFrame(const MotionComposition& composition,
                         const mcp_json::Value& props, int64_t frame,
                         MotionMeasurer& measurer,
                         const MotionEvaluationContext& context,
                         MotionFrame& output, MotionError& error);

// MB11 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The composition as
// the instance `props` plays it: every animated key and media time bound to
// a prop holds that instance's value, and nothing is bound any more. For a
// reader that takes keys as written instead of evaluating them (the After
// export); `measurer` is the evaluator's, binding measures nothing.
bool BindMotionInstance(const MotionComposition& composition,
                        const mcp_json::Value& props, MotionMeasurer& measurer,
                        MotionComposition& out, MotionError& error);

// Canonical JSON of an evaluated frame: fixed key order, numbers rounded to a
// thousandth of a pixel for reading. This is what a caller inspects instead of
// measuring rendered pixels.
std::string DumpMotionFrame(const MotionFrame& frame);

// MOTION-AFTER-2026-09 -- what a timecode binding shows for composition
// frame `frame` (its offset added; a negative frame reads as 0): HH:MM:SS:FF
// of a whole frame at the nominal rate, 25 for 25 fps and 30 for
// 30000/1001, never drop-frame. MA6's humanize pass reads a row of digit
// strips against it.
std::string MotionTimecodeText(int64_t frame, int64_t fpsNumerator,
                               int64_t fpsDenominator);

// MA11 -- docs/roadmap/MOTION-AFTER-2026-09.md. A clip-path shape against a
// border box, and two colours mixed, exactly as the evaluator does: what
// the After export reads a mask and a colour key from.
MotionResolvedClip ResolveMotionClip(const MotionClipShape& shape,
                                     const MotionBox& box);
MotionColor MixMotionColors(const MotionColor& from, const MotionColor& to,
                            double fraction);
