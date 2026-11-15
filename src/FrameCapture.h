#pragma once

#include "ClipFraming.h"
#include "Document.h"
#include "RationalTime.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// QC-2026-08 -- One frame of a source, encoded so a model can actually look
// at it.
//
// This exists because measurement is not enough, and the gap was found the
// hard way. ShotQuality.h grades sharpness and steadiness; on a real edit it
// passed a shot as Sharp/Steady that was unusable as a cutaway, because it
// showed a person visibly speaking words other than the ones on the
// soundtrack. No threshold on Laplacian variance can catch that. A human
// monteur catches it in one glance, and so can a model -- but only if
// something hands it the picture.
//
// So the rule this file lives under is narrower than "give the agent
// vision": what is *provable* stays in ShotQuality.cc and is decided by
// code, and what needs a glance gets a frame. Nothing here decides anything;
// it renders.
//
// Deliberately not a Document-mutating path and not an operation: capturing
// a frame changes nothing, and the result is never cached -- a frame is
// cheap to re-render and stale ones would outlive a relink.

struct FrameCaptureSettings {
    // Long edge of the returned image. Small on purpose: this is for
    // recognition ("is someone talking", "is the subject in frame"), not
    // inspection, and every pixel costs tokens on the way to a model.
    int32_t max_dimension = 512;
    // JPEG, not PNG: a photographic frame compresses an order of magnitude
    // smaller, and the artifacts are far below what any of these judgements
    // turn on.
    int32_t jpeg_quality = 6;  // FFmpeg -q:v scale, 2 (best) to 31 (worst).
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    // ITW5 -- ROADMAP.md. The project's colour management. When enabled the
    // frame goes through the same SDR display view as contact_sheet and
    // cut_sheet: read_frame used to hand back the raw signal, so one rush
    // looked two different ways depending on the tool that showed it.
    ColorManagementSettings color_management;
    // R1 -- docs/roadmap/REEL-2026-09.md. When the frame asked for belongs
    // to a framed clip, what comes back is what the sequence shows there,
    // not the whole rush: the source rectangle ClipFraming.h named, drawn
    // where it named it in a frame of `frame_width` x `frame_height`. An
    // agent that reframes a shot and then reads it back has to see the
    // composed picture -- that is the whole of IA4's lesson, and the reason
    // this is not left to the caller to assemble. False means the rush
    // straight, exactly as read_frame has always returned it.
    bool framed = false;
    ClipFramingGeometry framing;
    int64_t source_width = 0;
    int64_t source_height = 0;
    int64_t frame_width = 0;
    int64_t frame_height = 0;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred clip is composed as
    // a framed one (`framed` is set, with the default fit when it frames
    // nothing), and its blur is drawn on the picture before it is laid in
    // the frame: `gblur` in this frame's pixels (ClipBlur.h), empty for a
    // clip that blurs nothing. `blur_rgb` when its media is RGB, blurred as
    // RGB rather than through a matrix the picture does not have.
    std::string blur_filter;
    bool blur_rgb = false;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. A masked clip is composed as
    // a framed one too, and its mask is put on the picture after the blur
    // (ClipMaskFilter); what it cuts out is shown black, the frame's own
    // colour here, since a read shows one clip alone.
    std::string mask_filter;
    // PA15 -- docs/roadmap/PARITE-2026-11.md. A clip cut out along its
    // people takes the matte of the frame read (PersonMatte.h) in place of
    // a shape's filter. The caller says where the package is;
    // ApplyClipFramingToCapture finds the matte there and names the frame.
    std::string package_directory;
    std::string matte_path;
    int64_t matte_frame = 0;
    bool matte_inverted = false;
};

// The last step before a JPEG is written, here and for the timeline sheets.
// Every picture they render is BT.709 video: the export passes a source's
// values through and tags them BT.709, and the colour-managed view ends in
// BT.709. A JPEG is BT.601 full range by definition, and every reader --
// CoreGraphics, the chat panel, a vision model -- decodes it that way.
// Written without this conversion, the values were read back as BT.601 and
// reds and greens drifted from what the monitor and the file show (agentic
// QA of 23 September 2026: 27.5 dB against the pattern decoded as BT.709,
// 40.8 with it).
inline constexpr char kJpegFromVideoFilter[] =
    "scale=in_color_matrix=bt709:out_color_matrix=bt601:out_range=pc,"
    "format=yuvj420p";

// Renders the frame containing `time` in `inputPath`'s own time domain, as
// JPEG bytes. Rotation metadata is honoured, so a vertical rush comes back
// vertical rather than on its side.
bool CaptureSourceFrame(const std::string& inputPath, const RationalTime& time,
                        const FrameCaptureSettings& settings,
                        std::string& jpegBytes, std::string& error);

// R1 -- docs/roadmap/REEL-2026-09.md. Fills the framing part of
// `settings` for the clip `clipId` names, so that read_frame answers with
// the picture the sequence shows rather than the whole rush. An empty id,
// an id the document does not hold, or a clip that frames nothing leaves
// the settings alone and the capture unchanged. Written here rather than in
// each backend because two backends serve read_frame and they must compose
// the same rectangle.
//
// R2 -- `sourceTime` is the source time being read, which is what a
// backend has; on an animated clip it names the frame whose values are
// sampled. An agent that animates a zoom and reads the middle of it back
// has to see the middle of the zoom, not the first frame: reading back
// what was set is the whole of IA4's lesson.
bool ApplyClipFramingToCapture(const Document& document, const Ulid& clipId,
                               const RationalTime& sourceTime,
                               FrameCaptureSettings& settings,
                               std::string& message);

// The source time a named place in a clip reads: "Start", "Middle" or
// "End". Named positions, not a time: the caller says which part of the
// shot it wants and the engine resolves the frame. "End" steps back one
// sample interval so it lands inside the clip rather than on the cut after
// it. R3 -- the place is in the clip's own span, so the source time goes
// through the speed rule (ClipSpeed.h) and names the frame the monitor and
// the export show there. G1 -- docs/roadmap/GRADE-2026-09.md: read_frame
// and measure_color share it, so a measurement is of the frame an agent
// looked at. False only for a name outside the three; the caller words the
// refusal.
bool ClipFramePositionTime(const DocumentClip& clip,
                           const std::string& position, RationalTime& time);

// G1b -- docs/roadmap/GRADE-2026-09.md. A picture the engine computed --
// display RGB in 0..1, row by row, `width` x `height` -- as the JPEG every
// other picture here becomes: laid into the sequence frame when
// `settings.framed` (the rectangle `settings.framing` names, black around
// it), then BT.601 full range, which is what a JPEG is.
bool EncodeDisplayPictureAsJpeg(const std::vector<std::array<float, 3>>& rgb,
                                int64_t width, int64_t height,
                                const FrameCaptureSettings& settings,
                                std::string& jpegBytes, std::string& error);

// A non-negative exact time as the decimal seconds FFmpeg's -ss takes, by
// long division rather than through a double (see the .cc). G1 -- shared
// with ColorMeasure.cc, which seeks the same frames.
std::string FfmpegSeekSeconds(const RationalTime& time);

// Standard base64, no line breaks, padded. Written here rather than pulled
// in because this is the only place in the project that needs it.
std::string EncodeBase64(const std::string& bytes);
