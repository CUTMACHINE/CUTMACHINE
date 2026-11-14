#pragma once

// RESOLVE-2026-08 -- a DaVinci Resolve bin is not a file: it lives in the
// project database, and a .drp is an opaque archive. There is nothing to
// parse, so the only honest way in is to make Resolve talk. The Studio-only
// scripting API does that in `sidecar/resolve_bridge.py`, which flattens the
// Media Pool into the manifest this file consumes.
//
// The split matters: the bridge exports *identity and organisation only*
// (path, name, bin), never technical metadata. Resolve reports FPS as a
// float, and PHILOSOPHY.md principle 4 forbids a float from becoming a rate.
// FFmpeg stays the single source of truth for cadence and duration, through
// the same ProbeMediaMetadata the directory ingest already uses -- so a rush
// imported from Resolve is byte-identical to the same rush ingested from
// disk, and no import path can invent a time value.
//
// Parsing and planning are pure and testable without FFmpeg, a project on
// disk, or Resolve running (tests/resolve_import_tests.cc). Only
// ImportResolveCommand touches any of the three.

#include "Document.h"
#include "Operations.h"
#include "ResolveGrade.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

inline constexpr char kResolveManifestSchema[] =
    "cutmachine.resolve-manifest.v1";

struct ResolveManifestBin {
    // Opaque intra-manifest reference. Never stored: bins are addressed by
    // ULID once imported, and the manifest is not a document.
    std::string key;
    std::string name;
    // Empty designates the project root, which has no bin object.
    std::string parent_key;
};

struct ResolveManifestClip {
    std::string path;
    std::string name;
    std::string bin_key;
    // Resolve's own clip identifier, carried for diagnostics only. Media
    // identity on this side is the resolved file path, as in Ingest.cc.
    std::string resolve_uid;
};

struct ResolveManifest {
    std::string project;
    std::string resolve_version;
    std::vector<ResolveManifestBin> bins;
    std::vector<ResolveManifestClip> clips;
};

// Strict: rejects an unknown schema, a duplicate or empty bin key, an unknown
// parent, a cycle, an empty clip path, and a clip in an unknown bin. Every
// failure names what it refused.
bool ParseResolveManifest(const std::string& json, ResolveManifest& manifest,
                          std::string& error);

struct ResolveImportPlan {
    // Bins to create, parents first, with their ULIDs already generated.
    std::vector<AddBinOperation> new_bins;
    // Every manifest key, including "" for the root, mapped to its ULID.
    std::map<std::string, Ulid> bin_ids;
    // Manifest bins already present in the document under the same name and
    // parent. Re-importing the same manifest creates nothing.
    size_t reused_bins = 0;
};

// Matches manifest bins onto the document's existing bins by (name, parent)
// so a second import of a growing Media Pool is additive, not duplicating.
bool PlanResolveImport(const Document& document,
                       const ResolveManifest& manifest, ResolveImportPlan& plan,
                       std::string& error);

// Headless entry point: creates the bins through AddBinOperation, reads each
// new clip as an ingest reads it (ProbeIngestFile: headers and audio level),
// adds the new ones in their bins through one IngestMediaOperation, files
// the known ones with SetMediaBinOperation, and commits document, project
// and timeline log together. DEC32 (PRD7) -- every step is journaled and
// undoable now: the library insertion was the one that was not, and the
// level the one thing an ingest measured and this did not.
int ImportResolveCommand(const std::string& projectPath,
                         const std::string& manifestPath, std::string& output);

// ---------------------------------------------------------------------------
// R10 -- docs/roadmap/REEL-2026-09.md. Reading a Resolve *timeline* back into
// the document, and not only the Media Pool above.
//
// Same split, for the same reason: the bridge records what Resolve says
// (sidecar/resolve_bridge.py, `--read-timeline`), and everything below is
// pure C++ that parses, plans and refuses without Resolve, FFmpeg or a
// project on disk. The manifest carries no floating-point rate and no
// floating-point time -- whole frames only -- because FFmpeg stays the single
// source of truth for cadence and duration, exactly as for the media
// manifest, and `integral_rate` already refuses a non-integer cadence.
//
// Route: the scripting API (`GetItemListInTrack`), not the `.drt`. It is the
// route the bridge already uses, it keeps Resolve's undocumented project
// format out of the repository, and it leaves this file testable. The `.drt`
// stays open for the one thing the API does not publish: the keyframes of a
// framing. A speed and a transition both do come across -- measured on
// 20.3.1, 22 September 2026 -- and a speed the bridge could not measure is
// still *refused by name* rather than guessed: reading a retimed clip as a
// clip at speed 1 would move every frame of its source range without saying
// so.
//
// What this never does: add a media. A clip whose file is unknown to the
// library is refused by name, and `--import-resolve` is what brings the
// Media Pool in first. So a pre-selection of 130 extracts taken from a
// handful of rushes cannot duplicate a media -- there is no code path that
// could create one.

