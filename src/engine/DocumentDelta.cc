#include "DocumentDelta.h"

#include "AggregateMembers.h"
#include "ClipAnimation.h"
#include "IdResolver.h"
#include "Json.h"
#include "MotionDocument.h"
#include "Timeline.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

struct Placed {
    const DocumentClip* clip = nullptr;
    const DocumentTrack* track = nullptr;
};

std::map<Ulid, Placed> IndexClips(const Document& document) {
    std::map<Ulid, Placed> index;
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            index[clip.id] = Placed{&clip, &track};
    return index;
}

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

// A move and nothing else: same track, same source range, same everything
// but where it sits. Only these can be folded into a DeltaShift.
bool OnlyMoved(const Placed& before, const Placed& after) {
    if (before.track->id != after.track->id) return false;
    DocumentClip candidate = *after.clip;
    candidate.timeline_in = before.clip->timeline_in;
    return ClipsEqual(*before.clip, candidate);
}

template <typename Item>
DeltaIdentities DiffIdentities(const std::vector<Item>& before,
                               const std::vector<Item>& after) {
    DeltaIdentities identities;
    const auto contains = [](const std::vector<Item>& items, const Ulid& id) {
        return std::any_of(items.begin(), items.end(),
                           [&](const Item& item) { return item.id == id; });
    };
    for (const Item& item : after)
        if (!contains(before, item.id)) identities.created.push_back(item.id);
    for (const Item& item : before)
        if (!contains(after, item.id)) identities.removed.push_back(item.id);
    return identities;
}

void SetIdentities(Value& root, const std::string& kind,
                   const DeltaIdentities& identities) {
    const auto publish = [&](const std::string& key,
                             const std::vector<Ulid>& ids) {
        if (ids.empty()) return;
        Value array = Value::MakeArray();
        for (const Ulid& id : ids) array.Push(Value::MakeString(id));
        root.Set(key, std::move(array));
    };
    publish("created_" + kind + "_ids", identities.created);
    publish("removed_" + kind + "_ids", identities.removed);
}

// R1 -- docs/roadmap/REEL-2026-09.md. A framing compared the way the
// speed above is: field by field, as the file spells it, because a delta
// describes bytes and not geometry.
bool SameFramingSpelling(const ClipFraming& left, const ClipFraming& right) {
    const auto same = [](const EffectParamValue& a, const EffectParamValue& b) {
        return a.num == b.num && a.den == b.den;
    };
    return left.mode == right.mode && same(left.scale, right.scale) &&
           same(left.position_x, right.position_x) &&
           same(left.position_y, right.position_y) &&
           same(left.crop_left, right.crop_left) &&
           same(left.crop_right, right.crop_right) &&
           same(left.crop_top, right.crop_top) &&
           same(left.crop_bottom, right.crop_bottom);
}

// R2 -- docs/roadmap/REEL-2026-09.md. Keys compared as they are spelled,
// like the framing above: the same curve written with other fractions is
// other bytes, and a delta describes bytes.
bool SameKeySpelling(const std::vector<ClipKeyframe>& left,
                     const std::vector<ClipKeyframe>& right) {
    const auto same = [](const EffectParamValue& first,
                         const EffectParamValue& second) {
        return first.num == second.num && first.den == second.den;
    };
    if (left.size() != right.size()) return false;
    for (size_t key = 0; key < left.size(); ++key) {
        const ClipKeyframe& first = left[key];
        const ClipKeyframe& second = right[key];
        if (first.id != second.id ||
            first.interpolation != second.interpolation ||
            first.time.value != second.time.value ||
            first.time.rate != second.time.rate ||
            !same(first.value, second.value) ||
            !same(first.easing_x1, second.easing_x1) ||
            !same(first.easing_y1, second.easing_y1) ||
            !same(first.easing_x2, second.easing_x2) ||
            !same(first.easing_y2, second.easing_y2))
            return false;
    }
    return true;
}

bool SameAnimationSpelling(const std::vector<ClipParameterAnimation>& left,
                           const std::vector<ClipParameterAnimation>& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].parameter != right[index].parameter ||
            !SameKeySpelling(left[index].keyframes, right[index].keyframes))
            return false;
    }
    return true;
}

