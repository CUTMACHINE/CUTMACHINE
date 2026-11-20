// G1 -- docs/roadmap/GRADE-2026-09.md. The measure end to end: a real file
// decoded by FFmpeg at the position a clip names, through the document's
// grade, framing, colour management and look.
#include "ColorMeasure.h"
#include "FaceFinder.h"
#include "FfmpegPipe.h"
#include "LookLutStorage.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

bool Between(int64_t value, int64_t low, int64_t high) {
    return value >= low && value <= high;
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

bool Generate(const std::string& lavfi, const std::filesystem::path& output) {
    const std::string command =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -filter_complex '" + lavfi +
        "' -c:v libx264 -qp 0 -pix_fmt yuv420p -colorspace bt709 "
        "-color_primaries "
        "bt709 -color_trc bt709 -y " +
        Quote(output);
    return std::system(command.c_str()) == 0;
}
// Lossless, so white stays on its top code: a lossy encode leaves it one
// code under, which is not a clipped channel.

void AddMedia(Document& document, const std::string& id,
              const std::string& file) {
    LibraryMedia media;
    media.id = id;
    media.path = file;
    media.filename = file;
    media.rate = {25, 1};
    media.duration = {25, 25};
    media.width = 160;
    media.height = 90;
    media.pixel_format = "yuv420p";
    document.library.push_back(media);
    document.sources.push_back(
        {media.id, media.path, media.rate, media.duration});
}

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  int64_t timelineIn) {
    // source_in, duration, timeline_in.
    return {id, sourceId, {0, 25}, {25, 25}, {timelineIn, 25}};
}

// A JPEG read back as 8-bit RGB, the way a model's decoder reads it.
bool DecodeJpeg(const std::string& jpeg, const std::filesystem::path& root,
                int64_t width, int64_t height, std::vector<uint8_t>& rgb) {
    const std::filesystem::path file = root / "frame.jpg";
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output.write(jpeg.data(), static_cast<std::streamsize>(jpeg.size()));
    }
    std::string error;
    rgb.clear();
    return ReadFfmpegRawFrames(
               {FFMPEG_EXECUTABLE, "-hide_banner", "-loglevel", "error",
                "-nostdin", "-i", file.string(), "-f", "rawvideo", "-pix_fmt",
                "rgb24", "pipe:1"},
               static_cast<size_t>(width * height * 3),
               [&](const uint8_t* bytes) {
                   if (rgb.empty())
                       rgb.assign(bytes, bytes + width * height * 3);
               },
               error) &&
           !rgb.empty();
}

// Mean of the red channel over a block of an RGB24 picture.
double MeanRed(const std::vector<uint8_t>& rgb, int64_t width, int64_t x,
               int64_t y, int64_t size) {
    double sum = 0.0;
    for (int64_t row = y; row < y + size; ++row)
        for (int64_t column = x; column < x + size; ++column)
            sum += rgb[static_cast<size_t>((row * width + column) * 3)];
    return sum / static_cast<double>(size * size);
}

