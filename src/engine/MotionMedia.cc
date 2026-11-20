#include "MotionMedia.h"

#include "WorkCounters.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/version_major.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <utility>

namespace {

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

struct FormatCloser {
    void operator()(AVFormatContext* context) const {
        avformat_close_input(&context);
    }
};
struct CodecCloser {
    void operator()(AVCodecContext* context) const {
        avcodec_free_context(&context);
    }
};
struct FrameCloser {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct PacketCloser {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct ScaleCloser {
    void operator()(SwsContext* context) const { sws_freeContext(context); }
};
struct ResampleCloser {
    void operator()(SwrContext* context) const { swr_free(&context); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;

bool Open(const std::string& path, FormatPtr& format, std::string& code,
          std::string& error) {
    AVFormatContext* raw = nullptr;
    int result = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        code = "MediaMissing";
        error = "cannot open media '" + path + "': " + AvError(result);
        return false;
    }
    format.reset(raw);
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0) {
        code = "MediaMissing";
        error = "cannot read the streams of '" + path + "': " + AvError(result);
        return false;
    }
    return true;
}

// Clockwise rotation, as FFmpeg's own autorotate reads the display matrix.
int32_t DisplayRotation(const AVStream* stream) {
    const uint8_t* matrix = nullptr;
    size_t size = 0;
#if LIBAVFORMAT_VERSION_MAJOR >= 62
    const AVPacketSideData* side = av_packet_side_data_get(
        stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (side != nullptr) {
        matrix = side->data;
        size = side->size;
    }
#else
    matrix = av_stream_get_side_data(const_cast<AVStream*>(stream),
                                     AV_PKT_DATA_DISPLAYMATRIX, &size);
#endif
    if (matrix == nullptr || size < 9 * sizeof(int32_t)) {
        return 0;
    }
    const double counterclockwise =
        av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (std::isnan(counterclockwise)) {
        return 0;
    }
    long quarter = std::lround(-counterclockwise / 90.0) % 4;
    if (quarter < 0) {
        quarter += 4;
    }
    return static_cast<int32_t>(quarter * 90);
}

// The duration a timeline gives the same file (Ingest.cc): the stream's,
// or the container's, counted in units of 1/rate_numerator.
RationalTime StreamDuration(const AVFormatContext* format,
                            const AVStream* stream, int32_t unit) {
    int64_t value = 0;
    const auto rounding =
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        value = av_rescale_q_rnd(stream->duration, stream->time_base,
                                 AVRational{1, unit}, rounding);
    } else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        value = av_rescale_q_rnd(format->duration, AVRational{1, AV_TIME_BASE},
                                 AVRational{1, unit}, rounding);
    }
    return {std::max<int64_t>(0, value), unit};
}

bool ProbeOpen(const AVFormatContext* format, MotionMediaInfo& info) {
    info = MotionMediaInfo{};
    const int video =
        av_find_best_stream(const_cast<AVFormatContext*>(format),
                            AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio =
        av_find_best_stream(const_cast<AVFormatContext*>(format),
                            AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    // A still image read by FFmpeg is a video stream of one picture: it is
    // an image node's business, not a video's.
    if (video >= 0 && (format->streams[video]->disposition &
                       AV_DISPOSITION_ATTACHED_PIC) == 0) {
        const AVStream* stream = format->streams[video];
        const AVRational rate = stream->avg_frame_rate;
        if (rate.num > 0 && rate.den > 0 && stream->codecpar->width > 0 &&
            stream->codecpar->height > 0) {
            info.has_video = true;
            info.rate_numerator = rate.num;
            info.rate_denominator = rate.den;
            info.rotation = DisplayRotation(stream);
            const bool quarter = info.rotation == 90 || info.rotation == 270;
            info.width =
                quarter ? stream->codecpar->height : stream->codecpar->width;
            info.height =
                quarter ? stream->codecpar->width : stream->codecpar->height;
            info.duration = StreamDuration(format, stream, rate.num);
        }
    }
    if (audio >= 0) {
        const AVStream* stream = format->streams[audio];
        if (stream->codecpar->sample_rate > 0 &&
            stream->codecpar->ch_layout.nb_channels > 0) {
            info.has_audio = true;
            if (!info.has_video) {
                info.duration = StreamDuration(format, stream,
                                               stream->codecpar->sample_rate);
            }
        }
    }
    return info.has_video || info.has_audio;
}

int ScaleColorspace(const AVFrame* frame) {
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            return SWS_CS_ITU709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M:
            return SWS_CS_SMPTE240M;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return SWS_CS_BT2020;
        default:
            // Untagged: high definition is BT.709, standard definition
            // BT.601, the rule players follow.
            return frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

bool HasFormatFlag(int format, uint64_t flag) {
    const AVPixFmtDescriptor* descriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return descriptor != nullptr && (descriptor->flags & flag) != 0;
}

// An upright RGBA picture turned as its container displays it.
void Rotate(std::vector<uint8_t>& pixels, int32_t width, int32_t height,
            int32_t rotation, MotionVideoPicture& picture) {
    if (rotation == 0) {
        picture.width = width;
        picture.height = height;
        picture.rgba = std::move(pixels);
        return;
    }
    const bool quarter = rotation == 90 || rotation == 270;
    picture.width = quarter ? height : width;
    picture.height = quarter ? width : height;
    picture.rgba.assign(pixels.size(), 0);
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            int32_t tx = x;
            int32_t ty = y;
            if (rotation == 90) {
                tx = height - 1 - y;
                ty = x;
            } else if (rotation == 180) {
                tx = width - 1 - x;
                ty = height - 1 - y;
            } else {
                tx = y;
                ty = width - 1 - x;
            }
            std::copy_n(
                &pixels[(static_cast<size_t>(y) * width + x) * 4], 4,
                &picture
                     .rgba[(static_cast<size_t>(ty) * picture.width + tx) * 4]);
        }
    }
}

// One open video stream, decoding forward from where the last read left it.
class VideoReader {
public:
    bool Open(const std::string& path, std::string& code, std::string& error) {
        path_ = path;
        if (!::Open(path, format_, code, error)) {
            return false;
        }
        MotionMediaInfo info;
        if (!ProbeOpen(format_.get(), info) || !info.has_video) {
            code = "MediaHasNoVideo";
            error = "media '" + path + "' has no video stream";
            return false;
        }
        info_ = info;
        stream_ = av_find_best_stream(format_.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                      nullptr, 0);
        const AVStream* stream = format_->streams[stream_];
        const AVCodec* decoder =
            avcodec_find_decoder(stream->codecpar->codec_id);
        codec_.reset(decoder != nullptr ? avcodec_alloc_context3(decoder)
                                        : nullptr);
        if (!codec_ ||
            avcodec_parameters_to_context(codec_.get(), stream->codecpar) < 0 ||
            avcodec_open2(codec_.get(), decoder, nullptr) < 0) {
            code = "DecodeFailed";
            error = "no decoder for the video of '" + path + "'";
            return false;
        }
        packet_.reset(av_packet_alloc());
        current_.reset(av_frame_alloc());
        pending_.reset(av_frame_alloc());
        if (!packet_ || !current_ || !pending_) {
            code = "DecodeFailed";
            error = "cannot allocate a decoder for '" + path + "'";
            return false;
        }
        return true;
    }

    // PERF11 -- positions the decoder on the picture that `frame` shows and
    // hands out a copy of it. The conversion is the caller's, outside this
    // reader's lock.
    bool Read(int64_t frame, MotionDecodedPicture& decoded, std::string& code,
              std::string& error) {
        if (frame < 0) {
            frame = 0;
        }
        // Forward reads within two seconds decode on; anything else seeks.
        const int64_t reach = std::max<int64_t>(
            60, 2 * info_.rate_numerator / std::max(1, info_.rate_denominator));
        if (!has_current_ || frame < current_index_ ||
            frame > current_index_ + reach) {
            if (!Seek(frame, code, error)) {
                return false;
            }
        }
        while (!has_current_ || current_index_ < frame) {
            bool ended = false;
            if (!Advance(ended, code, error)) {
                return false;
            }
            if (ended) {
                break;
            }
            if (next_index_ > frame && has_current_) {
                // The next picture starts after the one asked for: the
                // current one still holds it (a variable cadence).
                break;
            }
            Promote();
        }
        if (!has_current_) {
            code = "DecodeFailed";
            error = "no picture decoded from '" + path_ + "'";
            return false;
        }
        // A copy, not a reference: the decoder's pool would keep a
        // referenced buffer for as long as the file stays open (PERF10).
        FramePtr copy(av_frame_alloc());
        if (copy) {
            copy->format = current_->format;
            copy->width = current_->width;
            copy->height = current_->height;
        }
        if (!copy || av_frame_get_buffer(copy.get(), 0) < 0 ||
            av_frame_copy(copy.get(), current_.get()) < 0 ||
            av_frame_copy_props(copy.get(), current_.get()) < 0) {
            code = "DecodeFailed";
            error = "cannot keep a picture of '" + path_ + "'";
            return false;
        }
        decoded.path = path_;
        decoded.frame = frame;
        decoded.rotation = info_.rotation;
        decoded.pixels = std::shared_ptr<void>(copy.release(), [](void* held) {
            auto* owned = static_cast<AVFrame*>(held);
            av_frame_free(&owned);
        });
        return true;
    }

private:
    int64_t IndexOf(const AVFrame* frame) const {
        const AVStream* stream = format_->streams[stream_];
        int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) {
            timestamp = frame->pts;
        }
        if (timestamp == AV_NOPTS_VALUE) {
            return current_index_ + 1;
        }
        const int64_t start =
            first_timestamp_ != AV_NOPTS_VALUE ? first_timestamp_ : timestamp;
        // Picture index = (pts - first) * time_base * rate, to the nearest:
        // a timestamp a tick off its grid still names its own picture.
        return av_rescale_q_rnd(
            timestamp - start, stream->time_base,
            AVRational{info_.rate_denominator, info_.rate_numerator},
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    }

    bool Seek(int64_t frame, std::string& code, std::string& error) {
        work_counters::Count(work_counters::Work::kMotionVideoSeek);
        has_current_ = false;
        has_pending_ = false;
        ended_ = false;
        if (first_timestamp_ == AV_NOPTS_VALUE) {
            // The first picture fixes the origin of every index.
            av_seek_frame(format_.get(), stream_, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codec_.get());
            bool ended = false;
            if (!Decode(ended, code, error)) {
                return false;
            }
            if (ended) {
                code = "DecodeFailed";
                error = "no picture in '" + path_ + "'";
                return false;
            }
            int64_t timestamp = pending_->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) {
                timestamp = pending_->pts;
            }
            first_timestamp_ = timestamp;
            next_index_ = 0;
            has_pending_ = true;
            if (frame == 0) {
                return true;
            }
        }
        const AVStream* stream = format_->streams[stream_];
        const int64_t target =
            (first_timestamp_ != AV_NOPTS_VALUE ? first_timestamp_ : 0) +
            av_rescale_q(
                frame, AVRational{info_.rate_denominator, info_.rate_numerator},
                stream->time_base);
        has_pending_ = false;
        const int result =
            av_seek_frame(format_.get(), stream_, target, AVSEEK_FLAG_BACKWARD);
        if (result < 0) {
            av_seek_frame(format_.get(), stream_, 0, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(codec_.get());
        return true;
    }

    // Decodes the next picture into pending_.
    bool Decode(bool& ended, std::string& code, std::string& error) {
        ended = false;
        while (true) {
            const int received =
                avcodec_receive_frame(codec_.get(), pending_.get());
            if (received == 0) {
                return true;
            }
            if (received == AVERROR_EOF) {
                ended = true;
                return true;
            }
            if (received != AVERROR(EAGAIN)) {
                code = "DecodeFailed";
                error = "cannot decode '" + path_ + "': " + AvError(received);
                return false;
            }
            if (ended_) {
                ended = true;
                return true;
            }
            const int read = av_read_frame(format_.get(), packet_.get());
            if (read < 0) {
                ended_ = true;
                avcodec_send_packet(codec_.get(), nullptr);
                continue;
            }
            if (packet_->stream_index == stream_) {
                const int sent =
                    avcodec_send_packet(codec_.get(), packet_.get());
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    av_packet_unref(packet_.get());
                    code = "DecodeFailed";
                    error = "cannot decode '" + path_ + "': " + AvError(sent);
                    return false;
                }
            }
            av_packet_unref(packet_.get());
        }
    }

    // Makes sure pending_ holds the picture after current_.
    bool Advance(bool& ended, std::string& code, std::string& error) {
        ended = false;
        if (has_pending_) {
            next_index_ = IndexOf(pending_.get());
            return true;
        }
        if (!Decode(ended, code, error)) {
            return false;
        }
        if (ended) {
            return true;
        }
        has_pending_ = true;
        next_index_ = IndexOf(pending_.get());
        return true;
    }

    void Promote() {
        std::swap(current_, pending_);
        av_frame_unref(pending_.get());
        has_pending_ = false;
        has_current_ = true;
        current_index_ = next_index_;
    }

    std::string path_;
    MotionMediaInfo info_;
    FormatPtr format_;
    CodecPtr codec_;
    PacketPtr packet_;
    FramePtr current_;
    FramePtr pending_;
    int stream_ = -1;
    bool has_current_ = false;
    bool has_pending_ = false;
    bool ended_ = false;
    int64_t current_index_ = -1;
    int64_t next_index_ = 0;
    int64_t first_timestamp_ = AV_NOPTS_VALUE;
};

bool DecodeAudio(const std::string& path, MotionPcm& output, std::string& code,
                 std::string& error) {
    FormatPtr format;
    if (!Open(path, format, code, error)) {
        return false;
    }
    const int streamIndex = av_find_best_stream(
        format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        code = "MediaHasNoAudio";
        error = "media '" + path + "' has no audio stream";
        return false;
    }
    const AVStream* stream = format->streams[streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    CodecPtr codec(decoder != nullptr ? avcodec_alloc_context3(decoder)
                                      : nullptr);
    if (!codec ||
        avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
        avcodec_open2(codec.get(), decoder, nullptr) < 0) {
        code = "DecodeFailed";
        error = "no decoder for the sound of '" + path + "'";
        return false;
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* rawResampler = nullptr;
    const int configured = swr_alloc_set_opts2(
        &rawResampler, &stereo, AV_SAMPLE_FMT_FLTP, kMotionAudioRate,
        &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&stereo);
    std::unique_ptr<SwrContext, ResampleCloser> resampler(rawResampler);
    if (configured < 0 || !resampler || swr_init(resampler.get()) < 0) {
        code = "DecodeFailed";
        error = "cannot resample the sound of '" + path + "'";
        return false;
    }
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    const auto convert = [&](const AVFrame* input) {
        const int samples = input != nullptr ? input->nb_samples : 0;
        const int capacity =
            std::max(1, swr_get_out_samples(resampler.get(), samples));
        const size_t at = output.left.size();
        output.left.resize(at + static_cast<size_t>(capacity));
        output.right.resize(at + static_cast<size_t>(capacity));
        uint8_t* planes[2] = {reinterpret_cast<uint8_t*>(&output.left[at]),
                              reinterpret_cast<uint8_t*>(&output.right[at])};
        const int count = swr_convert(
            resampler.get(), planes, capacity,
            input != nullptr ? const_cast<const uint8_t**>(input->extended_data)
                             : nullptr,
            samples);
        if (count < 0) {
            return false;
        }
        output.left.resize(at + static_cast<size_t>(count));
        output.right.resize(at + static_cast<size_t>(count));
        return true;
    };
    const auto receive = [&]() {
        while (true) {
            const int status = avcodec_receive_frame(codec.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return true;
            }
            if (status < 0 || !convert(frame.get())) {
                return false;
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == streamIndex) {
            const int sent = avcodec_send_packet(codec.get(), packet.get());
            if ((sent < 0 && sent != AVERROR(EAGAIN)) || !receive()) {
                av_packet_unref(packet.get());
                code = "DecodeFailed";
                error = "cannot decode the sound of '" + path + "'";
                return false;
            }
        }
        av_packet_unref(packet.get());
    }
    avcodec_send_packet(codec.get(), nullptr);
    if (!receive()) {
        code = "DecodeFailed";
        error = "cannot decode the sound of '" + path + "'";
        return false;
    }
    // What the resampler still holds.
    while (swr_get_delay(resampler.get(), kMotionAudioRate) > 0) {
        const size_t before = output.left.size();
        if (!convert(nullptr) || output.left.size() == before) {
            break;
        }
    }
    output.left.shrink_to_fit();
    output.right.shrink_to_fit();
    return true;
}

// Process-wide caches. Readers are few and costly to open; sounds are
// large, so the oldest is dropped past a budget (about 45 minutes of
// stereo float).
constexpr size_t kOpenReaders = 8;
constexpr size_t kSoundBudgetBytes = size_t{1} << 30;

struct ReaderEntry {
    std::string path;
    std::shared_ptr<VideoReader> reader;
    std::shared_ptr<std::mutex> mutex;
};

struct Caches {
    std::mutex mutex;
    std::list<ReaderEntry> readers;  // most recently used first
    std::list<std::pair<std::string, std::shared_ptr<const MotionPcm>>> sounds;
    size_t sound_bytes = 0;
    std::map<std::string, MotionMediaInfo> infos;
};

Caches& Shared() {
    static Caches caches;
    return caches;
}

size_t PcmBytes(const MotionPcm& pcm) {
    return (pcm.left.size() + pcm.right.size()) * sizeof(float);
}

}  // namespace

bool ProbeMotionMedia(const std::string& path, MotionMediaInfo& info,
                      std::string& code, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(Shared().mutex);
        const auto found = Shared().infos.find(path);
        if (found != Shared().infos.end()) {
            info = found->second;
            return true;
        }
    }
    FormatPtr format;
    if (!Open(path, format, code, error)) {
        return false;
    }
    if (!ProbeOpen(format.get(), info)) {
        code = "MediaMissing";
        error = "media '" + path + "' holds neither pictures nor sound";
        return false;
    }
    std::lock_guard<std::mutex> lock(Shared().mutex);
    Shared().infos[path] = info;
    return true;
}

bool ReadMotionVideoFrame(const std::string& path, int64_t frame,
                          MotionVideoPicture& picture, std::string& code,
                          std::string& error) {
    MotionDecodedPicture decoded;
    return DecodeMotionVideoFrame(path, frame, decoded, code, error) &&
           ConvertMotionVideoFrame(decoded, picture, code, error);
}

bool DecodeMotionVideoFrame(const std::string& path, int64_t frame,
                            MotionDecodedPicture& decoded, std::string& code,
                            std::string& error) {
    std::shared_ptr<VideoReader> reader;
    std::shared_ptr<std::mutex> readerMutex;
    {
        std::lock_guard<std::mutex> lock(Shared().mutex);
        auto& readers = Shared().readers;
        const auto found = std::find_if(
            readers.begin(), readers.end(),
            [&](const ReaderEntry& entry) { return entry.path == path; });
        if (found != readers.end()) {
            readers.splice(readers.begin(), readers, found);
            reader = readers.front().reader;
            readerMutex = readers.front().mutex;
        }
    }
    if (!reader) {
        auto opened = std::make_shared<VideoReader>();
        if (!opened->Open(path, code, error)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(Shared().mutex);
        auto& readers = Shared().readers;
        readers.push_front({path, opened, std::make_shared<std::mutex>()});
        while (readers.size() > kOpenReaders) {
            readers.pop_back();
        }
        reader = readers.front().reader;
        readerMutex = readers.front().mutex;
    }
    std::lock_guard<std::mutex> lock(*readerMutex);
    return reader->Read(frame, decoded, code, error);
}

bool ConvertMotionVideoFrame(const MotionDecodedPicture& decoded,
                             MotionVideoPicture& picture, std::string& code,
                             std::string& error) {
    const auto* source = static_cast<const AVFrame*>(decoded.pixels.get());
    if (source == nullptr) {
        code = "DecodeFailed";
        error = "no picture decoded from '" + decoded.path + "'";
        return false;
    }
    // PERF11 -- one converter per thread instead of one per file: the
    // conversion runs outside the file's lock, on whichever thread renders
    // the frame. Same arguments, same pixels.
    thread_local std::unique_ptr<SwsContext, ScaleCloser> scale;
    scale.reset(
        sws_getCachedContext(scale.release(), source->width, source->height,
                             static_cast<AVPixelFormat>(source->format),
                             source->width, source->height, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR | SWS_ACCURATE_RND |
                                 SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP,
                             nullptr, nullptr, nullptr));
    if (!scale) {
        code = "DecodeFailed";
        error = "cannot convert the pictures of '" + decoded.path + "'";
        return false;
    }
    if (!HasFormatFlag(source->format, AV_PIX_FMT_FLAG_RGB)) {
        const int* coefficients = sws_getCoefficients(ScaleColorspace(source));
        const int full = source->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(scale.get(), coefficients, full,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                 1 << 16, 1 << 16);
    }
    work_counters::Count(work_counters::Work::kMotionVideoConvert);
    std::vector<uint8_t> upright(static_cast<size_t>(source->width) *
                                 static_cast<size_t>(source->height) * 4);
    uint8_t* planes[4] = {upright.data(), nullptr, nullptr, nullptr};
    int strides[4] = {source->width * 4, 0, 0, 0};
    sws_scale(scale.get(), source->data, source->linesize, 0, source->height,
              planes, strides);
    // A picture without alpha is opaque; one with alpha (a ProRes 4444
    // rendered with --alpha) keeps it, straight as swscale writes it.
    picture.opaque = true;
    if (!HasFormatFlag(source->format, AV_PIX_FMT_FLAG_ALPHA)) {
        for (size_t at = 3; at < upright.size(); at += 4) {
            upright[at] = 255;
        }
    } else {
        for (size_t at = 3; at < upright.size() && picture.opaque; at += 4) {
            picture.opaque = upright[at] == 255;
        }
    }
    picture.frame = decoded.frame;
    Rotate(upright, source->width, source->height, decoded.rotation, picture);
    return true;
}

void MotionMediaTurns::Enter(size_t ordinal) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return next_ >= ordinal; });
}

void MotionMediaTurns::Leave(size_t ordinal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ordinal < next_ || !left_.insert(ordinal).second) {
        return;
    }
    while (!left_.empty() && *left_.begin() == next_) {
        left_.erase(left_.begin());
        ++next_;
    }
    changed_.notify_all();
}

bool ReadMotionAudio(const std::string& path,
                     std::shared_ptr<const MotionPcm>& pcm, std::string& code,
                     std::string& error) {
    {
        std::lock_guard<std::mutex> lock(Shared().mutex);
        auto& sounds = Shared().sounds;
        const auto found = std::find_if(
            sounds.begin(), sounds.end(),
            [&](const auto& entry) { return entry.first == path; });
        if (found != sounds.end()) {
            sounds.splice(sounds.begin(), sounds, found);
            pcm = sounds.front().second;
            return true;
        }
    }
    auto decoded = std::make_shared<MotionPcm>();
    if (!DecodeAudio(path, *decoded, code, error)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(Shared().mutex);
    auto& caches = Shared();
    caches.sounds.emplace_front(path, decoded);
    caches.sound_bytes += PcmBytes(*decoded);
    while (caches.sound_bytes > kSoundBudgetBytes && caches.sounds.size() > 1) {
        caches.sound_bytes -= PcmBytes(*caches.sounds.back().second);
        caches.sounds.pop_back();
    }
    pcm = decoded;
    return true;
}

void ResetMotionMediaCache() {
    std::lock_guard<std::mutex> lock(Shared().mutex);
    Shared().readers.clear();
    Shared().sounds.clear();
    Shared().sound_bytes = 0;
    Shared().infos.clear();
}
