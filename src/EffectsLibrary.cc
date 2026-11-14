#include "EffectsLibrary.h"

#include "ClipSpeed.h"
#include "MotionClip.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <numeric>

namespace ui::effects {

const std::vector<Effect>& Catalog() {
    static const std::vector<Effect> kCatalog = {
        {"cross_dissolve", "Fondu enchaîné", Category::VideoTransition,
         kTransitionCrossDissolve, "#000000",
         "Le plan suivant apparaît sur le précédent"},
        {"dip_to_black", "Fondu au noir", Category::VideoTransition,
         kTransitionDipToColor, "#000000",
         "Un passage par le noir ; au bord d’une piste, une ouverture ou "
         "une fermeture"},
        {"dip_to_white", "Fondu au blanc", Category::VideoTransition,
         kTransitionDipToColor, "#ffffff",
         "Un passage par le blanc ; au bord d’une piste, une ouverture ou "
         "une fermeture"},
        {"cross_fade", "Fondu croisé +3 dB", Category::AudioTransition,
         kTransitionCrossFade, "#000000",
         "Le son suivant monte pendant que le précédent descend, à "
         "puissance constante"},
        // EF3 -- docs/roadmap/EFFETS-2026-10.md.
        {"gaussian_blur", "Flou gaussien", Category::VideoEffect, "", "",
         "Floute l’image du plan ; l’intensité se règle dans l’Inspecteur "
         "(Flou)"},
    };
    return kCatalog;
}

const Effect* FindEffect(const std::string& id) {
    for (const Effect& effect : Catalog())
        if (id == effect.id) return &effect;
    return nullptr;
}

const char* CategoryTitle(Category category) {
    switch (category) {
        case Category::VideoTransition:
            return "Transitions vidéo";
        case Category::AudioTransition:
            return "Transitions audio";
        case Category::VideoEffect:
            return "Effets vidéo";
    }
    return "";
}

bool IsClipEffect(const Effect& effect) {
    return effect.category == Category::VideoEffect;
}

EffectParamValue DefaultClipBlur(const DocumentSequence& sequence) {
    const int64_t side = std::min(sequence.width, sequence.height);
    if (side <= 0) return {10, 1};
    int64_t num = side;
    int64_t den = 108;
    const int64_t common = std::gcd(num, den);
    num /= common;
    den /= common;
    // Never past the bound Document::Validate holds.
    if (num > static_cast<int64_t>(kMaxClipBlur) * den)
        return {kMaxClipBlur, 1};
    return {static_cast<int32_t>(num), static_cast<int32_t>(den)};
}

const char* CategoryTrackKind(Category category) {
    return category == Category::AudioTransition ? "audio" : "video";
}

namespace {

bool IsDip(const Effect& effect) {
    return std::string(effect.transition_type) == kTransitionDipToColor;
}

int64_t Frames(const RationalTime& time, const MediaRate& rate) {
    return time.to_frames(rate.num, rate.den);
}

// EF3 -- why a clip takes no blur, in the interface's French; empty when it
// does. The same refusals Document::Validate makes, said before the drop
// rather than after it.
std::string ClipEffectRefusal(const DocumentTrack& track,
                              const DocumentClip& clip) {
    if (track.kind != "video")
        return "Un effet vidéo se pose sur un plan vidéo";
    if (track.locked) return "Cette piste est verrouillée";
    if (IsMotionClip(clip))
        return "Un plan de motion se floute dans sa composition";
    for (const ClipParameterAnimation& entry : clip.animation)
        if (entry.parameter != ClipAnimatedParameter::Opacity)
            return "Un plan dont le cadrage est animé ne prend pas de flou";
    return {};
}

}  // namespace

DropTarget ClipEffectTarget(const Document& document, const Effect& effect,
                            const Ulid& clipId) {
    DropTarget target;
    const DocumentClip* clip = document.FindClip(clipId);
    const DocumentTrack* track =
        clip != nullptr ? document.FindTrackForClip(clipId) : nullptr;
    if (!IsClipEffect(effect) || clip == nullptr || track == nullptr) {
        target.refusal = "Sélectionnez un plan vidéo";
        return target;
    }
    target.track_id = track->id;
    target.clip_id = clip->id;
    target.refusal = ClipEffectRefusal(*track, *clip);
    target.valid = target.refusal.empty();
    return target;
}

DropTarget FindDropTarget(const Document& document, const Effect& effect,
                          const Ulid& trackId, const RationalTime& time) {
    DropTarget target;
    target.track_id = trackId;
    const DocumentTrack* track = document.FindTrack(trackId);
    // EF3 -- a clip effect: the clip under the pointer, the whole of it.
    if (IsClipEffect(effect)) {
        if (track == nullptr || track->kind != "video") {
            target.refusal = "Un effet vidéo se pose sur un plan vidéo";
            return target;
        }
        for (const DocumentClip& clip : track->clips) {
            if (clip.timeline_in <= time &&
                time < clip.timeline_in.add(clip.duration))
                return ClipEffectTarget(document, effect, clip.id);
        }
        target.refusal = "Déposez l’effet sur un plan";
        return target;
    }
    const bool audio = effect.category == Category::AudioTransition;
    if (track == nullptr || track->kind != CategoryTrackKind(effect.category)) {
        target.refusal = audio ? "Un fondu croisé se pose sur une piste son"
                               : "Une transition vidéo se pose sur une piste "
                                 "vidéo";
        return target;
    }
    if (track->locked) {
        target.refusal = "Cette piste est verrouillée";
        return target;
    }
    std::vector<const DocumentClip*> clips;
    for (const DocumentClip& clip : track->clips) clips.push_back(&clip);
    std::stable_sort(clips.begin(), clips.end(),
                     [](const DocumentClip* left, const DocumentClip* right) {
                         return left->timeline_in < right->timeline_in;
                     });
    size_t under = clips.size();
    for (size_t index = 0; index < clips.size(); ++index) {
        const RationalTime end =
            clips[index]->timeline_in.add(clips[index]->duration);
        if (clips[index]->timeline_in <= time && time < end) {
            under = index;
            break;
        }
    }
    if (under == clips.size()) {
        target.refusal = "Déposez la transition sur un plan, près du raccord";
        return target;
    }
    const DocumentClip& clip = *clips[under];
    const RationalTime end = clip.timeline_in.add(clip.duration);
    const RationalTime offset = time.sub(clip.timeline_in);
    if (offset.add(offset) < clip.duration) {
        target.cut = clip.timeline_in;
        target.right_clip_id = clip.id;
        if (under > 0 && clips[under - 1]->timeline_in.add(
                             clips[under - 1]->duration) == clip.timeline_in)
            target.left_clip_id = clips[under - 1]->id;
    } else {
        target.cut = end;
        target.left_clip_id = clip.id;
        if (under + 1 < clips.size() && clips[under + 1]->timeline_in == end)
            target.right_clip_id = clips[under + 1]->id;
    }
    if ((target.left_clip_id.empty() || target.right_clip_id.empty()) &&
        !IsDip(effect)) {
        target.refusal =
            "Rien de l’autre côté de ce raccord : seul un fondu au noir ou au "
            "blanc se pose au bord d’une piste";
        return target;
    }
    target.valid = true;
    return target;
}

DropEdit EditForDrop(const Document& document, const Effect& effect,
                     const DropTarget& target) {
    DropEdit edit;
    if (!target.valid) {
        edit.refusal = target.refusal;
        return edit;
    }
    const std::string title = effect.title;
    // EF3 -- the blur on the clip. A second drop does not stack a second
    // blur, nor put the first back to the default under the hand that set
    // it: the Inspector is where a blur changes.
    if (IsClipEffect(effect)) {
        const DocumentClip* clip = document.FindClip(target.clip_id);
        if (clip == nullptr) {
            edit.refusal = "Ce plan n’existe plus";
            return edit;
        }
        if (clip->blur.num != 0) {
            edit.refusal =
                "Ce plan est déjà flouté : réglez son flou dans l’Inspecteur";
            return edit;
        }
        const EffectParamValue blur = DefaultClipBlur(document.sequence);
        edit.operation = SetClipBlurOperation{clip->id, blur};
        char amount[32];
        std::snprintf(amount, sizeof(amount), "%.3g",
                      static_cast<double>(blur.num) / blur.den);
        edit.summary = title + " · " + amount + " px";
        return edit;
    }
    for (const DocumentTransition& existing : document.sequence.transitions) {
        if (existing.track_id != target.track_id ||
            existing.left_clip_id != target.left_clip_id ||
            existing.right_clip_id != target.right_clip_id)
            continue;
        if (existing.type == effect.transition_type &&
            existing.color == effect.color) {
            edit.refusal = "Ce raccord a déjà un " + title;
            return edit;
        }
        edit.operation = UpdateTransitionOperation{
            existing.id, effect.transition_type, existing.duration,
            existing.alignment, effect.color};
        edit.summary = title + " · remplace la transition du raccord";
        return edit;
    }

    const MediaRate rate = document.sequence.frame_rate;
    const DocumentClip* left = target.left_clip_id.empty()
                                   ? nullptr
                                   : document.FindClip(target.left_clip_id);
    const DocumentClip* right = target.right_clip_id.empty()
                                    ? nullptr
                                    : document.FindClip(target.right_clip_id);
    // One second, Resolve's default, in whole frames.
    const int64_t second = std::max<int64_t>(
        2, (static_cast<int64_t>(rate.num) + rate.den / 2) / rate.den);
    DocumentTransition transition;
    transition.track_id = target.track_id;
    transition.left_clip_id = target.left_clip_id;
    transition.right_clip_id = target.right_clip_id;
    transition.type = effect.transition_type;
    transition.color = effect.color;
    int64_t frames = 0;
    if (!IsDip(effect)) {
        if (left == nullptr || right == nullptr) {
            edit.refusal = "Ce raccord n’a pas deux plans";
            return edit;
        }
        if (ClipHasSpeed(*left) || ClipHasSpeed(*right)) {
            edit.refusal =
                "Un plan dont la vitesse change ne prend pas de " + title;
            return edit;
        }
        // The frames each side holds past the cut: what the transition
        // reads (TransitionRender.h).
        const std::optional<DocumentSource> leftSource =
            ClipSource(document, *left);
        if (!leftSource) {
            edit.refusal = "Le média du plan sortant est introuvable";
            return edit;
        }
        const int64_t leftHandle = Frames(
            leftSource->duration.sub(left->source_in.add(left->duration)),
            rate);
        const int64_t rightHandle = Frames(right->source_in, rate);
        const int64_t half = std::min({second / 2, leftHandle, rightHandle});
        if (half < 1) {
            edit.refusal =
                "Il manque des images de chaque côté du raccord "
                "pour ce fondu";
            return edit;
        }
        frames = half * 2;
    } else if (left != nullptr && right != nullptr) {
        // A dip reads no handle; it stays on the two clips it covers.
        const int64_t half = std::min({second / 2, Frames(left->duration, rate),
                                       Frames(right->duration, rate)});
        frames = half * 2;
    } else {
        const DocumentClip* covered = left != nullptr ? left : right;
        if (covered == nullptr) {
            edit.refusal = "Ce raccord n’a pas de plan";
            return edit;
        }
        frames = std::min(second, Frames(covered->duration, rate));
        transition.alignment = left == nullptr ? TransitionAlignment::StartAtCut
                                               : TransitionAlignment::EndAtCut;
    }
    if (frames < 1) {
        edit.refusal = "Le plan est trop court pour ce fondu";
        return edit;
    }
    transition.duration = RationalTime{frames * rate.den, rate.num};
    edit.operation = AddTransitionOperation{transition};
    edit.summary = title + " · " + std::to_string(frames) + " images";
    return edit;
}

}  // namespace ui::effects
