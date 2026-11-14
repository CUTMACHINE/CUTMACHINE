#include "MotionSafeArea.h"

#include "Json.h"

#include <algorithm>
#include <cmath>

namespace {

// A 2D affine map, x' = a x + c y + e, y' = b x + d y + f -- the order Core
// Graphics composes its CTM in, which is what the renderer applies.
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Affine Then(const Affine& inner) const {
        // this ∘ inner: inner is applied first.
        Affine out;
        out.a = a * inner.a + c * inner.b;
        out.b = b * inner.a + d * inner.b;
        out.c = a * inner.c + c * inner.d;
        out.d = b * inner.c + d * inner.d;
        out.e = a * inner.e + c * inner.f + e;
        out.f = b * inner.e + d * inner.f + f;
        return out;
    }
    void Apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
};

Affine Translation(double x, double y) {
    Affine m;
    m.e = x;
    m.f = y;
    return m;
}

// The renderer's Transform (MotionRender.mm): translate to the centre plus
// the node's translation, rotate, scale, translate back.
Affine NodeTransform(const MotionEvaluatedNode& node) {
    const double centerX = node.box.x + (node.box.width / 2.0);
    const double centerY = node.box.y + (node.box.height / 2.0);
    const double radians = node.rotate * M_PI / 180.0;
    Affine rotation;
    rotation.a = std::cos(radians);
    rotation.b = std::sin(radians);
    rotation.c = -std::sin(radians);
    rotation.d = std::cos(radians);
    Affine scale;
    scale.a = node.scale_x;
    scale.d = node.scale_y;
    return Translation(centerX + node.translate_x, centerY + node.translate_y)
        .Then(rotation)
        .Then(scale)
        .Then(Translation(-centerX, -centerY));
}

MotionBox Bounds(const Affine& transform, double x, double y, double width,
                 double height) {
    double xs[4];
    double ys[4];
    transform.Apply(x, y, xs[0], ys[0]);
    transform.Apply(x + width, y, xs[1], ys[1]);
    transform.Apply(x, y + height, xs[2], ys[2]);
    transform.Apply(x + width, y + height, xs[3], ys[3]);
    const double left = *std::min_element(xs, xs + 4);
    const double right = *std::max_element(xs, xs + 4);
    const double top = *std::min_element(ys, ys + 4);
    const double bottom = *std::max_element(ys, ys + 4);
    return {left, top, right - left, bottom - top};
}

