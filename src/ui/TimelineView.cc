#include "TimelineView.h"

#include "ClipBadges.h"
#include "ClipSpeed.h"
#include "MotionClip.h"
#include "TransitionRender.h"
#include "UiTheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {

void CheckViewport(const TimelineViewport& viewport) {
    if (viewport.view_start.rate <= 0 ||
        !std::isfinite(viewport.pixels_per_second) ||
        viewport.pixels_per_second <= 0.0 ||
        !std::isfinite(viewport.track_height) || viewport.track_height <= 0.0 ||
        !std::isfinite(viewport.header_width)) {
        throw std::invalid_argument("invalid timeline viewport");
    }
}

int64_t RoundedInt64(long double value) {
    if (!std::isfinite(value) ||
        value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        throw std::overflow_error("timeline coordinate is outside int64 range");
    }
    // Round to nearest frame/tick. Halfway values are rounded away from zero,
    // matching std::round and making the policy explicit for negative views.
    return static_cast<int64_t>(std::round(value));
}

void ClampViewportStartToZero(TimelineViewport& viewport) {
    if (viewport.view_start < RationalTime{0, 1})
        viewport.view_start = {0, viewport.view_start.rate};
}

double DominantScrollDelta(double deltaX, double deltaY) {
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY)) return 0.0;
    return std::abs(deltaX) > 0.01 ? deltaX : deltaY;
}

long double Seconds(const RationalTime& time) {
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    return static_cast<long double>(time.value) / time.rate;
}

long double ZoomBarRulerSeconds(long double durationSeconds,
                                long double startSeconds,
                                long double visibleSeconds) {
    // The rail stands for the program, stretched to the end of the view when
    // the view reaches past it. Continuous in the zoom: a thumb whose width
    // jumps cannot be held by its handle (TimelineView.h).
    return std::max(durationSeconds, startSeconds + visibleSeconds);
}

int64_t RoundNonNegativeRatio(__int128 numerator, __int128 denominator) {
    if (numerator < 0 || denominator <= 0)
        throw std::invalid_argument("invalid playhead quantization ratio");
    const __int128 quotient = numerator / denominator;
    const __int128 remainder = numerator % denominator;
    const __int128 rounded = quotient + (remainder * 2 >= denominator ? 1 : 0);
    if (rounded > std::numeric_limits<int64_t>::max())
        throw std::overflow_error("playhead position exceeds int64 range");
    return static_cast<int64_t>(rounded);
}

ExactClipTimes ProposedTimes(const DocumentClip& clip, TrimEdge edge,
                             const RationalTime& delta) {
    ExactClipTimes result{clip.source_in, clip.duration, clip.timeline_in};
    if (edge == TrimEdge::Head) {
        // R3 -- docs/roadmap/REEL-2026-09.md. A head trim of one timeline
        // frame advances the source by that frame times the clip's speed,
        // which is what ApplyTrim stores: the preview has to say the same
        // thing the edit will do. (The drag limits below still measure the
        // remaining handles at 1/1; on a speed-changed clip a drag can
        // therefore propose a trim the operation refuses by name.)
        result.source_in = result.source_in.add(ClipSourceAdvance(clip, delta));
        result.duration = result.duration.sub(delta);
        result.timeline_in = result.timeline_in.add(delta);
    } else {
        result.duration = result.duration.add(delta);
    }
    return result;
}

RationalTime MaxTime(const RationalTime& left, const RationalTime& right) {
    return left < right ? right : left;
}

RationalTime MinTime(const RationalTime& left, const RationalTime& right) {
    return left > right ? right : left;
}

std::vector<const DocumentTrack*> TracksOfKindInEditOrder(
    const Document& document, const std::string& kind) {
    std::vector<const DocumentTrack*> result;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == kind) result.push_back(&track);
    std::stable_sort(result.begin(), result.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return result;
}

std::optional<int64_t> TrackOrdinal(const Document& document,
                                    const DocumentTrack& track) {
    const auto tracks = TracksOfKindInEditOrder(document, track.kind);
    const auto found = std::find(tracks.begin(), tracks.end(), &track);
    if (found == tracks.end()) return std::nullopt;
    return static_cast<int64_t>(std::distance(tracks.begin(), found));
}

}  // namespace

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate, int32_t sampleRate) {
    if (position.rate <= 0 || position.value < 0)
        throw std::invalid_argument("playhead position must be non-negative");
    if (resolution == PlayheadResolution::Sample) {
        if (sampleRate <= 0)
            throw std::invalid_argument("sample rate must be positive");
        const int64_t sample = RoundNonNegativeRatio(
            static_cast<__int128>(position.value) * sampleRate, position.rate);
        return {sample, sampleRate};
    }
    if (frameRate.num <= 0 || frameRate.den <= 0)
        throw std::invalid_argument("frame rate must be positive");
    const int64_t frame = RoundNonNegativeRatio(
        static_cast<__int128>(position.value) * frameRate.num,
        static_cast<__int128>(position.rate) * frameRate.den);
    if (static_cast<__int128>(frame) * frameRate.den >
        std::numeric_limits<int64_t>::max())
        throw std::overflow_error("frame position exceeds int64 range");
    return {frame * frameRate.den, frameRate.num};
}

double TimelineViewport::TimeToX(RationalTime time) const {
    CheckViewport(*this);
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    const long double seconds =
        static_cast<long double>(time.value) / time.rate -
        static_cast<long double>(view_start.value) / view_start.rate;
    return header_width + static_cast<double>(seconds * pixels_per_second);
}

RationalTime TimelineViewport::XToTime(double x, int32_t rate) const {
    CheckViewport(*this);
    if (!std::isfinite(x) || rate <= 0)
        throw std::invalid_argument("invalid coordinate or requested rate");
    const long double seconds =
        static_cast<long double>(view_start.value) / view_start.rate +
        (static_cast<long double>(x) - header_width) / pixels_per_second;
    return {RoundedInt64(seconds * rate), rate};
}

void TimelineViewport::ScrollByPixels(double delta_x, int32_t rate) {
    view_start = XToTime(header_width + delta_x, rate);
    ClampViewportStartToZero(*this);
}

void TimelineViewport::ZoomAroundX(double x, double factor, int32_t rate) {
    if (!std::isfinite(factor) || factor <= 0.0)
        throw std::invalid_argument("zoom factor must be positive");
    CheckViewport(*this);
    if (!std::isfinite(x) || rate <= 0)
        throw std::invalid_argument("invalid coordinate or requested rate");
    // The time under x is kept unrounded across the change of scale and the
    // new start rounded once. Rounding that time to `rate` before and after
    // moved the very point the zoom holds still, by up to a tick each way.
    const long double offset = static_cast<long double>(x) - header_width;
    const long double anchor = Seconds(view_start) + offset / pixels_per_second;
    pixels_per_second =
        std::clamp(pixels_per_second * factor, kTimelineMinPixelsPerSecond,
                   kTimelineMaxPixelsPerSecond);
    const long double start =
        std::max(0.0L, anchor - offset / pixels_per_second);
    view_start = {RoundedInt64(start * rate), rate};
}

double TimelineZoomOutFloor(const TimelineViewport& viewport,
                            RationalTime duration, double width) {
    CheckViewport(viewport);
    const double rail = width - viewport.header_width;
    if (!std::isfinite(rail) || rail <= 0.0 || duration.rate <= 0 ||
        duration.value <= 0)
        return kTimelineMinPixelsPerSecond;
    return std::clamp(static_cast<double>(rail * 0.5L / Seconds(duration)),
                      kTimelineMinPixelsPerSecond, kTimelineMaxPixelsPerSecond);
}

namespace {

// Changes the scale by `factor`, no further out than the floor, and puts
// `anchor.time` back at `anchor.x`. Returns the anchor with the view it left.
TimelineZoomAnchor ApplyAnchoredZoom(TimelineViewport& viewport,
                                     TimelineZoomAnchor anchor, double factor,
                                     RationalTime duration, double width) {
    if (!std::isfinite(factor) || factor <= 0.0)
        throw std::invalid_argument("zoom factor must be positive");
    // A view already further out than the floor (a window since resized)
    // is not pulled in by a zoom out: it only goes no further.
    const double floor =
        std::min(viewport.pixels_per_second,
                 TimelineZoomOutFloor(viewport, duration, width));
    viewport.pixels_per_second = std::clamp(viewport.pixels_per_second * factor,
                                            floor, kTimelineMaxPixelsPerSecond);
    long double start =
        Seconds(anchor.time) -
        (static_cast<long double>(anchor.x) - viewport.header_width) /
            viewport.pixels_per_second;
    // The program's end goes no further left than the middle of the view:
    // traced at the floor, a view began at 9 s, the film's opening out of
    // sight on the left of a window more than half empty on the right. The
    // anchor is kept as it was taken, so zooming back in undoes this too.
    if (duration.rate > 0 && duration.value > 0) {
        const long double visible =
            std::max(0.0, width - viewport.header_width) /
            viewport.pixels_per_second;
        start = std::min(start, Seconds(duration) - visible * 0.5L);
    }
    start = std::max(0.0L, start);
    viewport.view_start = {RoundedInt64(start * kTimelineViewRate),
                           kTimelineViewRate};
    anchor.pixels_per_second = viewport.pixels_per_second;
    anchor.view_start = viewport.view_start;
    return anchor;
}

bool ViewUnmovedSince(const TimelineViewport& viewport,
                      const TimelineZoomAnchor& held) {
    return held.pixels_per_second == viewport.pixels_per_second &&
           held.view_start == viewport.view_start;
}

}  // namespace

TimelineZoomAnchor ZoomTimelineAtPointer(
    TimelineViewport& viewport, double x, double factor, RationalTime duration,
    double width, const std::optional<TimelineZoomAnchor>& held) {
    CheckViewport(viewport);
    if (!std::isfinite(x))
        throw std::invalid_argument("invalid zoom coordinate");
    TimelineZoomAnchor anchor;
    if (held && std::abs(x - held->x) <= kTimelineZoomAnchorSlop &&
        ViewUnmovedSince(viewport, *held)) {
        anchor = *held;
    } else {
        anchor.x = x;
        anchor.time = viewport.XToTime(x, kTimelineViewRate);
        if (anchor.time < RationalTime{0, 1})
            anchor.time = {0, kTimelineViewRate};
        if (duration.rate > 0 && duration.value > 0 && anchor.time > duration)
            anchor.time = duration;
    }
    return ApplyAnchoredZoom(viewport, anchor, factor, duration, width);
}

TimelineZoomAnchor ZoomTimelineAtPlayhead(
    TimelineViewport& viewport, const RationalTime& playhead, double factor,
    RationalTime duration, double width,
    const std::optional<TimelineZoomAnchor>& held) {
    CheckViewport(viewport);
    if (playhead.rate <= 0 || playhead.value < 0)
        throw std::invalid_argument("playhead position must be non-negative");
    TimelineZoomAnchor anchor;
    if (held && held->time == playhead && ViewUnmovedSince(viewport, *held)) {
        anchor = *held;
    } else {
        anchor.time = playhead;
        const double x = viewport.TimeToX(playhead);
        const double rail = std::max(0.0, width - viewport.header_width);
        // Out of sight, the playhead comes to the middle of the view.
        anchor.x = x >= viewport.header_width && x <= width
                       ? x
                       : viewport.header_width + rail * 0.5;
    }
    return ApplyAnchoredZoom(viewport, anchor, factor, duration, width);
}

void TimelineViewport::FitDuration(RationalTime duration, double width) {
    CheckViewport(*this);
    if (duration.rate <= 0 || !std::isfinite(width) || width <= header_width) {
        throw std::invalid_argument("invalid duration or viewport width");
    }
    view_start = {0, duration.rate};
    if (duration.value <= 0) {
        pixels_per_second = 100.0;
        return;
    }
    const long double seconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double available = std::max(1.0, width - header_width - 24.0);
    pixels_per_second =
        std::clamp(static_cast<double>(available / seconds),
                   kTimelineMinPixelsPerSecond, kTimelineMaxPixelsPerSecond);
}

TimelineFit FitTimeline(TimelineViewport& viewport, RationalTime duration,
                        double width) {
    viewport.FitDuration(duration, width);
    TimelineFit fit;
    fit.made = true;
    fit.width = width;
    fit.view_start = viewport.view_start;
    fit.pixels_per_second = viewport.pixels_per_second;
    return fit;
}

bool TimelineFitFollowsResize(const TimelineFit& fit,
                              const TimelineViewport& viewport, double width) {
    if (!std::isfinite(width) || width <= viewport.header_width) return false;
    if (!fit.made) return true;
    return width != fit.width && viewport.view_start == fit.view_start &&
           viewport.pixels_per_second == fit.pixels_per_second;
}

TimelineViewMemory RememberTimelineView(const TimelineViewport& viewport,
                                        const TimelineFit& fit) {
    TimelineViewMemory memory;
    memory.view_start = viewport.view_start;
    memory.pixels_per_second = viewport.pixels_per_second;
    memory.fit = fit;
    return memory;
}

TimelineFit OpenTimelineView(TimelineViewport& viewport,
                             const TimelineViewMemory* left,
                             RationalTime duration, double width) {
    if (left != nullptr && left->view_start < duration) {
        viewport.view_start = left->view_start;
        viewport.pixels_per_second = left->pixels_per_second;
        if (left->fit.made &&
            TimelineFitFollowsResize(left->fit, viewport, width))
            return FitTimeline(viewport, duration, width);
        return left->fit;
    }
    if (duration.rate <= 0 || !std::isfinite(width) ||
        width <= viewport.header_width)
        return {};
    return FitTimeline(viewport, duration, width);
}

TimelineScrollIntent ResolveTimelineScrollIntent(double deltaX, double deltaY,
                                                 bool preciseDeltas,
                                                 bool shiftPressed,
                                                 bool zoomModifier,
                                                 bool naturalScrolling) {
    // The deltas as the hand made them, negative away from the editor.
    const double hand = naturalScrolling ? 1.0 : -1.0;
    if (zoomModifier) {
        // Either axis zooms: the one the gesture mostly follows.
        if (!std::isfinite(deltaX) || !std::isfinite(deltaY))
            return {TimelineScrollAction::Zoom, 0.0};
        return {
            TimelineScrollAction::Zoom,
            hand * (std::abs(deltaY) >= std::abs(deltaX) ? deltaY : deltaX)};
    }
    const double delta = DominantScrollDelta(deltaX, deltaY);
    if (preciseDeltas) {
        // Precise deltas already use logical points.
        return {TimelineScrollAction::Pan, delta};
    }
    if (shiftPressed) {
        // A discrete wheel reports accelerated line units: a notch pans by
        // 32 points at least, however slowly it was turned (a slow one came
        // as 0.1 line and moved the view by 3 points).
        if (delta == 0.0) return {TimelineScrollAction::Pan, 0.0};
        const double step = std::max(32.0, std::abs(delta) * 32.0);
        return {TimelineScrollAction::Pan, delta < 0.0 ? -step : step};
    }
    return {TimelineScrollAction::Zoom, hand * delta};
}

double TimelineScrollZoomFactor(double delta, bool preciseDeltas) {
    if (!std::isfinite(delta) || delta == 0.0) return 1.0;
    // Precise deltas are points: 70 of them double the scale. A wheel's are
    // accelerated lines: one notch is 16 %, however slowly it was turned,
    // and a flick is capped at 35 % an event.
    const double exponent =
        preciseDeltas ? std::min(std::abs(delta) * 0.01, 0.30)
                      : std::clamp(std::abs(delta) * 0.15, 0.15, 0.30);
    // A negative delta zooms in, as it always has here.
    return std::exp(delta < 0.0 ? exponent : -exponent);
}

