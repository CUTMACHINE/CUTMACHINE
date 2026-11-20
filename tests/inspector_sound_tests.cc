// GitHub #330 -- the Inspector's « Son » section as policy
// (InspectorSound.h): the clip it sets, what a typed gain or fade becomes,
// and what is refused. Every answer it gives is applied here through the
// engine, so that what the panel would send is something EditLog takes.

#include "InspectorSound.h"

#include "EditLog.h"

#include <iostream>
#include <string>

#include "TestSupport.h"

namespace {

constexpr char kSource[] = "01KT0000000000000000000001";
constexpr char kVideoTrack[] = "01KT0000000000000000000002";
constexpr char kAudio1[] = "01KT0000000000000000000003";
constexpr char kAudio2[] = "01KT0000000000000000000004";
constexpr char kPicture[] = "01KT0000000000000000000010";
constexpr char kSoundA1[] = "01KT0000000000000000000011";
constexpr char kSoundA2[] = "01KT0000000000000000000012";
constexpr char kLaterA1[] = "01KT0000000000000000000013";
constexpr char kAlone[] = "01KT0000000000000000000014";
constexpr char kLink[] = "01KT0000000000000000000020";

DocumentClip Clip(const char* id, int64_t in, int64_t duration,
                  const char* link = "") {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = kSource;
    clip.source_in = {0, 25};
    clip.timeline_in = {in, 25};
    clip.duration = {duration, 25};
    clip.link_group_id = link;
    return clip;
}

// 25 i/s. V1: a picture [0, 50) linked to its sounds on A1 and A2; A1 also
// holds a later clip of the same group, after the picture, and a clip
// linked to nothing.
Document Fixture() {
    Document document;
    document.sources = {{kSource, "source.mov", {25, 1}, {500, 25}}};
    document.sequence.frame_rate = {25, 1};
    DocumentTrack video{kVideoTrack, "video", 0, {}};
    DocumentTrack a1{kAudio1, "audio", 1, {}};
    DocumentTrack a2{kAudio2, "audio", 2, {}};
    video.clips.push_back(Clip(kPicture, 0, 50, kLink));
    a1.clips.push_back(Clip(kSoundA1, 0, 50, kLink));
    a1.clips.push_back(Clip(kLaterA1, 100, 50, kLink));
    a1.clips.push_back(Clip(kAlone, 200, 50));
    a2.clips.push_back(Clip(kSoundA2, 0, 50, kLink));
    document.sequence.tracks = {video, a1, a2};
    return document;
}

bool Same(const EffectParamValue& left, const EffectParamValue& right) {
    return left.num == right.num && left.den == right.den;
}

bool Applies(Document document, const SetClipAudioOperation& operation,
             std::string& message) {
    EditLog log;
    EditError error = EditError::None;
    return log.Apply(document, operation, error, message);
}

}  // namespace