inline constexpr char kResolveTimelineManifestSchema[] =
    "cutmachine.resolve-timeline-read.v1";

// Item properties whose default value proves a clip carries no framing, no
// compositing and no speed. The bridge holds the same table and records
// exactly these keys; a key it sends that is missing here is refused rather
// than ignored, so the two sides cannot drift apart in silence.
//
// Values travel as text, never as a JSON float: comparison is exact decimal
// text against the whole number below ("1.000" is 1, "0.5" is not).
//
// One measured blind spot (Resolve 20.3, REEL-2026-09): GetProperty returns
// 0 on an *animated* parameter, so an animated zoom lands here as ZoomX=0 and
// is refused because 0 is not the default 1, while an animated Pan or Tilt
// reads 0, which *is* its default, and passes unseen. That is what the `.drt`
// route would close.
struct ResolveItemPropertyDefault {
    const char* key;
    int64_t value;
};

// ZoomX, ZoomY, Pan, Tilt, RotationAngle, AnchorPointX, AnchorPointY,
// CropLeft, CropRight, CropTop, CropBottom, FlipX, FlipY, Opacity,
// CompositeMode, Dynamic Zoom.
//
// A "Speed" key is deliberately absent: nothing in this project has measured
// whether Resolve publishes one, nor whether it would be a ratio or a
// percentage, and a wrong default here would refuse every clip of every
// timeline. The speed is handled by the cross-check on the item duration
// below, and by the refusal to pretend it was seen.
const std::vector<ResolveItemPropertyDefault>& ResolveItemPropertyDefaults();

struct ResolveTimelineItem {
    std::string name;
    // Whole frames of the sequence, counted from the timeline's own start
    // frame: the bridge subtracts Resolve's origin (90000 at 25 i/s for a
    // 01:00:00:00 start) so nothing downstream has to know that convention.
    // Half-open, like every range in this engine.
    int64_t record_start = 0;
    int64_t record_end = 0;
    // GetDuration() as Resolve reports it. Cross-check only: the API does not
    // say whether it counts sequence frames or source frames, so the planner
    // accepts either reading and refuses a value that is neither.
    int64_t duration = 0;
    // GetSourceStartFrame(), in the source's own frame domain -- the same
    // domain ResolveExport's clipInfo uses. GetSourceEndFrame is deliberately
    // absent, as in `flatten_timeline`: measured, it disagrees with the item
    // duration by one frame on some clips and two on others.
    int64_t source_start = 0;
    int64_t fusion_comps = 0;
    // R10 -- the four integers an exact speed is derived from. Measured on
    // Resolve 20.3.1: GetLeftOffset and GetRightOffset are in *timeline*
    // frames and already carry the speed, so left + (record_end -
    // record_start) + right is the whole media as this clip plays it, while
    // media_frames * timeline_rate / media_rate is what it would span at
    // speed 1. The ratio is the speed, exactly, from integers alone.
    //
    // `speed_unmeasurable` is set instead when the bridge could not gather
    // them (a build without the offsets, a media with a non-integer cadence).
    // A clip whose speed cannot be measured is refused by name rather than
    // read as normal -- reading a retimed clip as a clip at speed 1 moves
    // every frame of its source range without saying so.
    int64_t left_offset = 0;
    int64_t right_offset = 0;
    int64_t media_frames = 0;
    int32_t media_rate = 0;
    std::string speed_unmeasurable;
    std::string path;
    // Set when `path` is empty: what Resolve calls the thing that has no file
    // (compound clip, nested timeline, generator, title).
    std::string no_file_reason;
    std::vector<std::pair<std::string, std::string>> properties;
};

