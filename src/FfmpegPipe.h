#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ITW1/ITW4 -- ROADMAP.md. Runs FFmpeg with `arguments` (the executable
// first; empty for the binary FfmpegBinary.h resolves, PRD1) and hands each
// complete raw frame of `frameBytes` it writes to its standard output to
// `onFrame`, in order. Frames stay in memory one at a time, so a whole export
// can be scanned. Returns false with the tail of FFmpeg's error output when
// the process fails.
// `diagnostics`, when given, receives the tail of FFmpeg's error output on
// success too: where filters such as ebur128 print their summary.
bool ReadFfmpegRawFrames(std::vector<std::string> arguments, size_t frameBytes,
                         const std::function<void(const uint8_t*)>& onFrame,
                         std::string& error,
                         std::string* diagnostics = nullptr);
