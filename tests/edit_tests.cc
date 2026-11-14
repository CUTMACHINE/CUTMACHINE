#include "AudioMixFilters.h"
#include "ClipAnimation.h"
#include "ClipGradeCopy.h"
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "Timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
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

Document EditDocument() {
    Document document;
    document.sources = {
        {"01K20000000000000000000001", "A.MP4", {25, 1}, {1000, 25}},
        {"01K20000000000000000000002", "B.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K20000000000000000000003",
         "video",
         0,
         {
             {"01K20000000000000000000004",
              "01K20000000000000000000001",
              {100, 25},
              {10, 25},
              {0, 25}},
             {"01K20000000000000000000005",
              "01K20000000000000000000001",
              {200, 25},
              {10, 25},
              {20, 25}},
             {"01K20000000000000000000006",
              "01K20000000000000000000002",
              {300, 25},
              {10, 25},
              {40, 25}},
         }},
    };
    return document;
}

// R3 -- how many times a key appears in a saved document.
size_t CountOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

uint64_t CanonicalHash(const Document& document) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned char byte : document.SaveToString()) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
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
    Check(log.AppliedCount() == 0 && log.UndoneCount() == 0,
          label + " must leave the log unchanged");
}

// PL1 -- ROADMAP.md. The edit fixture with an audio clip, and the batch the
// ticket names: an insertion, a trim and an audio setting.
const Ulid kBatchAudioClip = "01K20000000000000000000091";
const Ulid kBatchInsertedClip = "01K20000000000000000000050";

Document BatchDocument() {
    Document document = EditDocument();
    document.sequence.id = "01K2000000000000000000000Q";
    document.sequence.tracks.push_back({"01K20000000000000000000090",
                                        "audio",
                                        1,
                                        {{kBatchAudioClip,
                                          "01K20000000000000000000001",
                                          {100, 25},
                                          {10, 25},
                                          {0, 25}}}});
    return document;
}

BatchOperation InterventionBatch() {
    return {
        "Resserrer la question 2",
        {InsertClipOperation{"01K20000000000000000000003",
                             "01K20000000000000000000002",
                             {500, 25},
                             {10, 25},
                             {60, 25},
                             kBatchInsertedClip},
         TrimClipOperation{"01K20000000000000000000004",
                           TrimEdge::Tail,
                           {-2, 25},
                           std::nullopt},
         SetClipAudioOperation{kBatchAudioClip, {-6, 1}, {0, 25}, {3, 25}}}};
}

}  // namespace

