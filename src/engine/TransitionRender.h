#pragma once

// R5 -- docs/roadmap/REEL-2026-09.md. What a transition shows, frame by
// frame, written once.
//
// WHY THIS FILE EXISTS. Before it, the split of a transition's frames around
// its cut was written out four times -- Document::Validate, Timeline,
// Export and ResolveXmlExport -- and the fade curve twice: as a ratio of
// long doubles in Timeline, and as FFmpeg's own `fade` ramp in the export
// graph. The two agreed by accident, and no test could have caught them
// drifting apart (guard rail 1 of the lot). Everything about where a
// transition starts, how long it lasts and what it is worth at frame k now
// comes from here, in portable C++ tested off macOS.
//
// TWO FAMILIES, ONE SPAN.
//
//   cross_dissolve -- the incoming shot appears over the outgoing one, both
//     read past their shared cut through the media handles around it. The
//     clips overlap; both are decoded for the length of the transition.
//
//   dip_to_color -- a flat colour covers the junction and thins out either
//     side of it. The clips do NOT overlap and no handle is read: the
//     montage under the veil is exactly the montage without it. That is
//     what lets the same type sit at the edge of a track, over a single
//     clip, where there is nothing to dissolve with: an opening from black
//     is a veil whose cut is the first frame of the film (19 September --
//     the first film measured opens on 40 black frames and closes on 50).
//
//   cross_fade -- EF2, docs/roadmap/EFFETS-2026-10.md. The sound of a
//     dissolve: on an audio track, the outgoing clip's sound falls while
//     the incoming one's rises, both read past their shared cut through
//     the handles, exactly the span and the handles a cross dissolve takes.
//     The ramps are equal power (Resolve's « Cross Fade +3 dB »), so two
//     takes of the same room do not dip in the middle; the curve is the
//     mix's (AudioMixFilters.h, ClipAudioEdges), not a picture's opacity.
//
// THE CURVE. A span covers `frames` sequence frames, `pre` of them before
// the cut and `post` from the cut on. For the frame at index k in [0,
// frames):
//
//   cross dissolve, opacity of the incoming shot:  k / frames
//   colour veil, opacity of the colour:            k / pre        if k < pre
//                                                  (frames - k) / post  else
//
// Both are exact fractions. The veil peaks at exactly 1 on the frame of the
// cut (k == pre) and is 0 on the frame that enters the span, which is what
// FFmpeg's `fade` computes on either side of that cut -- t=out over the
// outgoing clip's last `pre` frames, t=in over the incoming clip's first
// `post`. The export says nothing else; it hands FFmpeg the same two
// integers this file derives (guard rail 2: the frame number is the only
// time variable in the graph).
//
// An edge transition degenerates without a special case: pre == 0 leaves
// only the falling branch, which is 1 on the first frame and 1/post on the
// last -- a film that opens fully black and reaches its picture on the
// frame after the span. post == 0 leaves only the rising branch, ending at
// (frames - 1) / frames: the last frame of a closing fade is nearly, not
// quite, the colour, exactly as the last frame of a dissolve is nearly, not
// quite, the incoming shot.
//
// THE COLOUR. `#rrggbb`, the spelling CaptionStyle already uses, parsed to
// three exact bytes here and nowhere else. It becomes a float once, in
// whichever renderer draws it.

#include "Document.h"

#include <cstdint>
#include <optional>
#include <string>

// The types a document carries. Anything else is refused by
// Document::Validate rather than rendered as one of them.
inline constexpr const char* kTransitionCrossDissolve = "cross_dissolve";
inline constexpr const char* kTransitionDipToColor = "dip_to_color";
inline constexpr const char* kTransitionCrossFade = "cross_fade";

inline bool TransitionIsColorDip(const DocumentTransition& transition) {
    return transition.type == kTransitionDipToColor;
}

