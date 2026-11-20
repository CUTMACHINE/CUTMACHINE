#pragma once

// MOTION-2026-09 M1 -- ROADMAP.md, docs/MOTION_FORMAT.md. A motion composition
// is data: a tree of nodes styled with CSS names and animated with Remotion's
// vocabulary. The author chooses composition and values; the engine measures
// what follows from them. Parsing is strict so a misspelt property fails with
// its path instead of silently doing nothing on screen.

#include "Json.h"
#include "Keyframes.h"
#include "MotionPath.h"
#include "MotionSpring.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct MotionError {
    std::string code;
    std::string path;
    std::string message;
};

// M3 adds point, size and group, the three kinds of control M0 counted on
// the templates and M1b left out: a position, a size, and a heading that
// gathers other props for whoever edits them.
enum class MotionPropType {
    Text,
    Number,
    Color,
    Bool,
    Enum,
    List,
    Point,
    Size,
    Group,
};

struct MotionPropDefinition {
    std::string name;
    MotionPropType type = MotionPropType::Text;
    mcp_json::Value default_value;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::vector<std::string> values;
    // M1b: the type of each element of a list prop (text, number or color);
    // minimum and maximum then bound each number.
    MotionPropType element_type = MotionPropType::Text;
    // M1b: "px" or "%" on a number prop. A bound value then reads as that CSS
    // length ("50%"), which is how a speed in percent reaches timeScale.
    // M3: also on a point or a size, whose components then read so.
    std::string unit;
    // M3: the props a group gathers, by name. Presentation only: a group has
    // no value, and nothing binds to it.
    std::vector<std::string> members;
};

// A literal, a prop, a prop looked up in a table (named token table or inline
// object), or whether a text prop is non-empty. This is what replaces the
// "dropdown -> colour" and "checkbox -> opacity" expressions of After Effects.
// M1b adds the element and the index of the enclosing repeat, the element
// optionally looked up in a table too.
enum class MotionBindingKind {
    Literal,
    Prop,
    PropMap,
    PropNotEmpty,
    Item,
    ItemMap,
    Index,
    // MOTION-AFTER-2026-09 -- a text's content: the timecode of the frame,
    // HH:MM:SS:FF at the composition's nominal rate, of the whole
    // composition ("composition") or of the enclosing sequence ("local"),
    // plus `timecode_offset` frames. What After says with an expression.
    Timecode,
};

struct MotionValue {
    MotionBindingKind kind = MotionBindingKind::Literal;
    mcp_json::Value literal;
    std::string prop;
    // M3: the component of a point or size prop, as JavaScript reads one --
    // {"prop": "position.x"} -- "x", "y", "width" or "height".
    std::string component;
    std::string table;
    std::vector<std::pair<std::string, mcp_json::Value>> inline_map;
    // MOTION-AFTER-2026-09 -- Timecode: "composition" or "local".
    std::string timecode_scope;
    int64_t timecode_offset = 0;
};

enum class MotionAnimationKind { Interpolate, Spring };

// What an interpolation outputs. M1b adds colours and polygons: their
// keyframe track holds the keyframe's index, so the eased progress of the
// current segment is read once and applied to every channel or vertex.
// REM3 adds gradients and filter lists, interpolated the same way: stop by
// stop, parameter by parameter, between values of the same shape. Polygon
// is every clip-path shape since REM3 (circle, ellipse, inset).
// MOTION-DESIGN-2026-09 -- Path: a path's `d`, morphed.
enum class MotionValueKind { Number, Color, Polygon, Gradient, Filter, Path };

// MOTION-WEB-2026-10 MW3 -- the named curves of GSAP, which two thirds of the
// tweens measured in MW0 use. Only power1 and power2 (in, out) and back (in,
// out) are cubic Béziers; the others are not, so the evaluator runs GSAP's
// own formula, and the track keeps the closest cubic-bezier for every
// reader that only knows Béziers (After, the canvas).
enum class MotionNamedFamily { None, Power, Sine, Expo, Circ, Back };
enum class MotionNamedDirection { In, Out, InOut };

