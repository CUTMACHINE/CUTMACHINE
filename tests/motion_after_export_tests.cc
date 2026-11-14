// MOTION-AFTER-2026-09 -- the description of a composition for After
// Effects, read without After: what each node became, where it sits, and
// the keyframes and eases After is given. MA1 -- export_motion_after, the
// tool that describes and hands the description to the bridge, with a
// describer and a bridge that stand in for Core Text and After. The build
// itself and the comparison of After's frames with the engine's run by
// hand against After (tools/after-bridge/scripts/motion_import.jsx).

#include "AfterBridge.h"
#include "Document.h"
#include "Json.h"
#include "Keyframes.h"
#include "McpBackend.h"
#include "McpTools.h"
#include "MotionAfterExport.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "Ulid.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

// Fixed advances, a baseline at 0.8 of the size, and one PostScript name.
class FakeMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& /*error*/) override {
        const double advance = style.font_size * 0.5 + style.letter_spacing;
        layout = MotionTextLayout{};
        std::stringstream stream(text);
        std::string content;
        while (std::getline(stream, content, '\n')) {
            MotionTextLine line;
            line.text = content;
            double x = 0.0;
            for (size_t index = 0; index < content.size(); ++index) {
                line.clusters.push_back({index, 1, x, advance});
                x += advance;
            }
            line.width = x;
            line.height = style.font_size * 1.2;
            line.y = static_cast<double>(layout.lines.size()) * line.height;
            const double frame = max_width >= 0.0 ? max_width : x;
            line.x = style.text_align == "center"  ? (frame - x) / 2.0
                     : style.text_align == "right" ? frame - x
                                                   : 0.0;
            line.baseline = line.y + style.font_size * 0.8;
            layout.width = std::max(layout.width, x);
            layout.height += line.height;
            layout.lines.push_back(line);
        }
        return true;
    }

    bool ImageSize(const std::string& /*source*/, double& /*width*/,
                   double& /*height*/, MotionError& error) override {
        error = {"MediaMissing", "", "no media"};
        return false;
    }

    bool FontPostScriptName(const MotionTextStyle& /*style*/, std::string& name,
                            MotionError& /*error*/) override {
        name = "Fake-Bold";
        return true;
    }
};

const mcp_json::Value* Comp(const mcp_json::Value& description,
                            const std::string& name) {
    for (const mcp_json::Value& comp : description.Find("comps")->AsArray()) {
        if (comp.Find("name")->AsString() == name) {
            return &comp;
        }
    }
    return nullptr;
}

const mcp_json::Value* Layer(const mcp_json::Value& comp,
                             const std::string& name) {
    for (const mcp_json::Value& layer : comp.Find("layers")->AsArray()) {
        if (layer.Find("name")->AsString() == name) {
            return &layer;
        }
    }
    return nullptr;
}

bool Near(const mcp_json::Value& value, double expected) {
    // Numbers are written with four decimals; compare their spelling.
    const double read = std::stod(value.Dump());
    return std::abs(read - expected) < 1e-3;
}

void TestEase() {
    // cubic-bezier(0.16, 1, 0.3, 1) from 120 to 100 in 0.4 s: the handles
    // leave at 16 % and 70 % of the segment, with the curve's slopes.
    MotionAfterEase out;
    MotionAfterEase in;
    MotionAfterBezierEase(0.16, 1.0, 0.3, 1.0, 120.0, 100.0, 0.4, out, in);
    Check(std::abs(out.influence - 16.0) < 1e-9 &&
              std::abs(out.speed - (-50.0 / 0.16)) < 1e-9 &&
              std::abs(in.influence - 70.0) < 1e-9 && std::abs(in.speed) < 1e-9,
          "a CSS curve becomes After's speed and influence");
}

void TestDescription() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 50,
      "root": {"type": "fill", "style": {"backgroundColor": "#000000", "fontFamily": "Fake"}, "children": [
        {"type": "sequence", "from": 0, "duration": 25, "id": "TITRE#a", "children": [
          {"type": "text", "content": "TITRE",
           "style": {"position": "absolute", "left": 0, "top": 50, "width": 400, "textAlign": "center", "fontSize": 40},
           "animate": {"scale": {"interpolate": {"frames": [0, 10], "values": [1.2, 1],
                                                 "easing": "cubic-bezier(0.16, 1, 0.3, 1)"}}}},
          {"type": "text", "content": "un deux", "units": "word", "stagger": {"each": 2},
           "style": {"position": "absolute", "left": 10, "top": 120, "fontSize": 20},
           "animate": {"opacity": {"interpolate": {"frames": [0, 1], "values": [0, 1], "easing": "step-end"}}}},
          {"type": "text", "content": {"timecode": "composition"},
           "style": {"position": "absolute", "left": 300, "top": 180, "fontSize": 10}}]},
        {"type": "sequence", "from": 25, "duration": 25, "children": [
          {"type": "box", "id": "groupe",
           "style": {"position": "absolute", "left": 0, "top": 0, "width": 400, "height": 200},
           "animate": {"opacity": {"interpolate": {"frames": [0, 5], "values": [0, 1]}}},
           "children": [
             {"type": "box", "style": {"position": "absolute", "left": 10, "top": 10, "width": 50,
                                       "height": 50, "backgroundColor": "#FF0000"}}]}]}]}})J";
    MotionComposition composition;
    MotionError error;
    if (!ParseMotionComposition(json, composition, error)) {
        Check(false, "the composition parses: " + error.message);
        return;
    }
    FakeMeasurer measurer;
    MotionAfterExportOptions options;
    options.name = "Film";
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!ExportMotionToAfter(composition, mcp_json::Value(), measurer, {},
                             options, description, report, error)) {
        Check(false, "the composition is described: " + error.message);
        return;
    }
    Check(description.Find("schema")->AsString() == kMotionAfterSchema &&
              report.comps == 4,
          "four compositions: the film, two scenes, one group: " +
              std::to_string(report.comps));

    const mcp_json::Value* first = Comp(description, "01 TITRE");
    const mcp_json::Value* second = Comp(description, "02 groupe");
    Check(first != nullptr && second != nullptr,
          "scenes are numbered and named by their id (before '#') or their "
          "biggest text");
    if (first == nullptr || second == nullptr) {
        return;
    }
    const mcp_json::Value* title = Layer(*first, "TITRE");
    Check(title != nullptr &&
              title->Find("text")->Find("justify")->AsString() == "center" &&
              title->Find("text")->Find("font")->AsString() == "Fake-Bold" &&
              Near(title->Find("transform")->Find("position")->AsArray()[0],
                   200.0),
          "a centred text is justified at the centre, on the frame's middle");
    if (title != nullptr) {
        const mcp_json::Value& key =
            title->Find("transform")->Find("keys")->Find("scale")->AsArray()[0];
        const mcp_json::Value& ease = key.Find("ease_out")->AsArray()[0];
        Check(key.Find("out")->AsString() == "bezier" &&
                  Near(ease.AsArray()[0], -20.0 / 0.4 / 0.16) &&
                  Near(ease.AsArray()[1], 16.0),
              "the scale's curve is After's ease, in percent per second");
    }
    const mcp_json::Value* words = Layer(*first, "un deux");
    if (words != nullptr) {
        const mcp_json::Value* units = words->Find("text")->Find("units");
        Check(units != nullptr &&
                  units->Find("based_on")->AsString() == "words" &&
                  units->Find("order")->AsString() == "forward" &&
                  units->Find("each")->Dump() == "2" &&
                  units->Find("animators")
                          ->AsArray()[0]
                          .Find("slider")
                          ->AsString() == "opacité (unités)",
              "words in cascade: a delay and a slider an animator reads");
    } else {
        Check(false, "the words are one text layer");
    }
    bool timecode = false;
    for (const mcp_json::Value& layer : first->Find("layers")->AsArray()) {
        const mcp_json::Value* text = layer.Find("text");
        if (text != nullptr && text->Find("expression") != nullptr) {
            const std::string& expression =
                text->Find("expression")->AsString();
            timecode = expression.find("var n = 25") != std::string::npos &&
                       expression.find("+ 0;") != std::string::npos;
        }
    }
    Check(timecode, "a timecode is an expression of the source text");
    const mcp_json::Value* group = Layer(*second, "groupe");
    Check(
        group != nullptr && group->Find("kind")->AsString() == "precomp" &&
            group->Find("transform")->Find("keys")->Find("opacity") != nullptr,
        "a group with an opacity is a precomposition carrying it");
}

