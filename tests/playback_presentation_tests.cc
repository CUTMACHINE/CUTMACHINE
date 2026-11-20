// Pure-C++ test for the Record monitor's hold rule (PERF-2026-09).
// PlaybackPresentation.h has no AppKit dependency, so this builds and runs on
// a plain Linux host, the same way tests/panel_layout_tests.cc does.
//
// What it guards: a video layer whose decoder is late must never composite.
// Renderer.mm skips a null frame, so presenting one is not "an image that is
// not ready" but a hole -- and the track underneath shows through it. That
// hole is the flash frame seen when playback crosses a cut onto a source the
// frame cache has never held.

#include "PlaybackPresentation.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

playback::LayerReadiness Ready() { return {true, true, true}; }
playback::LayerReadiness Late() { return {true, false, true}; }
playback::LayerReadiness Offline() { return {true, false, false}; }
playback::LayerReadiness Hole() { return {false, false, false}; }

}  // namespace

int main() {
    Test("a complete composite is presented, and clears the counter", [] {
        const auto decision =
            playback::DecideProgramHold({Ready(), Ready()}, true, 7);
        Check(!decision.hold, "every layer has an image: present it");
        Check(decision.held_ticks == 0,
              "the counter resets as soon as nothing is late");
    });

    Test("a late layer holds the previous image", [] {
        const auto decision =
            playback::DecideProgramHold({Ready(), Late()}, true, 0);
        Check(decision.hold,
              "an undecoded layer over a decoded one must not composite: that "
              "is the flash of the lower track");
        Check(decision.held_ticks == 1, "a held tick is counted");
    });

    Test("a late layer under a ready one holds too", [] {
        Check(playback::DecideProgramHold({Late(), Ready()}, true, 0).hold,
              "a hole below is still a hole -- letterboxing and opacity mean "
              "the layer above does not necessarily cover it");
    });

    Test("the first present is never held", [] {
        const auto decision = playback::DecideProgramHold({Late()}, false, 0);
        Check(!decision.hold,
              "with nothing on screen yet there is no previous image to keep");
        Check(decision.held_ticks == 0, "and no tick to count");
    });

    Test("an offline source never holds the monitor", [] {
        Check(!playback::DecideProgramHold({Ready(), Offline()}, true, 0).hold,
              "a source without a decoder will never deliver: holding would "
              "freeze the monitor for good");
    });

    Test("a hole the edit asked for is not a late layer", [] {
        Check(!playback::DecideProgramHold({Ready(), Hole()}, true, 0).hold,
              "an inactive layer reveals the track below on purpose");
        Check(!playback::DecideProgramHold({Hole(), Hole()}, true, 0).hold,
              "an empty position presents as empty");
    });

    Test("the hold is bounded, and does not restart once spent", [] {
        const auto last = playback::DecideProgramHold(
            {Late()}, true, playback::kMaximumHeldTicks - 1);
        Check(last.hold && last.held_ticks == playback::kMaximumHeldTicks,
              "the last tick of the budget still holds");
        const auto spent = playback::DecideProgramHold(
            {Late()}, true, playback::kMaximumHeldTicks);
        Check(!spent.hold,
              "past the budget the composite is presented as it stands, hole "
              "included");
        Check(spent.held_ticks == playback::kMaximumHeldTicks,
              "the counter stays spent while the layer is late, instead of "
              "alternating between a held image and a hole");
        Check(playback::DecideProgramHold({Ready()}, true,
                                          playback::kMaximumHeldTicks)
                      .held_ticks == 0,
              "the frame landing gives the next stall a full budget again");
    });

    // PRD37 -- the sound output stays open between two plays: closing it at
    // every stop made each start wait 100 to 200 ms for the device.
    Test("a silent output stays open for the whole grace, then closes", [] {
        using std::chrono::milliseconds;
        const auto grace = playback::kAudioOutputIdleGrace;
        Check(!playback::ShouldReleaseAudioOutput(true, false, milliseconds(0)),
              "a stop does not close the output");
        Check(!playback::ShouldReleaseAudioOutput(true, false,
                                                  grace - milliseconds(1)),
              "within the grace, the next play finds the output open");
        Check(playback::ShouldReleaseAudioOutput(true, false, grace),
              "silent for the whole grace, it closes");
        Check(grace >= std::chrono::seconds(5) &&
                  grace <= std::chrono::minutes(1),
              "long enough for a pause and a trim, short against the sleep "
              "timer");
    });

    Test("an output that sounds, or is closed, is left alone", [] {
        const auto long_ago = std::chrono::hours(1);
        Check(!playback::ShouldReleaseAudioOutput(true, true, long_ago),
              "a play or a scrub still sounding keeps it open");
        Check(!playback::ShouldReleaseAudioOutput(false, false, long_ago),
              "a closed output has nothing to close");
    });

    return failures == 0 ? 0 : 1;
}
