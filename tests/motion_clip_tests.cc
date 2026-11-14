// MOTION-2026-09 M4 -- ROADMAP.md. Motion clips on the timeline: a clip
// whose source is a composition. Its schema and version, the two
// operations that pose it and its instance props, what the editing
// operations of every clip do to it, and the frame Timeline resolves for
// it. Portable: no framework, no file, no text measured.

#include "Document.h"
#include "DocumentDelta.h"
#include "EditLog.h"
#include "Json.h"
#include "MotionClip.h"
#include "MotionClipPreview.h"
#include "MotionDocument.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectDelta.h"
#include "ResolveExport.h"
#include "ShotQuality.h"
#include "SourceAddress.h"
#include "Timeline.h"
#include "TimelineSheets.h"
#include "TimelineStats.h"

#include <algorithm>
#include <chrono>
#include <iostream>
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

const Ulid kSequence = "01K80000000000000000000001";
const Ulid kSource = "01K80000000000000000000002";
const Ulid kVideo = "01K80000000000000000000003";
const Ulid kClip = "01K80000000000000000000004";
const Ulid kMotionTrack = "01K80000000000000000000005";
const Ulid kAudio = "01K80000000000000000000006";
const Ulid kMotion = "01K80000000000000000000007";
const Ulid kSecondMotion = "01K80000000000000000000008";
const Ulid kComposition = "01K80000000000000000000010";

// A lower third of 60 frames at 25 fps: a title, a size, a point and a
// group, the title faded in.
const char kDefinition[] =
    R"({"width":1080,"height":1920,"fps":25,"duration":60,)"
    R"("props":{"titre":{"type":"text","default":"Titre"},)"
    R"("taille":{"type":"number","default":80,"min":10,"max":200},)"
    R"("position":{"type":"point","default":{"x":0,"y":0},"min":-540,"max":540},)"
    R"("carte":{"type":"group","props":["titre","taille"]}},)"
    R"("root":{"type":"fill","uid":"01K80000000000000000000011",)"
    R"("children":[{"type":"text","uid":"01K80000000000000000000012",)"
    R"("id":"titre","content":{"prop":"titre"},)"
    R"("style":{"fontSize":{"prop":"taille"},"lineHeight":1.4},)"
    R"("animate":{"opacity":{"interpolate":{"frames":[0,6],"values":[0,1],)"
    R"("uids":["01K80000000000000000000013","01K80000000000000000000014"]}}}}]}})";

DocumentComposition Card(const std::string& definition = kDefinition) {
    return {kComposition, "Nom à l'écran", Json(definition)};
}

// An interview clip on V1 and an empty V2 above it, and a composition.
Document Base() {
    Document document;
    document.sequence.id = kSequence;
    document.sequence.name = "Interview";
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    document.sources = {{kSource, "itw.mov", {25, 1}, {1000, 25}}};
    document.sequence.tracks = {
        {kVideo, "video", 0, {{kClip, kSource, {0, 25}, {200, 25}, {0, 25}}}},
        {kMotionTrack, "video", 1, {}},
        {kAudio, "audio", 2, {}}};
    document.compositions = {Card()};
    return document;
}

InsertMotionClipOperation Insertion(RationalTime sourceIn = {0, 25},
                                    RationalTime duration = {50, 25},
                                    RationalTime timelineIn = {25, 25}) {
    InsertMotionClipOperation insert;
    insert.track_id = kMotionTrack;
    insert.composition_id = kComposition;
    insert.source_in = sourceIn;
    insert.duration = duration;
    insert.timeline_in = timelineIn;
    insert.clip_id = kMotion;
    return insert;
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

Document WithMotion(const Value& props = Value()) {
    Document document = Base();
    InsertMotionClipOperation insert = Insertion();
    insert.motion_props = props;
    EditLog log;
    const Applied applied = Apply(document, log, insert);
    if (!applied.ok) {
        throw std::runtime_error("fixture motion clip: " + applied.message);
    }
    return document;
}

const DocumentClip& Motion(const Document& document) {
    const DocumentClip* clip = document.FindClip(kMotion);
    if (clip == nullptr) {
        throw std::runtime_error("no motion clip");
    }
    return *clip;
}

// Apply, undo, redo, and the same from the journal read back: every step
// lands on the exact bytes it should (I4, I5, I9).
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
    const Operation& recorded = log.AppliedEntries().back().op;
    Operation parsed;
    Check(DeserializeOperation(SerializeOperation(recorded), parsed, error,
                               message) &&
              SerializeOperation(parsed) == SerializeOperation(recorded),
          label + ": its serialization is canonical: " + message);
}