// EF2 -- a transition of the sound: the only one an audio track takes, and
// one a video track refuses.
inline bool TransitionIsCrossFade(const DocumentTransition& transition) {
    return transition.type == kTransitionCrossFade;
}

// The kind of track a type belongs to: "audio" for a cross fade, "video"
// for the dissolve and the dip.
inline const char* TransitionTrackKind(const std::string& type) {
    return type == kTransitionCrossFade ? "audio" : "video";
}

// A transition with nothing on one side of its cut: the opening and closing
// fades of a track. Only a colour dip can be one -- a dissolve needs two
// pictures, a cross fade two sounds.
inline bool TransitionIsAtTrackEdge(const DocumentTransition& transition) {
    return transition.left_clip_id.empty() || transition.right_clip_id.empty();
}

struct TransitionColor {
    int32_t red = 0;
    int32_t green = 0;
    int32_t blue = 0;

    bool operator==(const TransitionColor& other) const {
        return red == other.red && green == other.green && blue == other.blue;
    }
};

// `#rrggbb`, upper or lower case. Nothing else: a colour that has to be
// guessed is a colour that comes out differently in the monitor and in the
// file.
bool ParseTransitionColor(const std::string& text, TransitionColor& color,
                          std::string& error);

// `rrggbb` with no '#', for the places that spell a colour without it
// (FFmpeg's `0x......`, xmeml). Exact, from the parsed bytes.
std::string TransitionColorHex(const TransitionColor& color);

// Where a transition sits, in whole sequence frames from the start of the
// sequence. Derived once from the document; every consumer is handed this
// rather than the alignment enum.
struct TransitionSpan {
    // The frame the cut falls on: the first frame of the right clip, or --
    // at a track edge -- the first frame of the film for an opening and the
    // frame just past the last one for a closing.
    int64_t cut_frame = 0;
    int64_t pre_frames = 0;
    int64_t post_frames = 0;

    int64_t frames() const { return pre_frames + post_frames; }
    int64_t first_frame() const { return cut_frame - pre_frames; }
    // Exclusive.
    int64_t end_frame() const { return cut_frame + post_frames; }
    bool covers(int64_t frame) const {
        return frame >= first_frame() && frame < end_frame();
    }
};

// The span of `transition` in `sequence`. Fails, with a message, when the
// duration is not a whole number of sequence frames, when the clips it
// names are not on the track, or when they do not share a cut -- the same
// facts Document::Validate reports, computed here so that there is one
// answer to compare against.
bool ResolveTransitionSpan(const DocumentSequence& sequence,
                           const DocumentTransition& transition,
                           TransitionSpan& span, std::string& error);

// The incoming shot's opacity at index k of a cross dissolve, exact.
// Outside [0, frames) it is clamped to 0 and 1: a caller that asks about a
// frame the transition does not cover gets the state it leaves behind.
EffectParamValue CrossDissolveProgress(int64_t index, int64_t frames);

// The colour's opacity at index k of a dip, exact, by the curve above.
EffectParamValue ColorVeilAlpha(const TransitionSpan& span, int64_t index);

// What covers one track at one sequence frame: the colour and how much of
// it. Absent when no colour transition of that track covers the frame.
struct ResolvedColorVeil {
    Ulid transition_id;
    TransitionColor color;
    EffectParamValue alpha{0, 1};
};

// The veil of `trackId` at sequence frame `frame`. Document::Validate
// refuses two transitions of one track whose spans overlap, so at most one
// answers. A malformed transition is skipped rather than reported: this is
// the rendering path, and Validate has already refused the document.
std::optional<ResolvedColorVeil> ResolveTrackColorVeil(
    const DocumentSequence& sequence, const Ulid& trackId, int64_t frame);

// The exact fraction as the 8-bit code an alpha plane carries, and as the
// float a renderer multiplies with. The two places the exact value stops
// being exact, both here so a reader can see there are only two.
int32_t TransitionAlphaCode(const EffectParamValue& alpha);
float TransitionAlphaFloat(const EffectParamValue& alpha);
