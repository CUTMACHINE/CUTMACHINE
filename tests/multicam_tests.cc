// F1.5 -- ROADMAP.md. The multicam picture rule (Multicam.h), the builders
// every surface shares, and the consumers that must agree on which angle is
// on screen: the monitor's Timeline, timeline_stats, the timeline sheets, the
// shot quality report and the Resolve return trip. The export is checked
// frame by frame in export_tests, the CLI in cli_tests, MCP in
// mcp_tools_tests.

#include "EditLog.h"
#include "Json.h"
#include "Multicam.h"
#include "Operations.h"
#include "ResolveExport.h"
#include "ShotQuality.h"
#include "Timeline.h"
#include "TimelineSheets.h"
#include "TimelineStats.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
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

const Ulid kSource = "01K9M000000000000000000001";
const Ulid kWideTrack = "01K9M000000000000000000002";
const Ulid kCloseTrack = "01K9M000000000000000000003";
const Ulid kSoundTrack = "01K9M000000000000000000004";
const Ulid kWide = "01K9M000000000000000000011";
const Ulid kClose = "01K9M000000000000000000012";
const Ulid kCloseSound = "01K9M000000000000000000013";
const Ulid kTail = "01K9M000000000000000000014";

DocumentClip Clip(const Ulid& id, int64_t sourceIn, int64_t duration,
                  int64_t timelineIn) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = kSource;
    clip.source_in = {sourceIn, 25};
    clip.duration = {duration, 25};
    clip.timeline_in = {timelineIn, 25};
    return clip;
}

// Two synchronised cameras over the same two seconds: the wide shot on V1,
// the close-up on V2 with its own sound on A1, then one more V1 shot.
Document Fixture() {
    Document document;
    LibraryMedia media;
    media.id = kSource;
    media.path = "/rushes/camera.mp4";
    media.filename = "camera.mp4";
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.orientation = "landscape";
    media.rate = {25, 1};
    media.duration = {1000, 25};
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    document.library = {media};
    document.sources = {{kSource, media.path, {25, 1}, {1000, 25}}};
    DocumentTrack wide{kWideTrack, "video", 0, {}};
    wide.clips = {Clip(kWide, 0, 50, 25), Clip(kTail, 200, 25, 75)};
    DocumentTrack close{kCloseTrack, "video", 1, {Clip(kClose, 100, 50, 25)}};
    DocumentTrack sound{
        kSoundTrack, "audio", 2, {Clip(kCloseSound, 100, 50, 25)}};
    document.sequence.tracks = {wide, close, sound};
    return document;
}

bool Apply(EditLog& log, Document& document, Operation operation,
           const std::string& label) {
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        log.Apply(document, std::move(operation), error, message);
    Check(applied, label + ": " + message);
    return applied;
}

Ulid TrackClipAt(const Document& document, const Ulid& trackId, int64_t frame) {
    const auto resolved =
        Timeline(document).ResolveTrack(trackId, RationalTime{frame, 25});
    return resolved ? resolved->clip_id : Ulid{};
}

std::vector<Ulid> ContactSheetClips(const Document& document) {
    TimelineSheetPlan plan;
    std::string error;
    std::vector<Ulid> clips;
    if (!BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, {}, plan,
                                error)) {
        Check(false, "contact sheet plan builds: " + error);
        return clips;
    }
    for (const TimelineSheetFrame& frame : plan.frames) {
        clips.push_back(frame.clip_id);
    }
    return clips;
}

