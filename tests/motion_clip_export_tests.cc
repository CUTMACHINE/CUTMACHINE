// MOTION-2026-09 M4 -- ROADMAP.md. A motion clip above an interview shot,
// exported and read back. Every expected pixel is the frame the engine
// renders for that instant (MotionRender.h, the renderer the export uses),
// composited over the source by the rule of each graph: over the display
// signal without colour management, in scene-linear light with it, through
// the OpenColorIO path the export samples into its cubes. macOS only: Core
// Text and Core Graphics draw the frames, FFmpeg encodes and decodes.

#include "ClipAnimation.h"
#include "ColorEffects.h"
#include "ColorManagement.h"
#include "Document.h"
#include "Export.h"
#include "ExportVerification.h"
#include "Json.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "MotionCli.h"
#include "MotionClip.h"
#include "MotionClipPreview.h"
#include "MotionClipPreviewRender.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionFrameRenderer.h"
#include "MotionMedia.h"
#include "MotionRender.h"
#include "MotionTemplates.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

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
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

using mcp_json::Value;

Value Json(const std::string& text) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error)) {
        throw std::runtime_error("test JSON: " + error + "\n" + text);
    }
    return value;
}

std::string Quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    return quoted + "'";
}

constexpr int kWidth = 192;
constexpr int kHeight = 108;
const Ulid kComposition = "01K90000000000000000000010";

// A yellow box sliding down one pixel a frame, so that a frame shown at the
// wrong instant cannot pass, and a white title whose text is a prop, drawn
// with antialiased edges over whatever lies under the clip.
const char kDefinition[] =
    R"({"width":192,"height":108,"fps":25,"duration":60,)"
    R"("props":{"titre":{"type":"text","default":"Hi"}},)"
    R"("root":{"type":"fill","uid":"01K90000000000000000000011","children":[)"
    R"({"type":"box","uid":"01K90000000000000000000012","id":"boite",)"
    R"("style":{"position":"absolute","left":12,"top":8,"width":40,)"
    R"("height":24,"backgroundColor":"#FFD23F"},)"
    R"("animate":{"translateY":{"interpolate":{"frames":[0,59],)"
    R"("values":[0,59],"extrapolateRight":"extend",)"
    R"("uids":["01K90000000000000000000013","01K90000000000000000000014"]}}}},)"
    R"({"type":"text","uid":"01K90000000000000000000015","id":"titre",)"
    R"("content":{"prop":"titre"},)"
    R"("style":{"position":"absolute","left":92,"top":14,)"
    R"("fontFamily":"Fira Sans Condensed","fontWeight":600,)"
    R"("fontSize":64,"color":"#FFFFFF"}}]}})";

bool WriteFlatSource(const std::filesystem::path& path,
                     const std::string& color) {
    const std::string encode =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i 'color=c=" + color +
        ":s=" + std::to_string(kWidth) + 'x' + std::to_string(kHeight) +
        ":r=25:d=4' -c:v libx264 -crf 0 -pix_fmt yuv444p "
        "-colorspace bt709 -color_primaries bt709 -color_trc bt709 -y " +
        Quote(path.string());
    return std::system(encode.c_str()) == 0;
}

// Decoded frames as full-range float RGB. FFmpeg's planar order is green,
// blue, red.
struct FloatFrames {
    std::vector<float> samples;

    size_t Count() const {
        return samples.size() / (static_cast<size_t>(kWidth) * kHeight * 3);
    }

    RgbColor At(size_t frame, int x, int y) const {
        const size_t plane = static_cast<size_t>(kWidth) * kHeight;
        const size_t base =
            (frame * plane * 3) + (static_cast<size_t>(y) * kWidth) + x;
        return {samples[base + (2 * plane)], samples[base],
                samples[base + plane]};
    }
};

bool ReadFloatFrames(const std::string& path, FloatFrames& frames) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-motion-float.raw");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(path) +
        " -map 0:v:0 -vf scale=in_color_matrix=bt709:out_range=full,"
        "format=gbrpf32le -f rawvideo -pix_fmt gbrpf32le -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    const size_t frameBytes =
        static_cast<size_t>(kWidth) * kHeight * 3 * sizeof(float);
    if (bytes.empty() || bytes.size() % frameBytes != 0) {
        return false;
    }
    frames.samples.resize(bytes.size() / sizeof(float));
    std::memcpy(frames.samples.data(), bytes.data(), bytes.size());
    return true;
}

// REMOTION-PARITY-2026-09 REM6 -- the Y plane of a 10-bit limited-range
// file, as full-range values: what a 4:2:0 delivery keeps at full
// resolution, read as stored, before any conversion to RGB can clip a
// colour its chroma smeared.
bool ReadLumaFrames(const std::string& path, std::vector<float>& luma) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-motion-luma.raw");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(path) +
        " -map 0:v:0 -vf extractplanes=y -f rawvideo -pix_fmt gray10le -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    const size_t frameBytes =
        static_cast<size_t>(kWidth) * kHeight * sizeof(uint16_t);
    if (bytes.empty() || bytes.size() % frameBytes != 0) {
        return false;
    }
    std::vector<uint16_t> codes(bytes.size() / sizeof(uint16_t));
    std::memcpy(codes.data(), bytes.data(), bytes.size());
    luma.resize(codes.size());
    for (size_t index = 0; index < codes.size(); ++index) {
        luma[index] = (static_cast<float>(codes[index]) - 64.0f) / 876.0f;
    }
    return true;
}

double Distance(const RgbColor& left, const RgbColor& right) {
    return std::max({std::abs(left.red - right.red),
                     std::abs(left.green - right.green),
                     std::abs(left.blue - right.blue)});
}

std::string Describe(const RgbColor& color) {
    return '(' + std::to_string(color.red) + ' ' + std::to_string(color.green) +
           ' ' + std::to_string(color.blue) + ')';
}

RgbColor Mix(const RgbColor& from, const RgbColor& to, double amount) {
    return {from.red + ((to.red - from.red) * amount),
            from.green + ((to.green - from.green) * amount),
            from.blue + ((to.blue - from.blue) * amount)};
}

// The composition frame the renderer draws, premultiplied, 8 bits.
MotionRaster Reference(const Value& props, int64_t frame) {
    MotionComposition composition;
    MotionError error;
    MotionCoreTextMeasurer measurer;
    MotionFrame evaluated;
    MotionRaster raster;
    const bool ok = ParseMotionComposition(kDefinition, composition, error) &&
                    EvaluateMotionFrame(composition, props, frame, measurer,
                                        evaluated, error) &&
                    RenderMotionFrame(evaluated, measurer, raster, error);
    if (!ok) {
        throw std::runtime_error("reference frame: " + error.code + " " +
                                 error.message);
    }
    return raster;
}

struct Pixel {
    RgbColor color;  // straight
    double alpha = 0.0;
};

Pixel At(const MotionRaster& raster, int x, int y) {
    const uint8_t* rgba =
        &raster.pixels[((static_cast<size_t>(y) * raster.width) + x) * 4];
    Pixel pixel;
    pixel.alpha = rgba[3] / 255.0;
    if (rgba[3] != 0) {
        pixel.color = {rgba[0] / static_cast<double>(rgba[3]),
                       rgba[1] / static_cast<double>(rgba[3]),
                       rgba[2] / static_cast<double>(rgba[3])};
    }
    return pixel;
}

// What a frame of the export must show where the motion clip covers the
// grey source, by the rule of the graph. `opacity` scales the clip's alpha.
using Composite = RgbColor (*)(const ColorManagementSettings&,
                               const RgbColor& source, const Pixel& motion,
                               double opacity);

RgbColor DisplayComposite(const ColorManagementSettings&,
                          const RgbColor& source, const Pixel& motion,
                          double opacity) {
    return Mix(source, motion.color, motion.alpha * opacity);
}

// Visual QA of 19 September 2026 -- under colour management a motion clip
// is mixed in the SDR display view (OpenColorIoCompositeDisplayReferred),
// no longer in the working space, where a translucent white stayed white.
RgbColor DisplayReferredComposite(const ColorManagementSettings& color,
                                  const RgbColor& source, const Pixel& motion,
                                  double opacity) {
    RgbColor below;
    RgbColor mixed;
    RgbColor shown;
    std::string error;
    OpenColorIoWorkingColor(color, ResolvedColorGrade{}, source, below, error);
    OpenColorIoCompositeDisplayReferred(color, below, motion.color,
                                        motion.alpha * opacity, mixed, error);
    OpenColorIoDisplayColor(color, mixed, shown, error);
    return shown;
}

struct Comparison {
    double interior = 0.0;  // worst where the motion is opaque or absent
    double edge = 0.0;      // worst where its alpha is strictly between
    size_t edges = 0;
};

// Every pixel of the title's region -- white over grey, so the 4:2:0
// chroma of the delivery cannot blur a colour into it -- and the centre of
// the red box, clear of its edges.
Comparison Measure(const FloatFrames& shown, size_t frame,
                   const MotionRaster& reference, const RgbColor& source,
                   const ColorManagementSettings& color, Composite composite,
                   double opacity) {
    Comparison result;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 70; x < kWidth; ++x) {
            const Pixel motion = At(reference, x, y);
            const RgbColor expected = composite(color, source, motion, opacity);
            const double distance = Distance(shown.At(frame, x, y), expected);
            if (motion.alpha > 0.0 && motion.alpha < 1.0) {
                result.edge = std::max(result.edge, distance);
                ++result.edges;
            } else {
                result.interior = std::max(result.interior, distance);
            }
        }
    }
    return result;
}

Comparison Compare(const FloatFrames& shown, size_t frame,
                   const MotionRaster& reference, const RgbColor& source,
                   const ColorManagementSettings& color, Composite composite,
                   double opacity, const std::string& label, double tolerance,
                   double edgeTolerance) {
    const Comparison result =
        Measure(shown, frame, reference, source, color, composite, opacity);
    Check(result.interior <= tolerance,
          label + ": the title and the grey around it, worst " +
              std::to_string(result.interior));
    Check(result.edges > 20 && result.edge <= edgeTolerance,
          label + ": the antialiased edge of the title follows its alpha (" +
              std::to_string(result.edges) + " pixels), worst " +
              std::to_string(result.edge));
    // The yellow box: its centre, and its top edge by luma, which 4:2:0
    // does not blur (BT.709 luma of the decoded RGB is the Y plane). A frame
    // shown one instant off moves that edge by a row.
    int top = -1;
    for (int y = 0; y < kHeight && top < 0; ++y) {
        if (At(reference, 32, y).alpha == 1.0) {
            top = y;
        }
    }
    if (top >= 1 && top + 12 < kHeight && opacity > 0.0) {
        const Pixel box = At(reference, 32, top + 12);
        const RgbColor expected = composite(color, source, box, opacity);
        const RgbColor actual = shown.At(frame, 32, top + 12);
        Check(Distance(actual, expected) <= tolerance,
              label + ": the yellow box's colour: got " + Describe(actual) +
                  ", expected " + Describe(expected));
        const auto luma = [](const RgbColor& value) {
            return (0.2126 * value.red) + (0.7152 * value.green) +
                   (0.0722 * value.blue);
        };
        for (const int row : {top - 1, top}) {
            const RgbColor want =
                composite(color, source, At(reference, 32, row), opacity);
            Check(std::abs(luma(shown.At(frame, 32, row)) - luma(want)) <=
                      tolerance,
                  label + ": the yellow box's top edge on row " +
                      std::to_string(row));
        }
    }
    return result;
}

