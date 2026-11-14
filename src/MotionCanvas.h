#pragma once

// REMOTION-PARITY-2026-09 REM5 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// The visual editing of a composition, as policy: what a click selects,
// where a selection is drawn, and the operation a gesture commits.
//
// Two rules of the lot hold here. The boxes a click selects are the
// evaluated frame's, through the same transforms the painter applies
// (MotionGeometry.h): no second clickable geometry (constraint 6). And a
// gesture commits one operation of those that exist -- a whole new
// definition (UpdateMotionComposition) when it changes several properties,
// so that one undo takes the whole gesture back (constraint 2); what the
// interface shows while the mouse is down stays in the interface.
//
// A property that is animated by keys is edited by its key at the frame
// shown -- set if there is one, added otherwise -- and a property that is
// not is set; `keyed` asks for a key where there is none yet. The same
// rules serve the agent: edit_motion_composition's `key`, `unkey` and
// `ease` are built from them (McpMotionTools.cc).
//
// Portable: no framework. main.mm only turns mouse events into calls here.

#include "ClipFraming.h"
#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionGeometry.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

struct MotionHit {
    std::string uid;
    std::string path;
    // The instance nodes the hit sits in, outermost first: empty for a
    // node of the composition itself.
    std::vector<std::string> instances;
    // Its box in frame pixels: top-left, top-right, bottom-right,
    // bottom-left.
    std::pair<double, double> quad[4];
};

// The node under (x, y), frame pixels, as painted: a later sibling over an
// earlier one, a child over its parent; what a clip-path, a transition's
// wipe or an overflow: hidden ancestor hides, and a node at zero opacity,
// are passed through. Sounds have no box. With `into_instances` false, a
// click inside an instance selects the instance itself. False when the
// point falls on nothing.
bool MotionHitTest(const MotionFrame& frame, double x, double y,
                   bool into_instances, MotionHit& hit);

// Where the node `uid` of the composition itself is drawn at this frame
// (its first copy, for a repeat's template). False when it is not shown.
bool MotionNodeQuad(const MotionFrame& frame, const std::string& uid,
                    std::pair<double, double> quad[4]);

// The eight handles of a selection, corners then edge middles
// (top-left, top-right, bottom-right, bottom-left, top, right, bottom,
// left), in frame pixels.
std::vector<std::pair<double, double>> MotionSelectionHandles(
    const std::pair<double, double> quad[4]);

// One property of one node changed by a gesture: its new value, and
// whether it is to be keyed at `frame` (a frame of the composition).
struct MotionPropertyEdit {
    std::string uid;
    std::string property;
    mcp_json::Value value;
    bool keyed = false;
    int64_t frame = 0;
};

// `animation` (an `animate` entry, or null for a property not animated)
// with a key at `frame` holding `value`: the key there moved to the new
// value, or a key added in order with a fresh identity, the easing kept:
// where it lists a curve per segment (MB5), a segment cut in two keeps its
// curve on both halves, and a new outer segment takes its neighbour's.
// Refused when the property moves by a spring, which has no keys.
bool MotionAnimationWithKey(const mcp_json::Value* animation, int64_t frame,
                            const mcp_json::Value& value,
                            mcp_json::Value& output, std::string& error);

// The same without the key at `frame`; empty when it was the last one.
// Of a curve per segment, the merged segment keeps the one that left the
// key before it. Refused when there is no key there.
bool MotionAnimationWithoutKey(const mcp_json::Value& animation, int64_t frame,
                               std::optional<mcp_json::Value>& output,
                               std::string& error);

// The same with another easing on every segment, spelled as interpolate
// takes it ("ease-in-out", "cubic-bezier(0.2, 0, 0, 1)") -- or, MB5, on
// the one segment leaving the key at `frame`, the others keeping theirs:
// the easing then becomes a list, unless every segment ends up the same.
bool MotionAnimationWithEasing(const mcp_json::Value& animation,
                               const std::string& easing,
                               mcp_json::Value& output, std::string& error,
                               std::optional<int64_t> frame = std::nullopt);

// `definition` with every edit made: a key where the property is animated
// by keys or `keyed` asks for one, the style value otherwise, and the style
// removed for a null value (a field emptied). The motion parser judges the
// result when the operation is applied.
bool MotionDefinitionWithEdits(const mcp_json::Value& definition,
                               const std::vector<MotionPropertyEdit>& edits,
                               mcp_json::Value& output, std::string& error);

// A drag of the node `uid` by (dx, dy) frame pixels: its translateX and
// translateY, moved by what that drag is in its parent's space, at `frame`.
bool MotionMoveEdits(const MotionFrame& frame,
                     const mcp_json::Value& definition, const std::string& uid,
                     int64_t compositionFrame, double dx, double dy,
                     std::vector<MotionPropertyEdit>& edits,
                     std::string& error);

// A drag of handle `handle` (MotionSelectionHandles' order) by (dx, dy)
// frame pixels: the node's width and height, grown or shrunk by what that
// drag is along its own axes. Where the box then sits is flexbox's to say,
// as it is for any size.
bool MotionResizeEdits(const MotionFrame& frame,
                       const mcp_json::Value& definition,
                       const std::string& uid, int64_t compositionFrame,
                       int handle, double dx, double dy,
                       std::vector<MotionPropertyEdit>& edits,
                       std::string& error);

// A decimal for a style value, as short as it reads back to the same
// double, and never "-0".
mcp_json::Value MotionNumberValue(double value);

// REM5 -- one row of the Inspector for a selected node: what it edits,
// what it shows now, and how it is animated. The view draws the rows it is
// given and hands back the value typed; it decides nothing.
enum class MotionFieldKind { Number, Color, Text, Choice };

