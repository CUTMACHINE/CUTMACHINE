#include "MotionVideoRender.h"

#include "FfmpegCapabilities.h"
#include "FfmpegPipe.h"
#include "Loudness.h"
#include "MotionAudio.h"
#include "MotionEvaluate.h"
#include "MotionRender.h"
#include "Ulid.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace {

using mcp_json::Value;

std::string Label(const MotionVideoItem& item) {
    std::string label =
        "version '" + item.variant + "' in format '" + item.format + "'";
    if (item.variant.empty() && item.format.empty()) {
        label = "the composition";
    } else if (item.variant.empty()) {
        label = "format '" + item.format + "'";
    } else if (item.format.empty()) {
        label = "version '" + item.variant + "'";
    }
    return label;
}

std::string FrameName(int64_t frame) {
    char name[32];
    std::snprintf(name, sizeof name, "%06lld.png",
                  static_cast<long long>(frame));
    return name;
}

// Removes the frames of one render whatever becomes of it.
struct FrameDirectory {
    std::filesystem::path path;
    ~FrameDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

// PRD16 -- ROADMAP.md. On a background task, drawing is the first half of a
// file's share of the task, encoding the second; and a cancellation stops
// the drawing at the next frame.
bool DrawFrames(const MotionVideoItem& item, const MotionVideoRequest& request,
                const std::filesystem::path& directory, MotionError& error,
                const ExportTaskStep& step, MediaTaskContext* context) {
    MotionCoreTextMeasurer measurer(request.media_directory);
    measurer.SetMediaPaths(request.reach.media_paths);
    for (int64_t index = 0; index < item.composition.duration; ++index) {
        if (context != nullptr) {
            if (context->Cancelled()) {
                error = {"Cancelled", "output", "render cancelled"};
                return false;
            }
            context->SetProgress(
                Exporter::TaskFraction(
                    step, 0.5 * static_cast<double>(index) /
                              static_cast<double>(item.composition.duration)),
                (step.files > 1 ? "file " + std::to_string(step.file + 1) +
                                      "/" + std::to_string(step.files) + " (" +
                                      std::to_string(step.file) + " complete): "
                                : std::string()) +
                    "drawing frame " + std::to_string(index + 1) + "/" +
                    std::to_string(item.composition.duration) + " of " +
                    item.output_path);
        }
        MotionFrame frame;
        MotionRaster raster;
        if (!EvaluateMotionFrame(item.composition, item.props, index, measurer,
                                 request.reach.Context(), frame, error) ||
            !RenderMotionFrame(frame, measurer, raster, error)) {
            return false;
        }
        if (!request.options.alpha) {
            // Premultiplied colour is the frame composited over black: an
            // opaque alpha is all flattening takes. Dropping alpha from a
            // straight-alpha PNG instead would turn every antialiased edge
            // to full colour.
            for (size_t at = 3; at < raster.pixels.size(); at += 4) {
                raster.pixels[at] = 255;
            }
        }
        if (!WriteMotionPng(raster, (directory / FrameName(index)).string(),
                            error)) {
            return false;
        }
    }
    return true;
}

// MM1 -- the sound track of an encoded file, decoded by FFmpeg to 48 kHz
// stereo floats: what a viewer hears, measured as the file holds it.
bool DecodeSound(const std::string& path, const std::string& ffmpegPath,
                 MotionPcm& pcm, std::string& error) {
    pcm = MotionPcm{};
    return ReadFfmpegRawFrames(
        {ffmpegPath, "-hide_banner", "-nostdin", "-v", "error", "-i", path,
         "-vn", "-ac", "2", "-ar", "48000", "-f", "f32le", "-"},
        2 * sizeof(float),
        [&](const uint8_t* bytes) {
            float sample[2];
            std::memcpy(sample, bytes, sizeof(sample));
            pcm.left.push_back(sample[0]);
            pcm.right.push_back(sample[1]);
        },
        error);
}

double Rounded(double value) { return std::round(value * 100.0) / 100.0; }

Value MeasureValue(const LoudnessMeasure& measure) {
    Value value = Value::MakeObject();
    value.Set("integrated_lufs",
              measure.integrated_lufs.has_value()
                  ? Value::MakeDouble(Rounded(*measure.integrated_lufs))
                  : Value::MakeNull());
    value.Set("true_peak_dbtp",
              std::isfinite(measure.true_peak_dbtp)
                  ? Value::MakeDouble(Rounded(measure.true_peak_dbtp))
                  : Value::MakeNull());
    return value;
}

// REM2 -- the composition's whole sound, for FFmpeg to encode beside the
// frames. MM1 -- brought to the loudness asked, if any (Loudness.h). AAC
// rebuilds a waveform whose peaks can stand over the samples it was given:
// for an .mp4 the mix is encoded once on its own, the encoder's output
// measured, and the limiter's ceiling lowered by what it overshot, until
// what the encoder gives back is under the ceiling. `sound` receives what
// was done.
bool WriteSound(const MotionVideoItem& item, const MotionVideoRequest& request,
                const std::filesystem::path& directory, Value& sound,
                MotionError& error) {
    MotionCoreTextMeasurer measurer(request.media_directory);
    measurer.SetMediaPaths(request.reach.media_paths);
    MotionPcm mix;
    if (!MixMotionAudio(item.composition, item.props, request.reach.Context(),
                        measurer, 0, MotionAudioSampleCount(item.composition),
                        mix, error)) {
        return false;
    }
    const MotionVideoOptions& options = request.options;
    const std::string wav = (directory / "sound.wav").string();
    std::string message;
    if (!options.loudness.has_value()) {
        if (!WriteMotionAudioWav(mix, wav, message)) {
            error = {"IoError", "output", message};
            return false;
        }
        return true;
    }
    const bool aac =
        std::filesystem::path(item.output_path).extension() != ".mov";
    LoudnessTarget target{*options.loudness, options.true_peak};
    LoudnessResult result;
    double coded = 0.0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        MotionPcm normalized = mix;
        NormalizeLoudness(normalized.left, normalized.right, kMotionAudioRate,
                          target, result);
        std::error_code ignored;
        std::filesystem::remove(wav, ignored);
        if (!WriteMotionAudioWav(normalized, wav, message)) {
            error = {"IoError", "output", message};
            return false;
        }
        if (!aac || result.silent) {
            break;
        }
        const std::string trial = (directory / "trial.m4a").string();
        std::filesystem::remove(trial, ignored);
        MotionPcm decoded;
        if (!ReadFfmpegRawFrames(
                {options.ffmpeg_path, "-hide_banner", "-nostdin", "-v", "error",
                 "-y", "-i", wav, "-c:a", "aac", "-ar", "48000", "-ac", "2",
                 "-b:a", "320k", trial},
                1, [](const uint8_t*) {}, message) ||
            !DecodeSound(trial, options.ffmpeg_path, decoded, message)) {
            error = {"ExportFailed", "output",
                     "the sound's trial encoding failed: " + message};
            return false;
        }
        coded = TruePeakDb(decoded.left, decoded.right);
        if (coded <= options.true_peak) {
            break;
        }
        target.true_peak_dbtp -= coded - options.true_peak + 0.05;
    }
    sound = Value::MakeObject();
    sound.Set("target_lufs", Value::MakeDouble(*options.loudness));
    sound.Set("true_peak_ceiling_dbtp", Value::MakeDouble(options.true_peak));
    sound.Set("mix", MeasureValue(result.before));
    if (result.silent) {
        sound.Set("silent", Value::MakeBool(true));
        return true;
    }
    sound.Set("gain_db", Value::MakeDouble(Rounded(result.gain_db)));
    sound.Set("limited_db", Value::MakeDouble(Rounded(result.limited_db)));
    if (target.true_peak_dbtp < options.true_peak) {
        // The ceiling the limiter was set to, under the one asked, for the
        // codec's overshoot.
        sound.Set("limiter_ceiling_dbtp",
                  Value::MakeDouble(Rounded(target.true_peak_dbtp)));
    }
    return true;
}

