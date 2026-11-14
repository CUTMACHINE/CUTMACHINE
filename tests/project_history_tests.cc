// S6 -- SAVING_ROADMAP.md. Project history records what an operation
// changed, never whole projects.
//
// Every project operation (and a B10 batch) goes through the round trip
// PHILOSOPHY.md requires -- apply, undo, redo, each compared byte for byte,
// plus canonical serialization -- on a project the size of a real interview
// package, with a bound on each journal entry that a whole-project snapshot
// would exceed a hundred times over. A journal written before S6 must still
// replay, and shrink the moment it is read.

#include "EditLog.h"
#include "ProjectDelta.h"
#include "ProjectFixture.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

const ExactProjectState* Exact(const ProjectOperation& operation) {
    return std::visit(
        [](const auto& value) -> const ExactProjectState* {
            return value.exact_project_result ? &*value.exact_project_result
                                              : nullptr;
        },
        operation);
}

bool IsDelta(const ProjectOperation& operation) {
    const ExactProjectState* exact = Exact(operation);
    return exact && exact->delta && exact->canonical_json.empty();
}

size_t EntryBytes(const ProjectEntry& entry) {
    return SerializeProjectOperation(entry.op).size() +
           SerializeProjectOperation(entry.inverse).size();
}

void CheckCanonical(const ProjectOperation& operation,
                    const std::string& label) {
    ProjectOperation decoded = AddProjectTimelineOperation{};
    EditError error = EditError::None;
    std::string message;
    const std::string serialized = SerializeProjectOperation(operation);
    Check(DeserializeProjectOperation(serialized, decoded, error, message),
          label + " reads back: " + message);
    Check(SerializeProjectOperation(decoded) == serialized,
          label + " serializes canonically");
}

void CheckJournaled(const ProjectEntry& entry, const std::string& label,
                    size_t maximumEntryBytes) {
    Check(IsDelta(entry.op) && IsDelta(entry.inverse),
          label + " is journaled as a delta, not as whole projects");
    Check(EntryBytes(entry) <= maximumEntryBytes,
          label + " entry stays within " + std::to_string(maximumEntryBytes) +
              " bytes (" + std::to_string(EntryBytes(entry)) + ")");
    CheckCanonical(entry.op, label + " operation");
    CheckCanonical(entry.inverse, label + " inverse");
}

void CheckUndoRedo(Project& project, ProjectEditLog& log,
                   const std::string& before, const std::string& after,
                   const std::string& label) {
    EditError error = EditError::None;
    std::string message;
    Check(log.Undo(project, error, message), label + " undoes: " + message);
    Check(project.SaveToString() == before,
          label + " undo restores byte-identical JSON");
    Check(log.Redo(project, error, message), label + " redoes: " + message);
    Check(project.SaveToString() == after,
          label + " redo restores byte-identical JSON");

    ProjectEditLog reloaded;
    Check(
        ProjectEditLog::Deserialize(log.Serialize(), reloaded, error, message),
        label + " journal reloads: " + message);
    Check(reloaded.Serialize() == log.Serialize(),
          label + " journal serialization is canonical");
    Project replay = project;
    Check(reloaded.Undo(replay, error, message) &&
              replay.SaveToString() == before,
          label + " reloaded journal undoes exactly: " + message);
}

void RoundTrip(Project& project, ProjectEditLog& log,
               ProjectOperation operation, const std::string& label,
               size_t maximumEntryBytes) {
    const std::string before = project.SaveToString();
    EditError error = EditError::None;
    std::string message;
    Check(log.Apply(project, std::move(operation), error, message),
          label + " applies: " + message);
    const std::string after = project.SaveToString();
    Check(after != before, label + " changes the project");
    CheckJournaled(log.AppliedEntries().back(), label, maximumEntryBytes);
    CheckUndoRedo(project, log, before, after, label);
}

}  // namespace

