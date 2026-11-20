#include "TimelineSheets.h"

#include "ClipAnimation.h"
#include "ClipBlur.h"
#include "ClipEnabled.h"
#include "ClipFraming.h"
#include "ClipMask.h"
#include "ClipSpeed.h"
#include "ColorManagement.h"
#include "ColorMeasure.h"
#include "Export.h"
#include "FfmpegBinary.h"
#include "FrameCapture.h"
#include "Json.h"
#include "PersonMatte.h"
#include "ShotDescriptions.h"
#include "Ulid.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace {

using mcp_json::Value;

struct VisibleSegment {
    const DocumentClip* clip = nullptr;
    int32_t track_index = 0;
    RationalTime start;
    RationalTime end;
};

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

bool ClipEnd(const DocumentClip& clip, RationalTime& output,
             std::string& error) {
    if (clip.timeline_in.rate <= 0 || clip.duration.rate <= 0 ||
        clip.source_in.rate <= 0) {
        error = "clip '" + clip.id + "' has a non-positive time rate";
        return false;
    }
    if (clip.timeline_in.value < 0 || clip.duration.value < 0 ||
        clip.source_in.value < 0) {
        error = "clip '" + clip.id + "' has a negative time extent";
        return false;
    }
    try {
        output = clip.timeline_in.add(clip.duration);
    } catch (const std::exception&) {
        error = "clip '" + clip.id + "' timeline extent overflows";
        return false;
    }
    return true;
}

bool VisibleSegments(const Document& document,
                     std::vector<VisibleSegment>& output, std::string& error) {
    output.clear();
    std::vector<RationalTime> boundaries;
    // F1.5 -- ROADMAP.md. An inactive multicam angle is not on screen: it is
    // neither sampled nor allowed to occlude (Multicam.h). Nor is a disabled
    // clip (ClipEnabled.h).
    const std::set<Ulid> offScreen = OffScreenClipIds(document.sequence);
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video" || !track.visible) continue;
        for (const DocumentClip& clip : track.clips) {
            // MOTION-2026-09 M4 -- a sheet shows the shots; a motion clip
            // is drawn over them, and neither sampled nor occluding.
            if (offScreen.count(clip.id) != 0 || IsMotionClip(clip)) {
                continue;
            }
            RationalTime end;
            if (!ClipEnd(clip, end, error)) return false;
            if (clip.duration.value == 0) continue;
            boundaries.push_back(clip.timeline_in);
            boundaries.push_back(end);
        }
    }
    std::sort(boundaries.begin(), boundaries.end(),
              [](const RationalTime& left, const RationalTime& right) {
                  return left < right;
              });
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());

    for (size_t index = 0; index + 1 < boundaries.size(); ++index) {
        const RationalTime start = boundaries[index];
        const RationalTime end = boundaries[index + 1];
        if (start >= end) continue;
        const DocumentClip* topClip = nullptr;
        int32_t topTrack = std::numeric_limits<int32_t>::min();
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "video" || !track.visible) continue;
            for (const DocumentClip& clip : track.clips) {
                if (offScreen.count(clip.id) != 0 || IsMotionClip(clip)) {
                    continue;
                }
                RationalTime clipEnd;
                if (!ClipEnd(clip, clipEnd, error)) return false;
                if (clip.timeline_in <= start && clipEnd > start &&
                    track.index > topTrack) {
                    topClip = &clip;
                    topTrack = track.index;
                }
            }
        }
        if (topClip == nullptr) continue;
        if (!output.empty() && output.back().clip->id == topClip->id &&
            output.back().end == start) {
            output.back().end = end;
        } else {
            output.push_back({topClip, topTrack, start, end});
        }
    }
    return true;
}

bool FrameDuration(const Document& document, RationalTime& output,
                   std::string& error) {
    if (document.sequence.frame_rate.num <= 0 ||
        document.sequence.frame_rate.den <= 0) {
        error = "sequence frame rate must be positive";
        return false;
    }
    output = {document.sequence.frame_rate.den,
              document.sequence.frame_rate.num};
    return true;
}

