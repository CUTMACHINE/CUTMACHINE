#include "SeparateSound.h"

#include "ClipSpeed.h"
#include "MotionClip.h"
#include "Ulid.h"

#include <algorithm>

namespace {

bool Refuse(EditError code, const std::string& text, EditError& error,
            std::string& message) {
    error = code;
    message = text;
    return false;
}

// `time` on the sequence's frames, rounded up or down: num/den frames per
// second, exact.
RationalTime OnFrame(const RationalTime& time, const MediaRate& rate, bool up) {
    const __int128 scaled = static_cast<__int128>(time.value) * rate.num;
    const __int128 per = static_cast<__int128>(time.rate) * rate.den;
    __int128 frames = scaled / per;
    if (scaled % per != 0 && (scaled < 0) != up) {
        frames += up ? 1 : -1;
    }
    return RationalTime{static_cast<int64_t>(frames) * rate.den, rate.num};
}

// Decided by the author on 8 October 2026: the camera's own sound, linked to the
// picture, stays where it is and its track is turned off. Nothing when the
// picture has no linked sound of its own source, or its track is off
// already. `soundTrackId` is the separate sound's: never the one muted.
void MuteCameraSound(const Document& document, const DocumentClip& picture,
                     const Ulid& soundTrackId, BatchOperation& batch,
                     SeparateSoundPlacement& placement) {
    if (picture.link_group_id.empty()) {
        return;
    }
    for (const DocumentTrack& candidate : document.sequence.tracks) {
        if (candidate.kind != "audio" || candidate.id == soundTrackId) {
            continue;
        }
        const bool holds = std::any_of(
            candidate.clips.begin(), candidate.clips.end(),
            [&](const DocumentClip& clip) {
                return clip.link_group_id == picture.link_group_id &&
                       clip.source_id == picture.source_id;
            });
        if (!holds) {
            continue;
        }
        if (!candidate.muted) {
            SetTrackOutputOperation mute;
            mute.track_id = candidate.id;
            mute.visible = candidate.visible;
            mute.muted = true;
            mute.solo = candidate.solo;
            batch.operations.push_back(mute);
            placement.muted_track_id = candidate.id;
        }
        return;
    }
}

// The picture a separate sound goes under: a clip of a rush on a video
// track, at its own speed.
bool CheckPicture(const Document& document, const Ulid& clipId,
                  const std::string& tool, const DocumentClip*& picture,
                  EditError& error, std::string& message) {
    picture = document.FindClip(clipId);
    const DocumentTrack* track = document.FindTrackForClip(clipId);
    if (picture == nullptr || track == nullptr) {
        return Refuse(EditError::UnknownClip,
                      "unknown clip_id '" + clipId + "'", error, message);
    }
    if (track->kind != "video" || IsMotionClip(*picture)) {
        return Refuse(EditError::InvalidOperation,
                      "a separate sound is laid under a picture clip of a "
                      "rush; clip '" +
                          picture->id + "' is not one",
                      error, message);
    }
    if (ClipHasSpeed(*picture)) {
        return Refuse(EditError::InvalidOperation,
                      ClipSpeedRefusal(*picture, tool), error, message);
    }
    return true;
}

}  // namespace

