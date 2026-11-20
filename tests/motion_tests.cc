// MOTION-2026-09 M1 -- ROADMAP.md. Portable: fake text metrics stand in for
// Core Text, so every expected box below is computed by hand.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionGeometry.h"
#include "MotionSpring.h"
#include "MotionVideo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include "TestSupport.h"

namespace {

bool Near(double left, double right, double tolerance = 1e-6) {
    return std::fabs(left - right) <= tolerance;
}

// Every code point advances half the font size plus the letter spacing (plus
// the word spacing for a space); a line is 1.2x the font size unless the
// style resolves a line height. Lines break only on '\n'. Each code point is
// a cluster. "Missing Sans" does not exist.
class FakeMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override {
        ++text_calls;
        if (style.font_family == "Missing Sans") {
            error = {"FontMissing", "", "font 'Missing Sans' is not installed"};
            return false;
        }
        const double lineHeight =
            style.line_height > 0.0 ? style.line_height : style.font_size * 1.2;
        const double advance = style.font_size * 0.5 + style.letter_spacing;
        layout = MotionTextLayout{};
        std::stringstream stream(text);
        std::string line;
        std::vector<std::pair<std::string, double>> lines;
        std::vector<std::vector<MotionTextCluster>> clusters;
        while (std::getline(stream, line, '\n')) {
            clusters.emplace_back();
            double x = 0.0;
            for (size_t index = 0; index < line.size(); ++index) {
                if ((static_cast<unsigned char>(line[index]) & 0xC0) == 0x80) {
                    clusters.back().back().length += 1;
                    continue;
                }
                const double width =
                    advance + (line[index] == ' ' ? style.word_spacing : 0.0);
                clusters.back().push_back({index, 1, x, width});
                x += width;
            }
            lines.emplace_back(line, x);
            layout.width = std::max(layout.width, lines.back().second);
        }
        const double frame = max_width >= 0.0 ? max_width : layout.width;
        for (size_t index = 0; index < lines.size(); ++index) {
            MotionTextLine placed;
            placed.text = lines[index].first;
            placed.clusters = clusters[index];
            placed.width = lines[index].second;
            placed.height = lineHeight;
            placed.y = lineHeight * static_cast<double>(index);
            placed.x = style.text_align == "center"
                           ? (frame - placed.width) / 2.0
                       : style.text_align == "right" ? frame - placed.width
                                                     : 0.0;
            placed.baseline = placed.y + style.font_size * 0.8;
            layout.lines.push_back(placed);
        }
        layout.height = lineHeight * static_cast<double>(lines.size());
        return true;
    }

    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override {
        if (source != "logo.png") {
            error = {"MediaMissing", "", "no media '" + source + "'"};
            return false;
        }
        width = 400.0;
        height = 200.0;
        return true;
    }

    int text_calls = 0;
};

bool Parse(const std::string& json, MotionComposition& composition,
           const std::string& label) {
    MotionError error;
    const bool ok = ParseMotionComposition(json, composition, error);
    Check(ok, label + " parses: " + error.code + " at " + error.path + " -- " +
                  error.message);
    return ok;
}

bool Evaluate(const MotionComposition& composition, int64_t frame,
              MotionFrame& output, const std::string& props = "null") {
    mcp_json::Value overrides;
    std::string parseError;
    mcp_json::Value::Parse(props, overrides, parseError);
    FakeMeasurer measurer;
    MotionError error;
    const bool ok = EvaluateMotionFrame(composition, overrides, frame, measurer,
                                        output, error);
    Check(ok, "frame " + std::to_string(frame) + " evaluates: " + error.code +
                  " at " + error.path + " -- " + error.message);
    return ok;
}

MotionError EvaluateError(const MotionComposition& composition, int64_t frame,
                          const std::string& props = "null") {
    mcp_json::Value overrides;
    std::string parseError;
    mcp_json::Value::Parse(props, overrides, parseError);
    FakeMeasurer measurer;
    MotionFrame output;
    MotionError error;
    if (EvaluateMotionFrame(composition, overrides, frame, measurer, output,
                            error)) {
        error = {"NoError", "", ""};
    }
    return error;
}

MotionError ParseError(const std::string& json) {
    MotionComposition composition;
    MotionError error;
    if (ParseMotionComposition(json, composition, error)) {
        error = {"NoError", "", ""};
    }
    return error;
}

const MotionEvaluatedNode& Child(const MotionEvaluatedNode& node,
                                 std::initializer_list<size_t> path) {
    const MotionEvaluatedNode* current = &node;
    for (size_t index : path) {
        if (index >= current->children.size()) {
            static MotionEvaluatedNode missing;
            ++failures;
            std::cerr << "FAIL: missing child " << index << " under "
                      << current->path << '\n';
            return missing;
        }
        current = &current->children[index];
    }
    return *current;
}

bool BoxIs(const MotionEvaluatedNode& node, double x, double y, double width,
           double height) {
    return Near(node.box.x, x) && Near(node.box.y, y) &&
           Near(node.box.width, width) && Near(node.box.height, height);
}

std::string Describe(const MotionEvaluatedNode& node) {
    std::ostringstream text;
    text << node.path << " box [" << node.box.x << ", " << node.box.y << ", "
         << node.box.width << ", " << node.box.height << "]";
    return text.str();
}

void TestSpring() {
    MotionSpring spring;
    Check(Near(EvaluateMotionSpring(spring, {0, 25}), 0.0),
          "a spring starts at from");
    Check(Near(EvaluateMotionSpring(spring, {2, 1}), 1.0, 1e-3),
          "Remotion's default spring settles on its target within two seconds");
    double peak = 0.0;
    for (int frame = 0; frame <= 50; ++frame) {
        peak = std::max(peak, EvaluateMotionSpring(spring, {frame, 25}));
    }
    Check(peak > 1.05, "the default underdamped spring overshoots");

    spring.overshoot_clamping = true;
    double previous = 0.0;
    bool monotonic = true;
    bool bounded = true;
    for (int frame = 0; frame <= 50; ++frame) {
        const double value = EvaluateMotionSpring(spring, {frame, 25});
        monotonic = monotonic && value >= previous;
        bounded = bounded && value <= 1.0;
        previous = value;
    }
    Check(monotonic && bounded && previous == 1.0,
          "overshootClamping stops at the target and never passes it");

    MotionSpring plain;
    Check(EvaluateMotionSpring(plain, {25, 25}) ==
              EvaluateMotionSpring(plain, {50, 50}),
          "the same instant gives the same value at 25 and 50 fps");

    for (double damping : {20.0, 40.0}) {
        MotionSpring heavy;
        heavy.damping = damping;
        double last = 0.0;
        bool clean = true;
        for (int frame = 0; frame <= 75; ++frame) {
            const double value = EvaluateMotionSpring(heavy, {frame, 25});
            clean = clean && value >= last - 1e-12 && value <= 1.0 + 1e-12;
            last = value;
        }
        Check(clean,
              "a critically or over-damped spring rises without "
              "overshoot (damping " +
                  std::to_string(damping) + ")");
    }

    std::string reason;
    MotionSpring invalid;
    invalid.mass = 0.0;
    Check(!ValidateMotionSpring(invalid, reason) &&
              reason.find("mass") != std::string::npos,
          "a massless spring is refused");
}

void TestTitleCardLayout() {
    MotionComposition composition;
    if (!Parse(R"({
      "width": 1080, "height": 1920, "fps": 25, "duration": 95,
      "root": {"type": "fill", "style": {"justifyContent": "center", "alignItems": "center"},
        "children": [
          {"type": "box", "style": {"flexDirection": "column", "alignItems": "center", "gap": 78,
                                    "fontFamily": "Fira Sans Condensed", "fontSize": 62, "lineHeight": "72px"},
           "children": [
             {"type": "text", "id": "title", "content": "AB\nCD\nEF"},
             {"type": "text", "id": "date", "content": "DATE"}]}]}})",
               composition, "title card")) {
        return;
    }
    MotionFrame frame;
    if (!Evaluate(composition, 0, frame)) {
        return;
    }
    // Title 2 x 31 wide and 3 x 72 tall; date 4 x 31 by 72. The column is
    // 124 x (216 + 78 + 72) = 366, centred in 1080 x 1920.
    const MotionEvaluatedNode& column = Child(frame.root, {0});
    const MotionEvaluatedNode& title = Child(frame.root, {0, 0});
    const MotionEvaluatedNode& date = Child(frame.root, {0, 1});
    Check(BoxIs(frame.root, 0, 0, 1080, 1920), "the fill covers the frame");
    Check(
        BoxIs(column, 478, 777, 124, 366),
        "the column is centred from its measured content: " + Describe(column));
    Check(BoxIs(title, 509, 777, 62, 216),
          "the title is centred in the column: " + Describe(title));
    Check(BoxIs(date, 478, 1071, 124, 72),
          "the gap separates the date from the title: " + Describe(date));
    Check(title.text_style.font_family == "Fira Sans Condensed" &&
              date.text_style.font_size == 62.0 &&
              title.text_style.line_height == 72.0,
          "typography is inherited from the enclosing box");
    Check(title.text_layout.lines.size() == 3 &&
              Near(title.text_layout.lines[2].y, 144.0),
          "line positions are exposed, relative to the text box");
}

void TestFlexboxCases() {
    MotionComposition composition;
    if (!Parse(R"({
      "width": 1080, "height": 1920, "fps": 25, "duration": 10,
      "root": {"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 62, "lineHeight": "72px"},
        "children": [
          {"type": "box", "style": {"width": 1000, "padding": "0 50px", "justifyContent": "space-between"},
           "children": [{"type": "text", "content": "A"}, {"type": "text", "content": "B"}, {"type": "text", "content": "C"}]},
          {"type": "box", "style": {"width": "50%", "padding": "10%", "justifyContent": "flex-end"},
           "children": [{"type": "text", "content": "Z"}]},
          {"type": "box", "style": {"position": "absolute", "top": 100, "left": "10%", "width": 20, "height": 30}},
          {"type": "image", "src": "logo.png", "style": {"position": "absolute", "right": 0, "bottom": 0, "width": 200}}]}})",
               composition, "flexbox cases")) {
        return;
    }
    MotionFrame frame;
    if (!Evaluate(composition, 0, frame)) {
        return;
    }
    // Row with 900 px of content and three 31 px items: 807 px shared in
    // two gaps of 403.5.
    const MotionEvaluatedNode& row = Child(frame.root, {0});
    Check(BoxIs(Child(row, {0}), 50, 0, 31, 72) &&
              BoxIs(Child(row, {1}), 484.5, 0, 31, 72) &&
              BoxIs(Child(row, {2}), 919, 0, 31, 72),
          "space-between distributes the row inside its padding: " +
              Describe(Child(row, {1})));
    Check(Child(row, {0}).box.height == row.box.height,
          "items stretch across a row by default, as in CSS");

    // 50% of 1080 is 540; 10% padding is 108 on every side, against the
    // containing block's width.
    const MotionEvaluatedNode& half = Child(frame.root, {1});
    Check(BoxIs(half, 0, 72, 540, 288),
          "percentage width and padding resolve against the parent: " +
              Describe(half));
    Check(BoxIs(Child(half, {0}), 401, 180, 31, 72),
          "flex-end packs to the inner right edge: " +
              Describe(Child(half, {0})));

    Check(BoxIs(Child(frame.root, {2}), 108, 100, 20, 30),
          "absolute insets resolve pixels and percentages: " +
              Describe(Child(frame.root, {2})));
    Check(BoxIs(Child(frame.root, {3}), 880, 1820, 200, 100),
          "an image given a width keeps its aspect ratio: " +
              Describe(Child(frame.root, {3})));
}

