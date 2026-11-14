// Visual QA of 19 September 2026 -- VISUAL_QA_CHECKLIST.md, 2 ter and
// 5 quinquies. Compared by eye, the Record monitor and the exported file
// opened in Preview disagreed: a graded shot looked brighter or darker, a
// motion clip without colour management had other colours. Two pictures
// on two screens through two viewers cannot tell a real gap from the
// protocol; this test can. For the same frame of the same timeline, it puts
// the values Renderer.mm hands the display (RenderFramesToPixels, the
// passes RenderFrames presents) beside the exported file decoded back to
// RGB, patch by patch.
//
// The monitor side is assembled from the pieces main.mm uses for Record:
// the frames the app's DecodeWorker decodes, motion pictures from
// RenderMotionPreview, Timeline's layers and opacities, ResolveColorGrade,
// the SDR preview. main.mm's own loop, its caches and its hold policy stay
// out of reach: a divergence there would not show here.
//
// R6 -- docs/roadmap/REEL-2026-09.md. A sequence look LUT is read by two
// different programs from ONE file: Metal samples it as a 3D texture, FFmpeg
// reads it with lut3d. Nothing but this test says the two agree, and where
// the look sits in the managed graph -- after every clip's grade, before the
// display transform -- is exactly the kind of stage R5 got wrong by 0.26.
//
// Without colour management, and for a graded shot with it, the two agreed
// within 0.01 from the first run: the gaps seen by eye were the protocol.
// Two real ones were found. The managed monitor sent the display up to
// 1.06 where the file holds 1.0 (displaySignal now bounds it), and the
// export's mask of a motion layer's coverage read 0.9961 instead of 1, so
// that a highlight under a translucent title showed through by 0.06
// (UnitCubeText in Export.cc).
//
// COLOR-T (4 October 2026). "Very yellow on the timeline, not in the
// exports": the shots of SERIE_Hugo and SERIE_Camille carry the stack
// propose_grade writes -- a temperature far from the as-shot balance, a
// tint, an exposure -- on Sony FX30 rushes. Measured on the real rushes,
// Record and an export made the same day agreed on the temperature; the
// yellow came from the media gaining their as-shot balance, in a relink,
// after the films were exported. What this test did not hold either
// way: its monitor resolved every stack without the clip's context, so a
// temperature was drawn as main.mm never draws it, and no case read an
// untagged rush, the one picture whose YCbCr matrix the export left to
// swscale (ColorManagement.h, ResolveUntaggedColorInput) -- 0.011 to 0.019
// in green on a face, monitor against file.

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ColorEffects.h"
#include "DecodeWorker.h"
#include "Document.h"
#include "Export.h"
#include "FrameCache.h"
#include "Ingest.h"
#include "Json.h"
#include "LookLut.h"
#include "LookLutStorage.h"
#include "MotionCli.h"
#include "MotionClipPreview.h"
#include "MotionClipPreviewRender.h"
#include "PerformanceMetrics.h"
#include "Renderer.h"
#include "Timeline.h"
#include "TransitionRender.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr int kWidth = 320;
constexpr int kHeight = 180;
constexpr int kColumns = 4;
constexpr int kRows = 2;
constexpr int kPatchWidth = kWidth / kColumns;
constexpr int kPatchHeight = kHeight / kRows;
constexpr int64_t kFrame = 12;
// About five codes in eight bits: below what an eye catches between two
// pictures side by side, well above what HEVC at CRF 18 does to a flat
// patch.
constexpr double kTolerance = 0.02;
// EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred picture, compared pixel
// by pixel over the whole frame (RunCase): the mean gap here, the largest
// one per case (Case::blur_largest_tolerance). Measured on 29 October 2026
// with `gblur` at 6 steps (ClipBlur.h): mean 0.0019 to 0.0040, largest
// 0.019 to 0.037 without colour management or on a graphic, 0.072 on the
// S-Log3 board, where the display transform is steep enough to turn the
// two blurs' few thousandths into that much red. The same board, sharp,
// already differs by 0.086 at its edges pixel for pixel (chroma
// subsampling, the codec); its patch means are what kTolerance holds.
constexpr double kBlurMeanTolerance = 0.006;

// Dark to light greys, then a skin tone, a blue and a green: the grade and
// the transforms bend each differently.
const std::array<const char*, kColumns * kRows> kPatches = {
    "0x101010", "0x404040", "0x808080", "0xD0D0D0",
    "0xEBEBEB", "0xB07050", "0x3060A0", "0x50A040",
};

// A white band over a charter magenta band, the colours of the QA's title,
// with no text: the result does not depend on an installed font.
const char kDefinition[] =
    R"({"width":320,"height":180,"fps":25,"duration":50,)"
    R"("root":{"type":"fill","uid":"01K90000000000000000000011",)"
    R"("style":{"flexDirection":"column"},"children":[)"
    R"({"type":"box","uid":"01K90000000000000000000012",)"
    R"("style":{"width":"100%","height":"50%","backgroundColor":"#FFFFFF"}},)"
    R"({"type":"box","uid":"01K90000000000000000000013",)"
    R"("style":{"width":"100%","height":"50%","backgroundColor":"#E6007E"}}]}})";

std::string Quote(const std::string& value) { return "'" + value + "'"; }

// R6 -- the look under test: a contrast around 0.4 with a different gain per
// channel, which is what a grading LUT does and what the identity is not.
// Different on each channel so that a monitor and a file that swapped two of
// them, or laid the LUT at another stage of the managed graph, cannot come
// out equal by symmetry. Small and written here rather than committed: a
// .cube of a real look belongs to its author (guard rail 7).
constexpr int kLookSize = 33;

std::string LookCubeText() {
    const double gains[3] = {1.15, 1.0, 0.85};
    const double mid = 0.4;
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "TITLE \"CUTMACHINE parity look\"\nLUT_3D_SIZE " << kLookSize
         << '\n'
         << std::fixed << std::setprecision(6);
    for (int blue = 0; blue < kLookSize; ++blue) {
        for (int green = 0; green < kLookSize; ++green) {
            for (int red = 0; red < kLookSize; ++red) {
                const int index[3] = {red, green, blue};
                for (int channel = 0; channel < 3; ++channel) {
                    const double input =
                        index[channel] / static_cast<double>(kLookSize - 1);
                    const double output = std::clamp(
                        mid + (input - mid) * gains[channel], 0.0, 1.0);
                    text << output << (channel == 2 ? '\n' : ' ');
                }
            }
        }
    }
    return text.str();
}

bool WritePatchSource(const std::filesystem::path& path) {
    std::string command =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error";
    for (const char* color : kPatches)
        command += std::string(" -f lavfi -i color=c=") + color +
                   ":s=" + std::to_string(kPatchWidth) + 'x' +
                   std::to_string(kPatchHeight) + ":r=25:d=2";
    command +=
        " -filter_complex '[0][1][2][3]hstack=4[top];[4][5][6][7]hstack=4"
        "[bottom];[top][bottom]vstack,scale=out_color_matrix=bt709:"
        "out_range=tv,format=yuv422p10le' -c:v prores_ks -profile:v 3 "
        "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -y " +
        Quote(path.string());
    return std::system(command.c_str()) == 0;
}

// COLOR-T -- the same patches as an FX30 records its rushes: 4:2:2 10-bit,
// full range, coded with the BT.709 matrix and tagged with nothing but the
// range. Lossless (FFV1, which every FFmpeg has) where the camera
// compresses, so that what is compared is the reading, not the codec.
bool WriteCameraSource(const std::filesystem::path& path) {
    std::string command =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error";
    for (const char* color : kPatches)
        command += std::string(" -f lavfi -i color=c=") + color +
                   ":s=" + std::to_string(kPatchWidth) + 'x' +
                   std::to_string(kPatchHeight) + ":r=25:d=2";
    command +=
        " -filter_complex '[0][1][2][3]hstack=4[top];[4][5][6][7]hstack=4"
        "[bottom];[top][bottom]vstack,scale=out_color_matrix=bt709:"
        "out_range=pc,format=yuv422p10le,setparams=range=pc:colorspace="
        "unknown:color_primaries=unknown:color_trc=unknown' -c:v ffv1 "
        "-color_range pc -y " +
        Quote(path.string());
    return std::system(command.c_str()) == 0;
}

