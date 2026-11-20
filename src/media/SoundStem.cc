#include "SoundStem.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace {

bool Refuse(EditError code, std::string text, EditError& error,
            std::string& message) {
    error = code;
    message = std::move(text);
    return false;
}

uint32_t Le32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint16_t Le16(const unsigned char* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

// a / b rounded down, b > 0.
__int128 FloorDiv(__int128 a, __int128 b) {
    const __int128 quotient = a / b;
    return (a % b != 0 && a < 0) ? quotient - 1 : quotient;
}

std::string RateText(const MediaRate& rate) {
    return std::to_string(rate.num) + "/" + std::to_string(rate.den);
}

}  // namespace

bool ReadBroadcastWave(const std::string& path, BroadcastWave& wave,
                       std::string& error) {
    wave = {};
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot read '" + path + "'";
        return false;
    }
    std::array<unsigned char, 12> head{};
    if (!file.read(reinterpret_cast<char*>(head.data()), head.size())) {
        error = "'" + path + "' is too short to be a WAV file";
        return false;
    }
    const std::string riff(head.begin(), head.begin() + 4);
    const std::string kind(head.begin() + 8, head.end());
    if (riff == "RF64") {
        error = "'" + path + "' is an RF64 file, which is not read yet";
        return false;
    }
    if (riff != "RIFF" || kind != "WAVE") {
        error = "'" + path + "' is not a RIFF/WAVE file";
        return false;
    }
    bool haveFormat = false;
    bool haveData = false;
    uint16_t blockAlign = 0;
    uint64_t dataBytes = 0;
    std::array<unsigned char, 8> chunk{};
    while (file.read(reinterpret_cast<char*>(chunk.data()), chunk.size())) {
        const std::string id(chunk.begin(), chunk.begin() + 4);
        const uint32_t size = Le32(chunk.data() + 4);
        const std::streamoff next =
            static_cast<std::streamoff>(file.tellg()) + size + (size & 1);
        if (id == "fmt " || id == "bext") {
            // Both are a few hundred bytes; read them whole.
            if (size > (1u << 20)) {
                error = "'" + path + "' has a ";
                error += id + " chunk of " + std::to_string(size) + " bytes";
                return false;
            }
            std::string body(size, '\0');
            if (!file.read(body.data(), size)) {
                error = "'" + path + "' is cut short in its ";
                error += id + " chunk";
                return false;
            }
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(body.data());
            if (id == "fmt ") {
                if (size < 16) {
                    error = "'" + path + "' has a fmt chunk of " +
                            std::to_string(size) + " bytes";
                    return false;
                }
                wave.channels = Le16(bytes + 2);
                const uint32_t rate = Le32(bytes + 4);
                blockAlign = Le16(bytes + 12);
                if (rate == 0 ||
                    rate > static_cast<uint32_t>(
                               std::numeric_limits<int32_t>::max()) ||
                    blockAlign == 0 || wave.channels == 0) {
                    error = "'" + path +
                            "' declares no usable sample rate, "
                            "channel count or block size";
                    return false;
                }
                wave.sample_rate = static_cast<int32_t>(rate);
                haveFormat = true;
            } else if (size >= 346) {
                // EBU Tech 3285: TimeReferenceLow, then High, at 338.
                wave.time_reference =
                    static_cast<int64_t>(Le32(bytes + 338)) |
                    (static_cast<int64_t>(Le32(bytes + 342)) << 32);
                wave.has_time_reference = true;
            }
        } else if (id == "data") {
            dataBytes = size;
            haveData = true;
        }
        // On past the data too: a bext chunk may follow it.
        file.seekg(next);
    }
    if (!haveFormat || !haveData) {
        error = "'" + path + "' has no " +
                (haveFormat ? std::string("data") : std::string("fmt")) +
                " chunk";
        return false;
    }
    wave.samples = static_cast<int64_t>(dataBytes / blockAlign);
    error.clear();
    return true;
}

int64_t SoundStemFrameSample(int64_t frame, const MediaRate& rate,
                             int32_t sampleRate) {
    // round(frame x den x sampleRate / num), a half up: floor((2n + d) / 2d).
    const __int128 numerator = static_cast<__int128>(frame) * 2 * rate.den *
                               static_cast<__int128>(sampleRate);
    const __int128 denominator = static_cast<__int128>(rate.num) * 2;
    return static_cast<int64_t>(FloorDiv(numerator + rate.num, denominator));
}

