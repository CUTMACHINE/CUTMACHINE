#pragma once

// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/interchange/src/edl.rs (MIT OR Apache-2.0, commit 09aaf9b): the
// CMX 3600 edit decision list, written for a timeline and its markers, and
// read back for its markers only.
//
// WHY AN EDL BESIDE THE XML. ResolveXmlExport.h is the measured return trip
// to Resolve; an EDL is the format every other conform, grading or online
// tool reads, and Resolve's own marker list ("Timeline Markers to EDL") is
// one. NOT VERIFIED IN RESOLVE: nothing this file writes or reads has been
// put in front of Resolve yet; the layout is CMX 3600 as FilmCraft writes it
// and as the format describes it, not a measurement.
//
// WHAT CHANGES FROM FILMCRAFT, by rule rather than taste:
//   - FilmCraft computes a speed in f64 and rounds the M2 rate to a tenth.
//     Here a speed stays the clip's exact fraction, and the M2 rate is
//     written only when speed * timecode base is a whole number of tenths of
//     a frame per second: 1/2 at 25 i/s is 012.5, 2/3 at 25 i/s has no
//     spelling and the clip is refused by name (PHILOSOPHY.md principle 4).
//   - FilmCraft rounds sub-frame positions and warns. A position, a length
//     or a source in-point that is not a whole frame is refused here.
//   - Every loss is counted and named (EdlLoss), the spirit of
//     ResolveXmlExport's audio_tracks_not_sent and grades_not_sent: an EDL
//     carries cuts, dissolves and constant speeds, and nothing else.
//
// ONE EDL PER VIDEO TRACK. CMX 3600 has one picture channel; a montage of
// several layers is several lists, written side by side (FilmCraft's
// export_edl_per_track): `<stem>_V1.edl`, `<stem>_V2.edl`, numbered from the
// bottom track up, as ResolveExport.h numbers layers. A hidden track, or one
// with no picture clip, keeps its number and writes no file, so that V<n> is
// the window's V<n>.
//
// THE TIMECODES. The document carries no start timecode for a rush (see
// kUnknownStartTimecode in ResolveXmlExport.h), so a source timecode is the
// media's own time counted from 00:00:00:00, and the report says so
// (`source_timecodes`). A rush whose camera stamped another start will not
// conform on timecode until that start is known. The record side starts at
// `start_timecode`, 01:00:00:00 by default, Resolve's default timeline start.
// A source must play at the sequence cadence: CMX counts both sides in one
// timecode base, and a 50 i/s rush in a 25 i/s list has no unambiguous
// spelling, so it is refused by name.
//
// DROP FRAME. 30000/1001 and 60000/1001 are written drop-frame (`;` before
// the frames, FCM: DROP FRAME), dropping 2 or 4 frame numbers each minute
// except every tenth; 24000/1001 has no drop-frame and is written on a base
// of 24. A list read back honours its FCM line and the separator of each
// timecode.
//
// THE REEL. Every rush is reel AX, the auxiliary source of file-based
// lists, named by a `* FROM CLIP NAME:` comment (and `* TO CLIP NAME:` on
// the incoming side of a dissolve) carrying its file name. A dissolve whose
// outgoing clip is retimed is refused: with every reel AX, two M2 lines in
// one event could not say which side each speaks for.

#include "Document.h"
#include "Operations.h"

#include <cstdint>
#include <string>
#include <vector>

// How a list counts frames: a nominal base (25, 24 for 23.976, 30 for
// 29.97) and whether it drops frame numbers.
struct EdlTimecodeFormat {
    int64_t base = 25;
    bool drop_frame = false;
};

inline constexpr const char* kEdlDefaultStartTimecode = "01:00:00:00";

// The format a sequence at `rate` writes: whole rates on their own base,
// 24000/1001 on 24 non-drop, 30000/1001 and 60000/1001 drop-frame. Any
// other cadence is refused by name.
bool EdlTimecodeFormatFor(const MediaRate& rate, EdlTimecodeFormat& format,
                          std::string& error);

