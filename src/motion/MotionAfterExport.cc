#include "MotionAfterExport.h"

#include "ClipEnabled.h"
#include "Document.h"
#include "Keyframes.h"
#include "MotionGeometry.h"
#include "MotionOutline.h"
#include "MotionPath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace {

using mcp_json::Value;

constexpr double kPi = 3.14159265358979323846;

Value Num(double value) {
    if (!std::isfinite(value)) {
        value = 0.0;
    }
    // Four decimals: sub-pixel places and eases, without float noise.
    return Value::MakeDouble(std::round(value * 10000.0) / 10000.0);
}

Value Array(std::initializer_list<double> values) {
    Value out = Value::MakeArray();
    for (const double value : values) {
        out.Push(Num(value));
    }
    return out;
}

Value Array(const std::vector<double>& values) {
    Value out = Value::MakeArray();
    for (const double value : values) {
        out.Push(Num(value));
    }
    return out;
}

Value Rgb(const MotionColor& color) {
    return Array({color.red, color.green, color.blue});
}

bool HasAnimation(const MotionNode& node, const std::string& property) {
    for (const auto& entry : node.animate) {
        if (entry.first == property) {
            return true;
        }
    }
    return false;
}

const MotionAnimation* Animation(const MotionNode& node,
                                 const std::string& property) {
    for (const auto& entry : node.animate) {
        if (entry.first == property) {
            return &entry.second;
        }
    }
    return nullptr;
}

// After's names for the CSS blend modes that have one. MA7 -- only modes
// the parser accepts (MotionComposition.cc, mixBlendMode): hue, saturation,
// color and luminosity, which it refuses, were listed here too. The mattes
// (mask-in, mask-out, atop) have no layer mode: the report says so.
std::string AfterBlend(const std::string& mode) {
    static const std::map<std::string, std::string> table = {
        {"multiply", "MULTIPLY"},     {"screen", "SCREEN"},
        {"overlay", "OVERLAY"},       {"darken", "DARKEN"},
        {"lighten", "LIGHTEN"},       {"color-dodge", "COLOR_DODGE"},
        {"color-burn", "COLOR_BURN"}, {"hard-light", "HARD_LIGHT"},
        {"soft-light", "SOFT_LIGHT"}, {"difference", "DIFFERENCE"},
        {"exclusion", "EXCLUSION"},   {"plus-lighter", "ADD"},
    };
    const auto found = table.find(mode);
    return found == table.end() ? std::string() : found->second;
}

// MOTION-AFTER-2026-09 -- an author's id `name#scope` (ids are unique in a
// composition) is shown as `name`.
// MA6 -- the names the editor's composition panel shows too
// (MotionOutline.h).
std::string Display(const std::string& id) { return MotionDisplayId(id); }

std::string Shorten(std::string text) {
    return MotionShortName(std::move(text));
}

// MA11 -- the eased progress of one segment at time fraction `x`.
double EaseAt(KeyframeInterpolation interpolation, const KeyframeEasing& easing,
              double x) {
    if (interpolation == KeyframeInterpolation::Hold) {
        return x >= 1.0 ? 1.0 : 0.0;
    }
    if (interpolation == KeyframeInterpolation::Linear) {
        return std::clamp(x, 0.0, 1.0);
    }
    return KeyframeTrack::EvaluateCubicBezier(easing, x);
}

// A straight segment said as a curve: handles on the line, a third in.
// After's linear keys and bezier keys cannot share one key, and a property
// whose dimensions come from several tracks may need both.
constexpr KeyframeEasing kStraight{1.0 / 3.0, 1.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0};

