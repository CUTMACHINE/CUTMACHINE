// MOTION-2026-09 M3 -- ROADMAP.md. Compositions in the document: the
// operations that edit them, their identities, their refusals, the version
// they need and the journal that replays them. Portable: no framework, no
// file, no text measured.

#include "Document.h"
#include "DocumentDelta.h"
#include "EditLog.h"
#include "Json.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectDelta.h"

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using mcp_json::Value;

Value Json(const std::string& text) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error)) {
        throw std::runtime_error("test JSON: " + error + "\n" + text);
    }
    return value;
}

const Ulid kSequence = "01K70000000000000000000001";
const Ulid kSource = "01K70000000000000000000002";
const Ulid kTrack = "01K70000000000000000000003";
const Ulid kClip = "01K70000000000000000000004";
const Ulid kComposition = "01K70000000000000000000010";
const Ulid kRoot = "01K70000000000000000000011";
const Ulid kBlock = "01K70000000000000000000012";
const Ulid kTitle = "01K70000000000000000000013";
const Ulid kKeyIn = "01K70000000000000000000014";
const Ulid kKeyOut = "01K70000000000000000000015";

// A title card: a column block holding a title, the block moved by a point
// prop, the title faded in over six frames.
const char kDefinition[] =
    R"({"width":1080,"height":1920,"fps":25,"duration":60,)"
    R"("props":{"titre":{"type":"text","default":"Titre"},)"
    R"("taille":{"type":"number","default":80,"min":10,"max":200},)"
    R"("position":{"type":"point","default":{"x":0,"y":0},"min":-540,"max":540},)"
    R"("carte":{"type":"group","props":["titre","taille"]}},)"
    R"("root":{"type":"fill","uid":"01K70000000000000000000011",)"
    R"("style":{"justifyContent":"center","alignItems":"center"},"children":[)"
    R"({"type":"box","uid":"01K70000000000000000000012","id":"bloc",)"
    R"("style":{"flexDirection":"column","translateX":{"prop":"position.x"}},)"
    R"("children":[{"type":"text","uid":"01K70000000000000000000013",)"
    R"("id":"titre","content":{"prop":"titre"},)"
    R"("style":{"fontSize":{"prop":"taille"},"lineHeight":1.4},)"
    R"("animate":{"opacity":{"interpolate":{"frames":[0,6],"values":[0,1],)"
    R"("uids":["01K70000000000000000000014","01K70000000000000000000015"]}}}}]}]}})";

Document Base() {
    Document document;
    document.sequence.id = kSequence;
    document.sequence.name = "Montage";
    document.sources = {{kSource, "rush.mov", {25, 1}, {1000, 25}}};
    document.sequence.tracks = {
        {kTrack, "video", 0, {{kClip, kSource, {0, 25}, {50, 25}, {0, 25}}}}};
    return document;
}

DocumentComposition Card() {
    return {kComposition, "Carton titre", Json(kDefinition)};
}

Document WithCard() {
    Document document = Base();
    document.compositions = {Card()};
    return document;
}

struct Applied {
    bool ok = false;
    EditError error = EditError::None;
    std::string message;
};

Applied Apply(Document& document, EditLog& log, Operation operation) {
    Applied result;
    result.ok =
        log.Apply(document, std::move(operation), result.error, result.message);
    return result;
}

// Apply, undo, redo, and the same from the journal read back: every step
// lands on the exact bytes it should.
void RoundTrip(const std::string& label, Document document,
               const Operation& operation) {
    EditLog log;
    const std::string before = document.SaveToString();
    const Applied applied = Apply(document, log, operation);
    Check(applied.ok, label + " applies: " + applied.message);
    if (!applied.ok) {
        return;
    }
    const std::string after = document.SaveToString();
    Check(after != before, label + " changes the document");

    const std::string journal = log.Serialize();
    EditError error = EditError::None;
    std::string message;
    Document undone = document;
    Check(log.Undo(undone, error, message) && undone.SaveToString() == before,
          label + ": undo restores the bytes: " + message);
    Check(log.Redo(undone, error, message) && undone.SaveToString() == after,
          label + ": redo replays the same bytes: " + message);

    EditLog reloaded;
    Check(EditLog::Deserialize(journal, reloaded, error, message) &&
              reloaded.Serialize() == journal,
          label + ": the journal reads back to its bytes: " + message);
    Document replay = document;
    Check(reloaded.Undo(replay, error, message) &&
              replay.SaveToString() == before,
          label + ": undo from the journal read back: " + message);
    Check(
        reloaded.Redo(replay, error, message) && replay.SaveToString() == after,
        label + ": redo from the journal read back: " + message);

    // Canonical: what the journal holds reads back as itself.
    const Operation& recorded = log.AppliedEntries().back().op;
    Operation parsed;
    Check(DeserializeOperation(SerializeOperation(recorded), parsed, error,
                               message) &&
              SerializeOperation(parsed) == SerializeOperation(recorded),
          label + ": its serialization is canonical: " + message);
}

