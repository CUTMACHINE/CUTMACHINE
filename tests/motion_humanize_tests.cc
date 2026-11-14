// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. The humanize pass on fake
// metrics: each rule applied where it holds, left alone where it would move
// something, and what it leaves drawn as it was -- every glyph at the same
// place, as opaque, moved as far, at every frame checked. The proof by
// rendering (CompareMotionRenders) needs Core Graphics and runs in the
// executable; this suite holds the rules. Portable: no framework.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionHumanize.h"
#include "motion_fake_measurer.h"
#include "motion_humanize_fixtures.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using mcp_json::Value;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

Value Json(const std::string& text) {
    Value value;
    std::string error;
    Check(Value::Parse(text, value, error), "test JSON parses: " + error);
    return value;
}

std::string Card(const std::string& children, int64_t duration = 20) {
    return R"({"width": 400, "height": 200, "fps": 25, "duration": )" +
           std::to_string(duration) +
           R"(, "root": {"type": "fill", "style": {"fontFamily": "Fake",
              "fontSize": 20, "color": "#ffffff"}, "children": [)" +
           children + "]}}";
}

bool Humanize(const Value& definition, Value& output,
              MotionHumanizeReport& report) {
    FakeMotionMeasurer measurer;
    MotionError error;
    const bool ok = HumanizeMotionDefinition(definition, measurer, {}, output,
                                             report, error);
    Check(ok, "humanizes: " + error.code + " at " + error.path + " -- " +
                  error.message);
    return ok;
}

// One glyph as drawn: its character, where it lands once every move is
// applied, and how opaque -- every ancestor's share included, and a glyph a
// window (overflow: hidden) keeps out of sight left out.
struct Glyph {
    std::string character;
    double x = 0.0;
    double y = 0.0;
    double opacity = 1.0;

    bool operator<(const Glyph& other) const {
        return std::tie(x, y, character) <
               std::tie(other.x, other.y, other.character);
    }
};

struct Window {
    double left = -1e9;
    double top = -1e9;
    double right = 1e9;
    double bottom = 1e9;
};

void Collect(const MotionEvaluatedNode& node, double opacity, double dx,
             double dy, Window window, std::vector<Glyph>& glyphs) {
    opacity *= node.opacity;
    dx += node.translate_x;
    dy += node.translate_y;
    if (node.overflow_hidden) {
        window.left = std::max(window.left, node.box.x + dx);
        window.top = std::max(window.top, node.box.y + dy);
        window.right = std::min(window.right, node.box.x + node.box.width + dx);
        window.bottom =
            std::min(window.bottom, node.box.y + node.box.height + dy);
    }
    if (node.type == MotionNodeType::Text) {
        for (size_t index = 0; index < node.chars.size(); ++index) {
            const MotionTextSpan& span = node.chars[index];
            if (span.text == " ") {
                continue;
            }
            Glyph glyph{span.text, node.box.x + span.x + dx,
                        node.box.y + span.y + dy, opacity};
            for (const MotionTextUnitState& unit : node.units) {
                if (node.unit_kind == MotionTextUnit::Char &&
                    unit.index == index) {
                    glyph.opacity *= unit.opacity;
                    glyph.x += unit.translate_x;
                    glyph.y += unit.translate_y;
                }
            }
            const bool hidden = glyph.x + span.width <= window.left + 1e-9 ||
                                glyph.x >= window.right - 1e-9 ||
                                glyph.y + span.height <= window.top + 1e-9 ||
                                glyph.y >= window.bottom - 1e-9;
            if (!hidden) {
                glyphs.push_back(glyph);
            }
        }
    }
    for (const MotionEvaluatedNode& child : node.children) {
        Collect(child, opacity, dx, dy, window, glyphs);
    }
}

std::vector<Glyph> Glyphs(const Value& definition, int64_t frame) {
    MotionComposition composition;
    MotionError error;
    std::vector<Glyph> glyphs;
    if (!ParseMotionComposition(definition, composition, error)) {
        Check(false, "parses: " + error.message);
        return glyphs;
    }
    FakeMotionMeasurer measurer;
    MotionFrame evaluated;
    if (!EvaluateMotionFrame(composition, Value(), frame, measurer, {},
                             evaluated, error)) {
        Check(false, "evaluates: " + error.path + " " + error.message);
        return glyphs;
    }
    Collect(evaluated.root, 1.0, 0.0, 0.0, Window{}, glyphs);
    std::sort(glyphs.begin(), glyphs.end());
    return glyphs;
}

