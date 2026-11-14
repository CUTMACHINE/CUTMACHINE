// FC8 -- docs/roadmap/FILMCRAFT-2026-10.md. Random sequences of timeline
// operations, from a fixed seed, on a document shaped like a real edit.
//
// WHY THIS SUITE EXISTS. Every operation has its hand-written round trip in
// edit_tests, on the document its author had in mind. What none of them
// meets is the document another operation left behind: a ripple trim after
// a split through a linked pair, a roll on the cut a gap deletion just made,
// a slip on a clip whose speed is 1/2. edit_sequence_tests draws sequences
// too, but over one source, without links, and without ripple, roll, slip,
// splits, speed, gaps or transitions. Here each step draws one of those
// operations with plausible parameters -- and with the bounds: a trim to
// nothing, a source read past its end, a sub-frame time, a locked track --
// and holds the engine to PHILOSOPHY.md principles 2 and 6:
//
//   - refused: an error and a message are named, the document is unchanged
//     byte for byte and the journal does not grow;
//   - applied: the document validates (Document::Validate) and reads back
//     to the same bytes, the journaled operation and its inverse
//     re-serialize canonically, undo gives the previous bytes and redo the
//     next ones;
//   - at the end, undoing everything gives the starting bytes, redoing
//     everything the final ones, and the journal read back from its text
//     undoes to the start as well.
//
// REPLAYING A FAILURE. A failure prints its seed and its step. The same
// story is told again by
//
//     CUTMACHINE_OP_SEED=<seed> ./cutmachine_operation_property_tests
//
// with CUTMACHINE_OP_STEPS=<n> for its length and CUTMACHINE_OP_TRACE=1 to
// print every operation drawn, as the journal writes it. The parameters the
// generator draws depend on the seed only: it picks tracks, clips and
// markers by their place in the document, never by their identifiers, and
// gives the objects it creates identifiers counted from the seed. The
// engine still generates its own (the right half of a split, ...) at
// random, which is why "same seed, same story" is checked below: an engine
// decision that depended on an identifier's value would show there.
//
// The idea of drawing edit operations and checking the model after each
// one comes from FilmCraft (crates/edit/src/tests.rs, `never_overlaps`,
// MIT OR Apache-2.0, commit 09aaf9b); no code is taken from it.

#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectFixture.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& e) {
        ++failures;
        std::cerr << "FAIL: " << name << ": threw: " << e.what() << '\n';
    }
}

// SplitMix64: small, fast, and the same numbers on every platform, which
// <random>'s distributions do not promise.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed) {}

    uint64_t Next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Inclusive bounds.
    int64_t Range(int64_t low, int64_t high) {
        if (high <= low) return low;
        const uint64_t span = static_cast<uint64_t>(high - low) + 1;
        return low + static_cast<int64_t>(Next() % span);
    }
    bool Chance(int percent) {
        return static_cast<int>(Next() % 100) < percent;
    }
    size_t Index(size_t size) {
        return size == 0 ? 0 : static_cast<size_t>(Next() % size);
    }

private:
    uint64_t state_;
};

// Identifiers for what the generator creates, counted rather than drawn
// from the clock, so that a seed names the same operations on every run.
class Ids {
public:
    Ulid Next() {
        static const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        uint64_t value = ++counter_;
        std::string tail(21, '0');
        for (size_t index = tail.size(); index-- > 0 && value != 0;) {
            tail[index] = kAlphabet[value % 32];
            value /= 32;
        }
        return "01KF8" + tail;
    }

private:
    uint64_t counter_ = 0;
};

constexpr int32_t kRate = 25;

RationalTime F(int64_t frames) { return {frames, kRate}; }

int64_t Frames(const RationalTime& time) { return time.to_frames(kRate); }

// The starting edit: the interview shape the save benchmark is measured on
// (tests/ProjectFixture.h) -- six shots on V1, each linked to its sound on
// A1 -- with what that fixture leaves out and an edit has: cutaways on V2
// with gaps between them, a music bed on A2 that is not sync-locked, and
// markers, one of them a range.
Document MakeStartDocument() {
    const Project project = MakeInterviewShapedProject(6, 1, 6);
    Document document = project.MakeActiveDocument();

    DocumentTrack cutaways;
    cutaways.kind = "video";
    cutaways.index = 2;
    const std::vector<std::pair<int64_t, int64_t>> spans = {
        {10, 20}, {60, 30}, {150, 15}, {200, 40}};
    for (size_t index = 0; index < spans.size(); ++index) {
        DocumentClip clip;
        clip.source_id = document.sources[index % document.sources.size()].id;
        clip.source_in = F(100 + static_cast<int64_t>(index) * 40);
        clip.timeline_in = F(spans[index].first);
        clip.duration = F(spans[index].second);
        clip.include_audio = false;
        cutaways.clips.push_back(std::move(clip));
    }
    document.sequence.tracks.push_back(std::move(cutaways));

    DocumentTrack music;
    music.kind = "audio";
    music.index = 3;
    music.sync_lock = false;
    DocumentClip bed;
    bed.source_id = document.sources.back().id;
    bed.source_in = F(0);
    bed.timeline_in = F(0);
    bed.duration = F(260);
    bed.include_audio = false;
    music.clips.push_back(std::move(bed));
    document.sequence.tracks.push_back(std::move(music));

    DocumentMarker opening;
    opening.name = "Ouverture";
    opening.time = F(0);
    DocumentMarker answer;
    answer.name = "Réponse";
    answer.time = F(100);
    answer.duration = F(25);
    answer.color = "#e5484d";
    DocumentMarker last;
    last.name = "Fin";
    last.time = F(280);
    document.sequence.markers = {opening, answer, last};

    std::string error;
    if (!document.Validate(error)) {
        throw std::runtime_error("starting document does not validate: " +
                                 error);
    }
    return document;
}

// ---------------------------------------------------------------------
// What the generator reads in the document, by place, never by ID.
// ---------------------------------------------------------------------

struct ClipRef {
    const DocumentTrack* track = nullptr;
    const DocumentClip* clip = nullptr;
};

std::vector<ClipRef> AllClips(const Document& document,
                              const std::string& kind = "") {
    std::vector<ClipRef> clips;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (!kind.empty() && track.kind != kind) continue;
        for (const DocumentClip& clip : track.clips) {
            clips.push_back({&track, &clip});
        }
    }
    return clips;
}

