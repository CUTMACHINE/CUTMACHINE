// SYNCHRO-2026-10 SY2 -- docs/roadmap/SYNCHRO-2026-10.md. A recorder's sound
// laid under a picture clip at a measured offset: on the timeline's frames,
// its source_in on the sample, linked with its exact phase, the camera's
// sound kept on a track turned off, one undo restoring every byte; and,
// once laid, carried by a word cut and by create_interview_short (SY3).
// Portable: no file is decoded, the offset is given as SY1 would give it.

#include "EditLog.h"
#include "Operations.h"
#include "Project.h"
#include "SeparateSound.h"
#include "SeparateSoundGesture.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

const Ulid kCamera = "01K30000000000000000000Y01";
const Ulid kRecorder = "01K30000000000000000000Y02";
const Ulid kPicture = "01K30000000000000000000Y03";
const Ulid kCameraSound = "01K30000000000000000000Y04";
const Ulid kVideoTrack = "01K30000000000000000000Y05";
const Ulid kCameraTrack = "01K30000000000000000000Y06";
const Ulid kRecorderTrack = "01K30000000000000000000Y07";

// A picture clip 2 s into its rush, 4 s long, placed at 8 s; the camera's
// sound linked under it; an empty audio track for the recorder.
Document Interview() {
    Document document;
    document.sequence.id = "01K30000000000000000000Y08";
    document.sequence.frame_rate = {25, 1};
    document.sources = {
        {kCamera, "Media/camera.mov", {25, 1}, {1500, 25}},
        {kRecorder, "Media/recorder.wav", {25, 1}, {1500, 25}},
    };
    DocumentClip picture{kPicture, kCamera, {50, 25}, {100, 25}, {200, 25}};
    DocumentClip sound{kCameraSound, kCamera, {50, 25}, {100, 25}, {200, 25}};
    picture.link_group_id = "01K30000000000000000000Y09";
    picture.sync_anchor_clip_id = kPicture;
    sound.link_group_id = picture.link_group_id;
    sound.sync_anchor_clip_id = kPicture;
    const auto trackOf = [](const Ulid& id, const std::string& kind,
                            int32_t index, std::vector<DocumentClip> clips) {
        DocumentTrack track;
        track.id = id;
        track.kind = kind;
        track.index = index;
        track.clips = std::move(clips);
        return track;
    };
    document.sequence.tracks = {
        trackOf(kVideoTrack, "video", 0, {picture}),
        trackOf(kCameraTrack, "audio", 1, {sound}),
        trackOf(kRecorderTrack, "audio", 2, {}),
    };
    return document;
}

bool Apply(EditLog& log, Document& document, const Operation& operation,
           EditError& error, std::string& message) {
    return log.Apply(document, operation, error, message);
}

// The recorder 2.572 s ahead of the camera (offset -123 456 samples), laid
// under the picture as sync_audio does.
const RationalTime kAhead{123456, 48000};

Document Synced() {
    Document document = Interview();
    BatchOperation batch;
    SeparateSoundPlacement placement;
    EditError error = EditError::None;
    std::string message;
    EditLog log;
    if (!BuildSeparateSoundSync(document,
                                {kPicture, kRecorder, kRecorderTrack,
                                 RationalTime{-123456, 48000}, true},
                                batch, placement, error, message) ||
        !log.Apply(document, batch, error, message)) {
        Check(false, "the synced interview is built: " + message);
    }
    return document;
}

