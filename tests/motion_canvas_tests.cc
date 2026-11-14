// REMOTION-PARITY-2026-09 REM5 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// The policy of the visual editor: what a click selects, through
// transforms, perspective, instances and clips; the operation a drag or a
// key commits, applied and undone by the edit log; the node landing where
// the gesture put it. Portable: no framework, fake metrics.

#include "Document.h"
#include "EditLog.h"
#include "Json.h"
#include "MotionCanvas.h"
#include "MotionComposition.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionGeometry.h"
#include "MotionOutline.h"
#include "Operations.h"
#include "motion_fake_measurer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Near(double left, double right, double tolerance = 1e-6) {
    return std::fabs(left - right) <= tolerance;
}

using mcp_json::Value;

Value Json(const std::string& text) {
    Value value;
    std::string error;
    Check(Value::Parse(text, value, error), "test JSON parses: " + error);
    return value;
}

std::string Read(const std::string& name) {
    std::ifstream input(std::string(MOTION_PARITY_FIXTURE_DIR) + "/" + name);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

MotionComposition Parse(const Value& definition) {
    MotionComposition composition;
    MotionError error;
    Check(
        ParseMotionComposition(definition, composition, error),
        "parses: " + error.code + " at " + error.path + " -- " + error.message);
    return composition;
}

bool Evaluate(const Value& definition, int64_t frame, MotionFrame& out,
              const MotionLibrary* library = nullptr) {
    FakeMotionMeasurer measurer;
    MotionError error;
    MotionEvaluationContext context;
    context.library = library;
    const bool ok = EvaluateMotionFrame(Parse(definition), Value(), frame,
                                        measurer, context, out, error);
    Check(ok, "evaluates frame " + std::to_string(frame) + ": " + error.code +
                  " at " + error.path + " -- " + error.message);
    return ok;
}

const MotionEvaluatedNode* FindUid(const MotionEvaluatedNode& node,
                                   const std::string& uid) {
    if (node.uid == uid) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = FindUid(child, uid)) {
            return found;
        }
    }
    return nullptr;
}

// The centre of a quad.
std::pair<double, double> Center(const std::pair<double, double> quad[4]) {
    return {
        (quad[0].first + quad[1].first + quad[2].first + quad[3].first) / 4.0,
        (quad[0].second + quad[1].second + quad[2].second + quad[3].second) /
            4.0};
}

// A document holding `definition` as a composition, for the edit log.
Document WithComposition(const Value& definition, const Ulid& id) {
    Document document;
    document.sequence.id = "01K7D000000000000000000001";
    document.compositions.push_back({id, "Canvas", definition});
    return document;
}

// A gesture committed as the interface commits it: one
// UpdateMotionComposition, applied then undone to the byte.
bool Commit(Document& document, const Ulid& id,
            const std::vector<MotionPropertyEdit>& edits,
            const std::string& label) {
    DocumentComposition* stored = document.FindComposition(id);
    Value definition;
    std::string error;
    if (!MotionDefinitionWithEdits(stored->definition, edits, definition,
                                   error)) {
        Check(false, label + ": " + error);
        return false;
    }
    DocumentComposition updated = *stored;
    updated.definition = definition;
    EditLog log;
    const std::string before = document.SaveToString();
    EditError failure = EditError::None;
    std::string message;
    Document undone = document;
    const bool applied = log.Apply(
        undone, UpdateMotionCompositionOperation{updated}, failure, message);
    Check(applied, label + " applies as one operation: " + message);
    if (!applied) {
        return false;
    }
    Document after = undone;
    Check(log.Undo(undone, failure, message) && undone.SaveToString() == before,
          label + ": one undo takes the whole gesture back");
    document = after;
    return true;
}

void TestHits() {
    const Value c01 = Json(Read("c01-centered-card.json"));
    MotionFrame frame;
    MotionHit hit;
    if (Evaluate(c01, 0, frame)) {
        Check(MotionHitTest(frame, 500, 950, true, hit) &&
                  hit.uid == "01K71000000000000000000002" &&
                  Near(hit.quad[0].first, 470) &&
                  Near(hit.quad[2].second, 1008),
              "a click on the title selects it, its box as the quad");
        Check(
            MotionHitTest(frame, 10, 10, true, hit) &&
                hit.uid == "01K71000000000000000000001",
            "a click on the page selects the page, which paints a background");
    }
    // Through a perspective: inside the projected card, and inside where
    // the card would be unturned but outside its projection.
    const Value c19 = Json(Read("c19-perspective-card.json"));
    if (Evaluate(c19, 20, frame)) {
        const MotionEvaluatedNode* card =
            FindUid(frame.root, "01K7C000000000000000001902");
        const auto& q = card->quad;
        const double leftX = q[0].first + (q[3].first - q[0].first) * 0.5;
        const double leftY = q[0].second + (q[3].second - q[0].second) * 0.5;
        const double rightX = q[1].first + (q[2].first - q[1].first) * 0.5;
        const double rightY = q[1].second + (q[2].second - q[1].second) * 0.5;
        Check(MotionHitTest(frame, leftX + (rightX - leftX) * 0.05,
                            leftY + (rightY - leftY) * 0.05, true, hit) &&
                  hit.uid == "01K7C000000000000000001902",
              "a click inside the turned card selects it");
        Check(!MotionHitTest(frame, 162, 180, true, hit),
              "a click where the card would be unturned, outside its "
              "projection, selects nothing");
    }
    // Into an instance, or onto it.
    MotionLibrary library;
    library.compositions.emplace("01K7C000000000000000001400",
                                 Parse(Json(Read("c14-lower-third.json"))));
    const Value c14 = Json(Read("c14-precomposition.json"));
    if (Evaluate(c14, 17, frame, &library)) {
        Check(MotionHitTest(frame, 250, 85, true, hit) &&
                  hit.uid == "01K7C000000000000000001402" &&
                  hit.instances ==
                      std::vector<std::string>{"01K7C000000000000000001412"},
              "a click on the left band selects the band inside the left "
              "instance");
        Check(MotionHitTest(frame, 250, 85, false, hit) &&
                  hit.uid == "01K7C000000000000000001412" &&
                  hit.instances.empty(),
              "without entering instances, the same click selects the "
              "instance");
    }
    // What a clip hides is not selected.
    const Value c18 = Json(Read("c18-circle-polygon-mask.json"));
    if (Evaluate(c18, 0, frame)) {
        Check(!MotionHitTest(frame, 550, 225, true, hit),
              "a picture hidden by a closed circle is not selected");
    }
    if (Evaluate(c18, 20, frame)) {
        Check(MotionHitTest(frame, 550, 225, true, hit) &&
                  hit.uid == "01K7C000000000000000001803",
              "the same picture, uncovered, is");
    }
}

