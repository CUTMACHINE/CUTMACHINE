// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/engine/src/trim.rs (MIT OR Apache-2.0, commit 09aaf9b); see
// TrimSession.h for what is kept and what is not.

#include "TrimSession.h"

#include "ClipSpeed.h"
#include "MotionClip.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace trim {

namespace {

bool Contains(const std::vector<Ulid>& ids, const Ulid& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void AddUnique(std::vector<Ulid>& ids, const Ulid& id) {
    if (!id.empty() && !Contains(ids, id)) ids.push_back(id);
}

RationalTime EndOf(const DocumentClip& clip) {
    return clip.timeline_in.add(clip.duration);
}

bool IsCaptionTrack(const DocumentTrack* track) {
    return track != nullptr && track->kind == "caption";
}

// `frame` times `count`, exactly, or false on overflow.
bool FramesTime(const RationalTime& frame, int64_t count,
                RationalTime& output) {
    const __int128 value = static_cast<__int128>(frame.value) * count;
    if (value > std::numeric_limits<int64_t>::max() ||
        value < std::numeric_limits<int64_t>::min())
        return false;
    output = {static_cast<int64_t>(value), frame.rate};
    return true;
}

// The clip on `track` other than `except` whose edge lies at `time`.
const DocumentClip* ClipEndingAt(const DocumentTrack& track,
                                 const RationalTime& time,
                                 const Ulid& except = {}) {
    for (const DocumentClip& clip : track.clips)
        if (clip.id != except && EndOf(clip) == time) return &clip;
    return nullptr;
}

const DocumentClip* ClipStartingAt(const DocumentTrack& track,
                                   const RationalTime& time,
                                   const Ulid& except = {}) {
    for (const DocumentClip& clip : track.clips)
        if (clip.id != except && clip.timeline_in == time) return &clip;
    return nullptr;
}

// Whether a point has the clips its side needs. Stale ids read as absent.
bool SideAvailable(const Document& document, const TrimPoint& point,
                   TrimSide side) {
    const DocumentClip* out = point.outgoing_clip_id.empty()
                                  ? nullptr
                                  : document.FindClip(point.outgoing_clip_id);
    const DocumentClip* in = point.incoming_clip_id.empty()
                                 ? nullptr
                                 : document.FindClip(point.incoming_clip_id);
    switch (side) {
        case TrimSide::A:
            return out != nullptr;
        case TrimSide::B:
            return in != nullptr;
        case TrimSide::Roll:
            return out != nullptr && in != nullptr &&
                   document.FindTrackForClip(out->id) ==
                       document.FindTrackForClip(in->id) &&
                   EndOf(*out) == in->timeline_in;
    }
    return false;
}

TrimSide NextSide(TrimSide side) {
    switch (side) {
        case TrimSide::A:
            return TrimSide::Roll;
        case TrimSide::Roll:
            return TrimSide::B;
        case TrimSide::B:
            return TrimSide::A;
    }
    return TrimSide::A;
}

TrimEdit Refuse(TrimRefusal refusal, const Ulid& clipId = {},
                std::string detail = {}) {
    TrimEdit edit;
    edit.refusal = refusal;
    edit.refused_clip_id = clipId;
    edit.detail = std::move(detail);
    return edit;
}

// One point, resolved against the document and turned into what its
// operation needs.
struct Resolved {
    TrimSide side = TrimSide::Roll;
    const DocumentClip* out = nullptr;
    const DocumentClip* in = nullptr;
    const DocumentTrack* track = nullptr;
    RationalTime movement{0, 1};
    // The linked partners that follow the moving edge (A or B side).
    std::vector<Ulid> partners;
    // A roll's pairs: the point's own first, then its linked partners'.
    std::vector<RollEditPair> pairs;
    bool skipped = false;
};

// Checks one edge against its media, its length and, for a plain trim, its
// neighbours, before the operation is tried: so that the refusal is named
// and names the clip, instead of reading the operation's message.
TrimRefusal CheckEdge(const Document& document, const DocumentClip& clip,
                      TrimEdge edge, const RationalTime& delta, bool plainTrim,
                      std::string& detail) {
    const DocumentTrack* track = document.FindTrackForClip(clip.id);
    if (track != nullptr && track->locked) return TrimRefusal::LockedTrack;
    const std::optional<DocumentSource> source =
        IsCaptionTrack(track) ? std::nullopt : ClipSource(document, clip);
    if (edge == TrimEdge::Tail) {
        const RationalTime duration = clip.duration.add(delta);
        if (duration.value <= 0) return TrimRefusal::ClipTooShort;
        if (source && delta.value > 0) {
            const RationalTime end = clip.source_in.add(
                ClipSourceAdvanceBetween(clip, {0, 1}, duration));
            if (end > source->duration) {
                detail = "the source ends at " +
                         std::to_string(source->duration.value) + "/" +
                         std::to_string(source->duration.rate);
                return TrimRefusal::MediaLimit;
            }
        }
        if (plainTrim && delta.value > 0 && track != nullptr) {
            const RationalTime oldEnd = EndOf(clip);
            const RationalTime newEnd = clip.timeline_in.add(duration);
            for (const DocumentClip& other : track->clips) {
                if (other.id != clip.id && other.timeline_in >= oldEnd &&
                    other.timeline_in < newEnd)
                    return TrimRefusal::BlockedByNeighbour;
            }
        }
        return TrimRefusal::None;
    }
    const RationalTime duration = clip.duration.sub(delta);
    if (duration.value <= 0) return TrimRefusal::ClipTooShort;
    if (source && delta.value < 0) {
        const RationalTime sourceIn =
            clip.source_in.add(ClipSourceAdvance(clip, delta));
        if (sourceIn.value < 0) {
            detail = "the source starts at 0";
            return TrimRefusal::MediaLimit;
        }
    }
    if (plainTrim && delta.value < 0) {
        const RationalTime newStart = clip.timeline_in.add(delta);
        if (newStart.value < 0) {
            detail = "the timeline starts at 0";
            return TrimRefusal::BlockedByNeighbour;
        }
        if (track != nullptr) {
            for (const DocumentClip& other : track->clips) {
                const RationalTime otherEnd = EndOf(other);
                if (other.id != clip.id && otherEnd > newStart &&
                    otherEnd <= clip.timeline_in)
                    return TrimRefusal::BlockedByNeighbour;
            }
        }
    }
    return TrimRefusal::None;
}

TrimRefusal RefusalOf(EditError error) {
    switch (error) {
        case EditError::Overlap:
        case EditError::InvalidTimelineIn:
            return TrimRefusal::BlockedByNeighbour;
        case EditError::LockedTrack:
            return TrimRefusal::LockedTrack;
        case EditError::InvalidDuration:
            return TrimRefusal::ClipTooShort;
        case EditError::UnknownClip:
            return TrimRefusal::StaleEditPoint;
        default:
            return TrimRefusal::Rejected;
    }
}

}  // namespace

const char* TrimRefusalName(TrimRefusal refusal) {
    switch (refusal) {
        case TrimRefusal::None:
            return "None";
        case TrimRefusal::NoEditPoint:
            return "NoEditPoint";
        case TrimRefusal::StaleEditPoint:
            return "StaleEditPoint";
        case TrimRefusal::NoJunction:
            return "NoJunction";
        case TrimRefusal::MediaLimit:
            return "MediaLimit";
        case TrimRefusal::ClipTooShort:
            return "ClipTooShort";
        case TrimRefusal::BlockedByNeighbour:
            return "BlockedByNeighbour";
        case TrimRefusal::LockedTrack:
            return "LockedTrack";
        case TrimRefusal::InvalidFrameRate:
            return "InvalidFrameRate";
        case TrimRefusal::NoMovement:
            return "NoMovement";
        case TrimRefusal::Rejected:
            return "Rejected";
    }
    return "Rejected";
}

const char* TrimRefusalDescription(TrimRefusal refusal) {
    switch (refusal) {
        case TrimRefusal::None:
            return "";
        case TrimRefusal::NoEditPoint:
            return "Aucun point de montage sur les pistes ciblées.";
        case TrimRefusal::StaleEditPoint:
            return "Le point de montage sélectionné n’existe plus.";
        case TrimRefusal::NoJunction:
            return "Il n’y a pas de plan de ce côté du point de montage.";
        case TrimRefusal::MediaLimit:
            return "Le média n’a plus d’images au-delà de ce point.";
        case TrimRefusal::ClipTooShort:
            return "Le plan n’aurait plus aucune durée.";
        case TrimRefusal::BlockedByNeighbour:
            return "Le plan voisin bloque ce trim.";
        case TrimRefusal::LockedTrack:
            return "La piste est verrouillée.";
        case TrimRefusal::InvalidFrameRate:
            return "La cadence de la timeline n’est pas valide.";
        case TrimRefusal::NoMovement:
            return "Le point de montage est déjà à cette position.";
        case TrimRefusal::Rejected:
            return "Le trim est refusé.";
    }
    return "Le trim est refusé.";
}

bool SequenceFrame(const Document& document, RationalTime& frame) {
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) return false;
    frame = {rate.den, rate.num};
    return true;
}