TimelineZoomBarPart TimelineZoomBarGeometry::HitTest(double x) const {
    if (!std::isfinite(x) || rail_width <= 0.0 || thumb_width <= 0.0 ||
        x < thumb_x || x > thumb_x + thumb_width)
        return TimelineZoomBarPart::None;
    const double hitWidth =
        std::min(thumb_width * 0.5, std::max(8.0, handle_width * 2.0));
    if (x <= thumb_x + hitWidth) return TimelineZoomBarPart::LeftHandle;
    if (x >= thumb_x + thumb_width - hitWidth)
        return TimelineZoomBarPart::RightHandle;
    return TimelineZoomBarPart::Body;
}

TimelineZoomBarGeometry CalculateTimelineZoomBarGeometry(
    const TimelineViewport& viewport, RationalTime duration, double width,
    double heldRulerSeconds) {
    CheckViewport(viewport);
    TimelineZoomBarGeometry geometry;
    geometry.rail_x = viewport.header_width;
    geometry.rail_width = std::max(0.0, width - viewport.header_width);
    geometry.thumb_x = geometry.rail_x;
    geometry.thumb_width = geometry.rail_width;
    if (geometry.rail_width <= 0.0) return geometry;

    const long double durationSeconds = std::max(0.0L, Seconds(duration));
    const long double visibleSeconds =
        geometry.rail_width / viewport.pixels_per_second;
    const long double rulerSeconds =
        std::isfinite(heldRulerSeconds) && heldRulerSeconds > 0.0
            ? static_cast<long double>(heldRulerSeconds)
            : ZoomBarRulerSeconds(durationSeconds,
                                  std::max(0.0L, Seconds(viewport.view_start)),
                                  visibleSeconds);
    geometry.ruler_seconds = static_cast<double>(rulerSeconds);
    if (rulerSeconds <= 0.0L) return geometry;

    const double rawWidth = geometry.rail_width *
                            static_cast<double>(visibleSeconds / rulerSeconds);
    geometry.thumb_width = std::clamp(
        rawWidth, std::min(24.0, geometry.rail_width), geometry.rail_width);
    const long double maximumStart =
        std::max(0.0L, rulerSeconds - visibleSeconds);
    if (maximumStart <= 0.0L) return geometry;
    const long double start =
        std::clamp(Seconds(viewport.view_start), 0.0L, maximumStart);
    geometry.thumb_x += (geometry.rail_width - geometry.thumb_width) *
                        static_cast<double>(start / maximumStart);
    return geometry;
}

void UpdateTimelineZoomBarDrag(TimelineViewport& viewport,
                               const TimelineZoomBarDrag& drag, double pointerX,
                               RationalTime duration, double width,
                               int32_t rate) {
    if (!std::isfinite(pointerX) || rate <= 0 ||
        drag.part == TimelineZoomBarPart::None)
        return;
    viewport = drag.initial_viewport;
    const double delta = pointerX - drag.pointer_x;
    const TimelineZoomBarGeometry& geometry = drag.initial_geometry;
    if (geometry.rail_width <= 0.0) return;

    if (drag.part == TimelineZoomBarPart::Body) {
        const double travel = geometry.rail_width - geometry.thumb_width;
        const long double visibleSeconds =
            geometry.rail_width / viewport.pixels_per_second;
        // The span the thumb was laid out in when the drag began: the thumb
        // then travels the rail the pointer sees. A geometry built by hand
        // carries none and gets the one the viewport would.
        const long double rulerSeconds =
            geometry.ruler_seconds > 0.0
                ? static_cast<long double>(geometry.ruler_seconds)
                : ZoomBarRulerSeconds(
                      std::max(0.0L, Seconds(duration)),
                      std::max(0.0L, Seconds(viewport.view_start)),
                      visibleSeconds);
        const long double maximumStart =
            std::max(0.0L, rulerSeconds - visibleSeconds);
        if (travel <= 0.0 || maximumStart <= 0.0L) return;
        const long double initialStart = Seconds(viewport.view_start);
        const long double requested =
            std::clamp(initialStart + static_cast<long double>(delta / travel) *
                                          maximumStart,
                       0.0L, maximumStart);
        viewport.view_start = {RoundedInt64(requested * rate), rate};
        return;
    }

    const double minimumWidth = std::min(24.0, geometry.rail_width);
    double requestedWidth = geometry.thumb_width;
    double anchorX = viewport.header_width;
    if (drag.part == TimelineZoomBarPart::LeftHandle) {
        requestedWidth -= delta;
        anchorX = width;
    } else {
        requestedWidth += delta;
    }
    // Keep following the pointer beyond the physical rail. Clamping the
    // requested width to the rail used to turn the rightmost position into a
    // hard "fit duration" stop, which made the bottom bar unable to zoom out
    // further even though wheel and magnify gestures still could.
    requestedWidth = std::max(requestedWidth, minimumWidth);
    if (geometry.thumb_width <= 0.0) return;
    // No further out than the wheel goes (TimelineZoomOutFloor).
    const double floor =
        std::min(viewport.pixels_per_second,
                 TimelineZoomOutFloor(viewport, duration, width));
    const double factor = std::max(geometry.thumb_width / requestedWidth,
                                   floor / viewport.pixels_per_second);
    viewport.ZoomAroundX(anchorX, factor, rate);
}

std::vector<double> TimelineViewport::TickXs(double width,
                                             double minimum_spacing) const {
    CheckViewport(*this);
    struct Step {
        int64_t value;
        int32_t rate;
    };
    static constexpr Step steps[] = {
        {1, 120}, {1, 60},  {1, 30},  {1, 24},  {1, 10},   {1, 5},
        {1, 2},   {1, 1},   {2, 1},   {5, 1},   {10, 1},   {30, 1},
        {60, 1},  {120, 1}, {300, 1}, {600, 1}, {1800, 1}, {3600, 1}};
    Step step = steps[sizeof(steps) / sizeof(steps[0]) - 1];
    for (const Step candidate : steps) {
        if ((static_cast<double>(candidate.value) / candidate.rate) *
                pixels_per_second >=
            minimum_spacing) {
            step = candidate;
            break;
        }
    }
    const __int128 numerator =
        static_cast<__int128>(view_start.value) * step.rate;
    const __int128 denominator =
        static_cast<__int128>(view_start.rate) * step.value;
    __int128 ordinal = numerator / denominator;
    if (numerator > 0 && numerator % denominator != 0) ++ordinal;
    std::vector<double> result;
    for (size_t count = 0; count < 10000; ++count, ++ordinal) {
        const __int128 tickValue = ordinal * step.value;
        if (tickValue < std::numeric_limits<int64_t>::min() ||
            tickValue > std::numeric_limits<int64_t>::max())
            break;
        const double x = TimeToX({static_cast<int64_t>(tickValue), step.rate});
        if (x > width) break;
        if (x >= header_width) result.push_back(x);
    }
    return result;
}

std::vector<const DocumentTrack*> TimelineTracksInDisplayOrder(
    const Document& document) {
    std::vector<const DocumentTrack*> tracks;
    tracks.reserve(document.sequence.tracks.size());
    for (const DocumentTrack& track : document.sequence.tracks)
        tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* a, const DocumentTrack* b) {
                         const auto rank = [](const std::string& kind) {
                             if (kind == "caption") return 0;
                             if (kind == "video") return 1;
                             return 2;
                         };
                         if (a->kind != b->kind)
                             return rank(a->kind) < rank(b->kind);
                         // Like Resolve: the highest video layer is displayed
                         // at the top, while audio counts downward from A1.
                         return a->kind == "video" ? a->index > b->index
                                                   : a->index < b->index;
                     });
    return tracks;
}

std::string TrackDisplayName(const Document& document, const Ulid& trackId) {
    const std::vector<const DocumentTrack*> tracks =
        TimelineTracksInDisplayOrder(document);
    uint32_t video = static_cast<uint32_t>(std::count_if(
        tracks.begin(), tracks.end(),
        [](const DocumentTrack* t) { return t->kind == "video"; }));
    uint32_t audio = 0;
    uint32_t caption = 0;
    for (const DocumentTrack* track : tracks) {
        const bool isVideo = track->kind == "video";
        const bool isCaption = track->kind == "caption";
        const uint32_t number = isVideo     ? video--
                                : isCaption ? ++caption
                                            : ++audio;
        if (track->id == trackId) {
            return std::string(isVideo     ? "V"
                               : isCaption ? "S"
                                           : "A") +
                   std::to_string(number);
        }
    }
    return {};
}

std::optional<TimelineHit> HitTestTimeline(const Document& document,
                                           const TimelineViewport& viewport,
                                           double x, double y, double width) {
    if (x < viewport.header_width || x > width || y < kTimelineRulerHeight)
        return std::nullopt;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const size_t trackIndex = static_cast<size_t>(
        std::floor((y - kTimelineRulerHeight) / viewport.track_height));
    if (trackIndex >= tracks.size()) return std::nullopt;
    const DocumentTrack& track = *tracks[trackIndex];
    for (const DocumentClip& clip : track.clips) {
        const double left = viewport.TimeToX(clip.timeline_in);
        const double right =
            viewport.TimeToX(clip.timeline_in.add(clip.duration));
        if (right < viewport.header_width || left > width) continue;
        if (x >= left && x <= right) {
            const double headDistance = std::abs(x - left);
            const double tailDistance = std::abs(x - right);
            TimelineHitEdge edge = TimelineHitEdge::None;
            if (std::min(headDistance, tailDistance) <= kTimelineEdgeHitWidth) {
                edge = headDistance <= tailDistance ? TimelineHitEdge::Head
                                                    : TimelineHitEdge::Tail;
            }
            return TimelineHit{clip.id, track.id, edge};
        }
    }
    return std::nullopt;
}

namespace {

// FC2 -- the clip a press takes hold of, for PointerDown and TimelineHitAt
// alike: a slip takes an edge for the body. FC4 -- the window sets Slip
// only for a press TimelineHitAt found on a body.
// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. The whole sequence frames of
// `delta`, toward zero; false for a rate that is not a positive fraction.
bool SequenceFramesOf(const Document& document, const RationalTime& delta,
                      int64_t& frames) {
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0 || delta.rate <= 0) return false;
    const __int128 numerator = static_cast<__int128>(delta.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(delta.rate) * rate.den;
    const __int128 whole = numerator / denominator;
    if (whole > std::numeric_limits<int32_t>::max() ||
        whole < std::numeric_limits<int32_t>::min())
        return false;
    frames = static_cast<int64_t>(whole);
    return true;
}

// FC3 -- the gesture's operation for `frames` sequence frames, applied to a
// copy: the farthest number of frames toward `wanted` that applies, and its
// result. A slide's and a stretch's limits (a neighbour used up, a handle,
// the next clip) grow with the distance, so the boundary is found by
// bisection, as TrimSession's dynamic trim finds it. Zero always "applies":
// it is the gesture not moved.
template <typename Build>
int64_t FarthestApplying(const Document& document, int64_t wanted,
                         const Build& build, Document& reached) {
    reached = document;
    const auto attempt = [&](int64_t frames, Document& result) {
        if (frames == 0) {
            result = document;
            return true;
        }
        result = document;
        Operation operation = build(frames);
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        return ApplyOperation(result, operation, inverse, error, message);
    };
    {
        Document candidate;
        if (attempt(wanted, candidate)) {
            reached = std::move(candidate);
            return wanted;
        }
    }
    int64_t good = 0;
    int64_t bad = wanted;
    while (bad - good > 1 || good - bad > 1) {
        const int64_t middle = good + ((bad - good) / 2);
        Document candidate;
        if (attempt(middle, candidate)) {
            good = middle;
            reached = std::move(candidate);
        } else {
            bad = middle;
        }
    }
    if (good == 0) reached = document;
    return good;
}

// The clips `candidate` changed, with their new times, as a preview lists
// them.
std::vector<std::pair<Ulid, ExactClipTimes>> ChangedClipTimes(
    const Document& original, const Document& candidate) {
    std::vector<std::pair<Ulid, ExactClipTimes>> changed;
    for (const DocumentTrack& track : candidate.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            const DocumentClip* before = original.FindClip(clip.id);
            if (before != nullptr && before->source_in == clip.source_in &&
                before->duration == clip.duration &&
                before->timeline_in == clip.timeline_in)
                continue;
            changed.push_back(
                {clip.id, {clip.source_in, clip.duration, clip.timeline_in}});
        }
    }
    return changed;
}

std::optional<TimelineHit> ClipHitForTrimMode(const Document& document,
                                              const TimelineViewport& viewport,
                                              double x, double y, double width,
                                              TimelineTrimMode mode) {
    std::optional<TimelineHit> hit =
        HitTestTimeline(document, viewport, x, y, width);
    if (hit &&
        (mode == TimelineTrimMode::Slip || mode == TimelineTrimMode::Slide))
        hit->edge = TimelineHitEdge::None;
    return hit;
}

}  // namespace

std::optional<TimelineGapSelection> HitTestTimelineGap(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, int32_t timeRate) {
    if (x < viewport.header_width || x > width || y < kTimelineRulerHeight)
        return std::nullopt;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const size_t trackIndex = static_cast<size_t>(
        std::floor((y - kTimelineRulerHeight) / viewport.track_height));
    if (trackIndex >= tracks.size()) return std::nullopt;
    const DocumentTrack& track = *tracks[trackIndex];
    const RationalTime position = viewport.XToTime(x, timeRate);
    RationalTime cursor{0, timeRate};
    for (const DocumentClip& clip : track.clips) {
        if (cursor < clip.timeline_in && position >= cursor &&
            position < clip.timeline_in) {
            return TimelineGapSelection{track.id, cursor,
                                        clip.timeline_in.sub(cursor)};
        }
        cursor = clip.timeline_in.add(clip.duration);
    }
    return std::nullopt;
}

std::vector<Ulid> LassoHitTestTimeline(const Document& document,
                                       const TimelineViewport& viewport,
                                       double startX, double startY,
                                       double endX, double endY, double width) {
    const double left = std::max(viewport.header_width, std::min(startX, endX));
    const double right = std::min(width, std::max(startX, endX));
    const double top = std::max(kTimelineRulerHeight, std::min(startY, endY));
    const double bottom = std::max(startY, endY);
    std::vector<Ulid> result;
    if (right < left || bottom < top) return result;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const double clipTop =
            kTimelineRulerHeight + trackIndex * viewport.track_height + 3.0;
        const double clipBottom = clipTop + viewport.track_height - 6.0;
        if (clipBottom < top || clipTop > bottom) continue;
        for (const DocumentClip& clip : tracks[trackIndex]->clips) {
            const double clipLeft = viewport.TimeToX(clip.timeline_in);
            const double clipRight =
                viewport.TimeToX(clip.timeline_in.add(clip.duration));
            if (clipRight >= left && clipLeft <= right)
                result.push_back(clip.id);
        }
    }
    return result;
}

std::string TrimReadoutText(int64_t frames, const MediaRate& rate) {
    const int64_t fps =
        rate.num > 0 && rate.den > 0
            ? std::max<int64_t>(
                  1, std::llround(static_cast<double>(rate.num) / rate.den))
            : 25;
    const long long magnitude = static_cast<long long>(std::llabs(frames));
    const char sign = frames < 0 ? '-' : '+';
    char buffer[32];
    if (magnitude < fps)
        std::snprintf(buffer, sizeof(buffer), "%c%lld", sign, magnitude);
    else
        std::snprintf(buffer, sizeof(buffer), "%c%lld:%02lld", sign,
                      magnitude / fps, magnitude % fps);
    return buffer;
}