void Refused(const std::string& label, Document document,
             const Operation& operation, EditError expected,
             const std::string& mentions = std::string()) {
    EditLog log;
    const std::string before = document.SaveToString();
    const Applied applied = Apply(document, log, operation);
    Check(!applied.ok && applied.error == expected,
          label + " is refused as " + EditErrorName(expected) + ", got " +
              EditErrorName(applied.error) + ": " + applied.message);
    Check(
        mentions.empty() || applied.message.find(mentions) != std::string::npos,
        label + ": the refusal says '" + mentions + "': " + applied.message);
    Check(document.SaveToString() == before && log.AppliedCount() == 0,
          label + ": a refusal changes nothing");
}

// The composition frame Timeline resolves on V2 at `frame` of the
// sequence, or -1 where V2 shows nothing.
int64_t ResolvedMotionFrame(const Document& document, int64_t frame) {
    const Timeline timeline(document);
    const std::optional<ResolvedFrame> resolved =
        timeline.ResolveTrack(kMotionTrack, RationalTime{frame, 25});
    if (!resolved) {
        return -1;
    }
    Check(resolved->source_id == kComposition,
          "a motion frame resolves to its composition");
    return resolved->source_frame;
}

void TestSchemaAndVersion() {
    const Document base = Base();
    Check(base.SaveToString().find("\"version\": 8") != std::string::npos,
          "a document with a composition and no motion clip stays at 8");
    Check(base.SaveToString().find("composition_id") == std::string::npos,
          "and spells nothing of motion clips");

    const Document motion = WithMotion(Json(R"({"titre":"Léa Martin",)"
                                            R"("taille":72.5})"));
    const std::string text = motion.SaveToString();
    Check(text.find("\"version\": 9") != std::string::npos,
          "a document holding a motion clip is written at 9:\n" + text);
    Check(text.find("\"source_id\":\"\"") != std::string::npos &&
              text.find("\"composition_id\":\"" + kComposition + "\"") !=
                  std::string::npos &&
              text.find("\"motion_props\":{\"titre\":\"Léa Martin\","
                        "\"taille\":72.5}") != std::string::npos,
          "the clip names its composition and keeps its props as written:\n" +
              text);
    Document reread;
    std::string error;
    Check(Document::LoadFromString(text, reread, error) &&
              reread.SaveToString() == text,
          "and reads back to the same bytes: " + error);

    // A project delta carries the clip, its decimals as written: the
    // journal of a project replays a motion clip's insertion exactly.
    {
        const Project before = Project::FromDocument(base, "Projet");
        Project after = before;
        after.timelines.front() = motion.sequence;
        ProjectDelta delta;
        ProjectDelta parsed;
        Project replayed = before;
        Check(ComputeProjectDelta(before, after, delta, error) &&
                  DeserializeProjectDelta(SerializeProjectDelta(delta), parsed,
                                          error) &&
                  ApplyProjectDelta(replayed, parsed, error) &&
                  replayed.SaveToString() == after.SaveToString(),
              "a project delta replays a motion clip byte for byte: " + error);
    }

    // Removing the clip gives the version back.
    Document removed = motion;
    EditLog log;
    Check(Apply(removed, log, RemoveClipOperation{kMotion}).ok &&
              removed.SaveToString() == base.SaveToString(),
          "without its motion clip the document is 8 again, byte for byte");

    // A project file declares 9 as soon as one timeline holds one.
    Project project = Project::FromDocument(motion, "Projet");
    const std::string projectText = project.SaveToString();
    Check(projectText.find("\"document_version\":9") != std::string::npos,
          "the project file declares version 9");
    Project reloaded;
    Check(Project::LoadFromString(projectText, reloaded, error) &&
              reloaded.SaveToString() == projectText,
          "and reads back to the same bytes: " + error);
}

