#pragma once

// M5 -- docs/roadmap/MOTION-2026-09.md. Where an instance of a composition
// lands on the timeline, computed by the engine from an anchor the agent
// names: a clip or a transcribed passage (the name goes on the person
// speaking), or the start or the end of the film (a card, a logo). The agent
// never adds a time to a time (PHILOSOPHY.md principle 7): it says "on this
// clip, a second in" or "on span S12", and the engine answers the exact range
// and the track, or refuses by name.
//
// A passage's words start and end between frames. The instance covers every
// frame in which one of them is heard: it starts on the frame that contains
// the first word's start, the rule Timeline applies to every source frame,
// and, bounded by the passage, ends on the frame that contains its last word's
// end. Unbounded, it lasts as long as the composition, up to the end of the
// shot that plays the passage's start -- a name does not outlive the shot of
// the person it names.
//
// A shot is the anchor clip and the clips of the same source that follow it
// on its track without a gap: on a real interview cut in jump cuts, the
// first passage's clip lasted 45 frames and the person stayed on screen for
// the next ones.
//
// The track, when not named: the lowest video track above the anchor -- the
// clip's own track, or the bottom video track for the film's edges and for an
// anchor heard only on a sound track -- whose range is free. A motion clip is
// laid over the picture, never under it, and never over another clip: a taken
// range is refused, not rippled.
//
// Portable: no AppKit, no FFmpeg (tests/motion_templates_tests.cc).

#include "Document.h"

#include <optional>
#include <string>

enum class MotionAnchor { Clip, Span, Start, End };

struct MotionPlacementRequest {
    Ulid composition_id;
    MotionAnchor anchor = MotionAnchor::Clip;
    // With MotionAnchor::Clip: the clip whose head the instance starts on.
    Ulid clip_id;
    // With MotionAnchor::Span: where a transcribed passage starts on the
    // timeline and the source it was spoken in, as get_timeline_transcript
    // spans give them, and where the passage ends when it bounds the
    // instance.
    RationalTime span_in{0, 1};
    Ulid span_source_id;
    std::optional<RationalTime> span_end;
    // After the anchor: into the clip or the passage, or into the film for
    // Start. With End it must be zero (the instance ends on the film's last
    // frame).
    RationalTime offset{0, 1};
    // Omitted: the whole passage when span_end bounds it, else as much of
    // the composition as the anchor leaves room for -- up to the shot's end
    // for Clip and Span, up to the film's for Start and End.
    std::optional<RationalTime> duration;
    // Omitted: chosen as the header says.
    Ulid track_id;
};

struct MotionPlacement {
    Ulid track_id;
    RationalTime timeline_in{0, 1};
    RationalTime duration{0, 1};
    // The composition's own length, for the report.
    RationalTime composition_duration{0, 1};
};

// `errorName` is a name of the catalog: UnknownComposition, UnknownClip,
// UnknownTrack, InvalidTimelineIn (a range that is not whole frames or does
// not fit), or NoFreeTrack (nothing above the anchor is free there: add a
// video track).
bool ResolveMotionPlacement(const Document& document,
                            const MotionPlacementRequest& request,
                            MotionPlacement& placement, std::string& errorName,
                            std::string& error);
