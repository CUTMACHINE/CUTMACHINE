// VFR-2026-11 -- docs/roadmap/VFR-2026-11.md. Which picture a frame number
// names (SourceGrid.h): the rate a source is counted at, the rounding of a
// picture onto the grid, and the read the export and the verification
// share. The pictures themselves are checked by the export test, on a file
// whose timestamps drift like a phone's (tests/export_tests.cc).
//
// Portable: no AppKit, no Metal; FFmpeg's headers only through the model.

#include "SourceGrid.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

bool Same(MediaRate left, MediaRate right) {
    return static_cast<int64_t>(left.num) * right.den ==
           static_cast<int64_t>(right.num) * left.den;
}

bool Has(const std::vector<std::string>& arguments, const std::string& option,
         const std::string& value) {
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == option && arguments[index + 1] == value)
            return true;
    }
    return false;
}

}  // namespace

int main() {
    Test("VF2: a phone's variable rate is counted on its nominal grid", [] {
        // Averages measured on fourteen phone rushes, 15 November 2026:
        // the nominal rate FFmpeg reads from their timestamps is 30/1.
        for (const MediaRate average :
             {MediaRate{42975000, 1432321}, MediaRate{26760000, 893383},
              MediaRate{23460000, 782267}, MediaRate{81360000, 2713393},
              MediaRate{2025000, 68683}}) {
            Check(Same(SourceCountingRate(average, {30, 1}), {30, 1}),
                  "the average " + std::to_string(average.num) + "/" +
                      std::to_string(average.den) + " is counted at 30/1");
        }
    });

    Test("VF2: every other stream keeps its average", [] {
        Check(Same(SourceCountingRate({30000, 1001}, {30, 1}), {30000, 1001}),
              "a standard average is kept even when the nominal says 30");
        Check(Same(SourceCountingRate({24000, 1001}, {24, 1}), {24000, 1001}),
              "a genuine 23.976 is never counted at 24");
        Check(Same(SourceCountingRate({25, 1}, {25, 1}), {25, 1}),
              "a constant-rate stream keeps its rate");
        Check(Same(SourceCountingRate({29, 1}, {30, 1}), {29, 1}),
              "an average 3.3 % off the nominal is a cadence of its own");
        Check(Same(SourceCountingRate({2700, 91}, {90000, 1}), {2700, 91}),
              "a nominal that is no cadence (a time base) is not taken");
        Check(Same(SourceCountingRate({2700, 91}, {0, 1}), {2700, 91}),
              "an invalid nominal is not taken");
        Check(Same(SourceCountingRate({0, 1}, {30, 1}), {0, 1}),
              "an invalid average is left for ingest to refuse");
    });

    Test("VF2: the standard cadences", [] {
        for (const MediaRate rate :
             {MediaRate{24000, 1001}, MediaRate{48, 2}, MediaRate{25, 1},
              MediaRate{60000, 1001}, MediaRate{120000, 1001},
              MediaRate{240, 1}}) {
            Check(IsStandardFrameRate(rate), std::to_string(rate.num) + "/" +
                                                 std::to_string(rate.den) +
                                                 " is a standard cadence");
        }
        Check(!IsStandardFrameRate({2997, 100}),
              "29.97 written as 2997/100 is not 30000/1001");
        Check(!IsStandardFrameRate({42975000, 1432321}),
              "a phone's average is not a cadence");
    });

    Test("VF1: a picture is rounded onto the grid as fps does", [] {
        const MediaRate tick{1, 90000};
        const MediaRate rate{30, 1};
        Check(SourceGridStep(0, 0, tick, rate) == 0, "the origin is step 0");
        Check(SourceGridStep(1499, 0, tick, rate) == 0,
              "just under half a step rounds down");
        Check(SourceGridStep(1500, 0, tick, rate) == 1,
              "half a step rounds away from zero");
        Check(SourceGridStep(-1500, 0, tick, rate) == -1,
              "half a step before the origin rounds away from zero too");
        Check(SourceGridStep(7503, 3, tick, rate) == 3,
              "the step is counted from the origin");
        Check(SourceGridStep(int64_t{1001} * 7, 0, {1, 30000}, {30000, 1001}) ==
                  7,
              "a 29.97 picture sits on its step");
    });

    Test("VF3: the first pts of a step is the rounding's boundary", [] {
        struct Case {
            MediaRate tick;
            MediaRate rate;
            int64_t origin;
        };
        for (const Case& item :
             {Case{{1, 90000}, {30, 1}, 0}, Case{{1, 600}, {30, 1}, 7},
              Case{{1, 30000}, {30000, 1001}, -5},
              Case{{1001, 30000}, {30000, 1001}, 0},
              Case{{1, 15360}, {25, 1}, 1024}}) {
            for (int64_t step = -3; step < 40; ++step) {
                const int64_t first =
                    SourceGridFirstPts(step, item.origin, item.tick, item.rate);
                Check(SourceGridStep(first, item.origin, item.tick,
                                     item.rate) >= step &&
                          SourceGridStep(first - 1, item.origin, item.tick,
                                         item.rate) < step,
                      "step " + std::to_string(step) + " at " +
                          std::to_string(item.tick.num) + "/" +
                          std::to_string(item.tick.den));
            }
        }
        Check(SourceGridFirstPts(1, 0, {1, 90000}, {30, 1}) == 1500,
              "frame 1 starts half a step after the origin");
        Check(SourceGridFirstPts(0, 0, {1, 90000}, {30, 1}) == -1499,
              "frame 0 takes a picture just before the origin");
    });

    Test("VF1: the grid read the export and the verification share", [] {
        const SourceGridRead late =
            SourceGridReadFor(0, {30, 1}, 90, 120, false);
        Check(Has(late.input_arguments, "-ss", "2.000000"),
              "the seek lands a second ahead of the first frame");
        Check(
            std::find(late.input_arguments.begin(), late.input_arguments.end(),
                      "-copyts") != late.input_arguments.end(),
            "timestamps are kept as the file has them");
        Check(
            std::find(late.input_arguments.begin(), late.input_arguments.end(),
                      "-t") == late.input_arguments.end(),
            "no duration unless asked");
        Check(late.filter ==
                  "setpts=PTS-0,fps=30/1:start_time=0,trim=start_frame=90",
              "the grid is anchored on the first picture: " + late.filter);

        const SourceGridRead early =
            SourceGridReadFor(1001, {30000, 1001}, 10, 40, true);
        Check(std::find(early.input_arguments.begin(),
                        early.input_arguments.end(),
                        "-ss") == early.input_arguments.end(),
              "a frame in the first second is read from the start");
        Check(Has(early.input_arguments, "-t", "2.401400"),
              "the read stops a second after the last frame's successor");
        Check(early.filter ==
                  "setpts=PTS-1001,fps=30000/1001:start_time=0,"
                  "trim=start_frame=10",
              "the origin is the first picture's own timestamp, an integer "
              "FFmpeg subtracts exactly: " +
                  early.filter);
    });

    if (failures > 0) {
        std::cerr << failures << " source grid check(s) failed\n";
        return 1;
    }
    std::cout << "all source grid tests passed\n";
    return 0;
}
