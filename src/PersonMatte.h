#pragma once

// PA15 -- docs/roadmap/PARITE-2026-11.md. The roto: the people a media
// shows, cut out frame by frame. A clip whose mask has the shape "person"
// (Document.h, ClipMask) keeps its picture where this matte is white and
// lets the tracks below show where it is black.
//
// THE MATTE. One grey picture per frame of the media, the people at 255,
// the rest at 0, the edges in between. It is computed once per media by a
// segmenter the executable installs -- on macOS, Vision's person
// segmentation (PersonMatteVision.mm), on the Mac, without any remote
// service -- and kept in the package, under .cutmachine/mattes/<media>.matte.
// Every renderer reads that file: the export and read_frame through FFmpeg
// (WriteRawFrames), the monitor frame by frame (Reader). The matte is not
// part of the document: the document says only that a clip follows it, and
// a renderer that cannot find it refuses by name instead of drawing the
// whole picture.
//
// WHERE IT LIES. A matte frame covers the whole displayed picture of its
// media (rotation applied, as FFmpeg decodes it), at the aspect of that
// picture, its long side at most kMatteLongSide. Frame n of the matte is
// frame n of the media as FFmpeg decodes it from the start, one for one
// (`-fps_mode passthrough`); a renderer asks for the matte of the source
// frame it shows. A clip with a speed or a ramp reads its matte through the
// same rule as its picture.
//
// THE FILE. Little-endian: the magic "CMMATTE\0", the format version, the
// matte's width and height, the media's frame rate, the segmenter's quality
// and model; then each frame, compressed by zstd on its own (a matte is
// mostly flat: about 48 KB a frame at 576 x 1024, against 576 KB raw);
// then the offset of every frame and of the end; then the frame count, the
// offset of that index and the magic again. A frame is read by its offset
// alone, which is what lets the monitor show any frame at once.
//
// Nothing here is an operation, and nothing here judges who the people
// are: Vision finds every person in the picture, not one of them.

#include "Document.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace person_matte {

// What the executable installs: one session per media, frames handed in
// order (Vision smooths the matte from one frame to the next when it sees
// them as a sequence).
struct Segmenter {
    // A session for `quality` ("accurate", "balanced", "fast"). Null with
    // `error` when it cannot be opened.
    void* (*open)(const std::string& quality, std::string& error) = nullptr;
    // The matte of one 8-bit RGB picture, top-left origin, rows packed: an
    // 8-bit grey picture of the size the model gives, covering the whole
    // picture handed in (stretched to the model's own aspect when it has
    // one).
    bool (*segment)(void* session, int32_t width, int32_t height,
                    const uint8_t* rgb8, int32_t& matteWidth,
                    int32_t& matteHeight, std::vector<uint8_t>& matte,
                    std::string& error) = nullptr;
    void (*close)(void* session) = nullptr;
};

void SetSegmenter(const Segmenter* segmenter);
// Null when this process has none (a test that links the model alone, the
// build without a window).
const Segmenter* CurrentSegmenter();
// Installs Vision's person segmentation (PersonMatteVision.mm).
void InstallVisionSegmenter();

inline constexpr char kModel[] = "vision-person-segmentation";
inline constexpr int32_t kFormatVersion = 1;
// The long side of a matte as kept, and of the picture handed to the
// segmenter: Vision's accurate level answers 1512 x 2016 whatever it is
// given, and 1024 keeps a hair's width at 1080p.
inline constexpr int32_t kMatteLongSide = 1024;
inline constexpr int32_t kAnalysisLongSide = 1024;

inline constexpr char kQualityAccurate[] = "accurate";
inline constexpr char kQualityBalanced[] = "balanced";
inline constexpr char kQualityFast[] = "fast";

// The size a matte of a displayed picture of `width` x `height` is kept
// at: its aspect, its long side at most `longSide`, both sides even and at
// least 2.
void FittedSize(int64_t width, int64_t height, int32_t longSide,
                int32_t& outWidth, int32_t& outHeight);

// The displayed size of a media: its stored size, sides swapped by a
// rotation of a quarter turn. False when the library does not know it.
bool DisplayedSize(const LibraryMedia& media, int64_t& width, int64_t& height);

// Bilinear resampling of an 8-bit grey picture, pixel centres aligned (the
// rule FFmpeg's `scale` and a Metal sampler follow).
void Resample(int32_t sourceWidth, int32_t sourceHeight, const uint8_t* source,
              int32_t width, int32_t height, uint8_t* destination);

