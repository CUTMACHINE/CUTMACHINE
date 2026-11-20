// MOTIF1, MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. Why an edit was made,
// in the journal: a journal without motifs keeps the bytes every build
// before MOTIF1 wrote and read; one with motifs reads back to its own bytes
// (both pinned by tests/fixtures/edit_log/); undo and redo move a motif with
// its entry and leave the document's bytes alone; a fold keeps each entry's
// motifs on its members; list_motifs groups what the journal says. Portable.
//
// MOTIF6 -- the same for the project's journal: a timeline created, cloned
// or declined, and the choice between timelines (DEC41), with their
// reasons; a project journal written by main before MOTIF6 reads back to its
// bytes.
//
// `--print` writes the fixtures this build produces, to paste after a
// change that was meant.

#include "Document.h"
#include "EditLog.h"
#include "EditMotif.h"
#include "Json.h"
#include "Operations.h"
#include "Project.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

#ifndef EDIT_LOG_FIXTURES_DIR
#error "EDIT_LOG_FIXTURES_DIR must point at tests/fixtures/edit_log"
#endif

namespace {

const Ulid kSource = "01K30000000000000000000001";
const Ulid kVideo = "01K30000000000000000000002";
const Ulid kFirst = "01K30000000000000000000003";
const Ulid kSecond = "01K30000000000000000000004";

Document Fixture() {
    Document document;
    document.sequence.id = "01K3S000000000000000000001";
    document.sources = {{kSource, "folder/A.MP4", {25, 1}, {1000, 25}}};
    document.sequence.tracks = {
        {kVideo,
         "video",
         0,
         {{kFirst, kSource, {100, 25}, {10, 25}, {0, 25}},
          {kSecond, kSource, {200, 25}, {10, 25}, {10, 25}}}}};
    return document;
}

AddMarkerOperation Marker(const std::string& id, const std::string& name,
                          int64_t frame) {
    DocumentMarker marker;
    marker.id = id;
    marker.name = name;
    marker.time = {frame, 25};
    return AddMarkerOperation{marker};
}

mcp_json::Value Parse(const std::string& json) {
    mcp_json::Value value;
    std::string error;
    mcp_json::Value::Parse(json, value, error);
    return value;
}

EditMotif PauseMotif(int64_t threshold) {
    EditMotif motif;
    motif.reason = MotifReason::Pause;
    motif.rule = "tighten_pauses";
    motif.params = Parse("{\"min_gap_ms\":" + std::to_string(threshold) +
                         ",\"keep_frames\":6}");
    return motif;
}

std::string ReadFixture(const std::string& name) {
    std::ifstream input(std::string(EDIT_LOG_FIXTURES_DIR) + "/" + name,
                        std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

// A journal as every build before MOTIF1 wrote one: two entries applied,
// one undone.
EditLog JournalWithoutMotifs(Document& document) {
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    Check(log.Apply(document,
                    Marker("01K3M000000000000000000001", "Respiration", 3),
                    error, message) &&
              log.Apply(document,
                        Marker("01K3M000000000000000000002", "Coupe", 7), error,
                        message) &&
              log.Undo(document, error, message),
          "the journal without motifs is built: " + message);
    return log;
}

// The same shape, with what MOTIF1 adds: a motif on an operation, motifs
// on two members of a batch, one member left without.
EditLog JournalWithMotifs(Document& document) {
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    EditMotif brief;
    brief.reason = MotifReason::Brief;
    brief.author = MotifAuthor::Person;
    brief.note = "le brief dit « 20 s », pas plus";
    brief.marker_id = "01K3M000000000000000000009";
    const Operation single =
        Marker("01K3M000000000000000000001", "Respiration", 3);
    const Operation batch =
        BatchOperation{"Trois repères",
                       {Marker("01K3M000000000000000000002", "A", 5),
                        Marker("01K3M000000000000000000003", "B", 6),
                        Marker("01K3M000000000000000000004", "C", 8)}};
    EditMotif rhythm;
    rhythm.reason = MotifReason::Rhythm;
    rhythm.author = MotifAuthor::Procedure;
    Check(log.Apply(document, single, error, message,
                    EntryMotifsFor(single, {}, brief)) &&
              log.Apply(document, batch, error, message,
                        {{0, 1, PauseMotif(400)}, {2, 1, rhythm}}) &&
              log.Apply(document, Marker("01K3M000000000000000000005", "D", 9),
                        error, message) &&
              log.Undo(document, error, message),
          "the journal with motifs is built: " + message);
    return log;
}

void CheckFixtures(bool print) {
    Document plain = Fixture();
    const std::string without = JournalWithoutMotifs(plain).Serialize();
    Document motivated = Fixture();
    const std::string with = JournalWithMotifs(motivated).Serialize();
    if (print) {
        std::cout << "--- without_motifs.json\n"
                  << without << "--- with_motifs.json\n"
                  << with;
        return;
    }
    const std::string oldJournal = ReadFixture("without_motifs.json");
    Check(without == oldJournal,
          "a journal without motifs is written as before MOTIF1, version 1; "
          "if the change is meant, without_motifs.json:\n" +
              without);
    Check(with == ReadFixture("with_motifs.json"),
          "a journal with motifs is written as pinned; if the change is "
          "meant, with_motifs.json:\n" +
              with);

    // Each fixture reads back to its own bytes.
    for (const std::string& name :
         {std::string("without_motifs.json"), std::string("with_motifs.json"),
          std::string("before_motif1.json")}) {
        const std::string text = ReadFixture(name);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!text.empty() && EditLog::Deserialize(text, log, error, message),
              name + " reads: " + message);
        Check(log.Serialize() == text,
              name + " reads back to its own bytes:\n" + log.Serialize());
    }
    // A journal written by main before MOTIF1 (b49b9db, `--call-tool`: two
    // markers added, the second undone): read, nothing gained, nothing lost.
    {
        EditLog old;
        EditError error = EditError::None;
        std::string message;
        EditLog::Deserialize(ReadFixture("before_motif1.json"), old, error,
                             message);
        bool none = old.AppliedCount() > 0;
        for (const Entry& entry : old.AppliedEntries()) {
            none = none && entry.motifs.empty();
        }
        Check(none, "an old journal has entries and no motif");
        const std::string listed = ListMotifsJson("t", old, {}, "");
        Check(listed.find("\"groups\":[]") != std::string::npos &&
                  listed.find("\"operations_without_motif\":" +
                              std::to_string(old.AppliedCount())) !=
                      std::string::npos,
              "list_motifs on an old journal: no group, every operation "
              "without motif: " +
                  listed);
    }
    // What the motifs carry, read back.
    EditLog read;
    EditError error = EditError::None;
    std::string message;
    Check(EditLog::Deserialize(with, read, error, message) &&
              read.AppliedCount() == 2 && read.UndoneCount() == 1 &&
              read.AppliedEntries()[0].motifs.size() == 1 &&
              read.AppliedEntries()[0].motifs[0].motif.note ==
                  "le brief dit « 20 s », pas plus" &&
              read.AppliedEntries()[0].motifs[0].motif.author ==
                  MotifAuthor::Person &&
              read.AppliedEntries()[1].motifs.size() == 2 &&
              read.AppliedEntries()[1].motifs[1].first == 2 &&
              read.AppliedEntries()[1].motifs[0].motif.params.Dump() ==
                  "{\"min_gap_ms\":400,\"keep_frames\":6}" &&
              read.UndoneEntries()[0].motifs.empty(),
          "the motifs read back whole: " + message);
}

// MOTIF6 -- a project with one timeline, every ID pinned, so that the
// project journals built on it are bytes a fixture can hold.
const Ulid kMain = "01K3S000000000000000000001";
const Ulid kVertical = "01K3T000000000000000000001";
const Ulid kSquare = "01K3T000000000000000000004";

Project ProjectFixture() {
    Project project = Project::FromDocument(Fixture(), "Choix");
    project.id = "01K3P000000000000000000001";
    return project;
}

AddProjectTimelineOperation Timeline(const Ulid& id, const std::string& name,
                                     int32_t width, int32_t height) {
    AddProjectTimelineOperation add;
    add.name = name;
    add.width = width;
    add.height = height;
    add.timeline_id = id;
    // The tracks follow the timeline's ID: ...1 gives ...2 and ...3.
    add.video_track_id = id.substr(0, 25) + char(id.back() + 1);
    add.audio_track_id = id.substr(0, 25) + char(id.back() + 2);
    return add;
}

// Two timelines added, the second undone: as every build before MOTIF6
// wrote a project journal.
ProjectEditLog ProjectJournalWithoutMotifs(Project& project) {
    ProjectEditLog log;
    EditError error = EditError::None;
    std::string message;
    Check(log.Apply(project, Timeline(kVertical, "Verticale", 1080, 1920),
                    error, message) &&
              log.Apply(project, Timeline(kSquare, "Carrée", 1080, 1080), error,
                        message) &&
              log.Undo(project, error, message),
          "the project journal without motifs is built: " + message);
    return log;
}

// What MOTIF6 adds: a declension with its reason, an entry left without,
// the person's choice between the three timelines, and a choice undone.
ProjectEditLog ProjectJournalWithMotifs(Project& project) {
    ProjectEditLog log;
    EditError error = EditError::None;
    std::string message;
    EditMotif format;
    format.reason = MotifReason::Format;
    format.note = "la déclinaison verticale du brief";
    EditMotif rhythm;
    rhythm.reason = MotifReason::Rhythm;
    rhythm.author = MotifAuthor::Person;
    rhythm.note = "la verticale tient mieux la chute";
    EditMotif selection;
    selection.reason = MotifReason::Selection;
    Check(log.Apply(project, Timeline(kVertical, "Verticale", 1080, 1920),
                    error, message, ProjectEntryMotifsFor(format)) &&
              log.Apply(project, Timeline(kSquare, "Carrée", 1080, 1080), error,
                        message) &&
              log.Apply(project,
                        ChooseProjectTimelineOperation{
                            kVertical, {kMain, kSquare}, true},
                        error, message, ProjectEntryMotifsFor(rhythm)) &&
              log.Apply(
                  project,
                  ChooseProjectTimelineOperation{kSquare, {kVertical}, false},
                  error, message, ProjectEntryMotifsFor(selection)) &&
              log.Undo(project, error, message),
          "the project journal with motifs is built: " + message);
    return log;
}

void CheckProjectFixtures(bool print) {
    Project plain = ProjectFixture();
    const std::string without = ProjectJournalWithoutMotifs(plain).Serialize();
    Project motivated = ProjectFixture();
    const std::string with = ProjectJournalWithMotifs(motivated).Serialize();
    if (print) {
        std::cout << "--- project_without_motifs.json\n"
                  << without << "--- project_with_motifs.json\n"
                  << with;
        return;
    }
    Check(without == ReadFixture("project_without_motifs.json"),
          "a project journal without motifs is written as before MOTIF6, "
          "version 1; if the change is meant, project_without_motifs.json:\n" +
              without);
    Check(with == ReadFixture("project_with_motifs.json"),
          "a project journal with motifs is written as pinned; if the change "
          "is meant, project_with_motifs.json:\n" +
              with);
    for (const std::string& name :
         {std::string("project_without_motifs.json"),
          std::string("project_with_motifs.json"),
          std::string("project_before_motif6.json")}) {
        const std::string text = ReadFixture(name);
        ProjectEditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!text.empty() &&
                  ProjectEditLog::Deserialize(text, log, error, message),
              name + " reads: " + message);
        Check(log.Serialize() == text,
              name + " reads back to its own bytes:\n" + log.Serialize());
    }
    // Written by main fe258ff, before MOTIF6 (`--create-project`, then
    // `--call-tool` add_timeline and clone_timeline, the clone undone by
    // `--undo-project-op`): read, nothing gained, nothing lost.
    {
        ProjectEditLog old;
        EditError error = EditError::None;
        std::string message;
        ProjectEditLog::Deserialize(ReadFixture("project_before_motif6.json"),
                                    old, error, message);
        Check(old.AppliedCount() == 1 && old.UndoneCount() == 1 &&
                  old.AppliedEntries()[0].motifs.empty() &&
                  old.UndoneEntries()[0].motifs.empty(),
              "an old project journal has its entries and no motif");
        const std::string listed =
            ListProjectMotifsJson(Project(), old, {}, "");
        Check(listed.find("\"groups\":[]") != std::string::npos &&
                  listed.find("\"operations_without_motif\":1") !=
                      std::string::npos,
              "list_motifs on an old project journal: no group, its one "
              "entry without motif: " +
                  listed);
    }
    // What the motifs carry, read back, and the state they led to.
    ProjectEditLog read;
    EditError error = EditError::None;
    std::string message;
    Check(ProjectEditLog::Deserialize(with, read, error, message) &&
              read.AppliedCount() == 3 && read.UndoneCount() == 1 &&
              read.AppliedEntries()[0].motifs.size() == 1 &&
              read.AppliedEntries()[0].motifs[0].motif.reason ==
                  MotifReason::Format &&
              read.AppliedEntries()[1].motifs.empty() &&
              read.AppliedEntries()[2].motifs[0].motif.author ==
                  MotifAuthor::Person &&
              read.UndoneEntries()[0].motifs[0].motif.reason ==
                  MotifReason::Selection,
          "the project motifs read back whole: " + message);
    Check(motivated.active_timeline_id == kVertical &&
              motivated.timelines.size() == 3,
          "the choice made the kept timeline active, and only that");
}

void CheckProjectVersions() {
    const std::string withMotifs = ReadFixture("project_with_motifs.json");
    ProjectEditLog log;
    EditError error = EditError::None;
    std::string message;
    std::string newer = withMotifs;
    newer.replace(newer.find("\"version\":2"), 11, "\"version\":3");
    Check(!ProjectEditLog::Deserialize(newer, log, error, message) &&
              error == EditError::ParseError &&
              message.find("project edit log version 3 is newer than this "
                           "build reads (up to 2)") != std::string::npos,
          "a newer project journal is refused by name: " + message);
    std::string mislabelled = withMotifs;
    mislabelled.replace(mislabelled.find("\"version\":2"), 11, "\"version\":1");
    Check(!ProjectEditLog::Deserialize(mislabelled, log, error, message),
          "a version 1 project journal carries no motif");
    std::string empty = ReadFixture("project_without_motifs.json");
    empty.replace(empty.find("\"version\":1"), 11, "\"version\":2");
    Check(ProjectEditLog::Deserialize(empty, log, error, message) &&
              log.Serialize() == ReadFixture("project_without_motifs.json"),
          "a version 2 project journal without a motif is read, and written "
          "as the version 1 it can be: " +
              message);
    std::string wide = withMotifs;
    wide.replace(wide.find("\"first\":0,\"count\":1"), 19,
                 "\"first\":0,\"count\":2");
    Check(!ProjectEditLog::Deserialize(wide, log, error, message),
          "a project entry is one member: a wider span does not read");
}

void CheckProjectList() {
    Project project = ProjectFixture();
    const ProjectEditLog log = ProjectJournalWithMotifs(project);
    const mcp_json::Value all =
        Parse(ListProjectMotifsJson(project, log, {}, ""));
    const mcp_json::Value* groups = all.Find("groups");
    int64_t without = -1;
    Check(all.Find("journal") != nullptr &&
              all.Find("journal")->AsString() == "project" &&
              groups != nullptr && groups->AsArray().size() == 2 &&
              all.Find("operations_without_motif")->AsInt64(without) &&
              without == 1,
          "two groups (format, rhythm), the undone choice left out, one "
          "entry nobody explained: " +
              all.Dump());
    if (groups != nullptr && groups->AsArray().size() == 2) {
        const mcp_json::Value& choice = groups->AsArray()[1];
        const mcp_json::Value& entry = choice.Find("entries")->AsArray()[0];
        const mcp_json::Value& touched = entry.Find("touched")->AsArray()[0];
        Check(choice.Find("reason")->AsString() == "rhythm" &&
                  entry.Find("type")->AsString() == "ChooseProjectTimeline" &&
                  entry.Find("author")->AsString() == "person" &&
                  entry.Find("note")->AsString() ==
                      "la verticale tient mieux la chute" &&
                  touched.Find("timeline")->Find("name")->AsString() ==
                      "Verticale" &&
                  touched.Find("alternatives")->AsArray().size() == 2 &&
                  touched.Find("alternatives")
                          ->AsArray()[1]
                          .Find("name")
                          ->AsString() == "Carrée" &&
                  touched.Find("make_active") != nullptr,
              "a choice reads as the timeline kept rather than those, by "
              "name: " +
                  choice.Dump());
        const mcp_json::Value& declined = groups->AsArray()[0];
        Check(declined.Find("reason")->AsString() == "format" &&
                  declined.Find("entries")
                          ->AsArray()[0]
                          .Find("touched")
                          ->AsArray()[0]
                          .Find("timeline")
                          ->Find("timeline_id")
                          ->AsString() == kVertical,
              "a timeline created says which: " + declined.Dump());
    }
    // A timeline removed since is still named, by ID, and said gone.
    Project without_square = project;
    without_square.timelines.pop_back();
    const std::string gone =
        ListProjectMotifsJson(without_square, log, MotifReason::Rhythm, "");
    Check(gone.find("{\"timeline_id\":\"" + kSquare + "\",\"removed\":true}") !=
                  std::string::npos &&
              Parse(gone).Find("groups")->AsArray().size() == 1,
          "filtered on a reason, a timeline gone since said removed: " + gone);
}

void CheckVersions() {
    const std::string withMotifs = ReadFixture("with_motifs.json");
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    std::string newer = withMotifs;
    newer.replace(newer.find("\"version\":2"), 11, "\"version\":3");
    Check(!EditLog::Deserialize(newer, log, error, message) &&
              error == EditError::ParseError &&
              message.find("edit log version 3 is newer than this build "
                           "reads (up to 2)") != std::string::npos,
          "a newer journal is refused by name: " + message);
    std::string mislabelled = withMotifs;
    mislabelled.replace(mislabelled.find("\"version\":2"), 11, "\"version\":1");
    Check(!EditLog::Deserialize(mislabelled, log, error, message),
          "a version 1 journal carries no motif");
    std::string empty = ReadFixture("without_motifs.json");
    empty.replace(empty.find("\"version\":1"), 11, "\"version\":2");
    Check(EditLog::Deserialize(empty, log, error, message) &&
              log.Serialize() == ReadFixture("without_motifs.json"),
          "a version 2 journal without a motif is read, and written as the "
          "version 1 it can be: " +
              message);
}

void CheckSpans() {
    Document document = Fixture();
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    const Operation marker = Marker("01K3M000000000000000000001", "A", 3);
    const std::string before = document.SaveToString();
    Check(!log.Apply(document, marker, error, message,
                     {{0, 2, PauseMotif(400)}}) &&
              !log.Apply(document, marker, error, message,
                         {{0, 0, PauseMotif(400)}}) &&
              log.AppliedCount() == 0 && document.SaveToString() == before,
          "a span outside the entry refuses the edit before it is applied");
    const Operation batch =
        BatchOperation{"Deux",
                       {Marker("01K3M000000000000000000002", "B", 4),
                        Marker("01K3M000000000000000000003", "C", 5)}};
    Check(!log.Apply(document, batch, error, message,
                     {{0, 2, PauseMotif(400)}, {1, 1, PauseMotif(200)}}) &&
              !log.Apply(document, batch, error, message,
                         {{1, 1, PauseMotif(400)}, {0, 1, PauseMotif(200)}}),
          "spans are sorted and disjoint");
    EditMotif empty;
    Check(!log.Apply(document, marker, error, message, {{0, 1, empty}}),
          "a motif that says nothing is refused");

    // Undo and redo: the document's bytes as before, the motif carried.
    Check(log.Apply(document, batch, error, message, {{0, 2, PauseMotif(400)}}),
          "a batch under one motif applies: " + message);
    const std::string applied = log.Serialize();
    const std::string after = document.SaveToString();
    Check(log.Undo(document, error, message) &&
              document.SaveToString() == before &&
              log.UndoneEntries().back().motifs.size() == 1,
          "undo restores the document byte for byte, the motif kept on the "
          "undone entry");
    Check(log.Redo(document, error, message) &&
              document.SaveToString() == after && log.Serialize() == applied,
          "redo replays to the same document and the same journal bytes");
}

void CheckFold() {
    Document document = Fixture();
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    Check(log.Apply(document, Marker("01K3M000000000000000000001", "A", 3),
                    error, message),
          "an entry before the intervention");
    const InterventionMark mark = MarkIntervention(document.sequence.id, log);
    const Operation pause = Marker("01K3M000000000000000000002", "B", 4);
    const Operation batch =
        BatchOperation{"Deux",
                       {Marker("01K3M000000000000000000003", "C", 5),
                        Marker("01K3M000000000000000000004", "D", 6)}};
    EditMotif cutaway;
    cutaway.reason = MotifReason::Cutaway;
    Check(log.Apply(document, pause, error, message,
                    EntryMotifsFor(pause, {}, PauseMotif(300))) &&
              log.Apply(document, batch, error, message, {{1, 1, cutaway}}) &&
              log.Apply(document, Marker("01K3M000000000000000000005", "E", 7),
                        error, message),
          "three entries in the intervention: " + message);
    EditMotif brief;
    brief.reason = MotifReason::Brief;
    size_t grouped = 0;
    Check(log.GroupSince(document, mark, "Agent", grouped, error, message,
                         brief) &&
              grouped == 3,
          "the intervention folds: " + message);
    const std::vector<EntryMotif>& spans = log.AppliedEntries().back().motifs;
    Check(spans.size() == 4 && spans[0].first == 0 &&
              spans[0].motif.reason == MotifReason::Pause &&
              spans[1].first == 1 && spans[1].count == 1 &&
              spans[1].motif.reason == MotifReason::Brief &&
              spans[2].first == 2 &&
              spans[2].motif.reason == MotifReason::Cutaway &&
              spans[3].first == 3 &&
              spans[3].motif.reason == MotifReason::Brief,
          "each entry's motifs follow its members, the intervention's fill "
          "the rest");
    EditLog reread;
    Check(EditLog::Deserialize(log.Serialize(), reread, error, message) &&
              reread.Serialize() == log.Serialize(),
          "the folded journal reads back to its bytes: " + message);
}

void CheckList() {
    Document document = Fixture();
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    const Operation a = Marker("01K3M000000000000000000001", "A", 3);
    const Operation b = Marker("01K3M000000000000000000002", "B", 4);
    const Operation c = Marker("01K3M000000000000000000003", "C", 5);
    const Operation d = Marker("01K3M000000000000000000004", "D", 6);
    const Operation cut = RemoveClipOperation{kFirst, {}, {}};
    EditMotif mine;
    mine.reason = MotifReason::Duplicate;
    mine.note = "la prise 2 est meilleure";
    Check(log.Apply(document, a, error, message,
                    EntryMotifsFor(a, {}, PauseMotif(400))) &&
              log.Apply(document, b, error, message,
                        EntryMotifsFor(b, {}, PauseMotif(250))) &&
              log.Apply(document, c, error, message,
                        EntryMotifsFor(c, {}, PauseMotif(400))) &&
              log.Apply(document, cut, error, message,
                        EntryMotifsFor(cut, {}, mine)) &&
              log.Apply(document, d, error, message,
                        EntryMotifsFor(d, {}, PauseMotif(100))) &&
              log.Undo(document, error, message),
          "five edits, the last undone: " + message);
    const mcp_json::Value all =
        Parse(ListMotifsJson(document.sequence.id, log, {}, ""));
    const mcp_json::Value* groups = all.Find("groups");
    Check(groups != nullptr && groups->AsArray().size() == 3,
          "one group per reason, rule and parameters, the undone entry left "
          "out: " +
              all.Dump());
    if (groups != nullptr && groups->AsArray().size() == 3) {
        const mcp_json::Value& first = groups->AsArray()[0];
        int64_t operations = 0;
        Check(first.Find("params")->Dump() ==
                      "{\"min_gap_ms\":400,\"keep_frames\":6}" &&
                  first.Find("operations")->AsInt64(operations) &&
                  operations == 2 &&
                  first.Find("entries")->AsArray().size() == 2,
              "the two passes at 400 ms are one group: " + first.Dump());
        const mcp_json::Value& third = groups->AsArray()[2];
        Check(third.Find("reason")->AsString() == "duplicate" &&
                  third.Find("rule") == nullptr &&
                  third.Find("entries")
                          ->AsArray()[0]
                          .Find("touched")
                          ->AsArray()[0]
                          .Find("type")
                          ->AsString() == "RemoveClip",
              "a caller's own reason, and what it touched: " + third.Dump());
    }
    const mcp_json::Value pauses = Parse(ListMotifsJson(
        document.sequence.id, log, MotifReason::Pause, "tighten_pauses"));
    Check(pauses.Find("groups")->AsArray().size() == 2,
          "filtered on the reason and the rule: " + pauses.Dump());
}

void CheckArguments() {
    EditMotif motif;
    std::string message;
    Check(ParseMotifArgument(
              Parse(R"({"reason":"marker","marker_id":"M1","author":)"
                    R"("person","note":"l'auteur l'a marqué"})"),
              "t.motif", motif, message) &&
              motif.reason == MotifReason::Marker &&
              motif.author == MotifAuthor::Person && motif.marker_id == "M1",
          "a motif argument is read: " + message);
    Check(ParseMotifArgument(Parse(R"({"note":"juste une note"})"), "t.motif",
                             motif, message) &&
              motif.reason == MotifReason::Unstated &&
              motif.author == MotifAuthor::Agent,
          "a note alone is a motif, by an agent");
    for (const char* refused :
         {R"({})", R"({"reason":"unstated"})", R"({"reason":"Pause"})",
          R"({"reason":"other"})", R"({"reason":"pause","rule":"x"})",
          R"({"reason":"pause","params":[1]})", R"({"reason":"pause","why":1})",
          R"({"author":"robot","note":"x"})", R"("pause")"}) {
        Check(!ParseMotifArgument(Parse(refused), "t.motif", motif, message),
              std::string("refused: ") + refused);
    }
    std::string note;
    for (int index = 0; index < 280; ++index) note += "é";
    EditMotif longNote;
    longNote.note = note;
    Check(ValidateEditMotif(longNote, message),
          "a note of 280 characters, two bytes each, is a line");
    longNote.note += "é";
    Check(!ValidateEditMotif(longNote, message), "281 is not");
    for (const std::string& name : MotifReasonNames()) {
        MotifReason reason = MotifReason::Unstated;
        Check(ParseMotifReason(name, reason) && MotifReasonName(reason) == name,
              "the vocabulary reads back: " + name);
    }
    Check(MotifReasonNames().size() == 13,
          "thirteen reasons a caller may give");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--print") {
        CheckFixtures(true);
        CheckProjectFixtures(true);
        return 0;
    }
    CheckFixtures(false);
    CheckVersions();
    CheckSpans();
    CheckFold();
    CheckList();
    CheckArguments();
    CheckProjectFixtures(false);
    CheckProjectVersions();
    CheckProjectList();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "edit_motif_tests: all passed\n";
    return 0;
}
