#include "ExportVerification.h"

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "FfmpegPipe.h"
#include "Json.h"
#include "MotionClip.h"
#include "MotionDocument.h"
#include "MotionFrameRenderer.h"
#include "MotionParallelRender.h"
#include "SourceGrid.h"
#include "Timeline.h"
#include "Ulid.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

// Mean absolute difference between two z-normalized thumbnails. Normalizing
// first makes the comparison blind to a grade, a colour transform or an
// exposure change applied at export -- all affine or monotonic on luma --
// while a different picture still lands near 2/sqrt(pi), about 1.13.
constexpr double kUnrelatedDistance = 1.2;
// Below this deviation (8-bit luma) a thumbnail is flat: black, a fade or a
// wall, and z-normalizing it would only amplify noise.
constexpr double kFlatDeviation = 3.0;
// A shown frame farther than this from every searched source frame is not
// the expected shot at all.
constexpr double kWrongPictureDistance = 0.5;
// Measured on the ITW-2026-09 interview export (1 359 frames, HLG rushes,
// 36x64 thumbnails): the rendering alone puts the right source frame at a
// median 0.033 from the delivered one, more than two neighbouring source
// frames differ (median 0.022). No absolute threshold can separate those.
// What holds is the ordering: the right frame was the nearest in 99 % of
// frames, by a median 0.009, and a neighbour never won by more than 0.0015.
// A frame is therefore decided by its margin over the runner-up, at twice
// that worst inversion; below it, the frame is counted as ambiguous.
constexpr double kConfidentMargin = 0.003;
// REMOTION-PARITY-2026-09 REM6 -- two flat thumbnails are compared by their
// level (Distance), and the delivery moves a level by a code or two: the
// 10-bit encode, the 8-bit read, the matrix. On a flat picture changing by
// about one code of luma a frame, an exact export was reported shifted by
// one or two frames on every frame (28 September 2026, ITW-2026-09,
// « Constats »). Between flat frames, a side wins only by four codes.
constexpr double kFlatMargin = 4.0 / 64.0;
// An isolated frame (a repeat at a catch-up) needs a margin no inversion
// came near, since no neighbouring frame corroborates it.
constexpr double kSingleFrameMargin = 0.006;

struct Thumbnail {
    std::vector<float> normalized;
    double mean = 0.0;
    double deviation = 0.0;
};

Thumbnail MakeThumbnail(const uint8_t* pixels, size_t count) {
    Thumbnail thumbnail;
    double sum = 0.0;
    double squares = 0.0;
    for (size_t index = 0; index < count; ++index) {
        sum += pixels[index];
        squares += static_cast<double>(pixels[index]) * pixels[index];
    }
    thumbnail.mean = sum / static_cast<double>(count);
    thumbnail.deviation =
        std::sqrt(std::max(0.0, (squares / static_cast<double>(count)) -
                                    (thumbnail.mean * thumbnail.mean)));
    if (thumbnail.deviation >= kFlatDeviation) {
        thumbnail.normalized.resize(count);
        for (size_t index = 0; index < count; ++index) {
            thumbnail.normalized[index] = static_cast<float>(
                (pixels[index] - thumbnail.mean) / thumbnail.deviation);
        }
    }
    return thumbnail;
}

bool IsFlat(const Thumbnail& thumbnail) {
    return thumbnail.deviation < kFlatDeviation;
}

bool IsBlack(const Thumbnail& thumbnail) {
    return IsFlat(thumbnail) && thumbnail.mean <= 32.0;
}

// MOTION-DESIGN-2026-09 -- below twice the flat line a thumbnail is nearly
// flat: a thin line or a grid on black, which the delivery and the render
// can put on either side of that line. The teaser's intro was reported
// black at two frames whose render and file both measured a deviation of
// 3.0 +- 0.1 (28 September 2026).
bool IsNearlyFlat(const Thumbnail& thumbnail) {
    return thumbnail.deviation < 2.0 * kFlatDeviation;
}

// A picture no one could take for black: what a black frame is judged
// against.
bool IsClearlyPicture(const Thumbnail& thumbnail) {
    return !IsNearlyFlat(thumbnail) || thumbnail.mean > 48.0;
}

// MOTION-DESIGN-2026-09 -- `byLevel` is decided once per shown frame, from
// it and the expected frame, and holds for every candidate: a level gap
// over 64 and a mean normalized difference are two scales, and mixing them
// let a flat neighbour win by its level against the expected frame judged
// by its structure. The teaser's fade to black was reported shifted by one
// frame at 1361, where the delivered and expected frames stood at a
// deviation of 3.1 and 3.3 and the next one at 2.3 (28 September 2026).
double Distance(const Thumbnail& left, const Thumbnail& right, bool byLevel) {
    if (byLevel) {
        // The same faint picture on either side of the flat line is
        // compared by level, not called unrelated.
        return std::min(kUnrelatedDistance,
                        std::abs(left.mean - right.mean) / 64.0);
    }
    if (IsFlat(left) || IsFlat(right)) {
        return kUnrelatedDistance;
    }
    double sum = 0.0;
    const size_t count = left.normalized.size();
    for (size_t index = 0; index < count; ++index) {
        sum += std::abs(left.normalized[index] - right.normalized[index]);
    }
    return sum / static_cast<double>(count);
}

enum class Expectation { Black, Picture, Unverifiable };

struct ExpectedFrame {
    Expectation kind = Expectation::Black;
    ResolvedFrame frame;
    // R1 -- docs/roadmap/REEL-2026-09.md. Which decode this frame is
    // compared against: a source is decoded once per framing used on it,
    // because two clips of one rush framed differently do not show the
    // same picture at the same source frame.
    std::string decode_key;
    // REM6 -- a motion clip alone on its frame: `frame` names its
    // composition and a frame of it, as Timeline resolves them.
    bool motion = false;
};