struct Case {
    const char* label;
    bool managed;
    Composite composite;
    double tolerance;
    double edge_tolerance;
};

// V1: a grey shot for 3 s. V2: two instances of the lower third, the first
// at 3/4 opacity from 0.4 s, source_in 5 frames and its own title; the
// second takes over at 1.6 s through a 10-frame dissolve, with the default
// title.
void CheckMotionOverInterview(const std::filesystem::path& directory,
                              const Case& test) {
    const std::string label = test.label;
    const auto sourcePath = directory / (label + "-grey.mp4");
    FloatFrames sourceFrames;
    if (!WriteFlatSource(sourcePath, "0x5A5A5A") ||
        !ReadFloatFrames(sourcePath.string(), sourceFrames) ||
        sourceFrames.Count() == 0) {
        Check(false, label + ": the grey source is generated");
        return;
    }
    const RgbColor grey = sourceFrames.At(0, kWidth / 2, kHeight / 2);

    Document document;
    document.sequence.id = GenerateUlid();
    document.sequence.name = "Motion";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.color_management.enabled = test.managed;
    document.sources = {
        {GenerateUlid(), sourcePath.string(), {25, 1}, {100, 25}}};
    // The rush is in the library, as in every real project: the export
    // asks the library whether each layer's media is a graphic (DEC31).
    // With an empty library the lookup never ran, and a motion clip, which
    // has no media, was asked through a null source unseen (a crash on
    // every real export with a title, 26 October 2026).
    LibraryMedia rush;
    rush.id = document.sources[0].id;
    rush.path = sourcePath.string();
    rush.filename = sourcePath.filename().string();
    rush.codec = "h264";
    rush.width = kWidth;
    rush.height = kHeight;
    rush.orientation = "landscape";
    rush.rate = {25, 1};
    rush.duration = {100, 25};
    document.library = {rush};
    document.compositions = {{kComposition, "Titre", Json(kDefinition)}};
    DocumentClip shot;
    shot.source_id = document.sources[0].id;
    shot.source_in = {0, 25};
    shot.duration = {75, 25};
    shot.timeline_in = {0, 25};
    shot.include_audio = false;
    DocumentClip first;
    first.composition_id = kComposition;
    first.source_in = {5, 25};
    first.duration = {30, 25};
    first.timeline_in = {10, 25};
    first.include_audio = false;
    first.opacity = {3, 4};
    first.motion_props = Json(R"({"titre":"Lo"})");
    DocumentClip second;
    second.composition_id = kComposition;
    second.source_in = {20, 25};
    second.duration = {25, 25};
    second.timeline_in = {40, 25};
    second.include_audio = false;
    const Ulid upper = GenerateUlid();
    document.sequence.tracks = {{GenerateUlid(), "video", 0, {shot}},
                                {upper, "video", 1, {first, second}}};
    document.sequence.transitions = {
        {GenerateUlid(), upper, first.id, second.id, "cross_dissolve",
         RationalTime{10, 25}, TransitionAlignment::Center}};
    std::string error;
    Check(document.Validate(error),
          label + ": the timeline is valid: " + error);

    const auto output = directory / (label + "-export.mp4");
    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(output.string());
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    const auto started = std::chrono::steady_clock::now();
    const bool exported =
        Exporter::BuildPlan(document, (directory / "motion.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    Check(exported, label + ": the timeline exports: " + error);
    Check(plan.motion_assets.size() == 2 &&
              plan.motion_assets[0].frames.front() == 5 &&
              plan.motion_assets[0].frames.size() == 35 &&
              plan.motion_assets[1].frames.front() == 15,
          label +
              ": each clip renders its own frames, the outgoing one "
              "five more for the dissolve, the incoming one from five "
              "before its cut");
    FloatFrames shown;
    if (!exported || !ReadFloatFrames(output.string(), shown) ||
        shown.Count() != 75) {
        Check(false, label + ": the export decodes to 75 frames");
        return;
    }
    std::cout << label << ": exported in " << seconds << " s\n";
    // verify_export: the frames a motion clip is drawn on have no media to
    // compare with; they are counted unverifiable, the rest checked.
    if (!test.managed) {
        ExportVerificationSettings verification;
        verification.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportVerificationReport report;
        Check(VerifyExport(document, directory, output.string(), verification,
                           report, error) &&
                  report.unverifiable_frames == 55 &&
                  report.checked_frames == 20 && report.divergent_frames == 0,
              label +
                  ": verify_export counts the motion frames "
                  "unverifiable (" +
                  std::to_string(report.unverifiable_frames) +
                  ") and checks "
                  "the others (" +
                  std::to_string(report.checked_frames) + "): " + error);
    }
    const ColorManagementSettings& color = document.color_management;

    // 1. Before the motion clip: the shot alone.
    Compare(shown, 5, Reference(Value(), 0), grey, color, test.composite, 0.0,
            label + " frame 5", test.tolerance, 1.0);
    // 2. The first instance, alone, at 3/4: composition frame 5 + 15.
    const Comparison alone =
        Compare(shown, 25, Reference(Json(R"({"titre":"Lo"})"), 20), grey,
                color, test.composite, 0.75, label + " frame 25",
                test.tolerance, test.edge_tolerance);
    // 3. Mid-dissolve, frame 42 = cut 40 + 2: the outgoing instance at
    // composition frame 5 + 32, opacity 3/4, under the incoming one at
    // 20 + 2, faded in to 7/10.
    {
        const MotionRaster outgoing = Reference(Json(R"({"titre":"Lo"})"), 37);
        const MotionRaster incoming = Reference(Value(), 22);
        // Where both titles are opaque or absent the pixel is exact to the
        // tolerance; where an antialiased edge of one crosses the other,
        // x265 at CRF 18 smooths the faint detail of a dissolve, so edges
        // are held on average and bounded, not pixel by pixel.
        double worstFlat = 0.0;
        double worstEdge = 0.0;
        double edgeSum = 0.0;
        size_t edgeCount = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 70; x < kWidth; ++x) {
                const Pixel low = At(outgoing, x, y);
                const Pixel high = At(incoming, x, y);
                RgbColor expected;
                if (test.managed) {
                    RgbColor base;
                    RgbColor under;
                    RgbColor mixed;
                    OpenColorIoWorkingColor(color, ResolvedColorGrade{}, grey,
                                            base, error);
                    OpenColorIoCompositeDisplayReferred(
                        color, base, low.color, low.alpha * 0.75, under, error);
                    OpenColorIoCompositeDisplayReferred(
                        color, under, high.color, high.alpha * 0.7, mixed,
                        error);
                    OpenColorIoDisplayColor(color, mixed, expected, error);
                } else {
                    expected = Mix(Mix(grey, low.color, low.alpha * 0.75),
                                   high.color, high.alpha * 0.7);
                }
                const double distance = Distance(shown.At(42, x, y), expected);
                // Flat: both titles opaque or absent over the pixel and
                // its neighbours, clear of the ringing an edge leaves.
                // DEC28 -- docs/roadmap/REEL-2026-09.md: three pixels each
                // way, not one. Once the unmanaged composite kept 10 bits
                // in 4:4:4, x265 at CRF 18 coded one pixel three from a
                // title's edge 0.0227 off (0.0102 from the 8-bit 4:2:0
                // composite); at CRF 4 the same frame is within 0.0063 of
                // the reference (0.0051 before), so the composite is not
                // what moved. Edge pixels stay held on average and bounded.
                bool flat = true;
                for (int dy = -3; dy <= 3 && flat; ++dy) {
                    for (int dx = -3; dx <= 3 && flat; ++dx) {
                        const int nx = std::clamp(x + dx, 0, kWidth - 1);
                        const int ny = std::clamp(y + dy, 0, kHeight - 1);
                        flat = At(outgoing, nx, ny).alpha == low.alpha &&
                               At(incoming, nx, ny).alpha == high.alpha &&
                               (low.alpha == 0.0 || low.alpha == 1.0) &&
                               (high.alpha == 0.0 || high.alpha == 1.0);
                    }
                }
                if (flat) {
                    worstFlat = std::max(worstFlat, distance);
                } else {
                    worstEdge = std::max(worstEdge, distance);
                    edgeSum += distance;
                    ++edgeCount;
                }
            }
        }
        const double edgeMean =
            edgeCount == 0 ? 1.0 : edgeSum / static_cast<double>(edgeCount);
        Check(worstFlat <= test.tolerance,
              label +
                  " frame 42: the incoming title fades in at 7/10 over "
                  "the outgoing one at 3/4, worst " +
                  std::to_string(worstFlat));
        Check(edgeCount > 40 && edgeMean <= 0.01 && worstEdge <= 0.1,
              label + " frame 42: the edges of both titles, mean " +
                  std::to_string(edgeMean) + ", worst " +
                  std::to_string(worstEdge));
        std::cout << label << ": dissolve worst " << worstFlat
                  << ", edges mean " << edgeMean << " worst " << worstEdge
                  << " over " << edgeCount << " pixels\n";
    }
    std::cout << label << ": interior worst " << alone.interior
              << ", title edge worst " << alone.edge << " over " << alone.edges
              << " pixels\n";

    // The wrong frame, or the default title, would not pass: the check
    // tells them apart.
    {
        const Comparison control = Measure(shown, 25, Reference(Value(), 20),
                                           grey, color, test.composite, 0.75);
        Check(control.interior > 10 * test.tolerance,
              label +
                  ": the comparison tells the instance's title from the "
                  "default one, worst " +
                  std::to_string(control.interior));
    }
}

