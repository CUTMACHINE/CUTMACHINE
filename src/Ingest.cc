#include "Ingest.h"

#include "CameraMetadata.h"

#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "FfmpegBinary.h"
#include "MediaTaskManager.h"
#include "ProjectStorage.h"
#include "SequenceFormat.h"
#include "SourceGrid.h"
#include "SpeechOnset.h"
#include "Ulid.h"
#include "WorkCounters.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/version_major.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// 8 kHz mono is enough for a level: the measurement is an energy average,
// not a spectrum, and a lower rate is a shorter decode. The rate does not
// change the answer beyond rounding, so it is not recorded anywhere.
constexpr uint32_t kLevelSampleRate = 8000;

std::string EscapeJson(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string AvError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::filesystem::path Resolved(const std::filesystem::path& path,
                               std::error_code& error) {
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    return std::filesystem::weakly_canonical(absolute, error);
}

bool ProbeImpl(const std::filesystem::path& absolutePath, LibraryMedia& media,
               std::string& reason) {
    AVFormatContext* rawContext = nullptr;
    int result = avformat_open_input(&rawContext, absolutePath.c_str(), nullptr,
                                     nullptr);
    if (result < 0) {
        reason = "unable to open media: " + AvError(result);
        return false;
    }
    struct ContextCloser {
        void operator()(AVFormatContext* context) const {
            avformat_close_input(&context);
        }
    };
    std::unique_ptr<AVFormatContext, ContextCloser> context(rawContext);
    result = avformat_find_stream_info(context.get(), nullptr);
    if (result < 0) {
        reason = "unable to read stream headers: " + AvError(result);
        return false;
    }

    // FI14 -- docs/roadmap/FIABILITE-2026-10.md. A song's cover art (an
    // .m4a's or an .mp3's attached picture) is a video stream of one image,
    // without a frame rate: taken for the picture, it refused the file
    // before its sound was read. Only a stream that plays is a picture.
    int videoIndex = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1,
                                         -1, nullptr, 0);
    if (videoIndex >= 0 && (context->streams[videoIndex]->disposition &
                            AV_DISPOSITION_ATTACHED_PIC) != 0) {
        videoIndex = -1;
        for (unsigned index = 0; index < context->nb_streams; ++index) {
            const AVStream* stream = context->streams[index];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
                videoIndex = static_cast<int>(index);
                break;
            }
        }
    }
    const int audioIndex = av_find_best_stream(
        context.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (videoIndex < 0 && audioIndex < 0) {
        reason = "no audio or video stream";
        return false;
    }

    media.has_video = videoIndex >= 0;
    if (media.has_video) {
        AVStream* video = context->streams[videoIndex];
        const AVCodecParameters* parameters = video->codecpar;
        if (parameters->width <= 0 || parameters->height <= 0) {
            reason = "video stream has invalid dimensions";
            return false;
        }
        if (video->avg_frame_rate.num <= 0 || video->avg_frame_rate.den <= 0) {
            reason = "video stream has no valid avg_frame_rate";
            return false;
        }
        // VF2 -- docs/roadmap/VFR-2026-11.md. The rate the frames are
        // counted at, the one the decoder reports too (MediaSource.mm): a
        // phone's variable-rate stream is counted on the grid it was
        // recorded on, not at its average (SourceGrid.h).
        const MediaRate counted = SourceCountingRate(
            {video->avg_frame_rate.num, video->avg_frame_rate.den},
            {video->r_frame_rate.num, video->r_frame_rate.den});
        const AVRational frameRate{counted.num, counted.den};

        int64_t duration = 0;
        if (video->duration != AV_NOPTS_VALUE && video->duration > 0) {
            duration = av_rescale_q_rnd(
                video->duration, video->time_base, AVRational{1, frameRate.num},
                static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                        AV_ROUND_PASS_MINMAX));
        } else if (context->duration != AV_NOPTS_VALUE &&
                   context->duration > 0) {
            duration =
                av_rescale_q_rnd(context->duration, AVRational{1, AV_TIME_BASE},
                                 AVRational{1, frameRate.num},
                                 static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                                         AV_ROUND_PASS_MINMAX));
        }
        // R4 -- docs/roadmap/REEL-2026-09.md. A still image (a PNG, a JPEG,
        // a TIFF) read by an image demuxer has no duration of its own: it
        // is one frame, at the rate the demuxer gives it, and a clip holds
        // it for as long as it likes at speed 0/1, the freeze rule of
        // ClipSpeed.h. An image sequence has a duration and is read as a
        // video.
        const std::string demuxer =
            context->iformat != nullptr && context->iformat->name != nullptr
                ? context->iformat->name
                : "";
        const bool imageDemuxer =
            demuxer == "image2" ||
            (demuxer.size() > 5 &&
             demuxer.compare(demuxer.size() - 5, 5, "_pipe") == 0);
        if (duration <= 0 && imageDemuxer) {
            duration = frameRate.den;
        }
        if (duration <= 0) {
            reason = "video stream has no positive duration";
            return false;
        }

        double rotation = 0.0;
        const uint8_t* displayMatrix = nullptr;
        size_t displayMatrixSize = 0;
