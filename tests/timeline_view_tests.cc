#include "TimelineView.h"

#include "UiTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <variant>

namespace {

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

Document Fixture() {
    Document document;
    document.sources = {
        {"01KT0000000000000000000001", "source.mov", {10, 1}, {100, 10}},
    };
    document.sequence.tracks = {
        {"01KT0000000000000000000002",
         "video",
         0,
         {{"01KT0000000000000000000003",
           "01KT0000000000000000000001",
           {10, 10},
           {20, 10},
           {10, 10}},
          {"01KT0000000000000000000004",
           "01KT0000000000000000000001",
           {40, 10},
           {10, 10},
           {40, 10}}}},
    };
    return document;
}

TimelineViewport Viewport() {
    TimelineViewport viewport;
    viewport.view_start = {0, 10};
    viewport.pixels_per_second = 100.0;
    viewport.track_height = 40.0;
    viewport.header_width = 50.0;
    return viewport;
}

uint64_t Hash(const std::string& bytes) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

}  // namespace

int main() {
    Test("ruler graduations leave room for their timecodes", [] {
        // Agentic QA of 23 September 2026 -- fitted to the window, a
        // timeline drew graduations 60 points apart under 74-point
        // timecodes. Given the label width, the ruler steps to the next
        // interval instead.
        TimelineViewport ruler;
        ruler.header_width = 96.0;
        ruler.pixels_per_second = 60.0;
        const std::vector<double> dense = ruler.TickXs(1200.0);
        const std::vector<double> spaced = ruler.TickXs(1200.0, 86.0);
        Check(dense.size() > 2 && dense[1] - dense[0] == 60.0,
              "without a label width, one second is 60 points");
        bool apart = spaced.size() > 2;
        for (size_t index = 1; index < spaced.size(); ++index)
            apart = apart && spaced[index] - spaced[index - 1] >= 86.0;
        Check(apart && spaced[1] - spaced[0] == 120.0,
              "graduations stay a label width apart, at two seconds");
    });

    Test("tracks go by the names the timeline gives them", [] {
        // Visual QA of 19 September 2026: the Inspector said "piste 0".
        Document document;
        DocumentTrack v1{GenerateUlid(), "video", 0, {}};
        DocumentTrack a1{GenerateUlid(), "audio", 1, {}};
        DocumentTrack v2{GenerateUlid(), "video", 2, {}};
        DocumentTrack s1{GenerateUlid(), "caption", 3, {}};
        document.sequence.tracks = {v1, a1, v2, s1};
        Check(TrackDisplayName(document, v1.id) == "V1" &&
                  TrackDisplayName(document, v2.id) == "V2" &&
                  TrackDisplayName(document, a1.id) == "A1" &&
                  TrackDisplayName(document, s1.id) == "S1",
              "V1 is the lowest video track, A1 and S1 the first audio and "
              "caption tracks");
        Check(TrackDisplayName(document, GenerateUlid()).empty(),
              "a track the document does not hold has no name");
    });

    Test("Source/Record three-point edits resolve exact NTSC durations", [] {
        const RationalTime duration{300300, 30000};
        SourceRecordEditPoints points;
        points.source_in = RationalTime{10010, 30000};
        points.timeline_in = RationalTime{50050, 30000};
        points.timeline_out = RationalTime{70070, 30000};
        SourceRecordEditRange range;
        std::string error;
        Check(ResolveSourceRecordEditRange(duration, points, range, error) &&
                  range.source_in == RationalTime{10010, 30000} &&
                  range.source_out == RationalTime{30030, 30000} &&
                  range.timeline_in == RationalTime{50050, 30000},
              "Record duration determines exclusive Source out exactly");
        points.source_in.reset();
        points.source_out = RationalTime{40040, 30000};
        Check(ResolveSourceRecordEditRange(duration, points, range, error) &&
                  range.source_in == RationalTime{20020, 30000},
              "Source out and Record range determine Source in");
        points.source_in = RationalTime{20020, 30000};
        points.timeline_in.reset();
        Check(ResolveSourceRecordEditRange(duration, points, range, error) &&
                  range.timeline_in == RationalTime{50050, 30000},
              "Source range and Record out determine Record in");
        points.timeline_in = RationalTime{50050, 30000};
        points.source_in = RationalTime{2, 3};
        points.source_out = RationalTime{40040, 30000};
        const SourceRecordEditRange previous = range;
        Check(!ResolveSourceRecordEditRange(duration, points, range, error) &&
                  range.source_in == previous.source_in &&
                  range.source_out == previous.source_out &&
                  range.timeline_in == previous.timeline_in,
              "conflicting four-point durations are refused without output "
              "mutation");
    });

    Test("Source/Record points refuse invalid and overflowing ranges", [] {
        const RationalTime duration{100, 25};
        SourceRecordEditRange range;
        std::string error;
        SourceRecordEditPoints points;
        Check(!ResolveSourceRecordEditRange(duration, points, range, error),
              "a Record position is required");
        points.timeline_in = RationalTime{0, 25};
        Check(ResolveSourceRecordEditRange(duration, points, range, error) &&
                  range.source_in == RationalTime{0, 25} &&
                  range.source_out == duration,
              "unmarked Source uses its full duration");
        points.timeline_out = RationalTime{0, 25};
        Check(!ResolveSourceRecordEditRange(duration, points, range, error),
              "empty Record range is refused");
        points.timeline_out = RationalTime{150, 25};
        Check(!ResolveSourceRecordEditRange(duration, points, range, error),
              "derived Source out cannot exceed the rush");
        points.timeline_in.reset();
        points.timeline_out = RationalTime{25, 25};
        Check(!ResolveSourceRecordEditRange(duration, points, range, error),
              "derived Record in cannot be negative");
        points.timeline_in = RationalTime{0, 0};
        Check(!ResolveSourceRecordEditRange(duration, points, range, error),
              "invalid rates are refused without throwing");
        points.timeline_in = RationalTime{1, 2147483647};
        points.timeline_out = RationalTime{10, 2147483646};
        Check(!ResolveSourceRecordEditRange(duration, points, range, error),
              "an unrepresentable common rate is refused instead of rounded");
    });

    Test("In/Out clear splits boundaries and leaves a gap", [] {
        Document document = Fixture();
        const std::string original = document.SaveToString();
        auto operation = TimelineRangeDeleteOperation(
            document, RationalTime{15, 10}, RationalTime{25, 10}, false);
        Check(operation.has_value(), "clear range produces one exact edit");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "clear range applies: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 3 &&
                  clips[0].timeline_in == RationalTime{10, 10} &&
                  clips[0].duration == RationalTime{5, 10} &&
                  clips[1].timeline_in == RationalTime{25, 10} &&
                  clips[1].source_in == RationalTime{25, 10} &&
                  clips[1].duration == RationalTime{5, 10} &&
                  clips[2].timeline_in == RationalTime{40, 10},
              "clear retains exact left/right survivors around the gap");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "clear range undoes byte-exactly");
    });

    Test("In/Out ripple delete closes the range on unlocked tracks", [] {
        Document document = Fixture();
        DocumentTrack locked = document.sequence.tracks[0];
        locked.id = "01KT0000000000000000000008";
        locked.index = 1;
        locked.locked = true;
        for (DocumentClip& clip : locked.clips) clip.id = GenerateUlid();
        document.sequence.tracks.push_back(locked);
        auto operation = TimelineRangeDeleteOperation(
            document, RationalTime{15, 10}, RationalTime{25, 10}, true);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "ripple range applies: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 3 &&
                  clips[0].timeline_in == RationalTime{10, 10} &&
                  clips[0].duration == RationalTime{5, 10} &&
                  clips[1].timeline_in == RationalTime{15, 10} &&
                  clips[1].duration == RationalTime{5, 10} &&
                  clips[2].timeline_in == RationalTime{30, 10},
              "ripple removes ten ticks and closes both boundaries");
        Check(document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{10, 10} &&
                  document.sequence.tracks[1].clips[0].duration ==
                      RationalTime{20, 10} &&
                  document.sequence.tracks[1].clips[1].timeline_in ==
                      RationalTime{40, 10},
              "locked track remains byte-for-byte timed as before");
    });

    Test("selected clip ripple delete removes and closes in one operation", [] {
        Document document = Fixture();
        const std::string before = document.SaveToString();
        const Ulid selected = document.sequence.tracks[0].clips[0].id;
        const Ulid following = document.sequence.tracks[0].clips[1].id;
        const auto operation =
            BuildTimelineRippleDeleteSelection(document, {selected});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "selected clip produces an applicable exact ripple edit");
        Check(!document.FindClip(selected) &&
                  document.FindClip(following)->timeline_in ==
                      RationalTime{20, 10},
              "selected duration is removed and the next clip closes it");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "selected ripple delete undoes byte-exactly");
    });

    Test("range edits obey targeting and independent sync lock", [] {
        Document document = Fixture();
        DocumentTrack follower = document.sequence.tracks[0];
        follower.id = "01KT0000000000000000000008";
        follower.kind = "audio";
        follower.index = 1;
        follower.sync_lock = false;
        for (DocumentClip& clip : follower.clips) clip.id = GenerateUlid();
        document.sequence.tracks.push_back(follower);
        const Ulid target = document.sequence.tracks[0].id;

        auto clear =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, false, {target});
        Check(clear && clear->exact_track_result.size() == 1 &&
                  clear->exact_track_result.front().track_id == target,
              "clear edits only the explicitly targeted track");

        auto unsyncedRipple =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, true, {target});
        Check(unsyncedRipple && unsyncedRipple->exact_track_result.size() == 1,
              "ripple leaves an untargeted sync-unlocked track untouched");

        document.sequence.tracks[1].sync_lock = true;
        auto syncedRipple =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, true, {target});
        Check(syncedRipple && syncedRipple->exact_track_result.size() == 2,
              "ripple carries an untargeted sync-locked track");
        Check(!TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                            RationalTime{25, 10}, true, {}),
              "ripple without an editable target is a no-op");
    });

    Test("range and source edits split a graded clip", [] {
        const auto graded = [] {
            Document document = Fixture();
            ClipEffect contrast;
            contrast.id = "01KT000000000000000000000E";
            contrast.type = "color.contrast";
            contrast.params["amount"] = {1, 5};
            document.sequence.tracks[0].clips[0].effects = {contrast};
            return document;
        };
        const Ulid target = "01KT0000000000000000000002";
        for (const bool ripple : {false, true}) {
            Document document = graded();
            auto operation = TimelineRangeDeleteOperation(
                document, RationalTime{15, 10}, RationalTime{25, 10}, ripple,
                {target});
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(operation && log.Apply(document, Operation{*operation}, error,
                                         message),
                  "a range delete splits a graded clip: " + message);
        }
        for (const bool insert : {false, true}) {
            Document document = graded();
            auto operation = TimelineSourceEditOperation(
                document, "01KT0000000000000000000001", RationalTime{0, 10},
                RationalTime{5, 10}, RationalTime{20, 10}, target, insert);
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(operation && log.Apply(document, Operation{*operation}, error,
                                         message),
                  "a source edit splits a graded clip: " + message);
        }
    });

    Test("continuous followers keep a spanning cutaway whole", [] {
        Document document = Fixture();
        // A cutaway over [12, 28) spans the removed range [15, 25), and a
        // second one starts inside it.
        DocumentTrack cutaways{"01KT0000000000000000000009", "video", 1, {}};
        cutaways.clips = {{"01KT000000000000000000000A",
                           "01KT0000000000000000000001",
                           {60, 10},
                           {16, 10},
                           {12, 10}},
                          {"01KT000000000000000000000B",
                           "01KT0000000000000000000001",
                           {80, 10},
                           {10, 10},
                           {30, 10}}};
        document.sequence.tracks.push_back(cutaways);
        const std::string before = document.SaveToString();
        const Ulid target = document.sequence.tracks[0].id;
        auto operation = TimelineRangeDeleteOperation(
            document, RationalTime{15, 10}, RationalTime{25, 10}, true,
            {target}, RangeDeleteFollowers::Continuous);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "continuous range delete applies: " + message);
        const auto& kept = document.sequence.tracks[1].clips;
        Check(kept.size() == 2 && kept[0].timeline_in == RationalTime{12, 10} &&
                  kept[0].source_in == RationalTime{60, 10} &&
                  kept[0].duration == RationalTime{6, 10},
              "the spanning cutaway keeps its head frames and ends with the "
              "removed words");
        Check(kept.size() == 2 && kept[1].timeline_in == RationalTime{20, 10} &&
                  kept[1].source_in == RationalTime{80, 10},
              "a later cutaway moves back unchanged");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "continuous range delete undoes byte-exactly");

        auto cut =
            TimelineRangeDeleteOperation(document, RationalTime{15, 10},
                                         RationalTime{25, 10}, true, {target});
        Check(cut && log.Apply(document, Operation{*cut}, error, message) &&
                  document.sequence.tracks[1].clips.size() == 3,
              "the default still cuts the range out of followers");
    });

    Test("source insert opens sync-locked tracks atomically", [] {
        Document document = Fixture();
        DocumentTrack follower = document.sequence.tracks[0];
        follower.id = "01KT0000000000000000000008";
        follower.kind = "audio";
        follower.index = 1;
        for (DocumentClip& clip : follower.clips) clip.id = GenerateUlid();
        document.sequence.tracks.push_back(follower);
        const std::string original = document.SaveToString();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10},
            document.sequence.tracks[0].id, true);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation && operation->exact_track_result.size() == 2 &&
                  log.Apply(document, Operation{*operation}, error, message),
              "source insert applies to target and sync follower: " + message);
        const auto& target = document.sequence.tracks[0].clips;
        Check(target.size() == 4 &&
                  target[0].duration == RationalTime{10, 10} &&
                  target[1].source_in == RationalTime{0, 10} &&
                  target[1].timeline_in == RationalTime{20, 10} &&
                  target[2].timeline_in == RationalTime{30, 10} &&
                  target[3].timeline_in == RationalTime{50, 10},
              "insert splits the edit-point clip and opens exact duration");
        Check(document.sequence.tracks[1].clips.back().timeline_in ==
                  RationalTime{50, 10},
              "sync follower opens the same duration");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "source insert undoes byte-exactly");
    });

    Test("source overwrite replaces only the destination interval", [] {
        Document document = Fixture();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10},
            document.sequence.tracks[0].id, false);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation &&
                  log.Apply(document, Operation{*operation}, error, message),
              "source overwrite applies: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 3 && clips[0].duration == RationalTime{10, 10} &&
                  clips[1].source_in == RationalTime{0, 10} &&
                  clips[1].timeline_in == RationalTime{20, 10} &&
                  clips[2].timeline_in == RationalTime{40, 10},
              "overwrite preserves timeline duration and downstream clips");
    });

    Test("source edit patches linked audio to its explicit target", [] {
        Document document = Fixture();
        const Ulid audioTrackId = "01KT0000000000000000000008";
        document.sequence.tracks.push_back(
            {audioTrackId, "audio", 1, {}, false, false});
        const std::string original = document.SaveToString();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10},
            document.sequence.tracks[0].id, false, {audioTrackId});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation && operation->exact_track_result.size() == 2 &&
                  log.Apply(document, Operation{*operation}, error, message),
              "video/audio source overwrite applies atomically: " + message);
        const DocumentClip* video = nullptr;
        for (const DocumentClip& clip : document.sequence.tracks[0].clips)
            if (clip.timeline_in == RationalTime{20, 10}) video = &clip;
        const auto& audio = document.sequence.tracks[1].clips;
        Check(video && !video->include_audio && audio.size() == 1 &&
                  !video->link_group_id.empty() &&
                  audio[0].link_group_id == video->link_group_id &&
                  video->sync_anchor_clip_id == video->id &&
                  audio[0].sync_anchor_clip_id == video->id &&
                  audio[0].source_in == video->source_in &&
                  audio[0].duration == video->duration &&
                  audio[0].timeline_in == video->timeline_in,
              "patched audio is separate, sample-aligned, linked and synced");
        Check(video &&
                  ExpandLinkedClipSelection(document, {video->id}).size() == 2,
              "linked selection expands across the patched pair");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "patched source edit undoes byte-exactly");
    });

    // Out of lot -- 29 October 2026: a drop finds or creates its tracks.
    Test("a drop goes to the pointed track and its partner of the same number",
         [] {
             Document document = Fixture();
             const Ulid v1 = document.sequence.tracks[0].id;
             const Ulid a1 = "01KT0000000000000000000008";
             const Ulid v2 = "01KT0000000000000000000009";
             document.sequence.tracks.push_back({a1, "audio", 1, {}});
             document.sequence.tracks.push_back({v2, "video", 2, {}});
             TimelineViewport viewport;
             // Rows down the timeline: V2, V1, A1, then the blank.
             const auto at = [&](int row, bool video, bool audio,
                                 std::string& refusal) {
                 return TimelineDropTracksAt(
                     document, viewport,
                     kTimelineRulerHeight + (row + 0.5) * viewport.track_height,
                     video, audio, refusal);
             };
             std::string refusal;
             auto onV1 = at(1, true, true, refusal);
             Check(onV1 && onV1->video_track_id == v1 &&
                       onV1->audio_track_id == a1 &&
                       !onV1->create_video_track && !onV1->create_audio_track,
                   "on V1, the picture goes to V1 and the sound to A1");
             auto onV2 = at(0, true, true, refusal);
             Check(onV2 && onV2->video_track_id == v2 &&
                       onV2->audio_track_id.empty() && onV2->create_audio_track,
                   "on V2 without an A2, the sound gets a new track, not A1");
             auto onA1 = at(2, true, true, refusal);
             Check(onA1 && onA1->audio_track_id == a1 &&
                       onA1->video_track_id == v1,
                   "on A1, the sound goes to A1 and the picture to V1");
             auto music = at(1, false, true, refusal);
             Check(music && music->video_track_id.empty() &&
                       !music->create_video_track &&
                       music->audio_track_id == a1,
                   "a sound-only media dropped on V1 goes to A1");
             auto blank = at(3, true, true, refusal);
             Check(blank && blank->video_track_id.empty() &&
                       blank->audio_track_id.empty() &&
                       blank->create_video_track && blank->create_audio_track,
                   "the blank below the tracks creates a track of each kind");
             auto stillInBlank = at(5, true, false, refusal);
             Check(stillInBlank && stillInBlank->create_video_track &&
                       !stillInBlank->create_audio_track,
                   "a picture-only media in the blank creates a video track");

             document.sequence.tracks.push_back(
                 {"01KT000000000000000000000A", "caption", 3, {}});
             // Rows: S1, V2, V1, A1.
             auto onCaptions = at(0, true, true, refusal);
             Check(onCaptions && onCaptions->create_video_track &&
                       onCaptions->create_audio_track,
                   "a caption track takes no media: the drop creates tracks");

             Check(!TimelineDropTracksAt(document, viewport,
                                         kTimelineRulerHeight - 1.0, true, true,
                                         refusal) &&
                       refusal.empty(),
                   "above the tracks, nothing is dropped and nothing said");
             Check(!at(2, false, false, refusal) && !refusal.empty(),
                   "a drop that carries nothing is refused by name");
         });

    Test("a locked track refuses what it would take, its partner is replaced",
         [] {
             Document document = Fixture();
             const Ulid a1 = "01KT0000000000000000000008";
             document.sequence.tracks.push_back({a1, "audio", 1, {}});
             TimelineViewport viewport;
             const double v1Row =
                 kTimelineRulerHeight + 0.5 * viewport.track_height;
             std::string refusal;
             document.sequence.tracks[0].locked = true;
             Check(!TimelineDropTracksAt(document, viewport, v1Row, true, true,
                                         refusal) &&
                       refusal.find("V1") != std::string::npos,
                   "a picture dropped on a locked V1 is refused, naming V1");
             auto sound = TimelineDropTracksAt(document, viewport, v1Row, false,
                                               true, refusal);
             Check(sound && sound->audio_track_id == a1,
                   "a locked V1 does not stop a sound-only media from A1");
             document.sequence.tracks[0].locked = false;
             document.sequence.tracks[1].locked = true;
             auto partnerLocked = TimelineDropTracksAt(
                 document, viewport, v1Row, true, true, refusal);
             Check(partnerLocked && partnerLocked->audio_track_id.empty() &&
                       partnerLocked->create_audio_track,
                   "a locked A1 is left alone: the sound gets a new track");
         });

    Test("a sound-only source edit targets its audio track alone", [] {
        Document document = Fixture();
        const Ulid audioTrackId = "01KT0000000000000000000008";
        document.sequence.tracks.push_back({audioTrackId, "audio", 1, {}});
        const std::string original = document.SaveToString();
        auto operation = TimelineSourceEditOperation(
            document, document.sources[0].id, RationalTime{0, 10},
            RationalTime{10, 10}, RationalTime{20, 10}, audioTrackId, false);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(operation && operation->exact_track_result.size() == 1 &&
                  log.Apply(document, Operation{*operation}, error, message),
              "a sound-only overwrite applies: " + message);
        const auto& audio = document.sequence.tracks[1].clips;
        Check(audio.size() == 1 && audio[0].include_audio &&
                  audio[0].link_group_id.empty() &&
                  audio[0].timeline_in == RationalTime{20, 10} &&
                  document.sequence.tracks[0].clips.size() == 2,
              "the sound lands alone on its track, the picture is untouched");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "the sound-only edit undoes byte-exactly");
        Check(!TimelineSourceEditOperation(
                  document, document.sources[0].id, RationalTime{0, 10},
                  RationalTime{10, 10}, RationalTime{20, 10}, audioTrackId,
                  false, {audioTrackId}),
              "an audio target with audio partners is still refused");
    });

    Test("display order follows professional video/audio stacking", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "video", 2, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000006", "audio", 3, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000007", "audio", 1, {}});
        const auto tracks = TimelineTracksInDisplayOrder(document);
        Check(tracks.size() == 4 && tracks[0]->kind == "video" &&
                  tracks[0]->index == 2 && tracks[1]->kind == "video" &&
                  tracks[1]->index == 0 && tracks[2]->kind == "audio" &&
                  tracks[2]->index == 1 && tracks[3]->index == 3,
              "higher video layers appear above V1 and audio starts at A1");
    });

    Test("time and pixels invert at frame boundaries", [] {
        const struct Case {
            int32_t rate;
            int64_t frameStep;
        } cases[] = {{24, 1}, {25, 1}, {30000, 1001}};
        const double zooms[] = {4.0, 37.5, 100.0, 1333.0, 4000.0};
        for (const Case value : cases) {
            for (double zoom : zooms) {
                TimelineViewport viewport;
                viewport.view_start = {value.frameStep * 7, value.rate};
                viewport.pixels_per_second = zoom;
                viewport.header_width = 71.0;
                for (int64_t frame = -5; frame <= 50; ++frame) {
                    const RationalTime time{frame * value.frameStep,
                                            value.rate};
                    const RationalTime roundTrip =
                        viewport.XToTime(viewport.TimeToX(time), value.rate);
                    Check(roundTrip == time,
                          "round trip must preserve frame at every zoom");
                }
            }
        }
    });

    Test("playhead quantizes exactly to frames or audio samples", [] {
        const MediaRate ntsc{30000, 1001};
        Check(QuantizePlayheadPosition({1500, 30000}, PlayheadResolution::Frame,
                                       ntsc) == RationalTime{1001, 30000},
              "NTSC position below half-frame rounds to the first frame");
        Check(QuantizePlayheadPosition({1502, 30000}, PlayheadResolution::Frame,
                                       ntsc) == RationalTime{2002, 30000},
              "NTSC position above half-frame rounds to the second frame");
        Check(QuantizePlayheadPosition({1, 96000}, PlayheadResolution::Sample,
                                       ntsc) == RationalTime{1, 48000},
              "half an audio sample rounds explicitly away from zero");
        Check(QuantizePlayheadPosition({1, 192000}, PlayheadResolution::Sample,
                                       ntsc) == RationalTime{0, 48000},
              "sub-half-sample position rounds to sample zero");
    });

    Test("hit test resolves clips, holes and six-point edges", [] {
        const Document document = Fixture();
        const TimelineViewport viewport = Viewport();
        const double trackY = kTimelineRulerHeight + 20.0;
        const auto inside =
            HitTestTimeline(document, viewport, 200.0, trackY, 700.0);
        Check(inside &&
                  inside->clip_id == document.sequence.tracks[0].clips[0].id &&
                  inside->edge == TimelineHitEdge::None,
              "clip body returns its ULID");
        Check(!HitTestTimeline(document, viewport, 375.0, trackY, 700.0),
              "gap returns no clip");
        const auto head =
            HitTestTimeline(document, viewport, 154.0, trackY, 700.0);
        Check(head && head->edge == TimelineHitEdge::Head,
              "point within six points of head returns head edge");
        const auto tail =
            HitTestTimeline(document, viewport, 346.0, trackY, 700.0);
        Check(tail && tail->edge == TimelineHitEdge::Tail,
              "point within six points of tail returns tail edge");
    });

    Test("bounded gaps are selectable but trailing space is not", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        const double trackY = kTimelineRulerHeight + 20.0;
        const auto gap =
            HitTestTimelineGap(document, viewport, 375.0, trackY, 700.0, 10);
        Check(gap && gap->track_id == document.sequence.tracks[0].id &&
                  gap->start == RationalTime{30, 10} &&
                  gap->duration == RationalTime{10, 10},
              "clicking the bounded hole resolves its exact range");
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.PointerDown(375.0, trackY, 700.0, 10);
        Check(interaction.SelectedGap() && interaction.SelectedClipId().empty(),
              "pointer interaction selects the gap instead of a clip");
        interaction.PointerDown(600.0, trackY, 700.0, 10);
        Check(!interaction.SelectedGap(),
              "unbounded space after the last clip is not a deletable gap");
    });

    Test("lasso selects every intersecting clip rectangle", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        const double top = kTimelineRulerHeight + 5.0;
        const double bottom =
            kTimelineRulerHeight + viewport.track_height - 5.0;
        const auto selected = LassoHitTestTimeline(document, viewport, 140.0,
                                                   top, 460.0, bottom, 700.0);
        Check(selected.size() == 2 &&
                  selected[0] == document.sequence.tracks[0].clips[0].id &&
                  selected[1] == document.sequence.tracks[0].clips[1].id,
              "lasso intersection returns both clip ULIDs in display order");
        Check(LassoHitTestTimeline(document, viewport, 360.0, top, 440.0,
                                   bottom, 700.0)
                  .empty(),
              "lasso confined to a gap selects no clip");
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SelectClips(selected);
        Check(interaction.SelectedClipIds().size() == 2 &&
                  interaction.SelectedClipId().empty(),
              "multi-selection has no accidental single edit target");
        // ...but it still has an anchor. Selecting an A/V-linked clip always
        // brings in its partner, so a panel that describes one clip at a
        // time cannot key off SelectedClipId() or it stays blank for every
        // clip imported from a video with sound -- which is what left the
        // Inspector empty in practice.
        Check(interaction.PrimarySelectedClipId() == selected.front(),
              "multi-selection still anchors on the caller's first clip");
        interaction.SelectClips({});
        Check(interaction.PrimarySelectedClipId().empty(),
              "clearing the selection clears its anchor");
        interaction.SelectClips(selected);
        const auto rectangles =
            VisibleTimelineClips(document, viewport, 700.0,
                                 interaction.SelectedClipIds(), std::nullopt);
        Check(std::count_if(rectangles.begin(), rectangles.end(),
                            [](const TimelineClipRect& clip) {
                                return clip.selected;
                            }) == 2,
              "every lasso member receives selected rendering");
    });

    Test("dragging one member preserves and moves an arbitrary selection", [] {
        Document document = Fixture();
        const std::string before = document.SaveToString();
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const Ulid second = document.sequence.tracks[0].clips[1].id;
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SelectClips({first, second});
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(250.0, trackY, 700.0, 10);
        Check(interaction.SelectedClipIds().size() == 2,
              "mouse-down on a selected clip keeps the group selected");
        interaction.PointerDrag(350.0, trackY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(pending && std::holds_alternative<MoveClipsOperation>(*pending),
              "group drag emits the canonical multi-clip operation");
        EditError error = EditError::None;
        std::string message;
        Check(
            interaction.PointerUp(error, message) &&
                document.FindClip(first)->timeline_in == RationalTime{20, 10} &&
                document.FindClip(second)->timeline_in == RationalTime{50, 10},
            "both clips move by the same exact delta");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "group drag undo restores byte-identical state");
    });

    Test("group drag treats timeline zero as a magnetic wall", [] {
        Document document = Fixture();
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const Ulid second = document.sequence.tracks[0].clips[1].id;
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SelectClips({first, second});
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(500.0, trackY, 700.0, 10);
        interaction.PointerDrag(0.0, trackY, 700.0);
        Check(interaction.MovePreview() && interaction.MovePreview()->valid &&
                  interaction.MovePreview()->linked_moves[0].timeline_in ==
                      RationalTime{0, 10} &&
                  interaction.MovePreview()->linked_moves[1].timeline_in ==
                      RationalTime{30, 10},
              "the earliest selected clip cannot cross timeline zero");

        TimelineViewport magneticViewport = Viewport();
        magneticViewport.pixels_per_second = 50.0;
        TimelineInteraction magnetic(document, log, magneticViewport);
        magnetic.SelectClips({first, second});
        magnetic.PointerDown(275.0, trackY, 700.0, 10);
        magnetic.PointerDrag(230.0, trackY, 700.0);
        Check(magnetic.MovePreview() && magnetic.MovePreview()->valid &&
                  magnetic.MovePreview()->linked_moves[0].timeline_in ==
                      RationalTime{0, 10} &&
                  magnetic.MovePreview()->linked_moves[1].timeline_in ==
                      RationalTime{30, 10} &&
                  magnetic.SnapGuideTime() &&
                  *magnetic.SnapGuideTime() == RationalTime{0, 10},
              "the full selection snaps when its earliest clip nears zero");
    });

    Test("deleting a gap closes one track atomically and undo restores bytes",
         [] {
             Document document = Fixture();
             const std::string initial = document.SaveToString();
             EditLog log;
             EditError error = EditError::None;
             std::string message;
             Operation operation = DeleteGapOperation{
                 document.sequence.tracks[0].id, {30, 10}, {10, 10}, {}};
             Check(log.Apply(document, std::move(operation), error, message),
                   "gap deletion applies: " + message);
             Check(log.AppliedCount() == 1 &&
                       document.sequence.tracks[0].clips[0].timeline_in ==
                           RationalTime{10, 10} &&
                       document.sequence.tracks[0].clips[1].timeline_in ==
                           RationalTime{30, 10},
                   "all following clips shift left by the exact gap duration");
             const std::string json =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveClipOperation{};
             Check(DeserializeOperation(json, parsed, error, message) &&
                       SerializeOperation(parsed) == json,
                   "DeleteGap JSON round-trips canonically");
             Check(log.Undo(document, error, message),
                   "gap deletion undo succeeds: " + message);
             Check(document.SaveToString() == initial,
                   "undo restores the canonical document byte for byte");
             Check(log.Redo(document, error, message) &&
                       document.sequence.tracks[0].clips[1].timeline_in ==
                           RationalTime{30, 10},
                   "redo closes the same gap exactly");
         });

    Test("linked gap deletion ripples video and audio together", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[1].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked gap fixture detaches the following audio");
        const Ulid audioId = document.sequence.tracks[1].clips.front().id;
        const std::string before = document.SaveToString();
        Operation close = DeleteGapOperation{document.sequence.tracks[0].id,
                                             {30, 10},
                                             {10, 10},
                                             {},
                                             {document.sequence.tracks[1].id}};
        Check(log.Apply(document, std::move(close), error, message),
              "linked gap deletion applies: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{30, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{30, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked image and audio ripple by the same exact duration");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "linked DeleteGap JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked gap undo restores both tracks byte-exactly");
    });

    Test("a gap deletion crossing a clip is rejected without mutation", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation operation = DeleteGapOperation{
            document.sequence.tracks[0].id, {20, 10}, {10, 10}, {}};
        Check(!log.Apply(document, std::move(operation), error, message) &&
                  error == EditError::InvalidOperation,
              "range intersecting a clip is rejected");
        Check(log.AppliedCount() == 0 && document.SaveToString() == initial,
              "rejected deletion leaves document bytes and event log intact");
    });

    Test("adding a video track is atomic, stable and exactly reversible", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation operation = AddTrackOperation{"", "video", 1};
        Check(log.Apply(document, std::move(operation), error, message),
              "track creation applies: " + message);
        Check(document.sequence.tracks.size() == 2 &&
                  document.sequence.tracks[1].kind == "video" &&
                  document.sequence.tracks[1].index == 1 &&
                  IsValidUlid(document.sequence.tracks[1].id),
              "new video layer has a stable generated identity");
        const Ulid addedId = document.sequence.tracks[1].id;
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "AddTrack JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "track creation undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.sequence.tracks.back().id == addedId,
              "redo retains the generated track ULID");
    });

    Test("detached audio becomes an independent exact timeline clip", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{
            videoId, document.sequence.tracks[1].id, "", {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "audio detach applies: " + message);
        Check(!document.sequence.tracks[0].clips[0].include_audio &&
                  document.sequence.tracks[1].clips.size() == 1,
              "video contribution is muted and audio rectangle is created");
        const DocumentClip audio = document.sequence.tracks[1].clips[0];
        Check(IsValidUlid(audio.id) && audio.id != videoId &&
                  audio.source_id ==
                      document.sequence.tracks[0].clips[0].source_id &&
                  audio.source_in ==
                      document.sequence.tracks[0].clips[0].source_in &&
                  audio.duration ==
                      document.sequence.tracks[0].clips[0].duration &&
                  audio.timeline_in ==
                      document.sequence.tracks[0].clips[0].timeline_in,
              "detached clip retains exact source and timeline timing");
        Check(
            audio.link_group_id == audio.id &&
                document.sequence.tracks[0].clips[0].link_group_id == audio.id,
            "detached image and audio share a stable link group");
        const std::vector<Ulid> linked =
            ExpandLinkedClipSelection(document, {videoId});
        Check(linked.size() == 2 && std::find(linked.begin(), linked.end(),
                                              audio.id) != linked.end(),
              "linked selection expands from video to its audio clip");
        const auto rectangles = VisibleTimelineClips(
            document, Viewport(), 700.0, linked, std::nullopt);
        Check(std::count_if(rectangles.begin(), rectangles.end(),
                            [](const TimelineClipRect& rectangle) {
                                return rectangle.audio;
                            }) == 1,
              "audio rectangles carry their distinct semantic render role");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "DetachAudio JSON round-trips canonically");

        Operation sampleOffset =
            MoveClipOperation{audio.id,
                              document.sequence.tracks[1].id,
                              audio.timeline_in.add({1, 48000}),
                              {}};
        Check(log.Apply(document, std::move(sampleOffset), error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{1, 48000},
              "sub-frame drift remains exact at audio-sample resolution");
        Check(log.Undo(document, error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{0, 1},
              "sample offset undo restores zero drift");
        Operation headTrim =
            TrimClipOperation{audio.id, TrimEdge::Head, {1, 10}, std::nullopt};
        Check(log.Apply(document, std::move(headTrim), error, message) &&
                  ClipSyncDrift(document, audio.id) == RationalTime{0, 1},
              "head trim changes source and timeline equally without false "
              "drift");
        Check(log.Undo(document, error, message),
              "head trim sync test undo succeeds");

        Operation move = MoveClipOperation{
            audio.id, document.sequence.tracks[1].id, {50, 10}, {}};
        Check(log.Apply(document, std::move(move), error, message),
              "detached audio moves independently: " + message);
        Check(document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{50, 10} &&
                  document.sequence.tracks[0].clips[0].timeline_in ==
                      RationalTime{10, 10},
              "moving audio leaves video timing unchanged");
        Check(log.Undo(document, error, message), "audio move undo succeeds");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "detach undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.sequence.tracks[1].clips[0].id == audio.id,
              "detach redo retains the generated audio ULID");
        Operation unlink = SetClipLinkOperation{videoId, audio.id, "", ""};
        Check(
            log.Apply(document, std::move(unlink), error, message) &&
                document.FindClip(videoId)->link_group_id.empty() &&
                document.FindClip(audio.id)->link_group_id.empty(),
            "linked selection can be atomically removed without editing times");
        const std::string unlinkJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(unlinkJson, parsed, error, message) &&
                  SerializeOperation(parsed) == unlinkJson,
              "SetClipLink JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindClip(videoId)->link_group_id == audio.id &&
                  document.FindClip(audio.id)->link_group_id == audio.id,
              "link toggle undo restores the exact shared group");
    });

    Test("linked selection moves video and audio atomically", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked fixture detach applies: " + message);
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string beforeMove = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double y = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, y, 700.0, 10);
        interaction.PointerDrag(250.0, y, 700.0);
        Check(interaction.SelectedClipIds().size() == 2 &&
                  interaction.MovePreview() &&
                  interaction.MovePreview()->linked_moves.size() == 2 &&
                  interaction.MovePreview()->valid,
              "linked drag previews both members as one valid candidate");
        const auto pending = interaction.PendingOperation();
        Check(pending &&
                  std::holds_alternative<MoveLinkedClipsOperation>(*pending),
              "linked drag emits the dedicated atomic operation");
        Check(interaction.PointerUp(error, message),
              "linked pointer-up applies: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{15, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "video and audio receive the same exact delta without drift");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  SerializeOperation(parsed) == operationJson,
              "MoveLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeMove,
              "one undo restores the pre-move document byte-exactly");
    });

    Test("locked linked partner detaches only the current move gesture", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(
                  document,
                  Operation{DetachAudioOperation{videoId,
                                                 document.sequence.tracks[1].id,
                                                 "01KT0000000000000000000006",
                                                 {}}},
                  error, message),
              "locked-partner fixture detaches audio: " + message);
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        document.sequence.tracks[0].locked = true;
        const std::string beforeMove = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double audioY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        interaction.PointerDown(200.0, audioY, 700.0, 10);
        interaction.PointerDrag(250.0, audioY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.SelectedClipIds() == std::vector<Ulid>{audioId} &&
                  interaction.MovePreview() &&
                  interaction.MovePreview()->linked_moves.empty() &&
                  interaction.MovePreview()->valid && pending &&
                  std::holds_alternative<MoveClipOperation>(*pending),
              "dragging unlocked audio skips its locked video partner");
        Check(interaction.PointerUp(error, message),
              "independent audio gesture applies: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{10, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  !document.FindClip(audioId)->link_group_id.empty(),
              "gesture moves only audio without destroying the persistent "
              "link");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeMove,
              "independent locked-partner move undoes byte-exactly");

        TimelineInteraction lockedAnchor(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        lockedAnchor.PointerDown(200.0, videoY, 700.0, 10);
        lockedAnchor.PointerDrag(250.0, videoY, 700.0);
        Check(!lockedAnchor.HasActiveDrag() && !lockedAnchor.PendingOperation(),
              "a clip on the locked track itself remains immovable");
    });

    Test("linked cross-track move maps V1/A1 to V2/A2", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000010", "video", 1, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000011", "audio", 2, {}});
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000012", "audio", 3, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(
                  document,
                  Operation{DetachAudioOperation{videoId,
                                                 document.sequence.tracks[2].id,
                                                 "01KT0000000000000000000013",
                                                 {}}},
                  error, message),
              "four-track linked fixture detaches audio: " + message);
        const Ulid audioId = document.sequence.tracks[2].clips[0].id;
        const Ulid videoTwo = document.sequence.tracks[1].id;
        const Ulid audioTwo = document.sequence.tracks[3].id;
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        // Display order is V2, V1, A1, A2.
        const double videoOneY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        const double videoTwoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, videoOneY, 700.0, 10);
        interaction.PointerDrag(250.0, videoTwoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(pending &&
                  std::holds_alternative<MoveLinkedClipsOperation>(*pending),
              "V1 to V2 emits one linked move");
        if (pending) {
            const auto& move = std::get<MoveLinkedClipsOperation>(*pending);
            const auto video =
                std::find_if(move.moves.begin(), move.moves.end(),
                             [&](const LinkedClipMove& item) {
                                 return item.clip_id == videoId;
                             });
            const auto audio =
                std::find_if(move.moves.begin(), move.moves.end(),
                             [&](const LinkedClipMove& item) {
                                 return item.clip_id == audioId;
                             });
            Check(video != move.moves.end() && video->track_id == videoTwo &&
                      audio != move.moves.end() && audio->track_id == audioTwo,
                  "the same +1 ordinal maps video to V2 and audio to A2");
        }
        Check(interaction.PointerUp(error, message),
              "paired V2/A2 move applies: " + message);
        Check(document.FindTrackForClip(videoId)->id == videoTwo &&
                  document.FindTrackForClip(audioId)->id == audioTwo &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked clips land on V2/A2 without sync drift");
    });

    Test("linked move respects the zero wall and sync drift is exact", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "invalid fixture detach applies");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        Operation offset = MoveClipOperation{
            audioId, document.sequence.tracks[1].id, {0, 10}, {}};
        Check(log.Apply(document, std::move(offset), error, message),
              "independent audio offset applies");
        Check(ClipSyncDrift(document, audioId) == RationalTime{-10, 10},
              "audio drift reports its exact signed phase difference");
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        TimelineInteraction independent(document, log, viewport);
        independent.SetLinkedSelectionEnabled(false);
        const double audioY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        independent.PointerDown(75.0, audioY, 700.0, 10);
        independent.PointerDrag(120.0, audioY, 700.0);
        Check(independent.MovePreview() &&
                  independent.MovePreview()->timeline_in ==
                      RationalTime{10, 10} &&
                  independent.SnapGuideTime() == RationalTime{10, 10},
              "independent audio snaps exactly back to its sync reference");
        independent.CancelDrag();
        const std::string beforeInvalid = document.SaveToString();
        const size_t logCount = log.AppliedCount();
        TimelineInteraction interaction(document, log, viewport);
        const double y = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(125.0, y, 700.0, 10);
        interaction.PointerDrag(50.0, y, 700.0);
        Check(interaction.MovePreview() && interaction.MovePreview()->valid &&
                  interaction.MovePreview()->timeline_in ==
                      RationalTime{10, 10} &&
                  interaction.SnapGuideTime() == RationalTime{0, 10},
              "a partner at zero stops the whole linked selection");
        Check(!interaction.PointerUp(error, message) &&
                  document.SaveToString() == beforeInvalid &&
                  log.AppliedCount() == logCount,
              "a drop against the wall emits no edit and preserves bytes");
    });

    Test("linked trim previews and commits one atomic operation", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked trim fixture detaches audio");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string beforeTrim = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(200.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() &&
                  interaction.TrimPreview()->linked_times.size() == 2 &&
                  interaction.TrimPreview()->valid,
              "linked trim previews both A/V rectangles");
        Check(pending &&
                  std::holds_alternative<TrimLinkedClipsOperation>(*pending),
              "linked edge drag emits TrimLinkedClips");
        Check(interaction.PointerUp(error, message),
              "linked trim commits: " + message);
        Check(document.FindClip(videoId)->timeline_in == RationalTime{15, 10} &&
                  document.FindClip(audioId)->timeline_in ==
                      RationalTime{15, 10} &&
                  ClipSyncDrift(document, audioId) == RationalTime{0, 1},
              "linked head trim applies the same exact delta without drift");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "TrimLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeTrim,
              "one undo restores the pre-trim document bytes");
    });

    // Out of lot -- the author, 29 October 2026: while a head trim was held,
    // the waveform squeezed the whole committed source range into the previewed
    // width instead of losing the part the edge had passed.
    Test("a held trim cuts the drawn source window, it does not squeeze it",
         [] {
             Document document = Fixture();
             document.sequence.tracks.push_back(
                 {"01KT0000000000000000000005", "audio", 1, {}});
             EditLog log;
             EditError error = EditError::None;
             std::string message;
             const Ulid videoId = document.sequence.tracks[0].clips[0].id;
             Operation detach =
                 DetachAudioOperation{videoId,
                                      document.sequence.tracks[1].id,
                                      "01KT0000000000000000000006",
                                      {}};
             Check(log.Apply(document, std::move(detach), error, message),
                   "waveform fixture detaches audio");
             const Ulid audioId = document.sequence.tracks[1].clips[0].id;
             TimelineViewport viewport = Viewport();
             TimelineInteraction interaction(document, log, viewport);
             const double videoY = kTimelineRulerHeight + 20.0;
             const auto audioRect = [&](const auto& preview) {
                 const auto rects = VisibleTimelineClips(document, viewport,
                                                         700.0, {}, preview);
                 const auto found =
                     std::find_if(rects.begin(), rects.end(),
                                  [&](const TimelineClipRect& rect) {
                                      return rect.clip_id == audioId;
                                  });
                 return found == rects.end()
                            ? std::optional<TimelineClipRect>{}
                            : std::optional<TimelineClipRect>{*found};
             };
             const auto still = audioRect(std::nullopt);
             Check(still && still->source_in == RationalTime{10, 10} &&
                       still->source_span == RationalTime{20, 10},
                   "an idle audio rectangle shows its committed source range");
             interaction.PointerDown(150.0, videoY, 700.0, 10);
             interaction.PointerDrag(200.0, videoY, 700.0);
             const auto held = audioRect(interaction.TrimPreview());
             Check(held && held->preview &&
                       held->source_in == RationalTime{15, 10} &&
                       held->source_span == RationalTime{15, 10},
                   "a held head trim starts the drawn range half a second "
                   "later and keeps only what is left");
             Check(held && std::abs(held->width / 1.5 - 100.0) < 1e-9,
                   "the previewed width still draws 100 px per source "
                   "second: cut, not squeezed");
             interaction.CancelDrag();
             document.sequence.tracks[1].clips[0].speed = {2, 1};
             const auto fast = audioRect(std::nullopt);
             Check(fast && fast->source_span == RationalTime{40, 10},
                   "a clip at speed 2 shows twice its duration of source");
         });

    // The author, 31 October 2026 -- this used to hold the opposite: a linked
    // member that could not follow made the whole trim invalid, the
    // preview red and the release a no-op. A linked trim now stops where
    // its shortest member runs out, as Resolve's does.
    Test("a linked trim stops where its shortest member runs out", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Operation detach =
            DetachAudioOperation{document.sequence.tracks[0].clips[0].id,
                                 document.sequence.tracks[1].id,
                                 "01KT0000000000000000000006",
                                 {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "invalid linked trim fixture detaches audio");
        document.sequence.tracks[1].clips[0].duration = {2, 10};
        const std::string before = document.SaveToString();
        const size_t logCount = log.AppliedCount();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(180.0, videoY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid,
              "the 0.2 s sound holds the head trim back instead of refusing "
              "it");
        Check(interaction.PointerUp(error, message) &&
                  log.AppliedCount() == logCount + 1 &&
                  document.sequence.tracks[0].clips[0].source_in ==
                      RationalTime{11, 10} &&
                  document.sequence.tracks[1].clips[0].duration ==
                      RationalTime{1, 10},
              "both heads move 0.1 s, the sound keeping one frame: " + message);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "and one undo gives every byte back");
    });

    Test("linked delete is atomic and reversible", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Operation detach = DetachAudioOperation{videoId,
                                                document.sequence.tracks[1].id,
                                                "01KT0000000000000000000006",
                                                {}};
        Check(log.Apply(document, std::move(detach), error, message),
              "linked delete fixture detaches audio");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const Ulid group = document.FindClip(videoId)->link_group_id;
        const std::string before = document.SaveToString();
        Operation remove =
            RemoveLinkedClipsOperation{group, {videoId, audioId}, {}};
        Check(log.Apply(document, std::move(remove), error, message) &&
                  !document.FindClip(videoId) && !document.FindClip(audioId),
              "linked delete removes both members in one event");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "RemoveLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked delete undo restores exact document bytes");
        const Ulid followingVideoId = document.sequence.tracks[0].clips[1].id;
        const RationalTime followingPosition =
            document.sequence.tracks[0].clips[1].timeline_in;
        Operation clear =
            ClearLinkedClipsOperation{group, {videoId, audioId}, {}};
        Check(log.Apply(document, std::move(clear), error, message) &&
                  !document.FindClip(videoId) && !document.FindClip(audioId) &&
                  document.FindClip(followingVideoId)->timeline_in ==
                      followingPosition,
              "linked clear removes A/V and leaves following clips in place");
        const std::string clearJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(clearJson, parsed, error, message) &&
                  std::holds_alternative<ClearLinkedClipsOperation>(parsed) &&
                  SerializeOperation(parsed) == clearJson,
              "ClearLinkedClips JSON round-trips as its own operation");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "linked clear undo restores both tracks exactly");
    });

    Test("pan, cursor-centered zoom and fit preserve navigation invariants",
         [] {
             TimelineViewport viewport = Viewport();
             const double anchorX = 425.0;
             const RationalTime before = viewport.XToTime(anchorX, 1000);
             viewport.ZoomAroundX(anchorX, 2.0, 1000);
             Check(viewport.XToTime(anchorX, 1000) == before,
                   "zoom keeps the cursor time anchored");
             const RationalTime start = viewport.view_start;
             viewport.ScrollByPixels(100.0, 1000);
             Check(viewport.view_start > start,
                   "positive pan advances the visible start");
             viewport.FitDuration({200, 10}, 1000.0);
             Check(viewport.view_start == RationalTime{0, 10},
                   "fit starts at timeline zero");
             Check(viewport.TimeToX({200, 10}) <= 1000.0,
                   "fit keeps the timeline end visible");

             const RationalTime longFormDuration{6 * 60 * 60, 1};
             viewport.FitDuration(longFormDuration, 1000.0);
             Check(viewport.pixels_per_second < 4.0,
                   "long-form fit can zoom out past the former three-minute "
                   "floor");
             Check(viewport.TimeToX(longFormDuration) <= 1000.0,
                   "six-hour timelines fit in an ordinary editor pane");
         });

    Test("timeline navigation never exposes negative time", [] {
        TimelineViewport viewport = Viewport();
        viewport.ScrollByPixels(-1000.0, 1000);
        Check(viewport.view_start == RationalTime{0, 1000},
              "trackpad pan clamps the visible start to zero");
        viewport.ZoomAroundX(425.0, 0.5, 1000);
        Check(viewport.view_start == RationalTime{0, 1000},
              "zooming out near the origin cannot reveal negative time");
    });

    Test("mouse wheel and trackpad resolve to distinct navigation intents", [] {
        const TimelineScrollIntent wheel =
            ResolveTimelineScrollIntent(0.0, 1.0, false, false);
        Check(wheel.action == TimelineScrollAction::Zoom && wheel.delta == 1.0,
              "a discrete wheel zooms without a modifier");
        const TimelineScrollIntent shifted =
            ResolveTimelineScrollIntent(0.0, -1.0, false, true);
        Check(shifted.action == TimelineScrollAction::Pan &&
                  shifted.delta == -32.0,
              "Shift plus wheel pans by a useful logical-point step");
        const TimelineScrollIntent trackpad =
            ResolveTimelineScrollIntent(7.5, 0.0, true, false);
        Check(trackpad.action == TimelineScrollAction::Pan &&
                  trackpad.delta == 7.5,
              "precise two-finger deltas pan without changing scale");
    });

    Test("zoom scroll bar geometry and drags follow the viewport", [] {
        TimelineViewport viewport = Viewport();
        viewport.view_start = {50, 10};
        const RationalTime duration{200, 10};
        const double width = 1050.0;
        const TimelineZoomBarGeometry geometry =
            CalculateTimelineZoomBarGeometry(viewport, duration, width);
        Check(std::abs(geometry.thumb_x - 300.0) < 0.001 &&
                  std::abs(geometry.thumb_width - 500.0) < 0.001,
              "thumb position and width represent the visible time range");
        Check(geometry.HitTest(302.0) == TimelineZoomBarPart::LeftHandle &&
                  geometry.HitTest(550.0) == TimelineZoomBarPart::Body &&
                  geometry.HitTest(798.0) == TimelineZoomBarPart::RightHandle,
              "the rendered thumb exposes two handles and a draggable body");

        TimelineZoomBarDrag body{TimelineZoomBarPart::Body, 550.0, viewport,
                                 geometry};
        UpdateTimelineZoomBarDrag(viewport, body, 650.0, duration, width, 10);
        Check(viewport.view_start == RationalTime{70, 10} &&
                  std::abs(viewport.pixels_per_second - 100.0) < 0.001,
              "dragging the thumb pans without changing zoom");

        TimelineViewport zoomed = body.initial_viewport;
        TimelineZoomBarDrag right{TimelineZoomBarPart::RightHandle, 800.0,
                                  zoomed, geometry};
        UpdateTimelineZoomBarDrag(zoomed, right, 900.0, duration, width, 10);
        Check(zoomed.pixels_per_second < 100.0,
              "dragging the right handle outward zooms out");
        UpdateTimelineZoomBarDrag(zoomed, right, 2000.0, duration, width, 10);
        Check(
            zoomed.TimeToX(duration) < width && zoomed.pixels_per_second < 40.0,
            "dragging beyond the rail continues zooming past fit duration");
        // Visual QA of 1 October 2026 -- the view starts at 5 s and now
        // reaches well past the program's end: the rail stands for 0 s to
        // the end of the view, the thumb for what is seen of it.
        const TimelineZoomBarGeometry zoomedOutGeometry =
            CalculateTimelineZoomBarGeometry(zoomed, duration, width);
        Check(std::abs(zoomedOutGeometry.thumb_x +
                       zoomedOutGeometry.thumb_width - width) < 0.001 &&
                  zoomedOutGeometry.thumb_x > zoomedOutGeometry.rail_x &&
                  zoomedOutGeometry.thumb_width >
                      zoomedOutGeometry.rail_width * 0.5,
              "a view reaching past the program ends the thumb at the end of "
              "the rail, the hidden start left of it");
        TimelineZoomBarDrag zoomedOutBody{
            TimelineZoomBarPart::Body,
            zoomedOutGeometry.thumb_x + zoomedOutGeometry.thumb_width * 0.5,
            zoomed, zoomedOutGeometry};
        TimelineViewport further = zoomed;
        UpdateTimelineZoomBarDrag(further, zoomedOutBody,
                                  zoomedOutBody.pointer_x + 100.0, duration,
                                  width, 10);
        Check(further.view_start == zoomed.view_start,
              "that thumb has nowhere further to go");
        UpdateTimelineZoomBarDrag(zoomed, zoomedOutBody,
                                  zoomedOutBody.pointer_x - 1000.0, duration,
                                  width, 10);
        Check(zoomed.view_start == RationalTime{0, 10},
              "and comes back to the start of the program");
    });

    Test("a zoom bar handle stays under the pointer that holds it", [] {
        // Visual QA of 1 October 2026 -- a 57 s film fitted to the window,
        // the state the editor opens in. The thumb was half the rail there,
        // and pulling its right handle 25 points drew it 592 points away.
        const RationalTime duration{1429, 25};
        const double width = 1280.0;
        TimelineViewport fitted;
        fitted.header_width = 96.0;
        fitted.FitDuration(duration, width);
        const TimelineZoomBarGeometry whole =
            CalculateTimelineZoomBarGeometry(fitted, duration, width);
        Check(std::abs(whole.thumb_x - whole.rail_x) < 0.001 &&
                  std::abs(whole.thumb_width - whole.rail_width) < 0.001,
              "a program shown whole fills the rail");
        const double right = whole.thumb_x + whole.thumb_width;

        const TimelineZoomBarDrag pullRight{TimelineZoomBarPart::RightHandle,
                                            right - 2.0, fitted, whole};
        double previousWidth = whole.thumb_width;
        for (const double pull : {10.0, 25.0, 50.0, 200.0, 600.0}) {
            TimelineViewport during = fitted;
            UpdateTimelineZoomBarDrag(during, pullRight,
                                      pullRight.pointer_x - pull, duration,
                                      width, kTimelineViewRate);
            const TimelineZoomBarGeometry held =
                CalculateTimelineZoomBarGeometry(during, duration, width,
                                                 whole.ruler_seconds);
            Check(std::abs(held.thumb_x + held.thumb_width - (right - pull)) <
                          0.5 &&
                      std::abs(held.thumb_x - whole.thumb_x) < 0.5,
                  "during the drag the right handle is where the pointer "
                  "is and the left one has not moved");
            const TimelineZoomBarGeometry released =
                CalculateTimelineZoomBarGeometry(during, duration, width);
            // A fitted view shows a margin after the program's end (24
            // points here): a pull shorter than it still shows the whole
            // program, and the thumb goes back to the whole rail.
            Check(released.thumb_width <= previousWidth &&
                      std::abs(released.thumb_x + released.thumb_width -
                               (right - pull)) < whole.rail_width * 0.03,
                  "released, the thumb settles within the fit margin of "
                  "where it was held, and never jumps");
            previousWidth = released.thumb_width;
        }

        const TimelineZoomBarDrag pullLeft{TimelineZoomBarPart::LeftHandle,
                                           whole.thumb_x + 2.0, fitted, whole};
        TimelineViewport during = fitted;
        UpdateTimelineZoomBarDrag(during, pullLeft, pullLeft.pointer_x + 150.0,
                                  duration, width, kTimelineViewRate);
        const TimelineZoomBarGeometry held = CalculateTimelineZoomBarGeometry(
            during, duration, width, whole.ruler_seconds);
        Check(std::abs(held.thumb_x - (whole.thumb_x + 150.0)) < 0.5 &&
                  std::abs(held.thumb_x + held.thumb_width - right) < 0.5,
              "the left handle follows the pointer and the right one stays");
    });

    Test("a wheel notch zooms by the same step at any speed", [] {
        // Visual QA of 1 October 2026 -- macOS accelerates a wheel's delta:
        // about 0.1 for a notch turned slowly, 10 and more for a flick.
        const double slowNotch = TimelineScrollZoomFactor(-0.1, false);
        Check(slowNotch == TimelineScrollZoomFactor(-1.0, false) &&
                  slowNotch > 1.15 && slowNotch < 1.18,
              "a notch is 16 %, turned slowly or not");
        Check(std::abs(TimelineScrollZoomFactor(0.1, false) * slowNotch - 1.0) <
                  1e-12,
              "a notch back undoes a notch forward");
        const double tenSlow = std::pow(slowNotch, 10.0);
        const double tenFast =
            std::pow(TimelineScrollZoomFactor(-10.0, false), 10.0);
        Check(tenSlow > 4.0 && tenFast < 25.0 && tenFast > tenSlow,
              "ten slow notches zoom by 4, ten flicked ones by no more than "
              "25 (they did 1.13 and 197)");
        double trackpad = 1.0;
        for (int event = 0; event < 7; ++event)
            trackpad *= TimelineScrollZoomFactor(-10.0, true);
        Check(std::abs(trackpad - 2.0) < 0.02,
              "70 points of precise scroll double the scale");
        Check(TimelineScrollZoomFactor(-1000.0, true) < 1.36,
              "one precise event is capped");
        Check(TimelineScrollZoomFactor(0.0, false) == 1.0 &&
                  TimelineScrollZoomFactor(std::nan(""), true) == 1.0,
              "nothing scrolled, nothing zoomed");
    });

    Test("the zoom modifier makes any scroll a zoom", [] {
        const TimelineScrollIntent trackpad =
            ResolveTimelineScrollIntent(3.0, -8.0, true, false, true);
        Check(trackpad.action == TimelineScrollAction::Zoom &&
                  trackpad.delta == -8.0,
              "a precise vertical scroll zooms, by the axis it mostly follows");
        const TimelineScrollIntent sideways =
            ResolveTimelineScrollIntent(-9.0, 2.0, true, false, true);
        Check(sideways.action == TimelineScrollAction::Zoom &&
                  sideways.delta == -9.0,
              "a mostly horizontal one too");
        Check(
            ResolveTimelineScrollIntent(0.0, -1.0, false, true, true).action ==
                TimelineScrollAction::Zoom,
            "the modifier wins over Shift");
        Check(ResolveTimelineScrollIntent(0.0, -8.0, true, false).action ==
                  TimelineScrollAction::Pan,
              "without it a precise scroll still pans");
        const TimelineScrollIntent slowShift =
            ResolveTimelineScrollIntent(0.0, -0.1, false, true);
        Check(slowShift.action == TimelineScrollAction::Pan &&
                  slowShift.delta == -32.0,
              "a slow Shift notch pans a whole step, not a tenth of one");
    });

    Test("zooming out and back in at the pointer returns where it left", [] {
        // Visual QA of 1 October 2026 -- the wheel events of the trace: a 57 s
        // film, the pointer at x = 200, out to a film 104 points wide and
        // back. The view came back on 54 s; it had left from 7 s.
        const RationalTime duration{1429, 25};
        const double width = 1280.0;
        TimelineViewport viewport;
        viewport.header_width = 96.0;
        viewport.pixels_per_second = 14.888;
        viewport.view_start = {0, 25};
        const double pointer = 200.4;
        const RationalTime pointed =
            viewport.XToTime(pointer, kTimelineViewRate);
        std::optional<TimelineZoomAnchor> anchor;
        const auto wheel = [&](double delta, int events, double x) {
            for (int event = 0; event < events; ++event)
                anchor = ZoomTimelineAtPointer(
                    viewport, x, TimelineScrollZoomFactor(delta, false),
                    duration, width, anchor);
        };
        wheel(4.0, 9, pointer);
        Check(viewport.pixels_per_second < 14.888 &&
                  viewport.view_start == RationalTime{0, 1},
              "zoomed out, the view is pinned at the program's start");
        // The hand is not a tripod: the pointer shifts by a point or two.
        wheel(-4.0, 12, pointer + 1.6);
        Check(std::abs(viewport.TimeToX(pointed) - pointer) < 0.5,
              "zoomed back in, the time first pointed at is under the pointer "
              "again");
        Check(viewport.view_start < RationalTime{7, 1},
              "and the view is nowhere near the end of the film");

        const double zoomedIn = viewport.pixels_per_second;
        wheel(-4.0, 1, pointer + 300.0);
        Check(std::abs(viewport.TimeToX(anchor->time) - (pointer + 300.0)) <
                      0.5 &&
                  viewport.pixels_per_second > zoomedIn,
              "a pointer that moved takes a new anchor");
        viewport.ScrollByPixels(120.0, kTimelineViewRate);
        const RationalTime afterPan =
            viewport.XToTime(pointer + 300.0, kTimelineViewRate);
        wheel(-4.0, 1, pointer + 300.0);
        Check(anchor->time == afterPan,
              "a view moved by anything else takes a new anchor too");

        // The pointer on the right of the window, the film shrunk to its
        // left: the anchor is the film's end, not the empty time after it.
        TimelineViewport shrunk;
        shrunk.header_width = 96.0;
        shrunk.pixels_per_second = 2.0;
        shrunk.view_start = {0, 25};
        std::optional<TimelineZoomAnchor> fresh;
        for (int event = 0; event < 12; ++event)
            fresh = ZoomTimelineAtPointer(shrunk, 900.0,
                                          TimelineScrollZoomFactor(-4.0, false),
                                          duration, width, fresh);
        Check(fresh->time == duration &&
                  std::abs(shrunk.TimeToX(duration) - 900.0) < 0.5,
              "a pointer past the end of the film zooms on its end");
    });

    Test("a zoom out stops while the film still fills half the view", [] {
        // Visual QA of 1 October 2026 -- the second trace: out to a film
        // 1.4 points wide, the pointer 19 points into the timeline, and
        // back in to 56.8 s, the end of the film.
        const RationalTime duration{1429, 25};
        const double width = 1280.0;
        const double rail = width - 96.0;
        TimelineViewport viewport;
        viewport.header_width = 96.0;
        viewport.FitDuration(duration, width);
        std::optional<TimelineZoomAnchor> anchor;
        for (int event = 0; event < 40; ++event)
            anchor = ZoomTimelineAtPointer(viewport, 291.0,
                                           TimelineScrollZoomFactor(8.0, false),
                                           duration, width, anchor);
        Check(std::abs(viewport.TimeToX(duration) - (96.0 + rail * 0.5)) < 0.5,
              "forty flicks out leave the film on half the view");
        for (int event = 0; event < 20; ++event)
            anchor = ZoomTimelineAtPointer(
                viewport, 115.0, TimelineScrollZoomFactor(-7.0, false),
                duration, width, anchor);
        Check(viewport.view_start < RationalTime{3, 1} &&
                  viewport.pixels_per_second > 400.0,
              "zooming back in near the left edge lands near the film's "
              "start, not on its end");

        TimelineViewport byBar;
        byBar.header_width = 96.0;
        byBar.FitDuration(duration, width);
        const TimelineZoomBarGeometry whole =
            CalculateTimelineZoomBarGeometry(byBar, duration, width);
        const TimelineZoomBarDrag far{TimelineZoomBarPart::RightHandle,
                                      whole.thumb_x + whole.thumb_width - 2.0,
                                      byBar, whole};
        UpdateTimelineZoomBarDrag(byBar, far, far.pointer_x + 50000.0, duration,
                                  width, kTimelineViewRate);
        Check(std::abs(byBar.TimeToX(duration) - (96.0 + rail * 0.5)) < 0.5,
              "the bar's handle stops at the same floor");

        TimelineViewport empty;
        empty.header_width = 96.0;
        Check(TimelineZoomOutFloor(empty, {0, 25}, width) ==
                  kTimelineMinPixelsPerSecond,
              "an empty timeline has no floor of its own");

        // A view already beyond the floor is not snapped in by a zoom out.
        TimelineViewport legacy;
        legacy.header_width = 96.0;
        legacy.pixels_per_second = 2.0;
        ZoomTimelineAtPointer(legacy, 300.0, 0.5, duration, width,
                              std::nullopt);
        Check(legacy.pixels_per_second == 2.0,
              "a view further out than the floor stays where it is");
    });

    Test("the wheel zooms around the playhead, as the editor asked", [] {
        // Visual QA of 1 October 2026 -- "je veux le zoom de Resolve".
        const RationalTime duration{1429, 25};
        const double width = 971.0;  // the traced window: a rail of 875
        const RationalTime playhead{282, 25};
        TimelineViewport viewport;
        viewport.header_width = 96.0;
        viewport.FitDuration(duration, width);
        const double placed = viewport.TimeToX(playhead);
        std::optional<TimelineZoomAnchor> anchor;
        const auto wheel = [&](double delta, int events,
                               const RationalTime& at) {
            for (int event = 0; event < events; ++event)
                anchor = ZoomTimelineAtPlayhead(
                    viewport, at, TimelineScrollZoomFactor(delta, false),
                    duration, width, anchor);
        };
        wheel(-4.0, 10, playhead);
        Check(viewport.pixels_per_second > 200.0 &&
                  std::abs(viewport.TimeToX(playhead) - placed) < 0.5,
              "zoomed in ten notches, the playhead has not moved on screen");
        wheel(4.0, 40, playhead);
        Check(viewport.view_start == RationalTime{0, 1} &&
                  std::abs(viewport.TimeToX(duration) -
                           (96.0 + (width - 96.0) * 0.5)) < 0.5,
              "zoomed out to the floor, the film starts at the left edge "
              "and fills half the view");
        wheel(-4.0, 10, playhead);
        Check(viewport.pixels_per_second > 30.0 &&
                  std::abs(viewport.TimeToX(playhead) - placed) < 0.5,
              "zoomed back in, the playhead is where it was: not the "
              "film's end, not the pointer");

        // The playhead out of sight comes to the middle of the view.
        TimelineViewport away;
        away.header_width = 96.0;
        away.pixels_per_second = 100.0;
        away.view_start = {40, 1};
        std::optional<TimelineZoomAnchor> fresh;
        fresh = ZoomTimelineAtPlayhead(away, playhead, 1.16, duration, width,
                                       fresh);
        Check(std::abs(away.TimeToX(playhead) - (96.0 + (width - 96.0) * 0.5)) <
                  0.5,
              "a playhead out of sight is brought to the middle");

        // A playhead that moved, still in sight, is followed where it is.
        const RationalTime later{307, 25};
        const double laterX = viewport.TimeToX(later);
        Check(laterX > placed + 50.0 && laterX < width,
              "(the moved playhead is on screen, right of the first one)");
        wheel(-4.0, 3, later);
        Check(std::abs(viewport.TimeToX(later) - laterX) < 0.5,
              "a playhead that moved keeps the place it has now");

        // At the film's end, the end stays at the middle at the latest.
        TimelineViewport atEnd;
        atEnd.header_width = 96.0;
        atEnd.FitDuration(duration, width);
        std::optional<TimelineZoomAnchor> endAnchor;
        for (int event = 0; event < 10; ++event)
            endAnchor = ZoomTimelineAtPlayhead(
                atEnd, duration, TimelineScrollZoomFactor(-4.0, false),
                duration, width, endAnchor);
        Check(atEnd.TimeToX(duration) >= 96.0 + (width - 96.0) * 0.5 - 0.5,
              "the film's end never goes left of the middle of the view");
    });

    Test("a wheel pushed away zooms in whatever the system's direction", [] {
        // The trace again: natural scrolling off, a wheel turned away from
        // the editor reports a positive delta, and zoomed out.
        const TimelineScrollIntent away =
            ResolveTimelineScrollIntent(0.0, 1.0, false, false, false, false);
        Check(away.action == TimelineScrollAction::Zoom &&
                  TimelineScrollZoomFactor(away.delta, false) > 1.0,
              "natural scrolling off: away is a zoom in");
        const TimelineScrollIntent natural =
            ResolveTimelineScrollIntent(0.0, -1.0, false, false, false, true);
        Check(natural.action == TimelineScrollAction::Zoom &&
                  TimelineScrollZoomFactor(natural.delta, false) > 1.0,
              "natural scrolling on: the same hand, the same zoom in");
        const TimelineScrollIntent optionAway =
            ResolveTimelineScrollIntent(0.0, 1.0, false, false, true, false);
        Check(TimelineScrollZoomFactor(optionAway.delta, false) > 1.0,
              "with the zoom modifier too");
        Check(ResolveTimelineScrollIntent(0.0, 1.0, false, true, false, false)
                      .delta == 32.0,
              "a pan is left as the system reports it");
    });

    Test("the view moves in time finer than a frame", [] {
        // Visual QA of 1 October 2026 -- moved at the sequence's 25 fps, a
        // view zoomed to 400 points a second swallowed a slow pan whole.
        for (const double scale : {50.0, 200.0, 400.0, 1000.0, 4000.0}) {
            TimelineViewport viewport;
            viewport.header_width = 96.0;
            viewport.pixels_per_second = scale;
            viewport.view_start = {250, 25};
            const double before = viewport.TimeToX({500, 25});
            for (int event = 0; event < 60; ++event)
                viewport.ScrollByPixels(3.0, kTimelineViewRate);
            Check(std::abs(before - viewport.TimeToX({500, 25}) - 180.0) < 1.0,
                  "sixty pans of 3 points move the timeline 180 points");

            viewport.view_start = {250, 25};
            const double pointer = 613.0;
            const long double pointed =
                10.0L + (pointer - viewport.header_width) / scale;
            double worst = 0.0;
            for (int event = 0; event < 12; ++event) {
                viewport.ZoomAroundX(pointer, event % 2 ? 1.0 / 1.16 : 1.16,
                                     kTimelineViewRate);
                const long double x =
                    viewport.header_width +
                    (pointed -
                     static_cast<long double>(viewport.view_start.value) /
                         viewport.view_start.rate) *
                        viewport.pixels_per_second;
                worst =
                    std::max(worst, std::abs(static_cast<double>(x) - pointer));
            }
            Check(worst < 0.5,
                  "the time under the pointer stays under it through twelve "
                  "zooms");
        }
    });

    Test("simulated trim emits the canonical operation and undo restores hash",
         [] {
             Document document = Fixture();
             const uint64_t initialHash = Hash(document.SaveToString());
             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             const double trackY = kTimelineRulerHeight + 20.0;
             interaction.PointerDown(350.0, trackY, 700.0, 10);
             interaction.PointerDrag(300.0, trackY, 700.0);
             const auto pending = interaction.PendingOperation();
             Check(
                 pending && std::holds_alternative<TrimClipOperation>(*pending),
                 "drag creates a TrimClip operation");
             if (pending) {
                 const auto& trim = std::get<TrimClipOperation>(*pending);
                 Check(
                     trim.clip_id == document.sequence.tracks[0].clips[0].id &&
                         trim.edge == TrimEdge::Tail &&
                         trim.delta == RationalTime{-5, 10},
                     "tail drag has the expected exact delta");
             }
             EditError error = EditError::None;
             std::string message;
             Check(interaction.PointerUp(error, message),
                   "valid drag applies through the edit log: " + message);
             Check(log.AppliedCount() == 1,
                   "one mouse gesture emits exactly one operation");
             Check(document.sequence.tracks[0].clips[0].duration ==
                       RationalTime{15, 10},
                   "document contains the trim result");
             Check(log.Undo(document, error, message),
                   "trim undo succeeds: " + message);
             Check(Hash(document.SaveToString()) == initialHash,
                   "undo restores the initial canonical document hash");
         });

    Test("ripple trim gesture shifts downstream sync-locked clips", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back({"01KT0000000000000000000005",
                                            "audio",
                                            1,
                                            {{"01KT0000000000000000000006",
                                              "01KT0000000000000000000001",
                                              {40, 10},
                                              {10, 10},
                                              {40, 10}}}});
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Ripple);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, videoY, 700.0, 10);
        interaction.PointerDrag(400.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->linked_times.size() == 2,
              "ripple preview includes primary and sync followers");
        Check(pending && std::holds_alternative<RippleTrimOperation>(*pending),
              "ripple gesture emits its dedicated atomic operation");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "ripple trim commits: " + message);
        Check(document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{25, 10} &&
                  document.sequence.tracks[0].clips[1].timeline_in ==
                      RationalTime{45, 10} &&
                  document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{45, 10},
              "ripple extends the edit and moves every sync follower exactly");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "one undo restores the ripple gesture byte-exactly");
    });

    // The author, 31 October 2026: the trim "ne refuse pas proprement d'étirer
    // un plan au delà de la limite". A ripple and a roll stop at the last frame
    // the rush has, as a trim that leaves the track in place always did,
    // instead of going red and committing nothing.
    Test("a ripple past the end of the rush stops at its last frame", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Ripple);
        const double videoY = kTimelineRulerHeight + 20.0;
        // The first clip ends at 3 s of a 10 s rush; dragged 17 s on.
        interaction.PointerDown(350.0, videoY, 700.0, 10);
        interaction.PointerDrag(2050.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        const auto* ripple =
            pending ? std::get_if<RippleTrimOperation>(&*pending) : nullptr;
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  ripple && ripple->delta == RationalTime{70, 10},
              "the tail stops 7 s on, at the rush's end");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message) &&
                  document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{90, 10} &&
                  document.sequence.tracks[0].clips[1].timeline_in ==
                      RationalTime{110, 10},
              "and the release commits it, the next clip carried along: " +
                  message);
    });

    Test("a ripple of a head stops at the rush's first frame", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips[1].source_in = {2, 10};
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Ripple);
        const double videoY = kTimelineRulerHeight + 20.0;
        // The second clip reads its rush from 0.2 s; its head pulled 1 s
        // out.
        interaction.PointerDown(450.0, videoY, 700.0, 10);
        interaction.PointerDrag(350.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        const auto* ripple =
            pending ? std::get_if<RippleTrimOperation>(&*pending) : nullptr;
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  ripple && ripple->delta == RationalTime{-2, 10},
              "the head stops 0.2 s out, on the rush's first frame");
    });

    Test("a roll stops where either side of the cut runs out", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips[1].timeline_in = {30, 10};
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Roll);
        const double videoY = kTimelineRulerHeight + 20.0;
        // The cut at 3 s moved 3 s on: the clip after it is 1 s long.
        interaction.PointerDown(350.0, videoY, 700.0, 10);
        interaction.PointerDrag(650.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        const auto* roll =
            pending ? std::get_if<RollEditOperation>(&*pending) : nullptr;
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  roll && roll->delta == RationalTime{9, 10},
              "the cut stops one frame short of the next clip's end");
    });

    Test(
        "a linked ripple stops at its shorter partner, and ignores other "
        "selected clips",
        [] {
            Document document = Fixture();
            // A recorder's sound linked to the first clip, 3.5 s long: it
            // runs out 0.5 s after the picture's tail.
            document.sources.push_back({"01KT0000000000000000000007",
                                        "recorder.wav",
                                        {10, 1},
                                        {35, 10}});
            const Ulid group = "01KT0000000000000000000008";
            document.sequence.tracks[0].clips[0].link_group_id = group;
            DocumentClip sound;
            sound.id = "01KT0000000000000000000009";
            sound.source_id = "01KT0000000000000000000007";
            sound.source_in = {10, 10};
            sound.duration = {20, 10};
            sound.timeline_in = {10, 10};
            sound.link_group_id = group;
            document.sequence.tracks.push_back(
                {"01KT0000000000000000000005", "audio", 1, {sound}});
            TimelineViewport viewport = Viewport();
            EditLog log;
            TimelineInteraction interaction(document, log, viewport);
            interaction.SetTrimMode(TimelineTrimMode::Ripple);
            // The picture, its sound and the second clip, of no group.
            interaction.SelectClips({document.sequence.tracks[0].clips[0].id,
                                     sound.id,
                                     document.sequence.tracks[0].clips[1].id});
            const double videoY = kTimelineRulerHeight + 20.0;
            interaction.PointerDown(350.0, videoY, 700.0, 10);
            interaction.PointerDrag(2050.0, videoY, 700.0);
            const auto pending = interaction.PendingOperation();
            const auto* ripple =
                pending ? std::get_if<RippleTrimOperation>(&*pending) : nullptr;
            Check(interaction.TrimPreview() &&
                      interaction.TrimPreview()->valid && ripple &&
                      ripple->delta == RationalTime{5, 10} &&
                      ripple->linked_clip_ids == std::vector<Ulid>{sound.id},
                  "the ripple stops 0.5 s on, where the sound ends, and "
                  "takes the sound alone along");
        });

    // ON9 -- the centre of a cut rolls it, as in Resolve (p. 1065).
    Test("the centre of a cut between two clips is a roll", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips[1].timeline_in = {30, 10};
        const TimelineViewport viewport = Viewport();
        const double y = kTimelineRulerHeight + 20.0;
        const auto at = [&](double x, TimelinePointerTool tool,
                            TimelinePointerModifiers modifiers = {}) {
            return TimelineHitAt(document, viewport, x, y, 700.0, 400.0,
                                 RationalTime{60, 10}, tool, modifiers);
        };
        // The cut at 3 s is at x = 50 + 300.
        for (const double x : {348.0, 350.0, 352.0}) {
            const TimelinePointerTarget select =
                at(x, TimelinePointerTool::Select);
            const TimelinePointerTarget trim = at(x, TimelinePointerTool::Trim);
            Check(select.kind == TimelinePointerTargetKind::ClipEdge &&
                      select.trim_mode == TimelineTrimMode::Roll &&
                      trim.trim_mode == TimelineTrimMode::Roll,
                  "two points or less from the cut, both tools roll (x " +
                      std::to_string(x) + ")");
        }
        const TimelinePointerTarget out =
            at(346.0, TimelinePointerTool::Select);
        const TimelinePointerTarget in = at(354.0, TimelinePointerTool::Trim);
        Check(out.edge == TimelineHitEdge::Tail &&
                  out.trim_mode == TimelineTrimMode::Normal &&
                  in.edge == TimelineHitEdge::Head &&
                  in.trim_mode == TimelineTrimMode::Ripple,
              "four points off, each side trims its own edge");
        Check(at(346.0, TimelinePointerTool::Select, {false, false, true})
                      .trim_mode == TimelineTrimMode::Roll,
              "Command still rolls from anywhere on the edge");
        // The second clip ends at 4 s with nothing after it.
        const TimelinePointerTarget free =
            at(449.0, TimelinePointerTool::Select);
        Check(free.edge == TimelineHitEdge::Tail &&
                  free.trim_mode == TimelineTrimMode::Normal,
              "a free tail has no cut to roll");
    });

    Test("roll gesture moves one cut while preserving program duration", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips[1].timeline_in = {30, 10};
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Roll);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, videoY, 700.0, 10);
        interaction.PointerDrag(400.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->linked_times.size() == 1,
              "roll preview updates both sides of the cut");
        Check(pending && std::holds_alternative<RollEditOperation>(*pending),
              "roll gesture emits its dedicated atomic operation");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "roll edit commits: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips[0].duration == RationalTime{25, 10} &&
                  clips[1].source_in == RationalTime{45, 10} &&
                  clips[1].duration == RationalTime{5, 10} &&
                  clips[1].timeline_in == RationalTime{35, 10} &&
                  clips[1].timeline_in.add(clips[1].duration) ==
                      RationalTime{40, 10},
              "roll moves the cut without changing the sequence end");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "one undo restores the roll gesture byte-exactly");
    });

    Test("roll gesture moves the boundary between two cartons", [] {
        // Visual QA of 19 September 2026, 5b.7: the engine accepted the
        // roll, the mouse could not move the boundary.
        Document document;
        CaptionStyle style;
        document.sequence.caption_styles = {style};
        DocumentClip first{GenerateUlid(), {}, {0, 1}, {20, 10}, {10, 10}};
        first.caption_group_id = style.id;
        first.caption_text = "Bonjour";
        DocumentClip second{GenerateUlid(), {}, {0, 1}, {10, 10}, {30, 10}};
        second.caption_group_id = style.id;
        second.caption_text = "à tous";
        document.sequence.tracks = {
            {GenerateUlid(), "caption", 0, {first, second}}};
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Roll);
        const double captionY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, captionY, 700.0, 10);
        interaction.PointerDrag(370.0, captionY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid,
              "caption roll preview is valid");
        Check(pending && std::holds_alternative<RollEditOperation>(*pending),
              "caption roll emits a roll edit");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "caption roll commits: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips.size() == 2 && clips[0].duration == RationalTime{22, 10} &&
                  clips[1].timeline_in == RationalTime{32, 10} &&
                  clips[1].duration == RationalTime{8, 10} &&
                  clips[0].caption_text == "Bonjour" &&
                  clips[1].caption_text == "à tous",
              "the boundary moves and each carton keeps its text");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "one undo gives both cartons back");
    });

    Test("shift-click adds a clip to the selection, and takes it out", [] {
        // Visual QA of 19 September 2026, 2.2: on a full timeline nothing
        // selected a second clip.
        Document document = Fixture();
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const Ulid second = document.sequence.tracks[0].clips[1].id;
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        EditError error = EditError::None;
        std::string message;
        interaction.PointerDown(250.0, videoY, 700.0, 10);
        interaction.PointerUp(error, message);
        interaction.SetSelectionToggle(true);
        interaction.PointerDown(500.0, videoY, 700.0, 10);
        interaction.PointerUp(error, message);
        Check(
            interaction.SelectedClipIds() == std::vector<Ulid>{first, second} &&
                interaction.PrimarySelectedClipId() == second,
            "shift-click adds the clip and makes it the primary one");
        interaction.PointerDown(250.0, videoY, 700.0, 10);
        interaction.PointerUp(error, message);
        Check(interaction.SelectedClipIds() == std::vector<Ulid>{second} &&
                  interaction.PrimarySelectedClipId() == second &&
                  log.AppliedCount() == 0,
              "shift-click on a selected clip takes it out, editing nothing");

        interaction.PointerDown(250.0, videoY, 700.0, 10);
        interaction.PointerDrag(300.0, videoY, 700.0);
        Check(interaction.PointerUp(error, message),
              "the extended selection moves: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips[0].timeline_in == RationalTime{15, 10} &&
                  clips[1].timeline_in == RationalTime{45, 10} &&
                  log.AppliedCount() == 1,
              "a drag after shift-click moves every selected clip at once");

        // The first clip now ends at 3.5 s, x = 400.
        interaction.PointerDown(400.0, videoY, 700.0, 10);
        Check(interaction.SelectedClipIds() == std::vector<Ulid>{first},
              "on an edge, shift keeps its trim meaning and selects the clip");
    });

    Test("a roll does not snap back onto the cut it moves", [] {
        // The partner's edge is the cut itself: kept as a snap target, it
        // pulled every roll shorter than the snap distance back to nothing.
        Document document;
        CaptionStyle style;
        document.sequence.caption_styles = {style};
        DocumentClip first{GenerateUlid(), {}, {0, 1}, {200, 100}, {100, 100}};
        first.caption_group_id = style.id;
        first.caption_text = "Bonjour";
        DocumentClip second{GenerateUlid(), {}, {0, 1}, {100, 100}, {300, 100}};
        second.caption_group_id = style.id;
        second.caption_text = "à tous";
        document.sequence.tracks = {
            {GenerateUlid(), "caption", 0, {first, second}}};
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Roll);
        const double captionY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, captionY, 700.0, 100);
        interaction.PointerDrag(355.0, captionY, 700.0);
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "a short roll commits: " + message);
        const auto& clips = document.sequence.tracks[0].clips;
        Check(clips[0].duration == RationalTime{205, 100} &&
                  clips[1].timeline_in == RationalTime{305, 100},
              "a roll of five pixels moves the cut five pixels");
    });

    Test("a motion clip's head trims with the mouse", [] {
        // Visual QA of 19 September 2026, 5q.6.
        Document document;
        DocumentComposition composition;
        composition.name = "Titre";
        std::string error;
        Check(mcp_json::Value::Parse(
                  R"({"width":1080,"height":1920,"fps":10,"duration":60,)"
                  R"("root":{"type":"fill","uid":"01K80000000000000000000011",)"
                  R"("children":[]}})",
                  composition.definition, error),
              "composition parses: " + error);
        document.compositions = {composition};
        DocumentClip motion{GenerateUlid(), {}, {0, 10}, {20, 10}, {10, 10}};
        motion.composition_id = composition.id;
        document.sequence.tracks = {{GenerateUlid(), "video", 0, {motion}}};
        std::string validation;
        Check(document.Validate(validation), "fixture: " + validation);
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, videoY, 700.0, 10);
        interaction.PointerDrag(200.0, videoY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid,
              "motion head trim preview is valid");
        EditError editError = EditError::None;
        std::string message;
        Check(interaction.PointerUp(editError, message),
              "motion head trim commits: " + message);
        const DocumentClip& trimmed = document.sequence.tracks[0].clips[0];
        Check(trimmed.source_in == RationalTime{5, 10} &&
                  trimmed.timeline_in == RationalTime{15, 10} &&
                  trimmed.duration == RationalTime{15, 10},
              "the composition starts five frames later");
        // Dragged past the composition's first frame, the head stops on it,
        // as a media clip's head stops on its media's first frame.
        interaction.PointerDown(200.0, videoY, 700.0, 10);
        interaction.PointerDrag(80.0, videoY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid,
              "an overshooting head drag stays valid");
        Check(interaction.PointerUp(editError, message),
              "the overshooting head drag commits: " + message);
        const DocumentClip& extended = document.sequence.tracks[0].clips[0];
        Check(extended.source_in == RationalTime{0, 10} &&
                  extended.timeline_in == RationalTime{10, 10} &&
                  extended.duration == RationalTime{20, 10},
              "the head stops on the composition's first frame");
    });

    Test("slip gesture keeps linked A/V rectangles fixed", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(document,
                        DetachAudioOperation{videoId,
                                             document.sequence.tracks[1].id,
                                             "01KT0000000000000000000006",
                                             {}},
                        error, message),
              "slip fixture detaches audio");
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string before = document.SaveToString();
        TimelineViewport viewport = Viewport();
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetTrimMode(TimelineTrimMode::Slip);
        const double videoY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, videoY, 700.0, 10);
        interaction.PointerDrag(250.0, videoY, 700.0);
        const auto pending = interaction.PendingOperation();
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->linked_times.size() == 2,
              "slip previews both separate linked rectangles");
        Check(pending && std::holds_alternative<SlipEditOperation>(*pending),
              "slip gesture emits one dedicated operation");
        Check(interaction.PointerUp(error, message),
              "linked slip commits: " + message);
        Check(
            document.FindClip(videoId)->source_in == RationalTime{15, 10} &&
                document.FindClip(audioId)->source_in == RationalTime{15, 10} &&
                document.FindClip(videoId)->timeline_in ==
                    RationalTime{10, 10} &&
                document.FindClip(audioId)->timeline_in ==
                    RationalTime{10, 10} &&
                document.FindClip(videoId)->duration == RationalTime{20, 10} &&
                document.FindClip(audioId)->duration == RationalTime{20, 10} &&
                ClipSyncDrift(document, audioId) == RationalTime{0, 1},
            "slip changes only both source windows and preserves A/V sync");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores the linked slip byte-exactly");
    });

    Test("clip body drag moves atomically across tracks and undo restores hash",
         [] {
             Document document = Fixture();
             document.sequence.tracks.push_back(
                 {"01KT0000000000000000000005", "video", 1, {}});
             const std::string initial = document.SaveToString();
             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             const double firstTrackY = kTimelineRulerHeight + 20.0;
             const double secondTrackY =
                 kTimelineRulerHeight + viewport.track_height + 20.0;
             // V2 is displayed above V1, matching professional NLE stacking.
             interaction.PointerDown(200.0, secondTrackY, 700.0, 10);
             interaction.PointerDrag(250.0, firstTrackY, 700.0);
             Check(
                 interaction.MovePreview() && interaction.MovePreview()->valid,
                 "cross-track move preview is valid");
             const auto pending = interaction.PendingOperation();
             Check(
                 pending && std::holds_alternative<MoveClipOperation>(*pending),
                 "body drag emits MoveClip");
             if (pending) {
                 const auto& move = std::get<MoveClipOperation>(*pending);
                 Check(move.track_id == document.sequence.tracks[1].id &&
                           move.timeline_in == RationalTime{15, 10},
                       "move carries exact destination track and time");
                 const std::string json = SerializeOperation(*pending);
                 Operation parsed = RemoveClipOperation{};
                 EditError parseError = EditError::None;
                 std::string parseMessage;
                 Check(DeserializeOperation(json, parsed, parseError,
                                            parseMessage) &&
                           SerializeOperation(parsed) == json,
                       "MoveClip JSON round-trips canonically");
             }
             EditError error = EditError::None;
             std::string message;
             Check(interaction.PointerUp(error, message),
                   "move applies through one edit-log entry: " + message);
             Check(log.AppliedCount() == 1 &&
                       document.sequence.tracks[0].clips.size() == 1 &&
                       document.sequence.tracks[1].clips.size() == 1,
                   "one event transfers the clip between tracks");
             Check(log.Undo(document, error, message),
                   "move undo succeeds: " + message);
             Check(document.SaveToString() == initial,
                   "move undo restores canonical document bytes");
         });

    Test("overlapping clip move overwrites covered clips atomically", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(500.0, trackY, 700.0);
        Check(interaction.MovePreview() && interaction.MovePreview()->valid,
              "overlap is accepted as an overwrite preview");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message) && log.AppliedCount() == 1,
              "overwrite emits one atomic move event");
        Check(document.sequence.tracks[0].clips.size() == 1 &&
                  document.sequence.tracks[0].clips[0].id ==
                      "01KT0000000000000000000003" &&
                  document.sequence.tracks[0].clips[0].timeline_in ==
                      RationalTime{40, 10},
              "fully covered destination clip is removed");
        Check(log.Undo(document, error, message), "overwrite undo succeeds");
        Check(document.SaveToString() == initial,
              "overwrite undo restores document bytes");
    });

    Test("overwrite through a longer clip preserves both survivors", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back({"01KT0000000000000000000005",
                                            "video",
                                            1,
                                            {{"01KT0000000000000000000006",
                                              "01KT0000000000000000000001",
                                              {40, 10},
                                              {50, 10},
                                              {30, 10}}}});
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double firstTrackY = kTimelineRulerHeight + 20.0;
        const double secondTrackY =
            kTimelineRulerHeight + viewport.track_height + 20.0;
        interaction.PointerDown(200.0, secondTrackY, 900.0, 10);
        interaction.PointerDrag(600.0, firstTrackY, 900.0);
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "spanning overwrite applies: " + message);
        const auto& clips = document.sequence.tracks[1].clips;
        Check(clips.size() == 3,
              "spanning destination becomes left, moved and right clips");
        Check(clips[0].id == "01KT0000000000000000000006" &&
                  clips[0].timeline_in == RationalTime{30, 10} &&
                  clips[0].duration == RationalTime{20, 10},
              "left survivor retains its ULID");
        Check(clips[1].id == "01KT0000000000000000000003" &&
                  clips[1].timeline_in == RationalTime{50, 10},
              "moved clip occupies the overwrite interval");
        const Ulid rightSurvivorId = clips[2].id;
        Check(rightSurvivorId != clips[0].id && IsValidUlid(rightSurvivorId) &&
                  clips[2].source_in == RationalTime{80, 10} &&
                  clips[2].timeline_in == RationalTime{70, 10} &&
                  clips[2].duration == RationalTime{10, 10},
              "right survivor gets an exact stable identity and source range");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "spanning overwrite undo restores exact bytes");
        Check(log.Redo(document, error, message),
              "spanning overwrite redo succeeds");
        Check(document.sequence.tracks[1].clips[2].id == rightSurvivorId,
              "redo retains the right survivor ULID");
    });

    Test("split clip is atomic, stable and exactly reversible", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(
                  document,
                  SplitClipOperation{
                      document.sequence.tracks[0].clips[0].id, {20, 10}, {}},
                  error, message),
              "split applies: " + message);
        const Ulid rightId =
            std::get<SplitClipOperation>(log.AppliedEntries().back().op)
                .right_clip_id;
        Check(IsValidUlid(rightId), "split generates a stable right ULID");
        Check(document.sequence.tracks[0].clips.size() == 3,
              "split adds one clip without ripple");
        const DocumentClip& left = document.sequence.tracks[0].clips[0];
        const DocumentClip& right = document.sequence.tracks[0].clips[1];
        Check(left.duration == RationalTime{10, 10} && right.id == rightId &&
                  right.source_in == RationalTime{20, 10} &&
                  right.duration == RationalTime{10, 10} &&
                  right.timeline_in == RationalTime{20, 10},
              "split preserves exact source and timeline continuity");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  SerializeOperation(parsed) == operationJson,
              "SplitClip JSON round-trips canonically");
        Check(log.Undo(document, error, message), "split undo succeeds");
        Check(document.SaveToString() == initial,
              "split undo restores document bytes");
        Check(log.Redo(document, error, message), "split redo succeeds");
        Check(document.sequence.tracks[0].clips[1].id == rightId,
              "split redo retains the generated right ULID");
    });

    Test("linked cut creates independently selectable A/V segments", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(document,
                        DetachAudioOperation{videoId,
                                             document.sequence.tracks[1].id,
                                             "01KT0000000000000000000006",
                                             {}},
                        error, message),
              "linked cut fixture detaches audio: " + message);
        Check(log.Apply(document,
                        SetClipEffectsOperation{
                            videoId,
                            {{{}, "color.exposure", {{"amount", {35, 100}}}}}},
                        error, message),
              "linked cut fixture grades video: " + message);
        const Ulid audioId = document.sequence.tracks[1].clips[0].id;
        const std::string beforeCut = document.SaveToString();
        Operation cut = TimelineCutOperationForClip(
            document, *document.FindClip(videoId), {20, 10}, true);
        Check(std::holds_alternative<SplitLinkedClipsOperation>(cut),
              "linked selection builds one linked blade operation");
        Check(log.Apply(document, std::move(cut), error, message),
              "linked cut applies atomically: " + message);
        const auto& applied =
            std::get<SplitLinkedClipsOperation>(log.AppliedEntries().back().op);
        Check(applied.right_clip_ids.size() == 2,
              "linked cut creates one stable right ID per A/V member");
        const Ulid rightVideoId = applied.right_clip_ids[0];
        const Ulid rightAudioId = applied.right_clip_ids[1];
        const DocumentClip* leftVideo = document.FindClip(videoId);
        const DocumentClip* rightVideo = document.FindClip(rightVideoId);
        Check(leftVideo && rightVideo && leftVideo->effects.size() == 1 &&
                  rightVideo->effects.size() == 1 &&
                  rightVideo->effects[0].type == "color.exposure" &&
                  rightVideo->effects[0].params.at("amount").num == 35 &&
                  rightVideo->effects[0].params.at("amount").den == 100 &&
                  rightVideo->effects[0].id != leftVideo->effects[0].id,
              "linked cut preserves grading with an independent effect ID");
        const std::vector<Ulid> leftSelection =
            ExpandLinkedClipSelection(document, {videoId});
        const std::vector<Ulid> rightSelection =
            ExpandLinkedClipSelection(document, {rightVideoId});
        Check(leftSelection.size() == 2 &&
                  std::find(leftSelection.begin(), leftSelection.end(),
                            audioId) != leftSelection.end() &&
                  std::find(leftSelection.begin(), leftSelection.end(),
                            rightVideoId) == leftSelection.end(),
              "left selection contains only its linked image and sound");
        Check(rightSelection.size() == 2 &&
                  std::find(rightSelection.begin(), rightSelection.end(),
                            rightAudioId) != rightSelection.end() &&
                  std::find(rightSelection.begin(), rightSelection.end(),
                            videoId) == rightSelection.end(),
              "right selection contains only its linked image and sound");
        Check(document.FindClip(videoId)->link_group_id !=
                  document.FindClip(rightVideoId)->link_group_id,
              "the two sides receive distinct link groups");
        const std::string json = SerializeOperation(applied);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SplitLinkedClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == beforeCut,
              "one undo restores the complete linked cut");
        Check(log.Redo(document, error, message) &&
                  document.FindClip(rightVideoId) &&
                  document.FindClip(rightAudioId),
              "linked cut redo retains both generated identities");
    });

    Test("split at a clip boundary is rejected atomically", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(
                  document,
                  SplitClipOperation{
                      document.sequence.tracks[0].clips[0].id, {10, 10}, {}},
                  error, message) &&
                  error == EditError::InvalidOperation,
              "cut on head is rejected");
        Check(message.find("strictly within (10/10, 30/10)") !=
                      std::string::npos &&
                  message.find("got 10/10") != std::string::npos,
              "cut refusal reports the actual bounds and requested position: " +
                  message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == initial,
              "rejected cut leaves bytes and event log unchanged");
    });

    Test("linked split reports the member bounds before it reaches the log",
         [] {
             Document document = Fixture();
             document.sequence.tracks.push_back(
                 {"01KT0000000000000000000005", "audio", 1, {}});
             EditLog setupLog;
             EditError error = EditError::None;
             std::string message;
             const Ulid videoId = document.sequence.tracks[0].clips[0].id;
             Check(setupLog.Apply(
                       document,
                       DetachAudioOperation{videoId,
                                            document.sequence.tracks[1].id,
                                            "01KT0000000000000000000006",
                                            {}},
                       error, message),
                   "linked split bounds fixture detaches audio: " + message);
             const Ulid audioId = document.sequence.tracks[1].clips[0].id;
             const Ulid groupId = document.FindClip(videoId)->link_group_id;
             const std::string before = document.SaveToString();
             EditLog log;
             const SplitLinkedClipsOperation operation{
                 groupId, {videoId, audioId}, {10, 10}, {}, {}, {}, {}};
             Check(!log.Apply(document, operation, error, message) &&
                       error == EditError::InvalidOperation,
                   "linked cut on its head is InvalidOperation: " + message);
             Check(message.find("strictly within (10/10, 30/10)") !=
                           std::string::npos &&
                       message.find("got 10/10") != std::string::npos,
                   "linked cut refusal reports actual bounds and request: " +
                       message);
             Check(log.AppliedCount() == 0 && document.SaveToString() == before,
                   "rejected linked cut is never journaled");
         });

    Test("trim drag is clamped before duration can cross zero", [] {
        Document document = Fixture();
        const std::string initial = document.SaveToString();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(150.0, trackY, 700.0, 10);
        interaction.PointerDrag(400.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{1, 10},
              "preview stops at one timebase tick of duration");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message),
              "release applies the constrained trim");
        Check(log.AppliedCount() == 1 &&
                  document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{1, 10},
              "constrained trim emits one valid event");
        Check(log.Undo(document, error, message), "constrained trim undoes");
        Check(document.SaveToString() == initial,
              "undo restores bytes after constrained trim");
    });

    Test("magnetism snaps a trim to the neighboring cut", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(243.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{30, 10},
              "tail snaps exactly to the next clip start");
        Check(interaction.SnapGuideTime() &&
                  *interaction.SnapGuideTime() == RationalTime{40, 10},
              "snap exposes an exact guide time for rendering");
        EditError error = EditError::None;
        std::string message;
        Check(interaction.PointerUp(error, message), "snapped trim applies");
        Check(document.sequence.tracks[0].clips[0].duration ==
                  RationalTime{30, 10},
              "document receives the snapped duration");
    });

    Test("playhead edit navigation and temporary snapping are exact", [] {
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        const std::vector<RationalTime> points = TimelineEditPoints(document);
        Check(points == std::vector<RationalTime>(
                            {{0, 10}, {10, 10}, {30, 10}, {40, 10}, {50, 10}}),
              "edit points are sorted and deduplicated across boundaries");
        Check(AdjacentTimelineEdit(document, {35, 10}, false) ==
                      RationalTime{30, 10} &&
                  AdjacentTimelineEdit(document, {35, 10}, true) ==
                      RationalTime{40, 10},
              "up/down navigation chooses the strict previous/next cut");
        Check(SnapTimelineTimeToEdit(document, viewport, {393, 100}) ==
                  RationalTime{40, 10},
              "command scrub snaps the playhead inside the pixel threshold");

        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetSnappingEnabled(false);
        interaction.SetPlayheadSnapTarget(RationalTime{35, 10});
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(223.0, trackY, 700.0);
        Check(interaction.TrimPreview() && interaction.SnapGuideTime() &&
                  *interaction.SnapGuideTime() == RationalTime{35, 10},
              "holding Command snaps an edit to the playhead even when global "
              "magnetism is off");

        // Out of lot -- the author, 29 October 2026: with the magnet on, the
        // playhead holds a trimmed edge without Command, as in Resolve.
        EditLog magnetLog;
        TimelineInteraction magnet(document, magnetLog, viewport);
        magnet.SetPlayhead(RationalTime{35, 10});
        magnet.PointerDown(200.0, trackY, 700.0, 10);
        magnet.PointerDrag(223.0, trackY, 700.0);
        Check(magnet.TrimPreview() && magnet.SnapGuideTime() &&
                  *magnet.SnapGuideTime() == RationalTime{35, 10},
              "with the magnet on, a trim snaps to the playhead without "
              "Command");
        magnet.CancelDrag();
        EditLog offLog;
        TimelineInteraction off(document, offLog, viewport);
        off.SetSnappingEnabled(false);
        off.SetPlayhead(RationalTime{35, 10});
        off.PointerDown(200.0, trackY, 700.0, 10);
        off.PointerDrag(223.0, trackY, 700.0);
        Check(off.TrimPreview() && !off.SnapGuideTime(),
              "with the magnet off and no Command, the playhead does not "
              "hold the edge");
        off.CancelDrag();
    });

    // Out of lot -- the author, 29 October 2026: Alt+Y, Resolve's Select All
    // Clips Forward.
    Test("everything after the playhead, on every unlocked track", [] {
        Document document = Fixture();
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const Ulid second = document.sequence.tracks[0].clips[1].id;
        Check(TimelineClipsAfter(document, {20, 10}) ==
                  std::vector<Ulid>({first, second}),
              "the clip under the playhead and every clip after it");
        Check(TimelineClipsAfter(document, {30, 10}) ==
                  std::vector<Ulid>({second}),
              "a clip that ends at the playhead is not after it");
        Check(TimelineClipsAfter(document, {50, 10}).empty(),
              "nothing after the last clip");
        document.sequence.tracks[0].locked = true;
        Check(TimelineClipsAfter(document, {0, 10}).empty(),
              "a locked track gives nothing to select");
    });

    // Out of lot -- the author, 29 October 2026: the trim's frame counter.
    Test("a held trim reads how many frames its edge moved", [] {
        Check(TrimReadoutText(12, {25, 1}) == "+12" &&
                  TrimReadoutText(-7, {25, 1}) == "-7" &&
                  TrimReadoutText(25, {25, 1}) == "+1:00" &&
                  TrimReadoutText(-30, {25, 1}) == "-1:05" &&
                  TrimReadoutText(0, {24000, 1001}) == "+0",
              "frames under a second, seconds:frames from one on");
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        Check(!TimelineTrimReadoutFor(document, interaction.TrimPreview(),
                                      {10, 1}),
              "no counter without a trim");
        interaction.PointerDown(150.0, trackY, 700.0, 10);
        interaction.PointerDrag(200.0, trackY, 700.0);
        const auto head = TimelineTrimReadoutFor(
            document, interaction.TrimPreview(), {10, 1});
        Check(head && head->frames == 5 && head->text == "+5" &&
                  head->edge == RationalTime{15, 10},
              "a head trimmed in by half a second reads +5 at 10 fps, "
              "beside its new head");
        interaction.CancelDrag();
        interaction.PointerDown(350.0, trackY, 700.0, 10);
        interaction.PointerDrag(330.0, trackY, 700.0);
        const auto tail = TimelineTrimReadoutFor(
            document, interaction.TrimPreview(), {10, 1});
        Check(tail && tail->frames == -2 && tail->text == "-2" &&
                  tail->edge == RationalTime{28, 10},
              "a tail pulled in by 0.2 s reads -2, beside its new tail");
        interaction.CancelDrag();
    });

    Test("markers are drawn and hit from one geometry", [] {
        // D2 -- docs/roadmap/DIALOGUE-2026-10.md.
        Document document = Fixture();
        const DocumentMarker first{
            "01KT0000000000000000000010", "euh", {20, 10}, "#f5c542", "note"};
        const DocumentMarker second{"01KT0000000000000000000011",
                                    "fin de mot",
                                    {40, 10},
                                    "#f5c542",
                                    "note"};
        const DocumentMarker beyond{"01KT0000000000000000000012",
                                    "plus loin",
                                    {900, 10},
                                    "#f5c542",
                                    "note"};
        document.sequence.markers = {first, second, beyond};
        const TimelineViewport viewport = Viewport();
        const auto flags = VisibleTimelineMarkers(document, viewport, 700.0);
        Check(flags.size() == 2 && flags[0].marker_id == first.id &&
                  flags[1].marker_id == second.id,
              "only the markers in the visible span have a flag");
        Check(flags[0].x == 250.0 && flags[0].flag_x == 246.0 &&
                  flags[0].flag_y + kTimelineMarkerFlagHeight ==
                      kTimelineToolbarHeight + kTimelineScaleHeight,
              "a flag is centred on its exact time and sits on the ruler's "
              "baseline");
        // Visual QA of 1 October 2026 -- a tag pointing at its time, not a
        // square with a line under it.
        const auto shape = TimelineMarkerFlagShape(flags[0]);
        const double baseline = kTimelineToolbarHeight + kTimelineScaleHeight;
        bool tapers = shape.size() == 5 && shape[0].x == 246.0 &&
                      shape[0].y == flags[0].flag_y && shape[0].width == 9.0 &&
                      shape[0].y + shape[0].height == shape[1].y;
        for (size_t row = 1; tapers && row < shape.size(); ++row)
            tapers =
                shape[row].height == 1.0 &&
                shape[row].width == shape[row - 1].width - 2.0 &&
                shape[row].x + shape[row].width / 2.0 == flags[0].x + 0.5 &&
                shape[row].y == shape[row - 1].y + shape[row - 1].height;
        Check(tapers, "the tag narrows by a point each side on every row");
        Check(tapers && shape.back().x == flags[0].x &&
                  shape.back().width == 1.0 &&
                  shape.back().y + shape.back().height == baseline,
              "and ends on a single point, on the marker's time, at the "
              "ruler's baseline");
        const double flagY = flags[0].flag_y + 4.0;
        Check(HitTestTimelineMarker(document, viewport, 250.0, flagY, 700.0) ==
                  std::optional<Ulid>(first.id),
              "the pointer on a flag names its marker");
        Check(HitTestTimelineMarker(document, viewport,
                                    246.0 - kTimelineMarkerHitSlop, flagY,
                                    700.0) == std::optional<Ulid>(first.id) &&
                  !HitTestTimelineMarker(document, viewport,
                                         245.0 - kTimelineMarkerHitSlop, flagY,
                                         700.0),
              "the hit area is the flag and its slop, no more");
        Check(!HitTestTimelineMarker(document, viewport, 250.0,
                                     kTimelineRulerHeight + 20.0, 700.0),
              "a click on a track under a marker is not a click on it");

        Document crowded = document;
        crowded.sequence.markers[1].time = {203, 100};
        Check(HitTestTimelineMarker(crowded, viewport, 253.0, flagY, 700.0) ==
                      std::optional<Ulid>(second.id) &&
                  HitTestTimelineMarker(crowded, viewport, 249.0, flagY,
                                        700.0) == std::optional<Ulid>(first.id),
              "between overlapping flags the nearest stem wins");

        TimelineViewport scrolled = viewport;
        scrolled.view_start = {30, 10};
        const auto later = VisibleTimelineMarkers(document, scrolled, 700.0);
        Check(later.size() == 1 && later[0].marker_id == second.id,
              "a marker scrolled under the track headers has no flag");
    });

    Test("a marker with a duration is a span on the ruler", [] {
        // D6 -- docs/roadmap/DIALOGUE-2026-10.md.
        Document document = Fixture();
        const DocumentMarker span{"01KT0000000000000000000010",
                                  "ce passage",
                                  {20, 10},
                                  "#4da3ff",
                                  "note",
                                  {15, 10}};
        const DocumentMarker inside{
            "01KT0000000000000000000011", "euh", {28, 10}, "#4da3ff", "note"};
        document.sequence.markers = {span, inside};
        const TimelineViewport viewport = Viewport();
        const auto flags = VisibleTimelineMarkers(document, viewport, 700.0);
        Check(flags.size() == 2 && flags[0].x == 250.0 &&
                  flags[0].end_x == 400.0 && flags[1].end_x == flags[1].x,
              "a span runs from its time to its end; a point ends where it "
              "starts");
        const auto shape = TimelineMarkerFlagShape(flags[0]);
        Check(shape.size() == 5 && shape[0].x == 246.0 &&
                  shape[0].x + shape[0].width == 400.0 &&
                  shape.back().x == 250.0 && shape.back().width == 1.0,
              "its body runs to the end, its point stays on the start");
        const double flagY = flags[0].flag_y + 3.0;
        Check(HitTestTimelineMarker(document, viewport, 300.0, flagY, 700.0) ==
                      std::optional<Ulid>(span.id) &&
                  HitTestTimelineMarker(document, viewport, 399.0, flagY,
                                        700.0) == std::optional<Ulid>(span.id),
              "anywhere along the span is on it");
        Check(HitTestTimelineMarker(document, viewport, 331.0, flagY, 700.0) ==
                  std::optional<Ulid>(inside.id),
              "a point marker inside a span is still picked, by its tag");
        Check(!HitTestTimelineMarker(document, viewport, 410.0, flagY, 700.0),
              "past its end and the slop, nothing is hit");

        TimelineViewport scrolled = viewport;
        scrolled.view_start = {30, 10};
        const auto partly = VisibleTimelineMarkers(document, scrolled, 700.0);
        Check(partly.size() == 1 && partly[0].marker_id == span.id &&
                  partly[0].x < scrolled.header_width &&
                  partly[0].end_x == 100.0,
              "a span whose start scrolled out is still there by its end");
        Check(!HitTestTimelineMarker(document, scrolled, 40.0, flagY, 700.0) &&
                  HitTestTimelineMarker(document, scrolled, 80.0, flagY,
                                        700.0) == std::optional<Ulid>(span.id),
              "and is hit on what shows of it, not under the headers");

        const MediaRate rate{25, 1};
        const DocumentMarker* covering =
            TimelineMarkerCoveringPlayhead(document, {60, 25}, rate);
        Check(covering && covering->id == span.id &&
                  !TimelineMarkerAtPlayhead(document, {60, 25}, rate),
              "the playhead inside a span reads its note, without standing "
              "on a marker");
        const DocumentMarker* standing =
            TimelineMarkerCoveringPlayhead(document, {70, 25}, rate);
        Check(standing && standing->id == inside.id,
              "the marker the playhead stands on comes before the span "
              "around it");
        Check(!TimelineMarkerCoveringPlayhead(document, {100, 25}, rate),
              "past the span, no note");

        const TimelineMarkerPreview stretched{inside.id, {28, 10}, {30, 10}};
        const auto previewed =
            VisibleTimelineMarkers(document, viewport, 700.0, stretched);
        Check(previewed.size() == 2 && previewed[1].end_x == 630.0 &&
                  flags[1].end_x == 330.0,
              "a stretch in progress is drawn before the document has it");
    });

    Test("stretching a marker makes a span either side of it", [] {
        // D6 -- the gesture made with Option held.
        const DocumentMarker point{
            "01KT0000000000000000000010", "ici", {50, 25}, "#4da3ff", "note"};
        const UpdateMarkerOperation right =
            TimelineMarkerSpanUpdate(point, point.time, {125, 25});
        Check(right.marker_id == point.id && right.name == "ici" &&
                  right.color == "#4da3ff" && right.category == "note" &&
                  right.time == RationalTime{50, 25} &&
                  right.duration == RationalTime{75, 25},
              "pulled right: from the marker's time to the pointer");
        const UpdateMarkerOperation left =
            TimelineMarkerSpanUpdate(point, point.time, {20, 25});
        Check(left.time == RationalTime{20, 25} &&
                  left.duration == RationalTime{30, 25},
              "pulled left: from the pointer to the marker's time");
        const UpdateMarkerOperation back =
            TimelineMarkerSpanUpdate(point, point.time, {50, 25});
        Check(back.duration.value == 0 && back.duration.rate == 1,
              "back on the anchor: a point, spelled as the document omits it");

        DocumentMarker span = point;
        span.duration = {75, 25};
        const TimelineViewport viewport = Viewport();
        // The span runs from x = 250 to x = 550.
        Check(TimelineMarkerSpanAnchor(point, viewport, 300.0) == point.time,
              "a point is stretched from its time");
        Check(TimelineMarkerSpanAnchor(span, viewport, 540.0) ==
                      RationalTime{50, 25} &&
                  TimelineMarkerSpanAnchor(span, viewport, 260.0) ==
                      RationalTime{125, 25},
              "a span is stretched by the end taken hold of; the other "
              "stays");
    });

    Test("the playhead finds markers that sit between frames", [] {
        // An agent puts a marker on a word boundary, not on a frame.
        Document document = Fixture();
        const MediaRate rate{25, 1};
        const DocumentMarker onFrame{"01KT0000000000000000000010",
                                     "sur une image",
                                     {50, 25},
                                     "#f5c542",
                                     "note"};
        // 144500 / 48000 s is frame 75.26: the playhead sent there shows 75.
        const DocumentMarker offFrame{"01KT0000000000000000000011",
                                      "entre deux images",
                                      {144500, 48000},
                                      "#f5c542",
                                      "note"};
        document.sequence.markers = {offFrame, onFrame};
        const DocumentMarker* at =
            TimelineMarkerAtPlayhead(document, {50, 25}, rate);
        Check(at && at->id == onFrame.id,
              "the playhead on a marker's frame is on that marker");
        Check(!TimelineMarkerAtPlayhead(document, {49, 25}, rate) &&
                  !TimelineMarkerAtPlayhead(document, {51, 25}, rate),
              "the frames around a marker are not on it");
        const DocumentMarker* between =
            TimelineMarkerAtPlayhead(document, {75, 25}, rate);
        Check(between && between->id == offFrame.id &&
                  !TimelineMarkerAtPlayhead(document, {76, 25}, rate),
              "a marker between two frames is under the playhead on the "
              "frame it is sent to");
        Check(
            AdjacentTimelineMarker(document, {0, 25}, rate, true) ==
                    std::optional<RationalTime>(RationalTime{50, 25}) &&
                AdjacentTimelineMarker(document, {50, 25}, rate, true) ==
                    std::optional<RationalTime>(RationalTime{144500, 48000}) &&
                !AdjacentTimelineMarker(document, {75, 25}, rate, true),
            "next goes to the nearest marker strictly after the frame "
            "shown, and stops at the last one");
        Check(AdjacentTimelineMarker(document, {75, 25}, rate, false) ==
                      std::optional<RationalTime>(RationalTime{50, 25}) &&
                  !AdjacentTimelineMarker(document, {50, 25}, rate, false),
              "previous goes to the nearest marker strictly before, and "
              "stops at the first one");

        // 96500 / 48000 s is frame 50.26: the same picture as onFrame.
        Document shared = document;
        shared.sequence.markers.push_back({"01KT0000000000000000000012",
                                           "même image",
                                           {96500, 48000},
                                           "#f5c542",
                                           "note"});
        const DocumentMarker* firstOfTwo =
            TimelineMarkerAtPlayhead(shared, {50, 25}, rate);
        Check(firstOfTwo && firstOfTwo->id == onFrame.id &&
                  AdjacentTimelineMarker(shared, {50, 25}, rate, true) ==
                      std::optional<RationalTime>(RationalTime{144500, 48000}),
              "two markers on one picture: the first in the document "
              "answers, and next does not stay on that picture");
    });

    Test("a typed note always makes a name the document accepts", [] {
        Check(TimelineMarkerNameFromNote("") == "Marqueur" &&
                  TimelineMarkerNameFromNote("  \t ") == "Marqueur",
              "a marker dropped without a word has the default name");
        Check(TimelineMarkerNameFromNote("  euh ici \n") == "euh ici" &&
                  TimelineMarkerNameFromNote("fin\nde mot") == "fin de mot",
              "spaces around a note go, a pasted line break becomes a space");
        Check(TimelineMarkerNameFromNote("coupé trop tôt") == "coupé trop tôt",
              "accents are kept");
        // 2047 two-byte letters and one more byte put the limit inside "é".
        std::string longNote = "a";
        for (int index = 0; index < 2100; ++index) longNote += "é";
        const std::string cut = TimelineMarkerNameFromNote(longNote);
        Check(cut.size() == 4095 &&
                  cut.compare(0, cut.size(), longNote, 0, cut.size()) == 0,
              "a note too long is cut before the letter that would not fit");
        Document document = Fixture();
        document.sequence.markers = {
            {"01KT0000000000000000000010", cut, {20, 10}, "#f5c542", "note"}};
        std::string error;
        Check(document.Validate(error), "and the document takes it: " + error);
    });

    Test("tail trim cannot pass the source media end", [] {
        Document document = Fixture();
        document.sequence.tracks[0].clips.erase(
            document.sequence.tracks[0].clips.begin() + 1);
        TimelineViewport viewport = Viewport();
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        const double trackY = kTimelineRulerHeight + 20.0;
        interaction.PointerDown(350.0, trackY, 2200.0, 10);
        interaction.PointerDrag(2000.0, trackY, 2200.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{90, 10},
              "preview stops exactly at source duration");
        Check(interaction.TrimPreview()->times.source_in.add(
                  interaction.TrimPreview()->times.duration) ==
                  document.sources[0].duration,
              "preview source range ends at the media boundary");
    });

    // PRD37 -- the QA of 3 October 2026: fitted while the window was laid
    // out (971 points), shown at 917.5, the end of a 2 s film out of view.
    Test("a fit nobody moved follows the view's width; a moved view does not",
         [] {
             TimelineViewport viewport;
             const RationalTime film{50, 25};
             TimelineFit fit;
             Check(TimelineFitFollowsResize(fit, viewport, 971.0),
                   "a timeline never fitted is fitted at its first width");
             Check(!TimelineFitFollowsResize(fit, viewport, 40.0),
                   "not within the track headers: the view is not laid out");
             fit = FitTimeline(viewport, film, 971.0);
             Check(fit.made &&
                       std::abs(viewport.pixels_per_second - 425.5) < 1e-9,
                   "the fit at 971 points is the 425.5 points a second the "
                   "QA measured");
             Check(TimelineFitFollowsResize(fit, viewport, 917.5),
                   "the view settling at 917.5 is fitted again");
             fit = FitTimeline(viewport, film, 917.5);
             Check(viewport.TimeToX(film) <= 917.5,
                   "and the end of the film is in view");
             Check(!TimelineFitFollowsResize(fit, viewport, 917.5),
                   "the same width asks for nothing");
             viewport.ZoomAroundX(400.0, 2.0, kTimelineViewRate);
             Check(!TimelineFitFollowsResize(fit, viewport, 1200.0),
                   "zoomed by the editor, a resize keeps the scale");
             viewport.pixels_per_second = fit.pixels_per_second;
             viewport.view_start = {12000, kTimelineViewRate};
             Check(!TimelineFitFollowsResize(fit, viewport, 1200.0),
                   "scrolled, too");
         });

    // PRD37 -- the QA saw a 9:16 timeline whose shots fit in eight seconds
    // open at 39 s, empty, in the view of the timeline left behind.
    Test(
        "each timeline opens on its own view, and on a fit the first time", [] {
            TimelineViewport viewport;
            viewport.pixels_per_second = 2000.0;
            viewport.view_start = {39, 1};
            const RationalTime eight{200, 25};
            TimelineFit fit = OpenTimelineView(viewport, nullptr, eight, 900.0);
            Check(fit.made && viewport.view_start == RationalTime{0, 25} &&
                      viewport.TimeToX(eight) <= 900.0,
                  "a timeline opened for the first time shows all of it");

            viewport.ZoomAroundX(300.0, 3.0, kTimelineViewRate);
            const TimelineViewMemory zoomed =
                RememberTimelineView(viewport, fit);
            TimelineViewport other = viewport;
            OpenTimelineView(other, nullptr, {2500, 25}, 900.0);
            fit = OpenTimelineView(other, &zoomed, eight, 900.0);
            Check(other.view_start == zoomed.view_start &&
                      other.pixels_per_second == zoomed.pixels_per_second &&
                      !TimelineFitFollowsResize(fit, other, 1100.0),
                  "coming back, it shows the view it was left with, and a "
                  "resize keeps it");

            TimelineViewport fitted;
            const TimelineFit first = FitTimeline(fitted, eight, 900.0);
            const TimelineViewMemory untouched =
                RememberTimelineView(fitted, first);
            TimelineViewport back;
            fit = OpenTimelineView(back, &untouched, eight, 1100.0);
            TimelineViewport expected;
            expected.FitDuration(eight, 1100.0);
            Check(fit.made &&
                      back.pixels_per_second == expected.pixels_per_second,
                  "an untouched fit left at another width is fitted to the "
                  "width the window has now");

            TimelineViewMemory pastTheEnd = zoomed;
            pastTheEnd.view_start = {10, 1};
            fit = OpenTimelineView(back, &pastTheEnd, eight, 900.0);
            Check(fit.made && back.view_start == RationalTime{0, 25},
                  "a remembered view an edit left past the end gives way "
                  "to a fit");
            TimelineViewport unlaid;
            Check(!OpenTimelineView(unlaid, nullptr, eight, 50.0).made,
                  "a view not laid out yet waits for its width to fit");
        });

    // D4 -- decided on 8 October 2026: the agent's question keeps its
    // colour, set for it, and keeps the colour to itself.
    Test("the agent's question marker keeps its colour", [] {
        std::string color;
        std::string error;
        Check(ResolveMarkerColor("question", "", color, error) &&
                  color == kTimelineMarkerAgentColor,
              "a question with no colour takes the agent's");
        Check(ResolveMarkerColor("question", "#FF8A3D", color, error) &&
                  color == kTimelineMarkerAgentColor,
              "its own colour, in any case, is accepted");
        Check(!ResolveMarkerColor("question", "#4da3ff", color, error) &&
                  error.find("keeps its colour") != std::string::npos,
              "a question in another colour is refused");
        Check(!ResolveMarkerColor("note", kTimelineMarkerAgentColor, color,
                                  error),
              "another category may not take the agent's colour");
        color = "#f5c542";
        Check(ResolveMarkerColor("note", "", color, error) &&
                  color == "#f5c542" &&
                  ResolveMarkerColor("note", "#4da3ff", color, error) &&
                  color == "#4da3ff",
              "other categories keep the default, or what they are given");
    });

    // FC1 -- docs/roadmap/FILMCRAFT-2026-10.md. At 50 points a second the
    // tail of V1's first clip sits at x = 200 (3 s). Dragged to x = 231 it
    // asks for 3.6 s; a target at 3.7 s is 5 points away, within the snap
    // distance, and V1's own next cut (4 s) is 20 points away.
    const auto dragFirstTail = [](TimelineInteraction& interaction,
                                  double trackY) {
        interaction.PointerDown(200.0, trackY, 700.0, 10);
        interaction.PointerDrag(231.0, trackY, 700.0);
    };

    Test("FC1: the magnet holds on a cut of another track", [&] {
        Document document = Fixture();
        document.sequence.tracks.push_back({"01KT0000000000000000000005",
                                            "video",
                                            1,
                                            {{"01KT0000000000000000000006",
                                              "01KT0000000000000000000001",
                                              {0, 10},
                                              {5, 10},
                                              {37, 10}}}});
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        // V2 is drawn above V1.
        dragFirstTail(interaction,
                      kTimelineRulerHeight + viewport.track_height + 20.0);
        Check(interaction.TrimPreview() && interaction.TrimPreview()->valid &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{27, 10} &&
                  interaction.SnapGuideTime() == RationalTime{37, 10},
              "a V1 trim snaps to the head of a V2 clip");
    });

    Test("FC1: the magnet holds on a marker, and on a span's end", [&] {
        for (const bool span : {false, true}) {
            Document document = Fixture();
            DocumentMarker marker{
                "01KT0000000000000000000010", "ici",
                span ? RationalTime{5, 10} : RationalTime{37, 10}, "#f5c542",
                "note"};
            if (span) marker.duration = {32, 10};
            document.sequence.markers.push_back(marker);
            TimelineViewport viewport = Viewport();
            viewport.pixels_per_second = 50.0;
            EditLog log;
            TimelineInteraction interaction(document, log, viewport);
            dragFirstTail(interaction, kTimelineRulerHeight + 20.0);
            Check(interaction.TrimPreview() &&
                      interaction.TrimPreview()->times.duration ==
                          RationalTime{27, 10} &&
                      interaction.SnapGuideTime() == RationalTime{37, 10},
                  span ? "a trim snaps to where a marked passage ends"
                       : "a trim snaps to a marker");
        }
    });

    Test("FC1: the magnet holds on the In and the Out points", [&] {
        for (const bool in : {true, false}) {
            Document document = Fixture();
            TimelineViewport viewport = Viewport();
            viewport.pixels_per_second = 50.0;
            EditLog log;
            TimelineInteraction interaction(document, log, viewport);
            const std::optional<RationalTime> point = RationalTime{37, 10};
            interaction.SetInOutSnapTargets(in ? point : std::nullopt,
                                            in ? std::nullopt : point);
            dragFirstTail(interaction, kTimelineRulerHeight + 20.0);
            Check(interaction.TrimPreview() &&
                      interaction.TrimPreview()->times.duration ==
                          RationalTime{27, 10} &&
                      interaction.SnapGuideTime() == RationalTime{37, 10},
                  in ? "a trim snaps to the In point"
                     : "a trim snaps to the Out point");
        }
        Document document = Fixture();
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        EditLog log;
        TimelineInteraction interaction(document, log, viewport);
        interaction.SetInOutSnapTargets(RationalTime{37, 10},
                                        RationalTime{37, 10});
        interaction.SetInOutSnapTargets(std::nullopt, std::nullopt);
        dragFirstTail(interaction, kTimelineRulerHeight + 20.0);
        Check(interaction.TrimPreview() &&
                  interaction.TrimPreview()->times.duration ==
                      RationalTime{26, 10} &&
                  !interaction.SnapGuideTime(),
              "cleared In and Out points no longer attract");
    });

    Test("FC1: what the gesture carries along is never a target", [] {
        Document document = Fixture();
        document.sequence.tracks.push_back(
            {"01KT0000000000000000000005", "audio", 1, {}});
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        Check(log.Apply(
                  document,
                  Operation{DetachAudioOperation{videoId,
                                                 document.sequence.tracks[1].id,
                                                 "01KT0000000000000000000006",
                                                 {}}},
                  error, message),
              "linked fixture detach applies: " + message);
        TimelineViewport viewport = Viewport();
        viewport.pixels_per_second = 50.0;
        const double y = kTimelineRulerHeight + 20.0;
        // Dragged 5 points past its tail: the linked audio's tail, on A1,
        // is where the trim started.
        TimelineInteraction linked(document, log, viewport);
        linked.PointerDown(200.0, y, 700.0, 10);
        linked.PointerDrag(205.0, y, 700.0);
        Check(
            linked.TrimPreview() && linked.TrimPreview()->valid &&
                linked.TrimPreview()->times.duration == RationalTime{21, 10} &&
                !linked.SnapGuideTime(),
            "a linked trim is not pulled back by the partner it trims");
        TimelineInteraction alone(document, log, viewport);
        alone.SetLinkedSelectionEnabled(false);
        alone.PointerDown(200.0, y, 700.0, 10);
        alone.PointerDrag(205.0, y, 700.0);
        Check(alone.TrimPreview() &&
                  alone.TrimPreview()->times.duration == RationalTime{20, 10} &&
                  alone.SnapGuideTime() == RationalTime{30, 10},
              "trimmed alone, the video snaps back level with its audio");
    });

    Test("FC1: at equal distance the earlier target keeps priority", [] {
        // Quarters and eighths of a second: every distance is exact. The
        // tail of V1's clip (3 s, x = 200) is dragged to 3.5 s (x = 225);
        // 3.375 s and 3.625 s are both 6.25 points away.
        const auto tie = [](bool marker, std::optional<RationalTime> playhead,
                            std::optional<RationalTime> in) {
            Document document;
            document.sources = {
                {"01KT0000000000000000000001",
                 "source.mov",
                 {10, 1},
                 {100, 10}},
            };
            document.sequence.tracks = {
                {"01KT0000000000000000000002",
                 "video",
                 0,
                 {{"01KT0000000000000000000003",
                   "01KT0000000000000000000001",
                   {0, 4},
                   {8, 4},
                   {4, 4}}}},
                {"01KT0000000000000000000005",
                 "video",
                 1,
                 {{"01KT0000000000000000000006",
                   "01KT0000000000000000000001",
                   {0, 4},
                   {4, 4},
                   {29, 8}}}},
            };
            if (marker)
                document.sequence.markers.push_back(
                    {"01KT0000000000000000000010",
                     "ici",
                     {27, 8},
                     "#f5c542",
                     "note"});
            if (in) document.sequence.tracks[1].clips.clear();
            TimelineViewport viewport = Viewport();
            viewport.pixels_per_second = 50.0;
            EditLog log;
            TimelineInteraction interaction(document, log, viewport);
            interaction.SetPlayheadSnapTarget(playhead);
            interaction.SetInOutSnapTargets(in, std::nullopt);
            const double y =
                kTimelineRulerHeight + viewport.track_height + 20.0;
            interaction.PointerDown(200.0, y, 700.0, 4);
            interaction.PointerDrag(225.0, y, 700.0);
            return interaction.SnapGuideTime();
        };
        Check(tie(true, std::nullopt, std::nullopt) == RationalTime{29, 8},
              "a clip's edge on another track wins over a marker");
        Check(tie(false, RationalTime{27, 8}, std::nullopt) ==
                  RationalTime{27, 8},
              "the playhead wins over a clip's edge on another track");
        Check(
            tie(true, std::nullopt, RationalTime{29, 8}) == RationalTime{27, 8},
            "a marker wins over the In point");
    });

    // FC2 -- docs/roadmap/FILMCRAFT-2026-10.md. The fixture at 100 points a
    // second, behind 50 points of headers: V1 holds [1 s, 3 s) at x 150 to
    // 350 and [4 s, 5 s) at x 450 to 550, in the row from y 59 to 99. The
    // timeline is 700 by 300 points: the zoom strip starts at y 286.
    Test("FC2: one table says what is under the pointer", [] {
        using Kind = TimelinePointerTargetKind;
        using Tool = TimelinePointerTool;
        Document document = Fixture();
        document.sequence.markers.push_back(
            {"01KT0000000000000000000010", "ici", {60, 10}, "#f5c542", "note"});
        const TimelineViewport viewport = Viewport();
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const Ulid track = document.sequence.tracks[0].id;
        const double row = kTimelineRulerHeight + 20.0;
        const double flagY =
            kTimelineToolbarHeight + kTimelineScaleHeight - 5.0;
        constexpr TimelinePointerModifiers none{};
        constexpr TimelinePointerModifiers shift{true, false, false};
        constexpr TimelinePointerModifiers option{false, true, false};
        constexpr TimelinePointerModifiers command{false, false, true};
        constexpr TimelinePointerModifiers commandShift{true, false, true};
        struct Case {
            const char* name;
            Tool tool;
            TimelinePointerModifiers modifiers;
            double x;
            double y;
            Kind kind;
            TimelineHitEdge edge;
            TimelineTrimMode mode;
            bool duplicate;
            bool toggle;
        };
        const std::vector<Case> cases = {
            {"select, body", Tool::Select, none, 250.0, row, Kind::ClipBody,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"select, head", Tool::Select, none, 152.0, row, Kind::ClipEdge,
             TimelineHitEdge::Head, TimelineTrimMode::Normal, false, false},
            {"select, tail", Tool::Select, none, 348.0, row, Kind::ClipEdge,
             TimelineHitEdge::Tail, TimelineTrimMode::Normal, false, false},
            {"select, at the edge zone's limit", Tool::Select, none, 156.0, row,
             Kind::ClipEdge, TimelineHitEdge::Head, TimelineTrimMode::Normal,
             false, false},
            {"select, just past the edge zone", Tool::Select, none, 157.0, row,
             Kind::ClipBody, TimelineHitEdge::None, TimelineTrimMode::Normal,
             false, false},
            {"shift, tail ripples", Tool::Select, shift, 348.0, row,
             Kind::ClipEdge, TimelineHitEdge::Tail, TimelineTrimMode::Ripple,
             false, false},
            {"shift, body toggles", Tool::Select, shift, 250.0, row,
             Kind::ClipBody, TimelineHitEdge::None, TimelineTrimMode::Ripple,
             false, true},
            {"command, tail rolls", Tool::Select, command, 348.0, row,
             Kind::ClipEdge, TimelineHitEdge::Tail, TimelineTrimMode::Roll,
             false, false},
            {"command and shift, Command wins", Tool::Select, commandShift,
             348.0, row, Kind::ClipEdge, TimelineHitEdge::Tail,
             TimelineTrimMode::Roll, false, false},
            {"option, body duplicates", Tool::Select, option, 250.0, row,
             Kind::ClipBody, TimelineHitEdge::None, TimelineTrimMode::Normal,
             true, false},
            {"option, tail trims", Tool::Select, option, 348.0, row,
             Kind::ClipEdge, TimelineHitEdge::Tail, TimelineTrimMode::Normal,
             false, false},
            // FC4 -- the Trim tool: edges ripple, Command rolls, the body
            // slips, Shift neither toggles nor ripples a body.
            {"trim, tail ripples", Tool::Trim, none, 348.0, row, Kind::ClipEdge,
             TimelineHitEdge::Tail, TimelineTrimMode::Ripple, false, false},
            {"trim, head ripples", Tool::Trim, none, 152.0, row, Kind::ClipEdge,
             TimelineHitEdge::Head, TimelineTrimMode::Ripple, false, false},
            {"trim, command on the tail rolls", Tool::Trim, command, 348.0, row,
             Kind::ClipEdge, TimelineHitEdge::Tail, TimelineTrimMode::Roll,
             false, false},
            {"trim, body slips", Tool::Trim, none, 250.0, row, Kind::ClipBody,
             TimelineHitEdge::None, TimelineTrimMode::Slip, false, false},
            {"trim, shift does not toggle", Tool::Trim, shift, 250.0, row,
             Kind::ClipBody, TimelineHitEdge::None, TimelineTrimMode::Slip,
             false, false},
            {"trim, option on the body slips", Tool::Trim, option, 250.0, row,
             Kind::ClipBody, TimelineHitEdge::None, TimelineTrimMode::Slip,
             false, false},
            {"trim, gap", Tool::Trim, none, 400.0, row, Kind::None,
             TimelineHitEdge::None, TimelineTrimMode::Ripple, false, false},
            {"blade, tail is the body", Tool::Cut, none, 348.0, row,
             Kind::ClipBody, TimelineHitEdge::None, TimelineTrimMode::Normal,
             false, false},
            {"hand, nothing", Tool::Hand, none, 250.0, row, Kind::None,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"zoom, nothing", Tool::Zoom, command, 250.0, row, Kind::None,
             TimelineHitEdge::None, TimelineTrimMode::Roll, false, false},
            {"select, gap", Tool::Select, none, 400.0, row, Kind::None,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"select, ruler", Tool::Select, none, 250.0, 30.0, Kind::None,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"select, under the tracks", Tool::Select, none, 250.0, 200.0,
             Kind::None, TimelineHitEdge::None, TimelineTrimMode::Normal, false,
             false},
            {"select, headers", Tool::Select, none, 40.0, row, Kind::None,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"select, marker", Tool::Select, none, 650.0, flagY, Kind::Marker,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"hand, marker", Tool::Hand, none, 650.0, flagY, Kind::Marker,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"blade, zoom strip", Tool::Cut, none, 250.0, 290.0, Kind::ZoomBar,
             TimelineHitEdge::None, TimelineTrimMode::Normal, false, false},
            {"select, past the bottom", Tool::Select, none, 250.0, 301.0,
             Kind::None, TimelineHitEdge::None, TimelineTrimMode::Normal, false,
             false},
        };
        for (const Case& c : cases) {
            const TimelinePointerTarget target =
                TimelineHitAt(document, viewport, c.x, c.y, 700.0, 300.0,
                              {50, 10}, c.tool, c.modifiers);
            const bool onClip =
                c.kind == Kind::ClipBody || c.kind == Kind::ClipEdge;
            Check(target.kind == c.kind && target.edge == c.edge &&
                      target.trim_mode == c.mode &&
                      target.duplicate == c.duplicate &&
                      target.selection_toggle == c.toggle &&
                      target.clip_id == (onClip ? first : Ulid{}),
                  std::string("hit table: ") + c.name);
        }
        const TimelinePointerTarget gap =
            TimelineHitAt(document, viewport, 400.0, row, 700.0, 300.0,
                          {50, 10}, Tool::Select, none);
        Check(gap.track_id == track,
              "an empty stretch of a row names its track (gap, lasso)");
        const TimelinePointerTarget marker =
            TimelineHitAt(document, viewport, 650.0, flagY, 700.0, 300.0,
                          {50, 10}, Tool::Select, option);
        Check(marker.marker_id == document.sequence.markers[0].id &&
                  marker.marker_stretch,
              "Option on a marker takes it to be stretched");
        const TimelineZoomBarGeometry strip =
            CalculateTimelineZoomBarGeometry(viewport, {50, 10}, 700.0);
        const double thumbX = strip.thumb_x + strip.thumb_width / 2.0;
        const TimelinePointerTarget thumb =
            TimelineHitAt(document, viewport, thumbX, 290.0, 700.0, 300.0,
                          {50, 10}, Tool::Select, none);
        Check(thumb.kind == Kind::ZoomBar &&
                  thumb.zoom_bar_part == strip.HitTest(thumbX) &&
                  thumb.zoom_bar_part != TimelineZoomBarPart::None,
              "the zoom strip answers with its own geometry");
        document.sequence.tracks[0].locked = true;
        Check(TimelineHitAt(document, viewport, 250.0, row, 700.0, 300.0,
                            {50, 10}, Tool::Select, none)
                  .track_locked,
              "a clip on a locked track says so");
    });

    Test("FC2: the table and the press take hold of the same thing", [] {
        using Tool = TimelinePointerTool;
        using Kind = TimelinePointerTargetKind;
        const double row = kTimelineRulerHeight + 20.0;
        for (const Tool tool : {Tool::Select, Tool::Trim}) {
            for (const double x : {152.0, 250.0, 348.0, 400.0}) {
                for (const bool shift : {false, true}) {
                    for (const int keys : {0, 1, 2, 3}) {
                        const bool option = (keys & 1) != 0;
                        const bool command = (keys & 2) != 0;
                        Document document = Fixture();
                        TimelineViewport viewport = Viewport();
                        EditLog log;
                        const TimelinePointerModifiers modifiers{shift, option,
                                                                 command};
                        const TimelinePointerTarget target =
                            TimelineHitAt(document, viewport, x, row, 700.0,
                                          300.0, {50, 10}, tool, modifiers);
                        TimelineInteraction interaction(document, log,
                                                        viewport);
                        // What the window does before a press, as
                        // fc2-main-mm.diff wires it.
                        interaction.SetTrimMode(target.trim_mode);
                        interaction.SetSelectionToggle(target.selection_toggle);
                        interaction.PointerDown(x, row, 700.0, 10);
                        interaction.PointerDrag(x + 10.0, row, 700.0);
                        const bool trims =
                            interaction.TrimPreview().has_value();
                        const bool moves =
                            interaction.MovePreview().has_value();
                        const bool slips =
                            target.trim_mode == TimelineTrimMode::Slip;
                        const std::string name =
                            std::string(tool == Tool::Trim ? "trim"
                                                           : "select") +
                            " x=" + std::to_string(static_cast<int>(x)) +
                            (shift ? " shift" : "") +
                            (option ? " option" : "") +
                            (command ? " command" : "");
                        if (target.kind == Kind::None)
                            Check(
                                !interaction.HasActiveDrag(),
                                "nothing under the pointer, no drag: " + name);
                        else if (target.kind == Kind::ClipEdge)
                            Check(trims && !moves,
                                  "an edge is trimmed: " + name);
                        else
                            Check(slips ? trims && !moves : moves && !trims,
                                  "a body is moved, or slipped: " + name);
                    }
                }
            }
        }
    });

    Test("FC3: the Trim tool's name bar slides, Retime ends stretch", [] {
        using Kind = TimelinePointerTargetKind;
        using Tool = TimelinePointerTool;
        const Document document = Fixture();
        const TimelineViewport viewport = Viewport();
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const double row = kTimelineRulerHeight + 20.0;
        const double nameBar =
            kTimelineRulerHeight + ui::theme::kTimelineClipNameBandTop + 2.0;
        const double retime = kTimelineRulerHeight +
                              ui::theme::kTimelineClipBodyTop +
                              (ui::theme::kTimelineRetimeBandHeight / 2.0);
        const auto at = [&](double x, double y, Tool tool,
                            TimelinePointerModifiers modifiers,
                            const std::vector<Ulid>& retimed = {}) {
            return TimelineHitAt(document, viewport, x, y, 700.0, 300.0,
                                 {50, 10}, tool, modifiers, retimed);
        };
        const TimelinePointerTarget slide = at(250.0, nameBar, Tool::Trim, {});
        Check(slide.kind == Kind::ClipBody &&
                  slide.trim_mode == TimelineTrimMode::Slide &&
                  slide.clip_id == first,
              "Trim, the name bar slides");
        Check(
            at(250.0, row, Tool::Trim, {}).trim_mode == TimelineTrimMode::Slip,
            "Trim, above it the body slips");
        Check(at(348.0, nameBar, Tool::Trim, {}).trim_mode ==
                      TimelineTrimMode::Ripple &&
                  at(348.0, nameBar, Tool::Trim, {}).kind == Kind::ClipEdge,
              "Trim, an edge in the name bar still ripples");
        Check(at(250.0, nameBar, Tool::Select, {}).trim_mode ==
                  TimelineTrimMode::Normal,
              "Select, the name bar moves the clip as before");
        const TimelinePointerTarget tail =
            at(348.0, retime, Tool::Select, {true, false, false}, {first});
        Check(tail.kind == Kind::ClipEdge &&
                  tail.edge == TimelineHitEdge::Tail &&
                  tail.trim_mode == TimelineTrimMode::RateStretch &&
                  !tail.selection_toggle,
              "Retime controls shown: their tail end stretches, Shift or not");
        const TimelinePointerTarget head =
            at(152.0, retime, Tool::Trim, {false, false, true}, {first});
        // DEC40 -- with the Trim tool the stretch ripples.
        Check(head.kind == Kind::ClipEdge &&
                  head.edge == TimelineHitEdge::Head &&
                  head.trim_mode == TimelineTrimMode::RippleRateStretch &&
                  TimelineTrimModeStretches(head.trim_mode),
              "and their head end, with the Trim tool and Command too, "
              "where the stretch ripples");
        Check(!TimelineTrimModeStretches(TimelineTrimMode::Ripple) &&
                  TimelineTrimModeStretches(tail.trim_mode),
              "a ripple is no stretch; the Select tool's stretch is one");
        Check(at(250.0, retime, Tool::Select, {}, {first}).kind ==
                      Kind::ClipBody &&
                  at(348.0, row, Tool::Select, {}, {first}).trim_mode ==
                      TimelineTrimMode::Normal &&
                  at(348.0, retime, Tool::Select, {}).trim_mode ==
                      TimelineTrimMode::Normal &&
                  at(348.0, retime, Tool::Cut, {}, {first}).trim_mode ==
                      TimelineTrimMode::Normal,
              "the band's middle, the edge under it, a clip without the "
              "controls and the blade do not stretch");
        const std::vector<TimelineRetimeBand> bands =
            VisibleTimelineRetimeBands(document, viewport, 700.0, {first});
        Check(bands.size() == 1 && bands[0].x == 150.0 &&
                  bands[0].width == 200.0 &&
                  bands[0].y ==
                      kTimelineRulerHeight + ui::theme::kTimelineClipBodyTop &&
                  bands[0].height == ui::theme::kTimelineRetimeBandHeight &&
                  bands[0].head_x == 150.0 &&
                  bands[0].head_width == kTimelineEdgeHitWidth &&
                  bands[0].tail_x == 350.0 - kTimelineEdgeHitWidth &&
                  bands[0].tail_width == kTimelineEdgeHitWidth &&
                  bands[0].label == "100%",
              "the band is drawn where its ends are pressed, at 100%");
    });

    Test("FC3: a slide drag moves the clip between its neighbours", [] {
        // 10 i/s. V1 and A1: three clips end to end, each pair linked:
        // [0, 20) reads 10..30, [20, 40) reads 40..60, [40, 60) 70..90.
        Document document;
        document.sources = {
            {"01KT0000000000000000000001", "source.mov", {10, 1}, {100, 10}}};
        document.sequence.frame_rate = {10, 1};
        DocumentTrack video{"01KT0000000000000000000002", "video", 0, {}};
        DocumentTrack audio{"01KT0000000000000000000005", "audio", 1, {}};
        const std::array<const char*, 3> videoIds = {
            "01KT0000000000000000000010", "01KT0000000000000000000011",
            "01KT0000000000000000000012"};
        const std::array<const char*, 3> audioIds = {
            "01KT0000000000000000000020", "01KT0000000000000000000021",
            "01KT0000000000000000000022"};
        const std::array<const char*, 3> links = {"01KT0000000000000000000030",
                                                  "01KT0000000000000000000031",
                                                  "01KT0000000000000000000032"};
        for (size_t index = 0; index < 3; ++index) {
            DocumentClip clip;
            clip.id = videoIds[index];
            clip.source_id = "01KT0000000000000000000001";
            clip.source_in = {static_cast<int64_t>(10 + 30 * index), 10};
            clip.duration = {20, 10};
            clip.timeline_in = {static_cast<int64_t>(20 * index), 10};
            clip.include_audio = false;
            clip.link_group_id = links[index];
            video.clips.push_back(clip);
            clip.id = audioIds[index];
            clip.include_audio = true;
            audio.clips.push_back(clip);
        }
        document.sequence.tracks = {video, audio};
        std::string error;
        Check(document.Validate(error), "the slide fixture is valid: " + error);
        const auto tracks = TimelineTracksInDisplayOrder(document);
        const size_t videoRow = static_cast<size_t>(std::distance(
            tracks.begin(), std::find_if(tracks.begin(), tracks.end(),
                                         [](const DocumentTrack* track) {
                                             return track->kind == "video";
                                         })));
        const double nameBar = kTimelineRulerHeight + (videoRow * 40.0) +
                               ui::theme::kTimelineClipNameBandTop + 2.0;
        const auto slide = [&](Document& edited, EditLog& log, double toX,
                               bool linked) {
            TimelineViewport viewport = Viewport();
            TimelineInteraction interaction(edited, log, viewport);
            interaction.SetLinkedSelectionEnabled(linked);
            interaction.SetTrimMode(TimelineTrimMode::Slide);
            // The middle clip spans x 250 to 450.
            interaction.PointerDown(350.0, nameBar, 700.0, 10);
            interaction.PointerDrag(toX, nameBar, 700.0);
            EditError editError = EditError::None;
            std::string message;
            return interaction.PointerUp(editError, message);
        };
        const auto times = [](const Document& edited, const char* id) {
            const DocumentClip* clip = edited.FindClip(id);
            return clip ? std::to_string(clip->source_in.value) + "/" +
                              std::to_string(clip->duration.value) + "@" +
                              std::to_string(clip->timeline_in.value)
                        : std::string("gone");
        };
        {
            Document edited = document;
            EditLog log;
            Check(slide(edited, log, 380.0, true) && log.AppliedCount() == 1,
                  "a 30-point drag on the name bar slides, in one edit");
            const auto entries = log.AppliedEntries();
            const SlideEditOperation* slid =
                entries.empty()
                    ? nullptr
                    : std::get_if<SlideEditOperation>(&entries.back().op);
            Check(slid && slid->delta == RationalTime{3, 10} &&
                      slid->clip_ids.size() == 2 &&
                      slid->clip_ids[1] == Ulid(audioIds[1]),
                  "a SlideEditOperation of three frames, its sound along");
            Check(times(edited, videoIds[0]) == "10/23@0" &&
                      times(edited, videoIds[1]) == "40/20@23" &&
                      times(edited, videoIds[2]) == "73/17@43" &&
                      times(edited, audioIds[1]) == "40/20@23",
                  "the clip keeps its source and length, its neighbours "
                  "absorb the move: " +
                      times(edited, videoIds[0]) + " " +
                      times(edited, videoIds[1]) + " " +
                      times(edited, videoIds[2]));
            EditError editError = EditError::None;
            std::string message;
            Check(log.Undo(edited, editError, message) &&
                      edited.SaveToString() == document.SaveToString(),
                  "one undo gives the montage back byte for byte");
        }
        {
            Document edited = document;
            EditLog log;
            slide(edited, log, 380.0, false);
            Check(times(edited, videoIds[1]) == "40/20@23" &&
                      times(edited, audioIds[1]) == "40/20@20",
                  "without the linked selection the picture slides alone");
        }
        {
            Document edited = document;
            EditLog log;
            // 25 frames wanted: the next clip keeps one frame, at 19.
            Check(slide(edited, log, 600.0, true) &&
                      times(edited, videoIds[1]) == "40/20@39" &&
                      times(edited, videoIds[2]) == "89/1@59",
                  "dragged too far, it stops where the next clip keeps a "
                  "frame: " +
                      times(edited, videoIds[1]));
        }
    });

    Test("FC3: a stretch drag changes the length, so the speed, exactly", [] {
        Document document = Fixture();
        document.sequence.frame_rate = {10, 1};
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const double row = kTimelineRulerHeight + 20.0;
        const auto stretch = [&](Document& edited, EditLog& log, double fromX,
                                 double toX) {
            TimelineViewport viewport = Viewport();
            TimelineInteraction interaction(edited, log, viewport);
            interaction.SetTrimMode(TimelineTrimMode::RateStretch);
            interaction.PointerDown(fromX, row, 700.0, 10);
            interaction.PointerDrag(toX, row, 700.0);
            const std::optional<Operation> pending =
                interaction.PendingOperation();
            EditError error = EditError::None;
            std::string message;
            const bool applied = interaction.PointerUp(error, message);
            return std::make_pair(applied, pending);
        };
        {
            Document edited = document;
            EditLog log;
            // The tail, from 3 s, half a second later: 25 frames for 20.
            const auto [applied, pending] = stretch(edited, log, 348.0, 398.0);
            const RateStretchOperation* operation =
                pending ? std::get_if<RateStretchOperation>(&*pending)
                        : nullptr;
            const DocumentClip* clip = edited.FindClip(first);
            Check(applied && operation && operation->edge == TrimEdge::Tail &&
                      operation->duration == RationalTime{25, 10} && clip &&
                      clip->duration == RationalTime{25, 10} &&
                      clip->speed.num * 5 == clip->speed.den * 4 &&
                      clip->source_in == RationalTime{10, 10},
                  "stretched to 25 frames, the clip plays its 20 at 4/5");
        }
        {
            Document edited = document;
            EditLog log;
            // Past the next clip, at 4 s: held where it begins.
            const auto [applied, pending] = stretch(edited, log, 348.0, 498.0);
            const DocumentClip* clip = edited.FindClip(first);
            Check(applied && clip && clip->duration == RationalTime{30, 10} &&
                      clip->speed.num * 3 == clip->speed.den * 2,
                  "dragged into the next clip, it stops against it, at 2/3");
        }
        {
            Document edited = document;
            EditLog log;
            // The head, half a second later: 15 frames, the tail kept.
            const auto [applied, pending] = stretch(edited, log, 152.0, 202.0);
            const DocumentClip* clip = edited.FindClip(first);
            Check(applied && clip && clip->duration == RationalTime{15, 10} &&
                      clip->timeline_in == RationalTime{15, 10} &&
                      clip->speed.num * 3 == clip->speed.den * 4,
                  "its head stretched, the clip ends where it did, at 4/3");
            EditError error = EditError::None;
            std::string message;
            Check(log.Undo(edited, error, message) &&
                      edited.SaveToString() == document.SaveToString(),
                  "one undo gives it back byte for byte");
        }
    });

    // Cases 96 and 107b of VISUAL_QA_CHECKLIST.md -- a trim the operation
    // refuses is not proposed.
    Test("a drag on a retimed clip proposes only what the trim accepts", [] {
        const auto drag = [](Document& edited, EditLog& log, double fromX,
                             double toX) {
            TimelineViewport viewport = Viewport();
            TimelineInteraction interaction(edited, log, viewport);
            interaction.SetSnappingEnabled(false);
            const double row = kTimelineRulerHeight + 20.0;
            interaction.PointerDown(fromX, row, 700.0, 10);
            interaction.PointerDrag(toX, row, 700.0);
            const bool valid = interaction.TrimPreview().has_value() &&
                               interaction.TrimPreview()->valid;
            EditError error = EditError::None;
            std::string message;
            const bool applied = interaction.PointerUp(error, message);
            return std::make_pair(valid, applied);
        };
        {
            // A at 2/5 reads 0.8 s from 1 s: its head moves by whole frames
            // of the rush only, every fifth sequence frame.
            Document document = Fixture();
            document.sequence.frame_rate = {10, 1};
            document.sequence.tracks[0].clips[0].speed = {2, 5};
            Document edited = document;
            EditLog log;
            // Seven frames asked: five apply, the head lands on frame 12.
            const auto [valid, applied] = drag(edited, log, 152.0, 222.0);
            const DocumentClip& clip = edited.sequence.tracks[0].clips[0];
            Check(valid && applied &&
                      clip.timeline_in == RationalTime{15, 10} &&
                      clip.duration == RationalTime{15, 10} &&
                      clip.source_in == RationalTime{12, 10},
                  "seven frames asked at 2/5, the head stops at five, on a "
                  "frame of the rush");
            EditError error = EditError::None;
            std::string message;
            Check(log.Undo(edited, error, message) &&
                      edited.SaveToString() == document.SaveToString(),
                  "one undo gives it back");
        }
        {
            // B at 2/1 reads 2 s from 4 s of a 10 s rush: 4 s of rush, so
            // 2 s of timeline, are left after it, not 5.
            Document document = Fixture();
            document.sequence.frame_rate = {10, 1};
            document.sequence.tracks[0].clips[1].speed = {2, 1};
            Document edited = document;
            EditLog log;
            // Three seconds asked.
            const auto [valid, applied] = drag(edited, log, 548.0, 848.0);
            const DocumentClip& clip = edited.sequence.tracks[0].clips[1];
            Check(valid && applied && clip.duration == RationalTime{30, 10},
                  "at 2/1 the tail stops where the rush ends, two seconds "
                  "on, instead of proposing a trim the operation refuses");
        }
    });

    Test("DEC40: the Trim tool's stretch ripples what follows", [] {
        Document document = Fixture();
        document.sequence.frame_rate = {10, 1};
        const Ulid first = document.sequence.tracks[0].clips[0].id;
        const Ulid second = document.sequence.tracks[0].clips[1].id;
        const double row = kTimelineRulerHeight + 20.0;
        const auto stretch = [&](Document& edited, EditLog& log, double fromX,
                                 double toX) {
            TimelineViewport viewport = Viewport();
            TimelineInteraction interaction(edited, log, viewport);
            interaction.SetTrimMode(TimelineTrimMode::RippleRateStretch);
            interaction.PointerDown(fromX, row, 700.0, 10);
            interaction.PointerDrag(toX, row, 700.0);
            const std::optional<Operation> pending =
                interaction.PendingOperation();
            const std::optional<TimelineTrimPreview> preview =
                interaction.TrimPreview();
            EditError error = EditError::None;
            std::string message;
            const bool applied = interaction.PointerUp(error, message);
            return std::make_tuple(applied, pending, preview);
        };
        const auto at = [](const Document& edited, const Ulid& id) {
            const DocumentClip* clip = edited.FindClip(id);
            return clip ? clip->timeline_in : RationalTime{-1, 1};
        };
        {
            Document edited = document;
            EditLog log;
            // The tail, from 3 s, half a second later: B, at 4 s, follows.
            const auto [applied, pending, preview] =
                stretch(edited, log, 348.0, 398.0);
            const RateStretchOperation* operation =
                pending ? std::get_if<RateStretchOperation>(&*pending)
                        : nullptr;
            const DocumentClip* clip = edited.FindClip(first);
            Check(applied && operation && operation->ripple &&
                      operation->duration == RationalTime{25, 10} && clip &&
                      clip->speed.num * 5 == clip->speed.den * 4 &&
                      at(edited, second) == RationalTime{45, 10},
                  "stretched to 25 frames, the clip plays at 4/5 and the "
                  "next one moves half a second later");
            Check(preview && std::any_of(preview->linked_times.begin(),
                                         preview->linked_times.end(),
                                         [&](const auto& member) {
                                             return member.first == second &&
                                                    member.second.timeline_in ==
                                                        RationalTime{45, 10};
                                         }),
                  "the drag previews the next clip where it will land");
        }
        {
            Document edited = document;
            EditLog log;
            // Past where the next clip began: nothing stops it, B moves.
            const auto [applied, pending, preview] =
                stretch(edited, log, 348.0, 498.0);
            const DocumentClip* clip = edited.FindClip(first);
            Check(applied && clip && clip->duration == RationalTime{35, 10} &&
                      at(edited, second) == RationalTime{55, 10},
                  "dragged past the next clip's head, the clip takes the "
                  "full length and pushes it");
        }
        {
            Document edited = document;
            EditLog log;
            // The head, half a second later: the head stays, B comes back.
            const auto [applied, pending, preview] =
                stretch(edited, log, 152.0, 202.0);
            const DocumentClip* clip = edited.FindClip(first);
            Check(applied && clip && clip->duration == RationalTime{15, 10} &&
                      clip->timeline_in == RationalTime{10, 10} &&
                      at(edited, second) == RationalTime{35, 10},
                  "its head stretched, the clip keeps its head and the next "
                  "one comes half a second closer");
            EditError error = EditError::None;
            std::string message;
            Check(log.Undo(edited, error, message) &&
                      edited.SaveToString() == document.SaveToString(),
                  "one undo gives it back byte for byte");
        }
    });

    Test("DEC40: command-click extends the selection, a drag still moves it",
         [] {
             Document document = Fixture();
             const Ulid first = document.sequence.tracks[0].clips[0].id;
             const Ulid second = document.sequence.tracks[0].clips[1].id;
             const TimelineViewport view = Viewport();
             const double videoY = kTimelineRulerHeight + 20.0;
             const auto target =
                 [&](double x, TimelinePointerModifiers modifiers,
                     TimelinePointerTool tool = TimelinePointerTool::Select) {
                     return TimelineHitAt(document, view, x, videoY, 700.0,
                                          300.0, {50, 10}, tool, modifiers);
                 };
             const TimelinePointerTarget command =
                 target(250.0, {false, false, true});
             Check(command.selection_extend && !command.selection_toggle &&
                       !command.duplicate,
                   "Command on a body extends the selection");
             Check(target(250.0, {true, false, true}).selection_toggle &&
                       !target(250.0, {true, false, true}).selection_extend,
                   "with Shift too, Shift's toggle wins");
             Check(target(250.0, {false, true, true}).duplicate &&
                       !target(250.0, {false, true, true}).selection_extend,
                   "Option-Command stays a duplicate");
             Check(!target(348.0, {false, false, true}).selection_extend &&
                       target(348.0, {false, false, true}).trim_mode ==
                           TimelineTrimMode::Roll,
                   "on an edge Command still rolls");
             Check(
                 !target(250.0, {false, false, true}, TimelinePointerTool::Trim)
                      .selection_extend,
                 "the Trim tool's body slips, Command or not");

             TimelineViewport viewport = Viewport();
             EditLog log;
             TimelineInteraction interaction(document, log, viewport);
             EditError error = EditError::None;
             std::string message;
             interaction.PointerDown(250.0, videoY, 700.0, 10);
             interaction.PointerUp(error, message);
             interaction.SetSelectionExtend(true);
             interaction.PointerDown(500.0, videoY, 700.0, 10);
             interaction.PointerUp(error, message);
             Check(interaction.SelectedClipIds() ==
                           std::vector<Ulid>{first, second} &&
                       interaction.PrimarySelectedClipId() == second &&
                       interaction.SelectedClipId().empty(),
                   "command-click adds the clip and makes it the primary one");
             interaction.PointerDown(250.0, videoY, 700.0, 10);
             Check(interaction.SelectedClipIds() ==
                           std::vector<Ulid>{first, second} &&
                       interaction.HasActiveDrag(),
                   "pressed again, a selected clip stays in until the release");
             interaction.PointerDrag(300.0, videoY, 700.0);
             Check(interaction.PointerUp(error, message),
                   "a command-drag moves the selection: " + message);
             const auto& clips = document.sequence.tracks[0].clips;
             Check(clips[0].timeline_in == RationalTime{15, 10} &&
                       clips[1].timeline_in == RationalTime{45, 10} &&
                       log.AppliedCount() == 1 &&
                       interaction.SelectedClipIds() ==
                           std::vector<Ulid>{first, second},
                   "both clips move, and both stay selected");
             // The first clip now spans 1.5 s to 3.5 s, x 200 to 400.
             interaction.PointerDown(300.0, videoY, 700.0, 10);
             interaction.PointerUp(error, message);
             Check(interaction.SelectedClipIds() == std::vector<Ulid>{second} &&
                       interaction.PrimarySelectedClipId() == second &&
                       interaction.SelectedClipId() == second &&
                       log.AppliedCount() == 1,
                   "a command-click that moves nothing takes it out, "
                   "editing nothing");
             document.sequence.tracks[0].locked = true;
             interaction.PointerDown(550.0, videoY, 700.0, 10);
             Check(interaction.SelectedClipIds().empty() &&
                       !interaction.HasActiveDrag(),
                   "on a locked track, where nothing is dragged, the press "
                   "takes it out at once");
         });

    Test("FC2: a transition is found where it is drawn", [] {
        Document document = Fixture();
        // Close the gap: the cut at 3 s, x = 350.
        document.sequence.tracks[0].clips[1].timeline_in = {30, 10};
        DocumentTransition transition;
        transition.id = "01KT0000000000000000000020";
        transition.track_id = document.sequence.tracks[0].id;
        transition.left_clip_id = document.sequence.tracks[0].clips[0].id;
        transition.right_clip_id = document.sequence.tracks[0].clips[1].id;
        // Four frames at 25 fps, centred on the cut: x 342 to 358.
        transition.duration = {4, 25};
        document.sequence.transitions.push_back(transition);
        const TimelineViewport viewport = Viewport();
        const std::vector<TimelineTransitionRect> rects =
            VisibleTimelineTransitions(document, viewport, 700.0);
        Check(rects.size() == 1 && std::abs(rects[0].x - 342.0) < 1e-9 &&
                  std::abs(rects[0].width - 16.0) < 1e-9 &&
                  rects[0].y == kTimelineRulerHeight + 4.0 &&
                  rects[0].height == viewport.track_height - 8.0 &&
                  std::abs(rects[0].cut_x - 350.0) < 1e-9,
              "the transition spans its frames, inset in its row");
        const double row = kTimelineRulerHeight + 20.0;
        const TimelinePointerTarget over =
            TimelineHitAt(document, viewport, 345.0, row, 700.0, 300.0,
                          {50, 10}, TimelinePointerTool::Select, {});
        Check(over.transition_id == transition.id &&
                  over.kind == TimelinePointerTargetKind::ClipEdge &&
                  over.clip_id == transition.left_clip_id,
              "over a transition, the press still takes the clip's edge");
        Check(TimelineHitAt(document, viewport, 345.0,
                            kTimelineRulerHeight + 2.0, 700.0, 300.0, {50, 10},
                            TimelinePointerTool::Select, {})
                      .transition_id.empty() &&
                  TimelineHitAt(document, viewport, 330.0, row, 700.0, 300.0,
                                {50, 10}, TimelinePointerTool::Select, {})
                      .transition_id.empty(),
              "above its inset, or beside it, there is no transition");
    });

    return failures == 0 ? 0 : 1;
}