// SY3 -- create_interview_short on a rush already synced: which link it
// reads, where it lays the sound, and that the short keeps the phase.
void CheckShortKeepsSeparateSound() {
    EditError error = EditError::None;
    std::string message;
    const Document synced = Synced();

    // Read on the picture, 3 to 4 s into the camera's rush.
    ProjectTimelineSourceSegment onPicture{kCamera, {75, 25}, {25, 25}};
    Check(ResolveShortSegmentSound(synced, onPicture, error, message) &&
              onPicture.source_id == kCamera &&
              onPicture.source_in == RationalTime{75, 25} &&
              onPicture.separate_source_id == kRecorder &&
              onPicture.separate_source_in == RationalTime{75, 25}.add(kAhead),
          "a range read on the picture gains the recorder's sound, at the "
          "same instants: " +
              message);

    // Read on the recorder, the cleaner sound for Whisper and NeMo.
    ProjectTimelineSourceSegment onSound{
        kRecorder, RationalTime{75, 25}.add(kAhead), {25, 25}};
    Check(ResolveShortSegmentSound(synced, onSound, error, message) &&
              onSound.source_id == kCamera &&
              onSound.source_in == RationalTime{75, 25} &&
              onSound.separate_source_id == kRecorder &&
              onSound.separate_source_in == RationalTime{75, 25}.add(kAhead),
          "a range read on the recorder becomes the picture's, the "
          "recorder's kept beside it: " +
              message);

    // Nothing synced: the segment is left as it was given.
    ProjectTimelineSourceSegment alone{kCamera, {75, 25}, {25, 25}};
    Check(ResolveShortSegmentSound(Interview(), alone, error, message) &&
              alone.source_id == kCamera && alone.separate_source_id.empty(),
          "a rush without a separate sound is left alone");

    // A recorder started 3.01 s into the camera holds nothing of 2 to 3 s.
    {
        Document late = Interview();
        BatchOperation batch;
        SeparateSoundPlacement placement;
        EditLog log;
        Check(BuildSeparateSoundSync(
                  late,
                  {kPicture, kRecorder, kRecorderTrack,
                   RationalTime{50, 25}.add(RationalTime{101, 100}), true},
                  batch, placement, error, message) &&
                  log.Apply(late, batch, error, message),
              "a late recorder is laid: " + message);
        ProjectTimelineSourceSegment early{kCamera, {50, 25}, {25, 25}};
        Check(!ResolveShortSegmentSound(late, early, error, message) &&
                  error == EditError::SourceOutOfBounds,
              "a range the recorder does not wholly hold is refused, not "
              "laid half silent");
    }

    // A second recorder on the same picture: which one is not guessed.
    {
        Document twice = synced;
        const Ulid second = "01K30000000000000000000Y10";
        const Ulid secondTrack = "01K30000000000000000000Y11";
        twice.sources.push_back(
            {second, "Media/lavalier.wav", {25, 1}, {1500, 25}});
        DocumentClip lavalier = *twice.FindClip(kCameraSound);
        lavalier.id = "01K30000000000000000000Y12";
        lavalier.source_id = second;
        DocumentTrack track;
        track.id = secondTrack;
        track.kind = "audio";
        track.index = 3;
        track.clips = {lavalier};
        twice.sequence.tracks.push_back(track);
        ProjectTimelineSourceSegment segment{kCamera, {75, 25}, {25, 25}};
        Check(!ResolveShortSegmentSound(twice, segment, error, message) &&
                  error == EditError::InvalidOperation,
              "two recorders' sounds on one picture are refused");
    }

    // The short itself: the picture, the camera's sound on a track turned
    // off, the recorder's on a third track, linked and anchored on the
    // picture with the phase it had.
    Project project = Project::FromDocument(synced, "Synced interview");
    const std::string before = project.SaveToString();
    CreateProjectTimelineFromSegmentsOperation shortOperation;
    shortOperation.name = "Short synced";
    shortOperation.width = 1920;
    shortOperation.height = 1080;
    shortOperation.frame_rate = {25, 1};
    ProjectTimelineSourceSegment second{kCamera, {100, 25}, {50, 25}};
    Check(ResolveShortSegmentSound(synced, second, error, message),
          "the second range resolves: " + message);
    shortOperation.segments = {onPicture, second};
    ProjectEditLog log;
    Check(log.Apply(project, ProjectOperation{shortOperation}, error, message),
          "the short is created: " + message);
    const DocumentSequence* made = project.ActiveTimeline();
    const bool shaped =
        made != nullptr && made->tracks.size() == 3 && made->tracks[1].muted &&
        !made->tracks[2].muted && made->tracks[2].kind == "audio" &&
        made->tracks[0].clips.size() == 2 && made->tracks[2].clips.size() == 2;
    Check(shaped,
          "three tracks: picture, the camera's sound turned off, the "
          "recorder's");
    bool phased = shaped;
    for (size_t index = 0; phased && index < 2; ++index) {
        const DocumentClip& image = made->tracks[0].clips[index];
        const DocumentClip& camera = made->tracks[1].clips[index];
        const DocumentClip& heard = made->tracks[2].clips[index];
        const RationalTime drift =
            heard.timeline_in.sub(heard.source_in)
                .sub(image.timeline_in.sub(image.source_in))
                .sub(heard.sync_reference_delta);
        phased = heard.timeline_in == image.timeline_in &&
                 heard.duration == image.duration &&
                 heard.source_id == kRecorder &&
                 heard.source_in == image.source_in.add(kAhead) &&
                 heard.link_group_id == image.link_group_id &&
                 camera.link_group_id == image.link_group_id &&
                 heard.sync_anchor_clip_id == image.id &&
                 camera.sync_anchor_clip_id == image.id &&
                 drift == RationalTime{0, 1};
    }
    Check(phased,
          "each piece of the recorder's sound is under its piece of "
          "picture, on the same sample, linked, without drift");

    const auto& applied = std::get<CreateProjectTimelineFromSegmentsOperation>(
        log.AppliedEntries().back().op);
    const std::string written =
        SerializeProjectOperation(log.AppliedEntries().back().op);
    ProjectOperation decoded;
    Check(DeserializeProjectOperation(written, decoded, error, message) &&
              SerializeProjectOperation(decoded) == written,
          "the operation reads back to the same bytes: " + message);
    Check(written.find("\"separate_track_id\":\"" + applied.separate_track_id +
                       "\"") != std::string::npos &&
              written.find("\"separate_source_in\":{\"value\":") !=
                  std::string::npos,
          "the separate sound is frozen in the operation");
    Check(log.Undo(project, error, message) && project.SaveToString() == before,
          "one undo restores every byte");
    Check(log.Redo(project, error, message) &&
              project.ActiveTimeline() != nullptr &&
              project.ActiveTimeline()->tracks.size() == 3,
          "and redo builds the same short again");

    // A short without a separate sound is written as before.
    CreateProjectTimelineFromSegmentsOperation plain;
    plain.segments = {{kCamera, {75, 25}, {25, 25}}};
    const std::string plainText =
        SerializeProjectOperation(ProjectOperation{plain});
    Check(plainText.find("separate") == std::string::npos,
          "a short without a separate sound writes none of its fields");
}