// One file of the request, through a temporary PNG sequence removed after
// it: empty on success, the reason otherwise. PRD16 -- `context`, on a
// background task, is followed and polled.
std::string RenderItem(const MotionVideoItem& item,
                       const MotionVideoRequest& request, bool audible,
                       const ExportTaskStep& step, MediaTaskContext* context,
                       Value& sound) {
    FrameDirectory frames;
    frames.path = std::filesystem::temp_directory_path() /
                  ("cutmachine-motion-" + GenerateUlid());
    std::error_code status;
    MotionError drawn;
    if (!std::filesystem::create_directory(frames.path, status)) {
        return "cannot create the frame directory '" + frames.path.string() +
               "': " + status.message();
    }
    if (!DrawFrames(item, request, frames.path, drawn, step, context)) {
        return drawn.code + ": " + drawn.message;
    }
    if (audible && !WriteSound(item, request, frames.path, sound, drawn)) {
        return drawn.code + ": " + drawn.message;
    }
    const ExportPlan plan = MotionVideoExportPlan(
        item, request.options, (frames.path / "%06d.png").string(),
        audible ? (frames.path / "sound.wav").string() : std::string());
    std::string message;
    ExportProgressCallback progress;
    if (context != nullptr) {
        progress = [&](const ExportProgress& encoded) {
            context->SetProgress(
                Exporter::TaskFraction(step, 0.5 + 0.5 * encoded.fraction),
                Exporter::ProgressDetail(step, item.output_path, encoded));
        };
    }
    if (!Exporter::Run(plan, progress,
                       context != nullptr ? context->CancelFlag() : nullptr,
                       message)) {
        return message;
    }
    // MM1 -- the file's own sound, measured as delivered.
    if (audible && sound.IsObject() && sound.Find("silent") == nullptr) {
        MotionPcm delivered;
        if (DecodeSound(item.output_path, request.options.ffmpeg_path,
                        delivered, message)) {
            const LoudnessMeasure measure = MeasureLoudness(
                delivered.left, delivered.right, kMotionAudioRate);
            const Value file = MeasureValue(measure);
            sound.Set("file", file);
            sound.Set("ceiling_kept",
                      Value::MakeBool(measure.true_peak_dbtp <=
                                      request.options.true_peak + 1e-3));
        } else {
            sound.Set("file_error", Value::MakeString(message));
        }
    }
    return std::string();
}

}  // namespace

