#include "Multicam.h"

#include "Ulid.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace {

using mcp_json::Value;

// The one place the rule of Multicam.h is spelled: an angle is off screen
// when its group has an active angle and it is not that angle.
template <typename Visit>
void VisitHiddenAngleClips(const DocumentSequence& sequence, Visit&& visit) {
    for (const MulticamGroup& group : sequence.multicam_groups) {
        if (group.active_angle_id.empty()) {
            continue;
        }
        for (const MulticamAngle& angle : group.angles) {
            if (angle.id != group.active_angle_id && visit(angle.clip_id)) {
                return;
            }
        }
    }
}

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

bool Refuse(EditError code, std::string text, EditError& error,
            std::string& message) {
    error = code;
    message = std::move(text);
    return false;
}

}  // namespace

std::set<Ulid> MulticamHiddenClipIds(const DocumentSequence& sequence) {
    std::set<Ulid> hidden;
    VisitHiddenAngleClips(sequence, [&](const Ulid& clipId) {
        hidden.insert(clipId);
        return false;
    });
    return hidden;
}

bool MulticamHidesClip(const DocumentSequence& sequence, const Ulid& clipId) {
    bool hidden = false;
    VisitHiddenAngleClips(sequence, [&](const Ulid& angleClipId) {
        hidden = angleClipId == clipId;
        return hidden;
    });
    return hidden;
}

bool BuildAddMulticamGroup(const Document& document, const std::string& name,
                           const std::vector<MulticamAngleRequest>& angles,
                           const Ulid& activeClipId,
                           AddMulticamGroupOperation& operation,
                           EditError& error, std::string& message) {
    operation = {};
    if (angles.size() < 2) {
        return Refuse(EditError::InvalidOperation,
                      "a multicam group needs at least two angles; got " +
                          std::to_string(angles.size()),
                      error, message);
    }
    MulticamGroup group;
    group.id = GenerateUlid();
    group.name =
        name.empty()
            ? "Multicam " +
                  std::to_string(document.sequence.multicam_groups.size() + 1)
            : name;
    for (size_t index = 0; index < angles.size(); ++index) {
        const MulticamAngleRequest& request = angles[index];
        const DocumentTrack* track = document.FindTrackForClip(request.clip_id);
        if (track == nullptr) {
            return Refuse(EditError::UnknownClip,
                          "unknown clip_id '" + request.clip_id + "'", error,
                          message);
        }
        if (track->kind != "video") {
            return Refuse(EditError::InvalidOperation,
                          "clip '" + request.clip_id + "' is on a " +
                              track->kind +
                              " track; a multicam angle is a video clip",
                          error, message);
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (angles[previous].clip_id == request.clip_id) {
                return Refuse(EditError::InvalidOperation,
                              "clip '" + request.clip_id +
                                  "' is named twice; each angle is its own "
                                  "clip",
                              error, message);
            }
        }
        for (const MulticamGroup& existing :
             document.sequence.multicam_groups) {
            for (const MulticamAngle& angle : existing.angles) {
                if (angle.clip_id != request.clip_id) {
                    continue;
                }
                return Refuse(EditError::InvalidOperation,
                              "clip '" + request.clip_id +
                                  "' is already angle '" + angle.id +
                                  "' of multicam group '" + existing.id +
                                  "'; remove that group first",
                              error, message);
            }
        }
        MulticamAngle angle;
        angle.id = GenerateUlid();
        angle.name = request.name.empty() ? "Angle " + std::to_string(index + 1)
                                          : request.name;
        angle.clip_id = request.clip_id;
        if (!activeClipId.empty() && angle.clip_id == activeClipId) {
            group.active_angle_id = angle.id;
        }
        group.angles.push_back(std::move(angle));
    }
    if (!activeClipId.empty() && group.active_angle_id.empty()) {
        return Refuse(EditError::UnknownMulticamAngle,
                      "active clip '" + activeClipId +
                          "' is not one of the group's angles",
                      error, message);
    }
    operation.group = std::move(group);
    operation.insertion_index = -1;
    error = EditError::None;
    message.clear();
    return true;
}

