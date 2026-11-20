#include "AudioDecode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <memory>

namespace audio_decode {

namespace {

std::string AvError(int value) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(value, text, sizeof(text));
    return text;
}

}  // namespace

bool DecodeStereoPcm16(const std::string& path, int32_t sampleRate,
                       StereoPcm16& output, std::string& error,
                       const std::atomic_bool* cancel, DecodeStats* stats) {
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "audio open failed: " + AvError(result);
        return false;
    }
    const auto closeFormat = [](AVFormatContext* value) {
        avformat_close_input(&value);
    };
    std::unique_ptr<AVFormatContext, decltype(closeFormat)> formatOwner(
        format, closeFormat);
    // Whatever the outcome, what the file cost is reported.
    const auto report = [&] {
        if (stats != nullptr && format->pb != nullptr) {
            stats->bytes_read = format->pb->bytes_read;
        }
    };
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        report();
        error = "audio stream headers failed: " + AvError(result);
        return false;
    }
    const int streamIndex =
        av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        report();
        error = "no audio stream";
        return false;
    }
    // PERF13 -- the pictures, the camera's metadata track and any other
    // stream are never read: see the header. Set after the stream headers
    // are read, so that what FFmpeg learns of the file is what it learnt
    // before, and the packets it buffered while probing are only dropped.
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        if (static_cast<int>(index) != streamIndex) {
            format->streams[index]->discard = AVDISCARD_ALL;
        }
    }
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        report();
        error = "no audio decoder";
        return false;
    }
    AVCodecContext* rawDecoder = avcodec_alloc_context3(codec);
    const auto freeDecoder = [](AVCodecContext* value) {
        avcodec_free_context(&value);
    };
    std::unique_ptr<AVCodecContext, decltype(freeDecoder)> decoder(rawDecoder,
                                                                   freeDecoder);
    if (!decoder ||
        avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0) {
        report();
        error = "unable to initialize audio decoder";
        return false;
    }

    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* rawResampler = nullptr;
    result = swr_alloc_set_opts2(&rawResampler, &stereo, AV_SAMPLE_FMT_S16P,
                                 sampleRate, &decoder->ch_layout,
                                 decoder->sample_fmt, decoder->sample_rate, 0,
                                 nullptr);
    av_channel_layout_uninit(&stereo);
    const auto freeResampler = [](SwrContext* value) { swr_free(&value); };
    std::unique_ptr<SwrContext, decltype(freeResampler)> resampler(
        rawResampler, freeResampler);
    if (result < 0 || !resampler || swr_init(resampler.get()) < 0) {
        report();
        error = "unable to initialize audio resampler";
        return false;
    }

    AVPacket* rawPacket = av_packet_alloc();
    AVFrame* rawFrame = av_frame_alloc();
    const auto freePacket = [](AVPacket* value) { av_packet_free(&value); };
    const auto freeFrame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(rawPacket,
                                                           freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(rawFrame, freeFrame);
    if (!packet || !frame) {
        report();
        error = "unable to allocate audio packet/frame";
        return false;
    }

    const auto convert = [&](const AVFrame* input) {
        const int inputSamples = input != nullptr ? input->nb_samples : 0;
        const int capacity =
            std::max(1, swr_get_out_samples(resampler.get(), inputSamples));
        std::vector<int16_t> left(static_cast<size_t>(capacity));
        std::vector<int16_t> right(static_cast<size_t>(capacity));
        uint8_t* planes[2] = {
            reinterpret_cast<uint8_t*>(left.data()),
            reinterpret_cast<uint8_t*>(right.data()),
        };
        const uint8_t* const* inputData =
            input != nullptr
                ? const_cast<const uint8_t* const*>(input->extended_data)
                : nullptr;
        const int count =
            swr_convert(resampler.get(), planes, capacity,
                        const_cast<const uint8_t**>(inputData), inputSamples);
        if (count < 0) {
            return false;
        }
        output.left.insert(output.left.end(), left.begin(),
                           left.begin() + count);
        output.right.insert(output.right.end(), right.begin(),
                            right.begin() + count);
        return true;
    };
    const auto receive = [&]() {
        while (true) {
            const int status =
                avcodec_receive_frame(decoder.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return true;
            }
            if (status < 0 || !convert(frame.get())) {
                return false;
            }
            av_frame_unref(frame.get());
        }
    };

    // Grown one packet at a time, a vector doubles its capacity as it goes
    // and ends up to twice the size of what it holds. Reserved from the
    // declared duration (a little over, for the decoder's padding), and
    // trimmed to what was decoded at the end. Capped at three hours, so a
    // container that declares a wrong duration cannot reserve gigabytes.
    if (format->duration > 0) {
        const long double seconds = std::min<long double>(
            static_cast<long double>(format->duration) / AV_TIME_BASE,
            3.0L * 3600.0L);
        const auto expected =
            static_cast<size_t>(seconds * sampleRate + sampleRate);
        output.left.reserve(expected);
        output.right.reserve(expected);
    }
    while (av_read_frame(format, packet.get()) >= 0) {
        if (cancel != nullptr && cancel->load()) {
            av_packet_unref(packet.get());
            report();
            error = "audio decode cancelled";
            return false;
        }
        if (packet->stream_index == streamIndex) {
            result = avcodec_send_packet(decoder.get(), packet.get());
            if (result < 0 && result != AVERROR(EAGAIN)) {
                report();
                error = "audio packet decode failed: " + AvError(result);
                return false;
            }
            if (!receive()) {
                report();
                error = "audio frame decode/resample failed";
                return false;
            }
        }
        av_packet_unref(packet.get());
    }
    report();
    avcodec_send_packet(decoder.get(), nullptr);
    if (!receive()) {
        error = "audio decoder drain failed";
        return false;
    }
    while (swr_get_delay(resampler.get(), sampleRate) > 0) {
        const size_t before = output.left.size();
        if (!convert(nullptr) || output.left.size() == before) {
            break;
        }
    }
    output.left.shrink_to_fit();
    output.right.shrink_to_fit();
    return !output.left.empty();
}

}  // namespace audio_decode