struct MotionNamedCurve {
    MotionNamedFamily family = MotionNamedFamily::None;
    MotionNamedDirection direction = MotionNamedDirection::Out;
    // Power: the exponent GSAP raises to (power2 is 3).
    int exponent = 1;
    double overshoot = 1.70158;
    // As written, for the After report.
    std::string name;
    // The track's cubic-bezier is the curve itself, not an approximation.
    bool exact_bezier = false;
    // Largest gap between the curve and that cubic-bezier, in part of the
    // segment's amplitude (0.0026 for power3.out).
    double bezier_error = 0.0;
};

// GSAP 3.14's value of `curve` at progress `p` in [0, 1].
double EvaluateMotionNamedCurve(const MotionNamedCurve& curve, double p);

// Reads "power2.out", "sine.inOut", "back.out(1.7)", the bare family names
// ("expo" is expo.out, as in GSAP) and GSAP's aliases (quad, cubic, quart,
// quint, strong). `approximation` receives the cubic-bezier the track keeps.
bool ParseMotionNamedCurve(const std::string& text, MotionNamedCurve& curve,
                           KeyframeEasing& approximation);

// Interpolation keyframes are addressed by ULID like every other keyframe.
// M3: a composition stored in the document names them in `uids`, beside
// `frames` and `values`; a composition read from a file without them still
// gets stable synthetic IDs derived from the keyframe order.
struct MotionAnimation {
    MotionAnimationKind kind = MotionAnimationKind::Interpolate;
    MotionValueKind value_kind = MotionValueKind::Number;
    KeyframeTrack track;
    KeyframeExtrapolation extrapolate_left = KeyframeExtrapolation::Clamp;
    KeyframeExtrapolation extrapolate_right = KeyframeExtrapolation::Clamp;
    MotionSpring spring;
    int64_t delay = 0;
    // Colour or polygon keyframe values, in keyframe order.
    std::vector<mcp_json::Value> values;
    // MB11 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The keys whose
    // value comes from a prop ({"prop": name}, or through a map), by key
    // index. The track and `values` hold what the prop's default gives, so
    // every reader but the evaluator (After, humanize, the canvas) sees a
    // literal; the evaluator puts the instance's value in its place.
    std::vector<std::pair<size_t, MotionValue>> bound_values;
    // MW3 -- by key index, the named curve of the segment the key starts;
    // empty when no segment has one.
    std::vector<MotionNamedCurve> named_curves;
};

// MW3 -- the track's value at `time`, a segment with a named curve following
// that curve exactly. Every evaluation of an interpolation goes through it.
KeyframeEvaluation EvaluateMotionTrack(const MotionAnimation& animation,
                                       RationalTime time);

enum class MotionNodeType {
    Fill,
    Box,
    Text,
    Image,
    Sequence,
    Path,
    Repeat,
    Series,
    // REMOTION-PARITY-2026-09 REM2 -- a media's pictures (and its sound
    // unless muted), a media's sound alone, and an instance of another
    // composition of the project.
    Video,
    Audio,
    Composition,
    // REMOTION-PARITY-2026-09 REM3 -- the passage from one sequence of a
    // series to the next: how long they overlap (timing) and how each is
    // shown meanwhile (presentation).
    Transition,
};

// REM3 -- how a transition shows the two sequences it joins: a crossfade,
// the next pushing the previous out, a straight edge or a circle revealing
// the next over the previous.
enum class MotionPresentation { Fade, Slide, Wipe, Iris };

enum class MotionDirection { FromLeft, FromRight, FromTop, FromBottom };

// M1b -- text units, cut after layout.
enum class MotionTextUnit { None, Line, Word, Char };

enum class MotionStaggerOrder { Forward, Reverse, Random };

// Delays unit (or copy) i by rank(i) * each frames. Random ranks come from a
// seeded permutation: there is no unseeded randomness.
struct MotionStagger {
    bool present = false;
    int64_t each = 0;
    MotionStaggerOrder order = MotionStaggerOrder::Forward;
    uint64_t seed = 0;
};