void TestOperationsRoundTrip() {
    RoundTrip("InsertMotionClip", Base(), Insertion());
    InsertMotionClipOperation withProps = Insertion();
    withProps.motion_props =
        Json(R"({"position":{"x":12.5,"y":-4e1},"titre":"Deux\nlignes"})");
    RoundTrip("InsertMotionClip with props", Base(), withProps);
    InsertMotionClipOperation rippling = Insertion({0, 25}, {50, 25}, {0, 25});
    rippling.track_id = kVideo;
    rippling.sync_track_ids = {kAudio};
    RoundTrip("InsertMotionClip that ripples V1", Base(), rippling);

    RoundTrip(
        "SetClipMotionProps", WithMotion(),
        SetClipMotionPropsOperation{kMotion, Json(R"({"taille":0.5e2})")});
    RoundTrip("SetClipMotionProps back to none",
              WithMotion(Json(R"({"titre":"A"})")),
              SetClipMotionPropsOperation{kMotion, Value()});

    // Every clip edit applies to a motion clip as to any other.
    RoundTrip("TrimClip head on a motion clip", WithMotion(),
              TrimClipOperation{kMotion, TrimEdge::Head, {5, 25}, {}});
    RoundTrip("TrimClip tail on a motion clip", WithMotion(),
              TrimClipOperation{kMotion, TrimEdge::Tail, {10, 25}, {}});
    RoundTrip("SplitClip on a motion clip", WithMotion(),
              SplitClipOperation{kMotion, {45, 25}, {}, {}});
    RoundTrip("SlipEdit on a motion clip", WithMotion(),
              SlipEditOperation{{kMotion}, {4, 25}, {}});
    RoundTrip("MoveClip of a motion clip", WithMotion(),
              MoveClipOperation{kMotion, kMotionTrack, {100, 25}, {}});
    RoundTrip("SetClipOpacity on a motion clip", WithMotion(),
              SetClipOpacityOperation{kMotion, {1, 2}});
    RippleTrimOperation ripple;
    ripple.clip_id = kMotion;
    ripple.edge = TrimEdge::Tail;
    ripple.delta = {-10, 25};
    RoundTrip("RippleTrim on a motion clip", WithMotion(), ripple);
    RoundTrip("RemoveClip of a motion clip", WithMotion(),
              RemoveClipOperation{kMotion, {}, {}});
    RoundTrip("ClearClip of a motion clip", WithMotion(),
              ClearClipOperation{kMotion, {}});
}