// R2b -- docs/roadmap/REEL-2026-09.md. The end card of the 22 October 2026
// relevé: a motion clip alone on its track, fading to black by its
// own opacity keys over its last 28 frames. The export used to refuse it at
// the first frame while the monitor played it. Each measured frame is the
// engine's render of that instant, at the opacity SampleClipParameters gives
// that frame of the clip, over the composite's black.
void CheckMotionClipOpacityFade(const std::filesystem::path& directory,
                                const Case& test) {
    const std::string label = std::string(test.label) + " fade";
    constexpr int64_t kStart = 10;
    constexpr int64_t kLength = 40;
    constexpr int64_t kSourceIn = 5;
    Document document;
    document.sequence.id = GenerateUlid();
    document.sequence.name = "Carton de fin";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.color_management.enabled = test.managed;
    document.compositions = {{kComposition, "Titre", Json(kDefinition)}};
    DocumentClip card;
    card.composition_id = kComposition;
    card.source_in = {kSourceIn, 25};
    card.duration = {kLength, 25};
    card.timeline_in = {kStart, 25};
    card.include_audio = false;
    ClipParameterAnimation fade;
    fade.parameter = ClipAnimatedParameter::Opacity;
    ClipKeyframe from;
    from.id = GenerateUlid();
    from.time = {kLength - 28, 25};
    from.value = {1, 1};
    ClipKeyframe to;
    to.id = GenerateUlid();
    to.time = {kLength - 1, 25};
    to.value = {0, 1};
    fade.keyframes = {from, to};
    card.animation = {fade};
    document.sequence.tracks = {{GenerateUlid(), "video", 0, {card}}};
    std::string error;
    Check(document.Validate(error),
          label + ": the timeline is valid: " + error);

    const auto output = directory / (label + "-export.mp4");
    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(output.string());
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    const bool exported =
        Exporter::BuildPlan(document, (directory / "fade.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(exported,
          label + ": a motion clip whose opacity moves exports: " + error);
    FloatFrames shown;
    if (!exported || !ReadFloatFrames(output.string(), shown) ||
        shown.Count() != static_cast<size_t>(kStart + kLength)) {
        Check(false, label + ": the export decodes to " +
                         std::to_string(kStart + kLength) + " frames");
        return;
    }
    if (!test.managed) {
        // R2 -- a frame of a clip whose opacity moves is not compared with
        // a picture it no longer shows at full strength: the card's 40
        // frames are counted unverifiable, none diverges.
        ExportVerificationSettings verification;
        verification.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportVerificationReport report;
        Check(VerifyExport(document, directory, output.string(), verification,
                           report, error) &&
                  report.exported_frames == kStart + kLength &&
                  report.unverifiable_frames == kLength &&
                  report.divergent_frames == 0,
              label + ": verify_export reads the faded card back (" +
                  std::to_string(report.unverifiable_frames) +
                  " unverifiable, " + std::to_string(report.divergent_frames) +
                  " divergent): " + error);
    }

    const RgbColor black{0, 0, 0};
    const ColorManagementSettings& color = document.color_management;
    // Before the fade, half way down it, two thirds down, and its last
    // frame, where the card is gone.
    for (const int64_t local :
         {int64_t{5}, kLength - 28 + 9, kLength - 28 + 18, kLength - 1}) {
        ClipSampledParameters sampled;
        if (!SampleClipParameters(card, local, document.sequence.frame_rate,
                                  sampled, error)) {
            Check(false, label + ": the engine samples frame " +
                             std::to_string(local) + ": " + error);
            continue;
        }
        const double opacity = static_cast<double>(sampled.opacity.num) /
                               static_cast<double>(sampled.opacity.den);
        Compare(shown, static_cast<size_t>(kStart + local),
                Reference(Value(), kSourceIn + local), black, color,
                test.composite, opacity,
                label + " clip frame " + std::to_string(local) +
                    " at opacity " + std::to_string(opacity),
                test.tolerance, test.edge_tolerance);
    }
    // The card shown at full strength mid-fade -- the still the old graph
    // would have delivered -- does not pass.
    const Comparison still =
        Measure(shown, static_cast<size_t>(kStart + kLength - 28 + 18),
                Reference(Value(), kSourceIn + kLength - 28 + 18), black, color,
                test.composite, 1.0);
    Check(still.interior > 10 * test.tolerance,
          label + ": the comparison tells the fade from a still, worst " +
              std::to_string(still.interior));
}

// The monitor draws what the export draws: the picture RenderMotionPreview
// hands Renderer.mm is the export's frame, alpha and colour.
void CheckMonitorPicture() {
    DocumentComposition stored{kComposition, "Titre", Json(kDefinition)};
    MotionPreviewRequest request;
    request.composition = &stored;
    request.props = Json(R"({"titre":"Lo"})");
    request.frame = 20;
    MotionPreviewImage image;
    MotionError error;
    Check(RenderMotionPreview(request, Document{}, std::string(), image, error),
          "the monitor picture renders: " + error.message);
    const MotionRaster reference = Reference(request.props, 20);
    MotionPreviewImage expected;
    MotionPreviewFromPremultiplied(reference.pixels.data(), reference.width,
                                   reference.height, expected);
    Check(image.width == kWidth && image.height == kHeight &&
              image.planes == expected.planes,
          "the monitor shows the export's frame, alpha included");
}

// FI12 -- the original package is unavailable when the collected copy renders.
void CheckCollectedMotionImage(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    const fs::path sourceDirectory = directory / "image-source";
    Document document;
    DocumentComposition stored;
    stored.name = "Portable image";
    stored.definition = WithDefinitionIdentities(Json(R"({
      "width":32,"height":32,"fps":25,"duration":25,
      "root":{"type":"fill","children":[
        {"type":"image","src":"images/logo.png",
         "style":{"width":32,"height":32}}]}})"));
    document.compositions = {stored};
    Project project = Project::FromDocument(document, "Portable image");
    std::string sourcePath;
    std::string error;
    Check(CreatePortableProject(sourceDirectory.string(), project, sourcePath,
                                error),
          "create motion-image package: " + error);
    fs::create_directories(sourceDirectory / "images");
    MotionRaster raster;
    raster.width = 32;
    raster.height = 32;
    raster.pixels.resize(size_t{32} * 32 * 4, 255);
    MotionError motionError;
    Check(WriteMotionPng(raster, (sourceDirectory / "images/logo.png").string(),
                         motionError),
          "write the real image fixture: " + motionError.message);
    MotionPreviewRequest request;
    request.composition = &document.compositions.front();
    request.frame = 0;
    MotionPreviewImage before;
    Check(RenderMotionPreview(request, document, sourceDirectory.string(),
                              before, motionError),
          "render the original motion image: " + motionError.message);
    PortableProjectResult result;
    Check(CollectPortableProject(sourcePath,
                                 (directory / "image-collected").string(),
                                 result, error),
          "collect the real motion image: " + error);
    fs::rename(sourceDirectory, directory / "image-source-unavailable");
    Project reopened;
    Check(LoadStoredProject(result.project_path, reopened, error),
          "load the relocated image project: " + error);
    if (reopened.compositions.empty()) {
        return;
    }
    document = reopened.MakeActiveDocument();
    request.composition = &document.compositions.front();
    MotionPreviewImage after;
    Check(RenderMotionPreview(request, document, result.directory_path, after,
                              motionError) &&
              after.width == before.width && after.height == before.height &&
              after.planes == before.planes,
          "the collected image renders identically without its source: " +
              motionError.message);
}

// FI12 -- the shipped `logo` template as it is used: each clip names a logo
// outside the package by its absolute path, and the template's default
// (logo.png) exists nowhere. The copy renders the clip with neither the
// package nor the logo's folder left where they were.
void CheckCollectedLogoTemplate(const std::filesystem::path& directory) {
    namespace fs = std::filesystem;
    std::vector<MotionTemplateInfo> catalog;
    std::string error;
    Check(MotionTemplateCatalog(catalog, error),
          "read the shipped templates: " + error);
    const auto shipped = std::find_if(
        catalog.begin(), catalog.end(),
        [](const MotionTemplateInfo& piece) { return piece.name == "logo"; });
    if (shipped == catalog.end()) {
        Check(false, "the logo template ships");
        return;
    }
    Value instance;
    std::string format;
    Check(InstantiateMotionTemplate(shipped->definition, 1080, 1080, instance,
                                    format, error),
          "instantiate the logo template in 1:1: " + error);
    Document document;
    DocumentComposition stored;
    stored.name = "Logo";
    stored.definition = WithDefinitionIdentities(instance);
    document.compositions = {stored};

    // Two colours side by side: a logo drawn mirrored or from another file
    // would not give the same pixels.
    const fs::path brand = directory / "logo-brand";
    fs::create_directories(brand);
    MotionRaster raster;
    raster.width = 64;
    raster.height = 32;
    raster.pixels.resize(size_t{64} * 32 * 4, 255);
    for (size_t pixel = 0; pixel < size_t{64} * 32; ++pixel) {
        const bool left = pixel % 64 < 32;
        raster.pixels[pixel * 4 + 0] = left ? 224 : 0;
        raster.pixels[pixel * 4 + 1] = left ? 60 : 0;
        raster.pixels[pixel * 4 + 2] = left ? 40 : 255;
    }
    const fs::path mark = brand / "marque.png";
    MotionError motionError;
    Check(WriteMotionPng(raster, mark.string(), motionError),
          "write the brand logo: " + motionError.message);

    DocumentTrack track;
    track.kind = "video";
    DocumentClip clip;
    clip.composition_id = stored.id;
    clip.timeline_in = {0, 25};
    clip.source_in = {0, 25};
    clip.duration = {250, 25};
    clip.motion_props =
        Json("{\"logo\":" + Value::MakeString(mark.string()).Dump() +
             ",\"coin\":\"bas-gauche\"}");
    track.clips = {clip};
    document.sequence.tracks = {track};
    Project project = Project::FromDocument(document, "Logo");
    const fs::path sourceDirectory = directory / "logo-source";
    std::string sourcePath;
    Check(CreatePortableProject(sourceDirectory.string(), project, sourcePath,
                                error),
          "create the logo package: " + error);

    // Frame 120: the logo has faded in and not yet out.
    MotionPreviewRequest request;
    request.composition = &document.compositions.front();
    request.props = clip.motion_props;
    request.frame = 120;
    MotionPreviewImage before;
    Check(RenderMotionPreview(request, document, sourceDirectory.string(),
                              before, motionError),
          "render the posed logo: " + motionError.message);
    size_t covered = 0;
    for (size_t pixel = 0; pixel < before.PlaneSize(); ++pixel) {
        covered += before.planes[(3 * before.PlaneSize()) + pixel] != 0 ? 1 : 0;
    }
    Check(covered > 1000, "the posed logo is drawn, over " +
                              std::to_string(covered) + " pixels");

    PortableProjectResult result;
    Check(
        CollectPortableProject(
            sourcePath, (directory / "logo-collected").string(), result, error),
        "collect the logo posed by its absolute path: " + error);
    fs::rename(sourceDirectory, directory / "logo-source-unavailable");
    fs::rename(brand, directory / "logo-brand-unavailable");
    Project reopened;
    Check(LoadStoredProject(result.project_path, reopened, error),
          "load the relocated logo project: " + error);
    if (reopened.compositions.empty() || reopened.timelines.empty() ||
        reopened.timelines.front().tracks.empty() ||
        reopened.timelines.front().tracks.front().clips.empty()) {
        Check(false, "the relocated logo project keeps its clip");
        return;
    }
    document = reopened.MakeActiveDocument();
    request.composition = &document.compositions.front();
    request.props =
        reopened.timelines.front().tracks.front().clips.front().motion_props;
    MotionPreviewImage after;
    Check(RenderMotionPreview(request, document, result.directory_path, after,
                              motionError) &&
              after.width == before.width && after.height == before.height &&
              after.planes == before.planes,
          "the collected logo renders identically without the package or "
          "the logo's folder: " +
              motionError.message);
}

// A hex colour of the composition comes out of the display transform as
// written; read as a camera Rec.709 signal, it would go through the tone
// curve of the view. Both measured, in full precision.
void CheckDisplayReferredRoundTrip() {
    ColorManagementSettings color;
    color.enabled = true;
    ColorManagementSettings camera = color;
    camera.input_gamut = "rec709";
    camera.input_transfer = "rec709";
    double worst = 0.0;
    for (const RgbColor& signal :
         {RgbColor{1.0, 1.0, 1.0}, RgbColor{0.5, 0.5, 0.5},
          RgbColor{0.878, 0.235, 0.157}, RgbColor{1.0, 0.824, 0.247},
          RgbColor{0.0, 0.0, 0.0}}) {
        RgbColor working;
        RgbColor shown;
        std::string error;
        Check(OpenColorIoDisplayReferredWorkingColor(color, signal, working,
                                                     error) &&
                  OpenColorIoDisplayColor(color, working, shown, error),
              "display-referred path evaluates: " + error);
        worst = std::max(worst, Distance(shown, signal));
    }
    RgbColor working;
    RgbColor dimmed;
    std::string error;
    OpenColorIoWorkingColor(camera, ResolvedColorGrade{}, {1.0, 1.0, 1.0},
                            working, error);
    OpenColorIoDisplayColor(camera, working, dimmed, error);
    Check(worst < 4e-3,
          "a composition's colours come back as written, "
          "within one 8-bit code, worst " +
              std::to_string(worst));
    Check(dimmed.red < 0.9,
          "read as a camera signal, white would come back "
          "dimmed: " +
              Describe(dimmed));
    std::cout << "display-referred round trip worst " << worst
              << "; white as a camera Rec.709 signal shows " << Describe(dimmed)
              << '\n';
}

// Visual QA of 19 September 2026 -- the rule a translucent motion clip is
// mixed by under colour management (OpenColorIoCompositeDisplayReferred).
void CheckDisplayReferredComposite() {
    ColorManagementSettings color;
    color.enabled = true;
    color.input_gamut = "sony_sgamut3_cine";
    color.input_transfer = "sony_slog3";
    const ColorManagementSettings view = ColorManagementForSdrPreview(color);
    std::string error;
    RgbColor black;
    RgbColor grey;
    OpenColorIoWorkingColor(color, ResolvedColorGrade{}, {0.0, 0.0, 0.0}, black,
                            error);
    OpenColorIoWorkingColor(color, ResolvedColorGrade{}, {0.41, 0.41, 0.41},
                            grey, error);
    RgbColor mixed;
    Check(
        OpenColorIoCompositeDisplayReferred(color, grey, {1.0, 0.0, 0.0}, 0.0,
                                            mixed, error) &&
            mixed.red == grey.red && mixed.green == grey.green &&
            mixed.blue == grey.blue,
        "a layer covering nothing leaves the composite bit for bit: " + error);
    RgbColor shownBlack;
    RgbColor shown;
    OpenColorIoDisplayColor(view, black, shownBlack, error);
    Check(OpenColorIoCompositeDisplayReferred(color, black, {1.0, 1.0, 1.0},
                                              0.5, mixed, error) &&
              OpenColorIoDisplayColor(view, mixed, shown, error),
          "white at 50 % over black composes: " + error);
    const double half = shownBlack.green + ((1.0 - shownBlack.green) * 0.5);
    Check(std::abs(shown.red - half) < 4e-3 &&
              std::abs(shown.green - half) < 4e-3 &&
              std::abs(shown.blue - half) < 4e-3,
          "white at 50 % over black is half-way in the view, not white: " +
              Describe(shown));
    std::cout << "white at 50 % over black shows " << Describe(shown) << '\n';
    // Opaque, the layer hides the composite: the pixel is the layer alone.
    // (A saturated colour with a zero channel does not come back exactly as
    // written, in this path as before it: #E6007E shows green 0.044, the
    // SDR view's gamut compression not being exactly invertible.)
    RgbColor alone;
    RgbColor aloneShown;
    OpenColorIoDisplayReferredWorkingColor(color, {0.9, 0.0, 0.494}, alone,
                                           error);
    OpenColorIoDisplayColor(view, alone, aloneShown, error);
    Check(OpenColorIoCompositeDisplayReferred(color, grey, {0.9, 0.0, 0.494},
                                              1.0, mixed, error) &&
              OpenColorIoDisplayColor(view, mixed, shown, error) &&
              Distance(shown, aloneShown) < 1e-6,
          "an opaque layer shows as the layer alone: " + Describe(shown) +
              " against " + Describe(aloneShown));
}

// BH1 -- docs/roadmap/BLANC-HLG-2026-10.md. In an HLG delivery a graphic is
// placed in the delivery itself: white at the view's peak (through the SDR
// view it sat at 0.93 of the signal, some 640 nits), a neutral grey at its
// own value, a colour at the HLG signal of its code value, and a translucent
// layer mixed in its code values. An SDR delivery keeps the checks above.
void CheckDisplayReferredHlg() {
    ColorManagementSettings color;
    color.enabled = true;
    color.input_gamut = "sony_sgamut3_cine";
    color.input_transfer = "sony_slog3";
    color.output_gamut = "rec2020";
    color.output_transfer = "hlg";
    Check(GraphicsPlacedInHlg(color) &&
              !GraphicsPlacedInHlg(ColorManagementForSdrPreview(color)),
          "graphics are placed in the delivery only when it is HLG");
    std::string error;
    const auto shownFor = [&](const RgbColor& code) {
        RgbColor working;
        RgbColor shown;
        Check(OpenColorIoDisplayReferredWorkingColor(color, code, working,
                                                     error) &&
                  OpenColorIoDisplayColor(color, working, shown, error),
              "the HLG graphics path evaluates: " + error);
        return shown;
    };
    const RgbColor white = shownFor({1.0, 1.0, 1.0});
    Check(white.red > 0.99 && white.green > 0.99 && white.blue > 0.99,
          "a white title is at the HLG view's peak, not at the SDR view's "
          "white: " +
              Describe(white));
    const RgbColor grey = shownFor({0.5, 0.5, 0.5});
    Check(Distance(grey, {0.5, 0.5, 0.5}) < 4e-3,
          "a neutral grey keeps its value as an HLG signal: " + Describe(grey));
    // A saturated colour inside Rec.709 (a red, #C85A5A) comes back as the
    // HLG signal of its code value.
    const RgbColor rouge{200.0 / 255.0, 90.0 / 255.0, 90.0 / 255.0};
    const RgbColor rougeShown = shownFor(rouge);
    const RgbColor rougeSignal = GraphicsCodeToHlgSignal(rouge);
    Check(Distance(rougeShown, rougeSignal) < 4e-3,
          "a colour comes out as the HLG signal of its code value: " +
              Describe(rougeShown) + " against " + Describe(rougeSignal));
    Check(Distance(HlgSignalToGraphicsCode(rougeSignal), rouge) < 1e-6,
          "the code-to-signal conversion inverts exactly inside Rec.709");
    // White at 50 % over black: half-way in code values, hence in the HLG
    // signal for a neutral.
    RgbColor black;
    RgbColor mixed;
    RgbColor shown;
    OpenColorIoWorkingColor(color, ResolvedColorGrade{}, {0.0, 0.0, 0.0}, black,
                            error);
    RgbColor blackShown;
    OpenColorIoDisplayColor(color, black, blackShown, error);
    Check(OpenColorIoCompositeDisplayReferred(color, black, {1.0, 1.0, 1.0},
                                              0.5, mixed, error) &&
              OpenColorIoDisplayColor(color, mixed, shown, error),
          "white at 50 % over black composes in HLG: " + error);
    const double half = blackShown.green + ((1.0 - blackShown.green) * 0.5);
    Check(std::abs(shown.green - half) < 4e-3,
          "white at 50 % over black is half-way in the HLG signal: " +
              Describe(shown));
    std::cout << "HLG graphics: white shows " << Describe(white) << ", grey "
              << Describe(grey) << ", red " << Describe(rougeShown)
              << ", white at 50 % over black " << Describe(shown) << '\n';
}

std::string DecodeBase64ForTest(const std::string& text) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char character : text) {
        const size_t value = alphabet.find(character);
        if (value == std::string::npos) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return bytes;
}