// R1 -- the thumbnails of one source as one framing composes it.
struct SourceDecode {
    Ulid source_id;
    std::string filter;
    // A framed or animated clip: its thumbnails are cropped and scaled by
    // RenderFramedThumbnail from one working-size decode of the rush, and
    // `filter` only names the framing.
    bool framed = false;
    ClipFramingGeometry framing;
    int64_t displayed_width = 0;
    int64_t displayed_height = 0;
    std::set<int64_t> frames;
    std::map<int64_t, Thumbnail> thumbnails;
    // REM6 -- the frames of a motion clip's composition, rendered rather
    // than decoded (DecodeMotionThumbnails); source_id is the composition.
    bool motion = false;
    Ulid clip_id;
};

// Long edge of the working-size decode framed thumbnails are cut from:
// sixteen times the thumbnail's, so a crop of a sixteenth of the picture
// still has a pixel for every thumbnail pixel.
constexpr int32_t kWorkingScale = 16;

// Needed frames further apart than this many seconds of the rush are
// decoded as two runs, each after its own seek, rather than through every
// frame in between.
constexpr int64_t kRunGapSeconds = 2;

std::vector<std::pair<int64_t, int64_t>> FrameRuns(
    const std::set<int64_t>& frames, int64_t gap) {
    std::vector<std::pair<int64_t, int64_t>> runs;
    for (const int64_t frame : frames) {
        if (runs.empty() || frame - runs.back().second > gap)
            runs.push_back({frame, frame});
        else
            runs.back().second = frame;
    }
    return runs;
}

// R1/R2 -- the thumbnail a framed clip shows, from a decode of its rush at
// workWidth x workHeight: the framing's source region, area-averaged into
// the rectangle it is drawn in, on the black FFmpeg gives that rush. Done
// here rather than by an FFmpeg crop per framing because an animated clip
// has a framing per timeline frame, and each was a decode of the rush.
std::vector<uint8_t> RenderFramedThumbnail(
    const uint8_t* work, int32_t workWidth, int32_t workHeight, uint8_t black,
    const ClipFramingGeometry& framing, int64_t displayedWidth,
    int64_t displayedHeight, int32_t thumbnailWidth, int32_t thumbnailHeight) {
    std::vector<uint8_t> thumbnail(static_cast<size_t>(thumbnailWidth) *
                                       static_cast<size_t>(thumbnailHeight),
                                   black);
    if (framing.width <= 0 || framing.height <= 0 || displayedWidth <= 0 ||
        displayedHeight <= 0 || framing.source_width <= 0 ||
        framing.source_height <= 0)
        return thumbnail;
    const double toWorkX =
        static_cast<double>(workWidth) / static_cast<double>(displayedWidth);
    const double toWorkY =
        static_cast<double>(workHeight) / static_cast<double>(displayedHeight);
    const double left = static_cast<double>(framing.source_x) * toWorkX;
    const double top = static_cast<double>(framing.source_y) * toWorkY;
    const double spanX = static_cast<double>(framing.source_width) * toWorkX /
                         static_cast<double>(framing.width);
    const double spanY = static_cast<double>(framing.source_height) * toWorkY /
                         static_cast<double>(framing.height);
    // The work pixels [begin, end) covers, with how much of each.
    const auto cover = [](double begin, double end, int32_t limit,
                          std::vector<std::pair<int32_t, double>>& weights) {
        weights.clear();
        begin = std::clamp(begin, 0.0, static_cast<double>(limit));
        end = std::clamp(end, begin, static_cast<double>(limit));
        for (auto pixel = static_cast<int32_t>(std::floor(begin));
             pixel < limit && static_cast<double>(pixel) < end; ++pixel) {
            const double weight =
                std::min(end, static_cast<double>(pixel) + 1.0) -
                std::max(begin, static_cast<double>(pixel));
            if (weight > 0.0) weights.push_back({pixel, weight});
        }
    };
    std::vector<std::vector<std::pair<int32_t, double>>> columns(
        static_cast<size_t>(framing.width));
    for (int64_t column = 0; column < framing.width; ++column)
        cover(left + static_cast<double>(column) * spanX,
              left + static_cast<double>(column + 1) * spanX, workWidth,
              columns[static_cast<size_t>(column)]);
    std::vector<std::pair<int32_t, double>> rows;
    std::vector<double> accumulated(static_cast<size_t>(framing.width));
    for (int64_t row = 0; row < framing.height; ++row) {
        const int64_t y = framing.y + row;
        if (y < 0 || y >= thumbnailHeight) continue;
        cover(top + static_cast<double>(row) * spanY,
              top + static_cast<double>(row + 1) * spanY, workHeight, rows);
        std::fill(accumulated.begin(), accumulated.end(), 0.0);
        double rowWeight = 0.0;
        for (const auto& [workRow, weightY] : rows) {
            rowWeight += weightY;
            const uint8_t* line = work + static_cast<size_t>(workRow) *
                                             static_cast<size_t>(workWidth);
            for (int64_t column = 0; column < framing.width; ++column) {
                double sum = 0.0;
                for (const auto& [workColumn, weightX] :
                     columns[static_cast<size_t>(column)])
                    sum += weightX * line[workColumn];
                accumulated[static_cast<size_t>(column)] += weightY * sum;
            }
        }
        for (int64_t column = 0; column < framing.width; ++column) {
            const int64_t x = framing.x + column;
            if (x < 0 || x >= thumbnailWidth) continue;
            double columnWeight = 0.0;
            for (const auto& entry : columns[static_cast<size_t>(column)])
                columnWeight += entry.second;
            const double area = rowWeight * columnWeight;
            if (area <= 0.0) continue;
            thumbnail[static_cast<size_t>(y) *
                          static_cast<size_t>(thumbnailWidth) +
                      static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::clamp(
                    std::lround(accumulated[static_cast<size_t>(column)] /
                                area),
                    0L, 255L));
        }
    }
    return thumbnail;
}

std::string RateString(MediaRate rate) {
    return std::to_string(rate.num) + '/' + std::to_string(rate.den);
}