// R5 -- docs/roadmap/REEL-2026-09.md. The flash, as the two cut-down
// versions of the film actually made it and as R5 decided to leave it: a
// flat colour on a track for four frames, at a falling opacity. This is
// here to be run, not to be believed: it is the whole of the effect a
// "flash" transition type would have added, and it needs no type, no
// validation rule, no export path and no Resolve mapping of its own.
//
// It also does what a transition could not. The colour is a parameter, so
// the red flash of the third version is the same composition with another
// value; the curve is the motion vocabulary's, so a flash can ease; and
// the clip sits where the music puts it -- both flashes of the measured
// films land on a drum hit, not on a cut, which is the one place a
// transition of a cut cannot go.
// MOTION-WEB-2026-10 MW1 -- a key between two frames, written as an exact
// fraction. A web animation timed at 0.1 s at 25 fps keys frame 5/2.
void TestKeysBetweenFrames() {
    const auto card = [](const std::string& frames, const std::string& values) {
        return R"({"width": 100, "height": 100, "fps": 25, "duration": 20,
          "root": {"type": "fill", "children": [{"type": "box", "id": "dot",
            "animate": {"translateX": {"interpolate": {
              "frames": )" +
               frames + R"(, "values": )" + values + "}}}}]}}";
    };
    const auto at = [](const MotionComposition& composition, int64_t frame) {
        MotionFrame output;
        return Evaluate(composition, frame, output)
                   ? Child(output.root, {0}).translate_x
                   : std::nan("");
    };

    // Halfway to frame 5/2 is frame 5/4: the same straight line as a key at
    // frame 5 with twice the value, at every frame the line covers.
    MotionComposition between;
    MotionComposition whole;
    if (Parse(card(R"([0, "5/2"])", "[0, 5]"), between, "a key at 5/2") &&
        Parse(card("[0, 5]", "[0, 10]"), whole, "a key at 5")) {
        Check(at(between, 1) == at(whole, 1) && at(between, 2) == at(whole, 2),
              "a key at 5/2 is reached at frame 2.5, exactly");
        Check(at(between, 3) == 5.0,
              "a key between frames holds after it like any other");
        const std::vector<Keyframe>& keys =
            between.root.children[0].animate[0].second.track.keyframes();
        Check(keys.size() == 2 && keys[1].time == RationalTime(1, 10),
              "frame 5/2 at 25 fps is the exact time 1/10 s");
    }

    // At 30000/1001 the same key is still exact: nothing is rounded.
    MotionComposition ntsc;
    if (Parse(R"({"width": 10, "height": 10, "fps": "30000/1001",
                  "duration": 10, "root": {"type": "fill", "animate": {
                    "opacity": {"interpolate": {"frames": [0, "7/3"],
                                                "values": [0, 1]}}}}})",
              ntsc, "a fraction at 29.97")) {
        const std::vector<Keyframe>& keys =
            ntsc.root.animate[0].second.track.keyframes();
        Check(keys.size() == 2 &&
                  keys[1].time.compare(RationalTime(7007, 90000)) == 0,
              "frame 7/3 at 30000/1001 is 7007/90000 s");
    }

    // One spelling per key: an integer, or a reduced fraction "n/d".
    const auto refused = [&](const std::string& frames) {
        const MotionError error = ParseError(card(frames, "[0, 1]"));
        return error.code == "InvalidAnimation" &&
               error.path ==
                   "root.children[0].animate.translateX.interpolate.frames";
    };
    Check(refused("[0, 2.5]"), "a decimal frame is refused: write \"5/2\"");
    Check(refused(R"([0, "4/2"])"), "an unreduced fraction is refused");
    Check(refused(R"([0, "5/1"])"), "a whole fraction is written as 5");
    Check(refused(R"([0, "5/0"])") && refused(R"([0, "/2"])") &&
              refused(R"([0, "05/2"])") && refused(R"([0, "+5/2"])") &&
              refused(R"([0, "5 / 2"])"),
          "a malformed fraction is refused");
    Check(refused(R"(["5/2", 2])") && refused(R"(["5/2", "5/2"])"),
          "fractions keep keys strictly increasing");
    MotionComposition negative;
    Check(Parse(card(R"(["-5/2", 0])", "[0, 1]"), negative,
                "a key before the start"),
          "a fraction may be negative, like an integer frame");
    Check(ParseError(card(R"([0, "5/2"])", "[0, 1]")).message.empty() &&
              ParseError(card("[0, 2.5]", "[0, 1]")).message.find("\"21/2\"") !=
                  std::string::npos,
          "the refusal shows how to write a fraction");
}

// MOTION-WEB-2026-10 MW3 -- GSAP's named curves. The expected values are
// GSAP 3.14.2's own (gsap.parseEase in Node), at progress 0.1, 0.25, 0.5,
// 0.75 and 0.9; cos and pow may differ in their last bits between libms.
void TestGsapCurves() {
    const std::pair<const char*, std::array<double, 5>> gsap[] = {
        {"power1.in", {0.010000000000000002, 0.0625, 0.25, 0.5625, 0.81}},
        {"power1.out", {0.18999999999999995, 0.4375, 0.75, 0.9375, 0.99}},
        {"power1.inOut", {0.020000000000000004, 0.125, 0.5, 0.875, 0.98}},
        {"power2.in",
         {0.0010000000000000002, 0.015625, 0.125, 0.421875,
          0.7290000000000001}},
        {"power2.out", {0.2709999999999999, 0.578125, 0.875, 0.984375, 0.999}},
        {"power2.inOut", {0.004000000000000001, 0.0625, 0.5, 0.9375, 0.996}},
        {"power3.in",
         {0.00010000000000000002, 0.00390625, 0.0625, 0.31640625,
          0.6561000000000001}},
        {"power3.out",
         {0.3438999999999999, 0.68359375, 0.9375, 0.99609375, 0.9999}},
        {"power3.inOut",
         {0.0008000000000000001, 0.03125, 0.5, 0.96875, 0.9992}},
        {"power4.in",
         {1.0000000000000003e-05, 0.0009765625, 0.03125, 0.2373046875,
          0.5904900000000001}},
        {"power4.out",
         {0.40950999999999993, 0.7626953125, 0.96875, 0.9990234375, 0.99999}},
        {"power4.inOut",
         {0.00016000000000000004, 0.015625, 0.5, 0.984375, 0.99984}},
        {"sine.in",
         {0.01231165940486223, 0.07612046748871326, 0.2928932188134524,
          0.6173165676349102, 0.843565534959769}},
        {"sine.out",
         {0.15643446504023095, 0.38268343236508984, 0.7071067811865476,
          0.9238795325112867, 0.9876883405951378}},
        {"sine.inOut",
         {0.024471741852423234, 0.1464466094067262, 0.5, 0.8535533905932737,
          0.9755282581475768}},
        {"expo.in",
         {0.0001962125, 0.0015641734007549755, 0.0234375, 0.17707715037872765,
          0.5031441000000001}},
        {"expo.out",
         {0.4968558999999999, 0.8229228496212724, 0.9765625, 0.998435826599245,
          0.9998037875}},
        {"expo.inOut",
         {0.00041622500000000005, 0.01171875, 0.5, 0.98828125, 0.999583775}},
        {"circ.in",
         {0.005012562893380035, 0.031754163448145745, 0.1339745962155614,
          0.3385621722338523, 0.5641101056459328}},
        {"circ.out",
         {0.4358898943540672, 0.6614378277661477, 0.8660254037844386,
          0.9682458365518543, 0.99498743710662}},
        {"circ.inOut",
         {0.010102051443364402, 0.0669872981077807, 0.5, 0.9330127018922193,
          0.9898979485566356}},
        {"back.in",
         {-0.014314219999999933, -0.0641365625000001, -0.08769749999999998,
          0.1825903124999999, 0.5911720199999999}},
        {"back.out",
         {0.4088279800000001, 0.8174096875000001, 1.0876975, 1.0641365625,
          1.01431422}},
        {"back.inOut",
         {-0.023225280000000015, -0.04384874999999999, 0.5, 1.04384875,
          1.0232252800000001}},
        {"back.out(1.7)",
         {0.40869999999999984, 0.8171874999999997, 1.0875, 1.0640625, 1.0143}},
        {"back.in(2.4)",
         {-0.02059999999999995, -0.09687500000000004, -0.17500000000000004,
          0.08437499999999998, 0.5346000000000002}},
        {"back.inOut(1.2)",
         {-0.015199999999999991, -0.012499999999999956, 0.5, 1.0125, 1.0152}},
    };
    const double progress[] = {0.1, 0.25, 0.5, 0.75, 0.9};
    for (const auto& [name, expected] : gsap) {
        MotionNamedCurve curve;
        KeyframeEasing approximation;
        if (!ParseMotionNamedCurve(name, curve, approximation)) {
            Check(false, std::string("GSAP curve read: ") + name);
            continue;
        }
        double worst = 0.0;
        for (size_t index = 0; index < 5; ++index) {
            worst = std::max(worst, std::fabs(EvaluateMotionNamedCurve(
                                                  curve, progress[index]) -
                                              expected[index]));
        }
        Check(worst <= 1e-12, std::string(name) + " is GSAP's curve, off by " +
                                  std::to_string(worst));
        Check(EvaluateMotionNamedCurve(curve, 0.0) == 0.0 &&
                  EvaluateMotionNamedCurve(curve, 1.0) == 1.0,
              std::string(name) + " starts at 0 and lands on 1");
    }

    const auto card = [](const std::string& easing) {
        return R"({"width": 100, "height": 100, "fps": 25, "duration": 30,
          "root": {"type": "fill", "children": [{"type": "box", "id": "dot",
            "animate": {"translateX": {"interpolate": {
              "frames": [0, 20], "values": [0, 100], "easing": )" +
               easing + "}}}}]}}";
    };
    const auto at = [](const MotionComposition& composition, int64_t frame) {
        MotionFrame output;
        return Evaluate(composition, frame, output)
                   ? Child(output.root, {0}).translate_x
                   : std::nan("");
    };
    MotionComposition power3;
    if (Parse(card(R"("power3.out")"), power3, "power3.out")) {
        Check(Near(at(power3, 5), 68.359375, 1e-9) &&
                  Near(at(power3, 10), 93.75, 1e-9) && at(power3, 20) == 100.0,
              "an animation at power3.out plays GSAP's formula, not a "
              "Bézier: " +
                  std::to_string(at(power3, 5)));
        const MotionAnimation& animation =
            power3.root.children[0].animate[0].second;
        Check(animation.named_curves.size() == 2 &&
                  !animation.named_curves[0].exact_bezier &&
                  animation.track.keyframes()[0].interpolation ==
                      KeyframeInterpolation::CubicBezier,
              "the track keeps the closest cubic-bezier beside the curve");
    }
    // power2.out is a cubic: its cubic-bezier is the curve itself.
    MotionComposition named;
    MotionComposition bezier;
    if (Parse(card(R"("power2.out")"), named, "power2.out") &&
        Parse(card("\"cubic-bezier(0.3333333333333333, 1, "
                   "0.6666666666666666, 1)\""),
              bezier, "its cubic-bezier")) {
        double worst = 0.0;
        for (int64_t frame = 0; frame <= 20; ++frame) {
            worst = std::max(worst,
                             std::fabs(at(named, frame) - at(bezier, frame)));
        }
        Check(worst < 1e-6 && named.root.children[0]
                                  .animate[0]
                                  .second.named_curves[0]
                                  .exact_bezier,
              "power2.out and its cubic-bezier are one curve: " +
                  std::to_string(worst));
    }
    // A curve per segment mixes GSAP names with the CSS ones.
    MotionComposition mixed;
    if (Parse(R"({"width": 10, "height": 10, "fps": 25, "duration": 30,
                  "root": {"type": "fill", "animate": {"opacity": {
                    "interpolate": {"frames": [0, 10, 20],
                                    "values": [0, 1, 0],
                                    "easing": ["expo.out", "step-end"]}}}}})",
              mixed, "a GSAP curve then a held segment")) {
        MotionFrame output;
        // An animated value reaches the node through Value::MakeDouble,
        // which keeps six decimals (std::to_string): 1e-6, not 1e-12.
        const bool early = Evaluate(mixed, 5, output);
        const double five = output.root.opacity;
        const bool late = Evaluate(mixed, 15, output);
        Check(early && late && Near(five, 0.9765625, 1e-6) &&
                  output.root.opacity == 1.0,
              "expo.out leads the first segment, step-end holds the second: " +
                  std::to_string(five) + ", " +
                  std::to_string(output.root.opacity));
    }
    // Keys between frames whose rates have a common multiple beyond int32_t
    // (100000 and 817, at 30 fps): the curve is evaluated, nothing throws.
    MotionComposition far;
    if (Parse(R"({"width": 10, "height": 10, "fps": 30, "duration": 100,
                  "root": {"type": "fill", "animate": {"opacity": {
                    "interpolate": {"frames": ["5755653/100000", "63750/817"],
                                    "values": [0, 1],
                                    "easing": "power2.out"}}}}})",
              far, "keys at /100000 and /817")) {
        MotionFrame output;
        Check(Evaluate(far, 70, output) && output.root.opacity > 0.0 &&
                  output.root.opacity < 1.0,
              "a GSAP curve between keys at /100000 and /817 evaluates");
    }
    for (const char* refused :
         {"\"elastic.out\"", "\"bounce.out\"", "\"power5.out\"",
          "\"power2.sideways\"", "\"sine.out(2)\"", "\"back.out(-1)\"",
          "\"back.out(11)\"", "\"Power2.easeOut\""}) {
        const MotionError error = ParseError(card(refused));
        Check(error.code == "InvalidAnimation" &&
                  error.message.find("power1 to power4") != std::string::npos,
              std::string("refused, naming what is read: ") + refused);
    }
}

void TestFlashAsAClip() {
    MotionComposition composition;
    if (!Parse(R"({
      "width": 100, "height": 100, "fps": 25, "duration": 4,
      "root": {"type": "fill", "style": {"backgroundColor": "#FFFFFF"},
       "animate": {
         "opacity": {"interpolate": {"frames": [0, 4], "values": [1, 0]}}}}})",
               composition, "a four-frame white flash")) {
        return;
    }
    const double expected[4] = {1.0, 0.75, 0.5, 0.25};
    for (int64_t frame = 0; frame < 4; ++frame) {
        MotionFrame output;
        Check(Evaluate(composition, frame, output),
              "the flash evaluates at frame " + std::to_string(frame));
        Check(Near(output.root.opacity, expected[static_cast<size_t>(frame)]),
              "flash frame " + std::to_string(frame) + " is " +
                  std::to_string(output.root.opacity) + ", expected " +
                  std::to_string(expected[static_cast<size_t>(frame)]));
    }
    // The red one of the third version: the same composition, one value.
    MotionComposition red;
    Check(Parse(R"({
      "width": 100, "height": 100, "fps": 25, "duration": 4,
      "root": {"type": "fill", "style": {"backgroundColor": "#E02020"},
       "animate": {
         "opacity": {"interpolate": {"frames": [0, 4], "values": [1, 0]}}}}})",
                red, "a four-frame red flash"),
          "a flash through another colour is the same composition");
}

void TestSequencesAndAnimation() {
    MotionComposition composition;
    if (!Parse(R"({
      "width": 100, "height": 100, "fps": 25, "duration": 60,
      "root": {"type": "fill", "children": [
        {"type": "sequence", "from": 10, "duration": 20, "children": [
          {"type": "sequence", "from": 5, "children": [
            {"type": "box", "id": "dot", "style": {"width": 10, "height": 10},
             "animate": {
               "opacity": {"interpolate": {"frames": [0, 10], "values": [0, 1]}},
               "translateX": {"interpolate": {"frames": [0, 10], "values": [0, 100], "extrapolateRight": "extend"}},
               "translateY": {"interpolate": {"frames": [0, 10], "values": [0, 100]}},
               "scale": {"spring": {"from": 0, "to": 1, "delay": 2, "overshootClamping": true}}}}]}]}]}})",
               composition, "nested sequences")) {
        return;
    }
    const auto dot = [&](int64_t frame,
                         MotionFrame& output) -> const MotionEvaluatedNode* {
        if (!Evaluate(composition, frame, output)) {
            return nullptr;
        }
        const MotionEvaluatedNode* node = &output.root;
        while (!node->children.empty()) {
            node = &node->children[0];
        }
        return node->id == "dot" ? node : nullptr;
    };
    MotionFrame output;
    Check(dot(9, output) == nullptr, "the outer sequence is empty before from");
    Check(dot(12, output) == nullptr,
          "the inner sequence waits for its own from, in local time");
    const MotionEvaluatedNode* start = dot(15, output);
    Check(start != nullptr && start->opacity == 0.0 && start->scale_x == 0.0,
          "local time restarts at zero inside nested sequences");
    MotionFrame middle;
    const MotionEvaluatedNode* half = dot(20, middle);
    Check(half != nullptr && Near(half->opacity, 0.5) &&
              Near(half->translate_x, 50.0),
          "animations read the node's local frame");
    MotionFrame late;
    const MotionEvaluatedNode* past = dot(29, late);
    Check(past != nullptr && Near(past->translate_x, 140.0) &&
              Near(past->translate_y, 100.0),
          "extrapolateRight extend continues the line; clamp holds the end");
    Check(dot(30, output) == nullptr,
          "the outer sequence ends at from + duration");
}