std::string Run(const std::string& command) {
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return output;
    }
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof buffer, pipe)) > 0) {
        output.append(buffer, count);
    }
    pclose(pipe);
    return output;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

// The surfaces: a motion clip posed and changed by the MCP tools, looked
// at by render_motion_frame, read_frame, --render-motion-frame and
// --describe, posed again by --apply-op, and exported by the executable,
// which installs the renderer the export draws with. The title uses a
// system face: the executable registers no font directory for --export.
void CheckSurfaces(const std::filesystem::path& directory) {
    const auto grey = directory / "surfaces-grey.mp4";
    if (!WriteFlatSource(grey, "0x5A5A5A")) {
        Check(false, "surfaces: the grey source is generated");
        return;
    }
    std::string definition = kDefinition;
    definition.replace(definition.find("Fira Sans Condensed"), 19, "Helvetica");
    Document document;
    document.sequence.id = "01K90000000000000000000001";
    document.sequence.name = "Surfaces";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sources = {
        {"01K90000000000000000000002", grey.string(), {25, 1}, {100, 25}}};
    document.compositions = {{kComposition, "Titre", Json(definition)}};
    DocumentClip shot;
    shot.id = "01K90000000000000000000003";
    shot.source_id = document.sources[0].id;
    shot.source_in = {0, 25};
    shot.duration = {50, 25};
    shot.timeline_in = {0, 25};
    shot.include_audio = false;
    document.sequence.tracks = {
        {"01K90000000000000000000004", "video", 0, {shot}},
        {"01K90000000000000000000005", "video", 1, {}}};
    std::string path;
    std::string error;
    Check(CreatePortableProject((directory / "Surfaces").string(),
                                Project::FromDocument(document, "Surfaces"),
                                path, error),
          "surfaces: the project saves: " + error);

    McpProjectBackend backend(path);
    const McpToolRegistry tools;
    const auto call = [&](const std::string& tool, const std::string& args) {
        return tools.Call(backend, tool, Json(args));
    };
    McpToolCallOutcome inserted = call(
        "insert_motion_clip",
        R"({"track_id":"01K90000000000000000000005","composition_id":"01K9",)"
        R"("timeline_in":{"value":10,"rate":25},)"
        R"("source_in":{"value":5,"rate":25},"props":{"titre":"Lo"}})");
    Value result;
    Value::Parse(inserted.result_json, result, error);
    const Value* clipField = result.Find("clip_id");
    Check(
        inserted.ok && clipField != nullptr,
        "surfaces: insert_motion_clip poses the clip: " + inserted.result_json);
    if (clipField == nullptr) {
        return;
    }
    const std::string clip = clipField->AsString();
    Project project;
    Check(LoadStoredProject(path, project, error) &&
              project.MakeActiveDocument().FindClip(clip) != nullptr &&
              project.MakeActiveDocument().FindClip(clip)->duration ==
                  RationalTime{55, 25},
          "surfaces: the clip is stored, running to the composition's end");

    const McpToolCallOutcome merged =
        call("set_clip_motion_props",
             R"({"clip_id":")" + clip + R"(","props":{"titre":"Ho"}})");
    Check(merged.ok &&
              merged.result_json.find(R"("motion_props":{"titre":"Ho"})") !=
                  std::string::npos,
          "surfaces: set_clip_motion_props merges: " + merged.result_json);
    const McpToolCallOutcome unknown =
        call("set_clip_motion_props",
             R"({"clip_id":")" + clip + R"(","props":{"sous_titre":"x"}})");
    Check(!unknown.ok && unknown.error_name == "UnknownProp",
          "surfaces: an undeclared prop is refused by name: " +
              unknown.error_name + " " + unknown.message);

    const McpToolCallOutcome frame =
        call("render_motion_frame",
             R"({"clip_id":")" + clip + R"(","time":{"value":25,"rate":25}})");
    Check(frame.ok && frame.image_mime == "image/png" &&
              frame.result_json.find(R"("frame":20)") != std::string::npos,
          "surfaces: render_motion_frame shows the clip's frame 20 at 1 s: " +
              frame.result_json.substr(0, 200) + " " + frame.message);
    const McpToolCallOutcome outside =
        call("render_motion_frame",
             R"({"clip_id":")" + clip + R"(","time":{"value":5,"rate":25}})");
    Check(
        !outside.ok && outside.error_name == "InvalidTimelineIn",
        "surfaces: a time outside the clip is refused: " + outside.error_name);
    const McpToolCallOutcome read = call(
        "read_frame", R"({"clip_id":")" + clip + R"(","position":"Start"})");
    Check(read.ok && read.image_mime == "image/png" &&
              read.result_json.find(R"("frame":5)") != std::string::npos,
          "surfaces: read_frame shows a motion clip as the engine renders "
          "it: " +
              read.message);
    const McpToolCallOutcome described = call("describe", "{}");
    Check(described.ok &&
              described.result_json.find(R"("composition_id":")" +
                                         kComposition) != std::string::npos &&
              described.result_json.find(R"("motion_props":{"titre":"Ho"})") !=
                  std::string::npos,
          "surfaces: describe names the composition and the props");

    // The CLI renders the same frame, byte for byte.
    const auto cliFrame = directory / "surfaces-cli.png";
    const std::string rendered = Run(
        Quote(CUTMACHINE_EXECUTABLE) + " --render-motion-frame " + Quote(path) +
        " 25 " + Quote(cliFrame.string()) + " --clip " + clip);
    Check(rendered.find("\"ok\":true") != std::string::npos &&
              ReadFile(cliFrame) ==
                  std::string(DecodeBase64ForTest(frame.image_base64)),
          "surfaces: --render-motion-frame --clip draws what the MCP tool "
          "does: " +
              rendered);
    const std::string describedCli =
        Run(Quote(CUTMACHINE_EXECUTABLE) + " --describe " + Quote(path));
    Check(describedCli.find(R"("composition_id":")" + kComposition) !=
              std::string::npos,
          "surfaces: --describe names the composition: " +
              describedCli.substr(0, 200));
    const std::string applied =
        Run(Quote(CUTMACHINE_EXECUTABLE) + " --apply-op " + Quote(path) + " " +
            Quote(R"({"type":"InsertMotionClip",)"
                  R"("track_id":"01K90000000000000000000005",)"
                  R"("composition_id":")" +
                  kComposition +
                  R"(","source_in":{"value":0,"rate":25},)"
                  R"("duration":{"value":5,"rate":25},)"
                  R"("timeline_in":{"value":0,"rate":25},)"
                  R"("motion_props":{"titre":"Hé"},"clip_id":"",)"
                  R"("exact_timeline":[],"sync_track_ids":[]})"));
    Check(applied.find("\"ok\":true") != std::string::npos,
          "surfaces: --apply-op poses a motion clip: " + applied);

    // The executable exports the timeline with its motion clips. The clip
    // --apply-op inserted at 0 pushed the first one 5 frames later, as an
    // insertion does: it now ends at 70.
    const auto exported = directory / "surfaces.mp4";
    const std::string report =
        Run(Quote(CUTMACHINE_EXECUTABLE) + " --export " + Quote(path) + " " +
            Quote(exported.string()) + " --codec h264 --software 2>/dev/null");
    FloatFrames shown;
    Check(report.find("\"ok\":true") != std::string::npos &&
              ReadFloatFrames(exported.string(), shown) && shown.Count() == 70,
          "surfaces: --export renders the motion clips: " + report);
    if (shown.Count() == 70) {
        // Frame 30 shows the title's white somewhere right of the box.
        double brightest = 0.0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 90; x < kWidth; ++x) {
                brightest = std::max(brightest, shown.At(30, x, y).green);
            }
        }
        Check(brightest > 0.9, "surfaces: the exported frame shows the title");
    }
}

