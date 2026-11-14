#include "MotionEvaluate.h"

#include "MotionAudio.h"
#include "MotionGeometry.h"
#include "Ulid.h"

#include <yoga/Yoga.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>

namespace {

using mcp_json::Value;

bool Fail(MotionError& error, std::string code, std::string path,
          std::string message) {
    error = {std::move(code), std::move(path), std::move(message)};
    return false;
}

// M1b -- local time as an exact fraction of frames. A timeScale of 1.25 makes
// frame 3 local frame 15/4; it must stay 15/4 all the way to the keyframes,
// never 3.75 rounded somewhere.
struct Frames {
    int64_t numerator = 0;
    int64_t denominator = 1;
};

bool MakeFrames(__int128 numerator, __int128 denominator, Frames& out) {
    if (denominator == 0) {
        return false;
    }
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    __int128 a = numerator < 0 ? -numerator : numerator;
    __int128 b = denominator;
    while (b != 0) {
        const __int128 rest = a % b;
        a = b;
        b = rest;
    }
    if (a > 1) {
        numerator /= a;
        denominator /= a;
    }
    const __int128 high = std::numeric_limits<int64_t>::max();
    const __int128 low = std::numeric_limits<int64_t>::min();
    if (numerator > high || numerator < low || denominator > high) {
        return false;
    }
    out = {static_cast<int64_t>(numerator), static_cast<int64_t>(denominator)};
    return true;
}

bool Minus(const Frames& time, int64_t frames, Frames& out) {
    return MakeFrames(static_cast<__int128>(time.numerator) -
                          static_cast<__int128>(frames) * time.denominator,
                      time.denominator, out);
}

bool Scale(const Frames& time, int64_t numerator, int64_t denominator,
           Frames& out) {
    return MakeFrames(static_cast<__int128>(time.numerator) * numerator,
                      static_cast<__int128>(time.denominator) * denominator,
                      out);
}

// Sign of time - frames.
int Compare(const Frames& time, int64_t frames) {
    const __int128 left = time.numerator;
    const __int128 right = static_cast<__int128>(frames) * time.denominator;
    return left < right ? -1 : (left > right ? 1 : 0);
}

// REM2 -- exact arithmetic on the same fractions, for the time maps of
// sounds (local = t * scale + offset).
bool Multiply(const Frames& a, const Frames& b, Frames& out) {
    return MakeFrames(static_cast<__int128>(a.numerator) * b.numerator,
                      static_cast<__int128>(a.denominator) * b.denominator,
                      out);
}

bool Add(const Frames& a, const Frames& b, Frames& out) {
    return MakeFrames(static_cast<__int128>(a.numerator) * b.denominator +
                          static_cast<__int128>(b.numerator) * a.denominator,
                      static_cast<__int128>(a.denominator) * b.denominator,
                      out);
}

bool Subtract(const Frames& a, const Frames& b, Frames& out) {
    return Add(a, Frames{-b.numerator, b.denominator}, out);
}

bool Divide(const Frames& a, const Frames& b, Frames& out) {
    return MakeFrames(static_cast<__int128>(a.numerator) * b.denominator,
                      static_cast<__int128>(a.denominator) * b.numerator, out);
}

int CompareFrames(const Frames& a, const Frames& b) {
    const __int128 left = static_cast<__int128>(a.numerator) * b.denominator;
    const __int128 right = static_cast<__int128>(b.numerator) * a.denominator;
    return left < right ? -1 : (left > right ? 1 : 0);
}

// Smallest integer >= a / b, b > 0.
__int128 CeilDivide(__int128 a, __int128 b) {
    return a >= 0 ? (a + b - 1) / b : -((-a) / b);
}

// Largest integer <= a / b, b > 0.
__int128 FloorDivide(__int128 a, __int128 b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// REM2 -- one sound of a composition: its media, and how the composition's
// time maps onto it. `t` is a frame of the evaluated (outermost)
// composition; the node's local frame is t * scale + offset, in frames of
// the composition that holds it (fps); it exists for t in [low, high).
struct AudioVoice {
    std::string path;
    std::string uid;
    std::string src;
    std::string media;
    bool video = false;
    Frames scale{1, 1};
    Frames offset{0, 1};
    bool has_low = false;
    Frames low;
    bool has_high = false;
    Frames high;
    int32_t fps_numerator = 25;
    int32_t fps_denominator = 1;
    int64_t start_from = 0;
    std::optional<int64_t> end_at;
    int64_t rate_numerator = 1;
    int64_t rate_denominator = 1;
    const MotionAnimation* volume_animation = nullptr;
    // MB11 -- a volume animation with keys bound to props, resolved: the
    // voice outlives the evaluator that collected it, so it keeps its copy.
    std::shared_ptr<const MotionAnimation> volume_bound;
    double volume = 1.0;
};

// Text properties CSS inherits from ancestors. A bare lineHeight number stays
// a multiplier while inherited, exactly as in CSS, and becomes pixels only
// against the font size of the text that uses it. M1b adds SVG painting,
// which SVG inherits the same way.
struct InheritedText {
    MotionTextStyle style;
    double line_height_multiplier = 0.0;
    double line_height_pixels = 0.0;
    std::string fill = "black";
    std::string stroke = "none";
    double stroke_width = 1.0;
    std::string linecap = "butt";
    std::string linejoin = "miter";
    std::string fill_rule = "nonzero";
};

enum class MeasureKind { Text, Image, Path, Intrinsic };

struct MeasureContext {
    MotionMeasurer* measurer = nullptr;
    MeasureKind kind = MeasureKind::Text;
    std::string text;
    MotionTextStyle style;
    std::string source;
    // A path's own size: its viewBox, or the extent of its geometry.
    double intrinsic_width = 0.0;
    double intrinsic_height = 0.0;
    bool keep_ratio = false;
    std::string path;
    MotionError* error = nullptr;
    bool* failed = nullptr;
};

struct ResolvedRange {
    MotionTextUnit unit = MotionTextUnit::Word;
    int64_t start = 0;
    int64_t count = std::numeric_limits<int64_t>::max();
    std::vector<std::pair<std::string, Value>> style;
};

// MB3 -- a highlight with its bindings and its progress read at the text's
// local frame; its bands wait for layout.
struct ResolvedHighlight {
    MotionTextUnit unit = MotionTextUnit::Word;
    int64_t start = 0;
    int64_t count = std::numeric_limits<int64_t>::max();
    MotionColor color;
    double progress = 1.0;
    double top = 0.0;
    double bottom = 1.0;
    double bleed = 0.0;
};

struct PendingNode {
    const MotionNode* source = nullptr;
    std::string path;
    std::string id;
    Frames local;
    YGNodeRef yoga = nullptr;
    std::vector<std::pair<std::string, Value>> style;
    std::string text;
    MotionTextStyle text_style;
    MotionTextUnit unit_kind = MotionTextUnit::None;
    std::vector<ResolvedRange> ranges;
    std::vector<ResolvedHighlight> highlights;
    std::string image_source;
    // REM2 -- a video's or a sound's media as resolved, what the media is,
    // and its speed; an instance's composition and the props it gives it,
    // validated.
    std::string media_src;
    std::string media_id;
    MotionMediaInfo media_info;
    int64_t rate_numerator = 1;
    int64_t rate_denominator = 1;
    // MB11 -- startFrom and endAt, the instance's values when bound.
    int64_t start_from = 0;
    std::optional<int64_t> end_at;
    const MotionComposition* instance = nullptr;
    Value instance_props;
    // An animated clip-path: the keyframe polygons of the current segment
    // and the eased progress between them, resolved against the box later.
    const MotionAnimation* clip_animation = nullptr;
    size_t clip_segment = 0;
    double clip_fraction = 0.0;
    // REM3 -- the same for the other values mixed after layout: gradients
    // (resolved against the box) and filter lists, by property.
    struct Deferred {
        const MotionAnimation* animation = nullptr;
        size_t segment = 0;
        double fraction = 0.0;
    };
    std::vector<std::pair<std::string, Deferred>> deferred;
    // REM3 -- a sequence a transition shows, and a series' running
    // transition.
    std::string transition_role;
    MotionTransitionState presented_by;
    MotionTransitionState transition;
    MotionPath path_geometry;
    // MOTION-DESIGN-2026-09 -- a morphing path's geometry at this instant.
    bool has_morph = false;
    MotionPath morph_geometry;
    InheritedText paint;
    std::vector<std::unique_ptr<PendingNode>> children;
};

YGSize MeasureNode(YGNodeConstRef node, float width, YGMeasureMode widthMode,
                   float height, YGMeasureMode heightMode) {
    auto* context = static_cast<MeasureContext*>(YGNodeGetContext(node));
    if (context == nullptr || *context->failed) {
        return YGSize{0.0f, 0.0f};
    }
    double measuredWidth = 0.0;
    double measuredHeight = 0.0;
    MotionError error;
    if (context->kind != MeasureKind::Text) {
        // A video, or an instance of another composition, has the intrinsic
        // size its media or its canvas gives it, kept in ratio like an
        // image's: both are Intrinsic, sized when the node was built.
        bool keepRatio = context->keep_ratio;
        if (context->kind == MeasureKind::Image) {
            keepRatio = true;
            if (!context->measurer->ImageSize(context->source, measuredWidth,
                                              measuredHeight, error)) {
                *context->error = error;
                if (context->error->path.empty()) {
                    context->error->path = context->path;
                }
                *context->failed = true;
                return YGSize{0.0f, 0.0f};
            }
        } else {
            measuredWidth = context->intrinsic_width;
            measuredHeight = context->intrinsic_height;
        }
        // An image, or a path with a viewBox, given one dimension keeps its
        // aspect ratio for the other.
        if (keepRatio && widthMode == YGMeasureModeExactly &&
            measuredWidth > 0.0 && heightMode != YGMeasureModeExactly) {
            measuredHeight = measuredHeight * width / measuredWidth;
            measuredWidth = width;
        } else if (keepRatio && heightMode == YGMeasureModeExactly &&
                   measuredHeight > 0.0 && widthMode != YGMeasureModeExactly) {
            measuredWidth = measuredWidth * height / measuredHeight;
            measuredHeight = height;
        }
    } else {
        MotionTextLayout layout;
        const double maxWidth =
            widthMode == YGMeasureModeUndefined ? -1.0 : width;
        if (!context->measurer->LayoutText(context->text, context->style,
                                           maxWidth, layout, error)) {
            *context->error = error;
            if (context->error->path.empty()) {
                context->error->path = context->path;
            }
            *context->failed = true;
            return YGSize{0.0f, 0.0f};
        }
        measuredWidth = layout.width;
        measuredHeight = layout.height;
    }
    float resultWidth = static_cast<float>(measuredWidth);
    float resultHeight = static_cast<float>(measuredHeight);
    if (widthMode == YGMeasureModeExactly) {
        resultWidth = width;
    }
    if (widthMode == YGMeasureModeAtMost) {
        resultWidth = std::min(resultWidth, width);
    }
    if (heightMode == YGMeasureModeExactly) {
        resultHeight = height;
    }
    if (heightMode == YGMeasureModeAtMost) {
        resultHeight = std::min(resultHeight, height);
    }
    return YGSize{resultWidth, resultHeight};
}

// SplitMix64: a published, fully specified generator, so a seed gives the
// same order on every machine and standard library -- unlike std::shuffle,
// whose algorithm is left to the implementation.
class SeededRandom {
public:
    explicit SeededRandom(uint64_t seed) : state_(seed) {}

    uint64_t Next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Unbiased integer in [0, bound), by rejection.
    uint64_t Below(uint64_t bound) {
        const uint64_t threshold = (0 - bound) % bound;
        for (;;) {
            const uint64_t value = Next();
            if (value >= threshold) {
                return value % bound;
            }
        }
    }

private:
    uint64_t state_;
};

// Rank of each unit (or copy) in the cascade: unit i starts rank[i] * each
// frames late.
std::vector<int64_t> StaggerRanks(const MotionStagger& stagger, size_t count) {
    std::vector<int64_t> ranks(count, 0);
    if (!stagger.present) {
        return ranks;
    }
    std::vector<size_t> order(count);
    for (size_t index = 0; index < count; ++index) {
        order[index] = index;
    }
    if (stagger.order == MotionStaggerOrder::Reverse) {
        std::reverse(order.begin(), order.end());
    } else if (stagger.order == MotionStaggerOrder::Random) {
        SeededRandom random(stagger.seed);
        for (size_t index = count; index > 1; --index) {
            std::swap(order[index - 1], order[random.Below(index)]);
        }
    }
    for (size_t position = 0; position < count; ++position) {
        ranks[order[position]] = static_cast<int64_t>(position);
    }
    return ranks;
}

// Separators between words: spaces of every width, the no-break spaces of
// French typography included ("projet ?" is two words, as in a text tool).
bool IsSpaceCluster(const std::string& text) {
    size_t index = 0;
    bool any = false;
    while (index < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        uint32_t code = 0;
        size_t length = 1;
        if (byte < 0x80) {
            code = byte;
        } else if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
            code = ((byte & 0x1Fu) << 6) | (text[index + 1] & 0x3Fu);
            length = 2;
        } else if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
            code = ((byte & 0x0Fu) << 12) | ((text[index + 1] & 0x3Fu) << 6) |
                   (text[index + 2] & 0x3Fu);
            length = 3;
        } else {
            return false;
        }
        const bool space = code == 0x20 || code == 0x09 || code == 0x0A ||
                           code == 0x0D || code == 0xA0 ||
                           (code >= 0x2000 && code <= 0x200A) ||
                           code == 0x202F || code == 0x205F || code == 0x3000;
        if (!space) {
            return false;
        }
        any = true;
        index += length;
    }
    return any;
}

std::string ColorValue(const MotionColor& color) {
    // Full precision, so an animated colour parses back to the same doubles.
    const auto channel = [](double value) {
        char buffer[64];
        const auto result =
            std::to_chars(buffer, buffer + sizeof(buffer), value,
                          std::chars_format::general, 17);
        return std::string(buffer, result.ptr);
    };
    return "rgba(" + channel(color.red * 255.0) + "," +
           channel(color.green * 255.0) + "," + channel(color.blue * 255.0) +
           "," + channel(color.alpha) + ")";
}

// CSS interpolates legacy sRGB colours with premultiplied alpha, so a fade
// from transparent to red never passes through a dark red.
MotionColor MixColors(const MotionColor& from, const MotionColor& to,
                      double fraction) {
    const double alpha = from.alpha + (to.alpha - from.alpha) * fraction;
    const auto channel = [&](double a, double b) {
        const double premultiplied =
            a * from.alpha + (b * to.alpha - a * from.alpha) * fraction;
        return alpha > 0.0 ? premultiplied / alpha : 0.0;
    };
    MotionColor mixed{channel(from.red, to.red), channel(from.green, to.green),
                      channel(from.blue, to.blue), alpha};
    mixed.red = std::clamp(mixed.red, 0.0, 1.0);
    mixed.green = std::clamp(mixed.green, 0.0, 1.0);
    mixed.blue = std::clamp(mixed.blue, 0.0, 1.0);
    mixed.alpha = std::clamp(mixed.alpha, 0.0, 1.0);
    return mixed;
}

// REM3 -- a length along `extent`: pixels, or a percentage of it.
double Resolve1(const MotionLength& length, double extent) {
    return length.unit == MotionLengthUnit::Percent
               ? extent * length.value / 100.0
               : length.value;
}

// REM3 -- a gradient against the box it paints, as CSS resolves it.
MotionResolvedGradient ResolveGradient(const MotionGradient& gradient,
                                       const MotionBox& box) {
    constexpr double kPi = 3.14159265358979323846;
    MotionResolvedGradient out;
    out.kind = gradient.kind;
    const double width = box.width;
    const double height = box.height;
    if (gradient.kind == MotionGradientKind::Linear) {
        double angle = gradient.angle;
        if (gradient.corner_x != 0 && gradient.corner_y != 0) {
            // Perpendicular to the diagonal between the two neighbouring
            // corners, pointing into the named one.
            angle = std::atan2(gradient.corner_x * height,
                               -gradient.corner_y * width) *
                    180.0 / kPi;
        }
        const double radians = angle * kPi / 180.0;
        const double dx = std::sin(radians);
        const double dy = -std::cos(radians);
        const double length = std::abs(width * dx) + std::abs(height * dy);
        const double cx = box.x + width / 2.0;
        const double cy = box.y + height / 2.0;
        out.x0 = cx - dx * length / 2.0;
        out.y0 = cy - dy * length / 2.0;
        out.x1 = cx + dx * length / 2.0;
        out.y1 = cy + dy * length / 2.0;
    } else {
        const double cx = Resolve1(gradient.center_x, width);
        const double cy = Resolve1(gradient.center_y, height);
        const double left = cx;
        const double right = width - cx;
        const double top = cy;
        const double bottom = height - cy;
        const bool closest = gradient.extent.rfind("closest", 0) == 0;
        const bool corner = gradient.extent.find("corner") != std::string::npos;
        double rx = closest ? std::min(std::abs(left), std::abs(right))
                            : std::max(std::abs(left), std::abs(right));
        double ry = closest ? std::min(std::abs(top), std::abs(bottom))
                            : std::max(std::abs(top), std::abs(bottom));
        if (gradient.circle) {
            const double radius =
                corner ? std::hypot(rx, ry)
                       : (closest ? std::min(rx, ry) : std::max(rx, ry));
            rx = ry = radius;
        } else if (corner) {
            // The ellipse of the side's proportions through the corner.
            rx *= std::sqrt(2.0);
            ry *= std::sqrt(2.0);
        }
        out.x0 = box.x + cx;
        out.y0 = box.y + cy;
        out.x1 = rx;
        out.y1 = ry;
    }
    const std::vector<double> positions = MotionGradientPositions(gradient);
    for (size_t index = 0; index < gradient.stops.size(); ++index) {
        out.stops.emplace_back(positions[index], gradient.stops[index].color);
    }
    return out;
}

MotionResolvedGradient MixGradients(const MotionGradient& from,
                                    const MotionGradient& to, double t,
                                    const MotionBox& box) {
    if (from.kind == MotionGradientKind::Linear) {
        // CSS turns the line: the angle is mixed, then resolved.
        MotionGradient mixed = from;
        mixed.angle = from.angle + (to.angle - from.angle) * t;
        MotionResolvedGradient out = ResolveGradient(mixed, box);
        const MotionResolvedGradient b = ResolveGradient(to, box);
        for (size_t index = 0; index < out.stops.size(); ++index) {
            out.stops[index].first +=
                (b.stops[index].first - out.stops[index].first) * t;
            out.stops[index].second =
                MixColors(out.stops[index].second, b.stops[index].second, t);
        }
        return out;
    }
    MotionResolvedGradient out = ResolveGradient(from, box);
    const MotionResolvedGradient b = ResolveGradient(to, box);
    out.x0 += (b.x0 - out.x0) * t;
    out.y0 += (b.y0 - out.y0) * t;
    out.x1 += (b.x1 - out.x1) * t;
    out.y1 += (b.y1 - out.y1) * t;
    for (size_t index = 0; index < out.stops.size(); ++index) {
        out.stops[index].first +=
            (b.stops[index].first - out.stops[index].first) * t;
        out.stops[index].second =
            MixColors(out.stops[index].second, b.stops[index].second, t);
    }
    return out;
}