int main() {
    using namespace ui::inspector;
    const MediaRate rate{25, 1};

    // --- the clip the section sets ---
    {
        const Document document = Fixture();
        const auto own = InspectorSoundTarget(document, kAlone);
        Check(own && own->clip_id == kAlone && own->track_id == kAudio1 &&
                  !own->linked && !own->locked,
              "an audio clip sets its own sound");
        const auto linked = InspectorSoundTarget(document, kPicture);
        Check(linked && linked->clip_id == kSoundA1 &&
                  linked->track_id == kAudio1 && linked->linked,
              "a picture sets its first linked sound at its place, A1 "
              "before A2, never the later clip of its group");
        Check(!InspectorSoundTarget(document, "").has_value() &&
                  !InspectorSoundTarget(document, "01KT0000000000000000000099")
                       .has_value(),
              "no clip, no sound");
        Document unlinked = Fixture();
        unlinked.sequence.tracks[0].clips[0].link_group_id.clear();
        Check(!InspectorSoundTarget(unlinked, kPicture).has_value(),
              "a picture linked to nothing has no sound to set");
        Document locked = Fixture();
        locked.sequence.tracks[1].locked = true;
        const auto read = InspectorSoundTarget(locked, kPicture);
        Check(read && read->locked, "a locked track's sound is read only");
    }

    // --- the gain ---
    {
        Check(Same(QuantizeClipGain(-18.0f), {-18, 1}) &&
                  Same(QuantizeClipGain(-4.5f), {-9, 2}) &&
                  Same(QuantizeClipGain(0.04f), {0, 1}) &&
                  Same(QuantizeClipGain(-500.0f), {-120, 1}),
              "a slider's gain is a tenth of a dB, reduced, in range");
        Check(CurrentClipGain({-9, 2}) == -4.5f && CurrentClipGain({0, 0}) == 0,
              "the slider shows the fraction, and no gain for a bad one");
        Check(ClipGainText({0, 1}) == "0 dB" &&
                  ClipGainText({-18, 1}) == "-18 dB" &&
                  ClipGainText({-9, 2}) == "-4.5 dB" &&
                  ClipGainText({3, 1}) == "+3 dB",
              "the field reads the badge's text, 0 dB without gain");
        EffectParamValue gain;
        std::string refusal;
        const auto parsed = [&](const std::string& typed) {
            refusal.clear();
            gain = {7, 7};
            return ParseClipGain(typed, gain, refusal);
        };
        Check(parsed("-18") && Same(gain, {-18, 1}), "-18");
        Check(parsed(" -4,5 dB ") && Same(gain, {-9, 2}),
              "a comma, spaces and the unit");
        Check(parsed("+3dB") && Same(gain, {3, 1}), "+3dB");
        Check(parsed("-0.25") && Same(gain, {-3, 10}),
              "to the nearest tenth, half away from zero");
        Check(parsed("0") && Same(gain, {0, 1}) && parsed("-0.04") &&
                  Same(gain, {0, 1}),
              "nothing is {0, 1}, as a clip that never had a gain");
        Check(parsed("48") && parsed("-120") && !parsed("48.1") &&
                  refusal.find("-120") != std::string::npos && !parsed("-121"),
              "the engine's range, refused past it in words");
        Check(!parsed("fort") && refusal.find("fort") != std::string::npos &&
                  !parsed("") && !parsed("-") && !parsed("1.2.3") &&
                  !parsed("dB"),
              "what is not a number is refused, and said");
        Check(Same(gain, {7, 7}), "a refusal leaves the gain");
    }

    // --- the fades as the field shows them ---
    {
        Check(FadeLengthText({0, 1}, rate) == "0" &&
                  FadeLengthText({12, 25}, rate) == "12" &&
                  FadeLengthText({30, 25}, rate) == "1:05" &&
                  FadeLengthText({2, 1}, rate) == "2:00",
              "frames under a second, seconds:frames from one");
        Check(FadeLengthText({13, 25}, rate) == "13" &&
                  FadeLengthText({1, 2}, {24, 1}) == "12" &&
                  FadeLengthText({1, 3}, rate) == "0.33 s",
              "a length off the frame grid reads in seconds");
        Check(FadeLengthText({12012, 30000}, {30000, 1001}) == "12" &&
                  FadeLengthText({1001, 2500}, {30000, 1001}) == "12",
              "NTSC frames, reduced or not");
        RationalTime fade;
        std::string refusal;
        const auto parsed = [&](const std::string& typed,
                                MediaRate at = {25, 1}) {
            refusal.clear();
            fade = {99, 1};
            return ParseFadeLength(typed, at, fade, refusal);
        };
        Check(parsed("12") && fade == RationalTime{12, 25}, "frames");
        Check(parsed("12 i") && fade == RationalTime{12, 25} && parsed("12f") &&
                  fade == RationalTime{12, 25} && parsed("12 images") &&
                  fade == RationalTime{12, 25},
              "frames with their unit");
        Check(parsed("1:05") && fade == RationalTime{30, 25} &&
                  !parsed("1:25") && !parsed("1:") && !parsed(":5"),
              "seconds:frames, the frames under the rate");
        Check(parsed("0,5 s") && fade == RationalTime{13, 25} &&
                  parsed("1.5") && fade == RationalTime{38, 25} &&
                  parsed("2s") && fade == RationalTime{50, 25},
              "seconds, rounded to the nearest frame");
        Check(parsed("0") && fade == RationalTime{0, 1} && parsed("0 s") &&
                  fade == RationalTime{0, 1},
              "no fade is {0, 1}");
        Check(parsed("12", {30000, 1001}) && fade == RationalTime{12012, 30000},
              "NTSC frames are exact");
        Check(!parsed("long") && refusal.find("long") != std::string::npos &&
                  !parsed("") && !parsed("-12") && !parsed("12x"),
              "what is not a duration is refused, and said");
        Check(!parsed("12", {0, 1}), "no rate, no frames");
    }

    // --- the fades and the room the other one leaves ---
    {
        Document document = Fixture();
        DocumentClip& sound = document.sequence.tracks[1].clips[0];
        sound.audio_fade_out = {20, 25};
        Check(FadeRoomFrames(sound, true, rate) == 30 &&
                  FadeRoomFrames(sound, false, rate) == 50,
              "a fade takes what the other leaves of the clip");
        std::string refusal;
        const auto in = WithClipFade(sound, true, {30, 25}, rate, refusal);
        Check(in && in->clip_id == kSoundA1 &&
                  in->fade_in == RationalTime{30, 25} &&
                  in->fade_out == RationalTime{20, 25} &&
                  Same(in->gain_db, {0, 1}),
              "up to the other fade, the rest of the settings kept");
        std::string message;
        Check(in && Applies(document, *in, message),
              "and the engine takes it: " + message);
        Check(!WithClipFade(sound, true, {31, 25}, rate, refusal) &&
                  refusal ==
                      "le fondu d’entrée ne peut pas dépasser ce que le fondu "
                      "de sortie laisse du plan : 1:05 au plus",
              "past it, refused with the other fade and the room in words: " +
                  refusal);
        Check(!WithClipFade(sound, false, {51, 25}, rate, refusal) &&
                  refusal ==
                      "le fondu de sortie ne peut pas dépasser la durée du "
                      "plan : 2:00 au plus",
              "with no other fade, the clip is the limit: " + refusal);
        sound.audio_gain_db = {-18, 1};
        const SetClipAudioOperation gained = WithClipGain(sound, {-9, 2});
        Check(Same(gained.gain_db, {-9, 2}) &&
                  gained.fade_out == RationalTime{20, 25} &&
                  Applies(document, gained, message),
              "a gain keeps the fades, and the engine takes it: " + message);
        sound.duration = {101, 50};
        sound.audio_fade_out = {0, 1};
        Check(FadeRoomFrames(sound, true, rate) == 50,
              "a clip off the frame grid gives its whole frames");
    }

    if (failures == 0) std::cout << "inspector_sound_tests: all passed\n";
    return failures == 0 ? 0 : 1;
}