// MA11 -- the part [a, b] (fractions of the segment's time) of a CSS
// easing, as an easing of its own: the same curve cut by de Casteljau and
// set back in the unit square. Keys at a and b with that easing draw the
// engine's curve exactly, so a segment another track cuts stays exact.
KeyframeEasing SubEasing(const KeyframeEasing& easing, double a, double b) {
    if (a <= 0.0 && b >= 1.0) {
        return easing;
    }
    struct Point {
        double x;
        double y;
    };
    const auto lerp = [](Point p, Point q, double t) {
        return Point{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t};
    };
    const auto coordinate = [](double p1, double p2, double t) {
        const double u = 1.0 - t;
        return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
    };
    // The curve's parameter at a time fraction: x(t) rises (x1, x2 in
    // [0, 1]), and a fixed number of halvings reaches double resolution.
    const auto parameter = [&](double x) {
        if (x <= 0.0) {
            return 0.0;
        }
        if (x >= 1.0) {
            return 1.0;
        }
        double low = 0.0;
        double high = 1.0;
        for (int step = 0; step < 64; ++step) {
            const double middle = 0.5 * (low + high);
            if (coordinate(easing.x1, easing.x2, middle) < x) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return 0.5 * (low + high);
    };
    const double sa = parameter(a);
    const double sb = parameter(b);
    const Point p0{0.0, 0.0};
    const Point p1{easing.x1, easing.y1};
    const Point p2{easing.x2, easing.y2};
    const Point p3{1.0, 1.0};
    // The left part up to sb, then its right part from sa.
    const Point l1 = lerp(p0, p1, sb);
    const Point m = lerp(p1, p2, sb);
    const Point l2 = lerp(l1, m, sb);
    const Point l3 = lerp(l2, lerp(m, lerp(p2, p3, sb), sb), sb);
    const double u = sb > 0.0 ? sa / sb : 0.0;
    const Point a01 = lerp(p0, l1, u);
    const Point a12 = lerp(l1, l2, u);
    const Point a23 = lerp(l2, l3, u);
    const Point b012 = lerp(a01, a12, u);
    const Point b123 = lerp(a12, a23, u);
    const Point r0 = lerp(b012, b123, u);
    const Point r1 = b123;
    const Point r2 = a23;
    const Point r3 = l3;
    const double width = r3.x - r0.x;
    const double height = r3.y - r0.y;
    if (!(width > 0.0) || std::abs(height) < 1e-12) {
        return kStraight;
    }
    return KeyframeEasing{(r1.x - r0.x) / width, (r1.y - r0.y) / height,
                          (r2.x - r0.x) / width, (r2.y - r0.y) / height};
}

// MA11 -- how After reads a key's speed (measured on After 26, 9 October
// 2026): per dimension for a number or a non-spatial pair (scale, a
// rectangle's size); once for a mask's path, in progress through the
// segment whose average is 1; once for a colour, in distance between the
// two colours, red, green and blue counted 0 to 255, per second.
enum class EaseModel : uint8_t { PerDimension, Progress, Distance };

struct FrameIndex {
    MotionFrame frame;
    std::unordered_map<std::string, const MotionEvaluatedNode*> by_path;
};

void IndexNodes(const MotionEvaluatedNode& node, FrameIndex& index) {
    index.by_path[node.path] = &node;
    for (const MotionEvaluatedNode& child : node.children) {
        IndexNodes(child, index);
    }
}

// Where a node's transforms turn: the centre of its layout box, as CSS.
struct Pivot {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double CenterX() const { return x + width / 2.0; }
    double CenterY() const { return y + height / 2.0; }
};

// MOTION-WEB-2026-10 MW2 -- ROADMAP.md. Where a key sits, in frames of the
// composition: an exact fraction, reduced, its denominator positive. A key
// the format writes between two frames (MW1) keeps its instant in After,
// which keeps a key set by script between frames (setValuesAtTimes, checked
// in After 26 on 14 November 2026): the description spells it "n/d", as the
// format does, and an integer stays an integer, so a composition on whole
// frames is described byte for byte as before.
struct KeyAt {
    int64_t num = 0;
    int64_t den = 1;

    static KeyAt Whole(int64_t frame) { return KeyAt{frame, 1}; }

    bool IsWhole() const { return den == 1; }

    KeyAt Plus(int64_t frames) const {
        return KeyAt{Checked(static_cast<__int128>(frames) * den + num), den};
    }

    // The whole frames around it: the one at or before, the one at or after.
    int64_t Floor() const {
        int64_t quotient = num / den;
        if (num % den != 0 && num < 0) {
            --quotient;
        }
        return quotient;
    }
    int64_t Ceil() const { return IsWhole() ? num : Floor() + 1; }

    // b - a, in frames.
    static double Between(const KeyAt& a, const KeyAt& b) {
        const __int128 numerator = static_cast<__int128>(b.num) * a.den -
                                   static_cast<__int128>(a.num) * b.den;
        const __int128 denominator = static_cast<__int128>(a.den) * b.den;
        return static_cast<double>(static_cast<long double>(numerator) /
                                   static_cast<long double>(denominator));
    }

    Value Spelled() const {
        if (IsWhole()) {
            return Value::MakeInt(num);
        }
        return Value::MakeString(std::to_string(num) + "/" +
                                 std::to_string(den));
    }

    // RationalTime's own rule: a value that leaves int64 is refused by an
    // exception, never wrapped.
    static int64_t Checked(__int128 value) {
        if (value > std::numeric_limits<int64_t>::max() ||
            value < std::numeric_limits<int64_t>::min()) {
            throw std::overflow_error("key frame overflows int64_t");
        }
        return static_cast<int64_t>(value);
    }
};

// One After composition being written.
struct Comp {
    std::string id;
    std::string name;
    double origin_x = 0.0;  // composition pixels of its (0, 0)
    double origin_y = 0.0;
    int32_t width = 0;
    int32_t height = 0;
    int64_t duration = 0;  // frames
    int64_t t0 = 0;        // absolute frame of its local 0
    Value layers = Value::MakeArray();
};

class Exporter {
public:
    Exporter(const MotionComposition& composition, const Value& props,
             MotionMeasurer& measurer, const MotionEvaluationContext& context,
             const MotionAfterExportOptions& options,
             MotionAfterExportReport& report, MotionError& error)
        : composition_(composition),
          props_(props),
          measurer_(measurer),
          context_(context),
          options_(options),
          report_(report),
          error_(error) {}

    bool Run(Value& description, int32_t blur_samples, double blur_angle) {
        const MotionEvaluatedNode* root = At(0, "root");
        if (root == nullptr) {
            return false;
        }
        CollectLayout(composition_.root, 0);
        blur_ = blur_samples >= 2;
        if (blur_) {
            // MA12 -- After blurs what moves a layer (position, scale,
            // rotation); the engine averages every instant of its shutter.
            const std::function<bool(const MotionNode&)> inPlace =
                [&](const MotionNode& node) {
                    for (const auto& entry : node.animate) {
                        if (entry.first == "opacity" ||
                            entry.first == "color" ||
                            entry.first == "backgroundColor" ||
                            entry.first == "clipPath" ||
                            entry.first == "fill" || entry.first == "stroke") {
                            return true;
                        }
                    }
                    return std::any_of(node.children.begin(),
                                       node.children.end(), inPlace);
                };
            if (inPlace(composition_.root)) {
                report_.notes.push_back(
                    "composition: flou de mouvement : After flou ce qui "
                    "bouge ; un fondu, une couleur ou une découpe qui "
                    "changent sur place y gardent la valeur de l'image, que "
                    "le moteur moyenne sur l'obturateur (écart aux premières "
                    "images d'un changement rapide)");
            }
        }
        Comp main;
        main.id = NextComp();
        main.name = options_.name;
        main.width = composition_.width;
        main.height = composition_.height;
        main.duration = composition_.duration;
        EmitAudio(main);
        if (!EmitChildren(composition_.root, "root", *root, main, "",
                          main.origin_x, main.origin_y, 0)) {
            return false;
        }
        // MA7 -- a frame read for the layout (SampleAt) that did not
        // evaluate counted as a frame where the node is not shown, and the
        // export went on, its error kept but its result true: refused.
        if (!error_.code.empty()) {
            return false;
        }
        for (const std::string& id : options_.boil) {
            if (boiled_.count(id) == 0) {
                error_ = {"ValidationFailed", "boil",
                          "nothing to boil in '" + id +
                              "': boil names a node whose translateX or "
                              "translateY is held at a constant step"};
                return false;
            }
        }
        if (!approximated_curves_.empty()) {
            std::string list;
            for (const auto& [name, gap] : approximated_curves_) {
                char percent[32];
                std::snprintf(percent, sizeof percent, "%.2f", gap * 100.0);
                std::string shown = percent;
                std::replace(shown.begin(), shown.end(), '.', ',');
                if (!list.empty()) {
                    list += ", ";
                }
                list += name;
                list += " (";
                list += shown;
                list += " %)";
            }
            report_.notes.push_back(
                "composition: courbes GSAP approchées par une Bézier, écart "
                "maximal en part du mouvement : " +
                list);
        }
        const std::string mainId = main.id;
        Finish(main);
        description = Value::MakeObject();
        description.Set("schema", Value::MakeString(kMotionAfterSchema));
        description.Set("name", Value::MakeString(options_.name));
        description.Set("fps_numerator",
                        Value::MakeInt(composition_.fps_numerator));
        description.Set("fps_denominator",
                        Value::MakeInt(composition_.fps_denominator));
        description.Set("main", Value::MakeString(mainId));
        description.Set("aep_path", Value::MakeString(options_.aep_path));
        if (blur_samples >= 2) {
            // MA12 -- the engine's shutter: `samples` instants over `angle`
            // degrees, opening on the frame (After's phase 0; its -90 by
            // default shows a fast move's start a frame late).
            Value blur = Value::MakeObject();
            blur.Set("samples", Value::MakeInt(blur_samples));
            blur.Set("shutter_angle", Num(blur_angle));
            blur.Set("shutter_phase", Num(0.0));
            description.Set("motion_blur", blur);
        }
        description.Set("footage", footage_);
        Value comps = Value::MakeArray();
        for (Value& comp : comps_) {
            comps.Push(std::move(comp));
        }
        description.Set("comps", comps);
        Value notes = Value::MakeArray();
        for (const std::string& note : report_.notes) {
            notes.Push(Value::MakeString(note));
        }
        description.Set("notes", notes);
        return true;
    }

private:
    // --- evaluation --------------------------------------------------------
    const MotionEvaluatedNode* At(int64_t frame, const std::string& path) {
        frame = std::clamp<int64_t>(
            frame, 0, std::max<int64_t>(0, composition_.duration - 1));
        auto& slot = frames_[frame];
        if (!slot) {
            slot = std::make_unique<FrameIndex>();
            if (!EvaluateMotionFrame(composition_, props_, frame, measurer_,
                                     context_, slot->frame, error_)) {
                return nullptr;
            }
            IndexNodes(slot->frame.root, *slot);
        }
        const auto found = slot->by_path.find(path);
        return found == slot->by_path.end() ? nullptr : found->second;
    }

    // MOTION-WEB-2026-10 MW1, MW2 -- a key's instant in frames, exactly: a
    // key between two frames (MW1) goes to After where it is, no longer on
    // the frame before it (MW2).
    KeyAt FrameOf(const RationalTime& time) const {
        const __int128 numerator =
            static_cast<__int128>(time.value) * composition_.fps_numerator;
        const __int128 denominator =
            static_cast<__int128>(time.rate) * composition_.fps_denominator;
        __int128 a = numerator < 0 ? -numerator : numerator;
        __int128 b = denominator;
        while (b != 0) {
            const __int128 r = a % b;
            a = b;
            b = r;
        }
        const __int128 divisor = a == 0 ? 1 : a;
        return KeyAt{KeyAt::Checked(numerator / divisor),
                     KeyAt::Checked(denominator / divisor)};
    }

    // The length of a stretch between two keys, in seconds.
    double Seconds(const KeyAt& from, const KeyAt& to) const {
        return KeyAt::Between(from, to) * composition_.fps_denominator /
               composition_.fps_numerator;
    }

    std::string NextComp() { return "C" + std::to_string(++comp_count_); }
    std::string NextLayer() { return "L" + std::to_string(++layer_count_); }

    // MA2 -- each note keeps the node it is about (MotionAfterNoteScope).
    void Note(const std::string& path, const std::string& what) {
        report_.scopes.push_back({report_.notes.size(), path, false});
        report_.notes.push_back(path + ": " + what);
    }

    // MA3 -- a flow's terms name the layers of the same composition; a flow
    // one of whose texts has no layer here keeps its fixed place (exact).
    static void ResolveFlows(Value& layers) {
        if (std::none_of(layers.AsArray().begin(), layers.AsArray().end(),
                         [](const Value& layer) {
                             return layer.Find("flow") != nullptr;
                         })) {
            return;
        }
        std::map<std::string, std::string> texts;
        for (const Value& layer : layers.AsArray()) {
            if (layer.Find("kind")->AsString() == "text") {
                texts[layer.Find("path")->AsString()] =
                    layer.Find("id")->AsString();
            }
        }
        Value resolved = Value::MakeArray();
        for (const Value& layer : layers.AsArray()) {
            const Value* flow = layer.Find("flow");
            if (flow == nullptr) {
                resolved.Push(layer);
                continue;
            }
            Value terms = Value::MakeArray();
            bool whole = true;
            for (const Value& term : flow->Find("terms")->AsArray()) {
                const auto found = texts.find(term.Find("path")->AsString());
                if (found == texts.end()) {
                    whole = false;
                    break;
                }
                Value named = Value::MakeObject();
                named.Set("layer", Value::MakeString(found->second));
                named.Set("k", *term.Find("k"));
                terms.Push(std::move(named));
            }
            Value kept = Value::MakeObject();
            kept.Set("terms", terms);
            if (const Value* anchor = flow->Find("anchor")) {
                const auto self = texts.find(flow->Find("self")->AsString());
                whole = whole && self != texts.end();
                if (whole) {
                    kept.Set("anchor", *anchor);
                    kept.Set("self", Value::MakeString(self->second));
                }
            }
            Value copy = Value::MakeObject();
            for (const auto& [key, value] : layer.AsObject()) {
                if (key != "flow") {
                    copy.Set(key, value);
                } else if (whole) {
                    copy.Set(key, kept);
                }
            }
            resolved.Push(std::move(copy));
        }
        layers = std::move(resolved);
    }

    void Finish(Comp& comp) {
        ResolveFlows(comp.layers);
        Value out = Value::MakeObject();
        out.Set("id", Value::MakeString(comp.id));
        out.Set("name", Value::MakeString(comp.name));
        out.Set("width", Value::MakeInt(std::max(4, comp.width)));
        out.Set("height", Value::MakeInt(std::max(4, comp.height)));
        out.Set("duration",
                Value::MakeInt(std::max<int64_t>(1, comp.duration)));
        out.Set("layers", std::move(comp.layers));
        comps_.push_back(std::move(out));
        ++report_.comps;
    }

    // --- keyframes -----------------------------------------------------------
    // A track written as After keyframes: `map` turns one engine value into
    // the dimensions of the After property.
    // MOTION-WEB-2026-10 MW3 -- a GSAP curve that is not a cubic-bezier
    // reaches After as the closest one; the report names each and its gap.
    void NoteCurves(const MotionAnimation& animation) const {
        for (const MotionNamedCurve& curve : animation.named_curves) {
            if (curve.family != MotionNamedFamily::None &&
                !curve.exact_bezier) {
                approximated_curves_[curve.name] = curve.bezier_error;
            }
        }
    }

    Value Keys(const MotionAnimation& animation,
               const std::function<std::vector<double>(double)>& map) {
        NoteCurves(animation);
        Value keys = Value::MakeArray();
        const std::vector<Keyframe>& frames = animation.track.keyframes();
        for (size_t index = 0; index < frames.size(); ++index) {
            const Keyframe& key = frames[index];
            const std::vector<double> value = map(key.value);
            Value out = Value::MakeObject();
            out.Set("f", FrameOf(key.time).Plus(key_offset_).Spelled());
            out.Set("v", Array(value));
            const auto type = [](KeyframeInterpolation interpolation) {
                switch (interpolation) {
                    case KeyframeInterpolation::Hold:
                        return "hold";
                    case KeyframeInterpolation::Linear:
                        return "linear";
                    case KeyframeInterpolation::CubicBezier:
                        return "bezier";
                }
                return "linear";
            };
            out.Set("out", Value::MakeString(type(key.interpolation)));
            out.Set("in",
                    Value::MakeString(
                        index == 0 ? "linear"
                                   : type(frames[index - 1].interpolation)));
            // Eases of the segment leaving this key, and of the one
            // arriving at it, for every dimension.
            const auto eases = [&](size_t from, bool outgoing) {
                Value list = Value::MakeArray();
                const Keyframe& a = frames[from];
                const Keyframe& b = frames[from + 1];
                const std::vector<double> va = map(a.value);
                const std::vector<double> vb = map(b.value);
                const double seconds =
                    Seconds(FrameOf(a.time), FrameOf(b.time));
                for (size_t d = 0; d < va.size(); ++d) {
                    MotionAfterEase easeOut;
                    MotionAfterEase easeIn;
                    MotionAfterBezierEase(a.easing.x1, a.easing.y1, a.easing.x2,
                                          a.easing.y2, va[d], vb[d], seconds,
                                          easeOut, easeIn);
                    const MotionAfterEase& ease = outgoing ? easeOut : easeIn;
                    list.Push(Array({ease.speed, ease.influence}));
                }
                return list;
            };
            if (key.interpolation == KeyframeInterpolation::CubicBezier &&
                index + 1 < frames.size()) {
                out.Set("ease_out", eases(index, true));
            }
            if (index > 0 && frames[index - 1].interpolation ==
                                 KeyframeInterpolation::CubicBezier) {
                out.Set("ease_in", eases(index - 1, false));
            }
            keys.Push(std::move(out));
            ++report_.keyframes;
        }
        if (animation.extrapolate_left == KeyframeExtrapolation::Extend ||
            animation.extrapolate_right == KeyframeExtrapolation::Extend) {
            extend_used_ = true;
        }
        return keys;
    }

    // MA4 -- a track of a node named by `boil`, held at a constant step,
    // written as an expression on `dimension` rather than keyframes: a jump
    // every `step` frames (posterizeTime), to a place drawn in the track's
    // range. Returns false, and leaves the track to its keyframes, for a node
    // not named or a track that is not held at a constant step.
    bool Boil(const MotionNode& source, const MotionAnimation& animation,
              const std::string& property, const std::string& path,
              Value& expressions, const std::string& dimension) {
        if (source.id.empty() ||
            std::find(options_.boil.begin(), options_.boil.end(), source.id) ==
                options_.boil.end()) {
            return false;
        }
        const std::vector<Keyframe>& frames = animation.track.keyframes();
        if (frames.size() < 2) {
            return false;
        }
        // MW2 -- the jumps of a texture are whole frames; a key between two
        // frames leaves the track to its keys, which After keeps there.
        for (const Keyframe& key : frames) {
            if (!FrameOf(key.time).IsWhole()) {
                return false;
            }
        }
        const int64_t step =
            FrameOf(frames[1].time).num - FrameOf(frames[0].time).num;
        double low = frames[0].value;
        double high = frames[0].value;
        for (size_t index = 0; index < frames.size(); ++index) {
            if (index + 1 < frames.size() &&
                (frames[index].interpolation != KeyframeInterpolation::Hold ||
                 FrameOf(frames[index + 1].time).num -
                         FrameOf(frames[index].time).num !=
                     step)) {
                return false;
            }
            low = std::min(low, frames[index].value);
            high = std::max(high, frames[index].value);
        }
        if (step <= 0) {
            return false;
        }
        const double rate = static_cast<double>(composition_.fps_numerator) /
                            (static_cast<double>(composition_.fps_denominator) *
                             static_cast<double>(step));
        const std::string every =
            step == 1 ? "à chaque image"
                      : "toutes les " + std::to_string(step) + " images";
        const std::string expression =
            "// " + property + " (MA4) : un saut " + every + ", entre " +
            Plain(low) + " et " + Plain(high) + " px.\n" + "posterizeTime(" +
            Plain(rate) + ");\nseedRandom(" + std::to_string(++boil_seed_) +
            ", false);\nvalue + random(" + Plain(low) + ", " + Plain(high) +
            ");";
        expressions.Set(dimension, Value::MakeString(expression));
        boiled_[source.id] = true;
        Note(path, property + " en expression (posterizeTime(" + Plain(rate) +
                       "), random(" + Plain(low) + ", " + Plain(high) +
                       ")) : les sauts sont ceux d'After, plus ceux du "
                       "moteur (" +
                       std::to_string(frames.size()) +
                       " clés maintenues remplacées)");
        return true;
    }

    // --- MA11: keys After reads otherwise ----------------------------------
    // A segment of a track, in absolute frames: what may move a property
    // between `from` and `to`, and the curve it moves along. `curve` false:
    // a spring, which no After ease draws, or a segment whose ends fall
    // between two frames (MW2).
    struct Segment {
        int64_t from = 0;
        int64_t to = 0;
        KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
        KeyframeEasing easing;
        bool curve = true;
    };

    // One key to write: its frame in the composition's time, its value
    // (one number per dimension of the After property, or the flattened
    // vertices and tangents of a mask), and the segment leaving it -- one
    // easing per dimension, or one for the whole value.
    struct KeyOut {
        KeyAt frame;
        std::vector<double> value;
        KeyframeInterpolation out = KeyframeInterpolation::Linear;
        std::vector<KeyframeEasing> easing;
    };

    Value WriteKeys(const std::vector<KeyOut>& keys, EaseModel model,
                    const std::function<Value(const std::vector<double>&)>&
                        spell = nullptr) {
        const auto type = [](KeyframeInterpolation interpolation) {
            switch (interpolation) {
                case KeyframeInterpolation::Hold:
                    return "hold";
                case KeyframeInterpolation::Linear:
                    return "linear";
                case KeyframeInterpolation::CubicBezier:
                    return "bezier";
            }
            return "linear";
        };
        // Speed and influence of segment `index` (to the next key), at its
        // start or at its end, in the units After reads them in.
        const auto eases = [&](size_t index, bool outgoing) {
            const KeyOut& a = keys[index];
            const KeyOut& b = keys[index + 1];
            const double seconds = Seconds(a.frame, b.frame);
            Value list = Value::MakeArray();
            const auto push = [&](const KeyframeEasing& e, double v0, double v1,
                                  double span) {
                MotionAfterEase easeOut;
                MotionAfterEase easeIn;
                MotionAfterBezierEase(e.x1, e.y1, e.x2, e.y2, v0, v1, span,
                                      easeOut, easeIn);
                const MotionAfterEase& ease = outgoing ? easeOut : easeIn;
                list.Push(Array({ease.speed, ease.influence}));
            };
            const KeyframeEasing& first =
                a.easing.empty() ? kStraight : a.easing[0];
            if (model == EaseModel::Progress) {
                push(first, 0.0, 1.0, 1.0);
            } else if (model == EaseModel::Distance) {
                double sum = 0.0;
                for (size_t d = 0; d < 3 && d < a.value.size(); ++d) {
                    const double delta = (b.value[d] - a.value[d]) * 255.0;
                    sum += delta * delta;
                }
                push(first, 0.0, std::sqrt(sum), seconds);
            } else {
                for (size_t d = 0; d < a.value.size(); ++d) {
                    const KeyframeEasing& e =
                        a.easing.empty()
                            ? kStraight
                            : a.easing[std::min(d, a.easing.size() - 1)];
                    push(e, a.value[d], b.value[d], seconds);
                }
            }
            return list;
        };
        Value out = Value::MakeArray();
        for (size_t index = 0; index < keys.size(); ++index) {
            const KeyOut& key = keys[index];
            Value item = Value::MakeObject();
            item.Set("f", key.frame.Spelled());
            item.Set("v", spell ? spell(key.value) : Array(key.value));
            item.Set("out", Value::MakeString(type(key.out)));
            item.Set("in",
                     Value::MakeString(index == 0 ? "linear"
                                                  : type(keys[index - 1].out)));
            if (key.out == KeyframeInterpolation::CubicBezier &&
                index + 1 < keys.size()) {
                item.Set("ease_out", eases(index, true));
            }
            if (index > 0 &&
                keys[index - 1].out == KeyframeInterpolation::CubicBezier) {
                item.Set("ease_in", eases(index - 1, false));
            }
            out.Push(std::move(item));
            ++report_.keyframes;
        }
        return out;
    }

    // The segments of a track whose value changes, its key frames moved by
    // `origin` into absolute frames. A spring changes from its delay to
    // the composition's end, along no curve.
    void AddSegments(const MotionAnimation& animation, int64_t origin,
                     std::vector<Segment>& out) const {
        NoteCurves(animation);
        if (animation.kind == MotionAnimationKind::Spring) {
            out.push_back({origin + animation.delay,
                           composition_.duration,
                           KeyframeInterpolation::Linear,
                           {},
                           false});
            return;
        }
        const std::vector<Keyframe>& frames = animation.track.keyframes();
        for (size_t index = 0; index + 1 < frames.size(); ++index) {
            const bool numbers =
                animation.value_kind == MotionValueKind::Number;
            const bool same =
                numbers ? frames[index].value == frames[index + 1].value
                : index + 1 < animation.values.size()
                    ? animation.values[index].Dump() ==
                          animation.values[index + 1].Dump()
                    : true;
            if (same) {
                continue;
            }
            // MW2 -- a segment that starts or ends between two frames moves
            // the frames from the one before its start to the one after its
            // end. Its curve, cut to those whole frames, is not its own: the
            // frames read from the engine are then drawn by a straight line
            // or a key each (DerivedKeys).
            const KeyAt from = FrameOf(frames[index].time);
            const KeyAt to = FrameOf(frames[index + 1].time);
            out.push_back({origin + from.Floor(), origin + to.Ceil(),
                           frames[index].interpolation, frames[index].easing,
                           from.IsWhole() && to.IsWhole()});
        }
    }

    // What moves the layout: a size, a place, a spacing, a font's size --
    // anything the flexbox reads. Transforms, colours and paint do not.
    static bool IsLayoutProperty(const std::string& property) {
        static const char* layout[] = {
            "width",       "height",        "minWidth",     "minHeight",
            "maxWidth",    "maxHeight",     "top",          "right",
            "bottom",      "left",          "gap",          "rowGap",
            "columnGap",   "paddingTop",    "paddingRight", "paddingBottom",
            "paddingLeft", "marginTop",     "marginRight",  "marginBottom",
            "marginLeft",  "flexGrow",      "flexShrink",   "fontSize",
            "lineHeight",  "letterSpacing", "wordSpacing"};
        return std::find(std::begin(layout), std::end(layout), property) !=
               std::end(layout);
    }

    // Every segment of the composition that may move a box, in absolute
    // frames: a sequence's own animations run from its start, its
    // children's too.
    void CollectLayout(const MotionNode& node, int64_t origin) {
        const int64_t own =
            node.type == MotionNodeType::Sequence ? origin + node.from : origin;
        for (const auto& entry : node.animate) {
            if (IsLayoutProperty(entry.first)) {
                AddSegments(entry.second, own, layout_);
            }
        }
        for (const MotionNode& child : node.children) {
            CollectLayout(child, own);
        }
    }

    // What a layer's keys are read from, for one node at one frame.
    struct Sample {
        MotionBox box;
        double translate_x = 0.0;
        double translate_y = 0.0;
        double scale_x = 1.0;
        double scale_y = 1.0;
        double rotate = 0.0;
        bool has_clip = false;
        MotionResolvedClip clip;
        double border_radius = 0.0;
        bool overflow_hidden = false;
        // A text's lines: moved, a text keeps them; recomposed, it does not.
        std::string lines;
        double line_x = 0.0;
        double line_width = 0.0;
        double baseline = 0.0;
        double letter_spacing = 0.0;
        std::vector<MotionHighlightBand> highlights;
        MotionBox unit_bounds;
    };

    static void Record(const MotionEvaluatedNode& node,
                       std::unordered_map<std::string, Sample>& out) {
        out[node.path] = SampleOf(node);
        for (const MotionEvaluatedNode& child : node.children) {
            Record(child, out);
        }
    }

    // What one evaluated node shows, as a frame read for it keeps it.
    static Sample SampleOf(const MotionEvaluatedNode& node) {
        Sample sample;
        sample.box = node.box;
        sample.translate_x = node.translate_x;
        sample.translate_y = node.translate_y;
        sample.scale_x = node.scale_x;
        sample.scale_y = node.scale_y;
        sample.rotate = node.rotate;
        sample.has_clip = node.has_clip;
        sample.clip = node.clip_shape;
        sample.border_radius = node.border_radius;
        sample.overflow_hidden = node.overflow_hidden;
        sample.highlights = node.highlights;
        sample.letter_spacing = node.text_style.letter_spacing;
        if (!node.highlights.empty() && !node.units.empty()) {
            double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
            bool first = true;
            for (const MotionTextUnitState& unit : node.units) {
                MotionEvaluatedNode moving;
                if (node.unit_kind == MotionTextUnit::Line) {
                    const MotionTextLine& line =
                        node.text_layout.lines[unit.index];
                    moving.box = {line.x, line.y, line.width, line.height};
                } else {
                    const MotionTextSpan& span =
                        node.unit_kind == MotionTextUnit::Word
                            ? node.words[unit.index]
                            : node.chars[unit.index];
                    moving.box = {span.x, span.y, span.width, span.height};
                }
                moving.translate_x = unit.translate_x;
                moving.translate_y = unit.translate_y;
                moving.scale_x = unit.scale_x;
                moving.scale_y = unit.scale_y;
                moving.rotate = unit.rotate;
                const MotionAffine map = MotionNodeTransform(moving);
                // Glyph overhang grows and turns with the unit too.
                const double padding = node.text_style.font_size;
                for (const double x :
                     {moving.box.x - padding,
                      moving.box.x + moving.box.width + padding}) {
                    for (const double y :
                         {moving.box.y - padding,
                          moving.box.y + moving.box.height + padding}) {
                        double px = 0.0, py = 0.0;
                        map.Apply(x, y, px, py);
                        left = first ? px : std::min(left, px);
                        top = first ? py : std::min(top, py);
                        right = first ? px : std::max(right, px);
                        bottom = first ? py : std::max(bottom, py);
                        first = false;
                    }
                }
            }
            sample.unit_bounds = {left, top, right - left, bottom - top};
        }
        if (!node.text_layout.lines.empty()) {
            const MotionTextLine& first = node.text_layout.lines.front();
            for (const MotionTextLine& line : node.text_layout.lines) {
                sample.lines += line.text + "\n";
            }
            sample.line_x = first.x;
            sample.line_width = first.width;
            sample.baseline = first.baseline;
        }
        return sample;
    }

    // The node at `path` at absolute frame `frame`, or null when it is not
    // shown then (or the frame did not evaluate: error_ says so). Frames
    // are evaluated once and kept as these few numbers per node.
    const Sample* SampleAt(int64_t frame, const std::string& path) {
        if (frame < 0 || frame >= composition_.duration) {
            return nullptr;
        }
        auto& slot = samples_[frame];
        if (!slot) {
            slot = std::make_unique<std::unordered_map<std::string, Sample>>();
            MotionFrame evaluated;
            if (!EvaluateMotionFrame(composition_, props_, frame, measurer_,
                                     context_, evaluated, error_)) {
                return nullptr;
            }
            Record(evaluated.root, *slot);
        }
        const auto found = slot->find(path);
        return found == slot->end() ? nullptr : &found->second;
    }

    // Yoga lays boxes out in single precision: a box that only slides
    // differs from its track by millionths of a pixel. A thousandth is far
    // under what a frame shows, and over that noise.
    static constexpr double kLayoutEpsilon = 1e-3;

    static bool SameBox(const MotionBox& a, const MotionBox& b) {
        constexpr double kEpsilon = kLayoutEpsilon;
        return std::abs(a.x - b.x) < kEpsilon &&
               std::abs(a.y - b.y) < kEpsilon &&
               std::abs(a.width - b.width) < kEpsilon &&
               std::abs(a.height - b.height) < kEpsilon;
    }

    // How far the group the layer being emitted lives in has moved from its
    // reference place at absolute frame `frame`: the layer's space moves
    // with it, its own place is counted from there.
    std::pair<double, double> Displacement(int64_t frame) {
        if (follow_path_.empty()) {
            return {0.0, 0.0};
        }
        const Sample* sample = SampleAt(frame, follow_path_);
        if (sample == nullptr) {
            return {0.0, 0.0};
        }
        return {sample->box.x - follow_ref_.x, sample->box.y - follow_ref_.y};
    }

    // The box of `path` at `frame` as its group's space sees it.
    MotionBox Relative(const Sample& sample, int64_t frame) {
        const auto moved = Displacement(frame);
        MotionBox box = sample.box;
        box.x -= moved.first;
        box.y -= moved.second;
        return box;
    }

    // Whether the layout moves the box of `path`, in its group's space,
    // while `comp` shows it.
    bool LayoutMoves(const std::string& path, const Comp& comp,
                     const MotionBox& reference) {
        for (const Segment& segment : layout_) {
            const int64_t first = std::max(segment.from, comp.t0);
            const int64_t last =
                std::min(segment.to, comp.t0 + comp.duration - 1);
            for (int64_t frame = first; frame <= last; ++frame) {
                const Sample* sample = SampleAt(frame, path);
                if (sample != nullptr &&
                    !SameBox(Relative(*sample, frame), reference)) {
                    return true;
                }
            }
        }
        return false;
    }

    // MA11 -- whether the layout moves a point of a node -- its box's
    // corner, a text's origin -- in its group's space (or, `sized`, resizes
    // its box), and whether the left and top tracks of `own` (the node, or
    // the box whose animations a wrapped leaf carries) account for all of
    // it: their keys then say it, as they always did.
    struct Moves {
        bool moves = false;
        bool own_only = false;
    };
    Moves PointMoves(
        const MotionNode& own, const std::string& path, const Comp& comp,
        int64_t ref, bool sized, const MotionBox& reference_box,
        const std::pair<double, double>& origin,
        const std::function<std::pair<double, double>(const Sample&)>& point) {
        const MotionAnimation* left = Animation(own, "left");
        const MotionAnimation* top = Animation(own, "top");
        const bool springs =
            (left != nullptr && left->kind == MotionAnimationKind::Spring) ||
            (top != nullptr && top->kind == MotionAnimationKind::Spring);
        const auto at = [&](const MotionAnimation* track, int64_t frame) {
            if (track == nullptr) {
                return 0.0;
            }
            const KeyframeEvaluation value = EvaluateMotionTrack(
                *track, MotionFrameTime(composition_, frame));
            return value ? value.value : 0.0;
        };
        Moves result;
        result.own_only = (left != nullptr || top != nullptr) && !springs;
        for (const Segment& segment : layout_) {
            const int64_t last =
                std::min(segment.to, comp.t0 + comp.duration - 1);
            for (int64_t frame = std::max(segment.from, comp.t0); frame <= last;
                 ++frame) {
                const Sample* sample = SampleAt(frame, path);
                if (sample == nullptr) {
                    continue;
                }
                const auto moved = Displacement(frame);
                const auto here = point(*sample);
                const double dx = here.first - moved.first - origin.first;
                const double dy = here.second - moved.second - origin.second;
                const bool resized =
                    sized &&
                    (std::abs(sample->box.width - reference_box.width) >
                         kLayoutEpsilon ||
                     std::abs(sample->box.height - reference_box.height) >
                         kLayoutEpsilon);
                if (std::abs(dx) <= kLayoutEpsilon &&
                    std::abs(dy) <= kLayoutEpsilon && !resized) {
                    continue;
                }
                result.moves = true;
                const int64_t local = frame - comp.t0;
                if (resized ||
                    std::abs(dx - (at(left, local) - at(left, ref))) >
                        kLayoutEpsilon ||
                    std::abs(dy - (at(top, local) - at(top, ref))) >
                        kLayoutEpsilon) {
                    result.own_only = false;
                }
            }
        }
        if (!result.moves) {
            result.own_only = false;
        }
        return result;
    }

    // MA11 -- whether a box's layer takes its place and size from the
    // engine's frames: the layout moves or resizes it, and not only its own
    // left and top.
    bool FromFrames(const MotionNode& node, const std::string& path,
                    const Comp& comp, const MotionBox& reference, int64_t ref) {
        const Moves moves =
            PointMoves(node, path, comp, ref, true, reference,
                       {reference.x, reference.y}, [](const Sample& sample) {
                           return std::make_pair(sample.box.x, sample.box.y);
                       });
        return moves.moves && !moves.own_only;
    }

    // MA11 -- the keys of one property read from the engine rather than
    // from a track: its value at each end of the segments that may move it,
    // and between two of those the curve of one of the segments, cut to the
    // stretch (SubEasing), when it gives back every frame between them; a
    // key per frame when none does, said in the report. Null when the value
    // never moves. `value` reads the property from a sample, false when the
    // node is not shown; `dims` values for a per-dimension property, or a
    // mask's or a colour's, which take one curve for all.
    Value DerivedKeys(
        const std::vector<Segment>& segments, const Comp& comp,
        const std::string& path, const std::string& what, EaseModel model,
        double tolerance,
        const std::function<bool(const Sample&, std::vector<double>&)>& read,
        const std::function<Value(const std::vector<double>&)>& spell = nullptr,
        bool position = false) {
        // A place is counted in the group's space, which moves with it.
        int64_t reading = 0;
        const auto value = [&](const Sample& sample, std::vector<double>& out) {
            if (!read(sample, out)) {
                return false;
            }
            if (position) {
                const auto moved = Displacement(reading);
                out[0] -= what == "position.y" ? moved.second : moved.first;
            }
            return true;
        };
        const int64_t first = comp.t0;
        const int64_t last = comp.t0 + comp.duration - 1;
        std::vector<int64_t> ends;
        for (const Segment& segment : segments) {
            for (const int64_t end : {segment.from, segment.to}) {
                ends.push_back(std::clamp(end, first, last));
            }
        }
        std::sort(ends.begin(), ends.end());
        ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
        // The ends where the node is shown, and its value there.
        std::vector<int64_t> at;
        std::vector<std::vector<double>> values;
        for (const int64_t frame : ends) {
            const Sample* sample = SampleAt(frame, path);
            std::vector<double> read;
            reading = frame;
            if (sample != nullptr && value(*sample, read)) {
                at.push_back(frame);
                values.push_back(std::move(read));
            }
        }
        if (at.size() < 2) {
            return Value();
        }
        const size_t dims = values.front().size();
        // Every frame between the first end and the last, read once.
        std::map<int64_t, std::vector<double>> frames;
        for (int64_t frame = at.front(); frame <= at.back(); ++frame) {
            const Sample* sample = SampleAt(frame, path);
            std::vector<double> read;
            reading = frame;
            if (sample != nullptr && value(*sample, read) &&
                read.size() == dims) {
                frames[frame] = std::move(read);
            }
        }
        struct Candidate {
            KeyframeInterpolation interpolation;
            KeyframeEasing easing;
        };
        struct Stretch {
            size_t from = 0;
            size_t to = 0;
            bool flat = false;
            bool dense = false;
            KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
            std::vector<KeyframeEasing> easing;
        };
        // Whether one stretch, from end i to end j, is still, or drawn by
        // one curve -- a segment over it, cut to it, a straight line, a
        // hold -- at every frame between them.
        const auto draw = [&](size_t i, size_t j, Stretch& stretch) {
            const int64_t a = at[i];
            const int64_t b = at[j];
            const std::vector<double>& va = values[i];
            const std::vector<double>& vb = values[j];
            stretch = Stretch{i, j};
            bool flat = true;
            for (size_t d = 0; d < dims && flat; ++d) {
                flat = std::abs(va[d] - vb[d]) <= tolerance;
                for (auto it = frames.lower_bound(a);
                     flat && it != frames.end() && it->first <= b; ++it) {
                    flat = std::abs(it->second[d] - va[d]) <= tolerance;
                }
            }
            if (flat) {
                stretch.flat = true;
                return true;
            }
            std::vector<Candidate> candidates;
            for (const Segment& segment : segments) {
                if (!segment.curve || segment.from > a || segment.to < b ||
                    segment.to <= segment.from) {
                    continue;
                }
                const double span =
                    static_cast<double>(segment.to - segment.from);
                if (segment.interpolation == KeyframeInterpolation::Hold) {
                    candidates.push_back({KeyframeInterpolation::Hold, {}});
                } else if (segment.interpolation ==
                           KeyframeInterpolation::CubicBezier) {
                    candidates.push_back(
                        {KeyframeInterpolation::CubicBezier,
                         SubEasing(segment.easing, (a - segment.from) / span,
                                   (b - segment.from) / span)});
                }
            }
            candidates.push_back({KeyframeInterpolation::Linear, kStraight});
            const auto fits = [&](const Candidate& candidate, size_t d) {
                for (auto it = frames.upper_bound(a);
                     it != frames.end() && it->first < b; ++it) {
                    const double x = static_cast<double>(it->first - a) /
                                     static_cast<double>(b - a);
                    const double progress =
                        EaseAt(candidate.interpolation, candidate.easing, x);
                    if (std::abs(va[d] + (vb[d] - va[d]) * progress -
                                 it->second[d]) > tolerance) {
                        return false;
                    }
                }
                return true;
            };
            if (model == EaseModel::PerDimension) {
                // Each dimension its own curve -- After eases them apart --
                // as long as they agree on holding or not.
                bool held = false;
                bool eased = false;
                bool curved = false;
                for (size_t d = 0; d < dims; ++d) {
                    const Candidate* chosen = nullptr;
                    for (const Candidate& candidate : candidates) {
                        if (fits(candidate, d)) {
                            chosen = &candidate;
                            break;
                        }
                    }
                    if (chosen == nullptr) {
                        return false;
                    }
                    const bool moving = std::abs(va[d] - vb[d]) > tolerance;
                    held = held || (moving && chosen->interpolation ==
                                                  KeyframeInterpolation::Hold);
                    eased =
                        eased || (moving && chosen->interpolation !=
                                                KeyframeInterpolation::Hold);
                    curved = curved ||
                             (moving && chosen->interpolation ==
                                            KeyframeInterpolation::CubicBezier);
                    stretch.easing.push_back(
                        chosen->interpolation ==
                                KeyframeInterpolation::CubicBezier
                            ? chosen->easing
                            : kStraight);
                }
                if (held && eased) {
                    return false;
                }
                // Straight in every dimension that moves: linear keys.
                stretch.interpolation = held ? KeyframeInterpolation::Hold
                                        : curved
                                            ? KeyframeInterpolation::CubicBezier
                                            : KeyframeInterpolation::Linear;
                if (!curved) {
                    stretch.easing.clear();
                }
                return true;
            }
            for (const Candidate& candidate : candidates) {
                bool all = true;
                for (size_t d = 0; d < dims && all; ++d) {
                    all = fits(candidate, d);
                }
                if (all) {
                    stretch.interpolation = candidate.interpolation;
                    if (candidate.interpolation ==
                        KeyframeInterpolation::CubicBezier) {
                        stretch.easing = {candidate.easing};
                    }
                    return true;
                }
            }
            return false;
        };
        // From each end, the longest stretch one curve draws: a segment
        // other tracks cut stays whole. None: a key per frame to the next.
        std::vector<Stretch> stretches;
        bool dense = false;
        for (size_t i = 0; i + 1 < at.size();) {
            Stretch stretch;
            size_t j = at.size() - 1;
            for (; j > i; --j) {
                if (draw(i, j, stretch)) {
                    break;
                }
            }
            if (j == i) {
                stretch = Stretch{i, i + 1};
                stretch.dense = true;
                dense = true;
                j = i + 1;
            }
            stretches.push_back(stretch);
            i = j;
        }
        // Still at the start and at the end: After holds the first and the
        // last key.
        while (!stretches.empty() && stretches.front().flat) {
            stretches.erase(stretches.begin());
        }
        while (!stretches.empty() && stretches.back().flat) {
            stretches.pop_back();
        }
        if (stretches.empty()) {
            return Value();
        }
        std::vector<KeyOut> keys;
        for (const Stretch& stretch : stretches) {
            KeyOut key;
            key.frame = KeyAt::Whole(at[stretch.from] - comp.t0);
            key.value = values[stretch.from];
            key.out = stretch.flat || stretch.dense
                          ? KeyframeInterpolation::Linear
                          : stretch.interpolation;
            if (key.out == KeyframeInterpolation::CubicBezier) {
                key.easing = stretch.easing;
            }
            keys.push_back(key);
            if (stretch.dense) {
                for (auto it = frames.upper_bound(at[stretch.from]);
                     it != frames.end() && it->first < at[stretch.to]; ++it) {
                    KeyOut inside;
                    inside.frame = KeyAt::Whole(it->first - comp.t0);
                    inside.value = it->second;
                    keys.push_back(inside);
                }
            }
        }
        KeyOut closing;
        closing.frame = KeyAt::Whole(at[stretches.back().to] - comp.t0);
        closing.value = values[stretches.back().to];
        keys.push_back(closing);
        if (dense) {
            Note(path, what +
                           " : une clé par image où la mise en page la "
                           "bouge (aucune courbe du moteur ne la redonne)");
        }
        return WriteKeys(keys, model, spell);
    }

    // --- MA11: masks ---------------------------------------------------------
    // A clip shape as a mask path, flattened: per vertex its place, then its
    // in and out tangents, all in a layer's space (`place` maps a point of
    // the composition there). `rounded` keeps an inset's eight corner
    // vertices even where its radius is 0, so every key has as many.
    static std::vector<double> MaskVector(
        const MotionResolvedClip& clip, bool rounded,
        const std::function<std::pair<double, double>(double, double)>& place) {
        constexpr double kKappa = 0.5522847498307936;
        struct Vertex {
            double x, y, ix, iy, ox, oy;
        };
        std::vector<Vertex> vertices;
        if (clip.kind == MotionClipKind::Polygon) {
            for (const auto& point : clip.points) {
                vertices.push_back({point.first, point.second, 0, 0, 0, 0});
            }
        } else if (clip.kind == MotionClipKind::Inset) {
            const MotionBox& r = clip.rect;
            if (!rounded) {
                vertices = {{r.x, r.y, 0, 0, 0, 0},
                            {r.x + r.width, r.y, 0, 0, 0, 0},
                            {r.x + r.width, r.y + r.height, 0, 0, 0, 0},
                            {r.x, r.y + r.height, 0, 0, 0, 0}};
            } else {
                const double c = std::max(
                    0.0, std::min({clip.round, r.width / 2.0, r.height / 2.0}));
                const double k = kKappa * c;
                const double right = r.x + r.width;
                const double bottom = r.y + r.height;
                vertices = {{r.x + c, r.y, -k, 0, 0, 0},
                            {right - c, r.y, 0, 0, k, 0},
                            {right, r.y + c, 0, -k, 0, 0},
                            {right, bottom - c, 0, 0, 0, k},
                            {right - c, bottom, k, 0, 0, 0},
                            {r.x + c, bottom, 0, 0, -k, 0},
                            {r.x, bottom - c, 0, k, 0, 0},
                            {r.x, r.y + c, 0, 0, 0, -k}};
            }
        } else if (clip.kind == MotionClipKind::Circle ||
                   clip.kind == MotionClipKind::Ellipse) {
            const double kx = kKappa * clip.rx;
            const double ky = kKappa * clip.ry;
            vertices = {{clip.cx, clip.cy - clip.ry, -kx, 0, kx, 0},
                        {clip.cx + clip.rx, clip.cy, 0, -ky, 0, ky},
                        {clip.cx, clip.cy + clip.ry, kx, 0, -kx, 0},
                        {clip.cx - clip.rx, clip.cy, 0, ky, 0, -ky}};
        }
        std::vector<double> out;
        for (const Vertex& v : vertices) {
            const auto p = place(v.x, v.y);
            const auto in = place(v.x + v.ix, v.y + v.iy);
            const auto ot = place(v.x + v.ox, v.y + v.oy);
            out.insert(out.end(), {p.first, p.second, in.first - p.first,
                                   in.second - p.second, ot.first - p.first,
                                   ot.second - p.second});
        }
        return out;
    }

    static Value MaskShape(const std::vector<double>& flat) {
        Value vertices = Value::MakeArray();
        Value in = Value::MakeArray();
        Value out = Value::MakeArray();
        for (size_t at = 0; at + 5 < flat.size(); at += 6) {
            vertices.Push(Array({flat[at], flat[at + 1]}));
            in.Push(Array({flat[at + 2], flat[at + 3]}));
            out.Push(Array({flat[at + 4], flat[at + 5]}));
        }
        Value shape = Value::MakeObject();
        shape.Set("vertices", vertices);
        shape.Set("in", in);
        shape.Set("out", out);
        shape.Set("closed", Value::MakeBool(true));
        return shape;
    }

    // Where a composition point lands in a layer, from what the node shows
    // at one frame: the layer moves with its box when the layout moves it.
    using Place =
        std::function<std::pair<double, double>(const Sample&, double, double)>;

    // MA11 -- a node's clip-path (and, `overflow`, its own box when it hides
    // what overflows and the frame of its precomposition does not say it)
    // as masks of the layer: a mask path, keyed where the clip animates or
    // the layout moves it. The second mask intersects the first.
    Value Masks(const MotionNode& node, const MotionEvaluatedNode& ev,
                const std::string& path, const Comp& comp, bool moves,
                bool overflow, const Place& place) {
        Value masks = Value::MakeArray();
        // The still shape: the node as the reference frame shows it.
        const Sample still = SampleOf(ev);
        const Sample* reference = &still;
        const MotionAnimation* animated = Animation(node, "clipPath");
        const auto add =
            [&](const std::function<MotionResolvedClip(const Sample&)>& shapeOf,
                const std::vector<Segment>& segments, bool rounded) {
                const auto read = [&](const Sample& sample,
                                      std::vector<double>& out) {
                    const MotionResolvedClip clip = shapeOf(sample);
                    out = MaskVector(clip, rounded, [&](double x, double y) {
                        return place(sample, x, y);
                    });
                    return true;
                };
                std::vector<double> flat;
                read(*reference, flat);
                Value mask = Value::MakeObject();
                mask.Set("mode", Value::MakeString(masks.AsArray().empty()
                                                       ? "add"
                                                       : "intersect"));
                mask.Set("shape", MaskShape(flat));
                if (!segments.empty()) {
                    const Value keys = DerivedKeys(
                        segments, comp, path, "clipPath", EaseModel::Progress,
                        1e-3, read, &MaskShape);
                    if (keys.IsArray()) {
                        mask.Set("keys", keys);
                    }
                }
                masks.Push(std::move(mask));
            };
        if (overflow) {
            const bool rounded = ev.border_radius > 0.0;
            add(
                [](const Sample& sample) {
                    MotionResolvedClip clip;
                    clip.kind = MotionClipKind::Inset;
                    clip.rect = sample.box;
                    clip.round = sample.border_radius;
                    return clip;
                },
                moves ? layout_ : std::vector<Segment>(), rounded);
        }
        if (ev.has_clip || animated != nullptr) {
            std::vector<Segment> segments =
                moves ? layout_ : std::vector<Segment>();
            bool rounded = ev.clip_shape.kind == MotionClipKind::Inset &&
                           ev.clip_shape.round > 0.0;
            if (animated != nullptr) {
                AddSegments(*animated, comp.t0 + key_offset_, segments);
                for (const Value& text : animated->values) {
                    MotionClipShape shape;
                    if (text.IsString() &&
                        ParseMotionClipShape(text.AsString(), shape) &&
                        shape.kind == MotionClipKind::Inset &&
                        shape.round.value > 0.0) {
                        rounded = true;
                    }
                    if (shape.kind != ev.clip_shape.kind) {
                        Note(path,
                             "clipPath qui change de forme : masque de "
                             "l'image de référence");
                        segments.clear();
                        break;
                    }
                }
                for (const Keyframe& key : animated->track.keyframes()) {
                    if (key.interpolation ==
                            KeyframeInterpolation::CubicBezier &&
                        (key.easing.y1 < 0.0 || key.easing.y1 > 1.0 ||
                         key.easing.y2 < 0.0 || key.easing.y2 > 1.0)) {
                        Note(path,
                             "clipPath : une courbe qui dépasse ses clés, "
                             "qu'After borne sur un masque");
                        break;
                    }
                }
            }
            add([](const Sample& sample) { return sample.clip; }, segments,
                rounded);
        }
        return masks.AsArray().empty() ? Value() : masks;
    }

    // --- MA11: colours -------------------------------------------------------
    // A colour track as After's keys: [r, g, b, 1] with one ease per segment
    // (EaseModel::Distance), and its alpha apart, in percent, when it
    // moves. CSS mixes colours premultiplied: exact while the alpha holds,
    // or when one end is transparent (its colour is then the other end's,
    // which a straight mix gives too); anything else is said. `origin`:
    // the frame, in the composition's time, of the track's 0.
    bool ColourKeys(const MotionAnimation& animation, const std::string& path,
                    const std::string& what, int64_t origin, Value& colours,
                    Value& alpha) {
        if (animation.kind == MotionAnimationKind::Spring) {
            Note(path, what +
                           " en spring : non traduit, couleur de l'image "
                           "de référence");
            return false;
        }
        NoteCurves(animation);
        const std::vector<Keyframe>& frames = animation.track.keyframes();
        std::vector<MotionColor> colors;
        for (size_t index = 0; index < frames.size(); ++index) {
            MotionColor color;
            if (index >= animation.values.size() ||
                !animation.values[index].IsString() ||
                !ParseMotionColor(animation.values[index].AsString(), color)) {
                Note(path, what +
                               " animé vers autre chose qu'une couleur : "
                               "non traduit");
                return false;
            }
            colors.push_back(color);
        }
        // A transparent key takes its neighbour's colour; two neighbours
        // that differ leave it inexact.
        bool exact = true;
        std::vector<MotionColor> shown = colors;
        for (size_t index = 0; index < colors.size(); ++index) {
            if (colors[index].alpha > 0.0) {
                continue;
            }
            const MotionColor* before =
                index > 0 && colors[index - 1].alpha > 0.0 ? &colors[index - 1]
                                                           : nullptr;
            const MotionColor* after =
                index + 1 < colors.size() && colors[index + 1].alpha > 0.0
                    ? &colors[index + 1]
                    : nullptr;
            const MotionColor* take = after != nullptr ? after : before;
            if (take != nullptr) {
                shown[index].red = take->red;
                shown[index].green = take->green;
                shown[index].blue = take->blue;
            }
            if (before != nullptr && after != nullptr &&
                (before->red != after->red || before->green != after->green ||
                 before->blue != after->blue)) {
                exact = false;
            }
        }
        bool alphaMoves = false;
        for (size_t index = 0; index + 1 < colors.size(); ++index) {
            const MotionColor& a = colors[index];
            const MotionColor& b = colors[index + 1];
            alphaMoves = alphaMoves || a.alpha != b.alpha;
            if (a.alpha != b.alpha && a.alpha > 0.0 && b.alpha > 0.0 &&
                (a.red != b.red || a.green != b.green || a.blue != b.blue)) {
                exact = false;
            }
        }
        if (!exact) {
            Note(path, what +
                           " : couleur et transparence animées ensemble, "
                           "mêlées autrement par After (approché)");
        }
        std::vector<KeyOut> rgb;
        std::vector<KeyOut> opacity;
        for (size_t index = 0; index < frames.size(); ++index) {
            KeyOut key;
            key.frame = FrameOf(frames[index].time).Plus(origin);
            key.out = frames[index].interpolation;
            if (key.out == KeyframeInterpolation::CubicBezier) {
                key.easing = {frames[index].easing};
            }
            KeyOut fade = key;
            key.value = {shown[index].red, shown[index].green,
                         shown[index].blue, 1.0};
            fade.value = {colors[index].alpha * 100.0};
            rgb.push_back(key);
            opacity.push_back(fade);
        }
        colours = WriteKeys(rgb, EaseModel::Distance);
        alpha =
            alphaMoves ? WriteKeys(opacity, EaseModel::PerDimension) : Value();
        return true;
    }

    // MA11 -- the node whose `color` a text shows -- itself or the nearest
    // ancestor that sets one, as CSS inherits it -- when that colour is
    // animated: its track and the frame, in `comp`'s time, of the track's
    // 0. Null when the colour holds.
    const MotionAnimation* InheritedColour(const MotionNode& text,
                                           const Comp& comp,
                                           int64_t& origin) const {
        const auto sets = [](const MotionNode& node) {
            for (const auto& entry : node.style) {
                if (entry.first == "color") {
                    return true;
                }
            }
            return false;
        };
        if (const MotionAnimation* own = Animation(text, "color")) {
            origin = 0;
            return own;
        }
        if (sets(text)) {
            return nullptr;
        }
        for (auto it = ancestors_.rbegin(); it != ancestors_.rend(); ++it) {
            if (const MotionAnimation* track = Animation(*it->node, "color")) {
                origin = it->origin - comp.t0;
                return track;
            }
            if (sets(*it->node)) {
                return nullptr;
            }
        }
        return nullptr;
    }

    // MA12 -- After leaves out a layer whose scale is 0 at the frame's
    // instant, its motion blur's instants with it: a growth that starts at
    // 0 loses the first frame of its streak, which the engine draws from
    // the shutter's later instants (a trade-show loop, 30 September 2026).
    // With a shutter, a key at 0 is written a thousandth of a percent above
    // it: nothing shows, and After draws the streak. An opacity gains
    // nothing by it: After's blur never averages a fade (measured on 9
    // October).
    double Unculled(double percent) const {
        constexpr double kUnculled = 0.001;
        if (blur_ && std::abs(percent) < kUnculled) {
            return kUnculled;
        }
        return percent;
    }

    // A number as an expression spells it: no trailing zeros.
    static std::string Plain(double value) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.4f", value);
        std::string text(buffer);
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
        return text == "-0" ? "0" : text;
    }

    // --- transforms ---------------------------------------------------------
    // Transform of a layer turning about `pivot` (composition pixels), its
    // anchor at
    // `anchor_x`, `anchor_y` in the layer's own space, placed in a space whose
    // origin is `local_x`, `local_y`. `source` gives the animations and `ev`
    // the static values. `base_x`, `base_y`: a scale the layer needs before any
    // animation (an image fitted to its box).
    Value Transform(const MotionNode& source, const MotionEvaluatedNode& ev,
                    const std::string& path, const Pivot& pivot,
                    double anchor_x, double anchor_y, double local_x,
                    double local_y, double base_x, double base_y, int64_t ref) {
        Value transform = Value::MakeObject();
        Value keys = Value::MakeObject();
        // MOTION-WEB-2026-10 MW5 -- transformOrigin is After's anchor point:
        // the anchor moves from the box's centre to the origin, and the
        // position, the place the anchor lands, with it. A box the layout
        // resizes, or whose width After draws as a scale from its edge,
        // keeps its anchor where those need it: said.
        double shiftX = 0.0;
        double shiftY = 0.0;
        if (!ev.origin.IsCentre()) {
            if (layout_keys_ ||
                (source.children.empty() && HasAnimation(source, "width"))) {
                Note(path,
                     "transformOrigin sur une boîte que la mise en page "
                     "redimensionne : l'ancre reste au centre dans After");
            } else {
                shiftX = pivot.width * (ev.origin.x_fraction - 0.5) +
                         ev.origin.x_pixels;
                shiftY = pivot.height * (ev.origin.y_fraction - 0.5) +
                         ev.origin.y_pixels;
                anchor_x += shiftX / (base_x != 0.0 ? base_x : 1.0);
                anchor_y += shiftY / (base_y != 0.0 ? base_y : 1.0);
            }
        }
        const double centerX = pivot.CenterX() + shiftX - local_x;
        const double centerY = pivot.CenterY() + shiftY - local_y;
        transform.Set("anchor", Array({anchor_x, anchor_y}));
        transform.Set("position", Array({centerX + ev.translate_x,
                                         centerY + ev.translate_y}));
        transform.Set("scale", Array({ev.scale_x * base_x * 100.0,
                                      ev.scale_y * base_y * 100.0}));
        transform.Set("rotation", Num(ev.rotate));
        transform.Set("opacity", Num(ev.opacity * 100.0));

        const auto track =
            [&](const std::string& property) -> const MotionAnimation* {
            const MotionAnimation* animation = Animation(source, property);
            if (animation != nullptr &&
                animation->kind == MotionAnimationKind::Spring) {
                Note(path, property +
                               ": spring non traduit, valeur de l'image de "
                               "référence");
                return nullptr;
            }
            return animation;
        };
        const auto value_at = [&](const MotionAnimation& animation) {
            const KeyframeEvaluation value = EvaluateMotionTrack(
                animation, MotionFrameTime(composition_, ref));
            return value ? value.value : 0.0;
        };

        // Position: left/top move the box, translate moves it on top. MA11 --
        // with layout_keys_, the emitter writes the box's place from the
        // engine's frames, and an animated translate has gone to a null.
        Value expressions = Value::MakeObject();
        bool boiledX = false;
        bool boiledY = false;
        // MA13 -- a leaf whose width grows anchors on its left edge (the
        // scale below): left then places that edge, not the centre. A
        // group's width is its box's, which its children do not follow: it
        // scales nothing.
        const auto usable = [&](const char* property) {
            const MotionAnimation* animation = Animation(source, property);
            return animation != nullptr &&
                   animation->kind != MotionAnimationKind::Spring;
        };
        const bool group = !source.children.empty();
        const bool fromEdge = !layout_keys_ && !group && usable("width") &&
                              !usable("scale") && !usable("scaleX") &&
                              !usable("scaleY");
        const MotionAnimation* left = layout_keys_ ? nullptr : track("left");
        const MotionAnimation* translateX = track("translateX");
        if (left != nullptr) {
            const double contentX = pivot.x - value_at(*left);
            const double tx = ev.translate_x;
            const double toAnchor = fromEdge ? 0.0 : pivot.width / 2.0 + shiftX;
            keys.Set("position.x",
                     Keys(*left, [&](double v) -> std::vector<double> {
                         return {contentX + v + toAnchor - local_x + tx};
                     }));
            if (translateX != nullptr) {
                Note(path,
                     "left et translateX animés ensemble : translateX "
                     "gardé à sa valeur de l'image de référence");
            }
        } else if (translateX != nullptr) {
            if (Boil(source, *translateX, "translateX", path, expressions,
                     "position.x")) {
                boiledX = true;
            } else {
                keys.Set(
                    "position.x",
                    Keys(*translateX, [&](double v) -> std::vector<double> {
                        return {centerX + v};
                    }));
            }
        }
        const MotionAnimation* top = layout_keys_ ? nullptr : track("top");
        const MotionAnimation* translateY = track("translateY");
        if (top != nullptr) {
            const double contentY = pivot.y - value_at(*top);
            const double ty = ev.translate_y;
            keys.Set("position.y",
                     Keys(*top, [&](double v) -> std::vector<double> {
                         return {contentY + v + pivot.height / 2.0 + shiftY -
                                 local_y + ty};
                     }));
            if (translateY != nullptr) {
                Note(path,
                     "top et translateY animés ensemble : translateY "
                     "gardé à sa valeur de l'image de référence");
            }
        } else if (translateY != nullptr) {
            if (Boil(source, *translateY, "translateY", path, expressions,
                     "position.y")) {
                boiledY = true;
            } else {
                keys.Set(
                    "position.y",
                    Keys(*translateY, [&](double v) -> std::vector<double> {
                        return {centerY + v};
                    }));
            }
        }
        if (boiledX || boiledY) {
            // The expression adds the jump to the layer's own place, which a
            // designer can then move.
            transform.Set("position",
                          Array({centerX + (boiledX ? 0.0 : ev.translate_x),
                                 centerY + (boiledY ? 0.0 : ev.translate_y)}));
            transform.Set("expressions", expressions);
        }

        // Scale: one track becomes After's scale keyframes. MA11 -- two at
        // once (scale and scaleY: the engine sets each axis from the last
        // one written) are read from the engine's frames, each dimension
        // along its own curve, cut where the other track has a key.
        const MotionAnimation* scale = track("scale");
        const MotionAnimation* scaleX = track("scaleX");
        const MotionAnimation* scaleY = track("scaleY");
        const MotionAnimation* width =
            layout_keys_ || group ? nullptr : track("width");
        const int scaleTracks = (scale != nullptr ? 1 : 0) +
                                (scaleX != nullptr ? 1 : 0) +
                                (scaleY != nullptr ? 1 : 0);
        if (scaleTracks + (width != nullptr ? 1 : 0) > 1 &&
            (scaleTracks < 2 || layer_comp_ == nullptr)) {
            Note(path,
                 "plusieurs échelles animées ensemble : seule la "
                 "première est traduite");
        }
        if (scaleTracks >= 2 && layer_comp_ != nullptr) {
            std::vector<Segment> segments;
            const int64_t origin = layer_comp_->t0 + key_offset_;
            for (const MotionAnimation* animation : {scale, scaleX, scaleY}) {
                if (animation != nullptr) {
                    AddSegments(*animation, origin, segments);
                }
            }
            const Value derived = DerivedKeys(
                segments, *layer_comp_,
                source_path_.empty() ? path : source_path_, "échelle",
                EaseModel::PerDimension, 1e-3,
                [&](const Sample& sample, std::vector<double>& out) {
                    out = {Unculled(sample.scale_x * base_x * 100.0),
                           Unculled(sample.scale_y * base_y * 100.0)};
                    return true;
                });
            if (derived.IsArray()) {
                keys.Set("scale", derived);
            }
        } else if (scale != nullptr) {
            keys.Set("scale",
                     Keys(*scale, [&](double v) -> std::vector<double> {
                         return {Unculled(v * base_x * 100.0),
                                 Unculled(v * base_y * 100.0)};
                     }));
        } else if (scaleX != nullptr) {
            const double sy = ev.scale_y * base_y * 100.0;
            keys.Set("scale",
                     Keys(*scaleX, [&](double v) -> std::vector<double> {
                         return {Unculled(v * base_x * 100.0), sy};
                     }));
        } else if (scaleY != nullptr) {
            const double sx = ev.scale_x * base_x * 100.0;
            keys.Set("scale",
                     Keys(*scaleY, [&](double v) -> std::vector<double> {
                         return {sx, Unculled(v * base_y * 100.0)};
                     }));
        } else if (width != nullptr) {
            // A width that grows from the left edge: the layer scales from
            // an anchor on that edge (see the rectangle's anchor).
            const double sy = ev.scale_y * base_y * 100.0;
            const double full = std::max(1e-6, width_full_);
            keys.Set("scale",
                     Keys(*width, [&](double v) -> std::vector<double> {
                         return {v / full * 100.0, sy};
                     }));
            transform.Set("anchor", Array({0.0, anchor_y}));
            transform.Set("position", Array({pivot.x - local_x + ev.translate_x,
                                             centerY + ev.translate_y}));
        }
        if (const MotionAnimation* rotate = track("rotate")) {
            keys.Set("rotation",
                     Keys(*rotate, [&](double v) -> std::vector<double> {
                         return {v};
                     }));
        }
        if (const MotionAnimation* opacity = track("opacity")) {
            keys.Set("opacity",
                     Keys(*opacity, [&](double v) -> std::vector<double> {
                         return {std::clamp(v, 0.0, 1.0) * 100.0};
                     }));
        }
        Skew(source, ev, path, base_x, base_y, track, transform);
        for (const auto& entry : source.animate) {
            static const char* known[] = {
                "left", "top", "translateX", "translateY", "scale", "scaleX",
                "scaleY", "width", "rotate", "opacity", "filter", "trimStart",
                "trimEnd", "letterSpacing", "skewX", "skewY",
                // MA11 -- written by the layer itself: masks, colour keys
                // (a colour no text inherits changes nothing), the volume.
                "clipPath", "backgroundColor", "color", "volume"};
            const bool painted =
                source.type == MotionNodeType::Path &&
                (entry.first == "fill" || entry.first == "stroke");
            if (!painted && !(layout_keys_ && IsLayoutProperty(entry.first)) &&
                ((group && entry.first == "width") ||
                 std::find(std::begin(known), std::end(known), entry.first) ==
                     std::end(known))) {
                Note(path, entry.first + " animé : non traduit");
            }
        }
        transform.Set("keys", keys);
        return transform;
    }

    // MOTION-WEB-2026-10 MW5 -- After has no skew on a layer; the
    // Transform effect has one, last in the stack (a filter comes before
    // the transform, as in CSS), about the layer's anchor. Measured in
    // After 26 on 14 November 2026: its skew s on axis 0 moves y by
    // tan(s) * x, on axis 90 moves x by -tan(s) * y, and a larger axis
    // turns that shear against a layer's rotation. So skewX(a) is s = -a on
    // axis 90, skewY(a) s = a on axis 0. Where the effect acts depends on the
    // layer: a shape or a text layer is rasterized after its transform, so its
    // effect works in the composition's space -- motion_import.jsx then keeps
    // the pivot on the anchor and turns the axis with the layer by expressions,
    // which gives the engine's order exactly, whatever the scale. A footage or
    // a precomposition layer gets its effect in its own space, before its
    // scale: under a scale that is not uniform its angle is set so that the
    // two agree (`raster_angle`: tan s' = tan s * sy / sx for skewX), exact
    // while the scale holds. Both axes at once are no single skew.
    void Skew(
        const MotionNode& source, const MotionEvaluatedNode& ev,
        const std::string& path, double base_x, double base_y,
        const std::function<const MotionAnimation*(const std::string&)>& track,
        Value& transform) {
        const MotionAnimation* skewX = track("skewX");
        const MotionAnimation* skewY = track("skewY");
        const bool alongX = skewX != nullptr || ev.skew_x != 0.0;
        const bool alongY = skewY != nullptr || ev.skew_y != 0.0;
        if (!alongX && !alongY) {
            return;
        }
        if (alongX && alongY) {
            Note(path,
                 "skewX et skewY ensemble : seule l'inclinaison horizontale "
                 "est traduite (l'effet Transformer n'en a qu'une)");
        }
        const double sx = ev.scale_x * base_x;
        const double sy = ev.scale_y * base_y;
        const double ratio =
            alongX ? (sx != 0.0 ? sy / sx : 1.0) : (sy != 0.0 ? sx / sy : 1.0);
        const double sign = alongX ? -1.0 : 1.0;
        const auto raster = [ratio, sign](double degrees) {
            return sign * std::atan(std::tan(degrees * kPi / 180.0) * ratio) *
                   180.0 / kPi;
        };
        const MotionAnimation* animated = alongX ? skewX : skewY;
        const double still = alongX ? ev.skew_x : ev.skew_y;
        Value skew = Value::MakeObject();
        skew.Set("axis", Num(alongX ? 90.0 : 0.0));
        skew.Set("angle", Num(sign * still));
        if (animated != nullptr) {
            skew.Set("keys",
                     Keys(*animated, [&](double v) -> std::vector<double> {
                         return {sign * v};
                     }));
        }
        if (ratio != 1.0) {
            skew.Set("raster_angle", Num(raster(still)));
            if (animated != nullptr) {
                skew.Set("raster_keys",
                         Keys(*animated, [&](double v) -> std::vector<double> {
                             return {raster(v)};
                         }));
            }
        }
        transform.Set("skew", skew);
        const bool scaled = Animation(source, "scale") != nullptr ||
                            Animation(source, "scaleX") != nullptr ||
                            Animation(source, "scaleY") != nullptr;
        if (ratio != 1.0 && (animated != nullptr || scaled)) {
            Note(path, std::string(alongX ? "skewX" : "skewY") +
                           " sous une échelle non uniforme : approché sur "
                           "une image ou une précomposition");
        }
        if (blur_ && animated != nullptr) {
            Note(path, std::string(alongX ? "skewX" : "skewY") +
                           " animé : l'effet Transformer n'a pas le flou de "
                           "mouvement du calque");
        }
    }

    // --- effects ------------------------------------------------------------
    Value Effects(const MotionNode& source, const MotionEvaluatedNode& ev,
                  const std::string& path) {
        Value effects = Value::MakeArray();
        const MotionAnimation* animated = Animation(source, "filter");
        std::vector<MotionFilter> filters = ev.filters;
        if (filters.empty() && animated != nullptr &&
            !animated->values.empty() && animated->values[0].IsString()) {
            ParseMotionFilters(animated->values[0].AsString(), filters);
        }
        for (size_t index = 0; index < filters.size(); ++index) {
            const MotionFilter& filter = filters[index];
            if (filter.kind != MotionFilterKind::Slice) {
                Note(path, "filtre autre que slice() : non traduit");
                continue;
            }
            Value effect = Value::MakeObject();
            effect.Set("kind", Value::MakeString("wave_warp"));
            effect.Set("height", Num(filter.amplitude));
            effect.Set("width", Num(std::max(2.0, filter.wavelength * 2.0)));
            if (animated != nullptr) {
                // The amplitude of this slice at each keyframe of the list.
                effect.Set(
                    "height_keys",
                    Keys(*animated, [&](double v) -> std::vector<double> {
                        std::vector<MotionFilter> at;
                        const auto slot = static_cast<size_t>(v);
                        if (slot < animated->values.size() &&
                            animated->values[slot].IsString()) {
                            ParseMotionFilters(
                                animated->values[slot].AsString(), at);
                        }
                        return {index < at.size() ? at[index].amplitude : 0.0};
                    }));
            }
            effects.Push(std::move(effect));
            // One note says it for every slice(); MA2 keeps each node it
            // is about, shown only while its slice cuts.
            if (!slice_noted_) {
                slice_note_ = report_.notes.size();
                report_.notes.push_back(path +
                                        ": slice() → Wave Warp carré "
                                        "(approché) ; les autres slice() "
                                        "aussi");
                slice_noted_ = true;
            }
            report_.scopes.push_back({slice_note_, path, true});
        }
        return effects;
    }

    // --- nodes --------------------------------------------------------------
    // What in a node's own values makes it a group After can only render as
    // one, its filter aside.
    // MW5 -- a skew is an effect in After, which a null does not pass on
    // to its children: a skewed group is a precomposition.
    static bool GroupedByValues(const MotionEvaluatedNode& ev) {
        const bool blend = !ev.blend_mode.empty() && ev.blend_mode != "normal";
        return ev.opacity < 0.999 || blend || ev.overflow_hidden ||
               ev.isolate || ev.has_mask || ev.has_clip || ev.skew_x != 0.0 ||
               ev.skew_y != 0.0;
    }

    static bool NeedsGroupBesidesFilter(const MotionNode& node,
                                        const MotionEvaluatedNode& ev) {
        return HasAnimation(node, "opacity") || HasAnimation(node, "skewX") ||
               HasAnimation(node, "skewY") || GroupedByValues(ev);
    }

    static bool HasFilter(const MotionNode& node,
                          const MotionEvaluatedNode& ev) {
        return !ev.filters.empty() || HasAnimation(node, "filter");
    }

    bool NeedsGroup(const MotionNode& node, const MotionEvaluatedNode& ev) {
        return NeedsGroupBesidesFilter(node, ev) || HasFilter(node, ev) ||
               HasAnimation(node, "clipPath");
    }

    // MA5 -- MOTION-AFTER-2026-09. A text is cut into units after layout and
    // `animate` then moves each unit, so a filter over the whole text goes on
    // a box around it. After says the same thing with an effect on the text
    // layer: the box folds into the text when a group is only what its
    // filter asked for and the text loses nothing by carrying the box --
    // no filter, opacity, blend or transform of its own, and animations, if
    // any, that are its units'.
    bool FoldsFilterIntoText(const MotionNode& node,
                             const MotionEvaluatedNode& ev,
                             const std::string& path, int64_t at) {
        if (!HasFilter(node, ev) || NeedsGroupBesidesFilter(node, ev) ||
            ev.has_background || node.children.size() != 1) {
            return false;
        }
        const MotionNode& text = node.children[0];
        if (text.type != MotionNodeType::Text ||
            (!text.animate.empty() && text.units == MotionTextUnit::None)) {
            return false;
        }
        // A text in units animates its units, never itself: its own values
        // are the static ones read here.
        const MotionEvaluatedNode* own = At(at, path + ".children[0]");
        return own != nullptr && own->filters.empty() &&
               !GroupedByValues(*own) && IsIdentity(*own);
    }

    static bool IsIdentity(const MotionEvaluatedNode& ev) {
        return ev.translate_x == 0.0 && ev.translate_y == 0.0 &&
               ev.scale_x == 1.0 && ev.scale_y == 1.0 && ev.rotate == 0.0 &&
               ev.skew_x == 0.0 && ev.skew_y == 0.0;
    }

    Value BaseLayer(const std::string& kind, const std::string& name,
                    const std::string& parent, const MotionEvaluatedNode& ev,
                    const std::string& path) {
        Value layer = Value::MakeObject();
        layer.Set("id", Value::MakeString(NextLayer()));
        layer.Set("kind", Value::MakeString(kind));
        layer.Set("name", Value::MakeString(Shorten(name)));
        layer.Set("path", Value::MakeString(path));
        if (!parent.empty()) {
            layer.Set("parent", Value::MakeString(parent));
        }
        if (!ev.blend_mode.empty() && ev.blend_mode != "normal") {
            const std::string blend = AfterBlend(ev.blend_mode);
            if (blend.empty()) {
                Note(path, "mixBlendMode " + ev.blend_mode +
                               " : pas d'équivalent de calque, ignoré");
            } else {
                layer.Set("blend", Value::MakeString(blend));
            }
        }
        ++report_.layers;
        return layer;
    }

    bool EmitChildren(const MotionNode& node, const std::string& path,
                      const MotionEvaluatedNode& ev, Comp& comp,
                      const std::string& parent, double local_x, double local_y,
                      int64_t ref) {
        if (ev.has_background) {
            EmitRect(node, ev, path, comp, parent, local_x, local_y, ref, true);
        }
        PlanFlow(node, path, comp.t0 + ref);
        // Its animations run in the time of the composition its children
        // are emitted into (a sequence's own, from its start).
        ancestors_.push_back({&node, comp.t0});
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (!Emit(node.children[index],
                      path + ".children[" + std::to_string(index) + "]", comp,
                      parent, local_x, local_y, ref, nullptr, nullptr)) {
                ancestors_.pop_back();
                return false;
            }
        }
        ancestors_.pop_back();
        return true;
    }

    // MA3 -- docs/roadmap/MOTION-AFTER-2026-09.md. The node at a structural
    // path of `root` ("root", "root.children[2].children[0]"), or null when
    // the path does not name a child by its index all the way down.
    static const MotionNode* NodeAt(const MotionNode& root,
                                    const std::string& path) {
        static const std::string kChild = ".children[";
        if (path.compare(0, 4, "root") != 0) {
            return nullptr;
        }
        const MotionNode* node = &root;
        size_t at = 4;
        while (at < path.size()) {
            if (path.compare(at, kChild.size(), kChild) != 0) {
                return nullptr;
            }
            at += kChild.size();
            const size_t close = path.find(']', at);
            if (close == std::string::npos || close == at || close - at > 9 ||
                path.find_first_not_of("0123456789", at) != close) {
                return nullptr;
            }
            const size_t index = std::stoul(path.substr(at, close - at));
            if (index >= node->children.size()) {
                return nullptr;
            }
            node = &node->children[index];
            at = close + 1;
        }
        return node;
    }

    static bool HasStyle(const MotionNode& node, const std::string& property) {
        return std::any_of(
            node.style.begin(), node.style.end(),
            [&](const auto& entry) { return entry.first == property; });
    }

    // MA3 -- a row of words stays a row in After. CSS pushes a word along
    // when the word before it grows; After places each text layer where it
    // was put. A row of single-line texts (a box laid out in a row, not
    // wrapping, every child a text the flow places) gets, on each text, an
    // expression that moves it by the change of width of the texts whose
    // width moves it, measured by After itself (sourceRectAtTime) against
    // the width it measured when it built the project: untouched, every
    // change is 0 and the pixels are the engine's; a word replaced in After
    // pushes the others as the engine would.
    //
    // How much a word moves when another grows is the engine's to say, not
    // a guess at flexbox: each text the row sizes by its content is widened
    // by 64 px in a copy of the composition, the frame is laid out again,
    // and every text's move over 64 is its coefficient (1 for a word after
    // it in a row packed to the start, 0.5 and -0.5 around a centred one).
    // A row the widening changes otherwise -- a text that shrinks instead,
    // a line that wraps, a word that moves vertically -- keeps its fixed
    // positions, which are exact.
    void PlanFlow(const MotionNode& row, const std::string& path, int64_t at) {
        constexpr double kWiden = 64.0;
        if (row.children.size() < 2 || (row.type != MotionNodeType::Box &&
                                        row.type != MotionNodeType::Fill)) {
            return;
        }
        const std::string direction = StyleString(row, "flexDirection");
        const std::string wrap = StyleString(row, "flexWrap");
        if ((row.type == MotionNodeType::Fill
                 ? direction != "row"
                 : !direction.empty() && direction != "row") ||
            (!wrap.empty() && wrap != "nowrap")) {
            return;
        }
        std::vector<std::string> paths;
        std::vector<const MotionEvaluatedNode*> placed;
        std::vector<bool> sized;
        for (size_t index = 0; index < row.children.size(); ++index) {
            const MotionNode& child = row.children[index];
            const std::string childPath =
                path + ".children[" + std::to_string(index) + "]";
            const MotionEvaluatedNode* ev = At(at, childPath);
            const bool boils =
                std::find(options_.boil.begin(), options_.boil.end(),
                          child.id) != options_.boil.end();
            if (child.type != MotionNodeType::Text || ev == nullptr ||
                StyleString(child, "position") == "absolute" ||
                child.units != MotionTextUnit::None ||
                !child.highlights.empty() ||
                HasAnimation(child, "letterSpacing") || boils ||
                (child.content.has_value() &&
                 child.content->kind == MotionBindingKind::Timecode) ||
                ev->text_layout.lines.size() != 1 ||
                (!ev->text_style.text_align.empty() &&
                 ev->text_style.text_align != "left") ||
                NodeAt(composition_.root, childPath) != &child) {
                return;
            }
            paths.push_back(childPath);
            placed.push_back(ev);
            // A text whose box is not its content's width does not push
            // anything when its words change.
            sized.push_back(
                !HasStyle(child, "width") && !HasStyle(child, "minWidth") &&
                !HasStyle(child, "maxWidth") && !HasStyle(child, "flexBasis") &&
                !HasStyle(child, "flexGrow") && !HasAnimation(child, "width"));
        }
        std::vector<std::vector<std::pair<std::string, double>>> terms(
            paths.size());
        for (size_t j = 0; j < paths.size(); ++j) {
            if (!sized[j]) {
                continue;
            }
            MotionComposition widened = composition_;
            auto* target =
                const_cast<MotionNode*>(NodeAt(widened.root, paths[j]));
            if (target == nullptr) {
                return;
            }
            MotionValue width;
            width.literal = Value::MakeDouble(placed[j]->box.width + kWiden);
            target->style.emplace_back("width", width);
            FrameIndex index;
            MotionError ignored;
            if (!EvaluateMotionFrame(widened, props_, at, measurer_, context_,
                                     index.frame, ignored)) {
                return;
            }
            IndexNodes(index.frame.root, index);
            for (size_t i = 0; i < paths.size(); ++i) {
                const auto found = index.by_path.find(paths[i]);
                if (found == index.by_path.end()) {
                    return;
                }
                const MotionBox& now = found->second->box;
                const MotionBox& before = placed[i]->box;
                const double grown = i == j ? kWiden : 0.0;
                if (std::abs(now.y - before.y) > kLayoutEpsilon ||
                    std::abs(now.width - before.width - grown) >
                        kLayoutEpsilon ||
                    found->second->text_layout.lines.size() != 1) {
                    return;
                }
                // Yoga is single precision: a coefficient is read to the
                // ten-thousandth.
                const double k =
                    std::round((now.x - before.x) / kWiden * 10000.0) / 10000.0;
                if (k != 0.0) {
                    terms[i].emplace_back(paths[j], k);
                }
            }
        }
        for (size_t i = 0; i < paths.size(); ++i) {
            flows_[paths[i]] = Flow{terms[i], sized[i]};
        }
    }

    // MA3 -- the flow of a text layer, its terms naming layers by the path
    // of their node until Finish knows every layer of the composition. A
    // text that scales, turns or skews turns about a point of its box (its
    // centre, or its transformOrigin): when its own word changes, that point
    // moves by the same fraction of the change, and so must its anchor
    // (`anchor`, the fraction), or a scale would grow from the old centre.
    void SetFlow(Value& layer, const std::string& path, const MotionNode& node,
                 const MotionEvaluatedNode& ev) {
        const auto found = flows_.find(path);
        if (found == flows_.end()) {
            return;
        }
        const bool turns =
            found->second.sized && ev.origin.x_fraction != 0.0 &&
            (ev.scale_x != 1.0 || ev.scale_y != 1.0 || ev.rotate != 0.0 ||
             ev.skew_x != 0.0 || ev.skew_y != 0.0 ||
             std::any_of(node.animate.begin(), node.animate.end(),
                         [](const auto& entry) {
                             return entry.first == "scale" ||
                                    entry.first == "scaleX" ||
                                    entry.first == "scaleY" ||
                                    entry.first == "rotate" ||
                                    entry.first == "skewX" ||
                                    entry.first == "skewY";
                         }));
        if (found->second.terms.empty() && !turns) {
            return;
        }
        Value terms = Value::MakeArray();
        for (const auto& [source, k] : found->second.terms) {
            Value term = Value::MakeObject();
            term.Set("path", Value::MakeString(source));
            term.Set("k", Num(k));
            terms.Push(std::move(term));
        }
        Value flow = Value::MakeObject();
        flow.Set("terms", terms);
        if (turns) {
            flow.Set("anchor", Num(ev.origin.x_fraction));
            flow.Set("self", Value::MakeString(path));
        }
        layer.Set("flow", flow);
    }

    // A leaf carrying the animations of the box around it: the box is its
    // parent for what it inherits.
    bool EmitWrapped(const MotionNode& node, const std::string& path,
                     const MotionEvaluatedNode& ev, Comp& comp,
                     const std::string& parent, double local_x, double local_y,
                     int64_t ref) {
        ancestors_.push_back({&node, comp.t0});
        const bool done = Emit(node.children[0], path + ".children[0]", comp,
                               parent, local_x, local_y, ref, &node, &ev);
        ancestors_.pop_back();
        return done;
    }

    void EmitRect(const MotionNode& node, const MotionEvaluatedNode& ev,
                  const std::string& path, Comp& comp,
                  const std::string& parent, double local_x, double local_y,
                  int64_t ref, bool background_only) {
        Pivot pivot{ev.box.x, ev.box.y, ev.box.width, ev.box.height};
        // MA11 -- a box the layout resizes or moves: a rectangle centred on
        // the layer, its size keyed, the layer's place keyed, both read
        // from the engine's frames. A group's background lives in the
        // group's space, which carries the group's own moves.
        const bool moves = background_only
                               ? LayoutMoves(path, comp, ev.box)
                               : FromFrames(node, path, comp, ev.box, ref) &&
                                     !GrowsFromLeft(node, path, comp, ev, ref);
        MotionNode rest = node;
        MotionEvaluatedNode restEv = ev;
        const std::string taken =
            background_only
                ? parent
                : Prepared(node, ev, path, comp, parent, rest, restEv, moves);
        Value layer =
            BaseLayer("rect", node.id.empty() ? "fond" : Display(node.id),
                      taken, ev, path);
        width_full_ = pivot.width;
        if (const MotionAnimation* width = Animation(node, "width");
            width != nullptr && !moves) {
            double most = 0.0;
            for (const Keyframe& key : width->track.keyframes()) {
                most = std::max(most, key.value);
            }
            width_full_ = most;
        }
        Value rect = Value::MakeObject();
        rect.Set("size", Array({width_full_, pivot.height}));
        rect.Set("color", Rgb(ev.background));
        rect.Set("alpha", Num(ev.background.alpha));
        rect.Set("roundness", Num(ev.border_radius));
        if (const MotionAnimation* fill = Animation(node, "backgroundColor")) {
            Value colours;
            Value alpha;
            if (ColourKeys(*fill, path, "backgroundColor", 0, colours, alpha)) {
                rect.Set("color_keys", colours);
                if (alpha.IsArray()) {
                    rect.Set("alpha_keys", alpha);
                }
            }
        }
        if (moves) {
            rect.Set("centered", Value::MakeBool(true));
            const Value size = DerivedKeys(
                layout_, comp, path, "taille", EaseModel::PerDimension, 1e-3,
                [](const Sample& sample, std::vector<double>& out) {
                    out = {sample.box.width, sample.box.height};
                    return true;
                });
            if (size.IsArray()) {
                rect.Set("size_keys", size);
            }
        }
        layer.Set("rect", rect);
        const double anchorX = moves ? 0.0 : width_full_ / 2.0;
        const double anchorY = moves ? 0.0 : pivot.height / 2.0;
        layer_comp_ = &comp;
        source_path_.clear();
        layout_keys_ = moves;
        Value transform;
        if (background_only) {
            // The background of a group: it takes the group's place, never
            // its animations, which the group layer carries.
            MotionNode still;
            MotionEvaluatedNode flat = ev;
            flat.translate_x = flat.translate_y = 0.0;
            flat.scale_x = flat.scale_y = 1.0;
            flat.rotate = 0.0;
            flat.skew_x = flat.skew_y = 0.0;
            flat.origin = MotionTransformOrigin{};
            flat.opacity = 1.0;
            transform = Transform(still, flat, path, pivot, anchorX, anchorY,
                                  local_x, local_y, 1.0, 1.0, ref);
        } else {
            transform = Transform(rest, restEv, path, pivot, anchorX, anchorY,
                                  local_x, local_y, 1.0, 1.0, ref);
        }
        layout_keys_ = false;
        if (moves) {
            // A translation the null does not carry stays in the place.
            const bool withX =
                !background_only && !HasAnimation(node, "translateX");
            const bool withY =
                !background_only && !HasAnimation(node, "translateY");
            LayoutPosition(transform, comp, path, local_x, local_y,
                           [&](const Sample& sample, double& x, double& y) {
                               x = sample.box.x + sample.box.width / 2.0 +
                                   (withX ? sample.translate_x : 0.0);
                               y = sample.box.y + sample.box.height / 2.0 +
                                   (withY ? sample.translate_y : 0.0);
                               return true;
                           });
        }
        layer.Set("transform", transform);
        if (!background_only) {
            Value effects = Effects(node, ev, path);
            if (!effects.AsArray().empty()) {
                layer.Set("effects", effects);
            }
            // The layer's space goes with its box, wherever the box is.
            const Value masks = Masks(
                node, ev, path, comp, moves, false,
                [&](const Sample& sample, double x, double y) {
                    return moves ? std::make_pair(x - (sample.box.x +
                                                       sample.box.width / 2.0),
                                                  y - (sample.box.y +
                                                       sample.box.height / 2.0))
                                 : std::make_pair(x - sample.box.x,
                                                  y - sample.box.y);
                });
            if (masks.IsArray()) {
                layer.Set("masks", masks);
            }
        }
        comp.layers.Push(std::move(layer));
    }

    // A bar that grows from its left edge by its own width track, square
    // and alone: an x scale anchored on that edge says it, After's idiom.
    bool GrowsFromLeft(const MotionNode& node, const std::string& path,
                       const Comp& comp, const MotionEvaluatedNode& ev,
                       int64_t ref) {
        (void)ref;
        const MotionAnimation* width = Animation(node, "width");
        if (width == nullptr || width->kind == MotionAnimationKind::Spring ||
            ev.border_radius > 0.0 || HasAnimation(node, "height") ||
            HasAnimation(node, "left") || HasAnimation(node, "top")) {
            return false;
        }
        for (const Segment& segment : layout_) {
            const int64_t last =
                std::min(segment.to, comp.t0 + comp.duration - 1);
            for (int64_t frame = std::max(segment.from, comp.t0); frame <= last;
                 ++frame) {
                const Sample* sample = SampleAt(frame, path);
                if (sample == nullptr) {
                    continue;
                }
                const MotionBox box = Relative(*sample, frame);
                const KeyframeEvaluation grown = EvaluateMotionTrack(
                    *width, MotionFrameTime(composition_, frame - comp.t0));
                if (!grown || std::abs(box.x - ev.box.x) > kLayoutEpsilon ||
                    std::abs(box.y - ev.box.y) > kLayoutEpsilon ||
                    std::abs(box.height - ev.box.height) > kLayoutEpsilon ||
                    std::abs(box.width - grown.value) > kLayoutEpsilon) {
                    return false;
                }
            }
        }
        return true;
    }

    // MA11 -- the position keys of a layer the layout moves: `point` reads,
    // from what the node shows at a frame, where the layer's anchor lands
    // in composition pixels.
    void LayoutPosition(
        Value& transform, const Comp& comp, const std::string& path,
        double local_x, double local_y,
        const std::function<bool(const Sample&, double&, double&)>& point) {
        Value keys = *transform.Find("keys");
        for (int axis = 0; axis < 2; ++axis) {
            const Value derived = DerivedKeys(
                layout_, comp, path, axis == 0 ? "position.x" : "position.y",
                EaseModel::PerDimension, 1e-3,
                [&](const Sample& sample, std::vector<double>& out) {
                    double x = 0.0;
                    double y = 0.0;
                    if (!point(sample, x, y)) {
                        return false;
                    }
                    out = {axis == 0 ? x - local_x : y - local_y};
                    return true;
                },
                nullptr, true);
            if (derived.IsArray()) {
                keys.Set(axis == 0 ? "position.x" : "position.y", derived);
            }
        }
        transform.Set("keys", keys);
    }

    // left and translateX (or top and translateY) animated together -- or,
    // MA11, a translation on a box the layout moves: a null at the local
    // origin carries the translation, the layer the box's place. Returns
    // the parent the layer takes.
    std::string SplitTranslate(const MotionNode& source,
                               const std::string& path, Comp& comp,
                               const std::string& parent, MotionNode& rest,
                               bool moves) {
        const bool x = HasAnimation(source, "translateX") &&
                       (moves || HasAnimation(source, "left"));
        const bool y = HasAnimation(source, "translateY") &&
                       (moves || HasAnimation(source, "top"));
        rest = source;
        if (!x && !y) {
            return parent;
        }
        MotionNode carrier;
        for (const auto& entry : source.animate) {
            if ((x && entry.first == "translateX") ||
                (y && entry.first == "translateY")) {
                carrier.animate.push_back(entry);
            }
        }
        rest.animate.erase(
            std::remove_if(rest.animate.begin(), rest.animate.end(),
                           [&](const auto& entry) {
                               return (x && entry.first == "translateX") ||
                                      (y && entry.first == "translateY");
                           }),
            rest.animate.end());
        MotionEvaluatedNode still;
        Value layer = BaseLayer("null", "translation", parent, still, path);
        const std::string id = layer.Find("id")->AsString();
        // A null with its anchor on its origin, at the local origin: its
        // position is the translation alone.
        layer_comp_ = &comp;
        source_path_.clear();
        layer.Set("transform", Transform(carrier, still, path, Pivot{}, 0.0,
                                         0.0, 0.0, 0.0, 1.0, 1.0, 0));
        comp.layers.Push(std::move(layer));
        return id;
    }

    // The animations and static values a layer keeps once its translation
    // may have gone to a carrier null; returns the parent it then takes.
    std::string Prepared(const MotionNode& source,
                         const MotionEvaluatedNode& ev, const std::string& path,
                         Comp& comp, const std::string& parent,
                         MotionNode& rest, MotionEvaluatedNode& rest_ev,
                         bool moves = false) {
        const std::string taken =
            SplitTranslate(source, path, comp, parent, rest, moves);
        rest_ev = ev;
        if (taken != parent) {
            if (HasAnimation(source, "translateX") &&
                (moves || HasAnimation(source, "left"))) {
                rest_ev.translate_x = 0.0;
            }
            if (HasAnimation(source, "translateY") &&
                (moves || HasAnimation(source, "top"))) {
                rest_ev.translate_y = 0.0;
            }
        }
        return taken;
    }

    // The timeline's sounds, at the bottom of the main composition.
    void EmitAudio(Comp& main) {
        constexpr double kSilence = -96.0;  // After's level for nothing
        for (const MotionAfterAudio& sound : options_.audio) {
            Value layer = Value::MakeObject();
            layer.Set("id", Value::MakeString(NextLayer()));
            layer.Set("kind", Value::MakeString("audio"));
            layer.Set("name", Value::MakeString(Shorten(sound.name)));
            layer.Set("footage", Value::MakeString(FootageFor(sound.path)));
            layer.Set("start_s", Num(sound.timeline_in - sound.source_in));
            layer.Set("in_s", Num(sound.timeline_in));
            layer.Set("out_s", Num(sound.timeline_in + sound.duration));
            layer.Set("gain_db", Num(sound.gain_db));
            Value levels = Value::MakeArray();
            const auto key = [&](double seconds, double db) {
                levels.Push(Array({seconds, db}));
            };
            const double in = sound.timeline_in;
            const double out = sound.timeline_in + sound.duration;
            if (sound.fade_in > 0.0) {
                key(in, kSilence);
                key(in + sound.fade_in, sound.gain_db);
            }
            if (sound.fade_out > 0.0) {
                key(out - sound.fade_out, sound.gain_db);
                key(out, kSilence);
            }
            layer.Set("levels", levels);
            main.layers.Push(std::move(layer));
            ++report_.layers;
        }
    }

    // A node, or a leaf wrapped by `wrapper` whose animations it carries.
    bool Emit(const MotionNode& node, const std::string& path, Comp& comp,
              const std::string& parent, double local_x, double local_y,
              int64_t ref, const MotionNode* wrapper,
              const MotionEvaluatedNode* wrapper_ev) {
        // A sequence is looked at where it begins; anything else where its
        // parent is.
        const int64_t at = node.type == MotionNodeType::Sequence
                               ? comp.t0 + node.from
                               : comp.t0 + ref;
        const MotionEvaluatedNode* found = At(at, path);
        if (found == nullptr) {
            // Hidden (visible: false) or outside every window at this
            // instant: nothing to place -- unless the evaluation failed.
            return error_.code.empty();
        }
        const MotionEvaluatedNode& ev = *found;
        const MotionNode& animated = wrapper != nullptr ? *wrapper : node;
        const MotionEvaluatedNode& placed =
            wrapper_ev != nullptr ? *wrapper_ev : ev;

        switch (node.type) {
            case MotionNodeType::Sequence:
                return EmitSequence(node, ev, path, comp, parent, local_x,
                                    local_y, ref);
            case MotionNodeType::Text:
                EmitText(node, ev, animated, placed, path, comp, parent,
                         local_x, local_y, ref);
                return true;
            case MotionNodeType::Image:
                EmitImage(node, ev, animated, placed, path, comp, parent,
                          local_x, local_y, ref);
                return true;
            case MotionNodeType::Path:
                EmitPath(node, ev, animated, placed, path, comp, parent,
                         local_x, local_y, ref);
                return true;
            case MotionNodeType::Video:
            case MotionNodeType::Audio:
                EmitMedia(node, ev, animated, placed, path, comp, parent,
                          local_x, local_y, ref, 0, comp.duration);
                return true;
            case MotionNodeType::Fill:
            case MotionNodeType::Box:
                break;
            default:
                Note(path, "type de nœud non traduit");
                return true;
        }

        // A container.
        if (node.children.empty()) {
            if (ev.has_background) {
                EmitRect(node, ev, path, comp, parent, local_x, local_y, ref,
                         false);
            }
            return true;
        }
        // A group the author named and filled stays one in After: a
        // precomposition under that name, which the timeline folds.
        if (FoldsFilterIntoText(node, ev, path, at)) {
            return EmitWrapped(node, path, ev, comp, parent, local_x, local_y,
                               ref);
        }
        const bool group = NeedsGroup(node, ev) ||
                           (!node.id.empty() && node.children.size() >= 6);
        if (!group && !ev.has_background && node.children.size() == 1 &&
            node.children[0].animate.empty() &&
            (node.children[0].type == MotionNodeType::Text ||
             node.children[0].type == MotionNodeType::Image ||
             node.children[0].type == MotionNodeType::Path)) {
            // A wrapper around one leaf: the leaf carries its animations.
            return EmitWrapped(node, path, ev, comp, parent, local_x, local_y,
                               ref);
        }
        if (!group && node.animate.empty() && IsIdentity(ev)) {
            // A still container: its children stand in its place.
            return EmitChildren(node, path, ev, comp, parent, local_x, local_y,
                                ref);
        }
        Pivot pivot{ev.box.x, ev.box.y, ev.box.width, ev.box.height};
        // MA11 -- a group the layout moves follows its box's corner, read
        // from the engine's frames; its children live in its space and
        // move with it. A group the layout resizes and that turns or
        // scales turns about the centre of its reference box.
        const bool moves = FromFrames(node, path, comp, ev.box, ref);
        bool resized = false;
        bool turnedResized = false;
        if (moves) {
            for (const Segment& segment : layout_) {
                const int64_t last =
                    std::min(segment.to, comp.t0 + comp.duration - 1);
                for (int64_t frame = std::max(segment.from, comp.t0);
                     frame <= last; ++frame) {
                    const Sample* sample = SampleAt(frame, path);
                    if (sample == nullptr ||
                        (std::abs(sample->box.width - ev.box.width) <=
                             kLayoutEpsilon &&
                         std::abs(sample->box.height - ev.box.height) <=
                             kLayoutEpsilon)) {
                        continue;
                    }
                    resized = true;
                    turnedResized = turnedResized || sample->scale_x != 1.0 ||
                                    sample->scale_y != 1.0 ||
                                    sample->rotate != 0.0;
                }
            }
        }
        if (turnedResized) {
            Note(path,
                 "groupe que la mise en page redimensionne pendant qu'il "
                 "tourne ou change d'échelle : autour du centre de sa boîte "
                 "de référence");
        }
        const double halfX = ev.box.width / 2.0;
        const double halfY = ev.box.height / 2.0;
        const bool followX = !HasAnimation(node, "translateX");
        const bool followY = !HasAnimation(node, "translateY");
        const auto corner = [&](const Sample& sample, double& x, double& y) {
            x = sample.box.x + halfX + (followX ? sample.translate_x : 0.0);
            y = sample.box.y + halfY + (followY ? sample.translate_y : 0.0);
            return true;
        };
        // Its children's space: this group, wherever it goes.
        const std::string followPath = follow_path_;
        const MotionBox followRef = follow_ref_;
        const auto children = [&](Comp& into, const std::string& parentId,
                                  double x, double y) {
            follow_path_ = path;
            follow_ref_ = ev.box;
            const bool done =
                EmitChildren(node, path, ev, into, parentId, x, y, ref);
            follow_path_ = followPath;
            follow_ref_ = followRef;
            return done;
        };
        if (!group) {
            MotionNode rest;
            MotionEvaluatedNode restEv;
            const std::string taken =
                Prepared(node, ev, path, comp, parent, rest, restEv, moves);
            Value layer =
                BaseLayer("null", node.id.empty() ? "groupe" : Display(node.id),
                          taken, ev, path);
            const std::string id = layer.Find("id")->AsString();
            layer_comp_ = &comp;
            source_path_.clear();
            layout_keys_ = moves;
            Value transform =
                Transform(rest, restEv, path, pivot, pivot.width / 2.0,
                          pivot.height / 2.0, local_x, local_y, 1.0, 1.0, ref);
            layout_keys_ = false;
            if (moves) {
                LayoutPosition(transform, comp, path, local_x, local_y, corner);
            }
            layer.Set("transform", transform);
            comp.layers.Push(std::move(layer));
            return children(comp, id, pivot.x, pivot.y);
        }
        // A group After can only render as one: a precomposition, in the
        // same time as its parent. After crops a precomposition to its
        // frame and CSS does not crop a box to its size (a box of absolute
        // children measures nothing): the group takes its parent's frame
        // grown over what it holds (MA13), and its own box only when it
        // clips (overflow: hidden).
        Comp inner;
        inner.id = NextComp();
        inner.name = GroupName(node, ev);
        if (ev.overflow_hidden) {
            // MA11 -- a box the layout resizes: the frame, which moves with
            // its corner, holds every size it takes, and a mask keyed on the
            // box does the clipping.
            MotionBox bounds = ev.box;
            if (resized) {
                for (const Segment& segment : layout_) {
                    const int64_t last =
                        std::min(segment.to, comp.t0 + comp.duration - 1);
                    for (int64_t frame = std::max(segment.from, comp.t0);
                         frame <= last; ++frame) {
                        if (const Sample* sample = SampleAt(frame, path)) {
                            bounds.width =
                                std::max(bounds.width, sample->box.width);
                            bounds.height =
                                std::max(bounds.height, sample->box.height);
                        }
                    }
                }
            }
            // MA7 -- a box that clips was given its own frame whatever its
            // size, and After refuses a composition over 30 000 px: the
            // build stopped there.
            if (LimitToAfter(comp, bounds)) {
                Note(path,
                     "groupe plus grand que ce qu'After accepte : cadre "
                     "limité à 30 000 px, le reste est rogné");
            }
            inner.origin_x = std::floor(bounds.x);
            inner.origin_y = std::floor(bounds.y);
            inner.width = static_cast<int32_t>(
                std::ceil(bounds.x + bounds.width) - inner.origin_x);
            inner.height = static_cast<int32_t>(
                std::ceil(bounds.y + bounds.height) - inner.origin_y);
        } else {
            // MA13 -- docs/roadmap/MOTION-AFTER-2026-09.md. Its parent's
            // frame, grown over every box it holds that reaches past it: a
            // move of the group (a timeline longer than the screen,
            // sliding) brings into view what CSS never cropped.
            MotionBox bounds{comp.origin_x, comp.origin_y,
                             static_cast<double>(comp.width),
                             static_cast<double>(comp.height)};
            if (GrowOverHeld(ev, comp, bounds)) {
                Note(path,
                     "groupe plus grand que ce qu'After accepte : cadre "
                     "limité à 30 000 px, le reste est rogné");
            }
            inner.origin_x = std::floor(bounds.x);
            inner.origin_y = std::floor(bounds.y);
            inner.width = static_cast<int32_t>(
                std::ceil(bounds.x + bounds.width) - inner.origin_x);
            inner.height = static_cast<int32_t>(
                std::ceil(bounds.y + bounds.height) - inner.origin_y);
        }
        inner.duration = comp.duration;
        inner.t0 = comp.t0;
        if (!children(inner, "", inner.origin_x, inner.origin_y)) {
            return false;
        }
        MotionNode rest;
        MotionEvaluatedNode restEv;
        const std::string taken =
            Prepared(node, ev, path, comp, parent, rest, restEv, moves);
        Value layer = BaseLayer("precomp", inner.name, taken, ev, path);
        layer.Set("comp", Value::MakeString(inner.id));
        // Turning about the node's box centre, inside the rounded comp.
        layer_comp_ = &comp;
        source_path_.clear();
        layout_keys_ = moves;
        Value transform = Transform(
            rest, restEv, path, pivot, pivot.CenterX() - inner.origin_x,
            pivot.CenterY() - inner.origin_y, local_x, local_y, 1.0, 1.0, ref);
        layout_keys_ = false;
        if (moves) {
            LayoutPosition(transform, comp, path, local_x, local_y, corner);
        }
        layer.Set("transform", transform);
        Value effects = Effects(node, ev, path);
        if (!effects.AsArray().empty()) {
            layer.Set("effects", effects);
        }
        // MA11 -- its clip-path, and the box it hides the overflow of when
        // the frame does not say it: a box that moves, or rounds its
        // corners.
        const double originX = inner.origin_x;
        const double originY = inner.origin_y;
        const MotionBox reference = ev.box;
        const Value masks =
            Masks(node, ev, path, comp, resized,
                  ev.overflow_hidden && (resized || ev.border_radius > 0.0),
                  [&](const Sample& sample, double x, double y) {
                      return std::make_pair(
                          x - (sample.box.x - reference.x) - originX,
                          y - (sample.box.y - reference.y) - originY);
                  });
        if (masks.IsArray()) {
            layer.Set("masks", masks);
        }
        Finish(inner);
        comp.layers.Push(std::move(layer));
        return true;
    }

    // After's largest composition, 30 000 px a side, centred on `comp`:
    // `bounds` cut to it, true when that cut something.
    static bool LimitToAfter(const Comp& comp, MotionBox& bounds) {
        constexpr double kAfterLargest = 30000.0;
        const double spareX = (kAfterLargest - comp.width) / 2.0;
        const double spareY = (kAfterLargest - comp.height) / 2.0;
        double left = bounds.x;
        double top = bounds.y;
        double right = bounds.x + bounds.width;
        double bottom = bounds.y + bounds.height;
        const bool cut = left < comp.origin_x - spareX ||
                         right > comp.origin_x + comp.width + spareX ||
                         top < comp.origin_y - spareY ||
                         bottom > comp.origin_y + comp.height + spareY;
        left = std::max(left, comp.origin_x - spareX);
        right = std::min(right, comp.origin_x + comp.width + spareX);
        top = std::max(top, comp.origin_y - spareY);
        bottom = std::min(bottom, comp.origin_y + comp.height + spareY);
        bounds = {left, top, std::max(0.0, right - left),
                  std::max(0.0, bottom - top)};
        return cut;
    }

    // A frame grown over the boxes a group holds, down to those that hide
    // their overflow, within After's largest composition around `comp`;
    // true when that limit cut it.
    static bool GrowOverHeld(const MotionEvaluatedNode& ev, const Comp& comp,
                             MotionBox& bounds) {
        double left = bounds.x;
        double top = bounds.y;
        double right = bounds.x + bounds.width;
        double bottom = bounds.y + bounds.height;
        const std::function<void(const MotionEvaluatedNode&)> grow =
            [&](const MotionEvaluatedNode& at) {
                for (const MotionEvaluatedNode& child : at.children) {
                    if (child.box.width > 0.0 && child.box.height > 0.0) {
                        left = std::min(left, child.box.x);
                        top = std::min(top, child.box.y);
                        right = std::max(right, child.box.x + child.box.width);
                        bottom =
                            std::max(bottom, child.box.y + child.box.height);
                    }
                    if (!child.overflow_hidden) {
                        grow(child);
                    }
                }
            };
        grow(ev);
        bounds = {left, top, right - left, bottom - top};
        return LimitToAfter(comp, bounds);
    }

    // A group is named by its biggest text: the word the scene is about.
    static std::string GroupName(const MotionNode& node,
                                 const MotionEvaluatedNode& ev) {
        return MotionGroupName(node.id, &ev);
    }

    bool EmitSequence(const MotionNode& node, const MotionEvaluatedNode& ev,
                      const std::string& path, Comp& comp,
                      const std::string& parent, double local_x, double local_y,
                      int64_t ref) {
        (void)ref;
        if (node.time_scale.has_value()) {
            Note(path, "timeScale : non traduit, la séquence joue à vitesse 1");
        }
        const int64_t duration =
            node.duration.value_or(comp.duration - node.from);
        if (!node.time_scale.has_value() && OnlySounds(node)) {
            return EmitSounds(node, path, comp, node.from,
                              std::min(comp.duration, node.from + duration));
        }
        Comp inner;
        inner.id = NextComp();
        // The scenes of the film are numbered; a sequence inside one is not.
        std::string number;
        if (comp.t0 == 0 && comp.origin_x == 0.0 && comp.origin_y == 0.0 &&
            comp.duration == composition_.duration) {
            number = MotionSceneNumber(++scene_count_);
        }
        inner.name = number + GroupName(node, ev);
        // MA7 -- within After's largest composition, as a group's.
        MotionBox frame = ev.box;
        if (LimitToAfter(comp, frame)) {
            Note(path,
                 "séquence plus grande que ce qu'After accepte : cadre "
                 "limité à 30 000 px, le reste est rogné");
        }
        inner.origin_x = std::floor(frame.x);
        inner.origin_y = std::floor(frame.y);
        inner.width = static_cast<int32_t>(std::ceil(frame.x + frame.width) -
                                           inner.origin_x);
        inner.height = static_cast<int32_t>(std::ceil(frame.y + frame.height) -
                                            inner.origin_y);
        inner.duration = duration;
        inner.t0 = comp.t0 + node.from;
        const std::string followPath = follow_path_;
        const MotionBox followRef = follow_ref_;
        follow_path_ = path;
        follow_ref_ = ev.box;
        const bool done = EmitChildren(node, path, ev, inner, "",
                                       inner.origin_x, inner.origin_y, 0);
        follow_path_ = followPath;
        follow_ref_ = followRef;
        if (!done) {
            return false;
        }
        Pivot pivot{ev.box.x, ev.box.y, ev.box.width, ev.box.height};
        Value layer = BaseLayer("precomp", inner.name, parent, ev, path);
        layer.Set("comp", Value::MakeString(inner.id));
        layer.Set("start", Value::MakeInt(node.from));
        layer.Set("in", Value::MakeInt(node.from));
        layer.Set("out", Value::MakeInt(node.from + duration));
        // MA8 -- the evaluator runs a sequence's own animations in its local
        // time (it subtracts `from` first), while its layer sits in the
        // parent's time, where After keys a layer: its keys move by `from`,
        // and its static values are read at its first frame. Written at
        // `from` unchanged, a scene's exit fell before the scene began.
        key_offset_ = node.from;
        layer_comp_ = &comp;
        source_path_.clear();
        Value transform = Transform(
            node, ev, path, pivot, pivot.CenterX() - inner.origin_x,
            pivot.CenterY() - inner.origin_y, local_x, local_y, 1.0, 1.0, 0);
        key_offset_ = 0;
        layer.Set("transform", std::move(transform));
        Finish(inner);
        comp.layers.Push(std::move(layer));
        return true;
    }

    static std::string StyleString(const MotionNode& node,
                                   const std::string& property) {
        for (const auto& entry : node.style) {
            if (entry.first == property &&
                entry.second.kind == MotionBindingKind::Literal &&
                entry.second.literal.IsString()) {
                return entry.second.literal.AsString();
            }
        }
        return {};
    }

    // The expression that writes a timecode on After's source text, frames
    // counted like the engine's (MotionComposition.h, Timecode). MA7 -- the
    // rounding to the nearest frame was suspected (19 October) of reading
    // the next frame's timecode at the late instants of After's motion blur,
    // where the engine keeps the frame's. After takes a text's source at the
    // frame's instant, as it does a fade: a timecode moving under a 360°
    // shutter showed its own frame's digits on all twenty frames compared,
    // no ghost of the next (After 26, 14 November 2026).
    std::string TimecodeExpression(int64_t offset) const {
        const int64_t nominal =
            (composition_.fps_numerator + composition_.fps_denominator - 1) /
            composition_.fps_denominator;
        return "var f = Math.floor(time / thisComp.frameDuration + 0.5) + " +
               std::to_string(offset) +
               ";\nif (f < 0) f = 0;\nvar n = " + std::to_string(nominal) +
               ";\nfunction p(x) { return (x < 10 ? \"0\" : \"\") + x; }\n"
               "var s = Math.floor(f / n);\n"
               "p(Math.floor(s / 3600)) + \":\" + p(Math.floor(s / 60) % 60) + "
               "\":\" + p(s % 60) + \":\" + p(f % n)";
    }

    void EmitText(const MotionNode& node, const MotionEvaluatedNode& ev,
                  const MotionNode& animated, const MotionEvaluatedNode& placed,
                  const std::string& path, Comp& comp,
                  const std::string& parent, double local_x, double local_y,
                  int64_t ref) {
        if (ev.text_layout.lines.empty()) {
            return;
        }
        std::string font;
        MotionError fontError;
        if (!measurer_.FontPostScriptName(ev.text_style, font, fontError)) {
            Note(path, "police sans nom PostScript : " + fontError.message);
        }
        // MOTION-WEB-2026-10 MW7 -- ROADMAP.md. A text in an instance of a
        // variable font also gives its family, its weight on the wght axis
        // and its slope: motion_import.jsx asks After for the instance at
        // that weight (FontObject.postScriptNameForDesignVector, measured in
        // After 26 on 14 November 2026), named or not. The PostScript name
        // Core Text gives stays for a family After does not list.
        int variableWeight = 0;
        bool namedInstance = false;
        const bool variable =
            !font.empty() && measurer_.FontVariableWeight(
                                 ev.text_style, variableWeight, namedInstance);
        // MM2 -- After applies no OpenType feature to a text layer (its
        // scripting has no figure style, After 26): figures the variant
        // changes would show in the font's default ones there.
        if (measurer_.NumericVariantChanges(ev.text, ev.text_style)) {
            Note(path,
                 "fontVariantNumeric : After n'applique pas ces chiffres, il "
                 "montre ceux de la police par défaut (une police dont ce "
                 "sont les chiffres par défaut les y garde)");
        }
        std::string content;
        for (size_t index = 0; index < ev.text_layout.lines.size(); ++index) {
            content +=
                (index > 0 ? "\r" : "") + ev.text_layout.lines[index].text;
        }
        const bool wrapped = &animated != &node;
        Value layer = BaseLayer("text", ev.text, parent, placed, path);
        SetFlow(layer, path, node, placed);
        Value text = Value::MakeObject();
        text.Set("content", Value::MakeString(content));
        text.Set("font", Value::MakeString(font));
        if (variable) {
            Value axes = Value::MakeObject();
            axes.Set("family", Value::MakeString(ev.text_style.font_family));
            axes.Set("wght", Value::MakeInt(variableWeight));
            axes.Set("italic", Value::MakeBool(ev.text_style.italic));
            text.Set("variable", axes);
        }
        text.Set("size", Num(ev.text_style.font_size));
        text.Set("color", Rgb(ev.text_style.color));
        const double em = std::max(1e-6, ev.text_style.font_size);
        // After's tracking is in thousandths of an em, and a whole number:
        // After refuses 133.33 and the build stopped half way (MA9). The
        // rounding moves each letter by at most half a thousandth of an em.
        const double tracking = ev.text_style.letter_spacing / em * 1000.0;
        const double whole = std::round(tracking);
        if (std::fabs(whole - tracking) > 1e-6) {
            Note(path, "letterSpacing : approche arrondie à " +
                           std::to_string(static_cast<int64_t>(whole)) +
                           " millièmes de cadratin (After n'accepte qu'un "
                           "entier)");
        }
        text.Set("tracking", Num(whole));
        const MotionTextLine& first = ev.text_layout.lines.front();
        if (ev.text_layout.lines.size() > 1) {
            text.Set("leading", Num(ev.text_layout.lines[1].y - first.y));
        }
        // Centred by its own alignment or by the box that holds it alone:
        // After justifies it at the centre, so an edited word stays there.
        const std::string& align = ev.text_style.text_align;
        const bool centered =
            align == "center" ||
            (wrapped && animated.children.size() == 1 &&
             StyleString(animated, "justifyContent") == "center" &&
             StyleString(animated, "flexDirection") != "column");
        const bool right = !centered && align == "right";
        text.Set("justify", Value::MakeString(centered ? "center"
                                              : right  ? "right"
                                                       : "left"));
        // Point text sits on its first baseline, at the line's start, middle
        // or end as it is justified.
        const double originX = ev.box.x + first.x +
                               (centered ? first.width / 2.0
                                : right  ? first.width
                                         : 0.0);
        const double originY = ev.box.y + first.baseline;
        Pivot pivot{placed.box.x, placed.box.y, placed.box.width,
                    placed.box.height};

        if (node.content.has_value() &&
            node.content->kind == MotionBindingKind::Timecode) {
            const int64_t offset =
                node.content->timecode_offset +
                (node.content->timecode_scope == "composition" ? comp.t0 : 0);
            text.Set("expression",
                     Value::MakeString(TimecodeExpression(offset)));
        }
        if (const MotionAnimation* spacing = Animation(node, "letterSpacing")) {
            // An animator's tracking counts pixels (measured on After 26:
            // 1 000 widens each gap by 1 000 px), the paragraph's thousandths
            // of an em: the animation goes to the animator, in pixels.
            text.Set("tracking", Num(0.0));
            text.Set("tracking_keys",
                     Keys(*spacing, [&](double v) -> std::vector<double> {
                         return {v};
                     }));
        }

        // Animations by unit: one slider per property carries the node's
        // keyframes, and an animator's expression selector reads it at each
        // unit's delay -- the engine's cascade, said the way After says it.
        static const char* unitProps[] = {"opacity", "translateX", "translateY",
                                          "scale",   "scaleX",     "scaleY",
                                          "rotate"};
        const auto isUnitProp = [&](const std::string& name) {
            return std::find(std::begin(unitProps), std::end(unitProps),
                             name) != std::end(unitProps);
        };
        MotionNode carrier = animated;
        MotionEvaluatedNode still = placed;
        if (node.units != MotionTextUnit::None) {
            Value units = Value::MakeObject();
            units.Set(
                "based_on",
                Value::MakeString(node.units == MotionTextUnit::Word ? "words"
                                  : node.units == MotionTextUnit::Line
                                      ? "lines"
                                      : "characters_excluding_spaces"));
            if (node.stagger.order == MotionStaggerOrder::Forward) {
                units.Set("order", Value::MakeString("forward"));
            } else if (node.stagger.order == MotionStaggerOrder::Reverse) {
                units.Set("order", Value::MakeString("reverse"));
            } else {
                // A seeded permutation: each unit's delay, as evaluated.
                units.Set("order", Value::MakeString("list"));
                Value delays = Value::MakeArray();
                for (const MotionTextUnitState& state : ev.units) {
                    delays.Push(Value::MakeInt(state.delay));
                }
                units.Set("delays", delays);
            }
            units.Set(
                "each",
                Value::MakeInt(node.stagger.present ? node.stagger.each : 0));
            // Units turn about the middle of their line box.
            units.Set("anchor_y",
                      Num(first.y + first.height / 2.0 - first.baseline));
            Value animators = Value::MakeArray();
            for (const auto& entry : node.animate) {
                if (!isUnitProp(entry.first)) {
                    continue;
                }
                const MotionAnimation& animation = entry.second;
                if (animation.kind == MotionAnimationKind::Spring) {
                    Note(path,
                         entry.first + " par unité en spring : non traduit");
                    continue;
                }
                Value animator = Value::MakeObject();
                double factor = 1.0;
                std::string property;
                std::string slider;
                double reference = 0.0;
                double neutral = 0.0;
                if (entry.first == "opacity") {
                    property = "opacity", slider = "opacité (unités)";
                    factor = 100.0, reference = 0.0, neutral = 100.0;
                } else if (entry.first == "translateX") {
                    property = "position_x", slider = "décalage x (unités)";
                    reference = 1000.0;
                } else if (entry.first == "translateY") {
                    property = "position_y", slider = "décalage y (unités)";
                    reference = 1000.0;
                } else if (entry.first == "rotate") {
                    property = "rotation", slider = "rotation (unités)";
                    reference = 360.0;
                } else {
                    property = entry.first == "scale"    ? "scale"
                               : entry.first == "scaleX" ? "scale_x"
                                                         : "scale_y";
                    slider = "échelle (unités)";
                    factor = 100.0, reference = 200.0, neutral = 100.0;
                }
                animator.Set("property", Value::MakeString(property));
                animator.Set("slider", Value::MakeString(slider));
                animator.Set("reference", Num(reference));
                animator.Set("neutral", Num(neutral));
                animator.Set("keys", Keys(animation,
                                          [&](double v) -> std::vector<double> {
                                              return {v * factor};
                                          }));
                animators.Push(std::move(animator));
            }
            units.Set("animators", animators);
            text.Set("units", units);
            // What the units carry leaves the layer.
            if (!wrapped) {
                carrier.animate.erase(
                    std::remove_if(carrier.animate.begin(),
                                   carrier.animate.end(),
                                   [&](const auto& entry) {
                                       return isUnitProp(entry.first);
                                   }),
                    carrier.animate.end());
                still.opacity = 1.0;
                still.translate_x = still.translate_y = 0.0;
                still.scale_x = still.scale_y = 1.0;
                still.rotate = 0.0;
            }
        }
        carrier.animate.erase(
            std::remove_if(carrier.animate.begin(), carrier.animate.end(),
                           [&](const auto& entry) {
                               return entry.first == "letterSpacing";
                           }),
            carrier.animate.end());
        // MA11 -- the colour it shows, its own or inherited, keyed when it
        // moves. A colour that cascades unit by unit stays as it is.
        int64_t colourOrigin = 0;
        if (const MotionAnimation* colour =
                InheritedColour(node, comp, colourOrigin)) {
            if (node.units != MotionTextUnit::None &&
                colour == Animation(node, "color")) {
                Note(path, "color par unité : non traduit");
            } else {
                Value colours;
                Value alpha;
                if (ColourKeys(*colour, path, "color", colourOrigin, colours,
                               alpha)) {
                    text.Set("color_keys", colours);
                    if (alpha.IsArray()) {
                        text.Set("alpha_keys", alpha);
                    }
                }
            }
        }
        layer.Set("text", text);
        // MA11 -- a text the layout moves (a neighbour that grows, a box
        // that resizes) takes its origin from the engine's frames.
        const auto originOf = [&](const Sample& sample) {
            return std::make_pair(sample.box.x + sample.line_x +
                                      (centered ? sample.line_width / 2.0
                                       : right  ? sample.line_width
                                                : 0.0),
                                  sample.box.y + sample.baseline);
        };
        const Moves moved = PointMoves(animated, path, comp, ref, false, ev.box,
                                       {originX, originY}, originOf);
        const bool moves = moved.moves && !moved.own_only;
        if (moves) {
            std::string lines;
            for (const MotionTextLine& line : ev.text_layout.lines) {
                lines += line.text + "\n";
            }
            for (const Segment& segment : layout_) {
                const int64_t last =
                    std::min(segment.to, comp.t0 + comp.duration - 1);
                bool recomposed = false;
                for (int64_t frame = std::max(segment.from, comp.t0);
                     frame <= last && !recomposed; ++frame) {
                    const Sample* sample = SampleAt(frame, path);
                    recomposed = sample != nullptr && sample->lines != lines;
                }
                if (recomposed) {
                    Note(path,
                         "texte que la mise en page recompose : lignes de "
                         "l'image de référence");
                    break;
                }
            }
        }
        MotionNode rest;
        MotionEvaluatedNode restEv;
        const std::string taken =
            Prepared(carrier, still, path, comp, parent, rest, restEv, moves);
        if (taken != parent) {
            layer.Set("parent", Value::MakeString(taken));
        }
        layer_comp_ = &comp;
        source_path_ = wrapped ? path.substr(0, path.rfind(".children[")) : "";
        layout_keys_ = moves;
        Value transform = Transform(
            rest, restEv, path, pivot, pivot.CenterX() - originX,
            pivot.CenterY() - originY, local_x, local_y, 1.0, 1.0, ref);
        layout_keys_ = false;
        source_path_.clear();
        if (centered && !HasAnimation(rest, "left") &&
            !HasAnimation(rest, "width")) {
            // CSS letter-spacing also trails the last letter, and a centred
            // line centres that trailing space too; After's does not: the
            // glyphs sit half a spacing further left in the engine.
            // A dimension that boils (MA4) keeps its jump in its expression.
            const Value* boiled = transform.Find("expressions");
            const bool boiledX =
                boiled != nullptr && boiled->Find("position.x") != nullptr;
            const bool boiledY =
                boiled != nullptr && boiled->Find("position.y") != nullptr;
            const double base = pivot.CenterX() - local_x +
                                (boiledX ? 0.0 : restEv.translate_x);
            const double y = pivot.CenterY() - local_y +
                             (boiledY ? 0.0 : restEv.translate_y);
            const MotionAnimation* spacing = Animation(node, "letterSpacing");
            Value keys = *transform.Find("keys");
            if (spacing != nullptr && keys.Find("position.x") == nullptr) {
                keys.Set("position.x",
                         Keys(*spacing, [&](double v) -> std::vector<double> {
                             return {base - v / 2.0};
                         }));
                transform.Set("keys", keys);
            }
            transform.Set(
                "position",
                Array({base - ev.text_style.letter_spacing / 2.0, y}));
        }
        if (moves) {
            const double anchorX = pivot.CenterX() - originX;
            const double anchorY = pivot.CenterY() - originY;
            const double trailing =
                centered ? ev.text_style.letter_spacing / 2.0 : 0.0;
            const bool withX = !HasAnimation(carrier, "translateX");
            const bool withY = !HasAnimation(carrier, "translateY");
            LayoutPosition(transform, comp, path, local_x, local_y,
                           [&](const Sample& sample, double& x, double& y) {
                               const auto at = originOf(sample);
                               x = at.first + anchorX - trailing +
                                   (withX ? still.translate_x : 0.0);
                               y = at.second + anchorY +
                                   (withY ? still.translate_y : 0.0);
                               return true;
                           });
        }
        layer.Set("transform", transform);
        Value effects = Effects(animated, placed, path);
        if (!effects.AsArray().empty()) {
            layer.Set("effects", effects);
        }
        const Value masks =
            Masks(node, ev, path, comp, moves, false,
                  [&](const Sample& sample, double x, double y) {
                      // The text's origin where its layer stands then.
                      const auto at = originOf(sample);
                      return std::make_pair(x - at.first, y - at.second);
                  });
        if (masks.IsArray()) {
            layer.Set("masks", masks);
        }
        if (node.highlights.empty()) {
            comp.layers.Push(std::move(layer));
        } else {
            EmitHighlights(node, ev, path, comp, layer, originX, originY,
                           centered, originOf);
        }
    }

    // MB3 -- a text and its highlighter are one painted object: applying
    // opacity or a clip to each separately would let the band show through
    // the glyphs. A precomposition carries their common transform, masks
    // and effects, with editable rectangles beneath the editable text.
    void EmitHighlights(
        const MotionNode& node, const MotionEvaluatedNode& ev,
        const std::string& path, Comp& comp, const Value& text_layer,
        double origin_x, double origin_y, bool centered,
        const std::function<std::pair<double, double>(const Sample&)>& origin) {
        std::vector<Segment> segments = layout_;
        bool animated = false;
        for (const MotionTextHighlight& highlight : node.highlights) {
            if (highlight.progress_animation.has_value()) {
                AddSegments(*highlight.progress_animation, comp.t0, segments);
                animated = true;
            }
        }
        // A band keeps its identity when progress has not reached it yet.
        // Layout may create another line, so gather all the bands first.
        using BandId = std::pair<size_t, size_t>;
        std::map<BandId, MotionHighlightBand> bands;
        const double padding = ev.text_style.font_size;
        double left = ev.box.x - origin_x - padding;
        double top = ev.box.y - origin_y - padding;
        double right = left + ev.box.width + 2.0 * padding;
        double bottom = top + ev.box.height + 2.0 * padding;
        const auto gather = [&](const Sample& sample) {
            const auto at = origin(sample);
            const double trailing =
                centered ? sample.letter_spacing / 2.0 : 0.0;
            if (sample.unit_bounds.width > 0.0 &&
                sample.unit_bounds.height > 0.0) {
                const double x = sample.box.x + sample.unit_bounds.x - at.first;
                const double y =
                    sample.box.y + sample.unit_bounds.y - at.second;
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x + sample.unit_bounds.width);
                bottom = std::max(bottom, y + sample.unit_bounds.height);
            }
            for (const MotionHighlightBand& band : sample.highlights) {
                bands.emplace(BandId{band.highlight, band.line}, band);
                const double x =
                    sample.box.x + band.box.x - at.first + trailing;
                const double y = sample.box.y + band.box.y - at.second;
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x + band.box.width);
                bottom = std::max(bottom, y + band.box.height);
            }
        };
        const Sample reference = SampleOf(ev);
        gather(reference);
        // Unit animators move glyphs inside this new precomposition, and
        // stagger can move its last unit after every source track ended.
        // Read the whole window rather than guessing the animator's reach.
        if (!ev.units.empty()) {
            for (int64_t frame = comp.t0; frame < comp.t0 + comp.duration;
                 ++frame) {
                if (const Sample* sample = SampleAt(frame, path)) {
                    gather(*sample);
                }
            }
        }
        for (const Segment& segment : segments) {
            const int64_t last =
                std::min(segment.to, comp.t0 + comp.duration - 1);
            for (int64_t frame = std::max(segment.from, comp.t0); frame <= last;
                 ++frame) {
                if (const Sample* sample = SampleAt(frame, path)) {
                    gather(*sample);
                }
            }
        }
        if (bands.empty()) {
            comp.layers.Push(text_layer);
            return;
        }
        left = std::floor(left);
        top = std::floor(top);
        Comp inner;
        inner.id = NextComp();
        inner.name = "Surligneur — " + Shorten(ev.text);
        inner.width = static_cast<int32_t>(
            std::clamp(std::ceil(right) - left, 4.0, 30000.0));
        inner.height = static_cast<int32_t>(
            std::clamp(std::ceil(bottom) - top, 4.0, 30000.0));
        inner.duration = comp.duration;
        inner.t0 = comp.t0;
        if (right - left > 30000.0 || bottom - top > 30000.0) {
            Note(path, "surligneur : cadre limité à 30 000 px par After");
        }
        const auto bandAt = [](const Sample& sample,
                               const BandId& id) -> const MotionHighlightBand* {
            for (const MotionHighlightBand& band : sample.highlights) {
                if (BandId{band.highlight, band.line} == id) {
                    return &band;
                }
            }
            return nullptr;
        };
        const auto still = [&](double x, double y) {
            Value transform = Value::MakeObject();
            transform.Set("anchor", Array({0.0, 0.0}));
            transform.Set("position", Array({x, y}));
            transform.Set("scale", Array({100.0, 100.0}));
            transform.Set("rotation", Num(0.0));
            transform.Set("opacity", Num(100.0));
            transform.Set("keys", Value::MakeObject());
            return transform;
        };
        for (const auto& entry : bands) {
            const BandId& id = entry.first;
            const MotionHighlightBand& band = entry.second;
            const auto read = [&](const Sample& sample) {
                const MotionHighlightBand* at = bandAt(sample, id);
                const auto point = origin(sample);
                return std::vector<double>{
                    at == nullptr ? 0.0 : at->painted,
                    at == nullptr ? 0.0 : at->box.height,
                    sample.box.x + (at == nullptr ? band.box.x : at->box.x) -
                        point.first - left +
                        (centered ? sample.letter_spacing / 2.0 : 0.0),
                    sample.box.y + (at == nullptr ? band.box.y : at->box.y) -
                        point.second - top};
            };
            const std::vector<double> initial = read(reference);
            Value rect = Value::MakeObject();
            rect.Set("size", Array({initial[0], initial[1]}));
            rect.Set("color", Rgb(band.color));
            rect.Set("alpha", Num(band.color.alpha));
            rect.Set("roundness", Num(0.0));
            // A left-anchored rectangle needs its centre to move by half
            // its painted width; the layer itself stays at the band's
            // corner. Both properties use the same evaluated widths.
            rect.Set("position", Array({initial[0] / 2.0, initial[1] / 2.0}));
            const Value sizes = DerivedKeys(
                segments, comp, path, "surligneur, taille",
                EaseModel::PerDimension, kLayoutEpsilon,
                [&](const Sample& sample, std::vector<double>& out) {
                    const auto value = read(sample);
                    out = {value[0], value[1]};
                    return true;
                });
            const Value centres = DerivedKeys(
                segments, comp, path, "surligneur, centre",
                EaseModel::PerDimension, kLayoutEpsilon,
                [&](const Sample& sample, std::vector<double>& out) {
                    const auto value = read(sample);
                    out = {value[0] / 2.0, value[1] / 2.0};
                    return true;
                });
            if (sizes.IsArray()) {
                rect.Set("size_keys", sizes);
            }
            if (centres.IsArray()) {
                rect.Set("position_keys", centres);
            }
            Value transform = still(initial[2], initial[3]);
            Value keys = Value::MakeObject();
            for (int axis = 0; axis < 2; ++axis) {
                const Value places = DerivedKeys(
                    segments, comp, path, "surligneur, position",
                    EaseModel::PerDimension, kLayoutEpsilon,
                    [&](const Sample& sample, std::vector<double>& out) {
                        out = {read(sample)[2 + axis]};
                        return true;
                    });
                if (places.IsArray()) {
                    keys.Set(axis == 0 ? "position.x" : "position.y", places);
                }
            }
            transform.Set("keys", keys);
            Value shape =
                BaseLayer("rect",
                          "Surligneur " + std::to_string(id.first + 1) +
                              ", ligne " + std::to_string(id.second + 1),
                          "", MotionEvaluatedNode{}, path);
            shape.Set("rect", rect);
            shape.Set("transform", transform);
            inner.layers.Push(std::move(shape));
        }
        // Its point-text origin is (0, 0) in the common source space.
        Value text =
            BaseLayer("text", ev.text, "", MotionEvaluatedNode{}, path);
        text.Set("text", *text_layer.Find("text"));
        text.Set("transform", still(-left, -top));
        inner.layers.Push(std::move(text));
        Value outer = Value::MakeObject();
        for (const auto& [key, value] : text_layer.AsObject()) {
            if (key != "text" && key != "masks") {
                outer.Set(key, value);
            }
        }
        outer.Set("kind", Value::MakeString("precomp"));
        outer.Set("comp", Value::MakeString(inner.id));
        Value transform = *text_layer.Find("transform");
        const auto& anchor = transform.Find("anchor")->AsArray();
        transform.Set("anchor", Array({std::stod(anchor[0].Dump()) - left,
                                       std::stod(anchor[1].Dump()) - top}));
        outer.Set("transform", transform);
        // Mask vertices are in the text's point space too; tangents are
        // vectors and stay unchanged when that space is translated.
        const auto shifted = [&](const Value& shape) {
            Value result = shape;
            Value vertices = Value::MakeArray();
            for (const Value& vertex : shape.Find("vertices")->AsArray()) {
                vertices.Push(
                    Array({std::stod(vertex.AsArray()[0].Dump()) - left,
                           std::stod(vertex.AsArray()[1].Dump()) - top}));
            }
            result.Set("vertices", vertices);
            return result;
        };
        if (const Value* list = text_layer.Find("masks")) {
            Value masks = Value::MakeArray();
            for (const Value& item : list->AsArray()) {
                Value mask = item;
                mask.Set("shape", shifted(*item.Find("shape")));
                if (const Value* track = item.Find("keys")) {
                    Value keys = Value::MakeArray();
                    for (const Value& key : track->AsArray()) {
                        Value moved = key;
                        moved.Set("v", shifted(*key.Find("v")));
                        keys.Push(std::move(moved));
                    }
                    mask.Set("keys", keys);
                }
                masks.Push(std::move(mask));
            }
            outer.Set("masks", masks);
        }
        if (animated && blur_) {
            Note(path,
                 "surligneur : After lit la progression à l'instant de "
                 "l'image, sans la moyenner sur l'obturateur");
        }
        Finish(inner);
        comp.layers.Push(std::move(outer));
    }

    std::string FootageFor(const std::string& source) {
        std::filesystem::path file(source);
        if (file.is_relative() && !options_.media_directory.empty()) {
            file = std::filesystem::path(options_.media_directory) / file;
        }
        const std::string key = file.lexically_normal().string();
        const auto found = footage_ids_.find(key);
        if (found != footage_ids_.end()) {
            return found->second;
        }
        const std::string id = "F" + std::to_string(footage_ids_.size() + 1);
        footage_ids_[key] = id;
        Value item = Value::MakeObject();
        item.Set("id", Value::MakeString(id));
        item.Set("path", Value::MakeString(key));
        footage_.Push(std::move(item));
        return id;
    }

    // How a picture of `width` x `height` fills `box`: objectFit's scales.
    static std::pair<double, double> Fit(const MotionBox& box,
                                         const std::string& object_fit,
                                         double width, double height) {
        double sx = box.width / width;
        double sy = box.height / height;
        if (object_fit == "contain" || object_fit == "cover") {
            const double fit =
                object_fit == "contain" ? std::min(sx, sy) : std::max(sx, sy);
            sx = sy = fit;
        }
        return {sx, sy};
    }

    // An image's or a video's picture of `width` x `height`: a footage layer
    // fitted to its box as objectFit asks, turning about the box's centre.
    // MA11 -- clipped to the box when it covers it, its clip-path a mask,
    // and its place, scale and masks read from the engine's frames when the
    // layout moves or resizes the box.
    Value Picture(const std::string& kind, const std::string& name,
                  const std::string& footage, double width, double height,
                  const MotionNode& node, const MotionEvaluatedNode& ev,
                  const MotionNode& animated, const MotionEvaluatedNode& placed,
                  const std::string& path, Comp& comp,
                  const std::string& parent, double local_x, double local_y,
                  int64_t ref) {
        const bool wrapped = &animated != &node;
        const bool moves = FromFrames(animated, path, comp, ev.box, ref);
        MotionNode rest = animated;
        MotionEvaluatedNode restEv = placed;
        const std::string taken = moves ? Prepared(animated, placed, path, comp,
                                                   parent, rest, restEv, true)
                                        : parent;
        Value layer = BaseLayer(kind, name, taken, placed, path);
        layer.Set("footage", Value::MakeString(footage));
        const std::pair<double, double> fitted =
            Fit(ev.box, ev.object_fit, width, height);
        const double sx = fitted.first;
        const double sy = fitted.second;
        const std::string fit = ev.object_fit;
        layer_comp_ = &comp;
        source_path_ = wrapped ? path.substr(0, path.rfind(".children[")) : "";
        Value transform;
        if (!moves) {
            // A footage layer turns about its own centre, which the box's
            // centre is: the image fills the box.
            Pivot pivot{placed.box.x, placed.box.y, placed.box.width,
                        placed.box.height};
            const double offsetX =
                (ev.box.x + ev.box.width / 2.0) - pivot.CenterX();
            const double offsetY =
                (ev.box.y + ev.box.height / 2.0) - pivot.CenterY();
            transform = Transform(
                animated, placed, path, pivot, width / 2.0 - offsetX / sx,
                height / 2.0 - offsetY / sy, local_x, local_y, sx, sy, ref);
        } else {
            if (wrapped && (HasAnimation(animated, "scale") ||
                            HasAnimation(animated, "scaleX") ||
                            HasAnimation(animated, "scaleY") ||
                            HasAnimation(animated, "rotate"))) {
                Note(path,
                     "image que la mise en page déplace, tournée ou mise à "
                     "l'échelle par sa boîte : autour du centre de l'image");
            }
            Pivot pivot{ev.box.x, ev.box.y, ev.box.width, ev.box.height};
            layout_keys_ = true;
            transform = Transform(rest, restEv, path, pivot, width / 2.0,
                                  height / 2.0, local_x, local_y, sx, sy, ref);
            layout_keys_ = false;
            const bool withX = !HasAnimation(animated, "translateX");
            const bool withY = !HasAnimation(animated, "translateY");
            LayoutPosition(transform, comp, path, local_x, local_y,
                           [&](const Sample& sample, double& x, double& y) {
                               x = sample.box.x + sample.box.width / 2.0 +
                                   (withX ? restEv.translate_x : 0.0);
                               y = sample.box.y + sample.box.height / 2.0 +
                                   (withY ? restEv.translate_y : 0.0);
                               return true;
                           });
            // The fit, and the node's own scale with it.
            std::vector<Segment> segments = layout_;
            for (const char* property : {"scale", "scaleX", "scaleY"}) {
                if (const MotionAnimation* track =
                        wrapped ? nullptr : Animation(node, property)) {
                    AddSegments(*track, comp.t0, segments);
                }
            }
            const double ownX = wrapped ? placed.scale_x : 1.0;
            const double ownY = wrapped ? placed.scale_y : 1.0;
            const Value scale = DerivedKeys(
                segments, comp, path, "échelle", EaseModel::PerDimension, 1e-3,
                [&](const Sample& sample, std::vector<double>& out) {
                    const auto f = Fit(sample.box, fit, width, height);
                    out = {
                        f.first * (wrapped ? ownX : sample.scale_x) * 100.0,
                        f.second * (wrapped ? ownY : sample.scale_y) * 100.0};
                    return true;
                });
            if (scale.IsArray()) {
                Value keys = *transform.Find("keys");
                keys.Set("scale", scale);
                transform.Set("keys", keys);
            }
        }
        source_path_.clear();
        layer.Set("transform", transform);
        Value effects = Effects(animated, placed, path);
        if (!effects.AsArray().empty()) {
            layer.Set("effects", effects);
        }
        // Where a composition point lands in the picture's own pixels.
        const MotionBox still = ev.box;
        const Value masks = Masks(
            node, ev, path, comp, moves,
            fit == "cover" &&
                (moves || std::abs(still.width / width -
                                   still.height / height) > 1e-9) &&
                !ClippedByParent(path, comp, ev),
            [&](const Sample& sample, double x, double y) {
                const MotionBox& box = sample.box;
                const auto f = moves ? Fit(box, fit, width, height)
                                     : std::make_pair(sx, sy);
                return std::make_pair(
                    width / 2.0 + (x - (box.x + box.width / 2.0)) / f.first,
                    height / 2.0 + (y - (box.y + box.height / 2.0)) / f.second);
            });
        if (masks.IsArray()) {
            layer.Set("masks", masks);
        }
        return layer;
    }

    // Whether the box around a node hides its overflow on the node's own
    // box, square, at every frame the layout moves them: the node's own
    // clip would then cut nothing more.
    bool ClippedByParent(const std::string& path, const Comp& comp,
                         const MotionEvaluatedNode& ev) {
        const size_t cut = path.rfind(".children[");
        if (cut == std::string::npos || ev.border_radius > 0.0) {
            return false;
        }
        const std::string parent = path.substr(0, cut);
        const auto same = [&](int64_t frame) {
            const Sample* own = SampleAt(frame, path);
            const Sample* around = SampleAt(frame, parent);
            if (own == nullptr || around == nullptr) {
                return own == nullptr;
            }
            return around->overflow_hidden && around->border_radius <= 0.0 &&
                   own->box.x <= around->box.x + kLayoutEpsilon &&
                   own->box.y <= around->box.y + kLayoutEpsilon &&
                   own->box.x + own->box.width >=
                       around->box.x + around->box.width - kLayoutEpsilon &&
                   own->box.y + own->box.height >=
                       around->box.y + around->box.height - kLayoutEpsilon;
        };
        if (!same(comp.t0)) {
            return false;
        }
        for (const Segment& segment : layout_) {
            const int64_t last =
                std::min(segment.to, comp.t0 + comp.duration - 1);
            for (int64_t frame = std::max(segment.from, comp.t0); frame <= last;
                 ++frame) {
                if (!same(frame)) {
                    return false;
                }
            }
        }
        return true;
    }

    void EmitImage(const MotionNode& node, const MotionEvaluatedNode& ev,
                   const MotionNode& animated,
                   const MotionEvaluatedNode& placed, const std::string& path,
                   Comp& comp, const std::string& parent, double local_x,
                   double local_y, int64_t ref) {
        double width = 0.0;
        double height = 0.0;
        MotionError sizeError;
        if (!measurer_.ImageSize(ev.image_source, width, height, sizeError) ||
            width <= 0.0 || height <= 0.0) {
            Note(path, "image illisible : " + sizeError.message);
            return;
        }
        Value layer = Picture(
            "image", std::filesystem::path(ev.image_source).filename().string(),
            FootageFor(ev.image_source), width, height, node, ev, animated,
            placed, path, comp, parent, local_x, local_y, ref);
        comp.layers.Push(std::move(layer));
    }

    // MA11 -- a video or an audio node: a footage layer, its picture placed
    // like an image's, its sound where the engine plays it -- from local
    // frame `offset` of `comp` (the node's 0), media frame startFrom there,
    // until `end`, endAt or the media's end -- at its volume, a slider the
    // levels read when it is animated (the engine's gain is linear, After's
    // levels are decibels: the slider keeps the engine's curve).
    void EmitMedia(const MotionNode& node, const MotionEvaluatedNode& ev,
                   const MotionNode& animated,
                   const MotionEvaluatedNode& placed, const std::string& path,
                   Comp& comp, const std::string& parent, double local_x,
                   double local_y, int64_t ref, int64_t offset, int64_t end) {
        const MotionMediaState& media = ev.media;
        std::string file = media.src;
        if (!media.media.empty()) {
            const auto found = options_.media_paths.find(media.media);
            if (found == options_.media_paths.end()) {
                Note(path, "média du projet introuvable : non traduit");
                return;
            }
            file = found->second;
        }
        MotionMediaInfo info;
        MotionError infoError;
        if (!measurer_.MediaInfo(media.src, media.media, info, infoError)) {
            Note(path, "média illisible : " + infoError.message);
            return;
        }
        // Its speed: a literal rate, or 1.
        double rate = 1.0;
        if (node.playback_rate.has_value()) {
            if (node.playback_rate->kind != MotionBindingKind::Literal ||
                !MotionNumber(node.playback_rate->literal, rate) ||
                !(rate > 0.0)) {
                Note(path, "playbackRate lié : vitesse 1");
                rate = 1.0;
            }
        }
        const bool video = node.type == MotionNodeType::Video &&
                           info.has_video && info.width > 0 && info.height > 0;
        const bool sound = info.has_audio && !node.muted && rate == 1.0;
        if (!video && !sound) {
            return;
        }
        const double fps = static_cast<double>(composition_.fps_numerator) /
                           composition_.fps_denominator;
        // Composition frames: the media's 0, and the stretch it plays.
        const double start =
            static_cast<double>(offset) - node.start_from / rate;
        double out = static_cast<double>(end);
        if (node.end_at.has_value()) {
            out = std::min(out, static_cast<double>(offset) +
                                    (*node.end_at - node.start_from) / rate);
        }
        if (info.duration.rate > 0) {
            const double mediaFrames =
                static_cast<double>(info.duration.value) / info.duration.rate *
                fps;
            out = std::min(out, start + mediaFrames / rate);
        }
        const std::string footage = FootageFor(file);
        const std::string name =
            node.id.empty() ? std::filesystem::path(file).filename().string()
                            : Display(node.id);
        Value layer;
        if (video) {
            layer = Picture("video", name, footage, info.width, info.height,
                            node, ev, animated, placed, path, comp, parent,
                            local_x, local_y, ref);
            layer.Set("start", Num(start));
            layer.Set("in", Value::MakeInt(offset));
            layer.Set("out", Num(out));
            if (rate != 1.0) {
                layer.Set("stretch", Num(100.0 / rate));
            }
            layer.Set("audio", Value::MakeBool(sound));
        } else {
            layer = Value::MakeObject();
            layer.Set("id", Value::MakeString(NextLayer()));
            layer.Set("kind", Value::MakeString("audio"));
            layer.Set("name", Value::MakeString(Shorten(name)));
            layer.Set("path", Value::MakeString(path));
            layer.Set("footage", Value::MakeString(footage));
            layer.Set("start_s", Num(start / fps));
            layer.Set("in_s", Num(offset / fps));
            layer.Set("out_s", Num(out / fps));
            layer.Set("levels", Value::MakeArray());
            ++report_.layers;
        }
        if (sound) {
            const MotionAnimation* volume = Animation(node, "volume");
            if (volume != nullptr &&
                volume->kind == MotionAnimationKind::Spring) {
                Note(path,
                     "volume en spring : non traduit, volume de l'image "
                     "de référence");
                volume = nullptr;
            }
            if (volume != nullptr) {
                const int64_t saved = key_offset_;
                key_offset_ = offset;
                layer.Set("volume_keys",
                          Keys(*volume, [](double v) -> std::vector<double> {
                              return {std::clamp(v, 0.0, 4.0)};
                          }));
                key_offset_ = saved;
                layer.Set("gain_db", Num(0.0));
            } else {
                layer.Set("gain_db", Num(media.volume > 0.0
                                             ? 20.0 * std::log10(media.volume)
                                             : -192.0));
            }
        }
        comp.layers.Push(std::move(layer));
    }

    // MA11 -- a sequence that only plays sounds (a music, effects) is no
    // scene: its sounds go straight into the composition it plays in, from
    // its start (`offset`, in that composition's frames) to its end.
    static bool OnlySounds(const MotionNode& node) {
        if (node.children.empty()) {
            return false;
        }
        for (const MotionNode& child : node.children) {
            const bool sounds =
                child.type == MotionNodeType::Audio ||
                (child.type == MotionNodeType::Sequence &&
                 !child.time_scale.has_value() && OnlySounds(child));
            if (!sounds) {
                return false;
            }
        }
        return true;
    }

    bool EmitSounds(const MotionNode& node, const std::string& path, Comp& comp,
                    int64_t offset, int64_t end) {
        for (size_t index = 0; index < node.children.size(); ++index) {
            const MotionNode& child = node.children[index];
            const std::string at =
                path + ".children[" + std::to_string(index) + "]";
            if (child.type == MotionNodeType::Sequence) {
                const int64_t from = offset + child.from;
                const int64_t until =
                    child.duration.has_value()
                        ? std::min(end, from + *child.duration)
                        : end;
                if (!EmitSounds(child, at, comp, from, until)) {
                    return false;
                }
                continue;
            }
            const MotionEvaluatedNode* ev = At(comp.t0 + offset, at);
            if (ev == nullptr) {
                if (!error_.code.empty()) {
                    return false;
                }
                continue;
            }
            EmitMedia(child, *ev, child, *ev, at, comp, "", 0.0, 0.0, offset,
                      offset, end);
        }
        return true;
    }

    void EmitPath(const MotionNode& node, const MotionEvaluatedNode& ev,
                  const MotionNode& animated, const MotionEvaluatedNode& placed,
                  const std::string& path, Comp& comp,
                  const std::string& parent, double local_x, double local_y,
                  int64_t ref) {
        if (HasAnimation(node, "d")) {
            Note(path,
                 "morphing de tracé : non traduit, tracé de l'image de "
                 "référence");
        }
        // The engine's mapping of the viewBox (xMidYMid meet).
        double scale = 1.0;
        double dx = ev.box.x;
        double dy = ev.box.y;
        if (node.view_box.has_value()) {
            const MotionViewBox& view = *node.view_box;
            scale = std::min(ev.box.width / view.width,
                             ev.box.height / view.height);
            dx = ev.box.x + (ev.box.width - view.width * scale) / 2.0 -
                 view.x * scale;
            dy = ev.box.y + (ev.box.height - view.height * scale) / 2.0 -
                 view.y * scale;
        }
        // Vertices in the layer's space: composition pixels less the box's
        // corner.
        const auto point = [&](double x, double y) {
            return std::pair<double, double>(x * scale + dx - ev.box.x,
                                             y * scale + dy - ev.box.y);
        };
        Value shapes = Value::MakeArray();
        struct Sub {
            std::vector<std::pair<double, double>> vertices;
            std::vector<std::pair<double, double>> in;
            std::vector<std::pair<double, double>> out;
            bool closed = false;
        };
        std::vector<Sub> subs;
        for (const MotionPathSegment& segment : node.path) {
            switch (segment.verb) {
                case MotionPathVerb::Move: {
                    subs.emplace_back();
                    subs.back().vertices.push_back(point(segment.x, segment.y));
                    subs.back().in.emplace_back(0.0, 0.0);
                    subs.back().out.emplace_back(0.0, 0.0);
                    break;
                }
                case MotionPathVerb::Line:
                case MotionPathVerb::Cubic: {
                    if (subs.empty()) {
                        subs.emplace_back();
                        subs.back().vertices.push_back(point(0.0, 0.0));
                        subs.back().in.emplace_back(0.0, 0.0);
                        subs.back().out.emplace_back(0.0, 0.0);
                    }
                    Sub& sub = subs.back();
                    const auto end = point(segment.x, segment.y);
                    if (segment.verb == MotionPathVerb::Cubic) {
                        const auto c1 = point(segment.x1, segment.y1);
                        const auto c2 = point(segment.x2, segment.y2);
                        const auto& last = sub.vertices.back();
                        sub.out.back() = {c1.first - last.first,
                                          c1.second - last.second};
                        sub.vertices.push_back(end);
                        sub.in.emplace_back(c2.first - end.first,
                                            c2.second - end.second);
                    } else {
                        sub.vertices.push_back(end);
                        sub.in.emplace_back(0.0, 0.0);
                    }
                    sub.out.emplace_back(0.0, 0.0);
                    break;
                }
                case MotionPathVerb::Close: {
                    if (subs.empty()) {
                        break;
                    }
                    Sub& sub = subs.back();
                    sub.closed = true;
                    if (sub.vertices.size() > 1) {
                        const auto& a = sub.vertices.front();
                        const auto& b = sub.vertices.back();
                        if (std::abs(a.first - b.first) < 1e-6 &&
                            std::abs(a.second - b.second) < 1e-6) {
                            sub.in.front() = sub.in.back();
                            sub.vertices.pop_back();
                            sub.in.pop_back();
                            sub.out.pop_back();
                        }
                    }
                    break;
                }
            }
        }
        const auto pairs =
            [](const std::vector<std::pair<double, double>>& list) {
                Value out = Value::MakeArray();
                for (const auto& p : list) {
                    out.Push(Array({p.first, p.second}));
                }
                return out;
            };
        for (const Sub& sub : subs) {
            Value shape = Value::MakeObject();
            shape.Set("vertices", pairs(sub.vertices));
            shape.Set("in", pairs(sub.in));
            shape.Set("out", pairs(sub.out));
            shape.Set("closed", Value::MakeBool(sub.closed));
            shapes.Push(std::move(shape));
        }
        Value layer =
            BaseLayer("shape", node.id.empty() ? "tracé" : Display(node.id),
                      parent, placed, path);
        Value shape = Value::MakeObject();
        shape.Set("paths", shapes);
        const MotionPathPaint& paint = ev.path_paint;
        if (paint.has_stroke) {
            Value stroke = Value::MakeObject();
            stroke.Set("color", Rgb(paint.stroke));
            stroke.Set("alpha", Num(paint.stroke.alpha));
            stroke.Set("width", Num(paint.stroke_width));
            stroke.Set("cap", Value::MakeString(paint.linecap));
            stroke.Set("join", Value::MakeString(paint.linejoin));
            if (!paint.dash.empty()) {
                Note(path, "pointillés : non traduits");
            }
            Paint(node, "stroke", path, stroke);
            shape.Set("stroke", stroke);
        }
        if (paint.has_fill) {
            Value fill = Value::MakeObject();
            fill.Set("color", Rgb(paint.fill));
            fill.Set("alpha", Num(paint.fill.alpha));
            fill.Set("evenodd", Value::MakeBool(paint.fill_rule == "evenodd"));
            Paint(node, "fill", path, fill);
            shape.Set("fill", fill);
        }
        // Trim: static values, and keyframes where the node animates them.
        const MotionAnimation* trimStart = Animation(node, "trimStart");
        const MotionAnimation* trimEnd = Animation(node, "trimEnd");
        if (trimStart != nullptr || trimEnd != nullptr ||
            paint.trim_start > 0.0 || paint.trim_end < 1.0) {
            Value trim = Value::MakeObject();
            trim.Set("start", Num(paint.trim_start * 100.0));
            trim.Set("end", Num(paint.trim_end * 100.0));
            const auto percent = [](double v) -> std::vector<double> {
                return {std::clamp(v, 0.0, 1.0) * 100.0};
            };
            if (trimStart != nullptr) {
                trim.Set("start_keys", Keys(*trimStart, percent));
            }
            if (trimEnd != nullptr) {
                trim.Set("end_keys", Keys(*trimEnd, percent));
            }
            shape.Set("trim", trim);
        }
        layer.Set("shape", shape);
        Pivot pivot{placed.box.x, placed.box.y, placed.box.width,
                    placed.box.height};
        if (FromFrames(animated, path, comp, ev.box, ref)) {
            Note(path,
                 "tracé que la mise en page déplace ou redimensionne : "
                 "approché, sa géométrie reste celle de l'image de "
                 "référence");
        }
        layer_comp_ = &comp;
        source_path_ =
            &animated != &node ? path.substr(0, path.rfind(".children[")) : "";
        layer.Set("transform", Transform(animated, placed, path, pivot,
                                         pivot.CenterX() - ev.box.x,
                                         pivot.CenterY() - ev.box.y, local_x,
                                         local_y, 1.0, 1.0, ref));
        source_path_.clear();
        Value effects = Effects(animated, placed, path);
        if (!effects.AsArray().empty()) {
            layer.Set("effects", effects);
        }
        const Value masks =
            Masks(node, ev, path, comp, false, false,
                  [&](const Sample& sample, double x, double y) {
                      return std::make_pair(x - sample.box.x, y - sample.box.y);
                  });
        if (masks.IsArray()) {
            layer.Set("masks", masks);
        }
        comp.layers.Push(std::move(layer));
    }

    // MA11 -- a path's fill or stroke animated: its colour keys, and its
    // alpha's as opacity keys.
    void Paint(const MotionNode& node, const std::string& property,
               const std::string& path, Value& paint) {
        const MotionAnimation* animation = Animation(node, property);
        if (animation == nullptr) {
            return;
        }
        Value colours;
        Value alpha;
        if (ColourKeys(*animation, path, property, 0, colours, alpha)) {
            paint.Set("color_keys", colours);
            if (alpha.IsArray()) {
                paint.Set("alpha_keys", alpha);
            }
        }
    }

    const MotionComposition& composition_;
    const Value& props_;
    MotionMeasurer& measurer_;
    const MotionEvaluationContext& context_;
    const MotionAfterExportOptions& options_;
    MotionAfterExportReport& report_;
    MotionError& error_;
    std::map<int64_t, std::unique_ptr<FrameIndex>> frames_;
    // MA11 -- what the layout moves (every segment that may move a box) and
    // what each frame read for it showed, node by node.
    std::vector<Segment> layout_;
    std::map<int64_t, std::unique_ptr<std::unordered_map<std::string, Sample>>>
        samples_;
    // MA11 -- the nodes above the one being emitted, nearest last, each with
    // the frame (absolute) of its animations' 0: what a text inherits its
    // colour from.
    struct Ancestor {
        const MotionNode* node = nullptr;
        int64_t origin = 0;
    };
    std::vector<Ancestor> ancestors_;
    std::vector<Value> comps_;
    Value footage_ = Value::MakeArray();
    std::map<std::string, std::string> footage_ids_;
    int64_t comp_count_ = 0;
    // MW3 -- GSAP curves written as their closest cubic-bezier, by name.
    mutable std::map<std::string, double> approximated_curves_;
    int64_t scene_count_ = 0;
    // Frames added to every key written: a sequence's own keys, in its
    // local time, placed on its layer in the parent's time (MA8).
    int64_t key_offset_ = 0;
    int64_t layer_count_ = 0;
    double width_full_ = 0.0;
    // MA11 -- set by an emitter around its Transform: the layout's moves
    // are its keys (left, top and size leave the tracks), the composition
    // the layer lives in, and the node whose animations a wrapped leaf
    // carries, read from the engine where two tracks meet.
    // MA12 -- the composition has a shutter (Unculled).
    bool blur_ = false;
    bool layout_keys_ = false;
    const Comp* layer_comp_ = nullptr;
    std::string source_path_;
    // MA11 -- the group whose space the layers being emitted live in, and
    // its reference box: they move with it (Displacement).
    std::string follow_path_;
    MotionBox follow_ref_;
    bool slice_noted_ = false;
    size_t slice_note_ = 0;
    // MA3 -- for each text of a row, the texts whose change of width moves
    // it, by their node's path, and how much (PlanFlow); and whether its own
    // box is its words' width.
    struct Flow {
        std::vector<std::pair<std::string, double>> terms;
        bool sized = false;
    };
    std::map<std::string, Flow> flows_;
    bool extend_used_ = false;
    // MA4 -- the named nodes that did boil, and the seed of the next track.
    std::map<std::string, bool> boiled_;
    int64_t boil_seed_ = 0;
};

}  // namespace

