// REMOTION-PARITY-2026-09 REM2 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// Media and precompositions with real files and the real painter: the
// corpus scenes C12 (a video in a rounded box), C13 (a detached sound, its
// gain and fades) and C14 (one precomposition shown twice), the reader
// under them, and the surfaces that reach a project's media by ULID.
//
// The media are made by FFmpeg for each run, chosen so that a value read
// back names where it was read from: picture n is the colour (4n, 255 - 4n,
// 80), stored losslessly as RGB; sample n of the sound is n % 1000 / 1000,
// stored as 32-bit float. macOS only: this suite draws with Core Graphics.
//
// PERF11 -- docs/roadmap/PERF-2026-09.md. And what a frame costs in video:
// one conversion for each picture it shows, however many shutter instants
// and nodes draw it, and no seek when the frames are rendered on every
// core. Counted (WorkCounters.h), not timed.

#include "Document.h"
#include "MotionAudio.h"
#include "MotionCli.h"
#include "MotionComposition.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionMedia.h"
#include "MotionParallelRender.h"
#include "MotionRender.h"
#include "MotionVideo.h"
#include "MotionVideoRender.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"
#include "WorkCounters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const std::string kFfmpeg = FFMPEG_EXECUTABLE;
const std::string kFonts = MOTION_FONT_DIR;
const std::string kParity = MOTION_PARITY_FIXTURE_DIR;

std::string Quote(const std::string& text) { return "'" + text + "'"; }

bool Run(const std::string& command) {
    return std::system((command + " > /dev/null 2>&1").c_str()) == 0;
}

std::string Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

mcp_json::Value Json(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    Check(mcp_json::Value::Parse(text, value, error), "JSON parses: " + error);
    return value;
}

MotionComposition Parse(const std::string& json, const std::string& label) {
    MotionComposition composition;
    MotionError error;
    Check(ParseMotionComposition(json, composition, error),
          label + " parses: " + error.code + " at " + error.path + " -- " +
              error.message);
    return composition;
}

// Everything a run makes, in one directory removed at the end.
struct Media {
    std::filesystem::path directory;
    std::string frames;   // 64x36, 25 fps, 50 pictures, PNG in MOV
    std::string clip;     // 64x36, 30000/1001, 300 pictures
    std::string yuv;      // the same pictures as H.264 4:2:0
    std::string rotated;  // frames.mov, displayed a quarter turn
    std::string tone;     // 3 s of 48 kHz stereo float
    std::string voice;    // 100 pictures at 25 fps and 4 s of the sound
    std::string veiled;   // one colour at alpha 128, PNG in MOV
    std::string solid;    // the same with alpha 255 everywhere

    ~Media() {
        std::error_code ignored;
        if (!directory.empty()) {
            std::filesystem::remove_all(directory, ignored);
        }
    }
};

const char kPictures[] = "format=rgb24,geq=r=4*N:g=255-4*N:b=80";
const char kSawtooth[] =
    "aevalsrc=exprs=mod(n\\,1000)/1000|-mod(n\\,1000)/1000:s=48000";

bool MakeMedia(Media& media) {
    media.directory = std::filesystem::temp_directory_path() /
                      ("cutmachine-motion-media-" + GenerateUlid());
    std::filesystem::create_directories(media.directory);
    const auto at = [&](const char* name) {
        return (media.directory / name).string();
    };
    media.frames = at("frames.mov");
    media.clip = at("clip.mov");
    media.yuv = at("yuv.mp4");
    media.rotated = at("rotated.mov");
    media.tone = at("tone.wav");
    media.voice = at("voice.mov");
    media.veiled = at("veiled.mov");
    media.solid = at("solid.mov");
    const std::string ff = Quote(kFfmpeg) + " -hide_banner -y ";
    const bool ok =
        Run(ff + "-f lavfi -i " +
            Quote(std::string("color=c=black:s=64x36:r=25:d=2,") + kPictures) +
            " -c:v png " + Quote(media.frames)) &&
        Run(ff + "-f lavfi -i " +
            Quote(std::string("color=c=black:s=64x36:r=30000/1001,") +
                  kPictures) +
            " -frames:v 300 -c:v png " + Quote(media.clip)) &&
        Run(ff + "-i " + Quote(media.frames) +
            " -c:v libx264 -pix_fmt yuv420p -qp 0 -colorspace bt709 "
            "-color_primaries bt709 -color_trc bt709 " +
            Quote(media.yuv)) &&
        Run(ff + "-display_rotation 90 -i " + Quote(media.frames) +
            " -c copy " + Quote(media.rotated)) &&
        Run(ff + "-f lavfi -i " + Quote(std::string(kSawtooth) + ":d=3") +
            " -c:a pcm_f32le " + Quote(media.tone)) &&
        Run(ff + "-f lavfi -i " +
            Quote(std::string("color=c=black:s=64x36:r=25:d=4,") + kPictures) +
            " -f lavfi -i " + Quote(std::string(kSawtooth) + ":d=4") +
            " -c:v png -c:a pcm_f32le " + Quote(media.voice)) &&
        Run(ff + "-f lavfi -i " +
            Quote("color=c=black:s=64x36:r=25:d=0.2,format=rgba,"
                  "geq=r=200:g=100:b=50:a=128") +
            " -c:v png " + Quote(media.veiled)) &&
        Run(ff + "-f lavfi -i " +
            Quote("color=c=black:s=64x36:r=25:d=0.2,format=rgba,"
                  "geq=r=200:g=100:b=50:a=255") +
            " -c:v png " + Quote(media.solid));
    Check(ok, "FFmpeg makes the test media in " + media.directory.string());
    return ok;
}

