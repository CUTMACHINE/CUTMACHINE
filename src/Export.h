#pragma once

#include "CaptionRenderer.h"
#include "Document.h"
#include "FontCatalog.h"
#include "MediaTaskManager.h"
#include "MotionEvaluate.h"
#include "Project.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class ExportEncoder {
    HevcVideoToolbox,
    HevcSoftware,
    H264VideoToolbox,
    H264Software,
    // IA4 -- docs/roadmap/IA-2026-09.md. Not a delivery: FFV1 and 32-bit
    // float PCM in Matroska, without loss, so that a frame or a sample read
    // back from an inspection render is the export's composite itself and
    // not an encoder's approximation of it.
    LosslessInspection,
};

// PRD16 -- ROADMAP.md. The presets are the one vocabulary of export on every
// surface: the window lists them, `--export --preset` and the `preset`
// argument of export_timeline and export_timelines name them by their key.
// They used to be the window's alone, and each surface chose its own encoder
// when the caller named none: the CLI wrote HEVC Main10, export_timeline
// H.264 High, from the same project.
enum class ExportPresetId {
    HevcHighQuality,
    HevcMaster,
    HevcWeb1080,
    HevcUhdDelivery,
    // PRD16 -- H.264 High 8-bit at the sequence's format, as export_timeline
    // has delivered since September: the engine's default for an SDR
    // timeline (Exporter::DefaultPreset). Last in the list, so that the keys
    // already published keep their place; a menu preselects the default by
    // Exporter::PresetIndex, not by position.
    H264,
};

struct ExportPresetDescriptor {
    ExportPresetId id;
    // PRD16 -- the identifier every surface names the preset by: lowercase,
    // never translated, never renamed once published.
    const char* key;
    const char* name;
    const char* summary;
};

// VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. How an SDR delivery tells a
// player to show it. Rec709 is the broadcast tag (1-1-1): Apple's players
// decode it with the inverse BT.709 camera curve, about 1.96, and show the
// midtones 12 to 19 codes lighter than a player that reads the values as
// sRGB or BT.1886. Rec709A is DaVinci Resolve's tag for the same values,
// measured on two of its deliveries: colr 1-2-1 and a gama atom of 2.4,
// which ColorSync decodes as a pure 2.4 gamma -- the reference monitor's
// picture, and the one Record shows. The gama atom exists only in a
// QuickTime .mov; an .mp4 tagged 1-2-1 is shown exactly as 1-1-1, so
// Rec709A is refused there rather than delivered as a tag that lies.
enum class ExportSdrTag { Rec709, Rec709A };

// PA2 -- docs/roadmap/PARITE-2026-11.md. The loudness a delivery is brought
// to. The mix was summed and limited as it stood (audio_mix::
// AppendMixedOutput), "sans normalisation automatique": a film left at the
// level its rushes and its music happened to add up to, and every delivery
// was levelled by hand afterwards, or not at all.
//
// A target is a property of the DELIVERY, like its codec: it is asked of an
// export and is not in the document, and the monitor, the timeline's
// transcription and analyze_timeline_audio keep hearing the mix as it is
// cut. So nothing here touches the mix the three share. The film is
// rendered first with its sound lossless; that sound is measured as a
// broadcast meter does (EBU R 128, FFmpeg's ebur128); then one gain brings
// it to the target, the mix's own limiter holds its ceiling again, and the
// sound is encoded once, the picture copied as it is. Measured on the
// rendered file rather than predicted from the timeline, so that whatever
// the export mixes -- a motion clip's sound, a film rendered by segments --
// is in the measure.
struct ExportLoudnessTarget {
    bool requested = false;
    // Thousandths of a LUFS: -14000 is -14 LUFS.
    int32_t integrated_millilufs = -14000;
};

// The targets an export takes: from -40 to -5 LUFS.
inline constexpr int32_t kMinimumLoudnessTargetMillilufs = -40000;
inline constexpr int32_t kMaximumLoudnessTargetMillilufs = -5000;