// `before` is the clip as it was, null for a clip just created or one that
// only moved (a move leaves its speed alone).
DeltaClip Describe(const Placed& placed, bool created,
                   const DocumentClip* before = nullptr) {
    const DocumentClip& clip = *placed.clip;
    DeltaClip entry;
    entry.clip_id = clip.id;
    entry.track_id = placed.track->id;
    entry.source_id = clip.source_id;
    entry.source_in = clip.source_in;
    entry.duration = clip.duration;
    entry.timeline_in = clip.timeline_in;
    const bool unit = clip.speed.num == 1 && clip.speed.den == 1;
    const bool speedChanged =
        before != nullptr && (before->speed.num != clip.speed.num ||
                              before->speed.den != clip.speed.den);
    if (!unit || speedChanged) entry.speed = clip.speed;
    if (!clip.speed_ramp.empty() ||
        (before != nullptr && !before->speed_ramp.empty()))
        entry.speed_ramp = !clip.speed_ramp.empty();
    if (!clip.enabled || (before != nullptr && !before->enabled))
        entry.enabled = clip.enabled;
    entry.created = created;
    return entry;
}

}  // namespace

// FORMAT-2026-09 -- ROADMAP.md. ClipsEqual lists DocumentClip's members by
// hand, and QC-2026-09 A7's gain and fades were added without it learning
// them: a clip whose sound changed read as untouched, and a clip moved and
// re-leveled folded into a plain shift. A member added to the struct is a
// compile error here until it is compared below.
static_assert(AggregateMemberCount<DocumentClip>::value == 26,
              "DocumentClip gained a member: compare it in ClipsEqual");

bool ClipsEqual(const DocumentClip& left, const DocumentClip& right) {
    if (left.id != right.id || left.source_id != right.source_id ||
        left.include_audio != right.include_audio ||
        left.enabled != right.enabled ||
        left.link_group_id != right.link_group_id ||
        left.sync_anchor_clip_id != right.sync_anchor_clip_id ||
        left.caption_group_id != right.caption_group_id ||
        left.caption_text != right.caption_text ||
        left.composition_id != right.composition_id ||
        left.motion_props.Dump() != right.motion_props.Dump())
        return false;
    if (left.source_in.compare(right.source_in) != 0 ||
        left.duration.compare(right.duration) != 0 ||
        left.timeline_in.compare(right.timeline_in) != 0 ||
        left.sync_reference_delta.compare(right.sync_reference_delta) != 0 ||
        left.audio_fade_in.compare(right.audio_fade_in) != 0 ||
        left.audio_fade_out.compare(right.audio_fade_out) != 0)
        return false;
    if (left.opacity.num != right.opacity.num ||
        left.opacity.den != right.opacity.den ||
        // EF3 -- a blur set or changed is a clip that changed.
        left.blur.num != right.blur.num || left.blur.den != right.blur.den ||
        left.audio_gain_db.num != right.audio_gain_db.num ||
        left.audio_gain_db.den != right.audio_gain_db.den ||
        // R3 -- the fraction as it is spelled, not the rational it means: a
        // clip written 2/4 and a clip written 1/2 are the same speed and
        // not the same bytes, and a delta describes bytes.
        left.speed.num != right.speed.num ||
        left.speed.den != right.speed.den ||
        // R1 -- the framing as it is spelled too, for the same reason.
        !SameFramingSpelling(left.framing, right.framing) ||
        // R2 -- and the keys as they are spelled: a clip whose zoom was
        // reshaped is a clip that changed, and a delta that folded it into
        // a plain move would hide the only thing the edit did.
        !SameAnimationSpelling(left.animation, right.animation) ||
        // R3, criterion 4 -- a ramp reshaped is a clip that changed.
        !SameKeySpelling(left.speed_ramp, right.speed_ramp))
        return false;
    // PA8 -- the mask as it is spelled: a clip cut out otherwise is a clip
    // that changed.
    const auto sameFraction = [](const EffectParamValue& a,
                                 const EffectParamValue& b) {
        return a.num == b.num && a.den == b.den;
    };
    if (left.mask.shape != right.mask.shape ||
        left.mask.inverted != right.mask.inverted ||
        !sameFraction(left.mask.center_x, right.mask.center_x) ||
        !sameFraction(left.mask.center_y, right.mask.center_y) ||
        !sameFraction(left.mask.width, right.mask.width) ||
        !sameFraction(left.mask.height, right.mask.height) ||
        !sameFraction(left.mask.feather, right.mask.feather))
        return false;
    // PA4 -- the words' times as they are spelled: a carton retimed is a
    // carton that changed.
    if (left.caption_word_times.size() != right.caption_word_times.size())
        return false;
    for (size_t index = 0; index < left.caption_word_times.size(); ++index) {
        if (left.caption_word_times[index].value !=
                right.caption_word_times[index].value ||
            left.caption_word_times[index].rate !=
                right.caption_word_times[index].rate)
            return false;
    }
    if (left.effects.size() != right.effects.size()) return false;
    for (size_t index = 0; index < left.effects.size(); ++index) {
        const ClipEffect& a = left.effects[index];
        const ClipEffect& b = right.effects[index];
        if (a.id != b.id || a.type != b.type) return false;
        if (a.params.size() != b.params.size()) return false;
        for (const auto& entry : a.params) {
            const auto found = b.params.find(entry.first);
            if (found == b.params.end() ||
                found->second.num != entry.second.num ||
                found->second.den != entry.second.den)
                return false;
        }
    }
    return true;
}

