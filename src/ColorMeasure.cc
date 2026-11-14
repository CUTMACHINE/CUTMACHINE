#include "ColorMeasure.h"

#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "ColorEffects.h"
#include "ColorMatch.h"
#include "Export.h"
#include "FaceFinder.h"
#include "FfmpegBinary.h"
#include "FfmpegPipe.h"
#include "FrameCapture.h"
#include "Ingest.h"
#include "LookLutStorage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>

namespace {

struct LumaWeights {
    float red;
    float green;
    float blue;
};

// The display signal's own matrix: BT.709 unless the output is Rec.2020, as
// shader.metal's vectorscope weighs it.
constexpr LumaWeights kRec709Luma{0.2126f, 0.7152f, 0.0722f};
constexpr LumaWeights kRec2020Luma{0.2627f, 0.6780f, 0.0593f};
// The Y row of ACEScg's RGB-to-XYZ matrix (AP1 primaries, ACES white).
constexpr LumaWeights kAp1Luminance{0.2722287168f, 0.6740817658f,
                                    0.0536895174f};

// 18 % grey, the reference of a stop reading.
constexpr double kMiddleGrey = 0.18;

// Half a 10-bit code from either end: a channel there is on its end code.
constexpr float kEndCode = 0.5f / 1023.0f;

// Below half an 8-bit chroma code the mean colour is neutral to within what
// an 8-bit delivery can say, and its angle is noise.
constexpr double kMinimumHueChroma = 0.5;

LumaWeights DisplayWeights(const ColorManagementSettings& color) {
    return color.enabled && color.output_gamut == "rec2020" ? kRec2020Luma
                                                            : kRec709Luma;
}

LumaWeights SceneWeights(const ColorManagementSettings& color) {
    if (color.working_gamut == "rec709") return kRec709Luma;
    if (color.working_gamut == "rec2020") return kRec2020Luma;
    return kAp1Luminance;
}

float Weigh(const LumaWeights& weights, const std::array<float, 3>& color) {
    return weights.red * color[0] + weights.green * color[1] +
           weights.blue * color[2];
}

int64_t Scaled(double value) {
    return std::llround(value * static_cast<double>(kColorMeasureScale));
}

// One measured pixel, reduced to what the statistics read.
struct Sample {
    std::array<float, 3> display;
    float luma;  // Display luma, 0..1.
    float cb;
    float cr;
    float scene;  // Working-space luminance; managed only.
};

// One source colour through the clip's chain: the display signal, and under
// colour management the working-space luminance before the look. Shared by
// the measure and by the graded frame (G1b), so what an agent reads and
// what it sees are the same picture.
bool GradePixel(const ColorMeasureChain& chain, const LumaWeights& scene,
                const std::array<float, 3>& signal,
                std::array<float, 3>& display, float& sceneLuminance,
                std::string& error) {
    // Both renderers read the source through a LUT whose lookup clamps its
    // coordinates to 0..1: the monitor's input texture, the export's lut3d.
    std::array<float, 3> color = signal;
    for (float& channel : color) channel = std::clamp(channel, 0.0f, 1.0f);
    sceneLuminance = 0.0f;
    if (chain.color.enabled) {
        // The managed chain, as Export.cc lays it for one clip: the working
        // cube (input and grade), then the shaper, the look on the working
        // log values, the display cube, and the look on the delivered
        // signal.
        const std::array<float, 3> working =
            OpenColorIoPathWorking(chain.path, chain.grade, color);
        if (!std::isfinite(working[0]) || !std::isfinite(working[1]) ||
            !std::isfinite(working[2])) {
            error = "graded colour path produces a non-finite colour";
            return false;
        }
        sceneLuminance = Weigh(scene, working);
        color = OpenColorIoPathCoordinates(chain.path, working);
        if (chain.look_stage == LookLutStage::WorkingLog)
            color = EvaluateLookLut(*chain.look, color);
        color = OpenColorIoPathDisplay(chain.path, color);
        if (chain.look_stage == LookLutStage::DeliveredDisplay)
            color = EvaluateLookLut(*chain.look, color);
    } else {
        // Without colour management the grade acts on the display signal
        // and its cube clamps the result (BuildColorGradeCube); the look
        // goes on the composite after it.
        if (chain.grade.count > 0) {
            color = ApplyResolvedColorGrade(color, chain.grade);
            if (!std::isfinite(color[0]) || !std::isfinite(color[1]) ||
                !std::isfinite(color[2])) {
                error = "clip grade produces a non-finite colour";
                return false;
            }
            for (float& channel : color)
                channel = std::clamp(channel, 0.0f, 1.0f);
        }
        if (chain.look_stage == LookLutStage::Display)
            color = EvaluateLookLut(*chain.look, color);
    }
    display = color;
    return true;
}

bool ReadPixel(const ColorMeasureChain& chain, const LumaWeights& display,
               const LumaWeights& scene, const std::array<float, 3>& signal,
               Sample& sample, std::string& error) {
    if (!GradePixel(chain, scene, signal, sample.display, sample.scene, error))
        return false;
    const std::array<float, 3>& color = sample.display;
    sample.luma = Weigh(display, color);
    sample.cb = (color[2] - sample.luma) / (2.0f * (1.0f - display.blue));
    sample.cr = (color[0] - sample.luma) / (2.0f * (1.0f - display.red));
    return true;
}

// Nearest rank: the smallest value with at least `percent` % of the values
// at or below it. Deterministic, and never between two measured values.
float Percentile(const std::vector<float>& sorted, int64_t percent) {
    const size_t count = sorted.size();
    size_t rank = static_cast<size_t>(
        (static_cast<uint64_t>(percent) * count + 99) / 100);
    rank = std::clamp<size_t>(rank, 1, count);
    return sorted[rank - 1];
}

std::optional<int64_t> Stops(float luminance) {
    if (!(luminance > 0.0f)) return std::nullopt;
    return Scaled(std::log2(static_cast<double>(luminance) / kMiddleGrey));
}

void Summarize(const std::vector<Sample>& samples, bool scene,
               ColorPictureMeasure& out) {
    out = ColorPictureMeasure{};
    out.pixels = static_cast<int64_t>(samples.size());
    if (samples.empty()) return;
    const double count = static_cast<double>(samples.size());

    std::vector<float> luma;
    std::vector<float> saturation;
    luma.reserve(samples.size());
    saturation.reserve(samples.size());
    int64_t clipped = 0;
    int64_t crushed = 0;
    double cbSum = 0.0;
    double crSum = 0.0;
    std::array<std::array<double, 3>, 3> zoneSums{};
    for (const Sample& sample : samples) {
        luma.push_back(sample.luma);
        saturation.push_back(224.0f * std::hypot(sample.cb, sample.cr));
        const float top =
            std::max({sample.display[0], sample.display[1], sample.display[2]});
        if (top >= 1.0f - kEndCode) ++clipped;
        if (top <= kEndCode) ++crushed;
        cbSum += sample.cb;
        crSum += sample.cr;
        const float percent = sample.luma * 100.0f;
        const size_t zone =
            percent < static_cast<float>(kColorShadowsBelowPercent)      ? 0
            : percent >= static_cast<float>(kColorHighlightsFromPercent) ? 2
                                                                         : 1;
        ++out.zones[zone].pixels;
        for (int channel = 0; channel < 3; ++channel)
            zoneSums[zone][channel] += sample.display[channel];
    }
    std::sort(luma.begin(), luma.end());
    std::sort(saturation.begin(), saturation.end());
    out.luma_p02 = Scaled(100.0 * Percentile(luma, 2));
    out.luma_p50 = Scaled(100.0 * Percentile(luma, 50));
    out.luma_p98 = Scaled(100.0 * Percentile(luma, 98));
    out.clipped = Scaled(100.0 * static_cast<double>(clipped) / count);
    out.crushed = Scaled(100.0 * static_cast<double>(crushed) / count);
    double saturationSum = 0.0;
    for (float value : saturation) saturationSum += value;
    out.saturation_mean = Scaled(saturationSum / count);
    out.saturation_p95 = Scaled(Percentile(saturation, 95));

    const double meanCb = cbSum / count;
    const double meanCr = crSum / count;
    if (224.0 * std::hypot(meanCb, meanCr) >= kMinimumHueChroma) {
        double degrees = std::atan2(meanCr, meanCb) * 180.0 / M_PI;
        if (degrees < 0.0) degrees += 360.0;
        int64_t hue = Scaled(degrees);
        if (hue >= 360 * kColorMeasureScale) hue -= 360 * kColorMeasureScale;
        int64_t offset = hue - kSkinLineDegrees * kColorMeasureScale;
        if (offset > 180 * kColorMeasureScale)
            offset -= 360 * kColorMeasureScale;
        if (offset <= -180 * kColorMeasureScale)
            offset += 360 * kColorMeasureScale;
        out.hue = hue;
        out.skin_line_offset = offset;
    }

    for (size_t zone = 0; zone < 3; ++zone) {
        ColorZoneMeasure& measure = out.zones[zone];
        if (measure.pixels == 0) continue;
        const double pixels = static_cast<double>(measure.pixels);
        measure.red = Scaled(100.0 * zoneSums[zone][0] / pixels);
        measure.green = Scaled(100.0 * zoneSums[zone][1] / pixels);
        measure.blue = Scaled(100.0 * zoneSums[zone][2] / pixels);
        // From the rounded means, so the reply adds up as it reads.
        measure.warmth = measure.red - measure.blue;
        measure.green_magenta =
            std::llround(static_cast<double>(2 * measure.green - measure.red -
                                             measure.blue) /
                         2.0);
    }

    out.scene = scene;
    if (scene) {
        std::vector<float> luminance;
        luminance.reserve(samples.size());
        for (const Sample& sample : samples) luminance.push_back(sample.scene);
        std::sort(luminance.begin(), luminance.end());
        out.scene_p02 = Stops(Percentile(luminance, 2));
        out.scene_p50 = Stops(Percentile(luminance, 50));
        out.scene_p98 = Stops(Percentile(luminance, 98));
    }
}

// Whether the centre of pixel `index` of `size` lies in [start, start +
// extent), both fractions of `size`: start <= (2i + 1) / 2n < start +
// extent, compared exactly.
bool CentreInside(int64_t index, int64_t size, const EffectParamValue& start,
                  const EffectParamValue& extent) {
    const __int128 centre = 2 * static_cast<__int128>(index) + 1;
    const __int128 twice = 2 * static_cast<__int128>(size);
    if (centre * start.den < twice * start.num) return false;
    const __int128 endNum = static_cast<__int128>(start.num) * extent.den +
                            static_cast<__int128>(extent.num) * start.den;
    const __int128 endDen = static_cast<__int128>(start.den) * extent.den;
    return centre * endDen < twice * endNum;
}

const char* LookStageName(LookLutStage stage) {
    switch (stage) {
        case LookLutStage::None:
            return "none";
        case LookLutStage::Display:
            return "display";
        case LookLutStage::WorkingLog:
            return "working_log";
        case LookLutStage::DeliveredDisplay:
            return "delivered_display";
    }
    return "none";
}

const DocumentTrack* TrackOf(const Document& document, const Ulid& clipId) {
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

}  // namespace

bool ValidateColorMeasureRegion(const ColorMeasureRegion& region,
                                std::string& error) {
    if (!region.present) return true;
    const std::array<std::pair<const char*, const EffectParamValue*>, 4> parts =
        {{{"x", &region.x},
          {"y", &region.y},
          {"width", &region.width},
          {"height", &region.height}}};
    for (const auto& [name, value] : parts) {
        if (value->den <= 0) {
            error = std::string("region.") + name +
                    " must have a positive denominator";
            return false;
        }
    }
    if (region.x.num < 0 || region.y.num < 0) {
        error = "the region must start inside the picture";
        return false;
    }
    if (region.width.num <= 0 || region.height.num <= 0) {
        error = "the region must have a positive width and height";
        return false;
    }
    // start + extent <= 1, exactly.
    const auto fits = [](const EffectParamValue& start,
                         const EffectParamValue& extent) {
        return static_cast<__int128>(start.num) * extent.den +
                   static_cast<__int128>(extent.num) * start.den <=
               static_cast<__int128>(start.den) * extent.den;
    };
    if (!fits(region.x, region.width) || !fits(region.y, region.height)) {
        error = "the region must end inside the picture";
        return false;
    }
    return true;
}

bool MeasureColorSignal(const std::vector<std::array<float, 3>>& signal,
                        int32_t width, int32_t height,
                        const ColorMeasureChain& chain,
                        const ColorMeasureRegion& region,
                        ColorPictureMeasure& picture,
                        ColorPictureMeasure& inRegion, std::string& error) {
    error.clear();
    picture = ColorPictureMeasure{};
    inRegion = ColorPictureMeasure{};
    if (width <= 0 || height <= 0 ||
        signal.size() != static_cast<size_t>(width) * height) {
        error = "the measured picture does not match its size";
        return false;
    }
    if (!ValidateColorMeasureRegion(region, error)) return false;
    if (chain.color.enabled && !chain.path.state) {
        error = "the colour path was not built";
        return false;
    }
    if (chain.look_stage != LookLutStage::None && !chain.look) {
        error = "the sequence look was not loaded";
        return false;
    }
    const LumaWeights display = DisplayWeights(chain.color);
    const LumaWeights scene = SceneWeights(chain.color);
    std::vector<Sample> samples(signal.size());
    std::vector<Sample> regionSamples;
    std::vector<bool> columns;
    if (region.present) {
        columns.resize(static_cast<size_t>(width));
        for (int32_t column = 0; column < width; ++column)
            columns[column] =
                CentreInside(column, width, region.x, region.width);
    }
    for (int32_t row = 0; row < height; ++row) {
        const bool rowInside =
            region.present &&
            CentreInside(row, height, region.y, region.height);
        for (int32_t column = 0; column < width; ++column) {
            const size_t index = static_cast<size_t>(row) * width + column;
            if (!ReadPixel(chain, display, scene, signal[index], samples[index],
                           error))
                return false;
            if (rowInside && columns[column])
                regionSamples.push_back(samples[index]);
        }
    }
    if (region.present && regionSamples.empty()) {
        error = "the region holds no measured pixel; widen it";
        return false;
    }
    Summarize(samples, chain.color.enabled, picture);
    if (region.present) Summarize(regionSamples, chain.color.enabled, inRegion);
    return true;
}

namespace {

// What every clip of a call shares: the colour path, in `view` -- the
// document's management, or its SDR preview for a picture shown to a model
// -- and the sequence look, whose stage the document decides.
bool BuildSharedChain(const Document& document,
                      const ColorManagementSettings& view,
                      const std::string& projectDirectory,
                      ColorMeasureChain& chain, std::string& error) {
    chain = ColorMeasureChain{};
    chain.color = view;
    if (view.enabled && !MakeOpenColorIoColorPath(view, chain.path, error)) {
        error = "unable to build the colour path: " + error;
        return false;
    }
    LoadedLookLut look;
    if (!LoadSequenceLookLut(document.sequence, document.color_management,
                             projectDirectory, look, error))
        return false;
    chain.look_stage = look.stage;
    if (look.stage != LookLutStage::None)
        chain.look =
            std::make_shared<const LookLutSamples>(std::move(look.samples));
    return true;
}

// G3 -- docs/roadmap/GRADE-2026-09.md. The shared chain read with one
// clip's media input: a colour path of its own when the media declares an
// input, the shared one otherwise. The look and the view stay the call's.
bool ClipChain(const Document& document, const ColorMeasureChain& base,
               const DocumentClip& clip, ColorMeasureChain& chain,
               std::string& error) {
    chain = base;
    const LibraryMedia* media = document.FindLibraryMedia(clip.source_id);
    // DEC31 -- a graphic is read as its code values, with its own matrix
    // and range (WithMediaColorInput): no colour path to build.
    if (media == nullptr || (!DeclaresColorInput(media->color_input) &&
                             !media->color_input.graphics))
        return true;
    chain.color = WithMediaColorInput(base.color, media->color_input);
    if (chain.color.enabled &&
        OpenColorIoCacheKey(chain.color) != OpenColorIoCacheKey(base.color) &&
        !MakeOpenColorIoColorPath(chain.color, chain.path, error)) {
        error = "clip '" + clip.id + "': unable to build its media's colour " +
                "path: " + error;
        return false;
    }
    return true;
}

// A clip whose picture the sequence shows and a grade acts on. Refused by
// name otherwise.
bool CheckGradedClip(const Document& document, const DocumentClip& clip,
                     std::string& error) {
    const std::string named = "clip '" + clip.id + "': ";
    if (IsMotionClip(clip)) {
        error = named +
                "a motion clip is drawn by the engine and carries no grade "
                "to measure";
        return false;
    }
    // DEC31, BH2 -- under colour management a graphic is placed as
    // authored, and the export refuses a grade on it.
    if (document.color_management.enabled &&
        document.IsGraphicsMedia(clip.source_id)) {
        error = named +
                "its media is a graphic: under colour management it is "
                "placed as authored and carries no grade to measure";
        return false;
    }
    const DocumentTrack* track = TrackOf(document, clip.id);
    if (track == nullptr || track->kind != "video") {
        error = named + "only a clip of a video track is seen";
        return false;
    }
    if (document.FindSource(clip.source_id) == nullptr) {
        error = named + "its source is missing from the document";
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(clip.source_id);
    if (media == nullptr) {
        error = named + "its media was never probed";
        return false;
    }
    if (media->metadata_complete && !media->has_video) {
        error = named + "its media is audio-only";
        return false;
    }
    return true;
}

enum class ClipPictureSize {
    // Reduced to the bound, never enlarged: an enlargement adds no pixel to
    // measure, only the ringing of the filter.
    Measured,
    // The size read_frame composes at: a framed clip's rectangle as it is
    // drawn in the sequence frame reduced to the bound, or the whole
    // picture bounded.
    Shown,
    // DEC29 -- the size a sheet's cell composes at: a framed clip as Shown,
    // any other fitted inside the sequence frame reduced to the bound, as
    // the monitor fits a clip that frames nothing.
    Fitted,
};

struct ClipPicture {
    // The framing as ApplyClipFramingToCapture resolves it, for a caller
    // that composes the picture into the sequence frame.
    FrameCaptureSettings framing;
    int64_t width = 0;
    int64_t height = 0;
    std::vector<std::array<float, 3>> signal;
};

// The frame of `clip` at source time `time`, of what the sequence shows of
// its source -- the framed rectangle, or the whole displayed picture --
// decoded as the monitor decodes it.
bool DecodeClipPicture(const Document& document,
                       const std::string& projectDirectory,
                       const DocumentClip& clip, const RationalTime& time,
                       ClipPictureSize size, int32_t bound,
                       const ColorManagementSettings& color,
                       const std::string& ffmpegPath, ClipPicture& picture,
                       std::string& error) {
    const std::string named = "clip '" + clip.id + "': ";
    const DocumentSource* source = document.FindSource(clip.source_id);
    const LibraryMedia* media = document.FindLibraryMedia(clip.source_id);
    picture = ClipPicture{};
    picture.framing.max_dimension = bound;
    if (!ApplyClipFramingToCapture(document, clip.id, time, picture.framing,
                                   error))
        return false;
    int64_t shownWidth = 0;
    int64_t shownHeight = 0;
    std::string filter;
    if (picture.framing.framed) {
        const ClipFramingGeometry& geometry = picture.framing.framing;
        filter = ClipFramingCropFilter(geometry, picture.framing.source_width,
                                       picture.framing.source_height);
        if (!filter.empty()) filter += ',';
        shownWidth = geometry.source_width;
        shownHeight = geometry.source_height;
    } else if (!ClipDisplayedSize(document, clip, shownWidth, shownHeight,
                                  error)) {
        error = named + error;
        return false;
    }
    if (size != ClipPictureSize::Measured && picture.framing.framed) {
        picture.width = picture.framing.framing.width;
        picture.height = picture.framing.framing.height;
    } else if (size == ClipPictureSize::Fitted) {
        int64_t frameWidth = 0;
        int64_t frameHeight = 0;
        BoundedFrameSize(document.sequence.width, document.sequence.height,
                         bound, frameWidth, frameHeight);
        if (shownWidth * frameHeight >= shownHeight * frameWidth) {
            picture.width = frameWidth;
            picture.height =
                std::max<int64_t>(2, (frameWidth * shownHeight) /
                                         std::max<int64_t>(1, shownWidth));
        } else {
            picture.height = frameHeight;
            picture.width =
                std::max<int64_t>(2, (frameHeight * shownWidth) /
                                         std::max<int64_t>(1, shownHeight));
        }
    } else {
        BoundedFrameSize(
            shownWidth, shownHeight,
            size == ClipPictureSize::Shown
                ? bound
                : std::min<int64_t>(bound, std::max(shownWidth, shownHeight)),
            picture.width, picture.height);
    }
    if (picture.width <= 0 || picture.height <= 0) {
        error = named + "the picture it shows is empty";
        return false;
    }
    const int64_t width = picture.width;
    const int64_t height = picture.height;
    // Decoded as the monitor decodes it (Renderer.mm, shader.metal's
    // sampleYUV): FFmpeg only crops, reduces and lays the chroma out at
    // full resolution, at the source's own bit depth, and the code values
    // become RGB here with the monitor's range, matrix and constants. Not
    // through swscale's YCbCr-to-RGB conversion, which the export uses:
    // measured on FFmpeg 8.1, it reads limited-range codes 0.385 % low
    // (white 0.99615 instead of 1, at 8 and 10 bits alike), and a clipped
    // channel would read as unclipped.
    std::filesystem::path path(source->path);
    if (path.is_relative())
        path = std::filesystem::path(projectDirectory) / path;
    const std::string file =
        std::filesystem::absolute(path).lexically_normal().string();
    // PRD40 -- an offline rush is named by its path before it is probed or
    // decoded.
    if (!ffmpeg::InputPresent(file, error)) {
        error = named + error;
        return false;
    }
    // A media recorded without its pixel format (an early project, a
    // hand-written document) is asked of the file, as Export.cc asks for a
    // media it was never told about.
    LibraryMedia probed;
    const LibraryMedia* described = media;
    if (media->pixel_format.empty()) {
        if (!ProbeMediaMetadata(file, probed, error)) {
            error = named + "unable to probe its media: " + error;
            return false;
        }
        described = &probed;
    }
    const bool rgb = IsRgbPixelFormat(described->pixel_format);
    const int sourceDepth = PixelFormatBitDepth(described->pixel_format);
    if (sourceDepth <= 0 || sourceDepth > 16) {
        error = named + "its pixel format '" + described->pixel_format +
                "' is not one this measure reads";
        return false;
    }
    const int depth = sourceDepth <= 8    ? 8
                      : sourceDepth <= 10 ? 10
                      : sourceDepth <= 12 ? 12
                                          : 16;
    const std::string pixelFormat =
        rgb          ? "gbrpf32le"
        : depth == 8 ? "yuv444p"
                     : "yuv444p" + std::to_string(depth) + "le";
    const size_t sampleBytes = rgb ? 4 : depth == 8 ? 1 : 2;
    filter += "scale=w=" + std::to_string(width) +
              ":h=" + std::to_string(height) +
              ":flags=lanczos,format=" + pixelFormat;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. The picture a model is shown
    // (read_frame graded, a graded sheet) carries the clip's blur, drawn in
    // its own signal before it is graded, as the export draws it. The
    // measures read the material, unblurred: a blur moves no average, and
    // what match_color and propose_grade correct is the shot itself. The
    // signal here is already 4:4:4, or RGB.
    if (size != ClipPictureSize::Measured &&
        !picture.framing.blur_filter.empty())
        filter += ',' + picture.framing.blur_filter;
    const YuvRangeTag rangeTag =
        described->color_range == "pc"   ? YuvRangeTag::Full
        : described->color_range == "tv" ? YuvRangeTag::Limited
                                         : YuvRangeTag::Unspecified;
    const YuvCodeParameters code = BuildYuvCodeParameters(
        depth, ResolveYuvFullRange(color, rangeTag, rgb));
    const YuvMatrixParameters matrix = BuildYuvMatrixParameters(
        ResolveYuvBt2020Matrix(color, described->color_space == "bt2020nc"));
    const std::vector<std::string> arguments = {
        ffmpegPath,
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-ss",
        FfmpegSeekSeconds(time),
        "-i",
        file,
        "-map",
        "0:v:0",
        "-an",
        "-frames:v",
        "1",
        "-vf",
        filter,
        "-f",
        "rawvideo",
        "-pix_fmt",
        pixelFormat,
        "pipe:1",
    };
    const size_t pixels = static_cast<size_t>(width) * height;
    const float maximum = static_cast<float>((1u << depth) - 1u);
    std::vector<std::array<float, 3>>& signal = picture.signal;
    const bool decoded = ReadFfmpegRawFrames(
        arguments, pixels * 3 * sampleBytes,
        [&](const uint8_t* bytes) {
            if (!signal.empty()) return;
            signal.resize(pixels);
            const size_t planeBytes = pixels * sampleBytes;
            const auto sample = [&](size_t plane, size_t index) {
                const uint8_t* at =
                    bytes + plane * planeBytes + index * sampleBytes;
                if (rgb) {
                    float value = 0.0f;
                    std::memcpy(&value, at, sizeof(value));
                    return value;
                }
                if (sampleBytes == 1) return at[0] / maximum;
                return static_cast<float>(at[0] | (at[1] << 8)) / maximum;
            };
            for (size_t index = 0; index < pixels; ++index) {
                if (rgb) {
                    // gbrp: green, blue, red planes.
                    signal[index] = {sample(2, index), sample(0, index),
                                     sample(1, index)};
                    continue;
                }
                const float y =
                    (sample(0, index) - code.y_offset) * code.y_scale;
                const float cb =
                    (sample(1, index) - code.chroma_offset) * code.chroma_scale;
                const float cr =
                    (sample(2, index) - code.chroma_offset) * code.chroma_scale;
                signal[index] = {
                    y + matrix.red_from_cr * cr,
                    y + matrix.green_from_cb * cb + matrix.green_from_cr * cr,
                    y + matrix.blue_from_cb * cb};
            }
        },
        error);
    if (!decoded) {
        error = named + "FFmpeg could not decode the frame: " + error;
        return false;
    }
    if (signal.empty()) {
        error = named + "the source produced no frame at that position";
        return false;
    }
    return true;
}

// G2 -- what a stage appended to the clip's stack receives: the source
// through the input transform and the clip's own grade, in the space the
// grade acts in -- ACEScct code values, the linear working value, or the
// display signal -- and not clamped, since stages after it see it whole.
bool GradingValue(const ColorMeasureChain& chain,
                  const std::array<float, 3>& signal,
                  std::array<float, 3>& value, std::string& error) {
    std::array<float, 3> color = signal;
    for (float& channel : color) channel = std::clamp(channel, 0.0f, 1.0f);
    if (chain.color.enabled) {
        value = OpenColorIoPathWorking(chain.path, chain.grade, color);
        if (chain.color.working_gamut == "acescct") {
            for (float& channel : value)
                channel = static_cast<float>(EncodeAcesCct(channel));
        }
    } else {
        value = chain.grade.count > 0
                    ? ApplyResolvedColorGrade(color, chain.grade)
                    : color;
    }
    if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
        !std::isfinite(value[2])) {
        error = "the clip's grade produces a non-finite colour";
        return false;
    }
    return true;
}

// The values whose pixel centres lie in `region`, or all of them.
std::vector<std::array<float, 3>> InRegion(
    const std::vector<std::array<float, 3>>& values, int64_t width,
    int64_t height, const ColorMeasureRegion& region) {
    if (!region.present) return values;
    std::vector<std::array<float, 3>> selected;
    for (int64_t row = 0; row < height; ++row) {
        if (!CentreInside(row, height, region.y, region.height)) continue;
        for (int64_t column = 0; column < width; ++column) {
            if (CentreInside(column, width, region.x, region.width))
                selected.push_back(
                    values[static_cast<size_t>(row * width + column)]);
        }
    }
    return selected;
}

}  // namespace

bool RenderColorMeasurement(const Document& document,
                            const std::string& projectDirectory,
                            const ColorMeasurePlan& plan,
                            const ColorMeasureSettings& settings,
                            std::vector<ColorClipMeasurement>& results,
                            std::string& error) {
    results.clear();
    error.clear();
    if (plan.clip_ids.empty() || plan.clip_ids.size() > kMaxColorMeasureClips) {
        error = "name between 1 and " + std::to_string(kMaxColorMeasureClips) +
                " clips to measure";
        return false;
    }
    if (plan.position != "Start" && plan.position != "Middle" &&
        plan.position != "End") {
        error = "the position must be 'Start', 'Middle' or 'End'";
        return false;
    }
    if (!ValidateColorMeasureRegion(plan.region, error)) return false;
    if (settings.max_dimension < 16) {
        error = "invalid colour measurement settings";
        return false;
    }
    ColorMeasureChain base;
    if (!BuildSharedChain(document, document.color_management, projectDirectory,
                          base, error))
        return false;

    for (const Ulid& clipId : plan.clip_ids) {
        const DocumentClip* clip = document.FindClip(clipId);
        if (clip == nullptr) {
            error = "unknown clip_id '" + clipId + "'";
            return false;
        }
        if (!CheckGradedClip(document, *clip, error)) return false;
        RationalTime time;
        ClipFramePositionTime(*clip, plan.position, time);
        time =
            SourceFrameStart(time, document.FindSource(clip->source_id)->rate);
        ColorMeasureChain chain;
        if (!ClipChain(document, base, *clip, chain, error)) return false;
        ClipPicture picture;
        if (!DecodeClipPicture(document, projectDirectory, *clip, time,
                               ClipPictureSize::Measured,
                               settings.max_dimension, chain.color,
                               settings.ffmpeg_path, picture, error))
            return false;

        chain.grade = ResolveColorGrade(clip->effects,
                                        ColorGradeContextFor(document, *clip));
        ColorClipMeasurement result;
        result.clip_id = clipId;
        result.source_id = clip->source_id;
        result.source_time = time;
        result.width = static_cast<int32_t>(picture.width);
        result.height = static_cast<int32_t>(picture.height);
        if (!MeasureColorSignal(picture.signal, result.width, result.height,
                                chain, plan.region, result.picture,
                                result.region, error)) {
            error = "clip '" + clipId + "': " + error;
            return false;
        }
        results.push_back(std::move(result));
    }
    return true;
}

bool CaptureGradedClipFrame(const Document& document,
                            const std::string& projectDirectory,
                            const Ulid& clipId, const RationalTime& sourceTime,
                            const FrameCaptureSettings& settings,
                            std::string& jpegBytes, std::string& error) {
    jpegBytes.clear();
    error.clear();
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    if (!CheckGradedClip(document, *clip, error)) return false;
    // Record shows an HLG delivery through its SDR view, and so does every
    // picture a model reads (read_frame, the sheets).
    ColorMeasureChain shared;
    ColorMeasureChain chain;
    if (!BuildSharedChain(
            document, ColorManagementForSdrPreview(document.color_management),
            projectDirectory, shared, error) ||
        !ClipChain(document, shared, *clip, chain, error))
        return false;
    chain.grade =
        ResolveColorGrade(clip->effects, ColorGradeContextFor(document, *clip));
    ClipPicture picture;
    if (!DecodeClipPicture(document, projectDirectory, *clip, sourceTime,
                           ClipPictureSize::Shown, settings.max_dimension,
                           chain.color, settings.ffmpeg_path, picture, error))
        return false;
    const LumaWeights scene = SceneWeights(chain.color);
    std::vector<std::array<float, 3>> display(picture.signal.size());
    float sceneLuminance = 0.0f;
    for (size_t index = 0; index < display.size(); ++index) {
        if (!GradePixel(chain, scene, picture.signal[index], display[index],
                        sceneLuminance, error)) {
            error = "clip '" + clipId + "': " + error;
            return false;
        }
    }
    FrameCaptureSettings encoding = settings;
    encoding.framed = picture.framing.framed;
    encoding.framing = picture.framing.framing;
    encoding.frame_width = picture.framing.frame_width;
    encoding.frame_height = picture.framing.frame_height;
    return EncodeDisplayPictureAsJpeg(display, picture.width, picture.height,
                                      encoding, jpegBytes, error);
}

bool RenderGradedSheetCells(
    const Document& document, const std::string& projectDirectory,
    const std::vector<GradedSheetCell>& cells, int64_t cellWidth,
    int64_t cellHeight, int32_t bound, const std::string& ffmpegPath,
    std::vector<std::vector<std::array<float, 3>>>& pictures,
    std::string& error) {
    pictures.clear();
    error.clear();
    if (cellWidth <= 0 || cellHeight <= 0 || bound < 16) {
        error = "invalid graded sheet cell size";
        return false;
    }
    // One colour path for the whole sheet, in the SDR view every picture a
    // model reads goes through, as read_frame graded builds it per frame.
    ColorMeasureChain shared;
    if (!BuildSharedChain(
            document, ColorManagementForSdrPreview(document.color_management),
            projectDirectory, shared, error))
        return false;
    pictures.reserve(cells.size());
    for (const GradedSheetCell& cell : cells) {
        const DocumentClip* clip = document.FindClip(cell.clip_id);
        if (clip == nullptr) {
            error = "unknown clip_id '" + cell.clip_id + "'";
            return false;
        }
        // DEC31 -- under colour management a graphic takes no grade (the
        // export refuses one): its cell shows it as authored, its code
        // values, without the sequence look either. Every other clip is
        // checked as read_frame graded checks it.
        const bool graphic = document.color_management.enabled &&
                             document.IsGraphicsMedia(clip->source_id);
        if (!graphic && !CheckGradedClip(document, *clip, error)) return false;
        ColorMeasureChain chain;
        if (!ClipChain(document, shared, *clip, chain, error)) return false;
        if (graphic) {
            chain.look_stage = LookLutStage::None;
            chain.look.reset();
        } else {
            chain.grade = ResolveColorGrade(
                clip->effects, ColorGradeContextFor(document, *clip));
        }
        ClipPicture picture;
        if (!DecodeClipPicture(document, projectDirectory, *clip,
                               cell.source_time, ClipPictureSize::Fitted, bound,
                               chain.color, ffmpegPath, picture, error))
            return false;
        // Where the picture lies in the cell: the framing's rectangle, or
        // centred. A cell is the sequence frame rounded down to even sizes,
        // so a pixel the rounding cut is dropped, never squeezed in.
        const int64_t left = picture.framing.framed
                                 ? picture.framing.framing.x
                                 : (cellWidth - picture.width) / 2;
        const int64_t top = picture.framing.framed
                                ? picture.framing.framing.y
                                : (cellHeight - picture.height) / 2;
        std::vector<std::array<float, 3>> composed(
            static_cast<size_t>(cellWidth * cellHeight),
            std::array<float, 3>{0.0f, 0.0f, 0.0f});
        const LumaWeights scene = SceneWeights(chain.color);
        float sceneLuminance = 0.0f;
        for (int64_t row = 0; row < picture.height; ++row) {
            const int64_t y = top + row;
            if (y < 0 || y >= cellHeight) continue;
            for (int64_t column = 0; column < picture.width; ++column) {
                const int64_t x = left + column;
                if (x < 0 || x >= cellWidth) continue;
                std::array<float, 3> display{};
                if (!GradePixel(chain, scene,
                                picture.signal[static_cast<size_t>(
                                    row * picture.width + column)],
                                display, sceneLuminance, error)) {
                    error = "clip '" + cell.clip_id + "': " + error;
                    return false;
                }
                composed[static_cast<size_t>(y * cellWidth + x)] = display;
            }
        }
        pictures.push_back(std::move(composed));
    }
    return true;
}

namespace {

// One clip as the grade tools read it: its frame, its chain, its grading
// values. Shared by match_color and propose_grade (A20).
struct GradedSide {
    const DocumentClip* clip = nullptr;
    RationalTime time;
    ClipPicture picture;
    ColorMeasureChain chain;
    ColorGradeContext context;
    std::vector<std::array<float, 3>> values;
};

bool LoadGradedSide(const Document& document,
                    const std::string& projectDirectory,
                    const ColorMeasureChain& base,
                    const ColorMeasureSettings& settings, const Ulid& clipId,
                    const std::string& position, GradedSide& side,
                    std::string& error) {
    side.clip = document.FindClip(clipId);
    if (side.clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    if (!CheckGradedClip(document, *side.clip, error)) return false;
    ClipFramePositionTime(*side.clip, position, side.time);
    side.time = SourceFrameStart(
        side.time, document.FindSource(side.clip->source_id)->rate);
    if (!ClipChain(document, base, *side.clip, side.chain, error) ||
        !DecodeClipPicture(document, projectDirectory, *side.clip, side.time,
                           ClipPictureSize::Measured, settings.max_dimension,
                           side.chain.color, settings.ffmpeg_path, side.picture,
                           error))
        return false;
    side.context = ColorGradeContextFor(document, *side.clip);
    side.chain.grade = ResolveColorGrade(side.clip->effects, side.context);
    side.values.resize(side.picture.signal.size());
    for (size_t index = 0; index < side.values.size(); ++index) {
        if (!GradingValue(side.chain, side.picture.signal[index],
                          side.values[index], error)) {
            error = "clip '" + clipId + "': " + error;
            return false;
        }
    }
    return true;
}

// measure_color's numbers of one side under a grade.
bool MeasureSide(const GradedSide& side, const ResolvedColorGrade& grade,
                 const ColorMeasureRegion& region, ColorClipMeasurement& out,
                 std::string& error) {
    ColorMeasureChain chain = side.chain;
    chain.grade = grade;
    out.clip_id = side.clip->id;
    out.source_id = side.clip->source_id;
    out.source_time = side.time;
    out.width = static_cast<int32_t>(side.picture.width);
    out.height = static_cast<int32_t>(side.picture.height);
    return MeasureColorSignal(side.picture.signal, out.width, out.height, chain,
                              region, out.picture, out.region, error);
}

// What the refinement reads of a measure.
ColorMatchDisplay DisplayOf(const ColorPictureMeasure& measured) {
    ColorMatchDisplay out;
    out.luma_p50 = static_cast<double>(measured.luma_p50);
    out.has_midtones = measured.zones[1].pixels > 0;
    out.warmth = static_cast<double>(measured.zones[1].warmth);
    out.green_magenta = static_cast<double>(measured.zones[1].green_magenta);
    out.saturation = static_cast<double>(measured.saturation_mean);
    return out;
}

// AGENT-2026-09 A20 -- the mean colour of a whole anchor, the three zones
// weighed by their pixels: the colour whose Cb, Cr the skin line offset is
// read on (the mean of a linear function is the function of the mean), so
// a balance aimed at it lands where the reply reads. Aimed at the midtones
// alone, a red prosthetic with dark red shadows balanced its midtones and
// read 4 degrees off all the same.
ColorZoneMeasure MeanZone(const ColorPictureMeasure& measured) {
    ColorZoneMeasure mean;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (const ColorZoneMeasure& zone : measured.zones) {
        mean.pixels += zone.pixels;
        red += static_cast<double>(zone.pixels) * zone.red;
        green += static_cast<double>(zone.pixels) * zone.green;
        blue += static_cast<double>(zone.pixels) * zone.blue;
    }
    if (mean.pixels <= 0) return mean;
    const double pixels = static_cast<double>(mean.pixels);
    mean.red = std::llround(red / pixels);
    mean.green = std::llround(green / pixels);
    mean.blue = std::llround(blue / pixels);
    mean.warmth = mean.red - mean.blue;
    mean.green_magenta = std::llround(
        static_cast<double>(2 * mean.green - mean.red - mean.blue) / 2.0);
    return mean;
}

ColorMatchDisplay AnchorDisplayOf(const ColorPictureMeasure& measured) {
    const ColorZoneMeasure mean = MeanZone(measured);
    ColorMatchDisplay out;
    out.luma_p50 = static_cast<double>(measured.luma_p50);
    out.has_midtones = mean.pixels > 0;
    out.warmth = static_cast<double>(mean.warmth);
    out.green_magenta = static_cast<double>(mean.green_magenta);
    out.saturation = static_cast<double>(measured.saturation_mean);
    return out;
}

}  // namespace

bool RenderColorMatch(const Document& document,
                      const std::string& projectDirectory,
                      const ColorMatchPlan& plan,
                      const ColorMeasureSettings& settings,
                      ColorMatchResult& result, std::string& error) {
    result = ColorMatchResult{};
    error.clear();
    for (const std::string* position :
         {&plan.position, &plan.reference_position}) {
        if (*position != "Start" && *position != "Middle" &&
            *position != "End") {
            error = "a position must be 'Start', 'Middle' or 'End'";
            return false;
        }
    }
    if (!ValidateColorMeasureRegion(plan.region, error) ||
        !ValidateColorMeasureRegion(plan.reference_region, error))
        return false;
    if (settings.max_dimension < 16) {
        error = "invalid colour measurement settings";
        return false;
    }
    ColorMeasureChain base;
    if (!BuildSharedChain(document, document.color_management, projectDirectory,
                          base, error))
        return false;
    result.grading_space = !base.color.enabled                     ? "display"
                           : base.color.working_gamut == "acescct" ? "acescct"
                                                                   : "linear";

    GradedSide reference;
    GradedSide clip;
    if (!LoadGradedSide(document, projectDirectory, base, settings,
                        plan.reference_clip_id, plan.reference_position,
                        reference, error) ||
        !LoadGradedSide(document, projectDirectory, base, settings,
                        plan.clip_id, plan.position, clip, error))
        return false;

    const auto clipValues = InRegion(clip.values, clip.picture.width,
                                     clip.picture.height, plan.region);
    const auto referenceValues =
        InRegion(reference.values, reference.picture.width,
                 reference.picture.height, plan.reference_region);
    if (clipValues.empty() || referenceValues.empty()) {
        error = "a region holds no measured pixel; widen it";
        return false;
    }
    ColorMatchGuess guess;
    if (!SolveColorMatch(clipValues, referenceValues, plan.aspects, guess,
                         error))
        return false;
    result.saturation_undefined = guess.saturation_undefined;

    // measure_color's numbers of one side under a grade.
    const auto measure =
        [&](const GradedSide& side, const ResolvedColorGrade& grade,
            const ColorMeasureRegion& region, ColorClipMeasurement& out) {
            ColorMeasureChain chain = side.chain;
            chain.grade = grade;
            out.clip_id = side.clip->id;
            out.source_id = side.clip->source_id;
            out.source_time = side.time;
            out.width = static_cast<int32_t>(side.picture.width);
            out.height = static_cast<int32_t>(side.picture.height);
            return MeasureColorSignal(side.picture.signal, out.width,
                                      out.height, chain, region, out.picture,
                                      out.region, error);
        };
    const auto display = [](const ColorPictureMeasure& measured) {
        ColorMatchDisplay out;
        out.luma_p50 = static_cast<double>(measured.luma_p50);
        out.has_midtones = measured.zones[1].pixels > 0;
        out.warmth = static_cast<double>(measured.zones[1].warmth);
        out.green_magenta =
            static_cast<double>(measured.zones[1].green_magenta);
        out.saturation = static_cast<double>(measured.saturation_mean);
        return out;
    };
    if (!measure(reference, reference.chain.grade, plan.reference_region,
                 result.reference))
        return false;
    // Refined against what the reply will report: the clip rendered with
    // candidate knobs in its own stages.
    const ColorMatchEvaluator evaluate = [&](const ColorMatchKnobs& candidate,
                                             ColorMatchDisplay& out,
                                             std::string& evaluationError) {
        const std::vector<ClipEffect> stack =
            PlaceColorMatch(clip.clip->effects, candidate, clip.context);
        ColorClipMeasurement measured;
        if (!measure(clip, ResolveColorGrade(stack, clip.context), plan.region,
                     measured)) {
            evaluationError = error;
            return false;
        }
        out = display(plan.region.present ? measured.region : measured.picture);
        return true;
    };
    const ColorMatchDisplay target =
        display(plan.reference_region.present ? result.reference.region
                                              : result.reference.picture);
    result.knobs_before = ReadColorMatchKnobs(clip.clip->effects, clip.context);
    ColorMatchKnobs knobs = ApplyColorMatchGuess(result.knobs_before, guess,
                                                 plan.aspects, clip.context);
    if (!RefineColorMatch(target, plan.aspects, !guess.saturation_undefined,
                          evaluate, knobs, error))
        return false;
    result.effects = PlaceColorMatch(clip.clip->effects, knobs, clip.context,
                                     &result.placements);
    result.knobs_after = ReadColorMatchKnobs(result.effects, clip.context);
    result.kelvin_limited =
        plan.aspects.balance && (result.knobs_after.kelvin <= kMinimumKelvin ||
                                 result.knobs_after.kelvin >= kMaximumKelvin);
    if (result.effects.size() > static_cast<size_t>(kMaxColorEffectsPerClip)) {
        error = "the clip's stack would pass the " +
                std::to_string(kMaxColorEffectsPerClip) +
                " stages the renderer draws; simplify it first";
        return false;
    }
    for (const ClipEffect& effect : result.effects) {
        if (!ValidateColorEffect(effect, error)) {
            error = "the matched stack would not be valid: " + error;
            return false;
        }
    }
    // The clip as it is, and as the step would leave it.
    return measure(clip, clip.chain.grade, plan.region, result.before) &&
           measure(clip, ResolveColorGrade(result.effects, clip.context),
                   plan.region, result.after);
}

namespace {

using mcp_json::Value;

// An integer of hundredths as the exact decimal it is: 4150 is 41.5, -35
// is -0.35. No double on the way, so the reply is the measure.
Value Hundredths(int64_t value) {
    const bool negative = value < 0;
    const uint64_t magnitude = negative
                                   ? static_cast<uint64_t>(-(value + 1)) + 1
                                   : static_cast<uint64_t>(value);
    std::string text = negative ? "-" : "";
    text += std::to_string(magnitude / 100);
    const uint64_t fraction = magnitude % 100;
    if (fraction != 0) {
        text += '.';
        text += static_cast<char>('0' + fraction / 10);
        if (fraction % 10 != 0) text += static_cast<char>('0' + fraction % 10);
    }
    return Value::MakeRawNumber(text);
}

Value OptionalHundredths(const std::optional<int64_t>& value) {
    return value ? Hundredths(*value) : Value::MakeNull();
}

Value PercentOfPixels(int64_t pixels, int64_t total) {
    if (total <= 0) return Hundredths(0);
    return Hundredths(std::llround(10000.0 * static_cast<double>(pixels) /
                                   static_cast<double>(total)));
}

Value DisplayValue(const ColorPictureMeasure& measure, bool withHue) {
    Value display = Value::MakeObject();
    Value luma = Value::MakeObject();
    luma.Set("p02", Hundredths(measure.luma_p02));
    luma.Set("p50", Hundredths(measure.luma_p50));
    luma.Set("p98", Hundredths(measure.luma_p98));
    display.Set("luma_percent", std::move(luma));
    display.Set("clipped_percent", Hundredths(measure.clipped));
    display.Set("crushed_percent", Hundredths(measure.crushed));
    Value saturation = Value::MakeObject();
    saturation.Set("mean", Hundredths(measure.saturation_mean));
    saturation.Set("p95", Hundredths(measure.saturation_p95));
    display.Set("saturation", std::move(saturation));
    Value zones = Value::MakeObject();
    static const char* const kZoneNames[] = {"shadows", "midtones",
                                             "highlights"};
    for (size_t index = 0; index < 3; ++index) {
        const ColorZoneMeasure& zone = measure.zones[index];
        Value item = Value::MakeObject();
        item.Set("share_percent", PercentOfPixels(zone.pixels, measure.pixels));
        if (zone.pixels > 0) {
            item.Set("red", Hundredths(zone.red));
            item.Set("green", Hundredths(zone.green));
            item.Set("blue", Hundredths(zone.blue));
            item.Set("warmth", Hundredths(zone.warmth));
            item.Set("green_magenta", Hundredths(zone.green_magenta));
        }
        zones.Set(kZoneNames[index], std::move(item));
    }
    display.Set("zones", std::move(zones));
    if (withHue) {
        display.Set("hue_degrees", OptionalHundredths(measure.hue));
        display.Set("skin_line_offset_degrees",
                    OptionalHundredths(measure.skin_line_offset));
    }
    return display;
}

Value SceneValue(const ColorPictureMeasure& measure) {
    if (!measure.scene) return Value::MakeNull();
    Value stops = Value::MakeObject();
    stops.Set("p02", OptionalHundredths(measure.scene_p02));
    stops.Set("p50", OptionalHundredths(measure.scene_p50));
    stops.Set("p98", OptionalHundredths(measure.scene_p98));
    Value scene = Value::MakeObject();
    scene.Set("stops_from_middle_grey", std::move(stops));
    return scene;
}

Value FractionValue(const EffectParamValue& value) {
    Value fraction = Value::MakeObject();
    fraction.Set("num", Value::MakeInt(value.num));
    fraction.Set("den", Value::MakeInt(value.den));
    return fraction;
}

Value ChainValue(const Document& document) {
    Value chain = Value::MakeObject();
    const ColorManagementSettings& color = document.color_management;
    chain.Set("color_management", Value::MakeBool(color.enabled));
    if (color.enabled) {
        chain.Set("working", Value::MakeString(color.working_gamut));
        chain.Set("output", Value::MakeString(color.output_gamut + "/" +
                                              color.output_transfer));
    }
    LookLutStage stage = LookLutStage::None;
    std::string ignored;
    ResolveLookLutStage(document.sequence.look_lut, color, stage, ignored);
    chain.Set("look", Value::MakeString(LookStageName(stage)));
    return chain;
}

Value RegionValue(const ColorMeasureRegion& region) {
    Value value = Value::MakeObject();
    value.Set("x", FractionValue(region.x));
    value.Set("y", FractionValue(region.y));
    value.Set("width", FractionValue(region.width));
    value.Set("height", FractionValue(region.height));
    return value;
}

Value ClipMeasurementValue(const Document& document,
                           const ColorClipMeasurement& result,
                           bool withRegion) {
    Value item = Value::MakeObject();
    item.Set("clip_id", Value::MakeString(result.clip_id));
    item.Set("source_id", Value::MakeString(result.source_id));
    // G3 -- the input the clip was read with, and whose it is: a clip can
    // measure wrong only because its media is read in the wrong space.
    if (document.color_management.enabled) {
        const LibraryMedia* media = document.FindLibraryMedia(result.source_id);
        const bool own =
            media != nullptr && DeclaresColorInput(media->color_input);
        const ColorManagementSettings color =
            document.ColorManagementFor(result.source_id);
        Value input = Value::MakeObject();
        input.Set("gamut", Value::MakeString(color.input_gamut));
        input.Set("transfer", Value::MakeString(color.input_transfer));
        input.Set("from", Value::MakeString(own ? "media" : "project"));
        item.Set("input", std::move(input));
    }
    Value at = Value::MakeObject();
    at.Set("value", Value::MakeInt(result.source_time.value));
    at.Set("rate", Value::MakeInt(result.source_time.rate));
    item.Set("source_in", std::move(at));
    Value size = Value::MakeObject();
    size.Set("width", Value::MakeInt(result.width));
    size.Set("height", Value::MakeInt(result.height));
    item.Set("measured_pixels", std::move(size));
    item.Set("display", DisplayValue(result.picture, false));
    item.Set("scene", SceneValue(result.picture));
    if (withRegion) {
        Value region = Value::MakeObject();
        region.Set("pixels", Value::MakeInt(result.region.pixels));
        region.Set("display", DisplayValue(result.region, true));
        region.Set("scene", SceneValue(result.region));
        item.Set("region", std::move(region));
    }
    return item;
}

Value EffectsValue(const std::vector<ClipEffect>& effects) {
    Value list = Value::MakeArray();
    for (const ClipEffect& effect : effects) {
        Value item = Value::MakeObject();
        item.Set("type", Value::MakeString(effect.type));
        Value params = Value::MakeObject();
        for (const auto& [name, value] : effect.params)
            params.Set(name, FractionValue(value));
        item.Set("params", std::move(params));
        list.Push(std::move(item));
    }
    return list;
}

}  // namespace

mcp_json::Value ColorMeasurementsValue(
    const Document& document, const ColorMeasurePlan& plan,
    const std::vector<ColorClipMeasurement>& results) {
    Value reply = Value::MakeObject();
    reply.Set("position", Value::MakeString(plan.position));
    reply.Set("chain", ChainValue(document));
    if (plan.region.present) reply.Set("region", RegionValue(plan.region));
    Value clips = Value::MakeArray();
    for (const ColorClipMeasurement& result : results)
        clips.Push(ClipMeasurementValue(document, result, plan.region.present));
    reply.Set("clips", std::move(clips));
    return reply;
}

mcp_json::Value ColorMatchValue(const Document& document,
                                const ColorMatchPlan& plan,
                                const ColorMatchResult& result) {
    Value reply = Value::MakeObject();
    reply.Set("clip_id", Value::MakeString(plan.clip_id));
    reply.Set("reference_clip_id", Value::MakeString(plan.reference_clip_id));
    reply.Set("chain", ChainValue(document));
    reply.Set("grading_space", Value::MakeString(result.grading_space));
    if (plan.region.present) reply.Set("region", RegionValue(plan.region));
    if (plan.reference_region.present)
        reply.Set("reference_region", RegionValue(plan.reference_region));
    // Each knob as the clip holds it before and as the step leaves it.
    const auto knob = [](double before, double after, int digits) {
        Value pair = Value::MakeObject();
        const auto number = [digits](double value) {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << std::fixed << std::setprecision(digits) << value;
            return Value::MakeRawNumber(stream.str());
        };
        pair.Set("before", number(before));
        pair.Set("after", number(after));
        return pair;
    };
    Value correction = Value::MakeObject();
    correction.Set("exposure", knob(result.knobs_before.exposure,
                                    result.knobs_after.exposure, 3));
    correction.Set("temperature_kelvin", knob(result.knobs_before.kelvin,
                                              result.knobs_after.kelvin, 0));
    correction.Set("tint",
                   knob(result.knobs_before.tint, result.knobs_after.tint, 3));
    correction.Set("saturation", knob(result.knobs_before.saturation,
                                      result.knobs_after.saturation, 3));
    correction.Set("kelvin_limited", Value::MakeBool(result.kelvin_limited));
    correction.Set("saturation_undefined",
                   Value::MakeBool(result.saturation_undefined));
    reply.Set("correction", std::move(correction));
    Value placements = Value::MakeArray();
    for (const ColorMatchPlacement& placement : result.placements) {
        Value item = Value::MakeObject();
        item.Set("type", Value::MakeString(placement.type));
        item.Set("placement",
                 Value::MakeString(placement.adjusted ? "adjusted" : "added"));
        placements.Push(std::move(item));
    }
    reply.Set("placements", std::move(placements));
    // What to hand prepare_edit_plan as a step, or set_clip_effects
    // directly: the clip's whole stack with the correction in it.
    Value step = Value::MakeObject();
    step.Set("tool", Value::MakeString("set_clip_effects"));
    Value arguments = Value::MakeObject();
    arguments.Set("clip_id", Value::MakeString(plan.clip_id));
    arguments.Set("effects", EffectsValue(result.effects));
    step.Set("arguments", std::move(arguments));
    reply.Set("step", std::move(step));
    // After minus reference, in measure_color's units, on what each side
    // compared: how far the step lands from the reference.
    const ColorPictureMeasure& reached =
        plan.region.present ? result.after.region : result.after.picture;
    const ColorPictureMeasure& aimed = plan.reference_region.present
                                           ? result.reference.region
                                           : result.reference.picture;
    Value residual = Value::MakeObject();
    residual.Set("luma_percent", Hundredths(reached.luma_p50 - aimed.luma_p50));
    if (reached.zones[1].pixels > 0 && aimed.zones[1].pixels > 0) {
        residual.Set("midtone_warmth", Hundredths(reached.zones[1].warmth -
                                                  aimed.zones[1].warmth));
        residual.Set("midtone_green_magenta",
                     Hundredths(reached.zones[1].green_magenta -
                                aimed.zones[1].green_magenta));
    }
    residual.Set("saturation",
                 Hundredths(reached.saturation_mean - aimed.saturation_mean));
    reply.Set("residual", std::move(residual));
    Value measures = Value::MakeObject();
    measures.Set("reference",
                 ClipMeasurementValue(document, result.reference,
                                      plan.reference_region.present));
    measures.Set("before", ClipMeasurementValue(document, result.before,
                                                plan.region.present));
    measures.Set("after", ClipMeasurementValue(document, result.after,
                                               plan.region.present));
    reply.Set("measures", std::move(measures));
    return reply;
}

bool ProposeExposure(const Document& document,
                     const std::vector<ColorClipMeasurement>& measurements,
                     const ExposureProposalSettings& settings,
                     std::vector<ExposureProposal>& proposals,
                     std::string& error) {
    proposals.clear();
    error.clear();
    if (settings.tolerance_hundredths < 0) {
        error = "exposure tolerance must not be negative";
        return false;
    }
    for (const ColorClipMeasurement& measurement : measurements) {
        const DocumentClip* clip = document.FindClip(measurement.clip_id);
        if (clip == nullptr) {
            error = "unknown clip '" + measurement.clip_id + "'";
            return false;
        }
        ExposureProposal proposal;
        proposal.clip_id = measurement.clip_id;
        const ColorGradeContext context = ColorGradeContextFor(document, *clip);
        const ColorMatchKnobs knobs =
            ReadColorMatchKnobs(clip->effects, context);
        proposal.exposure_before = knobs.exposure;
        proposal.exposure_after = knobs.exposure;
        if (!measurement.picture.scene || !measurement.picture.scene_p50) {
            proposal.reason = "no_scene_measure";
            proposals.push_back(std::move(proposal));
            continue;
        }
        proposal.scene_p50_hundredths = measurement.picture.scene_p50;
        const int64_t difference =
            *measurement.picture.scene_p50 - settings.target_hundredths;
        if (std::llabs(difference) <= settings.tolerance_hundredths) {
            proposal.reason = "within_tolerance";
            proposals.push_back(std::move(proposal));
            continue;
        }
        proposal.correction_hundredths = -difference;
        ColorMatchKnobs corrected = knobs;
        corrected.exposure =
            knobs.exposure - static_cast<double>(difference) / 100.0;
        proposal.exposure_after = corrected.exposure;
        proposal.effects = PlaceColorMatch(clip->effects, corrected, context,
                                           &proposal.placements);
        proposal.reason = "proposed";
        proposals.push_back(std::move(proposal));
    }
    return true;
}

mcp_json::Value ExposureProposalsValue(
    const Document& document, const ColorMeasurePlan& plan,
    const ExposureProposalSettings& settings,
    const std::vector<ColorClipMeasurement>& measurements,
    const std::vector<ExposureProposal>& proposals) {
    Value reply = Value::MakeObject();
    reply.Set("position", Value::MakeString(plan.position));
    reply.Set("target_stops", Hundredths(settings.target_hundredths));
    reply.Set("tolerance_stops", Hundredths(settings.tolerance_hundredths));
    reply.Set("chain", ChainValue(document));
    const auto number = [](double value) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(3) << value;
        return Value::MakeRawNumber(stream.str());
    };
    Value clips = Value::MakeArray();
    Value steps = Value::MakeArray();
    int64_t proposed = 0;
    for (const ExposureProposal& proposal : proposals) {
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(proposal.clip_id));
        const auto measured =
            std::find_if(measurements.begin(), measurements.end(),
                         [&](const ColorClipMeasurement& measurement) {
                             return measurement.clip_id == proposal.clip_id;
                         });
        if (measured != measurements.end()) {
            item.Set("source_id", Value::MakeString(measured->source_id));
            item.Set("scene_p50_stops",
                     OptionalHundredths(measured->picture.scene_p50));
            item.Set("clipped_percent", Hundredths(measured->picture.clipped));
            item.Set("crushed_percent", Hundredths(measured->picture.crushed));
        }
        item.Set("correction_stops",
                 Hundredths(proposal.correction_hundredths));
        Value exposure = Value::MakeObject();
        exposure.Set("before", number(proposal.exposure_before));
        exposure.Set("after", number(proposal.exposure_after));
        item.Set("exposure", std::move(exposure));
        item.Set("reason", Value::MakeString(proposal.reason));
        if (proposal.reason == "proposed") {
            Value placements = Value::MakeArray();
            for (const ColorMatchPlacement& placement : proposal.placements) {
                Value placed = Value::MakeObject();
                placed.Set("type", Value::MakeString(placement.type));
                placed.Set("placement",
                           Value::MakeString(placement.adjusted ? "adjusted"
                                                                : "added"));
                placements.Push(std::move(placed));
            }
            item.Set("placements", std::move(placements));
            Value step = Value::MakeObject();
            step.Set("tool", Value::MakeString("set_clip_effects"));
            Value arguments = Value::MakeObject();
            arguments.Set("clip_id", Value::MakeString(proposal.clip_id));
            arguments.Set("effects", EffectsValue(proposal.effects));
            step.Set("arguments", std::move(arguments));
            steps.Push(std::move(step));
            ++proposed;
        }
        clips.Push(std::move(item));
    }
    reply.Set("clips", std::move(clips));
    reply.Set("steps", std::move(steps));
    // AGENT-2026-09 A19 -- where the montage sits as a whole, so a caller
    // can pick a target that keeps a bright scene bright: the median, the
    // lowest and the highest scene medians measured.
    std::vector<int64_t> medians;
    for (const ColorClipMeasurement& measurement : measurements)
        if (measurement.picture.scene && measurement.picture.scene_p50)
            medians.push_back(*measurement.picture.scene_p50);
    if (!medians.empty()) {
        std::sort(medians.begin(), medians.end());
        Value summary = Value::MakeObject();
        summary.Set("clips_measured",
                    Value::MakeInt(static_cast<int64_t>(medians.size())));
        summary.Set("median_stops", Hundredths(medians[medians.size() / 2]));
        summary.Set("min_stops", Hundredths(medians.front()));
        summary.Set("max_stops", Hundredths(medians.back()));
        reply.Set("summary", std::move(summary));
    }
    reply.Set("proposed", Value::MakeInt(proposed));
    reply.Set("skipped", Value::MakeInt(static_cast<int64_t>(proposals.size()) -
                                        proposed));
    return reply;
}

