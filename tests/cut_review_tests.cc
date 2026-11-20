// RC1 -- docs/roadmap/RACCORDS-2026-10.md. The portable half of a review of
// the junctions: which junctions a timeline holds, the frames of a band,
// the sound and picture measures on synthetic samples and planes, the alerts
// each threshold raises, and the review's loop with a renderer that renders
// nothing. No FFmpeg, no disk.

#include "CutReview.h"

#include <cmath>
#include <cstdint>
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

const Ulid kRushA = "01K9R000000000000000000001";
const Ulid kRushB = "01K9R000000000000000000002";

DocumentClip Clip(const Ulid& id, const Ulid& source, int64_t in,
                  int64_t length, int64_t at) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.source_in = {in, 25};
    clip.duration = {length, 25};
    clip.timeline_in = {at, 25};
    clip.include_audio = false;
    return clip;
}

// V1: A [0,50) from rush A, B [50,100) from rush B, C [100,150) from rush B
// jumping 50 frames ahead, D [150,175) reading on from C. A1 the same cuts,
// except that its third clip starts after a gap. A hidden V2 and a muted A2
// cut too, and must not count.
Document Montage() {
    Document document;
    document.sequence.id = "01K9R00000000000000000000S";
    document.sequence.width = 320;
    document.sequence.height = 180;
    document.sequence.frame_rate = {25, 1};
    document.sources = {{kRushA, "a.mov", {25, 1}, {500, 25}},
                        {kRushB, "b.mov", {25, 1}, {500, 25}}};
    DocumentTrack video{"01K9R0000000000000000000T0", "video", 0, {}};
    video.clips = {Clip("01K9R00000000000000000000A", kRushA, 0, 50, 0),
                   Clip("01K9R00000000000000000000B", kRushB, 0, 50, 50),
                   Clip("01K9R00000000000000000000C", kRushB, 100, 50, 100),
                   Clip("01K9R00000000000000000000D", kRushB, 150, 25, 150)};
    DocumentTrack audio{"01K9R0000000000000000000T1", "audio", 1, {}};
    audio.clips = {Clip("01K9R00000000000000000001A", kRushA, 0, 50, 0),
                   Clip("01K9R00000000000000000001B", kRushB, 0, 50, 50),
                   Clip("01K9R00000000000000000001C", kRushB, 100, 40, 110)};
    DocumentTrack hidden{"01K9R0000000000000000000T2", "video", 2, {}};
    hidden.visible = false;
    hidden.clips = {Clip("01K9R00000000000000000002A", kRushA, 0, 10, 0),
                    Clip("01K9R00000000000000000002B", kRushA, 20, 10, 10)};
    DocumentTrack muted{"01K9R0000000000000000000T3", "audio", 3, {}};
    muted.muted = true;
    muted.clips = {Clip("01K9R00000000000000000003A", kRushA, 0, 10, 0),
                   Clip("01K9R00000000000000000003B", kRushA, 20, 10, 10)};
    document.sequence.tracks = {video, audio, hidden, muted};
    return document;
}

// Interleaved stereo: `seconds` of a deterministic noise at `amplitude`,
// plus a 440 Hz tone at `tone` over [toneFrom, toneTo) seconds.
std::vector<float> Sound(double seconds, double amplitude,
                         double afterAmplitude, double cutSeconds,
                         double tone = 0.0, double toneFrom = 0.0,
                         double toneTo = 0.0) {
    const int64_t count = static_cast<int64_t>(std::llround(seconds * 48000));
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(count) * 2);
    uint32_t state = 12345;
    for (int64_t index = 0; index < count; ++index) {
        const double time = static_cast<double>(index) / 48000.0;
        state = (state * 1664525u) + 1013904223u;
        const double noise =
            (static_cast<double>(state >> 8) / 8388608.0) - 1.0;
        double value =
            noise * (time < cutSeconds ? amplitude : afterAmplitude) * 1.7;
        if (time >= toneFrom && time < toneTo)
            value += tone * std::sin(2.0 * 3.14159265358979 * 440.0 * time);
        samples.push_back(static_cast<float>(value));
        samples.push_back(static_cast<float>(value));
    }
    return samples;
}

