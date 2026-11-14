#pragma once

// MOTION-2026-09 M4 -- ROADMAP.md, docs/MOTION_FORMAT.md. Motion clips: clips
// of a timeline whose source is a composition of the project rather than a
// media (Document.h). What such a clip is in the engine's terms is decided
// once, here, and every consumer asks: the operations bound its range with
// it, Timeline resolves its frame with it, the export and the monitor render
// what it names, the surfaces describe it.
//
// A composition behaves as a source of its own: its duration in frames at
// its own cadence bounds the clip, as a media's duration bounds a media
// clip, and the clip's source_in is a time in the composition. A head trim
// therefore advances the composition, a slip slides it under the clip, a
// cut gives the right half the time the cut left it at. The frame shown at a
// time is the frame that contains it, the rule Timeline applies to every
// media frame (RationalTime::to_frames): a composition at another cadence
// than the sequence holds or skips frames exactly as a media would.
//
// Portable: no framework. Rendering needs Core Text and Core Graphics, which
// the executable installs behind MotionClipRenderer.h.

#include "Document.h"
#include "Json.h"
#include "MotionComposition.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The composition seen as a source: its id, no path, its cadence as the
// rate and its duration as a time at that cadence. False, with the motion
// parser's refusal, when the definition does not parse.
bool CompositionAsSource(const DocumentComposition& composition,
                         DocumentSource& source, MotionError& error);

// The same, for a composition already parsed.
DocumentSource CompositionAsSource(const Ulid& compositionId,
                                   const MotionComposition& composition);

// What a clip's range lives in: its media source, or for a motion clip its
// composition as a source. Empty when it names neither: a caption clip, or
// a reference Document::Validate refuses.
std::optional<DocumentSource> ClipSource(const Document& document,
                                         const DocumentClip& clip);

// Instance props against the composition's typed props: null, or a
// non-empty object of declared props whose values have their prop's type
// and bounds. The refusals are M3's: UnknownProp, PropTypeMismatch (a group
// included), PropOutOfRange, each at motion_props.<name>.
bool ValidateMotionInstanceProps(const MotionComposition& composition,
                                 const mcp_json::Value& props,
                                 MotionError& error);

// Document::Validate's part for a clip that names a composition or instance
// props: a composition of the project, no media, a video track, no colour
// effects, valid props and a range inside the composition. `composition` is
// the parsed composition the clip names, or null when there is none.
bool ValidateMotionClip(const DocumentTrack& track, const DocumentClip& clip,
                        const DocumentComposition* stored,
                        const MotionComposition* composition,
                        std::string& error);

// The composition frame shown at `sourceTime`, a time in the composition
// (source_in plus the offset into the clip): the frame that contains it.
int64_t MotionClipFrame(const MotionComposition& composition,
                        const RationalTime& sourceTime);

// The composition frame of each of `frameCount` consecutive frames at
// `rate`, the first at `sourceIn`: what an export at that cadence shows.
std::vector<int64_t> MotionClipFrames(const MotionComposition& composition,
                                      const RationalTime& sourceIn,
                                      int64_t frameCount, MediaRate rate);

// What a motion clip shows at `timelineTime`, a time of its timeline: its
// composition, its instance props and the composition frame, the same
// frame Timeline resolves and the export renders. Refused, with an error
// name of the catalog, when the clip is not a motion clip
// (InvalidOperation), when the time falls outside it (InvalidTimelineIn),
// or when its composition does not parse (InvalidMotion).
struct MotionClipInstant {
    const DocumentClip* clip = nullptr;
    const DocumentComposition* composition = nullptr;
    MotionComposition parsed;
    RationalTime source_time;
    int64_t frame = 0;
};

bool MotionClipAt(const Document& document, const Ulid& clipId,
                  const RationalTime& timelineTime, MotionClipInstant& instant,
                  std::string& errorName, std::string& message);

// `current` updated with `changes`, an object whose null values remove an
// instance prop: existing keys keep their place, new ones follow. Null when
// nothing remains. What a surface builds SetClipMotionProps from when it is
// asked to set some props, not all.
mcp_json::Value MergeMotionProps(const mcp_json::Value& current,
                                 const mcp_json::Value& changes);

// A stable text for what makes a motion clip's pictures: the stored
// definition, the instance props and the output size. Two clips with the
// same key show the same picture at the same composition frame; an edit of
// the composition or of the props changes it. The monitor's cache and the
// export's reuse of identical frames key on it (never persisted).
std::string MotionClipPictureKey(const DocumentComposition& composition,
                                 const mcp_json::Value& props, int32_t width,
                                 int32_t height);

// REMOTION-PARITY-2026-09 REM2 -- the same, with what the composition
// reaches in its project joined in (MotionReachKey): an edit of a
// precomposition it shows, or a relink of a media it plays, changes it.
std::string MotionClipPictureKey(const DocumentComposition& composition,
                                 const mcp_json::Value& props, int32_t width,
                                 int32_t height, const std::string& reach);
