// PA7 -- docs/roadmap/PARITE-2026-11.md. The movement between the frames of
// a clip, read from its rush. Separate from Stabilization.cc because this
// half decodes; the rule itself never does.
//
// One FFmpeg run over the clip's source range, where the followed framing
// seeks each sampled frame on its own (SubjectTrackingSampling.cc): every
// frame is wanted here, in order, and a clip with a speed -- the reason
// that file gives for its slower read -- is refused instead. The picture is
// the whole displayed source in grey (rotation applied by FFmpeg's decoder,
// as everywhere), reduced to the analysis size: the movement is the
// camera's, whatever the framing shows of it.

#include "Stabilization.h"

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "FfmpegBinary.h"
#include "FfmpegPipe.h"
#include "FrameCapture.h"
#include "MotionClip.h"
#include "ShotQuality.h"

#include <filesystem>

bool SampleClipMotion(const Document& document,
                      const std::string& projectDirectory, const Ulid& clipId,
                      const std::string& ffmpegPath,
                      std::vector<StabilizationSample>& motion,
                      std::string& error) {
    motion.clear();
    error.clear();
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    const std::string named = "clip '" + clipId + "': ";
    const DocumentSource* source = document.FindSource(clip->source_id);
    if (source == nullptr || IsMotionClip(*clip)) {
        error = named + "has no rush to read a movement in";
        return false;
    }
    if (ClipHasSpeed(*clip)) {
        error = named +
                "plays at a speed other than 1/1: its frames are not its "
                "source's, and the movement between two of them is not the "
                "camera's (steady the clip before changing its speed)";
        return false;
    }
    const MediaRate& rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0 || source->rate.num <= 0 ||
        source->rate.den <= 0 ||
        static_cast<int64_t>(source->rate.num) * rate.den !=
            static_cast<int64_t>(rate.num) * source->rate.den) {
        error = named +
                "its source is not at the sequence's cadence: frames "
                "repeated or dropped to fit read as a stop and a jump";
        return false;
    }
    int64_t shownWidth = 0;
    int64_t shownHeight = 0;
    if (!ClipDisplayedSize(document, *clip, shownWidth, shownHeight, error)) {
        error = named + error;
        return false;
    }
    int64_t frameCount = 0;
    if (!ClipFrameCount(*clip, rate, frameCount, error)) {
        error = named + error;
        return false;
    }
    int32_t width = 0;
    int32_t height = 0;
    StabilizationAnalysisSize(shownWidth, shownHeight, width, height);
    std::filesystem::path path(source->path);
    if (path.is_relative())
        path = std::filesystem::path(projectDirectory) / path;
    const std::string file =
        std::filesystem::absolute(path).lexically_normal().string();
    if (!ffmpeg::InputPresent(file, error)) {
        error = named + error;
        return false;
    }
    const RationalTime start =
        SourceFrameStart(ClipSourceTimeAt(*clip, {0, 1}), source->rate);
    if (start.value < 0) {
        error = named + "its first frame falls before its source";
        return false;
    }
    const std::vector<std::string> arguments = {
        ffmpegPath,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-ss",
        FfmpegSeekSeconds(start),
        "-i",
        file,
        "-map",
        "0:v:0",
        "-an",
        "-frames:v",
        std::to_string(frameCount),
        "-vf",
        "scale=w=" + std::to_string(width) + ":h=" + std::to_string(height) +
            ":flags=area",
        "-pix_fmt",
        "gray",
        "-f",
        "rawvideo",
        "-",
    };
    const size_t frameBytes = static_cast<size_t>(width) * height;
    std::vector<uint8_t> previous;
    motion.reserve(static_cast<size_t>(frameCount));
    if (!ReadFfmpegRawFrames(
            arguments, frameBytes,
            [&](const uint8_t* bytes) {
                if (static_cast<int64_t>(motion.size()) >= frameCount) return;
                StabilizationSample sample;
                if (!previous.empty()) {
                    const GlobalMotion measured = EstimateGlobalMotion(
                        previous.data(), bytes, width, height);
                    // Under four blocks the estimator fitted nothing.
                    sample.measured = measured.blocks >= 4;
                    if (sample.measured) {
                        sample.translation_x = measured.translation_x / width;
                        sample.translation_y = measured.translation_y / height;
                        sample.zoom = measured.zoom;
                    }
                }
                previous.assign(bytes, bytes + frameBytes);
                motion.push_back(sample);
            },
            error)) {
        error = named + error;
        return false;
    }
    if (motion.empty()) {
        error = named + "its source produced no picture there";
        return false;
    }
    // A rush that ends a frame or two before the clip does: the frames it
    // lacks are read as unmeasured, and counted as such.
    motion.resize(static_cast<size_t>(frameCount));
    return true;
}