// MOTION-BIBLIOTHEQUE-2026-10 MB5 -- one curve per segment.
void TestEasingPerSegment() {
    const auto card = [](const std::string& easing) {
        return R"({"width": 100, "height": 100, "fps": 25, "duration": 40,
          "root": {"type": "fill", "children": [{"type": "box", "id": "dot",
            "animate": {"translateY": {"interpolate": {
              "frames": [0, 10, 20, 30], "values": [100, 0, 50, -100])" +
               (easing.empty() ? std::string() : ", \"easing\": " + easing) +
               "}}}}]}}";
    };
    const auto track = [](const MotionComposition& composition) {
        return composition.root.children[0].animate[0].second.track.keyframes();
    };
    const auto same = [](const std::vector<Keyframe>& a,
                         const std::vector<Keyframe>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t index = 0; index < a.size(); ++index) {
            if (a[index].time != b[index].time ||
                a[index].value != b[index].value ||
                a[index].interpolation != b[index].interpolation ||
                a[index].easing.x1 != b[index].easing.x1 ||
                a[index].easing.y1 != b[index].easing.y1 ||
                a[index].easing.x2 != b[index].easing.x2 ||
                a[index].easing.y2 != b[index].easing.y2) {
                return false;
            }
        }
        return true;
    };
    const auto at = [&](const MotionComposition& composition, int64_t frame) {
        MotionFrame output;
        return Evaluate(composition, frame, output)
                   ? Child(output.root, {0}).translate_y
                   : std::nan("");
    };

    // The old spelling, and the list that repeats it, are the same track.
    MotionComposition named;
    MotionComposition repeated;
    if (Parse(card(R"("ease-out")"), named, "one name") &&
        Parse(card(R"(["ease-out", "ease-out", "ease-out"])"), repeated,
              "a list repeating one name")) {
        Check(same(track(named), track(repeated)),
              "a list repeating one curve is the track its name gives");
    }

    // An entrance that settles and an exit that accelerates, in one
    // animation: each segment is the animation its own curve would give.
    const std::string entrance = "cubic-bezier(0.16, 1, 0.3, 1)";
    const std::string exit = "cubic-bezier(0.7, 0, 0.84, 0)";
    MotionComposition mixed;
    MotionComposition allEntrance;
    MotionComposition allExit;
    MotionComposition unnamed;
    if (!Parse(card("[\"" + entrance + "\", \"step-end\", \"" + exit + "\"]"),
               mixed, "a curve per segment") ||
        !Parse(card("\"" + entrance + "\""), allEntrance, "the entrance") ||
        !Parse(card("\"" + exit + "\""), allExit, "the exit") ||
        !Parse(card(""), unnamed, "no easing")) {
        return;
    }
    bool entranceSame = true;
    bool held = true;
    for (int64_t frame = 0; frame <= 10; ++frame) {
        entranceSame = entranceSame &&
                       at(mixed, frame) == at(allEntrance, frame) &&
                       at(mixed, 20 + frame) == at(allExit, 20 + frame);
        held = held && at(mixed, 10 + frame) == (frame < 10 ? 0.0 : 50.0);
    }
    Check(entranceSame,
          "each segment follows its own curve, to the same doubles");
    Check(held, "a held segment between them jumps on its last frame");
    Check(at(mixed, 5) < at(unnamed, 5) && at(mixed, 25) > at(unnamed, 25),
          "the entrance runs ahead of a straight line, the exit behind it");
    const std::vector<Keyframe> keys = track(mixed);
    Check(keys.size() == 4 &&
              keys[0].interpolation == KeyframeInterpolation::CubicBezier &&
              keys[1].interpolation == KeyframeInterpolation::Hold &&
              keys[2].interpolation == KeyframeInterpolation::CubicBezier &&
              keys[2].easing.x1 == 0.7 && keys[2].easing.y2 == 0.0 &&
              keys[3].interpolation == keys[2].interpolation &&
              keys[3].easing.x1 == keys[2].easing.x1,
          "a key carries the curve of the segment it starts; the last one, "
          "the curve arriving at it");

    // One key, no segment: an empty list.
    MotionComposition single;
    Parse(R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
        "root": {"type": "fill", "animate": {"opacity": {"interpolate":
          {"frames": [2], "values": [0.5], "easing": []}}}}})",
          single, "one key and an empty list");

    MotionError error = ParseError(card(R"(["ease-out", "ease-in"])"));
    Check(error.code == "InvalidAnimation" &&
              error.path ==
                  "root.children[0].animate.translateY.interpolate.easing" &&
              error.message.find("3 for 4 frames, not 2") != std::string::npos,
          "a list of the wrong length is refused with the count it needs: " +
              error.message);
    error = ParseError(card(R"(["ease-out", "bounce", "ease-in"])"));
    Check(error.code == "InvalidAnimation" &&
              error.path ==
                  "root.children[0].animate.translateY.interpolate.easing[1]",
          "an unknown curve in a list is refused where it is: " + error.path);
    error = ParseError(card(R"(["ease-out", 3, "ease-in"])"));
    Check(error.code == "InvalidAnimation",
          "a curve of a list is a name, not a number");
    error = ParseError(card("3"));
    Check(
        error.code == "InvalidAnimation" &&
            error.message.find("list of one per segment") != std::string::npos,
        "an easing that is neither a name nor a list says both exist");
}

void TestBindings() {
    const std::string json = R"({
      "width": 400, "height": 400, "fps": 25, "duration": 10,
      "props": {
        "palette": {"type": "enum", "values": ["a", "b"]},
        "background": {"type": "bool", "default": true},
        "mention": {"type": "text"},
        "size": {"type": "number", "default": 5, "min": 0, "max": 10}},
      "tokens": {"colors": {"a": "#ff4500", "b": "#5fc3d0"}},
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "box", "id": "panel", "visible": {"prop": "background"},
         "style": {"backgroundColor": {"prop": "palette", "map": "colors"}, "width": 50, "height": 50,
                   "borderRadius": {"prop": "size"}}},
        {"type": "text", "id": "mention", "visible": {"prop": "mention", "notEmpty": true},
         "content": {"prop": "mention"}}]}})";
    MotionComposition composition;
    if (!Parse(json, composition, "bindings")) {
        return;
    }

    MotionFrame frame;
    if (Evaluate(composition, 0, frame)) {
        Check(
            frame.root.children.size() == 1 &&
                frame.root.children[0].has_background &&
                DumpMotionFrame(frame).find(
                    "\"backgroundColor\":\"#ff4500ff\"") != std::string::npos,
            "defaults: first enum value mapped through its token table, empty "
            "mention removed from layout");
    }
    MotionFrame overridden;
    if (Evaluate(
            composition, 0, overridden,
            R"({"palette": "b", "background": false, "mention": "Halloween", "size": 10})")) {
        Check(overridden.root.children.size() == 1 &&
                  overridden.root.children[0].id == "mention" &&
                  overridden.root.children[0].text == "Halloween",
              "a false boolean removes the panel; a non-empty mention appears");
    }
    Check(EvaluateError(composition, 0, R"({"colour": "a"})").code ==
              "UnknownProp",
          "an unknown prop override is refused");
    Check(EvaluateError(composition, 0, R"({"palette": "z"})").code ==
              "PropOutOfRange",
          "an enum value outside its list is refused");
    Check(EvaluateError(composition, 0, R"({"size": 11})").code ==
              "PropOutOfRange",
          "a number above its bound is refused, not clamped");
    Check(EvaluateError(composition, 10).code == "FrameOutOfRange",
          "the duration is exclusive");

    std::string missingToken = json;
    missingToken.replace(missingToken.find(R"("b": "#5fc3d0")"),
                         std::string(R"("b": "#5fc3d0")").size(),
                         R"("c": "#5fc3d0")");
    MotionComposition partial;
    if (Parse(missingToken, partial, "table without every enum value")) {
        const MotionError error =
            EvaluateError(partial, 0, R"({"palette": "b"})");
        Check(error.code == "UnknownToken" &&
                  error.path == "root.children[0].style.backgroundColor",
              "a missing token entry names the property that needed it");
    }
}

// MOTION-AFTER-2026-09 -- a text shows the timecode of the frame, of the
// whole composition or of its sequence, offset if asked.
void TestTimecode() {
    const std::string json = R"({
      "width": 400, "height": 100, "fps": 25, "duration": 200,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "text", "id": "whole", "content": {"timecode": "composition"}},
        {"type": "sequence", "from": 100, "duration": 100, "children": [
          {"type": "text", "id": "scene", "content": {"timecode": "local", "offset": 5}}]}]}})";
    MotionComposition composition;
    if (!Parse(json, composition, "timecode")) {
        return;
    }
    MotionFrame frame;
    if (Evaluate(composition, 140, frame)) {
        const std::string dump = DumpMotionFrame(frame);
        Check(frame.root.children.size() == 2 &&
                  frame.root.children[0].text == "00:00:05:15" &&
                  frame.root.children[1].children[0].text == "00:00:01:20",
              "timecode: 140 frames is 00:00:05:15, 40 local frames plus 5 "
              "is 00:00:01:20: " +
                  dump.substr(0, 200));
    }
    MotionComposition refused;
    MotionError error;
    Check(!ParseMotionComposition(
              R"({"width":8,"height":8,"fps":25,"duration":1,"root":{"type":)"
              R"("text","content":{"timecode":"now"}}})",
              refused, error) &&
              error.code == "InvalidValue",
          "a timecode names composition or local: " + error.code);
    // MA7 -- an offset is a whole number of frames After's expression can
    // hold exactly (2^53 at most either way). Past it, frame + offset
    // overflowed in the engine: INT64_MAX at frame 1 wrapped to a negative
    // frame and read 00:00:00:00.
    const auto offsetBy = [](const std::string& offset) {
        return ParseError(
            R"({"width":8,"height":8,"fps":25,"duration":2,"root":{"type":)"
            R"("text","style":{"fontFamily":"Fake"},"content":{"timecode":)"
            R"("composition","offset":)" +
            offset + "}}}");
    };
    Check(offsetBy("9007199254740992").code == "NoError" &&
              offsetBy("-9007199254740992").code == "NoError",
          "a timecode offset of 2^53 frames either way is accepted");
    for (const char* offset :
         {"9007199254740993", "-9007199254740993", "9223372036854775807"}) {
        const MotionError refusedOffset = offsetBy(offset);
        Check(refusedOffset.code == "InvalidValue" &&
                  refusedOffset.path == "root.content.offset",
              std::string("a timecode offset of ") + offset +
                  " frames is refused: " + refusedOffset.code + " at " +
                  refusedOffset.path);
    }
    MotionComposition farthest;
    if (Parse(R"({"width":8,"height":8,"fps":25,"duration":2,"root":{)"
              R"("type":"text","style":{"fontFamily":"Fake"},"content":{)"
              R"("timecode":"composition","offset":9007199254740992}}})",
              farthest, "the farthest offset")) {
        MotionFrame shown;
        // 2^53 + 1 frames at 25: 360287970189639 s and 18 frames.
        Check(Evaluate(farthest, 1, shown) &&
                  shown.root.text == "100079991719:20:39:18",
              "the farthest offset reads its frame: " + shown.root.text);
    }
}

// MA7 -- strokeDasharray is a list of lengths, in pixels with or without
// "px", separated by spaces or commas, or none. Anything else was taken
// for a pattern up to the first word it could not read: "10px 5px" drew
// 10 on, 10 off.
// MOTION-WEB-2026-10 MW5 -- transformOrigin places the point scale, rotate
// and skew turn about, as CSS; skewX and skewY slant as CSS's skew(), after
// the scale and before the rotation. A third origin value, or one CSS
// refuses, is refused; so is a skew in units, which have no skew.
void TestTransformOriginAndSkew() {
    const auto box = [](const std::string& style) {
        return R"({"width":200,"height":200,"fps":25,"duration":10,)"
               R"("root":{"type":"fill","children":[{"type":"box","style":{)"
               R"("position":"absolute","left":0,"top":0,"width":100,)"
               R"("height":100,"backgroundColor":"#fff",)" +
               style + "}}]}}";
    };
    const struct {
        const char* written;
        MotionTransformOrigin origin;
    } accepted[] = {
        {"left top", {0.0, 0.0, 0.0, 0.0}},
        {"top left", {0.0, 0.0, 0.0, 0.0}},
        {"center bottom", {0.5, 0.0, 1.0, 0.0}},
        {"bottom", {0.5, 0.0, 1.0, 0.0}},
        {"right", {1.0, 0.0, 0.5, 0.0}},
        {"25% 100%", {0.25, 0.0, 1.0, 0.0}},
        {"0 12px", {0.0, 0.0, 0.0, 12.0}},
        {"10px top", {0.0, 10.0, 0.0, 0.0}},
        {"50%", {0.5, 0.0, 0.5, 0.0}},
    };
    for (const auto& item : accepted) {
        MotionTransformOrigin origin;
        Check(ParseMotionTransformOrigin(item.written, origin) &&
                  origin.x_fraction == item.origin.x_fraction &&
                  origin.x_pixels == item.origin.x_pixels &&
                  origin.y_fraction == item.origin.y_fraction &&
                  origin.y_pixels == item.origin.y_pixels,
              std::string("transformOrigin \"") + item.written +
                  "\" reads as CSS reads it");
    }
    for (const char* refused : {"top 10px", "left right", "top bottom",
                                "1px 2px 3px", "", "middle", "10em 0"}) {
        const MotionError error = ParseError(
            box(std::string(R"("transformOrigin":")") + refused + "\""));
        Check(error.code == "InvalidValue" &&
                  error.path == "root.children[0].style.transformOrigin",
              std::string("transformOrigin \"") + refused +
                  "\" is refused: " + error.code + " at " + error.path);
    }
    // Rotated a quarter turn about its bottom-left corner: that corner
    // stays, the bottom-right one lands below it.
    MotionComposition composition;
    MotionFrame frame;
    if (Parse(box(R"("transformOrigin":"left bottom","rotate":90)"),
              composition, "origin") &&
        Evaluate(composition, 0, frame)) {
        const MotionEvaluatedNode& node = Child(frame.root, {0});
        const MotionAffine map = MotionNodeTransform(node);
        double x = 0.0;
        double y = 0.0;
        map.Apply(0.0, 100.0, x, y);
        const bool stays = Near(x, 0.0) && Near(y, 100.0);
        map.Apply(100.0, 100.0, x, y);
        Check(stays && Near(x, 0.0) && Near(y, 200.0),
              "a quarter turn about the bottom-left corner: " +
                  std::to_string(x) + ", " + std::to_string(y));
        Check(DumpMotionFrame(frame).find(R"("origin":[0,0,1,0])") !=
                  std::string::npos,
              "the descriptor says the origin");
    }
    // skewX(45deg) about the centre: the bottom edge goes right by half the
    // height, the top edge left; the scale comes first (CSS's skew(), as
    // GSAP and Lottie order them).
    MotionComposition skewed;
    if (Parse(box(R"("skewX":45,"scaleY":0.5)"), skewed, "skew") &&
        Evaluate(skewed, 0, frame)) {
        const MotionAffine map = MotionNodeTransform(Child(frame.root, {0}));
        double x = 0.0;
        double y = 0.0;
        map.Apply(50.0, 100.0, x, y);
        const bool bottom = Near(x, 75.0) && Near(y, 75.0);
        map.Apply(50.0, 0.0, x, y);
        Check(bottom && Near(x, 25.0) && Near(y, 25.0),
              "skewX(45deg) after scaleY(0.5) slants by a quarter of the box");
        Check(DumpMotionFrame(frame).find(R"("skewX":45,"skewY":0)") !=
                  std::string::npos,
              "the descriptor says the skew");
    }
    MotionComposition slanting;
    if (Parse(box(R"("transformOrigin":"0 0"},"animate":{"skewY":{)"
                  R"("interpolate":{"frames":[0,10],"values":[0,20]}})"),
              slanting, "animated skew") &&
        Evaluate(slanting, 5, frame)) {
        const MotionAffine map = MotionNodeTransform(Child(frame.root, {0}));
        double x = 0.0;
        double y = 0.0;
        map.Apply(100.0, 0.0, x, y);
        Check(Near(x, 100.0) && Near(y, 100.0 * std::tan(10.0 * M_PI / 180.0)),
              "skewY animates, about the top-left corner");
    }
    // Without either, a node's transform keeps every bit it had.
    MotionComposition plain;
    if (Parse(box(R"("rotate":30,"scale":1.5)"), plain, "plain") &&
        Evaluate(plain, 0, frame)) {
        Check(DumpMotionFrame(frame).find("skew") == std::string::npos &&
                  DumpMotionFrame(frame).find("origin") == std::string::npos,
              "no skew, no origin: the descriptor is as before");
    }
    const MotionError units = ParseError(
        R"({"width":200,"height":200,"fps":25,"duration":10,"root":{)"
        R"("type":"text","content":"ab","units":"char","style":{)"
        R"("fontFamily":"Fake"},"animate":{"skewX":{"interpolate":{)"
        R"("frames":[0,5],"values":[0,10]}}}}})");
    Check(units.code != "NoError",
          "a skew in units is refused: " + units.code + " " + units.message);
}

