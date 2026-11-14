// DecodeWorker's read-ahead at rest and in playback (PERF10 --
// docs/roadmap/PERF-2026-09.md), against a real decode of a generated clip.
//
// What it guards: a paused editor keeps a few frames around the playhead,
// not the prefetch window playback needs. Idle on one 4K rush, that window
// held 42 frames, 1.4 GB, for a picture nobody was moving.

#include "DecodeWorker.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "Ulid.h"

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

// The number of cached frames once the worker has stopped adding any.
size_t SettledCount(const FrameCache& cache) {
    size_t count = cache.EntryCount();
    auto stable = std::chrono::steady_clock::now();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const size_t now = cache.EntryCount();
        if (now != count) {
            count = now;
            stable = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - stable >
                   std::chrono::milliseconds(300)) {
            break;
        }
    }
    return count;
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-worker");
    std::filesystem::create_directories(root);
    const std::filesystem::path clip = root / "clip.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=s=64x32:r=25:d=8' -c:v mpeg4 -g 25 -y " +
        Quote(clip);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate the clip\n";
        return 1;
    }
    // A budget large enough that the prefetch window covers the whole clip.
    FrameCache cache(size_t{64} << 20);
    PerformanceMetrics metrics;
    DecodeWorker worker("SRC", cache, metrics);
    Check(worker.Open(clip.string(), 2), "the clip opens");
    worker.Start();

    worker.RequestFrame(100);
    const size_t resting = SettledCount(cache);
    Check(resting >= 1 && resting <= 1 + 2 * DecodeWorker::kRestingFrames,
          "at rest the worker caches the frame and a few around it, got " +
              std::to_string(resting));
    Check(cache.Contains("SRC", 100) && cache.Contains("SRC", 104) &&
              cache.Contains("SRC", 96),
          "enough on either side to step frame by frame");

    worker.SetPlaying(true);
    worker.RequestFrame(101);
    const size_t playing = SettledCount(cache);
    Check(playing > 40,
          "in playback the worker reads ahead the whole window, got " +
              std::to_string(playing));

    worker.Stop();
    std::filesystem::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "decode worker tests passed\n";
    return 0;
}
