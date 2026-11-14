#include "EditLog.h"
#include "ProjectFixture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    // ADOBE-CLI-2026-09 -- compare the whole clone after reversing only its
    // identity map; otherwise a missing effect or relationship is invisible
    // to a test that merely counts clips.
    {
        Project variants = MakeInterviewShapedProject(2, 2, 2);
        auto& original = *variants.ActiveTimeline();
        const Ulid sourceId = original.id;
        auto& video = original.tracks[0];
        auto& audio = original.tracks[1];
        video.locked = true;
        audio.solo = true;
        video.clips[0].effects = {
            {GenerateUlid(), "color.exposure", {{"stops", {1, 3}}}}};
        video.clips[0].opacity = {2, 3};
        audio.clips[0].audio_gain_db = {-3, 2};
        audio.clips[0].audio_fade_in = {2, 25};
        for (size_t index = 0; index < video.clips.size(); ++index) {
            video.clips[index].sync_anchor_clip_id = video.clips[index].id;
            audio.clips[index].sync_anchor_clip_id = video.clips[index].id;
        }
        original.markers = {
            {GenerateUlid(), "Concert", {50, 25}, "#33AAFF", "Montage"}};
        original.transitions = {{GenerateUlid(),
                                 video.id,
                                 video.clips[0].id,
                                 video.clips[1].id,
                                 "cross_dissolve",
                                 {4, 25},
                                 TransitionAlignment::Center}};
        original.caption_styles = {
            {GenerateUlid(), "Arial", 44, "#ffffff", "bottom"}};
        MulticamGroup multicam;
        multicam.angles = {{GenerateUlid(), "A", video.clips[0].id},
                           {GenerateUlid(), "B", video.clips[1].id}};
        multicam.active_angle_id = multicam.angles[1].id;
        original.multicam_groups = {multicam};
        DocumentClip caption;
        caption.source_in = {0, 25};
        caption.timeline_in = {0, 25};
        caption.duration = {100, 25};
        caption.include_audio = false;
        caption.caption_group_id = original.caption_styles[0].id;
        caption.caption_text = "PREMIER CARTON";
        original.tracks.push_back({GenerateUlid(), "caption", 2, {caption}});
        variants.timeline_bin_ids[sourceId] = variants.bins[0].id;
        variants.bin_metadata[sourceId] = {"Concert master", 5, {"concert"}, 7};
        std::string error;
        Check(variants.Validate(error), "complete clone fixture: " + error);
        const std::string before = variants.SaveToString();
        const std::string originalDocument =
            variants.MakeDocument(sourceId).SaveToString();
        ProjectEditLog log;
        EditError code = EditError::None;
        CloneProjectTimelineOperation clone;
        clone.source_timeline_id = sourceId;
        clone.name = "Déclinaison exemple";
        clone.make_active = true;
        Check(log.Apply(variants, ProjectOperation{clone}, code, error),
              "clone the complete timeline: " + error);
        const auto applied = std::get<CloneProjectTimelineOperation>(
            log.AppliedEntries().back().op);
        Check(variants.timelines.size() == 3 && variants.Validate(error) &&
                  variants.active_timeline_id == applied.timeline_id &&
                  variants.timeline_bin_ids.at(applied.timeline_id) ==
                      variants.bins[0].id &&
                  variants.bin_metadata.at(applied.timeline_id).description ==
                      "Concert master",
              "clone has independent identities, focus and bin metadata");
        Check(
            variants.MakeDocument(sourceId).SaveToString() == originalDocument,
            "cloning leaves the original timeline byte-identical");
        Document normalized = variants.MakeDocument(applied.timeline_id);
        normalized.sequence.name = variants.FindTimeline(sourceId)->name;
        std::string normalizedJson = normalized.SaveToString();
        for (const auto& [oldId, newId] : applied.id_remap) {
            Check(oldId != newId && IsValidUlid(newId),
                  "every timeline-owned identity is replaced");
            size_t position = 0;
            while ((position = normalizedJson.find(newId, position)) !=
                   std::string::npos) {
                normalizedJson.replace(position, newId.size(), oldId);
                position += oldId.size();
            }
        }
        Check(normalizedJson == originalDocument,
              "clone preserves all exact values and remaps every relationship");
        const std::string after = variants.SaveToString();
        ProjectOperation decoded;
        const std::string serialized = SerializeProjectOperation(applied);
        Check(DeserializeProjectOperation(serialized, decoded, code, error) &&
                  SerializeProjectOperation(decoded) == serialized,
              "clone operation has a canonical serialization round-trip");
        Check(log.Undo(variants, code, error) &&
                  variants.SaveToString() == before,
              "clone undo restores the exact original project bytes");
        Check(
            log.Redo(variants, code, error) && variants.SaveToString() == after,
            "clone redo retains every generated ID and exact value");
        clone.timeline_id = sourceId;
        const std::string refusedOperation = SerializeProjectOperation(clone);
        Check(!log.Apply(variants, ProjectOperation{clone}, code, error) &&
                  code == EditError::DuplicateId &&
                  variants.SaveToString() == after &&
                  SerializeProjectOperation(clone) == refusedOperation,
              "colliding clone IDs are refused without mutating any state");
        clone.timeline_id.clear();
        clone.source_timeline_id = GenerateUlid();
        Check(!log.Apply(variants, ProjectOperation{clone}, code, error) &&
                  code == EditError::UnknownSequence &&
                  variants.SaveToString() == after,
              "unknown clone sources are refused atomically");
    }

    // ADOBE-CLI-2026-09 -- dates x formats remain one reversible intention,
    // including a refusal discovered only after earlier clones were prepared.
    {
        Project project = MakeInterviewShapedProject(4, 4, 2);
        for (auto& timeline : project.timelines) {
            DocumentClip caption;
            caption.source_in = {0, 25};
            caption.timeline_in = {0, 25};
            caption.duration = {100, 25};
            caption.include_audio = false;
            caption.caption_text = "GRAND CONCERT\n9 FÉVRIER 2031";
            timeline.tracks.push_back(
                {GenerateUlid(), "caption", 2, {caption}});
        }
        const std::string before = project.SaveToString();
        const std::vector<std::string> dates = {
            "3 MARS 2031",  "27 MAI 2031",    "8 JUILLET 2032",
            "19 AOÛT 2032", "2 OCTOBRE 2033", "14 NOVEMBRE 2033"};
        CreateProjectTimelineVariantsOperation batch;
        for (const auto& date : dates) {
            for (const auto& timeline : project.timelines) {
                ProjectTimelineVariant variant;
                variant.clone.source_timeline_id = timeline.id;
                variant.clone.name = date + "_" + timeline.name;
                variant.replacements = {{timeline.tracks.back().clips[0].id,
                                         "9 FÉVRIER 2031", date, 1}};
                batch.variants.push_back(variant);
            }
        }
        ProjectEditLog log;
        EditError code = EditError::None;
        std::string error;
        const std::string intention = SerializeProjectOperation(batch);
        ProjectOperation decoded;
        Check(DeserializeProjectOperation(intention, decoded, code, error) &&
                  SerializeProjectOperation(decoded) == intention,
              "variant intention has canonical serialization");
        Check(log.Apply(project, decoded, code, error) &&
                  log.AppliedCount() == 1 && project.timelines.size() == 28 &&
                  project.Validate(error),
              "24 dates x formats variants commit as one operation: " + error);
        const auto applied = std::get<CreateProjectTimelineVariantsOperation>(
            log.AppliedEntries().back().op);
        for (const auto& variant : applied.variants) {
            const auto* timeline =
                project.FindTimeline(variant.clone.timeline_id);
            Check((timeline != nullptr) &&
                      timeline->tracks.back().clips[0].caption_text ==
                          "GRAND CONCERT\n" + variant.replacements[0].to,
                  "only the date substring changes in every variant");
        }
        const std::string after = project.SaveToString();
        ProjectEditLog reloaded;
        Check(ProjectEditLog::Deserialize(log.Serialize(), reloaded, code,
                                          error) &&
                  reloaded.Serialize() == log.Serialize(),
              "variant journal round-trips canonically");
        Check(reloaded.Undo(project, code, error) &&
                  project.SaveToString() == before,
              "one undo restores the exact pre-batch bytes");
        Check(reloaded.Redo(project, code, error) &&
                  project.SaveToString() == after,
              "redo restores all variant identities and exact values");
        Check(reloaded.Undo(project, code, error), "reset batch fixture");
        batch.variants.back().replacements[0].expected_count = 2;
        Check(!log.Apply(project, batch, code, error) &&
                  code == EditError::ValidationFailed &&
                  project.SaveToString() == before &&
                  batch.variants[0].clone.id_remap.empty(),
              "late count mismatch refuses every variant without side effects");
        batch.variants.back().replacements[0].expected_count = 1;
        batch.variants.back().clone.name = batch.variants.front().clone.name;
        Check(!log.Apply(project, batch, code, error) &&
                  code == EditError::InvalidOperation &&
                  project.SaveToString() == before,
              "duplicate variant names refuse the entire batch");
    }

    Project project("Commercial");
    Check(project.timelines.size() == 1, "a project starts with one timeline");
    Check(project.ActiveTimeline() == &project.timelines.front(),
          "the initial timeline is active");

    const Ulid firstId = project.active_timeline_id;
    const std::string beforeAdd = project.SaveToString();
    ProjectEditLog projectLog;
    EditError editError = EditError::None;
    std::string error;
    Check(projectLog.Apply(project,
                           ProjectOperation{AddProjectTimelineOperation{
                               "Social cut", 1080, 1920, {30000, 1001}}},
                           editError, error),
          "add timeline operation: " + error);
    const auto& added = std::get<AddProjectTimelineOperation>(
        projectLog.AppliedEntries().back().op);
    const Ulid verticalId = added.timeline_id;
    const std::string afterAdd = project.SaveToString();
    Check(project.FindTimeline(verticalId)->name == "Social cut" &&
              project.FindTimeline(verticalId)->width == 1080,
          "timeline-specific settings are retained");
    Check(projectLog.Undo(project, editError, error), "undo add: " + error);
    Check(project.SaveToString() == beforeAdd,
          "project undo restores byte-identical JSON");
    Check(projectLog.Redo(project, editError, error), "redo add: " + error);
    Check(project.SaveToString() == afterAdd &&
              project.FindTimeline(verticalId) != nullptr,
          "project redo retains generated timeline IDs");
    // IA10 -- ROADMAP.md. Both ways of getting a timeline give the same
    // tracks, in number and in kind, so a first insert never has to create
    // one.
    const auto trackShape = [](const DocumentSequence& timeline) {
        std::vector<std::pair<std::string, int32_t>> shape;
        for (const DocumentTrack& track : timeline.tracks)
            shape.emplace_back(track.kind, track.index);
        return shape;
    };
    const std::vector<std::pair<std::string, int32_t>> expectedTracks = {
        {"video", 0}, {"audio", 1}};
    Check(trackShape(*project.FindTimeline(firstId)) == expectedTracks &&
              trackShape(*project.FindTimeline(verticalId)) == expectedTracks,
          "the initial timeline has the tracks an added timeline has");
    Check(project.FindTimeline(firstId)->tracks[0].id !=
                  project.FindTimeline(firstId)->tracks[1].id &&
              Project("Other").timelines.front().tracks[0].id !=
                  project.FindTimeline(firstId)->tracks[0].id,
          "initial track IDs are generated per project");

    ProjectOperation decoded;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decoded, editError, error),
          "project operation round-trip: " + error);
    Check(std::get<AddProjectTimelineOperation>(decoded).timeline_id ==
              verticalId,
          "serialized project operation retains enriched IDs");

    // QC-2026-08 -- switching the active timeline is an operation like any
    // other, so an agent can navigate a project instead of being stuck on
    // whatever the app last left active.
    Check(project.active_timeline_id == firstId,
          "adding a timeline does not steal focus from the active one");
    const std::string beforeActivate = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{SetActiveProjectTimelineOperation{verticalId}},
              editError, error),
          "activate timeline operation: " + error);
    Check(project.active_timeline_id == verticalId &&
              project.ActiveTimeline() == project.FindTimeline(verticalId),
          "the named timeline becomes active");
    const std::string afterActivate = project.SaveToString();
    Check(projectLog.Undo(project, editError, error),
          "undo activate: " + error);
    Check(project.SaveToString() == beforeActivate &&
              project.active_timeline_id == firstId,
          "undoing an activation restores byte-identical JSON");
    Check(projectLog.Redo(project, editError, error),
          "redo activate: " + error);
    Check(project.SaveToString() == afterActivate,
          "redoing an activation is byte-identical too");
    ProjectOperation decodedActivate;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decodedActivate, editError, error) &&
              std::get<SetActiveProjectTimelineOperation>(decodedActivate)
                      .timeline_id == verticalId,
          "activation round-trips through canonical JSON: " + error);
    const std::string beforeUnknown = project.SaveToString();
    Check(!projectLog.Apply(project,
                            ProjectOperation{SetActiveProjectTimelineOperation{
                                "01K30000000000000000000099"}},
                            editError, error) &&
              editError == EditError::UnknownSequence,
          "activating a timeline the project does not hold is refused by name");
    Check(project.SaveToString() == beforeUnknown,
          "and the refusal leaves the project untouched");
    Check(projectLog.Apply(
              project,
              ProjectOperation{SetActiveProjectTimelineOperation{firstId}},
              editError, error),
          "restore the original active timeline: " + error);

    const std::string beforeSessionSelection = project.SaveToString();
    Document edit = project.MakeDocument(verticalId);
    Check(project.SaveToString() == beforeSessionSelection,
          "selecting a timeline snapshot is session state, not a mutation");

    edit.sequence.name = "Social cut v2";
    edit.color_management.enabled = true;
    Check(project.CommitDocument(verticalId, edit, error), "commit: " + error);
    Check(project.FindTimeline(verticalId)->name == "Social cut v2" &&
              project.settings.color_management.enabled,
          "the addressed edit snapshot commits into its project");

    const std::string beforeRemove = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{RemoveProjectTimelineOperation{verticalId}},
              editError, error),
          "remove timeline operation: " + error);
    ProjectOperation decodedRemove;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              decodedRemove, editError, error),
          "remove operation round-trip: " + error);
    Check(project.active_timeline_id == firstId,
          "removing a non-default timeline preserves the project default");
    Check(projectLog.Undo(project, editError, error), "undo remove: " + error);
    Check(project.SaveToString() == beforeRemove,
          "undo remove restores the exact timeline");
    Check(projectLog.Redo(project, editError, error), "redo remove: " + error);
    Check(
        !projectLog.Apply(
            project, ProjectOperation{RemoveProjectTimelineOperation{firstId}},
            editError, error),
        "the final timeline cannot be removed");
    ProjectOperation invalidAdd =
        AddProjectTimelineOperation{"Invalid", 0, 1080, {25, 1}};
    const std::string invalidAddBefore = SerializeProjectOperation(invalidAdd);
    ProjectOperation ignoredInverse = AddProjectTimelineOperation{};
    Check(!ApplyProjectOperation(project, invalidAdd, ignoredInverse, editError,
                                 error),
          "invalid project operation is rejected");
    Check(SerializeProjectOperation(invalidAdd) == invalidAddBefore,
          "a rejected project operation is left byte-identical");
    Check(project.Validate(error), "valid project: " + error);

    Document wrongTimeline = project.MakeActiveDocument();
    wrongTimeline.sequence.id = GenerateUlid();
    const std::string unchangedName = project.ActiveTimeline()->name;
    Check(!project.CommitActiveDocument(wrongTimeline, error),
          "a snapshot from another timeline must be rejected");
    Check(project.ActiveTimeline()->name == unchangedName,
          "a rejected commit leaves the project unchanged");

    Project stored("Serialized project");
    stored.rushes = {{GenerateUlid(),
                      "rush.mov",
                      "rush.mov",
                      "h264",
                      true,
                      1920,
                      1080,
                      "yuv420p",
                      "tv",
                      "bt709",
                      "bt709",
                      "bt709",
                      // white_balance_kelvin (G7): unknown.
                      0,
                      // color_input (G3): the project's.
                      {},
                      0,
                      {25, 1},
                      {250, 25},
                      "landscape",
                      true,
                      48000,
                      2,
                      // audio_level_measured / audio_level (QC-2026-09 A3):
                      // this fixture is built by hand, never ingested.
                      false,
                      0,
                      "",
                      "",
                      true}};
    DocumentSource source;
    source.id = stored.rushes.front().id;
    source.path = "rush.mov";
    source.rate = {25, 1};
    source.duration = {250, 25};
    stored.sources = {source};
    stored.ActiveTimeline()->tracks = {
        {GenerateUlid(),
         "video",
         0,
         {{GenerateUlid(), source.id, {0, 25}, {25, 25}, {0, 25}}}}};
    ProjectEditLog storedLog;
    Check(storedLog.Apply(stored,
                          ProjectOperation{AddProjectTimelineOperation{
                              "Vertical", 1080, 1920, {25, 1}}},
                          editError, error),
          "add serialized timeline: " + error);
    const Ulid second = std::get<AddProjectTimelineOperation>(
                            storedLog.AppliedEntries().back().op)
                            .timeline_id;
    stored.timelines.back().tracks = {
        {GenerateUlid(),
         "video",
         0,
         {{GenerateUlid(), source.id, {25, 25}, {25, 25}, {0, 25}}}}};
    stored.active_timeline_id = second;
    stored.bin_metadata[source.id] = {"Hero shot", 5, {"select", "day"}, 7};
    Check(stored.Validate(error), "serializable project: " + error);

    Project shortProject = stored;
    ProjectEditLog shortLog;
    const std::string beforeShort = shortProject.SaveToString();
    CreateProjectTimelineFromSegmentsOperation shortOperation;
    shortOperation.name = "Short dynamique";
    shortOperation.width = stored.ActiveTimeline()->width;
    shortOperation.height = stored.ActiveTimeline()->height;
    shortOperation.frame_rate = stored.ActiveTimeline()->frame_rate;
    shortOperation.segments = {
        {source.id, {50, 25}, {25, 25}},
        {source.id, {0, 25}, {50, 25}},
    };
    Check(shortLog.Apply(shortProject, ProjectOperation{shortOperation},
                         editError, error),
          "create short timeline: " + error);
    const auto& appliedShort =
        std::get<CreateProjectTimelineFromSegmentsOperation>(
            shortLog.AppliedEntries().back().op);
    const std::string afterShort = shortProject.SaveToString();
    Check(shortProject.active_timeline_id == appliedShort.timeline_id &&
              shortProject.ActiveTimeline()->tracks.size() == 2 &&
              shortProject.ActiveTimeline()->tracks[0].clips.size() == 2 &&
              shortProject.ActiveTimeline()->tracks[1].clips.size() == 2 &&
              shortProject.ActiveTimeline()->tracks[0].clips[1].timeline_in ==
                  RationalTime{25, 25},
          "short assembly creates linked A/V clips in the requested order");
    ProjectOperation decodedShort;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(shortLog.AppliedEntries().back().op),
              decodedShort, editError, error),
          "short operation round-trip: " + error);
    Check(std::get<CreateProjectTimelineFromSegmentsOperation>(decodedShort)
                  .segments[0]
                  .video_clip_id == appliedShort.segments[0].video_clip_id,
          "short operation retains generated clip IDs");
    Check(shortLog.Undo(shortProject, editError, error),
          "undo short timeline: " + error);
    Check(shortProject.SaveToString() == beforeShort,
          "undo short timeline restores byte-identical project JSON");
    Check(shortLog.Redo(shortProject, editError, error),
          "redo short timeline: " + error);
    Check(shortProject.SaveToString() == afterShort,
          "redo short timeline restores byte-identical project JSON");

    // B10 -- one ordered project batch is one exact history step. The rename
    // deliberately depends on the preceding add, so reversing execution order
    // would fail instead of merely producing a different cosmetic result.
    Project batchProject("Batch project");
    const std::string beforeBatch = batchProject.SaveToString();
    const Ulid batchTimelineId = GenerateUlid();
    ProjectEditLog batchLog;
    Check(batchLog.ApplyBatch(
              batchProject,
              {ProjectOperation{AddProjectTimelineOperation{"Added first",
                                                            1920,
                                                            1080,
                                                            {25, 1},
                                                            batchTimelineId,
                                                            GenerateUlid(),
                                                            GenerateUlid()}},
               ProjectOperation{
                   RenameProjectItemOperation{batchTimelineId, "Renamed"}}},
              editError, error),
          "ordered project batch applies: " + error);
    const std::string afterBatch = batchProject.SaveToString();
    Check(batchProject.FindTimeline(batchTimelineId) != nullptr &&
              batchProject.FindTimeline(batchTimelineId)->name == "Renamed",
          "project batch applies operations in input order");
    Check(batchLog.AppliedCount() == 1,
          "project batch creates exactly one history entry");
    ProjectEditLog decodedBatchLog;
    Check(ProjectEditLog::Deserialize(batchLog.Serialize(), decodedBatchLog,
                                      editError, error) &&
              decodedBatchLog.AppliedCount() == 1,
          "project batch history round-trips canonically: " + error);
    Check(batchLog.Undo(batchProject, editError, error) &&
              batchProject.SaveToString() == beforeBatch,
          "one undo restores the bytes before a project batch: " + error);
    Check(batchLog.Redo(batchProject, editError, error) &&
              batchProject.SaveToString() == afterBatch,
          "one redo restores the bytes after a project batch: " + error);

    const std::string beforeRejectedBatch = batchProject.SaveToString();
    const std::string logBeforeRejectedBatch = batchLog.Serialize();
    Check(!batchLog.ApplyBatch(batchProject,
                               {ProjectOperation{RenameProjectItemOperation{
                                    batchTimelineId, "Must roll back"}},
                                ProjectOperation{RemoveProjectTimelineOperation{
                                    "01K39999999999999999999999"}}},
                               editError, error) &&
              editError == EditError::UnknownSequence,
          "a refusal makes the whole project batch fail");
    Check(batchProject.SaveToString() == beforeRejectedBatch &&
              batchLog.Serialize() == logBeforeRejectedBatch,
          "a refused project batch leaves project and history byte-identical");

    const std::string serialized = stored.SaveToString();
    Project loaded("placeholder");
    Check(Project::LoadFromString(serialized, loaded, error),
          "load serialized project: " + error);
    Check(loaded.SaveToString() == serialized,
          "project JSON round-trips byte-identically");
    Check(loaded.timelines.size() == 2 && loaded.active_timeline_id == second &&
              loaded.ActiveTimeline()->name == "Vertical",
          "all timelines and the active identity round-trip");
    Check(loaded.FindBinMetadata(source.id) &&
              loaded.FindBinMetadata(source.id)->rating == 5 &&
              loaded.FindBinMetadata(source.id)->tags.size() == 2,
          "project-bin metadata round-trips");

    Project noPromotion("placeholder");
    Check(!Project::LoadFromString(stored.MakeActiveDocument().SaveToString(),
                                   noPromotion, error),
          "standalone timeline documents are not promoted to projects");

    Project unchanged("unchanged");
    const Ulid unchangedId = unchanged.id;
    Check(!Project::LoadFromString("{\"project_format\":\"cutmachine-project\","
                                   "\"project_version\":99}",
                                   unchanged, error),
          "unsupported project versions are rejected");
    Check(unchanged.id == unchangedId,
          "failed project parsing leaves output unchanged");
    std::cout << "project tests passed\n";
    return 0;
}