// REM3 -- a clip-path shape against the node's border box.
MotionResolvedClip ResolveClip(const MotionClipShape& shape,
                               const MotionBox& box) {
    MotionResolvedClip out;
    out.kind = shape.kind;
    const double width = box.width;
    const double height = box.height;
    if (shape.kind == MotionClipKind::Polygon) {
        for (const auto& vertex : shape.polygon) {
            out.points.emplace_back(box.x + Resolve1(vertex.first, width),
                                    box.y + Resolve1(vertex.second, height));
        }
        return out;
    }
    if (shape.kind == MotionClipKind::Inset) {
        const double top = Resolve1(shape.inset[0], height);
        const double right = Resolve1(shape.inset[1], width);
        const double bottom = Resolve1(shape.inset[2], height);
        const double left = Resolve1(shape.inset[3], width);
        out.rect = {box.x + left, box.y + top,
                    std::max(0.0, width - left - right),
                    std::max(0.0, height - top - bottom)};
        out.round = Resolve1(shape.round, std::min(width, height));
        return out;
    }
    const double cx = Resolve1(shape.center_x, width);
    const double cy = Resolve1(shape.center_y, height);
    out.cx = box.x + cx;
    out.cy = box.y + cy;
    const auto side = [&](MotionRadiusKeyword keyword, bool horizontal) {
        const double a = horizontal ? cx : cy;
        const double b = (horizontal ? width : height) - a;
        return keyword == MotionRadiusKeyword::FarthestSide
                   ? std::max(std::abs(a), std::abs(b))
                   : std::min(std::abs(a), std::abs(b));
    };
    if (shape.kind == MotionClipKind::Circle) {
        double radius = 0.0;
        if (shape.keyword_x == MotionRadiusKeyword::None) {
            // CSS: a percentage of the box's diagonal over the root of two.
            radius = shape.radius_x.unit == MotionLengthUnit::Percent
                         ? std::hypot(width, height) / std::sqrt(2.0) *
                               shape.radius_x.value / 100.0
                         : shape.radius_x.value;
        } else {
            const double horizontal = side(shape.keyword_x, true);
            const double vertical = side(shape.keyword_x, false);
            radius = shape.keyword_x == MotionRadiusKeyword::FarthestSide
                         ? std::max(horizontal, vertical)
                         : std::min(horizontal, vertical);
        }
        out.rx = out.ry = radius;
        return out;
    }
    out.rx = shape.keyword_x == MotionRadiusKeyword::None
                 ? Resolve1(shape.radius_x, width)
                 : side(shape.keyword_x, true);
    out.ry = shape.keyword_y == MotionRadiusKeyword::None
                 ? Resolve1(shape.radius_y, height)
                 : side(shape.keyword_y, false);
    return out;
}

MotionResolvedClip MixClips(const MotionResolvedClip& a,
                            const MotionResolvedClip& b, double t) {
    MotionResolvedClip out = a;
    for (size_t index = 0; index < out.points.size(); ++index) {
        out.points[index].first +=
            (b.points[index].first - a.points[index].first) * t;
        out.points[index].second +=
            (b.points[index].second - a.points[index].second) * t;
    }
    out.cx += (b.cx - a.cx) * t;
    out.cy += (b.cy - a.cy) * t;
    out.rx += (b.rx - a.rx) * t;
    out.ry += (b.ry - a.ry) * t;
    out.rect.x += (b.rect.x - a.rect.x) * t;
    out.rect.y += (b.rect.y - a.rect.y) * t;
    out.rect.width += (b.rect.width - a.rect.width) * t;
    out.rect.height += (b.rect.height - a.rect.height) * t;
    out.round += (b.round - a.round) * t;
    return out;
}

// REM3 -- two filter lists of the same effects, parameter by parameter.
std::vector<MotionFilter> MixFilters(const std::vector<MotionFilter>& a,
                                     const std::vector<MotionFilter>& b,
                                     double t) {
    std::vector<MotionFilter> out = a;
    const auto mix = [t](double from, double to) {
        return from + (to - from) * t;
    };
    for (size_t index = 0; index < out.size(); ++index) {
        MotionFilter& filter = out[index];
        const MotionFilter& other = b[index];
        filter.sigma = std::max(0.0, mix(filter.sigma, other.sigma));
        filter.offset_x = mix(filter.offset_x, other.offset_x);
        filter.offset_y = mix(filter.offset_y, other.offset_y);
        filter.color = MixColors(filter.color, other.color, t);
        filter.strength = std::max(0.0, mix(filter.strength, other.strength));
        filter.amplitude =
            std::max(0.0, mix(filter.amplitude, other.amplitude));
        filter.wavelength =
            std::max(1e-6, mix(filter.wavelength, other.wavelength));
        filter.phase = mix(filter.phase, other.phase);
        if (filter.mesh_resolved.size() == other.mesh_resolved.size()) {
            for (size_t vertex = 0; vertex < filter.mesh_resolved.size();
                 ++vertex) {
                filter.mesh_resolved[vertex].first =
                    mix(filter.mesh_resolved[vertex].first,
                        other.mesh_resolved[vertex].first);
                filter.mesh_resolved[vertex].second =
                    mix(filter.mesh_resolved[vertex].second,
                        other.mesh_resolved[vertex].second);
            }
        }
    }
    return out;
}

class Evaluator {
public:
    // `stack`: the ULIDs of the compositions being evaluated around this
    // one, outermost first -- REM2's instances, and the cycle guard.
    Evaluator(const MotionComposition& composition, MotionMeasurer& measurer,
              MotionError& error, const MotionEvaluationContext& context,
              std::vector<std::string> stack)
        : composition_(composition),
          measurer_(measurer),
          error_(error),
          context_(context),
          stack_(std::move(stack)) {
        config_ = YGConfigNew();
        // CSS semantics rather than React Native's: row by default, shrinking
        // flex items, and every legacy layout bug switched off.
        YGConfigSetUseWebDefaults(config_, true);
        YGConfigSetErrata(config_, YGErrataNone);
        // Exact float layout. Snapping to device pixels is a rendering
        // decision, taken in M2 where the pixels are.
        YGConfigSetPointScaleFactor(config_, 0.0f);
    }

    ~Evaluator() {
        if (canvas_ != nullptr) {
            YGNodeFreeRecursive(canvas_);
        }
        YGConfigFree(config_);
    }

    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    bool Run(const Value& overrides, int64_t frame, MotionFrame& output) {
        return RunAt(overrides, Frames{frame, 1}, frame, output);
    }

    // MOTION-DESIGN-2026-09 -- the same at an exact fraction of a frame: an
    // instant of a motion-blur shutter. `frame` labels the result.
    bool RunAt(const Value& overrides, const Frames& time, int64_t frame,
               MotionFrame& output) {
        if (frame < 0 || frame >= composition_.duration) {
            return Fail(error_, "FrameOutOfRange", "frame",
                        "frame must lie in [0, " +
                            std::to_string(composition_.duration) + ")");
        }
        if (!ResolveProps(overrides)) {
            return false;
        }

        canvas_ = YGNodeNewWithConfig(config_);
        YGNodeStyleSetWidth(canvas_, static_cast<float>(composition_.width));
        YGNodeStyleSetHeight(canvas_, static_cast<float>(composition_.height));

        InheritedText inherited;
        std::vector<std::unique_ptr<PendingNode>> roots;
        root_time_ = time;
        if (!Build(composition_.root, "root", "", time, inherited, canvas_,
                   roots, nullptr)) {
            return false;
        }
        YGNodeCalculateLayout(canvas_, static_cast<float>(composition_.width),
                              static_cast<float>(composition_.height),
                              YGDirectionLTR);
        if (measure_failed_) {
            return false;
        }

        output = MotionFrame{};
        output.width = composition_.width;
        output.height = composition_.height;
        output.frame = frame;
        if (!ExtractRoot(roots, output.root)) {
            return false;
        }
        // REM3 -- what each effect may write, once every transform is known.
        ResolveMotionEffectDomains(output);
        return true;
    }

    // REM2 -- an instance's composition at `local`, a frame of its own
    // cadence that may be a fraction; `props` are the instance's values,
    // already checked against this composition's props.
    bool RunInstance(const Frames& local, const Value& props,
                     MotionEvaluatedNode& root) {
        if (!ResolveProps(props)) {
            return false;
        }
        canvas_ = YGNodeNewWithConfig(config_);
        YGNodeStyleSetWidth(canvas_, static_cast<float>(composition_.width));
        YGNodeStyleSetHeight(canvas_, static_cast<float>(composition_.height));
        InheritedText inherited;
        std::vector<std::unique_ptr<PendingNode>> roots;
        root_time_ = local;
        if (!Build(composition_.root, "root", "", local, inherited, canvas_,
                   roots, nullptr)) {
            return false;
        }
        YGNodeCalculateLayout(canvas_, static_cast<float>(composition_.width),
                              static_cast<float>(composition_.height),
                              YGDirectionLTR);
        if (measure_failed_) {
            return false;
        }
        return ExtractRoot(roots, root);
    }

    // REM2 -- where the outermost composition's frames land in this one,
    // and the frames of it that are shown at all.
    struct TimeMap {
        Frames scale{1, 1};
        Frames offset{0, 1};
        bool has_low = false;
        Frames low;
        bool has_high = false;
        Frames high;
        int32_t top_numerator = 25;
        int32_t top_denominator = 1;
    };

    // REM2 -- every sound of this composition, in tree order.
    bool CollectAudio(const Value& overrides, const TimeMap& map,
                      std::vector<AudioVoice>& voices) {
        if (!ResolveProps(overrides)) {
            return false;
        }
        return Voices(composition_.root, "root", map, voices);
    }