constexpr char kHalves[] = "01K31000000000000000000001";
constexpr char kGrey[] = "01K31000000000000000000002";
constexpr char kHalvesClip[] = "01K31000000000000000000011";
constexpr char kGreyClip[] = "01K31000000000000000000012";
constexpr char kAudioClip[] = "01K31000000000000000000013";
constexpr char kDark[] = "01K31000000000000000000003";
constexpr char kWarm[] = "01K31000000000000000000004";
constexpr char kDarkClip[] = "01K31000000000000000000014";
constexpr char kWarmClip[] = "01K31000000000000000000015";
constexpr char kStrong[] = "01K31000000000000000000005";
constexpr char kStrongClip[] = "01K31000000000000000000016";

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cutmachine-color-measure";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    // Left half white, right half black; and an even grey whose code value
    // is the BT.709 camera encoding of 18 % (0.409, code 104 of 255).
    if (!Generate("color=c=white:s=80x90:r=25:d=1[a];"
                  "color=c=black:s=80x90:r=25:d=1[b];[a][b]hstack",
                  root / "halves.mp4") ||
        !Generate("color=c=0x686868:s=160x90:r=25:d=1", root / "grey.mp4") ||
        !Generate("color=c=0x404040:s=160x90:r=25:d=1", root / "dark.mp4") ||
        !Generate("color=c=0x786858:s=160x90:r=25:d=1", root / "warm.mp4") ||
        !Generate("color=c=0x806850:s=160x90:r=25:d=1", root / "strong.mp4")) {
        std::cerr << "FAIL: unable to generate the colour fixtures\n";
        return 1;
    }

    Document document;
    AddMedia(document, kHalves, "halves.mp4");
    AddMedia(document, kGrey, "grey.mp4");
    AddMedia(document, kDark, "dark.mp4");
    AddMedia(document, kWarm, "warm.mp4");
    AddMedia(document, kStrong, "strong.mp4");
    document.sequence.width = 160;
    document.sequence.height = 90;
    document.sequence.tracks = {
        {"01K31000000000000000000021",
         "video",
         0,
         {Clip(kHalvesClip, kHalves, 0), Clip(kGreyClip, kGrey, 25),
          Clip(kDarkClip, kDark, 50), Clip(kWarmClip, kWarm, 75),
          Clip(kStrongClip, kStrong, 100)}},
        {"01K31000000000000000000022",
         "audio",
         0,
         {Clip(kAudioClip, kHalves, 0)}},
    };
    ColorMeasureSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    const std::string directory = root.string();
    std::vector<ColorClipMeasurement> results;
    std::string error;

    // ---- unmanaged: the file as the delivery reads it ------------------
    ColorMeasurePlan plan;
    plan.clip_ids = {kHalvesClip};
    Check(RenderColorMeasurement(document, directory, plan, settings, results,
                                 error),
          "the halves measure: " + error);
    Check(results.size() == 1, "one clip, one measurement");
    if (results.size() == 1) {
        const ColorClipMeasurement& halves = results[0];
        Check(halves.width == 160 && halves.height == 90,
              "a small source is measured at its own size, not enlarged: " +
                  std::to_string(halves.width) + "x" +
                  std::to_string(halves.height));
        Check(halves.source_time == RationalTime{12, 25},
              "Middle is the frame read_frame shows");
        Check(Between(halves.picture.clipped, 4500, 5500) &&
                  Between(halves.picture.crushed, 4500, 5500),
              "half clipped, half crushed: " +
                  std::to_string(halves.picture.clipped) + " / " +
                  std::to_string(halves.picture.crushed));
    }

    plan.region.present = true;
    plan.region.x = {0, 1};
    plan.region.y = {0, 1};
    plan.region.width = {1, 4};
    plan.region.height = {1, 1};
    Check(RenderColorMeasurement(document, directory, plan, settings, results,
                                 error) &&
              results.size() == 1 && results[0].region.clipped == 10000 &&
              results[0].region.pixels == int64_t{40} * 90,
          "the left quarter is all white: " + error);
    plan.region = ColorMeasureRegion{};

    // ---- decoded as the monitor decodes, not as swscale does -----------
    // The grey is code 105 of a limited-range 8-bit file: (105 - 16) / 219
    // is 40.64 %. swscale's YCbCr-to-RGB conversion reads it 0.385 % low,
    // 40.48 %.
    plan.clip_ids = {kGreyClip};
    Check(RenderColorMeasurement(document, directory, plan, settings, results,
                                 error) &&
              results.size() == 1 &&
              Between(results[0].picture.luma_p50, 4063, 4065),
          "code 105 reads 40.64 %: " +
              (results.empty() ? error
                               : std::to_string(results[0].picture.luma_p50)));
    plan.clip_ids = {kHalvesClip};

    // ---- framing: only what the sequence shows is measured -------------
    Document framed = document;
    framed.sequence.tracks[0].clips[0].framing.crop_right = {1, 2};
    Check(RenderColorMeasurement(framed, directory, plan, settings, results,
                                 error) &&
              results.size() == 1,
          "a framed clip measures: " + error);
    if (results.size() == 1) {
        Check(results[0].width == 80, "the cropped half is what is measured: " +
                                          std::to_string(results[0].width));
        Check(results[0].picture.clipped >= 9500 &&
                  results[0].picture.crushed <= 500,
              "the white half the crop keeps: " +
                  std::to_string(results[0].picture.clipped));
    }

    // ---- the grade and colour management -------------------------------
    Document managed = document;
    managed.color_management.enabled = true;
    managed.color_management.input_gamut = "rec709";
    managed.color_management.input_transfer = "rec709";
    managed.color_management.input_ycbcr_matrix = "bt709";
    managed.color_management.input_range = "tv";
    plan.clip_ids = {kGreyClip};
    int64_t ungraded = 0;
    Check(RenderColorMeasurement(managed, directory, plan, settings, results,
                                 error) &&
              results.size() == 1 && results[0].picture.scene_p50,
          "the managed grey measures: " + error);
    if (results.size() == 1 && results[0].picture.scene_p50) {
        ungraded = results[0].picture.scene_p50.value_or(0);
        // Code 104 decodes to 0.4064, a hair under 18 %.
        Check(Between(ungraded, -15, 15),
              "the grey reads about 0 stops: " + std::to_string(ungraded));
    }
    ClipEffect exposure;
    exposure.type = "color.exposure";
    exposure.params["amount"] = {1, 1};
    managed.sequence.tracks[0].clips[1].effects = {exposure};
    Check(RenderColorMeasurement(managed, directory, plan, settings, results,
                                 error) &&
              results.size() == 1 && results[0].picture.scene_p50 &&
              Between(results[0].picture.scene_p50.value_or(0) - ungraded, 99,
                      101),
          "the clip's own grade moves it one stop: " + error);

    // ---- G3: a media read in its own input -----------------------------
    {
        Document declared = document;
        declared.color_management.enabled = true;
        declared.color_management.input_gamut = "sony_sgamut3_cine";
        declared.color_management.input_transfer = "sony_slog3";
        declared.FindLibraryMedia(kGrey)->color_input = {"rec709", "rec709",
                                                         "auto", "auto"};
        Document asProject = declared;
        asProject.FindLibraryMedia(kGrey)->color_input = {};
        asProject.color_management.input_gamut = "rec709";
        asProject.color_management.input_transfer = "rec709";
        ColorMeasurePlan both;
        both.clip_ids = {kGreyClip, kDarkClip};
        std::vector<ColorClipMeasurement> mine;
        std::vector<ColorClipMeasurement> theirs;
        Check(RenderColorMeasurement(declared, directory, both, settings, mine,
                                     error) &&
                  RenderColorMeasurement(asProject, directory, both, settings,
                                         theirs, error) &&
                  mine.size() == 2 && theirs.size() == 2,
              "G3: both projects measure: " + error);
        if (mine.size() == 2 && theirs.size() == 2) {
            const auto same = [](const ColorPictureMeasure& left,
                                 const ColorPictureMeasure& right) {
                return left.luma_p02 == right.luma_p02 &&
                       left.luma_p50 == right.luma_p50 &&
                       left.luma_p98 == right.luma_p98 &&
                       left.saturation_mean == right.saturation_mean &&
                       left.scene_p50 == right.scene_p50;
            };
            Check(same(mine[0].picture, theirs[0].picture),
                  "G3: the grey declared Rec.709 measures as in a Rec.709 "
                  "project: " +
                      std::to_string(mine[0].picture.luma_p50) + " / " +
                      std::to_string(theirs[0].picture.luma_p50));
            Check(!same(mine[1].picture, theirs[1].picture),
                  "G3: the dark clip, which declares nothing, is read with "
                  "the project's S-Log3");
            const std::string reply =
                ColorMeasurementsValue(declared, both, mine).Dump();
            Check(
                reply.find(R"("input":{"gamut":"rec709","transfer":"rec709",)"
                           R"("from":"media"})") != std::string::npos &&
                    reply.find(R"("transfer":"sony_slog3","from":"project")") !=
                        std::string::npos,
                "G3: each clip says the input it was read with, and "
                "whose: " +
                    reply);
        }
    }

    // ---- the sequence look, read from the package ----------------------
    const std::filesystem::path cube = root / "quarter.cube";
    {
        std::ofstream output(cube);
        output << "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
        for (int index = 0; index < 8; ++index) output << "0.25 0.25 0.25\n";
    }
    SequenceLut lut;
    Check(ImportLookLutIntoPackage((root / "project.cutmachine.json").string(),
                                   cube.string(), "", document.color_management,
                                   lut, error),
          "the look imports: " + error);
    Document looked = document;
    looked.sequence.look_lut = lut;
    plan.clip_ids = {kHalvesClip};
    Check(RenderColorMeasurement(looked, directory, plan, settings, results,
                                 error) &&
              results.size() == 1 &&
              Between(results[0].picture.luma_p50, 2499, 2501),
          "the look is laid after the grade: " + error);
    if (results.size() == 1) {
        const mcp_json::Value reply =
            ColorMeasurementsValue(looked, plan, results);
        const std::string text = reply.Dump();
        Check(text.find("\"look\":\"display\"") != std::string::npos,
              "the reply names the look's stage: " + text);
        Check(text.find("\"p50\":25") != std::string::npos,
              "25 % is written as the exact decimal 25");
    }

    // ---- G1b: the graded frame -----------------------------------------
    // The grey clip at 160x90 is shown at 512x288, read_frame's bound. Code
    // 105 is 0.4064, 103.6 in 8 bits; exposure 2 without colour management
    // adds 2/17.52 to the display signal, 132.7.
    FrameCaptureSettings frameSettings;
    frameSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
    std::string jpeg;
    std::vector<uint8_t> rgb;
    Check(CaptureGradedClipFrame(document, directory, kGreyClip, {12, 25},
                                 frameSettings, jpeg, error) &&
              DecodeJpeg(jpeg, root, 512, 288, rgb),
          "the graded frame renders at read_frame's size: " + error);
    if (!rgb.empty())
        Check(std::abs(MeanRed(rgb, 512, 200, 100, 64) - 103.6) < 2.5,
              "an ungraded clip shows its own grey: " +
                  std::to_string(MeanRed(rgb, 512, 200, 100, 64)));
    Document brightened = document;
    ClipEffect lift;
    lift.type = "color.exposure";
    lift.params["amount"] = {2, 1};
    brightened.sequence.tracks[0].clips[1].effects = {lift};
    Check(CaptureGradedClipFrame(brightened, directory, kGreyClip, {12, 25},
                                 frameSettings, jpeg, error) &&
              DecodeJpeg(jpeg, root, 512, 288, rgb),
          "the brightened frame renders: " + error);
    if (!rgb.empty())
        Check(std::abs(MeanRed(rgb, 512, 200, 100, 64) - 132.7) < 2.5,
              "the grade is in the picture: " +
                  std::to_string(MeanRed(rgb, 512, 200, 100, 64)));
    std::string plain;
    Check(CaptureSourceFrame((root / "grey.mp4").string(), {12, 25},
                             frameSettings, plain, error) &&
              DecodeJpeg(plain, root, 512, 288, rgb) &&
              std::abs(MeanRed(rgb, 512, 200, 100, 64) - 103.6) < 2.5,
          "the plain read_frame still shows the source ungraded");

    // Framed: the left (white) half, fitted into the 160x90 frame, is drawn
    // 80 pixels wide at x = 40, so 256 wide at x = 128 once the frame is
    // reduced to 512x288, with black on either side.
    Check(CaptureGradedClipFrame(framed, directory, kHalvesClip, {12, 25},
                                 frameSettings, jpeg, error) &&
              DecodeJpeg(jpeg, root, 512, 288, rgb),
          "a framed clip is composed into the sequence frame: " + error);
    if (!rgb.empty()) {
        Check(MeanRed(rgb, 512, 20, 120, 32) < 8.0 &&
                  MeanRed(rgb, 512, 460, 120, 32) < 8.0,
              "black where the sequence shows nothing of the clip");
        Check(MeanRed(rgb, 512, 240, 120, 32) > 247.0,
              "the white half where the framing draws it");
    }
    Check(!CaptureGradedClipFrame(document, directory, kAudioClip, {12, 25},
                                  frameSettings, jpeg, error),
          "a clip nobody sees has no graded frame");

    // ---- G2: matching a clip to a reference ----------------------------
    // Flat patches: once matched in the grading space, the clip's values are
    // the reference's, so every measure of the two pictures agrees.
    for (const bool managedChain : {false, true}) {
        Document matching = document;
        if (managedChain) matching.color_management = managed.color_management;
        const std::string label = managedChain ? "managed" : "unmanaged";
        ColorMatchPlan match;
        match.clip_id = kDarkClip;
        match.reference_clip_id = kGreyClip;
        ColorMatchResult matched;
        Check(RenderColorMatch(matching, directory, match, settings, matched,
                               error),
              label + ": a darker clip matches: " + error);
        Check(matched.grading_space == (managedChain ? "acescct" : "display"),
              label + ": solved where the grade acts");
        Check(std::abs(matched.after.picture.luma_p50 -
                       matched.reference.picture.luma_p50) <= 10 &&
                  std::abs(matched.before.picture.luma_p50 -
                           matched.reference.picture.luma_p50) > 500,
              label + ": the median comes to the reference's: " +
                  std::to_string(matched.before.picture.luma_p50) + " -> " +
                  std::to_string(matched.after.picture.luma_p50) + " for " +
                  std::to_string(matched.reference.picture.luma_p50));
        Check(matched.effects.size() == 1 &&
                  matched.effects[0].type == "color.exposure" &&
                  matched.placements.size() == 1 &&
                  !matched.placements[0].adjusted,
              label + ": a grey needs one exposure stage, added");

        match.clip_id = kWarmClip;
        Check(RenderColorMatch(matching, directory, match, settings, matched,
                               error),
              label + ": a warm clip matches: " + error);
        const ColorZoneMeasure& after = matched.after.picture.zones[1];
        const ColorZoneMeasure& target = matched.reference.picture.zones[1];
        Check(matched.before.picture.zones[1].warmth > 1000 &&
                  std::abs(after.warmth - target.warmth) <= 30 &&
                  std::abs(after.green_magenta - target.green_magenta) <= 30,
              label + ": the cast goes: warmth " +
                  std::to_string(matched.before.picture.zones[1].warmth) +
                  " -> " + std::to_string(after.warmth));
        Check(matched.knobs_after.kelvin < 6500,
              label + ": a warm clip is cooled");
        Check(matched.after.picture.saturation_mean <= 60,
              label + ": and left as grey as the reference: " +
                  std::to_string(matched.after.picture.saturation_mean));

        // Already graded: the match sets the clip's own exposure.
        Document graded = matching;
        ClipEffect own;
        own.type = "color.exposure";
        own.params["amount"] = {1, 2};
        graded.sequence.tracks[0].clips[2].effects = {own};
        match.clip_id = kDarkClip;
        Check(RenderColorMatch(graded, directory, match, settings, matched,
                               error) &&
                  matched.effects.size() == 1 &&
                  matched.placements.size() == 1 &&
                  matched.placements[0].adjusted &&
                  matched.knobs_before.exposure == 0.5,
              label + ": the match sets the clip's own exposure");
        const std::string reply =
            ColorMatchValue(graded, match, matched).Dump();
        Check(
            reply.find("\"tool\":\"set_clip_effects\"") != std::string::npos &&
                reply.find("\"placement\":\"adjusted\"") != std::string::npos,
            label + ": the reply carries the step: " + reply.substr(0, 200));
    }
    // A strong cast, which the offset temperature before G7 could not take
    // out of a display signal: the chromatic adaptation does.
    {
        ColorMatchPlan strong;
        strong.clip_id = kStrongClip;
        strong.reference_clip_id = kGreyClip;
        ColorMatchResult matched;
        Check(RenderColorMatch(document, directory, strong, settings, matched,
                               error) &&
                  !matched.kelvin_limited &&
                  std::abs(matched.after.picture.zones[1].warmth -
                           matched.reference.picture.zones[1].warmth) <= 30,
              "a strong cast is taken out: " + error + " " +
                  std::to_string(matched.before.picture.zones[1].warmth) +
                  " -> " +
                  std::to_string(matched.after.picture.zones[1].warmth));
    }
    ColorMatchPlan unseen;
    unseen.clip_id = kAudioClip;
    unseen.reference_clip_id = kGreyClip;
    ColorMatchResult nothing;
    Check(!RenderColorMatch(document, directory, unseen, settings, nothing,
                            error),
          "a clip nobody sees is not matched");

    // ---- GR15: the highlight ceiling of propose_grade ------------------
    // A rush whose left half is white and right half grey, read on a "face"
    // in the grey (the finder below says where). Its whites stand at about
    // 2.5 stops over 18 % grey.
    {
        constexpr char kLit[] = "01K31000000000000000000006";
        constexpr char kLitFirst[] = "01K31000000000000000000017";
        constexpr char kLitSecond[] = "01K31000000000000000000018";
        Check(Generate("color=c=white:s=80x90:r=25:d=1[a];"
                       "color=c=0x686868:s=80x90:r=25:d=1[b];[a][b]hstack",
                       root / "lit.mp4"),
              "the lit fixture is generated");
        Document lit = document;
        lit.color_management = managed.color_management;
        AddMedia(lit, kLit, "lit.mp4");
        lit.sequence.tracks.resize(1);
        lit.sequence.tracks[0].clips = {Clip(kLitFirst, kLit, 0),
                                        Clip(kLitSecond, kLit, 25)};
        SetFaceFinder(+[](int32_t, int32_t, const std::vector<uint8_t>&,
                          std::vector<ColorMeasureRegion>& faces,
                          std::string&) {
            ColorMeasureRegion face;
            face.present = true;
            face.x = {5, 8};
            face.y = {1, 4};
            face.width = {1, 4};
            face.height = {1, 2};
            faces = {face};
            return true;
        });
        GradeProposalPlan grade;
        grade.clip_ids = {kLitFirst, kLitSecond};
        grade.balance = false;
        std::vector<GradeProposal> proposals;

        // The scene rule, the ceiling under the whites: no lift, and each
        // clip says why, once its rush has voted as well as alone. On 6
        // November 2026 the vote turned such clips into "within_tolerance".
        grade.highlight_ceiling_hundredths = 200;
        for (const bool perSource : {true, false}) {
            grade.per_source = perSource;
            const std::string label =
                perSource ? "one grade per rush" : "clip by clip";
            Check(RenderGradeProposals(lit, directory, grade, settings,
                                       proposals, error) &&
                      proposals.size() == 2,
                  label + ": the lit rush is read: " + error);
            for (const GradeProposal& proposal : proposals)
                Check(proposal.anchor_used == "face" &&
                          proposal.correction_hundredths == 0 &&
                          proposal.reason == "limited_by_highlights" &&
                          proposal.wanted_hundredths > 50,
                      label + ": a face held back by the whites says so: " +
                          proposal.reason + ", wanted " +
                          std::to_string(proposal.wanted_hundredths));
        }

        // Under an HLG output the same lift to +1.5 stops: cut to a few
        // hundredths by the scene ceiling of 2.5 stops, whole under the
        // output's own clipping, which the lifted whites do not reach.
        lit.color_management.output_gamut = "rec2020";
        lit.color_management.output_transfer = "hlg";
        grade.per_source = true;
        grade.target_hundredths = 150;
        grade.highlight_ceiling_hundredths = 250;
        Check(RenderGradeProposals(lit, directory, grade, settings, proposals,
                                   error) &&
                  proposals.size() == 2 &&
                  proposals[0].reason == "limited_by_highlights" &&
                  proposals[0].correction_hundredths < 20,
              "HLG under the scene rule: the whites hold the face back: " +
                  error);
        grade.display_ceiling = true;
        Check(RenderGradeProposals(lit, directory, grade, settings, proposals,
                                   error) &&
                  proposals.size() == 2,
              "HLG under the output's clipping: the rush is read: " + error);
        for (const GradeProposal& proposal : proposals)
            Check(
                proposal.reason == "per_source" &&
                    proposal.correction_hundredths > 100 &&
                    proposal.after.picture.clipped <=
                        proposal.before.picture.clipped + kDisplayClipAllowance,
                "HLG under the output's clipping: the face is lifted "
                "whole and nothing more clips: " +
                    proposal.reason + ", " +
                    std::to_string(proposal.correction_hundredths) +
                    ", clipped " +
                    std::to_string(proposal.before.picture.clipped) + " -> " +
                    std::to_string(proposal.after.picture.clipped));
        SetFaceFinder(nullptr);
    }

    // ---- refusals ------------------------------------------------------
    ColorMeasurePlan bad = plan;
    bad.clip_ids = {"01K31000000000000000000099"};
    Check(!RenderColorMeasurement(document, directory, bad, settings, results,
                                  error) &&
              error.find("unknown clip_id") != std::string::npos,
          "an unknown clip is refused by name");
    bad.clip_ids = {kAudioClip};
    Check(!RenderColorMeasurement(document, directory, bad, settings, results,
                                  error) &&
              error.find("video track") != std::string::npos,
          "a clip of an audio track is refused: " + error);
    bad.clip_ids = {kHalvesClip};
    bad.position = "Somewhere";
    Check(!RenderColorMeasurement(document, directory, bad, settings, results,
                                  error),
          "an unknown position is refused");
    bad.position = "Middle";
    bad.clip_ids.clear();
    Check(!RenderColorMeasurement(document, directory, bad, settings, results,
                                  error),
          "a plan without clips is refused");

    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "color_measure_render: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
