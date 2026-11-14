#include "LinkGroupRepair.h"

#include "ClipSpeed.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace {

struct Member {
    const DocumentClip* clip = nullptr;
    bool video = false;
    size_t track = 0;  // the track's place in the sequence, for a stable order
};

RationalTime EndOf(const DocumentClip& clip) {
    return clip.timeline_in.add(clip.duration);
}

// A link refuses a freeze or a ramp (ApplySetClipLink, FC8b).
bool LinkRefuses(const DocumentClip& clip) {
    return ClipIsFreeze(clip) || !clip.speed_ramp.empty();
}

// The places of one group, earliest first: members whose spans overlap,
// directly or through another member, share a place; members that only
// touch do not, which is how a block's stretches lie after a cut.
std::vector<std::vector<Member>> PlacesOf(std::vector<Member> members) {
    std::stable_sort(
        members.begin(), members.end(),
        [](const Member& left, const Member& right) {
            if (left.clip->timeline_in != right.clip->timeline_in) {
                return left.clip->timeline_in < right.clip->timeline_in;
            }
            return left.track < right.track;
        });
    std::vector<std::vector<Member>> places;
    RationalTime end{0, 1};
    for (const Member& member : members) {
        if (places.empty() || !(member.clip->timeline_in < end)) {
            places.push_back({member});
            end = EndOf(*member.clip);
            continue;
        }
        places.back().push_back(member);
        if (end < EndOf(*member.clip)) {
            end = EndOf(*member.clip);
        }
    }
    return places;
}

}  // namespace

LinkGroupRepair PlanLinkGroupRepair(const Document& document,
                                    const std::function<Ulid()>& newGroupId) {
    // Ordered by group ID, so that the plan, and the IDs newGroupId hands
    // out, do not depend on the order of a hash.
    std::map<Ulid, std::vector<Member>> groups;
    const std::vector<DocumentTrack>& tracks = document.sequence.tracks;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const DocumentTrack& track = tracks[index];
        if (track.kind != "video" && track.kind != "audio") {
            continue;
        }
        for (const DocumentClip& clip : track.clips) {
            if (clip.link_group_id.empty()) {
                continue;
            }
            groups[clip.link_group_id].push_back(
                {&clip, track.kind == "video", index});
        }
    }

    LinkGroupRepair repair;
    for (auto& [groupId, members] : groups) {
        const std::vector<std::vector<Member>> places =
            PlacesOf(std::move(members));
        if (places.size() < 2) {
            continue;
        }
        ++repair.groups;
        const auto refused = [](const std::vector<Member>& place) {
            return std::any_of(place.begin(), place.end(), [](const Member& m) {
                return LinkRefuses(*m.clip);
            });
        };
        // A clip alone at its place has no partner there: its link goes,
        // two at a time, since clearing a link names two clips. One left
        // over keeps the old group, alone in it once the others have left.
        std::vector<const DocumentClip*> alone;
        bool keepsOldGroup = false;
        for (const std::vector<Member>& place : places) {
            if (place.size() == 1 && !refused(place)) {
                alone.push_back(place.front().clip);
            } else if (refused(place)) {
                // Linked again, it would be refused: it stays as it was.
                keepsOldGroup = true;
                repair.left_alone += place.size();
            }
        }
        for (size_t index = 0; index + 1 < alone.size(); index += 2) {
            SetClipLinkOperation clear;
            clear.first_clip_id = alone[index]->id;
            clear.second_clip_id = alone[index + 1]->id;
            repair.operations.push_back(std::move(clear));
            repair.cleared += 2;
        }
        if (alone.size() % 2 == 1) {
            keepsOldGroup = true;
            ++repair.left_alone;
        }
        // The old group stays with the earliest place, unless clips that
        // cannot move keep it: then every place takes a new one.
        bool oldGroupTaken = keepsOldGroup;
        for (const std::vector<Member>& place : places) {
            if (place.size() < 2 || refused(place)) {
                continue;
            }
            Ulid group = groupId;
            if (oldGroupTaken) {
                group = newGroupId();
                ++repair.places;
            }
            oldGroupTaken = true;
            // The picture of the place anchors it, as set_clip_link does;
            // a place of sounds only is anchored on its first sound.
            const auto picture =
                std::find_if(place.begin(), place.end(),
                             [](const Member& m) { return m.video; });
            const Member& anchor =
                picture != place.end() ? *picture : place.front();
            for (const Member& member : place) {
                if (member.clip == anchor.clip) {
                    continue;
                }
                SetClipLinkOperation link;
                link.first_clip_id = anchor.clip->id;
                link.second_clip_id = member.clip->id;
                link.first_group_id = group;
                link.second_group_id = group;
                repair.operations.push_back(std::move(link));
            }
        }
    }
    return repair;
}

std::vector<BatchOperation> LinkGroupRepairBatches(
    const LinkGroupRepair& repair) {
    std::vector<BatchOperation> batches;
    for (size_t at = 0; at < repair.operations.size();
         at += kMaxBatchOperations) {
        BatchOperation batch;
        batch.label = "Liens séparés par position";
        const size_t end =
            std::min(repair.operations.size(), at + kMaxBatchOperations);
        for (size_t index = at; index < end; ++index) {
            batch.operations.push_back(repair.operations[index]);
        }
        batches.push_back(std::move(batch));
    }
    return batches;
}