bool BuildSeparateSoundSync(const Document& document,
                            const SeparateSoundRequest& request,
                            BatchOperation& batch,
                            SeparateSoundPlacement& placement, EditError& error,
                            std::string& message) {
    batch = {};
    placement = {};
    const DocumentClip* picture = nullptr;
    if (!CheckPicture(document, request.picture_clip_id, "sync_audio", picture,
                      error, message)) {
        return false;
    }
    const DocumentSource* recorder =
        document.FindSource(request.recorder_source_id);
    if (recorder == nullptr) {
        return Refuse(EditError::UnknownSource,
                      "unknown media_id '" + request.recorder_source_id + "'",
                      error, message);
    }
    if (recorder->id == picture->source_id) {
        return Refuse(EditError::InvalidOperation,
                      "the separate sound must be another recording than the "
                      "picture's own rush",
                      error, message);
    }
    const DocumentTrack* track = document.FindTrack(request.track_id);
    if (track == nullptr) {
        return Refuse(EditError::UnknownTrack,
                      "unknown track_id '" + request.track_id + "'", error,
                      message);
    }
    if (track->kind != "audio") {
        return Refuse(EditError::InvalidOperation,
                      "track '" + track->id +
                          "' is not an audio track: the sound goes on one",
                      error, message);
    }
    if (track->locked) {
        return Refuse(EditError::LockedTrack,
                      "track '" + track->id + "' is locked", error, message);
    }

    // The picture shows its source over [in, in + duration); the recorder
    // holds the picture source's times [offset, offset + its duration).
    const RationalTime pictureIn = picture->source_in;
    const RationalTime pictureEnd = picture->source_in.add(picture->duration);
    const RationalTime heardFrom = request.offset;
    const RationalTime heardTo = request.offset.add(recorder->duration);
    const RationalTime first = std::max(pictureIn, heardFrom);
    const RationalTime last = std::min(pictureEnd, heardTo);
    if (!(first < last)) {
        return Refuse(EditError::SourceOutOfBounds,
                      "the recorder holds none of what clip '" + picture->id +
                          "' shows at this offset",
                      error, message);
    }
    const MediaRate rate = document.sequence.frame_rate;
    const RationalTime start =
        OnFrame(picture->timeline_in.add(first.sub(pictureIn)), rate, true);
    const RationalTime end =
        OnFrame(picture->timeline_in.add(last.sub(pictureIn)), rate, false);
    if (!(start < end)) {
        return Refuse(EditError::SourceOutOfBounds,
                      "the recorder holds less than a frame of clip '" +
                          picture->id + "'",
                      error, message);
    }
    placement.clip_id = GenerateUlid();
    placement.timeline_in = start;
    placement.duration = end.sub(start);
    // The picture source time the new clip starts on, read on the
    // recorder's own time: the sample the measure found stays.
    placement.source_in =
        pictureIn.add(start.sub(picture->timeline_in)).sub(request.offset);

    PastedClip pasted;
    pasted.copied_clip_id = GenerateUlid();
    pasted.track_id = track->id;
    pasted.source_id = recorder->id;
    pasted.source_in = placement.source_in;
    pasted.duration = placement.duration;
    pasted.timeline_in = placement.timeline_in;
    pasted.clip_id = placement.clip_id;
    PasteClipsOperation paste;
    paste.clips = {pasted};
    batch.operations.push_back(paste);

    const Ulid group = picture->link_group_id.empty() ? GenerateUlid()
                                                      : picture->link_group_id;
    SetClipLinkOperation link;
    link.first_clip_id = picture->id;
    link.second_clip_id = placement.clip_id;
    link.first_group_id = group;
    link.second_group_id = group;
    batch.operations.push_back(link);

    if (request.mute_camera_sound) {
        MuteCameraSound(document, *picture, track->id, batch, placement);
    }
    batch.label = "Son séparé posé sous le plan";
    return true;
}

SeparateSoundPairFault CheckSeparateSoundPair(const Document& document,
                                              const Ulid& pictureClipId,
                                              const Ulid& soundClipId) {
    using Fault = SeparateSoundPairFault;
    const DocumentClip* picture = document.FindClip(pictureClipId);
    const DocumentTrack* pictureTrack =
        document.FindTrackForClip(pictureClipId);
    if (picture == nullptr || pictureTrack == nullptr) {
        return Fault::UnknownPicture;
    }
    if (pictureTrack->kind != "video" || IsMotionClip(*picture)) {
        return Fault::NotAPicture;
    }
    if (ClipHasSpeed(*picture)) {
        return Fault::PictureSpeed;
    }
    const DocumentClip* sound = document.FindClip(soundClipId);
    const DocumentTrack* track = document.FindTrackForClip(soundClipId);
    if (sound == nullptr || track == nullptr) {
        return Fault::UnknownSound;
    }
    if (track->kind != "audio" || IsMotionClip(*sound)) {
        return Fault::NotASound;
    }
    if (sound->source_id == picture->source_id) {
        return Fault::SameSource;
    }
    if (ClipHasSpeed(*sound)) {
        return Fault::SoundSpeed;
    }
    if (track->locked || pictureTrack->locked) {
        return Fault::LockedTrack;
    }
    // Linked to something else than this picture: which link wins is not
    // guessed.
    if (!sound->link_group_id.empty() &&
        sound->link_group_id != picture->link_group_id) {
        for (const DocumentTrack& candidate : document.sequence.tracks) {
            for (const DocumentClip& clip : candidate.clips) {
                if (clip.id != sound->id &&
                    clip.link_group_id == sound->link_group_id) {
                    return Fault::LinkedElsewhere;
                }
            }
        }
    }
    return Fault::None;
}