std::optional<TimelineTrimReadout> TimelineTrimReadoutFor(
    const Document& document, const std::optional<TimelineTrimPreview>& preview,
    const MediaRate& rate) {
    if (!preview || rate.num <= 0 || rate.den <= 0) return std::nullopt;
    const DocumentClip* clip = document.FindClip(preview->clip_id);
    if (!clip) return std::nullopt;
    const ExactClipTimes& now = preview->times;
    RationalTime moved;
    RationalTime edge;
    try {
        const RationalTime grown = now.duration.sub(clip->duration);
        const bool head = now.source_in != clip->source_in ||
                          now.timeline_in != clip->timeline_in;
        if (grown.value == 0) {
            moved = now.source_in != clip->source_in
                        ? now.source_in.sub(clip->source_in)
                        : now.timeline_in.sub(clip->timeline_in);
            edge = now.timeline_in;
        } else if (head) {
            moved = RationalTime{0, grown.rate}.sub(grown);
            edge = now.timeline_in;
        } else {
            moved = grown;
            edge = now.timeline_in.add(now.duration);
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    const long double frames =
        static_cast<long double>(moved.value) * rate.num /
        (static_cast<long double>(moved.rate) * rate.den);
    TimelineTrimReadout readout;
    readout.clip_id = clip->id;
    readout.edge = edge;
    readout.frames = static_cast<int64_t>(std::llround(frames));
    readout.text = TrimReadoutText(readout.frames, rate);
    return readout;
}

std::vector<Ulid> TimelineClipsAfter(const Document& document,
                                     const RationalTime& time) {
    std::vector<Ulid> clips;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.locked) continue;
        for (const DocumentClip& clip : track.clips)
            if (clip.timeline_in.add(clip.duration) > time)
                clips.push_back(clip.id);
    }
    return clips;
}

std::vector<Ulid> ExpandLinkedClipSelection(const Document& document,
                                            const std::vector<Ulid>& clipIds) {
    std::vector<Ulid> result;
    std::vector<Ulid> groups;
    for (const Ulid& id : clipIds) {
        const DocumentClip* clip = document.FindClip(id);
        if (!clip ||
            std::find(result.begin(), result.end(), id) != result.end())
            continue;
        result.push_back(id);
        if (!clip->link_group_id.empty() &&
            std::find(groups.begin(), groups.end(), clip->link_group_id) ==
                groups.end())
            groups.push_back(clip->link_group_id);
    }
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.link_group_id.empty() ||
                std::find(groups.begin(), groups.end(), clip.link_group_id) ==
                    groups.end() ||
                std::find(result.begin(), result.end(), clip.id) !=
                    result.end())
                continue;
            result.push_back(clip.id);
        }
    }
    return result;
}

Operation TimelineCutOperationForClip(const Document& document,
                                      const DocumentClip& clip,
                                      const RationalTime& position,
                                      bool linkedSelection) {
    if (linkedSelection && !clip.link_group_id.empty()) {
        std::vector<Ulid> members =
            ExpandLinkedClipSelection(document, {clip.id});
        members.erase(
            std::remove_if(
                members.begin(), members.end(),
                [&](const Ulid& id) {
                    const DocumentClip* member = document.FindClip(id);
                    return !member || position <= member->timeline_in ||
                           position >=
                               member->timeline_in.add(member->duration);
                }),
            members.end());
        if (members.size() > 1)
            return SplitLinkedClipsOperation{
                clip.link_group_id, members, position, {}, {}, {}, {}};
    }
    return SplitClipOperation{clip.id, position, {}};
}

std::optional<RationalTime> ClipSyncDrift(const Document& document,
                                          const Ulid& clipId) {
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip || clip->sync_anchor_clip_id.empty()) return std::nullopt;
    const DocumentClip* anchor = document.FindClip(clip->sync_anchor_clip_id);
    if (!anchor || anchor->link_group_id != clip->link_group_id)
        return std::nullopt;
    try {
        const RationalTime clipPhase = clip->timeline_in.sub(clip->source_in);
        const RationalTime anchorPhase =
            anchor->timeline_in.sub(anchor->source_in);
        return clipPhase.sub(anchorPhase).sub(clip->sync_reference_delta);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<RationalTime> TimelineEditPoints(const Document& document) {
    std::vector<RationalTime> points;
    points.push_back({0, document.sequence.frame_rate.num});
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            points.push_back(clip.timeline_in);
            points.push_back(clip.timeline_in.add(clip.duration));
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::optional<RationalTime> AdjacentTimelineEdit(const Document& document,
                                                 const RationalTime& position,
                                                 bool forward) {
    const std::vector<RationalTime> points = TimelineEditPoints(document);
    if (forward) {
        const auto next =
            std::upper_bound(points.begin(), points.end(), position);
        return next == points.end() ? std::nullopt
                                    : std::optional<RationalTime>(*next);
    }
    const auto previous =
        std::lower_bound(points.begin(), points.end(), position);
    if (previous == points.begin()) return std::nullopt;
    return *std::prev(previous);
}

std::optional<RationalTime> SnapTimelineTimeToEdit(
    const Document& document, const TimelineViewport& viewport,
    const RationalTime& candidate) {
    std::optional<RationalTime> best;
    double bestDistance = kTimelineSnapDistance + 1.0;
    for (const RationalTime& point : TimelineEditPoints(document)) {
        const double distance =
            std::abs(viewport.TimeToX(point) - viewport.TimeToX(candidate));
        if (distance <= kTimelineSnapDistance && distance < bestDistance) {
            best = point;
            bestDistance = distance;
        }
    }
    return best;
}

std::vector<TimelineMarkerFlag> VisibleTimelineMarkers(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::optional<TimelineMarkerPreview>& preview) {
    std::vector<TimelineMarkerFlag> flags;
    for (const DocumentMarker& marker : document.sequence.markers) {
        const bool previewed = preview && preview->marker_id == marker.id;
        const RationalTime time = previewed ? preview->time : marker.time;
        const RationalTime duration =
            previewed ? preview->duration : marker.duration;
        const double x = viewport.TimeToX(time);
        const double endX =
            duration.value > 0 ? viewport.TimeToX(time.add(duration)) : x;
        if (endX < viewport.header_width || x > width) continue;
        TimelineMarkerFlag flag;
        flag.marker_id = marker.id;
        flag.x = x;
        flag.end_x = endX;
        flag.flag_x = x - std::floor(kTimelineMarkerFlagWidth / 2.0);
        flag.flag_y = kTimelineToolbarHeight + kTimelineScaleHeight -
                      kTimelineMarkerFlagHeight;
        flags.push_back(std::move(flag));
    }
    return flags;
}

std::vector<TimelineMarkerFlagRow> TimelineMarkerFlagShape(
    const TimelineMarkerFlag& flag) {
    // One row a point, losing a point on each side, until a single point is
    // left under the middle: (width - 1) / 2 rows of taper.
    const int taper = static_cast<int>((kTimelineMarkerFlagWidth - 1.0) / 2.0);
    const double body = kTimelineMarkerFlagHeight - taper;
    // D6 -- a span's body runs on to where it ends; the point under it
    // still marks where it starts.
    const double bodyWidth =
        std::max(kTimelineMarkerFlagWidth, flag.end_x - flag.flag_x);
    std::vector<TimelineMarkerFlagRow> rows;
    rows.push_back({flag.flag_x, flag.flag_y, bodyWidth, body});
    for (int row = 1; row <= taper; ++row)
        rows.push_back({flag.flag_x + row, flag.flag_y + body + (row - 1),
                        kTimelineMarkerFlagWidth - 2.0 * row, 1.0});
    return rows;
}

std::optional<Ulid> HitTestTimelineMarker(const Document& document,
                                          const TimelineViewport& viewport,
                                          double x, double y, double width) {
    std::optional<Ulid> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const TimelineMarkerFlag& flag :
         VisibleTimelineMarkers(document, viewport, width)) {
        const bool span = flag.end_x > flag.x;
        const double right =
            std::max(flag.flag_x + kTimelineMarkerFlagWidth, flag.end_x);
        if (x < flag.flag_x - kTimelineMarkerHitSlop ||
            x >= right + kTimelineMarkerHitSlop ||
            y < flag.flag_y - kTimelineMarkerHitSlop ||
            y >= flag.flag_y + kTimelineMarkerFlagHeight +
                     kTimelineMarkerHitSlop)
            continue;
        // A span started left of the view is not hit under the headers.
        if (span && x < viewport.header_width) continue;
        const double distance =
            span ? std::min(std::abs(x - flag.x), std::abs(x - flag.end_x))
                 : std::abs(x - flag.x);
        // "<=": at equal distance the later flag wins, the one drawn on top.
        if (distance <= bestDistance) {
            best = flag.marker_id;
            bestDistance = distance;
        }
    }
    return best;
}

const DocumentMarker* TimelineMarkerAtPlayhead(const Document& document,
                                               const RationalTime& playhead,
                                               MediaRate frameRate) {
    const RationalTime shown = QuantizePlayheadPosition(
        playhead, PlayheadResolution::Frame, frameRate);
    for (const DocumentMarker& marker : document.sequence.markers) {
        if (QuantizePlayheadPosition(marker.time, PlayheadResolution::Frame,
                                     frameRate) == shown)
            return &marker;
    }
    return nullptr;
}

const DocumentMarker* TimelineMarkerCoveringPlayhead(
    const Document& document, const RationalTime& playhead,
    MediaRate frameRate) {
    if (const DocumentMarker* at =
            TimelineMarkerAtPlayhead(document, playhead, frameRate))
        return at;
    const RationalTime shown = QuantizePlayheadPosition(
        playhead, PlayheadResolution::Frame, frameRate);
    for (const DocumentMarker& marker : document.sequence.markers) {
        if (marker.duration.value <= 0) continue;
        if (QuantizePlayheadPosition(marker.time, PlayheadResolution::Frame,
                                     frameRate) <= shown &&
            shown <= QuantizePlayheadPosition(marker.time.add(marker.duration),
                                              PlayheadResolution::Frame,
                                              frameRate))
            return &marker;
    }
    return nullptr;
}

UpdateMarkerOperation TimelineMarkerSpanUpdate(const DocumentMarker& marker,
                                               const RationalTime& anchor,
                                               const RationalTime& pointer) {
    const bool forward = anchor <= pointer;
    RationalTime duration = forward ? pointer.sub(anchor) : anchor.sub(pointer);
    if (duration.value == 0) duration = {0, 1};
    return UpdateMarkerOperation{
        marker.id,    marker.name,     forward ? anchor : pointer,
        marker.color, marker.category, duration};
}

RationalTime TimelineMarkerSpanAnchor(const DocumentMarker& marker,
                                      const TimelineViewport& viewport,
                                      double x) {
    if (marker.duration.value <= 0) return marker.time;
    const RationalTime end = marker.time.add(marker.duration);
    return std::abs(x - viewport.TimeToX(marker.time)) <=
                   std::abs(x - viewport.TimeToX(end))
               ? end
               : marker.time;
}

std::optional<RationalTime> AdjacentTimelineMarker(const Document& document,
                                                   const RationalTime& playhead,
                                                   MediaRate frameRate,
                                                   bool forward) {
    const RationalTime shown = QuantizePlayheadPosition(
        playhead, PlayheadResolution::Frame, frameRate);
    std::optional<RationalTime> best;
    std::optional<RationalTime> bestShown;
    for (const DocumentMarker& marker : document.sequence.markers) {
        const RationalTime at = QuantizePlayheadPosition(
            marker.time, PlayheadResolution::Frame, frameRate);
        if (forward ? at <= shown : at >= shown) continue;
        if (bestShown && (forward ? at >= *bestShown : at <= *bestShown))
            continue;
        best = marker.time;
        bestShown = at;
    }
    return best;
}

std::string TimelineMarkerNameFromNote(const std::string& typed) {
    constexpr size_t kMaximumBytes = 4096;
    std::string name = typed;
    for (char& byte : name) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value < 0x20 || value == 0x7f) byte = ' ';
    }
    // Never cut inside a UTF-8 sequence: back up to the byte that starts one.
    if (name.size() > kMaximumBytes) {
        size_t end = kMaximumBytes;
        while (end > 0 &&
               (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80)
            --end;
        name.resize(end);
    }
    const size_t first = name.find_first_not_of(' ');
    if (first == std::string::npos) return kTimelineMarkerDefaultName;
    const size_t last = name.find_last_not_of(' ');
    return name.substr(first, last - first + 1);
}

TimelineInteraction::TimelineInteraction(Document& document, EditLog& editLog,
                                         TimelineViewport& viewport)
    : document_(document), edit_log_(editLog), viewport_(viewport) {}

void TimelineInteraction::PointerDown(double x, double y, double width,
                                      int32_t timeRate) {
    requested_playhead_.reset();
    preview_.reset();
    trim_operation_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    drag_hit_.reset();
    drag_start_time_.reset();
    deselect_on_release_.clear();
    auto hit =
        ClipHitForTrimMode(document_, viewport_, x, y, width, trim_mode_);
    if (!hit) {
        selected_gap_ =
            HitTestTimelineGap(document_, viewport_, x, y, width, timeRate);
        if (x >= viewport_.header_width && y >= 0.0) {
            requested_playhead_ = viewport_.XToTime(x, timeRate);
        }
        selected_clip_id_.clear();
        primary_clip_id_.clear();
        selected_clip_ids_.clear();
        return;
    }
    selected_gap_.reset();
    if (selection_toggle_ && hit->edge == TimelineHitEdge::None) {
        const std::vector<Ulid> members =
            linked_selection_enabled_
                ? ExpandLinkedClipSelection(document_, {hit->clip_id})
                : std::vector<Ulid>{hit->clip_id};
        const bool selected =
            std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                      hit->clip_id) != selected_clip_ids_.end();
        selected_clip_id_.clear();
        if (selected) {
            selected_clip_ids_.erase(
                std::remove_if(
                    selected_clip_ids_.begin(), selected_clip_ids_.end(),
                    [&](const Ulid& id) {
                        return std::find(members.begin(), members.end(), id) !=
                               members.end();
                    }),
                selected_clip_ids_.end());
            primary_clip_id_ = selected_clip_ids_.empty()
                                   ? Ulid{}
                                   : selected_clip_ids_.front();
            return;
        }
        for (const Ulid& id : members)
            if (std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                          id) == selected_clip_ids_.end())
                selected_clip_ids_.push_back(id);
        primary_clip_id_ = hit->clip_id;
        const DocumentTrack* hitTrack = document_.FindTrack(hit->track_id);
        if (hitTrack && hitTrack->locked) return;
        const DocumentClip* clip = document_.FindClip(hit->clip_id);
        drag_rate_ = clip ? clip->duration.rate : timeRate;
        drag_x_ = x;
        drag_start_time_ = viewport_.XToTime(x, drag_rate_);
        drag_hit_ = hit;
        UpdatePreview(x, y, width);
        return;
    }
    // DEC40 -- Command extends the selection (SetSelectionExtend): a clip
    // not selected joins it, one already in it leaves only if the release
    // moves nothing, so that a Command-drag still takes the selection to
    // the playhead.
    if (selection_extend_ && hit->edge == TimelineHitEdge::None) {
        const std::vector<Ulid> members =
            linked_selection_enabled_
                ? ExpandLinkedClipSelection(document_, {hit->clip_id})
                : std::vector<Ulid>{hit->clip_id};
        const bool selected =
            std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                      hit->clip_id) != selected_clip_ids_.end();
        const DocumentTrack* hitTrack = document_.FindTrack(hit->track_id);
        const bool locked = hitTrack && hitTrack->locked;
        if (selected) {
            deselect_on_release_ = members;
        } else {
            for (const Ulid& id : members)
                if (std::find(selected_clip_ids_.begin(),
                              selected_clip_ids_.end(),
                              id) == selected_clip_ids_.end())
                    selected_clip_ids_.push_back(id);
        }
        primary_clip_id_ = hit->clip_id;
        selected_clip_id_ = selected_clip_ids_.size() == 1
                                ? selected_clip_ids_.front()
                                : Ulid{};
        if (locked) {
            // Nothing is dragged on a locked track: no release to wait for.
            DeselectPendingClips();
            return;
        }
        const DocumentClip* clip = document_.FindClip(hit->clip_id);
        drag_rate_ = clip ? clip->duration.rate : timeRate;
        drag_x_ = x;
        drag_start_time_ = viewport_.XToTime(x, drag_rate_);
        drag_hit_ = hit;
        UpdatePreview(x, y, width);
        return;
    }
    const bool preserveSelection =
        hit->edge == TimelineHitEdge::None && selected_clip_ids_.size() > 1 &&
        std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                  hit->clip_id) != selected_clip_ids_.end();
    selected_clip_id_ = preserveSelection ? Ulid{} : hit->clip_id;
    primary_clip_id_ = hit->clip_id;
    if (!preserveSelection) selected_clip_ids_ = {hit->clip_id};
    const DocumentTrack* selectedTrack = document_.FindTrack(hit->track_id);
    if (selectedTrack && selectedTrack->locked) return;
    if (linked_selection_enabled_ && !preserveSelection) {
        std::vector<Ulid> linked =
            ExpandLinkedClipSelection(document_, selected_clip_ids_);
        const bool lockedPartner =
            hit->edge == TimelineHitEdge::None &&
            std::any_of(linked.begin(), linked.end(), [&](const Ulid& id) {
                const DocumentTrack* track = document_.FindTrackForClip(id);
                return id != hit->clip_id && track && track->locked;
            });
        // A locked linked partner is a hard boundary, not a reason to make an
        // otherwise editable anchor immovable. For this body-move gesture,
        // temporarily detach the anchor from linked editing; the persistent
        // link group itself is unchanged.
        if (!lockedPartner) selected_clip_ids_ = std::move(linked);
    }
    const DocumentClip* clip = document_.FindClip(hit->clip_id);
    drag_rate_ = clip ? clip->duration.rate : timeRate;
    drag_x_ = x;
    drag_start_time_ = viewport_.XToTime(x, drag_rate_);
    drag_hit_ = hit;
    UpdatePreview(x, y, width);
}

