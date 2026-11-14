// CA3 -- docs/roadmap/CADRAGE-2026-10.md. The faces of a clip, read through
// the finder the executable installed (FaceFinder.h). Separate from
// SubjectTracking.cc because this half decodes rushes and calls the finder;
// the rule itself never does.
//
// Each sampled frame is its own FFmpeg run, seeked to the source time the
// speed rule names for that clip frame (ClipSpeed.h), as read_frame and
// measure_color seek theirs. A single run with a `select` filter would be
// faster, but it counts frames of the source from the seek, not frames of
// the clip, and a clip with a speed reads its source at another cadence --
// one rule for every clip is worth a slower read.
//
// The picture is the whole displayed source (rotation applied by FFmpeg's
// decoder, as everywhere), not the framed rectangle: the subject has to be
// found where it is, including outside what the framing shows today. It is
// 8-bit RGB converted by swscale, not the display-referred picture
// measure_color builds: the finder looks for a face, not for its colour.
// A log-encoded rush therefore reaches it flat; Vision has not been tried
// on one (CADRAGE-2026-10, "Ce qui n'est pas prouvé").

#include "SubjectTracking.h"

#include "ClipSpeed.h"
#include "FaceFinder.h"
#include "FfmpegBinary.h"
#include "FfmpegPipe.h"
#include "FrameCapture.h"

#include <filesystem>

bool SampleClipFaces(const Document& document,
                     const std::string& projectDirectory, const Ulid& clipId,
                     int64_t everyFrames, int32_t maxDimension,
                     const std::string& ffmpegPath,
                     std::vector<SubjectSample>& samples, std::string& error) {
    samples.clear();
    error.clear();
    const FaceFinder finder = CurrentFaceFinder();
    if (finder == nullptr) {
        error =
            "no face finder in this process: following a face needs Vision, "
            "which only the macOS cutmachine executable installs, not the "
            "build without a window (headless/); give the detections "
            "yourself with 'detections'";
        return false;
    }
    if (everyFrames <= 0 || maxDimension < 16) {
        error = "invalid sampling settings";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    const std::string named = "clip '" + clipId + "': ";
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr || IsMotionClip(*clip)) {
        error = named + "has no rush to read a face in";
        return false;
    }
    int64_t shownWidth = 0;
    int64_t shownHeight = 0;
    if (!ClipDisplayedSize(document, *clip, shownWidth, shownHeight, error)) {
        error = named + error;
        return false;
    }
    int64_t width = 0;
    int64_t height = 0;
    BoundedFrameSize(
        shownWidth, shownHeight,
        std::min<int64_t>(maxDimension, std::max(shownWidth, shownHeight)),
        width, height);
    if (width <= 0 || height <= 0) {
        error = named + "its picture is empty";
        return false;
    }
    int64_t frameCount = 0;
    if (!ClipFrameCount(*clip, document.sequence.frame_rate, frameCount,
                        error)) {
        error = named + error;
        return false;
    }
    std::filesystem::path path(source->path);
    if (path.is_relative())
        path = std::filesystem::path(projectDirectory) / path;
    const std::string file =
        std::filesystem::absolute(path).lexically_normal().string();
    if (!ffmpeg::InputPresent(file, error)) {
        error = named + error;
        return false;
    }
    const MediaRate& rate = document.sequence.frame_rate;
    const size_t frameBytes = static_cast<size_t>(width * height * 3);
    for (int64_t frame : SubjectSampleFrames(frameCount, everyFrames)) {
        const RationalTime local(frame * rate.den, rate.num);
        const RationalTime sourceTime =
            SourceFrameStart(ClipSourceTimeAt(*clip, local), source->rate);
        if (sourceTime.value < 0) {
            error = named + "a sampled frame falls before its source";
            return false;
        }
        std::vector<uint8_t> rgb;
        int64_t frames = 0;
        const std::vector<std::string> arguments = {
            ffmpegPath,
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-ss",
            FfmpegSeekSeconds(sourceTime),
            "-i",
            file,
            "-map",
            "0:v:0",
            "-an",
            "-frames:v",
            "1",
            "-vf",
            "scale=w=" + std::to_string(width) + ":h=" + std::to_string(height),
            "-pix_fmt",
            "rgb24",
            "-f",
            "rawvideo",
            "-",
        };
        if (!ReadFfmpegRawFrames(
                arguments, frameBytes,
                [&](const uint8_t* bytes) {
                    if (frames++ == 0) rgb.assign(bytes, bytes + frameBytes);
                },
                error)) {
            error = named + "frame " + std::to_string(frame) + ": " + error;
            return false;
        }
        if (rgb.empty()) {
            error = named + "frame " + std::to_string(frame) +
                    ": the source produced no picture there";
            return false;
        }
        SubjectSample sample;
        sample.time = local;
        if (!finder(static_cast<int32_t>(width), static_cast<int32_t>(height),
                    rgb, sample.faces, error)) {
            error = named + "frame " + std::to_string(frame) + ": " + error;
            return false;
        }
        samples.push_back(std::move(sample));
    }
    return true;
}