struct Rgb {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

double Distance(const Rgb& left, const Rgb& right) {
    return std::max({std::abs(left.red - right.red),
                     std::abs(left.green - right.green),
                     std::abs(left.blue - right.blue)});
}

std::string Describe(const Rgb& color) {
    char text[64];
    std::snprintf(text, sizeof(text), "(%.4f %.4f %.4f)", color.red,
                  color.green, color.blue);
    return text;
}

// The mean of a patch's middle, away from its edges, where chroma
// subsampling and scaling blur two patches together.
template <typename Sample>
Rgb PatchMean(int patch, const Sample& sample) {
    const int left = (patch % kColumns) * kPatchWidth;
    const int top = (patch / kColumns) * kPatchHeight;
    Rgb sum;
    int count = 0;
    for (int y = top + (kPatchHeight / 4); y < top + (3 * kPatchHeight / 4);
         ++y) {
        for (int x = left + (kPatchWidth / 4); x < left + (3 * kPatchWidth / 4);
             ++x) {
            const Rgb value = sample(x, y);
            sum.red += value.red;
            sum.green += value.green;
            sum.blue += value.blue;
            ++count;
        }
    }
    return {sum.red / count, sum.green / count, sum.blue / count};
}

// R1 -- docs/roadmap/REEL-2026-09.md. The mean of one cell of a coarse
// grid over the whole frame. A framing moves the picture, so the named
// patches no longer sit where they did; what must still hold, cell by
// cell, is that the monitor and the file show the same values.
template <typename Sample>
Rgb CellMean(int cell, int columns, int rows, const Sample& sample) {
    const int width = kWidth / columns;
    const int height = kHeight / rows;
    const int left = (cell % columns) * width;
    const int top = (cell / columns) * height;
    Rgb sum;
    int count = 0;
    for (int y = top + (height / 4); y < top + (3 * height / 4); ++y) {
        for (int x = left + (width / 4); x < left + (3 * width / 4); ++x) {
            const Rgb value = sample(x, y);
            sum.red += value.red;
            sum.green += value.green;
            sum.blue += value.blue;
            ++count;
        }
    }
    return {sum.red / count, sum.green / count, sum.blue / count};
}

// R1 -- where the two pictures line up. The luma profile along one axis,
// then the shift that makes the monitor's profile match the file's best.
// Values alone cannot answer the geometry question -- a cell mean swallows
// a shift of a few pixels -- and a pixel-by-pixel comparison cannot either,
// because the two resample with different kernels and disagree by a code
// value or two on every band edge. The alignment is what R1 must pin: the
// best shift has to be zero, on both axes, or the monitor and the export
// have placed the same rectangle in two places.
template <typename Sample>
std::vector<double> Profile(bool column, const Sample& sample) {
    const int count = column ? kWidth : kHeight;
    const int length = column ? kHeight : kWidth;
    std::vector<double> profile(static_cast<size_t>(count), 0.0);
    for (int index = 0; index < count; ++index) {
        double sum = 0.0;
        for (int along = 0; along < length; ++along) {
            const Rgb value =
                column ? sample(index, along) : sample(along, index);
            sum += value.red + value.green + value.blue;
        }
        profile[static_cast<size_t>(index)] = sum / length;
    }
    return profile;
}

// The shift, in pixels, that lines `left` up on `right`, searched over
// [-limit, limit], and by how much the second-best loses. The margin is
// reported, not asserted: on a picture of three bands, one pixel of shift
// only changes the columns at the two band edges, so the margin is small in
// absolute terms and its size says nothing useful. What says something is
// which shift wins.
int BestShift(const std::vector<double>& left, const std::vector<double>& right,
              int limit, double& margin) {
    int best = 0;
    double bestScore = 0.0;
    double runnerUp = 0.0;
    bool first = true;
    for (int shift = -limit; shift <= limit; ++shift) {
        double sum = 0.0;
        int count = 0;
        for (size_t index = static_cast<size_t>(limit);
             index + static_cast<size_t>(limit) < left.size(); ++index) {
            sum += std::abs(
                left[index] -
                right[static_cast<size_t>(static_cast<int>(index) + shift)]);
            ++count;
        }
        const double score = sum / count;
        if (first || score < bestScore) {
            if (!first) runnerUp = bestScore;
            bestScore = score;
            best = shift;
            first = false;
        } else if (runnerUp == 0.0 || score < runnerUp) {
            runnerUp = score;
        }
    }
    margin = runnerUp - bestScore;
    return best;
}

// One frame of the exported file as full-range float RGB, the way a player
// reads its BT.709 tags.
bool ReadExportedFrame(const std::filesystem::path& path, int64_t frame,
                       std::vector<float>& planes) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-parity.raw");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(path.string()) + " -map 0:v:0 -vf 'select=eq(n\\," +
        std::to_string(frame) +
        "),scale=in_color_matrix=bt709:out_range=full,format=gbrpf32le' "
        "-frames:v 1 -f rawvideo -pix_fmt gbrpf32le -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    const size_t expected =
        static_cast<size_t>(kWidth) * kHeight * 3 * sizeof(float);
    if (bytes.size() != expected) return false;
    planes.resize(expected / sizeof(float));
    std::memcpy(planes.data(), bytes.data(), bytes.size());
    return true;
}

// Renderer.mm copies what it needs before returning, as main.mm relies on.
struct MonitorFrames {
    std::vector<AVFrame*> frames;
    std::vector<MotionPreviewImage> motion;

    MonitorFrames() = default;
    MonitorFrames(const MonitorFrames&) = delete;
    MonitorFrames& operator=(const MonitorFrames&) = delete;
    ~MonitorFrames() {
        for (AVFrame*& frame : frames) av_frame_free(&frame);
    }
};

