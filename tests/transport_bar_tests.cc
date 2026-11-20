// Pure-C++ test for the Transport panel's scrub-bar fraction<->time
// boundary (ROADMAP.md F2.5). TransportBar.h has no AppKit dependency, so
// this builds and runs on a plain Linux host, the same way
// tests/timeline_view_tests.cc does for TimelineViewport.

#include "TransportBar.h"

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "TestSupport.h"

int main() {
    Test("J/L shuttle ramps and reverses deterministically", [] {
        Check(NextShuttleSpeed(0, 1) == 1, "first L press -> +1x");
        Check(NextShuttleSpeed(1, 1) == 2, "second L press -> +2x");
        Check(NextShuttleSpeed(2, 1) == 4, "third L press -> +4x");
        Check(NextShuttleSpeed(4, 1) == 4, "L remains capped at +4x");
        Check(NextShuttleSpeed(4, -1) == -1, "J reverses direction at -1x");
        Check(NextShuttleSpeed(-1, -1) == -2, "second J press -> -2x");
        Check(NextShuttleSpeed(-2, -1) == -4, "third J press -> -4x");
        Check(NextShuttleSpeed(-4, 0) == 0, "K stops shuttle playback");
    });

    Test("TimeToFraction: start and end of the range map to 0 and 1", [] {
        const ScrubBarRange range{RationalTime{100, 25}};  // 4 seconds @25fps
        Check(range.TimeToFraction({0, 25}) == 0.0, "time zero -> 0.0");
        Check(range.TimeToFraction({100, 25}) == 1.0, "duration -> 1.0");
    });

    Test("TimeToFraction: midpoint is 0.5 regardless of the time's own rate",
         [] {
             const ScrubBarRange range{RationalTime{100, 25}};
             const double atNativeRate = range.TimeToFraction({50, 25});
             const double atOtherRate = range.TimeToFraction({1, 1});  // 1s
             Check(std::abs(atNativeRate - 0.5) < 1e-12,
                   "50/25 (2s of 4s) should be fraction 0.5");
             Check(std::abs(atOtherRate - 0.25) < 1e-12,
                   "1s of 4s should be fraction 0.25 regardless of rate");
         });

    Test(
        "TimeToFraction clamps out-of-range positions instead of "
        "extrapolating",
        [] {
            const ScrubBarRange range{RationalTime{100, 25}};
            Check(range.TimeToFraction({-25, 25}) == 0.0,
                  "negative time clamps to 0.0");
            Check(range.TimeToFraction({200, 25}) == 1.0,
                  "past-duration time clamps to 1.0");
        });

    Test("TimeToFraction on a zero-duration sequence is always 0", [] {
        const ScrubBarRange range{RationalTime{0, 25}};
        Check(range.TimeToFraction({0, 25}) == 0.0, "empty sequence -> 0.0");
    });

    Test("FractionToTime: 0 and 1 land exactly on the endpoints", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        const RationalTime start = range.FractionToTime(0.0, 25);
        const RationalTime end = range.FractionToTime(1.0, 25);
        Check(start == RationalTime(0, 25), "fraction 0.0 -> time zero");
        Check(end == RationalTime(100, 25), "fraction 1.0 -> duration");
    });

    Test("FractionToTime clamps fractions outside [0, 1]", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        Check(range.FractionToTime(-0.5, 25) == RationalTime(0, 25),
              "negative fraction clamps to time zero");
        Check(range.FractionToTime(1.5, 25) == RationalTime(100, 25),
              "fraction past 1.0 clamps to duration");
    });

    Test("FractionToTime rounds to the nearest tick at the requested rate", [] {
        const ScrubBarRange range{RationalTime{3, 1}};  // 3 seconds
        const RationalTime quarter = range.FractionToTime(0.5, 2);
        // 1.5s at rate 2 is exactly value 3.
        Check(quarter == RationalTime(3, 2),
              "half of 3s at rate 2 should be exactly 1.5s");
    });

    Test(
        "round-tripping TimeToFraction -> FractionToTime is stable at the "
        "duration's own rate",
        [] {
            const ScrubBarRange range{RationalTime{48000, 48000}};  // 1s
            for (int64_t sample : {0LL, 12000LL, 24000LL, 36000LL, 48000LL}) {
                const RationalTime original{sample, 48000};
                const double fraction = range.TimeToFraction(original);
                const RationalTime roundTripped =
                    range.FractionToTime(fraction, 48000);
                Check(roundTripped == original,
                      "round trip through a fraction must land back on the "
                      "same sample-accurate time");
            }
        });

    Test("FractionToTime rejects a non-finite fraction", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        bool threw = false;
        try {
            range.FractionToTime(std::nan(""), 25);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "NaN fraction must be rejected, not silently clamped");
    });

    Test("both directions reject a non-positive rate", [] {
        const ScrubBarRange range{RationalTime{100, 25}};
        bool threw = false;
        try {
            range.FractionToTime(0.5, 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "rate <= 0 in FractionToTime must throw");

        threw = false;
        try {
            range.TimeToFraction({10, 0});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "a time with rate <= 0 in TimeToFraction must throw");
    });

    Test("a zero-rate duration is rejected by both directions", [] {
        const ScrubBarRange range{RationalTime{0, 0}};
        bool threw = false;
        try {
            range.TimeToFraction({0, 25});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "TimeToFraction must reject an invalid duration rate");

        threw = false;
        try {
            range.FractionToTime(0.5, 25);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        Check(threw, "FractionToTime must reject an invalid duration rate");
    });

    // Out of lot -- the author, 29 October 2026: the source monitor plays.
    Test("source playback advances with the clock and stops at an end", [] {
        const RationalTime duration{250, 25};
        const SourcePlaybackStep half =
            AdvanceSourcePlayback({25, 25}, 1, 500000, duration);
        Check(half.position == RationalTime{3, 2} && !half.ended,
              "half a second at 1x from 1 s reads 1.5 s");
        Check(AdvanceSourcePlayback({25, 25}, 2, 500000, duration).position ==
                  RationalTime{2, 1},
              "L twice plays at 2x");
        const SourcePlaybackStep end =
            AdvanceSourcePlayback({245, 25}, 1, 400000, duration);
        Check(end.position == duration && end.ended,
              "playing past the end stops on the last frame");
        const SourcePlaybackStep start =
            AdvanceSourcePlayback({5, 25}, -1, 400000, duration);
        Check(start.position == RationalTime{0, 25} && start.ended,
              "J past the start stops at zero");
        Check(AdvanceSourcePlayback({25, 25}, 0, 900000, duration).position ==
                  RationalTime{25, 25},
              "stopped, the clock does not move it");
    });

    Test("the source bar maps the whole width to the whole rush", [] {
        const ScrubBarRange range{{10, 1}};
        const SourceBarGeometry middle = LayoutSourceBar(
            400.0, 300.0, range, {5, 1}, std::nullopt, std::nullopt);
        Check(middle.track_x == 0.0 && middle.track_width == 400.0 &&
                  middle.track_y == 290.0 && middle.zone_width == 0.0,
              "the bar spans the view at its foot, no zone without In/Out");
        Check(std::abs(middle.playhead_x - 199.0) < 1e-9,
              "the playhead is centred on the middle of the rush");
        const SourceBarGeometry zone =
            LayoutSourceBar(400.0, 300.0, range, {5, 1}, RationalTime{2, 1},
                            RationalTime{4, 1});
        Check(std::abs(zone.zone_x - 80.0) < 1e-9 &&
                  std::abs(zone.zone_width - 80.0) < 1e-9,
              "In 2 s and Out 4 s mark 80 to 160");
        const SourceBarGeometry open = LayoutSourceBar(
            400.0, 300.0, range, {5, 1}, RationalTime{2, 1}, std::nullopt);
        Check(std::abs(open.zone_x - 80.0) < 1e-9 &&
                  std::abs(open.zone_width - 320.0) < 1e-9,
              "an In alone runs to the end of the rush");
        const SourceBarGeometry last = LayoutSourceBar(
            400.0, 300.0, range, {10, 1}, std::nullopt, std::nullopt);
        Check(std::abs(last.playhead_x - 398.0) < 1e-9,
              "the playhead at the end stays inside the view");
    });

    return failures == 0 ? 0 : 1;
}