void TestDashArray() {
    const auto dashed = [](const std::string& dashes) {
        return R"({"width":60,"height":10,"fps":25,"duration":1,"root":{)"
               R"("type":"path","style":{"fill":"none","stroke":"#FFFFFF",)"
               R"("strokeDasharray":")" +
               dashes + R"("},"d":"M0 5 L60 5"}})";
    };
    const struct {
        const char* written;
        std::vector<double> drawn;
    } accepted[] = {
        {"10 5", {10, 5}}, {"10px 5px", {10, 5}},
        {"10,5", {10, 5}}, {"10, 5 2", {10, 5, 2}},
        {"0 4", {0, 4}},   {"none", {}},
        {"0 0", {}},
    };
    for (const auto& dashes : accepted) {
        MotionComposition composition;
        MotionFrame frame;
        if (!Parse(dashed(dashes.written), composition, dashes.written) ||
            !Evaluate(composition, 0, frame)) {
            continue;
        }
        Check(frame.root.path_paint.dash == dashes.drawn,
              std::string("strokeDasharray \"") + dashes.written +
                  "\" draws its own pattern");
    }
    for (const char* refused :
         {"10 abc", "10px 5em", "-5 5", "10%", " ", "10,,5", "1e999"}) {
        const MotionError error = ParseError(dashed(refused));
        Check(error.code == "InvalidValue" &&
                  error.path == "root.style.strokeDasharray",
              std::string("strokeDasharray \"") + refused +
                  "\" is refused: " + error.code + " at " + error.path);
    }
}

// MA7 -- the seed of grain(), slice() and displace() is an int64. One past
// it was read as INT64_MAX (strtoll saturates): two different seeds
// written, one pattern drawn.
void TestFilterSeeds() {
    for (const char* filter : {"grain(0.2 1px 9223372036854775808)",
                               "slice(4px 8px 99999999999999999999)",
                               "displace(4px 8px 9223372036854775808 0)"}) {
        const MotionError error = ParseError(
            std::string(R"({"width":8,"height":8,"fps":25,"duration":1,)") +
            R"("root":{"type":"fill","style":{"filter":")" + filter + "\"}}}");
        Check(error.code == "InvalidValue",
              std::string("a seed past int64 is refused: ") + filter + " " +
                  error.code);
    }
    Check(ParseError(R"({"width":8,"height":8,"fps":25,"duration":1,)"
                     R"("root":{"type":"fill","style":{"filter":)"
                     R"J("grain(0.2 1px 9223372036854775807)"}}})J")
                  .code == "NoError",
          "the largest seed is accepted");
}

void TestRefusals() {
    const auto card = [](const std::string& style) {
        return R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
                   "root": {"type": "fill", "style": )" +
               style + "}}";
    };
    MotionError error = ParseError(card(R"({"paddingX": 10})"));
    Check(error.code == "UnknownStyleProperty" &&
              error.path == "root.style.paddingX",
          "a non-CSS property name is refused with its path");
    // MOTION-BIBLIOTHEQUE-2026-10 MB6 -- the CSS a model reaches for and
    // the format does not have is refused with what to write instead.
    const auto refusal = [&](const std::string& style) {
        const MotionError refused = ParseError(card(style));
        return refused.code == "UnknownStyleProperty" ? refused.message
                                                      : refused.code;
    };
    // MOTION-WEB-2026-10 MW5 -- transformOrigin is written now; CSS's skew()
    // shorthand says its two properties.
    Check(refusal(R"({"transformOrigin": "left center"})") == "NoError" &&
              refusal(R"({"skew": 10})").find("skewX and skewY") !=
                  std::string::npos,
          "transformOrigin is read; skew names skewX and skewY: " +
              refusal(R"({"skew": 10})"));
    Check(refusal(R"({"whiteSpace": "nowrap"})").find("flexShrink: 0") !=
                  std::string::npos &&
              refusal(R"({"white-space": "nowrap"})").find("flexShrink: 0") !=
                  std::string::npos,
          "whiteSpace, in either spelling, says how a line stays whole");
    Check(refusal(R"J({"transform": "scale(2)"})J").find("translateX") !=
                  std::string::npos &&
              refusal(R"({"boxShadow": "0 0 4px #000"})").find("drop-shadow") !=
                  std::string::npos &&
              refusal(R"({"zIndex": 2})").find("order of `children`") !=
                  std::string::npos &&
              refusal(R"({"display": "none"})").find("`visible`") !=
                  std::string::npos,
          "transform, boxShadow, zIndex and display name their equivalent");
    Check(refusal(R"({"background-color": "#000000"})")
                  .find("write 'backgroundColor'") != std::string::npos,
          "a property CSS writes with a hyphen is given in camelCase: " +
              refusal(R"({"background-color": "#000000"})"));
    Check(refusal(R"({"paddingX": 10})") ==
              "'paddingX' is not a supported CSS property name",
          "a name with no known equivalent is refused as before");
    error = ParseError(R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
        "root": {"type": "fill", "animate": {"transformOrigin": {"interpolate": {"frames": [0, 1], "values": [0, 1]}}}}})");
    Check(error.code == "InvalidAnimation",
          "transformOrigin is not animated: " + error.code);
    error = ParseError(card(R"({"lineHeight": "72"})"));
    Check(error.code == "InvalidValue",
          "a unitless lineHeight string is ambiguous and refused");
    error = ParseError(R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
        "root": {"type": "fill", "animate": {"backgroundColor": {"interpolate": {"frames": [0, 1], "values": [0, 1]}}}}})");
    Check(error.code == "InvalidAnimation",
          "a colour property interpolates colours, not numbers");
    error = ParseError(R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
        "root": {"type": "fill", "animate": {"opacity": {"interpolate": {"frames": [3, 3], "values": [0, 1]}}}}})");
    Check(error.code == "InvalidAnimation",
          "interpolate frames must strictly increase");
    // REMOTION-PARITY-2026-09 REM2 -- video arrived; it names exactly one
    // media, a project's by ULID or a file.
    error = ParseError(R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
        "root": {"type": "video"}})");
    Check(error.code == "InvalidValue" && error.path == "root",
          "a video without a media is refused where it is");
    error = ParseError(R"({"width": 10, "height": 10, "fps": 25, "duration": 5,
        "root": {"type": "fill", "children": [{"type": "box", "id": "x"}, {"type": "box", "id": "x"}]}})");
    Check(error.code == "DuplicateNodeId", "node ids are unique");

    MotionComposition multiplier;
    if (Parse(R"({"width": 200, "height": 200, "fps": 25, "duration": 5,
          "root": {"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 50, "lineHeight": 1.5},
            "children": [{"type": "text", "content": "x"}]}})",
              multiplier, "lineHeight multiplier")) {
        MotionFrame frame;
        if (Evaluate(multiplier, 0, frame)) {
            Check(Child(frame.root, {0}).text_style.line_height == 75.0,
                  "a bare lineHeight number multiplies the font size");
        }
    }

    MotionComposition missingFont;
    if (Parse(R"({"width": 200, "height": 200, "fps": 25, "duration": 5,
          "root": {"type": "fill", "children": [
            {"type": "text", "content": "x", "style": {"fontFamily": "Missing Sans"}}]}})",
              missingFont, "missing font")) {
        const MotionError missing = EvaluateError(missingFont, 0);
        Check(
            missing.code == "FontMissing" && missing.path == "root.children[0]",
            "a missing font fails with the text node's path");
    }
}

void TestDeterminismAndRates() {
    const std::string at25 =
        R"({"width": 300, "height": 300, "fps": 25, "duration": 50,
      "root": {"type": "fill", "style": {"alignItems": "center", "justifyContent": "center", "fontFamily": "Fake"},
        "children": [{"type": "text", "content": "Titre", "style": {"textAlign": "center"},
          "animate": {"scale": {"spring": {"damping": 12}},
                      "translateY": {"interpolate": {"frames": [0, 10], "values": [40, 0], "easing": "ease-out"}}}}]}})";
    std::string at50 = at25;
    at50.replace(at50.find("\"fps\": 25"), 9, "\"fps\": 50");
    at50.replace(at50.find("\"duration\": 50"), 14, "\"duration\": 99");
    MotionComposition first;
    MotionComposition second;
    if (!Parse(at25, first, "25 fps") || !Parse(at50, second, "50 fps")) {
        return;
    }

    MotionFrame one;
    MotionFrame two;
    if (Evaluate(first, 7, one) && Evaluate(first, 7, two)) {
        Check(DumpMotionFrame(one) == DumpMotionFrame(two),
              "same composition, props and frame give the same bytes");
    }
    MotionFrame slow;
    MotionFrame fast;
    if (Evaluate(first, 20, slow) && Evaluate(second, 40, fast)) {
        Check(Child(slow.root, {0}).scale_x == Child(fast.root, {0}).scale_x,
              "a spring gives the same value at the same instant at 25 and "
              "50 fps");
    }
}

void TestFormatDocumentExamples() {
    std::ifstream input(MOTION_FORMAT_DOC);
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string doc = buffer.str();
    Check(!doc.empty(), "docs/MOTION_FORMAT.md is readable");
    const std::regex block("```json\\n([\\s\\S]*?)```");
    int compositions = 0;
    for (std::sregex_iterator it(doc.begin(), doc.end(), block), end; it != end;
         ++it) {
        const std::string json = (*it)[1].str();
        // Only complete compositions: skeletons with placeholders are prose.
        if (json.find("\"duration\"") == std::string::npos ||
            json.find("\"root\"") == std::string::npos ||
            json.find("…") != std::string::npos) {
            continue;
        }
        ++compositions;
        MotionComposition composition;
        if (!Parse(json, composition,
                   "format example " + std::to_string(compositions))) {
            continue;
        }
        MotionFrame frame;
        Evaluate(composition, 3, frame);
        // M6 -- and every version in every format it declares.
        std::vector<std::string> variants = {""};
        std::vector<std::string> formats = {""};
        for (const MotionVariant& variant : composition.variants) {
            variants.push_back(variant.name);
        }
        for (const MotionFormat& format : composition.formats) {
            formats.push_back(format.name);
        }
        for (const std::string& variant : variants) {
            for (const std::string& format : formats) {
                MotionComposition resolved;
                mcp_json::Value props;
                MotionError error;
                const bool ok = ResolveMotionVariant(composition, variant,
                                                     format, mcp_json::Value(),
                                                     resolved, props, error) &&
                                Evaluate(resolved, 3, frame, props.Dump());
                Check(ok, "format example " + std::to_string(compositions) +
                              " in version '" + variant + "' and format '" +
                              format + "': " + error.message);
            }
        }
    }
    Check(compositions >= 7,
          "the format document keeps its complete examples executable");
}

// ---------------------------------------------------------------------------
// M1b -- text units, paths, clip-path, repeat, series, timeScale.

std::string Card(const std::string& root, int64_t duration = 50,
                 const std::string& props = "{}") {
    return R"({"width": 400, "height": 200, "fps": 25, "duration": )" +
           std::to_string(duration) + R"(, "props": )" + props +
           R"(, "root": )" + root + "}";
}

const MotionEvaluatedNode* FindId(const MotionEvaluatedNode& node,
                                  const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = FindId(child, id)) {
            return found;
        }
    }
    return nullptr;
}

// Opacity of each unit of the text node `id`, in unit order.
std::vector<double> UnitOpacities(const MotionComposition& composition,
                                  int64_t frame, const std::string& id,
                                  const std::string& props = "null") {
    MotionFrame output;
    std::vector<double> opacities;
    if (!Evaluate(composition, frame, output, props)) {
        return opacities;
    }
    const MotionEvaluatedNode* node = FindId(output.root, id);
    Check(node != nullptr, "text node " + id + " is evaluated");
    if (node != nullptr) {
        for (const MotionTextUnitState& unit : node->units) {
            opacities.push_back(unit.opacity);
        }
    }
    return opacities;
}

bool Same(const std::vector<double>& actual,
          const std::vector<double>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (size_t index = 0; index < actual.size(); ++index) {
        if (!Near(actual[index], expected[index])) {
            return false;
        }
    }
    return true;
}

std::string Show(const std::vector<double>& values) {
    std::ostringstream text;
    for (double value : values) {
        text << value << ' ';
    }
    return text.str();
}