    // MB11 -- this composition as the instance `props` plays it: the bound
    // keys and media times hold its values, and nothing is bound any more.
    bool BindInstance(const Value& props, MotionComposition& out) {
        if (!ResolveProps(props)) {
            return false;
        }
        out = composition_;
        return BindNode(out.root, "root");
    }

private:
    bool BindNode(MotionNode& node, const std::string& path) {
        const std::string animate = path + ".animate.";
        for (auto& [property, animation] : node.animate) {
            const MotionAnimation* resolved = nullptr;
            if (!Bound(animation, animate + property, resolved)) {
                return false;
            }
            if (resolved != &animation) {
                animation = *resolved;
            }
            animation.bound_values.clear();
        }
        // MB3 -- a highlight's progress is a track too. Without binding it
        // here, the exporter sees the default keys and may classify a
        // moving instance as a still stroke.
        for (size_t index = 0; index < node.highlights.size(); ++index) {
            auto& highlight = node.highlights[index];
            if (!highlight.progress_animation.has_value()) {
                continue;
            }
            MotionAnimation& animation = *highlight.progress_animation;
            const MotionAnimation* resolved = nullptr;
            if (!Bound(animation,
                       path + ".highlights[" + std::to_string(index) +
                           "].progress",
                       resolved)) {
                return false;
            }
            if (resolved != &animation) {
                animation = *resolved;
            }
            animation.bound_values.clear();
        }
        if (!MediaTimes(node, path, node.start_from, node.end_at)) {
            return false;
        }
        node.start_from_binding.reset();
        node.end_at_binding.reset();
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (!BindNode(node.children[index],
                          path + ".children[" + std::to_string(index) + "]")) {
                return false;
            }
        }
        return true;
    }

    // local >= bound, as a bound on t: t >= (bound - offset) / scale.
    bool AtLeast(TimeMap& map, const Frames& bound, const std::string& path) {
        Frames difference;
        Frames t;
        if (!Subtract(bound, map.offset, difference) ||
            !Divide(difference, map.scale, t)) {
            return Fail(error_, "TimeOverflow", path,
                        "the sound's time cannot be represented exactly");
        }
        if (!map.has_low || CompareFrames(t, map.low) > 0) {
            map.low = t;
            map.has_low = true;
        }
        return true;
    }

    // local < bound, as a bound on t.
    bool Before(TimeMap& map, const Frames& bound, const std::string& path) {
        Frames difference;
        Frames t;
        if (!Subtract(bound, map.offset, difference) ||
            !Divide(difference, map.scale, t)) {
            return Fail(error_, "TimeOverflow", path,
                        "the sound's time cannot be represented exactly");
        }
        if (!map.has_high || CompareFrames(t, map.high) < 0) {
            map.high = t;
            map.has_high = true;
        }
        return true;
    }

    // local becomes (local - shift) * factor.
    bool Remap(TimeMap& map, const Frames& shift, const Frames& factor,
               const std::string& path) {
        Frames offset;
        if (!Subtract(map.offset, shift, offset) ||
            !Multiply(offset, factor, map.offset) ||
            !Multiply(map.scale, factor, map.scale)) {
            return Fail(error_, "TimeOverflow", path,
                        "the sound's time cannot be represented exactly");
        }
        return true;
    }

    bool Voices(const MotionNode& node, const std::string& path, TimeMap map,
                std::vector<AudioVoice>& voices) {
        if (node.type == MotionNodeType::Sequence) {
            if (!AtLeast(map, Frames{node.from, 1}, path) ||
                (node.duration.has_value() &&
                 !Before(map, Frames{node.from + *node.duration, 1}, path))) {
                return false;
            }
            Frames factor{1, 1};
            if (node.time_scale.has_value()) {
                Value scale;
                if (!Resolve(*node.time_scale, path + ".timeScale", scale)) {
                    return false;
                }
                if (!ParseMotionTimeScale(scale, factor.numerator,
                                          factor.denominator)) {
                    return Fail(error_, "InvalidValue", path + ".timeScale",
                                "timeScale must resolve to a positive number "
                                "or a percentage, at most 1000");
                }
            }
            if (!Remap(map, Frames{node.from, 1}, factor, path)) {
                return false;
            }
        }
        if (node.visible.has_value()) {
            Value visible;
            if (!Resolve(*node.visible, path + ".visible", visible)) {
                return false;
            }
            if (!visible.IsBool()) {
                return Fail(error_, "InvalidValue", path + ".visible",
                            "visible must resolve to a boolean");
            }
            if (!visible.AsBool()) {
                return true;
            }
        }
        if (node.type == MotionNodeType::Repeat) {
            return RepeatVoices(node, path, map, voices);
        }
        if (node.type == MotionNodeType::Video ||
            node.type == MotionNodeType::Audio) {
            return MediaVoice(node, path, map, voices);
        }
        if (node.type == MotionNodeType::Composition) {
            return InstanceVoices(node, path, map, voices);
        }
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (!Voices(node.children[index],
                        path + ".children[" + std::to_string(index) + "]", map,
                        voices)) {
                return false;
            }
        }
        return true;
    }

    bool RepeatVoices(const MotionNode& node, const std::string& path,
                      const TimeMap& map, std::vector<AudioVoice>& voices) {
        std::vector<Value> items;
        if (!RepeatItems(node, path, items)) {
            return false;
        }
        const std::vector<int64_t> ranks =
            StaggerRanks(node.stagger, items.size());
        for (size_t index = 0; index < items.size(); ++index) {
            TimeMap copy = map;
            if (!Remap(copy, Frames{ranks[index] * node.stagger.each, 1},
                       Frames{1, 1}, path)) {
                return false;
            }
            scopes_.push_back({items[index], static_cast<int64_t>(index)});
            const bool ok =
                Voices(node.children.front(),
                       path + ".template[" + std::to_string(index) + "]", copy,
                       voices);
            scopes_.pop_back();
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    bool MediaVoice(const MotionNode& node, const std::string& path,
                    TimeMap map, std::vector<AudioVoice>& voices) {
        if (node.type == MotionNodeType::Video && node.muted) {
            return true;
        }
        AudioVoice voice;
        voice.path = path;
        voice.uid = node.uid;
        voice.video = node.type == MotionNodeType::Video;
        const bool byId = node.media.has_value();
        const std::string field = byId ? ".media" : ".src";
        const std::optional<MotionValue>& source = byId ? node.media : node.src;
        // The parser gives a video or a sound its media or its src.
        if (!source.has_value()) {
            return Fail(error_, "InvalidValue", path + field,
                        "a video or a sound names its src or its media");
        }
        Value resolved;
        if (!Resolve(*source, path + field, resolved)) {
            return false;
        }
        if (!resolved.IsString() || resolved.AsString().empty() ||
            (byId && !IsValidUlid(resolved.AsString()))) {
            return Fail(error_, "InvalidValue", path + field,
                        byId ? "media must resolve to a project media's ULID"
                             : "src must resolve to a non-empty string");
        }
        (byId ? voice.media : voice.src) = resolved.AsString();
        MotionMediaInfo info;
        MotionError error;
        if (!measurer_.MediaInfo(voice.src, voice.media, info, error)) {
            error_ = error;
            if (error_.path.empty()) {
                error_.path = path + field;
            }
            return false;
        }
        if (!info.has_audio) {
            if (voice.video) {
                return true;  // a silent video plays nothing, and says so
            }
            return Fail(error_, "MediaHasNoAudio", path + field,
                        "an audio node plays a media with sound");
        }
        if (node.playback_rate.has_value()) {
            Value rate;
            if (!Resolve(*node.playback_rate, path + ".playbackRate", rate)) {
                return false;
            }
            if (!ParseMotionTimeScale(rate, voice.rate_numerator,
                                      voice.rate_denominator)) {
                return Fail(error_, "InvalidValue", path + ".playbackRate",
                            "playbackRate must resolve to a positive number "
                            "or a percentage, at most 1000");
            }
        }
        for (const auto& entry : node.style) {
            if (entry.first != "volume") {
                continue;
            }
            Value volume;
            if (!Resolve(entry.second, path + ".style.volume", volume)) {
                return false;
            }
            if (entry.second.kind != MotionBindingKind::Literal &&
                !ValidateMotionStyleValue("volume", volume,
                                          path + ".style.volume", error_)) {
                return false;
            }
            voice.volume = Number(volume);
        }
        for (const auto& entry : node.animate) {
            if (entry.first == "volume") {
                const MotionAnimation* animation = nullptr;
                if (!Bound(entry.second, path + ".animate.volume", animation)) {
                    return false;
                }
                if (animation != &entry.second) {
                    voice.volume_bound =
                        std::make_shared<const MotionAnimation>(*animation);
                    animation = voice.volume_bound.get();
                }
                voice.volume_animation = animation;
            }
        }
        if (voice.volume_animation == nullptr && voice.volume == 0.0) {
            return true;
        }
        // The sound's pace against the outermost composition's: exactly 1,
        // or it would have to be resampled into a pitch nobody chose.
        const __int128 pace = static_cast<__int128>(map.scale.numerator) *
                              voice.rate_numerator * map.top_numerator *
                              composition_.fps_denominator;
        const __int128 unit = static_cast<__int128>(map.scale.denominator) *
                              voice.rate_denominator * map.top_denominator *
                              composition_.fps_numerator;
        if (pace != unit) {
            return Fail(error_, "UnsupportedAudioRate", path,
                        "this sound would play at another speed than "
                        "recorded (playbackRate or an enclosing timeScale): "
                        "set muted, or volume 0, to keep the picture's speed "
                        "without its sound");
        }
        if (!MediaTimes(node, path, voice.start_from, voice.end_at)) {
            return false;
        }
        voice.fps_numerator = composition_.fps_numerator;
        voice.fps_denominator = composition_.fps_denominator;
        // Heard from local frame 0, until endAt.
        if (!AtLeast(map, Frames{0, 1}, path)) {
            return false;
        }
        if (voice.end_at.has_value()) {
            Frames played;
            if (!MakeFrames(
                    static_cast<__int128>(*voice.end_at - voice.start_from) *
                        voice.rate_denominator,
                    voice.rate_numerator, played) ||
                !Before(map, played, path)) {
                return false;
            }
        }
        voice.scale = map.scale;
        voice.offset = map.offset;
        voice.has_low = map.has_low;
        voice.low = map.low;
        voice.has_high = map.has_high;
        voice.high = map.high;
        voices.push_back(std::move(voice));
        return true;
    }

    bool InstanceVoices(const MotionNode& node, const std::string& path,
                        TimeMap map, std::vector<AudioVoice>& voices) {
        const MotionComposition* child = nullptr;
        Value props;
        if (!InstanceTarget(node, path, child) ||
            !InstanceProps(node, path, *child, props)) {
            return false;
        }
        const Frames factor{static_cast<int64_t>(child->fps_numerator) *
                                composition_.fps_denominator,
                            static_cast<int64_t>(child->fps_denominator) *
                                composition_.fps_numerator};
        if (!Remap(map, Frames{0, 1}, factor, path) ||
            !AtLeast(map, Frames{0, 1}, path) ||
            !Before(map, Frames{child->duration, 1}, path)) {
            return false;
        }
        std::vector<std::string> stack = stack_;
        stack.push_back(node.instance_of);
        MotionEvaluationContext nested = context_;
        nested.composition_id = node.instance_of;
        Evaluator evaluator(*child, measurer_, error_, nested,
                            std::move(stack));
        if (!evaluator.CollectAudio(props, map, voices)) {
            error_.path = path + " > " + error_.path;
            return false;
        }
        return true;
    }

    // REM2 -- the composition an instance names, found and not a cycle.
    bool InstanceTarget(const MotionNode& node, const std::string& path,
                        const MotionComposition*& child) {
        const std::string where = path + ".composition";
        if (context_.library == nullptr) {
            return Fail(error_, "UnknownComposition", where,
                        "an instance needs the project's compositions: '" +
                            node.instance_of +
                            "' cannot be found from a composition alone");
        }
        const auto found =
            context_.library->compositions.find(node.instance_of);
        if (found == context_.library->compositions.end()) {
            return Fail(
                error_, "UnknownComposition", where,
                "the project has no composition '" + node.instance_of + "'");
        }
        if (std::find(stack_.begin(), stack_.end(), node.instance_of) !=
                stack_.end() ||
            stack_.size() >= static_cast<size_t>(kMotionInstanceDepth)) {
            return Fail(error_, "CompositionCycle", where,
                        "composition '" + node.instance_of +
                            "' would show itself, directly or through its "
                            "instances");
        }
        child = &found->second;
        return true;
    }

    // REM2 -- an instance's values, each resolved here, where this
    // composition's props and repeat scope are, then held to the other
    // composition's typed props with M3's refusals.
    bool InstanceProps(const MotionNode& node, const std::string& path,
                       const MotionComposition& child, Value& props) {
        props = Value::MakeObject();
        for (const auto& entry : node.instance_props) {
            const std::string propPath = path + ".props." + entry.first;
            const MotionPropDefinition* definition = nullptr;
            for (const MotionPropDefinition& prop : child.props) {
                if (prop.name == entry.first) {
                    definition = &prop;
                }
            }
            if (definition == nullptr) {
                return Fail(error_, "UnknownProp", propPath,
                            "composition '" + node.instance_of +
                                "' declares no prop '" + entry.first + "'");
            }
            Value value;
            if (!Resolve(entry.second, propPath, value) ||
                !ValidateMotionPropValue(*definition, value, propPath,
                                         error_)) {
                return false;
            }
            props.Set(entry.first, std::move(value));
        }
        return true;
    }

    // A repeat's elements: `count` integers, or the list it binds.
    bool RepeatItems(const MotionNode& node, const std::string& path,
                     std::vector<Value>& items) {
        if (node.count.has_value()) {
            int64_t count = 0;
            if (!NonNegativeInteger(*node.count, path + ".count", count)) {
                return false;
            }
            if (count > 1000) {
                return Fail(error_, "InvalidValue", path + ".count",
                            "count must resolve to at most 1000");
            }
            for (int64_t index = 0; index < count; ++index) {
                items.push_back(Value::MakeInt(index));
            }
        } else if (node.each.has_value()) {
            Value list;
            if (!Resolve(*node.each, path + ".each", list)) {
                return false;
            }
            if (!list.IsArray()) {
                return Fail(error_, "InvalidValue", path + ".each",
                            "each must resolve to a list");
            }
            items = list.AsArray();
        }
        return true;
    }

    bool ExtractRoot(const std::vector<std::unique_ptr<PendingNode>>& roots,
                     MotionEvaluatedNode& root) {
        MotionFrame output;
        if (roots.empty()) {
            // An invisible root still yields a well-formed, empty frame.
            output.root.path = "root";
            output.root.uid = composition_.root.uid;
            output.root.type = composition_.root.type;
            output.root.box = {0.0, 0.0,
                               static_cast<double>(composition_.width),
                               static_cast<double>(composition_.height)};
            root = std::move(output.root);
            return true;
        }
        return Extract(*roots.front(), 0.0, 0.0, root);
    }

    struct Scope {
        Value item;
        int64_t index = 0;
    };

    using Overrides = std::vector<std::pair<std::string, const MotionValue*>>;

    // MB11 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. What a prop
    // binding of a time or of an animated value gives this instance: the
    // prop's value, through its map when it has one. No unit conversion:
    // an animated number is a number.
    bool ResolveBound(const MotionValue& binding, const std::string& path,
                      Value& out) {
        const Value& whole = props_.at(binding.prop);
        const Value* component =
            binding.component.empty() ? &whole : whole.Find(binding.component);
        const Value& prop = component != nullptr ? *component : whole;
        if (binding.kind == MotionBindingKind::Prop) {
            out = prop;
            return true;
        }
        return Lookup(binding, prop, path, out);
    }

    // MB11 -- a media's startFrom and endAt for this instance.
    bool MediaTimes(const MotionNode& node, const std::string& path,
                    int64_t& start, std::optional<int64_t>& end) {
        start = node.start_from;
        end = node.end_at;
        const auto frames = [&](const MotionValue& binding, const char* key,
                                int64_t& value) {
            Value resolved;
            if (!ResolveBound(binding, path + "." + key, resolved)) {
                return false;
            }
            if (!resolved.AsInt64(value) || value < 0) {
                return Fail(error_, "InvalidValue", path + "." + key,
                            std::string(key) +
                                " must resolve to a non-negative number of "
                                "frames, not " +
                                resolved.Dump());
            }
            return true;
        };
        if (node.start_from_binding.has_value() &&
            !frames(*node.start_from_binding, "startFrom", start)) {
            return false;
        }
        if (node.end_at_binding.has_value()) {
            int64_t value = 0;
            if (!frames(*node.end_at_binding, "endAt", value)) {
                return false;
            }
            end = value;
        }
        if (end.has_value() && *end <= start) {
            return Fail(error_, "InvalidValue", path + ".endAt",
                        "endAt must be a number of frames after startFrom");
        }
        return true;
    }

    // MB11 -- an animation whose keys are bound to props, with this
    // instance's values in their place. Built once per frame and kept, so
    // that what points to it (a clip-path, a deferred effect, a volume)
    // holds until the frame is drawn.
    bool Bound(const MotionAnimation& animation, const std::string& path,
               const MotionAnimation*& out) {
        if (animation.bound_values.empty()) {
            out = &animation;
            return true;
        }
        if (const auto found = bound_.find(&animation); found != bound_.end()) {
            out = &found->second;
            return true;
        }
        MotionAnimation copy = animation;
        std::vector<Keyframe> keys = animation.track.keyframes();
        for (const auto& [index, binding] : animation.bound_values) {
            const std::string keyPath =
                path + ".values[" + std::to_string(index) + "]";
            Value value;
            if (!ResolveBound(binding, keyPath, value)) {
                return false;
            }
            if (animation.value_kind == MotionValueKind::Number) {
                double number = 0.0;
                if (!MotionNumber(value, number) || index >= keys.size()) {
                    return Fail(
                        error_, "InvalidAnimation", keyPath,
                        "the bound prop gives no number: " + value.Dump());
                }
                keys[index].value = number;
                continue;
            }
            MotionColor colour;
            MotionClipShape shape;
            MotionClipShape own;
            const bool fits =
                index < copy.values.size() && value.IsString() &&
                (animation.value_kind == MotionValueKind::Color
                     ? ParseMotionColor(value.AsString(), colour)
                     : ParseMotionClipShape(value.AsString(), shape) &&
                           ParseMotionClipShape(
                               animation.values[index].AsString(), own) &&
                           shape.kind == own.kind &&
                           shape.polygon.size() == own.polygon.size());
            if (!fits) {
                return Fail(error_, "InvalidAnimation", keyPath,
                            "the bound prop gives no value of this "
                            "animation's kind: " +
                                value.Dump());
            }
            copy.values[index] = value;
        }
        if (animation.value_kind == MotionValueKind::Number) {
            KeyframeTrack track;
            for (const Keyframe& key : keys) {
                if (!track.Add(key)) {
                    return Fail(error_, "InvalidAnimation", path,
                                "a bound key does not make a track");
                }
            }
            copy.track = std::move(track);
        }
        out = &bound_.emplace(&animation, std::move(copy)).first->second;
        return true;
    }

    bool ResolveProps(const Value& overrides) {
        bound_.clear();
        for (const MotionPropDefinition& prop : composition_.props) {
            props_[prop.name] = prop.default_value;
            definitions_[prop.name] = &prop;
        }
        // A group (M3) is in definitions_ so that an override of it is
        // refused by type, as ValidateMotionPropValue says, not as unknown.
        if (overrides.IsNull()) {
            return true;
        }
        if (!overrides.IsObject()) {
            return Fail(error_, "InvalidValue", "props",
                        "props must be an object of overrides");
        }
        for (const auto& entry : overrides.AsObject()) {
            const auto definition = definitions_.find(entry.first);
            if (definition == definitions_.end()) {
                return Fail(
                    error_, "UnknownProp", "props." + entry.first,
                    "the composition declares no prop '" + entry.first + "'");
            }
            if (!ValidateMotionPropValue(*definition->second, entry.second,
                                         "props." + entry.first, error_)) {
                return false;
            }
            props_[entry.first] = entry.second;
        }
        return true;
    }

    bool Lookup(const MotionValue& value, const Value& key_value,
                const std::string& path, Value& out) {
        const std::string key = key_value.IsString() ? key_value.AsString()
                                : key_value.IsBool()
                                    ? (key_value.AsBool() ? "true" : "false")
                                    : key_value.Dump();
        const std::vector<std::pair<std::string, Value>>* entries =
            &value.inline_map;
        if (!value.table.empty()) {
            for (const MotionTokenTable& table : composition_.tokens) {
                if (table.name == value.table) {
                    entries = &table.entries;
                }
            }
        }
        for (const auto& entry : *entries) {
            if (entry.first == key) {
                out = entry.second;
                return true;
            }
        }
        return Fail(
            error_, "UnknownToken", path,
            "no entry '" + key + "' in " +
                (value.table.empty() ? "the inline map"
                                     : "token table '" + value.table + "'"));
    }

    // MOTION-AFTER-2026-09 -- HH:MM:SS:FF of a whole frame (the shutter's
    // instants of one frame show its timecode), at the nominal rate: 25 for
    // 25 fps, 30 for 30000/1001, never drop-frame.
    std::string Timecode(const Frames& at, int64_t offset) const {
        int64_t frame = at.numerator / at.denominator;
        if (at.numerator < 0 && at.numerator % at.denominator != 0) {
            --frame;
        }
        // MA7 -- the parser bounds the offset at 2^53; a composition long
        // enough for the sum to pass int64 shows the last timecode.
        int64_t shown = 0;
        if (__builtin_add_overflow(frame, offset, &shown)) {
            shown = offset < 0 ? 0 : std::numeric_limits<int64_t>::max();
        }
        return MotionTimecodeText(shown, composition_.fps_numerator,
                                  composition_.fps_denominator);
    }

    bool Resolve(const MotionValue& value, const std::string& path,
                 Value& out) {
        switch (value.kind) {
            case MotionBindingKind::Literal:
                out = value.literal;
                return true;
            case MotionBindingKind::Item:
                out = scopes_.back().item;
                return true;
            case MotionBindingKind::ItemMap:
                return Lookup(value, scopes_.back().item, path, out);
            case MotionBindingKind::Index:
                out = Value::MakeInt(scopes_.back().index);
                return true;
            case MotionBindingKind::Timecode:
                return Fail(error_, "InvalidValue", path,
                            "a timecode binds a text's content only");
            default:
                break;
        }
        const Value& whole = props_.at(value.prop);
        // M3 -- a point or size binds one component; the parser only lets a
        // binding name one that the prop's validated value holds.
        const Value* component =
            value.component.empty() ? &whole : whole.Find(value.component);
        const Value& prop = component != nullptr ? *component : whole;
        if (value.kind == MotionBindingKind::Prop) {
            // A number prop with a unit reads as that CSS length.
            const std::string& unit = definitions_.at(value.prop)->unit;
            out = unit.empty() || !prop.IsNumber()
                      ? prop
                      : Value::MakeString(prop.Dump() + unit);
            return true;
        }
        if (value.kind == MotionBindingKind::PropNotEmpty) {
            out = Value::MakeBool(prop.IsString() && !prop.AsString().empty());
            return true;
        }
        return Lookup(value, prop, path, out);
    }

    bool ToTime(const Frames& local, const std::string& path,
                RationalTime& time) {
        Frames seconds;
        if (!MakeFrames(static_cast<__int128>(local.numerator) *
                            composition_.fps_denominator,
                        static_cast<__int128>(local.denominator) *
                            composition_.fps_numerator,
                        seconds) ||
            seconds.denominator > std::numeric_limits<int32_t>::max()) {
            return Fail(error_, "TimeOverflow", path,
                        "local time cannot be represented exactly; simplify "
                        "the timeScale");
        }
        time = {seconds.numerator, static_cast<int32_t>(seconds.denominator)};
        return true;
    }

    // REM5 -- the key a segment runs to: the next one, or the only one of a
    // single-key animation, which holds.
    static size_t NextKey(const MotionAnimation& animation, size_t segment) {
        return std::min(segment + 1, animation.values.size() - 1);
    }

    // Eased progress through the current keyframe segment of a colour or
    // polygon interpolation: the track's values are the keyframe indices.
    bool Progress(const MotionAnimation& animation, const Frames& local,
                  const std::string& path, size_t& segment, double& fraction) {
        RationalTime time;
        if (!ToTime(local, path, time)) {
            return false;
        }
        const KeyframeEvaluation evaluation =
            EvaluateMotionTrack(animation, time);
        if (!evaluation) {
            return Fail(error_, "InvalidAnimation", path,
                        std::string("evaluation failed: ") +
                            KeyframeErrorName(evaluation.error));
        }
        const std::vector<Keyframe>& keyframes = animation.track.keyframes();
        segment = 0;
        while (segment + 2 < keyframes.size() &&
               keyframes[segment + 1].time <= time) {
            ++segment;
        }
        fraction = evaluation.value - static_cast<double>(segment);
        return true;
    }

    bool AnimatedNumber(const MotionAnimation& animation, const Frames& local,
                        const std::string& path, double& value) {
        if (animation.kind == MotionAnimationKind::Spring) {
            Frames elapsed;
            RationalTime time;
            if (!Minus(local, animation.delay, elapsed)) {
                return Fail(error_, "TimeOverflow", path,
                            "local time cannot be represented exactly");
            }
            if (!ToTime(elapsed, path, time)) {
                return false;
            }
            value = EvaluateMotionSpring(animation.spring, time);
            return true;
        }
        RationalTime time;
        if (!ToTime(local, path, time)) {
            return false;
        }
        const KeyframeEvaluation evaluation =
            EvaluateMotionTrack(animation, time);
        if (!evaluation) {
            return Fail(error_, "InvalidAnimation", path,
                        std::string("evaluation failed: ") +
                            KeyframeErrorName(evaluation.error));
        }
        value = evaluation.value;
        return true;
    }

    bool AnimatedColor(const MotionAnimation& animation, const Frames& local,
                       const std::string& path, MotionColor& color) {
        size_t segment = 0;
        double fraction = 0.0;
        if (!Progress(animation, local, path, segment, fraction)) {
            return false;
        }
        MotionColor from;
        MotionColor to;
        ParseMotionColor(animation.values[segment].AsString(), from);
        ParseMotionColor(
            animation.values[NextKey(animation, segment)].AsString(), to);
        color = MixColors(from, to, fraction);
        return true;
    }

    // Numbers and colours become style values, exactly as if written.
    bool Animated(const MotionAnimation& animation, const Frames& local,
                  const std::string& path, Value& out) {
        if (animation.value_kind == MotionValueKind::Color) {
            MotionColor color;
            if (!AnimatedColor(animation, local, path, color)) {
                return false;
            }
            out = Value::MakeString(ColorValue(color));
            return true;
        }
        double number = 0.0;
        if (!AnimatedNumber(animation, local, path, number)) {
            return false;
        }
        out = Value::MakeDouble(number);
        return true;
    }

    // The finest unit a text is painted by: its units, or the finest of its
    // ranges when it only has ranges.
    static MotionTextUnit PaintUnit(const MotionNode& node) {
        MotionTextUnit unit = node.units;
        if (unit == MotionTextUnit::None) {
            for (const MotionTextRange& range : node.ranges) {
                unit = std::max(unit, range.unit);
            }
        }
        return unit;
    }

    bool NonNegativeInteger(const MotionValue& value, const std::string& path,
                            int64_t& number) {
        Value resolved;
        if (!Resolve(value, path, resolved)) {
            return false;
        }
        if (!resolved.IsNumber() || !resolved.AsInt64(number) || number < 0) {
            return Fail(error_, "InvalidValue", path,
                        "must resolve to a non-negative integer");
        }
        return true;
    }

    bool Build(const MotionNode& node, const std::string& path,
               const std::string& suffix, Frames local, InheritedText inherited,
               YGNodeRef parent, std::vector<std::unique_ptr<PendingNode>>& out,
               const Overrides* overrides) {
        if (node.type == MotionNodeType::Sequence) {
            const bool before = Compare(local, node.from) < 0;
            const bool after = node.duration.has_value() &&
                               Compare(local, node.from + *node.duration) >= 0;
            if (before || after) {
                return true;
            }
            if (!Minus(local, node.from, local)) {
                return Fail(error_, "TimeOverflow", path,
                            "local time cannot be represented exactly");
            }
            if (node.time_scale.has_value()) {
                Value scale;
                int64_t numerator = 1;
                int64_t denominator = 1;
                if (!Resolve(*node.time_scale, path + ".timeScale", scale)) {
                    return false;
                }
                if (!ParseMotionTimeScale(scale, numerator, denominator)) {
                    return Fail(error_, "InvalidValue", path + ".timeScale",
                                "timeScale must resolve to a positive number "
                                "or a percentage, at most 1000");
                }
                if (!Scale(local, numerator, denominator, local)) {
                    return Fail(error_, "TimeOverflow", path + ".timeScale",
                                "local time cannot be represented exactly");
                }
            }
        }
        if (node.visible.has_value()) {
            Value visible;
            if (!Resolve(*node.visible, path + ".visible", visible)) {
                return false;
            }
            if (!visible.IsBool()) {
                return Fail(error_, "InvalidValue", path + ".visible",
                            "visible must resolve to a boolean");
            }
            if (!visible.AsBool()) {
                return true;
            }
        }
        if (node.type == MotionNodeType::Repeat) {
            return BuildRepeat(node, path, suffix, local, inherited, parent,
                               out);
        }
        if (node.type == MotionNodeType::Transition) {
            // Read by its series (SeriesTransitions); nothing to lay out.
            return true;
        }

        auto pending = std::make_unique<PendingNode>();
        if (const auto role = roles_.find(&node); role != roles_.end()) {
            pending->transition_role = role->second.first;
            pending->presented_by = role->second.second;
        }
        if (node.type == MotionNodeType::Series &&
            !SeriesTransitions(node, path, local, *pending)) {
            return false;
        }
        pending->source = &node;
        pending->path = path;
        pending->id = node.id.empty() ? std::string() : node.id + suffix;
        pending->local = local;
        for (const auto& entry : node.style) {
            Value resolved;
            const std::string entryPath = path + ".style." + entry.first;
            if (!Resolve(entry.second, entryPath, resolved)) {
                return false;
            }
            if (entry.second.kind != MotionBindingKind::Literal &&
                !ValidateMotionStyleValue(entry.first, resolved, entryPath,
                                          error_)) {
                return false;
            }
            SetStyle(*pending, entry.first, std::move(resolved));
        }
        if (overrides != nullptr) {
            // A repeat's cycle: this copy's value of each alternating style.
            for (const auto& entry : *overrides) {
                Value resolved;
                const std::string entryPath = path + ".cycle." + entry.first;
                if (!Resolve(*entry.second, entryPath, resolved)) {
                    return false;
                }
                if (entry.second->kind != MotionBindingKind::Literal &&
                    !ValidateMotionStyleValue(entry.first, resolved, entryPath,
                                              error_)) {
                    return false;
                }
                SetStyle(*pending, entry.first, std::move(resolved));
            }
        }
        // A text cut into units animates each unit after layout instead.
        if (node.units == MotionTextUnit::None) {
            for (const auto& written : node.animate) {
                const std::string entryPath =
                    path + ".animate." + written.first;
                // MB11 -- with this instance's values in the bound keys.
                const MotionAnimation* resolved = nullptr;
                if (!Bound(written.second, entryPath, resolved)) {
                    return false;
                }
                const std::pair<const std::string&, const MotionAnimation&>
                    entry{written.first, *resolved};
                if (entry.second.value_kind == MotionValueKind::Polygon) {
                    pending->clip_animation = &entry.second;
                    if (!Progress(entry.second, local, entryPath,
                                  pending->clip_segment,
                                  pending->clip_fraction)) {
                        return false;
                    }
                    continue;
                }
                if (entry.second.value_kind == MotionValueKind::Path) {
                    size_t segment = 0;
                    double fraction = 0.0;
                    if (!Progress(entry.second, local, entryPath, segment,
                                  fraction)) {
                        return false;
                    }
                    const std::vector<Value>& values = entry.second.values;
                    MotionPath from;
                    MotionPath to;
                    std::string message;
                    ParseMotionSvgPath(values[segment].AsString(), from,
                                       message);
                    ParseMotionSvgPath(
                        values[NextKey(entry.second, segment)].AsString(), to,
                        message);
                    pending->morph_geometry =
                        MixMotionPaths(from, to, fraction);
                    pending->has_morph = true;
                    continue;
                }
                if (entry.second.value_kind == MotionValueKind::Gradient ||
                    entry.second.value_kind == MotionValueKind::Filter) {
                    PendingNode::Deferred deferred;
                    deferred.animation = &entry.second;
                    if (!Progress(entry.second, local, entryPath,
                                  deferred.segment, deferred.fraction)) {
                        return false;
                    }
                    pending->deferred.emplace_back(entry.first, deferred);
                    continue;
                }
                Value animated;
                if (!Animated(entry.second, local, entryPath, animated)) {
                    return false;
                }
                SetStyle(*pending, entry.first, std::move(animated));
            }
        }

        Inherit(*pending, inherited);
        pending->text_style = inherited.style;
        pending->text_style.line_height =
            inherited.line_height_pixels > 0.0
                ? inherited.line_height_pixels
                : inherited.line_height_multiplier * inherited.style.font_size;
        pending->paint = inherited;

        pending->yoga = YGNodeNewWithConfig(config_);
        YGNodeInsertChild(parent, pending->yoga, YGNodeGetChildCount(parent));
        if (!ApplyLayout(*pending)) {
            return false;
        }

        if (node.type == MotionNodeType::Text ||
            node.type == MotionNodeType::Image) {
            Value resolved;
            const bool text = node.type == MotionNodeType::Text;
            const std::string field = text ? ".content" : ".src";
            if (text && node.content->kind == MotionBindingKind::Timecode) {
                resolved = Value::MakeString(Timecode(
                    node.content->timecode_scope == "local" ? local
                                                            : root_time_,
                    node.content->timecode_offset));
            } else if (!Resolve(text ? *node.content : *node.src, path + field,
                                resolved)) {
                return false;
            }
            if (!resolved.IsString()) {
                return Fail(error_, "InvalidValue", path + field,
                            std::string(text ? "content" : "src") +
                                " must resolve to a string");
            }
            MeasureContext& context = contexts_.emplace_back();
            context.measurer = &measurer_;
            context.kind = text ? MeasureKind::Text : MeasureKind::Image;
            context.path = path;
            context.error = &error_;
            context.failed = &measure_failed_;
            if (text) {
                pending->text = resolved.AsString();
                pending->unit_kind = PaintUnit(node);
                if (pending->unit_kind == MotionTextUnit::Char) {
                    pending->text_style.ligatures = false;
                }
                if (!ResolveRanges(node, path, *pending) ||
                    !ResolveHighlights(node, path, *pending)) {
                    return false;
                }
                context.text = pending->text;
                context.style = pending->text_style;
            } else {
                pending->image_source = resolved.AsString();
                context.source = pending->image_source;
            }
            YGNodeSetContext(pending->yoga, &context);
            YGNodeSetMeasureFunc(pending->yoga, MeasureNode);
        }
        if (node.type == MotionNodeType::Path &&
            !PreparePath(node, path, *pending)) {
            return false;
        }
        if ((node.type == MotionNodeType::Video ||
             node.type == MotionNodeType::Audio) &&
            !PrepareMedia(node, path, *pending)) {
            return false;
        }
        if (node.type == MotionNodeType::Composition &&
            !PrepareInstance(node, path, *pending)) {
            return false;
        }

        for (size_t index = 0; index < node.children.size(); ++index) {
            if (!Build(node.children[index],
                       path + ".children[" + std::to_string(index) + "]",
                       suffix, local, inherited, pending->yoga,
                       pending->children, nullptr)) {
                return false;
            }
        }
        out.push_back(std::move(pending));
        return true;
    }

    // REM3 -- the transition of a series running at `local`, if any, and
    // the role it gives the two sequences it joins: the one before exits,
    // the one after enters, both shown by its presentation at its progress.
    bool SeriesTransitions(const MotionNode& node, const std::string& path,
                           const Frames& local, PendingNode& pending) {
        // What an earlier evaluation of this series (a copy of a repeat)
        // gave its sequences does not carry over.
        for (const MotionNode& child : node.children) {
            roles_.erase(&child);
        }
        for (size_t index = 0; index < node.children.size(); ++index) {
            const MotionNode& child = node.children[index];
            if (child.type != MotionNodeType::Transition) {
                continue;
            }
            // TransitionFields gives every transition its duration.
            const int64_t duration = child.duration.value_or(0);
            const int64_t from = child.from;
            const int64_t to = child.from + duration;
            if (Compare(local, from) < 0 || Compare(local, to) >= 0) {
                continue;
            }
            const std::string childPath =
                path + ".children[" + std::to_string(index) + "]";
            Frames elapsed;
            RationalTime time;
            if (!Minus(local, from, elapsed) ||
                !ToTime(elapsed, childPath, time)) {
                return Fail(error_, "TimeOverflow", childPath,
                            "the transition's time cannot be represented "
                            "exactly");
            }
            MotionTransitionState state;
            state.active = true;
            state.uid = child.uid;
            state.presentation = child.presentation;
            state.direction = child.direction;
            state.from = from;
            state.to = to;
            if (child.timing_spring) {
                state.progress =
                    EvaluateMotionSpring(child.timing_spring_config, time);
            } else {
                KeyframeTrack track;
                track.Add({"00000000000000000000000001", RationalTime{0, 1},
                           0.0, child.timing_interpolation,
                           child.timing_easing});
                track.Add({"00000000000000000000000002",
                           MotionFrameTime(composition_, duration), 1.0,
                           KeyframeInterpolation::Linear, KeyframeEasing{}});
                const KeyframeEvaluation evaluation = track.Evaluate(time);
                if (!evaluation) {
                    return Fail(error_, "InvalidAnimation",
                                childPath + ".timing",
                                std::string("evaluation failed: ") +
                                    KeyframeErrorName(evaluation.error));
                }
                state.progress = evaluation.value;
            }
            pending.transition = state;
            roles_[&node.children[index - 1]] = {"exiting", state};
            roles_[&node.children[index + 1]] = {"entering", state};
        }
        return true;
    }

    // Copies of the template take the repeat's place among its siblings, as
    // an array mapped to elements does in React.
    bool BuildRepeat(const MotionNode& node, const std::string& path,
                     const std::string& suffix, const Frames& local,
                     const InheritedText& inherited, YGNodeRef parent,
                     std::vector<std::unique_ptr<PendingNode>>& out) {
        std::vector<Value> items;
        if (!RepeatItems(node, path, items)) {
            return false;
        }
        const std::vector<int64_t> ranks =
            StaggerRanks(node.stagger, items.size());
        const MotionNode& templ = node.children.front();
        for (size_t index = 0; index < items.size(); ++index) {
            const std::string copyPath =
                path + ".template[" + std::to_string(index) + "]";
            Frames copyLocal = local;
            if (!Minus(local, ranks[index] * node.stagger.each, copyLocal)) {
                return Fail(error_, "TimeOverflow", copyPath,
                            "local time cannot be represented exactly");
            }
            Overrides overrides;
            for (const auto& entry : node.cycle) {
                overrides.emplace_back(
                    entry.first, &entry.second[index % entry.second.size()]);
            }
            scopes_.push_back({items[index], static_cast<int64_t>(index)});
            const bool ok = Build(
                templ, copyPath, suffix + "[" + std::to_string(index) + "]",
                copyLocal, inherited, parent, out, &overrides);
            scopes_.pop_back();
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    // Range bounds and styles are resolved now, while bindings to the
    // enclosing repeat still have their scope; which units they cover is only
    // known after layout.
    bool ResolveRanges(const MotionNode& node, const std::string& path,
                       PendingNode& pending) {
        for (size_t index = 0; index < node.ranges.size(); ++index) {
            const MotionTextRange& range = node.ranges[index];
            const std::string rangePath =
                path + ".ranges[" + std::to_string(index) + "]";
            ResolvedRange resolved;
            resolved.unit = range.unit;
            if (!NonNegativeInteger(range.start, rangePath + ".start",
                                    resolved.start)) {
                return false;
            }
            if (range.count.has_value() &&
                !NonNegativeInteger(*range.count, rangePath + ".count",
                                    resolved.count)) {
                return false;
            }
            for (const auto& entry : range.style) {
                Value value;
                const std::string entryPath =
                    rangePath + ".style." + entry.first;
                if (!Resolve(entry.second, entryPath, value)) {
                    return false;
                }
                if (entry.second.kind != MotionBindingKind::Literal &&
                    !ValidateMotionStyleValue(entry.first, value, entryPath,
                                              error_)) {
                    return false;
                }
                resolved.style.emplace_back(entry.first, std::move(value));
            }
            pending.ranges.push_back(std::move(resolved));
        }
        return true;
    }

    // MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. A highlight's
    // bounds and colour resolve like a range's, while the enclosing repeat's
    // bindings are in scope; its progress is read now, at the text's local
    // frame, and clamped to [0, 1] as a spring may overshoot.
    bool ResolveHighlights(const MotionNode& node, const std::string& path,
                           PendingNode& pending) {
        for (size_t index = 0; index < node.highlights.size(); ++index) {
            const MotionTextHighlight& highlight = node.highlights[index];
            const std::string itemPath =
                path + ".highlights[" + std::to_string(index) + "]";
            ResolvedHighlight resolved;
            resolved.unit = highlight.unit;
            resolved.top = highlight.top;
            resolved.bottom = highlight.bottom;
            resolved.bleed = highlight.bleed;
            if (!NonNegativeInteger(highlight.start, itemPath + ".start",
                                    resolved.start)) {
                return false;
            }
            if (highlight.count.has_value() &&
                !NonNegativeInteger(*highlight.count, itemPath + ".count",
                                    resolved.count)) {
                return false;
            }
            Value color;
            if (!Resolve(highlight.color, itemPath + ".color", color)) {
                return false;
            }
            if (highlight.color.kind != MotionBindingKind::Literal &&
                !ValidateMotionStyleValue("color", color, itemPath + ".color",
                                          error_)) {
                return false;
            }
            ParseMotionColor(color.AsString(), resolved.color);
            if (highlight.progress_animation.has_value()) {
                const std::string progressPath = itemPath + ".progress";
                const MotionAnimation* bound = nullptr;
                Value value;
                if (!Bound(*highlight.progress_animation, progressPath,
                           bound) ||
                    !Animated(*bound, pending.local, progressPath, value)) {
                    return false;
                }
                resolved.progress = Number(value);
            } else if (highlight.progress.has_value()) {
                Value value;
                if (!Resolve(*highlight.progress, itemPath + ".progress",
                             value)) {
                    return false;
                }
                double number = 0.0;
                if (!MotionNumber(value, number)) {
                    return Fail(error_, "InvalidValue", itemPath + ".progress",
                                "progress must resolve to a number");
                }
                resolved.progress = number;
            }
            resolved.progress = std::clamp(resolved.progress, 0.0, 1.0);
            pending.highlights.push_back(resolved);
        }
        return true;
    }

    // MB3 -- the bands of each highlight, once layout has placed the units:
    // the covered units of each line, from the left of the first to the
    // right of the last, so that the spaces between them are painted too. A
    // line unit spans its words. Lines come in reading order, and the
    // progress fills them in that order.
    void HighlightBands(const PendingNode& pending, MotionEvaluatedNode& out) {
        for (size_t index = 0; index < pending.highlights.size(); ++index) {
            const ResolvedHighlight& highlight = pending.highlights[index];
            // Per line: the leftmost and rightmost edge of what it covers.
            std::map<size_t, std::pair<double, double>> spans;
            const auto cover = [&](size_t line, double left, double right) {
                const auto found = spans.find(line);
                if (found == spans.end()) {
                    spans.emplace(line, std::make_pair(left, right));
                } else {
                    found->second.first = std::min(found->second.first, left);
                    found->second.second =
                        std::max(found->second.second, right);
                }
            };
            const auto inRange = [&](size_t position) {
                const auto at = static_cast<int64_t>(position);
                return at >= highlight.start &&
                       at - highlight.start < highlight.count;
            };
            if (highlight.unit == MotionTextUnit::Char) {
                for (size_t at = 0; at < out.chars.size(); ++at) {
                    if (inRange(at)) {
                        const MotionTextSpan& span = out.chars[at];
                        cover(span.line, span.x, span.x + span.width);
                    }
                }
            } else {
                for (size_t at = 0; at < out.words.size(); ++at) {
                    const MotionTextSpan& span = out.words[at];
                    const size_t position =
                        highlight.unit == MotionTextUnit::Line ? span.line : at;
                    if (inRange(position)) {
                        cover(span.line, span.x, span.x + span.width);
                    }
                }
            }
            double total = 0.0;
            std::vector<MotionHighlightBand> bands;
            for (const auto& [line, edges] : spans) {
                if (line >= out.text_layout.lines.size()) {
                    continue;
                }
                const MotionTextLine& placed = out.text_layout.lines[line];
                MotionHighlightBand band;
                band.highlight = index;
                band.line = line;
                band.box = {edges.first - highlight.bleed,
                            placed.y + highlight.top * placed.height,
                            edges.second - edges.first + 2.0 * highlight.bleed,
                            (highlight.bottom - highlight.top) * placed.height};
                band.color = highlight.color;
                total += band.box.width;
                bands.push_back(band);
            }
            double remaining = highlight.progress * total;
            for (MotionHighlightBand& band : bands) {
                band.painted = std::clamp(remaining, 0.0, band.box.width);
                remaining -= band.painted;
                out.highlights.push_back(band);
            }
        }
    }

    bool PreparePath(const MotionNode& node, const std::string& path,
                     PendingNode& pending) {
        if (pending.has_morph) {
            pending.path_geometry = pending.morph_geometry;
        } else if (node.path_parsed) {
            pending.path_geometry = node.path;
        } else if (node.path_data.has_value()) {
            Value data;
            std::string message;
            if (!Resolve(*node.path_data, path + ".d", data)) {
                return false;
            }
            if (!data.IsString() ||
                !ParseMotionSvgPath(data.AsString(), pending.path_geometry,
                                    message)) {
                return Fail(error_, "InvalidPath", path + ".d",
                            data.IsString()
                                ? message
                                : "d must resolve to SVG path data");
            }
        }
        MeasureContext& context = contexts_.emplace_back();
        context.measurer = &measurer_;
        context.kind = MeasureKind::Path;
        context.path = path;
        context.error = &error_;
        context.failed = &measure_failed_;
        if (node.view_box.has_value()) {
            context.intrinsic_width = node.view_box->width;
            context.intrinsic_height = node.view_box->height;
            context.keep_ratio = true;
        } else {
            // Without a viewBox a path is drawn in the node's own pixels,
            // from its top-left corner: it is as large as it reaches.
            MotionPathBounds bounds;
            if (MotionPathBoundsOf(pending.path_geometry, bounds)) {
                context.intrinsic_width = std::max(0.0, bounds.max_x);
                context.intrinsic_height = std::max(0.0, bounds.max_y);
            }
        }
        YGNodeSetContext(pending.yoga, &context);
        YGNodeSetMeasureFunc(pending.yoga, MeasureNode);
        return true;
    }

    // REM2 -- a video's or a sound's media, what it is, and its speed. A
    // video is as large as its pictures, kept in ratio like an image.
    bool PrepareMedia(const MotionNode& node, const std::string& path,
                      PendingNode& pending) {
        const bool byId = node.media.has_value();
        const std::string field = byId ? ".media" : ".src";
        const std::optional<MotionValue>& source = byId ? node.media : node.src;
        // The parser gives a video or a sound its media or its src.
        if (!source.has_value()) {
            return Fail(error_, "InvalidValue", path + field,
                        "a video or a sound names its src or its media");
        }
        Value resolved;
        if (!Resolve(*source, path + field, resolved)) {
            return false;
        }
        if (!resolved.IsString() || resolved.AsString().empty() ||
            (byId && !IsValidUlid(resolved.AsString()))) {
            return Fail(error_, "InvalidValue", path + field,
                        byId ? "media must resolve to a project media's ULID"
                             : "src must resolve to a non-empty string");
        }
        (byId ? pending.media_id : pending.media_src) = resolved.AsString();
        MotionError error;
        if (!measurer_.MediaInfo(pending.media_src, pending.media_id,
                                 pending.media_info, error)) {
            error_ = error;
            if (error_.path.empty()) {
                error_.path = path + field;
            }
            return false;
        }
        const bool video = node.type == MotionNodeType::Video;
        if (video && !pending.media_info.has_video) {
            return Fail(error_, "MediaHasNoVideo", path + field,
                        "a video node shows a media with pictures; use an "
                        "audio node for a sound");
        }
        if (!video && !pending.media_info.has_audio) {
            return Fail(error_, "MediaHasNoAudio", path + field,
                        "an audio node plays a media with sound");
        }
        if (node.playback_rate.has_value()) {
            Value rate;
            if (!Resolve(*node.playback_rate, path + ".playbackRate", rate)) {
                return false;
            }
            if (!ParseMotionTimeScale(rate, pending.rate_numerator,
                                      pending.rate_denominator)) {
                return Fail(error_, "InvalidValue", path + ".playbackRate",
                            "playbackRate must resolve to a positive number "
                            "or a percentage, at most 1000");
            }
        }
        if (!MediaTimes(node, path, pending.start_from, pending.end_at)) {
            return false;
        }
        if (video) {
            MeasureContext& context = contexts_.emplace_back();
            context.measurer = &measurer_;
            context.kind = MeasureKind::Intrinsic;
            context.intrinsic_width = pending.media_info.width;
            context.intrinsic_height = pending.media_info.height;
            context.keep_ratio = true;
            context.path = path;
            context.error = &error_;
            context.failed = &measure_failed_;
            YGNodeSetContext(pending.yoga, &context);
            YGNodeSetMeasureFunc(pending.yoga, MeasureNode);
        }
        return true;
    }

    // REM2 -- the composition an instance shows, and the values it gives
    // it: each resolved here, where this composition's props and repeat
    // scope are, then held to the other composition's typed props with
    // M3's refusals. Its box is as large as that composition's canvas, kept
    // in ratio like an image's.
    bool PrepareInstance(const MotionNode& node, const std::string& path,
                         PendingNode& pending) {
        const MotionComposition* found = nullptr;
        if (!InstanceTarget(node, path, found) ||
            !InstanceProps(node, path, *found, pending.instance_props)) {
            return false;
        }
        const MotionComposition& child = *found;
        pending.instance = &child;
        MeasureContext& context = contexts_.emplace_back();
        context.measurer = &measurer_;
        context.kind = MeasureKind::Intrinsic;
        context.intrinsic_width = child.width;
        context.intrinsic_height = child.height;
        context.keep_ratio = true;
        context.path = path;
        context.error = &error_;
        context.failed = &measure_failed_;
        YGNodeSetContext(pending.yoga, &context);
        YGNodeSetMeasureFunc(pending.yoga, MeasureNode);
        return true;
    }

    static void SetStyle(PendingNode& node, const std::string& property,
                         Value value) {
        for (auto& entry : node.style) {
            if (entry.first == property) {
                entry.second = std::move(value);
                return;
            }
        }
        node.style.emplace_back(property, std::move(value));
    }

    static const Value* Style(const PendingNode& node, const char* property) {
        for (const auto& entry : node.style) {
            if (entry.first == property) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    static double Number(const Value& value) {
        double number = 0.0;
        if (MotionNumber(value, number)) {
            return number;
        }
        MotionLength length;
        if (ParseMotionLength(value, false, length)) {
            return length.value;
        }
        return 0.0;
    }

    void Inherit(const PendingNode& node, InheritedText& inherited) {
        MotionTextStyle& style = inherited.style;
        for (const auto& entry : node.style) {
            const std::string& property = entry.first;
            const Value& value = entry.second;
            if (property == "fontFamily") {
                style.font_family = value.AsString();
            } else if (property == "fontWeight") {
                style.font_weight =
                    value.IsString()
                        ? (value.AsString() == "bold" ? 700.0 : 400.0)
                        : Number(value);
            } else if (property == "fontSize") {
                style.font_size = std::max(0.0, Number(value));
            } else if (property == "lineHeight") {
                MotionLength length;
                if (value.IsString() &&
                    ParseMotionLength(value, false, length)) {
                    inherited.line_height_pixels = length.value;
                    inherited.line_height_multiplier = 0.0;
                } else {
                    inherited.line_height_multiplier =
                        std::max(0.0, Number(value));
                    inherited.line_height_pixels = 0.0;
                }
            } else if (property == "letterSpacing") {
                style.letter_spacing = Number(value);
            } else if (property == "wordSpacing") {
                style.word_spacing = Number(value);
            } else if (property == "textTransform") {
                style.text_transform = value.AsString();
            } else if (property == "textAlign") {
                style.text_align = value.AsString();
            } else if (property == "fontStyle") {
                style.italic = value.AsString() == "italic";
            } else if (property == "fontVariantNumeric") {
                ParseMotionNumericVariant(value.AsString(),
                                          style.numeric_figures,
                                          style.numeric_spacing);
            } else if (property == "color") {
                ParseMotionColor(value.AsString(), style.color);
            } else if (property == "textStroke") {
                std::istringstream parts(value.AsString());
                std::string width;
                std::string color;
                parts >> width >> color;
                style.stroke_width = 0.0;
                if (!color.empty() &&
                    ParseMotionColor(color, style.stroke_color)) {
                    style.stroke_width =
                        std::max(0.0, Number(Value::MakeString(width)));
                }
            } else if (property == "fill") {
                inherited.fill = value.AsString();
            } else if (property == "stroke") {
                inherited.stroke = value.AsString();
            } else if (property == "strokeWidth") {
                inherited.stroke_width = std::max(0.0, Number(value));
            } else if (property == "strokeLinecap") {
                inherited.linecap = value.AsString();
            } else if (property == "strokeLinejoin") {
                inherited.linejoin = value.AsString();
            } else if (property == "fillRule") {
                inherited.fill_rule = value.AsString();
            }
        }
    }

    static bool Keyword(const Value& value, const char* expected) {
        return value.IsString() && value.AsString() == expected;
    }

    static YGAlign Align(const std::string& keyword) {
        if (keyword == "auto") {
            return YGAlignAuto;
        }
        if (keyword == "flex-start") {
            return YGAlignFlexStart;
        }
        if (keyword == "center") {
            return YGAlignCenter;
        }
        if (keyword == "flex-end") {
            return YGAlignFlexEnd;
        }
        if (keyword == "baseline") {
            return YGAlignBaseline;
        }
        if (keyword == "space-between") {
            return YGAlignSpaceBetween;
        }
        if (keyword == "space-around") {
            return YGAlignSpaceAround;
        }
        if (keyword == "space-evenly") {
            return YGAlignSpaceEvenly;
        }
        return YGAlignStretch;
    }

    using EdgeSetter = void (*)(YGNodeRef, YGEdge, float);

    static void SetEdge(YGNodeRef node, YGEdge edge, const MotionLength& length,
                        EdgeSetter pixels, EdgeSetter percent,
                        void (*autoSetter)(YGNodeRef, YGEdge)) {
        if (length.unit == MotionLengthUnit::Auto) {
            if (autoSetter != nullptr) {
                autoSetter(node, edge);
            }
        } else if (length.unit == MotionLengthUnit::Percent) {
            percent(node, edge, static_cast<float>(length.value));
        } else {
            pixels(node, edge, static_cast<float>(length.value));
        }
    }

    // CSS box shorthand: 1 value for all sides, 2 for vertical/horizontal, 3
    // for top/horizontal/bottom, 4 for top/right/bottom/left.
    static void SetShorthand(YGNodeRef node, const Value& value,
                             EdgeSetter pixels, EdgeSetter percent,
                             void (*autoSetter)(YGNodeRef, YGEdge)) {
        std::vector<MotionLength> lengths;
        double number = 0.0;
        if (MotionNumber(value, number)) {
            lengths.push_back({MotionLengthUnit::Px, number});
        } else {
            std::istringstream stream(value.AsString());
            std::string part;
            while (stream >> part) {
                MotionLength length;
                ParseMotionLength(Value::MakeString(part), true, length);
                lengths.push_back(length);
            }
        }
        const MotionLength top = lengths[0];
        const MotionLength right = lengths.size() > 1 ? lengths[1] : top;
        const MotionLength bottom = lengths.size() > 2 ? lengths[2] : top;
        const MotionLength left = lengths.size() > 3 ? lengths[3] : right;
        SetEdge(node, YGEdgeTop, top, pixels, percent, autoSetter);
        SetEdge(node, YGEdgeRight, right, pixels, percent, autoSetter);
        SetEdge(node, YGEdgeBottom, bottom, pixels, percent, autoSetter);
        SetEdge(node, YGEdgeLeft, left, pixels, percent, autoSetter);
    }

    static void SetDimension(YGNodeRef node, const Value& value,
                             void (*pixels)(YGNodeRef, float),
                             void (*percent)(YGNodeRef, float),
                             void (*autoSetter)(YGNodeRef)) {
        MotionLength length;
        ParseMotionLength(value, autoSetter != nullptr, length);
        if (length.unit == MotionLengthUnit::Auto) {
            autoSetter(node);
        } else if (length.unit == MotionLengthUnit::Percent) {
            percent(node, static_cast<float>(length.value));
        } else {
            // CSS computes a negative size as zero; an animation may pass
            // through one without that being an error.
            pixels(node, static_cast<float>(std::max(0.0, length.value)));
        }
    }

    bool ApplyLayout(const PendingNode& pending) {
        const YGNodeRef node = pending.yoga;
        const MotionNodeType type = pending.source->type;
        if (type == MotionNodeType::Fill || type == MotionNodeType::Sequence ||
            type == MotionNodeType::Series) {
            // Remotion's AbsoluteFill: positioned over the whole parent, a
            // flex column container. A series is one too, as each of its
            // sequences is.
            YGNodeStyleSetPositionType(node, YGPositionTypeAbsolute);
            for (YGEdge edge :
                 {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft}) {
                YGNodeStyleSetPosition(node, edge, 0.0f);
            }
            YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
        }
        if (type == MotionNodeType::Audio) {
            // A sound takes no place among its siblings.
            YGNodeStyleSetPositionType(node, YGPositionTypeAbsolute);
            YGNodeStyleSetWidth(node, 0.0f);
            YGNodeStyleSetHeight(node, 0.0f);
            return true;
        }
        for (const auto& entry : pending.style) {
            const std::string& property = entry.first;
            const Value& value = entry.second;
            MotionLength length;
            if (property == "flexDirection") {
                YGNodeStyleSetFlexDirection(
                    node,
                    Keyword(value, "row")           ? YGFlexDirectionRow
                    : Keyword(value, "row-reverse") ? YGFlexDirectionRowReverse
                    : Keyword(value, "column")      ? YGFlexDirectionColumn
                                               : YGFlexDirectionColumnReverse);
            } else if (property == "justifyContent") {
                const std::string& keyword = value.AsString();
                YGNodeStyleSetJustifyContent(
                    node, keyword == "center"          ? YGJustifyCenter
                          : keyword == "flex-end"      ? YGJustifyFlexEnd
                          : keyword == "space-between" ? YGJustifySpaceBetween
                          : keyword == "space-around"  ? YGJustifySpaceAround
                          : keyword == "space-evenly"  ? YGJustifySpaceEvenly
                                                       : YGJustifyFlexStart);
            } else if (property == "alignItems") {
                YGNodeStyleSetAlignItems(node, Align(value.AsString()));
            } else if (property == "alignSelf") {
                YGNodeStyleSetAlignSelf(node, Align(value.AsString()));
            } else if (property == "alignContent") {
                YGNodeStyleSetAlignContent(node, Align(value.AsString()));
            } else if (property == "flexWrap") {
                YGNodeStyleSetFlexWrap(node, Keyword(value, "wrap") ? YGWrapWrap
                                             : Keyword(value, "wrap-reverse")
                                                 ? YGWrapWrapReverse
                                                 : YGWrapNoWrap);
            } else if (property == "position") {
                YGNodeStyleSetPositionType(node, Keyword(value, "absolute")
                                                     ? YGPositionTypeAbsolute
                                                     : YGPositionTypeRelative);
            } else if (property == "flexGrow") {
                YGNodeStyleSetFlexGrow(node, static_cast<float>(Number(value)));
            } else if (property == "flexShrink") {
                YGNodeStyleSetFlexShrink(node,
                                         static_cast<float>(Number(value)));
            } else if (property == "flexBasis") {
                SetDimension(node, value, YGNodeStyleSetFlexBasis,
                             YGNodeStyleSetFlexBasisPercent,
                             YGNodeStyleSetFlexBasisAuto);
            } else if (property == "width") {
                SetDimension(node, value, YGNodeStyleSetWidth,
                             YGNodeStyleSetWidthPercent,
                             YGNodeStyleSetWidthAuto);
            } else if (property == "height") {
                SetDimension(node, value, YGNodeStyleSetHeight,
                             YGNodeStyleSetHeightPercent,
                             YGNodeStyleSetHeightAuto);
            } else if (property == "minWidth") {
                SetDimension(node, value, YGNodeStyleSetMinWidth,
                             YGNodeStyleSetMinWidthPercent, nullptr);
            } else if (property == "minHeight") {
                SetDimension(node, value, YGNodeStyleSetMinHeight,
                             YGNodeStyleSetMinHeightPercent, nullptr);
            } else if (property == "maxWidth") {
                SetDimension(node, value, YGNodeStyleSetMaxWidth,
                             YGNodeStyleSetMaxWidthPercent, nullptr);
            } else if (property == "maxHeight") {
                SetDimension(node, value, YGNodeStyleSetMaxHeight,
                             YGNodeStyleSetMaxHeightPercent, nullptr);
            } else if (property == "top" || property == "right" ||
                       property == "bottom" || property == "left") {
                ParseMotionLength(value, false, length);
                const YGEdge edge = property == "top"      ? YGEdgeTop
                                    : property == "right"  ? YGEdgeRight
                                    : property == "bottom" ? YGEdgeBottom
                                                           : YGEdgeLeft;
                SetEdge(node, edge, length, YGNodeStyleSetPosition,
                        YGNodeStyleSetPositionPercent, nullptr);
            } else if (property == "inset") {
                SetShorthand(node, value, YGNodeStyleSetPosition,
                             YGNodeStyleSetPositionPercent, nullptr);
            } else if (property == "gap" || property == "rowGap" ||
                       property == "columnGap") {
                ParseMotionLength(value, false, length);
                const YGGutter gutter = property == "gap"      ? YGGutterAll
                                        : property == "rowGap" ? YGGutterRow
                                                               : YGGutterColumn;
                if (length.unit == MotionLengthUnit::Percent) {
                    YGNodeStyleSetGapPercent(node, gutter,
                                             static_cast<float>(length.value));
                } else {
                    YGNodeStyleSetGap(
                        node, gutter,
                        static_cast<float>(std::max(0.0, length.value)));
                }
            } else if (property == "padding") {
                SetShorthand(node, value, YGNodeStyleSetPadding,
                             YGNodeStyleSetPaddingPercent, nullptr);
            } else if (property.rfind("padding", 0) == 0) {
                ParseMotionLength(value, false, length);
                length.value = std::max(0.0, length.value);
                SetEdge(node, EdgeFromSuffix(property, 7), length,
                        YGNodeStyleSetPadding, YGNodeStyleSetPaddingPercent,
                        nullptr);
            } else if (property == "margin") {
                SetShorthand(node, value, YGNodeStyleSetMargin,
                             YGNodeStyleSetMarginPercent,
                             YGNodeStyleSetMarginAuto);
            } else if (property.rfind("margin", 0) == 0) {
                ParseMotionLength(value, true, length);
                SetEdge(node, EdgeFromSuffix(property, 6), length,
                        YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent,
                        YGNodeStyleSetMarginAuto);
            }
        }
        return true;
    }

    static YGEdge EdgeFromSuffix(const std::string& property, size_t prefix) {
        const std::string side = property.substr(prefix);
        if (side == "Top") {
            return YGEdgeTop;
        }
        if (side == "Right") {
            return YGEdgeRight;
        }
        if (side == "Bottom") {
            return YGEdgeBottom;
        }
        return YGEdgeLeft;
    }

    bool Extract(const PendingNode& pending, double parentX, double parentY,
                 MotionEvaluatedNode& out) {
        const YGNodeRef node = pending.yoga;
        out.path = pending.path;
        out.id = pending.id;
        out.uid = pending.source->uid;
        out.type = pending.source->type;
        out.decor = pending.source->decor;
        out.box.x = parentX + YGNodeLayoutGetLeft(node);
        out.box.y = parentY + YGNodeLayoutGetTop(node);
        out.box.width = YGNodeLayoutGetWidth(node);
        out.box.height = YGNodeLayoutGetHeight(node);
        if (out.type == MotionNodeType::Sequence ||
            out.type == MotionNodeType::Series) {
            out.has_local_frame = true;
            out.local_numerator = pending.local.numerator;
            out.local_denominator = pending.local.denominator;
        }

        for (const auto& entry : pending.style) {
            const std::string& property = entry.first;
            const Value& value = entry.second;
            if (property == "opacity") {
                out.opacity = std::clamp(Number(value), 0.0, 1.0);
            } else if (property == "translateX") {
                out.translate_x = Number(value);
            } else if (property == "translateY") {
                out.translate_y = Number(value);
            } else if (property == "scale") {
                out.scale_x = out.scale_y = Number(value);
            } else if (property == "scaleX") {
                out.scale_x = Number(value);
            } else if (property == "scaleY") {
                out.scale_y = Number(value);
            } else if (property == "rotate") {
                out.rotate = Number(value);
            } else if (property == "rotateX") {
                out.rotate_x = Number(value);
            } else if (property == "rotateY") {
                out.rotate_y = Number(value);
            } else if (property == "perspective") {
                out.perspective = value.IsString() && value.AsString() == "none"
                                      ? 0.0
                                      : std::max(0.0, Number(value));
            } else if (property == "backgroundColor") {
                out.has_background =
                    ParseMotionColor(value.AsString(), out.background);
            } else if (property == "borderRadius") {
                out.border_radius = std::max(0.0, Number(value));
            } else if (property == "objectFit") {
                out.object_fit = value.AsString();
            } else if (property == "overflow") {
                out.overflow_hidden = value.AsString() == "hidden";
            } else if (property == "isolation") {
                out.isolate = value.AsString() == "isolate";
            } else if (property == "mixBlendMode") {
                out.blend_mode =
                    value.AsString() == "normal" ? "" : value.AsString();
            } else if (property == "volume") {
                out.media.volume = std::clamp(Number(value), 0.0, 4.0);
            } else if (property == "clipPath" &&
                       pending.clip_animation == nullptr) {
                MotionClipShape shape;
                if (ParseMotionClipShape(value.AsString(), shape)) {
                    SetClip(ResolveClip(shape, out.box), out);
                }
            } else if (property == "backgroundImage" &&
                       !HasDeferred(pending, property)) {
                MotionGradient gradient;
                if (ParseMotionGradient(value.AsString(), gradient)) {
                    out.has_gradient = true;
                    out.gradient = ResolveGradient(gradient, out.box);
                }
            } else if (property == "maskImage" &&
                       !HasDeferred(pending, property)) {
                MotionGradient gradient;
                if (ParseMotionGradient(value.AsString(), gradient)) {
                    out.has_mask = true;
                    out.mask = ResolveGradient(gradient, out.box);
                }
            } else if (property == "filter" &&
                       !HasDeferred(pending, property)) {
                ParseMotionFilters(value.AsString(), out.filters);
                ResolveMotionMeshes(out.filters, out.box.x, out.box.y,
                                    out.box.width, out.box.height);
            }
        }
        if (pending.clip_animation != nullptr) {
            // CSS resolves both shapes against the box, then moves each
            // vertex, centre and radius along a straight line.
            const std::vector<Value>& values = pending.clip_animation->values;
            MotionClipShape from;
            MotionClipShape to;
            ParseMotionClipShape(values[pending.clip_segment].AsString(), from);
            ParseMotionClipShape(
                values[NextKey(*pending.clip_animation, pending.clip_segment)]
                    .AsString(),
                to);
            SetClip(MixClips(ResolveClip(from, out.box),
                             ResolveClip(to, out.box), pending.clip_fraction),
                    out);
        }
        for (const auto& [property, deferred] : pending.deferred) {
            const std::vector<Value>& values = deferred.animation->values;
            const std::string& from = values[deferred.segment].AsString();
            const std::string& to =
                values[NextKey(*deferred.animation, deferred.segment)]
                    .AsString();
            if (property == "filter") {
                std::vector<MotionFilter> a;
                std::vector<MotionFilter> b;
                ParseMotionFilters(from, a);
                ParseMotionFilters(to, b);
                // mesh(): both grids resolved against the box, then each
                // vertex moved along a straight line, as a polygon's.
                ResolveMotionMeshes(a, out.box.x, out.box.y, out.box.width,
                                    out.box.height);
                ResolveMotionMeshes(b, out.box.x, out.box.y, out.box.width,
                                    out.box.height);
                out.filters = MixFilters(a, b, deferred.fraction);
                continue;
            }
            MotionGradient a;
            MotionGradient b;
            ParseMotionGradient(from, a);
            ParseMotionGradient(to, b);
            MotionResolvedGradient& target =
                property == "maskImage" ? out.mask : out.gradient;
            (property == "maskImage" ? out.has_mask : out.has_gradient) = true;
            target = MixGradients(a, b, deferred.fraction, out.box);
        }
        if (!pending.transition_role.empty()) {
            Present(pending, out);
        }
        out.transition = pending.transition;
        // REM4 -- a node turned in depth is placed by a 4x4 matrix, under
        // its parent's perspective about the parent's centre.
        if (out.rotate_x != 0.0 || out.rotate_y != 0.0) {
            out.has_3d = true;
            MotionNodeMatrix3D(out, parent_perspective_, parent_origin_x_,
                               parent_origin_y_, out.matrix3d);
        }

        if (out.type == MotionNodeType::Text) {
            out.text = pending.text;
            out.text_style = pending.text_style;
            const double left = YGNodeLayoutGetPadding(node, YGEdgeLeft);
            const double right = YGNodeLayoutGetPadding(node, YGEdgeRight);
            const double top = YGNodeLayoutGetPadding(node, YGEdgeTop);
            MotionError error;
            if (!measurer_.LayoutText(
                    out.text, out.text_style,
                    std::max(0.0, out.box.width - left - right),
                    out.text_layout, error)) {
                error_ = error;
                if (error_.path.empty()) {
                    error_.path = pending.path;
                }
                return false;
            }
            for (MotionTextLine& line : out.text_layout.lines) {
                line.x += left;
                line.y += top;
                line.baseline += top;
            }
            CutUnits(out);
            if (!PaintUnits(pending, out)) {
                return false;
            }
            HighlightBands(pending, out);
        } else if (out.type == MotionNodeType::Image) {
            out.image_source = pending.image_source;
        } else if (out.type == MotionNodeType::Path) {
            PaintPath(pending, out);
        } else if (out.type == MotionNodeType::Video ||
                   out.type == MotionNodeType::Audio) {
            if (!MediaState(pending, out)) {
                return false;
            }
        } else if (out.type == MotionNodeType::Composition) {
            if (!InstanceState(pending, out)) {
                return false;
            }
        }

        // REM4 -- this node's perspective is its children's.
        const double perspective = parent_perspective_;
        const double originX = parent_origin_x_;
        const double originY = parent_origin_y_;
        parent_perspective_ = out.perspective;
        parent_origin_x_ = out.box.x + out.box.width / 2.0;
        parent_origin_y_ = out.box.y + out.box.height / 2.0;
        for (const auto& child : pending.children) {
            MotionEvaluatedNode evaluated;
            if (!Extract(*child, out.box.x, out.box.y, evaluated)) {
                return false;
            }
            out.children.push_back(std::move(evaluated));
        }
        parent_perspective_ = perspective;
        parent_origin_x_ = originX;
        parent_origin_y_ = originY;
        return true;
    }

    // REM2 -- what a video or a sound plays now. Media time is
    // (startFrom + local * playbackRate) / fps, an exact fraction; the
    // picture shown is the one of the media's cadence that contains it.
    bool MediaState(const PendingNode& pending, MotionEvaluatedNode& out) {
        const MotionNode& node = *pending.source;
        MotionMediaState& media = out.media;
        media.src = pending.media_src;
        media.media = pending.media_id;
        media.muted = node.muted;
        const __int128 scale =
            static_cast<__int128>(pending.local.denominator) *
            pending.rate_denominator;
        // Position in frames of the composition's cadence, over `scale`.
        const __int128 position =
            static_cast<__int128>(pending.start_from) * scale +
            static_cast<__int128>(pending.local.numerator) *
                pending.rate_numerator;
        Frames seconds;
        if (!MakeFrames(position * composition_.fps_denominator,
                        scale * composition_.fps_numerator, seconds)) {
            return Fail(error_, "TimeOverflow", pending.path,
                        "media time cannot be represented exactly");
        }
        media.time_numerator = seconds.numerator;
        media.time_denominator = seconds.denominator;
        const MotionMediaInfo& info = pending.media_info;
        const bool ended =
            (pending.end_at.has_value() &&
             position >= static_cast<__int128>(*pending.end_at) * scale) ||
            static_cast<__int128>(seconds.numerator) * info.duration.rate >=
                static_cast<__int128>(info.duration.value) *
                    seconds.denominator;
        media.state = Compare(pending.local, 0) < 0 ? "before"
                      : ended                       ? "after"
                                                    : "playing";
        if (out.type == MotionNodeType::Video && info.rate_numerator > 0 &&
            media.state == "playing") {
            const __int128 top =
                static_cast<__int128>(seconds.numerator) * info.rate_numerator;
            const __int128 bottom = static_cast<__int128>(seconds.denominator) *
                                    info.rate_denominator;
            media.frame = static_cast<int64_t>(top / bottom);
        }
        return true;
    }

    // REM2 -- an instance: the other composition's frame at this instant,
    // evaluated on its own canvas, and how that canvas maps into this box.
    bool InstanceState(const PendingNode& pending, MotionEvaluatedNode& out) {
        const MotionComposition& child = *pending.instance;
        MotionInstanceState& instance = out.instance;
        instance.composition_id = pending.source->instance_of;
        instance.width = child.width;
        instance.height = child.height;
        // Same instant, the other composition's cadence.
        Frames local;
        if (!MakeFrames(static_cast<__int128>(pending.local.numerator) *
                            child.fps_numerator * composition_.fps_denominator,
                        static_cast<__int128>(pending.local.denominator) *
                            child.fps_denominator * composition_.fps_numerator,
                        local)) {
            return Fail(error_, "TimeOverflow", pending.path,
                        "the instance's time cannot be represented exactly");
        }
        instance.local_numerator = local.numerator;
        instance.local_denominator = local.denominator;
        const double width = out.box.width;
        const double height = out.box.height;
        double scaleX = width / child.width;
        double scaleY = height / child.height;
        if (out.object_fit == "contain") {
            scaleX = scaleY = std::min(scaleX, scaleY);
        } else if (out.object_fit == "cover") {
            scaleX = scaleY = std::max(scaleX, scaleY);
        } else if (out.object_fit == "none") {
            scaleX = scaleY = 1.0;
        }
        instance.scale_x = scaleX;
        instance.scale_y = scaleY;
        instance.origin_x = out.box.x + (width - child.width * scaleX) / 2.0;
        instance.origin_y = out.box.y + (height - child.height * scaleY) / 2.0;
        if (Compare(local, 0) < 0) {
            instance.state = "before";
            return true;
        }
        if (Compare(local, child.duration) >= 0) {
            instance.state = "after";
            return true;
        }
        std::vector<std::string> stack = stack_;
        stack.push_back(instance.composition_id);
        MotionEvaluationContext nested = context_;
        nested.composition_id = instance.composition_id;
        Evaluator evaluator(child, measurer_, error_, nested, std::move(stack));
        MotionEvaluatedNode root;
        if (!evaluator.RunInstance(local, pending.instance_props, root)) {
            error_.path = pending.path + " > " + error_.path;
            return false;
        }
        out.instance_root.push_back(std::move(root));
        return true;
    }

    static bool HasDeferred(const PendingNode& pending,
                            const std::string& property) {
        for (const auto& entry : pending.deferred) {
            if (entry.first == property) {
                return true;
            }
        }
        return false;
    }

    // A polygon keeps M1b's spelling (`clip`); every shape is also in
    // `clip_shape`, which the painter draws.
    static void SetClip(MotionResolvedClip clip, MotionEvaluatedNode& out) {
        out.has_clip = true;
        out.clip.clear();
        if (clip.kind == MotionClipKind::Polygon) {
            out.clip = clip.points;
        }
        out.clip_shape = std::move(clip);
    }

    // REM3 -- how a transition shows a sequence it joins, in its own box:
    // a fade mixes the two (the entering one added onto the exiting one,
    // so that two opaque scenes never dip), a slide pushes, a wipe or an
    // iris uncovers the entering one over the exiting one.
    static void Present(const PendingNode& pending, MotionEvaluatedNode& out) {
        const MotionTransitionState& state = pending.presented_by;
        const double p = state.progress;
        const bool entering = pending.transition_role == "entering";
        out.transition_role = pending.transition_role;
        out.transition_progress = p;
        const double width = out.box.width;
        const double height = out.box.height;
        switch (state.presentation) {
            case MotionPresentation::Fade:
                out.opacity *= std::clamp(entering ? p : 1.0 - p, 0.0, 1.0);
                out.blend_plus_lighter = entering;
                break;
            case MotionPresentation::Slide: {
                double dx = 0.0;
                double dy = 0.0;
                switch (state.direction) {
                    case MotionDirection::FromLeft:
                        dx = -width;
                        break;
                    case MotionDirection::FromRight:
                        dx = width;
                        break;
                    case MotionDirection::FromTop:
                        dy = -height;
                        break;
                    case MotionDirection::FromBottom:
                        dy = height;
                        break;
                }
                // The entering scene comes from the side, the exiting one is
                // pushed out the other way.
                const double travel = entering ? 1.0 - p : -p;
                out.translate_x += dx * travel;
                out.translate_y += dy * travel;
                break;
            }
            case MotionPresentation::Wipe: {
                if (!entering) {
                    break;
                }
                MotionResolvedClip clip;
                clip.kind = MotionClipKind::Inset;
                const double shown = std::clamp(p, 0.0, 1.0);
                clip.rect = out.box;
                switch (state.direction) {
                    case MotionDirection::FromLeft:
                        clip.rect.width = width * shown;
                        break;
                    case MotionDirection::FromRight:
                        clip.rect.x += width * (1.0 - shown);
                        clip.rect.width = width * shown;
                        break;
                    case MotionDirection::FromTop:
                        clip.rect.height = height * shown;
                        break;
                    case MotionDirection::FromBottom:
                        clip.rect.y += height * (1.0 - shown);
                        clip.rect.height = height * shown;
                        break;
                }
                out.transition_clip = clip;
                break;
            }
            case MotionPresentation::Iris: {
                if (!entering) {
                    break;
                }
                MotionResolvedClip clip;
                clip.kind = MotionClipKind::Circle;
                clip.cx = out.box.x + width / 2.0;
                clip.cy = out.box.y + height / 2.0;
                clip.rx = clip.ry =
                    std::max(0.0, p) * std::hypot(width, height) / 2.0;
                out.transition_clip = clip;
                break;
            }
        }
    }

    // Words and characters, cut from the clusters the measurer placed: the
    // same glyph positions the renderer draws, so a word's box is where the
    // word is.
    static void CutUnits(MotionEvaluatedNode& out) {
        for (size_t lineIndex = 0; lineIndex < out.text_layout.lines.size();
             ++lineIndex) {
            const MotionTextLine& line = out.text_layout.lines[lineIndex];
            bool inWord = false;
            for (const MotionTextCluster& cluster : line.clusters) {
                const std::string text =
                    line.text.substr(cluster.start, cluster.length);
                if (IsSpaceCluster(text)) {
                    inWord = false;
                    continue;
                }
                MotionTextSpan span;
                span.text = text;
                span.line = lineIndex;
                span.x = line.x + cluster.x;
                span.y = line.y;
                span.width = cluster.width;
                span.height = line.height;
                span.baseline = line.baseline;
                if (!inWord) {
                    MotionTextSpan word = span;
                    word.word = out.words.size();
                    out.words.push_back(word);
                    inWord = true;
                } else {
                    MotionTextSpan& word = out.words.back();
                    word.text += text;
                    word.width = span.x + span.width - word.x;
                }
                span.word = out.words.size() - 1;
                out.chars.push_back(std::move(span));
            }
        }
    }

    static size_t UnitCount(const MotionEvaluatedNode& out,
                            MotionTextUnit unit) {
        switch (unit) {
            case MotionTextUnit::Line:
                return out.text_layout.lines.size();
            case MotionTextUnit::Word:
                return out.words.size();
            case MotionTextUnit::Char:
                return out.chars.size();
            case MotionTextUnit::None:
                break;
        }
        return 0;
    }

    // Index, in `unit`, of the unit `index` of the text's paint unit: a word
    // is in one line, a character in one word and one line.
    static size_t IndexIn(const MotionEvaluatedNode& out, MotionTextUnit unit,
                          size_t index) {
        if (unit == out.unit_kind) {
            return index;
        }
        if (out.unit_kind == MotionTextUnit::Word) {
            return out.words[index].line;
        }
        return unit == MotionTextUnit::Line ? out.chars[index].line
                                            : out.chars[index].word;
    }

    bool PaintUnits(const PendingNode& pending, MotionEvaluatedNode& out) {
        out.unit_kind = pending.unit_kind;
        if (out.unit_kind == MotionTextUnit::None) {
            return true;
        }
        const MotionNode& source = *pending.source;
        const size_t count = UnitCount(out, out.unit_kind);
        const std::vector<int64_t> ranks = StaggerRanks(source.stagger, count);
        for (size_t index = 0; index < count; ++index) {
            MotionTextUnitState state;
            state.index = index;
            state.color = out.text_style.color;
            for (const ResolvedRange& range : pending.ranges) {
                const auto position =
                    static_cast<int64_t>(IndexIn(out, range.unit, index));
                if (position < range.start ||
                    position - range.start >= range.count) {
                    continue;
                }
                for (const auto& entry : range.style) {
                    if (entry.first == "color") {
                        ParseMotionColor(entry.second.AsString(), state.color);
                    } else if (entry.first == "backgroundColor") {
                        state.has_background = ParseMotionColor(
                            entry.second.AsString(), state.background);
                    } else if (entry.first == "opacity") {
                        state.opacity =
                            std::clamp(Number(entry.second), 0.0, 1.0);
                    }
                }
            }
            if (source.units != MotionTextUnit::None) {
                state.delay = ranks[index] * source.stagger.each;
                Frames local;
                if (!Minus(pending.local, state.delay, local)) {
                    return Fail(error_, "TimeOverflow", pending.path,
                                "local time cannot be represented exactly");
                }
                for (const auto& entry : source.animate) {
                    const std::string entryPath =
                        pending.path + ".animate." + entry.first;
                    // MB11 -- with this instance's values in the bound keys.
                    const MotionAnimation* resolved = nullptr;
                    Value value;
                    if (!Bound(entry.second, entryPath, resolved) ||
                        !Animated(*resolved, local, entryPath, value)) {
                        return false;
                    }
                    ApplyUnitProperty(entry.first, value, state);
                }
            }
            out.units.push_back(state);
        }
        return true;
    }

    static void ApplyUnitProperty(const std::string& property,
                                  const Value& value,
                                  MotionTextUnitState& state) {
        if (property == "opacity") {
            state.opacity = std::clamp(Number(value), 0.0, 1.0);
        } else if (property == "translateX") {
            state.translate_x = Number(value);
        } else if (property == "translateY") {
            state.translate_y = Number(value);
        } else if (property == "scale") {
            state.scale_x = state.scale_y = Number(value);
        } else if (property == "scaleX") {
            state.scale_x = Number(value);
        } else if (property == "scaleY") {
            state.scale_y = Number(value);
        } else if (property == "rotate") {
            state.rotate = Number(value);
        } else if (property == "color") {
            ParseMotionColor(value.AsString(), state.color);
        } else if (property == "backgroundColor") {
            state.has_background =
                ParseMotionColor(value.AsString(), state.background);
        }
    }

    static bool Paint(const std::string& value, const MotionColor& current,
                      MotionColor& color) {
        if (value == "none") {
            return false;
        }
        if (value == "currentColor") {
            color = current;
            return true;
        }
        return ParseMotionColor(value, color);
    }

    // The viewBox maps into the box (xMidYMid meet, SVG's default); without
    // one, path units are the node's pixels from its top-left corner.
    static void PaintPath(const PendingNode& pending,
                          MotionEvaluatedNode& out) {
        MotionPathPaint& paint = out.path_paint;
        MotionPath geometry = pending.path_geometry;
        double scale = 1.0;
        double dx = out.box.x;
        double dy = out.box.y;
        if (pending.source->view_box.has_value()) {
            const MotionViewBox& view = *pending.source->view_box;
            scale = std::min(out.box.width / view.width,
                             out.box.height / view.height);
            dx = out.box.x + (out.box.width - view.width * scale) / 2.0 -
                 view.x * scale;
            dy = out.box.y + (out.box.height - view.height * scale) / 2.0 -
                 view.y * scale;
        }
        TransformMotionPath(geometry, scale, dx, dy);
        for (const auto& entry : pending.style) {
            if (entry.first == "trimStart") {
                paint.trim_start = std::clamp(Number(entry.second), 0.0, 1.0);
            } else if (entry.first == "trimEnd") {
                paint.trim_end = std::clamp(Number(entry.second), 0.0, 1.0);
            } else if (entry.first == "strokeDashoffset") {
                paint.dash_offset = Number(entry.second) * scale;
            } else if (entry.first == "strokeDasharray" &&
                       entry.second.IsString()) {
                // Validated by the parser, or when its prop was resolved.
                paint.dash.clear();
                ParseMotionDashArray(entry.second.AsString(), paint.dash);
                for (double& length : paint.dash) {
                    length *= scale;
                }
                // A pattern of nothing but gaps draws nothing; SVG ignores it.
                double total = 0.0;
                for (const double part : paint.dash) {
                    total += part;
                }
                if (total <= 0.0) {
                    paint.dash.clear();
                }
            }
        }
        paint.length = MotionPathLength(geometry);
        paint.geometry =
            TrimMotionPath(geometry, paint.trim_start, paint.trim_end);
        MotionPathBounds bounds;
        paint.has_bounds = MotionPathBoundsOf(paint.geometry, bounds);
        if (paint.has_bounds) {
            paint.bounds = {bounds.min_x, bounds.min_y,
                            bounds.max_x - bounds.min_x,
                            bounds.max_y - bounds.min_y};
        }
        const InheritedText& inherited = pending.paint;
        const MotionColor& current = pending.text_style.color;
        paint.has_fill = Paint(inherited.fill, current, paint.fill);
        paint.has_stroke = Paint(inherited.stroke, current, paint.stroke);
        paint.stroke_width = inherited.stroke_width * scale;
        paint.linecap = inherited.linecap;
        paint.linejoin = inherited.linejoin;
        paint.fill_rule = inherited.fill_rule;
    }

    const MotionComposition& composition_;
    MotionMeasurer& measurer_;
    MotionError& error_;
    MotionEvaluationContext context_;
    std::vector<std::string> stack_;
    // REM3 -- the sequences a running transition shows, and how.
    std::map<const MotionNode*, std::pair<std::string, MotionTransitionState>>
        roles_;
    // REM4 -- the perspective the node being extracted sits under.
    double parent_perspective_ = 0.0;
    double parent_origin_x_ = 0.0;
    double parent_origin_y_ = 0.0;
    YGConfigRef config_ = nullptr;
    YGNodeRef canvas_ = nullptr;
    // MOTION-AFTER-2026-09 -- the composition's frame being evaluated, for
    // a timecode of the whole composition.
    Frames root_time_;
    std::map<std::string, Value> props_;
    // MB11 -- the animations with bound keys, resolved for this frame.
    std::map<const MotionAnimation*, MotionAnimation> bound_;
    std::map<std::string, const MotionPropDefinition*> definitions_;
    std::vector<Scope> scopes_;
    std::deque<MeasureContext> contexts_;
    bool measure_failed_ = false;
};

std::string Number(double value) {
    double rounded = std::round(value * 1000.0) / 1000.0;
    if (rounded == 0.0) {
        rounded = 0.0;  // no "-0"
    }
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), rounded,
                                      std::chars_format::general, 12);
    return std::string(buffer, result.ptr);
}

std::string Color(const MotionColor& color) {
    static const char kHex[] = "0123456789abcdef";
    std::string text = "#";
    for (double channel : {color.red, color.green, color.blue, color.alpha}) {
        const int byte = static_cast<int>(
            std::lround(std::clamp(channel, 0.0, 1.0) * 255.0));
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 15]);
    }
    return text;
}

std::string Quote(const std::string& text) {
    return "\"" + mcp_json::EscapeJsonString(text) + "\"";
}

std::string Box(double x, double y, double width, double height) {
    return "[" + Number(x) + "," + Number(y) + "," + Number(width) + "," +
           Number(height) + "]";
}

std::string Transform(double translateX, double translateY, double scaleX,
                      double scaleY, double rotate, double rotateX = 0.0,
                      double rotateY = 0.0) {
    if (translateX == 0.0 && translateY == 0.0 && scaleX == 1.0 &&
        scaleY == 1.0 && rotate == 0.0 && rotateX == 0.0 && rotateY == 0.0) {
        return "";
    }
    // REM4 -- the turns in depth only when there are some, so that a flat
    // node's descriptor keeps the bytes it had.
    const std::string depth = rotateX == 0.0 && rotateY == 0.0
                                  ? std::string()
                                  : ",\"rotateX\":" + Number(rotateX) +
                                        ",\"rotateY\":" + Number(rotateY);
    return ",\"transform\":{\"translateX\":" + Number(translateX) +
           ",\"translateY\":" + Number(translateY) +
           ",\"scaleX\":" + Number(scaleX) + ",\"scaleY\":" + Number(scaleY) +
           ",\"rotate\":" + Number(rotate) + depth + "}";
}

const char* UnitName(MotionTextUnit unit) {
    switch (unit) {
        case MotionTextUnit::Line:
            return "line";
        case MotionTextUnit::Word:
            return "word";
        case MotionTextUnit::Char:
            return "char";
        case MotionTextUnit::None:
            break;
    }
    return "none";
}

void DumpText(const MotionEvaluatedNode& node, std::string& out) {
    const MotionTextStyle& style = node.text_style;
    out += ",\"text\":{\"content\":" + Quote(node.text) +
           ",\"fontFamily\":" + Quote(style.font_family) +
           ",\"fontWeight\":" + Number(style.font_weight) +
           ",\"fontSize\":" + Number(style.font_size) +
           ",\"lineHeight\":" + Number(style.line_height) +
           ",\"letterSpacing\":" + Number(style.letter_spacing);
    if (style.word_spacing != 0.0) {
        out += ",\"wordSpacing\":" + Number(style.word_spacing);
    }
    if (style.italic) {
        out += ",\"fontStyle\":\"italic\"";
    }
    // MM2 -- written only when a variant is set, so that every frame
    // dumped before keeps its bytes.
    if (!style.numeric_figures.empty() || !style.numeric_spacing.empty()) {
        std::string variant;
        for (const std::string* part :
             {&style.numeric_figures, &style.numeric_spacing}) {
            if (!part->empty()) {
                variant += (variant.empty() ? "" : " ") + *part + "-nums";
            }
        }
        out += ",\"fontVariantNumeric\":" + Quote(variant);
    }
    out += ",\"textTransform\":" + Quote(style.text_transform) +
           ",\"textAlign\":" + Quote(style.text_align) +
           ",\"color\":" + Quote(Color(style.color));
    if (style.stroke_width > 0.0) {
        out += ",\"textStroke\":{\"width\":" + Number(style.stroke_width) +
               ",\"color\":" + Quote(Color(style.stroke_color)) + "}";
    }
    out += ",\"lines\":[";
    for (size_t index = 0; index < node.text_layout.lines.size(); ++index) {
        const MotionTextLine& line = node.text_layout.lines[index];
        out += (index > 0 ? "," : "");
        out += "{\"text\":" + Quote(line.text) +
               ",\"box\":" + Box(line.x, line.y, line.width, line.height) +
               ",\"baseline\":" + Number(line.baseline) + "}";
    }
    out += "],\"words\":[";
    for (size_t index = 0; index < node.words.size(); ++index) {
        const MotionTextSpan& word = node.words[index];
        out += (index > 0 ? "," : "");
        out += "{\"text\":" + Quote(word.text) +
               ",\"line\":" + std::to_string(word.line) +
               ",\"box\":" + Box(word.x, word.y, word.width, word.height) + "}";
    }
    out += "],\"chars\":[";
    for (size_t index = 0; index < node.chars.size(); ++index) {
        const MotionTextSpan& character = node.chars[index];
        out += (index > 0 ? "," : "");
        out +=
            "{\"text\":" + Quote(character.text) +
            ",\"line\":" + std::to_string(character.line) +
            ",\"word\":" + std::to_string(character.word) + ",\"box\":" +
            Box(character.x, character.y, character.width, character.height) +
            "}";
    }
    out += "]";
    // MB3 -- written only when a highlight is there, so that every frame
    // dumped before keeps its bytes.
    if (!node.highlights.empty()) {
        out += ",\"highlights\":[";
        for (size_t index = 0; index < node.highlights.size(); ++index) {
            const MotionHighlightBand& band = node.highlights[index];
            out += (index > 0 ? "," : "");
            out +=
                "{\"highlight\":" + std::to_string(band.highlight) +
                ",\"line\":" + std::to_string(band.line) + ",\"box\":" +
                Box(band.box.x, band.box.y, band.box.width, band.box.height) +
                ",\"painted\":" + Number(band.painted) +
                ",\"color\":" + Quote(Color(band.color)) + "}";
        }
        out += "]";
    }
    if (node.unit_kind != MotionTextUnit::None) {
        out += ",\"units\":{\"unit\":" + Quote(UnitName(node.unit_kind)) +
               ",\"items\":[";
        for (size_t index = 0; index < node.units.size(); ++index) {
            const MotionTextUnitState& unit = node.units[index];
            out += (index > 0 ? "," : "");
            out += "{\"index\":" + std::to_string(unit.index);
            if (unit.delay != 0) {
                out += ",\"delay\":" + std::to_string(unit.delay);
            }
            if (unit.opacity != 1.0) {
                out += ",\"opacity\":" + Number(unit.opacity);
            }
            out += Transform(unit.translate_x, unit.translate_y, unit.scale_x,
                             unit.scale_y, unit.rotate);
            out += ",\"color\":" + Quote(Color(unit.color));
            if (unit.has_background) {
                out += ",\"backgroundColor\":" + Quote(Color(unit.background));
            }
            out += "}";
        }
        out += "]}";
    }
    out += "}";
}

// REM6 -- REMOTION-PARITY-2026-09. Under its own key: a node's first key,
// "path", is its address in the tree (M1), and the geometry written under
// the same name (M1b) made every frame holding a vector path a JSON object
// with a duplicate key. A strict reader refused it (`--call-tool
// render_motion_frame` answered IoError on the CUTMACHINE teaser); a lax one
// kept the last and lost the address.
void DumpPath(const MotionPathPaint& paint, std::string& out) {
    out +=
        ",\"pathPaint\":{\"length\":" + Number(paint.length) + ",\"bounds\":";
    out += paint.has_bounds ? Box(paint.bounds.x, paint.bounds.y,
                                  paint.bounds.width, paint.bounds.height)
                            : "null";
    out += ",\"trimStart\":" + Number(paint.trim_start) +
           ",\"trimEnd\":" + Number(paint.trim_end) +
           ",\"fill\":" + (paint.has_fill ? Quote(Color(paint.fill)) : "null") +
           ",\"stroke\":" +
           (paint.has_stroke ? Quote(Color(paint.stroke)) : "null") +
           ",\"strokeWidth\":" + Number(paint.stroke_width) +
           ",\"strokeLinecap\":" + Quote(paint.linecap) +
           ",\"strokeLinejoin\":" + Quote(paint.linejoin) +
           ",\"fillRule\":" + Quote(paint.fill_rule) + "}";
}

// REM3 -- a polygon keeps M1b's spelling, a list of vertices; the other
// shapes name themselves.
std::string DumpClip(const MotionResolvedClip& clip) {
    switch (clip.kind) {
        case MotionClipKind::Polygon: {
            std::string out = "[";
            for (size_t index = 0; index < clip.points.size(); ++index) {
                out += (index > 0 ? "," : "");
                out += "[" + Number(clip.points[index].first) + "," +
                       Number(clip.points[index].second) + "]";
            }
            return out + "]";
        }
        case MotionClipKind::Circle:
            return "{\"circle\":[" + Number(clip.cx) + "," + Number(clip.cy) +
                   "," + Number(clip.rx) + "]}";
        case MotionClipKind::Ellipse:
            return "{\"ellipse\":[" + Number(clip.cx) + "," + Number(clip.cy) +
                   "," + Number(clip.rx) + "," + Number(clip.ry) + "]}";
        case MotionClipKind::Inset:
            return "{\"inset\":" +
                   Box(clip.rect.x, clip.rect.y, clip.rect.width,
                       clip.rect.height) +
                   ",\"round\":" + Number(clip.round) + "}";
        case MotionClipKind::None:
            break;
    }
    return "null";
}

std::string DumpGradient(const MotionResolvedGradient& gradient) {
    const bool linear = gradient.kind == MotionGradientKind::Linear;
    std::string out =
        std::string("{\"kind\":") +
        (linear ? "\"linear\",\"line\":[" : "\"radial\",\"center\":[") +
        Number(gradient.x0) + "," + Number(gradient.y0) +
        (linear ? "," : "],\"radii\":[") + Number(gradient.x1) + "," +
        Number(gradient.y1) + "],\"stops\":[";
    for (size_t index = 0; index < gradient.stops.size(); ++index) {
        out += (index > 0 ? "," : "");
        out += "[" + Number(gradient.stops[index].first) + "," +
               Quote(Color(gradient.stops[index].second)) + "]";
    }
    return out + "]}";
}

std::string DumpFilters(const std::vector<MotionFilter>& filters) {
    std::string out = "[";
    for (size_t index = 0; index < filters.size(); ++index) {
        const MotionFilter& filter = filters[index];
        out += (index > 0 ? "," : "");
        switch (filter.kind) {
            case MotionFilterKind::Blur:
                out += "{\"blur\":" + Number(filter.sigma) + "}";
                break;
            case MotionFilterKind::DropShadow:
                out += "{\"dropShadow\":[" + Number(filter.offset_x) + "," +
                       Number(filter.offset_y) + "," + Number(filter.sigma) +
                       "],\"color\":" + Quote(Color(filter.color)) + "}";
                break;
            case MotionFilterKind::Glow:
                out += "{\"glow\":" + Number(filter.sigma) +
                       ",\"color\":" + Quote(Color(filter.color)) +
                       ",\"strength\":" + Number(filter.strength) + "}";
                break;
            case MotionFilterKind::Displace:
                out += "{\"displace\":[" + Number(filter.amplitude) + "," +
                       Number(filter.wavelength) +
                       "],\"seed\":" + std::to_string(filter.seed) +
                       ",\"phase\":" + Number(filter.phase) + "}";
                break;
            case MotionFilterKind::Goo:
                out += "{\"goo\":" + Number(filter.sigma) + "}";
                break;
            case MotionFilterKind::Slice:
                out += "{\"slice\":[" + Number(filter.amplitude) + "," +
                       Number(filter.wavelength) +
                       "],\"seed\":" + std::to_string(filter.seed) +
                       ",\"phase\":" + Number(filter.phase) + "}";
                break;
            case MotionFilterKind::Grain:
                out += "{\"grain\":[" + Number(filter.amplitude) + "," +
                       Number(filter.wavelength) +
                       "],\"seed\":" + std::to_string(filter.seed) +
                       ",\"phase\":" + Number(filter.phase) + "}";
                break;
            case MotionFilterKind::Mesh: {
                // The vertices where they are at this instant, in the
                // node's own pixels, row by row.
                out += "{\"mesh\":[" + std::to_string(filter.mesh_rows) + "," +
                       std::to_string(filter.mesh_columns) +
                       "],\"quality\":" + std::to_string(filter.mesh_quality) +
                       ",\"points\":[";
                for (size_t vertex = 0; vertex < filter.mesh_resolved.size();
                     ++vertex) {
                    out += (vertex > 0 ? "," : "");
                    out += "[" + Number(filter.mesh_resolved[vertex].first) +
                           "," + Number(filter.mesh_resolved[vertex].second) +
                           "]";
                }
                out += "]}";
                break;
            }
        }
    }
    return out + "]";
}

const char* PresentationName(MotionPresentation presentation) {
    switch (presentation) {
        case MotionPresentation::Fade:
            return "fade";
        case MotionPresentation::Slide:
            return "slide";
        case MotionPresentation::Wipe:
            return "wipe";
        case MotionPresentation::Iris:
            return "iris";
    }
    return "fade";
}

std::string Fraction(int64_t numerator, int64_t denominator) {
    return denominator == 1 ? std::to_string(numerator)
                            : "\"" + std::to_string(numerator) + "/" +
                                  std::to_string(denominator) + "\"";
}

// REM2 -- a media's time is written as the exact fraction of seconds it is,
// beside the picture it names: a reader compares, it never rounds.
void DumpMedia(const MotionEvaluatedNode& node, std::string& out) {
    const MotionMediaState& media = node.media;
    const bool video = node.type == MotionNodeType::Video;
    out += std::string(",\"") + (video ? "video" : "audio") + "\":{";
    out += media.media.empty() ? "\"src\":" + Quote(media.src)
                               : "\"media\":" + Quote(media.media);
    out += ",\"state\":" + Quote(media.state) + ",\"time\":" +
           Fraction(media.time_numerator, media.time_denominator);
    if (video) {
        out += ",\"frame\":" + std::to_string(media.frame) +
               ",\"objectFit\":" + Quote(node.object_fit);
    }
    if (media.muted) {
        out += ",\"muted\":true";
    }
    out += ",\"volume\":" + Number(media.volume) + "}";
}

void DumpNode(const MotionEvaluatedNode& node, std::string& out);

// REM2 -- an instance names its composition, the frame it shows and how
// that canvas lands in the box; the tree below is in the canvas' pixels.
void DumpInstance(const MotionEvaluatedNode& node, std::string& out) {
    const MotionInstanceState& instance = node.instance;
    out += ",\"composition\":{\"id\":" + Quote(instance.composition_id) +
           ",\"state\":" + Quote(instance.state) + ",\"frame\":" +
           Fraction(instance.local_numerator, instance.local_denominator) +
           ",\"canvas\":[" + std::to_string(instance.width) + "," +
           std::to_string(instance.height) + "],\"scale\":[" +
           Number(instance.scale_x) + "," + Number(instance.scale_y) +
           "],\"origin\":[" + Number(instance.origin_x) + "," +
           Number(instance.origin_y) + "]";
    if (!node.instance_root.empty()) {
        out += ",\"root\":";
        DumpNode(node.instance_root.front(), out);
    }
    out += "}";
}

void DumpNode(const MotionEvaluatedNode& node, std::string& out) {
    out += "{\"path\":" + Quote(node.path);
    if (!node.id.empty()) {
        out += ",\"id\":" + Quote(node.id);
    }
    if (!node.uid.empty()) {
        out += ",\"uid\":" + Quote(node.uid);
    }
    out += ",\"type\":" + Quote(MotionNodeTypeName(node.type));
    out += ",\"box\":" +
           Box(node.box.x, node.box.y, node.box.width, node.box.height);
    if (node.has_local_frame) {
        out += ",\"localFrame\":" +
               Number(static_cast<double>(node.local_numerator) /
                      static_cast<double>(node.local_denominator));
    }
    if (node.opacity != 1.0) {
        out += ",\"opacity\":" + Number(node.opacity);
    }
    out += Transform(node.translate_x, node.translate_y, node.scale_x,
                     node.scale_y, node.rotate, node.rotate_x, node.rotate_y);
    if (node.perspective > 0.0) {
        out += ",\"perspective\":" + Number(node.perspective);
    }
    if (node.has_3d) {
        out += ",\"transform3d\":{\"matrix\":[";
        for (int index = 0; index < 16; ++index) {
            // Six significant digits: the perspective term is a small
            // number that thousandths would round away.
            char buffer[64];
            const double value = std::abs(node.matrix3d[index]) < 5e-13
                                     ? 0.0
                                     : node.matrix3d[index];
            const auto written =
                std::to_chars(buffer, buffer + sizeof(buffer), value,
                              std::chars_format::general, 6);
            out += (index > 0 ? "," : "");
            out.append(buffer, written.ptr);
        }
        out += "]";
        if (node.has_quad) {
            out += ",\"quad\":[";
            for (int corner = 0; corner < 4; ++corner) {
                out += (corner > 0 ? "," : "");
                out += "[" + Number(node.quad[corner].first) + "," +
                       Number(node.quad[corner].second) + "]";
            }
            out += "]";
        }
        out += "}";
    }
    if (node.has_background) {
        out += ",\"backgroundColor\":" + Quote(Color(node.background));
    }
    if (node.border_radius != 0.0) {
        out += ",\"borderRadius\":" + Number(node.border_radius);
    }
    if (node.has_clip) {
        out += ",\"clipPath\":" + DumpClip(node.clip_shape);
    }
    if (node.has_gradient) {
        out += ",\"backgroundImage\":" + DumpGradient(node.gradient);
    }
    if (node.has_mask) {
        out += ",\"maskImage\":" + DumpGradient(node.mask);
    }
    if (!node.filters.empty()) {
        out += ",\"filter\":{\"stack\":" + DumpFilters(node.filters);
        if (node.has_filter_domain) {
            out += ",\"domain\":" +
                   Box(node.filter_domain.x, node.filter_domain.y,
                       node.filter_domain.width, node.filter_domain.height);
        }
        out += "}";
    }
    if (!node.transition_role.empty()) {
        out += ",\"transitionRole\":" + Quote(node.transition_role) +
               ",\"transitionProgress\":" + Number(node.transition_progress);
        if (node.blend_plus_lighter) {
            out += ",\"blend\":\"plus-lighter\"";
        }
        if (node.transition_clip.kind != MotionClipKind::None) {
            out += ",\"transitionClip\":" + DumpClip(node.transition_clip);
        }
    }
    if (node.transition.active) {
        out += ",\"transition\":{\"uid\":" + Quote(node.transition.uid) +
               ",\"presentation\":" +
               Quote(PresentationName(node.transition.presentation)) +
               ",\"window\":[" + std::to_string(node.transition.from) + "," +
               std::to_string(node.transition.to) +
               "],\"progress\":" + Number(node.transition.progress) + "}";
    }
    if (node.type == MotionNodeType::Text) {
        DumpText(node, out);
    }
    if (node.type == MotionNodeType::Image) {
        out += ",\"image\":{\"src\":" + Quote(node.image_source) +
               ",\"objectFit\":" + Quote(node.object_fit) + "}";
    }
    if (node.type == MotionNodeType::Path) {
        DumpPath(node.path_paint, out);
    }
    if (!node.blend_mode.empty()) {
        out += ",\"mixBlendMode\":\"" + node.blend_mode + "\"";
    }
    if (node.isolate) {
        out += ",\"isolation\":\"isolate\"";
    }
    if (node.overflow_hidden) {
        out += ",\"overflow\":\"hidden\"";
    }
    if (node.type == MotionNodeType::Video ||
        node.type == MotionNodeType::Audio) {
        DumpMedia(node, out);
    }
    if (node.type == MotionNodeType::Composition) {
        DumpInstance(node, out);
    }
    if (!node.children.empty()) {
        out += ",\"children\":[";
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (index > 0) {
                out += ",";
            }
            DumpNode(node.children[index], out);
        }
        out += "]";
    }
    out += "}";
}

}  // namespace