// ---------------------------------------------------------------------
// AGENT-2026-09 A20 -- propose_grade.
// ---------------------------------------------------------------------

bool SkinLineBalanceTarget(const ColorZoneMeasure& midtones,
                           bool rec2020Display, int64_t& warmth,
                           int64_t& greenMagenta) {
    warmth = 0;
    greenMagenta = 0;
    if (midtones.pixels <= 0) return false;
    const LumaWeights weights = rec2020Display ? kRec2020Luma : kRec709Luma;
    // The zone's means, hundredths of a percent, back to 0..1.
    const double red = static_cast<double>(midtones.red) / 10000.0;
    const double green = static_cast<double>(midtones.green) / 10000.0;
    const double blue = static_cast<double>(midtones.blue) / 10000.0;
    const double luma =
        weights.red * red + weights.green * green + weights.blue * blue;
    const double cb = (blue - luma) / (2.0 * (1.0 - weights.blue));
    const double cr = (red - luma) / (2.0 * (1.0 - weights.red));
    const double magnitude = std::hypot(cb, cr);
    if (224.0 * magnitude < kMinimumHueChroma) return false;
    // The same chroma, turned onto the skin line.
    const double angle = static_cast<double>(kSkinLineDegrees) * M_PI / 180.0;
    const double skinCb = magnitude * std::cos(angle);
    const double skinCr = magnitude * std::sin(angle);
    const double skinRed = luma + skinCr * 2.0 * (1.0 - weights.red);
    const double skinBlue = luma + skinCb * 2.0 * (1.0 - weights.blue);
    const double skinGreen =
        (luma - weights.red * skinRed - weights.blue * skinBlue) /
        weights.green;
    warmth = Scaled(100.0 * (skinRed - skinBlue));
    greenMagenta = Scaled(100.0 * (skinGreen - (skinRed + skinBlue) / 2.0));
    return true;
}