void TestTextUnits() {
    const auto cascade = [](const std::string& text, const std::string& unit,
                            const std::string& stagger) {
        return Card(
            R"({"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20}, "children": [
            {"type": "text", "id": "t", "content": ")" +
            text + R"(", "units": ")" + unit + R"(",
             "animate": {"opacity": {"interpolate": {"frames": [0, 4], "values": [0, 1]}}},
             "stagger": )" +
            stagger + "}]}");
    };
    MotionComposition words;
    if (Parse(cascade("ab cd ef", "word", R"({"each": 3})"), words,
              "word cascade")) {
        // Word i starts 3i frames late: at frame 3 the first word is three
        // quarters in, the others have not started.
        const std::vector<double> at3 = UnitOpacities(words, 3, "t");
        Check(Same(at3, {0.75, 0.0, 0.0}),
              "word cascade at frame 3: " + Show(at3));
        const std::vector<double> at6 = UnitOpacities(words, 6, "t");
        Check(Same(at6, {1.0, 0.75, 0.0}),
              "word cascade at frame 6: " + Show(at6));
        MotionFrame frame;
        if (Evaluate(words, 0, frame)) {
            const MotionEvaluatedNode& text = Child(frame.root, {0});
            Check(text.words.size() == 3 && text.words[1].text == "cd" &&
                      Near(text.words[1].x, 30.0) &&
                      Near(text.words[1].width, 20.0) &&
                      Near(text.words[1].height, 24.0) &&
                      text.chars.size() == 6 && text.chars[3].text == "d" &&
                      text.chars[3].word == 1 && Near(text.chars[3].x, 40.0),
                  "words and characters are cut after layout, with boxes");
            Check(text.units.size() == 3 && text.units[2].delay == 6,
                  "each unit carries its delay");
        }
    }
    MotionComposition reversed;
    if (Parse(cascade("ab cd ef", "word", R"({"each": 3, "order": "reverse"})"),
              reversed, "reverse cascade")) {
        const std::vector<double> at3 = UnitOpacities(reversed, 3, "t");
        Check(Same(at3, {0.0, 0.0, 0.75}),
              "reverse starts with the last word: " + Show(at3));
    }
    MotionComposition chars;
    if (Parse(cascade("ab c", "char", R"({"each": 1})"), chars,
              "character cascade")) {
        // Spaces are not units: c is the third character, one frame after b.
        const std::vector<double> at2 = UnitOpacities(chars, 2, "t");
        Check(Same(at2, {0.5, 0.25, 0.0}),
              "character cascade at frame 2: " + Show(at2));
    }
    MotionComposition lines;
    if (Parse(cascade("ab\\ncd", "line", R"({"each": 4})"), lines,
              "line cascade")) {
        const std::vector<double> at4 = UnitOpacities(lines, 4, "t");
        Check(Same(at4, {1.0, 0.0}), "line cascade at frame 4: " + Show(at4));
    }

    const std::string many = "un deux trois quatre cinq six sept huit";
    MotionComposition first;
    MotionComposition second;
    MotionComposition other;
    if (Parse(cascade(many, "word",
                      R"({"each": 2, "order": "random", "seed": 7})"),
              first, "random cascade") &&
        Parse(cascade(many, "word",
                      R"({"each": 2, "order": "random", "seed": 7})"),
              second, "same seed") &&
        Parse(cascade(many, "word",
                      R"({"each": 2, "order": "random", "seed": 8})"),
              other, "other seed")) {
        MotionFrame a;
        MotionFrame b;
        MotionFrame c;
        if (Evaluate(first, 5, a) && Evaluate(second, 5, b) &&
            Evaluate(other, 5, c)) {
            std::vector<int64_t> delays;
            std::vector<int64_t> otherDelays;
            for (const MotionTextUnitState& unit : Child(a.root, {0}).units) {
                delays.push_back(unit.delay);
            }
            for (const MotionTextUnitState& unit : Child(c.root, {0}).units) {
                otherDelays.push_back(unit.delay);
            }
            std::vector<int64_t> sorted = delays;
            std::sort(sorted.begin(), sorted.end());
            Check(sorted == std::vector<int64_t>({0, 2, 4, 6, 8, 10, 12, 14}),
                  "a random order is a permutation of the forward delays");
            Check(delays != std::vector<int64_t>({0, 2, 4, 6, 8, 10, 12, 14}),
                  "a random order is not the forward order");
            Check(DumpMotionFrame(a) == DumpMotionFrame(b),
                  "the same seed gives the same order, byte for byte");
            Check(delays != otherDelays, "another seed gives another order");
        }
    }
    Check(ParseError(cascade("ab", "word", R"({"each": 2, "order": "random"})"))
                  .code == "InvalidValue",
          "a random order without a seed is refused");
    Check(ParseError(Card(R"({"type": "text", "content": "ab", "units": "word",
            "animate": {"letterSpacing": {"interpolate": {"frames": [0, 4], "values": [10, 0]}}}})"))
                  .code == "InvalidAnimation",
          "a unit animation that would change layout is refused");
    Check(
        ParseError(
            Card(
                R"({"type": "text", "content": "ab", "stagger": {"each": 2}})"))
                .code == "InvalidValue",
        "a stagger without units is refused");

    MotionComposition spaced;
    if (Parse(
            Card(
                R"({"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20, "wordSpacing": 10},
            "children": [{"type": "text", "content": "a b"}]})"),
            spaced, "word spacing")) {
        MotionFrame frame;
        if (Evaluate(spaced, 0, frame)) {
            const MotionEvaluatedNode& text = Child(frame.root, {0});
            Check(text.words.size() == 2 && Near(text.words[1].x, 30.0),
                  "wordSpacing widens every space");
        }
    }
}

void TestTextRanges() {
    const std::string json = Card(
        R"({"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20, "color": "#ffffff"}, "children": [
            {"type": "text", "id": "t", "content": "un deux trois quatre cinq", "units": "word",
             "ranges": [{"unit": "word", "start": {"prop": "premier"}, "count": {"prop": "combien"},
                         "style": {"color": {"prop": "accent"}}}]},
            {"type": "text", "id": "c", "content": "ab cd", "units": "char",
             "ranges": [{"unit": "word", "start": 1, "style": {"backgroundColor": "#00ff00"}}]}]})",
        10,
        R"({"premier": {"type": "number", "default": 1}, "combien": {"type": "number", "default": 2},
                 "accent": {"type": "color", "default": "#ff0000"}})");
    MotionComposition composition;
    if (!Parse(json, composition, "ranges")) {
        return;
    }
    const auto colored = [&](const std::string& props) {
        std::vector<size_t> indices;
        MotionFrame frame;
        if (!Evaluate(composition, 0, frame, props)) {
            return indices;
        }
        const MotionEvaluatedNode* text = FindId(frame.root, "t");
        for (const MotionTextUnitState& unit : text->units) {
            if (unit.color.green == 0.0) {
                indices.push_back(unit.index);
            }
        }
        return indices;
    };
    Check(colored("null") == std::vector<size_t>({1, 2}),
          "the range colours the words its props select");
    Check(colored(R"({"premier": 3, "combien": 5})") ==
              std::vector<size_t>({3, 4}),
          "a range past the last word stops at the text's end");
    Check(colored(R"({"combien": 0})").empty(),
          "a range of zero words colours nothing");
    Check(EvaluateError(composition, 0, R"({"premier": -1})").code ==
              "InvalidValue",
          "a negative range start is refused");
    Check(EvaluateError(composition, 0, R"({"premier": 1.5})").code ==
              "InvalidValue",
          "a fractional range start is refused");
    MotionFrame frame;
    if (Evaluate(composition, 0, frame)) {
        const MotionEvaluatedNode* chars = FindId(frame.root, "c");
        Check(chars != nullptr && chars->units.size() == 4 &&
                  !chars->units[1].has_background &&
                  chars->units[2].has_background &&
                  chars->units[3].has_background,
              "a word range styles the characters of that word");
    }
    Check(ParseError(Card(R"({"type": "text", "content": "ab", "units": "word",
            "ranges": [{"unit": "char", "start": 0, "style": {"color": "#fff"}}]})"))
                  .code == "InvalidValue",
          "a range finer than the text's units is refused");
    Check(ParseError(Card(R"({"type": "text", "content": "ab",
            "ranges": [{"unit": "word", "start": 0, "style": {"fontSize": 30}}]})"))
                  .code == "InvalidValue",
          "a range cannot change layout");
}

// MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. A highlighter stroke
// under a range of words in the middle of a text of several lines: one band
// per line, the spaces between the words included, from the boxes layout
// gave them, filled in reading order up to its progress. The fake measurer
// advances 10 px a code point at 20 px, lines 24 px tall:
//   line 0 "un deux trois": un 0-20, deux 30-70, trois 80-130
//   line 1 "quatre cinq":   quatre 0-60, cinq 70-110
void TestTextHighlights() {
    const std::string json = Card(
        R"({"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20, "color": "#ffffff"}, "children": [
            {"type": "text", "id": "t", "content": "un deux trois\nquatre cinq",
             "highlights": [{"unit": "word", "start": {"prop": "premier"}, "count": 3,
                             "color": {"prop": "feutre"}, "top": 0.5, "bottom": 1, "bleed": 4,
                             "progress": {"interpolate": {"frames": [0, 10], "values": [0, 1]}}}]},
            {"type": "text", "id": "l", "content": "un deux trois\nquatre cinq",
             "highlights": [{"unit": "line", "start": 1, "color": "#00ff00"}]},
            {"type": "text", "id": "c", "content": "un deux", "units": "word",
             "highlights": [{"unit": "char", "start": 0, "count": 3, "color": "#0000ff",
                             "progress": {"prop": "avance"}}]},
            {"type": "text", "id": "x", "content": "un deux",
             "highlights": [{"unit": "word", "start": 9, "color": "#0000ff"}]}]})",
        20,
        R"({"premier": {"type": "number", "default": 1},
            "feutre": {"type": "color", "default": "#ffe600"},
            "avance": {"type": "number", "default": 1}})");
    MotionComposition composition;
    if (!Parse(json, composition, "highlights")) {
        return;
    }
    const auto band = [](const MotionHighlightBand& b, size_t line, double x,
                         double y, double width, double height,
                         double painted) {
        return b.line == line && Near(b.box.x, x) && Near(b.box.y, y) &&
               Near(b.box.width, width) && Near(b.box.height, height) &&
               Near(b.painted, painted);
    };
    const auto show = [](const MotionEvaluatedNode* node) {
        std::ostringstream text;
        if (node == nullptr) return std::string("no node");
        for (const MotionHighlightBand& b : node->highlights) {
            text << "[line " << b.line << " " << b.box.x << "," << b.box.y
                 << " " << b.box.width << "x" << b.box.height << " painted "
                 << b.painted << "] ";
        }
        return text.str();
    };
    MotionFrame half;
    if (Evaluate(composition, 5, half)) {
        const MotionEvaluatedNode* text = FindId(half.root, "t");
        // deux to quatre: 30-130 on line 0 (the space 70-80 inside), 0-60
        // on line 1, 4 px of bleed at each end; the lower half of each line.
        // Halfway, 88 of 176 px: all on line 0, nothing yet on line 1.
        Check(text != nullptr && text->highlights.size() == 2 &&
                  band(text->highlights[0], 0, 26, 12, 108, 12, 88) &&
                  band(text->highlights[1], 1, -4, 36, 68, 12, 0) &&
                  Near(text->highlights[0].color.red, 1.0) &&
                  Near(text->highlights[0].color.blue, 0.0),
              "a stroke under three words across two lines, half drawn: " +
                  show(text));
        Check(text != nullptr && text->units.empty() &&
                  text->unit_kind == MotionTextUnit::None,
              "a highlight cuts the text into no units");
        const MotionEvaluatedNode* line = FindId(half.root, "l");
        Check(line != nullptr && line->highlights.size() == 1 &&
                  band(line->highlights[0], 1, 0, 24, 110, 24, 110),
              "a line highlight spans its words: " + show(line));
        const MotionEvaluatedNode* chars = FindId(half.root, "c");
        Check(chars != nullptr && chars->highlights.size() == 1 &&
                  band(chars->highlights[0], 0, 0, 0, 40, 24, 40) &&
                  chars->units.size() == 2,
              "u, n and d, the space between them included: " + show(chars));
        const MotionEvaluatedNode* past = FindId(half.root, "x");
        Check(past != nullptr && past->highlights.empty(),
              "a highlight past the text's end paints nothing");
        const std::string dumped = DumpMotionFrame(half);
        Check(dumped.find("\"highlights\":[{\"highlight\":0,\"line\":0,") !=
                      std::string::npos &&
                  dumped.find("\"painted\":88") != std::string::npos,
              "the geometry gives the bands: " + dumped.substr(0, 400));
    }
    MotionFrame later;
    if (Evaluate(composition, 8, later,
                 R"({"premier": 2, "feutre": "#ff00ff", "avance": 0.25})")) {
        const MotionEvaluatedNode* text = FindId(later.root, "t");
        // trois to cinq: 80-130 and 0-110, 58 + 118 = 176 px; at 8/10,
        // 140.8 px: line 0 full, 82.8 px of line 1.
        Check(text != nullptr && text->highlights.size() == 2 &&
                  band(text->highlights[0], 0, 76, 12, 58, 12, 58) &&
                  band(text->highlights[1], 1, -4, 36, 118, 12, 82.8) &&
                  Near(text->highlights[0].color.green, 0.0),
              "the props move it and the progress fills the next line: " +
                  show(text));
        const MotionEvaluatedNode* chars = FindId(later.root, "c");
        Check(chars != nullptr && chars->highlights.size() == 1 &&
                  Near(chars->highlights[0].painted, 10.0),
              "a bound progress: a quarter of 40 px: " + show(chars));
    }
    MotionFrame start;
    if (Evaluate(composition, 0, start)) {
        const MotionEvaluatedNode* text = FindId(start.root, "t");
        Check(text != nullptr && text->highlights.size() == 2 &&
                  Near(text->highlights[0].painted, 0.0),
              "at frame 0 nothing is drawn yet");
    }
    Check(EvaluateError(composition, 0, R"({"avance": 3})").code == "NoError",
          "a bound progress past 1 is held at 1");
    const auto refused = [](const std::string& highlight) {
        return ParseError(Card(R"({"type": "text", "content": "ab cd",
            "highlights": [)" + highlight +
                               "]}"))
                   .code == "InvalidValue";
    };
    Check(refused(R"({"unit": "word", "start": 0})"),
          "a highlight needs a color");
    Check(refused(R"({"unit": "word", "color": "#fff"})"),
          "a highlight needs a start");
    Check(refused(R"({"unit": "para", "start": 0, "color": "#fff"})"),
          "a unit is line, word or char");
    Check(
        refused(
            R"({"unit": "word", "start": 0, "color": "#fff", "top": 0.6, "bottom": 0.4})"),
        "the bottom is below the top");
    Check(
        refused(R"({"unit": "word", "start": 0, "color": "#fff", "top": 1.5})"),
        "top is a fraction of the line");
    Check(
        refused(
            R"({"unit": "word", "start": 0, "color": "#fff", "progress": 2})"),
        "a literal progress is from 0 to 1");
    Check(refused(
              R"({"unit": "word", "start": 0, "color": "#fff", "bleed": -1})"),
          "bleed is not negative");
    Check(refused(R"({"unit": "word", "start": -1, "color": "#fff"})"),
          "start is not negative");
    Check(ParseError(Card(R"({"type": "text", "content": "ab",
            "highlights": [{"unit": "word", "start": 0, "color": "#fff", "skew": 3}]})"))
                  .code != "NoError",
          "an unknown key is refused");
    Check(ParseError(Card(R"({"type": "box", "highlights": []})")).code !=
              "NoError",
          "only a text takes highlights");
}

void TestPaths() {
    const auto trimmed = [](const std::string& trim) {
        return Card(R"({"type": "fill", "children": [
            {"type": "path", "id": "p", "d": "M0 0 L100 0 L100 100",
             "style": {"position": "absolute", "left": 10, "top": 20, "fill": "none",
                       "stroke": "#ffffff", "strokeWidth": 4)" +
                    trim + R"(}}]})");
    };
    const auto paint = [&](const std::string& trim, MotionFrame& frame,
                           int64_t index = 0) -> const MotionPathPaint* {
        MotionComposition composition;
        if (!Parse(trimmed(trim), composition, "trim" + trim) ||
            !Evaluate(composition, index, frame)) {
            return nullptr;
        }
        const MotionEvaluatedNode* node = FindId(frame.root, "p");
        return node != nullptr ? &node->path_paint : nullptr;
    };
    const auto boundsAre = [](const MotionPathPaint* paint, double x, double y,
                              double width, double height) {
        return paint != nullptr && paint->has_bounds &&
               Near(paint->bounds.x, x) && Near(paint->bounds.y, y) &&
               Near(paint->bounds.width, width) &&
               Near(paint->bounds.height, height);
    };
    MotionFrame whole;
    const MotionPathPaint* full = paint("", whole);
    Check(boundsAre(full, 10, 20, 100, 100) && Near(full->length, 200.0) &&
              BoxIs(Child(whole.root, {0}), 10, 20, 100, 100) &&
              !full->has_fill && full->has_stroke &&
              Near(full->stroke_width, 4.0),
          "an untrimmed path: its box, bounds and length");
    MotionFrame half;
    Check(boundsAre(paint(R"(, "trimEnd": 0.5)", half), 10, 20, 100, 0),
          "trimEnd 0.5 keeps the first half of the length");
    MotionFrame none;
    const MotionPathPaint* empty = paint(R"(, "trimEnd": 0)", none);
    Check(empty != nullptr && !empty->has_bounds && empty->geometry.empty(),
          "trimEnd 0 draws nothing");
    MotionFrame one;
    Check(boundsAre(paint(R"(, "trimEnd": 1)", one), 10, 20, 100, 100),
          "trimEnd 1 draws the whole path");
    MotionFrame middle;
    Check(boundsAre(paint(R"(, "trimStart": 0.25, "trimEnd": 0.75)", middle),
                    60, 20, 50, 50),
          "trimStart and trimEnd keep the middle of the length");

    MotionComposition animated;
    if (Parse(Card(R"({"type": "fill", "children": [
            {"type": "path", "id": "p", "d": "M0 0 L100 0 L100 100",
             "style": {"fill": "none", "stroke": "#fff"},
             "animate": {"trimEnd": {"interpolate": {"frames": [0, 10], "values": [0, 1]}}}}]})"),
              animated, "animated trim")) {
        MotionFrame frame;
        if (Evaluate(animated, 5, frame)) {
            const MotionPathPaint& drawn = Child(frame.root, {0}).path_paint;
            Check(Near(drawn.trim_end, 0.5) && drawn.has_bounds &&
                      Near(drawn.bounds.width, 100.0) &&
                      Near(drawn.bounds.height, 0.0),
                  "an animated trimEnd draws the path on");
        }
    }

    MotionComposition circle;
    if (Parse(Card(R"({"type": "fill", "children": [
            {"type": "path", "d": "M0 50 A50 50 0 1 1 100 50 A50 50 0 1 1 0 50 Z",
             "style": {"position": "absolute", "left": 0, "top": 0, "trimEnd": 0.5}}]})"),
              circle, "circle")) {
        MotionFrame frame;
        if (Evaluate(circle, 0, frame)) {
            const MotionPathPaint& drawn = Child(frame.root, {0}).path_paint;
            Check(std::fabs(drawn.length - 314.159) < 0.1 && drawn.has_fill &&
                      drawn.fill.alpha == 1.0 &&
                      Near(drawn.bounds.x, 0.0, 1e-3) &&
                      Near(drawn.bounds.y, 0.0, 1e-3) &&
                      std::fabs(drawn.bounds.width - 100.0) < 0.5 &&
                      std::fabs(drawn.bounds.height - 50.0) < 0.5,
                  "arcs become curves: a circle's length, its upper half "
                  "trimmed, and SVG's default black fill");
        }
    }

    MotionComposition scaled;
    if (Parse(
            Card(
                R"({"type": "fill", "style": {"alignItems": "flex-start"}, "children": [
            {"type": "path", "d": "M0 0 L10 10", "viewBox": "0 0 10 10",
             "style": {"width": 100, "stroke": "currentColor", "color": "#00ff00"}},
            {"type": "path", "points": [[0, 0], [50, 50], [100, 0]], "curve": "catmull-rom",
             "style": {"fill": "none", "stroke": "#fff"}}]})"),
            scaled, "viewBox and points")) {
        MotionFrame frame;
        if (Evaluate(scaled, 0, frame)) {
            const MotionEvaluatedNode& box = Child(frame.root, {0});
            const MotionEvaluatedNode& spline = Child(frame.root, {1});
            Check(BoxIs(box, 0, 0, 100, 100) &&
                      Near(box.path_paint.bounds.width, 100.0) &&
                      Near(box.path_paint.stroke_width, 10.0) &&
                      box.path_paint.stroke.green == 1.0,
                  "a viewBox scales the path, its stroke width with it: " +
                      Describe(box));
            Check(spline.path_paint.has_bounds &&
                      spline.path_paint.bounds.height >= 50.0 - 1e-9 &&
                      spline.path_paint.geometry.size() == 3,
                  "a Catmull-Rom curve passes through its points");
        }
    }
    const MotionError invalid =
        ParseError(Card(R"({"type": "fill", "children": [
        {"type": "path", "d": "M0 0 L10"}]})"));
    Check(invalid.code == "InvalidPath" && invalid.path == "root.children[0].d",
          "malformed path data is refused with its path");
    Check(
        ParseError(
            Card(
                R"({"type": "path", "d": "M0 0 L1 1", "points": [[0, 0], [1, 1]]})"))
                .code == "InvalidValue",
        "a path is d or points, not both");

    MotionComposition outline;
    if (Parse(
            Card(
                R"({"type": "fill", "style": {"fontFamily": "Fake", "textStroke": "2px #ffffff", "color": "transparent"},
            "children": [{"type": "text", "content": "x"}]})"),
            outline, "text outline")) {
        MotionFrame frame;
        if (Evaluate(outline, 0, frame)) {
            const MotionTextStyle& style = Child(frame.root, {0}).text_style;
            Check(Near(style.stroke_width, 2.0) &&
                      style.stroke_color.red == 1.0 && style.color.alpha == 0.0,
                  "textStroke outlines text, and is inherited");
        }
    }
}

