// IA4 -- docs/roadmap/IA-2026-09.md. The portable half of an inspection:
// which window and which frames, the level arithmetic and the comparison of
// the words expected with the words heard. No FFmpeg, no disk, no model.

#include "CutInspection.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

// This suite's own named test, which TestSupport.h's Test would otherwise
// take the place of for a lambda; kept, with its output.
void Case(const std::string& name, const std::function<void()>& body) {
    const int before = failures;
    body();
    std::cout << (failures == before ? "PASS: " : "FAIL: ") << name << '\n';
}

// Two clips of two seconds on a video track at 25 fps, then a clip of one
// second: cuts at frames 50 and 100, the timeline 125 frames long.
Document ThreeShots() {
    Document document;
    document.sequence.id = "01K9G00000000000000000000S";
    document.sequence.frame_rate = {25, 1};
    document.sources = {
        {"01K9G000000000000000000001", "a.mov", {25, 1}, {500, 25}}};
    const auto clip = [](const char* id, int64_t at, int64_t length) {
        DocumentClip value;
        value.id = id;
        value.source_id = "01K9G000000000000000000001";
        value.source_in = {0, 25};
        value.duration = {length, 25};
        value.timeline_in = {at, 25};
        value.include_audio = false;
        return value;
    };
    document.sequence.tracks = {
        {"01K9G0000000000000000000T0",
         "video",
         0,
         {clip("01K9G00000000000000000000A", 0, 50),
          clip("01K9G00000000000000000000B", 50, 50),
          clip("01K9G00000000000000000000C", 100, 25)}}};
    return document;
}

CutInspectionRequest AtClip(const char* id, bool tail = false) {
    CutInspectionRequest request;
    request.clip_id = id;
    request.at_tail = tail;
    return request;
}

TimelineTranscriptSpan Span(const std::string& text, int64_t frame,
                            int64_t length, bool straddles = false) {
    TimelineTranscriptSpan span;
    span.text = text;
    span.timeline_in = {frame, 25};
    span.duration = {length, 25};
    span.straddles_cut = straddles;
    return span;
}

CutInspectionWord Heard(const std::string& text, int64_t frame,
                        int64_t length) {
    CutInspectionWord word;
    word.text = text;
    word.timeline_in = {frame, 25};
    word.duration = {length, 25};
    return word;
}

bool HasAlert(const CutInspectionWords& words, const std::string& kind,
              const std::string& text) {
    for (const CutInspectionAlert& alert : words.alerts)
        if (alert.kind == kind && alert.text == text) return true;
    return false;
}

}  // namespace

