#include "MonitorOcclusion.h"

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "Document.h"
#include "Timeline.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace monitor_occlusion {

namespace {

// Past this many frames a window is not read further: four seconds of 60
// i/s playback at four times its speed is under a thousand.
constexpr int64_t kMaxWindowFrames = 2000;

bool IsWhole(const EffectParamValue& value) {
    return value.den > 0 && value.num == value.den;
}

// The tracks Record composites, bottom to top: main.mm's videoTrackIds.
// Document::Validate refuses two tracks with one index, so the order is
// the one main.mm sorts them in.
std::vector<const DocumentTrack*> CompositedTracks(const Document& document) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video" && track.visible) {
            tracks.push_back(&track);
        }
    }
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return tracks;
}

// A transition of the track -- dissolve or colour dip -- within a frame of
// `position`, or one whose place cannot be worked out.
bool TransitionNear(const Document& document, const Ulid& trackId,
                    RationalTime position) {
    const MediaRate rate = document.sequence.frame_rate;
    const int64_t frame = position.to_frames(rate.num, rate.den);
    for (const DocumentTransition& transition : document.sequence.transitions) {
        if (transition.track_id != trackId) {
            continue;
        }
        TransitionSpan span;
        std::string error;
        if (!ResolveTransitionSpan(document.sequence, transition, span,
                                   error)) {
            return true;
        }
        if (frame >= span.first_frame() - 1 && frame <= span.end_frame()) {
            return true;
        }
    }
    return false;
}

// Whether `track`'s layer at `position` is drawn with an alpha of one on
// every pixel of the frame: the rule of the header, clause by clause.
bool CoversFrame(const Document& document, const Timeline& timeline,
                 const DocumentTrack& track, RationalTime position,
                 const PictureLookup& picture) {
    const std::vector<ResolvedLayer> layers =
        timeline.ResolveTrackLayers(track.id, position);
    if (layers.size() != 1 || layers.front().opacity < 1.0f ||
        TransitionNear(document, track.id, position)) {
        return false;
    }
    const ResolvedLayer& layer = layers.front();
    const DocumentClip* clip = document.FindClip(layer.frame.clip_id);
    if (clip == nullptr || IsMotionClip(*clip) || !IsWhole(clip->opacity)) {
        return false;
    }
    const std::optional<Picture> shown =
        picture(layer.frame.source_id, layer.frame.source_frame);
    if (!shown || shown->alpha || shown->width <= 0 || shown->height <= 0) {
        return false;
    }
    const int64_t frameWidth = document.sequence.width;
    const int64_t frameHeight = document.sequence.height;
    if (frameWidth <= 0 || frameHeight <= 0) {
        return false;
    }
    if (!ClipHasFraming(*clip) && !ClipHasAnimation(*clip)) {
        // Fitted by the renderer: the whole frame only at its exact aspect.
        return static_cast<__int128>(shown->width) * frameHeight ==
               static_cast<__int128>(shown->height) * frameWidth;
    }
    // Framed or keyed: the rectangle main.mm hands the renderer, resolved
    // from the same values (presentNearestFrameAtDeadline).
    ClipSampledParameters sampled;
    sampled.opacity = clip->opacity;
    sampled.framing = clip->framing;
    std::string error;
    if (ClipHasAnimation(*clip)) {
        const MediaRate rate = document.sequence.frame_rate;
        const RationalTime offset = position.sub(clip->timeline_in);
        if (!SampleClipParameters(*clip, offset.to_frames(rate.num, rate.den),
                                  rate, sampled, error) ||
            !IsWhole(sampled.opacity)) {
            return false;
        }
    }
    int64_t displayedWidth = 0;
    int64_t displayedHeight = 0;
    ClipFramingGeometry geometry;
    return ClipDisplayedSize(document, *clip, displayedWidth, displayedHeight,
                             error) &&
           ResolveClipFraming(sampled.framing, displayedWidth, displayedHeight,
                              frameWidth, frameHeight, geometry, error) &&
           geometry.covers_frame;
}

}  // namespace

Picture UprightPicture(int64_t width, int64_t height, int32_t rotationDegrees,
                       bool alpha) {
    const auto turns =
        static_cast<int32_t>(std::lround(rotationDegrees / 90.0));
    const int32_t quarterTurns = ((turns % 4) + 4) % 4;
    return quarterTurns % 2 != 0 ? Picture{height, width, alpha}
                                 : Picture{width, height, alpha};
}

std::set<Ulid> HiddenTracks(const Document& document, const Timeline& timeline,
                            RationalTime position, const PictureLookup& picture,
                            bool zoomed) {
    std::set<Ulid> hidden;
    if (zoomed || position.rate <= 0) {
        return hidden;
    }
    try {
        const std::vector<const DocumentTrack*> tracks =
            CompositedTracks(document);
        bool covered = false;
        for (auto track = tracks.rbegin(); track != tracks.rend(); ++track) {
            if (covered) {
                hidden.insert((*track)->id);
            } else {
                covered =
                    CoversFrame(document, timeline, **track, position, picture);
            }
        }
    } catch (const std::exception&) {
        // Undecidable is shown.
        hidden.clear();
    }
    return hidden;
}

std::map<Ulid, RationalTime> NextShown(
    const Document& document, const Timeline& timeline, RationalTime position,
    int direction, RationalTime lookahead, const std::set<Ulid>& hidden,
    const PictureLookup& picture, bool zoomed) {
    std::map<Ulid, RationalTime> shown;
    if (hidden.empty() || direction == 0 || position.rate <= 0 ||
        lookahead.rate <= 0 || lookahead.value <= 0) {
        return shown;
    }
    try {
        const MediaRate rate = document.sequence.frame_rate;
        if (rate.num <= 0 || rate.den <= 0) {
            return shown;
        }
        const __int128 numerator = static_cast<__int128>(lookahead.value) *
                                   std::abs(direction) * rate.num;
        const __int128 denominator =
            static_cast<__int128>(lookahead.rate) * rate.den;
        const int64_t frames = static_cast<int64_t>(std::min<__int128>(
            (numerator + denominator - 1) / denominator, kMaxWindowFrames));
        const RationalTime end = timeline.Duration();
        std::set<Ulid> pending = hidden;
        for (int64_t step = 1; step <= frames && !pending.empty(); ++step) {
            const RationalTime offset{step * rate.den, rate.num};
            const RationalTime at =
                direction > 0 ? position.add(offset) : position.sub(offset);
            if (at < RationalTime{0, 1} || at >= end) {
                break;
            }
            const std::set<Ulid> still =
                HiddenTracks(document, timeline, at, picture, zoomed);
            for (auto track = pending.begin(); track != pending.end();) {
                if (still.count(*track) == 0) {
                    shown.emplace(*track, at);
                    track = pending.erase(track);
                } else {
                    ++track;
                }
            }
        }
    } catch (const std::exception&) {
        // What was found stands; the rest is asked for once shown.
        return shown;
    }
    return shown;
}

}  // namespace monitor_occlusion