// MOTION-BIBLIOTHEQUE-2026-10 MB5 -- a curve per segment is what After
// keeps on each key: the ease leaving it, and the one arriving at it.
void TestEasePerSegment() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 50,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "sequence", "from": 0, "duration": 50, "id": "TITRE", "children": [
          {"type": "text", "content": "TITRE",
           "style": {"position": "absolute", "left": 0, "top": 50, "width": 400, "fontSize": 40},
           "animate": {"scale": {"interpolate": {"frames": [0, 10, 20, 30], "values": [1.2, 1, 1, 0.8],
             "easing": ["cubic-bezier(0.16, 1, 0.3, 1)", "step-end", "cubic-bezier(0.7, 0, 0.84, 0)"]}}}}]}]}})J";
    MotionComposition composition;
    MotionError error;
    FakeMeasurer measurer;
    MotionAfterExportOptions options;
    options.name = "Film";
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!ParseMotionComposition(json, composition, error) ||
        !ExportMotionToAfter(composition, mcp_json::Value(), measurer, {},
                             options, description, report, error)) {
        Check(false, "a curve per segment is described: " + error.message);
        return;
    }
    const mcp_json::Value* scene = Comp(description, "01 TITRE");
    const mcp_json::Value* title =
        scene != nullptr ? Layer(*scene, "TITRE") : nullptr;
    const mcp_json::Value* keys =
        title != nullptr ? title->Find("transform")->Find("keys")->Find("scale")
                         : nullptr;
    if (keys == nullptr || keys->AsArray().size() != 4) {
        Check(false, "the scale keeps its four keys");
        return;
    }
    const auto text = [&](size_t index, const char* field) {
        const mcp_json::Value* value = keys->AsArray()[index].Find(field);
        return value != nullptr ? value->AsString() : std::string();
    };
    const auto has = [&](size_t index, const char* field) {
        return keys->AsArray()[index].Find(field) != nullptr;
    };
    Check(text(0, "out") == "bezier" && text(1, "in") == "bezier" &&
              text(1, "out") == "hold" && text(2, "in") == "hold" &&
              text(2, "out") == "bezier" && text(3, "in") == "bezier",
          "each key leaves by its segment's curve and arrives by the one "
          "before");
    Check(has(0, "ease_out") && has(1, "ease_in") && !has(1, "ease_out") &&
              !has(2, "ease_in") && has(2, "ease_out") && has(3, "ease_in") &&
              !has(3, "ease_out"),
          "a held segment has no ease; the last key leaves nowhere");
    if (has(0, "ease_out") && has(2, "ease_out") && has(3, "ease_in")) {
        const mcp_json::Value& entrance =
            keys->AsArray()[0].Find("ease_out")->AsArray()[0];
        const mcp_json::Value& leaving =
            keys->AsArray()[2].Find("ease_out")->AsArray()[0];
        const mcp_json::Value& arriving =
            keys->AsArray()[3].Find("ease_in")->AsArray()[0];
        // The exit, 100 % to 80 % in 0.4 s: no speed on 70 % of the way out
        // of its key, then the curve's slope on the last 16 %.
        Check(Near(entrance.AsArray()[0], -20.0 / 0.4 / 0.16) &&
                  Near(entrance.AsArray()[1], 16.0) &&
                  Near(leaving.AsArray()[0], 0.0) &&
                  Near(leaving.AsArray()[1], 70.0) &&
                  Near(arriving.AsArray()[0], -20.0 / 0.4 / 0.16) &&
                  Near(arriving.AsArray()[1], 16.0),
              "the entrance and the exit each give After their own ease: " +
                  leaving.Dump() + " " + arriving.Dump());
    }
}

// MA8 -- a sequence's own animation runs in its local time, and its layer
// sits in the parent's time: a scene that starts at 30 and fades out over
// its local frames 10 to 20 fades on the parent's frames 40 to 50. Written
// at 10 and 20, the scene was gone before it began. MA9 -- After takes a
// whole tracking only: 4 px of spacing at 30 px is 133.33 thousandths of an
// em, refused by After, which stopped the build half way.
void TestSequenceTimeAndTracking() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 60,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "sequence", "from": 0, "duration": 30, "id": "UN", "children": [
          {"type": "text", "content": "UN",
           "style": {"position": "absolute", "left": 0, "top": 50, "fontSize": 30,
                     "letterSpacing": 4}}]},
        {"type": "sequence", "from": 30, "duration": 30, "id": "DEUX",
         "animate": {"opacity": {"interpolate": {"frames": [10, 20], "values": [1, 0]}},
                     "translateY": {"interpolate": {"frames": [0, 20], "values": [40, 0]}}},
         "children": [
          {"type": "text", "content": "DEUX",
           "style": {"position": "absolute", "left": 0, "top": 50, "fontSize": 30}}]}]}})J";
    MotionComposition composition;
    MotionError error;
    FakeMeasurer measurer;
    MotionAfterExportOptions options;
    options.name = "Film";
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!ParseMotionComposition(json, composition, error) ||
        !ExportMotionToAfter(composition, mcp_json::Value(), measurer, {},
                             options, description, report, error)) {
        Check(false, "two scenes are described: " + error.message);
        return;
    }
    const mcp_json::Value* main = nullptr;
    for (const mcp_json::Value& comp : description.Find("comps")->AsArray()) {
        if (comp.Find("id")->AsString() ==
            description.Find("main")->AsString()) {
            main = &comp;
        }
    }
    const mcp_json::Value* scene =
        main != nullptr ? Layer(*main, "02 DEUX") : nullptr;
    const mcp_json::Value* transform =
        scene != nullptr ? scene->Find("transform") : nullptr;
    const mcp_json::Value* fade = transform != nullptr
                                      ? transform->Find("keys")->Find("opacity")
                                      : nullptr;
    const mcp_json::Value* rise =
        transform != nullptr ? transform->Find("keys")->Find("position.y")
                             : nullptr;
    if (fade == nullptr || fade->AsArray().size() != 2 || rise == nullptr ||
        rise->AsArray().size() != 2) {
        Check(false, "the second scene keeps its fade and its rise");
        return;
    }
    const auto frame = [](const mcp_json::Value& value) {
        int64_t number = -1;
        return value.AsInt64(number) ? number : -1;
    };
    Check(frame(*scene->Find("start")) == 30 &&
              frame(*fade->AsArray()[0].Find("f")) == 40 &&
              frame(*fade->AsArray()[1].Find("f")) == 50 &&
              frame(*rise->AsArray()[0].Find("f")) == 30 &&
              frame(*rise->AsArray()[1].Find("f")) == 50,
          "a scene's own keys are moved by its start into the parent's "
          "time: " +
              fade->Dump() + " " + rise->Dump());
    Check(Near(*transform->Find("opacity"), 100.0),
          "its static opacity is the one of its first frame");

    const mcp_json::Value* first = Comp(description, "01 UN");
    const mcp_json::Value* word =
        first != nullptr ? Layer(*first, "UN") : nullptr;
    const mcp_json::Value* text =
        word != nullptr ? word->Find("text") : nullptr;
    Check(text != nullptr && Near(*text->Find("tracking"), 133.0),
          "the tracking is a whole number of thousandths of an em");
    bool said = false;
    for (const std::string& note : report.notes) {
        said = said || note.find("arrondie à 133") != std::string::npos;
    }
    Check(said, "the report says the spacing was rounded");
}

// MB11 -- a key bound to a prop reaches After with the instance's value,
// not the prop's default.
void TestBoundKey() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 50,
      "props": {"depart": {"type": "number", "default": 1.2}},
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "sequence", "from": 0, "duration": 50, "id": "TITRE", "children": [
          {"type": "text", "content": "TITRE",
           "style": {"position": "absolute", "left": 0, "top": 50, "width": 400, "fontSize": 40},
           "animate": {"scale": {"interpolate": {"frames": [0, 10],
             "values": [{"prop": "depart"}, 1]}}}}]}]}})J";
    MotionComposition composition;
    MotionError error;
    FakeMeasurer measurer;
    MotionAfterExportOptions options;
    options.name = "Film";
    mcp_json::Value props = mcp_json::Value::MakeObject();
    props.Set("depart", mcp_json::Value::MakeDouble(2.0));
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!ParseMotionComposition(json, composition, error) ||
        !ExportMotionToAfter(composition, props, measurer, {}, options,
                             description, report, error)) {
        Check(false, "a bound key is described: " + error.message);
        return;
    }
    const mcp_json::Value* scene = Comp(description, "01 TITRE");
    const mcp_json::Value* title =
        scene != nullptr ? Layer(*scene, "TITRE") : nullptr;
    const mcp_json::Value* keys =
        title != nullptr ? title->Find("transform")->Find("keys")->Find("scale")
                         : nullptr;
    if (keys == nullptr || keys->AsArray().size() != 2) {
        Check(false, "the bound scale keeps its two keys");
        return;
    }
    const mcp_json::Value& first = *keys->AsArray()[0].Find("v");
    Check(Near(first.AsArray()[0], 200.0),
          "the first key is the instance's 2, not the default's 1.2: " +
              first.Dump());
}