void MotionAfterBezierEase(double x1, double y1, double x2, double y2,
                           double v0, double v1, double seconds,
                           MotionAfterEase& out, MotionAfterEase& in) {
    // On one dimension After's temporal bezier is the CSS curve itself: the
    // handles leave the keys at `influence` of the segment's duration with
    // `speed` as slope. A handle at x = 0 has no length; After's shortest
    // is 0.1 %.
    const double average = seconds > 0.0 ? (v1 - v0) / seconds : 0.0;
    const double outX = std::max(0.001, x1);
    const double inX = std::max(0.001, 1.0 - x2);
    out.influence = std::clamp(outX * 100.0, 0.1, 100.0);
    in.influence = std::clamp(inX * 100.0, 0.1, 100.0);
    out.speed = average * (y1 / outX);
    in.speed = average * ((1.0 - y2) / inX);
}

bool ExportMotionToAfter(const MotionComposition& composition,
                         const mcp_json::Value& props, MotionMeasurer& measurer,
                         const MotionEvaluationContext& context,
                         const MotionAfterExportOptions& options,
                         mcp_json::Value& description,
                         MotionAfterExportReport& report, MotionError& error) {
    report = {};
    error = {};
    if (composition.grain_amount > 0.0) {
        report.notes.push_back("composition: grain non traduit");
    }
    // MB11 -- the exporter writes keys as they are: bound ones first take
    // this instance's values.
    MotionComposition bound;
    if (!BindMotionInstance(composition, props, measurer, bound, error)) {
        return false;
    }
    // MA12 -- the shutter goes to After; the exporter reads frames, never
    // their shutter instants.
    const int32_t samples = bound.motion_blur_samples;
    const double angle = bound.motion_blur_angle;
    bound.motion_blur_samples = 0;
    Exporter exporter(bound, props, measurer, context, options, report, error);
    return exporter.Run(description, samples, angle);
}

