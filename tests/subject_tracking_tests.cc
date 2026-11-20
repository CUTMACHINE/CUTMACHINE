// CA1, CA2 -- docs/roadmap/CADRAGE-2026-10.md. The operator's rule, on
// synthetic trajectories: a dead zone that holds the frame, moves bounded in
// speed and acceleration that do not overshoot, gaps bridged or held, one
// subject followed among several faces, the frame never uncovered -- and the
// operations it lays on a clip, applied, undone and replayed byte for byte
// without any face finder.
//
// Portable on purpose: no AppKit, no Metal, no FFmpeg, no Vision.

#include "ClipAnimation.h"
#include "EditLog.h"
#include "FaceFinder.h"
#include "Operations.h"
#include "SubjectTracking.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

constexpr int64_t kGrid = kAnimationSampleDenominator;
const MediaRate kRate{25, 1};

// A face centred on (cx, cy), `w` wide, as fractions of the picture.
ColorMeasureRegion Face(double cx, double cy, double w = 0.1, double h = 0.18) {
    ColorMeasureRegion region;
    region.present = true;
    const auto part = [](double value) {
        return EffectParamValue{
            static_cast<int32_t>(std::lround(value * 10000)), 10000};
    };
    region.x = part(cx - w / 2);
    region.y = part(cy - h / 2);
    region.width = part(w);
    region.height = part(h);
    return region;
}

// A 16:9 rush filled into a 9:16 frame: the picture overruns the frame
// across (3.16 frame widths), not down.
SubjectTrackingInput VerticalInput(int64_t frames) {
    SubjectTrackingInput input;
    input.source_width = 1920;
    input.source_height = 1080;
    input.frame_width = 1080;
    input.frame_height = 1920;
    input.framing.mode = FramingMode::Fill;
    input.rate = kRate;
    input.frame_count = frames;
    return input;
}

// One face at every `every`-th frame, centred where `place(frame)` says.
template <typename Place>
std::vector<SubjectSample> Samples(int64_t frames, int64_t every, Place place) {
    std::vector<SubjectSample> samples;
    for (int64_t frame : SubjectSampleFrames(frames, every)) {
        SubjectSample sample;
        sample.time = RationalTime(frame, 25);
        const double x = place(frame);
        if (x >= 0.0) sample.faces.push_back(Face(x, 0.4));
        samples.push_back(sample);
    }
    return samples;
}

SubjectTrackingResult Run(const SubjectTrackingInput& input,
                          const SubjectTrackingSettings& settings = {}) {
    SubjectTrackingResult result;
    std::string error;
    if (!ComputeSubjectTracking(input, settings, result, error))
        throw std::runtime_error("tracking failed: " + error);
    return result;
}

// Where position p puts a source point u, as a fraction of the frame width:
// 1/2 + p + span (u - 1/2), span = 1920 * (1920/1080) / 1080.
double OnFrame(int64_t position, double u) {
    const double span = 1920.0 * (1920.0 / 1080.0) / 1080.0;
    return 0.5 + static_cast<double>(position) / kGrid + span * (u - 0.5);
}

// The speed and acceleration bounds hold on the quantised path, give or
// take the quantisation (one grid step per frame on each difference).
void CheckDynamics(const std::vector<int64_t>& path,
                   const SubjectTrackingSettings& settings,
                   const std::string& label) {
    const double dt = 1.0 / 25.0;
    const double speed =
        static_cast<double>(settings.max_speed.num) / settings.max_speed.den;
    const double acceleration =
        static_cast<double>(settings.max_acceleration.num) /
        settings.max_acceleration.den;
    const double speedStep = speed * dt * kGrid + 1.0;
    const double accelerationStep = acceleration * dt * dt * kGrid + 2.0;
    for (size_t frame = 1; frame < path.size(); ++frame) {
        const double step = static_cast<double>(path[frame] - path[frame - 1]);
        Check(std::fabs(step) <= speedStep,
              label + ": frame " + std::to_string(frame) + " moves " +
                  std::to_string(step) + " > " + std::to_string(speedStep));
        if (frame >= 2) {
            const double change =
                step - static_cast<double>(path[frame - 1] - path[frame - 2]);
            Check(std::fabs(change) <= accelerationStep,
                  label + ": frame " + std::to_string(frame) +
                      " accelerates by " + std::to_string(change));
        }
    }
}

