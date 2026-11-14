// IA4 -- docs/roadmap/IA-2026-09.md. An inspection rendered for real: the
// window goes through the export's graph, the strip and the levels come back
// from it. The frames themselves are held to the film's, byte for byte, by
// export_tests (CheckWindowRender); this suite checks what the inspection
// makes of them.
//
// The last part reads real speech with the local Whisper model, which a
// build machine may not have and which takes over a minute: it runs when
// CUTMACHINE_TEST_WHISPER is set and a model is configured, and is skipped,
// saying so, otherwise. It is the ticket's fixture -- a weak question
// followed by a strong answer, then a cut through a word -- spoken by the
// system's French voices.

#include "CutInspection.h"
#include "CutInspectionRender.h"
#include "Transcription.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

// A tone for two seconds, then silence: the levels on either side of a cut
// at frame 50 are known without a model.
void CheckLevelsAndStrip(const std::filesystem::path& directory) {
    const auto rush = directory / "tone.mov";
    Check(Run(Quote(FFMPEG_EXECUTABLE) +
              " -hide_banner -loglevel error -f lavfi -i "
              "'testsrc2=size=320x180:rate=25:duration=4' -f lavfi -i "
              "'aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=48000:"
              "d=2:c=stereo' -f lavfi -i 'anullsrc=r=48000:cl=stereo:d=2' "
              "-filter_complex '[1:a][2:a]concat=n=2:v=0:a=1[a]' -map 0:v "
              "-map '[a]' -c:v "
              "libx264 -crf 0 -pix_fmt yuv420p -c:a pcm_f32le -y " +
              Quote(rush.string())),
          "levels: the rush renders");
    Document document;
    document.sequence.id = "01K9H00000000000000000000S";
    document.sequence.width = 320;
    document.sequence.height = 180;
    document.sequence.frame_rate = {25, 1};
    const Ulid source = "01K9H000000000000000000001";
    document.library = {Media(source, rush, 100)};
    document.sources = {{source, rush.string(), {25, 1}, {100, 25}}};
    document.sequence.tracks = {
        {"01K9H0000000000000000000T0",
         "video",
         0,
         {Clip("01K9H00000000000000000000A", source, 0, 100, 0)}},
        {"01K9H0000000000000000000T1",
         "audio",
         1,
         {Clip("01K9H00000000000000000000B", source, 0, 100, 0)}}};
    std::string error;
    Check(document.Validate(error), "levels: the timeline validates: " + error);

    CutInspectionRequest request;
    request.position = RationalTime{50, 25};
    CutInspectionPlan plan;
    Check(PlanCutInspection(document, request, plan, error),
          "levels: plan: " + error);
    CutInspectionRenderSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    CutInspectionResult result;
    const bool rendered =
        RenderCutInspection(document, directory / "project.cutmachine.json",
                            plan, settings, result, error);
    Check(rendered, "levels: the inspection renders: " + error);
    if (!rendered) return;
    Check(result.audio.frame_rms_dbfs.size() == 50,
          "levels: one level per frame of the window");
    Check(result.audio.before_cut_rms_dbfs == -9,
          "levels: the tone before the cut is -9 dBFS RMS (" +
              std::to_string(result.audio.before_cut_rms_dbfs.value_or(0)) +
              ")");
    Check(result.audio.after_cut_rms_dbfs == kCutInspectionSilenceDbfs,
          "levels: silence after the cut");
    Check(result.audio.window_peak_dbfs == -6,
          "levels: the window's peak is the tone's, -6 dBFS (" +
              std::to_string(result.audio.window_peak_dbfs) + ")");
    Check(!result.words.expected_unavailable.empty() &&
              result.words.expected.empty(),
          "levels: a project without transcripts says it has none");

    // Six pictures: three by two cells of 360 by 202, four pixels between.
    // Read from the JPEG's own frame header (SOF0): height then width.
    int width = 0;
    int height = 0;
    const std::string& jpeg = result.jpeg;
    for (size_t index = 0; index + 8 < jpeg.size(); ++index) {
        if (static_cast<unsigned char>(jpeg[index]) == 0xFF &&
            static_cast<unsigned char>(jpeg[index + 1]) == 0xC0) {
            height = (static_cast<unsigned char>(jpeg[index + 5]) << 8) |
                     static_cast<unsigned char>(jpeg[index + 6]);
            width = (static_cast<unsigned char>(jpeg[index + 7]) << 8) |
                    static_cast<unsigned char>(jpeg[index + 8]);
            break;
        }
    }
    Check(width == 1088 && height == 408,
          "levels: the strip is three by two cells (" + std::to_string(width) +
              "x" + std::to_string(height) + ")");
}

