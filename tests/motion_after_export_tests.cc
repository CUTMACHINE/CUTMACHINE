// MOTION-AFTER-2026-09 -- the description of a composition for After
// Effects, read without After: what each node became, where it sits, and
// the keyframes and eases After is given. MA1 -- export_motion_after, the
// tool that describes and hands the description to the bridge, with a
// describer and a bridge that stand in for Core Text and After. MA2 --
// verify_after_export, with a renderer and an After that stand in for the
// real ones. The build itself and the comparison of After's frames with the
// engine's run against After (tools/after-bridge/scripts/motion_import.jsx,
// verify_after_export). MB7 -- the other way: a reading of the bridge's
// read_comp translated into a composition, and import_after_comp, with an
// importer and a bridge that stand in for Core Text and After.

#include "AfterBridge.h"
#include "Document.h"
#include "Json.h"
#include "Keyframes.h"
#include "McpBackend.h"
#include "McpTools.h"
#include "MotionAfterExport.h"
#include "MotionAfterImport.h"
#include "MotionAfterVerify.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "Ulid.h"
#include "golden_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

// MW7 -- a text in an instance of a variable font gives After its family,
// its weight and its slope, whatever the instance, named or not, and the
// report says nothing; a static face gives only its PostScript name.
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
    const mcp_json::Value* main = MainComp(description);
    const auto variableOf = [&](const std::string& name) -> std::string {
        const mcp_json::Value* layer =
            main != nullptr ? Layer(*main, name) : nullptr;
        const mcp_json::Value* text =
            layer != nullptr ? layer->Find("text") : nullptr;
        const mcp_json::Value* axes =
            text != nullptr ? text->Find("variable") : nullptr;
        return axes != nullptr ? axes->Dump() : "none";
    };
    Check(variableOf("nommée") ==
                  R"({"family":"Variable","wght":600,"italic":false})" &&
              variableOf("entre deux") ==
                  R"({"family":"Variable","wght":650,"italic":false})" &&
              variableOf("statique") == "none",
          "the variable texts give their axes, the static one none: " +
              variableOf("nommée") + " " + variableOf("entre deux") + " " +
              variableOf("statique"));
    Check(!Said(report, "fontWeight"),
          "the report says nothing of the weights: " +
              (report.notes.empty() ? std::string() : report.notes[0]));
}

// MOTION-WEB-2026-10 MW2 -- a key between two frames goes to After at its
// instant, spelled "n/d" frames as the format spells it, with the eases of
// its true spans; a key on a frame stays an integer, and the report says
// nothing of either (MW1 said « posé sur l'image d'avant »).
void TestKeysBetweenFrames() {
    const auto card = [](const std::string& fps, const std::string& frames) {
        return R"J({
      "width": 400, "height": 200, "fps": )J" +
               fps + R"J(, "duration": 20,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "carre",
         "style": {"width": 40, "height": 40, "backgroundColor": "#ff0000"},
         "animate": {"translateX": {"interpolate": {"frames": )J" +
               frames + R"J(, "values": [0, 50, 100],
                       "easing": "cubic-bezier(0.5, 0.5, 0.5, 0.5)"}},
                     "opacity": {"interpolate": {"frames": )J" +
               frames + R"J(, "values": [0, 1, 1]}}}}]}})J";
    };
    const auto spelled = [](const mcp_json::Value* keys) {
        std::string out;
        if (keys == nullptr || !keys->IsArray()) {
            return std::string("none");
        }
        for (const mcp_json::Value& key : keys->AsArray()) {
            out += (out.empty() ? "" : " ") + key.Find("f")->Dump();
        }
        return out;
    };
    const auto keysOf = [](const mcp_json::Value& description,
                           const std::string& property) {
        const mcp_json::Value* main = MainComp(description);
        const mcp_json::Value* layer =
            main != nullptr ? Layer(*main, "carre") : nullptr;
        const mcp_json::Value* transform =
            layer != nullptr ? layer->Find("transform") : nullptr;
        const mcp_json::Value* keys =
            transform != nullptr ? transform->Find("keys") : nullptr;
        return keys != nullptr ? keys->Find(property) : nullptr;
    };
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (Describe(card("25", R"([0, "5/2", 10])"), measurer, description, report,
                 "keys between frames")) {
        const mcp_json::Value* x = keysOf(description, "position.x");
        Check(spelled(x) == R"(0 "5/2" 10)",
              "position keys at their instants: " + spelled(x));
        Check(spelled(keysOf(description, "opacity")) == R"(0 "5/2" 10)",
              "opacity keys at their instants: " +
                  spelled(keysOf(description, "opacity")));
        // 50 px in 0.1 s, then 50 px in 0.3 s, along a straight curve: the
        // speeds of the true spans (on the frame before, 0.08 s gave 625).
        if (x != nullptr && x->AsArray().size() == 3) {
            const auto speed = [&](size_t key, const char* side) {
                const mcp_json::Value* ease = x->AsArray()[key].Find(side);
                return ease != nullptr
                           ? std::stod(ease->AsArray()[0].AsArray()[0].Dump())
                           : -1.0;
            };
            Check(std::abs(speed(0, "ease_out") - 500.0) < 1e-3 &&
                      std::abs(speed(1, "ease_in") - 500.0) < 1e-3 &&
                      std::abs(speed(1, "ease_out") - 500.0 / 3.0) < 1e-3,
                  "eases measured on the exact spans: " +
                      std::to_string(speed(0, "ease_out")) + ", " +
                      std::to_string(speed(1, "ease_in")) + ", " +
                      std::to_string(speed(1, "ease_out")));
        }
        Check(report.notes.empty(),
              "keys between frames say nothing: " +
                  (report.notes.empty() ? std::string() : report.notes[0]));
    }
    MotionAfterExportReport whole;
    if (Describe(card("25", "[0, 5, 10]"), measurer, description, whole,
                 "keys on frames")) {
        Check(spelled(keysOf(description, "position.x")) == "0 5 10" &&
                  whole.notes.empty(),
              "keys on frames stay integers: " +
                  spelled(keysOf(description, "position.x")));
    }
    MotionAfterExportReport ntsc;
    if (Describe(card(R"("30000/1001")", R"([0, "1/3", "10/3"])"), measurer,
                 description, ntsc, "keys between frames at 29.97")) {
        Check(spelled(keysOf(description, "position.x")) == R"(0 "1/3" "10/3")",
              "a third of a frame at 29.97 stays a third: " +
                  spelled(keysOf(description, "position.x")));
    }
    // A sequence's own keys are moved by its start (MA8), exactly.
    const std::string scene = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 30,
      "root": {"type": "fill", "children": [
        {"type": "sequence", "id": "scene", "from": 10, "duration": 20,
         "animate": {"opacity": {"interpolate": {"frames": [0, "5/2"],
                                                 "values": [0, 1]}}},
         "children": [{"type": "box", "style": {"width": 40, "height": 40,
                                                "backgroundColor": "#fff"}}]}]}})J";
    MotionAfterExportReport shifted;
    if (Describe(scene, measurer, description, shifted, "a scene's keys")) {
        const std::string dump = description.Dump();
        Check(dump.find(R"("f":10,)") != std::string::npos &&
                  dump.find(R"("f":"25/2")") != std::string::npos,
              "a scene's key at 5/2 lands at 25/2 in its parent");
    }
    // What the layout moves is read at whole frames: a width that ends
    // between two frames pushes its neighbour until the frame after.
    const std::string row = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 10,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "box", "style": {"position": "absolute", "left": 0,
                                  "top": 100, "flexDirection": "row"},
         "children": [
           {"type": "box", "id": "pousse",
            "style": {"width": 20, "height": 10, "backgroundColor": "#000"},
            "animate": {"width": {"interpolate": {"frames": [0, "5/2"],
               "values": [20, 120]}}}},
           {"type": "text", "content": "mot"}]}]}})J";
    MotionAfterExportReport pushed;
    if (Describe(row, measurer, description, pushed, "a pushed word")) {
        const mcp_json::Value* main = MainComp(description);
        const mcp_json::Value* word =
            main != nullptr ? Layer(*main, "mot") : nullptr;
        const mcp_json::Value* keys =
            word != nullptr && word->Find("transform") != nullptr &&
                    word->Find("transform")->Find("keys") != nullptr
                ? word->Find("transform")->Find("keys")->Find("position.x")
                : nullptr;
        const bool read =
            keys != nullptr && keys->IsArray() && !keys->AsArray().empty();
        Check(read && Frame(keys->AsArray().back()) == 3 &&
                  std::abs(First(keys->AsArray().back()) -
                           First(keys->AsArray().front()) - 100.0) < 1e-3,
              "the word is pushed until frame 3, by the whole width: " +
                  (read ? keys->Dump() : std::string("no keys")));
    }
}

