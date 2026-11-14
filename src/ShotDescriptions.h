#pragma once

// PL1 to PL4 -- docs/roadmap/PLANS-2026-10.md. Described shots: what one
// stretch of a rush shows, written once by whoever looked at it (a model, a
// person), kept in the document (Document.h, ShotDescription), and queried
// afterwards instead of looked at again.
//
// The speech of the rushes has been indexed for a long time -- dated words,
// blocks -- and the picture had nothing: every edit that needed a cutaway
// paid for contact sheets again and lost what they showed. PHILOSOPHY.md
// principle 7 applied to perception: the model perceives once, the code
// keeps, validates and searches what it said, and the search is a rule --
// deterministic, explainable, the same for every model.
//
// Everything here is portable and pure: no FFmpeg, no file, no clock. The
// surfaces (McpTools.cc) read the arguments and the time of day, this file
// says whether a description is well formed, merges a call into a media's
// list, and answers a search.

#include "Document.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Bounds of one description. Wide enough for any honest description, narrow
// enough that the field holds a description and not a transcript: a search
// reads every one of them.
inline constexpr size_t kMaxShotsPerMedia = 1000;
inline constexpr size_t kMaxShotSubjects = 8;
inline constexpr size_t kMaxShotSubjectBytes = 80;
inline constexpr size_t kMaxShotActionBytes = 300;
inline constexpr size_t kMaxShotPlaceBytes = 120;
inline constexpr size_t kMaxShotReserveBytes = 200;
inline constexpr size_t kMaxShotKeywords = 16;
inline constexpr size_t kMaxShotKeywordBytes = 40;
inline constexpr size_t kMaxShotAuthorBytes = 80;

// The closed vocabularies, spelled as the file, the journal and the tools
// spell them (DocumentFormat.h and OperationFormat.h hold the same lists).
const std::vector<std::string>& ShotSizeNames();
const std::vector<std::string>& CameraMovementNames();
const std::vector<std::string>& ShotQualityNames();
const std::vector<std::string>& ShotAuthorKindNames();

std::string ShotSizeName(ShotSize size);
std::string CameraMovementName(CameraMovement movement);
std::string ShotQualityName(ShotQuality quality);
std::string ShotAuthorKindName(ShotAuthorKind kind);

// Exact spelling only: a closed vocabulary that guessed "gros plan" into
// `close` would be the model's guess again.
bool ParseShotSize(const std::string& name, ShotSize& size);
bool ParseCameraMovement(const std::string& name, CameraMovement& movement);
bool ParseShotQuality(const std::string& name, ShotQuality& quality);
bool ParseShotAuthorKind(const std::string& name, ShotAuthorKind& kind);

// "YYYY-MM-DDTHH:MM:SSZ", a real date and time of day.
bool IsShotTimestamp(const std::string& text);

// One description on its own, against the media it describes: a valid ULID,
// a positive stretch inside the media, every closed field named, text within
// its bounds and without control characters, a reserve when the quality is
// not good, an author and a timestamp.
bool ValidateShotDescription(const LibraryMedia& media,
                             const ShotDescription& shot, std::string& error);

// A media's whole list: each description valid, ids distinct, in increasing
// source_in, none overlapping the next. What Document::Validate and
// SetMediaShotsOperation both ask. Refuses a description on an audio-only
// media.
bool ValidateMediaShots(const LibraryMedia& media,
                        const std::vector<ShotDescription>& shots,
                        std::string& error);

// PL2 -- describe_shots. `written` replaces the description of the same id
// and adds the others; `removed` names descriptions to drop. The result is
// sorted by source_in and validated as a whole: a new description that
// overlaps one the call neither replaces nor removes is refused, naming it,
// rather than silently cut.
bool MergeShotDescriptions(const LibraryMedia& media,
                           const std::vector<ShotDescription>& written,
                           const std::vector<Ulid>& removed,
                           std::vector<ShotDescription>& merged,
                           std::string& error);

// PL3 -- search_shots. Every filter given must hold; within a filter, any of
// its values. `text` is split into words, and every word must appear in the
// free fields (subjects, action, place, keywords, reserve), ignoring case
// and French accents, as a substring: "velo" finds "vélos".
struct ShotQuery {
    std::vector<Ulid> media_ids;
    std::vector<ShotSize> sizes;
    std::vector<CameraMovement> movements;
    std::vector<ShotQuality> qualities;
    std::optional<ShotAuthorKind> author_kind;
    std::string text;
    // Zero: no minimum.
    RationalTime minimum_duration{0, 1};
};

struct ShotMatch {
    const LibraryMedia* media = nullptr;
    const ShotDescription* shot = nullptr;
};

// In library order, then by source_in. Never ranked: the order a search
// answers in says nothing about which shot is better (PHILOSOPHY.md
// principle 12), and the same document gives the same list.
std::vector<ShotMatch> SearchShots(const std::vector<LibraryMedia>& library,
                                   const ShotQuery& query);

// Lower case, French accents and ligatures folded to ASCII. Exposed for the
// tests and for whoever wants to say why a word matched.
std::string FoldShotText(const std::string& text);

// PL4 -- what is not described yet.
//
// The description covering `time` (source_in <= time < end), or null.
const ShotDescription* ShotAt(const LibraryMedia& media,
                              const RationalTime& time);

// The descriptions that share at least one instant with [start, end).
std::vector<const ShotDescription*> ShotsOverlapping(const LibraryMedia& media,
                                                     const RationalTime& start,
                                                     const RationalTime& end);

// Sum of the described durations, exact.
RationalTime DescribedDuration(const LibraryMedia& media);