std::optional<ClipRef> PickClip(const Document& document, Rng& rng,
                                const std::string& kind = "") {
    const std::vector<ClipRef> clips = AllClips(document, kind);
    if (clips.empty()) return std::nullopt;
    return clips[rng.Index(clips.size())];
}

std::optional<ClipRef> PickLinkedClip(const Document& document, Rng& rng) {
    std::vector<ClipRef> clips;
    for (const ClipRef& ref : AllClips(document)) {
        if (!ref.clip->link_group_id.empty()) clips.push_back(ref);
    }
    if (clips.empty()) return std::nullopt;
    return clips[rng.Index(clips.size())];
}

std::vector<ClipRef> GroupMembers(const Document& document, const Ulid& group) {
    std::vector<ClipRef> members;
    if (group.empty()) return members;
    for (const ClipRef& ref : AllClips(document)) {
        if (ref.clip->link_group_id == group) members.push_back(ref);
    }
    return members;
}

int64_t TimelineEndFrames(const Document& document) {
    int64_t end = 0;
    for (const ClipRef& ref : AllClips(document)) {
        end = std::max(end,
                       Frames(ref.clip->timeline_in.add(ref.clip->duration)));
    }
    return end;
}

int64_t SourceFrames(const Document& document, const DocumentClip& clip) {
    const DocumentSource* source = document.FindSource(clip.source_id);
    return source ? Frames(source->duration) : 0;
}

// Other tracks, some of them: what a ripple names as its sync tracks.
std::vector<Ulid> SomeOtherTracks(const Document& document, const Ulid& trackId,
                                  Rng& rng) {
    std::vector<Ulid> tracks;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.id != trackId && rng.Chance(60)) tracks.push_back(track.id);
    }
    return tracks;
}

// A time on the timeline: mostly where an editor puts one (a cut, a
// marker, the start, the end), else anywhere up to past the end, and now
// and then where nobody should: before zero, or between two frames.
RationalTime PickTime(const Document& document, Rng& rng) {
    std::vector<int64_t> boundaries = {0, TimelineEndFrames(document)};
    for (const ClipRef& ref : AllClips(document)) {
        boundaries.push_back(Frames(ref.clip->timeline_in));
        boundaries.push_back(
            Frames(ref.clip->timeline_in.add(ref.clip->duration)));
    }
    for (const DocumentMarker& marker : document.sequence.markers) {
        boundaries.push_back(Frames(marker.time));
    }
    const int64_t roll = rng.Range(0, 99);
    if (roll < 45) return F(boundaries[rng.Index(boundaries.size())]);
    if (roll < 60) {
        return F(boundaries[rng.Index(boundaries.size())] + rng.Range(-1, 1));
    }
    if (roll < 94) return F(rng.Range(0, TimelineEndFrames(document) + 25));
    if (roll < 97) return F(-rng.Range(1, 5));
    return {rng.Range(0, 2 * (TimelineEndFrames(document) + 1)) * 2 + 1,
            2 * kRate};
}

// A trim delta for one edge of `clip`: small ones mostly, and the bounds --
// the clip emptied, the source reached and passed, half a frame.
RationalTime PickDelta(const Document& document, const DocumentClip& clip,
                       Rng& rng) {
    const int64_t duration = Frames(clip.duration);
    const int64_t sourceIn = Frames(clip.source_in);
    const int64_t sourceLeft =
        SourceFrames(document, clip) - sourceIn - duration;
    const int64_t roll = rng.Range(0, 99);
    if (roll < 50) return F(rng.Range(-12, 12));
    if (roll < 70) return F(rng.Range(-80, 80));
    if (roll < 94) {
        const std::vector<int64_t> bounds = {
            duration,   -duration,   duration - 1,   1 - duration,
            sourceIn,   -sourceIn,   sourceIn + 1,   -sourceIn - 1,
            sourceLeft, -sourceLeft, sourceLeft + 1, -sourceLeft - 1,
            0};
        return F(bounds[rng.Index(bounds.size())]);
    }
    return {rng.Range(-20, 20) * 2 + 1, 2 * kRate};
}

struct AdjacentPair {
    const DocumentTrack* track = nullptr;
    const DocumentClip* left = nullptr;
    const DocumentClip* right = nullptr;
};

std::vector<AdjacentPair> AdjacentPairs(const Document& document) {
    std::vector<AdjacentPair> pairs;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (size_t index = 1; index < track.clips.size(); ++index) {
            const DocumentClip& left = track.clips[index - 1];
            const DocumentClip& right = track.clips[index];
            if (left.timeline_in.add(left.duration) == right.timeline_in) {
                pairs.push_back({&track, &left, &right});
            }
        }
    }
    return pairs;
}

// ---------------------------------------------------------------------
// One generator per operation. Each returns nothing when the document
// holds nothing it can act on.
// ---------------------------------------------------------------------

using MaybeOp = std::optional<Operation>;
using Generator = std::function<MaybeOp(const Document&, Rng&, Ids&)>;

MaybeOp GenInsertClip(const Document& document, Rng& rng, Ids& ids) {
    if (document.sequence.tracks.empty() || document.sources.empty()) {
        return std::nullopt;
    }
    const DocumentTrack& track =
        document.sequence.tracks[rng.Index(document.sequence.tracks.size())];
    const DocumentSource& source =
        document.sources[rng.Index(document.sources.size())];
    const int64_t sourceFrames = Frames(source.duration);
    InsertClipOperation op;
    op.track_id = track.id;
    op.source_id = source.id;
    op.duration = F(rng.Chance(8) ? rng.Range(-1, 0) : rng.Range(1, 80));
    op.source_in = F(rng.Chance(15) ? sourceFrames - rng.Range(0, 10)
                                    : rng.Range(0, sourceFrames - 1));
    op.timeline_in = PickTime(document, rng);
    op.clip_id = ids.Next();
    if (rng.Chance(40)) {
        op.sync_track_ids = SomeOtherTracks(document, track.id, rng);
    }
    if (rng.Chance(10))
        op.speed =
            rng.Chance(50) ? EffectParamValue{1, 2} : EffectParamValue{2, 1};
    return op;
}