// MOTION-WEB-2026-10 MW5 -- transformOrigin becomes the layer's anchor
// point, the position following it; a skew becomes the Transform effect,
// skewX(a) as -a on axis 90, skewY(a) as a on axis 0 (measured in After 26),
// with the angle a footage or precomposition layer needs under a scale that
// is not uniform; a skewed group is a precomposition; an exact skew says
// nothing.
void TestOriginAndSkew() {
    const std::string json = R"J({
      "width": 400, "height": 300, "fps": 25, "duration": 10,
      "root": {"type": "fill", "children": [
        {"type": "box", "id": "pendule",
         "style": {"position": "absolute", "left": 100, "top": 50,
                   "width": 40, "height": 80, "backgroundColor": "#fff",
                   "transformOrigin": "50% 0"},
         "animate": {"rotate": {"interpolate": {"frames": [0, 9],
                                                "values": [-20, 20]}}}},
        {"type": "box", "id": "penche",
         "style": {"position": "absolute", "left": 200, "top": 50,
                   "width": 40, "height": 40, "backgroundColor": "#fff",
                   "skewX": 20},
         "animate": {"skewX": {"interpolate": {"frames": [0, "5/2"],
                                               "values": [20, 0]}}}},
        {"type": "box", "id": "large",
         "style": {"position": "absolute", "left": 300, "top": 50,
                   "width": 40, "height": 40, "backgroundColor": "#fff",
                   "skewY": 30, "scaleX": 2}},
        {"type": "box", "id": "groupe",
         "style": {"position": "absolute", "left": 0, "top": 200,
                   "width": 100, "height": 50, "skewX": -10},
         "children": [{"type": "box", "style": {"width": 50, "height": 50,
                                                "backgroundColor": "#f00"}},
                      {"type": "box", "style": {"width": 50, "height": 50,
                                                "backgroundColor": "#0f0"}}]}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "origin and skew")) {
        return;
    }
    const mcp_json::Value* main = MainComp(description);
    const auto transformOf =
        [&](const std::string& name) -> const mcp_json::Value* {
        const mcp_json::Value* layer =
            main != nullptr ? Layer(*main, name) : nullptr;
        return layer != nullptr ? layer->Find("transform") : nullptr;
    };
    const mcp_json::Value* pendulum = transformOf("pendule");
    Check(pendulum != nullptr &&
              Near(pendulum->Find("anchor")->AsArray()[0], 20.0) &&
              Near(pendulum->Find("anchor")->AsArray()[1], 0.0) &&
              Near(pendulum->Find("position")->AsArray()[0], 120.0) &&
              Near(pendulum->Find("position")->AsArray()[1], 50.0),
          "the anchor sits on the origin, the position where it lands: " +
              (pendulum != nullptr ? pendulum->Dump() : std::string("none")));
    const mcp_json::Value* slanted = transformOf("penche");
    const mcp_json::Value* skew =
        slanted != nullptr ? slanted->Find("skew") : nullptr;
    Check(skew != nullptr && Near(*skew->Find("axis"), 90.0) &&
              Near(*skew->Find("angle"), -20.0) &&
              skew->Find("raster_angle") == nullptr &&
              skew->Find("keys") != nullptr &&
              skew->Find("keys")->AsArray().size() == 2 &&
              skew->Find("keys")->AsArray()[1].Find("f")->Dump() == R"("5/2")",
          "skewX(20) is a skew of -20 on axis 90, its keys at their "
          "instants: " +
              (skew != nullptr ? skew->Dump() : std::string("none")));
    const mcp_json::Value* wide = transformOf("large");
    const mcp_json::Value* wideSkew =
        wide != nullptr ? wide->Find("skew") : nullptr;
    const double expected =
        std::atan(std::tan(30.0 * M_PI / 180.0) * 2.0) * 180.0 / M_PI;
    Check(wideSkew != nullptr && Near(*wideSkew->Find("axis"), 0.0) &&
              Near(*wideSkew->Find("angle"), 30.0) &&
              wideSkew->Find("raster_angle") != nullptr &&
              Near(*wideSkew->Find("raster_angle"), expected),
          "skewY under scaleX 2: 30 on axis 0 for a shape, the angle a "
          "footage layer needs before its scale beside it: " +
              (wideSkew != nullptr ? wideSkew->Dump() : std::string("none")));
    Check(Comp(description, "groupe") != nullptr,
          "a skewed group is a precomposition");
    Check(report.notes.empty(),
          "an origin and exact skews say nothing: " +
              (report.notes.empty() ? std::string() : report.notes[0]));
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

// MA7 -- a measurer that cannot lay one text out: the timecode of frame 7.
class FailingMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override {
        if (text == "00:00:00:07") {
            error = {"FontMissing", "", "no glyphs for frame 7"};
            return false;
        }
        return inner_.LayoutText(text, style, max_width, layout, error);
    }

    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override {
        return inner_.ImageSize(source, width, height, error);
    }

    bool FontPostScriptName(const MotionTextStyle& style, std::string& name,
                            MotionError& error) override {
        return inner_.FontPostScriptName(style, name, error);
    }

private:
    FakeMeasurer inner_;
};