// MOTION-2026-09 M4 -- what a frame costs: render and PNG encoding at
// 1080x1920, the transport the export uses, and the render alone, what the
// monitor pays.
void MeasureCost() {
    const std::string card =
        R"({"width":1080,"height":1920,"fps":25,"duration":60,)"
        R"("props":{"nom":{"type":"text","default":"Prénom Nom"}},)"
        R"("root":{"type":"fill","style":{"justifyContent":"flex-end",)"
        R"("padding":96},"children":[{"type":"box","style":{)"
        R"("backgroundColor":"#101010CC","padding":"24px 36px",)"
        R"("flexDirection":"column","gap":8},"animate":{"opacity":)"
        R"({"interpolate":{"frames":[0,8],"values":[0,1]}}},"children":[)"
        R"({"type":"text","content":{"prop":"nom"},"style":{)"
        R"("fontFamily":"Fira Sans Condensed","fontWeight":600,)"
        R"("fontSize":72,"color":"#FFFFFF"}},{"type":"text",)"
        R"("content":"Fonction","style":{"fontFamily":"Fira Sans Condensed",)"
        R"("fontWeight":500,"fontSize":48,"color":"#FFD23F"}}]}]}})";
    MotionComposition composition;
    MotionError error;
    if (!ParseMotionComposition(card, composition, error)) {
        Check(false, "cost card parses: " + error.message);
        return;
    }
    MotionFrameRequest request;
    request.composition = &composition;
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    std::vector<double> encoded;
    std::vector<double> raster;
    size_t bytes = 0;
    for (int pass = 0; pass < 25; ++pass) {
        request.frame = pass;
        MotionFrameImage image;
        auto started = std::chrono::steady_clock::now();
        if (!render(request, image, error)) {
            Check(false, "cost card renders: " + error.message);
            return;
        }
        encoded.push_back(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count());
        bytes += image.png.size();
        MotionCoreTextMeasurer measurer;
        MotionFrame frame;
        MotionRaster pixels;
        started = std::chrono::steady_clock::now();
        if (!EvaluateMotionFrame(composition, Value(), pass, measurer, frame,
                                 error) ||
            !RenderMotionFrame(frame, measurer, pixels, error)) {
            Check(false, "cost card rasterises: " + error.message);
            return;
        }
        raster.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count());
    }
    // The monitor's picture: the same evaluation, laid out for Renderer.mm.
    DocumentComposition stored{kComposition, "Carte", Json(card)};
    MotionPreviewRequest preview;
    preview.composition = &stored;
    std::vector<double> monitor;
    for (int pass = 0; pass < 25; ++pass) {
        preview.frame = pass;
        MotionPreviewImage image;
        const auto started = std::chrono::steady_clock::now();
        if (!RenderMotionPreview(preview, Document{}, std::string(), image,
                                 error)) {
            Check(false, "cost card renders for the monitor: " + error.message);
            return;
        }
        monitor.push_back(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count());
    }
    std::sort(monitor.begin(), monitor.end());
    std::cout << "cost 1080x1920: monitor picture (evaluate, render, planes) "
              << "median " << monitor[monitor.size() / 2] << " ms\n";
    std::sort(encoded.begin(), encoded.end());
    std::sort(raster.begin(), raster.end());
    std::cout << "cost 1080x1920: evaluate + render + PNG median "
              << encoded[encoded.size() / 2] << " ms ("
              << bytes / encoded.size() / 1024
              << " KiB a frame); evaluate + render median "
              << raster[raster.size() / 2] << " ms, 25 passes\n";
}

}  // namespace