MaybeOp GenRemoveClip(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    RemoveClipOperation op;
    op.clip_id = ref->clip->id;
    if (rng.Chance(50)) {
        op.sync_track_ids = SomeOtherTracks(document, ref->track->id, rng);
    }
    return op;
}

MaybeOp GenClearClip(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    return ClearClipOperation{ref->clip->id, {}};
}

MaybeOp GenClearClips(const Document& document, Rng& rng, Ids&) {
    const std::vector<ClipRef> clips = AllClips(document);
    if (clips.size() < 2) return std::nullopt;
    ClearClipsOperation op;
    std::set<size_t> picked;
    const size_t count = static_cast<size_t>(rng.Range(2, 3));
    while (picked.size() < std::min(count, clips.size())) {
        picked.insert(rng.Index(clips.size()));
    }
    for (size_t index : picked) op.clip_ids.push_back(clips[index].clip->id);
    return op;
}

MaybeOp GenMoveClip(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    MoveClipOperation op;
    op.clip_id = ref->clip->id;
    op.track_id = rng.Chance(70)
                      ? ref->track->id
                      : document.sequence
                            .tracks[rng.Index(document.sequence.tracks.size())]
                            .id;
    op.timeline_in = rng.Chance(50)
                         ? PickTime(document, rng)
                         : ref->clip->timeline_in.add(F(rng.Range(-40, 40)));
    return op;
}

MaybeOp GenMoveClips(const Document& document, Rng& rng, Ids&) {
    const std::vector<ClipRef> clips = AllClips(document);
    if (clips.size() < 2) return std::nullopt;
    const size_t first = rng.Index(clips.size());
    size_t second = rng.Index(clips.size());
    if (second == first) second = (first + 1) % clips.size();
    const RationalTime delta = F(rng.Range(-30, 30));
    MoveClipsOperation op;
    for (size_t index : {first, second}) {
        op.moves.push_back({clips[index].clip->id, clips[index].track->id,
                            clips[index].clip->timeline_in.add(delta)});
    }
    return op;
}

MaybeOp GenMoveLinkedClips(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickLinkedClip(document, rng);
    if (!ref) return std::nullopt;
    const RationalTime delta = F(rng.Range(-40, 40));
    MoveLinkedClipsOperation op;
    op.link_group_id = ref->clip->link_group_id;
    for (const ClipRef& member : GroupMembers(document, op.link_group_id)) {
        if (member.clip != ref->clip && rng.Chance(15)) continue;
        op.moves.push_back({member.clip->id, member.track->id,
                            member.clip->timeline_in.add(delta)});
    }
    return op;
}

MaybeOp GenTrimClip(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    TrimClipOperation op;
    op.clip_id = ref->clip->id;
    op.edge = rng.Chance(50) ? TrimEdge::Head : TrimEdge::Tail;
    op.delta = PickDelta(document, *ref->clip, rng);
    return op;
}

MaybeOp GenTrimLinkedClips(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickLinkedClip(document, rng);
    if (!ref) return std::nullopt;
    TrimLinkedClipsOperation op;
    op.link_group_id = ref->clip->link_group_id;
    const TrimEdge edge = rng.Chance(50) ? TrimEdge::Head : TrimEdge::Tail;
    const RationalTime delta = PickDelta(document, *ref->clip, rng);
    for (const ClipRef& member : GroupMembers(document, op.link_group_id)) {
        op.trims.push_back({member.clip->id, edge, delta});
    }
    return op;
}

MaybeOp GenRippleTrim(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    RippleTrimOperation op;
    op.clip_id = ref->clip->id;
    op.edge = rng.Chance(50) ? TrimEdge::Head : TrimEdge::Tail;
    op.delta = PickDelta(document, *ref->clip, rng);
    if (rng.Chance(60)) {
        op.linked_clip_ids =
            LinkedEdgePartnerIds(document, *ref->clip, op.edge);
    }
    if (rng.Chance(50)) {
        op.sync_track_ids = SomeOtherTracks(document, ref->track->id, rng);
    }
    return op;
}

MaybeOp GenRollEdit(const Document& document, Rng& rng, Ids&) {
    const std::vector<AdjacentPair> pairs = AdjacentPairs(document);
    RollEditOperation op;
    const DocumentClip* reference = nullptr;
    if (!pairs.empty() && rng.Chance(85)) {
        const AdjacentPair& pair = pairs[rng.Index(pairs.size())];
        op.pairs.push_back({pair.left->id, pair.right->id});
        reference = rng.Chance(50) ? pair.left : pair.right;
        // The same cut on another track, as a linked roll names it.
        if (rng.Chance(40)) {
            for (const AdjacentPair& other : pairs) {
                if (other.track != pair.track &&
                    other.right->timeline_in == pair.right->timeline_in) {
                    op.pairs.push_back({other.left->id, other.right->id});
                    break;
                }
            }
        }
    } else {
        const std::vector<ClipRef> clips = AllClips(document);
        if (clips.size() < 2) return std::nullopt;
        const ClipRef& left = clips[rng.Index(clips.size())];
        const ClipRef& right = clips[rng.Index(clips.size())];
        op.pairs.push_back({left.clip->id, right.clip->id});
        reference = left.clip;
    }
    op.delta = PickDelta(document, *reference, rng);
    return op;
}

MaybeOp GenSlipEdit(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    SlipEditOperation op;
    if (!ref->clip->link_group_id.empty() && rng.Chance(60)) {
        for (const ClipRef& member :
             GroupMembers(document, ref->clip->link_group_id)) {
            op.clip_ids.push_back(member.clip->id);
        }
    } else {
        op.clip_ids.push_back(ref->clip->id);
    }
    op.delta = PickDelta(document, *ref->clip, rng);
    return op;
}

// FC3 -- slide and rate stretch meet what the other operations leave
// behind: groups split or relinked, neighbours retimed, locks, transitions.
MaybeOp GenSlideEdit(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    SlideEditOperation op;
    if (!ref->clip->link_group_id.empty() && rng.Chance(60)) {
        for (const ClipRef& member :
             GroupMembers(document, ref->clip->link_group_id)) {
            op.clip_ids.push_back(member.clip->id);
        }
    } else {
        op.clip_ids.push_back(ref->clip->id);
    }
    op.delta = PickDelta(document, *ref->clip, rng);
    return op;
}