void Refused(const std::string& label, Document document,
             const Operation& operation, EditError expected) {
    EditLog log;
    const std::string before = document.SaveToString();
    const Applied applied = Apply(document, log, operation);
    Check(!applied.ok && applied.error == expected,
          label + " is refused as " + EditErrorName(expected) + ", got " +
              EditErrorName(applied.error) + ": " + applied.message);
    Check(document.SaveToString() == before && log.AppliedCount() == 0,
          label + ": a refusal changes nothing");
}

const DocumentComposition& Only(const Document& document) {
    return document.compositions.front();
}

const Value* At(const Document& document, const std::string& spelled) {
    // "root.children.0.style" style addresses, for the checks below only.
    MotionTreePath path;
    size_t start = 0;
    while (start <= spelled.size()) {
        const size_t dot = spelled.find('.', start);
        const std::string step = spelled.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!step.empty() &&
            step.find_first_not_of("0123456789") == std::string::npos) {
            path.push_back({std::string(), std::stoul(step), true});
        } else {
            path.push_back({step, 0, false});
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return MotionTreeAt(Only(document).definition, path);
}

void TestEveryOperationRoundTrips() {
    AddMotionCompositionOperation add;
    add.composition = Card();
    RoundTrip("AddMotionComposition", Base(), add);
    RoundTrip("RemoveMotionComposition", WithCard(),
              RemoveMotionCompositionOperation{kComposition});
    DocumentComposition renamed = Card();
    renamed.name = "Carton titre — version courte";
    RoundTrip("UpdateMotionComposition", WithCard(),
              UpdateMotionCompositionOperation{renamed});
    RoundTrip(
        "SetMotionNode (insert)", WithCard(),
        SetMotionNodeOperation{kComposition, kBlock, kTitle,
                               Json(R"({"type":"text","content":"Date"})")});
    RoundTrip("SetMotionNode (replace)", WithCard(),
              SetMotionNodeOperation{
                  kComposition, "", "",
                  Json(R"({"type":"text","uid":"01K70000000000000000000013",)"
                       R"("content":"Autre titre"})")});
    RoundTrip("RemoveMotionNode", WithCard(),
              RemoveMotionNodeOperation{kComposition, kTitle});
    RoundTrip("SetMotionStyle (set)", WithCard(),
              SetMotionStyleOperation{kComposition, kTitle, "letterSpacing",
                                      Json("2.5")});
    RoundTrip("SetMotionStyle (remove)", WithCard(),
              SetMotionStyleOperation{kComposition, kTitle, "lineHeight",
                                      std::nullopt});
    RoundTrip("SetMotionAnimation (set)", WithCard(),
              SetMotionAnimationOperation{
                  kComposition, kBlock, "scale",
                  Json(R"({"spring":{"from":0.8,"to":1,"damping":12.5}})")});
    RoundTrip("SetMotionAnimation (remove)", WithCard(),
              SetMotionAnimationOperation{kComposition, kTitle, "opacity",
                                          std::nullopt});
    RoundTrip(
        "SetMotionKeyframe", WithCard(),
        SetMotionKeyframeOperation{kComposition, kKeyOut, 9, Json("0.75")});
    RoundTrip(
        "SetMotionProps", WithCard(),
        SetMotionPropsOperation{
            kComposition,
            Json(R"({"titre":"Nouveau","position":{"x":-120.5,"y":64}})")});
}

void TestEditsSayWhatTheyDo() {
    Document document = WithCard();
    EditLog log;
    Check(Apply(document, log,
                SetMotionStyleOperation{kComposition, kTitle, "letterSpacing",
                                        Json("2.5")})
              .ok,
          "a style property is set");
    Check((At(document, "root.children.0.children.0.style.letterSpacing") !=
           nullptr) &&
              At(document, "root.children.0.children.0.style.letterSpacing")
                      ->Dump() == "2.5",
          "the decimal is stored as written");
    Check(Apply(document, log,
                SetMotionStyleOperation{kComposition, kBlock, "translateX",
                                        std::nullopt})
                  .ok &&
              At(document, "root.children.0.style.translateX") == nullptr,
          "a style property is removed");
    Check(Apply(document, log,
                SetMotionStyleOperation{kComposition, kBlock, "flexDirection",
                                        std::nullopt})
                  .ok &&
              At(document, "root.children.0.style") == nullptr,
          "the style object goes with its last property");

    Check(Apply(document, log,
                SetMotionKeyframeOperation{kComposition, kKeyOut, 12,
                                           Json("0.9")})
              .ok,
          "a keyframe moves");
    const Value* interpolate =
        At(document, "root.children.0.children.0.animate.opacity.interpolate");
    Check((interpolate != nullptr) &&
              interpolate->Dump() ==
                  R"({"frames":[0,12],"values":[0,0.9],"uids":[")" + kKeyIn +
                      R"(",")" + kKeyOut + R"("]})",
          "the keyframe keeps its uid at its new frame and value: " +
              ((interpolate != nullptr) ? interpolate->Dump()
                                        : std::string("missing")));

    Check(Apply(document, log,
                SetMotionPropsOperation{kComposition,
                                        Json(R"({"taille":120,"titre":null})")})
              .ok,
          "prop values are set");
    Check(At(document, "props.taille.default")->Dump() == "120" &&
              At(document, "props.titre.default") == nullptr,
          "a value becomes the default; null removes it");
}