bool Same(const std::vector<Glyph>& a, const std::vector<Glyph>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t index = 0; index < a.size(); ++index) {
        if (a[index].character != b[index].character ||
            std::fabs(a[index].x - b[index].x) > 1e-9 ||
            std::fabs(a[index].y - b[index].y) > 1e-9 ||
            std::fabs(a[index].opacity - b[index].opacity) > 1e-9) {
            return false;
        }
    }
    return true;
}

void CheckDrawnAlike(const Value& before, const Value& after,
                     const std::vector<int64_t>& frames,
                     const std::string& label) {
    for (const int64_t frame : frames) {
        const std::vector<Glyph> a = Glyphs(before, frame);
        Check(!a.empty() && Same(a, Glyphs(after, frame)),
              label + ": drawn alike at frame " + std::to_string(frame));
    }
}

const Value* At(const Value& root, std::initializer_list<size_t> path) {
    const Value* node = root.Find("root");
    for (const size_t index : path) {
        const Value* children =
            node != nullptr ? node->Find("children") : nullptr;
        node = children != nullptr && index < children->AsArray().size()
                   ? &children->AsArray()[index]
                   : nullptr;
    }
    return node;
}

void TestTimecode() {
    // Scene one, from frame 0: the composition's timecode.
    const Value written = Json(Card(
        R"({"type": "sequence", "from": 0, "duration": 30, "children": [)" +
            TimecodeRow(0, 30, 11) + "]}",
        40));
    Value human;
    MotionHumanizeReport report;
    if (!Humanize(written, human, report)) {
        return;
    }
    const Value* row = At(human, {0, 0});
    const Value* text = row != nullptr ? At(human, {0, 0, 0}) : nullptr;
    Check(
        report.timecodes == 1 && text != nullptr &&
            text->Find("content")->Dump() == R"({"timecode":"composition"})" &&
            row->Find("children")->AsArray().size() == 1,
        "a row of digit strips reads the composition's timecode: one text");
    Check(text != nullptr &&
              text->Find("style")->Find("letterSpacing")->Dump() == "1",
          "spaced to its columns: 11 wide for glyphs 10 wide");
    Check(report.nodes_before == 3 + 3 * 2 + 8 * 22 && report.nodes_after == 4,
          "from " + std::to_string(report.nodes_before) + " nodes to " +
              std::to_string(report.nodes_after));
    CheckDrawnAlike(written, human, {0, 1, 9, 10, 24, 25, 29}, "the timecode");

    // A scene from frame 50 whose digits count from zero: the timecode of
    // the scene itself.
    const Value local = Json(Card(
        R"({"type": "sequence", "from": 50, "duration": 30, "children": [)" +
            TimecodeRow(0, 30, 11) + "]}",
        80));
    Value localHuman;
    if (Humanize(local, localHuman, report)) {
        const Value* localText = At(localHuman, {0, 0, 0});
        Check(localText != nullptr && localText->Find("content")->Dump() ==
                                          R"({"timecode":"local"})",
              "digits that count from the scene's start read its local "
              "timecode");
        CheckDrawnAlike(local, localHuman, {50, 60, 79}, "the local timecode");
    }

    // One digit wrong at one frame: not a timecode.
    const Value broken = Json(Card(
        R"({"type": "sequence", "from": 0, "duration": 30, "children": [)" +
            TimecodeRow(0, 30, 11, 17) + "]}",
        40));
    Value unchanged;
    if (Humanize(broken, unchanged, report)) {
        Check(report.timecodes == 0 && !report.notes.empty() &&
                  report.notes[0].find("l'image 17") != std::string::npos,
              "a row that leaves the timecode at frame 17 stays, and says "
              "where");
    }
}

