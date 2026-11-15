// PA15 -- docs/roadmap/PARITE-2026-11.md. The roto's matte: its file, its
// reading, and its computation through the segmenter the executable
// installs (PersonMatte.h).

#include "PersonMatte.h"

#include "FfmpegPipe.h"

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace person_matte {

namespace {

namespace fs = std::filesystem;

constexpr char kMagic[8] = {'C', 'M', 'M', 'A', 'T', 'T', 'E', '\0'};
// The magic, the frame count and the offset of the index.
constexpr size_t kFooterBytes = 8 + 8 + 8;
// Frames are compressed fast: the matte is computed far slower than zstd
// writes it, and a stronger level gains five per cent (measured on the
// bench rush, 13 November 2026).
constexpr int kZstdLevel = 3;

std::atomic<const Segmenter*>& SegmenterSlot() {
    static std::atomic<const Segmenter*> slot{nullptr};
    return slot;
}

void PutU32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xff));
}

void PutU64(std::string& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<char>((value >> shift) & 0xff));
}

uint32_t GetU32(const unsigned char* bytes) {
    uint32_t value = 0;
    for (int index = 3; index >= 0; --index)
        value = (value << 8) | bytes[index];
    return value;
}

uint64_t GetU64(const unsigned char* bytes) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index)
        value = (value << 8) | bytes[index];
    return value;
}

bool Seek(std::FILE* file, uint64_t offset) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
}

bool ReadExactly(std::FILE* file, void* buffer, size_t bytes) {
    return std::fread(buffer, 1, bytes, file) == bytes;
}

std::string MediaPathOf(const std::string& packageDirectory,
                        const LibraryMedia& media) {
    fs::path path(media.path);
    if (path.is_relative()) path = fs::path(packageDirectory) / path;
    return path.lexically_normal().string();
}

// The header and the index of an open matte file.
bool ReadHeader(std::FILE* file, Info& info, std::vector<uint64_t>* offsets,
                std::string& error) {
    unsigned char fixed[8 + 4 * 6];
    if (!Seek(file, 0) || !ReadExactly(file, fixed, sizeof(fixed)) ||
        std::memcmp(fixed, kMagic, sizeof(kMagic)) != 0) {
        error = "not a matte file";
        return false;
    }
    info.version = static_cast<int32_t>(GetU32(fixed + 8));
    if (info.version != kFormatVersion) {
        error = "matte format version " + std::to_string(info.version) +
                " is not the one this build reads (" +
                std::to_string(kFormatVersion) + ")";
        return false;
    }
    info.width = static_cast<int32_t>(GetU32(fixed + 12));
    info.height = static_cast<int32_t>(GetU32(fixed + 16));
    info.rate.num = static_cast<int32_t>(GetU32(fixed + 20));
    info.rate.den = static_cast<int32_t>(GetU32(fixed + 24));
    const uint32_t textBytes = GetU32(fixed + 28);
    if (info.width <= 0 || info.height <= 0 || info.rate.num <= 0 ||
        info.rate.den <= 0 || textBytes > 4096) {
        error = "the matte's header is damaged";
        return false;
    }
    std::string text(textBytes, '\0');
    if (textBytes > 0 && !ReadExactly(file, text.data(), textBytes)) {
        error = "the matte's header is cut short";
        return false;
    }
    const size_t split = text.find('\n');
    info.quality = text.substr(0, split);
    info.model = split == std::string::npos ? "" : text.substr(split + 1);

    if (fseeko(file, 0, SEEK_END) != 0) {
        error = "the matte file cannot be measured";
        return false;
    }
    const off_t size = ftello(file);
    unsigned char footer[kFooterBytes];
    if (size < static_cast<off_t>(kFooterBytes) ||
        !Seek(file, static_cast<uint64_t>(size) - kFooterBytes) ||
        !ReadExactly(file, footer, sizeof(footer)) ||
        std::memcmp(footer + 16, kMagic, sizeof(kMagic)) != 0) {
        error = "the matte file has no index: it was not written to its end";
        return false;
    }
    info.frames = static_cast<int64_t>(GetU64(footer));
    const uint64_t indexOffset = GetU64(footer + 8);
    const uint64_t indexBytes = (static_cast<uint64_t>(info.frames) + 1) * 8;
    if (info.frames <= 0 || indexOffset + indexBytes + kFooterBytes !=
                                static_cast<uint64_t>(size)) {
        error = "the matte's index is damaged";
        return false;
    }
    if (offsets == nullptr) return true;
    std::vector<unsigned char> raw(indexBytes);
    if (!Seek(file, indexOffset) ||
        !ReadExactly(file, raw.data(), raw.size())) {
        error = "the matte's index cannot be read";
        return false;
    }
    offsets->resize(static_cast<size_t>(info.frames) + 1);
    for (size_t index = 0; index < offsets->size(); ++index)
        (*offsets)[index] = GetU64(raw.data() + index * 8);
    for (size_t index = 1; index < offsets->size(); ++index) {
        if ((*offsets)[index] < (*offsets)[index - 1] ||
            (*offsets)[index] > indexOffset) {
            error = "the matte's index is damaged";
            return false;
        }
    }
    return true;
}

}  // namespace