// Units [start, start + count) of `unit` take `style`. Indices are zero-based
// and bounded by the text: a range past the end styles what exists.
struct MotionTextRange {
    MotionTextUnit unit = MotionTextUnit::Word;
    MotionValue start;
    std::optional<MotionValue> count;
    std::vector<std::pair<std::string, MotionValue>> style;
};

// MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. A highlighter stroke
// under units [start, start + count) of `unit`. A range's background is
// painted unit by unit, a hard box per word with the spaces left bare; this
// is one band per line, from the first covered unit to the last, spaces
// included, `bleed` pixels past each end, from `top` to `bottom` of the line
// box (fractions of its height), painted from the left in reading order --
// line after line -- up to `progress` of its whole length. The bands come
// from the boxes layout gives the units, so no position is written by hand.
// Separate from `ranges` on purpose: a highlight paints no unit, so it does
// not cut the text into units nor change how its glyphs are drawn.
struct MotionTextHighlight {
    MotionTextUnit unit = MotionTextUnit::Word;
    MotionValue start;
    std::optional<MotionValue> count;
    MotionValue color;
    // A number from 0 to 1 (or a binding); or `progress_animation`, an
    // interpolate or a spring read at the text's local frame. 1 when
    // neither is given.
    std::optional<MotionValue> progress;
    std::optional<MotionAnimation> progress_animation;
    double top = 0.0;
    double bottom = 1.0;
    double bleed = 0.0;
};

struct MotionViewBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct MotionNode {
    // The author's name for the node, unique in its composition.
    std::string id;
    // M3: the node's persistent identity in the document (ULID). Empty for a
    // composition read from a file that does not name one.
    std::string uid;
    MotionNodeType type = MotionNodeType::Box;
    std::vector<std::pair<std::string, MotionValue>> style;
    std::vector<std::pair<std::string, MotionAnimation>> animate;
    std::optional<MotionValue> visible;
    // M5b -- docs/roadmap/MOTION-2026-09.md. Decor: drawn, but not judged by
    // the safe-area check, nor its descendants -- the echoes of a marquee,
    // a wall of words, a texture the frame cuts on purpose. The check lists
    // what it left out, so nothing is silenced unseen.
    bool decor = false;
    std::optional<MotionValue> content;
    std::optional<MotionValue> src;
    // Sequence window in frames local to the enclosing sequence. Inside a
    // series, `from` is computed from the durations and offsets before it.
    int64_t from = 0;
    std::optional<int64_t> duration;
    int64_t offset = 0;
    // Speed of local time: a number, "N%" or a binding.
    std::optional<MotionValue> time_scale;
    // Text units, their cascade and their style ranges.
    MotionTextUnit units = MotionTextUnit::None;
    MotionStagger stagger;
    std::vector<MotionTextRange> ranges;
    // MB3 -- highlighter strokes under ranges of units.
    std::vector<MotionTextHighlight> highlights;
    // Path: SVG data (a literal is parsed once, here) or points.
    std::optional<MotionValue> path_data;
    MotionPath path;
    bool path_parsed = false;
    std::optional<MotionViewBox> view_box;
    // Repeat: a count or a list, one template, alternating styles.
    std::optional<MotionValue> count;
    std::optional<MotionValue> each;
    std::vector<std::pair<std::string, std::vector<MotionValue>>> cycle;
    // REM2 -- video and audio: the media, a project media by ULID (`media`)
    // or a file (`src`, like an image's), the frame of the composition's
    // cadence it starts from and the one it ends at, its speed, and whether
    // a video plays its sound. Media time at local frame t is
    // (startFrom + t * playbackRate) / fps, exact.
    std::optional<MotionValue> media;
    int64_t start_from = 0;
    std::optional<int64_t> end_at;
    // MB11 -- startFrom and endAt bound to a prop; start_from and end_at
    // then hold what the prop's default gives.
    std::optional<MotionValue> start_from_binding;
    std::optional<MotionValue> end_at_binding;
    std::optional<MotionValue> playback_rate;
    bool muted = false;
    // REM2 -- an instance: the composition it shows, by ULID, and the prop
    // values it gives it, each a literal or a binding of this composition.
    std::string instance_of;
    std::vector<std::pair<std::string, MotionValue>> instance_props;
    // REM3 -- a transition of a series: its overlap in frames (`duration`
    // above), how its progress runs over it -- an easing, or a spring from
    // 0 to 1 -- and its presentation.
    MotionPresentation presentation = MotionPresentation::Fade;
    MotionDirection direction = MotionDirection::FromRight;
    KeyframeInterpolation timing_interpolation = KeyframeInterpolation::Linear;
    KeyframeEasing timing_easing;
    bool timing_spring = false;
    MotionSpring timing_spring_config;
    // A repeat's template is its only child.
    std::vector<MotionNode> children;
};