// MA7 -- what the review of the export found, each shown failing first.
void TestExportEdges() {
    // A frame read only for the layout that does not evaluate: refused,
    // not taken for a frame where nothing is shown.
    FailingMeasurer failing;
    mcp_json::Value description;
    MotionAfterExportReport report;
    MotionComposition composition;
    MotionError error;
    MotionAfterExportOptions options;
    const std::string sliding = R"J({
      "width": 400, "height": 200, "fps": 25, "duration": 10,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "box", "style": {"position": "absolute", "left": 0, "top": 0, "height": 20,
                                  "backgroundColor": "#ff0000"},
         "animate": {"width": {"interpolate": {"frames": [0, 9], "values": [10, 100]}}}},
        {"type": "text", "content": {"timecode": "composition"},
         "style": {"position": "absolute", "left": 0, "top": 100, "fontSize": 20}}]}})J";
    if (ParseMotionComposition(sliding, composition, error)) {
        Check(!ExportMotionToAfter(composition, mcp_json::Value(), failing, {},
                                   options, description, report, error) &&
                  error.code == "FontMissing",
              "a frame of the layout that does not evaluate refuses the "
              "export: " +
                  error.code);
    } else {
        Check(false, "the sliding composition parses: " + error.message);
    }

    // A box that clips, wider than After's largest composition: its frame
    // is cut to 30 000 px, and the report says so.
    FakeMeasurer measurer;
    if (Describe(R"J({"width": 1920, "height": 1080, "fps": 25, "duration": 10,
          "root": {"type": "fill", "children": [
            {"type": "box", "id": "bande", "style": {"position": "absolute", "left": -20000, "top": 0,
              "width": 40000, "height": 100, "overflow": "hidden", "opacity": 0.5,
              "backgroundColor": "#00ff00"},
             "children": [{"type": "box", "style": {"width": 40000, "height": 100,
                                                    "backgroundColor": "#0000ff"}}]}]}})J",
                 measurer, description, report, "a wide clipping box")) {
        bool within = true;
        for (const mcp_json::Value& comp :
             description.Find("comps")->AsArray()) {
            int64_t width = 0;
            comp.Find("width")->AsInt64(width);
            within = within && width <= 30000;
        }
        bool said = false;
        for (const std::string& note : report.notes) {
            said = said || note.find("30 000 px") != std::string::npos;
        }
        Check(within && said,
              "no composition over 30 000 px, and the cut is said");
    }

    // Blend modes: the parser's, and the mattes, which After has no layer
    // mode for.
    if (Describe(R"J({"width": 200, "height": 100, "fps": 25, "duration": 10,
          "root": {"type": "fill", "children": [
            {"type": "box", "style": {"width": 50, "height": 50, "backgroundColor": "#ff0000",
                                      "mixBlendMode": "multiply"}},
            {"type": "box", "style": {"width": 50, "height": 50, "backgroundColor": "#00ff00",
                                      "mixBlendMode": "mask-in"}}]}})J",
                 measurer, description, report, "blend modes")) {
        const std::string dumped = description.Dump();
        bool said = false;
        for (const std::string& note : report.notes) {
            said = said || note.find("mask-in") != std::string::npos;
        }
        Check(dumped.find("\"MULTIPLY\"") != std::string::npos && said,
              "multiply is After's, mask-in is said to have no layer mode");
    }
    Check(!ParseMotionComposition(
              R"J({"width": 200, "height": 100, "fps": 25, "duration": 10,
                   "root": {"type": "fill", "style": {"mixBlendMode": "hue"}}})J",
              composition, error),
          "hue, which the export no longer names, is refused by the parser");
}

// MA3 -- the layer of the node at `path` in a composition of a description.
const mcp_json::Value* LayerAt(const mcp_json::Value& comp,
                               const std::string& path) {
    for (const mcp_json::Value& layer : comp.Find("layers")->AsArray()) {
        const mcp_json::Value* at = layer.Find("path");
        if (at != nullptr && at->AsString() == path &&
            layer.Find("kind")->AsString() == "text") {
            return &layer;
        }
    }
    return nullptr;
}

// MA3 -- a flow's terms as "layer-name:k" pairs, in order.
std::string FlowOf(const mcp_json::Value& comp, const mcp_json::Value* layer) {
    if (layer == nullptr || layer->Find("flow") == nullptr) {
        return "";
    }
    std::string said;
    for (const mcp_json::Value& term :
         layer->Find("flow")->Find("terms")->AsArray()) {
        std::string name = "?";
        for (const mcp_json::Value& other : comp.Find("layers")->AsArray()) {
            if (other.Find("id")->AsString() ==
                term.Find("layer")->AsString()) {
                name = other.Find("path")->AsString();
            }
        }
        char k[32];
        std::snprintf(k, sizeof k, "%g", std::stod(term.Find("k")->Dump()));
        said += (said.empty() ? "" : " ") + name + ":" + k;
    }
    return said;
}

// MA3 -- a row of words keeps its flexbox push in After: each text names
// the texts whose change of width moves it, with the engine's coefficient.
void TestRowFlow() {
    const std::string json = R"J({
      "width": 1000, "height": 400, "fps": 25, "duration": 10,
      "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
        {"type": "box", "style": {"position": "absolute", "left": 100, "top": 20, "gap": 10}, "children": [
          {"type": "text", "content": "COU", "style": {"fontSize": 40},
           "animate": {"scale": {"interpolate": {"frames": [0, 4], "values": [1.2, 1]}}}},
          {"type": "text", "content": "PER", "style": {"fontSize": 40},
           "animate": {"translateY": {"interpolate": {"frames": [0, 5], "values": [20, 0]}}}},
          {"type": "text", "content": "X", "style": {"fontSize": 40, "width": 30}}]},
        {"type": "box", "style": {"position": "absolute", "left": 0, "top": 120, "width": 1000,
                                  "justifyContent": "center"}, "children": [
          {"type": "text", "content": "COU", "style": {"fontSize": 40}},
          {"type": "text", "content": "PER", "style": {"fontSize": 40}}]},
        {"type": "box", "style": {"position": "absolute", "left": 0, "top": 200, "flexDirection": "column"}, "children": [
          {"type": "text", "content": "A", "style": {"fontSize": 40}},
          {"type": "text", "content": "B", "style": {"fontSize": 40}}]},
        {"type": "box", "style": {"position": "absolute", "left": 0, "top": 300}, "children": [
          {"type": "text", "content": "C", "style": {"fontSize": 40}},
          {"type": "text", "content": "D", "style": {"fontSize": 40, "position": "absolute", "left": 500}}]},
        {"type": "text", "content": "LIBRE", "style": {"position": "absolute", "left": 10, "top": 360, "fontSize": 40}}]}})J";
    FakeMeasurer measurer;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!Describe(json, measurer, description, report, "rows")) {
        return;
    }
    const mcp_json::Value* main = MainComp(description);
    if (main == nullptr) {
        Check(false, "the film has its main composition");
        return;
    }
    const std::string a = "root.children[0].children[";
    const std::string b = "root.children[1].children[";
    const mcp_json::Value* first = LayerAt(*main, a + "0]");
    const mcp_json::Value* flow =
        first != nullptr ? first->Find("flow") : nullptr;
    Check(FlowOf(*main, first).empty() && flow != nullptr &&
              flow->Find("terms")->AsArray().empty() &&
              Near(*flow->Find("anchor"), 0.5) &&
              flow->Find("self")->AsString() == first->Find("id")->AsString(),
          "the first word of a row packed to the start moves for no one, "
          "but the centre it scales about follows its own width");
    const mcp_json::Value* second = LayerAt(*main, a + "1]");
    Check(second != nullptr && second->Find("flow") != nullptr &&
              second->Find("flow")->Find("anchor") == nullptr,
          "a word that only slides keeps its anchor");
    Check(FlowOf(*main, LayerAt(*main, a + "1]")) == a + "0]:1",
          "the second moves by the first one's change of width: " +
              FlowOf(*main, LayerAt(*main, a + "1]")));
    Check(FlowOf(*main, LayerAt(*main, a + "2]")) == a + "0]:1 " + a + "1]:1",
          "a text of fixed width is pushed, and pushes nothing: " +
              FlowOf(*main, LayerAt(*main, a + "2]")));
    Check(FlowOf(*main, LayerAt(*main, b + "0]")) ==
                  b + "0]:-0.5 " + b + "1]:-0.5" &&
              FlowOf(*main, LayerAt(*main, b + "1]")) ==
                  b + "0]:0.5 " + b + "1]:-0.5",
          "a centred row spreads each change on both sides: " +
              FlowOf(*main, LayerAt(*main, b + "0]")) + " / " +
              FlowOf(*main, LayerAt(*main, b + "1]")));
    Check(
        FlowOf(*main, LayerAt(*main, "root.children[2].children[1]")).empty() &&
            FlowOf(*main, LayerAt(*main, "root.children[3].children[0]"))
                .empty() &&
            FlowOf(*main, LayerAt(*main, "root.children[4]")).empty(),
        "a column, a row with a word out of the flow and a free text keep "
        "their fixed places");
    // Without a row, the description is the one it was.
    mcp_json::Value plain;
    if (Describe(R"J({"width": 400, "height": 200, "fps": 25, "duration": 10,
          "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
            {"type": "text", "content": "COU", "style": {"position": "absolute", "left": 10, "top": 10, "fontSize": 40}},
            {"type": "text", "content": "PER", "style": {"position": "absolute", "left": 90, "top": 10, "fontSize": 40}}]}})J",
                 measurer, plain, report, "absolute words")) {
        Check(plain.Dump().find("\"flow\"") == std::string::npos,
              "words placed one by one have no flow");
    }
}

