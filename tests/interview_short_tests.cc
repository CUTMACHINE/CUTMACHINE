#include "InterviewShort.h"
#include "Json.h"
#include "TimelineTranscription.h"
#include "Transcription.h"
#include "Ulid.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}  // namespace

int main() {
    Document document;
    const Ulid sourceId = "01K50000000000000000000001";
    document.sources = {{sourceId, "interview.mov", {25, 1}, {250, 25}}};
    document.sequence.tracks = {
        {"01K50000000000000000000002",
         "audio",
         0,
         {{"01K50000000000000000000003",
           sourceId,
           {50, 25},
           {100, 25},
           {25, 25}}}},
    };
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-interview-short-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    {
        std::ofstream cache(directory / (sourceId + ".json"));
        cache << "{\"version\":1,\"media_id\":\"" << sourceId
              << "\",\"whisper_model\":\"test.bin\","
                 "\"source_rate\":{\"num\":25,\"den\":1},"
                 "\"words\":[{\"text\":\"Une\",\"start\":{\"value\":"
                 "50,\"rate\":25},\"end\":{\"value\":60,\"rate\":25}},{"
                 "\"text\":\"accroche.\",\"start\":{\"value\":60,"
                 "\"rate\":25},\"end\":{\"value\":75,\"rate\":25}}]}";
    }
    std::string json;
    std::string error;
    Check(DescribeTimelineTranscriptForAgent(document, directory, json, error),
          "describe transcript: " + error);
    mcp_json::Value root;
    Check(mcp_json::Value::Parse(json, root, error),
          "agent transcript JSON parses: " + error);
    const mcp_json::Value* spans = root.Find("spans");
    Check(spans && spans->IsArray() && spans->AsArray().size() == 1,
          "words are grouped into one semantic span");
    const mcp_json::Value& span = spans->AsArray().front();
    int64_t sourceIn = -1;
    Check(span.Find("source_in") && span.Find("source_in")->Find("value") &&
              span.Find("source_in")->Find("value")->AsInt64(sourceIn) &&
              sourceIn == 50 && span.Find("text") &&
              span.Find("text")->AsString() == "Une accroche.",
          "span exposes source-exact boundaries and readable text");

    // ---- span ids resolve to exact ranges; the caller never adds times ----
    std::vector<TimelineTranscriptSpan> built;
    Check(BuildTimelineTranscriptSpans(document, directory, built, error),
          "spans build: " + error);
    Check(built.size() == 1 && built[0].span_id == "S1",
          "spans are numbered in timeline order");

    // B6 -- both clips overlap the word "milieu": the outgoing clip plays its
    // first two frames, the incoming clip the other four. It must be emitted
    // once and marked as cut. ITW3 -- on the outgoing side, which plays its
    // start: a subtitle shows a word when it begins to be heard, where the
    // former greatest-overlap rule showed it two frames late.
    Document splitDocument = document;
    splitDocument.sequence.tracks[0].clips = {
        {"01K50000000000000000000003", sourceId, {0, 25}, {6, 25}, {0, 25}},
        {"01K50000000000000000000004", sourceId, {6, 25}, {8, 25}, {6, 25}},
    };
    {
        std::ofstream cache(directory / (sourceId + ".json"), std::ios::trunc);
        cache << "{\"version\":1,\"media_id\":\"" << sourceId
              << "\",\"whisper_model\":\"test.bin\","
                 "\"source_rate\":{\"num\":25,\"den\":1},\"words\":["
                 "{\"text\":\"Avant\",\"start\":{\"value\":0,\"rate\":25},"
                 "\"end\":{\"value\":4,\"rate\":25}},"
                 "{\"text\":\"milieu\",\"start\":{\"value\":4,\"rate\":25},"
                 "\"end\":{\"value\":10,\"rate\":25}},"
                 "{\"text\":\"Après\",\"start\":{\"value\":10,\"rate\":25},"
                 "\"end\":{\"value\":14,\"rate\":25}},"
                 "{\"text\":\"Hors\",\"start\":{\"value\":20,\"rate\":25},"
                 "\"end\":{\"value\":22,\"rate\":25}}]}";
    }
    std::vector<TimelineTranscriptSpan> splitSpans;
    Check(BuildTimelineTranscriptSpans(splitDocument, directory, splitSpans,
                                       error),
          "a transcript split through a word builds: " + error);
    std::string splitText;
    size_t middleCount = 0;
    bool middleStraddles = false;
    for (const TimelineTranscriptSpan& item : splitSpans) {
        splitText += item.text + " ";
        if (item.text.find("milieu") != std::string::npos) {
            ++middleCount;
            middleStraddles = item.straddles_cut;
            Check(item.timeline_in < RationalTime{6, 25},
                  "the straddled word is attributed to the clip playing its "
                  "start");
        }
    }
    Check(middleCount == 1 && middleStraddles,
          "a word cut between two clips appears once with straddles_cut");
    Check(splitText.find("Hors") == std::string::npos,
          "a word outside every played source range is omitted");
    std::vector<TimelineTranscriptSpan> wordSpans;
    Check(BuildTimelineTranscriptWordSpans(splitDocument, directory, wordSpans,
                                           error),
          "word-by-word review spans build: " + error);
    Check(wordSpans.size() == 3 && wordSpans[0].text == "Avant" &&
              wordSpans[1].text == "milieu" && wordSpans[2].text == "Après" &&
              wordSpans[1].straddles_cut &&
              wordSpans[1].timeline_in == RationalTime{4, 25},
          "review spans keep one cue per played word and deduplicate cuts");
    const std::string splitJson =
        SerializeTimelineTranscriptSpans(splitDocument, splitSpans);
    std::vector<TimelineTranscriptSpan> splitReparsed;
    Check(ParseTimelineTranscriptSpans(splitJson, splitReparsed, error) &&
              std::any_of(splitReparsed.begin(), splitReparsed.end(),
                          [](const TimelineTranscriptSpan& item) {
                              return item.straddles_cut;
                          }),
          "straddles_cut survives the transcript view round trip");

    // Two contiguous spans of one source, and a third after a gap: the
    // shape that separates a legitimate merge from one that would swallow
    // a silence nobody asked to cut.
    const std::vector<TimelineTranscriptSpan> run = {
        {"S1", sourceId, {50, 25}, {25, 25}, {25, 25}, "Une accroche."},
        {"S2",
         sourceId,
         {75, 25},
         {30, 25},
         {50, 25},
         "Puis la suite.",
         false,
         true,
         true},
        {"S3", sourceId, {200, 25}, {20, 25}, {80, 25}, "Et la chute."},
        {"S4",
         "01K50000000000000000000009",
         {0, 25},
         {10, 25},
         {100, 25},
         "Autre source."},
    };
    Ulid resolvedSource;
    RationalTime resolvedIn;
    RationalTime resolvedDuration;

    Check(ResolveTranscriptSpanRange(run, "S1", "", resolvedSource, resolvedIn,
                                     resolvedDuration, error),
          "a single span resolves: " + error);
    Check(resolvedSource == sourceId && resolvedIn.compare({50, 25}) == 0 &&
              resolvedDuration.compare({25, 25}) == 0,
          "one span resolves to its own exact range");

    Check(ResolveTranscriptSpanRange(run, "S1", "S2", resolvedSource,
                                     resolvedIn, resolvedDuration, error),
          "a contiguous run resolves: " + error);
    Check(resolvedIn.compare({50, 25}) == 0 &&
              resolvedDuration.compare({55, 25}) == 0,
          "a contiguous run merges into one range spanning both spans");

    Check(!ResolveTranscriptSpanRange(run, "S2", "S3", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a run across a real editorial gap is refused");
    Check(error.find("breath") != std::string::npos,
          "the refusal names the reason and suggests separate segments");

    // PRD40 -- ROADMAP.md. Every break of a run in one refusal, with the
    // pieces that select it: the first break alone was named, and an editor
    // found the others one refusal at a time.
    {
        const std::vector<TimelineTranscriptSpan> broken = {
            {"S1", sourceId, {0, 25}, {25, 25}, {0, 25}, "Un."},
            {"S2", sourceId, {25, 25}, {25, 25}, {25, 25}, "Deux."},
            // A second of silence before S3, and another before S5.
            {"S3", sourceId, {75, 25}, {25, 25}, {50, 25}, "Trois."},
            {"S4", sourceId, {100, 25}, {25, 25}, {75, 25}, "Quatre."},
            {"S5", sourceId, {150, 25}, {25, 25}, {100, 25}, "Cinq."},
        };
        Check(
            !ResolveTranscriptSpanRange(broken, "S1", "S5", resolvedSource,
                                        resolvedIn, resolvedDuration, error) &&
                error.find("'S2' and 'S3'") != std::string::npos &&
                error.find("'S4' and 'S5'") != std::string::npos &&
                error.find("'S1' to 'S2', 'S3' to 'S4', 'S5'") !=
                    std::string::npos,
            "one refusal names both breaks and the three pieces: " + error);
    }

    // A cue breaks on a pause between words, and that pause belongs to
    // neither cue. Refusing those made ordinary sentences unselectable: on a
    // real interview, three of eight editorial beats were rejected over gaps
    // of one and two frames. Anything under the cue segmenter's own maximum
    // gap is one continuous utterance and merges, breath included.
    const std::vector<TimelineTranscriptSpan> breathing = {
        {"S1", sourceId, {50, 25}, {25, 25}, {25, 25}, "Une accroche"},
        // Two frames of silence, well under kSubtitleCueMaximumGap.
        {"S2", sourceId, {77, 25}, {30, 25}, {52, 25}, "puis la suite."},
    };
    Check(ResolveTranscriptSpanRange(breathing, "S1", "S2", resolvedSource,
                                     resolvedIn, resolvedDuration, error),
          "a two-frame breath between cues still merges: " + error);
    Check(resolvedIn.compare({50, 25}) == 0 &&
              resolvedDuration.compare({57, 25}) == 0,
          "and the merged range keeps the breath rather than dropping it");

    // The boundary itself: exactly the cue segmenter's maximum gap is a real
    // break, not a breath, so it is refused.
    const std::vector<TimelineTranscriptSpan> atLimit = {
        {"S1", sourceId, {0, 25}, {25, 25}, {0, 25}, "Avant"},
        {"S2", sourceId, {25, 25}, {25, 25}, {25, 25}, "après"},
    };
    std::vector<TimelineTranscriptSpan> justOver = atLimit;
    // 0.7 s at 25 fps is 17.5 frames, so 18 frames is over the line.
    justOver[1].source_in = {25 + 18, 25};
    Check(!ResolveTranscriptSpanRange(justOver, "S1", "S2", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a gap past the cue segmenter's own limit is refused");

    Check(!ResolveTranscriptSpanRange(run, "S3", "S4", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a run across two sources is refused");
    Check(!ResolveTranscriptSpanRange(run, "S2", "S1", resolvedSource,
                                      resolvedIn, resolvedDuration, error),
          "a backwards run is refused rather than silently reordered");
    Check(!ResolveTranscriptSpanRange(run, "S9", "", resolvedSource, resolvedIn,
                                      resolvedDuration, error),
          "an unknown span id is refused");

    // The rendered view and the structured spans must stay the same thing:
    // a backend that can only hand out JSON still answers span lookups.
    std::vector<TimelineTranscriptSpan> reparsed;
    Check(ParseTimelineTranscriptSpans(
              SerializeTimelineTranscriptSpans(document, run), reparsed, error),
          "the rendered view parses back to spans: " + error);
    Check(reparsed.size() == run.size() && reparsed[1].span_id == "S2" &&
              reparsed[1].source_in.compare(run[1].source_in) == 0 &&
              reparsed[1].duration.compare(run[1].duration) == 0 &&
              reparsed[1].likely_hallucinated && reparsed[1].likely_incomplete,
          "a span survives the view round trip with its exact times and "
          "transcript guard");

    // ITW3 -- attribution when a word's start was cut away, and the check
    // against readings of the montage audio. A project-shaped cache, since
    // the readings live beside the source transcripts.
    {
        const std::filesystem::path package =
            std::filesystem::temp_directory_path() /
            ("cutmachine-itw3-" + GenerateUlid());
        const std::filesystem::path transcripts =
            package / ".cutmachine" / "transcripts";
        std::filesystem::create_directories(transcripts);
        {
            std::ofstream cache(transcripts / (sourceId + ".json"));
            cache << "{\"version\":1,\"media_id\":\"" << sourceId
                  << "\",\"whisper_model\":\"test.bin\","
                     "\"source_rate\":{\"num\":25,\"den\":1},\"words\":["
                     "{\"text\":\"Donc\",\"start\":{\"value\":0,\"rate\":25},"
                     "\"end\":{\"value\":5,\"rate\":25}},"
                     "{\"text\":\"coupé\",\"start\":{\"value\":10,\"rate\":"
                     "25},\"end\":{\"value\":20,\"rate\":25}},"
                     "{\"text\":\"Et\",\"start\":{\"value\":28,\"rate\":25},"
                     "\"end\":{\"value\":33,\"rate\":25}},"
                     "{\"text\":\"pour\",\"start\":{\"value\":34,\"rate\":"
                     "25},\"end\":{\"value\":38,\"rate\":25}},"
                     "{\"text\":\"la suite\",\"start\":{\"value\":40,"
                     "\"rate\":25},\"end\":{\"value\":50,\"rate\":25}}]}";
        }
        // [0, 8) then [30, 50): "coupé" is cut out, and "Et" starts in the
        // removed frames but ends in the second clip.
        Document montage = document;
        montage.sequence.tracks[0].clips = {
            {"01K5000000000000000000000A", sourceId, {0, 25}, {8, 25}, {0, 25}},
            {"01K5000000000000000000000B",
             sourceId,
             {30, 25},
             {20, 25},
             {8, 25}},
        };
        std::vector<TimelineTranscriptSpan> words;
        TimelineWordCheck check;
        Check(BuildTimelineTranscriptWordSpans(montage, transcripts, words,
                                               error, &check),
              "montage word spans build: " + error);
        Check(words.size() == 4 && words[1].text == "Et" &&
                  words[1].timeline_in == RationalTime{8, 25} &&
                  check.readings == 0 && check.unheard.empty(),
              "a word whose start was cut goes to the clip playing the rest, "
              "and nothing is dropped without a montage reading");

        // A reading of the montage audio that hears everything but "pour".
        TimelineAudioPlan plan;
        Check(BuildTimelineAudioPlan(
                  montage, package / "project.cutmachine.json", plan, error),
              "the montage audio identity is computed: " + error);
        std::filesystem::create_directories(package / ".cutmachine" /
                                            "timeline-transcripts");
        {
            std::ofstream reading(TimelineTranscriptCachePath(
                package / ".cutmachine", montage.sequence.id, true));
            reading << "{\"version\":1,\"media_id\":\"" << plan.cache_identity
                    << "\",\"whisper_model\":\"test.bin\",\"verbatim\":true,"
                       "\"source_rate\":{\"num\":25,\"den\":1},\"words\":["
                       "{\"text\":\"donc,\",\"start\":{\"value\":1,\"rate\":"
                       "25},\"end\":{\"value\":5,\"rate\":25}},"
                       "{\"text\":\"et la\",\"start\":{\"value\":9,\"rate\":"
                       "25},\"end\":{\"value\":20,\"rate\":25}},"
                       "{\"text\":\"suite.\",\"start\":{\"value\":20,"
                       "\"rate\":25},\"end\":{\"value\":28,\"rate\":25}}]}";
        }
        Check(BuildTimelineTranscriptWordSpans(montage, transcripts, words,
                                               error, &check),
              "montage word spans build with a reading: " + error);
        Check(check.readings == 1 && check.unheard.size() == 1 &&
                  check.unheard[0].text == "pour" &&
                  check.unheard[0].clip_id == "01K5000000000000000000000B" &&
                  words.size() == 3 &&
                  std::none_of(words.begin(), words.end(),
                               [](const TimelineTranscriptSpan& span) {
                                   return span.text == "pour";
                               }),
              "a word no reading of the montage contains is reported, not "
              "published");
        const std::string view =
            SerializeTimelineTranscriptSpans(montage, words, &check);
        Check(view.find("\"unheard_words\":[{\"text\":\"pour\"") !=
                      std::string::npos &&
                  view.find("\"montage_readings\":1") != std::string::npos &&
                  view.find("\"" + sourceId + "\":\"whisper\"") !=
                      std::string::npos,
              "the agent view names the unheard word and the word timing: " +
                  view);

        // A reading of an older arrangement proves nothing.
        montage.sequence.tracks[0].clips[1].duration = {19, 25};
        Check(BuildTimelineTranscriptWordSpans(montage, transcripts, words,
                                               error, &check) &&
                  check.readings == 0 && check.unheard.empty(),
              "a stale montage reading is not used");
        std::filesystem::remove_all(package);
    }

    // BENCH-2026-09 -- the rule a reading must meet for a word to count as
    // heard: folding, then a bounded inflection, and nothing looser.
    {
        using Pairs = std::vector<std::pair<std::string, std::string>>;
        for (const auto& same : Pairs{{"Mélangé", "mélangé"},
                                      {"ÉTÉ", "été"},
                                      {"Œuvre", "œuvre"},
                                      {"donc,", "Donc"},
                                      {"« alors »", "alors…"},
                                      {"l'", "l’homme"},
                                      {"aujourd'hui", "aujourd’hui"},
                                      {"-ce", "est-ce"},
                                      {"peut-être", "peut être"},
                                      // AGENT-2026-09 A13 -- digits read
                                      // back as words.
                                      {"3ème", "troisième"},
                                      {"3e", "Troisième"},
                                      {"1er", "premier"},
                                      {"1re", "première"},
                                      {"2nd", "second"},
                                      {"4ème", "quatrième"},
                                      {"5e", "cinquième"},
                                      {"9ème", "neuvième"},
                                      {"21", "vingt et un"},
                                      {"71", "soixante et onze"},
                                      {"80", "quatre-vingts"},
                                      {"92", "quatre-vingt-douze"},
                                      {"200", "deux cents"},
                                      {"250", "deux cent cinquante"},
                                      {"20e", "vingtième"},
                                      {"2026", "2026"}}) {
            Check(MatchHeardWord(same.first, same.second) ==
                      HeardWordMatch::Exact,
                  "'" + same.first + "' and '" + same.second +
                      "' are the same word once folded");
        }
        for (const auto& inflected : Pairs{{"mélangé", "mélangés"},
                                           {"mélangé", "mélangées"},
                                           {"Mélangée", "mélangés"},
                                           {"grand", "grandes"},
                                           {"beau", "beaux"},
                                           {"été", "étés"},
                                           {"fait", "faits,"},
                                           // AGENT-2026-09 A13 -- -al/-aux.
                                           {"spécial.", "spéciaux"},
                                           {"principaux", "principal"}}) {
            Check(MatchHeardWord(inflected.first, inflected.second) ==
                      HeardWordMatch::Inflected,
                  "'" + inflected.first + "' and '" + inflected.second +
                      "' differ only by gender or number");
        }
        for (const auto& other : Pairs{{"le", "les"},
                                       {"de", "des"},
                                       {"un", "une"},
                                       {"vu", "vue"},
                                       {"est", "et"},
                                       {"tout", "tous"},
                                       {"mangé", "mange"},
                                       {"mélangé", "mélanger"},
                                       {"quel", "quelle"},
                                       {"travail", "travaux"},
                                       {"mal", "maux"},
                                       {"2026", "deux mille vingt-six"},
                                       {"3ème", "trois"},
                                       {"grand", "grandir"}}) {
            Check(MatchHeardWord(other.first, other.second) ==
                      HeardWordMatch::None,
                  "'" + other.first + "' is not taken for '" + other.second +
                      "'");
        }
    }

    // BENCH-2026-09 -- a montage that loses words three ways: a reading
    // writes another inflection, the aligner put a heard word in a cut, and
    // a word no reading hears. Frames at 25 i/s; clips [0, 60) and
    // [80, 140) of the rush, then [150, 200).
    {
        const std::filesystem::path package =
            std::filesystem::temp_directory_path() /
            ("cutmachine-bench-words-" + GenerateUlid());
        const std::filesystem::path transcripts =
            package / ".cutmachine" / "transcripts";
        std::filesystem::create_directories(transcripts);
        const auto frames = [](int64_t frame) {
            return RationalTime{frame, 25};
        };
        Transcript source;
        source.media_id = sourceId;
        source.whisper_model = "test.bin";
        source.source_rate = {25, 1};
        source.words = {
            {"Les", frames(0), frames(5)},
            {"couleurs", frames(6), frames(15)},
            {"sont", frames(16), frames(20)},
            {"mélangé", frames(21), frames(40)},
            {"dans", frames(41), frames(50)},
            {"l'eau", frames(51), frames(58)},
            // Removed [60, 80): a hesitation, and a word the aligner put
            // inside it although the montage still says it.
            {"euh", frames(62), frames(72)},
            {"qui", frames(73), frames(79)},
            {"coule", frames(81), frames(95)},
            {"vite.", frames(96), frames(110)},
            {"Enfin", frames(112), frames(120)},
            {"vraiment", frames(121), frames(139)},
            // Removed [140, 150): "donc" only the verbatim reading hears,
            // and a "puis" the kept "Puis" already explains.
            {"donc", frames(140), frames(144)},
            {"puis", frames(145), frames(149)},
            {"Puis", frames(151), frames(157)},
            {"c'est", frames(158), frames(165)},
            {"fini.", frames(166), frames(180)},
        };
        Check(SaveAudioTranscript((transcripts / (sourceId + ".json")).string(),
                                  source, error),
              "bench transcript saves: " + error);
        Document montage = document;
        montage.sequence.tracks[0].clips = {
            {"01K5000000000000000000000C", sourceId, frames(0), frames(60),
             frames(0)},
            {"01K5000000000000000000000D", sourceId, frames(80), frames(60),
             frames(60)},
            {"01K5000000000000000000000E", sourceId, frames(150), frames(50),
             frames(120)},
        };
        TimelineAudioPlan plan;
        Check(BuildTimelineAudioPlan(
                  montage, package / "project.cutmachine.json", plan, error),
              "bench montage audio identity: " + error);
        std::filesystem::create_directories(package / ".cutmachine" /
                                            "timeline-transcripts");
        const auto reading =
            [&](bool verbatim,
                const std::vector<std::pair<std::string, int64_t>>& heard) {
                Transcript montageReading;
                montageReading.media_id = plan.cache_identity;
                montageReading.whisper_model = "test.bin";
                montageReading.verbatim = verbatim;
                montageReading.source_rate = {25, 1};
                for (const auto& word : heard) {
                    montageReading.words.push_back({word.first,
                                                    frames(word.second),
                                                    frames(word.second + 4)});
                }
                Check(SaveAudioTranscript(TimelineTranscriptCachePath(
                                              package / ".cutmachine",
                                              montage.sequence.id, verbatim)
                                              .string(),
                                          montageReading, error),
                      "bench reading saves: " + error);
            };
        // Montage positions: Les 0 ... l'eau 51, the junction at 60, coule
        // 61, vite. 76, Enfin 92, vraiment 101, the junction at 120, Puis
        // 121. Whisper's times drift a little later.
        // The standard reading writes "qui coule" as one word, as Whisper
        // wrote "à la" on the bench run: the kept "coule" must not explain
        // the "qui" with it.
        reading(false, {{"Les", 1},
                        {"couleurs", 7},
                        {"sont", 17},
                        {"mélangés", 22},
                        {"dans", 42},
                        {"l'eau", 52},
                        {"qui coule", 58},
                        {"vite.", 78},
                        {"Enfin,", 93},
                        {"Puis", 122},
                        {"c'est", 129},
                        {"fini.", 137}});
        reading(true, {{"Les", 1},
                       {"couleurs", 7},
                       {"sont", 17},
                       {"mélangés", 22},
                       {"dans", 42},
                       {"l'eau,", 52},
                       {"qui", 59},
                       {"coule", 63},
                       {"vite.", 78},
                       {"Enfin,", 93},
                       {"donc", 118},
                       {"puis", 121},
                       {"c'est", 129},
                       {"fini.", 137}});

        std::vector<TimelineTranscriptSpan> words;
        TimelineWordCheck check;
        Check(BuildTimelineTranscriptWordSpans(montage, transcripts, words,
                                               error, &check),
              "bench word spans build: " + error);
        Check(check.readings == 2, "both readings of the montage are used");
        Check(std::any_of(words.begin(), words.end(),
                          [](const TimelineTranscriptSpan& span) {
                              return span.text == "mélangé";
                          }) &&
                  check.inflected.size() == 1 &&
                  check.inflected[0].text == "mélangé" &&
                  check.inflected[0].heard_as == "mélangés" &&
                  check.inflected[0].timeline_in == frames(21),
              "a word a reading writes in the plural is kept, and the "
              "inflected match is reported");
        Check(check.unheard.size() == 1 &&
                  check.unheard[0].text == "vraiment" &&
                  check.unheard[0].timeline_in == frames(101) &&
                  check.unheard[0].duration == frames(18),
              "a word no reading hears is still left out, with its played "
              "duration");
        Check(check.cut_but_heard.size() == 1 &&
                  check.cut_but_heard[0].text == "qui" &&
                  check.cut_but_heard[0].clip_id ==
                      "01K5000000000000000000000D" &&
                  check.cut_but_heard[0].source_in == frames(73) &&
                  check.cut_but_heard[0].duration == frames(6) &&
                  check.cut_but_heard[0].timeline_in == frames(60) &&
                  check.cut_but_heard[0].heard_as ==
                      std::vector<std::string>({"qui coule", "qui"}),
              "a word aligned into a cut but heard by both readings at the "
              "junction is named; the hesitation, a word only one reading "
              "hears and a word a kept one explains are not");
        Check(std::none_of(words.begin(), words.end(),
                           [](const TimelineTranscriptSpan& span) {
                               return span.text == "qui";
                           }),
              "the word is reported, never put back into the spans");
        const std::string view =
            SerializeTimelineTranscriptSpans(montage, words, &check);
        Check(view.find("\"inflected_matches\":[{\"text\":\"mélangé\","
                        "\"heard_as\":\"mélangés\"") != std::string::npos &&
                  view.find("\"cut_but_heard\":[{\"text\":\"qui\",\"reason\":"
                            "\"aligned_in_removed_range\"") !=
                      std::string::npos &&
                  view.find("\"heard_as\":[\"qui coule\",\"qui\"]") !=
                      std::string::npos,
              "the agent view carries both reports: " + view);

        // One reading only: it alone decides.
        std::filesystem::remove(TimelineTranscriptCachePath(
            package / ".cutmachine", montage.sequence.id, false));
        Check(BuildTimelineTranscriptWordSpans(montage, transcripts, words,
                                               error, &check) &&
                  check.readings == 1 && check.cut_but_heard.size() == 2 &&
                  check.cut_but_heard[0].text == "qui" &&
                  check.cut_but_heard[1].text == "donc",
              "with the verbatim reading alone, its extra word counts too");
        std::filesystem::remove_all(package);
    }

    // BENCH-2026-09 -- gaps between cartons while a word is said.
    // DEC18 -- docs/roadmap/BENCH-2026-09.md. A word the cartons were made
    // from is never in a gap any more (generate_captions keeps every one
    // covered, caption_generation_tests), so the gaps name only the words
    // the check found outside the cartons: "carton_edge_on_cut" is gone.
    {
        const auto frames = [](int64_t frame) {
            return RationalTime{frame, 25};
        };
        const std::vector<std::pair<RationalTime, RationalTime>> cartons = {
            {frames(0), frames(40)},    // gap [40, 60): an unheard word
            {frames(60), frames(30)},   // gap [90, 100): silence
            {frames(100), frames(20)},  // gap [120, 130): silence
            {frames(130), frames(20)},
        };
        TimelineWordCheck check;
        check.unheard.push_back(
            {"", "", "trou", frames(0), frames(44), frames(12)});
        CutHeardWord cut;
        cut.text = "qui";
        cut.timeline_in = frames(60);
        check.cut_but_heard.push_back(cut);
        const std::vector<CaptionSpeechGap> gaps =
            FindCaptionSpeechGaps(cartons, check);
        Check(gaps.size() == 1, "one of three gaps falls during speech");
        if (gaps.size() == 1) {
            Check(gaps[0].after == 0 && gaps[0].timeline_in == frames(40) &&
                      gaps[0].duration == frames(20) &&
                      gaps[0].words.size() == 2 &&
                      gaps[0].words[0].text == "trou" &&
                      gaps[0].words[0].reason == "unheard" &&
                      gaps[0].words[1].text == "qui" &&
                      gaps[0].words[1].reason == "aligned_in_removed_range",
                  "a gap names the unheard word and the word heard at a "
                  "junction inside it");
        }
        const std::string json = SerializeCaptionSpeechGaps(gaps);
        Check(json.find("\"after_carton\":0") != std::string::npos &&
                  json.find("\"reason\":\"unheard\"") != std::string::npos &&
                  json.find("carton_edge_on_cut") == std::string::npos,
              "the gaps serialize, without the retired reason: " + json);
    }

    std::filesystem::remove_all(directory);
    std::cout << "interview short tests passed\n";
    return 0;
}