bool EditPointTime(const Document& document, const TrimPoint& point,
                   RationalTime& time) {
    const DocumentClip* out = point.outgoing_clip_id.empty()
                                  ? nullptr
                                  : document.FindClip(point.outgoing_clip_id);
    const DocumentClip* in = point.incoming_clip_id.empty()
                                 ? nullptr
                                 : document.FindClip(point.incoming_clip_id);
    // The edge that moves: B reads the incoming head, which a plain trim of
    // A may have left apart from the outgoing tail.
    if (point.side == TrimSide::B && in != nullptr) {
        time = in->timeline_in;
        return true;
    }
    if (out != nullptr) {
        time = EndOf(*out);
        return true;
    }
    if (in != nullptr) {
        time = in->timeline_in;
        return true;
    }
    return false;
}

TrimEdit PlanTrim(const Document& document,
                  const std::vector<TrimPoint>& points,
                  const std::vector<RationalTime>& movements, TrimStyle style,
                  bool followLinks) {
    if (points.empty()) return Refuse(TrimRefusal::NoEditPoint);
    if (movements.size() != points.size())
        return Refuse(TrimRefusal::Rejected, {},
                      "one movement per edit point is required");
    const bool plainTrim = style == TrimStyle::Trim;

    // First pass: every point against the document, with the clips that
    // follow it, so that the second knows every track that trims itself.
    std::vector<Resolved> resolved(points.size());
    std::vector<std::pair<Ulid, TrimEdge>> covered;
    const auto isCovered = [&](const Ulid& id, TrimEdge edge) {
        return std::find(covered.begin(), covered.end(),
                         std::make_pair(id, edge)) != covered.end();
    };
    try {
        for (size_t index = 0; index < points.size(); ++index) {
            const TrimPoint& point = points[index];
            Resolved& item = resolved[index];
            item.side = point.side;
            item.movement = movements[index];
            if (!point.outgoing_clip_id.empty()) {
                item.out = document.FindClip(point.outgoing_clip_id);
                if (item.out == nullptr)
                    return Refuse(TrimRefusal::StaleEditPoint,
                                  point.outgoing_clip_id);
            }
            if (!point.incoming_clip_id.empty()) {
                item.in = document.FindClip(point.incoming_clip_id);
                if (item.in == nullptr)
                    return Refuse(TrimRefusal::StaleEditPoint,
                                  point.incoming_clip_id);
            }
            if (!SideAvailable(document, point, point.side))
                return Refuse(TrimRefusal::NoJunction,
                              item.out != nullptr  ? item.out->id
                              : item.in != nullptr ? item.in->id
                                                   : Ulid{});
            const DocumentClip* anchor =
                point.side == TrimSide::B ? item.in : item.out;
            item.track = document.FindTrackForClip(anchor->id);
            if (item.track != nullptr && item.track->locked)
                return Refuse(TrimRefusal::LockedTrack, anchor->id);
            if (item.movement.value == 0) {
                item.skipped = true;
                continue;
            }
            if (point.side == TrimSide::Roll) {
                if (isCovered(item.out->id, TrimEdge::Tail) ||
                    isCovered(item.in->id, TrimEdge::Head)) {
                    item.skipped = true;
                    continue;
                }
                item.pairs.push_back({item.out->id, item.in->id});
                if (followLinks && !item.out->link_group_id.empty() &&
                    !item.in->link_group_id.empty()) {
                    const std::vector<Ulid> rights = LinkedEdgePartnerIds(
                        document, *item.in, TrimEdge::Head);
                    for (const Ulid& leftId : LinkedEdgePartnerIds(
                             document, *item.out, TrimEdge::Tail)) {
                        const DocumentClip* left = document.FindClip(leftId);
                        const DocumentTrack* track =
                            document.FindTrackForClip(leftId);
                        if (left == nullptr || track == nullptr) continue;
                        const DocumentClip* right =
                            ClipStartingAt(*track, EndOf(*left), leftId);
                        if (right != nullptr && Contains(rights, right->id) &&
                            !isCovered(left->id, TrimEdge::Tail) &&
                            !isCovered(right->id, TrimEdge::Head))
                            item.pairs.push_back({left->id, right->id});
                    }
                }
                for (const RollEditPair& pair : item.pairs) {
                    covered.emplace_back(pair.left_clip_id, TrimEdge::Tail);
                    covered.emplace_back(pair.right_clip_id, TrimEdge::Head);
                }
                continue;
            }
            const TrimEdge edge =
                point.side == TrimSide::A ? TrimEdge::Tail : TrimEdge::Head;
            if (isCovered(anchor->id, edge)) {
                item.skipped = true;
                continue;
            }
            if (followLinks) {
                for (const Ulid& partner :
                     LinkedEdgePartnerIds(document, *anchor, edge))
                    if (!isCovered(partner, edge))
                        item.partners.push_back(partner);
            }
            covered.emplace_back(anchor->id, edge);
            for (const Ulid& partner : item.partners)
                covered.emplace_back(partner, edge);
        }
    } catch (const std::exception& exception) {
        return Refuse(TrimRefusal::Rejected, {}, exception.what());
    }

    // The tracks that trim themselves never follow another point's ripple:
    // in Premiere and Resolve a track with an edit point does its own trim,
    // and only the sync-locked tracks without one follow. They follow the
    // first ripple point's track (the lead): every ripple on it carries
    // them, so two cuts on one track move them twice, and the same cut on
    // V1 and V2 moves them once.
    std::vector<Ulid> trimmingTracks;
    const Ulid* leadTrack = nullptr;
    for (const Resolved& item : resolved) {
        if (item.track == nullptr) continue;
        AddUnique(trimmingTracks, item.track->id);
        for (const Ulid& partner : item.partners) {
            const DocumentTrack* track = document.FindTrackForClip(partner);
            if (track != nullptr) AddUnique(trimmingTracks, track->id);
        }
        for (const RollEditPair& pair : item.pairs) {
            const DocumentTrack* track =
                document.FindTrackForClip(pair.left_clip_id);
            if (track != nullptr) AddUnique(trimmingTracks, track->id);
        }
        if (leadTrack == nullptr && !item.skipped &&
            item.side != TrimSide::Roll && style == TrimStyle::Ripple)
            leadTrack = &item.track->id;
    }
    // Every sync-locked track follows, locked ones included, as the drag
    // and ripple_trim name them: the engine then refuses a ripple that would
    // leave a locked track behind (LockedTrackTouchedBy) instead of letting
    // its clips fall out of sync.
    std::vector<Ulid> syncTracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.sync_lock && !Contains(trimmingTracks, track.id))
            syncTracks.push_back(track.id);

    // Second pass: the named checks, then the operations.
    std::vector<Operation> operations;
    try {
        for (const Resolved& item : resolved) {
            if (item.skipped) continue;
            std::string detail;
            if (item.side == TrimSide::Roll) {
                for (const RollEditPair& pair : item.pairs) {
                    const DocumentClip* left =
                        document.FindClip(pair.left_clip_id);
                    const DocumentClip* right =
                        document.FindClip(pair.right_clip_id);
                    TrimRefusal refusal =
                        CheckEdge(document, *left, TrimEdge::Tail,
                                  item.movement, false, detail);
                    if (refusal != TrimRefusal::None)
                        return Refuse(refusal, left->id, detail);
                    refusal = CheckEdge(document, *right, TrimEdge::Head,
                                        item.movement, false, detail);
                    if (refusal != TrimRefusal::None)
                        return Refuse(refusal, right->id, detail);
                }
                operations.push_back(
                    RollEditOperation{item.pairs, item.movement, {}});
                continue;
            }
            const DocumentClip* anchor =
                item.side == TrimSide::B ? item.in : item.out;
            const TrimEdge edge =
                item.side == TrimSide::A ? TrimEdge::Tail : TrimEdge::Head;
            std::vector<Ulid> members{anchor->id};
            for (const Ulid& partner : item.partners)
                members.push_back(partner);
            for (const Ulid& id : members) {
                const DocumentClip* clip = document.FindClip(id);
                const TrimRefusal refusal = CheckEdge(
                    document, *clip, edge, item.movement, plainTrim, detail);
                if (refusal != TrimRefusal::None)
                    return Refuse(refusal, id, detail);
            }
            if (style == TrimStyle::Ripple) {
                const bool lead =
                    leadTrack != nullptr && item.track->id == *leadTrack;
                operations.push_back(
                    RippleTrimOperation{anchor->id,
                                        edge,
                                        item.movement,
                                        item.partners,
                                        lead ? syncTracks : std::vector<Ulid>{},
                                        {}});
            } else if (item.partners.empty()) {
                operations.push_back(TrimClipOperation{
                    anchor->id, edge, item.movement, std::nullopt});
            } else {
                std::vector<LinkedClipTrim> trims;
                for (const Ulid& id : members)
                    trims.push_back({id, edge, item.movement});
                operations.push_back(TrimLinkedClipsOperation{
                    anchor->link_group_id, std::move(trims), {}});
            }
        }
    } catch (const std::exception& exception) {
        return Refuse(TrimRefusal::Rejected, {}, exception.what());
    }
    if (operations.empty()) return Refuse(TrimRefusal::NoMovement);
    if (operations.size() > kMaxBatchOperations)
        return Refuse(TrimRefusal::Rejected, {},
                      "more edit points than one undo step holds");

    TrimEdit edit;
    edit.movements = movements;
    if (operations.size() == 1) {
        edit.operation = std::move(operations.front());
    } else {
        edit.operation = Operation{
            BatchOperation{"trim de " + std::to_string(operations.size()) +
                               " points de montage",
                           std::move(operations)}};
    }
    // The operation itself is the last judge: what the named checks do not
    // see (an overlap a ripple pushes into, a caption rule) is its refusal.
    Document candidate = document;
    Operation trial = *edit.operation;
    Operation inverse = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!ApplyOperation(candidate, trial, inverse, error, message))
        return Refuse(RefusalOf(error), {}, message);
    return edit;
}

