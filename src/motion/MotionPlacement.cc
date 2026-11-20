#include "MotionPlacement.h"

#include "MotionClip.h"
#include "Timeline.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace {

bool Refuse(std::string& errorName, std::string& error, const char* name,
            std::string text) {
    errorName = name;
    error = std::move(text);
    return false;
}

// Whole frames of the sequence, or nothing: a placement between two frames
// would be a second answer to where the frame is.
bool WholeFrames(const RationalTime& time, const MediaRate& rate) {
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    return denominator != 0 && numerator % denominator == 0;
}

// The first frame of the sequence that contains `time`, and the first that
// starts at or after it: a passage's edges, from the words' exact times.
RationalTime FrameContaining(const RationalTime& time, const MediaRate& rate) {
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    return {static_cast<int64_t>(numerator / denominator) * rate.den, rate.num};
}

RationalTime FrameAtOrAfter(const RationalTime& time, const MediaRate& rate) {
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    const __int128 frames = (numerator + denominator - 1) / denominator;
    return {static_cast<int64_t>(frames) * rate.den, rate.num};
}

// Where the shot `clip` belongs to ends: the clips of the same source that
// follow it on its track without a gap. An interview cut in jump cuts is one
// person on screen from the first to the last of them, and a name posed on
// the first must not stop at the first cut.
RationalTime ShotEnd(const DocumentTrack& track, const DocumentClip& clip) {
    RationalTime end = clip.timeline_in.add(clip.duration);
    bool extended = true;
    while (extended) {
        extended = false;
        for (const DocumentClip& next : track.clips) {
            if (next.source_id == clip.source_id && next.timeline_in == end &&
                next.duration.value > 0) {
                end = next.timeline_in.add(next.duration);
                extended = true;
            }
        }
    }
    return end;
}

bool Overlaps(const DocumentTrack& track, const RationalTime& start,
              const RationalTime& end) {
    return std::any_of(
        track.clips.begin(), track.clips.end(), [&](const DocumentClip& clip) {
            const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
            return clip.timeline_in < end && clipEnd > start;
        });
}

}  // namespace