void TestKeys() {
    Value animation;
    std::string error;
    Check(
        MotionAnimationWithKey(nullptr, 10, Json("7"), animation, error) &&
            animation.Find("interpolate")->Find("frames")->Dump() == "[10]" &&
            animation.Find("interpolate")->Find("values")->Dump() == "[7]" &&
            animation.Find("interpolate")->Find("uids")->AsArray().size() == 1,
        "a first key makes an animation of one key");
    const std::string first =
        animation.Find("interpolate")->Find("uids")->AsArray()[0].AsString();
    Value two;
    Check(MotionAnimationWithKey(&animation, 5, Json("3"), two, error) &&
              two.Find("interpolate")->Find("frames")->Dump() == "[5,10]" &&
              two.Find("interpolate")->Find("values")->Dump() == "[3,7]" &&
              two.Find("interpolate")->Find("uids")->AsArray()[1].AsString() ==
                  first,
          "a key before it is added in order, the first keeping its uid");
    Value moved;
    Check(
        MotionAnimationWithKey(&two, 10, Json("8"), moved, error) &&
            moved.Find("interpolate")->Find("values")->Dump() == "[3,8]" &&
            moved.Find("interpolate")->Find("uids")->AsArray()[1].AsString() ==
                first,
        "a key at a keyed frame moves that key");
    std::optional<Value> fewer;
    Check(MotionAnimationWithoutKey(moved, 5, fewer, error) && fewer &&
              fewer->Find("interpolate")->Find("frames")->Dump() == "[10]",
          "a key is removed");
    std::optional<Value> none;
    if (fewer) {
        Check(MotionAnimationWithoutKey(*fewer, 10, none, error) && !none,
              "removing the last key removes the animation");
        Check(!MotionAnimationWithoutKey(*fewer, 99, none, error),
              "a frame without a key is refused");
    }
    // MOTION-WEB-2026-10 MW1 -- a key between two frames is placed by its
    // exact position, and never taken for frame 0.
    const Value between =
        Json(R"({"interpolate":{"frames":[0,"5/2",10],"values":[0,5,9]}})");
    Value inserted;
    Check(MotionAnimationWithKey(&between, 2, Json("4"), inserted, error) &&
              inserted.Find("interpolate")->Find("frames")->Dump() ==
                  R"([0,2,"5/2",10])",
          "a key at frame 2 goes before the key at 5/2");
    Value after;
    Check(MotionAnimationWithKey(&between, 3, Json("6"), after, error) &&
              after.Find("interpolate")->Find("frames")->Dump() ==
                  R"([0,"5/2",3,10])",
          "a key at frame 3 goes after it");
    std::optional<Value> withoutZero;
    Check(MotionAnimationWithoutKey(between, 0, withoutZero, error) &&
              withoutZero &&
              withoutZero->Find("interpolate")->Find("frames")->Dump() ==
                  R"(["5/2",10])",
          "removing the key at frame 0 keeps the key at 5/2");
    std::optional<Value> missing;
    Check(!MotionAnimationWithoutKey(between, 2, missing, error),
          "no key sits at frame 2: 5/2 is not a frame");
    Value eased;
    Check(MotionAnimationWithEasing(two, "ease-in-out", eased, error) &&
              eased.Find("interpolate")->Find("easing")->AsString() ==
                  "ease-in-out",
          "an easing is set on every segment");
    const Value spring = Json(R"({"spring":{"from":0,"to":1}})");
    Check(!MotionAnimationWithKey(&spring, 3, Json("1"), animation, error) &&
              !MotionAnimationWithEasing(spring, "linear", eased, error),
          "a spring has neither keys nor easing");

    // MOTION-BIBLIOTHEQUE-2026-10 MB5 -- one curve per segment survives
    // every rewriting of the keys.
    const auto curves = [](const Value& from) {
        const Value* easing = from.Find("interpolate")->Find("easing");
        return easing != nullptr ? easing->Dump() : std::string("none");
    };
    const Value listed =
        Json(R"({"interpolate":{"frames":[0,10,20],"values":[0,1,0],)"
             R"("easing":["ease-out","ease-in"]}})");
    Value cut;
    Check(MotionAnimationWithKey(&listed, 15, Json("0.5"), cut, error) &&
              curves(cut) == R"(["ease-out","ease-in","ease-in"])",
          "a key inside a segment cuts it in two halves of its curve: " +
              curves(cut));
    Check(MotionAnimationWithKey(&listed, 5, Json("0.5"), cut, error) &&
              curves(cut) == R"(["ease-out","ease-out","ease-in"])",
          "the first segment too: " + curves(cut));
    Check(MotionAnimationWithKey(&listed, 30, Json("1"), cut, error) &&
              curves(cut) == R"(["ease-out","ease-in","ease-in"])" &&
              cut.Find("interpolate")->Find("frames")->Dump() == "[0,10,20,30]",
          "a key after the last one adds a segment shaped as the one before: " +
              curves(cut));
    Check(MotionAnimationWithKey(&listed, 10, Json("2"), cut, error) &&
              curves(cut) == R"(["ease-out","ease-in"])",
          "a key moved at its frame leaves the curves alone");
    const Value alone =
        Json(R"({"interpolate":{"frames":[4],"values":[1],"easing":[]}})");
    Check(MotionAnimationWithKey(&alone, 9, Json("2"), cut, error) &&
              curves(cut) == R"(["linear"])",
          "the first segment of a list is linear: " + curves(cut));
    std::optional<Value> merged;
    Check(MotionAnimationWithoutKey(listed, 10, merged, error) && merged &&
              curves(*merged) == R"(["ease-out"])",
          "without a key, the merged segment keeps the curve that left the "
          "key before it");
    Check(MotionAnimationWithoutKey(listed, 0, merged, error) && merged &&
              curves(*merged) == R"(["ease-in"])" &&
              MotionAnimationWithoutKey(listed, 20, merged, error) && merged &&
              curves(*merged) == R"(["ease-out"])",
          "without an outer key, its segment goes with it");
    Check(MotionAnimationWithEasing(two, "ease-in", eased, error, 5) &&
              curves(eased) == R"("ease-in")",
          "one segment eased, and no other: the name alone");
    const Value three =
        Json(R"({"interpolate":{"frames":[0,10,20],"values":[0,1,0],)"
             R"("easing":"ease-out"}})");
    Check(MotionAnimationWithEasing(three, "ease-in", eased, error, 10) &&
              curves(eased) == R"(["ease-out","ease-in"])",
          "one segment eased among others makes a list: " + curves(eased));
    Check(MotionAnimationWithEasing(eased, "ease-in", eased, error, 0) &&
              curves(eased) == R"("ease-in")",
          "a list of one curve is written as its name: " + curves(eased));
    Check(!MotionAnimationWithEasing(three, "ease-in", eased, error, 20) &&
              error.find("last one") != std::string::npos &&
              !MotionAnimationWithEasing(three, "ease-in", eased, error, 7) &&
              error.find("no key at frame 7") != std::string::npos,
          "no segment leaves the last key, nor a frame without a key");
    Check(MotionAnimationWithEasing(listed, "linear", eased, error) &&
              curves(eased) == R"("linear")",
          "an easing without a frame replaces the whole list");

    // A key asked for on a static property: one key holds its value.
    const Ulid id = "01K7D000000000000000000010";
    Document document =
        WithComposition(Json(Read("c01-centered-card.json")), id);
    if (Commit(
            document, id,
            {{"01K71000000000000000000002", "opacity", Json("0.5"), true, 12}},
            "a key on the title's opacity")) {
        MotionFrame at0;
        MotionFrame at40;
        const Value& definition = document.compositions[0].definition;
        if (Evaluate(definition, 0, at0) && Evaluate(definition, 40, at40)) {
            Check(Near(FindUid(at0.root, "01K71000000000000000000002")->opacity,
                       0.5) &&
                      Near(FindUid(at40.root, "01K71000000000000000000002")
                               ->opacity,
                           0.5),
                  "one key holds its value before and after it");
        }
    }
}