bool BuildSeparateSoundAlign(const Document& document,
                             const SeparateSoundAlignRequest& request,
                             BatchOperation& batch,
                             SeparateSoundPlacement& placement,
                             EditError& error, std::string& message) {
    batch = {};
    placement = {};
    using Fault = SeparateSoundPairFault;
    const Ulid& pictureId = request.picture_clip_id;
    const Ulid& soundId = request.sound_clip_id;
    switch (CheckSeparateSoundPair(document, pictureId, soundId)) {
        case Fault::None:
            break;
        case Fault::UnknownPicture:
            return Refuse(EditError::UnknownClip,
                          "unknown clip_id '" + pictureId + "'", error,
                          message);
        case Fault::NotAPicture:
            return Refuse(EditError::InvalidOperation,
                          "a separate sound is laid under a picture clip of "
                          "a rush; clip '" +
                              pictureId + "' is not one",
                          error, message);
        case Fault::PictureSpeed:
            return Refuse(
                EditError::InvalidOperation,
                ClipSpeedRefusal(*document.FindClip(pictureId), "sync_audio"),
                error, message);
        case Fault::UnknownSound:
            return Refuse(EditError::UnknownClip,
                          "unknown sound_clip_id '" + soundId + "'", error,
                          message);
        case Fault::NotASound:
            return Refuse(EditError::InvalidOperation,
                          "clip '" + soundId +
                              "' is not a sound: the recorder's sound is a "
                              "clip of a media on an audio track",
                          error, message);
        case Fault::SameSource:
            return Refuse(EditError::InvalidOperation,
                          "clip '" + soundId +
                              "' is the picture's own sound; the separate "
                              "sound must be another recording than the "
                              "picture's rush",
                          error, message);
        case Fault::SoundSpeed:
            return Refuse(
                EditError::InvalidOperation,
                ClipSpeedRefusal(*document.FindClip(soundId), "sync_audio"),
                error, message);
        case Fault::LockedTrack:
            return Refuse(EditError::LockedTrack,
                          "the track of clip '" + pictureId + "' or of clip '" +
                              soundId + "' is locked",
                          error, message);
        case Fault::LinkedElsewhere:
            return Refuse(EditError::InvalidOperation,
                          "clip '" + soundId +
                              "' is linked to another clip; unlink it before "
                              "aligning it to clip '" +
                              pictureId + "'",
                          error, message);
    }
    const DocumentClip* picture = document.FindClip(pictureId);
    const DocumentClip* sound = document.FindClip(soundId);
    const DocumentTrack* track = document.FindTrackForClip(soundId);

    RationalTime start;
    RationalTime end;
    RationalTime exact;
    try {
        // Where the sound's first source sample is heard once in sync: the
        // picture shows its source time p at timeline_in + (p - source_in),
        // and the sound's media time r is the picture's r + offset.
        exact = picture->timeline_in.add(
            sound->source_in.add(request.offset).sub(picture->source_in));
        const RationalTime soundEnd = exact.add(sound->duration);
        const RationalTime pictureEnd =
            picture->timeline_in.add(picture->duration);
        const RationalTime first = std::max(exact, picture->timeline_in);
        const RationalTime last = std::min(soundEnd, pictureEnd);
        if (!(first < last)) {
            return Refuse(EditError::SourceOutOfBounds,
                          "in sync, clip '" + sound->id +
                              "' holds none of what clip '" + picture->id +
                              "' shows",
                          error, message);
        }
        const MediaRate rate = document.sequence.frame_rate;
        start = OnFrame(first, rate, true);
        end = OnFrame(last, rate, false);
        if (!(start < end)) {
            return Refuse(EditError::SourceOutOfBounds,
                          "in sync, clip '" + sound->id +
                              "' holds less than a frame of clip '" +
                              picture->id + "'",
                          error, message);
        }
    } catch (const std::exception& exception) {
        return Refuse(EditError::ArithmeticError, exception.what(), error,
                      message);
    }
    for (const DocumentClip& other : track->clips) {
        if (other.id != sound->id && other.timeline_in < end &&
            start < other.timeline_in.add(other.duration)) {
            return Refuse(EditError::Overlap,
                          "track '" + track->id + "' holds clip '" + other.id +
                              "' where clip '" + sound->id +
                              "' would be heard in sync",
                          error, message);
        }
    }

    placement.clip_id = sound->id;
    placement.timeline_in = start;
    placement.duration = end.sub(start);
    // The sample the sound's first frame now starts on: the sound keeps
    // its own media, slid by the measure, only cut inward.
    const RationalTime head = start.sub(exact);
    placement.source_in = sound->source_in.add(head);

    if (head != RationalTime{0, 1}) {
        batch.operations.push_back(
            TrimClipOperation{sound->id, TrimEdge::Head, head, std::nullopt});
    }
    const RationalTime tail = placement.duration.sub(sound->duration.sub(head));
    if (tail != RationalTime{0, 1}) {
        batch.operations.push_back(
            TrimClipOperation{sound->id, TrimEdge::Tail, tail, std::nullopt});
    }
    if (sound->timeline_in.add(head) != start) {
        MoveClipOperation move;
        move.clip_id = sound->id;
        move.track_id = track->id;
        move.timeline_in = start;
        batch.operations.push_back(move);
    }
    // Linked as the sync is: the picture the anchor, the sound's phase
    // recorded. Already so, nothing to write.
    const RationalTime phase =
        start.sub(placement.source_in)
            .sub(picture->timeline_in.sub(picture->source_in));
    const bool linked = !picture->link_group_id.empty() &&
                        sound->link_group_id == picture->link_group_id &&
                        picture->sync_anchor_clip_id == picture->id &&
                        sound->sync_anchor_clip_id == picture->id &&
                        sound->sync_reference_delta == phase;
    if (!batch.operations.empty() || !linked) {
        const Ulid group = picture->link_group_id.empty()
                               ? GenerateUlid()
                               : picture->link_group_id;
        SetClipLinkOperation link;
        link.first_clip_id = picture->id;
        link.second_clip_id = sound->id;
        link.first_group_id = group;
        link.second_group_id = group;
        batch.operations.push_back(link);
    }
    if (request.mute_camera_sound) {
        MuteCameraSound(document, *picture, track->id, batch, placement);
    }
    batch.label = "Son séparé aligné sur le plan";
    return true;
}