// 6 November 2026 -- a window rendered at twice its largest picture rather
// than at the sequence's size measures what the full-size render measures.
// Uses the rush CheckLevelsAndStrip wrote, on a 1280x720 sequence.
void CheckReducedRender(const std::filesystem::path& directory) {
    const auto rush = directory / "tone.mov";
    Document document;
    document.sequence.id = "01K9H00000000000000000001S";
    document.sequence.width = 1280;
    document.sequence.height = 720;
    document.sequence.frame_rate = {25, 1};
    const Ulid source = "01K9H000000000000000000011";
    document.library = {Media(source, rush, 100)};
    document.sources = {{source, rush.string(), {25, 1}, {100, 25}}};
    document.sequence.tracks = {
        {"01K9H0000000000000000001T0",
         "video",
         0,
         {Clip("01K9H00000000000000000001A", source, 0, 50, 0),
          Clip("01K9H00000000000000000001C", source, 70, 30, 50)}},
        {"01K9H0000000000000000001T1",
         "audio",
         1,
         {Clip("01K9H00000000000000000001B", source, 0, 80, 0)}}};
    std::string error;
    Check(document.Validate(error),
          "reduced: the timeline validates: " + error);
    CutInspectionRequest request;
    request.position = RationalTime{50, 25};
    CutInspectionPlan plan;
    Check(PlanCutInspection(document, request, plan, error),
          "reduced: plan: " + error);
    const auto render = [&](int32_t cell, CutInspectionResult& result) {
        CutInspectionRenderSettings settings;
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        settings.cell_dimension = cell;
        settings.analysis_dimension = 160;
        return RenderCutInspection(document,
                                   directory / "project.cutmachine.json", plan,
                                   settings, result, error);
    };
    // A 240-pixel cell renders the window at 480x270; a 640-pixel one at
    // the sequence's own 1280x720.
    CutInspectionResult reduced;
    CutInspectionResult full;
    Check(render(240, reduced), "reduced: the small render runs: " + error);
    Check(render(640, full), "reduced: the full render runs: " + error);
    Check(reduced.analysis_width == 160 && reduced.analysis_height == 90 &&
              full.analysis_width == 160 && full.analysis_height == 90,
          "reduced: the analysis planes keep their size");
    Check(!full.analysis_planes.empty() &&
              reduced.analysis_planes.size() == full.analysis_planes.size(),
          "reduced: both renders keep the same frames");
    double difference = 0.0;
    size_t count = 0;
    for (size_t frame = 0; frame < full.analysis_planes.size() &&
                           frame < reduced.analysis_planes.size();
         ++frame) {
        const std::vector<uint8_t>& a = full.analysis_planes[frame];
        const std::vector<uint8_t>& b = reduced.analysis_planes[frame];
        for (size_t index = 0; index < a.size() && index < b.size(); ++index) {
            difference += std::abs(static_cast<int>(a[index]) - b[index]);
            ++count;
        }
    }
    const double mean = count > 0 ? difference / count : 255.0;
    Check(mean < 2.0,
          "reduced: the planes match the full render within two "
          "levels on average (" +
              std::to_string(mean) + ")");
    Check(reduced.audio.before_cut_rms_dbfs == full.audio.before_cut_rms_dbfs,
          "reduced: the sound is the same");
}