// REMOTION-PARITY-2026-09 REM2 -- a motion clip whose composition plays a
// media of the project, picture and sound, exported and read back: the
// picture shown is the media's frame at the composition's instant, and the
// film's sound is the composition's mix placed where the clip starts. The
// lossless inspection render keeps the samples: PCM float, no codec.
void CheckMotionMediaExport(const std::filesystem::path& directory) {
    const auto voice = directory / "voice.mov";
    const std::string make =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=black:s=192x108:r=25:d=4,format=rgb24,"
        "geq=r=4*N:g=255-4*N:b=80' -f lavfi -i "
        "'aevalsrc=exprs=mod(n\\,1000)/1000|-mod(n\\,1000)/1000:s=48000:d=4' "
        "-c:v png -c:a pcm_f32le " +
        Quote(voice.string());
    if (std::system(make.c_str()) != 0) {
        Check(false, "the media with picture and sound is made");
        return;
    }
    const Ulid source = GenerateUlid();
    const Ulid player = "01K90000000000000000000030";
    Document document;
    document.sequence.id = GenerateUlid();
    document.sequence.name = "Lecteur";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.sources = {{source, voice.string(), {25, 1}, {100, 25}}};
    document.compositions = {
        {player, "Lecteur",
         Json(
             R"({"width":192,"height":108,"fps":25,"duration":60,)"
             R"("root":{"type":"fill","uid":"01K90000000000000000000031",)"
             R"("children":[{"type":"video","uid":"01K90000000000000000000032",)"
             R"("media":")" +
             source +
             R"(","style":{"width":"100%","height":"100%","volume":0.25}}]}})")}};
    DocumentClip clip;
    clip.composition_id = player;
    clip.source_in = {10, 25};
    clip.duration = {30, 25};
    clip.timeline_in = {20, 25};
    clip.include_audio = false;
    document.sequence.tracks = {{GenerateUlid(), "video", 0, {clip}}};
    std::string error;
    Check(document.Validate(error), "the player timeline is valid: " + error);
    const auto output = directory / "player.mkv";
    ExportSettings settings;
    settings.output_path = output.string();
    settings.encoder = ExportEncoder::LosslessInspection;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    const bool exported =
        Exporter::BuildPlan(document, (directory / "player.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(exported && plan.motion_assets.size() == 1 &&
              plan.motion_assets[0].audio &&
              plan.motion_assets[0].audio_first == 19200 &&
              plan.motion_assets[0].audio_count == 57600,
          "the player exports with the part of its sound the clip shows: " +
              error);
    FloatFrames shown;
    if (!exported || !ReadFloatFrames(output.string(), shown) ||
        shown.Count() != 50) {
        Check(false, "the player export decodes to 50 frames");
        return;
    }
    // Timeline frame 25 is clip frame 5, composition frame 15: media
    // picture 15, (60, 195, 80).
    const RgbColor expected{60 / 255.0, 195 / 255.0, 80 / 255.0};
    Check(Distance(shown.At(25, kWidth / 2, kHeight / 2), expected) <= 0.02,
          "the export shows the media picture of the composition's instant: " +
              Describe(shown.At(25, kWidth / 2, kHeight / 2)));
    std::shared_ptr<const MotionPcm> sound;
    std::string code;
    std::string message;
    if (!ReadMotionAudio(output.string(), sound, code, message) ||
        sound->left.size() < 96000 || sound->right.size() < 96000) {
        Check(false,
              "the export's sound decodes to the film's 96000 "
              "samples: " +
                  message);
        return;
    }
    // Sample n of the film is sample n - 38400 of the clip, which is sample
    // n - 38400 + 19200 of the composition and of the media. The clip ends
    // with the film, at sample 96000: nothing after it is read.
    double worst = 0.0;
    int64_t worstAt = -1;
    double worstGot = 0.0;
    double worstWant = 0.0;
    for (const int64_t n : {int64_t{1000}, int64_t{38000}, int64_t{40000},
                            int64_t{50001}, int64_t{70123}, int64_t{90000}}) {
        const bool inside = n >= 38400 + 144 && n < 96000 - 144;
        const bool outside = n < 38400 || n >= 96000;
        if (!inside && !outside) {
            continue;
        }
        const double want =
            inside ? 0.25 * static_cast<double>((n - 38400 + 19200) % 1000) /
                         1000.0
                   : 0.0;
        const double miss = std::max(std::abs(sound->left[n] - want),
                                     std::abs(sound->right[n] + want));
        if (miss > worst) {
            worst = miss;
            worstAt = n;
            worstGot = sound->left[n];
            worstWant = want;
        }
    }
    Check(sound->left.size() >= 96000 && worst <= 1e-5,
          "the film's sound is the composition's mix, placed where the clip "
          "starts: worst " +
              std::to_string(worst) + " at sample " + std::to_string(worstAt) +
              " (" + std::to_string(worstGot) + " for " +
              std::to_string(worstWant) + ") of " +
              std::to_string(sound->left.size()));
}

// REMOTION-PARITY-2026-09 REM6 -- a dress rehearsal of the closing montage,
// not the montage itself: the four capabilities the contract asks of it in
// one project, through the export and verify_export. V1 is a shot; on V2 a
// motion clip plays a series whose first scene shows the same media nested
// in a rounded box, under a title with a shadow and a glow, and fades into
// a scene where a lower third is instanced twice with different props.
// Every pixel of the delivered file is the engine's own frame for that
// instant over the shot, and each capability is looked at where it acts.
const Ulid kRehearsal = "01K90000000000000000000040";
const Ulid kLowerThird = "01K90000000000000000000060";