// What bringing a file to its target measured and did. Filled by
// Exporter::Run, read by Exporter::ResultJson.
struct ExportLoudnessOutcome {
    bool applied = false;
    // The mix had no loudness to bring anywhere: left as it was.
    bool silent = false;
    double target_lufs = 0.0;
    // The mix as rendered, before the gain.
    std::optional<double> mix_lufs;
    std::optional<double> mix_true_peak_dbfs;
    double gain_db = 0.0;
    // The delivered file, measured again after the gain and the limiter.
    std::optional<double> delivered_lufs;
    std::optional<double> delivered_true_peak_dbfs;
};

struct ExportSettings {
    std::string output_path;
    int32_t width = 1920;
    int32_t height = 1080;
    MediaRate frame_rate{25, 1};
    ExportEncoder encoder = ExportEncoder::HevcVideoToolbox;
    int64_t video_bitrate = 45000000;
    int32_t audio_bitrate = 320000;
    int32_t audio_sample_rate = 48000;
    bool main10 = true;
    bool overwrite = false;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    ExportSdrTag sdr_tag = ExportSdrTag::Rec709;
    // IA4 -- docs/roadmap/IA-2026-09.md. A window of the timeline, in
    // sequence frames at `frame_rate`: [window_first, window_end). A zero
    // `window_end` is the entire timeline, which every delivery exports.
    // The window is rendered by the SAME graph, not by a second description
    // of the montage: the composite's black starts at the window's first
    // frame, the clips that touch no frame of it are left out, and every
    // other filter is the one the full export runs -- so frame k of the
    // window is frame window_first + k of the film.
    int64_t window_first = 0;
    int64_t window_end = 0;
    // IA4 -- with LosslessInspection only: the sequence frames kept of the
    // window, ascending. The graph still composes every frame -- a clip's
    // chain and every placement count them -- and keeps these at its very
    // end, so an inspection of a few frames of a 4K window does not store a
    // lossless second of it. Empty keeps every frame. The sound is kept
    // whole.
    std::vector<int64_t> inspection_frames;
    // Rendering by segments. One FFmpeg graph opens every clip of the film
    // at once, and each 4K decoder holds its own frames: 64 XAVC-I clips
    // measured 732 MB per decoder and got FFmpeg killed by the system on a
    // 16 GB Mac (24 September 2026, a 78 s fashion reel). An export of the
    // whole timeline with more inputs than this is rendered window by
    // window, each window opening only the clips it touches, losslessly,
    // then assembled and encoded once. Zero is the default limit; tests set
    // it low to force segments on a small timeline.
    int32_t max_inputs_per_pass = 0;
    // PA2 -- the loudness the delivery is brought to; not requested, the
    // mix is delivered as it stands. Ignored by an inspection render.
    ExportLoudnessTarget loudness;
};

struct ExportProgress {
    int64_t rendered_frames = 0;
    int64_t total_frames = 0;
    int64_t output_time_us = 0;
    double speed = 0.0;
    double fraction = 0.0;
    // PRD40 -- ROADMAP.md. A segmented export reads the film twice: its
    // segments, then the pass that assembles them. rendered_frames and
    // total_frames count both passes as one run (twice the film's frames),
    // which the window and the command line show as they always have; these
    // say which pass it is ("segment 2/3", "assembly") and count it against
    // the film's own frames, for get_task. Empty and zero for one pass.
    std::string pass;
    int64_t pass_frames = 0;
    int64_t pass_total = 0;
};

