// PRD7 -- docs/roadmap/PRODUIT-2026-10.md. What an ingest does to a document
// (AddProbedMedia, Ingest.h), given files that were already read. No media
// is decoded and no FFmpeg binary runs: the probed entries are built here,
// so this runs on a plain host like the other portable suites.
//
// What it guards: the window and the command line add a rush through this
// one function, so the same file gives the same library entry on both. The
// only thing a window says that a command line does not is the bin, and
// that is all that may differ. ingest_tests.cc holds the same comparison on
// a real file.

#include "Document.h"
#include "EditLog.h"
#include "Ingest.h"
#include "Operations.h"
#include "Project.h"
#include "Ulid.h"

#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "TestSupport.h"

namespace {

// A camera rush as ProbeIngestFile leaves it: headers read, level measured.
LibraryMedia ProbedRush() {
    LibraryMedia media;
    media.codec = "h264";
    media.width = 3840;
    media.height = 2160;
    media.pixel_format = "yuv422p10le";
    media.color_range = "tv";
    media.color_space = "bt709";
    media.color_transfer = "bt709";
    media.color_primaries = "bt709";
    media.rate = {25, 1};
    media.duration = {250, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    media.audio_level_measured = true;
    media.audio_level = 88000;
    // COLOR-T -- what a Sony camera writes in its rtmd track.
    media.white_balance_kelvin = 4000;
    media.color_input = {"sony_sgamut3_cine", "sony_slog3", "auto", "auto"};
    return media;
}

ProbedIngestFile Read(const std::filesystem::path& path) {
    ProbedIngestFile file;
    file.path = path;
    file.media = ProbedRush();
    return file;
}

// A file ProbeIngestFile left alone: the library held it, complete.
ProbedIngestFile Unread(const std::filesystem::path& path) {
    ProbedIngestFile file;
    file.path = path;
    return file;
}

ProbedIngestFile Unreadable(const std::filesystem::path& path,
                            const std::string& reason) {
    ProbedIngestFile file;
    file.path = path;
    file.error = reason;
    return file;
}

// A library entry as the document format writes it, without the two things
// two ingests of one file may not share: its id, and the path it is stored
// under.
std::string Written(const Document& document, LibraryMedia media) {
    Document holder = document;
    media.id = "01K70000000000000000000000";
    media.path = "rush";
    holder.library = {std::move(media)};
    holder.sources.clear();
    return holder.SaveToString();
}

size_t EntriesIn(const std::filesystem::path& directory) {
    std::error_code error;
    return static_cast<size_t>(
        std::distance(std::filesystem::directory_iterator(directory, error),
                      std::filesystem::directory_iterator()));
}

}  // namespace

int main() {
    std::error_code pathError;
    const std::filesystem::path root = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-ingest-document"),
        pathError);
    const std::filesystem::path project = root / "Film.cutmachine-project";
    const std::filesystem::path rushes = root / "rushes";
    std::filesystem::create_directories(project);
    std::filesystem::create_directories(rushes);
    const std::filesystem::path first = rushes / "C0001.MP4";
    const std::filesystem::path second = rushes / "C0002.MP4";

    const Document empty = Project("Film").MakeActiveDocument();
    std::string reason;

    // New files, as --ingest gives them: no bin said.
    Document document = empty;
    IngestOutcome outcome = AddProbedMedia(
        document, project, {Read(first), Read(second)}, std::nullopt);
    Check(outcome.added_ids.size() == 2 && outcome.skipped == 0 &&
              outcome.moved == 0 && outcome.errors.empty() &&
              outcome.completed_ids.empty() && outcome.changed,
          "two new files are two new entries");
    Check(document.library.size() == 2 && document.sources.size() == 2 &&
              document.Validate(reason),
          "the document holds them and validates: " + reason);
    if (document.library.size() == 2 && outcome.added_ids.size() == 2) {
        const LibraryMedia& media = document.library[0];
        const DocumentSource* source = document.FindSource(media.id);
        Check(media.id == outcome.added_ids[0] &&
                  document.library[1].id == outcome.added_ids[1] &&
                  media.id != document.library[1].id,
              "each new entry has its own id, reported in the order given");
        Check(media.path == "../rushes/C0001.MP4" &&
                  media.filename == "C0001.MP4" && media.bin_id.empty() &&
                  media.proxy_path.empty(),
              "an entry is stored relative to the project, at the root: " +
                  media.path);
        Check(media.metadata_complete && media.audio_level_measured &&
                  media.audio_level == 88000 && media.codec == "h264",
              "the entry carries what was read, the audio level with it");
        Check(media.white_balance_kelvin == 4000 &&
                  media.color_input.transfer == "sony_slog3",
              "and the reading of its picture the camera wrote");
        Check(source != nullptr && source->path == media.path &&
                  source->rate.num == 25 && source->rate.den == 1 &&
                  source->duration == media.duration,
              "a new entry has a source under the same id");
    }
    Check(EntriesIn(project) == 0, "adding to a document writes no file");