void TestIdentities() {
    Document document = WithCard();
    EditLog log;
    SetMotionNodeOperation insert{
        kComposition, kBlock, "",
        Json(R"({"type":"text","content":"Date","animate":{"opacity":)"
             R"({"interpolate":{"frames":[6,12],"values":[0,1]}}}})")};
    Check(Apply(document, log, insert).ok, "a node without uid is posed");
    const auto* recorded =
        std::get_if<SetMotionNodeOperation>(&log.AppliedEntries().back().op);
    const Value* uid =
        (recorded != nullptr) ? recorded->node.Find("uid") : nullptr;
    Check((uid != nullptr) && uid->IsString() && IsValidUlid(uid->AsString()),
          "first application gives the node a ULID, kept in the operation");
    const Value* keys = (recorded != nullptr) ? recorded->node.Find("animate")
                                                    ->Find("opacity")
                                                    ->Find("interpolate")
                                                    ->Find("uids")
                                              : nullptr;
    Check((keys != nullptr) && keys->AsArray().size() == 2 &&
              IsValidUlid(keys->AsArray()[0].AsString()),
          "and each of its keyframes one");
    Check(
        SerializeOperation((recorded != nullptr) ? Operation(*recorded)
                                                 : Operation())
                .find("\"uid\":\"" + ((uid != nullptr) ? uid->AsString() : "") +
                      "\"") != std::string::npos,
        "the journal records the identities, so redo replays them");
    // The uid follows the type, where a reader looks for it.
    Check((recorded != nullptr) && recorded->node.AsObject()[1].first == "uid",
          "a generated uid is written right after the node's type");

    EditError error = EditError::None;
    std::string message;
    const std::string after = document.SaveToString();
    Check(log.Undo(document, error, message) &&
              log.Redo(document, error, message) &&
              document.SaveToString() == after,
          "redo gives the node the same identities again");

    AddMotionCompositionOperation add;
    add.composition.id.clear();
    add.composition.name = "Sans identités";
    add.composition.definition =
        Json(R"({"width":100,"height":100,"fps":25,"duration":10,)"
             R"("root":{"type":"fill","children":[{"type":"box"}]}})");
    Document fresh = Base();
    EditLog freshLog;
    Check(Apply(fresh, freshLog, add).ok, "a composition without identities");
    const DocumentComposition& stored = fresh.compositions.front();
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(stored.definition, nodes, keyframes);
    Check(IsValidUlid(stored.id) && nodes.size() == 2,
          "gets an id and a uid on each of its nodes");
    DocumentDelta delta;
    ComputeDocumentDelta(Base(), fresh, delta);
    const std::string reported = SerializeDocumentDelta(delta);
    Check(reported.find("\"created_composition_ids\":[\"" + stored.id +
                        "\"]") != std::string::npos &&
              reported.find("\"created_motion_node_ids\":[\"" + nodes[0] +
                            "\",\"" + nodes[1] + "\"]") != std::string::npos,
          "the delta names what was created: " + reported);
}

