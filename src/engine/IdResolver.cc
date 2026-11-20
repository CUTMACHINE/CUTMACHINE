#include "IdResolver.h"

#include "Document.h"

#include <algorithm>
#include <cctype>

namespace {

// PRD14 -- the letter tracks are aliased under, and so the one clip prefix a
// track alias can meet (see TrackAlias).
constexpr char kTrackAliasLetter = 'T';

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](char character) {
                       return static_cast<char>(
                           std::toupper(static_cast<unsigned char>(character)));
                   });
    return value;
}

}  // namespace

std::vector<const DocumentTrack*> TracksInDescribeOrder(
    const DocumentSequence& sequence) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : sequence.tracks) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return tracks;
}

std::string ClipAliasPrefix(size_t trackOrdinal) {
    std::string prefix;
    do {
        prefix.insert(
            prefix.begin(),
            static_cast<char>('A' + static_cast<int>(trackOrdinal % 26)));
        trackOrdinal /= 26;
        if (trackOrdinal == 0) break;
        --trackOrdinal;
    } while (true);
    return prefix;
}

std::string TrackAlias(const std::vector<const DocumentTrack*>& tracks,
                       size_t trackOrdinal) {
    const size_t lettered = static_cast<size_t>(kTrackAliasLetter - 'A');
    if (lettered < tracks.size() &&
        trackOrdinal < tracks[lettered]->clips.size())
        return std::string();
    return std::string(1, kTrackAliasLetter) + std::to_string(trackOrdinal + 1);
}

std::vector<Ulid> CollectDocumentIds(const Document& document) {
    std::vector<Ulid> ids;
    ids.push_back(document.sequence.id);
    for (const DocumentTrack& track : document.sequence.tracks) {
        ids.push_back(track.id);
        for (const DocumentClip& clip : track.clips) {
            ids.push_back(clip.id);
            // F1.2 -- ROADMAP.md: linked-group operations expose their group
            // ID as an addressable argument even though the group is stored
            // on its member clips rather than as a standalone document node.
            if (!clip.link_group_id.empty() &&
                std::find(ids.begin(), ids.end(), clip.link_group_id) ==
                    ids.end())
                ids.push_back(clip.link_group_id);
        }
    }
    for (const DocumentSource& source : document.sources)
        ids.push_back(source.id);
    for (const LibraryMedia& media : document.library) ids.push_back(media.id);
    for (const DocumentBin& bin : document.bins) ids.push_back(bin.id);
    for (const DocumentMarker& marker : document.sequence.markers)
        ids.push_back(marker.id);
    for (const DocumentTransition& transition : document.sequence.transitions)
        ids.push_back(transition.id);
    for (const CaptionStyle& style : document.sequence.caption_styles)
        ids.push_back(style.id);
    for (const MulticamGroup& group : document.sequence.multicam_groups) {
        ids.push_back(group.id);
        for (const MulticamAngle& angle : group.angles) ids.push_back(angle.id);
    }
    return ids;
}

IdResolver::IdResolver(const Document& document)
    : universe_(CollectDocumentIds(document)),
      sequence_id_(document.sequence.id) {
    const std::vector<const DocumentTrack*> tracks =
        TracksInDescribeOrder(document.sequence);
    for (size_t trackOrdinal = 0; trackOrdinal < tracks.size();
         ++trackOrdinal) {
        // PRD14 -- a track answers to its alias wherever a track is named:
        // every such argument is resolved here.
        if (const std::string alias = TrackAlias(tracks, trackOrdinal);
            !alias.empty())
            aliases_.push_back({alias, tracks[trackOrdinal]->id});
        for (size_t clipIndex = 0;
             clipIndex < tracks[trackOrdinal]->clips.size(); ++clipIndex) {
            aliases_.push_back(
                {ClipAliasPrefix(trackOrdinal) + std::to_string(clipIndex + 1),
                 tracks[trackOrdinal]->clips[clipIndex].id});
        }
    }
    for (size_t index = 0; index < document.library.size(); ++index)
        aliases_.push_back(
            {"M" + std::to_string(index + 1), document.library[index].id});
    for (size_t index = 0; index < document.sequence.markers.size(); ++index)
        aliases_.push_back({"K" + std::to_string(index + 1),
                            document.sequence.markers[index].id});
}

bool IdResolver::Resolve(const std::string& fieldName, const std::string& input,
                         Ulid& output, std::string& error) const {
    if (input.empty()) {
        error = "'" + fieldName + "' must not be empty";
        return false;
    }
    // A full ID is checked first and, if present, wins outright: since every
    // ID in the universe has the same fixed length, a full match can never
    // also be a proper prefix of a different one, so this cannot mask an
    // ambiguity.
    if (std::find(universe_.begin(), universe_.end(), input) !=
        universe_.end()) {
        output = input;
        return true;
    }
    const std::string alias = UpperAscii(input);
    const Ulid* aliasMatch = nullptr;
    size_t aliasMatchCount = 0;
    for (const auto& candidate : aliases_) {
        if (candidate.first != alias) continue;
        aliasMatch = &candidate.second;
        ++aliasMatchCount;
    }
    if (aliasMatchCount == 1) {
        output = *aliasMatch;
        return true;
    }
    if (aliasMatchCount > 1) {
        error = "'" + fieldName + "': alias '" + input +
                "' is ambiguous, matches more than one object";
        return false;
    }
    const Ulid* match = nullptr;
    size_t matchCount = 0;
    for (const Ulid& id : universe_) {
        if (id.size() >= input.size() &&
            id.compare(0, input.size(), input) == 0) {
            if (matchCount == 0) match = &id;
            ++matchCount;
            if (matchCount > 1) break;
        }
    }
    if (matchCount == 0) {
        error = "'" + fieldName + "': no object matches id or prefix '" +
                input + "'";
        return false;
    }
    if (matchCount > 1) {
        error = "'" + fieldName + "': id prefix '" + input +
                "' is ambiguous, matches more than one object";
        return false;
    }
    output = *match;
    return true;
}
