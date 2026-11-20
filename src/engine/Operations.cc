#include "Operations.h"
#include "ShotDescriptions.h"

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "EditLog.h"
#include "MotionClip.h"
#include "MotionDocument.h"
#include "MotionOperations.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

// For the static_assert closing an exhaustive std::visit: dependent on T, so
// it fires only for an alternative that reaches the final branch.
template <typename>
inline constexpr bool kAlwaysFalse = false;

void Fail(EditError code, const std::string& text, EditError& error,
          std::string& message) {
    error = code;
    message = text;
}

ExactClipTimes TimesOf(const DocumentClip& clip) {
    return {clip.source_in, clip.duration, clip.timeline_in};
}

RationalTime PhaseOf(const DocumentClip& clip) {
    return clip.timeline_in.sub(clip.source_in);
}

std::vector<ExactTimelinePosition> PositionsAfter(const DocumentTrack& track,
                                                  size_t first) {
    std::vector<ExactTimelinePosition> positions;
    for (size_t index = first; index < track.clips.size(); ++index) {
        positions.push_back(
            {track.clips[index].id, track.clips[index].timeline_in});
    }
    return positions;
}

void AppendPositionsAtOrAfter(const DocumentTrack& track,
                              const RationalTime& boundary,
                              std::vector<ExactTimelinePosition>& positions) {
    for (const DocumentClip& clip : track.clips) {
        if (clip.timeline_in >= boundary)
            positions.push_back({clip.id, clip.timeline_in});
    }
}

void ShiftPositionsAtOrAfter(DocumentTrack& track, const RationalTime& boundary,
                             const RationalTime& delta) {
    for (DocumentClip& clip : track.clips) {
        if (clip.timeline_in >= boundary)
            clip.timeline_in = clip.timeline_in.add(delta);
    }
}

std::vector<ExactTimelinePosition> CurrentPositionsFor(
    const Document& document,
    const std::vector<ExactTimelinePosition>& reference) {
    std::vector<ExactTimelinePosition> positions;
    positions.reserve(reference.size());
    for (const ExactTimelinePosition& item : reference) {
        const DocumentClip* clip = document.FindClip(item.clip_id);
        if (clip) positions.push_back({clip->id, clip->timeline_in});
    }
    return positions;
}

bool Negate(const RationalTime& value, RationalTime& output);

bool ApplyExactPositions(Document& document,
                         const std::vector<ExactTimelinePosition>& positions,
                         EditError& error, std::string& message) {
    for (const ExactTimelinePosition& position : positions) {
        DocumentClip* clip = document.FindClip(position.clip_id);
        if (!clip) {
            Fail(EditError::UnknownClip,
                 "exact ripple state references unknown clip_id '" +
                     position.clip_id + "'",
                 error, message);
            return false;
        }
        if (position.timeline_in.rate <= 0 || position.timeline_in.value < 0) {
            Fail(EditError::InvalidTimelineIn,
                 "exact ripple state has invalid timeline_in", error, message);
            return false;
        }
        clip->timeline_in = position.timeline_in;
    }
    return true;
}

bool ValidateResult(const Document& candidate, EditError& error,
                    std::string& message) {
    std::string validation;
    if (candidate.Validate(validation)) return true;
    const EditError code = validation.find("overlap") != std::string::npos
                               ? EditError::Overlap
                               : EditError::ValidationFailed;
    Fail(code, validation, error, message);
    return false;
}

std::vector<Ulid> SplitAffectedTrackIds(const Document& document,
                                        const DocumentClip& clip);