bool MotionAfterTimelineAudio(const Document& document,
                              const std::string& directory,
                              std::vector<MotionAfterAudio>& audio,
                              MotionError& error) {
    const auto seconds = [](const RationalTime& time) {
        return static_cast<double>(time.value) / time.rate;
    };
    // Heard as playback and export mix them: a muted track is silent, and
    // once a track is soloed only soloed tracks play.
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo)) {
            continue;
        }
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip is not heard (ClipEnabled.h).
            if (!ClipIsHeard(clip)) {
                continue;
            }
            const DocumentSource* source = document.FindSource(clip.source_id);
            if (source == nullptr) {
                continue;
            }
            if (clip.speed.num != clip.speed.den) {
                error = {"Unsupported", clip.id,
                         "a sound played at another speed has no audio layer "
                         "equivalent here"};
                return false;
            }
            MotionAfterAudio sound;
            std::filesystem::path file(source->path);
            if (file.is_relative() && !directory.empty()) {
                file = std::filesystem::path(directory) / file;
            }
            sound.path = file.lexically_normal().string();
            sound.name = file.filename().string();
            sound.source_in = seconds(clip.source_in);
            sound.timeline_in = seconds(clip.timeline_in);
            sound.duration = seconds(clip.duration);
            sound.gain_db = static_cast<double>(clip.audio_gain_db.num) /
                            clip.audio_gain_db.den;
            sound.fade_in = seconds(clip.audio_fade_in);
            sound.fade_out = seconds(clip.audio_fade_out);
            audio.push_back(sound);
        }
    }
    return true;
}

void MotionAfterReportFields(const MotionAfterExportReport& report,
                             mcp_json::Value& result) {
    result.Set("comps", mcp_json::Value::MakeInt(report.comps));
    result.Set("layers", mcp_json::Value::MakeInt(report.layers));
    result.Set("keyframes", mcp_json::Value::MakeInt(report.keyframes));
    mcp_json::Value notes = mcp_json::Value::MakeArray();
    for (const std::string& note : report.notes) {
        notes.Push(mcp_json::Value::MakeString(note));
    }
    result.Set("notes", notes);
}
