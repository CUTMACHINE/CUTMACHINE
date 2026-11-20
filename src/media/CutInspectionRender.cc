#include "CutInspectionRender.h"

#include "ColorManagement.h"
#include "Export.h"
#include "FfmpegPipe.h"
#include "FrameCapture.h"
#include "InterviewShort.h"
#include "Transcription.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

// A temporary directory that goes away with the call, whatever it returns.
struct ScratchDirectory {
    std::filesystem::path path;
    ScratchDirectory()
        : path(std::filesystem::temp_directory_path() /
               ("cutmachine-inspection-" + GenerateUlid())) {
        std::filesystem::create_directories(path);
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
};

// One side of a picture scaled so that the sequence's longest side becomes
// `dimension`, kept even for the chroma of the JPEG.
int32_t CellSide(int32_t side, int32_t longest, int32_t dimension) {
    const int32_t value = static_cast<int32_t>(
        (static_cast<int64_t>(side) * dimension) / std::max(longest, 1));
    return std::max<int32_t>(2, value - (value % 2));
}

// `select` keeping the frames of the render at these indices: the render
// numbers the frames it kept from zero.
std::string SelectIndices(const std::vector<size_t>& indices) {
    std::ostringstream select;
    select << "select=";
    for (size_t index = 0; index < indices.size(); ++index)
        select << (index ? "+" : "") << "eq(n\\," << indices[index] << ')';
    return select.str();
}

}  // namespace

int32_t CutBandWidth(int32_t sequenceWidth, int32_t sequenceHeight,
                     int32_t cellDimension, int32_t before, int32_t after) {
    const int32_t longest = std::max(sequenceWidth, sequenceHeight);
    const int32_t cell = CellSide(sequenceWidth, longest, cellDimension);
    const auto row = [&](int32_t count) {
        return count <= 0 ? 0 : (count * cell) + ((count - 1) * 4);
    };
    return row(before) + 8 + row(after);
}

