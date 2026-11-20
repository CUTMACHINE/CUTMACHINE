#pragma once

// S6 -- SAVING_ROADMAP.md. The exact difference between two project states.
//
// A project history entry used to carry both whole states, hex-encoded: each
// timeline rename grew the journal by four times the project, and every edit
// of every timeline then re-serialized, re-parsed and fsync'd that journal
// twice (199 MB for 33 entries, measured on a 1.5 MB interview project; one
// project operation took 3.7 s and 1.4 GB). An entry now records only what
// its operation changed -- the timelines, rushes, bins and sources it
// touched, the metadata entries it set and the scalar fields it moved -- so
// its size follows the change and not the project.
//
// Replaying a delta replaces exactly what it names and nothing else.
// Undoing a timeline rename restores the old name and keeps the edits made
// to that timeline since, where restoring a whole snapshot silently
// discarded them along with every other timeline's.

#include "Project.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

template <typename T>
struct DeltaChange {
    T from;
    T to;
};

template <typename T>
struct DeltaItem {
    size_t index = 0;
    T item;
};

// Items identified by ULID inside an ordered vector. `removed` indices refer
// to the state the delta starts from and `added` indices to the state it
// produces, both ascending; items present on both sides keep their relative
// order, which is why a reordering cannot be expressed (ComputeProjectDelta
// refuses it rather than pretending).
template <typename T>
struct OrderedDelta {
    std::vector<DeltaItem<T>> removed;
    std::vector<DeltaItem<T>> added;
    std::vector<DeltaChange<T>> changed;
};

// A timeline present on both sides. Its header fields are recorded one by
// one, so that replaying a rename touches the name and nothing else;
// `content` (markers, transitions, tracks, caption styles, multicam groups)
// appears only when an operation changed it.
struct TimelineDelta {
    Ulid id;
    std::optional<DeltaChange<std::string>> name;
    std::optional<DeltaChange<int32_t>> width;
    std::optional<DeltaChange<int32_t>> height;
    std::optional<DeltaChange<MediaRate>> frame_rate;
    std::optional<DeltaChange<DocumentSequence>> content;
};

// A keyed map entry; an empty side means the key is absent on that side.
template <typename T>
struct KeyedDelta {
    Ulid key;
    std::optional<T> from;
    std::optional<T> to;
};

struct ProjectDelta {
    std::optional<DeltaChange<std::string>> name;
    std::optional<DeltaChange<Ulid>> active_timeline_id;
    std::optional<DeltaChange<ColorManagementSettings>> color_management;
    std::vector<DeltaItem<DocumentSequence>> removed_timelines;
    std::vector<DeltaItem<DocumentSequence>> added_timelines;
    std::vector<TimelineDelta> changed_timelines;
    OrderedDelta<LibraryMedia> rushes;
    OrderedDelta<DocumentBin> bins;
    OrderedDelta<DocumentSource> sources;
    std::vector<KeyedDelta<ProjectBinMetadata>> bin_metadata;
    std::vector<KeyedDelta<Ulid>> timeline_bin_ids;
};

// Fails, leaving `delta` untouched, when the two states do not share their
// project ID or when items present in both were reordered: no operation does
// either, and a caller must then keep whole states rather than a delta that
// would replay into something else.
bool ComputeProjectDelta(const Project& from, const Project& to,
                         ProjectDelta& delta, std::string& error);

ProjectDelta ReverseProjectDelta(const ProjectDelta& delta);

// Replays `delta` onto `project`. Refuses a removal of an item the project
// no longer holds, an addition of an item it already holds and a position it
// cannot reach, leaving `project` partially changed: callers apply to a
// candidate copy and validate it, as ApplyProjectOperation does.
bool ApplyProjectDelta(Project& project, const ProjectDelta& delta,
                       std::string& error);

// Canonical, strict JSON: one byte sequence per delta, every key required.
std::string SerializeProjectDelta(const ProjectDelta& delta);
bool DeserializeProjectDelta(const std::string& json, ProjectDelta& delta,
                             std::string& error);