// clip id -> fully_covered, as list_shot_quality reports it.
std::map<Ulid, bool> CoveredByShotQuality(const Document& document) {
    ShotQualityReport report;
    report.media_id = kSource;
    report.samples_per_second = 4;
    report.analysis_width = 384;
    report.analysis_height = 216;
    for (int index = 0; index < 60; ++index) {
        ShotQualitySample sample;
        sample.time = RationalTime{index, 4};
        sample.sharpness = 4000;
        sample.motion = 2000;
        report.samples.push_back(sample);
    }
    report.median_sharpness = 4000;
    report.median_motion = 2000;
    mcp_json::Value view;
    std::string error;
    std::map<Ulid, bool> covered;
    if (!mcp_json::Value::Parse(
            DescribeShotQualityForAgent(document, {{kSource, report}},
                                        ShotQualityThresholds{},
                                        ShotSegmentationSettings{}),
            view, error)) {
        Check(false, "shot quality view parses: " + error);
        return covered;
    }
    const mcp_json::Value* clips = view.Find("clips");
    if (clips == nullptr || !clips->IsArray()) {
        return covered;
    }
    for (const mcp_json::Value& entry : clips->AsArray()) {
        const mcp_json::Value* id = entry.Find("clip_id");
        const mcp_json::Value* flag = entry.Find("fully_covered");
        if ((id != nullptr) && (flag != nullptr)) {
            covered[id->AsString()] = flag->AsBool();
        }
    }
    return covered;
}

std::vector<int32_t> ResolveLayers(const Document& document,
                                   std::string& error) {
    ResolveTimelineExport exported;
    std::vector<int32_t> layers;
    if (!BuildResolveTimelineExport(document, exported, error)) {
        return layers;
    }
    for (const ResolveTimelineClip& clip : exported.clips) {
        if (!clip.audio) layers.push_back(clip.track - 1);
    }
    return layers;
}

}  // namespace