#if LIBAVFORMAT_VERSION_MAJOR >= 62
        const AVPacketSideData* matrixSideData = av_packet_side_data_get(
            parameters->coded_side_data, parameters->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX);
        if (matrixSideData) {
            displayMatrix = matrixSideData->data;
            displayMatrixSize = matrixSideData->size;
        }
#else
        displayMatrix = av_stream_get_side_data(
            video, AV_PKT_DATA_DISPLAYMATRIX, &displayMatrixSize);
#endif
        if (displayMatrix && displayMatrixSize >= 9 * sizeof(int32_t)) {
            const double value = av_display_rotation_get(
                reinterpret_cast<const int32_t*>(displayMatrix));
            if (!std::isnan(value)) rotation = value;
        }
        media.rotation_degrees = static_cast<int32_t>(std::lround(rotation));
        const double radians =
            media.rotation_degrees * 3.14159265358979323846 / 180.0;
        const double displayedWidth =
            std::abs(parameters->width * std::cos(radians)) +
            std::abs(parameters->height * std::sin(radians));
        const double displayedHeight =
            std::abs(parameters->width * std::sin(radians)) +
            std::abs(parameters->height * std::cos(radians));

        media.codec = avcodec_get_name(parameters->codec_id);
        media.width = parameters->width;
        media.height = parameters->height;
        const char* pixelFormat =
            av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format));
        const char* colorRange = av_color_range_name(parameters->color_range);
        const char* colorSpace = av_color_space_name(parameters->color_space);
        const char* colorTransfer =
            av_color_transfer_name(parameters->color_trc);
        const char* colorPrimaries =
            av_color_primaries_name(parameters->color_primaries);
        media.pixel_format = pixelFormat ? pixelFormat : "unknown";
        media.color_range = colorRange ? colorRange : "unknown";
        media.color_space = colorSpace ? colorSpace : "unknown";
        media.color_transfer = colorTransfer ? colorTransfer : "unknown";
        media.color_primaries = colorPrimaries ? colorPrimaries : "unknown";
        media.rate = {frameRate.num, frameRate.den};
        media.duration = {duration, frameRate.num};
        const double scale = std::max(displayedWidth, displayedHeight);
        if (std::abs(displayedWidth - displayedHeight) <= scale * 1e-9) {
            media.orientation = "square";
        } else {
            media.orientation =
                displayedWidth > displayedHeight ? "landscape" : "portrait";
        }
    } else {
        AVStream* audioStream = context->streams[audioIndex];
        const AVCodecParameters* audio = audioStream->codecpar;
        if (audio->sample_rate <= 0 || audio->ch_layout.nb_channels <= 0) {
            reason = "audio stream has invalid sample rate or channel count";
            return false;
        }
        int64_t duration = 0;
        if (audioStream->duration != AV_NOPTS_VALUE &&
            audioStream->duration > 0) {
            duration =
                av_rescale_q_rnd(audioStream->duration, audioStream->time_base,
                                 AVRational{1, audio->sample_rate},
                                 static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                                         AV_ROUND_PASS_MINMAX));
        } else if (context->duration != AV_NOPTS_VALUE &&
                   context->duration > 0) {
            duration =
                av_rescale_q_rnd(context->duration, AVRational{1, AV_TIME_BASE},
                                 AVRational{1, audio->sample_rate},
                                 static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                                         AV_ROUND_PASS_MINMAX));
        }
        if (duration <= 0) {
            reason = "audio stream has no positive duration";
            return false;
        }
        media.codec = avcodec_get_name(audio->codec_id);
        media.rate = {audio->sample_rate, 1};
        media.duration = {duration, audio->sample_rate};
        media.orientation = "audio";
    }

    // G7 -- docs/roadmap/GRADE-2026-09.md. The white balance a Sony camera
    // recorded at, from its timed metadata track (CameraMetadata.h): the
    // first rtmd packet, every other stream discarded so that only that
    // track is read. Unknown, 0, for every other file. G3 -- and the
    // picture's gamma and primaries, from the same set.
    media.white_balance_kelvin = 0;
    SonyRtmdCamera camera;
    if (media.has_video) {
        int rtmdIndex = -1;
        for (unsigned index = 0; index < context->nb_streams; ++index) {
            if (context->streams[index]->codecpar->codec_tag ==
                MKTAG('r', 't', 'm', 'd')) {
                rtmdIndex = static_cast<int>(index);
                break;
            }
        }
        if (rtmdIndex >= 0) {
            for (unsigned index = 0; index < context->nb_streams; ++index) {
                context->streams[index]->discard =
                    static_cast<int>(index) == rtmdIndex ? AVDISCARD_DEFAULT
                                                         : AVDISCARD_ALL;
            }
            AVPacket* packet = av_packet_alloc();
            // The rtmd track interleaves one packet per frame; a few
            // hundred reads reach its first even behind other buffered
            // packets.
            for (int read = 0; packet != nullptr && read < 256; ++read) {
                if (av_read_frame(context.get(), packet) < 0) break;
                const bool metadata = packet->stream_index == rtmdIndex;
                if (metadata &&
                    ParseSonyRtmdCamera(packet->data,
                                        static_cast<size_t>(packet->size),
                                        camera)) {
                    media.white_balance_kelvin = camera.white_balance_kelvin;
                }
                av_packet_unref(packet);
                if (metadata) break;
            }
            av_packet_free(&packet);
        }
    }

    // G3 -- the colour input the file declares, read in order of trust: the
    // camera's own labels (an FX30 in S-Log3 tags nothing in its MP4), then
    // the container's tags -- unless the container announces a curve of its
    // own. A Blackmagic Pocket 4K writes its Film log as BT.709 in the tags
    // and names the log under com.apple.proapps.customgamma: sixty such
    // rushes were found on the drive this was measured on. Anything else
    // follows the project's input.
    media.color_input = {};
    if (media.has_video) {
        if (!camera.transfer.empty() && !camera.gamut.empty()) {
            media.color_input.gamut = camera.gamut;
            media.color_input.transfer = camera.transfer;
        } else if (av_dict_get(context->metadata,
                               "com.apple.proapps.customgamma", nullptr,
                               0) == nullptr) {
            media.color_input = ColorInputFromTags(media);
        }
    }

    if (audioIndex >= 0) {
        const AVCodecParameters* audio = context->streams[audioIndex]->codecpar;
        if (audio->sample_rate > 0 && audio->ch_layout.nb_channels > 0) {
            media.has_audio = true;
            media.audio_rate = audio->sample_rate;
            media.audio_channels = audio->ch_layout.nb_channels;
        }
    }
    return true;
}

