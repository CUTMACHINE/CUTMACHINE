#pragma once

// MOTION-2026-09 M6 -- ROADMAP.md, docs/MOTION_FORMAT.md. A composition
// rendered to video on its own, without a timeline: every version in every
// format asked for, in one call -- the case that opened the lot, three
// versions of one card in 9:16 and 16:9.
//
// This half is portable: which files, where, and how FFmpeg encodes them.
// It refuses what it can before the first frame is drawn, as the batch export
// of whole timelines does (ADOBE-CLI-2026-09): a batch that fails on its
// fifth file after writing four has to be sorted out by hand. The frames are
// drawn by Core Text and Core Graphics (MotionVideoRender.h), and encoded by
// Exporter::Run, which the timeline export already trusts with its temporary
// file and its commit on exFAT delivery drives.
//
// Two outputs, chosen for where a card goes next:
//   - with alpha: ProRes 4444 in .mov, straight alpha, 10 bits -- what a
//     carton needs to sit over an edit in Resolve;
//   - without: H.264 High 8-bit 4:2:0, flattened over black, with the
//     encoder, bit rate and tags of the timeline's H.264 delivery
//     (Exporter::H264Preset).

#include "Export.h"
#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

struct MotionVideoOptions {
    // Every destination, .mov or .mp4: "{variant}" and "{format}" stand for
    // the names, so one pattern names every file of a batch.
    std::string output;
    // Versions and formats to render, in this order. Empty: every one the
    // composition declares -- or, when it declares none, its own prop values
    // or its own size alone.
    std::vector<std::string> variants;
    std::vector<std::string> formats;
    bool alpha = false;
    bool overwrite = false;
    // Plans and checks everything, encodes nothing.
    bool preview = false;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    // MOTION-MARQUE-2026-10 MM1 -- a delivery loudness: the composition's
    // mix brought to `loudness` LUFS integrated, its true peak under
    // `true_peak` dBTP (Loudness.h), checked through the codec, and the
    // file's sound measured in the report. Unset: the mix as it sums.
    std::optional<double> loudness;
    double true_peak = -1.0;
};

struct MotionVideoItem {
    // Empty: the composition's own prop values, or its own size.
    std::string variant;
    std::string format;
    // Absolute and normalised.
    std::string output_path;
    // Sized to the format.
    MotionComposition composition;
    // The format's then the version's prop values (ResolveMotionVariant).
    mcp_json::Value props;
};

// Every version x format asked for, versions outermost, each with its
// destination. Refused before anything is written: an unknown version or
// format (UnknownVariant, UnknownFormat), and as InvalidExport a destination
// that is not .mov or .mp4 (only .mov with alpha), a placeholder that names
// nothing, two items on one path, a missing directory, an existing file
// without `overwrite`, or odd dimensions for H.264's 4:2:0.
bool PlanMotionVideos(const MotionComposition& composition,
                      const MotionVideoOptions& options,
                      std::vector<MotionVideoItem>& items, MotionError& error);

// How one item is encoded: FFmpeg reads `framePattern`, a PNG sequence
// numbered from 0 as printf would ("%06d.png"), at the composition's exact
// cadence, and writes exactly its duration in frames to a temporary file
// that Exporter::Run commits to the destination. REM2 -- with `audioPath`,
// the composition's own mix (MotionAudio.h) as its sound track: 24-bit PCM
// in a .mov, AAC in an .mp4.
ExportPlan MotionVideoExportPlan(const MotionVideoItem& item,
                                 const MotionVideoOptions& options,
                                 const std::string& framePattern,
                                 const std::string& audioPath = std::string());

// One line of the report: names, destination, codec, size, cadence, frames
// and `status` (planned, complete or failed). REM2 -- `audio` says whether
// the file carries the composition's sound.
mcp_json::Value MotionVideoItemReport(const MotionVideoItem& item,
                                      const MotionVideoOptions& options,
                                      const std::string& status,
                                      bool audio = false);

// MM1 -- the loudness a request asks for, refused when out of reason: an
// integrated loudness from -40 to -5 LUFS, a ceiling from -9 to 0 dBTP.
bool ValidMotionLoudness(const MotionVideoOptions& options, MotionError& error);

// The whole report: {"ok", "preview", "total", "completed", "outputs"}, the
// shape of the timeline batch export's.
std::string MotionVideoReport(const std::vector<mcp_json::Value>& outputs,
                              bool ok, bool preview, size_t total,
                              size_t completed);

// What a surface of the model hands the renderer: the composition, what to
// render, where its relative images resolve, the fonts to register.
struct MotionVideoRequest {
    const MotionComposition* composition = nullptr;
    MotionVideoOptions options;
    std::string media_directory;
    std::vector<std::string> font_directories;
    // REMOTION-PARITY-2026-09 REM2 -- the project's media and the
    // compositions an instance may show.
    MotionReach reach;
    // PRD16 -- ROADMAP.md. A background task's runner
    // (McpBackend::BackgroundTaskRunner): each file is then one piece of its
    // media work, its frames drawn and encoded as the task's progress, and
    // cancel_task stops it between two frames or kills FFmpeg. Empty
    // everywhere else: the files are rendered on the calling thread.
    MediaTaskRunner runner;
};

// Fills `report` on success, and on a failure after the first file was
// started (error.code ExportFailed): the files already complete stay, and
// are listed. A refusal before any encoding leaves `report` empty.
using MotionVideoRenderer = bool (*)(const MotionVideoRequest& request,
                                     std::string& report, MotionError& error);

// Installed by the executable beside the frame renderer
// (MotionFrameRenderer.h says why); null in a process that links the model
// alone.
namespace motion_video_renderer {
inline std::atomic<MotionVideoRenderer>& Slot() {
    static std::atomic<MotionVideoRenderer> renderer{nullptr};
    return renderer;
}
}  // namespace motion_video_renderer

inline void SetMotionVideoRenderer(MotionVideoRenderer renderer) {
    motion_video_renderer::Slot().store(renderer);
}

inline MotionVideoRenderer CurrentMotionVideoRenderer() {
    return motion_video_renderer::Slot().load();
}
