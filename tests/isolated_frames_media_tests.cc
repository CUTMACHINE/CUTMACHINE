#include "Cli.h"
#include "Document.h"
#include "Export.h"
#include "ExportVerification.h"
#include "Ingest.h"
#include "IsolatedFrames.h"
#include "Json.h"
#include "MediaTaskManager.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "TestSupport.h"

// R8 -- docs/roadmap/REEL-2026-09.md. The decode half. Everything about
// which frames are isolated is settled in isolated_frames_tests.cc on
// synthetic series; this file proves the pass actually feeds them the frames
// it says it does -- that a flash at frame 120 of the file is reported at
// frame 120 and not at the start of the range it was found in, which is the
// one thing a select expression and a muxer's cadence can silently ruin.

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

bool RunScan(const std::filesystem::path& input,
             const std::filesystem::path& output, const std::string& mediaId,
             MediaRate rate, const std::vector<SourceFrameSpan>& spans,
             const IsolatedFrameMeasureSettings& settings) {
    MediaTaskManager manager;
    manager.Enqueue(MediaTaskKind::IsolatedFrames, "isolated frames",
                    [&](MediaTaskContext& context, std::string& taskError) {
                        return GenerateIsolatedFrameScan(
                            input.string(), output.string(), mediaId, rate,
                            spans, settings, context, taskError);
                    });
    return manager.WaitForIdle(600000);
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-r8");
    std::filesystem::create_directories(root);
    IsolatedFrameMeasureSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;

    // A four second take at 50 i/s with one frame lit by a studio flash:
    // frame 120 raised by about eighty luma levels, the figure the first
    // film of the lot measured. The take moves, so the neighbourhood the
    // detector judges against is not a frozen picture.
    const std::filesystem::path flash = root / "flash.mp4";
    const std::string makeFlash =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=640x360:rate=50:duration=4' -vf " +
        Quote("eq=brightness=0.31:enable='eq(n,120)'") +
        " -c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv420p -y " +
        Quote(flash);
    if (std::system(makeFlash.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate the flash fixture\n";
        std::filesystem::remove_all(root);
        return 1;
    }

    const std::string mediaId = "01KR00000000000000000000F5";
    const std::filesystem::path cache = root / "cache" / "flash.json";
    // Two ranges with a gap between them, as a timeline that uses two pieces
    // of one rush produces. The flash sits in the second: if the select
    // expression or the muxer's cadence lost the gap, it would be reported
    // eleven frames early and this is where that shows.
    const std::vector<SourceFrameSpan> spans = {{20, 60}, {100, 180}};
    if (!RunScan(flash, cache, mediaId, {50, 1}, spans, settings)) {
        std::cerr << "FAIL: the isolated frame scan timed out\n";
        std::filesystem::remove_all(root);
        return 1;
    }

    IsolatedFrameScan scan;
    std::string error;
    Check(LoadIsolatedFrameScan(cache.string(), scan, error),
          "the scan wrote a readable cache: " + error);
    Check(scan.media_id == mediaId && scan.rate.num == 50,
          "carrying the identity and the frame grid it was asked for");
    Check(scan.series.size() == 2 && scan.series[0].first_frame == 20 &&
              scan.series[0].frames.size() == 40 &&
              scan.series[1].first_frame == 100 &&
              scan.series[1].frames.size() == 80,
          "and exactly the two ranges asked for, with their own frame "
          "numbers");

    IsolatedFrameSettings detection;
    std::vector<IsolatedFrameEvent> events;
    for (const IsolatedFrameSeries& series : scan.series)
        for (const IsolatedFrameEvent& event :
             DetectIsolatedFrames(series, detection))
            events.push_back(event);
    Check(events.size() == 1,
          "one isolated frame in four seconds of real decoded frames, got " +
              std::to_string(events.size()));
    if (events.size() == 1) {
        Check(events[0].first_frame == 120 && events[0].frames == 1,
              "at the frame of the file it was put on, got " +
                  std::to_string(events[0].first_frame));
        Check(
            events[0].deviation > 40 * kIsolatedFrameLumaScale,
            "with the deviation it was encoded with, " +
                std::to_string(events[0].deviation / kIsolatedFrameLumaScale) +
                " levels");
    }

    // Acceptance criterion 4: two passes over the same media write the same
    // bytes. This is what makes a cached measurement worth keeping.
    {
        const std::filesystem::path again = root / "cache" / "again.json";
        Check(RunScan(flash, again, mediaId, {50, 1}, spans, settings),
              "the repeat scan finishes");
        std::ifstream first(cache, std::ios::binary);
        std::ifstream second(again, std::ios::binary);
        const std::string firstBytes((std::istreambuf_iterator<char>(first)),
                                     std::istreambuf_iterator<char>());
        const std::string secondBytes((std::istreambuf_iterator<char>(second)),
                                      std::istreambuf_iterator<char>());
        Check(!firstBytes.empty() && firstBytes == secondBytes,
              "scanning the same media twice produces identical bytes");
    }

    // The same take with no flash at all: the pass has to stay quiet on
    // ordinary moving footage, which is the half of criterion 2 that no
    // synthetic series can prove about the decode.
    {
        const std::filesystem::path plain = root / "plain.mp4";
        const std::string makePlain =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi -i "
            "'testsrc2=size=640x360:rate=50:duration=4'"
            " -c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv420p -y " +
            Quote(plain);
        if (std::system(makePlain.c_str()) != 0) {
            std::cerr << "FAIL: unable to generate the quiet fixture\n";
            std::filesystem::remove_all(root);
            return 1;
        }
        const std::filesystem::path quietCache = root / "cache" / "plain.json";
        Check(RunScan(plain, quietCache, "01KR00000000000000000000P5", {50, 1},
                      {{0, 200}}, settings),
              "the quiet fixture scans");
        IsolatedFrameScan quiet;
        Check(LoadIsolatedFrameScan(quietCache.string(), quiet, error),
              "and reads back: " + error);
        int64_t found = 0;
        for (const IsolatedFrameSeries& series : quiet.series)
            found += static_cast<int64_t>(
                DetectIsolatedFrames(series, detection).size());
        Check(found == 0,
              "four seconds of ordinary moving footage raise no alarm, got " +
                  std::to_string(found));
    }

    // Acceptance criterion 3: what a minute of 4K at 50 i/s costs. Measured
    // rather than asserted -- a wall clock in a test suite is a flaky
    // threshold, and the figure is what the ticket asks to record. Two
    // seconds of 3840x2160 at 50 i/s, scanned end to end, extrapolated.
    {
        const std::filesystem::path uhd = root / "uhd.mp4";
        const std::string makeUhd =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi -i "
            "'testsrc2=size=3840x2160:rate=50:duration=2'"
            " -c:v libx264 -preset ultrafast -crf 30 -pix_fmt yuv420p -y " +
            Quote(uhd);
        if (std::system(makeUhd.c_str()) != 0) {
            std::cerr << "WARN: unable to generate the 4K fixture; the cost "
                         "per minute was not measured\n";
        } else {
            const std::filesystem::path uhdCache = root / "cache" / "uhd.json";
            const auto started = std::chrono::steady_clock::now();
            Check(RunScan(uhd, uhdCache, "01KR00000000000000000000H4", {50, 1},
                          {{0, 100}}, settings),
                  "the 4K fixture scans");
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                    .count();
            IsolatedFrameScan uhdScan;
            Check(LoadIsolatedFrameScan(uhdCache.string(), uhdScan, error),
                  "and reads back: " + error);
            int64_t frames = 0;
            for (const IsolatedFrameSeries& series : uhdScan.series)
                frames += static_cast<int64_t>(series.frames.size());
            std::cout << "R8 cost: " << frames
                      << " frames of 3840x2160 at 50 i/s in " << seconds
                      << " s, i.e. "
                      << (frames > 0 ? seconds * 3000 / frames : 0.0)
                      << " s per minute of media\n";
            std::ifstream size(uhdCache, std::ios::binary | std::ios::ate);
            std::cout << "R8 cache: " << size.tellg() << " bytes for " << frames
                      << " frames\n";
        }
    }

    // Acceptance criterion 1, its second half, on real pixels: the flash
    // falls inside a dissolve handle, so the delivered file shows it at half
    // opacity over the outgoing shot. verify_export has to find it there --
    // and those are exactly the frames its own comparison counts
    // unverifiable, because two layers are mixed and no single source frame
    // is the right answer.
    {
        Document document;
        document.sequence.width = 640;
        document.sequence.height = 360;
        document.sequence.frame_rate = {25, 1};
        LibraryMedia media;
        media.id = mediaId;
        media.path = flash.string();
        media.filename = "flash.mp4";
        media.codec = "h264";
        media.width = 640;
        media.height = 360;
        media.rate = {50, 1};
        media.duration = {200, 50};
        media.has_video = true;
        media.orientation = "landscape";
        media.metadata_complete = true;
        document.library = {media};
        document.sources = {{mediaId, flash.string(), {50, 1}, {200, 50}}};
        DocumentTrack track{"01KR00000000000000000000T0", "video", 0, {}};
        DocumentClip outgoing;
        outgoing.id = "01KR00000000000000000000A1";
        outgoing.source_id = mediaId;
        outgoing.source_in = {0, 50};
        outgoing.duration = {30, 25};
        outgoing.timeline_in = {0, 25};
        outgoing.include_audio = false;
        DocumentClip incoming;
        incoming.id = "01KR00000000000000000000B1";
        incoming.source_id = mediaId;
        // The flash is this clip's first frame, so a centred dissolve draws
        // it at exactly half opacity.
        incoming.source_in = {120, 50};
        incoming.duration = {30, 25};
        incoming.timeline_in = {30, 25};
        incoming.include_audio = false;
        track.clips = {outgoing, incoming};
        document.sequence.tracks = {track};
        DocumentTransition dissolve;
        dissolve.id = "01KR00000000000000000000D1";
        dissolve.track_id = track.id;
        dissolve.left_clip_id = outgoing.id;
        dissolve.right_clip_id = incoming.id;
        dissolve.duration = {12, 25};
        dissolve.alignment = TransitionAlignment::Center;
        document.sequence.transitions = {dissolve};
        std::string validationError;
        Check(document.Validate(validationError),
              "the dissolve fixture is a valid document: " + validationError);

        // The scan of the rushes must reach that frame at all, which is what
        // the transition handles in the plan are for: the flash sits six
        // sequence frames before the incoming clip's own first frame.
        std::vector<IsolatedFrameScanTarget> targets;
        std::string planError;
        Check(PlanIsolatedFrameScan(document, IsolatedFrameScanSettings{},
                                    targets, planError),
              "the dissolve timeline plans: " + planError);
        bool reaches = false;
        for (const IsolatedFrameScanTarget& target : targets)
            for (const SourceFrameSpan& span : target.spans)
                if (span.first_frame <= 108 && span.end_frame > 108)
                    reaches = true;
        Check(reaches,
              "the planned ranges reach into the dissolve's head handle");

        const std::filesystem::path exported = root / "dissolve.mp4";
        ExportSettings settings2 = Exporter::H264Preset(exported.string());
        settings2.encoder = ExportEncoder::H264Software;
        settings2.width = 640;
        settings2.height = 360;
        settings2.frame_rate = {25, 1};
        settings2.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan plan;
        std::string exportError;
        const bool rendered =
            Exporter::BuildPlan(document, (root / "p.json").string(), settings2,
                                plan, exportError) &&
            Exporter::Run(plan, {}, nullptr, exportError);
        Check(rendered, "the dissolve timeline exports: " + exportError);
        if (rendered) {
            ExportVerificationSettings verification;
            verification.ffmpeg_path = FFMPEG_EXECUTABLE;
            ExportVerificationReport report;
            Check(VerifyExport(document, root, exported.string(), verification,
                               report, exportError),
                  "verify_export runs on it: " + exportError);
            Check(report.unverifiable_frames > 0,
                  "the dissolve leaves frames its comparison cannot judge");
            Check(report.isolated_frames.size() == 1,
                  "and verify_export finds one isolated frame in the "
                  "delivered file, got " +
                      std::to_string(report.isolated_frames.size()));
            if (report.isolated_frames.size() == 1) {
                const IsolatedFrameEvent& event = report.isolated_frames[0];
                Check(event.first_frame == 30 && event.frames == 1,
                      "on the export frame the dissolve draws it at, got " +
                          std::to_string(event.first_frame));
                Check(event.deviation > 20 * kIsolatedFrameLumaScale &&
                          event.deviation < 60 * kIsolatedFrameLumaScale,
                      "at about half the deviation the rush holds, " +
                          std::to_string(event.deviation /
                                         kIsolatedFrameLumaScale) +
                          " levels");
            }
            Check(SerializeExportVerification(report).find(
                      "\"isolated_frames\"") != std::string::npos,
                  "and the report publishes them");
        }
    }

    // The derush: a rush no timeline uses, scanned whole through the
    // command the CLI, the MCP server and the chat all run. The flash at
    // frame 120 is found although nothing has been cut from the take.
    {
        const std::filesystem::path derush = root / "derush";
        std::filesystem::create_directories(derush);
        std::filesystem::copy_file(flash, derush / "take.mp4");
        std::string projectPath;
        std::string projectError;
        Check(
            CreatePortableProject((root / "Derush.cutmachine-project").string(),
                                  Project("Derush"), projectPath, projectError),
            "the derush project is created: " + projectError);
        std::string output;
        Check(IngestCommand(projectPath, (derush / "take.mp4").string(), false,
                            output) == 0,
              "the take is ingested: " + output);
        Project project;
        Check(LoadStoredProject(projectPath, project, projectError) &&
                  project.rushes.size() == 1,
              "the project holds the take and no cut: " + projectError);
        const std::string takeId =
            project.rushes.empty() ? std::string() : project.rushes[0].id;
        Check(IsolatedFramesCommand(projectPath, "", true, output, {},
                                    {takeId}) == 0,
              "the take is scanned whole: " + output);
        mcp_json::Value report;
        std::string parseError;
        bool found = false;
        if (mcp_json::Value::Parse(output, report, parseError)) {
            const mcp_json::Value* sources = report.Find("sources");
            if (sources != nullptr && sources->IsArray())
                for (const mcp_json::Value& entry : sources->AsArray()) {
                    const mcp_json::Value* events =
                        entry.Find("isolated_frames");
                    if (events == nullptr || !events->IsArray()) continue;
                    for (const mcp_json::Value& event : events->AsArray()) {
                        const mcp_json::Value* first =
                            event.Find("first_frame");
                        int64_t frame = -1;
                        found |= first != nullptr && first->AsInt64(frame) &&
                                 frame == 120;
                    }
                }
        }
        Check(found,
              "the flash is reported at frame 120 of the take: " + output);
        Check(IsolatedFramesCommand(projectPath, "", false, output, {},
                                    {"01KR00000000000000000000ZZ"}) != 0 &&
                  output.find("UnknownMedia") != std::string::npos,
              "an unknown media is refused by name: " + output);
    }

    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "isolated frame media tests passed\n";
    return failures == 0 ? 0 : 1;
}