int main() {
    // ITW4 -- restyling a run of cartons is one reversible edit.
    Test("caption style update round-trips and undoes byte-exactly", [] {
        Document document = EditDocument();
        CaptionStyle style;
        style.id = "01K2000000000000000000000S";
        document.sequence.caption_styles = {style};
        const std::string original = document.SaveToString();
        CaptionStyle legible = style;
        legible.font_weight = 500;
        legible.outline_width = 4;
        legible.outline_color = "#000000";
        legible.shadow_offset = 2;
        legible.shadow_color = "#000000@0.5";
        legible.background_color = "#101010@0.4";
        legible.background_padding = 12;
        legible.vertical_position = {3, 4};
        EditLog log;
        Check(Apply(log, document, UpdateCaptionStyleOperation{legible},
                    "update caption style"),
              "a caption style update applies");
        const CaptionStyle* stored = document.FindCaptionStyle(style.id);
        Check(stored && stored->outline_width == 4 &&
                  stored->font_weight == 500 &&
                  stored->vertical_position.num == 3 &&
                  stored->vertical_position.den == 4 &&
                  stored->background_color == "#101010@0.4",
              "every decoration setting is stored");
        Document reloaded;
        std::string error;
        Check(Document::LoadFromString(document.SaveToString(), reloaded,
                                       error) &&
                  reloaded.SaveToString() == document.SaveToString(),
              "the decorated style survives a save and reload: " + error);
        std::string message;
        EditError editError = EditError::None;
        Operation parsed = RemoveClipOperation{};
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(json, parsed, editError, message) &&
                  SerializeOperation(parsed) == json,
              "UpdateCaptionStyle JSON round-trips canonically");
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "the update undoes to the original bytes");
        CaptionStyle invalid = style;
        invalid.vertical_position = {5, 4};
        ExpectRejected(document, UpdateCaptionStyleOperation{invalid},
                       EditError::ValidationFailed,
                       "a vertical position below the frame");
        CaptionStyle weightless = style;
        weightless.font_weight = 0;
        ExpectRejected(document, UpdateCaptionStyleOperation{weightless},
                       EditError::ValidationFailed,
                       "a font weight outside CSS's 1 to 1000");
        CaptionStyle unknown = style;
        unknown.id = "01K2000000000000000000000T";
        ExpectRejected(document, UpdateCaptionStyleOperation{unknown},
                       EditError::UnknownCaptionStyle,
                       "an update of an unknown style");
    });

    // PA4 -- docs/roadmap/PARITE-2026-11.md. A carton's words are timed on
    // its own clock, and stay where they are said through the edits that
    // resize it.
    Test("a carton's word times follow a trim, a roll and a cut", [] {
        Document document = EditDocument();
        CaptionStyle style;
        style.id = "01K2000000000000000000000S";
        style.word_animation = kCaptionWordAnimationHighlight;
        document.sequence.caption_styles = {style};
        const Ulid first = "01K200000000000000000000C1";
        const Ulid second = "01K200000000000000000000C2";
        const auto carton = [&](const Ulid& id, int64_t in,
                                const std::string& text,
                                std::vector<RationalTime> times) {
            DocumentClip clip;
            clip.id = id;
            clip.source_id.clear();
            clip.source_in = {0, 25};
            clip.timeline_in = {in, 25};
            clip.duration = {50, 25};
            clip.include_audio = false;
            clip.caption_group_id = style.id;
            clip.caption_text = text;
            clip.caption_word_times = std::move(times);
            return clip;
        };
        DocumentTrack track;
        track.id = "01K200000000000000000000CT";
        track.kind = "caption";
        track.index = static_cast<int32_t>(document.sequence.tracks.size());
        // "un" as the carton opens, "deux" ten frames in, "trois" twenty.
        track.clips = {
            carton(first, 0, "un deux\ntrois", {{0, 1}, {2, 5}, {4, 5}}),
            carton(second, 50, "quatre cinq", {{0, 1}, {2, 5}})};
        document.sequence.tracks.push_back(track);
        std::string error;
        Check(document.Validate(error), "the timed cartons validate: " + error);
        const std::string original = document.SaveToString();
        Document reloaded;
        Check(Document::LoadFromString(original, reloaded, error) &&
                  reloaded.SaveToString() == original &&
                  reloaded.FindClip(first)->caption_word_times.size() == 3,
              "the word times survive a save and reload: " + error);
        Check(CaptionWordCount("un deux\ntrois") == 3 &&
                  CaptionWordCount("  ") == 0,
              "words are counted between spaces and line breaks");
        const auto spoken = [&](const Document& from, const Ulid& id,
                                int64_t frame) {
            return CaptionWordsSpoken(*from.FindClip(id), {frame, 25});
        };
        Check(spoken(document, first, 0) == 1 &&
                  spoken(document, first, 9) == 1 &&
                  spoken(document, first, 10) == 2 &&
                  spoken(document, first, 49) == 3,
              "a word counts from the frame it starts on");

        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        // A head trim moves the carton's start, not its words.
        Check(Apply(log, document,
                    TrimClipOperation{first, TrimEdge::Head, {5, 25}},
                    "trim the carton's head"),
              "the head trim applies");
        const DocumentClip* trimmed = document.FindClip(first);
        Check(trimmed->timeline_in == RationalTime(5, 25) &&
                  trimmed->caption_word_times[0].value == -1 &&
                  trimmed->caption_word_times[0].rate == 5 &&
                  trimmed->caption_word_times[1].value == 1 &&
                  trimmed->caption_word_times[1].rate == 5,
              "the times moved back by the trim, in lowest terms");
        Check(spoken(document, first, 9) == 1 &&
                  spoken(document, first, 10) == 2 &&
                  spoken(document, first, 20) == 3,
              "each word is still said on its frame");
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "the trim undoes to the original bytes: " + message);
        Check(log.Redo(document, editError, message) &&
                  spoken(document, first, 10) == 2 &&
                  log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "redone then undone, the same: " + message);

        // A roll moves the second carton's start under its words.
        Check(Apply(log, document,
                    RollEditOperation{{{first, second}}, {10, 25}, {}},
                    "roll the two cartons"),
              "the roll applies");
        Check(document.FindClip(second)->timeline_in == RationalTime(60, 25) &&
                  spoken(document, second, 60) == 2 &&
                  document.FindClip(first)->caption_word_times[1].value == 2,
              "the right carton's words stayed; the left carton's were not "
              "touched");
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "the roll undoes to the original bytes: " + message);

        // A cut: the right half keeps the times and starts later on the
        // same clock.
        SplitClipOperation split;
        split.clip_id = first;
        split.timeline_position = {15, 25};
        Check(Apply(log, document, split, "cut the carton"), "the cut applies");
        const DocumentTrack* cut = document.FindTrack(track.id);
        Check(cut->clips.size() == 3 &&
                  cut->clips[1].caption_word_times.size() == 3 &&
                  CaptionWordsSpoken(cut->clips[1], {15, 25}) == 2 &&
                  CaptionWordsSpoken(cut->clips[1], {20, 25}) == 3 &&
                  CaptionWordsSpoken(cut->clips[0], {10, 25}) == 2,
              "both halves say each word on its frame");
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "the cut undoes to the original bytes: " + message);

        // A text corrected keeps its times; a text of another length
        // loses them; both undo exactly.
        Check(Apply(log, document,
                    SetClipCaptionOperation{first, style.id, "Un deux trois"},
                    "correct the text"),
              "the correction applies");
        Check(document.FindClip(first)->caption_word_times.size() == 3,
              "a text of as many words keeps its times");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(json.find("\"word_times_resolved\":true") != std::string::npos &&
                  json.find("\"caption_word_times\"") != std::string::npos &&
                  DeserializeOperation(json, parsed, editError, message) &&
                  SerializeOperation(parsed) == json,
              "the journal holds what the operation decided: " + json);
        Check(Apply(log, document,
                    SetClipCaptionOperation{first, style.id, "Un deux"},
                    "shorten the text"),
              "the shorter text applies");
        Check(document.FindClip(first)->caption_word_times.empty() &&
                  spoken(document, first, 20) == 0,
              "a text of another length is drawn whole");
        Check(log.Undo(document, editError, message) &&
                  document.FindClip(first)->caption_word_times.size() == 3 &&
                  log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "both text edits undo to the original bytes: " + message);

        // What the document refuses.
        Document miscounted = document;
        miscounted.FindClip(first)->caption_word_times.pop_back();
        Check(!miscounted.Validate(error) &&
                  error.find("one time per word") != std::string::npos,
              "a time missing for a word is refused: " + error);
        Document unreduced = document;
        unreduced.FindClip(first)->caption_word_times[1] = {10, 25};
        Check(!unreduced.Validate(error) &&
                  error.find("lowest terms") != std::string::npos,
              "a time not in lowest terms is refused: " + error);
        CaptionStyle bouncing = style;
        bouncing.word_animation = "bounce";
        ExpectRejected(document, UpdateCaptionStyleOperation{bouncing},
                       EditError::ValidationFailed,
                       "an animation the renderer does not draw");
        CaptionStyle revealing = style;
        revealing.word_animation = kCaptionWordAnimationReveal;
        revealing.highlight_color = "#00ffcc";
        Check(Apply(log, document, UpdateCaptionStyleOperation{revealing},
                    "restyle"),
              "the animation is a setting of the style");
        Check(document.SaveToString().find("\"word_animation\":\"reveal\"") !=
                      std::string::npos &&
                  original.find("\"highlight_color\"") == std::string::npos,
              "the animation is written; a default colour is not");
    });

    // PA8 -- docs/roadmap/PARITE-2026-11.md. A clip's mask is one
    // reversible edit, reduced when stored, refused where it has no place.
    Test("a clip mask applies, reduces, declares its version and undoes", [] {
        Document document = EditDocument();
        const Ulid clipId = "01K20000000000000000000004";
        const std::string original = document.SaveToString();
        Check(original.find("\"mask\"") == std::string::npos &&
                  original.find("\"version\": 7") != std::string::npos,
              "a document that masks nothing spells no mask, at version 7");
        ClipMask window;
        window.shape = kClipMaskEllipse;
        window.center_x = {2, 4};
        window.center_y = {40, 100};
        window.width = {3, 6};
        window.height = {1, 1};
        window.feather = {25, 100};
        EditLog log;
        Check(Apply(log, document, SetClipMaskOperation{clipId, window},
                    "mask the clip"),
              "the mask applies");
        const DocumentClip* masked = document.FindClip(clipId);
        Check(ClipHasMask(*masked) && masked->mask.shape == "ellipse" &&
                  masked->mask.center_x.num == 1 &&
                  masked->mask.center_x.den == 2 &&
                  masked->mask.center_y.num == 2 &&
                  masked->mask.center_y.den == 5 &&
                  masked->mask.width.num == 1 && masked->mask.width.den == 2 &&
                  masked->mask.feather.num == 1 &&
                  masked->mask.feather.den == 4,
              "its fractions are stored in lowest terms");
        const std::string saved = document.SaveToString();
        Check(saved.find("\"version\": 26") != std::string::npos &&
                  saved.find("\"mask\":{\"shape\":\"ellipse\"") !=
                      std::string::npos,
              "a masked document declares version 26 and spells the mask");
        Document reloaded;
        std::string error;
        Check(Document::LoadFromString(saved, reloaded, error) &&
                  reloaded.SaveToString() == saved,
              "it survives a save and reload: " + error);
        std::string message;
        EditError editError = EditError::None;
        Operation parsed = RemoveClipOperation{};
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(json.find("\"center_x\":{\"num\":1,\"den\":2}") !=
                      std::string::npos &&
                  DeserializeOperation(json, parsed, editError, message) &&
                  SerializeOperation(parsed) == json,
              "the journal holds the reduced mask: " + json);

        // Another shape over it, then none: each undo steps back exactly.
        ClipMask half;
        half.shape = kClipMaskRectangle;
        half.width = {1, 2};
        half.center_x = {1, 4};
        half.inverted = true;
        Check(Apply(log, document, SetClipMaskOperation{clipId, half},
                    "another mask"),
              "a mask replaces a mask");
        ClipMask none;
        // A mask taken off forgets where it was.
        none.center_x = {1, 4};
        Check(Apply(log, document, SetClipMaskOperation{clipId, none},
                    "no mask") &&
                  !ClipHasMask(*document.FindClip(clipId)) &&
                  document.SaveToString() == original,
              "a mask without a shape removes it, back to the first bytes");
        Check(log.Undo(document, editError, message) &&
                  document.FindClip(clipId)->mask.shape == "rectangle" &&
                  document.FindClip(clipId)->mask.inverted &&
                  log.Undo(document, editError, message) &&
                  document.SaveToString() == saved &&
                  log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "three undos walk back through the masks to none: " + message);

        // What a mask refuses.
        ClipMask blob = window;
        blob.shape = "blob";
        ExpectRejected(document, SetClipMaskOperation{clipId, blob},
                       EditError::ValidationFailed, "a shape that is none");
        ClipMask flat = window;
        flat.width = {0, 1};
        ExpectRejected(document, SetClipMaskOperation{clipId, flat},
                       EditError::ValidationFailed, "a mask with no width");
        ClipMask far = window;
        far.center_y = {5, 2};
        ExpectRejected(document, SetClipMaskOperation{clipId, far},
                       EditError::ValidationFailed,
                       "a centre two pictures away");
        ClipMask soft = window;
        soft.feather = {3, 2};
        ExpectRejected(document, SetClipMaskOperation{clipId, soft},
                       EditError::ValidationFailed,
                       "a feather past the centre");
        ExpectRejected(
            document,
            SetClipMaskOperation{"01K29999999999999999999994", window},
            EditError::UnknownClip, "a mask on no clip");
    });

    // ITW7 -- a cutaway on a synchronized track follows the words removed
    // under it instead of staying put over other words.
    Test("word removal carries cutaways through the cut", [] {
        Document document;
        const Ulid source = "01K20000000000000000000001";
        document.sources = {{source, "A.MP4", {25, 1}, {1000, 25}}};
        const auto clip = [&](const Ulid& id, int64_t sourceIn,
                              int64_t duration, int64_t timelineIn) {
            return DocumentClip{
                id, source, {sourceIn, 25}, {duration, 25}, {timelineIn, 25}};
        };
        document.sequence.tracks = {
            {"01K20000000000000000000010",
             "video",
             0,
             {clip("01K20000000000000000000011", 100, 40, 0)}},
            {"01K20000000000000000000020",
             "video",
             1,
             {clip("01K20000000000000000000021", 500, 10, 2),
              clip("01K20000000000000000000022", 600, 17, 13),
              clip("01K20000000000000000000023", 700, 5, 45)}},
            {"01K20000000000000000000030",
             "video",
             2,
             {clip("01K20000000000000000000031", 800, 3, 11),
              clip("01K20000000000000000000032", 900, 25, 30)}},
        };
        const std::string before = document.SaveToString();
        RemoveWordsOperation words;
        words.clip_id = "01K20000000000000000000011";
        // Source [110, 115) is timeline [10, 15).
        words.ranges = {{{110, 25}, {115, 25}}};
        words.sync_track_ids = {"01K20000000000000000000020",
                                "01K20000000000000000000030"};
        EditLog log;
        Check(Apply(log, document, words, "remove words under cutaways"),
              "the removal applies");
        const auto at = [&](const Ulid& id) { return document.FindClip(id); };
        Check(at("01K20000000000000000000021") &&
                  at("01K20000000000000000000021")->timeline_in ==
                      RationalTime{2, 25} &&
                  at("01K20000000000000000000021")->duration ==
                      RationalTime{8, 25},
              "a cutaway ending over removed words ends with the words kept "
              "before them");
        Check(at("01K20000000000000000000022") &&
                  at("01K20000000000000000000022")->timeline_in ==
                      RationalTime{10, 25} &&
                  at("01K20000000000000000000022")->duration ==
                      RationalTime{15, 25} &&
                  at("01K20000000000000000000022")->source_in ==
                      RationalTime{600, 25},
              "a cutaway starting over removed words starts with the next "
              "kept words and stays continuous");
        Check(at("01K20000000000000000000023") &&
                  at("01K20000000000000000000023")->timeline_in ==
                      RationalTime{40, 25},
              "a cutaway after the clip moves back by the removal");
        Check(!at("01K20000000000000000000031"),
              "a cutaway lying wholly over removed words goes with them");
        Check(at("01K20000000000000000000032") &&
                  at("01K20000000000000000000032")->timeline_in ==
                      RationalTime{25, 25},
              "a cutaway starting inside the clip after the removal moves "
              "back with its words");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "the removal with its cutaways undoes byte-exactly");
    });

    // ITW7 -- every cut through a graded clip gives the new piece its own
    // effect ids; reusing them failed validation as a duplicate id.
    Test("cuts through a graded clip keep every effect id unique", [] {
        const auto graded = [] {
            Document document = EditDocument();
            DocumentClip& clip = document.sequence.tracks[0].clips[0];
            clip.duration = {18, 25};
            ClipEffect exposure;
            exposure.id = "01K2000000000000000000000E";
            exposure.type = "color.exposure";
            exposure.params["stops"] = {-1, 2};
            clip.effects = {exposure};
            return document;
        };
        {
            Document document = graded();
            const std::string before = document.SaveToString();
            RemoveWordsOperation words;
            words.clip_id = "01K20000000000000000000004";
            words.ranges = {{{104, 25}, {108, 25}}};
            EditLog log;
            Check(Apply(log, document, words, "remove words in a graded clip"),
                  "word removal splits a graded clip");
            const auto& clips = document.sequence.tracks[0].clips;
            Check(clips.size() == 4 && clips[1].effects.size() == 1 &&
                      clips[1].effects[0].id != clips[0].effects[0].id,
                  "the second fragment carries its own effect id");
            EditError error = EditError::None;
            std::string message;
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "graded word removal undoes byte-exactly");
        }
        {
            Document document = graded();
            PasteClipsOperation paste;
            paste.overwrite = true;
            PastedClip pasted;
            pasted.copied_clip_id = "01K20000000000000000000006";
            pasted.track_id = "01K20000000000000000000003";
            pasted.source_id = "01K20000000000000000000002";
            pasted.source_in = {300, 25};
            pasted.duration = {4, 25};
            pasted.timeline_in = {6, 25};
            paste.clips = {pasted};
            EditLog log;
            Check(Apply(log, document, paste, "overwrite paste into grade"),
                  "an overwrite paste splits a graded clip");
        }
        {
            Document document = graded();
            MoveClipOperation move{"01K20000000000000000000006",
                                   "01K20000000000000000000003",
                                   {6, 25},
                                   {}};
            EditLog log;
            Check(Apply(log, document, move, "overwrite move into grade"),
                  "an overwrite move splits a graded clip");
        }
    });

    // Out of lot -- the author, 29 October 2026: "cut shots stay linked to each
    // other". A word removal through an A/V pair leaves one pair per kept
    // stretch, as a cut in the window does, instead of every stretch of
    // picture and sound in the original group.
    Test("word removal pairs each kept stretch on its own", [] {
        Document document = EditDocument();
        const Ulid picture = "01K20000000000000000000004";
        const Ulid sound = "01K20000000000000000000041";
        const Ulid group = "01K20000000000000000000049";
        DocumentClip& video = document.sequence.tracks[0].clips[0];
        video.duration = {18, 25};
        video.link_group_id = group;
        video.sync_anchor_clip_id = picture;
        DocumentClip audio = video;
        audio.id = sound;
        document.sequence.tracks.push_back(
            {"01K20000000000000000000040", "audio", 1, {audio}});
        const std::string before = document.SaveToString();
        RemoveWordsOperation words;
        words.clip_id = picture;
        words.ranges = {{{104, 25}, {106, 25}}, {{110, 25}, {112, 25}}};
        words.linked_clip_ids = {sound};
        EditLog log;
        Check(Apply(log, document, words, "remove words through a pair"),
              "the removal applies to the pair");
        const auto stretch = [&](size_t track, int64_t sourceIn) {
            const auto& clips = document.sequence.tracks[track].clips;
            const auto found = std::find_if(
                clips.begin(), clips.end(), [&](const DocumentClip& clip) {
                    return clip.source_in == RationalTime{sourceIn, 25};
                });
            return found == clips.end() ? nullptr : &*found;
        };
        const DocumentClip* firstPicture = stretch(0, 100);
        const DocumentClip* firstSound = stretch(1, 100);
        Check(firstPicture && firstSound && firstPicture->id == picture &&
                  firstSound->id == sound &&
                  firstPicture->link_group_id == group &&
                  firstSound->link_group_id == group,
              "the first stretch keeps its ids and the original group");
        Ulid previous = group;
        for (const int64_t sourceIn : {106, 112}) {
            const DocumentClip* stretchPicture = stretch(0, sourceIn);
            const DocumentClip* stretchSound = stretch(1, sourceIn);
            const std::string at = std::to_string(sourceIn);
            Check(stretchPicture && stretchSound &&
                      !stretchPicture->link_group_id.empty() &&
                      stretchPicture->link_group_id ==
                          stretchSound->link_group_id &&
                      stretchPicture->link_group_id != group &&
                      stretchPicture->link_group_id != previous,
                  "the stretch from " + at +
                      " is a pair of its own, linked to nothing else");
            Check(
                stretchPicture && stretchSound &&
                    stretchPicture->sync_anchor_clip_id == stretchPicture->id &&
                    stretchSound->sync_anchor_clip_id == stretchPicture->id,
                "the stretch from " + at +
                    " keeps sync against its own picture");
            if (stretchPicture) previous = stretchPicture->link_group_id;
        }
        const std::string after = document.SaveToString();
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "the paired removal undoes byte-exactly");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == after,
              "redo restores the same groups byte-exactly");
    });

    Test("color management is serializable and byte-exact on undo", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        ColorManagementSettings settings;
        settings.enabled = true;
        settings.input_gamut = "sony_sgamut3_cine";
        settings.input_transfer = "sony_slog3";
        settings.input_ycbcr_matrix = "bt709";
        settings.input_range = "full";
        settings.working_gamut = "acescct";
        settings.output_gamut = "rec2020";
        settings.output_transfer = "hlg";
        EditLog log;
        Check(Apply(log, document, SetColorManagementOperation{settings},
                    "set color management"),
              "color operation applies");
        const std::string operationJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(operationJson, parsed, error, message) &&
                  std::holds_alternative<SetColorManagementOperation>(parsed) &&
                  SerializeOperation(parsed) == operationJson,
              "SetColorManagement JSON is canonical");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "color operation undo restores byte-identical document");
    });

    Test("transition operations are exact, serializable and undoable", [] {
        Document document = EditDocument();
        document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        DocumentTransition transition{
            "01K20000000000000000000030",
            document.sequence.tracks[0].id,
            document.sequence.tracks[0].clips[0].id,
            document.sequence.tracks[0].clips[1].id,
            "cross_dissolve",
            {4, 25},
            TransitionAlignment::Center,
        };
        const std::string before = document.SaveToString();
        EditLog log;
        Check(Apply(log, document, AddTransitionOperation{transition},
                    "add transition"),
              "transition add applies");
        Check(document.FindTransition(transition.id) != nullptr,
              "added transition is addressable");
        const std::string added = document.SaveToString();
        const std::string addJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(addJson, parsed, error, message) &&
                  std::holds_alternative<AddTransitionOperation>(parsed) &&
                  SerializeOperation(parsed) == addJson,
              "AddTransition JSON round-trips exactly");

        Check(Apply(log, document,
                    UpdateTransitionOperation{transition.id,
                                              "cross_dissolve",
                                              {2, 25},
                                              TransitionAlignment::EndAtCut},
                    "update transition"),
              "transition update applies");
        Check(document.FindTransition(transition.id)->duration ==
                      RationalTime{2, 25} &&
                  document.FindTransition(transition.id)->alignment ==
                      TransitionAlignment::EndAtCut,
              "transition update changes only transition state");
        Check(Apply(log, document, RemoveTransitionOperation{transition.id},
                    "remove transition"),
              "transition remove applies");
        Check(!document.FindTransition(transition.id),
              "transition remove leaves both clips intact");
        Check(log.Undo(document, error, message) &&
                  document.FindTransition(transition.id),
              "remove undo restores transition");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == added,
              "update undo restores exact added state");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "add undo restores byte-identical document");

        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, AddTransitionOperation{transition},
                       EditError::LockedTrack,
                       "transition cannot be added to a locked track");
    });

    // EF2 -- docs/roadmap/EFFETS-2026-10.md. A cross fade is the sound's
    // transition: exact, serializable, undoable byte for byte, refused on a
    // picture track as a dissolve is refused on a sound one; and the edges
    // the mixes read from it say the same handles and the same ramps.
    Test("a cross fade joins two sounds and is undone byte for byte", [] {
        Document document = EditDocument();
        document.sequence.tracks[0].kind = "audio";
        document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        // Copies: applying an operation replaces the document.
        const Ulid trackId = document.sequence.tracks[0].id;
        DocumentTransition fade{
            "01K20000000000000000000031",
            trackId,
            document.sequence.tracks[0].clips[0].id,
            document.sequence.tracks[0].clips[1].id,
            "cross_fade",
            {4, 25},
            TransitionAlignment::Center,
        };
        DocumentTransition dissolve = fade;
        dissolve.type = "cross_dissolve";
        ExpectRejected(document, AddTransitionOperation{dissolve},
                       EditError::ValidationFailed,
                       "a cross dissolve refuses a sound track");

        const std::string before = document.SaveToString();
        EditLog log;
        Check(Apply(log, document, AddTransitionOperation{fade},
                    "add cross fade"),
              "a cross fade applies on a sound track");
        Check(document.SaveToString().find("\"version\": 24") !=
                  std::string::npos,
              "a document with a cross fade is written at version 24");
        const std::string addJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(addJson, parsed, error, message) &&
                  SerializeOperation(parsed) == addJson,
              "the cross fade's operation round-trips exactly");

        // Centred over four frames at 25 fps: two read past the cut on
        // either side, and each side ramps over all four.
        const DocumentClip& left = document.sequence.tracks[0].clips[0];
        const DocumentClip& right = document.sequence.tracks[0].clips[1];
        audio_mix::ClipAudioEdges outgoing;
        audio_mix::ClipAudioEdges incoming;
        Check(audio_mix::ResolveClipAudioEdges(document, left, trackId,
                                               outgoing, message) &&
                  audio_mix::ResolveClipAudioEdges(document, right, trackId,
                                                   incoming, message),
              "both sides' edges resolve: " + message);
        Check(outgoing.tail == RationalTime{2, 25} &&
                  outgoing.lead.value == 0 &&
                  outgoing.fade_out == RationalTime{4, 25} &&
                  outgoing.equal_power_out && !outgoing.equal_power_in &&
                  outgoing.fade_in == audio_mix::kAudioEdgeFade &&
                  outgoing.length == RationalTime{12, 25},
              "the outgoing sound runs two frames past the cut and falls "
              "over four, its head keeping the 3 ms edge");
        Check(incoming.lead == RationalTime{2, 25} &&
                  incoming.fade_in == RationalTime{4, 25} &&
                  incoming.equal_power_in && !incoming.equal_power_out &&
                  incoming.length == RationalTime{12, 25},
              "the incoming sound starts two frames before the cut and "
              "rises over four");
        std::ostringstream graph;
        audio_mix::AppendClipEnvelope(graph, right, incoming);
        Check(graph.str() ==
                  ",afade=t=in:st=0:d=0.16:curve=qsin,afade=t=out:st=0.477:"
                  "d=0.003",
              "the export ramps the incoming side on a quarter sine: " +
                  graph.str());
        const float in =
            audio_mix::ClipAudioEnvelopeAt(50, 100, 100, 0, true, false);
        const float out =
            audio_mix::ClipAudioEnvelopeAt(50, 100, 0, 100, false, true);
        Check(std::abs(in * in + out * out - 1.0f) < 1e-5f &&
                  audio_mix::ClipAudioEnvelopeAt(25, 100, 100, 0, false,
                                                 false) == 0.25f,
              "at the middle the two sides' powers add to one, and a plain "
              "fade stays linear");

        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "undoing the cross fade restores a byte-identical document");

        Document picture = EditDocument();
        picture.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        DocumentTransition wrong = fade;
        wrong.track_id = picture.sequence.tracks[0].id;
        ExpectRejected(picture, AddTransitionOperation{wrong},
                       EditError::ValidationFailed,
                       "a cross fade refuses a picture track");
    });

    // R5 -- docs/roadmap/REEL-2026-09.md. A colour dip at the edge of a
    // track: the shape the first film measured needs, and the one a
    // dissolve cannot take. Round trip apply -> undo, byte for byte, and
    // the colour carried through the operation's own JSON.
    Test("a colour dip at the edge of a track is exact and undoable", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        Check(before.find("\"version\": 7") != std::string::npos,
              "the document starts at version 7");
        DocumentTransition opening;
        opening.id = "01K20000000000000000000031";
        opening.track_id = document.sequence.tracks[0].id;
        opening.right_clip_id = document.sequence.tracks[0].clips[0].id;
        opening.type = "dip_to_color";
        opening.duration = {4, 25};
        opening.alignment = TransitionAlignment::StartAtCut;
        EditLog log;
        Check(Apply(log, document, AddTransitionOperation{opening},
                    "open on black"),
              "an opening dip applies");
        const std::string opened = document.SaveToString();
        Check(opened.find("\"version\": 13") != std::string::npos,
              "a document that dips declares version 13");
        Check(opened.find("\"left_clip_id\":\"\"") != std::string::npos,
              "and writes the missing side as an empty id");
        Check(opened.find("\"color\"") == std::string::npos,
              "black is the default and is not written");

        const std::string addJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(addJson, parsed, error, message) &&
                  std::holds_alternative<AddTransitionOperation>(parsed) &&
                  SerializeOperation(parsed) == addJson,
              "AddTransition with an empty side round-trips exactly");
        Check(addJson.find("\"color\"") == std::string::npos,
              "and a black dip adds no key to the journal");

        UpdateTransitionOperation toWhite;
        toWhite.transition_id = opening.id;
        toWhite.type = "dip_to_color";
        toWhite.duration = {4, 25};
        toWhite.alignment = TransitionAlignment::StartAtCut;
        toWhite.color = "#ffffff";
        Check(Apply(log, document, toWhite, "open on white"),
              "the colour changes through an operation");
        Check(document.FindTransition(opening.id)->color == "#ffffff",
              "and reaches the document");
        const std::string updateJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(updateJson.find("\"color\":\"#ffffff\"") != std::string::npos,
              "a colour other than black is spelled in the journal");
        Check(document.SaveToString().find("\"color\":\"#ffffff\"") !=
                  std::string::npos,
              "and in the document");

        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == opened,
              "undoing the colour restores the exact opened state");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "undoing the dip restores a byte-identical version-7 document");

        // A dissolve cannot take the same shape: there is nothing on the
        // other side to dissolve with.
        DocumentTransition impossible = opening;
        impossible.type = "cross_dissolve";
        ExpectRejected(document, AddTransitionOperation{impossible},
                       EditError::ValidationFailed,
                       "a cross dissolve refuses the edge of a track");
    });

    // R6 -- docs/roadmap/REEL-2026-09.md. The look of a sequence: a
    // reference and not a number, laid and taken away by one reversible
    // operation. Round trip apply -> undo, byte for byte, and the version
    // that follows what the file spells.
    Test("a sequence look is exact, versioned and undoable", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        Check(before.find("\"version\": 7") != std::string::npos,
              "the document starts at version 7");
        Check(before.find("look_lut") == std::string::npos,
              "a sequence without a look writes no key for it");

        SequenceLut look;
        look.digest = std::string(64, 'b');
        look.name = "look-v2.cube";
        EditLog log;
        Check(Apply(log, document, SetSequenceLutOperation{look},
                    "look de séquence"),
              "a look applies");
        const std::string looked = document.SaveToString();
        Check(looked.find("\"version\": 14") != std::string::npos,
              "a document that carries a look declares version 14");
        Check(looked.find("\"digest\":\"" + std::string(64, 'b') + "\"") !=
                  std::string::npos,
              "and writes the digest it names");
        // Guard rail 2: what the document carries is a reference, spelled
        // whole and holding not one number out of the .cube.
        Check(looked.find("\"look_lut\":{\"digest\":\"" + std::string(64, 'b') +
                          "\",\"name\":\"look-v2.cube\",\"space\":\"\"}") !=
                  std::string::npos,
              "the look is not written as the three strings it is");

        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<SetSequenceLutOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "SetSequenceLut round-trips through the journal exactly");

        Check(Apply(log, document, SetSequenceLutOperation{}, "sans look"),
              "the look is cleared by the same operation");
        Check(document.sequence.look_lut.digest.empty(),
              "and the sequence no longer names one");
        Check(document.SaveToString() == before,
              "clearing the look gives back a byte-identical version-7 "
              "document");

        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == looked,
              "undoing the clear restores the exact look");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "undoing the look restores a byte-identical version-7 "
              "document");

        // Guard rail 4: under colour management the space is the author's
        // to declare, and an undeclared look is refused rather than laid
        // somewhere plausible.
        document.color_management.enabled = true;
        document.color_management.input_gamut = "sony_sgamut3_cine";
        document.color_management.input_transfer = "sony_slog3";
        ExpectRejected(document, SetSequenceLutOperation{look},
                       EditError::ValidationFailed,
                       "an undeclared look under colour management");
        SequenceLut declared = look;
        declared.space = "acescct";
        Check(Apply(log, document, SetSequenceLutOperation{declared},
                    "look déclaré"),
              "a declared look applies under colour management");
    });

    Test("insert/remove ripple positions", [] {
        Document document = EditDocument();
        EditLog log;
        InsertClipOperation insert{document.sequence.tracks[0].id,
                                   document.sources[1].id,
                                   {50, 25},
                                   {5, 25},
                                   {10, 25},
                                   {},
                                   {}};
        Check(Apply(log, document, insert, "insert"), "insert applies");
        const Ulid insertedId =
            std::get<InsertClipOperation>(log.AppliedEntries().back().op)
                .clip_id;
        Check(document.sequence.tracks[0].clips.size() == 4,
              "insert adds one clip");
        Check(document.FindClip(insertedId) != nullptr,
              "inserted clip is addressed by generated ULID");
        Check(!document.FindClip(insertedId)->include_audio,
              "new video inserts never embed source audio");
        Check(document.FindClip("01K20000000000000000000005")->timeline_in ==
                  RationalTime{25, 25},
              "second original clip ripples right by insertion duration");
        Check(document.FindClip("01K20000000000000000000006")->timeline_in ==
                  RationalTime{45, 25},
              "all following clips ripple right");

        Check(
            Apply(log, document, RemoveClipOperation{insertedId, {}}, "remove"),
            "remove applies");
        Check(document.FindClip(insertedId) == nullptr, "remove deletes clip");
        Check(document.FindClip("01K20000000000000000000005")->timeline_in ==
                  RationalTime{20, 25},
              "remove ripples following clip left");
        Check(document.FindClip("01K20000000000000000000006")->timeline_in ==
                  RationalTime{40, 25},
              "remove restores every following position");
    });

    // QC-2026-09 C1 -- an insert used to ripple only track_id; an
    // A/V-linked partner living on another track was silently left behind
    // and drifted out of sync_reference_delta. Mirrors A5's RemoveClip
    // coverage above.
    Test("insert_clip ripples a synchronized track and undoes exactly", [] {
        const Ulid syncTrackId = "01K20000000000000000000022";
        Document document = EditDocument();
        DocumentTrack synced{syncTrackId,
                             "audio",
                             1,
                             {{"01K20000000000000000000023",
                               document.sources[0].id,
                               {400, 25},
                               {10, 25},
                               {20, 25}}}};
        document.sequence.tracks.push_back(synced);
        const std::string before = document.SaveToString();
        EditLog log;

        InsertClipOperation unsynced{document.sequence.tracks[0].id,
                                     document.sources[1].id,
                                     {50, 25},
                                     {5, 25},
                                     {10, 25},
                                     {},
                                     {}};
        Check(Apply(log, document, unsynced, "unsynchronized insert"),
              "insert_clip applies without sync_track_ids");
        Check(document.FindClip("01K20000000000000000000023")->timeline_in ==
                  RationalTime{20, 25},
              "without sync_track_ids the other track is untouched");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "unsynchronized insert undo restores exact bytes");

        InsertClipOperation synchronizedInsert{document.sequence.tracks[0].id,
                                               document.sources[1].id,
                                               {50, 25},
                                               {5, 25},
                                               {10, 25},
                                               {},
                                               {},
                                               {syncTrackId}};
        Check(Apply(log, document, synchronizedInsert, "synchronized insert"),
              "insert_clip accepts a synchronized track");
        Check(
            document.FindClip("01K20000000000000000000005")->timeline_in ==
                    RationalTime{25, 25} &&
                document.FindClip("01K20000000000000000000023")->timeline_in ==
                    RationalTime{25, 25},
            "insert_clip ripples every named track by the same exact "
            "duration");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::get<InsertClipOperation>(parsed).sync_track_ids ==
                      std::vector<Ulid>{syncTrackId} &&
                  SerializeOperation(parsed) == json,
              "InsertClip sync_track_ids round-trip canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "InsertClip synchronized undo restores exact bytes");

        document.sequence.tracks[1].locked = true;
        ExpectRejected(document, synchronizedInsert, EditError::LockedTrack,
                       "insert_clip refuses a locked sync track");
    });

    Test("arbitrary multi-clip move is atomic, serializable and reversible",
         [] {
             Document document = EditDocument();
             const std::string before = document.SaveToString();
             const Ulid trackId = document.sequence.tracks[0].id;
             const Ulid firstId = document.sequence.tracks[0].clips[0].id;
             const Ulid secondId = document.sequence.tracks[0].clips[1].id;
             EditLog log;
             MoveClipsOperation move{
                 {{firstId, trackId, {50, 25}}, {secondId, trackId, {70, 25}}},
                 {}};
             Check(Apply(log, document, move, "multi-clip move"),
                   "multi-clip move applies as one entry");
             Check(document.FindClip(firstId)->timeline_in ==
                           RationalTime{50, 25} &&
                       document.FindClip(secondId)->timeline_in ==
                           RationalTime{70, 25} &&
                       log.AppliedCount() == 1,
                   "every selected clip keeps its ID and relative spacing");
             const std::string json =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveClipOperation{};
             EditError error = EditError::None;
             std::string message;
             Check(DeserializeOperation(json, parsed, error, message) &&
                       std::holds_alternative<MoveClipsOperation>(parsed) &&
                       SerializeOperation(parsed) == json,
                   "MoveClips canonical JSON round-trips");
             Check(log.Undo(document, error, message) &&
                       document.SaveToString() == before,
                   "multi-clip move undo restores byte-identical state");
         });

    Test("clear and ripple delete are distinct operations", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        const Ulid clearedId = "01K20000000000000000000005";
        const Ulid followingId = "01K20000000000000000000006";
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(Apply(log, document, ClearClipOperation{clearedId, {}}, "clear"),
              "clear applies");
        Check(!document.FindClip(clearedId), "clear removes the selected clip");
        Check(
            document.FindClip(followingId)->timeline_in == RationalTime{40, 25},
            "clear leaves the following clip in place and creates a gap");
        const std::string cleared = document.SaveToString();
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<ClearClipOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "ClearClip JSON round-trips as its own operation");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "clear undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == cleared,
              "clear redo restores the gap without ripple");

        Document rippleDocument = EditDocument();
        EditLog rippleLog;
        Check(Apply(rippleLog, rippleDocument,
                    RemoveClipOperation{clearedId, {}}, "ripple delete"),
              "ripple delete applies");
        Check(rippleDocument.FindClip(followingId)->timeline_in ==
                  RationalTime{30, 25},
              "ripple delete closes the removed clip interval");
    });

    Test("clearing an arbitrary multi-selection is one exact operation", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        const Ulid firstId = "01K20000000000000000000004";
        const Ulid lastId = "01K20000000000000000000006";
        const Ulid untouchedId = "01K20000000000000000000005";
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(Apply(log, document, ClearClipsOperation{{firstId, lastId}, {}},
                    "multi-clear"),
              "multi-clear applies");
        Check(!document.FindClip(firstId) && !document.FindClip(lastId) &&
                  document.FindClip(untouchedId),
              "multi-clear removes every selected clip and no other clip");
        const std::string cleared = document.SaveToString();
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<ClearClipsOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "ClearClips JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "multi-clear undo restores exact document bytes");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == cleared,
              "multi-clear redo restores every gap exactly");

        Document locked = EditDocument();
        locked.sequence.tracks.front().locked = true;
        ExpectRejected(locked, ClearClipsOperation{{firstId, lastId}, {}},
                       EditError::LockedTrack,
                       "multi-clear touching a locked track");
    });

    Test("paste duplicates linked clips as one exact operation", [] {
        Document document = EditDocument();
        DocumentClip& video = document.sequence.tracks[0].clips[0];
        video.include_audio = false;
        video.link_group_id = "01K20000000000000000000020";
        video.sync_anchor_clip_id = video.id;
        video.sync_reference_delta = {0, 1};
        const DocumentClip copiedVideo = video;
        const Ulid videoTrackId = document.sequence.tracks[0].id;
        DocumentTrack audio;
        audio.id = "01K20000000000000000000021";
        audio.kind = "audio";
        audio.index = 1;
        DocumentClip audioClip{"01K20000000000000000000022", video.source_id,
                               video.source_in, video.duration,
                               video.timeline_in};
        audioClip.include_audio = false;
        audioClip.link_group_id = video.link_group_id;
        audioClip.sync_anchor_clip_id = video.id;
        audioClip.sync_reference_delta = {0, 1};
        audio.clips.push_back(audioClip);
        document.sequence.tracks.push_back(audio);
        const std::string before = document.SaveToString();

        PasteClipsOperation paste{{
            {copiedVideo.id,
             videoTrackId,
             copiedVideo.source_id,
             copiedVideo.source_in,
             copiedVideo.duration,
             {60, 25},
             {},
             copiedVideo.link_group_id,
             {},
             copiedVideo.sync_anchor_clip_id,
             {},
             copiedVideo.sync_reference_delta},
            {audioClip.id,
             audio.id,
             audioClip.source_id,
             audioClip.source_in,
             audioClip.duration,
             {60, 25},
             {},
             audioClip.link_group_id,
             {},
             audioClip.sync_anchor_clip_id,
             {},
             audioClip.sync_reference_delta},
        }};
        EditLog log;
        Check(Apply(log, document, paste, "paste linked clips"),
              "paste applies");
        const auto& stored =
            std::get<PasteClipsOperation>(log.AppliedEntries().back().op);
        Check(stored.clips.size() == 2 && !stored.clips[0].clip_id.empty() &&
                  !stored.clips[1].clip_id.empty() &&
                  stored.clips[0].link_group_id ==
                      stored.clips[1].link_group_id &&
                  stored.clips[0].link_group_id != copiedVideo.link_group_id,
              "paste generates stable clip IDs and a fresh link group");
        const DocumentClip* pastedVideo =
            document.FindClip(stored.clips[0].clip_id);
        const DocumentClip* pastedAudio =
            document.FindClip(stored.clips[1].clip_id);
        Check(pastedVideo && pastedAudio && !pastedVideo->include_audio &&
                  !pastedAudio->include_audio &&
                  pastedVideo->sync_anchor_clip_id == pastedVideo->id &&
                  pastedAudio->sync_anchor_clip_id == pastedVideo->id,
              "pasted A/V remains separate and linked to the new video clip");
        const std::string after = document.SaveToString();
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "PasteClips JSON round-trips exactly");
        std::string legacyPaste = serialized;
        const std::string appendedField = ",\"overwrite\":false";
        const size_t appendedAt = legacyPaste.rfind(appendedField);
        Check(appendedAt != std::string::npos,
              "canonical paste contains its overwrite field");
        if (appendedAt != std::string::npos)
            legacyPaste.erase(appendedAt, appendedField.size());
        const std::string opacityField = ",\"opacity\":{\"num\":1,\"den\":1}";
        size_t opacityAt = 0;
        while ((opacityAt = legacyPaste.find(opacityField, opacityAt)) !=
               std::string::npos) {
            legacyPaste.erase(opacityAt, opacityField.size());
        }
        Operation parsedLegacy = RemoveClipOperation{};
        Check(DeserializeOperation(legacyPaste, parsedLegacy, error, message) &&
                  std::holds_alternative<PasteClipsOperation>(parsedLegacy) &&
                  !std::get<PasteClipsOperation>(parsedLegacy).overwrite &&
                  SerializeOperation(parsedLegacy).find(opacityField) !=
                      std::string::npos,
              "legacy PasteClips logs without overwrite or opacity remain "
              "readable");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "paste undo restores byte-identical document JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == after,
              "paste redo retains generated clip and link IDs");

        PasteClipsOperation overlap = paste;
        overlap.exact_track_result.clear();
        for (PastedClip& item : overlap.clips) {
            item.timeline_in = {0, 25};
            item.clip_id.clear();
            item.link_group_id.clear();
            item.sync_anchor_clip_id.clear();
        }
        ExpectRejected(document, overlap, EditError::Overlap,
                       "paste cannot overwrite existing clips");

        PasteClipsOperation overwrite = paste;
        overwrite.overwrite = true;
        for (PastedClip& item : overwrite.clips) item.timeline_in = {5, 25};
        const std::string beforeOverwrite = document.SaveToString();
        EditLog overwriteLog;
        Check(Apply(overwriteLog, document, overwrite, "overwrite paste"),
              "overwrite paste applies at the playhead");
        Check(document.sequence.tracks[0].clips.front().duration ==
                      RationalTime{5, 25} &&
                  document.sequence.tracks[1].clips.front().duration ==
                      RationalTime{5, 25},
              "overwrite paste trims covered clips on video and audio tracks");
        EditError overwriteError = EditError::None;
        std::string overwriteMessage;
        Check(overwriteLog.Undo(document, overwriteError, overwriteMessage) &&
                  document.SaveToString() == beforeOverwrite,
              "overwrite paste undo restores every covered track exactly");

        PasteClipsOperation locked = paste;
        for (PastedClip& item : locked.clips) item.timeline_in = {80, 25};
        document.sequence.tracks[1].locked = true;
        ExpectRejected(document, locked, EditError::LockedTrack,
                       "paste cannot modify a locked destination track");
    });

    Test("mixed-rate ripple undo restores exact representation", [] {
        Document document;
        document.sources = {
            {"01K40000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K40000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.sequence.tracks = {
            {"01K40000000000000000000003",
             "video",
             0,
             {
                 {"01K40000000000000000000004",
                  "01K40000000000000000000001",
                  {0, 25},
                  {25, 25},
                  {0, 25}},
                 {"01K40000000000000000000005",
                  "01K40000000000000000000002",
                  {100100, 30000},
                  {1001, 30000},
                  {30000, 30000}},
             }},
        };
        const std::string original = document.SaveToString();
        EditLog log;
        Check(Apply(log, document,
                    InsertClipOperation{document.sequence.tracks[0].id,
                                        document.sources[1].id,
                                        {0, 30000},
                                        {1001, 30000},
                                        {25, 25},
                                        "01K40000000000000000000006",
                                        {}},
                    "mixed-rate insert"),
              "mixed-rate insertion applies");
        Check(
            document.FindClip("01K40000000000000000000005")->timeline_in.rate ==
                30000,
            "ripple uses an exact common timebase");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message),
              "mixed-rate insertion undo succeeds: " + message);
        Check(document.SaveToString() == original,
              "mixed-rate undo restores original RationalTime representation");
    });

    Test("head and tail trim stay local", [] {
        {
            Document isolated = EditDocument();
            isolated.sequence.tracks[0].clips.erase(
                isolated.sequence.tracks[0].clips.begin() + 1,
                isolated.sequence.tracks[0].clips.end());
            EditLog log;
            Check(
                Apply(log, isolated,
                      TrimClipOperation{isolated.sequence.tracks[0].clips[0].id,
                                        TrimEdge::Head,
                                        {2, 25},
                                        std::nullopt},
                      "isolated head trim"),
                "isolated head trim applies");
            const DocumentClip& clip = isolated.sequence.tracks[0].clips[0];
            Check(clip.source_in == RationalTime{102, 25} &&
                      clip.duration == RationalTime{8, 25} &&
                      clip.timeline_in == RationalTime{2, 25},
                  "head trim changes exactly source_in, duration and "
                  "timeline_in");
            Check(Apply(log, isolated,
                        TrimClipOperation{
                            clip.id, TrimEdge::Tail, {-2, 25}, std::nullopt},
                        "isolated tail trim"),
                  "isolated tail trim applies");
            Check(isolated.sequence.tracks[0].clips[0].duration ==
                      RationalTime{6, 25},
                  "tail trim changes duration");
        }
        {
            Document framed = EditDocument();
            const RationalTime beforePrevious =
                framed.sequence.tracks[0].clips[0].timeline_in;
            const RationalTime beforeNext =
                framed.sequence.tracks[0].clips[2].timeline_in;
            EditLog log;
            const Ulid middle = framed.sequence.tracks[0].clips[1].id;
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Head, {2, 25}, std::nullopt},
                        "framed head trim"),
                  "framed head trim applies");
            Check(framed.sequence.tracks[0].clips[0].timeline_in ==
                          beforePrevious &&
                      framed.sequence.tracks[0].clips[2].timeline_in ==
                          beforeNext,
                  "head trim does not move surrounding clips");
            Check(Apply(log, framed,
                        TrimClipOperation{
                            middle, TrimEdge::Tail, {2, 25}, std::nullopt},
                        "framed tail trim"),
                  "framed tail trim applies inside the gap");
            Check(framed.sequence.tracks[0].clips[2].timeline_in == beforeNext,
                  "tail trim does not move following clip");
        }
    });

    Test("invalid preconditions are atomic and named", [] {
        const Document base = EditDocument();
        const InsertClipOperation validInsert{base.sequence.tracks[0].id,
                                              base.sources[0].id,
                                              {0, 25},
                                              {2, 25},
                                              {10, 25},
                                              {},
                                              {}};
        InsertClipOperation unknownTrack = validInsert;
        unknownTrack.track_id = "01K29999999999999999999991";
        ExpectRejected(base, unknownTrack, EditError::UnknownTrack,
                       "unknown track_id");
        InsertClipOperation unknownSource = validInsert;
        unknownSource.source_id = "01K29999999999999999999992";
        ExpectRejected(base, unknownSource, EditError::UnknownSource,
                       "unknown source_id");
        InsertClipOperation zeroDuration = validInsert;
        zeroDuration.duration = {0, 25};
        ExpectRejected(base, zeroDuration, EditError::InvalidDuration,
                       "zero insert duration");
        InsertClipOperation negativeDuration = validInsert;
        negativeDuration.duration = {-1, 25};
        ExpectRejected(base, negativeDuration, EditError::InvalidDuration,
                       "negative insert duration");
        InsertClipOperation negativeTimeline = validInsert;
        negativeTimeline.timeline_in = {-1, 25};
        ExpectRejected(base, negativeTimeline, EditError::InvalidTimelineIn,
                       "negative insertion timeline_in");
        InsertClipOperation overlap = validInsert;
        overlap.timeline_in = {5, 25};
        ExpectRejected(base, overlap, EditError::Overlap, "insertion overlap");
        InsertClipOperation outside = validInsert;
        outside.source_in = {999, 25};
        ExpectRejected(base, outside, EditError::SourceOutOfBounds,
                       "insert source bounds");
        InsertClipOperation unknownSyncTrack = validInsert;
        unknownSyncTrack.sync_track_ids = {"01K29999999999999999999996"};
        ExpectRejected(base, unknownSyncTrack, EditError::UnknownTrack,
                       "unknown insert sync track_id");
        ExpectRejected(base,
                       RemoveClipOperation{"01K29999999999999999999993", {}},
                       EditError::UnknownClip, "unknown remove clip_id");
        ExpectRejected(base,
                       TrimClipOperation{"01K29999999999999999999994",
                                         TrimEdge::Tail,
                                         {1, 25},
                                         std::nullopt},
                       EditError::UnknownClip, "unknown trim clip_id");
        ExpectRejected(base,
                       TrimClipOperation{base.sequence.tracks[0].clips[0].id,
                                         TrimEdge::Tail,
                                         {-10, 25},
                                         std::nullopt},
                       EditError::InvalidDuration, "zero trim duration");
        ExpectRejected(base,
                       TrimClipOperation{base.sequence.tracks[0].clips[0].id,
                                         TrimEdge::Head,
                                         {-101, 25},
                                         std::nullopt},
                       EditError::InvalidTimelineIn,
                       "head trim before timeline zero");

        Document sourceBound = base;
        sourceBound.sequence.tracks[0].clips[0].timeline_in = {200, 25};
        sourceBound.sequence.tracks[0].clips[1].timeline_in = {220, 25};
        sourceBound.sequence.tracks[0].clips[2].timeline_in = {240, 25};
        ExpectRejected(
            sourceBound,
            TrimClipOperation{sourceBound.sequence.tracks[0].clips[0].id,
                              TrimEdge::Head,
                              {-101, 25},
                              std::nullopt},
            EditError::InvalidOperation, "head trim before source start");

        Document tailOverlap = base;
        tailOverlap.sequence.tracks[0].clips[1].timeline_in = {11, 25};
        tailOverlap.sequence.tracks[0].clips[2].timeline_in = {40, 25};
        ExpectRejected(
            tailOverlap,
            TrimClipOperation{tailOverlap.sequence.tracks[0].clips[0].id,
                              TrimEdge::Tail,
                              {2, 25},
                              std::nullopt},
            EditError::Overlap, "tail trim overlap");
    });

    Test("operation and edit-log serialization round trips", [] {
        Document document = EditDocument();
        const std::string originalDocument = document.SaveToString();
        EditLog log;
        Check(Apply(log, document,
                    InsertClipOperation{document.sequence.tracks[0].id,
                                        document.sources[1].id,
                                        {10, 25},
                                        {3, 25},
                                        {10, 25},
                                        {},
                                        {}},
                    "serialized insert"),
              "insert applies");
        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "serialized trim"),
              "trim applies");
        const Operation& original = log.AppliedEntries()[0].op;
        const std::string json = SerializeOperation(original);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(json, parsed, error, message),
              "operation parses: " + message);
        Check(SerializeOperation(parsed) == json,
              "operation JSON is canonical after round trip");

        const std::string logJson = log.Serialize();
        EditLog parsedLog;
        Check(EditLog::Deserialize(logJson, parsedLog, error, message),
              "edit log parses: " + message);
        Check(parsedLog.Serialize() == logJson,
              "edit log JSON is canonical after round trip");

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-cutmachine-edit-log.json");
        Check(log.Save(path.string(), error, message),
              "edit log saves: " + message);
        EditLog loaded;
        Check(EditLog::Load(path.string(), loaded, error, message),
              "edit log loads: " + message);
        Check(loaded.Serialize() == logJson, "persisted edit log is identical");
        Check(loaded.Undo(document, error, message),
              "loaded log can undo trim: " + message);
        Check(loaded.Undo(document, error, message),
              "loaded log can undo insert: " + message);
        Check(document.SaveToString() == originalDocument,
              "persisted inverses restore the original document bytes");
        std::filesystem::remove(path);
    });

    Test("sequence settings are addressable and reversible", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        const Ulid sequenceId = document.sequence.id;
        const Ulid firstTrackId = document.sequence.tracks.front().id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(
            Apply(log, document,
                  UpdateSequenceOperation{
                      sequenceId, "Vertical master", 1080, 1920, {24000, 1001}},
                  "update sequence"),
            "sequence update succeeds");
        Check(document.sequence.id == sequenceId &&
                  document.sequence.name == "Vertical master" &&
                  document.sequence.width == 1080 &&
                  document.sequence.height == 1920 &&
                  document.sequence.frame_rate.num == 24000 &&
                  document.sequence.frame_rate.den == 1001,
              "sequence settings change without replacing its identity");
        Check(document.sequence.tracks.front().id == firstTrackId,
              "sequence update preserves the owned timeline");

        const Operation& stored = log.AppliedEntries().back().op;
        const std::string json = SerializeOperation(stored);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "UpdateSequence JSON round-trips canonically: " + message);
        const std::string updated = document.SaveToString();
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "sequence settings undo restores original bytes");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == updated,
              "sequence settings redo restores updated bytes");

        ExpectRejected(
            document,
            UpdateSequenceOperation{
                "01K83000000000000000000009", "Other", 1920, 1080, {25, 1}},
            EditError::UnknownSequence, "unknown sequence");
        ExpectRejected(
            document,
            UpdateSequenceOperation{sequenceId, "Invalid", 0, 1080, {25, 1}},
            EditError::ValidationFailed, "invalid sequence dimensions");
    });

    Test("removing a populated track restores its clips on undo", [] {
        Document document = EditDocument();
        EditLog log;
        const Ulid trackId = document.sequence.tracks.front().id;
        const std::string original = document.SaveToString();
        Check(!document.sequence.tracks.front().clips.empty(),
              "fixture track contains clips");
        Check(Apply(log, document, RemoveTrackOperation{trackId},
                    "remove populated track"),
              "populated track removal succeeds");
        Check(!document.FindTrack(trackId), "track and its clips are removed");
        const std::string inverseJson =
            SerializeOperation(log.AppliedEntries().back().inverse);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(inverseJson, parsed, error, message) &&
                  SerializeOperation(parsed) == inverseJson,
              "track contents round-trip in the inverse operation");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "undo restores the populated track byte-exactly");
    });

    Test("bins create and classify media through reversible operations", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        const std::string initial = document.SaveToString();
        EditLog log;
        const Ulid binId = "01K80000000000000000000001";
        Check(Apply(log, document, AddBinOperation{binId, "Rushes \"A\""},
                    "add bin"),
              "bin creation succeeds");
        Check(Apply(log, document,
                    SetMediaBinOperation{document.library[0].id, binId},
                    "assign media bin"),
              "media assignment succeeds");
        Check(document.bins.size() == 1 && document.library[0].bin_id == binId,
              "bin and media assignment are stored in the document");
        const std::string serialized = log.Serialize();
        EditLog parsed;
        EditError error = EditError::None;
        std::string message;
        Check(EditLog::Deserialize(serialized, parsed, error, message) &&
                  parsed.Serialize() == serialized,
              "bin operations round-trip in the event log: " + message);
        Check(log.Undo(document, error, message) &&
                  document.library[0].bin_id.empty(),
              "undo restores media to the project root");
        Check(log.Undo(document, error, message) && document.bins.empty() &&
                  document.SaveToString() == initial,
              "second undo removes the empty bin byte-exactly");
        Check(log.Redo(document, error, message) &&
                  log.Redo(document, error, message) &&
                  document.library[0].bin_id == binId,
              "redo recreates the same bin ULID and assignment");
    });

    // G7 -- docs/roadmap/GRADE-2026-09.md. A media's as-shot white balance:
    // set, cleared and undone byte for byte, carried by the journal, and the
    // version that makes an older binary refuse the picture it would get
    // wrong.
    Test("an as-shot white balance is exact, versioned and undoable", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        const std::string initial = document.SaveToString();
        Check(initial.find("white_balance_kelvin") == std::string::npos &&
                  initial.find("\"version\": 7") != std::string::npos,
              "an unknown balance writes no key and keeps version 7");

        EditLog log;
        Check(
            Apply(log, document, SetMediaWhiteBalanceOperation{media.id, 4500},
                  "balance de tournage"),
            "a white balance applies");
        const std::string balanced = document.SaveToString();
        Check(document.library[0].white_balance_kelvin == 4500 &&
                  balanced.find("\"white_balance_kelvin\":4500") !=
                      std::string::npos &&
                  balanced.find("\"version\": 15") != std::string::npos,
              "it is written, and the document declares version 15");

        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(
            DeserializeOperation(json, parsed, error, message) &&
                std::holds_alternative<SetMediaWhiteBalanceOperation>(parsed) &&
                SerializeOperation(parsed) == json,
            "SetMediaWhiteBalance round-trips through the journal exactly");

        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "undo gives back the version-7 document byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == balanced,
              "redo gives back the balanced one");
        Check(Apply(log, document, SetMediaWhiteBalanceOperation{media.id, 0},
                    "balance inconnue") &&
                  document.SaveToString() == initial,
              "0 clears it, byte for byte");

        ExpectRejected(document, SetMediaWhiteBalanceOperation{media.id, 500},
                       EditError::ValidationFailed,
                       "500 K is no white balance");
        ExpectRejected(
            document,
            SetMediaWhiteBalanceOperation{"01K20000000000000000000099", 4500},
            EditError::UnknownMedia, "an unknown media is refused");

        // A color.temperature alone declares the version too: an older
        // binary would read it as the offset it was.
        Document graded = EditDocument();
        graded.sequence.tracks[0].clips[0].effects = {
            {"01K80000000000000000000009",
             "color.temperature",
             {{"kelvin", {3200, 1}}}}};
        Check(
            graded.SaveToString().find("\"version\": 15") != std::string::npos,
            "a document holding a color.temperature declares version 15");
        Document reread;
        Check(Document::LoadFromString(balanced, reread, message) &&
                  reread.library[0].white_balance_kelvin == 4500,
              "a version-15 document reads back its balance: " + message);
    });

    // GR6 -- docs/roadmap/GRADE-RESOLVE-2026-10.md.
    Test("a contrast pivot and a hue stage declare version 17", [] {
        const auto saved = [](std::vector<ClipEffect> effects) {
            Document document = EditDocument();
            document.sequence.tracks[0].clips[0].effects = std::move(effects);
            return document.SaveToString();
        };
        const Ulid id = "01K80000000000000000000009";
        Check(saved({{id, "color.contrast", {{"amount", {1, 5}}}}})
                      .find("\"version\": 17") == std::string::npos,
              "a contrast without a pivot keeps the version it had");
        const std::string pivoted = saved(
            {{id, "color.contrast", {{"amount", {1, 5}}, {"pivot", {3, 10}}}}});
        Check(pivoted.find("\"version\": 17") != std::string::npos,
              "a contrast with a pivot declares version 17: an older binary "
              "would pivot it about middle grey");
        Check(saved({{id, "color.hue", {{"degrees", {30, 1}}}}})
                      .find("\"version\": 17") != std::string::npos,
              "a hue rotation declares it too: an older binary would skip "
              "the stage");
        // GR8.
        Check(saved({{id,
                      "color.hue_curves",
                      {{"hue_sat_x0", {1, 5}},
                       {"hue_sat_y0", {1, 2}},
                       {"hue_sat_x1", {2, 5}},
                       {"hue_sat_y1", {1, 4}}}}})
                      .find("\"version\": 17") != std::string::npos,
              "hue curves declare it as well");
        Check(saved({{id, "color.wheels", {{"offset_r", {1, 20}}}}})
                      .find("\"version\": 17") == std::string::npos,
              "an offset wheel does not: an older binary refuses the knob");
        Document reread;
        std::string message;
        Check(Document::LoadFromString(pivoted, reread, message) &&
                  reread.sequence.tracks[0].clips[0].effects.size() == 1 &&
                  reread.sequence.tracks[0]
                          .clips[0]
                          .effects[0]
                          .params.at("pivot")
                          .num == 3 &&
                  reread.SaveToString() == pivoted,
              "a version-17 document reads back its pivot, byte for byte: " +
                  message);
        Document refused = EditDocument();
        refused.sequence.tracks[0].clips[0].effects = {
            {id, "color.contrast", {{"pivot", {3, 2}}}}};
        Check(!refused.Validate(message) &&
                  message.find("'pivot' (3/2) is outside [0, 1]") !=
                      std::string::npos,
              "a pivot outside the picture's values is refused: " + message);
    });

    // G3 -- docs/roadmap/GRADE-2026-09.md.
    Test("a media's own colour input is exact, versioned and undoable", [] {
        Document document = EditDocument();
        document.color_management.enabled = true;
        document.color_management.input_gamut = "sony_sgamut3_cine";
        document.color_management.input_transfer = "sony_slog3";
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        LibraryMedia sound = media;
        sound.id = "01K20000000000000000000088";
        sound.path = "A.WAV";
        sound.filename = "A.WAV";
        sound.codec = "pcm_s16le";
        sound.has_video = false;
        sound.has_audio = true;
        sound.audio_rate = 48000;
        sound.audio_channels = 2;
        sound.width = 0;
        sound.height = 0;
        sound.orientation = "audio";
        document.library.push_back(sound);
        const std::string initial = document.SaveToString();
        Check(initial.find("color_input") == std::string::npos &&
                  initial.find("\"version\": 7") != std::string::npos,
              "a media following the project writes no key");
        Check(document.ColorManagementFor(media.id).input_transfer ==
                  "sony_slog3",
              "and is read with the project's input");

        const MediaColorInput phone{"rec709", "rec709", "auto", "auto"};
        EditLog log;
        Check(Apply(log, document, SetMediaColorInputOperation{media.id, phone},
                    "entrée couleur"),
              "a declared input applies");
        const std::string declared = document.SaveToString();
        const ColorManagementSettings read =
            document.ColorManagementFor(media.id);
        Check(
            document.library[0].color_input == phone &&
                declared.find("\"color_input\":{\"gamut\":\"rec709\","
                              "\"transfer\":\"rec709\"") != std::string::npos &&
                declared.find("\"version\": 16") != std::string::npos,
            "it is written, and the document declares version 16");
        Check(
            read.enabled && read.input_gamut == "rec709" &&
                read.input_transfer == "rec709" &&
                read.working_gamut == document.color_management.working_gamut &&
                read.output_transfer ==
                    document.color_management.output_transfer,
            "the media's input replaces the project's, and only its "
            "input");

        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(json, parsed, error, message) &&
                  std::holds_alternative<SetMediaColorInputOperation>(parsed) &&
                  SerializeOperation(parsed) == json,
              "SetMediaColorInput round-trips through the journal exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "undo gives back the version-7 document byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == declared,
              "redo gives back the declared one");
        // Back to the project, whatever the other fields say: the default
        // spelling, so the file keeps no key.
        Check(Apply(log, document,
                    SetMediaColorInputOperation{
                        media.id, {"rec709", "", "bt709", "full"}},
                    "entrée du projet") &&
                  document.SaveToString() == initial,
              "an input without a transfer follows the project, byte for "
              "byte");

        ExpectRejected(document,
                       SetMediaColorInputOperation{
                           media.id, {"rec709", "hlg", "auto", "auto"}},
                       EditError::ValidationFailed,
                       "an HLG picture outside BT.2020 is not an input the "
                       "config reads");
        ExpectRejected(document,
                       SetMediaColorInputOperation{
                           media.id, {"rec709", "rec709", "bt601", "auto"}},
                       EditError::ValidationFailed,
                       "an unknown matrix is refused");
        ExpectRejected(document, SetMediaColorInputOperation{sound.id, phone},
                       EditError::ValidationFailed,
                       "an audio-only media has no picture to declare");
        ExpectRejected(
            document,
            SetMediaColorInputOperation{"01K20000000000000000000099", phone},
            EditError::UnknownMedia, "an unknown media is refused");

        // A file spelling half a declaration is damaged, not a choice.
        Document spelled = document;
        spelled.library[0].color_input = {"rec709", "", "auto", "auto"};
        Check(!spelled.Validate(message),
              "a gamut without a transfer is refused on read");
        Document reread;
        Check(Document::LoadFromString(declared, reread, message) &&
                  reread.library[0].color_input == phone,
              "a version-16 document reads back its input: " + message);

        // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A media declared
        // a graphic: version 21, read as its code values with its own
        // matrix and range, and undone to the byte.
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == declared &&
                  log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "DEC31: back to the version-7 document");
        const MediaColorInput logo{"", "", "bt709", "full", true};
        Check(Apply(log, document, SetMediaColorInputOperation{media.id, logo},
                    "graphisme"),
              "DEC31: a graphic applies");
        const std::string graphic = document.SaveToString();
        const ColorManagementSettings shown =
            document.ColorManagementFor(media.id);
        Check(document.library[0].color_input == logo &&
                  document.IsGraphicsMedia(media.id) &&
                  graphic.find("\"graphics\":true") != std::string::npos &&
                  graphic.find("\"version\": 21") != std::string::npos,
              "DEC31: it is written, and the document declares version 21");
        Check(!shown.enabled && shown.input_ycbcr_matrix == "bt709" &&
                  shown.input_range == "full" &&
                  shown.input_transfer == "rec709",
              "DEC31: a graphic is read as its code values, with its own "
              "matrix and range");
        const std::string graphicJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(graphicJson, parsed, error, message) &&
                  SerializeOperation(parsed) == graphicJson &&
                  graphicJson.find("\"graphics\":true") != std::string::npos,
              "DEC31: the graphic round-trips through the journal exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "DEC31: undo gives back the version-7 document byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == graphic,
              "DEC31: redo gives back the graphic");
        Document regraphic;
        Check(
            Document::LoadFromString(graphic, regraphic, message) &&
                regraphic.library[0].color_input == logo,
            "DEC31: a version-21 document reads back its graphic: " + message);
        ExpectRejected(
            document,
            SetMediaColorInputOperation{
                media.id, {"rec709", "rec709", "auto", "auto", true}},
            EditError::ValidationFailed,
            "DEC31: a graphic does not name a camera curve too");
        ExpectRejected(document,
                       SetMediaColorInputOperation{
                           sound.id, {"", "", "auto", "auto", true}},
                       EditError::ValidationFailed,
                       "DEC31: an audio-only media cannot be a graphic");
        Check(Apply(log, document,
                    SetMediaColorInputOperation{media.id, MediaColorInput{}},
                    "entrée du projet") &&
                  document.SaveToString() == initial,
              "DEC31: following the project again writes no key");
    });

    // GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. One clip's
    // grade pasted onto others: SetClipEffects steps in one batch, undone
    // to the byte; a clip without a grade is skipped and said.
    Test("a grade pasted onto clips is one step, undone to the byte", [] {
        Document document = EditDocument();
        ClipEffect exposure;
        exposure.id = "01K20000000000000000000E01";
        exposure.type = "color.exposure";
        exposure.params["amount"] = {1, 2};
        ClipEffect saturation;
        saturation.id = "01K20000000000000000000E02";
        saturation.type = "color.saturation";
        saturation.params["amount"] = {-1, 5};
        document.sequence.tracks[0].clips[0].effects = {exposure, saturation};
        DocumentTrack sound{"01K20000000000000000000A03", "audio", 1, {}};
        sound.clips.push_back({"01K20000000000000000000A04",
                               "01K20000000000000000000001",
                               {100, 25},
                               {10, 25},
                               {0, 25}});
        document.sequence.tracks.push_back(sound);
        const std::string initial = document.SaveToString();
        const Ulid source = "01K20000000000000000000004";
        ClipGradePaste paste;
        std::string message;
        Check(BuildClipGradePaste(
                  document, source,
                  {"01K20000000000000000000005", "01K20000000000000000000006",
                   source, "01K20000000000000000000A04",
                   "01K20000000000000000000005"},
                  paste, message) &&
                  paste.operation &&
                  std::holds_alternative<BatchOperation>(*paste.operation) &&
                  paste.pasted.size() == 2 && paste.unchanged.size() == 1 &&
                  paste.skipped.size() == 1 &&
                  paste.skipped[0].clip_id == "01K20000000000000000000A04",
              "GR11: two shots take the grade, the source is unchanged, the "
              "sound is skipped, a clip named twice counts once: " +
                  message);
        EditLog log;
        Check(
            paste.operation && Apply(log, document, *paste.operation, "coller"),
            "GR11: the paste applies");
        const auto& clips = document.sequence.tracks[0].clips;
        bool same = clips.size() == 3;
        for (size_t index = 1; same && index < 3; ++index) {
            same = clips[index].effects.size() == 2 &&
                   clips[index].effects[0].type == "color.exposure" &&
                   clips[index].effects[0].params.at("amount").num == 1 &&
                   clips[index].effects[0].params.at("amount").den == 2 &&
                   clips[index].effects[1].type == "color.saturation" &&
                   clips[index].effects[0].id != exposure.id &&
                   clips[index].effects[1].id != saturation.id &&
                   !clips[index].effects[0].id.empty();
        }
        std::string validation;
        Check(same && document.Validate(validation),
              "GR11: each shot holds the stack, with ids of its own: " +
                  validation);
        const std::string pasted = document.SaveToString();
        EditError error = EditError::None;
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == initial,
              "GR11: one undo gives back the document byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == pasted,
              "GR11: and redo the pasted one");
        Check(BuildClipGradePaste(document, source,
                                  {"01K20000000000000000000005"}, paste,
                                  message) &&
                  !paste.operation && paste.unchanged.size() == 1,
              "GR11: a shot already graded so is no step");
        Check(!BuildClipGradePaste(document, "01K20000000000000000000A04",
                                   {"01K20000000000000000000005"}, paste,
                                   message) &&
                  message.find("no grade to copy") != std::string::npos,
              "GR11: a sound has no grade to copy: " + message);
        Check(!BuildClipGradePaste(document, source,
                                   {"01K200000000000000000000ZZ"}, paste,
                                   message) &&
                  message.find("unknown clip_id") != std::string::npos,
              "GR11: an unknown clip is refused by name");
        // DEC31 -- under colour management, a graphic takes no grade.
        Document managed = document;
        managed.color_management.enabled = true;
        LibraryMedia logo;
        logo.id = "01K20000000000000000000002";
        logo.path = "B.MP4";
        logo.filename = "B.MP4";
        logo.width = 1920;
        logo.height = 1080;
        logo.rate = {25, 1};
        logo.duration = {1000, 25};
        logo.color_input.graphics = true;
        managed.library.push_back(logo);
        Check(
            BuildClipGradePaste(managed, source, {"01K20000000000000000000006"},
                                paste, message) &&
                !paste.operation && paste.skipped.size() == 1 &&
                paste.skipped[0].reason.find("graphic") != std::string::npos,
            "GR11: a graphic under colour management is skipped, and why");
        // The window's paste: a stack copied earlier, its source since gone.
        Check(BuildClipGradePasteOfStack(document, {}, {source}, paste,
                                         message) &&
                  paste.operation && paste.pasted.size() == 1,
              "GR11: an empty stack pasted takes the grade away");
    });

    Test("nested bins preserve hierarchy and reject destructive removal", [] {
        Document document = EditDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = "A.MP4";
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        media.orientation = "landscape";
        document.library.push_back(media);
        EditLog log;
        const Ulid parent = "01K81000000000000000000001";
        const Ulid child = "01K81000000000000000000002";
        Check(Apply(log, document, AddBinOperation{parent, "Rushes", ""},
                    "add parent bin"),
              "top-level bin creation succeeds");
        Check(Apply(log, document, AddBinOperation{child, "Interview", parent},
                    "add child bin"),
              "child bin creation succeeds");
        Check(document.FindBin(child) &&
                  document.FindBin(child)->parent_id == parent,
              "child retains its exact parent ULID");

        const std::string childJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(childJson, parsed, error, message) &&
                  SerializeOperation(parsed) == childJson,
              "nested AddBin JSON round-trips canonically");

        const Ulid sibling = "01K81000000000000000000003";
        Check(Apply(log, document, AddBinOperation{sibling, "B-roll", ""},
                    "add sibling"),
              "second top-level bin creation succeeds");
        Check(Apply(log, document, MoveBinOperation{child, sibling},
                    "move child bin"),
              "bin can be moved under another bin");
        Check(document.FindBin(child)->parent_id == sibling,
              "moved bin stores its new parent");
        const std::string moveJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(moveJson, parsed, error, message) &&
                  SerializeOperation(parsed) == moveJson,
              "MoveBin JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindBin(child)->parent_id == parent,
              "move undo restores the previous bin parent");
        Check(log.Redo(document, error, message) &&
                  document.FindBin(child)->parent_id == sibling,
              "move redo restores the new bin parent");
        Check(log.Undo(document, error, message),
              "restore original hierarchy for following checks");
        ExpectRejected(document, MoveBinOperation{parent, child},
                       EditError::InvalidOperation, "cyclic bin move");

        Check(
            Apply(log, document, RenameBinOperation{child, "Interview selects"},
                  "rename child bin"),
            "bin rename succeeds through the event log");
        const std::string renameJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(DeserializeOperation(renameJson, parsed, error, message) &&
                  SerializeOperation(parsed) == renameJson,
              "RenameBin JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.FindBin(child)->name == "Interview",
              "rename undo restores the original name");

        ExpectRejected(document, RemoveBinOperation{parent, "", ""},
                       EditError::InvalidOperation, "remove bin with child");
        Check(Apply(log, document, SetMediaBinOperation{media.id, child},
                    "classify in child"),
              "media can be placed in a nested bin");
        ExpectRejected(document, RemoveBinOperation{child, "", ""},
                       EditError::InvalidOperation,
                       "remove non-empty child bin");

        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded,
                                       message) &&
                  loaded.FindBin(child) &&
                  loaded.FindBin(child)->parent_id == parent,
              "document JSON preserves the bin hierarchy");

        Document cyclic = document;
        cyclic.FindBin(parent)->parent_id = child;
        Check(!cyclic.Validate(message), "bin hierarchy cycles are rejected");
    });

    Test(
        "markers are addressable, serializable and byte-exactly reversible",
        [] {
            Document document = EditDocument();
            const Ulid first = "01K82000000000000000000001";
            const Ulid second = "01K82000000000000000000002";
            document.sequence.markers = {
                {first, "Opening", {0, 25}, "#e84a5f", "chapter"},
                {second, "Interview", {125, 25}, "blue", "comment"},
            };
            std::string validation;
            Check(document.Validate(validation),
                  "marker fixture validates: " + validation);
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            AddMarkerOperation add{
                {{}, "B-roll", {30000, 1001}, "#33aa77", "select"}, -1};
            Check(Apply(log, document, add, "add marker"),
                  "marker addition succeeds");
            const auto& storedAdd =
                std::get<AddMarkerOperation>(log.AppliedEntries().back().op);
            Check(IsValidUlid(storedAdd.marker.id) &&
                      document.FindMarker(storedAdd.marker.id),
                  "AddMarker receives and retains a stable ULID");
            Check(storedAdd.insertion_index == 2,
                  "AddMarker records its exact canonical position");
            const std::string added = document.SaveToString();
            const std::string addJson = SerializeOperation(storedAdd);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(addJson, parsed, error, message) &&
                      SerializeOperation(parsed) == addJson,
                  "AddMarker JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "AddMarker undo restores original bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == added,
                  "AddMarker redo restores marker ID and bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "second AddMarker undo is byte-exact");

            UpdateMarkerOperation update{
                second, "Interview hero", {1001, 24000}, "amber", "approved"};
            Check(Apply(log, document, update, "update marker"),
                  "marker update succeeds");
            const std::string updateJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(updateJson, parsed, error, message) &&
                      SerializeOperation(parsed) == updateJson,
                  "UpdateMarker JSON round-trips canonically");
            Check(
                document.FindMarker(second)->time == RationalTime{1001, 24000},
                "UpdateMarker preserves its exact rational time");
            Check(
                log.Undo(document, error, message) &&
                    document.SaveToString() == original,
                "UpdateMarker inverse restores every field and original bytes");

            Check(Apply(log, document, RemoveMarkerOperation{first},
                        "remove marker"),
                  "marker removal succeeds");
            const std::string removeJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(removeJson, parsed, error, message) &&
                      SerializeOperation(parsed) == removeJson,
                  "RemoveMarker JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original &&
                      document.sequence.markers.front().id == first,
                  "RemoveMarker undo restores the original position and bytes");

            // D6 -- docs/roadmap/DIALOGUE-2026-10.md. A marker with a
            // duration is a span; one without keeps the bytes it always had.
            Check(original.find("\"category\":\"chapter\"}") !=
                          std::string::npos &&
                      addJson.find("\"duration\"") == std::string::npos &&
                      updateJson.find("\"duration\"") == std::string::npos,
                  "a point marker writes no duration, in the document or in "
                  "the journal");
            const Ulid span = "01K82000000000000000000003";
            AddMarkerOperation addSpan{
                {span, "Passage", {50, 25}, "#4da3ff", "note", {75, 25}}, -1};
            Check(Apply(log, document, addSpan, "add span marker"),
                  "a marker with a duration is added");
            const std::string spanJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(spanJson ==
                      "{\"type\":\"AddMarker\",\"marker\":{\"id\":"
                      "\"01K82000000000000000000003\",\"name\":\"Passage\","
                      "\"time\":{\"value\":50,\"rate\":25},\"color\":"
                      "\"#4da3ff\",\"category\":\"note\",\"duration\":"
                      "{\"value\":75,\"rate\":25}},\"insertion_index\":2}",
                  "AddMarker spells a span's duration after its category: " +
                      spanJson);
            Check(DeserializeOperation(spanJson, parsed, error, message) &&
                      SerializeOperation(parsed) == spanJson,
                  "a span's AddMarker JSON round-trips canonically");
            const std::string withSpan = document.SaveToString();
            Check(withSpan.find("\"category\":\"note\",\"duration\":"
                                "{\"value\":75,\"rate\":25}}") !=
                      std::string::npos,
                  "the document spells the span the same way");
            Document reread;
            std::string readError;
            Check(
                Document::LoadFromString(withSpan, reread, readError) &&
                    reread.FindMarker(span) &&
                    reread.FindMarker(span)->duration == RationalTime{75, 25} &&
                    reread.SaveToString() == withSpan,
                "and reads it back to the same bytes: " + readError);

            UpdateMarkerOperation stretch{span,      "Passage", {25, 25},
                                          "#4da3ff", "note",    {150, 25}};
            Check(Apply(log, document, stretch, "stretch span marker"),
                  "a span is moved and stretched in one update");
            const std::string stretchJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(stretchJson ==
                      "{\"type\":\"UpdateMarker\",\"marker_id\":"
                      "\"01K82000000000000000000003\",\"name\":\"Passage\","
                      "\"time\":{\"value\":25,\"rate\":25},\"color\":"
                      "\"#4da3ff\",\"category\":\"note\",\"duration\":"
                      "{\"value\":150,\"rate\":25}}",
                  "UpdateMarker spells the duration last: " + stretchJson);
            Check(DeserializeOperation(stretchJson, parsed, error, message) &&
                      SerializeOperation(parsed) == stretchJson,
                  "a span's UpdateMarker JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withSpan,
                  "UpdateMarker undo restores the span's time and duration, "
                  "byte for byte");
            Check(Apply(log, document,
                        UpdateMarkerOperation{
                            span, "Passage", {50, 25}, "#4da3ff", "note"},
                        "span to point"),
                  "an update without a duration makes the marker a point");
            Check(document.FindMarker(span)->duration.value == 0 &&
                      document.SaveToString().find(
                          "\"duration\":{\"value\":75") == std::string::npos,
                  "and the point writes no duration");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withSpan,
                  "undoing it brings the span back, byte for byte");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "undoing the span's AddMarker restores the original bytes");
            ExpectRejected(
                document,
                UpdateMarkerOperation{
                    second, "Invalid", {5, 25}, "blue", "comment", {-1, 25}},
                EditError::ValidationFailed, "negative marker duration");

            ExpectRejected(document,
                           UpdateMarkerOperation{
                               second, "Invalid", {-1, 25}, "blue", "comment"},
                           EditError::ValidationFailed, "negative marker time");
            ExpectRejected(document,
                           RemoveMarkerOperation{"01K82000000000000000000009"},
                           EditError::UnknownMarker, "unknown marker");
        });

    Test("empty undo/redo and redo clearing", [] {
        Document document = EditDocument();
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Undo(document, error, message) &&
                  error == EditError::EmptyUndo,
              "undo on empty log returns EmptyUndo");
        Check(!log.Redo(document, error, message) &&
                  error == EditError::EmptyRedo,
              "redo on empty log returns EmptyRedo");
        Check(document.SaveToString() == original,
              "empty undo/redo leave document unchanged");

        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "trim before undo"),
              "trim applies");
        Check(log.Undo(document, error, message), "undo succeeds");
        Check(log.UndoneCount() == 1, "undo populates redo stack");
        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[1].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "new trim"),
              "new trim applies");
        Check(log.UndoneCount() == 0, "new operation clears redo stack");
    });

    Test("20 operations full undo/redo canonical hash", [] {
        Document document = EditDocument();
        // Keep one base clip so eight end insertions form a simple, valid
        // chain.
        document.sequence.tracks[0].clips.erase(
            document.sequence.tracks[0].clips.begin() + 1,
            document.sequence.tracks[0].clips.end());
        std::string validation;
        Check(document.Validate(validation),
              "hash fixture validates: " + validation);
        const std::string originalJson = document.SaveToString();
        const uint64_t originalHash = CanonicalHash(document);
        EditLog log;
        std::vector<Ulid> inserted;
        const std::vector<Ulid> insertedIds = {
            "01K30000000000000000000001", "01K30000000000000000000002",
            "01K30000000000000000000003", "01K30000000000000000000004",
            "01K30000000000000000000005", "01K30000000000000000000006",
            "01K30000000000000000000007", "01K30000000000000000000008",
        };

        for (int index = 0; index < 8; ++index) {
            const RationalTime end = Timeline(document).Duration();
            Check(Apply(log, document,
                        InsertClipOperation{document.sequence.tracks[0].id,
                                            document.sources[index % 2].id,
                                            {400 + index * 10, 25},
                                            {5, 25},
                                            end,
                                            insertedIds[index],
                                            {}},
                        "hash insert " + std::to_string(index)),
                  "hash insertion succeeds");
            inserted.push_back(
                std::get<InsertClipOperation>(log.AppliedEntries().back().op)
                    .clip_id);
        }
        for (int index = 4; index < 8; ++index) {
            Check(Apply(log, document,
                        TrimClipOperation{inserted[index],
                                          TrimEdge::Tail,
                                          {-1, 25},
                                          std::nullopt},
                        "hash tail trim " + std::to_string(index)),
                  "hash tail trim succeeds");
        }
        for (int index = 0; index < 4; ++index) {
            Check(
                Apply(
                    log, document,
                    TrimClipOperation{
                        inserted[index], TrimEdge::Head, {1, 25}, std::nullopt},
                    "hash head trim " + std::to_string(index)),
                "hash head trim succeeds");
        }
        Check(Apply(log, document, RemoveClipOperation{inserted[0], {}},
                    "hash remove 0"),
              "first hash remove succeeds");
        Check(Apply(log, document, RemoveClipOperation{inserted[2], {}},
                    "hash remove 2"),
              "second hash remove succeeds");
        Check(Apply(log, document,
                    TrimClipOperation{document.sequence.tracks[0].clips[0].id,
                                      TrimEdge::Tail,
                                      {-1, 25},
                                      std::nullopt},
                    "hash base tail trim"),
              "base tail trim succeeds");
        Check(Apply(log, document,
                    TrimClipOperation{
                        inserted[7], TrimEdge::Head, {1, 25}, std::nullopt},
                    "hash final head trim"),
              "final head trim succeeds");
        Check(log.AppliedCount() == 20, "exactly 20 operations were applied");
        const uint64_t editedHash = CanonicalHash(document);
        Check(editedHash != originalHash,
              "20 operations materially change document");

        EditError error = EditError::None;
        std::string message;
        for (int index = 0; index < 20; ++index) {
            Check(log.Undo(document, error, message),
                  "undo " + std::to_string(index) + ": " + message);
        }
        const uint64_t firstUndoHash = CanonicalHash(document);
        Check(firstUndoHash == originalHash &&
                  document.SaveToString() == originalJson,
              "full undo restores canonical bytes exactly");

        for (int index = 0; index < 20; ++index) {
            Check(log.Redo(document, error, message),
                  "redo " + std::to_string(index) + ": " + message);
        }
        Check(CanonicalHash(document) == editedHash,
              "full redo restores edited canonical hash");
        for (int index = 0; index < 20; ++index) {
            Check(log.Undo(document, error, message),
                  "second undo " + std::to_string(index) + ": " + message);
        }
        const uint64_t secondUndoHash = CanonicalHash(document);
        Check(secondUndoHash == originalHash &&
                  document.SaveToString() == originalJson,
              "redo then full undo restores canonical bytes exactly again");
        std::cout << "HASH original=" << originalHash
                  << " edited=" << editedHash << " undo1=" << firstUndoHash
                  << " undo2=" << secondUndoHash << '\n';
    });

    Test("track locking is undoable and blocks edits", [] {
        Document document = EditDocument();
        EditLog log;
        const Ulid trackId = document.sequence.tracks[0].id;
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        Check(Apply(log, document, SetTrackLockOperation{trackId, true},
                    "lock track"),
              "track lock applies");
        Check(document.sequence.tracks[0].locked, "track is marked locked");
        ExpectRejected(
            document,
            TrimClipOperation{clipId, TrimEdge::Tail, {-1, 25}, std::nullopt},
            EditError::LockedTrack, "trim on locked track");
        const std::string serialized =
            SerializeOperation(SetTrackLockOperation{trackId, true});
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SetTrackLock JSON round-trips");
        Check(log.Undo(document, error, message), "track unlock undo");
        Check(!document.sequence.tracks[0].locked,
              "undo restores the unlocked state");
    });

    Test("sync locking is independent, undoable, and serializable", [] {
        Document document = EditDocument();
        EditLog log;
        const Ulid trackId = document.sequence.tracks[0].id;
        Check(document.sequence.tracks[0].sync_lock,
              "new and migrated tracks start sync locked");
        Check(Apply(log, document, SetTrackSyncLockOperation{trackId, false},
                    "disable sync lock"),
              "sync lock operation applies");
        Check(!document.sequence.tracks[0].sync_lock &&
                  !document.sequence.tracks[0].locked,
              "sync lock does not change hard lock");
        const std::string serialized =
            SerializeOperation(SetTrackSyncLockOperation{trackId, false});
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SetTrackSyncLock JSON round-trips");
        Check(log.Undo(document, error, message) &&
                  document.sequence.tracks[0].sync_lock,
              "undo restores sync lock");
    });

    Test("track output state round-trips and is undoable", [] {
        Document document = EditDocument();
        EditLog log;
        const std::string original = document.SaveToString();
        const Ulid trackId = document.sequence.tracks[0].id;
        SetTrackOutputOperation change{trackId, false, true, true};
        Check(Apply(log, document, change, "change track output"),
              "track output operation applies");
        const DocumentTrack* changed = document.FindTrack(trackId);
        Check(changed && !changed->visible && changed->muted && changed->solo,
              "all output flags are stored");
        const std::string serialized = SerializeOperation(change);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SetTrackOutput JSON round-trips");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "undo restores canonical bytes exactly");
    });

    Test("ripple trim shifts downstream and sync-locked tracks atomically", [] {
        Document document = EditDocument();
        DocumentTrack audio;
        audio.id = "01K20000000000000000000020";
        audio.kind = "audio";
        audio.index = 1;
        audio.clips.push_back({"01K20000000000000000000021",
                               document.sources[0].id,
                               {400, 25},
                               {5, 25},
                               {20, 25},
                               false});
        document.sequence.tracks.push_back(audio);
        const std::string before = document.SaveToString();
        EditLog log;
        RippleTrimOperation ripple{document.sequence.tracks[0].clips[0].id,
                                   TrimEdge::Tail,
                                   {5, 25},
                                   {},
                                   {audio.id},
                                   {}};
        Check(Apply(log, document, ripple, "ripple tail trim"),
              "ripple trim applies");
        Check(document.sequence.tracks[0].clips[0].duration ==
                      RationalTime{15, 25} &&
                  document.sequence.tracks[0].clips[1].timeline_in ==
                      RationalTime{25, 25} &&
                  document.sequence.tracks[1].clips[0].timeline_in ==
                      RationalTime{25, 25},
              "ripple extends the edge and shifts every affected downstream "
              "clip");
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "RippleTrim JSON round-trips exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores every ripple-trimmed track");

        document.sequence.tracks[1].locked = true;
        ExpectRejected(document, ripple, EditError::LockedTrack,
                       "ripple cannot shift a locked sync track");
    });

    // AX-2026-09 -- a trim that takes time out takes the same stretch out of
    // each sync-locked follower. A cutaway that started on the cut used to
    // slide into the shot before it: the run refused the edit as an overlap.
    Test("a ripple trim takes its stretch out of the sync-locked tracks", [] {
        Document document = EditDocument();
        DocumentTrack cutaways;
        cutaways.id = "01K20000000000000000000030";
        cutaways.kind = "video";
        cutaways.index = 1;
        cutaways.sync_lock = true;
        const auto cutaway = [&](const char* id, int64_t in, int64_t length) {
            return DocumentClip{
                id, document.sources[1].id, {500, 25}, {length, 25}, {in, 25}};
        };
        cutaways.clips = {cutaway("01K20000000000000000000031", 10, 10),
                          cutaway("01K20000000000000000000032", 20, 8),
                          cutaway("01K20000000000000000000033", 30, 5)};
        document.sequence.tracks.push_back(cutaways);
        const std::string before = document.SaveToString();
        EditLog log;
        // The head of the second shot loses four frames: [20, 24) goes.
        const RippleTrimOperation head{"01K20000000000000000000005",
                                       TrimEdge::Head,
                                       {4, 25},
                                       {},
                                       {cutaways.id},
                                       {}};
        Check(Apply(log, document, head, "ripple head trim"),
              "a head trim under a cutaway placed on the cut applies");
        const DocumentTrack& followed = document.sequence.tracks[1];
        Check(followed.clips.size() == 3 &&
                  followed.clips[0].timeline_in == RationalTime{10, 25} &&
                  followed.clips[0].duration == RationalTime{10, 25},
              "the cutaway that ends on the cut does not move");
        Check(followed.clips[1].timeline_in == RationalTime{20, 25} &&
                  followed.clips[1].duration == RationalTime{4, 25} &&
                  followed.clips[1].source_in == RationalTime{500, 25},
              "the one that starts on it stays with its words, whole from "
              "its head, four frames shorter");
        Check(followed.clips[2].timeline_in == RationalTime{26, 25},
              "the one after the stretch moves back by it");
        Check(document.sequence.tracks[0].clips[2].timeline_in ==
                  RationalTime{36, 25},
              "as the interview does");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores both tracks");

        // The tail of the first shot loses four frames: [6, 10) goes, and a
        // cutaway across that end keeps its head and loses its tail.
        Document tail = EditDocument();
        DocumentTrack across = cutaways;
        across.clips = {cutaway("01K20000000000000000000034", 5, 10),
                        cutaway("01K20000000000000000000035", 15, 5)};
        tail.sequence.tracks.push_back(across);
        EditLog tailLog;
        const RippleTrimOperation shorter{"01K20000000000000000000004",
                                          TrimEdge::Tail,
                                          {-4, 25},
                                          {},
                                          {across.id},
                                          {}};
        Check(Apply(tailLog, tail, shorter, "ripple tail trim"),
              "a tail trim under a cutaway across the cut applies");
        const DocumentTrack& shortened = tail.sequence.tracks[1];
        Check(shortened.clips[0].timeline_in == RationalTime{5, 25} &&
                  shortened.clips[0].duration == RationalTime{6, 25} &&
                  shortened.clips[1].timeline_in == RationalTime{11, 25},
              "the cutaway across the end is shortened, the next moves back");
    });

    Test("ripple trim refuses a source range outside the real rush bounds", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const RippleTrimOperation operation{
            document.sequence.tracks[0].clips[0].id,
            TrimEdge::Tail,
            {1000, 25},
            {},
            {},
            {}};
        Check(!log.Apply(document, operation, error, message) &&
                  error == EditError::InvalidOperation,
              "ripple trim beyond the rush is InvalidOperation: " + message);
        Check(message.find("within [0/25, 1000/25)") != std::string::npos &&
                  message.find("requested [100/25, 1110/25)") !=
                      std::string::npos,
              "ripple trim reports the real source bounds and request: " +
                  message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == before,
              "rejected ripple trim is never journaled");
    });

    Test("trim clip refuses a source range outside the real rush bounds", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const TrimClipOperation operation{
            document.sequence.tracks[0].clips[0].id,
            TrimEdge::Tail,
            {1000, 25},
            std::nullopt};
        Check(!log.Apply(document, operation, error, message) &&
                  error == EditError::InvalidOperation,
              "trim beyond the rush is InvalidOperation: " + message);
        Check(message.find("within [0/25, 1000/25)") != std::string::npos &&
                  message.find("requested [100/25, 1110/25)") !=
                      std::string::npos,
              "trim reports the real source bounds and request: " + message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == before,
              "rejected trim is never journaled");
    });

    Test("A5 synchronized ripple and cuts are exact and reversible", [] {
        const Ulid syncTrackId = "01K20000000000000000000020";
        {
            Document document = EditDocument();
            DocumentTrack synced{syncTrackId,
                                 "video",
                                 1,
                                 {{"01K20000000000000000000021",
                                   document.sources[0].id,
                                   {400, 25},
                                   {10, 25},
                                   {20, 25}}}};
            document.sequence.tracks.push_back(synced);
            const std::string before = document.SaveToString();
            EditLog log;
            RemoveClipOperation remove{
                document.sequence.tracks[0].clips[0].id, {syncTrackId}, {}};
            Check(Apply(log, document, remove, "synchronized remove"),
                  "remove_clip accepts a synchronized track");
            Check(document.sequence.tracks[0].clips[0].timeline_in ==
                          RationalTime{10, 25} &&
                      document.sequence.tracks[1].clips[0].timeline_in ==
                          RationalTime{10, 25},
                  "remove_clip ripples every named track by the same exact "
                  "duration");
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(DeserializeOperation(json, parsed, error, message) &&
                      std::get<RemoveClipOperation>(parsed).sync_track_ids ==
                          std::vector<Ulid>{syncTrackId} &&
                      SerializeOperation(parsed) == json,
                  "RemoveClip sync_track_ids round-trip canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "RemoveClip synchronized undo restores exact bytes");
        }

        const auto linkedDocument = [&](bool crossingSyncClip) {
            Document document;
            document.sources = {
                {"01K20000000000000000000001", "A.MP4", {25, 1}, {1000, 25}}};
            const Ulid group = "01K20000000000000000000030";
            DocumentClip video{"01K20000000000000000000031",
                               document.sources[0].id,
                               {100, 25},
                               {10, 25},
                               {0, 25}};
            video.link_group_id = group;
            video.sync_anchor_clip_id = video.id;
            DocumentClip audio = video;
            audio.id = "01K20000000000000000000032";
            audio.sync_anchor_clip_id = video.id;
            DocumentClip followingVideo{"01K20000000000000000000033",
                                        document.sources[0].id,
                                        {200, 25},
                                        {10, 25},
                                        {10, 25}};
            DocumentClip followingAudio = followingVideo;
            followingAudio.id = "01K20000000000000000000034";
            const RationalTime syncStart =
                crossingSyncClip ? RationalTime{0, 25} : RationalTime{10, 25};
            DocumentClip synced{"01K20000000000000000000035",
                                document.sources[0].id,
                                {300, 25},
                                {10, 25},
                                syncStart};
            document.sequence.tracks = {
                {"01K20000000000000000000036",
                 "video",
                 0,
                 {video, followingVideo}},
                {syncTrackId, "video", 1, {synced}},
                {"01K20000000000000000000037",
                 "audio",
                 2,
                 {audio, followingAudio}},
            };
            return document;
        };

        {
            Document document = linkedDocument(false);
            const std::string before = document.SaveToString();
            const Ulid videoId = document.sequence.tracks[0].clips[0].id;
            const Ulid audioId = document.sequence.tracks[2].clips[0].id;
            const Ulid group = document.FindClip(videoId)->link_group_id;
            EditLog log;
            RemoveLinkedClipsOperation remove{
                group, {videoId, audioId}, {syncTrackId}, {}};
            Check(Apply(log, document, remove, "synchronized linked remove"),
                  "remove_linked_clips accepts a synchronized track");
            Check(document.sequence.tracks[0].clips[0].timeline_in ==
                          RationalTime{0, 25} &&
                      document.sequence.tracks[1].clips[0].timeline_in ==
                          RationalTime{0, 25} &&
                      document.sequence.tracks[2].clips[0].timeline_in ==
                          RationalTime{0, 25},
                  "linked removal leaves all three downstream tracks "
                  "aligned");
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(
                DeserializeOperation(json, parsed, error, message) &&
                    std::get<RemoveLinkedClipsOperation>(parsed)
                            .sync_track_ids == std::vector<Ulid>{syncTrackId} &&
                    SerializeOperation(parsed) == json,
                "RemoveLinkedClips sync_track_ids round-trip canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "linked synchronized removal undo restores exact bytes");
        }

        {
            Document document = linkedDocument(true);
            const std::string before = document.SaveToString();
            const Ulid videoId = document.sequence.tracks[0].clips[0].id;
            const Ulid audioId = document.sequence.tracks[2].clips[0].id;
            const Ulid group = document.FindClip(videoId)->link_group_id;
            EditLog log;
            SplitLinkedClipsOperation split{
                group, {videoId, audioId}, {5, 25}, {}, {},
                {},    {syncTrackId},      {}};
            Check(Apply(log, document, split, "synchronized linked split"),
                  "split_linked_clips accepts a synchronized track");
            Check(document.sequence.tracks[0].clips.size() == 3 &&
                      document.sequence.tracks[1].clips.size() == 2 &&
                      document.sequence.tracks[2].clips.size() == 3,
                  "linked split cuts the crossing clip on the synchronized "
                  "track too");
            const std::string after = document.SaveToString();
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            EditError error = EditError::None;
            std::string message;
            Check(
                DeserializeOperation(json, parsed, error, message) &&
                    std::get<SplitLinkedClipsOperation>(parsed)
                            .sync_track_ids == std::vector<Ulid>{syncTrackId} &&
                    SerializeOperation(parsed) == json,
                "SplitLinkedClips sync_track_ids round-trip canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "linked synchronized split undo restores exact bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == after,
                  "linked synchronized split redo restores generated IDs");
        }
    });

    Test("roll edit moves one contiguous cut without changing total duration",
         [] {
             Document document = EditDocument();
             document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
             document.sequence.tracks[0].clips[2].timeline_in = {30, 25};
             const std::string before = document.SaveToString();
             const RationalTime durationBefore = Timeline(document).Duration();
             EditLog log;
             RollEditOperation roll{{{document.sequence.tracks[0].clips[0].id,
                                      document.sequence.tracks[0].clips[1].id}},
                                    {2, 25},
                                    {}};
             Check(Apply(log, document, roll, "roll edit"),
                   "roll edit applies");
             const DocumentClip& left = document.sequence.tracks[0].clips[0];
             const DocumentClip& right = document.sequence.tracks[0].clips[1];
             Check(left.duration == RationalTime{12, 25} &&
                       right.timeline_in == RationalTime{12, 25} &&
                       right.source_in == RationalTime{202, 25} &&
                       right.duration == RationalTime{8, 25} &&
                       Timeline(document).Duration() == durationBefore,
                   "roll preserves the program duration and clip contiguity");
             const std::string serialized =
                 SerializeOperation(log.AppliedEntries().back().op);
             Operation parsed = RemoveTrackOperation{};
             EditError error = EditError::None;
             std::string message;
             Check(DeserializeOperation(serialized, parsed, error, message) &&
                       SerializeOperation(parsed) == serialized,
                   "RollEdit JSON round-trips exactly");
             Check(log.Undo(document, error, message) &&
                       document.SaveToString() == before,
                   "one undo restores both sides of the roll edit");
         });

    Test("roll edit refuses a source range outside the real rush bounds", [] {
        Document document = EditDocument();
        document.sources.push_back(
            {"01K20000000000000000000022", "short.MP4", {25, 1}, {110, 25}});
        document.sequence.tracks[0].clips[0].source_id =
            "01K20000000000000000000022";
        document.sequence.tracks[0].clips[1].timeline_in = {10, 25};
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const RollEditOperation operation{
            {{document.sequence.tracks[0].clips[0].id,
              document.sequence.tracks[0].clips[1].id}},
            {1, 25},
            {}};
        Check(!log.Apply(document, operation, error, message) &&
                  error == EditError::InvalidOperation,
              "roll beyond the rush is InvalidOperation: " + message);
        Check(
            message.find("within [0/25, 110/25)") != std::string::npos &&
                message.find("requested [100/25, 111/25)") != std::string::npos,
            "roll reports the real source bounds and request: " + message);
        Check(log.AppliedCount() == 0 && document.SaveToString() == before,
              "rejected roll edit is never journaled");
    });

    // BENCH-2026-09 -- two adjacent cartons share a boundary the way two
    // shots share a cut. roll_edit demanded a source and refused every pair
    // of captions with UnknownSource, so moving the boundary took two
    // trim_clip calls and two undo steps.
    const auto captionDocument = [] {
        Document document = EditDocument();
        CaptionStyle style;
        style.id = "01K20000000000000000000030";
        document.sequence.caption_styles = {style};
        DocumentClip first;
        first.id = "01K20000000000000000000031";
        first.duration = {20, 25};
        first.timeline_in = {0, 25};
        first.include_audio = false;
        first.caption_group_id = style.id;
        first.caption_text = "Bonjour à tous";
        DocumentClip second = first;
        second.id = "01K20000000000000000000032";
        second.timeline_in = {20, 25};
        second.duration = {15, 25};
        second.caption_text = "et bienvenue";
        document.sequence.tracks.push_back(
            {"01K20000000000000000000033", "caption", 1, {first, second}});
        return document;
    };

    Test("roll edit moves the boundary between two adjacent captions", [&] {
        Document document = captionDocument();
        const std::string before = document.SaveToString();
        const RationalTime durationBefore = Timeline(document).Duration();
        EditLog log;
        const RollEditOperation roll{
            {{"01K20000000000000000000031", "01K20000000000000000000032"}},
            {-4, 25},
            {}};
        Check(Apply(log, document, roll, "caption roll"),
              "a roll between two cartons applies");
        const DocumentClip* first =
            document.FindClip("01K20000000000000000000031");
        const DocumentClip* second =
            document.FindClip("01K20000000000000000000032");
        Check(
            first && second && first->duration == RationalTime{16, 25} &&
                second->timeline_in == RationalTime{16, 25} &&
                second->duration == RationalTime{19, 25} &&
                second->source_in == captionDocument()
                                         .FindClip("01K20000000000000000000032")
                                         ->source_in &&
                first->timeline_in.add(first->duration) == second->timeline_in,
            "the shared boundary moves 4 frames earlier, nothing else does");
        Check(first && second && first->caption_text == "Bonjour à tous" &&
                  second->caption_text == "et bienvenue" &&
                  first->caption_group_id == "01K20000000000000000000030" &&
                  second->caption_group_id == "01K20000000000000000000030",
              "each carton keeps its own text and style");
        Check(Timeline(document).Duration() == durationBefore,
              "a caption roll leaves the program duration alone");
        const std::string after = document.SaveToString();
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "a caption RollEdit round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores both cartons byte for byte");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == after,
              "redo replays the caption roll exactly");
    });

    Test("roll edit refuses to empty a caption or to pair it with media", [&] {
        const Document document = captionDocument();
        const auto refusal = [&](const RollEditOperation& roll) {
            Document copy = document;
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            const bool applied = log.Apply(copy, roll, error, message);
            return std::make_pair(
                !applied && error == EditError::InvalidOperation &&
                    copy.SaveToString() == document.SaveToString() &&
                    log.AppliedCount() == 0,
                message);
        };
        const Ulid first = "01K20000000000000000000031";
        const Ulid second = "01K20000000000000000000032";
        auto outcome = refusal({{{first, second}}, {15, 25}, {}});
        Check(outcome.first && outcome.second.find("caption clip '" + second +
                                                   "'") != std::string::npos,
              "rolling the next carton down to nothing is refused by name: " +
                  outcome.second);
        outcome = refusal({{{first, second}}, {-20, 25}, {}});
        Check(outcome.first && outcome.second.find("caption clip '" + first +
                                                   "'") != std::string::npos,
              "rolling the first carton down to nothing is refused by name: " +
                  outcome.second);
        outcome =
            refusal({{{first, "01K20000000000000000000005"}}, {1, 25}, {}});
        Check(
            outcome.first && outcome.second.find("cannot pair caption clip") !=
                                 std::string::npos,
            "a carton never rolls against a clip that reads media: " +
                outcome.second);
        outcome =
            refusal({{{"01K20000000000000000000004", second}}, {1, 25}, {}});
        Check(outcome.first &&
                  outcome.second.find("with media clip "
                                      "'01K20000000000000000000004'") !=
                      std::string::npos,
              "the refusal names both clips whichever side the carton is on: " +
                  outcome.second);
    });

    Test("slip edit changes only the source window and is reversible", [] {
        Document document = EditDocument();
        const std::string before = document.SaveToString();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        EditLog log;
        SlipEditOperation slip{{clipId}, {5, 25}, {}};
        Check(Apply(log, document, slip, "slip edit"), "slip edit applies");
        const DocumentClip& clip = document.sequence.tracks[0].clips[0];
        Check(clip.source_in == RationalTime{105, 25} &&
                  clip.duration == RationalTime{10, 25} &&
                  clip.timeline_in == RationalTime{0, 25},
              "slip advances source without moving or resizing the clip");
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveTrackOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "SlipEdit JSON round-trips exactly");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "slip undo restores the document byte-exactly");

        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, slip, EditError::LockedTrack,
                       "slip rejects a locked track");
    });

    Test("an interval subdivides a clip in one exact, reversible gesture", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const DocumentClip* clip = document.FindClip(clipId);
        const RationalTime start = clip->timeline_in;
        const RationalTime end = start.add(clip->duration);
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const std::string before = document.SaveToString();

        // Stated as a duration, not as a frame count the caller worked out.
        // That is the whole point of the resolver.
        SplitClipAtPositionsOperation op;
        Check(ResolveIntervalSplits(document, clipId, {2, 25}, op, message),
              "resolving an interval succeeds: " + message);
        Check(!op.positions.empty(), "the resolver produced cut positions");
        bool ascending = true;
        for (size_t index = 0; index < op.positions.size(); ++index) {
            if (op.positions[index] <= start || end <= op.positions[index])
                ascending = false;
            if (index && op.positions[index] <= op.positions[index - 1])
                ascending = false;
        }
        Check(ascending,
              "resolved positions are strictly increasing and inside the clip");

        // An interval whose rate differs from the clip's: 1/5s against a
        // 25fps clip. Nobody converted anything by hand, and what comes out
        // is exactly one interval past the clip's start -- this is the
        // off-by-one-per-cut that principle 7 exists to prevent. Checked
        // before the document is subdivided, while clipId still names the
        // whole clip.
        SplitClipAtPositionsOperation crossRate;
        Check(ResolveIntervalSplits(document, clipId, {1, 5}, crossRate,
                                    message) &&
                  crossRate.positions.size() == 1 &&
                  crossRate.positions[0].compare(start.add({1, 5})) == 0,
              "an interval stated in another timebase resolves exactly");

        const size_t clipsBefore = document.sequence.tracks[0].clips.size();
        Check(Apply(log, document, op, "subdivide at one-second interval"),
              "the interval split applies");
        Check(document.sequence.tracks[0].clips.size() ==
                  clipsBefore + op.positions.size(),
              "each resolved position produced exactly one more clip");
        const std::string after = document.SaveToString();

        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SplitClipAtPositions JSON round-trips canonically");

        // The reason this is one operation and not N: one undo puts the
        // whole subdivision back.
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo reverses every cut of the gesture, byte-exactly");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == after,
              "redo reproduces the subdivision byte-exactly");

        // Rejections are checked against a pristine document: `document` has
        // been subdivided by now, so its clipId names a much shorter first
        // piece and would fail these for the wrong reason.
        Document fresh = EditDocument();
        std::string resolveError;
        SplitClipAtPositionsOperation rejected;
        Check(!ResolveIntervalSplits(fresh, clipId, {0, 1}, rejected,
                                     resolveError),
              "a zero interval is refused rather than looping forever");
        Check(!ResolveIntervalSplits(fresh, clipId, {9999, 1}, rejected,
                                     resolveError),
              "an interval longer than the clip is refused, not a silent "
              "no-op");
        Check(!ResolveIntervalSplits(fresh, "01K20000000000000000000099",
                                     {1, 1}, rejected, resolveError),
              "an unknown clip is refused");

        ExpectRejected(fresh, SplitClipAtPositionsOperation{clipId, {}, {}},
                       EditError::InvalidOperation,
                       "an interval split with no positions");
        ExpectRejected(
            fresh,
            SplitClipAtPositionsOperation{
                clipId, {start.add({4, 25}), start.add({2, 25})}, {}},
            EditError::InvalidOperation,
            "positions that are not strictly increasing");
        ExpectRejected(fresh, SplitClipAtPositionsOperation{clipId, {end}, {}},
                       EditError::InvalidTimelineIn,
                       "a position on the clip's own edge");
    });

    Test("splitting a clip carries its grade and caption onto both halves", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;

        Check(Apply(log, document,
                    SetClipEffectsOperation{
                        clipId,
                        {{{}, "color.exposure", {{"amount", {35, 100}}}}}},
                    "grade the clip"),
              "grading the clip to be split succeeds");
        const DocumentClip* before = document.FindClip(clipId);
        const RationalTime cut = before->timeline_in.add(
            {before->duration.value / 2, before->duration.rate});
        const std::string graded = document.SaveToString();

        SplitClipOperation split{clipId, cut};
        Check(Apply(log, document, split, "split the graded clip"),
              "splitting a graded clip succeeds");
        const Ulid rightId =
            std::get<SplitClipOperation>(log.AppliedEntries().back().op)
                .right_clip_id;
        const DocumentClip* left = document.FindClip(clipId);
        const DocumentClip* right = document.FindClip(rightId);
        // The regression this pins: the right half used to be built
        // from an explicit field list, so anything added to
        // DocumentClip later -- effects, captions -- was dropped on
        // every piece after the first cut, silently.
        Check(right && right->effects.size() == 1 &&
                  left->effects.size() == 1 &&
                  right->effects[0].type == "color.exposure" &&
                  right->effects[0].params.at("amount").num == 35 &&
                  right->effects[0].params.at("amount").den == 100,
              "the right half of a split keeps the grade");
        // Same grade, its own effect IDs: a ClipEffect id is unique
        // document-wide, so cloning the stack verbatim would produce a
        // document that fails validation.
        Check(right && right->effects[0].id != left->effects[0].id,
              "the copied grade gets its own effect IDs");
        Check(right && right->include_audio == left->include_audio &&
                  right->link_group_id == left->link_group_id &&
                  right->sync_anchor_clip_id == left->sync_anchor_clip_id,
              "the right half of a split keeps its link and sync identity");
        const std::string afterSplit = document.SaveToString();
        const std::string splitJson =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation reparsed = RemoveClipOperation{};
        Check(DeserializeOperation(splitJson, reparsed, error, message) &&
                  SerializeOperation(reparsed) == splitJson,
              "SplitClip JSON round-trips canonically with its effect IDs");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == graded,
              "undoing the split restores the graded clip byte-exactly");
        // The reason right_effect_ids is stored rather than generated
        // per application: without it, redo would mint fresh ULIDs and
        // land on different bytes every time.
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == afterSplit,
              "redoing the split reproduces the same effect IDs");

        // A log written before splits carried the grade has no
        // right_effect_ids at all. Every project with a cut in its
        // history contains these, so refusing them would make undo and
        // redo fail on real projects rather than on a fixture.
        Operation legacy = RemoveClipOperation{};
        const std::string legacyJson =
            "{\"type\":\"SplitClip\",\"clip_id\":\"" + clipId +
            "\",\"timeline_position\":{\"value\":" + std::to_string(cut.value) +
            ",\"rate\":" + std::to_string(cut.rate) +
            "},\"right_clip_id\":\"01K20000000000000000000098\"}";
        Check(DeserializeOperation(legacyJson, legacy, error, message) &&
                  std::get<SplitClipOperation>(legacy).right_effect_ids.empty(),
              "a SplitClip logged without right_effect_ids still parses");
    });

    Test(
        "clip color effects are addressable, serializable and byte-exactly "
        "reversible",
        [] {
            Document document = EditDocument();
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            SetClipEffectsOperation setEffects{
                clipId,
                {{{}, "color.exposure", {{"amount", {35, 100}}}},
                 {{}, "color.contrast", {{"amount", {10, 100}}}}}};
            Check(Apply(log, document, setEffects, "set clip effects"),
                  "clip effects apply succeeds");
            const auto& stored = std::get<SetClipEffectsOperation>(
                log.AppliedEntries().back().op);
            Check(stored.effects.size() == 2 &&
                      IsValidUlid(stored.effects[0].id) &&
                      IsValidUlid(stored.effects[1].id),
                  "SetClipEffects fills in stable ULIDs for new effects");
            const std::string withEffects = document.SaveToString();
            const std::string effectsJson = SerializeOperation(stored);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(effectsJson, parsed, error, message) &&
                      SerializeOperation(parsed) == effectsJson,
                  "SetClipEffects JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "SetClipEffects undo restores original bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withEffects,
                  "SetClipEffects redo restores the effect stack and bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "second SetClipEffects undo is byte-exact");

            ExpectRejected(
                document,
                SetClipEffectsOperation{"01K20000000000000000000099", {}},
                EditError::UnknownClip, "unknown clip_id for SetClipEffects");
            ExpectRejected(
                document,
                SetClipEffectsOperation{clipId, {{{}, "brightness", {}}}},
                EditError::ValidationFailed,
                "effect type outside the color.* family");

            // F1.3 -- wheels and curves travel through the same operation
            // and the same journal format, and undo byte for byte.
            SetClipEffectsOperation wheelsAndCurves{
                clipId,
                {{{},
                  "color.wheels",
                  {{"lift_b", {-1, 50}}, {"gain_master", {1, 10}}}},
                 {{},
                  "color.curves",
                  {{"r_x0", {0, 1}},
                   {"r_y0", {1, 20}},
                   {"r_x1", {1, 1}},
                   {"r_y1", {19, 20}}}}}};
            Check(Apply(log, document, wheelsAndCurves, "wheels and curves"),
                  "wheels and curves apply");
            const std::string gradedJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(gradedJson, parsed, error, message) &&
                      SerializeOperation(parsed) == gradedJson,
                  "wheels and curves round-trip through the journal format");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "wheels and curves undo byte for byte");
            ExpectRejected(document,
                           SetClipEffectsOperation{clipId,
                                                   {{{},
                                                     "color.curves",
                                                     {{"g_x0", {1, 2}},
                                                      {"g_y0", {0, 1}},
                                                      {"g_x1", {1, 3}},
                                                      {"g_y1", {1, 1}}}}}},
                           EditError::ValidationFailed,
                           "a curve that goes back in x");

            document.sequence.tracks[0].locked = true;
            ExpectRejected(document, setEffects, EditError::LockedTrack,
                           "clip effects reject a locked track");
        });

    Test("clip opacity is exact, addressable and byte-exactly reversible", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document, SetClipOpacityOperation{clipId, {425, 1000}},
                    "set clip opacity"),
              "clip opacity applies");
        const std::string changed = document.SaveToString();
        Check(document.FindClip(clipId)->opacity.num == 425,
              "exact opacity is stored on the addressed clip");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipOpacity JSON round-trips canonically");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipOpacity undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipOpacity redo restores byte-identical JSON");
        ExpectRejected(document, SetClipOpacityOperation{clipId, {11, 10}},
                       EditError::ValidationFailed,
                       "opacity above one is rejected");
        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, SetClipOpacityOperation{clipId, {1, 2}},
                       EditError::LockedTrack,
                       "clip opacity rejects a locked track");
    });

    // EF3 -- docs/roadmap/EFFETS-2026-10.md.
    Test("clip blur is exact, reduced and byte-exactly reversible", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        Check(original.find("\"blur\"") == std::string::npos &&
                  original.find("\"version\": 24") == std::string::npos,
              "a document that blurs nothing spells no blur and keeps its "
              "version");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document, SetClipBlurOperation{clipId, {20, 2}},
                    "set clip blur"),
              "clip blur applies");
        const std::string changed = document.SaveToString();
        const DocumentClip* blurred = document.FindClip(clipId);
        Check(blurred != nullptr && blurred->blur.num == 10 &&
                  blurred->blur.den == 1,
              "the blur is stored reduced: 20/2 is 10/1");
        Check(changed.find("\"blur\":{\"num\":10,\"den\":1}") !=
                      std::string::npos &&
                  changed.find("\"version\": 24") != std::string::npos,
              "a blurred clip is written, at version 24");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Check(json == "{\"type\":\"SetClipBlur\",\"clip_id\":\"" + clipId +
                          "\",\"blur\":{\"num\":10,\"den\":1}}",
              "the journal keeps the reduced blur: " + json);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipBlur JSON round-trips canonically");
        Document reread;
        Check(Document::LoadFromString(changed, reread, message) &&
                  reread.SaveToString() == changed,
              "a blurred document reads back to the same bytes: " + message);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipBlur undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipBlur redo restores byte-identical JSON");
        Check(Apply(log, document, SetClipBlurOperation{clipId, {0, 5}},
                    "remove the blur") &&
                  document.SaveToString() == original,
              "a blur set to 0/5 is removed, and the file is the one it was");

        ExpectRejected(document, SetClipBlurOperation{clipId, {201, 1}},
                       EditError::ValidationFailed,
                       "a blur past 200 sequence pixels is rejected");
        ExpectRejected(document, SetClipBlurOperation{clipId, {-1, 2}},
                       EditError::ValidationFailed,
                       "a negative blur is rejected");
        ExpectRejected(document, SetClipBlurOperation{clipId, {1, 0}},
                       EditError::InvalidOperation,
                       "a blur without a denominator is rejected");
        ExpectRejected(
            document,
            SetClipBlurOperation{"01K2000000000000000000000Z", {10, 1}},
            EditError::UnknownClip, "an unknown clip is named");
        Document withSound = document;
        DocumentTrack sound;
        sound.id = "01K20000000000000000000007";
        sound.kind = "audio";
        sound.index = 1;
        DocumentClip voice = withSound.sequence.tracks[0].clips[0];
        voice.id = "01K20000000000000000000008";
        sound.clips = {voice};
        withSound.sequence.tracks.push_back(sound);
        ExpectRejected(withSound, SetClipBlurOperation{voice.id, {10, 1}},
                       EditError::InvalidOperation,
                       "a sound clip takes no blur");

        // A blurred picture keeps one size and one place: a framing that
        // moves is refused with it, an opacity that moves is not.
        Document animated = document;
        DocumentClip& shot = animated.sequence.tracks[0].clips[0];
        shot.blur = {10, 1};
        ClipKeyframe first;
        first.id = "01K80000000000000000000K01";
        first.time = {0, 25};
        first.value = {1, 1};
        ClipKeyframe second = first;
        second.id = "01K80000000000000000000K02";
        second.time = {5, 25};
        second.value = {23, 20};
        ClipParameterAnimation zoom;
        zoom.parameter = ClipAnimatedParameter::FramingScale;
        zoom.keyframes = {first, second};
        shot.animation = {zoom};
        std::string invalid;
        Check(!animated.Validate(invalid) &&
                  invalid.find("animates its framing") != std::string::npos,
              "a blur with an animated framing is refused: " + invalid);
        second.value = {1, 2};
        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        fade.keyframes = {first, second};
        shot.animation = {fade};
        Check(animated.Validate(invalid),
              "a blur with an animated opacity is valid: " + invalid);

        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, SetClipBlurOperation{clipId, {10, 1}},
                       EditError::LockedTrack,
                       "clip blur rejects a locked track");
    });

    // R3 -- docs/roadmap/REEL-2026-09.md.
    Test("clip speed is exact, addressable and byte-exactly reversible", [] {
        Document document = EditDocument();
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        Check(original.find("\"speed\"") == std::string::npos,
              "a document nobody gave a speed writes no speed key");
        Check(original.find("\"version\": 7") != std::string::npos,
              "and keeps version 7");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document, SetClipSpeedOperation{clipId, {1, 2}},
                    "set clip speed"),
              "clip speed applies");
        const std::string changed = document.SaveToString();
        Check(document.FindClip(clipId)->speed.num == 1 &&
                  document.FindClip(clipId)->speed.den == 2,
              "the exact fraction is stored on the addressed clip");
        Check(changed.find("\"speed\":{\"num\":1,\"den\":2}") !=
                  std::string::npos,
              "the clip that has one spells it: " + changed);
        Check(changed.find("\"version\": 10") != std::string::npos,
              "and the document declares version 10");
        // Only the clip that was addressed: the other two keep their bytes.
        Check(CountOccurrences(changed, "\"speed\"") == 1,
              "no other clip gained a key");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipSpeed JSON round-trips canonically");
        Check(json == "{\"type\":\"SetClipSpeed\",\"clip_id\":\"" + clipId +
                          "\",\"speed\":{\"num\":1,\"den\":2}}",
              "and is spelled as the journal pins it: " + json);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipSpeed undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipSpeed redo restores byte-identical JSON");
        Check(log.Undo(document, error, message), "back to 1/1");

        // A document written at version 10 is refused by a reader that only
        // knows 9, rather than read as a montage at 1/1.
        Check(Apply(log, document, SetClipSpeedOperation{clipId, {1, 2}},
                    "set clip speed"),
              "clip speed applies again");
        std::string asNine = document.SaveToString();
        const size_t versionAt = asNine.find("\"version\": 10");
        Check(versionAt != std::string::npos, "the saved document is v10");
        asNine.replace(versionAt, std::string("\"version\": 10").size(),
                       "\"version\": 9");
        Document older;
        std::string olderError;
        Check(!Document::LoadFromString(asNine, older, olderError) &&
                  olderError.find("cannot hold a clip speed") !=
                      std::string::npos,
              "a version-9 file holding a speed is refused: " + olderError);
        Check(log.Undo(document, error, message), "back to 1/1 again");

        // A speed that is not a speed, and a source_in the export could not
        // count from.
        ExpectRejected(document, SetClipSpeedOperation{clipId, {-1, 2}},
                       EditError::InvalidOperation, "a reverse is refused");
        // R4 -- decided on 8 October 2026: 0/1 is the freeze, a speed of a
        // picture clip carrying no sound (clip_speed_tests holds the
        // refusals of a sound and of a linked clip).
        {
            Document frozen = document;
            EditLog frozenLog;
            EditError frozenError = EditError::None;
            std::string frozenMessage;
            Check(frozenLog.Apply(frozen, SetClipSpeedOperation{clipId, {0, 1}},
                                  frozenError, frozenMessage),
                  "a freeze is a speed now: " + frozenMessage);
        }
        {
            Document offset = EditDocument();
            // Half a source frame into the rush: the monitor could resolve
            // it, the export could not count from it.
            offset.sequence.tracks[0].clips[0].source_in = {201, 50};
            ExpectRejected(offset,
                           SetClipSpeedOperation{
                               offset.sequence.tracks[0].clips[0].id, {2, 5}},
                           EditError::ValidationFailed,
                           "a source_in inside a source frame is refused");
        }
        document.sequence.tracks[0].locked = true;
        ExpectRejected(document, SetClipSpeedOperation{clipId, {1, 2}},
                       EditError::LockedTrack,
                       "clip speed rejects a locked track");
    });

    // R3, criterion 3 -- the operations that turn a timeline span into a
    // source span, on a shot that is slowed. Each one either lands on a
    // source frame, exactly, or is refused by name.
    Test("split, trim, slip and ripple read a slowed clip's source exactly",
         [] {
             // A 50 i/s rush in a 25 i/s sequence, slowed to 1/2: one
             // timeline frame is one source frame.
             const auto fifty = [] {
                 Document document;
                 document.sources = {{"01K20000000000000000000001",
                                      "A.MP4",
                                      {50, 1},
                                      {2000, 50}}};
                 DocumentTrack track;
                 track.id = "01K20000000000000000000003";
                 track.kind = "video";
                 DocumentClip clip;
                 clip.id = "01K20000000000000000000004";
                 clip.source_id = "01K20000000000000000000001";
                 clip.source_in = {100, 50};
                 clip.duration = {20, 25};
                 clip.timeline_in = {0, 25};
                 clip.speed = {1, 2};
                 track.clips = {clip};
                 document.sequence.tracks = {track};
                 return document;
             };
             const Ulid clipId = "01K20000000000000000000004";
             EditError error = EditError::None;
             std::string message;

             {
                 // Trimming four timeline frames off the head advances the
                 // source by four source frames, not eight.
                 Document document = fifty();
                 EditLog log;
                 const std::string original = document.SaveToString();
                 Check(Apply(log, document,
                             TrimClipOperation{clipId, TrimEdge::Head, {4, 25}},
                             "trim"),
                       "a head trim applies to a slowed clip");
                 Check(document.FindClip(clipId)->source_in ==
                           RationalTime{104, 50},
                       "the source advanced by four source frames");
                 Check(log.Undo(document, error, message) &&
                           document.SaveToString() == original,
                       "and undo puts the bytes back");
             }
             {
                 Document document = fifty();
                 EditLog log;
                 Check(Apply(log, document,
                             SlipEditOperation{{clipId}, {2, 25}}, "slip"),
                       "a slip applies to a slowed clip");
                 Check(document.FindClip(clipId)->source_in ==
                           RationalTime{102, 50},
                       "and moves the window by two source frames");
             }
             {
                 Document document = fifty();
                 EditLog log;
                 Check(Apply(log, document, SplitClipOperation{clipId, {6, 25}},
                             "split"),
                       "a split applies to a slowed clip");
                 const DocumentTrack& track = document.sequence.tracks[0];
                 Check(track.clips.size() == 2 &&
                           track.clips[1].source_in == RationalTime{106, 50} &&
                           track.clips[1].speed.num == 1 &&
                           track.clips[1].speed.den == 2,
                       "and the right-hand piece opens six source frames in, "
                       "at the same speed");
             }
             {
                 Document document = fifty();
                 EditLog log;
                 Check(
                     Apply(log, document,
                           RippleTrimOperation{clipId, TrimEdge::Head, {2, 25}},
                           "ripple"),
                     "a ripple trim applies to a slowed clip");
                 Check(document.FindClip(clipId)->source_in ==
                           RationalTime{102, 50},
                       "and advances the source by two source frames");
             }
             {
                 // At 2/5 on a 25 i/s rush, one timeline frame is 2/5 of a
                 // source frame: a one-frame trim would leave source_in
                 // inside a frame, and is refused rather than exported one
                 // frame away from what the monitor shows.
                 Document document = EditDocument();
                 EditLog log;
                 const Ulid slowId = document.sequence.tracks[0].clips[0].id;
                 Check(Apply(log, document,
                             SetClipSpeedOperation{slowId, {2, 5}}, "speed"),
                       "2/5 applies on a frame-aligned source_in");
                 ExpectRejected(
                     document,
                     TrimClipOperation{slowId, TrimEdge::Head, {1, 25}},
                     EditError::ValidationFailed,
                     "a trim that would land inside a source frame is "
                     "refused");
                 Check(Apply(log, document,
                             TrimClipOperation{slowId, TrimEdge::Head, {5, 25}},
                             "trim"),
                       "and five frames, which are two source frames, apply");
                 Check(document.FindClip(slowId)->source_in ==
                           RationalTime{102, 25},
                       "landing two source frames in");
             }
         });

    // R3, criterion 2 -- one speed for the whole link group, so picture and
    // sound cannot part company.
    Test("a speed on a linked clip is set on its whole group", [] {
        Document document = EditDocument();
        document.sequence.tracks.push_back({"01K20000000000000000000080",
                                            "audio",
                                            1,
                                            {{"01K20000000000000000000081",
                                              "01K20000000000000000000001",
                                              {100, 25},
                                              {10, 25},
                                              {0, 25}}}});
        const Ulid videoId = document.sequence.tracks[0].clips[0].id;
        const Ulid audioId = document.sequence.tracks.back().clips[0].id;
        document.FindClip(videoId)->link_group_id =
            "01K20000000000000000000082";
        document.FindClip(audioId)->link_group_id =
            "01K20000000000000000000082";
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document, SetClipSpeedOperation{videoId, {1, 2}},
                    "set clip speed"),
              "a speed on the picture applies");
        Check(document.FindClip(audioId)->speed.num == 1 &&
                  document.FindClip(audioId)->speed.den == 2,
              "and the sound of the group follows it");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "undo puts the whole group back, byte for byte");
    });

    // R1 -- docs/roadmap/REEL-2026-09.md.
    Test("clip framing is exact, addressable and byte-exactly reversible", [] {
        Document document = EditDocument();
        // A framing needs the media's displayed size: the operation refuses
        // one it cannot resolve rather than storing a rectangle nobody can
        // draw.
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        for (const DocumentSource& source : document.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename = source.path;
            media.codec = "h264";
            media.orientation = "landscape";
            media.has_video = true;
            media.width = 1920;
            media.height = 1080;
            media.rate = source.rate;
            media.duration = source.duration;
            media.metadata_complete = true;
            document.library.push_back(media);
        }
        const Ulid firstId = document.sequence.tracks[0].clips[0].id;
        const Ulid secondId = document.sequence.tracks[0].clips[1].id;
        const std::string original = document.SaveToString();
        Check(original.find("\"framing\"") == std::string::npos,
              "a document nobody framed writes no framing key");
        Check(original.find("\"version\": 7") != std::string::npos,
              "and keeps version 7");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        framing.scale = EffectParamValue{22, 20};
        framing.position_x = EffectParamValue{-1, 8};
        Check(Apply(log, document,
                    SetClipFramingOperation{
                        {{firstId, framing}, {secondId, framing}}},
                    "set clip framing"),
              "one framing on two clips applies");
        const std::string changed = document.SaveToString();
        const DocumentClip* framed = document.FindClip(firstId);
        Check(framed->framing.mode == FramingMode::Fill &&
                  framed->framing.scale.num == 11 &&
                  framed->framing.scale.den == 10,
              "the fraction is reduced before it is stored");
        Check(changed.find("\"version\": 11") != std::string::npos,
              "the document declares version 11: " + changed);
        // Only the two clips addressed: the third keeps its bytes.
        Check(CountOccurrences(changed, "\"framing\"") == 2,
              "no other clip gained a key");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipFraming JSON round-trips canonically");
        Check(json.find("\"type\":\"SetClipFraming\",\"clips\":[{\"clip_id\":"
                        "\"" +
                        firstId + "\",\"framing\":{\"mode\":\"fill\"") !=
                  std::string::npos,
              "and is spelled as the journal pins it: " + json);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipFraming undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipFraming redo restores byte-identical JSON");

        // Framing one of the two back to the default leaves the other
        // framed, and the inverse still restores both exactly.
        Check(Apply(log, document,
                    SetClipFramingOperation{{{firstId, ClipFraming{}}}},
                    "unframe one"),
              "a clip is unframed by setting the default");
        Check(CountOccurrences(document.SaveToString(), "\"framing\"") == 1,
              "and its key disappears");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == changed,
              "undo puts it back byte for byte");

        // A document written at version 11 is refused by a reader that only
        // knows 10, rather than delivered with the bars the framing removed.
        std::string asTen = document.SaveToString();
        const size_t versionAt = asTen.find("\"version\": 11");
        Check(versionAt != std::string::npos, "the saved document is v11");
        asTen.replace(versionAt, std::string("\"version\": 11").size(),
                      "\"version\": 10");
        Document older;
        std::string olderError;
        Check(!Document::LoadFromString(asTen, older, olderError) &&
                  olderError.find("cannot hold a clip framing") !=
                      std::string::npos,
              "a version-10 file holding a framing is refused: " + olderError);
        Check(log.Undo(document, error, message), "back to unframed");
        Check(document.SaveToString() == original,
              "and byte-identical to where this started");
    });

    Test("a framing that cannot be drawn is refused when it is set", [] {
        Document document = EditDocument();
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        for (const DocumentSource& source : document.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename = source.path;
            media.codec = "h264";
            media.orientation = "landscape";
            media.has_video = true;
            media.width = 1920;
            media.height = 1080;
            media.rate = source.rate;
            media.duration = source.duration;
            media.metadata_complete = true;
            document.library.push_back(media);
        }
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        ClipFraming impossible;
        impossible.crop_left = EffectParamValue{1, 2};
        impossible.crop_right = EffectParamValue{1, 2};
        Check(!log.Apply(document,
                         SetClipFramingOperation{{{clipId, impossible}}}, error,
                         message) &&
                  message.find("leave a picture") != std::string::npos,
              "a crop that leaves no picture is refused: " + message);
        ClipFraming offscreen;
        offscreen.position_y = EffectParamValue{3, 1};
        Check(
            !log.Apply(document, SetClipFramingOperation{{{clipId, offscreen}}},
                       error, message) &&
                message.find("outside the sequence frame") != std::string::npos,
            "a picture pushed off screen is refused: " + message);
        Check(!log.Apply(document, SetClipFramingOperation{{}}, error, message),
              "an empty framing operation is refused: " + message);
        Check(document.SaveToString() == original,
              "and none of them changed a byte");

        // A media that was never probed has no size to frame against.
        Document unprobed = document;
        unprobed.library[0].metadata_complete = false;
        ClipFraming fill;
        fill.mode = FramingMode::Fill;
        EditLog unprobedLog;
        Check(!unprobedLog.Apply(unprobed,
                                 SetClipFramingOperation{{{clipId, fill}}},
                                 error, message) &&
                  message.find("never probed") != std::string::npos,
              "an unprobed media is refused: " + message);

        // A locked track stops the whole operation, not part of it.
        Document locked = document;
        locked.sequence.tracks[0].locked = true;
        EditLog lockedLog;
        Check(
            !lockedLog.Apply(locked, SetClipFramingOperation{{{clipId, fill}}},
                             error, message),
            "clip framing rejects a locked track");
    });

    // Out of lot -- the author, 29 October 2026: Resolve's Enable/Disable Clip.
    Test("a clip is disabled and enabled again byte for byte", [] {
        Document document = EditDocument();
        const Ulid firstId = document.sequence.tracks[0].clips[0].id;
        const Ulid secondId = document.sequence.tracks[0].clips[1].id;
        const Ulid thirdId = document.sequence.tracks[0].clips[2].id;
        const std::string original = document.SaveToString();
        // The colour management's own switch is the one "enabled" a
        // document without a disabled clip writes.
        const size_t switches = CountOccurrences(original, "\"enabled\"");
        Check(original.find("\"version\": 7") != std::string::npos,
              "a document that disables nothing keeps version 7");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document,
                    SetClipEnabledOperation{
                        {{firstId, false}, {secondId, false}}},
                    "disable two clips"),
              "two clips are disabled in one operation");
        const std::string changed = document.SaveToString();
        Check(changed.find("\"version\": 23") != std::string::npos,
              "the document declares version 23: " + changed);
        Check(CountOccurrences(changed, "\"enabled\"") == switches + 2,
              "only the two clips addressed carry the key");
        const DocumentClip* disabled = document.FindClip(firstId);
        Check(disabled != nullptr && !disabled->enabled &&
                  disabled->timeline_in == RationalTime{0, 25} &&
                  disabled->duration == RationalTime{10, 25} &&
                  disabled->source_in == RationalTime{100, 25},
              "a disabled clip keeps its place, its length and its source");
        Check(document.FindClip(thirdId)->enabled, "the third stays enabled");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipEnabled JSON round-trips canonically");
        Check(
            json == "{\"type\":\"SetClipEnabled\",\"clips\":[{\"clip_id\":\"" +
                        firstId + "\",\"enabled\":false},{\"clip_id\":\"" +
                        secondId + "\",\"enabled\":false}]}",
            "and is spelled as the journal pins it: " + json);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipEnabled undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipEnabled redo restores byte-identical JSON");

        // A selection that mixes both states comes back as it was: the
        // inverse carries each clip's own previous state.
        Check(
            Apply(log, document,
                  SetClipEnabledOperation{{{firstId, true}, {thirdId, false}}},
                  "mixed states"),
            "one clip enabled and another disabled in one step");
        Check(document.FindClip(firstId)->enabled &&
                  !document.FindClip(secondId)->enabled &&
                  !document.FindClip(thirdId)->enabled,
              "each clip takes the state it was given");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == changed,
              "undo restores each clip's own state, byte for byte");

        // A disabled clip is cut like any other: both halves stay disabled.
        Check(Apply(log, document, SplitClipOperation{firstId, {5, 25}},
                    "split the disabled clip"),
              "a disabled clip is cut");
        const DocumentClip* right = document.FindClip(
            std::get<SplitClipOperation>(log.AppliedEntries().back().op)
                .right_clip_id);
        Check(right != nullptr && !right->enabled &&
                  !document.FindClip(firstId)->enabled,
              "both halves of a disabled clip stay disabled");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == changed,
              "and the cut undoes byte for byte");

        // Enabled again, the film is the one it was, version included.
        Check(
            Apply(log, document,
                  SetClipEnabledOperation{{{firstId, true}, {secondId, true}}},
                  "enable again"),
            "the clips are enabled again");
        Check(document.SaveToString() == original,
              "enabled again, the document is byte-identical to the start");

        // Refusals change nothing.
        const std::string before = document.SaveToString();
        Check(!log.Apply(document, SetClipEnabledOperation{{}}, error, message),
              "an empty operation is refused");
        Check(!log.Apply(
                  document,
                  SetClipEnabledOperation{{{firstId, false}, {firstId, true}}},
                  error, message) &&
                  message.find("twice") != std::string::npos,
              "a clip named twice is refused: " + message);
        Check(!log.Apply(document,
                         SetClipEnabledOperation{
                             {{"01K2000000000000000000000Z", false}}},
                         error, message) &&
                  error == EditError::UnknownClip,
              "an unknown clip is refused: " + message);
        Document locked = document;
        locked.sequence.tracks[0].locked = true;
        EditLog lockedLog;
        Check(!lockedLog.Apply(locked,
                               SetClipEnabledOperation{{{firstId, false}}},
                               error, message) &&
                  error == EditError::LockedTrack,
              "a locked track keeps its clips as they play: " + message);
        Check(document.SaveToString() == before,
              "and none of the refusals changed a byte");
    });

    // R2 -- docs/roadmap/REEL-2026-09.md.
    Test("clip keys are exact, addressable and byte-exactly reversible", [] {
        Document document = EditDocument();
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        for (const DocumentSource& source : document.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename = source.path;
            media.codec = "h264";
            media.orientation = "landscape";
            media.has_video = true;
            media.width = 1920;
            media.height = 1080;
            media.rate = source.rate;
            media.duration = source.duration;
            media.metadata_complete = true;
            document.library.push_back(media);
        }
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        Check(original.find("\"animation\"") == std::string::npos,
              "a document nobody animated writes no animation key");
        Check(original.find("\"version\": 7") != std::string::npos,
              "and keeps version 7");

        // A zoom from 100 % to 115 % over 12 frames, in cubic-bezier: the
        // ticket's own criterion 3, as an operation.
        ClipKeyframe first;
        first.id = "01K95000000000000000000K01";
        first.time = {0, 25};
        first.value = EffectParamValue{2, 2};
        first.interpolation = KeyframeInterpolation::CubicBezier;
        first.easing_x1 = EffectParamValue{2, 5};
        first.easing_y1 = EffectParamValue{0, 1};
        first.easing_x2 = EffectParamValue{1, 5};
        first.easing_y2 = EffectParamValue{1, 1};
        ClipKeyframe last;
        last.id = "01K95000000000000000000K02";
        last.time = {12, 25};
        last.value = EffectParamValue{23, 20};
        last.interpolation = KeyframeInterpolation::Linear;
        ClipParameterAnimation zoom;
        zoom.parameter = ClipAnimatedParameter::FramingScale;
        zoom.keyframes = {first, last};

        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(
            Apply(log, document, SetClipAnimationOperation{{{clipId, {zoom}}}},
                  "set clip animation"),
            "a zoom applies to one clip");
        const std::string changed = document.SaveToString();
        Check(changed.find("\"version\": 12") != std::string::npos,
              "the document declares version 12: " + changed);
        Check(CountOccurrences(changed, "\"animation\"") == 1,
              "no other clip gained a key");
        const std::string json =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, error, message) &&
                  SerializeOperation(parsed) == json,
              "SetClipAnimation JSON round-trips canonically");
        Check(json.find("\"type\":\"SetClipAnimation\",\"clips\":[{\"clip_id\":"
                        "\"" +
                        clipId +
                        "\",\"animation\":[{\"parameter\":\"framing."
                        "scale\"") != std::string::npos,
              "and is spelled as the journal pins it: " + json);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "SetClipAnimation undo restores byte-identical JSON");
        Check(log.Redo(document, error, message) &&
                  document.SaveToString() == changed,
              "SetClipAnimation redo restores byte-identical JSON");

        // Criterion 4 -- adding, changing and removing a key are the same
        // operation, and each of the three undoes byte for byte.
        ClipParameterAnimation withMiddle = zoom;
        ClipKeyframe middle;
        middle.id = "01K95000000000000000000K03";
        middle.time = {6, 25};
        middle.value = EffectParamValue{21, 20};
        withMiddle.keyframes = {first, middle, last};
        Check(Apply(log, document,
                    SetClipAnimationOperation{{{clipId, {withMiddle}}}},
                    "add a key"),
              "a key is added");
        const std::string threeKeys = document.SaveToString();
        Check(CountOccurrences(threeKeys, "01K95000000000000000000K03") == 1,
              "and it is in the file");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == changed,
              "removing it again is byte-exact");
        Check(log.Redo(document, error, message), "and so is putting it back");

        ClipParameterAnimation moved = withMiddle;
        moved.keyframes[1].time = {7, 25};
        moved.keyframes[1].value = EffectParamValue{11, 10};
        Check(
            Apply(log, document, SetClipAnimationOperation{{{clipId, {moved}}}},
                  "move a key"),
            "a key is moved and revalued");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == threeKeys,
              "and moving it back is byte-exact");

        Check(Apply(log, document, SetClipAnimationOperation{{{clipId, {}}}},
                    "clear the animation"),
              "an empty animation clears the clip");
        Check(document.SaveToString() == original,
              "which puts the document back to where it started, byte for "
              "byte -- including its version");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == threeKeys,
              "and undoing the clear restores every key");

        // A version-12 file is refused by a reader that knows only 11,
        // rather than played as a still.
        std::string asEleven = document.SaveToString();
        const size_t versionAt = asEleven.find("\"version\": 12");
        Check(versionAt != std::string::npos, "the saved document is v12");
        asEleven.replace(versionAt, std::string("\"version\": 12").size(),
                         "\"version\": 11");
        Document older;
        std::string olderError;
        Check(!Document::LoadFromString(asEleven, older, olderError) &&
                  olderError.find("cannot hold an animated clip parameter") !=
                      std::string::npos,
              "a version-11 file holding a key is refused: " + olderError);
    });

    // R2 -- criterion 1. The rule, through the operations that move a head.
    Test("keys stay on the frames they were put on", [] {
        Document document = EditDocument();
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        for (const DocumentSource& source : document.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename = source.path;
            media.codec = "h264";
            media.orientation = "landscape";
            media.has_video = true;
            media.width = 1920;
            media.height = 1080;
            media.rate = source.rate;
            media.duration = source.duration;
            media.metadata_complete = true;
            document.library.push_back(media);
        }
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        ClipKeyframe first;
        first.id = "01K96000000000000000000K01";
        first.time = {0, 25};
        first.value = EffectParamValue{1, 1};
        ClipKeyframe last;
        last.id = "01K96000000000000000000K02";
        last.time = {12, 25};
        last.value = EffectParamValue{23, 20};
        ClipParameterAnimation zoom;
        zoom.parameter = ClipAnimatedParameter::FramingScale;
        zoom.keyframes = {first, last};
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document,
                    SetClipAnimationOperation{{{clipId, {zoom}}}}, "animate"),
              "the clip is animated");
        const MediaRate rate = document.sequence.frame_rate;
        const auto scaleAt = [&](const DocumentClip& clip, int64_t frame) {
            ClipSampledParameters sampled;
            std::string sampleError;
            Check(SampleClipParameters(clip, frame, rate, sampled, sampleError),
                  "sampling works: " + sampleError);
            return static_cast<double>(sampled.framing.scale.num) /
                   static_cast<double>(sampled.framing.scale.den);
        };
        const double atTwelve = scaleAt(*document.FindClip(clipId), 12);
        const std::string animated = document.SaveToString();

        // MOVED: timeline_in changes, nothing else. The keys do not move.
        Document moved = document;
        EditLog moveLog;
        const DocumentTrack* movedTrack = moved.FindTrackForClip(clipId);
        const RationalTime target =
            moved.FindClip(clipId)->timeline_in.add({7, 25});
        if (movedTrack != nullptr &&
            moveLog.Apply(moved,
                          MoveClipOperation{clipId, movedTrack->id, target},
                          error, message)) {
            Check(scaleAt(*moved.FindClip(clipId), 12) == atTwelve,
                  "a move leaves the clip's twelfth frame worth what it was");
        }

        // HEAD TRIMMED by five frames: the frame that was twelfth is the
        // seventh now, and the zoom's end went with it.
        Document trimmed = document;
        EditLog trimLog;
        Check(
            trimLog.Apply(
                trimmed, TrimClipOperation{clipId, TrimEdge::Head, {5, 25}, {}},
                error, message),
            "a head trim applies: " + message);
        Check(scaleAt(*trimmed.FindClip(clipId), 7) == atTwelve,
              "the end of the zoom is now the clip's seventh frame");
        Check(trimmed.FindClip(clipId)->animation[0].keyframes.size() == 2,
              "and the key pushed before the head is kept, not dropped");
        Check(trimLog.Undo(trimmed, error, message) &&
                  trimmed.SaveToString() == animated,
              "undoing the trim restores every key byte for byte");

        // CUT IN TWO: both pieces show what the uncut clip showed.
        Document cut = document;
        EditLog cutLog;
        const DocumentClip* whole = cut.FindClip(clipId);
        const RationalTime cutAt = whole->timeline_in.add({5, 25});
        SplitClipOperation split;
        split.clip_id = clipId;
        split.timeline_position = cutAt;
        split.right_clip_id = "01K96000000000000000000C99";
        for (size_t index = 0; index < whole->effects.size(); ++index) {
            split.right_effect_ids.push_back("01K96000000000000000000E0" +
                                             std::to_string(index));
        }
        if (cutLog.Apply(cut, split, error, message)) {
            const DocumentClip* left = cut.FindClip(clipId);
            const DocumentClip* right =
                cut.FindClip("01K96000000000000000000C99");
            Check(left != nullptr && right != nullptr,
                  "the cut made two pieces");
            if (left != nullptr && right != nullptr) {
                for (int64_t frame = 0; frame < 20; ++frame) {
                    const DocumentClip& piece = frame < 5 ? *left : *right;
                    const int64_t local = frame < 5 ? frame : frame - 5;
                    Check(scaleAt(piece, local) ==
                              scaleAt(*document.FindClip(clipId), frame),
                          "frame " + std::to_string(frame) +
                              " of the cut pair shows what the uncut clip "
                              "showed");
                }
            }
            Check(cutLog.Undo(cut, error, message) &&
                      cut.SaveToString() == animated,
                  "and undoing the cut restores the document byte for byte");
        }
    });

    Test("an animation that cannot be drawn is refused when it is set", [] {
        Document document = EditDocument();
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        for (const DocumentSource& source : document.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename = source.path;
            media.codec = "h264";
            media.orientation = "landscape";
            media.has_video = true;
            media.width = 1920;
            media.height = 1080;
            media.rate = source.rate;
            media.duration = source.duration;
            media.metadata_complete = true;
            document.library.push_back(media);
        }
        const Ulid clipId = document.sequence.tracks[0].clips[0].id;
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const auto track = [](ClipAnimatedParameter parameter,
                              EffectParamValue from, EffectParamValue to) {
            ClipKeyframe first;
            first.id = "01K97000000000000000000K01";
            first.time = {0, 25};
            first.value = from;
            ClipKeyframe last;
            last.id = "01K97000000000000000000K02";
            last.time = {12, 25};
            last.value = to;
            ClipParameterAnimation entry;
            entry.parameter = parameter;
            entry.keyframes = {first, last};
            return entry;
        };
        Check(
            !log.Apply(document,
                       SetClipAnimationOperation{
                           {{clipId,
                             {track(ClipAnimatedParameter::FramingPositionY,
                                    {0, 1}, {3, 1})}}}},
                       error, message) &&
                message.find("outside the sequence frame") != std::string::npos,
            "an animation that walks the picture off screen is refused "
            "when it is set: " +
                message);
        // R2c -- one key is accepted (the Inspector's first click on the
        // diamond), and an undo gives every byte back.
        ClipParameterAnimation single =
            track(ClipAnimatedParameter::FramingScale, {1, 1}, {23, 20});
        single.keyframes.pop_back();
        {
            Document keyed = document;
            EditLog keyedLog;
            Check(
                keyedLog.Apply(keyed,
                               SetClipAnimationOperation{{{clipId, {single}}}},
                               error, message) &&
                    keyed.FindClip(clipId)->animation.size() == 1 &&
                    keyed.FindClip(clipId)->animation[0].keyframes.size() == 1,
                "a single key is accepted: " + message);
            Check(keyedLog.Undo(keyed, error, message) &&
                      keyed.SaveToString() == original,
                  "and undone byte for byte: " + message);
        }
        ClipParameterAnimation none = single;
        none.keyframes.clear();
        Check(
            !log.Apply(document, SetClipAnimationOperation{{{clipId, {none}}}},
                       error, message) &&
                message.find("carries no key") != std::string::npos,
            "a parameter without a key is refused: " + message);
        Check(
            !log.Apply(document, SetClipAnimationOperation{{}}, error, message),
            "an empty operation is refused: " + message);
        Check(document.SaveToString() == original,
              "and none of them changed a byte");
        Document locked = document;
        locked.sequence.tracks[0].locked = true;
        EditLog lockedLog;
        Check(!lockedLog.Apply(locked,
                               SetClipAnimationOperation{
                                   {{clipId,
                                     {track(ClipAnimatedParameter::FramingScale,
                                            {1, 1}, {23, 20})}}}},
                               error, message),
              "clip animation rejects a locked track");
    });

    Test(
        "clip audio settings are exact, serializable and byte-exactly "
        "reversible",
        [] {
            Document document = EditDocument();
            document.sequence.tracks.push_back({"01K20000000000000000000090",
                                                "audio",
                                                1,
                                                {{"01K20000000000000000000091",
                                                  "01K20000000000000000000001",
                                                  {100, 25},
                                                  {10, 25},
                                                  {0, 25}}}});
            const Ulid clipId = document.sequence.tracks.back().clips[0].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(
                Apply(log, document,
                      SetClipAudioOperation{clipId, {10, 1}, {2, 25}, {3, 25}},
                      "set clip audio"),
                "clip audio settings apply");
            const std::string changed = document.SaveToString();
            const DocumentClip* clip = document.FindClip(clipId);
            Check(clip && clip->audio_gain_db.num == 10 &&
                      clip->audio_fade_in == RationalTime{2, 25} &&
                      clip->audio_fade_out == RationalTime{3, 25},
                  "audio gain and exact fade durations are stored");
            const std::string json =
                SerializeOperation(log.AppliedEntries().back().op);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(json, parsed, error, message) &&
                      SerializeOperation(parsed) == json,
                  "SetClipAudio JSON round-trips canonically");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "SetClipAudio undo restores byte-identical JSON");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == changed,
                  "SetClipAudio redo restores byte-identical JSON");
            ExpectRejected(
                document,
                SetClipAudioOperation{clipId, {49, 1}, {0, 1}, {0, 1}},
                EditError::ValidationFailed,
                "gain above the supported range is rejected");
            ExpectRejected(
                document,
                SetClipAudioOperation{clipId, {0, 1}, {11, 25}, {0, 1}},
                EditError::ValidationFailed,
                "fade longer than clip is rejected");
            const Ulid videoClip = document.sequence.tracks[0].clips[0].id;
            ExpectRejected(
                document,
                SetClipAudioOperation{videoClip, {0, 1}, {0, 1}, {0, 1}},
                EditError::InvalidOperation,
                "audio settings reject a video clip");
            document.sequence.tracks.back().locked = true;
            ExpectRejected(
                document, SetClipAudioOperation{clipId, {1, 1}, {0, 1}, {0, 1}},
                EditError::LockedTrack,
                "clip audio settings reject a locked track");
        });

    Test(
        "clip audio settings survive an exact-track snapshot in a reloaded "
        "journal",
        [] {
            // QC-2026-09 A7 -- a move records the exact tracks its undo
            // restores. Their format used to leave gain and fades out, so
            // undoing from a reloaded journal (project reopened, any CLI
            // command) reset the clip to 0 dB without fades.
            Document document = EditDocument();
            const Ulid trackId = "01K20000000000000000000090";
            const Ulid clipId = "01K20000000000000000000091";
            document.sequence.tracks.push_back({trackId,
                                                "audio",
                                                1,
                                                {{clipId,
                                                  "01K20000000000000000000001",
                                                  {100, 25},
                                                  {10, 25},
                                                  {0, 25}}}});
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(
                Apply(log, document,
                      SetClipAudioOperation{clipId, {-6, 1}, {0, 25}, {3, 25}},
                      "set clip audio"),
                "clip audio settings apply");
            const std::string beforeMove = document.SaveToString();
            Check(Apply(log, document,
                        MoveClipOperation{clipId, trackId, {40, 25}},
                        "move audio clip"),
                  "audio clip moves");
            const std::string afterMove = document.SaveToString();
            const std::string moveJson =
                SerializeOperation(log.AppliedEntries().back().inverse);
            const std::string gain = R"("audio_gain_db":{"num":-6,"den":1})";
            const std::string fade = R"("audio_fade_in":{"value":0,"rate":25})";
            Check(moveJson.find(gain) != std::string::npos &&
                      moveJson.find(fade) != std::string::npos,
                  "exact tracks carry the gain and a zero fade at its rate");

            EditLog reloaded;
            Check(
                EditLog::Deserialize(log.Serialize(), reloaded, error, message),
                "journal reloads: " + message);
            Document replay = document;
            Check(reloaded.Undo(replay, error, message) &&
                      replay.SaveToString() == beforeMove,
                  "reloaded journal undoes the move byte-identically");
            Check(reloaded.Redo(replay, error, message) &&
                      replay.SaveToString() == afterMove,
                  "reloaded journal redoes the move byte-identically");

            // A clip without audio processing keeps the snapshot bytes that
            // journals written before these fields existed contain.
            Check(Apply(log, document,
                        MoveClipOperation{"01K20000000000000000000004",
                                          "01K20000000000000000000003",
                                          {100, 25}},
                        "move video clip"),
                  "video clip moves");
            Check(SerializeOperation(log.AppliedEntries().back().inverse)
                          .find("\"audio_") == std::string::npos,
                  "default audio settings add nothing to the snapshot");
        });

    Test(
        "caption styles and per-clip captions are addressable, "
        "serializable and byte-exactly reversible",
        [] {
            Document document = EditDocument();
            const Ulid clipId = document.sequence.tracks[0].clips[0].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            AddCaptionStyleOperation addStyle{
                {{}, "Inter", 64, "#ffffff", "bottom"}, -1};
            Check(Apply(log, document, addStyle, "add caption style"),
                  "caption style addition succeeds");
            const auto& storedAdd = std::get<AddCaptionStyleOperation>(
                log.AppliedEntries().back().op);
            const Ulid styleId = storedAdd.style.id;
            Check(IsValidUlid(styleId) && document.FindCaptionStyle(styleId),
                  "AddCaptionStyle receives and retains a stable ULID");
            Check(storedAdd.insertion_index == 0,
                  "AddCaptionStyle records its exact canonical position");
            const std::string withStyle = document.SaveToString();
            const std::string addJson = SerializeOperation(storedAdd);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(addJson, parsed, error, message) &&
                      SerializeOperation(parsed) == addJson,
                  "AddCaptionStyle JSON round-trips canonically");

            SetClipCaptionOperation setCaption{clipId, styleId,
                                               "Bonjour le monde"};
            Check(Apply(log, document, setCaption, "set clip caption"),
                  "clip caption assignment succeeds");
            const std::string withCaption = document.SaveToString();
            const std::string setJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(setJson, parsed, error, message) &&
                      SerializeOperation(parsed) == setJson,
                  "SetClipCaption JSON round-trips canonically");
            Check(document.FindClip(clipId)->caption_text == "Bonjour le monde",
                  "SetClipCaption keeps the exact per-clip text");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withStyle,
                  "SetClipCaption undo restores the pre-caption bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withCaption,
                  "SetClipCaption redo restores caption bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withStyle,
                  "second SetClipCaption undo is byte-exact");

            ExpectRejected(
                document,
                RemoveCaptionStyleOperation{"01K87000000000000000000099"},
                EditError::UnknownCaptionStyle, "unknown caption style_id");
            ExpectRejected(document,
                           SetClipCaptionOperation{"01K20000000000000000000099",
                                                   styleId, "x"},
                           EditError::UnknownClip,
                           "unknown clip_id for SetClipCaption");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "AddCaptionStyle undo restores original bytes");

            document.sequence.tracks[0].locked = true;
            ExpectRejected(document, SetClipCaptionOperation{clipId, "", ""},
                           EditError::LockedTrack,
                           "clip caption rejects a locked track");
        });

    Test(
        "multicam group operations are addressable, serializable and "
        "byte-exactly reversible",
        [] {
            Document document = EditDocument();
            const Ulid clipA = document.sequence.tracks[0].clips[0].id;
            const Ulid clipB = document.sequence.tracks[0].clips[1].id;
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;

            AddMulticamGroupOperation addGroup{
                {{},
                 "Interview",
                 {{{}, "Wide", clipA}, {{}, "Close", clipB}},
                 {}},
                -1};
            Check(Apply(log, document, addGroup, "add multicam group"),
                  "multicam group addition succeeds");
            const auto& storedAdd = std::get<AddMulticamGroupOperation>(
                log.AppliedEntries().back().op);
            const Ulid groupId = storedAdd.group.id;
            const Ulid wideAngleId = storedAdd.group.angles[0].id;
            const Ulid closeAngleId = storedAdd.group.angles[1].id;
            Check(IsValidUlid(groupId) && document.FindMulticamGroup(groupId),
                  "AddMulticamGroup receives and retains a stable ULID");
            Check(IsValidUlid(wideAngleId) && IsValidUlid(closeAngleId) &&
                      wideAngleId != closeAngleId,
                  "AddMulticamGroup assigns stable ULIDs to each angle");
            Check(storedAdd.insertion_index == 0,
                  "AddMulticamGroup records its exact canonical position");
            Check(document.FindMulticamGroup(groupId)->angles.size() == 2 &&
                      document.FindMulticamGroup(groupId)
                          ->active_angle_id.empty(),
                  "AddMulticamGroup starts with no active angle selected");
            const std::string withGroup = document.SaveToString();
            const std::string addJson = SerializeOperation(storedAdd);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(addJson, parsed, error, message) &&
                      SerializeOperation(parsed) == addJson,
                  "AddMulticamGroup JSON round-trips canonically");

            SetMulticamActiveAngleOperation setActive{groupId, wideAngleId};
            Check(Apply(log, document, setActive, "set active angle"),
                  "active angle change succeeds");
            const std::string withActive = document.SaveToString();
            const std::string setJson =
                SerializeOperation(log.AppliedEntries().back().op);
            Check(DeserializeOperation(setJson, parsed, error, message) &&
                      SerializeOperation(parsed) == setJson,
                  "SetMulticamActiveAngle JSON round-trips canonically");
            Check(document.FindMulticamGroup(groupId)->active_angle_id ==
                      wideAngleId,
                  "SetMulticamActiveAngle keeps the exact selected angle");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withGroup,
                  "SetMulticamActiveAngle undo restores the pre-selection "
                  "bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withActive,
                  "SetMulticamActiveAngle redo restores the selection bytes");
            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == withGroup,
                  "second SetMulticamActiveAngle undo is byte-exact");

            ExpectRejected(
                document,
                SetMulticamActiveAngleOperation{"01K88000000000000000000001",
                                                wideAngleId},
                EditError::UnknownMulticamGroup,
                "unknown multicam group_id for SetMulticamActiveAngle");
            ExpectRejected(document,
                           SetMulticamActiveAngleOperation{
                               groupId, "01K88000000000000000000002"},
                           EditError::UnknownMulticamAngle,
                           "active_angle_id outside the group's own angles");
            ExpectRejected(
                document,
                AddMulticamGroupOperation{
                    {groupId, "Duplicate", {{{}, "Wide", clipA}}, {}}, -1},
                EditError::DuplicateId,
                "AddMulticamGroup rejects a group_id already in use");
            ExpectRejected(
                document,
                AddMulticamGroupOperation{
                    {{},
                     "Bad angle",
                     {{{}, "Wide", "01K20000000000000000000099"}},
                     {}},
                    -1},
                EditError::ValidationFailed,
                "AddMulticamGroup angle clip_id must resolve to a video "
                "clip");
            ExpectRejected(
                document,
                AddMulticamGroupOperation{
                    {{},
                     "Same clip twice",
                     {{{}, "Wide", clipA}, {{}, "Close", clipA}},
                     {}},
                    -1},
                EditError::ValidationFailed,
                "AddMulticamGroup rejects a clip reused within one group");
            ExpectRejected(
                document,
                RemoveMulticamGroupOperation{"01K88000000000000000000099"},
                EditError::UnknownMulticamGroup,
                "unknown multicam group_id for RemoveMulticamGroup");

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "AddMulticamGroup undo restores original bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withGroup,
                  "AddMulticamGroup redo restores the group bytes");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == withActive,
                  "AddMulticamGroup redo chain restores the selection bytes");
            Check(log.Undo(document, error, message) &&
                      log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "full multicam undo chain is byte-exact");
        });

    // PL1 -- ROADMAP.md. One intervention, one undo step.
    Test(
        "a batch of insertion, trim and audio setting is exact, serializable "
        "and byte-exactly reversible",
        [] {
            Document document = BatchDocument();
            const std::string original = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(Apply(log, document, InterventionBatch(), "apply batch"),
                  "the batch applies");
            Check(log.AppliedCount() == 1,
                  "three members leave one history entry");
            const std::string changed = document.SaveToString();
            const DocumentClip* inserted =
                document.FindClip(kBatchInsertedClip);
            const DocumentClip* trimmed =
                document.FindClip("01K20000000000000000000004");
            const DocumentClip* sound = document.FindClip(kBatchAudioClip);
            Check(inserted && inserted->timeline_in == RationalTime{60, 25} &&
                      trimmed && trimmed->duration == RationalTime{8, 25} &&
                      sound && sound->audio_gain_db.num == -6 &&
                      sound->audio_fade_out == RationalTime{3, 25},
                  "every member took effect");

            // A batch does what its members do one after the other.
            Document oneByOne = BatchDocument();
            EditLog separate;
            for (const Operation& member : InterventionBatch().operations)
                Apply(separate, oneByOne, member, "member alone");
            Check(oneByOne.SaveToString() == changed,
                  "the batch and its members one by one reach the same bytes");

            const Entry& entry = log.AppliedEntries().back();
            const auto* stored = std::get_if<BatchOperation>(&entry.op);
            const auto* undo = std::get_if<BatchOperation>(&entry.inverse);
            Check(stored && stored->operations.size() == 3 && undo &&
                      undo->operations.size() == 3 &&
                      undo->label == "Resserrer la question 2",
                  "the inverse is a batch of three under the same label");
            Check(stored && std::get<TrimClipOperation>(stored->operations[1])
                                .exact_clip_result.has_value(),
                  "members are kept as applied, exact results included");
            Check(undo && std::holds_alternative<SetClipAudioOperation>(
                              undo->operations.front()),
                  "the inverse undoes the last member first");

            const std::string json = SerializeOperation(entry.op);
            Operation parsed = RemoveClipOperation{};
            Check(DeserializeOperation(json, parsed, error, message) &&
                      SerializeOperation(parsed) == json,
                  "Batch JSON round-trips canonically: " + message);
            Check(json.find(R"({"type":"Batch","label":"Resserrer la question )"
                            R"(2","operations":[{"type":"InsertClip",)") == 0,
                  "members are written as operations, not as strings: " +
                      json.substr(0, 80));

            Check(log.Undo(document, error, message) &&
                      document.SaveToString() == original,
                  "one undo restores byte-identical JSON");
            Check(log.Redo(document, error, message) &&
                      document.SaveToString() == changed,
                  "one redo restores byte-identical JSON");

            EditLog reloaded;
            Check(
                EditLog::Deserialize(log.Serialize(), reloaded, error, message),
                "the journal reloads: " + message);
            Document replay = document;
            Check(reloaded.Undo(replay, error, message) &&
                      replay.SaveToString() == original,
                  "the reloaded journal undoes the batch in one step");
            Check(reloaded.Redo(replay, error, message) &&
                      replay.SaveToString() == changed,
                  "the reloaded journal redoes the batch in one step");
        });

    Test("a removal replayed from its exact state restores the whole clip", [] {
        // PL1 -- a plan prepared on a copy is applied with its members
        // as they were applied there, a removal's exact positions
        // included. That removal's inverse used to be a bare InsertClip:
        // the clip came back without the opacity it had before.
        Document document = BatchDocument();
        document.sequence.tracks[0].clips[0].opacity = {1, 2};
        const std::string original = document.SaveToString();
        Document scratch = document;
        EditLog preview;
        Apply(preview, scratch,
              RemoveClipOperation{"01K20000000000000000000004", {}},
              "prepared removal");
        const Operation prepared = preview.AppliedEntries().back().op;
        Check(!std::get<RemoveClipOperation>(prepared)
                   .exact_timeline_result.empty(),
              "the prepared removal carries exact positions");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, document, BatchOperation{"Plan", {prepared}},
                    "prepared batch") &&
                  document.SaveToString() == scratch.SaveToString(),
              "the prepared batch reaches the preview's bytes");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == original,
              "undo restores the removed clip whole, opacity included");
    });

    Test("a batch refused in its middle leaves nothing applied", [] {
        const Document document = BatchDocument();
        BatchOperation batch = InterventionBatch();
        std::get<TrimClipOperation>(batch.operations[1]).clip_id =
            "01K29999999999999999999999";
        ExpectRejected(document, batch, EditError::UnknownClip,
                       "a batch whose second member names no clip");
        EditLog log;
        Document copy = document;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(copy, batch, error, message) &&
                  message.find("batch operation 1: ") == 0 &&
                  message.find("nothing was applied") != std::string::npos,
              "the refusal names the member: " + message);

        Document locked = document;
        for (DocumentTrack& track : locked.sequence.tracks)
            if (track.kind == "audio") track.locked = true;
        ExpectRejected(locked, InterventionBatch(), EditError::LockedTrack,
                       "a batch whose last member touches a locked track");
    });

    Test("a batch is bounded, labelled and never nested", [] {
        const Document document = BatchDocument();
        ExpectRejected(document, BatchOperation{"Vide", {}},
                       EditError::InvalidOperation, "an empty batch");
        BatchOperation unlabelled = InterventionBatch();
        unlabelled.label.clear();
        ExpectRejected(document, unlabelled, EditError::InvalidOperation,
                       "a batch without a label");
        ExpectRejected(document,
                       BatchOperation{"Imbriqué", {InterventionBatch()}},
                       EditError::InvalidOperation, "a batch in a batch");

        const SetClipAudioOperation gain{
            kBatchAudioClip, {-3, 1}, {0, 25}, {0, 25}};
        BatchOperation largest{"256 réglages", {}};
        largest.operations.assign(kMaxBatchOperations, gain);
        BatchOperation tooLarge = largest;
        tooLarge.operations.push_back(gain);
        ExpectRejected(document, tooLarge, EditError::InvalidOperation,
                       "a batch of 257 operations");
        Document bounded = document;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(Apply(log, bounded, largest, "256 operations") &&
                  log.Undo(bounded, error, message) &&
                  bounded.SaveToString() == document.SaveToString(),
              "a batch of 256 operations applies and undoes in one step");

        Operation parsed = RemoveClipOperation{};
        Check(!DeserializeOperation(SerializeOperation(BatchOperation{
                                        "Imbriqué", {InterventionBatch()}}),
                                    parsed, error, message),
              "the journal refuses a batch in a batch too");
    });

    Test("an intervention folds into one undo step, journal included", [] {
        Document document = BatchDocument();
        Document original = document;
        const Ulid timeline = document.sequence.id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        // A person's edit before the agent starts: it must stay its own step.
        Apply(log, document,
              TrimClipOperation{"01K20000000000000000000006",
                                TrimEdge::Head,
                                {1, 25},
                                std::nullopt},
              "a person's trim");
        Document beforeIntervention = document;
        const InterventionMark mark = MarkIntervention(timeline, log);
        InterventionMark parsedMark;
        Check(ParseInterventionMark(FormatInterventionMark(mark), parsedMark,
                                    message) &&
                  parsedMark.timeline_id == timeline &&
                  parsedMark.history_depth == 1 &&
                  parsedMark.history_anchor == mark.history_anchor,
              "the token carries the mark: " + FormatInterventionMark(mark));

        const BatchOperation plan = InterventionBatch();
        for (const Operation& member : plan.operations)
            Apply(log, document, member, "agent edit");
        Apply(log, document,
              BatchOperation{"Réglage déjà groupé",
                             {SetClipAudioOperation{
                                  kBatchAudioClip, {-9, 1}, {1, 25}, {0, 25}},
                              MoveClipOperation{kBatchInsertedClip,
                                                "01K20000000000000000000003",
                                                {80, 25}}}},
              "agent batch");
        // Renaming the timeline is a project step: the document changes,
        // this history does not, and the fold must not stop for it. No
        // step of this history restores the old name either.
        document.sequence.name = "Version courte";
        original.sequence.name = document.sequence.name;
        beforeIntervention.sequence.name = document.sequence.name;
        const std::string afterIntervention = document.SaveToString();
        size_t grouped = 0;
        Check(log.GroupSince(document, mark, "Agent : resserrer", grouped,
                             error, message),
              "the intervention folds: " + message);
        Check(grouped == 4 && log.AppliedCount() == 2,
              "four steps become one, the person's stays apart");
        const auto* folded =
            std::get_if<BatchOperation>(&log.AppliedEntries().back().op);
        Check(folded && folded->operations.size() == 5 &&
                  folded->label == "Agent : resserrer" &&
                  std::none_of(
                      folded->operations.begin(), folded->operations.end(),
                      [](const Operation& member) {
                          return std::holds_alternative<BatchOperation>(member);
                      }),
              "a batch inside the intervention is flattened, not nested");
        Check(document.SaveToString() == afterIntervention,
              "folding leaves the montage untouched");

        EditLog reloaded;
        Check(EditLog::Deserialize(log.Serialize(), reloaded, error, message),
              "the folded journal reloads: " + message);
        for (EditLog* history : {&log, &reloaded}) {
            Document replay = document;
            Check(
                history->Undo(replay, error, message) &&
                    replay.SaveToString() == beforeIntervention.SaveToString(),
                "one undo reverts the whole intervention");
            Check(history->Undo(replay, error, message) &&
                      replay.SaveToString() == original.SaveToString(),
                  "the person's edit is a step of its own");
            Check(history->Redo(replay, error, message) &&
                      replay.SaveToString() ==
                          beforeIntervention.SaveToString() &&
                      history->Redo(replay, error, message) &&
                      replay.SaveToString() == afterIntervention,
                  "redo replays the person's edit, then the intervention");
        }
    });

    Test("an intervention is not folded over a rewritten history", [] {
        Document document = BatchDocument();
        const Ulid timeline = document.sequence.id;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        size_t grouped = 0;
        const BatchOperation plan = InterventionBatch();
        Apply(log, document, plan.operations[0], "first edit");
        const InterventionMark mark = MarkIntervention(timeline, log);

        // Nothing applied, then one edit: already one step, left as it is.
        std::string journal = log.Serialize();
        Check(log.GroupSince(document, mark, "Rien", grouped, error, message) &&
                  grouped == 0 && log.Serialize() == journal,
              "an empty intervention changes nothing");
        // PRD40 -- one edit is folded too, so the history names it by the
        // intervention's label as it names one of several steps.
        const std::string beforeOne = document.SaveToString();
        Apply(log, document, plan.operations[1], "second edit");
        const std::string afterOne = document.SaveToString();
        Check(log.GroupSince(document, mark, "Une", grouped, error, message) &&
                  grouped == 1 && log.AppliedCount() == 2 &&
                  document.SaveToString() == afterOne,
              "a single step is folded, the montage untouched: " + message);
        const auto* single =
            std::get_if<BatchOperation>(&log.AppliedEntries().back().op);
        Check(single != nullptr && single->label == "Une" &&
                  single->operations.size() == 1 &&
                  !std::holds_alternative<BatchOperation>(
                      single->operations.front()),
              "into a batch of one under the intervention's label");
        journal = log.Serialize();
        Check(log.GroupSince(document, mark, "Une", grouped, error, message) &&
                  grouped == 1 && log.Serialize() == journal,
              "folding the same intervention again changes nothing");
        {
            EditLog copy = log;
            Document replay = document;
            Check(copy.Undo(replay, error, message) &&
                      replay.SaveToString() == beforeOne &&
                      copy.Redo(replay, error, message) &&
                      replay.SaveToString() == afterOne,
                  "the batch of one undoes and redoes its step exactly: " +
                      message);
            EditLog reloaded;
            Check(EditLog::Deserialize(journal, reloaded, error, message) &&
                      reloaded.Serialize() == journal,
                  "the journal of a batch of one reads back: " + message);
        }

        // Undone below the mark.
        Document undone = document;
        EditLog below = log;
        below.Undo(undone, error, message);
        below.Undo(undone, error, message);
        journal = below.Serialize();
        Check(!below.GroupSince(undone, mark, "Trop bas", grouped, error,
                                message) &&
                  error == EditError::InvalidOperation &&
                  below.Serialize() == journal,
              "a history undone below the mark is refused: " + message);

        // Undone below the mark, then edited again to the same depth.
        Apply(below, undone, plan.operations[2], "another first edit");
        Apply(below, undone, plan.operations[1], "another second edit");
        journal = below.Serialize();
        Check(!below.GroupSince(undone, mark, "Réécrit", grouped, error,
                                message) &&
                  below.Serialize() == journal &&
                  message.find("rewritten") != std::string::npos,
              "a history rewritten under the mark is refused: " + message);

        Check(!log.GroupSince(document, mark, "", grouped, error, message),
              "an intervention needs a label");
        InterventionMark ignored;
        Check(
            !ParseInterventionMark("A1", ignored, message) &&
                !ParseInterventionMark(timeline + ":x:0123456789abcdef",
                                       ignored, message) &&
                !ParseInterventionMark(timeline + ":1:0123", ignored, message),
            "a malformed token is refused");
    });

    Test("an intervention of more than 256 operations is left as it was", [] {
        Document document = BatchDocument();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        const InterventionMark mark =
            MarkIntervention(document.sequence.id, log);
        for (size_t index = 0; index <= kMaxBatchOperations; ++index) {
            Apply(log, document,
                  SetClipAudioOperation{kBatchAudioClip,
                                        {static_cast<int32_t>(index % 7), 1},
                                        {0, 25},
                                        {0, 25}},
                  "one of 257 settings");
        }
        const std::string journal = log.Serialize();
        size_t grouped = 0;
        Check(!log.GroupSince(document, mark, "Trop long", grouped, error,
                              message) &&
                  error == EditError::InvalidOperation &&
                  log.Serialize() == journal &&
                  log.AppliedCount() == kMaxBatchOperations + 1,
              "257 steps stay 257 steps: " + message);
    });

    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All edit tests passed\n";
    return 0;
}
