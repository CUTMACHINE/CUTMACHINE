// PA7 -- docs/roadmap/PARITE-2026-11.md. A handheld shot steadied: the rule
// on synthetic paths (a pan kept, a tremble removed, the zoom it costs, the
// edge of the room), the operations it lays on a clip, applied and undone
// byte for byte, and the movement read back from a rush whose every shift
// is known -- a still texture cropped at a place that changes with the
// frame.

#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "EditLog.h"
#include "Operations.h"
#include "Stabilization.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
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

double Fraction(const EffectParamValue& value) {
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

// A deterministic tremble in [-1, 1]: no two runs differ, no library's
// generator is asked.
double Tremble(int64_t frame, int64_t salt) {
    uint64_t state = static_cast<uint64_t>(frame) * 6364136223846793005ULL +
                     static_cast<uint64_t>(salt) * 1442695040888963407ULL + 1;
    state ^= state >> 33;
    state *= 0xff51afd7ed558ccdULL;
    state ^= state >> 33;
    return (static_cast<double>(state % 20001) / 10000.0) - 1.0;
}

// A 16:9 rush in a 16:9 sequence, filled: the room is what the scale adds.
StabilizationInput Landscape(int64_t frames) {
    StabilizationInput input;
    input.clip.source_width = 1920;
    input.clip.source_height = 1080;
    input.clip.frame_width = 1920;
    input.clip.frame_height = 1080;
    input.clip.framing.mode = FramingMode::Fill;
    input.clip.rate = {25, 1};
    input.clip.frame_count = frames;
    input.motion.resize(static_cast<size_t>(frames));
    return input;
}

// A pan of `pan` per frame to the right with a tremble of `amplitude`
// around it, both fractions of the source; the picture's place at each
// frame is returned in `place`.
void Shake(StabilizationInput& input, double pan, double amplitude,
           std::vector<double>& placeX, std::vector<double>& placeY) {
    const size_t count = input.motion.size();
    placeX.assign(count, 0.0);
    placeY.assign(count, 0.0);
    for (size_t frame = 0; frame < count; ++frame) {
        placeX[frame] = pan * static_cast<double>(frame) +
                        amplitude * Tremble(static_cast<int64_t>(frame), 1);
        placeY[frame] = amplitude * Tremble(static_cast<int64_t>(frame), 2);
    }
    for (size_t frame = 1; frame < count; ++frame) {
        input.motion[frame].measured = true;
        input.motion[frame].translation_x = placeX[frame] - placeX[frame - 1];
        input.motion[frame].translation_y = placeY[frame] - placeY[frame - 1];
    }
}

// How rough a path is: the root mean square of its second difference,
// which a steady pan leaves at zero and a tremble does not.
double Roughness(const std::vector<double>& path) {
    double sum = 0.0;
    for (size_t frame = 1; frame + 1 < path.size(); ++frame) {
        const double second =
            path[frame + 1] - 2.0 * path[frame] + path[frame - 1];
        sum += second * second;
    }
    return std::sqrt(sum / static_cast<double>(path.size() - 2));
}

// The picture's place once the keys are laid, in fractions of the source:
// where it drifted, plus what the framing moved it by.
std::vector<double> Steadied(const std::vector<double>& place,
                             const SubjectTrackingAxis& axis, double scale) {
    std::vector<double> steadied(place.size());
    for (size_t frame = 0; frame < place.size(); ++frame) {
        const double moved = static_cast<double>(axis.path[frame]) /
                             static_cast<double>(kAnimationSampleDenominator);
        // A 16:9 picture filling a 16:9 frame: a frame at scale 1.
        steadied[frame] = place[frame] + moved / scale;
    }
    return steadied;
}

Document LandscapeDocument(const std::string& path = "A.MP4",
                           int64_t frames = 100) {
    Document document;
    const Ulid source = "01KPA700000000000000000S01";
    document.sources = {{source, path, {25, 1}, {frames, 25}}};
    LibraryMedia media;
    media.id = source;
    media.path = path;
    media.filename = "A.MP4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = 1280;
    media.height = 720;
    media.rate = {25, 1};
    media.duration = {frames, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sequence.width = 1280;
    document.sequence.height = 720;
    document.sequence.frame_rate = {25, 1};
    document.sequence.tracks = {{"01KPA700000000000000000T01",
                                 "video",
                                 0,
                                 {{"01KPA700000000000000000C01",
                                   source,
                                   {0, 25},
                                   {frames, 25},
                                   {0, 25}}}}};
    return document;
}

const Ulid kClip = "01KPA700000000000000000C01";

std::function<Ulid()> Ids() {
    auto counter = std::make_shared<int>(0);
    return [counter]() {
        const std::string number = std::to_string(++*counter);
        return Ulid("01KPA70000000000000000" +
                    std::string(4 - number.size(), '0') + number);
    };
}

std::string Quote(const std::string& text) { return "'" + text + "'"; }

}  // namespace

int main() {
    Test("a tremble is removed and a pan is kept", [] {
        StabilizationInput input = Landscape(150);
        std::vector<double> placeX;
        std::vector<double> placeY;
        // A pan of a frame's width in 100 s, and a tremble of 4 px either
        // way on a 1920 picture.
        Shake(input, 0.0004, 4.0 / 1920.0, placeX, placeY);
        StabilizationResult result;
        std::string error;
        Check(ComputeStabilization(input, {}, result, error),
              "the path is steadied: " + error);
        Check(result.frames == 150 && result.frames_unmeasured == 0,
              "every frame was measured");
        const double scale = Fraction(result.scale);
        Check(result.scale_chosen && scale > 1.0 && scale <= 1.25 &&
                  scale >= result.scale_needed,
              "the smallest scale that holds the corrections is chosen: " +
                  std::to_string(scale) + " for " +
                  std::to_string(result.scale_needed));
        Check(scale < 1.03, "and a tremble of 4 px costs under 3 % of zoom: " +
                                std::to_string(scale));
        Check(result.x.frames_at_edge == 0 && result.y.frames_at_edge == 0,
              "no correction reaches the edge of the room");
        Check(result.x.animated && result.y.animated &&
                  result.x.keyframes.size() > 50,
              "both axes carry keys, close to one a frame");
        Check(result.shake_before_rms > 2.0 && result.shake_before_rms < 8.0 &&
                  result.shake_after_rms < 0.6,
              "the picture strayed " + std::to_string(result.shake_before_rms) +
                  " px from its path and strays " +
                  std::to_string(result.shake_after_rms) + " px behind keys");
        const std::vector<double> steadyX = Steadied(placeX, result.x, scale);
        const std::vector<double> steadyY = Steadied(placeY, result.y, scale);
        Check(Roughness(steadyX) * 12.0 < Roughness(placeX) &&
                  Roughness(steadyY) * 12.0 < Roughness(placeY),
              "the steadied path is over twelve times smoother: " +
                  std::to_string(Roughness(placeX) / Roughness(steadyX)));
        // The pan is still there: between the first and last thirds the
        // picture travels what the pan travels.
        const double travelled = steadyX[120] - steadyX[30];
        Check(std::fabs(travelled - 0.0004 * 90.0) < 0.0015,
              "the pan is kept: " + std::to_string(travelled) + " for " +
                  std::to_string(0.0004 * 90.0));
        for (size_t frame = 0; frame < result.x.path.size(); ++frame) {
            const int64_t room = static_cast<int64_t>(
                Fraction(result.x.room) * kAnimationSampleDenominator + 0.5);
            if (std::llabs(result.x.path[frame]) > room) {
                Check(false, "frame " + std::to_string(frame) +
                                 " leaves the room across");
                break;
            }
        }
    });

    Test("a longer smoothing steadies more and costs more zoom", [] {
        StabilizationInput input = Landscape(200);
        std::vector<double> placeX;
        std::vector<double> placeY;
        Shake(input, 0.0, 6.0 / 1920.0, placeX, placeY);
        // A slow sway on top, two seconds long: a short smoothing follows
        // it, a long one removes it.
        for (size_t frame = 0; frame < placeX.size(); ++frame)
            placeX[frame] += 0.006 * std::sin(frame * 2.0 * 3.14159265 / 50.0);
        for (size_t frame = 1; frame < placeX.size(); ++frame)
            input.motion[frame].translation_x =
                placeX[frame] - placeX[frame - 1];
        StabilizationSettings brief;
        brief.smoothing = {1, 5};
        StabilizationSettings lasting;
        lasting.smoothing = {2, 1};
        StabilizationResult quick;
        StabilizationResult slow;
        std::string error;
        Check(ComputeStabilization(input, brief, quick, error) &&
                  ComputeStabilization(input, lasting, slow, error),
              "both smoothings compute: " + error);
        Check(Fraction(slow.scale) > Fraction(quick.scale) &&
                  slow.shake_before_rms > quick.shake_before_rms,
              "two seconds remove the sway too, for more zoom: " +
                  std::to_string(Fraction(quick.scale)) + " then " +
                  std::to_string(Fraction(slow.scale)));
    });

    Test("past max_scale the corrections stop at the edge", [] {
        StabilizationInput input = Landscape(120);
        std::vector<double> placeX;
        std::vector<double> placeY;
        Shake(input, 0.0, 30.0 / 1920.0, placeX, placeY);
        StabilizationSettings tight;
        tight.max_scale = {101, 100};
        StabilizationResult result;
        std::string error;
        Check(ComputeStabilization(input, tight, result, error),
              "a tight zoom still steadies: " + error);
        Check(result.scale.num == 101 && result.scale.den == 100 &&
                  result.scale_needed > 1.02,
              "the scale stops at max_scale, short of what was needed: " +
                  std::to_string(result.scale_needed));
        Check(result.x.frames_at_edge > 0 && result.shake_after_rms > 1.0,
              "frames are held at the edge, and counted: " +
                  std::to_string(result.x.frames_at_edge));
        const int64_t room = static_cast<int64_t>(
            Fraction(result.x.room) * kAnimationSampleDenominator + 0.5);
        bool inside = true;
        for (const int64_t position : result.x.path)
            inside = inside && std::llabs(position) <= room;
        Check(inside, "no position leaves the room");

        // A scale given is taken as is, even too small.
        input.clip.framing.scale = {21, 20};
        input.scale_given = true;
        StabilizationResult given;
        Check(ComputeStabilization(input, {}, given, error) &&
                  given.scale.num == 21 && given.scale.den == 20 &&
                  !given.scale_chosen,
              "a scale given is not chosen again: " + error);
    });

    Test("what cannot be steadied is refused by name", [] {
        StabilizationResult result;
        std::string error;
        StabilizationInput input = Landscape(40);
        Check(!ComputeStabilization(input, {}, result, error) &&
                  error.find("no movement could be measured") !=
                      std::string::npos,
              "a clip with no measured movement: " + error);
        std::vector<double> placeX;
        std::vector<double> placeY;
        Shake(input, 0.0, 0.002, placeX, placeY);
        input.motion[7].measured = false;
        input.motion[8].measured = false;
        Check(ComputeStabilization(input, {}, result, error) &&
                  result.frames_unmeasured == 2,
              "frames the estimator missed are counted, not fatal: " + error);
        StabilizationInput shorter = input;
        shorter.motion.pop_back();
        Check(
            !ComputeStabilization(shorter, {}, result, error) &&
                error.find("39 samples for a clip of 40") != std::string::npos,
            "a motion of another length: " + error);
        StabilizationInput fitted = input;
        fitted.clip.framing.mode = FramingMode::Fit;
        Check(!ComputeStabilization(fitted, {}, result, error) &&
                  error.find("fill") != std::string::npos,
              "a framing that fits: " + error);
        StabilizationSettings none;
        none.smoothing = {0, 1};
        Check(!ComputeStabilization(input, none, result, error) &&
                  error.find("smoothing") != std::string::npos,
              "no smoothing: " + error);
        StabilizationSettings shrunk;
        shrunk.max_scale = {9, 10};
        Check(!ComputeStabilization(input, shrunk, result, error) &&
                  error.find("max_scale") != std::string::npos,
              "a max_scale under 1: " + error);
    });

    Test("the operations apply, cover every frame and undo", [] {
        Document document = LandscapeDocument();
        DocumentClip* clip = document.FindClip(kClip);
        // A position of its own and a fade: the first is the centre the
        // corrections move around, the second is kept.
        clip->framing.position_x = {1, 200};
        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        fade.keyframes = {{"01KPA700000000000000000K01", {0, 25}, {0, 1}},
                          {"01KPA700000000000000000K02", {10, 25}, {1, 1}}};
        clip->animation = {fade};
        std::string error;
        Check(document.Validate(error), "the fixture validates: " + error);
        const std::string original = document.SaveToString();

        StabilizationInput synthetic = Landscape(100);
        std::vector<double> placeX;
        std::vector<double> placeY;
        Shake(synthetic, 0.0003, 5.0 / 1280.0, placeX, placeY);
        std::vector<Operation> operations;
        StabilizationResult result;
        bool modeChanged = false;
        Check(BuildStabilizationOperations(document, kClip, synthetic.motion,
                                           {}, std::nullopt, Ids(), operations,
                                           result, modeChanged, error),
              "the operations build: " + error);
        Check(modeChanged && operations.size() == 2,
              "a fitted clip is put in fill: a framing and an animation");

        EditLog log;
        EditError editError = EditError::None;
        const Operation batch = BatchOperation{"stabilisation", operations};
        Check(log.Apply(document, batch, editError, error),
              "the batch applies: " + error);
        const DocumentClip* steadied = document.FindClip(kClip);
        Check(steadied->framing.mode == FramingMode::Fill &&
                  Fraction(steadied->framing.scale) > 1.0 &&
                  steadied->framing.scale.num == result.scale.num &&
                  steadied->framing.scale.den == result.scale.den,
              "the clip fills, enlarged by the scale chosen");
        Check(
            ClipAnimates(*steadied, ClipAnimatedParameter::FramingPositionX) &&
                ClipAnimates(*steadied,
                             ClipAnimatedParameter::FramingPositionY) &&
                ClipAnimates(*steadied, ClipAnimatedParameter::Opacity),
            "its position carries keys and its fade is still there");
        // The clip's own position is the centre: the keys average near it.
        double sum = 0.0;
        for (const int64_t position : result.x.path)
            sum += static_cast<double>(position);
        const double mean = sum / static_cast<double>(result.x.path.size()) /
                            kAnimationSampleDenominator;
        Check(std::fabs(mean - 0.005) < 0.002,
              "the corrections move around the clip's own position: " +
                  std::to_string(mean));
        // Every frame, through the call every renderer makes.
        bool covered = true;
        for (int64_t frame = 0; frame < 100 && covered; ++frame) {
            ClipFramingGeometry geometry;
            EffectParamValue opacity;
            covered =
                ResolveClipFramingAtFrame(document, *steadied, frame, 1280, 720,
                                          geometry, opacity, error) &&
                geometry.uncovered_left == 0 && geometry.uncovered_top == 0 &&
                geometry.uncovered_right == 0 && geometry.uncovered_bottom == 0;
        }
        Check(covered, "no frame leaves an edge uncovered: " + error);

        const std::string json = SerializeOperation(batch);
        Operation parsed = RemoveClipOperation{};
        Check(DeserializeOperation(json, parsed, editError, error) &&
                  SerializeOperation(parsed) == json,
              "the batch round-trips canonically: " + error);
        Check(log.Undo(document, editError, error) &&
                  document.SaveToString() == original,
              "undo restores byte-identical JSON: " + error);

        // What a steadied framing refuses, as a followed one does.
        Document scaled = document;
        ClipParameterAnimation zoom;
        zoom.parameter = ClipAnimatedParameter::FramingScale;
        zoom.keyframes = {{"01KPA700000000000000000K03", {0, 25}, {1, 1}},
                          {"01KPA700000000000000000K04", {50, 25}, {6, 5}}};
        scaled.FindClip(kClip)->animation.push_back(zoom);
        Check(!BuildStabilizationOperations(scaled, kClip, synthetic.motion, {},
                                            std::nullopt, Ids(), operations,
                                            result, modeChanged, error) &&
                  error.find("animates its scale") != std::string::npos,
              "a clip that animates its scale: " + error);
        Check(
            !BuildStabilizationOperations(
                document, "01KPA700000000000000000C99", synthetic.motion, {},
                std::nullopt, Ids(), operations, result, modeChanged, error) &&
                error.find("unknown clip_id") != std::string::npos,
            "an unknown clip: " + error);
    });

    Test("the movement of a rush is read back to the pixel", [] {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() /
            ("cutmachine-stabilization-" + GenerateUlid());
        std::filesystem::create_directories(directory);
        const std::string texture = (directory / "texture.png").string();
        const std::string shaky = (directory / "shaky.mp4").string();
        // A still texture with no period: noise, softened so that it
        // survives the reduction to the analysis picture.
        const std::string makeTexture =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi -i "
            "'nullsrc=s=1600x900,geq=lum=random(1)*255:cb=128:cr=128,"
            "gblur=sigma=6,lutyuv=y=clip((val-128)*6+128\\,0\\,255)' "
            "-frames:v 1 -y " +
            Quote(texture);
        Check(std::system(makeTexture.c_str()) == 0,
              "the texture is generated");
        // The window read moves with the frame: 160 + 24 sin, 90 + 14 cos,
        // plus a slow drift to the right. Whole pixels, so that the shift
        // between two frames is known exactly.
        const auto windowX = [](int64_t frame) {
            return 160 + static_cast<int64_t>(std::lround(
                             24.0 * std::sin(frame * 0.9) + 0.4 * frame));
        };
        const auto windowY = [](int64_t frame) {
            return 90 + static_cast<int64_t>(
                            std::lround(14.0 * std::cos(frame * 1.3)));
        };
        const std::string makeShaky =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -loop 1 -framerate 25 -i " +
            Quote(texture) +
            " -vf \"crop=1280:720:x='160+round(24*sin(n*0.9)+0.4*n)':"
            "y='90+round(14*cos(n*1.3))'\" -frames:v 60 -c:v libx264 "
            "-preset ultrafast -crf 12 -pix_fmt yuv420p -y " +
            Quote(shaky);
        Check(std::system(makeShaky.c_str()) == 0,
              "the shaky rush is generated");

        Document document = LandscapeDocument(shaky, 60);
        std::vector<StabilizationSample> motion;
        std::string error;
        const bool read = SampleClipMotion(document, directory.string(), kClip,
                                           FFMPEG_EXECUTABLE, motion, error);
        Check(read && motion.size() == 60, "the movement is read: " + error);
        if (read && motion.size() == 60) {
            int64_t measured = 0;
            double worst = 0.0;
            double sum = 0.0;
            for (int64_t frame = 1; frame < 60; ++frame) {
                const StabilizationSample& sample =
                    motion[static_cast<size_t>(frame)];
                if (!sample.measured) continue;
                ++measured;
                // The window moves right: the picture moves left.
                const double expectedX =
                    -static_cast<double>(windowX(frame) - windowX(frame - 1));
                const double expectedY =
                    -static_cast<double>(windowY(frame) - windowY(frame - 1));
                const double dx = sample.translation_x * 1280.0 - expectedX;
                const double dy = sample.translation_y * 720.0 - expectedY;
                const double distance = std::sqrt(dx * dx + dy * dy);
                worst = std::max(worst, distance);
                sum += distance * distance;
            }
            const double rms = measured > 0 ? std::sqrt(sum / measured) : 1e9;
            // Said on every run: the figure the reference quotes.
            std::cout << "read: rms " << rms << " px, worst " << worst
                      << " px of a 1280x720 rush\n";
            Check(measured == 59, "every pair of frames was measured: " +
                                      std::to_string(measured));
            Check(rms < 0.5 && worst < 1.5,
                  "each shift is read within a pixel of the rush: rms " +
                      std::to_string(rms) + " px, worst " +
                      std::to_string(worst) + " px");

            // And the whole of it: read, steadied, laid.
            std::vector<Operation> operations;
            StabilizationResult result;
            bool modeChanged = false;
            Check(BuildStabilizationOperations(document, kClip, motion, {},
                                               std::nullopt, Ids(), operations,
                                               result, modeChanged, error),
                  "the rush is steadied: " + error);
            std::cout << "steadied: " << result.shake_before_rms
                      << " px rms before, " << result.shake_after_rms
                      << " px left, scale " << Fraction(result.scale)
                      << ", keys " << result.x.keyframes.size() << " and "
                      << result.y.keyframes.size() << " for 60 frames\n";
            Check(result.shake_before_rms > 8.0 &&
                      result.shake_after_rms < 1.0 &&
                      Fraction(result.scale) > 1.02 &&
                      Fraction(result.scale) <= 1.25,
                  "a tremble of " + std::to_string(result.shake_before_rms) +
                      " px is left at " +
                      std::to_string(result.shake_after_rms) + " px, for " +
                      std::to_string(Fraction(result.scale)) + " of zoom");
        }

        // What is refused before a frame is decoded.
        Document slowed = document;
        slowed.FindClip(kClip)->speed = {1, 2};
        slowed.FindClip(kClip)->duration = {120, 25};
        Check(!SampleClipMotion(slowed, directory.string(), kClip,
                                FFMPEG_EXECUTABLE, motion, error) &&
                  error.find("speed other than 1/1") != std::string::npos,
              "a clip with a speed: " + error);
        Document other = document;
        other.sequence.frame_rate = {30, 1};
        Check(!SampleClipMotion(other, directory.string(), kClip,
                                FFMPEG_EXECUTABLE, motion, error) &&
                  error.find("cadence") != std::string::npos,
              "a rush at another cadence: " + error);
        Document absent =
            LandscapeDocument((directory / "nowhere.mp4").string(), 60);
        Check(!SampleClipMotion(absent, directory.string(), kClip,
                                FFMPEG_EXECUTABLE, motion, error),
              "a rush that is not there: " + error);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    });

    return failures == 0 ? 0 : 1;
}
