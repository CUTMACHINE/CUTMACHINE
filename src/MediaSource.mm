#include "MediaSource.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>

namespace {

std::atomic<int64_t> gAdvisedBytes{0};
// PERF15 -- MediaSource::Counters.
std::atomic<int64_t> gDemuxedBytes{0};
std::atomic<int64_t> gVideoPacketBytes{0};
std::atomic<int64_t> gOtherPacketBytes{0};
std::atomic<int64_t> gOpenBytes{0};
std::atomic<int64_t> gSeeks{0};
std::atomic<int64_t> gDecodedFrames{0};
std::atomic<bool> gKernelReadAhead{false};

// PERF15 -- a decoder's file, read through a descriptor of its own.
struct FileIo {
    int descriptor = -1;
    int64_t size = 0;
};

int ReadFileIo(void* opaque, uint8_t* buffer, int size) {
    auto* io = static_cast<FileIo*>(opaque);
    const ssize_t got = read(io->descriptor, buffer, static_cast<size_t>(size));
    if (got < 0) return AVERROR(errno);
    if (got == 0) return AVERROR_EOF;
    return static_cast<int>(got);
}

int64_t SeekFileIo(void* opaque, int64_t offset, int whence) {
    auto* io = static_cast<FileIo*>(opaque);
    if (whence & AVSEEK_SIZE) return io->size;
    const off_t position = lseek(io->descriptor, static_cast<off_t>(offset),
                                 whence & ~AVSEEK_FORCE);
    return position < 0 ? AVERROR(errno) : static_cast<int64_t>(position);
}

// What the demuxer reads at once below a packet's size; a video packet
// larger than this is read straight into the packet, in one read.
constexpr int kFileIoBufferSize = 256 * 1024;

void LogAvError(const char* operation, int error) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    std::fprintf(stderr, "%s failed: %s\n", operation, text);
}

}  // namespace

struct MediaSource::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* planarAlphaFrame = nullptr;
    SwsContext* alphaConversion = nullptr;
    AVRational frameRate = {0, 1};
    int64_t frameCount = 0;
    bool draining = false;
    // PERF14 -- the file AdviseRead advises on, opened on first use.
    std::string path;
    int adviceDescriptor = -1;
    // PERF15 -- the AVIOContext's bytes_read already counted.
    int64_t countedBytes = 0;
    // PERF15 -- the file read through its own descriptor, when it is one.
    FileIo fileIo;
    AVIOContext* io = nullptr;
    // PERF15 -- MediaSource::SampleTable.
    std::shared_ptr<const std::vector<read_ahead::SampleRange>> samples;
};

MediaSource::MediaSource() : impl_(new Impl()) {}

MediaSource::~MediaSource() {
    sws_freeContext(impl_->alphaConversion);
    av_frame_free(&impl_->planarAlphaFrame);
    av_frame_free(&impl_->frame);
    av_packet_free(&impl_->packet);
    avcodec_free_context(&impl_->decoder);
    avformat_close_input(&impl_->format);
    if (impl_->io != nullptr) {
        av_freep(&impl_->io->buffer);
        avio_context_free(&impl_->io);
    }
    if (impl_->fileIo.descriptor >= 0) close(impl_->fileIo.descriptor);
    if (impl_->adviceDescriptor >= 0) close(impl_->adviceDescriptor);
    delete impl_;
}

