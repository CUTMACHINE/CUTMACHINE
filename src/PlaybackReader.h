#pragma once

// PERF15 -- docs/roadmap/PERF-2026-09.md. One reader that reads, in the
// order the picture is shown, the bytes of the pictures playback is about
// to show, and nothing else.
//
// Measured on 4 October 2026, on the external disk of the projects (USB,
// exFAT through FSKit; 39.7 MiB/s for one reader, 23 for two at once) and
// the 73-second PRJA_Camille: its picture needs 28.4 MiB/s, every frame of
// it a 1.14 MiB sample. DaVinci Resolve played it there at 25 images a
// second reading 28.5 MiB/s, flat. CUTMACHINE read 34 to 38 and froze at
// cuts: its decoders asked for about what was needed, the system read a
// fifth more ahead of them for nobody, and each decoder read its own file
// in the thread that decodes, one packet at a time, so two of them -- the
// shot playing and the next -- shared the disk at a cut.
//
// Here a single thread reads ahead for every decoder: the frames the next
// seconds show (PlanReads), the video samples alone, the sound between them
// skipped (RunsFor), soonest first, one read at a time, into the system's
// file cache, where the decoders then find them. The decoders read with
// the system's own read-ahead off (MediaSource::SetKernelReadAhead).
//
// Measured the same day, cold, 40 s on that disk, one binary with the
// reader on and off: PRJA_Camille dropped 21 and 31 images instead of 421,
// SERIE_Camille 72 and 86 instead of 590. What it still dropped fell at cuts
// within one rush, decoding rather than reading (PERF16,
// read_ahead::BehindFill).
//
// The plan and the byte runs are plain C++ read from a document; the reader
// uses POSIX files and a thread, no AppKit and no FFmpeg. main.mm wires it.
// See tests/read_ahead_tests.cc.

#include "MonitorOcclusion.h"
#include "RationalTime.h"
#include "ReadAhead.h"
#include "Ulid.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class Document;
class Timeline;

namespace playback_reader {

// Source frames `first` to `last` of one source, shown from sequence frame
// `at` on, in that order.
struct PlannedRead {
    Ulid source;
    int64_t first = 0;
    int64_t last = 0;
    int64_t at = 0;

    bool operator==(const PlannedRead& other) const {
        return source == other.source && first == other.first &&
               last == other.last && at == other.at;
    }
};

// A run is cut every so many sequence frames, so that the reads of layers
// shown together -- a translucent title over an interview -- interleave in
// the order they are needed instead of one whole layer first.
inline constexpr int64_t kMaxPlannedRunFrames = 12;

// What the picture played forward from sequence frame `fromFrame` reads,
// for `frames` sequence frames: frame by frame, the layers the monitor
// composites there -- the visible video tracks a shot above does not hide
// (MonitorOcclusion.h, with `pictures`), motion clips left out, as no file
// is read for them --, merged into runs of source frames that follow on
// (a gap of up to 4 frames, which a decoder reading on reads through, is
// kept in the run), sorted by the sequence frame each run starts at, lower
// tracks first at one instant.
std::vector<PlannedRead> PlanReads(
    const Document& document, const Timeline& timeline, int64_t fromFrame,
    int64_t frames, const monitor_occlusion::PictureLookup& pictures,
    bool zoomed);

// A stretch of a file to read.
struct ByteRun {
    int64_t offset = 0;
    int64_t size = 0;

    bool operator==(const ByteRun& other) const {
        return offset == other.offset && size == other.size;
    }
};

// The stretches holding the samples of frames `first` to `last`: samples
// that touch are read as one, anything between them -- the sound
// interleaved with the picture -- is skipped, and no stretch is longer than
// `maxRun` bytes (a sample longer than that is one stretch on its own).
std::vector<ByteRun> RunsFor(
    const std::vector<read_ahead::SampleRange>& samples, int64_t first,
    int64_t last, int64_t maxRun);

// How far ahead of the frame on screen the reader reads, and the most it
// reads at once: three seconds of picture, in reads of at most 2 MiB, one
// at a time.
inline constexpr int64_t kReaderHorizonSeconds = 3;
inline constexpr int64_t kReaderMaxRunBytes = int64_t{2} * 1024 * 1024;

class Reader {
public:
    struct Stats {
        int64_t bytes = 0;
        int64_t reads = 0;
        int64_t plans = 0;
        // Sources a plan named with no sample table to read them by: their
        // decoders read them alone, as before.
        int64_t unplaced_sources = 0;
    };

    Reader();
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Where a source's pictures lie: its file and its sample table
    // (MediaSource::SampleTable). Kept until replaced; read by the thread.
    void SetSource(
        const Ulid& source, std::string path,
        std::shared_ptr<const std::vector<read_ahead::SampleRange>> samples);

    // Starts reading ahead of playback of `document`, a copy the reader
    // keeps, forward from `position`, `horizonSeconds` ahead. `threaded`
    // false starts no thread: Step does the work, for tests.
    void Start(std::unique_ptr<Document> document, bool zoomed,
               RationalTime position, int64_t horizonSeconds,
               bool threaded = true);
    // The playhead, as it moves.
    void SetPosition(RationalTime position);
    // Stops reading; what was read stays in the system's cache.
    void Stop();
    bool Running() const;
    // Whether the reader reads `source` when a plan names it: it has its
    // file and a sample table.
    bool Covers(const Ulid& source) const;

    // Plans if the playhead moved since the last plan, then reads the next
    // stretch not read yet. The bytes read; 0 when there is nothing to read
    // before the horizon.
    int64_t Step();

    Stats GetStats() const;

private:
    struct Read;
    void Run();
    bool NextRead(Read& read);
    int DescriptorFor(const Ulid& source);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool running_ = false;
    bool stopping_ = false;
    std::unique_ptr<Document> document_;
    std::unique_ptr<Timeline> timeline_;
    bool zoomed_ = false;
    int64_t horizonFrames_ = 0;
    int64_t positionFrame_ = 0;
    int64_t plannedFrom_ = -1;
    std::vector<PlannedRead> plan_;
    struct SourceFile {
        std::string path;
        std::shared_ptr<const std::vector<read_ahead::SampleRange>> samples;
        int descriptor = -1;
    };
    std::map<Ulid, SourceFile> sources_;
    std::map<Ulid, std::set<int64_t>> read_;
    std::set<Ulid> unplaced_;
    std::vector<unsigned char> buffer_;
    Stats stats_;
};

}  // namespace playback_reader