bool FrameFor(const Document& document, const VisibleSegment& segment,
              const RationalTime& timeline, const std::string& role,
              const RationalTime* cutPosition, TimelineSheetFrame& output,
              std::string& error) {
    try {
        output.clip_id = segment.clip->id;
        output.source_id = segment.clip->source_id;
        output.timeline_position = timeline;
        // R3 -- docs/roadmap/REEL-2026-09.md. The sheets read the same
        // source frame as the monitor and the export do (ClipSpeed.h), from
        // its first instant: that is the time the render seeks to.
        output.source_position = ClipSourceTimeAt(
            *segment.clip, timeline.sub(segment.clip->timeline_in));
        if (const DocumentSource* source =
                document.FindSource(segment.clip->source_id)) {
            output.source_position =
                SourceFrameStart(output.source_position, source->rate);
        }
        output.role = role;
        if (cutPosition != nullptr) {
            output.cut_position = *cutPosition;
            output.has_cut_position = true;
        }
        return true;
    } catch (const std::exception&) {
        error = "clip '" + segment.clip->id + "' source position overflows";
        return false;
    }
}

std::vector<size_t> EvenSelection(size_t count, size_t maximum) {
    if (count <= maximum) {
        std::vector<size_t> all(count);
        for (size_t index = 0; index < count; ++index) all[index] = index;
        return all;
    }
    std::vector<size_t> selected;
    selected.reserve(maximum);
    if (maximum == 1) return {count / 2};
    for (size_t index = 0; index < maximum; ++index) {
        selected.push_back(index * (count - 1) / (maximum - 1));
    }
    return selected;
}

std::string ExactSecondsString(const RationalTime& time) {
    const bool negative = time.value < 0;
    const uint64_t magnitude =
        negative ? static_cast<uint64_t>(-(time.value + 1)) + 1
                 : static_cast<uint64_t>(time.value);
    const uint64_t rate = static_cast<uint64_t>(time.rate);
    const uint64_t whole = magnitude / rate;
    uint64_t remainder = magnitude % rate;
    std::ostringstream output;
    if (negative) output << '-';
    output << whole;
    if (remainder == 0) return output.str();
    output << '.';
    for (int digit = 0; digit < 12 && remainder != 0; ++digit) {
        remainder *= 10;
        output << static_cast<char>('0' + remainder / rate);
        remainder %= rate;
    }
    return output.str();
}

bool RunFfmpeg(const std::vector<std::string>& arguments, std::string& error) {
    process::Request request;
    request.arguments = arguments;
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "FFmpeg pipes");
        return false;
    }
    if (!result.Succeeded()) {
        error = "FFmpeg could not render the timeline sheet";
        return false;
    }
    return true;
}

const LibraryMedia* FindMedia(const Document& document, const Ulid& sourceId) {
    return document.FindLibraryMedia(sourceId);
}

std::filesystem::path ResolvePath(const std::filesystem::path& directory,
                                  const std::string& path) {
    std::filesystem::path resolved(path);
    if (resolved.is_relative()) resolved = directory / resolved;
    return resolved.lexically_normal();
}