bool MediaSource::Open(const std::string& path, int threadCount,
                       int threadType) {
    impl_->path = path;
    // PERF15 -- a regular file is read through a descriptor of its own,
    // the kernel's read-ahead off (SetKernelReadAhead). Anything else -- a
    // pattern of images, a URL -- is FFmpeg's to open.
    struct stat status {};
    if (stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode)) {
        const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor >= 0) {
            if (!gKernelReadAhead.load()) fcntl(descriptor, F_RDAHEAD, 0);
            impl_->fileIo = {descriptor, static_cast<int64_t>(status.st_size)};
            auto* buffer =
                static_cast<unsigned char*>(av_malloc(kFileIoBufferSize));
            impl_->io = buffer == nullptr
                            ? nullptr
                            : avio_alloc_context(buffer, kFileIoBufferSize, 0,
                                                 &impl_->fileIo, ReadFileIo,
                                                 nullptr, SeekFileIo);
            if (impl_->io == nullptr) av_free(buffer);
            impl_->format = impl_->io ? avformat_alloc_context() : nullptr;
            if (impl_->format != nullptr) {
                impl_->format->pb = impl_->io;
                impl_->format->flags |= AVFMT_FLAG_CUSTOM_IO;
            }
        }
    }
    int result =
        avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        LogAvError("avformat_open_input", result);
        return false;
    }

    result = avformat_find_stream_info(impl_->format, nullptr);
    if (result < 0) {
        LogAvError("avformat_find_stream_info", result);
        return false;
    }

    result = av_find_best_stream(impl_->format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                 nullptr, 0);
    if (result < 0) {
        LogAvError("av_find_best_stream(video)", result);
        return false;
    }
    impl_->streamIndex = result;
    impl_->stream = impl_->format->streams[impl_->streamIndex];

    const AVCodec* codec =
        avcodec_find_decoder(impl_->stream->codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "No FFmpeg decoder for codec id %d\n",
                     impl_->stream->codecpar->codec_id);
        return false;
    }

    impl_->decoder = avcodec_alloc_context3(codec);
    if (!impl_->decoder) {
        std::fprintf(stderr, "avcodec_alloc_context3 failed\n");
        return false;
    }
    result =
        avcodec_parameters_to_context(impl_->decoder, impl_->stream->codecpar);
    if (result < 0) {
        LogAvError("avcodec_parameters_to_context", result);
        return false;
    }

    // Required for software decoding performance. These must be set before
    // open2.
    //
    // PERF10 -- docs/roadmap/PERF-2026-09.md. An intra-only stream decodes
    // each picture on its own, so slice threading parallelises it inside a
    // single context. Frame threading gives every thread its own context and
    // pictures in flight: on a 4K XAVC-I rush (H.264 High 4:2:2 Intra, 10
    // bits) five frame threads held 269 MB and took 32 ms to seek, against
    // 88 MB and 6 ms for slice threading on every core, at the same 180
    // frames per second. Long-GOP streams usually carry one slice per
    // picture and keep frame threading.
    const AVCodecDescriptor* descriptor =
        avcodec_descriptor_get(impl_->stream->codecpar->codec_id);
    const bool intraOnly =
        (descriptor && (descriptor->props & AV_CODEC_PROP_INTRA_ONLY)) ||
        (impl_->stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
         impl_->stream->codecpar->profile != AV_PROFILE_UNKNOWN &&
         (impl_->stream->codecpar->profile & AV_PROFILE_H264_INTRA));
    if (threadType < 0 && intraOnly &&
        (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)) {
        impl_->decoder->thread_count = 0;
        impl_->decoder->thread_type = FF_THREAD_SLICE;
    } else {
        impl_->decoder->thread_count = threadCount;
        impl_->decoder->thread_type =
            threadType < 0 ? FF_THREAD_SLICE | FF_THREAD_FRAME : threadType;
    }

    result = avcodec_open2(impl_->decoder, codec, nullptr);
    if (result < 0) {
        LogAvError("avcodec_open2", result);
        return false;
    }
    std::fprintf(stderr,
                 "Decoder threading: count=%d requested=0x%x active=0x%x\n",
                 impl_->decoder->thread_count, impl_->decoder->thread_type,
                 impl_->decoder->active_thread_type);

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    impl_->planarAlphaFrame = av_frame_alloc();
    if (!impl_->packet || !impl_->frame || !impl_->planarAlphaFrame) {
        std::fprintf(stderr, "Unable to allocate FFmpeg packet/frame\n");
        return false;
    }

    // Ingest persists avg_frame_rate as the exact source time basis. Using
    // av_guess_frame_rate here can replace it with a nominal rate for VFR
    // phone footage, making the same media fail its own reopen check.
    impl_->frameRate = impl_->stream->avg_frame_rate;
    if (impl_->frameRate.num <= 0 || impl_->frameRate.den <= 0) {
        std::fprintf(stderr, "Unable to determine video frame rate\n");
        return false;
    }

    if (impl_->stream->nb_frames > 0) {
        impl_->frameCount = impl_->stream->nb_frames;
    } else {
        int64_t duration = 0;
        AVRational durationTimeBase = {0, 1};
        if (impl_->stream->duration != AV_NOPTS_VALUE) {
            duration = impl_->stream->duration;
            durationTimeBase = impl_->stream->time_base;
        } else if (impl_->format->duration != AV_NOPTS_VALUE) {
            duration = impl_->format->duration;
            durationTimeBase = AVRational{1, AV_TIME_BASE};
        }
        impl_->frameCount =
            duration > 0 && durationTimeBase.num > 0
                ? std::max<int64_t>(1,
                                    av_rescale_q_rnd(duration, durationTimeBase,
                                                     av_inv_q(impl_->frameRate),
                                                     AV_ROUND_NEAR_INF))
                : 1;
    }

    if (impl_->format->pb != nullptr) {
        impl_->countedBytes = impl_->format->pb->bytes_read;
        gOpenBytes.fetch_add(impl_->countedBytes);
    }
    // PERF15 -- each frame's sample, by the index, as AdviseRead finds it.
    {
        auto table = std::make_shared<std::vector<read_ahead::SampleRange>>();
        table->reserve(static_cast<size_t>(impl_->frameCount));
        for (int64_t frame = 0; frame < impl_->frameCount; ++frame) {
            int64_t pts = av_rescale_q(frame, av_inv_q(impl_->frameRate),
                                       impl_->stream->time_base);
            if (impl_->stream->start_time != AV_NOPTS_VALUE)
                pts += impl_->stream->start_time;
            const int index = av_index_search_timestamp(
                impl_->stream, pts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            const AVIndexEntry* entry =
                index < 0 ? nullptr
                          : avformat_index_get_entry(impl_->stream, index);
            if (entry == nullptr || entry->pos < 0 || entry->size <= 0) {
                table->clear();
                break;
            }
            table->push_back({entry->pos, entry->size});
        }
        impl_->samples = std::move(table);
    }
    std::fprintf(stderr,
                 "Opened %dx%d, %d/%d fps, %lld frames (software decode)\n",
                 Width(), Height(), impl_->frameRate.num, impl_->frameRate.den,
                 static_cast<long long>(impl_->frameCount));
    return true;
}

