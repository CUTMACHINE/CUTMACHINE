#pragma once

// PERF8 -- ROADMAP.md. Counts of the work PERF-2026-09 stopped repeating:
// parsing the whole package, probing a rush with FFmpeg, hashing a rush,
// committing a package file with a full fsync.
//
// A benchmark bounds these counts rather than milliseconds. A duration moves
// with whatever else the machine is doing, and a CI runner under a sanitizer
// shares nothing with the external drive projects are cut from; a count does
// not move, so a test can assert that a gain is kept (see
// tests/load_benchmark_tests.cc and docs/PERF_BENCHMARK.md).
//
// Always on, and read by nothing in the product. Each increment is one
// relaxed atomic add next to work that costs from about a hundred
// microseconds (parsing a package held in the page cache) to hundreds of
// milliseconds (a probe or an F_FULLFSYNC on a USB volume), so an "off"
// switch would cost as much as the count it saves. Atomic because the editor
// probes legacy media from several threads at once (main.mm,
// ParallelForEachIndex).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace work_counters {

enum class Work : uint8_t {
    // LoadStoredProject: the project file and every Timelines/*.json,
    // parsed and validated.
    kPackageLoad,
    // ProbeMediaMetadata: avformat_open_input + avformat_find_stream_info
    // on a rush.
    kMediaProbe,
    // VerifyPortableProject: SHA-256 of a rush copied into a collected
    // package, read end to end.
    kMediaHash,
    // CommitArtifacts (Cli.cc): a file rewritten or removed under the commit
    // transaction, each paying its own F_FULLFSYNC and directory syncs --
    // package files, and whatever else goes through the same transaction.
    // Files whose bytes are already on disk are not counted: PERF6 skips
    // them.
    kArtifactCommit,
    // PERF11 -- VideoReader::Seek (MotionMedia.cc): a composition's video
    // repositioned, which decodes again from the key frame before the picture
    // asked for. Reading forward costs none.
    kMotionVideoSeek,
    // PERF11 -- ConvertMotionVideoFrame: one decoded picture turned into
    // RGBA, the whole picture whatever the size it is drawn at.
    kMotionVideoConvert,
    kCount,
};

inline std::array<std::atomic<uint64_t>, static_cast<size_t>(Work::kCount)>
    gCounts{};

inline void Count(Work work, uint64_t amount = 1) {
    gCounts[static_cast<size_t>(work)].fetch_add(amount,
                                                 std::memory_order_relaxed);
}

struct Tally {
    uint64_t package_loads = 0;
    uint64_t media_probes = 0;
    uint64_t media_hashes = 0;
    uint64_t artifact_commits = 0;
    uint64_t motion_video_seeks = 0;
    uint64_t motion_video_converts = 0;
};

inline Tally Read() {
    const auto load = [](Work work) {
        return gCounts[static_cast<size_t>(work)].load(
            std::memory_order_relaxed);
    };
    return {load(Work::kPackageLoad),     load(Work::kMediaProbe),
            load(Work::kMediaHash),       load(Work::kArtifactCommit),
            load(Work::kMotionVideoSeek), load(Work::kMotionVideoConvert)};
}

// The work done since `before` was read.
inline Tally Since(const Tally& before) {
    const Tally now = Read();
    return {now.package_loads - before.package_loads,
            now.media_probes - before.media_probes,
            now.media_hashes - before.media_hashes,
            now.artifact_commits - before.artifact_commits,
            now.motion_video_seeks - before.motion_video_seeks,
            now.motion_video_converts - before.motion_video_converts};
}

}  // namespace work_counters