// REM2 / M5b -- what `clip` leaves of `bounds`; false when nothing: the
// element is wholly hidden by an ancestor's overflow: hidden.
bool Intersect(const MotionBox& bounds, const MotionBox& clip, MotionBox& out) {
    const double x0 = std::max(bounds.x, clip.x);
    const double y0 = std::max(bounds.y, clip.y);
    const double x1 = std::min(bounds.x + bounds.width, clip.x + clip.width);
    const double y1 = std::min(bounds.y + bounds.height, clip.y + clip.height);
    if (x1 - x0 <= 0.0 || y1 - y0 <= 0.0) {
        return false;
    }
    out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

MotionBox Union(const MotionBox& left, const MotionBox& right) {
    if (left.width <= 0 && left.height <= 0) {
        return right;
    }
    const double x0 = std::min(left.x, right.x);
    const double y0 = std::min(left.y, right.y);
    const double x1 = std::max(left.x + left.width, right.x + right.width);
    const double y1 = std::max(left.y + left.height, right.y + right.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

void Record(MotionSafeAreaReport& report, MotionSafeAreaIssue issue) {
    for (MotionSafeAreaIssue& existing : report.issues) {
        // MM3 -- a node with neither id nor ULID is told apart by its path.
        if (existing.kind == issue.kind && existing.uid == issue.uid &&
            existing.node_id == issue.node_id &&
            (!issue.uid.empty() || !issue.node_id.empty() ||
             existing.path == issue.path)) {
            existing.last_frame = issue.last_frame;
            existing.at_rest = existing.at_rest || issue.at_rest;
            for (const std::string& edge : issue.edges) {
                if (std::find(existing.edges.begin(), existing.edges.end(),
                              edge) == existing.edges.end()) {
                    existing.edges.push_back(edge);
                }
            }
            return;
        }
    }
    report.issues.push_back(std::move(issue));
}

// Half a pixel of tolerance: bounds are floating layout, and an element
// exactly on the edge of the safe area is inside it.
constexpr double kTolerance = 0.5;

// The edges of the safe area `bounds` crosses. With `bleed`, a side that
// reaches the frame's own edge is not one: a background run off the frame is
// drawn that way on purpose, one that stops between the safe area and the
// frame's edge is the mistake the margin is there to catch.
std::vector<std::string> CrossedEdges(const MotionBox& bounds,
                                      const MotionBox& safe, double width,
                                      double height, bool bleed) {
    std::vector<std::string> edges;
    if (bounds.width <= 0 && bounds.height <= 0) {
        return edges;
    }
    const double right = bounds.x + bounds.width;
    const double bottom = bounds.y + bounds.height;
    if (bounds.x < safe.x - kTolerance && !(bleed && bounds.x <= kTolerance)) {
        edges.push_back("left");
    }
    if (right > safe.x + safe.width + kTolerance &&
        !(bleed && right >= width - kTolerance)) {
        edges.push_back("right");
    }
    if (bounds.y < safe.y - kTolerance && !(bleed && bounds.y <= kTolerance)) {
        edges.push_back("top");
    }
    if (bottom > safe.y + safe.height + kTolerance &&
        !(bleed && bottom >= height - kTolerance)) {
        edges.push_back("bottom");
    }
    return edges;
}

// MM3 -- where each line of `lines` ends in `text`, the whole text: a line
// the text itself breaks after (a line feed, a space) was not broken by
// Core Text. "12 480\n9 731" laid out on two lines is two lines, not
// "480|9".
std::vector<bool> BrokenByText(const std::string& text,
                               const std::vector<MotionTextLine>& lines) {
    std::vector<bool> broken(lines.size(), false);
    size_t at = 0;
    for (size_t index = 0; index < lines.size(); ++index) {
        const size_t found = text.find(lines[index].text, at);
        if (found == std::string::npos) {
            continue;
        }
        at = found + lines[index].text.size();
        if (at < text.size() &&
            (text[at] == '\n' || text[at] == ' ' || text[at] == '\t')) {
            broken[index] = true;
        }
    }
    return broken;
}

// Whether a line ends where the text may break: after a space or a line
// feed, or after a hyphen or a dash. Core Text breaks a word too long for its
// box between two characters; the line it leaves ends on neither.
bool EndsOnBreak(const std::string& line) {
    if (line.empty()) {
        return true;
    }
    static const char* const kBreaks[] = {" ", "\n",     "\t",     "-",
                                          "/", "\u2010", "\u2013", "\u2014"};
    for (const char* ending : kBreaks) {
        const std::string text = ending;
        if (line.size() >= text.size() &&
            line.compare(line.size() - text.size(), text.size(), text) == 0) {
            return true;
        }
    }
    return false;
}

struct Walker {
    double width = 0;
    double height = 0;
    int64_t frame = 0;
    MotionSafeAreaReport& report;

    // What each element showed this frame, for the next to compare with.
    std::map<std::string, MotionBox>& current;

    // Whether the element `key` stands where it stood on the frame checked
    // before; remembered for the next either way.
    bool Still(const std::string& key, const MotionBox& bounds) {
        current[key] = bounds;
        const auto found = report.previous_bounds.find(key);
        if (found == report.previous_bounds.end()) {
            return false;
        }
        const MotionBox& before = found->second;
        return std::abs(before.x - bounds.x) <= kTolerance &&
               std::abs(before.y - bounds.y) <= kTolerance &&
               std::abs(before.width - bounds.width) <= kTolerance &&
               std::abs(before.height - bounds.height) <= kTolerance;
    }

    // Whether `bounds` lie wholly off the frame: nothing of the element
    // is on screen.
    bool OffFrame(const MotionBox& bounds) const {
        return bounds.x + bounds.width <= kTolerance ||
               bounds.y + bounds.height <= kTolerance ||
               bounds.x >= width - kTolerance ||
               bounds.y >= height - kTolerance;
    }

    // `raw`, the element's bounds, judged on what `clip` (the nearest
    // overflow: hidden ancestor, in composition pixels; none when null)
    // leaves of them.
    void Outside(const MotionEvaluatedNode& node, std::string text,
                 const MotionBox& raw, bool bleed, const std::string& what,
                 const MotionBox* clip) {
        MotionBox bounds = raw;
        // M5b -- what an overflow: hidden ancestor hides is not seen: the
        // check reads the visible part, as the renderer paints it (REM2).
        const bool hidden = clip != nullptr && !Intersect(raw, *clip, bounds);
        const bool still = Still(
            what + "|" + node.uid + "|" + node.id + "|" + node.path, bounds);
        // M5b -- an element wholly off the frame is not seen either: a band
        // waiting beyond the edge before it enters is neither at fault nor
        // a passage. Its bounds are still remembered, so that its entrance
        // reads as motion.
        if (hidden || OffFrame(bounds)) {
            return;
        }
        std::vector<std::string> edges =
            CrossedEdges(bounds, report.safe, width, height, bleed);
        if (edges.empty()) {
            return;
        }
        if (report.at_rest_only && !still) {
            ++report.in_motion;
            return;
        }
        MotionSafeAreaIssue outside;
        outside.kind = "outside_safe_area";
        outside.node_id = node.id;
        outside.uid = node.uid;
        outside.path = node.path;
        outside.at_rest = still;
        outside.text = std::move(text);
        outside.edges = std::move(edges);
        outside.first_frame = frame;
        outside.last_frame = frame;
        outside.bounds = bounds;
        Record(report, std::move(outside));
    }

    void Walk(const MotionEvaluatedNode& node, const Affine& parent,
              double parentOpacity, const MotionBox* clip) {
        // M5b -- decor is drawn, not judged, and said: the report names
        // what its verdict leaves out.
        if (node.decor) {
            const std::string name = node.id.empty() ? node.path : node.id;
            if (std::find(report.decor.begin(), report.decor.end(), name) ==
                report.decor.end()) {
                report.decor.push_back(name);
            }
            return;
        }
        const double opacity = parentOpacity * node.opacity;
        if (opacity <= 0.0 || node.scale_x == 0.0 || node.scale_y == 0.0) {
            return;
        }
        const Affine transform = parent.Then(NodeTransform(node));
        const bool seen = opacity >= 0.5;
        // REM2 -- overflow: hidden clips the descendants to the node's box,
        // not the node's own background. Rotated, the box's bounds stand
        // in for it: the check then reads a little more than is painted,
        // never less.
        MotionBox own{0, 0, 0, 0};
        const MotionBox* childClip = clip;
        if (node.overflow_hidden) {
            own = Bounds(transform, node.box.x, node.box.y, node.box.width,
                         node.box.height);
            if (clip != nullptr && !Intersect(own, *clip, own)) {
                own = {0, 0, 0, 0};
            }
            childClip = &own;
        }
        if (seen && node.has_background &&
            node.background.alpha * opacity >= 0.5) {
            const MotionBox bounds = Bounds(transform, node.box.x, node.box.y,
                                            node.box.width, node.box.height);
            Outside(node, "", bounds, true, "background", clip);
        }
        if (seen && node.type == MotionNodeType::Text) {
            MotionBox bounds{0, 0, 0, 0};
            for (const MotionTextLine& line : node.text_layout.lines) {
                bounds = Union(bounds, Bounds(transform, node.box.x + line.x,
                                              node.box.y + line.y, line.width,
                                              line.height));
                if (line.width > node.box.width + kTolerance) {
                    MotionSafeAreaIssue overflow;
                    overflow.kind = "text_overflows_box";
                    overflow.node_id = node.id;
                    overflow.uid = node.uid;
                    overflow.path = node.path;
                    overflow.text = node.text;
                    overflow.first_frame = frame;
                    overflow.last_frame = frame;
                    overflow.bounds = Bounds(transform, node.box.x, node.box.y,
                                             node.box.width, node.box.height);
                    Record(report, std::move(overflow));
                }
            }
            const std::vector<MotionTextLine>& lines = node.text_layout.lines;
            const std::vector<bool> broken = BrokenByText(node.text, lines);
            for (size_t index = 0; index + 1 < lines.size(); ++index) {
                if (EndsOnBreak(lines[index].text) || broken[index] ||
                    lines[index + 1].text.empty()) {
                    continue;
                }
                // The two pieces of the word, split at '|'.
                const std::string& before = lines[index].text;
                const std::string& after = lines[index + 1].text;
                const size_t head = before.find_last_of(" \n\t");
                const size_t tail = after.find_first_of(" \n\t");
                MotionSafeAreaIssue split;
                split.kind = "word_split";
                split.node_id = node.id;
                split.uid = node.uid;
                split.path = node.path;
                split.text =
                    (head == std::string::npos ? before
                                               : before.substr(head + 1)) +
                    "|" + after.substr(0, tail);
                split.first_frame = frame;
                split.last_frame = frame;
                split.bounds = Bounds(transform, node.box.x, node.box.y,
                                      node.box.width, node.box.height);
                Record(report, std::move(split));
            }
            Outside(node, node.text, bounds, false, "text", clip);
        } else if (seen && node.type == MotionNodeType::Image) {
            const MotionBox bounds = Bounds(transform, node.box.x, node.box.y,
                                            node.box.width, node.box.height);
            Outside(node, node.image_source, bounds, false, "image", clip);
        }
        for (const MotionEvaluatedNode& child : node.children) {
            Walk(child, transform, opacity, childClip);
        }
    }
};

mcp_json::Value Box(const MotionBox& box) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("x", mcp_json::Value::MakeInt(std::lround(box.x)));
    value.Set("y", mcp_json::Value::MakeInt(std::lround(box.y)));
    value.Set("width", mcp_json::Value::MakeInt(std::lround(box.width)));
    value.Set("height", mcp_json::Value::MakeInt(std::lround(box.height)));
    return value;
}

// One side's margin in pixels. The product first, in double, then the
// division: for the default zone that is the arithmetic the single
// margin_percent was checked with, so its rectangle keeps its pixels.
double MarginPixels(int32_t extent, const MotionSafeMargin& margin) {
    if (margin.den <= 0) {
        return 0.0;
    }
    return static_cast<double>(extent) * static_cast<double>(margin.num) /
           static_cast<double>(margin.den);
}

// The side as a percentage, to two decimals, floored: 14, 6, 12.96. Written
// from integers, so a report says the same thing on every host.
std::string PercentText(const MotionSafeMargin& margin) {
    if (margin.den <= 0 || margin.num < 0) {
        return "0";
    }
    const int64_t hundredths = margin.num * 10000 / margin.den;
    std::string text = std::to_string(hundredths / 100);
    const int64_t fraction = hundredths % 100;
    if (fraction != 0) {
        text += '.';
        text += static_cast<char>('0' + (fraction / 10));
        if (fraction % 10 != 0) {
            text += static_cast<char>('0' + (fraction % 10));
        }
    }
    return text;
}

// Whether `margin` is a whole percentage, written to `percent`.
bool WholePercent(const MotionSafeMargin& margin, int64_t& percent) {
    if (margin.den <= 0 || (margin.num * 100) % margin.den != 0) {
        return false;
    }
    percent = margin.num * 100 / margin.den;
    return true;
}

}  // namespace

MotionSafeZone DefaultMotionSafeZone(int64_t percent) {
    MotionSafeZone zone;
    zone.name = "default";
    zone.top = zone.bottom = zone.left = zone.right = {percent, 100};
    return zone;
}

MotionSafeZone SocialVerticalMotionSafeZone() {
    MotionSafeZone zone;
    zone.name = "social_vertical";
    zone.top = {14, 100};
    zone.bottom = {35, 100};
    zone.left = {6, 100};
    // TikTok's rail, 140 px of a 1080 px frame: tighter than the Reels 6 %.
    zone.right = {140, 1080};
    return zone;
}

bool NamedMotionSafeZone(const std::string& name, MotionSafeZone& zone) {
    if (name == "default") {
        zone = DefaultMotionSafeZone();
        return true;
    }
    if (name == "social_vertical") {
        zone = SocialVerticalMotionSafeZone();
        return true;
    }
    return false;
}

bool IsNineBySixteen(int64_t width, int64_t height) {
    return width > 0 && height > 0 && width * 16 == height * 9;
}

MotionSafeZone TimelineMotionSafeZone(int64_t width, int64_t height) {
    return IsNineBySixteen(width, height) ? SocialVerticalMotionSafeZone()
                                          : DefaultMotionSafeZone();
}

std::string DescribeMotionSafeZone(const MotionSafeZone& zone) {
    const std::string top = PercentText(zone.top);
    if (top == PercentText(zone.bottom) && top == PercentText(zone.left) &&
        top == PercentText(zone.right)) {
        return top + " % on each side";
    }
    return "top " + top + " %, bottom " + PercentText(zone.bottom) +
           " %, left " + PercentText(zone.left) + " %, right " +
           PercentText(zone.right) + " %";
}

void CheckMotionFrameSafeArea(const MotionFrame& frame,
                              MotionSafeAreaReport& report) {
    const double left = MarginPixels(frame.width, report.zone.left);
    const double right = MarginPixels(frame.width, report.zone.right);
    const double top = MarginPixels(frame.height, report.zone.top);
    const double bottom = MarginPixels(frame.height, report.zone.bottom);
    report.safe = {left, top, frame.width - left - right,
                   frame.height - top - bottom};
    ++report.checked_frames;
    std::map<std::string, MotionBox> current;
    Walker walker{static_cast<double>(frame.width),
                  static_cast<double>(frame.height), frame.frame, report,
                  current};
    walker.Walk(frame.root, Affine{}, 1.0, nullptr);
    report.previous_bounds = std::move(current);
}

std::string SerializeMotionSafeAreaReport(const MotionSafeAreaReport& report) {
    using mcp_json::Value;
    Value result = Value::MakeObject();
    // PRD38 -- which zone, by name and side by side; margin_percent as
    // before whenever the zone is one whole percentage on every side, which
    // the default zone always is.
    const MotionSafeZone& zone = report.zone;
    result.Set("zone", Value::MakeString(zone.name));
    Value margins = Value::MakeObject();
    margins.Set("top", Value::MakeRawNumber(PercentText(zone.top)));
    margins.Set("bottom", Value::MakeRawNumber(PercentText(zone.bottom)));
    margins.Set("left", Value::MakeRawNumber(PercentText(zone.left)));
    margins.Set("right", Value::MakeRawNumber(PercentText(zone.right)));
    result.Set("margins", std::move(margins));
    int64_t top = 0;
    int64_t side = 0;
    if (WholePercent(zone.top, top) && WholePercent(zone.bottom, side) &&
        side == top && WholePercent(zone.left, side) && side == top &&
        WholePercent(zone.right, side) && side == top) {
        result.Set("margin_percent", Value::MakeInt(top));
    }
    result.Set("safe_area", Box(report.safe));
    result.Set("checked_frames", Value::MakeInt(report.checked_frames));
    result.Set("inside", Value::MakeBool(report.issues.empty()));
    Value issues = Value::MakeArray();
    for (const MotionSafeAreaIssue& issue : report.issues) {
        Value entry = Value::MakeObject();
        entry.Set("kind", Value::MakeString(issue.kind));
        if (!issue.node_id.empty()) {
            entry.Set("node", Value::MakeString(issue.node_id));
        }
        if (!issue.uid.empty()) {
            entry.Set("uid", Value::MakeString(issue.uid));
        }
        if (issue.uid.empty() && issue.node_id.empty() && !issue.path.empty()) {
            entry.Set("path", Value::MakeString(issue.path));
        }
        if (issue.kind == "outside_safe_area") {
            entry.Set("at_rest", Value::MakeBool(issue.at_rest));
        }
        entry.Set("text", Value::MakeString(issue.text));
        if (!issue.edges.empty()) {
            Value edges = Value::MakeArray();
            for (const std::string& edge : issue.edges) {
                edges.Push(Value::MakeString(edge));
            }
            entry.Set("edges", std::move(edges));
        }
        entry.Set("first_frame", Value::MakeInt(issue.first_frame));
        entry.Set("last_frame", Value::MakeInt(issue.last_frame));
        entry.Set("bounds", Box(issue.bounds));
        issues.Push(std::move(entry));
    }
    result.Set("issues", std::move(issues));
    if (!report.decor.empty()) {
        // M5b -- what the verdict does not cover.
        Value decor = Value::MakeArray();
        for (const std::string& name : report.decor) {
            decor.Push(Value::MakeString(name));
        }
        result.Set("decor", std::move(decor));
    }
    if (report.at_rest_only) {
        // MM3 -- what was left out: element-frames outside while moving.
        result.Set("at_rest_only", Value::MakeBool(true));
        result.Set("in_motion", Value::MakeInt(report.in_motion));
    }
    return result.Dump();
}
