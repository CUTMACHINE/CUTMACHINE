#pragma once

// SYNCHRO-2026-10 SY2 -- ROADMAP.md. The window's gesture for a recorder's
// sound, as Resolve has it for two clips already on the timeline: select
// the picture and the sound, then Auto Align Clips, Based on Waveform
// (DaVinci Resolve 21.1 manual, chapter 45, pages 955-956). This is the
// policy of that gesture, portable and tested: which clips of a selection
// it takes, and what the window says, in French, when it refuses. main.mm
// only wires it; the edit itself is BuildSeparateSoundAlign
// (SeparateSound.h), the same the MCP and the CLI apply.

#include "Document.h"
#include "Json.h"
#include "MotionClip.h"
#include "Operations.h"
#include "SeparateSound.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ui::separate_sound {

struct SoundPair {
    Ulid picture_clip_id;
    Ulid sound_clip_id;
};

// The picture and the recorder's sound a selection names, or nothing with
// `refusal` saying why. One clip of a rush on a video track, and one clip
// of another recording on an audio track: the picture's own sound, which a
// linked selection brings with it, is not counted, nor are cartons and
// motion clips. Resolve aligns several clips at once on the lowest track;
// the engine lays one recorder under one picture, and says so rather than
// choosing.
inline std::optional<SoundPair> PickSoundPair(
    const Document& document, const std::vector<Ulid>& selection,
    std::string& refusal) {
    refusal.clear();
    std::vector<const DocumentClip*> pictures;
    for (const Ulid& id : selection) {
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (clip != nullptr && track != nullptr && track->kind == "video" &&
            !IsMotionClip(*clip) && !clip->source_id.empty())
            pictures.push_back(clip);
    }
    if (pictures.empty()) {
        refusal =
            "sélectionnez le plan image et le son de l’enregistreur, puis "
            "alignez";
        return std::nullopt;
    }
    if (pictures.size() > 1) {
        refusal =
            "un seul plan image à la fois : le son d’un enregistreur se cale "
            "sous un plan";
        return std::nullopt;
    }
    const DocumentClip& picture = *pictures.front();
    std::vector<const DocumentClip*> sounds;
    bool ownSound = false;
    for (const Ulid& id : selection) {
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (clip == nullptr || track == nullptr || track->kind != "audio" ||
            IsMotionClip(*clip))
            continue;
        if (clip->source_id == picture.source_id) {
            ownSound = true;
            continue;
        }
        sounds.push_back(clip);
    }
    if (sounds.empty()) {
        refusal = ownSound ? "le son sélectionné est celui du plan : "
                             "sélectionnez aussi celui de l’enregistreur"
                           : "sélectionnez aussi le son de l’enregistreur, "
                             "sur une piste audio";
        return std::nullopt;
    }
    if (sounds.size() > 1) {
        refusal =
            "un seul son d’enregistreur à la fois : lequel caler n’est pas "
            "deviné";
        return std::nullopt;
    }
    return SoundPair{picture.id, sounds.front()->id};
}

// Why the engine refuses the pair whatever the offset, said before any
// sound is decoded. Empty for SeparateSoundPairFault::None.
inline std::string PairFaultText(SeparateSoundPairFault fault) {
    using Fault = SeparateSoundPairFault;
    switch (fault) {
        case Fault::None:
            return {};
        case Fault::UnknownPicture:
        case Fault::UnknownSound:
            return "un des deux plans n’est plus sur la timeline";
        case Fault::NotAPicture:
            return "le son se cale sous un plan image d’un rush";
        case Fault::PictureSpeed:
            return "le plan image a une vitesse : la synchro se fait à "
                   "vitesse normale";
        case Fault::NotASound:
            return "le son de l’enregistreur est un plan d’une piste audio";
        case Fault::SameSource:
            return "c’est le son du plan lui-même : choisissez celui de "
                   "l’enregistreur";
        case Fault::SoundSpeed:
            return "le son a une vitesse : la synchro se fait à vitesse "
                   "normale";
        case Fault::LockedTrack:
            return "la piste du plan ou celle du son est verrouillée";
        case Fault::LinkedElsewhere:
            return "le son est lié à un autre plan : déliez-le d’abord";
    }
    return {};
}