void TestWrappers() {
    const Value written = Json(Card(R"(
        {"type": "box", "style": {"position": "absolute", "left": 40, "top": 30},
         "animate": {"opacity": {"interpolate": {"frames": [0, 4], "values": [0, 1]}},
                     "translateX": {"interpolate": {"frames": [0, 8], "values": [-20, 0]}}},
         "children": [{"type": "text", "content": "Titre"}]},
        {"type": "box", "id": "nom", "style": {"position": "absolute", "left": 40, "top": 90},
         "children": [{"type": "text", "content": "Nom"}]},
        {"type": "box", "style": {"position": "absolute", "left": 40, "top": 120},
         "animate": {"opacity": {"interpolate": {"frames": [0, 4], "values": [0, 1]}}},
         "children": [{"type": "text", "content": "Deux",
           "animate": {"translateX": {"interpolate": {"frames": [0, 8], "values": [-20, 0]}}}}]},
        {"type": "box", "style": {"position": "absolute", "left": 40, "top": 150, "padding": 4},
         "children": [{"type": "text", "content": "Marge"}]})"));
    Value human;
    MotionHumanizeReport report;
    if (!Humanize(written, human, report)) {
        return;
    }
    const Value* title = At(human, {0});
    Check(report.wrappers == 2 && title != nullptr &&
              title->Find("type")->AsString() == "text" &&
              title->Find("style")->Find("left")->Dump() == "40" &&
              title->Find("animate")->Find("translateX") != nullptr,
          "a box that only places a text folds into it, animations and all");
    const Value* named = At(human, {1});
    Check(named != nullptr && named->Find("type")->AsString() == "text" &&
              named->Find("id")->AsString() == "nom",
          "the box's id goes to its text");
    Check(At(human, {2})->Find("type")->AsString() == "box" &&
              At(human, {3})->Find("type")->AsString() == "box",
          "a box stays when both it and its text move, or when it pads");
    CheckDrawnAlike(written, human, {0, 2, 4, 8, 12}, "the wrappers");
}

void TestLetters() {
    // Fake glyphs are 10 wide at 20 px: letters placed 10 apart start where
    // one text puts them; a space is 10 more.
    const auto letter = [](const char* character, double left, int64_t at) {
        return std::string(R"({"type": "text", "content": ")") + character +
               R"(", "style": {"position": "absolute", "left": )" +
               std::to_string(left) +
               R"(, "top": 40}, "animate": {"opacity": {"interpolate": {"frames": [)" +
               std::to_string(at) + "," + std::to_string(at + 1) +
               R"(], "values": [0, 1], "easing": "step-end"}}}})";
    };
    const Value written =
        Json(Card(letter("C", 100, 0) + "," + letter("O", 110, 2) + "," +
                  letter("U", 120, 4) + "," + letter("P", 140, 6) + "," +
                  letter("E", 150, 8)));
    Value human;
    MotionHumanizeReport report;
    if (!Humanize(written, human, report)) {
        return;
    }
    const Value* word = At(human, {0});
    Check(report.words == 1 && report.letters == 5 && word != nullptr &&
              word->Find("content")->AsString() == "COU PE" &&
              word->Find("units")->AsString() == "char" &&
              word->Find("stagger")->Dump() == R"({"each":2})" &&
              human.Find("root")->Find("children")->AsArray().size() == 1,
          "letters each where the last ends, two frames apart, are one word "
          "in cascade, the gap a space");
    CheckDrawnAlike(written, human, {0, 1, 3, 5, 7, 9, 12}, "the word");

    const Value backwards =
        Json(Card(letter("A", 100, 4) + "," + letter("B", 110, 2) + "," +
                  letter("C", 120, 0)));
    Value reversed;
    if (Humanize(backwards, reversed, report)) {
        const Value* abc = At(reversed, {0});
        Check(abc != nullptr && abc->Find("content")->AsString() == "ABC" &&
                  abc->Find("stagger")->Dump() ==
                      R"({"each":2,"order":"reverse"})",
              "the last letter first: a reverse cascade");
        CheckDrawnAlike(backwards, reversed, {0, 1, 3, 5}, "the reverse word");
    }

    // Placed by other metrics, a third of a pixel off: kept apart.
    const Value off =
        Json(Card(letter("C", 100, 0) + "," + letter("O", 110.3, 2) + "," +
                  letter("U", 120.3, 4)));
    Value kept;
    if (Humanize(off, kept, report)) {
        Check(report.words == 1 && report.letters == 2 &&
                  At(kept, {0})->Find("type")->AsString() == "text" &&
                  At(kept, {0})->Find("content")->AsString() == "C" &&
                  At(kept, {1})->Find("content")->AsString() == "OU" &&
                  report.notes.size() == 1 &&
                  report.notes[0].find("0.300") != std::string::npos,
              "a letter a third of a pixel off stays apart; the two that "
              "agree fuse; the report says how far");
        CheckDrawnAlike(off, kept, {0, 2, 4, 6}, "letters kept apart");
    }
}

}  // namespace

int main() {
    TestTimecode();
    TestWrappers();
    TestLetters();
    if (failures != 0) {
        std::cerr << failures << " humanize assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion humanize\n";
    return 0;
}
