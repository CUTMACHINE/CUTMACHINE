#include "CaptionGeneration.h"
#include "CaptionRender.h"
#include "Document.h"
#include "EditLog.h"
#include "Export.h"
#include "InterviewShort.h"
#include "Subtitles.h"
#include "Transcription.h"
#include "Ulid.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// ITW4 -- ROADMAP.md. Cartons from montage words: rendered width measured by
// drawtext, cuts found on the composited picture, edges set onto them
// without making a carton unreadable.

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

TimelineTranscriptSpan Word(const std::string& text, int64_t start,
                            int64_t frames) {
    TimelineTranscriptSpan span;
    span.text = text;
    span.timeline_in = {start, 25};
    span.duration = {frames, 25};
    return span;
}

// REMOTION-PARITY-2026-09 REM1, scene C20 -- a paginated caption timed on
// transcript words, end to end: a rush's words on disk, a montage whose jump
// cut drops two of them, the words read back at montage positions, cartons
// of at most 20 characters, then the caption track. Each stage is held to
// an exact descriptor, and the cartons to what the scene means: every word
// the montage says is shown once, in order, by the carton on screen while
// it is said. The smoke test plays the same scene in Record.
void CheckTranscriptPagination() {
    std::string error;
    const auto id = [](int number) {
        const std::string digits = std::to_string(number);
        return "01K4C2" + std::string(20 - digits.size(), '0') + digits;
    };
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-c20-" + GenerateUlid());
    std::filesystem::create_directories(directory / "transcripts");

    const Ulid rush = id(1);
    Document document;
    document.sequence.id = id(2);
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    document.sequence.frame_rate = {25, 1};
    document.sources = {{rush, "parole.mov", {25, 1}, {400, 25}}};
    const auto clip = [&](int number, int64_t sourceIn, int64_t at) {
        DocumentClip shot;
        shot.id = id(number);
        shot.source_id = rush;
        shot.source_in = {sourceIn, 25};
        shot.duration = {100, 25};
        shot.timeline_in = {at, 25};
        return shot;
    };
    // Rush [0, 100) then [150, 250): a jump cut at 100 drops "euh bref".
    document.sequence.tracks = {
        {id(3), "video", 0, {clip(4, 0, 0), clip(5, 150, 100)}},
        {id(6), "audio", 1, {clip(7, 0, 0), clip(8, 150, 100)}}};

    struct Said {
        const char* text;
        int64_t start;
        int64_t end;
    };
    const std::vector<Said> source = {
        {"On", 5, 12},       {"commence", 13, 30}, {"par", 31, 38},
        {"la", 39, 43},      {"lumière.", 44, 70}, {"euh", 105, 115},
        {"bref", 120, 135},  {"Puis", 155, 165},   {"on", 166, 172},
        {"cadre", 173, 190}, {"le", 191, 195},     {"visage.", 196, 225},
        {"Voilà.", 230, 245}};
    Transcript transcript;
    transcript.media_id = rush;
    transcript.whisper_model = "ggml-large-v3.bin";
    transcript.language = "fr";
    transcript.source_rate = {25, 1};
    for (const Said& word : source)
        transcript.words.push_back(
            {word.text, {word.start, 25}, {word.end, 25}});
    Check(SaveAudioTranscript(
              (directory / "transcripts" / (rush + ".json")).string(),
              transcript, error),
          "C20: the rush transcript is saved: " + error);

    // The words at montage positions: the second shot's 50 frames earlier,
    // the two words of the dropped stretch gone.
    std::vector<TimelineTranscriptSpan> words;
    Check(BuildTimelineTranscriptWordSpans(document, directory / "transcripts",
                                           words, error),
          "C20: the montage words are read: " + error);
    const std::vector<Said> said = {
        {"On", 5, 12},         {"commence", 13, 30}, {"par", 31, 38},
        {"la", 39, 43},        {"lumière.", 44, 70}, {"Puis", 105, 115},
        {"on", 116, 122},      {"cadre", 123, 140},  {"le", 141, 145},
        {"visage.", 146, 175}, {"Voilà.", 180, 195}};
    bool placed = words.size() == said.size();
    for (size_t index = 0; placed && index < said.size(); ++index)
        placed =
            words[index].text == said[index].text &&
            words[index].timeline_in == RationalTime{said[index].start, 25} &&
            words[index].duration ==
                RationalTime{said[index].end - said[index].start, 25};
    Check(placed,
          "C20: eleven words at their exact montage frames, none of "
          "the dropped stretch");

    // The default line follows the frame: 32 characters when it is wider
    // than tall, 20 on a vertical, square or 4:5 one.
    Check(DefaultCaptionCartonMaximumCharacters(1920, 1080) == 32 &&
              DefaultCaptionCartonMaximumCharacters(3840, 2160) == 32 &&
              DefaultCaptionCartonMaximumCharacters(1080, 1920) == 20 &&
              DefaultCaptionCartonMaximumCharacters(1080, 1080) == 20 &&
              DefaultCaptionCartonMaximumCharacters(1080, 1350) == 20,
          "the default carton length is 32 on a landscape frame, 20 "
          "otherwise");

    CaptionStyle style;
    style.id = id(9);
    style.font_size = 58;
    CaptionGenerationSettings settings;
    settings.maximum_characters = 20;
    std::vector<GeneratedCarton> cartons;
    std::vector<GeneratedCarton> again;
    const bool generated =
        GenerateCaptionCartons(document, style, words, settings, cartons,
                               error) &&
        GenerateCaptionCartons(document, style, words, settings, again, error);
    Check(generated, "C20: the cartons are generated: " + error);
    Check(
        SerializeGeneratedCartons(cartons) == SerializeGeneratedCartons(again),
        "C20: the same words give the same cartons, byte for byte");

    // The pages, frozen. "Puis", ten frames, finds one frame of silence
    // after it and grows back to the jump cut five frames before it, never
    // over the words of the next page; it stays under 0.8 s. The five-frame
    // hole after "visage." closes. The last grows from 15 frames to 0.8 s,
    // which is where the film ends.
    std::string listed;
    for (const GeneratedCarton& carton : cartons)
        listed += "[" + carton.text + " " +
                  std::to_string(carton.timeline_in.value) + "+" +
                  std::to_string(carton.duration.value) + "]";
    Check(listed ==
              "[On commence 5+26][par la lumière. 31+39][Puis 100+16]"
              "[on cadre le visage. 116+64][Voilà. 180+20]",
          "C20: the pages are " + listed);

    // Units: the cartons' words are the montage's, once each, in order.
    std::vector<std::string> shown;
    std::vector<size_t> first;
    for (const GeneratedCarton& carton : cartons) {
        first.push_back(shown.size());
        std::string token;
        for (const char letter : carton.text + " ") {
            if (letter == ' ' || letter == '\n') {
                if (!token.empty()) shown.push_back(token);
                token.clear();
            } else {
                token += letter;
            }
        }
    }
    first.push_back(shown.size());
    std::vector<std::string> texts;
    for (const TimelineTranscriptSpan& word : words) texts.push_back(word.text);
    Check(shown == texts,
          "C20: every montage word is on exactly one page, in order");

    // Bounds: a page opens on its first word, or on a visible cut at most
    // snap_frames before it; it does not overlap the next or run past the
    // film. And the scene's meaning, frame by frame: while a word is said,
    // the page on screen is the one that holds it.
    const std::vector<int64_t> cuts = VisibleCutFrames(document);
    const RationalTime end{200, 25};
    const auto onScreen = [&](const RationalTime& at) -> int {
        for (size_t page = 0; page < cartons.size(); ++page)
            if (cartons[page].timeline_in <= at &&
                at < cartons[page].timeline_in.add(cartons[page].duration))
                return static_cast<int>(page);
        return -1;
    };
    for (size_t page = 0; shown == texts && page < cartons.size(); ++page) {
        const GeneratedCarton& carton = cartons[page];
        const TimelineTranscriptSpan& opening = words[first[page]];
        const int64_t in = carton.timeline_in.to_frames(25, 1);
        const int64_t lead = opening.timeline_in.to_frames(25, 1) - in;
        const bool onCut =
            std::find(cuts.begin(), cuts.end(), in) != cuts.end();
        Check(lead == 0 || (onCut && lead > 0 && lead < settings.snap_frames),
              "C20: page " + std::to_string(page) +
                  " opens on its first word or on the cut just before it");
        const RationalTime closes = carton.timeline_in.add(carton.duration);
        const RationalTime next =
            page + 1 < cartons.size() ? cartons[page + 1].timeline_in : end;
        Check(closes <= next, "C20: page " + std::to_string(page) +
                                  " ends before the next page or the film");
        for (size_t word = first[page]; word < first[page + 1]; ++word) {
            const int64_t from = words[word].timeline_in.to_frames(25, 1);
            const int64_t to = words[word]
                                   .timeline_in.add(words[word].duration)
                                   .to_frames(25, 1);
            bool covered = true;
            for (int64_t frame = from; frame < to; ++frame)
                covered =
                    covered && onScreen({frame, 25}) == static_cast<int>(page);
            Check(covered,
                  "C20: \"" + words[word].text + "\" is on screen, on page " +
                      std::to_string(page) + ", for every frame it is said");
        }
    }

    // The track: one edit, one clip per page, undone byte for byte.
    document.sequence.caption_styles = {style};
    const std::string before = document.SaveToString();
    AddTrackOperation track = BuildCaptionTrackEdit(cartons, style.id, 2);
    EditLog log;
    EditError editError = EditError::None;
    const bool applied = log.Apply(document, track, editError, error);
    Check(applied, "C20: the caption track is added: " + error);
    const DocumentTrack* captions = nullptr;
    for (const DocumentTrack& candidate : document.sequence.tracks)
        if (candidate.kind == "caption") captions = &candidate;
    bool same = captions != nullptr && captions->clips.size() == cartons.size();
    for (size_t page = 0; same && page < cartons.size(); ++page)
        same = captions->clips[page].caption_text == cartons[page].text &&
               captions->clips[page].caption_group_id == style.id &&
               captions->clips[page].timeline_in == cartons[page].timeline_in &&
               captions->clips[page].duration == cartons[page].duration;
    Check(same, "C20: the track holds the pages as generated");
    Check(applied && log.Undo(document, editError, error) &&
              document.SaveToString() == before,
          "C20: the caption track undoes byte for byte: " + error);

    // Named refusals: nothing said, and a shot whose speed would place its
    // words by a second derivation of the same mapping.
    std::vector<GeneratedCarton> none;
    Check(!GenerateCaptionCartons(document, style, {}, settings, none, error) &&
              error == "the montage has no words to caption",
          "C20: a montage without words is refused by name: " + error);
    Document slowed = document;
    slowed.sequence.tracks[0].clips[1].speed = {1, 2};
    slowed.sequence.tracks[0].clips[1].duration = {200, 25};
    Check(
        !GenerateCaptionCartons(slowed, style, words, settings, none, error) &&
            error.find("generate_captions") != std::string::npos,
        "C20: a shot not at 1/1 is refused by name: " + error);
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    // LG2, LG3 -- the cartons are measured and drawn by Core Text.
    InstallCaptionRenderer();
    std::string error;
    DocumentSequence vertical;
    vertical.width = 1080;
    vertical.height = 1920;
    CaptionStyle style;
    style.font_size = 58;

    // Visual QA of 19 September 2026 -- a line wider than the frame is
    // broken at spaces for delivery, as the monitor breaks it.
    {
        Check(CaptionMaximumLineWidth(1080) == 972 &&
                  CaptionMaximumLineWidth(0) == 0,
              "captions are broken at nine tenths of the sequence width");
        const auto byLetters = [](const std::string& line) {
            return static_cast<int64_t>(line.size()) * 10;
        };
        Check(WrapCaptionLines("court", 100, byLetters) == "court",
              "a line that fits is kept");
        Check(WrapCaptionLines("un deux trois quatre", 100, byLetters) ==
                  "un deux\ntrois\nquatre",
              "a long line is broken greedily at spaces: " +
                  WrapCaptionLines("un deux trois quatre", 100, byLetters));
        Check(WrapCaptionLines("a b\nun deux trois quatre", 100, byLetters) ==
                  "a b\nun deux\ntrois\nquatre",
              "existing line breaks are kept");
        Check(WrapCaptionLines("anticonstitutionnellement oui", 100,
                               byLetters) == "anticonstitutionnellement\noui",
              "a word wider than the limit keeps a line to itself");

        // LG2 -- measured by the caption renderer: the QA's carton, 64 px
        // Arial with an outline, on a 1080-wide vertical frame.
        CaptionStyle qa;
        qa.font_family = "Arial";
        qa.font_size = 64;
        qa.outline_width = 4;
        qa.shadow_offset = 4;
        std::vector<std::string> texts = {
            "Premier carton",
            "Un carton plus long pour voir le retour à la ligne automatique"};
        Check(WrapCaptionTextsForDelivery({qa, qa}, vertical, texts, error),
              "the delivered texts are prepared: " + error);
        Check(texts[0] == "Premier carton",
              "a short carton is delivered as written");
        Check(texts[1].find('\n') != std::string::npos,
              "the long carton is broken: " + texts[1]);
        std::vector<std::string> lines;
        for (size_t start = 0; start <= texts[1].size();) {
            const size_t end =
                std::min(texts[1].find('\n', start), texts[1].size());
            lines.push_back(texts[1].substr(start, end - start));
            start = end + 1;
        }
        std::vector<int64_t> lineWidths;
        Check(
            MeasureCaptionTextWidths(qa, vertical, lines, lineWidths, error) &&
                std::all_of(lineWidths.begin(), lineWidths.end(),
                            [](int64_t width) { return width <= 972; }),
            "every delivered line fits the frame: " + texts[1]);
    }

    {
        std::vector<int64_t> widths;
        Check(MeasureCaptionTextWidths(style, vertical,
                                       {"iiii", "WWWW", "WWWW WWWW"}, widths,
                                       error) &&
                  widths.size() == 3 && widths[0] > 0 &&
                  widths[1] > 2 * widths[0] && widths[2] > 2 * widths[1],
              "measured widths follow the glyphs: " + error);
        CaptionStyle outlined = style;
        outlined.outline_width = 6;
        std::vector<int64_t> outlinedWidths;
        Check(
            MeasureCaptionTextWidths(outlined, vertical, {"WWWW"},
                                     outlinedWidths, error) &&
                outlinedWidths[0] == widths[1] + 12,
            "the outline adds its width on both sides: " +
                std::to_string(widths[1]) + " -> " +
                std::to_string(outlinedWidths.empty() ? 0 : outlinedWidths[0]));
        // LG2 -- a family name is no longer filter syntax: one Core Text
        // lacks is measured in the fallback it is drawn in, never refused.
        CaptionStyle unknown = style;
        unknown.font_family = "Sans':x=0";
        CaptionStyle fallback = style;
        fallback.font_family = "Helvetica Neue";
        std::vector<int64_t> unknownWidths;
        std::vector<int64_t> fallbackWidths;
        Check(MeasureCaptionTextWidths(unknown, vertical, {"carton"},
                                       unknownWidths, error) &&
                  MeasureCaptionTextWidths(fallback, vertical, {"carton"},
                                           fallbackWidths, error) &&
                  unknownWidths == fallbackWidths,
              "a missing family measures as its fallback: " + error);
    }

    Document document;
    document.sequence = vertical;
    const Ulid source = "01K4CAP0000000000000000001";
    const Ulid other = "01K4CAP0000000000000000002";
    document.sources = {{source, "a.mov", {25, 1}, {1000, 25}},
                        {other, "b.mov", {25, 1}, {1000, 25}}};
    DocumentTrack picture{"01K4CAP0000000000000000003", "video", 0, {}};
    // [0,50) and [50,100) play the same frames on: no cut. [100,150) jumps
    // within the rush, [150,300) is another rush.
    picture.clips = {{GenerateUlid(), source, {0, 25}, {50, 25}, {0, 25}},
                     {GenerateUlid(), source, {50, 25}, {50, 25}, {50, 25}},
                     {GenerateUlid(), source, {300, 25}, {50, 25}, {100, 25}},
                     {GenerateUlid(), other, {0, 25}, {150, 25}, {150, 25}}};
    document.sequence.tracks = {picture};
    {
        const std::vector<int64_t> cuts = VisibleCutFrames(document);
        Check(cuts == std::vector<int64_t>({100, 150, 300}),
              "a contiguous split is not a cut; a jump and a new rush are");
    }

    {
        // A sentence end breaks, and a carton edge five frames after the cut
        // at 100 moves onto it.
        const std::vector<TimelineTranscriptSpan> words = {
            Word("Donc", 10, 10),     Word("voilà.", 22, 12),
            Word("Le", 105, 6),       Word("thème", 112, 10),
            Word("c'était", 123, 10), Word("simple.", 134, 12),
        };
        std::vector<GeneratedCarton> cartons;
        const bool generated =
            GenerateCaptionCartons(document, style, words, {}, cartons, error);
        std::string listed;
        for (const GeneratedCarton& carton : cartons)
            listed += " [" + carton.text + "]";
        Check(
            generated && cartons.size() >= 2 &&
                cartons[0].text == "Donc voilà." &&
                listed ==
                    " [Donc voilà.]" +
                        listed.substr(std::string(" [Donc voilà.]").size()) &&
                listed.find("simple.]") != std::string::npos,
            "a sentence end breaks, words keep their order: " + error + listed);
        if (cartons.size() >= 2) {
            Check(cartons[1].start_snapped &&
                      cartons[1].timeline_in == RationalTime{100, 25} &&
                      cartons[1].start_cut_distance == 0,
                  "an edge five frames from a cut moves onto it");
            Check(!cartons[0].start_snapped &&
                      cartons[0].start_cut_distance == 90 &&
                      cartons[0].rendered_width > 0 &&
                      cartons[0].characters == 11,
                  "a far edge stays, and every carton is measured");
        }
        // PA4 -- docs/roadmap/PARITE-2026-11.md. Every carton keeps when
        // each of its words starts, counted from its own start.
        bool timed = generated;
        size_t word = 0;
        for (const GeneratedCarton& carton : cartons) {
            timed = timed &&
                    carton.word_times.size() == CaptionWordCount(carton.text);
            for (const RationalTime& time : carton.word_times) {
                if (word >= words.size()) {
                    timed = false;
                    break;
                }
                const RationalTime said = carton.timeline_in.add(time);
                timed = timed && said == words[word].timeline_in &&
                        ReducedCaptionWordTime(time).rate == time.rate;
                ++word;
            }
        }
        Check(timed && word == words.size(),
              "each word of each carton is timed where the montage says it");
        if (cartons.size() >= 2) {
            Check(cartons[1].word_times[0].value > 0,
                  "a carton snapped back onto a cut opens before its first "
                  "word");
            const AddTrackOperation edit =
                BuildCaptionTrackEdit(cartons, style.id, 1);
            Check(edit.clips.size() == cartons.size() &&
                      edit.clips[1].caption_word_times.size() ==
                          cartons[1].word_times.size() &&
                      edit.clips[1].caption_word_times[0] ==
                          cartons[1].word_times[0],
                  "the track's clips carry the times");
        }
    }

    {
        // Nine words that do not fit on 400 px become several one-line
        // cartons, each within the limit.
        std::vector<TimelineTranscriptSpan> words;
        const std::vector<std::string> texts = {
            "une",      "phrase",    "assez", "longue", "pour",
            "déborder", "largement", "de",    "la"};
        for (size_t index = 0; index < texts.size(); ++index)
            words.push_back(
                Word(texts[index], 160 + static_cast<int64_t>(index) * 8, 7));
        words.push_back(Word("ligne.", 232, 10));
        CaptionGenerationSettings narrow;
        narrow.maximum_line_width = 400;
        std::vector<GeneratedCarton> cartons;
        Check(GenerateCaptionCartons(document, style, words, narrow, cartons,
                                     error) &&
                  cartons.size() >= 2,
              "a line wider than the limit is split: " + error);
        bool fits = !cartons.empty();
        for (const GeneratedCarton& carton : cartons)
            fits = fits && carton.rendered_width <= 400;
        Check(fits, "every carton fits the rendered width limit");
    }

    // AGENT-2026-09 A32 -- a carton under 0.8 s grows into the silence
    // after it; a hole under a breath between two cartons closes.
    {
        // Away from the cuts at 100, 150 and 300, so nothing snaps.
        const std::vector<TimelineTranscriptSpan> words = {
            Word("Là,", 400, 4), Word("on", 440, 30),
            Word("continue.", 471, 30), Word("Que", 505, 20)};
        std::vector<GeneratedCarton> cartons;
        std::string listed;
        const bool generated =
            GenerateCaptionCartons(document, style, words, {}, cartons, error);
        for (const GeneratedCarton& carton : cartons)
            listed += " [" + carton.text + " " +
                      std::to_string(carton.timeline_in.value) + "+" +
                      std::to_string(carton.duration.value) + "]";
        Check(generated && cartons.size() == 3 && cartons[0].text == "Là," &&
                  cartons[0].duration == RationalTime{20, 25},
              "a four-frame carton grows into the silence to 0.8 s: " + error +
                  listed);
        Check(cartons.size() == 3 &&
                  cartons[1].timeline_in.add(cartons[1].duration) ==
                      cartons[2].timeline_in,
              "a four-frame hole before the next carton closes: " + listed);
    }

    // RM2 -- docs/roadmap/REGLES-2026-10.md. On an interview series (the
    // words here are made up), "les marins." lasted 16 frames, joined the
    // next carton across its own full stop and gave "les marins. ensuite";
    // and "ensuite", which the montage put after a sentence from elsewhere,
    // kept the transcript's lower case.
    {
        const std::vector<TimelineTranscriptSpan> words = {
            Word("où", 401, 6),         Word("dorment", 407, 13),
            Word("les", 420, 5),        Word("marins.", 425, 11),
            Word("ensuite", 437, 6),    Word("notre", 453, 8),
            Word("capitaine,", 461, 13)};
        CaptionGenerationSettings twenty;
        twenty.maximum_characters = 20;
        std::vector<GeneratedCarton> cartons;
        std::string listed;
        const bool generated = GenerateCaptionCartons(document, style, words,
                                                      twenty, cartons, error);
        bool crosses = false;
        for (const GeneratedCarton& carton : cartons) {
            listed += " [" + carton.text + "]";
            crosses = crosses || carton.text.find(". ") != std::string::npos;
        }
        Check(generated && !crosses,
              "a short carton never joins across a sentence end: " + error +
                  listed);
        Check(listed.find(" [Ensuite") != std::string::npos,
              "a carton after a sentence end opens on a capital: " + listed);
    }
    {
        // The film's first carton opens on a capital too, an accented one
        // included; an ellipsis breaks the carton but is no sentence end for
        // the capital.
        const std::vector<TimelineTranscriptSpan> words = {
            Word("était", 400, 10),     Word("la", 411, 6),
            Word("maison...", 418, 30), Word("du", 470, 6),
            Word("coup.", 477, 15),     Word("on", 520, 8),
            Word("verra.", 529, 20)};
        std::vector<GeneratedCarton> cartons;
        std::string listed;
        const bool generated =
            GenerateCaptionCartons(document, style, words, {}, cartons, error);
        for (const GeneratedCarton& carton : cartons)
            listed += " [" + carton.text + "]";
        Check(generated && listed.rfind(" [Était", 0) == 0 &&
                  listed.find(" [du coup.]") != std::string::npos &&
                  listed.find(" [On verra.]") != std::string::npos,
              "the first carton and each sentence start take a capital, not "
              "what follows an ellipsis: " +
                  error + listed);
    }

    {
        // A short carton between two cuts is not squeezed onto them.
        const std::vector<TimelineTranscriptSpan> words = {
            Word("Court.", 140, 20)};
        std::vector<GeneratedCarton> cartons;
        Check(GenerateCaptionCartons(document, style, words, {}, cartons,
                                     error) &&
                  cartons.size() == 1 && !cartons[0].start_snapped &&
                  cartons[0].timeline_in == RationalTime{140, 25},
              "a snap that would cut a fifth of a carton is refused");
    }

    // DEC18 -- docs/roadmap/BENCH-2026-09.md. A snap never uncovers a word.
    // "Bonjour" starts four frames before the cut at 100, and "continue."
    // ends four frames after the cut at 150: both edges used to move onto
    // the cut, and the word was said without its carton. Every word of the
    // montage is now on screen for every frame it is said, the nearby cut
    // is still reported, and no gap between cartons holds a carton's word.
    {
        const std::vector<TimelineTranscriptSpan> words = {
            Word("Bonjour", 96, 12), Word("à", 109, 3), Word("tous.", 113, 11),
            Word("On", 128, 3), Word("continue.", 132, 22)};
        std::vector<GeneratedCarton> cartons;
        std::string listed;
        const bool generated =
            GenerateCaptionCartons(document, style, words, {}, cartons, error);
        for (const GeneratedCarton& carton : cartons)
            listed += " [" + carton.text + " " +
                      std::to_string(carton.timeline_in.value) + "+" +
                      std::to_string(carton.duration.value) + "]";
        Check(generated && !cartons.empty(),
              "DEC18: the cartons are generated: " + error);
        if (!cartons.empty()) {
            const GeneratedCarton& first = cartons.front();
            const GeneratedCarton& last = cartons.back();
            Check(first.timeline_in == RationalTime{96, 25} &&
                      !first.start_snapped && first.start_cut_distance == 4,
                  "DEC18: a start that would open after its first word stays "
                  "on it, the cut four frames away reported: " +
                      listed);
            Check(
                last.timeline_in.add(last.duration) == RationalTime{154, 25} &&
                    !last.end_snapped && last.end_cut_distance == 4,
                "DEC18: an end that would close before its last word ends "
                "stays on it: " +
                    listed);
        }
        bool covered = generated;
        for (const TimelineTranscriptSpan& word : words) {
            const int64_t from = word.timeline_in.to_frames(25, 1);
            const int64_t to =
                word.timeline_in.add(word.duration).to_frames(25, 1);
            for (int64_t frame = from; frame < to; ++frame) {
                const RationalTime at{frame, 25};
                bool shown = false;
                for (const GeneratedCarton& carton : cartons)
                    shown = shown ||
                            (carton.timeline_in <= at &&
                             at < carton.timeline_in.add(carton.duration) &&
                             carton.text.find(word.text) != std::string::npos);
                covered = covered && shown;
            }
        }
        Check(covered,
              "DEC18: every word is on screen, in its carton, for every frame "
              "it is said: " +
                  listed);
        std::vector<std::pair<RationalTime, RationalTime>> shown;
        for (const GeneratedCarton& carton : cartons)
            shown.emplace_back(carton.timeline_in, carton.duration);
        Check(FindCaptionSpeechGaps(shown, TimelineWordCheck{}).empty(),
              "DEC18: no gap between the cartons while one of their words is "
              "said");
    }
    {
        // A nearer cut inside the first word gives way to a farther one
        // before it. Cuts at 100 and 108; "Alors" starts at 105, three
        // frames before the cut at 108 and five after the one at 100.
        Document twoCuts = document;
        twoCuts.sequence.tracks[0].clips = {
            {GenerateUlid(), source, {0, 25}, {100, 25}, {0, 25}},
            {GenerateUlid(), other, {0, 25}, {8, 25}, {100, 25}},
            {GenerateUlid(), source, {500, 25}, {192, 25}, {108, 25}}};
        Check(
            VisibleCutFrames(twoCuts) == std::vector<int64_t>({100, 108, 300}),
            "DEC18: the fixture has cuts at 100, 108 and its end");
        const std::vector<TimelineTranscriptSpan> words = {
            Word("Alors", 105, 12), Word("voilà.", 118, 12)};
        std::vector<GeneratedCarton> cartons;
        Check(
            GenerateCaptionCartons(twoCuts, style, words, {}, cartons, error) &&
                cartons.size() == 1 && cartons[0].start_snapped &&
                cartons[0].timeline_in == RationalTime{100, 25} &&
                cartons[0].start_cut_distance == 0,
            "DEC18: the start moves onto the cut before its first word, "
            "not onto the nearer one inside it: " +
                error);
    }

    // DEC4 -- docs/roadmap/REGLES-2026-10.md. A burned-in carton is one line
    // of about 20 characters by default; a subtitle file keeps 42.
    {
        Check(CaptionGenerationSettings{}.maximum_characters == 20 &&
                  SubtitleSegmentationLimits{}.maximum_characters == 42,
              "DEC4: cartons default to 20 characters, subtitle files keep 42");
        const std::vector<std::string> texts = {
            "Les",   "couleurs", "sont", "mélangées", "dans",
            "l'eau", "de",       "la",   "rivière."};
        std::vector<TimelineTranscriptSpan> words;
        for (size_t index = 0; index < texts.size(); ++index)
            words.push_back(
                Word(texts[index], 400 + static_cast<int64_t>(index) * 8, 7));
        std::vector<GeneratedCarton> byDefault;
        std::string listed;
        const bool generated = GenerateCaptionCartons(document, style, words,
                                                      {}, byDefault, error);
        size_t longest = 0;
        for (const GeneratedCarton& carton : byDefault) {
            listed += " [" + carton.text + "]";
            longest = std::max(longest, carton.characters);
        }
        Check(generated && byDefault.size() >= 3 && longest <= 20,
              "DEC4: by default no carton passes 20 characters: " + error +
                  listed);
        CaptionGenerationSettings former;
        former.maximum_characters = 42;
        former.maximum_line_width = 1000;
        std::vector<GeneratedCarton> wide;
        size_t widest = 0;
        Check(
            GenerateCaptionCartons(document, style, words, former, wide, error),
            "DEC4: the former limits still generate: " + error);
        for (const GeneratedCarton& carton : wide)
            widest = std::max(widest, carton.characters);
        Check(widest > 20 && wide.size() < byDefault.size(),
              "DEC4: 42 characters, asked for, still group more words: " +
                  std::to_string(widest));
    }

    {
        std::vector<GeneratedCarton> cartons(1);
        cartons[0].text = "Bonjour";
        cartons[0].timeline_in = {100, 25};
        cartons[0].duration = {20, 25};
        const Ulid styleId = "01K4CAP0000000000000000009";
        document.sequence.caption_styles = {style};
        document.sequence.caption_styles[0].id = styleId;
        AddTrackOperation track = BuildCaptionTrackEdit(cartons, styleId, 4);
        Check(track.kind == "caption" && track.sync_lock &&
                  track.clips.size() == 1 &&
                  track.clips[0].caption_group_id == styleId &&
                  track.clips[0].caption_text == "Bonjour",
              "the cartons land on a sync-locked track in the style's run");
        const std::string before = document.SaveToString();
        EditLog log;
        EditError editError = EditError::None;
        Check(log.Apply(document, track, editError, error) &&
                  log.Undo(document, editError, error) &&
                  document.SaveToString() == before,
              "adding the caption track undoes byte-exactly: " + error);
    }

    {
        // A white carton over a white picture, then outlined: the check reads
        // the delivered pixels around the letters.
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() /
            ("cutmachine-legibility-tests-" + GenerateUlid());
        std::filesystem::create_directories(directory);
        const auto white = directory / "white.mp4";
        const std::string generate =
            std::string("'") + FFMPEG_EXECUTABLE +
            "' -hide_banner -loglevel error -f lavfi -i "
            "'color=c=white:size=320x180:rate=25:duration=1' -c:v libx264 "
            "-pix_fmt yuv420p -y '" +
            white.string() + "'";
        Check(std::system(generate.c_str()) == 0, "white source is generated");
        const auto legibility = [&](int32_t outline, const std::string& name,
                                    std::vector<CaptionLegibility>& results) {
            Document shirt;
            shirt.sequence.width = 320;
            shirt.sequence.height = 180;
            shirt.sources = {
                {GenerateUlid(), white.string(), {25, 1}, {25, 25}}};
            DocumentClip picture;
            picture.source_id = shirt.sources[0].id;
            picture.source_in = {0, 25};
            picture.duration = {20, 25};
            picture.timeline_in = {0, 25};
            CaptionStyle carton;
            carton.font_size = 36;
            carton.outline_width = outline;
            DocumentClip text;
            text.timeline_in = {5, 25};
            text.duration = {10, 25};
            text.include_audio = false;
            text.caption_group_id = carton.id;
            text.caption_text = "LISIBLE";
            shirt.sequence.caption_styles = {carton};
            shirt.sequence.tracks = {{GenerateUlid(), "video", 0, {picture}},
                                     {GenerateUlid(), "caption", 1, {text}}};
            ExportSettings settings =
                Exporter::H264Preset((directory / (name + ".mp4")).string());
            settings.encoder = ExportEncoder::H264Software;
            settings.width = 320;
            settings.height = 180;
            settings.frame_rate = {25, 1};
            settings.ffmpeg_path = FFMPEG_EXECUTABLE;
            ExportPlan plan;
            std::string failure;
            if (!Exporter::BuildPlan(shirt, (directory / "p").string(),
                                     settings, plan, failure) ||
                !Exporter::Run(plan, {}, nullptr, failure)) {
                Check(false, name + " export: " + failure);
                return false;
            }
            return CheckCaptionLegibility(shirt, settings.output_path,
                                          FFMPEG_EXECUTABLE, results,
                                          failure) ||
                   (Check(false, name + " check: " + failure), false);
        };
        std::vector<CaptionLegibility> plain;
        std::vector<CaptionLegibility> outlined;
        Check(legibility(0, "plain", plain) && plain.size() == 1 &&
                  plain[0].frame == 10 && plain[0].ring_pixels > 50 &&
                  plain[0].contrast_median < 1.5,
              "white on white measures about 1:1");
        Check(legibility(3, "outlined", outlined) && outlined.size() == 1 &&
                  outlined[0].contrast_median > 10.0,
              "an outline makes the same carton legible: " +
                  std::to_string(
                      outlined.empty() ? 0.0 : outlined[0].contrast_median));
        const std::string json = SerializeCaptionLegibility(plain, 3.0);
        Check(json.find("\"below_minimum\":1") != std::string::npos,
              "the illegible carton is counted: " + json);
        std::filesystem::remove_all(directory);
    }

    CheckTranscriptPagination();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All caption generation tests passed\n";
    return 0;
}