bool PixelIs(const std::vector<uint8_t>& rgba, int32_t width, int x, int y,
             int red, int green, int blue, int tolerance = 0) {
    const size_t at = (static_cast<size_t>(y) * width + x) * 4;
    return at + 3 < rgba.size() && std::abs(rgba[at] - red) <= tolerance &&
           std::abs(rgba[at + 1] - green) <= tolerance &&
           std::abs(rgba[at + 2] - blue) <= tolerance;
}

std::string PixelText(const std::vector<uint8_t>& rgba, int32_t width, int x,
                      int y) {
    const size_t at = (static_cast<size_t>(y) * width + x) * 4;
    if (at + 3 >= rgba.size()) {
        return "outside";
    }
    return std::to_string(rgba[at]) + "," + std::to_string(rgba[at + 1]) + "," +
           std::to_string(rgba[at + 2]) + "," + std::to_string(rgba[at + 3]);
}

void TestReader(const Media& media) {
    MotionMediaInfo info;
    std::string code;
    std::string error;
    Check(ProbeMotionMedia(media.frames, info, code, error) && info.has_video &&
              !info.has_audio && info.width == 64 && info.height == 36 &&
              info.rate_numerator == 25 && info.rate_denominator == 1 &&
              info.duration.value == 50 && info.duration.rate == 25,
          "the reader probes a picture file: " + error);
    Check(ProbeMotionMedia(media.clip, info, code, error) &&
              info.rate_numerator == 30000 && info.rate_denominator == 1001 &&
              info.duration.value == 300300 && info.duration.rate == 30000,
          "the reader keeps an NTSC cadence exact: " +
              std::to_string(info.duration.value) + "/" +
              std::to_string(info.duration.rate));
    Check(ProbeMotionMedia(media.tone, info, code, error) && !info.has_video &&
              info.has_audio && info.duration.value == 144000 &&
              info.duration.rate == 48000,
          "the reader probes a sound file");
    Check(ProbeMotionMedia(media.rotated, info, code, error) &&
              info.rotation == 270 && info.width == 36 && info.height == 64,
          "the reader turns a picture its container rotates: " +
              std::to_string(info.rotation));
    Check(!ProbeMotionMedia((media.directory / "absent.mov").string(), info,
                            code, error) &&
              code == "MediaMissing",
          "an absent media is refused as MediaMissing");

    // Forward, backward, far ahead, past the end: each picture by its index.
    for (const int64_t index : {0, 7, 8, 49, 3, 30, 60}) {
        MotionVideoPicture picture;
        const int64_t shown = std::min<int64_t>(index, 49);
        Check(ReadMotionVideoFrame(media.frames, index, picture, code, error) &&
                  picture.width == 64 && picture.height == 36 &&
                  PixelIs(picture.rgba, 64, 10, 10, static_cast<int>(4 * shown),
                          static_cast<int>(255 - 4 * shown), 80) &&
                  picture.rgba[3] == 255,
              "picture " + std::to_string(index) + " is read exactly: " +
                  PixelText(picture.rgba, 64, 10, 10) + " " + error);
    }
    MotionVideoPicture picture;
    Check(ReadMotionVideoFrame(media.clip, 35, picture, code, error) &&
              PixelIs(picture.rgba, 64, 5, 5, 140, 115, 80),
          "picture 35 of the NTSC file is read exactly: " +
              PixelText(picture.rgba, 64, 5, 5));
    Check(ReadMotionVideoFrame(media.yuv, 20, picture, code, error) &&
              PixelIs(picture.rgba, 64, 30, 18, 80, 175, 80, 3),
          "a 4:2:0 picture is converted with its BT.709 matrix: " +
              PixelText(picture.rgba, 64, 30, 18));
    Check(ReadMotionVideoFrame(media.rotated, 10, picture, code, error) &&
              picture.width == 36 && picture.height == 64 &&
              PixelIs(picture.rgba, 36, 3, 50, 40, 215, 80),
          "a rotated picture comes back upright");
    Check(!ReadMotionVideoFrame(media.tone, 0, picture, code, error) &&
              code == "MediaHasNoVideo",
          "a sound file has no picture to read");

    std::shared_ptr<const MotionPcm> pcm;
    bool exact = ReadMotionAudio(media.tone, pcm, code, error) &&
                 pcm->left.size() == 144000;
    for (int64_t n = 0; exact && n < 144000; n += 997) {
        const float expected = static_cast<float>(n % 1000) / 1000.0f;
        exact = pcm->left[n] == expected && pcm->right[n] == -expected;
    }
    Check(exact, "the reader decodes a float sound to the bit: " + error);
    Check(!ReadMotionAudio(media.frames, pcm, code, error) &&
              code == "MediaHasNoAudio",
          "a picture file has no sound to read");
}