// MA2 -- the composition the scopes and the verification read: a slice
// that cuts at frame 5 only, one that always cuts but whose text is gone
// from frame 8, and a rounded letterSpacing shown from frame 3 to 5.
const char kScoped[] = R"J({
  "width": 20, "height": 10, "fps": 25, "duration": 10,
  "root": {"type": "fill", "style": {"fontFamily": "Fake"}, "children": [
    {"type": "text", "content": "UN", "style": {"position": "absolute", "left": 0, "top": 0, "fontSize": 4,
                                               "filter": "slice(0px 10px 3 0)"},
     "animate": {"filter": {"interpolate": {"frames": [4, 5, 6],
       "values": ["slice(0px 10px 3 4)", "slice(8px 10px 3 5)", "slice(0px 10px 3 6)"], "easing": "step-end"}}}},
    {"type": "text", "content": "DEUX", "style": {"position": "absolute", "left": 0, "top": 5, "fontSize": 4,
                                                 "filter": "slice(5px 10px 3 0)"},
     "animate": {"opacity": {"interpolate": {"frames": [0, 7, 8], "values": [1, 1, 0], "easing": "step-end"}}}},
    {"type": "sequence", "from": 3, "duration": 3, "children": [
      {"type": "text", "content": "TROIS", "style": {"position": "absolute", "left": 10, "top": 0, "fontSize": 3,
                                                    "letterSpacing": 1}}]}]}})J";

std::vector<size_t> ShownAt(const MotionComposition& composition,
                            const MotionAfterExportReport& report,
                            int64_t frame) {
    FakeMeasurer measurer;
    MotionFrame evaluated;
    MotionError error;
    if (!EvaluateMotionFrame(composition, mcp_json::Value(), frame, measurer,
                             evaluated, error)) {
        Check(false, "frame " + std::to_string(frame) + ": " + error.message);
        return {};
    }
    return MotionAfterNotesShown(report, evaluated);
}

// MA2 -- each note keeps the node it is about, a slice() only while it
// cuts, and a note about the whole composition none.
void TestNoteScopes() {
    MotionComposition composition;
    MotionError error;
    if (!ParseMotionComposition(kScoped, composition, error)) {
        Check(false, "the scoped composition parses: " + error.message);
        return;
    }
    FakeMeasurer measurer;
    MotionAfterExportOptions options;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!ExportMotionToAfter(composition, mcp_json::Value(), measurer, {},
                             options, description, report, error)) {
        Check(false, "the scoped composition is described: " + error.message);
        return;
    }
    size_t slice = report.notes.size();
    size_t spacing = report.notes.size();
    for (size_t index = 0; index < report.notes.size(); ++index) {
        if (report.notes[index].find("slice()") != std::string::npos) {
            slice = index;
        }
        if (report.notes[index].find("letterSpacing") != std::string::npos) {
            spacing = index;
        }
    }
    int slices = 0;
    for (const MotionAfterNoteScope& scope : report.scopes) {
        slices += scope.note == slice && scope.slice ? 1 : 0;
    }
    Check(slice < report.notes.size() && spacing < report.notes.size() &&
              slices == 2,
          "one note for both slice(), each node in its scope: " +
              std::to_string(slices));
    const auto shown = [&](int64_t frame) {
        const std::vector<size_t> notes = ShownAt(composition, report, frame);
        std::string said;
        for (const size_t note : notes) {
            said += note == slice     ? "slice "
                    : note == spacing ? "spacing "
                                      : "other ";
        }
        return said;
    };
    Check(shown(1) == "slice ", "frame 1: the second slice cuts: " + shown(1));
    Check(shown(5) == "slice spacing " || shown(5) == "spacing slice ",
          "frame 5: both cut, the rounded spacing is shown: " + shown(5));
    Check(shown(9).empty(),
          "frame 9: the first slice is at 0, the second text is hidden, the "
          "third gone: " +
              shown(9));
    MotionAfterExportReport whole = report;
    whole.notes.push_back("composition: grain non traduit");
    Check(ShownAt(composition, whole, 1).size() == 1,
          "a note about the whole composition never shows at a frame");
}