// MA5 -- a text in cascade glitches through a box around it, whose filter
// covers the whole text; After gets the glitch as an effect on the text
// layer, not a precomposition around one layer. The box stays one when
// folding would lose something: an opacity of its own, a filter of the
// text's.
void TestFilterOverUnits() {
    const auto describe = [](const std::string& box, const std::string& text,
                             mcp_json::Value& description,
                             MotionAfterExportReport& report) {
        const std::string json =
            R"J({"width": 400, "height": 200, "fps": 25, "duration": 50,
              "root": {"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 40},
                "children": [{"type": "box", "id": "glitch", )J" +
            box + R"J(, "children": [{"type": "text", "content": "GLITCH", )J" +
            text + R"J(}]}]}})J";
        MotionComposition composition;
        MotionError error;
        FakeMeasurer measurer;
        MotionAfterExportOptions options;
        options.name = "Film";
        const bool ok =
            ParseMotionComposition(json, composition, error) &&
            ExportMotionToAfter(composition, mcp_json::Value(), measurer, {},
                                options, description, report, error);
        Check(ok, "the composition is described: " + error.message);
        return ok;
    };
    const std::string glitch =
        R"J("animate": {"filter": {"interpolate": {"frames": [0, 20],
             "values": ["slice(0px 12px 3 0)", "slice(10px 12px 3 1)"]}}})J";
    const std::string cascade =
        R"J("units": "char", "stagger": {"each": 2},
            "animate": {"opacity": {"interpolate": {"frames": [0, 6], "values": [0, 1]}}})J";

    mcp_json::Value description;
    MotionAfterExportReport report;
    if (describe(glitch, cascade, description, report)) {
        const mcp_json::Value* film = Comp(description, "Film");
        const mcp_json::Value* text =
            film != nullptr ? Layer(*film, "GLITCH") : nullptr;
        Check(report.comps == 1 && text != nullptr &&
                  text->Find("kind")->AsString() == "text",
              "the box folds into the text: one composition, no "
              "precomposition around one layer");
        if (text != nullptr) {
            const mcp_json::Value* effects = text->Find("effects");
            Check(effects != nullptr && effects->AsArray().size() == 1 &&
                      effects->AsArray()[0].Find("kind")->AsString() ==
                          "wave_warp" &&
                      effects->AsArray()[0].Find("height_keys") != nullptr,
                  "the text layer carries the box's glitch, animated");
            const mcp_json::Value* units = text->Find("text")->Find("units");
            Check(units != nullptr &&
                      units->Find("based_on")->AsString() ==
                          "characters_excluding_spaces" &&
                      units->Find("animators")->AsArray().size() == 1,
                  "and still its cascade, by characters");
        }
    }

    mcp_json::Value plain;
    MotionAfterExportReport plainReport;
    if (describe(glitch, R"J("style": {"color": "#ffffff"})J", plain,
                 plainReport)) {
        Check(plainReport.comps == 1,
              "a still text under a filter folds the same way");
    }

    mcp_json::Value faded;
    MotionAfterExportReport fadedReport;
    if (describe(
            glitch.substr(0, glitch.size() - 1) +
                R"J(, "opacity": {"interpolate": {"frames": [0, 5], "values": [0, 1]}}})J",
            cascade, faded, fadedReport)) {
        const mcp_json::Value* film = Comp(faded, "Film");
        const mcp_json::Value* group =
            film != nullptr ? Layer(*film, "glitch") : nullptr;
        Check(fadedReport.comps == 2 && group != nullptr &&
                  group->Find("kind")->AsString() == "precomp" &&
                  group->Find("effects") != nullptr,
              "a box that also fades stays a precomposition, carrying the "
              "glitch");
    }

    mcp_json::Value doubled;
    MotionAfterExportReport doubledReport;
    if (describe(glitch, R"J("style": {"filter": "slice(4px 8px 2 0)"})J",
                 doubled, doubledReport)) {
        Check(doubledReport.comps == 2,
              "a text with a filter of its own keeps the box's apart");
    }
}

// MA4 -- a texture that jumps every two frames: hold keyframes by default,
// exact; named in `boil`, an expression that draws its jumps in the same
// range, which the report says are After's. A name that boils nothing is
// refused rather than ignored.
void TestBoil() {
    const auto describe =
        [](const std::string& translateX, const std::string& translateY,
           const std::vector<std::string>& boil, mcp_json::Value& description,
           MotionAfterExportReport& report, MotionError& error) {
            const std::string json =
                R"J({"width": 400, "height": 200, "fps": 25, "duration": 50,
              "root": {"type": "fill", "children": [
                {"type": "box", "id": "grain",
                 "style": {"position": "absolute", "left": 0, "top": 0, "width": 100,
                           "height": 100, "backgroundColor": "#FF0000"},
                 "animate": {"translateX": )J" +
                translateX + R"J(, "translateY": )J" + translateY +
                R"J(}}]}})J";
            MotionComposition composition;
            FakeMeasurer measurer;
            MotionAfterExportOptions options;
            options.name = "Film";
            options.boil = boil;
            return ParseMotionComposition(json, composition, error) &&
                   ExportMotionToAfter(composition, mcp_json::Value(), measurer,
                                       {}, options, description, report, error);
        };
    const std::string steppedX =
        R"J({"interpolate": {"frames": [0, 2, 4, 6, 8, 10, 12, 14],
             "values": [-5, 3, 7, -2, 0, 4, -1, 6], "easing": "step-end"}})J";
    const std::string steppedY =
        R"J({"interpolate": {"frames": [0, 2, 4, 6, 8, 10, 12, 14],
             "values": [1, -3, 2, 5, -4, 0, 3, -2], "easing": "step-end"}})J";
    const std::string curved =
        R"J({"interpolate": {"frames": [0, 14], "values": [-5, 6],
             "easing": "cubic-bezier(0.16, 1, 0.3, 1)"}})J";
    const std::string uneven =
        R"J({"interpolate": {"frames": [0, 2, 5, 6], "values": [-5, 3, 7, -2],
             "easing": "step-end"}})J";
    const auto grain = [](const mcp_json::Value& description) {
        const mcp_json::Value* film = Comp(description, "Film");
        return film != nullptr ? Layer(*film, "grain") : nullptr;
    };
    const auto boils = [](const mcp_json::Value* layer,
                          const std::string& dimension) {
        const mcp_json::Value* expressions =
            layer->Find("transform")->Find("expressions");
        return expressions != nullptr &&
               expressions->Find(dimension) != nullptr;
    };
    const auto keyed = [](const mcp_json::Value* layer,
                          const std::string& dimension) {
        return layer->Find("transform")->Find("keys")->Find(dimension) !=
               nullptr;
    };

    mcp_json::Value held;
    MotionAfterExportReport heldReport;
    MotionError error;
    if (!describe(steppedX, steppedY, {}, held, heldReport, error)) {
        Check(false, "the texture is described: " + error.message);
        return;
    }
    const mcp_json::Value* exact = grain(held);
    Check(exact != nullptr && keyed(exact, "position.x") &&
              !boils(exact, "position.x") && heldReport.keyframes == 16,
          "by default a texture's jumps are hold keyframes, exact");

    mcp_json::Value boiled;
    MotionAfterExportReport boiledReport;
    if (!describe(steppedX, steppedY, {"grain"}, boiled, boiledReport, error)) {
        Check(false, "the texture boils: " + error.message);
        return;
    }
    const mcp_json::Value* texture = grain(boiled);
    if (texture == nullptr) {
        Check(false, "the texture is one layer");
        return;
    }
    const mcp_json::Value& transform = *texture->Find("transform");
    const std::string x =
        boils(texture, "position.x")
            ? transform.Find("expressions")->Find("position.x")->AsString()
            : "";
    const std::string y =
        boils(texture, "position.y")
            ? transform.Find("expressions")->Find("position.y")->AsString()
            : "";
    Check(x.find("posterizeTime(12.5);") != std::string::npos &&
              x.find("value + random(-5, 7);") != std::string::npos &&
              y.find("value + random(-4, 5);") != std::string::npos &&
              x.find("seedRandom(1, false);") != std::string::npos &&
              y.find("seedRandom(2, false);") != std::string::npos,
          "named, it jumps 12.5 times a second within the track's range, "
          "each dimension on its own seed: " +
              x);
    Check(!keyed(texture, "position.x") && !keyed(texture, "position.y") &&
              boiledReport.keyframes == 0,
          "and carries none of its hold keyframes");
    Check(Near(transform.Find("position")->AsArray()[0], 50.0) &&
              Near(transform.Find("position")->AsArray()[1], 50.0),
          "the expression adds the jump to the layer's own place");
    size_t said = 0;
    for (const std::string& note : boiledReport.notes) {
        said += note.find("les sauts sont ceux d'After") != std::string::npos
                    ? 1
                    : 0;
    }
    Check(said == 2, "the report names both tracks as After's jumps");

    // Only what is held at a constant step boils; the rest stays exact.
    for (const std::string& other : {curved, uneven}) {
        mcp_json::Value partly;
        MotionAfterExportReport partlyReport;
        const mcp_json::Value* layer =
            describe(other, steppedY, {"grain"}, partly, partlyReport, error)
                ? grain(partly)
                : nullptr;
        Check(layer != nullptr && keyed(layer, "position.x") &&
                  !boils(layer, "position.x") && boils(layer, "position.y"),
              "a track curved or held at an uneven step keeps its keyframes");
    }

    mcp_json::Value refused;
    MotionAfterExportReport refusedReport;
    MotionError unknown;
    Check(!describe(steppedX, steppedY, {"absent"}, refused, refusedReport,
                    unknown) &&
              unknown.code == "ValidationFailed",
          "a name that is not a node is refused");
    MotionError still;
    Check(!describe(curved, uneven, {"grain"}, refused, refusedReport, still) &&
              still.code == "ValidationFailed",
          "a node named that has nothing held at a constant step is refused, "
          "not ignored");
}

// The sound of a timeline as it plays: a muted track is silent, a soloed
// one silences the others, relative files resolve against the project.
void TestTimelineAudio() {
    Document document;
    DocumentSource music;
    music.path = "sons/musique.wav";
    document.sources.push_back(music);
    const auto track = [&](bool muted, bool solo) {
        DocumentTrack audio;
        audio.kind = "audio";
        audio.muted = muted;
        audio.solo = solo;
        DocumentClip clip;
        clip.source_id = music.id;
        clip.source_in = {50, 25};
        clip.timeline_in = {25, 25};
        clip.duration = {100, 25};
        clip.audio_gain_db = {-6, 1};
        clip.audio_fade_out = {1, 2};
        audio.clips.push_back(clip);
        return audio;
    };
    document.sequence.tracks.push_back(track(false, false));
    document.sequence.tracks.push_back(track(true, false));
    std::vector<MotionAfterAudio> audio;
    MotionError error;
    Check(MotionAfterTimelineAudio(document, "/projets", audio, error) &&
              audio.size() == 1 &&
              audio[0].path == "/projets/sons/musique.wav" &&
              audio[0].source_in == 2.0 && audio[0].timeline_in == 1.0 &&
              audio[0].duration == 4.0 && audio[0].gain_db == -6.0 &&
              audio[0].fade_out == 0.5,
          "the unmuted track's sound, in seconds, its file resolved");
    document.sequence.tracks.push_back(track(false, true));
    document.sequence.tracks.back().clips[0].timeline_in = {0, 25};
    audio.clear();
    Check(MotionAfterTimelineAudio(document, "/projets", audio, error) &&
              audio.size() == 1 && audio[0].timeline_in == 0.0,
          "a soloed track silences the others");
    document.sequence.tracks.back().clips[0].speed = {2, 1};
    audio.clear();
    Check(!MotionAfterTimelineAudio(document, "/projets", audio, error) &&
              error.code == "Unsupported",
          "a sound at another speed is refused, not played at 1:1");
}

