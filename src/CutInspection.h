#pragma once

// IA4 -- docs/roadmap/IA-2026-09.md. Inspecting the montage as it is
// delivered, around one cut.
//
// The sheets decode one rush per cell and apply the format and the global
// colour transform (TimelineSheets.h): they say what the SOURCE holds. They
// do not prove the result -- layers, clip effects, transitions, captions,
// the look, the mixed sound. An agent that judges a cut on them judges
// something the film does not show; the first REEL film took four passes of
// re-reading an image exported from Resolve before its framings were right.
//
// So an inspection renders a short window around a cut through the export's
// own graph (Export.h, `window_first`/`window_end`, lossless), and reads
// back: composed frames at exact positions, the mixed sound's levels frame by
// frame, the words the montage is expected to say there, and -- when a local
// Whisper model is configured and asked for -- the words a fresh reading of
// that mix hears. Differences between the two are ALERTS TO EXAMINE, never a
// verdict: a weak question can be audible to a viewer and not to the model,
// and a model can hear a word that is not there.
//
// This header is the portable half: which window and which frames, the
// level arithmetic and the word comparison. No FFmpeg, no disk, no AppKit
// (tests/cut_inspection_tests.cc, label `portable`). The render lives in
// CutInspectionRender.h.

#include "Document.h"
#include "InterviewShort.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Bounds of a request. A window is a look at a cut, not a way to export a
// sequence: five seconds on either side is more than any editorial check of
// a cut has used, and keeps a render in seconds.
inline constexpr int64_t kCutInspectionMaxSideSeconds = 5;
inline constexpr int32_t kCutInspectionMinImages = 2;
inline constexpr int32_t kCutInspectionMaxImages = 12;
// The stretch whose level is compared on either side of the cut.
inline constexpr int64_t kCutInspectionLevelMilliseconds = 200;
// Below this the mix is reported as silence rather than a number.
inline constexpr int64_t kCutInspectionSilenceDbfs = -120;

struct CutInspectionRequest {
    // The cut to look at: the head of `clip_id`, or its tail when
    // `at_tail`. Without a clip, `position` on the timeline, snapped to
    // nothing: the window is centred on that exact frame.
    Ulid clip_id;
    bool at_tail = false;
    std::optional<RationalTime> position;
    // How much of the montage to show before and after the cut. Whole
    // frames of the sequence, each at most kCutInspectionMaxSideSeconds.
    RationalTime before{1, 1};
    RationalTime after{1, 1};
    // Frames returned as pictures, the two around the cut always among them.
    int32_t images = 6;
};

struct CutInspectionPlan {
    Ulid timeline_id;
    MediaRate rate{25, 1};
    // Sequence frames: the window is [first, end), and `cut` is the first
    // frame after the cut -- the incoming clip's first frame, or the frame
    // asked for by position. first <= cut <= end.
    int64_t first = 0;
    int64_t end = 0;
    int64_t cut = 0;
    // The window as it was asked, before the timeline's edges clipped it.
    bool clipped_at_start = false;
    bool clipped_at_end = false;
    // The frames returned as pictures, ascending, all inside the window.
    std::vector<int64_t> shown;
};

// Resolves a request into exact frames. Refuses, by name: an unknown clip,
// a clip on no video or audio track of this timeline, a position outside
// the timeline, a side that is not a whole number of frames or exceeds the
// bound, and an image count outside its bounds. A window that would leave
// the timeline is clipped to it, and the plan says so.
bool PlanCutInspection(const Document& document,
                       const CutInspectionRequest& request,
                       CutInspectionPlan& plan, std::string& error);

// The mixed sound of the window, measured frame by frame. Levels are dBFS
// rounded to the integer, the unit ITW8 reports in, and
// kCutInspectionSilenceDbfs for digital silence.
struct CutInspectionAudio {
    int32_t sample_rate = 48000;
    // One entry per frame of the window, over both channels.
    std::vector<int64_t> frame_rms_dbfs;
    std::vector<int64_t> frame_peak_dbfs;
    int64_t window_peak_dbfs = kCutInspectionSilenceDbfs;
    // The kCutInspectionLevelMilliseconds just before and just after the
    // cut, or what the window holds of them. Absent when the window holds
    // nothing on that side.
    std::optional<int64_t> before_cut_rms_dbfs;
    std::optional<int64_t> after_cut_rms_dbfs;
};

// `samples` is the window's mix, interleaved stereo float, as the lossless
// render stores it. Frame k of the window starts at the sample nearest to
// k / rate, the rounding the export's `atrim` uses.
bool MeasureCutInspectionAudio(const std::vector<float>& samples,
                               int32_t sampleRate,
                               const CutInspectionPlan& plan,
                               CutInspectionAudio& audio, std::string& error);

// One word of the montage in the window, on the timeline.
struct CutInspectionWord {
    std::string text;
    RationalTime timeline_in{0, 1};
    RationalTime duration{0, 1};
    // An expected word that crossed an edit boundary: a cut ran through
    // it (TimelineTranscriptSpan::straddles_cut).
    bool crosses_cut = false;
    // Only partly inside the window: a reading of the window cannot be
    // expected to hear it, so it is shown and never compared.
    bool at_window_edge = false;
};

struct CutInspectionAlert {
    // "word_crosses_cut", "expected_not_heard", "heard_not_expected".
    std::string kind;
    std::string text;
    RationalTime timeline_in{0, 1};
};

struct CutInspectionWords {
    std::vector<CutInspectionWord> expected;
    // Why no word is expected, when the project has no transcript to say
    // it: said, rather than read as "the montage is silent here".
    std::string expected_unavailable;
    // False when no reading of the window was made: nothing is then
    // compared, and no alert says a word was not heard.
    bool transcribed = false;
    std::vector<CutInspectionWord> heard;
    std::vector<CutInspectionAlert> alerts;
};

// The expected words of the window, from one-word spans in timeline order
// (BuildTimelineTranscriptWordSpans), each flagged when a cut runs through it
// or when it only partly lies in the window. Adds a word_crosses_cut alert
// for every word a cut runs through. Sets `transcribed` to false.
void CollectExpectedWords(const std::vector<TimelineTranscriptSpan>& spans,
                          const CutInspectionPlan& plan,
                          CutInspectionWords& words);

// Compares the expected words, in order, with what a reading of the window
// heard, placed on the timeline. The two sequences are aligned by their
// longest common subsequence under MatchHeardWord (exact or inflected);
// an expected word inside the window left unmatched is expected_not_heard,
// a heard word left unmatched and not at the window's edge is
// heard_not_expected. Sets `transcribed`. Raises nothing when the expected
// words are unavailable.
void CompareHeardWords(const std::vector<CutInspectionWord>& heard,
                       const CutInspectionPlan& plan,
                       CutInspectionWords& words);

// The report as the agent reads it: exact frames and times, levels, words
// and alerts.
std::string SerializeCutInspection(const CutInspectionPlan& plan,
                                   const CutInspectionAudio& audio,
                                   const CutInspectionWords& words);
