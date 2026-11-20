#include "Timeline.h"

#include "ClipAnimation.h"
#include "ClipEnabled.h"
#include "ClipSpeed.h"
#include "MotionClip.h"

#include <algorithm>
#include <stdexcept>

Timeline::Timeline(const Document& document) : document_(document) {}

namespace {

// R2c -- docs/roadmap/REEL-2026-09.md. The opacity the clip shows at
// `position`: keyed, it is the key's value, which replaces the constant as
// it does in the export (Export.cc samples the same function). The monitor
// used to multiply the constant by the sampled value, so a clip at 50 %
// with keys, or with only its framing keyed, was shown darker than it was
// delivered.
float ClipOpacity(const DocumentClip& clip, RationalTime position,
                  const MediaRate& rate) {
    EffectParamValue opacity = clip.opacity;
    if (ClipAnimates(clip, ClipAnimatedParameter::Opacity)) {
        ClipSampledParameters sampled;
        std::string error;
        if (SampleClipParameters(
                clip,
                position.sub(clip.timeline_in).to_frames(rate.num, rate.den),
                rate, sampled, error)) {
            opacity = sampled.opacity;
        }
    }
    return static_cast<float>(opacity.num) / static_cast<float>(opacity.den);
}

}  // namespace

std::vector<TrackResolution> Timeline::Resolve(RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    std::vector<TrackResolution> result;
    result.reserve(document_.sequence.tracks.size());
    for (const DocumentTrack& track : document_.sequence.tracks)
        result.push_back({track.id, track.kind == "caption"
                                        ? std::optional<ResolvedFrame>{}
                                        : ResolveInTrack(track, position)});
    return result;
}

std::optional<ResolvedFrame> Timeline::ResolveTrack(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0) {
        throw std::invalid_argument("timeline position rate must be positive");
    }
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track) {
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    }
    return track->kind == "caption" ? std::optional<ResolvedFrame>{}
                                    : ResolveInTrack(*track, position);
}

const DocumentSource& Timeline::SourceOf(const DocumentClip& clip) const {
    if (!IsMotionClip(clip)) {
        const DocumentSource* source = document_.FindSource(clip.source_id);
        if (source == nullptr) {
            throw std::logic_error(
                "validated clip references an unknown source");
        }
        return *source;
    }
    auto found = composition_sources_.find(clip.composition_id);
    if (found == composition_sources_.end()) {
        std::optional<DocumentSource> source = ClipSource(document_, clip);
        if (!source) {
            throw std::logic_error(
                "validated motion clip references an unknown composition");
        }
        found =
            composition_sources_.emplace(clip.composition_id, *source).first;
    }
    return found->second;
}

ResolvedFrame Timeline::ResolveClipAt(const DocumentClip& clip,
                                      RationalTime sourceTime) const {
    const DocumentSource& source = SourceOf(clip);
    return {source.id, sourceTime.to_frames(source.rate.num, source.rate.den),
            clip.id};
}