void TimelineInteraction::PointerDrag(double x, double y, double width) {
    if (drag_hit_) UpdatePreview(x, y, width);
}

void TimelineInteraction::UpdatePreview(double x, double y, double width) {
    (void)width;
    const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
    if (!clip || !drag_start_time_) {
        preview_.reset();
        return;
    }
    RationalTime delta =
        viewport_.XToTime(x, drag_rate_).sub(*drag_start_time_);
    drag_x_ = x;
    snap_guide_time_.reset();
    if (drag_hit_->edge == TimelineHitEdge::None) {
        // FC3 -- the Trim tool's name bar: the clip slides between its
        // neighbours, which absorb the move, its linked sound with it when
        // the linked selection is on (the rule of slide_edit, Operations.h).
        if (trim_mode_ == TimelineTrimMode::Slide) {
            const std::vector<Ulid> clipIds =
                linked_selection_enabled_
                    ? SlideClipsWithLinkedPartners(document_, {clip->id})
                    : std::vector<Ulid>{clip->id};
            const MediaRate rate = document_.sequence.frame_rate;
            int64_t wanted = 0;
            TimelineTrimPreview preview;
            preview.clip_id = clip->id;
            preview.times = {clip->source_in, clip->duration,
                             clip->timeline_in};
            preview.valid = SequenceFramesOf(document_, delta, wanted);
            trim_operation_.reset();
            if (preview.valid) {
                const auto build = [&](int64_t frames) {
                    return Operation{SlideEditOperation{
                        clipIds,
                        RationalTime{frames * rate.den, rate.num},
                        {}}};
                };
                Document reached;
                const int64_t frames =
                    FarthestApplying(document_, wanted, build, reached);
                preview.linked_times = ChangedClipTimes(document_, reached);
                if (const DocumentClip* moved = reached.FindClip(clip->id))
                    preview.times = {moved->source_in, moved->duration,
                                     moved->timeline_in};
                if (frames != 0) trim_operation_ = build(frames);
            }
            preview_ = std::move(preview);
            move_preview_.reset();
            return;
        }
        if (trim_mode_ == TimelineTrimMode::Slip) {
            std::vector<Ulid> clipIds{clip->id};
            if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
                for (const Ulid& id : selected_clip_ids_) {
                    const DocumentClip* member = document_.FindClip(id);
                    if (id != clip->id && member &&
                        member->link_group_id == clip->link_group_id)
                        clipIds.push_back(id);
                }
            }
            RationalTime constrained = delta;
            for (const Ulid& id : clipIds) {
                const DocumentClip* member = document_.FindClip(id);
                const std::optional<DocumentSource> source =
                    member ? ClipSource(document_, *member) : std::nullopt;
                if (!member || !source) continue;
                const RationalTime minimum =
                    RationalTime{0, member->source_in.rate}.sub(
                        member->source_in);
                const RationalTime maximum = source->duration.sub(
                    member->source_in.add(member->duration));
                if (constrained < minimum) constrained = minimum;
                if (constrained > maximum) constrained = maximum;
            }
            TimelineTrimPreview preview;
            preview.clip_id = clip->id;
            preview.link_group_id =
                clipIds.size() > 1 ? clip->link_group_id : Ulid{};
            Document candidate = document_;
            Operation operation = SlipEditOperation{clipIds, constrained, {}};
            Operation inverse = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            preview.valid =
                ApplyOperation(candidate, operation, inverse, error, message);
            for (const Ulid& id : clipIds) {
                const DocumentClip* changed = candidate.FindClip(id);
                if (!changed) continue;
                const ExactClipTimes times{changed->source_in,
                                           changed->duration,
                                           changed->timeline_in};
                if (id == clip->id) preview.times = times;
                preview.linked_times.push_back({id, times});
            }
            if (preview.valid && constrained.value != 0)
                trim_operation_ =
                    SlipEditOperation{std::move(clipIds), constrained, {}};
            else
                trim_operation_.reset();
            preview_ = std::move(preview);
            move_preview_.reset();
            return;
        }
        TimelineMovePreview preview;
        preview.clip_id = clip->id;
        RationalTime selectionStart = clip->timeline_in;
        if (selected_clip_ids_.size() > 1) {
            for (const Ulid& id : selected_clip_ids_) {
                const DocumentClip* member = document_.FindClip(id);
                if (member && member->timeline_in < selectionStart)
                    selectionStart = member->timeline_in;
            }
        }
        const RationalTime zero{0, selectionStart.rate};
        const RationalTime wallDelta = zero.sub(selectionStart);
        const RationalTime constrainedDelta =
            delta < wallDelta ? wallDelta : delta;
        preview.timeline_in = clip->timeline_in.add(constrainedDelta);
        const auto tracks = TimelineTracksInDisplayOrder(document_);
        if (y >= kTimelineRulerHeight) {
            const size_t index = static_cast<size_t>(std::floor(
                (y - kTimelineRulerHeight) / viewport_.track_height));
            if (index < tracks.size())
                preview.target_track_id = tracks[index]->id;
        }
        if (!preview.target_track_id.empty()) {
            const DocumentTrack* targetTrack =
                document_.FindTrack(preview.target_track_id);
            RationalTime unsnapped = preview.timeline_in;
            std::optional<RationalTime> snappedBoundary;
            if ((snapping_enabled_ || playhead_snap_target_) && targetTrack) {
                const RationalTime selectionCandidate =
                    selectionStart.add(constrainedDelta);
                const double wallDistance =
                    std::abs(viewport_.TimeToX(selectionCandidate) -
                             viewport_.TimeToX(zero));
                if (selected_clip_ids_.size() > 1 &&
                    wallDistance <= kTimelineSnapDistance) {
                    preview.timeline_in = clip->timeline_in.add(wallDelta);
                    snappedBoundary = zero;
                } else {
                    const auto startSnap =
                        FindSnapTime(unsnapped, *targetTrack, clip->id, true);
                    const RationalTime end = unsnapped.add(clip->duration);
                    const auto endSnap =
                        FindSnapTime(end, *targetTrack, clip->id);
                    if (startSnap && endSnap) {
                        const double startDistance =
                            std::abs(viewport_.TimeToX(*startSnap) -
                                     viewport_.TimeToX(unsnapped));
                        const double endDistance =
                            std::abs(viewport_.TimeToX(*endSnap) -
                                     viewport_.TimeToX(end));
                        if (startDistance <= endDistance) {
                            preview.timeline_in = *startSnap;
                            snappedBoundary = startSnap;
                        } else {
                            preview.timeline_in = endSnap->sub(clip->duration);
                            snappedBoundary = endSnap;
                        }
                    } else if (startSnap) {
                        preview.timeline_in = *startSnap;
                        snappedBoundary = startSnap;
                    } else if (endSnap) {
                        preview.timeline_in = endSnap->sub(clip->duration);
                        snappedBoundary = endSnap;
                    }
                }
            }
            const auto buildOperation = [&](RationalTime anchorTimeline) {
                preview.linked_moves.clear();
                preview.link_group_id.clear();
                const bool multiSelection = selected_clip_ids_.size() > 1;
                const bool linked =
                    linked_selection_enabled_ && multiSelection &&
                    !clip->link_group_id.empty() &&
                    std::all_of(selected_clip_ids_.begin(),
                                selected_clip_ids_.end(), [&](const Ulid& id) {
                                    const DocumentClip* member =
                                        document_.FindClip(id);
                                    return member && member->link_group_id ==
                                                         clip->link_group_id;
                                });
                if (multiSelection) {
                    const RationalTime groupDelta =
                        anchorTimeline.sub(clip->timeline_in);
                    const DocumentTrack* anchorTrack =
                        document_.FindTrackForClip(clip->id);
                    const DocumentTrack* anchorTarget =
                        document_.FindTrack(preview.target_track_id);
                    std::optional<int64_t> trackOffset;
                    if (anchorTrack && anchorTarget &&
                        anchorTrack->kind == anchorTarget->kind) {
                        const auto sourceOrdinal =
                            TrackOrdinal(document_, *anchorTrack);
                        const auto targetOrdinal =
                            TrackOrdinal(document_, *anchorTarget);
                        if (sourceOrdinal && targetOrdinal)
                            trackOffset = *targetOrdinal - *sourceOrdinal;
                    }
                    for (const Ulid& id : selected_clip_ids_) {
                        const DocumentClip* member = document_.FindClip(id);
                        const DocumentTrack* memberTrack =
                            document_.FindTrackForClip(id);
                        if (!member || !memberTrack) continue;
                        Ulid destinationTrack;
                        if (trackOffset) {
                            const auto memberOrdinal =
                                TrackOrdinal(document_, *memberTrack);
                            const auto compatibleTracks =
                                TracksOfKindInEditOrder(document_,
                                                        memberTrack->kind);
                            if (memberOrdinal) {
                                const int64_t destinationOrdinal =
                                    *memberOrdinal + *trackOffset;
                                if (destinationOrdinal >= 0 &&
                                    static_cast<size_t>(destinationOrdinal) <
                                        compatibleTracks.size())
                                    destinationTrack =
                                        compatibleTracks
                                            [static_cast<size_t>(
                                                 destinationOrdinal)]
                                                ->id;
                            }
                        }
                        // An empty destination deliberately makes the atomic
                        // candidate invalid when one linked kind has no
                        // corresponding Vn/An track. Never leave the partner
                        // silently behind on its original track.
                        preview.linked_moves.push_back(
                            {id, destinationTrack,
                             member->timeline_in.add(groupDelta)});
                    }
                    if (preview.linked_moves.size() > 1) {
                        if (linked) {
                            preview.link_group_id = clip->link_group_id;
                            return Operation{MoveLinkedClipsOperation{
                                clip->link_group_id, preview.linked_moves, {}}};
                        }
                        return Operation{
                            MoveClipsOperation{preview.linked_moves, {}}};
                    }
                }
                return Operation{MoveClipOperation{
                    clip->id, preview.target_track_id, anchorTimeline, {}}};
            };
            Document candidate = document_;
            Operation operation = buildOperation(preview.timeline_in);
            Operation inverse = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            preview.valid =
                ApplyOperation(candidate, operation, inverse, error, message);
            if (!preview.valid && snappedBoundary) {
                // Never let magnetism turn an otherwise legal free drop into
                // an invalid one.
                candidate = document_;
                operation = buildOperation(unsnapped);
                preview.valid = ApplyOperation(candidate, operation, inverse,
                                               error, message);
                preview.timeline_in = unsnapped;
                snappedBoundary.reset();
            }
            if (preview.valid && snappedBoundary)
                snap_guide_time_ = snappedBoundary;
        }
        move_preview_ = preview;
        preview_.reset();
        return;
    }
    const TrimEdge edge = drag_hit_->edge == TimelineHitEdge::Head
                              ? TrimEdge::Head
                              : TrimEdge::Tail;
    // FC3 -- an end of the Retime controls: the clip takes the length the
    // pointer gives it, in whole sequence frames, and keeps its source
    // range, so its speed changes (RateStretchOperation, which takes its
    // link group along). Where the speed would not be exact, or the clip
    // would run into the next, it stops at the last length that applies.
    //
    // DEC40 -- with the Trim tool the stretch ripples: the clip keeps its
    // head, what follows on its tracks moves by the change of length, and
    // the sync-locked tracks follow, as the Trim tool's ripple of an edge
    // asks them (every sync-locked track; the engine skips the member's
    // own and refuses a locked one).
    if (TimelineTrimModeStretches(trim_mode_)) {
        const bool ripple = trim_mode_ == TimelineTrimMode::RippleRateStretch;
        std::vector<Ulid> syncTracks;
        if (ripple) {
            for (const DocumentTrack& candidateTrack :
                 document_.sequence.tracks)
                if (candidateTrack.sync_lock)
                    syncTracks.push_back(candidateTrack.id);
        }
        const MediaRate rate = document_.sequence.frame_rate;
        int64_t wanted = 0;
        TimelineTrimPreview preview;
        preview.clip_id = clip->id;
        preview.link_group_id = clip->link_group_id;
        preview.times = {clip->source_in, clip->duration, clip->timeline_in};
        preview.valid = SequenceFramesOf(document_, delta, wanted);
        trim_operation_.reset();
        if (preview.valid) {
            const auto build = [&](int64_t frames) {
                const RationalTime moved{frames * rate.den, rate.num};
                return Operation{RateStretchOperation{
                    clip->id,
                    edge,
                    edge == TrimEdge::Tail ? clip->duration.add(moved)
                                           : clip->duration.sub(moved),
                    {},
                    ripple,
                    syncTracks}};
            };
            Document reached;
            const int64_t frames =
                FarthestApplying(document_, wanted, build, reached);
            preview.linked_times = ChangedClipTimes(document_, reached);
            if (const DocumentClip* stretched = reached.FindClip(clip->id))
                preview.times = {stretched->source_in, stretched->duration,
                                 stretched->timeline_in};
            if (frames != 0) trim_operation_ = build(frames);
        }
        preview_ = std::move(preview);
        move_preview_.reset();
        return;
    }
    const DocumentTrack* track = document_.FindTrackForClip(clip->id);
    const RationalTime originalBoundary =
        edge == TrimEdge::Head ? clip->timeline_in
                               : clip->timeline_in.add(clip->duration);
    RationalTime candidateBoundary = originalBoundary.add(delta);
    std::optional<RationalTime> snappedBoundary;
    if ((snapping_enabled_ || playhead_snap_target_) && track) {
        // Visual QA of 19 September 2026, 5b.7 -- a roll moves both sides of
        // the cut. The partner's edge sits on the cut itself; as a target it
        // pulled any roll shorter than the snap distance back to nothing.
        Ulid partner;
        if (trim_mode_ == TimelineTrimMode::Roll) {
            for (const DocumentClip& candidateClip : track->clips) {
                const RationalTime partnerEdge =
                    edge == TrimEdge::Head
                        ? candidateClip.timeline_in.add(candidateClip.duration)
                        : candidateClip.timeline_in;
                if (candidateClip.id != clip->id &&
                    partnerEdge == originalBoundary)
                    partner = candidateClip.id;
            }
        }
        snappedBoundary =
            FindSnapTime(candidateBoundary, *track, clip->id, false, partner);
        if (snappedBoundary) delta = snappedBoundary->sub(originalBoundary);
    }
    // The clips the gesture trims besides this one: its linked partners
    // the selection holds, when the linked selection is on -- of its own
    // group only, which is all a linked trim or a ripple takes.
    std::vector<const DocumentClip*> partners;
    if (linked_selection_enabled_ && !clip->link_group_id.empty()) {
        for (const Ulid& id : selected_clip_ids_) {
            const DocumentClip* member = document_.FindClip(id);
            if (member && id != clip->id &&
                member->link_group_id == clip->link_group_id)
                partners.push_back(member);
        }
    }
    // A roll's pairs: the two clips of the cut, and with the linked
    // selection the same cut of their linked partners on other tracks.
    const auto rollPairs = [&]() {
        std::vector<RollEditPair> pairs;
        if (!track) return pairs;
        const RationalTime cut = originalBoundary;
        const DocumentClip* left = nullptr;
        const DocumentClip* right = nullptr;
        for (const DocumentClip& candidateClip : track->clips) {
            if (candidateClip.timeline_in.add(candidateClip.duration) == cut)
                left = &candidateClip;
            if (candidateClip.timeline_in == cut) right = &candidateClip;
        }
        if (!left || !right || left->id == right->id) return pairs;
        pairs.push_back({left->id, right->id});
        if (!linked_selection_enabled_ || left->link_group_id.empty() ||
            right->link_group_id.empty())
            return pairs;
        for (const DocumentTrack& candidateTrack : document_.sequence.tracks) {
            if (candidateTrack.id == track->id) continue;
            const DocumentClip* linkedLeft = nullptr;
            const DocumentClip* linkedRight = nullptr;
            for (const DocumentClip& candidateClip : candidateTrack.clips) {
                if (candidateClip.link_group_id == left->link_group_id &&
                    candidateClip.timeline_in.add(candidateClip.duration) ==
                        cut)
                    linkedLeft = &candidateClip;
                if (candidateClip.link_group_id == right->link_group_id &&
                    candidateClip.timeline_in == cut)
                    linkedRight = &candidateClip;
            }
            if (linkedLeft && linkedRight)
                pairs.push_back({linkedLeft->id, linkedRight->id});
        }
        return pairs;
    };
    // The author, 31 October 2026: the trim "ne refuse pas proprement d'étirer
    // un plan au delà de la limite". A trim that leaves the track in place
    // always stopped at the rush, the neighbour and one frame; a ripple and
    // a roll had no bound, so past the end of the rush the operation was
    // refused, the preview went red and the release did nothing. Each now
    // stops where the first of its clips runs out, linked partners
    // included, as Resolve's edge stops at the last frame of the media.
    if (trim_mode_ == TimelineTrimMode::Normal ||
        trim_mode_ == TimelineTrimMode::Ripple) {
        const bool holdsNeighbours = trim_mode_ == TimelineTrimMode::Normal;
        delta = ConstrainTrimDelta(*clip, edge, delta, holdsNeighbours);
        for (const DocumentClip* member : partners)
            delta = ConstrainTrimDelta(*member, edge, delta, holdsNeighbours);
    } else if (trim_mode_ == TimelineTrimMode::Roll) {
        for (const RollEditPair& pair : rollPairs()) {
            if (const DocumentClip* left =
                    document_.FindClip(pair.left_clip_id))
                delta = ConstrainTrimDelta(*left, TrimEdge::Tail, delta, false);
            if (const DocumentClip* right =
                    document_.FindClip(pair.right_clip_id))
                delta =
                    ConstrainTrimDelta(*right, TrimEdge::Head, delta, false);
        }
    }
    candidateBoundary = originalBoundary.add(delta);
    if (snappedBoundary && candidateBoundary == *snappedBoundary)
        snap_guide_time_ = snappedBoundary;
    TimelineTrimPreview preview;
    preview.clip_id = clip->id;
    try {
        // Case 96 of VISUAL_QA_CHECKLIST.md -- the gesture's operation for
        // `delta`, so that a refused one can be looked for again nearer.
        const auto buildTrim = [&](const RationalTime& delta) {
            Operation operation =
                TrimClipOperation{clip->id, edge, delta, std::nullopt};
            if (trim_mode_ == TimelineTrimMode::Ripple) {
                // Its own group only: another selected clip made the
                // operation refuse the whole ripple ("ripple trim linked
                // clips must share one group").
                std::vector<Ulid> linkedIds;
                for (const DocumentClip* member : partners)
                    linkedIds.push_back(member->id);
                std::vector<Ulid> syncTracks;
                for (const DocumentTrack& candidateTrack :
                     document_.sequence.tracks)
                    if (candidateTrack.sync_lock)
                        syncTracks.push_back(candidateTrack.id);
                operation = RippleTrimOperation{clip->id,
                                                edge,
                                                delta,
                                                std::move(linkedIds),
                                                std::move(syncTracks),
                                                {}};
            } else if (trim_mode_ == TimelineTrimMode::Roll) {
                std::vector<RollEditPair> pairs = rollPairs();
                operation = RollEditOperation{std::move(pairs), delta, {}};
            } else if (linked_selection_enabled_ &&
                       !clip->link_group_id.empty()) {
                std::vector<LinkedClipTrim> trims;
                for (const Ulid& id : selected_clip_ids_) {
                    const DocumentClip* member = document_.FindClip(id);
                    if (member && member->link_group_id == clip->link_group_id)
                        trims.push_back({id, edge, delta});
                }
                if (trims.size() > 1) {
                    operation = TrimLinkedClipsOperation{
                        clip->link_group_id, std::move(trims), {}};
                }
            }
            return operation;
        };
        preview.times = ProposedTimes(*clip, edge, delta);
        Document candidate = document_;
        Operation operation = buildTrim(delta);
        Operation pendingOperation = operation;
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        preview.valid =
            ApplyOperation(candidate, operation, inverse, error, message);
        // Cases 96 and 107b of VISUAL_QA_CHECKLIST.md -- a trim the
        // operation refuses is not proposed: the drag stops at the nearest
        // number of sequence frames toward zero that applies, as the slide
        // and the stretch do (FarthestApplying). A clip's limits (a
        // neighbour, the end of the rush) grow with the distance and are
        // found by bisection; a slowed clip's head also has to land on a
        // frame of its rush (at 2/5, every fifth sequence frame), which
        // bisection skips, so the frames just short of the pointer are
        // tried one by one first. Only for a retimed clip: elsewhere a
        // refused linked trim stays refused whole, as it always was.
        if (!preview.valid && ClipHasSpeed(*clip)) {
            const MediaRate rate = document_.sequence.frame_rate;
            int64_t wanted = 0;
            if (SequenceFramesOf(document_, delta, wanted) && wanted != 0) {
                const auto build = [&](int64_t frames) {
                    return buildTrim(RationalTime{frames * rate.den, rate.num});
                };
                const auto applies = [&](int64_t frames) {
                    Document attempt = document_;
                    Operation attempted = build(frames);
                    Operation ignored = RemoveClipOperation{};
                    EditError attemptError = EditError::None;
                    std::string attemptMessage;
                    return ApplyOperation(attempt, attempted, ignored,
                                          attemptError, attemptMessage);
                };
                int64_t reached = 0;
                const int64_t step = wanted > 0 ? -1 : 1;
                int64_t frames = wanted;
                for (int scanned = 0;
                     scanned < kTimelineTrimAlignmentScan && frames != 0;
                     ++scanned) {
                    if (applies(frames)) {
                        reached = frames;
                        break;
                    }
                    frames += step;
                }
                if (reached == 0) {
                    Document farthest;
                    reached =
                        FarthestApplying(document_, wanted, build, farthest);
                }
                if (reached != 0) {
                    delta = RationalTime{reached * rate.den, rate.num};
                    preview.times = ProposedTimes(*clip, edge, delta);
                    candidate = document_;
                    operation = build(reached);
                    pendingOperation = operation;
                    preview.valid = ApplyOperation(candidate, operation,
                                                   inverse, error, message);
                }
            }
        }
        if (std::holds_alternative<TrimLinkedClipsOperation>(pendingOperation))
            preview.link_group_id = clip->link_group_id;
        if (preview.valid) {
            const DocumentClip* changed = candidate.FindClip(clip->id);
            if (changed)
                preview.times = {changed->source_in, changed->duration,
                                 changed->timeline_in};
            for (const DocumentTrack& candidateTrack :
                 candidate.sequence.tracks) {
                for (const DocumentClip& changedClip : candidateTrack.clips) {
                    if (changedClip.id == clip->id) continue;
                    const DocumentClip* original =
                        document_.FindClip(changedClip.id);
                    if (original &&
                        (original->source_in != changedClip.source_in ||
                         original->duration != changedClip.duration ||
                         original->timeline_in != changedClip.timeline_in))
                        preview.linked_times.push_back(
                            {changedClip.id,
                             {changedClip.source_in, changedClip.duration,
                              changedClip.timeline_in}});
                }
            }
            if (delta.value != 0)
                trim_operation_ = pendingOperation;
            else
                trim_operation_.reset();
        } else {
            trim_operation_.reset();
        }
        if (!preview.link_group_id.empty()) {
            for (const Ulid& id : selected_clip_ids_) {
                if (std::any_of(
                        preview.linked_times.begin(),
                        preview.linked_times.end(),
                        [&](const auto& linked) { return linked.first == id; }))
                    continue;
                const DocumentClip* member = preview.valid
                                                 ? candidate.FindClip(id)
                                                 : document_.FindClip(id);
                if (member && member->link_group_id == preview.link_group_id)
                    preview.linked_times.push_back(
                        {id, preview.valid
                                 ? ExactClipTimes{member->source_in,
                                                  member->duration,
                                                  member->timeline_in}
                                 : ProposedTimes(*member, edge, delta)});
            }
        }
    } catch (const std::exception&) {
        preview.times = {clip->source_in, clip->duration, clip->timeline_in};
        preview.valid = false;
        trim_operation_.reset();
    }
    preview_ = preview;
    move_preview_.reset();
}