// The Record monitor's picture of `document` at kFrame.
bool RenderMonitor(const Document& document, Renderer& renderer,
                   const std::filesystem::path& directory,
                   std::vector<float>& rgba, std::string& error) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "video" && track.visible) tracks.push_back(&track);
    std::sort(tracks.begin(), tracks.end(),
              [](const DocumentTrack* left, const DocumentTrack* right) {
                  return left->index < right->index;
              });
    const Timeline timeline(document);
    const RationalTime position{kFrame, 25};

    FrameCache cache(size_t{256} << 20);
    PerformanceMetrics metrics;
    MonitorFrames monitor;
    monitor.motion.reserve(tracks.size());
    TimelineRenderData data;
    data.color_management = document.color_management;
    data.display_sdr_preview = true;
    data.sequence_width = document.sequence.width;
    data.sequence_height = document.sequence.height;
    data.video_height = kHeight;
    for (const DocumentTrack* track : tracks) {
        for (const ResolvedLayer& layer :
             timeline.ResolveTrackLayers(track->id, position)) {
            AVFrame* frame = nullptr;
            uint8_t displayReferred = 0;
            MotionPreviewRequest motion;
            if (MotionPreviewFor(document, layer.frame, motion)) {
                MotionPreviewImage image;
                MotionError motionError;
                if (!RenderMotionPreview(motion, document, directory.string(),
                                         image, motionError)) {
                    error = "motion preview: " + motionError.code + ": " +
                            motionError.message;
                    return false;
                }
                monitor.motion.push_back(std::move(image));
                const MotionPreviewImage& stored = monitor.motion.back();
                frame = av_frame_alloc();
                frame->format = AV_PIX_FMT_GBRAP;
                frame->width = stored.width;
                frame->height = stored.height;
                frame->color_range = AVCOL_RANGE_JPEG;
                auto* planes = const_cast<uint8_t*>(stored.planes.data());
                for (int plane = 0; plane < 4; ++plane) {
                    frame->data[plane] = planes + (stored.PlaneSize() * plane);
                    frame->linesize[plane] = stored.width;
                }
                displayReferred = 1;
            } else {
                const DocumentSource* source =
                    document.FindSource(layer.frame.source_id);
                if (source == nullptr) {
                    error = "layer without a source";
                    return false;
                }
                DecodeWorker worker(source->id, cache, metrics);
                if (!worker.Open(source->path, 1)) {
                    error = "unable to open " + source->path;
                    return false;
                }
                worker.Start();
                worker.RequestFrame(layer.frame.source_frame);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while ((frame = cache.GetExact(
                            source->id, layer.frame.source_frame)) == nullptr &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                worker.Stop();
                if (frame == nullptr) {
                    error = "the decoder never delivered frame " +
                            std::to_string(layer.frame.source_frame);
                    return false;
                }
                // DEC31 -- the one test main.mm makes for Record: a media
                // declared a graphic is placed as a motion frame is.
                if (document.IsGraphicsMedia(source->id)) displayReferred = 1;
            }
            monitor.frames.push_back(frame);
            data.video_display_referred.push_back(displayReferred);
            data.video_rotation_degrees.push_back(0);
            // G3 -- the same one lookup main.mm makes for Record.
            const LibraryMedia* media =
                document.FindLibraryMedia(layer.frame.source_id);
            data.video_color_inputs.push_back(
                media != nullptr ? media->color_input : MediaColorInput{});
            const DocumentClip* clip = document.FindClip(layer.frame.clip_id);
            // COLOR-T -- the same one call main.mm makes for Record: the
            // clip's context gives color.temperature its grading space and
            // its media's as-shot balance. Resolved without it, as this
            // test did, a temperature was drawn in the display space about
            // 6500 K, a picture Record never shows.
            data.video_color_grades.push_back(
                clip != nullptr
                    ? ResolveColorGrade(clip->effects,
                                        ColorGradeContextFor(document, *clip))
                    : ResolvedColorGrade{});
            // R2 -- the values THIS frame shows, sampled the way main.mm
            // samples them for Record: the framing replaces the clip's
            // constant one. R2c -- the opacity is the layer's as Timeline
            // resolves it, the key already in place of the constant; main.mm
            // no longer multiplies it by the sampled value.
            ClipSampledParameters sampled;
            if (clip != nullptr) {
                sampled.opacity = clip->opacity;
                sampled.framing = clip->framing;
                if (ClipHasAnimation(*clip)) {
                    std::string animationError;
                    const RationalTime offset = position.sub(clip->timeline_in);
                    if (!SampleClipParameters(*clip, offset.to_frames(25, 1),
                                              {25, 1}, sampled,
                                              animationError)) {
                        error = "animation: " + animationError;
                        return false;
                    }
                }
            }
            data.video_opacities.push_back(layer.opacity);
            // R1 -- the same one call main.mm makes for Record: the monitor
            // is handed the rectangle, it does not resolve one.
            MonitorFraming framing;
            if (clip != nullptr &&
                (ClipHasFraming(*clip) || ClipHasAnimation(*clip))) {
                ClipFramingGeometry geometry;
                int64_t displayedWidth = 0;
                int64_t displayedHeight = 0;
                std::string framingError;
                if (!ClipDisplayedSize(document, *clip, displayedWidth,
                                       displayedHeight, framingError) ||
                    !ResolveClipFraming(
                        sampled.framing, displayedWidth, displayedHeight,
                        document.sequence.width, document.sequence.height,
                        geometry, framingError)) {
                    error = "framing: " + framingError;
                    return false;
                }
                framing = ClipHasAnimation(*clip)
                              ? MonitorFramingOfWholePicture(
                                    geometry, displayedWidth, displayedHeight)
                              : MonitorFramingOf(geometry, displayedWidth,
                                                 displayedHeight);
            }
            data.video_framings.push_back(framing);
            // R5 -- docs/roadmap/REEL-2026-09.md, guard rail 1. The same
            // one call main.mm makes for Record: the monitor is handed the
            // colour and the strength, it computes no curve.
            MonitorVeil veil;
            if (const std::optional<ResolvedColorVeil> dip =
                    timeline.ResolveTrackColorVeil(track->id, position)) {
                veil = {static_cast<float>(dip->color.red) / 255.0f,
                        static_cast<float>(dip->color.green) / 255.0f,
                        static_cast<float>(dip->color.blue) / 255.0f,
                        TransitionAlphaFloat(dip->alpha)};
            }
            data.video_veils.push_back(veil);
            // EF3 -- the same one value main.mm hands Record: the clip's
            // blur, in sequence pixels.
            data.video_blurs.push_back(
                clip != nullptr && clip->blur.den > 0
                    ? static_cast<float>(clip->blur.num) /
                          static_cast<float>(clip->blur.den)
                    : 0.0f);
            // PA8 -- and its mask, as main.mm hands it.
            data.video_masks.push_back(
                clip != nullptr ? MonitorMaskOf(clip->mask) : MonitorMask{});
        }
    }
    // R6 -- docs/roadmap/REEL-2026-09.md. The same one call main.mm makes
    // for Record: the monitor is handed the samples, read from the package
    // and checked against the digest. The export is handed the same file.
    LoadedLookLut look;
    if (!LoadSequenceLookLut(document.sequence, document.color_management,
                             directory.string(), look, error)) {
        error = "look LUT: " + error;
        return false;
    }
    data.look_lut = look.samples;
    data.look_lut_digest = look.digest;
    data.look_lut_stage = look.stage;
    if (!renderer.RenderFramesToPixels(monitor.frames, data, kWidth, kHeight,
                                       rgba)) {
        error = "the monitor did not render";
        return false;
    }
    return true;
}

ClipEffect Effect(const std::string& type,
                  std::map<std::string, EffectParamValue> params) {
    ClipEffect effect;
    effect.type = type;
    effect.params = std::move(params);
    return effect;
}

// The QA's grade: exposure, the three wheels and a master S curve.
std::vector<ClipEffect> Grade() {
    return {
        Effect("color.exposure", {{"amount", {1, 3}}}),
        Effect("color.wheels", {{"lift_r", {1, 10}},
                                {"gamma_master", {1, 2}},
                                {"gain_master", {1, 2}}}),
        Effect("color.curves", {{"master_x0", {0, 1}},
                                {"master_y0", {0, 1}},
                                {"master_x1", {1, 4}},
                                {"master_y1", {1, 5}},
                                {"master_x2", {1, 2}},
                                {"master_y2", {1, 2}},
                                {"master_x3", {3, 4}},
                                {"master_y3", {4, 5}},
                                {"master_x4", {1, 1}},
                                {"master_y4", {1, 1}}}),
    };
}

// COLOR-T -- what propose_grade wrote on the first shots of SERIE_Hugo
// (PRJA): a temperature far above the rush's 4000 K, a tint toward green,
// and the exposure it gave the next shots of the same take.
std::vector<ClipEffect> ProposedGrade() {
    return {
        Effect("color.temperature", {{"kelvin", {11461, 1}}}),
        Effect("color.tint", {{"amount", {-36, 125}}}),
        Effect("color.exposure", {{"amount", {7, 25}}}),
    };
}

struct Case {
    const char* label;
    ColorManagementSettings color;
    bool graded = false;
    bool motion = false;
    // R1 -- the clip carries a framing: a crop of the patch board blown up
    // to fill the frame. What is compared then is a grid over the whole
    // picture, since the named patches have moved.
    bool framed = false;
    // R5 -- docs/roadmap/REEL-2026-09.md, guard rail 1. A colour dip
    // covering the compared frame at a strength neither 0 nor 1, which is
    // where a monitor and a file that draw two different curves part.
    bool dipped = false;
    // R6 -- docs/roadmap/REEL-2026-09.md, guard rail 1. The sequence's look
    // LUT, imported into the package the export reads and the monitor
    // reads. The one test that would catch the two reading one .cube
    // differently, or laying it at two different stages of the managed
    // graph.
    bool look = false;
    // R2 -- the framed clip zooms and pans over the compared frame: the one
    // test that would catch the managed composite placing a moving picture
    // anywhere but where the monitor draws it.
    bool zoom = false;
    // Where the zoom ends, 24 frames on. 3/2 puts the picture's top edge on
    // an odd row at the compared frame. The unmanaged composite used to
    // round it down to the even row, a pixel off the monitor (measured
    // here): its `overlay` composed in 4:2:0. DEC28 -- docs/roadmap/
    // REEL-2026-09.md: it composes in 4:4:4 10 bits, as the managed one
    // does, and is held on the odd row too; 7/5 keeps the even one.
    EffectParamValue zoom_to{3, 2};
    // DEC28 -- the clip is fitted at half its size, still, a pixel right
    // of centre: its picture starts on row 45 and column 81 of the 320 x
    // 180 frame, an odd row and an odd column the unmanaged `overlay`
    // rounded to 44 and 80 when it composed in 4:2:0.
    bool shrunk = false;
    // R2 -- the clip's opacity falls from 1 to 0 over the compared frame.
    bool fading = false;
    // R2c -- the clip's constant opacity. Under keys of its framing it
    // holds; under keys of its opacity the key replaces it. Record used to
    // multiply the two (0.25 for a zoom keyed on a clip at 1/2).
    EffectParamValue opacity{1, 1};
    // R5 -- the dip goes to black rather than white.
    bool black_dip = false;
    // R6 -- the look is declared in the display space ("rec709") rather
    // than on the working log values.
    std::string look_space = "acescct";
    // G3 -- the shot's media declares its own colour input, read in place
    // of the project's by both sides.
    MediaColorInput media_input;
    // COLOR-T -- the shot's media is an FX30 rush as ingest describes one:
    // probed from the untagged file WriteCameraSource writes, then given
    // what the camera's rtmd track holds (CameraMetadata.h, read on real
    // packets by camera_metadata_tests) -- the as-shot white balance, 0 for
    // none, and S-Log3 S-Gamut3.Cine declared as its input, range and
    // matrix "auto".
    bool camera_media = false;
    int32_t camera_balance = 0;
    // COLOR-T -- the shot carries ProposedGrade().
    bool proposed = false;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. The shot's Gaussian blur, in
    // sequence pixels (the sequence is the patch board's 320 x 180, so one
    // pixel of the monitor and of the file). Compared pixel by pixel over
    // the whole frame rather than patch by patch: the blur lives on the
    // patches' edges, which the patch means leave out.
    EffectParamValue blur{0, 1};
    // The largest pixel gap the case allows (see kBlurMeanTolerance).
    double blur_largest_tolerance = 0.05;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. The shot's mask. The board
    // stands alone over black, so what the mask cuts out is black on both
    // sides, and its feathered edge is the board mixed with that black:
    // compared pixel by pixel over the whole frame, as a blur is.
    ClipMask mask;
    // The most the mask may add to the gap between Record and the file at
    // one pixel (LargestMaskExcess), and the mean gap over the frame.
    double mask_largest_tolerance = 0.04;
    double mask_mean_tolerance = 0.006;
};

