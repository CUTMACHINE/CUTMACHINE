#pragma once

// RESOLVE-2026-08 -- the return trip: a CUTMACHINE timeline described in the
// terms DaVinci Resolve's scripting API consumes.
//
// This is deliberately not an EDL/FCPXML/OTIO writer. The media is already in
// Resolve's Media Pool -- the same files CUTMACHINE imported -- so the cut can
// be rebuilt by naming source frame ranges, with no interchange format to
// serialize, parse, or get subtly wrong at either end.
//
// The exactness rule is the whole point of putting this in the engine rather
// than in the bridge script. Resolve addresses source frames as integers, and
// `--describe` publishes frames through RationalTime::to_frames, which
// *floors*. Flooring a 50 fps rush's position in a 25 fps sequence silently
// moves the cut. So this converts each boundary itself and refuses any clip
// whose position or duration does not land on a whole frame, rather than
// shipping a montage that is a frame off somewhere nobody will look.
//
// PA16 -- docs/roadmap/PARITE-2026-11.md. ONE SEND THAT CARRIES THE MONTAGE.
// Until 13 November 2026 this descriptor (v2) carried cuts and layers only,
// and refused a speed, a transition, a framing, a disabled clip or an
// opacity by name, pointing at the FCP7 XML route (ResolveXmlExport.h),
// which carries those but no sound and no subtitles. A real film needs
// both halves.
// Resolve 21.1 added what was missing to the API, and each piece was
// measured on 21.1.0.17 (13 November, throwaway project, rushes with their
// frame number burned in, stills read back):
//
//   - TimelineItem.SetSpeed keeps the item's position and length and
//     changes the source it reads from its first frame on. So a clip at
//     speed p/q is appended with the source range its *record* length
//     covers at speed 1 -- [S, S + D * R_src / R_seq) -- and then given its
//     speed: it lasts D and reads [S, S + D * p/q). A 50 i/s rush at 1/2
//     shows one source frame per sequence frame, as the engine does.
//   - SetSpeed at 0 holds the LAST frame of the appended range (119 for
//     [100, 120), 349 for [300, 350), read on the stills;
//     GetSourceStartFrame reported 348, its known one-frame error). So a
//     held frame F lasting D is appended as [F - D + 1, F + 1).
//   - TimelineItem.AddTransition puts a « Cross Dissolve » on an item's end
//     edge: "center" straddles the cut, "left" ends on it, "right" starts
//     on it -- the engine's Center, EndAtCut and StartAtCut. Without a
//     handle it answers None. « Cross Fade +3 dB » (category "audio") is
//     the engine's equal-power cross fade.
//   - SetFades on a video item fades its opacity: a dip to black at the
//     edge of a track or between two clips is two fades and reads no
//     handle, exactly as the engine's veil does (TransitionRender.h).
//   - SetProperties takes ZoomX/ZoomY, Pan, Tilt, Opacity; SetCDL the ten
//     numbers; SetClipEnabled a disabled clip; AudioVolume and SetFades an
//     audio item's gain and envelopes; Timeline.AddMarker a marker (one per
//     frame, and a length of at least one frame); SetClipsLinked a pair.
//   - Subtitles appended first, on the empty timeline, keep their place
//     while every clip, speed and transition is added after them.
//
// What the API still cannot carry is not refused: it is counted and named
// in `losses`, clip by clip, and the montage goes over without it. Keyframes
// of a framing (no API sets one; the clip takes its first frame's framing),
// a crop, a blur, a mask, a motion clip (its place stays empty), a sequence
// look, a caption style, a second caption track, a dip through a colour
// other than black. A grade travels only on request, as a CDL
// (ResolveSendOptions::grade_cdl): its numbers mean something only on the
// values the engine grades, which a Resolve project need not share.
//
// Every clip is sent as what it is: a video clip as picture only
// (mediaType 1), an audio clip as sound only (mediaType 2) on its own
// track, the two linked again when the document links them. The v2 rule
// that let a video clip bring its rush's sound along, when an audio clip
// played exactly that range, is gone with it: a J or L cut no longer comes
// back silent.

#include "ColorCdl.h"
#include "Document.h"
#include "ResolveXmlExport.h"

#include <cstdint>
#include <string>
#include <vector>

// A clip's framing in the units Resolve's Transform reads: one zoom for
// both axes, and Pan and Tilt in pixels of the timeline (Tilt positive up).
struct ResolveSendFraming {
    ResolveXmlFraction zoom{1, 1};
    ResolveXmlFraction pan{0, 1};
    ResolveXmlFraction tilt{0, 1};
};

