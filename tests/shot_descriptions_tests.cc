// PL1 to PL4 -- docs/roadmap/PLANS-2026-10.md. Described shots: their
// validation, the operation that writes them (apply, undo, journal), the
// document version that carries them, the merge describe_shots makes, the
// search, and what tells described from undescribed. Portable: no FFmpeg,
// no AppKit, no file.

#include "Document.h"
#include "DocumentFormat.h"
#include "EditLog.h"
#include "OperationFormat.h"
#include "Operations.h"
#include "Project.h"
#include "ShotDescriptions.h"

#include <iostream>
#include <string>
#include <variant>
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

const Ulid kRush = "01K90000000000000000000R01";
const Ulid kOther = "01K90000000000000000000R02";
const Ulid kSound = "01K90000000000000000000R03";

LibraryMedia Rush(const Ulid& id, const std::string& name) {
    LibraryMedia media;
    media.id = id;
    media.path = name;
    media.filename = name;
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.rate = {25, 1};
    media.duration = {1000, 25};  // 40 s
    media.orientation = "landscape";
    return media;
}

Document ShotDocument() {
    Document document;
    document.sources = {
        {kRush, "A.MP4", {25, 1}, {1000, 25}},
        {kOther, "B.MP4", {25, 1}, {1000, 25}},
        {kSound, "C.WAV", {25, 1}, {1000, 25}},
    };
    document.library.push_back(Rush(kRush, "A.MP4"));
    document.library.push_back(Rush(kOther, "B.MP4"));
    LibraryMedia sound = Rush(kSound, "C.WAV");
    sound.codec = "pcm_s16le";
    sound.has_video = false;
    sound.has_audio = true;
    sound.audio_rate = 48000;
    sound.audio_channels = 2;
    sound.width = 0;
    sound.height = 0;
    sound.orientation = "audio";
    document.library.push_back(sound);
    return document;
}

ShotDescription Shot(const Ulid& id, int64_t from, int64_t frames) {
    ShotDescription shot;
    shot.id = id;
    shot.source_in = {from, 25};
    shot.duration = {frames, 25};
    shot.size = ShotSize::Wide;
    shot.subjects = {"Nadia", "un vélo"};
    shot.action = "traverse la cour";
    shot.place = "cour de la ferme";
    shot.movement = CameraMovement::Static;
    shot.keywords = {"moisson"};
    shot.author_kind = ShotAuthorKind::Model;
    shot.author = "claude-opus-5-5";
    shot.described_at = "2026-10-21T09:30:00Z";
    return shot;
}

bool Apply(EditLog& log, Document& document, Operation operation,
           const std::string& label) {
    EditError error = EditError::None;
    std::string message;
    const bool result =
        log.Apply(document, std::move(operation), error, message);
    Check(result,
          label + " failed with " + EditErrorName(error) + ": " + message);
    return result;
}

void ExpectRejected(Document document, Operation operation, EditError expected,
                    const std::string& label) {
    const std::string before = document.SaveToString();
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    Check(!log.Apply(document, std::move(operation), error, message),
          label + " must be rejected");
    Check(error == expected, label + " expected " + EditErrorName(expected) +
                                 ", got " + EditErrorName(error) + ": " +
                                 message);
    Check(document.SaveToString() == before,
          label + " must leave the document byte-identical");
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

template <typename Enum, typename Table>
std::vector<std::string> TableNames(const Table& names) {
    std::vector<std::string> out;
    for (const auto& entry : names) out.emplace_back(entry.second);
    return out;
}

}  // namespace