Case Masked(const char* label, ColorManagementSettings color, ClipMask mask) {
    Case test{label, color};
    test.mask = std::move(mask);
    return test;
}

// PA8 -- an ellipse on the middle of the board, feathered half-way in, and
// a rectangle off its centre, feathered a quarter.
ClipMask SoftEllipse() {
    ClipMask mask;
    mask.shape = kClipMaskEllipse;
    mask.width = {1, 2};
    mask.height = {3, 4};
    mask.feather = {1, 2};
    return mask;
}

ClipMask OffCentreWindow() {
    ClipMask mask;
    mask.shape = kClipMaskRectangle;
    mask.center_x = {2, 5};
    mask.center_y = {3, 5};
    mask.width = {1, 2};
    mask.height = {1, 2};
    mask.feather = {1, 4};
    return mask;
}

Case Blurred(const char* label, ColorManagementSettings color,
             EffectParamValue blur) {
    Case test{label, color};
    test.blur = blur;
    return test;
}

Case CameraShot(const char* label, ColorManagementSettings color,
                int32_t balance, bool proposed) {
    Case test{label, color};
    test.camera_media = true;
    test.camera_balance = balance;
    test.proposed = proposed;
    return test;
}

Case Framed(const char* label, ColorManagementSettings color) {
    Case test{label, color};
    test.framed = true;
    return test;
}

Case Zoomed(const char* label, ColorManagementSettings color) {
    Case test = Framed(label, color);
    test.zoom = true;
    return test;
}

Case Fading(const char* label, ColorManagementSettings color) {
    Case test{label, color};
    test.fading = true;
    return test;
}

Case Dipped(const char* label, ColorManagementSettings color, bool black) {
    Case test{label, color};
    test.dipped = true;
    test.black_dip = black;
    return test;
}

Case Looked(const char* label, ColorManagementSettings color,
            const std::string& space) {
    Case test{label, color};
    test.look = true;
    test.look_space = space;
    return test;
}

ClipKeyframe Key(const char* id, int64_t frame, EffectParamValue value) {
    ClipKeyframe key;
    key.id = id;
    key.time = {frame, 25};
    key.value = value;
    return key;
}

ColorManagementSettings Unmanaged() { return {}; }

ColorManagementSettings Slog3() {
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    return settings;
}

ColorManagementSettings ManagedRec709() {
    ColorManagementSettings settings;
    settings.enabled = true;
    return settings;
}

// COLOR-T -- the series' colour management when it was graded: S-Log3
// S-Gamut3.Cine read with the BT.709 matrix at full range, ACEScct, an SDR
// delivery.
ColorManagementSettings GradedSeries() {
    ColorManagementSettings settings = Slog3();
    settings.input_ycbcr_matrix = "bt709";
    settings.input_range = "full";
    return settings;
}

// Both pictures of the case, for a caller that compares two cases.
struct CasePictures {
    std::vector<float> monitor;
    std::vector<float> file;
};