// LG3 -- docs/roadmap/LEGENDES-2026-10.md. The cartons of the rendered
// window, drawn by the caption renderer (CaptionRenderer.h) into one layer
// before FFmpeg starts: one PNG per distinct picture -- the cartons on
// screen together, in track order -- and, as for a motion clip, one
// numbered PNG per frame of the window, a hard link to the picture it
// shows. FFmpeg reads the sequence as an input with alpha and lays it on the
// delivered signal. The worker owns the directory's lifetime.
struct ExportCaptionAsset {
    std::string directory;
    // The delivered frame, and the width the styles are authored at.
    int32_t width = 0;
    int32_t height = 0;
    int32_t sequence_width = 0;
    // The window's cartons, their texts as the clips hold them: broken for
    // delivery (WrapCaptionTextsForDelivery) when drawn.
    std::vector<CaptionCarton> cartons;
    // Each distinct picture, as indices into `cartons` in drawing order;
    // the first is the empty one.
    std::vector<std::vector<size_t>> pictures;
    // PA4 -- docs/roadmap/PARITE-2026-11.md. Beside each carton of each
    // picture, how many of its words have started (CaptionCarton::
    // words_spoken), -1 for a carton drawn whole. A carton whose style
    // animates its words is one picture per word instead of one: the
    // pictures stay distinct states drawn once, and a frame still links to
    // the one it shows.
    std::vector<std::vector<int32_t>> picture_words;
    // The picture each frame of the window shows.
    std::vector<uint32_t> frames;
};

// F1.3 -- one derived grade LUT can serve several clips with the same
// resolved stack. The worker owns its temporary lifetime, like captions.
struct ExportLutAsset {
    std::string path;
    std::string cube;
};

// MOTION-2026-09 M4 -- one motion clip, rendered frame by frame into a
// temporary PNG sequence before FFmpeg starts, and read back by it as an
// input with alpha. `frames[i]` is the composition frame output frame i of
// the input shows (MotionClip.h); the worker renders each distinct one once
// and owns the directory's lifetime, like the other derived assets.
struct ExportMotionAsset {
    std::string directory;
    DocumentComposition composition;
    mcp_json::Value props;
    std::vector<int64_t> frames;
    // MOTION-DESIGN-2026-09 -- the composition drawn at the size the
    // sequence shows it (fitted, never enlarged): a 960 x 540 preview of a
    // 1920 x 1080 composition renders a quarter of the pixels.
    double render_scale = 1.0;
    // Relative image sources of the composition resolve here.
    std::string media_directory;
    // REMOTION-PARITY-2026-09 REM2 -- the project's media and the
    // compositions an instance shows (MotionReachFor).
    MotionReach reach;
    // REM2 -- when the composition plays a sound: samples [audio_first,
    // audio_first + audio_count) of its mix (MotionAudio.h), the part the
    // clip shows, written to `audio_path` beside the frames and read by
    // FFmpeg as one more input of the timeline's mix.
    bool audio = false;
    int64_t audio_first = 0;
    int64_t audio_count = 0;
    std::string audio_path;
};

struct ExportPlan {
    ExportSettings settings;
    Ulid timeline_id;
    RationalTime duration{0, 1};
    int64_t total_frames = 0;
    std::string filter_graph;
    std::vector<std::string> arguments;
    std::string temporary_output_path;
    std::string temporary_lut_path;
    std::string color_lut;
    // LG3 -- the window's cartons, when it shows any: one layer.
    std::vector<ExportCaptionAsset> caption_assets;
    std::vector<ExportLutAsset> clip_grade_luts;
    // F1.3 -- under colour management the display transform runs once, on
    // the linear composite: its cube is temporary_lut_path/color_lut, read
    // through this 1D shaper when grading in ACEScct, and clip_grade_luts
    // holds each clip's working-space cube, graded or not.
    ExportLutAsset display_shaper;
    // MOTION-2026-09 M4 -- the motion clips, in input order.
    std::vector<ExportMotionAsset> motion_assets;
    // Rendering by segments (ExportSettings::max_inputs_per_pass): when not
    // empty, these windows are rendered first, losslessly, into
    // segment_directory, listed in segment_list_path, and `arguments` is the
    // pass that reads that list back and encodes the delivery. The segments
    // own their own temporary assets.
    std::vector<ExportPlan> segments;
    std::string segment_directory;
    std::string segment_list_path;
    // A delivery's caption families that Core Text does not have, each
    // with the one the cartons are drawn in instead (FontCatalog.h). The
    // export goes ahead; ResultJson names them, and the window warns before
    // it starts. Empty for an inspection render, which is never delivered.
    std::vector<CaptionFontSubstitution> caption_font_substitutions;
    // PA2 -- set for a delivery with a loudness target; what Run measured
    // and applied. Shared rather than owned so that the plan a caller keeps
    // reads what the run, which takes it const, wrote.
    std::shared_ptr<ExportLoudnessOutcome> loudness_outcome;
};

