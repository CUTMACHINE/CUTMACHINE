#pragma once

// R9 -- docs/roadmap/REEL-2026-09.md. The other return trip to DaVinci
// Resolve: a timeline written as an FCP7 XML (xmeml v5) file, because the
// AppendToTimeline descriptor of ResolveExport.h carries neither a speed nor
// a transition.
//
// WHY A FILE AT ALL, after ResolveExport.h argued against interchange
// formats. Measured on Resolve 20.3 (19 September) and again on 20.3.1
// (22 September): AppendToTimeline carries no speed, and no key of
// TimelineItem.SetProperty sets one. A FCPXML <timeMap> does retime, but
// makes a *compound clip* per slowed shot in the Media Pool, with no file
// path. Only a xmeml v5 with the `Time Remap` filter gives a native retime
// linked to the rushes already in the Media Pool, with no duplicate: the
// Media Pool, counted before and after, gains the timeline and nothing else.
// So the file is not a taste for interchange, it is the only measured way a
// speed crosses. A generic output remains ROADMAP.md's "Interop" line.
//
// THE UNIT THAT EVERYTHING TURNS ON. In this format `<in>` and `<out>` are
// *sequence frames after speed*, at the sequence cadence -- not source
// frames. A clip opened on source frame S of a source at R_src, at speed
// p/q, in a sequence at R_seq, opens at
//
//     in = S * q * R_seq / (p * R_src)
//
// and `out` is `in` plus the clip's own length in sequence frames. Written
// in source frames instead, they name the wrong range. The exactness rule of
// ResolveExport.h therefore extends: a bound that is not a whole number *in
// these units* is refused, by name. The case the ticket names is a 50 i/s
// rush at speed 1 opened on an odd source frame: 101 * 1 * 25 / (1 * 50) is
// 50.5, and there is no frame to write.
//
// HOW RESOLVE FINDS THE RUSH, measured 22 September 2026 on 20.3.1, and this
// is the trap of the whole route. With `importSourceClips` false, Resolve
// matches a <file> to a Media Pool clip **by its start timecode alone**. The
// <pathurl> and the <name> are ignored: a file element carrying the path and
// the name of one rush and the timecode of another links to the *other* one,
// and the timeline item then even takes that other rush's name. A <file>
// with no timecode, or with 00:00:00:00, links to whatever untimecoded media
// the search folders happen to hold -- silently, under a successful import.
//
// The document carries no start timecode for a media (it is not an edit
// fact, and R9 adds nothing to the document format), so the engine cannot
// know it. It writes kUnknownStartTimecode instead, which matches nothing:
// an XML imported as-is leaves its clips offline but correctly named, which
// is a visible failure rather than a silent wrong rush. resolve_bridge.py
// --send-xml stamps the real timecodes from the Media Pool, refuses a path
// it cannot find there, and reads the timeline back to check that every clip
// landed on the media the montage named.
//
// PICTURE ONLY. The XML route carries video tracks; a rush's own sound does
// not come with it, unlike AppendToTimeline's `with_audio`. The audio tracks
// a montage holds are counted and reported, never dropped in silence.
//
// THE FRAMING (the step after R1 and R2, 24 September 2026) travels as the
// `Basic Motion` filter of each clipitem, static or keyed, rather than as a
// TimelineItem.SetProperty pass after the import: an animated framing needs
// keys, SetProperty would overwrite them, and GetProperty reads 0 on an
// animated parameter -- so one route for both. What was measured on Resolve
// 20.3 (19 and 20 September, by hand-built XML): `scale` 100 is Zoom 1.0;
// `center` is positive right and DOWN; `<when>` is
// in the basis of `<in>`, sequence frames after speed; keys import linear.
// What Zoom 1.0 shows is the project's choice, not a constant (see
// ResolveScaling): that measurement, on vertical rushes in a vertical
// timeline, could not tell fitted from filled. So a key is written on every
// frame whose value differs from a neighbour's, and a curve arrives as the
// values the engine samples. A crop has no measured equivalent and is refused
// by name; so is an opacity, which neither route carries. R9, #240 (4 November
// 2026): this file's Basic Motion was imported and rendered by Resolve 21.1.
// Its centre is normalized against the fitted/filled picture at Zoom 1.0,
// before the clip's own zoom, so sequence fractions are converted per axis.
//
// WHERE THE REST OF R9 WILL GO, so the next agent does not have to guess:
//   - the fade from black is written, since R5: a "dip_to_color" at the
//     edge of a track becomes a <transitionitem> with alignment
//     "start-black" or "end-black" and no clip on one side. NOT MEASURED
//     against Resolve -- the rushes were offline the day it was written --
//     so it is the one part of this file no import has confirmed. A dip in
//     the middle of a track, or through a colour other than black, is
//     refused by name rather than sent as a dissolve;
//   - the sequence LUT (step after R6) is not expressible here at all: it
//     goes through a colour group's post-clip node graph after the import.
//     Not written. A clip's own colour.* grade does not travel either: it is
//     counted (`grades_not_sent`), not refused, since a montage sent to
//     Resolve is usually sent there to be graded. A clip's blur (EF3) is
//     counted the same way (`blurs_not_sent`).