bool Render(const MotionComposition& composition, int64_t frame,
            const std::string& directory, const MotionReach& reach,
            MotionRaster& raster, MotionFrame& evaluated,
            const std::string& label) {
    MotionCoreTextMeasurer measurer(directory);
    measurer.SetMediaPaths(reach.media_paths);
    MotionError error;
    const bool ok =
        EvaluateMotionFrame(composition, mcp_json::Value(), frame, measurer,
                            reach.Context(), evaluated, error) &&
        RenderMotionFrame(evaluated, measurer, raster, error);
    Check(ok, label + ": " + error.code + " at " + error.path + " -- " +
                  error.message);
    return ok;
}

// C12 -- the picture a frame names is the picture drawn, cropped by the
// rounded box.
void TestC12Painted(const Media& media) {
    const MotionComposition composition =
        Parse(Read(kParity + "/c12-rounded-video.json"), "C12");
    struct Expected {
        int64_t frame;
        int picture;  // -1: nothing drawn
    };
    for (const Expected& expected :
         {Expected{0, 11}, Expected{1, 12}, Expected{25, 26}, Expected{39, 35},
          Expected{40, -1}}) {
        MotionRaster raster;
        MotionFrame frame;
        MotionRaster again;
        MotionFrame second;
        if (!Render(composition, expected.frame, media.directory.string(), {},
                    raster, frame,
                    "C12 frame " + std::to_string(expected.frame)) ||
            !Render(composition, expected.frame, media.directory.string(), {},
                    again, second, "C12 again")) {
            continue;
        }
        const bool shown = expected.picture >= 0;
        Check(shown ? PixelIs(raster.pixels, raster.width, 320, 180,
                              4 * expected.picture, 255 - 4 * expected.picture,
                              80, 1)
                    : PixelIs(raster.pixels, raster.width, 320, 180, 0, 0, 0),
              "C12 frame " + std::to_string(expected.frame) + " draws " +
                  (shown ? "picture " + std::to_string(expected.picture)
                         : std::string("the box alone, the video ended")) +
                  ": " + PixelText(raster.pixels, raster.width, 320, 180));
        Check(raster.pixels == again.pixels,
              "C12 frame " + std::to_string(expected.frame) +
                  " renders the same bytes twice");
        // The rounded corner hides the video: the page shows there.
        Check(PixelIs(raster.pixels, raster.width, 161, 91, 0x10, 0x18, 0x20),
              "C12 hides the video past the rounded corner: " +
                  PixelText(raster.pixels, raster.width, 161, 91));
    }
}

// C13 -- the sound read from a real file, mixed by the engine, then
// encoded beside the frames: the samples before the codec, and after it.
void TestC13Real(const Media& media) {
    const MotionComposition composition =
        Parse(Read(kParity + "/c13-detached-sound.json"), "C13");
    MotionCoreTextMeasurer measurer(media.directory.string());
    MotionPcm mix;
    MotionError error;
    Check(MixMotionAudio(composition, mcp_json::Value(),
                         MotionEvaluationContext{}, measurer, 0,
                         MotionAudioSampleCount(composition), mix, error),
          "C13 mixes the real sound: " + error.code + " " + error.message);
    struct Point {
        int64_t n;
        double gain;
    };
    for (const Point& point :
         {Point{19199, 0.0}, Point{24000, 0.25}, Point{28800, 0.5},
          Point{50000, 0.5}, Point{110400, 0.25}, Point{115200, 0.0}}) {
        const float expected = static_cast<float>(
            static_cast<float>(point.n % 1000) / 1000.0f * point.gain);
        Check(static_cast<size_t>(point.n) < mix.left.size() &&
                  std::fabs(mix.left[point.n] - expected) <= 1e-6 &&
                  std::fabs(mix.right[point.n] + expected) <= 1e-6,
              "C13 real sample " + std::to_string(point.n) +
                  " is the file's own sample times its gain");
    }
    MotionRaster raster;
    MotionFrame frame;
    if (Render(composition, 30, media.directory.string(), {}, raster, frame,
               "C13 frame 30")) {
        Check(PixelIs(raster.pixels, raster.width, 160, 90, 120, 135, 80),
              "C13 shows picture 30 while it plays the sound of frame 30: " +
                  PixelText(raster.pixels, raster.width, 160, 90));
    }

    // The same composition rendered to a file: its sound track is the mix,
    // quantised to 24 bits and nothing else.
    MotionVideoRequest request;
    request.composition = &composition;
    request.media_directory = media.directory.string();
    request.options.output = (media.directory / "c13.mov").string();
    request.options.alpha = true;
    std::string report;
    Check(RenderMotionVideos(request, report, error) &&
              report.find("\"audio\":\"pcm_s24le\"") != std::string::npos,
          "C13 renders to a file with its sound: " + report + " " +
              error.message);
    std::shared_ptr<const MotionPcm> encoded;
    std::string code;
    std::string message;
    ResetMotionMediaCache();
    bool same =
        ReadMotionAudio(request.options.output, encoded, code, message) &&
        encoded->left.size() >= mix.left.size();
    double worst = 0.0;
    for (size_t n = 0; same && n < mix.left.size(); ++n) {
        worst = std::max(worst, static_cast<double>(
                                    std::fabs(encoded->left[n] - mix.left[n])));
    }
    Check(same && worst <= 1.0 / (1 << 23),
          "C13's file carries the mix to 24-bit precision: worst " +
              std::to_string(worst) + " " + message);

    // MM1 -- the same sound delivered at -23 LUFS in an .mp4: measured,
    // brought there, checked through AAC, and the file measured again.
    request.options.output = (media.directory / "c13-23.mp4").string();
    request.options.alpha = false;
    request.options.loudness = -23.0;
    report.clear();
    mcp_json::Value parsed;
    std::string parseError;
    const bool rendered = RenderMotionVideos(request, report, error) &&
                          mcp_json::Value::Parse(report, parsed, parseError);
    const mcp_json::Value* loudness =
        rendered ? parsed.Find("outputs")->AsArray()[0].Find("loudness")
                 : nullptr;
    const mcp_json::Value* file =
        loudness != nullptr ? loudness->Find("file") : nullptr;
    const auto number = [](const mcp_json::Value* value) {
        return value != nullptr && value->IsNumber() ? std::stod(value->Dump())
                                                     : 0.0;
    };
    Check(file != nullptr &&
              std::fabs(number(file->Find("integrated_lufs")) + 23.0) <= 0.5 &&
              number(file->Find("true_peak_dbtp")) <= -1.0 &&
              loudness->Find("ceiling_kept")->AsBool(),
          "C13 delivered at -23 LUFS under -1 dBTP, as its file measures: " +
              report + " " + error.message);
    request.options.loudness = -50.0;
    Check(!RenderMotionVideos(request, report, error) &&
              error.code == "InvalidValue",
          "a loudness out of reason is refused before anything is drawn");
}

