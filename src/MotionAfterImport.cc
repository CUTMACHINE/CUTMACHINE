// MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md (DEC43). After Effects
// composition -> composition of the format. See MotionAfterImport.h for the
// contract; the rules below are the equivalences, each with its reason.
//
// Geometry. An After layer maps a point q of its own space to its parent's
// as P + R S (q - A): anchor A, position P, scale S, rotation R. A node of
// the format maps its box the same way about transformOrigin, after its
// translate: a box at (0, 0) of its parent, origin at A, translated by
// P - A, is the layer, exactly. Parenting nests one box per parent (without
// its opacity, which After does not hand down).
//
// Time. Keys stay keys: After's speed and influence on both sides of a
// segment are a cubic Bezier in (time, value), the format's cubic-bezier()
// for that segment (MB5). A hold is step-end. What After computes instead
// of interpolating (expressions, curved paths) arrives sampled, one value
// per frame, and becomes one key per frame where it changes.

#include "MotionAfterImport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <optional>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

// ---------------------------------------------------------------------------
// Reading the after-comp.v1 tree

const Value* Field(const Value* object, const std::string& key) {
    return object == nullptr ? nullptr : object->Find(key);
}

double Number(const Value* value, double fallback = 0.0) {
    double number = fallback;
    if (value != nullptr && MotionNumber(*value, number)) {
        return number;
    }
    return fallback;
}

std::string Text(const Value* value) {
    return value != nullptr && value->IsString() ? value->AsString()
                                                 : std::string();
}

bool Flag(const Value* value) {
    return value != nullptr && value->IsBool() && value->AsBool();
}

// A value read in After as numbers: a scalar is one, an array its items.
std::vector<double> Numbers(const Value* value) {
    std::vector<double> numbers;
    if (value == nullptr) {
        return numbers;
    }
    if (value->IsArray()) {
        for (const Value& item : value->AsArray()) {
            numbers.push_back(Number(&item));
        }
    } else if (value->IsNumber()) {
        numbers.push_back(Number(value));
    }
    return numbers;
}

struct AfterKey {
    double time = 0.0;  // seconds, composition time
    std::vector<double> value;
    bool hold_out = false;
    bool linear_in = false;
    bool linear_out = false;
    // (speed, influence %) per dimension, or one for a spatial property.
    std::vector<std::pair<double, double>> in_ease;
    std::vector<std::pair<double, double>> out_ease;
};

// One property of the reading: a constant, keys, or samples.
struct AfterProperty {
    bool present = false;
    std::string name;
    std::vector<double> value;
    std::vector<AfterKey> keys;
    std::string expression;
    int64_t samples_from = 0;
    std::vector<std::vector<double>> samples;
    bool spatial = false;

    bool Animated() const { return !keys.empty() || !samples.empty(); }
};

std::vector<std::pair<double, double>> Eases(const Value* list) {
    std::vector<std::pair<double, double>> eases;
    if (list == nullptr || !list->IsArray()) {
        return eases;
    }
    for (const Value& item : list->AsArray()) {
        const std::vector<double> pair = Numbers(&item);
        if (pair.size() == 2) {
            eases.emplace_back(pair[0], pair[1]);
        }
    }
    return eases;
}

AfterProperty ReadProperty(const Value* record, bool spatial = false) {
    AfterProperty property;
    if (record == nullptr || !record->IsObject()) {
        return property;
    }
    property.present = true;
    property.spatial = spatial;
    property.name = Text(record->Find("name"));
    property.value = Numbers(record->Find("value"));
    property.expression = Text(record->Find("expression"));
    if (const Value* keys = record->Find("keys");
        keys != nullptr && keys->IsArray()) {
        for (const Value& raw : keys->AsArray()) {
            AfterKey key;
            key.time = Number(raw.Find("t"));
            key.value = Numbers(raw.Find("v"));
            key.hold_out = Text(raw.Find("out_type")) == "hold";
            key.linear_in = Text(raw.Find("in_type")) == "linear";
            key.linear_out = Text(raw.Find("out_type")) == "linear";
            key.in_ease = Eases(raw.Find("in_ease"));
            key.out_ease = Eases(raw.Find("out_ease"));
            property.keys.push_back(std::move(key));
        }
    }
    if (const Value* samples = record->Find("samples");
        samples != nullptr && samples->IsArray()) {
        property.samples_from =
            static_cast<int64_t>(Number(record->Find("samples_from")));
        for (const Value& sample : samples->AsArray()) {
            property.samples.push_back(Numbers(&sample));
        }
    }
    return property;
}

const Value* Properties(const Value* group) {
    return Field(group, "properties");
}

AfterProperty GroupProperty(const Value* group, const std::string& match,
                            bool spatial = false) {
    return ReadProperty(Field(Properties(group), match), spatial);
}

std::vector<const Value*> Groups(const Value* group) {
    std::vector<const Value*> groups;
    const Value* list = Field(group, "groups");
    if (list != nullptr && list->IsArray()) {
        for (const Value& item : list->AsArray()) {
            groups.push_back(&item);
        }
    }
    return groups;
}

// ---------------------------------------------------------------------------
// Evaluating a property at a frame, as After does

struct Timebase {
    int64_t numerator = 25;
    int64_t denominator = 1;
    double Fps() const {
        return static_cast<double>(numerator) /
               static_cast<double>(denominator);
    }
};

// One dimension of a cubic Bezier in (time, value), solved for time by
// bisection: monotonic in time, since After keeps influence in (0, 100].
double BezierAt(double t0, double v0, double t1, double v1, double c1t,
                double c1v, double c2t, double c2v, double time) {
    double low = 0.0;
    double high = 1.0;
    double u = 0.5;
    for (int step = 0; step < 60; ++step) {
        u = (low + high) / 2.0;
        const double a = 1.0 - u;
        const double t = a * a * a * t0 + 3 * a * a * u * c1t +
                         3 * a * u * u * c2t + u * u * u * t1;
        if (t < time) {
            low = u;
        } else {
            high = u;
        }
    }
    const double a = 1.0 - u;
    return a * a * a * v0 + 3 * a * a * u * c1v + 3 * a * u * u * c2v +
           u * u * u * v1;
}

std::pair<double, double> EaseOf(
    const std::vector<std::pair<double, double>>& eases, size_t dimension) {
    if (eases.empty()) {
        return {0.0, 100.0 / 6.0};
    }
    return eases[std::min(dimension, eases.size() - 1)];
}

double Distance(const std::vector<double>& a, const std::vector<double>& b) {
    double sum = 0.0;
    for (size_t index = 0; index < std::min(a.size(), b.size()); ++index) {
        sum += (b[index] - a[index]) * (b[index] - a[index]);
    }
    return std::sqrt(sum);
}

// The segment's progress (0 to 1) at `time` for one dimension, or along the
// straight path of a spatial property.
double SegmentProgress(const AfterKey& from, const AfterKey& to,
                       size_t dimension, bool spatial, double time) {
    const double span = to.time - from.time;
    if (span <= 0.0) {
        return 1.0;
    }
    double delta = 0.0;
    if (spatial) {
        delta = Distance(from.value, to.value);
        dimension = 0;
    } else if (dimension < from.value.size() && dimension < to.value.size()) {
        delta = to.value[dimension] - from.value[dimension];
    }
    if (std::fabs(delta) < 1e-12) {
        return (time - from.time) / span;
    }
    const auto [outSpeed, outInfluence] = EaseOf(from.out_ease, dimension);
    const auto [inSpeed, inInfluence] = EaseOf(to.in_ease, dimension);
    const double x1 = outInfluence / 100.0;
    const double x2 = 1.0 - inInfluence / 100.0;
    const double y1 = outSpeed * span * x1 / delta;
    const double y2 = 1.0 - inSpeed * span * (inInfluence / 100.0) / delta;
    return BezierAt(0.0, 0.0, 1.0, 1.0, x1, y1, x2, y2,
                    (time - from.time) / span);
}

std::vector<double> ValueAt(const AfterProperty& property, int64_t frame,
                            const Timebase& timebase) {
    if (!property.samples.empty()) {
        const int64_t index = std::clamp<int64_t>(
            frame - property.samples_from, 0,
            static_cast<int64_t>(property.samples.size()) - 1);
        return property.samples[static_cast<size_t>(index)];
    }
    if (property.keys.empty()) {
        return property.value;
    }
    const double time = static_cast<double>(frame) / timebase.Fps();
    if (time <= property.keys.front().time) {
        return property.keys.front().value;
    }
    if (time >= property.keys.back().time) {
        return property.keys.back().value;
    }
    size_t index = 0;
    while (index + 1 < property.keys.size() &&
           property.keys[index + 1].time <= time) {
        ++index;
    }
    const AfterKey& from = property.keys[index];
    const AfterKey& to = property.keys[index + 1];
    if (from.hold_out) {
        return from.value;
    }
    std::vector<double> value = from.value;
    for (size_t dimension = 0;
         dimension < value.size() && dimension < to.value.size(); ++dimension) {
        const double progress =
            SegmentProgress(from, to, dimension, property.spatial, time);
        value[dimension] =
            from.value[dimension] +
            (to.value[dimension] - from.value[dimension]) * progress;
    }
    return value;
}