std::vector<ResolvedLayer> Timeline::ResolveTrackLayers(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0)
        throw std::invalid_argument("timeline position rate must be positive");
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track)
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    if (track->kind == "caption") return {};
    for (const DocumentTransition& transition :
         document_.sequence.transitions) {
        if (transition.track_id != trackId) continue;
        // R5 -- a colour dip overlaps nothing: it veils the layers this
        // function returns, which ResolveTrackColorVeil below reports
        // separately.
        if (TransitionIsColorDip(transition)) continue;
        const DocumentClip* left = document_.FindClip(transition.left_clip_id);
        const DocumentClip* right =
            document_.FindClip(transition.right_clip_id);
        if (!left || !right) continue;
        // R5 -- where the transition falls, by the one function
        // Document::Validate, the export and verify_export also ask.
        TransitionSpan span;
        std::string spanError;
        if (!ResolveTransitionSpan(document_.sequence, transition, span,
                                   spanError))
            continue;
        const MediaRate rate = document_.sequence.frame_rate;
        const RationalTime pre{span.pre_frames * rate.den, rate.num};
        const RationalTime post{span.post_frames * rate.den, rate.num};
        const RationalTime cut = right->timeline_in;
        const RationalTime start = cut.sub(pre);
        const RationalTime end = cut.add(post);
        if (position < start || position >= end) continue;
        const RationalTime fromCut = position.sub(cut);
        // R3 -- a dissolve reads both shots past their cut, in their own
        // source time (ClipSpeed.h). Document::Validate refuses a
        // transition on a clip that is not at 1/1, so these are identities
        // today; they are written through the helpers so that the day R5
        // lifts that refusal, this reads the same source frame as the
        // export does.
        const RationalTime leftTime =
            ClipSourceTimeAt(*left, left->duration.add(fromCut));
        const RationalTime rightTime = ClipSourceTimeAt(*right, fromCut);
        // R5 -- the curve is TransitionRender.h's and only its: k over
        // n, exact, turned into a float once here. Before this ticket the
        // monitor divided two long doubles and the export handed FFmpeg a
        // `fade` ramp, and nothing compared the two.
        const float progress = TransitionAlphaFloat(CrossDissolveProgress(
            position.sub(start).to_frames(rate.num, rate.den), span.frames()));
        // F1.5 -- an inactive multicam angle drops out of its dissolve the
        // way Export.cc drops its input: the other side keeps its own
        // extension and fade, over whatever lies underneath. A disabled
        // clip drops out the same way (ClipEnabled.h).
        std::vector<ResolvedLayer> layers;
        if (ClipIsSeen(document_.sequence, *left)) {
            layers.push_back({ResolveClipAt(*left, leftTime),
                              ClipOpacity(*left, position, rate)});
        }
        if (ClipIsSeen(document_.sequence, *right)) {
            layers.push_back({ResolveClipAt(*right, rightTime),
                              progress * ClipOpacity(*right, position, rate)});
        }
        return layers;
    }
    const std::optional<ResolvedFrame> frame = ResolveInTrack(*track, position);
    if (!frame) return {};
    const DocumentClip* clip = document_.FindClip(frame->clip_id);
    return clip ? std::vector<ResolvedLayer>{{*frame,
                                              ClipOpacity(*clip, position,
                                                          document_.sequence
                                                              .frame_rate)}}
                : std::vector<ResolvedLayer>{};
}

std::optional<ResolvedColorVeil> Timeline::ResolveTrackColorVeil(
    const Ulid& trackId, RationalTime position) const {
    if (position.rate <= 0)
        throw std::invalid_argument("timeline position rate must be positive");
    const DocumentTrack* track = document_.FindTrack(trackId);
    if (!track)
        throw std::invalid_argument("unknown track ID '" + trackId + "'");
    if (track->kind != "video") return std::nullopt;
    const MediaRate rate = document_.sequence.frame_rate;
    return ::ResolveTrackColorVeil(document_.sequence, trackId,
                                   position.to_frames(rate.num, rate.den));
}

std::vector<ResolvedFrame> Timeline::ResolveUpcoming(
    RationalTime position, int direction, RationalTime lookahead) const {
    if (position.rate <= 0)
        throw std::invalid_argument("timeline position rate must be positive");
    if (lookahead.rate <= 0)
        throw std::invalid_argument("lookahead rate must be positive");
    std::vector<ResolvedFrame> upcoming;
    if (direction == 0 || lookahead.value <= 0) return upcoming;
    for (const DocumentTrack& track : document_.sequence.tracks) {
        // Only the tracks that actually feed the composite, which is what
        // main.mm builds its video slots from: warming the decoder of a
        // hidden or audio track would spend a seek on an image nothing is
        // going to draw.
        if (track.kind != "video" || !track.visible) continue;
        const std::optional<ResolvedFrame> frame =
            ResolveUpcomingInTrack(track, position, direction, lookahead);
        if (frame) upcoming.push_back(*frame);
    }
    return upcoming;
}