// Stands in for the executable's Core Text describer.
bool FakeDescribe(const MotionAfterRequest& request,
                  mcp_json::Value& description, MotionAfterExportReport& report,
                  MotionError& error) {
    FakeMeasurer measurer;
    return ExportMotionToAfter(*request.composition, request.props, measurer,
                               request.reach.Context(), request.options,
                               description, report, error);
}

// One composition; the bridge's requests recorded, with the description
// as After would have read it.
class ExportBackend final : public McpBackend {
public:
    ExportBackend() {
        DocumentComposition composition;
        composition.name = "Carton";
        std::string parseError;
        mcp_json::Value::Parse(
            R"J({"width": 200, "height": 100, "fps": 25, "duration": 10,
                 "root": {"type": "fill", "style": {"fontFamily": "Fake"},
                   "children": [{"type": "text", "content": "BONJOUR"}]}})J",
            composition.definition, parseError);
        document_.compositions.push_back(composition);
        composition_id = composition.id;
    }

    bool SnapshotDocument(Document& document, std::string&) override {
        document = document_;
        return true;
    }

    bool ApplyOperation(Operation, std::string&, std::string& errorName,
                        std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "not used here";
        return false;
    }

    bool RunAfterRequest(const std::string& requestJson,
                         std::string& resultJson, std::string& errorName,
                         std::string& message) override {
        ++calls;
        if (!ParseAfterBridgeRequest(requestJson, request, message)) {
            errorName = "ValidationFailed";
            return false;
        }
        std::ifstream input(
            request.arguments.Find("description_path")->AsString());
        std::ostringstream bytes;
        bytes << input.rdbuf();
        description = bytes.str();
        AfterBridgeResponse response;
        response.ok = true;
        response.project_name = "Carton";
        response.path = request.arguments.Find("project_path")->AsString();
        response.result = mcp_json::Value::MakeObject();
        response.result.Set("layers", mcp_json::Value::MakeInt(1));
        resultJson = SerializeAfterBridgeResponse(response);
        return true;
    }

    bool Undo(std::string&, std::string& errorName,
              std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "not used here";
        return false;
    }

    bool Redo(std::string&, std::string& errorName,
              std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "not used here";
        return false;
    }

    bool Describe(std::string& json, std::string&) override {
        json = "{}";
        return true;
    }

    std::string composition_id;
    int calls = 0;
    AfterBridgeRequest request;
    std::string description;

private:
    Document document_;
};

void TestExportTool() {
    const McpToolRegistry registry;
    ExportBackend backend;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path();
    const std::string aep =
        (directory / (GenerateUlid() + "-carton.aep")).string();
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("composition_id",
                  mcp_json::Value::MakeString(backend.composition_id));
    arguments.Set("project_path", mcp_json::Value::MakeString(aep));

    SetMotionAfterDescriber(nullptr);
    McpToolCallOutcome outcome =
        registry.Call(backend, "export_motion_after", arguments);
    Check(!outcome.ok && outcome.error_name == "UnsupportedOperation" &&
              backend.calls == 0,
          "without Core Text the export is refused, After untouched");

    SetMotionAfterDescriber(&FakeDescribe);
    outcome = registry.Call(backend, "export_motion_after", arguments);
    mcp_json::Value result;
    std::string parseError;
    mcp_json::Value::Parse(outcome.result_json, result, parseError);
    const mcp_json::Value* path =
        backend.calls == 1 ? backend.request.arguments.Find("description_path")
                           : nullptr;
    Check(
        outcome.ok && backend.calls == 1 &&
            backend.request.method == AfterBridgeMethod::BuildMotionProject &&
            backend.request.arguments.Find("project_path")->AsString() == aep &&
            backend.description.find("\"after-motion.v1\"") !=
                std::string::npos &&
            backend.description.find("\"Carton\"") != std::string::npos &&
            path != nullptr && !std::filesystem::exists(path->AsString()),
        "the description is written, built by the bridge, then removed: " +
            outcome.message);
    Check(
        result.Find("comps") != nullptr &&
            result.Find("comps")->Dump() == "1" &&
            result.Find("after") != nullptr &&
            result.Find("description_path") == nullptr,
        "the answer gives the counts and After's own: " + outcome.result_json);

    // Without build, the description is the output, and it stays.
    const std::string kept =
        (directory / (GenerateUlid() + "-carton.json")).string();
    arguments.Set("build", mcp_json::Value::MakeBool(false));
    outcome = registry.Call(backend, "export_motion_after", arguments);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed",
          "without build, a description path is required");
    arguments.Set("description_path", mcp_json::Value::MakeString(kept));
    outcome = registry.Call(backend, "export_motion_after", arguments);
    Check(outcome.ok && backend.calls == 1 && std::filesystem::exists(kept),
          "without build, only the description is written: " + outcome.message);
    outcome = registry.Call(backend, "export_motion_after", arguments);
    Check(!outcome.ok && outcome.error_name == "IoError",
          "a description is never written over a file");

    {
        std::ofstream existing(aep, std::ios::binary);
        existing << "a project";
    }
    arguments.Set("build", mcp_json::Value::MakeBool(true));
    arguments.Set("description_path",
                  mcp_json::Value::MakeString(
                      (directory / (GenerateUlid() + "-other.json")).string()));
    outcome = registry.Call(backend, "export_motion_after", arguments);
    Check(!outcome.ok && outcome.error_name == "ProjectExists" &&
              backend.calls == 1,
          "an existing project is refused before anything is written");
    arguments.Set("project_path", mcp_json::Value::MakeString("carton.aep"));
    outcome = registry.Call(backend, "export_motion_after", arguments);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed",
          "a relative project path is refused");
    std::error_code ignored;
    std::filesystem::remove(aep, ignored);
    std::filesystem::remove(kept, ignored);
    SetMotionAfterDescriber(nullptr);
}

// MA11 -- the fake's text, and one sound, "son.wav": four seconds.
class SoundMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override {
        return text_.LayoutText(text, style, max_width, layout, error);
    }
    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override {
        return text_.ImageSize(source, width, height, error);
    }
    bool FontPostScriptName(const MotionTextStyle& style, std::string& name,
                            MotionError& error) override {
        return text_.FontPostScriptName(style, name, error);
    }
    bool MediaInfo(const std::string& src, const std::string& /*media*/,
                   MotionMediaInfo& info, MotionError& error) override {
        info = MotionMediaInfo{};
        if (src == "plan.mov") {
            // Two seconds of 1920 x 1080 pictures, with a sound.
            info.has_video = true;
            info.has_audio = true;
            info.width = 1920;
            info.height = 1080;
            info.rate_numerator = 25;
            info.duration = RationalTime{50, 25};
            return true;
        }
        if (src != "son.wav") {
            error = {"MediaMissing", "", "no media '" + src + "'"};
            return false;
        }
        info.has_audio = true;
        info.duration = RationalTime{4 * 48000, 48000};
        return true;
    }

private:
    FakeMeasurer text_;
};

bool Describe(const std::string& json, MotionMeasurer& measurer,
              mcp_json::Value& description, MotionAfterExportReport& report,
              const std::string& label) {
    MotionComposition composition;
    MotionError error;
    MotionAfterExportOptions options;
    options.name = "Film";
    if (!ParseMotionComposition(json, composition, error) ||
        !ExportMotionToAfter(composition, mcp_json::Value(), measurer, {},
                             options, description, report, error)) {
        Check(false, label + ": " + error.path + " " + error.message);
        return false;
    }
    return true;
}

const mcp_json::Value* MainComp(const mcp_json::Value& description) {
    for (const mcp_json::Value& comp : description.Find("comps")->AsArray()) {
        if (comp.Find("id")->AsString() ==
            description.Find("main")->AsString()) {
            return &comp;
        }
    }
    return nullptr;
}

bool Said(const MotionAfterExportReport& report, const std::string& part) {
    for (const std::string& note : report.notes) {
        if (note.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int64_t Frame(const mcp_json::Value& key) {
    int64_t frame = -1;
    key.Find("f")->AsInt64(frame);
    return frame;
}

double First(const mcp_json::Value& key, size_t index = 0) {
    return std::stod(key.Find("v")->AsArray()[index].Dump());
}

// MA12 -- the shutter goes with the composition, opening on the frame; a
// growth from 0 starts a thousandth of a percent above it, which After does
// not leave out of the blur.
void TestMotionBlur() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "motionBlur": {"samples": 6, "shutterAngle": 180},
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "carre",
         "style": {"position": "absolute", "left": 10, "top": 10, "width": 50,
                   "height": 50, "backgroundColor": "#ff0000"},
         "animate": {"scale": {"interpolate": {"frames": [0, 10], "values": [0, 1]}}}}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a blurred square")) {
        return;
    }
    const mcp_json::Value* blur = description.Find("motion_blur");
    Check(blur != nullptr && Near(*blur->Find("samples"), 6.0) &&
              Near(*blur->Find("shutter_angle"), 180.0) &&
              Near(*blur->Find("shutter_phase"), 0.0),
          "the description carries the engine's shutter, phase 0");
    Check(!Said(report, "flou de mouvement non traduit"),
          "the motion blur is no longer reported as left out");
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* square =
        main != nullptr ? Layer(*main, "carre") : nullptr;
    const mcp_json::Value* scale =
        square != nullptr
            ? square->Find("transform")->Find("keys")->Find("scale")
            : nullptr;
    Check(scale != nullptr &&
              Near(scale->AsArray()[0].Find("v")->AsArray()[0], 0.001),
          "a scale key at 0 is written 0.001 % under a shutter");

    const std::string still = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "carre",
         "style": {"position": "absolute", "left": 10, "top": 10, "width": 50,
                   "height": 50, "backgroundColor": "#ff0000"},
         "animate": {"scale": {"interpolate": {"frames": [0, 10], "values": [0, 1]}}}}]}})J";
    if (!Describe(still, measurer, description, report, "a square")) {
        return;
    }
    main = MainComp(description);
    square = main != nullptr ? Layer(*main, "carre") : nullptr;
    scale = square != nullptr
                ? square->Find("transform")->Find("keys")->Find("scale")
                : nullptr;
    Check(description.Find("motion_blur") == nullptr && scale != nullptr &&
              Near(scale->AsArray()[0].Find("v")->AsArray()[0], 0.0),
          "without a shutter, nothing changes");
}