void TrimSession::Clear() {
    points_.clear();
    dynamic_ = {};
}

TrimRefusal TrimSession::SelectNearest(
    const Document& document, const RationalTime& playhead,
    const std::vector<Ulid>& targetedTrackIds, TrimSide side) {
    std::vector<TrimPoint> selected;
    try {
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (!targetedTrackIds.empty() &&
                !Contains(targetedTrackIds, track.id))
                continue;
            if (track.locked) continue;
            std::optional<TrimPoint> best;
            RationalTime bestTime;
            RationalTime bestDistance;
            for (const DocumentClip& clip : track.clips) {
                for (const RationalTime& time :
                     {clip.timeline_in, EndOf(clip)}) {
                    TrimPoint point;
                    point.side = side;
                    if (const DocumentClip* out = ClipEndingAt(track, time))
                        point.outgoing_clip_id = out->id;
                    if (const DocumentClip* in = ClipStartingAt(track, time))
                        point.incoming_clip_id = in->id;
                    if (!SideAvailable(document, point, side)) continue;
                    RationalTime distance = time.sub(playhead);
                    if (distance.value < 0) distance.value = -distance.value;
                    if (!best || distance < bestDistance ||
                        (distance == bestDistance && time < bestTime)) {
                        best = point;
                        bestTime = time;
                        bestDistance = distance;
                    }
                }
            }
            if (best) selected.push_back(*best);
        }
    } catch (const std::exception&) {
        return TrimRefusal::Rejected;
    }
    if (selected.empty()) return TrimRefusal::NoEditPoint;
    dynamic_ = {};
    points_ = std::move(selected);
    return TrimRefusal::None;
}