// C14 -- one lower third drawn twice, each with its own colour.
void TestC14Painted() {
    const MotionComposition parent =
        Parse(Read(kParity + "/c14-precomposition.json"), "C14");
    MotionReach reach;
    auto library = std::make_shared<MotionLibrary>();
    library->compositions.emplace(
        "01K7C000000000000000001400",
        Parse(Read(kParity + "/c14-lower-third.json"), "C14 lower third"));
    reach.library = library;
    reach.composition_id = "01K7C000000000000000001410";
    MotionRaster raster;
    MotionFrame frame;
    if (!Render(parent, 17, std::string(), reach, raster, frame, "C14 at 17")) {
        return;
    }
    // Each band spans x 10..230 and y 30..60 of its half-size instance; the
    // points are on the band, clear of its label.
    Check(PixelIs(raster.pixels, raster.width, 250, 85, 0xe4, 0x57, 0x2e, 1),
          "C14 draws the left band in the parent's accent: " +
              PixelText(raster.pixels, raster.width, 250, 85));
    Check(PixelIs(raster.pixels, raster.width, 570, 245, 0x4d, 0xab, 0xa1, 1),
          "C14 draws the right band in the lower third's own colour: " +
              PixelText(raster.pixels, raster.width, 570, 245));
    Check(
        raster.pixels[(static_cast<size_t>(20) * raster.width + 20) * 4 + 3] ==
            0,
        "C14 leaves the parent transparent around its instances");
    MotionRaster at5;
    if (Render(parent, 5, std::string(), reach, at5, frame, "C14 at 5")) {
        Check(
            at5.pixels[(static_cast<size_t>(245) * at5.width + 570) * 4 + 3] ==
                0,
            "C14 does not draw the second instance before its sequence");
    }
}

// The surfaces: a project's composition that plays one of its media by
// ULID and shows another composition, through the CLI.
void TestProjectSurfaces(const Media& media) {
    const Ulid source = "01K7C000000000000000001399";
    const Ulid player = "01K7C000000000000000001500";
    Document document;
    document.sequence.id = "01K7C000000000000000001501";
    document.sequence.name = "Montage";
    document.sources = {{source, media.voice, {25, 1}, {100, 25}}};
    document.compositions.push_back(
        {"01K7C000000000000000001400", "Bandeau",
         Json(Read(kParity + "/c14-lower-third.json"))});
    const std::string definition =
        R"({"width":320,"height":180,"fps":25,"duration":60,)"
        R"("root":{"type":"fill","uid":"01K7C000000000000000001502",)"
        R"("children":[{"type":"video","uid":"01K7C000000000000000001503",)"
        R"("id":"picture","media":")" +
        source +
        R"(","style":{"width":"100%","height":"100%"}},)"
        R"({"type":"composition","uid":"01K7C000000000000000001504",)"
        R"("id":"band","composition":"01K7C000000000000000001400",)"
        R"("props":{"name":"Chloé"},)"
        R"("style":{"position":"absolute","left":0,"bottom":0,"width":160}}]}})";
    document.compositions.push_back({player, "Lecteur", Json(definition)});
    std::string message;
    Check(document.Validate(message), "the project validates: " + message);
    std::string project;
    if (!CreatePortableProject((media.directory / "Surfaces").string(),
                               Project::FromDocument(document, "Surfaces"),
                               project, message)) {
        Check(false, "the project saves: " + message);
        return;
    }
    std::string output;
    Check(DescribeMotionFrameCommand({project, "30", "--composition", player},
                                     output) == 0 &&
              output.find("\"video\":{\"media\":\"" + source +
                          "\",\"state\":\"playing\",\"time\":\"6/5\","
                          "\"frame\":30") != std::string::npos &&
              output.find("\"composition\":{\"id\":"
                          "\"01K7C000000000000000001400\",\"state\":"
                          "\"playing\",\"frame\":60") != std::string::npos,
          "the CLI describes the media by ULID and the instance: " +
              output.substr(0, 400));
    const std::string image = (media.directory / "player-30.png").string();
    Check(RenderMotionFrameCommand(
              {project, "30", image, "--composition", player}, output) == 0 &&
              std::filesystem::exists(image),
          "the CLI renders the frame: " + output);
    const std::string video = (media.directory / "player.mov").string();
    Check(RenderMotionVideoCommand({project, video, "--composition", player},
                                   output) == 0 &&
              output.find("\"audio\":\"pcm_s24le\"") != std::string::npos,
          "the CLI renders the composition with its media's sound: " + output);
}

