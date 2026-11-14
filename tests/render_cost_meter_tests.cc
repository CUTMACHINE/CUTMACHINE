// Pure-C++ test for the timeline render cost report (PERF7 -- ROADMAP.md).
// RenderCostMeter.h has no AppKit dependency, so this builds and runs on a
// plain Linux host, like tests/playback_presentation_tests.cc.
//
// What it guards: the numbers main.mm hands over come out of the report
// unchanged -- the right percentile, microseconds from nanoseconds, a line
// per window of display ticks, the window still open written when the
// editor quits -- and the report stays off unless asked for.

#include "RenderCostMeter.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

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
    function();
    if (before == failures) {
        std::cout << "PASS: " << name << '\n';
    }
}

std::vector<std::string> Split(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream input(line);
    while (std::getline(input, field, separator)) {
        fields.push_back(field);
    }
    return fields;
}

std::vector<std::string> Lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

render_cost::Sample SampleOf(int64_t buildUs, int64_t submitUs,
                             uint64_t rects) {
    render_cost::Sample sample;
    sample.build_ns = buildUs * 1000;
    sample.submit_ns = submitUs * 1000;
    sample.counts.rects = rects;
    sample.counts.texts = 3;
    sample.counts.text_bytes = 20;
    sample.counts.icons = 7;
    return sample;
}

}  // namespace

int main() try {
    Test("percentiles are nearest-rank, as PerformanceMetrics reports", [] {
        std::vector<int64_t> hundred;
        for (int64_t value = 100; value >= 1; --value) {
            hundred.push_back(value);
        }
        const render_cost::Summary summary = render_cost::Summarize(hundred);
        Check(summary.median == 50 && summary.p95 == 95 && summary.max == 100,
              "1..100 summarises to 50 / 95 / 100");
        const render_cost::Summary one = render_cost::Summarize({42});
        Check(one.median == 42 && one.p95 == 42 && one.max == 42,
              "a single sample is its own median, p95 and maximum");
        const render_cost::Summary none = render_cost::Summarize({});
        Check(none.median == 0 && none.p95 == 0 && none.max == 0,
              "a window without a build summarises to zeros");
    });

    Test("a window closes on its last tick and starts the next one", [] {
        const int64_t start = 1'000'000'000;
        render_cost::Meter meter(start, 3);
        meter.Record(SampleOf(100, 900, 50));
        Check(!meter.Tick() && !meter.Tick(), "two ticks do not close it");
        meter.Record(SampleOf(300, 700, 60));
        meter.Record(SampleOf(200, 800, 70));
        Check(meter.Tick(), "the third tick closes it");
        const render_cost::Window window = meter.Take(start + 2'500'000'000);
        Check(window.ticks == 3 && window.builds == 3,
              "three ticks and three builds");
        Check(window.start_s == 0.0 && window.end_s == 2.5,
              "seconds are counted from the meter's start");
        Check(window.build_us.median == 200 && window.build_us.max == 300,
              "build durations are microseconds, median and maximum");
        Check(window.submit_us.median == 800 && window.submit_us.p95 == 900,
              "submit durations are summarised separately");
        Check(window.rects.median == 60 && window.rects.max == 70 &&
                  window.texts.max == 3 && window.text_bytes.max == 20 &&
                  window.icons.median == 7,
              "command counts are summarised per build");
        Check(meter.Empty(), "taking a window empties the meter");
        Check(!meter.Tick(), "the next window counts from zero");
        const render_cost::Window idle = meter.Take(start + 3'000'000'000);
        Check(idle.start_s == 2.5 && idle.ticks == 1 && idle.builds == 0 &&
                  idle.build_us.max == 0,
              "a tick without a rebuild is reported as such");
    });

    Test("a line has one field per header column", [] {
        render_cost::Meter meter(0, 1);
        meter.Record(SampleOf(1234, 5678, 90));
        meter.Tick();
        const std::string line =
            render_cost::FormatWindow(meter.Take(5'000'000'000));
        const std::vector<std::string> header =
            Split(render_cost::HeaderLine(), '\t');
        const std::vector<std::string> fields = Split(line, '\t');
        Check(header.size() == 18 && fields.size() == header.size(),
              "18 columns in the header and in the line");
        Check(line ==
                  "0.0\t5.0\t1\t1\t1234\t1234\t1234\t5678\t5678\t5678"
                  "\t90\t90\t3\t3\t20\t20\t7\t7",
              "the line carries the values as recorded: " + line);
    });

    Test("the report is off unless asked for", [] {
        using Kind = render_cost::Destination::Kind;
        Check(render_cost::ParseDestination(nullptr).kind == Kind::kOff &&
                  render_cost::ParseDestination("").kind == Kind::kOff,
              "unset or empty is off");
        Check(render_cost::ParseDestination("1").kind == Kind::kStderr &&
                  render_cost::ParseDestination("stderr").kind == Kind::kStderr,
              "1 and stderr write to the app's stderr");
        const render_cost::Destination file =
            render_cost::ParseDestination("/tmp/render.tsv");
        Check(file.kind == Kind::kFile && file.path == "/tmp/render.tsv",
              "anything else is a file");
        Check(render_cost::Recorder::Open({}, 0) == nullptr,
              "off opens no recorder");
        Check(render_cost::Recorder::Open(
                  render_cost::ParseDestination(
                      "/nonexistent-directory-perf7/render.tsv"),
                  0) == nullptr,
              "a file that cannot be opened leaves the editor unmeasured, "
              "not stopped");
    });

    Test("a recorder writes each window, and the open one when it ends", [] {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("cutmachine-render-cost-" + std::to_string(::getpid()) + ".tsv");
        std::filesystem::remove(path);
        {
            auto recorder = render_cost::Recorder::Open(
                render_cost::ParseDestination(path.c_str()),
                render_cost::NowNanoseconds());
            Check(recorder != nullptr, "a writable file opens a recorder");
            if (!recorder) {
                return;
            }
            recorder->Record(SampleOf(10, 20, 30));
            for (uint64_t tick = 0; tick < render_cost::kTicksPerWindow;
                 ++tick) {
                recorder->Tick(render_cost::NowNanoseconds());
            }
            recorder->Record(SampleOf(40, 50, 60));
            for (int tick = 0; tick < 5; ++tick) {
                recorder->Tick(render_cost::NowNanoseconds());
            }
        }
        const std::vector<std::string> lines = Lines(path.string());
        Check(lines.size() == 4, "comment, header and two windows");
        if (lines.size() == 4) {
            Check(lines[0].rfind("# PERF7", 0) == 0, "a comment opens it");
            Check(lines[1] == render_cost::HeaderLine(), "then the header");
            const std::vector<std::string> full = Split(lines[2], '\t');
            const std::vector<std::string> partial = Split(lines[3], '\t');
            Check(full.size() == 18 && full[2] == "300" && full[3] == "1" &&
                      full[4] == "10" && full[10] == "30",
                  "a full window of 300 ticks with its one build");
            Check(partial.size() == 18 && partial[2] == "5" &&
                      partial[3] == "1" && partial[4] == "40",
                  "the window open at the end is written, not dropped");
        }
        {
            auto recorder = render_cost::Recorder::Open(
                render_cost::ParseDestination(path.c_str()),
                render_cost::NowNanoseconds());
        }
        Check(Lines(path.string()).size() == 6,
              "a second launch appends its own header and writes no empty "
              "window");
        std::filesystem::remove(path);
    });

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "Render cost meter tests passed\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
}