RationalTime TimelineInteraction::ConstrainTrimDelta(
    const DocumentClip& clip, TrimEdge edge, RationalTime delta,
    bool holdsNeighbours) const {
    // Visual QA of 19 September 2026, 5q.6 -- a motion clip names no media,
    // and FindSource left its trim unbounded: dragged past the first frame
    // of its composition, the preview went invalid and the release did
    // nothing, instead of stopping on that frame as a media clip does.
    const std::optional<DocumentSource> source = ClipSource(document_, clip);
    const DocumentTrack* track = document_.FindTrackForClip(clip.id);
    if (!track || (!source && track->kind != "caption")) return delta;
    const bool captionClip = track->kind == "caption";
    const RationalTime zero{0, 1};
    const RationalTime minimumDuration{1, drag_rate_};
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& candidate) { return candidate.id == clip.id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));

    // Case 96 of VISUAL_QA_CHECKLIST.md -- the rush's limits were measured
    // at 1/1: a clip at 2/1 was offered twice the media it has, which the
    // operation then refused. At a constant speed they are the source left
    // on each side over the speed (UnscaleBySpeed); a ramp and a freeze
    // keep the bound they had, the operation saying the rest.
    const bool constantSpeed = !captionClip && !ClipHasSpeedRamp(clip) &&
                               !ClipIsFreeze(clip) && clip.speed.num > 0 &&
                               clip.speed.den > 0;
    const auto timelineSpan = [&](const RationalTime& sourceSpan) {
        return constantSpeed ? UnscaleBySpeed(sourceSpan, clip.speed)
                             : sourceSpan;
    };
    const RationalTime sourceEnd =
        constantSpeed ? ClipSourceEnd(clip) : clip.source_in.add(clip.duration);
    std::optional<RationalTime> lower;
    std::optional<RationalTime> upper;
    const auto raiseLower = [&](const RationalTime& bound) {
        lower = lower ? MaxTime(*lower, bound) : bound;
    };
    const auto lowerUpper = [&](const RationalTime& bound) {
        upper = upper ? MinTime(*upper, bound) : bound;
    };
    if (edge == TrimEdge::Head) {
        if (!captionClip) raiseLower(timelineSpan(zero.sub(clip.source_in)));
        if (holdsNeighbours) {
            raiseLower(zero.sub(clip.timeline_in));
            if (index > 0) {
                const DocumentClip& previous = track->clips[index - 1];
                raiseLower(previous.timeline_in.add(previous.duration)
                               .sub(clip.timeline_in));
            }
        }
        lowerUpper(clip.duration.sub(minimumDuration));
    } else {
        raiseLower(minimumDuration.sub(clip.duration));
        if (holdsNeighbours && index + 1 < track->clips.size()) {
            const DocumentClip& next = track->clips[index + 1];
            lowerUpper(
                next.timeline_in.sub(clip.timeline_in.add(clip.duration)));
        }
        if (!captionClip)
            lowerUpper(timelineSpan(source->duration.sub(sourceEnd)));
    }
    if (lower && delta < *lower) return *lower;
    if (upper && delta > *upper) return *upper;
    return delta;
}

std::optional<RationalTime> TimelineInteraction::FindSnapTime(
    RationalTime candidate, const DocumentTrack& track,
    const Ulid& excludedClipId, bool includeSyncTarget,
    const Ulid& partnerClipId) const {
    std::optional<RationalTime> best;
    double bestDistance = kTimelineSnapDistance + 1.0;
    const auto consider = [&](RationalTime target) {
        const double distance =
            std::abs(viewport_.TimeToX(target) - viewport_.TimeToX(candidate));
        if (distance <= kTimelineSnapDistance && distance < bestDistance) {
            best = target;
            bestDistance = distance;
        }
    };
    consider({0, candidate.rate});
    if (playhead_snap_target_)
        consider(*playhead_snap_target_);
    else if (snapping_enabled_ && playhead_)
        consider(*playhead_);
    if (includeSyncTarget && !linked_selection_enabled_) {
        const DocumentClip* clip = document_.FindClip(excludedClipId);
        const DocumentClip* anchor =
            clip ? document_.FindClip(clip->sync_anchor_clip_id) : nullptr;
        if (clip && anchor && anchor->id != clip->id &&
            anchor->link_group_id == clip->link_group_id) {
            try {
                const RationalTime anchorPhase =
                    anchor->timeline_in.sub(anchor->source_in);
                consider(clip->source_in.add(anchorPhase)
                             .add(clip->sync_reference_delta));
            } catch (const std::exception&) {
            }
        }
    }
    const DocumentClip* partner =
        partnerClipId.empty() ? nullptr : document_.FindClip(partnerClipId);
    const auto carried = [&](const DocumentClip& other) {
        if (other.id == excludedClipId) return true;
        if (!partnerClipId.empty() && other.id == partnerClipId) return true;
        if (std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                      other.id) != selected_clip_ids_.end())
            return true;
        // A linked roll moves the partner's linked clips along with it.
        return partner && linked_selection_enabled_ &&
               !partner->link_group_id.empty() &&
               other.link_group_id == partner->link_group_id;
    };
    const auto considerEdges = [&](const DocumentTrack& candidateTrack) {
        for (const DocumentClip& other : candidateTrack.clips) {
            if (carried(other)) continue;
            consider(other.timeline_in);
            consider(other.timeline_in.add(other.duration));
        }
    };
    considerEdges(track);
    for (const DocumentTrack& other : document_.sequence.tracks)
        if (other.id != track.id) considerEdges(other);
    for (const DocumentMarker& marker : document_.sequence.markers) {
        consider(marker.time);
        if (marker.duration.value > 0)
            consider(marker.time.add(marker.duration));
    }
    if (in_snap_target_) consider(*in_snap_target_);
    if (out_snap_target_) consider(*out_snap_target_);
    return best;
}