std::string EscapeFilterPath(const std::filesystem::path& path) {
    std::string escaped;
    for (char character : path.string()) {
        if (character == '\\' || character == '\'' || character == ':')
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

// R1 -- docs/roadmap/REEL-2026-09.md. The sequence frame as it fits in a
// cell: a framed clip is composed into *that* rectangle, by the same
// function the monitor and the export call.
//
// A cell IS that rectangle, cell_dimension on its long edge. Cells used to
// be squares with the frame letterboxed inside: 44 % of every cell of a 16:9
// or a 9:16 sheet was black, and a rush of another shape than the sequence
// was fitted to the square rather than to the frame the monitor fits it to
// (a vertical rush in a 16:9 film looked full-height). Rounded down to even
// sizes for the JPEG's chroma.
void CellFrameSize(const Document& document,
                   const TimelineSheetSettings& settings, int64_t& width,
                   int64_t& height) {
    width = settings.cell_dimension;
    height = settings.cell_dimension;
    const bool ownShape = settings.frame_width > 0 && settings.frame_height > 0;
    BoundedFrameSize(
        ownShape ? settings.frame_width : document.sequence.width,
        ownShape ? settings.frame_height : document.sequence.height,
        settings.cell_dimension, width, height);
    width = std::max<int64_t>(2, width & ~int64_t{1});
    height = std::max<int64_t>(2, height & ~int64_t{1});
}

// DEC29 -- docs/roadmap/GRADE-2026-09.md. The graded sheet: each cell graded
// by the engine, pixel for pixel the chain read_frame graded uses
// (RenderGradedSheetCells), laid in the grid the plain sheet lays, and the
// grid encoded as that frame is (EncodeDisplayPictureAsJpeg). The cells are
// not passed through FFmpeg's display transform: they are display values
// already, the look included.
bool RenderGradedTimelineSheet(const Document& document,
                               const std::filesystem::path& projectDirectory,
                               const TimelineSheetPlan& plan,
                               const TimelineSheetSettings& settings,
                               std::string& jpegBytes, std::string& error) {
    if (plan.kind == TimelineSheetKind::Media) {
        error =
            "a sheet of rushes is never graded: a rush has no grade until a "
            "clip places it";
        return false;
    }
    int64_t cellWidth = 0;
    int64_t cellHeight = 0;
    CellFrameSize(document, settings, cellWidth, cellHeight);
    std::vector<GradedSheetCell> cells;
    cells.reserve(plan.frames.size());
    for (const TimelineSheetFrame& frame : plan.frames)
        cells.push_back({frame.clip_id, frame.source_position});
    std::vector<std::vector<std::array<float, 3>>> pictures;
    if (!RenderGradedSheetCells(document, projectDirectory.string(), cells,
                                cellWidth, cellHeight, settings.cell_dimension,
                                settings.ffmpeg_path, pictures, error))
        return false;
    const int64_t columns = std::min<int64_t>(
        settings.columns, static_cast<int64_t>(plan.frames.size()));
    const int64_t rows =
        (static_cast<int64_t>(plan.frames.size()) + columns - 1) / columns;
    const int64_t gridWidth = columns * cellWidth;
    const int64_t gridHeight = rows * cellHeight;
    std::vector<std::array<float, 3>> grid(
        static_cast<size_t>(gridWidth * gridHeight),
        std::array<float, 3>{0.0f, 0.0f, 0.0f});
    for (size_t index = 0; index < pictures.size(); ++index) {
        const int64_t left =
            (static_cast<int64_t>(index) % columns) * cellWidth;
        const int64_t top =
            (static_cast<int64_t>(index) / columns) * cellHeight;
        for (int64_t row = 0; row < cellHeight; ++row) {
            std::copy_n(pictures[index].begin() + row * cellWidth, cellWidth,
                        grid.begin() + (top + row) * gridWidth + left);
        }
    }
    FrameCaptureSettings encoding;
    encoding.jpeg_quality = settings.jpeg_quality;
    encoding.ffmpeg_path = settings.ffmpeg_path;
    return EncodeDisplayPictureAsJpeg(grid, gridWidth, gridHeight, encoding,
                                      jpegBytes, error);
}

}  // namespace

bool BuildTimelineSheetPlan(const Document& document, TimelineSheetKind kind,
                            const TimelineSheetSettings& settings,
                            TimelineSheetPlan& output, std::string& error) {
    output = {};
    output.kind = kind;
    error.clear();
    if (settings.maximum_images <= 0) {
        error = "maximum_images must be positive";
        return false;
    }
    if (kind == TimelineSheetKind::Cuts && settings.maximum_images < 2) {
        error = "a cut sheet needs room for at least one image pair";
        return false;
    }
    std::vector<VisibleSegment> segments;
    if (!VisibleSegments(document, segments, error)) return false;
    RationalTime frameDuration;
    if (!FrameDuration(document, frameDuration, error)) return false;

    if (kind == TimelineSheetKind::Contact) {
        output.total_candidates = static_cast<int64_t>(segments.size());
        const std::vector<size_t> selected =
            EvenSelection(segments.size(), settings.maximum_images);
        for (size_t index : selected) {
            const VisibleSegment& segment = segments[index];
            int64_t frames = 0;
            try {
                frames = segment.end.sub(segment.start)
                             .to_frames(document.sequence.frame_rate.num,
                                        document.sequence.frame_rate.den);
            } catch (const std::exception&) {
                error = "visible segment for clip '" + segment.clip->id +
                        "' is not aligned to the sequence frame rate";
                return false;
            }
            if (frames <= 0) continue;
            const RationalTime middle = segment.start.add(
                {frames / 2 * document.sequence.frame_rate.den,
                 document.sequence.frame_rate.num});
            TimelineSheetFrame frame;
            if (!FrameFor(document, segment, middle, "middle", nullptr, frame,
                          error))
                return false;
            output.frames.push_back(std::move(frame));
        }
        return true;
    }

    std::vector<size_t> cuts;
    for (size_t index = 1; index < segments.size(); ++index) {
        if (segments[index - 1].end == segments[index].start)
            cuts.push_back(index);
    }
    output.total_candidates = static_cast<int64_t>(cuts.size());
    const size_t maximumCuts =
        std::max<size_t>(1, static_cast<size_t>(settings.maximum_images / 2));
    for (size_t selectedIndex : EvenSelection(cuts.size(), maximumCuts)) {
        const size_t rightIndex = cuts[selectedIndex];
        const VisibleSegment& left = segments[rightIndex - 1];
        const VisibleSegment& right = segments[rightIndex];
        const RationalTime cut = right.start;
        const RationalTime before = cut.sub(frameDuration);
        if (before < left.start) {
            error = "visible segment before cut is shorter than one frame";
            return false;
        }
        TimelineSheetFrame leftFrame;
        TimelineSheetFrame rightFrame;
        if (!FrameFor(document, left, before, "before", &cut, leftFrame,
                      error) ||
            !FrameFor(document, right, cut, "after", &cut, rightFrame, error))
            return false;
        output.frames.push_back(std::move(leftFrame));
        output.frames.push_back(std::move(rightFrame));
    }
    return true;
}

bool BuildMediaSheetPlan(const Document& document,
                         const std::vector<Ulid>& mediaIds,
                         TimelineSheetSettings& settings,
                         TimelineSheetPlan& output, std::string& error) {
    output = {};
    output.kind = TimelineSheetKind::Media;
    error.clear();
    if (settings.maximum_images <= 0) {
        error = "maximum_images must be positive";
        return false;
    }
    if (mediaIds.empty()) {
        error = "a sheet of rushes needs at least one media";
        return false;
    }
    struct Take {
        Ulid id;
        MediaRate rate;
        int64_t frames = 0;
    };
    std::vector<Take> takes;
    std::set<Ulid> seen;
    for (const Ulid& mediaId : mediaIds) {
        if (!seen.insert(mediaId).second) continue;
        const DocumentSource* source = document.FindSource(mediaId);
        const LibraryMedia* media = document.FindLibraryMedia(mediaId);
        if (source == nullptr || media == nullptr) {
            error = "unknown media_id '" + mediaId + "'";
            return false;
        }
        if (media->metadata_complete && !media->has_video) {
            error = "media_id '" + mediaId + "' is audio-only";
            return false;
        }
        const int64_t frames =
            source->rate.num > 0 && source->rate.den > 0
                ? source->duration.to_frames(source->rate.num, source->rate.den)
                : 0;
        if (frames <= 0) {
            error = "media_id '" + mediaId + "' has no frame to show";
            return false;
        }
        if (takes.empty() && media->width > 0 && media->height > 0) {
            const bool quarter = media->rotation_degrees % 180 != 0;
            settings.frame_width = quarter ? media->height : media->width;
            settings.frame_height = quarter ? media->width : media->height;
        }
        takes.push_back({mediaId, source->rate, frames});
    }
    const auto add = [&](const Take& take, int64_t frame) {
        TimelineSheetFrame cell;
        cell.source_id = take.id;
        cell.source_position = {frame * take.rate.den, take.rate.num};
        cell.role = "media";
        // PL4 -- which cells are described already, and by what.
        if (const ShotDescription* shot = ShotAt(
                *document.FindLibraryMedia(take.id), cell.source_position))
            cell.shot_id = shot->id;
        output.frames.push_back(std::move(cell));
    };
    if (takes.size() == 1) {
        const Take& take = takes.front();
        const int64_t count =
            std::min<int64_t>(settings.maximum_images, take.frames);
        output.total_candidates = take.frames;
        for (int64_t part = 0; part < count; ++part)
            add(take, ((2 * part + 1) * take.frames) / (2 * count));
        return true;
    }
    output.total_candidates = static_cast<int64_t>(takes.size());
    for (size_t index : EvenSelection(
             takes.size(), static_cast<size_t>(settings.maximum_images)))
        add(takes[index], takes[index].frames / 2);
    return true;
}

std::string EmptyTimelineSheetReason(const TimelineSheetPlan& plan) {
    if (!plan.frames.empty()) return std::string();
    switch (plan.kind) {
        case TimelineSheetKind::Contact:
            return "nothing to show: the timeline has no visible video shot "
                   "(its video tracks are empty or hidden, or hold only "
                   "motion clips)";
        case TimelineSheetKind::Cuts:
            return "nothing to show: the timeline has no visible cut, where "
                   "one visible shot is followed directly by another (a "
                   "single shot, shots apart from each other and titles over "
                   "one shot make none)";
        case TimelineSheetKind::Media:
            break;
    }
    return "nothing to show: the sheet names no media";
}

std::string SerializeTimelineSheetPlan(const TimelineSheetPlan& plan,
                                       bool graded) {
    Value root = Value::MakeObject();
    root.Set("kind", Value::MakeString(plan.kind == TimelineSheetKind::Contact
                                           ? "contact_sheet"
                                       : plan.kind == TimelineSheetKind::Cuts
                                           ? "cut_sheet"
                                           : "media_sheet"));
    if (graded) root.Set("graded", Value::MakeBool(true));
    root.Set("total_candidates", Value::MakeInt(plan.total_candidates));
    root.Set("selected_images",
             Value::MakeInt(static_cast<int64_t>(plan.frames.size())));
    Value frames = Value::MakeArray();
    for (size_t index = 0; index < plan.frames.size(); ++index) {
        const TimelineSheetFrame& frame = plan.frames[index];
        Value value = Value::MakeObject();
        value.Set("cell", Value::MakeInt(static_cast<int64_t>(index)));
        value.Set("role", Value::MakeString(frame.role));
        value.Set("clip_id", Value::MakeString(frame.clip_id));
        value.Set("source_id", Value::MakeString(frame.source_id));
        value.Set("timeline_position", TimeValue(frame.timeline_position));
        value.Set("source_in", TimeValue(frame.source_position));
        if (frame.has_cut_position)
            value.Set("cut_position", TimeValue(frame.cut_position));
        // PL4 -- docs/roadmap/PLANS-2026-10.md. A frame of a rush says
        // whether a description covers it: null, it is still to describe.
        if (plan.kind == TimelineSheetKind::Media)
            value.Set("shot_id", frame.shot_id.empty()
                                     ? Value::MakeNull()
                                     : Value::MakeString(frame.shot_id));
        frames.Push(std::move(value));
    }
    root.Set("frames", std::move(frames));
    return root.Dump();
}

bool RenderTimelineSheet(const Document& document,
                         const std::filesystem::path& projectDirectory,
                         const TimelineSheetPlan& plan,
                         const TimelineSheetSettings& settings,
                         std::string& jpegBytes, std::string& error) {
    jpegBytes.clear();
    error.clear();
    if (plan.frames.empty()) {
        error = EmptyTimelineSheetReason(plan);
        return false;
    }
    if (settings.columns <= 0 || settings.cell_dimension < 16 ||
        settings.jpeg_quality < 2 || settings.jpeg_quality > 31) {
        error = "invalid timeline sheet render settings";
        return false;
    }
    if (settings.graded)
        return RenderGradedTimelineSheet(document, projectDirectory, plan,
                                         settings, jpegBytes, error);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::string id = GenerateUlid();
    const std::filesystem::path imagePath =
        temporaryDirectory / ("cutmachine-sheet-" + id + ".jpg");
    const std::filesystem::path lutPath =
        temporaryDirectory / ("cutmachine-sheet-" + id + ".cube");
    // G3 -- docs/roadmap/GRADE-2026-09.md. Each frame is read with its
    // media's input. When they all share one, the whole sheet takes one
    // transform after the grid, as it always has; when they differ, each
    // cell takes its own before it is padded, one cube per input.
    std::map<std::string, std::filesystem::path> cellLuts;
    // PA15 -- the matte of each cell cut out along its people, one picture
    // each, read as inputs after the cells' own.
    std::map<size_t, std::pair<int, std::filesystem::path>> cellMattes;
    const auto cleanup = [&]() {
        std::error_code ignored;
        std::filesystem::remove(imagePath, ignored);
        std::filesystem::remove(lutPath, ignored);
        for (const auto& [key, path] : cellLuts)
            std::filesystem::remove(path, ignored);
        for (const auto& [cell, matte] : cellMattes)
            std::filesystem::remove(matte.second, ignored);
    };
    bool mixedInputs = false;
    if (document.color_management.enabled && !plan.frames.empty()) {
        const ColorManagementSettings firstColor =
            document.ColorManagementFor(plan.frames.front().source_id);
        for (const TimelineSheetFrame& frame : plan.frames) {
            const ColorManagementSettings frameColor =
                document.ColorManagementFor(frame.source_id);
            // DEC31 -- a graphic manages no colour: shown as authored, it
            // takes no transform, so beside anything else it is mixed.
            if (frameColor.enabled != firstColor.enabled ||
                OpenColorIoCacheKey(frameColor) !=
                    OpenColorIoCacheKey(firstColor) ||
                ColorInputScaleOptions(frameColor) !=
                    ColorInputScaleOptions(firstColor))
                mixedInputs = true;
        }
    }
    // The transform of one cell, when inputs are mixed; nothing otherwise.
    const auto cellTransform = [&](const Ulid& sourceId, std::string& filter) {
        filter.clear();
        if (!mixedInputs) return true;
        const ColorManagementSettings frameColor =
            document.ColorManagementFor(sourceId);
        // DEC31 -- a graphic's cell keeps its code values.
        if (!frameColor.enabled) return true;
        const std::string key = OpenColorIoCacheKey(frameColor);
        auto found = cellLuts.find(key);
        if (found == cellLuts.end()) {
            std::string cube;
            if (!BuildOpenColorIoCube(ColorManagementForSdrPreview(frameColor),
                                      cube, error)) {
                error = "unable to build OpenColorIO sheet transform: " + error;
                return false;
            }
            const std::filesystem::path path =
                temporaryDirectory /
                ("cutmachine-sheet-" + id + "-" +
                 std::to_string(cellLuts.size()) + ".cube");
            found = cellLuts.emplace(key, path).first;
            std::ofstream lut(path, std::ios::binary | std::ios::trunc);
            lut << cube;
            lut.close();
            if (!lut) {
                error = "unable to create the timeline sheet colour transform";
                return false;
            }
        }
        filter = ',' + ColorLutFilter(frameColor,
                                      EscapeFilterPath(found->second), "bt709");
        return true;
    };

    std::vector<std::string> arguments = {settings.ffmpeg_path, "-hide_banner",
                                          "-loglevel", "error", "-nostdin"};
    for (const TimelineSheetFrame& frame : plan.frames) {
        const LibraryMedia* media = FindMedia(document, frame.source_id);
        if (media == nullptr) {
            cleanup();
            error = "unknown media_id '" + frame.source_id + "'";
            return false;
        }
        if (media->metadata_complete && !media->has_video) {
            cleanup();
            error = "media_id '" + frame.source_id +
                    "' is audio-only; timeline sheets require video";
            return false;
        }
        const std::string input =
            ResolvePath(projectDirectory, media->path).string();
        // PRD40 -- an offline rush is named, not left to FFmpeg's "could not
        // render the timeline sheet".
        if (!ffmpeg::InputPresent(input, error)) {
            cleanup();
            return false;
        }
        arguments.insert(
            arguments.end(),
            {"-ss", ExactSecondsString(frame.source_position), "-i", input});
    }
    for (size_t index = 0; index < plan.frames.size(); ++index) {
        const DocumentClip* clip =
            document.FindClip(plan.frames[index].clip_id);
        if (clip == nullptr || !ClipHasPersonMask(*clip)) continue;
        const LibraryMedia* media = FindMedia(document, clip->source_id);
        std::string mattePath;
        person_matte::Info info;
        if (media == nullptr || !person_matte::MatteForMedia(
                                    document, projectDirectory.string(),
                                    clip->source_id, mattePath, info, error)) {
            cleanup();
            error = "clip '" + clip->id + "': " + error;
            return false;
        }
        const std::filesystem::path picture =
            temporaryDirectory / ("cutmachine-sheet-" + id + "-matte-" +
                                  std::to_string(index) + ".pgm");
        const int input =
            static_cast<int>(plan.frames.size() + cellMattes.size());
        cellMattes[index] = {input, picture};
        if (!person_matte::WriteFramePgm(
                mattePath,
                person_matte::SourceFrameAt(plan.frames[index].source_position,
                                            media->rate),
                picture.string(), error)) {
            cleanup();
            return false;
        }
        arguments.insert(arguments.end(), {"-i", picture.string()});
    }

    const int32_t columns = std::min<int32_t>(
        settings.columns, static_cast<int32_t>(plan.frames.size()));
    int64_t cellFrameWidth = 0;
    int64_t cellFrameHeight = 0;
    CellFrameSize(document, settings, cellFrameWidth, cellFrameHeight);
    std::ostringstream graph;
    for (size_t index = 0; index < plan.frames.size(); ++index) {
        const DocumentClip* clip =
            document.FindClip(plan.frames[index].clip_id);
        std::string cellLut;
        if (!cellTransform(plan.frames[index].source_id, cellLut)) {
            cleanup();
            return false;
        }
        graph << '[' << index << ":v:0]";
        // EF3 -- a blurred clip is drawn as a framed one, in the rectangle
        // its framing names (the default fit when it names none): the blur
        // is measured in that frame's pixels and drawn on that rectangle.
        if (clip != nullptr &&
            (ClipHasFraming(*clip) || ClipHasAnimation(*clip) ||
             ClipHasBlur(*clip) || ClipHasMask(*clip))) {
            int64_t displayedWidth = 0;
            int64_t displayedHeight = 0;
            ClipFramingGeometry framing;
            // R2 -- docs/roadmap/REEL-2026-09.md. The sheet draws one
            // frame of the clip, so it draws the framing THAT frame has.
            // The clip's own frame number comes from the plan's timeline
            // position, and the values are sampled by the one function the
            // monitor and the export also call (guard rail 1).
            ClipSampledParameters sampled;
            sampled.framing = clip->framing;
            if (ClipHasAnimation(*clip)) {
                const RationalTime offset =
                    plan.frames[index].timeline_position.sub(clip->timeline_in);
                const int64_t localFrame =
                    offset.to_frames(document.sequence.frame_rate.num,
                                     document.sequence.frame_rate.den);
                if (!SampleClipParameters(*clip, localFrame,
                                          document.sequence.frame_rate, sampled,
                                          error)) {
                    cleanup();
                    error = "clip '" + clip->id + "': " + error;
                    return false;
                }
            }
            if (!ClipDisplayedSize(document, *clip, displayedWidth,
                                   displayedHeight, error) ||
                !ResolveClipFramingReduced(
                    sampled.framing, displayedWidth, displayedHeight,
                    document.sequence.width, document.sequence.height,
                    cellFrameWidth, cellFrameHeight, framing, error)) {
                cleanup();
                error = "clip '" + clip->id + "': " + error;
                return false;
            }
            const std::string crop =
                ClipFramingCropFilter(framing, displayedWidth, displayedHeight);
            // EF3 -- docs/roadmap/EFFETS-2026-10.md. The blur in the cell's
            // pixels, on the picture before it is laid in the cell, as the
            // export draws it (ClipBlur.h).
            std::string blur;
            if (!ClipBlurFilter(*clip, cellFrameWidth, cellFrameHeight,
                                document.sequence.width,
                                document.sequence.height, blur, error)) {
                cleanup();
                return false;
            }
            if (!blur.empty()) {
                const LibraryMedia* media =
                    document.FindLibraryMedia(clip->source_id);
                const bool rgb =
                    media != nullptr && IsRgbPixelFormat(media->pixel_format);
                blur = std::string(",format=") +
                       (rgb ? "gbrap16le" : "yuva444p16le") + ',' + blur;
            }
            // PA8 -- docs/roadmap/PARITE-2026-11.md. The mask after the
            // blur, as the export puts it, and what it cuts out brought
            // onto the cell's black: a sheet draws each clip alone.
            std::string mask = ClipMaskFilter(
                clip->mask,
                ClipMaskRegion{framing.source_x, framing.source_y,
                               framing.source_width, framing.source_height,
                               displayedWidth, displayedHeight},
                "c" + std::to_string(index));
            // PA15 -- a cell cut out along its people takes its matte,
            // read through the crop and the scale its picture went through.
            const auto matte = cellMattes.find(index);
            if (matte != cellMattes.end()) {
                mask = person_matte::MatteFilter(
                    matte->second.first, displayedWidth, displayedHeight,
                    (crop.empty() ? std::string() : crop + ",") +
                        "scale=w=" + std::to_string(framing.width) +
                        ":h=" + std::to_string(framing.height),
                    clip->mask.inverted, "c" + std::to_string(index));
            }
            if (!mask.empty()) {
                const LibraryMedia* media =
                    document.FindLibraryMedia(clip->source_id);
                const bool rgb =
                    media != nullptr && IsRgbPixelFormat(media->pixel_format);
                mask = std::string(",format=") +
                       (rgb ? "gbrap16le" : "yuva444p16le") + ',' + mask +
                       ",premultiply=inplace=1";
            }
            if (!crop.empty()) graph << crop << ',';
            graph << "scale=w=" << framing.width << ":h=" << framing.height
                  << ":flags=lanczos" << blur << mask << cellLut
                  << ",pad=" << cellFrameWidth << ':' << cellFrameHeight << ':'
                  << framing.x << ':' << framing.y << ":black,setsar=1[cell"
                  << index << "];";
            continue;
        }
        graph << "scale=w=" << cellFrameWidth << ":h=" << cellFrameHeight
              << ":force_original_aspect_ratio=decrease:flags=lanczos"
              << cellLut << ",pad=" << cellFrameWidth << ':' << cellFrameHeight
              << ":(ow-iw)/2:(oh-ih)/2:black,setsar=1[cell" << index << "];";
    }
    if (plan.frames.size() == 1) {
        graph << "[cell0]null";
    } else {
        for (size_t index = 0; index < plan.frames.size(); ++index)
            graph << "[cell" << index << ']';
        graph << "xstack=inputs=" << plan.frames.size() << ":layout=";
        for (size_t index = 0; index < plan.frames.size(); ++index) {
            if (index) graph << '|';
            graph << (index % columns) * cellFrameWidth << '_'
                  << (index / columns) * cellFrameHeight;
        }
        graph << ":fill=black";
    }

    const bool sheetManaged =
        document.color_management.enabled && !mixedInputs &&
        (plan.frames.empty() ||
         document.ColorManagementFor(plan.frames.front().source_id).enabled);
    if (sheetManaged) {
        const ColorManagementSettings sheetColor =
            plan.frames.empty()
                ? document.color_management
                : document.ColorManagementFor(plan.frames.front().source_id);
        const ColorManagementSettings preview =
            ColorManagementForSdrPreview(sheetColor);
        std::string cube;
        if (!BuildOpenColorIoCube(preview, cube, error)) {
            cleanup();
            error = "unable to build OpenColorIO sheet transform: " + error;
            return false;
        }
        std::ofstream lut(lutPath, std::ios::binary | std::ios::trunc);
        lut << cube;
        lut.close();
        if (!lut) {
            cleanup();
            error = "unable to create the timeline sheet colour transform";
            return false;
        }
        graph << ','
              << ColorLutFilter(sheetColor, EscapeFilterPath(lutPath), "bt709");
    }
    graph << ',' << kJpegFromVideoFilter << "[sheet]";

    arguments.insert(arguments.end(), {"-filter_complex", graph.str(), "-map",
                                       "[sheet]", "-frames:v", "1", "-q:v",
                                       std::to_string(settings.jpeg_quality),
                                       "-y", imagePath.string()});
    if (!RunFfmpeg(arguments, error)) {
        cleanup();
        return false;
    }
    std::ifstream input(imagePath, std::ios::binary);
    if (!input) {
        cleanup();
        error = "unable to read the rendered timeline sheet";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    jpegBytes = buffer.str();
    cleanup();
    if (jpegBytes.empty()) {
        error = "the timeline sheet produced no image";
        return false;
    }
    return true;
}
