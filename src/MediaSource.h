#pragma once

#include "ReadAhead.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVFrame;

class MediaSource {
public:
    MediaSource();
    ~MediaSource();

    // A negative threadType lets the stream choose: slice threading on every
    // core for an intra-only stream, else slice or frame threading on
    // threadCount threads (PERF10).
    bool Open(const std::string& path, int threadCount = 0,
              int threadType = -1);
    bool DecodeFrame(int64_t frameIndex, const AVFrame*& outFrame,
                     int64_t& outPts);
    bool DecodeNextFrame(const AVFrame*& outFrame, int64_t& outPts);

    // FF_THREAD_FRAME, FF_THREAD_SLICE or 0: the threading the decoder
    // actually runs, which Open picks for the stream unless told otherwise.
    int ActiveThreadType() const;

    int Width() const;
    int Height() const;
    int64_t FrameCount() const;
    int32_t FrameRateNumerator() const;
    int32_t FrameRateDenominator() const;
    int64_t FrameIndexForPts(int64_t pts) const;

    // PERF14 -- docs/roadmap/PERF-2026-09.md. Asks the system to read into
    // its file cache, without waiting, the bytes that hold frames
    // `firstFrame` to `lastFrame` (F_RDADVISE): from the first one's sample
    // to the end of the last one's, by the container's index, with whatever
    // is interleaved between. The decoder then reads them from memory. False
    // when the index does not place them, or the system refuses.
    bool AdviseRead(int64_t firstFrame, int64_t lastFrame);
    // Bytes asked for so far by every MediaSource of this process: what a
    // play probe reports.
    static int64_t AdvisedBytes();

    // PERF15 -- what every MediaSource of this process has read so far, to
    // tell the bytes a picture needs from the ones read around it: what the
    // demuxer pulled from the files (AVIOContext), in the video packets it
    // handed over and in the others it dropped, what opening the files
    // cost, the seeks, and the pictures decoded. A play probe reports them.
    struct ReadCounters {
        int64_t demuxed_bytes = 0;
        int64_t video_packet_bytes = 0;
        int64_t other_packet_bytes = 0;
        int64_t open_bytes = 0;
        int64_t seeks = 0;
        int64_t decoded_frames = 0;
    };
    static ReadCounters Counters();

    // PERF15 -- whether the system may read a decoder's file ahead of what
    // it asks for. Off by default: a decoder reads its file through its own
    // descriptor with the kernel's speculative read-ahead turned off
    // (F_RDAHEAD), and what is read ahead is what playback decides: the
    // playback reader in forward play (PlaybackReader.h), AdviseRead
    // otherwise (ReadAhead.h). Measured on 4 October 2026, 40 s of
    // PRJA_Camille on the external disk: the picture needs 1,137 MiB, the
    // decoders asked for 1,209, the disk read 1,387 -- 394 without PERF14's
    // advice --, the rest read ahead by macOS for nobody, on a disk that has
    // a quarter more than one stream to give. DaVinci Resolve read exactly
    // the 28.4 MiB/s the picture needs on the same disk, and played it at 25
    // images a second. Applies to sources opened after the call.
    static void SetKernelReadAhead(bool enabled);

    // PERF15 -- where each frame's picture lies in the file, read once from
    // the container's index when the source opens: what the playback reader
    // reads ahead of the decoder (PlaybackReader.h). Empty when the index
    // does not place every frame. Immutable, so any thread may read it.
    std::shared_ptr<const std::vector<read_ahead::SampleRange>> SampleTable()
        const;

private:
    struct Impl;
    Impl* impl_;
};