std::optional<Operation> TimelineInteraction::PendingOperation() const {
    if (!drag_hit_ || !drag_start_time_) return std::nullopt;
    if (trim_operation_) return trim_operation_;
    if (drag_hit_->edge == TimelineHitEdge::None) {
        if (!move_preview_ || move_preview_->target_track_id.empty())
            return std::nullopt;
        const DocumentTrack* originalTrack =
            document_.FindTrackForClip(drag_hit_->clip_id);
        const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
        if (!originalTrack || !clip) return std::nullopt;
        if (move_preview_->target_track_id == originalTrack->id &&
            move_preview_->timeline_in == clip->timeline_in)
            return std::nullopt;
        if (move_preview_->linked_moves.size() > 1) {
            if (!move_preview_->link_group_id.empty())
                return Operation{
                    MoveLinkedClipsOperation{move_preview_->link_group_id,
                                             move_preview_->linked_moves,
                                             {}}};
            return Operation{
                MoveClipsOperation{move_preview_->linked_moves, {}}};
        }
        return Operation{MoveClipOperation{drag_hit_->clip_id,
                                           move_preview_->target_track_id,
                                           move_preview_->timeline_in}};
    }
    const DocumentClip* clip = document_.FindClip(drag_hit_->clip_id);
    if (!clip || !preview_) return std::nullopt;
    const RationalTime delta =
        drag_hit_->edge == TimelineHitEdge::Head
            ? preview_->times.timeline_in.sub(clip->timeline_in)
            : preview_->times.duration.sub(clip->duration);
    if (delta.value == 0) return std::nullopt;
    if (!preview_->link_group_id.empty() && preview_->linked_times.size() > 1) {
        std::vector<LinkedClipTrim> trims;
        trims.reserve(preview_->linked_times.size());
        for (const auto& member : preview_->linked_times)
            trims.push_back({member.first,
                             drag_hit_->edge == TimelineHitEdge::Head
                                 ? TrimEdge::Head
                                 : TrimEdge::Tail,
                             delta});
        return Operation{TrimLinkedClipsOperation{
            preview_->link_group_id, std::move(trims), {}}};
    }
    return Operation{TrimClipOperation{drag_hit_->clip_id,
                                       drag_hit_->edge == TimelineHitEdge::Head
                                           ? TrimEdge::Head
                                           : TrimEdge::Tail,
                                       delta, std::nullopt}};
}

bool TimelineInteraction::PointerUp(EditError& error, std::string& message) {
    bool applied = false;
    const std::optional<Operation> operation = PendingOperation();
    const bool valid = (preview_ && preview_->valid) ||
                       (move_preview_ && move_preview_->valid);
    if (operation && valid)
        applied = edit_log_.Apply(document_, *operation, error, message);
    else {
        error = EditError::None;
        message.clear();
    }
    // DEC40 -- a Command-click that moved nothing takes its clip out.
    if (!operation) DeselectPendingClips();
    deselect_on_release_.clear();
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    trim_operation_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
    return applied;
}

void TimelineInteraction::DeselectPendingClips() {
    if (deselect_on_release_.empty()) return;
    selected_clip_ids_.erase(
        std::remove_if(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                       [&](const Ulid& id) {
                           return std::find(deselect_on_release_.begin(),
                                            deselect_on_release_.end(),
                                            id) != deselect_on_release_.end();
                       }),
        selected_clip_ids_.end());
    deselect_on_release_.clear();
    primary_clip_id_ =
        selected_clip_ids_.empty() ? Ulid{} : selected_clip_ids_.front();
    selected_clip_id_ =
        selected_clip_ids_.size() == 1 ? selected_clip_ids_.front() : Ulid{};
}

void TimelineInteraction::CancelDrag() {
    deselect_on_release_.clear();
    drag_hit_.reset();
    drag_start_time_.reset();
    preview_.reset();
    trim_operation_.reset();
    move_preview_.reset();
    snap_guide_time_.reset();
}

const Ulid& TimelineInteraction::SelectedClipId() const {
    return selected_clip_id_;
}

const Ulid& TimelineInteraction::PrimarySelectedClipId() const {
    return primary_clip_id_;
}

const std::vector<Ulid>& TimelineInteraction::SelectedClipIds() const {
    return selected_clip_ids_;
}

void TimelineInteraction::SelectClip(const Ulid& clipId) {
    selected_clip_id_ = document_.FindClip(clipId) ? clipId : Ulid{};
    primary_clip_id_ = selected_clip_id_;
    selected_clip_ids_.clear();
    if (!selected_clip_id_.empty())
        selected_clip_ids_.push_back(selected_clip_id_);
    selected_gap_.reset();
}

void TimelineInteraction::SelectClips(const std::vector<Ulid>& clipIds) {
    selected_clip_ids_.clear();
    for (const Ulid& id : clipIds) {
        if (document_.FindClip(id) &&
            std::find(selected_clip_ids_.begin(), selected_clip_ids_.end(),
                      id) == selected_clip_ids_.end())
            selected_clip_ids_.push_back(id);
    }
    selected_clip_id_ =
        selected_clip_ids_.size() == 1 ? selected_clip_ids_.front() : Ulid{};
    // The anchor is the caller's first surviving id. Callers that expand a
    // click through ExpandLinkedClipSelection pass the clicked clip first
    // and that function preserves input order, so this is the clip the user
    // actually pointed at rather than whichever linked clip sorts first.
    primary_clip_id_ =
        selected_clip_ids_.empty() ? Ulid{} : selected_clip_ids_.front();
    selected_gap_.reset();
}

const std::optional<TimelineGapSelection>& TimelineInteraction::SelectedGap()
    const {
    return selected_gap_;
}

void TimelineInteraction::ClearGapSelection() { selected_gap_.reset(); }

const std::optional<RationalTime>& TimelineInteraction::RequestedPlayhead()
    const {
    return requested_playhead_;
}

void TimelineInteraction::ClearRequestedPlayhead() {
    requested_playhead_.reset();
}

const std::optional<TimelineTrimPreview>& TimelineInteraction::TrimPreview()
    const {
    return preview_;
}

const std::optional<TimelineMovePreview>& TimelineInteraction::MovePreview()
    const {
    return move_preview_;
}

bool TimelineInteraction::HasActiveDrag() const {
    return drag_hit_.has_value();
}

void TimelineInteraction::SetSnappingEnabled(bool enabled) {
    snapping_enabled_ = enabled;
    if (!enabled) snap_guide_time_.reset();
}

bool TimelineInteraction::SnappingEnabled() const { return snapping_enabled_; }

void TimelineInteraction::SetPlayheadSnapTarget(
    std::optional<RationalTime> target) {
    playhead_snap_target_ = std::move(target);
}

void TimelineInteraction::SetPlayhead(std::optional<RationalTime> playhead) {
    playhead_ = std::move(playhead);
}

void TimelineInteraction::SetInOutSnapTargets(std::optional<RationalTime> in,
                                              std::optional<RationalTime> out) {
    in_snap_target_ = std::move(in);
    out_snap_target_ = std::move(out);
}

const std::optional<RationalTime>& TimelineInteraction::SnapGuideTime() const {
    return snap_guide_time_;
}

void TimelineInteraction::SetLinkedSelectionEnabled(bool enabled) {
    linked_selection_enabled_ = enabled;
}

void TimelineInteraction::SetTrimMode(TimelineTrimMode mode) {
    trim_mode_ = mode;
}

TimelineTrimMode TimelineInteraction::TrimMode() const { return trim_mode_; }

void TimelineInteraction::SetSelectionToggle(bool toggle) {
    selection_toggle_ = toggle;
}

void TimelineInteraction::SetSelectionExtend(bool extend) {
    selection_extend_ = extend;
}

TimelineTrimMode TimelineTrimModeFor(TimelinePointerTool tool,
                                     TimelinePointerModifiers modifiers) {
    if (modifiers.command) return TimelineTrimMode::Roll;
    if (tool == TimelinePointerTool::Trim || modifiers.shift)
        return TimelineTrimMode::Ripple;
    return TimelineTrimMode::Normal;
}

std::vector<TimelineTransitionRect> VisibleTimelineTransitions(
    const Document& document, const TimelineViewport& viewport, double width) {
    std::vector<TimelineTransitionRect> rects;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    const MediaRate rate = document.sequence.frame_rate;
    for (const DocumentTransition& transition : document.sequence.transitions) {
        const auto displayTrack = std::find_if(
            tracks.begin(), tracks.end(), [&](const DocumentTrack* track) {
                return track->id == transition.track_id;
            });
        if (displayTrack == tracks.end()) continue;
        TransitionSpan span;
        std::string error;
        if (!ResolveTransitionSpan(document.sequence, transition, span, error))
            continue;
        const RationalTime cut{span.cut_frame * rate.den, rate.num};
        const RationalTime pre{span.pre_frames * rate.den, rate.num};
        const RationalTime post{span.post_frames * rate.den, rate.num};
        const double left =
            std::max(viewport.TimeToX(cut.sub(pre)), viewport.header_width);
        const double right = std::min(viewport.TimeToX(cut.add(post)), width);
        if (right <= left) continue;
        TimelineTransitionRect rect;
        rect.transition_id = transition.id;
        rect.track_id = transition.track_id;
        rect.x = left;
        rect.width = right - left;
        rect.y = kTimelineRulerHeight +
                 std::distance(tracks.begin(), displayTrack) *
                     viewport.track_height +
                 4.0;
        rect.height = std::max(8.0, viewport.track_height - 8.0);
        rect.cut_x = viewport.TimeToX(cut);
        rects.push_back(rect);
    }
    return rects;
}

std::vector<TimelineEdgeMarkRect> VisibleTimelineEdgeMarks(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<TimelineEdgeMark>& marks) {
    std::vector<TimelineEdgeMarkRect> rects;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (const TimelineEdgeMark& mark : marks) {
        const DocumentTrack* track = document.FindTrackForClip(mark.clip_id);
        const DocumentClip* clip = document.FindClip(mark.clip_id);
        if (track == nullptr || clip == nullptr) continue;
        const auto row = std::find(tracks.begin(), tracks.end(), track);
        if (row == tracks.end()) continue;
        const double left = viewport.TimeToX(clip->timeline_in);
        const double right =
            viewport.TimeToX(clip->timeline_in.add(clip->duration));
        // The zone the press finds this edge in: within the edge distance,
        // and never past the clip's middle, where the other edge is nearer.
        const double reach = std::min(kTimelineEdgeHitWidth,
                                      std::max(0.0, (right - left) / 2.0));
        double zoneLeft = mark.edge == TrimEdge::Head ? left : right - reach;
        double zoneRight = mark.edge == TrimEdge::Head ? left + reach : right;
        zoneLeft = std::max(zoneLeft, viewport.header_width);
        zoneRight = std::min(zoneRight, width);
        if (zoneRight <= zoneLeft) continue;
        TimelineEdgeMarkRect rect;
        rect.clip_id = mark.clip_id;
        rect.edge = mark.edge;
        rect.at_limit = mark.at_limit;
        rect.x = zoneLeft;
        rect.width = zoneRight - zoneLeft;
        rect.y = kTimelineRulerHeight +
                 std::distance(tracks.begin(), row) * viewport.track_height;
        rect.height = viewport.track_height;
        rects.push_back(rect);
    }
    return rects;
}

std::vector<TimelineRetimeBand> VisibleTimelineRetimeBands(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& retimeClipIds) {
    std::vector<TimelineRetimeBand> bands;
    if (retimeClipIds.empty()) return bands;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t row = 0; row < tracks.size(); ++row) {
        for (const DocumentClip& clip : tracks[row]->clips) {
            if (std::find(retimeClipIds.begin(), retimeClipIds.end(),
                          clip.id) == retimeClipIds.end())
                continue;
            const double left = viewport.TimeToX(clip.timeline_in);
            const double right =
                viewport.TimeToX(clip.timeline_in.add(clip.duration));
            const double shownLeft = std::max(left, viewport.header_width);
            const double shownRight = std::min(right, width);
            if (shownRight <= shownLeft) continue;
            const double reach = std::min(kTimelineEdgeHitWidth,
                                          std::max(0.0, (right - left) / 2.0));
            TimelineRetimeBand band;
            band.clip_id = clip.id;
            band.x = shownLeft;
            band.width = shownRight - shownLeft;
            band.y = kTimelineRulerHeight + (row * viewport.track_height) +
                     ui::theme::kTimelineClipBodyTop;
            band.height = ui::theme::kTimelineRetimeBandHeight;
            const auto clipped = [&](double from, double to, double& x,
                                     double& zone) {
                x = std::max(from, viewport.header_width);
                zone = std::max(0.0, std::min(to, width) - x);
            };
            clipped(left, left + reach, band.head_x, band.head_width);
            clipped(right - reach, right, band.tail_x, band.tail_width);
            // Resolve writes 100% on a clip at its own speed.
            band.label = ClipHasSpeedRamp(clip) ? ClipSpeedRampBadgeLabel(clip)
                         : ClipIsFreeze(clip)   ? "0%"
                                              : ClipSpeedBadgeLabel(clip.speed);
            if (band.label.empty()) band.label = "100%";
            bands.push_back(std::move(band));
        }
    }
    return bands;
}

namespace {

double SecondsOf(const RationalTime& time) {
    return time.rate > 0 ? static_cast<double>(time.value) / time.rate : 0.0;
}

}  // namespace

std::vector<TimelineAudioFadeRect> VisibleTimelineAudioFades(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<TimelineClipRect>& clips,
    const std::vector<Ulid>& retimeClipIds,
    const std::optional<TimelineFadePreview>& preview) {
    std::vector<TimelineAudioFadeRect> fades;
    const double handle = ui::theme::kTimelineFadeHandleSize;
    const double step = ui::theme::kTimelineFadeStepWidth;
    for (const TimelineClipRect& rect : clips) {
        if (!rect.audio) continue;
        const DocumentClip* clip = document.FindClip(rect.clip_id);
        const DocumentTrack* track = document.FindTrackForClip(rect.clip_id);
        if (clip == nullptr || track == nullptr) continue;
        const bool previewed = preview && preview->clip_id == clip->id;
        const RationalTime fadeIn =
            previewed ? preview->fade_in : clip->audio_fade_in;
        const RationalTime fadeOut =
            previewed ? preview->fade_out : clip->audio_fade_out;
        const double left = std::min(rect.x, rect.x + rect.width);
        const double right = std::max(rect.x, rect.x + rect.width);
        const double span = right - left;
        const double inWidth = std::clamp(
            SecondsOf(fadeIn) * viewport.pixels_per_second, 0.0, span);
        const double outWidth = std::clamp(
            SecondsOf(fadeOut) * viewport.pixels_per_second, 0.0, span);
        TimelineAudioFadeRect fade;
        fade.clip_id = clip->id;
        fade.body_y = rect.y + ui::theme::kTimelineClipBodyTop;
        fade.body_height = ui::theme::kTimelineClipBodyHeight;
        fade.fade_in_end_x = left + inWidth;
        fade.fade_out_start_x = right - outWidth;

        // The attenuated stretches, one when the two ramps meet, clipped to
        // the view.
        std::vector<std::pair<double, double>> stretches;
        if (inWidth > 0.0) stretches.push_back({left, fade.fade_in_end_x});
        if (outWidth > 0.0) {
            if (!stretches.empty() &&
                fade.fade_out_start_x <= stretches.back().second)
                stretches.back().second = right;
            else
                stretches.push_back({fade.fade_out_start_x, right});
        }
        for (const auto& [from, to] : stretches) {
            const double shownFrom = std::max(from, viewport.header_width);
            const double shownTo = std::min(to, width);
            for (double x = shownFrom; x < shownTo; x += step) {
                const double columnWidth = std::min(step, shownTo - x);
                const double middle = x + (columnWidth / 2.0);
                double gain = 1.0;
                if (inWidth > 0.0)
                    gain *= std::min(1.0, (middle - left) / inWidth);
                if (outWidth > 0.0)
                    gain *= std::min(1.0, (right - middle) / outWidth);
                fade.steps.push_back(
                    {x, columnWidth, std::clamp(gain, 0.0, 1.0)});
            }
        }

        fade.handles = !track->locked && span >= 3.0 * handle &&
                       std::find(retimeClipIds.begin(), retimeClipIds.end(),
                                 clip->id) == retimeClipIds.end();
        if (fade.handles) {
            fade.handle_y = fade.body_y;
            fade.handle_height = handle;
            const auto clipped = [&](double from, double& x, double& zone) {
                x = std::max(from, viewport.header_width);
                zone = std::max(0.0, std::min(from + handle, width) - x);
            };
            clipped(std::min(fade.fade_in_end_x, right - handle),
                    fade.in_handle_x, fade.in_handle_width);
            clipped(std::max(fade.fade_out_start_x - handle, left),
                    fade.out_handle_x, fade.out_handle_width);
        }
        fades.push_back(std::move(fade));
    }
    return fades;
}