void SetSegmenter(const Segmenter* segmenter) {
    SegmenterSlot().store(segmenter);
}

const Segmenter* CurrentSegmenter() { return SegmenterSlot().load(); }

void FittedSize(int64_t width, int64_t height, int32_t longSide,
                int32_t& outWidth, int32_t& outHeight) {
    outWidth = 2;
    outHeight = 2;
    if (width <= 0 || height <= 0 || longSide < 2) return;
    const int64_t longest = std::max(width, height);
    const double scale =
        longest > longSide ? static_cast<double>(longSide) / longest : 1.0;
    const auto even = [](double value) {
        const int64_t rounded = static_cast<int64_t>(std::llround(value / 2.0));
        return static_cast<int32_t>(std::max<int64_t>(1, rounded) * 2);
    };
    outWidth = even(static_cast<double>(width) * scale);
    outHeight = even(static_cast<double>(height) * scale);
}

bool DisplayedSize(const LibraryMedia& media, int64_t& width, int64_t& height) {
    width = media.width;
    height = media.height;
    if (width <= 0 || height <= 0) return false;
    const int32_t turn = ((media.rotation_degrees % 360) + 360) % 360;
    if (turn == 90 || turn == 270) std::swap(width, height);
    return true;
}

void Resample(int32_t sourceWidth, int32_t sourceHeight, const uint8_t* source,
              int32_t width, int32_t height, uint8_t* destination) {
    if (sourceWidth == width && sourceHeight == height) {
        std::memcpy(destination, source,
                    static_cast<size_t>(width) * static_cast<size_t>(height));
        return;
    }
    const double scaleX = static_cast<double>(sourceWidth) / width;
    const double scaleY = static_cast<double>(sourceHeight) / height;
    for (int32_t y = 0; y < height; ++y) {
        // The centre of destination pixel y, in source pixels.
        const double sy = std::clamp((y + 0.5) * scaleY - 0.5, 0.0,
                                     static_cast<double>(sourceHeight - 1));
        const int32_t y0 = static_cast<int32_t>(sy);
        const int32_t y1 = std::min(y0 + 1, sourceHeight - 1);
        const double fy = sy - y0;
        const uint8_t* row0 = source + static_cast<size_t>(y0) * sourceWidth;
        const uint8_t* row1 = source + static_cast<size_t>(y1) * sourceWidth;
        uint8_t* out = destination + static_cast<size_t>(y) * width;
        for (int32_t x = 0; x < width; ++x) {
            const double sx = std::clamp((x + 0.5) * scaleX - 0.5, 0.0,
                                         static_cast<double>(sourceWidth - 1));
            const int32_t x0 = static_cast<int32_t>(sx);
            const int32_t x1 = std::min(x0 + 1, sourceWidth - 1);
            const double fx = sx - x0;
            const double top = row0[x0] + (row0[x1] - row0[x0]) * fx;
            const double bottom = row1[x0] + (row1[x1] - row1[x0]) * fx;
            out[x] = static_cast<uint8_t>(
                std::clamp(std::lround(top + (bottom - top) * fy), 0L, 255L));
        }
    }
}