int64_t LimitGradeExposure(int64_t wanted, std::optional<int64_t> framePeak,
                           int64_t ceiling, bool& limited) {
    limited = false;
    if (wanted <= 0 || !framePeak) return wanted;
    const int64_t room = ceiling - *framePeak;
    if (room >= wanted) return wanted;
    limited = true;
    return std::max<int64_t>(room, 0);
}

bool LimitGradeExposureOnDisplay(int64_t wanted, int64_t clippedBefore,
                                 const DisplayClipEvaluator& clippedAt,
                                 int64_t& allowed, bool& limited,
                                 std::string& error) {
    limited = false;
    allowed = wanted;
    if (wanted <= 0) return true;
    const int64_t bound = clippedBefore + kDisplayClipAllowance;
    int64_t clipped = 0;
    if (!clippedAt(wanted, clipped, error)) return false;
    if (clipped <= bound) return true;
    // The whole lift clips: the largest one that does not, between no lift
    // (the frame as it is) and the whole one.
    limited = true;
    int64_t low = 0;
    int64_t high = wanted;
    for (int renders = 1;
         renders < kDisplayCeilingRenders && high - low > kDisplayCeilingStep;
         ++renders) {
        const int64_t middle = low + (high - low) / 2;
        if (!clippedAt(middle, clipped, error)) return false;
        if (clipped <= bound)
            low = middle;
        else
            high = middle;
    }
    allowed = low;
    return true;
}