// PERF11 -- three bands of one long-GOP file and a fourth of another, all
// moving under motion blur: every picture is drawn by several nodes at each
// of nine shutter instants.
const char kBands[] = R"({
  "width": 320, "height": 180, "fps": 25, "duration": 40,
  "motionBlur": {"samples": 8, "shutterAngle": 180},
  "root": {"type": "fill", "style": {"backgroundColor": "#101820"},
    "children": [
      {"type": "video", "src": "yuv.mp4", "startFrom": 5, "muted": true,
       "style": {"position": "absolute", "left": 0, "top": 0, "width": 320,
                 "height": 40, "objectFit": "cover"},
       "animate": {"translateX": {"interpolate": {"frames": [0, 39],
                   "values": [0, 120], "easing": "linear"}}}},
      {"type": "video", "src": "yuv.mp4", "startFrom": 5, "muted": true,
       "style": {"position": "absolute", "left": 0, "top": 45, "width": 320,
                 "height": 40, "objectFit": "cover"},
       "animate": {"translateX": {"interpolate": {"frames": [0, 39],
                   "values": [0, -120], "easing": "linear"}}}},
      {"type": "video", "src": "yuv.mp4", "startFrom": 5, "muted": true,
       "style": {"position": "absolute", "left": 0, "top": 90, "width": 320,
                 "height": 40, "objectFit": "cover"},
       "animate": {"translateY": {"interpolate": {"frames": [0, 39],
                   "values": [0, 30], "easing": "linear"}}}},
      {"type": "video", "src": "frames.mov", "startFrom": 2, "muted": true,
       "style": {"position": "absolute", "left": 0, "top": 135, "width": 320,
                 "height": 40, "objectFit": "cover"},
       "animate": {"translateX": {"interpolate": {"frames": [0, 39],
                   "values": [0, 60], "easing": "linear"}}}}
    ]}
})";

// MB11 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. startFrom and endAt
// bound to props, and a volume whose first key is a prop: what a piece that
// plays the next shot of a timeline from its in-point needs.
const char kBoundTimes[] = R"({
  "width": 64, "height": 36, "fps": 25, "duration": 40,
  "props": {"debut": {"type": "number", "default": 5, "min": 0, "max": 49},
            "fin": {"type": "number", "default": 30, "min": 1, "max": 50},
            "fort": {"type": "number", "default": 1, "min": 0, "max": 1}},
  "root": {"type": "fill", "children": [
    {"type": "video", "id": "plan", "src": "frames.mov", "muted": true,
     "startFrom": {"prop": "debut"}, "endAt": {"prop": "fin"}},
    {"type": "audio", "id": "son", "src": "tone.wav",
     "startFrom": {"prop": "debut"},
     "animate": {"volume": {"interpolate": {"frames": [0, 39],
                 "values": [{"prop": "fort"}, {"prop": "fort"}]}}}}]}
})";

const MotionEvaluatedNode* FindVideo(const MotionEvaluatedNode& node) {
    if (node.type == MotionNodeType::Video) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = FindVideo(child)) {
            return found;
        }
    }
    return nullptr;
}