int main() {
    Document document = Fixture();
    std::string validation;
    Check(document.Validate(validation), "fixture validates: " + validation);
    const std::string original = document.SaveToString();
    EditLog log;
    EditError error = EditError::None;
    std::string message;

    // --- Builders: what a group may be ---------------------------------
    AddMulticamGroupOperation add;
    Check(!BuildAddMulticamGroup(document, "", {{kWide, ""}}, {}, add, error,
                                 message) &&
              error == EditError::InvalidOperation,
          "a group of one angle is refused");
    Check(!BuildAddMulticamGroup(document, "", {{kWide, ""}, {kCloseSound, ""}},
                                 {}, add, error, message) &&
              error == EditError::InvalidOperation,
          "an audio clip is not an angle");
    Check(!BuildAddMulticamGroup(document, "", {{kWide, ""}, {kWide, ""}}, {},
                                 add, error, message) &&
              error == EditError::InvalidOperation,
          "a clip named twice is refused");
    Check(!BuildAddMulticamGroup(
              document, "", {{kWide, ""}, {"01K9M000000000000000000099", ""}},
              {}, add, error, message) &&
              error == EditError::UnknownClip,
          "an unknown clip is refused by name");
    Check(!BuildAddMulticamGroup(document, "", {{kWide, ""}, {kClose, ""}},
                                 kTail, add, error, message) &&
              error == EditError::UnknownMulticamAngle,
          "an active clip outside the angles is refused");
    Check(BuildAddMulticamGroup(document, "", {{kWide, ""}, {kClose, "Serré"}},
                                {}, add, error, message),
          "two video clips make a group: " + message);
    Check(IsValidUlid(add.group.id) && add.group.name == "Multicam 1" &&
              add.group.angles.size() == 2 &&
              IsValidUlid(add.group.angles[0].id) &&
              add.group.angles[0].name == "Angle 1" &&
              add.group.angles[1].name == "Serré" &&
              add.group.active_angle_id.empty(),
          "identities and default names are assigned before the apply");
    const Ulid groupId = add.group.id;
    const Ulid wideAngle = add.group.angles[0].id;
    const Ulid closeAngle = add.group.angles[1].id;

    mcp_json::Value report;
    Check(DescribeMulticamEdit(document, add, report) &&
              (report.Find("group_id") != nullptr) &&
              report.Find("group_id")->AsString() == groupId &&
              (report.Find("angle_ids") != nullptr) &&
              report.Find("angle_ids")->AsArray().size() == 2 &&
              report.Find("angle_ids")->AsArray()[1].AsString() == closeAngle &&
              (report.Find("hidden_clip_ids") != nullptr) &&
              report.Find("hidden_clip_ids")->AsArray().empty(),
          "the dry run reports the ids the apply will keep");
    Check(document.SaveToString() == original,
          "describing an edit never changes the document");
    {
        AddMulticamGroupOperation unassigned = add;
        unassigned.group.angles[1].id.clear();
        Check(!DescribeMulticamEdit(document, unassigned, report),
              "an add whose ids the apply would draw is not described");
    }
    Check(Apply(log, document, add, "add group"), "group is added");
    const std::string grouped = document.SaveToString();
    Check(document.FindMulticamGroup(groupId) != nullptr &&
              document.FindMulticamGroup(groupId)->angles[0].id == wideAngle,
          "the applied group keeps the ids the builder gave it");

    AddMulticamGroupOperation again;
    Check(!BuildAddMulticamGroup(document, "", {{kClose, ""}, {kTail, ""}}, {},
                                 again, error, message) &&
              error == EditError::InvalidOperation &&
              message.find(groupId) != std::string::npos,
          "a clip already in a group is refused, naming that group");

    // --- No active angle: nothing changes -------------------------------
    Check(MulticamHiddenClipIds(document.sequence).empty(),
          "a group without an active angle hides nothing");
    Check(TrackClipAt(document, kWideTrack, 30) == kWide &&
              TrackClipAt(document, kCloseTrack, 30) == kClose,
          "without an active angle both angles resolve as ordinary clips");

    // --- Wide angle active ----------------------------------------------
    SetMulticamActiveAngleOperation set;
    Check(
        BuildSetMulticamActiveAngle(document, {}, kWide, set, error, message) &&
            set.group_id == groupId && set.active_angle_id == wideAngle,
        "an angle is found from the clip it shows: " + message);
    Check(Apply(log, document, set, "activate wide"), "wide angle activates");
    const std::string wideActive = document.SaveToString();
    Check(MulticamHiddenClipIds(document.sequence) == std::set<Ulid>{kClose},
          "the other angle is the one hidden");
    Check(MulticamHidesClip(document.sequence, kClose) &&
              !MulticamHidesClip(document.sequence, kWide) &&
              !MulticamHidesClip(document.sequence, kCloseSound),
          "only the inactive angle's picture is taken off screen");
    Check(TrackClipAt(document, kCloseTrack, 30).empty() &&
              TrackClipAt(document, kWideTrack, 30) == kWide,
          "the monitor sees the wide angle through the hidden close-up");
    Check(Timeline(document)
              .ResolveTrackLayers(kCloseTrack, RationalTime{30, 25})
              .empty(),
          "the hidden angle contributes no layer");
    Check(TrackClipAt(document, kSoundTrack, 30) == kCloseSound,
          "the close-up's own sound keeps playing: audio does not follow");
    {
        const std::vector<ResolvedFrame> upcoming =
            Timeline(document).ResolveUpcoming({0, 25}, 1, {2, 1});
        Check(upcoming.size() == 1 && upcoming[0].clip_id == kWide,
              "playback warms the decoder of the shown angle only");
    }
    TimelineStats stats;
    Check(CalculateTimelineStats(document, stats, message) &&
              stats.visible_shots == 2 && stats.visible_cuts == 1 &&
              stats.cutaway_plans == 0 &&
              stats.duration == RationalTime{100, 25},
          "timeline_stats counts the wide angle, not the covering close-up");
    Check(ContactSheetClips(document) == std::vector<Ulid>{kWide, kTail},
          "the contact sheet samples the angle on screen");
    {
        const std::map<Ulid, bool> covered = CoveredByShotQuality(document);
        Check((covered.count(kClose) != 0u) && covered.at(kClose) &&
                  (covered.count(kWide) != 0u) && !covered.at(kWide),
              "shot quality treats the hidden angle as unseen and the shown "
              "one as uncovered");
    }
    {
        // PA16 -- every clip travels as what it is: the hidden angle's
        // picture stays behind, its sound goes as a clip of its own.
        std::string resolveError;
        Check(
            ResolveLayers(document, resolveError) == std::vector<int32_t>{0, 0},
            "the Resolve trip sends the shown pictures only: " + resolveError);
        ResolveTimelineExport exported;
        Check(BuildResolveTimelineExport(document, exported, resolveError) &&
                  exported.multicam_angles_not_sent == 1 &&
                  std::any_of(exported.clips.begin(), exported.clips.end(),
                              [](const ResolveTimelineClip& clip) {
                                  return clip.audio &&
                                         clip.clip_id == kCloseSound;
                              }),
              "the hidden angle's own sound still travels: " + resolveError);
        Document muted = document;
        muted.sequence.tracks[2].muted = true;
        resolveError.clear();
        Check(BuildResolveTimelineExport(muted, exported, resolveError) &&
                  !exported.tracks.back().enabled &&
                  exported.tracks.back().audio,
              "a muted track arrives disabled: " + resolveError);
    }

    // --- Close angle active ---------------------------------------------
    Check(BuildSetMulticamActiveAngle(document, groupId, closeAngle, set, error,
                                      message) &&
              set.active_angle_id == closeAngle,
          "an angle is found from its own id within its group: " + message);
    Check(Apply(log, document, set, "activate close"), "close angle activates");
    Check(MulticamHiddenClipIds(document.sequence) == std::set<Ulid>{kWide},
          "switching hides the other camera");
    Check(TrackClipAt(document, kWideTrack, 30).empty() &&
              TrackClipAt(document, kWideTrack, 80) == kTail &&
              TrackClipAt(document, kCloseTrack, 30) == kClose,
          "a hidden angle leaves a hole only over its own extent");
    Check(CalculateTimelineStats(document, stats, message) &&
              stats.visible_shots == 2 && stats.cutaway_plans == 1,
          "timeline_stats follows the switch");
    Check(ContactSheetClips(document) == std::vector<Ulid>{kClose, kTail},
          "the contact sheet follows the switch");
    {
        std::string resolveError;
        const std::vector<int32_t> layers =
            ResolveLayers(document, resolveError);
        Check(layers == std::vector<int32_t>{0, 1},
              "a hidden angle without its own sound is simply not sent: " +
                  resolveError);
    }
    {
        // A dissolve from the hidden wide angle into the next shot keeps
        // only the incoming side, fading over whatever is underneath: the
        // layers Export.cc builds for the same timeline.
        Document dissolve = document;
        dissolve.sequence.transitions = {{"01K9M000000000000000000031",
                                          kWideTrack,
                                          kWide,
                                          kTail,
                                          "cross_dissolve",
                                          {10, 25},
                                          TransitionAlignment::Center}};
        const std::vector<ResolvedLayer> layers =
            Timeline(dissolve).ResolveTrackLayers(kWideTrack, {72, 25});
        Check(layers.size() == 1 && layers[0].frame.clip_id == kTail &&
                  layers[0].opacity < 1.0f,
              "a hidden angle drops out of its dissolve");
        if (!dissolve.sequence.multicam_groups.empty()) {
            dissolve.sequence.multicam_groups[0].active_angle_id = wideAngle;
        }
        Check(Timeline(dissolve)
                      .ResolveTrackLayers(kWideTrack, {72, 25})
                      .size() == 2,
              "a shown angle keeps both sides of its dissolve");
    }
    const mcp_json::Value listed = DescribeMulticamGroups(document.sequence);
    const mcp_json::Value* groups = listed.Find("groups");
    Check((groups != nullptr) && groups->AsArray().size() == 1 &&
              (listed.Find("audio_follows_video") != nullptr) &&
              !listed.Find("audio_follows_video")->AsBool(),
          "the list names every group and says sound does not follow");
    if ((groups != nullptr) && groups->AsArray().size() == 1) {
        const mcp_json::Value* angles = groups->AsArray()[0].Find("angles");
        Check((angles != nullptr) && angles->AsArray().size() == 2 &&
                  angles->AsArray()[0].Find("hidden_by_multicam")->AsBool() &&
                  !angles->AsArray()[1].Find("hidden_by_multicam")->AsBool() &&
                  angles->AsArray()[1].Find("active")->AsBool() &&
                  angles->AsArray()[1].Find("track_id")->AsString() ==
                      kCloseTrack,
              "each angle says where it sits and whether it is on screen");
    }

    // --- Clearing, and what cannot be guessed ---------------------------
    Check(!BuildSetMulticamActiveAngle(document, {}, {}, set, error, message) &&
              error == EditError::InvalidOperation,
          "clearing without a group is refused");
    Check(!BuildSetMulticamActiveAngle(document, {}, kTail, set, error,
                                       message) &&
              error == EditError::UnknownMulticamAngle,
          "a clip that is no angle is refused");
    Check(!BuildSetMulticamActiveAngle(document, "01K9M000000000000000000098",
                                       kWide, set, error, message) &&
              error == EditError::UnknownMulticamGroup,
          "an unknown group is refused");
    {
        // The schema lets one clip be the angle of two groups; the builders
        // never make it, but a document may carry it (--apply-op).
        Document shared = document;
        EditLog sharedLog;
        MulticamGroup second;
        second.id = "01K9M000000000000000000041";
        second.name = "Second";
        second.angles = {{"01K9M000000000000000000042", "A", kWide},
                         {"01K9M000000000000000000043", "B", kTail}};
        Check(Apply(sharedLog, shared, AddMulticamGroupOperation{second, -1},
                    "second group over a shared clip"),
              "the operation itself accepts a shared clip");
        Check(!BuildSetMulticamActiveAngle(shared, {}, kWide, set, error,
                                           message) &&
                  error == EditError::InvalidOperation,
              "a clip in two groups is refused without its group");
        Check(BuildSetMulticamActiveAngle(shared, second.id, kWide, set, error,
                                          message) &&
                  set.active_angle_id == second.angles[0].id,
              "naming the group settles it");
        // The second group now shows the wide shot, the first still hides
        // it behind the close-up.
        Check(Apply(sharedLog, shared, set, "activate shared wide") &&
                  MulticamHidesClip(shared.sequence, kWide) &&
                  MulticamHidesClip(shared.sequence, kTail),
              "a clip is off screen as soon as one of its groups hides it");
    }
    Check(BuildSetMulticamActiveAngle(document, groupId, {}, set, error,
                                      message) &&
              set.group_id == groupId && set.active_angle_id.empty(),
          "clearing names the group: " + message);
    Check(Apply(log, document, set, "clear active angle"), "choice clears");
    Check(document.SaveToString() == grouped,
          "clearing restores the bytes of the group with no active angle");

    // --- Removal, and the whole history ---------------------------------
    const Operation remove = RemoveMulticamGroupOperation{groupId};
    Check(DescribeMulticamEdit(document, remove, report) &&
              (report.Find("group") != nullptr) &&
              report.Find("group")->IsNull(),
          "a removal reports the group gone");
    Check(
        !DescribeMulticamEdit(
            document,
            RemoveMulticamGroupOperation{"01K9M000000000000000000097"}, report),
        "a refused edit has nothing to report");
    Check(Apply(log, document, remove, "remove group"), "group dissolves");
    Check(document.SaveToString() == original,
          "dissolving leaves the clips exactly as they were");
    for (int step = 0; step < 5; ++step) {
        Check(log.Undo(document, error, message), "undo step: " + message);
    }
    Check(document.SaveToString() == original,
          "the full multicam history undoes byte for byte");
    Check(log.Redo(document, error, message) &&
              log.Redo(document, error, message) &&
              document.SaveToString() == wideActive,
          "redo replays the same group and angle");

    // F1.5 -- decided on 8 October 2026: a cut through the group at frame
    // 50 cuts both cameras and makes a second group of what follows, which
    // shows the wide shot while the first keeps the close-up. Sound is not
    // cut.
    {
        Document cutDocument = Fixture();
        EditLog cutLog;
        AddMulticamGroupOperation group;
        Check(BuildAddMulticamGroup(cutDocument, "Plateau",
                                    {{kWide, "Large"}, {kClose, "Serré"}},
                                    kClose, group, error, message) &&
                  Apply(cutLog, cutDocument, group, "group the two cameras"),
              "the group is made: " + message);
        const Ulid wideAngle = group.group.angles[0].id;
        const Ulid closeAngle = group.group.angles[1].id;
        const std::string grouped = cutDocument.SaveToString();

        BatchOperation batch;
        MulticamCut cut;
        Check(!BuildSplitMulticamGroup(cutDocument, group.group.id, {10, 25},
                                       {}, batch, cut, error, message) &&
                  error == EditError::InvalidOperation,
              "a position before every angle cuts nothing and is refused");
        Check(!BuildSplitMulticamGroup(cutDocument, group.group.id, {50, 25},
                                       kTail, batch, cut, error, message) &&
                  error == EditError::UnknownMulticamAngle,
              "an angle the second group would not hold is refused");
        Check(!BuildSplitMulticamGroup(cutDocument,
                                       "01K9M000000000000000000098", {50, 25},
                                       {}, batch, cut, error, message) &&
                  error == EditError::UnknownMulticamGroup,
              "an unknown group is refused by name");
        Check(BuildSplitMulticamGroup(cutDocument, group.group.id, {50, 25},
                                      wideAngle, batch, cut, error, message) &&
                  batch.operations.size() == 3 && cut.angle_ids.size() == 2,
              "two splits and the second group: " + message);
        Check(Apply(cutLog, cutDocument, batch, "cut the multicam"),
              "the cut applies");
        const auto& groups = cutDocument.sequence.multicam_groups;
        Check(groups.size() == 2 && groups[0].id == group.group.id &&
                  groups[0].active_angle_id == closeAngle &&
                  groups[0].angles[0].clip_id == kWide &&
                  groups[0].angles[1].clip_id == kClose &&
                  groups[1].id == cut.group_id &&
                  groups[1].active_angle_id == cut.angle_ids[0] &&
                  groups[1].angles[0].name == "Large",
              "the group keeps the left pieces and the close-up; the second "
              "takes the right pieces and shows the wide shot");
        const DocumentClip* wideRight =
            groups.size() == 2
                ? cutDocument.FindClip(groups[1].angles[0].clip_id)
                : nullptr;
        const DocumentClip* closeRight =
            groups.size() == 2
                ? cutDocument.FindClip(groups[1].angles[1].clip_id)
                : nullptr;
        const std::set<Ulid> hidden =
            MulticamHiddenClipIds(cutDocument.sequence);
        Check(wideRight != nullptr && closeRight != nullptr &&
                  wideRight->timeline_in == (RationalTime{50, 25}) &&
                  closeRight->timeline_in == (RationalTime{50, 25}) &&
                  hidden == std::set<Ulid>{kWide, closeRight->id},
              "before the cut the wide shot is hidden, after it the "
              "close-up");
        const DocumentTrack* sound = cutDocument.FindTrack(kSoundTrack);
        Check(sound != nullptr && sound->clips.size() == 1 &&
                  sound->clips[0].duration == (RationalTime{50, 25}),
              "the close-up's sound is not cut: it does not follow the angle");
        Check(cutLog.Undo(cutDocument, error, message) &&
                  cutDocument.SaveToString() == grouped,
              "one undo restores the group and both cameras whole");
        Check(cutLog.Redo(cutDocument, error, message) &&
                  cutDocument.sequence.multicam_groups.size() == 2,
              "and redo cuts them again");
    }

    // An angle wholly after the cut leaves the group for the second one,
    // which the group is rebuilt without, in its place.
    {
        Document cutDocument = Fixture();
        EditLog cutLog;
        AddMulticamGroupOperation group;
        Check(
            BuildAddMulticamGroup(cutDocument, "", {{kClose, ""}, {kTail, ""}},
                                  kTail, group, error, message) &&
                Apply(cutLog, cutDocument, group, "group close and tail"),
            "the group is made: " + message);
        BatchOperation batch;
        MulticamCut cut;
        Check(BuildSplitMulticamGroup(cutDocument, group.group.id, {50, 25}, {},
                                      batch, cut, error, message) &&
                  Apply(cutLog, cutDocument, batch, "cut before the tail"),
              "the cut applies: " + message);
        const auto& groups = cutDocument.sequence.multicam_groups;
        Check(groups.size() == 2 && groups[0].angles.size() == 1 &&
                  groups[0].angles[0].clip_id == kClose &&
                  groups[1].angles.size() == 2 &&
                  groups[1].angles[1].clip_id == kTail &&
                  groups[1].angles[1].id == group.group.angles[1].id &&
                  groups[1].active_angle_id == group.group.angles[1].id,
              "the tail moves to the second group with its angle id, still "
              "the active one; the group keeps the close-up's left piece");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All multicam tests passed\n";
    return 0;
}