bool RenderGradeProposals(const Document& document,
                          const std::string& projectDirectory,
                          const GradeProposalPlan& plan,
                          const ColorMeasureSettings& settings,
                          std::vector<GradeProposal>& proposals,
                          std::string& error) {
    proposals.clear();
    error.clear();
    if (plan.position != "Start" && plan.position != "Middle" &&
        plan.position != "End") {
        error = "a position must be 'Start', 'Middle' or 'End'";
        return false;
    }
    if (plan.anchor != "face" && plan.anchor != "neutral" &&
        plan.anchor != "median") {
        error = "an anchor must be 'face', 'neutral' or 'median'";
        return false;
    }
    if (plan.anchor == "neutral" && !plan.region.present) {
        error = "a neutral anchor needs the region of the white or grey patch";
        return false;
    }
    if (!ValidateColorMeasureRegion(plan.region, error)) return false;
    if (plan.tolerance_hundredths < 0) {
        error = "the tolerance must not be negative";
        return false;
    }
    if (plan.anchor == "face" && CurrentFaceFinder() == nullptr) {
        error =
            "no face finder in this process: a grade read on the face "
            "needs Vision, which only the macOS cutmachine executable "
            "installs, not the build without a window (headless/)";
        return false;
    }
    if (settings.max_dimension < 16) {
        error = "invalid colour measurement settings";
        return false;
    }
    ColorMeasureChain base;
    if (!BuildSharedChain(document, document.color_management, projectDirectory,
                          base, error))
        return false;
    const bool rec2020 =
        base.color.enabled && base.color.output_gamut == "rec2020";
    const LumaWeights displayWeights = DisplayWeights(base.color);
    const LumaWeights sceneWeights = SceneWeights(base.color);

    std::vector<GradedSide> sides(plan.clip_ids.size());
    std::vector<size_t> faceless;
    std::vector<int64_t> framesAfter;
    // GR15 -- the lift a clip's highlights allow from `base`: under the
    // scene rule its frame's scene p98 (`framePeak`) kept under the
    // ceiling; under an HLG output, no more of the rendered frame on the
    // top code than the clip had as it came (`clippedBefore`).
    const auto limitLift =
        [&](const GradedSide& side, const ColorMatchKnobs& base, int64_t wanted,
            std::optional<int64_t> framePeak, int64_t clippedBefore,
            int64_t& allowed, bool& limited) -> bool {
        if (!plan.display_ceiling) {
            allowed = LimitGradeExposure(
                wanted, framePeak, plan.highlight_ceiling_hundredths, limited);
            return true;
        }
        const DisplayClipEvaluator clippedAt =
            [&](int64_t correction, int64_t& clipped,
                std::string& evaluationError) {
                ColorMatchKnobs candidate = base;
                candidate.exposure += static_cast<double>(correction) / 100.0;
                ColorClipMeasurement measured;
                if (!MeasureSide(side,
                                 ResolveColorGrade(
                                     PlaceColorMatch(side.clip->effects,
                                                     candidate, side.context),
                                     side.context),
                                 ColorMeasureRegion{}, measured,
                                 evaluationError))
                    return false;
                clipped = measured.picture.clipped;
                return true;
            };
        if (!LimitGradeExposureOnDisplay(wanted, clippedBefore, clippedAt,
                                         allowed, limited, error)) {
            error = "clip '" + side.clip->id + "': " + error;
            return false;
        }
        return true;
    };
    for (size_t index = 0; index < plan.clip_ids.size(); ++index) {
        GradedSide& side = sides[index];
        if (!LoadGradedSide(document, projectDirectory, base, settings,
                            plan.clip_ids[index], plan.position, side, error))
            return false;
        GradeProposal proposal;
        proposal.clip_id = side.clip->id;
        proposal.source_id = side.clip->source_id;
        proposal.knobs_before =
            ReadColorMatchKnobs(side.clip->effects, side.context);
        proposal.knobs_after = proposal.knobs_before;

        // The anchor: where the clip is read.
        ColorMeasureRegion region;
        if (plan.anchor == "face") {
            // The picture as the display shows it, 8 bits per channel, for
            // the finder.
            std::vector<uint8_t> rgb8;
            rgb8.reserve(side.picture.signal.size() * 3);
            for (const std::array<float, 3>& signal : side.picture.signal) {
                Sample sample;
                if (!ReadPixel(side.chain, displayWeights, sceneWeights, signal,
                               sample, error))
                    return false;
                for (int channel = 0; channel < 3; ++channel)
                    rgb8.push_back(static_cast<uint8_t>(std::lround(
                        std::clamp(sample.display[channel], 0.0f, 1.0f) *
                        255.0f)));
            }
            std::vector<ColorMeasureRegion> faces;
            if (!CurrentFaceFinder()(static_cast<int32_t>(side.picture.width),
                                     static_cast<int32_t>(side.picture.height),
                                     rgb8, faces, error)) {
                error = "clip '" + side.clip->id + "': " + error;
                return false;
            }
            if (faces.empty()) {
                proposal.anchor_used = "none";
            } else {
                region = faces.front();
                // A box the finder lets run past the edge is cut to the
                // picture: on the fourth run one such face refused the
                // measure of the whole montage.
                const auto clampSpan = [](EffectParamValue& origin,
                                          EffectParamValue& span) {
                    if (origin.den <= 0 || span.den <= 0) return;
                    const int64_t room =
                        static_cast<int64_t>(origin.den) * span.den -
                        static_cast<int64_t>(origin.num) * span.den;
                    const int64_t extent =
                        static_cast<int64_t>(span.num) * origin.den;
                    if (extent > room)
                        span = {static_cast<int32_t>(room / origin.den),
                                span.den};
                };
                clampSpan(region.x, region.width);
                clampSpan(region.y, region.height);
                proposal.anchor_used = "face";
                proposal.anchor_region = region;
            }
        } else if (plan.anchor == "neutral") {
            region = plan.region;
            proposal.anchor_used = "neutral";
            proposal.anchor_region = region;
        } else {
            proposal.anchor_used = "median";
        }
        if (!MeasureSide(side, side.chain.grade, region, proposal.before,
                         error)) {
            // One clip that cannot be read does not refuse the montage:
            // said, with its reason, and left alone.
            proposal.anchor_used = "none";
            proposal.reason = "unreadable";
            proposal.error = error;
            error.clear();
            proposals.push_back(std::move(proposal));
            continue;
        }
        proposal.after = proposal.before;
        if (proposal.anchor_used == "none") {
            proposal.reason = "no_face";
            faceless.push_back(index);
            proposals.push_back(std::move(proposal));
            continue;
        }

        const ColorPictureMeasure& anchored =
            region.present ? proposal.before.region : proposal.before.picture;
        ColorMatchKnobs knobs = proposal.knobs_before;
        bool changed = false;
        bool limited = false;
        std::string reason = "within_tolerance";
        if (plan.exposure) {
            if (!anchored.scene || !anchored.scene_p50) {
                reason = "no_scene_measure";
            } else {
                const int64_t wanted =
                    plan.target_hundredths - *anchored.scene_p50;
                if (std::llabs(wanted) > plan.tolerance_hundredths) {
                    proposal.wanted_hundredths = wanted;
                    int64_t correction = 0;
                    if (!limitLift(side, knobs, wanted,
                                   proposal.before.picture.scene_p98,
                                   proposal.before.picture.clipped, correction,
                                   limited))
                        return false;
                    if (correction != 0) {
                        knobs.exposure +=
                            static_cast<double>(correction) / 100.0;
                        proposal.correction_hundredths = correction;
                        changed = true;
                    }
                }
            }
        }
        if (plan.balance && region.present) {
            // The target is read once the exposure is set: the midtones of
            // the anchor are other pixels, brighter, once it is.
            ColorClipMeasurement exposed = proposal.before;
            if (changed &&
                !MeasureSide(
                    side,
                    ResolveColorGrade(PlaceColorMatch(side.clip->effects, knobs,
                                                      side.context),
                                      side.context),
                    region, exposed, error))
                return false;
            ColorMatchDisplay target;
            target.has_midtones = true;
            const ColorZoneMeasure anchorMean = MeanZone(exposed.region);
            bool haveTarget = anchorMean.pixels > 0;
            if (haveTarget && proposal.anchor_used == "face") {
                int64_t warmth = 0;
                int64_t greenMagenta = 0;
                haveTarget = SkinLineBalanceTarget(anchorMean, rec2020, warmth,
                                                   greenMagenta);
                target.warmth = static_cast<double>(warmth);
                target.green_magenta = static_cast<double>(greenMagenta);
            }
            if (haveTarget) {
                ColorMatchAspects aspects;
                aspects.exposure = false;
                aspects.balance = true;
                aspects.saturation = false;
                const ColorMatchEvaluator evaluate =
                    [&](const ColorMatchKnobs& candidate,
                        ColorMatchDisplay& out, std::string& evaluationError) {
                        const std::vector<ClipEffect> stack = PlaceColorMatch(
                            side.clip->effects, candidate, side.context);
                        ColorClipMeasurement measured;
                        if (!MeasureSide(side,
                                         ResolveColorGrade(stack, side.context),
                                         region, measured, evaluationError))
                            return false;
                        out = AnchorDisplayOf(measured.region);
                        return true;
                    };
                const ColorMatchKnobs unbalanced = knobs;
                if (!RefineColorMatch(target, aspects, false, evaluate, knobs,
                                      error)) {
                    error = "clip '" + side.clip->id + "': " + error;
                    return false;
                }
                if (std::abs(knobs.kelvin - unbalanced.kelvin) >= 1.0 ||
                    std::abs(knobs.tint - unbalanced.tint) >= 0.001) {
                    changed = true;
                    proposal.balanced = true;
                }
            }
        }
        if (changed) {
            proposal.placements.clear();
            proposal.effects = PlaceColorMatch(
                side.clip->effects, knobs, side.context, &proposal.placements);
            if (!MeasureSide(side,
                             ResolveColorGrade(proposal.effects, side.context),
                             region, proposal.after, error))
                return false;
            // The balance moved the luminance a little: the exposure trimmed
            // once more on what was rendered, within the same ceiling.
            const ColorPictureMeasure& landed =
                region.present ? proposal.after.region : proposal.after.picture;
            if (plan.exposure && landed.scene && landed.scene_p50) {
                const int64_t wanted =
                    plan.target_hundredths - *landed.scene_p50;
                if (std::llabs(wanted) > plan.tolerance_hundredths) {
                    bool trimmed = false;
                    int64_t trim = 0;
                    if (!limitLift(side, knobs, wanted,
                                   proposal.after.picture.scene_p98,
                                   proposal.before.picture.clipped, trim,
                                   trimmed))
                        return false;
                    limited = limited || trimmed;
                    if (trim != 0) {
                        knobs.exposure += static_cast<double>(trim) / 100.0;
                        proposal.correction_hundredths += trim;
                        proposal.placements.clear();
                        proposal.effects =
                            PlaceColorMatch(side.clip->effects, knobs,
                                            side.context, &proposal.placements);
                        if (!MeasureSide(side,
                                         ResolveColorGrade(proposal.effects,
                                                           side.context),
                                         region, proposal.after, error))
                            return false;
                    }
                }
            }
            proposal.reason = limited ? "limited_by_highlights" : "proposed";
        } else {
            proposal.reason = limited ? "limited_by_highlights" : reason;
        }
        if (!limited)
            proposal.wanted_hundredths = proposal.correction_hundredths;
        proposal.knobs_after = knobs;
        if (proposal.after.picture.scene && proposal.after.picture.scene_p50)
            framesAfter.push_back(*proposal.after.picture.scene_p50);
        proposals.push_back(std::move(proposal));
    }

    // AGENT-2026-09 A31 -- one grade per rush: the anchored clips of a
    // source vote, every clip of the source gets the vote, each within its
    // own highlight ceiling. A clip without a face whose rush has a vote
    // follows its rush rather than the montage.
    if (plan.per_source) {
        const std::map<Ulid, SourceGradeVote> votes =
            SourceGradeVotes(proposals);
        std::vector<size_t> stillFaceless;
        for (size_t index : faceless)
            if (!votes.count(proposals[index].source_id))
                stillFaceless.push_back(index);
        faceless = stillFaceless;
        for (size_t index = 0; index < proposals.size(); ++index) {
            GradeProposal& proposal = proposals[index];
            const auto vote = votes.find(proposal.source_id);
            if (vote == votes.end()) continue;
            const GradedSide& side = sides[index];
            const ColorMeasureRegion region = proposal.anchor_region;
            ColorMatchKnobs knobs = proposal.knobs_before;
            bool changed = false;
            bool limited = false;
            proposal.correction_hundredths = 0;
            proposal.balanced = false;
            if (plan.exposure) {
                int64_t correction = 0;
                if (!limitLift(side, knobs, vote->second.correction_hundredths,
                               proposal.before.picture.scene_p98,
                               proposal.before.picture.clipped, correction,
                               limited))
                    return false;
                // GR15 -- the rush itself was held back: its clips say so,
                // even those whose own frame had room.
                limited = limited || vote->second.limited;
                if (correction != 0) {
                    knobs.exposure += static_cast<double>(correction) / 100.0;
                    proposal.correction_hundredths = correction;
                    changed = true;
                }
            }
            if (plan.balance) {
                knobs.kelvin = vote->second.kelvin;
                knobs.tint = vote->second.tint;
                proposal.balanced =
                    std::abs(knobs.kelvin - proposal.knobs_before.kelvin) >=
                        1.0 ||
                    std::abs(knobs.tint - proposal.knobs_before.tint) >= 0.001;
                changed = changed || proposal.balanced;
            }
            proposal.knobs_after = knobs;
            if (proposal.anchor_used == "none") proposal.anchor_used = "source";
            proposal.placements.clear();
            proposal.effects.clear();
            if (changed) {
                proposal.effects =
                    PlaceColorMatch(side.clip->effects, knobs, side.context,
                                    &proposal.placements);
                if (!MeasureSide(
                        side, ResolveColorGrade(proposal.effects, side.context),
                        region, proposal.after, error))
                    return false;
                proposal.reason =
                    limited ? "limited_by_highlights" : "per_source";
            } else {
                proposal.after = proposal.before;
                proposal.reason =
                    limited ? "limited_by_highlights" : "within_tolerance";
            }
        }
        framesAfter.clear();
        for (size_t index = 0; index < proposals.size(); ++index) {
            if (std::find(faceless.begin(), faceless.end(), index) !=
                faceless.end())
                continue;
            const GradeProposal& proposal = proposals[index];
            if (proposal.after.picture.scene &&
                proposal.after.picture.scene_p50)
                framesAfter.push_back(*proposal.after.picture.scene_p50);
        }
    }

    // A clip without a face follows the montage: its frame brought to the
    // median the faces left theirs at.
    if (!faceless.empty() && plan.exposure && !framesAfter.empty()) {
        std::sort(framesAfter.begin(), framesAfter.end());
        const int64_t montage = framesAfter[framesAfter.size() / 2];
        for (size_t index : faceless) {
            GradeProposal& proposal = proposals[index];
            const GradedSide& side = sides[index];
            const ColorPictureMeasure& frame = proposal.before.picture;
            if (!frame.scene || !frame.scene_p50) continue;
            proposal.anchor_used = "median";
            proposal.reason = "no_face_matched_montage";
            const int64_t wanted = montage - *frame.scene_p50;
            if (std::llabs(wanted) <= plan.tolerance_hundredths) continue;
            bool limited = false;
            int64_t correction = 0;
            if (!limitLift(side, proposal.knobs_before, wanted, frame.scene_p98,
                           frame.clipped, correction, limited))
                return false;
            if (limited) proposal.reason = "limited_by_highlights";
            if (correction == 0) continue;
            ColorMatchKnobs knobs = proposal.knobs_before;
            knobs.exposure += static_cast<double>(correction) / 100.0;
            proposal.knobs_after = knobs;
            proposal.correction_hundredths = correction;
            proposal.effects = PlaceColorMatch(
                side.clip->effects, knobs, side.context, &proposal.placements);
            if (!MeasureSide(side,
                             ResolveColorGrade(proposal.effects, side.context),
                             ColorMeasureRegion{}, proposal.after, error))
                return false;
        }
    }
    return true;
}