bool RenderMotionVideos(const MotionVideoRequest& request, std::string& report,
                        MotionError& error) {
    report.clear();
    if (request.composition == nullptr) {
        error = {"InvalidValue", "composition", "no composition to render"};
        return false;
    }
    for (const std::string& directory : request.font_directories) {
        if (!RegisterMotionFontDirectory(directory, error)) {
            return false;
        }
    }
    std::vector<MotionVideoItem> items;
    if (!PlanMotionVideos(*request.composition, request.options, items,
                          error)) {
        return false;
    }
    // Evaluation costs a tenth of a millisecond a frame (M2): every frame of
    // every render is laid out before anything is encoded. REM2 -- and every
    // sound is found, so that a missing media or a sound at another speed
    // is refused before the first file is written.
    std::vector<bool> audible(items.size(), false);
    for (size_t at = 0; at < items.size(); ++at) {
        const MotionVideoItem& item = items[at];
        MotionCoreTextMeasurer measurer(request.media_directory);
        measurer.SetMediaPaths(request.reach.media_paths);
        for (int64_t index = 0; index < item.composition.duration; ++index) {
            MotionFrame frame;
            if (!EvaluateMotionFrame(item.composition, item.props, index,
                                     measurer, request.reach.Context(), frame,
                                     error)) {
                error.message = Label(item) + ", frame " +
                                std::to_string(index) + ": " + error.message;
                return false;
            }
        }
        std::string voices;
        size_t count = 0;
        if (!DescribeMotionAudio(item.composition, item.props,
                                 request.reach.Context(), measurer, voices,
                                 count, error)) {
            error.message = Label(item) + ", sound: " + error.message;
            return false;
        }
        audible[at] = count > 0;
    }
    const MotionVideoOptions& options = request.options;
    std::vector<Value> outputs;
    if (options.preview) {
        for (size_t at = 0; at < items.size(); ++at) {
            outputs.push_back(MotionVideoItemReport(items[at], options,
                                                    "planned", audible[at]));
        }
        report = MotionVideoReport(outputs, true, true, items.size(), 0);
        return true;
    }
    // PRD2 -- every file of a request is encoded the same way: an encoder
    // this FFmpeg lacks is refused before the first frame is drawn, not
    // after the last.
    if (!items.empty()) {
        const ExportPlan encoding = MotionVideoExportPlan(
            items.front(), options, "%06d.png", std::string());
        std::string refusal;
        if (!ffmpeg::Require(options.ffmpeg_path, encoding.arguments,
                             refusal)) {
            error = {"ExportFailed", "output", refusal};
            return false;
        }
    }
    size_t completed = 0;
    for (size_t at = 0; at < items.size(); ++at) {
        const MotionVideoItem& item = items[at];
        const ExportTaskStep step{at, items.size()};
        std::string failure;
        Value sound;
        if (!request.runner) {
            failure =
                RenderItem(item, request, audible[at], step, nullptr, sound);
        } else {
            // PRD16 -- one piece of the task's media work per file. A file
            // committed as the cancellation came is complete: it is there.
            bool ran = false;
            const MediaTaskSnapshot finished = request.runner(
                MediaTaskKind::Export, "Render " + item.output_path,
                [&](MediaTaskContext& context, std::string& reason) {
                    ran = true;
                    failure = RenderItem(item, request, audible[at], step,
                                         &context, sound);
                    reason = failure;
                    return failure.empty();
                });
            if (finished.state == MediaTaskState::Cancelled &&
                (!ran || !failure.empty())) {
                failure = "render cancelled";
            } else if (finished.state == MediaTaskState::Failed &&
                       failure.empty()) {
                failure =
                    finished.error.empty() ? "render failed" : finished.error;
            } else if (finished.state == MediaTaskState::Succeeded) {
                failure.clear();
            }
        }
        if (!failure.empty()) {
            Value failed =
                MotionVideoItemReport(item, options, "failed", audible[at]);
            failed.Set("detail", Value::MakeString(failure));
            outputs.push_back(std::move(failed));
            report = MotionVideoReport(outputs, false, false, items.size(),
                                       completed);
            error = {
                "ExportFailed", "output",
                Label(item) + " -> '" + item.output_path + "': " + failure};
            return false;
        }
        Value complete =
            MotionVideoItemReport(item, options, "complete", audible[at]);
        if (sound.IsObject()) {
            complete.Set("loudness", sound);
        }
        outputs.push_back(std::move(complete));
        ++completed;
    }
    report = MotionVideoReport(outputs, true, false, items.size(), completed);
    return true;
}