int main() {
    Test("the three spellings of each vocabulary agree", [] {
        Check(ShotSizeNames() ==
                      TableNames<ShotSize>(
                          document_format::EnumNames<ShotSize>::kNames) &&
                  ShotSizeNames() ==
                      TableNames<ShotSize>(
                          operation_format::EnumNames<ShotSize>::kNames),
              "shot sizes");
        Check(CameraMovementNames() ==
                      TableNames<CameraMovement>(
                          document_format::EnumNames<CameraMovement>::kNames) &&
                  CameraMovementNames() ==
                      TableNames<CameraMovement>(
                          operation_format::EnumNames<CameraMovement>::kNames),
              "camera movements");
        Check(ShotQualityNames() ==
                      TableNames<ShotQuality>(
                          document_format::EnumNames<ShotQuality>::kNames) &&
                  ShotQualityNames() ==
                      TableNames<ShotQuality>(
                          operation_format::EnumNames<ShotQuality>::kNames),
              "qualities");
        Check(ShotAuthorKindNames() ==
                      TableNames<ShotAuthorKind>(
                          document_format::EnumNames<ShotAuthorKind>::kNames) &&
                  ShotAuthorKindNames() ==
                      TableNames<ShotAuthorKind>(
                          operation_format::EnumNames<ShotAuthorKind>::kNames),
              "author kinds");
        ShotSize size = ShotSize::Wide;
        Check(ParseShotSize("extreme_close", size) &&
                  size == ShotSize::ExtremeClose &&
                  !ParseShotSize("gros plan", size) &&
                  !ParseShotSize("Close", size),
              "a size parses by its exact spelling only");
    });

    Test("a description is refused field by field", [] {
        const LibraryMedia media = Rush(kRush, "A.MP4");
        std::string error;
        Check(ValidateShotDescription(
                  media, Shot("01K90000000000000000000S01", 0, 50), error),
              "a well-formed description passes: " + error);
        const auto refused = [&](ShotDescription shot, const std::string& why,
                                 const std::string& label) {
            std::string message;
            Check(!ValidateShotDescription(media, shot, message) &&
                      Contains(message, why),
                  label + ": " + message);
        };
        ShotDescription shot = Shot("01K90000000000000000000S01", 0, 50);
        ShotDescription bad = shot;
        bad.id = "not-a-ulid";
        refused(bad, "not a valid ULID", "a bad id");
        bad = shot;
        bad.duration = {0, 25};
        refused(bad, "positive duration", "an empty stretch");
        bad = shot;
        bad.source_in = {-1, 25};
        refused(bad, "source_in from zero", "a negative start");
        bad = shot;
        bad.source_in = {990, 25};
        bad.duration = {20, 25};
        refused(bad, "past the end of media", "a stretch past the rush");
        bad = shot;
        bad.quality = ShotQuality::Usable;
        refused(bad, "needs a reserve", "a held-back shot without a reason");
        bad = shot;
        bad.author.clear();
        refused(bad, "author is empty", "no author");
        bad = shot;
        bad.described_at = "2026-02-30T09:30:00Z";
        refused(bad, "described_at", "the 30th of February");
        bad = shot;
        bad.described_at = "2026-10-21 09:30";
        refused(bad, "described_at", "another date format");
        bad = shot;
        bad.action = "ligne\nsuivante";
        refused(bad, "control character", "a control character");
        bad = shot;
        bad.subjects = std::vector<std::string>(kMaxShotSubjects + 1, "x");
        refused(bad, "more than 8", "too many subjects");
        bad = shot;
        bad.keywords = {"Vélo", "velo"};
        refused(bad, "repeats", "a keyword twice, up to case and accent");
        bad = shot;
        bad.place = std::string(kMaxShotPlaceBytes + 1, 'a');
        refused(bad, "exceeds", "a place too long");
        bad = shot;
        bad.size = static_cast<ShotSize>(99);
        refused(bad, "closed vocabulary", "a size outside the vocabulary");
    });

    Test("a media's list is ordered, distinct and without overlap", [] {
        const LibraryMedia media = Rush(kRush, "A.MP4");
        std::string error;
        const ShotDescription first = Shot("01K90000000000000000000S01", 0, 50);
        const ShotDescription second =
            Shot("01K90000000000000000000S02", 50, 50);
        Check(ValidateMediaShots(media, {first, second}, error),
              "two adjacent stretches pass: " + error);
        Check(!ValidateMediaShots(media, {second, first}, error) &&
                  Contains(error, "increasing source_in"),
              "out of order is refused: " + error);
        ShotDescription overlapping = second;
        overlapping.source_in = {49, 25};
        Check(!ValidateMediaShots(media, {first, overlapping}, error) &&
                  Contains(error, "overlaps"),
              "an overlap is refused: " + error);
        ShotDescription twin = second;
        twin.id = first.id;
        Check(!ValidateMediaShots(media, {first, twin}, error) &&
                  Contains(error, "appears twice"),
              "the same id twice is refused: " + error);
        LibraryMedia sound = media;
        sound.has_video = false;
        sound.has_audio = true;
        Check(!ValidateMediaShots(sound, {first}, error) &&
                  Contains(error, "audio-only"),
              "a sound has no picture to describe: " + error);
    });

    Test("SetMediaShots applies, undoes to the byte, and round-trips", [] {
        Document document = ShotDocument();
        const std::string initial = document.SaveToString();
        Check(Contains(initial, "\"version\": 7") &&
                  !Contains(initial, "\"shots\""),
              "a library without a description keeps version 7 and no key");
        EditLog log;
        const std::vector<ShotDescription> shots = {
            Shot("01K90000000000000000000S01", 0, 50),
            Shot("01K90000000000000000000S02", 100, 75)};
        Check(Apply(log, document, SetMediaShotsOperation{kRush, shots},
                    "describe"),
              "describing applies");
        const std::string described = document.SaveToString();
        Check(Contains(described, "\"version\": 19") &&
                  Contains(described, "\"shots\":[") &&
                  Contains(described, "\"size\":\"wide\"") &&
                  Contains(described, "\"movement\":\"static\"") &&
                  !Contains(described, "\"reserve\""),
              "the document declares version 19 and writes what is set: " +
                  described);
        Document reread;
        std::string message;
        Check(Document::LoadFromString(described, reread, message) &&
                  reread.SaveToString() == described,
              "a version-19 document reads back to the same bytes: " + message);

        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<SetMediaShotsOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "SetMediaShots round-trips through the journal exactly: " + json);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "undo gives back the version-7 document byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == described,
              "redo gives back the described one");
        Check(
            Apply(log, document, SetMediaShotsOperation{kRush, {}}, "forget") &&
                document.SaveToString() == initial,
            "an empty list leaves no key and version 7 again");

        ExpectRejected(
            document,
            SetMediaShotsOperation{"01K90000000000000000000R99", shots},
            EditError::UnknownMedia, "an unknown media");
        ExpectRejected(document, SetMediaShotsOperation{kSound, shots},
                       EditError::ValidationFailed, "an audio-only media");
        ExpectRejected(document,
                       SetMediaShotsOperation{kRush, {shots[1], shots[0]}},
                       EditError::ValidationFailed, "an unordered list");
        // One space of IDs: a shot cannot take a media's id.
        ShotDescription clash = shots[0];
        clash.id = kOther;
        ExpectRejected(document, SetMediaShotsOperation{kRush, {clash}},
                       EditError::ValidationFailed,
                       "a shot id another object holds");
    });

    Test("an older version holding a shot is refused", [] {
        Document document = ShotDocument();
        document.library[0].shots = {Shot("01K90000000000000000000S01", 0, 50)};
        std::string text = document.SaveToString();
        const std::string declared = "\"version\": 19";
        Check(Contains(text, declared), "written at 19");
        text.replace(text.find(declared), declared.size(), "\"version\": 18");
        Document reread;
        std::string message;
        Check(!Document::LoadFromString(text, reread, message) &&
                  Contains(message, "cannot hold a described shot"),
              "version 18 with a shot is refused: " + message);
    });

    Test("a project keeps the descriptions, and a relink too", [] {
        Document document = ShotDocument();
        document.library[0].shots = {Shot("01K90000000000000000000S01", 0, 50)};
        Project project = Project::FromDocument(document, "Plans");
        const std::string saved = project.SaveToString();
        Check(Contains(saved, "\"document_version\":19"),
              "the project file declares version 19");
        Project reread;
        std::string message;
        Check(Project::LoadFromString(saved, reread, message) &&
                  reread.SaveToString() == saved &&
                  reread.rushes[0].shots.size() == 1,
              "and reads back to the same bytes: " + message);
        LibraryMedia probed = Rush(kRush, "A-relinked.MP4");
        KeepMediaReading(project.rushes[0], probed);
        Check(probed.shots.size() == 1 &&
                  probed.shots[0].id == "01K90000000000000000000S01",
              "a new probe of the file keeps what was written about it");
    });

    Test("describe_shots merges: add, replace, remove, refuse overlap", [] {
        LibraryMedia media = Rush(kRush, "A.MP4");
        media.shots = {Shot("01K90000000000000000000S01", 0, 50),
                       Shot("01K90000000000000000000S02", 100, 50)};
        std::vector<ShotDescription> merged;
        std::string error;
        ShotDescription added = Shot("01K90000000000000000000S03", 50, 25);
        Check(MergeShotDescriptions(media, {added}, {}, merged, error) &&
                  merged.size() == 3 && merged[1].id == added.id,
              "a new stretch lands in order: " + error);
        ShotDescription replaced = media.shots[1];
        replaced.size = ShotSize::Close;
        Check(MergeShotDescriptions(media, {replaced}, {}, merged, error) &&
                  merged.size() == 2 && merged[1].size == ShotSize::Close,
              "the same id replaces: " + error);
        Check(MergeShotDescriptions(media, {}, {"01K90000000000000000000S01"},
                                    merged, error) &&
                  merged.size() == 1,
              "a removal drops: " + error);
        ShotDescription overlapping =
            Shot("01K90000000000000000000S04", 25, 50);
        Check(
            !MergeShotDescriptions(media, {overlapping}, {}, merged, error) &&
                Contains(error, "shot '01K90000000000000000000S04'") &&
                Contains(error, "overlaps shot '01K90000000000000000000S01'") &&
                Contains(error, "remove_shot_ids"),
            "an overlap names the new and the standing one: " + error);
        Check(MergeShotDescriptions(media, {overlapping},
                                    {"01K90000000000000000000S01"}, merged,
                                    error),
              "and passes once the standing one is removed: " + error);
        Check(!MergeShotDescriptions(media, {}, {"01K90000000000000000000S09"},
                                     merged, error) &&
                  Contains(error, "no described shot"),
              "removing what is not there is refused: " + error);
        Check(!MergeShotDescriptions(media, {media.shots[0]},
                                     {media.shots[0].id}, merged, error) &&
                  Contains(error, "both written and removed"),
              "written and removed at once is refused: " + error);
    });

    Test("search filters on closed fields and reads words without accents", [] {
        Document document = ShotDocument();
        ShotDescription courtyard = Shot("01K90000000000000000000S01", 0, 50);
        ShotDescription hands = Shot("01K90000000000000000000S02", 100, 25);
        hands.size = ShotSize::Insert;
        hands.subjects = {"mains"};
        hands.action = "tapent sur un clavier";
        hands.place = "";
        hands.keywords = {"travail", "Ordinateur"};
        hands.movement = CameraMovement::Handheld;
        hands.quality = ShotQuality::Usable;
        hands.reserve = "léger flou au début";
        ShotDescription street = Shot("01K90000000000000000000S03", 0, 100);
        street.size = ShotSize::ExtremeWide;
        street.subjects = {"rue"};
        street.action = "des vélos passent";
        street.keywords = {};
        street.place = "boulevard";
        street.author_kind = ShotAuthorKind::Person;
        street.author = "Nadia";
        document.library[0].shots = {courtyard, hands};
        document.library[1].shots = {street};

        const auto ids = [&](const ShotQuery& query) {
            std::vector<std::string> out;
            for (const ShotMatch& match : SearchShots(document.library, query))
                out.push_back(match.shot->id.substr(23));
            return out;
        };
        ShotQuery all;
        Check(ids(all) == std::vector<std::string>{"S01", "S02", "S03"},
              "no filter answers every shot, in library then time "
              "order");
        ShotQuery text;
        text.text = "VELO";
        Check(ids(text) == std::vector<std::string>{"S01", "S03"},
              "a word matches without case or accent, as a substring");
        text.text = "velo cour";
        Check(ids(text) == std::vector<std::string>{"S01"},
              "every word must appear");
        text.text = "flou";
        Check(ids(text) == std::vector<std::string>{"S02"},
              "the reserve is read too");
        text.text = "ordinateur";
        Check(ids(text) == std::vector<std::string>{"S02"},
              "keywords are read");
        ShotQuery closed;
        closed.sizes = {ShotSize::Insert, ShotSize::ExtremeWide};
        Check(ids(closed) == std::vector<std::string>{"S02", "S03"},
              "any of the sizes given");
        closed.movements = {CameraMovement::Static};
        Check(ids(closed) == std::vector<std::string>{"S03"},
              "and every filter at once");
        ShotQuery quality;
        quality.qualities = {ShotQuality::Good};
        quality.author_kind = ShotAuthorKind::Model;
        Check(ids(quality) == std::vector<std::string>{"S01"},
              "quality and author kind filter");
        ShotQuery longer;
        longer.minimum_duration = {2, 1};
        Check(ids(longer) == std::vector<std::string>{"S01", "S03"},
              "a minimum duration keeps the stretches at least that "
              "long");
        ShotQuery scoped;
        scoped.media_ids = {kOther};
        Check(ids(scoped) == std::vector<std::string>{"S03"},
              "media_ids scope the search");
        Check(FoldShotText("Œuvre À Éte ÇA") == "oeuvre a ete ca",
              "folding: " + FoldShotText("Œuvre À Éte ÇA"));
    });

    Test("what covers a time, and what overlaps a window", [] {
        LibraryMedia media = Rush(kRush, "A.MP4");
        media.shots = {Shot("01K90000000000000000000S01", 0, 50),
                       Shot("01K90000000000000000000S02", 100, 50)};
        Check(ShotAt(media, {0, 25}) == &media.shots[0] &&
                  ShotAt(media, {49, 25}) == &media.shots[0] &&
                  ShotAt(media, {50, 25}) == nullptr &&
                  ShotAt(media, {4, 1}) == &media.shots[1] &&
                  ShotAt(media, {150, 25}) == nullptr,
              "a time inside a stretch, its end excluded");
        Check(ShotsOverlapping(media, {40, 25}, {110, 25}).size() == 2 &&
                  ShotsOverlapping(media, {50, 25}, {100, 25}).empty() &&
                  ShotsOverlapping(media, {149, 25}, {200, 25}).size() == 1,
              "a window shares an instant with a stretch or not");
        Check(DescribedDuration(media) == RationalTime{4, 1},
              "described duration is exact");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
