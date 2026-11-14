#pragma once

// IA1 -- ROADMAP.md. The agent's transcription must observe the exported mix:
// sharing the clip envelopes and final limiter prevents the two FFmpeg paths
// from silently disagreeing after an audio edit. Times stay rational until
// the existing decimal serialization boundary required by FFmpeg options.

#include "Document.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <string>

namespace audio_mix {

inline constexpr int kSampleRate = 48000;

inline std::string Seconds(const RationalTime& time) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(12)
           << static_cast<long double>(time.value) / time.rate;
    std::string value = output.str();
    while (value.size() > 2 && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.push_back('0');
    return value;
}

// AGENT-2026-09 A46 -- every audio edge without a fade of its own gets a
// 3 ms one. A cut in continuous voice lands on whatever sample the frame
// edge falls on: measured on the export of the second native run of the
// 26th, 32 of 37 cuts carried a step of 8 to 80 times the local sample
// change, a click at nearly every splice, and no measurement had said so.
// Three milliseconds is under a frame, inaudible as a dip, and enough to
// take the step out. Playback (AudioPlayback.mm) does the same.
inline constexpr RationalTime kAudioEdgeFade{3, 1000};

inline bool WantsEdgeFade(const DocumentClip& clip) {
    return clip.duration.compare(kAudioEdgeFade.add(kAudioEdgeFade)) > 0;
}

// EF2 -- docs/roadmap/EFFETS-2026-10.md. What one clip's sound reads and
// how its two edges ramp, decided once for the three mixes that must agree:
// the export, the timeline transcription (IA1 above) and playback
// (AudioPlayback.mm). A cross fade reads past the clip's own edges --
// `lead` before its first frame, `tail` past its last, the handles of
// TransitionRender.h's span -- and ramps over the whole span on an equal
// power curve (FFmpeg's `qsin`: sin of a quarter turn, so the two sides'
// powers add to one). An edge without one keeps the clip's own fade, or the
// 3 ms one.
struct ClipAudioEdges {
    RationalTime lead{0, 1};
    RationalTime tail{0, 1};
    RationalTime fade_in{0, 1};
    RationalTime fade_out{0, 1};
    bool equal_power_in = false;
    bool equal_power_out = false;
    // The chain's length: the clip's duration with the lead and the tail.
    RationalTime length{0, 1};
};

// A clip's edges with no cross fade: its own fades, or the edge fade.
inline ClipAudioEdges ClipOwnAudioEdges(const DocumentClip& clip) {
    ClipAudioEdges edges;
    edges.length = clip.duration;
    const bool edge = WantsEdgeFade(clip);
    edges.fade_in = clip.audio_fade_in.value != 0 ? clip.audio_fade_in
                    : edge                        ? kAudioEdgeFade
                                                  : RationalTime{0, 1};
    edges.fade_out = clip.audio_fade_out.value != 0 ? clip.audio_fade_out
                     : edge                         ? kAudioEdgeFade
                                                    : RationalTime{0, 1};
    return edges;
}

// The edges of `clip` on track `trackId`, its cross fades included. Fails
// only on a transition Document::Validate would refuse.
inline bool ResolveClipAudioEdges(const Document& document,
                                  const DocumentClip& clip, const Ulid& trackId,
                                  ClipAudioEdges& edges, std::string& error) {
    edges = ClipOwnAudioEdges(clip);
    const MediaRate rate = document.sequence.frame_rate;
    for (const DocumentTransition& transition : document.sequence.transitions) {
        if (transition.track_id != trackId ||
            !TransitionIsCrossFade(transition) ||
            (transition.left_clip_id != clip.id &&
             transition.right_clip_id != clip.id))
            continue;
        TransitionSpan span;
        if (!ResolveTransitionSpan(document.sequence, transition, span,
                                   error)) {
            error = "transition '" + transition.id + "' " + error;
            return false;
        }
        const RationalTime whole{span.frames() * rate.den, rate.num};
        if (transition.right_clip_id == clip.id) {
            edges.lead = RationalTime{span.pre_frames * rate.den, rate.num};
            edges.fade_in = whole;
            edges.equal_power_in = true;
        }
        if (transition.left_clip_id == clip.id) {
            edges.tail = RationalTime{span.post_frames * rate.den, rate.num};
            edges.fade_out = whole;
            edges.equal_power_out = true;
        }
    }
    edges.length = clip.duration.add(edges.lead).add(edges.tail);
    return true;
}

// The gain at `offset` samples into a chain of `length` samples, as the
// export's `afade` computes it: what playback multiplies by. Linear ramps,
// or a quarter sine on a cross fade's side.
inline float ClipAudioEnvelopeAt(int64_t offset, int64_t length, int64_t fadeIn,
                                 int64_t fadeOut, bool equalPowerIn,
                                 bool equalPowerOut) {
    constexpr float kQuarterTurn = 1.57079632679489661923f;
    float envelope = 1.0f;
    if (fadeIn > 0) {
        const float x = std::min(
            1.0f, static_cast<float>(offset) / static_cast<float>(fadeIn));
        envelope *= equalPowerIn ? std::sin(x * kQuarterTurn) : x;
    }
    if (fadeOut > 0) {
        const float x = std::min(1.0f, static_cast<float>(length - offset) /
                                           static_cast<float>(fadeOut));
        envelope *= equalPowerOut ? std::sin(x * kQuarterTurn) : x;
    }
    return envelope;
}

inline void AppendClipEnvelope(std::ostream& graph, const DocumentClip& clip,
                               const ClipAudioEdges& edges) {
    if (clip.audio_gain_db.num != 0)
        graph << ",volume=pow(10\\,(" << clip.audio_gain_db.num << '/'
              << clip.audio_gain_db.den << ")/20)";
    if (edges.fade_in.value != 0)
        graph << ",afade=t=in:st=0:d=" << Seconds(edges.fade_in)
              << (edges.equal_power_in ? ":curve=qsin" : "");
    if (edges.fade_out.value != 0)
        graph << ",afade=t=out:st=" << Seconds(edges.length.sub(edges.fade_out))
              << ":d=" << Seconds(edges.fade_out)
              << (edges.equal_power_out ? ":curve=qsin" : "");
}

inline void AppendClipEnvelope(std::ostream& graph, const DocumentClip& clip) {
    AppendClipEnvelope(graph, clip, ClipOwnAudioEdges(clip));
}

inline void AppendMixedOutput(std::ostream& graph, size_t inputs,
                              const RationalTime& duration) {
    graph << "amix=inputs=" << inputs
          << ":duration=longest:normalize=0,"
             "alimiter=limit=0.668344:level=0:latency=1,"
             "apad,atrim=duration="
          << Seconds(duration) << "[audio]";
}

}  // namespace audio_mix
