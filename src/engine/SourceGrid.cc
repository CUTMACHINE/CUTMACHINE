#include "SourceGrid.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace {

bool SameRate(MediaRate left, MediaRate right) {
    return static_cast<int64_t>(left.num) * right.den ==
           static_cast<int64_t>(right.num) * left.den;
}

bool ValidRate(MediaRate rate) { return rate.num > 0 && rate.den > 0; }

// Halves away from zero, as AV_ROUND_NEAR_INF.
int64_t RoundNearInf(__int128 numerator, __int128 denominator) {
    if (numerator >= 0) {
        return static_cast<int64_t>((2 * numerator + denominator) /
                                    (2 * denominator));
    }
    return -static_cast<int64_t>((-2 * numerator + denominator) /
                                 (2 * denominator));
}

std::string Seconds(const char* format, double seconds) {
    char text[64];
    std::snprintf(text, sizeof(text), format, seconds);
    return text;
}

}  // namespace

bool IsStandardFrameRate(MediaRate rate) {
    static constexpr MediaRate kStandard[] = {
        {24000, 1001}, {24, 1},  {25, 1},  {30000, 1001},  {30, 1},
        {48000, 1001}, {48, 1},  {50, 1},  {60000, 1001},  {60, 1},
        {100, 1},      {120, 1}, {240, 1}, {120000, 1001},
    };
    if (!ValidRate(rate)) return false;
    return std::any_of(
        std::begin(kStandard), std::end(kStandard),
        [&](MediaRate standard) { return SameRate(rate, standard); });
}

MediaRate SourceCountingRate(MediaRate average, MediaRate nominal) {
    if (!ValidRate(nominal) || !ValidRate(average)) return average;
    if (SameRate(average, nominal) || IsStandardFrameRate(average) ||
        !IsStandardFrameRate(nominal)) {
        return average;
    }
    // |average / nominal - 1| <= 1/50, cross-multiplied.
    const __int128 a = static_cast<__int128>(average.num) * nominal.den;
    const __int128 n = static_cast<__int128>(nominal.num) * average.den;
    const __int128 gap = a > n ? a - n : n - a;
    return gap * 50 <= n ? nominal : average;
}

int64_t SourceGridStep(int64_t pts, int64_t originPts, MediaRate timeBase,
                       MediaRate rate) {
    const __int128 numerator =
        static_cast<__int128>(pts - originPts) * timeBase.num * rate.num;
    const __int128 denominator = static_cast<__int128>(timeBase.den) * rate.den;
    return RoundNearInf(numerator, denominator);
}

int64_t SourceGridFirstPts(int64_t step, int64_t originPts, MediaRate timeBase,
                           MediaRate rate) {
    // step - 1/2 grid steps after the origin, in ticks, then the exact
    // boundary of the rounding found by walking at most a tick or two.
    const __int128 numerator =
        (2 * static_cast<__int128>(step) - 1) * timeBase.den * rate.den;
    const __int128 denominator =
        2 * static_cast<__int128>(timeBase.num) * rate.num;
    __int128 offset = numerator / denominator;
    if (offset * denominator > numerator) --offset;
    int64_t pts = originPts + static_cast<int64_t>(offset);
    while (SourceGridStep(pts, originPts, timeBase, rate) >= step) --pts;
    while (SourceGridStep(pts, originPts, timeBase, rate) < step) ++pts;
    return pts;
}

bool FirstPicturePts(const std::string& path, int64_t& pts,
                     std::string& error) {
    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        error = "unable to open " + path;
        return false;
    }
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool found = false;
    int stream = -1;
    if (avformat_find_stream_info(format, nullptr) >= 0)
        stream =
            av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* codec =
        stream >= 0
            ? avcodec_find_decoder(format->streams[stream]->codecpar->codec_id)
            : nullptr;
    if (codec != nullptr && packet != nullptr && frame != nullptr) {
        decoder = avcodec_alloc_context3(codec);
        if (decoder != nullptr &&
            avcodec_parameters_to_context(
                decoder, format->streams[stream]->codecpar) >= 0 &&
            avcodec_open2(decoder, codec, nullptr) >= 0) {
            bool draining = false;
            while (!found) {
                if (!draining) {
                    const int read = av_read_frame(format, packet);
                    if (read < 0) {
                        draining = true;
                        avcodec_send_packet(decoder, nullptr);
                    } else if (packet->stream_index == stream) {
                        avcodec_send_packet(decoder, packet);
                        av_packet_unref(packet);
                    } else {
                        av_packet_unref(packet);
                        continue;
                    }
                }
                const int received = avcodec_receive_frame(decoder, frame);
                if (received == 0) {
                    const int64_t timestamp = frame->best_effort_timestamp;
                    if (timestamp != AV_NOPTS_VALUE) {
                        pts = timestamp;
                        found = true;
                    }
                } else if (received == AVERROR_EOF ||
                           (draining && received != AVERROR(EAGAIN))) {
                    break;
                }
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    if (!found) error = "no picture decoded from " + path;
    return found;
}

SourceGridRead SourceGridReadFor(int64_t firstPicturePts, MediaRate rate,
                                 int64_t first, int64_t last,
                                 bool boundDuration) {
    SourceGridRead read;
    const double runSeconds =
        static_cast<double>(first) * rate.den / static_cast<double>(rate.num);
    const double seek = std::max(0.0, runSeconds - kSourceGridSeekLeadSeconds);
    if (seek > 0.0) {
        read.input_arguments.insert(read.input_arguments.end(),
                                    {"-ss", Seconds("%.6f", seek)});
    }
    if (boundDuration) {
        const double end = static_cast<double>(std::max(first, last) + 2) *
                           rate.den / static_cast<double>(rate.num);
        read.input_arguments.insert(
            read.input_arguments.end(),
            {"-t", Seconds("%.6f", end - seek + kSourceGridSeekLeadSeconds)});
    }
    read.input_arguments.push_back("-copyts");
    // fps anchored at the first picture pads the frames the seek skipped
    // with copies, so the grid index survives it; trim then drops
    // everything before `first`.
    read.filter = "setpts=PTS-" + std::to_string(firstPicturePts) +
                  ",fps=" + std::to_string(rate.num) + '/' +
                  std::to_string(rate.den) +
                  ":start_time=0,trim=start_frame=" + std::to_string(first);
    return read;
}