// R10 -- one transition item, as the bridge relève it. GetItemListInTrack
// publishes these among the clips, named after their kind, carrying no media
// and *overlapping* the two clips they join: that overlap is how Resolve lays
// a dissolve, so it is never a refusal here. An overlap of two *clips* still
// is, and the two never meet, because the bridge keeps them in separate
// lists.
//
// No alignment travels: the API publishes none. It is read from where these
// bounds sit around the cut the two clips share, which only the planner can
// do -- it is the side that holds the clips.
struct ResolveTimelineTransition {
    // Resolve's own name for the kind: "Cross Dissolve", "Dip to Color
    // Dissolve", "Cross Fade -3dB"... Anything but a cross dissolve is
    // refused by this name: R5 gave the document a colour dip, but what
    // Resolve reports for its own dip -- the effect's name and where its
    // colour lives -- has not been measured.
    std::string name;
    // Whole frames of the sequence, from the timeline's own start frame,
    // half-open, exactly like a clip's.
    int64_t record_start = 0;
    int64_t record_end = 0;
    // GetDuration() as Resolve reports it. Carried, unused: the bounds are
    // the fact, and on the three measured dissolves the two agree.
    int64_t duration = 0;
};

struct ResolveTimelineTrackRead {
    // "video" or "audio".
    std::string kind;
    // Resolve's own 1-based track index, kept as it was read. The document's
    // own index is assigned by the plan, which shares one index space across
    // kinds (ApplyAddTrack refuses a duplicate index whatever the kind).
    int32_t index = 0;
    std::string name;
    std::vector<ResolveTimelineItem> clips;
    std::vector<ResolveTimelineTransition> transitions;
};

struct ResolveTimelineRead {
    std::string project;
    std::string resolve_version;
    std::string name;
    // Whole frames per second. A non-integer cadence never reaches here:
    // `integral_rate` refuses it in the bridge.
    int32_t frame_rate = 0;
    int64_t origin = 0;
    int32_t width = 0;
    int32_t height = 0;
    // What the API does not publish for this timeline, in the bridge's own
    // words. Reported as-is so the relecture says what it could not see.
    std::vector<std::string> unpublished;
    std::vector<ResolveTimelineTrackRead> tracks;
};

// One thing the relecture will not carry, named where it sits. A count is
// not a report: on a 113-clip montage, "refused: 7" leaves the editor to
// find them by hand.
struct ResolveTimelineRefusal {
    // "V1", "A2"... as Resolve numbers them.
    std::string track;
    // The clip's name, or -- for a refused transition -- Resolve's own name
    // for its kind, so the editor reads back the words the Resolve interface
    // shows them.
    std::string clip;
    std::string reason;
};

struct ResolveTimelineImportPlan {
    UpdateSequenceOperation sequence;
    // Tracks the document is missing, in the order they must be created.
    std::vector<AddTrackOperation> new_tracks;
    size_t reused_tracks = 0;
    // In application order: track by track, and within a track by ascending
    // position, so each insert lands past the previous one and ripples
    // nothing.
    std::vector<InsertClipOperation> clips;
    // R10 -- the cross dissolves, applied after every clip: a transition
    // names its two clips by id, and those ids exist only because the plan
    // generates them itself (EditLog::Apply takes its operation by value, so
    // an id it generates never comes back).
    std::vector<AddTransitionOperation> transitions;
    std::vector<ResolveTimelineRefusal> refusals;
    // Parallel to `clips`: the speed each one carries, {1, 1} for most.
    // Applied by a SetClipSpeed after the insert, the clip id being known
    // only once the insert has been applied.
    std::vector<EffectParamValue> clip_speeds;
    // How many of them are not at speed 1. Reported on every success: a
    // montage that is one third slow motion is worth saying out loud.
    size_t retimed_clips = 0;
    std::vector<std::string> unpublished;
    // DEC15 (GR15) -- parallel to `clips`: the manifest track and the item
    // of that track (indices into ResolveTimelineRead::tracks and the
    // track's `clips`, in the manifest's order) each clip comes from, so
    // that what Resolve graded on an item reaches the clip id the document
    // gives it.
    struct ClipOrigin {
        size_t track = 0;
        size_t item = 0;
    };
    std::vector<ClipOrigin> clip_origins;
};

// Strict: rejects an unknown schema, a missing or non-integer cadence or
// resolution, an empty or duplicated track, a negative or empty range, and
// any property value that is not exact decimal text. Every failure names
// what it refused.
bool ParseResolveTimelineManifest(const std::string& json,
                                  ResolveTimelineRead& manifest,
                                  std::string& error);

