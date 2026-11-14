#pragma once

#include "Document.h"
#include "Operations.h"

#include <set>
#include <vector>

// Out of lot -- the author, 29 October 2026: "D on a selected shot disables it:
// greyed, it is no longer seen or heard". DaVinci Resolve's Clip >
// Enable/Disable Clip (clipEnableDisableClip), stated once for every surface
// that shows, plays, renders or measures the timeline.
//
// A disabled clip (DocumentClip::enabled false, SetClipEnabledOperation):
//
// - stays on its track. It keeps its place, its length, its links, its grade
//   and its sound settings; every edit moves, trims and cuts it like any
//   other clip, and the timeline's duration still counts it, as Resolve's
//   does. Enabled again, it gives back the film it was;
// - is not seen. It leaves a hole in its track, as a hidden track does:
//   the tracks underneath show through, black where none does. In a
//   dissolve it drops out the way an inactive multicam angle does
//   (Multicam.h): the other side keeps its own extension and fade, over
//   whatever lies underneath. A disabled carton is not drawn;
// - is not heard, be it a sound clip or a motion clip's own sound. A
//   disabled picture does not silence the sound linked to it by itself:
//   the window disables a linked pair together because its selection takes
//   both (linked selection on), as Resolve's does, and a caller that names
//   the picture alone keeps the sound.
//
// The picture side joins the multicam rule: ClipIsSeen and OffScreenClipIds
// answer for both, so a renderer that asked Multicam.h for the picture asks
// here instead. The sound side is ClipIsHeard, asked beside the track's own
// mute and solo. Timeline.cc (the monitor, verify_export, the end-of-film
// check), Export.cc, the monitor's mix (AudioPlayback.mm), the timeline
// audio plan (transcribe_timeline, analyze_timeline_audio), the timeline
// transcript, the subtitles, the sheets, timeline_stats, the shot quality
// report, check_timeline and review_cuts ask this file. A new consumer of
// the picture or the sound must ask it too.

// Whether `clip`'s picture, or its carton, is on screen: enabled, and not an
// angle its multicam group takes off screen.
bool ClipIsSeen(const DocumentSequence& sequence, const DocumentClip& clip);

// Whether `clip`'s sound plays, the track's mute and solo aside.
inline bool ClipIsHeard(const DocumentClip& clip) { return clip.enabled; }

// Every clip off screen: the multicam groups' hidden angles and every
// disabled clip.
std::set<Ulid> OffScreenClipIds(const DocumentSequence& sequence);

// The disabled clips, in the order of the tracks, then of each track's clips.
std::vector<Ulid> DisabledClipIds(const DocumentSequence& sequence);

// The window's D on a selection, Resolve's toggle: when any of `clipIds` is
// enabled, all of them are disabled; when none is, all are enabled. An id
// that names no clip, or a clip on a locked track (which keeps its clips as
// they play), is left out, once each. No clip left: an empty operation,
// which the caller does not apply.
SetClipEnabledOperation ToggleClipsEnabled(const Document& document,
                                           const std::vector<Ulid>& clipIds);