void TestMoves() {
    const Ulid id = "01K7D000000000000000000010";
    Document document =
        WithComposition(Json(Read("c01-centered-card.json")), id);
    MotionFrame before;
    std::vector<MotionPropertyEdit> edits;
    std::string error;
    if (!Evaluate(document.compositions[0].definition, 0, before) ||
        !MotionMoveEdits(before, document.compositions[0].definition,
                         "01K71000000000000000000002", 0, 10, -5, edits,
                         error) ||
        !Commit(document, id, edits, "a drag of the title")) {
        Check(false, "the title is dragged: " + error);
        return;
    }
    MotionFrame after;
    if (Evaluate(document.compositions[0].definition, 0, after)) {
        std::pair<double, double> from[4];
        std::pair<double, double> to[4];
        MotionNodeQuad(before, "01K71000000000000000000002", from);
        MotionNodeQuad(after, "01K71000000000000000000002", to);
        Check(
            Near(to[0].first - from[0].first, 10) &&
                Near(to[0].second - from[0].second, -5) &&
                document.compositions[0].definition.Dump().find(
                    "\"translateX\":10,\"translateY\":-5") != std::string::npos,
            "the dragged title lands where the drag put it, set as a "
            "style: " +
                document.compositions[0].definition.Dump());
    }

    // Inside a parent turned a quarter: the drag is turned back into the
    // parent's space, and the child lands under the mouse all the same.
    const Value turned = Json(
        R"({"width":400,"height":400,"fps":25,"duration":10,"root":{"type":"fill",)"
        R"("uid":"01K7D000000000000000000021","style":{"justifyContent":"center",)"
        R"("alignItems":"center"},"children":[{"type":"box","uid":"01K7D000000000000000000022",)"
        R"("style":{"width":200,"height":100,"rotate":90,"scale":2},"children":[)"
        R"({"type":"box","uid":"01K7D000000000000000000023","style":{"width":20,)"
        R"("height":20,"backgroundColor":"#fff"}}]}]}})");
    Document rotated = WithComposition(turned, id);
    edits.clear();
    if (Evaluate(turned, 0, before) &&
        MotionMoveEdits(before, turned, "01K7D000000000000000000023", 0, 10, 0,
                        edits, error) &&
        Commit(rotated, id, edits, "a drag inside a turned parent") &&
        Evaluate(rotated.compositions[0].definition, 0, after)) {
        std::pair<double, double> from[4];
        std::pair<double, double> to[4];
        MotionNodeQuad(before, "01K7D000000000000000000023", from);
        MotionNodeQuad(after, "01K7D000000000000000000023", to);
        const auto a = Center(from);
        const auto b = Center(to);
        const MotionEvaluatedNode* child =
            FindUid(after.root, "01K7D000000000000000000023");
        Check(Near(b.first - a.first, 10, 1e-6) &&
                  Near(b.second - a.second, 0, 1e-6) &&
                  Near(child->translate_x, 0, 1e-6) &&
                  Near(child->translate_y, -5, 1e-6),
              "a drag of (10, 0) under a parent turned 90 degrees and doubled "
              "is (0, -5) in its space, and lands under the mouse");
    }

    // Under a perspective: the node's centre goes where the mouse went.
    const Value c19 = Json(Read("c19-perspective-card.json"));
    Document perspective = WithComposition(c19, id);
    edits.clear();
    if (Evaluate(c19, 20, before) &&
        MotionMoveEdits(before, c19, "01K7C000000000000000001905", 20, 12, 7,
                        edits, error) &&
        Commit(perspective, id, edits, "a drag under a perspective") &&
        Evaluate(perspective.compositions[0].definition, 20, after)) {
        const auto centerOf = [](const MotionFrame& frame, const Ulid& uid) {
            MotionHomography parent;
            const MotionEvaluatedNode* card =
                FindUid(frame.root, "01K7C000000000000000001902");
            const MotionEvaluatedNode* label = FindUid(frame.root, uid);
            const MotionHomography mapping =
                MotionNodeMapping(frame.root).After(MotionNodeMapping(*card));
            double x = 0.0;
            double y = 0.0;
            mapping.Apply(
                label->box.x + label->box.width / 2 + label->translate_x,
                label->box.y + label->box.height / 2 + label->translate_y, x,
                y);
            return std::make_pair(x, y);
        };
        const auto a = centerOf(before, "01K7C000000000000000001905");
        const auto b = centerOf(after, "01K7C000000000000000001905");
        Check(Near(b.first - a.first, 12, 1e-3) &&
                  Near(b.second - a.second, 7, 1e-3),
              "a drag of the label on the turned card moves its centre by "
              "the drag, through the projection");
    }

    // A property moved by keys takes a key at the frame shown.
    const Value keyed = Json(
        R"({"width":100,"height":100,"fps":25,"duration":30,"root":{"type":"fill",)"
        R"("uid":"01K7D000000000000000000031","children":[{"type":"box",)"
        R"("uid":"01K7D000000000000000000032","style":{"width":10,"height":10,)"
        R"("backgroundColor":"#fff"},"animate":{"translateX":{"interpolate":{)"
        R"("frames":[0,20],"values":[0,40],"uids":["01K7D000000000000000000033",)"
        R"("01K7D000000000000000000034"]}}}}]}})");
    Document animated = WithComposition(keyed, id);
    edits.clear();
    if (Evaluate(keyed, 10, before) &&
        MotionMoveEdits(before, keyed, "01K7D000000000000000000032", 10, 5, 0,
                        edits, error) &&
        Commit(animated, id, edits, "a drag of a keyed node")) {
        const std::string dump = animated.compositions[0].definition.Dump();
        Check(dump.find("\"frames\":[0,10,20],\"values\":[0,25,40]") !=
                  std::string::npos,
              "the drag at frame 10 adds a key there, at 20 + 5: " + dump);
    }
    const Value springy = Json(
        R"({"width":100,"height":100,"fps":25,"duration":30,"root":{"type":"fill",)"
        R"("uid":"01K7D000000000000000000041","children":[{"type":"box",)"
        R"("uid":"01K7D000000000000000000042","style":{"width":10,"height":10},)"
        R"("animate":{"translateX":{"spring":{"from":-40,"to":0}}}}]}})");
    edits.clear();
    Check(Evaluate(springy, 5, before) &&
              !MotionMoveEdits(before, springy, "01K7D000000000000000000042", 5,
                               5, 0, edits, error) &&
              error.find("spring") != std::string::npos,
          "a node moved by a spring is not dragged: its spring is edited");
}

void TestResize() {
    const Ulid id = "01K7D000000000000000000010";
    Document document =
        WithComposition(Json(Read("c01-centered-card.json")), id);
    MotionFrame before;
    std::vector<MotionPropertyEdit> edits;
    std::string error;
    if (!Evaluate(document.compositions[0].definition, 0, before) ||
        !MotionResizeEdits(before, document.compositions[0].definition,
                           "01K71000000000000000000002", 0, 2, 20, 10, edits,
                           error) ||
        !Commit(document, id, edits, "a resize of the title")) {
        Check(false, "the title is resized: " + error);
        return;
    }
    MotionFrame after;
    if (Evaluate(document.compositions[0].definition, 0, after)) {
        const MotionEvaluatedNode* title =
            FindUid(after.root, "01K71000000000000000000002");
        Check(Near(title->box.width, 160) && Near(title->box.height, 106),
              "the bottom-right handle grows the box by the drag");
    }
    std::pair<double, double> quad[4];
    MotionNodeQuad(before, "01K71000000000000000000002", quad);
    const auto handles = MotionSelectionHandles(quad);
    Check(handles.size() == 8 && Near(handles[4].first, 540) &&
              Near(handles[4].second, 912) && Near(handles[5].first, 610),
          "the handles are the corners, then the middles of the edges");
}

// The uid the edit log gave the node at `path` of the stored definition.
std::string UidAt(const Document& document, const MotionTreePath& path) {
    const Value* node =
        MotionTreeAt(document.compositions.front().definition, path);
    return node != nullptr && node->Find("uid") != nullptr
               ? node->Find("uid")->AsString()
               : std::string();
}

MotionTreePath Child(MotionTreePath path, size_t index) {
    path.push_back({"children", 0, false});
    path.push_back({std::string(), index, true});
    return path;
}

// One editor action: a definition rewritten by a helper, committed as the
// interface commits it.
bool Act(Document& document, const Ulid& id,
         const std::function<bool(const Value&, Value&, std::string&)>& action,
         const std::string& label) {
    DocumentComposition updated = *document.FindComposition(id);
    Value definition;
    std::string error;
    if (!action(updated.definition, definition, error)) {
        Check(false, label + ": " + error);
        return false;
    }
    updated.definition = definition;
    EditLog log;
    EditError failure = EditError::None;
    std::string message;
    const bool ok = log.Apply(
        document, UpdateMotionCompositionOperation{updated}, failure, message);
    Check(ok, label + " applies: " + message);
    return ok;
}

// REM5's condition: C02, then C15, built without writing any JSON -- each
// step one of the helpers the interface calls, committed as it commits
// them -- and shown as the corpus scenes show them.
void TestScenesWithoutJson() {
    const Ulid id = "01K7D000000000000000000050";
    Document document;
    document.sequence.id = "01K7D000000000000000000001";
    AddMotionCompositionOperation add;
    add.composition = {id, "Crédit",
                       MotionNewComposition(1080, 1920, 25, 1, 75, false)};
    EditLog log;
    EditError failure = EditError::None;
    std::string message;
    Check(log.Apply(document, add, failure, message),
          "a new composition is added: " + message);
    const MotionTreePath rootPath = {{"root", 0, false}};
    const std::string root = UidAt(document, rootPath);
    Act(
        document, id,
        [&](const Value& in, Value& out, std::string& error) {
            return MotionDefinitionWithEdits(
                in,
                {{root, "alignItems", Value::MakeString("flex-start"), false,
                  0},
                 {root, "padding", Value::MakeString("0px 0px 0px 80px"), false,
                  0}},
                out, error);
        },
        "the page is laid out to the left");
    Act(
        document, id,
        [&](const Value& in, Value& out, std::string& error) {
            return MotionDefinitionWithChild(in, root, "", MotionNewNode("box"),
                                             out, error);
        },
        "a box is added");
    const std::string credit = UidAt(document, Child(rootPath, 0));
    Act(
        document, id,
        [&](const Value& in, Value& out, std::string& error) {
            return MotionDefinitionWithEdits(
                in,
                {{credit, "width", Value(), false, 0},
                 {credit, "height", Value(), false, 0},
                 {credit, "backgroundColor", Value(), false, 0},
                 {credit, "flexDirection", Value::MakeString("column"), false,
                  0},
                 {credit, "gap", Value::MakeInt(12), false, 0}},
                out, error);
        },
        "the box becomes a column");
    const struct Line {
        const char* content;
        int size;
        const char* color;
        int64_t delay;
    } lines[] = {{"Nora Bell", 60, "#ffffff", 0},
                 {"Monteuse", 36, "#4daba1", 6}};
    for (size_t index = 0; index < 2; ++index) {
        const Line& line = lines[index];
        Act(
            document, id,
            [&](const Value& in, Value& out, std::string& error) {
                return MotionDefinitionWithChild(
                    in, credit, "", MotionNewNode("text"), out, error);
            },
            std::string("a text is added: ") + line.content);
        const std::string text =
            UidAt(document, Child(Child(rootPath, 0), index));
        Act(
            document, id,
            [&](const Value& in, Value& out, std::string& error) {
                Value named;
                return MotionDefinitionWithNodeField(
                           in, text, "content", Value::MakeString(line.content),
                           named, error) &&
                       MotionDefinitionWithEdits(
                           named,
                           {{text, "fontSize", Value::MakeInt(line.size), false,
                             0},
                            {text, "color", Value::MakeString(line.color),
                             false, 0}},
                           out, error);
            },
            std::string("its text and look: ") + line.content);
        Act(
            document, id,
            [&](const Value& in, Value& out, std::string& error) {
                return MotionDefinitionWithSpring(in, text, "translateX", true,
                                                  -80, 0, line.delay, 14, out,
                                                  error);
            },
            "its spring");
        Act(
            document, id,
            [&](const Value& in, Value& out, std::string& error) {
                return MotionDefinitionWithEdits(
                    in,
                    {{text, "opacity", Value::MakeInt(0), true, line.delay},
                     {text, "opacity", Value::MakeInt(1), true,
                      line.delay + 6}},
                    out, error);
            },
            "its fade, two keys");
    }
    // Shown as C02 shows it: the same values at the same instants.
    const Value built = document.compositions.front().definition;
    const Value fixture = Json(Read("c02-staggered-credit.json"));
    for (const int64_t frame : {0, 3, 4, 9, 12, 30}) {
        MotionFrame ours;
        MotionFrame theirs;
        if (!Evaluate(built, frame, ours) ||
            !Evaluate(fixture, frame, theirs)) {
            continue;
        }
        const MotionEvaluatedNode& ourCredit = ours.root.children[0];
        const MotionEvaluatedNode& theirCredit = theirs.root.children[0];
        bool same = ourCredit.children.size() == 2;
        for (size_t index = 0; same && index < 2; ++index) {
            const MotionEvaluatedNode& a = ourCredit.children[index];
            const MotionEvaluatedNode& b = theirCredit.children[index];
            same = a.text == b.text && Near(a.opacity, b.opacity) &&
                   Near(a.translate_x, b.translate_x) &&
                   Near(a.box.x, b.box.x, 1e-3) &&
                   Near(a.box.y, b.box.y, 1e-3) &&
                   Near(a.text_style.font_size, b.text_style.font_size) &&
                   Near(a.text_style.color.green, b.text_style.color.green);
        }
        Check(same,
              "C02 built by the editor shows what the fixture shows at "
              "frame " +
                  std::to_string(frame) + ": " +
                  DumpMotionFrame(ours).substr(0, 600));
    }

    // C15: scenes and transitions.
    const Ulid scenesId = "01K7D000000000000000000060";
    Document scenes;
    scenes.sequence.id = "01K7D000000000000000000001";
    AddMotionCompositionOperation addScenes;
    addScenes.composition = {scenesId, "Scènes",
                             MotionNewComposition(320, 180, 25, 1, 70, true)};
    Check(log.Apply(scenes, addScenes, failure, message),
          "a composition of scenes is added: " + message);
    const std::string series = UidAt(scenes, rootPath);
    for (int scene = 0; scene < 2; ++scene) {
        Act(
            scenes, scenesId,
            [&](const Value& in, Value& out, std::string& error) {
                return MotionDefinitionWithScene(
                    in, series, MotionNewNode("sequence"), 30, out, error);
            },
            "a scene is added");
    }
    const char* colors[] = {"#E4572E", "#3D5AFE", "#4DABA1"};
    std::string uids[3];
    for (size_t index = 0; index < 3; ++index) {
        uids[index] = UidAt(scenes, Child(rootPath, index));
        Act(
            scenes, scenesId,
            [&](const Value& in, Value& out, std::string& error) {
                return MotionDefinitionWithEdits(
                    in,
                    {{uids[index], "backgroundColor",
                      Value::MakeString(colors[index]), false, 0}},
                    out, error);
            },
            "a scene is coloured");
    }
    Act(
        scenes, scenesId,
        [&](const Value& in, Value& out, std::string& error) {
            return MotionDefinitionWithChild(
                in, series, uids[0],
                MotionNewTransition("fade", "", 10, "linear"), out, error);
        },
        "a fade joins the first two scenes");
    Act(
        scenes, scenesId,
        [&](const Value& in, Value& out, std::string& error) {
            return MotionDefinitionWithChild(
                in, series, uids[1],
                MotionNewTransition("slide", "from-right", 10, "ease-in-out"),
                out, error);
        },
        "a slide joins the last two");
    const Value builtScenes = scenes.compositions.front().definition;
    const Value c15 = Json(Read("c15-fade-slide.json"));
    for (const int64_t frame : {19, 25, 29, 30, 45, 69}) {
        MotionFrame ours;
        MotionFrame theirs;
        if (!Evaluate(builtScenes, frame, ours) ||
            !Evaluate(c15, frame, theirs)) {
            continue;
        }
        bool same =
            ours.root.transition.active == theirs.root.transition.active &&
            Near(ours.root.transition.progress,
                 theirs.root.transition.progress) &&
            ours.root.children.size() == theirs.root.children.size();
        for (size_t index = 0; same && index < ours.root.children.size();
             ++index) {
            const MotionEvaluatedNode& a = ours.root.children[index];
            const MotionEvaluatedNode& b = theirs.root.children[index];
            same = Near(a.opacity, b.opacity) &&
                   Near(a.translate_x, b.translate_x) &&
                   a.transition_role == b.transition_role &&
                   a.blend_plus_lighter == b.blend_plus_lighter &&
                   Near(a.background.red, b.background.red);
        }
        Check(same,
              "C15 built by the editor runs its transitions as the "
              "fixture does at frame " +
                  std::to_string(frame));
    }
}

// A composition of 1080 x 1920 fitted into a 1920 x 1080 sequence, shown
// in a monitor at half size: the composition's centre lands in the
// monitor's, and a point comes back where it went.
void TestMonitorMapping() {
    ClipFramingGeometry framing;
    std::string error;
    Check(ResolveClipFraming(ClipFraming{}, 1080, 1920, 1920, 1080, framing,
                             error),
          "the default framing resolves: " + error);
    const MotionMonitorMapping mapping =
        MotionMonitorMappingFor(framing, 10.0, 20.0, 0.5);
    double x = 0.0;
    double y = 0.0;
    mapping.ToView(540.0, 960.0, x, y);
    Check(
        Near(x, 10.0 + 960.0 * 0.5, 1e-9) && Near(y, 20.0 + 540.0 * 0.5, 1e-9),
        "the composition's centre is the monitor's canvas centre: " +
            std::to_string(x) + ", " + std::to_string(y));
    double back = 0.0;
    double up = 0.0;
    mapping.FromView(x + 7.0, y - 3.0, back, up);
    Check(Near(back, 540.0 + 7.0 / mapping.scale_x, 1e-9) &&
              Near(up, 960.0 - 3.0 / mapping.scale_y, 1e-9),
          "a point of the monitor comes back to the composition");
}

}  // namespace

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. A word's cascade set from
// the Inspector's rows alone: cut, delay, order and its seed, and the curve
// every unit animation shares -- each a row typed as the interface types
// it, committed as one operation, and read back by the engine.
void TestCascadeInInspector() {
    const Ulid id = "01K7D000000000000000000060";
    Document document = WithComposition(
        Json(R"J({"width": 400, "height": 200, "fps": 25, "duration": 50,
          "root": {"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20},
            "children": [{"type": "text", "content": "abcd",
              "animate": {
                "opacity": {"interpolate": {"frames": [0, 4], "values": [0, 1]}},
                "translateY": {"interpolate": {"frames": [0, 4], "values": [10, 0],
                                               "easing": "ease-in"}}}}]}})J"),
        id);
    Act(
        document, id,
        [](const Value& in, Value& out, std::string& error) {
            out = in;
            (void)error;
            return true;
        },
        "the composition is given its identities");
    const MotionTreePath text = Child({{"root", 0, false}}, 0);
    const std::string uid = UidAt(document, text);
    const auto rows = [&](std::vector<MotionField>& fields) {
        MotionFrame frame;
        std::string error;
        return Evaluate(document.compositions.front().definition, 0, frame) &&
               MotionNodeFields(document.compositions.front().definition, frame,
                                uid, 0, fields, error);
    };
    const auto row = [](const std::vector<MotionField>& fields,
                        const std::string& property) -> const MotionField* {
        for (const MotionField& field : fields) {
            if (field.property == property) {
                return &field;
            }
        }
        return nullptr;
    };
    const auto type = [&](const std::string& property,
                          const std::string& typed) {
        std::vector<MotionField> fields;
        const MotionField* field =
            rows(fields) ? row(fields, property) : nullptr;
        if (field == nullptr) {
            Check(false, "the row " + property + " is shown");
            return false;
        }
        return Act(
            document, id,
            [&](const Value& in, Value& out, std::string& error) {
                return MotionDefinitionWithTypedField(in, uid, *field, typed, 0,
                                                      out, error);
            },
            property + " set to '" + typed + "'");
    };
    const auto delays = [&]() {
        std::vector<int64_t> found;
        MotionFrame frame;
        if (Evaluate(document.compositions.front().definition, 0, frame)) {
            const MotionEvaluatedNode* node = FindUid(frame.root, uid);
            for (const MotionTextUnitState& unit : node->units) {
                found.push_back(unit.delay);
            }
        }
        return found;
    };

    std::vector<MotionField> fields;
    Check(rows(fields) && row(fields, "units") != nullptr &&
              row(fields, "units")->text == "none" &&
              row(fields, "stagger.each") == nullptr,
          "a text not cut shows its cut, and no cascade yet");
    type("units", "char");
    fields.clear();
    Check(rows(fields) && row(fields, "stagger.each") != nullptr &&
              row(fields, "stagger.each")->text == "0" &&
              row(fields, "stagger.order")->text == "forward" &&
              row(fields, "stagger.seed") == nullptr &&
              row(fields, "animate.easing")->text == "mixte",
          "cut into letters, it shows a delay of none, the forward order, and "
          "the curves its animations do not share");
    type("stagger.each", "3");
    Check(delays() == std::vector<int64_t>({0, 3, 6, 9}),
          "a delay of three frames cascades the letters");
    type("stagger.order", "reverse");
    Check(delays() == std::vector<int64_t>({9, 6, 3, 0}),
          "the reverse order starts from the last letter");
    type("stagger.order", "random");
    fields.clear();
    Check(rows(fields) && row(fields, "stagger.seed") != nullptr &&
              row(fields, "stagger.seed")->text == "0",
          "a random order is given a seed, which shows");
    std::vector<int64_t> drawn = delays();
    type("stagger.seed", "7");
    std::sort(drawn.begin(), drawn.end());
    std::vector<int64_t> reseeded = delays();
    std::sort(reseeded.begin(), reseeded.end());
    Check(drawn == std::vector<int64_t>({0, 3, 6, 9}) && reseeded == drawn,
          "a seed draws a permutation of the same delays");
    type("stagger.order", "forward");
    const Value* stagger = MotionTreeAt(
        document.compositions.front().definition, {text.begin(), text.end()});
    stagger = stagger != nullptr ? stagger->Find("stagger") : nullptr;
    Check(stagger != nullptr && stagger->Find("seed") == nullptr &&
              stagger->Find("order") == nullptr &&
              delays() == std::vector<int64_t>({0, 3, 6, 9}),
          "back to forward, the seed goes with the random order");
    type("animate.easing", "ease-out");
    fields.clear();
    Check(rows(fields) && row(fields, "animate.easing")->text == "ease-out",
          "one curve for every animation of the letters");

    std::string error;
    Value refused;
    fields.clear();
    rows(fields);
    Check(!MotionDefinitionWithTypedField(
              document.compositions.front().definition, uid,
              *row(fields, "stagger.each"), "2.5", 0, refused, error),
          "a delay is a whole number of frames");
    Check(!MotionDefinitionWithTypedField(
              document.compositions.front().definition, uid,
              *row(fields, "animate.easing"), "mixte", 0, refused, error),
          "\"mixte\" is not a curve");

    type("units", "none");
    fields.clear();
    const Value* uncut = MotionTreeAt(document.compositions.front().definition,
                                      {text.begin(), text.end()});
    Check(rows(fields) && row(fields, "stagger.each") == nullptr &&
              uncut->Find("units") == nullptr &&
              uncut->Find("stagger") == nullptr,
          "no cut, no cascade: the stagger goes with the units");
}

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. The composition panel's
// rows: the scenes of the film numbered and named as After names their
// precompositions -- the author's id before '#', else the biggest text --
// and every layer named by what it shows.
void TestOutline() {
    const Value definition = Json(R"J({"width": 400, "height": 200,
      "fps": 25, "duration": 40,
      "root": {"type": "fill", "style": {"fontFamily": "Fake", "fontSize": 20},
        "children": [
          {"type": "sequence", "id": "titre#1", "from": 0, "duration": 10,
           "children": [{"type": "text", "content": "TITRE"}]},
          {"type": "sequence", "from": 10, "duration": 20, "children": [
            {"type": "box", "children": [
              {"type": "text", "content": "petit"},
              {"type": "text", "content": "GRAND", "style": {"fontSize": 60}}]},
            {"type": "sequence", "from": 5, "children": [
              {"type": "text", "content": "dedans"}]}]},
          {"type": "box", "id": "fond#a", "style": {"position": "absolute",
           "width": 10, "height": 10, "backgroundColor": "#ff0000"}},
          {"type": "text", "content": {"timecode": "composition"}},
          {"type": "text", "content": "Une ligne bien trop longue pour tenir dans un calque"}]}})J");
    MotionComposition composition = Parse(definition);
    FakeMotionMeasurer measurer;
    std::vector<MotionOutlineRow> rows;
    MotionError error;
    if (!MotionOutlineRows(composition, Value(), measurer, {}, rows, error)) {
        Check(false, "the outline is built: " + error.message);
        return;
    }
    std::vector<std::string> names;
    names.reserve(rows.size());
    for (const MotionOutlineRow& row : rows) {
        names.push_back(std::string(static_cast<size_t>(row.depth) * 2, ' ') +
                        row.name);
    }
    const std::vector<std::string> expected = {
        "01 titre",
        "  TITRE",
        "02 GRAND",
        "  GRAND",
        "    petit",
        "    GRAND",
        "  dedans",
        "    dedans",
        "fond",
        "timecode",
        "Une ligne bien trop longue pour …"};
    std::string read;
    for (const std::string& name : names) {
        read += "\n" + name;
    }
    Check(names == expected, "the panel reads as After would:" + read);
    Check(rows.size() == expected.size() && rows[0].scene &&
              rows[0].start == 0 && rows[0].duration == 10 && rows[2].scene &&
              rows[2].start == 10 && rows[2].duration == 20 && !rows[6].scene &&
              rows[6].kind == "Scène" && rows[1].kind == "Texte" &&
              rows[8].kind == "Boîte",
          "scenes of the film carry their frames; one inside a scene is not "
          "numbered");

    // Opened along the selection: the scenes, and under each ancestor of
    // "petit" (scene two, its box) its children.
    std::vector<MotionOutlineRow> named = rows;
    for (size_t index = 0; index < named.size(); ++index) {
        named[index].uid = "n" + std::to_string(index);
    }
    Check(
        MotionOutlineShown(named, "") == std::vector<size_t>({0, 2, 8, 9, 10}),
        "nothing selected: the scenes and the top-level layers");
    Check(MotionOutlineShown(named, "n4") ==
              std::vector<size_t>({0, 2, 3, 4, 5, 6, 8, 9, 10}),
          "\"petit\" selected: its scene and its box opened");
}

int main() {
    TestHits();
    TestKeys();
    TestMoves();
    TestResize();
    TestScenesWithoutJson();
    TestCascadeInInspector();
    TestOutline();
    TestMonitorMapping();
    if (failures != 0) {
        std::cerr << failures << " motion canvas assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion canvas\n";
    return 0;
}