const DocumentTrack* LockedTrackTouchedBy(const Document& document,
                                          const Operation& operation) {
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto addClipTrack = [&](const Ulid& clipId) {
        if (const DocumentTrack* track = document.FindTrackForClip(clipId))
            addTrack(track->id);
    };
    const auto addExactTracks =
        [&](const std::vector<ExactTrackState>& states) {
            for (const ExactTrackState& state : states)
                addTrack(state.track_id);
        };
    const auto addExactPositions =
        [&](const std::vector<ExactTimelinePosition>& positions) {
            for (const ExactTimelinePosition& position : positions)
                addClipTrack(position.clip_id);
        };
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InsertClipOperation> ||
                          std::is_same_v<T, InsertMotionClipOperation>) {
                addTrack(value.track_id);
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactPositions(value.exact_timeline_result);
            } else if constexpr (std::is_same_v<T, RemoveClipOperation> ||
                                 std::is_same_v<T, TrimClipOperation> ||
                                 std::is_same_v<T, SplitClipOperation>) {
                addClipTrack(value.clip_id);
                if constexpr (std::is_same_v<T, RemoveClipOperation>) {
                    for (const Ulid& trackId : value.sync_track_ids)
                        addTrack(trackId);
                    addExactPositions(value.exact_timeline_result);
                }
            } else if constexpr (std::is_same_v<T, ClearClipOperation>) {
                addClipTrack(value.clip_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, ClearClipsOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, PasteClipsOperation>) {
                for (const PastedClip& clip : value.clips)
                    addTrack(clip.track_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, MoveClipOperation>) {
                addClipTrack(value.clip_id);
                addTrack(value.track_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, MoveClipsOperation>) {
                for (const LinkedClipMove& move : value.moves) {
                    addClipTrack(move.clip_id);
                    addTrack(move.track_id);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, MoveLinkedClipsOperation>) {
                for (const LinkedClipMove& move : value.moves) {
                    addClipTrack(move.clip_id);
                    addTrack(move.track_id);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, TrimLinkedClipsOperation>) {
                for (const LinkedClipTrim& trim : value.trims)
                    addClipTrack(trim.clip_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RippleTrimOperation>) {
                addClipTrack(value.clip_id);
                for (const Ulid& clipId : value.linked_clip_ids)
                    addClipTrack(clipId);
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, TrimBoundaryAirOperation>) {
                for (const BoundaryAirTrim& trim : value.trims) {
                    addClipTrack(trim.clip_id);
                    for (const Ulid& clipId : trim.linked_clip_ids)
                        addClipTrack(clipId);
                }
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RollEditOperation>) {
                for (const RollEditPair& pair : value.pairs) {
                    addClipTrack(pair.left_clip_id);
                    addClipTrack(pair.right_clip_id);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, SlipEditOperation> ||
                                 std::is_same_v<T, SlideEditOperation>) {
                // FC3 -- a slide's neighbours are on its own track.
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RateStretchOperation>) {
                // FC3 -- the whole link group is stretched, so a lock on
                // any member's track stops it.
                addClipTrack(value.clip_id);
                if (const DocumentClip* anchor =
                        document.FindClip(value.clip_id);
                    anchor != nullptr && !anchor->link_group_id.empty()) {
                    for (const DocumentTrack& track : document.sequence.tracks)
                        for (const DocumentClip& member : track.clips)
                            if (member.link_group_id == anchor->link_group_id)
                                addTrack(track.id);
                }
                // DEC40 -- and a ripple moves its sync tracks' clips.
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T,
                                                RemoveLinkedClipsOperation> ||
                                 std::is_same_v<T, ClearLinkedClipsOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                if constexpr (std::is_same_v<T, RemoveLinkedClipsOperation>) {
                    for (const Ulid& trackId : value.sync_track_ids)
                        addTrack(trackId);
                }
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, DeleteGapOperation>) {
                addTrack(value.track_id);
                for (const Ulid& trackId : value.linked_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, DetachAudioOperation>) {
                addClipTrack(value.video_clip_id);
                addTrack(value.audio_track_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RemoveTrackOperation>) {
                addTrack(value.track_id);
            } else if constexpr (std::is_same_v<T, SetClipLinkOperation>) {
                addClipTrack(value.first_clip_id);
                addClipTrack(value.second_clip_id);
            } else if constexpr (std::is_same_v<T, AddTransitionOperation>) {
                addTrack(value.transition.track_id);
            } else if constexpr (std::is_same_v<T, RemoveTransitionOperation> ||
                                 std::is_same_v<T, UpdateTransitionOperation>) {
                if (const DocumentTransition* transition =
                        document.FindTransition(value.transition_id))
                    addTrack(transition->track_id);
            } else if constexpr (std::is_same_v<T, SplitLinkedClipsOperation>) {
                for (const Ulid& clipId : value.clip_ids) addClipTrack(clipId);
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, JoinClipOperation>) {
                addClipTrack(value.left_clip_id);
                addClipTrack(value.right_clip_id);
            } else if constexpr (std::is_same_v<T, SetClipEffectsOperation> ||
                                 std::is_same_v<T, SetClipOpacityOperation> ||
                                 std::is_same_v<T, SetClipBlurOperation> ||
                                 std::is_same_v<T, SetClipMaskOperation> ||
                                 std::is_same_v<T, SetClipSpeedOperation> ||
                                 std::is_same_v<T, SetClipAudioOperation> ||
                                 std::is_same_v<T, SetClipCaptionOperation> ||
                                 std::is_same_v<T,
                                                SetClipMotionPropsOperation>) {
                addClipTrack(value.clip_id);
            } else if constexpr (std::is_same_v<T, SetClipFramingOperation>) {
                // R1 -- one operation touches several clips, so a lock on
                // any of their tracks stops the whole of it.
                for (const ClipFramingAssignment& assignment : value.clips)
                    addClipTrack(assignment.clip_id);
            } else if constexpr (std::is_same_v<T, SetClipAnimationOperation>) {
                // R2 -- the same, for the keys of several clips at once.
                for (const ClipAnimationAssignment& assignment : value.clips)
                    addClipTrack(assignment.clip_id);
            } else if constexpr (std::is_same_v<T, SetClipEnabledOperation>) {
                // Out of lot -- and for a selection disabled at once: a
                // locked track keeps its clips as they play, as in Resolve.
                for (const ClipEnabledAssignment& assignment : value.clips)
                    addClipTrack(assignment.clip_id);
            } else if constexpr (std::is_same_v<
                                     T, SplitClipAtPositionsOperation>) {
                // FC8b -- the cut runs through the whole link group, so the
                // lock check reads the same tracks the cut snapshots: a lock
                // on the linked sound's track stops it instead of letting a
                // cut through that its own undo would then refuse.
                if (const DocumentClip* clip = document.FindClip(value.clip_id))
                    for (const Ulid& trackId :
                         SplitAffectedTrackIds(document, *clip))
                        addTrack(trackId);
                addClipTrack(value.clip_id);
                addExactTracks(value.exact_track_result);
            } else if constexpr (std::is_same_v<T, RemoveWordsOperation>) {
                addClipTrack(value.clip_id);
                // SYNCHRO-2026-10 SY3 -- a recorder's sound on a locked
                // track refuses the cut rather than let it drift.
                for (const LinkedWordRemoval& link : value.translated_links) {
                    addClipTrack(link.clip_id);
                }
                for (const Ulid& trackId : value.sync_track_ids)
                    addTrack(trackId);
                addExactTracks(value.exact_track_result);
            }
        },
        operation);
    for (const Ulid& trackId : trackIds) {
        const DocumentTrack* track = document.FindTrack(trackId);
        if (track && track->locked) return track;
    }
    return nullptr;
}

bool ValidateSourceRange(const DocumentSource& source,
                         const RationalTime& sourceIn,
                         const RationalTime& duration, EditError& error,
                         std::string& message) {
    if (sourceIn.rate <= 0 || duration.rate <= 0) {
        Fail(EditError::ArithmeticError, "time rate must be positive", error,
             message);
        return false;
    }
    if (duration.value <= 0) {
        Fail(EditError::InvalidDuration, "duration must be positive", error,
             message);
        return false;
    }
    if (sourceIn.value < 0 || sourceIn.add(duration) > source.duration) {
        Fail(EditError::SourceOutOfBounds,
             "source range is outside source_id '" + source.id + "'", error,
             message);
        return false;
    }
    return true;
}

// The part of an insertion that does not depend on what the clip shows:
// its identity, the ripple of its own track and of `syncTrackIds`, the
// exact positions redo replays, and the RemoveClip inverse. Shared by media
// clips and MOTION-2026-09 M4's motion clips, so both move the same way.
bool InsertAndRipple(Document& candidate, DocumentTrack& track,
                     DocumentClip inserted,
                     const std::vector<Ulid>& syncTrackIds, Ulid& clipId,
                     std::vector<ExactTimelinePosition>& exactTimelineResult,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const Ulid trackId = track.id;
    const RationalTime timelineIn = inserted.timeline_in;
    const RationalTime duration = inserted.duration;
    if (clipId.empty()) clipId = GenerateUlid();
    if (!IsValidUlid(clipId) || candidate.FindClip(clipId) ||
        candidate.FindSource(clipId) || candidate.FindTrack(clipId)) {
        Fail(EditError::DuplicateId,
             "insert clip_id is invalid or already exists: '" + clipId + "'",
             error, message);
        return false;
    }
    inserted.id = clipId;

    auto insertion = std::lower_bound(
        track.clips.begin(), track.clips.end(), timelineIn,
        [](const DocumentClip& clip, const RationalTime& position) {
            return clip.timeline_in < position;
        });
    const size_t insertionIndex =
        static_cast<size_t>(std::distance(track.clips.begin(), insertion));
    if (insertion != track.clips.begin()) {
        const DocumentClip& previous = *std::prev(insertion);
        if (timelineIn < previous.timeline_in.add(previous.duration)) {
            Fail(EditError::Overlap,
                 "insertion timeline_in overlaps clip_id '" + previous.id + "'",
                 error, message);
            return false;
        }
    }

    // QC-2026-09 C1 -- an insert on one track never touches another by
    // default (a cutaway densified on its own track must leave V1/A1 alone),
    // but a track named explicitly here ripples in lockstep so an A/V-linked
    // partner living on a different track doesn't silently fall out of
    // sync_reference_delta. Mirrors ApplyRemove's sync_track_ids handling.
    std::vector<Ulid> affectedTrackIds{trackId};
    const auto addAffectedTrack = [&](const Ulid& id) {
        if (std::find(affectedTrackIds.begin(), affectedTrackIds.end(), id) ==
            affectedTrackIds.end())
            affectedTrackIds.push_back(id);
    };
    for (const Ulid& syncTrackId : syncTrackIds) {
        if (!candidate.FindTrack(syncTrackId)) {
            Fail(EditError::UnknownTrack,
                 "unknown insert sync track_id '" + syncTrackId + "'", error,
                 message);
            return false;
        }
        addAffectedTrack(syncTrackId);
    }

    std::vector<ExactTimelinePosition> before =
        PositionsAfter(track, insertionIndex);
    std::vector<Ulid> shiftedTrackIds{trackId};
    for (const Ulid& syncTrackId : syncTrackIds) {
        if (std::find(shiftedTrackIds.begin(), shiftedTrackIds.end(),
                      syncTrackId) != shiftedTrackIds.end())
            continue;
        shiftedTrackIds.push_back(syncTrackId);
        AppendPositionsAtOrAfter(*candidate.FindTrack(syncTrackId), timelineIn,
                                 before);
    }
    for (size_t index = insertionIndex; index < track.clips.size(); ++index) {
        track.clips[index].timeline_in =
            track.clips[index].timeline_in.add(duration);
    }
    for (size_t index = 1; index < shiftedTrackIds.size(); ++index) {
        ShiftPositionsAtOrAfter(*candidate.FindTrack(shiftedTrackIds[index]),
                                timelineIn, duration);
    }
    inserted.include_audio = false;
    track.clips.insert(
        track.clips.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
        std::move(inserted));
    if (!exactTimelineResult.empty() &&
        !ApplyExactPositions(candidate, exactTimelineResult, error, message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;

    if (exactTimelineResult.empty()) {
        exactTimelineResult = CurrentPositionsFor(candidate, before);
    }
    inverse = RemoveClipOperation{clipId, syncTrackIds, before};
    return true;
}

bool ApplyInsert(Document& candidate, InsertClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    const DocumentSource* source = candidate.FindSource(operation.source_id);
    if (!source) {
        Fail(EditError::UnknownSource,
             "unknown source_id '" + operation.source_id + "'", error, message);
        return false;
    }
    const LibraryMedia* media = candidate.FindLibraryMedia(operation.source_id);
    if (track->kind == "video" && media && media->metadata_complete &&
        !media->has_video) {
        Fail(EditError::InvalidOperation,
             "source_id '" + operation.source_id +
                 "' is audio-only and cannot be inserted on a video track",
             error, message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "timeline_in must be non-negative with a positive rate", error,
             message);
        return false;
    }
    // R4 -- a clip laid at a speed reads `duration * speed` of its source;
    // a freeze reads one frame, which has to be there.
    std::string speedError;
    if (!ClipSpeedIsValid(operation.speed, speedError)) {
        Fail(EditError::InvalidOperation, speedError, error, message);
        return false;
    }
    if (IsFreezeSpeed(operation.speed)) {
        if (track->kind != "video") {
            Fail(EditError::InvalidOperation,
                 "a freeze (speed 0/1) holds a picture: it goes on a video "
                 "track",
                 error, message);
            return false;
        }
        if (operation.duration.rate <= 0 || operation.duration.value <= 0) {
            Fail(EditError::InvalidDuration, "duration must be positive", error,
                 message);
            return false;
        }
        if (operation.source_in.rate <= 0 || operation.source_in.value < 0 ||
            !(operation.source_in < source->duration)) {
            Fail(EditError::SourceOutOfBounds,
                 "the frame to hold is outside source_id '" + source->id + "'",
                 error, message);
            return false;
        }
    } else if (!ValidateSourceRange(
                   *source, operation.source_in,
                   ScaleBySpeed(operation.duration, operation.speed), error,
                   message)) {
        return false;
    }
    DocumentClip inserted{operation.clip_id, operation.source_id,
                          operation.source_in, operation.duration,
                          operation.timeline_in};
    inserted.speed = operation.speed;
    return InsertAndRipple(candidate, *track, std::move(inserted),
                           operation.sync_track_ids, operation.clip_id,
                           operation.exact_timeline_result, inverse, error,
                           message);
}

// MOTION-2026-09 M4 -- the composition a motion operation names, parsed.
// Its refusals are M3's: UnknownComposition, and InvalidMotion for a stored
// definition that does not parse.
bool ParsedComposition(const Document& candidate, const Ulid& compositionId,
                       MotionComposition& composition, EditError& error,
                       std::string& message) {
    const DocumentComposition* stored =
        candidate.FindComposition(compositionId);
    if (stored == nullptr) {
        Fail(EditError::UnknownComposition,
             "unknown composition_id '" + compositionId + "'", error, message);
        return false;
    }
    MotionError motion;
    if (!ParseDocumentComposition(*stored, composition, motion)) {
        MotionRefusal(motion, error, message);
        return false;
    }
    return true;
}

bool CheckInstanceProps(const MotionComposition& composition,
                        const mcp_json::Value& props, EditError& error,
                        std::string& message) {
    MotionError motion;
    if (ValidateMotionInstanceProps(composition, props, motion)) {
        return true;
    }
    MotionRefusal(motion, error, message);
    if (error == EditError::InvalidMotion) {
        error = EditError::InvalidOperation;
    }
    return false;
}

bool ApplyInsertMotionClip(Document& candidate,
                           InsertMotionClipOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (track == nullptr) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    if (track->kind != "video") {
        Fail(EditError::InvalidOperation,
             "a motion clip goes on a video track; track_id '" +
                 operation.track_id + "' is " + track->kind,
             error, message);
        return false;
    }
    MotionComposition composition;
    if (!ParsedComposition(candidate, operation.composition_id, composition,
                           error, message)) {
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "timeline_in must be non-negative with a positive rate", error,
             message);
        return false;
    }
    if (!CheckInstanceProps(composition, operation.motion_props, error,
                            message) ||
        !ValidateSourceRange(
            CompositionAsSource(operation.composition_id, composition),
            operation.source_in, operation.duration, error, message)) {
        return false;
    }
    DocumentClip inserted{operation.clip_id, Ulid(), operation.source_in,
                          operation.duration, operation.timeline_in};
    inserted.composition_id = operation.composition_id;
    inserted.motion_props = operation.motion_props;
    return InsertAndRipple(candidate, *track, std::move(inserted),
                           operation.sync_track_ids, operation.clip_id,
                           operation.exact_timeline_result, inverse, error,
                           message);
}

bool ApplySetClipMotionProps(Document& candidate,
                             SetClipMotionPropsOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (clip == nullptr) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (!IsMotionClip(*clip)) {
        Fail(EditError::InvalidOperation,
             "clip_id '" + operation.clip_id +
                 "' shows a media, not a composition: it has no props",
             error, message);
        return false;
    }
    MotionComposition composition;
    if (!ParsedComposition(candidate, clip->composition_id, composition, error,
                           message) ||
        !CheckInstanceProps(composition, operation.props, error, message)) {
        return false;
    }
    const mcp_json::Value before = clip->motion_props;
    clip->motion_props = operation.props;
    if (!ValidateResult(candidate, error, message)) {
        return false;
    }
    inverse = SetClipMotionPropsOperation{clip->id, before};
    return true;
}

bool ApplyRemove(Document& candidate, RemoveClipOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip removed = *found;
    const Ulid trackId = track->id;
    const RationalTime followingBoundary =
        removed.timeline_in.add(removed.duration);
    std::vector<Ulid> affectedTrackIds{trackId};
    const auto addAffectedTrack = [&](const Ulid& id) {
        if (std::find(affectedTrackIds.begin(), affectedTrackIds.end(), id) ==
            affectedTrackIds.end())
            affectedTrackIds.push_back(id);
    };
    for (const Ulid& syncTrackId : operation.sync_track_ids) {
        if (!candidate.FindTrack(syncTrackId)) {
            Fail(EditError::UnknownTrack,
                 "unknown remove sync track_id '" + syncTrackId + "'", error,
                 message);
            return false;
        }
        addAffectedTrack(syncTrackId);
    }
    std::vector<ExactTrackState> beforeTracks;
    for (const Ulid& affectedTrackId : affectedTrackIds) {
        const DocumentTrack* affectedTrack =
            candidate.FindTrack(affectedTrackId);
        beforeTracks.push_back({affectedTrackId, affectedTrack->clips});
    }
    std::vector<ExactTimelinePosition> before =
        PositionsAfter(*track, index + 1);
    std::vector<Ulid> shiftedTrackIds{trackId};
    for (const Ulid& syncTrackId : operation.sync_track_ids) {
        DocumentTrack* syncTrack = candidate.FindTrack(syncTrackId);
        if (std::find(shiftedTrackIds.begin(), shiftedTrackIds.end(),
                      syncTrackId) != shiftedTrackIds.end())
            continue;
        shiftedTrackIds.push_back(syncTrackId);
        AppendPositionsAtOrAfter(*syncTrack, followingBoundary, before);
    }
    for (size_t next = index + 1; next < track->clips.size(); ++next) {
        track->clips[next].timeline_in =
            track->clips[next].timeline_in.sub(removed.duration);
    }
    track->clips.erase(track->clips.begin() +
                       static_cast<std::ptrdiff_t>(index));
    RationalTime rippleDelta;
    if (!Negate(removed.duration, rippleDelta)) {
        Fail(EditError::ArithmeticError,
             "removed clip duration cannot be negated", error, message);
        return false;
    }
    for (size_t index = 1; index < shiftedTrackIds.size(); ++index) {
        ShiftPositionsAtOrAfter(*candidate.FindTrack(shiftedTrackIds[index]),
                                followingBoundary, rippleDelta);
    }
    if (!operation.exact_timeline_result.empty() &&
        !ApplyExactPositions(candidate, operation.exact_timeline_result, error,
                             message)) {
        return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    if (operation.exact_timeline_result.empty()) {
        operation.exact_timeline_result =
            CurrentPositionsFor(candidate, before);
    }
    // A remove must restore the complete clip (including legacy, linked and
    // audio metadata) on undo.
    //
    // PL1 -- ROADMAP.md. A remove carrying exact positions used to get an
    // InsertClip inverse, on the grounds that it was the inverse of an
    // insert and its clip bare. That holds while it only ever runs as an
    // undo, whose own inverse the history drops. A batch applies its
    // members, exact removes included, as fresh operations and keeps their
    // inverses: a clip whose gain or effects were set before the removal
    // then came back without them (found by I9 on batches). The complete
    // tracks are restored instead, whichever way the remove arrives.
    inverse = ClearClipsOperation{{operation.clip_id}, std::move(beforeTracks)};
    return true;
}

bool ApplyClear(Document& candidate, ClearClipOperation& operation,
                Operation& inverse, EditError& error, std::string& message) {
    if (!operation.exact_track_result.empty()) {
        if (operation.exact_track_result.size() != 1) {
            Fail(EditError::InvalidOperation,
                 "exact clear state must contain the clip track", error,
                 message);
            return false;
        }
        DocumentTrack* track =
            candidate.FindTrack(operation.exact_track_result.front().track_id);
        if (!track) {
            Fail(EditError::UnknownTrack,
                 "exact clear references an unknown track", error, message);
            return false;
        }
        const ExactTrackState before{track->id, track->clips};
        track->clips = operation.exact_track_result.front().clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = ClearClipOperation{operation.clip_id, {before}};
        return true;
    }
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const ExactTrackState before{track->id, track->clips};
    track->clips.erase(std::remove_if(track->clips.begin(), track->clips.end(),
                                      [&](const DocumentClip& clip) {
                                          return clip.id == operation.clip_id;
                                      }),
                       track->clips.end());
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = {{track->id, track->clips}};
    inverse = ClearClipOperation{operation.clip_id, {before}};
    return true;
}

bool ApplyClearClips(Document& candidate, ClearClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> result;
        for (const Ulid& trackId : trackIds) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == trackId;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) result.push_back({trackId, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> trackIds;
        for (const ExactTrackState& state : operation.exact_track_result)
            trackIds.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact multi-clear references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = ClearClipsOperation{operation.clip_ids, before};
        return true;
    }
    if (operation.clip_ids.empty()) {
        Fail(EditError::InvalidOperation,
             "ClearClips requires at least one clip", error, message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!track) {
            Fail(EditError::UnknownClip,
                 "unknown multi-clear clip_id '" + id + "'", error, message);
            return false;
        }
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "ClearClips members must be unique", error, message);
            return false;
        }
        seen.push_back(id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const Ulid& id : operation.clip_ids) {
        DocumentTrack* track = candidate.FindTrackForClip(id);
        track->clips.erase(
            std::remove_if(
                track->clips.begin(), track->clips.end(),
                [&](const DocumentClip& clip) { return clip.id == id; }),
            track->clips.end());
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = ClearClipsOperation{operation.clip_ids, before};
    return true;
}

bool ApplyPasteClips(Document& candidate, PasteClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : trackIds) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> trackIds;
        for (const ExactTrackState& state : operation.exact_track_result)
            trackIds.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact paste state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = PasteClipsOperation{{}, before, operation.overwrite};
        return true;
    }
    if (operation.clips.empty()) {
        Fail(EditError::InvalidOperation, "paste selection is empty", error,
             message);
        return false;
    }

    std::vector<Ulid> trackIds;
    std::vector<Ulid> copiedIds;
    std::vector<Ulid> pastedIds;
    std::map<Ulid, size_t> linkCounts;
    std::map<Ulid, Ulid> pastedByCopiedId;
    std::map<Ulid, Ulid> pastedLinkGroups;
    for (PastedClip& item : operation.clips) {
        DocumentTrack* track = candidate.FindTrack(item.track_id);
        if (!track) {
            Fail(EditError::UnknownTrack,
                 "paste references unknown track_id '" + item.track_id + "'",
                 error, message);
            return false;
        }
        const DocumentSource* source = candidate.FindSource(item.source_id);
        if (!source) {
            // MOTION-2026-09 M4 -- a pasted clip carries a media range only;
            // a motion clip is posed again with insert_motion_clip.
            Fail(EditError::UnknownSource,
                 item.source_id.empty()
                     ? "paste carries no source_id: a motion clip is not "
                       "pasted, insert it again with InsertMotionClip"
                     : "paste references unknown source_id '" + item.source_id +
                           "'",
                 error, message);
            return false;
        }
        if (!IsValidUlid(item.copied_clip_id) ||
            std::find(copiedIds.begin(), copiedIds.end(),
                      item.copied_clip_id) != copiedIds.end()) {
            Fail(EditError::DuplicateId,
                 "pasted copied_clip_id is invalid or duplicated", error,
                 message);
            return false;
        }
        if (item.timeline_in.rate <= 0 || item.timeline_in.value < 0) {
            Fail(EditError::InvalidTimelineIn,
                 "pasted timeline_in must be non-negative", error, message);
            return false;
        }
        if (!ValidateSourceRange(*source, item.source_in, item.duration, error,
                                 message))
            return false;
        if (item.clip_id.empty()) item.clip_id = GenerateUlid();
        if (!IsValidUlid(item.clip_id) || candidate.FindClip(item.clip_id) ||
            candidate.FindTrack(item.clip_id) ||
            candidate.FindSource(item.clip_id) ||
            std::find(pastedIds.begin(), pastedIds.end(), item.clip_id) !=
                pastedIds.end()) {
            Fail(EditError::DuplicateId,
                 "pasted clip_id is invalid or already exists", error, message);
            return false;
        }
        copiedIds.push_back(item.copied_clip_id);
        pastedIds.push_back(item.clip_id);
        pastedByCopiedId[item.copied_clip_id] = item.clip_id;
        trackIds.push_back(item.track_id);
        if (!item.copied_link_group_id.empty())
            ++linkCounts[item.copied_link_group_id];
    }

    for (PastedClip& item : operation.clips) {
        const bool linked = !item.copied_link_group_id.empty() &&
                            linkCounts[item.copied_link_group_id] > 1;
        if (!linked) {
            item.link_group_id.clear();
            item.sync_anchor_clip_id.clear();
            item.sync_reference_delta = {0, 1};
            continue;
        }
        Ulid& group = pastedLinkGroups[item.copied_link_group_id];
        if (group.empty()) {
            group = item.link_group_id.empty() ? GenerateUlid()
                                               : item.link_group_id;
        } else if (!item.link_group_id.empty() && item.link_group_id != group) {
            Fail(EditError::InvalidOperation,
                 "pasted linked clips disagree on their new link group", error,
                 message);
            return false;
        }
        item.link_group_id = group;
        const auto anchor =
            pastedByCopiedId.find(item.copied_sync_anchor_clip_id);
        if (anchor == pastedByCopiedId.end()) {
            Fail(EditError::InvalidOperation,
                 "pasted linked selection is missing its sync anchor", error,
                 message);
            return false;
        }
        item.sync_anchor_clip_id = anchor->second;
    }

    for (size_t index = 0; index < operation.clips.size(); ++index) {
        const PastedClip& item = operation.clips[index];
        const RationalTime end = item.timeline_in.add(item.duration);
        const DocumentTrack* track = candidate.FindTrack(item.track_id);
        if (!operation.overwrite)
            for (const DocumentClip& existing : track->clips) {
                if (existing.timeline_in < end &&
                    existing.timeline_in.add(existing.duration) >
                        item.timeline_in) {
                    Fail(EditError::Overlap,
                         "paste overlaps clip_id '" + existing.id + "'", error,
                         message);
                    return false;
                }
            }
        for (size_t other = 0; other < index; ++other) {
            const PastedClip& previous = operation.clips[other];
            if (previous.track_id != item.track_id) continue;
            if (previous.timeline_in < end &&
                previous.timeline_in.add(previous.duration) >
                    item.timeline_in) {
                Fail(EditError::Overlap, "pasted clips overlap each other",
                     error, message);
                return false;
            }
        }
    }

    const std::vector<ExactTrackState> before = snapshots(trackIds);
    if (operation.overwrite) {
        for (const PastedClip& item : operation.clips) {
            DocumentTrack* track = candidate.FindTrack(item.track_id);
            const RationalTime pasteEnd = item.timeline_in.add(item.duration);
            std::vector<DocumentClip> survivors;
            survivors.reserve(track->clips.size() + 1);
            for (const DocumentClip& existing : track->clips) {
                const RationalTime existingEnd =
                    existing.timeline_in.add(existing.duration);
                if (existingEnd <= item.timeline_in ||
                    existing.timeline_in >= pasteEnd) {
                    survivors.push_back(existing);
                    continue;
                }
                const bool keepLeft = existing.timeline_in < item.timeline_in;
                const bool keepRight = existingEnd > pasteEnd;
                if (keepLeft) {
                    DocumentClip left = existing;
                    left.duration = item.timeline_in.sub(existing.timeline_in);
                    survivors.push_back(std::move(left));
                }
                if (keepRight) {
                    DocumentClip right = existing;
                    if (keepLeft) RenewClipIdentity(right);
                    const RationalTime sourceOffset =
                        pasteEnd.sub(existing.timeline_in);
                    right.source_in = ClipSourceTimeAt(existing, sourceOffset);
                    right.duration = existingEnd.sub(pasteEnd);
                    right.timeline_in = pasteEnd;
                    // R2 -- the surviving tail's head moved forward by that
                    // much of its own local time, so its keys move back by
                    // it and stay on the frames they were put on.
                    ShiftClipAnimationHead(right, sourceOffset);
                    survivors.push_back(std::move(right));
                }
            }
            track->clips = std::move(survivors);
        }
    }
    for (const PastedClip& item : operation.clips) {
        DocumentClip clip{item.clip_id, item.source_id, item.source_in,
                          item.duration, item.timeline_in};
        clip.include_audio = false;
        clip.link_group_id = item.link_group_id;
        clip.sync_anchor_clip_id = item.sync_anchor_clip_id;
        clip.sync_reference_delta = item.sync_reference_delta;
        candidate.FindTrack(item.track_id)->clips.push_back(std::move(clip));
    }
    for (const Ulid& id : trackIds) {
        DocumentTrack* track = candidate.FindTrack(id);
        std::stable_sort(
            track->clips.begin(), track->clips.end(),
            [](const DocumentClip& left, const DocumentClip& right) {
                return left.timeline_in < right.timeline_in;
            });
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = PasteClipsOperation{{}, before, operation.overwrite};
    return true;
}

bool Negate(const RationalTime& value, RationalTime& output) {
    if (value.value == std::numeric_limits<int64_t>::min()) return false;
    output = {-value.value, value.rate};
    return true;
}

// B3 -- ROADMAP.md. A rejected edit is useful only when it says which exact
// interval was available and which one was requested. Keeping this check
// before any candidate mutation also makes a failed edit indistinguishable
// from no edit to the event log.
std::string DescribeTime(const RationalTime& value) {
    return std::to_string(value.value) + "/" + std::to_string(value.rate);
}

bool ValidateEditedSourceRange(const std::string& operationName,
                               const DocumentClip& clip,
                               const DocumentSource& source,
                               const ExactClipTimes& requested,
                               EditError& error, std::string& message,
                               const RationalTime& headAdvance = {0, 1}) {
    // R3 -- docs/roadmap/REEL-2026-09.md. What an edit reads of its source
    // is the requested duration times the clip's speed (ClipSpeed.h), so
    // the bound every trim, ripple and roll checks is that one. Criterion 4
    // -- on a ramp it is the integral over the frames the edited clip
    // keeps, which start `headAdvance` into the clip as it is now (its keys
    // follow, ShiftClipAnimationHead).
    const RationalTime requestedEnd =
        ClipHasSpeedRamp(clip)
            ? requested.source_in.add(ClipSourceAdvanceBetween(
                  clip, headAdvance, headAdvance.add(requested.duration)))
            : requested.source_in.add(
                  ScaleBySpeed(requested.duration, clip.speed));
    if (requested.duration.value <= 0 || requested.source_in.value < 0 ||
        requestedEnd > source.duration) {
        Fail(EditError::InvalidOperation,
             operationName + " source range for clip_id '" + clip.id +
                 "' must stay within [0/" +
                 std::to_string(source.duration.rate) + ", " +
                 DescribeTime(source.duration) + "); requested [" +
                 DescribeTime(requested.source_in) + ", " +
                 DescribeTime(requestedEnd) + ")",
             error, message);
        return false;
    }
    return true;
}

bool ValidateEditedDuration(const std::string& operationName,
                            const DocumentClip& clip,
                            const ExactClipTimes& requested, EditError& error,
                            std::string& message) {
    if (requested.duration.value <= 0) {
        Fail(EditError::InvalidOperation,
             operationName + " duration for clip_id '" + clip.id +
                 "' must be positive; requested " +
                 DescribeTime(requested.duration),
             error, message);
        return false;
    }
    if (requested.timeline_in.value < 0) {
        Fail(EditError::InvalidOperation,
             operationName + " timeline_in for clip_id '" + clip.id +
                 "' must stay within [0/" +
                 std::to_string(requested.timeline_in.rate) + ", inf); got " +
                 DescribeTime(requested.timeline_in),
             error, message);
        return false;
    }
    return true;
}

bool ApplyTrim(Document& candidate, TrimClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (operation.delta.rate <= 0) {
        Fail(EditError::ArithmeticError, "trim delta rate must be positive",
             error, message);
        return false;
    }
    // MOTION-2026-09 M4 -- a motion clip's range lives in its composition.
    const std::optional<DocumentSource> source = ClipSource(candidate, *clip);
    const DocumentTrack* track = candidate.FindTrackForClip(clip->id);
    const bool captionClip = track && track->kind == "caption";
    if (!source && !captionClip) {
        Fail(EditError::UnknownSource,
             "clip references unknown source_id '" + clip->source_id + "'",
             error, message);
        return false;
    }
    const ExactClipTimes before = TimesOf(*clip);
    ExactClipTimes requested = before;
    if (operation.edge == TrimEdge::Head) {
        if (!captionClip)
            requested.source_in = requested.source_in.add(
                ClipSourceAdvance(*clip, operation.delta));
        requested.duration = requested.duration.sub(operation.delta);
        requested.timeline_in = requested.timeline_in.add(operation.delta);
    } else {
        requested.duration = requested.duration.add(operation.delta);
    }
    if (requested.duration.value <= 0) {
        Fail(EditError::InvalidDuration,
             "trim would make duration zero or negative", error, message);
        return false;
    }
    if (requested.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "trim would make timeline_in negative", error, message);
        return false;
    }
    if (source && !ValidateEditedSourceRange(
                      "trim_clip", *clip, *source, requested, error, message,
                      operation.edge == TrimEdge::Head ? operation.delta
                                                       : RationalTime{0, 1})) {
        return false;
    }
    clip->source_in = requested.source_in;
    clip->duration = requested.duration;
    clip->timeline_in = requested.timeline_in;
    if (operation.exact_clip_result) {
        clip->source_in = operation.exact_clip_result->source_in;
        clip->duration = operation.exact_clip_result->duration;
        clip->timeline_in = operation.exact_clip_result->timeline_in;
    }
    // R2 -- docs/roadmap/REEL-2026-09.md. A trim_clip only ever moves
    // timeline_in by moving the head, so the difference IS the head's
    // advance; a tail trim leaves it at zero and the keys alone.
    ShiftClipAnimationHead(*clip, clip->timeline_in.sub(before.timeline_in));
    if (!ValidateResult(candidate, error, message)) return false;
    if (!operation.exact_clip_result)
        operation.exact_clip_result = TimesOf(*clip);

    RationalTime inverseDelta;
    if (!Negate(operation.delta, inverseDelta)) {
        Fail(EditError::ArithmeticError, "trim delta cannot be negated", error,
             message);
        return false;
    }
    inverse = TrimClipOperation{operation.clip_id, operation.edge, inverseDelta,
                                before};
    return true;
}

bool ApplyMove(Document& candidate, MoveClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* sourceTrack = candidate.FindTrackForClip(operation.clip_id);
    if (!sourceTrack) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    DocumentTrack* targetTrack = candidate.FindTrack(operation.track_id);
    if (!targetTrack) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    if (sourceTrack->kind != targetTrack->kind) {
        Fail(EditError::InvalidOperation,
             "cannot move a clip between tracks of different kinds", error,
             message);
        return false;
    }
    if (operation.timeline_in.rate <= 0 || operation.timeline_in.value < 0) {
        Fail(EditError::InvalidTimelineIn,
             "move timeline_in must be non-negative with a positive rate",
             error, message);
        return false;
    }

    const auto snapshotTracks = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> snapshots;
        for (const Ulid& trackId : trackIds) {
            if (std::any_of(snapshots.begin(), snapshots.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == trackId;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) snapshots.push_back({track->id, track->clips});
        }
        return snapshots;
    };

    if (!operation.exact_track_result.empty()) {
        const Ulid currentTrackId = sourceTrack->id;
        const RationalTime currentTimelineIn =
            candidate.FindClip(operation.clip_id)->timeline_in;
        std::vector<Ulid> affected;
        for (const ExactTrackState& state : operation.exact_track_result)
            affected.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshotTracks(affected);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact move state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result) {
            candidate.FindTrack(state.track_id)->clips = state.clips;
        }
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveClipOperation{operation.clip_id, currentTrackId,
                                    currentTimelineIn, before};
        return true;
    }

    const Ulid sourceTrackId = sourceTrack->id;
    const std::vector<ExactTrackState> before =
        snapshotTracks({sourceTrackId, operation.track_id});
    const auto found = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const DocumentClip original = *found;
    sourceTrack->clips.erase(found);

    // Removing a clip cannot invalidate the tracks vector, but reacquiring the
    // destination avoids retaining a clip-vector-dependent pointer across the
    // erase and keeps same-track moves straightforward.
    targetTrack = candidate.FindTrack(operation.track_id);
    DocumentClip moved = original;
    moved.timeline_in = operation.timeline_in;
    const RationalTime movedEnd = moved.timeline_in.add(moved.duration);
    std::vector<DocumentClip> overwritten;
    overwritten.reserve(targetTrack->clips.size() + 2);
    for (const DocumentClip& existing : targetTrack->clips) {
        const RationalTime existingEnd =
            existing.timeline_in.add(existing.duration);
        if (existingEnd <= moved.timeline_in ||
            existing.timeline_in >= movedEnd) {
            overwritten.push_back(existing);
            continue;
        }

        const bool keepLeft = existing.timeline_in < moved.timeline_in;
        const bool keepRight = existingEnd > movedEnd;
        if (keepLeft) {
            DocumentClip left = existing;
            left.duration = moved.timeline_in.sub(existing.timeline_in);
            // An overwrite cut must not introduce a fade-out at the new
            // internal edge: only the surviving original tail owns it.
            left.audio_fade_out = {0, 1};
            overwritten.push_back(std::move(left));
        }
        if (keepRight) {
            DocumentClip right = existing;
            if (keepLeft) {
                do {
                    RenewClipIdentity(right);
                } while (candidate.FindClip(right.id));
            }
            const RationalTime sourceOffset =
                movedEnd.sub(existing.timeline_in);
            right.source_in = ClipSourceTimeAt(existing, sourceOffset);
            right.duration = existingEnd.sub(movedEnd);
            right.timeline_in = movedEnd;
            // R2 -- see the paste above: the head moved, so the keys do.
            ShiftClipAnimationHead(right, sourceOffset);
            // Symmetrically, the original head envelope belongs only to the
            // surviving original head, not to this post-overwrite piece.
            right.audio_fade_in = {0, 1};
            overwritten.push_back(std::move(right));
        }
    }
    overwritten.push_back(std::move(moved));
    std::stable_sort(overwritten.begin(), overwritten.end(),
                     [](const DocumentClip& left, const DocumentClip& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    targetTrack->clips = std::move(overwritten);
    if (!ValidateResult(candidate, error, message)) return false;

    operation.exact_track_result =
        snapshotTracks({sourceTrackId, operation.track_id});
    inverse = MoveClipOperation{operation.clip_id, sourceTrackId,
                                original.timeline_in, before};
    return true;
}

bool ApplyMoveClips(Document& candidate, MoveClipsOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact multi-move references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveClipsOperation{operation.moves, before};
        return true;
    }
    if (operation.moves.size() < 2) {
        Fail(EditError::InvalidOperation,
             "MoveClips requires at least two members", error, message);
        return false;
    }
    struct OriginalMove {
        DocumentClip clip;
        Ulid source_track_id;
        LinkedClipMove destination;
    };
    std::vector<OriginalMove> originals;
    std::vector<LinkedClipMove> inverseMoves;
    std::vector<Ulid> affectedTracks;
    std::vector<Ulid> seenClips;
    for (const LinkedClipMove& move : operation.moves) {
        const DocumentClip* clip = candidate.FindClip(move.clip_id);
        const DocumentTrack* source = candidate.FindTrackForClip(move.clip_id);
        const DocumentTrack* target = candidate.FindTrack(move.track_id);
        if (!clip || !source || !target) {
            Fail(!target ? EditError::UnknownTrack : EditError::UnknownClip,
                 "multi-move references an unknown clip or track", error,
                 message);
            return false;
        }
        if (source->kind != target->kind || move.timeline_in.rate <= 0 ||
            move.timeline_in.value < 0 ||
            std::find(seenClips.begin(), seenClips.end(), move.clip_id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "multi-move members must be unique and keep their track kind",
                 error, message);
            return false;
        }
        seenClips.push_back(move.clip_id);
        affectedTracks.push_back(source->id);
        affectedTracks.push_back(target->id);
        originals.push_back({*clip, source->id, move});
        inverseMoves.push_back({clip->id, source->id, clip->timeline_in});
    }
    for (size_t left = 0; left < originals.size(); ++left) {
        const RationalTime leftEnd =
            originals[left].destination.timeline_in.add(
                originals[left].clip.duration);
        for (size_t right = left + 1; right < originals.size(); ++right) {
            if (originals[left].destination.track_id !=
                originals[right].destination.track_id)
                continue;
            const RationalTime rightEnd =
                originals[right].destination.timeline_in.add(
                    originals[right].clip.duration);
            if (originals[left].destination.timeline_in < rightEnd &&
                originals[right].destination.timeline_in < leftEnd) {
                Fail(EditError::Overlap,
                     "multi-move destinations overlap each other", error,
                     message);
                return false;
            }
        }
    }
    const std::vector<ExactTrackState> before = snapshots(affectedTracks);
    for (const OriginalMove& original : originals) {
        DocumentTrack* source = candidate.FindTrack(original.source_track_id);
        source->clips.erase(
            std::remove_if(source->clips.begin(), source->clips.end(),
                           [&](const DocumentClip& clip) {
                               return clip.id == original.clip.id;
                           }),
            source->clips.end());
    }
    for (const OriginalMove& original : originals) {
        DocumentTrack* source = candidate.FindTrack(original.source_track_id);
        source->clips.push_back(original.clip);
        MoveClipOperation single{original.clip.id,
                                 original.destination.track_id,
                                 original.destination.timeline_in,
                                 {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyMove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(affectedTracks);
    inverse = MoveClipsOperation{std::move(inverseMoves), before};
    return true;
}

bool ApplyMoveLinked(Document& candidate, MoveLinkedClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked move references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = MoveLinkedClipsOperation{operation.link_group_id,
                                           operation.moves, before};
        return true;
    }
    if (operation.moves.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "MoveLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> affectedTracks;
    std::vector<LinkedClipMove> inverseMoves;
    std::vector<Ulid> seenClips;
    for (const LinkedClipMove& move : operation.moves) {
        const DocumentClip* clip = candidate.FindClip(move.clip_id);
        const DocumentTrack* source = candidate.FindTrackForClip(move.clip_id);
        const DocumentTrack* target = candidate.FindTrack(move.track_id);
        if (!clip || !source || !target) {
            Fail(!target ? EditError::UnknownTrack : EditError::UnknownClip,
                 "linked move references an unknown clip or track", error,
                 message);
            return false;
        }
        if (clip->link_group_id != operation.link_group_id ||
            std::find(seenClips.begin(), seenClips.end(), clip->id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "linked move members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seenClips.push_back(clip->id);
        affectedTracks.push_back(source->id);
        affectedTracks.push_back(target->id);
        inverseMoves.push_back({clip->id, source->id, clip->timeline_in});
    }
    const std::vector<ExactTrackState> before = snapshots(affectedTracks);
    for (const LinkedClipMove& move : operation.moves) {
        MoveClipOperation single{
            move.clip_id, move.track_id, move.timeline_in, {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyMove(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(affectedTracks);
    inverse = MoveLinkedClipsOperation{operation.link_group_id,
                                       std::move(inverseMoves), before};
    return true;
}

bool ApplyTrimLinked(Document& candidate, TrimLinkedClipsOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked trim references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = TrimLinkedClipsOperation{operation.link_group_id,
                                           operation.trims, before};
        return true;
    }
    if (operation.trims.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "TrimLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> seen;
    for (const LinkedClipTrim& trim : operation.trims) {
        const DocumentClip* clip = candidate.FindClip(trim.clip_id);
        const DocumentTrack* track = candidate.FindTrackForClip(trim.clip_id);
        if (!clip || !track || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), trim.clip_id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked trim members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(trim.clip_id);
        trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const LinkedClipTrim& trim : operation.trims) {
        TrimClipOperation single{trim.clip_id, trim.edge, trim.delta,
                                 std::nullopt};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyTrim(candidate, single, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(trackIds);
    inverse = TrimLinkedClipsOperation{operation.link_group_id, operation.trims,
                                       before};
    return true;
}

}  // namespace

bool CollapseClipThroughRange(const DocumentClip& original,
                              const RationalTime& rangeIn,
                              const RationalTime& rangeOut,
                              DocumentClip& mapped) {
    const RationalTime rangeDuration = rangeOut.sub(rangeIn);
    const RationalTime clipStart = original.timeline_in;
    const RationalTime clipEnd = clipStart.add(original.duration);
    mapped = original;
    if (clipEnd <= rangeIn) return true;
    if (clipStart >= rangeOut) {
        mapped.timeline_in = clipStart.sub(rangeDuration);
        return true;
    }
    const RationalTime start = clipStart < rangeIn ? clipStart : rangeIn;
    const RationalTime end =
        clipEnd >= rangeOut ? clipEnd.sub(rangeDuration) : rangeIn;
    if (end <= start) return false;  // Entirely inside the range.
    mapped.timeline_in = start;
    mapped.duration = end.sub(start);
    if (mapped.duration < mapped.audio_fade_out)
        mapped.audio_fade_out = mapped.duration;
    if (mapped.duration.sub(mapped.audio_fade_out) < mapped.audio_fade_in)
        mapped.audio_fade_in = mapped.duration.sub(mapped.audio_fade_out);
    return true;
}

namespace {

bool ApplyRippleTrim(Document& candidate, RippleTrimOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    if (operation.delta.rate <= 0) {
        Fail(EditError::ArithmeticError,
             "ripple trim delta rate must be positive", error, message);
        return false;
    }
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };
    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact ripple trim references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RippleTrimOperation{
            operation.clip_id,        operation.edge,
            operation.delta,          operation.linked_clip_ids,
            operation.sync_track_ids, before};
        return true;
    }

    DocumentClip* anchor = candidate.FindClip(operation.clip_id);
    const DocumentTrack* anchorTrack =
        candidate.FindTrackForClip(operation.clip_id);
    if (!anchor || !anchorTrack) {
        Fail(EditError::UnknownClip, "unknown ripple trim clip", error,
             message);
        return false;
    }
    std::vector<Ulid> trimIds{operation.clip_id};
    for (const Ulid& id : operation.linked_clip_ids)
        if (std::find(trimIds.begin(), trimIds.end(), id) == trimIds.end())
            trimIds.push_back(id);
    for (const Ulid& id : trimIds) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track ||
            (id != operation.clip_id &&
             (anchor->link_group_id.empty() ||
              clip->link_group_id != anchor->link_group_id))) {
            Fail(EditError::InvalidOperation,
                 "ripple trim linked clips must share one group", error,
                 message);
            return false;
        }
        addTrack(track->id);
    }
    for (const Ulid& id : operation.sync_track_ids) {
        if (!candidate.FindTrack(id)) {
            Fail(EditError::UnknownTrack,
                 "unknown ripple sync track_id '" + id + "'", error, message);
            return false;
        }
        addTrack(id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    const RationalTime primaryBoundary =
        operation.edge == TrimEdge::Head
            ? anchor->timeline_in
            : anchor->timeline_in.add(anchor->duration);
    RationalTime sequenceDelta = operation.delta;
    if (operation.edge == TrimEdge::Head &&
        !Negate(operation.delta, sequenceDelta)) {
        Fail(EditError::ArithmeticError, "ripple delta cannot be negated",
             error, message);
        return false;
    }
    std::map<Ulid, RationalTime> followingBoundary;
    for (const Ulid& id : trimIds) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        const RationalTime originalEnd = clip->timeline_in.add(clip->duration);
        followingBoundary[track->id] = originalEnd;
        ExactClipTimes requested = TimesOf(*clip);
        if (operation.edge == TrimEdge::Head) {
            requested.source_in = requested.source_in.add(
                ClipSourceAdvance(*clip, operation.delta));
            requested.duration = requested.duration.sub(operation.delta);
        } else {
            requested.duration = requested.duration.add(operation.delta);
        }
        const std::optional<DocumentSource> source =
            ClipSource(candidate, *clip);
        if (!ValidateEditedDuration("ripple_trim", *clip, requested, error,
                                    message)) {
            return false;
        }
        if (!source) {
            Fail(EditError::UnknownSource,
                 "ripple trim clip references an unknown source", error,
                 message);
            return false;
        }
        if (!ValidateEditedSourceRange(
                "ripple_trim", *clip, *source, requested, error, message,
                operation.edge == TrimEdge::Head ? operation.delta
                                                 : RationalTime{0, 1})) {
            return false;
        }
    }
    for (const Ulid& id : trimIds) {
        DocumentClip* clip = candidate.FindClip(id);
        if (operation.edge == TrimEdge::Head) {
            clip->source_in =
                clip->source_in.add(ClipSourceAdvance(*clip, operation.delta));
            clip->duration = clip->duration.sub(operation.delta);
            // R2 -- the head advanced by the delta, so the keys move back
            // by it: the zoom stays on the frames it was put on.
            ShiftClipAnimationHead(*clip, operation.delta);
        } else {
            clip->duration = clip->duration.add(operation.delta);
        }
    }
    for (const auto& boundary : followingBoundary) {
        DocumentTrack* track = candidate.FindTrack(boundary.first);
        for (DocumentClip& clip : track->clips) {
            if (std::find(trimIds.begin(), trimIds.end(), clip.id) !=
                trimIds.end())
                continue;
            if (clip.timeline_in >= boundary.second)
                clip.timeline_in = clip.timeline_in.add(sequenceDelta);
        }
    }
    // AX-2026-09. A trim that takes time out takes the same stretch out of
    // each sync-locked follower: [edit point, edit point + delta) at a head,
    // [new end, old end) at a tail. Moving every follower clip that starts
    // at or after the edit point back by the delta, as this did, slid a
    // cutaway that started on the cut into the shot before it (refused as an
    // overlap on a run) or, with nothing there, out of sync with the words.
    // A trim that adds time still opens it at the edit point.
    const bool removesTime = sequenceDelta.value < 0;
    const RationalTime removedIn = operation.edge == TrimEdge::Head
                                       ? primaryBoundary
                                       : primaryBoundary.add(sequenceDelta);
    const RationalTime removedOut = removedIn.sub(sequenceDelta);
    for (const Ulid& id : operation.sync_track_ids) {
        if (followingBoundary.count(id)) continue;
        DocumentTrack* track = candidate.FindTrack(id);
        if (!removesTime) {
            for (DocumentClip& clip : track->clips)
                if (clip.timeline_in >= primaryBoundary)
                    clip.timeline_in = clip.timeline_in.add(sequenceDelta);
            continue;
        }
        std::vector<DocumentClip> collapsed;
        for (const DocumentClip& clip : track->clips) {
            DocumentClip mapped;
            if (CollapseClipThroughRange(clip, removedIn, removedOut, mapped))
                collapsed.push_back(std::move(mapped));
        }
        track->clips = std::move(collapsed);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = RippleTrimOperation{
        operation.clip_id,         operation.edge,           operation.delta,
        operation.linked_clip_ids, operation.sync_track_ids, before};
    return true;
}

// B4 -- ROADMAP.md. Boundary cleanup is deliberately a small compound over
// RippleTrim rather than a second implementation of ripple geometry. The
// outer ApplyOperation already works on a candidate document, so a refusal
// from any constituent trim discards them all. One exact snapshot then makes
// the successful compound one undo/redo step.
bool ApplyTrimBoundaryAir(Document& candidate,
                          TrimBoundaryAirOperation& operation,
                          Operation& inverse, EditError& error,
                          std::string& message) {
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };

    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact boundary trim references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = TrimBoundaryAirOperation{operation.trims,
                                           operation.sync_track_ids, before};
        return true;
    }

    if (operation.trims.empty()) {
        Fail(EditError::InvalidOperation,
             "TrimBoundaryAir requires at least one boundary", error, message);
        return false;
    }
    std::vector<std::pair<Ulid, TrimEdge>> seen;
    for (const BoundaryAirTrim& trim : operation.trims) {
        if (trim.delta.rate <= 0 ||
            (trim.edge == TrimEdge::Head && trim.delta.value <= 0) ||
            (trim.edge == TrimEdge::Tail && trim.delta.value >= 0)) {
            Fail(EditError::InvalidOperation,
                 "TrimBoundaryAir only accepts shortening deltas", error,
                 message);
            return false;
        }
        const std::pair<Ulid, TrimEdge> key{trim.clip_id, trim.edge};
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            Fail(EditError::DuplicateId,
                 "TrimBoundaryAir repeats a clip boundary", error, message);
            return false;
        }
        seen.push_back(key);
        const DocumentTrack* anchorTrack =
            candidate.FindTrackForClip(trim.clip_id);
        if (!anchorTrack) {
            Fail(EditError::UnknownClip,
                 "unknown boundary trim clip_id '" + trim.clip_id + "'", error,
                 message);
            return false;
        }
        addTrack(anchorTrack->id);
        for (const Ulid& linkedId : trim.linked_clip_ids) {
            const DocumentTrack* linkedTrack =
                candidate.FindTrackForClip(linkedId);
            if (!linkedTrack) {
                Fail(EditError::UnknownClip,
                     "unknown boundary trim linked clip_id '" + linkedId + "'",
                     error, message);
                return false;
            }
            addTrack(linkedTrack->id);
        }
    }
    for (const Ulid& trackId : operation.sync_track_ids) {
        if (!candidate.FindTrack(trackId)) {
            Fail(EditError::UnknownTrack,
                 "unknown boundary trim sync track_id '" + trackId + "'", error,
                 message);
            return false;
        }
        addTrack(trackId);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);

    for (const BoundaryAirTrim& trim : operation.trims) {
        RippleTrimOperation ripple{trim.clip_id,
                                   trim.edge,
                                   trim.delta,
                                   trim.linked_clip_ids,
                                   operation.sync_track_ids,
                                   {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyRippleTrim(candidate, ripple, ignored, error, message))
            return false;
    }
    operation.exact_track_result = snapshots(trackIds);
    inverse = TrimBoundaryAirOperation{operation.trims,
                                       operation.sync_track_ids, before};
    return true;
}

bool ApplyRollEdit(Document& candidate, RollEditOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.delta.rate <= 0 || operation.pairs.empty()) {
        Fail(EditError::InvalidOperation,
             "roll edit requires pairs and a positive delta rate", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };
    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact roll edit references an unknown track", error, message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RollEditOperation{operation.pairs, operation.delta, before};
        return true;
    }
    // BENCH-2026-09 -- a caption clip reads no media: its text is the whole
    // of it, and a head trim moves where it starts without a source to slide
    // (ApplyTrim does the same). roll_edit demanded a source and refused
    // every pair of captions as UnknownSource, so moving the boundary between
    // two cartons took two trim_clip calls, two undo steps, and a moment where
    // the two overlapped or left a hole.
    const auto isCaption = [&](const DocumentTrack* track) {
        return track != nullptr && track->kind == "caption";
    };
    std::vector<Ulid> seenClips;
    for (const RollEditPair& pair : operation.pairs) {
        DocumentClip* left = candidate.FindClip(pair.left_clip_id);
        DocumentClip* right = candidate.FindClip(pair.right_clip_id);
        const DocumentTrack* leftTrack =
            candidate.FindTrackForClip(pair.left_clip_id);
        const DocumentTrack* rightTrack =
            candidate.FindTrackForClip(pair.right_clip_id);
        if (left != nullptr && right != nullptr && leftTrack != nullptr &&
            rightTrack != nullptr &&
            isCaption(leftTrack) != isCaption(rightTrack)) {
            Fail(EditError::InvalidOperation,
                 "roll edit cannot pair caption clip '" +
                     (isCaption(leftTrack) ? left->id : right->id) +
                     "' with media clip '" +
                     (isCaption(leftTrack) ? right->id : left->id) +
                     "': a caption only rolls against the adjacent caption",
                 error, message);
            return false;
        }
        if (!left || !right || !leftTrack || !rightTrack ||
            leftTrack->id != rightTrack->id ||
            left->timeline_in.add(left->duration) != right->timeline_in ||
            std::find(seenClips.begin(), seenClips.end(), left->id) !=
                seenClips.end() ||
            std::find(seenClips.begin(), seenClips.end(), right->id) !=
                seenClips.end()) {
            Fail(EditError::InvalidOperation,
                 "roll edit requires unique contiguous clip pairs", error,
                 message);
            return false;
        }
        seenClips.push_back(left->id);
        seenClips.push_back(right->id);
        addTrack(leftTrack->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const RollEditPair& pair : operation.pairs) {
        const DocumentClip* left = candidate.FindClip(pair.left_clip_id);
        const DocumentClip* right = candidate.FindClip(pair.right_clip_id);
        const bool captions =
            isCaption(candidate.FindTrackForClip(pair.left_clip_id));
        ExactClipTimes leftRequested = TimesOf(*left);
        ExactClipTimes rightRequested = TimesOf(*right);
        leftRequested.duration = leftRequested.duration.add(operation.delta);
        if (!captions) {
            rightRequested.source_in = rightRequested.source_in.add(
                ClipSourceAdvance(*right, operation.delta));
        }
        rightRequested.duration = rightRequested.duration.sub(operation.delta);
        rightRequested.timeline_in =
            rightRequested.timeline_in.add(operation.delta);
        if (captions) {
            // A carton rolled to nothing would lose its text with it. Merging
            // two cartons is a different gesture (clear_clip, then trim_clip)
            // and must be asked for as one.
            const DocumentClip* emptied =
                leftRequested.duration.value <= 0
                    ? left
                    : (rightRequested.duration.value <= 0 ? right : nullptr);
            if (emptied != nullptr) {
                Fail(EditError::InvalidOperation,
                     "roll edit would leave caption clip '" + emptied->id +
                         "' with no duration; a caption keeps at least part "
                         "of its span",
                     error, message);
                return false;
            }
        }
        if (!ValidateEditedDuration("roll_edit", *left, leftRequested, error,
                                    message) ||
            !ValidateEditedDuration("roll_edit", *right, rightRequested, error,
                                    message)) {
            return false;
        }
        if (captions) {
            continue;
        }
        const std::optional<DocumentSource> leftSource =
            ClipSource(candidate, *left);
        const std::optional<DocumentSource> rightSource =
            ClipSource(candidate, *right);
        if (!leftSource || !rightSource) {
            Fail(EditError::UnknownSource,
                 "roll edit pair references an unknown source", error, message);
            return false;
        }
        if (!ValidateEditedSourceRange("roll_edit", *left, *leftSource,
                                       leftRequested, error, message) ||
            !ValidateEditedSourceRange("roll_edit", *right, *rightSource,
                                       rightRequested, error, message,
                                       operation.delta))
            return false;
    }
    for (const RollEditPair& pair : operation.pairs) {
        DocumentClip* left = candidate.FindClip(pair.left_clip_id);
        DocumentClip* right = candidate.FindClip(pair.right_clip_id);
        left->duration = left->duration.add(operation.delta);
        if (!isCaption(candidate.FindTrackForClip(pair.right_clip_id))) {
            right->source_in = right->source_in.add(
                ClipSourceAdvance(*right, operation.delta));
        }
        right->duration = right->duration.sub(operation.delta);
        right->timeline_in = right->timeline_in.add(operation.delta);
        // R2 -- a roll moves the shared cut: the right clip's head moved,
        // the left clip's did not. Only the right one's keys shift.
        ShiftClipAnimationHead(*right, operation.delta);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = RollEditOperation{operation.pairs, operation.delta, before};
    return true;
}

bool ApplySlipEdit(Document& candidate, SlipEditOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.delta.rate <= 0 || operation.clip_ids.empty()) {
        Fail(EditError::InvalidOperation,
             "slip edit requires clips and a positive delta rate", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };
    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact slip edit references an unknown track", error, message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse =
            SlipEditOperation{operation.clip_ids, operation.delta, before};
        return true;
    }

    std::vector<Ulid> seen;
    Ulid linkGroup;
    for (const Ulid& id : operation.clip_ids) {
        DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "slip edit clip IDs must exist and be unique", error, message);
            return false;
        }
        if (operation.clip_ids.size() > 1) {
            if (clip->link_group_id.empty()) {
                Fail(EditError::InvalidOperation,
                     "multi-clip slip requires one linked group", error,
                     message);
                return false;
            }
            if (linkGroup.empty()) linkGroup = clip->link_group_id;
            if (clip->link_group_id != linkGroup) {
                Fail(EditError::InvalidOperation,
                     "multi-clip slip members must share one linked group",
                     error, message);
                return false;
            }
        }
        seen.push_back(id);
        addTrack(track->id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    for (const Ulid& id : operation.clip_ids) {
        DocumentClip* clip = candidate.FindClip(id);
        const std::optional<DocumentSource> source =
            ClipSource(candidate, *clip);
        if (!source) {
            Fail(EditError::UnknownSource,
                 "slip edit clip references an unknown source", error, message);
            return false;
        }
        // Criterion 4 -- a ramp has no one speed to scale a slip by, and
        // its integral would not undo: slipping by d and then by -d has to
        // come back where it was, which a curve's two integrals do not. So
        // a ramped clip slips by d of source time, as at 1/1; its keys stay
        // on their frames, R2's rule for a slip.
        clip->source_in = clip->source_in.add(
            ClipHasSpeedRamp(*clip)
                ? operation.delta
                : ScaleBySpeed(operation.delta, clip->speed));
        if (!ValidateSourceRange(*source, clip->source_in,
                                 ClipSourceSpan(*clip), error, message))
            return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = SlipEditOperation{operation.clip_ids, operation.delta, before};
    return true;
}

// The exact snapshots of `trackIds`, in that order: the redo and the
// inverse of the FC3 operations, as of roll and slip.
std::vector<ExactTrackState> TrackSnapshots(const Document& document,
                                            const std::vector<Ulid>& trackIds) {
    std::vector<ExactTrackState> states;
    for (const Ulid& id : trackIds) {
        const DocumentTrack* track = document.FindTrack(id);
        if (track) states.push_back({id, track->clips});
    }
    return states;
}

// Replays recorded snapshots: what redo and undo of a snapshot operation
// do. `before` receives the tracks as they were.
bool RestoreTrackSnapshots(Document& candidate,
                           const std::vector<ExactTrackState>& states,
                           const char* what,
                           std::vector<ExactTrackState>& before,
                           EditError& error, std::string& message) {
    std::vector<Ulid> trackIds;
    for (const ExactTrackState& state : states)
        if (std::find(trackIds.begin(), trackIds.end(), state.track_id) ==
            trackIds.end())
            trackIds.push_back(state.track_id);
    before = TrackSnapshots(candidate, trackIds);
    if (before.size() != states.size()) {
        Fail(EditError::UnknownTrack,
             std::string("exact ") + what + " references an unknown track",
             error, message);
        return false;
    }
    for (const ExactTrackState& state : states)
        candidate.FindTrack(state.track_id)->clips = state.clips;
    return ValidateResult(candidate, error, message);
}

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. See SlideEditOperation. The
// neighbours' edits are the two halves of a roll each (the left one's tail,
// the right one's head), so they are checked by the same functions roll
// checks them with, and a transition on either cut is left to
// Document::Validate, as for roll: its handles are measured on the clips as
// they end up.
bool ApplySlideEdit(Document& candidate, SlideEditOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    if (operation.delta.rate <= 0 || operation.clip_ids.empty()) {
        Fail(EditError::InvalidOperation,
             "slide edit requires clips and a positive delta rate", error,
             message);
        return false;
    }
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        if (!RestoreTrackSnapshots(candidate, operation.exact_track_result,
                                   "slide edit", before, error, message))
            return false;
        inverse =
            SlideEditOperation{operation.clip_ids, operation.delta, before};
        return true;
    }

    struct Slid {
        Ulid clip;
        Ulid left;
        Ulid right;
        bool caption = false;
    };
    std::vector<Slid> slides;
    std::vector<Ulid> trackIds;
    std::vector<Ulid> touched;
    Ulid linkGroup;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track) {
            Fail(EditError::UnknownClip,
                 "unknown slide edit clip_id '" + id + "'", error, message);
            return false;
        }
        if (operation.clip_ids.size() > 1) {
            if (clip->link_group_id.empty() ||
                (!linkGroup.empty() && clip->link_group_id != linkGroup)) {
                Fail(EditError::InvalidOperation,
                     "multi-clip slide members must share one linked group",
                     error, message);
                return false;
            }
            linkGroup = clip->link_group_id;
        }
        const RationalTime clipEnd = clip->timeline_in.add(clip->duration);
        const DocumentClip* left = nullptr;
        const DocumentClip* right = nullptr;
        for (const DocumentClip& other : track->clips) {
            if (other.id == clip->id) continue;
            if (other.timeline_in.add(other.duration) == clip->timeline_in)
                left = &other;
            if (other.timeline_in == clipEnd) right = &other;
        }
        if (!left || !right) {
            Fail(EditError::InvalidOperation,
                 "slide edit needs a clip touching clip_id '" + id +
                     "' on each side of its track; " +
                     (left ? "nothing starts where it ends"
                           : "nothing ends where it starts") +
                     " (move_clip moves a clip into a gap)",
                 error, message);
            return false;
        }
        // A clip slid twice, or a neighbour shared by two slides (or slid
        // itself), would take two edits in one gesture at once.
        for (const Ulid& member : {clip->id, left->id, right->id}) {
            if (std::find(touched.begin(), touched.end(), member) !=
                touched.end()) {
                Fail(EditError::InvalidOperation,
                     "slide edit touches clip_id '" + member +
                         "' twice: each slid clip and its two neighbours must "
                         "be distinct from every other slide's",
                     error, message);
                return false;
            }
            touched.push_back(member);
        }
        slides.push_back(
            {clip->id, left->id, right->id, track->kind == "caption"});
        if (std::find(trackIds.begin(), trackIds.end(), track->id) ==
            trackIds.end())
            trackIds.push_back(track->id);
    }
    const std::vector<ExactTrackState> before =
        TrackSnapshots(candidate, trackIds);

    for (const Slid& slide : slides) {
        const DocumentClip* clip = candidate.FindClip(slide.clip);
        const DocumentClip* left = candidate.FindClip(slide.left);
        const DocumentClip* right = candidate.FindClip(slide.right);
        ExactClipTimes clipRequested = TimesOf(*clip);
        ExactClipTimes leftRequested = TimesOf(*left);
        ExactClipTimes rightRequested = TimesOf(*right);
        clipRequested.timeline_in =
            clipRequested.timeline_in.add(operation.delta);
        leftRequested.duration = leftRequested.duration.add(operation.delta);
        if (!slide.caption) {
            rightRequested.source_in = rightRequested.source_in.add(
                ClipSourceAdvance(*right, operation.delta));
        }
        rightRequested.duration = rightRequested.duration.sub(operation.delta);
        rightRequested.timeline_in =
            rightRequested.timeline_in.add(operation.delta);
        if (!ValidateEditedDuration("slide_edit", *left, leftRequested, error,
                                    message) ||
            !ValidateEditedDuration("slide_edit", *right, rightRequested, error,
                                    message) ||
            !ValidateEditedDuration("slide_edit", *clip, clipRequested, error,
                                    message))
            return false;
        if (slide.caption) continue;
        const std::optional<DocumentSource> leftSource =
            ClipSource(candidate, *left);
        const std::optional<DocumentSource> rightSource =
            ClipSource(candidate, *right);
        if (!leftSource || !rightSource) {
            Fail(EditError::UnknownSource,
                 "slide edit neighbour references an unknown source", error,
                 message);
            return false;
        }
        if (!ValidateEditedSourceRange("slide_edit", *left, *leftSource,
                                       leftRequested, error, message) ||
            !ValidateEditedSourceRange("slide_edit", *right, *rightSource,
                                       rightRequested, error, message,
                                       operation.delta))
            return false;
    }
    for (const Slid& slide : slides) {
        DocumentClip* clip = candidate.FindClip(slide.clip);
        DocumentClip* left = candidate.FindClip(slide.left);
        DocumentClip* right = candidate.FindClip(slide.right);
        left->duration = left->duration.add(operation.delta);
        if (!slide.caption) {
            right->source_in = right->source_in.add(
                ClipSourceAdvance(*right, operation.delta));
        }
        right->duration = right->duration.sub(operation.delta);
        right->timeline_in = right->timeline_in.add(operation.delta);
        // R2 -- the right neighbour's head moved, so its keys move back and
        // stay on their frames; the slid clip and the left one keep their
        // heads, and their keys are local to them.
        ShiftClipAnimationHead(*right, operation.delta);
        clip->timeline_in = clip->timeline_in.add(operation.delta);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = TrackSnapshots(candidate, trackIds);
    inverse = SlideEditOperation{operation.clip_ids, operation.delta, before};
    return true;
}

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. The speed that makes `span` of
// source last `duration` of timeline, exactly and reduced, or false when
// that fraction has no ClipSpeedIsValid spelling.
bool StretchedSpeed(const RationalTime& span, const RationalTime& duration,
                    EffectParamValue& speed) {
    if (span.rate <= 0 || duration.rate <= 0 || duration.value <= 0 ||
        span.value < 0)
        return false;
    // (span.value / span.rate) / (duration.value / duration.rate)
    __int128 num = static_cast<__int128>(span.value) * duration.rate;
    __int128 den = static_cast<__int128>(span.rate) * duration.value;
    __int128 left = num;
    __int128 right = den;
    while (right != 0) {
        const __int128 remainder = left % right;
        left = right;
        right = remainder;
    }
    if (left > 1) {
        num /= left;
        den /= left;
    }
    if (num > kMaxClipSpeedTerm || den > kMaxClipSpeedTerm) return false;
    speed = {static_cast<int32_t>(num), static_cast<int32_t>(den)};
    if (speed.num == 0) speed.den = 1;
    return true;
}

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. See RateStretchOperation.
bool ApplyRateStretch(Document& candidate, RateStretchOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    if (operation.duration.rate <= 0 || operation.duration.value <= 0) {
        Fail(EditError::InvalidDuration,
             "rate_stretch duration must be positive; requested " +
                 DescribeTime(operation.duration),
             error, message);
        return false;
    }
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        if (!RestoreTrackSnapshots(candidate, operation.exact_track_result,
                                   "rate stretch", before, error, message))
            return false;
        inverse =
            RateStretchOperation{operation.clip_id,  operation.edge,
                                 operation.duration, before,
                                 operation.ripple,   operation.sync_track_ids};
        return true;
    }
    if (!operation.ripple && !operation.sync_track_ids.empty()) {
        Fail(EditError::InvalidOperation,
             "rate_stretch sync_track_ids only say which tracks follow a "
             "ripple; set ripple to true or leave them out",
             error, message);
        return false;
    }
    const DocumentClip* anchor = candidate.FindClip(operation.clip_id);
    if (!anchor) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    // The anchor first, then the rest of its link group in track order.
    std::vector<Ulid> members{anchor->id};
    if (!anchor->link_group_id.empty()) {
        for (const DocumentTrack& track : candidate.sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (clip.id != anchor->id &&
                    clip.link_group_id == anchor->link_group_id)
                    members.push_back(clip.id);
    }
    std::vector<Ulid> trackIds;
    for (const Ulid& id : members) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!track || (track->kind != "video" && track->kind != "audio")) {
            Fail(EditError::InvalidOperation,
                 "rate_stretch is only supported on video and audio clips; "
                 "clip_id '" +
                     id + "' is not on one",
                 error, message);
            return false;
        }
        if (IsMotionClip(*clip)) {
            Fail(EditError::InvalidOperation,
                 "rate_stretch on motion clip '" + id +
                     "' is refused: a composition is rendered at the "
                     "sequence cadence, so its speed belongs to the "
                     "composition (R3)",
                 error, message);
            return false;
        }
        if (ClipHasSpeedRamp(*clip)) {
            Fail(EditError::InvalidOperation,
                 "rate_stretch on clip '" + id +
                     "' is refused: it carries a speed ramp, whose keys "
                     "would have to be rescaled, not one speed to replace",
                 error, message);
            return false;
        }
        if (ClipIsFreeze(*clip)) {
            Fail(EditError::InvalidOperation,
                 "rate_stretch on clip '" + id +
                     "' is refused: a freeze (speed 0/1) reads no source "
                     "span to stretch; trim_clip changes how long it holds",
                 error, message);
            return false;
        }
        if (clip->duration != anchor->duration ||
            clip->speed.num != anchor->speed.num ||
            clip->speed.den != anchor->speed.den) {
            Fail(EditError::InvalidOperation,
                 "rate_stretch refuses link group '" + anchor->link_group_id +
                     "': member '" + id +
                     "' does not have the duration and speed of clip '" +
                     anchor->id +
                     "', so the group would leave with several speeds (R3)",
                 error, message);
            return false;
        }
        if (std::find(trackIds.begin(), trackIds.end(), track->id) !=
            trackIds.end()) {
            // DEC40 -- the ripple of one member would move the other.
            if (operation.ripple) {
                Fail(EditError::InvalidOperation,
                     "rate_stretch with ripple refuses link group '" +
                         anchor->link_group_id + "': member '" + id +
                         "' shares track '" + track->id +
                         "' with another member, whose ripple would move it",
                     error, message);
                return false;
            }
            continue;
        }
        trackIds.push_back(track->id);
    }
    // The tracks that hold a member ripple through their own clips; the
    // sync tracks follow the anchor's old end.
    const size_t memberTrackCount = trackIds.size();
    for (const Ulid& id : operation.sync_track_ids) {
        if (!candidate.FindTrack(id)) {
            Fail(EditError::UnknownTrack,
                 "unknown rate_stretch sync track_id '" + id + "'", error,
                 message);
            return false;
        }
        if (std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    }
    const RationalTime span = ClipSourceSpan(*anchor);
    EffectParamValue speed;
    if (!StretchedSpeed(span, operation.duration, speed)) {
        Fail(EditError::InvalidOperation,
             "rate_stretch of clip '" + anchor->id + "' to " +
                 DescribeTime(operation.duration) + " reads " +
                 DescribeTime(span) +
                 " of source, a speed with no exact spelling whose terms "
                 "stay at or below " +
                 std::to_string(kMaxClipSpeedTerm) +
                 "; choose a duration whose ratio to the source span is "
                 "such a fraction",
             error, message);
        return false;
    }
    // By construction; checked so that a source range never moves silently.
    if (ScaleBySpeed(operation.duration, speed).compare(span) != 0) {
        Fail(EditError::ArithmeticError,
             "rate_stretch speed does not keep the source span exactly", error,
             message);
        return false;
    }
    const std::vector<ExactTrackState> before =
        TrackSnapshots(candidate, trackIds);
    // DEC40 -- what the stretch adds to (or, negative, takes from) the
    // program after each member: every member has the anchor's duration.
    const RationalTime lengthChange = operation.duration.sub(anchor->duration);
    const RationalTime anchorOldEnd = anchor->timeline_in.add(anchor->duration);
    std::map<Ulid, RationalTime> oldEndOnTrack;
    for (const Ulid& id : members) {
        DocumentClip* clip = candidate.FindClip(id);
        ExactClipTimes requested = TimesOf(*clip);
        if (operation.edge == TrimEdge::Head && !operation.ripple) {
            requested.timeline_in =
                clip->timeline_in.add(clip->duration).sub(operation.duration);
        }
        requested.duration = operation.duration;
        if (!ValidateEditedDuration("rate_stretch", *clip, requested, error,
                                    message))
            return false;
        oldEndOnTrack[candidate.FindTrackForClip(id)->id] =
            clip->timeline_in.add(clip->duration);
        clip->timeline_in = requested.timeline_in;
        clip->duration = requested.duration;
        clip->speed = speed;
    }
    if (operation.ripple && lengthChange.value != 0) {
        for (const auto& [trackId, oldEnd] : oldEndOnTrack) {
            DocumentTrack* track = candidate.FindTrack(trackId);
            for (DocumentClip& clip : track->clips) {
                if (std::find(members.begin(), members.end(), clip.id) !=
                    members.end())
                    continue;
                if (clip.timeline_in >= oldEnd)
                    clip.timeline_in = clip.timeline_in.add(lengthChange);
            }
        }
        // The sync tracks, as RippleTrimOperation treats a tail: time added
        // opens at the old end, time taken out is [new end, old end).
        const RationalTime removedIn = anchorOldEnd.add(lengthChange);
        for (size_t index = memberTrackCount; index < trackIds.size();
             ++index) {
            DocumentTrack* track = candidate.FindTrack(trackIds[index]);
            if (lengthChange.value > 0) {
                for (DocumentClip& clip : track->clips)
                    if (clip.timeline_in >= anchorOldEnd)
                        clip.timeline_in = clip.timeline_in.add(lengthChange);
                continue;
            }
            std::vector<DocumentClip> collapsed;
            for (const DocumentClip& clip : track->clips) {
                DocumentClip mapped;
                if (CollapseClipThroughRange(clip, removedIn, anchorOldEnd,
                                             mapped))
                    collapsed.push_back(std::move(mapped));
            }
            track->clips = std::move(collapsed);
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = TrackSnapshots(candidate, trackIds);
    inverse = RateStretchOperation{
        operation.clip_id, operation.edge,          operation.duration, before,
        operation.ripple,  operation.sync_track_ids};
    return true;
}

bool ApplyRemoveLinked(Document& candidate,
                       RemoveLinkedClipsOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact linked remove references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RemoveLinkedClipsOperation{operation.link_group_id,
                                             operation.clip_ids,
                                             operation.sync_track_ids, before};
        return true;
    }
    if (operation.clip_ids.size() < 2 || operation.link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "RemoveLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> trackIds;
    std::vector<Ulid> memberTrackIds;
    std::vector<Ulid> seen;
    const DocumentClip* rippleAnchor = nullptr;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || !track || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked remove members must be unique and share link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(id);
        if (std::find(trackIds.begin(), trackIds.end(), track->id) ==
            trackIds.end())
            trackIds.push_back(track->id);
        if (std::find(memberTrackIds.begin(), memberTrackIds.end(),
                      track->id) == memberTrackIds.end())
            memberTrackIds.push_back(track->id);
        if (!rippleAnchor || clip->sync_anchor_clip_id == clip->id)
            rippleAnchor = clip;
    }
    for (const Ulid& syncTrackId : operation.sync_track_ids) {
        if (!candidate.FindTrack(syncTrackId)) {
            Fail(EditError::UnknownTrack,
                 "unknown linked remove sync track_id '" + syncTrackId + "'",
                 error, message);
            return false;
        }
        if (std::find(trackIds.begin(), trackIds.end(), syncTrackId) ==
            trackIds.end())
            trackIds.push_back(syncTrackId);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);
    const RationalTime followingBoundary =
        rippleAnchor->timeline_in.add(rippleAnchor->duration);
    RationalTime rippleDelta;
    if (!Negate(rippleAnchor->duration, rippleDelta)) {
        Fail(EditError::ArithmeticError,
             "linked remove duration cannot be negated", error, message);
        return false;
    }
    for (const Ulid& id : operation.clip_ids) {
        RemoveClipOperation single{id, {}, {}};
        Operation ignored = RemoveClipOperation{};
        if (!ApplyRemove(candidate, single, ignored, error, message))
            return false;
    }
    for (const Ulid& syncTrackId : operation.sync_track_ids) {
        if (std::find(memberTrackIds.begin(), memberTrackIds.end(),
                      syncTrackId) != memberTrackIds.end())
            continue;
        ShiftPositionsAtOrAfter(*candidate.FindTrack(syncTrackId),
                                followingBoundary, rippleDelta);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse =
        RemoveLinkedClipsOperation{operation.link_group_id, operation.clip_ids,
                                   operation.sync_track_ids, before};
    return true;
}

bool ApplyClearLinked(Document& candidate, ClearLinkedClipsOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrackForClip(id);
            if (!track || std::any_of(result.begin(), result.end(),
                                      [&](const ExactTrackState& state) {
                                          return state.track_id == track->id;
                                      }))
                continue;
            result.push_back({track->id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        for (const ExactTrackState& state : operation.exact_track_result) {
            DocumentTrack* track = candidate.FindTrack(state.track_id);
            if (!track) {
                Fail(EditError::UnknownTrack,
                     "exact linked clear references an unknown track", error,
                     message);
                return false;
            }
            before.push_back({track->id, track->clips});
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = ClearLinkedClipsOperation{operation.link_group_id,
                                            operation.clip_ids, before};
        return true;
    }
    if (operation.link_group_id.empty() || operation.clip_ids.size() < 2) {
        Fail(EditError::InvalidOperation,
             "ClearLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> seen;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        if (!clip || clip->link_group_id != operation.link_group_id ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "linked clear members must be unique and share "
                 "link_group_id",
                 error, message);
            return false;
        }
        seen.push_back(id);
    }
    const std::vector<ExactTrackState> before = snapshots(operation.clip_ids);
    for (const Ulid& id : operation.clip_ids) {
        DocumentTrack* track = candidate.FindTrackForClip(id);
        track->clips.erase(
            std::remove_if(
                track->clips.begin(), track->clips.end(),
                [&](const DocumentClip& clip) { return clip.id == id; }),
            track->clips.end());
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = before;
    for (ExactTrackState& state : operation.exact_track_result)
        state.clips = candidate.FindTrack(state.track_id)->clips;
    inverse = ClearLinkedClipsOperation{operation.link_group_id,
                                        operation.clip_ids, before};
    return true;
}

bool ApplyDeleteGap(Document& candidate, DeleteGapOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    std::vector<Ulid> trackIds{operation.track_id};
    for (const Ulid& id : operation.linked_track_ids) {
        if (id == operation.track_id ||
            std::find(trackIds.begin(), trackIds.end(), id) != trackIds.end()) {
            Fail(EditError::InvalidOperation,
                 "linked gap track_ids must be unique", error, message);
            return false;
        }
        trackIds.push_back(id);
    }
    std::vector<ExactTrackState> before;
    for (const Ulid& id : trackIds) {
        DocumentTrack* track = candidate.FindTrack(id);
        if (!track) {
            Fail(EditError::UnknownTrack, "unknown track_id '" + id + "'",
                 error, message);
            return false;
        }
        before.push_back({track->id, track->clips});
    }
    if (!operation.exact_track_result.empty()) {
        if (operation.exact_track_result.size() != trackIds.size()) {
            Fail(EditError::InvalidOperation,
                 "exact gap state must contain every destination track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result) {
            if (std::find(trackIds.begin(), trackIds.end(), state.track_id) ==
                trackIds.end()) {
                Fail(EditError::InvalidOperation,
                     "exact gap state contains an unexpected track", error,
                     message);
                return false;
            }
            candidate.FindTrack(state.track_id)->clips = state.clips;
        }
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = DeleteGapOperation{operation.track_id, operation.gap_start,
                                     operation.gap_duration, before,
                                     operation.linked_track_ids};
        return true;
    }
    if (operation.gap_start.rate <= 0 || operation.gap_start.value < 0 ||
        operation.gap_duration.rate <= 0 || operation.gap_duration.value <= 0) {
        Fail(EditError::InvalidOperation,
             "gap start and duration must describe a positive range", error,
             message);
        return false;
    }
    const RationalTime gapEnd = operation.gap_start.add(operation.gap_duration);
    for (const Ulid& id : trackIds) {
        DocumentTrack* track = candidate.FindTrack(id);
        bool hasFollowingClip = false;
        for (const DocumentClip& clip : track->clips) {
            const RationalTime clipEnd = clip.timeline_in.add(clip.duration);
            if (clip.timeline_in < gapEnd && clipEnd > operation.gap_start) {
                Fail(EditError::InvalidOperation,
                     "DeleteGap range contains clip_id '" + clip.id + "'",
                     error, message);
                return false;
            }
            if (clip.timeline_in >= gapEnd) hasFollowingClip = true;
        }
        if (!hasFollowingClip) {
            Fail(EditError::InvalidOperation,
                 "DeleteGap requires a following clip on every linked track",
                 error, message);
            return false;
        }
        for (DocumentClip& clip : track->clips) {
            if (clip.timeline_in >= gapEnd)
                clip.timeline_in = clip.timeline_in.sub(operation.gap_duration);
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result.clear();
    for (const Ulid& id : trackIds) {
        const DocumentTrack* track = candidate.FindTrack(id);
        operation.exact_track_result.push_back({id, track->clips});
    }
    inverse = DeleteGapOperation{operation.track_id, operation.gap_start,
                                 operation.gap_duration, before,
                                 operation.linked_track_ids};
    return true;
}

bool ApplyDetachAudio(Document& candidate, DetachAudioOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentTrack* videoTrack =
        candidate.FindTrackForClip(operation.video_clip_id);
    DocumentTrack* audioTrack = candidate.FindTrack(operation.audio_track_id);
    if (!videoTrack) {
        Fail(EditError::UnknownClip,
             "unknown video_clip_id '" + operation.video_clip_id + "'", error,
             message);
        return false;
    }
    if (!audioTrack) {
        Fail(EditError::UnknownTrack,
             "unknown audio_track_id '" + operation.audio_track_id + "'", error,
             message);
        return false;
    }
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            if (std::any_of(result.begin(), result.end(),
                            [&](const ExactTrackState& state) {
                                return state.track_id == id;
                            }))
                continue;
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) result.push_back({id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<Ulid> ids;
        for (const ExactTrackState& state : operation.exact_track_result)
            ids.push_back(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(ids);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact detach state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = DetachAudioOperation{operation.video_clip_id,
                                       operation.audio_track_id,
                                       operation.audio_clip_id, before};
        return true;
    }
    if (videoTrack->kind != "video" || audioTrack->kind != "audio") {
        Fail(EditError::InvalidOperation,
             "DetachAudio requires a video clip and an audio track", error,
             message);
        return false;
    }
    DocumentClip* videoClip = candidate.FindClip(operation.video_clip_id);
    if (!videoClip->include_audio) {
        Fail(EditError::InvalidOperation,
             "video clip audio is already detached", error, message);
        return false;
    }
    const LibraryMedia* media =
        candidate.FindLibraryMedia(videoClip->source_id);
    if (media && media->metadata_complete && !media->has_audio) {
        Fail(EditError::InvalidOperation, "source media has no audio stream",
             error, message);
        return false;
    }
    if (operation.audio_clip_id.empty())
        operation.audio_clip_id = GenerateUlid();
    if (!IsValidUlid(operation.audio_clip_id) ||
        candidate.FindClip(operation.audio_clip_id) ||
        candidate.FindTrack(operation.audio_clip_id) ||
        candidate.FindSource(operation.audio_clip_id) ||
        candidate.FindLibraryMedia(operation.audio_clip_id)) {
        Fail(EditError::DuplicateId,
             "audio_clip_id is invalid or already exists", error, message);
        return false;
    }
    const std::vector<ExactTrackState> before =
        snapshots({videoTrack->id, audioTrack->id});
    const RationalTime detachedEnd =
        videoClip->timeline_in.add(videoClip->duration);
    for (const DocumentClip& existing : audioTrack->clips) {
        if (existing.timeline_in < detachedEnd &&
            existing.timeline_in.add(existing.duration) >
                videoClip->timeline_in) {
            Fail(EditError::Overlap,
                 "detached audio would overlap clip_id '" + existing.id + "'",
                 error, message);
            return false;
        }
    }
    DocumentClip audioClip = *videoClip;
    audioClip.id = operation.audio_clip_id;
    videoClip->include_audio = false;
    videoClip->link_group_id = operation.audio_clip_id;
    videoClip->sync_anchor_clip_id = videoClip->id;
    videoClip->sync_reference_delta = {0, 1};
    audioClip.link_group_id = operation.audio_clip_id;
    audioClip.sync_anchor_clip_id = videoClip->id;
    audioClip.sync_reference_delta = {0, 1};
    audioTrack->clips.push_back(std::move(audioClip));
    std::stable_sort(audioTrack->clips.begin(), audioTrack->clips.end(),
                     [](const DocumentClip& left, const DocumentClip& right) {
                         return left.timeline_in < right.timeline_in;
                     });
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots({videoTrack->id, audioTrack->id});
    inverse =
        DetachAudioOperation{operation.video_clip_id, operation.audio_track_id,
                             operation.audio_clip_id, before};
    return true;
}

bool ApplyAddTrack(Document& candidate, AddTrackOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    if (operation.track_id.empty()) operation.track_id = GenerateUlid();
    if (!IsValidUlid(operation.track_id) ||
        candidate.FindTrack(operation.track_id) ||
        candidate.FindClip(operation.track_id) ||
        candidate.FindSource(operation.track_id) ||
        candidate.FindLibraryMedia(operation.track_id)) {
        Fail(EditError::DuplicateId,
             "track_id is invalid or already exists: '" + operation.track_id +
                 "'",
             error, message);
        return false;
    }
    if (operation.kind != "video" && operation.kind != "audio" &&
        operation.kind != "caption") {
        Fail(EditError::InvalidOperation,
             "track kind must be 'video', 'audio' or 'caption'", error,
             message);
        return false;
    }
    if (operation.index < 0) {
        Fail(EditError::InvalidOperation, "track index must be non-negative",
             error, message);
        return false;
    }
    // Video, audio and caption tracks share one numbering: an agent that
    // asked for index 1 for a second video track, beside an audio track at 1,
    // was only told the index was not unique. Name who holds it and the
    // first index free from there, so one retry is enough.
    const auto taken = [&](int32_t index) {
        return std::find_if(
            candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
            [&](const DocumentTrack& track) { return track.index == index; });
    };
    if (const auto holder = taken(operation.index);
        holder != candidate.sequence.tracks.end()) {
        int32_t free = operation.index;
        while (taken(free) != candidate.sequence.tracks.end()) ++free;
        Fail(EditError::InvalidOperation,
             "track index " + std::to_string(operation.index) +
                 " is taken by " + holder->kind + " track '" + holder->id +
                 "' (video, audio and caption tracks share one numbering); "
                 "the first free index from there is " +
                 std::to_string(free),
             error, message);
        return false;
    }
    auto position = candidate.sequence.tracks.end();
    if (!operation.insert_before_track_id.empty()) {
        position = std::find_if(
            candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
            [&](const DocumentTrack& track) {
                return track.id == operation.insert_before_track_id;
            });
        if (position == candidate.sequence.tracks.end()) {
            Fail(EditError::UnknownTrack,
                 "unknown insert_before_track_id '" +
                     operation.insert_before_track_id + "'",
                 error, message);
            return false;
        }
    }
    candidate.sequence.tracks.insert(
        position, {operation.track_id, operation.kind, operation.index,
                   operation.clips, operation.locked, operation.sync_lock,
                   operation.visible, operation.muted, operation.solo});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveTrackOperation{operation.track_id};
    return true;
}

bool ApplyRemoveTrack(Document& candidate, RemoveTrackOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto found = std::find_if(candidate.sequence.tracks.begin(),
                                    candidate.sequence.tracks.end(),
                                    [&](const DocumentTrack& track) {
                                        return track.id == operation.track_id;
                                    });
    if (found == candidate.sequence.tracks.end()) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    const auto next = std::next(found);
    inverse = AddTrackOperation{
        found->id,
        found->kind,
        found->index,
        found->clips,
        found->locked,
        found->sync_lock,
        found->visible,
        found->muted,
        found->solo,
        next == candidate.sequence.tracks.end() ? Ulid{} : next->id};
    candidate.sequence.tracks.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplySetTrackLock(Document& candidate, SetTrackLockOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = SetTrackLockOperation{track->id, track->locked};
    track->locked = operation.locked;
    return ValidateResult(candidate, error, message);
}

bool ApplySetTrackSyncLock(Document& candidate,
                           SetTrackSyncLockOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = SetTrackSyncLockOperation{track->id, track->sync_lock};
    track->sync_lock = operation.sync_lock;
    return ValidateResult(candidate, error, message);
}

bool ApplySetTrackOutput(Document& candidate,
                         SetTrackOutputOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrack(operation.track_id);
    if (!track) {
        Fail(EditError::UnknownTrack,
             "unknown track_id '" + operation.track_id + "'", error, message);
        return false;
    }
    inverse = SetTrackOutputOperation{track->id, track->visible, track->muted,
                                      track->solo};
    track->visible = operation.visible;
    track->muted = operation.muted;
    track->solo = operation.solo;
    return ValidateResult(candidate, error, message);
}

bool ApplyUpdateSequence(Document& candidate,
                         UpdateSequenceOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    if (operation.sequence_id != candidate.sequence.id) {
        Fail(EditError::UnknownSequence,
             "unknown sequence_id '" + operation.sequence_id + "'", error,
             message);
        return false;
    }
    const DocumentSequence& current = candidate.sequence;
    inverse = UpdateSequenceOperation{current.id, current.name, current.width,
                                      current.height, current.frame_rate};
    candidate.sequence.name = operation.name;
    candidate.sequence.width = operation.width;
    candidate.sequence.height = operation.height;
    candidate.sequence.frame_rate = operation.frame_rate;
    return ValidateResult(candidate, error, message);
}

bool ApplySetColorManagement(Document& candidate,
                             SetColorManagementOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    inverse = SetColorManagementOperation{candidate.color_management};
    candidate.color_management = operation.settings;
    return ValidateResult(candidate, error, message);
}

// R6 -- docs/roadmap/REEL-2026-09.md. Nothing here decides whether the
// declared space makes sense: Document::Validate does, through
// ResolveLookLutStage, so a LUT laid down by an operation, read from a file
// or replayed from a journal all meet the same rule.
bool ApplySetSequenceLut(Document& candidate,
                         SetSequenceLutOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    inverse = SetSequenceLutOperation{candidate.sequence.look_lut};
    candidate.sequence.look_lut = operation.lut;
    return ValidateResult(candidate, error, message);
}

bool ApplyAddBin(Document& candidate, AddBinOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    if (operation.bin_id.empty()) operation.bin_id = GenerateUlid();
    if (!IsValidUlid(operation.bin_id) || candidate.FindBin(operation.bin_id) ||
        candidate.FindTrack(operation.bin_id) ||
        candidate.FindClip(operation.bin_id) ||
        candidate.FindSource(operation.bin_id) ||
        candidate.FindLibraryMedia(operation.bin_id)) {
        Fail(EditError::DuplicateId,
             "bin_id is invalid or already exists: '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.name.empty() || operation.name.size() > 128 ||
        std::any_of(operation.name.begin(), operation.name.end(),
                    [](unsigned char character) { return character < 0x20; })) {
        Fail(EditError::InvalidOperation,
             "bin name must contain between 1 and 128 bytes", error, message);
        return false;
    }
    if (!operation.parent_id.empty() &&
        !candidate.FindBin(operation.parent_id)) {
        Fail(EditError::UnknownBin,
             "unknown parent bin_id '" + operation.parent_id + "'", error,
             message);
        return false;
    }
    candidate.bins.push_back(
        {operation.bin_id, operation.name, operation.parent_id});
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveBinOperation{operation.bin_id, operation.name,
                                 operation.parent_id};
    return true;
}

bool ApplyRemoveBin(Document& candidate, RemoveBinOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    const auto found = std::find_if(
        candidate.bins.begin(), candidate.bins.end(),
        [&](const DocumentBin& bin) { return bin.id == operation.bin_id; });
    if (found == candidate.bins.end()) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (std::any_of(candidate.library.begin(), candidate.library.end(),
                    [&](const LibraryMedia& media) {
                        return media.bin_id == operation.bin_id;
                    })) {
        Fail(EditError::InvalidOperation,
             "cannot remove a bin that still contains media", error, message);
        return false;
    }
    if (std::any_of(candidate.bins.begin(), candidate.bins.end(),
                    [&](const DocumentBin& bin) {
                        return bin.parent_id == operation.bin_id;
                    })) {
        Fail(EditError::InvalidOperation,
             "cannot remove a bin that still contains child bins", error,
             message);
        return false;
    }
    operation.name = found->name;
    operation.parent_id = found->parent_id;
    inverse = AddBinOperation{found->id, found->name, found->parent_id};
    candidate.bins.erase(found);
    return ValidateResult(candidate, error, message);
}

bool ApplyRenameBin(Document& candidate, RenameBinOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    DocumentBin* bin = candidate.FindBin(operation.bin_id);
    if (!bin) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.name.empty() || operation.name.size() > 128 ||
        std::any_of(operation.name.begin(), operation.name.end(),
                    [](unsigned char character) { return character < 0x20; })) {
        Fail(EditError::InvalidOperation,
             "bin name must contain between 1 and 128 bytes", error, message);
        return false;
    }
    inverse = RenameBinOperation{bin->id, bin->name};
    bin->name = operation.name;
    return ValidateResult(candidate, error, message);
}

bool ApplyMoveBin(Document& candidate, MoveBinOperation& operation,
                  Operation& inverse, EditError& error, std::string& message) {
    DocumentBin* bin = candidate.FindBin(operation.bin_id);
    if (!bin) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    if (operation.parent_id == operation.bin_id) {
        Fail(EditError::InvalidOperation, "a bin cannot contain itself", error,
             message);
        return false;
    }
    if (!operation.parent_id.empty() &&
        !candidate.FindBin(operation.parent_id)) {
        Fail(EditError::UnknownBin,
             "unknown parent bin_id '" + operation.parent_id + "'", error,
             message);
        return false;
    }
    const DocumentBin* cursor = candidate.FindBin(operation.parent_id);
    while (cursor) {
        if (cursor->id == operation.bin_id) {
            Fail(EditError::InvalidOperation,
                 "a bin cannot be moved into one of its descendants", error,
                 message);
            return false;
        }
        cursor = candidate.FindBin(cursor->parent_id);
    }
    inverse = MoveBinOperation{bin->id, bin->parent_id};
    bin->parent_id = operation.parent_id;
    return ValidateResult(candidate, error, message);
}

bool ApplySetMediaBin(Document& candidate, SetMediaBinOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (!media) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    if (!operation.bin_id.empty() && !candidate.FindBin(operation.bin_id)) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    inverse = SetMediaBinOperation{operation.media_id, media->bin_id};
    media->bin_id = operation.bin_id;
    return ValidateResult(candidate, error, message);
}

bool ApplySetMediaProxy(Document& candidate, SetMediaProxyOperation& operation,
                        Operation& inverse, EditError& error,
                        std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (!media) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    inverse = SetMediaProxyOperation{operation.media_id, media->proxy_path};
    media->proxy_path = operation.proxy_path;
    return ValidateResult(candidate, error, message);
}

bool ApplySetMediaWhiteBalance(Document& candidate,
                               SetMediaWhiteBalanceOperation& operation,
                               Operation& inverse, EditError& error,
                               std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (!media) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    // The span a camera writes (CameraMetadata.h), or 0 for unknown.
    if (operation.kelvin != 0 &&
        (operation.kelvin < kMinimumMediaWhiteBalance ||
         operation.kelvin > kMaximumMediaWhiteBalance)) {
        Fail(EditError::ValidationFailed,
             "a white balance lies between " +
                 std::to_string(kMinimumMediaWhiteBalance) + " and " +
                 std::to_string(kMaximumMediaWhiteBalance) +
                 " K, or is 0 for unknown; got " +
                 std::to_string(operation.kelvin),
             error, message);
        return false;
    }
    inverse = SetMediaWhiteBalanceOperation{operation.media_id,
                                            media->white_balance_kelvin};
    media->white_balance_kelvin = operation.kelvin;
    return ValidateResult(candidate, error, message);
}

// PL1 -- docs/roadmap/PLANS-2026-10.md. A media's described shots, whole.
bool ApplySetMediaShots(Document& candidate, SetMediaShotsOperation& operation,
                        Operation& inverse, EditError& error,
                        std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (media == nullptr) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    std::string invalid;
    if (!ValidateMediaShots(*media, operation.shots, invalid)) {
        Fail(EditError::ValidationFailed, invalid, error, message);
        return false;
    }
    inverse = SetMediaShotsOperation{operation.media_id, media->shots};
    media->shots = operation.shots;
    return ValidateResult(candidate, error, message);
}

bool ApplySetMediaColorInput(Document& candidate,
                             SetMediaColorInputOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (media == nullptr) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    // Back to the project's input is the default spelling, whatever the
    // caller left in the other three: the file then keeps no key. DEC31 --
    // a graphic declares no curve and keeps its matrix and range.
    if (!DeclaresColorInput(operation.input) && !operation.input.graphics)
        operation.input = {};
    if (media->metadata_complete && !media->has_video &&
        (DeclaresColorInput(operation.input) || operation.input.graphics)) {
        Fail(EditError::ValidationFailed,
             "media '" + operation.media_id +
                 "' is audio-only: it has no picture to read in a colour "
                 "space",
             error, message);
        return false;
    }
    inverse =
        SetMediaColorInputOperation{operation.media_id, media->color_input};
    media->color_input = operation.input;
    return ValidateResult(candidate, error, message);
}

// GN3 -- docs/roadmap/GENERATIF-2026-10.md. Document::Validate says what a
// provenance must name; this only swaps it.
bool ApplySetMediaGeneration(Document& candidate,
                             SetMediaGenerationOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    LibraryMedia* media = candidate.FindLibraryMedia(operation.media_id);
    if (media == nullptr) {
        Fail(EditError::UnknownMedia,
             "unknown media_id '" + operation.media_id + "'", error, message);
        return false;
    }
    inverse =
        SetMediaGenerationOperation{operation.media_id, media->generation};
    media->generation = operation.generation;
    return ValidateResult(candidate, error, message);
}

// DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. The source an ingest
// mounts a library entry as: the one a remove can take back exactly.
DocumentSource IngestedSourceOf(const LibraryMedia& media) {
    return {media.id, media.path, media.rate, media.duration};
}

bool SameSource(const DocumentSource& left, const DocumentSource& right) {
    return left.id == right.id && left.path == right.path &&
           left.rate.num == right.rate.num && left.rate.den == right.rate.den &&
           left.duration == right.duration;
}

bool ApplyIngestMedia(Document& candidate, IngestMediaOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    if (operation.added.empty() && operation.filed.empty() &&
        operation.completed.empty() && operation.mounted.empty()) {
        Fail(EditError::InvalidOperation,
             "an ingest adds, files, completes or mounts at least one media",
             error, message);
        return false;
    }
    if (!operation.bin_id.empty() && !candidate.FindBin(operation.bin_id)) {
        Fail(EditError::UnknownBin, "unknown bin_id '" + operation.bin_id + "'",
             error, message);
        return false;
    }
    std::set<Ulid> named;
    for (const LibraryMedia& media : operation.added) {
        const Ulid& id = media.id;
        if (!IsValidUlid(id) || !named.insert(id).second ||
            candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
            candidate.FindBin(id) || candidate.FindTrack(id) ||
            candidate.FindClip(id)) {
            Fail(EditError::DuplicateId,
                 "ingested media_id is invalid or already exists: '" + id + "'",
                 error, message);
            return false;
        }
    }
    RemoveIngestedMediaOperation undo;
    undo.bin_id = operation.bin_id;
    // The repairs first, as the plan made them against the library before
    // anything was filed or added.
    std::set<Ulid> repaired;
    for (const LibraryMedia& completed : operation.completed) {
        LibraryMedia* media = candidate.FindLibraryMedia(completed.id);
        if (media == nullptr || named.count(completed.id) != 0 ||
            !repaired.insert(completed.id).second) {
            Fail(media == nullptr ? EditError::UnknownMedia
                                  : EditError::DuplicateId,
                 (media == nullptr ? "unknown media_id '"
                                   : "media_id completed twice or also "
                                     "added in an ingest: '") +
                     completed.id + "'",
                 error, message);
            return false;
        }
        undo.restored.push_back(*media);
        *media = completed;
    }
    std::set<Ulid> mounted;
    for (const Ulid& id : operation.mounted) {
        const LibraryMedia* media = candidate.FindLibraryMedia(id);
        if (media == nullptr || named.count(id) != 0 ||
            !mounted.insert(id).second) {
            Fail(media == nullptr ? EditError::UnknownMedia
                                  : EditError::DuplicateId,
                 (media == nullptr ? "unknown media_id '"
                                   : "media_id mounted twice or also added "
                                     "in an ingest: '") +
                     id + "'",
                 error, message);
            return false;
        }
        if (candidate.FindSource(id) != nullptr) {
            Fail(EditError::InvalidOperation,
                 "media '" + id + "' is already mounted as a source", error,
                 message);
            return false;
        }
        candidate.sources.push_back(IngestedSourceOf(*media));
        undo.unmounted.push_back(id);
    }
    for (const Ulid& id : operation.filed) {
        LibraryMedia* media = candidate.FindLibraryMedia(id);
        if (media == nullptr || !named.insert(id).second) {
            Fail(media == nullptr ? EditError::UnknownMedia
                                  : EditError::DuplicateId,
                 (media == nullptr ? "unknown media_id '"
                                   : "media_id named twice in an ingest: '") +
                     id + "'",
                 error, message);
            return false;
        }
        undo.refiled.push_back({id, media->bin_id});
        media->bin_id = operation.bin_id;
    }
    for (const LibraryMedia& media : operation.added) {
        candidate.library.push_back(media);
        candidate.sources.push_back(IngestedSourceOf(media));
        undo.media_ids.push_back(media.id);
    }
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = std::move(undo);
    return true;
}

bool ApplyRemoveIngestedMedia(Document& candidate,
                              RemoveIngestedMediaOperation& operation,
                              Operation& inverse, EditError& error,
                              std::string& message) {
    const size_t count = operation.media_ids.size();
    const size_t unmountedCount = operation.unmounted.size();
    if (count == 0 && operation.refiled.empty() && unmountedCount == 0 &&
        operation.restored.empty()) {
        Fail(EditError::InvalidOperation,
             "removing an ingest takes back at least one media", error,
             message);
        return false;
    }
    if (count > candidate.library.size() ||
        count + unmountedCount > candidate.sources.size()) {
        Fail(EditError::InvalidOperation,
             "the library no longer ends with the ingested media", error,
             message);
        return false;
    }
    const size_t libraryStart = candidate.library.size() - count;
    const size_t sourceStart = candidate.sources.size() - count;
    const size_t unmountStart = sourceStart - unmountedCount;
    for (size_t index = 0; index < unmountedCount; ++index) {
        const Ulid& id = operation.unmounted[index];
        const LibraryMedia* media = candidate.FindLibraryMedia(id);
        if (media == nullptr ||
            candidate.sources[unmountStart + index].id != id ||
            !SameSource(candidate.sources[unmountStart + index],
                        IngestedSourceOf(*media))) {
            Fail(EditError::InvalidOperation,
                 "the source of media '" + id +
                     "' is no longer where its ingest mounted it: undo what "
                     "came after first",
                 error, message);
            return false;
        }
    }
    std::set<Ulid> removed;
    for (size_t index = 0; index < count; ++index) {
        const Ulid& id = operation.media_ids[index];
        const LibraryMedia& media = candidate.library[libraryStart + index];
        if (media.id != id ||
            !SameSource(candidate.sources[sourceStart + index],
                        IngestedSourceOf(media))) {
            Fail(EditError::InvalidOperation,
                 "media '" + id +
                     "' is no longer where its ingest put it, last in the "
                     "library and in the sources: undo what came after "
                     "first",
                 error, message);
            return false;
        }
        removed.insert(id);
    }
    std::set<Ulid> unsourced = removed;
    unsourced.insert(operation.unmounted.begin(), operation.unmounted.end());
    for (const DocumentTrack& track : candidate.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (unsourced.count(clip.source_id) != 0) {
                Fail(EditError::InvalidOperation,
                     "media '" + clip.source_id + "' is used by clip '" +
                         clip.id + "'",
                     error, message);
                return false;
            }
        }
    }
    IngestMediaOperation redo;
    redo.bin_id = operation.bin_id;
    for (const MediaBinAssignment& assignment : operation.refiled) {
        LibraryMedia* media = candidate.FindLibraryMedia(assignment.media_id);
        if (media == nullptr || removed.count(assignment.media_id) != 0) {
            Fail(EditError::UnknownMedia,
                 "unknown media_id '" + assignment.media_id + "'", error,
                 message);
            return false;
        }
        if (media->bin_id != operation.bin_id) {
            Fail(EditError::InvalidOperation,
                 "media '" + assignment.media_id +
                     "' is no longer in the bin its ingest filed it in",
                 error, message);
            return false;
        }
        if (!assignment.bin_id.empty() &&
            !candidate.FindBin(assignment.bin_id)) {
            Fail(EditError::UnknownBin,
                 "unknown bin_id '" + assignment.bin_id + "'", error, message);
            return false;
        }
        media->bin_id = assignment.bin_id;
        redo.filed.push_back(assignment.media_id);
    }
    redo.added.assign(candidate.library.begin() + libraryStart,
                      candidate.library.end());
    candidate.library.resize(libraryStart);
    candidate.sources.resize(unmountStart);
    redo.mounted = operation.unmounted;
    // Restored last, once refiled: the entry the redo completes again is the
    // one the forward ingest found, in the bin it found it in.
    for (const LibraryMedia& restored : operation.restored) {
        LibraryMedia* media = candidate.FindLibraryMedia(restored.id);
        if (media == nullptr) {
            Fail(EditError::UnknownMedia,
                 "unknown media_id '" + restored.id + "'", error, message);
            return false;
        }
        redo.completed.push_back(*media);
        *media = restored;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = std::move(redo);
    return true;
}

bool ApplyAddMarker(Document& candidate, AddMarkerOperation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    if (operation.marker.id.empty()) operation.marker.id = GenerateUlid();
    const Ulid& id = operation.marker.id;
    const bool collision = id == candidate.sequence.id ||
                           candidate.FindMarker(id) || candidate.FindBin(id) ||
                           candidate.FindLibraryMedia(id) ||
                           candidate.FindSource(id) ||
                           candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "marker_id is invalid or already exists: '" + id + "'", error,
             message);
        return false;
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.markers.size())) {
        Fail(EditError::InvalidOperation,
             "marker insertion_index is outside the marker list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.markers.size());
    candidate.sequence.markers.insert(
        candidate.sequence.markers.begin() + operation.insertion_index,
        operation.marker);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveMarkerOperation{id};
    return true;
}

bool ApplyRemoveMarker(Document& candidate, RemoveMarkerOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    const auto found = std::find_if(candidate.sequence.markers.begin(),
                                    candidate.sequence.markers.end(),
                                    [&](const DocumentMarker& marker) {
                                        return marker.id == operation.marker_id;
                                    });
    if (found == candidate.sequence.markers.end()) {
        Fail(EditError::UnknownMarker,
             "unknown marker_id '" + operation.marker_id + "'", error, message);
        return false;
    }
    const int64_t index = static_cast<int64_t>(
        std::distance(candidate.sequence.markers.begin(), found));
    const DocumentMarker removed = *found;
    candidate.sequence.markers.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddMarkerOperation{removed, index};
    return true;
}

bool ApplyUpdateMarker(Document& candidate, UpdateMarkerOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    DocumentMarker* marker = candidate.FindMarker(operation.marker_id);
    if (!marker) {
        Fail(EditError::UnknownMarker,
             "unknown marker_id '" + operation.marker_id + "'", error, message);
        return false;
    }
    const DocumentMarker before = *marker;
    marker->name = operation.name;
    marker->time = operation.time;
    marker->color = operation.color;
    marker->category = operation.category;
    marker->duration = operation.duration;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse =
        UpdateMarkerOperation{before.id,    before.name,     before.time,
                              before.color, before.category, before.duration};
    return true;
}

bool ApplyAddCaptionStyle(Document& candidate,
                          AddCaptionStyleOperation& operation,
                          Operation& inverse, EditError& error,
                          std::string& message) {
    if (operation.style.id.empty()) operation.style.id = GenerateUlid();
    const Ulid& id = operation.style.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindCaptionStyle(id) ||
        candidate.FindMulticamGroup(id) || candidate.FindMarker(id) ||
        candidate.FindTransition(id) || candidate.FindBin(id) ||
        candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
        candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "caption style id is invalid or already exists: '" + id + "'",
             error, message);
        return false;
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.caption_styles.size())) {
        Fail(EditError::InvalidOperation,
             "caption style insertion_index is outside the style list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.caption_styles.size());
    candidate.sequence.caption_styles.insert(
        candidate.sequence.caption_styles.begin() + operation.insertion_index,
        operation.style);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveCaptionStyleOperation{id};
    return true;
}

bool ApplyRemoveCaptionStyle(Document& candidate,
                             RemoveCaptionStyleOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    auto& styles = candidate.sequence.caption_styles;
    const auto found = std::find_if(styles.begin(), styles.end(),
                                    [&](const CaptionStyle& style) {
                                        return style.id == operation.style_id;
                                    });
    if (found == styles.end()) {
        Fail(EditError::UnknownCaptionStyle,
             "unknown caption style_id '" + operation.style_id + "'", error,
             message);
        return false;
    }
    const bool inUse = std::any_of(
        candidate.sequence.tracks.begin(), candidate.sequence.tracks.end(),
        [&](const DocumentTrack& track) {
            return std::any_of(track.clips.begin(), track.clips.end(),
                               [&](const DocumentClip& clip) {
                                   return clip.caption_group_id ==
                                          operation.style_id;
                               });
        });
    if (inUse) {
        Fail(EditError::InvalidOperation,
             "caption style_id '" + operation.style_id +
                 "' is still referenced by a clip",
             error, message);
        return false;
    }
    const int64_t index =
        static_cast<int64_t>(std::distance(styles.begin(), found));
    const CaptionStyle removed = *found;
    styles.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddCaptionStyleOperation{removed, index};
    return true;
}

bool ApplyUpdateCaptionStyle(Document& candidate,
                             UpdateCaptionStyleOperation& operation,
                             Operation& inverse, EditError& error,
                             std::string& message) {
    auto& styles = candidate.sequence.caption_styles;
    const auto found = std::find_if(styles.begin(), styles.end(),
                                    [&](const CaptionStyle& style) {
                                        return style.id == operation.style.id;
                                    });
    if (found == styles.end()) {
        Fail(EditError::UnknownCaptionStyle,
             "unknown caption style_id '" + operation.style.id + "'", error,
             message);
        return false;
    }
    const CaptionStyle before = *found;
    *found = operation.style;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = UpdateCaptionStyleOperation{before};
    return true;
}

bool ApplySetClipCaption(Document& candidate,
                         SetClipCaptionOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const Ulid beforeGroup = clip->caption_group_id;
    const std::string beforeText = clip->caption_text;
    const std::vector<RationalTime> beforeTimes = clip->caption_word_times;
    // PA4 -- a caller that says nothing of the words keeps their times
    // when the text still counts as many: a carton restyled or corrected
    // goes on following the voice. Decided once, here, and written into
    // the operation.
    if (!operation.word_times_resolved) {
        if (operation.caption_word_times.empty() &&
            !operation.caption_group_id.empty() &&
            beforeTimes.size() == CaptionWordCount(operation.caption_text))
            operation.caption_word_times = beforeTimes;
        operation.word_times_resolved = true;
    }
    clip->caption_group_id = operation.caption_group_id;
    clip->caption_text = operation.caption_text;
    clip->caption_word_times = operation.caption_word_times;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipCaptionOperation{clip->id, beforeGroup, beforeText,
                                      beforeTimes, true};
    return true;
}

// F1.5 ("Multicam"). Registers a new multicam group and its angles. Angle
// clip existence, video-track membership, no-clip-reused-within-group and
// active_angle_id membership are all schema-level invariants already
// enforced by Document::Validate (see MulticamGroup validation), so this
// only owns the operation's own precondition: a fresh, collision-free group
// id and a well-formed insertion_index, mirroring
// ApplyAddTransition/ApplyAddCaptionStyle.
bool ApplyAddMulticamGroup(Document& candidate,
                           AddMulticamGroupOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    if (operation.group.id.empty()) operation.group.id = GenerateUlid();
    const Ulid& id = operation.group.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindMulticamGroup(id) ||
        candidate.FindCaptionStyle(id) || candidate.FindMarker(id) ||
        candidate.FindTransition(id) || candidate.FindBin(id) ||
        candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
        candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "multicam group id is invalid or already exists: '" + id + "'",
             error, message);
        return false;
    }
    for (MulticamAngle& angle : operation.group.angles) {
        if (angle.id.empty()) angle.id = GenerateUlid();
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.multicam_groups.size())) {
        Fail(EditError::InvalidOperation,
             "multicam group insertion_index is outside the group list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.multicam_groups.size());
    candidate.sequence.multicam_groups.insert(
        candidate.sequence.multicam_groups.begin() + operation.insertion_index,
        operation.group);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveMulticamGroupOperation{id};
    return true;
}

// Exact inverse of ApplyAddMulticamGroup. No other schema field references a
// multicam group by id, so unlike caption styles there is no "still in use"
// precondition to check here.
bool ApplyRemoveMulticamGroup(Document& candidate,
                              RemoveMulticamGroupOperation& operation,
                              Operation& inverse, EditError& error,
                              std::string& message) {
    auto& groups = candidate.sequence.multicam_groups;
    const auto found = std::find_if(
        groups.begin(), groups.end(),
        [&](const auto& group) { return group.id == operation.group_id; });
    if (found == groups.end()) {
        Fail(EditError::UnknownMulticamGroup,
             "unknown multicam group_id '" + operation.group_id + "'", error,
             message);
        return false;
    }
    const int64_t index =
        static_cast<int64_t>(std::distance(groups.begin(), found));
    const MulticamGroup removed = *found;
    groups.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddMulticamGroupOperation{removed, index};
    return true;
}

// Changes the active angle of an existing group. The group itself must
// exist (EditError::UnknownMulticamGroup); a non-empty active_angle_id must
// name one of that group's own angles (EditError::UnknownMulticamAngle) —
// Document::Validate would also catch a dangling active_angle_id, but only
// as a generic ValidationFailed, so this checks it directly for an exact
// error the way UpdateMarker/SetClipLink check their own references.
bool ApplySetMulticamActiveAngle(Document& candidate,
                                 SetMulticamActiveAngleOperation& operation,
                                 Operation& inverse, EditError& error,
                                 std::string& message) {
    MulticamGroup* group = candidate.FindMulticamGroup(operation.group_id);
    if (!group) {
        Fail(EditError::UnknownMulticamGroup,
             "unknown multicam group_id '" + operation.group_id + "'", error,
             message);
        return false;
    }
    if (!operation.active_angle_id.empty() &&
        std::none_of(group->angles.begin(), group->angles.end(),
                     [&](const MulticamAngle& angle) {
                         return angle.id == operation.active_angle_id;
                     })) {
        Fail(EditError::UnknownMulticamAngle,
             "active_angle_id '" + operation.active_angle_id +
                 "' is not an angle of multicam group '" + operation.group_id +
                 "'",
             error, message);
        return false;
    }
    const Ulid before = group->active_angle_id;
    group->active_angle_id = operation.active_angle_id;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetMulticamActiveAngleOperation{group->id, before};
    return true;
}

bool ApplyAddTransition(Document& candidate, AddTransitionOperation& operation,
                        Operation& inverse, EditError& error,
                        std::string& message) {
    if (operation.transition.id.empty())
        operation.transition.id = GenerateUlid();
    const Ulid& id = operation.transition.id;
    const bool collision =
        id == candidate.sequence.id || candidate.FindTransition(id) ||
        candidate.FindMarker(id) || candidate.FindBin(id) ||
        candidate.FindLibraryMedia(id) || candidate.FindSource(id) ||
        candidate.FindTrack(id) || candidate.FindClip(id);
    if (!IsValidUlid(id) || collision) {
        Fail(EditError::DuplicateId,
             "transition_id is invalid or already exists: '" + id + "'", error,
             message);
        return false;
    }
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) >
             candidate.sequence.transitions.size())) {
        Fail(EditError::InvalidOperation,
             "transition insertion_index is outside the transition list", error,
             message);
        return false;
    }
    if (operation.insertion_index < 0)
        operation.insertion_index =
            static_cast<int64_t>(candidate.sequence.transitions.size());
    candidate.sequence.transitions.insert(
        candidate.sequence.transitions.begin() + operation.insertion_index,
        operation.transition);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = RemoveTransitionOperation{id};
    return true;
}

bool ApplyRemoveTransition(Document& candidate,
                           RemoveTransitionOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    const auto found = std::find_if(
        candidate.sequence.transitions.begin(),
        candidate.sequence.transitions.end(), [&](const auto& transition) {
            return transition.id == operation.transition_id;
        });
    if (found == candidate.sequence.transitions.end()) {
        Fail(EditError::UnknownTransition,
             "unknown transition_id '" + operation.transition_id + "'", error,
             message);
        return false;
    }
    const int64_t index = static_cast<int64_t>(
        std::distance(candidate.sequence.transitions.begin(), found));
    const DocumentTransition removed = *found;
    candidate.sequence.transitions.erase(found);
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = AddTransitionOperation{removed, index};
    return true;
}

bool ApplyUpdateTransition(Document& candidate,
                           UpdateTransitionOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    DocumentTransition* transition =
        candidate.FindTransition(operation.transition_id);
    if (!transition) {
        Fail(EditError::UnknownTransition,
             "unknown transition_id '" + operation.transition_id + "'", error,
             message);
        return false;
    }
    const DocumentTransition before = *transition;
    transition->type = operation.type;
    transition->duration = operation.duration;
    transition->alignment = operation.alignment;
    transition->color = operation.color;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = UpdateTransitionOperation{before.id, before.type, before.duration,
                                        before.alignment, before.color};
    return true;
}

bool ApplySetClipLink(Document& candidate, SetClipLinkOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentClip* first = candidate.FindClip(operation.first_clip_id);
    DocumentClip* second = candidate.FindClip(operation.second_clip_id);
    if (!first || !second || first == second) {
        Fail(EditError::UnknownClip,
             "SetClipLink requires two distinct existing clips", error,
             message);
        return false;
    }
    if ((!operation.first_group_id.empty() &&
         !IsValidUlid(operation.first_group_id)) ||
        (!operation.second_group_id.empty() &&
         !IsValidUlid(operation.second_group_id)) ||
        (operation.exact_first_result.has_value() !=
         operation.exact_second_result.has_value())) {
        Fail(EditError::InvalidOperation,
             "link group IDs must be empty or valid ULIDs", error, message);
        return false;
    }
    const SetClipLinkOperation::ExactState oldFirst{
        first->link_group_id, first->sync_anchor_clip_id,
        first->sync_reference_delta};
    const SetClipLinkOperation::ExactState oldSecond{
        second->link_group_id, second->sync_anchor_clip_id,
        second->sync_reference_delta};
    inverse =
        SetClipLinkOperation{first->id,          second->id, oldFirst.group_id,
                             oldSecond.group_id, oldFirst,   oldSecond};
    if (operation.exact_first_result) {
        const auto apply = [](DocumentClip& clip,
                              const SetClipLinkOperation::ExactState& state) {
            clip.link_group_id = state.group_id;
            clip.sync_anchor_clip_id = state.anchor_clip_id;
            clip.sync_reference_delta = state.reference_delta;
        };
        apply(*first, *operation.exact_first_result);
        apply(*second, *operation.exact_second_result);
    } else if (operation.first_group_id.empty() &&
               operation.second_group_id.empty()) {
        first->link_group_id.clear();
        first->sync_anchor_clip_id.clear();
        first->sync_reference_delta = {0, 1};
        second->link_group_id.clear();
        second->sync_anchor_clip_id.clear();
        second->sync_reference_delta = {0, 1};
    } else {
        if (operation.first_group_id != operation.second_group_id) {
            Fail(EditError::InvalidOperation,
                 "newly linked clips must share one group ID", error, message);
            return false;
        }
        // FC8b -- R4 and criterion 4 refuse a freeze or a ramp on a linked
        // clip (ApplySetClipSpeed): a sound cannot hold a picture or follow
        // a curve. Linking one afterwards reached the same state by the
        // other door, and the speed edit that followed could no longer be
        // undone.
        for (const DocumentClip* clip : {first, second}) {
            if (ClipIsFreeze(*clip) || !clip->speed_ramp.empty()) {
                Fail(EditError::InvalidOperation,
                     "clip '" + clip->id + "' is " +
                         (ClipIsFreeze(*clip) ? "a freeze (speed 0/1)"
                                              : "on a speed ramp") +
                         ": a linked sound cannot follow it; give it a "
                         "constant speed first",
                     error, message);
                return false;
            }
        }
        const DocumentTrack* firstTrack = candidate.FindTrackForClip(first->id);
        DocumentClip* anchor =
            firstTrack && firstTrack->kind == "video" ? first : second;
        DocumentClip* member = anchor == first ? second : first;
        anchor->link_group_id = operation.first_group_id;
        anchor->sync_anchor_clip_id = anchor->id;
        anchor->sync_reference_delta = {0, 1};
        member->link_group_id = operation.first_group_id;
        member->sync_anchor_clip_id = anchor->id;
        member->sync_reference_delta = PhaseOf(*member).sub(PhaseOf(*anchor));
    }
    operation.exact_first_result = SetClipLinkOperation::ExactState{
        first->link_group_id, first->sync_anchor_clip_id,
        first->sync_reference_delta};
    operation.exact_second_result = SetClipLinkOperation::ExactState{
        second->link_group_id, second->sync_anchor_clip_id,
        second->sync_reference_delta};
    return ValidateResult(candidate, error, message);
}

bool ApplySetClipEffects(Document& candidate,
                         SetClipEffectsOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const std::vector<ClipEffect> before = clip->effects;
    for (ClipEffect& effect : operation.effects) {
        if (effect.id.empty()) effect.id = GenerateUlid();
    }
    clip->effects = operation.effects;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipEffectsOperation{clip->id, before};
    return true;
}

bool ApplySetClipOpacity(Document& candidate,
                         SetClipOpacityOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    const DocumentTrack* track =
        clip ? candidate.FindTrackForClip(operation.clip_id) : nullptr;
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (!track || track->kind != "video") {
        Fail(EditError::InvalidOperation,
             "clip opacity is only supported on video tracks", error, message);
        return false;
    }
    const EffectParamValue before = clip->opacity;
    clip->opacity = operation.opacity;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipOpacityOperation{clip->id, before};
    return true;
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. A clip's blur. The range, the
// track and the framing it may sit with are Document::Validate's, checked
// once for every way a clip gets there; what is refused here is said in
// the terms of this operation.
// PA8 -- docs/roadmap/PARITE-2026-11.md.
bool ApplySetClipMask(Document& candidate, SetClipMaskOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    const DocumentTrack* track =
        clip ? candidate.FindTrackForClip(operation.clip_id) : nullptr;
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (!track || track->kind != "video") {
        Fail(EditError::InvalidOperation,
             "a clip mask is only supported on video tracks", error, message);
        return false;
    }
    if (IsMotionClip(*clip)) {
        Fail(EditError::InvalidOperation,
             "a motion clip takes no clip mask: its composition draws its "
             "own shapes (PA8)",
             error, message);
        return false;
    }
    // Reduced before it is stored, and a mask without a shape brought back
    // to the default the file omits: the operation keeps what the document
    // holds, so the journal replays its bytes.
    operation.mask = ReduceClipMask(operation.mask);
    if (!operation.mask.shape.empty()) {
        std::string maskError;
        if (!ClipMaskIsValid(operation.mask, maskError)) {
            Fail(EditError::ValidationFailed,
                 "clip '" + operation.clip_id + "' " + maskError, error,
                 message);
            return false;
        }
    }
    const ClipMask before = clip->mask;
    clip->mask = operation.mask;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipMaskOperation{clip->id, before};
    return true;
}

bool ApplySetClipBlur(Document& candidate, SetClipBlurOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    const DocumentTrack* track =
        clip ? candidate.FindTrackForClip(operation.clip_id) : nullptr;
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (!track || track->kind != "video") {
        Fail(EditError::InvalidOperation,
             "a clip blur is only supported on video tracks", error, message);
        return false;
    }
    if (IsMotionClip(*clip)) {
        Fail(EditError::InvalidOperation,
             "a motion clip takes no clip blur: its composition blurs its own "
             "nodes (EF3)",
             error, message);
        return false;
    }
    if (operation.blur.den <= 0) {
        Fail(EditError::InvalidOperation,
             "a clip blur needs a positive denominator", error, message);
        return false;
    }
    // Reduced before it is stored, for the reason a speed is: 20/2 and 10/1
    // are the same blur and must be the same bytes, and 0/5 must be the 0/1
    // the file omits rather than a key an older binary would drop. The
    // operation keeps the reduced value, so the journal replays the bytes
    // the document holds.
    const int32_t common = std::gcd(operation.blur.num, operation.blur.den);
    if (common > 1) {
        operation.blur.num /= common;
        operation.blur.den /= common;
    }
    const EffectParamValue before = clip->blur;
    clip->blur = operation.blur;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipBlurOperation{clip->id, before};
    return true;
}

// R3 -- docs/roadmap/REEL-2026-09.md. One speed for the whole link group,
// or none: criterion 2 of the ticket forbids a picture and a sound that
// drift apart with nothing saying so, and a group whose members carried
// different speeds would be exactly that. The source range and the
// source_in alignment every member then needs are checked by
// Document::Validate, once, for all of them.
bool ApplySetClipSpeed(Document& candidate, SetClipSpeedOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    const DocumentTrack* track =
        clip ? candidate.FindTrackForClip(operation.clip_id) : nullptr;
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    std::string speedError;
    if (!ClipSpeedIsValid(operation.speed, speedError) ||
        !ClipSpeedRampIsValid(operation.ramp, speedError)) {
        Fail(EditError::InvalidOperation, speedError, error, message);
        return false;
    }
    if (!operation.ramp.empty() && !IsUnitSpeed(operation.speed)) {
        Fail(EditError::InvalidOperation,
             "set_clip_speed takes a constant speed or a ramp, not both: a "
             "ramped clip's speed is its keys",
             error, message);
        return false;
    }
    if (!track || track->kind == "caption") {
        Fail(EditError::InvalidOperation,
             "clip speed is only supported on video and audio tracks", error,
             message);
        return false;
    }
    if (IsMotionClip(*clip)) {
        Fail(EditError::InvalidOperation,
             "clip speed on a motion clip is refused: a composition is "
             "rendered at the sequence cadence, so its speed belongs to the "
             "composition (R3)",
             error, message);
        return false;
    }
    // Reduced before it is stored: 50/100 and 1/2 are the same slow motion
    // and must be the same bytes, and a speed spelled 2/2 must be the 1/1
    // the file omits rather than a key an older binary would drop.
    EffectParamValue speed = operation.speed;
    for (int32_t left = speed.num, right = speed.den; right != 0;) {
        const int32_t remainder = left % right;
        left = right;
        right = remainder;
        if (right == 0 && left > 1) {
            speed.num /= left;
            speed.den /= left;
        }
    }
    // Criterion 4 -- the keys reduced the same way, for the same reason.
    std::vector<ClipKeyframe> ramp = operation.ramp;
    for (ClipKeyframe& key : ramp) {
        const int32_t common = std::gcd(key.value.num, key.value.den);
        if (common > 1) {
            key.value.num /= common;
            key.value.den /= common;
        }
    }
    if (!ramp.empty()) {
        speed = {1, 1};
    }
    // Criterion 4 -- a ramp changes the speed frame by frame, and `atempo`
    // stretches a sound by one factor (ClipSpeed.h): picture only, like a
    // freeze.
    if (!ramp.empty() &&
        (track->kind != "video" || !clip->link_group_id.empty())) {
        Fail(EditError::InvalidOperation,
             track->kind != "video"
                 ? "a speed ramp is refused on a sound clip: atempo stretches "
                   "a sound by one factor, not by a curve"
                 : "a speed ramp is refused on a linked clip: its sound "
                   "cannot follow a speed that changes every frame; detach "
                   "the sound first (detach_audio)",
             error, message);
        return false;
    }
    // R4 -- a freeze holds a picture, and a sound has nothing to hold.
    if (IsFreezeSpeed(operation.speed) &&
        (track->kind != "video" || !clip->link_group_id.empty())) {
        Fail(EditError::InvalidOperation,
             track->kind != "video"
                 ? "a freeze (speed 0/1) holds a picture: it is refused on a "
                   "sound clip"
                 : "a freeze (speed 0/1) holds a picture and its linked sound "
                   "would have nothing to hold: unlink the clip first, or "
                   "use freeze_frame for a new one",
             error, message);
        return false;
    }
    // FC8b -- a freeze already linked (set_clip_link refuses it now, but a
    // project linked before, or a journal's exact state, can hold one) does
    // not leave its freeze by this door: the inverse would put a freeze back
    // on a linked clip, which the check above refuses, and the step could not
    // be undone. Unlinking first keeps every step reversible, and leaves
    // Document::Validate, and so the projects that already hold such a link,
    // as they are.
    if (ClipIsFreeze(*clip) && !clip->link_group_id.empty()) {
        Fail(EditError::InvalidOperation,
             "clip '" + clip->id +
                 "' is a freeze (speed 0/1) linked to a sound, which a link "
                 "cannot carry: unlink it first (set_clip_link with empty "
                 "groups), then set its speed",
             error, message);
        return false;
    }
    const EffectParamValue before = clip->speed;
    const std::vector<ClipKeyframe> rampBefore = clip->speed_ramp;
    const Ulid group = clip->link_group_id;
    for (DocumentTrack& candidateTrack : candidate.sequence.tracks) {
        for (DocumentClip& member : candidateTrack.clips) {
            const bool sameGroup =
                !group.empty() && member.link_group_id == group;
            if (member.id != clip->id && !sameGroup) continue;
            if (member.speed.num != before.num ||
                member.speed.den != before.den) {
                Fail(EditError::InvalidOperation,
                     "link group '" + group +
                         "' does not carry one speed; set_clip_speed refuses "
                         "to change a group whose members already differ (R3)",
                     error, message);
                return false;
            }
            member.speed = speed;
            // A ramp is never on a linked clip (Document::Validate), so
            // only a clip alone ever gains or loses one here.
            member.speed_ramp = ramp;
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipSpeedOperation{operation.clip_id, before, rampBefore};
    return true;
}

// R1 -- docs/roadmap/REEL-2026-09.md. One framing, one clip, any number of
// clips at once: the vertical declination of a whole film is one reversible
// step rather than thirty. The rectangle it implies is never computed here
// -- ClipFraming.h owns that, once, for every surface -- but the framing is
// resolved against each clip's own media so that a framing which leaves
// nothing on screen is refused when it is set, not when it is delivered.
bool ApplySetClipFraming(Document& candidate,
                         SetClipFramingOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    if (operation.clips.empty()) {
        Fail(EditError::InvalidOperation,
             "set_clip_framing needs at least one clip", error, message);
        return false;
    }
    std::vector<ClipFramingAssignment> before;
    std::set<Ulid> seen;
    for (const ClipFramingAssignment& assignment : operation.clips) {
        if (!seen.insert(assignment.clip_id).second) {
            Fail(EditError::InvalidOperation,
                 "set_clip_framing names clip '" + assignment.clip_id +
                     "' twice",
                 error, message);
            return false;
        }
        DocumentClip* clip = candidate.FindClip(assignment.clip_id);
        if (clip == nullptr) {
            Fail(EditError::UnknownClip,
                 "unknown clip_id '" + assignment.clip_id + "'", error,
                 message);
            return false;
        }
        const DocumentTrack* track =
            candidate.FindTrackForClip(assignment.clip_id);
        if (track == nullptr || track->kind != "video") {
            Fail(EditError::InvalidOperation,
                 "clip framing is only supported on video tracks", error,
                 message);
            return false;
        }
        if (IsMotionClip(*clip)) {
            Fail(EditError::InvalidOperation,
                 "clip framing on a motion clip is refused: a composition is "
                 "laid out at the sequence size already, so its framing "
                 "belongs to the composition (R1)",
                 error, message);
            return false;
        }
        if (!clip->caption_group_id.empty()) {
            Fail(EditError::InvalidOperation,
                 "clip framing on a caption clip is refused: a caption is "
                 "drawn in the sequence frame, not scaled into it (R1)",
                 error, message);
            return false;
        }
        std::string framingError;
        if (!ClipFramingIsValid(assignment.framing, framingError)) {
            Fail(EditError::InvalidOperation, framingError, error, message);
            return false;
        }
        before.push_back({clip->id, clip->framing});
        clip->framing = ReduceClipFraming(assignment.framing);
        ClipFramingGeometry geometry;
        if (ClipHasFraming(*clip) &&
            !ResolveClipFramingForClip(
                candidate, *clip, candidate.sequence.width,
                candidate.sequence.height, geometry, framingError)) {
            Fail(EditError::InvalidOperation,
                 "clip '" + clip->id + "': " + framingError, error, message);
            return false;
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipFramingOperation{before};
    return true;
}

// R2 -- docs/roadmap/REEL-2026-09.md. The whole animation of a clip, in one
// reversible step, on one clip or on several. The inverse carries each
// clip's previous animation, so adding a key, moving one and deleting one
// all undo byte for byte -- including deleting the last one, which is an
// empty list here and the absent `animation` key in the file.
//
// What is checked, and where. The shape of the track is ClipAnimation.h's
// business; the rectangle each frame implies is R1's, resolved by
// Document::Validate frame by frame so that an animation which pushes the
// picture off the screen is refused when it is set and not discovered in
// the delivery (guard rail 4 -- the engine measures).
bool ApplySetClipAnimation(Document& candidate,
                           SetClipAnimationOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    if (operation.clips.empty()) {
        Fail(EditError::InvalidOperation,
             "set_clip_animation needs at least one clip", error, message);
        return false;
    }
    std::vector<ClipAnimationAssignment> before;
    std::set<Ulid> seen;
    for (const ClipAnimationAssignment& assignment : operation.clips) {
        if (!seen.insert(assignment.clip_id).second) {
            Fail(EditError::InvalidOperation,
                 "set_clip_animation names clip '" + assignment.clip_id +
                     "' twice",
                 error, message);
            return false;
        }
        DocumentClip* clip = candidate.FindClip(assignment.clip_id);
        if (clip == nullptr) {
            Fail(EditError::UnknownClip,
                 "unknown clip_id '" + assignment.clip_id + "'", error,
                 message);
            return false;
        }
        const DocumentTrack* track =
            candidate.FindTrackForClip(assignment.clip_id);
        if (track == nullptr || track->kind != "video") {
            Fail(EditError::InvalidOperation,
                 "an animated clip parameter is only supported on video "
                 "tracks: the audio side waits for R0 to ask for it (R2)",
                 error, message);
            return false;
        }
        std::string animationError;
        if (!ClipAnimationIsValid(assignment.animation, animationError)) {
            Fail(EditError::InvalidOperation, animationError, error, message);
            return false;
        }
        const bool framed = std::any_of(
            assignment.animation.begin(), assignment.animation.end(),
            [](const ClipParameterAnimation& entry) {
                return entry.parameter != ClipAnimatedParameter::Opacity;
            });
        if (framed && IsMotionClip(*clip)) {
            Fail(EditError::InvalidOperation,
                 "an animated framing on a motion clip is refused: a "
                 "composition is laid out at the sequence size already, so "
                 "its movement belongs to the composition (R2)",
                 error, message);
            return false;
        }
        if (framed && !clip->caption_group_id.empty()) {
            Fail(EditError::InvalidOperation,
                 "an animated framing on a caption clip is refused: a "
                 "caption is drawn in the sequence frame, not scaled into "
                 "it (R2)",
                 error, message);
            return false;
        }
        before.push_back({clip->id, clip->animation});
        clip->animation = assignment.animation;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipAnimationOperation{before};
    return true;
}

// Out of lot -- the author, 29 October 2026 (Resolve's Enable/Disable Clip, D).
// Any clip of any track: a shot, its sound, a motion clip or a carton. The
// state is all the operation touches, so it needs no check of its own past
// naming each clip once; the inverse is each clip's previous state.
bool ApplySetClipEnabled(Document& candidate,
                         SetClipEnabledOperation& operation, Operation& inverse,
                         EditError& error, std::string& message) {
    if (operation.clips.empty()) {
        Fail(EditError::InvalidOperation,
             "set_clip_enabled needs at least one clip", error, message);
        return false;
    }
    std::vector<ClipEnabledAssignment> before;
    std::set<Ulid> seen;
    for (const ClipEnabledAssignment& assignment : operation.clips) {
        if (!seen.insert(assignment.clip_id).second) {
            Fail(EditError::InvalidOperation,
                 "set_clip_enabled names clip '" + assignment.clip_id +
                     "' twice",
                 error, message);
            return false;
        }
        DocumentClip* clip = candidate.FindClip(assignment.clip_id);
        if (clip == nullptr) {
            Fail(EditError::UnknownClip,
                 "unknown clip_id '" + assignment.clip_id + "'", error,
                 message);
            return false;
        }
        before.push_back({clip->id, clip->enabled});
        clip->enabled = assignment.enabled;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SetClipEnabledOperation{before};
    return true;
}

bool ApplySetClipAudio(Document& candidate, SetClipAudioOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    DocumentClip* clip = candidate.FindClip(operation.clip_id);
    const DocumentTrack* track =
        clip ? candidate.FindTrackForClip(operation.clip_id) : nullptr;
    if (!clip) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (!track || track->kind != "audio") {
        Fail(EditError::InvalidOperation,
             "clip audio settings are only supported on audio tracks", error,
             message);
        return false;
    }
    const SetClipAudioOperation before{clip->id, clip->audio_gain_db,
                                       clip->audio_fade_in,
                                       clip->audio_fade_out};
    clip->audio_gain_db = operation.gain_db;
    clip->audio_fade_in = operation.fade_in;
    clip->audio_fade_out = operation.fade_out;
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = before;
    return true;
}

bool ApplySplit(Document& candidate, SplitClipOperation& operation,
                Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const DocumentClip& clip) { return clip.id == operation.clip_id; });
    const size_t index =
        static_cast<size_t>(std::distance(track->clips.begin(), found));
    const DocumentClip original = *found;
    const RationalTime end = original.timeline_in.add(original.duration);
    if (operation.timeline_position.rate <= 0 ||
        operation.timeline_position <= original.timeline_in ||
        operation.timeline_position >= end) {
        Fail(EditError::InvalidOperation,
             "split_clip timeline_position must be strictly within (" +
                 DescribeTime(original.timeline_in) + ", " + DescribeTime(end) +
                 "); got " + DescribeTime(operation.timeline_position),
             error, message);
        return false;
    }
    if (operation.right_clip_id.empty())
        operation.right_clip_id = GenerateUlid();
    if (!IsValidUlid(operation.right_clip_id) ||
        candidate.FindClip(operation.right_clip_id) ||
        candidate.FindSource(operation.right_clip_id) ||
        candidate.FindTrack(operation.right_clip_id)) {
        Fail(EditError::DuplicateId,
             "split right_clip_id is invalid or already exists: '" +
                 operation.right_clip_id + "'",
             error, message);
        return false;
    }

    const RationalTime leftDuration =
        operation.timeline_position.sub(original.timeline_in);
    DocumentClip left = original;
    left.duration = leftDuration;
    left.audio_fade_out = {0, 1};
    // Copy the original and override only what a cut actually changes --
    // identity and *where* the piece sits. Everything else a clip carries
    // describes the material, not its placement, so it belongs to both
    // halves. This used to build the right half from an explicit field
    // list, which silently dropped every field the list had not been taught
    // about: ClipEffect stacks and captions were added to DocumentClip
    // without touching this, so splitting a graded clip lost the grade on
    // every piece but the first, with no error anywhere.
    //
    // Captions are copied wholesale rather than re-sliced: duplicating the
    // run's text on both halves is wrong for a cut that lands mid-caption,
    // but re-slicing needs word timings this operation does not have, and
    // silently discarding the text is worse than duplicating it.
    if (operation.right_effect_ids.empty() && !original.effects.empty()) {
        for (size_t effect = 0; effect < original.effects.size(); ++effect)
            operation.right_effect_ids.push_back(GenerateUlid());
    }
    if (operation.right_effect_ids.size() != original.effects.size()) {
        Fail(EditError::InvalidOperation,
             "split requires one right effect ID per effect on the clip", error,
             message);
        return false;
    }
    for (const Ulid& effectId : operation.right_effect_ids) {
        if (!IsValidUlid(effectId) || candidate.FindClip(effectId) ||
            candidate.FindSource(effectId) || candidate.FindTrack(effectId)) {
            Fail(EditError::DuplicateId,
                 "split right effect ID is invalid or already exists: '" +
                     effectId + "'",
                 error, message);
            return false;
        }
    }

    DocumentClip right = original;
    right.id = operation.right_clip_id;
    right.source_in = ClipSourceTimeAt(original, leftDuration);
    right.duration = original.duration.sub(leftDuration);
    right.timeline_in = operation.timeline_position;
    // R2 -- docs/roadmap/REEL-2026-09.md. The left piece keeps every key as
    // it is, the right piece keeps every key shifted by the cut's offset:
    // sampled at the same timeline frame, the two pieces give exactly what
    // the uncut clip gave there. Keys that now sit outside a piece are kept
    // so that the curve keeps its slope at the cut and a join undoes to the
    // same bytes.
    ShiftClipAnimationHead(right, leftDuration);
    right.audio_fade_in = {0, 1};
    for (size_t effect = 0; effect < right.effects.size(); ++effect)
        right.effects[effect].id = operation.right_effect_ids[effect];
    track->clips[index] = std::move(left);
    track->clips.insert(
        track->clips.begin() + static_cast<std::ptrdiff_t>(index + 1),
        std::move(right));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = JoinClipOperation{original.id, operation.right_clip_id,
                                TimesOf(original)};
    return true;
}

bool ApplySplitLinked(Document& candidate, SplitLinkedClipsOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> result;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (!track || std::any_of(result.begin(), result.end(),
                                      [&](const ExactTrackState& state) {
                                          return state.track_id == track->id;
                                      }))
                continue;
            result.push_back({track->id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        for (const ExactTrackState& state : operation.exact_track_result) {
            DocumentTrack* track = candidate.FindTrack(state.track_id);
            if (!track) {
                Fail(EditError::UnknownTrack,
                     "exact linked split references an unknown track", error,
                     message);
                return false;
            }
            before.push_back({track->id, track->clips});
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = SplitLinkedClipsOperation{
            operation.link_group_id,     operation.clip_ids,
            operation.timeline_position, operation.left_group_id,
            operation.right_group_id,    operation.right_clip_ids,
            operation.sync_track_ids,    std::move(before)};
        return true;
    }
    if (operation.link_group_id.empty() || operation.clip_ids.size() < 2) {
        Fail(EditError::InvalidOperation,
             "SplitLinkedClips requires at least two linked members", error,
             message);
        return false;
    }
    std::vector<Ulid> seen;
    std::vector<Ulid> memberTrackIds;
    std::vector<Ulid> affectedTrackIds;
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        const DocumentTrack* track = candidate.FindTrackForClip(id);
        if (!clip || clip->link_group_id != operation.link_group_id || !track ||
            std::find(seen.begin(), seen.end(), id) != seen.end()) {
            Fail(EditError::InvalidOperation,
                 "split_linked_clips members must be unique and share "
                 "link_group_id",
                 error, message);
            return false;
        }
        const RationalTime end = clip->timeline_in.add(clip->duration);
        if (operation.timeline_position.rate <= 0 ||
            operation.timeline_position <= clip->timeline_in ||
            operation.timeline_position >= end) {
            Fail(EditError::InvalidOperation,
                 "split_linked_clips timeline_position must be strictly "
                 "within (" +
                     DescribeTime(clip->timeline_in) + ", " +
                     DescribeTime(end) + ") for clip_id '" + clip->id +
                     "'; got " + DescribeTime(operation.timeline_position),
                 error, message);
            return false;
        }
        seen.push_back(id);
        if (std::find(memberTrackIds.begin(), memberTrackIds.end(),
                      track->id) == memberTrackIds.end())
            memberTrackIds.push_back(track->id);
        if (std::find(affectedTrackIds.begin(), affectedTrackIds.end(),
                      track->id) == affectedTrackIds.end())
            affectedTrackIds.push_back(track->id);
    }
    for (const Ulid& syncTrackId : operation.sync_track_ids) {
        if (!candidate.FindTrack(syncTrackId)) {
            Fail(EditError::UnknownTrack,
                 "unknown linked split sync track_id '" + syncTrackId + "'",
                 error, message);
            return false;
        }
        if (std::find(affectedTrackIds.begin(), affectedTrackIds.end(),
                      syncTrackId) == affectedTrackIds.end())
            affectedTrackIds.push_back(syncTrackId);
    }
    if (operation.left_group_id.empty())
        operation.left_group_id = GenerateUlid();
    if (operation.right_group_id.empty())
        operation.right_group_id = GenerateUlid();
    if (!IsValidUlid(operation.left_group_id) ||
        !IsValidUlid(operation.right_group_id) ||
        operation.left_group_id == operation.right_group_id) {
        Fail(EditError::InvalidOperation,
             "linked split requires two distinct valid group IDs", error,
             message);
        return false;
    }
    if (operation.right_clip_ids.empty()) {
        for (size_t index = 0; index < operation.clip_ids.size(); ++index)
            operation.right_clip_ids.push_back(GenerateUlid());
    }
    if (operation.right_clip_ids.size() != operation.clip_ids.size()) {
        Fail(EditError::InvalidOperation,
             "linked split requires one right clip ID per member", error,
             message);
        return false;
    }
    std::vector<Ulid> generated;
    for (const Ulid& id : operation.right_clip_ids) {
        if (!IsValidUlid(id) || candidate.FindClip(id) ||
            candidate.FindTrack(id) || candidate.FindSource(id) ||
            std::find(generated.begin(), generated.end(), id) !=
                generated.end()) {
            Fail(EditError::DuplicateId,
                 "linked split right clip ID is invalid or already exists",
                 error, message);
            return false;
        }
        generated.push_back(id);
    }

    const std::vector<ExactTrackState> before = snapshots(affectedTrackIds);
    Ulid leftAnchor = operation.clip_ids.front();
    for (const Ulid& id : operation.clip_ids) {
        const DocumentClip* clip = candidate.FindClip(id);
        if (clip && clip->sync_anchor_clip_id == id) {
            leftAnchor = id;
            break;
        }
    }
    const auto anchorPosition = std::find(operation.clip_ids.begin(),
                                          operation.clip_ids.end(), leftAnchor);
    const size_t anchorIndex = static_cast<size_t>(
        std::distance(operation.clip_ids.begin(), anchorPosition));
    const Ulid rightAnchor = operation.right_clip_ids[anchorIndex];
    for (size_t index = 0; index < operation.clip_ids.size(); ++index) {
        SplitClipOperation split{operation.clip_ids[index],
                                 operation.timeline_position,
                                 operation.right_clip_ids[index]};
        Operation ignored = RemoveClipOperation{};
        if (!ApplySplit(candidate, split, ignored, error, message))
            return false;
    }
    for (size_t index = 0; index < operation.clip_ids.size(); ++index) {
        DocumentClip* left = candidate.FindClip(operation.clip_ids[index]);
        DocumentClip* right =
            candidate.FindClip(operation.right_clip_ids[index]);
        left->link_group_id = operation.left_group_id;
        left->sync_anchor_clip_id = leftAnchor;
        right->link_group_id = operation.right_group_id;
        right->sync_anchor_clip_id = rightAnchor;
    }
    for (const Ulid& syncTrackId : operation.sync_track_ids) {
        if (std::find(memberTrackIds.begin(), memberTrackIds.end(),
                      syncTrackId) != memberTrackIds.end())
            continue;
        const DocumentTrack* syncTrack = candidate.FindTrack(syncTrackId);
        Ulid crossingClipId;
        for (const DocumentClip& clip : syncTrack->clips) {
            if (clip.timeline_in < operation.timeline_position &&
                operation.timeline_position <
                    clip.timeline_in.add(clip.duration)) {
                crossingClipId = clip.id;
                break;
            }
        }
        if (crossingClipId.empty()) continue;
        SplitClipOperation split{crossingClipId, operation.timeline_position};
        Operation ignored = RemoveClipOperation{};
        if (!ApplySplit(candidate, split, ignored, error, message))
            return false;
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(affectedTrackIds);
    inverse = SplitLinkedClipsOperation{
        operation.link_group_id,     operation.clip_ids,
        operation.timeline_position, operation.left_group_id,
        operation.right_group_id,    operation.right_clip_ids,
        operation.sync_track_ids,    before};
    return true;
}

// Every track the gesture can touch: the clip's own, plus each A/V-linked
// member's. Snapshotting all of them is what makes one undo restore the whole
// subdivision, however many pieces it produced.
std::vector<Ulid> SplitAffectedTrackIds(const Document& document,
                                        const DocumentClip& clip) {
    std::vector<Ulid> trackIds;
    const auto addTrackOf = [&](const Ulid& clipId) {
        const DocumentTrack* track = document.FindTrackForClip(clipId);
        if (track && std::find(trackIds.begin(), trackIds.end(), track->id) ==
                         trackIds.end())
            trackIds.push_back(track->id);
    };
    addTrackOf(clip.id);
    if (!clip.link_group_id.empty()) {
        for (const DocumentTrack& track : document.sequence.tracks)
            for (const DocumentClip& member : track.clips)
                if (member.link_group_id == clip.link_group_id)
                    addTrackOf(member.id);
    }
    return trackIds;
}

// The single-position cut for `clip`: linked when the clip belongs to a group
// whose members actually straddle `position`, plain otherwise. Deliberately a
// local twin of TimelineView.cc's TimelineCutOperationForClip rather than a
// call into it -- Operations.cc is the layer below the timeline view and does
// not depend on it. Both read the same link_group_id off the document, so
// they agree by construction; only the caller's linked-selection preference,
// which is a view concern, is missing here.
Operation TimelineCutOperation(const Document& document,
                               const DocumentClip& clip,
                               const RationalTime& position) {
    if (!clip.link_group_id.empty()) {
        std::vector<Ulid> members;
        for (const DocumentTrack& track : document.sequence.tracks) {
            for (const DocumentClip& member : track.clips) {
                if (member.link_group_id != clip.link_group_id) continue;
                if (position <= member.timeline_in ||
                    position >= member.timeline_in.add(member.duration))
                    continue;
                members.push_back(member.id);
            }
        }
        if (members.size() > 1)
            return SplitLinkedClipsOperation{
                clip.link_group_id, members, position, {}, {}, {}, {}};
    }
    return SplitClipOperation{clip.id, position, {}, {}};
}

bool ApplySplitAtPositions(Document& candidate,
                           SplitClipAtPositionsOperation& operation,
                           Operation& inverse, EditError& error,
                           std::string& message) {
    const auto snapshots = [&](const std::vector<Ulid>& trackIds) {
        std::vector<ExactTrackState> result;
        for (const Ulid& trackId : trackIds) {
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (track) result.push_back({track->id, track->clips});
        }
        return result;
    };
    if (!operation.exact_track_result.empty()) {
        std::vector<ExactTrackState> before;
        for (const ExactTrackState& state : operation.exact_track_result) {
            DocumentTrack* track = candidate.FindTrack(state.track_id);
            if (!track) {
                Fail(EditError::UnknownTrack,
                     "exact interval split references an unknown track", error,
                     message);
                return false;
            }
            before.push_back({track->id, track->clips});
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = SplitClipAtPositionsOperation{
            operation.clip_id, operation.positions, std::move(before)};
        return true;
    }

    const DocumentClip* original = candidate.FindClip(operation.clip_id);
    if (!original) {
        Fail(EditError::UnknownClip,
             "unknown clip_id '" + operation.clip_id + "'", error, message);
        return false;
    }
    if (operation.positions.empty()) {
        Fail(EditError::InvalidOperation,
             "split requires at least one position", error, message);
        return false;
    }
    const RationalTime clipStart = original->timeline_in;
    const RationalTime clipEnd = clipStart.add(original->duration);
    for (size_t index = 0; index < operation.positions.size(); ++index) {
        const RationalTime& position = operation.positions[index];
        if (position.rate <= 0) {
            Fail(EditError::ArithmeticError, "time rate must be positive",
                 error, message);
            return false;
        }
        if (position <= clipStart || position >= clipEnd) {
            Fail(EditError::InvalidTimelineIn,
                 "split position must be strictly inside the clip", error,
                 message);
            return false;
        }
        if (index > 0 && position <= operation.positions[index - 1]) {
            Fail(EditError::InvalidOperation,
                 "split positions must be strictly increasing", error, message);
            return false;
        }
    }

    const std::vector<Ulid> trackIds =
        SplitAffectedTrackIds(candidate, *original);
    const std::vector<ExactTrackState> before = snapshots(trackIds);

    // Each position is applied through the ordinary single-cut path, so a
    // subdivision is exactly N normal cuts -- same link-group bookkeeping,
    // same effect-stack cloning -- and never a second implementation of
    // "split" that could drift from it. Positions ascend, so each one lands
    // in the right-hand piece produced by the previous.
    for (const RationalTime& position : operation.positions) {
        const DocumentClip* piece = nullptr;
        for (const Ulid& trackId : trackIds) {
            const DocumentTrack* track = candidate.FindTrack(trackId);
            if (!track) continue;
            for (const DocumentClip& candidateClip : track->clips) {
                if (candidateClip.timeline_in < position &&
                    position <
                        candidateClip.timeline_in.add(candidateClip.duration) &&
                    !(candidateClip.timeline_in.add(candidateClip.duration) <=
                      clipStart) &&
                    !(clipEnd <= candidateClip.timeline_in)) {
                    piece = &candidateClip;
                    break;
                }
            }
            if (piece) break;
        }
        if (!piece) {
            Fail(EditError::InvalidTimelineIn,
                 "split position must be strictly inside the clip", error,
                 message);
            return false;
        }
        Operation cut = TimelineCutOperation(candidate, *piece, position);
        Operation ignored = RemoveClipOperation{};
        bool cutApplied = false;
        if (auto* linked = std::get_if<SplitLinkedClipsOperation>(&cut)) {
            cutApplied =
                ApplySplitLinked(candidate, *linked, ignored, error, message);
        } else {
            auto& single = std::get<SplitClipOperation>(cut);
            cutApplied = ApplySplit(candidate, single, ignored, error, message);
        }
        if (!cutApplied) return false;
    }

    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = SplitClipAtPositionsOperation{operation.clip_id,
                                            operation.positions, before};
    return true;
}

bool ApplyJoin(Document& candidate, JoinClipOperation& operation,
               Operation& inverse, EditError& error, std::string& message) {
    DocumentTrack* track = candidate.FindTrackForClip(operation.left_clip_id);
    if (!track) {
        Fail(EditError::UnknownClip,
             "unknown left_clip_id '" + operation.left_clip_id + "'", error,
             message);
        return false;
    }
    const auto left = std::find_if(track->clips.begin(), track->clips.end(),
                                   [&](const DocumentClip& clip) {
                                       return clip.id == operation.left_clip_id;
                                   });
    if (left == track->clips.end() || std::next(left) == track->clips.end() ||
        std::next(left)->id != operation.right_clip_id) {
        Fail(EditError::InvalidOperation,
             "join clips must be adjacent on the same track", error, message);
        return false;
    }
    const DocumentClip right = *std::next(left);
    // R3 -- contiguous in the source the two pieces read: the left one
    // ends, its speed applied, where the right one opens. `source_in +
    // duration`, the test this replaced, refused to undo a cut through any
    // clip not at 1/1. Criterion 4 -- and a ramp runs on across the cut:
    // the right piece carries the left one's keys, shifted by its length
    // (ShiftClipAnimationHead), or the join would drop a curve.
    bool rampContinues = left->speed_ramp.size() == right.speed_ramp.size();
    for (size_t index = 0; rampContinues && index < right.speed_ramp.size();
         ++index) {
        const ClipKeyframe& before = left->speed_ramp[index];
        const ClipKeyframe& after = right.speed_ramp[index];
        rampContinues = before.id == after.id &&
                        before.interpolation == after.interpolation &&
                        before.value.num == after.value.num &&
                        before.value.den == after.value.den &&
                        before.time.sub(left->duration) == after.time;
    }
    if (left->source_id != right.source_id ||
        left->composition_id != right.composition_id ||
        left->motion_props.Dump() != right.motion_props.Dump() ||
        left->speed.num != right.speed.num ||
        left->speed.den != right.speed.den || !rampContinues ||
        left->timeline_in.add(left->duration) != right.timeline_in ||
        ClipSourceEnd(*left) != right.source_in) {
        Fail(EditError::InvalidOperation,
             "join clips are not contiguous pieces of one source", error,
             message);
        return false;
    }
    const RationalTime splitPosition = right.timeline_in;
    left->source_in = operation.joined_times.source_in;
    left->duration = operation.joined_times.duration;
    left->timeline_in = operation.joined_times.timeline_in;
    // Split keeps the attack with the left piece and release with the right;
    // restore the latter when their exact inverse joins them again.
    left->audio_fade_out = right.audio_fade_out;
    track->clips.erase(std::next(left));
    if (!ValidateResult(candidate, error, message)) return false;
    inverse = SplitClipOperation{operation.left_clip_id, splitPosition,
                                 operation.right_clip_id};
    return true;
}

// Removes the clip's transcript-derived ranges and ripple-closes each cut.
//
// Fragment placement rule (the one explicit rounding-adjacent decision this
// operation makes, documented once here rather than left implicit):
//   - The clip is cut into the kept fragments that remain outside every
//     removed range, in source order.
//   - The first kept fragment keeps the original clip's timeline_in exactly
//     -- if the very first range starts at the clip's own source_in (a head
//     removal), this is exactly an ordinary head ripple trim: nothing before
//     the clip moves.
//   - Every following kept fragment starts `gap_padding` after the previous
//     kept fragment's end. Padding therefore only ever appears *between two
//     fragments of this same clip* (an interior cut); it is never inserted
//     at the clip's own head or tail edge, where there is no fragment on the
//     other side to pad against -- a tail removal ripples the same way an
//     ordinary tail ripple trim does, with no trailing gap.
//   - Everything after the clip's original end -- on its own track, and on
//     every track in sync_track_ids -- shifts by exactly
//     (new total span) - (original total span), so downstream material
//     closes up around whatever combination of real cuts and kept padding
//     resulted.
bool ApplyRemoveWords(Document& candidate, RemoveWordsOperation& operation,
                      Operation& inverse, EditError& error,
                      std::string& message) {
    std::vector<Ulid> trackIds;
    const auto addTrack = [&](const Ulid& id) {
        if (!id.empty() &&
            std::find(trackIds.begin(), trackIds.end(), id) == trackIds.end())
            trackIds.push_back(id);
    };
    const auto snapshots = [&](const std::vector<Ulid>& ids) {
        std::vector<ExactTrackState> states;
        for (const Ulid& id : ids) {
            const DocumentTrack* track = candidate.FindTrack(id);
            if (track) states.push_back({id, track->clips});
        }
        return states;
    };

    if (!operation.exact_track_result.empty()) {
        for (const ExactTrackState& state : operation.exact_track_result)
            addTrack(state.track_id);
        const std::vector<ExactTrackState> before = snapshots(trackIds);
        if (before.size() != operation.exact_track_result.size()) {
            Fail(EditError::UnknownTrack,
                 "exact word removal state references an unknown track", error,
                 message);
            return false;
        }
        for (const ExactTrackState& state : operation.exact_track_result)
            candidate.FindTrack(state.track_id)->clips = state.clips;
        if (!ValidateResult(candidate, error, message)) return false;
        inverse = RemoveWordsOperation{
            operation.clip_id,         operation.ranges,
            operation.gap_padding,     operation.linked_clip_ids,
            operation.sync_track_ids,  before,
            operation.translated_links};
        return true;
    }

    if (operation.ranges.empty()) {
        Fail(EditError::InvalidOperation,
             "RemoveWords requires at least one range", error, message);
        return false;
    }
    // R3 -- docs/roadmap/REEL-2026-09.md, criterion 3. A word cut is given
    // in source time and rebuilds the clip as a run of fragments, each of
    // which would have to keep the speed's phase inside the source frame it
    // opens on. Refused by name rather than cut a frame away from the word.
    if (const DocumentClip* anchor = candidate.FindClip(operation.clip_id)) {
        if (ClipHasSpeed(*anchor)) {
            Fail(EditError::InvalidOperation,
                 ClipSpeedRefusal(*anchor, "remove_words"), error, message);
            return false;
        }
    }
    if (operation.gap_padding.rate <= 0 || operation.gap_padding.value < 0) {
        Fail(EditError::ArithmeticError,
             "RemoveWords gap_padding must be a non-negative exact duration",
             error, message);
        return false;
    }
    const auto rangesValid = [&](const std::vector<WordRemovalRange>& ranges) {
        for (size_t index = 0; index < ranges.size(); ++index) {
            const WordRemovalRange& range = ranges[index];
            if (range.source_start.rate <= 0 || range.source_end.rate <= 0 ||
                range.source_start >= range.source_end) {
                Fail(EditError::InvalidOperation,
                     "RemoveWords range must have a positive duration", error,
                     message);
                return false;
            }
            if (index > 0 &&
                range.source_start < ranges[index - 1].source_end) {
                Fail(EditError::InvalidOperation,
                     "RemoveWords ranges must be sorted and non-overlapping",
                     error, message);
                return false;
            }
        }
        return true;
    };
    if (!rangesValid(operation.ranges)) {
        return false;
    }

    // The anchor, plus every linked clip named alongside it. Each is cut by
    // the same source ranges and ripple-closed on its own track, inside one
    // operation. An A/V pair has to lose the same frames of picture and
    // sound or it drifts apart, and splitting the cleanup into two
    // operations would make one editorial gesture two undo steps.
    std::vector<Ulid> cutClipIds;
    // The source ranges each clip loses: the anchor's for itself and for
    // the linked clips of its source; their own for the others (SY3).
    std::vector<const std::vector<WordRemovalRange>*> cutRanges;
    cutClipIds.push_back(operation.clip_id);
    cutRanges.push_back(&operation.ranges);
    for (const Ulid& linkedId : operation.linked_clip_ids) {
        if (std::find(cutClipIds.begin(), cutClipIds.end(), linkedId) !=
            cutClipIds.end()) {
            Fail(EditError::DuplicateId,
                 "RemoveWords linked_clip_ids repeats '" + linkedId + "'",
                 error, message);
            return false;
        }
        cutClipIds.push_back(linkedId);
        cutRanges.push_back(&operation.ranges);
    }
    for (const LinkedWordRemoval& link : operation.translated_links) {
        if (std::find(cutClipIds.begin(), cutClipIds.end(), link.clip_id) !=
            cutClipIds.end()) {
            Fail(EditError::DuplicateId,
                 "RemoveWords translated_links repeats '" + link.clip_id + "'",
                 error, message);
            return false;
        }
        if (link.ranges.empty()) {
            Fail(EditError::InvalidOperation,
                 "RemoveWords translated link '" + link.clip_id +
                     "' has no range",
                 error, message);
            return false;
        }
        if (!rangesValid(link.ranges)) {
            return false;
        }
        cutClipIds.push_back(link.clip_id);
        cutRanges.push_back(&link.ranges);
    }

    // Every clip is resolved and validated before any of them is touched, so
    // a partially applied cut is not a state this function can leave behind.
    struct CutTarget {
        DocumentTrack* track = nullptr;
        DocumentClip original;
        const std::vector<WordRemovalRange>* ranges = nullptr;
    };
    std::vector<CutTarget> targets;
    for (size_t cutIndex = 0; cutIndex < cutClipIds.size(); ++cutIndex) {
        const Ulid& cutId = cutClipIds[cutIndex];
        const std::vector<WordRemovalRange>& cutRange = *cutRanges[cutIndex];
        DocumentTrack* cutTrack = candidate.FindTrackForClip(cutId);
        if (!cutTrack) {
            Fail(EditError::UnknownClip, "unknown clip_id '" + cutId + "'",
                 error, message);
            return false;
        }
        const auto found = std::find_if(
            cutTrack->clips.begin(), cutTrack->clips.end(),
            [&](const DocumentClip& clip) { return clip.id == cutId; });
        const DocumentClip original = *found;
        // MOTION-2026-09 M4 -- words belong to a transcribed media; a
        // composition has none to remove.
        if (IsMotionClip(original)) {
            Fail(EditError::InvalidOperation,
                 "RemoveWords cannot cut motion clip '" + cutId +
                     "': a composition has no transcribed words",
                 error, message);
            return false;
        }
        const RationalTime sourceEnd =
            original.source_in.add(original.duration);
        for (const WordRemovalRange& range : cutRange) {
            if (range.source_start < original.source_in ||
                range.source_end > sourceEnd) {
                Fail(EditError::SourceOutOfBounds,
                     "RemoveWords range falls outside the source range of "
                     "clip_id '" +
                         cutId + "'",
                     error, message);
                return false;
            }
        }
        targets.push_back({cutTrack, original, &cutRange});
        addTrack(cutTrack->id);
    }
    for (const Ulid& id : operation.sync_track_ids) {
        if (!candidate.FindTrack(id)) {
            Fail(EditError::UnknownTrack,
                 "unknown RemoveWords sync track_id '" + id + "'", error,
                 message);
            return false;
        }
        addTrack(id);
    }
    const std::vector<ExactTrackState> before = snapshots(trackIds);

    struct Fragment {
        RationalTime source_start;
        RationalTime source_end;
    };
    std::vector<DocumentClip> newClips;
    RationalTime anchorOriginalEnd{0, 1};
    RationalTime anchorShiftDelta{0, 1};
    // The anchor's kept fragments: original source span and new timeline
    // start, which is all the time map below needs.
    struct PlacedFragment {
        RationalTime source_start;
        RationalTime source_end;
        RationalTime timeline_in;
    };
    std::vector<PlacedFragment> anchorFragments;
    // The ids of each target's kept stretches, in order: the link groups
    // below pair them across targets.
    std::vector<std::vector<Ulid>> stretchIds(targets.size());
    for (size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        DocumentTrack* cutTrack = targets[targetIndex].track;
        const DocumentClip original = targets[targetIndex].original;
        const RationalTime clipSourceStart = original.source_in;
        const RationalTime clipSourceEnd =
            original.source_in.add(original.duration);

        std::vector<Fragment> fragments;
        RationalTime cursor = clipSourceStart;
        for (const WordRemovalRange& range : *targets[targetIndex].ranges) {
            if (range.source_start > cursor)
                fragments.push_back({cursor, range.source_start});
            cursor = range.source_end;
        }
        if (cursor < clipSourceEnd)
            fragments.push_back({cursor, clipSourceEnd});

        std::vector<DocumentClip> targetClips;
        RationalTime timelineCursor = original.timeline_in;
        for (size_t fragmentIndex = 0; fragmentIndex < fragments.size();
             ++fragmentIndex) {
            if (fragmentIndex > 0)
                timelineCursor = timelineCursor.add(operation.gap_padding);
            DocumentClip fragment = original;
            if (fragmentIndex != 0) RenewClipIdentity(fragment);
            fragment.source_in = fragments[fragmentIndex].source_start;
            fragment.duration = fragments[fragmentIndex].source_end.sub(
                fragments[fragmentIndex].source_start);
            fragment.timeline_in = timelineCursor;
            // R2 -- a word removal cuts the clip into pieces whose heads
            // are further into the material; remove_words refuses a clip
            // that is not at 1/1 (R3), so that distance is also the local
            // time the head advanced by, and the keys follow their
            // pictures.
            ShiftClipAnimationHead(
                fragment,
                fragments[fragmentIndex].source_start.sub(clipSourceStart));
            // Internal removals create edit boundaries, not fade boundaries:
            // retain the original attack on the first fragment and release on
            // the last fragment only.
            if (fragmentIndex != 0) fragment.audio_fade_in = {0, 1};
            if (fragmentIndex + 1 != fragments.size())
                fragment.audio_fade_out = {0, 1};
            if (targetIndex == 0)
                anchorFragments.push_back(
                    {fragments[fragmentIndex].source_start,
                     fragments[fragmentIndex].source_end, timelineCursor});
            timelineCursor = timelineCursor.add(fragment.duration);
            targetClips.push_back(std::move(fragment));
        }
        const RationalTime newEnd =
            fragments.empty() ? original.timeline_in : timelineCursor;
        const RationalTime originalEnd =
            original.timeline_in.add(original.duration);
        const RationalTime shiftDelta = newEnd.sub(originalEnd);
        if (targetIndex == 0) {
            anchorOriginalEnd = originalEnd;
            anchorShiftDelta = shiftDelta;
        }

        // Re-found rather than remembered: an earlier target may already have
        // spliced this same track, which moves every index after it.
        const auto position = std::find_if(
            cutTrack->clips.begin(), cutTrack->clips.end(),
            [&](const DocumentClip& clip) { return clip.id == original.id; });
        if (position == cutTrack->clips.end()) {
            Fail(EditError::UnknownClip,
                 "RemoveWords lost clip_id '" + original.id + "' while cutting",
                 error, message);
            return false;
        }
        const size_t positionIndex = static_cast<size_t>(
            std::distance(cutTrack->clips.begin(), position));
        for (size_t next = positionIndex + 1; next < cutTrack->clips.size();
             ++next)
            cutTrack->clips[next].timeline_in =
                cutTrack->clips[next].timeline_in.add(shiftDelta);
        cutTrack->clips.erase(cutTrack->clips.begin() +
                              static_cast<std::ptrdiff_t>(positionIndex));
        cutTrack->clips.insert(cutTrack->clips.begin() +
                                   static_cast<std::ptrdiff_t>(positionIndex),
                               targetClips.begin(), targetClips.end());
        for (const DocumentClip& stretch : targetClips)
            stretchIds[targetIndex].push_back(stretch.id);
        newClips.insert(newClips.end(), targetClips.begin(), targetClips.end());
    }

    // Out of lot -- the author, 29 October 2026: "cut shots stay linked to each
    // other". Each kept stretch is a shot of its own, linked to the matching
    // stretch of its partner and to nothing else, as a cut in the window
    // leaves it (SplitLinkedClips, and a range delete's sideGroup). The
    // stretches used to keep the original group, so one click selected, and
    // one drag moved, every stretch of a speech block at once. The first
    // stretch keeps its group and ids; each later one gets a group shared by
    // the members cut at that same stretch, and its sync anchor moves to the
    // anchor's own stretch. When the group holds a clip this removal did not
    // cut, or the members were cut into different numbers of stretches, the
    // old grouping stays: no pairing is invented.
    const Ulid sharedGroup = targets.front().original.link_group_id;
    const size_t stretchCount = stretchIds.front().size();
    bool regroup =
        !sharedGroup.empty() && targets.size() > 1 && stretchCount > 1;
    for (size_t index = 0; regroup && index < targets.size(); ++index)
        regroup = targets[index].original.link_group_id == sharedGroup &&
                  stretchIds[index].size() == stretchCount;
    if (regroup) {
        size_t members = 0;
        for (const DocumentTrack& track : candidate.sequence.tracks)
            members += static_cast<size_t>(
                std::count_if(track.clips.begin(), track.clips.end(),
                              [&](const DocumentClip& clip) {
                                  return clip.link_group_id == sharedGroup;
                              }));
        regroup = members == stretchCount * targets.size();
    }
    for (size_t stretch = 1; regroup && stretch < stretchCount; ++stretch) {
        const Ulid group = GenerateUlid();
        for (size_t index = 0; index < targets.size(); ++index) {
            DocumentClip* piece =
                candidate.FindClip(stretchIds[index][stretch]);
            if (!piece) continue;
            piece->link_group_id = group;
            const Ulid& anchor = targets[index].original.sync_anchor_clip_id;
            for (size_t other = 0; other < targets.size(); ++other)
                if (!anchor.empty() && targets[other].original.id == anchor)
                    piece->sync_anchor_clip_id = stretchIds[other][stretch];
        }
    }

    // Sync tracks follow the anchor. A track that was itself cut above has
    // already closed up around its own fragments, so moving it again here
    // would move it twice.
    //
    // ITW7 -- ROADMAP.md. They used to shift only when they started after the
    // anchor's end, so a cutaway placed over the words being removed did not
    // move at all and ended up over other words; every cut made the agent
    // recompute its cutaways. Each sync clip now goes through the same time
    // change as the words under it, pictures kept continuous from its head
    // (RangeDeleteFollowers::Continuous in TimelineView.h, same rule): its
    // start lands where the next kept words now start, its end where the last
    // kept words before it now end, and one lying wholly over removed words
    // goes with them.
    const DocumentClip& anchorOriginal = targets.front().original;
    const auto mapTime = [&](const RationalTime& time, bool isEnd) {
        if (time <= anchorOriginal.timeline_in) return time;
        if (time >= anchorOriginalEnd) return time.add(anchorShiftDelta);
        const RationalTime source =
            anchorOriginal.source_in.add(time.sub(anchorOriginal.timeline_in));
        RationalTime collapsed = isEnd
                                     ? anchorOriginal.timeline_in
                                     : anchorOriginalEnd.add(anchorShiftDelta);
        for (const PlacedFragment& fragment : anchorFragments) {
            const bool inside = isEnd ? fragment.source_start < source &&
                                            source <= fragment.source_end
                                      : fragment.source_start <= source &&
                                            source < fragment.source_end;
            if (inside)
                return fragment.timeline_in.add(
                    source.sub(fragment.source_start));
            if (isEnd && fragment.source_end < source)
                collapsed = fragment.timeline_in.add(
                    fragment.source_end.sub(fragment.source_start));
            if (!isEnd && source < fragment.source_start) {
                collapsed = fragment.timeline_in;
                break;
            }
        }
        return collapsed;
    };
    for (const Ulid& id : operation.sync_track_ids) {
        const bool alreadyCut = std::any_of(
            targets.begin(), targets.end(),
            [&](const CutTarget& target) { return target.track->id == id; });
        if (alreadyCut) continue;
        DocumentTrack* syncTrack = candidate.FindTrack(id);
        std::vector<DocumentClip> followed;
        for (const DocumentClip& clip : syncTrack->clips) {
            const RationalTime start = mapTime(clip.timeline_in, false);
            const RationalTime end =
                mapTime(clip.timeline_in.add(clip.duration), true);
            if (end <= start) continue;
            DocumentClip moved = clip;
            moved.timeline_in = start;
            moved.duration = end.sub(start);
            if (moved.duration < moved.audio_fade_out)
                moved.audio_fade_out = moved.duration;
            if (moved.duration.sub(moved.audio_fade_out) < moved.audio_fade_in)
                moved.audio_fade_in = moved.duration.sub(moved.audio_fade_out);
            followed.push_back(std::move(moved));
        }
        syncTrack->clips = std::move(followed);
    }

    for (const DocumentClip& fragment : newClips) {
        const DocumentSource* source = candidate.FindSource(fragment.source_id);
        if (!source) {
            Fail(EditError::UnknownSource,
                 "RemoveWords clip references an unknown source", error,
                 message);
            return false;
        }
        if (!ValidateSourceRange(*source, fragment.source_in, fragment.duration,
                                 error, message)) {
            return false;
        }
    }
    if (!ValidateResult(candidate, error, message)) return false;
    operation.exact_track_result = snapshots(trackIds);
    inverse = RemoveWordsOperation{
        operation.clip_id,         operation.ranges,
        operation.gap_padding,     operation.linked_clip_ids,
        operation.sync_track_ids,  before,
        operation.translated_links};
    return true;
}

// PL1 -- ROADMAP.md. Each member goes through ApplyOperation and every
// check it makes (locked track, validation), on the batch's own candidate,
// so a batch can do nothing its members could not do one after the other.
// A refusal anywhere returns before the caller commits the candidate: no
// earlier member's effect survives it.
bool ApplyBatch(Document& candidate, BatchOperation& operation,
                Operation& inverse, EditError& error, std::string& message) {
    if (operation.label.empty()) {
        Fail(EditError::InvalidOperation, "batch label is empty", error,
             message);
        return false;
    }
    if (operation.operations.empty()) {
        Fail(EditError::InvalidOperation, "batch holds no operation", error,
             message);
        return false;
    }
    if (operation.operations.size() > kMaxBatchOperations) {
        Fail(EditError::InvalidOperation,
             "batch holds " + std::to_string(operation.operations.size()) +
                 " operations; at most " + std::to_string(kMaxBatchOperations) +
                 " make one undo step",
             error, message);
        return false;
    }
    BatchOperation undo{operation.label, {}};
    undo.operations.reserve(operation.operations.size());
    for (size_t index = 0; index < operation.operations.size(); ++index) {
        Operation& member = operation.operations[index];
        if (std::holds_alternative<BatchOperation>(member)) {
            Fail(EditError::InvalidOperation,
                 "batch operation " + std::to_string(index) +
                     " is itself a batch",
                 error, message);
            return false;
        }
        Operation memberInverse = RemoveClipOperation{};
        if (!ApplyOperation(candidate, member, memberInverse, error, message)) {
            message = "batch operation " + std::to_string(index) + ": " +
                      message + "; nothing was applied";
            return false;
        }
        undo.operations.push_back(std::move(memberInverse));
    }
    std::reverse(undo.operations.begin(), undo.operations.end());
    inverse = std::move(undo);
    return true;
}

}  // namespace

std::vector<Ulid> WordRemovalSyncTrackIds(
    const Document& document, const RemoveWordsOperation& operation) {
    std::vector<Ulid> cutTracks;
    const auto addCutTrack = [&](const Ulid& clipId) {
        if (const DocumentTrack* track = document.FindTrackForClip(clipId))
            cutTracks.push_back(track->id);
    };
    addCutTrack(operation.clip_id);
    for (const Ulid& linkedId : operation.linked_clip_ids)
        addCutTrack(linkedId);
    for (const LinkedWordRemoval& link : operation.translated_links) {
        addCutTrack(link.clip_id);
    }
    std::vector<Ulid> sync;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.sync_lock && !track.locked &&
            std::find(cutTracks.begin(), cutTracks.end(), track.id) ==
                cutTracks.end())
            sync.push_back(track.id);
    }
    return sync;
}

std::vector<Ulid> LinkedClipIdsCoveringRanges(
    const Document& document, const DocumentClip& clip,
    const std::vector<WordRemovalRange>& ranges) {
    std::vector<Ulid> linked;
    if (clip.link_group_id.empty()) return linked;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& other : track.clips) {
            if (other.id == clip.id ||
                other.link_group_id != clip.link_group_id ||
                other.source_id != clip.source_id)
                continue;
            const RationalTime sourceEnd = other.source_in.add(other.duration);
            const bool covers = std::all_of(
                ranges.begin(), ranges.end(),
                [&](const WordRemovalRange& range) {
                    return range.source_start.compare(other.source_in) >= 0 &&
                           range.source_end.compare(sourceEnd) <= 0;
                });
            if (covers) linked.push_back(other.id);
        }
    }
    return linked;
}

std::vector<LinkedWordRemoval> TranslatedLinkedRemovals(
    const Document& document, const DocumentClip& clip,
    const std::vector<WordRemovalRange>& ranges) {
    std::vector<LinkedWordRemoval> links;
    if (clip.link_group_id.empty() || ranges.empty()) {
        return links;
    }
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& other : track.clips) {
            if (other.id == clip.id ||
                other.link_group_id != clip.link_group_id ||
                other.source_id == clip.source_id || IsMotionClip(other)) {
                continue;
            }
            // The instant `clip` plays source time s is played by `other` at
            // s + (clip's phase - other's phase), phase being where a
            // clip's source time zero falls on the timeline.
            try {
                const RationalTime shift =
                    clip.timeline_in.sub(clip.source_in)
                        .sub(other.timeline_in.sub(other.source_in));
                const RationalTime otherEnd =
                    other.source_in.add(other.duration);
                LinkedWordRemoval link{other.id, {}};
                bool covers = true;
                for (const WordRemovalRange& range : ranges) {
                    const WordRemovalRange moved{range.source_start.add(shift),
                                                 range.source_end.add(shift)};
                    if (moved.source_start < other.source_in ||
                        moved.source_end > otherEnd) {
                        covers = false;
                        break;
                    }
                    link.ranges.push_back(moved);
                }
                if (covers) {
                    links.push_back(std::move(link));
                }
            } catch (const std::overflow_error&) {
                // Rates whose common multiple overflows: no exact
                // translation, so the member is left alone.
                continue;
            }
        }
    }
    return links;
}

void AttachLinkedRemovals(const Document& document, const DocumentClip& clip,
                          RemoveWordsOperation& operation) {
    operation.linked_clip_ids =
        LinkedClipIdsCoveringRanges(document, clip, operation.ranges);
    operation.translated_links =
        TranslatedLinkedRemovals(document, clip, operation.ranges);
}

std::vector<Ulid> LinkedEdgePartnerIds(const Document& document,
                                       const DocumentClip& clip,
                                       TrimEdge edge) {
    std::vector<Ulid> partners;
    if (clip.link_group_id.empty()) {
        return partners;
    }
    const DocumentTrack* anchorTrack = document.FindTrackForClip(clip.id);
    const auto edgeOf = [&](const DocumentClip& member) {
        return edge == TrimEdge::Head ? member.timeline_in
                                      : member.timeline_in.add(member.duration);
    };
    const RationalTime anchorEdge = edgeOf(clip);
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (&track == anchorTrack) {
            continue;
        }
        const DocumentClip* partner = nullptr;
        RationalTime nearest;
        for (const DocumentClip& member : track.clips) {
            if (member.link_group_id != clip.link_group_id) {
                continue;
            }
            RationalTime distance = edgeOf(member).sub(anchorEdge);
            if (distance.value < 0) {
                distance.value = -distance.value;
            }
            if (partner == nullptr || distance < nearest) {
                partner = &member;
                nearest = distance;
            }
        }
        if (partner != nullptr) {
            partners.push_back(partner->id);
        }
    }
    return partners;
}

std::vector<Ulid> SlideClipsWithLinkedPartners(const Document& document,
                                               std::vector<Ulid> clipIds) {
    const size_t named = clipIds.size();
    for (size_t index = 0; index < named; ++index) {
        const DocumentClip* clip = document.FindClip(clipIds[index]);
        if (clip == nullptr) {
            continue;
        }
        for (const Ulid& partner :
             LinkedEdgePartnerIds(document, *clip, TrimEdge::Head)) {
            if (std::find(clipIds.begin(), clipIds.end(), partner) ==
                clipIds.end()) {
                clipIds.push_back(partner);
            }
        }
    }
    return clipIds;
}

std::vector<Ulid> SlipClipsWithLinkedPartners(const Document& document,
                                              std::vector<Ulid> clipIds) {
    return SlideClipsWithLinkedPartners(document, std::move(clipIds));
}

bool ResolveIntervalSplits(const Document& document, const Ulid& clipId,
                           const RationalTime& interval,
                           SplitClipAtPositionsOperation& operation,
                           std::string& error) {
    operation = SplitClipAtPositionsOperation{};
    if (interval.rate <= 0 || interval.value <= 0) {
        error = "interval must be a positive duration";
        return false;
    }
    const DocumentClip* clip = document.FindClip(clipId);
    if (!clip) {
        error = "no clip matches id '" + clipId + "'";
        return false;
    }
    const RationalTime start = clip->timeline_in;
    const RationalTime end = start.add(clip->duration);
    // Accumulate by repeated exact addition rather than multiplying a
    // converted frame count: RationalTime::add is the only thing in this
    // codebase allowed to reconcile two timebases, and going through it once
    // per step keeps every position exact even when the interval's rate and
    // the clip's disagree.
    RationalTime position = start.add(interval);
    while (position < end) {
        operation.positions.push_back(position);
        position = position.add(interval);
    }
    if (operation.positions.empty()) {
        error = "interval is longer than the clip; nothing to cut";
        return false;
    }
    operation.clip_id = clipId;
    return true;
}

const char* EditErrorName(EditError error) {
    switch (error) {
        case EditError::None:
            return "None";
        case EditError::UnknownTrack:
            return "UnknownTrack";
        case EditError::UnknownClip:
            return "UnknownClip";
        case EditError::UnknownSource:
            return "UnknownSource";
        case EditError::UnknownBin:
            return "UnknownBin";
        case EditError::UnknownMarker:
            return "UnknownMarker";
        case EditError::UnknownTransition:
            return "UnknownTransition";
        case EditError::UnknownCaptionStyle:
            return "UnknownCaptionStyle";
        case EditError::UnknownMulticamGroup:
            return "UnknownMulticamGroup";
        case EditError::UnknownMulticamAngle:
            return "UnknownMulticamAngle";
        case EditError::UnknownSequence:
            return "UnknownSequence";
        case EditError::UnknownMedia:
            return "UnknownMedia";
        case EditError::InvalidDuration:
            return "InvalidDuration";
        case EditError::InvalidTimelineIn:
            return "InvalidTimelineIn";
        case EditError::SourceOutOfBounds:
            return "SourceOutOfBounds";
        case EditError::Overlap:
            return "Overlap";
        case EditError::DuplicateId:
            return "DuplicateId";
        case EditError::ArithmeticError:
            return "ArithmeticError";
        case EditError::InvalidOperation:
            return "InvalidOperation";
        case EditError::LockedTrack:
            return "LockedTrack";
        case EditError::ValidationFailed:
            return "ValidationFailed";
        case EditError::EmptyUndo:
            return "EmptyUndo";
        case EditError::EmptyRedo:
            return "EmptyRedo";
        case EditError::IoError:
            return "IoError";
        case EditError::ParseError:
            return "ParseError";
        case EditError::UnknownComposition:
            return "UnknownComposition";
        case EditError::UnknownMotionNode:
            return "UnknownMotionNode";
        case EditError::UnknownKeyframe:
            return "UnknownKeyframe";
        case EditError::UnknownProp:
            return "UnknownProp";
        case EditError::PropOutOfRange:
            return "PropOutOfRange";
        case EditError::PropTypeMismatch:
            return "PropTypeMismatch";
        case EditError::InvalidMotion:
            return "InvalidMotion";
    }
    return "InvalidOperation";
}

// PA4 -- docs/roadmap/PARITE-2026-11.md. A carton's words are timed on its
// own clock, `timeline_in - source_in` (DocumentClip::caption_word_times).
// A cut advances the right half's source_in, so the clock holds. A head
// trim, a roll and a slide move a carton's start and leave its source_in
// alone -- a carton has no media to slide -- which would move every word
// with the edge. This puts the times back: for a carton whose start moved
// while its end, its source_in and its times stayed, each time moves the
// other way. It is written once here, after whatever operation applied,
// and not in the three that need it today: the rule is about what happened
// to the clip, and an inverse is the same movement backwards, so an undo
// lands on the bytes it left. An operation that wrote the times itself (a
// snapshot restored, set_clip_caption) is left alone.
void KeepCaptionWordsOnTheirTime(const Document& before, Document& after) {
    for (DocumentTrack& track : after.sequence.tracks) {
        if (track.kind != "caption") continue;
        for (DocumentClip& clip : track.clips) {
            if (clip.caption_word_times.empty()) continue;
            const DocumentClip* was = before.FindClip(clip.id);
            if (was == nullptr || was->caption_word_times.size() !=
                                      clip.caption_word_times.size())
                continue;
            bool same =
                was->source_in.compare(clip.source_in) == 0 &&
                was->timeline_in.add(was->duration)
                        .compare(clip.timeline_in.add(clip.duration)) == 0 &&
                was->timeline_in.compare(clip.timeline_in) != 0;
            for (size_t index = 0;
                 same && index < clip.caption_word_times.size(); ++index) {
                same = was->caption_word_times[index].value ==
                           clip.caption_word_times[index].value &&
                       was->caption_word_times[index].rate ==
                           clip.caption_word_times[index].rate;
            }
            if (!same) continue;
            const RationalTime moved = clip.timeline_in.sub(was->timeline_in);
            for (RationalTime& time : clip.caption_word_times)
                time = ReducedCaptionWordTime(time.sub(moved));
        }
    }
}

bool ApplyOperation(Document& document, Operation& operation,
                    Operation& inverse, EditError& error,
                    std::string& message) {
    Document candidate = document;
    Operation normalized = operation;
    Operation generatedInverse = RemoveClipOperation{};
    try {
        if (!std::holds_alternative<SetTrackLockOperation>(normalized) &&
            !std::holds_alternative<SetTrackSyncLockOperation>(normalized) &&
            !std::holds_alternative<SetTrackOutputOperation>(normalized)) {
            if (const DocumentTrack* locked =
                    LockedTrackTouchedBy(document, normalized)) {
                Fail(EditError::LockedTrack,
                     "track_id '" + locked->id + "' is locked", error, message);
                return false;
            }
        }
        // The dispatch is exhaustive by construction: every alternative of
        // Operation reaches one branch, and an alternative added without
        // one fails to compile on the static_assert below instead of
        // refusing at run time with no error set.
        const bool applied = std::visit(
            [&](auto& value) -> bool {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, BatchOperation>) {
                    return ApplyBatch(candidate, value, generatedInverse, error,
                                      message);
                } else if constexpr (std::is_same_v<T, InsertClipOperation>) {
                    return ApplyInsert(candidate, value, generatedInverse,
                                       error, message);
                } else if constexpr (std::is_same_v<
                                         T, InsertMotionClipOperation>) {
                    // MOTION-2026-09 M4.
                    return ApplyInsertMotionClip(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetClipMotionPropsOperation>) {
                    return ApplySetClipMotionProps(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, RemoveClipOperation>) {
                    return ApplyRemove(candidate, value, generatedInverse,
                                       error, message);
                } else if constexpr (std::is_same_v<T, ClearClipOperation>) {
                    return ApplyClear(candidate, value, generatedInverse, error,
                                      message);
                } else if constexpr (std::is_same_v<T, ClearClipsOperation>) {
                    return ApplyClearClips(candidate, value, generatedInverse,
                                           error, message);
                } else if constexpr (std::is_same_v<T, PasteClipsOperation>) {
                    return ApplyPasteClips(candidate, value, generatedInverse,
                                           error, message);
                } else if constexpr (std::is_same_v<T, TrimClipOperation>) {
                    return ApplyTrim(candidate, value, generatedInverse, error,
                                     message);
                } else if constexpr (std::is_same_v<T, MoveClipOperation>) {
                    return ApplyMove(candidate, value, generatedInverse, error,
                                     message);
                } else if constexpr (std::is_same_v<T, MoveClipsOperation>) {
                    return ApplyMoveClips(candidate, value, generatedInverse,
                                          error, message);
                } else if constexpr (std::is_same_v<T,
                                                    MoveLinkedClipsOperation>) {
                    return ApplyMoveLinked(candidate, value, generatedInverse,
                                           error, message);
                } else if constexpr (std::is_same_v<T,
                                                    TrimLinkedClipsOperation>) {
                    return ApplyTrimLinked(candidate, value, generatedInverse,
                                           error, message);
                } else if constexpr (std::is_same_v<T, RippleTrimOperation>) {
                    return ApplyRippleTrim(candidate, value, generatedInverse,
                                           error, message);
                } else if constexpr (std::is_same_v<T,
                                                    TrimBoundaryAirOperation>) {
                    return ApplyTrimBoundaryAir(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, RollEditOperation>) {
                    return ApplyRollEdit(candidate, value, generatedInverse,
                                         error, message);
                } else if constexpr (std::is_same_v<T, SlipEditOperation>) {
                    return ApplySlipEdit(candidate, value, generatedInverse,
                                         error, message);
                } else if constexpr (std::is_same_v<T, SlideEditOperation>) {
                    // FC3 -- docs/roadmap/FILMCRAFT-2026-10.md.
                    return ApplySlideEdit(candidate, value, generatedInverse,
                                          error, message);
                } else if constexpr (std::is_same_v<T, RateStretchOperation>) {
                    return ApplyRateStretch(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<
                                         T, RemoveLinkedClipsOperation>) {
                    return ApplyRemoveLinked(candidate, value, generatedInverse,
                                             error, message);
                } else if constexpr (std::is_same_v<
                                         T, ClearLinkedClipsOperation>) {
                    return ApplyClearLinked(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T, SplitClipOperation>) {
                    return ApplySplit(candidate, value, generatedInverse, error,
                                      message);
                } else if constexpr (std::is_same_v<
                                         T, SplitLinkedClipsOperation>) {
                    return ApplySplitLinked(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<
                                         T, SplitClipAtPositionsOperation>) {
                    return ApplySplitAtPositions(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, DeleteGapOperation>) {
                    return ApplyDeleteGap(candidate, value, generatedInverse,
                                          error, message);
                } else if constexpr (std::is_same_v<T, DetachAudioOperation>) {
                    return ApplyDetachAudio(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T, AddTrackOperation>) {
                    return ApplyAddTrack(candidate, value, generatedInverse,
                                         error, message);
                } else if constexpr (std::is_same_v<T, RemoveTrackOperation>) {
                    return ApplyRemoveTrack(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T, SetTrackLockOperation>) {
                    return ApplySetTrackLock(candidate, value, generatedInverse,
                                             error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetTrackSyncLockOperation>) {
                    return ApplySetTrackSyncLock(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetTrackOutputOperation>) {
                    return ApplySetTrackOutput(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    UpdateSequenceOperation>) {
                    return ApplyUpdateSequence(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetColorManagementOperation>) {
                    return ApplySetColorManagement(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, AddBinOperation>) {
                    return ApplyAddBin(candidate, value, generatedInverse,
                                       error, message);
                } else if constexpr (std::is_same_v<T, RemoveBinOperation>) {
                    return ApplyRemoveBin(candidate, value, generatedInverse,
                                          error, message);
                } else if constexpr (std::is_same_v<T, RenameBinOperation>) {
                    return ApplyRenameBin(candidate, value, generatedInverse,
                                          error, message);
                } else if constexpr (std::is_same_v<T, MoveBinOperation>) {
                    return ApplyMoveBin(candidate, value, generatedInverse,
                                        error, message);
                } else if constexpr (std::is_same_v<T, SetMediaBinOperation>) {
                    return ApplySetMediaBin(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetMediaWhiteBalanceOperation>) {
                    return ApplySetMediaWhiteBalance(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetMediaShotsOperation>) {
                    return ApplySetMediaShots(candidate, value,
                                              generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetMediaColorInputOperation>) {
                    return ApplySetMediaColorInput(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetMediaGenerationOperation>) {
                    return ApplySetMediaGeneration(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, IngestMediaOperation>) {
                    return ApplyIngestMedia(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<
                                         T, RemoveIngestedMediaOperation>) {
                    return ApplyRemoveIngestedMedia(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, AddMarkerOperation>) {
                    return ApplyAddMarker(candidate, value, generatedInverse,
                                          error, message);
                } else if constexpr (std::is_same_v<T, RemoveMarkerOperation>) {
                    return ApplyRemoveMarker(candidate, value, generatedInverse,
                                             error, message);
                } else if constexpr (std::is_same_v<T, UpdateMarkerOperation>) {
                    return ApplyUpdateMarker(candidate, value, generatedInverse,
                                             error, message);
                } else if constexpr (std::is_same_v<T,
                                                    AddTransitionOperation>) {
                    return ApplyAddTransition(candidate, value,
                                              generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, RemoveTransitionOperation>) {
                    return ApplyRemoveTransition(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, UpdateTransitionOperation>) {
                    return ApplyUpdateTransition(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, SetClipLinkOperation>) {
                    return ApplySetClipLink(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T, JoinClipOperation>) {
                    return ApplyJoin(candidate, value, generatedInverse, error,
                                     message);
                } else if constexpr (std::is_same_v<T,
                                                    SetClipEffectsOperation>) {
                    return ApplySetClipEffects(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetClipOpacityOperation>) {
                    return ApplySetClipOpacity(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, SetClipMaskOperation>) {
                    return ApplySetClipMask(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T, SetClipBlurOperation>) {
                    return ApplySetClipBlur(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T, SetClipSpeedOperation>) {
                    return ApplySetClipSpeed(candidate, value, generatedInverse,
                                             error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetClipFramingOperation>) {
                    return ApplySetClipFraming(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetClipAnimationOperation>) {
                    return ApplySetClipAnimation(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetClipEnabledOperation>) {
                    return ApplySetClipEnabled(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetSequenceLutOperation>) {
                    return ApplySetSequenceLut(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, SetClipAudioOperation>) {
                    return ApplySetClipAudio(candidate, value, generatedInverse,
                                             error, message);
                } else if constexpr (std::is_same_v<T,
                                                    AddCaptionStyleOperation>) {
                    return ApplyAddCaptionStyle(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, RemoveCaptionStyleOperation>) {
                    return ApplyRemoveCaptionStyle(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, UpdateCaptionStyleOperation>) {
                    return ApplyUpdateCaptionStyle(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetClipCaptionOperation>) {
                    return ApplySetClipCaption(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, AddMulticamGroupOperation>) {
                    return ApplyAddMulticamGroup(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, RemoveMulticamGroupOperation>) {
                    return ApplyRemoveMulticamGroup(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<
                                         T, SetMulticamActiveAngleOperation>) {
                    return ApplySetMulticamActiveAngle(
                        candidate, value, generatedInverse, error, message);
                } else if constexpr (std::is_same_v<T, RemoveWordsOperation>) {
                    return ApplyRemoveWords(candidate, value, generatedInverse,
                                            error, message);
                } else if constexpr (std::is_same_v<T,
                                                    SetMediaProxyOperation>) {
                    return ApplySetMediaProxy(candidate, value,
                                              generatedInverse, error, message);
                } else if constexpr (kIsMotionOperation<T>) {
                    // MOTION-2026-09 M3 -- MotionOperations.cc.
                    return ApplyMotionOperation(candidate, normalized,
                                                generatedInverse, error,
                                                message);
                } else {
                    static_assert(
                        kAlwaysFalse<T>,
                        "an Operation alternative has no apply branch");
                }
            },
            normalized);
        if (!applied) return false;
        // A batch's members each went through here already.
        if (!std::holds_alternative<BatchOperation>(normalized))
            KeepCaptionWordsOnTheirTime(document, candidate);
    } catch (const std::exception& exception) {
        Fail(EditError::ArithmeticError, exception.what(), error, message);
        return false;
    }
    document = std::move(candidate);
    operation = std::move(normalized);
    inverse = std::move(generatedInverse);
    error = EditError::None;
    message.clear();
    return true;
}

namespace {

bool ValidateProjectRelinkCandidate(const Project& project,
                                    const ProjectRelinkItem& item,
                                    std::string& message) {
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& value) { return value.id == item.media_id; });
    const auto source = std::find_if(
        project.sources.begin(), project.sources.end(),
        [&](const DocumentSource& value) { return value.id == item.media_id; });
    if (media == project.rushes.end() || source == project.sources.end()) {
        message = "unknown media or mounted source";
        return false;
    }
    if (item.stored_path.empty()) {
        message = "replacement path is empty";
        return false;
    }
    const LibraryMedia& replacement = item.replacement;
    if (!replacement.metadata_complete ||
        (media->has_video && !replacement.has_video) ||
        (!media->has_video && !replacement.has_audio)) {
        message = media->has_video
                      ? "replacement has no video for existing video media"
                      : "replacement has no audio for existing audio media";
        return false;
    }
    if (static_cast<int64_t>(replacement.rate.num) * source->rate.den !=
        static_cast<int64_t>(source->rate.num) * replacement.rate.den) {
        message = "replacement frame rate differs from the original";
        return false;
    }
    if (replacement.duration < source->duration) {
        message = "replacement is shorter than the original source";
        return false;
    }
    bool audioRequired = false;
    for (const DocumentSequence& timeline : project.timelines)
        for (const DocumentTrack& track : timeline.tracks)
            if (track.kind == "audio")
                for (const DocumentClip& clip : track.clips)
                    if (clip.source_id == item.media_id) audioRequired = true;
    if (audioRequired && !replacement.has_audio) {
        message = "replacement has no audio for existing audio clips";
        return false;
    }
    return true;
}

template <typename T>
void SetExactProject(T& operation, ExactProjectState state) {
    operation.exact_project_result = std::move(state);
}

const ExactProjectState* ExactStateOf(const ProjectOperation& operation) {
    return std::visit(
        [](const auto& value) -> const ExactProjectState* {
            return value.exact_project_result ? &*value.exact_project_result
                                              : nullptr;
        },
        operation);
}

// ADOBE-CLI-2026-09 -- link groups are identities too, although several
// clips intentionally share one. Sources remain project-owned and reused.
std::set<Ulid> TimelineOwnedIds(const DocumentSequence& timeline) {
    std::set<Ulid> ids{timeline.id};
    for (const auto& marker : timeline.markers) {
        ids.insert(marker.id);
    }
    for (const auto& transition : timeline.transitions) {
        ids.insert(transition.id);
    }
    for (const auto& style : timeline.caption_styles) {
        ids.insert(style.id);
    }
    for (const auto& group : timeline.multicam_groups) {
        ids.insert(group.id);
        for (const auto& angle : group.angles) {
            ids.insert(angle.id);
        }
    }
    for (const auto& track : timeline.tracks) {
        ids.insert(track.id);
        for (const auto& clip : track.clips) {
            ids.insert(clip.id);
            if (!clip.link_group_id.empty()) {
                ids.insert(clip.link_group_id);
            }
            for (const auto& effect : clip.effects) {
                ids.insert(effect.id);
            }
        }
    }
    return ids;
}

bool CloneProjectTimeline(Project& project,
                          CloneProjectTimelineOperation& operation,
                          EditError& error, std::string& message) {
    const auto* source = project.FindTimeline(operation.source_timeline_id);
    if (source == nullptr) {
        Fail(EditError::UnknownSequence, "unknown source timeline", error,
             message);
        return false;
    }
    if (operation.name.empty()) {
        Fail(EditError::InvalidOperation, "clone name is empty", error,
             message);
        return false;
    }
    const auto owned = TimelineOwnedIds(*source);
    for (const auto& track : source->tracks) {
        for (const auto& clip : track.clips) {
            if (!clip.sync_anchor_clip_id.empty() &&
                (owned.count(clip.sync_anchor_clip_id) == 0u)) {
                Fail(EditError::UnknownClip,
                     "clone source contains a dangling sync anchor", error,
                     message);
                return false;
            }
        }
    }
    std::set<Ulid> occupied{project.id};
    for (const auto& media : project.rushes) {
        occupied.insert(media.id);
    }
    for (const auto& media : project.sources) {
        occupied.insert(media.id);
    }
    for (const auto& bin : project.bins) {
        occupied.insert(bin.id);
    }
    for (const auto& timeline : project.timelines) {
        const auto ids = TimelineOwnedIds(timeline);
        occupied.insert(ids.begin(), ids.end());
    }
    if (operation.id_remap.empty()) {
        auto reserved = occupied;
        for (const auto& id : owned) {
            Ulid replacement;
            if (id == source->id && !operation.timeline_id.empty()) {
                replacement = operation.timeline_id;
            } else {
                do {
                    replacement = GenerateUlid();
                } while (reserved.count(replacement) != 0u);
            }
            operation.id_remap.emplace(id, replacement);
            reserved.insert(replacement);
        }
    }
    if (operation.id_remap.size() != owned.size()) {
        Fail(EditError::InvalidOperation, "clone ID map is incomplete", error,
             message);
        return false;
    }
    for (const auto& id : owned) {
        const auto found = operation.id_remap.find(id);
        if (found == operation.id_remap.end() || !IsValidUlid(found->second) ||
            !occupied.insert(found->second).second) {
            Fail(EditError::DuplicateId, "clone ID map contains a collision",
                 error, message);
            return false;
        }
    }
    const auto& remap = operation.id_remap;
    const Ulid target = remap.at(source->id);
    if (!operation.timeline_id.empty() && operation.timeline_id != target) {
        Fail(EditError::InvalidOperation,
             "clone timeline ID disagrees with map", error, message);
        return false;
    }
    operation.timeline_id = target;
    DocumentSequence clone = *source;
    const auto replace = [&](Ulid& id) {
        if (!id.empty()) {
            id = remap.at(id);
        }
    };
    replace(clone.id);
    clone.name = operation.name;
    for (auto& marker : clone.markers) {
        replace(marker.id);
    }
    for (auto& style : clone.caption_styles) {
        replace(style.id);
    }
    for (auto& transition : clone.transitions) {
        replace(transition.id);
        replace(transition.track_id);
        replace(transition.left_clip_id);
        replace(transition.right_clip_id);
    }
    for (auto& track : clone.tracks) {
        replace(track.id);
        for (auto& clip : track.clips) {
            replace(clip.id);
            replace(clip.link_group_id);
            replace(clip.sync_anchor_clip_id);
            replace(clip.caption_group_id);
            for (auto& effect : clip.effects) {
                replace(effect.id);
            }
        }
    }
    for (auto& group : clone.multicam_groups) {
        replace(group.id);
        replace(group.active_angle_id);
        for (auto& angle : group.angles) {
            replace(angle.id);
            replace(angle.clip_id);
        }
    }
    if (const auto found = project.bin_metadata.find(source->id);
        found != project.bin_metadata.end()) {
        project.bin_metadata[target] = found->second;
    }
    if (const auto found = project.timeline_bin_ids.find(source->id);
        found != project.timeline_bin_ids.end()) {
        project.timeline_bin_ids[target] = found->second;
    }
    project.timelines.push_back(std::move(clone));
    if (operation.make_active) {
        project.active_timeline_id = target;
    }
    return true;
}

}  // namespace

bool ApplyProjectOperation(Project& project, ProjectOperation& operation,
                           ProjectOperation& inverse, EditError& error,
                           std::string& message) {
    Project candidate = project;
    ProjectOperation normalized = operation;
    bool applied = std::visit(
        [&](auto& value) -> bool {
            if (value.exact_project_result) {
                const ExactProjectState& exact = *value.exact_project_result;
                if (exact.delta) {
                    if (!ApplyProjectDelta(candidate, *exact.delta, message)) {
                        error = EditError::ValidationFailed;
                        message =
                            "project history no longer matches: " + message;
                        return false;
                    }
                    return true;
                }
                Project restored;
                if (!Project::LoadFromString(exact.canonical_json, restored,
                                             message)) {
                    error = EditError::ParseError;
                    return false;
                }
                candidate = std::move(restored);
                return true;
            }
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddProjectTimelineOperation>) {
                if (value.name.empty() || value.width <= 0 ||
                    value.height <= 0 || value.frame_rate.num <= 0 ||
                    value.frame_rate.den <= 0) {
                    Fail(EditError::InvalidOperation,
                         "project timeline settings are invalid", error,
                         message);
                    return false;
                }
                if (value.timeline_id.empty())
                    value.timeline_id = GenerateUlid();
                if (value.video_track_id.empty())
                    value.video_track_id = GenerateUlid();
                if (value.audio_track_id.empty())
                    value.audio_track_id = GenerateUlid();
                if (!IsValidUlid(value.timeline_id) ||
                    !IsValidUlid(value.video_track_id) ||
                    !IsValidUlid(value.audio_track_id) ||
                    candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::DuplicateId,
                         "project timeline operation contains duplicate IDs",
                         error, message);
                    return false;
                }
                DocumentSequence timeline;
                timeline.id = value.timeline_id;
                timeline.name = value.name;
                timeline.width = value.width;
                timeline.height = value.height;
                timeline.frame_rate = value.frame_rate;
                timeline.tracks = DefaultTimelineTracks(value.video_track_id,
                                                        value.audio_track_id);
                candidate.timelines.push_back(std::move(timeline));
                return true;
            } else if constexpr (std::is_same_v<
                                     T, CloneProjectTimelineOperation>) {
                return CloneProjectTimeline(candidate, value, error, message);
            } else if constexpr (std::is_same_v<
                                     T,
                                     CreateProjectTimelineVariantsOperation>) {
                if (value.variants.empty()) {
                    Fail(EditError::InvalidOperation, "variant batch is empty",
                         error, message);
                    return false;
                }
                std::set<std::string> names;
                for (const auto& timeline : candidate.timelines) {
                    names.insert(timeline.name);
                }
                for (auto& variant : value.variants) {
                    if (!names.insert(variant.clone.name).second ||
                        variant.clone.make_active ||
                        variant.clone.exact_project_result ||
                        variant.replacements.empty()) {
                        Fail(EditError::InvalidOperation,
                             "variant needs a unique name and replacements; "
                             "activation and nested replay state are refused",
                             error, message);
                        return false;
                    }
                    if (!CloneProjectTimeline(candidate, variant.clone, error,
                                              message)) {
                        return false;
                    }
                    Document document =
                        candidate.MakeDocument(variant.clone.timeline_id);
                    EditLog edits;
                    for (const auto& replacement : variant.replacements) {
                        if (replacement.from.empty() ||
                            replacement.expected_count <= 0) {
                            Fail(EditError::InvalidOperation,
                                 "replacement needs nonempty text and a "
                                 "positive expected_count",
                                 error, message);
                            return false;
                        }
                        Ulid target;
                        if (!replacement.clip_id.empty()) {
                            const auto found = variant.clone.id_remap.find(
                                replacement.clip_id);
                            if (found == variant.clone.id_remap.end()) {
                                Fail(EditError::UnknownClip,
                                     "replacement clip is absent from template",
                                     error, message);
                                return false;
                            }
                            target = found->second;
                        }
                        int64_t count = 0;
                        std::vector<Operation> changes;
                        for (const auto& track : document.sequence.tracks) {
                            if (track.kind != "caption") {
                                continue;
                            }
                            for (const auto& clip : track.clips) {
                                if (!target.empty() && clip.id != target) {
                                    continue;
                                }
                                std::string text = clip.caption_text;
                                size_t position = 0;
                                int64_t matches = 0;
                                while ((position = text.find(replacement.from,
                                                             position)) !=
                                       std::string::npos) {
                                    text.replace(position,
                                                 replacement.from.size(),
                                                 replacement.to);
                                    position += replacement.to.size();
                                    ++matches;
                                }
                                count += matches;
                                if (matches > 0) {
                                    changes.emplace_back(
                                        SetClipCaptionOperation{
                                            clip.id, clip.caption_group_id,
                                            text});
                                }
                            }
                        }
                        if (count != replacement.expected_count) {
                            Fail(
                                EditError::ValidationFailed,
                                "caption replacement count mismatch in " +
                                    variant.clone.name + ": expected " +
                                    std::to_string(replacement.expected_count) +
                                    ", found " + std::to_string(count),
                                error, message);
                            return false;
                        }
                        for (auto& change : changes) {
                            if (!edits.Apply(document, std::move(change), error,
                                             message)) {
                                return false;
                            }
                        }
                    }
                    if (!candidate.CommitDocument(variant.clone.timeline_id,
                                                  document, message)) {
                        error = EditError::ValidationFailed;
                        return false;
                    }
                }
                return true;
            } else if constexpr (
                std::is_same_v<T, CreateProjectTimelineFromSegmentsOperation>) {
                if (value.name.empty() || value.width <= 0 ||
                    value.height <= 0 || value.frame_rate.num <= 0 ||
                    value.frame_rate.den <= 0 || value.segments.empty()) {
                    Fail(EditError::InvalidOperation,
                         "project short timeline settings are invalid", error,
                         message);
                    return false;
                }
                if (value.timeline_id.empty())
                    value.timeline_id = GenerateUlid();
                if (value.video_track_id.empty())
                    value.video_track_id = GenerateUlid();
                if (value.audio_track_id.empty())
                    value.audio_track_id = GenerateUlid();
                // SYNCHRO-2026-10 SY3 -- a third track when a segment
                // carries a recorder's sound.
                const bool separate = std::any_of(
                    value.segments.begin(), value.segments.end(),
                    [](const ProjectTimelineSourceSegment& segment) {
                        return !segment.separate_source_id.empty();
                    });
                if (separate && value.separate_track_id.empty())
                    value.separate_track_id = GenerateUlid();
                if (!separate && !value.separate_track_id.empty()) {
                    Fail(EditError::InvalidOperation,
                         "project short names a separate-sound track but no "
                         "segment carries a separate sound",
                         error, message);
                    return false;
                }
                if (!IsValidUlid(value.timeline_id) ||
                    !IsValidUlid(value.video_track_id) ||
                    !IsValidUlid(value.audio_track_id) ||
                    (separate && !IsValidUlid(value.separate_track_id)) ||
                    candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::DuplicateId,
                         "project short operation contains duplicate IDs",
                         error, message);
                    return false;
                }
                DocumentSequence timeline;
                timeline.id = value.timeline_id;
                timeline.name = value.name;
                timeline.width = value.width;
                timeline.height = value.height;
                timeline.frame_rate = value.frame_rate;
                DocumentTrack video{value.video_track_id, "video", 0, {}};
                DocumentTrack audio{value.audio_track_id, "audio", 1, {}};
                DocumentTrack recorder{value.separate_track_id, "audio", 2, {}};
                // SY2 -- the camera's own sound stays, linked, on its track
                // turned off (decided by the author on 8 October 2026).
                audio.muted = separate;
                RationalTime cursor{0, value.frame_rate.num};
                std::vector<Ulid> generatedIds{value.timeline_id,
                                               value.video_track_id,
                                               value.audio_track_id};
                if (separate) generatedIds.push_back(value.separate_track_id);
                for (ProjectTimelineSourceSegment& segment : value.segments) {
                    const auto source = std::find_if(
                        candidate.sources.begin(), candidate.sources.end(),
                        [&](const DocumentSource& item) {
                            return item.id == segment.source_id;
                        });
                    if (source == candidate.sources.end() ||
                        segment.source_in < RationalTime{0, 1} ||
                        !(RationalTime{0, 1} < segment.duration) ||
                        source->duration <
                            segment.source_in.add(segment.duration)) {
                        Fail(EditError::InvalidOperation,
                             "project short contains an invalid source range",
                             error, message);
                        return false;
                    }
                    if (segment.video_clip_id.empty())
                        segment.video_clip_id = GenerateUlid();
                    if (segment.audio_clip_id.empty())
                        segment.audio_clip_id = GenerateUlid();
                    if (segment.link_group_id.empty())
                        segment.link_group_id = GenerateUlid();
                    const bool carriesSound =
                        !segment.separate_source_id.empty();
                    if (carriesSound) {
                        const auto heard = std::find_if(
                            candidate.sources.begin(), candidate.sources.end(),
                            [&](const DocumentSource& item) {
                                return item.id == segment.separate_source_id;
                            });
                        if (heard == candidate.sources.end() ||
                            heard->id == segment.source_id ||
                            segment.separate_source_in < RationalTime{0, 1} ||
                            heard->duration < segment.separate_source_in.add(
                                                  segment.duration)) {
                            Fail(EditError::InvalidOperation,
                                 "project short contains an invalid separate "
                                 "sound range",
                                 error, message);
                            return false;
                        }
                        if (segment.separate_clip_id.empty())
                            segment.separate_clip_id = GenerateUlid();
                    } else if (!segment.separate_clip_id.empty() ||
                               segment.separate_source_in !=
                                   RationalTime{0, 1}) {
                        Fail(EditError::InvalidOperation,
                             "project short segment has a separate-sound clip "
                             "but no separate source",
                             error, message);
                        return false;
                    }
                    std::vector<Ulid> segmentIds{segment.video_clip_id,
                                                 segment.audio_clip_id,
                                                 segment.link_group_id};
                    if (carriesSound)
                        segmentIds.push_back(segment.separate_clip_id);
                    for (const Ulid& id : segmentIds) {
                        if (!IsValidUlid(id) ||
                            std::find(generatedIds.begin(), generatedIds.end(),
                                      id) != generatedIds.end()) {
                            Fail(EditError::DuplicateId,
                                 "project short contains duplicate clip IDs",
                                 error, message);
                            return false;
                        }
                        generatedIds.push_back(id);
                    }
                    DocumentClip videoClip;
                    videoClip.id = segment.video_clip_id;
                    videoClip.source_id = segment.source_id;
                    videoClip.source_in = segment.source_in;
                    videoClip.duration = segment.duration;
                    videoClip.timeline_in = cursor;
                    videoClip.include_audio = false;
                    videoClip.link_group_id = segment.link_group_id;
                    DocumentClip audioClip = videoClip;
                    audioClip.id = segment.audio_clip_id;
                    if (carriesSound) {
                        // SY3 -- anchored on the picture, so that
                        // timeline_stats measures the recorder's drift
                        // against it: the phase difference it had in the
                        // timeline it was read from. Only on these
                        // segments, so a short without a separate sound
                        // builds the same document as before.
                        DocumentClip heardClip = videoClip;
                        heardClip.id = segment.separate_clip_id;
                        heardClip.source_id = segment.separate_source_id;
                        heardClip.source_in = segment.separate_source_in;
                        videoClip.sync_anchor_clip_id = videoClip.id;
                        audioClip.sync_anchor_clip_id = videoClip.id;
                        heardClip.sync_anchor_clip_id = videoClip.id;
                        heardClip.sync_reference_delta =
                            PhaseOf(heardClip).sub(PhaseOf(videoClip));
                        recorder.clips.push_back(std::move(heardClip));
                    }
                    video.clips.push_back(std::move(videoClip));
                    audio.clips.push_back(std::move(audioClip));
                    cursor = cursor.add(segment.duration);
                }
                timeline.tracks = {std::move(video), std::move(audio)};
                if (separate) timeline.tracks.push_back(std::move(recorder));
                candidate.timelines.push_back(std::move(timeline));
                if (value.make_active)
                    candidate.active_timeline_id = value.timeline_id;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, RemoveProjectTimelineOperation>) {
                const auto found = std::find_if(
                    candidate.timelines.begin(), candidate.timelines.end(),
                    [&](const DocumentSequence& timeline) {
                        return timeline.id == value.timeline_id;
                    });
                if (found == candidate.timelines.end()) {
                    Fail(EditError::UnknownSequence,
                         "unknown project timeline_id '" + value.timeline_id +
                             "'",
                         error, message);
                    return false;
                }
                if (candidate.timelines.size() == 1) {
                    Fail(EditError::InvalidOperation,
                         "a project must contain at least one timeline", error,
                         message);
                    return false;
                }
                candidate.timelines.erase(found);
                candidate.timeline_bin_ids.erase(value.timeline_id);
                candidate.bin_metadata.erase(value.timeline_id);
                if (candidate.active_timeline_id == value.timeline_id)
                    candidate.active_timeline_id =
                        candidate.timelines.front().id;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, SetProjectBinMetadataOperation>) {
                const bool exists =
                    candidate.FindTimeline(value.item_id) ||
                    std::any_of(candidate.rushes.begin(),
                                candidate.rushes.end(),
                                [&](const LibraryMedia& media) {
                                    return media.id == value.item_id;
                                });
                if (!exists) {
                    Fail(EditError::UnknownMedia,
                         "unknown project bin item '" + value.item_id + "'",
                         error, message);
                    return false;
                }
                if (value.metadata.rating > 5) {
                    Fail(EditError::InvalidOperation,
                         "project bin rating must be between 0 and 5", error,
                         message);
                    return false;
                }
                candidate.bin_metadata[value.item_id] = value.metadata;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, SetProjectTimelineBinOperation>) {
                if (!candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::UnknownSequence,
                         "unknown project timeline_id '" + value.timeline_id +
                             "'",
                         error, message);
                    return false;
                }
                if (!value.bin_id.empty() &&
                    std::none_of(candidate.bins.begin(), candidate.bins.end(),
                                 [&](const DocumentBin& bin) {
                                     return bin.id == value.bin_id;
                                 })) {
                    Fail(EditError::UnknownBin,
                         "unknown bin_id '" + value.bin_id + "'", error,
                         message);
                    return false;
                }
                if (value.bin_id.empty())
                    candidate.timeline_bin_ids.erase(value.timeline_id);
                else
                    candidate.timeline_bin_ids[value.timeline_id] =
                        value.bin_id;
                return true;
            } else if constexpr (std::is_same_v<T,
                                                RenameProjectItemOperation>) {
                if (value.name.empty() || value.name.size() > 128) {
                    Fail(EditError::InvalidOperation,
                         "project item name must contain between 1 and 128 "
                         "bytes",
                         error, message);
                    return false;
                }
                if (DocumentSequence* timeline =
                        candidate.FindTimeline(value.item_id)) {
                    timeline->name = value.name;
                    return true;
                }
                const auto rush = std::find_if(
                    candidate.rushes.begin(), candidate.rushes.end(),
                    [&](const LibraryMedia& media) {
                        return media.id == value.item_id;
                    });
                if (rush == candidate.rushes.end()) {
                    Fail(EditError::UnknownMedia,
                         "unknown project item_id '" + value.item_id + "'",
                         error, message);
                    return false;
                }
                candidate.bin_metadata[value.item_id].display_name = value.name;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, SetActiveProjectTimelineOperation>) {
                if (!candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::UnknownSequence,
                         "unknown timeline_id '" + value.timeline_id + "'",
                         error, message);
                    return false;
                }
                candidate.active_timeline_id = value.timeline_id;
                return true;
            } else if constexpr (std::is_same_v<T,
                                                RelinkProjectMediaOperation>) {
                if (value.replacements.empty()) {
                    Fail(EditError::InvalidOperation, "relink batch is empty",
                         error, message);
                    return false;
                }
                std::vector<Ulid> seen;
                for (const ProjectRelinkItem& item : value.replacements) {
                    if (std::find(seen.begin(), seen.end(), item.media_id) !=
                        seen.end()) {
                        Fail(EditError::InvalidOperation,
                             "relink batch contains duplicate media IDs", error,
                             message);
                        return false;
                    }
                    if (!ValidateProjectRelinkCandidate(candidate, item,
                                                        message)) {
                        error = EditError::InvalidOperation;
                        return false;
                    }
                    seen.push_back(item.media_id);
                    auto media = std::find_if(
                        candidate.rushes.begin(), candidate.rushes.end(),
                        [&](const LibraryMedia& value) {
                            return value.id == item.media_id;
                        });
                    auto source = std::find_if(
                        candidate.sources.begin(), candidate.sources.end(),
                        [&](const DocumentSource& value) {
                            return value.id == item.media_id;
                        });
                    const Ulid binId = media->bin_id;
                    *media = item.replacement;
                    media->id = item.media_id;
                    media->path = item.stored_path;
                    media->bin_id = binId;
                    media->proxy_path.clear();
                    source->id = item.media_id;
                    source->path = item.stored_path;
                    source->rate = item.replacement.rate;
                    source->duration = item.replacement.duration;
                }
                return true;
            } else if constexpr (std::is_same_v<
                                     T, ChooseProjectTimelineOperation>) {
                // MOTIF6 -- DEC41. Every timeline named is one the project
                // holds now: a choice among timelines nobody can see is not
                // a choice anyone made.
                if (!candidate.FindTimeline(value.timeline_id)) {
                    Fail(EditError::UnknownSequence,
                         "unknown timeline_id '" + value.timeline_id +
                             "': a choice keeps a timeline the project holds",
                         error, message);
                    return false;
                }
                if (value.alternative_timeline_ids.empty() ||
                    value.alternative_timeline_ids.size() >
                        kMaxChoiceAlternatives) {
                    Fail(EditError::InvalidOperation,
                         "a choice names the timelines it was made among: "
                         "between 1 and " +
                             std::to_string(kMaxChoiceAlternatives) +
                             " alternatives",
                         error, message);
                    return false;
                }
                std::set<Ulid> named{value.timeline_id};
                for (const Ulid& alternative : value.alternative_timeline_ids) {
                    if (!candidate.FindTimeline(alternative)) {
                        Fail(EditError::UnknownSequence,
                             "unknown alternative timeline_id '" + alternative +
                                 "'",
                             error, message);
                        return false;
                    }
                    if (!named.insert(alternative).second) {
                        Fail(EditError::InvalidOperation,
                             "a choice names each timeline once, the one "
                             "kept apart from its alternatives",
                             error, message);
                        return false;
                    }
                }
                if (value.make_active) {
                    candidate.active_timeline_id = value.timeline_id;
                }
                return true;
            }
            return false;
        },
        normalized);
    if (!applied) return false;
    if (!candidate.Validate(message)) {
        error = EditError::ValidationFailed;
        return false;
    }
    // S6 -- SAVING_ROADMAP.md. Record what changed rather than both states.
    // A replayed entry keeps the delta it already carries; one that arrived
    // with a whole state (an older journal, or a caller that assembled the
    // result itself) is recorded as the delta it amounts to. Only a change
    // a delta cannot express keeps whole states, as every entry used to.
    inverse = normalized;
    ProjectDelta delta;
    std::string deltaError;
    if (ComputeProjectDelta(project, candidate, delta, deltaError)) {
        std::visit(
            [&](auto& value) {
                SetExactProject(value,
                                {std::string(), ReverseProjectDelta(delta)});
            },
            inverse);
        const ExactProjectState* carried = ExactStateOf(normalized);
        if (!carried || !carried->delta)
            std::visit(
                [&](auto& value) {
                    SetExactProject(value, {std::string(), std::move(delta)});
                },
                normalized);
    } else {
        std::visit(
            [&](auto& value) {
                SetExactProject(value, {project.SaveToString(), std::nullopt});
            },
            inverse);
        if (!ExactStateOf(normalized))
            std::visit(
                [&](auto& value) {
                    SetExactProject(value,
                                    {candidate.SaveToString(), std::nullopt});
                },
                normalized);
    }
    project = std::move(candidate);
    operation = std::move(normalized);
    error = EditError::None;
    message.clear();
    return true;
}

bool CompactLegacyProjectEntry(ProjectOperation& operation,
                               ProjectOperation& inverse) {
    const ExactProjectState* after = ExactStateOf(operation);
    const ExactProjectState* before = ExactStateOf(inverse);
    if (!after || !before || after->delta || before->delta) return false;
    Project beforeProject;
    Project afterProject;
    ProjectDelta delta;
    std::string error;
    if (!Project::LoadFromString(before->canonical_json, beforeProject,
                                 error) ||
        !Project::LoadFromString(after->canonical_json, afterProject, error) ||
        !ComputeProjectDelta(beforeProject, afterProject, delta, error))
        return false;
    std::visit(
        [&](auto& value) {
            SetExactProject(value, {std::string(), ReverseProjectDelta(delta)});
        },
        inverse);
    std::visit(
        [&](auto& value) {
            SetExactProject(value, {std::string(), std::move(delta)});
        },
        operation);
    return true;
}