bool MediaSource::DecodeFrame(int64_t frameIndex, const AVFrame*& outFrame,
                              int64_t& outPts) {
    outFrame = nullptr;
    outPts = AV_NOPTS_VALUE;
    if (!impl_->format || !impl_->decoder || !impl_->stream || !impl_->packet ||
        !impl_->frame) {
        return false;
    }

    frameIndex = std::clamp<int64_t>(
        frameIndex, 0, std::max<int64_t>(0, impl_->frameCount - 1));
    int64_t targetPts = av_rescale_q(frameIndex, av_inv_q(impl_->frameRate),
                                     impl_->stream->time_base);
    if (impl_->stream->start_time != AV_NOPTS_VALUE) {
        targetPts += impl_->stream->start_time;
    }

    gSeeks.fetch_add(1);
    int result = av_seek_frame(impl_->format, impl_->streamIndex, targetPts,
                               AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        LogAvError("av_seek_frame", result);
        return false;
    }
    avcodec_flush_buffers(impl_->decoder);
    av_frame_unref(impl_->frame);
    impl_->draining = false;

    while (DecodeNextFrame(outFrame, outPts)) {
        if (outPts == AV_NOPTS_VALUE || outPts >= targetPts) {
            return true;
        }
    }
    return false;
}

bool MediaSource::DecodeNextFrame(const AVFrame*& outFrame, int64_t& outPts) {
    outFrame = nullptr;
    outPts = AV_NOPTS_VALUE;
    if (!impl_->format || !impl_->decoder || !impl_->packet || !impl_->frame) {
        return false;
    }

    av_frame_unref(impl_->frame);
    while (true) {
        int result = avcodec_receive_frame(impl_->decoder, impl_->frame);
        if (result >= 0) {
            // Some containers signal colour only at stream level. Preserve it
            // on every cached frame so the renderer never has to rediscover or
            // guess metadata that FFmpeg already parsed.
            const AVCodecParameters* parameters = impl_->stream->codecpar;
            if (impl_->frame->color_range == AVCOL_RANGE_UNSPECIFIED)
                impl_->frame->color_range = parameters->color_range;
            if (impl_->frame->colorspace == AVCOL_SPC_UNSPECIFIED)
                impl_->frame->colorspace = parameters->color_space;
            if (impl_->frame->color_trc == AVCOL_TRC_UNSPECIFIED)
                impl_->frame->color_trc = parameters->color_trc;
            if (impl_->frame->color_primaries == AVCOL_PRI_UNSPECIFIED)
                impl_->frame->color_primaries = parameters->color_primaries;
            gDecodedFrames.fetch_add(1);
            outPts = impl_->frame->best_effort_timestamp;
            if (outPts == AV_NOPTS_VALUE) {
                outPts = impl_->frame->pts;
            }
            const AVPixFmtDescriptor* pixel = av_pix_fmt_desc_get(
                static_cast<AVPixelFormat>(impl_->frame->format));
            const bool hasAlpha = pixel && pixel->nb_components >= 4 &&
                                  (pixel->flags & AV_PIX_FMT_FLAG_ALPHA);
            const bool planar = hasAlpha &&
                                pixel->comp[0].plane != pixel->comp[1].plane &&
                                pixel->comp[0].plane != pixel->comp[2].plane &&
                                pixel->comp[1].plane != pixel->comp[2].plane &&
                                pixel->comp[3].plane != pixel->comp[0].plane &&
                                pixel->comp[3].plane != pixel->comp[1].plane &&
                                pixel->comp[3].plane != pixel->comp[2].plane;
            if (hasAlpha && !planar) {
                // ALPHA-2026-08 -- Metal consumes separate component planes.
                // Packed PNG/WebP frames are normalized to planar GBR+A here;
                // RGB values remain untransformed until the project's OCIO
                // input stage and the alpha plane stays independent.
                av_frame_unref(impl_->planarAlphaFrame);
                impl_->planarAlphaFrame->format = AV_PIX_FMT_GBRAP16LE;
                impl_->planarAlphaFrame->width = impl_->frame->width;
                impl_->planarAlphaFrame->height = impl_->frame->height;
                if (av_frame_get_buffer(impl_->planarAlphaFrame, 64) < 0) {
                    std::fprintf(stderr,
                                 "Unable to allocate planar alpha frame\n");
                    return false;
                }
                impl_->alphaConversion = sws_getCachedContext(
                    impl_->alphaConversion, impl_->frame->width,
                    impl_->frame->height,
                    static_cast<AVPixelFormat>(impl_->frame->format),
                    impl_->frame->width, impl_->frame->height,
                    AV_PIX_FMT_GBRAP16LE, SWS_POINT, nullptr, nullptr, nullptr);
                if (!impl_->alphaConversion ||
                    sws_scale(impl_->alphaConversion, impl_->frame->data,
                              impl_->frame->linesize, 0, impl_->frame->height,
                              impl_->planarAlphaFrame->data,
                              impl_->planarAlphaFrame->linesize) !=
                        impl_->frame->height ||
                    av_frame_copy_props(impl_->planarAlphaFrame, impl_->frame) <
                        0) {
                    std::fprintf(stderr,
                                 "Unable to normalize packed alpha frame\n");
                    return false;
                }
                outFrame = impl_->planarAlphaFrame;
            } else {
                outFrame = impl_->frame;
            }
            return true;
        }
        if (result == AVERROR_EOF) {
            return false;
        }
        if (result != AVERROR(EAGAIN)) {
            LogAvError("avcodec_receive_frame", result);
            return false;
        }

        if (impl_->draining) {
            return false;
        }

        while (true) {
            result = av_read_frame(impl_->format, impl_->packet);
            if (impl_->format->pb != nullptr) {
                const int64_t pulled = impl_->format->pb->bytes_read;
                gDemuxedBytes.fetch_add(pulled - impl_->countedBytes);
                impl_->countedBytes = pulled;
            }
            if (result >= 0)
                (impl_->packet->stream_index == impl_->streamIndex
                     ? gVideoPacketBytes
                     : gOtherPacketBytes)
                    .fetch_add(impl_->packet->size);
            if (result < 0) {
                impl_->draining = true;
                result = avcodec_send_packet(impl_->decoder, nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    LogAvError("avcodec_send_packet(drain)", result);
                    return false;
                }
                break;
            }
            if (impl_->packet->stream_index != impl_->streamIndex) {
                av_packet_unref(impl_->packet);
                continue;
            }
            result = avcodec_send_packet(impl_->decoder, impl_->packet);
            av_packet_unref(impl_->packet);
            if (result < 0) {
                LogAvError("avcodec_send_packet", result);
                return false;
            }
            break;
        }
    }
}