// MA11 -- a clip-path becomes a mask of the layer, keyed where it
// animates: the underline that writes itself.
void TestClipMask() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "barre",
         "style": {"position": "absolute", "left": 20, "top": 30, "width": 100,
                   "height": 10, "backgroundColor": "#7aded1"},
         "animate": {"clipPath": {"interpolate": {"frames": [5, 12],
            "values": ["inset(0% 100% 0% 0%)", "inset(0% 0% 0% 0%)"],
            "easing": "cubic-bezier(0.16, 1, 0.3, 1)"}}}},
        {"type": "box", "id": "moitie",
         "style": {"position": "absolute", "left": 20, "top": 60, "width": 100,
                   "height": 10, "backgroundColor": "#000000",
                   "clipPath": "inset(0% 50% 0% 0%)"}},
        {"type": "box", "id": "glisse",
         "style": {"position": "absolute", "left": 0, "top": 90, "width": 100,
                   "height": 10, "backgroundColor": "#000000"},
         "animate": {
           "left": {"interpolate": {"frames": [5, 12], "values": [0, 200]}},
           "clipPath": {"interpolate": {"frames": [5, 12],
              "values": ["inset(0% 100% 0% 0%)", "inset(0% 0% 0% 0%)"]}}}},
        {"type": "text", "id": "coupe", "content": "Texte",
         "style": {"position": "absolute", "left": 20, "top": 120,
                   "fontFamily": "Fake", "clipPath": "inset(0% 50% 0% 0%)"}}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "two clipped bars")) {
        return;
    }
    Check(!Said(report, "clipPath"), "the clip-paths are translated");
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* bar =
        main != nullptr ? Layer(*main, "barre") : nullptr;
    const mcp_json::Value* masks =
        bar != nullptr ? bar->Find("masks") : nullptr;
    const mcp_json::Value* keys =
        masks != nullptr && masks->AsArray().size() == 1
            ? masks->AsArray()[0].Find("keys")
            : nullptr;
    if (keys == nullptr || keys->AsArray().size() != 2) {
        Check(false, "the writing bar has one keyed mask: " +
                         (bar != nullptr ? bar->Dump() : std::string()));
        return;
    }
    const mcp_json::Value& from = keys->AsArray()[0];
    const mcp_json::Value& to = keys->AsArray()[1];
    const auto vertex = [](const mcp_json::Value& key, size_t index,
                           size_t axis) {
        return std::stod(key.Find("v")
                             ->Find("vertices")
                             ->AsArray()[index]
                             .AsArray()[axis]
                             .Dump());
    };
    Check(Frame(from) == 5 && Frame(to) == 12 &&
              std::abs(vertex(from, 1, 0)) < 1e-6 &&
              std::abs(vertex(to, 1, 0) - 100.0) < 1e-6 &&
              std::abs(vertex(to, 2, 1) - 10.0) < 1e-6,
          "the mask goes from no width to the whole bar, in the layer's "
          "space: " +
              keys->Dump());
    // A mask's speed is a progress through the segment, average 1: the
    // CSS curve's first slope, y1 / x1, at x1.
    const mcp_json::Value* out = from.Find("ease_out");
    Check(out != nullptr && Near(out->AsArray()[0].AsArray()[0], 6.25) &&
              Near(out->AsArray()[0].AsArray()[1], 16.0),
          "the mask eases as After reads a mask's speed");
    const mcp_json::Value* half = Layer(*main, "moitie");
    const mcp_json::Value* still =
        half != nullptr ? half->Find("masks") : nullptr;
    Check(still != nullptr && still->AsArray()[0].Find("keys") == nullptr &&
              std::abs(std::stod(still->AsArray()[0]
                                     .Find("shape")
                                     ->Find("vertices")
                                     ->AsArray()[1]
                                     .AsArray()[0]
                                     .Dump()) -
                       50.0) < 1e-6,
          "a still clip-path is a still mask");
    // A bar sliding by its own left while its clip writes: the mask stays
    // on the bar, in the layer's space, which goes with it.
    const mcp_json::Value* sliding = Layer(*main, "glisse");
    const mcp_json::Value* slidingKeys =
        sliding != nullptr && sliding->Find("masks") != nullptr
            ? sliding->Find("masks")->AsArray()[0].Find("keys")
            : nullptr;
    Check(
        slidingKeys != nullptr && slidingKeys->AsArray().size() == 2 &&
            std::abs(vertex(slidingKeys->AsArray()[1], 1, 0) - 100.0) < 1e-3 &&
            sliding->Find("transform")->Find("keys")->Find("position.x") !=
                nullptr,
        "a sliding bar's mask stays on the bar: " +
            (sliding != nullptr ? sliding->Dump() : std::string()));
    // A text's layer starts on its baseline: its mask is counted from
    // there (the fake's baseline is 0.8 of 16 px).
    const mcp_json::Value* cut = Layer(*main, "Texte");
    const mcp_json::Value* cutMask =
        cut != nullptr && cut->Find("masks") != nullptr
            ? cut->Find("masks")->AsArray()[0].Find("shape")
            : nullptr;
    Check(cutMask != nullptr && std::abs(std::stod(cutMask->Find("vertices")
                                                       ->AsArray()[0]
                                                       .AsArray()[1]
                                                       .Dump()) +
                                         12.8) < 1e-3,
          "a text's mask is counted from its baseline: " +
              (cut != nullptr ? cut->Dump() : std::string()));
}

// MA11 -- a background colour and a text colour animated: colour keys,
// the text's inherited from the box around it, as CSS inherits it; a text
// that sets its own colour keeps it.
void TestColours() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "box", "id": "pastille",
         "style": {"position": "absolute", "left": 0, "top": 0, "width": 80,
                   "height": 40, "backgroundColor": "#000000"},
         "animate": {
           "backgroundColor": {"interpolate": {"frames": [0, 10],
              "values": ["#000000", "#ffffff"],
              "easing": "cubic-bezier(0.16, 1, 0.3, 1)"}},
           "color": {"interpolate": {"frames": [0, 10],
              "values": ["#ff0000", "#00ff00"]}}},
         "children": [{"type": "text", "content": "A"}]},
        {"type": "box", "id": "autre",
         "style": {"position": "absolute", "left": 0, "top": 100, "width": 80,
                   "height": 40},
         "animate": {"color": {"interpolate": {"frames": [0, 10],
              "values": ["#ff0000", "#00ff00"]}}},
         "children": [{"type": "text", "content": "B",
                       "style": {"color": "#0000ff"}}]}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "coloured boxes")) {
        return;
    }
    Check(!Said(report, "color"),
          "the colours are translated: " +
              (report.notes.empty() ? std::string() : report.notes.front()));
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* fill =
        main != nullptr ? Layer(*main, "pastille") : nullptr;
    // The group is a null; its background is the rectangle named after it.
    const mcp_json::Value* rect = nullptr;
    const mcp_json::Value* textA = nullptr;
    const mcp_json::Value* textB = nullptr;
    for (const mcp_json::Value& layer : main->Find("layers")->AsArray()) {
        if (layer.Find("kind")->AsString() == "rect") {
            rect = &layer;
        }
        if (layer.Find("name")->AsString() == "A") {
            textA = &layer;
        }
        if (layer.Find("name")->AsString() == "B") {
            textB = &layer;
        }
    }
    const mcp_json::Value* colours =
        rect != nullptr ? rect->Find("rect")->Find("color_keys") : nullptr;
    if (fill == nullptr || colours == nullptr ||
        colours->AsArray().size() != 2) {
        Check(false, "the background has two colour keys");
        return;
    }
    Check(Near(colours->AsArray()[1].Find("v")->AsArray()[0], 1.0) &&
              Near(colours->AsArray()[1].Find("v")->AsArray()[3], 1.0),
          "a colour key is [r, g, b, 1]");
    // One ease for the colour, in distance between the colours (0 to 255)
    // per second: black to white is 255 * sqrt(3) in 0.4 s.
    const double average = 255.0 * std::sqrt(3.0) / 0.4;
    const mcp_json::Value* ease = colours->AsArray()[0].Find("ease_out");
    Check(ease != nullptr && ease->AsArray().size() == 1 &&
              std::abs(std::stod(ease->AsArray()[0].AsArray()[0].Dump()) -
                       average * 6.25) < 0.01,
          "a colour eases as After reads a colour's speed");
    const mcp_json::Value* inherited =
        textA != nullptr ? textA->Find("text")->Find("color_keys") : nullptr;
    Check(inherited != nullptr && inherited->AsArray().size() == 2 &&
              Near(inherited->AsArray()[0].Find("v")->AsArray()[0], 1.0) &&
              Near(inherited->AsArray()[1].Find("v")->AsArray()[1], 1.0),
          "a text inherits its box's animated colour");
    Check(
        textB != nullptr && textB->Find("text")->Find("color_keys") == nullptr,
        "a text with a colour of its own keeps it");
}