using ExportProgressCallback = std::function<void(const ExportProgress&)>;

// PRD16 -- ROADMAP.md. Which file of how many an export is encoding: a
// batch encodes its files one after the other and stops at the first that
// fails, so `file` is also the number already complete.
struct ExportTaskStep {
    size_t file = 0;
    size_t files = 1;
};

// F1.3 -- true for an FFmpeg pixel format name that stores RGB (or a
// palette): such a picture has no YCbCr matrix or range of its own, so the
// ones the colour management declares for camera files do not apply to it.
// G1 -- docs/roadmap/GRADE-2026-09.md: shared with ColorMeasure.cc, which
// decodes a source the way the export does.
bool IsRgbPixelFormat(const std::string& name);

// G1 -- the bit depth of an FFmpeg pixel format's first component, the depth
// the monitor decodes a picture at (Renderer.mm); 0 for a name FFmpeg does
// not know.
int PixelFormatBitDepth(const std::string& name);

// Offline export is deliberately isolated from the realtime player. A plan is
// immutable and inspectable, so future Metal and distributed render backends
// can consume the same validated settings without changing the UI or presets.
class Exporter {
public:
    static const std::vector<ExportPresetDescriptor>& Presets();
    static ExportSettings SettingsForPreset(ExportPresetId preset,
                                            const std::string& outputPath,
                                            int32_t sourceWidth,
                                            int32_t sourceHeight,
                                            MediaRate sourceRate);
    // PRD16 -- the preset at the format of the timeline `timeline` holds.
    static ExportSettings SettingsForPreset(ExportPresetId preset,
                                            const Document& timeline,
                                            const std::string& outputPath);

    // PRD16 -- ROADMAP.md. The settings of an export whose caller named no
    // codec and no preset, on every surface: `--export`, export_timeline,
    // export_timelines and the window's preselection. H.264 High 8-bit for
    // an SDR timeline -- what the montages driven by an agent have delivered
    // since September, and what platforms accept most widely; HEVC Main10
    // (hevc_high_quality) when the sequence delivers HLG under colour
    // management, which eight bits would band. A surface that chose its own
    // encoder here is how the CLI and the catalog came to differ.
    static ExportPresetId DefaultPreset(const Document& timeline);
    static ExportSettings DefaultSettings(const Document& timeline,
                                          const std::string& outputPath);
    // PRD16 -- where `preset` stands in Presets(): what a menu built from
    // that list selects to offer the default,
    // PresetIndex(DefaultPreset(document)).
    static size_t PresetIndex(ExportPresetId preset);

    // PRD16 -- the preset `key` names (ExportPresetDescriptor::key); false
    // when none does.
    static bool FindPreset(const std::string& key, ExportPresetId& preset);
    // PRD16 -- the keys of Presets(), in order, joined by ", ".
    static std::string PresetKeys();
    // PRD16 -- a preset named by its key beside the codec named, if any
    // ("h264", "hevc", or empty when none was), read the same way by every
    // surface: false, with `error`, for a key no preset has (the keys are
    // listed) and for a preset that encodes another codec than the one
    // named. `argument` is how the surface spells the preset ("--preset",
    // "export_timeline.preset").
    static bool ReadPreset(const std::string& key, const std::string& codec,
                           const std::string& argument, ExportPresetId& preset,
                           std::string& error);
    // PRD16 -- `software: true` and `--software`: the software encoder of
    // the codec `settings` already encodes, x264 or x265.
    static void UseSoftwareEncoder(ExportSettings& settings);
    // PRD16 -- every timeline of `project`, "'Name' (ID)" in its order: what
    // a refusal of an unknown or missing timeline names, so that the caller
    // corrects in one turn.
    static std::string DescribeTimelines(const Project& project);