// SY2, the window -- the recorder's sound already on the timeline, as
// Resolve's Auto Align Clips takes it: dropped whole on an audio track,
// selected with the picture, slid into sync and linked in one edit.
const Ulid kDropped = "01K30000000000000000000Y13";

Document Dropped(const RationalTime& at, const RationalTime& sourceIn,
                 const RationalTime& duration) {
    Document document = Interview();
    DocumentClip dropped{kDropped, kRecorder, sourceIn, duration, at};
    dropped.audio_gain_db = {-3, 1};
    dropped.audio_fade_in = {5, 25};
    document.FindTrack(kRecorderTrack)->clips = {dropped};
    return document;
}

void CheckAlign() {
    EditError error = EditError::None;
    std::string message;
    const RationalTime ahead{-123456, 48000};
    // The whole recording dropped at 0: once in sync it would start
    // 4.572 s before the picture and run 51 s past it. It keeps what lies
    // under the picture -- the very clip sync_audio lays.
    {
        Document document = Dropped({0, 1}, {0, 1}, {1500, 25});
        const std::string before = document.SaveToString();
        BatchOperation batch;
        SeparateSoundPlacement placement;
        Check(
            BuildSeparateSoundAlign(document, {kPicture, kDropped, ahead, true},
                                    batch, placement, error, message) &&
                batch.operations.size() == 5,
            "two trims, a move, a link and the camera's track turned off: " +
                message);
        EditLog log;
        Check(Apply(log, document, batch, error, message),
              "the alignment applies: " + message);
        const DocumentClip* heard = document.FindClip(kDropped);
        const DocumentClip* picture = document.FindClip(kPicture);
        Check(heard != nullptr && picture != nullptr &&
                  heard->timeline_in == picture->timeline_in &&
                  heard->duration == picture->duration &&
                  heard->source_in ==
                      RationalTime{50, 25}.add(RationalTime{123456, 48000}) &&
                  placement.source_in == heard->source_in &&
                  document.FindTrackForClip(kDropped)->id == kRecorderTrack,
              "the sound lies under the whole picture on its own track, its "
              "source_in on the measured sample");
        Check(heard != nullptr && heard->audio_gain_db.num == -3 &&
                  heard->audio_gain_db.den == 1,
              "it keeps its id and its gain");
        Document laid = Interview();
        BatchOperation sync;
        SeparateSoundPlacement synced;
        Check(BuildSeparateSoundSync(
                  laid, {kPicture, kRecorder, kRecorderTrack, ahead, true},
                  sync, synced, error, message) &&
                  synced.source_in == placement.source_in &&
                  synced.timeline_in == placement.timeline_in &&
                  synced.duration == placement.duration,
              "the same sound sync_audio lays from the media");
        Check(heard != nullptr && picture != nullptr &&
                  heard->link_group_id == picture->link_group_id &&
                  heard->sync_anchor_clip_id == kPicture &&
                  heard->sync_reference_delta ==
                      heard->timeline_in.sub(heard->source_in)
                          .sub(picture->timeline_in.sub(picture->source_in)),
              "linked to the picture's group with its exact phase");
        Check(document.FindTrack(kCameraTrack)->muted &&
                  placement.muted_track_id == kCameraTrack,
              "the camera's sound stays, on a track turned off");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores every byte");
        Check(log.Redo(document, error, message) &&
                  document.FindClip(kDropped)->timeline_in ==
                      RationalTime{200, 25},
              "and redo aligns it again");
        BatchOperation again;
        SeparateSoundPlacement same;
        Check(
            BuildSeparateSoundAlign(document, {kPicture, kDropped, ahead, true},
                                    again, same, error, message) &&
                again.operations.empty(),
            "aligned, linked and the camera off: nothing left to write");
        const std::string written = SerializeOperation(batch);
        Operation decoded;
        Check(DeserializeOperation(written, decoded, error, message) &&
                  SerializeOperation(decoded) == written,
              "the batch reads back to the same bytes: " + message);
    }

    // A short piece of recording that, in sync, starts 1.01 s into the
    // picture: cut inward to the next frame, never longer than it was.
    {
        const RationalTime late =
            RationalTime{50, 25}.add(RationalTime{101, 100});
        Document document = Dropped({500, 25}, {0, 1}, {50, 25});
        BatchOperation batch;
        SeparateSoundPlacement placement;
        Check(
            BuildSeparateSoundAlign(document, {kPicture, kDropped, late, false},
                                    batch, placement, error, message) &&
                placement.timeline_in == RationalTime{226, 25} &&
                placement.source_in == RationalTime{3, 100} &&
                placement.timeline_in.add(placement.duration) ==
                    RationalTime{275, 25} &&
                placement.muted_track_id.empty(),
            "the head on the next frame, the tail on the last whole one, "
            "the track left as it is when asked: " +
                message);
        EditLog log;
        Check(Apply(log, document, batch, error, message) &&
                  !document.FindTrack(kCameraTrack)->muted,
              "and it applies: " + message);
        const DocumentClip* heard = document.FindClip(kDropped);
        Check(heard != nullptr &&
                  heard->source_in.add(heard->duration) <= RationalTime{50, 25},
              "the sound reads no more of its media than it did");
    }

    // Refusals, before and after the offset.
    {
        Document document = Dropped({0, 1}, {0, 1}, {1500, 25});
        BatchOperation batch;
        SeparateSoundPlacement placement;
        Check(!BuildSeparateSoundAlign(document,
                                       {kPicture, kDropped, {100, 1}, true},
                                       batch, placement, error, message) &&
                  error == EditError::SourceOutOfBounds,
              "a sound that holds none of the picture once in sync");
        Check(CheckSeparateSoundPair(document, kPicture, kCameraSound) ==
                      SeparateSoundPairFault::SameSource &&
                  !BuildSeparateSoundAlign(
                      document, {kPicture, kCameraSound, ahead, true}, batch,
                      placement, error, message),
              "the camera's own sound is not a separate one");
        Check(CheckSeparateSoundPair(document, kDropped, kPicture) ==
                  SeparateSoundPairFault::NotAPicture,
              "a sound is not a picture");
        document.FindTrack(kRecorderTrack)->locked = true;
        Check(CheckSeparateSoundPair(document, kPicture, kDropped) ==
                      SeparateSoundPairFault::LockedTrack &&
                  !BuildSeparateSoundAlign(document,
                                           {kPicture, kDropped, ahead, true},
                                           batch, placement, error, message) &&
                  error == EditError::LockedTrack,
              "a locked track");
        document.FindTrack(kRecorderTrack)->locked = false;
        Document linked = document;
        linked.FindClip(kDropped)->link_group_id = "01K30000000000000000000Y14";
        DocumentClip partner{"01K30000000000000000000Y15",
                             kRecorder,
                             {0, 1},
                             {10, 25},
                             {1600, 25}};
        partner.link_group_id = "01K30000000000000000000Y14";
        linked.FindTrack(kCameraTrack)->clips.push_back(partner);
        Check(CheckSeparateSoundPair(linked, kPicture, kDropped) ==
                  SeparateSoundPairFault::LinkedElsewhere,
              "a sound linked to another clip");
        // Another clip where the sound would be heard in sync.
        DocumentClip music{"01K30000000000000000000Y16",
                           kRecorder,
                           {0, 1},
                           {25, 25},
                           {250, 25}};
        Document crowded = Dropped({1600, 25}, {0, 1}, {1000, 25});
        crowded.FindTrack(kRecorderTrack)
            ->clips.insert(crowded.FindTrack(kRecorderTrack)->clips.begin(),
                           music);
        const std::string before = crowded.SaveToString();
        Check(
            !BuildSeparateSoundAlign(crowded, {kPicture, kDropped, ahead, true},
                                     batch, placement, error, message) &&
                error == EditError::Overlap && crowded.SaveToString() == before,
            "a track already holding a clip there: refused, not covered");
    }
}