// `frames` from 00:00:00:00 as HH:MM:SS:FF, or HH:MM:SS;FF drop-frame.
// Refused below zero and from 24 hours on.
bool FormatEdlTimecode(int64_t frames, const EdlTimecodeFormat& format,
                       std::string& text, std::string& error);

// The inverse. `;` or `,` before the frames reads drop-frame whatever
// `format` says; a frame number a drop-frame minute skips is refused, as is
// a drop-frame timecode on a base that has none.
bool ParseEdlTimecode(const std::string& text, const EdlTimecodeFormat& format,
                      int64_t& frames, std::string& error);

// What a list does not carry, named. `kind` is one of: "grade", "framing",
// "opacity", "speed_ramp" (written at its mean speed), "motion_clip",
// "multicam_angle", "disabled_clip" (out of the film, ClipEnabled.h),
// "audio_track", "caption_track", "hidden_track", "look_lut", "markers".
// `id` is the clip, track or sequence it is about.
struct EdlLoss {
    std::string kind;
    Ulid id;
};

struct EdlTrackFile {
    Ulid track_id;
    // 1 for the bottom video track, as the file is named.
    int32_t number = 1;
    std::string text;
    int32_t events = 0;
    int32_t clips = 0;
};

struct EdlExport {
    std::string name;
    MediaRate frame_rate{25, 1};
    EdlTimecodeFormat timecode;
    std::string start_timecode = kEdlDefaultStartTimecode;
    std::vector<EdlTrackFile> files;
    int32_t clips = 0;
    int32_t retimed_clips = 0;
    int32_t transitions = 0;
    std::vector<EdlLoss> losses;
};

struct EdlExportOptions {
    std::string start_timecode = kEdlDefaultStartTimecode;
};

// The lists of `document`'s video tracks. Refuses, by name: a cadence the
// format cannot count, a clip that does not sit on whole frames, a source on
// another cadence, a speed with no M2 spelling, a held frame, a dissolve
// touching a ramp or leaving a retimed clip, a dip to colour between two
// clips or to another colour than black, more than 999 events in a list,
// and a montage with no video clip. Pure: no filesystem.
bool BuildEdlExport(const Document& document, const EdlExportOptions& options,
                    EdlExport& exported, std::string& error);

// `<stem>_V<n>.edl` beside `path`, which must name a .edl file.
bool EdlTrackPaths(const std::string& path, const EdlExport& exported,
                   std::vector<std::string>& paths, std::string& error);

// Builds, writes every list or none (an existing destination is
// "OutputExists", like the XML), and answers the JSON report: files, counts
// and every loss. `errorName` is "InvalidExport" for a refused montage,
// "ValidationFailed" for a path that is not a .edl.
bool ExportEdlFiles(const Document& document, const std::string& path,
                    const EdlExportOptions& options, std::string& reportJson,
                    std::string& errorName, std::string& error);

// --- Markers ---------------------------------------------------------------

// "loc": one event per marker and a `* LOC: <tc> <COLOUR> <name>` comment,
// the extension FilmCraft and Avid read. "resolve": the line Resolve's own
// marker list writes under each event, ` |C:ResolveColorBlue |M:name |D:1`,
// which also carries a span's length. Both are read back.
enum class EdlMarkerStyle { Loc, Resolve };
bool ParseEdlMarkerStyle(const std::string& text, EdlMarkerStyle& style);

// A colour word and the `#rrggbb` the document reads it as. The LOC words
// are Avid's eight; Resolve's are its sixteen marker colours. Yellow and
// Blue read as the document's own default (#f5c542) and as the window's
// marker blue (#4da3ff), so the two colours a montage holds most come back
// as they left.
struct EdlMarkerColor {
    const char* word;
    const char* hex;
};
const std::vector<EdlMarkerColor>& EdlLocMarkerColors();
const std::vector<EdlMarkerColor>& EdlResolveMarkerColors();

