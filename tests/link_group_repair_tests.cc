// Out of lot -- the author, 4 November 2026: « J'ai encore le problème des
// blocs fusionnés que je peux pas bouger ». The rule of LinkGroupRepair.h: a
// link group that binds several places of a timeline (the shape a cut left
// before #213) is split one group per place, as one undo step, and leaves
// every other group as it is. Portable: no AppKit, FFmpeg never run.

#include "EditLog.h"
#include "LinkGroupRepair.h"
#include "Operations.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
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
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

const Ulid kRush = "01KDXC00000000000000000001";
const Ulid kPictureTrack = "01KDXC00000000000000000002";
const Ulid kSoundTrack = "01KDXC00000000000000000003";
const Ulid kSecondSoundTrack = "01KDXC00000000000000000004";
const Ulid kBlock = "01KDXC00000000000000000005";

// 26-character IDs of the form 01KDX<prefix>000...<n>, for clips and for the
// groups a repair hands out, so that a test can name what it expects.
Ulid Numbered(char prefix, int number) {
    char tail[21];
    std::snprintf(tail, sizeof(tail), "%020d", number);
    return std::string("01KDX") + prefix + tail;
}

std::function<Ulid()> GroupIds() {
    auto next = std::make_shared<int>(0);
    return [next] { return Numbered('G', ++*next); };
}

LibraryMedia Media() {
    LibraryMedia media;
    media.id = kRush;
    media.path = "/tmp/cutmachine-link-group-repair/rush.mp4";
    media.filename = "rush.mp4";
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.orientation = "landscape";
    media.rate = {25, 1};
    media.duration = {100000, 25};
    media.has_video = true;
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    media.metadata_complete = true;
    return media;
}

DocumentClip Clip(const Ulid& id, int64_t sourceIn, int64_t duration,
                  int64_t timelineIn, const Ulid& group) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = kRush;
    clip.source_in = {sourceIn, 25};
    clip.duration = {duration, 25};
    clip.timeline_in = {timelineIn, 25};
    clip.link_group_id = group;
    return clip;
}

Document Empty(bool secondSound = false) {
    Document document;
    document.sequence.width = 1920;
    document.sequence.height = 1080;
    document.sequence.frame_rate = {25, 1};
    document.library = {Media()};
    document.sources = {{kRush, Media().path, {25, 1}, {100000, 25}}};
    document.sequence.tracks = {DocumentTrack{kPictureTrack, "video", 0, {}},
                                DocumentTrack{kSoundTrack, "audio", 1, {}}};
    if (secondSound) {
        document.sequence.tracks.push_back(
            DocumentTrack{kSecondSoundTrack, "audio", 2, {}});
    }
    return document;
}

void Validated(const Document& document) {
    std::string error;
    if (!document.Validate(error))
        throw std::runtime_error("the fixture is invalid: " + error);
}

// A block cut before #213: `count` stretches of one rush laid end to end,
// each a picture and its sound, all under the block's one group. Stretch n
// is clip V<n> on the picture track and A<n> on the sound track; the
// sound of stretch `slipped` (if any) reads two frames later than its
// picture.
Document CutBlock(int count, int slipped = -1) {
    Document document = Empty();
    for (int index = 0; index < count; ++index) {
        const int64_t at = index * 40;
        const int64_t source = index * 60;
        document.sequence.tracks[0].clips.push_back(
            Clip(Numbered('V', index), source, 40, at, kBlock));
        document.sequence.tracks[1].clips.push_back(
            Clip(Numbered('A', index), source + (index == slipped ? 2 : 0), 40,
                 at, kBlock));
    }
    Validated(document);
    return document;
}

const DocumentClip* Find(const Document& document, const Ulid& id) {
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (clip.id == id) return &clip;
    return nullptr;
}

std::map<Ulid, std::set<Ulid>> Groups(const Document& document) {
    std::map<Ulid, std::set<Ulid>> groups;
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (!clip.link_group_id.empty())
                groups[clip.link_group_id].insert(clip.id);
    return groups;
}

