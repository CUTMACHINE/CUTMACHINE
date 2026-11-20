#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "Transcription.h"

#include <filesystem>
#include <string>
#include <vector>

// B7 -- ROADMAP.md. The exporter's audio filter graph is the authoritative
// description of what a timeline sounds like. This plan keeps the analogous
// decode route inspectable and lets transcription consume its PCM directly,
// never through a caller-visible movie or temporary project.
struct TimelineAudioPlan {
    std::vector<std::string> ffmpeg_arguments;
    // ITW8 -- the same assembly, for a caller that needs another output
    // than Whisper's mono 16 kHz: every input option, and the graph whose
    // [audio] pad is the stereo 48 kHz mix after the export's limiter.
    std::vector<std::string> input_arguments;
    std::string filter_graph;
    RationalTime duration{0, 1};
    size_t audio_clips = 0;
    // Changes whenever an audible clip's source boundaries, placement, gain
    // or fades change. Stored as Transcript::media_id alongside model and
    // language; the mix version also invalidates older decode pipelines.
    std::string cache_identity;
};

// ITW3 -- ROADMAP.md. One cache per reading: a verbatim transcription no
// longer overwrites the standard one, so the two readings a word check
// relies on can coexist. `cacheDirectory` is the project's `.cutmachine`.
std::filesystem::path TimelineTranscriptCachePath(
    const std::filesystem::path& cacheDirectory, const Ulid& timelineId,
    bool verbatim,
    WhisperDecodingProfile profile = WhisperDecodingProfile::Default);

bool BuildTimelineAudioPlan(const Document& document,
                            const std::filesystem::path& projectPath,
                            TimelineAudioPlan& plan, std::string& error);

// Transcribes the audible audio arrangement, atomically caching the normal
// Transcript under `cachePath`. Cache identity includes the exact audio edit;
// settings include model, language and verbatim mode.
bool TranscribeTimelineAudio(const Document& document,
                             const std::filesystem::path& projectPath,
                             const std::filesystem::path& cachePath,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, Transcript& transcript,
                             bool& cacheHit, std::string& error);