bool TrimSession::StillHolds(const Document& document) const {
    return std::all_of(points_.begin(), points_.end(),
                       [&](const TrimPoint& point) {
                           return SideAvailable(document, point, point.side);
                       });
}

TrimRefusal TrimSession::SelectNearestEdit(
    const Document& document, const RationalTime& playhead,
    const std::vector<Ulid>& targetedTrackIds, TrimSide side,
    RationalTime& editTime) {
    // Every candidate point, with its time, on the targeted unlocked
    // tracks; then the nearest time, then the points at that time.
    struct Candidate {
        TrimPoint point;
        RationalTime time;
    };
    std::vector<Candidate> candidates;
    std::optional<RationalTime> best;
    RationalTime bestDistance;
    try {
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (!Contains(targetedTrackIds, track.id) || track.locked) continue;
            for (const DocumentClip& clip : track.clips) {
                for (const RationalTime& time :
                     {clip.timeline_in, EndOf(clip)}) {
                    TrimPoint point;
                    point.side = side;
                    if (const DocumentClip* out = ClipEndingAt(track, time))
                        point.outgoing_clip_id = out->id;
                    if (const DocumentClip* in = ClipStartingAt(track, time))
                        point.incoming_clip_id = in->id;
                    if (!SideAvailable(document, point, side)) continue;
                    RationalTime distance = time.sub(playhead);
                    if (distance.value < 0) distance.value = -distance.value;
                    if (!best || distance < bestDistance ||
                        (distance == bestDistance && time < *best)) {
                        best = time;
                        bestDistance = distance;
                    }
                    candidates.push_back({point, time});
                }
            }
        }
    } catch (const std::exception&) {
        return TrimRefusal::Rejected;
    }
    if (!best) return TrimRefusal::NoEditPoint;
    std::vector<TrimPoint> selected;
    for (const Candidate& candidate : candidates) {
        if (candidate.time != *best) continue;
        // A cut is reached from both of its clips: keep it once.
        const bool known = std::any_of(
            selected.begin(), selected.end(), [&](const TrimPoint& point) {
                return point.outgoing_clip_id ==
                           candidate.point.outgoing_clip_id &&
                       point.incoming_clip_id ==
                           candidate.point.incoming_clip_id;
            });
        if (!known) selected.push_back(candidate.point);
    }
    dynamic_ = {};
    points_ = std::move(selected);
    editTime = *best;
    return TrimRefusal::None;
}