// Everything but the links: where each clip is and what it reads.
std::string Placement(const Document& document) {
    std::string text;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            text += clip.id + "@" + std::to_string(clip.timeline_in.value) +
                    "+" + std::to_string(clip.duration.value) + "<" +
                    std::to_string(clip.source_in.value) + ";";
        }
    }
    return text;
}

void ApplyAll(Document& document, EditLog& log, const LinkGroupRepair& repair) {
    for (BatchOperation& batch : LinkGroupRepairBatches(repair)) {
        EditError error = EditError::None;
        std::string message;
        if (!log.Apply(document, Operation{std::move(batch)}, error, message))
            throw std::runtime_error("the repair is refused: " + message);
    }
}

}  // namespace

int main() {
    Test("a block cut before #213 gets one group per stretch", [] {
        Document document = CutBlock(3);
        const std::string before = document.SaveToString();
        const std::string placement = Placement(document);
        const LinkGroupRepair repair =
            PlanLinkGroupRepair(document, GroupIds());
        Check(repair.groups == 1 && repair.places == 2 &&
                  repair.operations.size() == 3 && repair.cleared == 0 &&
                  repair.left_alone == 0,
              "one group, two places given a new one, a link per stretch");
        EditLog log;
        ApplyAll(document, log, repair);
        const auto groups = Groups(document);
        Check(groups.size() == 3, "three groups after the repair");
        for (int index = 0; index < 3; ++index) {
            const DocumentClip* picture = Find(document, Numbered('V', index));
            const DocumentClip* sound = Find(document, Numbered('A', index));
            Check(picture && sound &&
                      picture->link_group_id == sound->link_group_id &&
                      groups.at(picture->link_group_id).size() == 2,
                  "stretch " + std::to_string(index) +
                      ": its picture and its sound, alone in their group");
            Check(sound && sound->sync_anchor_clip_id == picture->id &&
                      sound->sync_reference_delta == RationalTime(0, 1),
                  "stretch " + std::to_string(index) +
                      ": its sound is in sync with its own picture");
        }
        Check(Find(document, Numbered('V', 0))->link_group_id == kBlock,
              "the earliest stretch keeps the block's group");
        Check(Placement(document) == placement,
              "no clip moves, none is trimmed");
        Check(log.AppliedCount() == 1, "the repair is one undo step");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "undone, the document is the one before, byte for byte: " +
                  message);
    });

    Test("a repaired timeline has nothing left to repair", [] {
        Document document = CutBlock(4);
        EditLog log;
        ApplyAll(document, log, PlanLinkGroupRepair(document, GroupIds()));
        Check(PlanLinkGroupRepair(document, GroupIds()).Empty(),
              "planned again, the repair finds nothing");
    });

    Test("a slipped sound keeps its offset against its own picture", [] {
        Document document = CutBlock(3, 1);
        EditLog log;
        ApplyAll(document, log, PlanLinkGroupRepair(document, GroupIds()));
        Document reference = Empty();
        reference.sequence.tracks[0].clips = {
            Clip(Numbered('V', 1), 60, 40, 40, "")};
        reference.sequence.tracks[1].clips = {
            Clip(Numbered('A', 1), 62, 40, 40, "")};
        SetClipLinkOperation link;
        link.first_clip_id = Numbered('V', 1);
        link.second_clip_id = Numbered('A', 1);
        link.first_group_id = Numbered('G', 99);
        link.second_group_id = Numbered('G', 99);
        EditLog referenceLog;
        EditError error = EditError::None;
        std::string message;
        Check(referenceLog.Apply(reference, Operation{link}, error, message),
              "the pair links on its own: " + message);
        const DocumentClip* repaired = Find(document, Numbered('A', 1));
        const DocumentClip* linked = Find(reference, Numbered('A', 1));
        Check(repaired && linked &&
                  repaired->sync_reference_delta ==
                      linked->sync_reference_delta &&
                  !(linked->sync_reference_delta == RationalTime(0, 1)),
              "the offset is the one set_clip_link gives the pair");
    });

    Test("a group at one place is left as it is", [] {
        // A J cut (the sound starts before its picture), and a picture with
        // two sounds, the second one a recorder's.
        Document document = Empty(true);
        const Ulid jCut = Numbered('J', 1);
        const Ulid triple = Numbered('T', 1);
        document.sequence.tracks[0].clips = {
            Clip(Numbered('V', 1), 100, 50, 20, jCut),
            Clip(Numbered('V', 2), 300, 50, 100, triple)};
        document.sequence.tracks[1].clips = {
            Clip(Numbered('A', 1), 80, 60, 0, jCut),
            Clip(Numbered('A', 2), 300, 50, 100, triple)};
        document.sequence.tracks[2].clips = {
            Clip(Numbered('R', 2), 300, 50, 100, triple)};
        Validated(document);
        const LinkGroupRepair repair =
            PlanLinkGroupRepair(document, GroupIds());
        Check(repair.Empty() && repair.groups == 0,
              "nothing to repair: each group lies at one place");
    });

    Test("clips alone at their place lose a link they share with nothing", [] {
        // Stretch 1 lost its sound, stretch 2 its picture.
        Document document = CutBlock(3);
        auto& pictures = document.sequence.tracks[0].clips;
        auto& sounds = document.sequence.tracks[1].clips;
        pictures.erase(pictures.begin() + 2);
        sounds.erase(sounds.begin() + 1);
        Validated(document);
        const LinkGroupRepair repair =
            PlanLinkGroupRepair(document, GroupIds());
        Check(repair.groups == 1 && repair.cleared == 2 &&
                  repair.left_alone == 0 && repair.places == 0,
              "two lone clips cleared, the first stretch keeps the "
              "group");
        EditLog log;
        ApplyAll(document, log, repair);
        Check(Find(document, Numbered('V', 1))->link_group_id.empty() &&
                  Find(document, Numbered('A', 2))->link_group_id.empty(),
              "the lone picture and the lone sound are unlinked");
        const auto groups = Groups(document);
        Check(groups.size() == 1 && groups.at(kBlock).size() == 2,
              "the block's group holds the first stretch only");
    });

    Test("an odd lone clip keeps the old group, alone in it", [] {
        Document document = CutBlock(2);
        document.sequence.tracks[1].clips.pop_back();  // stretch 1's sound
        Validated(document);
        const LinkGroupRepair repair =
            PlanLinkGroupRepair(document, GroupIds());
        Check(
            repair.cleared == 0 && repair.left_alone == 1 && repair.places == 1,
            "the lone picture stays, the first stretch takes a new group");
        EditLog log;
        ApplyAll(document, log, repair);
        const auto groups = Groups(document);
        Check(groups.size() == 2 && groups.at(kBlock).size() == 1 &&
                  groups.at(kBlock).count(Numbered('V', 1)) == 1,
              "the old group holds the lone picture alone");
    });

    Test("a long repair is split into batches the journal accepts", [] {
        Document document = CutBlock(300);
        const LinkGroupRepair repair =
            PlanLinkGroupRepair(document, GroupIds());
        const std::vector<BatchOperation> batches =
            LinkGroupRepairBatches(repair);
        Check(repair.operations.size() == 300 && batches.size() == 2 &&
                  batches[0].operations.size() == kMaxBatchOperations,
              "300 links, in two batches of at most 256");
        EditLog log;
        ApplyAll(document, log, repair);
        Check(Groups(document).size() == 300 && log.AppliedCount() == 2,
              "every stretch has its group, in two undo steps");
    });

    Test("a timeline without links has nothing to repair", [] {
        Document document = CutBlock(3);
        for (DocumentTrack& track : document.sequence.tracks)
            for (DocumentClip& clip : track.clips) clip.link_group_id.clear();
        Validated(document);
        Check(PlanLinkGroupRepair(document, GroupIds()).Empty() &&
                  LinkGroupRepairBatches({}).empty(),
              "no operation, no batch");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