void RunCase(const Case& test, const std::filesystem::path& directory,
             const std::filesystem::path& sourcePath, Renderer& renderer,
             CasePictures* pictures = nullptr) {
    const std::string label = test.label;
    Document document;
    document.sequence.id = GenerateUlid();
    document.sequence.name = "Parité";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.color_management = test.color;
    document.sources = {
        {GenerateUlid(), sourcePath.string(), {25, 1}, {50, 25}}};
    DocumentClip shot;
    shot.source_id = document.sources[0].id;
    shot.source_in = {0, 25};
    shot.duration = {25, 25};
    shot.timeline_in = {0, 25};
    shot.include_audio = false;
    if (test.graded) shot.effects = Grade();
    if (test.proposed) shot.effects = ProposedGrade();
    if (test.camera_media) {
        // COLOR-T -- probed from the file, as ingest probes a rush: its
        // size, pixel format and tags are the file's. The two fields after
        // the probe are what ingest reads in the camera's rtmd track.
        LibraryMedia media;
        std::string probeError;
        Check(ProbeMediaMetadata(sourcePath.string(), media, probeError),
              label + ": the camera rush probes: " + probeError);
        media.id = document.sources[0].id;
        media.path = sourcePath.string();
        media.filename = sourcePath.filename().string();
        Check(media.color_range == "pc" &&
                  (media.color_space.empty() || media.color_space == "unknown"),
              label +
                  ": the camera rush is full range and names no matrix, "
                  "as an FX30's MP4 (" +
                  media.color_range + ", " + media.color_space + ")");
        media.white_balance_kelvin = test.camera_balance;
        media.color_input = {"sony_sgamut3_cine", "sony_slog3", "auto", "auto"};
        document.library.push_back(media);
    } else if (test.framed || test.fading || test.blur.num != 0 ||
               !test.mask.shape.empty() ||
               DeclaresColorInput(test.media_input) ||
               test.media_input.graphics) {
        // R1, R2 -- a framed or animated clip is sized from its library
        // media, which a real project always has. G3 -- and a declared
        // input is the media's.
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = sourcePath.string();
        media.filename = sourcePath.filename().string();
        media.codec = "prores";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = kWidth;
        media.height = kHeight;
        media.rate = {25, 1};
        media.duration = {50, 25};
        media.color_input = test.media_input;
        document.library.push_back(media);
    }
    if (test.framed) {
        // Half of the first patch, the whole second, half of the third,
        // and the middle half of the board's height: a picture with known
        // boundaries inside it, blown up to fill the frame. A geometry the
        // monitor and the export disagreed about would move those
        // boundaries between the two.
        shot.framing.mode = FramingMode::Fill;
        shot.framing.crop_left = {1, 8};
        shot.framing.crop_right = {3, 8};
        shot.framing.crop_top = {1, 4};
        shot.framing.crop_bottom = {1, 4};
    }
    if (test.shrunk) {
        shot.framing = ClipFraming{};
        shot.framing.mode = FramingMode::Fit;
        shot.framing.scale = {1, 2};
        shot.framing.position_x = {1, 320};
    }
    if (test.zoom) {
        // From the framing above to `zoom_to` and a tenth of the frame to
        // the right, linear over 24 frames: at the compared frame the
        // picture is halfway there and a twentieth off centre, a place
        // neither end of the move occupies.
        ClipParameterAnimation scale;
        scale.parameter = ClipAnimatedParameter::FramingScale;
        scale.keyframes = {Key("01K9P0000000000000000000S0", 0, {1, 1}),
                           Key("01K9P0000000000000000000S1", 24, test.zoom_to)};
        ClipParameterAnimation right;
        right.parameter = ClipAnimatedParameter::FramingPositionX;
        right.keyframes = {Key("01K9P0000000000000000000X0", 0, {0, 1}),
                           Key("01K9P0000000000000000000X1", 24, {1, 10})};
        shot.animation = {scale, right};
    }
    if (test.fading) {
        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        fade.keyframes = {Key("01K9P0000000000000000000F0", 0, {1, 1}),
                          Key("01K9P0000000000000000000F1", 24, {0, 1})};
        shot.animation = {fade};
    }
    shot.blur = test.blur;
    shot.mask = test.mask;
    shot.opacity = test.opacity;
    document.sequence.tracks = {{GenerateUlid(), "video", 0, {shot}}};
    if (test.dipped) {
        // 24 frames of white from the shot's first frame: at frame 12 the
        // veil is exactly half, the one strength that tells a blend apart
        // from either end of it.
        DocumentTransition dip;
        dip.id = GenerateUlid();
        dip.track_id = document.sequence.tracks[0].id;
        dip.right_clip_id = document.sequence.tracks[0].clips[0].id;
        dip.type = "dip_to_color";
        dip.color = test.black_dip ? "#000000" : "#ffffff";
        dip.duration = {24, 25};
        dip.alignment = TransitionAlignment::StartAtCut;
        document.sequence.transitions = {dip};
    }
    if (test.motion) {
        mcp_json::Value definition;
        std::string parseError;
        Check(mcp_json::Value::Parse(kDefinition, definition, parseError),
              label + ": the composition parses: " + parseError);
        document.compositions = {
            {"01K90000000000000000000010", "Bandes", definition}};
        DocumentClip title;
        title.composition_id = "01K90000000000000000000010";
        title.source_in = {0, 25};
        title.duration = {25, 25};
        title.timeline_in = {0, 25};
        title.include_audio = false;
        title.opacity = {1, 2};
        document.sequence.tracks.push_back(
            {GenerateUlid(), "video", 1, {title}});
    }
    std::string error;
    if (test.look) {
        // Imported the way the tool imports it: written where an author
        // keeps it, then copied into the package under its digest. What the
        // export and the monitor read afterwards is that copy.
        const auto source = directory / (GenerateUlid() + "-look.cube");
        std::ofstream cube(source, std::ios::binary | std::ios::trunc);
        cube << LookCubeText();
        cube.close();
        Check(static_cast<bool>(cube), label + ": the look cube is written");
        SequenceLut lut;
        const bool imported = ImportLookLutIntoPackage(
            (directory / "parity.json").string(), source.string(),
            test.color.enabled ? test.look_space : "", test.color, lut, error);
        Check(imported, label + ": the look is imported: " + error);
        document.sequence.look_lut = lut;
    }
    Check(document.Validate(error),
          label + ": the timeline is valid: " + error);
    if (test.proposed) {
        // COLOR-T -- what both sides are handed: the media's as-shot
        // balance as the temperature's neutral, and every stage of the
        // stack resolved, so that parity cannot hold on a stage that
        // vanished from both.
        const DocumentClip& placed = document.sequence.tracks[0].clips[0];
        const ColorGradeContext context =
            ColorGradeContextFor(document, placed);
        Check(ColorGradeNeutralKelvin(context) == (test.camera_balance > 0
                                                       ? test.camera_balance
                                                       : kNeutralKelvin),
              label + ": the temperature's neutral is the media's balance");
        Check(ResolveColorGrade(placed.effects, context).count == 3,
              label + ": the temperature, the tint and the exposure resolve");
    }

    const auto output = directory / (GenerateUlid() + "-export.mp4");
    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(output.string());
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    const bool exported =
        Exporter::BuildPlan(document, (directory / "parity.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(exported, label + ": the timeline exports: " + error);
    if (const char* dump = std::getenv("CUTMACHINE_PARITY_GRAPH")) {
        // Debugging aid: the plan's cubes and command, to replay by hand.
        const std::filesystem::path where(dump);
        std::filesystem::create_directories(where);
        for (const ExportLutAsset& asset : plan.clip_grade_luts)
            std::ofstream(where / std::filesystem::path(asset.path).filename())
                << asset.cube;
        std::ofstream(where /
                      std::filesystem::path(plan.temporary_lut_path).filename())
            << plan.color_lut;
        if (!plan.display_shaper.path.empty())
            std::ofstream(
                where /
                std::filesystem::path(plan.display_shaper.path).filename())
                << plan.display_shaper.cube;
        std::filesystem::copy_file(
            sourcePath, where / "patches.mov",
            std::filesystem::copy_options::overwrite_existing);
        std::ofstream command(where / (GenerateUlid() + ".txt"));
        command << label << '\n' << plan.filter_graph << '\n';
        for (const std::string& argument : plan.arguments)
            command << argument << '\n';
    }
    std::vector<float> file;
    if (!exported || !ReadExportedFrame(output, kFrame, file)) {
        Check(false, label + ": the exported frame is read back");
        return;
    }
    std::vector<float> monitor;
    if (!RenderMonitor(document, renderer, directory, monitor, error)) {
        Check(false, label + ": " + error);
        return;
    }

    const size_t plane = static_cast<size_t>(kWidth) * kHeight;
    double worst = 0.0;
    std::string report;
    const auto monitorSample = [&](int x, int y) {
        const size_t pixel = (static_cast<size_t>(y) * kWidth) + x;
        return Rgb{monitor[pixel * 4], monitor[(pixel * 4) + 1],
                   monitor[(pixel * 4) + 2]};
    };
    const auto fileSample = [&](int x, int y) {
        const size_t pixel = (static_cast<size_t>(y) * kWidth) + x;
        return Rgb{file[(2 * plane) + pixel], file[pixel], file[plane + pixel]};
    };
    if (test.blur.num != 0) {
        // EF3 -- every pixel and every channel: the largest gap and the
        // mean one. The two blurs are two algorithms -- Metal Performance
        // Shaders' Gaussian on the monitor, FFmpeg's recursive `gblur` in
        // the file (ClipBlur.h) -- so they are not expected to agree to the
        // code, only within the bound below, measured.
        double largest = 0.0;
        double sum = 0.0;
        int largestX = 0;
        int largestY = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const double distance =
                    Distance(monitorSample(x, y), fileSample(x, y));
                sum += distance;
                if (distance > largest) {
                    largest = distance;
                    largestX = x;
                    largestY = y;
                }
            }
        }
        const double mean = sum / (static_cast<double>(kWidth) * kHeight);
        std::cout << label << ": blurred, largest pixel gap " << largest
                  << " at " << largestX << ',' << largestY << " (monitor "
                  << Describe(monitorSample(largestX, largestY)) << " file "
                  << Describe(fileSample(largestX, largestY))
                  << "), mean pixel gap " << mean << '\n';
        Check(largest <= test.blur_largest_tolerance &&
                  mean <= kBlurMeanTolerance,
              label +
                  ": the monitor blurs as the file does, largest pixel "
                  "gap " +
                  std::to_string(largest) + ", mean " + std::to_string(mean));
    }
    if (!test.mask.shape.empty()) {
        // PA8 -- every pixel and every channel. The rule is one (ClipMask.h
        // for the file, clipMaskCoverage for the monitor), evaluated at
        // each pixel's middle on both sides; what stands between them is
        // the file's own road -- its 4:2:0 chroma at the mask's edge, its
        // codec -- which the bounds below measure.
        double largest = 0.0;
        double sum = 0.0;
        int largestX = 0;
        int largestY = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const double distance =
                    Distance(monitorSample(x, y), fileSample(x, y));
                sum += distance;
                if (distance > largest) {
                    largest = distance;
                    largestX = x;
                    largestY = y;
                }
            }
        }
        const double mean = sum / (static_cast<double>(kWidth) * kHeight);
        std::cout << label << ": masked, largest pixel gap " << largest
                  << " at " << largestX << ',' << largestY << " (monitor "
                  << Describe(monitorSample(largestX, largestY)) << " file "
                  << Describe(fileSample(largestX, largestY))
                  << "), mean pixel gap " << mean << '\n';
        // The largest gap sits on the board's own edges, where the file's
        // chroma and scaling mix two patches whatever the mask does; it is
        // held against the same board unmasked, by the caller
        // (LargestMaskExcess). Here, the mean.
        Check(mean <= test.mask_mean_tolerance,
              label + ": the monitor masks as the file does, mean pixel gap " +
                  std::to_string(mean) + " (largest " +
                  std::to_string(largest) + ")");
    }
    if (test.framed) {
        for (int cell = 0; cell < 32; ++cell) {
            const Rgb shown = CellMean(cell, 8, 4, monitorSample);
            const Rgb delivered = CellMean(cell, 8, 4, fileSample);
            const double distance = Distance(shown, delivered);
            worst = std::max(worst, distance);
            report += "\n  cell " + std::to_string(cell) + " monitor " +
                      Describe(shown) + " file " + Describe(delivered);
        }
        for (int pass = 0; pass < 2; ++pass) {
            const bool column = pass == 0;
            double margin = 0.0;
            const int shift = BestShift(Profile(column, monitorSample),
                                        Profile(column, fileSample), 4, margin);
            Check(shift == 0, label +
                                  ": the monitor and the file place the framed "
                                  "picture at the same pixel (best " +
                                  std::string(column ? "column" : "row") +
                                  " shift " + std::to_string(shift) + ')');
            // A flat pair of profiles would make every shift equal and
            // leave the search on its first candidate, -4, so this check
            // cannot pass on a picture with nothing in it.
            std::cout << label << ": best " << (column ? "column" : "row")
                      << " shift " << shift << ", margin " << margin << '\n';
        }
    } else {
        for (int patch = 0; patch < kColumns * kRows; ++patch) {
            const Rgb shown = PatchMean(patch, monitorSample);
            const Rgb delivered = PatchMean(patch, fileSample);
            const double distance = Distance(shown, delivered);
            worst = std::max(worst, distance);
            report += std::string("\n  ") + kPatches[patch] + " monitor " +
                      Describe(shown) + " file " + Describe(delivered);
        }
    }
    std::cout << label << ": worst patch " << worst << report << '\n';
    Check(worst <= kTolerance,
          label + ": the monitor shows what the file delivers, worst patch " +
              std::to_string(worst) + report);
    if (pictures != nullptr) {
        pictures->monitor = std::move(monitor);
        pictures->file = std::move(file);
    }
}