TrimRefusal TrimSession::SelectEdge(const Document& document,
                                    const Ulid& clipId, TrimEdge edge,
                                    TrimSide side, bool add) {
    const DocumentClip* clip = document.FindClip(clipId);
    const DocumentTrack* track = document.FindTrackForClip(clipId);
    if (clip == nullptr || track == nullptr) return TrimRefusal::NoEditPoint;
    if (track->locked) return TrimRefusal::LockedTrack;
    TrimPoint point;
    point.side = side;
    if (edge == TrimEdge::Tail) {
        point.outgoing_clip_id = clip->id;
        if (const DocumentClip* in =
                ClipStartingAt(*track, EndOf(*clip), clip->id))
            point.incoming_clip_id = in->id;
    } else {
        point.incoming_clip_id = clip->id;
        if (const DocumentClip* out =
                ClipEndingAt(*track, clip->timeline_in, clip->id))
            point.outgoing_clip_id = out->id;
    }
    if (!SideAvailable(document, point, side)) return TrimRefusal::NoJunction;
    dynamic_ = {};
    if (!add) {
        points_ = {point};
        return TrimRefusal::None;
    }
    for (auto it = points_.begin(); it != points_.end(); ++it) {
        if (it->outgoing_clip_id == point.outgoing_clip_id &&
            it->incoming_clip_id == point.incoming_clip_id) {
            points_.erase(it);
            return TrimRefusal::None;
        }
    }
    points_.push_back(point);
    return TrimRefusal::None;
}

TrimRefusal TrimSession::CycleSide(const Document& document) {
    if (points_.empty()) return TrimRefusal::NoEditPoint;
    if (dynamic_.active) return TrimRefusal::Rejected;
    std::vector<TrimPoint> cycled = points_;
    for (TrimPoint& point : cycled) {
        TrimSide next = point.side;
        bool found = false;
        for (int step = 0; step < 3 && !found; ++step) {
            next = NextSide(next);
            found = SideAvailable(document, point, next);
        }
        if (!found) return TrimRefusal::StaleEditPoint;
        point.side = next;
    }
    points_ = std::move(cycled);
    return TrimRefusal::None;
}

TrimRefusal TrimSession::SetSide(const Document& document, TrimSide side) {
    if (points_.empty()) return TrimRefusal::NoEditPoint;
    if (dynamic_.active) return TrimRefusal::Rejected;
    if (!CanSetSide(document, side)) return TrimRefusal::NoJunction;
    for (TrimPoint& point : points_) point.side = side;
    return TrimRefusal::None;
}

bool TrimSession::CanSetSide(const Document& document, TrimSide side) const {
    if (points_.empty()) return false;
    for (const TrimPoint& point : points_)
        if (!SideAvailable(document, point, side)) return false;
    return true;
}

TrimEdit TrimSession::PlanUniform(const Document& document,
                                  const RationalTime& movement) const {
    return PlanTrim(document, points_,
                    std::vector<RationalTime>(points_.size(), movement), style_,
                    follow_links_);
}

TrimEdit TrimSession::PlanNudge(const Document& document,
                                int64_t frames) const {
    if (points_.empty()) return Refuse(TrimRefusal::NoEditPoint);
    RationalTime frame;
    if (!SequenceFrame(document, frame))
        return Refuse(TrimRefusal::InvalidFrameRate);
    if (frames == 0) return Refuse(TrimRefusal::NoMovement);
    RationalTime movement;
    if (!FramesTime(frame, frames, movement))
        return Refuse(TrimRefusal::Rejected, {}, "the nudge overflows");
    return PlanUniform(document, movement);
}