    // The same folder again: nothing to learn, nothing changes.
    const std::string once = document.SaveToString();
    outcome = AddProbedMedia(document, project, {Unread(first), Unread(second)},
                             std::nullopt);
    Check(outcome.added_ids.empty() && outcome.skipped == 2 &&
              outcome.errors.empty() && !outcome.changed &&
              document.SaveToString() == once,
          "files the library holds are skipped, and the document keeps its "
          "bytes");
    outcome = AddProbedMedia(document, project, {Read(first)}, std::nullopt);
    Check(outcome.added_ids.empty() && outcome.skipped == 1 &&
              !outcome.changed && document.SaveToString() == once,
          "a known file read again is still the entry it was");
    const std::set<std::string> complete =
        CompleteLibraryPaths(document, project);
    Check(complete.size() == 2 && complete.count(first.string()) == 1 &&
              complete.count(second.string()) == 1,
          "the library's complete entries are known by their resolved paths");

    // Unreadable files are counted and named, in order; nothing else moves.
    outcome = AddProbedMedia(
        document, project,
        {Unreadable(rushes / "notes.txt", "not media"), Unread(first),
         Unreadable(rushes / "corrupt.mp4", "broken")},
        std::nullopt);
    Check(outcome.skipped == 3 && outcome.errors.size() == 2 &&
              outcome.errors[0].file == "notes.txt" &&
              outcome.errors[0].reason == "not media" &&
              outcome.errors[1].file == "corrupt.mp4" &&
              outcome.errors[1].reason == "broken" && !outcome.changed &&
              document.SaveToString() == once,
          "an unreadable file is skipped with its reason, a known one "
          "without");
    // PRD20 -- an ingest read nothing when every file it was handed failed:
    // then, and only then, it is a failure for whoever reports it.
    Check(!IngestReadNothing(outcome),
          "an ingest that met a file the library holds did not read nothing");
    {
        Document untouched = empty;
        IngestOutcome failed =
            AddProbedMedia(untouched, project,
                           {Unreadable(rushes / "notes.txt", "not media"),
                            Unreadable(rushes / "corrupt.mp4", "broken")},
                           std::nullopt);
        Check(IngestReadNothing(failed) && !failed.changed &&
                  failed.errors.size() == 2 && untouched.library.empty(),
              "files that all fail are an ingest that read nothing");
        failed = AddProbedMedia(
            untouched, project,
            {Unreadable(rushes / "notes.txt", "not media"), Read(first)},
            std::nullopt);
        Check(!IngestReadNothing(failed) && failed.added_ids.size() == 1 &&
                  failed.errors.size() == 1,
              "one file read among failures is a partial success");
        failed = AddProbedMedia(untouched, project, {}, std::nullopt);
        Check(!IngestReadNothing(failed) && failed.errors.empty(),
              "no file at all is not a failure: nothing was refused");
    }

    // One file reached twice in one ingest is one entry.
    Document twice = empty;
    outcome = AddProbedMedia(twice, project, {Read(first), Read(first)},
                             std::nullopt);
    Check(outcome.added_ids.size() == 1 && outcome.skipped == 1 &&
              twice.library.size() == 1 && twice.sources.size() == 1,
          "two paths to one file add it once");

    // A file left unread because the library held it, which it no longer
    // does: refused with a reason, not added blind.
    Document moved = empty;
    outcome = AddProbedMedia(moved, project, {Unread(first)}, std::nullopt);
    Check(outcome.added_ids.empty() && outcome.skipped == 1 &&
              outcome.errors.size() == 1 &&
              outcome.errors[0].file == "C0001.MP4" && !outcome.changed &&
              moved.library.empty(),
          "a file nobody read is not added");