// MA2 -- a picture of `width` x `height`, 8-bit RGBA, every pixel `value`.
std::string Png(int32_t width, int32_t height, uint8_t value,
                uint8_t alpha = 255) {
    std::vector<uint8_t> samples;
    for (int32_t index = 0; index < width * height; ++index) {
        samples.insert(samples.end(), {value, value, value, alpha});
    }
    const std::string path =
        (std::filesystem::temp_directory_path() / (GenerateUlid() + ".png"))
            .string();
    std::string error;
    WriteGoldenPng(path, GoldenImageFromBytes(width, height, 4, samples),
                   GoldenDepth::Eight, error);
    std::ifstream file(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << file.rdbuf();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return bytes.str();
}

void TestPngLuma() {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<float> luma;
    std::string error;
    Check(MotionAfterPngLuma(Png(2, 1, 255), width, height, luma, error) &&
              width == 2 && height == 1 && luma.size() == 2 &&
              std::abs(luma[0] - 255.0F) < 1e-3F,
          "a white PNG is 255 levels: " + error);
    Check(MotionAfterPngLuma(Png(1, 1, 255, 128), width, height, luma, error) &&
              std::abs(luma[0] - 128.0F) < 1e-3F,
          "half transparent white over black is half way");
    const std::string path =
        (std::filesystem::temp_directory_path() / (GenerateUlid() + ".png"))
            .string();
    WriteGoldenPng(path, GoldenImageFromWords(1, 1, 3, {65535, 0, 0}),
                   GoldenDepth::Sixteen, error);
    std::ifstream file(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << file.rdbuf();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    Check(MotionAfterPngLuma(bytes.str(), width, height, luma, error) &&
              luma[0] == 76.0F,
          "16-bit red is weighed as Pillow's L weighs it (BT.601): " + error);
    Check(!MotionAfterPngLuma("not a png", width, height, luma, error),
          "bytes that are no PNG are refused");
    const MotionAfterFrameGap gap =
        MotionAfterCompareLuma({0.0F, 255.0F}, {1.0F, 245.0F});
    Check(std::abs(gap.mean - 5.5) < 1e-9 && std::abs(gap.max - 10.0) < 1e-9 &&
              std::abs(gap.over - 0.5) < 1e-9,
          "the mean, the largest gap and the share past 8 levels");
}

// MA2 -- a picture per frame: the frame's number in grey levels, so that a
// side that renders another frame shows.
bool FakeRender(const MotionFrameRequest& request, MotionFrameImage& image,
                MotionError& error) {
    FakeMeasurer measurer;
    if (!EvaluateMotionFrame(*request.composition, request.props, request.frame,
                             measurer, image.evaluated, error)) {
        return false;
    }
    image.png = Png(20, 10, static_cast<uint8_t>(request.frame * 10));
    image.width = 20;
    image.height = 10;
    return true;
}

// MA2 -- a project, and an After that renders what it is told to: the
// engine's picture, off by `gap` levels at the frames in `off`.
class VerifyBackend final : public McpBackend {
public:
    VerifyBackend() {
        DocumentComposition composition;
        composition.name = "Rangée";
        std::string parseError;
        mcp_json::Value::Parse(kScoped, composition.definition, parseError);
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
        AfterBridgeRequest request;
        if (!ParseAfterBridgeRequest(requestJson, request, message)) {
            errorName = "ValidationFailed";
            return false;
        }
        int64_t frame = 0;
        request.arguments.Find("frame")->AsInt64(frame);
        rendered.push_back(frame);
        comp_id = request.arguments.Find("comp_id")->AsString();
        const bool moved =
            std::find(off.begin(), off.end(), frame) != off.end();
        std::ofstream file(request.arguments.Find("image_path")->AsString(),
                           std::ios::binary);
        file << Png(width, 10,
                    static_cast<uint8_t>(frame * 10 + (moved ? 40 : 0)));
        AfterBridgeResponse response;
        response.ok = request.method == AfterBridgeMethod::RenderFrame;
        response.result = mcp_json::Value::MakeObject();
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
    std::vector<int64_t> off;
    std::vector<int64_t> rendered;
    std::string comp_id;
    int32_t width = 20;

private:
    Document document_;
};

// MA2 -- verify_after_export: both sides rendered, each frame judged, the
// pictures kept, and what it refuses.
void TestVerifyTool() {
    const McpToolRegistry registry;
    VerifyBackend backend;
    backend.off = {5, 9};
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-verif");
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("composition_id",
                  mcp_json::Value::MakeString(backend.composition_id));
    arguments.Set("comp_id", mcp_json::Value::MakeString("12"));
    arguments.Set("image_dir", mcp_json::Value::MakeString(directory.string()));
    mcp_json::Value frames = mcp_json::Value::MakeArray();
    for (const int64_t frame : {0, 5, 9}) {
        frames.Push(mcp_json::Value::MakeInt(frame));
    }
    arguments.Set("frames", frames);

    SetMotionAfterDescriber(&FakeDescribe);
    SetMotionFrameRenderer(nullptr);
    McpToolCallOutcome outcome =
        registry.Call(backend, "verify_after_export", arguments);
    Check(!outcome.ok && outcome.error_name == "UnsupportedOperation" &&
              backend.rendered.empty() && !std::filesystem::exists(directory),
          "without a renderer nothing is asked of After");

    SetMotionFrameRenderer(&FakeRender);
    outcome = registry.Call(backend, "verify_after_export", arguments);
    mcp_json::Value result;
    std::string parseError;
    mcp_json::Value::Parse(outcome.result_json, result, parseError);
    Check(outcome.ok && backend.rendered == std::vector<int64_t>({0, 5, 9}) &&
              backend.comp_id == "12",
          "After renders each frame of the composition it was given: " +
              outcome.message);
    if (outcome.ok) {
        const auto& checked = result.Find("frames")->AsArray();
        Check(result.Find("within")->Dump() == "1" &&
                  result.Find("announced")->Dump() == "1" &&
                  result.Find("unexplained")->Dump() == "1" &&
                  checked.size() == 3 &&
                  checked[0].Find("verdict")->AsString() == "within" &&
                  checked[1].Find("verdict")->AsString() == "announced" &&
                  checked[2].Find("verdict")->AsString() == "unexplained" &&
                  std::abs(std::stod(checked[1].Find("mean")->Dump()) - 40.0) <
                      1e-6,
              "frame 0 equal, 5 explained by the slice that cuts, 9 by "
              "nothing: " +
                  outcome.result_json);
        Check(checked[1].Find("notes") != nullptr &&
                  checked[1].Find("notes")->Dump().find("slice()") !=
                      std::string::npos &&
                  checked[2].Find("notes") == nullptr &&
                  result.Find("composition_notes")->IsArray(),
              "an announced frame names its notes");
        Check(std::filesystem::exists(directory / "moteur-000005.png") &&
                  std::filesystem::exists(directory / "after-000009.png"),
              "both pictures of each frame stay to be looked at");
    }
    outcome = registry.Call(backend, "verify_after_export", arguments);
    Check(!outcome.ok && outcome.error_name == "IoError",
          "an existing directory is refused, nothing overwritten");

    const std::filesystem::path other =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-verif");
    arguments.Set("image_dir", mcp_json::Value::MakeString(other.string()));
    arguments.Set("step", mcp_json::Value::MakeInt(2));
    outcome = registry.Call(backend, "verify_after_export", arguments);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed",
          "frames and step together are refused");
    mcp_json::Value outside = mcp_json::Value::MakeObject();
    for (const auto& [key, value] : arguments.AsObject()) {
        if (key != "step" && key != "frames") {
            outside.Set(key, value);
        }
    }
    mcp_json::Value late = mcp_json::Value::MakeArray();
    late.Push(mcp_json::Value::MakeInt(10));
    outside.Set("frames", late);
    outcome = registry.Call(backend, "verify_after_export", outside);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed" &&
              !std::filesystem::exists(other),
          "a frame past the composition is refused before anything is made");
    outside.Set("frames", frames);
    backend.width = 30;
    backend.rendered.clear();
    outcome = registry.Call(backend, "verify_after_export", outside);
    Check(
        !outcome.ok && outcome.error_name == "ValidationFailed" &&
            backend.rendered.size() == 1,
        "an After picture of another size stops the check: " + outcome.message);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::remove_all(other, ignored);
    SetMotionFrameRenderer(nullptr);
    SetMotionAfterDescriber(nullptr);
}

// MB7 -- a reading as read_comp.jsx gives it: neutral names, one of each
// kind the import handles or names. 25 fps, two seconds.
const char kReading[] = R"J({"schema":"after-response.v2","ok":true,
 "project_name":"Essai","path":"","item_count":3,"after_version":"26.0x67",
 "result":{"schema":"after-comp.v1","comp":{"id":"7","name":"Titre",
  "width":200,"height":100,"frame_rate":25,"frame_duration":0.04,
  "duration":2,"pixel_aspect":1,"background":[0,0,0],"motion_blur":false,
  "layers":[
   {"id":"11","index":1,"name":"TITRE","kind":"text","enabled":true,
    "guide":false,"solo":false,"three_d":false,"motion_blur":false,
    "parent_id":"","start":0,"in_point":0,"out_point":2,"stretch":100,
    "blending_mode":"NORMAL","track_matte":"ALPHA","is_track_matte":false,
    "track_matte_layer_id":"12","time_remap":false,"width":200,"height":100,
    "source":null,
    "transform":{"match_name":"ADBE Transform Group","name":"Transformer",
     "properties":{
      "ADBE Anchor Point":{"name":"Point d'ancrage","value":[0,0,0]},
      "ADBE Position":{"name":"Position","value":[20,60,0]},
      "ADBE Position_0":{"name":"Position X","value":0},
      "ADBE Position_1":{"name":"Position Y","value":0},
      "ADBE Scale":{"name":"Échelle","value":[100,100,100]},
      "ADBE Rotate Z":{"name":"Rotation","value":0},
      "ADBE Opacity":{"name":"Opacité","value":100}},"groups":[]},
    "masks":{"match_name":"ADBE Mask Parade","name":"Masques",
     "properties":{},"groups":[]},
    "effects":{"match_name":"ADBE Effect Parade","name":"Effets",
     "properties":{},"groups":[]},
    "styles":false,
    "text":{"keyed":false,"expression":false,"animators":0,"on_path":false,
     "text":"TITRE","font":"FakeSans-Bold","font_family":"Fake Sans",
     "font_style":"Bold","font_size":20,"apply_fill":true,
     "fill_color":[1,1,1],"apply_stroke":false,"justification":"LEFT_JUSTIFY",
     "tracking":50,"leading":24,"auto_leading":true,"all_caps":false,
     "small_caps":false,"faux_bold":false,"faux_italic":false,
     "baseline_shift":0,"horizontal_scale":1,"vertical_scale":1,
     "auto_kern":"NO_AUTO_KERN","box_text":false}},
   {"id":"12","index":2,"name":"Bande","kind":"shape","enabled":true,
    "guide":false,"solo":false,"three_d":false,"motion_blur":false,
    "parent_id":"11","start":0,"in_point":0,"out_point":2,"stretch":100,
    "blending_mode":"NORMAL","track_matte":"NO_TRACK_MATTE",
    "is_track_matte":true,"track_matte_layer_id":"","time_remap":false,
    "width":200,"height":100,"source":null,
    "transform":{"match_name":"ADBE Transform Group","name":"Transformer",
     "properties":{
      "ADBE Anchor Point":{"name":"Point d'ancrage","value":[0,0,0]},
      "ADBE Position":{"name":"Position","value":[0,0,0]},
      "ADBE Scale":{"name":"Échelle","value":[0,100,100],"keys":[
       {"t":0,"v":[0,100,100],"in_type":"bezier","out_type":"bezier",
        "in_ease":[[0,33.333333],[0,33.333333],[0,33.333333]],
        "out_ease":[[0,33.333333],[0,33.333333],[0,33.333333]]},
       {"t":0.4,"v":[100,100,100],"in_type":"bezier","out_type":"bezier",
        "in_ease":[[0,33.333333],[0,33.333333],[0,33.333333]],
        "out_ease":[[0,33.333333],[0,33.333333],[0,33.333333]]}]},
      "ADBE Rotate Z":{"name":"Rotation","value":0},
      "ADBE Opacity":{"name":"Opacité","value":100}},"groups":[]},
    "masks":null,"effects":null,"styles":false,
    "contents":{"match_name":"ADBE Root Vectors Group","name":"Contenu",
     "properties":{},"groups":[
      {"match_name":"ADBE Vector Group","name":"Rectangle 1","enabled":true,
       "properties":{},"groups":[
        {"match_name":"ADBE Vectors Group","name":"Contenu","properties":{},
         "groups":[
          {"match_name":"ADBE Vector Shape - Rect","name":"Rectangle Path 1",
           "enabled":true,"properties":{
            "ADBE Vector Rect Size":{"name":"Taille","value":[100,30]},
            "ADBE Vector Rect Position":{"name":"Position","value":[50,-5]},
            "ADBE Vector Rect Roundness":{"name":"Arrondi","value":0}},
           "groups":[]},
          {"match_name":"ADBE Vector Graphic - Fill","name":"Fill 1",
           "enabled":true,"properties":{
            "ADBE Vector Fill Color":{"name":"Couleur","value":[1,0,0,1]},
            "ADBE Vector Fill Opacity":{"name":"Opacité","value":100}},
           "groups":[]}]},
        {"match_name":"ADBE Vector Transform Group","name":"Transformer",
         "properties":{
          "ADBE Vector Anchor":{"name":"Point d'ancrage","value":[0,0]},
          "ADBE Vector Position":{"name":"Position","value":[0,0]},
          "ADBE Vector Scale":{"name":"Échelle","value":[100,100]},
          "ADBE Vector Rotation":{"name":"Rotation","value":0},
          "ADBE Vector Group Opacity":{"name":"Opacité","value":100}},
         "groups":[]}]}]}},
   {"id":"13","index":3,"name":"Contrôle","kind":"null","enabled":true,
    "guide":false,"parent_id":"","in_point":0,"out_point":2,"stretch":100,
    "blending_mode":"NORMAL","track_matte":"NO_TRACK_MATTE",
    "is_track_matte":false,"width":100,"height":100,"source":null,
    "transform":{"match_name":"ADBE Transform Group","name":"Transformer",
     "properties":{},"groups":[]},"masks":null,"effects":null},
   {"id":"14","index":4,"name":"Scène","kind":"av","enabled":true,
    "guide":false,"parent_id":"","in_point":0,"out_point":2,"stretch":100,
    "blending_mode":"NORMAL","track_matte":"NO_TRACK_MATTE",
    "is_track_matte":false,"width":200,"height":100,
    "source":{"kind":"comp","id":"3","name":"Scène"},
    "transform":{"match_name":"ADBE Transform Group","name":"Transformer",
     "properties":{},"groups":[]},"masks":null,"effects":null},
   {"id":"15","index":5,"name":"Fond","kind":"av","enabled":true,
    "guide":false,"parent_id":"","in_point":0.2,"out_point":2,
    "stretch":100,"blending_mode":"NORMAL","track_matte":"NO_TRACK_MATTE",
    "is_track_matte":false,"width":200,"height":100,
    "source":{"kind":"solid","name":"Fond","color":[0,0,1]},
    "transform":{"match_name":"ADBE Transform Group","name":"Transformer",
     "properties":{
      "ADBE Anchor Point":{"name":"Point d'ancrage","value":[100,50,0]},
      "ADBE Position":{"name":"Position","value":[100,50,0]},
      "ADBE Scale":{"name":"Échelle","value":[100,100,100]},
      "ADBE Rotate Z":{"name":"Rotation","value":0,
       "expression":"time * 25","expression_error":"",
       "samples_from":5,"samples":[5,6,7,7,7]},
      "ADBE Opacity":{"name":"Opacité","value":0,"keys":[
       {"t":0.2,"v":0,"in_type":"linear","out_type":"linear",
        "in_ease":[[0,16.666667]],"out_ease":[[500,16.666667]]},
       {"t":0.4,"v":100,"in_type":"linear","out_type":"hold",
        "in_ease":[[500,16.666667]],"out_ease":[[0,16.666667]]}]}},
     "groups":[]},
    "masks":null,"effects":null}]}}})J";