void CheckRehearsalMontage(const std::filesystem::path& directory) {
    // V1: a moving test pattern in grey, the textured picture verify_export
    // is made for, with no colour for the 4:2:0 delivery to smear. Nested: a
    // flat picture changing every frame, (2N, 255 - 2N, 80), whose instant can
    // be read back at any pixel.
    const auto shotPath = directory / "rehearsal-shot.mov";
    const auto nestedPath = directory / "rehearsal-nested.mov";
    const std::string makeShot =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'testsrc2=s=192x108:r=25:d=4,hue=s=0,format=rgb24' -c:v png " +
        Quote(shotPath.string());
    const std::string makeNested =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=black:s=192x108:r=25:d=4,format=rgb24,"
        "geq=r=2*N:g=255-2*N:b=80' -c:v png " +
        Quote(nestedPath.string());
    FloatFrames shot;
    FloatFrames nested;
    if (std::system(makeShot.c_str()) != 0 ||
        std::system(makeNested.c_str()) != 0 ||
        !ReadFloatFrames(shotPath.string(), shot) || shot.Count() != 100 ||
        !ReadFloatFrames(nestedPath.string(), nested) ||
        nested.Count() != 100) {
        Check(false, "rehearsal: the shot and the nested media are made");
        return;
    }
    const Ulid source = GenerateUlid();
    const Ulid media = GenerateUlid();
    const std::string lowerThird =
        R"({"width":192,"height":36,"fps":50,"duration":100,)"
        R"("props":{"name":{"type":"text","default":"Nom"},)"
        R"("color":{"type":"color","default":"#4DABA1"}},)"
        R"("root":{"type":"fill","uid":"01K90000000000000000000061",)"
        R"("style":{"justifyContent":"center","paddingLeft":8},)"
        R"("children":[{"type":"box","uid":"01K90000000000000000000062",)"
        R"("style":{"backgroundColor":{"prop":"color"},"width":120,)"
        R"("height":24,"paddingLeft":6,"justifyContent":"center"},)"
        R"("animate":{"opacity":{"interpolate":{"frames":[0,10],)"
        R"("values":[0,1],"uids":["01K90000000000000000000063",)"
        R"("01K90000000000000000000064"]}}},)"
        R"("children":[{"type":"text","uid":"01K90000000000000000000065",)"
        R"("content":{"prop":"name"},"style":{"fontFamily":)"
        R"("Fira Sans Condensed","fontSize":16,"color":"#ffffff"}}]}]}})";
    const std::string rehearsal =
        R"({"width":192,"height":108,"fps":25,"duration":50,)"
        R"("root":{"type":"series","uid":"01K90000000000000000000041",)"
        R"("children":[{"type":"sequence","uid":"01K90000000000000000000042",)"
        R"("duration":30,"style":{"backgroundColor":"#101820",)"
        R"("justifyContent":"center","alignItems":"center"},"children":[)"
        R"({"type":"box","uid":"01K90000000000000000000043","style":{)"
        R"("width":128,"height":72,"borderRadius":12,"overflow":"hidden"},)"
        R"("children":[{"type":"video","uid":"01K90000000000000000000044",)"
        R"("media":")" +
        media +
        R"(","startFrom":10,"muted":true,)"
        R"("style":{"width":"100%","height":"100%"}}]},)"
        R"({"type":"text","uid":"01K90000000000000000000045","content":"Plan",)"
        R"("style":{"position":"absolute","left":12,"top":6,)"
        R"("fontFamily":"Fira Sans Condensed","fontWeight":600,)"
        R"("fontSize":24,"color":"#ffffff","filter":)"
        R"J("drop-shadow(0px 3px 2px rgba(0,0,0,0.6)) glow(4px #FFD23F 1)"}}]},)J"
        R"({"type":"transition","uid":"01K90000000000000000000046",)"
        R"("presentation":"fade","timing":{"duration":10,"easing":"linear"}},)"
        R"({"type":"sequence","uid":"01K90000000000000000000047",)"
        R"("duration":30,"children":[)"
        R"({"type":"composition","uid":"01K90000000000000000000048",)"
        R"("composition":")" +
        kLowerThird +
        R"(","style":{"position":"absolute","left":0,"top":6,)"
        R"("width":192,"height":36}},)"
        R"({"type":"composition","uid":"01K90000000000000000000049",)"
        R"("composition":")" +
        kLowerThird +
        R"(","props":{"name":"Ana","color":"#E4572E"},)"
        R"("style":{"position":"absolute","left":0,"top":66,)"
        R"("width":192,"height":36}}]}]}})";

    Document document;
    document.sequence.id = GenerateUlid();
    document.sequence.name = "Répétition";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.sources = {{source, shotPath.string(), {25, 1}, {100, 25}},
                        {media, nestedPath.string(), {25, 1}, {100, 25}}};
    document.compositions = {{kLowerThird, "Bandeau", Json(lowerThird)},
                             {kRehearsal, "Habillage", Json(rehearsal)}};
    DocumentClip base;
    base.source_id = source;
    base.source_in = {0, 25};
    base.duration = {75, 25};
    base.timeline_in = {0, 25};
    base.include_audio = false;
    DocumentClip dressing;
    dressing.composition_id = kRehearsal;
    dressing.source_in = {0, 25};
    dressing.duration = {50, 25};
    dressing.timeline_in = {10, 25};
    dressing.include_audio = false;
    document.sequence.tracks = {{GenerateUlid(), "video", 0, {base}},
                                {GenerateUlid(), "video", 1, {dressing}}};
    std::string error;
    Check(document.Validate(error),
          "rehearsal: the montage is a valid document: " + error);

    const auto output = directory / "rehearsal.mkv";
    ExportSettings settings;
    settings.output_path = output.string();
    settings.encoder = ExportEncoder::LosslessInspection;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    const bool exported =
        Exporter::BuildPlan(document, (directory / "rehearsal.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    FloatFrames shown;
    std::vector<float> shownLuma;
    if (!exported || !ReadFloatFrames(output.string(), shown) ||
        shown.Count() != 75 || !ReadLumaFrames(output.string(), shownLuma) ||
        shownLuma.size() != static_cast<size_t>(75) * kWidth * kHeight) {
        Check(false, "rehearsal: the montage exports to 75 frames: " + error);
        return;
    }

    MotionReach reach;
    MotionError motionError;
    MotionComposition composition;
    if (!MotionReachFor(document, kRehearsal, reach, motionError) ||
        !ParseDocumentComposition(document.compositions[1], composition,
                                  motionError)) {
        Check(false, "rehearsal: the composition and what it reaches parse: " +
                         motionError.message);
        return;
    }
    const auto render = [&](int64_t frame, MotionFrame& evaluated,
                            MotionRaster& raster) {
        MotionCoreTextMeasurer measurer(directory.string());
        measurer.SetMediaPaths(reach.media_paths);
        const bool ok =
            EvaluateMotionFrame(composition, Value(), frame, measurer,
                                reach.Context(), evaluated, motionError) &&
            RenderMotionFrame(evaluated, measurer, raster, motionError);
        Check(ok, "rehearsal: composition frame " + std::to_string(frame) +
                      " renders: " + motionError.message);
        return ok;
    };
    const auto find = [](const MotionEvaluatedNode& node, const Ulid& uid,
                         const auto& self) -> const MotionEvaluatedNode* {
        if (node.uid == uid) {
            return &node;
        }
        for (const MotionEvaluatedNode& child : node.children) {
            if (const MotionEvaluatedNode* found = self(child, uid, self)) {
                return found;
            }
        }
        return nullptr;
    };
    const auto hex = [](int red, int green, int blue) {
        return RgbColor{red / 255.0, green / 255.0, blue / 255.0};
    };

    const auto luma = [](const RgbColor& color) {
        return (0.2126 * color.red) + (0.7152 * color.green) +
               (0.0722 * color.blue);
    };

    // Timeline frames: scene 1, the fade (composition frames 20 to 29),
    // scene 2 once the lower thirds are in, and the shot alone around the
    // clip. The delivery is 4:2:0: its luma is full resolution, its colour
    // is not. Every pixel's luma is compared on the file's Y plane, its
    // colour only where the expected picture is the same over the 9 x 9
    // pixels the encoder's chroma resampling reaches (a bicubic 2:1 down,
    // then up).
    for (const int64_t t : {int64_t{5}, int64_t{20}, int64_t{33}, int64_t{36},
                            int64_t{50}, int64_t{65}}) {
        const std::string at = "rehearsal frame " + std::to_string(t);
        const bool dressed = t >= 10 && t < 60;
        MotionFrame evaluated;
        MotionRaster reference;
        if (dressed && !render(t - 10, evaluated, reference)) {
            continue;
        }
        std::vector<RgbColor> expected(static_cast<size_t>(kWidth) * kHeight);
        std::vector<double> alpha(expected.size(), 0.0);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const Pixel motion = dressed ? At(reference, x, y) : Pixel{};
                const size_t index = (static_cast<size_t>(y) * kWidth) + x;
                expected[index] =
                    Mix(shot.At(t, x, y), motion.color, motion.alpha);
                alpha[index] = motion.alpha;
            }
        }
        double lumaMiss = 0.0;
        double colourMiss = 0.0;
        double edgeMiss = 0.0;
        size_t coloured = 0;
        std::string worstAt;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const size_t index = (static_cast<size_t>(y) * kWidth) + x;
                const RgbColor& want = expected[index];
                const RgbColor got = shown.At(t, x, y);
                const double miss = std::abs(
                    shownLuma[(static_cast<size_t>(t) * kWidth * kHeight) +
                              index] -
                    luma(want));
                if (alpha[index] > 0.0 && alpha[index] < 1.0) {
                    edgeMiss = std::max(edgeMiss, miss);
                    continue;
                }
                if (miss > lumaMiss) {
                    lumaMiss = miss;
                    worstAt = " at (" + std::to_string(x) + ", " +
                              std::to_string(y) + "): " + Describe(got) +
                              " for " + Describe(want) + ", Y " +
                              std::to_string(shownLuma[(static_cast<size_t>(t) *
                                                        kWidth * kHeight) +
                                                       index]) +
                              " for " + std::to_string(luma(want));
                }
                bool uniform = true;
                for (int dy = -4; uniform && dy <= 4; ++dy) {
                    for (int dx = -4; uniform && dx <= 4; ++dx) {
                        const int nx = std::clamp(x + dx, 0, kWidth - 1);
                        const int ny = std::clamp(y + dy, 0, kHeight - 1);
                        uniform =
                            Distance(
                                expected[(static_cast<size_t>(ny) * kWidth) +
                                         nx],
                                want) <= 0.004;
                    }
                }
                if (uniform) {
                    colourMiss = std::max(colourMiss, Distance(got, want));
                    ++coloured;
                }
            }
        }
        Check(
            lumaMiss <= 0.02 && colourMiss <= 0.02 && edgeMiss <= 0.06,
            at + ": the file is the engine's frame over the shot, worst luma " +
                std::to_string(lumaMiss) + worstAt + ", colour " +
                std::to_string(colourMiss) + " over " +
                std::to_string(coloured) + " pixels, edges " +
                std::to_string(edgeMiss));
        if (!dressed) {
            continue;
        }
        const int64_t local = t - 10;
        if (local < 20) {
            // Nested video at its exact instant -- the nearest of the
            // media's pictures around it, since neighbours differ by two
            // codes -- cropped by the rounded box; the title's effects
            // reach past its box.
            const int64_t picture = 10 + local;
            const RgbColor centre = shown.At(t, 96, 54);
            int64_t nearest = -1;
            double nearestMiss = 1.0;
            for (int64_t k = picture - 3; k <= picture + 3; ++k) {
                const double miss =
                    Distance(centre, nested.At(static_cast<size_t>(k), 96, 54));
                if (miss < nearestMiss) {
                    nearestMiss = miss;
                    nearest = k;
                }
            }
            const MotionEvaluatedNode* video =
                find(evaluated.root, "01K90000000000000000000044", find);
            const MotionEvaluatedNode* title =
                find(evaluated.root, "01K90000000000000000000045", find);
            const RgbColor corner = shown.At(t, 159, 89);
            Check(video != nullptr && video->media.frame == picture &&
                      nearest == picture &&
                      std::abs(luma(corner) - luma(hex(0x10, 0x18, 0x20))) <=
                          0.02,
                  at + ": the nested shot shows its picture " +
                      std::to_string(picture) + " (nearest " +
                      std::to_string(nearest) +
                      "), its rounded corner the scene: " + Describe(centre) +
                      " " + Describe(corner));
            Check(title != nullptr && title->filters.size() == 2 &&
                      title->has_filter_domain &&
                      title->filter_domain.width > title->box.width,
                  at + ": the title carries its shadow and glow");
        } else if (local < 30) {
            Check(evaluated.root.transition.active &&
                      std::abs(evaluated.root.transition.progress -
                               ((static_cast<double>(local) - 20.0) / 10.0)) <
                          1e-9,
                  at + ": the fade runs");
        } else {
            // Two instances of one precomposition, each with its props.
            const MotionEvaluatedNode* first =
                find(evaluated.root, "01K90000000000000000000048", find);
            const MotionEvaluatedNode* second =
                find(evaluated.root, "01K90000000000000000000049", find);
            Check(first != nullptr && second != nullptr &&
                      first->instance.composition_id == kLowerThird &&
                      Distance(shown.At(t, 110, 24), hex(0x4D, 0xAB, 0xA1)) <=
                          0.02 &&
                      Distance(shown.At(t, 110, 84), hex(0xE4, 0x57, 0x2E)) <=
                          0.02,
                  at + ": the lower third twice, with its own colours: " +
                      Describe(shown.At(t, 110, 24)) + " " +
                      Describe(shown.At(t, 110, 84)));
        }
    }

    // verify_export on the delivered file: the shot's frames are checked,
    // the dressed ones counted unverifiable, nothing divergent.
    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error) &&
              report.exported_frames == 75 && report.checked_frames == 25 &&
              report.unverifiable_frames == 50 && report.divergent_frames == 0,
          "rehearsal: verify_export passes (" +
              std::to_string(report.checked_frames) + " checked, " +
              std::to_string(report.unverifiable_frames) + " unverifiable, " +
              std::to_string(report.divergent_frames) +
              " divergent): " + error);
    for (const ExportDivergence& divergence : report.divergences) {
        std::cout << "rehearsal divergence: " << divergence.kind << " "
                  << divergence.first_frame << "-" << divergence.last_frame
                  << " offset " << divergence.offset << " expected "
                  << divergence.expected_source_frame << '\n';
    }
    std::cout << "rehearsal: " << report.isolated_frames.size()
              << " isolated frame(s) reported in the file\n";
}

// REMOTION-PARITY-2026-09 REM6 -- verify_export on a film made of motion
// alone. V1 holds two motion clips and nothing else: a sweep (a box crossing
// the frame 2.5 px a frame over a grey ground, so that a frame shown at the
// wrong instant cannot pass) from its frame 5, then a title fading in over
// black. Every frame is checked against the engine's render; a document that
// asks for other frames, or another composition, is told where the file
// differs; a process without the renderer counts the frames unverifiable.
const Ulid kSweep = "01K90000000000000000000080";
const Ulid kEnding = "01K90000000000000000000090";

std::string SerializedReport(const ExportVerificationReport& report) {
    return SerializeExportVerification(report);
}

