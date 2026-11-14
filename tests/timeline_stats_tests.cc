// QC-2026-09 (A8) -- the density check must follow the composited picture,
// not the number of clips stored on its tracks. This is deliberately a pure
// C++ test: the same result is consumed by CLI, MCP and a future UI.

#include "TimelineStats.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

Document Fixture() {
    Document document;
    document.sequence.tracks = {
        {"01K30000000000000000000001",
         "video",
         0,
         {{"01K30000000000000000000011", {}, {}, {10, 25}, {0, 25}},
          {"01K30000000000000000000012", {}, {}, {10, 25}, {10, 25}},
          {"01K30000000000000000000013", {}, {}, {10, 25}, {20, 25}}}},
        {"01K30000000000000000000002",
         "video",
         1,
         {{"01K30000000000000000000021", {}, {}, {20, 25}, {10, 25}}}},
    };
    return document;
}

}  // namespace

int main() {
    Document document = Fixture();
    TimelineStats stats;
    std::string error;
    Check(CalculateTimelineStats(document, stats, error),
          "overlap fixture calculates: " + error);
    Check(stats.duration == RationalTime{30, 25},
          "duration spans the full timeline");
    Check(stats.visible_shots == 2,
          "clips hidden by V2 do not become visible shots");
    Check(stats.visible_cuts == 1, "only the visible transition is a cut");
    Check(stats.first_cut && *stats.first_cut == RationalTime{10, 25},
          "first visible cutaway is at the overlay start");
    Check(stats.plans_per_minute.numerator == 100 &&
              stats.plans_per_minute.denominator == 1,
          "plans per minute is an exact reduced ratio");
    Check(stats.cutaway_plans == 1 && stats.cutaway_share.numerator == 1 &&
              stats.cutaway_share.denominator == 2,
          "cutaway share counts visible plans, not hidden V1 clips");

    const std::string expected =
        "{\"duration\":{\"value\":30,\"rate\":25},"
        "\"visible_shots\":2,\"visible_cuts\":1,"
        "\"plans_per_minute\":{\"numerator\":100,\"denominator\":1},"
        "\"cutaway_plans\":1,\"cutaway_share\":{\"numerator\":1,"
        "\"denominator\":2},\"first_cut\":{\"value\":10,"
        "\"rate\":25}}";
    Check(SerializeTimelineStats(stats) == expected,
          "timeline stats JSON is canonical");

    document.sequence.tracks[1].visible = false;
    Check(CalculateTimelineStats(document, stats, error),
          "hidden overlay calculates: " + error);
    Check(stats.visible_shots == 3 && stats.visible_cuts == 2 &&
              stats.cutaway_plans == 0,
          "hidden tracks do not create visible plans or cutaways");
    Check(!stats.first_cut,
          "primary-track edits are not reported as a first cutaway");

    Document invalid = document;
    invalid.sequence.tracks[0].clips[0].duration.rate = 0;
    Check(!CalculateTimelineStats(invalid, stats, error) &&
              error ==
                  "clip '01K30000000000000000000011' has a non-positive "
                  "time rate",
          "invalid time rates are refused with a stable error");

    // AGENT-2026-09 A5 -- picture and sound of a link group are compared
    // where they overlap: a pair reading its source at the same offset is
    // in sync, a pair that is not names its drift, and a timeline without
    // links reports nothing.
    {
        Document linked = Fixture();
        const Ulid source = "01K30000000000000000000099";
        const Ulid group = "01K30000000000000000000050";
        DocumentClip& video = linked.sequence.tracks[0].clips[0];
        video.source_id = source;
        video.source_in = {100, 25};
        video.link_group_id = group;
        // A second fragment of the same block, same link id, further on.
        DocumentClip& videoLater = linked.sequence.tracks[0].clips[2];
        videoLater.source_id = source;
        videoLater.source_in = {140, 25};
        videoLater.link_group_id = group;
        DocumentTrack audio;
        audio.id = "01K30000000000000000000003";
        audio.kind = "audio";
        audio.index = 2;
        // In sync with the first fragment...
        audio.clips.push_back({"01K30000000000000000000031",
                               source,
                               {100, 25},
                               {10, 25},
                               {0, 25}});
        audio.clips.back().link_group_id = group;
        // ...and two frames early against the second: the picture runs
        // ahead of its sound.
        audio.clips.push_back({"01K30000000000000000000032",
                               source,
                               {138, 25},
                               {10, 25},
                               {20, 25}});
        audio.clips.back().link_group_id = group;
        linked.sequence.tracks.push_back(audio);
        Check(CalculateTimelineStats(linked, stats, error),
              "linked fixture calculates: " + error);
        Check(stats.linked_pairs == 2,
              "each fragment is paired with the sound it overlaps, not with "
              "every fragment of the group: " +
                  std::to_string(stats.linked_pairs));
        Check(stats.out_of_sync.size() == 1 &&
                  stats.out_of_sync[0].video_clip_id ==
                      "01K30000000000000000000013" &&
                  stats.out_of_sync[0].audio_clip_id ==
                      "01K30000000000000000000032" &&
                  stats.out_of_sync[0].drift == RationalTime{2, 25},
              "the pair that drifted is named with its drift");
        const std::string serialized = SerializeTimelineStats(stats);
        Check(
            serialized.find("\"sync\":{\"linked_pairs\":2,\"out_of_sync\":[{"
                            "\"video_clip_id\":\"01K30000000000000000000013\","
                            "\"audio_clip_id\":\"01K30000000000000000000032\","
                            "\"drift\":{\"value\":2,\"rate\":25}}]}") !=
                std::string::npos,
            "sync is serialized with the drift as an exact time: " +
                serialized);
        Check(SerializeTimelineStats(TimelineStats{}).find("sync") ==
                  std::string::npos,
              "a timeline without links says nothing about sync");
    }

    return failures == 0 ? 0 : 1;
}
