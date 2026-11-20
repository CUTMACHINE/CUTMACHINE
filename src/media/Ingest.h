#pragma once

#include "Document.h"
#include "Operations.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

class MediaTaskContext;

// G7, G3 -- docs/roadmap/GRADE-2026-09.md. What ingest reads of a file's
// colour: the white balance its camera wrote (CameraMetadata.h), 0 when
// none, and the colour input the file declares (Ingest.cc), undeclared when
// it says nothing it can be trusted on.
struct MediaFileColor {
    int32_t white_balance_kelvin = 0;
    MediaColorInput input;
};

// Reads them again, for a media ingested before either was read. False only
// when the file cannot be probed.
bool ReadFileColorFacts(const std::string& path, MediaFileColor& color,
                        std::string& reason);

// Reads container/stream headers without decoding frames. The caller owns
// identity/path fields; technical metadata is filled on success.
bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason);

// QC-2026-09 A3 -- mean level of a media's first audio stream, in Document.h's
// kAudioLevelScale. Separate from ProbeMediaMetadata, which is a header scan
// and stays one: this decodes the audio, so it is the one part of an ingest
// that costs real time. It buys back far more than it costs -- 29 of the 71
// rushes of one measured project were mute cutaways at -74 dBFS, and each
// went to Whisper at eleven times its own runtime before anyone noticed.
//
// Decoded through the FFmpeg binary, mono at a low rate, exactly as
// Waveform.cc and SpeechOnset.cc already do: one decode pipeline for audio
// analysis in this project, not three. RMS is accumulated in integers so two
// runs over the same file give the same number.
//
// Downmixed and band-limited, so this is close to but not identical to
// FFmpeg's own volumedetect on the original streams -- measured 0.7 dB under
// it on a stereo rush, which is the decorrelation between the two channels
// disappearing into the mono sum. That does not matter for what the figure
// is for: it separates -74 dBFS from -30 dBFS, and both readings agree about
// which side of that line a rush is on.
//
// Returns false with a reason when the media has no audio stream, or when
// FFmpeg cannot be run -- neither of which is a reason to fail an ingest, so
// the caller records the entry as unmeasured and moves on.
bool MeasureMediaAudioLevel(const std::string& path,
                            const std::string& ffmpegPath, int64_t& level,
                            std::string& reason);

// What a Mac leaves on a volume that is not media, and that a folder scan
// passes over without a word: the AppleDouble companion ("._C1500.MP4") of
// every file written to an exFAT or FAT drive, .DS_Store, and the volume's
// own system folders (Spotlight, fseventsd, the trash, and Windows'
// equivalents on a shared drive). An ingest of 169 rushes off an exFAT card
// used to report 169 more "skipped" files, one per rush, each with an FFmpeg
// error, burying the one skip that mattered (24 September 2026). A path
// named explicitly is never litter -- only what a scan finds.
bool IsVolumeLitter(const std::filesystem::path& path);
bool IsVolumeSystemDirectory(const std::filesystem::path& path);

// PRD7 -- docs/roadmap/PRODUIT-2026-10.md. An ingest in two halves, so that
// the command line and the window run the same one.
//
// The window used to add media to the library with a loop of its own, which
// measured no audio level and completed no incomplete entry. A rush dropped
// on the window and the same rush given to --ingest were two different
// library entries, and only one of them could be told from a mute cutaway
// before Whisper ran on it.
//
// The first half (ProbeIngestFile) looks at a file and touches no document.
// It is the slow one -- it decodes the sound -- and the one a window keeps
// off its main thread. The second half (AddProbedMedia) adds what was found
// to a document and touches no file: it writes nothing, so whoever calls it
// still validates the document and decides how it reaches the disk, or how
// it is put back.

// A file an ingest could not read, as its report names it.
struct IngestError {
    std::string file;
    std::string reason;
};

// LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The path a library entry stores
// for `file`, a media reached from a
// project whose package is `projectDirectory`. Both are taken as they were
// handed -- made absolute and normalised lexically, links kept -- and the
// path is stored relative when the two share a folder below the root of
// the file system (the volume, under /Volumes), absolute otherwise.
//
// Ingest, the window's relinks and --import-resolve all store through this.
// They used to store std::filesystem::relative of the file with every link
// resolved: a rush reached through a « Prises » link beside the package, on
// an external disk, became ../../../../../../Volumes/Disque/..., a path that
// climbs to the root and is right only at the depth the package had then.
// On 4 October 2026 a relink in bulk wrote 710 such paths into a copy of a
// project, and every copy of that copy, deeper in the file system, lost
// every rush. Now the rush keeps ../Prises /C0101.MP4 -- the package and its
// link move together -- or, picked straight on the disk, its absolute path.
std::string StoredMediaPath(const std::filesystem::path& file,
                            const std::filesystem::path& projectDirectory);