std::filesystem::path ResolvePath(const std::filesystem::path& directory,
                                  const std::string& path) {
    std::filesystem::path resolved(path);
    if (resolved.is_relative()) resolved = directory / resolved;
    return std::filesystem::absolute(resolved).lexically_normal();
}

// REMOTION-PARITY-2026-09 REM6 -- the thumbnails of a motion clip: the
// composition frames the engine renders for it, asked for as the export asks
// (RenderMotionAsset: the clip's props, the project's directory and what the
// composition reaches), written as a sequence numbered by composition frame
// and reduced by FFmpeg with the clip's `filter`. Premultiplied first: that
// is the frame laid over the black of an empty composite, which is all a
// motion clip alone on its frame shows.
bool DecodeMotionThumbnails(const Document& document,
                            const std::filesystem::path& projectDirectory,
                            const ExportVerificationSettings& settings,
                            size_t frameBytes, SourceDecode& decode,
                            std::string& error) {
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    const DocumentClip* clip = document.FindClip(decode.clip_id);
    const DocumentComposition* stored =
        document.FindComposition(decode.source_id);
    if (render == nullptr || clip == nullptr || stored == nullptr) {
        error = "motion clip '" + decode.clip_id + "' cannot be rendered";
        return false;
    }
    MotionComposition composition;
    MotionFrameRequest request;
    MotionError motionError;
    if (!ParseDocumentComposition(*stored, composition, motionError) ||
        !MotionReachFor(document, stored->id, request.reach, motionError)) {
        error = "composition '" + stored->id + "': " + motionError.code + ": " +
                motionError.message;
        return false;
    }
    request.composition = &composition;
    request.props = clip->motion_props;
    request.media_directory = projectDirectory.string();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-verify-motion-" + GenerateUlid());
    std::error_code status;
    std::filesystem::create_directories(directory, status);
    if (status) {
        error = "unable to create '" + directory.string() + "'";
        return false;
    }
    const auto fail = [&](const std::string& message) {
        error = message;
        std::filesystem::remove_all(directory, status);
        return false;
    };
    // A frame the search radius reaches past either end of the composition
    // is simply not a candidate.
    std::set<int64_t> frames;
    for (const int64_t frame : decode.frames) {
        if (frame >= 0 && frame < composition.duration) {
            frames.insert(frame);
        }
    }
    // MOTION-DESIGN-2026-09 -- on every core, as the export renders them.
    std::string renderError;
    if (!RenderMotionFramesToFiles(
            request, std::vector<int64_t>(frames.begin(), frames.end()),
            [&](int64_t frame) {
                char name[32];
                std::snprintf(name, sizeof(name), "%06lld.png",
                              static_cast<long long>(frame));
                return (directory / name).string();
            },
            nullptr, renderError)) {
        return fail("composition '" + stored->id + "' " + renderError);
    }
    const std::string rate = std::to_string(composition.fps_numerator) + '/' +
                             std::to_string(composition.fps_denominator);
    for (const auto& [first, last] : FrameRuns(frames, 1)) {
        int64_t index = first;
        if (!ReadFfmpegRawFrames(
                {settings.ffmpeg_path,
                 "-hide_banner",
                 "-loglevel",
                 "error",
                 "-nostdin",
                 "-f",
                 "image2",
                 "-framerate",
                 rate,
                 "-start_number",
                 std::to_string(first),
                 "-i",
                 (directory / "%06d.png").string(),
                 "-frames:v",
                 std::to_string(last - first + 1),
                 "-vf",
                 "premultiply=inplace=1," + decode.filter + ",format=gray",
                 "-f",
                 "rawvideo",
                 "-pix_fmt",
                 "gray",
                 "pipe:1"},
                frameBytes,
                [&](const uint8_t* pixels) {
                    decode.thumbnails.emplace(
                        index++, MakeThumbnail(pixels, frameBytes));
                },
                error)) {
            return fail("unable to reduce the frames of composition '" +
                        stored->id + "': " + error);
        }
    }
    std::filesystem::remove_all(directory, status);
    return true;
}

}  // namespace

