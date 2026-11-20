// FrameCache ownership (PERF10 -- docs/roadmap/PERF-2026-09.md). Plain C++
// against libavutil, no AppKit, so this runs on a plain Linux host.
//
// What it guards: a cached picture owns its bytes. Holding a reference to the
// decoder's buffer instead pinned that buffer in the decoder's pool, which
// never frees while the decoder is open, so evicting a picture freed nothing
// and the cache's byte budget bounded nothing -- 8.6 GB with six 4K decoders
// open while playing a reel.

#include "FrameCache.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>

#include "TestSupport.h"

namespace {

// A picture whose planes live in one buffer drawn from `pool`, the way a
// decoder hands them out.
AVFrame* PooledFrame(AVBufferPool* pool, int width, int height, uint8_t shade) {
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    frame->buf[0] = av_buffer_pool_get(pool);
    uint8_t* data = frame->buf[0]->data;
    frame->data[0] = data;
    frame->data[1] = data + static_cast<ptrdiff_t>(width) * height;
    frame->data[2] =
        frame->data[1] + static_cast<ptrdiff_t>(width / 2) * (height / 2);
    frame->linesize[0] = width;
    frame->linesize[1] = width / 2;
    frame->linesize[2] = width / 2;
    std::memset(data, shade, frame->buf[0]->size);
    return frame;
}

}  // namespace

