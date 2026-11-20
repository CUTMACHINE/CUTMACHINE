#pragma once

// REMOTION-PARITY-2026-09 REM2 -- docs/roadmap/REMOTION-PARITY-2026-09.md,
// docs/MOTION_FORMAT.md. The media a composition plays: the pictures of a
// `video` node and the sound of a `video` or an `audio` node, read in-process
// with FFmpeg.
//
// Portable: FFmpeg only, no framework. The evaluator decides which frame of
// which media a node shows at an instant (MotionEvaluate.cc) and which
// samples it plays (MotionAudio.h); this file only opens, seeks and decodes.
// A media's picture 0 is its first decoded picture and its sample 0 its
// first decoded sample -- the convention of every clip of a timeline
// (AudioPlayback.mm, Timeline.cc), so that the same media plays the same
// frame and the same sound in a composition as on a timeline.
//
// Decoders stay open between calls, process-wide: a composition rendered
// frame after frame, by the export or by the monitor, decodes forward
// instead of seeking for each picture. Every function is thread-safe.
//
// PERF11 -- docs/roadmap/PERF-2026-09.md. Reading a picture is two steps
// that cost differently and share differently. Decoding belongs to the one
// decoder of a file, under its lock, and is only cheap in the order of the
// file. Converting is the expensive half on a 4K rush and needs nothing
// shared. They are separate calls so that frames rendered on every core
// decode in turn, in the order of the film (MotionMediaTurns), and convert
// together.

#include "RationalTime.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct MotionMediaInfo {
    bool has_video = false;
    bool has_audio = false;
    // Displayed size: a rotation the container declares (a phone held
    // upright) is applied, as FrameCapture.h honours it.
    int32_t width = 0;
    int32_t height = 0;
    // Clockwise display rotation, 0, 90, 180 or 270.
    int32_t rotation = 0;
    // The video stream's cadence, exact; 0/1 without video.
    int32_t rate_numerator = 0;
    int32_t rate_denominator = 1;
    // How long the media lasts: its video stream's duration, or its audio
    // stream's for a sound file, exact in the stream's time base.
    RationalTime duration;
};

// 8-bit RGBA of one picture, rows from the top, straight alpha -- opaque
// unless the media carries an alpha channel -- and display-referred:
// the signal converted with its own matrix (BT.709 unless the file says
// otherwise, BT.601 for standard definition) and range, as the export
// passes a source through without colour management.
struct MotionVideoPicture {
    int32_t width = 0;
    int32_t height = 0;
    int64_t frame = 0;
    // PERF11 -- every alpha is 255. Known from the media's pixel format; read
    // from the pixels, once, only for a media that carries alpha.
    bool opaque = true;
    std::vector<uint8_t> rgba;
};

// PERF11 -- one picture as its decoder gave it, not converted yet. It owns
// its bytes: nothing of the decoder is kept alive by holding it (a reference
// would grow the decoder's buffer pool for as long as the file stays open,
// PERF10).
struct MotionDecodedPicture {
    std::string path;
    int64_t frame = 0;
    // Clockwise display rotation of the media, applied by the conversion.
    int32_t rotation = 0;
    // An FFmpeg frame; opaque here so that this header stays plain C++.
    std::shared_ptr<void> pixels;
};

// A media's whole sound, 48 kHz stereo float: what a composition mixes.
struct MotionPcm {
    std::vector<float> left;
    std::vector<float> right;
};

inline constexpr int32_t kMotionAudioRate = 48000;

// Opens `path` and reads its streams. Refused as MediaMissing when the file
// cannot be opened or holds neither pictures nor sound.
bool ProbeMotionMedia(const std::string& path, MotionMediaInfo& info,
                      std::string& code, std::string& error);

// Picture `frame` of the media, counted at its own cadence from its first
// picture. Past the last picture, the last one: a caller that asks past
// the end has already decided it wants the media held (the evaluator never
// does). Refused as MediaMissing, MediaHasNoVideo or DecodeFailed.
bool ReadMotionVideoFrame(const std::string& path, int64_t frame,
                          MotionVideoPicture& picture, std::string& code,
                          std::string& error);

// PERF11 -- the two halves of ReadMotionVideoFrame, which is one after the
// other. Decoding takes the file's lock and moves its decoder; converting
// takes no lock.
bool DecodeMotionVideoFrame(const std::string& path, int64_t frame,
                            MotionDecodedPicture& decoded, std::string& code,
                            std::string& error);
bool ConvertMotionVideoFrame(const MotionDecodedPicture& decoded,
                             MotionVideoPicture& picture, std::string& code,
                             std::string& error);

// PERF11 -- the order in which frames rendered at the same time decode
// their pictures. Each frame has an ordinal, its rank in the film; Enter(n)
// waits until every frame before n has left, so a file shared by the workers
// is asked for its pictures in the order it holds them and its decoder never
// seeks back. Without it twelve workers ask one decoder for twelve pictures
// in any order, and each one that arrives late decodes again from the key
// frame.
//
// Every ordinal from 0 leaves exactly once, entered or not: a frame that
// shows no video, or failed before reading any, leaves too. Leave is
// idempotent.
class MotionMediaTurns {
public:
    void Enter(size_t ordinal);
    void Leave(size_t ordinal);

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    // The smallest ordinal that has not left, and those past it that have.
    size_t next_ = 0;
    std::set<size_t> left_;
};

// The media's whole sound, decoded once and kept while memory allows.
// Refused as MediaMissing, MediaHasNoAudio or DecodeFailed.
bool ReadMotionAudio(const std::string& path,
                     std::shared_ptr<const MotionPcm>& pcm, std::string& code,
                     std::string& error);

// Closes every decoder and forgets every sound: for a test that rewrites a
// file under the same name, and for shutdown.
void ResetMotionMediaCache();