std::map<Ulid, SourceGradeVote> SourceGradeVotes(
    const std::vector<GradeProposal>& proposals) {
    std::map<Ulid, std::vector<const GradeProposal*>> bySource;
    for (const GradeProposal& proposal : proposals)
        if (proposal.anchor_used == "face" || proposal.anchor_used == "neutral")
            bySource[proposal.source_id].push_back(&proposal);
    std::map<Ulid, SourceGradeVote> votes;
    for (auto& [sourceId, anchored] : bySource) {
        std::vector<int64_t> corrections;
        std::vector<int64_t> wanteds;
        std::vector<double> kelvins;
        std::vector<double> tints;
        for (const GradeProposal* proposal : anchored) {
            corrections.push_back(proposal->correction_hundredths);
            wanteds.push_back(proposal->wanted_hundredths);
            kelvins.push_back(proposal->knobs_after.kelvin);
            tints.push_back(proposal->knobs_after.tint);
        }
        std::sort(corrections.begin(), corrections.end());
        std::sort(wanteds.begin(), wanteds.end());
        std::sort(kelvins.begin(), kelvins.end());
        std::sort(tints.begin(), tints.end());
        SourceGradeVote vote;
        vote.count = anchored.size();
        vote.correction_hundredths = corrections[corrections.size() / 2];
        vote.wanted_hundredths = wanteds[wanteds.size() / 2];
        vote.limited = vote.wanted_hundredths > vote.correction_hundredths;
        vote.kelvin = kelvins[kelvins.size() / 2];
        vote.tint = tints[tints.size() / 2];
        votes[sourceId] = vote;
    }
    return votes;
}

