// RC1, RC2 -- docs/roadmap/RACCORDS-2026-10.md. A review rendered for real:
// rushes made by FFmpeg's lavfi sources (pictures, noise, a tone), cut on a
// timeline so that each junction holds one thing to find, each window
// rendered through the export's graph by RenderCutInspection, measured and
// judged; the bands drawn and stacked into one picture.
//
//   frame 50   room A then room B, 20 dB louder: an ambience that jumps,
//              between two different pictures.
//   frame 100  room B then a rush in a room of the same level, whose tone
//              bursts start on the cut: a sound cut live, nothing else.
//   frame 160  that rush jumped 50 frames ahead, picture of one framing,
//              the cut in the middle of a burst on both sides: a jump cut,
//              and a sound cut live.
//   frame 210  that rush read on from where it stopped, inside a burst: a
//              through edit, which raises nothing.

#include "CutInspectionRender.h"
#include "CutReview.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

std::string Quote(const std::string& value) { return "'" + value + "'"; }

bool Run(const std::string& command) {
    return std::system((command + " < /dev/null").c_str()) == 0;
}

LibraryMedia Media(const Ulid& id, const std::filesystem::path& path,
                   int64_t frames) {
    LibraryMedia media;
    media.id = id;
    media.path = path.string();
    media.filename = path.filename().string();
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    media.width = 320;
    media.height = 180;
    media.rate = {25, 1};
    media.duration = {frames, 25};
    media.metadata_complete = true;
    return media;
}

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

// A rush of `seconds`: `video` and `audio` are lavfi graphs.
bool MakeRush(const std::filesystem::path& path, const std::string& video,
              const std::string& audio) {
    return Run(Quote(FFMPEG_EXECUTABLE) +
               " -hide_banner -loglevel error -f lavfi -i " + Quote(video) +
               " -f lavfi -i " + Quote(audio) +
               " -map 0:v -map 1:a -shortest -c:v libx264 -crf 0 -pix_fmt "
               "yuv420p -c:a pcm_f32le -y " +
               Quote(path.string()));
}

// The JPEG's width and height, from its SOF0 header.
void JpegSize(const std::string& jpeg, int& width, int& height) {
    width = 0;
    height = 0;
    for (size_t index = 0; index + 8 < jpeg.size(); ++index) {
        if (static_cast<unsigned char>(jpeg[index]) == 0xFF &&
            static_cast<unsigned char>(jpeg[index + 1]) == 0xC0) {
            height = (static_cast<unsigned char>(jpeg[index + 5]) << 8) |
                     static_cast<unsigned char>(jpeg[index + 6]);
            width = (static_cast<unsigned char>(jpeg[index + 7]) << 8) |
                    static_cast<unsigned char>(jpeg[index + 8]);
            return;
        }
    }
}

const JunctionReview* At(const CutReviewReport& report, int64_t frame) {
    for (const JunctionReview& review : report.reviews)
        if (review.junction.frame == frame) return &review;
    return nullptr;
}

bool HasAlert(const JunctionReview* review, const std::string& kind) {
    if (review == nullptr) return false;
    for (const CutReviewAlert& alert : review->alerts)
        if (alert.kind == kind) return true;
    return false;
}

