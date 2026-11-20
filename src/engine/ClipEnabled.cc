#include "ClipEnabled.h"

#include "Multicam.h"

#include <algorithm>

bool ClipIsSeen(const DocumentSequence& sequence, const DocumentClip& clip) {
    return clip.enabled && !MulticamHidesClip(sequence, clip.id);
}

std::set<Ulid> OffScreenClipIds(const DocumentSequence& sequence) {
    std::set<Ulid> hidden = MulticamHiddenClipIds(sequence);
    for (const Ulid& clipId : DisabledClipIds(sequence)) {
        hidden.insert(clipId);
    }
    return hidden;
}

std::vector<Ulid> DisabledClipIds(const DocumentSequence& sequence) {
    std::vector<Ulid> disabled;
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.enabled) {
                disabled.push_back(clip.id);
            }
        }
    }
    return disabled;
}

SetClipEnabledOperation ToggleClipsEnabled(const Document& document,
                                           const std::vector<Ulid>& clipIds) {
    std::vector<const DocumentClip*> clips;
    std::set<Ulid> seen;
    for (const Ulid& clipId : clipIds) {
        const DocumentClip* clip = document.FindClip(clipId);
        const DocumentTrack* track = document.FindTrackForClip(clipId);
        if (clip == nullptr || track == nullptr || track->locked ||
            !seen.insert(clipId).second) {
            continue;
        }
        clips.push_back(clip);
    }
    const bool anyEnabled =
        std::any_of(clips.begin(), clips.end(),
                    [](const DocumentClip* clip) { return clip->enabled; });
    SetClipEnabledOperation operation;
    for (const DocumentClip* clip : clips) {
        operation.clips.push_back({clip->id, !anyEnabled});
    }
    return operation;
}