mcp_json::Value GradeProposalsValue(
    const Document& document, const GradeProposalPlan& plan,
    const std::vector<GradeProposal>& proposals) {
    Value reply = Value::MakeObject();
    reply.Set("anchor", Value::MakeString(plan.anchor));
    reply.Set("per_source", Value::MakeBool(plan.per_source));
    reply.Set("position", Value::MakeString(plan.position));
    reply.Set("exposure", Value::MakeBool(plan.exposure));
    reply.Set("balance", Value::MakeBool(plan.balance));
    reply.Set("target_stops", Hundredths(plan.target_hundredths));
    reply.Set("tolerance_stops", Hundredths(plan.tolerance_hundredths));
    // GR15 -- under an HLG output the ceiling is the output's clipping, and
    // no stop value would say what held a lift back.
    if (plan.display_ceiling)
        reply.Set("highlight_ceiling", Value::MakeString("display_clipping"));
    else
        reply.Set("highlight_ceiling_stops",
                  Hundredths(plan.highlight_ceiling_hundredths));
    reply.Set("chain", ChainValue(document));
    if (plan.region.present) reply.Set("region", RegionValue(plan.region));
    const auto number = [](double value, int digits) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(digits) << value;
        return Value::MakeRawNumber(stream.str());
    };
    const auto knob = [&](double before, double after, int digits) {
        Value pair = Value::MakeObject();
        pair.Set("before", number(before, digits));
        pair.Set("after", number(after, digits));
        return pair;
    };
    const auto reading = [&plan](const ColorClipMeasurement& measurement,
                                 bool anchored) {
        const ColorPictureMeasure& measure =
            anchored ? measurement.region : measurement.picture;
        Value out = Value::MakeObject();
        out.Set("anchor_stops", OptionalHundredths(measure.scene_p50));
        out.Set("skin_line_offset_degrees",
                OptionalHundredths(measure.skin_line_offset));
        out.Set("frame_stops",
                OptionalHundredths(measurement.picture.scene_p50));
        if (!plan.verbose) return out;
        out.Set("anchor_luma_percent", Hundredths(measure.luma_p50));
        const ColorZoneMeasure mean = MeanZone(measure);
        if (mean.pixels > 0) {
            out.Set("anchor_warmth", Hundredths(mean.warmth));
            out.Set("anchor_green_magenta", Hundredths(mean.green_magenta));
        }
        out.Set("frame_p98_stops",
                OptionalHundredths(measurement.picture.scene_p98));
        return out;
    };
    Value clips = Value::MakeArray();
    Value steps = Value::MakeArray();
    int64_t proposed = 0;
    std::vector<int64_t> after;
    for (const GradeProposal& proposal : proposals) {
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(proposal.clip_id));
        item.Set("source_id", Value::MakeString(proposal.source_id));
        item.Set("anchor_used", Value::MakeString(proposal.anchor_used));
        if (proposal.anchor_region.present)
            item.Set("anchor_region", RegionValue(proposal.anchor_region));
        item.Set("reason", Value::MakeString(proposal.reason));
        if (!proposal.error.empty())
            item.Set("error", Value::MakeString(proposal.error));
        item.Set("correction_stops",
                 Hundredths(proposal.correction_hundredths));
        item.Set("balanced", Value::MakeBool(proposal.balanced));
        // AX-2026-09 -- the knobs and the steps are what applying needs, and
        // apply: true does it; the reading is what judging needs. On 65
        // clips the reply weighed 57 KB with them, past what Claude Code
        // reads in one piece.
        if (plan.verbose) {
            Value knobs = Value::MakeObject();
            knobs.Set("exposure", knob(proposal.knobs_before.exposure,
                                       proposal.knobs_after.exposure, 3));
            knobs.Set("temperature_kelvin",
                      knob(proposal.knobs_before.kelvin,
                           proposal.knobs_after.kelvin, 0));
            knobs.Set("tint", knob(proposal.knobs_before.tint,
                                   proposal.knobs_after.tint, 3));
            item.Set("knobs", std::move(knobs));
        }
        const bool anchored = proposal.anchor_region.present;
        item.Set("before", reading(proposal.before, anchored));
        item.Set("after", reading(proposal.after, anchored));
        if (!proposal.effects.empty() && plan.verbose) {
            Value placements = Value::MakeArray();
            for (const ColorMatchPlacement& placement : proposal.placements) {
                Value placed = Value::MakeObject();
                placed.Set("type", Value::MakeString(placement.type));
                placed.Set("placement",
                           Value::MakeString(placement.adjusted ? "adjusted"
                                                                : "added"));
                placements.Push(std::move(placed));
            }
            item.Set("placements", std::move(placements));
        }
        if (!proposal.effects.empty()) {
            if (plan.verbose) {
                Value step = Value::MakeObject();
                step.Set("tool", Value::MakeString("set_clip_effects"));
                Value arguments = Value::MakeObject();
                arguments.Set("clip_id", Value::MakeString(proposal.clip_id));
                arguments.Set("effects", EffectsValue(proposal.effects));
                step.Set("arguments", std::move(arguments));
                steps.Push(std::move(step));
            }
            ++proposed;
        }
        if (proposal.after.picture.scene && proposal.after.picture.scene_p50)
            after.push_back(*proposal.after.picture.scene_p50);
        clips.Push(std::move(item));
    }
    reply.Set("clips", std::move(clips));
    if (plan.verbose) reply.Set("steps", std::move(steps));
    if (!after.empty()) {
        std::sort(after.begin(), after.end());
        Value summary = Value::MakeObject();
        summary.Set("clips_measured",
                    Value::MakeInt(static_cast<int64_t>(after.size())));
        summary.Set("frame_median_stops_after",
                    Hundredths(after[after.size() / 2]));
        summary.Set("frame_min_stops_after", Hundredths(after.front()));
        summary.Set("frame_max_stops_after", Hundredths(after.back()));
        reply.Set("summary", std::move(summary));
    }
    reply.Set("proposed", Value::MakeInt(proposed));
    reply.Set("skipped", Value::MakeInt(static_cast<int64_t>(proposals.size()) -
                                        proposed));
    return reply;
}