// A 2 s window, cut at its middle, at 25 fps.
CutInspectionPlan Window() {
    CutInspectionPlan plan;
    plan.rate = {25, 1};
    plan.first = 25;
    plan.cut = 50;
    plan.end = 75;
    plan.shown = {48, 49, 50, 51};
    return plan;
}

// A 64x36 plane: a diagonal gradient shifted by `shift` pixels, with a
// block of `block` grey.
std::vector<uint8_t> Plane(int32_t shift, uint8_t block = 0,
                           int32_t blockLeft = 0) {
    std::vector<uint8_t> plane(64 * 36);
    for (int32_t y = 0; y < 36; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
            plane[static_cast<size_t>((y * 64) + x)] =
                static_cast<uint8_t>(((x + shift) * 3 + y * 2) % 256);
        }
    }
    if (block != 0) {
        for (int32_t y = 8; y < 28; ++y)
            for (int32_t x = blockLeft; x < blockLeft + 16 && x < 64; ++x)
                plane[static_cast<size_t>((y * 64) + x)] = block;
    }
    return plane;
}

TimelineJunction SoundAndPicture() {
    TimelineJunction junction;
    junction.frame = 50;
    junction.picture = true;
    junction.sound = true;
    TimelineJunctionCut video;
    video.track_kind = "video";
    video.outgoing_source_id = kRushA;
    video.incoming_source_id = kRushB;
    TimelineJunctionCut audio = video;
    audio.track_kind = "audio";
    junction.cuts = {video, audio};
    return junction;
}

bool HasAlert(const JunctionReview& review, const std::string& kind) {
    for (const CutReviewAlert& alert : review.alerts)
        if (alert.kind == kind) return true;
    return false;
}

}  // namespace