    static ExportSettings HevcMain10Preset(const std::string& outputPath);
    static ExportSettings HevcMain10SoftwarePreset(
        const std::string& outputPath);
    // ADOBE-CLI-2026-09 -- match-source delivery is resolved from the
    // addressed timeline, never from a fixed landscape output format.
    static ExportSettings H264Preset(const std::string& outputPath) {
        ExportSettings settings;
        settings.output_path = outputPath;
        settings.width = 0;
        settings.height = 0;
        settings.frame_rate = {0, 1};
        settings.encoder = ExportEncoder::H264VideoToolbox;
        settings.main10 = false;
        settings.video_bitrate = 20000000;
        return settings;
    }
    static bool IsH264(ExportEncoder encoder) {
        return encoder == ExportEncoder::H264VideoToolbox ||
               encoder == ExportEncoder::H264Software;
    }
    static std::string ResultJson(const ExportPlan& plan);

    // ADOBE-CLI-2026-09 -- validate every timeline and destination before
    // rendering the first item. Completed files remain reviewable on failure.
    // PRD16 -- an item with zero width and height is rendered at its
    // timeline's exact format, and with a zero frame rate at its cadence;
    // any other size or cadence is the item's own (a preset's). Each file is
    // encoded by RunPlan with `runner`: a background task follows the batch
    // file by file and stops it, the files already complete kept and listed
    // as after a failure.
    static bool ExportBatch(
        const Project& project, const std::string& projectPath,
        const std::vector<std::pair<Ulid, ExportSettings>>& items, bool preview,
        std::string& resultJson, std::string& errorName, std::string& error,
        const MediaTaskRunner& runner = {});

    static bool BuildPlan(const Document& document,
                          const std::string& documentPath,
                          const ExportSettings& settings, ExportPlan& output,
                          std::string& error);

    // Runs synchronously. Callers that need a responsive UI should run this on
    // a worker queue. Setting cancel to true terminates FFmpeg and removes the
    // incomplete temporary file; the destination is only replaced on success.
    static bool Run(const ExportPlan& plan,
                    const ExportProgressCallback& progress,
                    const std::atomic_bool* cancel, std::string& error);

    // PRD16 -- ROADMAP.md. Where a file of an export is encoded, and who
    // follows it. Without a runner, Run on the calling thread, neither
    // followed nor cancellable, as every synchronous surface has it. With a
    // background task's runner (McpSession.h), the file is one piece of the
    // task's media work: get_task reads its frames as the task's progress
    // (ProgressDetail), and cancel_task reaches Run's cancel flag, which
    // stops FFmpeg and removes the partial file. Whatever the runner ran in
    // Run's place (a test's stand-in), no partial file outlives a failure.
    static bool RunPlan(const ExportPlan& plan, const MediaTaskRunner& runner,
                        const ExportTaskStep& step, std::string& error);
    // PRD16 -- what get_task says while `step` encodes `path`, from what
    // FFmpeg reported: "frames 120/250 of <path>", and for a batch "file 2/3
    // (1 complete): frames 120/250 of <path>".
    static std::string ProgressDetail(const ExportTaskStep& step,
                                      const std::string& path,
                                      const ExportProgress& progress);
    // PRD16 -- the task's share done: the files complete, and the part of
    // the current one.
    static double TaskFraction(const ExportTaskStep& step, double fraction);
};