bool ComputeDocumentDelta(const Document& before, const Document& after,
                          DocumentDelta& delta) {
    delta = DocumentDelta{};
    try {
        delta.duration = Timeline(after).Duration();
    } catch (const std::exception&) {
        delta.duration = RationalTime{0, 1};
    }
    const DocumentSequence& oldSequence = before.sequence;
    const DocumentSequence& newSequence = after.sequence;
    delta.sequence_changed =
        oldSequence.name != newSequence.name ||
        oldSequence.width != newSequence.width ||
        oldSequence.height != newSequence.height ||
        oldSequence.frame_rate.num != newSequence.frame_rate.num ||
        oldSequence.frame_rate.den != newSequence.frame_rate.den;

    std::map<Ulid, const DocumentTrack*> oldTracks;
    for (const DocumentTrack& track : before.sequence.tracks)
        oldTracks[track.id] = &track;
    const std::vector<const DocumentTrack*> listed =
        TracksInDescribeOrder(after.sequence);
    for (const DocumentTrack& track : after.sequence.tracks) {
        if (oldTracks.count(track.id)) continue;
        DeltaTrack entry;
        entry.track_id = track.id;
        entry.kind = track.kind;
        entry.index = track.index;
        const auto rank = std::find(listed.begin(), listed.end(), &track);
        if (rank != listed.end())
            entry.alias =
                TrackAlias(listed, static_cast<size_t>(rank - listed.begin()));
        delta.created_tracks.push_back(std::move(entry));
    }
    std::map<Ulid, const DocumentTrack*> newTracks;
    for (const DocumentTrack& track : after.sequence.tracks)
        newTracks[track.id] = &track;
    for (const auto& entry : oldTracks)
        if (!newTracks.count(entry.first))
            delta.removed_track_ids.push_back(entry.first);

    delta.caption_styles =
        DiffIdentities(oldSequence.caption_styles, newSequence.caption_styles);
    delta.markers = DiffIdentities(oldSequence.markers, newSequence.markers);
    delta.transitions =
        DiffIdentities(oldSequence.transitions, newSequence.transitions);
    delta.bins = DiffIdentities(before.bins, after.bins);
    delta.compositions =
        DiffIdentities(before.compositions, after.compositions);
    std::vector<Ulid> nodesBefore;
    std::vector<Ulid> nodesAfter;
    std::vector<Ulid> keyframesBefore;
    std::vector<Ulid> keyframesAfter;
    for (const DocumentComposition& composition : before.compositions) {
        MotionDefinitionIdentities(composition.definition, nodesBefore,
                                   keyframesBefore);
    }
    for (const DocumentComposition& composition : after.compositions) {
        MotionDefinitionIdentities(composition.definition, nodesAfter,
                                   keyframesAfter);
        const DocumentComposition* previous =
            before.FindComposition(composition.id);
        if (previous != nullptr &&
            (previous->name != composition.name ||
             previous->definition.Dump() != composition.definition.Dump())) {
            delta.changed_composition_ids.push_back(composition.id);
        }
    }
    const auto diffIds = [](const std::vector<Ulid>& from,
                            const std::vector<Ulid>& to) {
        DeltaIdentities identities;
        const std::set<Ulid> had(from.begin(), from.end());
        const std::set<Ulid> has(to.begin(), to.end());
        for (const Ulid& id : to) {
            if (had.count(id) == 0) {
                identities.created.push_back(id);
            }
        }
        for (const Ulid& id : from) {
            if (has.count(id) == 0) {
                identities.removed.push_back(id);
            }
        }
        return identities;
    };
    delta.motion_nodes = diffIds(nodesBefore, nodesAfter);
    delta.keyframes = diffIds(keyframesBefore, keyframesAfter);

    const std::map<Ulid, Placed> oldClips = IndexClips(before);
    const std::map<Ulid, Placed> newClips = IndexClips(after);
    for (const auto& entry : oldClips)
        if (!newClips.count(entry.first))
            delta.removed_clip_ids.push_back(entry.first);

    // Pure moves are held back: they may fold into a shift rule, and only
    // the ones that do not are published individually.
    std::vector<std::pair<Placed, Placed>> moved;  // before, after
    std::vector<DeltaClip> individual;
    for (const auto& entry : newClips) {
        const auto found = oldClips.find(entry.first);
        if (found == oldClips.end()) {
            individual.push_back(Describe(entry.second, true));
            continue;
        }
        if (found->second.track->id == entry.second.track->id &&
            ClipsEqual(*found->second.clip, *entry.second.clip))
            continue;  // untouched
        if (OnlyMoved(found->second, entry.second)) {
            moved.push_back({found->second, entry.second});
            continue;
        }
        individual.push_back(Describe(entry.second, false, found->second.clip));
    }

    // A rule stands only for a stretch of clips that sat side by side on the
    // track before the mutation and all moved by the same amount. It is
    // bounded by `until`, the old position of the next clip, unless it runs
    // to the end of the track; anything looser would let a caller apply it
    // to a clip that did not actually move. A lone moved clip is clearer as
    // its own resulting state than as a rule of one.
    std::map<Ulid, std::pair<Placed, RationalTime>> movedBy;
    for (const auto& pair : moved) {
        movedBy[pair.first.clip->id] = {
            pair.second,
            pair.second.clip->timeline_in.sub(pair.first.clip->timeline_in)};
    }
    for (const DocumentTrack& track : before.sequence.tracks) {
        std::vector<const DocumentClip*> order;
        for (const DocumentClip& clip : track.clips) order.push_back(&clip);
        std::stable_sort(
            order.begin(), order.end(),
            [](const DocumentClip* left, const DocumentClip* right) {
                return left->timeline_in.compare(right->timeline_in) < 0;
            });
        size_t first = 0;
        while (first < order.size()) {
            const auto found = movedBy.find(order[first]->id);
            if (found == movedBy.end()) {
                ++first;
                continue;
            }
            const RationalTime by = found->second.second;
            size_t end = first + 1;
            while (end < order.size()) {
                const auto next = movedBy.find(order[end]->id);
                if (next == movedBy.end() ||
                    next->second.second.compare(by) != 0)
                    break;
                ++end;
            }
            const bool toTrackEnd = end == order.size();
            // Two clips at one position cannot be told apart by a bound.
            const bool bounded =
                toTrackEnd || order[end]->timeline_in.compare(
                                  order[end - 1]->timeline_in) > 0;
            if (bounded && end - first >= 2) {
                DeltaShift shift;
                shift.track_id = track.id;
                shift.from = order[first]->timeline_in;
                shift.by = by;
                shift.count = static_cast<int32_t>(end - first);
                if (!toTrackEnd) shift.until = order[end]->timeline_in;
                delta.shifted.push_back(std::move(shift));
            } else {
                for (size_t index = first; index < end; ++index)
                    individual.push_back(Describe(
                        movedBy.find(order[index]->id)->second.first, false));
            }
            first = end;
        }
    }

    std::stable_sort(individual.begin(), individual.end(),
                     [](const DeltaClip& left, const DeltaClip& right) {
                         return left.timeline_in.compare(right.timeline_in) < 0;
                     });
    delta.clips = std::move(individual);
    return !delta.clips.empty() || !delta.removed_clip_ids.empty() ||
           !delta.shifted.empty() || !delta.created_tracks.empty() ||
           !delta.removed_track_ids.empty() || delta.sequence_changed;
}