// Decodes the first audio stream to mono 16-bit PCM on a pipe and folds it
// into one RMS figure. The same launch Waveform.cc and SpeechOnset.cc make,
// without their cancellation: an ingest is a short synchronous command, not
// a background task a user can interrupt.
bool MeasureAudioLevelImpl(const std::filesystem::path& path,
                           const std::string& ffmpegPath, int64_t& level,
                           std::string& reason) {
    level = 0;
    reason.clear();
    const std::string rate = std::to_string(kLevelSampleRate);
    std::vector<std::string> storage = {
        ffmpegPath, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-i",       path.string(),  "-map",      "0:a:0", "-vn",
        "-sn",      "-ac",          "1",         "-ar",   rate,
        "-f",       "s16le",        "pipe:1",
    };

    RunningRmsLevel running;
    std::vector<char> pending;
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_output = process::Stream::kPipe;
    // FFmpeg's diagnostics are not this function's business: a media with no
    // audio stream is an expected outcome, not an error to relay.
    request.standard_error = process::Stream::kDiscard;
    request.read_bytes = 65536;
    request.on_output = [&](const char* bytes, size_t count) {
        pending.insert(pending.end(), bytes, bytes + count);
        // Whole samples only: a read can split one in half, and interpreting
        // the halves would make the figure depend on buffer boundaries.
        const size_t whole =
            (pending.size() / sizeof(int16_t)) * sizeof(int16_t);
        if (whole > 0) {
            running.Add(reinterpret_cast<const int16_t*>(pending.data()),
                        whole / sizeof(int16_t));
            pending.erase(pending.begin(),
                          pending.begin() + static_cast<ptrdiff_t>(whole));
        }
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        reason = ffmpeg::StartError(result, "audio level pipe");
        return false;
    }
    if (!result.Succeeded()) {
        reason = "FFmpeg could not decode an audio stream";
        return false;
    }
    if (running.Samples() == 0) {
        reason = "media decoded no audio samples";
        return false;
    }
    level = running.Level();
    return true;
}

