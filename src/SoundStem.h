#pragma once

// SN4 -- docs/roadmap/SON-2026-10.md. A stem rendered by cutmachine-sound,
// laid on the timeline where its timecode says, in one undo.
//
// The stem says where it starts in its BWF chunk: TimeReference, the sample
// its first sample falls on, counted from midnight. The renderer wrote there
// the timeline's first frame at the start timecode the engine's EDL uses
// (kEdlDefaultStartTimecode), converted by one rule: a frame f goes to
// sample round(f x den x rate / num), a half rounding up
// (tools/cutmachine-sound/time.mjs). Laying the stem inverts that rule
// rather than dividing: the frame whose sample is the time reference, or a
// refusal when no frame has it -- a stem rendered for another cadence, start
// or timeline is refused by name, never moved to the nearest frame.
//
// Nothing new in the document, as for SY2 (SeparateSound.h): an audio track
// added when none is named, and a paste that refuses to cover what the track
// already holds, in one BatchOperation. The clip covers the timeline's whole
// frames: the stem's end inside its last frame (a reverb tail, a rate that
// does not divide) is not laid, and the placement says how much.

#include "Document.h"
#include "Edl.h"
#include "Operations.h"

#include <cstdint>
#include <string>

// What a WAV file says of itself, as far as laying it goes.
struct BroadcastWave {
    int32_t sample_rate = 0;
    int32_t channels = 0;
    // Per channel: the data chunk over the block size.
    int64_t samples = 0;
    // The bext chunk, when there is one: TimeReference, in samples.
    bool has_time_reference = false;
    int64_t time_reference = 0;
};

// RIFF/WAVE only (an RF64 file is refused by name). False with the reason
// for a file that is not a WAV, holds no fmt or data chunk, or is cut short.
bool ReadBroadcastWave(const std::string& path, BroadcastWave& wave,
                       std::string& error);

// The renderer's rule, exactly: the sample a frame of a timeline at `rate`
// falls on, at `sampleRate`.
int64_t SoundStemFrameSample(int64_t frame, const MediaRate& rate,
                             int32_t sampleRate);

struct SoundStemRequest {
    // The stem in the library: sound only, already ingested.
    Ulid media_id;
    // An audio track of the timeline; empty adds one above every track.
    Ulid track_id;
    // The timecode of the timeline's first frame the stem was rendered at.
    std::string start_timecode = kEdlDefaultStartTimecode;
    // The stem's file, as ReadBroadcastWave read it.
    BroadcastWave wave;
};

struct SoundStemPlacement {
    Ulid clip_id;
    Ulid track_id;
    bool track_added = false;
    // In whole frames of the timeline.
    int64_t first_frame = 0;
    int64_t frames = 0;
    RationalTime timeline_in{0, 1};
    RationalTime duration{0, 1};
    // The whole stem, and what of its end was not laid (less than a frame).
    RationalTime stem_duration{0, 1};
    RationalTime not_laid{0, 1};
};

// The batch that lays the stem, or false with the reason: an unknown or
// pictured media, a file whose length or rate is not the library's, no time
// reference, a time reference no frame falls on, a stem that starts before
// the timeline, a track that is not an audio one or is locked, less than a
// frame to lay. A clip the track already holds there is refused when the
// batch is applied (the paste's Overlap).
bool BuildSoundStemPlacement(const Document& document,
                             const SoundStemRequest& request,
                             BatchOperation& batch,
                             SoundStemPlacement& placement, EditError& error,
                             std::string& message);