bool FakeFont(const std::string& name, MotionAfterFont& face) {
    if (name != "FakeSans-Bold") {
        return false;
    }
    face = {"Fake Sans", 700, false};
    return true;
}

const mcp_json::Value* Child(const mcp_json::Value& node, size_t index) {
    const mcp_json::Value* children = node.Find("children");
    return children != nullptr && children->IsArray() &&
                   index < children->AsArray().size()
               ? &children->AsArray()[index]
               : nullptr;
}

std::string At(const mcp_json::Value* node, const std::string& path) {
    const mcp_json::Value* value = node;
    std::stringstream stream(path);
    std::string key;
    while (value != nullptr && std::getline(stream, key, '.')) {
        value = value->Find(key);
    }
    return value == nullptr ? "<absent>" : value->Dump();
}

bool HasNote(const MotionAfterImportReport& report, const std::string& layer,
             const std::string& part) {
    return std::any_of(report.notes.begin(), report.notes.end(),
                       [&](const MotionAfterImportNote& note) {
                           return note.layer == layer &&
                                  note.note.find(part) != std::string::npos;
                       });
}

// MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. Each layer a node named
// after it, in After's stacking order; the matte a clip in the text's own
// box; keys with After's eases; what is computed, sampled; the rest named.
void TestImportComposition() {
    mcp_json::Value reading;
    std::string parseError;
    Check(mcp_json::Value::Parse(kReading, reading, parseError),
          "the reading parses: " + parseError);
    FakeMeasurer measurer;
    mcp_json::Value definition;
    MotionAfterImportReport report;
    MotionError error;
    const bool ok = ImportAfterComposition(reading, {}, &FakeFont, measurer,
                                           definition, report, error);
    Check(ok, "a reading imports: " + error.code + " " + error.message);
    Check(At(&definition, "fps") == "25" &&
              At(&definition, "duration") == "50" &&
              At(&definition, "width") == "200",
          "size, rate and length are After's: " + definition.Dump());
    const mcp_json::Value* root = definition.Find("root");
    const mcp_json::Value* fond = root ? Child(*root, 0) : nullptr;
    const mcp_json::Value* bande = root ? Child(*root, 1) : nullptr;
    const mcp_json::Value* titre = root ? Child(*root, 2) : nullptr;
    Check(
        root != nullptr && Child(*root, 3) == nullptr && fond && bande && titre,
        "three drawn layers, the null and the precomposition left out: " +
            (root ? root->Dump() : std::string()));
    if (!fond || !bande || !titre) {
        return;
    }
    // The solid starts at frame 5: a sequence, its keys in its own time.
    Check(At(fond, "type") == "\"sequence\"" &&
              At(fond, "id") == "\"Fond#plage\"" && At(fond, "from") == "5" &&
              At(fond, "duration") == "45",
          "a layer that starts late is a sequence: " + fond->Dump());
    const mcp_json::Value* solid = Child(*fond, 0);
    Check(solid && At(solid, "id") == "\"Fond\"" &&
              At(solid, "style.backgroundColor") == "\"#0000FF\"" &&
              At(solid, "style.transformOrigin") == "\"100px 50px\"" &&
              At(solid, "animate.opacity.interpolate.frames") == "[0,5]" &&
              At(solid, "animate.opacity.interpolate.values") == "[0,1]" &&
              At(solid, "animate.opacity.interpolate.easing") == "\"linear\"",
          "a solid is a box, its fade on its own time: " +
              (solid ? solid->Dump() : std::string()));
    Check(solid &&
              At(solid, "animate.rotate.interpolate.frames") == "[0,1,2,4]" &&
              At(solid, "animate.rotate.interpolate.values") == "[5,6,7,7]" &&
              HasNote(report, "Fond",
                      "Rotation : expression lue image par "
                      "image, valeurs figées"),
          "an expression becomes one key per frame where it changes, said: " +
              (solid ? solid->Dump() : std::string()));
    // The shape, parented to the text: a box with the text's transform
    // around the shape's own.
    Check(At(bande, "id") == "\"TITRE#Bande\"" &&
              At(bande, "style.translateX") == "20" &&
              At(bande, "style.translateY") == "60" &&
              At(bande, "animate") == "<absent>",
          "a parent is a box around its child, without its opacity: " +
              bande->Dump());
    const mcp_json::Value* shape = Child(*bande, 0);
    Check(shape && At(shape, "id") == "\"Bande\"" &&
              At(shape, "animate.scaleX.interpolate.frames") == "[0,10]" &&
              At(shape, "animate.scaleX.interpolate.values") == "[0,1]" &&
              At(shape, "animate.scaleX.interpolate.easing") ==
                  "\"cubic-bezier(0.3333, 0, 0.6667, 1)\"" &&
              At(shape, "style.scaleY") == "<absent>",
          "After's ease is the segment's cubic-bezier, one axis: " +
              (shape ? shape->Dump() : std::string()));
    const mcp_json::Value* group = shape ? Child(*shape, 0) : nullptr;
    const mcp_json::Value* rectangle = group ? Child(*group, 0) : nullptr;
    Check(group && At(group, "id") == "\"Rectangle 1\"" && rectangle &&
              At(rectangle, "type") == "\"box\"" &&
              At(rectangle, "style.left") == "0" &&
              At(rectangle, "style.top") == "-20" &&
              At(rectangle, "style.width") == "100" &&
              At(rectangle, "style.backgroundColor") == "\"#FF0000\"",
          "a filled rectangle is a box: " +
              (group ? group->Dump() : std::string()));
    // The text: its face, on its baseline (0.8 of the size, the fake
    // measurer's), the matte a clip in its own box.
    Check(At(titre, "type") == "\"text\"" &&
              At(titre, "style.fontFamily") == "\"Fake Sans\"" &&
              At(titre, "style.fontWeight") == "700" &&
              At(titre, "style.top") == "-16" &&
              At(titre, "style.left") == "0" &&
              At(titre, "style.letterSpacing") == "1" &&
              At(titre, "style.lineHeight") == "\"24px\"" &&
              At(titre, "style.translateX") == "20",
          "a text keeps its face and sits on its baseline: " + titre->Dump());
    const std::string clip = At(titre, "animate.clipPath.interpolate.values");
    Check(
        clip.find("\"polygon(0px -4px, 0px -4px, 0px 26px, 0px 26px)\"") == 1 &&
            clip.find("polygon(0px -4px, 50px -4px, 50px 26px, 0px 26px)") !=
                std::string::npos &&
            clip.find("polygon(0px -4px, 100px -4px, 100px 26px, 0px "
                      "26px)\"]") != std::string::npos,
        "the matte is the rectangle, frame by frame, in the text's box "
        "(half open at frame 5, After's ease): " +
            clip);
    Check(HasNote(report, "Scène", "précomposition") &&
              HasNote(report, "TITRE", "cache de piste de « Bande »") &&
              !HasNote(report, "TITRE", "crénage") && report.layers == 5,
          "what is left out or read is named: " +
              std::to_string(report.notes.size()));

    // Without the face, the import says the render will refuse it.
    report = {};
    Check(ImportAfterComposition(reading, {}, {}, measurer, definition, report,
                                 error) &&
              HasNote(report, "TITRE", "absente de cette machine"),
          "a missing face is named");
    // A replaced face is resolved by its replacement, and named.
    MotionAfterImportOptions options;
    options.fonts["FakeSans-Bold"] = "Autre-Regular";
    report = {};
    ImportAfterComposition(reading, options, &FakeFont, measurer, definition,
                           report, error);
    Check(HasNote(report, "TITRE", "remplacée par Autre-Regular") &&
              HasNote(report, "TITRE", "absente de cette machine"),
          "a replacement is named, and resolved like any face");

    mcp_json::Value other;
    mcp_json::Value::Parse(R"({"comp": {"name": "x"}})", other, parseError);
    Check(!ImportAfterComposition(other, {}, &FakeFont, measurer, definition,
                                  report, error) &&
              error.code == "InvalidValue",
          "what is not a reading is refused");
}