TrimEdit TrimSession::PlanExtendToPlayhead(const Document& document,
                                           const RationalTime& playhead) const {
    if (points_.empty()) return Refuse(TrimRefusal::NoEditPoint);
    // Each movement is measured on the document before the batch, which
    // then ripples step by step: a ripple moves the points after it, which
    // would miss the playhead. Refused rather than applied off target.
    if (style_ == TrimStyle::Ripple && points_.size() > 1 &&
        std::any_of(points_.begin(), points_.end(), [](const TrimPoint& p) {
            return p.side != TrimSide::Roll;
        }))
        return Refuse(TrimRefusal::Rejected, {},
                      "extend to the playhead takes one point at a time in "
                      "ripple: a ripple would move the other points");
    std::vector<RationalTime> movements;
    try {
        for (const TrimPoint& point : points_) {
            RationalTime time;
            if (!EditPointTime(document, point, time))
                return Refuse(TrimRefusal::StaleEditPoint,
                              point.outgoing_clip_id.empty()
                                  ? point.incoming_clip_id
                                  : point.outgoing_clip_id);
            movements.push_back(playhead.sub(time));
        }
    } catch (const std::exception& exception) {
        return Refuse(TrimRefusal::Rejected, {}, exception.what());
    }
    return PlanTrim(document, points_, movements, style_, follow_links_);
}

void TrimSession::RecordApplied(const TrimEdit& edit) {
    if (!edit.ok() || edit.movements.size() != points_.size()) return;
    for (size_t index = 0; index < points_.size(); ++index) {
        TrimPoint& point = points_[index];
        const RationalTime& movement = edit.movements[index];
        if (point.side != TrimSide::B)
            point.out_shift = point.out_shift.add(movement);
        if (point.side != TrimSide::A)
            point.in_shift = point.in_shift.add(movement);
    }
}

bool TrimSession::Apply(const TrimEdit& edit, Document& document, EditLog& log,
                        EditError& error, std::string& message) {
    if (!edit.ok()) {
        error = EditError::InvalidOperation;
        message = std::string("trim refused: ") +
                  TrimRefusalName(edit.refusal) +
                  (edit.detail.empty() ? "" : " (" + edit.detail + ")");
        return false;
    }
    if (!log.Apply(document, *edit.operation, error, message)) return false;
    RecordApplied(edit);
    return true;
}

bool TrimSession::OffsetApplies(const Document& document,
                                int64_t frames) const {
    if (frames == 0) return true;
    RationalTime frame;
    RationalTime movement;
    if (!SequenceFrame(document, frame) || !FramesTime(frame, frames, movement))
        return false;
    return PlanUniform(document, movement).ok();
}

TrimRefusal TrimSession::Shuttle(const Document& document, int direction,
                                 const RationalTime& now) {
    if (points_.empty()) return TrimRefusal::NoEditPoint;
    RationalTime frame;
    if (!SequenceFrame(document, frame)) return TrimRefusal::InvalidFrameRate;
    if (direction == 0) return TrimRefusal::NoMovement;
    const int sign = direction < 0 ? -1 : 1;
    if (!dynamic_.active) {
        dynamic_ = {};
        dynamic_.active = true;
        dynamic_.speed = sign;
        dynamic_.anchor_clock = now;
        return TrimRefusal::None;
    }
    Tick(document, now);
    if (dynamic_.speed * sign > 0 && !dynamic_.at_limit) {
        dynamic_.speed =
            std::clamp(dynamic_.speed * 2, -kMaxShuttleSpeed, kMaxShuttleSpeed);
    } else {
        dynamic_.speed = sign;
    }
    dynamic_.anchor_clock = now;
    dynamic_.anchor_offset_frames = dynamic_.offset_frames;
    dynamic_.at_limit = false;
    return TrimRefusal::None;
}

void TrimSession::Tick(const Document& document, const RationalTime& now) {
    if (!dynamic_.active || dynamic_.speed == 0) return;
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) return;
    int64_t want = dynamic_.anchor_offset_frames;
    try {
        const RationalTime elapsed = now.sub(dynamic_.anchor_clock);
        if (elapsed.value > 0) {
            // floor(elapsed * fps * |speed|), in integers: no float ever
            // stands between the clock and the frame count.
            const int magnitude =
                dynamic_.speed < 0 ? -dynamic_.speed : dynamic_.speed;
            const __int128 numerator =
                static_cast<__int128>(elapsed.value) * rate.num * magnitude;
            const __int128 denominator =
                static_cast<__int128>(elapsed.rate) * rate.den;
            const __int128 frames = numerator / denominator;
            const __int128 target =
                static_cast<__int128>(dynamic_.anchor_offset_frames) +
                (dynamic_.speed < 0 ? -frames : frames);
            if (target > std::numeric_limits<int64_t>::max() / 2 ||
                target < std::numeric_limits<int64_t>::min() / 2)
                return;
            want = static_cast<int64_t>(target);
        }
    } catch (const std::exception&) {
        return;
    }
    if (want == dynamic_.offset_frames) return;
    if (OffsetApplies(document, want)) {
        dynamic_.offset_frames = want;
        return;
    }
    // Held at the last offset that applies, between the one reached (which
    // does) and the one wanted (which does not). A trim's limits are
    // monotonic in its offset -- a handle, a length, a neighbour -- so the
    // boundary is found by bisection rather than by trying every frame.
    int64_t good = dynamic_.offset_frames;
    int64_t bad = want;
    while (bad - good > 1 || good - bad > 1) {
        const int64_t middle = good + (bad - good) / 2;
        if (OffsetApplies(document, middle))
            good = middle;
        else
            bad = middle;
    }
    dynamic_.offset_frames = good;
    dynamic_.anchor_offset_frames = good;
    dynamic_.anchor_clock = now;
    dynamic_.speed = 0;
    dynamic_.at_limit = true;
}