bool BuildSetMulticamActiveAngle(const Document& document, const Ulid& groupId,
                                 const Ulid& angleOrClipId,
                                 SetMulticamActiveAngleOperation& operation,
                                 EditError& error, std::string& message) {
    operation = {};
    const MulticamGroup* named = nullptr;
    if (!groupId.empty()) {
        named = document.FindMulticamGroup(groupId);
        if (named == nullptr) {
            return Refuse(EditError::UnknownMulticamGroup,
                          "unknown multicam group_id '" + groupId + "'", error,
                          message);
        }
    }
    if (angleOrClipId.empty()) {
        if (named == nullptr) {
            return Refuse(EditError::InvalidOperation,
                          "clearing the active angle needs the group_id", error,
                          message);
        }
        operation = {named->id, {}};
        error = EditError::None;
        message.clear();
        return true;
    }
    std::vector<std::pair<const MulticamGroup*, const MulticamAngle*>> matches;
    for (const MulticamGroup& group : document.sequence.multicam_groups) {
        if (named != nullptr && group.id != named->id) {
            continue;
        }
        for (const MulticamAngle& angle : group.angles) {
            if (angle.id == angleOrClipId || angle.clip_id == angleOrClipId) {
                matches.emplace_back(&group, &angle);
            }
        }
    }
    if (matches.empty()) {
        return Refuse(
            EditError::UnknownMulticamAngle,
            "'" + angleOrClipId +
                "' is neither an angle nor the clip of an angle" +
                ((named != nullptr) ? " of multicam group '" + named->id + "'"
                                    : std::string(" of any multicam group")),
            error, message);
    }
    if (matches.size() > 1) {
        std::string groups;
        for (const auto& match : matches) {
            if (!groups.empty()) {
                groups += "', '";
            }
            groups += match.first->id;
        }
        return Refuse(EditError::InvalidOperation,
                      "clip '" + angleOrClipId +
                          "' is an angle of several multicam groups ('" +
                          groups + "'); name the group_id",
                      error, message);
    }
    operation = {matches.front().first->id, matches.front().second->id};
    error = EditError::None;
    message.clear();
    return true;
}

Value DescribeMulticamGroup(const DocumentSequence& sequence,
                            const MulticamGroup& group) {
    Value result = Value::MakeObject();
    result.Set("group_id", Value::MakeString(group.id));
    result.Set("name", Value::MakeString(group.name));
    result.Set("active_angle_id", Value::MakeString(group.active_angle_id));
    Value angles = Value::MakeArray();
    for (const MulticamAngle& angle : group.angles) {
        Value item = Value::MakeObject();
        item.Set("angle_id", Value::MakeString(angle.id));
        item.Set("name", Value::MakeString(angle.name));
        item.Set("clip_id", Value::MakeString(angle.clip_id));
        for (const DocumentTrack& track : sequence.tracks) {
            const auto clip =
                std::find_if(track.clips.begin(), track.clips.end(),
                             [&](const DocumentClip& candidate) {
                                 return candidate.id == angle.clip_id;
                             });
            if (clip == track.clips.end()) {
                continue;
            }
            item.Set("track_id", Value::MakeString(track.id));
            item.Set("track_index", Value::MakeInt(track.index));
            item.Set("track_visible", Value::MakeBool(track.visible));
            item.Set("timeline_in", TimeValue(clip->timeline_in));
            item.Set("duration", TimeValue(clip->duration));
            break;
        }
        item.Set("active", Value::MakeBool(!group.active_angle_id.empty() &&
                                           angle.id == group.active_angle_id));
        item.Set("hidden_by_multicam",
                 Value::MakeBool(MulticamHidesClip(sequence, angle.clip_id)));
        angles.Push(std::move(item));
    }
    result.Set("angles", std::move(angles));
    return result;
}

Value DescribeMulticamGroups(const DocumentSequence& sequence) {
    Value result = Value::MakeObject();
    result.Set("timeline_id", Value::MakeString(sequence.id));
    Value groups = Value::MakeArray();
    for (const MulticamGroup& group : sequence.multicam_groups) {
        groups.Push(DescribeMulticamGroup(sequence, group));
    }
    result.Set("groups", std::move(groups));
    Value hidden = Value::MakeArray();
    for (const Ulid& clipId : MulticamHiddenClipIds(sequence)) {
        hidden.Push(Value::MakeString(clipId));
    }
    result.Set("hidden_clip_ids", std::move(hidden));
    // Stated with the data so a caller never has to infer it from absence:
    // switching an angle leaves every audio clip playing (Multicam.h).
    result.Set("audio_follows_video", Value::MakeBool(false));
    return result;
}

bool DescribeMulticamEdit(const Document& before, const Operation& operation,
                          Value& report) {
    Ulid groupId;
    if (const auto* add = std::get_if<AddMulticamGroupOperation>(&operation)) {
        // An id left empty is drawn at random on apply, so a dry run would
        // report ids the real apply does not keep. BuildAddMulticamGroup
        // assigns them all; anything else gets no report rather than a false
        // one.
        const bool assigned =
            !add->group.id.empty() &&
            std::none_of(
                add->group.angles.begin(), add->group.angles.end(),
                [](const MulticamAngle& angle) { return angle.id.empty(); });
        if (!assigned) {
            return false;
        }
        groupId = add->group.id;
    } else if (const auto* set =
                   std::get_if<SetMulticamActiveAngleOperation>(&operation)) {
        groupId = set->group_id;
    } else if (const auto* remove =
                   std::get_if<RemoveMulticamGroupOperation>(&operation)) {
        groupId = remove->group_id;
    } else {
        return false;
    }
    Document after = before;
    Operation dryRun = operation;
    Operation inverse = RemoveMulticamGroupOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!ApplyOperation(after, dryRun, inverse, error, message)) {
        return false;
    }
    report = Value::MakeObject();
    report.Set("group_id", Value::MakeString(groupId));
    const MulticamGroup* group = after.FindMulticamGroup(groupId);
    if (group != nullptr) {
        Value angleIds = Value::MakeArray();
        for (const MulticamAngle& angle : group->angles) {
            angleIds.Push(Value::MakeString(angle.id));
        }
        report.Set("angle_ids", std::move(angleIds));
        report.Set("active_angle_id",
                   Value::MakeString(group->active_angle_id));
        report.Set("group", DescribeMulticamGroup(after.sequence, *group));
    } else {
        report.Set("group", Value::MakeNull());
    }
    Value hidden = Value::MakeArray();
    for (const Ulid& clipId : MulticamHiddenClipIds(after.sequence)) {
        hidden.Push(Value::MakeString(clipId));
    }
    report.Set("hidden_clip_ids", std::move(hidden));
    return true;
}