int64_t Count(const std::vector<int64_t>& path, int64_t value) {
    int64_t count = 0;
    for (int64_t item : path) count += item == value;
    return count;
}

// A document holding one 1920x1080 rush and one ten-second clip of it, in
// a 1080x1920 sequence at 25 i/s.
Document VerticalDocument() {
    Document document;
    document.sources = {
        {"01KCA000000000000000000S01", "A.MP4", {25, 1}, {1000, 25}}};
    LibraryMedia media;
    media.id = "01KCA000000000000000000S01";
    media.path = "A.MP4";
    media.filename = "A.MP4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = 1920;
    media.height = 1080;
    media.rate = {25, 1};
    media.duration = {1000, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    document.sequence.frame_rate = {25, 1};
    document.sequence.tracks = {{"01KCA000000000000000000T01",
                                 "video",
                                 0,
                                 {{"01KCA000000000000000000C01",
                                   "01KCA000000000000000000S01",
                                   {100, 25},
                                   {250, 25},
                                   {0, 25}}}}};
    return document;
}

std::function<Ulid()> Ids() {
    auto counter = std::make_shared<int>(0);
    return [counter]() {
        const std::string number = std::to_string(++*counter);
        return Ulid("01KCA00000000000000000" +
                    std::string(4 - number.size(), '0') + number);
    };
}

}  // namespace