int main() {
    constexpr int kWidth = 64;
    constexpr int kHeight = 32;
    constexpr size_t kPictureBytes = kWidth * kHeight * 3 / 2;
    AVBufferPool* pool = av_buffer_pool_init(kPictureBytes, nullptr);

    {
        FrameCache cache(kPictureBytes * 64);
        AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x5a);
        Check(cache.Put("A", 7, decoded), "a pooled picture is cached");
        Check(av_buffer_get_ref_count(decoded->buf[0]) == 1,
              "the cache holds no reference to the decoder's buffer");
        AVFrame* cached = cache.GetExact("A", 7);
        Check(cached && cached->data[0] != decoded->data[0] &&
                  cached->width == kWidth && cached->height == kHeight &&
                  cached->format == AV_PIX_FMT_YUV420P,
              "the cached picture has planes of its own");
        std::memset(decoded->buf[0]->data, 0x11, decoded->buf[0]->size);
        bool same = cached != nullptr;
        for (int plane = 0; same && plane < 3; ++plane) {
            const int rows = plane == 0 ? kHeight : kHeight / 2;
            const int bytes = plane == 0 ? kWidth : kWidth / 2;
            for (int row = 0; same && row < rows; ++row)
                for (int column = 0; same && column < bytes; ++column)
                    same = cached->data[plane][row * cached->linesize[plane] +
                                               column] == 0x5a;
        }
        Check(same,
              "the decoder reusing its buffer leaves the cached picture as "
              "it was decoded");
        av_frame_free(&cached);

        const size_t before = cache.TotalBytes();
        AVFrame* again = PooledFrame(pool, kWidth, kHeight, 0x33);
        Check(cache.Put("A", 7, again) && cache.TotalBytes() == before &&
                  cache.EntryCount() == 1,
              "putting a picture the cache holds adds nothing");
        AVFrame* kept = cache.GetExact("A", 7);
        Check(kept && kept->data[0][0] == 0x5a, "and keeps the picture it had");
        av_frame_free(&kept);
        av_frame_free(&again);
        av_frame_free(&decoded);
    }

    {
        // The budget now bounds what the pictures weigh: a decoder putting a
        // hundred pictures through a four-picture cache keeps only the
        // buffers it is decoding into.
        FrameCache cache(kPictureBytes * 4 + kPictureBytes / 2);
        bool pinned = false;
        for (int index = 0; index < 100; ++index) {
            AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x40);
            cache.Put("B", index, decoded);
            pinned |= av_buffer_get_ref_count(decoded->buf[0]) != 1;
            av_frame_free(&decoded);
        }
        Check(!pinned, "no cached picture pins a pool buffer");
        Check(
            cache.EntryCount() <= 4 && cache.TotalBytes() <= cache.ByteBudget(),
            "the cache stays within its budget");
    }

    {
        // Once playback stops the editor trims the cache: the least recently
        // shown pictures go, the ones at the playhead stay.
        FrameCache cache(kPictureBytes * 64);
        for (int index = 0; index < 10; ++index) {
            AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x20);
            cache.Put("C", index, decoded);
            av_frame_free(&decoded);
        }
        cache.TrimTo(kPictureBytes * 3 + kPictureBytes / 2);
        Check(cache.EntryCount() == 3 && cache.Contains("C", 9) &&
                  cache.Contains("C", 7) && !cache.Contains("C", 6),
              "trimming keeps the most recently cached pictures");
    }

    {
        // PERF14 -- the first pictures of a shot asked for ahead of its cut
        // stay while another source's pictures stream through the budget,
        // once the request is repeated; without it they go first.
        for (const bool refreshed : {false, true}) {
            FrameCache cache(kPictureBytes * 6 + kPictureBytes / 2);
            for (int index = 100; index < 103; ++index) {
                AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x40);
                cache.Put("V1", index, decoded);
                av_frame_free(&decoded);
            }
            for (int index = 0; index < 6; ++index) {
                if (refreshed) cache.TouchRun("V1", 100, 1, 8);
                AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x50);
                cache.Put("V2", index, decoded);
                av_frame_free(&decoded);
            }
            const bool kept = cache.Contains("V1", 100) &&
                              cache.Contains("V1", 101) &&
                              cache.Contains("V1", 102);
            Check(refreshed ? kept : !cache.Contains("V1", 100),
                  refreshed ? "a run refreshed stays while the cutaway "
                              "streams"
                            : "without the refresh it is evicted first");
            if (refreshed)
                Check(!cache.Contains("V2", 0) && cache.Contains("V2", 5),
                      "and the cutaway's pictures already shown go instead");
        }
    }

    {
        // PERF13 -- whether the monitor has a picture of a source to draw,
        // and its size, asked at every position played: answered without a
        // copy, and without keeping the picture from eviction.
        FrameCache cache(kPictureBytes * 2 + kPictureBytes / 2);
        int width = 0;
        int height = 0;
        bool alpha = true;
        Check(!cache.DescribeNearest("D", 0, width, height, alpha),
              "a source without a picture is described as such");
        for (int index : {0, 1}) {
            AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x30);
            cache.Put("D", index, decoded);
            av_frame_free(&decoded);
        }
        Check(cache.DescribeNearest("D", 40, width, height, alpha) &&
                  width == kWidth && height == kHeight && !alpha,
              "the picture GetNearest would hand back is described");
        Check(cache.DescribeNearest("D", 0, width, height, alpha),
              "the oldest picture is described too");
        AVFrame* decoded = PooledFrame(pool, kWidth, kHeight, 0x30);
        cache.Put("D", 2, decoded);
        av_frame_free(&decoded);
        Check(!cache.Contains("D", 0) && cache.Contains("D", 1) &&
                  cache.Contains("D", 2),
              "describing a picture leaves the eviction order alone");

        // A picture with an alpha plane, as a ProRes 4444 or a PNG decodes.
        FrameCache roomy(kPictureBytes * 64);
        AVFrame* keyed = av_frame_alloc();
        keyed->format = AV_PIX_FMT_YUVA420P;
        keyed->width = kWidth;
        keyed->height = kHeight;
        av_frame_get_buffer(keyed, 0);
        Check(roomy.Put("E", 0, keyed), "a picture with alpha is cached");
        av_frame_free(&keyed);
        Check(roomy.DescribeNearest("E", 0, width, height, alpha) && alpha,
              "a picture with an alpha plane is described as such");
    }

    av_buffer_pool_uninit(&pool);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "frame cache tests passed\n";
    return 0;
}