// One file an ingest was handed, as the first half left it.
struct ProbedIngestFile {
    // Absolute, symlinks followed: what a library entry is recognised by.
    // The path as it was given when it could not be resolved.
    std::filesystem::path path;
    // Absolute, as it was handed, links kept: what the entry stores
    // (StoredMediaPath). Empty, `path` stands in.
    std::filesystem::path given;
    // Technical metadata and audio level, when the file was read. Identity,
    // stored path, name, bin and proxy belong to the document: whatever
    // this carries there is ignored.
    std::optional<LibraryMedia> media;
    // Why the file could not be resolved or read. Empty with no `media` for
    // a file there was nothing to learn about: the library already held it,
    // complete.
    std::string error;
};

// The resolved paths of the library entries whose metadata is complete: the
// files an ingest has nothing to learn about, and does not decode again.
// `projectDirectory` is what stored relative paths are relative to.
std::set<std::string> CompleteLibraryPaths(
    const Document& document, const std::filesystem::path& projectDirectory);

// The first half, for one file: resolves its path and, unless `settled`
// holds it, reads its headers and measures its audio level. A file read
// successfully joins `settled`, so that a second path to the same file --
// a symlink beside it -- is not decoded twice. Seed `settled` with
// CompleteLibraryPaths. Reads no document and may run on any thread.
ProbedIngestFile ProbeIngestFile(const std::filesystem::path& candidate,
                                 std::set<std::string>& settled);

struct IngestOutcome {
    // New library entries, in the order their files were given.
    std::vector<Ulid> added_ids;
    // Entries that had no technical metadata (version-1 media, see
    // LibraryMedia::metadata_complete) and now have it.
    std::vector<Ulid> completed_ids;
    // Every file that did not become a new entry: already in the library,
    // or unreadable.
    size_t skipped = 0;
    // Known entries filed into the target bin. 0 without one.
    size_t moved = 0;
    // One per file that could not be read, in the order given. A file the
    // library already holds is counted in `skipped` and never listed: it is
    // the expected outcome of ingesting a folder again, not a failure.
    std::vector<IngestError> errors;
    // The document is not what it was: there is something to validate and
    // to write.
    bool changed = false;
    // DEC32 (PRD7) -- why the ingest operation was refused, when it was
    // (AddProbedMedia): a target bin the document does not have. Its
    // entries and moves are then not applied.
    std::string refused;
};

// DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. What an ingest of probed
// files changes, decided without changing anything.
//
// `operation` carries the new entries and the known ones filed into the
// target bin: an IngestMediaOperation (Operations.h), one undo step in the
// timeline's journal for whoever applies it through an EditLog. It carries
// the two repairs too (ARCHI-2026-11 AR2), which used to be written beside
// it: an entry without technical metadata takes the probed one
// (`completed`, same id, path, bin and proxy), and a complete entry without
// a source gets one (`mounted`). Undoing the import takes them back with
// the rest; the files' audio levels, measured beside the journal, stay.
struct IngestPlan {
    // Everything the ingest changes, repairs included (`completed`,
    // `mounted`): one journal entry, nothing written beside it.
    IngestMediaOperation operation;
    // As AddProbedMedia reports it, once the plan is applied.
    IngestOutcome outcome;

    // The operation changes something: there is a journal entry.
    bool HasJournalEntry() const {
        return !operation.added.empty() || !operation.filed.empty() ||
               !operation.completed.empty() || !operation.mounted.empty();
    }
};

IngestPlan PlanProbedMedia(const Document& document,
                           const std::filesystem::path& projectDirectory,
                           const std::vector<ProbedIngestFile>& files,
                           const std::optional<Ulid>& targetBin);