void CheckFullMotionVerification(const std::filesystem::path& directory) {
    const std::string sweep =
        R"({"width":192,"height":108,"fps":25,"duration":60,)"
        R"("root":{"type":"fill","uid":"01K90000000000000000000081",)"
        R"("style":{"backgroundColor":"#303840"},"children":[)"
        R"({"type":"box","uid":"01K90000000000000000000082","style":{)"
        R"("position":"absolute","left":4,"top":42,"width":24,"height":24,)"
        R"("backgroundColor":"#FFFFFF"},"animate":{"translateX":{)"
        R"("interpolate":{"frames":[0,59],"values":[0,150],)"
        R"("uids":["01K90000000000000000000083",)"
        R"("01K90000000000000000000084"]}}}},)"
        R"({"type":"text","uid":"01K90000000000000000000085",)"
        R"("content":"CUTMACHINE","style":{"position":"absolute","left":10,)"
        R"("top":8,"fontFamily":"Fira Sans Condensed","fontWeight":600,)"
        R"("fontSize":20,"color":"#FFFFFF"}}]}})";
    const std::string ending =
        R"({"width":192,"height":108,"fps":25,"duration":40,)"
        R"("root":{"type":"fill","uid":"01K90000000000000000000091",)"
        R"("style":{"justifyContent":"center","alignItems":"center"},)"
        R"("children":[{"type":"text","uid":"01K90000000000000000000092",)"
        R"("content":"Fin","style":{"fontFamily":"Fira Sans Condensed",)"
        R"("fontWeight":600,"fontSize":48,"color":"#FFFFFF"},)"
        R"("animate":{"opacity":{"interpolate":{"frames":[0,10],)"
        R"("values":[0,1],"uids":["01K90000000000000000000093",)"
        R"("01K90000000000000000000094"]}}}}]}})";
    Document document;
    document.sequence.id = GenerateUlid();
    document.sequence.name = "Teaser";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.compositions = {{kSweep, "Balayage", Json(sweep)},
                             {kEnding, "Fin", Json(ending)}};
    DocumentClip first;
    first.composition_id = kSweep;
    first.source_in = {5, 25};
    first.duration = {45, 25};
    first.timeline_in = {0, 25};
    first.include_audio = false;
    DocumentClip last;
    last.composition_id = kEnding;
    last.source_in = {0, 25};
    last.duration = {30, 25};
    last.timeline_in = {45, 25};
    last.include_audio = false;
    document.sequence.tracks = {{GenerateUlid(), "video", 0, {first, last}}};
    std::string error;
    Check(document.Validate(error),
          "full motion: the teaser is a valid document: " + error);
    const auto output = directory / "teaser.mkv";
    ExportSettings settings;
    settings.output_path = output.string();
    settings.encoder = ExportEncoder::LosslessInspection;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    if (!Exporter::BuildPlan(document, (directory / "teaser.json").string(),
                             settings, plan, error) ||
        !Exporter::Run(plan, {}, nullptr, error)) {
        Check(false, "full motion: the teaser exports: " + error);
        return;
    }
    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error) &&
              report.exported_frames == 75 && report.checked_frames == 75 &&
              report.motion_checked_frames == 75 &&
              report.unverifiable_frames == 0 && report.divergent_frames == 0,
          "full motion: every frame is checked against the engine's render, "
          "none diverges: " +
              error + " " + SerializedReport(report));

    // The document asks for the sweep two frames later than the file shows.
    Document later = document;
    later.sequence.tracks[0].clips[0].source_in = {7, 25};
    Check(VerifyExport(later, directory, output.string(), verification, report,
                       error),
          "full motion: verification runs on a diverging document: " + error);
    const std::string serialized = SerializedReport(report);
    const bool shifted = std::any_of(
        report.divergences.begin(), report.divergences.end(),
        [&](const ExportDivergence& divergence) {
            return divergence.kind == "offset" && divergence.offset == -2 &&
                   divergence.motion && divergence.source_id == kSweep &&
                   divergence.first_frame <= 5 && divergence.last_frame >= 40;
        });
    Check(
        shifted &&
            serialized.find(R"("composition_id":")" + kSweep) !=
                std::string::npos &&
            serialized.find("expected_composition_frame") != std::string::npos,
        "full motion: a sweep shown two frames early is reported as an "
        "offset of -2 on its composition: " +
            serialized);

    // The document asks for the sweep where the file shows the title.
    Document swapped = document;
    swapped.sequence.tracks[0].clips[1].composition_id = kSweep;
    Check(VerifyExport(swapped, directory, output.string(), verification,
                       report, error) &&
              std::any_of(report.divergences.begin(), report.divergences.end(),
                          [](const ExportDivergence& divergence) {
                              return divergence.kind == "wrong_picture" &&
                                     divergence.motion &&
                                     divergence.first_frame >= 45;
                          }),
          "full motion: another composition than the file shows is a wrong "
          "picture: " +
              error + " " + SerializedReport(report));

    // Without the renderer, nothing is compared and nothing is claimed.
    const MotionFrameRenderer renderer = CurrentMotionFrameRenderer();
    SetMotionFrameRenderer(nullptr);
    const bool blind = VerifyExport(document, directory, output.string(),
                                    verification, report, error);
    SetMotionFrameRenderer(renderer);
    Check(
        blind && report.checked_frames == 0 && report.unverifiable_frames == 75,
        "full motion: a process without the renderer counts the motion "
        "frames unverifiable: " +
            error + " " + SerializedReport(report));

    // Flat pictures (ITW-2026-09, « Constats »): a grey that changes by
    // about one code of luma a frame, exported exactly, is not reported
    // shifted -- its neighbours are closer than the level the delivery
    // moves; one that changes by six codes a frame still is, when the
    // document asks for other frames.
    const auto flatCase = [&](const std::string& name, int step,
                              int64_t askedIn, bool expectShift) {
        const auto path = directory / (name + ".mov");
        const std::string make =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -y -f lavfi -i "
            "'color=c=black:s=192x108:r=25:d=3,format=gray,geq=lum=64+" +
            std::to_string(step) + "*N/4' -c:v png " + Quote(path.string());
        if (std::system(make.c_str()) != 0) {
            Check(false, "full motion: the flat grey " + name + " is made");
            return;
        }
        Document flat;
        flat.sequence.id = GenerateUlid();
        flat.sequence.name = name;
        flat.sequence.width = kWidth;
        flat.sequence.height = kHeight;
        flat.sequence.frame_rate = {25, 1};
        flat.sources = {{GenerateUlid(), path.string(), {25, 1}, {75, 25}}};
        DocumentClip shot;
        shot.source_id = flat.sources[0].id;
        shot.source_in = {0, 25};
        shot.duration = {60, 25};
        shot.timeline_in = {0, 25};
        shot.include_audio = false;
        flat.sequence.tracks = {{GenerateUlid(), "video", 0, {shot}}};
        const auto flatOutput = directory / (name + ".mkv");
        ExportSettings flatSettings = settings;
        flatSettings.output_path = flatOutput.string();
        ExportPlan flatPlan;
        if (!Exporter::BuildPlan(flat, (directory / (name + ".json")).string(),
                                 flatSettings, flatPlan, error) ||
            !Exporter::Run(flatPlan, {}, nullptr, error)) {
            Check(false, "full motion: " + name + " exports: " + error);
            return;
        }
        ExportVerificationReport exact;
        Check(VerifyExport(flat, directory, flatOutput.string(), verification,
                           exact, error) &&
                  exact.checked_frames == 60 && exact.divergent_frames == 0,
              "full motion: " + name +
                  ", exported exactly, is not reported shifted: " + error +
                  " " + SerializedReport(exact));
        Document asked = flat;
        asked.sequence.tracks[0].clips[0].source_in = {askedIn, 25};
        ExportVerificationReport moved;
        const bool ran = VerifyExport(asked, directory, flatOutput.string(),
                                      verification, moved, error);
        const bool reported =
            std::any_of(moved.divergences.begin(), moved.divergences.end(),
                        [&](const ExportDivergence& divergence) {
                            return divergence.kind == "offset" &&
                                   divergence.offset == -askedIn;
                        });
        Check(ran && reported == expectShift,
              "full motion: " + name + " asked " + std::to_string(askedIn) +
                  " frames later is " +
                  (expectShift ? "reported" : "left ambiguous") + ": " + error +
                  " " + SerializedReport(moved));
    };
    // lum = 64 + step * N / 4: a quarter of `step` codes a frame.
    flatCase("flat-slow", 4, 2, false);
    flatCase("flat-fast", 24, 2, true);

    // A texture that changes every other frame, fading to black through
    // H.264, as the teaser's dust and ink voids did: the frames cross the
    // flat line one after the other. The expected frame, still textured,
    // and its flat neighbour are judged on one scale, so the neighbour's
    // near level does not win against a structure that 8-bit rounding
    // already blurs (MOTION-DESIGN-2026-09).
    const auto fading = directory / "fading.mov";
    const std::string makeFading =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=black:s=192x108:r=25:d=3,format=gray,geq=lum="
        "(1-N/64)*(64+12*sin(X/5+7*floor(N/2))*sin(Y/4+3*floor(N/2)))' "
        "-c:v png " +
        Quote(fading.string());
    if (std::system(makeFading.c_str()) != 0) {
        Check(false, "full motion: the fading texture is made");
        return;
    }
    Document fade;
    fade.sequence.id = GenerateUlid();
    fade.sequence.name = "fading";
    fade.sequence.width = kWidth;
    fade.sequence.height = kHeight;
    fade.sequence.frame_rate = {25, 1};
    fade.sources = {{GenerateUlid(), fading.string(), {25, 1}, {75, 25}}};
    DocumentClip faded;
    faded.source_id = fade.sources[0].id;
    faded.source_in = {0, 25};
    faded.duration = {60, 25};
    faded.timeline_in = {0, 25};
    faded.include_audio = false;
    fade.sequence.tracks = {{GenerateUlid(), "video", 0, {faded}}};
    ExportSettings fadeSettings = settings;
    fadeSettings.encoder = ExportEncoder::H264Software;
    fadeSettings.main10 = false;
    fadeSettings.output_path = (directory / "fading.mp4").string();
    ExportPlan fadePlan;
    if (!Exporter::BuildPlan(fade, (directory / "fading.json").string(),
                             fadeSettings, fadePlan, error) ||
        !Exporter::Run(fadePlan, {}, nullptr, error)) {
        Check(false, "full motion: the fading texture exports: " + error);
        return;
    }
    ExportVerificationReport faint;
    Check(VerifyExport(fade, directory, fadeSettings.output_path, verification,
                       faint, error) &&
              faint.checked_frames == 60 && faint.divergent_frames == 0,
          "full motion: a texture fading through the flat line, exported "
          "exactly, is not reported shifted: " +
              error + " " + SerializedReport(faint));
}

int main() {
    InstallMotionFrameRenderer();
    MotionError error;
    if (!RegisterMotionFontDirectory(MOTION_FONT_DIR, error)) {
        std::cerr << "FAIL: fonts: " << error.message << '\n';
        return 1;
    }
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-motion-clip-export");
    std::filesystem::create_directories(directory);
    try {
        // Tolerances: x265 at CRF 18, 10-bit 4:2:0; with colour management,
        // the 65^3 cubes too. 0.02 is five 8-bit codes; a lost or doubled
        // alpha misses an edge pixel by a quarter of full scale or more.
        CheckMotionOverInterview(
            directory, {"unmanaged", false, DisplayComposite, 0.02, 0.06});
        CheckMotionOverInterview(
            directory, {"managed", true, DisplayReferredComposite, 0.02, 0.06});
        CheckMotionClipOpacityFade(
            directory, {"unmanaged", false, DisplayComposite, 0.02, 0.06});
        CheckMotionClipOpacityFade(
            directory, {"managed", true, DisplayReferredComposite, 0.02, 0.06});
        CheckDisplayReferredRoundTrip();
        CheckDisplayReferredComposite();
        CheckDisplayReferredHlg();
        CheckMonitorPicture();
        CheckCollectedMotionImage(directory);
        CheckCollectedLogoTemplate(directory);
        CheckSurfaces(directory);
        CheckMotionMediaExport(directory);
        CheckRehearsalMontage(directory);
        CheckFullMotionVerification(directory);
        MeasureCost();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << exception.what() << '\n';
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: motion clip export\n";
    return 0;
}