MaybeOp GenRateStretch(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    RateStretchOperation op;
    op.clip_id = ref->clip->id;
    op.edge = rng.Chance(50) ? TrimEdge::Head : TrimEdge::Tail;
    // Mostly half to twice the length on the frame grid; sometimes nothing,
    // sometimes half a frame off it.
    const int64_t frames = Frames(ref->clip->duration);
    const int64_t roll = rng.Range(0, 99);
    if (roll < 85) {
        op.duration =
            F(std::max<int64_t>(1, rng.Range(frames / 2, frames * 2)));
    } else if (roll < 92) {
        op.duration = F(0);
    } else {
        op.duration = {rng.Range(1, frames * 2) * 2 + 1, 2 * kRate};
    }
    // DEC40 -- the stretch of the Trim mode, which moves what follows and
    // its sync tracks; now and then sync tracks without the ripple, which
    // are refused.
    const int64_t ripple = rng.Range(0, 99);
    if (ripple < 45) {
        op.ripple = true;
        if (rng.Chance(60))
            op.sync_track_ids = SomeOtherTracks(document, ref->track->id, rng);
    } else if (ripple < 50) {
        op.sync_track_ids = SomeOtherTracks(document, ref->track->id, rng);
    }
    return op;
}

RationalTime PickInside(const DocumentClip& clip, Rng& rng) {
    const int64_t in = Frames(clip.timeline_in);
    const int64_t duration = Frames(clip.duration);
    const int64_t roll = rng.Range(0, 99);
    if (roll < 75 && duration > 1) return F(in + rng.Range(1, duration - 1));
    if (roll < 85) return F(in);
    if (roll < 95) return F(in + duration);
    return {(in + rng.Range(0, duration)) * 2 + 1, 2 * kRate};
}

MaybeOp GenSplitClip(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    SplitClipOperation op;
    op.clip_id = ref->clip->id;
    op.timeline_position =
        rng.Chance(85) ? PickInside(*ref->clip, rng) : PickTime(document, rng);
    return op;
}

MaybeOp GenSplitLinkedClips(const Document& document, Rng& rng, Ids& ids) {
    const auto ref = PickLinkedClip(document, rng);
    if (!ref) return std::nullopt;
    SplitLinkedClipsOperation op;
    op.link_group_id = ref->clip->link_group_id;
    for (const ClipRef& member : GroupMembers(document, op.link_group_id)) {
        op.clip_ids.push_back(member.clip->id);
    }
    op.timeline_position = PickInside(*ref->clip, rng);
    op.left_group_id = ids.Next();
    op.right_group_id = ids.Next();
    if (rng.Chance(40)) {
        op.sync_track_ids = SomeOtherTracks(document, ref->track->id, rng);
    }
    return op;
}

MaybeOp GenSplitClipAtPositions(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    SplitClipAtPositionsOperation op;
    op.clip_id = ref->clip->id;
    const int64_t count = rng.Range(1, 3);
    for (int64_t index = 0; index < count; ++index) {
        op.positions.push_back(PickInside(*ref->clip, rng));
    }
    // Mostly what ResolveIntervalSplits produces; sometimes not sorted, or
    // with a duplicate, which must be refused rather than reordered.
    if (rng.Chance(80)) {
        std::sort(op.positions.begin(), op.positions.end(),
                  [](const RationalTime& a, const RationalTime& b) {
                      return a.compare(b) < 0;
                  });
    }
    return op;
}

MaybeOp GenDeleteGap(const Document& document, Rng& rng, Ids&) {
    if (document.sequence.tracks.empty()) return std::nullopt;
    struct Gap {
        const DocumentTrack* track;
        int64_t start;
        int64_t length;
    };
    std::vector<Gap> gaps;
    for (const DocumentTrack& track : document.sequence.tracks) {
        int64_t cursor = 0;
        for (const DocumentClip& clip : track.clips) {
            const int64_t in = Frames(clip.timeline_in);
            if (in > cursor) gaps.push_back({&track, cursor, in - cursor});
            cursor =
                std::max(cursor, Frames(clip.timeline_in.add(clip.duration)));
        }
    }
    DeleteGapOperation op;
    const DocumentTrack* track = nullptr;
    if (!gaps.empty() && rng.Chance(80)) {
        const Gap& gap = gaps[rng.Index(gaps.size())];
        track = gap.track;
        const int64_t offset =
            rng.Chance(60) ? 0 : rng.Range(0, gap.length - 1);
        op.gap_start = F(gap.start + offset);
        op.gap_duration = F(rng.Chance(70) ? gap.length - offset
                                           : rng.Range(1, gap.length + 2));
    } else {
        track = &document.sequence
                     .tracks[rng.Index(document.sequence.tracks.size())];
        op.gap_start = PickTime(document, rng);
        op.gap_duration = F(rng.Range(-1, 30));
    }
    op.track_id = track->id;
    if (rng.Chance(40)) {
        op.linked_track_ids = SomeOtherTracks(document, track->id, rng);
    }
    return op;
}

const char* PickMarkerColor(Rng& rng) {
    static const char* kColors[] = {"#f5c542", "#e5484d", "#30a46c", "#0091ff"};
    return kColors[rng.Index(4)];
}

MaybeOp GenAddMarker(const Document& document, Rng& rng, Ids& ids) {
    AddMarkerOperation op;
    op.marker.id = ids.Next();
    op.marker.name = "Note " + std::to_string(rng.Range(1, 99));
    op.marker.time = PickTime(document, rng);
    op.marker.color = PickMarkerColor(rng);
    if (rng.Chance(30)) op.marker.duration = F(rng.Range(-1, 50));
    return op;
}

MaybeOp GenRemoveMarker(const Document& document, Rng& rng, Ids&) {
    const auto& markers = document.sequence.markers;
    if (markers.empty()) return std::nullopt;
    return RemoveMarkerOperation{markers[rng.Index(markers.size())].id};
}