bool BuildSoundStemPlacement(const Document& document,
                             const SoundStemRequest& request,
                             BatchOperation& batch,
                             SoundStemPlacement& placement, EditError& error,
                             std::string& message) {
    batch = {};
    placement = {};
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        return Refuse(EditError::InvalidOperation,
                      "the timeline has no usable frame rate", error, message);
    }
    const LibraryMedia* media = document.FindLibraryMedia(request.media_id);
    const DocumentSource* source = document.FindSource(request.media_id);
    if (media == nullptr || source == nullptr) {
        return Refuse(EditError::UnknownMedia,
                      "unknown media_id '" + request.media_id + "'", error,
                      message);
    }
    if (media->has_video) {
        return Refuse(EditError::InvalidOperation,
                      "media '" + media->filename +
                          "' has a picture: a stem is sound only",
                      error, message);
    }
    const BroadcastWave& wave = request.wave;
    if (wave.sample_rate <= 0 || wave.samples <= 0) {
        return Refuse(EditError::InvalidOperation,
                      "the stem's file holds no sound", error, message);
    }
    // The file read now must be the one the library measured.
    const RationalTime fileLength{wave.samples, wave.sample_rate};
    if (fileLength != source->duration) {
        return Refuse(EditError::InvalidOperation,
                      "the file of '" + media->filename + "' lasts " +
                          std::to_string(wave.samples) + " samples at " +
                          std::to_string(wave.sample_rate) +
                          " Hz, not what the library holds: ingest it again",
                      error, message);
    }
    if (!wave.has_time_reference) {
        return Refuse(EditError::InvalidOperation,
                      "'" + media->filename +
                          "' carries no BWF timecode (bext chunk): nothing "
                          "says where it goes",
                      error, message);
    }

    EdlTimecodeFormat format;
    std::string formatError;
    int64_t startFrame = 0;
    if (!EdlTimecodeFormatFor(rate, format, formatError) ||
        !ParseEdlTimecode(request.start_timecode, format, startFrame,
                          formatError)) {
        return Refuse(
            EditError::InvalidOperation,
            "start timecode '" + request.start_timecode + "': " + formatError,
            error, message);
    }

    // The frame whose sample is the time reference: one at most, since a
    // frame lasts more than a sample.
    const int64_t reference = wave.time_reference;
    const __int128 guess =
        FloorDiv(static_cast<__int128>(reference) * rate.num,
                 static_cast<__int128>(rate.den) * wave.sample_rate);
    int64_t frame = -1;
    for (__int128 candidate = guess - 1; candidate <= guess + 1; ++candidate) {
        if (candidate >= 0 &&
            SoundStemFrameSample(static_cast<int64_t>(candidate), rate,
                                 wave.sample_rate) == reference) {
            frame = static_cast<int64_t>(candidate);
            break;
        }
    }
    if (frame < 0) {
        return Refuse(EditError::InvalidTimelineIn,
                      "'" + media->filename + "' starts at sample " +
                          std::to_string(reference) + ", on no frame of a " +
                          RateText(rate) + " timeline at " +
                          std::to_string(wave.sample_rate) +
                          " Hz: it was not rendered for this timeline",
                      error, message);
    }
    if (frame < startFrame) {
        return Refuse(EditError::InvalidTimelineIn,
                      "'" + media->filename + "' starts " +
                          std::to_string(startFrame - frame) +
                          " frames before the timeline's first frame at " +
                          request.start_timecode,
                      error, message);
    }

    // The whole frames the stem covers, from its first.
    const __int128 frames =
        static_cast<__int128>(wave.samples) * rate.num /
        (static_cast<__int128>(wave.sample_rate) * rate.den);
    if (frames <= 0) {
        return Refuse(EditError::InvalidDuration,
                      "'" + media->filename + "' lasts less than a frame",
                      error, message);
    }

    const DocumentTrack* track = nullptr;
    if (!request.track_id.empty()) {
        track = document.FindTrack(request.track_id);
        if (track == nullptr) {
            return Refuse(EditError::UnknownTrack,
                          "unknown track_id '" + request.track_id + "'", error,
                          message);
        }
        if (track->kind != "audio") {
            return Refuse(EditError::InvalidOperation,
                          "track '" + track->id +
                              "' is not an audio track: the stem goes on one",
                          error, message);
        }
        if (track->locked) {
            return Refuse(EditError::LockedTrack,
                          "track '" + track->id + "' is locked", error,
                          message);
        }
    }

    placement.first_frame = frame - startFrame;
    placement.frames = static_cast<int64_t>(frames);
    placement.timeline_in = {placement.first_frame * rate.den, rate.num};
    placement.duration = {placement.frames * rate.den, rate.num};
    placement.stem_duration = fileLength;
    placement.not_laid = fileLength.sub(placement.duration);
    placement.clip_id = GenerateUlid();

    if (track == nullptr) {
        AddTrackOperation add;
        add.track_id = GenerateUlid();
        add.kind = "audio";
        int32_t index = 0;
        for (const DocumentTrack& existing : document.sequence.tracks) {
            index = std::max(index, existing.index + 1);
        }
        add.index = index;
        batch.operations.push_back(add);
        placement.track_id = add.track_id;
        placement.track_added = true;
    } else {
        placement.track_id = track->id;
    }

    PastedClip pasted;
    pasted.copied_clip_id = GenerateUlid();
    pasted.track_id = placement.track_id;
    pasted.source_id = media->id;
    pasted.source_in = {0, wave.sample_rate};
    pasted.duration = placement.duration;
    pasted.timeline_in = placement.timeline_in;
    pasted.clip_id = placement.clip_id;
    PasteClipsOperation paste;
    paste.clips = {pasted};
    batch.operations.push_back(paste);
    batch.label = "Stem posé à son timecode";
    error = EditError::None;
    message.clear();
    return true;
}