bool MotionMeasurer::MediaInfo(const std::string& src, const std::string& media,
                               MotionMediaInfo& /*info*/, MotionError& error) {
    error = {"MediaMissing", "",
             "no media '" + (media.empty() ? src : media) +
                 "': this measurer reads no video or sound"};
    return false;
}

bool MotionMeasurer::MediaSound(const std::string& src,
                                const std::string& media,
                                std::shared_ptr<const MotionPcm>& /*pcm*/,
                                MotionError& error) {
    error = {"MediaMissing", "",
             "no media '" + (media.empty() ? src : media) +
                 "': this measurer reads no sound"};
    return false;
}

bool MotionMeasurer::NumericVariantChanges(const std::string& text,
                                           const MotionTextStyle& style) {
    return (!style.numeric_figures.empty() || !style.numeric_spacing.empty()) &&
           std::any_of(text.begin(), text.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

bool MotionMeasurer::FontPostScriptName(const MotionTextStyle& style,
                                        std::string& /*name*/,
                                        MotionError& error) {
    error = {"FontMissing", "",
             "font family '" + style.font_family +
                 "': this measurer resolves no font file"};
    return false;
}

bool MotionMeasurer::FontVariableWeight(const MotionTextStyle& /*style*/,
                                        int& /*weight*/, bool& /*named*/) {
    return false;
}

bool EvaluateMotionFrame(const MotionComposition& composition,
                         const Value& props, int64_t frame,
                         MotionMeasurer& measurer, MotionFrame& output,
                         MotionError& error) {
    return EvaluateMotionFrame(composition, props, frame, measurer,
                               MotionEvaluationContext{}, output, error);
}

bool EvaluateMotionFrame(const MotionComposition& composition,
                         const Value& props, int64_t frame,
                         MotionMeasurer& measurer,
                         const MotionEvaluationContext& context,
                         MotionFrame& output, MotionError& error) {
    std::vector<std::string> stack;
    if (!context.composition_id.empty()) {
        stack.push_back(context.composition_id);
    }
    Evaluator evaluator(composition, measurer, error, context, stack);
    MotionFrame evaluated;
    if (!evaluator.Run(props, frame, evaluated)) {
        return false;
    }
    // MOTION-DESIGN-2026-09 -- the shutter's other instants: sample k of n
    // is at frame + angle * k / (360 n), angle kept to a thousandth of a
    // degree, so every instant is an exact fraction of a frame, and all of
    // them lie before the next frame.
    const int32_t samples = composition.motion_blur_samples;
    if (samples >= 2) {
        const auto angle = static_cast<int64_t>(
            std::llround(composition.motion_blur_angle * 1000.0));
        const int64_t denominator = int64_t{360000} * samples;
        for (int32_t k = 1; k < samples; ++k) {
            Frames time;
            if (!MakeFrames(static_cast<__int128>(frame) * denominator +
                                static_cast<__int128>(angle) * k,
                            denominator, time)) {
                return Fail(error, "InvalidValue", "motionBlur",
                            "a shutter instant does not fit a frame count");
            }
            Evaluator instant(composition, measurer, error, context, stack);
            MotionFrame sample;
            if (!instant.RunAt(props, time, frame, sample)) {
                return false;
            }
            evaluated.motion_blur.push_back(std::move(sample.root));
        }
    }
    evaluated.grain_amount = composition.grain_amount;
    evaluated.grain_size = composition.grain_size;
    evaluated.grain_seed = composition.grain_seed;
    output = std::move(evaluated);
    return true;
}

void ScaleMotionFrame(MotionFrame& frame, double scale) {
    if (!(scale > 0.0) || scale == 1.0) {
        return;
    }
    const double centerX = frame.width / 2.0;
    const double centerY = frame.height / 2.0;
    // A root drawn translate t, then rotate and scale about its centre c:
    // scaling the whole by s about the origin is its scale times s and its
    // translation s t - (1 - s) c.
    const auto scaleRoot = [&](MotionEvaluatedNode& root) {
        root.scale_x *= scale;
        root.scale_y *= scale;
        root.translate_x = scale * root.translate_x - (1.0 - scale) * centerX;
        root.translate_y = scale * root.translate_y - (1.0 - scale) * centerY;
    };
    scaleRoot(frame.root);
    for (MotionEvaluatedNode& instant : frame.motion_blur) {
        scaleRoot(instant);
    }
    frame.width = std::max<int32_t>(
        1, static_cast<int32_t>(std::lround(frame.width * scale)));
    frame.height = std::max<int32_t>(
        1, static_cast<int32_t>(std::lround(frame.height * scale)));
    frame.grain_size = std::max(1.0, frame.grain_size * scale);
    ResolveMotionEffectDomains(frame);
    for (MotionEvaluatedNode& instant : frame.motion_blur) {
        MotionFrame alone;
        alone.width = frame.width;
        alone.height = frame.height;
        alone.root = std::move(instant);
        ResolveMotionEffectDomains(alone);
        instant = std::move(alone.root);
    }
}

bool BindMotionInstance(const MotionComposition& composition,
                        const Value& props, MotionMeasurer& measurer,
                        MotionComposition& out, MotionError& error) {
    Evaluator evaluator(composition, measurer, error, MotionEvaluationContext{},
                        {});
    return evaluator.BindInstance(props, out);
}

std::string DumpMotionFrame(const MotionFrame& frame) {
    std::string out = "{\"width\":" + std::to_string(frame.width) +
                      ",\"height\":" + std::to_string(frame.height) +
                      ",\"frame\":" + std::to_string(frame.frame) +
                      ",\"root\":";
    DumpNode(frame.root, out);
    out += "}";
    return out;
}

namespace {

// REM2 -- the voices of a composition, from its frame t onto each sound.
bool CollectVoices(const MotionComposition& composition, const Value& props,
                   const MotionEvaluationContext& context,
                   MotionMeasurer& measurer, std::vector<AudioVoice>& voices,
                   MotionError& error) {
    std::vector<std::string> stack;
    if (!context.composition_id.empty()) {
        stack.push_back(context.composition_id);
    }
    Evaluator evaluator(composition, measurer, error, context,
                        std::move(stack));
    Evaluator::TimeMap map;
    map.top_numerator = composition.fps_numerator;
    map.top_denominator = composition.fps_denominator;
    // The composition itself is heard from its frame 0 to its duration.
    map.has_low = true;
    map.low = Frames{0, 1};
    map.has_high = true;
    map.high = Frames{composition.duration, 1};
    return evaluator.CollectAudio(props, map, voices);
}

// A frame t of the outermost composition as a sample index: the first
// sample at or after it.
__int128 FirstSampleAt(const Frames& t, const MotionComposition& composition) {
    return CeilDivide(
        static_cast<__int128>(t.numerator) * kMotionAudioRate *
            composition.fps_denominator,
        static_cast<__int128>(t.denominator) * composition.fps_numerator);
}

struct VoicePlan {
    __int128 first = 0;
    __int128 end = 0;
    // The media sample sample n reads: n + shift.
    __int128 shift = 0;
};

// Where a voice is heard, in samples, and which of its media's samples it
// reads. Its pace is 1 (CollectVoices refused any other), so media time is
// composition time plus a constant: (startFrom + offset * rate) / fps.
bool PlanVoice(const AudioVoice& voice, const MotionComposition& composition,
               VoicePlan& plan, MotionError& error) {
    Frames position;
    Frames rate{voice.rate_numerator, voice.rate_denominator};
    Frames advanced;
    if (!Multiply(voice.offset, rate, advanced) ||
        !Add(Frames{voice.start_from, 1}, advanced, position)) {
        error = {"TimeOverflow", voice.path,
                 "the sound's time cannot be represented exactly"};
        return false;
    }
    // shift = floor(position / fps * rate): position in frames of the
    // node's cadence, as seconds, as samples.
    plan.shift = FloorDivide(
        static_cast<__int128>(position.numerator) * voice.fps_denominator *
            kMotionAudioRate,
        static_cast<__int128>(position.denominator) * voice.fps_numerator);
    plan.first = voice.has_low ? FirstSampleAt(voice.low, composition) : 0;
    plan.end = voice.has_high
                   ? FirstSampleAt(voice.high, composition)
                   : static_cast<__int128>(MotionAudioSampleCount(composition));
    return true;
}

}  // namespace

int64_t MotionAudioSampleCount(const MotionComposition& composition) {
    return static_cast<int64_t>(
        CeilDivide(static_cast<__int128>(composition.duration) *
                       kMotionAudioRate * composition.fps_denominator,
                   composition.fps_numerator));
}

bool MixMotionAudio(const MotionComposition& composition, const Value& props,
                    const MotionEvaluationContext& context,
                    MotionMeasurer& measurer, int64_t first, int64_t count,
                    MotionPcm& mix, MotionError& error) {
    mix.left.assign(static_cast<size_t>(std::max<int64_t>(0, count)), 0.0f);
    mix.right.assign(mix.left.size(), 0.0f);
    std::vector<AudioVoice> voices;
    if (!CollectVoices(composition, props, context, measurer, voices, error)) {
        return false;
    }
    for (const AudioVoice& voice : voices) {
        VoicePlan plan;
        if (!PlanVoice(voice, composition, plan, error)) {
            return false;
        }
        const __int128 from = std::max<__int128>(plan.first, first);
        const __int128 to = std::min<__int128>(plan.end, first + count);
        if (from >= to) {
            continue;
        }
        std::shared_ptr<const MotionPcm> pcm;
        if (!measurer.MediaSound(voice.src, voice.media, pcm, error)) {
            if (error.path.empty()) {
                error.path = voice.path;
            }
            return false;
        }
        const auto available = static_cast<__int128>(pcm->left.size());
        for (__int128 n = from; n < to; ++n) {
            const __int128 source = n + plan.shift;
            if (source < 0) {
                continue;
            }
            if (source >= available) {
                break;
            }
            double gain = voice.volume;
            if (voice.volume_animation != nullptr) {
                // The node's own time at this sample, in seconds of its
                // composition: ((n / rate) * fps_top * scale + offset) /
                // fps_node.
                const __int128 top =
                    static_cast<__int128>(composition.fps_numerator) * n;
                const __int128 topDen =
                    static_cast<__int128>(composition.fps_denominator) *
                    kMotionAudioRate;
                Frames t;
                Frames local;
                Frames scaled;
                Frames seconds;
                if (!MakeFrames(top, topDen, t) ||
                    !Multiply(t, voice.scale, scaled) ||
                    !Add(scaled, voice.offset, local) ||
                    !Multiply(
                        local,
                        Frames{voice.fps_denominator, voice.fps_numerator},
                        seconds) ||
                    seconds.denominator > std::numeric_limits<int32_t>::max()) {
                    error = {"TimeOverflow", voice.path,
                             "the sound's time cannot be represented exactly"};
                    return false;
                }
                const KeyframeEvaluation evaluation = EvaluateMotionTrack(
                    *voice.volume_animation,
                    RationalTime{seconds.numerator,
                                 static_cast<int32_t>(seconds.denominator)});
                if (!evaluation) {
                    error = {"InvalidAnimation", voice.path + ".animate.volume",
                             std::string("evaluation failed: ") +
                                 KeyframeErrorName(evaluation.error)};
                    return false;
                }
                gain = std::clamp(evaluation.value, 0.0, 4.0);
            }
            const auto at = static_cast<size_t>(n - first);
            const auto read = static_cast<size_t>(source);
            mix.left[at] += static_cast<float>(pcm->left[read] * gain);
            mix.right[at] += static_cast<float>(pcm->right[read] * gain);
        }
    }
    return true;
}

bool DescribeMotionAudio(const MotionComposition& composition,
                         const Value& props,
                         const MotionEvaluationContext& context,
                         MotionMeasurer& measurer, std::string& json,
                         size_t& audible, MotionError& error) {
    std::vector<AudioVoice> voices;
    if (!CollectVoices(composition, props, context, measurer, voices, error)) {
        return false;
    }
    json =
        "{\"samples\":" + std::to_string(MotionAudioSampleCount(composition)) +
        ",\"rate\":" + std::to_string(kMotionAudioRate) + ",\"voices\":[";
    audible = 0;
    for (const AudioVoice& voice : voices) {
        VoicePlan plan;
        if (!PlanVoice(voice, composition, plan, error)) {
            return false;
        }
        if (plan.first >= plan.end) {
            continue;
        }
        json += audible++ > 0 ? "," : "";
        json += "{\"path\":" + Quote(voice.path);
        if (!voice.uid.empty()) {
            json += ",\"uid\":" + Quote(voice.uid);
        }
        json += std::string(",\"type\":") +
                (voice.video ? "\"video\"" : "\"audio\"");
        json += voice.media.empty() ? ",\"src\":" + Quote(voice.src)
                                    : ",\"media\":" + Quote(voice.media);
        json +=
            ",\"first\":" + std::to_string(static_cast<int64_t>(plan.first)) +
            ",\"end\":" + std::to_string(static_cast<int64_t>(plan.end)) +
            ",\"mediaSample\":" +
            std::to_string(static_cast<int64_t>(plan.first + plan.shift));
        json += voice.volume_animation != nullptr
                    ? std::string(",\"volume\":\"animated\"")
                    : ",\"volume\":" + Number(voice.volume);
        json += "}";
    }
    json += "]}";
    return true;
}

std::string MotionTimecodeText(int64_t frame, int64_t fpsNumerator,
                               int64_t fpsDenominator) {
    const int64_t nominal =
        (fpsNumerator + fpsDenominator - 1) / fpsDenominator;
    frame = std::max<int64_t>(0, frame);
    const int64_t ff = frame % nominal;
    const int64_t seconds = frame / nominal;
    char text[32];
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(seconds / 3600),
                  static_cast<long long>(seconds / 60 % 60),
                  static_cast<long long>(seconds % 60),
                  static_cast<long long>(ff));
    return text;
}

MotionResolvedClip ResolveMotionClip(const MotionClipShape& shape,
                                     const MotionBox& box) {
    return ResolveClip(shape, box);
}

MotionColor MixMotionColors(const MotionColor& from, const MotionColor& to,
                            double fraction) {
    return MixColors(from, to, fraction);
}