void TestBoundTimes(const Media& media) {
    const MotionComposition composition = Parse(kBoundTimes, "bound times");
    MotionCoreTextMeasurer measurer(media.directory.string());
    const auto shown = [&](int64_t index, const std::string& props,
                           std::string& state, int64_t& frame,
                           MotionError& error) {
        mcp_json::Value overrides;
        std::string message;
        mcp_json::Value::Parse(props, overrides, message);
        MotionFrame output;
        if (!EvaluateMotionFrame(composition, overrides, index, measurer,
                                 MotionEvaluationContext{}, output, error)) {
            return false;
        }
        const MotionEvaluatedNode* video = FindVideo(output.root);
        state = video != nullptr ? video->media.state : "";
        frame = video != nullptr ? video->media.frame : -1;
        return video != nullptr;
    };
    std::string state;
    int64_t frame = -1;
    MotionError error;
    Check(shown(0, "null", state, frame, error) && state == "playing" &&
              frame == 5,
          "the default startFrom: frame 0 shows the media's frame 5 (" +
              std::to_string(frame) + ") " + error.message);
    Check(shown(0, R"({"debut": 12})", state, frame, error) && frame == 12,
          "the instance's startFrom: frame 0 shows the media's frame 12 (" +
              std::to_string(frame) + ") " + error.message);
    Check(shown(26, "null", state, frame, error) && state == "after",
          "the default endAt stops the media at its frame 30: " + state);
    Check(shown(26, R"({"fin": 40})", state, frame, error) &&
              state == "playing" && frame == 31,
          "the instance's endAt lets it play on: " + state + " " +
              std::to_string(frame));
    error = {};
    Check(!shown(0, R"({"fin": 3})", state, frame, error) &&
              error.message.find("after startFrom") != std::string::npos,
          "an endAt before the instance's startFrom is refused: " +
              error.message);

    // The sound starts where the instance says, and its bound volume
    // outlives the evaluation that collected it.
    const auto first = [&](const std::string& props, int64_t& sample) {
        mcp_json::Value overrides;
        std::string message;
        mcp_json::Value::Parse(props, overrides, message);
        std::string json;
        size_t audible = 0;
        MotionError audioError;
        if (!DescribeMotionAudio(composition, overrides,
                                 MotionEvaluationContext{}, measurer, json,
                                 audible, audioError)) {
            return false;
        }
        const size_t at = json.find("\"mediaSample\":");
        sample =
            at == std::string::npos ? -1 : std::stoll(json.substr(at + 14));
        return true;
    };
    int64_t sample = -1;
    Check(first("null", sample) && sample == 9600,
          "the sound reads from the default startFrom (5 frames, 9600 "
          "samples): " +
              std::to_string(sample));
    Check(first(R"({"debut": 10})", sample) && sample == 19200,
          "and from the instance's (10 frames): " + std::to_string(sample));
    const auto loudness = [&](const std::string& props) {
        mcp_json::Value overrides;
        std::string message;
        mcp_json::Value::Parse(props, overrides, message);
        MotionPcm mix;
        MotionError mixError;
        double peak = -1.0;
        if (MixMotionAudio(composition, overrides, MotionEvaluationContext{},
                           measurer, 0, 4800, mix, mixError)) {
            peak = 0.0;
            for (const float value : mix.left) {
                peak = std::max(peak, static_cast<double>(std::fabs(value)));
            }
        }
        return peak;
    };
    const double loud = loudness("null");
    const double silent = loudness(R"({"fort": 0})");
    Check(loud > 0.1 && silent == 0.0,
          "the volume's bound keys are the instance's: " +
              std::to_string(loud) + " then " + std::to_string(silent));
}

using PictureKey = std::pair<std::string, int64_t>;

// The pictures the painter draws under this node.
void CollectPictures(const MotionEvaluatedNode& node,
                     std::set<PictureKey>& pictures, size_t& draws) {
    if (node.type == MotionNodeType::Video && node.media.state == "playing") {
        pictures.emplace(node.media.src, node.media.frame);
        ++draws;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        CollectPictures(child, pictures, draws);
    }
}

size_t FramePictures(const MotionFrame& frame, size_t& draws) {
    std::set<PictureKey> pictures;
    CollectPictures(frame.root, pictures, draws);
    for (const MotionEvaluatedNode& root : frame.motion_blur) {
        CollectPictures(root, pictures, draws);
    }
    return pictures.size();
}

// PERF11 -- the two halves of a read are the read, and a picture says
// whether it is opaque without being scanned by whoever draws it.
void TestSplitRead(const Media& media) {
    std::string code;
    std::string error;
    MotionVideoPicture whole;
    MotionDecodedPicture decoded;
    MotionVideoPicture halves;
    Check(ReadMotionVideoFrame(media.yuv, 20, whole, code, error) &&
              DecodeMotionVideoFrame(media.yuv, 20, decoded, code, error) &&
              ConvertMotionVideoFrame(decoded, halves, code, error) &&
              halves.width == whole.width && halves.height == whole.height &&
              halves.frame == 20 && halves.rgba == whole.rgba && whole.opaque,
          "decoding then converting a picture is reading it: " + error);
    // The decoded picture owns its bytes: it outlives its decoder.
    ResetMotionMediaCache();
    MotionVideoPicture later;
    Check(ConvertMotionVideoFrame(decoded, later, code, error) &&
              later.rgba == whole.rgba,
          "a decoded picture converts after its file is closed: " + error);
    MotionVideoPicture rotated;
    Check(DecodeMotionVideoFrame(media.rotated, 10, decoded, code, error) &&
              decoded.rotation == 270 &&
              ConvertMotionVideoFrame(decoded, rotated, code, error) &&
              rotated.width == 36 && rotated.height == 64 &&
              PixelIs(rotated.rgba, 36, 3, 50, 40, 215, 80),
          "the conversion turns a rotated picture upright");
    MotionVideoPicture veiled;
    Check(ReadMotionVideoFrame(media.veiled, 0, veiled, code, error) &&
              !veiled.opaque && veiled.rgba[3] == 128 &&
              PixelIs(veiled.rgba, 64, 10, 10, 200, 100, 50),
          "a picture with alpha keeps it and says it is not opaque: " +
              PixelText(veiled.rgba, 64, 10, 10) + " " + error);
    MotionVideoPicture solid;
    Check(ReadMotionVideoFrame(media.solid, 0, solid, code, error) &&
              solid.opaque && solid.rgba[3] == 255,
          "an alpha channel at 255 everywhere reads as opaque: " + error);
    Check(
        !ConvertMotionVideoFrame(MotionDecodedPicture{}, solid, code, error) &&
            code == "DecodeFailed",
        "nothing decoded is nothing to convert");
}