void Print(const JunctionReview& review) {
    std::cout << "  frame " << review.junction.frame << ": ambience "
              << review.sound.ambience_before_dbfs.value_or(0) << " -> "
              << review.sound.ambience_after_dbfs.value_or(0) << ", voice "
              << review.sound.voice_before_dbfs.value_or(0) << " -> "
              << review.sound.voice_after_dbfs.value_or(0) << ", edges "
              << review.sound.edge_before_dbfs.value_or(0) << " / "
              << review.sound.edge_after_dbfs.value_or(0) << ", difference "
              << review.picture.difference_per_mille.value_or(-1)
              << ", histogram "
              << review.picture.histogram_per_mille.value_or(-1) << ", motion "
              << review.picture.motion_before_per_mille.value_or(-1) << " / "
              << review.picture.motion_after_per_mille.value_or(-1) << ':';
    for (const CutReviewAlert& alert : review.alerts)
        std::cout << ' ' << alert.kind;
    std::cout << '\n';
}

}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("cutmachine-review-tests-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    const auto roomA = directory / "room-a.mov";
    const auto roomB = directory / "room-b.mov";
    const auto tone = directory / "tone.mov";
    Check(MakeRush(roomA, "testsrc2=size=320x180:rate=25:duration=4",
                   "anoisesrc=r=48000:a=0.00316:c=white:seed=1:d=4,"
                   "aformat=channel_layouts=stereo"),
          "the rush in room A renders");
    Check(MakeRush(roomB, "smptebars=size=320x180:rate=25:duration=4",
                   "anoisesrc=r=48000:a=0.0316:c=white:seed=2:d=4,"
                   "aformat=channel_layouts=stereo"),
          "the rush in room B renders");
    // A still mire, room B's level of noise, and bursts of a tone from 1.0
    // to 1.8 s of every two seconds: like words, they leave the room
    // audible on either side of a cut.
    Check(MakeRush(tone, "testsrc=size=320x180:rate=25:duration=8",
                   "aevalsrc=0.3*sin(2*PI*440*t)*between(mod(t\\,2)\\,1."
                   "0\\,1.8)+0.0316*(2*random(0)-1):s=48000:d=8,"
                   "aformat=channel_layouts=stereo"),
          "the rush with a tone renders");

    const Ulid a = "01K9R100000000000000000001";
    const Ulid b = "01K9R100000000000000000002";
    const Ulid t = "01K9R100000000000000000003";
    Document document;
    document.sequence.id = "01K9R10000000000000000000S";
    document.sequence.width = 320;
    document.sequence.height = 180;
    document.sequence.frame_rate = {25, 1};
    document.library = {Media(a, roomA, 100), Media(b, roomB, 100),
                        Media(t, tone, 200)};
    document.sources = {{a, roomA.string(), {25, 1}, {100, 25}},
                        {b, roomB.string(), {25, 1}, {100, 25}},
                        {t, tone.string(), {25, 1}, {200, 25}}};
    const auto layout = [&](const char* prefix) {
        const std::string p(prefix);
        return std::vector<DocumentClip>{
            Clip(p + "0000000000000000000A", a, 0, 50, 0),
            Clip(p + "0000000000000000000B", b, 25, 50, 50),
            Clip(p + "0000000000000000000C", t, 25, 60, 100),
            Clip(p + "0000000000000000000D", t, 135, 50, 160),
            Clip(p + "0000000000000000000E", t, 185, 15, 210)};
    };
    document.sequence.tracks = {
        {"01K9R1000000000000000000T0", "video", 0, layout("01K9RV")},
        {"01K9R1000000000000000000T1", "audio", 1, layout("01K9RA")}};
    std::string error;
    Check(document.Validate(error), "the timeline validates: " + error);

    const std::filesystem::path project = directory / "project.cutmachine.json";
    const CutInspectionRenderer render =
        [&](const CutInspectionPlan& plan,
            const CutInspectionRenderSettings& settings,
            CutInspectionResult& result, std::string& renderError) {
            CutInspectionRenderSettings local = settings;
            local.ffmpeg_path = FFMPEG_EXECUTABLE;
            return RenderCutInspection(document, project, plan, local, result,
                                       renderError);
        };
    CutReviewRequest request;
    request.max_bands = 3;
    CutReviewReport report;
    std::vector<std::string> bands;
    const bool reviewed =
        ReviewCuts(document, request, render, report, bands, error);
    Check(reviewed, "the review renders: " + error);
    if (reviewed) {
        for (const JunctionReview& review : report.reviews) Print(review);
        Check(report.reviews.size() == 4, "four junctions reviewed");
        Check(HasAlert(At(report, 50), "ambience_jump"),
              "frame 50: the ambience jumps");
        Check(!HasAlert(At(report, 50), "jump_cut"),
              "frame 50: two pictures, no jump cut");
        Check(At(report, 100) != nullptr &&
                  At(report, 100)->alerts.size() == 1 &&
                  HasAlert(At(report, 100), "sound_cut_live"),
              "frame 100: the tone starts on the cut, and only that");
        Check(HasAlert(At(report, 160), "jump_cut"),
              "frame 160: one framing on both sides");
        Check(HasAlert(At(report, 160), "sound_cut_live"),
              "frame 160: the cut falls inside a burst");
        Check(At(report, 210) != nullptr &&
                  At(report, 210)->junction.picture_continuous &&
                  At(report, 210)->junction.sound_continuous &&
                  At(report, 210)->alerts.empty(),
              "frame 210: a through edit inside a burst raises nothing");
        Check(report.bands == std::vector<int64_t>({50, 100, 160}) &&
                  bands.size() == 3,
              "three bands, the flagged junctions");

        // Each band: three pictures of 240x134 on either side, 4 px apart,
        // an 8 px bar; the waveform 66 px under them.
        int width = 0;
        int height = 0;
        if (!bands.empty()) JpegSize(bands[0], width, height);
        Check(width == 1464 && height == 200, "a band is 1464x200 (" +
                                                  std::to_string(width) + "x" +
                                                  std::to_string(height) + ")");
        std::string sheet;
        Check(StackInspectionPictures(bands, FFMPEG_EXECUTABLE, sheet, error),
              "the bands stack: " + error);
        JpegSize(sheet, width, height);
        Check(width == 1464 && height == 616,
              "three bands and two gaps are 1464x616 (" +
                  std::to_string(width) + "x" + std::to_string(height) + ")");
        if (std::getenv("CUTMACHINE_REVIEW_SHEET") != nullptr) {
            const std::string path = std::getenv("CUTMACHINE_REVIEW_SHEET");
            FILE* file = std::fopen(path.c_str(), "wb");
            if (file != nullptr) {
                std::fwrite(sheet.data(), 1, sheet.size(), file);
                std::fclose(file);
            }
        }
    }

    // inspect_cut's band: the same picture for one cut, from its own plan.
    CutInspectionRequest inspection;
    inspection.position = RationalTime{160, 25};
    CutInspectionPlan plan;
    Check(PlanCutInspection(document, inspection, plan, error),
          "inspection plan: " + error);
    CutInspectionRenderSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.cut_band = true;
    CutInspectionResult result;
    Check(RenderCutInspection(document, project, plan, settings, result, error),
          "inspect_cut renders a band: " + error);
    int width = 0;
    int height = 0;
    JpegSize(result.jpeg, width, height);
    // Six pictures, three on either side, at inspect_cut's 360 px.
    Check(width == 2184 && height == 302, "inspect_cut's band is 2184x302 (" +
                                              std::to_string(width) + "x" +
                                              std::to_string(height) + ")");

    std::filesystem::remove_all(directory);
    if (failures == 0) std::cout << "All cut review render tests passed\n";
    return failures == 0 ? 0 : 1;
}