bool FakeImport(const MotionAfterImportRequest& request,
                mcp_json::Value& definition, MotionAfterImportReport& report,
                MotionError& error) {
    FakeMeasurer measurer;
    return ImportAfterComposition(*request.comp, request.options, &FakeFont,
                                  measurer, definition, report, error);
}

// The bridge answers read_comp with kReading; the composition added is
// kept.
class ImportBackend final : public McpBackend {
public:
    bool SnapshotDocument(Document& document, std::string&) override {
        document = {};
        return true;
    }

    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override {
        const auto* add =
            std::get_if<AddMotionCompositionOperation>(&operation);
        if (add == nullptr) {
            errorName = "UnsupportedOperation";
            message = "only an added composition here";
            return false;
        }
        added.push_back(add->composition);
        resultJson = R"({"revision": 1})";
        return true;
    }

    bool RunAfterRequest(const std::string& requestJson,
                         std::string& resultJson, std::string& errorName,
                         std::string& message) override {
        ++calls;
        if (!ParseAfterBridgeRequest(requestJson, request, message)) {
            errorName = "ValidationFailed";
            return false;
        }
        resultJson = kReading;
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

    int calls = 0;
    AfterBridgeRequest request;
    std::vector<DocumentComposition> added;
};

void TestImportTool() {
    const McpToolRegistry registry;
    ImportBackend backend;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("comp_id", mcp_json::Value::MakeString("7"));

    SetMotionAfterImporter(nullptr);
    McpToolCallOutcome outcome =
        registry.Call(backend, "import_after_comp", arguments);
    Check(!outcome.ok && outcome.error_name == "UnsupportedOperation" &&
              backend.calls == 0,
          "without Core Text the import is refused, After untouched");

    SetMotionAfterImporter(&FakeImport);
    const std::string kept = (std::filesystem::temp_directory_path() /
                              (GenerateUlid() + "-lecture.json"))
                                 .string();
    arguments.Set("reading_path", mcp_json::Value::MakeString(kept));
    outcome = registry.Call(backend, "import_after_comp", arguments);
    mcp_json::Value result;
    std::string parseError;
    mcp_json::Value::Parse(outcome.result_json, result, parseError);
    Check(outcome.ok && backend.calls == 1 &&
              backend.request.method == AfterBridgeMethod::ReadComp &&
              backend.request.arguments.Find("comp_id")->AsString() == "7" &&
              backend.added.size() == 1 && backend.added[0].name == "Titre" &&
              At(&result, "composition_id") ==
                  "\"" + backend.added[0].id + "\"" &&
              At(&result, "layers") == "5" && result.Find("notes") != nullptr &&
              std::filesystem::exists(kept),
          "the comp is read, translated and added by name, the reading "
          "kept: " +
              outcome.message + outcome.result_json);
    outcome = registry.Call(backend, "import_after_comp", arguments);
    Check(!outcome.ok && outcome.error_name == "IoError" && backend.calls == 1,
          "a reading is never written over a file, nor After asked");
    arguments.Set("reading_path", mcp_json::Value::MakeString("lecture.json"));
    outcome = registry.Call(backend, "import_after_comp", arguments);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed",
          "a relative reading path is refused");
    mcp_json::Value bad = mcp_json::Value::MakeObject();
    bad.Set("comp_id", mcp_json::Value::MakeString("7"));
    mcp_json::Value fonts = mcp_json::Value::MakeObject();
    fonts.Set("FakeSans-Bold", mcp_json::Value::MakeInt(3));
    bad.Set("fonts", fonts);
    outcome = registry.Call(backend, "import_after_comp", bad);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed",
          "fonts maps a name to a name");
    bad = mcp_json::Value::MakeObject();
    bad.Set("comp_id", mcp_json::Value::MakeString("7"));
    bad.Set("name", mcp_json::Value::MakeString("Carton importé"));
    outcome = registry.Call(backend, "import_after_comp", bad);
    Check(outcome.ok && backend.added.size() == 2 &&
              backend.added[1].name == "Carton importé",
          "a name of its own: " + outcome.message);
    std::error_code ignored;
    std::filesystem::remove(kept, ignored);
    SetMotionAfterImporter(nullptr);
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
    TestOriginAndSkew();
    TestGsapCurvesNoted();
    TestRowFlow();
    TestNoteScopes();
    TestPngLuma();
    TestVerifyTool();
    TestExportEdges();
    TestImportComposition();
    TestImportTool();
    if (failures > 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "motion after export: ok\n";
    return 0;
}