    // The window's half: the bin a rush is dropped on.
    DocumentBin bin;
    bin.name = "Interviews";
    Document filed = empty;
    filed.bins.push_back(bin);
    outcome = AddProbedMedia(filed, project, {Read(first)}, bin.id);
    Check(outcome.added_ids.size() == 1 && filed.library.size() == 1 &&
              filed.library[0].bin_id == bin.id && filed.Validate(reason),
          "a new file goes to the bin it is ingested into: " + reason);
    outcome = AddProbedMedia(filed, project, {Unread(first)}, std::nullopt);
    Check(outcome.moved == 0 && !outcome.changed &&
              filed.library[0].bin_id == bin.id,
          "without a bin, a known file stays where it is");
    outcome = AddProbedMedia(filed, project, {Unread(first)}, Ulid{});
    Check(outcome.moved == 1 && outcome.skipped == 1 && outcome.changed &&
              outcome.added_ids.empty() && filed.library[0].bin_id.empty(),
          "ingested into the root, a known file moves to the root");
    outcome = AddProbedMedia(filed, project, {Unread(first)}, bin.id);
    Check(outcome.moved == 1 && outcome.changed &&
              filed.library[0].bin_id == bin.id,
          "ingested into a bin, a known file moves into it");
    outcome = AddProbedMedia(filed, project, {Unread(first)}, bin.id);
    Check(outcome.moved == 0 && !outcome.changed,
          "a known file already in the bin is no change");
    // DEC32 (PRD7) -- the ingest operation refuses it by name, and nothing
    // of it is applied: Validate used to be the one to refuse.
    Document astray = empty;
    outcome = AddProbedMedia(astray, project, {Read(first)}, GenerateUlid());
    Check(!outcome.refused.empty() && outcome.added_ids.empty() &&
              !outcome.changed && astray.library.empty() &&
              astray.Validate(reason),
          "a bin the document does not have refuses the ingest: " +
              outcome.refused);

    // The same rush through the window and through --ingest: the bin is the
    // window's to say, and nothing else differs.
    Document byCommand = empty;
    Document byWindow = empty;
    byWindow.bins.push_back(bin);
    AddProbedMedia(byCommand, project, {Read(first)}, std::nullopt);
    AddProbedMedia(byWindow, project, {Read(first)}, Ulid{});
    Check(byCommand.library.size() == 1 && byWindow.library.size() == 1 &&
              byCommand.library[0].path == byWindow.library[0].path &&
              Written(empty, byCommand.library[0]) ==
                  Written(empty, byWindow.library[0]),
          "a rush dropped on the window's root is the entry --ingest writes");
    AddProbedMedia(byWindow, project, {Read(second)}, bin.id);
    AddProbedMedia(byCommand, project, {Read(second)}, std::nullopt);
    if (byWindow.library.size() == 2 && byCommand.library.size() == 2) {
        LibraryMedia unfiled = byWindow.library[1];
        Check(unfiled.bin_id == bin.id, "the window files it");
        unfiled.bin_id.clear();
        Check(Written(empty, unfiled) == Written(empty, byCommand.library[1]),
              "a rush dropped on a bin differs from --ingest's by the bin "
              "alone");
    } else {
        Check(false, "both documents hold two entries");
    }

    // A version-1 entry: known by its path, with no technical metadata and
    // no source. What is read completes it; what the document decided about
    // it -- id, path, bin, proxy -- stays.
    Document legacy = empty;
    legacy.bins.push_back(bin);
    LibraryMedia unprobed;
    unprobed.path = "../rushes/C0001.MP4";
    unprobed.filename = "old name";
    unprobed.rate = {25, 1};
    unprobed.duration = {250, 25};
    unprobed.bin_id = bin.id;
    unprobed.proxy_path = "proxies/C0001.mov";
    unprobed.metadata_complete = false;
    legacy.library.push_back(unprobed);
    Check(CompleteLibraryPaths(legacy, project).empty(),
          "an entry without metadata is still to be read");
    const std::string incomplete = legacy.SaveToString();
    outcome = AddProbedMedia(legacy, project, {Unreadable(first, "offline")},
                             std::nullopt);
    Check(outcome.skipped == 1 && outcome.errors.size() == 1 &&
              !outcome.changed && legacy.SaveToString() == incomplete,
          "an entry whose file cannot be read stays as it was, and is named");
    outcome = AddProbedMedia(legacy, project, {Read(first)}, std::nullopt);
    Check(outcome.added_ids.empty() && outcome.skipped == 1 &&
              outcome.completed_ids.size() == 1 &&
              outcome.completed_ids[0] == unprobed.id && outcome.changed &&
              legacy.library.size() == 1,
          "a known entry without metadata is completed, not added again");
    if (legacy.library.size() == 1) {
        const LibraryMedia& media = legacy.library[0];
        Check(media.id == unprobed.id && media.path == unprobed.path &&
                  media.bin_id == bin.id &&
                  media.proxy_path == "proxies/C0001.mov",
              "completing an entry keeps its id, path, bin and proxy");
        Check(media.metadata_complete && media.filename == "C0001.MP4" &&
                  media.codec == "h264" && media.audio_level_measured &&
                  media.audio_level == 88000,
              "and gives it what was read");
        // COLOR-T -- but not the reading of its picture, which the
        // document holds (KeepMediaReading, Document.h): a grade set
        // against the 6500 K stand-in would turn yellow under the 4000 K
        // the camera wrote.
        Check(media.white_balance_kelvin == 0 &&
                  !DeclaresColorInput(media.color_input),
              "completing an entry keeps how the project reads its picture");
        Check(legacy.FindSource(media.id) != nullptr && legacy.Validate(reason),
              "a completed entry gets the source it lacked: " + reason);
    }

    // LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The path an entry stores: as
    // it was handed, links kept; relative when the rush and the package
    // share a folder below the root (a volume, under /Volumes), absolute
    // otherwise -- never a path that climbs to the root.
    {
        const std::filesystem::path package =
            "/Users/monteur/Films/serie/Essai.cutmachine-project";
        Check(StoredMediaPath("/Users/monteur/Films/serie/Prises /C0101.MP4",
                              package) == "../Prises /C0101.MP4",
              "a rush reached through a link beside the package keeps it");
        Check(StoredMediaPath("/Volumes/Disque/Essai/Prises /C0101.MP4",
                              package) ==
                  "/Volumes/Disque/Essai/Prises /C0101.MP4",
              "a rush on another disk is stored absolute, not as "
              "../../../../../../Volumes/...");
        Check(StoredMediaPath("/Volumes/Disque/Essai/Prises /C0101.MP4",
                              "/Volumes/Disque/Essai/P.cutmachine-project") ==
                  "../Prises /C0101.MP4",
              "a rush on the package's own disk is relative to it");
        Check(StoredMediaPath("/Volumes/Disque/Prises/C1.MP4",
                              "/Volumes/Other/P.cutmachine-project") ==
                  "/Volumes/Disque/Prises/C1.MP4",
              "two disks under /Volumes share no folder");
        Check(StoredMediaPath("/Users/monteur/P.cutmachine-project/media/a.mov",
                              "/Users/monteur/P.cutmachine-project") ==
                  "media/a.mov",
              "a media inside the package is stored inside it");
        Check(StoredMediaPath("/Users/monteur/a/../rushes/./b.mov",
                              "/Users/monteur/P.cutmachine-project") ==
                  "../rushes/b.mov",
              "the path is normalised lexically");
        // And an ingest stores what it was handed, while it recognises the
        // file by its resolved path.
        Document linked = empty;
        ProbedIngestFile throughLink = Read("/Volumes/Disque/Essai/C0101.MP4");
        throughLink.given = "/Users/monteur/Films/serie/Prises /C0101.MP4";
        AddProbedMedia(linked, package, {throughLink}, std::nullopt);
        Check(linked.library.size() == 1 &&
                  linked.library[0].path == "../Prises /C0101.MP4" &&
                  linked.sources.size() == 1 &&
                  linked.sources[0].path == "../Prises /C0101.MP4",
              "an ingest through a link stores the link's path");
    }

    // A complete entry with no source: the ingest gives it one.
    Document sourceless = empty;
    AddProbedMedia(sourceless, project, {Read(first)}, std::nullopt);
    sourceless.sources.clear();
    outcome =
        AddProbedMedia(sourceless, project, {Unread(first)}, std::nullopt);
    Check(outcome.changed && outcome.added_ids.empty() &&
              sourceless.sources.size() == 1 &&
              sourceless.sources[0].id == sourceless.library[0].id,
          "a known entry without a source gets it back");

