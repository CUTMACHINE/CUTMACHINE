#include "EditLog.h"
#include "Subtitles.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <tuple>
#include <variant>

#include "TestSupport.h"

namespace {

Document Fixture() {
    Document document;
    document.sources = {
        {"01KT0000000000000000000001", "source.mov", {25, 1}, {250, 25}},
    };
    DocumentTrack video;
    video.id = "01KT0000000000000000000002";
    video.kind = "video";
    video.index = 0;
    DocumentClip clip;
    clip.id = "01KT0000000000000000000003";
    clip.source_id = document.sources[0].id;
    clip.source_in = {0, 25};
    clip.duration = {250, 25};
    clip.timeline_in = {0, 25};
    video.clips.push_back(clip);
    document.sequence.tracks.push_back(video);
    return document;
}

}  // namespace

int main() {
    const std::string srt =
        "\xEF\xBB\xBF"
        "1\r\n00:00:01,250 --> 00:00:02,500\r\n"
        "Bonjour\r\nle monde\r\n\r\n"
        "2\r\n00:00:03.000 --> 00:00:04,125\r\nDeuxième\r\n";
    std::vector<SubtitleCue> cues;
    std::string error;
    Check(ParseSrt(srt, cues, error), "valid SRT parses: " + error);
    Check(cues.size() == 2 && cues[0].timeline_in == RationalTime{1250, 1000} &&
              cues[0].duration == RationalTime{1250, 1000} &&
              cues[0].text == "Bonjour\nle monde" &&
              cues[1].duration == RationalTime{1125, 1000},
          "SRT timestamps and multiline text stay exact");

    // RM2 -- docs/roadmap/REGLES-2026-10.md.
    Check(SubtitleEndsSentence("coup.") && SubtitleEndsSentence("vrai ?") &&
              SubtitleEndsSentence("non!") && !SubtitleEndsSentence("donc,") &&
              !SubtitleEndsSentence(""),
          "a full stop, a question and an exclamation end a sentence");
    Check(SubtitleOpensSentenceAfter("") &&
              SubtitleOpensSentenceAfter("fin.") &&
              !SubtitleOpensSentenceAfter("maison...") &&
              !SubtitleOpensSentenceAfter("alors,"),
          "a sentence opens at the start and after an end, not an ellipsis");
    Check(SubtitleWithCapital("pendant") == "Pendant" &&
              SubtitleWithCapital("était") == "Était" &&
              SubtitleWithCapital("à") == "À" &&
              SubtitleWithCapital("ça") == "Ça" &&
              SubtitleWithCapital("œuvre") == "Œuvre" &&
              SubtitleWithCapital("«\u00A0oui") == "«\u00A0Oui" &&
              SubtitleWithCapital("(alors") == "(Alors" &&
              SubtitleWithCapital("2D") == "2D" &&
              SubtitleWithCapital("Déjà") == "Déjà" &&
              SubtitleWithCapital("").empty(),
          "the first letter takes its capital, accents and œ included");

    Document document = Fixture();
    const std::string before = document.SaveToString();
    EditLog log;
    EditError editError = EditError::None;
    std::string message;
    AddTrackOperation add =
        BuildSubtitleTrackEdit(cues, 1, "01KT0000000000000000000004");
    Check(log.Apply(document, Operation{add}, editError, message),
          "subtitle track operation applies: " + message);
    const DocumentTrack* track =
        document.FindTrack("01KT0000000000000000000004");
    Check(track && track->kind == "caption" && track->clips.size() == 2 &&
              track->clips[0].source_id.empty(),
          "SRT becomes a dedicated source-free caption track");
    const std::string serialized =
        SerializeOperation(log.AppliedEntries().back().op);
    Operation parsed = RemoveTrackOperation{};
    Check(DeserializeOperation(serialized, parsed, editError, message) &&
              SerializeOperation(parsed) == serialized,
          "caption AddTrack operation serializes canonically");
    Document loaded;
    Check(Document::LoadFromString(document.SaveToString(), loaded, error) &&
              loaded.SaveToString() == document.SaveToString(),
          "caption track document round-trips byte-identically: " + error);
    Check(ActiveSubtitles(document, {1500, 1000}).size() == 1 &&
              ActiveSubtitles(document, {2750, 1000}).empty(),
          "monitor lookup follows exact half-open cue ranges");

    Transcript transcript;
    transcript.media_id = document.sources[0].id;
    transcript.source_rate = {25, 1};
    transcript.words = {
        {"hors", {25, 25}, {40, 25}},     {"Bonjour", {50, 25}, {60, 25}},
        {",", {60, 25}, {61, 25}},        {"le", {62, 25}, {66, 25}},
        {"monde.", {67, 25}, {75, 25}},   {"Deuxième", {100, 25}, {112, 25}},
        {"phrase", {113, 25}, {125, 25}},
    };
    DocumentClip transcriptClip = document.sequence.tracks[0].clips[0];
    transcriptClip.source_in = {50, 25};
    transcriptClip.timeline_in = {250, 25};
    transcriptClip.duration = {75, 25};
    std::vector<SubtitleCue> generated;
    Check(SubtitleCuesForClip(transcript, transcriptClip, generated, error),
          "transcript maps into subtitle cues: " + error);
    Check(generated.size() == 2 && generated[0].text == "Bonjour, le monde." &&
              generated[0].timeline_in == RationalTime{250, 25} &&
              generated[1].text == "Deuxième phrase" &&
              generated[1].timeline_in == RationalTime{300, 25} &&
              generated[1].duration == RationalTime{25, 25},
          "word grouping is deterministic and clip-relative");

    Transcript syntaxTranscript;
    syntaxTranscript.media_id = document.sources[0].id;
    syntaxTranscript.source_rate = {25, 1};
    syntaxTranscript.words = {
        {"sous", {0, 25}, {4, 25}},     {"les", {5, 25}, {7, 25}},
        {"grandes", {8, 25}, {13, 25}}, {"arches", {14, 25}, {20, 25}},
        {"mais", {21, 25}, {23, 25}},   {"tu", {24, 25}, {26, 25}},
        {"peux", {27, 25}, {30, 25}},   {"passer.", {31, 25}, {40, 25}},
    };
    DocumentClip syntaxClip = document.sequence.tracks[0].clips[0];
    syntaxClip.duration = {100, 25};
    std::vector<SubtitleCue> syntaxCues;
    Check(SubtitleCuesForClip(syntaxTranscript, syntaxClip, syntaxCues, error),
          "syntax-aware cues build: " + error);
    Check(syntaxCues.size() == 2 &&
              syntaxCues[0].text == "sous les grandes arches" &&
              syntaxCues[1].text == "mais tu peux passer." &&
              syntaxCues[0].duration == RationalTime{21, 25},
          "the global grouping keeps French syntactic units together");

    Transcript pauseTranscript;
    pauseTranscript.media_id = document.sources[0].id;
    pauseTranscript.source_rate = {25, 1};
    pauseTranscript.words = {
        {"premier", {0, 25}, {5, 25}},
        {"bloc", {6, 25}, {10, 25}},
        {"deuxième", {30, 25}, {36, 25}},
        {"bloc.", {37, 25}, {42, 25}},
    };
    std::vector<SubtitleCue> pauseCues;
    Check(SubtitleCuesForClip(pauseTranscript, syntaxClip, pauseCues, error),
          "pause-aware cues build: " + error);
    Check(pauseCues.size() == 2 && pauseCues[0].text == "premier bloc" &&
              pauseCues[0].duration == RationalTime{10, 25} &&
              pauseCues[1].text == "deuxième bloc.",
          "a long measured pause splits cues and is not captioned");

    Transcript fastTranscript;
    fastTranscript.media_id = document.sources[0].id;
    fastTranscript.source_rate = {25, 1};
    fastTranscript.words = {
        {"phrase", {0, 25}, {3, 25}},
        {"très", {4, 25}, {5, 25}},
        {"rapide.", {6, 25}, {10, 25}},
    };
    std::vector<SubtitleCue> fastCues;
    Check(SubtitleCuesForClip(fastTranscript, syntaxClip, fastCues, error),
          "fast cues build: " + error);
    Check(
        kSubtitleTargetCharactersPerSecond == 18 && fastCues.size() == 1 &&
            fastCues[0].text == "phrase très rapide." &&
            19 * fastCues[0].duration.rate >
                kSubtitleTargetCharactersPerSecond * fastCues[0].duration.value,
        "18 CPS is an optimization target, not a per-cue maximum");

    Transcript utf8Transcript;
    utf8Transcript.media_id = document.sources[0].id;
    utf8Transcript.source_rate = {25, 1};
    for (int64_t index = 0; index < 10; ++index) {
        utf8Transcript.words.push_back({index == 9 ? "été." : "été",
                                        {index * 5, 25},
                                        {index * 5 + 4, 25}});
    }
    std::vector<SubtitleCue> utf8Cues;
    Check(SubtitleCuesForClip(utf8Transcript, syntaxClip, utf8Cues, error),
          "UTF-8 cues build: " + error);
    Check(utf8Cues.size() == 1,
          "French accented characters count as characters, not UTF-8 bytes");

    std::vector<SubtitleCue> wordByWord;
    Check(
        WordSubtitleCuesForClip(transcript, transcriptClip, wordByWord, error),
        "word review cues build: " + error);
    Check(wordByWord.size() == 6 && wordByWord[0].text == "Bonjour" &&
              wordByWord[0].timeline_in == RationalTime{250, 25} &&
              wordByWord[0].duration == RationalTime{10, 25} &&
              wordByWord[1].text == "," && wordByWord[5].text == "phrase",
          "word review mode keeps every stored boundary independent");

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("cutmachine-subtitles-" + GenerateUlid() + ".srt");
    Check(SaveSrt(document, output.string(), error),
          "caption track exports to SRT: " + error);
    std::vector<SubtitleCue> exported;
    Check(LoadSrt(output.string(), exported, error) &&
              exported.size() == cues.size() &&
              exported[0].timeline_in == cues[0].timeline_in &&
              exported[0].text == cues[0].text,
          "exported SRT imports with exact timing and text: " + error);
    std::error_code ignored;
    std::filesystem::remove(output, ignored);

    // WriteSrt sert le même sérialiseur à un appelant qui tient déjà ses
    // cues : --export-srt les tire des transcriptions en cache, sans poser
    // de piste de sous-titres dans le document juste pour obtenir un fichier.
    const std::filesystem::path direct =
        std::filesystem::temp_directory_path() /
        ("cutmachine-subtitles-" + GenerateUlid() + ".srt");
    Check(WriteSrt(generated, direct.string(), error),
          "des cues s'écrivent en SRT sans passer par le document : " + error);
    std::vector<SubtitleCue> roundTrip;
    Check(LoadSrt(direct.string(), roundTrip, error) &&
              roundTrip.size() == generated.size() &&
              roundTrip[0].timeline_in == generated[0].timeline_in &&
              roundTrip[0].text == generated[0].text &&
              roundTrip[1].text == generated[1].text,
          "l'aller-retour garde le minutage et le texte : " + error);
    std::filesystem::remove(direct, ignored);

    std::vector<SubtitleCue> nothing;
    Check(!WriteSrt(nothing, direct.string(), error),
          "une liste vide se refuse au lieu d'écrire un fichier vide");
    Check(!std::filesystem::exists(direct, ignored),
          "et rien n'est créé sur le disque");

    Check(log.Undo(document, editError, message) &&
              document.SaveToString() == before,
          "one undo removes the imported track byte-identically");

    std::vector<SubtitleCue> invalid;
    Check(!ParseSrt("1\n00:00:02,000 --> 00:00:01,000\nNon\n", invalid, error),
          "backwards SRT timing is rejected explicitly");

    // SRT-MS-2026-09 -- a frame at 24 fps rarely lands on a millisecond. The
    // writer used to refuse every such cue, so no 24 fps timeline or
    // delivery could be subtitled (--srt-from-media on a 24 fps export).
    const std::filesystem::path film =
        std::filesystem::temp_directory_path() /
        ("cutmachine-subtitles-" + GenerateUlid() + ".srt");
    const std::vector<SubtitleCue> at24 = {
        {{1, 24}, {2, 24}, "Un"},
        {{3, 24}, {21, 24}, "Deux"},
    };
    size_t rounded = 99;
    Check(WriteSrt(at24, film.string(), error, &rounded),
          "des cues à 24 i/s s'écrivent en SRT : " + error);
    std::ifstream filmFile(film, std::ios::binary);
    const std::string filmText((std::istreambuf_iterator<char>(filmFile)),
                               std::istreambuf_iterator<char>());
    filmFile.close();
    Check(filmText ==
              "1\n00:00:00,042 --> 00:00:00,125\nUn\n\n"
              "2\n00:00:00,125 --> 00:00:01,000\nDeux\n\n",
          "1/24 s s'écrit 042 ms, et une borne exacte n'est pas touchée");
    Check(rounded == 1, "seule la borne arrondie est comptée");

    bool wasRounded = false;
    Check(SrtMilliseconds({1, 2000}, &wasRounded) == 1 && wasRounded &&
              SrtMilliseconds({3, 2000}) == 2 &&
              SrtMilliseconds({1001, 30000}) == 33 &&
              SrtMilliseconds({3, 24}, &wasRounded) == 125 && !wasRounded,
          "la milliseconde la plus proche, une demie arrondie vers le haut");

    // The guarantee principle 4 asks for: over an hour of frames, the
    // rounded millisecond read back at the source rate is the same frame.
    const std::pair<int32_t, int32_t> rates[] = {
        {24, 1}, {25, 1}, {30000, 1001}, {50, 1}, {60000, 1001}};
    for (const auto& [num, den] : rates) {
        bool sameFrame = true;
        const RationalTime halfFrame{den, 2 * num};
        for (int64_t frame = 0; frame < 3600LL * num / den && sameFrame;
             ++frame) {
            const RationalTime time{frame * den, num};
            const RationalTime written{SrtMilliseconds(time), 1000};
            sameFrame = written.add(halfFrame).to_frames(num, den) == frame;
        }
        Check(sameFrame, "une heure à " + std::to_string(num) + "/" +
                             std::to_string(den) +
                             " i/s relue sur la même image, au ms près");
    }

    // A refused cue leaves an existing file exactly as it was: the writer
    // used to truncate it and write the cues before the refusal.
    {
        std::ofstream sentinel(film, std::ios::binary | std::ios::trunc);
        sentinel << "sentinelle";
    }
    const std::vector<SubtitleCue> collapsing = {
        {{0, 25}, {25, 25}, "Entier"},
        {{1, 24000}, {1, 24000}, "Éclair"},
    };
    Check(!WriteSrt(collapsing, film.string(), error, &rounded) &&
              error.find("subtitle 2") != std::string::npos && rounded == 0,
          "un sous-titre plus court qu'une milliseconde se refuse par son "
          "numéro");
    std::ifstream after(film, std::ios::binary);
    const std::string afterText((std::istreambuf_iterator<char>(after)),
                                std::istreambuf_iterator<char>());
    after.close();
    Check(afterText == "sentinelle",
          "le refus ne touche pas au fichier déjà là");
    std::filesystem::remove(film, ignored);

    // FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. WebVTT : les mêmes cues, la
    // même milliseconde, le même compte des bornes arrondies que le SRT.
    {
        std::string vtt;
        size_t vttRounded = 99;
        Check(FormatVtt(at24, vtt, error, &vttRounded),
              "des cues à 24 i/s s'écrivent en WebVTT : " + error);
        Check(vtt ==
                  "WEBVTT\n\n"
                  "1\n00:00:00.042 --> 00:00:00.125\nUn\n\n"
                  "2\n00:00:00.125 --> 00:00:01.000\nDeux\n\n",
              "le WebVTT est le SRT au point décimal près : " + vtt);
        Check(vttRounded == 1, "la même borne arrondie est comptée");
        const std::vector<SubtitleCue> marked = {
            {{3661, 1}, {1, 2}, "Tom & Léa <ici>\n--> deux lignes"}};
        Check(FormatVtt(marked, vtt, error, &vttRounded) &&
                  vtt ==
                      "WEBVTT\n\n1\n01:01:01.000 --> 01:01:01.500\n"
                      "Tom &amp; Léa &lt;ici&gt;\n--&gt; deux lignes\n\n" &&
                  vttRounded == 0,
              "le texte reste du texte, et les heures s'écrivent : " + vtt);
        const std::vector<SubtitleCue> blank = {
            {{0, 25}, {25, 25}, "Un\n\nDeux"}};
        Check(!FormatVtt(blank, vtt, error, &vttRounded) &&
                  error.find("empty line") != std::string::npos,
              "une ligne vide, qui finirait la cue, se refuse : " + error);
        Check(!FormatVtt(collapsing, vtt, error, &vttRounded) &&
                  error.find("subtitle 2") != std::string::npos,
              "le sous-titre sous la milliseconde se refuse comme en SRT");
        const std::filesystem::path web =
            std::filesystem::temp_directory_path() /
            ("cutmachine-subtitles-" + GenerateUlid() + ".vtt");
        Check(WriteVtt(at24, web.string(), error, &vttRounded) &&
                  vttRounded == 1 && std::filesystem::exists(web),
              "WriteVtt écrit le fichier : " + error);
        std::filesystem::remove(web, ignored);
    }

    // RM3 -- "au" and "aux" keep their noun, as the other articles do.
    Check(SubtitleRuleBoundaryCost("aux", "deux", RationalTime{4, 25}) >= 400 &&
              SubtitleRuleBoundaryCost("au", "cours", RationalTime{4, 25}) >=
                  400 &&
              (SubtitleWordFamilies("Aux") & kSubtitleDeterminer) != 0,
          "a carton never ends on a contracted article");

    // RM3 -- docs/roadmap/REGLES-2026-10.md. Cartons grouped by the caller:
    // the montage's words in order, timed by the segmentation's own rule,
    // the hard bounds refused by name.
    {
        const std::vector<std::tuple<const char*, int64_t, int64_t>> said = {
            {"Tu", 0, 200},         {"vas", 200, 400},
            {"vers", 400, 500},     {"la", 500, 900},
            {"plage,", 900, 1300},  {"l’eau", 1500, 1700},
            {"monte.", 1700, 2000}, {"Demain", 2900, 3200},
            {"il", 3200, 3300},     {"pleuvra.", 3300, 3700}};
        std::vector<SubtitleWord> words;
        for (const auto& [text, start, end] : said)
            words.push_back({text, {start, 1000}, {end, 1000}});
        SubtitleSegmentationLimits limits;
        limits.maximum_characters = 20;
        std::vector<SubtitleCue> grouped;
        std::vector<std::pair<size_t, size_t>> ranges;
        std::string error;
        Check(GroupSubtitleWords(words,
                                 {"tu vas", "vers la plage,", "L'eau monte.",
                                  "demain il pleuvra."},
                                 limits, grouped, &ranges, error),
              "given cartons group the montage's words: " + error);
        Check(grouped.size() == 4 && grouped[1].text == "vers la plage," &&
                  grouped[2].text == "l’eau monte." &&
                  ranges[3] == std::pair<size_t, size_t>{7, 10} &&
                  grouped[0].timeline_in == RationalTime{0, 1000} &&
                  grouped[0].duration == RationalTime{400, 1000} &&
                  grouped[2].duration == RationalTime{500, 1000},
              "the words keep their own text and the segmentation's times "
              "(the next carton's start, or the last word's end before a "
              "long pause)");
        const auto refusal = [&](const std::vector<std::string>& cartons) {
            std::vector<SubtitleCue> cues;
            std::string why;
            return GroupSubtitleWords(words, cartons, limits, cues, nullptr,
                                      why)
                       ? std::string()
                       : why;
        };
        Check(
            refusal({"tu vas", "vers la", "plage, l'eau monte.",
                     "demain il pleuvra."})
                        .find("where the montage says") == std::string::npos &&
                refusal({"tu va", "vers la plage,", "l'eau monte.",
                         "demain il pleuvra."})
                        .find("'va' where the montage says 'vas' (word 1)") !=
                    std::string::npos,
            "a word that differs is named, with its index");
        Check(refusal({"tu vas", "vers la plage,", "l'eau monte."})
                      .find("stop at word 7") != std::string::npos,
              "a word left out of every carton is refused");
        Check(refusal({"tu vas", "vers la plage,", "l'eau monte. demain",
                       "il pleuvra."})
                      .find("crosses a sentence end after 'monte.'") !=
                  std::string::npos,
              "a carton across a sentence end is refused");
        Check(refusal({"tu vas vers la plage,", "l'eau monte.",
                       "demain il pleuvra."})
                      .find("longer than 20") != std::string::npos,
              "a carton over the characters is refused");
    }

    if (failures) return 1;
    std::cout << "All subtitle tests passed\n";
    return 0;
}