void TestTimeFollowsTheClip() {
    Document document = WithMotion();
    // Placed at 1 s from its frame 0: sequence frame 25 is composition 0.
    Check(ResolvedMotionFrame(document, 24) == -1 &&
              ResolvedMotionFrame(document, 25) == 0 &&
              ResolvedMotionFrame(document, 40) == 15 &&
              ResolvedMotionFrame(document, 74) == 49 &&
              ResolvedMotionFrame(document, 75) == -1,
          "a motion clip shows its composition from its source_in");

    // A head trim advances the composition, like source_in on a media.
    EditLog log;
    Check(Apply(document, log,
                TrimClipOperation{kMotion, TrimEdge::Head, {5, 25}, {}})
              .ok,
          "head trim applies");
    Check(Motion(document).source_in == RationalTime{5, 25} &&
              ResolvedMotionFrame(document, 30) == 5 &&
              ResolvedMotionFrame(document, 40) == 15,
          "after a head trim of 5 frames the clip starts at frame 5, and "
          "every later frame is where it was");

    // A cut leaves every frame where it was.
    std::vector<int64_t> beforeCut;
    for (int64_t frame = 25; frame < 80; ++frame) {
        beforeCut.push_back(ResolvedMotionFrame(document, frame));
    }
    Check(
        Apply(document, log, SplitClipOperation{kMotion, {50, 25}, {}, {}}).ok,
        "split applies");
    std::vector<int64_t> afterCut;
    for (int64_t frame = 25; frame < 80; ++frame) {
        afterCut.push_back(ResolvedMotionFrame(document, frame));
    }
    Check(beforeCut == afterCut, "a cut changes no frame of the composition");
    const DocumentTrack* track = document.FindTrack(kMotionTrack);
    Check(track->clips.size() == 2 &&
              track->clips[1].composition_id == kComposition &&
              track->clips[1].source_in == RationalTime{25, 25},
          "the right half keeps the composition, at the time of the cut");

    // A tail past the composition's 60 frames is refused, as past a media.
    Refused("a tail past the composition's last frame", WithMotion(),
            TrimClipOperation{kMotion, TrimEdge::Tail, {11, 25}, {}},
            EditError::InvalidOperation, "must stay within");
    Refused("a slip past the composition's last frame", WithMotion(),
            SlipEditOperation{{kMotion}, {11, 25}, {}},
            EditError::SourceOutOfBounds);

    // Another cadence: a 50 fps composition on a 25 fps sequence shows every
    // other frame; a 10 fps one holds each frame for 2.5 sequence frames.
    MotionComposition fifty;
    fifty.fps_numerator = 50;
    MotionComposition ten;
    ten.fps_numerator = 10;
    Check(MotionClipFrames(fifty, {0, 25}, 4, {25, 1}) ==
                  std::vector<int64_t>({0, 2, 4, 6}) &&
              MotionClipFrames(ten, {0, 25}, 6, {25, 1}) ==
                  std::vector<int64_t>({0, 0, 0, 1, 1, 2}) &&
              MotionClipFrames(ten, {1, 10}, 3, {25, 1}) ==
                  std::vector<int64_t>({1, 1, 1}),
          "another cadence shows the frame that contains each time");
}

void TestRefusals() {
    InsertMotionClipOperation unknown = Insertion();
    unknown.composition_id = "01K8000000000000000000000Z";
    Refused("an unknown composition", Base(), unknown,
            EditError::UnknownComposition);
    InsertMotionClipOperation audio = Insertion();
    audio.track_id = kAudio;
    Refused("a motion clip on an audio track", Base(), audio,
            EditError::InvalidOperation, "video track");
    Refused("a range past the composition", Base(),
            Insertion({20, 25}, {41, 25}), EditError::SourceOutOfBounds);
    InsertMotionClipOperation overlapping =
        Insertion({0, 25}, {10, 25}, {30, 25});
    overlapping.clip_id = kSecondMotion;
    Refused("an overlap on its track", WithMotion(), overlapping,
            EditError::Overlap);

    const auto props = [](const char* text) {
        InsertMotionClipOperation insert = Insertion();
        insert.motion_props = Json(text);
        return insert;
    };
    Refused("an undeclared prop", Base(), props(R"({"sous_titre":"x"})"),
            EditError::UnknownProp, "motion_props.sous_titre");
    Refused("a prop of the wrong type", Base(), props(R"({"taille":"grand"})"),
            EditError::PropTypeMismatch, "motion_props.taille");
    Refused("a prop out of its bounds", Base(), props(R"({"taille":250})"),
            EditError::PropOutOfRange, "motion_props.taille");
    Refused("a component out of its bounds", Base(),
            props(R"({"position":{"x":600,"y":0}})"),
            EditError::PropOutOfRange);
    Refused("a value for a group", Base(), props(R"({"carte":"x"})"),
            EditError::PropTypeMismatch);
    Refused("an empty props object", Base(), props("{}"),
            EditError::InvalidOperation);
    Refused("a null prop value", Base(), props(R"({"titre":null})"),
            EditError::PropTypeMismatch);
    Refused("instance props on a media clip", Base(),
            SetClipMotionPropsOperation{kClip, Json(R"({"titre":"x"})")},
            EditError::InvalidOperation, "not a composition");
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A composition blurs its own
    // nodes; a clip blur on it would be a second blur nothing else draws.
    Refused("a clip blur on a motion clip", WithMotion(),
            SetClipBlurOperation{kMotion, {10, 1}}, EditError::InvalidOperation,
            "motion clip");
    Refused("instance props out of bounds", WithMotion(),
            SetClipMotionPropsOperation{kMotion, Json(R"({"taille":5})")},
            EditError::PropOutOfRange);

    // What the composition's own edits may no longer do.
    Refused("removing a composition a clip shows", WithMotion(),
            RemoveMotionCompositionOperation{kComposition},
            EditError::ValidationFailed, "unknown composition_id");
    std::string shorter = kDefinition;
    shorter.replace(shorter.find("\"duration\":60"), 13, "\"duration\":40");
    Refused("shortening a composition under its clip", WithMotion(),
            UpdateMotionCompositionOperation{Card(shorter)},
            EditError::ValidationFailed, "outside the 40 frames");
    Refused(
        "removing a prop an instance sets",
        WithMotion(Json(R"({"taille":90})")),
        UpdateMotionCompositionOperation{Card(
            R"({"width":1080,"height":1920,"fps":25,"duration":60,)"
            R"("root":{"type":"fill","uid":"01K80000000000000000000011"}})")},
        EditError::ValidationFailed, "UnknownProp");

    // What a motion clip cannot carry.
    ClipEffect effect;
    effect.type = "color.exposure";
    effect.params["amount"] = {1, 3};
    Refused("a colour effect on a motion clip", WithMotion(),
            SetClipEffectsOperation{kMotion, {effect}},
            EditError::ValidationFailed, "no colour effects");
    RemoveWordsOperation words;
    words.clip_id = kMotion;
    words.ranges = {{{10, 25}, {12, 25}}};
    Refused("removing words from a motion clip", WithMotion(), words,
            EditError::InvalidOperation, "no transcribed words");
}

