// STRUCTURE-2026-11 ST1 -- docs/roadmap/STRUCTURE-2026-11.md. The entry of
// cutmachine_ui_smoke_tests, which links the editor's own objects: it reads
// the run's flags, installs the hooks through which the run observes the
// editor and answers in a person's place (AppTestHooks.h), then hands over
// to the editor's body. Each hook does what main.mm, BrowserViews.mm and
// VideoOutput.mm did under CUTMACHINE_UI_SMOKE_TEST when the test compiled
// them a second time.

#include "UiSmokeHarness.h"

namespace cutmachine_app {
namespace {

class UiSmokeHooks final : public AppTestHooks {
public:
    void MediaIconPressed() override {
        if (gUiSmokeTesting) gUiSmokeIconMouseDown = true;
    }
    void MediaDragWritten(NSUInteger mediaCount) override {
        if (gUiSmokeTesting) gUiSmokeMediaDragCount = mediaCount;
    }
    void MediaDragSessionBegins() override {
        if (gUiSmokeTesting) gUiSmokeIconDragSession = true;
    }
    void EditorWindowShown(size_t decoders) override {
        if (gUiSmokeTesting) gUiSmokeWorkersAtWindowShow = decoders;
    }
    void EditorReady(AppDelegate* editor) override {
        if (gUiSmokeTesting)
            dispatch_async(dispatch_get_main_queue(), ^{
              [editor runUiSmokeTests];
            });
        if (!gUiPlayProbeProject.empty())
            dispatch_async(dispatch_get_main_queue(), ^{
              [editor runPlayProbe];
            });
    }
    void DecoderOpening(const Ulid& sourceId) override {
        if (gUiSmokeTesting) ++gUiSmokeDecodeOpens;
        if (!gUiPlayProbeProject.empty()) {
            ++gUiPlayProbeOpens;
            if (!gUiPlayProbeOpened.insert(sourceId).second)
                ++gUiPlayProbeReopens;
        }
    }
    void DecoderInstalled(const std::string& file, double seconds,
                          bool playing) override {
        // PERF12 -- what an open costs on the probed drive, at rest and
        // while playback streams.
        if (!gUiPlayProbeProject.empty())
            std::fprintf(stderr, "probe open %s took %.2f s (%s)\n",
                         file.c_str(), seconds,
                         playing ? "playing" : "at rest");
    }
    void DecodeWorkersReloaded() override {
        if (gUiSmokeTesting) ++gUiSmokeDecodeReloads;
    }
    void DisplayDeadline(bool topmostLate) override {
        if (gUiPlayProbeProject.empty()) return;
        ++gUiPlayProbeTicks;
        if (topmostLate) ++gUiPlayProbeTopDrops;
    }
    void AgentEventRecorded() override { ++gUiSmokeAgentEvents; }

    bool LaysOutEveryPanel() override { return gUiSmokeTesting; }
    // AppKit's panel runs out of process, a test can only end its sheet,
    // and the panel then answers the last folder it was used in -- on 8
    // October 2026, a bench project's package, which a test export wrote
    // into.
    bool AnswersSaveSheets() override { return gUiSmokeTesting; }
    NSURL* TakeSaveSheetAnswer() override {
        if (gUiSmokeSaveAnswer.empty()) return nil;
        NSURL* answer = [NSURL fileURLWithPath:@(gUiSmokeSaveAnswer.c_str())];
        gUiSmokeSaveAnswer.clear();
        return answer;
    }
    NSURL* ReferenceStillURL() override {
        if (!gUiSmokeTesting || gUiSmokeRoot.empty()) return nil;
        return [NSURL
            fileURLWithPath:CMStringFromUtf8(
                                (gUiSmokeRoot / "Reference.png").string())];
    }
    bool HidesClipSignage() override { return gUiSmokeClipSignageOff; }
    std::string AgentJournalTime() override { return gUiSmokeAgentJournalTime; }
    PlaybackTuning Playback() override {
        PlaybackTuning tuning;
        tuning.hide_occluded = gUiPlayProbeHide;
        tuning.read_ahead = gUiPlayProbeReadAhead;
        tuning.byte_seconds = gUiPlayProbeByteSeconds;
        tuning.reappear_seconds = gUiPlayProbeReappearSeconds;
        tuning.cut_seconds = gUiPlayProbeCutSeconds;
        tuning.reader = gUiPlayProbeReader;
        tuning.next_piece = gUiPlayProbeNextPiece;
        return tuning;
    }
};

}  // namespace
}  // namespace cutmachine_app

// PRD8 -- ROADMAP.md. The smoke's icon gesture queues its drag and its
// mouse up behind the press, but AppKit starts the drag session only at a
// later turn of the run loop, and the session runs until a mouse up reaches
// it. When the queued one had been taken by then, the session waited for a
// real hand on the mouse: the run hung until its timeout, on main as on any
// branch (4 October 2026). Half a second into a session still running, the
// smoke presses Escape, which cancels it -- never sooner, so a queued mouse
// up that is still there ends it first, as it always did. A category of
// the media grid, which only this executable adds.
@implementation ContextCollectionView (UiSmoke)

- (void)draggingSession:(NSDraggingSession*)session
       willBeginAtPoint:(NSPoint)screenPoint {
    if ([NSCollectionView instancesRespondToSelector:@selector
                          (draggingSession:willBeginAtPoint:)])
        [super draggingSession:session willBeginAtPoint:screenPoint];
    if (!gUiSmokeTesting) return;
    NSWindow* window = self.window;
    NSTimer* escape = [NSTimer
        timerWithTimeInterval:0.5
                      repeats:NO
                        block:^(NSTimer*) {
                          if (gUiSmokeIconDragEnded) return;
                          NSEvent* key = [NSEvent
                                         keyEventWithType:NSEventTypeKeyDown
                                                 location:NSZeroPoint
                                            modifierFlags:0
                                                timestamp:NSProcessInfo
                                                              .processInfo
                                                              .systemUptime
                                             windowNumber:window.windowNumber
                                                  context:nil
                                               characters:@"\x1b"
                              charactersIgnoringModifiers:@"\x1b"
                                                isARepeat:NO
                                                  keyCode:53];
                          [NSApp postEvent:key atStart:NO];
                        }];
    [NSRunLoop.mainRunLoop addTimer:escape forMode:NSRunLoopCommonModes];
}

- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    if ([NSCollectionView instancesRespondToSelector:@selector
                          (draggingSession:endedAtPoint:operation:)])
        [super draggingSession:session
                  endedAtPoint:screenPoint
                     operation:operation];
    if (gUiSmokeTesting) gUiSmokeIconDragEnded = true;
}

@end

int main(int argc, char* argv[]) {
    // Installed whatever the flags: run otherwise, this executable is the
    // editor as the test builds it -- no engine log, no preference written,
    // no relaunch on a project switch --, as it was under the macro.
    static UiSmokeHooks hooks;
    InstallTestHooks(&hooks);
    // The window test's own flags are not commands of the product: they
    // skip the command line and open the window.
    const bool harnessFlag =
        argc >= 2 && (std::string(argv[1]) == "--ui-smoke" ||
                      std::string(argv[1]) == "--play-probe");
    if (argc == 2 && std::string(argv[1]) == "--ui-smoke") {
        std::string error;
        if (!PrepareUiSmokeProject(error)) {
            std::string output;
            const int result = FailCliCommand(
                "IoError", "unable to prepare UI smoke project: " + error,
                output, 1);
            std::fwrite(output.data(), 1, output.size(), stdout);
            return result;
        }
        gUiSmokeTesting = true;
    }
    // A package folder stands for its project file (ProjectPathOfPackage).
    std::string packageProject;
    if (argc >= 3 && std::string(argv[1]) == "--play-probe") {
        packageProject = ProjectPathOfPackage(argv[2]);
        argv[2] = packageProject.data();
    }
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--play-probe") {
        gUiPlayProbeProject = argv[2];
        gUiPlayProbeSeconds = argc == 4 ? std::atof(argv[3]) : 600.0;
        // PERF13 -- the comparison with every layer composited, as before
        // hidden layers were left out, on the same binary.
        const char* hide = std::getenv("CUTMACHINE_PLAY_PROBE_HIDE");
        gUiPlayProbeHide = hide == nullptr || std::string(hide) != "0";
        const char* readAhead = std::getenv("CUTMACHINE_PLAY_PROBE_READ_AHEAD");
        gUiPlayProbeReadAhead =
            readAhead == nullptr || std::string(readAhead) != "0";
        if (const char* bytes =
                std::getenv("CUTMACHINE_PLAY_PROBE_BYTE_SECONDS"))
            gUiPlayProbeByteSeconds = std::atoll(bytes);
        if (const char* reappear =
                std::getenv("CUTMACHINE_PLAY_PROBE_REAPPEAR_SECONDS"))
            gUiPlayProbeReappearSeconds = std::atoll(reappear);
        if (const char* cut = std::getenv("CUTMACHINE_PLAY_PROBE_CUT_SECONDS"))
            gUiPlayProbeCutSeconds = std::atoll(cut);
        // PERF15 -- CUTMACHINE_PLAY_PROBE_KERNEL_READAHEAD=1 lets the system
        // read the decoders' files ahead as it did before PERF15.
        if (const char* kernel =
                std::getenv("CUTMACHINE_PLAY_PROBE_KERNEL_READAHEAD"))
            MediaSource::SetKernelReadAhead(std::string(kernel) == "1");
        const char* reader = std::getenv("CUTMACHINE_PLAY_PROBE_READER");
        gUiPlayProbeReader = reader == nullptr || std::string(reader) != "0";
        const char* nextPiece = std::getenv("CUTMACHINE_PLAY_PROBE_NEXT_PIECE");
        gUiPlayProbeNextPiece =
            nextPiece == nullptr || std::string(nextPiece) != "0";
        // PERF16 -- CUTMACHINE_PLAY_PROBE_FILL_BEHIND=1 has playback decode
        // the frames behind the one asked for, as before PERF16.
        if (const char* behind =
                std::getenv("CUTMACHINE_PLAY_PROBE_FILL_BEHIND"))
            DecodeWorker::SetPlaybackFillsBehind(std::string(behind) == "1");
        if (const char* perf7 = std::getenv("CUTMACHINE_PLAY_PROBE_PERF7"))
            gUiPlayProbePerf7Seconds = std::atof(perf7);
    }
    if (harnessFlag && !gUiSmokeTesting && gUiPlayProbeProject.empty()) {
        // The window test's own flag, miscounted.
        std::string output;
        const int result = FailCliCommand(
            "ValidationFailed",
            "these arguments do not fit '" + std::string(argv[1]) + "'", output,
            2);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    const char* editorProject = nullptr;
    if (gUiSmokeTesting)
        editorProject = gUiSmokeProjectPath.c_str();
    else if (!gUiPlayProbeProject.empty())
        editorProject = gUiPlayProbeProject.c_str();
    const int status = RunCutmachine(argc, argv, editorProject);
    if (gUiSmokeTesting) {
        std::error_code ignored;
        std::filesystem::remove_all(gUiSmokeRoot, ignored);
        // PRD28 -- 1 on a failure, 77 (CTest: Skipped) when nothing failed
        // but the screen kept checks from being played, 0 otherwise.
        return ui_smoke::ExitStatus(gUiSmokeFailures, gUiSmokeNotRun);
    }
    return status;
}
