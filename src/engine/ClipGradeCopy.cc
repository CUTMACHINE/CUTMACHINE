#include "ClipGradeCopy.h"

#include "Document.h"

#include <algorithm>
#include <utility>

namespace {

// Two stacks alike: the same stages in the same order, the same knobs
// spelled the same. Ids do not count: they are the clips' own.
bool SameStack(const std::vector<ClipEffect>& left,
               const std::vector<ClipEffect>& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].type != right[index].type ||
            left[index].params.size() != right[index].params.size())
            return false;
        for (const auto& [name, value] : left[index].params) {
            const auto found = right[index].params.find(name);
            if (found == right[index].params.end() ||
                found->second.num != value.num ||
                found->second.den != value.den)
                return false;
        }
    }
    return true;
}

}  // namespace

std::string ClipGradeRefusal(const Document& document,
                             const DocumentClip& clip) {
    if (IsMotionClip(clip))
        return "a motion clip is drawn by the engine and carries no grade";
    const DocumentTrack* track = document.FindTrackForClip(clip.id);
    if (track == nullptr || track->kind != "video")
        return "only a clip of a video track carries a grade";
    if (document.color_management.enabled &&
        document.IsGraphicsMedia(clip.source_id))
        return "its media is a graphic: under colour management it is "
               "placed as authored and takes no grade";
    return std::string();
}

bool BuildClipGradePaste(const Document& document, const Ulid& fromClipId,
                         const std::vector<Ulid>& clipIds,
                         ClipGradePaste& paste, std::string& error) {
    paste = ClipGradePaste{};
    error.clear();
    const DocumentClip* source = document.FindClip(fromClipId);
    if (source == nullptr) {
        error = "unknown clip_id '" + fromClipId + "'";
        return false;
    }
    const std::string sourceRefusal = ClipGradeRefusal(document, *source);
    if (!sourceRefusal.empty()) {
        error =
            "clip '" + fromClipId + "' has no grade to copy: " + sourceRefusal;
        return false;
    }
    return BuildClipGradePasteOfStack(document, source->effects, clipIds, paste,
                                      error);
}

bool BuildClipGradePasteOfStack(const Document& document,
                                const std::vector<ClipEffect>& stack,
                                const std::vector<Ulid>& clipIds,
                                ClipGradePaste& paste, std::string& error) {
    paste = ClipGradePaste{};
    error.clear();
    if (clipIds.empty()) {
        error = "name the clips to paste the grade onto";
        return false;
    }
    std::vector<Operation> steps;
    std::vector<Ulid> seen;
    for (const Ulid& clipId : clipIds) {
        if (std::find(seen.begin(), seen.end(), clipId) != seen.end()) continue;
        seen.push_back(clipId);
        const DocumentClip* target = document.FindClip(clipId);
        if (target == nullptr) {
            error = "unknown clip_id '" + clipId + "'";
            return false;
        }
        const std::string refusal = ClipGradeRefusal(document, *target);
        if (!refusal.empty()) {
            paste.skipped.push_back({clipId, refusal});
            continue;
        }
        if (SameStack(target->effects, stack)) {
            paste.unchanged.push_back(clipId);
            continue;
        }
        SetClipEffectsOperation step;
        step.clip_id = clipId;
        step.effects = stack;
        for (ClipEffect& effect : step.effects) effect.id.clear();
        steps.push_back(std::move(step));
        paste.pasted.push_back(clipId);
    }
    if (steps.size() == 1) {
        paste.operation = std::move(steps.front());
    } else if (!steps.empty()) {
        paste.operation =
            Operation{BatchOperation{"coller l'étalonnage", std::move(steps)}};
    }
    return true;
}
