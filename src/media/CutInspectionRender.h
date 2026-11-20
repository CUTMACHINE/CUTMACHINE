#pragma once

// IA4 -- docs/roadmap/IA-2026-09.md. The rendering half of an inspection
// (CutInspection.h has the portable half): the window is rendered by the
// export's own graph, without loss (Export.h, LosslessInspection), and read
// back -- the frames CutInspectionPlan::shown names as one JPEG strip, the
// whole window's mix as float samples for the levels, and, when asked and a
// local Whisper model is configured, a fresh reading of that mix.
//
// A delivery in HLG is inspected through the SDR view Record and the sheets
// show (ColorManagementForSdrPreview): a JPEG of HLG code values read as SDR
// would be a picture nobody sees. Everything else is the export's graph.

#include "CutInspection.h"
#include "Document.h"
#include "MediaTaskManager.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct CutInspectionRenderSettings {
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    // Longest side of each picture of the strip, in pixels.
    int32_t cell_dimension = 360;
    int32_t columns = 4;
    int32_t jpeg_quality = 5;
    // A reading of the window's mix by the configured local Whisper model
    // (ResolveConfiguredWhisperModel). Refused by name when none is.
    bool transcribe = false;
    std::string language = "auto";
    // Where the reading runs; empty, a private manager the call waits on.
    MediaTaskRunner runner;

    // RC2 -- docs/roadmap/RACCORDS-2026-10.md. The frames of the plan's
    // `shown` the picture holds, ascending; empty, all of them. A review
    // keeps frames for its measures that it does not need to show.
    std::vector<int64_t> strip_frames;
    // RC2 -- the picture as a cut band: the frames before the cut in one
    // row, a red bar, the frames from the cut, and under them the waveform
    // of the window's whole mix with the cut drawn in red and a grey tick
    // where each picture sits. Without a picture on one side of the cut
    // the picture falls back to the grid.
    bool cut_band = false;
    // RC2 -- the band's width in pixels, padded with black on the right;
    // 0 keeps its own. Bands of one sheet share a width this way even when
    // a window clipped at the timeline's edge shows fewer pictures.
    int32_t band_width = 0;
    // RC1 -- grey planes of every frame of `shown`, for the measures of a
    // review (CutReview.h): the longest side this many pixels, the other in
    // proportion, both even. 0 decodes none.
    int32_t analysis_dimension = 0;
    // RC1 -- keep the window's mix in the result, as the levels read it.
    bool keep_samples = false;
};

struct CutInspectionResult {
    CutInspectionAudio audio;
    CutInspectionWords words;
    std::string jpeg;
    // RC1 -- with analysis_dimension: one 8-bit grey plane per frame of
    // `shown`, in its order, each analysis_width * analysis_height.
    std::vector<std::vector<uint8_t>> analysis_planes;
    int32_t analysis_width = 0;
    int32_t analysis_height = 0;
    // RC1 -- with keep_samples: the window's mix, interleaved stereo float
    // at `audio.sample_rate`, from the window's first frame.
    std::vector<float> samples;
};

// RC2 -- the width of a cut band with `before` and `after` pictures of a
// sequence, each scaled so that its longest side is `cellDimension`: the
// cells, four pixels between two of one side, the eight-pixel red bar.
int32_t CutBandWidth(int32_t sequenceWidth, int32_t sequenceHeight,
                     int32_t cellDimension, int32_t before, int32_t after);

// RC2 -- JPEG pictures of one width stacked into one, top to bottom, eight
// dark pixels between two. `ffmpegPath` empty: the binary FfmpegBinary.h
// resolves.
bool StackInspectionPictures(const std::vector<std::string>& jpegs,
                             const std::string& ffmpegPath, std::string& jpeg,
                             std::string& error);

// `projectPath` is the project file; relative media and the project's
// `.cutmachine/transcripts` (the expected words) resolve beside it.
bool RenderCutInspection(const Document& document,
                         const std::filesystem::path& projectPath,
                         const CutInspectionPlan& plan,
                         const CutInspectionRenderSettings& settings,
                         CutInspectionResult& result, std::string& error);