// Where the matte of `mediaId` is kept in a package.
std::string MattePath(const std::string& packageDirectory, const Ulid& mediaId);

struct Info {
    int32_t version = 0;
    int32_t width = 0;
    int32_t height = 0;
    MediaRate rate{0, 1};
    int64_t frames = 0;
    std::string quality;
    std::string model;
};

// Writes a matte file. Frames go to `path` + ".partial", renamed to `path`
// by Finish: a matte cut short by a failure is never read as a whole one.
class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool Open(const std::string& path, int32_t width, int32_t height,
              const MediaRate& rate, const std::string& quality,
              std::string& error);
    // One frame of width x height bytes.
    bool Add(const uint8_t* frame, std::string& error);
    bool Finish(std::string& error);
    int64_t Frames() const { return static_cast<int64_t>(offsets_.size()); }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    std::string partial_;
    int32_t width_ = 0;
    int32_t height_ = 0;
    std::vector<uint64_t> offsets_;
    uint64_t position_ = 0;
};

// Reads a matte file: any frame, by its index.
class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool Open(const std::string& path, std::string& error);
    const Info& info() const { return info_; }
    const std::string& path() const { return path_; }
    // Frame `index`, width x height bytes; an index before the first frame
    // reads the first, one past the last reads the last (a decoder that
    // gave a frame fewer than the container announced).
    bool Frame(int64_t index, std::vector<uint8_t>& frame,
               std::string& error) const;

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    Info info_;
    std::vector<uint64_t> offsets_;
};

// The header of the matte at `path`, without its frames. False with
// `error` when there is none or it cannot be read.
bool ReadInfo(const std::string& path, Info& info, std::string& error);

// Frames [first, first + count) of the matte at `mattePath`, each clamped
// to the matte (Reader::Frame), written one after the other, raw, to
// `rawPath`: what FFmpeg reads as `-f rawvideo -pix_fmt gray`.
bool WriteRawFrames(const std::string& mattePath, int64_t first, int64_t count,
                    const std::string& rawPath, std::string& error);

// Frame `index` of the matte at `mattePath` as a binary PGM at `pgmPath`:
// one picture FFmpeg reads as an input, for a render of one frame.
bool WriteFramePgm(const std::string& mattePath, int64_t index,
                   const std::string& pgmPath, std::string& error);

// The source frame a time of a media falls in: floor(time x rate), the
// frame read_frame and the monitor show at that time.
int64_t SourceFrameAt(const RationalTime& time, const MediaRate& rate);

// The FFmpeg filters that cut a picture out by its matte, as one step to
// put inline in the picture's chain, after a format with an alpha that is
// still opaque: the chain's picture is held (`null`), the matte (input
// `inputIndex`, 8-bit grey) is brought to the displayed size, then read
// through `pictureChain` -- the very filters, timing, crop and scale, the
// picture went through since its decoding, so that both arrive frame for
// frame and pixel for pixel, its lanczos scales made bilinear as the
// monitor samples -- cut to `frames` frames when given, inverted
// when asked, and made the picture's alpha (`alphamerge`). `label` must be
// unique in the graph.
std::string MatteFilter(int inputIndex, int64_t displayedWidth,
                        int64_t displayedHeight,
                        const std::string& pictureChain, bool inverted,
                        const std::string& label, int64_t frames = 0);

// The matte a clip follows, ready for a renderer: its file and its header.
// False with a message an agent can act on when the media has none, or one
// that no longer matches the media's picture.
bool MatteForMedia(const Document& document,
                   const std::string& packageDirectory, const Ulid& mediaId,
                   std::string& path, Info& info, std::string& error);

struct ComputeResult {
    std::string path;
    Info info;
    // Frames the media's metadata announced (duration times rate).
    int64_t announced_frames = 0;
    double seconds = 0.0;
};

// Computes the matte of `mediaId`: FFmpeg decodes every frame of its
// picture, displayed and fitted to kAnalysisLongSide, in order; the
// segmenter makes each one's matte; it is resampled to the kept size and
// written. `progress(done, announced)` is called after each frame and may
// return false to stop (the file is then not written). Replaces a matte the
// media already had only once the new one is whole.
bool Compute(const Document& document, const std::string& packageDirectory,
             const Ulid& mediaId, const std::string& quality,
             const std::string& ffmpegPath,
             const std::function<bool(int64_t, int64_t)>& progress,
             ComputeResult& result, std::string& error);

}  // namespace person_matte
