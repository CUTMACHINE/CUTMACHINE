#pragma once

// PA1 -- docs/roadmap/PARITE-2026-11.md. A timeline written in the two
// formats the other editors read: OpenTimelineIO (.otio, a JSON file) and
// Final Cut Pro XML (.fcpxml, version 1.10).
//
// WHY THESE TWO, after ResolveExport.h argued against interchange formats
// and ResolveXmlExport.h took one only because it was the measured way a
// speed reaches Resolve. That argument holds for Resolve, where the bridge
// knows the Media Pool. It says nothing to someone who finishes in Final
// Cut, Premiere, Kdenlive or Avid, and until now a montage could leave for
// those only as a CMX 3600 list: one video track per file, no sound, no
// marker beside the cuts. ROADMAP.md kept the generic output as its
// "Interop" line "if the need is confirmed"; the survey of 11 November 2026
// found FCPXML written by ten of the editors an agent drives and OTIO by
// four. This file is that line.
//
// NOT VERIFIED IN ANY EDITOR. Nothing this file writes has been opened in
// Final Cut, Resolve, Premiere or Kdenlive, nor read back by the
// OpenTimelineIO library: the layouts are the formats as their
// documentation describes them. Every report says so
// (`verified_in_editor: false`) until someone measures.
//
// WHAT BOTH CARRY that the EDL and the Resolve XML do not: every video and
// audio track in one file, the gaps between clips, the timeline's markers,
// a disabled clip as a disabled clip. And what neither refuses: a position
// that is not on a whole frame. Both formats spell a time as a rational --
// OTIO a value at a rate, FCPXML a fraction of a second -- so a time is
// written as what it is, never rounded and never refused. In OTIO a time
// that is a whole number of frames is written in frames (the sequence's for
// the record side, the media's for the source side), which is what an
// importer expects; one that is not is written in its own rate and counted
// (`off_frame_times`).
//
// THE SPEED. A clip at constant speed s shows `duration * s` of its source.
//   - OTIO: `source_range` starts at the source in-point and lasts the
//     clip's length ON THE TIMELINE; a LinearTimeWarp effect carries
//     `time_scalar` s. This is the convention of the library's own FCP7
//     adapter and of the files Resolve exports, not a rule the schema
//     states. A held frame is a FreezeFrame effect.
//   - FCPXML: a <timeMap> of two points maps the clip's local time to the
//     source, and `start` is then counted in that local, retimed time
//     (source in-point divided by s). A held frame maps both points to the
//     same source time.
//   A ramp is written at its mean speed, the cut points exact, and counted
//   (`speed_ramp`), as Edl.h does.
//
// THE TRANSITIONS. OTIO places a Transition between two items of a track,
// with the two clips keeping their own, non-overlapping ranges -- exactly
// the document's model -- so a cross dissolve and an audio cross fade go
// through, their alignment as `in_offset` and `out_offset`. A dip to
// colour, and a fade at the edge of a track, have no spelling there and are
// counted (`transition`). FCPXML allows a transition only inside a
// storyline, and this file writes every clip connected to one gap (below),
// so every transition is counted there.
//
// THE FCPXML LAYOUT. One <gap> the length of the timeline in the spine, and
// every clip an <asset-clip> connected to it: `lane` 1, 2... for the video
// tracks from the bottom up, -1, -2... for the audio tracks, `offset` the
// position on the timeline. It is the plain reading of a track-based
// montage in a format built around a magnetic storyline: nothing ripples on
// import, and a lane is the track it was. A video-track clip plays picture
// only and an audio-track clip sound only (`srcEnable`), the document's own
// rule since B11.
//
// WHAT STAYS BEHIND, counted and named by clip, track or sequence in
// `losses`: "grade", "framing", "opacity", "blur", "mask", "audio_gain",
// "audio_fade", "speed_ramp" (written at its mean speed), "motion_clip"
// (its place left empty), "caption_track", "look_lut", "transition" (see
// above). A hidden multicam angle and the clips of a hidden or muted track
// travel disabled rather than being left out, and are counted
// ("multicam_angle", "hidden_track").

#include "Document.h"

#include <cstdint>
#include <string>
#include <vector>

struct InterchangeLoss {
    std::string kind;
    Ulid id;
};

// A rush, once per media the montage reads.
struct InterchangeMedia {
    Ulid id;
    std::string path;
    std::string filename;
    bool has_video = true;
    bool has_audio = false;
    int32_t width = 0;
    int32_t height = 0;
    // {0, 1} for a media with no picture.
    MediaRate rate{0, 1};
    RationalTime duration{0, 1};
    int32_t audio_rate = 0;
    int32_t audio_channels = 0;
};