void TestTwoClipsAndTheirHandles() {
    Document document = WithMotion();
    EditLog log;
    InsertMotionClipOperation second = Insertion({2, 25}, {20, 25}, {75, 25});
    second.clip_id = kSecondMotion;
    Check(Apply(document, log, second).ok, "a second motion clip applies");
    // Joining the halves of a cut restores the clip, and joining two
    // instances with different props is refused.
    Document cut = WithMotion(Json(R"({"titre":"A"})"));
    EditLog cutLog;
    Check(Apply(cut, cutLog, SplitClipOperation{kMotion, {40, 25}, {}, {}}).ok,
          "cut applies");
    const Ulid right = cut.FindTrack(kMotionTrack)->clips[1].id;
    Check(Apply(cut, cutLog,
                SetClipMotionPropsOperation{right, Json(R"({"titre":"B"})")})
              .ok,
          "the right half takes its own props");
    Refused("joining two instances with different props", cut,
            JoinClipOperation{kMotion, right, {{0, 25}, {50, 25}, {25, 25}}},
            EditError::InvalidOperation, "contiguous pieces");

    // A dissolve between two motion clips reads the composition around the
    // cut, and is refused past its frames.
    DocumentTransition transition;
    transition.track_id = kMotionTrack;
    transition.left_clip_id = kMotion;
    transition.right_clip_id = kSecondMotion;
    transition.duration = {10, 25};
    Refused("a dissolve past the composition's frames", document,
            AddTransitionOperation{transition, -1}, EditError::ValidationFailed,
            "handles");
    Document slipped = document;
    Check(
        Apply(slipped, log, SlipEditOperation{{kSecondMotion}, {5, 25}, {}}).ok,
        "the incoming clip slips to leave five frames before its cut");
    RoundTrip("a dissolve between two motion clips", slipped,
              AddTransitionOperation{transition, -1});
}