#include "Document.h"

#include <cstdint>
#include <string>
#include <vector>

// A rush, as this format needs it. One entry per media the montage reads,
// shared by every clip that reads it: Resolve's importer resolves a <file>
// once and later clipitems reference it by id.
struct ResolveXmlMedia {
    std::string path;
    std::string filename;
    MediaRate rate{0, 1};
    // The media's own length in its own frames. Advisory: measured on
    // 20.3.1, a deliberately wrong clipitem duration changed nothing about
    // where the clip landed or which frames it read.
    int64_t frames = 0;
    // Empty is never written; see kUnknownStartTimecode.
    std::string start_timecode;
};

// An exact fraction, spelled as a decimal only in the file.
struct ResolveXmlFraction {
    int64_t num = 0;
    int64_t den = 1;
};

// R9 -- one key of a clip's Basic Motion. `when` is a sequence frame after
// speed in the clip's <in> basis, i.e. <in> plus the clip's own frame.
struct ResolveXmlMotionKey {
    int64_t when = 0;
    // Zoom as the percentage Basic Motion calls scale: 100 is the picture
    // fitted to the frame, Resolve's Zoom 1.0.
    ResolveXmlFraction scale_percent{100, 1};
    // The picture's centre, as a fraction of the sequence frame away from
    // the frame's own centre: positive right, positive down.
    ResolveXmlFraction center_x{0, 1};
    ResolveXmlFraction center_y{0, 1};
};

struct ResolveXmlClip {
    Ulid clip_id;
    size_t media = 0;
    // 0 for the bottom video track, counting up, as ResolveTimelineClip's.
    int32_t video_layer = 0;
    // Position on the sequence, in whole sequence frames from its start.
    int64_t record_in = 0;
    int64_t record_out = 0;
    // The source range, in sequence frames after speed (see the header).
    int64_t media_in = 0;
    int64_t media_out = 0;
    EffectParamValue speed{1, 1};
    // Out of lot -- the author, 29 October 2026. A disabled clip
    // (ClipEnabled.h) travels as the xmeml's own <enabled>FALSE</enabled>, in
    // its place, rather than being left out. Not yet put in front of Resolve.
    bool enabled = true;
    // R9 -- empty for a clip that frames nothing, which then carries no
    // Basic Motion at all; one key for a still framing, written as the
    // parameters' values; several for an animated one, written as keys.
    std::vector<ResolveXmlMotionKey> motion;
};

struct ResolveXmlTransition {
    Ulid id;
    int32_t video_layer = 0;
    int64_t start = 0;
    int64_t end = 0;
    // xmeml alignments: "center", "start", "end", and -- R5 -- the two an
    // edge fade takes, "start-black" and "end-black". Only "center" has
    // been measured through a real import from a document this engine
    // wrote; the edge pair was measured on a hand-built XML on 19
    // September, and what R5 now writes into it has not been put in front
    // of Resolve (the media were offline).
    std::string alignment = "center";
};

struct ResolveXmlTimeline {
    std::string name;
    MediaRate frame_rate{25, 1};
    int32_t width = 0;
    int32_t height = 0;
    // Last record_out, in sequence frames.
    int64_t duration = 0;
    int32_t video_layers = 0;
    // Reported rather than dropped in silence: this route is picture only.
    int32_t audio_tracks_not_sent = 0;
    int32_t multicam_angles_not_sent = 0;
    // R9 -- clips whose colour.* grade stays behind (see the header).
    int32_t grades_not_sent = 0;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. Clips whose blur stays
    // behind, counted as a grade is.
    int32_t blurs_not_sent = 0;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. Clips whose mask stays
    // behind, counted the same way.
    int32_t masks_not_sent = 0;
    std::vector<ResolveXmlMedia> media;
    std::vector<ResolveXmlClip> clips;
    std::vector<ResolveXmlTransition> transitions;
};

