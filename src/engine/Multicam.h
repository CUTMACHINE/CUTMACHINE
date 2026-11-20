#pragma once

#include "Document.h"
#include "Json.h"
#include "Operations.h"

#include <set>
#include <string>
#include <vector>

// F1.5 -- ROADMAP.md. What a multicam group does to the montage, stated once
// for every surface that shows, renders or measures the picture.
//
// The schema (F0.3) gives a group one active angle, not a list of switches at
// cut points: an angle names a clip already placed on a video track, and the
// group says which of those clips is the one to see. The picture rule is the
// smallest one that reading allows:
//
// - a group with an active angle takes every other angle's clip off screen,
//   over the whole of that clip, as if its track were hidden there. The
//   active angle composites like any clip, and whatever lies underneath
//   shows wherever it does not cover;
// - a group with no active angle changes nothing: every angle composites as
//   an ordinary clip, which is what a group saved before any consumer existed
//   already rendered;
// - a clip that is an angle of several groups (the schema allows it, the
//   surfaces below refuse to create it) is off screen as soon as one of them
//   has another angle active.
//
// Sound is untouched. An angle names a video clip and nothing in the schema
// says which sound goes with it; muting a camera's linked audio when its
// picture is switched away ("audio follows video") would change the mix on a
// reading the document does not carry. Export, timeline transcription and
// playback keep mixing the audio tracks exactly as they did.
//
// Export.cc, Timeline.cc (monitor and export verification), TimelineStats,
// TimelineSheets, the shot quality report and the Resolve return trip all
// ask this file, so none of them can show an angle another one hides. A new
// consumer of the picture must ask it too -- through ClipEnabled.h since 29
// October 2026, whose ClipIsSeen and OffScreenClipIds answer for this rule
// and for the disabled clips at once.

// Every clip the sequence's multicam groups take off screen.
std::set<Ulid> MulticamHiddenClipIds(const DocumentSequence& sequence);

// The same rule for one clip, without building the set: the monitor asks once
// per track and frame, and a sequence carries a handful of groups at most.
bool MulticamHidesClip(const DocumentSequence& sequence, const Ulid& clipId);

// The CLI, MCP and the application build their operations through the two
// functions below, so none of them decides alone what a valid group, a
// default name or "the angle of this clip" is. Identifiers are already
// resolved (IdResolver) when they arrive here.

struct MulticamAngleRequest {
    Ulid clip_id;
    // Empty: "Angle N", N counting from 1 in request order.
    std::string name;
};

// Builds a new group and assigns every identity up front, so a caller can
// report the group and angle ids without reading the project back. An empty
// `name` becomes "Multicam N". An empty `activeClipId` leaves the group with
// no active angle.
//
// Refused, although Document::Validate would accept them: fewer than two
// angles (a group of one switches nothing), and a clip that is already an
// angle of another group (with the rule above, two groups would fight over
// it; it is almost always a second call for the same cameras). Also refused
// here rather than as a generic ValidationFailed: a clip that is unknown, not
// on a video track or named twice, and an active clip that is not one of the
// angles.
bool BuildAddMulticamGroup(const Document& document, const std::string& name,
                           const std::vector<MulticamAngleRequest>& angles,
                           const Ulid& activeClipId,
                           AddMulticamGroupOperation& operation,
                           EditError& error, std::string& message);

// Resolves what a caller names -- an angle id, or the id of the clip an angle
// shows -- to the single (group, angle) pair it designates. `groupId` may be
// empty when the angle alone determines the group; when given, the angle must
// belong to it. An empty `angleOrClipId` clears the group's active angle and
// then requires `groupId`. A clip that is an angle of several groups is
// refused without `groupId` rather than guessed.
bool BuildSetMulticamActiveAngle(const Document& document, const Ulid& groupId,
                                 const Ulid& angleOrClipId,
                                 SetMulticamActiveAngleOperation& operation,
                                 EditError& error, std::string& message);

// F1.5 -- decided by the author on 8 October 2026 (ROADMAP.md). A cut through a
// group at `position`: every angle that spans it is split there, and what
// follows becomes a second group, which can show another angle -- the
// multicam editing of Resolve, in a schema whose group has one active
// angle. The group keeps the angles before the cut (the left pieces keep
// their ids); the new one takes the right pieces and the angles that lie
// wholly after it. Its active angle is the one that was active, or the
// group's angle `activeAngleId` names when given (empty keeps it). Sound is
// not cut: it does not follow the angle. One batch, one undo.
//
// Refused: an unknown group; a position at which no angle is cut (before,
// after or between them all); an `activeAngleId` that is not one of the
// angles the second group holds.
struct MulticamCut {
    // The second group, its angles in the group's order, the one it shows.
    Ulid group_id;
    std::vector<Ulid> angle_ids;
    Ulid active_angle_id;
};
bool BuildSplitMulticamGroup(const Document& document, const Ulid& groupId,
                             const RationalTime& position,
                             const Ulid& activeAngleId, BatchOperation& batch,
                             MulticamCut& cut, EditError& error,
                             std::string& message);

// Read side shared by --list-multicam-groups and list_multicam_groups: each
// group with its angles, where each angle's clip sits, whether it is the
// active one and whether multicam takes it off screen. Times are exact
// {value, rate} pairs.
mcp_json::Value DescribeMulticamGroup(const DocumentSequence& sequence,
                                      const MulticamGroup& group);
mcp_json::Value DescribeMulticamGroups(const DocumentSequence& sequence);

// What a multicam edit reports beside its surface's own result, so the next
// call needs no read: `group_id`, the group as it stands afterwards (`group`,
// null once removed), its `angle_ids` in order and `active_angle_id`, and the
// timeline's `hidden_clip_ids`. Measured on a dry run of `operation` over
// `before` with the engine's own ApplyOperation (the IA9 pattern), so it
// describes what the edit does rather than what a surface expects. False
// when the dry run is refused -- the real apply is refused too and says why
// -- and for an add whose ids are not all assigned (BuildAddMulticamGroup
// assigns them), since the apply would draw others.
bool DescribeMulticamEdit(const Document& before, const Operation& operation,
                          mcp_json::Value& report);