std::optional<ResolvedFrame> Timeline::ResolveUpcomingInTrack(
    const DocumentTrack& track, RationalTime position, int direction,
    RationalTime lookahead) const {
    // Clips are ordered and never overlap (Document::Validate), so the first
    // one found in the direction of travel is the next one entered.
    if (direction > 0) {
        const RationalTime limit = position.add(lookahead);
        for (const DocumentClip& clip : track.clips) {
            if (clip.timeline_in <= position) continue;
            if (clip.timeline_in > limit) break;
            if (!ClipIsSeen(document_.sequence, clip)) {
                continue;
            }
            return ResolveClipAt(clip, clip.source_in);
        }
        return std::nullopt;
    }
    const RationalTime limit = position.sub(lookahead);
    for (auto clip = track.clips.rbegin(); clip != track.clips.rend(); ++clip) {
        const RationalTime end = clip->timeline_in.add(clip->duration);
        if (end > position) continue;
        if (end < limit) break;
        if (!ClipIsSeen(document_.sequence, *clip)) {
            continue;
        }
        // Played backward, a clip is entered by its last frame.
        ResolvedFrame frame = ResolveClipAt(*clip, end);
        frame.source_frame = LastSourceFrame(*clip);
        return frame;
    }
    return std::nullopt;
}

int64_t Timeline::LastSourceFrame(const DocumentClip& clip) const {
    const DocumentSource* source = &SourceOf(clip);
    // R3, criterion 4 -- a ramp can end on a stop, where the last frame
    // shows the source at its very end: the rule itself names that frame.
    if (ClipHasSpeedRamp(clip)) {
        const MediaRate rate = document_.sequence.frame_rate;
        return ClipSourceFrameAt(
            clip, clip.duration.sub(RationalTime{rate.den, rate.num}),
            source->rate);
    }
    const RationalTime end = ClipSourceEnd(clip);
    const int64_t frame = end.to_frames(source->rate.num, source->rate.den);
    // Ranges are half-open here as everywhere else: an end landing exactly on
    // a source frame boundary opens that frame rather than containing it, so
    // the clip's last frame is the one before.
    const RationalTime frameStart{
        frame * static_cast<int64_t>(source->rate.den), source->rate.num};
    return frameStart == end ? frame - 1 : frame;
}

std::optional<ResolvedFrame> Timeline::ResolveInTrack(
    const DocumentTrack& track, RationalTime position) const {
    // upper_bound finds the last clip whose start is <= position. Validation
    // guarantees ordering and no overlap, so only that clip can contain it.
    const auto after = std::upper_bound(
        track.clips.begin(), track.clips.end(), position,
        [](const RationalTime& value, const DocumentClip& clip) {
            return value < clip.timeline_in;
        });
    if (after == track.clips.begin()) {
        return std::nullopt;
    }
    const DocumentClip& clip = *std::prev(after);
    const RationalTime offset = position.sub(clip.timeline_in);
    if (offset.value < 0 || offset >= clip.duration) {
        return std::nullopt;
    }
    // F1.5 -- ROADMAP.md. An inactive multicam angle leaves a hole in its
    // track, like a hidden track; see Multicam.h. So does a disabled clip
    // (ClipEnabled.h).
    if (!ClipIsSeen(document_.sequence, clip)) {
        return std::nullopt;
    }
    // R3 -- docs/roadmap/REEL-2026-09.md. The one rule: the source frame a
    // clip shows `offset` into its own span, its speed -- or its ramp --
    // applied (ClipSpeed.h). The export, the sheets and verify_export reach
    // it through this same resolution.
    return ResolvedFrame{SourceOf(clip).id,
                         ClipSourceFrameAt(clip, offset, SourceOf(clip).rate),
                         clip.id};
}

RationalTime Timeline::Duration() const {
    RationalTime duration{0, 1};
    RationalTime timebase{0, 1};
    for (const DocumentTrack& track : document_.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            timebase = timebase.add(RationalTime{0, clip.timeline_in.rate});
            timebase = timebase.add(RationalTime{0, clip.duration.rate});
            const RationalTime end = clip.timeline_in.add(clip.duration);
            if (end > duration) {
                duration = end;
            }
        }
    }
    return duration.rescale(timebase.rate);
}