bool VerifyExport(const Document& document,
                  const std::filesystem::path& projectDirectory,
                  const std::string& exportPath,
                  const ExportVerificationSettings& settings,
                  ExportVerificationReport& report, std::string& error) {
    report = {};
    error.clear();
    const MediaRate rate = document.sequence.frame_rate;
    const int32_t width = document.sequence.width;
    const int32_t height = document.sequence.height;
    if (rate.num <= 0 || rate.den <= 0 || width <= 0 || height <= 0 ||
        settings.thumbnail_dimension < 8 || settings.search_radius < 1) {
        error = "invalid sequence format or verification settings";
        return false;
    }
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(exportPath, filesystemError)) {
        error = "exported file does not exist: " + exportPath;
        return false;
    }
    report.timeline_id = document.sequence.id;
    report.frame_rate = rate;

    std::vector<ExpectedFrame> expected;
    try {
        const Timeline timeline(document);
        const RationalTime duration = timeline.Duration();
        const __int128 numerator =
            static_cast<__int128>(duration.value) * rate.num;
        const __int128 denominator =
            static_cast<__int128>(duration.rate) * rate.den;
        if (numerator % denominator != 0) {
            error = "timeline duration must contain whole sequence frames";
            return false;
        }
        report.expected_frames = static_cast<int64_t>(numerator / denominator);
        std::vector<const DocumentTrack*> tracks;
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind == "video" && track.visible)
                tracks.push_back(&track);
        }
        // Export composites in ascending index, so the highest is on top.
        std::stable_sort(
            tracks.begin(), tracks.end(),
            [](const DocumentTrack* left, const DocumentTrack* right) {
                return left->index > right->index;
            });
        expected.resize(static_cast<size_t>(report.expected_frames));
        for (int64_t frame = 0; frame < report.expected_frames; ++frame) {
            const RationalTime position{frame * rate.den, rate.num};
            ExpectedFrame& slot = expected[static_cast<size_t>(frame)];
            if (!settings.only_frames.empty() &&
                settings.only_frames.count(frame) == 0) {
                slot.kind = Expectation::Unverifiable;
                continue;
            }
            for (size_t level = 0; level < tracks.size(); ++level) {
                const DocumentTrack* track = tracks[level];
                const std::vector<ResolvedLayer> layers =
                    timeline.ResolveTrackLayers(track->id, position);
                if (layers.empty()) continue;
                // MOTION-2026-09 M4 -- a motion clip is drawn with its own
                // alpha over what lies under it, and it has no media to
                // compare with: its frames are counted unverifiable.
                const DocumentClip* shown =
                    document.FindClip(layers.front().frame.clip_id);
                const bool motion = shown != nullptr && IsMotionClip(*shown);
                // R2 -- a clip whose opacity moves is not a picture laid
                // straight down, and Timeline reports its constant opacity,
                // not the frame's. Counted unverifiable rather than
                // compared against a picture it is no longer showing at
                // full strength: a wrong verdict is worse than none
                // (guard rail 5).
                const bool fading =
                    shown != nullptr &&
                    ClipAnimates(*shown, ClipAnimatedParameter::Opacity);
                // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred clip is
                // delivered as a picture its source thumbnail is not: at the
                // thumbnails' size a strong blur reads as another shot, a
                // light one passes or fails by chance. Counted unverifiable,
                // for the reason a fading clip is.
                // PA8 -- and a masked one: its thumbnail is the whole
                // picture, the delivery what the mask keeps of it over
                // the tracks below.
                const bool blurred = shown != nullptr && (ClipHasBlur(*shown) ||
                                                          ClipHasMask(*shown));
                // R5 -- docs/roadmap/REEL-2026-09.md. A colour dip over
                // this track, by the one function the monitor and the
                // export also ask. A frame the veil covers entirely is
                // the colour and nothing else, so a black dip at full
                // strength is checked as black -- the opening and the
                // closing of a film are verified, not excused. Anywhere
                // else on the ramp the delivered picture is a blend the
                // source thumbnails cannot be compared against, and a
                // wrong verdict is worse than none (guard rail 5).
                const std::optional<ResolvedColorVeil> veil =
                    timeline.ResolveTrackColorVeil(track->id, position);
                const bool fullyVeiled = veil && veil->alpha.den > 0 &&
                                         veil->alpha.num == veil->alpha.den;
                const bool veiled = veil && veil->alpha.num != 0;
                const bool plainPicture = !veiled && layers.size() == 1 &&
                                          layers.front().opacity >= 1.0f &&
                                          !motion && !fading && !blurred;
                // REMOTION-PARITY-2026-09 REM6 -- a motion clip with no
                // picture under it is laid over the composite's black, so
                // its rendered frame over black is the whole picture. Over
                // another one, its alpha lets that picture through, which
                // these thumbnails do not composite: unverifiable, as
                // before. A process without the renderer cannot say either.
                bool alone = motion && CurrentMotionFrameRenderer() != nullptr;
                for (size_t below = level + 1; alone && below < tracks.size();
                     ++below) {
                    alone =
                        timeline.ResolveTrackLayers(tracks[below]->id, position)
                            .empty();
                }
                const bool plainMotion = !veiled && layers.size() == 1 &&
                                         layers.front().opacity >= 1.0f &&
                                         alone && !fading;
                if (fullyVeiled && veil->color == TransitionColor{0, 0, 0}) {
                    slot.kind = Expectation::Black;
                } else if (plainPicture || plainMotion) {
                    slot.kind = Expectation::Picture;
                    slot.frame = layers.front().frame;
                    slot.motion = plainMotion;
                } else {
                    slot.kind = Expectation::Unverifiable;
                }
                break;
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }

    const double scale = static_cast<double>(settings.thumbnail_dimension) /
                         std::max(width, height);
    const int32_t thumbnailWidth =
        std::max<int32_t>(4, static_cast<int32_t>(std::lround(width * scale)));
    const int32_t thumbnailHeight =
        std::max<int32_t>(4, static_cast<int32_t>(std::lround(height * scale)));
    const size_t frameBytes = static_cast<size_t>(thumbnailWidth) *
                              static_cast<size_t>(thumbnailHeight);
    const std::string geometry =
        std::to_string(thumbnailWidth) + ':' + std::to_string(thumbnailHeight);

    // The file as encoded: no cadence conversion, so frame n is the n-th
    // picture a player shows.
    std::vector<Thumbnail> shown;
    // R8 -- measured from the same pixels, in the same pass: the mean and
    // the luma histogram of every delivered frame, which DetectIsolatedFrames
    // reads below.
    IsolatedFrameSeries delivered;
    if (!ReadFfmpegRawFrames(
            {settings.ffmpeg_path,
             "-hide_banner",
             "-loglevel",
             "error",
             "-nostdin",
             "-i",
             exportPath,
             "-map",
             "0:v:0",
             "-an",
             "-sn",
             "-fps_mode",
             "passthrough",
             "-vf",
             "scale=" + geometry + ":flags=area,format=gray",
             "-f",
             "rawvideo",
             "-pix_fmt",
             "gray",
             "pipe:1"},
            frameBytes,
            [&](const uint8_t* pixels) {
                // A confirmation pass keeps the few frames it looks at.
                const auto frame = static_cast<int64_t>(shown.size());
                shown.push_back(settings.only_frames.empty() ||
                                        settings.only_frames.count(frame) != 0
                                    ? MakeThumbnail(pixels, frameBytes)
                                    : Thumbnail{});
                IsolatedFrameMeasurement measurement;
                measurement.mean_luma =
                    FrameMeanLuma(pixels, thumbnailWidth, thumbnailHeight);
                FrameLumaHistogram(pixels, thumbnailWidth, thumbnailHeight,
                                   measurement.histogram.data());
                delivered.frames.push_back(measurement);
            },
            error)) {
        error = "unable to decode the exported file: " + error;
        return false;
    }
    report.exported_frames = static_cast<int64_t>(shown.size());
    report.isolated_frames =
        DetectIsolatedFrames(delivered, settings.isolated_frames);

    // Needed source frames, searched radius included, per source and per
    // framing: R1 -- the same rush framed two ways is two decodes.
    std::map<std::string, SourceDecode> decodes;
    for (size_t index = 0; index < expected.size(); ++index) {
        ExpectedFrame& slot = expected[index];
        if (slot.kind != Expectation::Picture) continue;
        // R1 -- the thumbnail grid is the sequence frame at another size,
        // so the rectangle comes from the same function the monitor, the
        // export and the sheets call, resolved against that grid.
        std::string filter =
            "scale=" + geometry +
            ":force_original_aspect_ratio=decrease:flags=area,pad=" + geometry +
            ":(ow-iw)/2:(oh-ih)/2:black";
        std::string key = slot.frame.source_id;
        const DocumentClip* clip = document.FindClip(slot.frame.clip_id);
        // REM6 -- a motion clip is fitted, never framed, by the export
        // (Export.cc): its frames take the plain fit above, and each clip
        // is its own render, its props being its own.
        if (slot.motion) {
            key = "motion|" + slot.frame.clip_id;
        }
        if (!slot.motion && clip != nullptr &&
            (ClipHasFraming(*clip) || ClipHasAnimation(*clip))) {
            ClipFramingGeometry framing;
            int64_t displayedWidth = 0;
            int64_t displayedHeight = 0;
            // R2 -- the framing of THIS timeline frame. An animated clip
            // shows a different rectangle every frame, so it is a decode
            // per frame; the values come from the one function the export
            // sampled from, so a divergence between the two is exactly
            // what this comparison catches (guard rail 1).
            ClipSampledParameters sampled;
            sampled.framing = clip->framing;
            if (ClipHasAnimation(*clip)) {
                const RationalTime position{
                    static_cast<int64_t>(index) * rate.den, rate.num};
                const RationalTime offset = position.sub(clip->timeline_in);
                if (!SampleClipParameters(*clip,
                                          offset.to_frames(rate.num, rate.den),
                                          rate, sampled, error)) {
                    error = "clip '" + clip->id + "': " + error;
                    return false;
                }
            }
            if (!ClipDisplayedSize(document, *clip, displayedWidth,
                                   displayedHeight, error) ||
                !ResolveClipFramingReduced(
                    sampled.framing, displayedWidth, displayedHeight,
                    document.sequence.width, document.sequence.height,
                    thumbnailWidth, thumbnailHeight, framing, error)) {
                error = "clip '" + clip->id + "': " + error;
                return false;
            }
            const std::string crop =
                ClipFramingCropFilter(framing, displayedWidth, displayedHeight);
            filter = (crop.empty() ? std::string() : crop + ",") +
                     "scale=" + std::to_string(framing.width) + ':' +
                     std::to_string(framing.height) +
                     ":flags=area,pad=" + geometry + ':' +
                     std::to_string(framing.x) + ':' +
                     std::to_string(framing.y) + ":black";
            key += '|' + filter;
            SourceDecode& decode = decodes[key];
            decode.framed = true;
            decode.framing = framing;
            decode.displayed_width = displayedWidth;
            decode.displayed_height = displayedHeight;
        }
        slot.decode_key = key;
        SourceDecode& decode = decodes[key];
        decode.source_id = slot.frame.source_id;
        decode.filter = filter;
        decode.motion = slot.motion;
        decode.clip_id = slot.frame.clip_id;
        for (int64_t offset = -settings.search_radius;
             offset <= settings.search_radius; ++offset) {
            if (slot.frame.source_frame + offset >= 0)
                decode.frames.insert(slot.frame.source_frame + offset);
        }
    }
    // verify_export took 21 minutes on a 78 s reel of 64 4K clips (24
    // September 2026): every decode read its rush from the first frame up to
    // the last one needed, and an animated clip, whose framing changes every
    // frame, was one such decode per timeline frame. A rush is now decoded
    // once per run of needed frames, after a seek a second ahead of the run,
    // and framed thumbnails are cut from one working-size decode of it.
    //
    // The frames are still numbered on the rush's own grid, anchored on its
    // first picture (FirstPicturePts), with timestamps kept as the file
    // has them: where the seek lands decides only how much is decoded for
    // nothing, never which frame is which. A verification that trusted the
    // seek would share the exporter's input seek, which is what it checks.
    std::map<Ulid, std::vector<SourceDecode*>> bySource;
    for (auto& entry : decodes) {
        if (entry.second.motion) {
            if (!DecodeMotionThumbnails(document, projectDirectory, settings,
                                        frameBytes, entry.second, error)) {
                return false;
            }
            continue;
        }
        bySource[entry.second.source_id].push_back(&entry.second);
    }
    for (auto& [sourceId, sourceDecodes] : bySource) {
        const DocumentSource* source = document.FindSource(sourceId);
        if (source == nullptr || source->rate.num <= 0 ||
            source->rate.den <= 0) {
            error = "clip references an unknown source or source rate";
            return false;
        }
        const std::string path =
            ResolvePath(projectDirectory, source->path).string();
        int64_t firstPts = 0;
        if (!FirstPicturePts(path, firstPts, error)) {
            error = "unable to decode source '" + sourceId + "': " + error;
            return false;
        }
        const int64_t gap = std::max<int64_t>(
            1, kRunGapSeconds * source->rate.num / source->rate.den);
        const auto decodeRuns =
            [&](const std::set<int64_t>& frames, const std::string& filter,
                size_t bytes,
                const std::function<void(int64_t, const uint8_t*)>& onFrame) {
                for (const auto& [first, last] : FrameRuns(frames, gap)) {
                    // VF1 -- the grid read the export makes too
                    // (SourceGrid.h), so the two cannot name a picture
                    // differently.
                    const SourceGridRead read = SourceGridReadFor(
                        firstPts, source->rate, first, last, false);
                    std::vector<std::string> arguments = {
                        settings.ffmpeg_path, "-hide_banner", "-loglevel",
                        "error", "-nostdin"};
                    arguments.insert(arguments.end(),
                                     read.input_arguments.begin(),
                                     read.input_arguments.end());
                    arguments.insert(
                        arguments.end(),
                        {"-i", path, "-map", "0:v:0", "-an", "-sn", "-vf",
                         read.filter + ',' + filter + ",format=gray",
                         "-fps_mode", "passthrough", "-frames:v",
                         std::to_string(last - first + 1), "-f", "rawvideo",
                         "-pix_fmt", "gray", "pipe:1"});
                    int64_t index = first;
                    if (!ReadFfmpegRawFrames(
                            arguments, bytes,
                            [&](const uint8_t* pixels) {
                                onFrame(index++, pixels);
                            },
                            error))
                        return false;
                }
                return true;
            };
        for (SourceDecode* decode : sourceDecodes) {
            if (decode->framed) continue;
            if (!decodeRuns(decode->frames, decode->filter, frameBytes,
                            [&](int64_t index, const uint8_t* pixels) {
                                if (decode->frames.count(index) != 0)
                                    decode->thumbnails.emplace(
                                        index,
                                        MakeThumbnail(pixels, frameBytes));
                            })) {
                error = "unable to decode source '" + sourceId + "': " + error;
                return false;
            }
        }
        std::map<int64_t, std::vector<SourceDecode*>> wanted;
        for (SourceDecode* decode : sourceDecodes)
            if (decode->framed)
                for (const int64_t frame : decode->frames)
                    wanted[frame].push_back(decode);
        if (wanted.empty()) continue;
        const SourceDecode& sample = *wanted.begin()->second.front();
        const int64_t longEdge =
            std::max(sample.displayed_width, sample.displayed_height);
        const double workScale =
            std::min(1.0, static_cast<double>(settings.thumbnail_dimension) *
                              kWorkingScale / static_cast<double>(longEdge));
        const auto even = [](double value) {
            return std::max<int32_t>(
                2, static_cast<int32_t>(std::lround(value / 2.0)) * 2);
        };
        const int32_t workWidth =
            even(static_cast<double>(sample.displayed_width) * workScale);
        const int32_t workHeight =
            even(static_cast<double>(sample.displayed_height) * workScale);
        // Two rows of FFmpeg's own black under the picture: the colour a
        // pad gives this rush once made gray, which is what the delivered
        // file shows where the framing leaves the frame empty.
        const std::string workFilter =
            "scale=" + std::to_string(workWidth) + ':' +
            std::to_string(workHeight) +
            ":flags=area,pad=" + std::to_string(workWidth) + ':' +
            std::to_string(workHeight + 2) + ":0:0:black";
        const size_t workBytes = static_cast<size_t>(workWidth) *
                                 static_cast<size_t>(workHeight + 2);
        std::set<int64_t> workFrames;
        for (const auto& entry : wanted) workFrames.insert(entry.first);
        if (!decodeRuns(workFrames, workFilter, workBytes,
                        [&](int64_t index, const uint8_t* pixels) {
                            const auto found = wanted.find(index);
                            if (found == wanted.end()) return;
                            const uint8_t black =
                                pixels[static_cast<size_t>(workWidth) *
                                       static_cast<size_t>(workHeight)];
                            for (SourceDecode* decode : found->second) {
                                const std::vector<uint8_t> thumbnail =
                                    RenderFramedThumbnail(
                                        pixels, workWidth, workHeight, black,
                                        decode->framing,
                                        decode->displayed_width,
                                        decode->displayed_height,
                                        thumbnailWidth, thumbnailHeight);
                                decode->thumbnails.emplace(
                                    index, MakeThumbnail(thumbnail.data(),
                                                         thumbnail.size()));
                            }
                        })) {
            error = "unable to decode source '" + sourceId + "': " + error;
            return false;
        }
    }

    // First pass: what each frame shows, relative to what it should show.
    enum class Verdict { Skip, Match, Undecided, Offset, Divergent };
    struct FrameVerdict {
        Verdict verdict = Verdict::Skip;
        std::string kind;
        int64_t offset = 0;
        double margin = 0.0;
    };
    const int64_t comparable =
        std::min(report.expected_frames, report.exported_frames);
    std::vector<FrameVerdict> verdicts(static_cast<size_t>(comparable));
    for (int64_t frame = 0; frame < comparable; ++frame) {
        const ExpectedFrame& slot = expected[static_cast<size_t>(frame)];
        const Thumbnail& picture = shown[static_cast<size_t>(frame)];
        FrameVerdict& verdict = verdicts[static_cast<size_t>(frame)];
        if (slot.kind == Expectation::Unverifiable) {
            ++report.unverifiable_frames;
            continue;
        }
        if (slot.kind == Expectation::Black) {
            ++report.checked_frames;
            verdict.verdict =
                IsBlack(picture) ? Verdict::Match : Verdict::Divergent;
            verdict.kind = "picture_in_gap";
            continue;
        }
        const std::map<int64_t, Thumbnail>& decoded =
            decodes[slot.decode_key].thumbnails;
        const auto expectedSource = decoded.find(slot.frame.source_frame);
        if (expectedSource == decoded.end()) {
            // The source ended before the frame the document asks for.
            ++report.unverifiable_frames;
            continue;
        }
        ++report.checked_frames;
        if (slot.motion) {
            ++report.motion_checked_frames;
        }
        if (IsBlack(picture) && IsClearlyPicture(expectedSource->second)) {
            verdict.verdict = Verdict::Divergent;
            verdict.kind = "black_frame";
            continue;
        }
        const bool flat =
            IsNearlyFlat(picture) && IsNearlyFlat(expectedSource->second);
        const double atExpected =
            Distance(picture, expectedSource->second, flat);
        int64_t bestOffset = 0;
        double best = atExpected;
        double bestOther = std::numeric_limits<double>::infinity();
        for (int64_t offset = -settings.search_radius;
             offset <= settings.search_radius; ++offset) {
            if (offset == 0) continue;
            const auto candidate =
                decoded.find(slot.frame.source_frame + offset);
            if (candidate == decoded.end()) continue;
            const double distance = Distance(picture, candidate->second, flat);
            bestOther = std::min(bestOther, distance);
            if (distance < best ||
                (distance == best && std::abs(offset) < std::abs(bestOffset))) {
                best = distance;
                bestOffset = offset;
            }
        }
        if (best > kWrongPictureDistance) {
            verdict.verdict = Verdict::Divergent;
            verdict.kind = "wrong_picture";
            continue;
        }
        verdict.offset = bestOffset;
        verdict.margin =
            bestOffset == 0 ? bestOther - atExpected : atExpected - best;
        if (verdict.margin < (flat ? kFlatMargin : kConfidentMargin)) {
            verdict.verdict = Verdict::Undecided;
            ++report.ambiguous_frames;
        } else {
            verdict.verdict =
                bestOffset == 0 ? Verdict::Match : Verdict::Offset;
        }
    }

    // Second pass: runs. A frame-level offset is only evidence; a shift is
    // reported when consecutive frames of one clip agree on it, undecided
    // frames of a still stretch neither extending nor breaking the run.
    const auto recordRun = [&](const std::string& kind, int64_t first,
                               int64_t last, int64_t offset) {
        const ExpectedFrame& slot = expected[static_cast<size_t>(first)];
        ExportDivergence divergence;
        divergence.kind = kind;
        divergence.first_frame = first;
        divergence.last_frame = last;
        divergence.clip_id = slot.frame.clip_id;
        divergence.source_id = slot.frame.source_id;
        divergence.expected_source_frame = slot.frame.source_frame;
        divergence.offset = offset;
        divergence.motion = slot.motion;
        report.divergent_frames += last - first + 1;
        report.divergences.push_back(std::move(divergence));
    };
    struct OpenRun {
        bool open = false;
        std::string kind;
        Ulid clip_id;
        int64_t offset = 0;
        int64_t first = 0;
        int64_t last = 0;
        int64_t confident = 0;
        double margin = 0.0;
    } run;
    const auto closeRun = [&]() {
        if (!run.open) return;
        if (run.kind != "offset" || run.confident >= 2 ||
            run.margin >= kSingleFrameMargin) {
            recordRun(run.kind, run.first, run.last, run.offset);
        }
        run = {};
    };
    for (int64_t frame = 0; frame < comparable; ++frame) {
        const FrameVerdict& verdict = verdicts[static_cast<size_t>(frame)];
        const Ulid& clipId = expected[static_cast<size_t>(frame)].frame.clip_id;
        if (verdict.verdict == Verdict::Undecided) {
            if (run.open && run.clip_id != clipId) closeRun();
            continue;
        }
        const bool extends =
            run.open && run.clip_id == clipId &&
            ((verdict.verdict == Verdict::Offset && run.kind == "offset" &&
              run.offset == verdict.offset) ||
             (verdict.verdict == Verdict::Divergent &&
              run.kind == verdict.kind && run.last + 1 == frame));
        if (extends) {
            run.last = frame;
            ++run.confident;
            run.margin = std::max(run.margin, verdict.margin);
            continue;
        }
        closeRun();
        if (verdict.verdict == Verdict::Offset ||
            verdict.verdict == Verdict::Divergent) {
            run.open = true;
            run.kind =
                verdict.verdict == Verdict::Offset ? "offset" : verdict.kind;
            run.clip_id = clipId;
            run.offset = verdict.offset;
            run.first = frame;
            run.last = frame;
            run.confident = 1;
            run.margin = verdict.margin;
        }
    }
    closeRun();

    // The closer look at isolated one-frame offsets (settings above).
    std::set<int64_t> around;
    for (const ExportDivergence& divergence : report.divergences) {
        if (divergence.kind != "offset" ||
            divergence.first_frame != divergence.last_frame)
            continue;
        for (int64_t frame = divergence.first_frame - 2;
             frame <= divergence.first_frame + 2; ++frame)
            if (frame >= 0) around.insert(frame);
    }
    if (!settings.confirm_single_offsets || around.empty()) return true;
    ExportVerificationSettings closer = settings;
    closer.thumbnail_dimension = settings.thumbnail_dimension * 4;
    closer.confirm_single_offsets = false;
    closer.only_frames = around;
    ExportVerificationReport look;
    if (!VerifyExport(document, projectDirectory, exportPath, closer, look,
                      error))
        return false;
    std::vector<ExportDivergence> kept;
    for (ExportDivergence& divergence : report.divergences) {
        const bool single = divergence.kind == "offset" &&
                            divergence.first_frame == divergence.last_frame;
        const bool confirmed =
            !single ||
            std::any_of(look.divergences.begin(), look.divergences.end(),
                        [&](const ExportDivergence& other) {
                            return other.kind == "offset" &&
                                   other.offset == divergence.offset &&
                                   other.first_frame <=
                                       divergence.first_frame &&
                                   other.last_frame >= divergence.first_frame;
                        });
        if (confirmed) {
            kept.push_back(std::move(divergence));
        } else {
            --report.divergent_frames;
            ++report.unconfirmed_offset_frames;
        }
    }
    report.divergences = std::move(kept);
    return true;
}