struct ResolveTimelineClip {
    Ulid clip_id;
    // A sound (mediaType 2, an audio track) or a picture (mediaType 1).
    bool audio = false;
    // Resolve's 1-based index among the tracks of the clip's kind.
    int32_t track = 1;
    // The absolute path is what identifies the media in Resolve's Media Pool;
    // the filename is carried for diagnostics and for the fallback match.
    std::string path;
    std::string filename;
    // The half-open range AppendToTimeline is given, in the source's own
    // frames -- or, for a media without pictures, in sequence frames, the
    // cadence Resolve gives an audio file. Measured against a real round
    // trip: Resolve builds end-start frames, so an inclusive end silently
    // drops the last frame of every single clip. Not the range the clip
    // reads once its speed is set: see the header.
    int64_t start_frame = 0;
    int64_t end_frame = 0;
    // Position on the sequence, in whole sequence frames counted from its
    // start. The bridge adds Resolve's own timeline start (90000 at 25 fps
    // for a 01:00:00:00 start), which is a Resolve fact and so is not baked
    // in here.
    int64_t record_frame = 0;
    // What the item must last on the timeline, in sequence frames: what the
    // bridge reads back once the speed is set.
    int64_t duration = 0;
    // Set by SetSpeed after the append, as a percentage of num/den; 0/1 is
    // a held frame. A ramp travels as its mean speed (a loss).
    ResolveXmlFraction speed{1, 1};
    bool enabled = true;
    // Items sharing a group, 0 and up, are linked in Resolve; -1 for none.
    int32_t link_group = -1;
    // Pictures of a media: the transform for each of Resolve's two ways of
    // placing a picture whose size is not the timeline's. The bridge picks
    // the one its project uses. False for a sound.
    bool framed = false;
    ResolveSendFraming fit;
    ResolveSendFraming fill;
    EffectParamValue opacity{1, 1};
    // The CDL for SetCDL, when grades travel and this one folds into one.
    bool graded = false;
    ColorCdl cdl;
    // Sound only.
    EffectParamValue gain_db{0, 1};
    // Frames of the item's fader: a sound's envelopes, or a picture's
    // opacity ramps standing for a dip to black.
    int64_t fade_in = 0;
    int64_t fade_out = 0;
};

// A cross dissolve or a cross fade, attached to the end of the outgoing clip.
struct ResolveSendTransition {
    Ulid transition_id;
    bool audio = false;
    // Index in ResolveTimelineExport::clips of the outgoing clip.
    size_t clip = 0;
    // "center", "left" (ends on the cut) or "right" (starts on it).
    std::string alignment;
    // Resolve's own name and category: « Cross Dissolve », "simple";
    // « Cross Fade +3 dB », "audio".
    std::string type;
    std::string category;
    int64_t duration = 0;
    // Where it must land, from the sequence start: for the read back.
    int64_t record_start = 0;
    int64_t record_end = 0;
};

struct ResolveSendMarker {
    // Every marker this one stands for: two markers on one frame are one
    // marker in Resolve, their names joined.
    std::vector<Ulid> marker_ids;
    int64_t frame = 0;
    // At least 1: Resolve refuses a marker of no length.
    int64_t duration = 1;
    // One of Resolve's sixteen marker colours (Edl.h's table).
    std::string color;
    std::string name;
    std::string note;
};

struct ResolveSendTrack {
    bool audio = false;
    int32_t index = 1;
    // A hidden video track, a muted (or not soloed) audio track.
    bool enabled = true;
    // An audio track whose every clip is a rush's own sound: the voice,
    // which the house rule puts on a mono track.
    bool mono = false;
};

// What did not travel, by kind, with every clip, transition or marker it
// concerns.
struct ResolveSendLoss {
    std::string what;
    std::string detail;
    std::vector<Ulid> ids;
};

struct ResolveSendOptions {
    // Send each clip's grade as a CDL for node `cdl_node` (export_cdl's
    // fold). False: grades are counted, not sent.
    bool grade_cdl = false;
    int cdl_node = 1;
};

struct ResolveTimelineExport {
    std::string name;
    MediaRate frame_rate{25, 1};
    int32_t width = 0;
    int32_t height = 0;
    // One entry per video track, then one per audio track, in Resolve's
    // order. Always at least one of each kind: a new timeline has them.
    std::vector<ResolveSendTrack> tracks;
    std::vector<ResolveTimelineClip> clips;
    std::vector<ResolveSendTransition> transitions;
    std::vector<ResolveSendMarker> markers;
    // The cartons of the first caption track that shows any, as SubRip.
    std::string subtitles;
    size_t subtitle_cues = 0;
    // The length the timeline must have, in sequence frames: the end of
    // whatever is sent, cartons included.
    int64_t duration = 0;
    bool grade_cdl = false;
    int cdl_node = 1;
    // Video clips of an inactive multicam angle: not in the picture, so not
    // sent, and not a loss.
    int32_t multicam_angles_not_sent = 0;
    std::vector<ResolveSendLoss> losses;
};

// Exact conversion of a time in its own rational domain to a whole frame
// count at `rate`. Returns false when the time falls between two frames.
bool ExactFrameCount(const RationalTime& time, const MediaRate& rate,
                     int64_t& frames);

// Fails, by name, on a clip that references unknown media, on a source with
// no usable rate, on any boundary that is not a whole frame, on a held
// frame too close to its media's start and on a slow motion too close to
// its media's end (both read past it at speed 1), and on a media's picture
// that cannot be framed. Never rounds a cut.
bool BuildResolveTimelineExport(const Document& document,
                                ResolveTimelineExport& exported,
                                std::string& error,
                                const ResolveSendOptions& options = {});

// The descriptor, schema cutmachine.resolve-timeline.v3, that
// sidecar/resolve_bridge.py --send builds the timeline from.
std::string SerializeResolveTimelineExport(
    const ResolveTimelineExport& exported);

// The library's paths made absolute against the project package: Resolve's
// Media Pool knows only absolute paths.
void AbsoluteLibraryPaths(Document& document,
                          const std::string& packageDirectory);

// Headless entry point: describes an explicit timeline, or the active one
// when timelineId is empty, for the bridge script. Read-only.
int ExportResolveTimelineCommand(const std::string& projectPath,
                                 std::string& output,
                                 const std::string& timelineId = {},
                                 const ResolveSendOptions& options = {});
