#pragma once

// Prefix-based short-ID resolution for the MCP surface (ROADMAP.md F1.2).
//
// Conceptually the same idea a comparable agent-native editor's MCP surface
// uses -- an agent sees and sends short unique prefixes of ULIDs instead of
// full 26-character IDs, and an ambiguous prefix is an explicit error
// rather than a silent guess -- but written from that description, not
// from any specific implementation's (GPLv3) source. See ROADMAP.md
// guard-rail 7.
//
// The resolver itself is deliberately kind-agnostic: it just answers
// "which full ID in this document does this string identify". A prefix
// that resolves to a real ID of the wrong kind (e.g. a track ID where a
// clip_id was expected) is still caught, exactly as an unresolved bad ID
// would be, by ApplyOperation's own Document::Find* lookups -- the
// resolver does not duplicate that validation.

#include "Ulid.h"

#include <string>
#include <utility>
#include <vector>

class Document;
struct DocumentSequence;
struct DocumentTrack;

// PRD14 -- ROADMAP.md. How an alias is spelled. DescribeDocument (Cli.cc)
// shows the aliases and IdResolver resolves them; each used to keep its own
// copy of the lettering, and both now go through these, so what one shows
// the other accepts.
//
// Every alias is a position in one reading of the timeline: a track by its
// rank, a clip by its track's rank and its own, a media and a marker by
// theirs. An edit that adds or removes something renumbers what follows it.

// The tracks of `sequence` in the order describe lists them: by `index`,
// ties in stored order.
std::vector<const DocumentTrack*> TracksInDescribeOrder(
    const DocumentSequence& sequence);

// The letters the clips of the track of rank `trackOrdinal` go by: A for the
// first track listed, then B ... Z, AA, AB; its clips are A1, A2...
std::string ClipAliasPrefix(size_t trackOrdinal);

// T1 for the first of `tracks` (in describe order), T2 for the second.
// Tracks had no alias: an agent laying a first clip copied a ULID out of
// describe. A track appended by add_track with no `index` takes the next
// number and moves no other; removing a track, or adding one at a lower
// index, renumbers the tracks listed after it -- and re-letters their clips,
// which was already so.
//
// Empty when a clip already goes by that spelling: the clips of the
// twentieth track are T1, T2... and were accepted before tracks had an
// alias, so on such a timeline the clip keeps the name and the track is
// named by its ID. An alias accepted yesterday is never made ambiguous.
std::string TrackAlias(const std::vector<const DocumentTrack*>& tracks,
                       size_t trackOrdinal);

class IdResolver {
public:
    // Builds the resolver over every stable ID currently addressable in
    // `document`: sequence, tracks, clips, A/V link groups, sources, library
    // media, bins, markers, transitions, caption styles, multicam groups and
    // angles.
    explicit IdResolver(const Document& document);

    // `input` may be one of DescribeDocument's visible aliases (a clip, a
    // track, a library media, a marker), a full ID (checked first, as an
    // exact match), or a non-empty prefix of exactly one known ID. Returns
    // false with a human-readable `error` naming `fieldName` when `input` is
    // empty, matches nothing, or matches more than one object -- never
    // guesses.
    bool Resolve(const std::string& fieldName, const std::string& input,
                 Ulid& output, std::string& error) const;

    const std::vector<Ulid>& Universe() const { return universe_; }

    // The document's own sequence ID. Exposed so tools that operate on "the
    // sequence" (there is exactly one per document) do not need to ask the
    // caller to look it up and pass it back.
    const Ulid& SequenceId() const { return sequence_id_; }

private:
    std::vector<Ulid> universe_;
    std::vector<std::pair<std::string, Ulid>> aliases_;
    Ulid sequence_id_;
};

// Exposed for testing and for callers that already hold a universe (e.g. one
// gathered from a freshly generated batch of IDs mid-operation).
std::vector<Ulid> CollectDocumentIds(const Document& document);