std::string MattePath(const std::string& packageDirectory,
                      const Ulid& mediaId) {
    return (fs::path(packageDirectory) / ".cutmachine" / "mattes" /
            (mediaId + ".matte"))
        .string();
}

// ---------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------

Writer::~Writer() {
    if (file_ != nullptr) {
        std::fclose(file_);
        std::error_code ignored;
        fs::remove(partial_, ignored);
    }
}

bool Writer::Open(const std::string& path, int32_t width, int32_t height,
                  const MediaRate& rate, const std::string& quality,
                  std::string& error) {
    if (width <= 0 || height <= 0 || rate.num <= 0 || rate.den <= 0) {
        error = "a matte needs a size and a frame rate";
        return false;
    }
    path_ = path;
    partial_ = path + ".partial";
    width_ = width;
    height_ = height;
    offsets_.clear();
    std::error_code made;
    fs::create_directories(fs::path(path).parent_path(), made);
    file_ = std::fopen(partial_.c_str(), "wb");
    if (file_ == nullptr) {
        error = "unable to write '" + partial_ + "'";
        return false;
    }
    std::string header(kMagic, sizeof(kMagic));
    PutU32(header, static_cast<uint32_t>(kFormatVersion));
    PutU32(header, static_cast<uint32_t>(width));
    PutU32(header, static_cast<uint32_t>(height));
    PutU32(header, static_cast<uint32_t>(rate.num));
    PutU32(header, static_cast<uint32_t>(rate.den));
    const std::string text = quality + "\n" + kModel;
    PutU32(header, static_cast<uint32_t>(text.size()));
    header += text;
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
        error = "unable to write '" + partial_ + "'";
        return false;
    }
    position_ = header.size();
    return true;
}

bool Writer::Add(const uint8_t* frame, std::string& error) {
    if (file_ == nullptr) {
        error = "the matte file is not open";
        return false;
    }
    const size_t bytes = static_cast<size_t>(width_) * height_;
    std::string compressed(ZSTD_compressBound(bytes), '\0');
    const size_t written = ZSTD_compress(compressed.data(), compressed.size(),
                                         frame, bytes, kZstdLevel);
    if (ZSTD_isError(written)) {
        error = std::string("unable to compress a matte frame: ") +
                ZSTD_getErrorName(written);
        return false;
    }
    if (std::fwrite(compressed.data(), 1, written, file_) != written) {
        error = "unable to write '" + partial_ + "'";
        return false;
    }
    offsets_.push_back(position_);
    position_ += written;
    return true;
}