std::optional<TimelineFadeHandleHit> HitTestTimelineFadeHandle(
    const std::vector<TimelineAudioFadeRect>& fades, double x, double y) {
    for (const TimelineAudioFadeRect& fade : fades) {
        if (!fade.handles || y < fade.handle_y ||
            y >= fade.handle_y + fade.handle_height)
            continue;
        const bool in = fade.in_handle_width > 0.0 && x >= fade.in_handle_x &&
                        x <= fade.in_handle_x + fade.in_handle_width;
        const bool out = fade.out_handle_width > 0.0 &&
                         x >= fade.out_handle_x &&
                         x <= fade.out_handle_x + fade.out_handle_width;
        if (!in && !out) continue;
        const bool fadeIn =
            in && (!out || std::abs(x - fade.fade_in_end_x) <=
                               std::abs(x - fade.fade_out_start_x));
        return TimelineFadeHandleHit{fade.clip_id, fadeIn
                                                       ? TimelineHitEdge::Head
                                                       : TimelineHitEdge::Tail};
    }
    return std::nullopt;
}

std::optional<TimelineFadeDrag> BeginTimelineFadeDrag(const Document& document,
                                                      const Ulid& clipId,
                                                      TimelineHitEdge edge,
                                                      double x) {
    const DocumentClip* clip = document.FindClip(clipId);
    const DocumentTrack* track = document.FindTrackForClip(clipId);
    if (clip == nullptr || track == nullptr || track->kind != "audio" ||
        edge == TimelineHitEdge::None)
        return std::nullopt;
    TimelineFadeDrag drag;
    drag.clip_id = clipId;
    drag.edge = edge;
    drag.press_x = x;
    drag.start =
        SetClipAudioOperation{clip->id, clip->audio_gain_db,
                              clip->audio_fade_in, clip->audio_fade_out};
    return drag;
}

SetClipAudioOperation UpdateTimelineFadeDrag(const Document& document,
                                             const TimelineViewport& viewport,
                                             const TimelineFadeDrag& drag,
                                             double x) {
    SetClipAudioOperation reached = drag.start;
    const DocumentClip* clip = document.FindClip(drag.clip_id);
    const MediaRate rate = document.sequence.frame_rate;
    if (clip == nullptr || rate.num <= 0 || rate.den <= 0 ||
        drag.edge == TimelineHitEdge::None)
        return reached;
    const bool head = drag.edge == TimelineHitEdge::Head;
    const RationalTime travel =
        viewport.XToTime(x, kTimelineViewRate)
            .sub(viewport.XToTime(drag.press_x, kTimelineViewRate));
    const RationalTime from = head ? drag.start.fade_in : drag.start.fade_out;
    const RationalTime wanted = head ? from.add(travel) : from.sub(travel);
    const RationalTime other = head ? drag.start.fade_out : drag.start.fade_in;
    const RationalTime room = clip->duration.sub(other);
    // Whole frames of the sequence: the nearest for the pointer, the last
    // that fits for the room.
    const auto frames = [&](const RationalTime& time, bool nearest) {
        const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
        const __int128 denominator =
            static_cast<__int128>(time.rate) * rate.den;
        if (numerator <= 0 || denominator <= 0) return static_cast<int64_t>(0);
        const __int128 whole =
            nearest ? ((2 * numerator) + denominator) / (2 * denominator)
                    : numerator / denominator;
        return static_cast<int64_t>(
            std::min<__int128>(whole, std::numeric_limits<int32_t>::max()));
    };
    const int64_t count =
        std::clamp(frames(wanted, true), int64_t{0}, frames(room, false));
    const RationalTime fade = count == 0
                                  ? RationalTime{0, 1}
                                  : RationalTime{count * rate.den, rate.num};
    (head ? reached.fade_in : reached.fade_out) = fade;
    return reached;
}

TimelinePointerTarget TimelineHitAt(const Document& document,
                                    const TimelineViewport& viewport, double x,
                                    double y, double width, double height,
                                    RationalTime duration,
                                    TimelinePointerTool tool,
                                    TimelinePointerModifiers modifiers,
                                    const std::vector<Ulid>& retimeClipIds) {
    TimelinePointerTarget target;
    target.trim_mode = TimelineTrimModeFor(tool, modifiers);
    if (y < 0.0 || y > height) return target;
    // The order is the click's: the strip, then a marker, then the tool.
    if (y >= height - kTimelineZoomBarHeight) {
        target.kind = TimelinePointerTargetKind::ZoomBar;
        target.zoom_bar_part =
            CalculateTimelineZoomBarGeometry(viewport, duration, width)
                .HitTest(x);
        return target;
    }
    if (const std::optional<Ulid> marker =
            HitTestTimelineMarker(document, viewport, x, y, width)) {
        target.kind = TimelinePointerTargetKind::Marker;
        target.marker_id = *marker;
        target.marker_stretch = modifiers.option;
        return target;
    }
    // The transition drawn last is the one on top.
    for (const TimelineTransitionRect& rect :
         VisibleTimelineTransitions(document, viewport, width))
        if (x >= rect.x && x <= rect.x + rect.width && y >= rect.y &&
            y < rect.y + rect.height)
            target.transition_id = rect.transition_id;
    // GitHub #330 -- a fade handle, at the top of an audio clip's body. Only
    // that band of a row reads the clips' rectangles: the pointer crosses
    // the timeline on every move.
    if ((tool == TimelinePointerTool::Select ||
         tool == TimelinePointerTool::Trim) &&
        y >= kTimelineRulerHeight && viewport.track_height > 0.0) {
        const double rowTop =
            kTimelineRulerHeight +
            (std::floor((y - kTimelineRulerHeight) / viewport.track_height) *
             viewport.track_height);
        // TimelineClipRect::y is the row's top and 1.5 points (Visible-
        // TimelineClips); the handles hang from the body's top.
        const double handleTop =
            rowTop + kTimelineClipRowInset + ui::theme::kTimelineClipBodyTop;
        if (y >= handleTop &&
            y < handleTop + ui::theme::kTimelineFadeHandleSize) {
            const std::optional<TimelineFadeHandleHit> handle =
                HitTestTimelineFadeHandle(
                    VisibleTimelineAudioFades(
                        document, viewport, width,
                        VisibleTimelineClips(document, viewport, width, {},
                                             std::nullopt),
                        retimeClipIds),
                    x, y);
            const DocumentTrack* track =
                handle ? document.FindTrackForClip(handle->clip_id) : nullptr;
            if (track != nullptr) {
                target.kind = TimelinePointerTargetKind::FadeHandle;
                target.clip_id = handle->clip_id;
                target.track_id = track->id;
                target.edge = handle->edge;
                return target;
            }
        }
    }
    std::optional<TimelineHit> hit;
    if (tool == TimelinePointerTool::Cut) {
        hit = HitTestTimeline(document, viewport, x, y, width);
        if (hit) hit->edge = TimelineHitEdge::None;
    } else if (tool == TimelinePointerTool::Select ||
               tool == TimelinePointerTool::Trim) {
        hit = ClipHitForTrimMode(document, viewport, x, y, width,
                                 target.trim_mode);
    }
    if (!hit) {
        if (y >= kTimelineRulerHeight) {
            const auto tracks = TimelineTracksInDisplayOrder(document);
            const size_t index = static_cast<size_t>(
                std::floor((y - kTimelineRulerHeight) / viewport.track_height));
            if (index < tracks.size()) target.track_id = tracks[index]->id;
        }
        return target;
    }
    target.clip_id = hit->clip_id;
    target.track_id = hit->track_id;
    target.edge = hit->edge;
    target.kind = hit->edge == TimelineHitEdge::None
                      ? TimelinePointerTargetKind::ClipBody
                      : TimelinePointerTargetKind::ClipEdge;
    // ON9 -- the centre of a cut between two clips rolls it, with the
    // Selection tool as with the Trim tool, no modifier needed.
    if ((tool == TimelinePointerTool::Select ||
         tool == TimelinePointerTool::Trim) &&
        hit->edge != TimelineHitEdge::None) {
        const DocumentTrack* track = document.FindTrack(hit->track_id);
        const DocumentClip* clip = document.FindClip(hit->clip_id);
        if (track && clip) {
            const RationalTime cut = hit->edge == TimelineHitEdge::Tail
                                         ? clip->timeline_in.add(clip->duration)
                                         : clip->timeline_in;
            const bool joined = std::any_of(
                track->clips.begin(), track->clips.end(),
                [&](const DocumentClip& other) {
                    return other.id != clip->id &&
                           (hit->edge == TimelineHitEdge::Tail
                                ? other.timeline_in == cut
                                : other.timeline_in.add(other.duration) == cut);
                });
            if (joined && std::abs(x - viewport.TimeToX(cut)) <=
                              kTimelineRollHitHalfWidth)
                target.trim_mode = TimelineTrimMode::Roll;
        }
    }
    // FC4 -- the Trim tool's body slips on its upper part and, FC3, slides
    // on its name bar (Resolve 21.1 manual, p. 1063-1064).
    if (tool == TimelinePointerTool::Trim &&
        target.kind == TimelinePointerTargetKind::ClipBody) {
        const double rowTop =
            kTimelineRulerHeight +
            (std::floor((y - kTimelineRulerHeight) / viewport.track_height) *
             viewport.track_height);
        target.trim_mode = y - rowTop >= ui::theme::kTimelineClipNameBandTop
                               ? TimelineTrimMode::Slide
                               : TimelineTrimMode::Slip;
    }
    // FC3 -- an end of the clip's Retime controls stretches it.
    if ((tool == TimelinePointerTool::Select ||
         tool == TimelinePointerTool::Trim) &&
        std::find(retimeClipIds.begin(), retimeClipIds.end(), hit->clip_id) !=
            retimeClipIds.end()) {
        for (const TimelineRetimeBand& band : VisibleTimelineRetimeBands(
                 document, viewport, width, {hit->clip_id})) {
            if (y < band.y || y >= band.y + band.height) continue;
            const bool head =
                x >= band.head_x && x <= band.head_x + band.head_width;
            const bool tail =
                x >= band.tail_x && x <= band.tail_x + band.tail_width;
            if (!head && !tail) continue;
            // Nearer the end the pointer is on, as an edge is found.
            target.kind = TimelinePointerTargetKind::ClipEdge;
            target.edge =
                head && (!tail ||
                         x - band.head_x <= band.tail_x + band.tail_width - x)
                    ? TimelineHitEdge::Head
                    : TimelineHitEdge::Tail;
            // DEC40 -- the Trim tool's stretch ripples, as in Resolve.
            target.trim_mode = tool == TimelinePointerTool::Trim
                                   ? TimelineTrimMode::RippleRateStretch
                                   : TimelineTrimMode::RateStretch;
            target.duplicate = false;
            target.selection_toggle = false;
        }
    }
    const bool selectBody = tool == TimelinePointerTool::Select &&
                            hit->edge == TimelineHitEdge::None;
    target.duplicate = selectBody && modifiers.option;
    target.selection_toggle = selectBody && modifiers.shift;
    // DEC40 -- Command-click extends the selection; Shift keeps its toggle
    // and Option-Command its duplicate snapped to the playhead.
    target.selection_extend = selectBody && modifiers.command &&
                              !modifiers.shift && !modifiers.option;
    const DocumentTrack* track = document.FindTrack(hit->track_id);
    target.track_locked = track && track->locked;
    return target;
}

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview) {
    std::vector<TimelineClipRect> result;
    std::vector<TimelineClipRect> movingRects;
    const auto tracks = TimelineTracksInDisplayOrder(document);
    for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        for (const DocumentClip& clip : tracks[trackIndex]->clips) {
            size_t displayTrackIndex = trackIndex;
            RationalTime start = clip.timeline_in;
            RationalTime duration = clip.duration;
            RationalTime sourceIn = clip.source_in;
            bool isPreview = false;
            bool valid = true;
            bool isMoving = false;
            if (preview && (preview->clip_id == clip.id ||
                            std::any_of(preview->linked_times.begin(),
                                        preview->linked_times.end(),
                                        [&](const auto& member) {
                                            return member.first == clip.id;
                                        }))) {
                const auto member = std::find_if(
                    preview->linked_times.begin(), preview->linked_times.end(),
                    [&](const auto& value) { return value.first == clip.id; });
                const ExactClipTimes& times =
                    member == preview->linked_times.end() ? preview->times
                                                          : member->second;
                start = times.timeline_in;
                duration = times.duration;
                sourceIn = times.source_in;
                isPreview = true;
                valid = preview->valid;
            } else if (movePreview) {
                const auto linked =
                    std::find_if(movePreview->linked_moves.begin(),
                                 movePreview->linked_moves.end(),
                                 [&](const LinkedClipMove& move) {
                                     return move.clip_id == clip.id;
                                 });
                const bool anchor = movePreview->clip_id == clip.id;
                if (!anchor && linked == movePreview->linked_moves.end()) {
                    // No preview for this clip.
                } else {
                    start = linked != movePreview->linked_moves.end()
                                ? linked->timeline_in
                                : movePreview->timeline_in;
                    isPreview = true;
                    valid = movePreview->valid;
                    isMoving = true;
                    const Ulid targetTrack =
                        linked != movePreview->linked_moves.end()
                            ? linked->track_id
                            : movePreview->target_track_id;
                    const auto target =
                        std::find_if(tracks.begin(), tracks.end(),
                                     [&](const DocumentTrack* track) {
                                         return track->id == targetTrack;
                                     });
                    if (target != tracks.end())
                        displayTrackIndex = static_cast<size_t>(
                            std::distance(tracks.begin(), target));
                }
            }
            const double left = viewport.TimeToX(start);
            const double right = viewport.TimeToX(start.add(duration));
            if (std::max(left, right) < viewport.header_width ||
                std::min(left, right) > width)
                continue;
            TimelineClipRect rect{
                clip.id,
                clip.source_id,
                left,
                kTimelineRulerHeight +
                    displayTrackIndex * viewport.track_height +
                    kTimelineClipRowInset,
                right - left,
                viewport.track_height - (2.0 * kTimelineClipRowInset),
                std::find(selectedClipIds.begin(), selectedClipIds.end(),
                          clip.id) != selectedClipIds.end(),
                isPreview,
                valid,
                isMoving,
                tracks[displayTrackIndex]->kind == "audio",
                std::nullopt};
            rect.source_in = sourceIn;
            try {
                rect.source_span = ClipSourceAdvance(clip, duration);
            } catch (const std::overflow_error&) {
                rect.source_span = RationalTime{};
            }
            if (rect.audio) {
                rect.sync_drift = ClipSyncDrift(document, clip.id);
                if (rect.sync_drift && isMoving && movePreview &&
                    movePreview->linked_moves.size() <= 1)
                    rect.sync_drift =
                        rect.sync_drift->add(start.sub(clip.timeline_in));
            }
            if (isMoving)
                movingRects.push_back(std::move(rect));
            else
                result.push_back(std::move(rect));
        }
    }
    // Moving candidates are deliberately last, matching NLE stacking
    // conventions and keeping the opaque overwrite result above affected
    // clips.
    for (TimelineClipRect& rect : movingRects)
        result.push_back(std::move(rect));
    return result;
}

