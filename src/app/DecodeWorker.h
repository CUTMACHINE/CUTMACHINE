#pragma once

#include "FrameCache.h"
#include "MediaSource.h"
#include "PerformanceMetrics.h"
#include "ReadAhead.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class DecodeWorker {
public:
    DecodeWorker(const FrameCache::SourceId& sourceId, FrameCache& cache,
                 PerformanceMetrics& metrics);
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    bool Open(const std::string& path, int threadCount);
    // The media this worker actually opened, proxy substitution included.
    // Lets a caller tell a worker that is still correct from one that has to
    // be rebuilt, instead of rebuilding every worker on every change.
    const std::string& Path() const { return path_; }
    // PERF15 -- MediaSource::SampleTable, once open.
    std::shared_ptr<const std::vector<read_ahead::SampleRange>> SampleTable()
        const {
        return source_.SampleTable();
    }
    void Start();
    void Stop();
    // PERF14 -- docs/roadmap/PERF-2026-09.md. How a request is read ahead
    // in playback (ReadAhead.h). Empty, as every caller but the Record
    // monitor's playback leaves it: the window ahead, as before.
    struct ReadPlan {
        // The source frames of the shot the frame belongs to: pictures are
        // decoded ahead within them and no further.
        std::optional<read_ahead::SourceSpan> span;
        // Frames whose bytes are asked of the system ahead of the decoder
        // (read_ahead::NextByteAdvice), and the smallest piece asked at
        // once. 0 asks for nothing.
        int64_t byte_horizon = 0;
        int64_t byte_chunk = 0;
        // PERF17 -- the first source frame of the next piece of the same
        // rush the shot cuts to, when playback reaches that cut soon: the
        // window decodes on into it (read_ahead::FollowOnFill).
        std::optional<int64_t> next_piece;
    };
    void RequestFrame(int64_t frameIndex) { RequestFrame(frameIndex, {}); }
    void RequestFrame(int64_t frameIndex, const ReadPlan& plan);
    // Whether the editor is playing. At rest the worker fills only
    // kRestingFrames on each side of what was asked -- enough to step frame
    // by frame -- instead of the prefetch window playback needs: idle on one
    // 4K rush, that window held 42 frames, 1.4 GB, for a picture nobody was
    // moving (25 September 2026).
    void SetPlaying(bool playing);
    static constexpr size_t kRestingFrames = 4;
    // PERF16 -- whether playback also fills the frames behind the one
    // asked for, as before PERF16 (read_ahead::BehindFill). Off; a play
    // probe turns it on to compare on one binary.
    static void SetPlaybackFillsBehind(bool fills);

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    int32_t FrameRateNumerator() const;
    int32_t FrameRateDenominator() const;

private:
    void Run();
    bool DecodeAt(int64_t frameIndex, uint64_t generation);
    bool FillAscending(int64_t firstFrame, int64_t lastFrame,
                       uint64_t generation);
    bool FillReverseAhead(int64_t firstFrame, int64_t lastFrame,
                          uint64_t generation);
    bool RequestChanged(uint64_t generation);
    bool IsStopping();

    const FrameCache::SourceId sourceId_;
    std::string path_;
    FrameCache& cache_;
    PerformanceMetrics& metrics_;
    MediaSource source_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wakeup_;
    bool started_ = false;
    bool stopping_ = false;
    std::atomic<bool> playing_{false};
    uint64_t requestGeneration_ = 0;
    int64_t requestedFrame_ = 0;
    ReadPlan requestedPlan_;
    // What this worker has asked the system to read, in its own thread.
    read_ahead::Advice advised_;
    int64_t previousRequestedFrame_ = -1;
    int candidateDirection_ = 0;
    int candidateDirectionSamples_ = 0;
    int stableDirection_ = 1;
    int64_t nextSequentialFrame_ = -1;
    bool registeredSource_ = false;
    bool requestWasHit_ = false;
    std::chrono::steady_clock::time_point requestStartedAt_;
    std::chrono::steady_clock::time_point previousRequestAt_;
    bool hasPreviousRequestTime_ = false;
};