bool BuildSplitMulticamGroup(const Document& document, const Ulid& groupId,
                             const RationalTime& position,
                             const Ulid& activeAngleId, BatchOperation& batch,
                             MulticamCut& cut, EditError& error,
                             std::string& message) {
    batch = {};
    cut = {};
    const auto& groups = document.sequence.multicam_groups;
    const auto found = std::find_if(
        groups.begin(), groups.end(),
        [&](const MulticamGroup& group) { return group.id == groupId; });
    if (found == groups.end()) {
        error = EditError::UnknownMulticamGroup;
        message = "unknown multicam group_id '" + groupId + "'";
        return false;
    }
    const MulticamGroup& group = *found;
    const int64_t index = std::distance(groups.begin(), found);
    MulticamGroup before = group;
    before.angles.clear();
    MulticamGroup after;
    after.name = group.name + " (2)";
    // Each angle of the group as it was, and the one it becomes after the
    // cut, so that "the angle that was active" reads across.
    std::vector<std::pair<Ulid, Ulid>> carried;
    bool split = false;
    for (const MulticamAngle& angle : group.angles) {
        const DocumentClip* clip = document.FindClip(angle.clip_id);
        if (clip == nullptr) {
            error = EditError::UnknownClip;
            message = "multicam angle '" + angle.id + "' names unknown clip '" +
                      angle.clip_id + "'";
            return false;
        }
        const RationalTime end = clip->timeline_in.add(clip->duration);
        if (!(clip->timeline_in < position)) {
            // Wholly after the cut: the angle moves to the second group.
            after.angles.push_back(angle);
            carried.emplace_back(angle.id, angle.id);
        } else if (!(position < end)) {
            before.angles.push_back(angle);
        } else {
            SplitClipOperation splitClip;
            splitClip.clip_id = clip->id;
            splitClip.timeline_position = position;
            splitClip.right_clip_id = GenerateUlid();
            batch.operations.push_back(splitClip);
            before.angles.push_back(angle);
            MulticamAngle right;
            right.name = angle.name;
            right.clip_id = splitClip.right_clip_id;
            after.angles.push_back(right);
            carried.emplace_back(angle.id, right.id);
            split = true;
        }
    }
    if (!split) {
        error = EditError::InvalidOperation;
        message = "no angle of multicam group '" + group.name +
                  "' is cut at this position: it lies before, after or "
                  "between them all";
        return false;
    }
    const auto carriedTo = [&carried](const Ulid& id) {
        for (const auto& [from, to] : carried) {
            if (from == id) {
                return to;
            }
        }
        return Ulid();
    };
    const Ulid wanted =
        activeAngleId.empty() ? group.active_angle_id : activeAngleId;
    after.active_angle_id = carriedTo(wanted);
    if (after.active_angle_id.empty()) {
        if (!activeAngleId.empty()) {
            error = EditError::UnknownMulticamAngle;
            message = "angle '" + activeAngleId +
                      "' is not one the second group holds";
            return false;
        }
        after.active_angle_id = after.angles.front().id;
    }
    // The group loses the angles that lie wholly after the cut: rebuilt under
    // its own id, in its place, rather than edited angle by angle.
    if (before.angles.size() != group.angles.size()) {
        const bool keepsActive =
            std::any_of(before.angles.begin(), before.angles.end(),
                        [&](const MulticamAngle& angle) {
                            return angle.id == group.active_angle_id;
                        });
        if (!keepsActive && !group.active_angle_id.empty()) {
            before.active_angle_id = before.angles.front().id;
        }
        batch.operations.push_back(RemoveMulticamGroupOperation{group.id});
        batch.operations.push_back(AddMulticamGroupOperation{before, index});
    }
    batch.operations.push_back(AddMulticamGroupOperation{after, index + 1});
    cut.group_id = after.id;
    for (const MulticamAngle& angle : after.angles) {
        cut.angle_ids.push_back(angle.id);
    }
    cut.active_angle_id = after.active_angle_id;
    batch.label = "Multicam coupé";
    return true;
}