bool CollectFiles(const std::filesystem::path& directory, bool recursive,
                  std::vector<std::filesystem::path>& files, size_t& ignored,
                  std::string& reason) {
    std::error_code error;
    if (std::filesystem::is_regular_file(directory, error) && !error) {
        files.push_back(directory);
        return true;
    }
    error.clear();
    if (!std::filesystem::is_directory(directory, error) || error) {
        reason = error ? error.message()
                       : "path is neither a regular file nor a directory";
        return false;
    }
    const auto options = std::filesystem::directory_options::none;
    if (recursive) {
        std::filesystem::recursive_directory_iterator iterator(directory,
                                                               options, error),
            end;
        if (error) {
            reason = error.message();
            return false;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                reason = error.message();
                return false;
            }
            if (IsVolumeSystemDirectory(iterator->path())) {
                iterator.disable_recursion_pending();
                continue;
            }
            if (iterator->is_regular_file(error)) {
                if (IsVolumeLitter(iterator->path()))
                    ++ignored;
                else
                    files.push_back(iterator->path());
            }
            if (error) {
                reason = error.message();
                return false;
            }
        }
    } else {
        std::filesystem::directory_iterator iterator(directory, options, error),
            end;
        if (error) {
            reason = error.message();
            return false;
        }
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                reason = error.message();
                return false;
            }
            if (iterator->is_regular_file(error)) {
                if (IsVolumeLitter(iterator->path()))
                    ++ignored;
                else
                    files.push_back(iterator->path());
            }
            if (error) {
                reason = error.message();
                return false;
            }
        }
    }
    std::sort(files.begin(), files.end());
    return true;
}

// Fills in the mean level of an entry that has just been probed. A failure
// to measure is deliberately not an ingest failure and not even an error
// entry: the file is perfectly usable without the figure, and
// `audio_level_measured` already says the level is unknown. Re-ingesting the
// same path fills it in later, exactly as it does for a v1 entry with no
// technical metadata at all.
void MeasureIngestedAudioLevel(const std::filesystem::path& absolute,
                               LibraryMedia& media) {
    if (!media.has_audio) return;
    int64_t level = 0;
    std::string levelReason;
    // PRD1 -- no path of its own: the binary FfmpegBinary.h resolves.
    if (!MeasureMediaAudioLevel(absolute.string(), std::string(), level,
                                levelReason))
        return;
    media.audio_level_measured = true;
    media.audio_level = level;
}

// What an ingest reports, as the members of a JSON object: the same four
// whether the ingest succeeded or read nothing (PRD20).
std::string ReportMembers(size_t added, size_t skipped, size_t ignored,
                          const std::vector<IngestError>& errors) {
    std::ostringstream output;
    output << "\"added\":" << added << ",\"skipped\":" << skipped
           << ",\"ignored\":" << ignored << ",\"errors\":[";
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index) output << ',';
        output << "{\"file\":\"" << EscapeJson(errors[index].file)
               << "\",\"reason\":\"" << EscapeJson(errors[index].reason)
               << "\"}";
    }
    output << ']';
    return output.str();
}