void TestClipPath() {
    MotionComposition composition;
    if (!Parse(Card(R"J({"type": "fill", "children": [
            {"type": "box", "id": "static", "style": {"position": "absolute", "left": 0, "top": 0,
             "width": 200, "height": 100, "clipPath": "polygon(0 0, 100% 0, 50% 100%)"}},
            {"type": "box", "id": "moving", "style": {"position": "absolute", "left": 100, "top": 50,
             "width": 200, "height": 100},
             "animate": {"clipPath": {"interpolate": {"frames": [0, 10],
               "values": ["polygon(0 0, 0 0, 0 100%)", "polygon(0 0, 100% 0, 50px 100%)"]}}}}]})J"),
               composition, "clip-path")) {
        return;
    }
    MotionFrame frame;
    if (!Evaluate(composition, 5, frame)) {
        return;
    }
    const auto clipIs =
        [](const MotionEvaluatedNode* node,
           const std::vector<std::pair<double, double>>& expected) {
            if (node == nullptr || !node->has_clip ||
                node->clip.size() != expected.size()) {
                return false;
            }
            for (size_t index = 0; index < expected.size(); ++index) {
                if (!Near(node->clip[index].first, expected[index].first) ||
                    !Near(node->clip[index].second, expected[index].second)) {
                    return false;
                }
            }
            return true;
        };
    Check(clipIs(FindId(frame.root, "static"), {{0, 0}, {200, 0}, {100, 100}}),
          "percentages of a polygon resolve against the border box");
    Check(clipIs(FindId(frame.root, "moving"),
                 {{100, 50}, {200, 50}, {125, 150}}),
          "an interpolated polygon moves each vertex, px and % mixed");
    const MotionError mismatch = ParseError(Card(R"J({"type": "fill",
        "animate": {"clipPath": {"interpolate": {"frames": [0, 10],
          "values": ["polygon(0 0, 100% 0, 50% 100%)", "polygon(0 0, 100% 0, 100% 100%, 0 100%)"]}}}})J"));
    Check(mismatch.code == "InvalidAnimation" &&
              mismatch.path == "root.animate.clipPath.interpolate.values[1]",
          "polygons with different vertex counts do not interpolate");
    Check(
        ParseError(
            Card(
                R"J({"type": "fill", "animate": {"clipPath": {"spring": {"from": 0, "to": 1}}}})J"))
                .code == "InvalidAnimation",
        "a spring does not animate a polygon");
    // REMOTION-PARITY-2026-09 REM3 -- circle(), ellipse() and inset()
    // joined polygon(); what CSS has beyond them is still refused.
    Check(
        ParseError(
            Card(R"J({"type": "fill", "style": {"clipPath": "circle(50%)"}})J"))
                    .code == "NoError" &&
            ParseError(
                Card(
                    R"J({"type": "fill", "style": {"clipPath": "path('M0 0 L1 1')"}})J"))
                    .code == "InvalidValue",
        "clip-path takes the basic shapes, and nothing else");

    MotionComposition colors;
    if (Parse(
            Card(
                R"J({"type": "fill", "animate": {"backgroundColor": {"interpolate":
            {"frames": [0, 10], "values": ["transparent", "#ff0000"]}}}})J"),
            colors, "colour interpolation")) {
        MotionFrame middle;
        if (Evaluate(colors, 5, middle)) {
            Check(DumpMotionFrame(middle).find(
                      "\"backgroundColor\":\"#ff000080\"") != std::string::npos,
                  "colours interpolate with premultiplied alpha, as CSS: "
                  "half-way from transparent to red is half-transparent red");
        }
    }
}

// MOTION-DESIGN-2026-09 -- mesh(): a grid of vertices, written in px or %
// of the box, resolved once the box is known, moved in straight lines.
void TestMeshWarp() {
    const std::string rest =
        "mesh(1 2, 0 0, 50% 0, 100% 0, 0 100%, 50% 100%, 100% 100%)";
    const std::string moved =
        "mesh(1 2, 0 0, 300px 0, 100% 0, 0 100%, 75% 100%, 100% 100%)";
    MotionComposition composition;
    if (Parse(Card(R"({"type": "fill", "id": "grid", "animate": {"filter":
                     {"interpolate": {"frames": [0, 10], "values": [")" +
                   rest + R"(", ")" + moved + R"("]}}}})"),
              composition, "mesh")) {
        MotionFrame middle;
        MotionFrame end;
        if (Evaluate(composition, 5, middle) &&
            Evaluate(composition, 10, end)) {
            const MotionEvaluatedNode* grid = FindId(middle.root, "grid");
            Check(grid != nullptr && grid->filters.size() == 1 &&
                      grid->filters[0].kind == MotionFilterKind::Mesh &&
                      grid->filters[0].mesh_resolved.size() == 6 &&
                      Near(grid->filters[0].mesh_resolved[1].first, 250) &&
                      Near(grid->filters[0].mesh_resolved[4].first, 250) &&
                      Near(grid->filters[0].mesh_resolved[4].second, 200) &&
                      Near(grid->filters[0].mesh_resolved[2].first, 400),
                  "mesh() resolves px and % against the box, then moves each "
                  "vertex along a straight line");
            const MotionEvaluatedNode* last = FindId(end.root, "grid");
            // The middle line went 100 px right: what the effect may write
            // grows by that much on every side, beyond the box and its 2 px
            // of overhang.
            Check(last != nullptr && last->has_filter_domain &&
                      Near(last->filter_domain.x, -102) &&
                      Near(last->filter_domain.width, 604) &&
                      Near(last->filter_domain.height, 404),
                  "mesh() widens the effect's domain by the farthest vertex "
                  "move");
        }
    }
    const auto refused = [](const std::string& filter) {
        return ParseError(Card(R"({"type": "fill", "style": {"filter": ")" +
                               filter + R"("}})"))
                   .code == "InvalidValue";
    };
    Check(refused("mesh(1 2, 0 0, 50% 0, 100% 0)") &&
              refused("mesh(0 2, 0 0, 50% 0, 100% 0)") &&
              refused("mesh(1 1 11, 0 0, 100% 0, 0 100%, 100% 100%)") &&
              refused("mesh(1 1, 0 0, 100% 0, 0 100%, 100% top)") &&
              ParseError(Card(R"({"type": "fill", "style": {"filter": ")" +
                              rest + R"("}})"))
                      .code == "NoError",
          "mesh() needs (rows + 1) x (columns + 1) vertices in px or %, a "
          "grid of 1 to 32 and a quality of 1 to 10");
    Check(ParseError(
              Card(R"({"type": "fill", "animate": {"filter":
                     {"interpolate": {"frames": [0, 10], "values": [")" +
                   rest +
                   R"J(", "mesh(1 1, 0 0, 100% 0, 0 100%, 100% 100%)"]}}}})J"))
                  .code == "InvalidAnimation",
          "two meshes of different grids do not interpolate");
}

void TestRepeat() {
    const std::string json = Card(
        R"({"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20}, "children": [
        {"type": "box", "id": "column", "style": {"flexDirection": "column", "gap": 10, "alignItems": "flex-start"}, "children": [
          {"type": "repeat", "each": {"prop": "lignes"}, "cycle": {"backgroundColor": ["#ff0000", "#0000ff"]},
           "stagger": {"each": 2},
           "template": {"type": "box", "id": "sticker", "style": {"width": 50, "height": 20, "translateX": {"index": true}},
             "animate": {"opacity": {"interpolate": {"frames": [0, 2], "values": [0, 1]}}},
             "children": [{"type": "text", "content": {"item": true}}]}}]}]})",
        10,
        R"({"lignes": {"type": "list", "of": "text", "default": ["A", "B", "C"]}})");
    MotionComposition composition;
    if (!Parse(json, composition, "repeat")) {
        return;
    }
    MotionFrame frame;
    if (Evaluate(composition, 2, frame)) {
        const MotionEvaluatedNode& column = Child(frame.root, {0});
        Check(column.children.size() == 3, "three copies of the template");
        if (column.children.size() == 3) {
            const auto& copies = column.children;
            Check(BoxIs(copies[0], 0, 0, 50, 20) &&
                      BoxIs(copies[1], 0, 30, 50, 20) &&
                      BoxIs(copies[2], 0, 60, 50, 20),
                  "copies are laid out among the repeat's siblings");
            Check(copies[0].background.red == 1.0 &&
                      copies[1].background.blue == 1.0 &&
                      copies[2].background.red == 1.0,
                  "cycle alternates the background of three copies");
            Check(copies[0].children[0].text == "A" &&
                      copies[2].children[0].text == "C" &&
                      copies[1].id == "sticker[1]" &&
                      Near(copies[2].translate_x, 2.0),
                  "item and index bind each copy; ids are suffixed");
            Check(Near(copies[0].opacity, 1.0) &&
                      Near(copies[1].opacity, 0.0) &&
                      Near(copies[2].opacity, 0.0),
                  "the stagger delays each copy");
        }
    }
    MotionFrame two;
    if (Evaluate(composition, 0, two, R"({"lignes": ["X", "Y"]})")) {
        Check(Child(two.root, {0}).children.size() == 2,
              "a list prop decides how many copies");
    }
    // M3 -- a value of the wrong kind is refused by name.
    Check(EvaluateError(composition, 0, R"({"lignes": ["X", 3]})").code ==
              "PropTypeMismatch",
          "a list prop checks the type of each element");
    Check(ParseError(Card(R"({"type": "text", "content": {"item": true}})"))
                  .code == "InvalidValue",
          "an item binding outside a repeat is refused");
    Check(
        ParseError(
            Card(
                R"({"type": "fill", "children": [{"type": "repeat", "count": 1001,
            "template": {"type": "box"}}]})"))
                .code == "InvalidValue",
        "a repeat is bounded");
    Check(
        ParseError(
            Card(R"({"type": "fill", "children": [{"type": "repeat", "count": 2,
            "style": {"width": 3}, "template": {"type": "box"}}]})"))
                .code == "UnknownField",
        "a repeat has no box to style");
}

