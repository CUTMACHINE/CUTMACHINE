#include "ShotDescriptions.h"

#include <algorithm>
#include <exception>
#include <set>
#include <utility>

namespace {

template <typename Enum>
struct Vocabulary {
    std::vector<std::pair<Enum, std::string>> entries;

    std::vector<std::string> Names() const {
        std::vector<std::string> names;
        for (const auto& entry : entries) names.push_back(entry.second);
        return names;
    }

    std::string NameOf(Enum value) const {
        for (const auto& entry : entries)
            if (entry.first == value) return entry.second;
        return std::string();
    }

    bool Parse(const std::string& name, Enum& value) const {
        for (const auto& entry : entries) {
            if (entry.second == name) {
                value = entry.first;
                return true;
            }
        }
        return false;
    }
};

// The spellings of DocumentFormat.h and OperationFormat.h; the tests check
// that the three agree (tests/shot_descriptions_tests.cc).
const Vocabulary<ShotSize>& Sizes() {
    static const Vocabulary<ShotSize> vocabulary{{
        {ShotSize::ExtremeWide, "extreme_wide"},
        {ShotSize::Wide, "wide"},
        {ShotSize::Full, "full"},
        {ShotSize::Medium, "medium"},
        {ShotSize::MediumClose, "medium_close"},
        {ShotSize::Close, "close"},
        {ShotSize::ExtremeClose, "extreme_close"},
        {ShotSize::Insert, "insert"},
    }};
    return vocabulary;
}

const Vocabulary<CameraMovement>& Movements() {
    static const Vocabulary<CameraMovement> vocabulary{{
        {CameraMovement::Static, "static"},
        {CameraMovement::Handheld, "handheld"},
        {CameraMovement::Pan, "pan"},
        {CameraMovement::Tilt, "tilt"},
        {CameraMovement::ZoomIn, "zoom_in"},
        {CameraMovement::ZoomOut, "zoom_out"},
        {CameraMovement::PushIn, "push_in"},
        {CameraMovement::PullOut, "pull_out"},
        {CameraMovement::Tracking, "tracking"},
        {CameraMovement::Crane, "crane"},
        {CameraMovement::Aerial, "aerial"},
    }};
    return vocabulary;
}

const Vocabulary<ShotQuality>& Qualities() {
    static const Vocabulary<ShotQuality> vocabulary{{
        {ShotQuality::Good, "good"},
        {ShotQuality::Usable, "usable"},
        {ShotQuality::Poor, "poor"},
    }};
    return vocabulary;
}

const Vocabulary<ShotAuthorKind>& AuthorKinds() {
    static const Vocabulary<ShotAuthorKind> vocabulary{{
        {ShotAuthorKind::Model, "model"},
        {ShotAuthorKind::Person, "person"},
    }};
    return vocabulary;
}

bool HasControlCharacter(const std::string& text) {
    for (const char character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7F) return true;
    }
    return false;
}

bool IsBlank(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](char character) { return character == ' '; });
}

// A free text field: within its bound, no control character. Empty is
// allowed where `required` is false.
bool CheckText(const std::string& text, size_t maximum, bool required,
               const std::string& what, std::string& error) {
    if (text.size() > maximum) {
        error = what + " exceeds " + std::to_string(maximum) + " bytes";
        return false;
    }
    if (HasControlCharacter(text)) {
        error = what + " holds a control character";
        return false;
    }
    if (required && IsBlank(text)) {
        error = what + " is empty";
        return false;
    }
    return true;
}

bool CheckList(const std::vector<std::string>& items, size_t maximumItems,
               size_t maximumBytes, const std::string& what,
               std::string& error) {
    if (items.size() > maximumItems) {
        error = what + " holds more than " + std::to_string(maximumItems) +
                " entries";
        return false;
    }
    std::set<std::string> seen;
    for (size_t index = 0; index < items.size(); ++index) {
        const std::string label = what + "[" + std::to_string(index) + "]";
        if (!CheckText(items[index], maximumBytes, true, label, error))
            return false;
        if (!seen.insert(FoldShotText(items[index])).second) {
            error = label + " repeats '" + items[index] + "'";
            return false;
        }
    }
    return true;
}

bool IsDigit(char character) { return character >= '0' && character <= '9'; }

int Number(const std::string& text, size_t first, size_t count) {
    int value = 0;
    for (size_t index = first; index < first + count; ++index)
        value = value * 10 + (text[index] - '0');
    return value;
}

std::string TimeText(const RationalTime& time) {
    return std::to_string(time.value) + "/" + std::to_string(time.rate);
}