// The window's policy: which clips of a selection the gesture takes, and
// that every refusal is said.
void CheckGesture() {
    using ui::separate_sound::PickSoundPair;
    const Document document = Dropped({0, 1}, {0, 1}, {1500, 25});
    std::string refusal;
    const auto pair =
        PickSoundPair(document, {kPicture, kCameraSound, kDropped}, refusal);
    Check(pair && pair->picture_clip_id == kPicture &&
              pair->sound_clip_id == kDropped && refusal.empty(),
          "a linked selection's camera sound is left out of the pair");
    Check(!PickSoundPair(document, {kPicture}, refusal) && !refusal.empty(),
          "a picture alone asks for the sound");
    Check(!PickSoundPair(document, {kPicture, kCameraSound}, refusal) &&
              refusal.find("celui du plan") != std::string::npos,
          "the camera's own sound is named as such");
    Check(!PickSoundPair(document, {kDropped}, refusal) && !refusal.empty(),
          "a sound alone asks for the picture");
    Document twice = document;
    DocumentClip second = *document.FindClip(kDropped);
    second.id = "01K30000000000000000000Y17";
    second.timeline_in = {1600, 25};
    twice.FindTrack(kRecorderTrack)->clips.push_back(second);
    Check(!PickSoundPair(twice, {kPicture, kDropped, second.id}, refusal) &&
              !refusal.empty(),
          "two recorders' sounds: which one is not guessed");
    for (const SeparateSoundPairFault fault :
         {SeparateSoundPairFault::UnknownPicture,
          SeparateSoundPairFault::NotAPicture,
          SeparateSoundPairFault::PictureSpeed,
          SeparateSoundPairFault::UnknownSound,
          SeparateSoundPairFault::NotASound, SeparateSoundPairFault::SameSource,
          SeparateSoundPairFault::SoundSpeed,
          SeparateSoundPairFault::LockedTrack,
          SeparateSoundPairFault::LinkedElsewhere})
        Check(!ui::separate_sound::PairFaultText(fault).empty(),
              "every fault of a pair is said");
    Check(
        ui::separate_sound::PairFaultText(SeparateSoundPairFault::None).empty(),
        "and none when there is none");
    for (const char* code : {"NoOverlap", "WeakCorrelation", "AmbiguousPeak",
                             "ClockDrift", "InvalidDocument"})
        Check(ui::separate_sound::MeasureRefusalText(code, "detail")
                      .find("detail") == std::string::npos,
              std::string("the measure's ") + code + " is said in French");
    Check(ui::separate_sound::MeasureRefusalText(
              "IoError", "audio offset measure cancelled") ==
              "la mesure a été annulée",
          "a measure stopped from the task bar says so");
    Check(ui::separate_sound::MeasureRefusalText("Other", "detail")
                  .find("detail") != std::string::npos,
          "an unknown refusal keeps the engine's words");
    RationalTime read;
    std::string code;
    std::string detail;
    Check(ui::separate_sound::ReadMeasuredOffset(
              R"({"ok":true,"offset":{"value":-123456,"rate":48000}})", read,
              code, detail) &&
              read == RationalTime{-123456, 48000},
          "the measure's offset is read as the command writes it");
    Check(!ui::separate_sound::ReadMeasuredOffset(
              R"({"ok":false,"error":"AmbiguousPeak","detail":"a loop"})", read,
              code, detail) &&
              code == "AmbiguousPeak" && detail == "a loop",
          "and its refusal, by name");
    Check(!ui::separate_sound::ReadMeasuredOffset("not json", read, code,
                                                  detail) &&
              code == "IoError",
          "an unreadable answer is a refusal, not an offset");
    Check(
        ui::separate_sound::OffsetText({-48000, 48000}, {25, 1}) == "-25 im." &&
            ui::separate_sound::OffsetText({-123456, 48000}, {25, 1}) ==
                "-2,572 s, -123456 éch." &&
            ui::separate_sound::OffsetText({-24000, 48000}, {25, 1}) ==
                "-0,500 s, -24000 éch.",
        "an offset reads in frames when it is whole ones, else in seconds "
        "and samples");
}

}  // namespace

