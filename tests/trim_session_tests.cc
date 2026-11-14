// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode (TrimSession.h):
// which edit point is selected and on which side, the plans each gesture
// makes -- compared to the exact operation of Operations.h it must be --
// what EditLog::Apply then does with them, the J/K/L dynamic trim on a
// simulated clock, Escape, the refusals and the Trim Monitor.
//
// Plain C++: no AppKit, no FFmpeg, no real clock.

#include "TrimBar.h"
#include "TrimSession.h"

#include "EditLog.h"
#include "Operations.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

using trim::DescribeTrimMonitor;
using trim::TrimEdit;
using trim::TrimMonitorView;
using trim::TrimPoint;
using trim::TrimRefusal;
using trim::TrimSession;
using trim::TrimSide;
using trim::TrimStyle;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (before == failures) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

const std::string kSourceOne = "01KTR000000000000000000S01";
const std::string kSourceTwo = "01KTR000000000000000000S02";
const std::string kV1 = "01KTR000000000000000000T01";
const std::string kA1 = "01KTR000000000000000000T02";
const std::string kV2 = "01KTR000000000000000000T03";
const std::string kClipA = "01KTR000000000000000000C0A";
const std::string kClipB = "01KTR000000000000000000C0B";
const std::string kClipC = "01KTR000000000000000000C0C";
const std::string kSoundA = "01KTR000000000000000000C1A";
const std::string kSoundB = "01KTR000000000000000000C1B";
const std::string kSoundC = "01KTR000000000000000000C1C";
const std::string kCutaway = "01KTR000000000000000000C2A";
const std::string kLinkA = "01KTR000000000000000000G0A";
const std::string kLinkB = "01KTR000000000000000000G0B";
const std::string kLinkC = "01KTR000000000000000000G0C";

RationalTime F(int64_t frames) { return {frames, 25}; }

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  int64_t sourceIn, int64_t duration, int64_t timelineIn,
                  const std::string& link = {}) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.source_in = F(sourceIn);
    clip.duration = F(duration);
    clip.timeline_in = F(timelineIn);
    clip.include_audio = false;
    clip.link_group_id = link;
    return clip;
}

// 25 i/s. Two rushes of 100 frames.
//   V1: A [0,20) reads 10..30 of one, B [20,40) reads 10..30 of two,
//       C [40,50) reads 50..60 of one;
//   A1: their linked sounds, same times;
//   V2: a cutaway [25,30), unlinked, sync-locked.
Document Fixture() {
    Document document;
    document.sources = {{kSourceOne, "one.mov", {25, 1}, F(100)},
                        {kSourceTwo, "two.mov", {25, 1}, F(100)}};
    document.sequence.frame_rate = {25, 1};
    DocumentTrack v1{kV1, "video", 0, {}};
    v1.clips = {Clip(kClipA, kSourceOne, 10, 20, 0, kLinkA),
                Clip(kClipB, kSourceTwo, 10, 20, 20, kLinkB),
                Clip(kClipC, kSourceOne, 50, 10, 40, kLinkC)};
    DocumentTrack a1{kA1, "audio", 1, {}};
    a1.clips = {Clip(kSoundA, kSourceOne, 10, 20, 0, kLinkA),
                Clip(kSoundB, kSourceTwo, 10, 20, 20, kLinkB),
                Clip(kSoundC, kSourceOne, 50, 10, 40, kLinkC)};
    DocumentTrack v2{kV2, "video", 2, {}};
    v2.clips = {Clip(kCutaway, kSourceTwo, 60, 5, 25)};
    document.sequence.tracks = {v1, a1, v2};
    return document;
}

bool Valid(const Document& document) {
    std::string error;
    const bool valid = document.Validate(error);
    if (!valid) std::cerr << "  fixture invalid: " << error << '\n';
    return valid;
}

std::string Bytes(const Operation& operation) {
    return SerializeOperation(operation);
}

bool Times(const Document& document, const std::string& clipId,
           int64_t sourceIn, int64_t duration, int64_t timelineIn) {
    const DocumentClip* clip = document.FindClip(clipId);
    return clip != nullptr && clip->source_in == F(sourceIn) &&
           clip->duration == F(duration) && clip->timeline_in == F(timelineIn);
}

// The clock the window would read: nanoseconds of uptime, as an exact time.
RationalTime Clock(int64_t milliseconds) { return {milliseconds, 1000}; }

// Applies `edit` through a log, then checks that one Undo restores the
// document byte for byte and one Redo replays it.
void CheckOneUndoStep(const TrimEdit& edit, const Document& original,
                      Document& document, EditLog& log,
                      const std::string& what) {
    EditError error = EditError::None;
    std::string message;
    Check(log.Undo(document, error, message),
          what + ": one Undo reverts it (" + message + ")");
    Check(document.SaveToString() == original.SaveToString(),
          what + ": the undo restores the document byte for byte");
    Check(log.Redo(document, error, message),
          what + ": one Redo replays it (" + message + ")");
    (void)edit;
}

}  // namespace