void TestRefusals() {
    const Ulid unknown = "01K7000000000000000000ZZZZ";
    Refused("an unknown composition", WithCard(),
            RemoveMotionCompositionOperation{unknown},
            EditError::UnknownComposition);
    Refused("a style on an unknown composition", WithCard(),
            SetMotionStyleOperation{unknown, kTitle, "opacity", Json("1")},
            EditError::UnknownComposition);
    Refused(
        "an unknown node", WithCard(),
        SetMotionStyleOperation{kComposition, unknown, "opacity", Json("1")},
        EditError::UnknownMotionNode);
    Refused("an unknown keyframe", WithCard(),
            SetMotionKeyframeOperation{kComposition, unknown, 3, Json("1")},
            EditError::UnknownKeyframe);
    Refused("a prop above its bound", WithCard(),
            SetMotionPropsOperation{kComposition, Json(R"({"taille":201})")},
            EditError::PropOutOfRange);
    Refused("a point component below its bound", WithCard(),
            SetMotionPropsOperation{kComposition,
                                    Json(R"({"position":{"x":-541,"y":0}})")},
            EditError::PropOutOfRange);
    Refused(
        "a prop of the wrong type", WithCard(),
        SetMotionPropsOperation{kComposition, Json(R"({"taille":"grand"})")},
        EditError::PropTypeMismatch);
    Refused("a value for a group", WithCard(),
            SetMotionPropsOperation{kComposition, Json(R"({"carte":1})")},
            EditError::PropTypeMismatch);
    Refused("a prop nothing declares", WithCard(),
            SetMotionPropsOperation{kComposition, Json(R"({"couleur":1})")},
            EditError::UnknownProp);
    Refused("a binding to a prop nothing declares", WithCard(),
            SetMotionStyleOperation{kComposition, kTitle, "color",
                                    Json(R"({"prop":"couleur"})")},
            EditError::UnknownProp);
    Refused(
        "a name that is not CSS", WithCard(),
        SetMotionStyleOperation{kComposition, kTitle, "paddingX", Json("4")},
        EditError::InvalidMotion);
    Refused(
        "a literal outside its domain", WithCard(),
        SetMotionStyleOperation{kComposition, kTitle, "opacity", Json("1.5")},
        EditError::InvalidMotion);
    Refused("a keyframe that would reorder its track", WithCard(),
            SetMotionKeyframeOperation{kComposition, kKeyIn, 6, Json("0")},
            EditError::InvalidMotion);
    Refused(
        "removing a property that is not there", WithCard(),
        SetMotionStyleOperation{kComposition, kTitle, "opacity", std::nullopt},
        EditError::InvalidOperation);
    Refused("removing the root", WithCard(),
            RemoveMotionNodeOperation{kComposition, kRoot},
            EditError::InvalidOperation);
    Refused("a child for a text", WithCard(),
            SetMotionNodeOperation{kComposition, kTitle, "",
                                   Json(R"({"type":"box"})")},
            EditError::InvalidOperation);
    Refused("before a node that is not a child", WithCard(),
            SetMotionNodeOperation{kComposition, kRoot, kTitle,
                                   Json(R"({"type":"box"})")},
            EditError::UnknownMotionNode);
    AddMotionCompositionOperation twice;
    twice.composition = Card();
    Refused("a composition id already there", WithCard(), twice,
            EditError::DuplicateId);
    AddMotionCompositionOperation clash;
    clash.composition = Card();
    clash.composition.id = kClip;
    Refused("a composition id a clip already has", Base(), clash,
            EditError::DuplicateId);
    SetMotionNodeOperation copied{
        kComposition, kRoot, "",
        Json(R"({"type":"box","children":[{"type":"box","uid":")" + kBlock +
             R"("}]})")};
    Refused("a uid the composition already uses", WithCard(), copied,
            EditError::InvalidMotion);
}

