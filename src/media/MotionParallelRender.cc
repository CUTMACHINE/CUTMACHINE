#include "MotionParallelRender.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <thread>

bool RenderMotionFramesToFiles(
    const MotionFrameRequest& base, const std::vector<int64_t>& frames,
    const std::function<std::string(int64_t)>& pathFor,
    const std::atomic_bool* cancel, std::string& error) {
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        error = std::string(
                    "this process has no motion renderer (Core Text and "
                    "Core Graphics) to render motion with: ") +
                kMotionRendererHolder;
        return false;
    }
    // One per core, twelve at most: a worker holds a few full-frame float
    // layers while an effect runs, and memory is the limit past that. MA7 --
    // never more than there are frames, and none for none: std::clamp was
    // given 1 as its bottom and the frame count as its top, undefined for
    // an empty list (ExportVerification can hand one over).
    const size_t workers = std::min<size_t>(
        {std::max<size_t>(std::thread::hardware_concurrency(), 1), 12,
         frames.size()});
    std::atomic<size_t> next{0};
    std::atomic_bool failed{false};
    std::mutex errorMutex;
    // PERF11 -- docs/roadmap/PERF-2026-09.md. The workers share one decoder
    // per video file. Left to themselves they ask it for their pictures in
    // any order, and each one that asks for a picture behind the decoder
    // makes it seek and decode again from the key frame: on 4K rushes the
    // workers spent three quarters of their time waiting for that. So the
    // frames decode in turn, in the order they were handed out, and only
    // decode: converting and painting stay in parallel.
    MotionMediaTurns turns;
    // Gives up the turn of a frame however its render ends.
    struct TurnLeft {
        MotionMediaTurns& turns;
        size_t ordinal;
        ~TurnLeft() { turns.Leave(ordinal); }
    };
    const auto work = [&]() {
        MotionFrameRequest request = base;
        request.media_turns = &turns;
        for (size_t index = next++; index < frames.size(); index = next++) {
            const TurnLeft left{turns, index};
            if (failed.load() || (cancel != nullptr && cancel->load())) {
                return;
            }
            request.frame = frames[index];
            request.media_turn = index;
            MotionFrameImage image;
            MotionError motionError;
            std::string message;
            if (!render(request, image, motionError)) {
                message =
                    "frame " + std::to_string(frames[index]) + ": " +
                    motionError.code +
                    (motionError.path.empty() ? ""
                                              : " at " + motionError.path) +
                    ": " + motionError.message;
            } else {
                const std::string path = pathFor(frames[index]);
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(image.png.data(),
                           static_cast<std::streamsize>(image.png.size()));
                file.close();
                if (!file) {
                    message = "unable to write motion frame '" + path + "'";
                }
            }
            if (!message.empty()) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!failed.exchange(true)) {
                    error = message;
                }
                return;
            }
        }
    };
    std::vector<std::thread> threads;
    for (size_t index = 1; index < workers; ++index) {
        threads.emplace_back(work);
    }
    work();
    for (std::thread& thread : threads) {
        thread.join();
    }
    if (failed.load()) {
        return false;
    }
    if (cancel != nullptr && cancel->load()) {
        error = "export cancelled";
        return false;
    }
    return true;
}