bool Writer::Finish(std::string& error) {
    if (file_ == nullptr) {
        error = "the matte file is not open";
        return false;
    }
    if (offsets_.empty()) {
        error = "a matte without a frame";
        return false;
    }
    std::string tail;
    const uint64_t indexOffset = position_;
    for (const uint64_t offset : offsets_) PutU64(tail, offset);
    PutU64(tail, position_);
    PutU64(tail, static_cast<uint64_t>(offsets_.size()));
    PutU64(tail, indexOffset);
    tail.append(kMagic, sizeof(kMagic));
    const bool wrote =
        std::fwrite(tail.data(), 1, tail.size(), file_) == tail.size();
    const bool closed = std::fclose(file_) == 0;
    file_ = nullptr;
    std::error_code renamed;
    if (wrote && closed) fs::rename(partial_, path_, renamed);
    if (!wrote || !closed || renamed) {
        std::error_code ignored;
        fs::remove(partial_, ignored);
        error = "unable to finish '" + path_ + "'";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------

Reader::~Reader() {
    if (file_ != nullptr) std::fclose(file_);
}

bool Reader::Open(const std::string& path, std::string& error) {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    path_ = path;
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        error = "no matte at '" + path + "'";
        return false;
    }
    if (!ReadHeader(file_, info_, &offsets_, error)) {
        std::fclose(file_);
        file_ = nullptr;
        error = "'" + path + "': " + error;
        return false;
    }
    return true;
}

bool Reader::Frame(int64_t index, std::vector<uint8_t>& frame,
                   std::string& error) const {
    if (file_ == nullptr) {
        error = "the matte file is not open";
        return false;
    }
    const int64_t clamped = std::clamp<int64_t>(index, 0, info_.frames - 1);
    const uint64_t begin = offsets_[static_cast<size_t>(clamped)];
    const uint64_t end = offsets_[static_cast<size_t>(clamped) + 1];
    std::vector<char> compressed(static_cast<size_t>(end - begin));
    if (!Seek(file_, begin) ||
        !ReadExactly(file_, compressed.data(), compressed.size())) {
        error = "matte frame " + std::to_string(clamped) + " cannot be read";
        return false;
    }
    const size_t bytes = static_cast<size_t>(info_.width) * info_.height;
    frame.resize(bytes);
    const size_t read = ZSTD_decompress(frame.data(), bytes, compressed.data(),
                                        compressed.size());
    if (ZSTD_isError(read) || read != bytes) {
        error = "matte frame " + std::to_string(clamped) + " is damaged";
        return false;
    }
    return true;
}

bool ReadInfo(const std::string& path, Info& info, std::string& error) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "no matte at '" + path + "'";
        return false;
    }
    const bool read = ReadHeader(file, info, nullptr, error);
    std::fclose(file);
    if (!read) error = "'" + path + "': " + error;
    return read;
}

bool WriteRawFrames(const std::string& mattePath, int64_t first, int64_t count,
                    const std::string& rawPath, std::string& error) {
    Reader reader;
    if (!reader.Open(mattePath, error)) return false;
    if (count <= 0) {
        error = "no matte frame asked for";
        return false;
    }
    std::FILE* out = std::fopen(rawPath.c_str(), "wb");
    if (out == nullptr) {
        error = "unable to write '" + rawPath + "'";
        return false;
    }
    std::vector<uint8_t> frame;
    for (int64_t index = 0; index < count; ++index) {
        if (!reader.Frame(first + index, frame, error) ||
            std::fwrite(frame.data(), 1, frame.size(), out) != frame.size()) {
            if (error.empty()) error = "unable to write '" + rawPath + "'";
            std::fclose(out);
            std::error_code ignored;
            fs::remove(rawPath, ignored);
            return false;
        }
    }
    if (std::fclose(out) != 0) {
        error = "unable to write '" + rawPath + "'";
        return false;
    }
    return true;
}

bool WriteFramePgm(const std::string& mattePath, int64_t index,
                   const std::string& pgmPath, std::string& error) {
    Reader reader;
    std::vector<uint8_t> frame;
    if (!reader.Open(mattePath, error) || !reader.Frame(index, frame, error))
        return false;
    std::FILE* out = std::fopen(pgmPath.c_str(), "wb");
    if (out == nullptr) {
        error = "unable to write '" + pgmPath + "'";
        return false;
    }
    const std::string header = "P5\n" + std::to_string(reader.info().width) +
                               " " + std::to_string(reader.info().height) +
                               "\n255\n";
    const bool wrote =
        std::fwrite(header.data(), 1, header.size(), out) == header.size() &&
        std::fwrite(frame.data(), 1, frame.size(), out) == frame.size();
    const bool closed = std::fclose(out) == 0;
    if (!wrote || !closed) {
        std::error_code ignored;
        fs::remove(pgmPath, ignored);
        error = "unable to write '" + pgmPath + "'";
        return false;
    }
    return true;
}

int64_t SourceFrameAt(const RationalTime& time, const MediaRate& rate) {
    if (time.rate <= 0 || rate.num <= 0 || rate.den <= 0) return 0;
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    __int128 frame = numerator / denominator;
    if (numerator % denominator != 0 && (numerator < 0) != (denominator < 0))
        --frame;
    return static_cast<int64_t>(frame);
}