bool ResolveShortSegmentSound(const Document& document,
                              ProjectTimelineSourceSegment& segment,
                              EditError& error, std::string& message) {
    const RationalTime end = segment.source_in.add(segment.duration);
    const auto holds = [&](const DocumentClip& clip) {
        return clip.source_id == segment.source_id &&
               clip.source_in <= segment.source_in &&
               end <= clip.source_in.add(clip.duration) && !IsMotionClip(clip);
    };
    // The clip the range was read from: the picture when one holds it,
    // else a sound.
    const DocumentClip* read = nullptr;
    const DocumentTrack* readTrack = nullptr;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!holds(clip)) {
                continue;
            }
            if (read == nullptr ||
                (readTrack->kind != "video" && track.kind == "video")) {
                read = &clip;
                readTrack = &track;
            }
        }
    }
    if (read == nullptr || read->link_group_id.empty()) {
        return true;
    }
    // The other members of its link group, by role.
    std::vector<const DocumentClip*> pictures;
    std::vector<const DocumentClip*> sounds;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == read->id ||
                clip.link_group_id != read->link_group_id ||
                clip.source_id == read->source_id || IsMotionClip(clip)) {
                continue;
            }
            (track.kind == "video" ? pictures : sounds).push_back(&clip);
        }
    }
    const bool readPicture = readTrack->kind == "video";
    // Read on the picture: its recorder's sound, if any. Read on a sound:
    // the picture of another source it is linked to, if any.
    const std::vector<const DocumentClip*>& partners =
        readPicture ? sounds : pictures;
    if (partners.empty()) {
        return true;
    }
    // Read on a sound, the picture's own sound is expected beside it; a
    // sound of a third source is a second recorder.
    const bool secondRecorder =
        !readPicture &&
        std::any_of(sounds.begin(), sounds.end(),
                    [&](const DocumentClip* clip) {
                        return clip->source_id != partners.front()->source_id;
                    });
    if (partners.size() > 1 || secondRecorder) {
        return Refuse(EditError::InvalidOperation,
                      "clip '" + read->id +
                          "' is linked to more than one recording of another "
                          "source; unlink the one the short should not carry",
                      error, message);
    }
    const DocumentClip& partner = *partners.front();
    for (const DocumentClip* clip : {read, &partner}) {
        if (ClipHasSpeed(*clip)) {
            return Refuse(EditError::InvalidOperation,
                          ClipSpeedRefusal(*clip, "create_interview_short"),
                          error, message);
        }
    }
    const std::vector<LinkedWordRemoval> translated = TranslatedLinkedRemovals(
        document, *read, {WordRemovalRange{segment.source_in, end}});
    const auto moved = std::find_if(translated.begin(), translated.end(),
                                    [&](const LinkedWordRemoval& link) {
                                        return link.clip_id == partner.id &&
                                               link.ranges.size() == 1;
                                    });
    if (moved == translated.end()) {
        return Refuse(EditError::SourceOutOfBounds,
                      "clip '" + partner.id + "', linked to clip '" + read->id +
                          "', does not hold the whole range the short takes "
                          "from it; take a range both hold",
                      error, message);
    }
    const RationalTime partnerIn = moved->ranges.front().source_start;
    if (readPicture) {
        segment.separate_source_id = partner.source_id;
        segment.separate_source_in = partnerIn;
    } else {
        segment.separate_source_id = read->source_id;
        segment.separate_source_in = segment.source_in;
        segment.source_id = partner.source_id;
        segment.source_in = partnerIn;
    }
    return true;
}