TrimEdit TrimSession::StopDynamic(const Document& document,
                                  const RationalTime& now) {
    if (!dynamic_.active) return Refuse(TrimRefusal::NoMovement);
    Tick(document, now);
    const int64_t offset = dynamic_.offset_frames;
    dynamic_ = {};
    if (offset == 0) return Refuse(TrimRefusal::NoMovement);
    RationalTime frame;
    RationalTime movement;
    if (!SequenceFrame(document, frame))
        return Refuse(TrimRefusal::InvalidFrameRate);
    if (!FramesTime(frame, offset, movement))
        return Refuse(TrimRefusal::Rejected, {}, "the offset overflows");
    return PlanUniform(document, movement);
}

bool TrimSession::CancelDynamic() {
    const bool was = dynamic_.active;
    dynamic_ = {};
    return was;
}

bool TrimSession::PreviewDocument(const Document& document,
                                  Document& preview) const {
    preview = document;
    if (!dynamic_.active || dynamic_.offset_frames == 0) return true;
    RationalTime frame;
    RationalTime movement;
    if (!SequenceFrame(document, frame) ||
        !FramesTime(frame, dynamic_.offset_frames, movement))
        return false;
    const TrimEdit edit = PlanUniform(document, movement);
    if (!edit.ok()) return false;
    Operation operation = *edit.operation;
    Operation inverse = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!ApplyOperation(preview, operation, inverse, error, message)) {
        preview = document;
        return false;
    }
    return true;
}

namespace {

void DescribeSide(const Document& shown, const Ulid& clipId, bool outgoing,
                  const RationalTime& frame, TrimMonitorSide& side) {
    if (clipId.empty()) return;
    const DocumentClip* clip = shown.FindClip(clipId);
    const DocumentTrack* track = shown.FindTrackForClip(clipId);
    if (clip == nullptr || track == nullptr) return;
    side.present = true;
    side.clip_id = clip->id;
    side.track_id = track->id;
    try {
        RationalTime offset{0, 1};
        if (outgoing) {
            offset = clip->duration.sub(frame);
            if (offset.value < 0) offset = {0, 1};
        }
        side.timeline_time = clip->timeline_in.add(offset);
        if (IsCaptionTrack(track)) return;
        const std::optional<DocumentSource> source = ClipSource(shown, *clip);
        if (!source) return;
        side.source_time = ClipSourceTimeAt(*clip, offset);
        side.source_frame = source->rate.num > 0 && source->rate.den > 0
                                ? ClipSourceFrameAt(*clip, offset, source->rate)
                                : -1;
        side.handle = outgoing ? source->duration.sub(ClipSourceEnd(*clip))
                               : clip->source_in;
        // At a constant speed the timeline covers handle / speed of it; a
        // ramp's extension has no single speed, a freeze no limit.
        const MediaRate rate = shown.sequence.frame_rate;
        if (!ClipHasSpeedRamp(*clip) && !ClipIsFreeze(*clip) && rate.num > 0 &&
            rate.den > 0) {
            side.handle_frames = UnscaleBySpeed(side.handle, clip->speed)
                                     .to_frames(rate.num, rate.den);
        }
        side.has_source = true;
    } catch (const std::exception&) {
        side.has_source = false;
    }
}

}  // namespace

TrimSide TrimSideOfEdge(TrimEdge edge, bool roll) {
    if (roll) return TrimSide::Roll;
    return edge == TrimEdge::Tail ? TrimSide::A : TrimSide::B;
}

std::vector<TimelineEdgeMark> TrimSessionEdgeMarks(const Document& document,
                                                   const TrimSession& session) {
    std::vector<TimelineEdgeMark> marks;
    const auto add = [&](const Ulid& clipId, TrimEdge edge) {
        if (clipId.empty()) return;
        const DocumentClip* clip = document.FindClip(clipId);
        if (clip == nullptr) return;
        if (std::any_of(marks.begin(), marks.end(),
                        [&](const TimelineEdgeMark& mark) {
                            return mark.clip_id == clipId && mark.edge == edge;
                        }))
            return;
        TimelineEdgeMark mark;
        mark.clip_id = clipId;
        mark.edge = edge;
        try {
            const DocumentTrack* track = document.FindTrackForClip(clipId);
            const std::optional<DocumentSource> source =
                IsCaptionTrack(track) || ClipIsFreeze(*clip)
                    ? std::nullopt
                    : ClipSource(document, *clip);
            if (source) {
                const RationalTime handle =
                    edge == TrimEdge::Tail
                        ? source->duration.sub(ClipSourceEnd(*clip))
                        : clip->source_in;
                mark.at_limit = handle.value <= 0;
            }
        } catch (const std::exception&) {
            mark.at_limit = false;
        }
        marks.push_back(mark);
    };
    for (const TrimPoint& point : session.Points()) {
        if (point.side != TrimSide::B)
            add(point.outgoing_clip_id, TrimEdge::Tail);
        if (point.side != TrimSide::A)
            add(point.incoming_clip_id, TrimEdge::Head);
    }
    return marks;
}