struct MotionTokenTable {
    std::string name;
    std::vector<std::pair<std::string, mcp_json::Value>> entries;
};

// M6 -- one version of a composition: a named set of prop values (a date, a
// message), checked against the typed props like any value.
struct MotionVariant {
    std::string name;
    mcp_json::Value props;
};

// M6 -- another size the composition renders at (9:16, 16:9), with the prop
// values that adapt it to that size. Layout still comes from flexbox and
// percentages: a format names a canvas, it places nothing.
struct MotionFormat {
    std::string name;
    int32_t width = 0;
    int32_t height = 0;
    mcp_json::Value props;
};

struct MotionComposition {
    int32_t width = 0;
    int32_t height = 0;
    int32_t fps_numerator = 25;
    int32_t fps_denominator = 1;
    int64_t duration = 0;
    std::vector<MotionPropDefinition> props;
    std::vector<MotionTokenTable> tokens;
    // M6 -- in declaration order, which is the order of a batch render.
    std::vector<MotionVariant> variants;
    std::vector<MotionFormat> formats;
    MotionNode root;
    // MOTION-DESIGN-2026-09 -- motion blur of the whole composition, as a
    // camera shutter: each frame is the average of `motion_blur_samples`
    // instants spread over `motion_blur_angle` degrees of shutter (360: the
    // whole frame), the shutter opening on the frame itself so that a cut
    // on a frame never ghosts into its neighbour. 0: no blur.
    int32_t motion_blur_samples = 0;
    double motion_blur_angle = 180.0;
    // MOTION-DESIGN-2026-09 -- film grain over the finished frame, once,
    // after the shutter's instants are averaged: a new grain every frame.
    // 0: none.
    double grain_amount = 0.0;
    double grain_size = 1.0;
    int64_t grain_seed = 0;
};

struct MotionColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;
};

enum class MotionLengthUnit { Px, Percent, Auto };

struct MotionLength {
    MotionLengthUnit unit = MotionLengthUnit::Px;
    double value = 0.0;
};

const char* MotionNodeTypeName(MotionNodeType type);

// CSS colour spellings accepted in M1: #rgb, #rrggbb, #rrggbbaa, rgb(), rgba(),
// transparent, white, black. Anything else is refused rather than guessed.
bool ParseMotionColor(const std::string& text, MotionColor& color);

// A JSON number is pixels; a string may be "12px", "45%" or, where the
// property allows it, "auto".
bool ParseMotionLength(const mcp_json::Value& value, bool allow_auto,
                       MotionLength& length);

// Checks a prop value -- a default or a caller's override -- against its
// definition: type, enum membership, numeric bounds. M3 names the three
// refusals a caller acts on differently: PropTypeMismatch for a value of the
// wrong kind (a group has none), PropOutOfRange for one outside its bounds
// or list; UnknownProp is the evaluator's, for a name nothing declares.
bool ValidateMotionPropValue(const MotionPropDefinition& prop,
                             const mcp_json::Value& value,
                             const std::string& path, MotionError& error);

// Locale-independent reading of a validated JSON number literal.
bool MotionNumber(const mcp_json::Value& value, double& number);

// Validates one style value -- literal or already resolved from a binding --
// against the property it is assigned to. Shared by the parser and the
// evaluator so a bound value obeys exactly the rules a literal does.
bool ValidateMotionStyleValue(const std::string& property,
                              const mcp_json::Value& value,
                              const std::string& path, MotionError& error);

