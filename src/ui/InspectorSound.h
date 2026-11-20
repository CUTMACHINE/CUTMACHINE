#pragma once

// Out of lot -- the author, 18 November 2026 (GitHub #330). The Inspector's «
// Son » (Sound) section: a clip's gain in decibels and its two fades, the
// settings SetClipAudioOperation holds. The engine had them, playback and
// export played them, and the window showed nothing: a person reviewing an
// agent's mix could neither read the gain it had set nor change a fade by ear,
// and had to ask the agent to take a fade out.
//
// What the section edits, what a typed value means, and what is refused, are
// decided here, in plain C++ (tests/inspector_sound_tests.cc), so that
// InspectorView.mm only wires controls to it. Every answer is one complete
// SetClipAudioOperation, applied by the window through EditLog::Apply like
// every other edit of the panel: undo, the project's journal and the agent's
// view of the edit come with it.

#include "Document.h"
#include "Operations.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ui::inspector {

// The gain slider's travel. Document::Validate allows -120 to +48 dB; a
// music bed sits around -20, and past +12 a clip is louder than a mix is
// made. A gain beyond the travel is shown, the slider at its end, and typed.
inline constexpr float kInspectorGainSliderMin = -60.0f;
inline constexpr float kInspectorGainSliderMax = 12.0f;
inline constexpr int32_t kInspectorGainMinimumDb = -120;
inline constexpr int32_t kInspectorGainMaximumDb = 48;

// The clip the section edits for the clip the Inspector shows.
struct SoundTarget {
    Ulid clip_id;
    Ulid track_id;
    // The sound of a linked picture: the Inspector shows a video clip, and
    // this is the first of its linked audio clips at its place, in the
    // timeline's order (A1 first). Its other sounds are set by selecting
    // them.
    bool linked = false;
    // A locked track's clip is read, not written.
    bool locked = false;
};

// `anchorClipId` itself on an audio track; otherwise its first linked audio
// clip that overlaps it in time; nothing for a clip without sound (a picture
// with none linked, a carton, a motion clip).
std::optional<SoundTarget> InspectorSoundTarget(const Document& document,
                                                const Ulid& anchorClipId);

// A slider's value to the nearest tenth of a decibel, inside the engine's
// range, its fraction reduced: -18 is {-18, 1}, -4.5 is {-9, 2}.
EffectParamValue QuantizeClipGain(float sliderValue);
float CurrentClipGain(const EffectParamValue& gainDb);

// What the gain field shows: "0 dB" without gain, ClipGainBadgeLabel's text
// otherwise ("-18 dB", "-4.5 dB", "+3 dB").
std::string ClipGainText(const EffectParamValue& gainDb);

// A typed gain: a number of decibels with an optional sign, a point or a
// comma before its decimals, and "dB" after it if one wishes ("-18",
// "-4,5 dB", "+3"), to the nearest tenth. Refused, in words, when it is not
// one, or past -120 or +48.
bool ParseClipGain(const std::string& typed, EffectParamValue& gainDb,
                   std::string& refusal);

// A fade as the field shows it, its length in frames of the sequence as the
// trim counter writes one (TrimReadoutText), unsigned: "0", "12" under a
// second, "1:05" (seconds:frames) from one on. A length that is not a whole
// number of frames, which an agent may set, reads in seconds: "0.52 s".
std::string FadeLengthText(const RationalTime& fade, const MediaRate& rate);

// A typed fade, rounded to the nearest whole frame, as a handle drag is: a
// number of frames ("12", "12 i"), seconds and frames ("1:05"), or seconds
// with a unit or decimals ("0,5 s", "1.5"). Refused, in words, otherwise.
bool ParseFadeLength(const std::string& typed, const MediaRate& rate,
                     RationalTime& fade, std::string& refusal);

// `frames` of the sequence as a fade: {0, 1} for none, as a clip that never
// had one holds it.
RationalTime FadeOfFrames(int64_t frames, const MediaRate& rate);
// The whole frames of `time`: the nearest, or the last that fits.
int64_t FadeFramesOf(const RationalTime& time, const MediaRate& rate,
                     bool nearest);

// The longest fade in (`fadeIn`) or out `clip` takes: what the other fade
// leaves of it, in whole frames. The two never cross, as a trim keeps them
// (Operations.cc) and a handle drag does (TimelineView.h).
int64_t FadeRoomFrames(const DocumentClip& clip, bool fadeIn,
                       const MediaRate& rate);

// `clip`'s settings with its fade in (`fadeIn`) or out set to `fade`; the
// gain and the other fade as they are. Refused, in words, past the room.
std::optional<SetClipAudioOperation> WithClipFade(const DocumentClip& clip,
                                                  bool fadeIn,
                                                  const RationalTime& fade,
                                                  const MediaRate& rate,
                                                  std::string& refusal);

// `clip`'s settings with its gain set to `gainDb`.
SetClipAudioOperation WithClipGain(const DocumentClip& clip,
                                   const EffectParamValue& gainDb);

}  // namespace ui::inspector
