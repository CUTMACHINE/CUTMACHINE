#pragma once

// PERF7 -- ROADMAP.md. What drawing the timeline costs, measured in the
// editor instead of supposed.
//
// main.mm times two calls in -presentNearestFrameAtDeadline: -- building the
// display list (-timelineRenderData) and handing it to Renderer.mm
// (RenderFrames, which encodes one draw call per quad, waits for a drawable
// and then for the GPU) -- and counts the commands in the list. This file is
// the policy around those numbers: whether the measurement is on, how the
// samples of a window of display ticks are summarised, and the line each
// window becomes. Plain C++ with no AppKit dependency, like
// PlaybackPresentation.h, so tests/render_cost_meter_tests.cc runs it
// anywhere; main.mm only wires it.
//
// Off unless CUTMACHINE_RENDER_COST is set, and off means main.mm reads no
// clock and counts nothing. The report goes to a local file or to the app's
// stderr, never into the project: it describes this machine, not the edit.
// docs/PERF_BENCHMARK.md says how to read it.
//
// Instruments sees the same two intervals through os_signpost, which main.mm
// emits whether or not this report is on (os_signpost is a cheap enabled
// check until a recording asks for it).

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace render_cost {

inline constexpr char kEnvironmentVariable[] = "CUTMACHINE_RENDER_COST";

// 300 display ticks: five seconds of main.mm's 60 Hz timer. Short enough to
// tell idle, playback and scrubbing apart by the clock, long enough for a
// p95 to mean something.
inline constexpr uint64_t kTicksPerWindow = 300;

// What one display list asks Renderer.mm to draw. A rectangle and an icon
// are one quad each; a text becomes one quad per UTF-16 unit that fits its
// width, so `text_bytes` (UTF-8) is an upper bound on its glyph quads, not a
// count of them.
struct DisplayListCounts {
    uint64_t rects = 0;
    uint64_t texts = 0;
    uint64_t text_bytes = 0;
    uint64_t icons = 0;
};

struct Sample {
    int64_t build_ns = 0;
    int64_t submit_ns = 0;
    DisplayListCounts counts;
};

struct Summary {
    int64_t median = 0;
    int64_t p95 = 0;
    int64_t max = 0;
};

// Nearest rank, the definition PerformanceMetrics.cc uses for the decode
// latencies, so the two reports read the same way.
inline int64_t Percentile(const std::vector<int64_t>& sorted, size_t percent) {
    if (sorted.empty()) {
        return 0;
    }
    const size_t rank =
        std::max<size_t>(1, (percent * sorted.size() + 99) / 100);
    return sorted[std::min(rank - 1, sorted.size() - 1)];
}

inline Summary Summarize(std::vector<int64_t> values) {
    std::sort(values.begin(), values.end());
    return {Percentile(values, 50), Percentile(values, 95),
            values.empty() ? 0 : values.back()};
}

// One closed window. Durations in microseconds; `builds` against `ticks` is
// the answer to "is the display list rebuilt on every tick?".
struct Window {
    double start_s = 0.0;
    double end_s = 0.0;
    uint64_t ticks = 0;
    uint64_t builds = 0;
    Summary build_us;
    Summary submit_us;
    Summary rects;
    Summary texts;
    Summary text_bytes;
    Summary icons;
};

class Meter {
public:
    explicit Meter(int64_t startNs, uint64_t ticksPerWindow = kTicksPerWindow)
        : ticks_per_window_(std::max<uint64_t>(1, ticksPerWindow)),
          origin_ns_(startNs),
          window_start_ns_(startNs) {}

    // One display tick. True when it closes the window: call Take().
    bool Tick() { return ++ticks_ >= ticks_per_window_; }

    void Record(const Sample& sample) { samples_.push_back(sample); }

    bool Empty() const { return ticks_ == 0 && samples_.empty(); }

    // Summarises the window that ends now and starts the next one.
    Window Take(int64_t nowNs) {
        std::vector<int64_t> build;
        std::vector<int64_t> submit;
        std::vector<int64_t> rects;
        std::vector<int64_t> texts;
        std::vector<int64_t> textBytes;
        std::vector<int64_t> icons;
        for (const Sample& sample : samples_) {
            build.push_back(sample.build_ns / 1000);
            submit.push_back(sample.submit_ns / 1000);
            rects.push_back(static_cast<int64_t>(sample.counts.rects));
            texts.push_back(static_cast<int64_t>(sample.counts.texts));
            textBytes.push_back(static_cast<int64_t>(sample.counts.text_bytes));
            icons.push_back(static_cast<int64_t>(sample.counts.icons));
        }
        Window window;
        window.start_s = Seconds(window_start_ns_);
        window.end_s = Seconds(nowNs);
        window.ticks = ticks_;
        window.builds = samples_.size();
        window.build_us = Summarize(std::move(build));
        window.submit_us = Summarize(std::move(submit));
        window.rects = Summarize(std::move(rects));
        window.texts = Summarize(std::move(texts));
        window.text_bytes = Summarize(std::move(textBytes));
        window.icons = Summarize(std::move(icons));
        ticks_ = 0;
        samples_.clear();
        window_start_ns_ = nowNs;
        return window;
    }

private:
    double Seconds(int64_t ns) const {
        return static_cast<double>(ns - origin_ns_) / 1e9;
    }

    uint64_t ticks_per_window_;
    int64_t origin_ns_;
    int64_t window_start_ns_;
    uint64_t ticks_ = 0;
    std::vector<Sample> samples_;
};

// Tab-separated, one line per window; docs/PERF_BENCHMARK.md reads these
// columns by name, so keep them stable.
inline std::string HeaderLine() {
    return "start_s\tend_s\tticks\tbuilds"
           "\tbuild_med_us\tbuild_p95_us\tbuild_max_us"
           "\tsubmit_med_us\tsubmit_p95_us\tsubmit_max_us"
           "\trects_med\trects_max\ttexts_med\ttexts_max"
           "\ttext_bytes_med\ttext_bytes_max\ticons_med\ticons_max";
}

inline std::string FormatWindow(const Window& window) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "%.1f\t%.1f\t%" PRIu64 "\t%" PRIu64 "\t%" PRId64 "\t%" PRId64
                  "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64
                  "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64
                  "\t%" PRId64 "\t%" PRId64 "\t%" PRId64 "\t%" PRId64,
                  window.start_s, window.end_s, window.ticks, window.builds,
                  window.build_us.median, window.build_us.p95,
                  window.build_us.max, window.submit_us.median,
                  window.submit_us.p95, window.submit_us.max,
                  window.rects.median, window.rects.max, window.texts.median,
                  window.texts.max, window.text_bytes.median,
                  window.text_bytes.max, window.icons.median, window.icons.max);
    return line;
}

