#include "PlaybackReads.h"

#include "ClipSpeed.h"
#include "Document.h"
#include "Timeline.h"

#include <algorithm>
#include <string>
#include <vector>

namespace playback_reads {

std::optional<read_ahead::SourceSpan> ClipReadSpan(const Document& document,
                                                   const DocumentClip& clip) {
    if (IsMotionClip(clip)) return std::nullopt;
    const DocumentSource* source = document.FindSource(clip.source_id);
    if (source == nullptr || source->rate.num <= 0 || source->rate.den <= 0)
        return std::nullopt;
    for (const DocumentTransition& transition : document.sequence.transitions) {
        if (transition.left_clip_id == clip.id ||
            transition.right_clip_id == clip.id)
            return std::nullopt;
    }
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0 || clip.duration.value <= 0)
        return std::nullopt;
    // The clip's last timeline frame starts one sequence frame before its
    // end.
    const RationalTime lastOffset =
        clip.duration.sub(RationalTime{rate.den, rate.num});
    const int64_t first =
        ClipSourceFrameAt(clip, RationalTime{0, rate.num}, source->rate);
    const int64_t last =
        lastOffset.value < 0
            ? first
            : ClipSourceFrameAt(clip, lastOffset, source->rate);
    return read_ahead::SourceSpan{std::min(first, last), std::max(first, last)};
}

int64_t SourceFramesIn(const Document& document, const DocumentClip& clip,
                       int64_t seconds) {
    const DocumentSource* source = document.FindSource(clip.source_id);
    if (source == nullptr || source->rate.num <= 0 || source->rate.den <= 0 ||
        seconds <= 0)
        return 0;
    // R3, criterion 4 -- a ramp is read at its fastest, which is enough at
    // every one of its frames.
    const EffectParamValue speed = ClipPeakSpeed(clip);
    const __int128 numerator = static_cast<__int128>(seconds) *
                               source->rate.num *
                               std::max<int32_t>(1, speed.num);
    const __int128 denominator = static_cast<__int128>(source->rate.den) *
                                 std::max<int32_t>(1, speed.den);
    return static_cast<int64_t>((numerator + denominator - 1) / denominator);
}

namespace {

bool PixelFormatHasAlpha(const std::string& format) {
    for (const char* prefix :
         {"yuva", "gbrap", "rgba", "bgra", "argb", "abgr", "ya8", "ya16"}) {
        if (format.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

monitor_occlusion::PictureLookup PicturesFromLibrary(const Document& document) {
    return [&document](const Ulid& source,
                       int64_t) -> std::optional<monitor_occlusion::Picture> {
        const LibraryMedia* media = document.FindLibraryMedia(source);
        if (media == nullptr || !media->metadata_complete ||
            !media->has_video || media->width <= 0 || media->height <= 0)
            return std::nullopt;
        return monitor_occlusion::UprightPicture(
            media->width, media->height, media->rotation_degrees,
            PixelFormatHasAlpha(media->pixel_format));
    };
}

std::optional<read_ahead::SourceSpan> VisibleReadSpan(
    const Document& document, const Timeline& timeline, const Ulid& trackId,
    const DocumentClip& clip, RationalTime position, int direction,
    RationalTime lookahead, const monitor_occlusion::PictureLookup& pictures,
    bool zoomed) {
    std::optional<read_ahead::SourceSpan> span = ClipReadSpan(document, clip);
    if (!span || direction == 0) return span;
    const DocumentSource* source = document.FindSource(clip.source_id);
    const MediaRate rate = document.sequence.frame_rate;
    const RationalTime frame{rate.den, rate.num};
    const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
    // The instants within reach where a shot on another track begins
    // (forward) or ends (backward): only there can this track start to be
    // hidden.
    std::vector<RationalTime> boundaries;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video" || track.id == trackId) continue;
        for (const DocumentClip& other : track.clips) {
            const RationalTime edge =
                direction > 0 ? other.timeline_in
                              : other.timeline_in.add(other.duration);
            if (direction > 0) {
                if (edge <= position || edge >= clipEnd ||
                    edge > position.add(lookahead))
                    continue;
            } else {
                if (edge >= position || edge <= clip.timeline_in ||
                    edge < position.sub(lookahead))
                    continue;
            }
            boundaries.push_back(edge);
        }
    }
    std::sort(boundaries.begin(), boundaries.end(),
              [direction](const RationalTime& a, const RationalTime& b) {
                  return direction > 0 ? a < b : b < a;
              });
    for (const RationalTime& edge : boundaries) {
        // Forward, the frame that starts at the edge; backward, the one
        // that ends there.
        const RationalTime probe = direction > 0 ? edge : edge.sub(frame);
        if (monitor_occlusion::HiddenTracks(document, timeline, probe, pictures,
                                            zoomed)
                .count(trackId) == 0)
            continue;
        if (direction > 0) {
            const int64_t lastSeen = ClipSourceFrameAt(
                clip, edge.sub(frame).sub(clip.timeline_in), source->rate);
            span->last = std::max(span->first, std::min(span->last, lastSeen));
        } else {
            const int64_t firstSeen = ClipSourceFrameAt(
                clip, edge.sub(clip.timeline_in), source->rate);
            span->first =
                std::min(span->last, std::max(span->first, firstSeen));
        }
        break;
    }
    return span;
}

std::optional<int64_t> NextPieceFrame(
    const Document& document, const Timeline& timeline, const Ulid& trackId,
    const DocumentClip& clip, RationalTime position, int direction,
    RationalTime lookahead, const monitor_occlusion::PictureLookup& pictures,
    bool zoomed) {
    if (direction <= 0 || IsMotionClip(clip)) return std::nullopt;
    const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
    if (!(position < clipEnd) || lookahead < clipEnd.sub(position))
        return std::nullopt;
    const DocumentTrack* track = document.FindTrack(trackId);
    if (track == nullptr) return std::nullopt;
    const DocumentClip* next = nullptr;
    for (const DocumentClip& candidate : track->clips) {
        if (candidate.timeline_in == clipEnd &&
            candidate.source_id == clip.source_id && !IsMotionClip(candidate)) {
            next = &candidate;
            break;
        }
    }
    // ClipReadSpan has nothing for a piece with a transition on either side.
    if (next == nullptr || !ClipReadSpan(document, clip) ||
        !ClipReadSpan(document, *next))
        return std::nullopt;
    if (monitor_occlusion::HiddenTracks(document, timeline, clipEnd, pictures,
                                        zoomed)
            .count(trackId) != 0)
        return std::nullopt;
    for (const ResolvedLayer& layer :
         timeline.ResolveTrackLayers(trackId, clipEnd)) {
        if (layer.frame.clip_id == next->id) return layer.frame.source_frame;
    }
    return std::nullopt;
}

}  // namespace playback_reads