MaybeOp GenUpdateMarker(const Document& document, Rng& rng, Ids&) {
    const auto& markers = document.sequence.markers;
    if (markers.empty()) return std::nullopt;
    const DocumentMarker& marker = markers[rng.Index(markers.size())];
    UpdateMarkerOperation op;
    op.marker_id = marker.id;
    op.name = rng.Chance(80) ? marker.name : "Renommé";
    op.time = rng.Chance(50) ? marker.time : PickTime(document, rng);
    op.color = PickMarkerColor(rng);
    op.category = marker.category;
    op.duration = rng.Chance(60) ? F(0) : F(rng.Range(-1, 60));
    return op;
}

MaybeOp GenSetClipSpeed(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickClip(document, rng);
    if (!ref) return std::nullopt;
    static const EffectParamValue kSpeeds[] = {{1, 2}, {2, 1},  {1, 1},
                                               {3, 2}, {2, 3},  {1, 4},
                                               {0, 1}, {-1, 2}, {1, 0}};
    SetClipSpeedOperation op;
    op.clip_id = ref->clip->id;
    op.speed = kSpeeds[rng.Index(sizeof(kSpeeds) / sizeof(kSpeeds[0]))];
    return op;
}

MaybeOp GenRemoveLinkedClips(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickLinkedClip(document, rng);
    if (!ref) return std::nullopt;
    RemoveLinkedClipsOperation op;
    op.link_group_id = ref->clip->link_group_id;
    for (const ClipRef& member : GroupMembers(document, op.link_group_id)) {
        op.clip_ids.push_back(member.clip->id);
    }
    if (rng.Chance(50)) {
        op.sync_track_ids = SomeOtherTracks(document, ref->track->id, rng);
    }
    return op;
}

MaybeOp GenClearLinkedClips(const Document& document, Rng& rng, Ids&) {
    const auto ref = PickLinkedClip(document, rng);
    if (!ref) return std::nullopt;
    ClearLinkedClipsOperation op;
    op.link_group_id = ref->clip->link_group_id;
    for (const ClipRef& member : GroupMembers(document, op.link_group_id)) {
        op.clip_ids.push_back(member.clip->id);
    }
    return op;
}

MaybeOp GenSetClipLink(const Document& document, Rng& rng, Ids& ids) {
    const auto picture = PickClip(document, rng, "video");
    const auto sound = PickClip(document, rng, "audio");
    if (!picture || !sound) return std::nullopt;
    SetClipLinkOperation op;
    op.first_clip_id = picture->clip->id;
    op.second_clip_id = sound->clip->id;
    if (rng.Chance(60)) {
        op.first_group_id = ids.Next();
        op.second_group_id =
            rng.Chance(90) ? op.first_group_id : Ulid(ids.Next());
    }
    return op;
}

MaybeOp GenAddTransition(const Document& document, Rng& rng, Ids& ids) {
    const std::vector<AdjacentPair> pairs = AdjacentPairs(document);
    if (pairs.empty()) return std::nullopt;
    const AdjacentPair& pair = pairs[rng.Index(pairs.size())];
    AddTransitionOperation op;
    op.transition.id = ids.Next();
    op.transition.track_id = pair.track->id;
    op.transition.left_clip_id = pair.left->id;
    op.transition.right_clip_id = pair.right->id;
    op.transition.duration = F(rng.Chance(10) ? 0 : rng.Range(1, 30));
    static const TransitionAlignment kAlignments[] = {
        TransitionAlignment::Center, TransitionAlignment::StartAtCut,
        TransitionAlignment::EndAtCut};
    op.transition.alignment = kAlignments[rng.Index(3)];
    if (rng.Chance(15)) {
        op.transition.type = "dip_to_color";
        op.transition.color = "#ffffff";
    }
    return op;
}

MaybeOp GenRemoveTransition(const Document& document, Rng& rng, Ids&) {
    const auto& transitions = document.sequence.transitions;
    if (transitions.empty()) return std::nullopt;
    return RemoveTransitionOperation{
        transitions[rng.Index(transitions.size())].id};
}

MaybeOp GenSetTrackLock(const Document& document, Rng& rng, Ids&) {
    if (document.sequence.tracks.empty()) return std::nullopt;
    const DocumentTrack& track =
        document.sequence.tracks[rng.Index(document.sequence.tracks.size())];
    return SetTrackLockOperation{track.id, !track.locked};
}

MaybeOp GenSetTrackSyncLock(const Document& document, Rng& rng, Ids&) {
    if (document.sequence.tracks.empty()) return std::nullopt;
    const DocumentTrack& track =
        document.sequence.tracks[rng.Index(document.sequence.tracks.size())];
    return SetTrackSyncLockOperation{track.id, !track.sync_lock};
}

MaybeOp GenAddTrack(const Document& document, Rng& rng, Ids& ids) {
    AddTrackOperation op;
    op.track_id = ids.Next();
    op.kind = rng.Chance(50) ? "video" : "audio";
    op.index = static_cast<int32_t>(
        rng.Range(-1, static_cast<int64_t>(document.sequence.tracks.size())));
    return op;
}

// Any track, not only the last: undo puts a removed track back in its place
// in the sequence (FC8b), which the byte comparison checks.
MaybeOp GenRemoveTrack(const Document& document, Rng& rng, Ids&) {
    // Keep something to edit: never below three tracks.
    if (document.sequence.tracks.size() < 3) return std::nullopt;
    const int64_t last =
        static_cast<int64_t>(document.sequence.tracks.size()) - 1;
    return RemoveTrackOperation{
        document.sequence.tracks[static_cast<size_t>(rng.Range(0, last))].id};
}

struct NamedGenerator {
    const char* name;
    int weight;
    Generator generate;
};