    // The two repairs are an operation's, not writes beside it
    // (IngestMediaOperation's `completed` and `mounted`): one journal entry,
    // undone and redone byte for byte, the entry put back as it was.
    {
        Document stale = empty;
        LibraryMedia unread;
        unread.path = "../rushes/C0001.MP4";
        unread.filename = "old name";
        unread.rate = {25, 1};
        unread.duration = {250, 25};
        unread.proxy_path = "proxies/C0001.mov";
        unread.metadata_complete = false;
        stale.library.push_back(unread);
        const std::string before = stale.SaveToString();
        const IngestPlan plan =
            PlanProbedMedia(stale, project, {Read(first)}, std::nullopt);
        Check(plan.HasJournalEntry() && plan.operation.added.empty() &&
                  plan.operation.filed.empty() &&
                  plan.operation.completed.size() == 1 &&
                  plan.operation.completed[0].id == unread.id &&
                  plan.operation.mounted.size() == 1 &&
                  plan.operation.mounted[0] == unread.id &&
                  stale.SaveToString() == before,
              "the plan carries the completion and the source it mounts, and "
              "changes nothing");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(stale, plan.operation, error, message) &&
                  log.AppliedCount() == 1 &&
                  stale.library[0].metadata_complete &&
                  stale.library[0].proxy_path == "proxies/C0001.mov" &&
                  stale.FindSource(unread.id) != nullptr &&
                  stale.Validate(reason),
              "the repairs apply as one journal entry: " + message + reason);
        const std::string after = stale.SaveToString();
        Check(log.Undo(stale, error, message) && stale.SaveToString() == before,
              "undo puts the entry back unread and unmounted, byte for byte: " +
                  message);
        Check(log.Redo(stale, error, message) && stale.SaveToString() == after,
              "redo repairs it again, byte for byte: " + message);
        const std::string journal = log.Serialize();
        EditLog reread;
        Check(EditLog::Deserialize(journal, reread, error, message) &&
                  reread.Serialize() == journal &&
                  journal.find("\"completed\"") != std::string::npos &&
                  journal.find("\"restored\"") != std::string::npos,
              "the journal carries both repairs and their inverse: " + message);
        Document replayed = stale;
        Check(reread.Undo(replayed, error, message) &&
                  replayed.SaveToString() == before,
              "and its reread entry undoes the same: " + message);
        Operation inverse = RemoveClipOperation{};
        Operation twice = plan.operation;
        Document mounted = stale;
        Check(!ApplyOperation(mounted, twice, inverse, error, message) &&
                  error == EditError::InvalidOperation,
              "mounting an entry that has a source is refused: " + message);
    }

    // DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. The ingest as an
    // operation: planned without touching the document, one step of a
    // journal, undone and redone byte for byte, written and read back as it
    // was; its inverse an operation of its own, refused where it could not
    // take an ingest back exactly.
    {
        Document base = empty;
        DocumentBin bin;
        bin.name = "Rushes";
        base.bins.push_back(bin);
        AddProbedMedia(base, project, {Read(first)}, std::nullopt);
        const Ulid known = base.library.empty() ? Ulid{} : base.library[0].id;
        const std::string before = base.SaveToString();
        const IngestPlan plan = PlanProbedMedia(
            base, project, {Unread(first), Read(second), Read(second)}, bin.id);
        Check(plan.HasJournalEntry() && plan.operation.added.size() == 1 &&
                  plan.operation.filed.size() == 1 &&
                  plan.operation.filed[0] == known &&
                  plan.operation.bin_id == bin.id &&
                  plan.operation.added[0].bin_id == bin.id &&
                  plan.outcome.moved == 1 && plan.outcome.skipped == 2 &&
                  plan.operation.completed.empty() &&
                  plan.operation.mounted.empty(),
              "the plan: one new entry in the bin, the known one filed there, "
              "a second path to the new file skipped");
        Check(base.SaveToString() == before, "planning changes nothing");

        EditLog log;
        Document edited = base;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(edited, plan.operation, error, message),
              "the ingest applies through a journal: " + message);
        const std::string after = edited.SaveToString();
        Check(log.AppliedCount() == 1 && edited.library.size() == 2 &&
                  edited.sources.size() == 2 &&
                  edited.library[0].bin_id == bin.id &&
                  edited.library[1].bin_id == bin.id && edited.Validate(reason),
              "one step: both in the bin, the new one with its source: " +
                  reason);
        Check(
            log.Undo(edited, error, message) && edited.SaveToString() == before,
            "undo restores the document byte for byte: " + message);
        Check(
            log.Redo(edited, error, message) && edited.SaveToString() == after,
            "redo replays it byte for byte: " + message);

        const std::string journal = log.Serialize();
        EditLog reread;
        Check(EditLog::Deserialize(journal, reread, error, message) &&
                  reread.Serialize() == journal,
              "the journal reads back to its bytes: " + message);
        Document replayed = edited;
        Check(reread.Undo(replayed, error, message) &&
                  replayed.SaveToString() == before,
              "and its entry undoes the same: " + message);

        const std::string json = SerializeOperation(plan.operation);
        Operation parsed = RemoveClipOperation{};
        Check(json.rfind("{\"type\":\"IngestMedia\",\"added\":[{", 0) == 0 &&
                  json.find("\"completed\"") == std::string::npos &&
                  json.find("\"mounted\"") == std::string::npos &&
                  DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<IngestMediaOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "the operation is canonical JSON, read back to its bytes: " +
                  json.substr(0, 60));

        // Its inverse, on its own: apply, then its own inverse.
        Operation remove = RemoveIngestedMediaOperation{
            {plan.operation.added[0].id}, bin.id, {{known, Ulid{}}}};
        Operation inverse = RemoveClipOperation{};
        Document removing = edited;
        Check(
            ApplyOperation(removing, remove, inverse, error, message) &&
                removing.SaveToString() == before,
            "removing the ingest restores the document before it: " + message);
        Operation reverse = RemoveClipOperation{};
        Check(std::holds_alternative<IngestMediaOperation>(inverse) &&
                  ApplyOperation(removing, inverse, reverse, error, message) &&
                  removing.SaveToString() == after,
              "and its inverse ingests again, byte for byte: " + message);

        // What it refuses.
        const auto refused = [&](Document document, Operation operation,
                                 EditError expected, const std::string& what) {
            const std::string bytes = document.SaveToString();
            Operation ignored = RemoveClipOperation{};
            EditError got = EditError::None;
            std::string why;
            Check(!ApplyOperation(document, operation, ignored, got, why) &&
                      got == expected && !why.empty() &&
                      document.SaveToString() == bytes,
                  what + ": " + why);
        };
        IngestMediaOperation stray = plan.operation;
        stray.bin_id = GenerateUlid();
        refused(base, stray, EditError::UnknownBin, "a bin that is not there");
        refused(base, IngestMediaOperation{}, EditError::InvalidOperation,
                "an ingest of nothing");
        IngestMediaOperation twice = plan.operation;
        twice.added.push_back(twice.added[0]);
        refused(base, twice, EditError::DuplicateId, "one id twice");
        refused(edited, plan.operation, EditError::DuplicateId,
                "an entry the library holds already");
        // Something ingested after it: the entry is no longer last.
        Document later = edited;
        AddProbedMedia(later, project, {Read(rushes / "C0003.MP4")},
                       std::nullopt);
        refused(later, remove, EditError::InvalidOperation,
                "the entry is no longer last");
        // A clip uses it.
        Document used = edited;
        const LibraryMedia& added = used.library.back();
        Operation insert = InsertClipOperation{used.sequence.tracks[0].id,
                                               added.id,
                                               {0, 25},
                                               added.duration,
                                               {0, 25},
                                               {},
                                               {}};
        Operation removeClip = RemoveClipOperation{};
        Check(ApplyOperation(used, insert, removeClip, error, message),
              "a clip of the new rush: " + message);
        refused(used, remove, EditError::InvalidOperation,
                "the entry a clip uses");
        // The known entry moved since.
        Document moved = edited;
        moved.library[0].bin_id.clear();
        refused(moved, remove, EditError::InvalidOperation,
                "a refiled entry no longer in the bin");
    }

    // The first half, where no decoding is needed to tell: a settled path
    // is left alone, a missing file is an error and stays unsettled.
    std::set<std::string> settled = {first.string()};
    const ProbedIngestFile left = ProbeIngestFile(first, settled);
    Check(!left.media && left.error.empty() && left.path == first,
          "a settled file is not read again");
    const ProbedIngestFile missing = ProbeIngestFile(second, settled);
    Check(!missing.media && !missing.error.empty() && missing.path == second &&
              settled.size() == 1,
          "a file that cannot be read says why, and is not settled");

    Check(EntriesIn(project) == 0 && EntriesIn(rushes) == 0,
          "nothing was written beside the project or the rushes");
    std::filesystem::remove_all(root, pathError);
    if (failures != 0) {
        std::cerr << failures << " ingest document assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: ingest document\n";
    return 0;
}