std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple) {
    std::vector<Ulid> allTracks;
    allTracks.reserve(document.sequence.tracks.size());
    for (const DocumentTrack& track : document.sequence.tracks)
        allTracks.push_back(track.id);
    return TimelineRangeDeleteOperation(document, rangeIn, rangeOut, ripple,
                                        allTracks);
}

std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple,
    const std::vector<Ulid>& targetedTrackIds, RangeDeleteFollowers followers) {
    if (rangeOut <= rangeIn) return std::nullopt;
    const std::set<Ulid> targeted(targetedTrackIds.begin(),
                                  targetedTrackIds.end());
    const bool hasEditableTarget = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [&](const DocumentTrack& track) {
            return !track.locked && targeted.count(track.id) != 0;
        });
    if (!hasEditableTarget) return std::nullopt;
    const RationalTime rangeDuration = rangeOut.sub(rangeIn);
    std::map<Ulid, Ulid> leftGroups;
    std::map<Ulid, Ulid> rightGroups;
    const auto sideGroup = [](std::map<Ulid, Ulid>& groups,
                              const Ulid& original) -> Ulid {
        if (original.empty()) return {};
        auto [item, inserted] = groups.emplace(original, Ulid{});
        if (inserted) item->second = GenerateUlid();
        return item->second;
    };
    std::vector<ExactTrackState> results;
    for (const DocumentTrack& track : document.sequence.tracks) {
        const bool directlyTargeted = targeted.count(track.id) != 0;
        if (track.locked || (!directlyTargeted && !(ripple && track.sync_lock)))
            continue;
        std::vector<DocumentClip> clips;
        bool changed = false;
        const bool continuous = ripple && !directlyTargeted &&
                                followers == RangeDeleteFollowers::Continuous;
        for (const DocumentClip& original : track.clips) {
            const RationalTime clipStart = original.timeline_in;
            const RationalTime clipEnd = clipStart.add(original.duration);
            if (clipEnd <= rangeIn) {
                clips.push_back(original);
                continue;
            }
            if (continuous && clipStart < rangeOut) {
                changed = true;
                // AX-2026-09 -- the rule a ripple trim's followers share.
                DocumentClip kept;
                if (CollapseClipThroughRange(original, rangeIn, rangeOut, kept))
                    clips.push_back(std::move(kept));
                continue;
            }
            if (clipStart >= rangeOut) {
                DocumentClip shifted = original;
                if (ripple) {
                    shifted.timeline_in = clipStart.sub(rangeDuration);
                    changed = true;
                }
                clips.push_back(std::move(shifted));
                continue;
            }
            changed = true;
            const bool hasLeft = clipStart < rangeIn;
            const bool hasRight = clipEnd > rangeOut;
            if (hasLeft) {
                DocumentClip left = original;
                left.duration = rangeIn.sub(clipStart);
                if (hasRight)
                    left.link_group_id =
                        sideGroup(leftGroups, original.link_group_id);
                clips.push_back(std::move(left));
            }
            if (hasRight) {
                DocumentClip right = original;
                if (hasLeft) RenewClipIdentity(right);
                right.source_in =
                    original.source_in.add(rangeOut.sub(clipStart));
                right.duration = clipEnd.sub(rangeOut);
                right.timeline_in = ripple ? rangeIn : rangeOut;
                if (hasLeft)
                    right.link_group_id =
                        sideGroup(rightGroups, original.link_group_id);
                clips.push_back(std::move(right));
            }
        }
        if (!changed) continue;
        std::sort(clips.begin(), clips.end(),
                  [](const DocumentClip& a, const DocumentClip& b) {
                      return a.timeline_in < b.timeline_in;
                  });
        results.push_back({track.id, std::move(clips)});
    }
    if (results.empty()) return std::nullopt;
    DeleteGapOperation operation{
        results.front().track_id, rangeIn, rangeDuration, results, {}};
    for (size_t index = 1; index < results.size(); ++index)
        operation.linked_track_ids.push_back(results[index].track_id);
    return operation;
}

std::optional<DeleteGapOperation> BuildTimelineRippleDeleteSelection(
    const Document& document, const std::vector<Ulid>& clipIds) {
    std::optional<RationalTime> rangeIn;
    std::optional<RationalTime> rangeOut;
    std::vector<Ulid> trackIds;
    for (const Ulid& id : clipIds) {
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (!clip || !track) continue;
        const RationalTime end = clip->timeline_in.add(clip->duration);
        if (!rangeIn || clip->timeline_in < *rangeIn)
            rangeIn = clip->timeline_in;
        if (!rangeOut || end > *rangeOut) rangeOut = end;
        if (std::find(trackIds.begin(), trackIds.end(), track->id) ==
            trackIds.end())
            trackIds.push_back(track->id);
    }
    if (!rangeIn || !rangeOut) return std::nullopt;
    return TimelineRangeDeleteOperation(document, *rangeIn, *rangeOut, true,
                                        trackIds);
}

bool ResolveSourceRecordEditRange(const RationalTime& sourceDuration,
                                  const SourceRecordEditPoints& points,
                                  SourceRecordEditRange& range,
                                  std::string& error) {
    const auto valid = [](const RationalTime& time) {
        return time.rate > 0 && time.value >= 0;
    };
    if (!valid(sourceDuration) || sourceDuration.value == 0 ||
        (!points.timeline_in && !points.timeline_out)) {
        error =
            "a positive source duration and a Record in or out are required";
        return false;
    }
    for (const auto& point : {points.source_in, points.source_out,
                              points.timeline_in, points.timeline_out}) {
        if (point && !valid(*point)) {
            error = "Source and Record points must be non-negative exact times";
            return false;
        }
    }
    try {
        SourceRecordEditRange resolved{
            points.source_in.value_or(RationalTime{0, sourceDuration.rate}),
            points.source_out.value_or(sourceDuration),
            points.timeline_in.value_or(RationalTime{0, 1})};
        if (points.timeline_in && points.timeline_out) {
            if (*points.timeline_out <= *points.timeline_in) {
                error = "Record out must be after Record in";
                return false;
            }
            const RationalTime duration =
                points.timeline_out->sub(*points.timeline_in);
            if (!points.source_out) {
                resolved.source_out = resolved.source_in.add(duration);
            } else if (!points.source_in) {
                resolved.source_in = resolved.source_out.sub(duration);
            } else if (resolved.source_out.sub(resolved.source_in) !=
                       duration) {
                error =
                    "four-point edit has different Source and Record durations";
                return false;
            }
        } else if (!points.timeline_in) {
            resolved.timeline_in = points.timeline_out->sub(
                resolved.source_out.sub(resolved.source_in));
        }
        if (!valid(resolved.source_in) || !valid(resolved.timeline_in) ||
            resolved.source_out <= resolved.source_in ||
            resolved.source_out > sourceDuration) {
            error =
                "resolved Source/Record range is outside the rush or timeline";
            return false;
        }
        range = resolved;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::optional<DeleteGapOperation> TimelineSourceEditOperation(
    const Document& document, const Ulid& sourceId,
    const RationalTime& sourceIn, const RationalTime& sourceOut,
    const RationalTime& timelineIn, const Ulid& targetTrackId, bool insert,
    const std::vector<Ulid>& audioTargetTrackIds) {
    const DocumentSource* source = document.FindSource(sourceId);
    const DocumentTrack* target = document.FindTrack(targetTrackId);
    const bool soundOnly =
        target && target->kind == "audio" && audioTargetTrackIds.empty();
    if (!source || !target || target->locked ||
        (target->kind != "video" && !soundOnly) || timelineIn.rate <= 0 ||
        timelineIn.value < 0 || sourceIn.rate <= 0 || sourceOut <= sourceIn ||
        sourceIn.value < 0 || sourceOut > source->duration)
        return std::nullopt;
    const std::set<Ulid> audioTargets(audioTargetTrackIds.begin(),
                                      audioTargetTrackIds.end());
    if (audioTargets.size() != audioTargetTrackIds.size() ||
        std::any_of(
            audioTargets.begin(), audioTargets.end(), [&](const Ulid& id) {
                const DocumentTrack* track = document.FindTrack(id);
                return !track || track->locked || track->kind != "audio";
            }))
        return std::nullopt;
    const RationalTime duration = sourceOut.sub(sourceIn);
    const RationalTime timelineOut = timelineIn.add(duration);
    const Ulid videoClipId = GenerateUlid();
    const Ulid linkGroupId = audioTargets.empty() ? Ulid{} : GenerateUlid();
    std::vector<ExactTrackState> results;

    for (const DocumentTrack& track : document.sequence.tracks) {
        const bool isTarget = track.id == targetTrackId;
        const bool isAudioTarget = audioTargets.count(track.id) != 0;
        const bool directlyTargeted = isTarget || isAudioTarget;
        if (track.locked || (!directlyTargeted && !(insert && track.sync_lock)))
            continue;
        std::vector<DocumentClip> clips;
        for (const DocumentClip& original : track.clips) {
            const RationalTime start = original.timeline_in;
            const RationalTime end = start.add(original.duration);
            if (insert) {
                if (end <= timelineIn) {
                    clips.push_back(original);
                } else if (start >= timelineIn) {
                    DocumentClip shifted = original;
                    shifted.timeline_in = start.add(duration);
                    clips.push_back(std::move(shifted));
                } else {
                    DocumentClip left = original;
                    left.duration = timelineIn.sub(start);
                    clips.push_back(std::move(left));
                    DocumentClip right = original;
                    RenewClipIdentity(right);
                    right.source_in =
                        original.source_in.add(timelineIn.sub(start));
                    right.duration = end.sub(timelineIn);
                    right.timeline_in = timelineIn.add(duration);
                    right.link_group_id.clear();
                    right.sync_anchor_clip_id.clear();
                    right.sync_reference_delta = {0, 1};
                    clips.push_back(std::move(right));
                }
                continue;
            }

            if (end <= timelineIn || start >= timelineOut) {
                clips.push_back(original);
                continue;
            }
            if (start < timelineIn) {
                DocumentClip left = original;
                left.duration = timelineIn.sub(start);
                clips.push_back(std::move(left));
            }
            if (end > timelineOut) {
                DocumentClip right = original;
                if (start < timelineIn) RenewClipIdentity(right);
                right.source_in =
                    original.source_in.add(timelineOut.sub(start));
                right.duration = end.sub(timelineOut);
                right.timeline_in = timelineOut;
                if (start < timelineIn) {
                    right.link_group_id.clear();
                    right.sync_anchor_clip_id.clear();
                    right.sync_reference_delta = {0, 1};
                }
                clips.push_back(std::move(right));
            }
        }
        if (directlyTargeted) {
            DocumentClip inserted{isTarget ? videoClipId : GenerateUlid(),
                                  sourceId, sourceIn, duration, timelineIn};
            if (isTarget && !soundOnly) inserted.include_audio = false;
            if (!audioTargets.empty()) {
                inserted.link_group_id = linkGroupId;
                inserted.sync_anchor_clip_id = videoClipId;
                inserted.sync_reference_delta = {0, 1};
            }
            clips.push_back(std::move(inserted));
        }
        std::stable_sort(
            clips.begin(), clips.end(),
            [](const DocumentClip& left, const DocumentClip& right) {
                return left.timeline_in < right.timeline_in;
            });
        results.push_back({track.id, std::move(clips)});
    }
    if (results.empty()) return std::nullopt;
    const auto targetResult = std::find_if(
        results.begin(), results.end(), [&](const ExactTrackState& state) {
            return state.track_id == targetTrackId;
        });
    if (targetResult == results.end()) return std::nullopt;
    DeleteGapOperation operation{
        targetTrackId, timelineIn, duration, results, {}};
    for (const ExactTrackState& state : results)
        if (state.track_id != targetTrackId)
            operation.linked_track_ids.push_back(state.track_id);
    return operation;
}

std::optional<TimelineDropTracks> TimelineDropTracksAt(
    const Document& document, const TimelineViewport& viewport, double rowY,
    bool video, bool audio, std::string& refusal) {
    refusal.clear();
    if (rowY < kTimelineRulerHeight || viewport.track_height <= 0.0)
        return std::nullopt;
    if (!video && !audio) {
        refusal = "Rien à monter : ce média n'a pas cette partie.";
        return std::nullopt;
    }
    const std::vector<const DocumentTrack*> rows =
        TimelineTracksInDisplayOrder(document);
    const size_t row = static_cast<size_t>((rowY - kTimelineRulerHeight) /
                                           viewport.track_height);
    const DocumentTrack* pointed = row < rows.size() ? rows[row] : nullptr;
    TimelineDropTracks tracks;
    if (!pointed || (pointed->kind != "video" && pointed->kind != "audio")) {
        // The blank below the last track, or a caption track, which takes
        // no media: new tracks.
        tracks.create_video_track = video;
        tracks.create_audio_track = audio;
        return tracks;
    }
    const bool onVideo = pointed->kind == "video";
    if (pointed->locked && (onVideo ? video : audio)) {
        refusal = "La piste " + TrackDisplayName(document, pointed->id) +
                  " est verrouillée.";
        return std::nullopt;
    }
    // The tracks of one kind as their headers number them, V1 and A1 first.
    const auto ofKind = [&](const std::string& kind) {
        std::vector<const DocumentTrack*> tracksOfKind;
        for (const DocumentTrack* track : rows)
            if (track->kind == kind) tracksOfKind.push_back(track);
        std::stable_sort(tracksOfKind.begin(), tracksOfKind.end(),
                         [](const DocumentTrack* a, const DocumentTrack* b) {
                             return a->index < b->index;
                         });
        return tracksOfKind;
    };
    const std::vector<const DocumentTrack*> videoTracks = ofKind("video");
    const std::vector<const DocumentTrack*> audioTracks = ofKind("audio");
    const std::vector<const DocumentTrack*>& pointedKind =
        onVideo ? videoTracks : audioTracks;
    const size_t number = static_cast<size_t>(
        std::find(pointedKind.begin(), pointedKind.end(), pointed) -
        pointedKind.begin());
    const auto take = [&](const std::vector<const DocumentTrack*>& kind,
                          Ulid& trackId, bool& create) {
        if (number < kind.size() && !kind[number]->locked)
            trackId = kind[number]->id;
        else
            create = true;
    };
    if (video)
        take(videoTracks, tracks.video_track_id, tracks.create_video_track);
    if (audio)
        take(audioTracks, tracks.audio_track_id, tracks.create_audio_track);
    return tracks;
}

bool ResolveMarkerColor(const std::string& category, const std::string& asked,
                        std::string& color, std::string& error) {
    const auto fold = [](std::string text) {
        for (char& character : text) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return text;
    };
    const bool question = category == kTimelineMarkerAgentCategory;
    const bool agentColor = fold(asked) == kTimelineMarkerAgentColor;
    if (question) {
        if (!asked.empty() && !agentColor) {
            error = std::string("a '") + kTimelineMarkerAgentCategory +
                    "' marker is the agent's and keeps its colour " +
                    kTimelineMarkerAgentColor;
            return false;
        }
        color = kTimelineMarkerAgentColor;
        return true;
    }
    if (agentColor) {
        error = std::string(kTimelineMarkerAgentColor) +
                " is kept for the agent's '" + kTimelineMarkerAgentCategory +
                "' markers";
        return false;
    }
    if (!asked.empty()) {
        color = asked;
    }
    return true;
}