// Where CUTMACHINE_RENDER_COST sends the report. Unset or empty: off.
// "1" or "stderr": the app's stderr. Anything else: a file, appended to, so
// that several launches accumulate in one place.
struct Destination {
    enum class Kind : uint8_t { kOff, kStderr, kFile };
    Kind kind = Kind::kOff;
    std::string path;
};

inline Destination ParseDestination(const char* value) {
    if (value == nullptr || *value == '\0') {
        return {};
    }
    const std::string text(value);
    if (text == "1" || text == "stderr") {
        return {Destination::Kind::kStderr, {}};
    }
    return {Destination::Kind::kFile, text};
}

inline int64_t NowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Owns the output and the meter. Writes a line whenever a window closes and,
// on destruction, the window still open -- main.mm keeps it in a static,
// destroyed when the app exits, so the last minutes of a session are not
// lost.
class Recorder {
    // Only Open() can make one: the constructor is public for
    // std::make_unique, and takes a key nothing outside can name.
    struct Key {
        explicit Key() = default;
    };

public:
    // Null when the destination is off, or when the file cannot be opened
    // (said once on stderr): a measurement that cannot be written must not
    // stop the editor.
    static std::unique_ptr<Recorder> Open(const Destination& destination,
                                          int64_t nowNs) {
        std::FILE* output = nullptr;
        bool owned = false;
        switch (destination.kind) {
            case Destination::Kind::kOff:
                return nullptr;
            case Destination::Kind::kStderr:
                output = stderr;
                break;
            case Destination::Kind::kFile:
                output = std::fopen(destination.path.c_str(), "a");
                owned = true;
                if (output == nullptr) {
                    std::fprintf(stderr,
                                 "%s: unable to open '%s', timeline render "
                                 "cost not recorded\n",
                                 kEnvironmentVariable,
                                 destination.path.c_str());
                    return nullptr;
                }
                break;
        }
        return std::make_unique<Recorder>(Key{}, output, owned, nowNs);
    }

    Recorder(Key /*key*/, std::FILE* output, bool owned, int64_t nowNs)
        : output_(output), owned_(owned), meter_(nowNs) {
        std::fprintf(output_,
                     "# PERF7 timeline render cost, %" PRIu64
                     " display ticks per line\n%s\n",
                     kTicksPerWindow, HeaderLine().c_str());
        std::fflush(output_);
    }

    ~Recorder() {
        if (!meter_.Empty()) {
            Write(meter_.Take(NowNanoseconds()));
        }
        if (owned_) {
            std::fclose(output_);
        }
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void Tick(int64_t nowNs) {
        if (meter_.Tick()) {
            Write(meter_.Take(nowNs));
        }
    }

    void Record(const Sample& sample) { meter_.Record(sample); }

private:
    void Write(const Window& window) {
        std::fprintf(output_, "%s\n", FormatWindow(window).c_str());
        std::fflush(output_);
    }

    std::FILE* output_;
    bool owned_;
    Meter meter_;
};

}  // namespace render_cost