bool StackInspectionPictures(const std::vector<std::string>& jpegs,
                             const std::string& ffmpegPath, std::string& jpeg,
                             std::string& error) {
    jpeg.clear();
    error.clear();
    if (jpegs.empty()) {
        error = "no picture to stack";
        return false;
    }
    if (jpegs.size() == 1) {
        jpeg = jpegs.front();
        return true;
    }
    const ScratchDirectory scratch;
    std::vector<std::string> arguments = {ffmpegPath, "-hide_banner",
                                          "-loglevel", "error", "-nostdin"};
    std::ostringstream graph;
    for (size_t index = 0; index < jpegs.size(); ++index) {
        const std::filesystem::path path =
            scratch.path / ("band-" + std::to_string(index) + ".jpg");
        std::ofstream output(path, std::ios::binary);
        output.write(jpegs[index].data(),
                     static_cast<std::streamsize>(jpegs[index].size()));
        output.close();
        arguments.insert(arguments.end(), {"-i", path.string()});
        // Each picture but the last carries the gap under it.
        graph << '[' << index << ":v]format=yuvj420p";
        if (index + 1 < jpegs.size())
            graph << ",pad=w=iw:h=ih+8:x=0:y=0:color=0x303030";
        graph << "[p" << index << "];";
    }
    for (size_t index = 0; index < jpegs.size(); ++index)
        graph << "[p" << index << ']';
    graph << "vstack=inputs=" << jpegs.size() << "[out]";
    const std::filesystem::path stacked = scratch.path / "sheet.jpg";
    arguments.insert(arguments.end(),
                     {"-filter_complex", graph.str(), "-map", "[out]",
                      "-frames:v", "1", "-q:v", "5", "-y", stacked.string()});
    std::string runError;
    if (!ReadFfmpegRawFrames(arguments, 1, [](const uint8_t*) {}, runError)) {
        error =
            "the pictures could not be stacked (they must share a "
            "width): " +
            runError;
        return false;
    }
    std::ifstream input(stacked, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    jpeg = bytes.str();
    if (jpeg.empty()) {
        error = "the stacked picture came out empty";
        return false;
    }
    return true;
}

bool RenderCutInspection(const Document& document,
                         const std::filesystem::path& projectPath,
                         const CutInspectionPlan& plan,
                         const CutInspectionRenderSettings& settings,
                         CutInspectionResult& result, std::string& error) {
    result = CutInspectionResult{};
    error.clear();
    if (plan.shown.empty() || plan.end <= plan.first) {
        error = "the inspection plan holds no frame";
        return false;
    }
    if (settings.cell_dimension < 16 || settings.cell_dimension > 1920 ||
        settings.columns < 1) {
        error = "invalid inspection picture size";
        return false;
    }
    // The configured model is resolved before anything renders: a request
    // for a reading that cannot be made is refused, not half answered.
    WhisperSettings whisper;
    if (settings.transcribe) {
        std::string reason;
        if (!ResolveConfiguredWhisperModel(whisper.whisper_model_path,
                                           reason)) {
            error = reason;
            return false;
        }
        whisper.ffmpeg_path = settings.ffmpeg_path;
        whisper.language = settings.language;
    }

    // HLG is looked at through the SDR view, as Record and the sheets show
    // it; everything else is the delivery's own graph.
    Document film = document;
    if (film.color_management.enabled &&
        film.color_management.output_transfer == "hlg") {
        film.color_management =
            ColorManagementForSdrPreview(film.color_management);
    }

    const ScratchDirectory scratch;
    const std::filesystem::path render = scratch.path / "window.mkv";
    ExportSettings exportSettings =
        Exporter::HevcMain10SoftwarePreset(render.string());
    exportSettings.encoder = ExportEncoder::LosslessInspection;
    // 6 November 2026 -- the window is rendered no larger than its pictures
    // need: twice the longest side of a cell or of an analysis plane, never
    // more than the sequence. Every picture below is scaled from the render
    // to an explicit size, so this changes how much they are reduced, not
    // what they show; the export graph composes captions, motion, blur and
    // framing for whatever size it delivers, as it does for a 1080 export
    // of a 4K sequence. At full size the graph -- 32-bit float at
    // 1080x1920, then lossless -- took 5.5 s per junction of a bench review
    // (199 s for 36 cuts), for pictures 240 pixels tall.
    const int32_t sequenceLongest =
        std::max(film.sequence.width, film.sequence.height);
    const int32_t renderLongest = std::min(
        sequenceLongest,
        2 * std::max(settings.cell_dimension, settings.analysis_dimension));
    exportSettings.width =
        renderLongest < sequenceLongest
            ? CellSide(film.sequence.width, sequenceLongest, renderLongest)
            : film.sequence.width;
    exportSettings.height =
        renderLongest < sequenceLongest
            ? CellSide(film.sequence.height, sequenceLongest, renderLongest)
            : film.sequence.height;
    exportSettings.frame_rate = plan.rate;
    exportSettings.ffmpeg_path = settings.ffmpeg_path;
    exportSettings.window_first = plan.first;
    exportSettings.window_end = plan.end;
    exportSettings.inspection_frames = plan.shown;
    ExportPlan exportPlan;
    if (!Exporter::BuildPlan(film, projectPath.string(), exportSettings,
                             exportPlan, error) ||
        !Exporter::Run(exportPlan, {}, nullptr, error)) {
        error = "the window did not render: " + error;
        return false;
    }

    // The pictures, each scaled to fit a cell, in the order of the plan;
    // then written as JPEG the way every picture the tools hand out is
    // (kJpegFromVideoFilter). The render kept `shown` only, in its order,
    // so a frame is selected by its index there.
    const std::vector<int64_t>& pictured =
        settings.strip_frames.empty() ? plan.shown : settings.strip_frames;
    std::vector<size_t> beforeIndices;
    std::vector<size_t> afterIndices;
    std::vector<size_t> allIndices;
    for (const int64_t frame : pictured) {
        const auto found =
            std::find(plan.shown.begin(), plan.shown.end(), frame);
        if (found == plan.shown.end()) {
            error = "frame " + std::to_string(frame) +
                    " is pictured but not among the frames the plan keeps";
            return false;
        }
        const size_t index = static_cast<size_t>(found - plan.shown.begin());
        allIndices.push_back(index);
        (frame < plan.cut ? beforeIndices : afterIndices).push_back(index);
    }
    const int32_t longest = std::max(film.sequence.width, film.sequence.height);
    const int32_t cellWidth =
        CellSide(film.sequence.width, longest, settings.cell_dimension);
    const int32_t cellHeight =
        CellSide(film.sequence.height, longest, settings.cell_dimension);
    std::ostringstream cellScale;
    cellScale << "scale=" << cellWidth << ':' << cellHeight << ":flags=lanczos";
    std::ostringstream graph;
    const bool band =
        settings.cut_band && !beforeIndices.empty() && !afterIndices.empty();
    if (band) {
        // RC2 -- before | red bar | after, the waveform under them.
        const int32_t before = static_cast<int32_t>(beforeIndices.size());
        const int32_t after = static_cast<int32_t>(afterIndices.size());
        const int32_t width =
            CutBandWidth(film.sequence.width, film.sequence.height,
                         settings.cell_dimension, before, after);
        const int32_t waveHeight =
            std::max<int32_t>(48, (cellHeight / 2) - ((cellHeight / 2) % 2));
        const int64_t length = plan.end - plan.first;
        const auto xAt = [&](int64_t numerator, int64_t denominator) {
            return static_cast<int32_t>((numerator * width) / denominator);
        };
        graph << "[0:v:0]split=2[vb][va];[vb]" << SelectIndices(beforeIndices)
              << ',' << cellScale.str() << ",tile=" << before
              << "x1:padding=4:color=black,pad=w=iw+8:h=ih:x=0:y=0:color=red"
              << "[b];[va]" << SelectIndices(afterIndices) << ','
              << cellScale.str() << ",tile=" << after
              << "x1:padding=4:color=black[a];"
              << "[b][a]hstack=inputs=2,format=yuv444p[strip];"
              << "[0:a:0]showwavespic=s=" << width << 'x' << waveHeight
              << ":split_channels=0:colors=0xb4b4b4:scale=sqrt:draw=full,"
              << "format=rgb24";
        for (const int64_t frame : pictured) {
            const int32_t x = std::clamp<int32_t>(
                xAt((2 * (frame - plan.first)) + 1, 2 * length) - 1, 0,
                width - 2);
            graph << ",drawbox=x=" << x << ":y=0:w=2:h=" << (waveHeight / 4)
                  << ":color=0x808080:t=fill";
        }
        const int32_t cutX = std::clamp<int32_t>(
            xAt(plan.cut - plan.first, length) - 1, 0, width - 3);
        graph << ",drawbox=x=" << cutX << ":y=0:w=3:h=" << waveHeight
              << ":color=red:t=fill"
              << ",scale=out_color_matrix=bt709:out_range=tv,format=yuv444p"
              << "[wave];[strip][wave]vstack=inputs=2";
        if (settings.band_width > width) {
            graph << ",pad=w=" << settings.band_width
                  << ":h=ih:x=0:y=0:color=black";
        }
        graph << ',' << kJpegFromVideoFilter << "[out]";
    } else {
        const int32_t count = static_cast<int32_t>(allIndices.size());
        // As few rows as the column bound allows, then as few columns as
        // those rows need: six pictures are three by two, not four and two
        // holes.
        const int32_t rows = (count + std::min(settings.columns, count) - 1) /
                             std::min(settings.columns, count);
        const int32_t columns = (count + rows - 1) / rows;
        graph << "[0:v:0]";
        if (!settings.strip_frames.empty())
            graph << SelectIndices(allIndices) << ',';
        graph << cellScale.str() << ",tile=" << columns << 'x' << rows
              << ":padding=4:color=black," << kJpegFromVideoFilter << "[out]";
    }
    const std::filesystem::path image = scratch.path / "window.jpg";
    std::string runError;
    if (!ReadFfmpegRawFrames(
            {settings.ffmpeg_path, "-hide_banner", "-loglevel", "error",
             "-nostdin", "-i", render.string(), "-filter_complex", graph.str(),
             "-map", "[out]", "-frames:v", "1", "-q:v",
             std::to_string(settings.jpeg_quality), "-y", image.string()},
            1, [](const uint8_t*) {}, runError)) {
        error = "the window's pictures did not render: " + runError;
        return false;
    }
    {
        std::ifstream input(image, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        result.jpeg = bytes.str();
    }
    if (result.jpeg.empty()) {
        error = "the window's pictures came out empty";
        return false;
    }

    // The mix, whole, as the render stored it: interleaved stereo float.
    std::vector<float> samples;
    if (!ReadFfmpegRawFrames(
            {settings.ffmpeg_path, "-hide_banner", "-loglevel", "error",
             "-nostdin", "-i", render.string(), "-map", "0:a:0", "-f", "f32le",
             "-ac", "2", "-"},
            2 * sizeof(float),
            [&](const uint8_t* frame) {
                float pair[2];
                std::memcpy(pair, frame, sizeof(pair));
                samples.push_back(pair[0]);
                samples.push_back(pair[1]);
            },
            runError)) {
        error = "the window's sound could not be read: " + runError;
        return false;
    }
    if (!MeasureCutInspectionAudio(samples, exportSettings.audio_sample_rate,
                                   plan, result.audio, error)) {
        return false;
    }
    if (settings.keep_samples) result.samples = std::move(samples);

    // RC1 -- the frames the review measures, small and grey: the luma
    // plane as the render holds it, averaged down.
    if (settings.analysis_dimension > 0) {
        const int32_t dimension = std::max<int32_t>(
            8, settings.analysis_dimension - (settings.analysis_dimension % 2));
        result.analysis_width =
            CellSide(film.sequence.width, longest, dimension);
        result.analysis_height =
            CellSide(film.sequence.height, longest, dimension);
        const size_t planeSize = static_cast<size_t>(result.analysis_width) *
                                 static_cast<size_t>(result.analysis_height);
        std::ostringstream scale;
        scale << "scale=" << result.analysis_width << ':'
              << result.analysis_height << ":flags=area,format=gray";
        if (!ReadFfmpegRawFrames(
                {settings.ffmpeg_path, "-hide_banner", "-loglevel", "error",
                 "-nostdin", "-i", render.string(), "-map", "0:v:0", "-vf",
                 scale.str(), "-fps_mode", "passthrough", "-f", "rawvideo",
                 "-"},
                planeSize,
                [&](const uint8_t* plane) {
                    result.analysis_planes.emplace_back(plane,
                                                        plane + planeSize);
                },
                runError)) {
            error = "the window's frames could not be measured: " + runError;
            return false;
        }
        if (result.analysis_planes.size() != plan.shown.size()) {
            error = "the window rendered " +
                    std::to_string(result.analysis_planes.size()) +
                    " frames to measure, " + std::to_string(plan.shown.size()) +
                    " expected";
            return false;
        }
    }

    // The words the montage is expected to say there, from the project's
    // transcripts and alignments, as get_timeline_transcript places them.
    std::vector<TimelineTranscriptSpan> spans;
    const std::filesystem::path transcripts =
        std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
        "transcripts";
    std::string wordsError;
    if (BuildTimelineTranscriptWordSpans(document, transcripts, spans,
                                         wordsError)) {
        CollectExpectedWords(spans, plan, result.words);
    } else {
        result.words = CutInspectionWords{};
        result.words.expected_unavailable = wordsError;
    }

    if (!settings.transcribe) return true;
    // A fresh reading of the window's mix: decoded to Whisper's 16 kHz mono
    // straight from the render, never from the rushes.
    const std::filesystem::path transcriptPath = scratch.path / "reading.json";
    MediaTaskManager::Work work = [&](MediaTaskContext& context,
                                      std::string& taskError) {
        std::vector<float> pcm;
        if (!DecodeFfmpegAudioToPcm16k(
                settings.ffmpeg_path,
                {"-hide_banner", "-loglevel", "error", "-nostdin", "-i",
                 render.string(), "-map", "0:a:0", "-ac", "1", "-ar", "16000",
                 "-f", "f32le", "-"},
                context, pcm, taskError)) {
            return false;
        }
        return GenerateAudioTranscriptFromPcm(pcm, transcriptPath.string(),
                                              "inspection", plan.rate, whisper,
                                              context, taskError);
    };
    MediaTaskSnapshot snapshot;
    if (settings.runner) {
        snapshot = settings.runner(MediaTaskKind::Transcription,
                                   "Lecture d'un raccord", std::move(work));
    } else {
        MediaTaskManager tasks(1);
        snapshot = RunMediaTaskAndWait(tasks, MediaTaskKind::Transcription,
                                       "Lecture d'un raccord", std::move(work));
    }
    if (snapshot.state != MediaTaskState::Succeeded) {
        error = "the reading of the window failed: " +
                (snapshot.error.empty()
                     ? std::string(MediaTaskStateName(snapshot.state))
                     : snapshot.error);
        return false;
    }
    Transcript reading;
    if (!LoadAudioTranscript(transcriptPath.string(), reading, error)) {
        error = "the reading of the window could not be read back: " + error;
        return false;
    }
    const RationalTime start{plan.first * plan.rate.den, plan.rate.num};
    std::vector<CutInspectionWord> heard;
    for (const TranscriptWord& word : reading.words) {
        CutInspectionWord placed;
        placed.text = word.text;
        placed.timeline_in = start.add(word.start);
        placed.duration = word.end.sub(word.start);
        heard.push_back(std::move(placed));
    }
    CompareHeardWords(heard, plan, result.words);
    return true;
}