void TestTimeScaleAndSeries() {
    const auto scaled = [](const std::string& scale) {
        return Card(
            R"({"type": "fill", "children": [
            {"type": "sequence", "timeScale": )" +
                scale + R"(, "children": [
              {"type": "box", "id": "b", "animate": {"opacity": {"interpolate": {"frames": [0, 20], "values": [0, 1]}}}}]}]})",
            50,
            R"({"vitesse": {"type": "number", "default": 100, "min": 1, "max": 800, "unit": "%"}})");
    };
    const auto opacity = [](const MotionComposition& composition, int64_t frame,
                            const std::string& props = "null") {
        MotionFrame output;
        if (!Evaluate(composition, frame, output, props)) {
            return -1.0;
        }
        const MotionEvaluatedNode* node = FindId(output.root, "b");
        return node != nullptr ? node->opacity : -1.0;
    };
    MotionComposition half;
    MotionComposition twice;
    MotionComposition bound;
    MotionComposition fine;
    if (Parse(scaled(R"("50%")"), half, "50%") &&
        Parse(scaled("2"), twice, "200%") &&
        Parse(scaled(R"({"prop": "vitesse"})"), bound, "bound speed") &&
        Parse(scaled("1.25"), fine, "1.25")) {
        Check(Near(opacity(half, 10), 0.25),
              "at 50 % the same sequence is at local frame 5 at frame 10");
        Check(Near(opacity(twice, 5), 0.5) && Near(opacity(twice, 10), 1.0),
              "at 200 % it is at local frame 10 at frame 5");
        Check(Near(opacity(bound, 10), 0.5) &&
                  Near(opacity(bound, 10, R"({"vitesse": 50})"), 0.25) &&
                  Near(opacity(bound, 5, R"({"vitesse": 200})"), 0.5),
              "a speed prop in percent drives timeScale");
        MotionFrame exact;
        if (Evaluate(fine, 3, exact)) {
            Check(DumpMotionFrame(exact).find("\"localFrame\":3.75") !=
                          std::string::npos &&
                      Near(Child(exact.root, {0, 0}).opacity, 3.75 / 20.0),
                  "a timeScale keeps local time exact (3 x 5/4 = 15/4)");
        }
    }
    Check(ParseError(scaled("0")).code == "InvalidValue" &&
              ParseError(scaled(R"("-50%")")).code == "InvalidValue",
          "a zero or negative speed is refused");

    MotionComposition series;
    if (Parse(
            Card(
                R"({"type": "fill", "children": [{"type": "series", "children": [
            {"type": "sequence", "duration": 10, "children": [{"type": "box", "id": "a"}]},
            {"type": "sequence", "duration": 10, "offset": -2, "children": [{"type": "box", "id": "b"}]},
            {"type": "sequence", "children": [{"type": "box", "id": "c"}]}]}]})"),
            series, "series")) {
        const auto present = [&](int64_t frame) {
            std::string ids;
            MotionFrame output;
            if (Evaluate(series, frame, output)) {
                for (const char* id : {"a", "b", "c"}) {
                    if (FindId(output.root, id) != nullptr) {
                        ids += id;
                    }
                }
            }
            return ids;
        };
        Check(present(0) == "a" && present(9) == "ab" && present(10) == "b" &&
                  present(17) == "b" && present(18) == "c",
              "a series places its sequences end to end, offset included");
    }
    Check(
        ParseError(Card(R"({"type": "series", "children": [{"type": "box"}]})"))
                .code == "InvalidValue",
        "a series holds sequences only");
    Check(ParseError(Card(R"({"type": "series", "children": [
            {"type": "sequence", "from": 3, "duration": 4}]})"))
                  .code == "InvalidValue",
          "a sequence in a series has no from");
    Check(ParseError(Card(R"({"type": "series", "children": [
            {"type": "sequence"}, {"type": "sequence", "duration": 4}]})"))
                  .code == "InvalidValue",
          "only the last sequence of a series may run open");
}

void TestM1bDeterminism() {
    const std::string json = Card(
        R"J({"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 30,
          "clipPath": "polygon(0 0, 100% 0, 90% 100%, 0 100%)"}, "children": [
        {"type": "text", "content": "Une question longue ?", "units": "char",
         "animate": {"opacity": {"interpolate": {"frames": [0, 3], "values": [0, 1]}},
                     "translateY": {"spring": {"from": 20, "to": 0}}},
         "stagger": {"each": 1, "order": "random", "seed": 42}},
        {"type": "repeat", "count": 3, "cycle": {"backgroundColor": ["#E4572E", "#4DABA1", "#3D5AFE"]},
         "template": {"type": "path", "d": "M0 0 C 20 40, 60 40, 80 0", "style": {"stroke": "#fff", "fill": "none"},
           "animate": {"trimEnd": {"interpolate": {"frames": [0, 12], "values": [0, 1], "easing": "ease-in-out"}}}}}]})J");
    MotionComposition composition;
    if (!Parse(json, composition, "M1b determinism")) {
        return;
    }
    MotionFrame one;
    MotionFrame two;
    if (Evaluate(composition, 7, one) && Evaluate(composition, 7, two)) {
        const std::string dump = DumpMotionFrame(one);
        Check(dump == DumpMotionFrame(two),
              "units, paths and repeats evaluate to the same bytes");
        Check(
            dump.find("\"chars\":[") != std::string::npos &&
                dump.find("\"units\":{\"unit\":\"char\"") !=
                    std::string::npos &&
                dump.find("\"pathPaint\":{\"length\":") != std::string::npos &&
                dump.find("\"clipPath\":[[0,0]") != std::string::npos,
            "the canonical dump exposes units, path bounds and clips");
        // REM6 -- a path node's geometry used to share its key with the
        // node's address: a duplicate key, refused by a strict reader.
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(dump, parsed, parseError),
              "a frame with a vector path is JSON a strict reader takes: " +
                  parseError);
    }
}

// M3 -- the three prop kinds M1b left out, bound the way JavaScript reads
// them, and the persistent identities a stored composition carries.
void TestM3PropsAndIdentities() {
    const std::string props = R"({
        "position": {"type": "point", "default": {"x": 10, "y": 20}, "min": 0, "max": 300},
        "logo": {"type": "size", "default": {"width": 80, "height": 40}, "unit": "%"},
        "titre": {"type": "text", "default": "Titre"},
        "carte": {"type": "group", "props": ["position", "logo"]}})";
    const std::string json = Card(
        R"J({"type": "fill", "uid": "01K60000000000000000000001", "children": [
        {"type": "box", "id": "b", "uid": "01K60000000000000000000002",
         "style": {"position": "absolute", "left": {"prop": "position.x"},
                   "top": {"prop": "position.y"}, "width": {"prop": "logo.width"},
                   "height": {"prop": "logo.height"}},
         "animate": {"opacity": {"interpolate": {"frames": [0, 10], "values": [0, 1],
             "uids": ["01K60000000000000000000003", "01K60000000000000000000004"]}}}}]})J",
        50, props);
    MotionComposition composition;
    if (!Parse(json, composition, "points, sizes, groups and uids")) {
        return;
    }
    Check(composition.root.uid == "01K60000000000000000000001" &&
              composition.root.children.size() == 1 &&
              composition.root.children[0].uid == "01K60000000000000000000002",
          "a node keeps the uid the document gave it");
    const MotionAnimation& opacity =
        composition.root.children[0].animate[0].second;
    Check(opacity.track.keyframes().size() == 2 &&
              opacity.track.keyframes()[0].id == "01K60000000000000000000003" &&
              opacity.track.keyframes()[1].id == "01K60000000000000000000004",
          "an interpolation's keyframes are the ULIDs it names, not synthetic");
    MotionFrame frame;
    if (Evaluate(composition, 5, frame)) {
        const MotionEvaluatedNode* box = FindId(frame.root, "b");
        Check(box != nullptr && BoxIs(*box, 10, 20, 320, 80),
              "a point places, a size in % sizes: " +
                  ((box != nullptr) ? Describe(*box) : std::string("missing")));
        Check(box != nullptr && box->uid == "01K60000000000000000000002" &&
                  DumpMotionFrame(frame).find(
                      "\"uid\":\"01K60000000000000000000002\"") !=
                      std::string::npos,
              "the evaluated frame names the document node of each box");
    }
    MotionFrame moved;
    if (Evaluate(
            composition, 5, moved,
            R"({"position": {"x": 100, "y": 0}, "logo": {"width": 50, "height": 50}})")) {
        const MotionEvaluatedNode* box = FindId(moved.root, "b");
        Check(box != nullptr && BoxIs(*box, 100, 0, 200, 100),
              "point and size overrides move and resize the bound box");
    }
    Check(EvaluateError(composition, 0, R"({"position": {"x": 400, "y": 0}})")
                  .code == "PropOutOfRange",
          "a point component outside its bounds is refused");
    Check(
        EvaluateError(composition, 0, R"({"logo": {"width": -1, "height": 5}})")
                .code == "PropOutOfRange",
        "a negative size is refused");
    Check(EvaluateError(composition, 0, R"({"position": [1, 2]})").code ==
              "PropTypeMismatch",
          "a point is an object of x and y, not an array");
    Check(EvaluateError(composition, 0, R"({"titre": 3})").code ==
              "PropTypeMismatch",
          "a number where text is declared is refused by type");
    Check(EvaluateError(composition, 0, R"({"carte": "x"})").code ==
              "PropTypeMismatch",
          "a group has no value to set");
    Check(EvaluateError(composition, 0, R"({"inconnue": 1})").code ==
              "UnknownProp",
          "a prop nothing declares is refused by name");

    const auto boxBoundTo = [](const std::string& binding) {
        return Card(
            R"({"type": "fill", "children": [{"type": "box", "style": {"left": )" +
                binding + R"(}}]})",
            50,
            R"({"position": {"type": "point"}, "g": {"type": "group", "props": ["position"]}})");
    };
    Check(ParseError(boxBoundTo(R"({"prop": "position"})")).code ==
              "PropTypeMismatch",
          "a point binds one component, never whole");
    Check(ParseError(boxBoundTo(R"({"prop": "position.z"})")).code ==
              "PropTypeMismatch",
          "a point has only x and y");
    Check(ParseError(boxBoundTo(R"({"prop": "g"})")).code == "PropTypeMismatch",
          "a group cannot be bound");
    Check(ParseError(Card(R"({"type": "fill"})", 50,
                          R"({"g": {"type": "group", "props": ["absente"]}})"))
                  .code == "UnknownProp",
          "a group names declared props");
    Check(
        ParseError(
            Card(
                R"({"type": "fill"})", 50,
                R"({"a": {"type": "text"}, "g": {"type": "group", "props": ["a"]},
                              "h": {"type": "group", "props": ["a"]}})"))
                .code == "InvalidValue",
        "a prop belongs to one group");
    Check(ParseError(
              Card(R"({"type": "fill"})", 50, R"({"a.b": {"type": "text"}})"))
                  .code == "InvalidValue",
          "a prop name holds no dot, which reads a component");
    Check(ParseError(Card(R"({"type": "fill", "uid": "pas-un-ulid"})")).code ==
              "InvalidValue",
          "a uid is a ULID");
    Check(
        ParseError(Card(R"({"type": "fill", "uid": "01K60000000000000000000001",
            "children": [{"type": "box", "uid": "01K60000000000000000000001"}]})"))
                .code == "DuplicateUid",
        "a uid is used once in a composition");
    Check(ParseError(
              Card(R"({"type": "fill", "animate": {"opacity": {"interpolate":
            {"frames": [0, 1], "values": [0, 1], "uids": ["01K60000000000000000000003"]}}}})"))
                  .code == "InvalidAnimation",
          "uids names every keyframe or none");
}

// A binary that links Yoga's layout objects must still catch the standard
// exceptions libc++ throws. Compiled with -fno-rtti, those objects carried
// reduced type_info for std::length_error and std::bad_array_new_length that
// the linker preferred program-wide, and catch (const std::exception&) no
// longer matched. CMakeLists.txt removes the flag; this keeps it removed.
// ---------------------------------------------------------------------------
// M6 -- versions (named prop sets) and formats (other sizes, with the props
// that adapt the card to them), resolved before evaluation.

std::string VersionedCard(const std::string& formats,
                          const std::string& variants) {
    return R"({"width": 400, "height": 800, "fps": 25, "duration": 20,
      "props": {"titre": {"type": "text", "default": "Titre"},
                "corps": {"type": "number", "default": 20, "min": 10, "max": 100},
                "marge": {"type": "number", "default": 10, "min": 0, "max": 50},
                "carte": {"type": "group", "props": ["titre"]}},
      "formats": )" +
           formats + R"(, "variants": )" + variants + R"(,
      "root": {"type": "fill", "style": {"justifyContent": "center", "alignItems": "center"},
        "children": [{"type": "box", "id": "bloc", "style": {"padding": {"prop": "marge"}},
          "children": [{"type": "text", "id": "titre", "content": {"prop": "titre"},
                        "style": {"fontSize": {"prop": "corps"}}}]}]}})";
}