void TestHelpers() {
    const Value merged =
        MergeMotionProps(Json(R"({"titre":"A","taille":90})"),
                         Json(R"({"taille":null,"position":{"x":1,"y":2}})"));
    Check(merged.Dump() == R"({"titre":"A","position":{"x":1,"y":2}})",
          "merging keeps what is not named, removes null, appends new: " +
              merged.Dump());
    Check(MergeMotionProps(Json(R"({"titre":"A"})"), Json(R"({"titre":null})"))
              .IsNull(),
          "nothing left is null");
    const DocumentComposition card = Card();
    Check(MotionClipPictureKey(card, Value(), 1080, 1920) ==
                  MotionClipPictureKey(card, Value(), 1080, 1920) &&
              MotionClipPictureKey(card, Value(), 1080, 1920) !=
                  MotionClipPictureKey(card, Json(R"({"titre":"B"})"), 1080,
                                       1920) &&
              MotionClipPictureKey(card, Value(), 1080, 1920) !=
                  MotionClipPictureKey(card, Value(), 540, 960) &&
              MotionClipPictureKey(card, Value(), 1080, 1920) !=
                  MotionClipPictureKey(
                      Card(std::string(kDefinition)
                               .replace(std::string(kDefinition).find("Titre"),
                                        5, "Autre")),
                      Value(), 1080, 1920),
          "a picture key changes with the props, the size and the "
          "composition, and only with them");

    // ClipsEqual sees a change of instance props (a delta that missed it
    // would under-report the edit).
    const Document document = WithMotion();
    DocumentClip changed = Motion(document);
    changed.motion_props = Json(R"({"titre":"B"})");
    Check(!ClipsEqual(Motion(document), changed),
          "a change of instance props is a change of the clip");
}

// What the readers of a timeline make of a motion clip: overlays, not
// shots; nothing filmed to grade; no media to send to Resolve.
void TestConsumers() {
    const Document document = WithMotion();
    TimelineStats stats;
    std::string error;
    Check(CalculateTimelineStats(document, stats, error) &&
              stats.visible_shots == 1 && stats.visible_cuts == 0 &&
              stats.cutaway_plans == 0 && stats.motion_clips == 1,
          "timeline_stats: a title over the shot is no cut, counted apart: " +
              SerializeTimelineStats(stats));
    Check(
        SerializeTimelineStats(stats).find("\"motion_clips\":1") !=
                std::string::npos &&
            [] {
                TimelineStats plain;
                std::string ignored;
                CalculateTimelineStats(Base(), plain, ignored);
                return SerializeTimelineStats(plain).find("motion_clips") ==
                       std::string::npos;
            }(),
        "and only a timeline that has one says so");

    const std::string quality = DescribeShotQualityForAgent(
        document, {}, ShotQualityThresholds{}, ShotSegmentationSettings{});
    Check(quality.find("\"composition_id\":\"" + kComposition) !=
                  std::string::npos &&
              quality.find("drawn, not filmed") != std::string::npos,
          "list_shot_quality: a motion clip is ungraded, and says why: " +
              quality);

    TimelineSheetPlan contact;
    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact,
                                 TimelineSheetSettings{}, contact, error) &&
              contact.frames.size() == 1 &&
              contact.frames.front().clip_id == kClip,
          "contact_sheet: the shot under the title, not the title: " +
              SerializeTimelineSheetPlan(contact));
    TimelineSheetPlan cuts;
    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Cuts,
                                 TimelineSheetSettings{}, cuts, error) &&
              cuts.frames.empty(),
          "cut_sheet: a title posed over a shot makes no cut");

    ResolveTimelineExport exported;
    Check(!BuildResolveTimelineExport(document, exported, error) &&
              error.find("to validate in Resolve") != std::string::npos,
          "Resolve: a motion clip is refused, not promised: " + error);

    // A source frame of a motion clip is a frame of its composition.
    RationalTime position;
    Check(ResolveClipSourceFramePosition(document, kMotion, 20, position,
                                         error) &&
              position == RationalTime{45, 25},
          "split at a composition frame: frame 20 of the clip at 1 s is "
          "at 1.8 s: " +
              error);
}