double Component(const std::vector<double>& value, size_t index,
                 double fallback = 0.0) {
    return index < value.size() ? value[index] : fallback;
}

// ---------------------------------------------------------------------------
// Writing the format

std::string Decimal(double number, int places = 3) {
    if (!std::isfinite(number)) {
        number = 0.0;
    }
    const double scale = std::pow(10.0, places);
    number = std::round(number * scale) / scale;
    if (number == 0.0) {
        number = 0.0;  // no "-0"
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", places, number);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text == "-0" ? "0" : text;
}

Value Num(double number, int places = 3) {
    return Value::MakeRawNumber(Decimal(number, places));
}

std::string Px(double number) { return Decimal(number) + "px"; }

int64_t Gcd(int64_t a, int64_t b) {
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        const int64_t r = a % b;
        a = b;
        b = r;
    }
    return a == 0 ? 1 : a;
}

// A frame as the format writes it: an integer, or a reduced "n/d" when a key
// falls between two frames. After keeps instants to 1/800 of a frame
// (MOTION-WEB-2026-10 MW2), so that is the finest fraction read.
struct FrameNumber {
    int64_t numerator = 0;
    int64_t denominator = 1;
    double Value() const {
        return static_cast<double>(numerator) /
               static_cast<double>(denominator);
    }
};

FrameNumber FrameOf(double seconds, const Timebase& timebase) {
    const double frames = seconds * timebase.Fps();
    const double nearest = std::round(frames);
    if (std::fabs(frames - nearest) < 1e-3) {
        return {static_cast<int64_t>(nearest), 1};
    }
    for (const int64_t denominator :
         {2, 3, 4, 5, 6, 8, 10, 16, 20, 25, 40, 50, 100, 200, 400, 800}) {
        const double scaled = frames * static_cast<double>(denominator);
        if (std::fabs(scaled - std::round(scaled)) < 1e-3 * denominator) {
            const auto numerator = static_cast<int64_t>(std::round(scaled));
            const int64_t divisor = Gcd(numerator, denominator);
            return {numerator / divisor, denominator / divisor};
        }
    }
    const auto numerator = static_cast<int64_t>(std::round(frames * 800.0));
    const int64_t divisor = Gcd(numerator, 800);
    return {numerator / divisor, 800 / divisor};
}

Value FrameValue(FrameNumber frame) {
    if (frame.denominator == 1) {
        return Value::MakeInt(frame.numerator);
    }
    return Value::MakeString(std::to_string(frame.numerator) + "/" +
                             std::to_string(frame.denominator));
}

FrameNumber Shifted(FrameNumber frame, int64_t offset) {
    return {frame.numerator - offset * frame.denominator, frame.denominator};
}

std::string HexColor(const std::vector<double>& rgb, double alpha = 1.0) {
    auto channel = [&](size_t index) {
        return static_cast<int>(
            std::lround(std::clamp(Component(rgb, index), 0.0, 1.0) * 255.0));
    };
    char buffer[32];
    if (alpha >= 1.0 - 1e-9) {
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", channel(0),
                      channel(1), channel(2));
    } else {
        std::snprintf(buffer, sizeof(buffer), "rgba(%d, %d, %d, %s)",
                      channel(0), channel(1), channel(2),
                      Decimal(std::clamp(alpha, 0.0, 1.0)).c_str());
    }
    return buffer;
}

std::string CubicBezier(double x1, double y1, double x2, double y2) {
    return "cubic-bezier(" + Decimal(std::clamp(x1, 0.0, 1.0), 4) + ", " +
           Decimal(y1, 4) + ", " + Decimal(std::clamp(x2, 0.0, 1.0), 4) + ", " +
           Decimal(y2, 4) + ")";
}

// The easing of one After segment for one dimension (or along the path of a
// spatial property): the same curve, normalised to [0, 1] x [0, 1].
std::string SegmentEasing(const AfterKey& from, const AfterKey& to,
                          size_t dimension, bool spatial) {
    if (from.hold_out) {
        return "step-end";
    }
    if (from.linear_out && to.linear_in) {
        return "linear";
    }
    const double span = to.time - from.time;
    double delta = 0.0;
    size_t easeDimension = dimension;
    if (spatial) {
        delta = Distance(from.value, to.value);
        easeDimension = 0;
    } else {
        delta =
            Component(to.value, dimension) - Component(from.value, dimension);
    }
    if (std::fabs(delta) < 1e-12 || span <= 0.0) {
        return "linear";
    }
    const auto [outSpeed, outInfluence] = EaseOf(from.out_ease, easeDimension);
    const auto [inSpeed, inInfluence] = EaseOf(to.in_ease, easeDimension);
    const double x1 = outInfluence / 100.0;
    const double x2 = 1.0 - inInfluence / 100.0;
    const double y1 = outSpeed * span * x1 / delta;
    const double y2 = 1.0 - inSpeed * span * (inInfluence / 100.0) / delta;
    if (std::fabs(x1 - y1) < 1e-4 && std::fabs(x2 - y2) < 1e-4) {
        return "linear";
    }
    return CubicBezier(x1, y1, x2, y2);
}

// ---------------------------------------------------------------------------
// The import

struct Layer {
    const Value* raw = nullptr;
    std::string id;
    int index = 0;
    std::string name;
    std::string kind;
    std::string parent_id;
    double width = 0.0;
    double height = 0.0;
    AfterProperty anchor;
    AfterProperty position;
    AfterProperty position_x;
    AfterProperty position_y;
    AfterProperty scale;
    AfterProperty rotation;
    AfterProperty opacity;
    bool separated = false;
};

// A scalar track of a node: what the format animates, as a function of one
// dimension of an After property (value * factor + bias).
struct Track {
    std::string style;
    const AfterProperty* property = nullptr;
    size_t dimension = 0;
    double factor = 1.0;
    double bias = 0.0;
};

// What paints a shape of a shape layer: the nearest fill and stroke listed
// under it (ShapeChildren). A rectangle only filled becomes a box (the
// readable form), any other shape a path.
struct ShapePaint {
    bool fill = false;
    std::vector<double> fill_color;
    double fill_opacity = 1.0;
    bool stroke = false;
    std::vector<double> stroke_color;
    double stroke_opacity = 1.0;
    double stroke_width = 0.0;
};

class Importer {
public:
    Importer(const Value& comp, const MotionAfterImportOptions& options,
             const MotionAfterFontResolver& fonts, MotionMeasurer& measurer,
             MotionAfterImportReport& report)
        : comp_(comp),
          options_(options),
          fonts_(fonts),
          measurer_(measurer),
          report_(report) {}

    bool Run(Value& definition, MotionError& error);

private:
    // A note is said once: two dimensions of one property, or two layers
    // reading the same matte, do not repeat it.
    void Note(const std::string& layer, const std::string& note) {
        for (const MotionAfterImportNote& said : report_.notes) {
            if (said.layer == layer && said.note == note) {
                return;
            }
        }
        report_.notes.push_back({layer, note});
    }

    std::string UniqueId(const std::string& wanted) {
        std::string base = wanted.empty() ? "calque" : wanted;
        std::string id = base;
        int suffix = 2;
        while (ids_.count(id) != 0) {
            id = base + "#" + std::to_string(suffix++);
        }
        ids_.insert(id);
        return id;
    }

    const Layer* FindLayer(const std::string& id) const {
        for (const Layer& layer : layers_) {
            if (layer.id == id) {
                return &layer;
            }
        }
        return nullptr;
    }

    // Value of a track at a frame.
    double TrackAt(const Track& track, int64_t frame) const {
        return Component(ValueAt(*track.property, frame, timebase_),
                         track.dimension) *
                   track.factor +
               track.bias;
    }

    void SetTrack(Value& style, Value& animate, const Track& track,
                  int64_t offset, double neutral, const std::string& where);
    Value Animation(const std::vector<FrameNumber>& frames,
                    const std::vector<Value>& values,
                    const std::vector<std::string>& easings) {
        Value interpolate = Value::MakeObject();
        Value frameList = Value::MakeArray();
        for (const FrameNumber& frame : frames) {
            frameList.Push(FrameValue(frame));
        }
        Value valueList = Value::MakeArray();
        for (const Value& value : values) {
            valueList.Push(value);
        }
        interpolate.Set("frames", std::move(frameList));
        interpolate.Set("values", std::move(valueList));
        if (!easings.empty()) {
            bool same = std::all_of(easings.begin(), easings.end(),
                                    [&](const std::string& easing) {
                                        return easing == easings[0];
                                    });
            if (same) {
                interpolate.Set("easing", Value::MakeString(easings[0]));
            } else {
                Value list = Value::MakeArray();
                for (const std::string& easing : easings) {
                    list.Push(Value::MakeString(easing));
                }
                interpolate.Set("easing", std::move(list));
            }
        }
        report_.keys += static_cast<int>(frames.size());
        Value wrapper = Value::MakeObject();
        wrapper.Set("interpolate", std::move(interpolate));
        return wrapper;
    }