int main() {
    Case("the junctions of a timeline, and only them", [] {
        std::vector<TimelineJunction> junctions;
        int64_t offGrid = -1;
        std::string error;
        Check(ListTimelineJunctions(Montage(), junctions, offGrid, error),
              "listed: " + error);
        Check(junctions.size() == 3, "three junctions: 50, 100, 150 (" +
                                         std::to_string(junctions.size()) +
                                         ")");
        if (junctions.size() != 3) return;
        Check(junctions[0].frame == 50 && junctions[0].picture &&
                  junctions[0].sound && junctions[0].cuts.size() == 2,
              "frame 50: picture and sound cut there");
        Check(!junctions[0].cuts[0].continuous &&
                  junctions[0].cuts[0].outgoing_source_id == kRushA,
              "frame 50: two rushes, not continuous");
        Check(junctions[1].frame == 100 && junctions[1].picture &&
                  !junctions[1].sound,
              "frame 100: the sound's next clip starts after a gap");
        Check(!junctions[1].cuts[0].continuous &&
                  junctions[1].cuts[0].outgoing_source_id ==
                      junctions[1].cuts[0].incoming_source_id,
              "frame 100: one rush, a jump in it");
        Check(junctions[2].frame == 150 && junctions[2].picture_continuous,
              "frame 150: D reads on from C, a through edit");
        Check(offGrid == 0, "nothing off the grid");

        Document offset = Montage();
        offset.sequence.tracks[1].clips[1].duration = {2001, 1000};
        offset.sequence.tracks[1].clips.pop_back();
        offset.sequence.tracks[1].clips.push_back(
            Clip("01K9R00000000000000000001E", kRushB, 0, 10, 0));
        offset.sequence.tracks[1].clips.back().timeline_in = {4001, 1000};
        Check(ListTimelineJunctions(offset, junctions, offGrid, error) &&
                  offGrid == 1,
              "a junction between two frames is counted, not reviewed");
    });

    Case("the frames of a band", [] {
        const std::vector<int64_t> frames = CutBandFrames(25, 50, 75, 3);
        Check(frames == std::vector<int64_t>({25, 37, 49, 50, 62, 74}),
              "three on either side, the two around the cut among them");
        Check(CutBandFrames(48, 50, 75, 3) ==
                  std::vector<int64_t>({48, 49, 50, 62, 74}),
              "two frames before the cut: two pictures before it");
        Check(CutBandFrames(25, 50, 75, 1) == std::vector<int64_t>({49, 50}),
              "one on either side: the two around the cut");
    });

    Case("a review's window keeps the band and the measured frames", [] {
        const Document document = Montage();
        std::vector<TimelineJunction> junctions;
        int64_t offGrid = 0;
        std::string error;
        ListTimelineJunctions(document, junctions, offGrid, error);
        CutReviewRequest request;
        CutInspectionPlan plan;
        std::vector<int64_t> band;
        Check(PlanJunctionReview(document, junctions[0], request, plan, band,
                                 error),
              "planned: " + error);
        Check(plan.first == 25 && plan.cut == 50 && plan.end == 75,
              "one second on either side");
        Check(plan.shown ==
                  std::vector<int64_t>({25, 37, 48, 49, 50, 51, 62, 74}),
              "the band and cut - 2 .. cut + 1");
        request.side = {1, 50};
        Check(!PlanJunctionReview(document, junctions[0], request, plan, band,
                                  error),
              "a side between two frames is refused");
    });

    Case("an ambience that jumps", [] {
        // -50 dBFS of noise, then -30.
        const std::vector<float> samples = Sound(2.0, 0.00316, 0.0316, 1.0);
        JunctionSoundMeasures sound;
        std::string error;
        Check(MeasureJunctionSound(samples, 48000, Window(), sound, error),
              "measured: " + error);
        Check(
            sound.ambience_before_dbfs && sound.ambience_after_dbfs &&
                *sound.ambience_after_dbfs - *sound.ambience_before_dbfs >=
                    18 &&
                *sound.ambience_after_dbfs - *sound.ambience_before_dbfs <= 22,
            "the ambience rises by about 20 dB (" +
                std::to_string(sound.ambience_before_dbfs.value_or(0)) +
                " -> " + std::to_string(sound.ambience_after_dbfs.value_or(0)) +
                ")");
        JunctionReview review;
        review.junction = SoundAndPicture();
        review.sound = sound;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(HasAlert(review, "ambience_jump"), "ambience_jump raised");
        Check(!HasAlert(review, "voice_jump"),
              "no voice on either side: no voice_jump");
        Check(!HasAlert(review, "sound_cut_live"),
              "the cut falls in the room on both sides");
        Check(review.alerts.size() == 1 && review.alerts[0].finding.find(
                                               "jumps by") != std::string::npos,
              "the finding says by how much");

        // The same room on both sides: nothing.
        JunctionSoundMeasures same;
        MeasureJunctionSound(Sound(2.0, 0.00316, 0.00316, 1.0), 48000, Window(),
                             same, error);
        review.sound = same;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(review.alerts.empty(), "one room: no alert");
        // A threshold is a declared value: under it, nothing.
        review.sound = sound;
        CutReviewThresholds lenient;
        lenient.ambience_jump_db = 25;
        JudgeJunction(lenient, review);
        Check(review.alerts.empty(), "a 25 dB threshold lets 20 dB pass");
    });

    Case("a sound cut live, and a voice that jumps", [] {
        // Room at -50 dBFS on both sides; a tone at -9 dBFS crossing the cut
        // from 0.6 s to 1.3 s: both edges sit in it.
        const std::vector<float> samples =
            Sound(2.0, 0.00316, 0.00316, 1.0, 0.5, 0.6, 1.3);
        JunctionSoundMeasures sound;
        std::string error;
        MeasureJunctionSound(samples, 48000, Window(), sound, error);
        JunctionReview review;
        review.junction = SoundAndPicture();
        review.sound = sound;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(HasAlert(review, "sound_cut_live"),
              "the tone runs through the cut: sound_cut_live (edges " +
                  std::to_string(sound.edge_before_dbfs.value_or(0)) + ", " +
                  std::to_string(sound.edge_after_dbfs.value_or(0)) + ")");
        Check(!HasAlert(review, "ambience_jump"), "one room either side");
        Check(!HasAlert(review, "voice_jump"),
              "one tone either side: no voice jump");

        // The tone stops 100 ms before the cut and another, 10 dB louder,
        // starts 100 ms after it: the voice jumps, the cut is in the room.
        std::vector<float> two =
            Sound(2.0, 0.00316, 0.00316, 1.0, 0.1, 0.4, 0.9);
        const std::vector<float> loud =
            Sound(2.0, 0.0, 0.0, 1.0, 0.316, 1.1, 1.6);
        for (size_t index = 0; index < two.size(); ++index)
            two[index] += loud[index];
        MeasureJunctionSound(two, 48000, Window(), sound, error);
        review.sound = sound;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(HasAlert(review, "voice_jump"),
              "a voice 10 dB louder after the cut: voice_jump (" +
                  std::to_string(sound.voice_before_dbfs.value_or(0)) + " -> " +
                  std::to_string(sound.voice_after_dbfs.value_or(0)) + ")");
        Check(!HasAlert(review, "sound_cut_live"),
              "the cut itself falls in the room");

        // A through edit raises nothing, whatever it measures.
        review.junction.sound_continuous = true;
        review.junction.picture = false;
        review.sound = sound;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(review.alerts.empty(), "a continuous cut raises no alert");
    });

    Case("a side too short has no ambience", [] {
        CutInspectionPlan plan = Window();
        plan.first = 45;
        JunctionSoundMeasures sound;
        std::string error;
        Check(MeasureJunctionSound(Sound(1.2, 0.01, 0.01, 0.2), 48000, plan,
                                   sound, error),
              "measured: " + error);
        Check(!sound.ambience_before_dbfs && sound.ambience_after_dbfs &&
                  sound.edge_before_dbfs,
              "200 ms before the cut: an edge, no ambience");
        Check(!MeasureJunctionSound(Sound(0.5, 0.01, 0.01, 0.2), 48000,
                                    Window(), sound, error),
              "a mix shorter than the window is refused");
    });

    Case("a jump cut, two shots, and a movement interrupted", [] {
        CutInspectionPlan plan = Window();
        JunctionPictureMeasures picture;
        std::string error;
        // One framing: a block moves a little across the cut.
        const std::vector<std::vector<uint8_t>> jump = {
            Plane(0, 200, 10), Plane(0, 200, 10), Plane(0, 200, 13),
            Plane(0, 200, 13)};
        Check(MeasureJunctionPicture(jump, 64, 36, plan, picture, error),
              "measured: " + error);
        JunctionReview review;
        review.junction = SoundAndPicture();
        review.junction.sound = false;
        review.picture = picture;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(HasAlert(review, "jump_cut"),
              "one framing: jump_cut (difference " +
                  std::to_string(picture.difference_per_mille.value_or(-1)) +
                  ", histogram " +
                  std::to_string(picture.histogram_per_mille.value_or(-1)) +
                  ")");
        Check(!HasAlert(review, "motion_interrupted"),
              "still on both sides: no movement to interrupt");

        // Two shots: a gradient, then its negative.
        std::vector<uint8_t> negative = Plane(0);
        for (uint8_t& value : negative)
            value = static_cast<uint8_t>(255 - value);
        const std::vector<std::vector<uint8_t>> shots = {Plane(0), Plane(0),
                                                         negative, negative};
        MeasureJunctionPicture(shots, 64, 36, plan, picture, error);
        review.picture = picture;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(review.alerts.empty(),
              "two shots: no alert (difference " +
                  std::to_string(picture.difference_per_mille.value_or(-1)) +
                  ")");

        // The gradient pans before the cut and stands still after it.
        const std::vector<std::vector<uint8_t>> pan = {Plane(0), Plane(6),
                                                       Plane(12), Plane(12)};
        MeasureJunctionPicture(pan, 64, 36, plan, picture, error);
        review.picture = picture;
        JudgeJunction(CutReviewThresholds{}, review);
        Check(HasAlert(review, "motion_interrupted"),
              "a pan that stops dead: motion_interrupted (before " +
                  std::to_string(picture.motion_before_per_mille.value_or(-1)) +
                  ", after " +
                  std::to_string(picture.motion_after_per_mille.value_or(-1)) +
                  ")");

        Check(!MeasureJunctionPicture({Plane(0)}, 64, 36, plan, picture, error),
              "planes that do not match the window are refused");
    });

    Case("the review's loop, bands first for the alerts", [] {
        const Document document = Montage();
        int calls = 0;
        // Junction 50 jumps in ambience; 100 and 150 render the same room.
        const CutInspectionRenderer render =
            [&](const CutInspectionPlan& plan,
                const CutInspectionRenderSettings& settings,
                CutInspectionResult& result, std::string&) {
                ++calls;
                Check(settings.cut_band && settings.keep_samples &&
                          settings.analysis_dimension > 0 &&
                          settings.strip_frames.size() == 6,
                      "the review asks for a band, the mix and the planes");
                const double seconds =
                    static_cast<double>(plan.end - plan.first) / 25.0;
                const double cut =
                    static_cast<double>(plan.cut - plan.first) / 25.0;
                result.samples = Sound(seconds, 0.00316,
                                       plan.cut == 50 ? 0.0316 : 0.00316, cut);
                result.audio.sample_rate = 48000;
                result.analysis_width = 64;
                result.analysis_height = 36;
                for (size_t index = 0; index < plan.shown.size(); ++index) {
                    std::vector<uint8_t> plane = Plane(0);
                    if (plan.shown[index] >= plan.cut)
                        for (uint8_t& value : plane)
                            value = static_cast<uint8_t>(255 - value);
                    result.analysis_planes.push_back(plane);
                }
                result.words.expected_unavailable = "no transcript";
                result.jpeg = "band-" + std::to_string(plan.cut);
                return true;
            };
        CutReviewRequest request;
        request.max_bands = 2;
        CutReviewReport report;
        std::vector<std::string> bands;
        std::string error;
        Check(ReviewCuts(document, request, render, report, bands, error),
              "reviewed: " + error);
        Check(calls == 3 && report.reviews.size() == 3 &&
                  report.junctions_total == 3,
              "three junctions, three windows");
        Check(report.bands == std::vector<int64_t>({50, 100}) &&
                  bands == std::vector<std::string>({"band-50", "band-100"}),
              "the junction with an alert first, then the next, in order");
        Check(report.reviews.size() == 3 && report.reviews[0].band == 0 &&
                  report.reviews[2].band == std::nullopt,
              "each junction names its band");
        Check(report.reviews.size() == 3 &&
                  HasAlert(report.reviews[0], "ambience_jump") &&
                  report.reviews[1].alerts.empty() &&
                  report.reviews[2].alerts.empty(),
              "only the junction at 50 is flagged");
        const std::string text = SerializeCutReview(report);
        Check(text.find("\"with_alerts\":1") != std::string::npos &&
                  text.find("\"ambience_jump\":1") != std::string::npos &&
                  text.find("\"thresholds\"") != std::string::npos,
              "the report counts the alerts and declares its thresholds");

        request = CutReviewRequest{};
        request.frames = {100};
        Check(ReviewCuts(document, request, render, report, bands, error) &&
                  report.reviews.size() == 1,
              "a junction named by its frame: " + error);
        request.frames = {60};
        Check(!ReviewCuts(document, request, render, report, bands, error) &&
                  error.find("no junction") != std::string::npos,
              "a frame without a junction is refused by name");
        request = CutReviewRequest{};
        request.images_per_side = 5;
        Check(!ReviewCuts(document, request, render, report, bands, error),
              "five pictures a side are refused");
    });

    if (failures == 0) std::cout << "All cut review tests passed\n";
    return failures == 0 ? 0 : 1;
}