const std::vector<NamedGenerator>& Generators() {
    static const std::vector<NamedGenerator> generators = {
        {"InsertClip", 6, GenInsertClip},
        {"RemoveClip", 4, GenRemoveClip},
        {"ClearClip", 3, GenClearClip},
        {"ClearClips", 2, GenClearClips},
        {"MoveClip", 5, GenMoveClip},
        {"MoveClips", 3, GenMoveClips},
        {"MoveLinkedClips", 4, GenMoveLinkedClips},
        {"TrimClip", 6, GenTrimClip},
        {"TrimLinkedClips", 4, GenTrimLinkedClips},
        {"RippleTrim", 6, GenRippleTrim},
        {"RollEdit", 6, GenRollEdit},
        {"SlipEdit", 5, GenSlipEdit},
        {"SlideEdit", 4, GenSlideEdit},
        {"RateStretch", 4, GenRateStretch},
        {"SplitClip", 5, GenSplitClip},
        {"SplitLinkedClips", 4, GenSplitLinkedClips},
        {"SplitClipAtPositions", 3, GenSplitClipAtPositions},
        {"DeleteGap", 4, GenDeleteGap},
        {"AddMarker", 3, GenAddMarker},
        {"RemoveMarker", 2, GenRemoveMarker},
        {"UpdateMarker", 2, GenUpdateMarker},
        {"SetClipSpeed", 4, GenSetClipSpeed},
        {"RemoveLinkedClips", 2, GenRemoveLinkedClips},
        {"ClearLinkedClips", 2, GenClearLinkedClips},
        {"SetClipLink", 3, GenSetClipLink},
        {"AddTransition", 3, GenAddTransition},
        {"RemoveTransition", 2, GenRemoveTransition},
        {"SetTrackLock", 1, GenSetTrackLock},
        {"SetTrackSyncLock", 1, GenSetTrackSyncLock},
        {"AddTrack", 1, GenAddTrack},
        {"RemoveTrack", 1, GenRemoveTrack},
    };
    return generators;
}

const NamedGenerator& PickGenerator(Rng& rng) {
    const auto& generators = Generators();
    int total = 0;
    for (const NamedGenerator& generator : generators) {
        total += generator.weight;
    }
    int ticket = static_cast<int>(rng.Range(0, total - 1));
    for (const NamedGenerator& generator : generators) {
        if (ticket < generator.weight) return generator;
        ticket -= generator.weight;
    }
    return generators.back();
}

// One step: an operation, or a batch of two or three drawn on the same
// state (PL1), whose later members may well be refused -- and then the
// whole batch must be.
MaybeOp Draw(const Document& document, Rng& rng, Ids& ids, std::string& name) {
    if (rng.Chance(6)) {
        BatchOperation batch;
        batch.label = "lot tiré";
        const int64_t count = rng.Range(2, 3);
        name = "Batch(";
        for (int64_t index = 0; index < count; ++index) {
            const NamedGenerator& generator = PickGenerator(rng);
            MaybeOp member = generator.generate(document, rng, ids);
            if (!member) continue;
            batch.operations.push_back(std::move(*member));
            name += std::string(index ? "," : "") + generator.name;
        }
        name += ")";
        if (batch.operations.empty()) return std::nullopt;
        return batch;
    }
    const NamedGenerator& generator = PickGenerator(rng);
    name = generator.name;
    return generator.generate(document, rng, ids);
}

// ---------------------------------------------------------------------
// The run.
// ---------------------------------------------------------------------

struct Outcome {
    std::string name;
    bool applied = false;
    EditError error = EditError::None;
    bool operator==(const Outcome& other) const {
        return name == other.name && applied == other.applied &&
               error == other.error;
    }
};

struct RunReport {
    bool ok = true;
    std::vector<Outcome> outcomes;
};

std::string HexSeed(uint64_t seed) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx",
                  static_cast<unsigned long long>(seed));
    return text;
}

std::string Clip(const std::string& text, size_t limit = 1200) {
    return text.size() <= limit ? text : text.substr(0, limit) + "…";
}

// The operation as the journal re-reads it: what it was and what undoes it
// must both be written and read back to the same text.
bool RoundTripsCanonically(const Operation& operation, std::string& why) {
    const std::string text = SerializeOperation(operation);
    Operation parsed = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    if (!DeserializeOperation(text, parsed, error, message)) {
        why = "the journal does not read it back: " + message;
        return false;
    }
    if (SerializeOperation(parsed) != text) {
        why = "read back, it is written differently";
        return false;
    }
    return true;
}

RunReport RunSeed(uint64_t seed, int steps, bool trace) {
    RunReport report;
    Rng rng(seed);
    Ids ids;
    Document document = MakeStartDocument();
    const std::string start = document.SaveToString();
    EditLog log;
    const std::string replay = "rejouer : CUTMACHINE_OP_SEED=" + HexSeed(seed) +
                               " CUTMACHINE_OP_STEPS=" + std::to_string(steps);

    const auto fail = [&](const std::string& where, const std::string& what) {
        report.ok = false;
        Check(false, "graine " + HexSeed(seed) + ", " + where + " : " + what +
                         " (" + replay + ")");
    };

    for (int step = 0; step < steps; ++step) {
        std::string name;
        MaybeOp drawn = Draw(document, rng, ids, name);
        if (!drawn) continue;
        const std::string text = SerializeOperation(*drawn);
        const std::string where = "pas " + std::to_string(step) + ", " + name;
        if (trace) std::cout << "  " << step << ' ' << name << ' ' << text;

        const std::string before = document.SaveToString();
        const size_t depth = log.AppliedCount();
        EditError error = EditError::None;
        std::string message;
        const bool applied = log.Apply(document, *drawn, error, message);
        report.outcomes.push_back({name, applied, error});
        if (trace) {
            std::cout << " -> "
                      << (applied ? std::string("appliquée")
                                  : std::string("refusée ") +
                                        EditErrorName(error) + " : " + message)
                      << '\n';
        }

        if (!applied) {
            if (document.SaveToString() != before) {
                fail(where, "refusée (" + message +
                                "), elle a pourtant modifié le document : " +
                                Clip(text));
                return report;
            }
            if (error == EditError::None || message.empty()) {
                fail(where, "refusée sans erreur nommée : " + Clip(text));
                return report;
            }
            if (log.AppliedCount() != depth) {
                fail(where, "refusée, elle est pourtant au journal");
                return report;
            }
            continue;
        }

        const std::string after = document.SaveToString();
        std::string why;
        if (!document.Validate(why)) {
            fail(where, "appliquée, le document ne se valide plus (" + why +
                            ") : " + Clip(text));
            return report;
        }
        Document reread;
        if (!Document::LoadFromString(after, reread, why) ||
            reread.SaveToString() != after) {
            fail(where, "appliquée, le document ne se relit pas à l'octet (" +
                            why + ") : " + Clip(text));
            return report;
        }
        if (log.AppliedCount() != depth + 1) {
            fail(where, "appliquée, le journal n'a pas une entrée de plus");
            return report;
        }
        const Entry& entry = log.AppliedEntries().back();
        if (!RoundTripsCanonically(entry.op, why)) {
            fail(where, "l'opération journalisée : " + why);
            return report;
        }
        if (!RoundTripsCanonically(entry.inverse, why)) {
            fail(where, "son inverse : " + why);
            return report;
        }
        if (!log.Undo(document, error, message)) {
            fail(where, "undo refusé : " + message + " : " + Clip(text));
            return report;
        }
        if (document.SaveToString() != before) {
            fail(where,
                 "undo ne rend pas le document d'avant octet pour octet : " +
                     Clip(text));
            return report;
        }
        if (!log.Redo(document, error, message)) {
            fail(where, "redo refusé : " + message + " : " + Clip(text));
            return report;
        }
        if (document.SaveToString() != after) {
            fail(where,
                 "redo ne rend pas le document d'après octet pour octet : " +
                     Clip(text));
            return report;
        }
    }

    const std::string final = document.SaveToString();
    const std::string journal = log.Serialize();
    const size_t depth = log.AppliedCount();
    EditError error = EditError::None;
    std::string message;
    while (log.AppliedCount() > 0) {
        if (!log.Undo(document, error, message)) {
            fail("fin", "undo refusé à la profondeur " +
                            std::to_string(log.AppliedCount()) + " : " +
                            message);
            return report;
        }
    }
    if (document.SaveToString() != start) {
        fail("fin", "tout annuler ne rend pas le document de départ");
        return report;
    }
    while (log.UndoneCount() > 0) {
        if (!log.Redo(document, error, message)) {
            fail("fin", "redo refusé : " + message);
            return report;
        }
    }
    if (document.SaveToString() != final) {
        fail("fin", "tout refaire ne rend pas le document d'arrivée");
        return report;
    }

    // The journal as a file holds it: read back, it undoes to the start too.
    EditLog reread;
    if (!EditLog::Deserialize(journal, reread, error, message) ||
        reread.AppliedCount() != depth) {
        fail("fin", "le journal ne se relit pas : " + message);
        return report;
    }
    Document replayed = document;
    while (reread.AppliedCount() > 0) {
        if (!reread.Undo(replayed, error, message)) {
            fail("fin", "le journal relu refuse un undo : " + message);
            return report;
        }
    }
    if (replayed.SaveToString() != start) {
        fail("fin", "le journal relu n'annule pas jusqu'au départ");
    }
    return report;
}