// Style properties whose value can be animated: numbers in the property's own
// unit (pixels, degrees, multipliers, opacity), colours and, from M1b,
// clip-path polygons.
bool IsAnimatableMotionProperty(const std::string& property);

// What an animation of `property` interpolates.
MotionValueKind MotionPropertyValueKind(const std::string& property);

// M1b -- CSS clip-path: polygon(x y, ...), each coordinate a length in px or
// a percentage of the node's border box.
using MotionPolygon = std::vector<std::pair<MotionLength, MotionLength>>;
bool ParseMotionPolygon(const std::string& text, MotionPolygon& polygon);

// REMOTION-PARITY-2026-09 REM3 -- the CSS basic shapes a clip-path takes:
// polygon() (M1b), circle(<r> at <x> <y>), ellipse(<rx> <ry> at <x> <y>),
// inset(<top> <right> <bottom> <left> round <radius>). A radius is a
// length, a percentage (of the reference CSS names: the box's diagonal
// over the square root of two for a circle, its width or height for an
// ellipse) or closest-side / farthest-side. Lengths as elsewhere, px or %.
enum class MotionClipKind { None, Polygon, Circle, Ellipse, Inset };
enum class MotionRadiusKeyword { None, ClosestSide, FarthestSide };

struct MotionClipShape {
    MotionClipKind kind = MotionClipKind::None;
    MotionPolygon polygon;
    MotionLength radius_x{MotionLengthUnit::Px, 0.0};
    MotionLength radius_y{MotionLengthUnit::Px, 0.0};
    MotionRadiusKeyword keyword_x = MotionRadiusKeyword::ClosestSide;
    MotionRadiusKeyword keyword_y = MotionRadiusKeyword::ClosestSide;
    MotionLength center_x{MotionLengthUnit::Percent, 50.0};
    MotionLength center_y{MotionLengthUnit::Percent, 50.0};
    // Inset: top, right, bottom, left, and the corner radius.
    MotionLength inset[4];
    MotionLength round{MotionLengthUnit::Px, 0.0};
};

bool ParseMotionClipShape(const std::string& text, MotionClipShape& shape);

// MOTION-MARQUE-2026-10 MM2 -- CSS font-variant-numeric: "normal", or
// lining-nums or oldstyle-nums (`figures`: "lining", "oldstyle") and/or
// tabular-nums or proportional-nums (`spacing`: "tabular",
// "proportional"), one of each at most. Empty: the font's default.
bool ParseMotionNumericVariant(const std::string& text, std::string& figures,
                               std::string& spacing);

// REM3 -- CSS gradients, the subset the corpus needs: linear-gradient()
// with an angle ("135deg") or a side or corner ("to top right"), and
// radial-gradient() with circle or ellipse, an extent keyword and a centre
// ("at 30% 40%"); stops are colours with an optional percentage.
enum class MotionGradientKind { Linear, Radial };

struct MotionGradientStop {
    MotionColor color;
    bool has_position = false;
    double position = 0.0;  // a fraction of the gradient line or ray
};

struct MotionGradient {
    MotionGradientKind kind = MotionGradientKind::Linear;
    // Linear: degrees, 0 towards the top, clockwise; 180 by default ("to
    // bottom"). A corner ("to top right") depends on the box: corner_x and
    // corner_y are -1, 0 or 1 (left/top, none, right/bottom).
    double angle = 180.0;
    int corner_x = 0;
    int corner_y = 0;
    // Radial.
    bool circle = false;
    std::string extent = "farthest-corner";
    MotionLength center_x{MotionLengthUnit::Percent, 50.0};
    MotionLength center_y{MotionLengthUnit::Percent, 50.0};
    std::vector<MotionGradientStop> stops;
};

bool ParseMotionGradient(const std::string& text, MotionGradient& gradient);

// Stop positions CSS fills in: the first at 0, the last at 1, the ones
// between spread evenly between their positioned neighbours, and each at
// least the one before it.
std::vector<double> MotionGradientPositions(const MotionGradient& gradient);