// The timecode the engine writes when it does not know a media's own. It
// matches no Media Pool clip, so an unstamped XML imports with its clips
// offline and correctly named, instead of silently linked to another rush.
inline constexpr const char* kUnknownStartTimecode = "99:00:00:00";

// R9 -- how the Resolve project places a picture whose size is not the
// timeline's ("Mismatched resolution files", or the timeline's own
// setting): Zoom 1.0 is then the picture fitted to the frame, or filling it.
// The document cannot know it, and it changes every zoom -- a 16:9 rush in a
// 9:16 timeline is filled at Zoom 1.0 in one and letterboxed in the other,
// a factor of three apart (measured on real deliveries, skill
// monter-une-itw, resolve.md). So the caller declares it, and a framed clip
// is refused until it does.
enum class ResolveScaling { Undeclared, Fit, Fill };

// "fit" or "fill"; anything else is refused.
bool ParseResolveScaling(const std::string& text, ResolveScaling& scaling);

// The `speed` parameter of the Time Remap filter is a percentage, and the
// format has no rational. This expands 100 * num / den by integer division
// only -- no double ever touches a speed -- exactly when the fraction
// terminates within kSpeedPercentDigits, truncated otherwise.
inline constexpr int kSpeedPercentDigits = 12;
std::string FormatSpeedPercent(const EffectParamValue& speed);

// Refuses, by name, a clip whose position, length or source in-point is not
// a whole number of sequence frames after speed, a media the library does
// not hold, a media with no usable rate, and a motion clip. Never rounds.
bool BuildResolveXmlTimeline(
    const Document& document, ResolveXmlTimeline& timeline, std::string& error,
    ResolveScaling scaling = ResolveScaling::Undeclared);

// R9 -- the Basic Motion of one clip whose <in> is `mediaIn`, from its
// framing and keys (ClipFraming.h, ClipAnimation.h): the zoom relative to
// the picture Resolve shows at Zoom 1.0 under `scaling`, the centre as a
// fraction of the frame. Refuses, by name, an undeclared scaling, a crop, a
// clip whose picture has no size (no library media), and a sequence frame
// the displayed source cannot be fitted to.
bool ResolveXmlMotionFor(const Document& document, const DocumentClip& clip,
                         int64_t mediaIn, int64_t durationFrames,
                         ResolveScaling scaling,
                         std::vector<ResolveXmlMotionKey>& keys,
                         std::string& error);

// A fraction as a decimal of at most `digits` places, truncated, never
// through a double: "-0.1", "316.049382".
std::string FormatResolveXmlFraction(const ResolveXmlFraction& value,
                                     int digits = 6);

// The xmeml v5 document. Pure text, no Resolve, no filesystem.
std::string WriteResolveXml(const ResolveXmlTimeline& timeline);

// Resolves `document`'s relative media paths against `packageDirectory`,
// builds, writes `xmlPath` and produces the report body. Shared by the CLI
// command and by the app's chat backend, which holds the live document
// instead of reading the package: what an agent asks for and what the CLI
// writes cannot differ. `errorName` is "InvalidExport" for a refused montage
// and "IoError" for an I/O failure. FI6 -- docs/roadmap/FIABILITE-2026-10.md:
// an existing destination (also a symlink) is refused as "OutputExists";
// publication installs a complete file atomically, without replacement.
// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. The same publication for the
// other interchange files a montage is sent out as (Edl.h): the complete
// bytes staged beside `path` and installed without replacement, an existing
// destination refused as "OutputExists".
bool PublishNewInterchangeFile(const std::string& path,
                               const std::string& bytes, std::string& errorName,
                               std::string& error);

bool BuildAndWriteResolveXml(
    Document document, const std::string& packageDirectory,
    const std::string& xmlPath, std::string& reportJson, std::string& errorName,
    std::string& error, ResolveScaling scaling = ResolveScaling::Undeclared);

// Headless entry point: writes `xmlPath` and answers with a JSON report.
// Describes an explicit timeline, or the active one when timelineId is empty.
int ExportResolveXmlCommand(
    const std::string& projectPath, const std::string& xmlPath,
    std::string& output, const std::string& timelineId = {},
    ResolveScaling scaling = ResolveScaling::Undeclared);
