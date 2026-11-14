#pragma once

// PERF13 -- docs/roadmap/PERF-2026-09.md. The sound of one rush, decoded
// whole for the monitor's mix (AudioPlayback.mm), and only its sound.
//
// The decode used to read every packet of the file and drop the ones that
// were not audio: on a 4K XAVC S-I rush, 240 Mb/s of picture for 1.5 Mb/s
// of sound, it read the whole file to keep less than one byte in a hundred.
// Measured on 4 October 2026 on the external drive projects are cut from
// (USB, exFAT, 41 MB/s for one reader), for the five rushes an interview's
// timeline hears, one after the other as the decode thread takes them:
// 4,469 MB read in 110 s, during which playback shared the drive. With the
// other streams discarded, the demuxer skips their samples without reading
// them: 55 MB in 5.4 s, the samples identical bit for bit. The sound is
// interleaved in half-second blocks there (92 KB of sound between 14 MB of
// picture), so reading it alone is one seek per block -- far cheaper than
// streaming the picture between them, even on a spinning disk.
//
// Plain C++ over FFmpeg, no AppKit and no AVFAudio, so it runs on a plain
// Linux host. See tests/audio_decode_tests.cc.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace audio_decode {

// A decoded rush, whole, held for the mix. 16-bit rather than float: this
// is the monitor's sound, the export has its own path, and 16 bits put the
// quantisation 96 dB down while halving what a rush holds -- a 30-minute
// interview rush was 690 MB of float stereo, three cameras of one interview
// 2 GB before the first frame played.
struct StereoPcm16 {
    std::vector<int16_t> left;
    std::vector<int16_t> right;
};

// What one decode read from its file.
struct DecodeStats {
    int64_t bytes_read = 0;
};

// The best audio stream of `path`, resampled to `sampleRate` stereo. Every
// other stream is discarded before the first packet is read, so a demuxer
// that can skip a sample without reading it (MP4 and QuickTime: an index
// says where each one lies) reads the sound and nothing else. `cancel` lets
// the caller abandon a rush in progress: closing a project used to wait for
// the whole file. Fails with "no audio stream" for a file without sound, and
// with "audio decode cancelled" once `cancel` is set.
bool DecodeStereoPcm16(const std::string& path, int32_t sampleRate,
                       StereoPcm16& output, std::string& error,
                       const std::atomic_bool* cancel = nullptr,
                       DecodeStats* stats = nullptr);

}  // namespace audio_decode