// Pure. `projectRoot` is only used lexically, to resolve the library's
// relative paths against; nothing here touches the filesystem.
//
// Returns false as soon as anything is not expressible, with `error` naming
// every refusal and `plan.refusals` carrying them structured -- so the caller
// writes nothing at all. A gap between two clips is *not* a refusal:
// `timeline_in` is absolute, so a hole costs the document nothing.
// `flatten_timeline` refuses one because a flat render needs contiguity;
// this does not. Neither is a transition overlapping its two clips: that is
// how Resolve lays one. An overlap of two *clips* is still refused.
//
// R10 -- transitions. A cross dissolve straddling one cut between two kept
// clips becomes an AddTransitionOperation, its alignment read from where its
// bounds sit around that cut. R5 -- one flush against the head or the tail
// of a single clip is Resolve's fade from or to black, and becomes a black
// "dip_to_color" on that clip; that mapping is read from the bounds alone
// and has not been put back in front of Resolve. Everything else is refused
// by name, with Resolve's own name for it: another kind, a transition on an
// audio track, one straddling no cut or several while sitting flush against
// nothing, one split around the cut in a ratio the document cannot express,
// and one joining a clip whose speed is not 1/1 (refused by
// Document::Validate since R3, refused here by name first).
bool PlanResolveTimelineImport(const Document& document,
                               const std::string& projectRoot,
                               const ResolveTimelineRead& manifest,
                               ResolveTimelineImportPlan& plan,
                               std::string& error);

// One French line per refusal, joined -- what the CLI puts in `detail`.
std::string DescribeResolveTimelineRefusals(
    const std::vector<ResolveTimelineRefusal>& refusals);

// ---------------------------------------------------------------------------
// DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. What was graded on
// the timeline being imported, read from a copy of the Resolve project's
// database (ResolveGrade.h), clip by clip, and handed to the agent in the
// answer of the import, beside the cut. A reading: the document gains
// nothing from it, the journal neither.

struct ResolveClipGrade {
    // The id the plan gives the clip in the document.
    Ulid clip_id;
    // "V1"... and Resolve's name for the clip, as a refusal names them.
    std::string track;
    std::string clip;
    ResolveGrade grade;
    // GR16 -- the grade as the stages that render it (ResolveGradeStages),
    // which the import writes on the clip; or, in `unrendered`, why it
    // writes nothing.
    std::vector<ClipEffect> stages;
    std::vector<std::string> unrendered;
};

struct ResolveGradeReading {
    std::string database;
    bool timeline_graded = false;
    ResolveGrade timeline_grade;
    // GR16 -- the timeline's own grade has nowhere to go in a document: a
    // stack belongs to a clip. Said here when there is one.
    std::string timeline_unrendered;
    // The project settings a contrast depends on, as read.
    ResolveProjectSettings settings;
    // The clips with something to say about their grade, in the plan's
    // order; a clip whose grade holds nothing is only counted.
    std::vector<ResolveClipGrade> clips;
    size_t ungraded = 0;
    // A clip of the import the database has no item for at its place --
    // the copy is older than the timeline, or the item reads another file.
    // Named, never guessed at; the cut is imported all the same.
    std::vector<ResolveTimelineRefusal> unmatched;
};

// Pure. Each clip the plan lays on a video track is matched to the item of
// the database on the same track (V1 is V1), at the same place -- the
// manifest's origin plus its record start, for as many frames -- and
// reading the same file. The timeline is the one of the manifest's name;
// when the database holds several, the one every video clip matches. False,
// with `error`, when no timeline of that name is in the database, or when
// several are and none or more than one matches the whole cut: which one
// was graded cannot be told then, and nothing is guessed.
bool MatchResolveTimelineGrades(
    const ResolveTimelineRead& manifest, const ResolveTimelineImportPlan& plan,
    const std::vector<ResolveDatabaseTimeline>& timelines,
    ResolveGradeReading& reading, std::string& error,
    const ResolveProjectSettings& settings = {});

// The reading as the `grades` object of the import's answer.
std::string SerializeResolveGradeReading(const ResolveGradeReading& reading);

// GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md (DEC46). One Resolve grade
// as the color.resolve_primaries stages that render it, one per enabled node
// in the order the chain applies them, the stored floats written as exact
// fractions (their shortest decimal). A grade is rendered whole or not at
// all: when the reading named something it cannot read, or a node holds a
// setting the stage does not render yet (a hue rotation, the Log wheels,
// Color Boost, Midtone Detail, highlights or shadows away from neutral),
// `stages` comes back empty and `unrendered` says why, in French. A
// half-rendered grade would be a picture Resolve never showed. A grade whose
// nodes are all neutral gives no stage and no reason.
//
// The project's settings (`settings`, ReadResolveDatabaseTimelines) decide
// the rest. A saturation, a contrast, a temperature or a tint away from
// neutral renders only when they were read and the colour science is one it
// was measured in (ResolveScienceRendersBalance); the contrast also follows
// the aware tools, the S-curve and the working luminance. In a colour
// science where even the lift, gamma and gain were never measured, no node
// renders.
void ResolveGradeStages(const ResolveGrade& grade,
                        std::vector<ClipEffect>& stages,
                        std::vector<std::string>& unrendered,
                        const ResolveProjectSettings& settings = {});

// ---------------------------------------------------------------------------
// F2.4 (R10) -- ImportResolveTimelineCommand in three steps, shared with the
// window's live backend (McpLiveBackend) the way Ingest.h shares
// IngestCommand's: the import failed from the Agent panel because the
// command loads and commits the package itself, while the window holds that
// project, its lock and its journals. The window reads the inputs off the
// thread that owns its document, then plans and applies on it, against its
// own document and journal; each step writes into `output` exactly what the
// command would, so that both answer the same bytes.

class EditLog;

struct ResolveTimelineImportInput {
    ResolveTimelineRead manifest;
    // DEC15 (GR15) -- the Resolve database asked for, empty when none, and
    // the timelines of the manifest's name read from its copy.
    std::string database;
    std::vector<ResolveDatabaseTimeline> database_timelines;
    // GR16 -- the project's settings, from the same copy.
    ResolveProjectSettings settings;
};

struct ResolveTimelineImportStaged {
    ResolveTimelineImportPlan plan;
    ResolveGradeReading grades;
};

// Reads and parses the manifest and, when `resolveDatabase` is not empty,
// the grades of its database. Touches no project. 0, or the command's
// refusal in `output` and its status.
int ReadResolveTimelineImportInput(const std::string& manifestPath,
                                   const std::string& resolveDatabase,
                                   ResolveTimelineImportInput& input,
                                   std::string& output);

// Plans the import into `document` (PlanResolveTimelineImport) and matches
// the grades (MatchResolveTimelineGrades). Pure; nothing is applied.
int PlanResolveTimelineImportStep(const Document& document,
                                  const std::string& projectRoot,
                                  const ResolveTimelineImportInput& input,
                                  ResolveTimelineImportStaged& staged,
                                  std::string& output);

// Applies the plan to `document` through `log` -- UpdateSequence, AddTrack,
// InsertClip with SetClipSpeed, AddTransition, and since GR16 one
// SetClipEffects per clip whose Resolve grade renders -- validates the
// result, and
// writes the command's answer into `output`. On a refusal the document and
// the log may hold part of the import: the caller applies to copies it can
// drop, as the command applies to a document it has not committed.
int ApplyResolveTimelineImportStep(Document& document, EditLog& log,
                                   const ResolveTimelineImportInput& input,
                                   const ResolveTimelineImportStaged& staged,
                                   std::string& output);

// Headless entry point: plans, then applies AddTrack, UpdateSequence,
// InsertClip and AddTransition through EditLog::Apply, and commits document,
// project and timeline log together. No new operation and no new document
// field: R10 holds entirely inside what the document already carries.
// PRD17 -- ROADMAP.md. `timelineId` names the timeline read into; empty,
// the active one, as `--import-resolve-timeline` has it. An agent of a
// series names its own.
// DEC15 (GR15) -- `resolveDatabase`, when given, is the Resolve project's
// Project.db: it is copied, its grades read (MatchResolveTimelineGrades),
// and the answer carries them in `grades`. A database that cannot be read,
// or that does not hold the timeline, refuses the import before anything
// is written: the call asked for both. GR16 -- every grade that renders
// whole is written on its clip as color.resolve_primaries stages, in the
// same journal as the cut; the others are named in `grades`.
int ImportResolveTimelineCommand(const std::string& projectPath,
                                 const std::string& manifestPath,
                                 std::string& output,
                                 const std::string& timelineId = {},
                                 const std::string& resolveDatabase = {});