bool ExportVerificationIsExact(const ExportVerificationReport& report) {
    return report.expected_frames > 0 &&
           report.exported_frames == report.expected_frames &&
           report.checked_frames == report.expected_frames &&
           report.unverifiable_frames == 0 && report.divergent_frames == 0;
}

namespace {

// checked / expected, floored to three decimals and written from integers:
// "1" only when every expected frame was compared, never 149 of 150 rounded
// up.
std::string VerifiedFractionText(const ExportVerificationReport& report) {
    if (report.expected_frames <= 0 || report.checked_frames <= 0) return "0";
    if (report.checked_frames >= report.expected_frames) return "1";
    const int64_t thousandths =
        static_cast<int64_t>(static_cast<__int128>(report.checked_frames) *
                             1000 / report.expected_frames);
    if (thousandths == 0) return "0";
    std::string digits = std::to_string(thousandths);
    digits.insert(0, 3 - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    return "0." + digits;
}

// Why "exact" is false, in the order a reader acts on it: a file of the
// wrong length, frames that diverge, frames nobody could compare.
std::string NotExactReason(const ExportVerificationReport& report) {
    std::vector<std::string> reasons;
    if (report.expected_frames <= 0) {
        reasons.push_back("the timeline has no frame to compare");
    }
    if (report.exported_frames != report.expected_frames) {
        reasons.push_back("the file has " +
                          std::to_string(report.exported_frames) +
                          " frames where the timeline has " +
                          std::to_string(report.expected_frames));
    }
    if (report.divergent_frames > 0) {
        reasons.push_back(std::to_string(report.divergent_frames) +
                          " frames diverge, listed under divergences");
    }
    const int64_t unchecked = report.expected_frames - report.checked_frames;
    if (report.expected_frames > 0 && unchecked > 0) {
        std::string text =
            std::to_string(unchecked) + " of the " +
            std::to_string(report.expected_frames) +
            " expected frames were not compared (a dissolve, a translucent, "
            "fading or blurred clip, a motion clip over a picture, a colour "
            "dip on its ramp, a source that ends early, or frames missing "
            "from the file): they are not verified";
        if (report.divergent_frames == 0) {
            text += ", and none of the " +
                    std::to_string(report.checked_frames) +
                    " compared diverges";
        }
        reasons.push_back(std::move(text));
    }
    std::string joined;
    for (const std::string& reason : reasons) {
        joined += (joined.empty() ? "" : "; ") + reason;
    }
    return joined;
}

}  // namespace

std::string SerializeExportVerification(
    const ExportVerificationReport& report) {
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(report.timeline_id));
    root.Set("frame_rate", Value::MakeString(RateString(report.frame_rate)));
    // PRD38 -- true only when every expected frame was compared and none
    // diverges (ExportVerificationIsExact); when it is false, the share of
    // frames compared and the reason stand beside it, so a film with a
    // dissolve reads "not verified" rather than "wrong".
    const bool exact = ExportVerificationIsExact(report);
    root.Set("exact", Value::MakeBool(exact));
    root.Set("verified_fraction",
             Value::MakeRawNumber(VerifiedFractionText(report)));
    if (!exact) {
        root.Set("not_exact_because",
                 Value::MakeString(NotExactReason(report)));
    }
    root.Set("expected_frames", Value::MakeInt(report.expected_frames));
    root.Set("exported_frames", Value::MakeInt(report.exported_frames));
    root.Set("checked_frames", Value::MakeInt(report.checked_frames));
    root.Set("motion_checked_frames",
             Value::MakeInt(report.motion_checked_frames));
    root.Set("unverifiable_frames", Value::MakeInt(report.unverifiable_frames));
    root.Set("ambiguous_frames", Value::MakeInt(report.ambiguous_frames));
    root.Set("divergent_frames", Value::MakeInt(report.divergent_frames));
    root.Set("unconfirmed_offset_frames",
             Value::MakeInt(report.unconfirmed_offset_frames));
    Value divergences = Value::MakeArray();
    for (const ExportDivergence& divergence : report.divergences) {
        Value item = Value::MakeObject();
        item.Set("kind", Value::MakeString(divergence.kind));
        item.Set("first_frame", Value::MakeInt(divergence.first_frame));
        item.Set("last_frame", Value::MakeInt(divergence.last_frame));
        if (!divergence.clip_id.empty() && divergence.motion) {
            item.Set("clip_id", Value::MakeString(divergence.clip_id));
            item.Set("composition_id", Value::MakeString(divergence.source_id));
            item.Set("expected_composition_frame",
                     Value::MakeInt(divergence.expected_source_frame));
        } else if (!divergence.clip_id.empty()) {
            item.Set("clip_id", Value::MakeString(divergence.clip_id));
            item.Set("source_id", Value::MakeString(divergence.source_id));
            item.Set("expected_source_frame",
                     Value::MakeInt(divergence.expected_source_frame));
        }
        if (divergence.kind == "offset")
            item.Set("offset", Value::MakeInt(divergence.offset));
        divergences.Push(std::move(item));
    }
    root.Set("divergences", std::move(divergences));
    // R8 -- kept out of "exact" on purpose: an isolated frame is a fact
    // about the picture, not a divergence from the document. The document
    // asked for that frame and the file delivered it; whether it should
    // have been asked for is the author's call, and eight shots of the same
    // film start on one deliberately.
    root.Set("isolated_frame_luma_scale",
             Value::MakeInt(kIsolatedFrameLumaScale));
    Value isolated = Value::MakeArray();
    for (const IsolatedFrameEvent& event : report.isolated_frames) {
        Value item = Value::MakeObject();
        item.Set("first_frame", Value::MakeInt(event.first_frame));
        item.Set("frames", Value::MakeInt(event.frames));
        item.Set("deviation", Value::MakeInt(event.deviation));
        item.Set("baseline", Value::MakeInt(event.baseline));
        item.Set("brighter", Value::MakeBool(event.deviation > 0));
        item.Set("histogram_distance",
                 Value::MakeInt(event.histogram_distance));
        item.Set("level_trigger", Value::MakeBool(event.level_trigger));
        item.Set("picture_trigger", Value::MakeBool(event.picture_trigger));
        isolated.Push(std::move(item));
    }
    root.Set("isolated_frames", std::move(isolated));
    return root.Dump();
}