// PRD20 -- ROADMAP.md. Whether an ingest read nothing of what it was handed:
// no file became an entry, the library knew none of them, and at least one
// could not be read. That is a failure, and every surface says so in its
// own way: IngestCommand refuses (IoError, exit status 1, the report under
// `result`), which is also what ingest_media answers; the window counts the
// files it refused. A folder where only some files fail is not this: it
// stays a success that lists them.
bool IngestReadNothing(const IngestOutcome& outcome);

// The second half: adds probed files to `document`'s library and sources.
//
// A file the library does not hold becomes an entry with a new id, its path
// stored relative to `projectDirectory` when it can be, and a source. A file
// it holds is left alone, save for two things: an entry without technical
// metadata takes the probed one, keeping its id, path, bin and proxy, and a
// complete entry that had no source gets it.
//
// `targetBin` is where the caller files what it ingests: new entries go
// there, and known ones move there -- "import this into that bin" means the
// rush ends up in it, even when the project already knew it. Without one,
// new entries go to the root and known ones stay where they are. An empty
// id is the root. A bin the document does not have is not checked here:
// Document::Validate refuses the result.
//
// DEC32 (PRD7) -- the plan (PlanProbedMedia) applied at once, its
// operation through ApplyOperation and not a journal: for a caller that
// keeps no history of the document. The command line, MCP and the window
// apply the plan's operation through their EditLog instead, so that an
// import is undone like any edit.
IngestOutcome AddProbedMedia(Document& document,
                             const std::filesystem::path& projectDirectory,
                             const std::vector<ProbedIngestFile>& files,
                             const std::optional<Ulid>& targetBin);

// F2.4 -- ROADMAP.md. The steps of IngestCommand, for a caller whose
// document lives elsewhere: the app's window, which answers ingest_media to
// its Agent panel and to the agents relayed to it (McpLiveBackend.h). The
// files a path names and their reading run on a worker, AddProbedMedia where
// the document lives, and the answer is IngestCommand's own.

// The files an ingest of `mediaPath` looks at: the file itself, or those of
// the folder -- and of its subfolders when `recursive` -- sorted, what a Mac
// leaves on a volume passed over and counted in `ignored`. The path is kept
// as it was named, links included, for StoredMediaPath (LIEN1). False with
// the reason when it names nothing that can be read.
bool CollectIngestFiles(const std::string& mediaPath, bool recursive,
                        std::vector<std::filesystem::path>& files,
                        size_t& ignored, std::string& reason);

// The first half over every file, in order: `files` ends with one entry per
// candidate. Seed `settled` with CompleteLibraryPaths. Run as a task, with
// the task's `context`, each file is named in its progress before it is
// read -- a long rush is measured for seconds -- and a cancel is heard
// between two files, never inside one measurement: false then, the files
// after it left unread. The window's import and ingest_media run this one.
bool ProbeIngestFiles(const std::vector<std::filesystem::path>& candidates,
                      std::set<std::string> settled,
                      std::vector<ProbedIngestFile>& files,
                      MediaTaskContext* context = nullptr);

// The name of that task, in the task bar, for `count` files.
std::string IngestTaskLabel(size_t count);

// What IngestCommand answers once `outcome` is known, and committed when it
// changed the document: 0 and the report, or 1 and the refusal of an ingest
// that read nothing (PRD20), in the --ingest format (Cli.h). `ignored` as
// CollectIngestFiles counted it.
int IngestCommandAnswer(const IngestOutcome& outcome, size_t ignored,
                        std::string& output);

// Scans either one regular file or every file in a directory, then measures
// the mean audio level of every media that has an audio stream (see
// MeasureMediaAudioLevel). No renderer, AppKit, or Metal object is created by
// this command; the only decoding is that of the audio, through FFmpeg.
//
// Answers {"ok":true,"added":..,"skipped":..,"ignored":..,"errors":[..]},
// `errors` naming each file it could not read. When it read nothing
// (IngestReadNothing) it refuses instead, with those four members under
// `result` (PRD20).
//
// DEC32 (PRD7) -- `targetBin` files what it ingests, as AddProbedMedia says:
// new entries go there and known ones move there; unset, new entries go to
// the root and known ones stay. The new entries and the moves are one
// IngestMediaOperation in the active timeline's journal: `--undo` and the
// `undo` tool take an ingest back. A bin the project does not have is
// refused (UnknownBin) and nothing is written.
int IngestCommand(const std::string& documentPath, const std::string& mediaPath,
                  bool recursive, std::string& output,
                  const std::optional<Ulid>& targetBin = std::nullopt);