std::string SuccessJson(size_t added, size_t skipped, size_t ignored,
                        const std::vector<IngestError>& errors) {
    return "{\"ok\":true," + ReportMembers(added, skipped, ignored, errors) +
           "}\n";
}

// PRD20 -- the sentence of an ingest that read nothing: how many files, and
// the first with its reason, so the refusal stands without its `result`.
std::string ReadNothingDetail(const std::vector<IngestError>& errors) {
    const IngestError& first = errors.front();
    if (errors.size() == 1) {
        return "nothing was ingested: '" + first.file +
               "' could not be read (" + first.reason + ")";
    }
    return "nothing was ingested: none of the " +
           std::to_string(errors.size()) +
           " files could be read, starting with '" + first.file + "' (" +
           first.reason + ")";
}

}  // namespace

bool IsVolumeLitter(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name.rfind("._", 0) == 0 || name == ".DS_Store" ||
           name == "Thumbs.db" || name == "desktop.ini";
}

bool IsVolumeSystemDirectory(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == ".Spotlight-V100" || name == ".fseventsd" ||
           name == ".Trashes" || name == ".TemporaryItems" ||
           name == ".DocumentRevisions-V100" || name == "$RECYCLE.BIN" ||
           name == "System Volume Information";
}

bool ProbeMediaMetadata(const std::string& path, LibraryMedia& media,
                        std::string& reason) {
    work_counters::Count(work_counters::Work::kMediaProbe);
    av_log_set_level(AV_LOG_ERROR);
    return ProbeImpl(std::filesystem::path(path), media, reason);
}

bool ReadFileColorFacts(const std::string& path, MediaFileColor& color,
                        std::string& reason) {
    LibraryMedia probed;
    if (!ProbeMediaMetadata(path, probed, reason)) return false;
    color.white_balance_kelvin = probed.white_balance_kelvin;
    color.input = probed.color_input;
    return true;
}

bool MeasureMediaAudioLevel(const std::string& path,
                            const std::string& ffmpegPath, int64_t& level,
                            std::string& reason) {
    return MeasureAudioLevelImpl(std::filesystem::path(path), ffmpegPath, level,
                                 reason);
}

namespace {

// PRD7 -- where each library entry's file is, resolved, against the entry's
// place in the library. Two entries naming one file: the first answers.
std::map<std::string, size_t> LibraryIndexByPath(
    const Document& document, const std::filesystem::path& projectDirectory) {
    std::map<std::string, size_t> known;
    for (size_t index = 0; index < document.library.size(); ++index) {
        std::filesystem::path stored(document.library[index].path);
        if (stored.is_relative()) {
            stored = projectDirectory / stored;
        }
        std::error_code error;
        const std::filesystem::path resolved = Resolved(stored, error);
        if (!error) {
            known.emplace(resolved.string(), index);
        }
    }
    return known;
}

}  // namespace

std::set<std::string> CompleteLibraryPaths(
    const Document& document, const std::filesystem::path& projectDirectory) {
    std::set<std::string> complete;
    for (const auto& [path, index] :
         LibraryIndexByPath(document, projectDirectory)) {
        if (document.library[index].metadata_complete) {
            complete.insert(path);
        }
    }
    return complete;
}

std::string StoredMediaPath(const std::filesystem::path& file,
                            const std::filesystem::path& projectDirectory) {
    std::error_code error;
    const std::filesystem::path target =
        std::filesystem::absolute(file, error).lexically_normal();
    if (error) return file.string();
    const std::filesystem::path base =
        std::filesystem::absolute(projectDirectory, error).lexically_normal();
    if (error) return target.string();
    // The folders both lie in, below the root.
    std::vector<std::string> targetParts;
    std::vector<std::string> baseParts;
    for (const std::filesystem::path& part : target.relative_path())
        targetParts.push_back(part.string());
    for (const std::filesystem::path& part : base.relative_path())
        baseParts.push_back(part.string());
    size_t shared = 0;
    while (shared + 1 < targetParts.size() && shared < baseParts.size() &&
           targetParts[shared] == baseParts[shared])
        ++shared;
    // Under /Volumes, a folder in common is the disk itself.
    const size_t needed =
        !targetParts.empty() && targetParts[0] == "Volumes" ? 2 : 1;
    if (shared < needed || target.root_path() != base.root_path())
        return target.string();
    const std::filesystem::path relative = target.lexically_relative(base);
    return relative.empty() ? target.string() : relative.string();
}