std::string MatteFilter(int inputIndex, int64_t displayedWidth,
                        int64_t displayedHeight,
                        const std::string& pictureChain, bool inverted,
                        const std::string& label, int64_t frames) {
    std::string filter =
        "null[pmp" + label + "];[" + std::to_string(inputIndex) +
        ":v:0]scale=" + std::to_string(displayedWidth) + ":" +
        std::to_string(displayedHeight) + ":flags=bilinear,format=gray";
    // The picture's own scales, but bilinear: an alpha has no detail a
    // lanczos kernel would keep, only an edge it would ring around, and the
    // monitor samples the matte bilinearly (shader.metal).
    std::string chain = pictureChain;
    for (size_t at = chain.find("flags=lanczos"); at != std::string::npos;
         at = chain.find("flags=lanczos", at))
        chain.replace(at, 13, "flags=bilinear");
    if (!chain.empty()) filter += "," + chain;
    if (frames > 0) filter += ",trim=end_frame=" + std::to_string(frames);
    if (inverted) filter += ",negate";
    filter += ",format=gray[pmm" + label + "];[pmp" + label + "][pmm" + label +
              "]alphamerge";
    return filter;
}

bool MatteForMedia(const Document& document,
                   const std::string& packageDirectory, const Ulid& mediaId,
                   std::string& path, Info& info, std::string& error) {
    const LibraryMedia* media = document.FindLibraryMedia(mediaId);
    if (media == nullptr) {
        error = "unknown media '" + mediaId + "'";
        return false;
    }
    path = MattePath(packageDirectory, mediaId);
    std::error_code exists;
    if (!fs::exists(path, exists)) {
        error = "media '" + mediaId +
                "' has no matte of its people: compute it once with "
                "analyze_person_matte";
        return false;
    }
    if (!ReadInfo(path, info, error)) return false;
    int64_t width = 0;
    int64_t height = 0;
    int32_t keptWidth = 0;
    int32_t keptHeight = 0;
    if (DisplayedSize(*media, width, height)) {
        FittedSize(width, height, kMatteLongSide, keptWidth, keptHeight);
        if (keptWidth != info.width || keptHeight != info.height) {
            error = "the matte of media '" + mediaId + "' is " +
                    std::to_string(info.width) + "x" +
                    std::to_string(info.height) +
                    ", not the shape of its picture any more: compute it "
                    "again with analyze_person_matte";
            return false;
        }
    }
    if (media->rate.num > 0 && media->rate.den > 0 &&
        static_cast<int64_t>(media->rate.num) * info.rate.den !=
            static_cast<int64_t>(info.rate.num) * media->rate.den) {
        error = "the matte of media '" + mediaId +
                "' was computed at another frame rate: compute it again "
                "with analyze_person_matte";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Computing a matte
// ---------------------------------------------------------------------

bool Compute(const Document& document, const std::string& packageDirectory,
             const Ulid& mediaId, const std::string& quality,
             const std::string& ffmpegPath,
             const std::function<bool(int64_t, int64_t)>& progress,
             ComputeResult& result, std::string& error) {
    result = {};
    const Segmenter* segmenter = CurrentSegmenter();
    if (segmenter == nullptr || segmenter->open == nullptr ||
        segmenter->segment == nullptr || segmenter->close == nullptr) {
        error =
            "no person segmenter in this process: the roto needs Vision, "
            "which only the macOS cutmachine executable installs, not the "
            "build without a window (headless/)";
        return false;
    }
    if (quality != kQualityAccurate && quality != kQualityBalanced &&
        quality != kQualityFast) {
        error = "quality is \"accurate\", \"balanced\" or \"fast\"";
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(mediaId);
    if (media == nullptr) {
        error = "unknown media '" + mediaId + "'";
        return false;
    }
    if (!media->has_video) {
        error = "media '" + mediaId + "' has no picture";
        return false;
    }
    if (media->color_input.graphics) {
        error = "media '" + mediaId +
                "' is a graphic: its people are drawn, not filmed";
        return false;
    }
    int64_t displayedWidth = 0;
    int64_t displayedHeight = 0;
    if (!DisplayedSize(*media, displayedWidth, displayedHeight) ||
        media->rate.num <= 0 || media->rate.den <= 0) {
        error = "media '" + mediaId + "' has no known size or frame rate";
        return false;
    }
    const std::string source = MediaPathOf(packageDirectory, *media);
    std::error_code exists;
    if (!fs::exists(source, exists)) {
        error =
            "the file of media '" + mediaId + "' is missing: '" + source + "'";
        return false;
    }
    int32_t analysisWidth = 0;
    int32_t analysisHeight = 0;
    FittedSize(displayedWidth, displayedHeight, kAnalysisLongSide,
               analysisWidth, analysisHeight);
    int32_t keptWidth = 0;
    int32_t keptHeight = 0;
    FittedSize(displayedWidth, displayedHeight, kMatteLongSide, keptWidth,
               keptHeight);
    if (media->duration.rate > 0) {
        result.announced_frames =
            media->duration.to_frames(media->rate.num, media->rate.den);
    }

    const std::string path = MattePath(packageDirectory, mediaId);
    Writer writer;
    if (!writer.Open(path, keptWidth, keptHeight, media->rate, quality, error))
        return false;
    std::string sessionError;
    void* session = segmenter->open(quality, sessionError);
    if (session == nullptr) {
        error = "the person segmenter could not start: " + sessionError;
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    bool stopped = false;
    std::string frameError;
    std::vector<uint8_t> matte;
    std::vector<uint8_t> kept(static_cast<size_t>(keptWidth) * keptHeight);
    const size_t frameBytes =
        static_cast<size_t>(analysisWidth) * analysisHeight * 3;
    // One run of FFmpeg for the whole media, every decoded frame passed
    // through as it comes: frame n of the matte is frame n of the media.
    const std::vector<std::string> arguments = {
        ffmpegPath,
        "-nostdin",
        "-v",
        "error",
        "-i",
        source,
        "-map",
        "0:v:0",
        "-an",
        "-sn",
        "-dn",
        "-fps_mode",
        "passthrough",
        "-vf",
        "scale=" + std::to_string(analysisWidth) + ":" +
            std::to_string(analysisHeight) + ":flags=bilinear,format=rgb24",
        "-f",
        "rawvideo",
        "-"};
    std::string decodeError;
    const bool decoded = ReadFfmpegRawFrames(
        arguments, frameBytes,
        [&](const uint8_t* frame) {
            if (stopped || !frameError.empty()) return;
            int32_t matteWidth = 0;
            int32_t matteHeight = 0;
            if (!segmenter->segment(session, analysisWidth, analysisHeight,
                                    frame, matteWidth, matteHeight, matte,
                                    frameError)) {
                frameError = "frame " + std::to_string(writer.Frames()) + ": " +
                             frameError;
                return;
            }
            if (matteWidth <= 0 || matteHeight <= 0 ||
                matte.size() != static_cast<size_t>(matteWidth) * matteHeight) {
                frameError = "frame " + std::to_string(writer.Frames()) +
                             ": the segmenter gave a matte of no size";
                return;
            }
            Resample(matteWidth, matteHeight, matte.data(), keptWidth,
                     keptHeight, kept.data());
            if (!writer.Add(kept.data(), frameError)) return;
            if (progress && !progress(writer.Frames(), result.announced_frames))
                stopped = true;
        },
        decodeError);
    segmenter->close(session);
    if (stopped) {
        error = "stopped before the matte was whole: nothing was kept";
        return false;
    }
    if (!frameError.empty()) {
        error = "the person segmenter failed: " + frameError;
        return false;
    }
    if (!decoded) {
        error = "FFmpeg could not read media '" + mediaId + "': " + decodeError;
        return false;
    }
    if (!writer.Finish(error)) return false;
    result.path = path;
    result.seconds = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    return ReadInfo(path, result.info, error);
}

}  // namespace person_matte