// PERF11 -- frames that decode at the same time do it in the order of
// their ranks, and a rank that shows no video only leaves.
void TestTurns() {
    MotionMediaTurns turns;
    std::mutex mutex;
    std::vector<size_t> order;
    std::vector<std::thread> threads;
    for (const size_t ordinal : {7, 6, 5, 4, 3, 2, 1, 0}) {
        threads.emplace_back([&, ordinal] {
            if (ordinal % 3 == 2) {
                turns.Leave(ordinal);
                turns.Leave(ordinal);
                return;
            }
            turns.Enter(ordinal);
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(ordinal);
            }
            turns.Leave(ordinal);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    Check(order == std::vector<size_t>({0, 1, 3, 4, 6, 7}),
          "turns are taken in the order of the ordinals, skipping those "
          "that only left");
}

// PERF11 -- a frame converts each picture it shows once, whatever the
// number of shutter instants and of nodes that draw it, and frames rendered
// one after the other never make a file seek.
void TestOneConversionPerPicture(const Media& media) {
    const MotionComposition composition = Parse(kBands, "the bands");
    MotionCoreTextMeasurer measurer(media.directory.string());
    ResetMotionMediaCache();
    const work_counters::Tally before = work_counters::Read();
    size_t pictures = 0;
    size_t draws = 0;
    bool ok = true;
    for (int64_t index = 0; ok && index < composition.duration; ++index) {
        MotionFrame frame;
        MotionRaster raster;
        MotionError error;
        ok =
            EvaluateMotionFrame(composition, mcp_json::Value(), index, measurer,
                                MotionEvaluationContext{}, frame, error) &&
            RenderMotionFrame(frame, measurer, raster, error);
        Check(ok, "the bands render frame " + std::to_string(index) + ": " +
                      error.code + " " + error.message);
        pictures += FramePictures(frame, draws);
    }
    const work_counters::Tally work = work_counters::Since(before);
    Check(draws >= 9 * pictures / 2 && pictures >= 80,
          "the bands draw each picture many times: " + std::to_string(draws) +
              " draws of " + std::to_string(pictures) + " pictures");
    Check(work.motion_video_converts == pictures,
          "each picture a frame shows is converted once: " +
              std::to_string(work.motion_video_converts) + " conversions for " +
              std::to_string(pictures) + " pictures");
    Check(work.motion_video_seeks == 2,
          "frames in order position each file once: " +
              std::to_string(work.motion_video_seeks) + " seeks, 2 files");
}

// PERF11 -- the same frames rendered on every core are the same bytes, and
// their pictures are still decoded in the order of the file: one position
// per file, where workers left to themselves seek back for every picture
// that another has passed.
void TestParallelFrames(const Media& media) {
    const MotionComposition composition = Parse(kBands, "the bands");
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        Check(false, "the renderer is installed");
        return;
    }
    MotionFrameRequest request;
    request.composition = &composition;
    request.media_directory = media.directory.string();
    std::vector<int64_t> frames;
    std::vector<std::string> serial;
    size_t pictures = 0;
    size_t draws = 0;
    for (int64_t index = 0; index < composition.duration; ++index) {
        MotionFrameImage image;
        MotionError error;
        request.frame = index;
        if (!render(request, image, error)) {
            Check(false, "the bands render alone: " + error.message);
            return;
        }
        pictures += FramePictures(image.evaluated, draws);
        frames.push_back(index);
        serial.push_back(std::move(image.png));
    }
    const std::filesystem::path directory = media.directory / "parallel";
    std::filesystem::create_directories(directory);
    const auto pathFor = [&](int64_t frame) {
        return (directory / (std::to_string(frame) + ".png")).string();
    };
    ResetMotionMediaCache();
    const work_counters::Tally before = work_counters::Read();
    std::string message;
    Check(RenderMotionFramesToFiles(request, frames, pathFor, nullptr, message),
          "the bands render on every core: " + message);
    const work_counters::Tally work = work_counters::Since(before);
    size_t same = 0;
    for (const int64_t frame : frames) {
        if (Read(pathFor(frame)) == serial[static_cast<size_t>(frame)]) {
            ++same;
        }
    }
    Check(same == frames.size(),
          "frames rendered together are the bytes of frames rendered alone: " +
              std::to_string(same) + " of " + std::to_string(frames.size()));
    Check(work.motion_video_seeks == 2,
          "frames rendered together read each file forward: " +
              std::to_string(work.motion_video_seeks) + " seeks, 2 files");
    Check(work.motion_video_converts == pictures,
          "frames rendered together convert each picture once: " +
              std::to_string(work.motion_video_converts) + " conversions for " +
              std::to_string(pictures) + " pictures");
}

// MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. The tools of
// MOTION-DESIGN-2026-09 rendered on every core: a text under a shutter of
// four instants, grain, slices and a composition grain are the bytes of the
// same frames rendered alone. Meanwhile another thread registers font
// directories, as a tool called during an export does: the set of
// registered files the painter reads to choose a face was written without
// a lock (ThreadSanitizer sees it once MotionRender.mm is instrumented).
// And a list of no frames is no work: std::clamp was asked for a range
// whose top (0 frames) lay under its bottom (1 worker), which is undefined.
void TestParallelMotionDesign(const Media& media) {
    const MotionComposition composition = Parse(
        R"({"width":160,"height":48,"fps":25,"duration":12,)"
        R"("motionBlur":{"samples":4,"shutterAngle":270},)"
        R"("grain":{"amount":0.1,"size":2,"seed":3},)"
        R"("root":{"type":"fill","style":{"backgroundColor":"#202020"},)"
        R"("children":[{"type":"text","content":"GRAIN","style":{)"
        R"("position":"absolute","left":10,"top":4,)"
        R"("fontFamily":"Fira Sans Condensed","fontWeight":600,)"
        R"("fontSize":32,)"
        R"J("color":"#FFFFFF","filter":"grain(0.3 1px 5) slice(4px 6px 2)"},)J"
        R"("animate":{"translateX":{"interpolate":{"frames":[0,11],)"
        R"("values":[0,88]}}}}]}})",
        "the motion design frames");
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        Check(false, "the renderer is installed");
        return;
    }
    MotionFrameRequest request;
    request.composition = &composition;
    request.media_directory = media.directory.string();
    std::vector<int64_t> frames;
    std::vector<std::string> serial;
    for (int64_t index = 0; index < composition.duration; ++index) {
        MotionFrameImage image;
        MotionError error;
        request.frame = index;
        if (!render(request, image, error)) {
            Check(false, "the motion design frames render alone: " +
                             error.code + " " + error.message);
            return;
        }
        frames.push_back(index);
        serial.push_back(std::move(image.png));
    }
    const std::filesystem::path directory = media.directory / "design";
    std::filesystem::create_directories(directory);
    const auto pathFor = [&](int64_t frame) {
        return (directory / (std::to_string(frame) + ".png")).string();
    };
    std::string message;
    Check(RenderMotionFramesToFiles(request, {}, pathFor, nullptr, message) &&
              message.empty() && std::filesystem::is_empty(directory),
          "no frame to render is no work and no refusal: " + message);
    // Copies of a font, each a file the process has not registered yet.
    std::vector<std::filesystem::path> fonts;
    for (int copy = 0; copy < 6; ++copy) {
        const std::filesystem::path fontDirectory =
            media.directory / ("fonts-" + std::to_string(copy));
        std::filesystem::create_directories(fontDirectory);
        std::filesystem::copy_file(
            std::filesystem::path(kFonts) / "outfit" / "Outfit[wght].ttf",
            fontDirectory / "Outfit[wght].ttf");
        fonts.push_back(fontDirectory);
    }
    bool registered = true;
    std::thread registering([&] {
        for (const std::filesystem::path& fontDirectory : fonts) {
            MotionError error;
            registered =
                RegisterMotionFontDirectory(fontDirectory.string(), error) &&
                registered;
        }
    });
    const bool rendered =
        RenderMotionFramesToFiles(request, frames, pathFor, nullptr, message);
    registering.join();
    Check(rendered && registered,
          "the motion design frames render on every core while fonts are "
          "registered: " +
              message);
    size_t same = 0;
    for (const int64_t frame : frames) {
        if (Read(pathFor(frame)) == serial[static_cast<size_t>(frame)]) {
            ++same;
        }
    }
    Check(same == frames.size(),
          "blur, grain and slices rendered together are the bytes of the "
          "frames rendered alone: " +
              std::to_string(same) + " of " + std::to_string(frames.size()));
}

}  // namespace

int main() {
    MotionError error;
    if (!RegisterMotionFontDirectory(kFonts, error)) {
        std::cerr << "FAIL: fixture fonts: " << error.message << '\n';
        return 1;
    }
    Media media;
    if (!MakeMedia(media)) {
        return 1;
    }
    InstallMotionFrameRenderer();
    TestReader(media);
    TestSplitRead(media);
    TestTurns();
    TestOneConversionPerPicture(media);
    TestParallelFrames(media);
    TestC12Painted(media);
    TestC13Real(media);
    TestC14Painted();
    TestProjectSurfaces(media);
    TestBoundTimes(media);
    // Last: it registers copies of a font for the rest of the process.
    TestParallelMotionDesign(media);
    if (failures != 0) {
        std::cerr << failures << " motion media assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: motion media\n";
    return 0;
}
