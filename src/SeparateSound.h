#pragma once

// SYNCHRO-2026-10 SY2 -- docs/roadmap/SYNCHRO-2026-10.md. A recorder's sound
// laid under the picture it was taken with, linked, in one undo.
//
// The offset comes from SY1 (MeasureAudioOffset): where the recorder's first
// sample falls on the camera's own time. The sound is laid over the stretch
// of the picture the recorder holds, on the timeline's frames: a recorder
// that starts or stops inside the picture is cut inward to the next frame,
// and its source_in keeps the sample the measure found. Nothing new in the
// document: a paste that refuses to cover what the track already holds, a
// link to the picture's group, which records the exact phase, and -- decided
// by the author on 8 October 2026 -- the camera's own sound kept, linked, on its
// track turned off rather than removed.

#include "Document.h"
#include "Operations.h"

#include <cstdint>

struct SeparateSoundRequest {
    Ulid picture_clip_id;
    Ulid recorder_source_id;
    Ulid track_id;
    // Where the recorder's first sample falls on the picture source's time.
    RationalTime offset{0, 1};
    // Turn off the track of the picture's linked sound of its own source.
    bool mute_camera_sound = true;
};

struct SeparateSoundPlacement {
    Ulid clip_id;
    RationalTime source_in{0, 1};
    RationalTime duration{0, 1};
    RationalTime timeline_in{0, 1};
    // The camera sound's track the batch turns off; empty when none was.
    Ulid muted_track_id;
};

// The batch that lays and links the sound, or false with the reason. The
// new clip's id is chosen here so that the link can name it before the
// paste is applied.
bool BuildSeparateSoundSync(const Document& document,
                            const SeparateSoundRequest& request,
                            BatchOperation& batch,
                            SeparateSoundPlacement& placement, EditError& error,
                            std::string& message);

// SYNCHRO-2026-10 SY2, the window -- docs/roadmap/SYNCHRO-2026-10.md. The
// gesture Resolve has for two clips already on the timeline: select the
// picture and the recorder's sound, then Auto Align Clips, Based on
// Waveform (DaVinci Resolve 21.1 manual, chapter 45, pages 955-956), and
// Link (chapter 43, page 916). Resolve slides the sound and leaves it as
// long as it was; here, as BuildSeparateSoundSync lays it, the sound keeps
// what of its own range lies under the picture once in sync, on the
// timeline's frames, its source_in on the sample, and joins the picture's
// link group with its exact phase -- one undo for the slide, the link and
// the camera's track turned off. Nothing new in the document: two trims, a
// move, a link and a track's output, in one BatchOperation. The sound keeps
// its id, its gain and its fades.
struct SeparateSoundAlignRequest {
    Ulid picture_clip_id;
    Ulid sound_clip_id;
    // Where the sound's media starts on the picture's media time, as
    // MeasureAudioOffset gives it with the picture's media as reference.
    RationalTime offset{0, 1};
    bool mute_camera_sound = true;
};

// Why two clips cannot be aligned, whatever the offset: what the window can
// say before it decodes any sound, in its own words.
enum class SeparateSoundPairFault : uint8_t {
    None,
    UnknownPicture,
    // Not a clip of a rush on a video track: a sound, a carton, a motion
    // clip.
    NotAPicture,
    PictureSpeed,
    UnknownSound,
    // Not a clip of a media on an audio track.
    NotASound,
    // The picture's own sound: the camera's, not the recorder's.
    SameSource,
    SoundSpeed,
    LockedTrack,
    // Linked to another clip than this picture.
    LinkedElsewhere,
};

SeparateSoundPairFault CheckSeparateSoundPair(const Document& document,
                                              const Ulid& pictureClipId,
                                              const Ulid& soundClipId);

// The batch that aligns and links the sound, or false with the reason. An
// empty batch, with true, says the sound is already in sync, linked, and
// the camera's track already off: nothing to apply. The move never covers
// another clip of the sound's track (Overlap), and the sound never reads
// more of its media than it did.
bool BuildSeparateSoundAlign(const Document& document,
                             const SeparateSoundAlignRequest& request,
                             BatchOperation& batch,
                             SeparateSoundPlacement& placement,
                             EditError& error, std::string& message);

// SYNCHRO-2026-10 SY3 -- create_interview_short on a rush already synced.
// `segment` names a range of `segment.source_id` read in `document`, the
// timeline it was selected in. When the clip that holds it is linked to a
// recorder's sound (SY2), the segment gains that sound: its source, and
// where the same instants fall in it, by the exact difference of the two
// clips' phases (TranslatedLinkedRemovals). A range read on the recorder's
// own sound -- the cleaner one for Whisper and the aligner -- becomes the
// picture's range, and the recorder's is kept as the separate sound.
// Nothing linked: the segment is left as it was given. Refused rather than
// guessed: two recorders' sounds on one picture, a sound that does not
// hold the whole range, a clip with a speed.
bool ResolveShortSegmentSound(const Document& document,
                              ProjectTimelineSourceSegment& segment,
                              EditError& error, std::string& message);