struct MotionField {
    // A style property, or a field of the node itself: "content",
    // "duration", "from", "presentation", "direction", "timing.duration",
    // "timing.easing".
    std::string property;
    std::string label;
    std::string group;
    MotionFieldKind kind = MotionFieldKind::Number;
    // The value shown at this frame, as the field spells it: a number as
    // short as it reads, a colour as #rrggbb[aa], a text, a keyword.
    std::string text;
    std::vector<std::string> choices;
    // False for a text bound to a prop: it is edited as that prop.
    bool editable = true;
    // A style property animation can drive, keyed or by a spring.
    bool keyable = false;
    bool animated = false;
    bool key_here = false;
    bool spring = false;
    std::string easing;
    // A spring's settings, when `spring`.
    double spring_from = 0.0;
    double spring_to = 0.0;
    int64_t spring_delay = 0;
    double spring_damping = 10.0;
};

// The rows for node `uid` at composition frame `compositionFrame`, in
// display order: text and its cascade (MA6), then transformation,
// appearance, layout, and for a sequence or a transition their time.
bool MotionNodeFields(const mcp_json::Value& definition,
                      const MotionFrame& frame, const std::string& uid,
                      int64_t compositionFrame,
                      std::vector<MotionField>& fields, std::string& error);

// `definition` with a field of node `uid` itself set -- `content`,
// `duration`, `presentation`, `direction`, `timing.duration`,
// `timing.easing` -- or removed when `value` is empty.
bool MotionDefinitionWithNodeField(const mcp_json::Value& definition,
                                   const std::string& uid,
                                   const std::string& field,
                                   const std::optional<mcp_json::Value>& value,
                                   mcp_json::Value& output, std::string& error);

// The value typed or chosen in the Inspector row `field` of node `uid`, as
// the definition the operation carries: a field of the node itself
// (MotionDefinitionWithNodeField), a setting of a text's cascade -- MA6,
// docs/roadmap/MOTION-AFTER-2026-09.md: "units", "stagger.each",
// "stagger.order", "stagger.seed", and "animate.easing", the curve every
// animation of its units then shares -- or a style, keyed at
// `compositionFrame` when the property is animated by keys. An empty text
// asks for the default. Refused, with a message for the status line, when
// the text is not what the row takes.
bool MotionDefinitionWithTypedField(
    const mcp_json::Value& definition, const std::string& uid,
    const MotionField& field, const std::string& text, int64_t compositionFrame,
    mcp_json::Value& output, std::string& error);

// `definition` with the animation of `property` on `uid` replaced by a
// spring from `from` to `to`, `delay` frames late, `damping` as Remotion
// defines it -- or removed when `spring` is false and the property then
// holds `from` as its style value.
bool MotionDefinitionWithSpring(const mcp_json::Value& definition,
                                const std::string& uid,
                                const std::string& property, bool spring,
                                double from, double to, int64_t delay,
                                double damping, mcp_json::Value& output,
                                std::string& error);

// What a menu inserts: a node of `type` -- text, box, image, sequence --
// with the defaults an author starts from, and no identity yet (the
// operation gives it one).
mcp_json::Value MotionNewNode(const std::string& type);

// A transition node: presentation, direction (slide and wipe), duration in
// frames, easing.
mcp_json::Value MotionNewTransition(const std::string& presentation,
                                    const std::string& direction,
                                    int64_t duration,
                                    const std::string& easing);

// A composition to start from: a canvas of the timeline's size, cadence
// and `duration`, a root `fill` -- or a `series` of scenes when `scenes`.
mcp_json::Value MotionNewComposition(int32_t width, int32_t height,
                                     int32_t fpsNumerator,
                                     int32_t fpsDenominator, int64_t duration,
                                     bool scenes);

// `definition` with `node` (no identity needed) inserted as the last child
// of `parentUid`, or after its sibling `afterUid` when that is given. The
// identities are given when the composition is judged.
bool MotionDefinitionWithChild(const mcp_json::Value& definition,
                               const std::string& parentUid,
                               const std::string& afterUid,
                               const mcp_json::Value& node,
                               mcp_json::Value& output, std::string& error);

// `definition` with one more scene at the end of series `seriesUid`: the
// scene that was last is given `duration` frames when it had no end, and
// the new one is `scene` (a sequence node).
bool MotionDefinitionWithScene(const mcp_json::Value& definition,
                               const std::string& seriesUid,
                               const mcp_json::Value& scene, int64_t duration,
                               mcp_json::Value& output, std::string& error);

// REM5 -- where a motion clip's composition sits in the Record monitor:
// composition pixels to the monitor's points, rows from the top, and back.
// Two steps, both the ones the picture takes: the clip's framing (the
// rectangle of the sequence the export draws the composition in,
// ClipFraming.h) and the monitor's fit of the sequence frame in its view
// (the canvas CaptionOverlayLayout.h computes for the captions).
struct MotionMonitorMapping {
    double scale_x = 1.0;
    double scale_y = 1.0;
    double offset_x = 0.0;
    double offset_y = 0.0;

    void ToView(double x, double y, double& viewX, double& viewY) const;
    void FromView(double viewX, double viewY, double& x, double& y) const;
};

MotionMonitorMapping MotionMonitorMappingFor(const ClipFramingGeometry& framing,
                                             double canvasX, double canvasY,
                                             double canvasScale);

// `definition` with the animation of `property` on `uid` set to
// `animation` (as MotionAnimationWithKey and its kin build it), or removed
// when it is empty.
bool MotionDefinitionWithAnimation(
    const mcp_json::Value& definition, const std::string& uid,
    const std::string& property,
    const std::optional<mcp_json::Value>& animation, mcp_json::Value& output,
    std::string& error);
