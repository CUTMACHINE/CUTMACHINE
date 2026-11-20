#pragma once

#include "Document.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// QC-2026-09 (A6) -- a model cannot judge a cut one isolated read_frame at a
// time.  These plans make the editorial sampling deterministic and keep all
// time arithmetic in the engine; rendering is a read-only consequence.
enum class TimelineSheetKind { Contact, Cuts, Media };

struct TimelineSheetSettings {
    // Bounded because one MCP image must stay useful to a vision model.  When
    // there are more candidates, the engine samples the whole timeline at
    // even intervals instead of returning only its beginning.
    // PRD39 -- ROADMAP.md. The one default of every sheet -- contact, cuts
    // and rushes -- that the command line and the tools both read here:
    // --media-sheet used to give 12 cells where media_sheet gave 24 for
    // several rushes, and 12 for one, with no reason written for either.
    int32_t maximum_images = 24;
    int32_t columns = 4;
    // Long edge of a cell, which has the sequence's shape: 320 x 180 for a
    // 16:9 timeline, 180 x 320 for a 9:16 one.
    int32_t cell_dimension = 320;
    // Another shape for the cells than the sequence's, when not zero: a
    // sheet of rushes takes the rushes' own (BuildMediaSheetPlan).
    int32_t frame_width = 0;
    int32_t frame_height = 0;
    int32_t jpeg_quality = 6;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    // DEC29 -- docs/roadmap/GRADE-2026-09.md. Each cell shows its clip as
    // read_frame graded does -- the clip's grade and the sequence look,
    // through the SDR view Record uses -- instead of the source through the
    // document's display transform alone. Not for a sheet of rushes: a
    // rush has no grade until a clip places it.
    bool graded = false;
};

struct TimelineSheetFrame {
    Ulid clip_id;
    Ulid source_id;
    RationalTime timeline_position;
    RationalTime source_position;
    // "middle", "before" or "after".  A cut pair shares cut_position.
    std::string role;
    RationalTime cut_position{0, 1};
    bool has_cut_position = false;
    // PL4 -- docs/roadmap/PLANS-2026-10.md. On a sheet of rushes, the
    // described shot this frame falls in (ShotDescriptions.h); empty when
    // no description covers it yet.
    Ulid shot_id;
};

struct TimelineSheetPlan {
    TimelineSheetKind kind = TimelineSheetKind::Contact;
    int64_t total_candidates = 0;
    std::vector<TimelineSheetFrame> frames;
};

// Contact sheets sample the middle frame of visible shots. Cut sheets sample
// the last frame before and first frame after every visible hard cut. Higher
// visible video tracks occlude lower ones exactly as timeline_stats does.
bool BuildTimelineSheetPlan(const Document& document, TimelineSheetKind kind,
                            const TimelineSheetSettings& settings,
                            TimelineSheetPlan& output, std::string& error);

// A sheet of rushes rather than of an edit: the derush. One media gives
// `maximum_images` frames spread evenly across it, each the middle of an
// equal part of the take; several give the middle frame of each, sampled
// evenly past `maximum_images`. Nothing needs to be on a timeline -- on 24
// September 2026 ten DERUSH timelines were built only to look at a shoot.
// The cells take the first media's displayed shape (`settings` is updated).
// Fails on a media the document does not hold or that has no picture.
bool BuildMediaSheetPlan(const Document& document,
                         const std::vector<Ulid>& mediaIds,
                         TimelineSheetSettings& settings,
                         TimelineSheetPlan& output, std::string& error);

// PRD39 -- ROADMAP.md. Why a plan holds no cell, in words, or empty when it
// holds some. A timeline with no visible shot, or no visible cut, has
// nothing to show: every surface refuses it as ValidationFailed with this
// sentence before anything renders. It used to reach the render and come
// back as an IoError, which it is not.
std::string EmptyTimelineSheetReason(const TimelineSheetPlan& plan);

// Compact canonical metadata accompanies the JPEG over CLI and MCP so every
// cell remains addressable by ULID and exact RationalTime. DEC29 --
// `graded` is written, as true, only for a graded sheet, as read_frame
// says it.
std::string SerializeTimelineSheetPlan(const TimelineSheetPlan& plan,
                                       bool graded = false);

// Renders a single JPEG grid. Relative media paths are resolved against
// projectDirectory. The document's display colour transform is applied after
// composition, so log rushes are never handed to the caller untransformed.
// DEC29 -- with `settings.graded`, each cell is graded by the engine
// (RenderGradedSheetCells, ColorMeasure.h) and the grid is encoded as
// read_frame graded encodes its frame; refused for a sheet of rushes.
bool RenderTimelineSheet(const Document& document,
                         const std::filesystem::path& projectDirectory,
                         const TimelineSheetPlan& plan,
                         const TimelineSheetSettings& settings,
                         std::string& jpegBytes, std::string& error);