    // The layer's transform as a node's: origin at the anchor, translate to
    // the position, scale, rotation, and the opacity when `withOpacity`.
    void SetTransform(const Layer& layer, double boxLeft, double boxTop,
                      Value& style, Value& animate, int64_t offset,
                      bool withOpacity);

    // Shape contents.
    bool ShapeChildren(const Layer& layer, const Value* group,
                       const std::optional<std::vector<double>>& fillOverride,
                       Value& children, int64_t offset,
                       const ShapePaint& inherited);

    // A rectangle's size and centre are drawn as read at the first frame:
    // said as a constant expression when After computes the same value at
    // every frame, as not translated when they move.
    void RectNote(const Layer& layer, const std::string& name,
                  const AfterProperty& size, const AfterProperty& position) {
        if (!size.Animated() && !position.Animated()) {
            return;
        }
        bool constant = size.keys.empty() && position.keys.empty();
        for (const auto& sample : size.samples) {
            constant = constant && sample == size.samples.front();
        }
        for (const auto& sample : position.samples) {
            constant = constant && sample == position.samples.front();
        }
        Note(layer.name,
             name + (constant ? " : expression lue image par image (constante)"
                              : " : taille ou position animée non traduite, "
                                "valeur de la première image"));
    }

    bool TextNode(const Layer& layer, Value& node, double& boxLeft,
                  double& boxTop, int64_t offset);

    std::optional<std::vector<double>> FillEffect(const Layer& layer);

    // The track matte of `layer` as a clip polygon in the coordinates of
    // its node's box, one key per frame where it changes.
    bool MatteClip(const Layer& layer, double boxLeft, double boxTop,
                   Value& style, Value& animate, int64_t offset);

    // Layer -> comp matrix at a frame (a, b, c, d, e, f: x' = a x + c y + e).
    std::array<double, 6> LayerMatrix(const Layer& layer, int64_t frame,
                                      int depth = 0) const;

    const Value& comp_;
    const MotionAfterImportOptions& options_;
    const MotionAfterFontResolver& fonts_;
    MotionMeasurer& measurer_;
    MotionAfterImportReport& report_;
    Timebase timebase_;
    int64_t duration_ = 0;
    std::vector<Layer> layers_;
    std::set<std::string> ids_;
};

Timebase TimebaseOf(double frameDuration, double frameRate) {
    const double fps = frameDuration > 0.0 ? 1.0 / frameDuration : frameRate;
    for (const double drop : {24.0, 30.0, 60.0, 120.0}) {
        const double ntsc = drop * 1000.0 / 1001.0;
        if (std::fabs(fps - ntsc) < 1e-3) {
            return {static_cast<int64_t>(drop * 1000.0), 1001};
        }
    }
    const double rounded = std::round(fps);
    if (std::fabs(fps - rounded) < 1e-3 && rounded > 0.0) {
        return {static_cast<int64_t>(rounded), 1};
    }
    // Anything else, to a thousandth of a frame per second.
    const auto numerator = static_cast<int64_t>(std::round(fps * 1000.0));
    const int64_t divisor = Gcd(numerator, 1000);
    return {numerator / divisor, 1000 / divisor};
}