// REM3 -- the raster effects of a node, applied in order to what it paints
// (itself and its descendants), as CSS `filter` is: blur(<σ>), drop-shadow(
// <dx> <dy> <σ> <colour>), and two CSS does not have, named for what they
// do: glow(<σ> <colour> <strength>) -- a halo of the node's own shape, its
// coverage multiplied by strength and bounded at full -- and displace(
// <amplitude> <wavelength> <seed> <phase>) -- each pixel moved by a smooth
// noise, seeded, never random. Lengths are pixels; σ is a standard
// deviation.
//
// MOTION-DESIGN-2026-09 -- grain(<amount> [<size>px] [<seed>] [<phase>]):
// film grain, a seeded luminance noise of cells `size` pixels wide added in
// proportion to the node's coverage, `amount` of full scale at most (0 to
// 1). The noise changes with the whole part of `phase` only, so that every
// instant of one motion-blurred frame shows the same grain. It reuses
// amplitude (amount), wavelength (size), seed and phase.
//
// MOTION-DESIGN-2026-09 -- goo(<σ>px): the node blurred, then its coverage
// pushed back to hard edges, so that shapes near each other melt together
// (metaballs). The edge lies where the blurred coverage crosses one half.
//
// MOTION-DESIGN-2026-09 -- slice(<amplitude>px <band>px <seed> [<phase>]):
// the node cut into horizontal bands about `band` pixels high, each band
// split once at a seeded height, and a seeded part of the slices shifted
// sideways by up to `amplitude` pixels, whole pixels, so a glitch stays
// crisp. Like grain, only the whole part of `phase` changes the pattern.
// It reuses amplitude, wavelength (band), seed and phase.
//
// MOTION-DESIGN-2026-09 -- mesh(<rows> <columns> [<quality>], <x> <y>,
// ...): After Effects' Mesh Warp. The node's box is a grid of rows x
// columns cells whose (rows + 1) x (columns + 1) vertices are written row
// by row, top-left first, each in pixels or percentages of the box; a
// vertex written where it rests leaves the picture as it is. Each cell is a
// Coons patch bounded by four cubic Béziers whose handles sit a third of
// the resting cell away from their vertex and travel with it -- what After
// does until someone drags a handle -- so a whole grid line moved keeps the
// lines straight and stretches the cells on either side smoothly.
// `quality` (1 to 10, After's) sets how finely each cell is tessellated.
enum class MotionFilterKind {
    Blur,
    DropShadow,
    Glow,
    Displace,
    Grain,
    Goo,
    Slice,
    Mesh
};

struct MotionFilter {
    MotionFilterKind kind = MotionFilterKind::Blur;
    double sigma = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;
    MotionColor color;
    double strength = 1.0;
    double amplitude = 0.0;
    double wavelength = 1.0;
    int64_t seed = 0;
    double phase = 0.0;
    // mesh(): the grid, and its vertices as written.
    int32_t mesh_rows = 0;
    int32_t mesh_columns = 0;
    int32_t mesh_quality = 8;
    std::vector<std::pair<MotionLength, MotionLength>> mesh_points;
    // mesh(), once the evaluator knows the node's box: the box, and the
    // vertices in the node's own pixels (the space of its box).
    double mesh_box_x = 0.0;
    double mesh_box_y = 0.0;
    double mesh_box_width = 0.0;
    double mesh_box_height = 0.0;
    std::vector<std::pair<double, double>> mesh_resolved;
};

// The resting place of vertex (row, column) of a mesh() whose box is known.
void MotionMeshRest(const MotionFilter& filter, int32_t row, int32_t column,
                    double& x, double& y);

// The vertices of every mesh() of `filters` in the pixels of `box` (x, y,
// width, height): percentages taken of its width or height, from its corner.
void ResolveMotionMeshes(std::vector<MotionFilter>& filters, double box_x,
                         double box_y, double box_width, double box_height);

bool ParseMotionFilters(const std::string& text,
                        std::vector<MotionFilter>& filters);

// MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. strokeDasharray: "none", or
// lengths in pixels -- a number, or one ending in "px" -- none negative,
// separated by spaces or commas, as SVG writes them (its other units
// aside). The lengths as written in `lengths`, empty for none. Anything
// else is false: the evaluator used to read up to the first word it could
// not, and drew "10px 5px" as 10 on, 10 off.
bool ParseMotionDashArray(const std::string& text,
                          std::vector<double>& lengths);