// The ticket's fixture, with real speech and the real model.
void CheckSpokenInterview(const std::filesystem::path& directory) {
    if (std::getenv("CUTMACHINE_TEST_WHISPER") == nullptr) {
        std::cout << "SKIP: spoken interview (set CUTMACHINE_TEST_WHISPER to "
                     "read real speech with the local model)\n";
        return;
    }
    std::string model;
    std::string reason;
    if (!ResolveConfiguredWhisperModel(model, reason)) {
        std::cout << "SKIP: spoken interview: " << reason << '\n';
        return;
    }
    const auto question = directory / "question.aiff";
    const auto answer = directory / "answer.aiff";
    if (!Run("say -v 'Flo (Français (France))' -o " + Quote(question.string()) +
             " \"Qu'est-ce qui vous a amené à la mode ?\"") ||
        !Run("say -v 'Eddy (Français (France))' -o " + Quote(answer.string()) +
             " \"J'ai toujours aimé dessiner des vêtements, depuis que je "
             "suis tout petit.\"")) {
        std::cout << "SKIP: spoken interview: the French voices are not "
                     "installed\n";
        return;
    }
    // Question, 0.6 s of silence, answer; picture under it.
    const auto rush = directory / "dialog.mov";
    Check(Run(Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
              Quote(question.string()) + " -i " + Quote(answer.string()) +
              " -f lavfi -i 'anullsrc=r=48000:cl=stereo:d=0.6' -f lavfi -i "
              "'testsrc2=size=320x180:rate=25:duration=6.36' -filter_complex "
              "'[0:a]aresample=48000,aformat=channel_layouts=stereo[q];"
              "[1:a]aresample=48000,aformat=channel_layouts=stereo[r];"
              "[q][2:a][r]concat=n=3:v=0:a=1[s]' -map 3:v -map '[s]' "
              "-shortest -c:v libx264 -crf 18 -pix_fmt yuv420p -c:a pcm_s16le "
              "-y " +
              Quote(rush.string())),
          "speech: the rush renders");
    const Ulid source = "01K9H000000000000000000002";
    const auto project = directory / "speech" / "project.cutmachine.json";
    const auto transcript = project.parent_path() / ".cutmachine" /
                            "transcripts" / (source + ".json");
    WhisperSettings whisper;
    whisper.whisper_model_path = model;
    whisper.ffmpeg_path = FFMPEG_EXECUTABLE;
    whisper.language = "fr";
    std::string error;
    MediaTaskManager tasks(1);
    const MediaTaskSnapshot snapshot = RunMediaTaskAndWait(
        tasks, MediaTaskKind::Transcription, "source",
        [&](MediaTaskContext& context, std::string& taskError) {
            return GenerateAudioTranscript(rush.string(), transcript.string(),
                                           source, {25, 1}, whisper, context,
                                           taskError);
        });
    Check(snapshot.state == MediaTaskState::Succeeded,
          "speech: the rush is transcribed: " + snapshot.error);
    if (snapshot.state != MediaTaskState::Succeeded) return;

    // A question at -45 dB, cut in the silence at frame 60, then the answer.
    Document document;
    document.sequence.id = "01K9H00000000000000000001S";
    document.sequence.width = 320;
    document.sequence.height = 180;
    document.sequence.frame_rate = {25, 1};
    document.library = {Media(source, rush, 159)};
    document.sources = {{source, rush.string(), {25, 1}, {159, 25}}};
    DocumentClip weak = Clip("01K9H00000000000000000001B", source, 0, 60, 0);
    weak.audio_gain_db = {-45, 1};
    DocumentClip strong =
        Clip("01K9H00000000000000000001C", source, 60, 99, 60);
    document.sequence.tracks = {
        {"01K9H0000000000000000001T0",
         "video",
         0,
         {Clip("01K9H00000000000000000001A", source, 0, 159, 0)}},
        {"01K9H0000000000000000001T1", "audio", 1, {weak, strong}}};
    Check(document.Validate(error), "speech: the timeline validates: " + error);
    CutInspectionRequest request;
    request.clip_id = strong.id;
    request.before = {12, 5};
    request.after = {4, 1};
    CutInspectionPlan plan;
    Check(PlanCutInspection(document, request, plan, error),
          "speech: plan: " + error);
    CutInspectionRenderSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.transcribe = true;
    settings.language = "fr";
    CutInspectionResult result;
    Check(RenderCutInspection(document, project, plan, settings, result, error),
          "speech: the inspection renders and reads: " + error);
    bool questionMissed = false;
    bool answerMissed = false;
    for (const CutInspectionAlert& alert : result.words.alerts) {
        std::cout << "  weak question: " << alert.kind << ' ' << alert.text
                  << " at " << alert.timeline_in.value << '\n';
        if (alert.kind != "expected_not_heard") continue;
        if (alert.timeline_in < RationalTime{60, 25}) questionMissed = true;
        if (!(alert.timeline_in < RationalTime{60, 25})) answerMissed = true;
    }
    Check(result.words.transcribed && questionMissed,
          "speech: the question at -45 dB is reported as not heard");
    Check(!answerMissed, "speech: the answer raises no alert");

    // Fourteen frames taken out of the middle of "vêtements".
    document.sequence.tracks[1].clips[1].duration = {40, 25};
    document.sequence.tracks[1].clips.push_back(
        Clip("01K9H00000000000000000001D", source, 114, 45, 100));
    document.sequence.tracks[0].clips[0].duration = {145, 25};
    Check(document.Validate(error),
          "speech: the cut timeline validates: " + error);
    request = CutInspectionRequest{};
    request.clip_id = "01K9H00000000000000000001D";
    request.before = {2, 1};
    request.after = {2, 1};
    Check(PlanCutInspection(document, request, plan, error),
          "speech: plan of the word cut: " + error);
    Check(RenderCutInspection(document, project, plan, settings, result, error),
          "speech: the word cut renders and reads: " + error);
    bool crossing = false;
    for (const CutInspectionAlert& alert : result.words.alerts) {
        std::cout << "  word cut: " << alert.kind << ' ' << alert.text << " at "
                  << alert.timeline_in.value << '\n';
        if (alert.kind == "word_crosses_cut") crossing = true;
    }
    Check(crossing, "speech: the word the cut runs through is named");
}

}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("cutmachine-inspection-tests-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    CheckLevelsAndStrip(directory);
    CheckReducedRender(directory);
    CheckSpokenInterview(directory);
    std::filesystem::remove_all(directory);
    if (failures == 0) std::cout << "All cut inspection render tests passed\n";
    return failures == 0 ? 0 : 1;
}