int main() {
    Test("a subject that stays inside the dead zone never moves the frame", [] {
        SubjectTrackingInput input = VerticalInput(250);
        // A face that sways 1.5 % of the source either side of 0.4 --
        // 47 px of frame on a 1080 frame, inside the 108 px zone --
        // plus detector jitter.
        input.samples = Samples(250, 5, [](int64_t frame) {
            return 0.4 + 0.015 * std::sin(frame * 0.3) +
                   ((frame / 5) % 2 ? 0.003 : -0.003);
        });
        const SubjectTrackingResult result = Run(input);
        Check(!result.x.animated, "the frame stays still across");
        Check(result.x.moves == 0, "and makes no move");
        Check(result.x.keyframes.empty(), "so it carries no key");
        Check(!result.y.animated && result.y.room.num == 0,
              "a 16:9 rush filled in 9:16 has no room down");
        // Composed on the subject from the first frame: the face sits
        // on the target, within the sway.
        Check(std::fabs(OnFrame(result.x.path.front(), 0.4) - 0.5) < 0.06,
              "the still frame is composed on the subject");
    });

    Test("a subject that walks is followed by bounded, deliberate moves", [] {
        SubjectTrackingInput input = VerticalInput(250);
        // From 0.3 to 0.7 of the source over four seconds, then still.
        input.samples = Samples(250, 5, [](int64_t frame) {
            const double t = std::min(frame / 100.0, 1.0);
            return 0.3 + 0.4 * t;
        });
        SubjectTrackingSettings settings;
        const SubjectTrackingResult result = Run(input, settings);
        Check(result.x.animated, "the frame moves");
        Check(result.x.moves >= 1, "in at least one move");
        Check(result.x.keyframes.size() >= 2 && result.x.keyframes.size() < 60,
              "carried by a handful of keys, not one per frame: " +
                  std::to_string(result.x.keyframes.size()));
        CheckDynamics(result.x.path, settings, "walk");
        std::cout << "walk: " << result.x.keyframes.size() << " keys, "
                  << result.x.moves << " move(s), subject at "
                  << OnFrame(result.x.path.back(), 0.7)
                  << " of the frame at the end\n";
        // The subject goes right, the picture goes left: never back.
        for (size_t frame = 1; frame < result.x.path.size(); ++frame)
            Check(result.x.path[frame] <= result.x.path[frame - 1],
                  "the frame never turns back, frame " + std::to_string(frame));
        // Once the subject has stopped, the frame has brought it back
        // inside the dead zone -- past its place by what braking at the
        // acceleration bound costs, and left there rather than brought back.
        Check(std::fabs(OnFrame(result.x.path.back(), 0.7) - 0.5) <= 0.1,
              "and ends with the subject inside the dead zone: " +
                  std::to_string(OnFrame(result.x.path.back(), 0.7)));

        // A subject that moves, then stays put long enough to be reached,
        // is put back on its place, within the settle band.
        input.samples = Samples(
            250, 5, [](int64_t frame) { return frame < 25 ? 0.4 : 0.6; });
        const SubjectTrackingResult step = Run(input, settings);
        CheckDynamics(step.x.path, settings, "step");
        Check(step.x.moves == 1, "one move for one change of place");
        Check(std::fabs(OnFrame(step.x.path.back(), 0.6) - 0.5) <= 0.02 + 1e-4,
              "which ends with the subject on its place: " +
                  std::to_string(OnFrame(step.x.path.back(), 0.6)));
        // The last second is still: a move ends, it does not creep.
        Check(Count(result.x.path, result.x.path.back()) >= 25,
              "the end of the clip is still");
    });

    Test("the keys sample back to the path, within the tolerance", [] {
        Document document = VerticalDocument();
        DocumentClip& clip = document.sequence.tracks[0].clips[0];
        SubjectTrackingInput input = VerticalInput(250);
        input.samples = Samples(
            250, 5, [](int64_t frame) { return frame < 125 ? 0.35 : 0.65; });
        const SubjectTrackingResult result = Run(input);
        Check(result.x.animated, "the jump of the subject is followed");
        clip.framing.mode = FramingMode::Fill;
        ClipParameterAnimation track;
        track.parameter = ClipAnimatedParameter::FramingPositionX;
        track.keyframes = result.x.keyframes;
        auto ids = Ids();
        for (ClipKeyframe& key : track.keyframes) key.id = ids();
        clip.animation = {track};
        for (int64_t frame = 0; frame < 250; ++frame) {
            ClipSampledParameters sampled;
            std::string error;
            Check(SampleClipParameters(clip, frame, kRate, sampled, error),
                  "frame samples: " + error);
            const int64_t value = static_cast<int64_t>(std::llround(
                static_cast<double>(sampled.framing.position_x.num) * kGrid /
                sampled.framing.position_x.den));
            Check(
                std::llabs(value - result.x.path[static_cast<size_t>(frame)]) <=
                    10 + 1,
                "frame " + std::to_string(frame) + " samples " +
                    std::to_string(value) + " for a path of " +
                    std::to_string(result.x.path[frame]));
        }
    });

    Test("one false detection far away moves nothing", [] {
        SubjectTrackingInput input = VerticalInput(125);
        input.samples = Samples(
            125, 5, [](int64_t frame) { return frame == 50 ? 0.9 : 0.5; });
        const SubjectTrackingResult result = Run(input);
        Check(!result.x.animated && result.x.moves == 0,
              "a face that jumps further than a person moves is ignored");
        Check(result.samples_with_subject == result.samples - 1,
              "and counted as a sample without the subject");
    });

    Test("a short hole is bridged, a long one holds the frame", [] {
        // Two samples lost, the subject where it was: nothing happens.
        SubjectTrackingInput input = VerticalInput(125);
        input.samples = Samples(125, 5, [](int64_t frame) {
            return frame == 50 || frame == 55 ? -1.0 : 0.45;
        });
        SubjectTrackingResult result = Run(input);
        Check(!result.x.animated, "a short hole does not move the frame");
        Check(result.reacquired == 0, "and the subject is not chosen again");

        // Three seconds lost; the subject reappears on the other side.
        input = VerticalInput(250);
        input.samples = Samples(250, 5, [](int64_t frame) {
            if (frame < 50) return 0.3;
            if (frame < 125) return -1.0;
            return 0.7;
        });
        result = Run(input);
        Check(result.reacquired == 1, "the subject is chosen again");
        const int64_t start = result.x.path.front();
        for (int64_t frame = 0; frame < 125; ++frame)
            Check(result.x.path[static_cast<size_t>(frame)] == start,
                  "the frame holds through the hole, frame " +
                      std::to_string(frame));
        Check(result.x.path.back() < start && result.x.moves == 1,
              "then makes one move to the subject");
    });

    Test("among several faces, one subject is chosen and kept", [] {
        // A small face on the left, a large one on the right.
        const auto pair = [](int64_t frames, double passer) {
            std::vector<SubjectSample> samples;
            for (int64_t frame : SubjectSampleFrames(frames, 5)) {
                SubjectSample sample;
                sample.time = RationalTime(frame, 25);
                sample.faces = {Face(0.3, 0.4, 0.08), Face(0.65, 0.4, 0.12)};
                // Midway, a much larger face crosses near the left one.
                if (passer > 0 && frame >= 60 && frame < 80)
                    sample.faces.push_back(Face(passer, 0.4, 0.2));
                samples.push_back(sample);
            }
            return samples;
        };
        SubjectTrackingInput input = VerticalInput(150);
        input.samples = pair(150, 0.0);
        SubjectTrackingResult result = Run(input);
        Check(std::fabs(OnFrame(result.x.path.front(), 0.65) - 0.5) < 0.01,
              "the largest face is followed by default");

        SubjectTrackingSettings hinted;
        hinted.has_hint = true;
        hinted.hint_x = {1, 4};
        hinted.hint_y = {1, 2};
        input.samples = pair(150, 0.33);
        result = Run(input, hinted);
        Check(std::fabs(OnFrame(result.x.path.front(), 0.3) - 0.5) < 0.01,
              "the face nearest the hint is followed when one is given");
        Check(!result.x.animated,
              "and a larger face crossing near it does not take the frame");
    });

    Test("a subject past the room is held at the edge, never uncovered", [] {
        SubjectTrackingInput input = VerticalInput(100);
        input.samples = Samples(100, 5, [](int64_t) { return 0.02; });
        const SubjectTrackingResult result = Run(input);
        Check(result.x.frames_at_edge == 100,
              "every frame is counted at the edge");
        Check(result.x.path.front() * result.x.room.den ==
                  static_cast<int64_t>(result.x.room.num) * kGrid,
              "the frame sits on its room");
    });

    Test("the refusals are named", [] {
        SubjectTrackingInput input = VerticalInput(50);
        input.samples = Samples(50, 5, [](int64_t) { return -1.0; });
        SubjectTrackingResult result;
        std::string error;
        Check(!ComputeSubjectTracking(input, {}, result, error) &&
                  error.find("no face found") != std::string::npos,
              "no face: " + error);
        input.samples = Samples(50, 5, [](int64_t) { return 0.5; });
        input.framing.mode = FramingMode::Fit;
        Check(!ComputeSubjectTracking(input, {}, result, error) &&
                  error.find("fill") != std::string::npos,
              "a fitted framing: " + error);
        input.framing.mode = FramingMode::Fill;
        input.source_width = 1080;
        input.source_height = 1920;
        Check(!ComputeSubjectTracking(input, {}, result, error) &&
                  error.find("nothing to pan") != std::string::npos,
              "a picture that does not overrun: " + error);
        SubjectTrackingSettings settings;
        settings.settle = {1, 5};
        Check(!SubjectTrackingSettingsAreValid(settings, error),
              "a settle wider than the dead zone");
        settings = {};
        settings.max_speed = {0, 1};
        Check(!SubjectTrackingSettingsAreValid(settings, error),
              "a frame that may not move");
        input = VerticalInput(50);
        input.samples = Samples(50, 5, [](int64_t) { return 0.5; });
        std::swap(input.samples[1], input.samples[2]);
        Check(!ComputeSubjectTracking(input, {}, result, error) &&
                  error.find("increasing") != std::string::npos,
              "samples out of order: " + error);
    });

    Test("the sampled frames include the last one", [] {
        Check(SubjectSampleFrames(11, 5) == std::vector<int64_t>({0, 5, 10}),
              "11 frames every 5");
        Check(
            SubjectSampleFrames(12, 5) == std::vector<int64_t>({0, 5, 10, 11}),
            "12 frames every 5, plus the last");
        Check(SubjectSampleFrames(0, 5).empty(), "no frame, no sample");
    });

    Test("the followed framing is two operations, undone byte for byte", [] {
        Document document = VerticalDocument();
        const Ulid clipId = "01KCA000000000000000000C01";
        // An opacity fade the clip already carries is kept.
        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        ClipKeyframe a;
        a.id = "01KCA000000000000000000K01";
        a.time = {0, 25};
        a.value = {1, 1};
        ClipKeyframe b;
        b.id = "01KCA000000000000000000K02";
        b.time = {25, 25};
        b.value = {1, 2};
        fade.keyframes = {a, b};
        document.sequence.tracks[0].clips[0].animation = {fade};
        const std::string original = document.SaveToString();

        const std::vector<SubjectSample> samples =
            Samples(250, 5, [](int64_t frame) {
                const double t = std::min(frame / 100.0, 1.0);
                return 0.3 + 0.4 * t;
            });
        std::vector<Operation> operations;
        SubjectTrackingResult result;
        bool modeChanged = false;
        std::string error;
        Check(BuildSubjectTrackingOperations(document, clipId, samples, {},
                                             std::nullopt, Ids(), operations,
                                             result, modeChanged, error),
              "the operations build: " + error);
        Check(modeChanged, "a fitted clip is put in fill");
        Check(operations.size() == 2, "a framing and an animation");

        EditLog log;
        EditError editError = EditError::None;
        const Operation batch = BatchOperation{"cadrage suivi", operations};
        Check(log.Apply(document, batch, editError, error),
              "the batch applies: " + error);
        const DocumentClip* clip = document.FindClip(clipId);
        Check(clip->framing.mode == FramingMode::Fill, "the clip fills");
        Check(ClipAnimates(*clip, ClipAnimatedParameter::FramingPositionX),
              "its position across carries keys");
        Check(ClipAnimates(*clip, ClipAnimatedParameter::Opacity),
              "and its fade is still there");
        const std::string changed = document.SaveToString();

        // The journal line replays on the document as it was, with no face
        // finder anywhere: the keys are in the operation.
        const std::string json = SerializeOperation(batch);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, editError, error) &&
                  SerializeOperation(parsed) == json,
              "the batch round-trips canonically: " + error);
        Check(log.Undo(document, editError, error) &&
                  document.SaveToString() == original,
              "undo restores byte-identical JSON");
        Check(log.Redo(document, editError, error) &&
                  document.SaveToString() == changed,
              "redo restores byte-identical JSON");
        Document replay;
        Check(Document::LoadFromString(original, replay, error),
              "the original reloads");
        EditLog replayLog;
        Check(replayLog.Apply(replay, parsed, editError, error) &&
                  replay.SaveToString() == changed,
              "the parsed line replays to the same bytes");
        Document reloaded;
        Check(Document::LoadFromString(changed, reloaded, error) &&
                  reloaded.SaveToString() == changed,
              "the followed document reloads to the same bytes: " + error);

        // Every frame resolves covered, through the renderers' own call.
        // Undo and redo rebuilt the clips: `clip` points into the old ones.
        const DocumentClip* followed = document.FindClip(clipId);
        Check(followed != nullptr, "the followed clip is found again");
        for (int64_t frame = 0; followed != nullptr && frame < 250;
             frame += 7) {
            ClipFramingGeometry geometry;
            EffectParamValue opacity;
            Check(ResolveClipFramingAtFrame(document, *followed, frame, 1080,
                                            1920, geometry, opacity, error) &&
                      geometry.uncovered_left == 0 &&
                      geometry.uncovered_right == 0,
                  "frame " + std::to_string(frame) + " is covered");
        }
    });

    Test("a clip whose scale is animated is refused by name", [] {
        Document document = VerticalDocument();
        DocumentClip& clip = document.sequence.tracks[0].clips[0];
        ClipParameterAnimation zoom;
        zoom.parameter = ClipAnimatedParameter::FramingScale;
        ClipKeyframe a;
        a.id = "01KCA000000000000000000K11";
        a.time = {0, 25};
        a.value = {1, 1};
        ClipKeyframe b;
        b.id = "01KCA000000000000000000K12";
        b.time = {25, 25};
        b.value = {6, 5};
        zoom.keyframes = {a, b};
        clip.animation = {zoom};
        std::vector<Operation> operations;
        SubjectTrackingResult result;
        bool modeChanged = false;
        std::string error;
        Check(
            !BuildSubjectTrackingOperations(
                document, clip.id, Samples(250, 5, [](int64_t) { return 0.5; }),
                {}, std::nullopt, Ids(), operations, result, modeChanged,
                error) &&
                error.find("animates its scale") != std::string::npos,
            "refused: " + error);
    });

    Test("without a face finder, sampling refuses by name", [] {
        SetFaceFinder(nullptr);
        Document document = VerticalDocument();
        std::vector<SubjectSample> samples;
        std::string error;
        Check(!SampleClipFaces(document, ".", "01KCA000000000000000000C01", 5,
                               512, "", samples, error) &&
                  error.find("no face finder") != std::string::npos,
              "refused: " + error);
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