// The largest difference between two pictures of one size, or infinity.
double LargestDifference(const std::vector<float>& left,
                         const std::vector<float>& right) {
    if (left.empty() || left.size() != right.size())
        return std::numeric_limits<double>::infinity();
    double largest = 0.0;
    for (size_t index = 0; index < left.size(); ++index)
        largest = std::max(
            largest, static_cast<double>(std::abs(left[index] - right[index])));
    return largest;
}

// PA8 -- how much further the monitor stands from the file at a pixel of a
// masked board than at the same pixel of the board whole: the largest such
// excess, over every pixel and channel. The board's own edges -- where the
// file's 4:2:0 chroma mixes two patches -- are as far apart with the mask
// as without, and cancel; what is left is the mask's.
double LargestMaskExcess(const CasePictures& whole,
                         const CasePictures& masked) {
    const size_t plane = static_cast<size_t>(kWidth) * kHeight;
    if (whole.monitor.size() != plane * 4 ||
        masked.monitor.size() != plane * 4 || whole.file.size() != plane * 3 ||
        masked.file.size() != plane * 3)
        return std::numeric_limits<double>::infinity();
    // Monitor: interleaved RGBA. File: planar G, B, R.
    const size_t filePlane[3] = {2 * plane, 0, plane};
    double largest = 0.0;
    for (size_t pixel = 0; pixel < plane; ++pixel) {
        double gapWhole = 0.0;
        double gapMasked = 0.0;
        for (size_t channel = 0; channel < 3; ++channel) {
            gapWhole = std::max(gapWhole,
                                static_cast<double>(std::abs(
                                    whole.monitor[(pixel * 4) + channel] -
                                    whole.file[filePlane[channel] + pixel])));
            gapMasked = std::max(gapMasked,
                                 static_cast<double>(std::abs(
                                     masked.monitor[(pixel * 4) + channel] -
                                     masked.file[filePlane[channel] + pixel])));
        }
        largest = std::max(largest, gapMasked - gapWhole);
    }
    return largest;
}

// One patch's mean in a picture RenderMonitor returns (interleaved RGBA)
// or in one ReadExportedFrame returns (planar G, B, R).
Rgb MonitorPatch(const std::vector<float>& rgba, int patch) {
    return PatchMean(patch, [&](int x, int y) {
        const size_t pixel = (static_cast<size_t>(y) * kWidth) + x;
        return Rgb{rgba[pixel * 4], rgba[(pixel * 4) + 1],
                   rgba[(pixel * 4) + 2]};
    });
}

Rgb FilePatch(const std::vector<float>& planes, int patch) {
    const size_t plane = static_cast<size_t>(kWidth) * kHeight;
    return PatchMean(patch, [&](int x, int y) {
        const size_t pixel = (static_cast<size_t>(y) * kWidth) + x;
        return Rgb{planes[(2 * plane) + pixel], planes[pixel],
                   planes[plane + pixel]};
    });
}

// REMOTION-PARITY-2026-09 REM1, scene C11 -- a composition at 50 % over a
// graded shot. Parity alone would pass if both sides forgot the grade, or
// graded the title along with the shot, so each side is also held to what
// the scene means: every patch shows half the title's own colour over half
// the graded shot. kDefinition's bands cover the two rows of patches
// exactly, white over the top one, the charter magenta over the bottom one.
void CheckTitleOverGradedShot(const std::string& mode,
                              ColorManagementSettings color,
                              const std::filesystem::path& directory,
                              const std::filesystem::path& source,
                              Renderer& renderer) {
    const std::string over = mode + ", motion à 50 % sur plan étalonné (C11)";
    const std::string shot = mode + ", plan étalonné (référence C11)";
    const std::string bare =
        mode + ", motion à 50 % sur plan brut (témoin C11)";
    CasePictures overPictures;
    CasePictures shotPictures;
    CasePictures barePictures;
    RunCase({over.c_str(), color, true, true}, directory, source, renderer,
            &overPictures);
    RunCase({shot.c_str(), color, true, false}, directory, source, renderer,
            &shotPictures);
    RunCase({bare.c_str(), color, false, true}, directory, source, renderer,
            &barePictures);
    const size_t expected = static_cast<size_t>(kWidth) * kHeight * 4;
    if (overPictures.monitor.size() != expected ||
        shotPictures.monitor.size() != expected ||
        barePictures.monitor.size() != expected ||
        overPictures.file.size() * 4 != expected * 3 ||
        shotPictures.file.size() * 4 != expected * 3) {
        Check(false, "C11 " + mode + ": the three cases are rendered");
        return;
    }
    const Rgb bands[kRows] = {{1.0, 1.0, 1.0},
                              {0xE6 / 255.0, 0x00 / 255.0, 0x7E / 255.0}};
    for (int side = 0; side < 2; ++side) {
        const bool monitor = side == 0;
        const auto patchOf = [&](const CasePictures& pictures, int patch) {
            return monitor ? MonitorPatch(pictures.monitor, patch)
                           : FilePatch(pictures.file, patch);
        };
        const std::string where = monitor ? "Record" : "the file";
        double worst = 0.0;
        double titleShows = 0.0;
        std::string report;
        for (int patch = 0; patch < kColumns * kRows; ++patch) {
            const Rgb band = bands[patch / kColumns];
            const Rgb graded = patchOf(shotPictures, patch);
            const Rgb shown = patchOf(overPictures, patch);
            const Rgb blend{(band.red + graded.red) / 2.0,
                            (band.green + graded.green) / 2.0,
                            (band.blue + graded.blue) / 2.0};
            worst = std::max(worst, Distance(shown, blend));
            titleShows = std::max(titleShows, Distance(shown, graded));
            report += std::string("\n  ") + kPatches[patch] + " shown " +
                      Describe(shown) + " expected " + Describe(blend);
        }
        std::cout << "C11 " << mode << ", " << where << ": worst blend "
                  << worst << report << '\n';
        Check(worst <= kTolerance,
              "C11 " + mode + ": in " + where +
                  ", each patch is half the title's own colour over half "
                  "the graded shot, worst " +
                  std::to_string(worst) + report);
        Check(titleShows > 0.1, "C11 " + mode + ": in " + where +
                                    ", the title changes the graded shot, "
                                    "largest patch gap " +
                                    std::to_string(titleShows));
    }
    // The negative witness: the same title over the ungraded shot. Were the
    // grade lost under the title, the two would be the same picture.
    const double gradeShows =
        LargestDifference(overPictures.monitor, barePictures.monitor);
    Check(gradeShows > 0.05,
          "C11 " + mode +
              ": in Record, the grade reaches the shot under the title, "
              "largest gap to the ungraded witness " +
              std::to_string(gradeShows));
}