struct EdlMarkerExport {
    std::string text;
    int32_t markers = 0;
    // Markers whose colour has no word of its own in the style, written as
    // the nearest one (by squared distance in RGB, ties to the table's
    // order).
    std::vector<Ulid> colors_approximated;
    // LOC only: spans written as points, the comment has no length.
    std::vector<Ulid> durations_not_sent;
};

// Refuses, by name, a marker that is not on a whole sequence frame, a name
// holding a line break (or a '|' in Resolve's style, its separator), a colour
// that is not `#rrggbb`, and a timeline with no marker.
bool BuildEdlMarkers(const Document& document, EdlMarkerStyle style,
                     const std::string& startTimecode,
                     EdlMarkerExport& exported, std::string& error);

bool ExportEdlMarkersFile(const Document& document, const std::string& path,
                          EdlMarkerStyle style,
                          const std::string& startTimecode,
                          std::string& reportJson, std::string& errorName,
                          std::string& error);

// One marker as a list spells it: a frame of the timeline (record timecode
// minus the start), a length in frames (0 for a point), the colour word as
// written and the document colour it reads as.
struct EdlMarkerEntry {
    int64_t frame = 0;
    int64_t duration_frames = 0;
    std::string color_word;
    std::string color;
    std::string name;
    size_t line = 0;
};

// Every `* LOC:` comment and every Resolve `|C:` line of `text`. The label
// of `startFrame` is read again in the count an FCM line sets: a non-drop
// list on a 29.97 timeline writes the same start label at another frame
// count. An unnamed marker takes kTimelineMarkerDefaultName, as the window
// names one. Refuses, by line: an unknown colour word (no default is
// guessed), a timecode that does not parse in `format` or falls before the
// start, an FCM line asking for drop-frame on a base that has none, an event
// whose record in does not parse, a `|C:` line under no event, a control
// character in a name, a list with no marker.
bool ParseEdlMarkers(const std::string& text, const EdlTimecodeFormat& format,
                     int64_t startFrame, std::vector<EdlMarkerEntry>& markers,
                     std::string& error);

// The markers of `text` as operations on `document`: one AddMarkerOperation
// each, category "note", grouped as one BatchOperation -- one undo step for
// the whole import -- when there are several. A marker the timeline already
// holds (same time, length, name and colour) is skipped and counted, so a
// list imported twice does not double. More than kMaxBatchOperations
// markers to add is refused rather than split over several undo steps.
struct EdlMarkerImport {
    std::vector<Operation> operations;
    std::vector<EdlMarkerEntry> added;
    size_t already_present = 0;
};
bool BuildEdlMarkerImport(const Document& document, const std::string& text,
                          const std::string& startTimecode,
                          EdlMarkerImport& imported, std::string& error);

// The single operation to apply: the AddMarker itself, or the batch.
Operation EdlMarkerImportOperation(EdlMarkerImport& imported);

// The JSON an import answers beside the applied edit.
std::string EdlMarkerImportReportJson(const EdlMarkerImport& imported,
                                      const std::string& path);

// Headless entry points, the shape of ExportResolveXmlCommand: an explicit
// timeline, or the active one when `timelineId` is empty.
int ExportEdlCommand(
    const std::string& projectPath, const std::string& path,
    std::string& output, const std::string& timelineId = {},
    const std::string& startTimecode = kEdlDefaultStartTimecode);
int ExportEdlMarkersCommand(
    const std::string& projectPath, const std::string& path,
    std::string& output, const std::string& timelineId = {},
    EdlMarkerStyle style = EdlMarkerStyle::Loc,
    const std::string& startTimecode = kEdlDefaultStartTimecode);
// Journaled: the import goes through ApplyOperationCommand, as --apply-op.
int ImportEdlMarkersCommand(
    const std::string& projectPath, const std::string& edlPath,
    std::string& output, const std::string& timelineId = {},
    const std::string& startTimecode = kEdlDefaultStartTimecode);