bool ValidTime(const RationalTime& time) { return time.rate > 0; }

// Two-byte UTF-8 for U+00C0..U+00FF, folded to the ASCII letter it carries.
// U+00D7 and U+00F7 are signs, kept as they are.
const char* FoldLatin1(unsigned int codepoint) {
    static const char* const kFolds[64] = {
        // C0..CF: À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i",
        "i", "i",
        // D0..DF: Ð Ñ Ò Ó Ô Õ Ö × Ø Ù Ú Û Ü Ý Þ ß
        "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u",
        "y", "th", "ss",
        // E0..EF
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i",
        "i", "i",
        // F0..FF
        "d", "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u", "u",
        "y", "th", "y"};
    if (codepoint < 0xC0 || codepoint > 0xFF) return nullptr;
    return kFolds[codepoint - 0xC0];
}

std::vector<std::string> Words(const std::string& folded) {
    std::vector<std::string> words;
    std::string current;
    for (const char character : folded) {
        const unsigned char byte = static_cast<unsigned char>(character);
        const bool word = (byte >= 'a' && byte <= 'z') ||
                          (byte >= '0' && byte <= '9') || byte >= 0x80;
        if (word) {
            current.push_back(character);
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    return words;
}

std::string Haystack(const ShotDescription& shot) {
    std::string text;
    for (const std::string& subject : shot.subjects) text += subject + "\n";
    text += shot.action + "\n" + shot.place + "\n";
    for (const std::string& keyword : shot.keywords) text += keyword + "\n";
    text += shot.reserve;
    return FoldShotText(text);
}

template <typename T>
bool AnyOf(const std::vector<T>& values, const T& value) {
    return values.empty() ||
           std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

const std::vector<std::string>& ShotSizeNames() {
    static const std::vector<std::string> names = Sizes().Names();
    return names;
}

const std::vector<std::string>& CameraMovementNames() {
    static const std::vector<std::string> names = Movements().Names();
    return names;
}

const std::vector<std::string>& ShotQualityNames() {
    static const std::vector<std::string> names = Qualities().Names();
    return names;
}

const std::vector<std::string>& ShotAuthorKindNames() {
    static const std::vector<std::string> names = AuthorKinds().Names();
    return names;
}

std::string ShotSizeName(ShotSize size) { return Sizes().NameOf(size); }

std::string CameraMovementName(CameraMovement movement) {
    return Movements().NameOf(movement);
}

std::string ShotQualityName(ShotQuality quality) {
    return Qualities().NameOf(quality);
}

std::string ShotAuthorKindName(ShotAuthorKind kind) {
    return AuthorKinds().NameOf(kind);
}

bool ParseShotSize(const std::string& name, ShotSize& size) {
    return Sizes().Parse(name, size);
}

bool ParseCameraMovement(const std::string& name, CameraMovement& movement) {
    return Movements().Parse(name, movement);
}

bool ParseShotQuality(const std::string& name, ShotQuality& quality) {
    return Qualities().Parse(name, quality);
}

bool ParseShotAuthorKind(const std::string& name, ShotAuthorKind& kind) {
    return AuthorKinds().Parse(name, kind);
}

bool IsShotTimestamp(const std::string& text) {
    // YYYY-MM-DDTHH:MM:SSZ
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        text[19] != 'Z')
        return false;
    for (const size_t index :
         {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u})
        if (!IsDigit(text[index])) return false;
    const int year = Number(text, 0, 4);
    const int month = Number(text, 5, 2);
    const int day = Number(text, 8, 2);
    if (month < 1 || month > 12 || day < 1) return false;
    static const int kDays[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int days = kDays[month - 1] + (month == 2 && leap ? 1 : 0);
    return day <= days && Number(text, 11, 2) < 24 &&
           Number(text, 14, 2) < 60 && Number(text, 17, 2) < 60;
}

bool ValidateShotDescription(const LibraryMedia& media,
                             const ShotDescription& shot, std::string& error) {
    const std::string what = "shot '" + shot.id + "'";
    if (!IsValidUlid(shot.id)) {
        error = "shot id '" + shot.id + "' is not a valid ULID";
        return false;
    }
    if (!ValidTime(shot.source_in) || !ValidTime(shot.duration) ||
        shot.source_in.value < 0 || shot.duration.value <= 0) {
        error = what +
                " needs a source_in from zero and a positive duration, "
                "exact (value/rate with a positive rate)";
        return false;
    }
    try {
        const RationalTime end = shot.source_in.add(shot.duration);
        if (ValidTime(media.duration) && media.duration < end) {
            error = what + " ends at " + TimeText(end) +
                    " s, past the end of media '" + media.id + "' (" +
                    TimeText(media.duration) + " s)";
            return false;
        }
    } catch (const std::exception& exception) {
        error = what + ": " + exception.what();
        return false;
    }
    if (ShotSizeName(shot.size).empty() ||
        CameraMovementName(shot.movement).empty() ||
        ShotQualityName(shot.quality).empty() ||
        ShotAuthorKindName(shot.author_kind).empty()) {
        error = what + " names a value outside its closed vocabulary";
        return false;
    }
    if (!CheckList(shot.subjects, kMaxShotSubjects, kMaxShotSubjectBytes,
                   what + " subjects", error) ||
        !CheckText(shot.action, kMaxShotActionBytes, false, what + " action",
                   error) ||
        !CheckText(shot.place, kMaxShotPlaceBytes, false, what + " place",
                   error) ||
        !CheckText(shot.reserve, kMaxShotReserveBytes, false, what + " reserve",
                   error) ||
        !CheckList(shot.keywords, kMaxShotKeywords, kMaxShotKeywordBytes,
                   what + " keywords", error) ||
        !CheckText(shot.author, kMaxShotAuthorBytes, true, what + " author",
                   error))
        return false;
    // A shot held back says why: "usable" or "poor" with nothing to read is
    // a verdict nobody can check.
    if (shot.quality != ShotQuality::Good && IsBlank(shot.reserve)) {
        error = what + " is '" + ShotQualityName(shot.quality) +
                "' and needs a reserve that says why";
        return false;
    }
    if (!IsShotTimestamp(shot.described_at)) {
        error = what +
                " needs described_at as YYYY-MM-DDTHH:MM:SSZ (UTC), "
                "got '" +
                shot.described_at + "'";
        return false;
    }
    return true;
}

bool ValidateMediaShots(const LibraryMedia& media,
                        const std::vector<ShotDescription>& shots,
                        std::string& error) {
    if (shots.empty()) return true;
    if (media.metadata_complete && !media.has_video) {
        error = "media '" + media.id +
                "' is audio-only: there is no picture to describe";
        return false;
    }
    if (shots.size() > kMaxShotsPerMedia) {
        error = "media '" + media.id + "' holds more than " +
                std::to_string(kMaxShotsPerMedia) + " described shots";
        return false;
    }
    std::set<Ulid> ids;
    for (size_t index = 0; index < shots.size(); ++index) {
        const ShotDescription& shot = shots[index];
        if (!ValidateShotDescription(media, shot, error)) return false;
        if (!ids.insert(shot.id).second) {
            error = "shot id '" + shot.id + "' appears twice";
            return false;
        }
        if (index == 0) continue;
        const ShotDescription& previous = shots[index - 1];
        if (shot.source_in < previous.source_in) {
            error = "the shots of media '" + media.id +
                    "' are not in increasing source_in";
            return false;
        }
        if (shot.source_in < previous.source_in.add(previous.duration)) {
            error =
                "shot '" + shot.id + "' overlaps shot '" + previous.id + "'";
            return false;
        }
    }
    return true;
}

bool MergeShotDescriptions(const LibraryMedia& media,
                           const std::vector<ShotDescription>& written,
                           const std::vector<Ulid>& removed,
                           std::vector<ShotDescription>& merged,
                           std::string& error) {
    merged = media.shots;
    std::set<Ulid> writtenIds;
    for (const ShotDescription& shot : written) {
        if (!writtenIds.insert(shot.id).second) {
            error = "shot id '" + shot.id + "' is written twice";
            return false;
        }
    }
    std::set<Ulid> removedIds;
    for (const Ulid& id : removed) {
        if (!removedIds.insert(id).second) {
            error = "shot id '" + id + "' is removed twice";
            return false;
        }
        if (writtenIds.count(id) != 0) {
            error = "shot id '" + id + "' is both written and removed";
            return false;
        }
        const auto found = std::find_if(
            merged.begin(), merged.end(),
            [&](const ShotDescription& shot) { return shot.id == id; });
        if (found == merged.end()) {
            error = "media '" + media.id + "' has no described shot '" + id +
                    "' to remove";
            return false;
        }
        merged.erase(found);
    }
    std::set<Ulid> added;
    for (const ShotDescription& shot : written) {
        const auto found = std::find_if(
            merged.begin(), merged.end(),
            [&](const ShotDescription& kept) { return kept.id == shot.id; });
        if (found != merged.end()) {
            *found = shot;
        } else {
            merged.push_back(shot);
            added.insert(shot.id);
        }
    }
    for (const ShotDescription& shot : merged) {
        if (!ValidateShotDescription(media, shot, error)) return false;
    }
    std::stable_sort(
        merged.begin(), merged.end(),
        [](const ShotDescription& left, const ShotDescription& right) {
            if (left.source_in != right.source_in)
                return left.source_in < right.source_in;
            return left.id < right.id;
        });
    // The overlap a person can act on: which description is in the way,
    // and the two ways out.
    for (size_t index = 1; index < merged.size(); ++index) {
        const ShotDescription& previous = merged[index - 1];
        const ShotDescription& shot = merged[index];
        if (!(shot.source_in < previous.source_in.add(previous.duration)))
            continue;
        const bool previousIsNew = added.count(previous.id) != 0;
        const bool shotIsNew = added.count(shot.id) != 0;
        const ShotDescription& incoming =
            shotIsNew || !previousIsNew ? shot : previous;
        const ShotDescription& standing = &incoming == &shot ? previous : shot;
        error = "shot '" + incoming.id + "' (" + TimeText(incoming.source_in) +
                " s + " + TimeText(incoming.duration) + " s) overlaps shot '" +
                standing.id + "' (" + TimeText(standing.source_in) + " s + " +
                TimeText(standing.duration) +
                " s); pass its shot_id to replace it, or name it in "
                "remove_shot_ids";
        return false;
    }
    return ValidateMediaShots(media, merged, error);
}

std::string FoldShotText(const std::string& text) {
    std::string folded;
    folded.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80) {
            folded.push_back(static_cast<char>(
                byte >= 'A' && byte <= 'Z' ? byte - 'A' + 'a' : byte));
            continue;
        }
        if (index + 1 < text.size()) {
            const unsigned char next =
                static_cast<unsigned char>(text[index + 1]);
            if (byte == 0xC3 && (next & 0xC0) == 0x80) {
                const unsigned int codepoint =
                    ((byte & 0x1F) << 6) | (next & 0x3F);
                if (const char* ascii = FoldLatin1(codepoint)) {
                    folded += ascii;
                    ++index;
                    continue;
                }
            }
            // U+0152 and U+0153: the French ligature.
            if (byte == 0xC5 && (next == 0x92 || next == 0x93)) {
                folded += "oe";
                ++index;
                continue;
            }
        }
        folded.push_back(static_cast<char>(byte));
    }
    return folded;
}

std::vector<ShotMatch> SearchShots(const std::vector<LibraryMedia>& library,
                                   const ShotQuery& query) {
    const std::vector<std::string> terms = Words(FoldShotText(query.text));
    std::vector<ShotMatch> matches;
    for (const LibraryMedia& media : library) {
        if (!AnyOf(query.media_ids, media.id)) continue;
        for (const ShotDescription& shot : media.shots) {
            if (!AnyOf(query.sizes, shot.size) ||
                !AnyOf(query.movements, shot.movement) ||
                !AnyOf(query.qualities, shot.quality))
                continue;
            if (query.author_kind && *query.author_kind != shot.author_kind)
                continue;
            if (query.minimum_duration.value > 0 &&
                shot.duration < query.minimum_duration)
                continue;
            if (!terms.empty()) {
                const std::string haystack = Haystack(shot);
                const bool all = std::all_of(
                    terms.begin(), terms.end(), [&](const std::string& term) {
                        return haystack.find(term) != std::string::npos;
                    });
                if (!all) continue;
            }
            matches.push_back({&media, &shot});
        }
    }
    return matches;
}

const ShotDescription* ShotAt(const LibraryMedia& media,
                              const RationalTime& time) {
    for (const ShotDescription& shot : media.shots) {
        if (time < shot.source_in) return nullptr;
        if (time < shot.source_in.add(shot.duration)) return &shot;
    }
    return nullptr;
}

std::vector<const ShotDescription*> ShotsOverlapping(const LibraryMedia& media,
                                                     const RationalTime& start,
                                                     const RationalTime& end) {
    std::vector<const ShotDescription*> found;
    for (const ShotDescription& shot : media.shots) {
        if (!(shot.source_in < end)) break;
        if (start < shot.source_in.add(shot.duration)) found.push_back(&shot);
    }
    return found;
}

RationalTime DescribedDuration(const LibraryMedia& media) {
    RationalTime total{0, 1};
    for (const ShotDescription& shot : media.shots)
        total = total.add(shot.duration);
    return total;
}