int main() {
    Case("a clip's head names the cut, the window straddles it", [] {
        CutInspectionPlan plan;
        std::string error;
        Check(PlanCutInspection(ThreeShots(),
                                AtClip("01K9G00000000000000000000B"), plan,
                                error),
              "plan: " + error);
        Check(plan.cut == 50 && plan.first == 25 && plan.end == 75,
              "the default window is one second on either side of frame 50");
        Check(!plan.clipped_at_start && !plan.clipped_at_end,
              "a window inside the timeline is not clipped");
        Check(plan.shown.size() == 6, "six pictures by default");
        const bool before = std::find(plan.shown.begin(), plan.shown.end(),
                                      49) != plan.shown.end();
        const bool after = std::find(plan.shown.begin(), plan.shown.end(),
                                     50) != plan.shown.end();
        Check(before && after,
              "the last frame before the cut and the first at it are shown");
        Check(std::is_sorted(plan.shown.begin(), plan.shown.end()) &&
                  plan.shown.front() == 25 && plan.shown.back() == 74,
              "the pictures are ascending and reach both edges");
    });

    Case("a clip's tail, and a window clipped by the timeline's end", [] {
        CutInspectionPlan plan;
        std::string error;
        CutInspectionRequest request =
            AtClip("01K9G00000000000000000000C", true);
        request.after = {2, 1};
        Check(PlanCutInspection(ThreeShots(), request, plan, error),
              "plan: " + error);
        Check(plan.cut == 125 && plan.end == 125 && plan.first == 100,
              "the tail of the last clip is the timeline's end");
        Check(plan.clipped_at_end, "the plan says the end was clipped");
        Check(plan.shown.back() == 124,
              "the last frame of the film is shown, the cut frame is not "
              "inside the window");
    });

    Case("a window clipped by the timeline's start", [] {
        CutInspectionPlan plan;
        std::string error;
        CutInspectionRequest request;
        request.position = RationalTime{10, 25};
        Check(PlanCutInspection(ThreeShots(), request, plan, error),
              "plan: " + error);
        Check(plan.first == 0 && plan.cut == 10 && plan.end == 35 &&
                  plan.clipped_at_start,
              "the window starts at the film's first frame and says so");
    });

    Case("refusals, by name", [] {
        CutInspectionPlan plan;
        std::string error;
        Check(!PlanCutInspection(ThreeShots(),
                                 AtClip("01K9G00000000000000000000Z"), plan,
                                 error) &&
                  error.find("no video or audio track") != std::string::npos,
              "an unknown clip is refused: " + error);
        CutInspectionRequest request = AtClip("01K9G00000000000000000000B");
        request.before = {1, 50};
        Check(!PlanCutInspection(ThreeShots(), request, plan, error) &&
                  error.find("whole frames") != std::string::npos,
              "a side between two frames is refused: " + error);
        request.before = {6, 1};
        Check(!PlanCutInspection(ThreeShots(), request, plan, error) &&
                  error.find("between 0 and 5") != std::string::npos,
              "a side past five seconds is refused: " + error);
        request = AtClip("01K9G00000000000000000000B");
        request.images = 13;
        Check(!PlanCutInspection(ThreeShots(), request, plan, error),
              "thirteen pictures are refused");
        request = CutInspectionRequest{};
        request.position = RationalTime{126, 25};
        Check(!PlanCutInspection(ThreeShots(), request, plan, error) &&
                  error.find("outside") != std::string::npos,
              "a position past the end is refused: " + error);
        Check(!PlanCutInspection(ThreeShots(), CutInspectionRequest{}, plan,
                                 error) &&
                  error.find("name the cut") != std::string::npos,
              "a request naming no cut is refused: " + error);
        request = CutInspectionRequest{};
        request.position = RationalTime{0, 25};
        request.before = {1, 1};
        request.after = {0, 1};
        Check(!PlanCutInspection(ThreeShots(), request, plan, error),
              "a window holding no frame is refused");
    });

    Case("two pictures are exactly the pair around the cut", [] {
        CutInspectionPlan plan;
        std::string error;
        CutInspectionRequest request = AtClip("01K9G00000000000000000000B");
        request.images = 2;
        Check(PlanCutInspection(ThreeShots(), request, plan, error),
              "plan: " + error);
        Check(plan.shown == std::vector<int64_t>({49, 50}),
              "the pair around the cut, and nothing else");
    });

    Case("levels: a tone before the cut, silence after it", [] {
        CutInspectionPlan plan;
        plan.rate = {25, 1};
        plan.first = 0;
        plan.end = 10;
        plan.cut = 5;
        // 1920 samples per frame at 48 kHz; a sine of amplitude 1/2 on both
        // channels for five frames, then digital silence.
        std::vector<float> samples;
        for (int index = 0; index < 1920 * 10; ++index) {
            const double value =
                index < 1920 * 5 ? 0.5 * std::sin(index * 0.0575958653) : 0.0;
            samples.push_back(static_cast<float>(value));
            samples.push_back(static_cast<float>(value));
        }
        CutInspectionAudio audio;
        std::string error;
        Check(MeasureCutInspectionAudio(samples, 48000, plan, audio, error),
              "measure: " + error);
        Check(audio.frame_rms_dbfs.size() == 10, "one level per frame");
        // RMS of a sine of amplitude 1/2: 0.354, -9 dBFS; peak -6 dBFS.
        Check(audio.frame_rms_dbfs[0] == -9 && audio.frame_peak_dbfs[0] == -6,
              "the tone measures -9 dBFS RMS, -6 dBFS peak (" +
                  std::to_string(audio.frame_rms_dbfs[0]) + ", " +
                  std::to_string(audio.frame_peak_dbfs[0]) + ")");
        Check(audio.frame_rms_dbfs[7] == kCutInspectionSilenceDbfs,
              "digital silence is reported as silence");
        Check(audio.before_cut_rms_dbfs == -9 &&
                  audio.after_cut_rms_dbfs == kCutInspectionSilenceDbfs,
              "the level drops at the cut");
        Check(audio.window_peak_dbfs == -6, "the window's peak");
        std::vector<float> shortMix(samples.begin(), samples.end() - 2);
        Check(!MeasureCutInspectionAudio(shortMix, 48000, plan, audio, error),
              "a mix shorter than the window is refused");
    });

    Case("expected words: edges, and a word a cut runs through", [] {
        CutInspectionPlan plan;
        plan.rate = {25, 1};
        plan.first = 25;
        plan.end = 75;
        plan.cut = 50;
        const std::vector<TimelineTranscriptSpan> spans = {
            Span("avant", 10, 5),         // outside
            Span("bord", 20, 10),         // across the window's start
            Span("question", 30, 10),     // inside
            Span("coupé", 45, 10, true),  // a cut runs through it
            Span("réponse", 60, 10),      // inside
            Span("après", 80, 5),         // outside
        };
        CutInspectionWords words;
        CollectExpectedWords(spans, plan, words);
        Check(words.expected.size() == 4, "four words touch the window");
        Check(words.expected[0].text == "bord" &&
                  words.expected[0].at_window_edge,
              "a word across the window's edge is marked");
        Check(!words.transcribed, "nothing was read");
        Check(words.alerts.size() == 1 &&
                  HasAlert(words, "word_crosses_cut", "coupé"),
              "the word a cut runs through raises the one alert");
    });

    Case("heard against expected: missing, extra, inflected, edges", [] {
        CutInspectionPlan plan;
        plan.rate = {25, 1};
        plan.first = 25;
        plan.end = 75;
        plan.cut = 50;
        const std::vector<TimelineTranscriptSpan> spans = {
            Span("bord", 20, 10),    Span("Vous", 30, 3), Span("êtes", 33, 3),
            Span("arrivée", 36, 5),  Span("ici", 41, 4),  Span("Oui", 52, 5),
            Span("vraiment", 58, 8),
        };
        CutInspectionWords words;
        CollectExpectedWords(spans, plan, words);
        // The model heard "arrivé" (another inflection), missed "ici",
        // added "bon", and heard a fragment right at the window's end.
        CompareHeardWords(
            {Heard("vous", 30, 3), Heard("êtes", 33, 3), Heard("arrivé", 36, 5),
             Heard("oui,", 52, 5), Heard("bon", 57, 1),
             Heard("vraiment.", 58, 8), Heard("et", 72, 3)},
            plan, words);
        Check(words.transcribed, "the reading is recorded");
        Check(HasAlert(words, "expected_not_heard", "ici"),
              "a word the reading missed is an alert");
        Check(HasAlert(words, "heard_not_expected", "bon"),
              "a word the reading added is an alert");
        Check(!HasAlert(words, "expected_not_heard", "arrivée"),
              "an inflected reading pairs with the expected word");
        Check(!HasAlert(words, "expected_not_heard", "bord"),
              "a word the window cuts is never held against the reading");
        Check(!HasAlert(words, "heard_not_expected", "et"),
              "a fragment at the window's edge is not an alert");
        Check(words.alerts.size() == 2,
              "exactly the two alerts, got " +
                  std::to_string(words.alerts.size()));
    });

    Case("two readings that group their words differently agree", [] {
        CutInspectionPlan plan;
        plan.rate = {25, 1};
        plan.first = 0;
        plan.end = 100;
        plan.cut = 50;
        // Measured on a synthetic interview: the source's reading and the
        // window's reading wrote the same sentence in different groups.
        const std::vector<TimelineTranscriptSpan> spans = {
            Span("à la", 25, 6), Span("mode", 31, 6), Span("? J'ai", 64, 6),
            Span("toujours", 70, 8)};
        CutInspectionWords words;
        CollectExpectedWords(spans, plan, words);
        CompareHeardWords({Heard("à la mode", 31, 6), Heard("?", 37, 1),
                           Heard("J'ai", 64, 6), Heard("toujours", 70, 8)},
                          plan, words);
        Check(words.alerts.empty(),
              "no alert when only the grouping differs, got " +
                  std::to_string(words.alerts.size()));
    });

    Case("no transcript: said, and a reading raises nothing", [] {
        CutInspectionPlan plan;
        plan.rate = {25, 1};
        plan.first = 0;
        plan.end = 50;
        plan.cut = 25;
        CutInspectionWords words;
        words.expected_unavailable = "no cached transcript";
        CompareHeardWords({Heard("bonjour", 10, 5)}, plan, words);
        Check(words.transcribed && words.heard.size() == 1 &&
                  words.alerts.empty(),
              "the heard words are kept, nothing is compared");
        const std::string json =
            SerializeCutInspection(plan, CutInspectionAudio{}, words);
        Check(json.find("\"expected_words_unavailable\"") != std::string::npos,
              "the report says why no word is expected");
    });

    if (failures == 0) std::cout << "All cut inspection tests passed\n";
    return failures == 0 ? 0 : 1;
}