ProbedIngestFile ProbeIngestFile(const std::filesystem::path& candidate,
                                 std::set<std::string>& settled) {
    ProbedIngestFile file;
    std::error_code error;
    file.given = std::filesystem::absolute(candidate, error).lexically_normal();
    if (error) file.given.clear();
    error.clear();
    file.path = Resolved(candidate, error);
    if (error) {
        file.path = candidate;
        file.error = error.message();
        return file;
    }
    if (settled.count(file.path.string()) != 0) {
        return file;
    }
    LibraryMedia media;
    if (!ProbeMediaMetadata(file.path.string(), media, file.error)) {
        return file;
    }
    MeasureIngestedAudioLevel(file.path, media);
    file.media = std::move(media);
    settled.insert(file.path.string());
    return file;
}

bool IngestReadNothing(const IngestOutcome& outcome) {
    return outcome.added_ids.empty() && !outcome.errors.empty() &&
           outcome.skipped == outcome.errors.size();
}

IngestPlan PlanProbedMedia(const Document& document,
                           const std::filesystem::path& projectDirectory,
                           const std::vector<ProbedIngestFile>& files,
                           const std::optional<Ulid>& targetBin) {
    IngestPlan plan;
    IngestOutcome& outcome = plan.outcome;
    plan.operation.bin_id = targetBin ? *targetBin : Ulid{};
    // Where each file already is: an entry of the library (its index), or
    // one this ingest adds (an index into `added`, past the library's).
    std::map<std::string, size_t> known =
        LibraryIndexByPath(document, projectDirectory);
    const size_t libraryCount = document.library.size();
    std::set<Ulid> completedIds;
    std::set<Ulid> mountedIds;
    std::set<Ulid> filedIds;
    for (const ProbedIngestFile& file : files) {
        if (!file.media && !file.error.empty()) {
            ++outcome.skipped;
            outcome.errors.push_back(
                {file.path.filename().string(), file.error});
            continue;
        }
        const auto found = known.find(file.path.string());
        if (found != known.end()) {
            ++outcome.skipped;
            if (found->second >= libraryCount) {
                // A second path to a file this ingest adds: nothing more.
                continue;
            }
            const LibraryMedia& existing = document.library[found->second];
            bool complete = existing.metadata_complete;
            if (!complete && file.media &&
                completedIds.insert(existing.id).second) {
                // What the probe read replaces the entry; what the document
                // decided about it stays. The bin and the proxy were lost
                // here before PRD7, with the rest of the entry.
                // COLOR-T -- and so would the reading of its picture be.
                LibraryMedia completed = *file.media;
                completed.id = existing.id;
                completed.path = existing.path;
                completed.filename = file.path.filename().string();
                completed.bin_id = existing.bin_id;
                completed.proxy_path = existing.proxy_path;
                KeepMediaReading(existing, completed);
                plan.completed.push_back(std::move(completed));
                outcome.completed_ids.push_back(existing.id);
                complete = true;
            } else if (completedIds.count(existing.id) != 0) {
                complete = true;
            }
            if (complete && document.FindSource(existing.id) == nullptr &&
                mountedIds.insert(existing.id).second) {
                plan.mounted.push_back(existing.id);
            }
            if (targetBin && existing.bin_id != *targetBin &&
                filedIds.insert(existing.id).second) {
                plan.operation.filed.push_back(existing.id);
                ++outcome.moved;
            }
            continue;
        }
        if (!file.media) {
            // Left unread because the library held it when the ingest
            // started, and it no longer does: a window's ingest spans two
            // moments, and the document may move between them.
            ++outcome.skipped;
            outcome.errors.push_back(
                {file.path.filename().string(),
                 "not read: the library held this file when the ingest "
                 "started, and no longer does"});
            continue;
        }
        LibraryMedia media = *file.media;
        media.id = GenerateUlid();
        media.filename = file.path.filename().string();
        media.path = StoredMediaPath(
            file.given.empty() ? file.path : file.given, projectDirectory);
        media.bin_id = targetBin ? *targetBin : Ulid{};
        media.proxy_path.clear();
        known.emplace(file.path.string(),
                      libraryCount + plan.operation.added.size());
        outcome.added_ids.push_back(media.id);
        plan.operation.added.push_back(std::move(media));
    }
    outcome.changed = plan.HasJournalEntry() || !plan.completed.empty() ||
                      !plan.mounted.empty();
    return plan;
}