std::string SerializeDocumentDelta(const DocumentDelta& delta) {
    Value root = Value::MakeObject();
    root.Set("duration", TimeValue(delta.duration));
    if (!delta.clips.empty()) {
        Value clips = Value::MakeArray();
        for (const DeltaClip& clip : delta.clips) {
            Value entry = Value::MakeObject();
            entry.Set("clip_id", Value::MakeString(clip.clip_id));
            entry.Set("track_id", Value::MakeString(clip.track_id));
            entry.Set("source_id", Value::MakeString(clip.source_id));
            entry.Set("source_in", TimeValue(clip.source_in));
            entry.Set("duration", TimeValue(clip.duration));
            entry.Set("timeline_in", TimeValue(clip.timeline_in));
            if (clip.speed) {
                Value speed = Value::MakeObject();
                speed.Set("num", Value::MakeInt(clip.speed->num));
                speed.Set("den", Value::MakeInt(clip.speed->den));
                entry.Set("speed", std::move(speed));
            }
            if (clip.speed_ramp)
                entry.Set("speed_ramp", Value::MakeBool(*clip.speed_ramp));
            if (clip.enabled)
                entry.Set("enabled", Value::MakeBool(*clip.enabled));
            if (clip.created) entry.Set("created", Value::MakeBool(true));
            clips.Push(std::move(entry));
        }
        root.Set("clips", std::move(clips));
    }
    if (!delta.shifted.empty()) {
        Value shifted = Value::MakeArray();
        for (const DeltaShift& shift : delta.shifted) {
            Value entry = Value::MakeObject();
            entry.Set("track_id", Value::MakeString(shift.track_id));
            entry.Set("from", TimeValue(shift.from));
            entry.Set("by", TimeValue(shift.by));
            entry.Set("count", Value::MakeInt(shift.count));
            if (shift.until) entry.Set("until", TimeValue(*shift.until));
            shifted.Push(std::move(entry));
        }
        root.Set("shifted", std::move(shifted));
    }
    if (!delta.removed_clip_ids.empty()) {
        Value removed = Value::MakeArray();
        for (const Ulid& id : delta.removed_clip_ids)
            removed.Push(Value::MakeString(id));
        root.Set("removed_clip_ids", std::move(removed));
    }
    if (!delta.created_tracks.empty()) {
        Value tracks = Value::MakeArray();
        for (const DeltaTrack& track : delta.created_tracks) {
            Value entry = Value::MakeObject();
            if (!track.alias.empty())
                entry.Set("alias", Value::MakeString(track.alias));
            entry.Set("track_id", Value::MakeString(track.track_id));
            entry.Set("kind", Value::MakeString(track.kind));
            entry.Set("index", Value::MakeInt(track.index));
            tracks.Push(std::move(entry));
        }
        root.Set("created_tracks", std::move(tracks));
    }
    if (!delta.removed_track_ids.empty()) {
        Value removed = Value::MakeArray();
        for (const Ulid& id : delta.removed_track_ids)
            removed.Push(Value::MakeString(id));
        root.Set("removed_track_ids", std::move(removed));
    }
    SetIdentities(root, "caption_style", delta.caption_styles);
    SetIdentities(root, "marker", delta.markers);
    SetIdentities(root, "transition", delta.transitions);
    SetIdentities(root, "bin", delta.bins);
    SetIdentities(root, "composition", delta.compositions);
    if (!delta.changed_composition_ids.empty()) {
        Value changed = Value::MakeArray();
        for (const Ulid& id : delta.changed_composition_ids) {
            changed.Push(Value::MakeString(id));
        }
        root.Set("changed_composition_ids", std::move(changed));
    }
    SetIdentities(root, "motion_node", delta.motion_nodes);
    SetIdentities(root, "keyframe", delta.keyframes);
    if (delta.sequence_changed)
        root.Set("sequence_changed", Value::MakeBool(true));
    return root.Dump();
}