void TestVersion() {
    // A document without compositions keeps version 7 and its bytes; one
    // with a composition is version 8, and goes back when it has none.
    Document document = Base();
    const std::string plain = document.SaveToString();
    Check(plain.rfind("{\n  \"version\": 7,", 0) == 0 &&
              plain.find("compositions") == std::string::npos,
          "without a composition: version 7, no key");
    EditLog log;
    AddMotionCompositionOperation add;
    add.composition = Card();
    Check(Apply(document, log, add).ok, "a composition is added");
    const std::string withCard = document.SaveToString();
    Check(withCard.rfind("{\n  \"version\": 8,", 0) == 0,
          "with a composition: version 8");
    Document reread;
    std::string error;
    Check(Document::LoadFromString(withCard, reread, error) &&
              reread.SaveToString() == withCard,
          "a version-8 document reads back to its bytes: " + error);
    Check(reread.compositions.size() == 1 &&
              reread.compositions[0].definition.Dump() ==
                  Card().definition.Dump(),
          "its composition reads back as it was written, decimals included");

    std::string olderVersion = withCard;
    olderVersion.replace(olderVersion.find("\"version\": 8"), 12,
                         "\"version\": 7");
    Check(!Document::LoadFromString(olderVersion, reread, error) &&
              error.find("cannot hold compositions") != std::string::npos,
          "a version-7 document cannot hold compositions: " + error);

    EditError editError = EditError::None;
    std::string message;
    Check(log.Undo(document, editError, message) &&
              document.SaveToString() == plain,
          "without its composition again, the document is version 7, byte "
          "for byte");

    // Migration: a document from before compositions -- version 4 here,
    // without opacity or clip audio -- reads, takes a composition and is
    // written complete at version 8.
    std::string versionFour = plain;
    versionFour.replace(versionFour.find("\"version\": 7"), 12,
                        "\"version\": 4");
    for (const std::string field :
         {",\"opacity\":{\"num\":1,\"den\":1}",
          ",\"audio_gain_db\":{\"num\":0,\"den\":1}",
          ",\"audio_fade_in\":{\"value\":0,\"rate\":1}",
          ",\"audio_fade_out\":{\"value\":0,\"rate\":1}"}) {
        const size_t at = versionFour.find(field);
        Check(at != std::string::npos, "the base document spells " + field);
        if (at != std::string::npos) {
            versionFour.erase(at, field.size());
        }
    }
    Document migrated;
    EditLog migratedLog;
    Check(Document::LoadFromString(versionFour, migrated, error) &&
              migrated.SaveToString() == plain,
          "a version-4 document reads and is written back as version 7: " +
              error);
    AddMotionCompositionOperation onOld;
    onOld.composition = Card();
    Check(Apply(migrated, migratedLog, onOld).ok &&
              migrated.SaveToString() == withCard,
          "and with a composition, as the same version-8 document");

    // The project file follows the same rule, and keeps compositions
    // across its timelines.
    Project project = Project::FromDocument(WithCard(), "Projet");
    const std::string saved = project.SaveToString();
    Check(saved.find("\"document_version\":8,") != std::string::npos &&
              saved.find("\"compositions\":[") != std::string::npos,
          "a project with a composition: document version 8");
    Project loaded;
    Check(Project::LoadFromString(saved, loaded, error) &&
              loaded.SaveToString() == saved && loaded.compositions.size() == 1,
          "reads back to its bytes: " + error);
    Document edited = loaded.MakeActiveDocument();
    Check(edited.compositions.size() == 1,
          "a timeline's edit document carries the project's compositions");
    EditLog projectLog;
    Check(Apply(edited, projectLog,
                SetMotionPropsOperation{kComposition,
                                        Json(R"({"titre":"Commit"})")})
                  .ok &&
              loaded.CommitActiveDocument(edited, error) &&
              loaded.compositions[0].definition.Dump().find("Commit") !=
                  std::string::npos,
          "and commits the edit back to the project: " + error);
    Project bare = Project::FromDocument(Base(), "Projet");
    Check(bare.SaveToString().find("\"document_version\":7,") !=
                  std::string::npos &&
              bare.SaveToString().find("compositions") == std::string::npos,
          "a project without one stays at document version 7");

    ProjectDelta delta;
    Project changed = loaded;
    changed.compositions.clear();
    Check(!ComputeProjectDelta(loaded, changed, delta, error),
          "a project delta refuses a change of compositions rather than "
          "losing it");
}

// Out of lot -- 29 October 2026. Project::Validate parses the compositions
// for its first timeline only (Document::CompositionCheck); every timeline
// is still met against their identities.
void TestProjectValidatesEveryTimeline() {
    std::string error;
    Project project = Project::FromDocument(WithCard(), "Projet");
    DocumentSequence variant = project.timelines.front();
    variant.id = "01K70000000000000000000020";
    variant.name = "Variante";
    variant.tracks = {{"01K70000000000000000000021",
                       "video",
                       0,
                       {{"01K70000000000000000000022",
                         kSource,
                         {0, 25},
                         {50, 25},
                         {0, 25}}}}};
    project.timelines.push_back(variant);
    Check(project.Validate(error),
          "two timelines sharing a composition: " + error);

    Project effectClash = project;
    ClipEffect effect;
    effect.id = kKeyIn;
    effect.type = "color.contrast";
    effect.params["amount"] = {1, 5};
    effectClash.timelines[1].tracks[0].clips[0].effects = {effect};
    Check(
        !effectClash.Validate(error) && error.find(kKeyIn) != std::string::npos,
        "a second timeline's clip effect named like a keyframe is refused: " +
            error);

    Project clipClash = project;
    clipClash.timelines[1].tracks[0].clips[0].id = kTitle;
    Check(!clipClash.Validate(error) && error.find(kTitle) != std::string::npos,
          "a second timeline's clip named like a node is refused: " + error);
}