std::array<double, 6> Multiply(const std::array<double, 6>& m,
                               const std::array<double, 6>& n) {
    // m after n: m(n(q)).
    return {m[0] * n[0] + m[2] * n[1],        m[1] * n[0] + m[3] * n[1],
            m[0] * n[2] + m[2] * n[3],        m[1] * n[2] + m[3] * n[3],
            m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}

std::array<double, 6> Invert(const std::array<double, 6>& m) {
    const double det = m[0] * m[3] - m[1] * m[2];
    if (std::fabs(det) < 1e-12) {
        return {0, 0, 0, 0, 0, 0};
    }
    const double a = m[3] / det;
    const double b = -m[1] / det;
    const double c = -m[2] / det;
    const double d = m[0] / det;
    return {a, b, c, d, -(a * m[4] + c * m[5]), -(b * m[4] + d * m[5])};
}

// P + R S (q - A) as a matrix.
std::array<double, 6> TransformMatrix(double ax, double ay, double px,
                                      double py, double sx, double sy,
                                      double degrees) {
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double a = cosine * sx;
    const double b = sine * sx;
    const double c = -sine * sy;
    const double d = cosine * sy;
    return {a, b, c, d, px - (a * ax + c * ay), py - (b * ax + d * ay)};
}

std::array<double, 6> Importer::LayerMatrix(const Layer& layer, int64_t frame,
                                            int depth) const {
    const std::vector<double> anchor = ValueAt(layer.anchor, frame, timebase_);
    double px = 0.0;
    double py = 0.0;
    if (layer.separated) {
        px = Component(ValueAt(layer.position_x, frame, timebase_), 0);
        py = Component(ValueAt(layer.position_y, frame, timebase_), 0);
    } else {
        const std::vector<double> position =
            ValueAt(layer.position, frame, timebase_);
        px = Component(position, 0);
        py = Component(position, 1);
    }
    const std::vector<double> scale = ValueAt(layer.scale, frame, timebase_);
    const double rotation =
        Component(ValueAt(layer.rotation, frame, timebase_), 0);
    const std::array<double, 6> own =
        TransformMatrix(Component(anchor, 0), Component(anchor, 1), px, py,
                        Component(scale, 0, 100.0) / 100.0,
                        Component(scale, 1, 100.0) / 100.0, rotation);
    if (!layer.parent_id.empty() && depth < 32) {
        if (const Layer* parent = FindLayer(layer.parent_id)) {
            return Multiply(LayerMatrix(*parent, frame, depth + 1), own);
        }
    }
    return own;
}

void Importer::SetTrack(Value& style, Value& animate, const Track& track,
                        int64_t offset, double neutral,
                        const std::string& where) {
    const AfterProperty& property = *track.property;
    if (!property.present) {
        return;
    }
    if (!property.samples.empty()) {
        // Sampled: one key per frame where the value changes, linear
        // between, so every frame shows the value After computed.
        std::vector<FrameNumber> frames;
        std::vector<Value> values;
        const auto count = static_cast<int64_t>(property.samples.size());
        auto at = [&](int64_t index) {
            return Component(property.samples[static_cast<size_t>(index)],
                             track.dimension) *
                       track.factor +
                   track.bias;
        };
        for (int64_t index = 0; index < count; ++index) {
            const double value = at(index);
            const bool changesBefore =
                index > 0 && std::fabs(at(index - 1) - value) > 1e-6;
            const bool changesAfter =
                index + 1 < count && std::fabs(at(index + 1) - value) > 1e-6;
            if (index == 0 || index + 1 == count || changesBefore ||
                changesAfter) {
                frames.push_back({property.samples_from + index - offset, 1});
                values.push_back(Num(value));
            }
        }
        bool constant = true;
        for (const Value& value : values) {
            constant = constant && value.Dump() == values.front().Dump();
        }
        // The note speaks of the whole property, every dimension.
        bool still = true;
        for (const std::vector<double>& sample : property.samples) {
            still = still && sample == property.samples.front();
        }
        if (!property.expression.empty()) {
            Note(where,
                 property.name + " : expression lue image par image" +
                     std::string(still ? " (constante)" : ", valeurs figées"));
        } else {
            Note(where,
                 property.name + " : trajectoire courbe, une clé par image");
        }
        if (constant) {
            const double value = at(0);
            if (std::fabs(value - neutral) > 1e-9) {
                style.Set(track.style, Num(value));
            }
            return;
        }
        std::vector<std::string> easings(frames.size() - 1, "linear");
        animate.Set(track.style, Animation(frames, values, easings));
        return;
    }
    if (property.keys.empty()) {
        const double value =
            Component(property.value, track.dimension) * track.factor +
            track.bias;
        if (std::fabs(value - neutral) > 1e-9) {
            style.Set(track.style, Num(value));
        }
        return;
    }
    // Keys: unchanged in this dimension -> a constant.
    bool moves = false;
    for (const AfterKey& key : property.keys) {
        moves = moves || std::fabs(Component(key.value, track.dimension) -
                                   Component(property.keys[0].value,
                                             track.dimension)) > 1e-9;
    }
    if (!moves) {
        const double value =
            Component(property.keys[0].value, track.dimension) * track.factor +
            track.bias;
        if (std::fabs(value - neutral) > 1e-9) {
            style.Set(track.style, Num(value));
        }
        return;
    }
    std::vector<FrameNumber> frames;
    std::vector<Value> values;
    std::vector<std::string> easings;
    for (size_t index = 0; index < property.keys.size(); ++index) {
        const AfterKey& key = property.keys[index];
        frames.push_back(Shifted(FrameOf(key.time, timebase_), offset));
        values.push_back(Num(
            Component(key.value, track.dimension) * track.factor + track.bias));
        if (index + 1 < property.keys.size()) {
            easings.push_back(SegmentEasing(key, property.keys[index + 1],
                                            track.dimension, property.spatial));
        }
    }
    animate.Set(track.style, Animation(frames, values, easings));
}

void Importer::SetTransform(const Layer& layer, double boxLeft, double boxTop,
                            Value& style, Value& animate, int64_t offset,
                            bool withOpacity) {
    if (layer.anchor.Animated()) {
        Note(layer.name,
             "point d'ancrage animé : non traduit, valeur de la première "
             "image");
    }
    const std::vector<double> anchor = layer.anchor.samples.empty()
                                           ? layer.anchor.value
                                           : layer.anchor.samples.front();
    const double ax = Component(anchor, 0);
    const double ay = Component(anchor, 1);
    style.Set("transformOrigin",
              Value::MakeString(Px(ax - boxLeft) + " " + Px(ay - boxTop)));
    if (layer.separated) {
        SetTrack(style, animate, {"translateX", &layer.position_x, 0, 1.0, -ax},
                 offset, 0.0, layer.name);
        SetTrack(style, animate, {"translateY", &layer.position_y, 0, 1.0, -ay},
                 offset, 0.0, layer.name);
    } else {
        SetTrack(style, animate, {"translateX", &layer.position, 0, 1.0, -ax},
                 offset, 0.0, layer.name);
        SetTrack(style, animate, {"translateY", &layer.position, 1, 1.0, -ay},
                 offset, 0.0, layer.name);
    }
    // Uniform scale is `scale`; otherwise one track per axis, each with its
    // own ease, as After keeps them.
    bool uniform = true;
    auto equalAxes = [](const std::vector<double>& value) {
        return std::fabs(Component(value, 0, 100.0) -
                         Component(value, 1, 100.0)) < 1e-9;
    };
    uniform = equalAxes(layer.scale.value);
    for (const AfterKey& key : layer.scale.keys) {
        uniform = uniform && equalAxes(key.value);
    }
    for (const std::vector<double>& sample : layer.scale.samples) {
        uniform = uniform && equalAxes(sample);
    }
    if (uniform) {
        SetTrack(style, animate, {"scale", &layer.scale, 0, 0.01, 0.0}, offset,
                 1.0, layer.name);
    } else {
        SetTrack(style, animate, {"scaleX", &layer.scale, 0, 0.01, 0.0}, offset,
                 1.0, layer.name);
        SetTrack(style, animate, {"scaleY", &layer.scale, 1, 0.01, 0.0}, offset,
                 1.0, layer.name);
    }
    SetTrack(style, animate, {"rotate", &layer.rotation, 0, 1.0, 0.0}, offset,
             0.0, layer.name);
    if (withOpacity) {
        SetTrack(style, animate, {"opacity", &layer.opacity, 0, 0.01, 0.0},
                 offset, 1.0, layer.name);
    }
}

// The Fill effect (ADBE Fill) paints the layer in one colour: on a text or a
// shape, its colour replaces theirs. Anything it does beyond that is named.
std::optional<std::vector<double>> Importer::FillEffect(const Layer& layer) {
    std::optional<std::vector<double>> colour;
    const Value* effects = Field(layer.raw, "effects");
    for (const Value* effect : Groups(effects)) {
        const std::string match = Text(Field(effect, "match_name"));
        const std::string name = Text(Field(effect, "name"));
        if (Field(effect, "enabled") != nullptr &&
            !Flag(Field(effect, "enabled"))) {
            continue;
        }
        if (match == "ADBE Fill" &&
            (layer.kind == "text" || layer.kind == "shape")) {
            const AfterProperty fill = GroupProperty(effect, "ADBE Fill-0002");
            const AfterProperty opacity =
                GroupProperty(effect, "ADBE Fill-0005");
            const AfterProperty invert =
                GroupProperty(effect, "ADBE Fill-0006");
            const AfterProperty feather =
                GroupProperty(effect, "ADBE Fill-0003");
            const AfterProperty all = GroupProperty(effect, "ADBE Fill-0007");
            if (fill.Animated() || opacity.Animated()) {
                Note(layer.name, "effet " + name +
                                     " animé : non traduit, valeur de la "
                                     "première image");
            }
            if (Component(opacity.value, 0, 1.0) < 1.0 - 1e-9 ||
                Component(invert.value, 0) != 0.0 ||
                Component(feather.value, 0) != 0.0 ||
                Component(all.value, 0) != 0.0) {
                Note(layer.name,
                     "effet " + name +
                         " : seule sa couleur est traduite (opacité, "
                         "inversion, contour progressif ou masques non)");
            }
            colour = fill.value;
            continue;
        }
        std::string note = "effet " + name;
        note += " (" + match + ") : non traduit";
        Note(layer.name, note);
    }
    return colour;
}

bool Importer::TextNode(const Layer& layer, Value& node, double& boxLeft,
                        double& boxTop, int64_t offset) {
    (void)offset;
    const Value* text = Field(layer.raw, "text");
    std::string content = Text(Field(text, "text"));
    // After ends a paragraph with CR and a line with ETX.
    for (char& character : content) {
        if (character == '\r' || character == '\x03') {
            character = '\n';
        }
    }
    if (Flag(Field(text, "keyed")) || Flag(Field(text, "expression"))) {
        Note(layer.name,
             "texte source animé ou par expression : non traduit, texte de "
             "la première image");
    }
    if (Number(Field(text, "animators")) > 0) {
        Note(layer.name, "animateur de texte : non traduit");
    }
    if (Flag(Field(text, "on_path"))) {
        Note(layer.name, "texte sur un tracé : non traduit");
    }
    std::string postScript = Text(Field(text, "font"));
    if (auto replaced = options_.fonts.find(postScript);
        replaced != options_.fonts.end()) {
        Note(layer.name,
             "police " + postScript + " remplacée par " + replaced->second);
        postScript = replaced->second;
    }
    MotionAfterFont face;
    bool found = fonts_ && fonts_(postScript, face);
    if (!found) {
        face.family = Text(Field(text, "font_family"));
        Note(layer.name, "police " + postScript +
                             " absente de cette machine : le rendu la "
                             "refusera (FontMissing)");
    }
    const double size = Number(Field(text, "font_size"), 72.0);
    const double leading = Flag(Field(text, "auto_leading"))
                               ? size * 1.2
                               : Number(Field(text, "leading"), size * 1.2);
    const double tracking = Number(Field(text, "tracking"));
    const std::string justification = Text(Field(text, "justification"));
    std::string align = "left";
    if (justification == "CENTER_JUSTIFY") {
        align = "center";
    } else if (justification == "RIGHT_JUSTIFY") {
        align = "right";
    } else if (justification != "LEFT_JUSTIFY" && !justification.empty()) {
        Note(layer.name, "justification " + justification +
                             " : non traduite, alignée à gauche");
    }
    if (Flag(Field(text, "small_caps"))) {
        Note(layer.name, "petites capitales : non traduites");
    }
    if (Flag(Field(text, "faux_bold")) || Flag(Field(text, "faux_italic"))) {
        Note(layer.name, "faux gras ou faux italique : non traduit");
    }
    if (Number(Field(text, "baseline_shift")) != 0.0 ||
        Number(Field(text, "horizontal_scale"), 1.0) != 1.0 ||
        Number(Field(text, "vertical_scale"), 1.0) != 1.0) {
        Note(layer.name,
             "décalage de ligne de base ou échelle des caractères : non "
             "traduit");
    }
    if (Flag(Field(text, "apply_stroke"))) {
        Note(layer.name, "contour du texte : non traduit");
    }
    // After kerns by the font's pairs unless told not to; the engine does
    // not kern (MOTION-AFTER-2026-09): a pair moves every letter after it.
    // A reading without the field comes from After before 24, which
    // kerned by metrics too.
    if (Text(Field(text, "auto_kern")) != "NO_AUTO_KERN") {
        Note(layer.name,
             "crénage d'After (" +
                 (Text(Field(text, "auto_kern")).empty()
                      ? std::string("métrique")
                      : Text(Field(text, "auto_kern"))) +
                 ") : le moteur ne crène pas, les lettres qui suivent une "
                 "paire crénée se décalent");
    }

    std::vector<double> colour = Numbers(Field(text, "fill_color"));
    if (std::optional<std::vector<double>> fill = FillEffect(layer)) {
        colour = *fill;
    }
    const bool filled =
        Field(text, "apply_fill") == nullptr || Flag(Field(text, "apply_fill"));

    MotionTextStyle measured;
    measured.font_family = face.family;
    measured.font_weight = face.weight;
    measured.italic = face.italic;
    measured.font_size = size;
    measured.line_height = leading;
    measured.letter_spacing = tracking / 1000.0 * size;
    measured.text_transform =
        Flag(Field(text, "all_caps")) ? "uppercase" : "none";
    measured.text_align = align;

    // Point text: lines break only where After breaks them, each placed
    // about the layer's origin by its justification, the first baseline on
    // the origin. The box is wider than the longest line so nothing wraps.
    double width = 0.0;
    double baseline = size * 0.8;
    const bool boxText = Flag(Field(text, "box_text"));
    std::vector<double> boxSize = Numbers(Field(text, "box_size"));
    std::vector<double> boxPosition = Numbers(Field(text, "box_position"));
    MotionTextLayout layout;
    MotionError measureError;
    if (found) {
        if (measurer_.LayoutText(content, measured, -1.0, layout,
                                 measureError)) {
            width = layout.width;
        }
    }
    double boxWidth = std::ceil(width) + 2.0 * size;
    if (boxText) {
        boxWidth = Component(boxSize, 0, boxWidth);
        Note(layer.name,
             "texte de paragraphe : première ligne de base posée par le "
             "moteur, pas mesurée dans After");
    }
    if (found) {
        layout = {};
        if (measurer_.LayoutText(content, measured, boxWidth, layout,
                                 measureError) &&
            !layout.lines.empty()) {
            baseline = layout.lines.front().baseline;
        }
    }
    if (boxText) {
        boxLeft = Component(boxPosition, 0);
        boxTop = Component(boxPosition, 1);
    } else {
        boxLeft = align == "left"     ? 0.0
                  : align == "center" ? -boxWidth / 2.0
                                      : -boxWidth;
        boxTop = -baseline;
    }

    node = Value::MakeObject();
    node.Set("type", Value::MakeString("text"));
    node.Set("id", Value::MakeString(UniqueId(layer.name)));
    node.Set("content", Value::MakeString(content));
    Value style = Value::MakeObject();
    style.Set("position", Value::MakeString("absolute"));
    style.Set("left", Num(boxLeft));
    style.Set("top", Num(boxTop));
    style.Set("width", Num(boxWidth));
    style.Set("fontFamily", Value::MakeString(face.family));
    if (face.weight != 400) {
        style.Set("fontWeight", Value::MakeInt(face.weight));
    }
    if (face.italic) {
        style.Set("fontStyle", Value::MakeString("italic"));
    }
    style.Set("fontSize", Num(size));
    style.Set("lineHeight", Value::MakeString(Px(leading)));
    if (tracking != 0.0) {
        style.Set("letterSpacing", Num(measured.letter_spacing));
    }
    if (measured.text_transform != "none") {
        style.Set("textTransform", Value::MakeString("uppercase"));
    }
    style.Set("textAlign", Value::MakeString(align));
    style.Set("color", Value::MakeString(filled ? HexColor(colour)
                                                : std::string("transparent")));
    node.Set("style", std::move(style));
    return true;
}

std::string RectPath(double cx, double cy, double w, double h, double r) {
    const double x0 = cx - w / 2.0;
    const double y0 = cy - h / 2.0;
    r = std::clamp(r, 0.0, std::min(w, h) / 2.0);
    if (r <= 0.0) {
        return "M " + Decimal(x0) + " " + Decimal(y0) + " H " +
               Decimal(x0 + w) + " V " + Decimal(y0 + h) + " H " + Decimal(x0) +
               " Z";
    }
    const std::string rr = Decimal(r) + " " + Decimal(r) + " 0 0 1 ";
    return "M " + Decimal(x0 + r) + " " + Decimal(y0) + " H " +
           Decimal(x0 + w - r) + " A " + rr + Decimal(x0 + w) + " " +
           Decimal(y0 + r) + " V " + Decimal(y0 + h - r) + " A " + rr +
           Decimal(x0 + w - r) + " " + Decimal(y0 + h) + " H " +
           Decimal(x0 + r) + " A " + rr + Decimal(x0) + " " +
           Decimal(y0 + h - r) + " V " + Decimal(y0 + r) + " A " + rr +
           Decimal(x0 + r) + " " + Decimal(y0) + " Z";
}

std::string EllipsePath(double cx, double cy, double w, double h) {
    const double rx = w / 2.0;
    const double ry = h / 2.0;
    const std::string arc = " A " + Decimal(rx) + " " + Decimal(ry) + " 0 1 1 ";
    return "M " + Decimal(cx - rx) + " " + Decimal(cy) + arc +
           Decimal(cx + rx) + " " + Decimal(cy) + arc + Decimal(cx - rx) + " " +
           Decimal(cy) + " Z";
}

std::string BezierPath(const Value* shape) {
    const Value* vertices = Field(shape, "vertices");
    const Value* ins = Field(shape, "in_tangents");
    const Value* outs = Field(shape, "out_tangents");
    if (vertices == nullptr || !vertices->IsArray() ||
        vertices->AsArray().empty()) {
        return {};
    }
    const auto& v = vertices->AsArray();
    auto point = [](const Value* list, size_t index) {
        std::vector<double> p;
        if (list != nullptr && list->IsArray() &&
            index < list->AsArray().size()) {
            p = Numbers(&list->AsArray()[index]);
        }
        p.resize(2, 0.0);
        return p;
    };
    std::string d;
    const size_t count = v.size();
    const bool closed = Flag(Field(shape, "closed"));
    for (size_t index = 0; index < count; ++index) {
        const std::vector<double> p = point(vertices, index);
        if (index == 0) {
            d += "M " + Decimal(p[0]) + " " + Decimal(p[1]);
            continue;
        }
        const std::vector<double> prev = point(vertices, index - 1);
        const std::vector<double> out = point(outs, index - 1);
        const std::vector<double> in = point(ins, index);
        d += " C " + Decimal(prev[0] + out[0]) + " " +
             Decimal(prev[1] + out[1]) + " " + Decimal(p[0] + in[0]) + " " +
             Decimal(p[1] + in[1]) + " " + Decimal(p[0]) + " " + Decimal(p[1]);
    }
    if (closed && count > 1) {
        const std::vector<double> last = point(vertices, count - 1);
        const std::vector<double> first = point(vertices, 0);
        const std::vector<double> out = point(outs, count - 1);
        const std::vector<double> in = point(ins, 0);
        d += " C " + Decimal(last[0] + out[0]) + " " +
             Decimal(last[1] + out[1]) + " " + Decimal(first[0] + in[0]) + " " +
             Decimal(first[1] + in[1]) + " " + Decimal(first[0]) + " " +
             Decimal(first[1]) + " Z";
    }
    return d;
}

bool Importer::ShapeChildren(
    const Layer& layer, const Value* group,
    const std::optional<std::vector<double>>& fillOverride, Value& children,
    int64_t offset, const ShapePaint& inherited) {
    // After draws a group's contents bottom to top, and a fill or a stroke
    // paints every shape listed above it, in its group and in the groups
    // above it there. `below[i]` is what paints item i: the nearest fill
    // and stroke under it, then what the enclosing group hands down.
    const std::vector<const Value*> items = Groups(group);
    std::vector<ShapePaint> below(items.size() + 1, inherited);
    for (size_t index = items.size(); index-- > 0;) {
        below[index] = below[index + 1];
        const Value* item = items[index];
        const std::string match = Text(Field(item, "match_name"));
        const bool enabled =
            Field(item, "enabled") == nullptr || Flag(Field(item, "enabled"));
        if (!enabled) {
            continue;
        }
        ShapePaint& paint = below[index];
        if (match == "ADBE Vector Graphic - Fill") {
            paint.fill = true;
            const AfterProperty colour =
                GroupProperty(item, "ADBE Vector Fill Color");
            const AfterProperty opacity =
                GroupProperty(item, "ADBE Vector Fill Opacity");
            if (colour.Animated() || opacity.Animated()) {
                Note(layer.name, Text(Field(item, "name")) +
                                     " : remplissage animé non traduit, "
                                     "valeur de la première image");
            }
            paint.fill_color = fillOverride ? *fillOverride : colour.value;
            paint.fill_opacity = Component(opacity.value, 0, 100.0) / 100.0;
        } else if (match == "ADBE Vector Graphic - Stroke") {
            paint.stroke = true;
            const AfterProperty colour =
                GroupProperty(item, "ADBE Vector Stroke Color");
            const AfterProperty opacity =
                GroupProperty(item, "ADBE Vector Stroke Opacity");
            const AfterProperty width =
                GroupProperty(item, "ADBE Vector Stroke Width");
            if (colour.Animated() || opacity.Animated() || width.Animated()) {
                Note(layer.name, Text(Field(item, "name")) +
                                     " : contour animé non traduit, valeur "
                                     "de la première image");
            }
            paint.stroke_color = fillOverride ? *fillOverride : colour.value;
            paint.stroke_opacity = Component(opacity.value, 0, 100.0) / 100.0;
            paint.stroke_width = Component(width.value, 0, 0.0);
            for (const Value* sub : Groups(item)) {
                const Value* dashes = Properties(sub);
                if (Text(Field(sub, "match_name")) ==
                        "ADBE Vector Stroke Dashes" &&
                    dashes != nullptr &&
                    dashes->Find("ADBE Vector Stroke Dash 1") != nullptr) {
                    Note(layer.name, "tirets du contour : non traduits");
                }
            }
        }
    }

    // Bottom of the list first: the format paints children in order.
    for (size_t position = items.size(); position-- > 0;) {
        const Value* item = items[position];
        const ShapePaint& paint = below[position + 1];
        const std::string match = Text(Field(item, "match_name"));
        const std::string name = Text(Field(item, "name"));
        const bool enabled =
            Field(item, "enabled") == nullptr || Flag(Field(item, "enabled"));
        if (!enabled || match == "ADBE Vector Graphic - Fill" ||
            match == "ADBE Vector Graphic - Stroke" ||
            match == "ADBE Vector Materials Group" ||
            match.rfind("ADBE Vector Filter", 0) == 0) {
            continue;
        }
        if (match == "ADBE Vector Group") {
            Value node = Value::MakeObject();
            node.Set("type", Value::MakeString("box"));
            node.Set("id", Value::MakeString(UniqueId(name)));
            Value style = Value::MakeObject();
            style.Set("position", Value::MakeString("absolute"));
            style.Set("left", Value::MakeInt(0));
            style.Set("top", Value::MakeInt(0));
            Value animate = Value::MakeObject();
            const Value* contents = nullptr;
            const Value* transform = nullptr;
            for (const Value* sub : Groups(item)) {
                const std::string subMatch = Text(Field(sub, "match_name"));
                if (subMatch == "ADBE Vectors Group") {
                    contents = sub;
                } else if (subMatch == "ADBE Vector Transform Group") {
                    transform = sub;
                }
            }
            if (transform != nullptr) {
                Layer groupTransform;
                groupTransform.name = layer.name + " › " + name;
                groupTransform.anchor =
                    GroupProperty(transform, "ADBE Vector Anchor", true);
                groupTransform.position =
                    GroupProperty(transform, "ADBE Vector Position", true);
                groupTransform.scale =
                    GroupProperty(transform, "ADBE Vector Scale");
                groupTransform.rotation =
                    GroupProperty(transform, "ADBE Vector Rotation");
                groupTransform.opacity =
                    GroupProperty(transform, "ADBE Vector Group Opacity");
                const AfterProperty skew =
                    GroupProperty(transform, "ADBE Vector Skew");
                if (skew.Animated() || Component(skew.value, 0) != 0.0) {
                    Note(groupTransform.name,
                         "inclinaison du groupe : non traduite");
                }
                SetTransform(groupTransform, 0.0, 0.0, style, animate, offset,
                             true);
            }
            Value nested = Value::MakeArray();
            if (contents != nullptr &&
                !ShapeChildren(layer, contents, fillOverride, nested, offset,
                               paint)) {
                return false;
            }
            node.Set("style", std::move(style));
            if (!animate.AsObject().empty()) {
                node.Set("animate", std::move(animate));
            }
            node.Set("children", std::move(nested));
            ++report_.nodes;
            children.Push(std::move(node));
            continue;
        }
        const bool rect = match == "ADBE Vector Shape - Rect";
        const bool ellipse = match == "ADBE Vector Shape - Ellipse";
        const bool path = match == "ADBE Vector Shape - Group";
        if (!rect && !ellipse && !path) {
            std::string note = name;
            note += " (" + match + ") : non traduit";
            Note(layer.name, note);
            continue;
        }
        if (!paint.fill && !paint.stroke) {
            continue;
        }
        Value node = Value::MakeObject();
        Value style = Value::MakeObject();
        Value animate = Value::MakeObject();
        style.Set("position", Value::MakeString("absolute"));
        if (rect && paint.fill && !paint.stroke) {
            const AfterProperty size =
                GroupProperty(item, "ADBE Vector Rect Size");
            const AfterProperty position =
                GroupProperty(item, "ADBE Vector Rect Position", true);
            const AfterProperty roundness =
                GroupProperty(item, "ADBE Vector Rect Roundness");
            const std::vector<double> s =
                size.samples.empty() ? size.value : size.samples.front();
            const std::vector<double> p = position.samples.empty()
                                              ? position.value
                                              : position.samples.front();
            RectNote(layer, name, size, position);
            node.Set("type", Value::MakeString("box"));
            node.Set("id", Value::MakeString(UniqueId(name)));
            style.Set("left", Num(Component(p, 0) - Component(s, 0) / 2.0));
            style.Set("top", Num(Component(p, 1) - Component(s, 1) / 2.0));
            style.Set("width", Num(Component(s, 0)));
            style.Set("height", Num(Component(s, 1)));
            style.Set("backgroundColor",
                      Value::MakeString(
                          HexColor(paint.fill_color, paint.fill_opacity)));
            if (Component(roundness.value, 0) > 0.0) {
                style.Set("borderRadius", Num(Component(roundness.value, 0)));
            }
        } else {
            std::string d;
            if (rect) {
                const AfterProperty size =
                    GroupProperty(item, "ADBE Vector Rect Size");
                const AfterProperty position =
                    GroupProperty(item, "ADBE Vector Rect Position", true);
                const AfterProperty roundness =
                    GroupProperty(item, "ADBE Vector Rect Roundness");
                const std::vector<double> s =
                    size.samples.empty() ? size.value : size.samples.front();
                const std::vector<double> p = position.samples.empty()
                                                  ? position.value
                                                  : position.samples.front();
                RectNote(layer, name, size, position);
                d = RectPath(Component(p, 0), Component(p, 1), Component(s, 0),
                             Component(s, 1), Component(roundness.value, 0));
            } else if (ellipse) {
                const AfterProperty size =
                    GroupProperty(item, "ADBE Vector Ellipse Size");
                const AfterProperty position =
                    GroupProperty(item, "ADBE Vector Ellipse Position", true);
                if (size.Animated() || position.Animated()) {
                    Note(layer.name, name +
                                         " : taille ou position lue à la "
                                         "première image");
                }
                d = EllipsePath(
                    Component(position.value, 0), Component(position.value, 1),
                    Component(size.value, 0), Component(size.value, 1));
            } else {
                const Value* record =
                    Field(Properties(item), "ADBE Vector Shape");
                if (Field(record, "keys") != nullptr ||
                    Field(record, "expression") != nullptr) {
                    Note(layer.name, name +
                                         " : tracé animé (morphing) non "
                                         "traduit, forme de la première "
                                         "image");
                }
                d = BezierPath(Field(record, "value"));
            }
            if (d.empty()) {
                continue;
            }
            node.Set("type", Value::MakeString("path"));
            node.Set("id", Value::MakeString(UniqueId(name)));
            node.Set("d", Value::MakeString(d));
            style.Set("left", Value::MakeInt(0));
            style.Set("top", Value::MakeInt(0));
            style.Set("fill", Value::MakeString(
                                  paint.fill ? HexColor(paint.fill_color,
                                                        paint.fill_opacity)
                                             : std::string("none")));
            if (paint.stroke) {
                style.Set("stroke",
                          Value::MakeString(HexColor(paint.stroke_color,
                                                     paint.stroke_opacity)));
                style.Set("strokeWidth", Num(paint.stroke_width));
            }
        }
        node.Set("style", std::move(style));
        if (!animate.AsObject().empty()) {
            node.Set("animate", std::move(animate));
        }
        ++report_.nodes;
        children.Push(std::move(node));
    }
    for (const Value* item : items) {
        const std::string match = Text(Field(item, "match_name"));
        if (match == "ADBE Vector Filter - Trim") {
            const AfterProperty start =
                GroupProperty(item, "ADBE Vector Trim Start");
            const AfterProperty end =
                GroupProperty(item, "ADBE Vector Trim End");
            if (start.Animated() || end.Animated() ||
                Component(start.value, 0) != 0.0 ||
                Component(end.value, 0, 100.0) != 100.0) {
                Note(layer.name, Text(Field(item, "name")) +
                                     " : réduction de tracé non traduite");
            }
        } else if (match.rfind("ADBE Vector Filter", 0) == 0) {
            Note(layer.name,
                 Text(Field(item, "name")) + " (" + match + ") : non traduit");
        }
    }
    return true;
}

// A track matte made of one filled rectangle -- a box that reveals a text,
// the commonest matte of a title -- is a clip: the rectangle's corners
// carried into the matted layer's space, frame by frame. Any other matte is
// named, and the layer drawn unmatted.
bool Importer::MatteClip(const Layer& layer, double boxLeft, double boxTop,
                         Value& style, Value& animate, int64_t offset) {
    const std::string type = Text(Field(layer.raw, "track_matte"));
    if (type.empty() || type == "NO_TRACK_MATTE") {
        return true;
    }
    std::string matteId = Text(Field(layer.raw, "track_matte_layer_id"));
    if (matteId.empty()) {
        // A reading from before After 23 names no matte layer: the only
        // layer used as one, else the layer above (the old rule).
        int mattes = 0;
        for (const Layer& other : layers_) {
            if (Flag(Field(other.raw, "is_track_matte"))) {
                ++mattes;
                matteId = other.id;
            }
        }
        if (mattes != 1) {
            matteId.clear();
            for (const Layer& other : layers_) {
                if (other.index == layer.index - 1) {
                    matteId = other.id;
                }
            }
        }
    }
    const Layer* matte = FindLayer(matteId);
    auto refuse = [&](const std::string& why) {
        Note(layer.name, "cache de piste " + type + " : non traduit (" + why +
                             "), calque dessiné sans cache");
        return true;
    };
    if (type != "ALPHA") {
        return refuse("seul un cache alpha l'est");
    }
    if (matte == nullptr || matte->kind != "shape") {
        return refuse("le cache n'est pas un calque de forme");
    }
    // One rectangle, filled, opaque, in a group without rotation of its
    // own; opacity of the matte layer constant at 100 %.
    const Value* rectangle = nullptr;
    const Value* groupTransform = nullptr;
    int shapes = 0;
    std::function<void(const Value*, const Value*)> visit =
        [&](const Value* group, const Value* transform) {
            for (const Value* item : Groups(group)) {
                const std::string match = Text(Field(item, "match_name"));
                if (match == "ADBE Vector Group") {
                    const Value* contents = nullptr;
                    const Value* own = nullptr;
                    for (const Value* sub : Groups(item)) {
                        const std::string sm = Text(Field(sub, "match_name"));
                        if (sm == "ADBE Vectors Group") {
                            contents = sub;
                        } else if (sm == "ADBE Vector Transform Group") {
                            own = sub;
                        }
                    }
                    if (transform != nullptr && own != nullptr) {
                        ++shapes;  // nested transforms: not read here
                    }
                    visit(contents, own);
                } else if (match.rfind("ADBE Vector Shape", 0) == 0) {
                    ++shapes;
                    if (match == "ADBE Vector Shape - Rect") {
                        rectangle = item;
                        groupTransform = transform;
                    }
                }
            }
        };
    visit(Field(matte->raw, "contents"), nullptr);
    if (shapes != 1 || rectangle == nullptr) {
        return refuse("le cache n'est pas un rectangle seul");
    }
    if (matte->opacity.Animated() ||
        Component(matte->opacity.value, 0, 100.0) < 100.0) {
        return refuse("l'opacité du cache n'est pas pleine");
    }
    const AfterProperty size =
        GroupProperty(rectangle, "ADBE Vector Rect Size");
    const AfterProperty center =
        GroupProperty(rectangle, "ADBE Vector Rect Position", true);
    const AfterProperty roundness =
        GroupProperty(rectangle, "ADBE Vector Rect Roundness");
    if (roundness.Animated() || Component(roundness.value, 0) > 0.0) {
        return refuse("le rectangle a des coins arrondis");
    }
    Layer group;
    if (groupTransform != nullptr) {
        group.anchor =
            GroupProperty(groupTransform, "ADBE Vector Anchor", true);
        group.position =
            GroupProperty(groupTransform, "ADBE Vector Position", true);
        group.scale = GroupProperty(groupTransform, "ADBE Vector Scale");
        group.rotation = GroupProperty(groupTransform, "ADBE Vector Rotation");
    }

    const int64_t first = offset;
    const int64_t last = std::max<int64_t>(duration_, first + 1);
    std::vector<std::string> polygons;
    for (int64_t frame = first; frame < last; ++frame) {
        const std::array<double, 6> matteMatrix = LayerMatrix(*matte, frame);
        std::array<double, 6> groupMatrix = {1, 0, 0, 1, 0, 0};
        if (groupTransform != nullptr) {
            const std::vector<double> a =
                ValueAt(group.anchor, frame, timebase_);
            const std::vector<double> p =
                ValueAt(group.position, frame, timebase_);
            const std::vector<double> s =
                ValueAt(group.scale, frame, timebase_);
            const double r =
                Component(ValueAt(group.rotation, frame, timebase_), 0);
            groupMatrix = TransformMatrix(Component(a, 0), Component(a, 1),
                                          Component(p, 0), Component(p, 1),
                                          Component(s, 0, 100.0) / 100.0,
                                          Component(s, 1, 100.0) / 100.0, r);
        }
        const std::array<double, 6> toLayer =
            Multiply(Invert(LayerMatrix(layer, frame)),
                     Multiply(matteMatrix, groupMatrix));
        const std::vector<double> s = ValueAt(size, frame, timebase_);
        const std::vector<double> c = ValueAt(center, frame, timebase_);
        const double hw = Component(s, 0) / 2.0;
        const double hh = Component(s, 1) / 2.0;
        const double corners[4][2] = {
            {Component(c, 0) - hw, Component(c, 1) - hh},
            {Component(c, 0) + hw, Component(c, 1) - hh},
            {Component(c, 0) + hw, Component(c, 1) + hh},
            {Component(c, 0) - hw, Component(c, 1) + hh}};
        std::string polygon = "polygon(";
        for (int corner = 0; corner < 4; ++corner) {
            const double x = corners[corner][0];
            const double y = corners[corner][1];
            const double lx = toLayer[0] * x + toLayer[2] * y + toLayer[4];
            const double ly = toLayer[1] * x + toLayer[3] * y + toLayer[5];
            polygon += (corner == 0 ? "" : ", ") + Px(lx - boxLeft) + " " +
                       Px(ly - boxTop);
        }
        polygons.push_back(polygon + ")");
    }
    // One key per frame where the clip changes, linear between.
    std::vector<FrameNumber> frames;
    std::vector<Value> values;
    const auto count = static_cast<int64_t>(polygons.size());
    for (int64_t index = 0; index < count; ++index) {
        const bool changesBefore =
            index > 0 && polygons[index - 1] != polygons[index];
        const bool changesAfter =
            index + 1 < count && polygons[index + 1] != polygons[index];
        if (index == 0 || index + 1 == count || changesBefore || changesAfter) {
            frames.push_back({first + index - offset, 1});
            values.push_back(
                Value::MakeString(polygons[static_cast<size_t>(index)]));
        }
    }
    if (frames.size() <= 2 && polygons.front() == polygons.back()) {
        style.Set("clipPath", Value::MakeString(polygons.front()));
    } else {
        std::vector<std::string> easings(frames.size() - 1, "linear");
        animate.Set("clipPath", Animation(frames, values, easings));
    }
    Note(layer.name, "cache de piste de « " + matte->name +
                         " » : découpe lue image par image");
    return true;
}

bool Importer::Run(Value& definition, MotionError& error) {
    const Value* comp = &comp_;
    if (const Value* result = comp->Find("result")) {
        comp = result;
    }
    if (const Value* inner = comp->Find("comp")) {
        comp = inner;
    }
    const Value* layers = comp->Find("layers");
    const double width = Number(comp->Find("width"));
    const double height = Number(comp->Find("height"));
    if (layers == nullptr || !layers->IsArray() || width <= 0.0 ||
        height <= 0.0) {
        error = {"InvalidValue", "comp",
                 "not an after-comp.v1 reading (read_comp): no width, height "
                 "or layers"};
        return false;
    }
    report_.comp_name = Text(comp->Find("name"));
    timebase_ = TimebaseOf(Number(comp->Find("frame_duration")),
                           Number(comp->Find("frame_rate"), 25.0));
    duration_ = static_cast<int64_t>(
        std::llround(Number(comp->Find("duration")) * timebase_.Fps()));
    if (duration_ <= 0) {
        duration_ = 1;
    }
    if (Flag(comp->Find("motion_blur"))) {
        Note(report_.comp_name,
             "flou de mouvement de la composition : non traduit");
    }

    for (const Value& raw : layers->AsArray()) {
        Layer layer;
        layer.raw = &raw;
        layer.id = Text(raw.Find("id"));
        layer.index = static_cast<int>(Number(raw.Find("index")));
        layer.name = Text(raw.Find("name"));
        layer.kind = Text(raw.Find("kind"));
        layer.parent_id = Text(raw.Find("parent_id"));
        layer.width = Number(raw.Find("width"), width);
        layer.height = Number(raw.Find("height"), height);
        const Value* transform = raw.Find("transform");
        layer.anchor = GroupProperty(transform, "ADBE Anchor Point", true);
        layer.position = GroupProperty(transform, "ADBE Position", true);
        layer.position_x = GroupProperty(transform, "ADBE Position_0");
        layer.position_y = GroupProperty(transform, "ADBE Position_1");
        layer.separated =
            layer.position_x.Animated() || layer.position_y.Animated();
        layer.scale = GroupProperty(transform, "ADBE Scale");
        layer.rotation = GroupProperty(transform, "ADBE Rotate Z");
        layer.opacity = GroupProperty(transform, "ADBE Opacity");
        layers_.push_back(std::move(layer));
    }
    report_.layers = static_cast<int>(layers_.size());

    Value children = Value::MakeArray();
    // After's first layer is on top: the format paints in order, so the
    // last layer comes first.
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        const Layer& layer = *it;
        const Value& raw = *layer.raw;
        if (!Flag(raw.Find("enabled"))) {
            if (!Flag(raw.Find("is_track_matte"))) {
                Note(layer.name, "calque éteint : ignoré");
            }
            continue;
        }
        if (Flag(raw.Find("guide"))) {
            Note(layer.name, "calque guide : ignoré, After ne le rend pas");
            continue;
        }
        if (layer.kind == "null") {
            continue;  // drawn nowhere; its transform reaches its children
        }
        if (layer.kind == "camera" || layer.kind == "light" ||
            layer.kind == "adjustment") {
            Note(layer.name, "calque " + layer.kind + " : non traduit");
            continue;
        }
        const Value* source = raw.Find("source");
        const std::string sourceKind = Text(Field(source, "kind"));
        if (layer.kind == "av" && sourceKind != "solid") {
            Note(layer.name, sourceKind == "comp"
                                 ? "précomposition « " +
                                       Text(Field(source, "name")) +
                                       " » : non traduite (à importer à part)"
                                 : "métrage « " + Text(Field(source, "name")) +
                                       " » : non traduit");
            continue;
        }
        if (Flag(raw.Find("three_d"))) {
            Note(layer.name, "calque 3D : posé à plat");
        }
        if (Flag(raw.Find("motion_blur")) && Flag(comp->Find("motion_blur"))) {
            Note(layer.name, "flou de mouvement : non traduit");
        }
        if (Text(raw.Find("blending_mode")) != "NORMAL" &&
            !Text(raw.Find("blending_mode")).empty()) {
            Note(layer.name, "mode de fusion " +
                                 Text(raw.Find("blending_mode")) +
                                 " : non traduit");
        }
        if (Flag(raw.Find("styles"))) {
            Note(layer.name, "styles de calque : non traduits");
        }
        if (Flag(raw.Find("time_remap"))) {
            Note(layer.name, "remappage temporel : non traduit");
        }
        if (std::fabs(Number(raw.Find("stretch"), 100.0) - 100.0) > 1e-9) {
            Note(layer.name, "étirement temporel : non traduit");
        }
        if (const Value* masks = raw.Find("masks"); !Groups(masks).empty()) {
            Note(layer.name, "masques : non traduits");
        }

        // The layer's span: a sequence when it does not cover the whole
        // composition, its keys then in the sequence's own time.
        const FrameNumber in = FrameOf(Number(raw.Find("in_point")), timebase_);
        const FrameNumber out =
            FrameOf(Number(raw.Find("out_point")), timebase_);
        const auto inFrame = static_cast<int64_t>(std::ceil(in.Value() - 1e-9));
        const auto outFrame =
            static_cast<int64_t>(std::ceil(out.Value() - 1e-9));
        const bool windowed = inFrame > 0 || outFrame < duration_;
        const int64_t offset = windowed ? std::max<int64_t>(inFrame, 0) : 0;

        Value node;
        Value style;
        Value shapes = Value::MakeArray();
        Value animate = Value::MakeObject();
        double boxLeft = 0.0;
        double boxTop = 0.0;
        if (layer.kind == "text") {
            if (!TextNode(layer, node, boxLeft, boxTop, offset)) {
                continue;
            }
            style = *node.Find("style");
        } else {
            node = Value::MakeObject();
            node.Set("type", Value::MakeString("box"));
            node.Set("id", Value::MakeString(UniqueId(layer.name)));
            style = Value::MakeObject();
            style.Set("position", Value::MakeString("absolute"));
            style.Set("left", Value::MakeInt(0));
            style.Set("top", Value::MakeInt(0));
            style.Set("width", Num(layer.width));
            style.Set("height", Num(layer.height));
            if (sourceKind == "solid") {
                style.Set("backgroundColor", Value::MakeString(HexColor(Numbers(
                                                 Field(source, "color")))));
            }
            if (layer.kind == "shape") {
                const std::optional<std::vector<double>> fill =
                    FillEffect(layer);
                if (!ShapeChildren(layer, raw.Find("contents"), fill, shapes,
                                   offset, ShapePaint())) {
                    continue;
                }
            } else {
                FillEffect(layer);
            }
        }
        SetTransform(layer, boxLeft, boxTop, style, animate, offset, true);
        MatteClip(layer, boxLeft, boxTop, style, animate, offset);
        node.Set("style", std::move(style));
        if (!animate.AsObject().empty()) {
            node.Set("animate", std::move(animate));
        }
        if (layer.kind == "shape") {
            node.Set("children", std::move(shapes));
        }
        ++report_.nodes;

        // Parents, innermost first: each a box carrying its transform (no
        // opacity: After does not hand it down).
        std::string parentId = layer.parent_id;
        int depth = 0;
        while (!parentId.empty() && depth++ < 32) {
            const Layer* parent = FindLayer(parentId);
            if (parent == nullptr) {
                break;
            }
            Value wrapper = Value::MakeObject();
            wrapper.Set("type", Value::MakeString("box"));
            wrapper.Set("id", Value::MakeString(
                                  UniqueId(parent->name + "#" + layer.name)));
            Value wrapperStyle = Value::MakeObject();
            Value wrapperAnimate = Value::MakeObject();
            wrapperStyle.Set("position", Value::MakeString("absolute"));
            wrapperStyle.Set("left", Value::MakeInt(0));
            wrapperStyle.Set("top", Value::MakeInt(0));
            wrapperStyle.Set("width", Num(parent->width));
            wrapperStyle.Set("height", Num(parent->height));
            SetTransform(*parent, 0.0, 0.0, wrapperStyle, wrapperAnimate,
                         offset, false);
            wrapper.Set("style", std::move(wrapperStyle));
            if (!wrapperAnimate.AsObject().empty()) {
                wrapper.Set("animate", std::move(wrapperAnimate));
            }
            Value inner = Value::MakeArray();
            inner.Push(std::move(node));
            wrapper.Set("children", std::move(inner));
            node = std::move(wrapper);
            ++report_.nodes;
            parentId = parent->parent_id;
        }
        if (windowed) {
            Value sequence = Value::MakeObject();
            sequence.Set("type", Value::MakeString("sequence"));
            sequence.Set("id",
                         Value::MakeString(UniqueId(layer.name + "#plage")));
            sequence.Set("from", Value::MakeInt(offset));
            sequence.Set("duration",
                         Value::MakeInt(std::max<int64_t>(
                             1, std::min(outFrame, duration_) - offset)));
            Value inner = Value::MakeArray();
            inner.Push(std::move(node));
            sequence.Set("children", std::move(inner));
            node = std::move(sequence);
            ++report_.nodes;
        }
        children.Push(std::move(node));
    }

    definition = Value::MakeObject();
    definition.Set("width", Num(width));
    definition.Set("height", Num(height));
    if (timebase_.denominator == 1) {
        definition.Set("fps", Value::MakeInt(timebase_.numerator));
    } else {
        definition.Set(
            "fps", Value::MakeString(std::to_string(timebase_.numerator) + "/" +
                                     std::to_string(timebase_.denominator)));
    }
    definition.Set("duration", Value::MakeInt(duration_));
    Value root = Value::MakeObject();
    root.Set("type", Value::MakeString("fill"));
    root.Set("children", std::move(children));
    definition.Set("root", std::move(root));

    // What the import wrote must be a composition: a refusal here is a bug
    // of the import, said as such rather than handed to the document.
    MotionComposition parsed;
    MotionError parseError;
    if (!ParseMotionComposition(definition, parsed, parseError)) {
        error = {"InvalidValue", parseError.path,
                 "the imported composition does not parse (" + parseError.code +
                     ": " + parseError.message + ")"};
        return false;
    }
    return true;
}

}  // namespace

bool ImportAfterComposition(const Value& comp,
                            const MotionAfterImportOptions& options,
                            const MotionAfterFontResolver& fonts,
                            MotionMeasurer& measurer, Value& definition,
                            MotionAfterImportReport& report,
                            MotionError& error) {
    report = {};
    Importer importer(comp, options, fonts, measurer, report);
    return importer.Run(definition, error);
}

void MotionAfterImportReportFields(const MotionAfterImportReport& report,
                                   Value& result) {
    result.Set("after_comp", Value::MakeString(report.comp_name));
    result.Set("layers", Value::MakeInt(report.layers));
    result.Set("nodes", Value::MakeInt(report.nodes));
    result.Set("keyframes", Value::MakeInt(report.keys));
    Value notes = Value::MakeArray();
    for (const MotionAfterImportNote& note : report.notes) {
        Value item = Value::MakeObject();
        item.Set("layer", Value::MakeString(note.layer));
        item.Set("note", Value::MakeString(note.note));
        notes.Push(std::move(item));
    }
    result.Set("notes", std::move(notes));
}