// The monitor's policy: what a slot showing a motion clip asks for, how the
// picture is laid out for Renderer.mm, and what the cache keeps.
void TestPreviewPolicy() {
    Document document = WithMotion(Json(R"({"titre":"A"})"));
    const Timeline timeline(document);
    const std::optional<ResolvedFrame> onMedia =
        timeline.ResolveTrack(kVideo, RationalTime{30, 25});
    const std::optional<ResolvedFrame> onMotion =
        timeline.ResolveTrack(kMotionTrack, RationalTime{30, 25});
    MotionPreviewRequest request;
    Check(onMedia && !MotionPreviewFor(document, *onMedia, request),
          "a media slot asks nothing of the motion renderer");
    Check(onMotion && MotionPreviewFor(document, *onMotion, request) &&
              request.frame == 5 && request.composition != nullptr &&
              request.props.Dump() == R"({"titre":"A"})",
          "a motion slot asks for its composition's frame with its props");
    const std::string key = request.key;
    EditLog log;
    Check(Apply(document, log,
                SetClipMotionPropsOperation{kMotion, Json(R"({"titre":"B"})")})
              .ok,
          "props change");
    MotionPreviewRequest changed;
    Check(MotionPreviewFor(document,
                           *Timeline(document).ResolveTrack(
                               kMotionTrack, RationalTime{30, 25}),
                           changed) &&
              changed.key != key,
          "a props edit names another picture: the cache cannot show the "
          "old one");

    // Premultiplied RGBA to straight GBRA planes.
    const uint8_t rgba[] = {0,   0,   0,  0,    // transparent
                            255, 128, 0,  255,  // opaque orange
                            64,  64,  64, 128,  // half-covered grey
                            1,   0,   0,  1};   // faint red
    MotionPreviewImage image;
    MotionPreviewFromPremultiplied(rgba, 4, 1, image);
    const auto at = [&](int plane, int x) {
        return static_cast<int>(image.planes[(plane * 4) + x]);
    };
    // Planes: green, blue, red, alpha.
    Check(image.planes.size() == 16 && at(3, 0) == 0 && at(2, 0) == 0 &&
              at(2, 1) == 255 && at(0, 1) == 128 && at(1, 1) == 0 &&
              at(3, 1) == 255 && at(2, 2) == 128 && at(0, 2) == 128 &&
              at(1, 2) == 128 && at(3, 2) == 128 && at(2, 3) == 255 &&
              at(3, 3) == 1,
          "planes are green, blue, red, alpha, straight");

    // The cache keeps the most recent pictures within its bytes.
    MotionPreviewCache cache(3 * 4 * 16);
    const auto picture = [](uint8_t value) {
        MotionPreviewImage made;
        made.width = 4;
        made.height = 4;
        made.planes.assign(64, value);
        return made;
    };
    cache.Insert("a", 0, picture(1));
    cache.Insert("a", 1, picture(2));
    cache.Insert("b", 0, picture(3));
    Check(cache.Find("a", 0) != nullptr, "a hit");
    cache.Insert("b", 1, picture(4));
    Check(cache.Count() == 3 && cache.Bytes() == 192 &&
              cache.Find("a", 1) == nullptr && cache.Find("a", 0) != nullptr &&
              cache.Find("b", 1)->planes.front() == 4,
          "the least recently used picture leaves first");
    MotionPreviewCache tiny(10);
    Check(tiny.Insert("c", 0, picture(5)).planes.front() == 5 &&
              tiny.Count() == 1,
          "a cache smaller than a frame still keeps the current one");

    // What the conversion costs on a 1080x1920 frame.
    std::vector<uint8_t> frame(static_cast<size_t>(1080) * 1920 * 4, 0);
    for (size_t index = 0; index < frame.size(); index += 4) {
        frame[index + 3] = static_cast<uint8_t>(index % 255);
        frame[index] = static_cast<uint8_t>(frame[index + 3] / 2);
    }
    std::vector<double> times;
    for (int pass = 0; pass < 9; ++pass) {
        const auto started = std::chrono::steady_clock::now();
        MotionPreviewFromPremultiplied(frame.data(), 1080, 1920, image);
        times.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count());
    }
    std::sort(times.begin(), times.end());
    std::cout << "preview planes 1080x1920: median " << times[4] << " ms\n";
}

}  // namespace

int main() {
    const auto run = [](const char* name, void (*test)()) {
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
    };
    run("schema and version", TestSchemaAndVersion);
    run("operations round-trip", TestOperationsRoundTrip);
    run("time follows the clip", TestTimeFollowsTheClip);
    run("refusals", TestRefusals);
    run("two clips and their handles", TestTwoClipsAndTheirHandles);
    run("helpers", TestHelpers);
    run("consumers", TestConsumers);
    run("preview policy", TestPreviewPolicy);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
