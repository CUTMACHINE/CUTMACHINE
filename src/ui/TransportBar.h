#pragma once

// F2.5 -- ROADMAP.md. The Transport panel's scrub bar is, like
// TimelineView.h's TimelineViewport, the one place a UI slider position is
// allowed to cross into exact playhead time -- see PHILOSOPHY.md's principle
// 4 ("le temps est exact") and TimelineViewport's own file comment, which
// this mirrors for a simpler [0, duration] range instead of a scrolling,
// zoomable one. Plain C++, no AppKit, so it builds and is tested on a
// plain Linux host the same way TimelineViewport is (see
// tests/timeline_view_tests.cc and this file's own
// tests/transport_bar_tests.cc).
//
// FractionToTime only crosses the fraction -> time boundary; it does not
// itself snap to a frame or sample grid. A caller must run the result through
// TimelineView.h's QuantizePlayheadPosition before it becomes the
// authoritative playhead -- exactly as the main timeline's ruler-drag scrub
// already does for TimelineViewport::XToTime. No raw pixel/fraction value is
// ever stored as playhead state; only the RationalTime this produces is.
//
// UI-2026-08 -- ScrubBarRange currently has no caller: the ATELIER redesign
// removed the transport dock slot and deleted the AppKit view that used it.
// It stays because it is the tested statement of that boundary, and a
// re-slotted transport needs the same rule rather than a fresh approximation
// of it. NextShuttleSpeed below is live, called from main.mm's J/K/L handling.

#include "RationalTime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

// Repeated J/L presses ramp shuttle playback through 1x, 2x and 4x. Pressing
// the opposite direction starts that direction at 1x; K remains zero.
inline int NextShuttleSpeed(int current, int direction) {
    if (direction == 0) return 0;
    const int sign = direction < 0 ? -1 : 1;
    if ((current < 0 ? -1 : current > 0 ? 1 : 0) != sign) return sign;
    const int magnitude = std::abs(current);
    return sign * (magnitude < 2 ? 2 : 4);
}

struct ScrubBarRange {
    // The sequence's total duration. A scrub fraction of 0.0 is time zero;
    // 1.0 is `duration`.
    RationalTime duration{0, 1};

    // Maps a playhead position to a slider fraction in [0, 1]. Positions
    // outside [0, duration] clamp rather than extrapolate -- a scrub knob
    // position is always a valid fraction, never off the end of the bar.
    double TimeToFraction(RationalTime time) const;

    // Maps a slider fraction (clamped to [0, 1] first) to an exact
    // RationalTime at `rate`. See the file comment for the quantization
    // this deliberately leaves to the caller.
    RationalTime FractionToTime(double fraction, int32_t rate) const;
};

inline double ScrubBarRange::TimeToFraction(RationalTime time) const {
    if (duration.rate <= 0)
        throw std::invalid_argument("scrub bar duration rate must be positive");
    if (time.rate <= 0)
        throw std::invalid_argument("time rate must be positive");
    if (duration.value <= 0) return 0.0;
    const long double positionSeconds =
        static_cast<long double>(time.value) / time.rate;
    const long double durationSeconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double fraction = positionSeconds / durationSeconds;
    return static_cast<double>(std::clamp(fraction, 0.0L, 1.0L));
}

inline RationalTime ScrubBarRange::FractionToTime(double fraction,
                                                  int32_t rate) const {
    if (duration.rate <= 0)
        throw std::invalid_argument("scrub bar duration rate must be positive");
    if (rate <= 0)
        throw std::invalid_argument("requested rate must be positive");
    if (!std::isfinite(fraction))
        throw std::invalid_argument("scrub fraction must be finite");
    const long double clamped =
        std::clamp(static_cast<long double>(fraction), 0.0L, 1.0L);
    const long double durationSeconds =
        static_cast<long double>(duration.value) / duration.rate;
    const long double value = durationSeconds * clamped * rate;
    if (!std::isfinite(value) ||
        value < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        value > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        throw std::overflow_error("scrub bar position exceeds int64 range");
    }
    // Round to nearest frame/tick at `rate`, halfway away from zero -- same
    // policy as TimelineViewport::XToTime's RoundedInt64.
    return {static_cast<int64_t>(std::round(value)), rate};
}

// Out of lot -- the author, 29 October 2026: "I need to play in the source
// monitor, picture and sound, with an indicator of the player". Where a
// playing source monitor is `elapsedUs` microseconds after it started from
// `anchor` at `direction` (J/L's -4..4), held inside [0, duration]: `ended`
// once it reaches an end, where the monitor stops, as the timeline does
// (main.mm's -advancePlayback). The window quantizes to a frame when it asks
// for the picture.
struct SourcePlaybackStep {
    RationalTime position;
    bool ended = false;
};

inline SourcePlaybackStep AdvanceSourcePlayback(RationalTime anchor,
                                                int direction,
                                                int64_t elapsedUs,
                                                RationalTime duration) {
    if (direction == 0 || elapsedUs <= 0) return {anchor, false};
    const RationalTime next =
        anchor.add({elapsedUs * static_cast<int64_t>(direction), 1000000});
    const RationalTime start{0, duration.rate > 0 ? duration.rate : 1};
    if (next <= start) return {start, true};
    if (next >= duration) return {duration, true};
    return {next, false};
}

// The bar drawn under the source picture: its whole width is the whole
// rush, as the click that scrubs the monitor reads it (main.mm's
// -scrubSourceMonitorEvent:), the In/Out zone on it, and the playhead
// across it. View points, origin at the top left.
struct SourceBarGeometry {
    double track_x = 0.0;
    double track_y = 0.0;
    double track_width = 0.0;
    double track_height = 0.0;
    double zone_x = 0.0;
    double zone_width = 0.0;  // 0: no In and no Out
    double playhead_x = 0.0;
    double playhead_y = 0.0;
    double playhead_width = 0.0;
    double playhead_height = 0.0;
};

inline constexpr double kSourceBarHeight = 4.0;
inline constexpr double kSourceBarBottomInset = 6.0;
inline constexpr double kSourceBarPlayheadWidth = 2.0;
inline constexpr double kSourceBarPlayheadHeight = 14.0;

inline SourceBarGeometry LayoutSourceBar(double viewWidth, double viewHeight,
                                         const ScrubBarRange& range,
                                         RationalTime position,
                                         std::optional<RationalTime> in,
                                         std::optional<RationalTime> out) {
    SourceBarGeometry bar;
    bar.track_x = 0.0;
    bar.track_width = std::max(0.0, viewWidth);
    bar.track_height = kSourceBarHeight;
    bar.track_y = viewHeight - kSourceBarBottomInset - kSourceBarHeight;
    const auto at = [&](RationalTime time) {
        return bar.track_x + range.TimeToFraction(time) * bar.track_width;
    };
    if (in || out) {
        const double zoneStart = in ? at(*in) : bar.track_x;
        const double zoneEnd = out ? at(*out) : bar.track_x + bar.track_width;
        bar.zone_x = std::min(zoneStart, zoneEnd);
        bar.zone_width = std::abs(zoneEnd - zoneStart);
    }
    bar.playhead_width = kSourceBarPlayheadWidth;
    bar.playhead_height = kSourceBarPlayheadHeight;
    bar.playhead_x =
        std::clamp(at(position) - (kSourceBarPlayheadWidth / 2.0), bar.track_x,
                   std::max(bar.track_x, bar.track_x + bar.track_width -
                                             kSourceBarPlayheadWidth));
    bar.playhead_y = bar.track_y + (kSourceBarHeight / 2.0) -
                     (kSourceBarPlayheadHeight / 2.0);
    return bar;
}