void TestStoredCompositionEvaluates() {
    // No text measured: a box laid out alone, bound to the point prop.
    const DocumentComposition card{
        kComposition, "Boîte",
        Json(
            R"({"width":200,"height":100,"fps":25,"duration":10,)"
            R"("props":{"position":{"type":"point","default":{"x":20,"y":10}}},)"
            R"("root":{"type":"fill","uid":"01K70000000000000000000011",)"
            R"("children":[{"type":"box","uid":"01K70000000000000000000012",)"
            R"("id":"b","style":{"position":"absolute","left":{"prop":"position.x"},)"
            R"("top":{"prop":"position.y"},"width":30,"height":15}}]}})")};
    MotionComposition parsed;
    MotionError error;
    Check(ParseDocumentComposition(card, parsed, error),
          "a stored composition parses: " + error.message);
    class NoText final : public MotionMeasurer {
    public:
        bool LayoutText(const std::string&, const MotionTextStyle&, double,
                        MotionTextLayout&, MotionError& error) override {
            error = {"FontMissing", "", "no text in this test"};
            return false;
        }
        bool ImageSize(const std::string&, double&, double&,
                       MotionError& error) override {
            error = {"MediaMissing", "", "no image in this test"};
            return false;
        }
    } measurer;
    MotionFrame frame;
    Check(EvaluateMotionFrame(parsed, Value(), 0, measurer, frame, error) &&
              frame.root.children.size() == 1 &&
              frame.root.children[0].uid == kBlock &&
              frame.root.children[0].box.x == 20 &&
              frame.root.children[0].box.y == 10,
          "and evaluates, each box naming its document node");
}

// M6 -- versions and formats live in the definition: they are posed and
// changed by UpdateMotionComposition, judged by the motion parser with the
// prop refusals of M3, and read back by every surface without a new
// operation or a new document field.
DocumentComposition WithEntry(const DocumentComposition& composition,
                              const std::string& section,
                              const std::string& name,
                              const std::optional<Value>& value) {
    DocumentComposition changed = composition;
    Check(MotionDefinitionWithSetEntry(composition.definition, section, name,
                                       value, changed.definition),
          "the " + section + " entry '" + name + "' is set");
    return changed;
}

void TestVersionsAndFormats() {
    const DocumentComposition versioned =
        WithEntry(Card(), "variants", "long",
                  Json(R"({"props":{"titre":"Un titre bien plus long"}})"));
    const auto& keys = versioned.definition.AsObject();
    Check(keys.size() >= 2 && keys[keys.size() - 2].first == "variants" &&
              keys.back().first == "root",
          "a first version opens its section before the tree");
    RoundTrip("a version posed by UpdateMotionComposition", WithCard(),
              UpdateMotionCompositionOperation{versioned});

    Document document = WithCard();
    EditLog log;
    const DocumentComposition sized = WithEntry(
        WithEntry(versioned, "variants", "court",
                  Json(R"({"props":{"titre":"Court"}})")),
        "formats", "16x9",
        Json(R"({"width":1920,"height":1080,"props":{"taille":150}})"));
    const Applied applied =
        Apply(document, log, UpdateMotionCompositionOperation{sized});
    Check(applied.ok, "two versions and a format apply: " + applied.message);
    MotionComposition parsed;
    MotionError error;
    Check(ParseDocumentComposition(Only(document), parsed, error) &&
              parsed.variants.size() == 2 && parsed.formats.size() == 1 &&
              parsed.formats[0].width == 1920,
          "the stored composition carries its versions and format: " +
              error.message);
    Check(MotionCompositionListJson(document.compositions)
                  .find(R"("variants":["long","court"],"formats":[)"
                        R"({"name":"16x9","width":1920,"height":1080}])") !=
              std::string::npos,
          "the list names them for whoever renders: " +
              MotionCompositionListJson(document.compositions));
    RoundTrip("a format removed", document,
              UpdateMotionCompositionOperation{
                  WithEntry(Only(document), "formats", "16x9", std::nullopt)});
    Check(WithEntry(Only(document), "formats", "16x9", std::nullopt)
                  .definition.Find("formats") == nullptr,
          "the last format takes its section with it");
    Value unchanged;
    Check(!MotionDefinitionWithSetEntry(Only(document).definition, "variants",
                                        "moyen", std::nullopt, unchanged),
          "removing a version that is not there is refused");

    // Judged like any value, with the same names.
    Refused("a version outside a prop's bounds", WithCard(),
            UpdateMotionCompositionOperation{WithEntry(
                Card(), "variants", "a", Json(R"({"props":{"taille":900}})"))},
            EditError::PropOutOfRange);
    Refused("a version that sets an undeclared prop", WithCard(),
            UpdateMotionCompositionOperation{WithEntry(
                Card(), "variants", "a", Json(R"({"props":{"date":"x"}})"))},
            EditError::UnknownProp);
    Refused("a version of the wrong type", WithCard(),
            UpdateMotionCompositionOperation{WithEntry(
                Card(), "variants", "a", Json(R"({"props":{"titre":12}})"))},
            EditError::PropTypeMismatch);
    Refused("a format that sets a group", WithCard(),
            UpdateMotionCompositionOperation{
                WithEntry(Card(), "formats", "f",
                          Json(R"({"width":10,"height":10,)"
                               R"("props":{"carte":"x"}})"))},
            EditError::PropTypeMismatch);
    Refused("a prop set by a format and by a version", document,
            UpdateMotionCompositionOperation{
                WithEntry(WithEntry(Only(document), "formats", "f",
                                    Json(R"({"width":10,"height":10,)"
                                         R"("props":{"titre":"x"}})")),
                          "variants", "b", Json(R"({"props":{"titre":"y"}})"))},
            EditError::InvalidMotion);
    // A later edit of the props is judged against the versions too.
    DocumentComposition narrowed = Only(document);
    narrowed.definition = MotionTreeWith(
        narrowed.definition, {{"props", 0, false}, {"titre", 0, false}},
        Json(R"({"type":"enum","values":["Titre","Court"]})"));
    Refused("props narrowed under a version's value", document,
            UpdateMotionCompositionOperation{narrowed},
            EditError::PropOutOfRange);
}

}  // namespace