int MediaSource::ActiveThreadType() const {
    return impl_->decoder ? impl_->decoder->active_thread_type : 0;
}

int MediaSource::Width() const {
    return impl_->decoder ? impl_->decoder->width : 0;
}
int MediaSource::Height() const {
    return impl_->decoder ? impl_->decoder->height : 0;
}
int64_t MediaSource::FrameCount() const { return impl_->frameCount; }
int32_t MediaSource::FrameRateNumerator() const { return impl_->frameRate.num; }
int32_t MediaSource::FrameRateDenominator() const {
    return impl_->frameRate.den;
}
int64_t MediaSource::FrameIndexForPts(int64_t pts) const {
    if (!impl_->stream || pts == AV_NOPTS_VALUE) {
        return -1;
    }
    if (impl_->stream->start_time != AV_NOPTS_VALUE) {
        pts -= impl_->stream->start_time;
    }
    return av_rescale_q_rnd(
        pts, impl_->stream->time_base, av_inv_q(impl_->frameRate),
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

bool MediaSource::AdviseRead(int64_t firstFrame, int64_t lastFrame) {
    if (!impl_->format || !impl_->stream || impl_->frameCount <= 0 ||
        lastFrame < firstFrame)
        return false;
    const auto entryOf = [&](int64_t frame) -> const AVIndexEntry* {
        frame = std::clamp<int64_t>(frame, 0, impl_->frameCount - 1);
        int64_t pts = av_rescale_q(frame, av_inv_q(impl_->frameRate),
                                   impl_->stream->time_base);
        if (impl_->stream->start_time != AV_NOPTS_VALUE)
            pts += impl_->stream->start_time;
        const int index = av_index_search_timestamp(
            impl_->stream, pts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        return index < 0 ? nullptr
                         : avformat_index_get_entry(impl_->stream, index);
    };
    const AVIndexEntry* first = entryOf(firstFrame);
    const AVIndexEntry* last = entryOf(lastFrame);
    if (!first || !last || first->pos < 0 || last->pos < 0) return false;
    const int64_t begin = std::min(first->pos, last->pos);
    const int64_t end =
        std::max(first->pos, last->pos) + std::max(0, last->size);
    if (end <= begin) return false;
    if (impl_->adviceDescriptor < 0)
        impl_->adviceDescriptor =
            open(impl_->path.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl_->adviceDescriptor < 0) return false;
    radvisory advice{};
    advice.ra_offset = begin;
    advice.ra_count = static_cast<int>(std::min<int64_t>(end - begin, INT_MAX));
    if (fcntl(impl_->adviceDescriptor, F_RDADVISE, &advice) == -1) return false;
    gAdvisedBytes.fetch_add(advice.ra_count);
    return true;
}

int64_t MediaSource::AdvisedBytes() { return gAdvisedBytes.load(); }

MediaSource::ReadCounters MediaSource::Counters() {
    ReadCounters counters;
    counters.demuxed_bytes = gDemuxedBytes.load();
    counters.video_packet_bytes = gVideoPacketBytes.load();
    counters.other_packet_bytes = gOtherPacketBytes.load();
    counters.open_bytes = gOpenBytes.load();
    counters.seeks = gSeeks.load();
    counters.decoded_frames = gDecodedFrames.load();
    return counters;
}

void MediaSource::SetKernelReadAhead(bool enabled) {
    gKernelReadAhead.store(enabled);
}

std::shared_ptr<const std::vector<read_ahead::SampleRange>>
MediaSource::SampleTable() const {
    return impl_->samples;
}