int main() {
    EditError error = EditError::None;
    std::string message;

    Project project = MakeInterviewShapedProject();
    Check(project.Validate(message), "fixture is a valid project: " + message);
    const size_t projectBytes = project.SaveToString().size();
    Check(projectBytes > 300000,
          "the fixture has the size of a real interview package");

    ProjectEditLog log;
    const Ulid first = project.timelines[0].id;
    const Ulid second = project.timelines[1].id;
    const Ulid rush = project.rushes[7].id;

    RoundTrip(project, log,
              RenameProjectItemOperation{second, "Version courte"},
              "timeline rename", 2048);
    RoundTrip(project, log,
              RenameProjectItemOperation{rush, "Interview principale"},
              "rush display name", 2048);
    RoundTrip(project, log,
              RenameProjectItemOperation{rush, "Crépuscule — \"é\" 🙂"},
              "non-ASCII display name", 2048);

    ProjectBinMetadata metadata;
    metadata.description = "ligne 1\nligne 2\t\\fin";
    metadata.rating = 4;
    metadata.tags = {"itw", "\"plan large\"", "é"};
    metadata.insert_order = 3;
    RoundTrip(project, log, SetProjectBinMetadataOperation{rush, metadata},
              "bin metadata with escapes", 2048);
    metadata.rating = 5;
    RoundTrip(project, log, SetProjectBinMetadataOperation{rush, metadata},
              "bin metadata update", 2048);

    RoundTrip(project, log,
              SetProjectTimelineBinOperation{second, project.bins[1].id},
              "timeline placed in a bin", 2048);
    RoundTrip(project, log, SetProjectTimelineBinOperation{second, ""},
              "timeline placement cleared", 2048);
    RoundTrip(project, log, SetActiveProjectTimelineOperation{second},
              "active timeline", 2048);
    RoundTrip(project, log,
              AddProjectTimelineOperation{"Vertical", 1080, 1920, {25, 1}},
              "timeline added", 4096);

    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. A choice between timelines,
    // with the reason on its entry: activating the one kept is the whole of
    // what it changes, and undoing it restores the bytes and keeps the
    // motif on the undone entry.
    {
        EditMotif why;
        why.reason = MotifReason::Rhythm;
        why.author = MotifAuthor::Person;
        why.note = "la plus serrée tient la chute";
        const std::string before = project.SaveToString();
        Check(log.Apply(project,
                        ChooseProjectTimelineOperation{first, {second}, true},
                        error, message, ProjectEntryMotifsFor(why)),
              "a choice applies: " + message);
        const std::string after = project.SaveToString();
        Check(after != before && project.active_timeline_id == first,
              "the choice makes the kept timeline active");
        CheckJournaled(log.AppliedEntries().back(), "choice", 2048);
        Check(log.AppliedEntries().back().motifs.size() == 1 &&
                  log.AppliedEntries().back().motifs[0].motif.note == why.note,
              "the choice's reason rides on its entry");
        CheckUndoRedo(project, log, before, after, "choice");
        Check(log.AppliedEntries().back().motifs.size() == 1,
              "undo and redo carry the reason with the entry");

        // Without activation the project's bytes do not move: the choice is
        // trace, and the journal alone records it.
        const std::string unchanged = project.SaveToString();
        Check(log.Apply(project,
                        ChooseProjectTimelineOperation{second, {first}, false},
                        error, message, ProjectEntryMotifsFor(why)) &&
                  project.SaveToString() == unchanged,
              "a choice without activation leaves the project's bytes: " +
                  message);
        CheckJournaled(log.AppliedEntries().back(), "inactive choice", 2048);
        CheckUndoRedo(project, log, unchanged, unchanged, "inactive choice");

        const size_t entries = log.AppliedCount();
        for (const auto& [refused, label] : std::vector<
                 std::pair<ChooseProjectTimelineOperation, std::string>>{
                 {{first, {}, false}, "no alternative"},
                 {{first, {first}, false}, "the kept timeline among them"},
                 {{first, {second, second}, false}, "an alternative twice"},
                 {{first, {"01K3ZZZZZZZZZZZZZZZZZZZZZZ"}, false},
                  "an unknown alternative"},
                 {{"01K3ZZZZZZZZZZZZZZZZZZZZZZ", {first}, false},
                  "an unknown timeline kept"}}) {
            Check(!log.Apply(project, refused, error, message) &&
                      project.SaveToString() == unchanged &&
                      log.AppliedCount() == entries,
                  "a choice with " + label +
                      " is refused and changes "
                      "nothing");
        }
        EditMotif twice = why;
        Check(!log.Apply(project,
                         ChooseProjectTimelineOperation{first, {second}, false},
                         error, message, {{0, 2, twice}}) &&
                  log.AppliedCount() == entries,
              "a project entry is one member: a wider span is refused");
    }

    CreateProjectTimelineFromSegmentsOperation shortcut;
    shortcut.name = "Short";
    shortcut.width = 1080;
    shortcut.height = 1920;
    shortcut.segments = {
        {project.sources[3].id, {25, 25}, {50, 25}, "", "", ""},
        {project.sources[9].id, {0, 25}, {75, 25}, "", "", ""}};
    RoundTrip(project, log, shortcut, "short created from segments", 16384);

    RoundTrip(project, log,
              SetProjectTimelineBinOperation{second, project.bins[2].id},
              "placement before removal", 2048);
    RoundTrip(project, log, SetProjectBinMetadataOperation{second, metadata},
              "metadata before removal", 2048);
    RoundTrip(project, log, RemoveProjectTimelineOperation{second},
              "timeline removed with its placement and metadata", 65536);

    LibraryMedia replacement = project.rushes[12];
    replacement.path = "1_RUSHES/relinked/C9012.MP4";
    replacement.filename = "C9012.MP4";
    replacement.duration = replacement.duration.add({25, 25});
    RelinkProjectMediaOperation relink;
    relink.replacements.push_back(
        {replacement.id, replacement, replacement.path});
    RoundTrip(project, log, relink, "media relinked", 8192);

    // main.mm's timeline-from-selection path hands the engine the whole
    // resulting project: it is recorded as the delta it amounts to.
    Project assembled = project;
    assembled.timelines[0].name = "Assemblée";
    RenameProjectItemOperation carried{first, "Assemblée"};
    carried.exact_project_result =
        ExactProjectState{assembled.SaveToString(), std::nullopt};
    RoundTrip(project, log, carried, "operation carrying a whole state", 2048);

    {
        const std::string before = project.SaveToString();
        Check(log.ApplyBatch(
                  project,
                  {RenameProjectItemOperation{first, "Master"},
                   SetActiveProjectTimelineOperation{first},
                   RemoveProjectTimelineOperation{project.timelines.back().id}},
                  error, message),
              "B10 batch applies: " + message);
        CheckJournaled(log.AppliedEntries().back(), "B10 batch", 65536);
        CheckUndoRedo(project, log, before, project.SaveToString(),
                      "B10 batch");
    }

    Check(log.Serialize().size() < projectBytes,
          "the whole journal (" + std::to_string(log.AppliedCount()) +
              " entries, " + std::to_string(log.Serialize().size()) +
              " bytes) is smaller than one copy of the project (" +
              std::to_string(projectBytes) + " bytes)");

    // A journal written before S6: each entry carries two whole projects,
    // hex-encoded. It still replays exactly, and shrinks once read.
    {
        Project legacyProject = MakeInterviewShapedProject(40, 3, 6);
        const std::string beforeJson = legacyProject.SaveToString();
        Project renamed = legacyProject;
        const Ulid timelineId = renamed.timelines[1].id;
        renamed.timelines[1].name = "Renommée";
        const std::string afterJson = renamed.SaveToString();
        RenameProjectItemOperation operation{timelineId, "Renommée"};
        operation.exact_project_result =
            ExactProjectState{afterJson, std::nullopt};
        RenameProjectItemOperation inverse{timelineId, "Renommée"};
        inverse.exact_project_result =
            ExactProjectState{beforeJson, std::nullopt};
        const std::string legacyJournal =
            "{\"version\":1,\"kind\":\"project\",\"applied\":[{\"op\":" +
            SerializeProjectOperation(operation) +
            ",\"inverse\":" + SerializeProjectOperation(inverse) +
            "}],\"undone\":[]}\n";
        Check(
            legacyJournal.find("\"exact_project_hex\":\"") != std::string::npos,
            "the fixture journal is in the pre-S6 form");

        ProjectEditLog legacy;
        Check(
            ProjectEditLog::Deserialize(legacyJournal, legacy, error, message),
            "a pre-S6 journal still reads: " + message);
        Check(IsDelta(legacy.AppliedEntries().back().op) &&
                  IsDelta(legacy.AppliedEntries().back().inverse),
              "reading converts its entry into a delta");
        Check(legacy.Serialize().size() * 20 < legacyJournal.size(),
              "the journal written back is a fraction of the one read (" +
                  std::to_string(legacy.Serialize().size()) + " vs " +
                  std::to_string(legacyJournal.size()) + " bytes)");
        Check(legacy.Undo(renamed, error, message) &&
                  renamed.SaveToString() == beforeJson,
              "a converted entry undoes exactly: " + message);
        Check(legacy.Redo(renamed, error, message) &&
                  renamed.SaveToString() == afterJson,
              "a converted entry redoes exactly: " + message);
    }

    // Undoing a project operation touches what that operation changed and
    // nothing else. A whole-state undo used to discard this marker.
    {
        Project edited = MakeInterviewShapedProject(20, 2, 4);
        ProjectEditLog history;
        const Ulid timelineId = edited.timelines[1].id;
        Check(history.Apply(edited,
                            RenameProjectItemOperation{timelineId, "Rough cut"},
                            error, message),
              "rename before a timeline edit: " + message);
        Document document = edited.MakeDocument(timelineId);
        DocumentMarker marker;
        marker.name = "Garder";
        marker.time = {50, 25};
        document.sequence.markers.push_back(marker);
        Check(edited.CommitDocument(timelineId, document, message),
              "a timeline edit commits: " + message);
        Check(history.Undo(edited, error, message),
              "project undo after a timeline edit: " + message);
        const DocumentSequence* timeline = edited.FindTimeline(timelineId);
        Check(timeline && timeline->name == "Timeline 2",
              "project undo restores the name");
        Check(timeline && timeline->markers.size() == 1 &&
                  timeline->markers.front().name == "Garder",
              "and keeps the edit made to that timeline since");
    }

    // A replay that no longer matches is refused and changes nothing.
    {
        Project stale = MakeInterviewShapedProject(5, 2, 2);
        ProjectEditLog history;
        Check(history.Apply(
                  stale,
                  AddProjectTimelineOperation{"Extra", 1920, 1080, {25, 1}},
                  error, message),
              "timeline added for the stale replay: " + message);
        ProjectOperation removal = history.AppliedEntries().back().inverse;
        Check(history.Undo(stale, error, message),
              "timeline removed by undo: " + message);
        const std::string afterUndo = stale.SaveToString();
        ProjectOperation ignored = AddProjectTimelineOperation{};
        Check(!ApplyProjectOperation(stale, removal, ignored, error, message) &&
                  error == EditError::ValidationFailed,
              "replaying the removal of a timeline already gone is refused");
        Check(stale.SaveToString() == afterUndo,
              "and leaves the project untouched");
    }

    // A delta cannot express a reordering, and says so instead of recording
    // something that would replay into another project.
    {
        const Project ordered = MakeInterviewShapedProject(5, 3, 2);
        Project reordered = ordered;
        std::swap(reordered.timelines[0], reordered.timelines[2]);
        ProjectDelta delta;
        Check(!ComputeProjectDelta(ordered, reordered, delta, message),
              "a reordering is refused rather than misrecorded");
    }

    // The delta format is strict: every field, and nothing else.
    {
        ProjectDelta delta;
        const std::string empty = SerializeProjectDelta(ProjectDelta{});
        Check(DeserializeProjectDelta(empty, delta, message),
              "the empty delta reads: " + message);
        Check(!DeserializeProjectDelta("{\"name\":null}", delta, message),
              "a delta with missing fields is refused");
        std::string extra = empty;
        extra.insert(1, "\"unexpected\":1,");
        Check(!DeserializeProjectDelta(extra, delta, message),
              "a delta with an unknown field is refused");
    }

    std::cout << "All project history tests passed\n";
    return 0;
}