bool ResolveMotionPlacement(const Document& document,
                            const MotionPlacementRequest& request,
                            MotionPlacement& placement, std::string& errorName,
                            std::string& error) {
    placement = MotionPlacement{};
    errorName.clear();
    error.clear();
    const DocumentComposition* composition =
        document.FindComposition(request.composition_id);
    if (composition == nullptr) {
        return Refuse(
            errorName, error, "UnknownComposition",
            "unknown composition_id '" + request.composition_id + "'");
    }
    DocumentSource source;
    MotionError motion;
    if (!CompositionAsSource(*composition, source, motion)) {
        return Refuse(errorName, error, "InvalidMotion",
                      motion.code + ": " + motion.message);
    }
    placement.composition_duration = source.duration;
    const MediaRate rate = document.sequence.frame_rate;
    RationalTime filmEnd{0, 1};
    try {
        filmEnd = Timeline(document).Duration();
    } catch (const std::exception& exception) {
        return Refuse(errorName, error, "InvalidTimelineIn", exception.what());
    }
    if (request.offset.value < 0) {
        return Refuse(errorName, error, "InvalidTimelineIn",
                      "offset must not be negative");
    }

    // The anchor: where the instance starts, how long it may last, and the
    // track index it must sit above.
    RationalTime start{0, 1};
    RationalTime room{0, 1};
    int32_t aboveIndex = std::numeric_limits<int32_t>::min();
    if (request.anchor == MotionAnchor::Clip) {
        const DocumentClip* anchor = nullptr;
        const DocumentTrack* anchorTrack = nullptr;
        for (const DocumentTrack& track : document.sequence.tracks) {
            for (const DocumentClip& clip : track.clips) {
                if (clip.id == request.clip_id) {
                    anchor = &clip;
                    anchorTrack = &track;
                }
            }
        }
        if (anchor == nullptr) {
            return Refuse(errorName, error, "UnknownClip",
                          "unknown clip_id '" + request.clip_id + "'");
        }
        if (!(request.offset < anchor->duration)) {
            return Refuse(
                errorName, error, "InvalidTimelineIn",
                "offset falls past the end of clip '" + anchor->id + "'");
        }
        start = anchor->timeline_in.add(request.offset);
        room = ShotEnd(*anchorTrack, *anchor).sub(start);
        // A motion clip goes over the picture: above the clip's own track
        // when it is a video track, above the bottom one when it is sound.
        if (anchorTrack->kind == "video") {
            aboveIndex = anchorTrack->index;
        }
    } else if (request.anchor == MotionAnchor::Span) {
        // The clip that plays the passage's start: its picture when the
        // source has one there, else its sound.
        const DocumentClip* anchor = nullptr;
        const DocumentTrack* anchorTrack = nullptr;
        for (const DocumentTrack& track : document.sequence.tracks) {
            for (const DocumentClip& clip : track.clips) {
                if (clip.source_id != request.span_source_id ||
                    request.span_in < clip.timeline_in ||
                    !(request.span_in < clip.timeline_in.add(clip.duration))) {
                    continue;
                }
                if (anchor == nullptr ||
                    (anchorTrack->kind != "video" && track.kind == "video")) {
                    anchor = &clip;
                    anchorTrack = &track;
                }
            }
        }
        if (anchor == nullptr) {
            return Refuse(errorName, error, "UnknownClip",
                          "no clip of source '" + request.span_source_id +
                              "' plays the passage's start on this timeline");
        }
        start = FrameContaining(request.span_in, rate).add(request.offset);
        if (request.span_end) {
            const RationalTime end = FrameAtOrAfter(*request.span_end, rate);
            if (!(start < end)) {
                return Refuse(errorName, error, "InvalidTimelineIn",
                              "offset falls past the end of the passage");
            }
            room = end.sub(start);
        } else {
            room = ShotEnd(*anchorTrack, *anchor).sub(start);
        }
        if (anchorTrack->kind == "video") {
            aboveIndex = anchorTrack->index;
        }
    } else if (request.anchor == MotionAnchor::Start) {
        start = request.offset;
        room = filmEnd.sub(start);
    } else {
        if (request.offset.value != 0) {
            return Refuse(errorName, error, "InvalidTimelineIn",
                          "an instance anchored at the end ends on the film's "
                          "last frame: give no offset");
        }
        room = filmEnd;
    }
    if (!(room.value > 0)) {
        return Refuse(errorName, error, "InvalidTimelineIn",
                      "the anchor leaves no room on the timeline");
    }
    // Omitted, the duration is what the anchor and the composition both
    // allow, down to a whole frame of the sequence: a composition at another
    // cadence need not end on one, and the part of a frame it would add
    // shows nothing.
    RationalTime duration{0, 1};
    if (request.duration) {
        duration = *request.duration;
    } else if (request.anchor == MotionAnchor::Span && request.span_end) {
        duration = room;
        if (duration > source.duration) {
            return Refuse(errorName, error, "InvalidTimelineIn",
                          "the passage lasts longer than the composition: "
                          "give a duration, or a shorter passage");
        }
    } else {
        const RationalTime longest = std::min(room, source.duration);
        const __int128 frames =
            (static_cast<__int128>(longest.value) * rate.num) /
            (static_cast<__int128>(longest.rate) * rate.den);
        duration = {static_cast<int64_t>(frames) * rate.den, rate.num};
    }
    if (duration > source.duration) {
        return Refuse(errorName, error, "InvalidTimelineIn",
                      "duration is longer than the composition");
    }
    if (duration > room) {
        return Refuse(errorName, error, "InvalidTimelineIn",
                      "duration runs past the end of its anchor");
    }
    if (request.anchor == MotionAnchor::End) {
        start = filmEnd.sub(duration);
    }
    if (!(duration.value > 0) || !WholeFrames(start, rate) ||
        !WholeFrames(duration, rate)) {
        return Refuse(errorName, error, "InvalidTimelineIn",
                      "the instance would not start and end on whole frames "
                      "of the timeline");
    }
    const RationalTime end = start.add(duration);
    placement.timeline_in = start;
    placement.duration = duration;

    if (!request.track_id.empty()) {
        const DocumentTrack* track = document.FindTrack(request.track_id);
        if (track == nullptr || track->kind != "video") {
            return Refuse(errorName, error, "UnknownTrack",
                          "track_id '" + request.track_id +
                              "' is not a video track of this timeline");
        }
        if (Overlaps(*track, start, end)) {
            return Refuse(errorName, error, "InvalidTimelineIn",
                          "the range is taken on track '" + track->id +
                              "': a motion clip is laid over the picture, "
                              "never over another clip");
        }
        placement.track_id = track->id;
        return true;
    }
    std::vector<const DocumentTrack*> video;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video") {
            video.push_back(&track);
        }
    }
    std::stable_sort(video.begin(), video.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    // For the film's edges, and for a clip on a sound track, the picture's
    // bottom track is the one to stay above.
    if (aboveIndex == std::numeric_limits<int32_t>::min() && !video.empty()) {
        aboveIndex = video.front()->index;
    }
    for (const DocumentTrack* track : video) {
        if (track->index <= aboveIndex) {
            continue;
        }
        if (!Overlaps(*track, start, end)) {
            placement.track_id = track->id;
            return true;
        }
    }
    return Refuse(errorName, error, "NoFreeTrack",
                  "no video track above the anchor is free over that range: "
                  "add one (add_track, kind video) and place the instance "
                  "again");
}
