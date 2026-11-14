#pragma once

#include "Document.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

// Realtime timeline audio for macOS. Media is decoded once to a common 16-bit
// stereo format at 48 kHz; the output callback only reads immutable mix plans
// and never touches the editable Document.
class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    bool Open(const Document& document, const std::string& baseDirectory,
              std::string& error);
    // Returns as soon as the mix plan is published for the sources already
    // decoded. Sources this timeline needs and does not have yet are decoded
    // on a background thread and join the mix as they land -- see
    // PERF-2026-09 in AudioPlayback.mm for why that is the tolerated state
    // rather than a new one.
    void RebuildTimeline(const Document& document);
    // Blocks until nothing is left to decode, or the timeout elapses. For
    // tests and for shutdown paths that need a settled mix; the editor never
    // waits on this.
    bool WaitForDecodes(int timeoutMilliseconds);
    // Both open the output if it is closed, which blocks the caller for as
    // long as the device takes to start.
    bool PlayFrom(RationalTime position, int direction, std::string& error);
    bool ScrubAt(RationalTime position, std::string& error);
    // PRD37 -- ROADMAP.md. Silences the output and leaves it open, so the
    // next play or scrub starts at once: closing an output and opening it
    // again straight after blocks for 100 to 200 ms.
    void Stop();
    // Closes an output left silent for playback::kAudioOutputIdleGrace
    // (PlaybackPresentation.h); an open output keeps the Mac from sleeping.
    // The editor calls it from its display timer.
    void ReleaseIdleOutput(std::chrono::steady_clock::time_point now);
    // For tests: whether the output is open, and how many times it opened.
    bool OutputOpen() const;
    uint64_t OutputStartCount() const;
    size_t DecodedSourceCount() const;
    // What the decoded sources hold in memory, capacity included.
    size_t DecodedBytes() const;
    size_t PlannedClipCount() const;
    uint64_t ScrubTriggerCount() const;
    int ShuttleSpeed() const;

private:
    void DecodeLoop();
    void EnsureDecoderStarted();
    bool EnsureOutputStarted(const char* failure, std::string& error);

    struct Impl;
    Impl* impl_;
};