int main() {
    Test("the fixture is a valid document",
         [] { Check(Valid(Fixture()), "valid"); });

    Test("the nearest roll point on a targeted track", [] {
        const Document document = Fixture();
        TrimSession session;
        Check(session.SelectNearest(document, F(18), {kV1}, TrimSide::Roll) ==
                  TrimRefusal::None,
              "a roll point is found near 18");
        Check(session.Points().size() == 1 &&
                  session.Points()[0].outgoing_clip_id == kClipA &&
                  session.Points()[0].incoming_clip_id == kClipB &&
                  session.Points()[0].side == TrimSide::Roll,
              "the cut A|B, addressed by the two clips' ULIDs");
        Check(session.SelectNearest(document, F(30), {kV1}, TrimSide::Roll) ==
                      TrimRefusal::None &&
                  session.Points()[0].outgoing_clip_id == kClipA,
              "halfway between 20 and 40, the earlier cut wins");
        Check(session.SelectNearest(document, F(48), {kV1}, TrimSide::Roll) ==
                      TrimRefusal::None &&
                  session.Points()[0].outgoing_clip_id == kClipB,
              "the free tail at 50 is no roll point: B|C at 40 is chosen");
        Check(session.SelectNearest(document, F(48), {kV1}, TrimSide::A) ==
                      TrimRefusal::None &&
                  session.Points()[0].outgoing_clip_id == kClipC &&
                  session.Points()[0].incoming_clip_id.empty(),
              "an A side takes the free tail of C at 50");
        Check(session.SelectNearest(document, F(1), {kV1}, TrimSide::B) ==
                      TrimRefusal::None &&
                  session.Points()[0].incoming_clip_id == kClipA &&
                  session.Points()[0].outgoing_clip_id.empty(),
              "a B side takes the free head of A at 0");
    });

    Test(
        "no target means every unlocked track; none found leaves the "
        "selection",
        [] {
            Document document = Fixture();
            TrimSession session;
            Check(session.SelectNearest(document, F(26), {}, TrimSide::A) ==
                      TrimRefusal::None,
                  "a point per track");
            Check(session.Points().size() == 3 &&
                      session.Points()[0].outgoing_clip_id == kClipA &&
                      session.Points()[1].outgoing_clip_id == kSoundA &&
                      session.Points()[2].outgoing_clip_id == kCutaway,
                  "V1 and A1 at 20, the cutaway's tail at 30 on V2");
            document.FindTrack(kA1)->locked = true;
            Check(session.SelectNearest(document, F(26), {}, TrimSide::A) ==
                          TrimRefusal::None &&
                      session.Points().size() == 2,
                  "a locked track is skipped");
            Check(
                session.SelectNearest(document, F(26), {kV2}, TrimSide::Roll) ==
                    TrimRefusal::NoEditPoint,
                "the cutaway has no junction: NoEditPoint");
            Check(session.Points().size() == 2,
                  "a refused selection leaves the previous one");
        });

    Test("selecting an edge, adding and toggling a point", [] {
        const Document document = Fixture();
        TrimSession session;
        Check(session.SelectEdge(document, kClipA, TrimEdge::Tail,
                                 TrimSide::Roll, false) == TrimRefusal::None,
              "A's tail as a roll");
        Check(session.SelectEdge(document, kClipC, TrimEdge::Head, TrimSide::B,
                                 true) == TrimRefusal::None &&
                  session.Points().size() == 2 &&
                  session.Points()[1].outgoing_clip_id == kClipB &&
                  session.Points()[1].incoming_clip_id == kClipC,
              "C's head added: the point B|C, B side");
        Check(session.SelectEdge(document, kClipB, TrimEdge::Tail,
                                 TrimSide::Roll, true) == TrimRefusal::None &&
                  session.Points().size() == 1,
              "the same cut added again is removed");
        Check(
            session.SelectEdge(document, kClipC, TrimEdge::Tail, TrimSide::Roll,
                               false) == TrimRefusal::NoJunction,
            "a roll at a free tail is NoJunction");
        Check(session.SelectEdge(document, kClipA, TrimEdge::Head, TrimSide::A,
                                 false) == TrimRefusal::NoJunction,
              "an A side at a free head is NoJunction");
        Check(session.SelectEdge(document, "01KTR00000000000000000ZZZZ",
                                 TrimEdge::Head, TrimSide::B,
                                 false) == TrimRefusal::NoEditPoint,
              "an unknown clip selects nothing");
    });

    Test("cycling the side skips what an edge does not have", [] {
        const Document document = Fixture();
        TrimSession session;
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        session.SelectEdge(document, kClipC, TrimEdge::Tail, TrimSide::A, true);
        Check(session.CycleSide(document) == TrimRefusal::None &&
                  session.Points()[0].side == TrimSide::Roll &&
                  session.Points()[1].side == TrimSide::A,
              "A -> Roll at the cut; the free tail stays A");
        session.CycleSide(document);
        Check(session.Points()[0].side == TrimSide::B, "Roll -> B");
        session.CycleSide(document);
        Check(session.Points()[0].side == TrimSide::A, "B -> A");
    });

    // ON8 -- docs/roadmap/ONGLETS-2026-10.md. A side chosen outright, and
    // the bar that shows the trim mode with its keys.
    Test("a side is chosen outright, or refused for every point", [] {
        const Document document = Fixture();
        TrimSession session;
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        Check(session.SetSide(document, TrimSide::B) == TrimRefusal::None &&
                  session.Points()[0].side == TrimSide::B,
              "the cut after A takes B");
        session.SelectEdge(document, kClipC, TrimEdge::Tail, TrimSide::A, true);
        Check(!session.CanSetSide(document, TrimSide::Roll) &&
                  session.SetSide(document, TrimSide::Roll) ==
                      TrimRefusal::NoJunction &&
                  session.Points()[0].side == TrimSide::B &&
                  session.Points()[1].side == TrimSide::A,
              "a roll refused because the free tail has no clip after it, "
              "and no point changed");
        Check(session.SetSide(document, TrimSide::A) == TrimRefusal::None &&
                  session.Points()[0].side == TrimSide::A &&
                  session.Points()[1].side == TrimSide::A,
              "A, which both have, is taken by both");
        TrimSession empty;
        Check(empty.SetSide(document, TrimSide::A) == TrimRefusal::NoEditPoint,
              "nothing selected, nothing chosen");
    });

    Test("the trim bar shows the mode, its side and its keys", [] {
        const Document document = Fixture();
        const auto keyFor = [](const std::string& command) -> std::string {
            if (command == "trim.select") return "V";
            if (command == "trim.side") return "U";
            if (command == "trim.nudge.back") return ",";
            if (command == "trim.nudge.forward.many") return "⇧.";
            return "";
        };
        TrimSession session;
        Check(!trim::DescribeTrimBar(session, document, false, false, keyFor)
                   .shown,
              "hidden with the Selection tool and no point");
        const trim::TrimBarModel idle =
            trim::DescribeTrimBar(session, document, true, false, keyFor);
        Check(idle.shown && idle.buttons.size() == 1 &&
                  idle.buttons[0].action == "select" &&
                  idle.buttons[0].help.find("· V") != std::string::npos,
              "with the Trim tool and no point: take the nearest edit, V");
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::Roll,
                           false);
        const trim::TrimBarModel held =
            trim::DescribeTrimBar(session, document, false, false, keyFor);
        std::string actions;
        for (const auto& button : held.buttons) actions += button.action + ' ';
        Check(held.shown && held.label == "TRIM · ripple" &&
                  actions ==
                      "side.a side.roll side.b nudge.-5 nudge.-1 nudge.+1 "
                      "nudge.+5 extend dynamic play.around leave ",
              "a point held: the sides, the nudges, Extend, the dynamic "
              "trim and the way out (" +
                  actions + ")");
        Check(held.buttons[1].on && !held.buttons[0].on && !held.buttons[2].on,
              "Roll is the side taken");
        Check(held.buttons[4].help == "Une image en arrière · ," &&
                  held.buttons[6].help == "Cinq images en avant · ⇧." &&
                  held.buttons[3].help == "Cinq images en arrière",
              "each button carries its key, and none when it has none");
        session.SetStyle(TrimStyle::Trim);
        session.SelectEdge(document, kClipC, TrimEdge::Tail, TrimSide::A, true);
        const trim::TrimBarModel two =
            trim::DescribeTrimBar(session, document, true, true, keyFor);
        Check(two.label == "TRIM · simple · 2 points" &&
                  !two.buttons[1].enabled && two.buttons[8].on,
              "two points in a plain trim, Roll out of reach for the free "
              "tail, the dynamic trim on");
    });

    Test("a roll by +1 frame is RollEditOperation, and undoes", [] {
        const Document original = Fixture();
        Document document = original;
        TrimSession session;
        session.SetFollowLinks(false);
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        const TrimEdit edit = session.PlanNudge(document, 1);
        Check(edit.ok(), std::string("planned: ") +
                             trim::TrimRefusalName(edit.refusal) + " " +
                             edit.detail);
        if (!edit.ok()) return;
        Check(Bytes(*edit.operation) ==
                  Bytes(RollEditOperation{{{kClipA, kClipB}}, F(1), {}}),
              "exactly RollEdit(A|B, +1/25)");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(edit, document, log, error, message),
              "applied: " + message);
        Check(Times(document, kClipA, 10, 21, 0) &&
                  Times(document, kClipB, 11, 19, 21) &&
                  Times(document, kSoundA, 10, 20, 0),
              "A ends at 21, B starts at 21 reading from 11; links off: the "
              "sound stays");
        Check(log.AppliedCount() == 1, "one journal entry");
        CheckOneUndoStep(edit, original, document, log, "roll +1");
    });

    Test("a roll with linked selection rolls the sound's cut too", [] {
        Document document = Fixture();
        TrimSession session;
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        const TrimEdit edit = session.PlanNudge(document, -1);
        Check(edit.ok() &&
                  Bytes(*edit.operation) ==
                      Bytes(RollEditOperation{
                          {{kClipA, kClipB}, {kSoundA, kSoundB}}, F(-1), {}}),
              "one RollEdit with the picture pair and the sound pair");
    });

    Test("a ripple of the A side by -5 frames, sync tracks following", [] {
        const Document original = Fixture();
        Document document = original;
        TrimSession session;
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        const TrimEdit edit =
            session.PlanNudge(document, -trim::kLargeTrimFrames);
        Check(edit.ok(), std::string("planned: ") +
                             trim::TrimRefusalName(edit.refusal) + " " +
                             edit.detail);
        if (!edit.ok()) return;
        Check(Bytes(*edit.operation) ==
                  Bytes(RippleTrimOperation{
                      kClipA, TrimEdge::Tail, F(-5), {kSoundA}, {kV2}, {}}),
              "exactly RippleTrim(A, tail, -5/25, linked [a], sync [V2])");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(edit, document, log, error, message),
              "applied: " + message);
        Check(Times(document, kClipA, 10, 15, 0) &&
                  Times(document, kSoundA, 10, 15, 0) &&
                  Times(document, kClipB, 10, 20, 15) &&
                  Times(document, kSoundB, 10, 20, 15) &&
                  Times(document, kClipC, 50, 10, 35) &&
                  Times(document, kCutaway, 60, 5, 20),
              "A and its sound lose 5 frames; everything after, the cutaway "
              "included, comes back 5");
        CheckOneUndoStep(edit, original, document, log, "ripple A -5");
    });

    Test("a ripple of the B side by +1 frame", [] {
        Document document = Fixture();
        TrimSession session;
        session.SelectEdge(document, kClipB, TrimEdge::Head, TrimSide::B,
                           false);
        const TrimEdit edit = session.PlanNudge(document, 1);
        Check(edit.ok() &&
                  Bytes(*edit.operation) ==
                      Bytes(RippleTrimOperation{
                          kClipB, TrimEdge::Head, F(1), {kSoundB}, {kV2}, {}}),
              "exactly RippleTrim(B, head, +1/25, linked [b], sync [V2])");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(edit, document, log, error, message) &&
                  Times(document, kClipB, 11, 19, 20) &&
                  Times(document, kClipC, 50, 10, 39),
              "B reads from 11 and stays at 20; C comes back one frame");
    });

    Test("plain trims: TrimClip, TrimLinkedClips, and the neighbour", [] {
        Document document = Fixture();
        TrimSession session;
        session.SetStyle(TrimStyle::Trim);
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        const TrimEdit blocked = session.PlanNudge(document, 1);
        Check(blocked.refusal == TrimRefusal::BlockedByNeighbour &&
                  blocked.refused_clip_id == kClipA,
              "a plain trim of A into B is BlockedByNeighbour");
        const TrimEdit linked = session.PlanNudge(document, -1);
        Check(linked.ok() && Bytes(*linked.operation) ==
                                 Bytes(TrimLinkedClipsOperation{
                                     kLinkA,
                                     {{kClipA, TrimEdge::Tail, F(-1)},
                                      {kSoundA, TrimEdge::Tail, F(-1)}},
                                     {}}),
              "with links: TrimLinkedClips of A and its sound");
        session.SetFollowLinks(false);
        const TrimEdit alone = session.PlanNudge(document, -1);
        Check(alone.ok() && Bytes(*alone.operation) ==
                                Bytes(TrimClipOperation{kClipA, TrimEdge::Tail,
                                                        F(-1), std::nullopt}),
              "without links: TrimClip of A");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(alone, document, log, error, message) &&
                  Times(document, kClipA, 10, 19, 0) &&
                  Times(document, kClipB, 10, 20, 20),
              "a one-frame gap opens; B does not move");
        session.SelectEdge(document, kClipA, TrimEdge::Head, TrimSide::B,
                           false);
        Check(session.PlanNudge(document, -1).refusal ==
                  TrimRefusal::BlockedByNeighbour,
              "a plain trim of a head before 0 is refused");
    });

    Test("refusals: media handle, length, stale point, locked track", [] {
        Document document = Fixture();
        TrimSession session;
        session.SetFollowLinks(false);
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        const TrimEdit handle = session.PlanNudge(document, -11);
        Check(handle.refusal == TrimRefusal::MediaLimit &&
                  handle.refused_clip_id == kClipB,
              "B has 10 frames before its In: a roll of -11 is MediaLimit");
        Check(session.PlanNudge(document, -10).ok(),
              "a roll of -10 uses the whole handle");
        Check(session.PlanNudge(document, 20).refusal ==
                  TrimRefusal::ClipTooShort,
              "a roll of +20 would leave B empty: ClipTooShort");
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        Check(session.PlanNudge(document, 70).ok() &&
                  session.PlanNudge(document, 71).refusal ==
                      TrimRefusal::MediaLimit,
              "A reads to 30 of 100: +70 holds, +71 is MediaLimit");

        // The selection outlives the clip: an undo, another surface.
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation removal = RemoveClipOperation{kClipA, {}, {}};
        Check(log.Apply(document, removal, error, message), message);
        const TrimEdit stale = session.PlanNudge(document, 1);
        Check(stale.refusal == TrimRefusal::StaleEditPoint &&
                  stale.refused_clip_id == kClipA,
              "a removed clip makes the point StaleEditPoint");

        Document locked = Fixture();
        TrimSession linked;
        linked.SelectEdge(locked, kClipA, TrimEdge::Tail, TrimSide::A, false);
        locked.FindTrack(kA1)->locked = true;
        const TrimEdit partner = linked.PlanNudge(locked, -1);
        Check(partner.refusal == TrimRefusal::LockedTrack &&
                  partner.refused_clip_id == kSoundA,
              "the linked sound on a locked track: LockedTrack");
        Check(linked.SelectEdge(locked, kSoundA, TrimEdge::Tail, TrimSide::A,
                                false) == TrimRefusal::LockedTrack,
              "an edge of a locked track is not selected");

        // The cutaway's track is sync-locked and locked: a ripple of V1
        // would leave it behind, out of sync with the words. Refused, as
        // the drag and ripple_trim refuse it.
        Document lockedSync = Fixture();
        lockedSync.FindTrack(kV2)->locked = true;
        TrimSession ripple;
        ripple.SetStyle(TrimStyle::Ripple);
        ripple.SelectEdge(lockedSync, kClipA, TrimEdge::Tail, TrimSide::A,
                          false);
        Check(ripple.PlanNudge(lockedSync, -5).refusal ==
                  TrimRefusal::LockedTrack,
              "a ripple that would leave a locked sync-locked track behind: "
              "LockedTrack");
        ripple.SetStyle(TrimStyle::Trim);
        Check(ripple.PlanNudge(lockedSync, -5).ok(),
              "a plain trim moves nothing downstream: the locked track does "
              "not matter");
        TrimSession none;
        Check(none.PlanNudge(locked, 1).refusal == TrimRefusal::NoEditPoint,
              "nothing selected: NoEditPoint");
        Document noRate = Fixture();
        noRate.sequence.frame_rate = {0, 1};
        TrimSession rate;
        rate.SelectEdge(noRate, kClipA, TrimEdge::Tail, TrimSide::A, false);
        Check(
            rate.PlanNudge(noRate, 1).refusal == TrimRefusal::InvalidFrameRate,
            "a sequence without a rate: InvalidFrameRate");
    });

    Test("several points are one batch, one undo step", [] {
        const Document original = Fixture();
        Document document = original;
        TrimSession session;
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        session.SelectEdge(document, kClipB, TrimEdge::Tail, TrimSide::A, true);
        const TrimEdit edit = session.PlanNudge(document, 1);
        Check(edit.ok(), std::string("planned: ") +
                             trim::TrimRefusalName(edit.refusal) + " " +
                             edit.detail);
        if (!edit.ok()) return;
        const BatchOperation* batch =
            std::get_if<BatchOperation>(&*edit.operation);
        Check(
            batch != nullptr && batch->operations.size() == 2 &&
                Bytes(batch->operations[0]) ==
                    Bytes(RippleTrimOperation{
                        kClipA, TrimEdge::Tail, F(1), {kSoundA}, {kV2}, {}}) &&
                Bytes(batch->operations[1]) ==
                    Bytes(RippleTrimOperation{
                        kClipB, TrimEdge::Tail, F(1), {kSoundB}, {kV2}, {}}),
            "a batch of the two ripples, each with its sound and V2");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(edit, document, log, error, message),
              "applied: " + message);
        Check(Times(document, kClipA, 10, 21, 0) &&
                  Times(document, kClipB, 10, 21, 21) &&
                  Times(document, kClipC, 50, 10, 42) &&
                  Times(document, kSoundC, 50, 10, 42) &&
                  Times(document, kCutaway, 60, 5, 26),
              "both cuts later by one; C two frames later; the cutaway "
              "follows the first cut only, as its place in time says");
        Check(log.AppliedCount() == 1, "one journal entry for two points");
        CheckOneUndoStep(edit, original, document, log, "two points");
    });

    Test("the same cut on two linked tracks is trimmed once", [] {
        Document document = Fixture();
        TrimSession session;
        session.SelectNearest(document, F(20), {kV1, kA1}, TrimSide::A);
        Check(session.Points().size() == 2, "a point on V1 and one on A1");
        const TrimEdit edit = session.PlanNudge(document, -1);
        Check(edit.ok() &&
                  std::holds_alternative<RippleTrimOperation>(*edit.operation),
              "the sound's point is the picture's linked partner: one "
              "RippleTrim, no batch");
        session.SetFollowLinks(false);
        const TrimEdit apart = session.PlanNudge(document, -1);
        const BatchOperation* batch =
            apart.ok() ? std::get_if<BatchOperation>(&*apart.operation)
                       : nullptr;
        Check(batch != nullptr && batch->operations.size() == 2 &&
                  Bytes(batch->operations[1]) ==
                      Bytes(RippleTrimOperation{
                          kSoundA, TrimEdge::Tail, F(-1), {}, {}, {}}),
              "links off: two ripples; only the lead track's carries the "
              "sync tracks");
        Document applied = document;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(apart.ok() &&
                  session.Apply(apart, applied, log, error, message) &&
                  Times(applied, kClipB, 10, 20, 19) &&
                  Times(applied, kSoundB, 10, 20, 19) &&
                  Times(applied, kCutaway, 60, 5, 24),
              "picture, sound and cutaway each move back once: " + message);
    });

    Test("extend to the playhead", [] {
        Document document = Fixture();
        TrimSession session;
        session.SetFollowLinks(false);
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        const TrimEdit roll = session.PlanExtendToPlayhead(document, F(23));
        Check(roll.ok() &&
                  Bytes(*roll.operation) ==
                      Bytes(RollEditOperation{{{kClipA, kClipB}}, F(3), {}}),
              "the cut rolls to the playhead: RollEdit +3/25");
        Check(session.PlanExtendToPlayhead(document, F(20)).refusal ==
                  TrimRefusal::NoMovement,
              "a playhead on the cut moves nothing: NoMovement");
        session.SelectEdge(document, kClipB, TrimEdge::Head, TrimSide::B,
                           false);
        const TrimEdit ripple = session.PlanExtendToPlayhead(document, F(23));
        Check(ripple.ok() &&
                  Bytes(*ripple.operation) ==
                      Bytes(RippleTrimOperation{
                          kClipB, TrimEdge::Head, F(3), {}, {kA1, kV2}, {}}),
              "a B side ripples its head by playhead - cut; links off, the "
              "sound is a sync track like the cutaway");
        // Two points at different times: each by its own distance.
        session.SetStyle(TrimStyle::Trim);
        session.SelectEdge(document, kClipC, TrimEdge::Tail, TrimSide::A,
                           false);
        session.SelectEdge(document, kCutaway, TrimEdge::Tail, TrimSide::A,
                           true);
        const TrimEdit both = session.PlanExtendToPlayhead(document, F(45));
        const BatchOperation* batch =
            both.ok() ? std::get_if<BatchOperation>(&*both.operation) : nullptr;
        Check(batch != nullptr && batch->operations.size() == 2 &&
                  Bytes(batch->operations[0]) ==
                      Bytes(TrimClipOperation{kClipC, TrimEdge::Tail, F(-5),
                                              std::nullopt}) &&
                  Bytes(batch->operations[1]) ==
                      Bytes(TrimClipOperation{kCutaway, TrimEdge::Tail, F(15),
                                              std::nullopt}),
              "C's tail -5 to 45, the cutaway's +15 to 45, in one batch");
        // In ripple, the first would move the second off its distance.
        session.SetStyle(TrimStyle::Ripple);
        Check(session.PlanExtendToPlayhead(document, F(45)).refusal ==
                  TrimRefusal::Rejected,
              "in ripple, two points do not extend together: Rejected");
    });

    Test("dynamic trim: L, doubling, the limit, and K commits once", [] {
        const Document original = Fixture();
        Document document = original;
        TrimSession session;
        session.SetFollowLinks(false);
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        Check(session.Shuttle(document, 1, Clock(0)) == TrimRefusal::None &&
                  session.Dynamic().active && session.Dynamic().speed == 1,
              "L starts at 1x");
        session.Tick(document, Clock(500));
        Check(session.Dynamic().offset_frames == 12,
              "after 0.5 s at 25 i/s: floor(12.5) = 12 frames");
        Document preview;
        Check(session.PreviewDocument(document, preview) &&
                  Times(preview, kClipA, 10, 32, 0) &&
                  Times(preview, kClipB, 22, 8, 32),
              "the preview shows the cut at 32");
        Check(document.SaveToString() == original.SaveToString(),
              "the document itself is untouched while J/K/L runs");
        session.Shuttle(document, 1, Clock(500));
        Check(session.Dynamic().speed == 2 &&
                  session.Dynamic().offset_frames == 12,
              "L again doubles to 2x, from the offset reached");
        session.Tick(document, Clock(560));
        Check(session.Dynamic().offset_frames == 15,
              "60 ms at 2x: floor(3.0) = 3 more frames");
        session.Shuttle(document, 1, Clock(560));
        session.Shuttle(document, 1, Clock(560));
        session.Shuttle(document, 1, Clock(560));
        Check(session.Dynamic().speed == trim::kMaxShuttleSpeed,
              "doubling stops at 8x");
        session.Tick(document, Clock(10000));
        Check(session.Dynamic().offset_frames == 19 &&
                  session.Dynamic().at_limit && session.Dynamic().speed == 0,
              "B keeps one frame: the roll holds at +19, at its limit");
        const TrimMonitorView monitor = DescribeTrimMonitor(preview, session);
        Check(monitor.out_shift_frames == 19 && monitor.in_shift_frames == 19,
              "the monitor's counters include the dynamic offset");
        const TrimEdit edit = session.StopDynamic(document, Clock(10040));
        Check(!session.Dynamic().active, "K ends the dynamic trim");
        Check(edit.ok() &&
                  Bytes(*edit.operation) ==
                      Bytes(RollEditOperation{{{kClipA, kClipB}}, F(19), {}}),
              "K plans ONE RollEdit of the 19 frames reached");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(edit, document, log, error, message) &&
                  log.AppliedCount() == 1 && Times(document, kClipB, 29, 1, 39),
              "one journal entry; B is its last frame: " + message);
        CheckOneUndoStep(edit, original, document, log, "dynamic roll");
    });

    Test("dynamic trim: J reverses, held at a limit, and resumes", [] {
        Document document = Fixture();
        TrimSession session;
        session.SetFollowLinks(false);
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        session.Shuttle(document, 1, Clock(0));
        session.Tick(document, Clock(200));
        Check(session.Dynamic().offset_frames == 5, "0.2 s forward: +5");
        session.Shuttle(document, -1, Clock(200));
        Check(session.Dynamic().speed == -1, "J reverses at 1x");
        session.Tick(document, Clock(400));
        Check(session.Dynamic().offset_frames == 0, "0.2 s back: 0");
        session.Tick(document, Clock(2000));
        Check(session.Dynamic().offset_frames == -10 &&
                  session.Dynamic().at_limit,
              "B's 10-frame handle stops it at -10");
        session.Shuttle(document, -1, Clock(2000));
        Check(session.Dynamic().speed == -1 && !session.Dynamic().at_limit,
              "at a limit, J again restarts at 1x instead of doubling");
        session.Shuttle(document, 1, Clock(2000));
        session.Tick(document, Clock(2080));
        Check(session.Dynamic().offset_frames == -8,
              "L from the limit: 80 ms forward is +2");
        Check(session.CycleSide(document) == TrimRefusal::Rejected,
              "the side does not change during a dynamic trim");
    });

    Test("dynamic trim: Escape writes nothing, K at zero writes nothing", [] {
        const Document original = Fixture();
        Document document = original;
        TrimSession session;
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        session.Shuttle(document, 1, Clock(0));
        session.Tick(document, Clock(300));
        Check(session.Dynamic().offset_frames == 7, "0.3 s: +7");
        Check(session.CancelDynamic(), "Escape cancels a running trim");
        Check(!session.Dynamic().active && session.Active(),
              "the dynamic trim is gone, the point stays selected");
        Check(document.SaveToString() == original.SaveToString(),
              "nothing was written");
        Check(session.StopDynamic(document, Clock(400)).refusal ==
                  TrimRefusal::NoMovement,
              "K afterwards has nothing to commit");
        Check(!session.CancelDynamic(),
              "Escape with no dynamic trim says so: the window leaves trim "
              "mode");
        session.Shuttle(document, 1, Clock(1000));
        const TrimEdit nothing = session.StopDynamic(document, Clock(1030));
        Check(nothing.refusal == TrimRefusal::NoMovement,
              "30 ms is less than a frame: K commits nothing");
        Check(session.Points()[0].out_shift == RationalTime{0, 1},
              "and the counters stay at zero");
    });

    Test("dynamic trim counts frames exactly at 30000/1001", [] {
        Document document;
        document.sources = {
            {kSourceOne, "ntsc.mov", {30000, 1001}, {1001 * 300, 30000}}};
        document.sequence.frame_rate = {30000, 1001};
        DocumentTrack v1{kV1, "video", 0, {}};
        DocumentClip a;
        a.id = kClipA;
        a.source_id = kSourceOne;
        a.source_in = {1001 * 10, 30000};
        a.duration = {1001 * 50, 30000};
        a.timeline_in = {0, 30000};
        a.include_audio = false;
        DocumentClip b = a;
        b.id = kClipB;
        b.timeline_in = {1001 * 50, 30000};
        b.source_in = {1001 * 100, 30000};
        v1.clips = {a, b};
        document.sequence.tracks = {v1};
        Check(Valid(document), "the NTSC fixture is valid");
        TrimSession session;
        session.SelectNearest(document, {1001 * 50, 30000}, {}, TrimSide::Roll);
        // The clock at the frame cadence itself: 10 frames is 10010/30000 s.
        session.Shuttle(document, 1, {0, 30000});
        session.Tick(document, {1001 * 10 - 1, 30000});
        Check(session.Dynamic().offset_frames == 9,
              "one tick short of 10 frames is 9");
        session.Tick(document, {1001 * 10, 30000});
        Check(session.Dynamic().offset_frames == 10, "exactly 10 frames");
        const TrimEdit edit = session.StopDynamic(document, {1001 * 10, 30000});
        Check(edit.ok() && Bytes(*edit.operation) ==
                               Bytes(RollEditOperation{
                                   {{kClipA, kClipB}}, {1001 * 10, 30000}, {}}),
              "the committed roll is 10 frames of 1001/30000");
    });

    Test("the Trim Monitor: last frame of A, first of B, the counters", [] {
        Document document = Fixture();
        TrimSession session;
        session.SetFollowLinks(false);
        Check(!DescribeTrimMonitor(document, session).active,
              "inactive without a point");
        session.SelectNearest(document, F(20), {kV1}, TrimSide::Roll);
        TrimMonitorView view = DescribeTrimMonitor(document, session);
        Check(view.active && view.side == TrimSide::Roll &&
                  view.point_count == 1 && view.edit_time == F(20),
              "active, roll, the cut at 20");
        Check(view.outgoing.present && view.outgoing.clip_id == kClipA &&
                  view.outgoing.track_id == kV1 &&
                  view.outgoing.timeline_time == F(19) &&
                  view.outgoing.has_source &&
                  view.outgoing.source_time == F(29) &&
                  view.outgoing.source_frame == 29 &&
                  view.outgoing.handle == F(70),
              "A shows its last frame, 19 on the timeline, source 29, with "
              "70 frames of handle");
        Check(view.incoming.present && view.incoming.clip_id == kClipB &&
                  view.incoming.timeline_time == F(20) &&
                  view.incoming.source_time == F(10) &&
                  view.incoming.source_frame == 10 &&
                  view.incoming.handle == F(10),
              "B shows its first frame, source 10, with 10 frames of handle");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        session.Apply(session.PlanNudge(document, 1), document, log, error,
                      message);
        session.Apply(session.PlanNudge(document, 5), document, log, error,
                      message);
        view = DescribeTrimMonitor(document, session);
        Check(view.out_shift_frames == 6 && view.in_shift_frames == 6 &&
                  view.out_shift == F(6),
              "the counters add +1 and +5");
        Check(view.outgoing.source_frame == 35 &&
                  view.incoming.source_frame == 16,
              "and the frames follow the cut");

        session.SelectEdge(document, kClipC, TrimEdge::Tail, TrimSide::A,
                           false);
        view = DescribeTrimMonitor(document, session);
        Check(view.outgoing.present && !view.incoming.present &&
                  view.out_shift_frames == 0,
              "a free tail shows only its outgoing side; a new selection "
              "restarts the counters");
        session.SelectEdge(document, kClipB, TrimEdge::Head, TrimSide::B,
                           false);
        session.Apply(session.PlanNudge(document, -1), document, log, error,
                      message);
        view = DescribeTrimMonitor(document, session);
        Check(view.in_shift_frames == -1 && view.out_shift_frames == 0,
              "a B side counts on the In counter only");
    });

    Test("the Trim Monitor reads through the clip's speed", [] {
        Document document = Fixture();
        // Unlinked, so that the speed needs no partner.
        for (DocumentTrack& track : document.sequence.tracks)
            for (DocumentClip& clip : track.clips) clip.link_group_id.clear();
        document.FindClip(kClipA)->speed = {2, 1};
        document.FindClip(kClipA)->duration = F(10);
        document.FindClip(kClipB)->timeline_in = F(10);
        document.sequence.tracks[0].clips[2].timeline_in = F(30);
        document.sequence.tracks.resize(1);
        Check(Valid(document), "a 2x clip before B");
        TrimSession session;
        session.SelectNearest(document, F(10), {}, TrimSide::Roll);
        const TrimMonitorView view = DescribeTrimMonitor(document, session);
        Check(view.outgoing.source_time == F(28) &&
                  view.outgoing.source_frame == 28 &&
                  view.outgoing.handle == F(70) &&
                  view.outgoing.handle_frames == 35,
              "A at 2x reads 10..30: its last frame shows source 28, and the "
              "70 frames of source after it are 35 to trim");
        // A ramp is not refused by the trim operations: a ramped clip
        // trims, its keys following (ApplyTrim, ShiftClipAnimationHead).
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        Check(session.PlanNudge(document, -1).ok(),
              "a ripple of a 2x clip's tail plans");
    });

    Test("a speed ramp trims, and the monitor names its frame", [] {
        // The ramp of clip_speed_tests: 1 to 1/4 and back over 40 frames;
        // over 60 frames the clip shows source frame 36 last.
        const auto key = [](const std::string& id, int64_t frame,
                            EffectParamValue value,
                            KeyframeInterpolation interpolation) {
            ClipKeyframe result;
            result.id = id;
            result.time = F(frame);
            result.value = value;
            result.interpolation = interpolation;
            return result;
        };
        Document document;
        document.sources = {{kSourceOne, "rush.mov", {25, 1}, F(100)}};
        document.sequence.frame_rate = {25, 1};
        DocumentTrack v1{kV1, "video", 0, {}};
        DocumentClip ramped = Clip(kClipA, kSourceOne, 0, 60, 0);
        ramped.speed_ramp = {key("01KTR000000000000000000R01", 0, {1, 1},
                                 KeyframeInterpolation::Linear),
                             key("01KTR000000000000000000R02", 10, {1, 4},
                                 KeyframeInterpolation::Hold),
                             key("01KTR000000000000000000R03", 30, {1, 4},
                                 KeyframeInterpolation::Linear),
                             key("01KTR000000000000000000R04", 40, {1, 1},
                                 KeyframeInterpolation::Linear)};
        v1.clips = {ramped, Clip(kClipB, kSourceOne, 60, 20, 60)};
        document.sequence.tracks = {v1};
        Check(Valid(document), "a ramped clip before B");
        TrimSession session;
        session.SelectNearest(document, F(60), {}, TrimSide::Roll);
        const TrimMonitorView view = DescribeTrimMonitor(document, session);
        Check(view.outgoing.has_source && view.outgoing.source_frame == 36,
              "the ramp's last frame shows source frame 36");
        Check(session.PlanNudge(document, 1).ok(),
              "the trim operations do not refuse a ramp: a roll plans");
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        Document applied = document;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(session.Apply(session.PlanNudge(applied, -1), applied, log, error,
                            message) &&
                  Times(applied, kClipA, 0, 59, 0),
              "and a ripple of its tail applies: " + message);
    });

    Test(
        "FC4 (window): V takes the one nearest cut, on every targeted track",
        [] {
            Document document = Fixture();
            // A music clip on A2 that cuts at 3 s, far from the playhead.
            DocumentTrack a2{"01KTR000000000000000000T04", "audio", 3, {}};
            a2.clips = {
                Clip("01KTR000000000000000000C3A", kSourceTwo, 0, 75, 0),
                Clip("01KTR000000000000000000C3B", kSourceTwo, 0, 25, 75)};
            document.sequence.tracks.push_back(a2);
            Check(Valid(document), "a music track under the edit");
            const std::vector<Ulid> all = {kV1, kA1, kV2, a2.id};
            TrimSession session;
            RationalTime editTime;
            Check(
                session.SelectNearestEdit(document, F(22), all, TrimSide::Roll,
                                          editTime) == TrimRefusal::None,
                "V selects");
            Check(editTime == F(20), "the nearest cut is at 20 frames");
            const std::vector<TrimPoint>& points = session.Points();
            Check(points.size() == 2, "the cut on V1 and on A1, not the music");
            for (const TrimPoint& point : points)
                Check(point.side == TrimSide::Roll &&
                          point.in_shift.value == 0 &&
                          ((point.outgoing_clip_id == kClipA &&
                            point.incoming_clip_id == kClipB) ||
                           (point.outgoing_clip_id == kSoundA &&
                            point.incoming_clip_id == kSoundB)),
                      "each is the A/B cut at 20, as a roll");
            // The old SelectNearest took the music cut 55 frames away.
            TrimSession perTrack;
            perTrack.SelectNearest(document, F(22), all, TrimSide::Roll);
            Check(perTrack.Points().size() > points.size(),
                  "SelectNearest, per track, reaches the far music cut");
            // The cutaway on V2 starts at 25: nearer to 24 than the 20 cut.
            Check(session.SelectNearestEdit(document, F(24), all, TrimSide::B,
                                            editTime) == TrimRefusal::None &&
                      editTime == F(25) && session.Points().size() == 1 &&
                      session.Points()[0].incoming_clip_id == kCutaway,
                  "at 24, the cutaway's head at 25, alone");
            // A tie goes to the earlier edit.
            Check(session.SelectNearestEdit(document, F(30), {kV1},
                                            TrimSide::Roll,
                                            editTime) == TrimRefusal::None &&
                      editTime == F(20),
                  "at 30, between the cuts at 20 and 40, the earlier");
            // Only the targeted tracks, never a locked one.
            document.sequence.tracks[0].locked = true;
            Check(session.SelectNearestEdit(document, F(20), {kV1},
                                            TrimSide::Roll, editTime) ==
                      TrimRefusal::NoEditPoint,
                  "a locked track gives nothing");
            Check(
                session.SelectNearestEdit(document, F(20), {}, TrimSide::Roll,
                                          editTime) == TrimRefusal::NoEditPoint,
                "no target, no point");
            // A side a cut does not have is skipped: the free head of A.
            document.sequence.tracks[0].locked = false;
            Check(session.SelectNearestEdit(document, F(1), {kV1}, TrimSide::B,
                                            editTime) == TrimRefusal::None &&
                      editTime == F(0),
                  "the free head at 0 has an incoming side");
            Check(
                session.SelectNearestEdit(document, F(1), {kV1}, TrimSide::Roll,
                                          editTime) == TrimRefusal::None &&
                    editTime == F(20),
                "but no roll: the nearest roll is the cut at 20");
        });

    Test("FC4 (window): a click on an edge, and the marks", [] {
        Check(
            trim::TrimSideOfEdge(TrimEdge::Tail, false) == TrimSide::A &&
                trim::TrimSideOfEdge(TrimEdge::Head, false) == TrimSide::B &&
                trim::TrimSideOfEdge(TrimEdge::Tail, true) == TrimSide::Roll &&
                trim::TrimSideOfEdge(TrimEdge::Head, true) == TrimSide::Roll,
            "a tail is A, a head is B, Command rolls");
        Document document = Fixture();
        TrimSession session;
        Check(session.SelectEdge(document, kClipA, TrimEdge::Tail,
                                 TrimSide::Roll, false) == TrimRefusal::None,
              "the A/B cut as a roll");
        std::vector<TimelineEdgeMark> marks =
            trim::TrimSessionEdgeMarks(document, session);
        Check(marks.size() == 2 && marks[0].clip_id == kClipA &&
                  marks[0].edge == TrimEdge::Tail && !marks[0].at_limit &&
                  marks[1].clip_id == kClipB &&
                  marks[1].edge == TrimEdge::Head && !marks[1].at_limit,
              "a roll marks A's tail and B's head, both with handles");
        // C reads 50..60 of a 100-frame rush; its tail has 40 frames left.
        // Make A read up to the rush's end: its tail is at the limit.
        document.sequence.tracks[0].clips[0].source_in = F(80);
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        marks = trim::TrimSessionEdgeMarks(document, session);
        Check(marks.size() == 1 && marks[0].at_limit,
              "a tail at the rush's end is at the limit");
        session.SelectEdge(document, kClipB, TrimEdge::Head, TrimSide::B,
                           false);
        document.sequence.tracks[0].clips[1].source_in = F(0);
        marks = trim::TrimSessionEdgeMarks(document, session);
        Check(marks.size() == 1 && marks[0].clip_id == kClipB &&
                  marks[0].edge == TrimEdge::Head && marks[0].at_limit,
              "a head at the rush's first frame is at the limit");
        // Drawn where it is pressed: the geometry of TimelineView.h.
        TimelineViewport viewport;
        viewport.view_start = {0, 1};
        viewport.pixels_per_second = 100.0;
        viewport.header_width = 96.0;
        viewport.track_height = 44.0;
        const std::vector<TimelineEdgeMarkRect> rects =
            VisibleTimelineEdgeMarks(document, viewport, 800.0, marks);
        // B starts at 20 frames = 0.8 s: x = 96 + 80.
        const auto tracks = TimelineTracksInDisplayOrder(document);
        const auto v1Row = std::find_if(
            tracks.begin(), tracks.end(),
            [](const DocumentTrack* track) { return track->id == kV1; });
        const double v1Top =
            kTimelineRulerHeight + std::distance(tracks.begin(), v1Row) * 44.0;
        Check(rects.size() == 1 && rects[0].x == 176.0 &&
                  rects[0].width == kTimelineEdgeHitWidth &&
                  rects[0].y == v1Top && rects[0].height == 44.0,
              "B's head mark is its edge zone, in V1's row");
        const std::optional<TimelineHit> hit = HitTestTimeline(
            document, viewport, rects[0].x + rects[0].width / 2.0,
            rects[0].y + 20.0, 800.0);
        Check(
            hit && hit->clip_id == kClipB && hit->edge == TimelineHitEdge::Head,
            "and a press in the middle of the mark takes that edge");
        Check(session.StillHolds(document), "B's head is still there");
        document.sequence.tracks[0].clips.erase(
            document.sequence.tracks[0].clips.begin() + 1);
        Check(!session.StillHolds(document) &&
                  trim::TrimSessionEdgeMarks(document, session).empty(),
              "B removed: the point no longer holds, and draws nothing");
        session.Clear();
        Check(session.StillHolds(document) &&
                  trim::TrimSessionEdgeMarks(document, session).empty(),
              "no point, no mark");
    });

    Test("FC4 (window): the Trim Monitor's two panes and its words", [] {
        // 1920x1080, two 16:9 pictures: each half is 960 wide, 540 high,
        // centred.
        trim::TrimMonitorLayout layout =
            trim::LayoutTrimMonitor(1920, 1080, 1920, 1080, 1280, 720);
        Check(layout.outgoing.x == 0 && layout.outgoing.y == 270 &&
                  layout.outgoing.width == 960 && layout.outgoing.height == 540,
              "A fills the left half's width");
        Check(layout.incoming.x == 960 && layout.incoming.y == 270 &&
                  layout.incoming.width == 960 && layout.incoming.height == 540,
              "B the right half's, whatever its own size");
        // A portrait picture in a 9:16 sequence: limited by the width.
        layout = trim::LayoutTrimMonitor(1080, 1920, 1080, 1920, 0, 0);
        Check(layout.outgoing.width == 540 && layout.outgoing.height == 960 &&
                  layout.outgoing.x == 0 && layout.outgoing.y == 480,
              "9:16 in 9:16, half the width");
        Check(layout.incoming.empty(), "no picture for B, no pane");
        // A tall picture in a wide half: limited by the height.
        layout = trim::LayoutTrimMonitor(1920, 1080, 0, 0, 180, 320);
        Check(layout.incoming.height == 1080 && layout.incoming.width == 607 &&
                  layout.incoming.x == 960 + (960 - 607) / 2 &&
                  layout.incoming.y == 0,
              "a portrait shot in the right half, full height");
        Check(trim::LayoutTrimMonitor(0, 1080, 10, 10, 10, 10).outgoing.empty(),
              "no sequence, nothing");

        Document document = Fixture();
        TrimSession session;
        session.SelectEdge(document, kClipA, TrimEdge::Tail, TrimSide::A,
                           false);
        Document applied = document;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        session.Apply(session.PlanNudge(applied, -3), applied, log, error,
                      message);
        const trim::TrimMonitorText text = trim::DescribeTrimMonitorText(
            DescribeTrimMonitor(applied, session));
        Check(text.outgoing_title == "A · SORTIE" &&
                  text.incoming_title == "B · ENTRÉE",
              "the two sides are named");
        Check(text.outgoing_shift == "-3" && text.incoming_shift == "0",
              "A was trimmed three frames back, B not: " + text.outgoing_shift +
                  " " + text.incoming_shift);
        // A now reads 10..27 of a 100-frame rush: 73 frames left after it.
        Check(text.outgoing_handle == "poignée : 73 images",
              "what A can still take: " + text.outgoing_handle);
        Check(text.status == "RIPPLE · sortie (A)", "status: " + text.status);
        session.CycleSide(applied);
        Check(
            trim::DescribeTrimMonitorText(DescribeTrimMonitor(applied, session))
                    .status == "ROLL",
            "a roll is a roll, whatever the style");
        session.CycleSide(applied);
        session.CycleSide(applied);
        // A plain trim backward opens a gap: nothing blocks it.
        session.SetStyle(TrimStyle::Trim);
        session.Shuttle(applied, -1, Clock(0));
        session.Tick(applied, Clock(80));
        const trim::TrimMonitorText dynamic = trim::DescribeTrimMonitorText(
            DescribeTrimMonitor(applied, session));
        Check(dynamic.status == "TRIM · sortie (A) · dynamique ×1 arrière",
              "a dynamic trim says its speed: " + dynamic.status);
        Check(dynamic.outgoing_shift == "-5",
              "and its offset is in the counter (two frames in 80 ms at 25 "
              "i/s, after -3): " +
                  dynamic.outgoing_shift);
        Check(trim::SignedFrames(0) == "0" && trim::SignedFrames(12) == "+12" &&
                  trim::SignedFrames(-5) == "-5",
              "signed counters");
        // Every word the monitor draws is in the atlas: Latin-1.
        for (const std::string& line :
             {text.outgoing_title, text.incoming_title, text.outgoing_handle,
              dynamic.status}) {
            bool latin1 = true;
            for (size_t index = 0; index < line.size(); ++index) {
                const unsigned char byte =
                    static_cast<unsigned char>(line[index]);
                if (byte >= 0x80 && byte != 0xC2 && byte != 0xC3)
                    latin1 = false;
                if (byte == 0xC2 || byte == 0xC3) ++index;
            }
            Check(latin1, "Latin-1 only: " + line);
        }
    });

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << '\n';
    return failures == 0 ? 0 : 1;
}