void TestM6VariantsAndFormats() {
    const std::string formats =
        R"({"vertical": {"width": 400, "height": 800},
            "large": {"width": 800, "height": 400, "props": {"corps": 40, "marge": 20}}})";
    const std::string variants =
        R"({"court": {"props": {"titre": "Deux"}},
            "long": {"props": {"titre": "Quatre mots ici"}},
            "defaut": {}})";
    MotionComposition composition;
    if (!Parse(VersionedCard(formats, variants), composition,
               "versions and formats")) {
        return;
    }
    Check(composition.formats.size() == 2 &&
              composition.formats[0].name == "vertical" &&
              composition.formats[1].name == "large" &&
              composition.formats[1].width == 800 &&
              composition.formats[1].height == 400 &&
              composition.variants.size() == 3 &&
              composition.variants[0].name == "court" &&
              composition.variants[2].name == "defaut" &&
              composition.variants[2].props.IsObject() &&
              composition.variants[2].props.AsObject().empty(),
          "formats and versions keep their declaration order");

    // A version in a format: the format's size and props, the version's
    // text; flexbox recentres the block, the engine measures it.
    const auto resolveAndEvaluate =
        [&](const std::string& variant, const std::string& format,
            const std::string& overrides, MotionFrame& frame) {
            mcp_json::Value parsed;
            std::string parseError;
            mcp_json::Value::Parse(overrides, parsed, parseError);
            MotionComposition resolved;
            mcp_json::Value props;
            MotionError error;
            const bool ok = ResolveMotionVariant(
                composition, variant, format, parsed, resolved, props, error);
            Check(ok, "version '" + variant + "' in format '" + format +
                          "' resolves: " + error.message);
            return ok && Evaluate(resolved, 5, frame, props.Dump());
        };
    MotionFrame longLarge;
    if (resolveAndEvaluate("long", "large", "null", longLarge)) {
        const MotionEvaluatedNode& bloc = Child(longLarge.root, {0});
        const MotionEvaluatedNode& titre = Child(longLarge.root, {0, 0});
        // "Quatre mots ici": 15 code points at 40 / 2 = 20 px, 48 px high,
        // plus 20 px of padding on each side, centred in 800 x 400.
        Check(longLarge.width == 800 && longLarge.height == 400 &&
                  titre.text == "Quatre mots ici" &&
                  BoxIs(bloc, 230.0, 156.0, 340.0, 88.0),
              "the long version in 16:9 takes the format's size and type, "
              "and flexbox centres it: " +
                  Describe(bloc));
    }
    MotionFrame shortVertical;
    if (resolveAndEvaluate("court", "vertical", "null", shortVertical)) {
        const MotionEvaluatedNode& bloc = Child(shortVertical.root, {0});
        // "Deux": 4 x 10 px, 24 px high, 10 px of padding.
        Check(shortVertical.width == 400 && shortVertical.height == 800 &&
                  Child(shortVertical.root, {0, 0}).text == "Deux" &&
                  BoxIs(bloc, 170.0, 378.0, 60.0, 44.0),
              "the short version in 9:16 keeps the composition's props: " +
                  Describe(bloc));
    }
    MotionFrame own;
    MotionFrame plain;
    if (resolveAndEvaluate("", "", "null", own) &&
        Evaluate(composition, 5, plain)) {
        Check(DumpMotionFrame(own) == DumpMotionFrame(plain),
              "no version and no format is the composition as it stands");
    }
    MotionFrame overridden;
    if (resolveAndEvaluate("court", "large", R"({"titre": "Autre"})",
                           overridden)) {
        Check(Child(overridden.root, {0, 0}).text == "Autre",
              "a caller's override for one frame wins over the version");
    }
    MotionFrame again;
    if (resolveAndEvaluate("long", "large", "null", again)) {
        Check(DumpMotionFrame(again) == DumpMotionFrame(longLarge),
              "the same version in the same format evaluates to the same "
              "bytes");
    }

    // Unknown names are refused by name, with what the composition declares.
    MotionComposition resolved;
    mcp_json::Value props;
    MotionError error;
    Check(!ResolveMotionVariant(composition, "moyen", "", mcp_json::Value(),
                                resolved, props, error) &&
              error.code == "UnknownVariant" && error.path == "variant" &&
              error.message.find("court, long, defaut") != std::string::npos,
          "an unknown version is refused as UnknownVariant: " + error.message);
    Check(!ResolveMotionVariant(composition, "", "carre", mcp_json::Value(),
                                resolved, props, error) &&
              error.code == "UnknownFormat" && error.path == "format" &&
              error.message.find("vertical, large") != std::string::npos,
          "an unknown format is refused as UnknownFormat: " + error.message);
    MotionComposition bare;
    if (Parse(Card(R"({"type": "fill"})"), bare, "no versions")) {
        Check(
            !ResolveMotionVariant(bare, "court", "", mcp_json::Value(),
                                  resolved, props, error) &&
                error.code == "UnknownVariant" &&
                error.message.find("declares no variants") != std::string::npos,
            "a composition without versions says so: " + error.message);
    }

    // What the parser refuses, each at its path, with M3's prop codes.
    struct Refusal {
        const char* label;
        std::string formats;
        std::string variants;
        const char* code;
        const char* path;
    };
    const Refusal refusals[] = {
        {"a version that sets an undeclared prop", "{}",
         R"({"a": {"props": {"sous-titre": "x"}}})", "UnknownProp",
         "variants.a.props.sous-titre"},
        {"a version outside a prop's bounds", "{}",
         R"({"a": {"props": {"corps": 500}}})", "PropOutOfRange",
         "variants.a.props.corps"},
        {"a version of the wrong type", "{}",
         R"({"a": {"props": {"titre": 3}}})", "PropTypeMismatch",
         "variants.a.props.titre"},
        {"a version that sets a group", "{}",
         R"({"a": {"props": {"carte": "x"}}})", "PropTypeMismatch",
         "variants.a.props.carte"},
        {"a format outside a prop's bounds",
         R"({"f": {"width": 10, "height": 10, "props": {"marge": -1}}})", "{}",
         "PropOutOfRange", "formats.f.props.marge"},
        {"a format without a height", R"({"f": {"width": 10}})", "{}",
         "InvalidValue", "formats.f.height"},
        {"a format name that cannot name a file",
         R"({"16:9": {"width": 16, "height": 9}})", "{}", "InvalidValue",
         "formats.16:9"},
        {"the draft's flat version", "{}", R"({"a": {"titre": "x"}})",
         "UnknownField", "variants.a.titre"},
        {"the draft's format inside a version", "{}",
         R"({"a": {"props": {}, "format": "1920x1080"}})", "UnknownField",
         "variants.a.format"},
        {"a prop a format adapts and a version varies",
         R"({"f": {"width": 10, "height": 10, "props": {"corps": 30}}})",
         R"({"a": {"props": {"corps": 40}}})", "InvalidComposition",
         "variants.a.props.corps"},
        {"versions that are not an object", "{}", "[]", "InvalidValue",
         "composition.variants"},
    };
    for (const Refusal& refusal : refusals) {
        const MotionError refused =
            ParseError(VersionedCard(refusal.formats, refusal.variants));
        Check(refused.code == refusal.code && refused.path == refusal.path,
              std::string(refusal.label) + " is refused as " + refusal.code +
                  " at " + refusal.path + ", got " + refused.code + " at " +
                  refused.path + ": " + refused.message);
    }
}

// M6 -- a batch planned: names, destinations and FFmpeg's arguments, and
// what is refused before any frame is drawn. No file is written here.
void TestM6VideoPlan() {
    MotionComposition composition;
    if (!Parse(VersionedCard(
                   R"({"vertical": {"width": 400, "height": 800},
                       "impair": {"width": 401, "height": 800}})",
                   R"({"court": {"props": {"titre": "Deux"}},
                       "long": {"props": {"titre": "Quatre mots ici"}}})"),
               composition, "a card to plan")) {
        return;
    }
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path();
    MotionVideoOptions options;
    options.output = (directory / "cm-m6-{variant}-{format}.mov").string();
    options.alpha = true;
    std::vector<MotionVideoItem> items;
    MotionError error;
    Check(
        PlanMotionVideos(composition, options, items, error) &&
            items.size() == 4 && items[0].variant == "court" &&
            items[0].format == "vertical" && items[1].format == "impair" &&
            items[3].variant == "long" && items[1].composition.width == 401 &&
            items[3].output_path ==
                (directory / "cm-m6-long-impair.mov").lexically_normal() &&
            items[3].props.Find("titre")->AsString() == "Quatre mots ici",
        "every version in every format, versions outermost: " + error.message);
    if (items.size() == 4) {
        const ExportPlan plan =
            MotionVideoExportPlan(items[1], options, "/frames/%06d.png");
        const auto has = [&](const std::string& flag,
                             const std::string& value) {
            for (size_t index = 0; index + 1 < plan.arguments.size(); ++index) {
                if (plan.arguments[index] == flag &&
                    plan.arguments[index + 1] == value) {
                    return true;
                }
            }
            return false;
        };
        Check(has("-framerate", "25/1") && has("-start_number", "0") &&
                  has("-i", "/frames/%06d.png") && has("-c:v", "prores_ks") &&
                  has("-profile:v", "4444") && has("-r", "25/1") &&
                  has("-frames:v", "20") && plan.total_frames == 20 &&
                  plan.arguments.back() == plan.temporary_output_path &&
                  plan.temporary_output_path.find(".part.mov") !=
                      std::string::npos &&
                  plan.settings.output_path == items[1].output_path,
              "ProRes 4444 reads the sequence at the exact cadence and "
              "writes the duration in frames");
    }

    const auto refused = [&](const MotionVideoOptions& asked, const char* code,
                             const std::string& fragment,
                             const std::string& label) {
        std::vector<MotionVideoItem> planned;
        MotionError refusal;
        Check(!PlanMotionVideos(composition, asked, planned, refusal) &&
                  refusal.code == code &&
                  refusal.message.find(fragment) != std::string::npos,
              label + " is refused as " + code + ": " + refusal.code + " " +
                  refusal.message);
    };
    MotionVideoOptions h264 = options;
    h264.alpha = false;
    refused(h264, "InvalidExport", "even dimensions", "an odd width for H.264");
    MotionVideoOptions twice = options;
    twice.variants = {"court", "court"};
    refused(twice, "InvalidValue", "twice", "a version named twice");
    MotionVideoOptions unknown = options;
    unknown.variants = {"moyen"};
    refused(unknown, "UnknownVariant", "court, long", "an unknown version");
    MotionVideoOptions typo = options;
    typo.output = (directory / "cm-m6-{version}-{format}.mov").string();
    refused(typo, "InvalidExport", "{variant} and {format}",
            "a placeholder that is not one");
    MotionVideoOptions missing = options;
    missing.output =
        (directory / "absent-cm-m6" / "{variant}-{format}.mov").string();
    refused(missing, "InvalidExport", "directory does not exist",
            "a missing directory");

    MotionComposition bare;
    if (Parse(Card(R"({"type": "fill"})"), bare, "a card without versions")) {
        MotionVideoOptions plain;
        plain.output = (directory / "cm-m6-{variant}.mp4").string();
        std::vector<MotionVideoItem> planned;
        Check(
            !PlanMotionVideos(bare, plain, planned, error) &&
                error.message.find("declares no versions") != std::string::npos,
            "{variant} is refused when there is no version to name");
        plain.output = (directory / "cm-m6-carte.mp4").string();
        Check(PlanMotionVideos(bare, plain, planned, error) &&
                  planned.size() == 1 && planned[0].variant.empty() &&
                  planned[0].composition.width == 400,
              "a composition without versions or formats renders once, as "
              "it stands: " +
                  error.message);
    }
}

void TestStandardExceptionsSurviveYoga() {
    MotionComposition composition;
    MotionFrame frame;
    if (Parse(R"({"width": 10, "height": 10, "fps": 25, "duration": 1,
          "root": {"type": "fill"}})",
              composition, "minimal layout")) {
        Evaluate(composition, 0, frame);
    }
    bool lengthCaught = false;
    try {
        std::vector<int> values;
        values.reserve(values.max_size() + 1);
    } catch (const std::exception&) {
        lengthCaught = true;
    }
    bool rangeCaught = false;
    try {
        static_cast<void>(std::string().at(1));
    } catch (const std::exception&) {
        rangeCaught = true;
    }
    Check(lengthCaught && rangeCaught,
          "standard exceptions stay catchable as std::exception next to Yoga");
}

}  // namespace

// MB11 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. An animated value
// bound to a prop: the default's value when nothing is set, the instance's
// otherwise, through a map when there is one; and the refusals, by name.
void TestBoundAnimationValues() {
    const std::string card = R"json({"width": 200, "height": 100, "fps": 25,
      "duration": 20,
      "props": {"chute": {"type": "number", "default": 100, "min": 0,
                          "max": 400},
                "montre": {"type": "bool", "default": true},
                "teinte": {"type": "color", "default": "#ff0000"}},
      "root": {"type": "fill", "children": [{"type": "box", "id": "mot",
        "style": {"width": 200, "height": 100},
        "animate": {
          "translateY": {"interpolate": {"frames": [0, 10],
                         "values": [{"prop": "chute"}, 0]}},
          "backgroundColor": {"interpolate": {"frames": [0, 10],
                         "values": [{"prop": "teinte"}, "#000000"]}},
          "clipPath": {"interpolate": {"frames": [0, 10], "values": [
            {"prop": "montre", "map": {"true": "inset(0% 100% 0% 0%)",
                                       "false": "inset(0% 0% 0% 0%)"}},
            "inset(0% 0% 0% 0%)"]}}}}]}})json";
    MotionComposition composition;
    if (!Parse(card, composition, "bound animated values")) {
        return;
    }
    const MotionAnimation& drop =
        composition.root.children[0].animate[0].second;
    Check(drop.bound_values.size() == 1 && drop.bound_values[0].first == 0 &&
              drop.track.keyframes()[0].value == 100.0,
          "the parser keeps the binding, and the default's value in the track");
    const auto at = [&](int64_t frame, const std::string& props) {
        MotionFrame output;
        Evaluate(composition, frame, output, props);
        return output;
    };
    MotionFrame frame = at(5, "null");
    Check(Child(frame.root, {0}).translate_y == 50.0,
          "the default: halfway from 100 to 0 is 50 (" +
              std::to_string(Child(frame.root, {0}).translate_y) + ")");
    frame = at(5, R"({"chute": 40})");
    Check(Child(frame.root, {0}).translate_y == 20.0,
          "the instance's value: halfway from 40 to 0 is 20 (" +
              std::to_string(Child(frame.root, {0}).translate_y) + ")");
    frame = at(0, R"({"teinte": "#0000ff"})");
    Check(Child(frame.root, {0}).has_background &&
              Child(frame.root, {0}).background.blue > 0.99 &&
              Child(frame.root, {0}).background.red < 0.01,
          "a bound colour is the instance's");
    const auto clipWidth = [](const MotionEvaluatedNode& node) {
        return node.clip_shape.kind == MotionClipKind::Inset
                   ? node.clip_shape.rect.width
                   : -1.0;
    };
    frame = at(0, "null");
    const double hidden = clipWidth(Child(frame.root, {0}));
    frame = at(0, R"({"montre": false})");
    const double whole = clipWidth(Child(frame.root, {0}));
    Check(hidden == 0.0 && whole == 200.0,
          "a clip-path bound through a map: closed by default, open for the "
          "instance (" +
              std::to_string(hidden) + ", " + std::to_string(whole) + ")");
    // The same composition, evaluated twice in a row with other props,
    // gives each its own value: nothing of one instance leaks into the next.
    frame = at(5, "null");
    Check(Child(frame.root, {0}).translate_y == 50.0,
          "the next frame without props is the default's again");

    const auto refused = [](const std::string& values,
                            const std::string& property,
                            const std::string& reason) {
        const MotionError error = ParseError(
            R"({"width": 100, "height": 100, "fps": 25, "duration": 10,
              "props": {"n": {"type": "number", "default": 1},
                        "t": {"type": "text", "default": "x"}},
              "root": {"type": "fill", "children": [{"type": "box",
                "animate": {")" +
            property + R"(": {"interpolate": {"frames": [0, 5],
                  "values": )" +
            values + "}}}}]}}");
        return error.message.find(reason) != std::string::npos;
    };
    Check(refused(R"([{"prop": "t"}, 0])", "translateY",
                  "every value the bound prop can give is a number"),
          "a text prop bound to a number is refused");
    Check(refused(R"([{"prop": "n", "map": {"1": "large"}}, 0])", "translateY",
                  "every value the bound prop can give is a number"),
          "a map that gives a word to a number is refused");
    Check(refused(R"json([{"prop": "n", "map": {"1": "inset(0% 0% 0% 0%)",
                     "2": "circle(10% at 50% 50%)"}}, "inset(0% 0% 0% 0%)"])json",
                  "clipPath", "shape of the same kind"),
          "a map that changes the shape is refused");
    Check(refused(R"json([{"prop": "n"}, "blur(2px)"])json", "filter",
                  "a gradient, a filter or a path is written"),
          "a filter is written, not bound");
    Check(refused(R"([{"index": true}, 0])", "translateY", "repeat template"),
          "an index outside a repeat is refused, as anywhere");
}

int main() {
    TestStandardExceptionsSurviveYoga();
    TestBoundAnimationValues();
    TestSpring();
    TestTitleCardLayout();
    TestFlexboxCases();
    TestSequencesAndAnimation();
    TestEasingPerSegment();
    TestKeysBetweenFrames();
    TestGsapCurves();
    TestFlashAsAClip();
    TestBindings();
    TestTimecode();
    TestDashArray();
    TestTransformOriginAndSkew();
    TestFilterSeeds();
    TestRefusals();
    TestDeterminismAndRates();
    TestTextUnits();
    TestTextRanges();
    TestTextHighlights();
    TestPaths();
    TestClipPath();
    TestMeshWarp();
    TestRepeat();
    TestTimeScaleAndSeries();
    TestM1bDeterminism();
    TestM3PropsAndIdentities();
    TestM6VariantsAndFormats();
    TestM6VideoPlan();
    TestFormatDocumentExamples();
    if (failures != 0) {
        std::cerr << failures << " motion assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion engine\n";
    return 0;
}
