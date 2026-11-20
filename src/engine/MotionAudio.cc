#include "MotionAudio.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>

namespace {

void Put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void Put32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

}  // namespace

bool WriteMotionAudioWav(const MotionPcm& pcm, const std::string& path,
                         std::string& error) {
    const uint64_t frames = pcm.left.size();
    const uint64_t bytes = frames * 2 * sizeof(float);
    if (pcm.right.size() != frames ||
        bytes > std::numeric_limits<uint32_t>::max() - 64) {
        error = "the mix is too long for a WAV file, or its channels differ";
        return false;
    }
    std::string data;
    data.reserve(static_cast<size_t>(bytes) + 44);
    data += "RIFF";
    Put32(data, static_cast<uint32_t>(36 + bytes));
    data += "WAVEfmt ";
    Put32(data, 16);
    Put16(data, 3);  // WAVE_FORMAT_IEEE_FLOAT
    Put16(data, 2);
    Put32(data, kMotionAudioRate);
    Put32(data, kMotionAudioRate * 2 * sizeof(float));
    Put16(data, 2 * sizeof(float));
    Put16(data, 32);
    data += "data";
    Put32(data, static_cast<uint32_t>(bytes));
    for (uint64_t frame = 0; frame < frames; ++frame) {
        for (const float sample : {pcm.left[frame], pcm.right[frame]}) {
            uint32_t bits = 0;
            std::memcpy(&bits, &sample, sizeof(bits));
            Put32(data, bits);
        }
    }
    const int file =
        open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file < 0) {
        error = "cannot create '" + path + "': " + std::strerror(errno);
        return false;
    }
    size_t offset = 0;
    int failure = 0;
    while (offset < data.size()) {
        const ssize_t count =
            write(file, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            failure = count < 0 ? errno : EIO;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (close(file) != 0 && failure == 0) {
        failure = errno;
    }
    if (failure != 0) {
        unlink(path.c_str());
        error = "cannot write '" + path + "': " + std::strerror(failure);
        return false;
    }
    return true;
}

bool MotionMediaMeasurer::LayoutText(const std::string&, const MotionTextStyle&,
                                     double, MotionTextLayout&,
                                     MotionError& error) {
    error = {"UnsupportedOperation", "",
             "this measurer reads media only: it lays out no text"};
    return false;
}

bool MotionMediaMeasurer::ImageSize(const std::string&, double&, double&,
                                    MotionError& error) {
    error = {"UnsupportedOperation", "",
             "this measurer reads media only: it sizes no image"};
    return false;
}

bool MotionMediaMeasurer::Path(const std::string& src, const std::string& media,
                               std::string& path, MotionError& error) const {
    std::string written = src;
    if (!media.empty()) {
        const auto found = paths_.find(media);
        if (found == paths_.end()) {
            error = {"MediaMissing", "",
                     "the project has no media '" + media + "'"};
            return false;
        }
        written = found->second;
    }
    std::filesystem::path resolved(written);
    if (resolved.is_relative() && !directory_.empty()) {
        resolved = std::filesystem::path(directory_) / resolved;
    }
    path = resolved.lexically_normal().string();
    return true;
}

bool MotionMediaMeasurer::MediaInfo(const std::string& src,
                                    const std::string& media,
                                    MotionMediaInfo& info, MotionError& error) {
    std::string path;
    std::string code;
    std::string message;
    if (!Path(src, media, path, error)) {
        return false;
    }
    if (!ProbeMotionMedia(path, info, code, message)) {
        error = {code, "", message};
        return false;
    }
    return true;
}

bool MotionMediaMeasurer::MediaSound(const std::string& src,
                                     const std::string& media,
                                     std::shared_ptr<const MotionPcm>& pcm,
                                     MotionError& error) {
    std::string path;
    std::string code;
    std::string message;
    if (!Path(src, media, path, error)) {
        return false;
    }
    if (!ReadMotionAudio(path, pcm, code, message)) {
        error = {code, "", message};
        return false;
    }
    return true;
}