void ApplyIngestRepairs(Document& document, const IngestPlan& plan) {
    for (const LibraryMedia& completed : plan.completed) {
        if (LibraryMedia* existing = document.FindLibraryMedia(completed.id)) {
            *existing = completed;
        }
    }
    for (const Ulid& id : plan.mounted) {
        const LibraryMedia* media = document.FindLibraryMedia(id);
        if (media != nullptr && document.FindSource(id) == nullptr) {
            document.sources.push_back(
                {media->id, media->path, media->rate, media->duration});
        }
    }
}

IngestOutcome AddProbedMedia(Document& document,
                             const std::filesystem::path& projectDirectory,
                             const std::vector<ProbedIngestFile>& files,
                             const std::optional<Ulid>& targetBin) {
    IngestPlan plan =
        PlanProbedMedia(document, projectDirectory, files, targetBin);
    ApplyIngestRepairs(document, plan);
    if (plan.HasJournalEntry()) {
        Operation operation = plan.operation;
        Operation inverse = RemoveIngestedMediaOperation{};
        EditError error = EditError::None;
        std::string message;
        if (!ApplyOperation(document, operation, inverse, error, message)) {
            plan.outcome.refused = message;
            plan.outcome.added_ids.clear();
            plan.outcome.moved = 0;
            plan.outcome.changed =
                !plan.completed.empty() || !plan.mounted.empty();
        }
    }
    return plan.outcome;
}

bool CollectIngestFiles(const std::string& mediaPath, bool recursive,
                        std::vector<std::filesystem::path>& files,
                        size_t& ignored, std::string& reason) {
    std::error_code pathError;
    Resolved(mediaPath, pathError);
    if (pathError) {
        reason = pathError.message();
        return false;
    }
    // LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The folder as it was named,
    // links kept: what the entries store (StoredMediaPath). The resolved
    // path above only checks that it exists.
    const std::filesystem::path namedMedia =
        std::filesystem::absolute(mediaPath, pathError).lexically_normal();
    if (pathError) {
        reason = pathError.message();
        return false;
    }
    return CollectFiles(namedMedia, recursive, files, ignored, reason);
}

bool ProbeIngestFiles(const std::vector<std::filesystem::path>& candidates,
                      std::set<std::string> settled,
                      std::vector<ProbedIngestFile>& files,
                      MediaTaskContext* context) {
    files.clear();
    files.reserve(candidates.size());
    const double count = static_cast<double>(candidates.size());
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (context != nullptr) {
            if (context->Cancelled()) {
                return false;
            }
            // Named before it is read: a long rush is measured for seconds,
            // and the task bar says which one.
            context->SetProgress(static_cast<double>(index) / count,
                                 candidates[index].filename().string());
        }
        files.push_back(ProbeIngestFile(candidates[index], settled));
        if (context != nullptr) {
            context->SetProgress(static_cast<double>(index + 1) / count,
                                 files.back().path.filename().string());
        }
    }
    return true;
}

std::string IngestTaskLabel(size_t count) {
    return "Import " + std::to_string(count) +
           (count == 1 ? " file" : " files");
}