// MA11 -- what the layout moves: a panel whose place and size animate
// together becomes a rectangle centred on its layer, its size and place
// keyed along the same curve; a word its neighbour pushes follows.
void TestLayoutKeys() {
    const std::string json = R"J({
      "width": 400, "height": 300, "fps": 25, "duration": 30,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "box", "id": "panneau",
         "style": {"position": "absolute", "left": 0, "top": 0, "width": 100,
                   "height": 50, "backgroundColor": "#7aded1"},
         "animate": {
           "left": {"interpolate": {"frames": [10, 20], "values": [0, 200],
              "easing": "cubic-bezier(0.65, 0, 0.35, 1)"}},
           "top": {"interpolate": {"frames": [10, 20], "values": [0, 100],
              "easing": "cubic-bezier(0.65, 0, 0.35, 1)"}},
           "width": {"interpolate": {"frames": [10, 20], "values": [100, 50],
              "easing": "cubic-bezier(0.65, 0, 0.35, 1)"}},
           "height": {"interpolate": {"frames": [10, 20], "values": [50, 20],
              "easing": "cubic-bezier(0.65, 0, 0.35, 1)"}}}},
        {"type": "box", "style": {"position": "absolute", "left": 0,
                                  "top": 200, "flexDirection": "row"},
         "children": [
           {"type": "box", "id": "pousse",
            "style": {"width": 20, "height": 10, "backgroundColor": "#000000"},
            "animate": {"width": {"interpolate": {"frames": [0, 10],
               "values": [20, 120]}}}},
           {"type": "text", "content": "mot"}]}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a panel and a row")) {
        return;
    }
    Check(!Said(report, "non traduit") && !Said(report, "une clé par image"),
          "the layout's moves are translated with the engine's curves: " +
              (report.notes.empty() ? std::string() : report.notes.front()));
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* panel =
        main != nullptr ? Layer(*main, "panneau") : nullptr;
    if (panel == nullptr) {
        Check(false, "the panel is a layer");
        return;
    }
    const mcp_json::Value* rect = panel->Find("rect");
    const mcp_json::Value* size = rect->Find("size_keys");
    const mcp_json::Value* keys = panel->Find("transform")->Find("keys");
    const mcp_json::Value* x = keys->Find("position.x");
    const mcp_json::Value* y = keys->Find("position.y");
    Check(rect->Find("centered") != nullptr && size != nullptr &&
              size->AsArray().size() == 2 && x != nullptr &&
              x->AsArray().size() == 2 && y != nullptr &&
              y->AsArray().size() == 2,
          "size and place keyed at the two ends of the move: " + panel->Dump());
    if (size == nullptr || x == nullptr || y == nullptr ||
        size->AsArray().size() != 2 || x->AsArray().size() != 2 ||
        y->AsArray().size() != 2) {
        return;
    }
    Check(Frame(size->AsArray()[0]) == 10 && Frame(size->AsArray()[1]) == 20 &&
              std::abs(First(size->AsArray()[1], 0) - 50.0) < 1e-3 &&
              std::abs(First(size->AsArray()[1], 1) - 20.0) < 1e-3 &&
              std::abs(First(x->AsArray()[0]) - 50.0) < 1e-3 &&
              std::abs(First(x->AsArray()[1]) - 225.0) < 1e-3 &&
              std::abs(First(y->AsArray()[1]) - 110.0) < 1e-3,
          "the box's centre and size at each end");
    Check(size->AsArray()[0].Find("out")->AsString() == "bezier" &&
              size->AsArray()[0].Find("ease_out")->AsArray().size() == 2,
          "the size eases along the engine's curve, one ease a dimension");
    const mcp_json::Value* word = Layer(*main, "mot");
    const mcp_json::Value* pushed =
        word != nullptr
            ? word->Find("transform")->Find("keys")->Find("position.x")
            : nullptr;
    Check(pushed != nullptr && pushed->AsArray().size() == 2 &&
              std::abs(First(pushed->AsArray()[1]) -
                       First(pushed->AsArray()[0]) - 100.0) < 1e-3 &&
              pushed->AsArray()[0].Find("out")->AsString() == "linear",
          "the word its neighbour pushes follows it, along a straight line");
    const mcp_json::Value* bar = Layer(*main, "pousse");
    Check(bar != nullptr && bar->Find("rect")->Find("centered") == nullptr &&
              bar->Find("transform")->Find("keys")->Find("scale") != nullptr,
          "a bar growing from its left edge keeps After's idiom: an x scale");
}

// MA11 -- two scales at once: each dimension from its own track, the
// segment another track cuts kept exact.
void TestTwoScales() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "carre",
         "style": {"position": "absolute", "left": 0, "top": 0, "width": 50,
                   "height": 50, "backgroundColor": "#000000"},
         "animate": {
           "scale": {"interpolate": {"frames": [0, 10], "values": [1, 2]}},
           "scaleY": {"interpolate": {"frames": [5, 15], "values": [1, 3],
              "easing": "cubic-bezier(0.16, 1, 0.3, 1)"}}}}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a stretched square")) {
        return;
    }
    Check(!Said(report, "échelle"), "both scales are translated");
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* square =
        main != nullptr ? Layer(*main, "carre") : nullptr;
    const mcp_json::Value* scale =
        square != nullptr
            ? square->Find("transform")->Find("keys")->Find("scale")
            : nullptr;
    if (scale == nullptr || scale->AsArray().size() != 4) {
        Check(false, "keys where either track has one: " +
                         (scale != nullptr ? scale->Dump() : std::string()));
        return;
    }
    const auto& keys = scale->AsArray();
    KeyframeEasing easing{0.16, 1.0, 0.3, 1.0};
    const double half = KeyframeTrack::EvaluateCubicBezier(easing, 0.5);
    Check(Frame(keys[0]) == 0 && Frame(keys[1]) == 5 && Frame(keys[2]) == 10 &&
              Frame(keys[3]) == 15 &&
              std::abs(First(keys[1], 0) - 150.0) < 1e-3 &&
              std::abs(First(keys[1], 1) - 100.0) < 1e-3 &&
              std::abs(First(keys[2], 0) - 200.0) < 1e-3 &&
              std::abs(First(keys[2], 1) - 100.0 * (1.0 + 2.0 * half)) < 1e-3 &&
              std::abs(First(keys[3], 1) - 300.0) < 1e-3,
          "x from scale, y from scaleY, as the engine sets them: " +
              scale->Dump());
}

// MA13 -- a precomposition crops to its frame, a CSS box does not: a group
// holding a track longer than the screen keeps all of it, since its move
// brings the end into view. A group that fits keeps its parent's frame.
void TestGroupFrameHoldsOverflow() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "piste",
         "style": {"position": "absolute", "left": 0, "top": 50, "width": 400,
                   "height": 40, "opacity": 0.5},
         "animate": {"translateX": {"interpolate": {"frames": [0, 10],
            "values": [0, -300]}}},
         "children": [
           {"type": "box", "id": "debut",
            "style": {"position": "absolute", "left": -100, "top": 0,
                      "width": 50, "height": 40, "backgroundColor": "#000000"}},
           {"type": "box", "id": "fin",
            "style": {"position": "absolute", "left": 500, "top": 0,
                      "width": 100, "height": 40,
                      "backgroundColor": "#000000"}}]},
        {"type": "box", "id": "dedans",
         "style": {"position": "absolute", "left": 0, "top": 120, "width": 400,
                   "height": 40, "opacity": 0.5},
         "children": [
           {"type": "box", "id": "bloc",
            "style": {"position": "absolute", "left": 300, "top": 0,
                      "width": 100, "height": 40,
                      "backgroundColor": "#000000"}}]}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a long track")) {
        return;
    }
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* track = Comp(description, "piste");
    const mcp_json::Value* layer =
        main != nullptr ? Layer(*main, "piste") : nullptr;
    const mcp_json::Value* end =
        track != nullptr ? Layer(*track, "fin") : nullptr;
    if (track == nullptr || layer == nullptr || end == nullptr) {
        Check(false, "the track is a precomposition holding its end: " +
                         description.Dump());
        return;
    }
    Check(Near(*track->Find("width"), 700.0) &&
              Near(*track->Find("height"), 200.0),
          "the track's frame runs from its start to its end: " + track->Dump());
    Check(Near(end->Find("transform")->Find("position")->AsArray()[0], 650.0),
          "its end sits in that frame, which starts 100 px left of the "
          "screen: " +
              end->Dump());
    Check(Near(layer->Find("transform")->Find("anchor")->AsArray()[0], 300.0),
          "the track still turns and moves about its box's centre: " +
              layer->Dump());
    const mcp_json::Value* inside = Comp(description, "dedans");
    Check(inside != nullptr && Near(*inside->Find("width"), 400.0) &&
              Near(*inside->Find("height"), 200.0),
          "a group that fits keeps its parent's frame: " +
              (inside != nullptr ? inside->Dump() : std::string()));
}