// The measure's refusal (MeasureAudioOffset's codes, and the command's
// own), said in French. `detail` is the engine's English message, kept
// for what has no words of its own here.
inline std::string MeasureRefusalText(const std::string& code,
                                      const std::string& detail) {
    if (code == "NoOverlap")
        return "les deux sons n’ont pas deux secondes en commun";
    if (code == "WeakCorrelation")
        return "les deux sons ne se ressemblent pas assez : pas la même "
               "prise, ou trop peu en commun";
    if (code == "AmbiguousPeak")
        return "plusieurs décalages conviennent presque autant (un son "
               "tenu, une boucle, un tic) : aucun n’est deviné";
    if (code == "ClockDrift")
        return "les deux horloges dérivent de plus d’une demi-image sur la "
               "prise : un décalage unique serait faux à la fin";
    if (code == "InvalidDocument")
        return "enregistrez le projet avant de mesurer ses rushes";
    if (code == "ValidationFailed")
        return "un des deux médias n’a pas de son à mesurer (" + detail + ")";
    // Stopped from the task bar: the measure's own words for it.
    if (code == "IoError" && detail.size() >= 9 &&
        detail.compare(detail.size() - 9, 9, "cancelled") == 0)
        return "la mesure a été annulée";
    if (code == "IoError") return "le son n’a pas pu être lu (" + detail + ")";
    return "la mesure a échoué (" + code + " : " + detail + ")";
}

// The measure's answer as --measure-audio-offset writes it (the window runs
// that command off the main thread): true with the offset, or false with
// the refusal's code and the engine's detail.
inline bool ReadMeasuredOffset(const std::string& output, RationalTime& offset,
                               std::string& code, std::string& detail) {
    code.clear();
    detail.clear();
    mcp_json::Value answer;
    std::string parseError;
    if (!mcp_json::Value::Parse(output, answer, parseError) ||
        !answer.IsObject()) {
        code = "IoError";
        detail = "the measure answered nothing readable";
        return false;
    }
    const mcp_json::Value* ok = answer.Find("ok");
    if (ok == nullptr || !ok->IsBool() || !ok->AsBool()) {
        const mcp_json::Value* error = answer.Find("error");
        const mcp_json::Value* text = answer.Find("detail");
        code = error != nullptr && error->IsString() ? error->AsString()
                                                     : "IoError";
        detail = text != nullptr && text->IsString() ? text->AsString() : "";
        return false;
    }
    const mcp_json::Value* found = answer.Find("offset");
    const mcp_json::Value* value =
        found != nullptr ? found->Find("value") : nullptr;
    const mcp_json::Value* rate =
        found != nullptr ? found->Find("rate") : nullptr;
    int64_t samples = 0;
    int64_t perSecond = 0;
    if (value == nullptr || rate == nullptr || !value->AsInt64(samples) ||
        !rate->AsInt64(perSecond) || perSecond <= 0 ||
        perSecond > std::numeric_limits<int32_t>::max()) {
        code = "IoError";
        detail = "the measure gave no offset";
        return false;
    }
    offset = {samples, static_cast<int32_t>(perSecond)};
    return true;
}

// The build's refusal once the offset is known.
inline std::string AlignRefusalText(EditError error,
                                    const std::string& detail) {
    switch (error) {
        case EditError::SourceOutOfBounds:
            return "une fois calé, le son ne couvre aucune image entière du "
                   "plan";
        case EditError::Overlap:
            return "la piste du son tient déjà un autre plan là où il irait "
                   "une fois calé";
        case EditError::LockedTrack:
            return "la piste du plan ou celle du son est verrouillée";
        case EditError::UnknownClip:
            return "un des deux plans n’est plus sur la timeline";
        default:
            return "refusé par le moteur (" + detail + ")";
    }
}

// The offset as the window reports it: in frames of the sequence when it
// falls on whole ones, else in seconds and in samples, signed, like the
// link badge on a clip (audio.md). `offset` is the media offset the
// measure found.
inline std::string OffsetText(const RationalTime& offset,
                              const MediaRate& rate) {
    if (rate.num > 0 && rate.den > 0) {
        const __int128 scaled = static_cast<__int128>(offset.value) * rate.num;
        const __int128 per = static_cast<__int128>(offset.rate) * rate.den;
        if (per != 0 && scaled % per == 0) {
            const long long frames = static_cast<long long>(scaled / per);
            return (frames > 0 ? "+" : "") + std::to_string(frames) + " im.";
        }
    }
    try {
        // In seconds to the millisecond for reading, then the samples.
        const int64_t samples = offset.rescale(48000).value;
        const int64_t magnitude = samples < 0 ? -samples : samples;
        const int64_t milliseconds = (magnitude + 24) / 48;
        std::string thousandths = std::to_string(milliseconds % 1000);
        thousandths.insert(0, 3 - thousandths.size(), '0');
        return std::string(samples < 0 ? "-" : (samples > 0 ? "+" : "")) +
               std::to_string(milliseconds / 1000) + "," + thousandths +
               " s, " + (samples > 0 ? "+" : "") + std::to_string(samples) +
               " éch.";
    } catch (const std::exception&) {
        return std::to_string(offset.value) + "/" +
               std::to_string(offset.rate) + " s";
    }
}

}  // namespace ui::separate_sound