bool ReadUnsigned(const char* name, uint64_t& value) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        throw std::runtime_error(std::string(name) +
                                 " is not a number: " + text);
    }
    value = parsed;
    return true;
}

// The seeds a plain run draws. Fixed, so a red CI run fails the same way on
// a laptop; a new failure found with another seed is added here once it is
// understood.
std::vector<uint64_t> DefaultSeeds(size_t count) {
    std::vector<uint64_t> seeds;
    for (uint64_t index = 1; seeds.size() < count; ++index) {
        seeds.push_back(0xFC80000000000000ULL + index * 0x9E3779B97F4AULL);
    }
    return seeds;
}

// ---------------------------------------------------------------------
// FixedEngineBugs -- what the sequences found on 17 October 2026 (FC8),
// reduced to the smallest document that shows it, and fixed (FC8b). The
// generator draws these cases again; the tests below keep the reduced
// cases named.
// ---------------------------------------------------------------------

// One shot on V1 linked to its sound on A1.
Document LinkedPairDocument() {
    Document document;
    document.sources.push_back({GenerateUlid(), "a.mov", {25, 1}, F(1000)});
    DocumentTrack video;
    video.kind = "video";
    video.index = 0;
    DocumentTrack audio;
    audio.kind = "audio";
    audio.index = 1;
    DocumentClip picture;
    picture.source_id = document.sources[0].id;
    picture.source_in = F(0);
    picture.duration = F(50);
    picture.timeline_in = F(0);
    picture.include_audio = false;
    picture.link_group_id = GenerateUlid();
    DocumentClip sound = picture;
    sound.id = GenerateUlid();
    video.clips.push_back(picture);
    audio.clips.push_back(sound);
    document.sequence.tracks = {video, audio};
    return document;
}