// MA13 -- a block falling by its top, its left and width keyed along (held
// still): the bar and the group land where the engine puts them. The bar
// anchors on its left edge, so left places that edge; the group anchors on
// its centre, and its width scales none of its children.
void TestLeftAndWidthKeyedTogether() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "barre",
         "style": {"position": "absolute", "left": 0, "top": -100,
                   "width": 400, "height": 100, "backgroundColor": "#000000"},
         "animate": {
           "left": {"interpolate": {"frames": [5, 10], "values": [0, 0]}},
           "top": {"interpolate": {"frames": [5, 10], "values": [-100, 0]}},
           "width": {"interpolate": {"frames": [5, 10], "values": [400, 400]}}}},
        {"type": "box", "id": "bloc",
         "style": {"position": "absolute", "left": 0, "top": -100,
                   "width": 400, "height": 100, "opacity": 0.5},
         "animate": {
           "left": {"interpolate": {"frames": [5, 10], "values": [0, 0]}},
           "top": {"interpolate": {"frames": [5, 10], "values": [-100, 0]}},
           "width": {"interpolate": {"frames": [5, 10], "values": [400, 400]}}},
         "children": [
           {"type": "box", "id": "dedans",
            "style": {"position": "absolute", "left": 0, "top": 0,
                      "width": 100, "height": 100,
                      "backgroundColor": "#000000"}}]}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a falling block")) {
        return;
    }
    const mcp_json::Value* main = MainComp(description);
    for (const char* name : {"barre", "bloc"}) {
        const mcp_json::Value* layer =
            main != nullptr ? Layer(*main, name) : nullptr;
        const mcp_json::Value* x =
            layer != nullptr
                ? layer->Find("transform")->Find("keys")->Find("position.x")
                : nullptr;
        if (x == nullptr || x->AsArray().empty()) {
            Check(false, std::string(name) + " has position.x keys: " +
                             (layer != nullptr ? layer->Dump() : ""));
            continue;
        }
        const double anchor = std::stod(
            layer->Find("transform")->Find("anchor")->AsArray()[0].Dump());
        // The layer's space starts at the composition's left edge, as the
        // box does: wherever the anchor is, the key puts it back there.
        Check(std::abs(First(x->AsArray().back()) - anchor) < 1e-3,
              std::string(name) + " lands on its box, not half a width " +
                  "right: " + layer->Dump());
        const mcp_json::Value* scale =
            layer->Find("transform")->Find("keys")->Find("scale");
        Check(std::string(name) == "barre" || scale == nullptr,
              "a group's width scales none of its children: " + layer->Dump());
    }
    Check(Said(report, "width animé"),
          "the group's width, which After does not follow, is said");
}

// MA11 -- a sequence that only plays a sound is no scene: an audio layer
// of the film, its volume on a slider.
void TestSounds() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 100,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "sequence", "from": 20, "duration": 50, "children": [
          {"type": "audio", "src": "son.wav", "startFrom": 10,
           "animate": {"volume": {"interpolate": {"frames": [0, 10],
              "values": [0, 1]}}}}]},
        {"type": "sequence", "from": 0, "duration": 100, "id": "TITRE",
         "children": [{"type": "text", "content": "T"}]}]}})J";
    SoundMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a film with a sound")) {
        return;
    }
    Check(!Said(report, "non traduit"), "the sound is translated");
    Check(description.Find("comps")->AsArray().size() == 2,
          "the sound's sequence makes no composition");
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* sound =
        main != nullptr ? Layer(*main, "son.wav") : nullptr;
    if (sound == nullptr) {
        Check(false, "the sound is a layer of the film");
        return;
    }
    const mcp_json::Value* volume = sound->Find("volume_keys");
    Check(sound->Find("kind")->AsString() == "audio" &&
              Near(*sound->Find("start_s"), 0.4) &&
              Near(*sound->Find("in_s"), 0.8) &&
              Near(*sound->Find("out_s"), 2.8) && volume != nullptr &&
              Frame(volume->AsArray()[0]) == 20 &&
              Frame(volume->AsArray()[1]) == 30,
          "media frame 10 plays at film frame 20, until the sequence ends, "
          "its volume keyed in the film's time: " +
              sound->Dump());
}

// MA11 -- a video: a footage layer placed like an image, cut where the
// engine starts and ends it, its sound off when the node is muted.
void TestVideo() {
    const std::string json = R"J({
      "width": 960, "height": 540, "fps": 25, "duration": 100,
      "root": {"type": "fill", "children": [
        {"type": "video", "id": "plan", "src": "plan.mov", "startFrom": 5,
         "muted": true,
         "style": {"position": "absolute", "left": 0, "top": 0,
                   "width": 960, "height": 540}}]}})J";
    SoundMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "a film with a shot")) {
        return;
    }
    const mcp_json::Value* main = MainComp(description);
    const mcp_json::Value* shot =
        main != nullptr ? Layer(*main, "plan") : nullptr;
    if (shot == nullptr) {
        Check(false, "the video is a layer: " + description.Dump());
        return;
    }
    const mcp_json::Value* scale = shot->Find("transform")->Find("scale");
    Check(shot->Find("kind")->AsString() == "video" &&
              Near(*shot->Find("start"), -5.0) &&
              Near(*shot->Find("in"), 0.0) && Near(*shot->Find("out"), 45.0) &&
              shot->Find("audio")->AsBool() == false &&
              Near(scale->AsArray()[0], 50.0) && !Said(report, "non traduit"),
          "media frame 5 at film frame 0, until the media ends, fitted to its "
          "box, silent: " +
              shot->Dump());
}

// MM2 -- After applies no OpenType feature: a text whose figures the
// variant changes is said in the report, a text without figures is not.
void TestNumericVariantNote() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 10,
      "root": {"type": "fill",
               "style": {"fontFamily": "Fake", "fontVariantNumeric": "lining-nums"},
               "children": [
        {"type": "text", "content": "1 point",
         "style": {"position": "absolute", "left": 0, "top": 0}},
        {"type": "text", "content": "point",
         "style": {"position": "absolute", "left": 0, "top": 100}}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "figures")) {
        return;
    }
    size_t said = 0;
    for (const std::string& note : report.notes) {
        if (note.find("fontVariantNumeric") != std::string::npos) {
            ++said;
            Check(note.find("children[0]") != std::string::npos,
                  "the note names the text with figures: " + note);
        }
    }
    Check(said == 1, "one text with figures, one note");
}

// MB3 -- one continuous band per line, including spaces, with the first
// line filled before the next. Text and bands share opacity and clipping.
void TestHighlights() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"},
        "children": [{"type": "sequence", "from": 5, "duration": 20,
          "children": [{"type": "text", "content": "un deux trois\nquatre cinq",
            "style": {"position": "absolute", "left": 50, "top": 20,
                      "fontSize": 20, "opacity": 0.5,
                      "clipPath": "inset(0px)"},
            "animate": {"rotate": {"interpolate": {
                "frames": [0, 10], "values": [0, 10]}}},
            "highlights": [{"unit": "word", "start": 1, "count": 3,
                "color": "#ffe60080", "top": 0.5, "bottom": 1,
                "bleed": 4, "progress": {"interpolate": {
                    "frames": [0, 10], "values": [0, 1]}}}]}]},
          {"type": "text", "content": "rien",
           "style": {"position": "absolute", "left": 0, "top": 150}}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "highlights")) return;
    const mcp_json::Value* group = nullptr;
    for (const auto& comp : description.Find("comps")->AsArray()) {
        if (comp.Find("name")->AsString().find("Surligneur") == 0) {
            group = &comp;
        }
    }
    Check(group != nullptr, "a highlighter and text have one composition");
    if (group == nullptr) return;
    const auto& layers = group->Find("layers")->AsArray();
    Check(
        layers.size() == 3 && layers.back().Find("kind")->AsString() == "text",
        "two bands, then editable text above them");
    if (layers.size() != 3) return;
    const auto* text = layers.back().Find("text");
    Check(text->Find("content")->AsString() == "un deux trois\rquatre cinq" &&
              text->Find("units") == nullptr &&
              Near(*layers.back().Find("transform")->Find("opacity"), 100.0),
          "the text keeps its glyphs, outside the band's progression");
    const auto valueAt = [](const mcp_json::Value& rect, const char* property,
                            int64_t frame) -> const mcp_json::Value* {
        const auto* keys = rect.Find(property);
        if (keys == nullptr) return nullptr;
        for (const auto& key : keys->AsArray()) {
            int64_t at = -1;
            key.Find("f")->AsInt64(at);
            if (at == frame) return key.Find("v");
        }
        return nullptr;
    };
    const double expected[][2] = {{88.0, 0.0}, {108.0, 32.8}, {108.0, 68.0}};
    const int64_t frames[] = {5, 8, 10};
    for (size_t line = 0; line < 2; ++line) {
        const auto& rect = *layers[line].Find("rect");
        Check(Near(rect.Find("size")->AsArray()[0], 0.0) &&
                  Near(*rect.Find("alpha"), 128.0 / 255.0),
              "the first frame is empty and the band's alpha is kept");
        for (size_t i = 0; i < 3; ++i) {
            const auto* size = valueAt(rect, "size_keys", frames[i]);
            const auto* center = valueAt(rect, "position_keys", frames[i]);
            Check(size != nullptr && center != nullptr &&
                      Near(size->AsArray()[0], expected[i][line]) &&
                      Near(size->AsArray()[1], 12.0) &&
                      Near(center->AsArray()[0], expected[i][line] / 2.0),
                  "line " + std::to_string(line) + " at local frame " +
                      std::to_string(frames[i]) +
                      " fills from its left, in reading order");
        }
    }
    const auto& a = *layers[0].Find("transform")->Find("position");
    const auto& b = *layers[1].Find("transform")->Find("position");
    Check(Near(b.AsArray()[0], std::stod(a.AsArray()[0].Dump()) - 30.0) &&
              Near(b.AsArray()[1], std::stod(a.AsArray()[1].Dump()) + 24.0),
          "the band's spaces, bleed and line offset follow layout");
    bool commonOpacity = false;
    for (const auto& comp : description.Find("comps")->AsArray()) {
        for (const auto& layer : comp.Find("layers")->AsArray()) {
            const auto* child = layer.Find("comp");
            if (child == nullptr ||
                child->AsString() != group->Find("id")->AsString())
                continue;
            commonOpacity =
                Near(*layer.Find("transform")->Find("opacity"), 50.0) &&
                layer.Find("masks") != nullptr &&
                layer.Find("transform")->Find("keys")->Find("rotation") !=
                    nullptr;
        }
    }
    Check(commonOpacity,
          "opacity, rotation and clipping apply once to text and bands");
    for (const auto& note : report.notes) {
        Check(note.find("non traduit") == std::string::npos,
              "the highlight is translated: " + note);
    }
}