int Run() {
    @autoreleasepool {
        if (MTLCreateSystemDefaultDevice() == nil) {
            std::cout << "SKIP: no Metal device\n";
            return 77;
        }
        const auto directory = std::filesystem::temp_directory_path() /
                               ("cutmachine-parity-" + GenerateUlid());
        std::filesystem::create_directories(directory);
        const auto source = directory / "patches.mov";
        Check(WritePatchSource(source), "the patch source is generated");
        const auto cameraSource = directory / "fx30.mkv";
        Check(WriteCameraSource(cameraSource),
              "the camera rush source is generated");
        // The export renders motion frames with the renderer the executable
        // installs; the monitor calls RenderMotionPreview directly.
        InstallMotionFrameRenderer();
        Renderer renderer;
        Check(renderer.Initialize(nil), "the renderer starts without a view");
        if (failures == 0) {
            const std::vector<Case> cases = {
                {"sans gestion, plan brut", Unmanaged(), false, false},
                {"sans gestion, plan étalonné", Unmanaged(), true, false},
                {"sans gestion, motion à 50 %", Unmanaged(), false, true},
                {"S-Log3 vers Rec.709, plan brut", Slog3(), false, false},
                {"S-Log3 vers Rec.709, plan étalonné", Slog3(), true, false},
                {"S-Log3 vers Rec.709, motion à 50 %", Slog3(), false, true},
                {"Rec.709 géré, plan étalonné", ManagedRec709(), true, false},
                // R1 -- docs/roadmap/REEL-2026-09.md, guard rail 1. The one
                // test that would catch the monitor and the export placing
                // the same rectangle differently.
                {"sans gestion, plan recadré", Unmanaged(), false, false, true},
                {"S-Log3 vers Rec.709, plan recadré", Slog3(), false, false,
                 true},
                // R5 -- docs/roadmap/REEL-2026-09.md, guard rail 1. The
                // one test that would catch the monitor and the export
                // drawing two different fade curves, or the same one one
                // frame apart. There is no managed case here because there
                // is no managed answer yet: run under S-Log3, this
                // measured the two 0.26 apart, because the managed graph
                // has no stage where a graphic colour can be laid and the
                // veil ended up in the camera's encoding. Export.cc now
                // refuses that export by name, and tests/export_tests.cc
                // pins the refusal.
                {"sans gestion, fondu au blanc à mi-course", Unmanaged(), false,
                 false, false, true},
                // R6 -- docs/roadmap/REEL-2026-09.md, guard rail 1. One
                // .cube, two programs reading it: Metal samples the
                // texture, FFmpeg runs lut3d on the same file. Managed,
                // the look sits between the ACEScct shaper and the display
                // cube, which is the stage the ticket names and the one
                // that cannot be checked by reading the code.
                {"sans gestion, look de séquence", Unmanaged(), false, false,
                 false, false, true},
                {"S-Log3 vers Rec.709, look de séquence", Slog3(), false, false,
                 false, false, true},
                {"S-Log3 vers Rec.709, plan étalonné et look de séquence",
                 Slog3(), true, false, false, false, true},
                // R2, R5, R6 under colour management -- the three refusals
                // the managed graph used to give, each now an export whose
                // frame the monitor must match. The unmanaged zoom is here
                // as the reference the managed one is held to.
                [] {
                    Case test = Zoomed("sans gestion, zoom animé", Unmanaged());
                    test.zoom_to = {7, 5};
                    return test;
                }(),
                // DEC28 -- docs/roadmap/REEL-2026-09.md. The unmanaged
                // composite in 4:4:4 10 bits places an edge on an odd row
                // where the monitor draws it, animated or still
                // (VISUAL_QA_CHECKLIST.md, box 161).
                Zoomed("sans gestion, zoom animé, bord sur une ligne impaire",
                       Unmanaged()),
                [] {
                    Case test =
                        Framed("sans gestion, plan réduit, bord sur une ligne "
                               "et une colonne impaires",
                               Unmanaged());
                    test.shrunk = true;
                    return test;
                }(),
                Zoomed("S-Log3 vers Rec.709, zoom animé", Slog3()),
                Fading("S-Log3 vers Rec.709, opacité animée", Slog3()),
                // R2c -- a translucent clip whose framing is keyed, and one
                // whose opacity is keyed over a constant that is not 1.
                [] {
                    Case test = Zoomed("sans gestion, plan à 1/2, zoom animé",
                                       Unmanaged());
                    test.opacity = {1, 2};
                    return test;
                }(),
                [] {
                    Case test = Fading(
                        "S-Log3 vers Rec.709, opacité animée sur un plan à 1/4",
                        Slog3());
                    test.opacity = {1, 4};
                    return test;
                }(),
                Dipped("S-Log3 vers Rec.709, fondu au blanc à mi-course",
                       Slog3(), false),
                Dipped("S-Log3 vers Rec.709, fondu au noir à mi-course",
                       Slog3(), true),
                Dipped("Rec.709 géré, fondu au blanc à mi-course",
                       ManagedRec709(), false),
                Looked("S-Log3 vers Rec.709, look Rec.709", Slog3(), "rec709"),
            };
            // EF3 -- the blur, unmanaged and managed, on the whole board,
            // on a framed one whose edges the frame cuts, and on a graphic
            // mixed in the graphics view. Parity alone would pass if both
            // sides forgot the blur, so each is also held to differ from
            // the sharp board (below).
            const std::vector<Case> blurred = {
                Blurred("sans gestion, flou gaussien 6 px", Unmanaged(),
                        {6, 1}),
                Blurred("sans gestion, flou gaussien 3/2 px", Unmanaged(),
                        {3, 2}),
                [] {
                    Case test =
                        Blurred("S-Log3 vers Rec.709, flou gaussien 6 px",
                                Slog3(), {6, 1});
                    // The steep display transform (kBlurMeanTolerance).
                    test.blur_largest_tolerance = 0.09;
                    return test;
                }(),
                [] {
                    Case test = Framed("sans gestion, plan recadré flou 4 px",
                                       Unmanaged());
                    test.blur = {4, 1};
                    return test;
                }(),
                [] {
                    Case test =
                        Blurred("S-Log3, graphisme flou 6 px", Slog3(), {6, 1});
                    test.media_input = {"", "", "auto", "auto", true};
                    return test;
                }(),
            };
            // A debugging aid: CUTMACHINE_PARITY_ONLY runs the cases whose
            // label holds it, and nothing else.
            const char* only = std::getenv("CUTMACHINE_PARITY_ONLY");
            const auto selected = [&](const Case& test) {
                return only == nullptr ||
                       std::string(test.label).find(only) != std::string::npos;
            };
            CasePictures sharpBoard;
            CasePictures blurredBoard;
            if (selected(blurred.front())) {
                RunCase({"sans gestion, plan brut (témoin du flou)",
                         Unmanaged(), false, false},
                        directory, source, renderer, &sharpBoard);
                RunCase(blurred.front(), directory, source, renderer,
                        &blurredBoard);
                const double monitorBlur =
                    LargestDifference(sharpBoard.monitor, blurredBoard.monitor);
                const double fileBlur =
                    LargestDifference(sharpBoard.file, blurredBoard.file);
                std::cout << "EF3: sharp against blurred board, monitor "
                          << monitorBlur << ", file " << fileBlur << '\n';
                Check(monitorBlur > 0.1 && fileBlur > 0.1,
                      "EF3: the blur changes the picture, in Record (" +
                          std::to_string(monitorBlur) + ") and in the file (" +
                          std::to_string(fileBlur) + ")");
            }
            for (size_t index = 1; index < blurred.size(); ++index)
                if (selected(blurred[index]))
                    RunCase(blurred[index], directory, source, renderer);
            // PA8 -- docs/roadmap/PARITE-2026-11.md. The mask, unmanaged
            // and managed, on a framed board whose fill cuts the picture
            // (the mask is placed on the source, not on what the frame
            // shows of it), and on a graphic mixed in the graphics view.
            // As for the blur, each side is also held to differ from the
            // whole board.
            const std::vector<Case> masked = {
                Masked("sans gestion, masque elliptique adouci", Unmanaged(),
                       SoftEllipse()),
                [] {
                    Case test =
                        Masked("S-Log3 vers Rec.709, masque elliptique adouci",
                               Slog3(), SoftEllipse());
                    // The steep display transform, as for the blur: on a
                    // feathered edge the file's 4:2:0 chroma spreads a
                    // colour that changes at every pixel, and the transform
                    // makes much of it. Measured 0.079 on 12 November 2026,
                    // against 0.017 to 0.026 for the three other cases.
                    test.mask_largest_tolerance = 0.09;
                    return test;
                }(),
                [] {
                    Case test = Framed("sans gestion, plan recadré masqué",
                                       Unmanaged());
                    test.mask = OffCentreWindow();
                    return test;
                }(),
                [] {
                    Case test = Masked("S-Log3, graphisme masqué", Slog3(),
                                       OffCentreWindow());
                    test.media_input = {"", "", "auto", "auto", true};
                    return test;
                }(),
            };
            for (size_t index = 0; index < masked.size(); ++index) {
                if (!selected(masked[index])) continue;
                // The same case without its mask, then with it.
                Case twin = masked[index];
                twin.mask = ClipMask{};
                const std::string twinLabel =
                    std::string(masked[index].label) + " (témoin sans masque)";
                twin.label = twinLabel.c_str();
                CasePictures wholeBoard;
                CasePictures maskedBoard;
                RunCase(twin, directory, source, renderer, &wholeBoard);
                RunCase(masked[index], directory, source, renderer,
                        &maskedBoard);
                const double monitorMask =
                    LargestDifference(wholeBoard.monitor, maskedBoard.monitor);
                const double fileMask =
                    LargestDifference(wholeBoard.file, maskedBoard.file);
                const double excess =
                    LargestMaskExcess(wholeBoard, maskedBoard);
                std::cout << masked[index].label
                          << ": whole against masked board, monitor "
                          << monitorMask << ", file " << fileMask
                          << "; largest gap the mask adds " << excess << '\n';
                Check(monitorMask > 0.1 && fileMask > 0.1,
                      std::string(masked[index].label) +
                          ": the mask changes the picture, in Record (" +
                          std::to_string(monitorMask) + ") and in the file (" +
                          std::to_string(fileMask) + ")");
                Check(excess <= masked[index].mask_largest_tolerance,
                      std::string(masked[index].label) +
                          ": the mask adds at most " + std::to_string(excess) +
                          " to the gap between Record and the file at any "
                          "pixel");
            }
            if (only != nullptr) {
                std::filesystem::remove_all(directory);
                return failures == 0 ? 0 : 1;
            }
            for (const Case& test : cases)
                RunCase(test, directory, source, renderer);

            // G3 -- docs/roadmap/GRADE-2026-09.md. A media that declares
            // its own input, under a project that declares another. Parity
            // alone would pass if both sides ignored the declaration, so
            // the picture is also held to the project whose own input is
            // the one declared: the same picture on the monitor and in the
            // file, and not the one the project's input gives.
            Case declared{"S-Log3, média déclaré Rec.709, plan étalonné",
                          Slog3(), true, false};
            declared.media_input = {"rec709", "rec709", "auto", "auto"};
            Case asProject{"Rec.709 géré, plan étalonné (référence G3)",
                           ManagedRec709(), true, false};
            Case asLog{"S-Log3, plan étalonné (référence G3)", Slog3(), true,
                       false};
            Case logDeclared{"Rec.709 géré, média déclaré S-Log3, plan brut",
                             ManagedRec709(), false, false};
            logDeclared.media_input = {"sony_sgamut3_cine", "sony_slog3",
                                       "auto", "auto"};
            CasePictures declaredPictures;
            CasePictures projectPictures;
            CasePictures logPictures;
            RunCase(declared, directory, source, renderer, &declaredPictures);
            RunCase(asProject, directory, source, renderer, &projectPictures);
            RunCase(asLog, directory, source, renderer, &logPictures);
            RunCase(logDeclared, directory, source, renderer);

            // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A media
            // declared a graphic, under an S-Log3 project: the real shader
            // decodes its YCbCr (the board is ProRes) and mixes it in the
            // graphics view, as the export does. And it is not the picture
            // the camera input gives. (No HLG case: Record shows an HLG
            // delivery through its SDR view, the file is HLG.)
            Case graphic{"S-Log3, média déclaré graphisme", Slog3(), false,
                         false};
            graphic.media_input = {"", "", "auto", "auto", true};
            Case graphicUnmanaged{"sans gestion, média déclaré graphisme",
                                  Unmanaged(), false, false};
            graphicUnmanaged.media_input = graphic.media_input;
            CasePictures graphicPictures;
            CasePictures cameraPictures;
            RunCase(graphic, directory, source, renderer, &graphicPictures);
            RunCase(graphicUnmanaged, directory, source, renderer);
            RunCase(
                {"S-Log3, plan brut (témoin graphisme)", Slog3(), false, false},
                directory, source, renderer, &cameraPictures);
            const double graphicGap = LargestDifference(graphicPictures.monitor,
                                                        cameraPictures.monitor);
            std::cout << "DEC31: graphic vs camera input, monitor "
                      << graphicGap << '\n';
            Check(graphicGap > 0.05,
                  "DEC31: the graphic is not read through the camera's "
                  "input, largest gap " +
                      std::to_string(graphicGap));
            const double monitorGap = LargestDifference(
                declaredPictures.monitor, projectPictures.monitor);
            const double fileGap =
                LargestDifference(declaredPictures.file, projectPictures.file);
            const double ignoredGap = LargestDifference(
                declaredPictures.monitor, logPictures.monitor);
            std::cout << "G3: declared vs project input, monitor " << monitorGap
                      << ", file " << fileGap << "; declared vs ignored "
                      << ignoredGap << '\n';
            Check(monitorGap <= 1e-6,
                  "G3: the monitor reads a declared Rec.709 media as a "
                  "Rec.709 project does, largest gap " +
                      std::to_string(monitorGap));
            Check(fileGap <= 1e-3,
                  "G3: the export reads a declared Rec.709 media as a "
                  "Rec.709 project does, largest gap " +
                      std::to_string(fileGap));
            Check(ignoredGap > 0.05,
                  "G3: the declaration changes the picture the project's "
                  "S-Log3 input gives, largest gap " +
                      std::to_string(ignoredGap));

            // COLOR-T -- an FX30 rush, untagged, whose media declares its
            // input as ingest writes it: bare, then under the stack
            // propose_grade wrote on SERIE_Hugo, at the as-shot balance
            // the camera recorded and with none known. Parity alone would
            // pass if both sides ignored the media's balance, so the two
            // balances are also held to two different pictures, on each
            // side.
            RunCase(CameraShot("S-Log3, rush FX30 sans balise, plan brut",
                               GradedSeries(), 4000, false),
                    directory, cameraSource, renderer);
            CasePictures balanced;
            CasePictures unbalanced;
            RunCase(CameraShot("S-Log3, rush FX30 à 4000 K, pile de "
                               "propose_grade",
                               GradedSeries(), 4000, true),
                    directory, cameraSource, renderer, &balanced);
            RunCase(CameraShot("S-Log3, rush FX30 sans balance connue, pile "
                               "de propose_grade",
                               GradedSeries(), 0, true),
                    directory, cameraSource, renderer, &unbalanced);
            const double monitorBalance =
                LargestDifference(balanced.monitor, unbalanced.monitor);
            const double fileBalance =
                LargestDifference(balanced.file, unbalanced.file);
            std::cout << "COLOR-T: 4000 K against an unknown balance, monitor "
                      << monitorBalance << ", file " << fileBalance << '\n';
            Check(monitorBalance > 0.05 && fileBalance > 0.05,
                  "COLOR-T: the media's as-shot balance changes what the "
                  "temperature does, in Record (" +
                      std::to_string(monitorBalance) + ") and in the file (" +
                      std::to_string(fileBalance) + ")");

            CheckTitleOverGradedShot("sans gestion", Unmanaged(), directory,
                                     source, renderer);
            CheckTitleOverGradedShot("S-Log3 vers Rec.709", Slog3(), directory,
                                     source, renderer);
            CheckTitleOverGradedShot("Rec.709 géré", ManagedRec709(), directory,
                                     source, renderer);
        }
        std::filesystem::remove_all(directory);
    }
    if (failures == 0) std::cout << "PASS: monitor and export agree\n";
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main() { return Run(); }