namespace {

// REMOTION-PARITY-2026-09 REM2 -- a composition that plays a media of the
// project and shows another composition through an instance: both are
// named by ULID, judged against the document, and edited by the operations
// that already exist.
const Ulid kPlayer = "01K70000000000000000000020";
const Ulid kPlayerRoot = "01K70000000000000000000021";
const Ulid kInstance = "01K70000000000000000000022";
const Ulid kVideo = "01K70000000000000000000023";

std::string PlayerDefinition(const std::string& media, const std::string& shown,
                             const std::string& props) {
    return R"({"width":1920,"height":1080,"fps":25,"duration":50,)"
           R"("root":{"type":"fill","uid":")" +
           kPlayerRoot + R"(","children":[{"type":"video","uid":")" + kVideo +
           R"(","media":")" + media +
           R"(","muted":true},{"type":"composition","uid":")" + kInstance +
           R"(","composition":")" + shown + R"(","props":)" + props + R"(}]}})";
}

Document WithPlayer(const std::string& definition) {
    Document document = WithCard();
    document.compositions.push_back({kPlayer, "Lecteur", Json(definition)});
    return document;
}

void TestMediaAndInstances() {
    const std::string valid =
        PlayerDefinition(kSource, kComposition, R"({"titre":"Ana"})");
    std::string validation;
    Check(WithPlayer(valid).Validate(validation),
          "a composition naming a media and an instance of the project is "
          "valid: " +
              validation);
    AddMotionCompositionOperation add;
    add.composition = {kPlayer, "Lecteur", Json(valid)};
    RoundTrip("AddMotionComposition (video and instance)", WithCard(), add);
    RoundTrip("SetMotionNode (a second instance)", WithPlayer(valid),
              SetMotionNodeOperation{
                  kPlayer, kPlayerRoot, "",
                  Json(R"({"type":"composition","composition":")" +
                       kComposition + R"(","props":{"taille":120}})")});
    RoundTrip("SetMotionNode (an audio node)", WithPlayer(valid),
              SetMotionNodeOperation{
                  kPlayer, kPlayerRoot, kVideo,
                  Json(R"({"type":"audio","media":")" + kSource +
                       R"(","startFrom":5,"style":{"volume":0.5}})")});
    RoundTrip("SetMotionAnimation (a volume fade)", WithPlayer(valid),
              SetMotionAnimationOperation{
                  kPlayer, kVideo, "volume",
                  Json(R"({"interpolate":{"frames":[0,10],"values":[0,1]}})")});

    const Ulid unknown = "01K7000000000000000000ZZZZ";
    AddMotionCompositionOperation missingMedia;
    missingMedia.composition = {
        kPlayer, "Lecteur",
        Json(PlayerDefinition(unknown, kComposition, "{}"))};
    Refused("a media the project does not have", WithCard(), missingMedia,
            EditError::UnknownMedia);
    AddMotionCompositionOperation missingComposition;
    missingComposition.composition = {
        kPlayer, "Lecteur", Json(PlayerDefinition(kSource, unknown, "{}"))};
    Refused("an instance of a composition the project does not have",
            WithCard(), missingComposition, EditError::UnknownComposition);
    AddMotionCompositionOperation unknownProp;
    unknownProp.composition = {
        kPlayer, "Lecteur",
        Json(PlayerDefinition(kSource, kComposition, R"({"couleur":"#fff"})"))};
    Refused("an instance prop the composition does not declare", WithCard(),
            unknownProp, EditError::UnknownProp);
    AddMotionCompositionOperation outOfRange;
    outOfRange.composition = {
        kPlayer, "Lecteur",
        Json(PlayerDefinition(kSource, kComposition, R"({"taille":500})"))};
    Refused("an instance prop above its bound", WithCard(), outOfRange,
            EditError::PropOutOfRange);
    AddMotionCompositionOperation wrongType;
    wrongType.composition = {
        kPlayer, "Lecteur",
        Json(PlayerDefinition(kSource, kComposition, R"({"titre":3})"))};
    Refused("an instance prop of the wrong type", WithCard(), wrongType,
            EditError::PropTypeMismatch);
    Refused("removing a composition an instance shows", WithPlayer(valid),
            RemoveMotionCompositionOperation{kComposition},
            EditError::ValidationFailed);
    // The card shows the player, which shows the card: a cycle.
    RoundTrip("RemoveMotionComposition (the player, then the card is free)",
              WithPlayer(valid), RemoveMotionCompositionOperation{kPlayer});
    Document cyclic = WithPlayer(valid);
    EditLog log;
    const Applied applied = Apply(
        cyclic, log,
        SetMotionNodeOperation{kComposition, kBlock, "",
                               Json(R"({"type":"composition","composition":")" +
                                    kPlayer + R"("})")});
    Check(!applied.ok && applied.error == EditError::InvalidMotion &&
              applied.message.rfind("CompositionCycle", 0) == 0,
          "a card that would show the player showing it is refused as a "
          "cycle: " +
              std::string(EditErrorName(applied.error)) + " " +
              applied.message);

    // What a render of the player reaches: the card, parsed, and the media.
    MotionReach reach;
    MotionError error;
    const Document player = WithPlayer(valid);
    Check(MotionReachFor(player, kPlayer, reach, error) &&
              reach.library->compositions.size() == 1 &&
              reach.library->compositions.count(kComposition) == 1 &&
              reach.media_paths.at(kSource) == "rush.mov" &&
              reach.composition_id == kPlayer,
          "the player reaches the card and the project's media: " +
              error.message);
    Check(MotionReachKey(player, kComposition).empty(),
          "a composition that reaches nothing adds nothing to its picture "
          "key");
    const std::string key = MotionReachKey(player, kPlayer);
    Document relinked = player;
    relinked.sources[0].path = "autre.mov";
    Document edited = player;
    edited.compositions[0].name = "Renommé";
    edited.compositions[0].definition =
        Json(std::string(kDefinition)
                 .replace(std::string(kDefinition).find("Titre"), 5, "Autre"));
    Check(!key.empty() && MotionReachKey(relinked, kPlayer) != key &&
              MotionReachKey(edited, kPlayer) != key,
          "a relink or an edit of the card changes the player's picture key");
}

int RunTests() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"every motion operation round-trips byte for byte",
         TestEveryOperationRoundTrips},
        {"motion edits change what they name", TestEditsSayWhatTheyDo},
        {"identities are given once and kept", TestIdentities},
        {"refusals are named and change nothing", TestRefusals},
        {"compositions need document version 8, and only they do", TestVersion},
        {"a project meets every timeline against its compositions",
         TestProjectValidatesEveryTimeline},
        {"a stored composition evaluates", TestStoredCompositionEvaluates},
        {"versions and formats are posed and judged", TestVersionsAndFormats},
        {"media and instances are named by ULID and judged",
         TestMediaAndInstances},
    };
    for (const auto& [name, test] : tests) {
        const int before = failures;
        try {
            test();
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
        }
        if (failures == before) {
            std::cout << "PASS: " << name << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    try {
        return RunTests();
    } catch (...) {
        return 1;
    }
}