void FixedEngineBugs() {
    // 1. Undoing RemoveTrack used to append the track to the end of the
    // vector, whose order is in the canonical bytes: removing V1 of
    // [V1, A1] and undoing left [A1, V1].
    Test("FC8b: undoing RemoveTrack puts the track back in its place", [] {
        Document document = LinkedPairDocument();
        const std::string before = document.SaveToString();
        const Ulid first = document.sequence.tracks[0].id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document, RemoveTrackOperation{first}, error, message),
              "remove V1: " + message);
        Check(log.Undo(document, error, message), "undo: " + message);
        Check(document.SaveToString() == before,
              "undo gives the document back byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.sequence.tracks.size() == 1,
              "redo removes it again");
    });

    Test("FC8b: an AddTrack before an unknown track is refused", [] {
        Document document = LinkedPairDocument();
        const std::string before = document.SaveToString();
        AddTrackOperation add;
        add.kind = "video";
        add.index = 5;
        add.insert_before_track_id = GenerateUlid();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(document, add, error, message) &&
                  error == EditError::UnknownTrack,
              "refused as UnknownTrack");
        Check(document.SaveToString() == before, "document unchanged");
    });

    // 3. SetClipLink used to link a freeze: SetClipSpeed refuses a freeze
    // on a linked clip, so once linked, giving it a speed back could not be
    // undone (its inverse, the freeze, was refused).
    Test("FC8b: a freeze cannot be linked", [] {
        Document document = LinkedPairDocument();
        DocumentClip& picture = document.sequence.tracks[0].clips[0];
        DocumentClip& sound = document.sequence.tracks[1].clips[0];
        picture.link_group_id.clear();
        sound.link_group_id.clear();
        picture.speed = {0, 1};
        const std::string before = document.SaveToString();
        const Ulid group = GenerateUlid();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(
            !log.Apply(document,
                       SetClipLinkOperation{picture.id, sound.id, group, group,
                                            std::nullopt, std::nullopt},
                       error, message) &&
                error == EditError::InvalidOperation,
            "refused: " + message);
        Check(document.SaveToString() == before, "document unchanged");
    });

    // 3b. A freeze linked before that fix (or by a journal's exact state):
    // giving it a speed back applied, and its undo, the freeze, was then
    // refused on the linked clip. It is refused until the clip is
    // unlinked, which keeps every step reversible without making
    // Document::Validate refuse the projects that hold such a link.
    Test("FC8b: a linked freeze is unlinked before its speed changes", [] {
        Document document = LinkedPairDocument();
        // Two pictures, both frozen, in one group: the state the sequences
        // reached through SetClipLink before it refused a freeze.
        document.sequence.tracks[1].kind = "video";
        document.sequence.tracks[0].clips[0].speed = {0, 1};
        document.sequence.tracks[1].clips[0].speed = {0, 1};
        const Ulid frozen = document.sequence.tracks[0].clips[0].id;
        const Ulid other = document.sequence.tracks[1].clips[0].id;
        std::string invalid;
        Check(document.Validate(invalid),
              "such a project still opens: " + invalid);
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(document, SetClipSpeedOperation{frozen, {1, 1}, {}},
                         error, message) &&
                  error == EditError::InvalidOperation &&
                  message.find("unlink it first") != std::string::npos,
              "its speed is refused while it is linked: " + message);
        Check(document.SaveToString() == before, "document unchanged");
        Check(log.Apply(document,
                        SetClipLinkOperation{
                            frozen, other, {}, {}, std::nullopt, std::nullopt},
                        error, message),
              "unlinked: " + message);
        Check(log.Apply(document, SetClipSpeedOperation{frozen, {1, 1}, {}},
                        error, message),
              "then its speed changes: " + message);
        Check(log.Undo(document, error, message) &&
                  log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "and both steps undo, back to the linked freeze: " + message);
    });

    // 2. SplitClipAtPositions used to cut the linked sound on a locked
    // track, and the entry could then not be undone while the lock held.
    Test(
        "FC8b: SplitClipAtPositions respects the lock of a linked "
        "partner's track",
        [] {
            Document document = LinkedPairDocument();
            document.sequence.tracks[1].locked = true;
            const std::string before = document.SaveToString();
            SplitClipAtPositionsOperation split;
            split.clip_id = document.sequence.tracks[0].clips[0].id;
            split.positions = {F(25)};
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(!log.Apply(document, split, error, message) &&
                      error == EditError::LockedTrack,
                  "refused as LockedTrack");
            Check(document.SaveToString() == before, "document unchanged");
            document.sequence.tracks[1].locked = false;
            const std::string unlocked = document.SaveToString();
            Check(log.Apply(document, split, error, message) &&
                      document.sequence.tracks[1].clips.size() == 2,
                  "unlocked, it cuts the linked sound too: " + message);
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == unlocked,
                  "and undoes byte for byte");
        });
}

}  // namespace

int main() {
    uint64_t value = 0;
    const int steps = ReadUnsigned("CUTMACHINE_OP_STEPS", value)
                          ? static_cast<int>(value)
                          : 200;
    const bool trace = ReadUnsigned("CUTMACHINE_OP_TRACE", value) && value;
    std::vector<uint64_t> seeds;
    if (ReadUnsigned("CUTMACHINE_OP_SEED", value)) {
        seeds = {value};
    } else {
        const size_t count = ReadUnsigned("CUTMACHINE_OP_SEEDS", value)
                                 ? static_cast<size_t>(value)
                                 : 16;
        seeds = DefaultSeeds(count);
    }

    Test("the starting document validates and reads back to its bytes", [] {
        const Document document = MakeStartDocument();
        const std::string text = document.SaveToString();
        Document reread;
        std::string error;
        Check(Document::LoadFromString(text, reread, error) &&
                  reread.SaveToString() == text,
              "the starting document reads back: " + error);
        Check(document.sequence.tracks.size() == 4 &&
                  !AdjacentPairs(document).empty() &&
                  !GroupMembers(
                       document,
                       document.sequence.tracks[0].clips[0].link_group_id)
                       .empty() &&
                  document.sequence.markers.size() == 3,
              "it has two video and two audio tracks, cuts, links and "
              "markers");
    });

    std::map<std::string, std::pair<int, int>> counts;
    std::vector<Outcome> firstStory;
    Test(
        "each operation drawn is refused cleanly or undone and redone "
        "byte for byte, and the whole sequence undoes to the start",
        [&] {
            const auto begin = std::chrono::steady_clock::now();
            for (uint64_t seed : seeds) {
                if (trace) std::cout << "graine " << HexSeed(seed) << '\n';
                const RunReport report = RunSeed(seed, steps, trace);
                if (firstStory.empty()) firstStory = report.outcomes;
                for (const Outcome& outcome : report.outcomes) {
                    auto& count = counts[outcome.name.rfind("Batch", 0) == 0
                                             ? "Batch"
                                             : outcome.name];
                    (outcome.applied ? count.first : count.second) += 1;
                }
            }
            const double seconds = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - begin)
                                       .count();
            std::cout << seeds.size() << " graine(s) x " << steps << " pas en "
                      << seconds << " s ; appliquées/refusées "
                      << "par opération :\n";
            for (const auto& [name, count] : counts) {
                std::cout << "  " << name << " " << count.first << "/"
                          << count.second << '\n';
            }
        });

    // Only meaningful on the default run, long enough to meet every case.
    if (seeds.size() > 1 && steps >= 100) {
        Test("every operation is applied at least once, and refusals happen",
             [&] {
                 int refused = 0;
                 for (const NamedGenerator& generator : Generators()) {
                     Check(counts[generator.name].first > 0,
                           std::string(generator.name) +
                               " was never applied: the generator no longer "
                               "draws a plausible one");
                     refused += counts[generator.name].second;
                 }
                 Check(counts["Batch"].first > 0, "no batch was ever applied");
                 Check(refused > 0,
                       "nothing was ever refused: the bounds "
                       "are no longer drawn");
             });
    }

    Test("the same seed tells the same story", [&] {
        const RunReport again = RunSeed(seeds.front(), steps, false);
        Check(again.outcomes == firstStory,
              "graine " + HexSeed(seeds.front()) +
                  " : a second run draws other operations or meets other "
                  "verdicts -- an engine decision depends on the value of "
                  "an identifier it generated");
    });

    FixedEngineBugs();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
