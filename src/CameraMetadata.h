#pragma once
// G7 -- docs/roadmap/GRADE-2026-09.md. What a camera writes about how it
// recorded a picture, read where it writes it.
//
// Sony's XAVC files (FX30, FX3, FX6...) carry a timed metadata track, codec
// tag "rtmd": one packet per frame of SMPTE KLV, whose camera unit set
// (RDD 18 local tags) holds, among the ISO and the shutter, the white
// balance the camera was set to, in Kelvin, under tag 0x810E. The XML the
// camera writes beside the file does not have it. Measured on thirty rushes
// of one FX30 across a year of shoots: 3617, 3912, 4000, 4500, 4560 and
// 6000 K, constant within a clip.
//
// G3 -- the same set names the picture's capture gamma (tag 0x3210) and
// colour primaries (0x3219) as SMPTE-registered labels, where the file's
// container tags say nothing: an FX30 in S-Log3 writes no transfer, no
// primaries and no matrix in its MP4. Only labels a file has proved are
// mapped: the pair every one of 3636 FX30 rushes wrote, whose XML sidecars
// all read "s-log3-cine" and "s-gamut3-cine". Any other label, S-Gamut3
// among them, is left unread rather than guessed from a list nobody here
// has checked against a file.
//
// Portable: no FFmpeg; Ingest.cc hands the first packet over.

#include <cstddef>
#include <cstdint>
#include <string>

// What the first camera unit set of one rtmd packet says, in the names the
// document uses (ColorManagementSettings): zero or empty for what it does
// not say.
struct SonyRtmdCamera {
    // 1000 to 50000 K; 0 when absent or outside, which no camera means.
    int32_t white_balance_kelvin = 0;
    // "sony_slog3", or empty.
    std::string transfer;
    // "sony_sgamut3_cine", or empty.
    std::string gamut;
};

// False when the packet holds no camera unit set, or one whose length runs
// past the packet.
bool ParseSonyRtmdCamera(const uint8_t* data, size_t size,
                         SonyRtmdCamera& camera);