int main() {
    // The recorder started 2.572 s before the camera: offset -123 456
    // samples. The picture's 2 s into its rush are 4.572 s into the
    // recorder.
    {
        Document document = Interview();
        const std::string before = document.SaveToString();
        const SeparateSoundRequest request{kPicture, kRecorder, kRecorderTrack,
                                           RationalTime{-123456, 48000}, true};
        BatchOperation batch;
        SeparateSoundPlacement placement;
        EditError error = EditError::None;
        std::string message;
        Check(BuildSeparateSoundSync(document, request, batch, placement, error,
                                     message) &&
                  batch.operations.size() == 3,
              "paste, link and the camera's track turned off: " + message);
        EditLog log;
        Check(Apply(log, document, batch, error, message),
              "the batch applies: " + message);
        const DocumentClip* laid = document.FindClip(placement.clip_id);
        const DocumentClip* picture = document.FindClip(kPicture);
        Check(laid != nullptr && picture != nullptr &&
                  laid->timeline_in == picture->timeline_in &&
                  laid->duration == picture->duration &&
                  laid->source_in ==
                      RationalTime{50, 25}.add(RationalTime{123456, 48000}),
              "the sound lies under the whole picture, its source_in on the "
              "sample");
        Check(laid != nullptr && picture != nullptr &&
                  laid->link_group_id == picture->link_group_id &&
                  laid->sync_anchor_clip_id == kPicture &&
                  laid->sync_reference_delta ==
                      laid->timeline_in.sub(laid->source_in)
                          .sub(picture->timeline_in.sub(picture->source_in)),
              "linked to the picture's group, its exact phase recorded");
        const DocumentTrack* cameraTrack = document.FindTrack(kCameraTrack);
        Check(cameraTrack != nullptr && cameraTrack->muted &&
                  cameraTrack->clips.size() == 1 &&
                  placement.muted_track_id == kCameraTrack,
              "the camera's sound stays, linked, on a track turned off");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores every byte");
        Check(log.Redo(document, error, message) &&
                  document.FindClip(placement.clip_id) != nullptr,
              "and redo lays the same clip again");

        // SY3 -- the laid sound is cut with the picture, at the same
        // instants.
        RemoveWordsOperation cut;
        cut.clip_id = kPicture;
        cut.ranges = {{{75, 25}, {100, 25}}};
        AttachLinkedRemovals(document, *document.FindClip(kPicture), cut);
        Check(cut.linked_clip_ids == std::vector<Ulid>{kCameraSound} &&
                  cut.translated_links.size() == 1 &&
                  cut.translated_links[0].clip_id == placement.clip_id,
              "a word cut on the picture carries the camera's sound and the "
              "recorder's");
        Check(Apply(log, document, cut, error, message),
              "the cut applies: " + message);
        const DocumentTrack* recorderTrack = document.FindTrack(kRecorderTrack);
        const DocumentTrack* videoTrack = document.FindTrack(kVideoTrack);
        bool aligned = recorderTrack != nullptr && videoTrack != nullptr &&
                       recorderTrack->clips.size() == 2 &&
                       videoTrack->clips.size() == 2;
        for (size_t index = 0; aligned && index < 2; ++index) {
            const DocumentClip& image = videoTrack->clips[index];
            const DocumentClip& heard = recorderTrack->clips[index];
            aligned = image.timeline_in == heard.timeline_in &&
                      image.duration == heard.duration &&
                      heard.source_in ==
                          image.source_in.add(RationalTime{123456, 48000});
        }
        Check(aligned,
              "after the cut, each piece of sound is under its "
              "piece of picture, on the same sample");
    }

    // The recorder was started 1.01 s into the picture: the sound starts
    // on the next frame, 1.04 s in, 0.03 s into the recorder.
    {
        Document document = Interview();
        const SeparateSoundRequest request{
            kPicture, kRecorder, kRecorderTrack,
            RationalTime{50, 25}.add(RationalTime{101, 100}), false};
        BatchOperation batch;
        SeparateSoundPlacement placement;
        EditError error = EditError::None;
        std::string message;
        Check(BuildSeparateSoundSync(document, request, batch, placement, error,
                                     message) &&
                  batch.operations.size() == 2 &&
                  placement.timeline_in == RationalTime{226, 25} &&
                  placement.source_in == RationalTime{3, 100} &&
                  placement.timeline_in.add(placement.duration) ==
                      RationalTime{300, 25},
              "a late recorder starts on the next frame, and the track is "
              "left as it is when asked: " +
                  message);
    }

    // Refusals: a recorder that holds none of the picture, a track already
    // holding sound there, a locked track, a picture that is not one.
    {
        Document document = Interview();
        BatchOperation batch;
        SeparateSoundPlacement placement;
        EditError error = EditError::None;
        std::string message;
        Check(!BuildSeparateSoundSync(document,
                                      {kPicture, kRecorder, kRecorderTrack,
                                       RationalTime{100, 1}, true},
                                      batch, placement, error, message) &&
                  error == EditError::SourceOutOfBounds,
              "a recorder holding none of the picture is refused");
        Check(!BuildSeparateSoundSync(document,
                                      {kCameraSound, kRecorder, kRecorderTrack,
                                       RationalTime{0, 1}, true},
                                      batch, placement, error, message) &&
                  error == EditError::InvalidOperation,
              "a sound clip is not a picture to lay a sound under");
        Check(!BuildSeparateSoundSync(
                  document,
                  {kPicture, kRecorder, kVideoTrack, RationalTime{0, 1}, true},
                  batch, placement, error, message) &&
                  error == EditError::InvalidOperation,
              "the sound goes on an audio track");
        document.FindTrack(kRecorderTrack)->locked = true;
        Check(!BuildSeparateSoundSync(document,
                                      {kPicture, kRecorder, kRecorderTrack,
                                       RationalTime{0, 1}, true},
                                      batch, placement, error, message) &&
                  error == EditError::LockedTrack,
              "a locked track is refused");
        document.FindTrack(kRecorderTrack)->locked = false;
        // The camera's track already holds sound under the picture.
        Check(BuildSeparateSoundSync(
                  document,
                  {kPicture, kRecorder, kCameraTrack, RationalTime{0, 1}, true},
                  batch, placement, error, message),
              "the batch is built");
        const std::string before = document.SaveToString();
        EditLog log;
        Check(!log.Apply(document, batch, error, message) &&
                  error == EditError::Overlap &&
                  document.SaveToString() == before,
              "a track holding sound there refuses the paste, and nothing "
              "changes");
    }

    CheckShortKeepsSeparateSound();
    CheckAlign();
    CheckGesture();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All separate sound tests passed\n";
    return 0;
}