TrimMonitorView DescribeTrimMonitor(const Document& shown,
                                    const TrimSession& session, size_t index) {
    TrimMonitorView view;
    const std::vector<TrimPoint>& points = session.Points();
    if (index >= points.size()) return view;
    const TrimPoint& point = points[index];
    view.active = true;
    view.side = point.side;
    view.style = session.Style();
    view.point_count = points.size();
    view.dynamic = session.Dynamic();
    RationalTime frame{1, 1};
    const bool rateValid = SequenceFrame(shown, frame);
    EditPointTime(shown, point, view.edit_time);
    DescribeSide(shown, point.outgoing_clip_id, true, frame, view.outgoing);
    DescribeSide(shown, point.incoming_clip_id, false, frame, view.incoming);
    view.out_shift = point.out_shift;
    view.in_shift = point.in_shift;
    try {
        RationalTime offset;
        if (rateValid && view.dynamic.active &&
            view.dynamic.offset_frames != 0 &&
            FramesTime(frame, view.dynamic.offset_frames, offset)) {
            if (point.side != TrimSide::B)
                view.out_shift = view.out_shift.add(offset);
            if (point.side != TrimSide::A)
                view.in_shift = view.in_shift.add(offset);
        }
        if (rateValid) {
            const MediaRate rate = shown.sequence.frame_rate;
            view.out_shift_frames =
                view.out_shift.to_frames(rate.num, rate.den);
            view.in_shift_frames = view.in_shift.to_frames(rate.num, rate.den);
        }
    } catch (const std::exception&) {
    }
    return view;
}

namespace {

// `width` x `height` fitted whole in the box, centred, in whole pixels.
TrimMonitorPane FitPane(int64_t boxX, int64_t boxWidth, int64_t boxHeight,
                        int64_t width, int64_t height) {
    TrimMonitorPane pane;
    if (width <= 0 || height <= 0 || boxWidth <= 0 || boxHeight <= 0)
        return pane;
    // Compare width / height with boxWidth / boxHeight in integers.
    const __int128 left = static_cast<__int128>(width) * boxHeight;
    const __int128 right = static_cast<__int128>(boxWidth) * height;
    int64_t fittedWidth = boxWidth;
    int64_t fittedHeight = boxHeight;
    if (left >= right) {
        fittedHeight = static_cast<int64_t>(static_cast<__int128>(boxWidth) *
                                            height / width);
    } else {
        fittedWidth = static_cast<int64_t>(static_cast<__int128>(boxHeight) *
                                           width / height);
    }
    if (fittedWidth <= 0 || fittedHeight <= 0) return pane;
    pane.x = static_cast<int32_t>(boxX + (boxWidth - fittedWidth) / 2);
    pane.y = static_cast<int32_t>((boxHeight - fittedHeight) / 2);
    pane.width = static_cast<int32_t>(fittedWidth);
    pane.height = static_cast<int32_t>(fittedHeight);
    return pane;
}

std::string HandleText(const TrimMonitorSide& side) {
    if (!side.present) return "";
    if (!side.has_source) return "poignée : inconnue";
    if (side.handle_frames < 0) return "poignée : libre";
    return "poignée : " + std::to_string(side.handle_frames) +
           (side.handle_frames == 1 ? " image" : " images");
}

}  // namespace

TrimMonitorLayout LayoutTrimMonitor(
    int32_t sequenceWidth, int32_t sequenceHeight, int64_t outgoingWidth,
    int64_t outgoingHeight, int64_t incomingWidth, int64_t incomingHeight) {
    TrimMonitorLayout layout;
    if (sequenceWidth <= 1 || sequenceHeight <= 0) return layout;
    const int64_t half = sequenceWidth / 2;
    layout.outgoing =
        FitPane(0, half, sequenceHeight, outgoingWidth, outgoingHeight);
    layout.incoming = FitPane(half, sequenceWidth - half, sequenceHeight,
                              incomingWidth, incomingHeight);
    return layout;
}

std::string SignedFrames(int64_t frames) {
    if (frames > 0) return "+" + std::to_string(frames);
    return std::to_string(frames);
}

TrimMonitorText DescribeTrimMonitorText(const TrimMonitorView& view) {
    TrimMonitorText text;
    if (!view.active) return text;
    text.outgoing_title = view.outgoing.present ? "A · SORTIE" : "A · AUCUN";
    text.incoming_title = view.incoming.present ? "B · ENTRÉE" : "B · AUCUN";
    text.outgoing_shift = SignedFrames(view.out_shift_frames);
    text.incoming_shift = SignedFrames(view.in_shift_frames);
    text.outgoing_handle = HandleText(view.outgoing);
    text.incoming_handle = HandleText(view.incoming);
    if (view.side == TrimSide::Roll) {
        text.status = "ROLL";
    } else {
        text.status = view.style == TrimStyle::Ripple ? "RIPPLE" : "TRIM";
        text.status +=
            view.side == TrimSide::A ? " · sortie (A)" : " · entrée (B)";
    }
    if (view.point_count > 1)
        text.status += " · " + std::to_string(view.point_count) + " points";
    if (view.dynamic.active) {
        text.status += " · dynamique ";
        text.status +=
            view.dynamic.at_limit ? "en butée"
            : view.dynamic.speed == 0
                ? "en pause"
                : "×" +
                      std::to_string(view.dynamic.speed < 0
                                         ? -view.dynamic.speed
                                         : view.dynamic.speed) +
                      (view.dynamic.speed < 0 ? " arrière" : " avant");
    }
    return text;
}

}  // namespace trim