struct InterchangeClip {
    Ulid clip_id;
    size_t media = 0;
    RationalTime timeline_in{0, 1};
    RationalTime duration{0, 1};
    RationalTime source_in{0, 1};
    // The constant speed, or a ramp's mean; {0, 1} holds a frame.
    EffectParamValue speed{1, 1};
    bool ramp = false;
    // False for a disabled clip, a hidden multicam angle, and every clip of
    // a hidden or muted track.
    bool enabled = true;
};

struct InterchangeTransition {
    Ulid id;
    // "cross_dissolve" or "cross_fade": the only two that travel.
    std::string type;
    Ulid left_clip_id;
    Ulid right_clip_id;
    // How much of the outgoing clip's tail and of the incoming clip's head
    // the transition covers, on the timeline.
    RationalTime before_cut{0, 1};
    RationalTime after_cut{0, 1};
};

struct InterchangeTrack {
    Ulid id;
    bool video = true;
    // 1 for the bottom track of its kind, counting up, as the window names
    // V1 and A1.
    int32_t number = 1;
    bool enabled = true;
    // In increasing timeline_in.
    std::vector<InterchangeClip> clips;
    std::vector<InterchangeTransition> transitions;
};

struct InterchangeMarker {
    Ulid id;
    std::string name;
    std::string color;
    std::string category;
    RationalTime time{0, 1};
    RationalTime duration{0, 1};
};

struct InterchangeTimeline {
    Ulid id;
    std::string name;
    MediaRate frame_rate{25, 1};
    int32_t width = 0;
    int32_t height = 0;
    // The end of the last clip, on any track.
    RationalTime duration{0, 1};
    std::vector<InterchangeMedia> media;
    // Video tracks from the bottom up, then audio tracks from the top down
    // (A1 first).
    std::vector<InterchangeTrack> tracks;
    std::vector<InterchangeMarker> markers;
    std::vector<InterchangeLoss> losses;
};

// The montage of `document` as both writers read it. Refuses, by name: a
// sequence with no usable cadence, a clip whose media the library does not
// hold, a ramp whose source span has no exact spelling, and a montage with
// no clip. Pure: no filesystem.
bool BuildInterchangeTimeline(const Document& document,
                              InterchangeTimeline& timeline,
                              std::string& error);

struct OtioExport {
    std::string text;
    // Times written in their own rate because they sit inside a frame.
    int32_t off_frame_times = 0;
    // Markers whose colour is not one of OTIO's eleven, written as the
    // nearest.
    std::vector<Ulid> colors_approximated;
    // Transitions an OTIO track cannot place (a dip to colour, an edge
    // fade, two clips that do not touch), by id.
    std::vector<Ulid> transitions_not_sent;
};

// The .otio document: a Timeline.1 holding a Stack.1 of Track.1. Pure text.
OtioExport WriteOtio(const InterchangeTimeline& timeline);

struct FcpxmlExport {
    std::string text;
    // Every transition of the montage: see the header.
    std::vector<Ulid> transitions_not_sent;
};

// The .fcpxml document, version 1.10. Pure text.
FcpxmlExport WriteFcpxml(const InterchangeTimeline& timeline);

// An exact fraction as a decimal of at most `digits` places, truncated,
// never through a double: the spelling of a cadence and of a speed in a
// JSON file. "25", "29.970029970029", "0.5".
std::string FormatInterchangeDecimal(int64_t num, int64_t den, int digits = 12);

// A time as FCPXML spells one: a reduced fraction of a second, "0s", "5s",
// "1001/30000s".
std::string FormatFcpxmlTime(const RationalTime& time);

// Resolves `document`'s relative media paths against `packageDirectory`,
// builds, writes `path` (which must be absolute, new, and end in .otio or
// .fcpxml as the format asks) and answers the JSON report: counts, every
// loss, and what was not verified. `errorName` is "InvalidExport" for a
// refused montage, "ValidationFailed" for a path the format does not take,
// "OutputExists" for an existing destination, "IoError" otherwise.
bool ExportOtioFile(Document document, const std::string& packageDirectory,
                    const std::string& path, std::string& reportJson,
                    std::string& errorName, std::string& error);
bool ExportFcpxmlFile(Document document, const std::string& packageDirectory,
                      const std::string& path, std::string& reportJson,
                      std::string& errorName, std::string& error);

// Headless entry points, the shape of ExportEdlCommand: an explicit
// timeline, or the active one when `timelineId` is empty.
int ExportOtioCommand(const std::string& projectPath, const std::string& path,
                      std::string& output, const std::string& timelineId = {});
int ExportFcpxmlCommand(const std::string& projectPath, const std::string& path,
                        std::string& output,
                        const std::string& timelineId = {});