// MB3 -- glyph animators may travel beyond the unanimated text box,
// including after the highlight itself stopped drawing.
void TestHighlightUnitBounds() {
    const std::string json = R"J({
      "width": 600, "height": 300, "fps": 25, "duration": 30,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"},
        "children": [{"type": "text", "content": "a abcdefghij", "units": "word",
          "stagger": {"each": 12}, "style": {"fontSize": 20},
          "animate": {"scale": {"interpolate": {
            "frames": [0, 5, 10], "values": [1, 4, 1]}},
            "translateY": {"interpolate": {
              "frames": [0, 5, 10], "values": [0, 100, 0]}}},
          "highlights": [{"unit": "word", "start": 0, "color": "#ff0",
            "progress": {"interpolate": {"frames": [0, 10], "values": [0, 1]}}}]}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "highlight units"))
        return;
    bool held = false;
    for (const auto& comp : description.Find("comps")->AsArray()) {
        if (comp.Find("name")->AsString().find("Surligneur") != 0) continue;
        int64_t width = 0, height = 0;
        comp.Find("width")->AsInt64(width);
        comp.Find("height")->AsInt64(height);
        // At frame 17 the second word spans [-130, 270] before glyph
        // overhang, and its bottom is 160. The first word alone cannot
        // produce those bounds during the highlight's [0,10] window.
        held = width >= 400 && height >= 160 &&
               comp.Find("layers")->AsArray().back().Find("text")->Find(
                   "units") != nullptr;
    }
    Check(held, "the frame contains every unit's translate, scale and stagger");
}

// MB3/MB11 -- the default's empty stroke must not erase an instance's
// moving one when the exporter collects its animation segments.
void TestBoundHighlightProgress() {
    const std::string json = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 20,
      "props": {"fin": {"type": "number", "default": 0}},
      "root": {"type": "fill", "style": {"fontFamily": "Fake"},
        "children": [{"type": "text", "content": "un deux", "style": {"fontSize": 20},
          "highlights": [{"unit": "word", "start": 0, "color": "#ff0",
            "progress": {"interpolate": {"frames": [0, 10],
              "values": [0, {"prop": "fin"}]}}}]}]}})J";
    MotionComposition composition;
    MotionError error;
    FakeMeasurer measurer;
    mcp_json::Value props = mcp_json::Value::MakeObject();
    props.Set("fin", mcp_json::Value::MakeInt(1));
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!ParseMotionComposition(json, composition, error) ||
        !ExportMotionToAfter(composition, props, measurer, {}, {}, description,
                             report, error)) {
        Check(false, "a bound highlight exports: " + error.message);
        return;
    }
    bool moving = false;
    for (const auto& comp : description.Find("comps")->AsArray()) {
        for (const auto& layer : comp.Find("layers")->AsArray()) {
            const auto* rect = layer.Find("rect");
            if (rect == nullptr) continue;
            const auto* keys = rect->Find("size_keys");
            moving =
                moving ||
                (keys != nullptr &&
                 Near(keys->AsArray().back().Find("v")->AsArray()[0], 70.0));
        }
    }
    Check(moving, "progress uses the instance's 1 rather than its default's 0");
}

// MOTION-WEB-2026-10 MW7 -- the fake's text, set in a variable font whose
// named instances stand every hundred: 650 falls between two of them.
class VariableMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override {
        return text_.LayoutText(text, style, max_width, layout, error);
    }
    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override {
        return text_.ImageSize(source, width, height, error);
    }
    bool FontPostScriptName(const MotionTextStyle& style, std::string& name,
                            MotionError& /*error*/) override {
        name = style.font_family == "Static"
                   ? "Static-Bold"
                   : "Variable_wght" +
                         std::to_string(std::lround(style.font_weight));
        return true;
    }
    bool FontVariableWeight(const MotionTextStyle& style, int& weight,
                            bool& named) override {
        if (style.font_family == "Static") {
            return false;
        }
        weight = static_cast<int>(std::lround(style.font_weight));
        named = weight % 100 == 0;
        return true;
    }

private:
    FakeMeasurer text_;
};

// MW7 -- After is given a PostScript name and no axis: a text in an
// instance of a variable font is said, a named one as unchecked, one set
// between named instances as not translated; a static face says nothing.
void TestVariableFontNote() {
    const std::string json = R"J({
      "width": 400, "height": 300, "fps": 25, "duration": 10,
      "root": {"type": "fill", "style": {"fontFamily": "Variable"},
               "children": [
        {"type": "text", "content": "nommée",
         "style": {"position": "absolute", "left": 0, "top": 0,
                   "fontWeight": 600}},
        {"type": "text", "content": "entre deux",
         "style": {"position": "absolute", "left": 0, "top": 100,
                   "fontWeight": 650}},
        {"type": "text", "content": "statique",
         "style": {"position": "absolute", "left": 0, "top": 200,
                   "fontFamily": "Static", "fontWeight": 700}}]}})J";
    VariableMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "variable font")) {
        return;
    }
    std::vector<std::string> said;
    for (const std::string& note : report.notes) {
        if (note.find("fontWeight") != std::string::npos) {
            said.push_back(note);
        }
    }
    Check(said.size() == 2 &&
              said[0].find("children[0]") != std::string::npos &&
              said[0].find("instance nommée « Variable_wght600 »") !=
                  std::string::npos &&
              said[0].find("non vérifié") != std::string::npos &&
              said[1].find("children[1]") != std::string::npos &&
              said[1].find("wght 650") != std::string::npos &&
              said[1].find("non traduit") != std::string::npos,
          "two texts in a variable font, two notes, the static face none: " +
              std::to_string(said.size()));
}

// MOTION-WEB-2026-10 MW1 -- a key between two frames goes to After on the
// frame before it, and the report counts such instants once each.
void TestKeysBetweenFrames() {
    const auto card = [](const std::string& frames) {
        return R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 20,
      "root": {"type": "fill", "children": [
        {"type": "box", "style": {"width": 40, "height": 40,
                                  "backgroundColor": "#ff0000"},
         "animate": {"translateX": {"interpolate": {"frames": )J" +
               frames + R"J(, "values": [0, 50, 100]}},
                     "opacity": {"interpolate": {"frames": )J" +
               frames + R"J(, "values": [0, 1, 1]}}}}]}})J";
    };
    const auto between = [](const MotionAfterExportReport& report) {
        std::string said;
        for (const std::string& note : report.notes) {
            if (note.find("entre deux images") != std::string::npos) {
                said += note;
            }
        }
        return said;
    };
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (Describe(card(R"([0, "5/2", 10])"), measurer, description, report,
                 "keys between frames")) {
        Check(between(report).find("composition: 1 instant") == 0,
              "one instant between frames, shared by two tracks, is said "
              "once: " +
                  between(report));
    }
    MotionAfterExportReport whole;
    if (Describe(card("[0, 5, 10]"), measurer, description, whole,
                 "keys on frames")) {
        Check(between(whole).empty(), "keys on frames say nothing");
    }
}

// MOTION-WEB-2026-10 MW3 -- a GSAP curve reaches After as its closest
// cubic-bezier, and the report names those that are not one.
void TestGsapCurvesNoted() {
    const auto card = [](const std::string& easing) {
        return R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 20,
      "root": {"type": "fill", "children": [
        {"type": "box", "style": {"width": 40, "height": 40,
                                  "backgroundColor": "#ff0000"},
         "animate": {"translateX": {"interpolate": {"frames": [0, 10],
                       "values": [0, 100], "easing": )J" +
               easing + R"J(}}}}]}})J";
    };
    const auto curves = [](const MotionAfterExportReport& report) {
        std::string said;
        for (const std::string& note : report.notes) {
            if (note.find("courbes GSAP") != std::string::npos) {
                said += note;
            }
        }
        return said;
    };
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport approached;
    if (Describe(card(R"("power3.out")"), measurer, description, approached,
                 "power3.out")) {
        Check(
            curves(approached).find("power3.out (0,05 %)") != std::string::npos,
            "power3.out is approached, and its gap said: " +
                curves(approached));
    }
    MotionAfterExportReport exact;
    if (Describe(card(R"("power2.out")"), measurer, description, exact,
                 "power2.out")) {
        Check(curves(exact).empty(),
              "power2.out is a cubic-bezier: nothing to say");
    }
}

}  // namespace

int main() {
    TestEase();
    TestDescription();
    TestEasePerSegment();
    TestBoundKey();
    TestSequenceTimeAndTracking();
    TestFilterOverUnits();
    TestBoil();
    TestTimelineAudio();
    TestExportTool();
    TestMotionBlur();
    TestClipMask();
    TestColours();
    TestLayoutKeys();
    TestTwoScales();
    TestGroupFrameHoldsOverflow();
    TestLeftAndWidthKeyedTogether();
    TestSounds();
    TestVideo();
    TestNumericVariantNote();
    TestVariableFontNote();
    TestHighlights();
    TestHighlightUnitBounds();
    TestBoundHighlightProgress();
    TestKeysBetweenFrames();
    TestGsapCurvesNoted();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "motion after export: ok\n";
    return 0;
}
