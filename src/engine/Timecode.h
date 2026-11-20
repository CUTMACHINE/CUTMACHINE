#pragma once

// F2.5 -- ROADMAP.md. Factored out of main.mm's TimelineTimecode() so the
// existing timeline-overlay timecode label and integrated transport toolbar
// format HH:MM:SS:FF from the exact same rule instead of two copies quietly
// drifting apart. Plain C++, no AppKit -- this is a pure
// function of a RationalTime and a MediaRate, so it builds and is tested on
// a plain Linux host the same way UiTheme.h/PanelLayout.h are (see
// tests/timecode_tests.cc).
//
// This does not round or pick a frame boundary -- `time` is presumed
// already exact (typically the output of TimelineView.h's
// QuantizePlayheadPosition). It only formats.

#include "Document.h"
#include "RationalTime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

// The frames of one timecode second: the rate rounded up, matching main.mm's
// original TimelineTimecode -- 30000/1001 counts as a 30 fps clock though the
// underlying rate is fractional. Also the step of Shift+Left/Right, which
// moves by one second as in Resolve (21.1 manual, p. 870).
inline int64_t NominalTimecodeFps(MediaRate rate) {
    if (rate.num <= 0 || rate.den <= 0) {
        return 1;
    }
    return std::max<int64_t>(1, (rate.num + rate.den - 1) / rate.den);
}

inline std::string FormatTimecode(const RationalTime& time, MediaRate rate) {
    if (rate.num <= 0 || rate.den <= 0) return "00:00:00:00";
    const int64_t absoluteFrames =
        std::max<int64_t>(0, time.to_frames(rate.num, rate.den));
    const int64_t nominalFps = NominalTimecodeFps(rate);
    const int64_t frames = absoluteFrames % nominalFps;
    const int64_t totalSeconds = absoluteFrames / nominalFps;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(totalSeconds / 3600),
                  static_cast<long long>((totalSeconds / 60) % 60),
                  static_cast<long long>(totalSeconds % 60),
                  static_cast<long long>(frames));
    return std::string(buffer);
}

// Visual QA of 19 September 2026 -- the playhead steps, and its timecode
// counts, at the timeline's own rate. It used the first source of the
// library: on a 29.97 timeline of a project whose first rush was 25p, the
// playhead moved by 1/25 s and the monitor showed a frame or two behind the
// timecode, drifting between the two. The first source remains the fallback
// for a sequence without a rate, then 25 fps.
inline MediaRate PlayheadFrameRate(const Document& document) {
    const MediaRate sequence = document.sequence.frame_rate;
    if (sequence.num > 0 && sequence.den > 0) return sequence;
    if (!document.sources.empty() && document.sources.front().rate.num > 0 &&
        document.sources.front().rate.den > 0)
        return document.sources.front().rate;
    return {25, 1};
}