// MOTION-WEB-2026-10 MW5 -- ROADMAP.md. CSS transform-origin in the plane:
// where a node's scale, rotate and skew turn, a fraction of its box plus
// pixels on each axis. The centre by default, as CSS. Written once, from
// the web (GSAP's transformOrigin, 68 times in MW0) and from After, whose
// anchor point it is.
struct MotionTransformOrigin {
    double x_fraction = 0.5;
    double x_pixels = 0.0;
    double y_fraction = 0.5;
    double y_pixels = 0.0;

    bool IsCentre() const {
        return x_fraction == 0.5 && x_pixels == 0.0 && y_fraction == 0.5 &&
               y_pixels == 0.0;
    }
};

// One or two values, as CSS writes them: keywords (left, center, right;
// top, center, bottom; two keywords in either order), percentages, or
// pixels ("12px", or 0). One value sets x, or y for top and bottom; the
// other stays at the centre. A third value (CSS's depth) is refused: the
// plane has none.
bool ParseMotionTransformOrigin(const std::string& text,
                                MotionTransformOrigin& origin);

// Exact rational reading of a decimal literal ("1.25" is 5/4), so a speed or
// a percentage never enters time as a rounded float.
bool MotionDecimalRational(const std::string& text, int64_t& numerator,
                           int64_t& denominator);

// M1b -- a sequence's timeScale: a positive number (a multiplier) or "N%",
// read as an exact fraction. At most 1000.
bool ParseMotionTimeScale(const mcp_json::Value& value, int64_t& numerator,
                          int64_t& denominator);

// Properties an `animate` of a text cut into units applies to each unit:
// what can change after layout without moving anything else.
bool IsMotionUnitProperty(const std::string& property);

bool ParseMotionComposition(const std::string& json,
                            MotionComposition& composition, MotionError& error);
// M3 -- the same, from a tree already parsed: a composition stored in the
// document is kept as one.
bool ParseMotionComposition(const mcp_json::Value& root,
                            MotionComposition& composition, MotionError& error);

// Frame `frame` of a composition at `fps_numerator / fps_denominator` as an
// exact time. Frames are the authoring unit; RationalTime is the evaluated one.
RationalTime MotionFrameTime(const MotionComposition& composition,
                             int64_t frame);

// MOTION-WEB-2026-10 MW1 -- a key between two frames. An animation imported
// from the web is timed in decimal seconds (0.35 s is frame 21/2 at 30 fps);
// rounding it to a frame would move every key silently, so `frames` also
// accepts an exact fraction, written "n/d". The spelling is canonical: an
// integer stays a JSON integer, a fraction is reduced with d > 1. Anything
// else (a decimal number, "4/2", "1/0") is refused.
bool ParseMotionKeyFrame(const mcp_json::Value& value, int64_t& numerator,
                         int64_t& denominator);

// Frame `numerator / denominator` as an exact time; false when its rate does
// not fit RationalTime.
bool MotionFrameTime(const MotionComposition& composition, int64_t numerator,
                     int64_t denominator, RationalTime& time);

// MOTION-2026-09 M6 -- the composition as version `variant` renders in format
// `format`: `resolved` takes the format's size, and `props` the values the
// format sets, then the version's, then `overrides` (a caller's, for one
// frame). The parser refuses a prop that a format and a version both set, so
// only a caller's override ever replaces one. An empty name keeps the
// composition's own size or values; a name it does not declare is refused as
// UnknownVariant or UnknownFormat, with the declared names.
bool ResolveMotionVariant(const MotionComposition& composition,
                          const std::string& variant, const std::string& format,
                          const mcp_json::Value& overrides,
                          MotionComposition& resolved, mcp_json::Value& props,
                          MotionError& error);

// M6 -- a variant or format name: 1 to 64 ASCII letters, digits, '-' or '_',
// since it becomes part of a rendered file's name.
bool IsMotionSetName(const std::string& name);