int IngestCommandAnswer(const IngestOutcome& outcome, size_t ignored,
                        std::string& output) {
    // PRD20 -- ROADMAP.md. An ingest that read nothing is a failure, with
    // the status of one: `--ingest <project> a.txt` answered ok:true and 0,
    // its only trace an entry in `errors` no script looked at. The report
    // travels as the refusal's `result`, the same four members a success
    // has.
    if (IngestReadNothing(outcome)) {
        return FailCliCommandWithResult(
            "IoError", ReadNothingDetail(outcome.errors),
            "{" + ReportMembers(0, outcome.skipped, ignored, outcome.errors) +
                "}",
            output);
    }
    output = SuccessJson(outcome.added_ids.size(), outcome.skipped, ignored,
                         outcome.errors);
    return 0;
}

int IngestCommand(const std::string& documentPath, const std::string& mediaPath,
                  bool recursive, std::string& output,
                  const std::optional<Ulid>& targetBin) {
    av_log_set_level(AV_LOG_ERROR);
    Project project;
    std::string reason;
    if (!LoadStoredProject(documentPath, project, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }
    Document document = project.MakeActiveDocument();

    std::error_code pathError;
    Resolved(documentPath, pathError);
    if (pathError) {
        return FailCliCommand("IoError", pathError.message(), output);
    }
    std::vector<std::filesystem::path> files;
    size_t ignored = 0;
    if (!CollectIngestFiles(mediaPath, recursive, files, ignored, reason)) {
        return FailCliCommand("IoError", reason, output);
    }

    // PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The two halves the window
    // runs too (Ingest.h): every file is looked at, then what was found is
    // added. Nothing is decoded for a file the library already holds
    // complete, which is what keeps a second ingest of a folder short.
    const std::filesystem::path projectDirectory =
        std::filesystem::absolute(documentPath, pathError)
            .lexically_normal()
            .parent_path();
    // DEC32 (PRD7) -- a target bin is checked before anything is decoded:
    // a typo in an id must not cost the measure of a whole folder.
    if (targetBin && !targetBin->empty() && !document.FindBin(*targetBin)) {
        return FailCliCommand("UnknownBin",
                              "unknown bin_id '" + *targetBin + "'", output);
    }
    std::vector<ProbedIngestFile> probed;
    ProbeIngestFiles(files, CompleteLibraryPaths(document, projectDirectory),
                     probed);
    const IngestPlan plan =
        PlanProbedMedia(document, projectDirectory, probed, targetBin);
    const IngestOutcome& outcome = plan.outcome;
    ApplyIngestRepairs(document, plan);

    // DEC32 (PRD7) -- the new entries and the moves, one operation in the
    // active timeline's journal, as --apply-op writes one. The project's
    // journal is read back to be written as it is (B10).
    std::map<std::string, EditLog> logs;
    ProjectEditLog projectLog;
    if (plan.HasJournalEntry() && !IngestReadNothing(outcome)) {
        EditError editError = EditError::None;
        EditLog& log = logs[project.active_timeline_id];
        const std::string logPath = TimelineEditLogPathForProject(
            documentPath, project.active_timeline_id);
        const std::string projectLogPath =
            ProjectEditLogPathForProject(documentPath);
        std::error_code existsError;
        if ((std::filesystem::exists(logPath, existsError) &&
             !EditLog::Load(logPath, log, editError, reason)) ||
            (std::filesystem::exists(projectLogPath, existsError) &&
             !ProjectEditLog::Load(projectLogPath, projectLog, editError,
                                   reason))) {
            return FailCliCommand("ParseError", reason, output);
        }
        if (existsError) {
            return FailCliCommand("IoError", existsError.message(), output);
        }
        if (!log.Apply(document, plan.operation, editError, reason)) {
            return FailCliCommand(EditErrorName(editError), reason, output);
        }
    }

    if (!document.Validate(reason)) {
        return FailCliCommand("ValidationFailed", reason, output);
    }
    // An ingest that read nothing changed nothing: there is nothing to
    // write, and the answer refuses it.
    if (outcome.changed && !IngestReadNothing(outcome)) {
        const bool committed =
            project.CommitActiveDocument(document, reason) &&
            (logs.empty()
                 ? CommitStoredProject(documentPath, project, reason)
                 : CommitStoredProjectAndLogs(documentPath, project, logs,
                                              projectLog, reason));
        if (!committed) {
            return FailCliCommand("IoError", reason, output);
        }
    }
    return IngestCommandAnswer(outcome, ignored, output);
}
