// PRD8 -- docs/roadmap/PRODUIT-2026-10.md. The window test harness of
// cutmachine_ui_smoke_tests, moved out of main.mm as it was: its
// fixtures and checks, the input it sends through the window, the QA
// captures and the play probe. Only that target compiles this file.
// STRUCTURE-2026-11 ST1 -- under tests/ since; its entry and the hooks it
// installs in the editor are in UiSmokeMain.mm.

#include "UiSmokeHarness.h"

// CH6 -- the Charte panel's preview, driven as a hand drags on it.
#import "CharterPreviewView.h"

namespace cutmachine_app {

bool gUiSmokeTesting = false;
int gUiSmokeFailures = 0;
bool gUiSmokeIconMouseDown = false;
bool gUiSmokeIconDragSession = false;
// PRD8 -- set when the icon gesture's AppKit drag session has ended.
bool gUiSmokeIconDragEnded = false;
NSUInteger gUiSmokeMediaDragCount = 0;
int gUiSmokeDecodeReloads = 0;
int gUiSmokeDecodeOpens = 0;
size_t gUiSmokeWorkersAtWindowShow = 0;
// PERF10 -- docs/roadmap/PERF-2026-09.md. `--play-probe <project> [seconds]`
// opens a real project, plays its active timeline through without sound and
// reports on stdout what the editor held on the way: the measurement that a
// Mac frozen by playback cannot give. Test builds only.
std::string gUiPlayProbeProject;
double gUiPlayProbeSeconds = 0.0;
// PERF12 -- the decoder opens a play probe pays for, and the sources it has
// opened, to tell a first open from a reopen the capacity forced.
int gUiPlayProbeOpens = 0;
int gUiPlayProbeReopens = 0;
std::set<Ulid> gUiPlayProbeOpened;
// PERF13 -- the display ticks a play probe presents, and those at which the
// topmost layer Record composites was late. dropped_ticks counts a tick at
// which any composited layer is late, one an opaque shot above hides
// included; on a timeline whose upper shots are opaque and full frame, as
// the probed interviews are, the topmost layer is the picture seen, so this
// is the drop a viewer sees, counted alike before and after hidden layers
// stopped being composited.
int gUiPlayProbeTicks = 0;
int gUiPlayProbeTopDrops = 0;
// PERF13 -- CUTMACHINE_PLAY_PROBE_HIDE=0 has a play probe composite and
// decode every layer, hidden ones included: the comparison on one binary.
bool gUiPlayProbeHide = true;
// PERF14 -- CUTMACHINE_PLAY_PROBE_READ_AHEAD=0 has a play probe read as
// before PERF14: no span, no byte advice, the window shared among the
// decoders open. The comparison on one binary.
bool gUiPlayProbeReadAhead = true;
// PERF14 -- and the two lengths it tries: CUTMACHINE_PLAY_PROBE_BYTE_SECONDS
// (the bytes asked ahead of a shot on screen, 0 for none) and
// CUTMACHINE_PLAY_PROBE_REAPPEAR_SECONDS (how early a track hidden under a
// cutaway is asked for again).
int64_t gUiPlayProbeByteSeconds = read_ahead::kByteHorizonSeconds;
int64_t gUiPlayProbeReappearSeconds = read_ahead::kReappearanceLookaheadSeconds;
// And CUTMACHINE_PLAY_PROBE_CUT_SECONDS, how early the next shot is asked
// for ahead of its cut.
int64_t gUiPlayProbeCutSeconds = read_ahead::kCutLookaheadSeconds;
// PERF15 -- CUTMACHINE_PLAY_PROBE_READER=0 has a play probe read as before
// PERF15: each decoder its own file, with PERF14's byte advice -- and with
// CUTMACHINE_PLAY_PROBE_KERNEL_READAHEAD=1, the system's read-ahead too.
// The comparison on one binary.
bool gUiPlayProbeReader = true;
// PERF17 -- CUTMACHINE_PLAY_PROBE_NEXT_PIECE=0 has a play probe stop
// decoding at a shot's out point, as before PERF17, when the next piece is
// of the same rush.
bool gUiPlayProbeNextPiece = true;
// PERF7 -- CUTMACHINE_PLAY_PROBE_PERF7=<seconds> has a play probe play the
// five phases of docs/PERF_BENCHMARK.md, that long each, instead of playing
// the timeline through: see -runPlayProbePerf7Phases.
double gUiPlayProbePerf7Seconds = 0.0;
Ulid gUiSmokeStoredMetadataMediaId;
// VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. A second timeline the main
// run never opens: a 16:9 test pattern in a 1080x1920 sequence, which is
// what R1's checklist cases frame, compared on screen with the export.
Ulid gUiSmokeFramingTimelineId;
Ulid gUiSmokeFramingClipId;
Ulid gUiSmokeSpeedTimelineId;
Ulid gUiSmokeSpeedClipId;
// R3's other speeds, on the 25 i/s pattern: 1/2, then 2, then 2/5.
Ulid gUiSmokeCadenceTimelineId;
Ulid gUiSmokeCadenceHalfClipId;
Ulid gUiSmokeCadenceDoubleClipId;
Ulid gUiSmokeCadenceTwoFifthsClipId;
// ITW4's cartons on a flat grey, one style each: outlined, boxed with a
// shadow, and too long for one line.
Ulid gUiSmokeCaptionTimelineId;
// R10 -- when CUTMACHINE_UI_QA_RESOLVE_MEDIA and _TIMELINE name the two
// manifests of sidecar/resolve_bridge.py, a timeline read back from Resolve
// by the same commands as the CLI. Empty otherwise.
Ulid gUiSmokeResolveTimelineId;
// M4: a white box sliding 20 px a frame, and a timeline with a second
// video track to lay it over the pattern.
Ulid gUiSmokeMotionCompositionId;
Ulid gUiSmokeMotionTimelineId;
Ulid gUiSmokeMotionTrackId;
// REM1 -- docs/roadmap/REMOTION-PARITY-2026-09.md. The three embedded M5
// templates, instantiated at their three declared sizes and shown in Record.
// The 16:9 timeline has four overlay tracks for the logo's four corners.
std::array<Ulid, 3> gUiSmokeTemplateTimelineIds;
std::array<Ulid, 3> gUiSmokeTemplateTrackIds;
std::array<std::array<Ulid, 3>, 3> gUiSmokeTemplateCompositionIds;
std::array<Ulid, 4> gUiSmokeTemplateCornerTrackIds;
Ulid gUiSmokePatternSourceId;
Ulid gUiSmokeGreySourceId;
// REM1 C20 -- a grey rush that has a (silent) sound track, so that the
// montage's words can be read from its transcript, and its timeline.
Ulid gUiSmokeSpeechSourceId;
Ulid gUiSmokeSpeechTimelineId;
// F1.5: the pattern on V1 and the grey on V2 over the same 50 frames, the
// two angles of a group.
Ulid gUiSmokeMulticamTimelineId;
Ulid gUiSmokeMulticamPatternClipId;
Ulid gUiSmokeMulticamGreyClipId;
// BENCH-2026-09: two cartons end to end on a caption track, for a roll.
Ulid gUiSmokeRollTimelineId;
Ulid gUiSmokeRollFirstCartonId;
Ulid gUiSmokeRollSecondCartonId;
// A 29.97 sequence whose shot sits three minutes in.
Ulid gUiSmokeNtscTimelineId;
Ulid gUiSmokeNtscClipId;
Ulid gUiSmokeRotatedClipId;
Ulid gUiSmokeAnimationTimelineId;
Ulid gUiSmokeAnimationClipId;
Ulid gUiSmokeDipTimelineId;
Ulid gUiSmokeDipFirstClipId;
Ulid gUiSmokeDipSecondClipId;
Ulid gUiSmokeLookTimelineId;
Ulid gUiSmokeLookClipId;
// PRD24 -- ROADMAP.md. A timeline of ordinary shots that the run slows,
// reframes, animates and grades one by one, to read in the display list what
// each rectangle then says. Three of them are the same shot at three widths,
// and the last two are the angles of a multicam group.
Ulid gUiSmokeSignageTimelineId;
Ulid gUiSmokeSignagePlainClipId;
Ulid gUiSmokeSignageFastClipId;
Ulid gUiSmokeSignageSoundClipId;
Ulid gUiSmokeSignageSlowClipId;
Ulid gUiSmokeSignageFramedClipId;
Ulid gUiSmokeSignageGradedClipId;
Ulid gUiSmokeSignageAnimatedClipId;
Ulid gUiSmokeSignageLoadedClipId;
Ulid gUiSmokeSignageTightClipId;
Ulid gUiSmokeSignageSliverClipId;
Ulid gUiSmokeSignageActiveAngleClipId;
Ulid gUiSmokeSignageHiddenAngleClipId;
// Set while the run builds the display list a second time without the
// badges and the veil, to compare the two.
bool gUiSmokeClipSignageOff = false;
// FC-fenêtre -- docs/roadmap/FILMCRAFT-2026-10.md. Three shots of the
// pattern end to end, with handles on every side, for the magnet, the trim
// mode, the Trim Monitor, the slide and the rate stretch.
Ulid gUiSmokeTrimTimelineId;
std::array<Ulid, 3> gUiSmokeTrimClipIds;
// SYNCHRO-2026-10 SY2 -- a camera's rush and its recorder, whose sound is
// the camera's half a second earlier: the picture with its linked sound,
// the recording dropped whole on a third track, and a grey picture over
// silence that no recording matches.
Ulid gUiSmokeSoundTimelineId;
Ulid gUiSmokeSoundPictureId;
Ulid gUiSmokeSoundCameraId;
Ulid gUiSmokeSoundRecorderId;
Ulid gUiSmokeSoundSilentPictureId;
// PRD25 -- three shots end to end that an agent relayed to the window edits
// through its endpoint; the time the journal's lines are written at while
// the run looks at them (empty: the clock's); and the endpoint's events the
// window has taken, which the run waits on.
Ulid gUiSmokeJournalTimelineId;
std::array<Ulid, 3> gUiSmokeJournalClipIds;
// QA pass of 8 October 2026 (docs/qa/2026-10-08-ui-agentique.md) -- the
// timeline the screen pass plays its markers, keys and forms on: three
// shots of two seconds end to end, cuts at 2 and 4 seconds.
Ulid gUiSmokeScreenTimelineId;
std::array<Ulid, 3> gUiSmokeScreenCartonIds;
std::string gUiSmokeAgentJournalTime;
int gUiSmokeAgentEvents = 0;
std::filesystem::path gUiSmokeRoot;
std::string gUiSmokeProjectPath;
// QA pass of 8 October 2026 -- the destination the next save panel answers
// (main.mm, CMSaveSheetAnswer); none, and it answers nothing.
std::filesystem::path gUiSmokeSaveAnswer;
std::filesystem::path gUiQaOutputDirectory;
std::vector<std::pair<std::string, bool>> gUiQaAssertions;

struct UiQaCapture {
    std::string name;
    std::string file;
    int64_t width = 0;
    int64_t height = 0;
};

std::vector<UiQaCapture> gUiQaCaptures;

bool UiQaEnabled() { return !gUiQaOutputDirectory.empty(); }

bool WaitForUiQaCapture(dispatch_semaphore_t semaphore,
                        NSTimeInterval timeout) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    while (dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW) != 0) {
        if (deadline.timeIntervalSinceNow <= 0.0) return false;
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return true;
}

// PRD28 -- docs/roadmap/PRODUIT-2026-10b.md. What the run knows of the
// editor window it drives (SmokeReport.h): whether it became key after the
// run's activation attempt (-runUiSmokeTests), whether WindowServer
// composites it. A window once found hidden -- at the start, or by a capture
// -- counts as hidden for the rest of the run: the run can no longer vouch
// for what it showed, and does not wait for it again.
ui_smoke::Screen gUiSmokeScreen;
// LG5 -- docs/roadmap/LEGENDES-2026-10.md. Set while a scenario measures
// what the monitor itself draws (the cartons, ITW4): a covered window is
// then drawn off screen rather than left unmeasured. The glass of macOS 26
// that the drawing lacks (VQA5) is nowhere on the monitor.
bool gUiQaDrawOffscreen = false;
ui_smoke::NotRun gUiSmokeNotRun;
// The checks not played, for the QA manifest: label, reason.
std::vector<std::pair<std::string, std::string>> gUiQaNotRun;
__weak NSWindow* gUiSmokeEditorWindow = nil;
constexpr const char* kUiSmokeHiddenReason =
    "the editor window is not visible (covered, minimised or on another "
    "Space)";

// What the run found at its start can change under it: someone back at the
// Mac clicks another application and takes the focus. Only another
// application makes this one inactive -- a window of the run's own that
// takes the keys leaves it active, and the check that suffers from it still
// fails.
void UiSmokeNoteFocus() {
    if (gUiSmokeScreen.key && !NSApp.isActive) {
        gUiSmokeScreen.key = false;
        gUiSmokeScreen.not_key =
            "another application took the focus during the run";
    }
}

// PRD28 -- `needs` says what the check needs of the screen. One the screen
// cannot give is not played: it is reported SKIP with the reason, whatever
// `condition` holds, since without what it needs its result says nothing --
// "Undoing a rename does not restart media decoders" passes on a rename
// that never happened.
void UiSmokeCheck(bool condition, const char* label,
                  ui_smoke::Needs needs = ui_smoke::Needs::kNothing) {
    if (needs == ui_smoke::Needs::kKeyWindow) UiSmokeNoteFocus();
    const std::string missing = ui_smoke::MissingNeed(needs, gUiSmokeScreen);
    if (!missing.empty()) {
        gUiSmokeNotRun.Add(missing);
        if (UiQaEnabled()) gUiQaNotRun.emplace_back(label, missing);
        std::fprintf(stdout, "SKIP: %s -- %s\n", label, missing.c_str());
        std::fflush(stdout);
        return;
    }
    if (UiQaEnabled()) gUiQaAssertions.emplace_back(label, condition);
    if (condition) {
        std::fprintf(stdout, "PASS: %s\n", label);
    } else {
        ++gUiSmokeFailures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
    std::fflush(condition ? stdout : stderr);
}

// PRD28 -- the window a capture is asked of, by name: the editor window, or
// another one by its title, so that a message about a dialog does not blame
// the editor window.
std::string UiQaWindowLabel(NSWindow* window) {
    if (window != nil && window == gUiSmokeEditorWindow)
        return "the editor window";
    NSString* title = window.title;
    if (title.length > 0)
        return "the window \"" + std::string(title.UTF8String ?: "") + "\"";
    return "the window #" + std::to_string(window.windowNumber);
}

// VQA1 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The smoke test already owns
// and drives the real editor window. Capturing that one window here observes
// AppKit and Metal after WindowServer composition without asking for global
// Accessibility input. CUTMACHINE_UI_QA_DIR is deliberately opt-in: normal
// CTest runs keep producing no images and touching no persistent QA state.
//
// PRD28 -- not drawn off screen instead (WriteUiQaViewCapture): that draws
// AppKit's views, and leaves a CAMetalLayer's content out, transparent --
// measured on 3 October 2026 -- so Record, Source and the timeline would be
// missing from every capture of this window. A capture refused because the
// editor window is hidden is not run, not failed: the check that asks for it
// needs the visible window (ui_smoke::Needs::kVisibleWindow). VQA5 draws the
// editor window off screen with its Metal surfaces (WindowCapture.h), and
// checks it against this one (runUiSmokeOffscreenCaptureScenario).
//
// The window as WindowServer composites it, +1 for the caller; null, with
// `error`, when it cannot be had.
CGImageRef CaptureWindowComposited(NSWindow* window, std::string& error) {
    error.clear();
    const std::string which = UiQaWindowLabel(window);
    if (window == nil || window.windowNumber <= 0) {
        error = which + " has no WindowServer identifier";
        return nullptr;
    }
    const bool editor = window == gUiSmokeEditorWindow;
    if (editor && !gUiSmokeScreen.visible) {
        error = gUiSmokeScreen.not_visible;
        return nullptr;
    }
    // VQA2 -- a window that is covered, minimised or on another Space is
    // not composited again, and ScreenCaptureKit hands back its last
    // composited image: Record and the timecode both frozen on an earlier
    // state, measured as if they were this one (23 September 2026, while
    // the Mac was in use). Refuse instead of measuring a stale picture.
    // Just after the window is shown, AppKit may not have reported it
    // visible yet: give it two seconds before concluding it is hidden.
    NSDate* visibleBy = [NSDate dateWithTimeIntervalSinceNow:2.0];
    while ((window.occlusionState & NSWindowOcclusionStateVisible) == 0 &&
           visibleBy.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    if ((window.occlusionState & NSWindowOcclusionStateVisible) == 0) {
        error = which +
                " is not visible (covered, minimised or on another Space): "
                "its capture would be a stale image";
        if (editor) {
            gUiSmokeScreen.visible = false;
            gUiSmokeScreen.not_visible = kUiSmokeHiddenReason;
        }
        return nullptr;
    }
    // VQA2 -- on macOS 26, ignoreShadowsSingleWindow does not keep the
    // shadow out: the window and its shadow were squeezed into the window's
    // own size, about 1.79 pixels per point instead of 2, which put every
    // crop computed from view geometry off target. Without a shadow the
    // image is the window at exactly its backing scale.
    const BOOL hadShadow = window.hasShadow;
    window.hasShadow = NO;
    [window invalidateShadow];
    struct ShadowRestore {
        NSWindow* window;
        BOOL shadow;
        ~ShadowRestore() {
            window.hasShadow = shadow;
            [window invalidateShadow];
        }
    } restoreShadow{window, hadShadow};
    [window displayIfNeeded];
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:0.15];
    while (deadline.timeIntervalSinceNow > 0.0) {
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                               beforeDate:deadline];
    }

    if (@available(macOS 14.4, *)) {
    } else {
        error = "current-process window capture requires macOS 14.4";
        return nullptr;
    }

    __block SCWindow* shareableWindow = nil;
    __block NSError* shareableError = nil;
    dispatch_semaphore_t shareableSemaphore = dispatch_semaphore_create(0);
    [SCShareableContent
        getCurrentProcessShareableContentWithCompletionHandler:^(
            SCShareableContent* content, NSError* contentError) {
          shareableError = contentError;
          for (SCWindow* candidate in content.windows) {
              if (candidate.windowID ==
                  static_cast<CGWindowID>(window.windowNumber)) {
                  shareableWindow = candidate;
                  break;
              }
          }
          dispatch_semaphore_signal(shareableSemaphore);
        }];
    if (!WaitForUiQaCapture(shareableSemaphore, 5.0)) {
        error = "timed out while listing this process's windows";
        return nullptr;
    }
    if (shareableWindow == nil) {
        error = shareableError
                    ? shareableError.localizedDescription.UTF8String
                    : which + " is absent from current-process shareable "
                              "content";
        return nullptr;
    }

    SCContentFilter* filter = [[SCContentFilter alloc]
        initWithDesktopIndependentWindow:shareableWindow];
    SCStreamConfiguration* configuration = [SCStreamConfiguration new];
    const double scale = window.backingScaleFactor;
    configuration.width = static_cast<size_t>(
        std::max(1.0, std::round(shareableWindow.frame.size.width * scale)));
    configuration.height = static_cast<size_t>(
        std::max(1.0, std::round(shareableWindow.frame.size.height * scale)));
    configuration.showsCursor = NO;
    configuration.ignoreShadowsSingleWindow = YES;
    configuration.captureResolution = SCCaptureResolutionBest;

    __block CGImageRef image = nullptr;
    __block NSError* captureError = nil;
    dispatch_semaphore_t captureSemaphore = dispatch_semaphore_create(0);
    [SCScreenshotManager
        captureImageWithFilter:filter
                 configuration:configuration
             completionHandler:^(CGImageRef captured, NSError* imageError) {
               if (captured != nullptr) image = CGImageRetain(captured);
               captureError = imageError;
               dispatch_semaphore_signal(captureSemaphore);
             }];
    if (!WaitForUiQaCapture(captureSemaphore, 5.0)) {
        error = "timed out while capturing " + which;
        return nullptr;
    }
    if (image == nullptr || CGImageGetWidth(image) == 0 ||
        CGImageGetHeight(image) == 0) {
        if (image != nullptr) CFRelease(image);
        error = captureError
                    ? captureError.localizedDescription.UTF8String
                    : "ScreenCaptureKit did not return the image of " + which;
        return nullptr;
    }
    return image;
}

bool CaptureUiQaWindow(NSWindow* window, const std::string& name,
                       std::string& error) {
    error.clear();
    if (!UiQaEnabled()) return true;
    const std::string which = UiQaWindowLabel(window);
    CGImageRef image = CaptureWindowComposited(window, error);
    if (image == nullptr) return false;
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                       properties:@{}];
    const std::filesystem::path path = gUiQaOutputDirectory / (name + ".png");
    const bool wrote =
        png != nil &&
        [png writeToFile:[NSString stringWithUTF8String:path.c_str()]
              atomically:YES];
    if (wrote) {
        gUiQaCaptures.push_back(
            {name, path.filename().string(),
             static_cast<int64_t>(CGImageGetWidth(image)),
             static_cast<int64_t>(CGImageGetHeight(image))});
    }
    CFRelease(image);
    if (!wrote) {
        error = "unable to write the PNG of " + which;
        return false;
    }
    return true;
}

// VQA5 -- how far two pictures of the same thing agree, once both are
// converted to sRGB at one size (WindowCaptureSrgb): the share of pixels
// whose red, green and blue all lie within `tolerance` codes, and the
// largest difference. Pixels inside `excluded` (in the compared size, from
// the top-left corner) are not counted.
struct UiCaptureAgreement {
    bool compared = false;
    double within = 0.0;
    int largest = 0;
};

UiCaptureAgreement CompareUiCaptures(CGImageRef first, CGRect firstRect,
                                     CGImageRef second, CGRect secondRect,
                                     size_t width, size_t height, int tolerance,
                                     const std::vector<CGRect>& excluded = {}) {
    UiCaptureAgreement agreement;
    CGImageRef a = WindowCaptureSrgb(first, firstRect, width, height);
    CGImageRef b = WindowCaptureSrgb(second, secondRect, width, height);
    CFDataRef aBytes =
        a ? CGDataProviderCopyData(CGImageGetDataProvider(a)) : nullptr;
    CFDataRef bBytes =
        b ? CGDataProviderCopyData(CGImageGetDataProvider(b)) : nullptr;
    if (aBytes != nullptr && bBytes != nullptr) {
        const UInt8* aPixels = CFDataGetBytePtr(aBytes);
        const UInt8* bPixels = CFDataGetBytePtr(bBytes);
        const size_t aRow = CGImageGetBytesPerRow(a);
        const size_t bRow = CGImageGetBytesPerRow(b);
        size_t agreeing = 0;
        size_t counted = 0;
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x) {
                const CGPoint centre = CGPointMake(x + 0.5, y + 0.5);
                if (std::any_of(excluded.begin(), excluded.end(),
                                [&](const CGRect& rect) {
                                    return CGRectContainsPoint(rect, centre);
                                }))
                    continue;
                ++counted;
                int difference = 0;
                for (size_t channel = 0; channel < 3; ++channel)
                    difference = std::max(
                        difference,
                        std::abs(int(aPixels[y * aRow + x * 4 + channel]) -
                                 int(bPixels[y * bRow + x * 4 + channel])));
                agreement.largest = std::max(agreement.largest, difference);
                if (difference <= tolerance) ++agreeing;
            }
        }
        agreement.compared = counted > 0;
        agreement.within = counted > 0 ? static_cast<double>(agreeing) /
                                             static_cast<double>(counted)
                                       : 0.0;
    }
    if (aBytes != nullptr) CFRelease(aBytes);
    if (bBytes != nullptr) CFRelease(bBytes);
    if (a != nullptr) CGImageRelease(a);
    if (b != nullptr) CGImageRelease(b);
    return agreement;
}

std::string DescribeUiCaptureAgreement(const UiCaptureAgreement& agreement) {
    if (!agreement.compared) return "not compared";
    char text[96];
    std::snprintf(text, sizeof(text), "%.2f %% within tolerance, largest %d",
                  agreement.within * 100.0, agreement.largest);
    return text;
}

// PRD21 (window) -- a view drawn off screen over `background` and kept as a
// QA capture, for a window the run does not put in front of whoever uses
// the Mac (the welcome screen, a dialog), and which a covered screen would
// refuse anyway. AppKit's drawing only: neither the title bar nor a layer's
// own background colour, which `background` stands for.
bool WriteUiQaViewCapture(NSView* view, NSColor* background,
                          const std::string& name, std::string& error) {
    error.clear();
    if (!UiQaEnabled()) return true;
    NSBitmapImageRep* drawn =
        view ? [view bitmapImageRepForCachingDisplayInRect:view.bounds] : nil;
    if (!drawn) {
        error = "no view to draw";
        return false;
    }
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:drawn];
    NSBitmapImageRep* bitmap =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                pixelsWide:drawn.pixelsWide
                                                pixelsHigh:drawn.pixelsHigh
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];
    bitmap.size = drawn.size;
    const NSRect bounds = NSMakeRect(0, 0, drawn.size.width, drawn.size.height);
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext =
        [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
    [view.effectiveAppearance performAsCurrentDrawingAppearance:^{
      [background setFill];
      NSRectFill(bounds);
    }];
    [drawn drawInRect:bounds
              fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver
              fraction:1.0
        respectFlipped:YES
                 hints:nil];
    [NSGraphicsContext restoreGraphicsState];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                       properties:@{}];
    const std::filesystem::path path = gUiQaOutputDirectory / (name + ".png");
    if (png == nil || ![png writeToFile:@(path.c_str()) atomically:YES]) {
        error = "unable to write " + path.string();
        return false;
    }
    gUiQaCaptures.push_back({name, path.filename().string(),
                             static_cast<int64_t>(bitmap.pixelsWide),
                             static_cast<int64_t>(bitmap.pixelsHigh)});
    return true;
}

bool WriteUiQaManifest(NSWindow* window, const Document& document,
                       std::string& error) {
    error.clear();
    if (!UiQaEnabled()) return true;
    mcp_json::Value manifest = mcp_json::Value::MakeObject();
    manifest.Set("schema_version", mcp_json::Value::MakeInt(1));
    NSDateFormatter* timestampFormatter = [NSDateFormatter new];
    timestampFormatter.locale =
        [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    timestampFormatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    timestampFormatter.dateFormat = @"yyyy-MM-dd'T'HH:mm:ss'Z'";
    manifest.Set(
        "captured_at_utc",
        mcp_json::Value::MakeString(
            [timestampFormatter stringFromDate:NSDate.date].UTF8String));
    manifest.Set(
        "macos",
        mcp_json::Value::MakeString(
            NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String));
    const char* commit = std::getenv("CUTMACHINE_UI_QA_COMMIT");
    manifest.Set("git_commit",
                 mcp_json::Value::MakeString(commit ? commit : ""));
    const char* diffHash = std::getenv("CUTMACHINE_UI_QA_DIFF_HASH");
    manifest.Set("source_diff_hash",
                 mcp_json::Value::MakeString(diffHash ? diffHash : ""));
    manifest.Set("document_hash", mcp_json::Value::MakeString(
                                      CanonicalHash(document.SaveToString())));
    manifest.Set("failure_count", mcp_json::Value::MakeInt(gUiSmokeFailures));

    mcp_json::Value windowValue = mcp_json::Value::MakeObject();
    windowValue.Set("width_points", mcp_json::Value::MakeDouble(
                                        window.contentView.bounds.size.width));
    windowValue.Set(
        "height_points",
        mcp_json::Value::MakeDouble(window.contentView.bounds.size.height));
    windowValue.Set("backing_scale",
                    mcp_json::Value::MakeDouble(window.backingScaleFactor));
    manifest.Set("window", std::move(windowValue));

    mcp_json::Value assertions = mcp_json::Value::MakeArray();
    for (const auto& assertion : gUiQaAssertions) {
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("label", mcp_json::Value::MakeString(assertion.first));
        value.Set("passed", mcp_json::Value::MakeBool(assertion.second));
        assertions.Push(std::move(value));
    }
    manifest.Set("assertions", std::move(assertions));
    // PRD28 -- the checks the screen did not let the run play, apart from
    // the assertions: an "assertions" entry still means a played check.
    mcp_json::Value notRun = mcp_json::Value::MakeArray();
    for (const auto& [label, reason] : gUiQaNotRun) {
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("label", mcp_json::Value::MakeString(label));
        value.Set("reason", mcp_json::Value::MakeString(reason));
        notRun.Push(std::move(value));
    }
    manifest.Set(
        "not_run_count",
        mcp_json::Value::MakeInt(static_cast<int64_t>(gUiQaNotRun.size())));
    manifest.Set("not_run", std::move(notRun));

    mcp_json::Value captures = mcp_json::Value::MakeArray();
    for (const UiQaCapture& capture : gUiQaCaptures) {
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("name", mcp_json::Value::MakeString(capture.name));
        value.Set("file", mcp_json::Value::MakeString(capture.file));
        value.Set("width", mcp_json::Value::MakeInt(capture.width));
        value.Set("height", mcp_json::Value::MakeInt(capture.height));
        captures.Push(std::move(value));
    }
    manifest.Set("captures", std::move(captures));

    const std::filesystem::path path = gUiQaOutputDirectory / "manifest.json";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << manifest.Dump() << '\n';
    output.close();
    if (!output) {
        error = "unable to write the UI QA manifest";
        return false;
    }
    return true;
}

// VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. A picture reduced to a grid
// of mean sRGB colours, top row first: coarse enough that a scaler, a codec
// and a display profile agree on it, fine enough that a picture moved by a
// quarter of its frame does not. The grid has the 9:16 shape of the framing
// timeline measured, so each cell is square in the sequence.
struct UiQaGridShape {
    int columns = 9;
    int rows = 16;
};

// Cells per side of that grid. A cell of the default grid spans half a
// period of the pattern's waves, which averages two neighbouring frames of
// it to within 0.05 of each other: R3, which must tell them apart, measures
// three times finer.
int gUiQaGridDensity = 1;

UiQaGridShape UiQaGridFor(const DocumentSequence& sequence) {
    const UiQaGridShape shape = sequence.width > sequence.height
                                    ? UiQaGridShape{16, 9}
                                    : UiQaGridShape{9, 16};
    return {shape.columns * gUiQaGridDensity, shape.rows * gUiQaGridDensity};
}

// How a PNG's values are turned into what a viewer sees, in sRGB.
enum class UiQaReading {
    // The colour space the file declares: a window capture carries the
    // display's profile.
    Declared,
    // The file's code values as the Record layer shows them: BT.709
    // primaries decoded by a pure 2.4 gamma (Renderer.mm), then shown in
    // sRGB. Until 23 September 2026 the layer was ITU-R 709, decoded by the
    // inverse camera curve, which the capture confirmed at +14, +18, +13 and
    // +3 codes over the raw values at 25, 64, 128 and 224.
    RecordLayer,
    // The code values themselves, whatever the file declares: what
    // read_frame hands a model is compared with the export this way.
    Raw,
};

uint8_t UiQaRecordLayerCode(int code) {
    const double linear = std::pow(code / 255.0, 2.4);
    const double shown = linear <= 0.0031308
                             ? linear * 12.92
                             : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
    return static_cast<uint8_t>(
        std::clamp(std::lround(shown * 255.0), 0L, 255L));
}

// `crop` is in the PNG's pixels, origin top-left; a null rect keeps it all.
bool UiQaGrid(const std::filesystem::path& png, CGRect crop,
              UiQaGridShape shape, std::vector<float>& grid,
              UiQaReading reading = UiQaReading::Declared) {
    NSData* data = [NSData
        dataWithContentsOfFile:[NSString stringWithUTF8String:png.c_str()]];
    NSBitmapImageRep* bitmap =
        data != nil ? [NSBitmapImageRep imageRepWithData:data] : nil;
    CGImageRef image = bitmap.CGImage;
    if (image == nullptr) return false;
    // Read as sRGB into an sRGB context, the values arrive untouched.
    CGImageRef retagged = nullptr;
    if (reading != UiQaReading::Declared) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        retagged = CGImageCreateCopyWithColorSpace(image, space);
        CGColorSpaceRelease(space);
        if (retagged == nullptr) return false;
        image = retagged;
    }
    if (CGRectIsNull(crop))
        crop = CGRectMake(0.0, 0.0, CGImageGetWidth(image),
                          CGImageGetHeight(image));
    // Two per cent off every edge, on the capture and on the file alike: the
    // crop of a capture is rounded to whole pixels, and a pixel of the black
    // around the frame in an edge cell reads as a picture that is not there.
    crop = CGRectIntegral(
        CGRectInset(crop, crop.size.width * 0.02, crop.size.height * 0.02));
    CGImageRef cropped = CGImageCreateWithImageInRect(image, crop);
    if (retagged != nullptr) CGImageRelease(retagged);
    if (cropped == nullptr) return false;
    constexpr int kSamples = 8;
    const int width = shape.columns * kSamples;
    const int height = shape.rows * kSamples;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        pixels.data(), width, height, 8, static_cast<size_t>(width) * 4, srgb,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(srgb);
    if (context == nullptr) {
        CGImageRelease(cropped);
        return false;
    }
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0.0, 0.0, width, height), cropped);
    CGContextRelease(context);
    CGImageRelease(cropped);
    if (reading == UiQaReading::RecordLayer) {
        uint8_t shown[256];
        for (int code = 0; code < 256; ++code)
            shown[code] = UiQaRecordLayerCode(code);
        for (size_t index = 0; index < pixels.size(); ++index)
            if (index % 4 != 3) pixels[index] = shown[pixels[index]];
    }
    grid.assign(static_cast<size_t>(shape.columns) * shape.rows * 3, 0.0f);
    constexpr float kWeight = 1.0f / (255.0f * kSamples * kSamples);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t cell =
                static_cast<size_t>(y / kSamples) * shape.columns +
                static_cast<size_t>(x / kSamples);
            const uint8_t* pixel =
                pixels.data() + ((static_cast<size_t>(y) * width) + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel)
                grid[(cell * 3) + channel] += pixel[channel] * kWeight;
        }
    }
    return true;
}

// Mean absolute difference per channel, over the cells not `ignored`.
double UiQaDistance(const std::vector<float>& left,
                    const std::vector<float>& right,
                    const std::vector<bool>& ignored) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t cell = 0; cell < ignored.size(); ++cell) {
        if (ignored[cell]) continue;
        for (size_t channel = 0; channel < 3; ++channel)
            sum += std::fabs(left[(cell * 3) + channel] -
                             right[(cell * 3) + channel]);
        count += 3;
    }
    return count == 0 ? 1.0 : sum / static_cast<double>(count);
}

// Mean of every channel of the cells not `ignored`: how light a picture is.
double UiQaMean(const std::vector<float>& grid,
                const std::vector<bool>& ignored) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t cell = 0; cell < ignored.size(); ++cell) {
        if (ignored[cell]) continue;
        for (size_t channel = 0; channel < 3; ++channel)
            sum += grid[(cell * 3) + channel];
        count += 3;
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

// The darkest channel of any cell not `ignored`.
double UiQaMinimum(const std::vector<float>& grid,
                   const std::vector<bool>& ignored) {
    double minimum = 1.0;
    for (size_t cell = 0; cell < ignored.size(); ++cell) {
        if (ignored[cell]) continue;
        for (size_t channel = 0; channel < 3; ++channel)
            minimum = std::min<double>(minimum, grid[(cell * 3) + channel]);
    }
    return minimum;
}

std::string UiQaQuote(const std::string& text) {
    std::string result = "'";
    for (char character : text)
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

// The delivered file: `document` through the same Exporter the export
// sheet runs. One file per state, from which every compared frame is read.
bool UiQaExportMovie(const Document& document, const std::string& documentPath,
                     const std::filesystem::path& movie, std::string& error) {
    ExportSettings settings = Exporter::H264Preset(movie.string());
    settings.width = document.sequence.width;
    settings.height = document.sequence.height;
    settings.frame_rate = document.sequence.frame_rate;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.overwrite = true;
    ExportPlan plan;
    if (!Exporter::BuildPlan(document, documentPath, settings, plan, error))
        return false;
    // Kept beside the frames it produced, for whoever reproduces a gap
    // outside the app: the filter graph, then the FFmpeg arguments.
    std::filesystem::path trace = movie;
    trace.replace_extension(".plan.txt");
    std::ofstream out(trace);
    out << plan.filter_graph << "\n";
    for (const std::string& argument : plan.arguments)
        out << UiQaQuote(argument) << ' ';
    out << "\n";
    return Exporter::Run(plan, {}, nullptr, error);
}

bool UiQaExtractFrame(const std::filesystem::path& movie, int64_t frame,
                      const std::filesystem::path& png) {
    const std::string extract =
        UiQaQuote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -y -i " +
        UiQaQuote(movie.string()) + " -vf 'select=eq(n\\," +
        std::to_string(frame) + ")' -frames:v 1 " + UiQaQuote(png.string());
    return std::system(extract.c_str()) == 0;
}

bool PrepareUiSmokeProject(std::string& error) {
    if (const char* output = std::getenv("CUTMACHINE_UI_QA_DIR");
        output != nullptr && output[0] != '\0') {
        gUiQaOutputDirectory = output;
        std::error_code directoryError;
        std::filesystem::create_directories(gUiQaOutputDirectory,
                                            directoryError);
        if (directoryError) {
            error = "unable to create UI QA output directory: " +
                    directoryError.message();
            return false;
        }
    }
    gUiSmokeRoot = std::filesystem::temp_directory_path() /
                   (GenerateUlid() + "-cutmachine-ui-smoke");
    std::filesystem::create_directories(gUiSmokeRoot);
    const std::filesystem::path mediaPath = gUiSmokeRoot / "fixture.mov";
    const std::filesystem::path audioPath = gUiSmokeRoot / "tone.wav";
    // Long enough to satisfy the 100-frame source declared below, which
    // DecodeWorkerMatchesSource checks against the real frame count.
    const auto quote = [](const std::filesystem::path& path) {
        std::string result = "'";
        for (char character : path.string())
            result += character == '\'' ? "'\\''" : std::string(1, character);
        return result + "'";
    };
    const std::string generate =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y "
        "-f lavfi -i 'color=c=black:s=64x64:r=25:d=4.2' -c:v mpeg4 " +
        quote(mediaPath);
    if (std::system(generate.c_str()) != 0) {
        error = "unable to create UI smoke media fixture";
        return false;
    }

    Document document;
    const Ulid parentBinId = GenerateUlid();
    const Ulid mediaBinId = GenerateUlid();
    LibraryMedia media;
    media.id = GenerateUlid();
    media.path = mediaPath.string();
    media.filename = "00-fixture.mov";
    media.rate = {25, 1};
    media.duration = {100, 25};
    media.metadata_complete = false;
    media.bin_id = mediaBinId;
    document.library = {media};
    document.bins = {{parentBinId, "Rush", ""},
                     {mediaBinId, "1_RUSHES", parentBinId}};
    document.sources = {
        {media.id, media.path, media.rate, media.duration},
    };
    // Multiple visual rows are deliberate: a one-row fixture cannot catch a
    // flipped NSCollectionView coordinate lookup selecting a lower tile.
    for (int index = 1; index < 7; ++index) {
        LibraryMedia extra = media;
        extra.id = GenerateUlid();
        extra.filename = "0" + std::to_string(index) + "-fixture.mov";
        // PERF-2026-09. One entry carries probed metadata, as every entry
        // ingested by a current version does, so the smoke run covers the
        // path that answers from the library as well as the legacy path that
        // still goes to the drive. Its rotation is deliberately a value the
        // real fixture file cannot report: if the startup path ever goes
        // back to probing this entry, the check below sees 0 instead of 90.
        if (index == 1) {
            extra.metadata_complete = true;
            extra.codec = "mpeg4";
            extra.has_video = true;
            extra.width = 64;
            extra.height = 64;
            extra.pixel_format = "yuv420p";
            extra.rotation_degrees = 90;
            extra.orientation = "portrait";
            extra.has_audio = false;
            gUiSmokeStoredMetadataMediaId = extra.id;
        }
        document.library.push_back(extra);
        document.sources.push_back(
            {extra.id, extra.path, extra.rate, extra.duration});
    }
    const std::string generateAudio =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=4.2' " +
        quote(audioPath);
    if (std::system(generateAudio.c_str()) != 0) {
        error = "unable to create UI smoke audio fixture";
        return false;
    }
    LibraryMedia audioMedia;
    audioMedia.path = audioPath.string();
    audioMedia.filename = "07-tone.wav";
    audioMedia.codec = "pcm_s16le";
    audioMedia.has_video = false;
    audioMedia.rate = {48000, 1};
    audioMedia.duration = {201600, 48000};
    audioMedia.orientation = "audio";
    audioMedia.has_audio = true;
    audioMedia.audio_rate = 48000;
    audioMedia.audio_channels = 1;
    audioMedia.bin_id = mediaBinId;
    audioMedia.metadata_complete = true;
    document.library.push_back(std::move(audioMedia));
    // VQA2 -- sine waves of about 40 pixels on each channel, drifting with
    // every frame: a zoom, a shift or another frame changes every cell of the
    // comparison grid. testsrc2 was tried first; its flat colour bars left a
    // 29/20 zoom within 0.05 of no zoom once framed to 9:16.
    const auto makePattern = [&](const std::string& rate,
                                 const std::filesystem::path& path) {
        const std::string command =
            quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -y -f lavfi -i "
            "\"nullsrc=s=320x180:r=" +
            rate +
            ":d=4.2,format=gbrp,geq=r='128+127*sin(X/6+N/2)*cos(Y/9)'"
            ":g='128+127*sin(Y/7-N/3)':b='128+127*cos((X+Y)/11+N/5)'\""
            " -c:v mpeg4 -q:v 2 " +
            quote(path);
        return std::system(command.c_str()) == 0;
    };
    const std::filesystem::path patternPath = gUiSmokeRoot / "mire.mov";
    if (!makePattern("25", patternPath)) {
        error = "unable to create UI smoke pattern fixture";
        return false;
    }
    // Between the two others: the checks below rename the first bin and
    // browse the last one as 1_RUSHES.
    const Ulid qaBinId = GenerateUlid();
    document.bins.insert(document.bins.begin() + 1, {qaBinId, "QA", ""});
    LibraryMedia pattern = media;
    pattern.id = GenerateUlid();
    pattern.path = patternPath.string();
    pattern.filename = "mire-16x9.mov";
    pattern.bin_id = qaBinId;
    pattern.duration = {105, 25};
    // A framing is resolved against the displayed size, so the pattern is
    // described as an ingest would describe it.
    pattern.metadata_complete = true;
    pattern.codec = "mpeg4";
    pattern.has_video = true;
    pattern.width = 320;
    pattern.height = 180;
    pattern.pixel_format = "yuv420p";
    pattern.orientation = "landscape";
    pattern.has_audio = false;
    gUiSmokePatternSourceId = pattern.id;
    document.library.push_back(pattern);
    document.sources.push_back(
        {pattern.id, pattern.path, pattern.rate, pattern.duration});
    // VQA2 -- R3's case: the same pattern shot at twice the sequence
    // cadence, which is what a slowed shot of the measured film was.
    const std::filesystem::path fastPatternPath = gUiSmokeRoot / "mire50.mov";
    if (!makePattern("50", fastPatternPath)) {
        error = "unable to create UI smoke 50 fps pattern fixture";
        return false;
    }
    LibraryMedia fastPattern = pattern;
    fastPattern.id = GenerateUlid();
    fastPattern.path = fastPatternPath.string();
    fastPattern.filename = "mire-50ips.mov";
    fastPattern.rate = {50, 1};
    fastPattern.duration = {210, 50};
    document.library.push_back(fastPattern);
    document.sources.push_back({fastPattern.id, fastPattern.path,
                                fastPattern.rate, fastPattern.duration});
    // VQA2 -- ITW4's cartons are measured on a flat mid grey, which none of
    // their colours can be mistaken for.
    const std::filesystem::path greyPath = gUiSmokeRoot / "gris.mov";
    const std::string generateGrey =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=0x808080:s=320x180:r=25:d=4.2' -c:v mpeg4 -q:v 2 " +
        quote(greyPath);
    if (std::system(generateGrey.c_str()) != 0) {
        error = "unable to create UI smoke grey fixture";
        return false;
    }
    LibraryMedia grey = pattern;
    grey.id = GenerateUlid();
    grey.path = greyPath.string();
    grey.filename = "gris.mov";
    gUiSmokeGreySourceId = grey.id;
    document.library.push_back(grey);
    document.sources.push_back({grey.id, grey.path, grey.rate, grey.duration});
    // REM1 C20 -- the same grey with ten seconds of silence under it: a
    // montage's words are read from the audio tracks, and an audio clip on
    // a file without sound would fail the export, not the captions.
    const std::filesystem::path speechPath = gUiSmokeRoot / "gris-parole.mov";
    const std::string generateSpeech =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=0x808080:s=320x180:r=25:d=10' -f lavfi -i "
        "'anullsrc=r=48000:cl=mono' -t 10 -c:v mpeg4 -q:v 2 -c:a pcm_s16le " +
        quote(speechPath);
    if (std::system(generateSpeech.c_str()) != 0) {
        error = "unable to create UI smoke speech fixture";
        return false;
    }
    LibraryMedia speech = grey;
    speech.id = GenerateUlid();
    speech.path = speechPath.string();
    speech.filename = "gris-parole.mov";
    speech.duration = {250, 25};
    speech.has_audio = true;
    speech.audio_rate = 48000;
    speech.audio_channels = 1;
    gUiSmokeSpeechSourceId = speech.id;
    document.library.push_back(speech);
    document.sources.push_back(
        {speech.id, speech.path, speech.rate, speech.duration});
    // SY2 -- twelve seconds of pink noise from a recorder, and a camera
    // whose sound is the same noise from 0.5 s to 10.5 s: the recorder's
    // first sample falls half a second before the camera's.
    const std::filesystem::path recorderPath = gUiSmokeRoot / "sy2-mix.wav";
    const std::filesystem::path cameraPath = gUiSmokeRoot / "sy2-camera.mov";
    const std::string generateRecorder =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'anoisesrc=d=12:c=pink:r=48000:a=0.4:s=7' -c:a pcm_s16le " +
        quote(recorderPath);
    const std::string generateCamera =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=0x5a6470:s=320x180:r=25:d=10' -i " +
        quote(recorderPath) +
        " -filter_complex "
        "'[1:a]atrim=start=0.5:end=10.5,asetpts=PTS-STARTPTS[a]' "
        "-map 0:v -map '[a]' -t 10 -c:v mpeg4 -q:v 2 -c:a pcm_s16le " +
        quote(cameraPath);
    if (std::system(generateRecorder.c_str()) != 0 ||
        std::system(generateCamera.c_str()) != 0) {
        error = "unable to create UI smoke separate sound fixtures";
        return false;
    }
    LibraryMedia soundCamera = speech;
    soundCamera.id = GenerateUlid();
    soundCamera.path = cameraPath.string();
    soundCamera.filename = "sy2-camera.mov";
    document.library.push_back(soundCamera);
    document.sources.push_back({soundCamera.id, soundCamera.path,
                                soundCamera.rate, soundCamera.duration});
    LibraryMedia soundRecorder;
    soundRecorder.path = recorderPath.string();
    soundRecorder.filename = "sy2-mix.wav";
    soundRecorder.codec = "pcm_s16le";
    soundRecorder.has_video = false;
    soundRecorder.rate = {25, 1};
    soundRecorder.duration = {300, 25};
    soundRecorder.orientation = "audio";
    soundRecorder.has_audio = true;
    soundRecorder.audio_rate = 48000;
    soundRecorder.audio_channels = 1;
    soundRecorder.bin_id = speech.bin_id;
    soundRecorder.metadata_complete = true;
    document.library.push_back(soundRecorder);
    document.sources.push_back({soundRecorder.id, soundRecorder.path,
                                soundRecorder.rate, soundRecorder.duration});
    // M5's logo template resolves relative image paths from the project
    // package. A plain, wide mark makes every corner and safe margin visible
    // without introducing a private asset into the test.
    const std::filesystem::path logoPath = gUiSmokeRoot / "logo.png";
    const std::string generateLogo =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'color=c=0xe6007e:s=400x160:r=1:d=1' -frames:v 1 " +
        quote(logoPath);
    if (std::system(generateLogo.c_str()) != 0) {
        error = "unable to create UI smoke logo fixture";
        return false;
    }
    // VQA2 -- the NTSC case of the transport: the same pattern at 29.97.
    // The library's first media stays the 25 i/s fixture, which is what
    // once made the playhead step at the wrong cadence.
    const std::filesystem::path ntscPatternPath = gUiSmokeRoot / "mire2997.mov";
    if (!makePattern("30000/1001", ntscPatternPath)) {
        error = "unable to create UI smoke 29.97 fps pattern fixture";
        return false;
    }
    LibraryMedia ntscPattern = pattern;
    ntscPattern.id = GenerateUlid();
    ntscPattern.path = ntscPatternPath.string();
    ntscPattern.filename = "mire-2997.mov";
    ntscPattern.rate = {30000, 1001};
    ntscPattern.duration = {125 * 1001, 30000};
    document.library.push_back(ntscPattern);
    document.sources.push_back({ntscPattern.id, ntscPattern.path,
                                ntscPattern.rate, ntscPattern.duration});
    // VQA2 -- R1's rotated case: a portrait test card stored on its side
    // with a display matrix of 90 degrees, as a phone writes it. testsrc2
    // rather than the waves, because its timecode says which way is up.
    const std::filesystem::path sidewaysPath = gUiSmokeRoot / "couche.mov";
    const std::filesystem::path rotatedPath = gUiSmokeRoot / "portrait.mov";
    const std::string generateRotated =
        quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -f lavfi -i "
        "'testsrc2=s=180x320:r=25:d=4.2' -vf transpose=1 -c:v mpeg4 -q:v 2 " +
        quote(sidewaysPath) + " && " + quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -display_rotation 90 -i " +
        quote(sidewaysPath) + " -c copy " + quote(rotatedPath);
    if (std::system(generateRotated.c_str()) != 0) {
        error = "unable to create UI smoke rotated fixture";
        return false;
    }
    LibraryMedia rotated = pattern;
    rotated.id = GenerateUlid();
    rotated.path = rotatedPath.string();
    rotated.filename = "mire-portrait-90.mov";
    rotated.rotation_degrees = 90;
    rotated.orientation = "portrait";
    document.library.push_back(rotated);
    document.sources.push_back(
        {rotated.id, rotated.path, rotated.rate, rotated.duration});
    DocumentClip clip;
    clip.source_id = media.id;
    clip.source_in = {0, 25};
    clip.duration = {50, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    DocumentTrack track;
    track.kind = "video";
    track.index = 0;
    track.clips = {clip};
    DocumentClip audioClip = clip;
    audioClip.id = GenerateUlid();
    audioClip.include_audio = true;
    DocumentTrack audioTrack;
    audioTrack.kind = "audio";
    audioTrack.index = 1;
    audioTrack.clips = {audioClip};
    CaptionStyle captionStyle;
    captionStyle.font_size = 28;
    captionStyle.color = "#ffffff";
    captionStyle.outline_width = 3;
    captionStyle.outline_color = "#000000";
    captionStyle.background_color = "#181818";
    captionStyle.background_padding = 6;
    captionStyle.vertical_position = {3, 4};
    DocumentClip captionClip;
    captionClip.source_in = {0, 25};
    captionClip.duration = {30, 25};
    captionClip.timeline_in = {10, 25};
    captionClip.include_audio = false;
    captionClip.caption_group_id = captionStyle.id;
    captionClip.caption_text = "Carton QA : lisible sur l’image";
    DocumentTrack captionTrack;
    captionTrack.kind = "caption";
    captionTrack.index = 2;
    captionTrack.clips = {captionClip};
    document.sequence.name = "UI Smoke";
    document.sequence.tracks = {track, audioTrack, captionTrack};
    document.sequence.caption_styles = {captionStyle};
    // Visual QA of 19 September 2026 -- a composition runUiSmokeTests places
    // on the timeline at its very end, in the system font so the run needs
    // no installed one. No clip uses it before then.
    {
        DocumentComposition composition;
        composition.name = "Smoke title";
        std::string parseError;
        // A stored composition names every node by a uid.
        if (!mcp_json::Value::Parse(
                R"({"width":64,"height":64,"fps":25,"duration":25,)"
                R"("root":{"type":"fill","uid":")" +
                    GenerateUlid() +
                    R"(","style":{"justifyContent":"center","alignItems":)"
                    R"("center"},"children":[{"type":"text","uid":")" +
                    GenerateUlid() +
                    R"(","content":"QA","style":{"fontFamily":"system-ui",)"
                    R"("fontSize":20,"color":"#ffffff"}}]}})",
                composition.definition, parseError)) {
            error = "unable to build the smoke composition: " + parseError;
            return false;
        }
        document.compositions.push_back(std::move(composition));
        // VQA2 -- M4's measured cases: a 600-pixel white box sliding right
        // 20 px a frame, so an opacity and a frame of the animation both
        // show in every comparison.
        DocumentComposition sliding;
        sliding.name = "QA boîte";
        if (!mcp_json::Value::Parse(
                R"({"width":1920,"height":1080,"fps":25,"duration":60,)"
                R"("root":{"type":"fill","uid":")" +
                    GenerateUlid() + R"(","children":[{"type":"box","uid":")" +
                    GenerateUlid() +
                    R"(","style":{"position":"absolute","left":0,"top":0,)"
                    R"("width":600,"height":1080,"backgroundColor":"#FFFFFF"},)"
                    R"("animate":{"translateX":{"interpolate":{)"
                    R"("frames":[0,59],"values":[0,1180],"uids":[")" +
                    GenerateUlid() + R"(",")" + GenerateUlid() + R"("]}}}}]}})",
                sliding.definition, parseError)) {
            error = "unable to build the QA composition: " + parseError;
            return false;
        }
        gUiSmokeMotionCompositionId = sliding.id;
        document.compositions.push_back(std::move(sliding));

        // REM1 -- the M5 catalog as users receive it, instantiated once per
        // declared format. WithMotionIdentities performs the same document
        // normalization as AddMotionCompositionOperation.
        std::vector<MotionTemplateInfo> templates;
        if (!MotionTemplateCatalog(templates, parseError)) {
            error = "unable to load the motion template catalog: " + parseError;
            return false;
        }
        constexpr std::array<const char*, 3> kTemplateNames = {"nom", "carton",
                                                               "logo"};
        constexpr std::array<std::pair<int32_t, int32_t>, 3> kTemplateSizes = {
            std::pair{1080, 1920}, std::pair{1920, 1080},
            std::pair{1080, 1080}};
        for (size_t format = 0; format < kTemplateSizes.size(); ++format) {
            for (size_t kind = 0; kind < kTemplateNames.size(); ++kind) {
                const auto found =
                    std::find_if(templates.begin(), templates.end(),
                                 [&](const MotionTemplateInfo& info) {
                                     return info.name == kTemplateNames[kind];
                                 });
                if (found == templates.end()) {
                    error = "motion template is missing: " +
                            std::string(kTemplateNames[kind]);
                    return false;
                }
                DocumentComposition stored;
                stored.name = "QA M5 " + found->name;
                std::string chosen;
                if (!InstantiateMotionTemplate(
                        found->definition, kTemplateSizes[format].first,
                        kTemplateSizes[format].second, stored.definition,
                        chosen, parseError)) {
                    error = "unable to instantiate motion template '" +
                            found->name + "': " + parseError;
                    return false;
                }
                const mcp_json::Value* root = stored.definition.Find("root");
                if (root == nullptr) {
                    error = "motion template has no root: " + found->name;
                    return false;
                }
                stored.definition.Set("root", WithMotionIdentities(*root));
                gUiSmokeTemplateCompositionIds[format][kind] = stored.id;
                document.compositions.push_back(std::move(stored));
            }
        }
    }
    Project project = Project::FromDocument(std::move(document), "UI Smoke");
    // VQA2 -- timelines the main run never opens, one per REEL section the
    // QA scenarios measure. Each shot reads the pattern from `source_in`
    // (frames) for `frames` frames at `at` on the first video track.
    const auto shotOf = [](const Ulid& source, int64_t sourceIn, int32_t rate,
                           int64_t frames, int64_t at) {
        DocumentClip shot;
        shot.source_id = source;
        shot.source_in = {sourceIn, rate};
        shot.duration = {frames, 25};
        shot.timeline_in = {at, 25};
        shot.include_audio = false;
        return shot;
    };
    const auto addTimeline = [&](const char* name, int32_t width,
                                 int32_t height,
                                 const std::vector<DocumentClip>& shots) {
        DocumentSequence sequence;
        sequence.name = name;
        sequence.width = width;
        sequence.height = height;
        sequence.tracks = DefaultTimelineTracks(GenerateUlid(), GenerateUlid());
        for (DocumentTrack& track : sequence.tracks)
            if (track.kind == "video") track.clips = shots;
        const Ulid id = sequence.id;
        project.timelines.push_back(std::move(sequence));
        return id;
    };
    {
        const DocumentClip framed = shotOf(pattern.id, 0, 25, 50, 0);
        const DocumentClip upright = shotOf(rotated.id, 12, 25, 50, 50);
        gUiSmokeFramingClipId = framed.id;
        gUiSmokeRotatedClipId = upright.id;
        gUiSmokeFramingTimelineId =
            addTimeline("QA cadrage", 1080, 1920, {framed, upright});

        const DocumentClip slowed = shotOf(fastPattern.id, 0, 50, 50, 0);
        gUiSmokeSpeedClipId = slowed.id;
        gUiSmokeSpeedTimelineId =
            addTimeline("QA vitesse", 1920, 1080, {slowed});

        // Three shots end to end, each read far enough into the pattern
        // for its speed to fit: 40 frames at 1/2 from source 0, 25 at 2
        // from source 50 (up to 99), 40 at 2/5 from source 10.
        const DocumentClip half = shotOf(pattern.id, 0, 25, 40, 0);
        const DocumentClip twice = shotOf(pattern.id, 50, 25, 25, 40);
        const DocumentClip fifths = shotOf(pattern.id, 10, 25, 40, 65);
        gUiSmokeCadenceHalfClipId = half.id;
        gUiSmokeCadenceDoubleClipId = twice.id;
        gUiSmokeCadenceTwoFifthsClipId = fifths.id;
        // Three cartons of 20 frames over 75 frames of grey.
        // REM1 C20 -- rush [0, 100) then [150, 250): a jump cut at 100.
        gUiSmokeSpeechTimelineId =
            addTimeline("QA sous-titres C20", 1920, 1080,
                        {shotOf(speech.id, 0, 25, 100, 0),
                         shotOf(speech.id, 150, 25, 100, 100)});
        for (DocumentTrack& track : project.timelines.back().tracks) {
            if (track.kind != "audio") continue;
            for (const DocumentClip& shot :
                 project.timelines.back().tracks.front().clips) {
                DocumentClip sound = shot;
                sound.id = GenerateUlid();
                sound.include_audio = true;
                track.clips.push_back(sound);
            }
        }
        gUiSmokeCaptionTimelineId = addTimeline(
            "QA légendes", 1920, 1080, {shotOf(grey.id, 0, 25, 75, 0)});
        {
            DocumentSequence& captions = project.timelines.back();
            CaptionStyle outlined;
            outlined.font_size = 64;
            outlined.outline_width = 4;
            outlined.vertical_position = {3, 4};
            CaptionStyle boxed = outlined;
            boxed.id = GenerateUlid();
            boxed.outline_width = 0;
            boxed.shadow_offset = 6;
            boxed.shadow_color = "#ff0000";
            boxed.background_color = "#1a1a8cc0";
            boxed.background_padding = 12;
            CaptionStyle wrapped = outlined;
            wrapped.id = GenerateUlid();
            captions.caption_styles = {outlined, boxed, wrapped};
            const auto carton = [](const CaptionStyle& style, int64_t at,
                                   const char* text) {
                DocumentClip clip;
                clip.source_in = {0, 25};
                clip.duration = {20, 25};
                clip.timeline_in = {at, 25};
                clip.include_audio = false;
                clip.caption_group_id = style.id;
                clip.caption_text = text;
                return clip;
            };
            DocumentTrack track;
            track.kind = "caption";
            track.index = static_cast<int32_t>(captions.tracks.size());
            track.clips = {
                carton(outlined, 0, "Carton contour"),
                carton(boxed, 25, "Carton fond et ombre"),
                carton(wrapped, 50,
                       "Un carton bien trop long pour tenir sur une seule "
                       "ligne de ce cadre, il doit se replier"),
            };
            captions.tracks.push_back(std::move(track));
        }

        gUiSmokeMotionTimelineId = addTimeline(
            "QA motion", 1920, 1080, {shotOf(pattern.id, 0, 25, 50, 0)});
        {
            DocumentTrack over;
            over.kind = "video";
            over.index =
                static_cast<int32_t>(project.timelines.back().tracks.size());
            gUiSmokeMotionTrackId = over.id;
            project.timelines.back().tracks.push_back(std::move(over));
        }

        // REM1 -- three format timelines for the M5 templates. Four equal
        // windows hold the name, card, logo, then the logo in all four
        // corners. The 16:9 name crosses a cut between two clips of the same
        // source and ends when the grey shot from another source begins.
        constexpr std::array<const char*, 3> kTemplateTimelineNames = {
            "QA M5 9x16", "QA M5 16x9", "QA M5 1x1"};
        constexpr std::array<std::pair<int32_t, int32_t>, 3> kTemplateSizes = {
            std::pair{1080, 1920}, std::pair{1920, 1080},
            std::pair{1080, 1080}};
        for (size_t format = 0; format < kTemplateSizes.size(); ++format) {
            std::vector<DocumentClip> background;
            if (format == 1) {
                background = {
                    shotOf(pattern.id, 0, 25, 25, 0),
                    shotOf(pattern.id, 25, 25, 25, 25),
                    shotOf(grey.id, 0, 25, 50, 50),
                    shotOf(pattern.id, 0, 25, 50, 100),
                    shotOf(pattern.id, 0, 25, 50, 150),
                };
            } else {
                for (int64_t start : {0, 50, 100, 150})
                    background.push_back(shotOf(pattern.id, 0, 25, 50, start));
            }
            gUiSmokeTemplateTimelineIds[format] = addTimeline(
                kTemplateTimelineNames[format], kTemplateSizes[format].first,
                kTemplateSizes[format].second, background);
            for (size_t layer = 0; layer < 4; ++layer) {
                DocumentTrack overlay;
                overlay.kind = "video";
                overlay.index = static_cast<int32_t>(
                    project.timelines.back().tracks.size());
                if (layer == 0) gUiSmokeTemplateTrackIds[format] = overlay.id;
                if (format == 1)
                    gUiSmokeTemplateCornerTrackIds[layer] = overlay.id;
                project.timelines.back().tracks.push_back(std::move(overlay));
            }
        }

        {
            const DocumentClip wide = shotOf(pattern.id, 0, 25, 50, 0);
            DocumentClip close = shotOf(grey.id, 0, 25, 50, 0);
            gUiSmokeMulticamPatternClipId = wide.id;
            gUiSmokeMulticamGreyClipId = close.id;
            gUiSmokeMulticamTimelineId =
                addTimeline("QA multicam", 1920, 1080, {wide});
            DocumentTrack upper;
            upper.kind = "video";
            upper.index =
                static_cast<int32_t>(project.timelines.back().tracks.size());
            upper.clips = {close};
            project.timelines.back().tracks.push_back(std::move(upper));
        }

        gUiSmokeRollTimelineId =
            addTimeline("QA roll", 1920, 1080, {shotOf(grey.id, 0, 25, 75, 0)});
        {
            DocumentSequence& roll = project.timelines.back();
            CaptionStyle style;
            style.font_size = 64;
            style.outline_width = 4;
            roll.caption_styles = {style};
            DocumentTrack track;
            track.kind = "caption";
            track.index = static_cast<int32_t>(roll.tracks.size());
            for (int index = 0; index < 2; ++index) {
                DocumentClip carton;
                carton.source_in = {0, 25};
                carton.duration = {30, 25};
                carton.timeline_in = {30 * index, 25};
                carton.include_audio = false;
                carton.caption_group_id = style.id;
                carton.caption_text =
                    index == 0 ? "Premier carton" : "Second carton";
                (index == 0 ? gUiSmokeRollFirstCartonId
                            : gUiSmokeRollSecondCartonId) = carton.id;
                track.clips.push_back(carton);
            }
            roll.tracks.push_back(std::move(track));
        }

        // 60 frames of the 29.97 pattern at 2:59:10 of a 29.97 sequence.
        DocumentClip ntsc;
        ntsc.source_id = ntscPattern.id;
        ntsc.source_in = {0, 30000};
        ntsc.duration = {60 * 1001, 30000};
        ntsc.timeline_in = {5380 * 1001, 30000};
        ntsc.include_audio = false;
        gUiSmokeNtscClipId = ntsc.id;
        gUiSmokeNtscTimelineId = addTimeline("QA NTSC", 640, 360, {ntsc});
        project.timelines.back().frame_rate = {30000, 1001};

        gUiSmokeCadenceTimelineId =
            addTimeline("QA cadences", 1920, 1080, {half, twice, fifths});

        const DocumentClip animated = shotOf(pattern.id, 0, 25, 50, 0);
        gUiSmokeAnimationClipId = animated.id;
        gUiSmokeAnimationTimelineId =
            addTimeline("QA animation", 1920, 1080, {animated});

        // Two shots that meet at frame 50, the second read further in so the
        // cut is visible.
        const DocumentClip first = shotOf(pattern.id, 0, 25, 50, 0);
        const DocumentClip second = shotOf(pattern.id, 52, 25, 50, 50);
        gUiSmokeDipFirstClipId = first.id;
        gUiSmokeDipSecondClipId = second.id;
        gUiSmokeDipTimelineId =
            addTimeline("QA fondus", 1920, 1080, {first, second});

        const DocumentClip looked = shotOf(pattern.id, 0, 25, 50, 0);
        gUiSmokeLookClipId = looked.id;
        gUiSmokeLookTimelineId = addTimeline("QA look", 1920, 1080, {looked});

        // PRD24 -- ordinary shots end to end, for the run to slow, reframe,
        // animate and grade through the edit log. The run looks at them at
        // 40 points a second, where a shot of 40 frames is 64 points wide:
        // room for any one badge. `loaded`, `tight` and `sliver` will carry
        // the same four things at three widths -- 160, 40 and 16 points --
        // so that the first shows all four, the second its speed alone and
        // the third nothing.
        {
            const DocumentClip plain = shotOf(pattern.id, 0, 25, 40, 0);
            DocumentClip fast = shotOf(speech.id, 0, 25, 40, 40);
            const DocumentClip slow = shotOf(pattern.id, 0, 25, 40, 80);
            const DocumentClip framed = shotOf(pattern.id, 0, 25, 40, 120);
            const DocumentClip graded = shotOf(pattern.id, 0, 25, 40, 160);
            const DocumentClip animated = shotOf(pattern.id, 0, 25, 40, 200);
            const DocumentClip loaded = shotOf(pattern.id, 0, 25, 100, 240);
            const DocumentClip tight = shotOf(pattern.id, 0, 25, 25, 340);
            const DocumentClip sliver = shotOf(pattern.id, 0, 25, 10, 365);
            // The two angles of a multicam group, one over the other.
            const DocumentClip angle = shotOf(pattern.id, 0, 25, 40, 375);
            const DocumentClip otherAngle = shotOf(grey.id, 0, 25, 40, 375);
            // The fast shot's sound, linked to it and three frames late: the
            // drift an audio clip already says, beside which its speed has
            // to find room.
            fast.link_group_id = GenerateUlid();
            fast.sync_anchor_clip_id = fast.id;
            DocumentClip sound = fast;
            sound.id = GenerateUlid();
            sound.include_audio = true;
            sound.timeline_in = {43, 25};
            gUiSmokeSignagePlainClipId = plain.id;
            gUiSmokeSignageFastClipId = fast.id;
            gUiSmokeSignageSoundClipId = sound.id;
            gUiSmokeSignageSlowClipId = slow.id;
            gUiSmokeSignageFramedClipId = framed.id;
            gUiSmokeSignageGradedClipId = graded.id;
            gUiSmokeSignageAnimatedClipId = animated.id;
            gUiSmokeSignageLoadedClipId = loaded.id;
            gUiSmokeSignageTightClipId = tight.id;
            gUiSmokeSignageSliverClipId = sliver.id;
            gUiSmokeSignageActiveAngleClipId = angle.id;
            gUiSmokeSignageHiddenAngleClipId = otherAngle.id;
            gUiSmokeSignageTimelineId =
                addTimeline("QA signalétique", 1920, 1080,
                            {plain, fast, slow, framed, graded, animated,
                             loaded, tight, sliver, angle});
            DocumentSequence& signage = project.timelines.back();
            for (DocumentTrack& track : signage.tracks)
                if (track.kind == "audio") track.clips = {sound};
            DocumentTrack upper;
            upper.kind = "video";
            upper.index = static_cast<int32_t>(signage.tracks.size());
            upper.clips = {otherAngle};
            signage.tracks.push_back(std::move(upper));
        }

        // PRD25 -- three shots of two seconds end to end, for an agent to
        // edit through the window's endpoint while the run reads the Agent
        // panel and the marks. At 80 points a second, the half of one cut
        // in two is still wide enough for the agent's badge.
        {
            const DocumentClip first = shotOf(pattern.id, 0, 25, 50, 0);
            const DocumentClip second = shotOf(pattern.id, 0, 25, 50, 50);
            const DocumentClip third = shotOf(pattern.id, 0, 25, 50, 100);
            gUiSmokeJournalClipIds = {first.id, second.id, third.id};
            gUiSmokeJournalTimelineId = addTimeline(
                "QA journal de l'agent", 1920, 1080, {first, second, third});
        }
        // FC-fenêtre -- A [0, 30) reads the pattern 10..40, B [30, 60)
        // reads 50..80, C [60, 85) reads 20..45: every cut has media on
        // both sides, and A and B come from the same rush, which the Trim
        // Monitor then shows twice.
        {
            const DocumentClip first = shotOf(pattern.id, 10, 25, 30, 0);
            const DocumentClip second = shotOf(pattern.id, 50, 25, 30, 30);
            const DocumentClip third = shotOf(pattern.id, 20, 25, 25, 60);
            gUiSmokeTrimClipIds = {first.id, second.id, third.id};
            gUiSmokeTrimTimelineId =
                addTimeline("QA trim", 1920, 1080, {first, second, third});
        }
        // SY2 -- the picture [1 s, 5 s) reads the camera from 2 s, its own
        // sound linked under it on A1; the grey picture [6 s, 8 s) reads
        // the silent rush; the recording lies whole on A2 from 0, where
        // it is half a second late on... nothing: Auto Align places it.
        {
            const DocumentClip picture =
                shotOf(soundCamera.id, 50, 25, 100, 25);
            const DocumentClip silent = shotOf(speech.id, 0, 25, 50, 150);
            gUiSmokeSoundPictureId = picture.id;
            gUiSmokeSoundSilentPictureId = silent.id;
            gUiSmokeSoundTimelineId =
                addTimeline("QA son séparé", 1920, 1080, {picture, silent});
            DocumentSequence& sound = project.timelines.back();
            const Ulid group = GenerateUlid();
            DocumentClip camera = picture;
            camera.id = GenerateUlid();
            camera.include_audio = true;
            camera.link_group_id = group;
            camera.sync_anchor_clip_id = picture.id;
            gUiSmokeSoundCameraId = camera.id;
            for (DocumentTrack& track : sound.tracks) {
                if (track.kind == "video") {
                    track.clips.front().link_group_id = group;
                    track.clips.front().sync_anchor_clip_id = picture.id;
                } else {
                    track.clips = {camera};
                }
            }
            DocumentClip recorder;
            recorder.source_id = soundRecorder.id;
            recorder.source_in = {0, 25};
            recorder.duration = {300, 25};
            recorder.timeline_in = {0, 25};
            gUiSmokeSoundRecorderId = recorder.id;
            DocumentTrack second;
            second.kind = "audio";
            second.index = static_cast<int32_t>(sound.tracks.size());
            second.clips = {recorder};
            sound.tracks.push_back(std::move(second));
        }
        gUiSmokeScreenTimelineId =
            addTimeline("QA écran", 1920, 1080,
                        {shotOf(pattern.id, 0, 25, 50, 0),
                         shotOf(pattern.id, 50, 25, 50, 50),
                         shotOf(grey.id, 0, 25, 50, 100)});
        {
            // Three cartons on the shots' cuts, the last of a single word,
            // in a style the export finds on any Mac.
            DocumentSequence& screen = project.timelines.back();
            CaptionStyle style;
            style.font_size = 64;
            style.outline_width = 4;
            style.vertical_position = {3, 4};
            screen.caption_styles = {style};
            DocumentTrack track;
            track.kind = "caption";
            track.index = static_cast<int32_t>(screen.tracks.size());
            const std::array<const char*, 3> words = {"Premier carton",
                                                      "Deuxième carton", "Mot"};
            for (size_t index = 0; index < words.size(); ++index) {
                DocumentClip carton;
                carton.source_in = {0, 25};
                carton.duration = {50, 25};
                carton.timeline_in = {static_cast<int64_t>(50 * index), 25};
                carton.include_audio = false;
                carton.caption_group_id = style.id;
                carton.caption_text = words[index];
                gUiSmokeScreenCartonIds[index] = carton.id;
                track.clips.push_back(carton);
            }
            screen.tracks.push_back(std::move(track));
        }
    }
    // R10 -- an empty timeline for the Resolve read-back, which
    // --import-resolve-timeline fills while it is the active one.
    const char* resolveMedia = std::getenv("CUTMACHINE_UI_QA_RESOLVE_MEDIA");
    const char* resolveTimeline =
        std::getenv("CUTMACHINE_UI_QA_RESOLVE_TIMELINE");
    const bool readResolve = !gUiQaOutputDirectory.empty() &&
                             resolveMedia != nullptr &&
                             resolveTimeline != nullptr;
    const Ulid mainTimelineId = project.active_timeline_id;
    Ulid resolveTimelineId;
    if (readResolve)
        resolveTimelineId = addTimeline("QA Resolve", 1920, 1080, {});
    const std::filesystem::path package =
        gUiSmokeRoot / "UI Smoke.cutmachine-project";
    if (!CreatePortableProject(package.string(), project, gUiSmokeProjectPath,
                               error))
        return false;
    std::error_code logoCopyError;
    std::filesystem::copy_file(
        logoPath, package / "logo.png",
        std::filesystem::copy_options::overwrite_existing, logoCopyError);
    if (logoCopyError) {
        error = "unable to install the UI smoke logo fixture: " +
                logoCopyError.message();
        return false;
    }
    if (!readResolve) return true;
    McpProjectBackend backend(gUiSmokeProjectPath);
    const McpToolRegistry registry;
    const auto activate = [&](const Ulid& id) {
        mcp_json::Value arguments = mcp_json::Value::MakeObject();
        arguments.Set("timeline_id", mcp_json::Value::MakeString(id));
        return registry.Call(backend, "set_active_timeline", arguments).ok;
    };
    std::string mediaOutput;
    std::string timelineOutput;
    if (!activate(resolveTimelineId) ||
        ImportResolveCommand(gUiSmokeProjectPath, resolveMedia, mediaOutput) !=
            0 ||
        ImportResolveTimelineCommand(gUiSmokeProjectPath, resolveTimeline,
                                     timelineOutput) != 0 ||
        !activate(mainTimelineId)) {
        error = "unable to read the Resolve timeline back: " + mediaOutput +
                timelineOutput;
        return false;
    }
    std::fprintf(stdout, "NOTE: VQA R10 import: %s", timelineOutput.c_str());
    gUiSmokeResolveTimelineId = resolveTimelineId;
    return true;
}

}  // namespace cutmachine_app

static NSButton* FindButtonWithTitle(NSView* root, NSString* title) {
    if ([root isKindOfClass:NSButton.class] &&
        [((NSButton*)root).title isEqualToString:title])
        return (NSButton*)root;
    for (NSView* child in root.subviews) {
        if (NSButton* match = FindButtonWithTitle(child, title)) return match;
    }
    return nil;
}

// PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. The visible buttons under
// `root` that say in no word what they do: neither their title nor their
// accessibility label holds a letter -- an empty title, a glyph such as
// « ↺ », a value such as « 100 % ». VoiceOver can read them only as their
// glyph, or not at all. A letter, not a label: AppKit hands a button's title
// back as its label when none was set, « ↺ » included. AppKit's own
// disclosure buttons are left out -- an outline's, which VoiceOver reads as
// its row expanded or not, and any by its disclosure role. A hidden view
// hides its subviews; `seen` counts the visible buttons looked at.
static void CollectUnnamedButtons(NSView* root, size_t& seen,
                                  std::vector<std::string>& unnamed) {
    if (root.hidden) return;
    const bool disclosure =
        [root.identifier isEqualToString:NSOutlineViewDisclosureButtonKey] ||
        [root.identifier isEqualToString:NSOutlineViewShowHideButtonKey] ||
        [root.accessibilityRole
            isEqualToString:NSAccessibilityDisclosureTriangleRole];
    if ([root isKindOfClass:NSButton.class] && !NSIsEmptyRect(root.frame) &&
        !disclosure) {
        NSButton* button = (NSButton*)root;
        NSString* title = button.title ?: @"";
        NSString* label = button.accessibilityLabel ?: @"";
        NSCharacterSet* letters = NSCharacterSet.letterCharacterSet;
        ++seen;
        if ([title rangeOfCharacterFromSet:letters].location == NSNotFound &&
            [label rangeOfCharacterFromSet:letters].location == NSNotFound)
            unnamed.push_back(
                std::string(NSStringFromClass(button.class).UTF8String) +
                " in " +
                std::string(
                    NSStringFromClass(button.superview.class).UTF8String) +
                " « " + std::string(title.UTF8String ?: "") + " » (" +
                std::string(button.accessibilityRole.UTF8String ?: "") + ")");
    }
    for (NSView* child in root.subviews)
        CollectUnnamedButtons(child, seen, unnamed);
}

// The Inspector's sliders of the clip: its opacity and its grade. F2.3 --
// the height of a carton's style (a CMTrackedSlider, the panel's only one)
// is the style's, not the clip's, and is not counted.
static void CollectSliders(NSView* root, NSMutableArray<NSSlider*>* result) {
    if ([root isKindOfClass:NSSlider.class] &&
        ![root isKindOfClass:CMTrackedSlider.class])
        [result addObject:(NSSlider*)root];
    for (NSView* child in root.subviews) CollectSliders(child, result);
}

// R2c -- docs/roadmap/REEL-2026-09.md. The button under `root` whose
// VoiceOver label starts with `prefix`: the Inspector names each diamond and
// arrow after its row (« Opacité : clé précédente »).
static NSButton* FindButtonLabelled(NSView* root, NSString* prefix) {
    if ([root isKindOfClass:NSButton.class] &&
        [((NSButton*)root).accessibilityLabel hasPrefix:prefix])
        return (NSButton*)root;
    for (NSView* child in root.subviews) {
        NSButton* found = FindButtonLabelled(child, prefix);
        if (found != nil) return found;
    }
    return nil;
}

// GR4, GR5 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The first view of a
// class under `root`.
static NSView* FindSubviewOfClass(NSView* root, Class kind) {
    if ([root isKindOfClass:kind]) return root;
    for (NSView* child in root.subviews) {
        NSView* found = FindSubviewOfClass(child, kind);
        if (found != nil) return found;
    }
    return nil;
}

// PRD37 -- ROADMAP.md. How many times AppKit has drawn each colour wheel, so
// that the window test can say a commit redraws one. -needsDisplay cannot:
// on macOS 26 it reads NO for a layer-backed view even right after it was
// set (measured on 3 October 2026), and drawing waits for a turn of the run
// loop. Installed once, test-only: the wheel's own drawing is called
// through.
static NSMapTable<NSView*, NSNumber*>* gUiSmokeWheelDraws = nil;
static IMP gUiSmokeWheelDrawRect = nullptr;

static void UiSmokeCountedWheelDrawRect(id view, SEL selector, NSRect dirty) {
    NSNumber* count = [gUiSmokeWheelDraws objectForKey:view];
    [gUiSmokeWheelDraws setObject:@(count.integerValue + 1) forKey:view];
    reinterpret_cast<void (*)(id, SEL, NSRect)>(gUiSmokeWheelDrawRect)(
        view, selector, dirty);
}

static void UiSmokeCountWheelDraws() {
    if (gUiSmokeWheelDrawRect != nullptr) return;
    gUiSmokeWheelDraws = [NSMapTable weakToStrongObjectsMapTable];
    Method method =
        class_getInstanceMethod(CMGradeWheelView.class, @selector(drawRect:));
    gUiSmokeWheelDrawRect = method_setImplementation(
        method, reinterpret_cast<IMP>(UiSmokeCountedWheelDrawRect));
}

static NSInteger UiSmokeWheelDrawCount(NSView* view) {
    return [[gUiSmokeWheelDraws objectForKey:view] integerValue];
}

// A press, a drag and a release handed to a grading control as AppKit hands
// them: `start` and `end` in the view's own coordinates, the modifier keys
// read off each event. `clicks` of 2 is a double click, which has no drag.
static void SendGradeGesture(NSView* view, NSPoint start, NSPoint end,
                             NSEventModifierFlags modifiers,
                             NSInteger clicks = 1) {
    NSWindow* window = view.window;
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSPoint location,
                           NSTimeInterval offset) {
        return
            [NSEvent mouseEventWithType:type
                               location:[view convertPoint:location toView:nil]
                          modifierFlags:modifiers
                              timestamp:timestamp + offset
                           windowNumber:window.windowNumber
                                context:nil
                            eventNumber:0
                             clickCount:clicks
                               pressure:type == NSEventTypeLeftMouseUp ||
                                                type == NSEventTypeRightMouseUp
                                            ? 0.0
                                            : 1.0];
    };
    [view mouseDown:event(NSEventTypeLeftMouseDown, start, 0.0)];
    if (clicks > 1) return;
    if (!NSEqualPoints(start, end))
        [view mouseDragged:event(NSEventTypeLeftMouseDragged, end, 0.01)];
    [view mouseUp:event(NSEventTypeLeftMouseUp, end, 0.02)];
}

static void ClickControlThroughWindow(NSControl* control) {
    NSWindow* window = control.window;
    const NSPoint local =
        NSMakePoint(NSMidX(control.bounds), NSMidY(control.bounds));
    const NSPoint windowPoint = [control convertPoint:local toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:windowPoint
                                  modifierFlags:0
                                      timestamp:timestamp
                                   windowNumber:window.windowNumber
                                        context:nil
                                    eventNumber:0
                                     clickCount:1
                                       pressure:1.0];
    NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:windowPoint
                                modifierFlags:0
                                    timestamp:timestamp + 0.01
                                 windowNumber:window.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.0];
    // NSButton's mouseDown enters a tracking loop. Queue mouse-up first so
    // that loop consumes it exactly as it would for a physical click.
    [NSApp postEvent:up atStart:NO];
    [window sendEvent:down];
}

static void SendWindowClick(
    NSView* view, NSPoint point,
    NSEventModifierFlags modifiers = static_cast<NSEventModifierFlags>(0)) {
    NSWindow* window = view.window;
    if (!window) return;
    const NSPoint windowPoint = [view convertPoint:point toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:windowPoint
                                  modifierFlags:modifiers
                                      timestamp:timestamp
                                   windowNumber:window.windowNumber
                                        context:nil
                                    eventNumber:0
                                     clickCount:1
                                       pressure:1.0];
    NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:windowPoint
                                modifierFlags:modifiers
                                    timestamp:timestamp + 0.01
                                 windowNumber:window.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.0];
    [NSApp postEvent:up atStart:NO];
    [window sendEvent:down];
}

static void SendTimelineMouseGesture(NSView* view, NSPoint start, NSPoint end) {
    NSWindow* window = view.window;
    const NSPoint windowStart = [view convertPoint:start toView:nil];
    const NSPoint windowEnd = [view convertPoint:end toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSPoint location,
                           NSTimeInterval offset) {
        return [NSEvent
            mouseEventWithType:type
                      location:location
                 modifierFlags:0
                     timestamp:timestamp + offset
                  windowNumber:window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:1
                      pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    };
    TimelineMetalView* timeline = (TimelineMetalView*)view;
    [timeline mouseDown:event(NSEventTypeLeftMouseDown, windowStart, 0.0)];
    if (!NSEqualPoints(start, end))
        [timeline
            mouseDragged:event(NSEventTypeLeftMouseDragged, windowEnd, 0.01)];
    [timeline mouseUp:event(NSEventTypeLeftMouseUp, windowEnd, 0.02)];
}

// The same gesture holding modifier keys, which the timeline reads off each
// event: Cmd turns a drag on a boundary into a roll.
static void SendTimelineMouseGesture(NSView* view, NSPoint start, NSPoint end,
                                     NSEventModifierFlags modifiers) {
    NSWindow* window = view.window;
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSPoint location,
                           NSTimeInterval offset) {
        return [NSEvent
            mouseEventWithType:type
                      location:[view convertPoint:location toView:nil]
                 modifierFlags:modifiers
                     timestamp:timestamp + offset
                  windowNumber:window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:1
                      pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    };
    TimelineMetalView* timeline = (TimelineMetalView*)view;
    [timeline mouseDown:event(NSEventTypeLeftMouseDown, start, 0.0)];
    [timeline mouseDragged:event(NSEventTypeLeftMouseDragged, end, 0.01)];
    [timeline mouseUp:event(NSEventTypeLeftMouseUp, end, 0.02)];
}

// D7 -- two clicks at one point, the second counted as AppKit counts it.
static void SendTimelineDoubleClick(NSView* view, NSPoint point) {
    NSWindow* window = view.window;
    const NSPoint location = [view convertPoint:point toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSInteger count,
                           NSTimeInterval offset) {
        return [NSEvent
            mouseEventWithType:type
                      location:location
                 modifierFlags:0
                     timestamp:timestamp + offset
                  windowNumber:window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:count
                      pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    };
    TimelineMetalView* timeline = (TimelineMetalView*)view;
    [timeline mouseDown:event(NSEventTypeLeftMouseDown, 1, 0.0)];
    [timeline mouseUp:event(NSEventTypeLeftMouseUp, 1, 0.01)];
    [timeline mouseDown:event(NSEventTypeLeftMouseDown, 2, 0.02)];
    [timeline mouseUp:event(NSEventTypeLeftMouseUp, 2, 0.03)];
}

static void SendWindowDragGesture(NSView* sourceView, NSPoint sourcePoint,
                                  NSView* destinationView,
                                  NSPoint destinationPoint) {
    NSWindow* window = sourceView.window;
    if (!window || destinationView.window != window) return;
    const NSPoint windowStart = [sourceView convertPoint:sourcePoint
                                                  toView:nil];
    const NSPoint windowEnd = [destinationView convertPoint:destinationPoint
                                                     toView:nil];
    const NSTimeInterval timestamp = NSProcessInfo.processInfo.systemUptime;
    const auto event = [&](NSEventType type, NSPoint location,
                           NSTimeInterval offset) {
        return [NSEvent
            mouseEventWithType:type
                      location:location
                 modifierFlags:0
                     timestamp:timestamp + offset
                  windowNumber:window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:1
                      pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    };
    const NSPoint windowThreshold =
        NSMakePoint(windowStart.x + 12.0, windowStart.y);
    NSEvent* threshold =
        event(NSEventTypeLeftMouseDragged, windowThreshold, 0.03);
    NSEvent* dragged = event(NSEventTypeLeftMouseDragged, windowEnd, 0.07);
    NSEvent* up = event(NSEventTypeLeftMouseUp, windowEnd, 0.10);
    [NSApp postEvent:threshold atStart:NO];
    [NSApp postEvent:dragged atStart:NO];
    [NSApp postEvent:up atStart:NO];
    [window sendEvent:event(NSEventTypeLeftMouseDown, windowStart, 0.0)];
}

static void SendKeyThroughWindow(NSView* view, NSString* characters,
                                 unsigned short keyCode) {
    NSWindow* window = view.window;
    [window makeFirstResponder:view];
    NSEvent* event =
        [NSEvent keyEventWithType:NSEventTypeKeyDown
                               location:NSZeroPoint
                          modifierFlags:0
                              timestamp:NSProcessInfo.processInfo.systemUptime
                           windowNumber:window.windowNumber
                                context:nil
                             characters:characters
            charactersIgnoringModifiers:characters
                              isARepeat:NO
                                keyCode:keyCode];
    [window sendEvent:event];
}

static void SendKeyThroughApplication(NSView* view, NSString* characters,
                                      unsigned short keyCode) {
    NSWindow* window = view.window;
    [window makeFirstResponder:view];
    NSEvent* event =
        [NSEvent keyEventWithType:NSEventTypeKeyDown
                               location:NSZeroPoint
                          modifierFlags:0
                              timestamp:NSProcessInfo.processInfo.systemUptime
                           windowNumber:window.windowNumber
                                context:nil
                             characters:characters
            charactersIgnoringModifiers:characters
                              isARepeat:NO
                                keyCode:keyCode];
    [NSApp sendEvent:event];
}

// PRD25 -- an agent's end of the window's endpoint (McpEndpoint.h): one
// JSON-RPC message a line on its Unix socket, as the relay Claude Code
// spawns speaks to it. Blocking; the run drives it from another thread.
class UiSmokeAgentLink {
public:
    ~UiSmokeAgentLink() { Close(); }
    bool Connect(const std::string& socketPath) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socketPath.empty() || socketPath.size() >= sizeof address.sun_path)
            return false;
        std::memcpy(address.sun_path, socketPath.c_str(),
                    socketPath.size() + 1);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof address) != 0) {
            ::close(fd);
            return false;
        }
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        fd_.store(fd);
        return true;
    }
    bool Send(const std::string& line) {
        const std::string data = line + "\n";
        size_t written = 0;
        while (written < data.size()) {
            const ssize_t sent = ::send(fd_.load(), data.data() + written,
                                        data.size() - written, 0);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) return false;
            written += static_cast<size_t>(sent);
        }
        return true;
    }
    bool Receive(std::string& line) {
        for (;;) {
            const size_t end = buffer_.find('\n');
            if (end != std::string::npos) {
                line = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                return true;
            }
            char chunk[65536];
            const ssize_t received = ::recv(fd_.load(), chunk, sizeof chunk, 0);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) return false;
            buffer_.append(chunk, static_cast<size_t>(received));
        }
    }
    // Also what unblocks a thread waiting in Receive.
    void Close() {
        const int fd = fd_.exchange(-1);
        if (fd < 0) return;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

private:
    std::atomic<int> fd_{-1};
    std::string buffer_;
};

// The text of the Agent panel's journal rows under `view`, top to bottom
// (ChatPanelView.h: their identifier is "chat.agent_journal").
static void UiSmokeAgentJournalRows(NSView* view,
                                    std::vector<std::string>& rows) {
    if ([view.identifier isEqualToString:@"chat.agent_journal"]) {
        for (NSView* child in view.subviews) {
            if ([child.identifier isEqualToString:@"chat.message.body"] &&
                [child isKindOfClass:NSTextView.class]) {
                rows.push_back(((NSTextView*)child).string.UTF8String ?: "");
                return;
            }
        }
        rows.push_back("");
        return;
    }
    for (NSView* child in view.subviews) UiSmokeAgentJournalRows(child, rows);
}

// QA pass of 8 October 2026 (docs/qa/2026-10-08-ui-agentique.md). The
// screen pass runs from the main run loop, outside the main-queue block of
// -runUiSmokeTests: what the window defers to the main queue -- the note a
// double click opens, the text system's handling of a typed key -- runs
// while this pumps.
static bool UiSmokeWaitUntil(const std::function<bool()>& done,
                             double seconds) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!done() && deadline.timeIntervalSinceNow > 0) {
        NSEvent* event = [NSApp
            nextEventMatchingMask:NSEventMaskAny
                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                           inMode:NSDefaultRunLoopMode
                          dequeue:YES];
        if (event) [NSApp sendEvent:event];
    }
    return done();
}

static NSEvent* UiSmokeKeyEvent(NSWindow* window, NSString* characters,
                                NSString* unmodified, unsigned short keyCode,
                                NSEventModifierFlags modifiers) {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:modifiers
                           timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:window.windowNumber
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:unmodified
                           isARepeat:NO
                             keyCode:keyCode];
}

// A key and its modifiers typed into `view`, the way the window hands it
// to its first responder -- not through the application, whose menus would
// take the same key a second time.
static void SendViewKey(NSView* view, NSString* characters,
                        NSString* unmodified, unsigned short keyCode,
                        NSEventModifierFlags modifiers) {
    NSWindow* window = view.window;
    [window makeFirstResponder:view];
    [window sendEvent:UiSmokeKeyEvent(window, characters, unmodified, keyCode,
                                      modifiers)];
}

// The same through the application, to whichever window has the keys: how
// Return and Escape reach a sheet's default and cancel buttons. Posted and
// left for the caller's pump to dequeue, as a typed key is: a text field
// ends its editing on Return by AppKit's current event, which -sendEvent:
// alone never sets -- a Return sent that way reached the field and never
// the default button (PRD26, the check that failed from 4 October 2026).
static void PostApplicationKey(NSWindow* window, NSString* characters,
                               unsigned short keyCode, CGEventFlags flags = 0) {
    // Built as the keyboard builds one, then handed to AppKit: the text
    // system reads a key's own record, which -keyEventWithType: has none of.
    // Its character is said too: a key code is a place on the keyboard, and
    // on a French one 43 is not the comma.
    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, keyCode, true);
    if (down) {
        CGEventSetFlags(down, flags);
        UniChar typed[4] = {};
        const NSUInteger length = std::min<NSUInteger>(characters.length, 4);
        [characters getCharacters:typed range:NSMakeRange(0, length)];
        CGEventKeyboardSetUnicodeString(down, length, typed);
    }
    NSEvent* event = down ? [NSEvent eventWithCGEvent:down] : nil;
    if (down) CFRelease(down);
    if (event == nil)
        event = UiSmokeKeyEvent(window, characters, characters, keyCode, 0);
    [NSApp postEvent:event atStart:NO];
    // And released, as a key is: the timeline plays on the space bar's
    // release, its press being the Hand tool's while it is held.
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, keyCode, false);
    if (up) {
        CGEventSetFlags(up, flags);
        UniChar typed[4] = {};
        const NSUInteger length = std::min<NSUInteger>(characters.length, 4);
        [characters getCharacters:typed range:NSMakeRange(0, length)];
        CGEventKeyboardSetUnicodeString(up, length, typed);
        if (NSEvent* released = [NSEvent eventWithCGEvent:up])
            [NSApp postEvent:released atStart:NO];
        CFRelease(up);
    }
}

// Whether an object under `value` -- itself included -- holds the string
// `text` under `key`: a tool's answer read as JSON rather than searched as
// text, which escapes what it likes.
static bool UiSmokeJsonHolds(const mcp_json::Value& value,
                             const std::string& key, const std::string& text) {
    if (value.IsObject()) {
        if (const mcp_json::Value* found = value.Find(key);
            found && found->IsString() && found->AsString() == text)
            return true;
        for (const auto& [name, member] : value.AsObject())
            if (UiSmokeJsonHolds(member, key, text)) return true;
    } else if (value.IsArray()) {
        for (const mcp_json::Value& item : value.AsArray())
            if (UiSmokeJsonHolds(item, key, text)) return true;
    }
    return false;
}

// One mouse event to the timeline's Metal view, `point` in its coordinates.
static void SendTimelineMouse(NSView* view, NSEventType type, NSPoint point,
                              NSEventModifierFlags modifiers,
                              NSInteger clicks = 1) {
    NSWindow* window = view.window;
    NSEvent* event =
        [NSEvent mouseEventWithType:type
                           location:[view convertPoint:point toView:nil]
                      modifierFlags:modifiers
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:window.windowNumber
                            context:nil
                        eventNumber:0
                         clickCount:clicks
                           pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
    TimelineMetalView* timeline = (TimelineMetalView*)view;
    switch (type) {
        case NSEventTypeLeftMouseDown:
            [timeline mouseDown:event];
            break;
        case NSEventTypeLeftMouseDragged:
            [timeline mouseDragged:event];
            break;
        case NSEventTypeLeftMouseUp:
            [timeline mouseUp:event];
            break;
        case NSEventTypeMouseMoved:
            [timeline mouseMoved:event];
            break;
        default:
            break;
    }
}

// Whether `data` holds a rectangle at (x, y, width, height) in `color`,
// to a hundredth of a point and of a channel.
static bool UiSmokeDrawsRect(const TimelineRenderData& data, double x, double y,
                             double width, double height,
                             const ui::theme::Color& color) {
    return std::any_of(data.overlays.begin(), data.overlays.end(),
                       [&](const MetalDrawCommand& command) {
                           const MetalRect* rect =
                               std::get_if<MetalRect>(&command);
                           return rect && std::abs(rect->x - x) < 0.01 &&
                                  std::abs(rect->y - y) < 0.01 &&
                                  std::abs(rect->width - width) < 0.01 &&
                                  std::abs(rect->height - height) < 0.01 &&
                                  std::abs(rect->red - color.r) < 0.01 &&
                                  std::abs(rect->green - color.g) < 0.01 &&
                                  std::abs(rect->blue - color.b) < 0.01;
                       });
}

// The text command that writes `text` in `data`, if one does.
static const MetalText* UiSmokeDrawnText(const TimelineRenderData& data,
                                         const std::string& text) {
    for (const MetalDrawCommand& command : data.overlays)
        if (const MetalText* drawn = std::get_if<MetalText>(&command);
            drawn && drawn->text == text)
            return drawn;
    return nullptr;
}

// A colour as the document spells one (#rrggbb), as the theme holds one.
static ui::theme::Color UiSmokeHexColor(const std::string& hex) {
    TransitionColor parsed;
    std::string error;
    if (!ParseTransitionColor(hex, parsed, error)) return {0, 0, 0, 0};
    return {static_cast<float>(parsed.red) / 255.0f,
            static_cast<float>(parsed.green) / 255.0f,
            static_cast<float>(parsed.blue) / 255.0f, 1.0f};
}

// The text of `field` replaced as typing replaces it: through its field
// editor while it is edited, which a sheet's check reads, else its value.
static void UiSmokeSetFieldText(NSTextField* field, NSString* text) {
    if (NSText* editor = field.currentEditor) {
        editor.string = text;
        editor.selectedRange = NSMakeRange(text.length, 0);
        [(NSTextView*)editor didChangeText];
        [field validateEditing];
    } else {
        field.stringValue = text;
    }
}

// A notch of a mouse wheel to the timeline's Metal view at `point`, in its
// coordinates: `lines` signed as the wheel turns, held keys in `flags`. A
// wheel's event has no window here, and carries its place in screen
// coordinates, which the timeline reads as the window's: it is put where
// both agree.
static void SendTimelineWheel(NSView* view, NSPoint point, int32_t lines,
                              CGEventFlags flags = 0) {
    const NSPoint inWindow = [view convertPoint:point toView:nil];
    CGEventRef wheel = CGEventCreateScrollWheelEvent(
        nullptr, kCGScrollEventUnitLine, 1, lines);
    if (wheel == nullptr) return;
    const CGFloat screenHeight = NSScreen.screens.firstObject.frame.size.height;
    CGEventSetLocation(wheel,
                       CGPointMake(inWindow.x, screenHeight - inWindow.y));
    CGEventSetFlags(wheel, flags);
    NSEvent* event = [NSEvent eventWithCGEvent:wheel];
    CFRelease(wheel);
    if (event) [view scrollWheel:event];
}

// The texts an AppKit view shows, its own and its subviews', in order.
static std::string UiSmokeViewTexts(NSView* view) {
    std::string texts;
    if ([view isKindOfClass:NSTextField.class])
        texts +=
            std::string(((NSTextField*)view).stringValue.UTF8String ?: "") +
            "\n";
    for (NSView* child in view.subviews) texts += UiSmokeViewTexts(child);
    return texts;
}

// The button titled `title` under `view`, if one is.
static NSButton* UiSmokeButtonIn(NSView* view, NSString* title) {
    if ([view isKindOfClass:NSButton.class] &&
        [((NSButton*)view).title isEqualToString:title])
        return (NSButton*)view;
    for (NSView* child in view.subviews)
        if (NSButton* found = UiSmokeButtonIn(child, title)) return found;
    return nil;
}

// The save panel a flow put on `window` answered with `path`, as a person
// types a name in a folder and presses « Enregistrer ».
static bool UiSmokeAnswerSavePanel(NSWindow* window,
                                   const std::filesystem::path& path) {
    UiSmokeWaitUntil(
        [&] { return [window.attachedSheet isKindOfClass:NSSavePanel.class]; },
        5.0);
    NSSavePanel* save = [window.attachedSheet isKindOfClass:NSSavePanel.class]
                            ? (NSSavePanel*)window.attachedSheet
                            : nil;
    if (save == nil) return false;
    // AppKit's save panel runs out of process: -ok: is not implemented on
    // this side, and what is typed into it from here does not reach it. The
    // sheet is ended with OK, as its button ends it, and the window reads
    // the test's destination instead of the panel's (CMSaveSheetAnswer).
    gUiSmokeSaveAnswer = path;
    UiSmokeWaitUntil([] { return false; }, 0.3);
    [window endSheet:save returnCode:NSModalResponseOK];
    const bool ended =
        UiSmokeWaitUntil([&] { return window.attachedSheet != save; }, 5.0);
    const bool taken = gUiSmokeSaveAnswer.empty();
    gUiSmokeSaveAnswer.clear();
    return ended && taken;
}

@implementation AppDelegate (UiSmoke)

// Moved from main.mm, where it was compiled for this test alone.
- (NSRect)programScopePanelFrame {
    const NSRect bounds = self.programMonitorView.bounds;
    const double scale = self.programMonitorView.layer.contentsScale > 0.0
                             ? self.programMonitorView.layer.contentsScale
                             : 1.0;
    const double height = bounds.size.height * scale;
    const VideoScopePanel panel = LayoutVideoScopePanel(
        bounds.size.width * scale, height, [self programScopeBottomInset]);
    return NSMakeRect(panel.x / scale,
                      (height - (panel.y + panel.height)) / scale,
                      panel.width / scale, panel.height / scale);
}

- (void)spinRunLoopFor:(NSTimeInterval)seconds {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (deadline.timeIntervalSinceNow > 0) {
        NSEvent* event = [NSApp
            nextEventMatchingMask:NSEventMaskAny
                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                           inMode:NSDefaultRunLoopMode
                          dequeue:YES];
        if (event) [NSApp sendEvent:event];
    }
}

// PERF10 -- see gUiPlayProbeProject. The sound is left out: the probe is
// about decoding, and it runs unattended.
- (void)runPlayProbe {
    const auto footprintMb = [] {
        task_vm_info_data_t info{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count);
        return static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
    };
    const auto threadCount = [] {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
            return 0;
        for (mach_msg_type_number_t index = 0; index < count; ++index)
            mach_port_deallocate(mach_task_self(), threads[index]);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                      count * sizeof(thread_act_t));
        return static_cast<int>(count);
    };
    AppState* state = self.state;
    // PERF12 -- before the wait at rest, not after it: the sound decodes
    // every packet of the timeline's rushes, pictures included, and left
    // running while the decoders open ahead it saturated the drive and
    // warmed the page cache for the rushes playback then read.
    state->audioPlayback.reset();
    for (int wait = 0; wait < 500 && (state->checkingSourceAvailability ||
                                      !state->openingDecoders.empty());
         ++wait)
        [self spinRunLoopFor:0.02];
    if (gUiPlayProbePerf7Seconds > 0.0) {
        [self runPlayProbePerf7Phases];
        return;
    }
    const double openedMb = footprintMb();
    const int openedThreads = threadCount();
    const size_t openedWorkers = state->workers.size();
    const int opensAtRest = gUiPlayProbeOpens;
    const RationalTime end = state->timeline->Duration();
    [self requestResolvedPosition:RationalTime{0, 1}];
    [self setPlaybackDirection:1];
    const uint64_t dropsBefore = state->performanceMetrics->GetSnapshot().drops;
    const int64_t advisedBefore = MediaSource::AdvisedBytes();
    const MediaSource::ReadCounters readBefore = MediaSource::Counters();
    const playback_reader::Reader::Stats readerBefore =
        state->playbackReader.GetStats();
    const int ticksBefore = gUiPlayProbeTicks;
    const int topDropsBefore = gUiPlayProbeTopDrops;
    const auto started = std::chrono::steady_clock::now();
    double peakMb = openedMb;
    int peakThreads = openedThreads;
    size_t peakWorkers = openedWorkers;
    double seconds = 0.0;
    int reported = -1;
    // PERF12 -- how long a layer on screen had no decoder at all: its rush
    // still opening, which is the stop at a cut. A late frame from an open
    // decoder is a dropped tick, not this.
    double waitingSeconds = 0.0;
    // PERF13 -- how long a layer was hidden under an opaque shot above, so
    // neither composited nor decoded.
    double hiddenSeconds = 0.0;
    auto sampled = started;
    while (state->requestedPosition < end && seconds < gUiPlayProbeSeconds) {
        [self spinRunLoopFor:0.1];
        const double nowMb = footprintMb();
        peakMb = std::max(peakMb, nowMb);
        peakThreads = std::max(peakThreads, threadCount());
        peakWorkers = std::max(peakWorkers, state->workers.size());
        const auto now = std::chrono::steady_clock::now();
        const bool waiting = std::any_of(
            state->requested.begin(), state->requested.end(),
            [&](const ResolvedSlot& slot) {
                return slot.active &&
                       state->document.FindSource(slot.sourceId) != nullptr &&
                       state->workers.count(slot.sourceId) == 0 &&
                       state->offlineSourceIds.count(slot.sourceId) == 0;
            });
        if (waiting)
            waitingSeconds +=
                std::chrono::duration<double>(now - sampled).count();
        if (!state->hiddenSlots.empty())
            hiddenSeconds +=
                std::chrono::duration<double>(now - sampled).count();
        sampled = now;
        seconds = std::chrono::duration<double>(now - started).count();
        if (static_cast<int>(seconds) != reported) {
            reported = static_cast<int>(seconds);
            std::fprintf(stderr,
                         "probe t=%.1f s position=%.2f s footprint=%.0f MB "
                         "decoders=%zu opening=%zu closing=%zu cache=%.0f MB "
                         "waiting=%.1f s top_drops=%d hidden=%zu\n",
                         seconds,
                         static_cast<double>(state->requestedPosition.value) /
                             static_cast<double>(state->requestedPosition.rate),
                         nowMb, state->workers.size(),
                         state->openingDecoders.size(),
                         state->closingDecoders.size(),
                         static_cast<double>(state->frameCache->TotalBytes()) /
                             (1024.0 * 1024.0),
                         waitingSeconds, gUiPlayProbeTopDrops - topDropsBefore,
                         state->hiddenSlots.size());
        }
    }
    [self setPlaybackDirection:0];
    const MediaSource::ReadCounters readAfter = MediaSource::Counters();
    const playback_reader::Reader::Stats readerAfter =
        state->playbackReader.GetStats();
    const PerformanceMetrics::Snapshot metrics =
        state->performanceMetrics->GetSnapshot();
    std::printf(
        "{\"sources\":%zu,\"opened_mb\":%.0f,\"opened_threads\":%d,"
        "\"opened_decoders\":%zu,\"played_s\":%.1f,\"timeline_s\":%.1f,"
        "\"peak_mb\":%.0f,\"peak_threads\":%d,\"peak_decoders\":%zu,"
        "\"decoder_capacity\":%zu,\"frame_cache_mb\":%.0f,"
        "\"dropped_ticks\":%llu,\"miss_p95_ms\":%.1f,"
        "\"opens_at_rest\":%d,\"opens_playing\":%d,\"reopens\":%d,"
        "\"waiting_s\":%.1f,\"ticks\":%d,\"top_dropped_ticks\":%d,"
        "\"hidden_s\":%.1f,\"read_ahead\":%d,\"advised_mb\":%.0f,"
        "\"demuxed_mib\":%.0f,\"video_packets_mib\":%.0f,"
        "\"other_packets_mib\":%.0f,\"open_mib\":%.0f,\"seeks\":%lld,"
        "\"decoded_frames\":%lld,\"reader\":%d,\"reader_mib\":%.0f,"
        "\"reader_reads\":%lld,\"reader_plans\":%lld,"
        "\"reader_unplaced_sources\":%lld}\n",
        state->document.sources.size(), openedMb, openedThreads, openedWorkers,
        seconds, static_cast<double>(end.value) / static_cast<double>(end.rate),
        peakMb, peakThreads, peakWorkers, state->decoderCapacity,
        static_cast<double>(state->frameCache->TotalBytes()) /
            (1024.0 * 1024.0),
        static_cast<unsigned long long>(metrics.drops - dropsBefore),
        static_cast<double>(metrics.missP95Us) / 1000.0, opensAtRest,
        gUiPlayProbeOpens - opensAtRest, gUiPlayProbeReopens, waitingSeconds,
        gUiPlayProbeTicks - ticksBefore, gUiPlayProbeTopDrops - topDropsBefore,
        hiddenSeconds, gUiPlayProbeReadAhead ? 1 : 0,
        static_cast<double>(MediaSource::AdvisedBytes() - advisedBefore) / 1e6,
        static_cast<double>(readAfter.demuxed_bytes -
                            readBefore.demuxed_bytes) /
            1048576.0,
        static_cast<double>(readAfter.video_packet_bytes -
                            readBefore.video_packet_bytes) /
            1048576.0,
        static_cast<double>(readAfter.other_packet_bytes -
                            readBefore.other_packet_bytes) /
            1048576.0,
        static_cast<double>(readAfter.open_bytes) / 1048576.0,
        static_cast<long long>(readAfter.seeks - readBefore.seeks),
        static_cast<long long>(readAfter.decoded_frames -
                               readBefore.decoded_frames),
        gUiPlayProbeReader ? 1 : 0,
        static_cast<double>(readerAfter.bytes - readerBefore.bytes) / 1048576.0,
        static_cast<long long>(readerAfter.reads - readerBefore.reads),
        static_cast<long long>(readerAfter.plans - readerBefore.plans),
        static_cast<long long>(readerAfter.unplaced_sources));
    std::fflush(stdout);
    [NSApp terminate:nil];
}

// PERF7 -- docs/PERF_BENCHMARK.md. The protocol's five phases, played on a
// real project rather than by hand: at rest, playback, playback at double
// speed, a scrub dragged along the ruler, then the wheel's zoom and
// Shift-wheel's scroll. Each phase prints its start and end on the clock of
// the CUTMACHINE_RENDER_COST report (its start_s column), and whether the
// window stayed key and visible throughout: the reading is about a window
// in front, which a locked screen does not give.
- (void)runPlayProbePerf7Phases {
    const double phase = gUiPlayProbePerf7Seconds;
    const auto seconds = [] {
        return (render_cost::NowNanoseconds() - gRenderCostOriginNs) / 1e9;
    };
    bool key = true;
    bool visible = true;
    const auto spin = [&](double duration,
                          const std::function<void(double)>& step) {
        key = true;
        visible = true;
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                    .count();
            if (elapsed >= duration) break;
            if (step) step(elapsed / duration);
            [self spinRunLoopFor:1.0 / 60.0];
            key = key && self.window.isKeyWindow && NSApp.isActive;
            visible = visible && (self.window.occlusionState &
                                  NSWindowOcclusionStateVisible) != 0;
        }
    };
    std::string phases;
    const auto mark = [&](const char* name, double start) {
        const double end = seconds();
        std::fprintf(stderr,
                     "probe phase=%s start_s=%.3f end_s=%.3f key=%d "
                     "visible=%d\n",
                     name, start, end, key ? 1 : 0, visible ? 1 : 0);
        char text[192];
        std::snprintf(text, sizeof text,
                      "%s{\"phase\":\"%s\",\"start_s\":%.3f,\"end_s\":%.3f,"
                      "\"key\":%s,\"visible\":%s}",
                      phases.empty() ? "" : ",", name, start, end,
                      key ? "true" : "false", visible ? "true" : "false");
        phases += text;
    };

    [self requestResolvedPosition:RationalTime{0, 1}];
    [self spinRunLoopFor:1.0];
    double start = seconds();
    spin(phase, nullptr);
    mark("rest", start);

    // Playback from the start, back to it when the timeline ends: a short
    // film played at double speed ends within seconds, and the rest of the
    // phase measured a timeline at rest (18 November 2026, 36 s).
    const RationalTime end = self.state->timeline->Duration();
    const double endSeconds =
        static_cast<double>(end.value) / static_cast<double>(end.rate);
    int loops = 0;
    const auto play = [&](int direction) {
        [self requestResolvedPosition:RationalTime{0, 1}];
        [self setPlaybackDirection:direction];
        spin(phase, [&](double) {
            const RationalTime at = self.state->requestedPosition;
            const double atSeconds =
                static_cast<double>(at.value) / static_cast<double>(at.rate);
            if (self.state->playbackDirection == 0 ||
                atSeconds >= endSeconds - 0.1) {
                [self requestResolvedPosition:RationalTime{0, 1}];
                [self setPlaybackDirection:direction];
                ++loops;
            }
        });
    };
    start = seconds();
    play(1);
    mark("play", start);

    // L pressed twice.
    start = seconds();
    play(2);
    mark("play_2x", start);
    [self setPlaybackDirection:0];

    // Two round trips along the ruler, the button held, one drag event a
    // tick, as a hand scrubbing does.
    const double width = self.metalView.bounds.size.width;
    const double height = self.metalView.bounds.size.height;
    const double header = self.state->viewport.header_width;
    const double rulerY = height - (kTimelineToolbarHeight + 8.0);
    const auto along = [&](double t) {
        const double back = std::fabs(std::fmod(t * 4.0, 2.0) - 1.0);
        return NSMakePoint(
            header + 12.0 + (1.0 - back) * (width - header - 24.0), rulerY);
    };
    int scrubPositions = 0;
    RationalTime lastPosition = self.state->requestedPosition;
    start = seconds();
    SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDown, along(0.0), 0);
    spin(phase, [&](double t) {
        SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDragged, along(t),
                          0);
        if (self.state->requestedPosition != lastPosition) {
            ++scrubPositions;
            lastPosition = self.state->requestedPosition;
        }
    });
    SendTimelineMouse(self.metalView, NSEventTypeLeftMouseUp, along(1.0), 0);
    mark("scrub", start);

    // The first half zooms, fifteen notches one way then fifteen back; the
    // second zooms in ten notches once, then scrolls thirty notches right
    // and thirty left with Shift. A notch every other tick.
    [self fitTimelineToViewportWidth];
    const NSPoint over = NSMakePoint(
        header + (width - header) / 2.0,
        height - (kTimelineRulerHeight + kTimelineToolbarHeight + 10.0));
    double scaleMin = self.state->viewport.pixels_per_second;
    double scaleMax = scaleMin;
    const auto viewStart = [&] {
        const RationalTime at = self.state->viewport.view_start;
        return static_cast<double>(at.value) / static_cast<double>(at.rate);
    };
    double scrollMin = viewStart();
    double scrollMax = scrollMin;
    int notches = 0;
    bool zoomedForScroll = false;
    start = seconds();
    spin(phase, [&](double t) {
        if (++notches % 2 != 0) return;
        const int index = notches / 2;
        if (t < 0.5) {
            SendTimelineWheel(self.metalView, over, index % 30 < 15 ? 1 : -1);
        } else {
            if (!zoomedForScroll) {
                for (int notch = 0; notch < 10; ++notch)
                    SendTimelineWheel(self.metalView, over, 1);
                zoomedForScroll = true;
            }
            SendTimelineWheel(self.metalView, over, index % 60 < 30 ? 1 : -1,
                              kCGEventFlagMaskShift);
        }
        self.state->overlayDirty = true;
        scaleMin = std::min(scaleMin, self.state->viewport.pixels_per_second);
        scaleMax = std::max(scaleMax, self.state->viewport.pixels_per_second);
        scrollMin = std::min(scrollMin, viewStart());
        scrollMax = std::max(scrollMax, viewStart());
    });
    mark("zoom_scroll", start);

    std::printf(
        "{\"perf7_phase_s\":%.1f,\"timeline_s\":%.2f,\"play_loops\":%d,"
        "\"phases\":[%s],\"scrub_positions\":%d,"
        "\"zoom_pps_min\":%.2f,\"zoom_pps_max\":%.2f,\"view_start_min_s\":%.2f,"
        "\"view_start_max_s\":%.2f}\n",
        phase, endSeconds, loops, phases.c_str(), scrubPositions, scaleMin,
        scaleMax, scrollMin, scrollMax);
    std::fflush(stdout);
    [NSApp terminate:nil];
}

// VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. Pixels decide what they
// can: each state has to match its own export, and a control has to show
// that the same measure tells two different states apart before it is
// trusted to call two pictures equal. Measured on 23 September 2026: equal
// states 0.006 to 0.013, controls 0.26 to 0.40 on the first scenarios; each
// bound keeps a clear factor from what it separates.
constexpr double kUiQaSame = 0.03;
constexpr double kUiQaDifferent = 0.10;

std::string UiQaDescribe(const std::string& what, double distance) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, " (distance %.4f)", distance);
    return "VQA " + what + buffer;
}

void UiQaExpectSame(const std::string& what, const std::vector<float>& left,
                    const std::vector<float>& right,
                    const std::vector<bool>& ignored) {
    const double distance = UiQaDistance(left, right, ignored);
    UiSmokeCheck(distance <= kUiQaSame, UiQaDescribe(what, distance).c_str());
}

void UiQaExpectDifferent(const std::string& what,
                         const std::vector<float>& left,
                         const std::vector<float>& right,
                         const std::vector<bool>& ignored) {
    const double distance = UiQaDistance(left, right, ignored);
    UiSmokeCheck(distance >= kUiQaDifferent,
                 UiQaDescribe("control: " + what, distance).c_str());
}

// Two neighbouring frames of the pattern, on the grid three times finer
// (gUiQaGridDensity): about 0.08 apart, where a repeated frame is within
// 0.01. R3 has to tell them apart, and the 0.10 of kUiQaDifferent is a
// change of framing, not of one frame.
constexpr double kUiQaNeighbour = 0.06;

void UiQaExpectNeighbour(const std::string& what,
                         const std::vector<float>& left,
                         const std::vector<float>& right,
                         const std::vector<bool>& ignored) {
    const double distance = UiQaDistance(left, right, ignored);
    UiSmokeCheck(distance >= kUiQaNeighbour,
                 UiQaDescribe("control: " + what, distance).c_str());
}

void UiQaExpectValue(const std::string& what, double value, double low,
                     double high) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, " (%.4f, expected %.4f to %.4f)",
                  value, low, high);
    UiSmokeCheck(value >= low && value <= high,
                 ("VQA " + what + buffer).c_str());
}

// VQA2 -- what one sequence frame shows: the clip, and the source frame.
struct UiQaShown {
    Ulid clip;
    int64_t source = -1;
    bool operator==(const UiQaShown& other) const {
        return clip == other.clip && source == other.source;
    }
    bool operator!=(const UiQaShown& other) const { return !(*this == other); }
};

// The cadence timeline frame by frame, from ClipSpeed.h's rule worked out
// by hand -- source_in + floor(k * p / q), a 25 i/s source in a 25 i/s
// sequence -- rather than by calling it: the monitor calls that function,
// and a check that calls it too proves nothing.
std::vector<UiQaShown> UiQaCadenceFrames() {
    std::vector<UiQaShown> frames;
    for (int64_t k = 0; k < 40; ++k)
        frames.push_back({gUiSmokeCadenceHalfClipId, k / 2});
    for (int64_t k = 0; k < 25; ++k)
        frames.push_back({gUiSmokeCadenceDoubleClipId, 50 + (2 * k)});
    for (int64_t k = 0; k < 40; ++k)
        frames.push_back({gUiSmokeCadenceTwoFifthsClipId, 10 + (2 * k / 5)});
    return frames;
}

// One image Record put on screen while playing, and the sequence frame the
// playhead stood on when it was drawn.
struct UiQaPresented {
    double at_ms = 0.0;
    UiQaShown shown;
    int64_t position = -1;
};

// VQA2 -- a playback trace against the frames the sequence holds. Images
// that repeat on consecutive frames are one run (two frames long at 1/2 on
// a 25 i/s rush): the trace must draw every run, in play order, each for
// about as long as its frames last. The trace itself is kept as `tsv`.
void UiQaCheckPlayback(const std::string& label,
                       std::vector<UiQaPresented> trace,
                       const std::vector<UiQaShown>& expected, int direction,
                       const std::filesystem::path& tsv, int64_t firstFrame = 0,
                       double frameMs = 40.0) {
    // Past the last frame the playhead shows nothing: that is the end of
    // the timeline, not a hole.
    while (!trace.empty() && trace.back().shown.clip.empty()) trace.pop_back();
    struct Run {
        UiQaShown shown;
        int64_t frames = 0;
    };
    std::vector<Run> runs;
    for (size_t step = 0; step < expected.size(); ++step) {
        const UiQaShown& shown =
            expected[direction > 0 ? step : expected.size() - 1 - step];
        if (!runs.empty() && runs.back().shown == shown)
            ++runs.back().frames;
        else
            runs.push_back({shown, 1});
    }
    constexpr size_t kNone = static_cast<size_t>(-1);
    std::vector<size_t> matched(trace.size(), kNone);
    size_t next = 0;
    size_t skipped = 0;
    size_t unknown = 0;
    size_t backwards = 0;
    size_t lastMatched = kNone;
    for (size_t index = 0; index < trace.size(); ++index) {
        size_t run = next;
        while (run < runs.size() && runs[run].shown != trace[index].shown)
            ++run;
        if (run == runs.size()) {
            bool earlier = false;
            for (size_t before = 0; before < next && !earlier; ++before)
                earlier = runs[before].shown == trace[index].shown;
            ++(earlier ? backwards : unknown);
            continue;
        }
        if (lastMatched != kNone) skipped += run - lastMatched - 1;
        matched[index] = run;
        lastMatched = run;
        next = run + 1;
    }
    const bool whole = !trace.empty() && matched.front() == 0 &&
                       lastMatched == runs.size() - 1;
    // How long each image stayed, against how long its frames last. The
    // last one has no end: playback stops on it.
    size_t late = 0;
    double longest = 0.0;
    int64_t spanFrames = 0;
    size_t mismatched = 0;
    std::ofstream out(tsv);
    out << "at_ms\tclip\tsource\tposition\trun\tframes\theld_ms\n";
    for (size_t index = 0; index < trace.size(); ++index) {
        const UiQaPresented& entry = trace[index];
        const bool known = matched[index] != kNone;
        double held = -1.0;
        if (index + 1 < trace.size()) {
            held = trace[index + 1].at_ms - entry.at_ms;
            if (known) {
                const double overstay =
                    held - (runs[matched[index]].frames * frameMs);
                longest = std::max(longest, overstay);
                if (overstay > 45.0) ++late;
                spanFrames += runs[matched[index]].frames;
            }
        }
        const int64_t offset = entry.position - firstFrame;
        if (offset >= 0 && offset < static_cast<int64_t>(expected.size()) &&
            expected[offset] != entry.shown)
            ++mismatched;
        out << entry.at_ms << '\t' << entry.shown.clip.substr(20) << '\t'
            << entry.shown.source << '\t' << entry.position << '\t'
            << (known ? std::to_string(matched[index]) : "-") << '\t'
            << (known ? runs[matched[index]].frames : 0) << '\t' << held
            << '\n';
    }
    UiSmokeCheck(
        whole && skipped == 0 && unknown == 0 && backwards == 0,
        ("VQA " + label + ": " + std::to_string(trace.size()) +
         " images drawn for " + std::to_string(runs.size()) +
         " expected, in play order from the first to the last (" +
         std::to_string(skipped) + " skipped, " + std::to_string(backwards) +
         " stepping back, " + std::to_string(unknown) + " foreign)")
            .c_str());
    UiSmokeCheck(
        late == 0,
        ("VQA " + label + ": no image stays more than 45 ms past " +
         "its frames (" + std::to_string(late) + " late, longest overstay " +
         std::to_string(std::lround(longest)) + " ms)")
            .c_str());
    UiSmokeCheck(mismatched == 0,
                 ("VQA " + label +
                  ": each image drawn is the one its playhead frame shows (" +
                  std::to_string(mismatched) + " drawn on another frame)")
                     .c_str());
    if (trace.size() > 1 && spanFrames > 0)
        UiQaExpectValue(
            label + ": plays at the sequence's speed",
            (trace.back().at_ms - trace.front().at_ms) / (spanFrames * frameMs),
            0.95, 1.05);
}

// The black along each edge of a PNG, read from its code values: lines
// (rows or columns) in which no channel of any pixel reaches 24. Left,
// top, right, bottom.
bool UiQaBlackEdges(const std::filesystem::path& png, int64_t edges[4]) {
    NSData* data = [NSData
        dataWithContentsOfFile:[NSString stringWithUTF8String:png.c_str()]];
    NSBitmapImageRep* bitmap =
        data != nil ? [NSBitmapImageRep imageRepWithData:data] : nil;
    CGImageRef image = bitmap.CGImage;
    if (image == nullptr) return false;
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef retagged = CGImageCreateCopyWithColorSpace(image, srgb);
    std::vector<uint8_t> pixels(width * height * 4);
    CGContextRef context = CGBitmapContextCreate(
        pixels.data(), width, height, 8, width * 4, srgb,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(srgb);
    if (context == nullptr || retagged == nullptr) {
        if (context != nullptr) CGContextRelease(context);
        if (retagged != nullptr) CGImageRelease(retagged);
        return false;
    }
    CGContextDrawImage(context, CGRectMake(0.0, 0.0, width, height), retagged);
    CGContextRelease(context);
    CGImageRelease(retagged);
    std::vector<uint8_t> rows(height, 0);
    std::vector<uint8_t> columns(width, 0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const uint8_t* pixel = pixels.data() + ((y * width) + x) * 4;
            const uint8_t brightest = std::max({pixel[0], pixel[1], pixel[2]});
            rows[y] = std::max(rows[y], brightest);
            columns[x] = std::max(columns[x], brightest);
        }
    }
    const auto count = [](const std::vector<uint8_t>& lines, bool reverse) {
        int64_t black = 0;
        for (size_t index = 0; index < lines.size(); ++index) {
            if (lines[reverse ? lines.size() - 1 - index : index] >= 24) break;
            ++black;
        }
        return black;
    };
    edges[0] = count(columns, false);
    edges[1] = count(rows, false);
    edges[2] = count(columns, true);
    edges[3] = count(rows, true);
    return true;
}

// VQA2 -- a carton in one picture of the sequence frame, as fractions of
// that frame: where the white fill of its text is, its red shadow, its blue
// box and its black outline, and how many lines of text there are.
struct UiQaCartonPart {
    double left = 1.0;
    double top = 1.0;
    double right = 0.0;
    double bottom = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    size_t pixels = 0;
    void Add(double x, double y) {
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
        sum_x += x;
        sum_y += y;
        ++pixels;
    }
    double CentreX() const { return pixels ? sum_x / pixels : 0.0; }
    double CentreY() const { return pixels ? sum_y / pixels : 0.0; }
};

struct UiQaCarton {
    UiQaCartonPart fill;
    UiQaCartonPart shadow;
    UiQaCartonPart box;
    UiQaCartonPart outline;
    // Share of the frame the white fill covers: how heavy the letters are.
    double fill_share = 0.0;
    int lines = 0;
};

// `crop` in the PNG's pixels, origin top-left; a null rect keeps it all.
// Read in the colour space the file declares, into sRGB. `size`, when not
// zero, is the size in pixels the crop is resampled to before it is read:
// how heavy letters look depends on how many pixels draw them.
bool UiQaMeasureCarton(const std::filesystem::path& png, CGRect crop,
                       UiQaCarton& carton, CGSize size = CGSizeZero) {
    NSData* data = [NSData
        dataWithContentsOfFile:[NSString stringWithUTF8String:png.c_str()]];
    NSBitmapImageRep* bitmap =
        data != nil ? [NSBitmapImageRep imageRepWithData:data] : nil;
    CGImageRef image = bitmap.CGImage;
    if (image == nullptr) return false;
    if (CGRectIsNull(crop))
        crop = CGRectMake(0.0, 0.0, CGImageGetWidth(image),
                          CGImageGetHeight(image));
    CGImageRef cropped = CGImageCreateWithImageInRect(image, crop);
    if (cropped == nullptr) return false;
    const size_t width = size.width > 0.0
                             ? static_cast<size_t>(std::lround(size.width))
                             : CGImageGetWidth(cropped);
    const size_t height = size.height > 0.0
                              ? static_cast<size_t>(std::lround(size.height))
                              : CGImageGetHeight(cropped);
    std::vector<uint8_t> pixels(width * height * 4);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        pixels.data(), width, height, 8, width * 4, srgb,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(srgb);
    if (context == nullptr) {
        CGImageRelease(cropped);
        return false;
    }
    CGContextDrawImage(context, CGRectMake(0.0, 0.0, width, height), cropped);
    CGContextRelease(context);
    CGImageRelease(cropped);
    carton = {};
    std::vector<size_t> fillRows(height, 0);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const uint8_t* pixel = pixels.data() + ((y * width) + x) * 4;
            const int r = pixel[0];
            const int g = pixel[1];
            const int b = pixel[2];
            const double fx = (x + 0.5) / width;
            const double fy = (y + 0.5) / height;
            if (std::min({r, g, b}) > 200) {
                carton.fill.Add(fx, fy);
                ++fillRows[y];
            } else if (r > 150 && g < 90 && b < 90) {
                carton.shadow.Add(fx, fy);
            } else if (b > r + 40 && b > g + 40 && b > 70) {
                carton.box.Add(fx, fy);
            } else if (std::max({r, g, b}) < 45) {
                carton.outline.Add(fx, fy);
            }
        }
    }
    carton.fill_share =
        static_cast<double>(carton.fill.pixels) / (width * height);
    // Lines: bands of rows holding fill, merged across gaps shorter than
    // 1 % of the frame (the dot of an i, the space between accents), and
    // counted when taller than 2 % of it.
    const size_t mergeGap = std::max<size_t>(1, height / 100);
    const size_t minimumBand = std::max<size_t>(1, height / 50);
    size_t bandStart = 0;
    size_t lastFilled = 0;
    bool inBand = false;
    for (size_t y = 0; y <= height; ++y) {
        const bool filled = y < height && fillRows[y] > 0;
        if (filled) {
            if (!inBand) {
                inBand = true;
                bandStart = y;
            }
            lastFilled = y;
        } else if (inBand && (y == height || y - lastFilled > mergeGap)) {
            inBand = false;
            if (lastFilled + 1 - bandStart >= minimumBand) ++carton.lines;
        }
    }
    return true;
}

// One row of a PNG's pixels, as sRGB, in the colour space it declares.
bool UiQaPngRow(const std::filesystem::path& png, int64_t y,
                std::vector<std::array<uint8_t, 3>>& row) {
    NSData* data = [NSData
        dataWithContentsOfFile:[NSString stringWithUTF8String:png.c_str()]];
    NSBitmapImageRep* bitmap =
        data != nil ? [NSBitmapImageRep imageRepWithData:data] : nil;
    CGImageRef image = bitmap.CGImage;
    if (image == nullptr || y < 0 ||
        y >= static_cast<int64_t>(CGImageGetHeight(image)))
        return false;
    CGImageRef line = CGImageCreateWithImageInRect(
        image, CGRectMake(0.0, y, CGImageGetWidth(image), 1.0));
    if (line == nullptr) return false;
    const size_t width = CGImageGetWidth(line);
    std::vector<uint8_t> pixels(width * 4);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        pixels.data(), width, 1, 8, width * 4, srgb,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(srgb);
    if (context == nullptr) {
        CGImageRelease(line);
        return false;
    }
    CGContextDrawImage(context, CGRectMake(0.0, 0.0, width, 1.0), line);
    CGContextRelease(context);
    CGImageRelease(line);
    row.resize(width);
    for (size_t x = 0; x < width; ++x)
        row[x] = {pixels[x * 4], pixels[(x * 4) + 1], pixels[(x * 4) + 2]};
    return true;
}

// Every frame of `movie` as a grid of its code values, through one FFmpeg
// run into `directory`.
bool UiQaFileFrames(const std::filesystem::path& movie,
                    const std::filesystem::path& directory, UiQaGridShape shape,
                    std::vector<std::vector<float>>& grids) {
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    const std::string extract =
        UiQaQuote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -y -i " +
        UiQaQuote(movie.string()) + " -vf scale=384:-2 " +
        UiQaQuote((directory / "%04d.png").string());
    if (std::system(extract.c_str()) != 0) return false;
    grids.clear();
    for (int index = 1;; ++index) {
        char file[16];
        std::snprintf(file, sizeof file, "%04d.png", index);
        if (!std::filesystem::exists(directory / file)) break;
        std::vector<float> grid;
        if (!UiQaGrid(directory / file, CGRectNull, shape, grid,
                      UiQaReading::Raw))
            return false;
        grids.push_back(std::move(grid));
    }
    return !grids.empty();
}

// R6 -- a look as a grading LUT does it: a contrast around 0.4 and an offset
// per channel. The offset is what a grid of mean colours sees; a pure
// contrast around mid grey barely moves a mean.
bool UiQaWriteLookCube(const std::filesystem::path& path, const double gains[3],
                       const double offsets[3]) {
    constexpr int kSize = 17;
    std::ofstream cube(path);
    cube.imbue(std::locale::classic());
    cube << "TITLE \"CUTMACHINE QA look\"\nLUT_3D_SIZE " << kSize << '\n'
         << std::fixed << std::setprecision(6);
    for (int blue = 0; blue < kSize; ++blue) {
        for (int green = 0; green < kSize; ++green) {
            for (int red = 0; red < kSize; ++red) {
                const int index[3] = {red, green, blue};
                for (int channel = 0; channel < 3; ++channel) {
                    const double input = index[channel] / double(kSize - 1);
                    cube << std::clamp(0.4 + ((input - 0.4) * gains[channel]) +
                                           offsets[channel],
                                       0.0, 1.0)
                         << (channel == 2 ? '\n' : ' ');
                }
            }
        }
    }
    return static_cast<bool>(cube);
}

// Applies `operation` through the edit log, as a menu or the chat does.
- (bool)uiQaApply:(Operation)operation label:(const std::string&)label {
    EditError editError = EditError::None;
    std::string message;
    const BOOL applied =
        [self applyAndPersistTimelineOperation:std::move(operation)
                                         error:editError
                                       message:message];
    UiSmokeCheck(applied,
                 ("VQA " + label + (applied ? "" : ": " + message)).c_str());
    return applied;
}

- (bool)uiQaOpen:(const Ulid&)timelineId label:(const std::string&)label {
    const BOOL opened = [self
        activateTimelineIdentifier:[NSString
                                       stringWithUTF8String:timelineId
                                                                .c_str()]];
    UiSmokeCheck(opened, ("VQA " + label + ": the timeline opens").c_str());
    return opened;
}

// PRD37 -- ROADMAP.md. Each timeline keeps its own view: the one it was
// left with, and the whole timeline the first time it opens (QA of 3
// October 2026: a 9:16 timeline whose shots fit in its first eight seconds
// opened at 39 s, empty, in the view of the timeline left behind).
- (void)runUiSmokeTimelineViewScenario {
    const Ulid mainId = self.state->activeTimelineId;
    const auto open = [&](const Ulid& timelineId) {
        return [self
            activateTimelineIdentifier:[NSString
                                           stringWithUTF8String:timelineId
                                                                    .c_str()]];
    };
    const auto describe = [&] {
        char text[96];
        std::snprintf(
            text, sizeof text, " (%.2f points a second from %.3f s)",
            self.state->viewport.pixels_per_second,
            static_cast<double>(self.state->viewport.view_start.value) /
                static_cast<double>(self.state->viewport.view_start.rate));
        return std::string(text);
    };
    // The main timeline, zoomed and scrolled somewhere of its own.
    const TimelineViewport original = self.state->viewport;
    const TimelineFit originalFit = self.state->timelineFit;
    self.state->viewport.pixels_per_second = 777.0;
    self.state->viewport.view_start = {12, 25};
    const TimelineViewport mainView = self.state->viewport;
    if (!open(gUiSmokeFramingTimelineId)) {
        UiSmokeCheck(false, "PRD37: the framing timeline opens");
        return;
    }
    const double width = self.metalView.bounds.size.width;
    TimelineViewport fitted = self.state->viewport;
    fitted.FitDuration(self.state->duration, width);
    UiSmokeCheck(self.state->viewport.view_start.value == 0 &&
                     std::abs(self.state->viewport.pixels_per_second -
                              fitted.pixels_per_second) <=
                         1e-6 * fitted.pixels_per_second,
                 ("PRD37: a timeline opened for the first time shows all of "
                  "it" +
                  describe())
                     .c_str());
    // Moved there too, then back and forth.
    self.state->viewport.pixels_per_second = 333.0;
    self.state->viewport.view_start = {5, 25};
    open(mainId);
    UiSmokeCheck(
        self.state->viewport.view_start == mainView.view_start &&
            self.state->viewport.pixels_per_second ==
                mainView.pixels_per_second,
        ("PRD37: back on a timeline, its own view is back" + describe())
            .c_str());
    open(gUiSmokeFramingTimelineId);
    UiSmokeCheck(self.state->viewport.view_start == RationalTime{5, 25} &&
                     self.state->viewport.pixels_per_second == 333.0,
                 ("PRD37: and the other one finds the view it was left "
                  "with" +
                  describe())
                     .c_str());
    open(mainId);
    // The view the checks after this one were written against, and the
    // framing timeline opened later as if for the first time.
    self.state->viewport.view_start = original.view_start;
    self.state->viewport.pixels_per_second = original.pixels_per_second;
    self.state->timelineFit = originalFit;
    self.state->timelineViews.erase(gUiSmokeFramingTimelineId);
    self.state->overlayDirty = true;
}

// FC4 -- the Trim Monitor, with the A/B cut of the trim timeline selected
// as a roll: A reads the pattern 10..40, B 50..80, so Record shows source
// frames 39 and 50 of the one rush, side by side.
- (void)runUiSmokeTrimMonitorChecks {
    const auto& requested = self.state->requested;
    const auto& panes = self.state->trimMonitorPanes;
    UiSmokeCheck(
        requested.size() == 2 && panes.size() == 2 &&
            requested[0].clipId == gUiSmokeTrimClipIds[0] &&
            requested[0].frame == 39 &&
            requested[1].clipId == gUiSmokeTrimClipIds[1] &&
            requested[1].frame == 50 && panes[0].x == 0 &&
            panes[0].x + panes[0].width <= 960 && panes[1].x >= 960 &&
            panes[1].x + panes[1].width <= 1920,
        ("FC4: in trim mode Record asks A's last frame (39) and B's first "
         "(50), in the left and right halves (" +
         std::to_string(requested.size()) + " frames: " +
         (requested.size() == 2 ? std::to_string(requested[0].frame) + ", " +
                                      std::to_string(requested[1].frame)
                                : std::string()) +
         ")")
            .c_str());
    // Both frames come from one rush, whose decoder holds one target: both
    // must still reach the screen.
    const bool shown = UiSmokeWaitUntil(
        [&] {
            [self presentNearestFrameAtDeadline:NO];
            const auto& rendered = self.state->rendered;
            return rendered.size() == 2 && requested.size() == 2 &&
                   rendered[0].frame == requested[0].frame &&
                   rendered[1].frame == requested[1].frame;
        },
        8.0);
    UiSmokeCheck(shown,
                 "FC4: both frames of the one rush are decoded and shown");
    TimelineRenderData data;
    data.video_framings.resize(panes.size());
    data.video_opacities.resize(panes.size(), 0.5f);
    data.video_veils.resize(panes.size(), MonitorVeil{1.0f, 0.0f, 0.0f, 1.0f});
    [self layOutTrimMonitor:data];
    const MetalText* outgoing = UiSmokeDrawnText(data, "A · SORTIE");
    const MetalText* incoming = UiSmokeDrawnText(data, "B · ENTRÉE");
    const double middle = self.programMonitorView.bounds.size.width / 2.0;
    UiSmokeCheck(
        outgoing && incoming && outgoing->x < middle && incoming->x > middle &&
            UiSmokeDrawnText(data, "poignée : 65 images") != nullptr &&
            UiSmokeDrawnText(data, "poignée : 50 images") != nullptr &&
            data.video_framings[0].width == panes[0].width &&
            data.video_framings[1].x == panes[1].x &&
            data.video_opacities[0] == 1.0f &&
            data.video_veils[1].alpha == 0.0f,
        "FC4: the Trim Monitor names A on the left and B on the right, says "
        "what each can still take (65 and 50 frames), and draws each frame "
        "whole in its pane, opaque, under no dip");
    [self keepFilmcraftDrawing:"fc-trim-monitor"];
}

// With CUTMACHINE_UI_QA_DIR, the window drawn off screen (VQA5), kept as
// `name`.png for a person or an agent to read: what no check here judges.
- (void)keepFilmcraftDrawing:(const char*)name {
    if (!UiQaEnabled()) return;
    [self presentNearestFrameAtDeadline:NO];
    // Drawn before AppKit's display pass, a label whose text has just
    // changed shows its old layer contents under the new string: let the
    // pass run first, as the VQA5 scenario does.
    [self spinRunLoopFor:0.3];
    std::string error;
    NSBitmapImageRep* drawn = [self drawEditorWindowOffscreen:error];
    NSData* png = drawn
                      ? [drawn representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}]
                      : nil;
    const std::filesystem::path path =
        gUiQaOutputDirectory / (std::string(name) + ".png");
    UiSmokeCheck(
        png != nil &&
            [png writeToFile:[NSString stringWithUTF8String:path.c_str()]
                  atomically:YES],
        ("FC: the window, drawn off screen, is kept for reading: " +
         path.string() + (drawn ? "" : " (" + error + ")"))
            .c_str());
}

// FC-fenêtre -- docs/roadmap/FILMCRAFT-2026-10.md. What the engine learned
// in lot FILMCRAFT-2026-10, wired in the window: every gesture is sent to
// the timeline's Metal view, every key to the window with the view first
// responder, so nothing here needs the window to be key or visible
// (CUTMACHINE_UI_SMOKE_SIMULATE=no-focus). Played on the "QA trim" timeline,
// left as it was found.
- (void)runUiSmokeFilmcraftScenario {
    const Ulid mainId = self.state->activeTimelineId;
    if (![self uiQaOpen:gUiSmokeTrimTimelineId label:"FC trim"]) return;
    const TimelineViewport originalView = self.state->viewport;
    // 100 points a second: a frame is four points wide, the magnet reaches
    // eight.
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 100.0;
    self.state->overlayDirty = true;
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    size_t lane = tracks.size();
    for (size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index]->kind == "video") lane = index;
    if (lane == tracks.size()) {
        UiSmokeCheck(false, "FC: the trim timeline has a video track");
        return;
    }
    const TimelineViewport& viewport = self.state->viewport;
    // A point of the video row, `fraction` of the way down it, in the Metal
    // view's own coordinates (y up), as the gestures are sent.
    const auto rowY = [&](double fraction) {
        return self.metalView.bounds.size.height -
               ([self videoHeight] + kTimelineRulerHeight +
                ((lane + fraction) * viewport.track_height));
    };
    const auto clip = [&](size_t index) {
        return self.state->document.FindClip(gUiSmokeTrimClipIds[index]);
    };
    const auto frames = [](const RationalTime& time) {
        return time.to_frames(25, 1);
    };
    const std::string pristine = self.state->document.SaveToString();

    [self setTimelineTool:TimelineTool::Select];
    // FC1 -- the timeline's In point holds the magnet. C's tail, at 85
    // frames, is dragged to five points (1.25 frames) right of an In at
    // 78: nothing else is within reach there, and without In the tail
    // would land on 79.
    {
        self.state->timelineIn = RationalTime{78, 25};
        self.state->timelineOut.reset();
        const size_t before = self.state->editLog.AppliedCount();
        SendTimelineMouseGesture(
            self.metalView, NSMakePoint(viewport.TimeToX({85, 25}), rowY(0.5)),
            NSMakePoint(viewport.TimeToX({78, 25}) + 5.0, rowY(0.5)));
        const DocumentClip* third = clip(2);
        const int64_t end =
            third ? frames(third->timeline_in.add(third->duration)) : -1;
        UiSmokeCheck(third && end == 78 &&
                         self.state->editLog.AppliedCount() == before + 1,
                     ("FC1: a tail dragged near the timeline's In point "
                      "lands on it (frame " +
                      std::to_string(end) + " for 78)")
                         .c_str());
        if (self.state->editLog.AppliedCount() == before + 1)
            [self menuUndo:nil];
        self.state->timelineIn.reset();
    }

    // FC4 -- the trim mode at the keyboard, on Resolve's keys. Every edit is
    // counted, to be undone at the end.
    const size_t editsBefore = self.state->editLog.AppliedCount();
    const auto edits = [&] {
        return self.state->editLog.AppliedCount() - editsBefore;
    };
    const auto key = [&](NSString* characters, unsigned short keyCode) {
        SendKeyThroughWindow(self.metalView, characters, keyCode);
    };
    const auto end = [&](size_t index) {
        const DocumentClip* shot = clip(index);
        return shot ? frames(shot->timeline_in.add(shot->duration)) : -1;
    };
    const auto start = [&](size_t index) {
        const DocumentClip* shot = clip(index);
        return shot ? frames(shot->timeline_in) : -1;
    };
    const auto label = [&] {
        return std::string(self.infoLabel.stringValue.UTF8String ?: "");
    };
    {
        [self requestTimelinePosition:{32, 25}];
        key(@"v", 9);
        const auto& points = self.state->trimSession.Points();
        UiSmokeCheck(points.size() == 1 &&
                         points[0].side == trim::TrimSide::Roll &&
                         points[0].outgoing_clip_id == gUiSmokeTrimClipIds[0] &&
                         points[0].incoming_clip_id == gUiSmokeTrimClipIds[1] &&
                         self.state->requestedPosition == RationalTime{30, 25},
                     ("FC4: V selects the cut nearest the playhead as a roll "
                      "and takes the playhead to it (" +
                      label() + ")")
                         .c_str());
        // Drawn on the zone a press finds each edge in, in the In green.
        const TimelineRenderData data = [self timelineRenderData];
        const std::vector<TimelineEdgeMarkRect> marks =
            VisibleTimelineEdgeMarks(
                self.state->document, viewport,
                self.metalView.bounds.size.width,
                trim::TrimSessionEdgeMarks(self.state->document,
                                           self.state->trimSession));
        const double cutX = viewport.TimeToX({30, 25});
        bool drawn = marks.size() == 2;
        for (const TimelineEdgeMarkRect& mark : marks)
            drawn =
                drawn &&
                UiSmokeDrawsRect(data, mark.x,
                                 data.video_height + mark.y +
                                     ui::theme::kTimelineClipBodyTop,
                                 mark.width, ui::theme::kTimelineClipBodyHeight,
                                 ui::theme::kTrimPoint) &&
                std::abs((mark.edge == TrimEdge::Tail ? mark.x + mark.width
                                                      : mark.x) -
                         cutX) < 0.01;
        UiSmokeCheck(drawn,
                     "FC4: the roll's two edges are marked green, on each "
                     "side of the cut, where a press takes them");
        [self runUiSmokeTrimMonitorChecks];
    }
    {
        key(@".", 47);
        UiSmokeCheck(
            edits() == 1 && end(0) == 31 && start(1) == 31 &&
                frames(self.state->trimSession.Points()[0].out_shift) == 1,
            ("FC4: the period rolls the cut one frame later, in one "
             "edit, and the counter says +1 (" +
             label() + ")")
                .c_str());
        key(@"u", 32);
        UiSmokeCheck(
            self.state->trimSession.Points()[0].side == trim::TrimSide::B,
            "FC4: U turns the centred point to its incoming side");
        key(@",", 43);
        UiSmokeCheck(edits() == 1 && start(1) == 31 &&
                         label().find("Trim refusé") != std::string::npos,
                     ("FC4: in Selection, the comma resizes B's head, which "
                      "A blocks: refused, said, nothing written (" +
                      label() + ")")
                         .c_str());
        key(@"t", 17);
        const RationalTime lengthBefore = self.state->duration;
        key(@",", 43);
        UiSmokeCheck(
            [self effectiveTool] == TimelineTool::Trim && edits() == 2 &&
                frames(self.state->duration.sub(lengthBefore)) == 1,
            ("FC4: T is the Trim mode, where the comma ripples B's head: one "
             "edit, the timeline one frame longer (" +
             label() + ")")
                .c_str());
        const RationalTime lengthMany = self.state->duration;
        [self menuTrimNudgeForwardMany:nil];
        UiSmokeCheck(
            edits() == 3 && frames(lengthMany.sub(self.state->duration)) ==
                                trim::kLargeTrimFrames,
            "FC4: Shift-period ripples five frames, in one edit");
        [self keepFilmcraftDrawing:"fc-trim-monitor-ripple"];
        [self setTimelineTool:TimelineTool::Select];
    }
    {
        // A click on A's tail that moves nothing selects its outgoing side;
        // E, in Selection, brings it to the playhead. Four points inside A:
        // the cut's own centre is a roll since ON9.
        const double tailX = viewport.TimeToX({end(0), 25}) - 4.0;
        SendTimelineMouseGesture(self.metalView, NSMakePoint(tailX, rowY(0.5)),
                                 NSMakePoint(tailX, rowY(0.5)));
        const auto& points = self.state->trimSession.Points();
        UiSmokeCheck(points.size() == 1 &&
                         points[0].side == trim::TrimSide::A &&
                         points[0].outgoing_clip_id == gUiSmokeTrimClipIds[0],
                     "FC4: a click on A's tail selects its outgoing side");
        [self requestTimelinePosition:{26, 25}];
        key(@"e", 14);
        UiSmokeCheck(edits() == 4 && end(0) == 26,
                     ("FC4: E brings A's tail to the playhead, in one edit (" +
                      std::to_string(end(0)) + ")")
                         .c_str());
        // A Shift-click on C's head adds its point.
        const double headX = viewport.TimeToX({start(2), 25}) + 4.0;
        SendTimelineMouseGesture(self.metalView, NSMakePoint(headX, rowY(0.5)),
                                 NSMakePoint(headX, rowY(0.5)),
                                 NSEventModifierFlagShift);
        UiSmokeCheck(
            self.state->trimSession.Points().size() == 2 && edits() == 4,
            "FC4: a Shift-click on another edge adds its point, and "
            "writes nothing");
        SendTimelineMouseGesture(self.metalView, NSMakePoint(headX, rowY(0.5)),
                                 NSMakePoint(headX, rowY(0.5)),
                                 NSEventModifierFlagShift);
        UiSmokeCheck(self.state->trimSession.Points().size() == 1,
                     "FC4: and a second one takes it out again");
    }
    {
        // W, L for a while, K: the dynamic trim runs into the gap E left
        // after A (five frames to B) and holds there; K writes it once.
        key(@"w", 13);
        key(@"l", 37);
        const bool running = self.state->trimSession.Dynamic().active;
        UiSmokeWaitUntil([&] { return false; }, 0.4);
        const bool previewed = self.state->trimPreview.has_value();
        UiSmokeCheck(running && previewed && edits() == 4,
                     "FC4: W then L trims A's tail on the clock, in a preview: "
                     "nothing written yet");
        key(@"k", 40);
        UiSmokeCheck(
            edits() == 5 && end(0) == start(1) &&
                !self.state->trimPreview.has_value() &&
                !self.state->trimSession.Dynamic().active,
            ("FC4: K writes the dynamic trim as one edit, held where B "
             "begins (A ends at " +
             std::to_string(end(0)) + ", B begins at " +
             std::to_string(start(1)) + ")")
                .c_str());
        key(@"j", 38);
        UiSmokeWaitUntil([&] { return false; }, 0.15);
        key(@"\x1b", 53);
        UiSmokeCheck(edits() == 5 && !self.state->trimPreview &&
                         !self.state->trimSession.Dynamic().active &&
                         self.state->trimSession.Active(),
                     "FC4: Escape drops a dynamic trim, nothing written, the "
                     "point kept");
        key(@"w", 13);
        UiSmokeCheck(!self.state->dynamicTrimMode,
                     "FC4: W again leaves the dynamic mode");
        key(@"\x1b", 53);
        UiSmokeCheck(!self.state->trimSession.Active(),
                     "FC4: Escape leaves the trim mode");
    }
    while (edits() > 0) [self menuUndo:nil];

    // ON8 -- docs/roadmap/ONGLETS-2026-10.md. The same cut, by the trim
    // bar's buttons, pressed without the keyboard or the focus.
    {
        [self setTimelineTool:TimelineTool::Select];
        [self leaveTrimMode];
        UiSmokeCheck(self.trimBarView.hidden,
                     "ON8: no trim bar with the Selection tool and no point");
        [self setTimelineTool:TimelineTool::Trim];
        NSButton* take = [self.trimBarView buttonForAction:@"select"];
        UiSmokeCheck(!self.trimBarView.hidden && take != nil && !take.hidden &&
                         [take.toolTip hasSuffix:@"· V"],
                     "ON8: the Trim tool shows the bar, whose button takes the "
                     "nearest edit and says its key");
        [self requestTimelinePosition:{32, 25}];
        [take performClick:nil];
        NSButton* roll = [self.trimBarView buttonForAction:@"side.roll"];
        UiSmokeCheck(self.state->trimSession.Points().size() == 1 &&
                         self.state->trimSession.Points()[0].side ==
                             trim::TrimSide::Roll &&
                         roll != nil && roll.state == NSControlStateValueOn,
                     "ON8: pressed, it takes the cut as a roll, and Roll "
                     "lights");
        [[self.trimBarView buttonForAction:@"side.a"] performClick:nil];
        UiSmokeCheck(
            self.state->trimSession.Points()[0].side == trim::TrimSide::A &&
                [self.trimBarView buttonForAction:@"side.a"].state ==
                    NSControlStateValueOn &&
                roll.state == NSControlStateValueOff,
            "ON8: A takes the outgoing side, and lights instead");
        const int64_t tail = end(0);
        [[self.trimBarView buttonForAction:@"nudge.-1"] performClick:nil];
        UiSmokeCheck(
            edits() == 1 && end(0) == tail - 1,
            ("ON8: -1 ripples A's tail one frame, in one edit (from " +
             std::to_string(tail) + " to " + std::to_string(end(0)) + ")")
                .c_str());
        UiSmokeCheck(self.window.firstResponder == self.metalView,
                     "ON8: after a button, the keys still reach the timeline");
        [[self.trimBarView buttonForAction:@"leave"] performClick:nil];
        UiSmokeCheck(!self.state->trimSession.Active() &&
                         [self.trimBarView buttonForAction:@"select"] != nil,
                     "ON8: Quitter leaves the trim mode; the Trim tool's bar "
                     "offers the nearest edit again");
        while (edits() > 0) [self menuUndo:nil];
        [self setTimelineTool:TimelineTool::Select];
        UiSmokeCheck(self.trimBarView.hidden,
                     "ON8: back to Selection, the bar goes");
    }

    // ON9 -- docs/roadmap/ONGLETS-2026-10.md. Resolve's trim gestures on the
    // same cut, at frame 30: its centre rolls with the Selection tool, the
    // Trim Monitor trims under the hand, the slash plays around it.
    {
        [self setTimelineTool:TimelineTool::Select];
        [self leaveTrimMode];
        const double cutX = viewport.TimeToX({30, 25});
        const TimelinePointerTarget centre =
            [self timelineTargetAt:NSMakePoint(
                                       cutX, self.metalView.bounds.size.height -
                                                 rowY(0.3) - [self videoHeight])
                             event:nil];
        UiSmokeCheck(centre.kind == TimelinePointerTargetKind::ClipEdge &&
                         centre.trim_mode == TimelineTrimMode::Roll,
                     "ON9: the centre of the cut rolls with the Selection "
                     "tool, no Command");
        SendTimelineMouseGesture(self.metalView, NSMakePoint(cutX, rowY(0.3)),
                                 NSMakePoint(cutX, rowY(0.3)));
        UiSmokeCheck(self.state->trimSession.Points().size() == 1 &&
                         self.state->trimSession.Points()[0].side ==
                             trim::TrimSide::Roll,
                     "ON9: a click there takes the cut as a roll");
        [self requestResolvedPosition:self.state->requestedPosition];
        UiSmokeCheck(!self.state->trimMonitorPanes.empty(),
                     "ON9: the Trim Monitor is up on Record");
        const NSRect record = self.programMonitorView.bounds;
        const CGFloat leftX = NSMinX(record) + record.size.width * 0.25;
        const int64_t tail = end(0);
        const bool pressed =
            [self trimMonitorPressAt:NSMakePoint(leftX, NSMidY(record))];
        [self trimMonitorDragTo:NSMakePoint(leftX - 12.0, NSMidY(record))];
        const bool previewed = self.state->trimPreview.has_value() &&
                               self.trimMonitorDragFrames == -2;
        [self trimMonitorRelease];
        UiSmokeCheck(
            pressed && previewed &&
                self.state->trimSession.Points()[0].side == trim::TrimSide::A &&
                edits() == 1 && end(0) == tail - 2,
            ("ON9: on the Trim Monitor the left image takes A; "
             "dragged 12 points left it previews two frames, and "
             "the release trims A's tail by two in one edit (" +
             std::to_string(tail) + " to " + std::to_string(end(0)) + ")")
                .c_str());
        while (edits() > 0) [self menuUndo:nil];
        [self leaveTrimMode];
        SendTimelineMouseGesture(self.metalView, NSMakePoint(cutX, rowY(0.3)),
                                 NSMakePoint(cutX, rowY(0.3)));
        [self playAroundSelection];
        UiSmokeCheck(self.playAroundActive &&
                         self.state->playbackDirection == 1 &&
                         self.playAroundReturn == RationalTime{30, 25} &&
                         self.playAroundEnd == RationalTime{80, 25},
                     "ON9: the slash plays from before the cut to two seconds "
                     "after it");
        [self requestTimelinePosition:{81, 25}];
        [self checkPlayAround];
        UiSmokeCheck(!self.playAroundActive &&
                         self.state->playbackDirection == 0 &&
                         self.state->requestedPosition == RationalTime{30, 25},
                     "ON9: past its end it stops, the playhead back on the "
                     "cut");
        self.loopPlayback = YES;
        [self playAroundSelection];
        [self requestTimelinePosition:{81, 25}];
        [self checkPlayAround];
        UiSmokeCheck(self.playAroundActive &&
                         self.state->playbackDirection == 1 &&
                         self.state->requestedPosition < RationalTime{30, 25},
                     "ON9: looped, it starts again from before the cut");
        [self setPlaybackDirection:0];
        self.loopPlayback = NO;
        UiSmokeCheck(!self.playAroundActive,
                     "ON9: any other playback command ends it");
        [self leaveTrimMode];
    }

    // FC3 -- the Trim tool's name bar slides B between A and C; Command-R
    // shows its Retime controls, whose tail end stretches it.
    {
        [self setTimelineTool:TimelineTool::Trim];
        const double nameBar =
            rowY((ui::theme::kTimelineClipNameBandTop + 4.0) /
                 viewport.track_height);
        const double from = viewport.TimeToX({45, 25});
        SendTimelineMouseGesture(self.metalView, NSMakePoint(from, nameBar),
                                 NSMakePoint(from + 12.0, nameBar));
        const DocumentClip* second = clip(1);
        const DocumentClip* third = clip(2);
        UiSmokeCheck(
            edits() == 1 && start(1) == 33 && end(1) == 63 && end(0) == 33 &&
                start(2) == 63 && end(2) == 85 && second &&
                second->source_in == RationalTime{50, 25} && third &&
                third->source_in == RationalTime{23, 25},
            ("FC3: with the Trim tool, B's name bar dragged three frames "
             "slides it between A and C, which absorb the move, in one edit "
             "(B from " +
             std::to_string(start(1)) + ")")
                .c_str());
        [self menuUndo:nil];
        [self setTimelineTool:TimelineTool::Select];
        self.state->interaction->SelectClip(gUiSmokeTrimClipIds[1]);
        [self.window makeFirstResponder:self.metalView];
        [self.window sendEvent:UiSmokeKeyEvent(self.window, @"r", @"r", 15,
                                               NSEventModifierFlagCommand)];
        UiSmokeCheck(
            self.state->retimeClipIds ==
                std::vector<Ulid>{gUiSmokeTrimClipIds[1]},
            "FC3: Command-R shows the selected clip's Retime controls");
        const double width = self.metalView.bounds.size.width;
        const std::vector<TimelineRetimeBand> bands =
            VisibleTimelineRetimeBands(self.state->document, viewport, width,
                                       self.state->retimeClipIds);
        const TimelineRenderData data = [self timelineRenderData];
        UiSmokeCheck(
            bands.size() == 1 &&
                UiSmokeDrawsRect(data, bands[0].x,
                                 data.video_height + bands[0].y, bands[0].width,
                                 bands[0].height, ui::theme::kRetimeBand) &&
                UiSmokeDrawsRect(data, bands[0].tail_x,
                                 data.video_height + bands[0].y,
                                 bands[0].tail_width, bands[0].height,
                                 ui::theme::kRetimeHandle) &&
                UiSmokeDrawnText(data, "100%") != nullptr,
            "FC3: the controls are drawn where their ends are pressed, the "
            "speed written on them");
        [self keepFilmcraftDrawing:"fc-retime-controls"];
        if (!bands.empty()) {
            const double bandY =
                self.metalView.bounds.size.height -
                ([self videoHeight] + bands[0].y + (bands[0].height / 2.0));
            const double tailX = viewport.TimeToX({60, 25}) - 1.0;
            SendTimelineMouseGesture(self.metalView, NSMakePoint(tailX, bandY),
                                     NSMakePoint(tailX - 20.0, bandY));
        }
        const DocumentClip* stretched = clip(1);
        UiSmokeCheck(
            edits() == 1 && stretched &&
                stretched->duration == RationalTime{25, 25} &&
                stretched->speed.num * 5 == stretched->speed.den * 6 &&
                stretched->source_in == RationalTime{50, 25} && start(2) == 60,
            ("FC3: its tail end dragged five frames back plays B's 30 frames "
             "in 25, at 6/5, and leaves C where it was, in one edit (" +
             (stretched ? std::to_string(stretched->speed.num) + "/" +
                              std::to_string(stretched->speed.den)
                        : std::string("no clip")) +
             ")")
                .c_str());
        // DEC40 -- the same drag with the Trim tool ripples, as Resolve's
        // Trim mode does: C comes five frames closer.
        [self menuUndo:nil];
        [self setTimelineTool:TimelineTool::Trim];
        if (!bands.empty()) {
            const double bandY =
                self.metalView.bounds.size.height -
                ([self videoHeight] + bands[0].y + (bands[0].height / 2.0));
            const double tailX = viewport.TimeToX({60, 25}) - 1.0;
            SendTimelineMouseGesture(self.metalView, NSMakePoint(tailX, bandY),
                                     NSMakePoint(tailX - 20.0, bandY));
        }
        const DocumentClip* rippled = clip(1);
        UiSmokeCheck(
            edits() == 1 && rippled &&
                rippled->duration == RationalTime{25, 25} &&
                rippled->speed.num * 5 == rippled->speed.den * 6 &&
                start(1) == 30 && start(2) == 55,
            ("DEC40: with the Trim tool, the same drag plays B's 30 frames "
             "in 25 and brings C five frames closer, in one edit (C from " +
             std::to_string(start(2)) + ")")
                .c_str());
        [self setTimelineTool:TimelineTool::Select];
        SendKeyThroughWindow(self.metalView, @"\x1b", 53);
        UiSmokeCheck(self.state->retimeClipIds.empty(),
                     "FC3: Escape closes the Retime controls");
        while (edits() > 0) [self menuUndo:nil];
    }

    // DEC40 -- Command-click extends a selection of clips, as in Resolve;
    // a second one takes the clip out. Nothing is written.
    {
        [self setTimelineTool:TimelineTool::Select];
        self.state->interaction->SelectClip(gUiSmokeTrimClipIds[0]);
        const NSPoint onC = NSMakePoint(viewport.TimeToX({72, 25}), rowY(0.5));
        SendTimelineMouseGesture(self.metalView, onC, onC,
                                 NSEventModifierFlagCommand);
        const std::vector<Ulid>& chosen =
            self.state->interaction->SelectedClipIds();
        const auto holds = [&](size_t index) {
            return std::find(chosen.begin(), chosen.end(),
                             gUiSmokeTrimClipIds[index]) != chosen.end();
        };
        UiSmokeCheck(holds(0) && holds(2) && !holds(1) && edits() == 0,
                     "DEC40: a Command-click on C adds it to A, writing "
                     "nothing");
        SendTimelineMouseGesture(self.metalView, onC, onC,
                                 NSEventModifierFlagCommand);
        UiSmokeCheck(holds(0) && !holds(2) && edits() == 0,
                     "DEC40: and a second one takes it out again");
    }

    UiSmokeCheck(self.state->document.SaveToString() == pristine,
                 "FC: the trim timeline is left as it was found");
    self.state->viewport = originalView;
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:mainId.c_str()]];
    self.state->timelineViews.erase(gUiSmokeTrimTimelineId);
    self.state->overlayDirty = true;
}

// SYNCHRO-2026-10 SY2 -- docs/roadmap/SYNCHRO-2026-10.md. Resolve's Auto
// Align Clips, Based on Waveform, wired in the window: the picture and the
// recording dropped on a third track are selected on the timeline, the
// entry of their own menu measures the two sounds in the background and
// slides the recording into sync, linked, in one edit. Every gesture is
// sent to the timeline's Metal view: nothing here needs the window to be
// key or visible. Played on the "QA son séparé" timeline, left as found.
- (void)runUiSmokeSeparateSoundScenario {
    const Ulid mainId = self.state->activeTimelineId;
    if (![self uiQaOpen:gUiSmokeSoundTimelineId label:"SY2"]) return;
    const TimelineViewport originalView = self.state->viewport;
    const bool originalLinked = self.state->linkedSelection;
    self.state->linkedSelection = true;
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 60.0;
    self.state->overlayDirty = true;
    [self setTimelineTool:TimelineTool::Select];
    // A point of `clipId`'s row, `seconds` into the timeline, in the Metal
    // view's own coordinates (y up), as the gestures are sent.
    const auto pointOf = [&](const Ulid& clipId, double seconds) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const DocumentTrack* track =
            self.state->document.FindTrackForClip(clipId);
        size_t lane = 0;
        for (size_t index = 0; index < tracks.size(); ++index)
            if (track != nullptr && tracks[index]->id == track->id)
                lane = index;
        const TimelineViewport& viewport = self.state->viewport;
        return NSMakePoint(
            viewport.TimeToX({static_cast<int64_t>(seconds * 25.0), 25}),
            self.metalView.bounds.size.height -
                ([self videoHeight] + kTimelineRulerHeight +
                 ((static_cast<double>(lane) + 0.5) * viewport.track_height)));
    };
    const auto click = [&](const Ulid& clipId, double seconds,
                           NSEventModifierFlags modifiers) {
        const NSPoint point = pointOf(clipId, seconds);
        SendTimelineMouseGesture(self.metalView, point, point, modifiers);
    };
    const auto label = [&] {
        return std::string(self.infoLabel.stringValue.UTF8String ?: "");
    };
    const auto selected = [&] {
        std::vector<Ulid> ids = self.state->interaction->SelectedClipIds();
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const std::string pristine = self.state->document.SaveToString();
    const size_t editsBefore = self.state->editLog.AppliedCount();
    const auto edits = [&] {
        return self.state->editLog.AppliedCount() - editsBefore;
    };

    // The picture alone: refused at once, in words, nothing measured.
    click(gUiSmokeSoundPictureId, 3.0, static_cast<NSEventModifierFlags>(0));
    [self menuAutoAlignByWaveform:nil];
    UiSmokeCheck(!self.state->soundAlignmentRunning && edits() == 0 &&
                     label().find("Synchro impossible") != std::string::npos &&
                     label().find("enregistreur") != std::string::npos,
                 ("SY2: the picture without a recording is refused at once, "
                  "saying what is missing (" +
                  label() + ")")
                     .c_str());

    // Shift-click adds the recording to the picture and its own sound; a
    // right click on it keeps all three and offers Resolve's entry.
    click(gUiSmokeSoundRecorderId, 9.0, NSEventModifierFlagShift);
    std::vector<Ulid> three = {gUiSmokeSoundPictureId, gUiSmokeSoundCameraId,
                               gUiSmokeSoundRecorderId};
    std::sort(three.begin(), three.end());
    UiSmokeCheck(selected() == three,
                 "SY2: a Shift-click adds the recording to the picture and "
                 "its linked sound");
    NSEvent* rightClick = [NSEvent
        mouseEventWithType:NSEventTypeRightMouseDown
                  location:[self.metalView
                               convertPoint:pointOf(gUiSmokeSoundRecorderId,
                                                    9.0)
                                     toView:nil]
             modifierFlags:0
                 timestamp:NSProcessInfo.processInfo.systemUptime
              windowNumber:self.window.windowNumber
                   context:nil
               eventNumber:0
                clickCount:1
                  pressure:1.0];
    NSMenu* menu = [self.metalView menuForEvent:rightClick];
    NSMenuItem* waveform = nil;
    for (NSMenuItem* entry in menu.itemArray)
        if ([entry.title isEqualToString:@"Aligner automatiquement les plans"])
            for (NSMenuItem* inner in entry.submenu.itemArray)
                if (inner.action == @selector(menuAutoAlignByWaveform:))
                    waveform = inner;
    UiSmokeCheck(
        selected() == three && waveform != nil &&
            [waveform.title isEqualToString:@"Selon la forme d’onde"] &&
            [self validateMenuItem:waveform],
        "SY2: a right click on a selected clip keeps the selection, "
        "and its menu offers « Aligner automatiquement les plans › "
        "Selon la forme d’onde », enabled");
    NSMenuItem* clipMenuEntry = nil;
    for (NSMenuItem* root in NSApp.mainMenu.itemArray)
        if ([root.title isEqualToString:@"Clip"])
            for (NSMenuItem* entry in root.submenu.itemArray)
                for (NSMenuItem* inner in entry.submenu.itemArray)
                    if (inner.action == @selector(menuAutoAlignByWaveform:))
                        clipMenuEntry = inner;
    UiSmokeCheck(clipMenuEntry != nil, "SY2: the Clip menu has the same entry");

    // Chosen: the two sounds are measured off the main thread, then the
    // recording slides under the picture, linked, the camera's track off.
    if (waveform)
        [NSApp sendAction:waveform.action to:waveform.target from:waveform];
    const bool started = self.state->soundAlignmentRunning;
    const std::string measuring = label();
    UiSmokeCheck(started && [self validateMenuItem:waveform] == NO &&
                     measuring.find("mesure") != std::string::npos,
                 ("SY2: the measure runs in the background, said, the entry "
                  "off meanwhile (" +
                  measuring + ")")
                     .c_str());
    UiSmokeWaitUntil([&] { return !self.state->soundAlignmentRunning; }, 60.0);
    const DocumentClip* heard =
        self.state->document.FindClip(gUiSmokeSoundRecorderId);
    const DocumentClip* picture =
        self.state->document.FindClip(gUiSmokeSoundPictureId);
    const DocumentTrack* cameraTrack =
        self.state->document.FindTrackForClip(gUiSmokeSoundCameraId);
    const bool aligned = edits() == 1 && heard != nullptr &&
                         picture != nullptr &&
                         heard->timeline_in == RationalTime{25, 25} &&
                         heard->duration == RationalTime{100, 25} &&
                         heard->source_in == RationalTime{5, 2} &&
                         heard->link_group_id == picture->link_group_id &&
                         heard->sync_anchor_clip_id == picture->id &&
                         cameraTrack != nullptr && cameraTrack->muted;
    UiSmokeCheck(aligned && label().find("calé") != std::string::npos,
                 ("SY2: the recording lies under the picture from 1 s to 5 s, "
                  "2.5 s into itself, linked, the camera's track off, in one "
                  "edit (" +
                  label() + ")")
                     .c_str());
    [self keepFilmcraftDrawing:"sy2-son-cale"];
    [self menuUndo:nil];
    UiSmokeCheck(self.state->document.SaveToString() == pristine,
                 "SY2: one undo gives the recording back where it was "
                 "dropped, the camera's track on");

    // The grey picture over silence and the recording: the measure refuses,
    // the window says why, nothing is laid.
    click(gUiSmokeSoundSilentPictureId, 7.0,
          static_cast<NSEventModifierFlags>(0));
    click(gUiSmokeSoundRecorderId, 9.0, NSEventModifierFlagShift);
    const BOOL measured = [self alignSelectedSoundByWaveform];
    UiSmokeWaitUntil([&] { return !self.state->soundAlignmentRunning; }, 60.0);
    UiSmokeCheck(measured && edits() == 0 &&
                     label().find("Synchro refusée") != std::string::npos &&
                     label().find("Rien n’a été posé") != std::string::npos &&
                     self.state->document.SaveToString() == pristine,
                 ("SY2: a recording that matches nothing is refused by the "
                  "measure, said in French, nothing laid (" +
                  label() + ")")
                     .c_str());
    [self keepFilmcraftDrawing:"sy2-synchro-refusee"];

    // GitHub #330 -- the picture's sound in the Inspector's « Son » section:
    // its gain and a fade typed, a fade too long refused in words; then its
    // fade out dragged by its handle on the timeline. Driven as AppKit
    // drives the controls, every gesture sent to the views: nothing here
    // needs the window key or visible.
    {
        const size_t base = self.state->editLog.AppliedCount();
        const auto made = [&] {
            return self.state->editLog.AppliedCount() - base;
        };
        const auto sound = [&] {
            return self.state->document.FindClip(gUiSmokeSoundCameraId);
        };
        const auto text = [](NSTextField* field) {
            return std::string(field.stringValue.UTF8String ?: "");
        };
        const auto type = [](NSTextField* field, NSString* typed) {
            field.stringValue = typed;
            [field sendAction:field.action to:field.target];
        };
        CMInspectorView* inspector = self.inspectorView;
        click(gUiSmokeSoundPictureId, 3.0,
              static_cast<NSEventModifierFlags>(0));
        UiSmokeCheck(
            !inspector.soundGainField.superview.hidden &&
                inspector.soundGainField.enabled &&
                text(inspector.soundHeader).find("A1") != std::string::npos &&
                text(inspector.soundGainField) == "0 dB" &&
                text(inspector.soundFadeInField) == "0" &&
                text(inspector.soundFadeOutField) == "0",
            ("#330: the picture shows the gain and fades of its sound, the "
             "section naming its track (" +
             text(inspector.soundHeader) + ")")
                .c_str());
        type(inspector.soundGainField, @"-6");
        type(inspector.soundFadeInField, @"12");
        const DocumentClip* set = sound();
        UiSmokeCheck(made() == 2 && set != nullptr &&
                         set->audio_gain_db.num == -6 &&
                         set->audio_gain_db.den == 1 &&
                         set->audio_fade_in == RationalTime{12, 25} &&
                         text(inspector.soundGainField) == "-6 dB",
                     "#330: a gain and a fade typed are one edit each, on the "
                     "linked sound");
        type(inspector.soundFadeOutField, @"999");
        UiSmokeCheck(
            made() == 2 &&
                text(inspector.soundNote).find("refusé") != std::string::npos &&
                text(inspector.soundFadeOutField) == "0",
            ("#330: a fade longer than the clip is refused in words, "
             "nothing written (" +
             text(inspector.soundNote) + ")")
                .c_str());
        const double width = self.metalView.bounds.size.width;
        const auto fadeOfSound = [&] {
            for (const TimelineAudioFadeRect& fade : VisibleTimelineAudioFades(
                     self.state->document, self.state->viewport, width,
                     VisibleTimelineClips(self.state->document,
                                          self.state->viewport, width, {},
                                          std::nullopt)))
                if (fade.clip_id == gUiSmokeSoundCameraId) return fade;
            return TimelineAudioFadeRect{};
        };
        const TimelineAudioFadeRect before = fadeOfSound();
        TimelineRenderData data = [self timelineRenderData];
        UiSmokeCheck(
            UiSmokeDrawnText(data, "-6 dB") != nullptr && before.handles &&
                UiSmokeDrawsRect(data, before.out_handle_x,
                                 data.video_height + before.handle_y,
                                 before.out_handle_width, before.handle_height,
                                 ui::theme::kTimelineFadeHandle) &&
                !before.steps.empty() &&
                UiSmokeDrawsRect(
                    data, before.steps.front().x,
                    data.video_height + before.body_y,
                    before.steps.front().width,
                    (1.0 - before.steps.front().gain) * before.body_height,
                    ui::theme::kTimelineFadeVeil),
            "#330: the selected sound says its gain, shows its handles where "
            "they are pressed and veils its fade in");
        // At 60 points a second, 30 points toward the middle are half a
        // second: 12.5 frames, rounded to 13.
        const double handleX =
            before.out_handle_x + (before.out_handle_width / 2.0);
        const double handleY = self.metalView.bounds.size.height -
                               ([self videoHeight] + before.handle_y +
                                (before.handle_height / 2.0));
        SendTimelineMouseGesture(self.metalView, NSMakePoint(handleX, handleY),
                                 NSMakePoint(handleX - 30.0, handleY));
        const DocumentClip* faded = sound();
        UiSmokeCheck(made() == 3 && faded != nullptr &&
                         faded->audio_fade_out == RationalTime{13, 25} &&
                         faded->audio_fade_in == RationalTime{12, 25} &&
                         faded->audio_gain_db.num == -6 &&
                         text(inspector.soundFadeOutField) == "13" &&
                         text(inspector.soundNote).empty() &&
                         label().find("FONDU DE SORTIE") != std::string::npos,
                     ("#330: the fade out's handle dragged half a second in "
                      "sets 13 frames, in one edit, said on the status line, "
                      "and the earlier refusal goes (" +
                      label() + ")")
                         .c_str());
        [self keepFilmcraftDrawing:"330-son-fondus"];
        while (self.state->editLog.AppliedCount() > base) [self menuUndo:nil];
        UiSmokeCheck(self.state->document.SaveToString() == pristine,
                     "#330: undone, the sound is as it was");
    }

    self.state->interaction->SelectClips({});
    self.state->linkedSelection = originalLinked;
    self.state->viewport = originalView;
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:mainId.c_str()]];
    self.state->timelineViews.erase(gUiSmokeSoundTimelineId);
    self.state->overlayDirty = true;
}

// F2.3 -- ROADMAP.md. A carton's style in the Inspector, as Resolve's Track
// Style: its stroke, shadow, box and position set by their own controls,
// each gesture one edit, a drag of the height one at its release, and the
// engine's bounds refused in words. The controls are driven as AppKit
// drives them, without the window being key. Played on "QA légendes", left
// as found.
- (void)runUiSmokeCaptionStyleScenario {
    const Ulid mainId = self.state->activeTimelineId;
    if (![self uiQaOpen:gUiSmokeCaptionTimelineId label:"F2.3"]) return;
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    Ulid cartonId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "caption" && !track.clips.empty())
            cartonId = track.clips.front().id;
    const DocumentClip* carton = self.state->document.FindClip(cartonId);
    if (carton == nullptr) {
        UiSmokeCheck(false, "F2.3: the captions timeline has a carton");
        return;
    }
    const Ulid styleId = carton->caption_group_id;
    const auto style = [&]() -> const CaptionStyle* {
        return self.state->document.FindCaptionStyle(styleId);
    };
    const std::string pristine = self.state->document.SaveToString();
    const size_t editsBefore = self.state->editLog.AppliedCount();
    const auto edits = [&] {
        return self.state->editLog.AppliedCount() - editsBefore;
    };
    self.state->interaction->SelectClip(cartonId);
    [self updateSelectionInfo];
    CMInspectorView* inspector = self.inspectorView;
    UiSmokeCheck(
        !inspector.captionStyleOutlineField.hidden &&
            inspector.captionStyleOutlineField.enabled &&
            inspector.captionStyleOutlineField.integerValue == 4 &&
            inspector.captionStylePositionPopup.indexOfSelectedItem == 3 &&
            inspector.captionStyleHeightSlider.integerValue == 75 &&
            [inspector.captionStyleNote.stringValue
                containsString:@"ce carton"],
        ("F2.3: a carton selected, the Inspector shows its style: "
         "a 4 px stroke, the height 3/4 chosen, and whom it dresses "
         "(« " +
         std::string(inspector.captionStyleNote.stringValue.UTF8String ?: "") +
         " »)")
            .c_str());

    // The stroke typed, then its colour chosen: one edit each.
    inspector.captionStyleOutlineField.integerValue = 8;
    [inspector.captionStyleOutlineField
        sendAction:inspector.captionStyleOutlineField.action
                to:inspector.captionStyleOutlineField.target];
    inspector.captionStyleOutlineWell.color = [NSColor colorWithSRGBRed:1.0
                                                                  green:0.0
                                                                   blue:0.0
                                                                  alpha:1.0];
    [inspector.captionStyleOutlineWell
        sendAction:inspector.captionStyleOutlineWell.action
                to:inspector.captionStyleOutlineWell.target];
    UiSmokeCheck(edits() == 2 && style() && style()->outline_width == 8 &&
                     style()->outline_color == "#ff0000" &&
                     inspector.captionStyleOutlineField.integerValue == 8,
                 "F2.3: the stroke typed and its colour chosen are two edits "
                 "of the style");

    // The drop shadow stepped, the box ticked: a black box at 60 %, a
    // quarter of the size as margin.
    const NSInteger shadowBefore =
        inspector.captionStyleShadowField.integerValue;
    NSStepper* shadowStepper = nil;
    for (NSView* view in inspector.captionStyleShadowWell.superview.subviews)
        if ([view isKindOfClass:NSStepper.class])
            shadowStepper = (NSStepper*)view;
    shadowStepper.integerValue = shadowBefore + 3;
    [shadowStepper sendAction:shadowStepper.action to:shadowStepper.target];
    [inspector.captionStyleBoxCheck performClick:nil];
    UiSmokeCheck(edits() == 4 && style() &&
                     style()->shadow_offset == shadowBefore + 3 &&
                     style()->background_color ==
                         ui::media_panel::kCaptionDefaultBackground &&
                     style()->background_padding == 16 &&
                     inspector.captionStylePaddingField.enabled,
                 "F2.3: the stepper moves the shadow by one edit; ticking "
                 "the box gives a black box at 60 % with a quarter of the "
                 "size as margin");

    // Past the engine's bound: refused, said, nothing written.
    inspector.captionStylePaddingField.stringValue = @"600";
    [inspector.captionStylePaddingField
        sendAction:inspector.captionStylePaddingField.action
                to:inspector.captionStylePaddingField.target];
    UiSmokeCheck(
        edits() == 4 && style() && style()->background_padding == 16 &&
            inspector.captionStylePaddingField.integerValue == 16 &&
            [inspector.captionStyleNote.stringValue containsString:@"512"],
        ("F2.3: a margin past the engine's bound is refused, the "
         "bound said, nothing written (« " +
         std::string(inspector.captionStyleNote.stringValue.UTF8String ?: "") +
         " »)")
            .c_str());

    // A preset, then the height dragged: one edit at the release.
    [inspector.captionStylePositionPopup selectItemAtIndex:2];
    [inspector.captionStylePositionPopup
        sendAction:inspector.captionStylePositionPopup.action
                to:inspector.captionStylePositionPopup.target];
    UiSmokeCheck(edits() == 5 && style() && style()->position == "top" &&
                     style()->vertical_position.num == 0,
                 "F2.3: « En haut » is the top preset, the height forgotten");
    [self keepFilmcraftDrawing:"f23-inspecteur-style"];
    // A drag of the height as the slider reports one (CMTrackedSlider):
    // ticks while a hand holds it, then the release. AppKit's own tracking
    // loop is not driven here: it waits for a hand.
    CMTrackedSlider* slider = inspector.captionStyleHeightSlider;
    slider.tracking = YES;
    for (const NSInteger height : {20, 25, 30}) {
        slider.integerValue = height;
        [slider sendAction:slider.action to:slider.target];
    }
    const size_t duringDrag = edits();
    slider.tracking = NO;
    [slider sendAction:slider.action to:slider.target];
    const CaptionStyle* dragged = style();
    UiSmokeCheck(
        duringDrag == 5 && edits() == 6 && dragged != nullptr &&
            dragged->vertical_position.num == 3 &&
            dragged->vertical_position.den == 10 &&
            inspector.captionStylePositionPopup.indexOfSelectedItem == 3,
        ("F2.3: a drag of the height writes nothing while it moves, "
         "then one edit at the release, 30 % as 3/10 (" +
         std::to_string(duringDrag - 5) + " during, " +
         std::to_string(edits() - duringDrag) + " at the release)")
            .c_str());

    while (edits() > 0) [self menuUndo:nil];
    UiSmokeCheck(self.state->document.SaveToString() == pristine,
                 "F2.3: the undos give the style back to the byte");
    self.state->interaction->SelectClips({});
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:mainId.c_str()]];
    self.state->timelineViews.erase(gUiSmokeCaptionTimelineId);
    self.state->overlayDirty = true;
}

// CH6 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel, wired, on a
// library of its own (a fresh folder named by CUTMACHINE_MOTION_LIBRARY for
// the scenario, never the user's) seeded with a neutral charter `marque`:
// a colour chosen in a well writes a version and rewrites the file; the
// logo dragged in 9:16 writes that format only; Cmd+Z with the panel key
// restores the file, the montage's EditLog untouched; a hand edit of the
// file is read again; a file that no longer reads is said, and refuses
// every write; the panel is drawn for capture_editor_window. The drag is
// handed to the preview's view as a hand makes it, with Command held (no
// snapping, so that the release lands where the steps say). CH6 (review)
// -- a gesture while the panel goes to another charter writes neither
// (a second charter, « autre », at the same version); with the panel key,
// Cmd+V and Delete do not reach the montage.
- (void)runUiSmokeCharterScenario {
    namespace fs = std::filesystem;
    const char* variable = "CUTMACHINE_MOTION_LIBRARY";
    const char* previous = std::getenv(variable);
    const std::optional<std::string> saved =
        previous != nullptr ? std::optional<std::string>(previous)
                            : std::nullopt;
    const fs::path library = gUiSmokeRoot / "charte-bibliotheque";
    std::error_code error;
    fs::remove_all(library, error);
    fs::create_directories(library / "kits", error);
    const fs::path kitPath = library / "kits" / "marque.json";
    const auto readFile = [&kitPath]() {
        std::ifstream input(kitPath, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        return bytes.str();
    };
    const auto writeFile = [&kitPath](const std::string& bytes) {
        std::ofstream output(kitPath, std::ios::binary | std::ios::trunc);
        output << bytes;
    };
    // A second charter, at v1 as « marque » is: a write sent to the wrong
    // one would find its version good.
    const fs::path otherPath = library / "kits" / "autre.json";
    const auto readOther = [&otherPath]() {
        std::ifstream input(otherPath, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        return bytes.str();
    };
    {
        std::ofstream output(otherPath, std::ios::binary | std::ios::trunc);
        output << "{\n"
                  "  \"format\": \"brand-kit.v1\",\n"
                  "  \"description\": \"Une autre charte d’exemple.\",\n"
                  "  \"props\": {\n"
                  "    \"accent\": \"#7A3E9D\",\n"
                  "    \"police\": \"Helvetica Neue\"\n"
                  "  }\n"
                  "}\n";
    }
    writeFile(
        "{\n"
        "  \"format\": \"brand-kit.v1\",\n"
        "  \"description\": \"Charte d’exemple, pour le test du panneau.\",\n"
        "  \"props\": {\n"
        "    \"accent\": \"#1D6FA3\",\n"
        "    \"texte\": \"#FFFFFF\",\n"
        "    \"fond\": \"#0B2233\",\n"
        "    \"police\": \"Helvetica Neue\"\n"
        "  },\n"
        "  \"fonts\": [\"Exemple Sans\"],\n"
        "  \"captions\": {\n"
        "    \"font_family\": \"Exemple Sans\",\n"
        "    \"font_size\": 56,\n"
        "    \"color\": \"#FFFFFF\",\n"
        "    \"vertical_position\": {\"num\": 3, \"den\": 4}\n"
        "  }\n"
        "}\n");
    setenv(variable, library.c_str(), 1);
    const size_t editsBefore = self.state->editLog.AppliedCount();
    const std::string documentBefore = self.state->document.SaveToString();

    [self toggleCharterPanel:nil];
    CMCharterPanel* panel = self.charterPanel;
    panel.rememberFrame = NO;
    BOOL idle = [panel waitUntilIdle:15.0];
    if ([panel selectCharterNamed:@"marque"]) idle = [panel waitUntilIdle:15.0];
    UiSmokeCheck(panel != nil && panel.shown && idle &&
                     [panel.selectedName isEqualToString:@"marque"] &&
                     panel.shownVersion == 1 && panel.writable,
                 ("CH6: the panel opens on the library's charter « marque », "
                  "read as v1 from its file (« " +
                  std::string(panel.noticeText.UTF8String ?: "") + " »)")
                     .c_str());

    // CH6 (review) -- a gesture while the panel changes charter: the list
    // asks for « autre », the panel still shows « marque », its fields and
    // its version (both v1). A colour chosen then is refused on the spot,
    // and neither file moves.
    const std::string marqueBefore = readFile();
    const std::string otherBefore = readOther();
    NSColorWell* early = (NSColorWell*)[panel controlForPath:@"props.accent"];
    const BOOL earlyWell = [early isKindOfClass:NSColorWell.class];
    const BOOL switching = [panel selectCharterNamed:@"autre"];
    if (earlyWell && switching) {
        early.color = [NSColor colorWithSRGBRed:200.0 / 255.0
                                          green:60.0 / 255.0
                                           blue:40.0 / 255.0
                                          alpha:1.0];
        [early sendAction:early.action to:early.target];
    }
    const BOOL refusedAtOnce =
        [panel.noticeText containsString:@"rien n’est écrit"];
    const std::string refusal = panel.noticeText.UTF8String ?: "";
    [panel waitUntilIdle:15.0];
    UiSmokeCheck(earlyWell && switching && refusedAtOnce &&
                     readFile() == marqueBefore && readOther() == otherBefore &&
                     [panel.selectedName isEqualToString:@"autre"] &&
                     panel.shownVersion == 1,
                 ("CH6: a colour chosen while the panel goes from « marque » "
                  "to « autre », both v1, writes neither file (« " +
                  refusal + " »)")
                     .c_str());
    if ([panel selectCharterNamed:@"marque"]) [panel waitUntilIdle:15.0];

    // A colour chosen in the accent's well: one version, the file rewritten.
    const std::string seeded = readFile();
    NSColorWell* accent = (NSColorWell*)[panel controlForPath:@"props.accent"];
    const BOOL isWell = [accent isKindOfClass:NSColorWell.class];
    if (isWell) {
        accent.color = [NSColor colorWithSRGBRed:42.0 / 255.0
                                           green:157.0 / 255.0
                                            blue:143.0 / 255.0
                                           alpha:1.0];
        [accent sendAction:accent.action to:accent.target];
        [panel waitUntilIdle:15.0];
    }
    const std::string afterAccent = readFile();
    UiSmokeCheck(
        isWell && panel.shownVersion == 2 && afterAccent != seeded &&
            afterAccent.find("\"accent\": \"#2a9d8f\"") != std::string::npos,
        ("CH6: the accent's well writes v2, and kits/marque.json "
         "follows (« " +
         std::string(panel.noticeText.UTF8String ?: "") + " »)")
            .c_str());

    // The logo dragged in 9:16, Command held: 9:16's own values.
    [panel selectFrameIndex:0];
    [panel waitUntilIdle:15.0];
    ui::charter::Quad logo{};
    CMCharterFrameView* frameView =
        (CMCharterFrameView*)[panel frameViewAtIndex:0];
    const BOOL anchored = [panel anchorOfPiece:@"logo" inFrame:0 quad:logo];
    if (anchored) {
        const ui::charter::Point centre = {
            (logo[0].first + logo[2].first) / 2.0,
            (logo[0].second + logo[2].second) / 2.0};
        const ui::charter::Point landing = {centre.first - 100.0,
                                            centre.second + 96.0};
        SendGradeGesture(frameView, [frameView viewPointFor:centre],
                         [frameView viewPointFor:landing],
                         NSEventModifierFlagCommand);
        [panel waitUntilIdle:15.0];
    }
    const std::string afterDrag = readFile();
    mcp_json::Value dragged;
    std::string parseError;
    const bool parses = mcp_json::Value::Parse(afterDrag, dragged, parseError);
    const mcp_json::Value* vertical =
        parses ? FindMotionKitPath(dragged, "pieces.logo.formats.9x16")
               : nullptr;
    UiSmokeCheck(
        anchored && panel.shownVersion == 3 && vertical != nullptr &&
            vertical->IsObject() &&
            (vertical->Find("marge_droite") != nullptr ||
             vertical->Find("marge_haut") != nullptr) &&
            FindMotionKitPath(dragged, "pieces.logo.marge_droite") == nullptr,
        ("CH6: the logo dragged in 9:16 writes v3, under "
         "pieces.logo.formats.9x16 only (« " +
         std::string(panel.noticeText.UTF8String ?: "") + " »)")
            .c_str());

    // Cmd+Z with the panel key: the file as it was before the drag, the
    // montage's history untouched.
    [panel.window makeKeyAndOrderFront:nil];
    [self spinRunLoopFor:0.3];
    const BOOL panelKey = panel.window.isKeyWindow;
    if (panelKey) {
        [NSApp
            sendEvent:[NSEvent keyEventWithType:NSEventTypeKeyDown
                                             location:NSZeroPoint
                                        modifierFlags:NSEventModifierFlagCommand
                                            timestamp:NSProcessInfo.processInfo
                                                          .systemUptime
                                         windowNumber:panel.window.windowNumber
                                              context:nil
                                           characters:@"z"
                          charactersIgnoringModifiers:@"z"
                                            isARepeat:NO
                                              keyCode:6]];
        [panel waitUntilIdle:15.0];
    }
    UiSmokeCheck(panelKey && readFile() == afterAccent &&
                     panel.shownVersion == 4 &&
                     self.state->editLog.AppliedCount() == editsBefore &&
                     self.state->document.SaveToString() == documentBefore,
                 ("CH6: Cmd+Z with the panel key gives kits/marque.json its "
                  "bytes before the drag back, as v4, and leaves the "
                  "montage's EditLog as it was (« " +
                  std::string(panel.noticeText.UTF8String ?: "") + " »)")
                     .c_str(),
                 ui_smoke::Needs::kKeyWindow);
    if (!panelKey) {
        // The keys could not be had (another application in front): the
        // panel's own undo, called as the menu calls it, so that what
        // follows still runs on the same versions.
        [panel undo];
        [panel waitUntilIdle:15.0];
        UiSmokeCheck(readFile() == afterAccent && panel.shownVersion == 4 &&
                         self.state->editLog.AppliedCount() == editsBefore,
                     "CH6: the panel's undo, called as the menu calls it, "
                     "gives the bytes before the drag back as v4");
    }
    // CH6 (review) -- the montage's commands while the panel holds the
    // keyboard, its preview the first responder: Cmd+V would have pasted
    // the clips copied in the editor window into the timeline, Delete
    // deleted the clip selected there. Neither reaches the montage, nor
    // writes a version.
    {
        [self.window makeKeyAndOrderFront:nil];
        [self spinRunLoopFor:0.2];
        Ulid clipId;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            if (clipId.empty() && !track.clips.empty())
                clipId = track.clips.front().id;
        if (!clipId.empty()) {
            self.state->interaction->SelectClips({clipId});
            [self menuCopyTimelineClips:nil];
        }
        const BOOL copied = !self.state->timelineClipboard.empty();
        const size_t editsHere = self.state->editLog.AppliedCount();
        const std::string documentHere = self.state->document.SaveToString();
        const int64_t versionHere = panel.shownVersion;
        [panel.window makeKeyAndOrderFront:nil];
        [self spinRunLoopFor:0.3];
        const BOOL keyed = panel.window.isKeyWindow &&
                           [panel.window makeFirstResponder:frameView] &&
                           panel.window.firstResponder == frameView;
        const auto press = [&](NSEventModifierFlags modifiers,
                               NSString* characters, unsigned short code) {
            [NSApp sendEvent:[NSEvent keyEventWithType:NSEventTypeKeyDown
                                                    location:NSZeroPoint
                                               modifierFlags:modifiers
                                                   timestamp:NSProcessInfo
                                                                 .processInfo
                                                                 .systemUptime
                                                windowNumber:panel.window
                                                                 .windowNumber
                                                     context:nil
                                                  characters:characters
                                 charactersIgnoringModifiers:characters
                                                   isARepeat:NO
                                                     keyCode:code]];
            [self spinRunLoopFor:0.1];
        };
        if (keyed) {
            press(NSEventModifierFlagCommand, @"v", 9);
            press(0,
                  [NSString stringWithFormat:@"%C", static_cast<unichar>(
                                                        NSDeleteCharacter)],
                  51);
            [panel waitUntilIdle:15.0];
        }
        UiSmokeCheck(copied && keyed &&
                         self.state->editLog.AppliedCount() == editsHere &&
                         self.state->document.SaveToString() == documentHere &&
                         panel.shownVersion == versionHere,
                     "CH6: with the panel key and its preview first "
                     "responder, Cmd+V and Delete leave the montage's "
                     "EditLog, and the charter, as they were",
                     ui_smoke::Needs::kKeyWindow);
        // What reached the montage anyway is taken back from the editor
        // window, so that what follows runs on the montage as it was.
        if (self.state->editLog.AppliedCount() > editsHere) {
            [self.window makeKeyAndOrderFront:nil];
            [self spinRunLoopFor:0.2];
            for (int attempt = 0;
                 attempt < 8 &&
                 self.state->editLog.AppliedCount() > editsHere &&
                 ![self.charterPanel holdsKeyboard];
                 ++attempt)
                [self menuUndo:nil];
        }
        self.state->timelineClipboard.clear();
        self.state->interaction->SelectClips({});
        [panel.window makeKeyAndOrderFront:nil];
        [self spinRunLoopFor:0.2];
    }

    // And its redo: the drag again, as v5.
    [panel redo];
    [panel waitUntilIdle:15.0];
    UiSmokeCheck(readFile() == afterDrag && panel.shownVersion == 5 &&
                     self.state->editLog.AppliedCount() == editsBefore,
                 ("CH6: the panel's redo writes the dragged charter again, "
                  "as v5 (« " +
                  std::string(panel.noticeText.UTF8String ?: "") + " »)")
                     .c_str());

    // A hand edit of the file: the panel reads it again.
    const int64_t beforeHand = panel.shownVersion;
    std::string edited = readFile();
    const size_t accentAt = edited.find("#2a9d8f");
    if (accentAt != std::string::npos) edited.replace(accentAt, 7, "#e76f51");
    writeFile(edited);
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
    while (panel.shownVersion == beforeHand &&
           deadline.timeIntervalSinceNow > 0)
        [self spinRunLoopFor:0.05];
    [panel waitUntilIdle:15.0];
    NSColorWell* followed =
        (NSColorWell*)[panel controlForPath:@"props.accent"];
    NSColor* shown =
        [followed.color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    UiSmokeCheck(accentAt != std::string::npos &&
                     panel.shownVersion == beforeHand + 1 && shown != nil &&
                     std::lround(shown.redComponent * 255.0) == 0xe7,
                 "CH6: a hand edit of kits/marque.json is read again: a "
                 "version more, the accent's well shows the colour typed");

    // A file that no longer reads: said, and every write refused.
    const std::string valid = readFile();
    const int64_t beforeBroken = panel.shownVersion;
    const std::string broken = "{\n  \"format\": \"brand-kit.v1\",\n";
    writeFile(broken);
    deadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
    while (panel.writable && deadline.timeIntervalSinceNow > 0)
        [self spinRunLoopFor:0.05];
    [panel waitUntilIdle:15.0];
    const BOOL said = !panel.writable &&
                      [panel.noticeText containsString:@"ne se lit plus"] &&
                      [panel.noticeText containsString:@"kits/marque.json"];
    NSColorWell* refused = (NSColorWell*)[panel controlForPath:@"props.accent"];
    if ([refused isKindOfClass:NSColorWell.class]) {
        refused.color = [NSColor colorWithSRGBRed:38.0 / 255.0
                                            green:70.0 / 255.0
                                             blue:83.0 / 255.0
                                            alpha:1.0];
        [refused sendAction:refused.action to:refused.target];
        [panel waitUntilIdle:15.0];
    }
    UiSmokeCheck(said && readFile() == broken &&
                     panel.shownVersion == beforeBroken &&
                     [panel.noticeText containsString:@"n’écrit rien"],
                 ("CH6: a hand edit that no longer reads is said, and a "
                  "colour chosen then writes nothing over it (« " +
                  std::string(panel.noticeText.UTF8String ?: "") + " »)")
                     .c_str());
    writeFile(valid);
    deadline = [NSDate dateWithTimeIntervalSinceNow:8.0];
    while (!panel.writable && deadline.timeIntervalSinceNow > 0)
        [self spinRunLoopFor:0.05];
    [panel waitUntilIdle:15.0];
    UiSmokeCheck(panel.writable && panel.shownVersion == beforeBroken,
                 "CH6: the file put back, the panel writes again, with no "
                 "version more");

    // capture_editor_window's "charter" region: the panel, drawn.
    EditorCaptureRequest request;
    request.region = "charter";
    EditorCapture capture;
    std::string errorName;
    std::string message;
    const bool captured = self.chatBackend->CaptureEditorWindow(
        request, capture, errorName, message);
    UiSmokeCheck(captured && !capture.jpeg.empty() && capture.width > 400 &&
                     capture.height > 300 && capture.regions.size() == 1 &&
                     capture.regions.front().name == "charter",
                 ("CH6: capture_editor_window draws the Charte panel for its "
                  "\"charter\" region: " +
                  (captured ? std::to_string(capture.width) + "x" +
                                  std::to_string(capture.height)
                            : errorName + " " + message))
                     .c_str(),
                 ui_smoke::Needs::kVisibleWindow);
    if (UiQaEnabled()) {
        const auto keep = [&](const char* name) {
            [self spinRunLoopFor:0.3];
            std::string drawError;
            NSBitmapImageRep* drawn =
                [self drawCharterWindowOffscreen:drawError];
            NSData* png =
                drawn ? [drawn representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}]
                      : nil;
            const fs::path path =
                gUiQaOutputDirectory / (std::string(name) + ".png");
            UiSmokeCheck(
                png != nil &&
                    [png
                        writeToFile:[NSString stringWithUTF8String:path.c_str()]
                         atomically:YES],
                ("CH6: the Charte panel, drawn off screen, is kept for "
                 "reading: " +
                 path.string() + (drawn ? "" : " (" + drawError + ")"))
                    .c_str());
        };
        keep("ch6-charte-9x16");
        [panel selectFrameIndex:3];
        [panel waitUntilIdle:15.0];
        keep("ch6-charte-les-trois");
    }

    [self toggleCharterPanel:nil];
    [panel waitUntilIdle:15.0];
    UiSmokeCheck(
        !panel.shown && self.state->editLog.AppliedCount() == editsBefore,
        "CH6: the panel closed, the montage as it was");
    if (saved)
        setenv(variable, saved->c_str(), 1);
    else
        unsetenv(variable);
    fs::remove_all(library, error);
    [self.window makeKeyAndOrderFront:nil];
}

// PERF12 -- DecoderPool.h, wired. At rest, the rush of a shot playback
// reaches next is opened before it gets there: on the 16:9 template
// timeline the pattern is on screen and the grey shot begins two seconds
// on. Nothing here needs the window to be key.
- (void)runUiSmokeDecodersAheadScenario {
    const Ulid mainId = self.state->activeTimelineId;
    const TimelineViewport original = self.state->viewport;
    const TimelineFit originalFit = self.state->timelineFit;
    const Ulid timelineId = gUiSmokeTemplateTimelineIds[1];
    if (![self activateTimelineIdentifier:
                   [NSString stringWithUTF8String:timelineId.c_str()]]) {
        UiSmokeCheck(false, "PERF12: the 16:9 template timeline opens");
        return;
    }
    [self requestTimelinePosition:RationalTime{0, 25}];
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while ((self.state->workers.count(gUiSmokeGreySourceId) == 0 ||
            !self.state->openingDecoders.empty()) &&
           deadline.timeIntervalSinceNow > 0)
        [self spinRunLoopFor:0.02];
    const bool greyShown = std::any_of(
        self.state->requested.begin(), self.state->requested.end(),
        [](const ResolvedSlot& slot) {
            return slot.active && slot.sourceId == gUiSmokeGreySourceId;
        });
    UiSmokeCheck(
        !greyShown && self.state->workers.count(gUiSmokePatternSourceId) != 0 &&
            self.state->workers.count(gUiSmokeGreySourceId) != 0 &&
            self.state->plannedSourceIds.count(gUiSmokeGreySourceId) != 0,
        "PERF12: at rest, the rush of the shot two seconds on is "
        "open before playback reaches it, and held");
    UiSmokeCheck(self.state->workers.size() <= self.state->decoderCapacity,
                 "PERF12: opening ahead stays within the decoder capacity");
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:mainId.c_str()]];
    // The view the checks after this one were written against, and the
    // template timeline opened later as if for the first time.
    self.state->viewport.view_start = original.view_start;
    self.state->viewport.pixels_per_second = original.pixels_per_second;
    self.state->timelineFit = originalFit;
    self.state->timelineViews.erase(timelineId);
    self.state->overlayDirty = true;
}

// VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The editor window drawn off
// screen (WindowCapture.h): the whole window at its backing scale, and each
// Metal surface in it, in place and upright, as its renderer presents it.
// None of that needs the screen -- it is the point. With the window
// composited, the drawing is held against ScreenCaptureKit's picture of it.
- (void)runUiSmokeOffscreenCaptureScenario {
    [self spinRunLoopFor:0.3];
    std::string error;
    NSBitmapImageRep* drawn = [self drawEditorWindowOffscreen:error];
    UiSmokeCheck(drawn != nil, ("VQA5: the editor window is drawn off screen" +
                                (drawn != nil ? std::string() : ": " + error))
                                   .c_str());
    if (drawn == nil) return;
    NSWindow* window = self.window;
    const CGFloat scale = window.backingScaleFactor;
    UiSmokeCheck(
        drawn.pixelsWide == std::lround(window.frame.size.width * scale) &&
            drawn.pixelsHigh == std::lround(window.frame.size.height * scale),
        "VQA5: the drawing is the whole window, title bar included, at its "
        "backing scale");
    CGImageRef offscreen = drawn.CGImage;

    struct Surface {
        const char* name;
        TimelineMetalView* view;
        Renderer* renderer;
    };
    std::vector<Surface> surfaces = {
        {"the timeline", self.metalView, self.state->renderer.get()},
        {"Record", self.programMonitorView, self.state->programRenderer.get()},
    };
    if (self.sourceMonitorView.window == window &&
        !self.sourceMonitorView.isHiddenOrHasHiddenAncestor)
        surfaces.push_back({"Source", self.sourceMonitorView,
                            self.state->sourceRenderer.get()});
    for (const Surface& surface : surfaces) {
        const CGRect rect = WindowCaptureRect(surface.view, scale);
        // What AppKit lays over the picture -- the monitor's title, a
        // caption, the motion canvas -- is no part of the renderer's.
        std::vector<CGRect> overlaid;
        for (NSView* subview in surface.view.subviews) {
            const CGRect over = WindowCaptureRect(subview, scale);
            if (!CGRectIsNull(over) && !CGRectIsEmpty(over))
                overlaid.push_back(
                    CGRectOffset(over, -rect.origin.x, -rect.origin.y));
        }
        surface.renderer->CaptureNextPresent();
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        CGImageRef presented = surface.renderer->TakePresentCapture();
        const UiCaptureAgreement agreement =
            presented == nullptr || CGRectIsEmpty(rect)
                ? UiCaptureAgreement{}
                : CompareUiCaptures(offscreen, rect, presented,
                                    CGRectMake(0, 0, CGImageGetWidth(presented),
                                               CGImageGetHeight(presented)),
                                    static_cast<size_t>(rect.size.width),
                                    static_cast<size_t>(rect.size.height), 4,
                                    overlaid);
        if (presented != nullptr) CGImageRelease(presented);
        UiSmokeCheck(agreement.compared && agreement.within >= 0.99,
                     ("VQA5: " + std::string(surface.name) +
                      " is drawn in place, upright, as its renderer presents "
                      "it: " +
                      DescribeUiCaptureAgreement(agreement))
                         .c_str());
    }

    if (UiQaEnabled()) {
        NSData* png = [drawn representationUsingType:NSBitmapImageFileTypePNG
                                          properties:@{}];
        const std::filesystem::path path =
            gUiQaOutputDirectory / "vqa5-offscreen.png";
        if (png != nil && [png writeToFile:@(path.c_str()) atomically:YES])
            gUiQaCaptures.push_back({"vqa5-offscreen", path.filename().string(),
                                     static_cast<int64_t>(drawn.pixelsWide),
                                     static_cast<int64_t>(drawn.pixelsHigh)});
    }

    // Against what WindowServer composites. The title bar is left out: its
    // material shows what lies behind the window. The glass it adds inside
    // is not in the drawing either (WindowCapture.h): 95.05 % of the content
    // within 8 codes on 8 October 2026, so the bound is on what the app
    // draws -- a panel lost or misplaced costs more than the margin -- and
    // the Metal surfaces, which hold no glass, are held tighter.
    std::string composedError;
    CGImageRef composed = gUiSmokeScreen.visible
                              ? CaptureWindowComposited(window, composedError)
                              : nullptr;
    const auto compare = [&](NSView* view, int tolerance) {
        const CGRect rect = WindowCaptureRect(view, scale);
        if (composed == nullptr || CGRectIsEmpty(rect) ||
            CGImageGetWidth(composed) !=
                static_cast<size_t>(drawn.pixelsWide) ||
            CGImageGetHeight(composed) != static_cast<size_t>(drawn.pixelsHigh))
            return UiCaptureAgreement{};
        return CompareUiCaptures(offscreen, rect, composed, rect,
                                 static_cast<size_t>(rect.size.width),
                                 static_cast<size_t>(rect.size.height),
                                 tolerance);
    };
    const UiCaptureAgreement content = compare(window.contentView, 8);
    UiSmokeCheck(content.compared && content.within >= 0.93,
                 ("VQA5: the off-screen drawing of the window's content "
                  "matches ScreenCaptureKit's: " +
                  (composed != nullptr ? DescribeUiCaptureAgreement(content)
                                       : composedError))
                     .c_str(),
                 ui_smoke::Needs::kVisibleWindow);
    for (const Surface& surface : surfaces) {
        const UiCaptureAgreement agreement = compare(surface.view, 4);
        UiSmokeCheck(
            agreement.compared && agreement.within >= 0.99,
            ("VQA5: " + std::string(surface.name) +
             " off screen matches ScreenCaptureKit's: " +
             (composed != nullptr ? DescribeUiCaptureAgreement(agreement)
                                  : composedError))
                .c_str(),
            ui_smoke::Needs::kVisibleWindow);
    }
    if (composed != nullptr) CGImageRelease(composed);
    if (gUiSmokeScreen.visible) {
        std::string qaError;
        CaptureUiQaWindow(window, "vqa5-composited", qaError);
    }
}

// PERF13 -- MonitorOcclusion.h, wired. On the QA multicam timeline, before
// any group is made of it, the grey shot on V2 covers the pattern on V1:
// both 16:9 in a 16:9 sequence, opaque. At rest both are composited and
// decoded as before; playing, the pattern is neither composited nor asked
// for, its decoder kept; stopped, it is composited again. Nothing here
// needs the window to be key.
- (void)runUiSmokeHiddenLayersScenario {
    const Ulid mainId = self.state->activeTimelineId;
    const TimelineViewport original = self.state->viewport;
    const TimelineFit originalFit = self.state->timelineFit;
    const Ulid timelineId = gUiSmokeMulticamTimelineId;
    if (![self activateTimelineIdentifier:
                   [NSString stringWithUTF8String:timelineId.c_str()]]) {
        UiSmokeCheck(false, "PERF13: the QA multicam timeline opens");
        return;
    }
    const DocumentClip* wide =
        self.state->document.FindClip(gUiSmokeMulticamPatternClipId);
    const DocumentClip* close =
        self.state->document.FindClip(gUiSmokeMulticamGreyClipId);
    const Ulid patternId = wide != nullptr ? wide->source_id : Ulid();
    const Ulid greyId = close != nullptr ? close->source_id : Ulid();
    const auto composited = [&](const Ulid& clipId) {
        return std::any_of(self.state->requested.begin(),
                           self.state->requested.end(),
                           [&](const ResolvedSlot& slot) {
                               return slot.active && slot.clipId == clipId;
                           });
    };
    [self requestTimelinePosition:RationalTime{5, 25}];
    int width = 0;
    int height = 0;
    bool alpha = false;
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while ((self.state->workers.count(patternId) == 0 ||
            self.state->workers.count(greyId) == 0 ||
            !self.state->frameCache->DescribeNearest(greyId, 5, width, height,
                                                     alpha)) &&
           deadline.timeIntervalSinceNow > 0)
        [self spinRunLoopFor:0.02];
    UiSmokeCheck(wide != nullptr && close != nullptr &&
                     composited(gUiSmokeMulticamPatternClipId) &&
                     composited(gUiSmokeMulticamGreyClipId) &&
                     self.state->hiddenSlots.empty(),
                 "PERF13: at rest, a shot under an opaque one is composited "
                 "and decoded as before");
    [self setPlaybackDirection:1];
    deadline = [NSDate dateWithTimeIntervalSinceNow:2.0];
    while (self.state->hiddenSlots.empty() &&
           self.state->playbackDirection != 0 &&
           deadline.timeIntervalSinceNow > 0)
        [self spinRunLoopFor:0.02];
    const bool hidden = self.state->hiddenSlots.size() == 1 &&
                        self.state->hiddenSlots.front().clipId ==
                            gUiSmokeMulticamPatternClipId &&
                        !composited(gUiSmokeMulticamPatternClipId) &&
                        composited(gUiSmokeMulticamGreyClipId);
    const bool kept = self.state->workers.count(patternId) != 0;
    [self setPlaybackDirection:0];
    UiSmokeCheck(hidden,
                 "PERF13: playing, the pattern under the opaque grey shot is "
                 "neither composited nor asked for");
    UiSmokeCheck(kept,
                 "PERF13: and its decoder is kept for when it shows again");
    [self requestTimelinePosition:self.state->requestedPosition];
    UiSmokeCheck(composited(gUiSmokeMulticamPatternClipId) &&
                     self.state->hiddenSlots.empty(),
                 "PERF13: stopped, it is composited again");
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:mainId.c_str()]];
    // The view the checks after this one were written against, and the
    // multicam timeline opened later as if for the first time.
    self.state->viewport.view_start = original.view_start;
    self.state->viewport.pixels_per_second = original.pixels_per_second;
    self.state->timelineFit = originalFit;
    self.state->timelineViews.erase(timelineId);
    self.state->overlayDirty = true;
}

// The active timeline, delivered once; every frame compared with a state
// is read from this one file.
- (std::filesystem::path)uiQaExport:(const std::string&)name {
    const std::filesystem::path movie = gUiQaOutputDirectory / (name + ".mp4");
    std::string error;
    const bool exported = UiQaExportMovie(
        self.state->document, self.documentPath.UTF8String ?: "", movie, error);
    UiSmokeCheck(exported, ("VQA " + name +
                            ": the exporter writes the "
                            "timeline" +
                            (exported ? "" : ": " + error))
                               .c_str());
    return exported ? movie : std::filesystem::path();
}

// Frame `frame` of `movie`, kept as `name`-export.png, as the Record layer
// would show its values.
- (bool)uiQaFileGrid:(const std::string&)name
               movie:(const std::filesystem::path&)movie
               frame:(int64_t)frame
                grid:(std::vector<float>&)grid {
    const std::filesystem::path png =
        gUiQaOutputDirectory / (name + "-export.png");
    return !movie.empty() && UiQaExtractFrame(movie, frame, png) &&
           UiQaGrid(png, CGRectNull, UiQaGridFor(self.state->document.sequence),
                    grid, UiQaReading::RecordLayer);
}

// One state of the active timeline, measured twice: the real Record
// monitor captured from the window, and frame `frame` of `movie`. The
// capture is kept as `name`.png beside `name`-export.png for whoever looks
// next. Cells under a control drawn over the monitor join `ignored`.
- (bool)uiQaMeasure:(const std::string&)name
              frame:(int64_t)frame
              movie:(const std::filesystem::path&)movie
             source:(const Ulid&)sourceId
        sourceFrame:(int64_t)sourceFrame
            ignored:(std::vector<bool>&)ignored
            monitor:(std::vector<float>&)monitor
               file:(std::vector<float>&)file {
    const DocumentSequence& sequence = self.state->document.sequence;
    const RationalTime position{frame * sequence.frame_rate.den,
                                sequence.frame_rate.num};
    [self refreshTimelineAfterEditFromPosition:position];
    [self requestTimelinePosition:position];
    // The decode is asynchronous: wait until Record has drawn exactly the
    // source frame this position shows, then let Core Animation put it on
    // screen before the window is captured.
    bool drawn = false;
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while (!drawn && deadline.timeIntervalSinceNow > 0) {
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        for (const RenderedSlot& slot : self.state->rendered)
            drawn = drawn || (slot.active && slot.sourceId == sourceId &&
                              slot.frame == sourceFrame);
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    [NSRunLoop.currentRunLoop
           runMode:NSDefaultRunLoopMode
        beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
    UiSmokeCheck(drawn, ("VQA " + name + ": Record draws source frame " +
                         std::to_string(sourceFrame))
                            .c_str());
    std::string captureError;
    // PRD28 -- the capture, and what is measured from it, need the editor
    // window composited. LG5 -- unless the scenario measures the monitor's
    // own drawing: then a covered window is drawn off screen (VQA5), and
    // the measures need nothing of the screen.
    const bool offscreen = gUiQaDrawOffscreen && !gUiSmokeScreen.visible;
    const bool captured =
        offscreen ? [self uiQaCaptureOffscreen:name error:captureError]
                  : CaptureUiQaWindow(self.window, name, captureError);
    const ui_smoke::Needs screen =
        offscreen ? ui_smoke::Needs::kNothing : ui_smoke::Needs::kVisibleWindow;
    UiSmokeCheck(
        captured,
        ("VQA " + std::string(offscreen ? "drew off screen " : "captured ") +
         name + (captured ? "" : ": " + captureError))
            .c_str(),
        screen);

    // The sequence frame inside Record, as the caption overlay places it.
    const UiQaGridShape shape = UiQaGridFor(sequence);
    const NSRect canvas = [self uiQaRecordCanvas];
    const double cellWidth = canvas.size.width / shape.columns;
    const double cellHeight = canvas.size.height / shape.rows;
    ignored.resize(static_cast<size_t>(shape.columns) * shape.rows, false);
    for (NSView* subview in self.programMonitorView.subviews) {
        if (subview.hidden) continue;
        // LG5 -- the cartons' view covers the whole frame; only what it
        // paints hides the picture.
        NSRect covered = subview.frame;
        if (subview == self.programCaptionView) {
            const CGRect share = self.programCaptionView.inkShare;
            covered =
                NSMakeRect(NSMinX(subview.frame) +
                               (share.origin.x * NSWidth(subview.frame)),
                           NSMinY(subview.frame) +
                               ((1.0 - share.origin.y - share.size.height) *
                                NSHeight(subview.frame)),
                           share.size.width * NSWidth(subview.frame),
                           share.size.height * NSHeight(subview.frame));
        }
        for (int row = 0; row < shape.rows; ++row) {
            for (int column = 0; column < shape.columns; ++column) {
                const NSRect cell =
                    NSMakeRect(NSMinX(canvas) + (column * cellWidth),
                               NSMaxY(canvas) - ((row + 1) * cellHeight),
                               cellWidth, cellHeight);
                if (NSIntersectsRect(cell, covered))
                    ignored[(static_cast<size_t>(row) * shape.columns) +
                            column] = true;
            }
        }
    }
    const CGRect crop = [self uiQaCaptureRect:canvas];
    const bool readMonitor =
        captured &&
        UiQaGrid(gUiQaOutputDirectory / (name + ".png"), crop, shape, monitor);
    const bool readFile = movie.empty() || [self uiQaFileGrid:name
                                                        movie:movie
                                                        frame:frame
                                                         grid:file];
    UiSmokeCheck(
        drawn && readMonitor && readFile,
        ("VQA " + name +
         (offscreen ? ": measured off screen" : ": measured on screen") +
         (movie.empty() ? "" : " and in the exported file"))
            .c_str(),
        screen);
    return drawn && readMonitor && readFile;
}

// LG5 -- the editor window drawn off screen (WindowCapture.h), written where
// a window capture goes: the whole window at its backing scale, the frame
// uiQaCaptureRect maps into.
- (bool)uiQaCaptureOffscreen:(const std::string&)name
                       error:(std::string&)error {
    error.clear();
    if (!UiQaEnabled()) return true;
    NSBitmapImageRep* drawn = [self drawEditorWindowOffscreen:error];
    if (drawn == nil) return false;
    NSData* png = [drawn representationUsingType:NSBitmapImageFileTypePNG
                                      properties:@{}];
    const std::filesystem::path path = gUiQaOutputDirectory / (name + ".png");
    if (png == nil ||
        ![png writeToFile:[NSString stringWithUTF8String:path.c_str()]
               atomically:YES]) {
        error = "unable to write the PNG of the window drawn off screen";
        return false;
    }
    gUiQaCaptures.push_back({name, path.filename().string(),
                             static_cast<int64_t>(drawn.pixelsWide),
                             static_cast<int64_t>(drawn.pixelsHigh)});
    return true;
}

// `rect` of the Record monitor, in the pixels of a window capture, origin
// top-left: the window has no shadow while it is captured, so the image is
// its frame at the backing scale.
- (CGRect)uiQaCaptureRect:(NSRect)rect {
    return [self uiQaCaptureRect:rect inView:self.programMonitorView];
}

- (CGRect)uiQaCaptureRect:(NSRect)rect inView:(NSView*)view {
    const NSRect inWindow = [view convertRect:rect toView:nil];
    const double scale = self.window.backingScaleFactor;
    return CGRectIntegral(
        CGRectMake(NSMinX(inWindow) * scale,
                   (NSHeight(self.window.frame) - NSMaxY(inWindow)) * scale,
                   NSWidth(inWindow) * scale, NSHeight(inWindow) * scale));
}

// The sequence frame inside Record, as the caption overlay places it.
- (NSRect)uiQaRecordCanvas {
    const DocumentSequence& sequence = self.state->document.sequence;
    const NSRect bounds = self.programMonitorView.bounds;
    const ui::caption_overlay::Layout layout = ui::caption_overlay::Compute(
        sequence.width, sequence.height, bounds.size.width, bounds.size.height,
        0.0);
    return NSMakeRect(
        layout.canvas_x,
        bounds.size.height - layout.canvas_y - layout.canvas_height,
        layout.canvas_width, layout.canvas_height);
}

// What read_frame hands a model for the frame `frame` of `clipId`, as a
// grid of its code values. The backends call these same two functions.
- (bool)uiQaReadFrame:(const Ulid&)clipId
                frame:(int64_t)frame
                 name:(const std::string&)name
                 grid:(std::vector<float>&)grid {
    const Document& document = self.state->document;
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) return false;
    const DocumentSource* source = nullptr;
    for (const DocumentSource& candidate : document.sources)
        if (candidate.id == clip->source_id) source = &candidate;
    if (source == nullptr) return false;
    const int64_t sourceFrame =
        clip->source_in.to_frames(source->rate.num, source->rate.den) +
        (frame - clip->timeline_in.to_frames(document.sequence.frame_rate.num,
                                             document.sequence.frame_rate.den));
    const RationalTime sourceTime{sourceFrame * source->rate.den,
                                  source->rate.num};
    FrameCaptureSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.color_management = document.ColorManagementFor(source->id);
    std::string message;
    std::string jpeg;
    if (!ApplyClipFramingToCapture(document, clipId, sourceTime, settings,
                                   message) ||
        !CaptureSourceFrame(source->path, sourceTime, settings, jpeg, message))
        return false;
    const std::filesystem::path path =
        gUiQaOutputDirectory / (name + "-read_frame.jpg");
    std::ofstream(path, std::ios::binary) << jpeg;
    return UiQaGrid(path, CGRectNull, UiQaGridFor(document.sequence), grid,
                    UiQaReading::Raw);
}

// What Record shows now: the topmost video layer it last drew.
- (UiQaShown)uiQaShownNow {
    UiQaShown shown;
    for (const RenderedSlot& slot : self.state->rendered)
        if (slot.active) shown = {slot.clipId, slot.frame};
    return shown;
}

// Puts the playhead on `frame` and waits until Record has drawn exactly
// what the monitor resolved there. Empty when it never does.
- (UiQaShown)uiQaShowFrame:(int64_t)frame {
    const MediaRate rate = self.state->document.sequence.frame_rate;
    [self requestTimelinePosition:{frame * rate.den, rate.num}];
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:3.0];
    while (deadline.timeIntervalSinceNow > 0) {
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        const std::vector<ResolvedSlot>& requested = self.state->requested;
        const std::vector<RenderedSlot>& rendered = self.state->rendered;
        bool drawn = rendered.size() == requested.size();
        for (size_t slot = 0; drawn && slot < requested.size(); ++slot)
            drawn = requested[slot].active == rendered[slot].active &&
                    (!requested[slot].active ||
                     (requested[slot].sourceId == rendered[slot].sourceId &&
                      requested[slot].frame == rendered[slot].frame &&
                      requested[slot].clipId == rendered[slot].clipId));
        if (drawn) return [self uiQaShownNow];
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return {};
}

// Real playback from `frame`, forwards or backwards at 1x, driven by the
// window's own display timer until it stops at an end of the timeline.
// Every change of the image Record draws is recorded with its time.
- (std::vector<UiQaPresented>)uiQaPlay:(int)direction from:(int64_t)frame {
    std::vector<UiQaPresented> trace;
    if ([self uiQaShowFrame:frame].clip.empty()) return trace;
    const MediaRate rate = self.state->document.sequence.frame_rate;
    const auto position = [&] {
        return self.state->requestedPosition.to_frames(rate.num, rate.den);
    };
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    [self setPlaybackDirection:direction];
    trace.push_back({0.0, [self uiQaShownNow], position()});
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:30.0];
    NSDate* settle = nil;
    while (deadline.timeIntervalSinceNow > 0) {
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
        const UiQaShown now = [self uiQaShownNow];
        if (now != trace.back().shown)
            trace.push_back({elapsed(), now, position()});
        // Stopped at an end: a few more ticks for its last image.
        if (self.state->playbackDirection == 0 && settle == nil)
            settle = [NSDate dateWithTimeIntervalSinceNow:0.2];
        if (settle != nil && settle.timeIntervalSinceNow <= 0) break;
    }
    [self setPlaybackDirection:0];
    return trace;
}

// `tool` (contact_sheet or cut_sheet) asked through the chat panel's own
// catalog on the active timeline. Each cell names a clip, a source frame
// and a position: Record must draw that source frame at that position,
// and the cell must show the picture `frames` (the exported file, frame by
// frame, read as code values) holds there.
- (void)uiQaSheet:(const std::string&)tool
             name:(const std::string&)name
           frames:(const std::vector<std::vector<float>>&)frames {
    const McpToolRegistry registry;
    const McpToolCallOutcome outcome =
        registry.Call(*self.chatBackend, tool, mcp_json::Value::MakeObject());
    NSData* jpeg =
        outcome.ok
            ? [[NSData alloc]
                  initWithBase64EncodedString:
                      [NSString
                          stringWithUTF8String:outcome.image_base64.c_str()]
                                      options:0]
            : nil;
    mcp_json::Value plan;
    std::string parseError;
    const bool parsed =
        jpeg != nil &&
        mcp_json::Value::Parse(outcome.result_json, plan, parseError) &&
        plan.Find("frames") != nullptr && plan.Find("frames")->IsArray();
    UiSmokeCheck(parsed, ("VQA " + name + ": " + tool +
                          " answers with a picture and its cells" +
                          (outcome.ok ? "" : ": " + outcome.message))
                             .c_str());
    if (!parsed) return;
    const std::filesystem::path sheet =
        gUiQaOutputDirectory / (name + "-" + tool + ".jpg");
    [jpeg writeToFile:[NSString stringWithUTF8String:sheet.c_str()]
           atomically:YES];
    const DocumentSequence& sequence = self.state->document.sequence;
    const UiQaGridShape shape = UiQaGridFor(sequence);
    // TimelineSheets.h: compact cells with a 320-pixel long edge, four
    // columns at most. The cell is the fitted sequence frame itself; since
    // R1 it has no surrounding square to crop away.
    const TimelineSheetSettings settings;
    int64_t frameWidth = 0;
    int64_t frameHeight = 0;
    BoundedFrameSize(sequence.width, sequence.height, settings.cell_dimension,
                     frameWidth, frameHeight);
    frameWidth = std::max<int64_t>(2, frameWidth & ~int64_t{1});
    frameHeight = std::max<int64_t>(2, frameHeight & ~int64_t{1});
    const std::vector<mcp_json::Value>& cells = plan.Find("frames")->AsArray();
    const int64_t columns =
        std::min<int64_t>(settings.columns, static_cast<int64_t>(cells.size()));
    std::vector<std::vector<float>> grids;
    std::vector<int64_t> positions;
    const std::vector<bool> none(
        static_cast<size_t>(shape.columns) * shape.rows, false);
    for (size_t index = 0; index < cells.size(); ++index) {
        const mcp_json::Value& entry = cells[index];
        const mcp_json::Value* clip = entry.Find("clip_id");
        const mcp_json::Value* at = entry.Find("timeline_position");
        const mcp_json::Value* in = entry.Find("source_in");
        int64_t value = 0;
        int64_t valueRate = 0;
        int64_t source = 0;
        int64_t sourceRate = 0;
        const bool described =
            clip != nullptr && at != nullptr && in != nullptr &&
            at->Find("value") && at->Find("value")->AsInt64(value) &&
            at->Find("rate") && at->Find("rate")->AsInt64(valueRate) &&
            in->Find("value") && in->Find("value")->AsInt64(source) &&
            in->Find("rate") && in->Find("rate")->AsInt64(sourceRate) &&
            valueRate > 0 && sourceRate > 0;
        if (!described) {
            UiSmokeCheck(false, ("VQA " + name + ": " + tool + " cell " +
                                 std::to_string(index) + " is described")
                                    .c_str());
            continue;
        }
        const int64_t frame =
            RationalTime(value, static_cast<int32_t>(valueRate))
                .to_frames(sequence.frame_rate.num, sequence.frame_rate.den);
        const DocumentClip* shown =
            self.state->document.FindClip(clip->AsString());
        const DocumentSource* media = nullptr;
        for (const DocumentSource& candidate : self.state->document.sources)
            if (shown != nullptr && candidate.id == shown->source_id)
                media = &candidate;
        const int64_t sourceFrame =
            media != nullptr
                ? RationalTime(source, static_cast<int32_t>(sourceRate))
                      .to_frames(media->rate.num, media->rate.den)
                : -1;
        const UiQaShown drawn = [self uiQaShowFrame:frame];
        UiSmokeCheck(
            drawn.clip == clip->AsString() && drawn.source == sourceFrame,
            ("VQA " + name + ": " + tool + " cell " + std::to_string(index) +
             " names source frame " + std::to_string(sourceFrame) +
             " at frame " + std::to_string(frame) + ", Record draws " +
             std::to_string(drawn.source) + " there")
                .c_str());
        std::vector<float> grid;
        const CGRect crop =
            CGRectMake((static_cast<int64_t>(index) % columns) * frameWidth,
                       (static_cast<int64_t>(index) / columns) * frameHeight,
                       frameWidth, frameHeight);
        if (!UiQaGrid(sheet, crop, shape, grid, UiQaReading::Raw) ||
            frame < 0 || frame >= static_cast<int64_t>(frames.size())) {
            UiSmokeCheck(false, ("VQA " + name + ": " + tool + " cell " +
                                 std::to_string(index) + " is read")
                                    .c_str());
            continue;
        }
        UiQaExpectSame(name + " " + tool + " cell " + std::to_string(index) +
                           " (frame " + std::to_string(frame) +
                           "): the picture the file holds there",
                       grid, frames[frame], none);
        grids.push_back(std::move(grid));
        positions.push_back(frame);
    }
    if (grids.size() >= 2)
        UiQaExpectDifferent(
            name + " " + tool + " cell 0, the file at cell " + "1's frame",
            grids[0], frames[positions[1]], none);
}

- (void)uiQaDiscard:(const std::filesystem::path&)movie {
    std::error_code error;
    if (!movie.empty()) std::filesystem::remove(movie, error);
}

// R1: what list_clip_framing, asked through the chat's catalog, says the
// fitted shot leaves black, against the black bands of the file's frame.
- (void)uiQaCheckUncoveredEdges:(const std::string&)name {
    const McpToolRegistry registry;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("clip_id",
                  mcp_json::Value::MakeString(gUiSmokeFramingClipId));
    const McpToolCallOutcome outcome =
        registry.Call(*self.chatBackend, "list_clip_framing", arguments);
    mcp_json::Value answer;
    std::string parseError;
    const mcp_json::Value* uncovered = nullptr;
    if (outcome.ok &&
        mcp_json::Value::Parse(outcome.result_json, answer, parseError) &&
        answer.Find("geometry") != nullptr)
        uncovered = answer.Find("geometry")->Find("uncovered");
    const char* edgeNames[4] = {"left", "top", "right", "bottom"};
    int64_t announced[4] = {-1, -1, -1, -1};
    for (int edge = 0; uncovered != nullptr && edge < 4; ++edge)
        if (const mcp_json::Value* value = uncovered->Find(edgeNames[edge]))
            value->AsInt64(announced[edge]);
    int64_t seen[4] = {-1, -1, -1, -1};
    const bool measured =
        UiQaBlackEdges(gUiQaOutputDirectory / (name + "-export.png"), seen);
    UiSmokeCheck(uncovered != nullptr && measured,
                 ("VQA R1 " + name +
                  ": list_clip_framing answers and the file's frame is read" +
                  (outcome.ok ? "" : ": " + outcome.message))
                     .c_str());
    if (uncovered == nullptr || !measured) return;
    std::string report;
    bool agree = announced[1] > 0 && announced[3] > 0;
    for (int edge = 0; edge < 4; ++edge) {
        report += std::string(edge == 0 ? "" : ", ") + edgeNames[edge] + " " +
                  std::to_string(announced[edge]) + " announced / " +
                  std::to_string(seen[edge]) + " in the file";
        // Two pixels for the scaler's ringing along the picture's edge.
        agree = agree && std::llabs(announced[edge] - seen[edge]) <= 2;
    }
    UiSmokeCheck(agree, ("VQA R1 " + name +
                         ": the black bands list_clip_framing announces are "
                         "the ones the file shows (" +
                         report + ")")
                            .c_str());
}

// R1: contact_sheet and read_frame, both asked through the chat's catalog,
// on a cropped framing, against the file frame by frame.
- (void)uiQaCheckFramedReads:(const std::string&)name
                       movie:(const std::filesystem::path&)movie {
    std::vector<std::vector<float>> frames;
    if (!UiQaFileFrames(movie, gUiQaOutputDirectory / (name + "-frames"),
                        UiQaGridFor(self.state->document.sequence), frames)) {
        UiSmokeCheck(false, ("VQA R1 " + name +
                             ": the file is read frame by "
                             "frame")
                                .c_str());
        return;
    }
    [self uiQaSheet:"contact_sheet" name:name frames:frames];
    const McpToolRegistry registry;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("clip_id",
                  mcp_json::Value::MakeString(gUiSmokeFramingClipId));
    arguments.Set("position", mcp_json::Value::MakeString("Middle"));
    const McpToolCallOutcome outcome =
        registry.Call(*self.chatBackend, "read_frame", arguments);
    NSData* jpeg =
        outcome.ok
            ? [[NSData alloc]
                  initWithBase64EncodedString:
                      [NSString
                          stringWithUTF8String:outcome.image_base64.c_str()]
                                      options:0]
            : nil;
    const std::filesystem::path path =
        gUiQaOutputDirectory / (name + "-read_frame-tool.jpg");
    [jpeg writeToFile:[NSString stringWithUTF8String:path.c_str()]
           atomically:YES];
    std::vector<float> grid;
    const bool read =
        jpeg != nil &&
        UiQaGrid(path, CGRectNull, UiQaGridFor(self.state->document.sequence),
                 grid, UiQaReading::Raw);
    // The shot is 50 frames from the timeline's start: its middle is 25.
    UiSmokeCheck(read && frames.size() > 75,
                 ("VQA R1 " + name + ": read_frame answers at the middle" +
                  (outcome.ok ? "" : ": " + outcome.message))
                     .c_str());
    if (!read || frames.size() <= 75) return;
    const std::vector<bool> none(grid.size() / 3, false);
    UiQaExpectSame("R1 " + name +
                       " read_frame at the middle: the file's "
                       "frame 25",
                   grid, frames[25], none);
    UiQaExpectDifferent("R1 " + name +
                            " read_frame at the middle, the file's "
                            "frame 75",
                        grid, frames[75], none);
}

// R1: does a zoom deform? The source region list_clip_framing announces,
// cut out of the pattern by FFmpeg and scaled to the frame, must be what
// the file holds; the two scales that region implies say how far from
// uniform the zoom is. The region is widened to the even grid a subsampled
// plane can be cut on (ClipFraming.h), which on a 320-pixel pattern is
// worth a few per cent, and on a 1080p rush a few tenths of one.
- (void)uiQaCheckZoomShape:(const std::string&)name frame:(int64_t)frame {
    const McpToolRegistry registry;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("clip_id",
                  mcp_json::Value::MakeString(gUiSmokeFramingClipId));
    const McpToolCallOutcome outcome =
        registry.Call(*self.chatBackend, "list_clip_framing", arguments);
    mcp_json::Value answer;
    std::string parseError;
    const mcp_json::Value* region = nullptr;
    if (outcome.ok &&
        mcp_json::Value::Parse(outcome.result_json, answer, parseError) &&
        answer.Find("geometry") != nullptr)
        region = answer.Find("geometry")->Find("source_rect");
    int64_t box[4] = {0, 0, 0, 0};
    const char* keys[4] = {"x", "y", "width", "height"};
    bool described = region != nullptr;
    for (int index = 0; described && index < 4; ++index)
        described = region->Find(keys[index]) != nullptr &&
                    region->Find(keys[index])->AsInt64(box[index]);
    const DocumentSequence& sequence = self.state->document.sequence;
    // Decoded as BT.709, as the export reads the pattern's values.
    const UiQaGridShape shape = UiQaGridFor(sequence);
    const std::filesystem::path png =
        gUiQaOutputDirectory / (name + "-reference.png");
    const std::string command =
        UiQaQuote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -y -i " +
        UiQaQuote((gUiSmokeRoot / "mire.mov").string()) +
        " -vf 'select=eq(n\\," + std::to_string(frame) +
        "),crop=" + std::to_string(box[2]) + ":" + std::to_string(box[3]) +
        ":" + std::to_string(box[0]) + ":" + std::to_string(box[1]) +
        ",scale=" + std::to_string(sequence.width) + ":" +
        std::to_string(sequence.height) +
        ":flags=lanczos:in_color_matrix=bt709' -frames:v 1 " +
        UiQaQuote(png.string());
    std::vector<float> file;
    std::vector<float> reference;
    const bool read =
        described && box[2] > 0 && box[3] > 0 &&
        std::system(command.c_str()) == 0 &&
        UiQaGrid(png, CGRectNull, shape, reference, UiQaReading::Raw) &&
        UiQaGrid(gUiQaOutputDirectory / (name + "-export.png"), CGRectNull,
                 shape, file, UiQaReading::Raw);
    UiSmokeCheck(read, ("VQA R1 " + name +
                        ": list_clip_framing names the region read, and it is "
                        "cut out of the pattern" +
                        (outcome.ok ? "" : ": " + outcome.message))
                           .c_str());
    if (!read) return;
    const std::vector<bool> none(file.size() / 3, false);
    UiQaExpectSame("R1 " + name +
                       ": the file draws exactly the region "
                       "list_clip_framing announces",
                   file, reference, none);
    const double horizontal = double(sequence.width) / double(box[2]);
    const double vertical = double(sequence.height) / double(box[3]);
    UiQaExpectValue("R1 " + name +
                        ": horizontal over vertical scale, the "
                        "deformation of a 320-pixel pattern",
                    horizontal / vertical, 0.965, 1.035);
}

// R1: one 16:9 pattern framed seven ways in a 1080x1920 sequence, a
// rotated portrait rush filled, and read_frame against the export.
- (void)runUiQaFramingScenario {
    if (![self uiQaOpen:gUiSmokeFramingTimelineId label:"R1"]) return;
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeFramingClipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    constexpr int64_t kFrame = 12;
    struct Variant {
        const char* name;
        FramingMode mode;
        EffectParamValue scale;
        EffectParamValue position_x;
        EffectParamValue crop_left;
        EffectParamValue crop_top;
    };
    const Variant variants[] = {
        {"40-r1-fill", FramingMode::Fill, {1, 1}, {0, 1}, {0, 1}, {0, 1}},
        {"41-r1-fill-left", FramingMode::Fill, {1, 1}, {-1, 4}, {0, 1}, {0, 1}},
        {"42-r1-fill-right", FramingMode::Fill, {1, 1}, {1, 4}, {0, 1}, {0, 1}},
        {"43-r1-fill-zoom",
         FramingMode::Fill,
         {29, 20},
         {0, 1},
         {0, 1},
         {0, 1}},
        {"44-r1-fit", FramingMode::Fit, {1, 1}, {0, 1}, {0, 1}, {0, 1}},
        {"45-r1-fill-11-10",
         FramingMode::Fill,
         {11, 10},
         {0, 1},
         {0, 1},
         {0, 1}},
        {"46-r1-fill-crop", FramingMode::Fill, {1, 1}, {0, 1}, {1, 4}, {1, 5}},
    };
    constexpr size_t kVariants = std::size(variants);
    std::vector<float> monitor[kVariants];
    std::vector<float> file[kVariants];
    std::vector<bool> ignored;
    std::vector<float> readFrame;
    std::vector<float> readFrameFile;
    bool measured = true;
    for (size_t index = 0; index < kVariants; ++index) {
        const Variant& variant = variants[index];
        SetClipFramingOperation operation;
        ClipFramingAssignment assignment;
        assignment.clip_id = gUiSmokeFramingClipId;
        assignment.framing.mode = variant.mode;
        assignment.framing.scale = variant.scale;
        assignment.framing.position_x = variant.position_x;
        assignment.framing.crop_left = variant.crop_left;
        assignment.framing.crop_top = variant.crop_top;
        operation.clips = {assignment};
        const std::string name = variant.name;
        [self uiQaApply:operation
                  label:"R1 " + name +
                        ": the framing applies through the "
                        "edit log"];
        const std::filesystem::path movie = [self uiQaExport:name];
        measured = [self uiQaMeasure:name
                               frame:kFrame
                               movie:movie
                              source:sourceId
                         sourceFrame:kFrame
                             ignored:ignored
                             monitor:monitor[index]
                                file:file[index]] &&
                   measured;
        // R1's read_frame case, on a framing that moves the picture.
        if (index == 1) {
            const bool read =
                [self uiQaReadFrame:gUiSmokeFramingClipId
                              frame:kFrame
                               name:name
                               grid:readFrame] &&
                UiQaGrid(gUiQaOutputDirectory / (name + "-export.png"),
                         CGRectNull, UiQaGridFor(self.state->document.sequence),
                         readFrameFile, UiQaReading::Raw);
            UiSmokeCheck(read, "VQA R1: read_frame answers for the framed "
                               "clip");
            measured = read && measured;
        }
        if (index == 3 || index == 5)
            [self uiQaCheckZoomShape:name frame:kFrame];
        if (index == 4) [self uiQaCheckUncoveredEdges:name];
        if (index == 6) [self uiQaCheckFramedReads:name movie:movie];
        [self uiQaDiscard:movie];
    }
    UiSmokeCheck(measured,
                 "VQA R1: every state is measured on screen and in the "
                 "exported file",
                 ui_smoke::Needs::kVisibleWindow);
    if (!measured) return;
    for (size_t index = 0; index < kVariants; ++index)
        UiQaExpectSame(std::string(variants[index].name) +
                           ": Record shows what the file holds",
                       monitor[index], file[index], ignored);
    UiQaExpectDifferent("R1 left framing on screen, right one in the file",
                        monitor[1], file[2], ignored);
    UiQaExpectDifferent("R1 fill on screen, fit in the file", monitor[0],
                        file[4], ignored);
    UiQaExpectDifferent("R1 29/20 zoom on screen, no zoom in the file",
                        monitor[3], file[0], ignored);
    UiQaExpectDifferent("R1 11/10 zoom on screen, no zoom in the file",
                        monitor[5], file[0], ignored);
    UiQaExpectDifferent("R1 cropped fill on screen, uncropped fill in the "
                        "file",
                        monitor[6], file[0], ignored);
    std::vector<bool> none(ignored.size(), false);
    UiQaExpectSame("R1 41-r1-fill-left: read_frame composes what the file "
                   "holds",
                   readFrame, readFrameFile, none);
    UiQaExpectDifferent("R1 read_frame of the left framing, the right one "
                        "in the file",
                        readFrame, file[2], none);
    size_t kept = 0;
    for (bool cell : ignored) kept += cell ? 0 : 1;
    UiSmokeCheck(kept * 2 >= ignored.size(),
                 ("VQA R1: " + std::to_string(kept) + " of " +
                  std::to_string(ignored.size()) +
                  " cells are compared, controls over the monitor excluded")
                     .c_str());

    // A portrait rush stored on its side with a 90 degree display matrix,
    // filled into the vertical frame.
    const DocumentClip* upright =
        self.state->document.FindClip(gUiSmokeRotatedClipId);
    if (upright == nullptr) return;
    const Ulid rotatedSource = upright->source_id;
    SetClipFramingOperation fill;
    ClipFramingAssignment rotated;
    rotated.clip_id = gUiSmokeRotatedClipId;
    rotated.framing.mode = FramingMode::Fill;
    fill.clips = {rotated};
    [self uiQaApply:fill label:"R1 47-r1-portrait-90: the rotated shot fills"];
    const std::filesystem::path movie = [self uiQaExport:"47-r1-portrait-90"];
    std::vector<float> rotatedMonitor;
    std::vector<float> rotatedFile;
    if ([self uiQaMeasure:"47-r1-portrait-90"
                    frame:62
                    movie:movie
                   source:rotatedSource
              sourceFrame:24
                  ignored:ignored
                  monitor:rotatedMonitor
                     file:rotatedFile]) {
        UiQaExpectSame("47-r1-portrait-90: Record shows what the file holds",
                       rotatedMonitor, rotatedFile, ignored);
        UiQaExpectDifferent("R1 rotated shot on screen, filled waves in the "
                            "file",
                            rotatedMonitor, file[0], ignored);
    }
    [self uiQaDiscard:movie];
}

// R3's second case: a 50 i/s pattern at 1/2 in a 25 i/s sequence, stopped
// at five positions spread over the shot. At 1/2 each sequence frame k
// shows source frame k (ClipSpeed.h), which is what Record must draw.
- (void)runUiQaSpeedScenario {
    if (![self uiQaOpen:gUiSmokeSpeedTimelineId label:"R3"]) return;
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeSpeedClipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    SetClipSpeedOperation operation;
    operation.clip_id = gUiSmokeSpeedClipId;
    operation.speed = {1, 2};
    if (![self uiQaApply:operation
                   label:"R3: the clip is set to 1/2 through the edit log"])
        return;
    const std::filesystem::path movie = [self uiQaExport:"50-r3-half"];
    constexpr int64_t kPositions[] = {2, 13, 24, 35, 47};
    constexpr size_t kCount = std::size(kPositions);
    std::vector<float> monitor[kCount];
    std::vector<float> file[kCount];
    std::vector<bool> ignored;
    bool measured = true;
    for (size_t index = 0; index < kCount; ++index) {
        measured = [self uiQaMeasure:"5" + std::to_string(index) + "-r3-half-" +
                                     std::to_string(kPositions[index])
                               frame:kPositions[index]
                               movie:movie
                              source:sourceId
                         sourceFrame:kPositions[index]
                             ignored:ignored
                             monitor:monitor[index]
                                file:file[index]] &&
                   measured;
    }
    [self uiQaDiscard:movie];
    if (!measured) return;
    for (size_t index = 0; index < kCount; ++index)
        UiQaExpectSame("R3 frame " + std::to_string(kPositions[index]) +
                           ": Record shows what the file holds",
                       monitor[index], file[index], ignored);
    UiQaExpectDifferent("R3 first position on screen, last one in the file",
                        monitor[0], file[kCount - 1], ignored);
}

// The first view of `root`'s tree that `match` accepts, depth first.
static NSView* UiQaFindView(NSView* root, BOOL (^match)(NSView*)) {
    if (match(root)) return root;
    for (NSView* child in root.subviews)
        if (NSView* found = UiQaFindView(child, match)) return found;
    return nil;
}

// F2.2: the Inspector in each selection state, then a grade taken back to
// neutral by a double click and by "Réinitialiser l'étalonnage", measured
// on screen against the ungraded shot in the file. Runs first, while the
// main timeline -- the one with a sound clip -- is still the active one.
- (void)runUiQaInspectorScenario {
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    NSView* inspector = self.inspectorView;
    NSTextField* placeholder =
        (NSTextField*)UiQaFindView(inspector, ^BOOL(NSView* view) {
          return [view isKindOfClass:NSTextField.class] &&
                 [((NSTextField*)view).stringValue
                     isEqualToString:@"Aucun clip sélectionné"];
        });
    NSButton* reset = (NSButton*)UiQaFindView(inspector, ^BOOL(NSView* view) {
      return [view isKindOfClass:NSButton.class] &&
             [((NSButton*)view).title
                 isEqualToString:@"Réinitialiser l'étalonnage"];
    });
    NSMutableArray<NSSlider*>* sliders = [NSMutableArray array];
    CollectSliders(inspector, sliders);
    // The opacity, the blur (EF3) and the grade, as the main run counts
    // them: the blur had left this count one short since 30 October 2026.
    UiSmokeCheck(placeholder != nil && reset != nil &&
                     sliders.count == ui::inspector::GradeControls().size() + 2,
                 "VQA F2.2: the Inspector's placeholder, reset button and "
                 "sliders are found");
    if (placeholder == nil || reset == nil || sliders.count == 0) return;
    const auto enabledSliders = [&] {
        size_t enabled = 0;
        for (NSSlider* slider in sliders) enabled += slider.enabled ? 1 : 0;
        return enabled;
    };
    std::optional<Ulid> sound;
    std::vector<Ulid> pictures;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        for (const DocumentClip& clip : track.clips) {
            if (track.kind == "audio" && !sound) sound = clip.id;
            if (track.kind == "video" && !IsMotionClip(clip))
                pictures.push_back(clip.id);
        }
    // Nothing selected: the placeholder, and no property rows.
    self.state->interaction->SelectClip("");
    [self updateSelectionInfo];
    UiSmokeCheck(!placeholder.hidden,
                 "VQA F2.2: with no clip selected, the Inspector shows its "
                 "placeholder");
    // A sound clip: no picture control can be moved.
    if (sound) {
        self.state->interaction->SelectClip(*sound);
        [self updateSelectionInfo];
        UiSmokeCheck(
            placeholder.hidden && enabledSliders() == 0 && !reset.enabled,
            ("VQA F2.2: on a sound clip, opacity and every grading "
             "slider are disabled (" +
             std::to_string(enabledSliders()) + " enabled)")
                .c_str());
    } else {
        UiSmokeCheck(false, "VQA F2.2: the main timeline has a sound clip");
    }
    // Several clips: no crash, the Inspector shows the first of them.
    if (pictures.size() >= 2) {
        self.state->interaction->SelectClips({pictures[0], pictures[1]});
        [self updateSelectionInfo];
        UiSmokeCheck(placeholder.hidden && enabledSliders() == sliders.count,
                     "VQA F2.2: with two video clips selected, the Inspector "
                     "shows one, every control live, and nothing crashes");
    }
    self.state->interaction->SelectClip("");
    [self updateSelectionInfo];

    // A grade on the first shot of the dip timeline, then back to neutral.
    if (![self uiQaOpen:gUiSmokeDipTimelineId label:"F2.2 grade"]) return;
    const Ulid clipId = gUiSmokeDipFirstClipId;
    const DocumentClip* shot = self.state->document.FindClip(clipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    const std::filesystem::path bare = [self uiQaExport:"a0-f22-bare"];
    self.state->interaction->SelectClip(clipId);
    [self updateSelectionInfo];
    // The grade's sliders by their action, not by a range other than the
    // opacity's: the blur's slider has one too, and taken for a grade it
    // left the shot blurred past « Réinitialiser l'étalonnage », failing
    // this scenario and refusing R2's shake on the same shot (QA of 18
    // November 2026).
    NSMutableArray<NSSlider*>* grades = [NSMutableArray array];
    for (NSSlider* slider in sliders)
        if (slider.action == NSSelectorFromString(@"cmGradeSliderChanged:"))
            [grades addObject:slider];
    const auto settle = [] {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:0.25];
        while (deadline.timeIntervalSinceNow > 0.0)
            [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                   beforeDate:deadline];
    };
    const auto move = [&](NSSlider* slider, double fraction) {
        slider.doubleValue =
            slider.minValue + ((slider.maxValue - slider.minValue) * fraction);
        [slider sendAction:slider.action to:slider.target];
        settle();
    };
    std::vector<bool> ignored;
    std::vector<float> ungraded;
    std::vector<float> graded;
    std::vector<float> doubleClicked;
    std::vector<float> resetShown;
    std::vector<float> file;
    if (grades.count < 2 || bare.empty()) return;
    NSSlider* first = grades[0];
    NSSlider* second = grades[1];
    move(first, 1.0);
    const bool gradedMeasured = [self uiQaMeasure:"a1-f22-graded"
                                            frame:12
                                            movie:bare
                                           source:sourceId
                                      sourceFrame:12
                                          ignored:ignored
                                          monitor:graded
                                             file:ungraded];
    // A double click, delivered to the slider as AppKit delivers one.
    const NSPoint centre = [first
        convertPoint:NSMakePoint(NSMidX(first.bounds), NSMidY(first.bounds))
              toView:nil];
    NSEvent* doubleClick =
        [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                           location:centre
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:self.window.windowNumber
                            context:nil
                        eventNumber:0
                         clickCount:2
                           pressure:1.0];
    [first mouseDown:doubleClick];
    settle();
    const DocumentClip* afterDouble = self.state->document.FindClip(clipId);
    const ui::inspector::GradeControlSpec& firstSpec =
        ui::inspector::GradeControls()[static_cast<size_t>(first.tag)];
    const bool neutral =
        afterDouble != nullptr &&
        std::abs(ui::inspector::CurrentGradeControlValue(afterDouble->effects,
                                                         firstSpec) -
                 ui::inspector::NeutralGradeControlValue(firstSpec)) < 1e-4;
    UiSmokeCheck(neutral, "VQA F2.2: a double click brings the slider's "
                          "value back to its neutral");
    const bool doubleMeasured = [self uiQaMeasure:"a2-f22-double-click"
                                            frame:12
                                            movie:bare
                                           source:sourceId
                                      sourceFrame:12
                                          ignored:ignored
                                          monitor:doubleClicked
                                             file:file];
    // Two sliders moved, then the reset button: one edit takes all back.
    // PRD37 -- ROADMAP.md. Pressed as a hand presses it: -performClick:
    // does nothing on a disabled button, where -sendAction:to: ignored its
    // state -- this check passed on a button that had never turned on.
    move(first, 0.8);
    move(second, 0.2);
    UiSmokeCheck(reset.enabled,
                 "VQA F2.2: two sliders' edits turn « Réinitialiser "
                 "l'étalonnage » on");
    const size_t before = self.state->editLog.AppliedCount();
    [reset performClick:nil];
    settle();
    const DocumentClip* afterReset = self.state->document.FindClip(clipId);
    UiSmokeCheck(afterReset != nullptr &&
                     ui::inspector::WithoutGrade(afterReset->effects).size() ==
                         afterReset->effects.size() &&
                     self.state->editLog.AppliedCount() == before + 1 &&
                     !reset.enabled,
                 "VQA F2.2: « Réinitialiser l'étalonnage » takes both "
                 "sliders back in one edit, and disables itself");
    const bool resetMeasured = [self uiQaMeasure:"a3-f22-reset"
                                           frame:12
                                           movie:bare
                                          source:sourceId
                                     sourceFrame:12
                                         ignored:ignored
                                         monitor:resetShown
                                            file:file];
    if (gradedMeasured && doubleMeasured && resetMeasured) {
        UiQaExpectDifferent("F2.2 graded shot on screen, ungraded in the file",
                            graded, ungraded, ignored);
        UiQaExpectSame("F2.2 after the double click, Record shows the "
                       "ungraded shot the file holds",
                       doubleClicked, ungraded, ignored);
        UiQaExpectSame("F2.2 after the reset, Record shows the ungraded "
                       "shot the file holds",
                       resetShown, ungraded, ignored);
    }
    self.state->interaction->SelectClip("");
    [self updateSelectionInfo];
    [self uiQaDiscard:bare];
}

// M4: a motion clip over the pattern, opaque, at 50 %, then with 5 frames
// trimmed off its head: Record against the file each time.
- (void)runUiQaMotionScenario {
    if (![self uiQaOpen:gUiSmokeMotionTimelineId label:"M4"]) return;
    Ulid patternId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && !track.clips.empty() && patternId.empty())
            patternId = track.clips.front().source_id;
    // The pattern alone, frame 10, before anything is laid over it.
    std::vector<float> bare10;
    const std::filesystem::path bareMovie = [self uiQaExport:"b0-m4-bare"];
    const bool readBare10 = [self uiQaFileGrid:"b0-m4-bare-10"
                                         movie:bareMovie
                                         frame:10
                                          grid:bare10];
    [self uiQaDiscard:bareMovie];
    InsertMotionClipOperation insert;
    insert.track_id = gUiSmokeMotionTrackId;
    insert.composition_id = gUiSmokeMotionCompositionId;
    insert.source_in = {0, 25};
    insert.duration = {40, 25};
    insert.timeline_in = {0, 25};
    if (![self uiQaApply:insert label:"M4: the sliding box is laid on V2"])
        return;
    Ulid motionId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.id == gUiSmokeMotionTrackId && !track.clips.empty())
            motionId = track.clips.front().id;
    const auto measure =
        [&](const std::string& name, int64_t frame,
            const std::filesystem::path& movie, std::vector<bool>& ignored,
            std::vector<float>& monitor, std::vector<float>& file) {
            return [self uiQaMeasure:name
                               frame:frame
                               movie:movie
                              source:patternId
                         sourceFrame:frame
                             ignored:ignored
                             monitor:monitor
                                file:file];
        };
    std::vector<bool> ignored;
    std::vector<float> opaque;
    std::vector<float> opaqueFile;
    std::vector<float> opaque20;
    std::vector<float> opaque20File;
    const std::filesystem::path full = [self uiQaExport:"b0-m4-opaque"];
    const bool measuredOpaque =
        measure("b0-m4-opaque-10", 10, full, ignored, opaque, opaqueFile) &&
        measure("b0-m4-opaque-20", 20, full, ignored, opaque20, opaque20File);
    std::vector<float> bare;
    std::vector<float> untrimmed5;
    const bool readBare = [self uiQaFileGrid:"b0-m4-bare-45"
                                       movie:full
                                       frame:45
                                        grid:bare] &&
                          [self uiQaFileGrid:"b0-m4-opaque-05"
                                       movie:full
                                       frame:5
                                        grid:untrimmed5];
    [self uiQaDiscard:full];

    SetClipOpacityOperation half;
    half.clip_id = motionId;
    half.opacity = {1, 2};
    [self uiQaApply:half label:"M4: the box is set to 50 %"];
    const std::filesystem::path halfMovie = [self uiQaExport:"b1-m4-half"];
    std::vector<float> halfShown;
    std::vector<float> halfFile;
    if (measuredOpaque &&
        measure("b1-m4-half-10", 10, halfMovie, ignored, halfShown, halfFile)) {
        UiQaExpectSame("M4 opaque box, frame 10: Record shows what the file "
                       "holds",
                       opaque, opaqueFile, ignored);
        UiQaExpectSame("M4 box at 50 %, frame 10: Record shows what the file "
                       "holds",
                       halfShown, halfFile, ignored);
        // Only the cells the box covers whole at frame 10 (x 200 to 800,
        // cells of 120 px): columns 2 to 5 of the 16.
        const UiQaGridShape shape = UiQaGridFor(self.state->document.sequence);
        std::vector<bool> outsideBox(ignored);
        for (size_t cell = 0; cell < outsideBox.size(); ++cell) {
            const int column = static_cast<int>(cell % shape.columns);
            if (column < 2 || column > 5) outsideBox[cell] = true;
        }
        UiQaExpectDifferent("M4 box at 50 % on screen, opaque box in the file, "
                            "inside the box",
                            halfShown, opaqueFile, outsideBox);
        if (readBare10) {
            const double midway =
                (UiQaMean(opaque, outsideBox) + UiQaMean(bare10, outsideBox)) /
                2.0;
            UiQaExpectValue("M4 box at 50 %: inside the box, the mean sits "
                            "halfway between the white box and the pattern "
                            "under it",
                            UiQaMean(halfShown, outsideBox), midway - 0.05,
                            midway + 0.05);
        }
    }
    [self uiQaDiscard:halfMovie];
    [self menuUndo:nil];

    // Five frames off the head: the clip starts at frame 5 on its sixth
    // frame of animation, and frame 20 does not move.
    TrimClipOperation trim;
    trim.clip_id = motionId;
    trim.edge = TrimEdge::Head;
    trim.delta = {5, 25};
    [self uiQaApply:trim label:"M4: five frames are trimmed off the head"];
    const DocumentClip* trimmed = self.state->document.FindClip(motionId);
    UiSmokeCheck(trimmed != nullptr &&
                     trimmed->timeline_in == RationalTime{5, 25} &&
                     trimmed->source_in == RationalTime{5, 25},
                 "VQA M4: the trimmed clip starts at frame 5 on its sixth "
                 "frame of animation");
    const std::filesystem::path trimMovie = [self uiQaExport:"b2-m4-trimmed"];
    std::vector<float> before;
    std::vector<float> beforeFile;
    std::vector<float> start;
    std::vector<float> startFile;
    std::vector<float> later;
    std::vector<float> laterFile;
    if (measuredOpaque && readBare &&
        measure("b2-m4-trimmed-04", 4, trimMovie, ignored, before,
                beforeFile) &&
        measure("b2-m4-trimmed-05", 5, trimMovie, ignored, start, startFile) &&
        measure("b2-m4-trimmed-20", 20, trimMovie, ignored, later, laterFile)) {
        UiQaExpectSame("M4 trimmed, frame 20: Record shows what the file holds",
                       later, laterFile, ignored);
        UiQaExpectSame("M4 trimmed, frame 20 is the frame 20 it was before "
                       "the trim",
                       later, opaque20, ignored);
        UiQaExpectSame("M4 trimmed, frame 5: Record shows what the file holds",
                       start, startFile, ignored);
        UiQaExpectSame("M4 trimmed, frame 5 shows the animation's frame 5, "
                       "as the untrimmed clip did",
                       start, untrimmed5, ignored);
        UiQaExpectSame("M4 trimmed, frame 4: Record shows what the file holds",
                       before, beforeFile, ignored);
        UiQaExpectDifferent("M4 trimmed, frame 4 has no box, frame 5 has it",
                            before, start, ignored);
    }
    [self uiQaDiscard:trimMovie];
    [self menuUndo:nil];
    if (measuredOpaque && !halfShown.empty())
        [self uiQaCheckTitleOverGradedShot:motionId
                                   pattern:patternId
                                     white:opaque
                                  ungraded:halfShown];
}

// REM1 -- docs/roadmap/REMOTION-PARITY-2026-09.md, scene C11. The box at
// 50 % over the pattern graded four stops down: Record against the file,
// and inside the box, halfway between the white box and the graded pattern
// Record shows alone. monitor_export_parity_tests holds the same scene to
// the exact blend on code values; here it is the real window, main.mm's
// wiring included.
//
// Under colour management, Record and the file part by more than 0.03 on
// a bare shot already: the open gap of 23 September (0.022 on a real shot,
// OCIO on the GPU against a cube in FFmpeg), which this saturated pattern
// read as S-Log3 widens. What C11 holds there is its own claim -- the box
// adds no gap to the shot's own, graded or not -- and the gaps are printed
// on every run rather than hidden under a looser bound.
- (void)uiQaCheckTitleOverGradedShot:(const Ulid&)motionId
                             pattern:(const Ulid&)patternId
                               white:(const std::vector<float>&)opaque
                            ungraded:(const std::vector<float>&)halfShown {
    Ulid shotId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && track.id != gUiSmokeMotionTrackId &&
            !track.clips.empty() && shotId.empty())
            shotId = track.clips.front().id;
    const size_t applied = self.state->editLog.AppliedCount();
    SetClipOpacityOperation half;
    half.clip_id = motionId;
    half.opacity = {1, 2};
    SetClipEffectsOperation grade;
    grade.clip_id = shotId;
    ClipEffect darker;
    darker.type = "color.exposure";
    // Four stops, as R6: two moved the grid's mean by 0.09 only.
    darker.params["amount"] = {-4, 1};
    grade.effects = {darker};
    SetClipEffectsOperation ungrade;
    ungrade.clip_id = shotId;
    SetTrackOutputOperation hide;
    hide.track_id = gUiSmokeMotionTrackId;
    hide.visible = false;
    SetTrackOutputOperation show = hide;
    show.visible = true;
    if (shotId.empty() ||
        ![self uiQaApply:half label:"C11: the box is set to 50 %"] ||
        ![self uiQaApply:grade label:"C11: the pattern is graded"])
        return;
    // Cells the box covers whole at frame 10 (x 200 to 800, cells of
    // 120 px): columns 2 to 5 of the 16, as for M4.
    const UiQaGridShape shape = UiQaGridFor(self.state->document.sequence);
    std::vector<bool> outsideBox(
        static_cast<size_t>(shape.columns) * shape.rows, false);
    for (size_t cell = 0; cell < outsideBox.size(); ++cell) {
        const int column = static_cast<int>(cell % shape.columns);
        if (column < 2 || column > 5) outsideBox[cell] = true;
    }
    const std::vector<bool> none(outsideBox.size(), false);
    // One state of the scene, exported, then Record captured at frame 10.
    struct State {
        std::vector<float> shown;
        std::vector<float> file;
        std::vector<bool> ignored;
        double gap = 1.0;
        bool ok = false;
    };
    const auto measure = [&](const std::string& name) {
        State state;
        const std::filesystem::path movie = [self uiQaExport:name];
        state.ok = !movie.empty() && [self uiQaMeasure:name + "-10"
                                                 frame:10
                                                 movie:movie
                                                source:patternId
                                           sourceFrame:10
                                               ignored:state.ignored
                                               monitor:state.shown
                                                  file:state.file];
        [self uiQaDiscard:movie];
        if (state.ok)
            state.gap = UiQaDistance(state.shown, state.file, state.ignored);
        return state;
    };
    const auto halfway = [&](const std::string& mode, const State& titled,
                             const State& alone) {
        const double midway =
            (UiQaMean(opaque, outsideBox) + UiQaMean(alone.shown, outsideBox)) /
            2.0;
        UiQaExpectValue("C11 " + mode +
                            ": inside the box, Record's mean sits halfway "
                            "between the white box and the graded pattern "
                            "it shows alone",
                        UiQaMean(titled.shown, outsideBox), midway - 0.05,
                        midway + 0.05);
    };

    // Without colour management: each state is the file's.
    State graded;
    State titled;
    if ([self uiQaApply:hide label:"C11: V2 is hidden"])
        graded = measure("b4-c11-graded-bare");
    if ([self uiQaApply:show label:"C11: V2 is shown"])
        titled = measure("b4-c11-graded");
    if (graded.ok && titled.ok) {
        UiQaExpectSame("C11 without colour management, the graded pattern "
                       "alone: Record shows what the file holds",
                       graded.shown, graded.file, graded.ignored);
        UiQaExpectSame("C11 without colour management, box at 50 % over the "
                       "graded pattern: Record shows what the file holds",
                       titled.shown, titled.file, titled.ignored);
        halfway("without colour management", titled, graded);
        UiQaExpectDifferent("C11 without colour management, the graded "
                            "pattern under the box, the ungraded one M4 "
                            "measured",
                            titled.shown, halfShown, none);
    }

    // S-Log3 in: the shot alone and under the box, graded and not.
    SetColorManagementOperation slog3;
    slog3.settings.enabled = true;
    slog3.settings.input_gamut = "sony_sgamut3_cine";
    slog3.settings.input_transfer = "sony_slog3";
    State managedGraded;
    State managedBare;
    State managedUngradedTitled;
    State managedTitled;
    if ([self uiQaApply:slog3 label:"C11: colour management on, S-Log3 in"] &&
        [self uiQaApply:hide label:"C11 managed: V2 is hidden"]) {
        managedGraded = measure("b5-c11-managed-graded-bare");
        if ([self uiQaApply:ungrade label:"C11 managed: the grade is removed"])
            managedBare = measure("b5-c11-managed-bare");
        if ([self uiQaApply:show label:"C11 managed: V2 is shown"])
            managedUngradedTitled = measure("b5-c11-managed-ungraded");
        if ([self uiQaApply:grade label:"C11 managed: the grade is back"])
            managedTitled = measure("b5-c11-managed");
    }
    if (managedGraded.ok && managedBare.ok && managedUngradedTitled.ok &&
        managedTitled.ok) {
        std::fprintf(stdout,
                     "NOTE: VQA C11 S-Log3 to Rec.709, Record against the "
                     "file: pattern alone %.4f, under the box %.4f; graded "
                     "alone %.4f, under the box %.4f\n",
                     managedBare.gap, managedUngradedTitled.gap,
                     managedGraded.gap, managedTitled.gap);
        UiQaExpectValue("C11 S-Log3 to Rec.709: the box adds no gap between "
                        "Record and the file to the pattern's own",
                        managedUngradedTitled.gap - managedBare.gap, -1.0,
                        0.01);
        UiQaExpectValue("C11 S-Log3 to Rec.709: the box adds no gap between "
                        "Record and the file to the graded pattern's own",
                        managedTitled.gap - managedGraded.gap, -1.0, 0.01);
        halfway("S-Log3 to Rec.709", managedTitled, managedGraded);
        UiQaExpectDifferent("C11 S-Log3 to Rec.709, the graded pattern under "
                            "the box, the ungraded one",
                            managedTitled.shown, managedUngradedTitled.shown,
                            none);
    }
    // Back to the state M4 left, one undo per edit made here.
    for (size_t guard = 0;
         self.state->editLog.AppliedCount() > applied && guard < 16; ++guard)
        [self menuUndo:nil];
    UiSmokeCheck(self.state->editLog.AppliedCount() == applied,
                 "VQA C11: every edit of the scene is undone");
}

// REM1 -- docs/roadmap/REMOTION-PARITY-2026-09.md. M5's three embedded
// templates in every declared format, then the logo's four corner values.
// Record is captured and measured against one export of each timeline.
- (void)runUiQaMotionTemplateScenario {
    constexpr std::array<const char*, 3> kFormats = {"9x16", "16x9", "1x1"};
    constexpr std::array<const char*, 3> kTemplates = {"nom", "carton", "logo"};
    for (size_t format = 0; format < kFormats.size(); ++format) {
        if (![self uiQaOpen:gUiSmokeTemplateTimelineIds[format] label:"M5"])
            continue;

        // The 16:9 fixture is the placement contract's difficult case: the
        // name spans a cut between two clips of the same source and ends as
        // another source begins. Keep the three bare frames so the overlay,
        // and then its absence, are measured instead of merely inferred from
        // a pretty screenshot.
        std::array<std::vector<float>, 3> bareNameFrames;
        bool readBareNameFrames = false;
        if (format == 1) {
            const std::filesystem::path bareMovie =
                [self uiQaExport:"b3-m5-16x9-bare"];
            constexpr std::array<int64_t, 3> kNameFrames = {24, 25, 50};
            readBareNameFrames = !bareMovie.empty();
            for (size_t frame = 0; frame < kNameFrames.size(); ++frame) {
                readBareNameFrames =
                    [self uiQaFileGrid:"b3-m5-16x9-bare-" +
                                       std::to_string(kNameFrames[frame])
                                 movie:bareMovie
                                 frame:kNameFrames[frame]
                                  grid:bareNameFrames[frame]] &&
                    readBareNameFrames;
            }
            UiSmokeCheck(readBareNameFrames,
                         "VQA M5 16x9: the three bare boundary frames are "
                         "read");
            [self uiQaDiscard:bareMovie];
        }
        for (size_t kind = 0; kind < kTemplates.size(); ++kind) {
            InsertMotionClipOperation insert;
            insert.track_id = gUiSmokeTemplateTrackIds[format];
            insert.composition_id =
                gUiSmokeTemplateCompositionIds[format][kind];
            insert.source_in = {0, 25};
            insert.duration = {50, 25};
            insert.timeline_in = {static_cast<int64_t>(kind) * 50, 25};
            insert.motion_props = mcp_json::Value::MakeObject();
            if (kind == 0) {
                insert.motion_props.Set(
                    "nom", mcp_json::Value::MakeString("Camille Durand"));
                insert.motion_props.Set(
                    "fonction", mcp_json::Value::MakeString("Cheffe monteuse"));
            } else if (kind == 1) {
                insert.motion_props.Set("titre", mcp_json::Value::MakeString(
                                                     "Une histoire à hauteur "
                                                     "de regard"));
                insert.motion_props.Set(
                    "sous_titre",
                    mcp_json::Value::MakeString("Gabarit CUTMACHINE"));
            } else {
                insert.motion_props.Set(
                    "logo", mcp_json::Value::MakeString("logo.png"));
            }
            if (![self uiQaApply:insert
                           label:"M5 " + std::string(kFormats[format]) + ": " +
                                 kTemplates[kind] + " is placed"])
                break;
        }

        // Four simultaneous instances make the margin symmetry visible in
        // one capture. The first uses the same track as the sequential set;
        // the other three use the extra overlay tracks of the 16:9 fixture.
        if (format == 1) {
            constexpr std::array<const char*, 4> kCorners = {
                "haut-droite", "haut-gauche", "bas-droite", "bas-gauche"};
            for (size_t corner = 0; corner < kCorners.size(); ++corner) {
                InsertMotionClipOperation insert;
                insert.track_id = gUiSmokeTemplateCornerTrackIds[corner];
                insert.composition_id =
                    gUiSmokeTemplateCompositionIds[format][2];
                insert.source_in = {0, 25};
                insert.duration = {50, 25};
                insert.timeline_in = {150, 25};
                insert.motion_props = mcp_json::Value::MakeObject();
                insert.motion_props.Set(
                    "logo", mcp_json::Value::MakeString("logo.png"));
                insert.motion_props.Set(
                    "coin", mcp_json::Value::MakeString(kCorners[corner]));
                [self uiQaApply:insert
                          label:"M5 16x9: logo " +
                                std::string(kCorners[corner]) + " is placed"];
            }
        }

        const std::string movieName = "b3-m5-" + std::string(kFormats[format]);
        const std::filesystem::path movie = [self uiQaExport:movieName];
        const auto measure = [&](const std::string& name, int64_t frame,
                                 const Ulid& source, int64_t sourceFrame,
                                 std::vector<float>* measuredMonitor,
                                 std::vector<float>* measuredFile) {
            std::vector<bool> ignored;
            std::vector<float> monitor;
            std::vector<float> file;
            const bool measured = [self uiQaMeasure:name
                                              frame:frame
                                              movie:movie
                                             source:source
                                        sourceFrame:sourceFrame
                                            ignored:ignored
                                            monitor:monitor
                                               file:file];
            if (measured) {
                UiQaExpectSame(
                    "M5 " + name + ": Record shows what the file holds",
                    monitor, file, ignored);
            }
            if (measuredMonitor != nullptr) *measuredMonitor = monitor;
            if (measuredFile != nullptr) *measuredFile = file;
            return measured;
        };
        if (format == 1) {
            std::array<std::vector<float>, 3> nameFrames;
            const bool measuredBoundaries =
                measure(movieName + "-nom-avant-coupe", 24,
                        gUiSmokePatternSourceId, 24, nullptr, &nameFrames[0]) &&
                measure(movieName + "-nom-apres-coupe", 25,
                        gUiSmokePatternSourceId, 25, nullptr, &nameFrames[1]) &&
                measure(movieName + "-nom-source-suivante", 50,
                        gUiSmokeGreySourceId, 0, nullptr, &nameFrames[2]);
            if (readBareNameFrames && measuredBoundaries) {
                const UiQaGridShape shape =
                    UiQaGridFor(self.state->document.sequence);
                std::vector<bool> nameArea(
                    static_cast<size_t>(shape.columns) * shape.rows, true);
                for (int row = 5; row < shape.rows; ++row)
                    for (int column = 0; column < shape.columns / 2; ++column)
                        nameArea[(static_cast<size_t>(row) * shape.columns) +
                                 column] = false;
                // A lower third changes much less of its region than a crop
                // or an angle switch, for which kUiQaDifferent is calibrated.
                // The frame at 50 is this assertion's negative control.
                UiQaExpectValue(
                    "M5 name is visible before the same-source jump cut",
                    UiQaDistance(nameFrames[0], bareNameFrames[0], nameArea),
                    0.02, 1.0);
                UiQaExpectValue(
                    "M5 name survives the same-source jump cut",
                    UiQaDistance(nameFrames[1], bareNameFrames[1], nameArea),
                    0.02, 1.0);
                UiQaExpectSame("M5 name is gone when the next source begins",
                               nameFrames[2], bareNameFrames[2], nameArea);
            }
        } else {
            measure(movieName + "-nom", 25, gUiSmokePatternSourceId, 25,
                    nullptr, nullptr);
        }
        measure(movieName + "-carton", 75,
                format == 1 ? gUiSmokeGreySourceId : gUiSmokePatternSourceId,
                25, nullptr, nullptr);
        measure(movieName + "-logo", 125, gUiSmokePatternSourceId, 25, nullptr,
                nullptr);
        if (format == 1)
            measure(movieName + "-coins", 175, gUiSmokePatternSourceId, 25,
                    nullptr, nullptr);
        [self uiQaDiscard:movie];
    }
}

// R2: a three-frame shake (position x and y) on frame 20 of the dip
// timeline's first shot, the second shot left fixed. Stepped against the
// file and the unanimated shot, played at 1x, and the fixed shot compared
// with itself before the animation, on screen and frame by frame in the
// file.
- (void)runUiQaShakeScenario {
    if (![self uiQaOpen:gUiSmokeDipTimelineId label:"R2 shake"]) return;
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeDipFirstClipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    const auto sourceFrameAt = [](int64_t frame) {
        return frame < 50 ? frame : frame + 2;
    };
    const std::filesystem::path still = [self uiQaExport:"c0-r2-still"];
    std::vector<bool> ignored;
    std::vector<float> fixedBefore;
    std::vector<float> fixedBeforeFile;
    const bool measuredFixed = [self uiQaMeasure:"c0-r2-fixed-before-70"
                                           frame:70
                                           movie:still
                                          source:sourceId
                                     sourceFrame:sourceFrameAt(70)
                                         ignored:ignored
                                         monitor:fixedBefore
                                            file:fixedBeforeFile];
    const auto key = [](int64_t frame, int32_t num, int32_t den) {
        ClipKeyframe keyframe;
        keyframe.id = GenerateUlid();
        keyframe.time = {frame, 25};
        keyframe.value = {num, den};
        keyframe.interpolation = KeyframeInterpolation::Hold;
        return keyframe;
    };
    ClipParameterAnimation x;
    x.parameter = ClipAnimatedParameter::FramingPositionX;
    x.keyframes = {key(0, 0, 1), key(20, 1, 20), key(21, -1, 20),
                   key(22, 1, 40), key(23, 0, 1)};
    ClipParameterAnimation y;
    y.parameter = ClipAnimatedParameter::FramingPositionY;
    y.keyframes = {key(0, 0, 1), key(20, -1, 20), key(21, 1, 20),
                   key(22, -1, 40), key(23, 0, 1)};
    SetClipAnimationOperation animate;
    animate.clips = {{gUiSmokeDipFirstClipId, {x, y}}};
    if (![self uiQaApply:animate label:"R2 shake: three frames on frame 20"]) {
        [self uiQaDiscard:still];
        return;
    }
    const std::filesystem::path movie = [self uiQaExport:"c1-r2-shake"];
    constexpr int64_t kFrames[] = {19, 20, 21, 22, 23};
    constexpr size_t kCount = std::size(kFrames);
    std::vector<float> monitor[kCount];
    std::vector<float> file[kCount];
    std::vector<float> unanimated[kCount];
    bool measured = true;
    for (size_t index = 0; measured && index < kCount; ++index)
        measured =
            [self uiQaMeasure:"c1-r2-shake-" + std::to_string(kFrames[index])
                        frame:kFrames[index]
                        movie:movie
                       source:sourceId
                  sourceFrame:kFrames[index]
                      ignored:ignored
                      monitor:monitor[index]
                         file:file[index]] &&
            [self uiQaFileGrid:"c0-r2-still-" + std::to_string(kFrames[index])
                         movie:still
                         frame:kFrames[index]
                          grid:unanimated[index]];
    if (measured) {
        for (size_t index = 0; index < kCount; ++index)
            UiQaExpectSame("R2 shake frame " + std::to_string(kFrames[index]) +
                               ": Record shows what the file holds",
                           monitor[index], file[index], ignored);
        UiQaExpectSame("R2 shake: frame 19, before the impact, is the still "
                       "shot",
                       monitor[0], unanimated[0], ignored);
        for (size_t index = 1; index < 4; ++index)
            UiQaExpectDifferent("R2 shake: frame " +
                                    std::to_string(kFrames[index]) +
                                    " is shaken, the still shot in the file",
                                monitor[index], unanimated[index], ignored);
        UiQaExpectSame("R2 shake: frame 23, after it, is the still shot again",
                       monitor[4], unanimated[4], ignored);
    }
    std::vector<UiQaShown> expected;
    for (int64_t frame = 0; frame < 100; ++frame)
        expected.push_back(
            {frame < 50 ? gUiSmokeDipFirstClipId : gUiSmokeDipSecondClipId,
             sourceFrameAt(frame)});
    UiQaCheckPlayback("playback R2 shake, forwards", [self uiQaPlay:1 from:0],
                      expected, 1, gUiQaOutputDirectory / "c2-play-shake.tsv");

    // The fixed shot beside it, on screen and in every frame of the file.
    std::vector<float> fixedAfter;
    std::vector<float> fixedAfterFile;
    if (measuredFixed && [self uiQaMeasure:"c3-r2-fixed-after-70"
                                     frame:70
                                     movie:movie
                                    source:sourceId
                               sourceFrame:sourceFrameAt(70)
                                   ignored:ignored
                                   monitor:fixedAfter
                                      file:fixedAfterFile]) {
        UiQaExpectValue("R2 the fixed shot on screen, before and after its "
                        "neighbour was animated",
                        UiQaDistance(fixedBefore, fixedAfter, ignored), 0.0,
                        0.0005);
    }
    gUiQaGridDensity = 3;
    std::vector<std::vector<float>> before;
    std::vector<std::vector<float>> after;
    const UiQaGridShape shape = UiQaGridFor(self.state->document.sequence);
    if (UiQaFileFrames(still, gUiQaOutputDirectory / "c4-r2-still-frames",
                       shape, before) &&
        UiQaFileFrames(movie, gUiQaOutputDirectory / "c4-r2-shake-frames",
                       shape, after) &&
        before.size() == after.size() && before.size() >= 100) {
        const std::vector<bool> none(before.front().size() / 3, false);
        double widest = 0.0;
        for (size_t frame = 50; frame < 100; ++frame)
            widest = std::max(widest,
                              UiQaDistance(before[frame], after[frame], none));
        UiQaExpectValue("R2 the fixed shot in the file, frames 50 to 99, "
                        "before and after its neighbour was animated (widest "
                        "gap)",
                        widest, 0.0, 0.005);
    } else {
        UiSmokeCheck(false, "VQA R2: both files are read frame by frame");
    }
    gUiQaGridDensity = 1;
    [self uiQaDiscard:still];
    [self uiQaDiscard:movie];
    [self menuUndo:nil];
}

// F1.5: a group of two angles, the pattern and the grey. Record against
// the file with the pattern active, then after switching to the grey.
- (void)runUiQaMulticamScenario {
    if (![self uiQaOpen:gUiSmokeMulticamTimelineId label:"F1.5"]) return;
    const DocumentClip* wide =
        self.state->document.FindClip(gUiSmokeMulticamPatternClipId);
    const DocumentClip* close =
        self.state->document.FindClip(gUiSmokeMulticamGreyClipId);
    if (wide == nullptr || close == nullptr) return;
    const Ulid patternId = wide->source_id;
    const Ulid greyId = close->source_id;
    AddMulticamGroupOperation add;
    EditError error = EditError::None;
    std::string message;
    const bool built = BuildAddMulticamGroup(
        self.state->document, "QA",
        {{gUiSmokeMulticamPatternClipId, "Mire"},
         {gUiSmokeMulticamGreyClipId, "Gris"}},
        gUiSmokeMulticamPatternClipId, add, error, message);
    UiSmokeCheck(built, ("VQA F1.5: a two-angle group is built" +
                         (built ? "" : ": " + message))
                            .c_str());
    if (!built || ![self uiQaApply:add label:"F1.5: the group is added"])
        return;
    const Ulid groupId = add.group.id;
    Ulid greyAngle;
    for (const MulticamAngle& angle : add.group.angles)
        if (angle.clip_id == gUiSmokeMulticamGreyClipId) greyAngle = angle.id;
    std::vector<bool> ignored;
    std::vector<float> patternShown;
    std::vector<float> patternFile;
    std::vector<float> greyShown;
    std::vector<float> greyFile;
    const std::filesystem::path first = [self uiQaExport:"d0-f15-pattern"];
    const bool measuredPattern = [self uiQaMeasure:"d0-f15-pattern"
                                             frame:12
                                             movie:first
                                            source:patternId
                                       sourceFrame:12
                                           ignored:ignored
                                           monitor:patternShown
                                              file:patternFile];
    [self uiQaDiscard:first];
    SetMulticamActiveAngleOperation change;
    change.group_id = groupId;
    change.active_angle_id = greyAngle;
    [self uiQaApply:change label:"F1.5: the grey angle becomes active"];
    const std::filesystem::path second = [self uiQaExport:"d1-f15-grey"];
    const bool measuredGrey = [self uiQaMeasure:"d1-f15-grey"
                                          frame:12
                                          movie:second
                                         source:greyId
                                    sourceFrame:12
                                        ignored:ignored
                                        monitor:greyShown
                                           file:greyFile];
    [self uiQaDiscard:second];
    if (measuredPattern && measuredGrey) {
        UiQaExpectSame("F1.5 pattern angle active: Record shows what the file "
                       "holds",
                       patternShown, patternFile, ignored);
        UiQaExpectSame("F1.5 after the switch to the grey angle: Record shows "
                       "what the file holds",
                       greyShown, greyFile, ignored);
        UiQaExpectDifferent("F1.5 grey angle on screen, pattern angle in the "
                            "file",
                            greyShown, patternFile, ignored);
    }
    [self menuUndo:nil];
    [self menuUndo:nil];
}

// BENCH-2026-09: a roll, Cmd-dragging the boundary of two cartons with the
// mouse. The boundary follows the pointer, each carton keeps its text in
// Record, a drag that would empty a carton stops short or is refused, and
// one undo gives both back.
- (void)runUiQaRollScenario {
    if (![self uiQaOpen:gUiSmokeRollTimelineId label:"roll"]) return;
    [self fitTimelineToViewportWidth];
    self.state->overlayDirty = true;
    [self presentNearestFrameAtDeadline:NO];
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    size_t lane = tracks.size();
    for (size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index]->kind == "caption") lane = index;
    if (lane == tracks.size()) return;
    const TimelineViewport& viewport = self.state->viewport;
    const double y = self.metalView.bounds.size.height -
                     ([self videoHeight] + kTimelineRulerHeight +
                      ((lane + 0.5) * viewport.track_height));
    const auto cartons = [&]() {
        const DocumentClip* first =
            self.state->document.FindClip(gUiSmokeRollFirstCartonId);
        const DocumentClip* second =
            self.state->document.FindClip(gUiSmokeRollSecondCartonId);
        return std::make_pair(first, second);
    };
    const size_t before = self.state->editLog.AppliedCount();
    const double boundary = viewport.TimeToX({30, 25});
    SendTimelineMouseGesture(self.metalView, NSMakePoint(boundary, y),
                             NSMakePoint(viewport.TimeToX({40, 25}), y),
                             NSEventModifierFlagCommand);
    auto [first, second] = cartons();
    const int64_t firstEnd =
        first ? first->timeline_in.add(first->duration).to_frames(25, 1) : -1;
    const int64_t secondStart =
        second ? second->timeline_in.to_frames(25, 1) : -1;
    UiSmokeCheck(
        first && second && firstEnd == secondStart &&
            std::llabs(firstEnd - 40) <= 1 &&
            second->timeline_in.add(second->duration).to_frames(25, 1) == 60 &&
            self.state->editLog.AppliedCount() == before + 1,
        ("VQA roll: Cmd-dragging the boundary 10 frames right moves "
         "it to frame " +
         std::to_string(firstEnd) +
         ", both cartons still meet, the second still ends at 60, "
         "in one edit")
            .c_str());
    [self requestTimelinePosition:{35, 25}];
    self.state->overlayDirty = true;
    [self presentNearestFrameAtDeadline:NO];
    NSString* at35 = self.programCaptionView.captionText;
    [self requestTimelinePosition:{50, 25}];
    self.state->overlayDirty = true;
    [self presentNearestFrameAtDeadline:NO];
    NSString* at50 = self.programCaptionView.captionText;
    UiSmokeCheck([at35 isEqualToString:@"Premier carton"] &&
                     [at50 isEqualToString:@"Second carton"],
                 ("VQA roll: Record shows the first carton's text at frame "
                  "35 and the second's at 50 (\"" +
                  std::string(at35.UTF8String ?: "") + "\", \"" +
                  std::string(at50.UTF8String ?: "") + "\")")
                     .c_str());
    // Past the second carton's end: it must not be emptied.
    const size_t rolled = self.state->editLog.AppliedCount();
    SendTimelineMouseGesture(
        self.metalView, NSMakePoint(viewport.TimeToX({40, 25}), y),
        NSMakePoint(viewport.TimeToX({70, 25}), y), NSEventModifierFlagCommand);
    std::tie(first, second) = cartons();
    UiSmokeCheck(
        first && second && second->duration.value > 0 &&
            first->timeline_in.add(first->duration) == second->timeline_in &&
            second->timeline_in.add(second->duration).to_frames(25, 1) == 60,
        ("VQA roll: a drag past the second carton's end leaves it "
         "at least one frame (" +
         std::to_string(second ? second->duration.to_frames(25, 1) : -1) +
         " frames, " +
         std::to_string(self.state->editLog.AppliedCount() - rolled) + " edit)")
            .c_str());
    if (self.state->editLog.AppliedCount() > rolled) [self menuUndo:nil];
    [self menuUndo:nil];
    std::tie(first, second) = cartons();
    UiSmokeCheck(first && second && first->duration == RationalTime{30, 25} &&
                     second->timeline_in == RationalTime{30, 25} &&
                     second->duration == RationalTime{30, 25},
                 "VQA roll: one undo gives both cartons back as they were");

    // D7 -- docs/roadmap/DIALOGUE-2026-10.md. The Inspector walks from
    // carton to carton: its two buttons, and Tab in its field, select the
    // neighbour and send the playhead to it, and what was typed is one edit
    // made on the way.
    NSTextField* field = self.inspectorView.captionTextField;
    NSButton* next = FindButtonWithTitle(self.inspectorView, @"Suivant ▶");
    NSButton* previous =
        FindButtonWithTitle(self.inspectorView, @"◀ Précédent");
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    self.state->interaction->SelectClip(gUiSmokeRollFirstCartonId);
    [self updateSelectionInfo];
    UiSmokeCheck(next && previous && next.enabled && !previous.enabled &&
                     [field.stringValue isEqualToString:@"Premier carton"],
                 "D7: on the first carton of a track, only Suivant is "
                 "offered");
    if (!next || !previous) return;
    [next sendAction:next.action to:next.target];
    UiSmokeCheck(
        self.state->interaction->SelectedClipId() ==
                gUiSmokeRollSecondCartonId &&
            self.state->requestedPosition == RationalTime{30, 25} &&
            [field.stringValue isEqualToString:@"Second carton"] &&
            !next.enabled && previous.enabled,
        "D7: Suivant selects the next carton, sends the playhead to its "
        "first frame and shows its words");
    const size_t walked = self.state->editLog.AppliedCount();
    [self editCartonText:gUiSmokeRollSecondCartonId];
    if (field.currentEditor) {
        field.currentEditor.string = @"Second carton, corrigé";
        [(id<NSTextFieldDelegate>)field.delegate
                        control:field
                       textView:(NSTextView*)field.currentEditor
            doCommandBySelector:@selector(insertBacktab:)];
    }
    std::tie(first, second) = cartons();
    UiSmokeCheck(
        second && second->caption_text == "Second carton, corrigé" &&
            self.state->editLog.AppliedCount() == walked + 1 &&
            self.state->interaction->SelectedClipId() ==
                gUiSmokeRollFirstCartonId &&
            self.state->requestedPosition == RationalTime{0, 25} &&
            [field.stringValue isEqualToString:@"Premier carton"] &&
            field.currentEditor != nil,
        "D7: Shift+Tab in the field commits what was typed, goes to the "
        "previous carton and keeps the keyboard in its text");
    if (field.currentEditor)
        [(id<NSTextFieldDelegate>)field.delegate
                        control:field
                       textView:(NSTextView*)field.currentEditor
            doCommandBySelector:@selector(insertBacktab:)];
    UiSmokeCheck(self.state->interaction->SelectedClipId() ==
                         gUiSmokeRollFirstCartonId &&
                     self.state->editLog.AppliedCount() == walked + 1 &&
                     field.currentEditor != nil,
                 "D7: at the head of the track, Shift+Tab stays on the "
                 "carton it has");
    [self.window makeFirstResponder:self.metalView];
    [self menuUndo:nil];
    std::tie(first, second) = cartons();
    UiSmokeCheck(second && second->caption_text == "Second carton" &&
                     self.state->editLog.AppliedCount() == walked,
                 "D7: one undo gives the second carton its words back");
}

// PERF7: 11 s at rest, then 11 s of playback, read back from the report
// CUTMACHINE_RENDER_COST writes. A window of 300 ticks wholly at rest must
// build no display list; one wholly in playback, about one per tick; and
// either must last about five seconds, the 60 Hz timer holding.
- (void)runUiQaRenderCostScenario {
    const char* destination = std::getenv(render_cost::kEnvironmentVariable);
    if (destination == nullptr || std::string(destination) == "1" ||
        std::string(destination) == "stderr")
        return;
    if (![self uiQaOpen:gUiSmokeNtscTimelineId label:"PERF7"]) return;
    [self requestTimelinePosition:{0, 1}];
    const auto seconds = [] {
        return (render_cost::NowNanoseconds() - gRenderCostOriginNs) / 1e9;
    };
    const auto wait = [](double duration) {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:duration];
        while (until.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop
                   runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    };
    wait(1.0);
    const double restStart = seconds();
    wait(11.0);
    const double restEnd = seconds();
    [self setPlaybackDirection:1];
    const double playStart = seconds();
    wait(11.0);
    const double playEnd = seconds();
    [self setPlaybackDirection:0];
    std::ifstream report(destination);
    std::string line;
    struct Window {
        double start = 0.0;
        double end = 0.0;
        long ticks = 0;
        long builds = 0;
    };
    std::vector<Window> rest;
    std::vector<Window> play;
    while (std::getline(report, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("start_s", 0) == 0)
            continue;
        Window window;
        if (std::sscanf(line.c_str(), "%lf\t%lf\t%ld\t%ld", &window.start,
                        &window.end, &window.ticks, &window.builds) != 4)
            continue;
        if (window.start >= restStart && window.end <= restEnd)
            rest.push_back(window);
        if (window.start >= playStart && window.end <= playEnd)
            play.push_back(window);
    }
    const auto describe = [](const std::vector<Window>& windows) {
        std::string text;
        for (const Window& window : windows) {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer,
                          " [%.1f-%.1f s, %ld ticks, %ld builds]", window.start,
                          window.end, window.ticks, window.builds);
            text += buffer;
        }
        return text;
    };
    bool restHolds = !rest.empty();
    for (const Window& window : rest)
        restHolds = restHolds && window.builds == 0 && window.ticks == 300 &&
                    std::abs((window.end - window.start) - 5.0) <= 0.5;
    UiSmokeCheck(restHolds,
                 ("VQA PERF7: at rest, a window of 300 ticks lasts five "
                  "seconds and builds no display list:" +
                  describe(rest))
                     .c_str());
    bool playHolds = !play.empty();
    for (const Window& window : play)
        playHolds = playHolds && window.ticks == 300 &&
                    window.builds * 10 >= window.ticks * 9 &&
                    std::abs((window.end - window.start) - 5.0) <= 0.5;
    UiSmokeCheck(playHolds,
                 ("VQA PERF7: in playback, a window of 300 ticks lasts five "
                  "seconds and builds a display list at nearly every tick:" +
                  describe(play))
                     .c_str());
}

// F1.3: an HLG delivery. The file is tagged HLG; Record shows the SDR
// preview -- the same picture contact_sheet hands a model -- and the gap
// between that preview and the file's values is measured and noted.
- (void)runUiQaHlgScenario {
    if (![self uiQaOpen:gUiSmokeLookTimelineId label:"HLG"]) return;
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeLookClipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    SetColorManagementOperation managed;
    managed.settings.enabled = true;
    managed.settings.output_gamut = "rec2020";
    managed.settings.output_transfer = "hlg";
    if (![self uiQaApply:managed label:"HLG: the sequence delivers HLG"])
        return;
    const std::filesystem::path movie = gUiQaOutputDirectory / "e0-hlg.mov";
    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(movie.string());
    settings.width = 1920;
    settings.height = 1080;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.overwrite = true;
    ExportPlan plan;
    std::string error;
    const bool exported =
        Exporter::BuildPlan(self.state->document,
                            self.documentPath.UTF8String ?: "", settings, plan,
                            error) &&
        Exporter::Run(plan, {}, nullptr, error);
    UiSmokeCheck(exported, ("VQA HLG: the sequence exports in HEVC 10 bits" +
                            (exported ? "" : ": " + error))
                               .c_str());
    std::string tags;
    if (exported) {
        // The stream line FFmpeg prints for the file, where it names the
        // pixel format and the colour tags.
        const std::string probe =
            UiQaQuote(FFMPEG_EXECUTABLE) + " -hide_banner -i " +
            UiQaQuote(movie.string()) + " 2>&1 | grep 'Video:'";
        if (FILE* pipe = popen(probe.c_str(), "r")) {
            char buffer[128];
            while (fgets(buffer, sizeof buffer, pipe)) tags += buffer;
            pclose(pipe);
        }
    }
    UiSmokeCheck(
        tags.find("arib-std-b67") != std::string::npos &&
            tags.find("bt2020") != std::string::npos,
        ("VQA HLG: the file is tagged HLG, BT.2020 (" + tags + ")").c_str());
    // Record at the shot's middle, where contact_sheet takes its cell.
    std::vector<bool> ignored;
    std::vector<float> monitor;
    std::vector<float> file;
    const bool measured = exported && [self uiQaMeasure:"e1-hlg-monitor"
                                                  frame:25
                                                  movie:movie
                                                 source:sourceId
                                            sourceFrame:25
                                                ignored:ignored
                                                monitor:monitor
                                                   file:file];
    const McpToolRegistry registry;
    const McpToolCallOutcome sheet = registry.Call(
        *self.chatBackend, "contact_sheet", mcp_json::Value::MakeObject());
    NSData* jpeg = sheet.ok
                       ? [[NSData alloc]
                             initWithBase64EncodedString:CMStringFromUtf8(
                                                             sheet.image_base64)
                                                 options:0]
                       : nil;
    const std::filesystem::path sheetPath =
        gUiQaOutputDirectory / "e2-hlg-contact_sheet.jpg";
    [jpeg writeToFile:[NSString stringWithUTF8String:sheetPath.c_str()]
           atomically:YES];
    std::vector<float> cell;
    const bool readCell =
        jpeg != nil && UiQaGrid(sheetPath, CGRectMake(0.0, 0.0, 320.0, 180.0),
                                UiQaGridFor(self.state->document.sequence),
                                cell, UiQaReading::RecordLayer);
    if (measured && readCell) {
        UiQaExpectSame("HLG: Record shows the SDR preview contact_sheet hands "
                       "a model",
                       monitor, cell, ignored);
        const double gap = UiQaDistance(monitor, file, ignored);
        std::fprintf(stdout,
                     "NOTE: VQA HLG: the file's HLG values read as SDR are "
                     "%.4f from Record's preview\n",
                     gap);
        UiQaExpectValue("HLG: the known gap is there, Record's SDR preview "
                        "against the file's HLG values read as SDR",
                        gap, 0.03, 1.0);
    }
    [self uiQaDiscard:movie];
    [self menuUndo:nil];
}

// R3's other speeds on the 25 i/s pattern: 1/2 holds each source frame
// for two sequence frames, 2 reads one in two and must cut on time, 2/5
// alternates three and two. Stepped through every frame, measured against
// the file around each case, and read back by contact_sheet and cut_sheet.
- (void)runUiQaCadenceScenario {
    if (![self uiQaOpen:gUiSmokeCadenceTimelineId label:"R3 cadences"]) return;
    const struct {
        Ulid clip;
        EffectParamValue speed;
    } speeds[] = {
        {gUiSmokeCadenceHalfClipId, {1, 2}},
        {gUiSmokeCadenceDoubleClipId, {2, 1}},
        {gUiSmokeCadenceTwoFifthsClipId, {2, 5}},
    };
    for (const auto& entry : speeds) {
        SetClipSpeedOperation operation;
        operation.clip_id = entry.clip;
        operation.speed = entry.speed;
        if (![self uiQaApply:operation
                       label:"R3 cadences: a clip is set to " +
                             std::to_string(entry.speed.num) + "/" +
                             std::to_string(entry.speed.den)])
            return;
    }
    const std::vector<UiQaShown> expected = UiQaCadenceFrames();
    gUiQaGridDensity = 3;
    struct DensityRestore {
        ~DensityRestore() { gUiQaGridDensity = 1; }
    } densityRestore;

    // Every frame, stepped: what Record draws against the rule.
    size_t wrong = 0;
    std::string firstWrong;
    std::vector<int64_t> twoFifths;
    for (size_t frame = 0; frame < expected.size(); ++frame) {
        const UiQaShown shown =
            [self uiQaShowFrame:static_cast<int64_t>(frame)];
        if (shown != expected[frame] && wrong++ == 0)
            firstWrong = ": frame " + std::to_string(frame) + " draws " +
                         std::to_string(shown.source) + ", the rule gives " +
                         std::to_string(expected[frame].source);
        if (shown.clip == gUiSmokeCadenceTwoFifthsClipId)
            twoFifths.push_back(shown.source);
    }
    UiSmokeCheck(wrong == 0,
                 ("VQA R3 cadences: stepped through all " +
                  std::to_string(expected.size()) +
                  " frames, Record draws the source frame the rule gives at "
                  "each" +
                  firstWrong)
                     .c_str());
    // How long each source frame stays on the 2/5 shot, as drawn.
    std::string holds;
    bool alternating = twoFifths.size() == 40;
    for (size_t index = 0, run = 0; index < twoFifths.size(); ++index) {
        ++run;
        if (index + 1 == twoFifths.size() ||
            twoFifths[index + 1] != twoFifths[index]) {
            holds += (holds.empty() ? "" : ",") + std::to_string(run);
            const size_t ordinal = std::count(holds.begin(), holds.end(), ',');
            alternating = alternating && run == (ordinal % 2 == 0 ? 3 : 2);
            run = 0;
        }
    }
    UiSmokeCheck(alternating, ("VQA R3 2/5: each source frame stays three "
                               "then two sequence frames, without a break "
                               "in the alternation (" +
                               holds + ")")
                                  .c_str());

    const std::filesystem::path movie = [self uiQaExport:"55-r3-cadences"];
    std::vector<std::vector<float>> frames;
    const bool read =
        !movie.empty() &&
        UiQaFileFrames(movie, gUiQaOutputDirectory / "55-r3-cadences-frames",
                       UiQaGridFor(self.state->document.sequence), frames);
    UiSmokeCheck(
        read && frames.size() == expected.size(),
        ("VQA R3 cadences: the file holds " + std::to_string(frames.size()) +
         " frames for " + std::to_string(expected.size()))
            .c_str());
    if (read && frames.size() == expected.size()) {
        // In the file, a frame repeats the one before exactly when the rule
        // shows the same source frame twice.
        const std::vector<bool> none(frames.front().size() / 3, false);
        size_t disagreements = 0;
        std::string firstDisagreement;
        double widestRepeat = 0.0;
        double narrowestChange = 1.0;
        for (size_t frame = 1; frame < frames.size(); ++frame) {
            const double distance =
                UiQaDistance(frames[frame - 1], frames[frame], none);
            const bool repeated = expected[frame] == expected[frame - 1];
            if (repeated)
                widestRepeat = std::max(widestRepeat, distance);
            else
                narrowestChange = std::min(narrowestChange, distance);
            if (repeated ? distance > kUiQaSame : distance < kUiQaNeighbour) {
                if (disagreements++ == 0) {
                    char buffer[96];
                    std::snprintf(buffer, sizeof buffer,
                                  ": frame %zu at %.4f from the one before",
                                  frame, distance);
                    firstDisagreement = buffer;
                }
            }
        }
        char spread[96];
        std::snprintf(spread, sizeof spread,
                      " (repeats within %.4f, changes from %.4f)", widestRepeat,
                      narrowestChange);
        UiSmokeCheck(disagreements == 0,
                     ("VQA R3 cadences: in the file, a frame repeats the one "
                      "before exactly where the rule holds a source frame, "
                      "and changes everywhere else" +
                      firstDisagreement + spread)
                         .c_str());
    }

    // On screen against the file: 6 and 7 hold source 3 and 8 moves to 4;
    // 64 is the last frame at 2 (source 98), 65 the first at 2/5; 67 and 68
    // move from source 10 to 11.
    constexpr int64_t kFrames[] = {6, 7, 8, 64, 65, 67, 68};
    constexpr size_t kCount = std::size(kFrames);
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeCadenceHalfClipId);
    const Ulid sourceId = shot != nullptr ? shot->source_id : Ulid();
    std::vector<float> monitor[kCount];
    std::vector<float> file[kCount];
    std::vector<bool> ignored;
    bool measured = !movie.empty();
    for (size_t index = 0; measured && index < kCount; ++index)
        measured =
            [self uiQaMeasure:"56-r3-cadence-" + std::to_string(kFrames[index])
                        frame:kFrames[index]
                        movie:movie
                       source:sourceId
                  sourceFrame:expected[kFrames[index]].source
                      ignored:ignored
                      monitor:monitor[index]
                         file:file[index]];
    if (measured) {
        for (size_t index = 0; index < kCount; ++index)
            UiQaExpectSame("R3 cadence frame " +
                               std::to_string(kFrames[index]) +
                               ": Record shows what the file holds",
                           monitor[index], file[index], ignored);
        UiQaExpectSame("R3 1/2: frame 7 repeats frame 6 on screen, no "
                       "blend of two source frames",
                       monitor[0], monitor[1], ignored);
        UiQaExpectNeighbour("R3 1/2: frame 7 on screen, frame 8 in the file",
                            monitor[1], file[2], ignored);
        UiQaExpectDifferent("R3 the cut after the shot at 2: frame 64 on "
                            "screen, frame 65 in the file",
                            monitor[3], file[4], ignored);
        UiQaExpectNeighbour("R3 2/5: frame 67 on screen, frame 68 in the "
                            "file",
                            monitor[5], file[6], ignored);
    }
    // A sheet cell is 320 pixels wide: read on the default grid.
    gUiQaGridDensity = 1;
    std::vector<std::vector<float>> coarse;
    if (read &&
        UiQaFileFrames(movie, gUiQaOutputDirectory / "57-r3-cadences-frames",
                       UiQaGridFor(self.state->document.sequence), coarse)) {
        [self uiQaSheet:"contact_sheet" name:"57-r3-cadences" frames:coarse];
        [self uiQaSheet:"cut_sheet" name:"57-r3-cadences" frames:coarse];
    }
    [self uiQaDiscard:movie];
}

// R10: a timeline Resolve laid out, read back by the bridge and imported
// by the CLI's own commands, drawn in the window. On each video track the
// capture is scanned along the middle of its lane: every run of pixels that
// is not the empty lane is a drawn clip, and its two edges must fall where
// TimelineViewport puts that clip's timeline_in and end -- the geometry a
// click uses. Resolve's own positions are compared with the document
// outside the app, from the API (docs/qa).
- (void)runUiQaResolveScenario {
    if (gUiSmokeResolveTimelineId.empty()) return;
    if (![self uiQaOpen:gUiSmokeResolveTimelineId label:"R10"]) return;
    [self fitTimelineToViewportWidth];
    for (int tick = 0; tick < 6; ++tick) {
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    std::string captureError;
    const bool captured =
        CaptureUiQaWindow(self.window, "96-r10-timeline", captureError);
    UiSmokeCheck(
        captured,
        ("VQA captured 96-r10-timeline" + (captured ? "" : ": " + captureError))
            .c_str(),
        ui_smoke::Needs::kVisibleWindow);
    if (!captured) return;
    const std::filesystem::path png =
        gUiQaOutputDirectory / "96-r10-timeline.png";
    const TimelineViewport& viewport = self.state->viewport;
    const NSRect bounds = self.metalView.bounds;
    const double top = [self videoHeight];
    const double scale = self.window.backingScaleFactor;
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    size_t measuredTracks = 0;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const DocumentTrack& track = *tracks[index];
        if (track.kind != "video") continue;
        // The lane's middle, from the top of the timeline view, then in the
        // capture's pixels.
        const double lane = top + kTimelineRulerHeight +
                            (index * viewport.track_height) +
                            (viewport.track_height / 2.0);
        const CGRect row = [self
            uiQaCaptureRect:NSMakeRect(0.0, bounds.size.height - lane - 0.5,
                                       bounds.size.width, 1.0)
                     inView:self.metalView];
        std::vector<std::array<uint8_t, 3>> pixels;
        if (!UiQaPngRow(png, static_cast<int64_t>(CGRectGetMidY(row)), pixels))
            continue;
        const auto pixelAt = [&](double x) {
            const int64_t column = std::clamp<int64_t>(
                static_cast<int64_t>(CGRectGetMinX(row) + (x * scale)), 0,
                static_cast<int64_t>(pixels.size()) - 1);
            return pixels[column];
        };
        // The empty lane: the colour just past the header, before anything
        // can be drawn over it, unless a clip starts there.
        std::vector<std::pair<double, double>> expected;
        for (const DocumentClip& clip : track.clips)
            expected.push_back(
                {viewport.TimeToX(clip.timeline_in),
                 viewport.TimeToX(clip.timeline_in.add(clip.duration))});
        std::sort(expected.begin(), expected.end());
        double laneX = bounds.size.width - 2.0;
        for (size_t gap = 0; gap < expected.size(); ++gap) {
            const double from =
                gap == 0 ? viewport.header_width : expected[gap - 1].second;
            if (expected[gap].first - from > 6.0) {
                laneX = (from + expected[gap].first) / 2.0;
                break;
            }
        }
        const std::array<uint8_t, 3> laneColour = pixelAt(laneX);
        const auto isClip = [&](const std::array<uint8_t, 3>& pixel) {
            int distance = 0;
            for (int channel = 0; channel < 3; ++channel)
                distance += std::abs(pixel[channel] - laneColour[channel]);
            return distance > 24;
        };
        // Runs of clip pixels, in points, merged over a single stray pixel.
        std::vector<std::pair<double, double>> drawn;
        const int64_t first =
            static_cast<int64_t>(CGRectGetMinX(row) +
                                 (viewport.header_width * scale)) +
            1;
        const int64_t last = static_cast<int64_t>(CGRectGetMaxX(row)) - 1;
        int64_t runStart = -1;
        for (int64_t x = first; x <= last + 1; ++x) {
            const bool clip =
                x <= last && x < static_cast<int64_t>(pixels.size()) &&
                (isClip(pixels[x]) ||
                 (x + 1 <= last && runStart >= 0 && isClip(pixels[x + 1])));
            if (clip && runStart < 0) runStart = x;
            if (!clip && runStart >= 0) {
                // A run under 1.5 pt is a border line, not a clip.
                if (x - runStart >= 1.5 * scale)
                    drawn.push_back({(runStart - CGRectGetMinX(row)) / scale,
                                     (x - CGRectGetMinX(row)) / scale});
                runStart = -1;
            }
        }
        std::string report;
        bool agree = drawn.size() == expected.size();
        for (size_t clip = 0; clip < expected.size(); ++clip) {
            char buffer[96];
            if (clip < drawn.size()) {
                std::snprintf(buffer, sizeof buffer,
                              " [%.1f-%.1f drawn, %.1f-%.1f expected]",
                              drawn[clip].first, drawn[clip].second,
                              expected[clip].first, expected[clip].second);
                agree =
                    agree &&
                    std::abs(drawn[clip].first - expected[clip].first) <= 1.5 &&
                    std::abs(drawn[clip].second - expected[clip].second) <= 1.5;
            } else {
                std::snprintf(buffer, sizeof buffer, " [missing]");
            }
            report += buffer;
        }
        UiSmokeCheck(
            agree,
            ("VQA R10 video track at index " + std::to_string(track.index) +
             ": " + std::to_string(drawn.size()) + " clips drawn for " +
             std::to_string(expected.size()) +
             ", each edge within 1.5 pt of its position, "
             "gaps included:" +
             report)
                .c_str());
        ++measuredTracks;
    }
    UiSmokeCheck(measuredTracks >= 2,
                 ("VQA R10: " + std::to_string(measuredTracks) +
                  " video tracks measured in the capture")
                     .c_str());
}

// REM1 -- docs/roadmap/REMOTION-PARITY-2026-09.md, scene C20. The words of
// a rush on disk, a montage whose jump cut drops two of them, then
// generate_captions called as the chat calls it. The pages land on one
// caption track, as caption_generation_tests freezes them, and Record shows
// each page's exact text from its first frame to its last, nothing in the
// silence between two pages, and the carton where the file draws it.
- (void)runUiQaTranscriptCaptionScenario {
    if (![self uiQaOpen:gUiSmokeSpeechTimelineId label:"C20"]) return;
    const std::filesystem::path transcripts =
        std::filesystem::absolute(self.documentPath.UTF8String ?: "")
            .parent_path() /
        ".cutmachine" / "transcripts";
    std::error_code directoryError;
    std::filesystem::create_directories(transcripts, directoryError);
    Transcript transcript;
    transcript.media_id = gUiSmokeSpeechSourceId;
    transcript.whisper_model = "ggml-large-v3.bin";
    transcript.language = "fr";
    transcript.source_rate = {25, 1};
    const struct {
        const char* text;
        int64_t start;
        int64_t end;
    } said[] = {
        {"On", 5, 12},       {"commence", 13, 30}, {"par", 31, 38},
        {"la", 39, 43},      {"lumière.", 44, 70}, {"euh", 105, 115},
        {"bref", 120, 135},  {"Puis", 155, 165},   {"on", 166, 172},
        {"cadre", 173, 190}, {"le", 191, 195},     {"visage.", 196, 225},
        {"Voilà.", 230, 245}};
    for (const auto& word : said)
        transcript.words.push_back(
            {word.text, {word.start, 25}, {word.end, 25}});
    std::string error;
    const bool saved = SaveAudioTranscript(
        (transcripts / (gUiSmokeSpeechSourceId + ".json")).string(), transcript,
        error);
    UiSmokeCheck(saved, ("VQA C20: the rush transcript is saved" +
                         (saved ? "" : ": " + error))
                            .c_str());
    if (!saved) return;
    // generate_captions measures widths with the ffmpeg on the PATH.
    const char* searchPath = std::getenv("PATH");
    setenv("PATH",
           (std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string() +
            ':' + (searchPath != nullptr ? searchPath : "/usr/bin"))
               .c_str(),
           1);

    const size_t applied = self.state->editLog.AppliedCount();
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("max_characters", mcp_json::Value::MakeInt(20));
    const McpToolRegistry registry;
    const McpToolCallOutcome outcome =
        registry.Call(*self.chatBackend, "generate_captions", arguments);
    [self refreshTimelineAfterEditFromPosition:{0, 25}];
    UiSmokeCheck(
        outcome.ok && self.state->editLog.AppliedCount() == applied + 1,
        ("VQA C20: generate_captions, called as the chat calls it, "
         "adds the pages in one edit" +
         (outcome.ok ? "" : ": " + outcome.message))
            .c_str());
    if (!outcome.ok) return;
    struct Page {
        std::string text;
        int64_t in = 0;
        int64_t end = 0;
    };
    std::vector<Page> pages;
    std::string listed;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "caption") continue;
        for (const DocumentClip& clip : track.clips) {
            pages.push_back(
                {clip.caption_text, clip.timeline_in.to_frames(25, 1),
                 clip.timeline_in.add(clip.duration).to_frames(25, 1)});
            listed += "[" + clip.caption_text + " " +
                      std::to_string(pages.back().in) + "+" +
                      std::to_string(pages.back().end - pages.back().in) + "]";
        }
    }
    UiSmokeCheck(listed == "[On commence 5+26][par la lumière. 31+39]"
                           "[Puis 100+16][on cadre le visage. 116+64]"
                           "[Voilà. 180+20]",
                 ("VQA C20: the pages are the ones caption_generation_tests "
                  "freezes: " +
                  listed)
                     .c_str());
    if (pages.empty()) return;

    const std::filesystem::path movie = [self uiQaExport:"c20-pages"];
    // The text a page boundary must show: the page holding the frame, or
    // nothing in a silence between two pages.
    const auto expectedAt = [&](int64_t frame) {
        for (const Page& page : pages)
            if (page.in <= frame && frame < page.end) return page.text;
        return std::string();
    };
    const auto shownAt = [&](int64_t frame) {
        [self requestTimelinePosition:{frame, 25}];
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        CMCaptionView* view = self.programCaptionView;
        return view.hidden ? std::string()
                           : std::string(view.captionText.UTF8String ?: "");
    };
    std::vector<int64_t> controls;
    for (const Page& page : pages) {
        if (page.in > 0) controls.push_back(page.in - 1);
        controls.push_back(page.in);
        controls.push_back(page.end - 1);
    }
    // A page's last frame is the eve of the next one when they meet.
    std::sort(controls.begin(), controls.end());
    controls.erase(std::unique(controls.begin(), controls.end()),
                   controls.end());
    for (const int64_t frame : controls) {
        const std::string expected = expectedAt(frame);
        const std::string shown = shownAt(frame);
        UiSmokeCheck(
            shown == expected,
            ("VQA C20 frame " + std::to_string(frame) + ": Record shows \"" +
             expected + "\" (shown \"" + shown + "\")")
                .c_str());
        const std::filesystem::path png =
            gUiQaOutputDirectory /
            ("c20-frame-" + std::to_string(frame) + "-export.png");
        UiQaCarton delivered;
        const bool read = !movie.empty() &&
                          UiQaExtractFrame(movie, frame, png) &&
                          UiQaMeasureCarton(png, CGRectNull, delivered);
        UiSmokeCheck(read && (delivered.fill.pixels > 0) == !expected.empty(),
                     ("VQA C20 frame " + std::to_string(frame) + ": the file " +
                      (expected.empty() ? "has no carton" : "has a carton"))
                         .c_str());
    }
    // One capture per page, at its middle: the carton on screen where the
    // file draws it.
    for (size_t index = 0; index < pages.size(); ++index) {
        const int64_t frame = (pages[index].in + pages[index].end) / 2;
        const std::string name = "c20-page-" + std::to_string(index);
        std::vector<bool> ignored;
        std::vector<float> monitor;
        std::vector<float> file;
        UiQaCarton shown;
        UiQaCarton delivered;
        const bool measured =
            [self uiQaMeasure:name
                        frame:frame
                        movie:movie
                       source:gUiSmokeSpeechSourceId
                  sourceFrame:frame < 100 ? frame : frame + 50
                      ignored:ignored
                      monitor:monitor
                         file:file] &&
            UiQaMeasureCarton(gUiQaOutputDirectory / (name + ".png"),
                              [self uiQaCaptureRect:[self uiQaRecordCanvas]],
                              shown) &&
            UiQaMeasureCarton(gUiQaOutputDirectory / (name + "-export.png"),
                              CGRectNull, delivered) &&
            shown.fill.pixels > 0 && delivered.fill.pixels > 0;
        char buffer[160];
        std::snprintf(buffer, sizeof buffer,
                      " (centre %.3f,%.3f on screen, %.3f,%.3f in the file)",
                      shown.fill.CentreX(), shown.fill.CentreY(),
                      delivered.fill.CentreX(), delivered.fill.CentreY());
        UiSmokeCheck(
            measured &&
                std::abs(shown.fill.CentreX() - delivered.fill.CentreX()) <=
                    0.01 &&
                std::abs(shown.fill.CentreY() - delivered.fill.CentreY()) <=
                    0.01,
            ("VQA C20 page \"" + pages[index].text +
             "\": the carton sits at the same place on screen and in the "
             "file, within 1 % of the frame" +
             buffer)
                .c_str(),
            ui_smoke::Needs::kVisibleWindow);
    }
    [self uiQaDiscard:movie];
    [self menuUndo:nil];
    UiSmokeCheck(self.state->editLog.AppliedCount() == applied,
                 "VQA C20: one undo takes the pages back");
}

// ITW4: three cartons, each measured in Record and in the exported file.
// Since LG5 (docs/roadmap/LEGENDES-2026-10.md) both are the caption
// renderer's picture, at two sizes: position, size and line breaks must
// agree, the outline must not thin the letters, and the shadow and the box
// must be where the style puts them.
- (void)runUiQaCaptionScenario {
    // LG5 -- measured off screen when the window is covered.
    struct OffscreenScope {
        OffscreenScope() { gUiQaDrawOffscreen = true; }
        ~OffscreenScope() { gUiQaDrawOffscreen = false; }
    } offscreenScope;
    if (![self uiQaOpen:gUiSmokeCaptionTimelineId label:"ITW4"]) return;
    const DocumentTrack* video = nullptr;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && !track.clips.empty() && video == nullptr)
            video = &track;
    if (video == nullptr) return;
    const Ulid greyId = video->clips.front().source_id;
    const std::filesystem::path movie = [self uiQaExport:"90-itw4-cartons"];
    const char* names[3] = {"contour", "fond et ombre", "replié"};
    const int expectedLines[3] = {1, 1, 2};
    for (int index = 0; index < 3; ++index) {
        const int64_t frame = (25 * index) + 10;
        const std::string name =
            "9" + std::to_string(index + 1) + "-itw4-" + std::to_string(index);
        std::vector<bool> ignored;
        std::vector<float> monitor;
        std::vector<float> file;
        if (![self uiQaMeasure:name
                         frame:frame
                         movie:movie
                        source:greyId
                   sourceFrame:frame
                       ignored:ignored
                       monitor:monitor
                          file:file])
            continue;
        UiQaCarton shown;
        UiQaCarton exported;
        const bool read =
            UiQaMeasureCarton(gUiQaOutputDirectory / (name + ".png"),
                              [self uiQaCaptureRect:[self uiQaRecordCanvas]],
                              shown) &&
            UiQaMeasureCarton(gUiQaOutputDirectory / (name + "-export.png"),
                              CGRectNull, exported);
        const std::string label = std::string("ITW4 carton ") + names[index];
        UiSmokeCheck(read && shown.fill.pixels > 0 && exported.fill.pixels > 0,
                     ("VQA " + label +
                      ": the text is found on screen and in "
                      "the file")
                         .c_str());
        if (!read || shown.fill.pixels == 0 || exported.fill.pixels == 0)
            continue;
        char buffer[256];
        std::snprintf(
            buffer, sizeof buffer,
            " (screen %.3f-%.3f x %.3f-%.3f, file %.3f-%.3f x %.3f-%.3f)",
            shown.fill.left, shown.fill.right, shown.fill.top,
            shown.fill.bottom, exported.fill.left, exported.fill.right,
            exported.fill.top, exported.fill.bottom);
        const double heightRatio = (shown.fill.bottom - shown.fill.top) /
                                   (exported.fill.bottom - exported.fill.top);
        const double widthRatio = (shown.fill.right - shown.fill.left) /
                                  (exported.fill.right - exported.fill.left);
        UiSmokeCheck(
            std::abs(shown.fill.CentreY() - exported.fill.CentreY()) <= 0.01 &&
                std::abs(shown.fill.CentreX() - exported.fill.CentreX()) <=
                    0.01,
            ("VQA " + label +
             ": the text sits at the same place on screen and in the file, "
             "within 1 % of the frame" +
             buffer)
                .c_str());
        UiQaExpectValue(label + ": text height on screen over in the file",
                        heightRatio, 0.9, 1.1);
        UiQaExpectValue(label + ": text width on screen over in the file",
                        widthRatio, 0.9, 1.1);
        UiSmokeCheck(
            shown.lines == expectedLines[index] &&
                exported.lines == expectedLines[index],
            ("VQA " + label + ": " + std::to_string(expectedLines[index]) +
             " line(s) on screen and in the file (screen " +
             std::to_string(shown.lines) + ", file " +
             std::to_string(exported.lines) + ")")
                .c_str());
        if (index == 0) {
            // LG5 -- the file's frame brought to the pixels Record shows it
            // in: at half the size, a letter has half the pixels and its
            // antialiased edge counts twice as much, so white shares only
            // compare at one size (0.74 against the full-size file, for a
            // drawing whose fill is laid last and cannot be eaten).
            const CGRect screenCrop =
                [self uiQaCaptureRect:[self uiQaRecordCanvas]];
            UiQaCarton exportedAtScreen;
            const bool resampled = UiQaMeasureCarton(
                gUiQaOutputDirectory / (name + "-export.png"), CGRectNull,
                exportedAtScreen, screenCrop.size);
            UiQaExpectValue(
                label + ": white of the letters on screen over in the file at "
                        "the screen's size, the outline eating none of it",
                resampled && exportedAtScreen.fill_share > 0.0
                    ? shown.fill_share / exportedAtScreen.fill_share
                    : 0.0,
                0.8, 1.25);
            UiSmokeCheck(
                shown.outline.pixels > 0 && exported.outline.pixels > 0,
                ("VQA " + label + ": the outline is drawn on both").c_str());
        }
        if (index == 1) {
            const auto shadowSide = [](const UiQaCarton& carton) {
                return carton.shadow.pixels > 0 &&
                       carton.shadow.CentreX() > carton.fill.CentreX() &&
                       carton.shadow.CentreY() > carton.fill.CentreY();
            };
            UiSmokeCheck(shadowSide(shown) && shadowSide(exported),
                         ("VQA " + label +
                          ": the shadow falls down and to the right, on "
                          "screen and in the file")
                             .c_str());
            std::snprintf(
                buffer, sizeof buffer,
                " (screen %.3f-%.3f x %.3f-%.3f, file %.3f-%.3f x %.3f-%.3f)",
                shown.box.left, shown.box.right, shown.box.top,
                shown.box.bottom, exported.box.left, exported.box.right,
                exported.box.top, exported.box.bottom);
            const bool boxes =
                shown.box.pixels > 0 && exported.box.pixels > 0 &&
                std::abs(shown.box.left - exported.box.left) <= 0.01 &&
                std::abs(shown.box.right - exported.box.right) <= 0.01 &&
                std::abs(shown.box.top - exported.box.top) <= 0.01 &&
                std::abs(shown.box.bottom - exported.box.bottom) <= 0.01;
            UiSmokeCheck(boxes, ("VQA " + label +
                                 ": the translucent box has the same edges "
                                 "on screen and in the file, within 1 %" +
                                 buffer)
                                    .c_str());
        }
    }
    const auto measureCarton = [&](const std::string& name, int64_t frame,
                                   const std::filesystem::path& file,
                                   UiQaCarton& shown, UiQaCarton& exported) {
        std::vector<bool> ignored;
        std::vector<float> monitor;
        std::vector<float> grid;
        return [self uiQaMeasure:name
                           frame:frame
                           movie:file
                          source:greyId
                     sourceFrame:frame
                         ignored:ignored
                         monitor:monitor
                            file:grid] &&
               UiQaMeasureCarton(gUiQaOutputDirectory / (name + ".png"),
                                 [self uiQaCaptureRect:[self uiQaRecordCanvas]],
                                 shown) &&
               UiQaMeasureCarton(gUiQaOutputDirectory / (name + "-export.png"),
                                 CGRectNull, exported) &&
               shown.fill.pixels > 0 && exported.fill.pixels > 0;
    };
    const auto placed = [](const UiQaCarton& shown, const UiQaCarton& file) {
        char buffer[160];
        std::snprintf(buffer, sizeof buffer,
                      " (centre %.3f,%.3f on screen, %.3f,%.3f in the file)",
                      shown.fill.CentreX(), shown.fill.CentreY(),
                      file.fill.CentreX(), file.fill.CentreY());
        const bool same =
            std::abs(shown.fill.CentreX() - file.fill.CentreX()) <= 0.01 &&
            std::abs(shown.fill.CentreY() - file.fill.CentreY()) <= 0.01 &&
            std::abs(((shown.fill.bottom - shown.fill.top) /
                      (file.fill.bottom - file.fill.top)) -
                     1.0) <= 0.1;
        return std::make_pair(same, std::string(buffer));
    };

    // ITW4 -- the window resized while a carton is shown: it follows the
    // picture at once.
    const NSRect original = self.window.frame;
    [self.window setFrame:NSMakeRect(NSMinX(original), NSMinY(original) + 80.0,
                                     NSWidth(original) - 260.0,
                                     NSHeight(original) - 160.0)
                  display:YES];
    [NSRunLoop.currentRunLoop
           runMode:NSDefaultRunLoopMode
        beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    // PRD37 -- ROADMAP.md. Narrower, the media panel's two buttons keep
    // their own room (QA of 3 October 2026: SOURCE slid over « IMPORTER DES
    // RUSHES… », its resizing mask set twice and the second one kept).
    {
        const NSRect source = self.sourceMonitorButton.frame;
        const NSRect import = self.assignMediaButton.frame;
        char detail[128];
        std::snprintf(detail, sizeof detail,
                      " (window %.0f points: import %.0f to %.0f, source %.0f "
                      "to %.0f)",
                      NSWidth(self.window.frame), NSMinX(import),
                      NSMaxX(import), NSMinX(source), NSMaxX(source));
        UiSmokeCheck(!NSIntersectsRect(source, import),
                     (std::string("PRD37: SOURCE stays beside IMPORTER DES "
                                  "RUSHES… in a narrower window") +
                      detail)
                         .c_str());
    }
    UiQaCarton resizedShown;
    UiQaCarton resizedFile;
    if (measureCarton("94-itw4-resized", 10, movie, resizedShown,
                      resizedFile)) {
        const auto [same, where] = placed(resizedShown, resizedFile);
        UiSmokeCheck(same, ("VQA ITW4 after a window resize, the carton sits "
                            "and sizes as in the file, within 1 %" +
                            where)
                               .c_str());
    }
    [self.window setFrame:original display:YES];
    [NSRunLoop.currentRunLoop
           runMode:NSDefaultRunLoopMode
        beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    [self uiQaDiscard:movie];

    // F2.3 and F2.2 -- a style applied from the Captions tab: Record and
    // the file wear it, and the Inspector shows the carton without a
    // conflict (a caption track has nothing to grade).
    const DocumentTrack* captions = nullptr;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "caption") captions = &track;
    const std::vector<CaptionStyle>& styles =
        self.state->document.sequence.caption_styles;
    if (captions == nullptr || captions->clips.empty() || styles.size() < 2)
        return;
    const Ulid carton = captions->clips.front().id;
    self.state->interaction->SelectClip(carton);
    [self updateSelectionInfo];
    [self.mediaTabStrip selectIndex:2];
    [self.captionStyleTable selectRowIndexes:[NSIndexSet indexSetWithIndex:1]
                        byExtendingSelection:NO];
    [self.applyCaptionStyleButton
        sendAction:self.applyCaptionStyleButton.action
                to:self.applyCaptionStyleButton.target];
    const DocumentClip* restyled = self.state->document.FindClip(carton);
    UiSmokeCheck(restyled && restyled->caption_group_id == styles[1].id &&
                     restyled->caption_text == "Carton contour",
                 "VQA F2.3: APPLIQUER LE STYLE from the Captions tab dresses "
                 "the carton in the boxed style, text kept");
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    [self updateSelectionInfo];
    NSMutableArray<NSSlider*>* sliders = [NSMutableArray array];
    CollectSliders(self.inspectorView, sliders);
    size_t enabled = 0;
    for (NSSlider* slider in sliders) enabled += slider.enabled ? 1 : 0;
    NSTextField* trackField =
        (NSTextField*)UiQaFindView(self.inspectorView, ^BOOL(NSView* view) {
          return [view isKindOfClass:NSTextField.class] &&
                 [((NSTextField*)view).stringValue hasPrefix:@"Sous-titres"];
        });
    UiSmokeCheck(trackField != nil && enabled == 0,
                 ("VQA F2.2: the Inspector shows the restyled carton as a "
                  "caption, with no picture control live (" +
                  std::to_string(enabled) + " enabled)")
                     .c_str());
    const std::filesystem::path restyledMovie =
        [self uiQaExport:"95-itw4-tab-style"];
    UiQaCarton tabShown;
    UiQaCarton tabFile;
    if (measureCarton("95-itw4-tab-style", 10, restyledMovie, tabShown,
                      tabFile)) {
        const auto [same, where] = placed(tabShown, tabFile);
        UiSmokeCheck(
            same && tabShown.box.pixels > 0 && tabFile.box.pixels > 0 &&
                std::abs(tabShown.box.top - tabFile.box.top) <= 0.01 &&
                std::abs(tabShown.box.bottom - tabFile.box.bottom) <= 0.01,
            ("VQA F2.3: the style applied from the tab is worn on "
             "screen and in the file, box included" +
             where)
                .c_str());
    }
    [self uiQaDiscard:restyledMovie];
    [self menuUndo:nil];
    self.state->interaction->SelectClip("");
    [self updateSelectionInfo];
    [self.mediaTabStrip selectIndex:0];
}

// F2.5, NTSC: a 29.97 sequence whose shot sits three minutes in, in a
// library whose first media runs at 25 i/s -- the cadence the playhead
// once stepped at, which put Record one frame, sometimes two, behind the
// timecode there. Stepped, measured against the file, and played.
- (void)runUiQaNtscScenario {
    if (![self uiQaOpen:gUiSmokeNtscTimelineId label:"NTSC"]) return;
    constexpr int64_t kFirst = 5380;
    constexpr int64_t kCount = 60;
    const MediaRate ntsc{30000, 1001};
    const MediaRate playhead = [self playheadFrameRate];
    UiSmokeCheck(playhead.num == ntsc.num && playhead.den == ntsc.den,
                 ("VQA NTSC: the playhead steps at the sequence's 30000/1001, "
                  "not the first media's 25 (" +
                  std::to_string(playhead.num) + "/" +
                  std::to_string(playhead.den) + ")")
                     .c_str());
    std::vector<UiQaShown> expected;
    for (int64_t frame = 0; frame < kCount; ++frame)
        expected.push_back({gUiSmokeNtscClipId, frame});
    size_t wrong = 0;
    size_t wrongLabels = 0;
    std::string first;
    for (int64_t frame = 0; frame < kCount; ++frame) {
        const UiQaShown shown = [self uiQaShowFrame:kFirst + frame];
        NSString* label =
            TimelineTimecode({(kFirst + frame) * ntsc.den, ntsc.num}, ntsc);
        const bool labelled =
            [self.timelineTimecodeLabel.stringValue isEqualToString:label];
        if ((shown != expected[frame] || !labelled) && first.empty())
            first = ": frame " + std::to_string(kFirst + frame) + " draws " +
                    std::to_string(shown.source) + " under " +
                    (self.timelineTimecodeLabel.stringValue.UTF8String ?: "");
        wrong += shown != expected[frame] ? 1 : 0;
        wrongLabels += labelled ? 0 : 1;
    }
    UiSmokeCheck(
        wrong == 0 && wrongLabels == 0,
        ("VQA NTSC: stepping from 00:02:59:10, Record draws each "
         "frame under its own timecode (" +
         std::to_string(wrong) + " frames, " + std::to_string(wrongLabels) +
         " timecodes wrong" + first + ")")
            .c_str());

    const std::filesystem::path movie = [self uiQaExport:"59-ntsc"];
    std::vector<float> monitor;
    std::vector<float> file;
    std::vector<float> later;
    std::vector<bool> ignored;
    if ([self uiQaMeasure:"59-ntsc-5392"
                    frame:kFirst + 12
                    movie:movie
                   source:self.state->document.FindClip(gUiSmokeNtscClipId)
                              ->source_id
              sourceFrame:12
                  ignored:ignored
                  monitor:monitor
                     file:file] &&
        [self uiQaFileGrid:"59-ntsc-5402"
                     movie:movie
                     frame:kFirst + 22
                      grid:later]) {
        UiQaExpectSame("NTSC frame 5392 (00:02:59:22): Record shows what the "
                       "file holds",
                       monitor, file, ignored);
        UiQaExpectDifferent("NTSC frame 5392 on screen, 5402 in the file",
                            monitor, later, ignored);
    }
    [self uiQaDiscard:movie];
    UiQaCheckPlayback("playback NTSC from 00:02:59:10, forwards",
                      [self uiQaPlay:1 from:kFirst], expected, 1,
                      gUiQaOutputDirectory / "59-play-ntsc.tsv", kFirst,
                      1000.0 * ntsc.den / ntsc.num);
}

// Real playback, at 1x, driven by the window's display timer: R3's 50 i/s
// shot at 1/2 forwards, the cadence timeline both ways, and a black dip
// centred on a cut both ways (R5).
- (void)runUiQaPlaybackScenario {
    const std::filesystem::path directory = gUiQaOutputDirectory;
    if ([self uiQaOpen:gUiSmokeSpeedTimelineId label:"playback R3 1/2"]) {
        std::vector<UiQaShown> expected;
        for (int64_t frame = 0; frame < 50; ++frame)
            expected.push_back({gUiSmokeSpeedClipId, frame});
        UiQaCheckPlayback("playback R3 50 i/s at 1/2, forwards",
                          [self uiQaPlay:1 from:0], expected, 1,
                          directory / "58-play-r3-half.tsv");
    }
    if ([self uiQaOpen:gUiSmokeCadenceTimelineId label:"playback R3"]) {
        const std::vector<UiQaShown> expected = UiQaCadenceFrames();
        UiQaCheckPlayback("playback R3 1/2, 2, 2/5, forwards",
                          [self uiQaPlay:1 from:0], expected, 1,
                          directory / "58-play-r3-cadences.tsv");
        UiQaCheckPlayback(
            "playback R3 1/2, 2, 2/5, backwards",
            [self uiQaPlay:-1 from:static_cast<int64_t>(expected.size()) - 1],
            expected, -1, directory / "58-play-r3-cadences-back.tsv");
    }
    if (![self uiQaOpen:gUiSmokeDipTimelineId label:"playback R5"]) return;
    Ulid trackId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && trackId.empty()) trackId = track.id;
    AddTransitionOperation add;
    add.transition.track_id = trackId;
    add.transition.left_clip_id = gUiSmokeDipFirstClipId;
    add.transition.right_clip_id = gUiSmokeDipSecondClipId;
    add.transition.type = "dip_to_color";
    add.transition.duration = {10, 25};
    add.transition.alignment = TransitionAlignment::Center;
    add.transition.color = "#000000";
    if (![self uiQaApply:add label:"playback R5: a black dip on the cut"])
        return;
    std::vector<UiQaShown> expected;
    for (int64_t frame = 0; frame < 100; ++frame)
        expected.push_back(
            {frame < 50 ? gUiSmokeDipFirstClipId : gUiSmokeDipSecondClipId,
             frame < 50 ? frame : frame + 2});
    UiQaCheckPlayback("playback R5 dip, forwards", [self uiQaPlay:1 from:0],
                      expected, 1, directory / "58-play-r5-dip.tsv");
    UiQaCheckPlayback("playback R5 dip, backwards", [self uiQaPlay:-1 from:99],
                      expected, -1, directory / "58-play-r5-dip-back.tsv");
    [self menuUndo:nil];
}

// R2: a 100 % to 115 % zoom eased over 12 frames and a 4-frame opacity
// fade, then the same shot trimmed and cut.
- (void)runUiQaAnimationScenario {
    if (![self uiQaOpen:gUiSmokeAnimationTimelineId label:"R2"]) return;
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeAnimationClipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    const Ulid clipId = gUiSmokeAnimationClipId;
    const std::filesystem::path still = [self uiQaExport:"60-r2-still"];

    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    ClipKeyframe zoomStart;
    zoomStart.id = GenerateUlid();
    zoomStart.time = {0, 25};
    zoomStart.value = {1, 1};
    zoomStart.interpolation = KeyframeInterpolation::CubicBezier;
    zoomStart.easing_x1 = {21, 50};
    zoomStart.easing_y1 = {0, 1};
    zoomStart.easing_x2 = {29, 50};
    zoomStart.easing_y2 = {1, 1};
    ClipKeyframe zoomEnd;
    zoomEnd.id = GenerateUlid();
    zoomEnd.time = {11, 25};
    zoomEnd.value = {23, 20};
    zoom.keyframes = {zoomStart, zoomEnd};
    ClipParameterAnimation fade;
    fade.parameter = ClipAnimatedParameter::Opacity;
    ClipKeyframe fadeStart;
    fadeStart.id = GenerateUlid();
    fadeStart.time = {20, 25};
    fadeStart.value = {1, 1};
    ClipKeyframe fadeEnd;
    fadeEnd.id = GenerateUlid();
    fadeEnd.time = {23, 25};
    fadeEnd.value = {0, 1};
    fade.keyframes = {fadeStart, fadeEnd};
    SetClipAnimationOperation animate;
    animate.clips = {{clipId, {zoom, fade}}};
    if (![self uiQaApply:animate
                   label:"R2: the zoom and the fade apply through the edit "
                         "log"]) {
        [self uiQaDiscard:still];
        return;
    }
    const std::filesystem::path movie = [self uiQaExport:"61-r2-animated"];
    std::vector<bool> ignored;
    constexpr int64_t kLast = 24;
    std::vector<float> monitor[kLast + 1];
    std::vector<float> file[kLast + 1];
    bool measured = true;
    for (int64_t frame = 0; frame <= kLast; ++frame) {
        if (frame > 11 && frame < 19) continue;
        char name[32];
        std::snprintf(name, sizeof name, "61-r2-frame-%02lld",
                      static_cast<long long>(frame));
        measured = [self uiQaMeasure:name
                               frame:frame
                               movie:movie
                              source:sourceId
                         sourceFrame:frame
                             ignored:ignored
                             monitor:monitor[frame]
                                file:file[frame]] &&
                   measured;
    }
    std::vector<float> stillEleven;
    std::vector<float> stillSix;
    measured = [self uiQaFileGrid:"60-r2-still-11"
                            movie:still
                            frame:11
                             grid:stillEleven] &&
               [self uiQaFileGrid:"60-r2-still-06"
                            movie:still
                            frame:6
                             grid:stillSix] &&
               measured;
    std::vector<float> readFrame;
    std::vector<float> readFrameFile;
    const bool read =
        [self uiQaReadFrame:clipId
                      frame:6
                       name:"61-r2-frame-06"
                       grid:readFrame] &&
        UiQaGrid(gUiQaOutputDirectory / "61-r2-frame-06-export.png", CGRectNull,
                 UiQaGridFor(self.state->document.sequence), readFrameFile,
                 UiQaReading::Raw);
    std::vector<float> stillSixRaw;
    const bool readStill =
        UiQaGrid(gUiQaOutputDirectory / "60-r2-still-06-export.png", CGRectNull,
                 UiQaGridFor(self.state->document.sequence), stillSixRaw,
                 UiQaReading::Raw);
    UiSmokeCheck(measured && read && readStill,
                 "VQA R2: every frame is measured on screen and in the "
                 "exported file",
                 ui_smoke::Needs::kVisibleWindow);
    if (measured && read && readStill) {
        for (int64_t frame = 0; frame <= kLast; ++frame) {
            if (frame > 11 && frame < 19) continue;
            UiQaExpectSame("R2 frame " + std::to_string(frame) +
                               ": Record shows what the file holds",
                           monitor[frame], file[frame], ignored);
        }
        // Frame by frame through the zoom: each monitor frame is nearer
        // its own exported frame than either neighbour, so none repeats and
        // none jumps back.
        bool ordered = true;
        for (int64_t frame = 0; frame <= 11; ++frame) {
            const double own =
                UiQaDistance(monitor[frame], file[frame], ignored);
            if (frame > 0)
                ordered =
                    ordered && own < UiQaDistance(monitor[frame],
                                                  file[frame - 1], ignored);
            if (frame < 11)
                ordered =
                    ordered && own < UiQaDistance(monitor[frame],
                                                  file[frame + 1], ignored);
        }
        UiSmokeCheck(ordered, "VQA R2: stepping through the zoom, every "
                              "monitor frame is its own exported frame, "
                              "not a neighbour");
        UiQaExpectDifferent("R2 end of the zoom on screen, unzoomed shot in "
                            "the file",
                            monitor[11], stillEleven, ignored);
        std::vector<bool> none(ignored.size(), false);
        UiQaExpectSame("R2 read_frame in the middle of the zoom composes "
                       "what the file holds",
                       readFrame, readFrameFile, none);
        UiQaExpectDifferent("R2 read_frame in the middle of the zoom, "
                            "unzoomed shot in the file",
                            readFrame, stillSixRaw, none);
        const double fading[4] = {
            UiQaMean(monitor[20], ignored), UiQaMean(monitor[21], ignored),
            UiQaMean(monitor[22], ignored), UiQaMean(monitor[23], ignored)};
        UiSmokeCheck(fading[0] > fading[1] && fading[1] > fading[2] &&
                         fading[2] > fading[3],
                     "VQA R2: the opacity fade darkens on every frame, no "
                     "step held");
        UiQaExpectValue("R2 the fade ends on black at its last key", fading[3],
                        0.0, 0.02);
    }

    // The same shot trimmed from the head by 5 frames: the zoom is tied to
    // the source, so frame 8 still shows what frame 8 showed.
    TrimClipOperation trim;
    trim.clip_id = clipId;
    trim.edge = TrimEdge::Head;
    trim.delta = {5, 25};
    std::vector<float> trimmed;
    std::vector<bool> trimIgnored;
    std::vector<float> unused;
    if ([self uiQaApply:trim
                  label:"R2: the animated shot is trimmed 5 frames "
                        "from the head"]) {
        if ([self uiQaMeasure:"62-r2-trimmed-08"
                        frame:8
                        movie:{}
                       source:sourceId
                  sourceFrame:8
                      ignored:trimIgnored
                      monitor:trimmed
                         file:unused] &&
            !file[8].empty())
            UiQaExpectSame("R2 trimmed head: frame 8 still shows the zoom's "
                           "frame 8",
                           trimmed, file[8], trimIgnored);
        [self menuUndo:nil];
    }

    // Cut in two in the middle of the zoom: the frames either side of the
    // cut are the uncut ones.
    SplitClipOperation split;
    split.clip_id = clipId;
    split.timeline_position = {6, 25};
    if ([self uiQaApply:split
                  label:"R2: the animated shot is cut at frame 6"]) {
        for (int64_t frame = 5; frame <= 7; ++frame) {
            std::vector<float> cut;
            std::vector<bool> cutIgnored;
            if ([self uiQaMeasure:"63-r2-cut-0" + std::to_string(frame)
                            frame:frame
                            movie:{}
                           source:sourceId
                      sourceFrame:frame
                          ignored:cutIgnored
                          monitor:cut
                             file:unused] &&
                !file[frame].empty())
                UiQaExpectSame("R2 cut in the zoom: frame " +
                                   std::to_string(frame) +
                                   " shows the uncut frame",
                               cut, file[frame], cutIgnored);
        }
        [self menuUndo:nil];
    }
    [self uiQaDiscard:still];
    [self uiQaDiscard:movie];
}

// R5: an opening from black, a closing to black, a white dip on a cut and
// the same dip through #bfbfbf.
- (void)runUiQaDipScenario {
    if (![self uiQaOpen:gUiSmokeDipTimelineId label:"R5"]) return;
    const DocumentClip* first =
        self.state->document.FindClip(gUiSmokeDipFirstClipId);
    if (first == nullptr) return;
    const Ulid sourceId = first->source_id;
    Ulid trackId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && trackId.empty()) trackId = track.id;
    // Frame k of the timeline reads source frame k on the first shot, and
    // 52 + (k - 50) on the second.
    const auto sourceFrameAt = [](int64_t frame) {
        return frame < 50 ? frame : frame + 2;
    };
    const std::filesystem::path bare = [self uiQaExport:"70-r5-bare"];
    const auto dip = [&](const Ulid& left, const Ulid& right, int64_t frames,
                         TransitionAlignment alignment, const char* color) {
        AddTransitionOperation add;
        add.transition.track_id = trackId;
        add.transition.left_clip_id = left;
        add.transition.right_clip_id = right;
        add.transition.type = "dip_to_color";
        add.transition.duration = {frames, 25};
        add.transition.alignment = alignment;
        add.transition.color = color;
        return add;
    };
    const auto measureAt =
        [&](const std::string& name, int64_t frame,
            const std::filesystem::path& movie, std::vector<bool>& ignored,
            std::vector<float>& monitor, std::vector<float>& file) {
            return [self uiQaMeasure:name
                               frame:frame
                               movie:movie
                              source:sourceId
                         sourceFrame:sourceFrameAt(frame)
                             ignored:ignored
                             monitor:monitor
                                file:file];
        };

    // Opening from black over 40 frames on the first shot.
    const AddTransitionOperation opening =
        dip({}, gUiSmokeDipFirstClipId, 40, TransitionAlignment::StartAtCut,
            "#000000");
    if ([self uiQaApply:opening label:"R5: an opening from black is laid"]) {
        const std::filesystem::path movie = [self uiQaExport:"71-r5-opening"];
        constexpr int64_t kFrames[] = {0, 10, 20, 30, 40};
        std::vector<float> monitor[5];
        std::vector<float> file[5];
        std::vector<bool> ignored;
        bool measured = true;
        for (size_t index = 0; index < 5; ++index)
            measured =
                measureAt("71-r5-opening-" + std::to_string(kFrames[index]),
                          kFrames[index], movie, ignored, monitor[index],
                          file[index]) &&
                measured;
        std::vector<float> bareForty;
        measured = [self uiQaFileGrid:"70-r5-bare-40"
                                movie:bare
                                frame:40
                                 grid:bareForty] &&
                   measured;
        if (measured) {
            for (size_t index = 0; index < 5; ++index)
                UiQaExpectSame("R5 opening frame " +
                                   std::to_string(kFrames[index]) +
                                   ": Record shows what the file holds",
                               monitor[index], file[index], ignored);
            UiQaExpectValue("R5 the opening's first frame is black",
                            UiQaMean(monitor[0], ignored), 0.0, 0.02);
            bool rising = true;
            for (size_t index = 1; index < 5; ++index)
                rising = rising && UiQaMean(monitor[index], ignored) >
                                       UiQaMean(monitor[index - 1], ignored);
            UiSmokeCheck(rising, "VQA R5: the opening lightens at every "
                                 "position measured, no step held");
            UiQaExpectSame("R5 the 41st frame is the bare shot", monitor[4],
                           bareForty, ignored);
            UiQaExpectDifferent("R5 a frame inside the opening, the bare "
                                "shot in the file",
                                monitor[2], bareForty, ignored);
        }
        [self uiQaDiscard:movie];
        [self menuUndo:nil];
    }

    // Closing to black over the last 50 frames of the second shot.
    const AddTransitionOperation closing =
        dip(gUiSmokeDipSecondClipId, {}, 50, TransitionAlignment::EndAtCut,
            "#000000");
    if ([self uiQaApply:closing label:"R5: a closing to black is laid"]) {
        const std::filesystem::path movie = [self uiQaExport:"72-r5-closing"];
        std::vector<float> monitor;
        std::vector<float> file;
        std::vector<bool> ignored;
        if (measureAt("72-r5-closing-99", 99, movie, ignored, monitor, file)) {
            UiQaExpectSame("R5 closing, last frame: Record shows what the "
                           "file holds",
                           monitor, file, ignored);
            UiQaExpectValue("R5 the last delivered frame is nearly black, "
                            "not black",
                            UiQaMean(monitor, ignored), 0.002, 0.06);
        }
        [self uiQaDiscard:movie];
        [self menuUndo:nil];
    }

    // A white dip of 6 frames centred on the cut at frame 50.
    const AddTransitionOperation white =
        dip(gUiSmokeDipFirstClipId, gUiSmokeDipSecondClipId, 6,
            TransitionAlignment::Center, "#ffffff");
    if ([self uiQaApply:white label:"R5: a white dip is laid on the cut"]) {
        const std::filesystem::path movie = [self uiQaExport:"73-r5-white"];
        std::vector<float> monitor[3];
        std::vector<float> file[3];
        std::vector<bool> ignored;
        bool measured = true;
        for (int64_t index = 0; index < 3; ++index)
            measured = measureAt("73-r5-white-" + std::to_string(49 + index),
                                 49 + index, movie, ignored, monitor[index],
                                 file[index]) &&
                       measured;
        if (measured) {
            for (int64_t index = 0; index < 3; ++index)
                UiQaExpectSame("R5 white dip frame " +
                                   std::to_string(49 + index) +
                                   ": Record shows what the file holds",
                               monitor[index], file[index], ignored);
            UiQaExpectValue("R5 the white is full on the incoming shot's "
                            "first frame",
                            UiQaMinimum(monitor[1], ignored), 0.97, 1.0);
            UiQaExpectValue("R5 the frame before the cut is not full white",
                            UiQaMinimum(monitor[0], ignored), 0.0, 0.95);
            UiQaExpectValue("R5 the frame after the cut is not full white",
                            UiQaMinimum(monitor[2], ignored), 0.0, 0.95);
        }
        [self uiQaDiscard:movie];

        // The same dip through the HLG graphics white, #bfbfbf.
        const Ulid dipId =
            self.state->document.sequence.transitions.empty()
                ? Ulid()
                : self.state->document.sequence.transitions.back().id;
        UpdateTransitionOperation grey;
        grey.transition_id = dipId;
        grey.type = "dip_to_color";
        grey.duration = {6, 25};
        grey.alignment = TransitionAlignment::Center;
        grey.color = "#bfbfbf";
        if ([self uiQaApply:grey label:"R5: the dip turns #bfbfbf"]) {
            const std::filesystem::path greyMovie =
                [self uiQaExport:"74-r5-bfbfbf"];
            std::vector<float> greyMonitor;
            std::vector<float> greyFile;
            if (measureAt("74-r5-bfbfbf-50", 50, greyMovie, ignored,
                          greyMonitor, greyFile)) {
                UiQaExpectSame("R5 #bfbfbf dip: Record shows what the file "
                               "holds",
                               greyMonitor, greyFile, ignored);
                const double expected = UiQaRecordLayerCode(0xbf) / 255.0;
                UiQaExpectValue("R5 the #bfbfbf dip shows the colour asked "
                                "for, as the Record layer shows 0xbf",
                                UiQaMean(greyMonitor, ignored), expected - 0.02,
                                expected + 0.02);
                UiQaExpectValue("R5 the #bfbfbf dip is flat",
                                UiQaMean(greyMonitor, ignored) -
                                    UiQaMinimum(greyMonitor, ignored),
                                0.0, 0.02);
            }
            [self uiQaDiscard:greyMovie];
            [self menuUndo:nil];
        }
        [self menuUndo:nil];
    }
    [self uiQaDiscard:bare];
}

// R6: a look imported and applied, a graded shot under it, another look,
// then none.
// R6: the rush in the Source monitor stays without the sequence's look.
- (void)uiQaCheckSourceWithoutLook:(const Ulid&)sourceId
                             frame:(int64_t)frame
                              bare:(const std::vector<float>&)bare
                            looked:(const std::vector<float>&)looked {
    [self openMediaIdentifierInSourceMonitor:CMStringFromUtf8(sourceId)];
    [self requestSourcePosition:{frame, 25}];
    bool drawn = false;
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
    while (!drawn && deadline.timeIntervalSinceNow > 0) {
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        drawn = self.state->sourceRendered.active &&
                self.state->sourceRendered.sourceId == sourceId &&
                self.state->sourceRendered.frame == frame;
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    [NSRunLoop.currentRunLoop
           runMode:NSDefaultRunLoopMode
        beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
    std::string captureError;
    const bool captured =
        drawn && CaptureUiQaWindow(self.window, "81b-r6-source", captureError);
    UiSmokeCheck(captured,
                 ("VQA R6: the Source monitor draws the rush's frame " +
                  std::to_string(frame) + " and is captured" +
                  (captureError.empty() ? "" : ": " + captureError))
                     .c_str(),
                 ui_smoke::Needs::kVisibleWindow);
    if (!captured) return;
    // The rush fitted into the Source monitor, as Record fits a sequence of
    // the same 16:9 shape; cells under its controls are left out.
    const DocumentSequence& sequence = self.state->document.sequence;
    const UiQaGridShape shape = UiQaGridFor(sequence);
    const NSRect bounds = self.sourceMonitorView.bounds;
    const ui::caption_overlay::Layout layout = ui::caption_overlay::Compute(
        sequence.width, sequence.height, bounds.size.width, bounds.size.height,
        0.0);
    const NSRect canvas =
        NSMakeRect(layout.canvas_x,
                   bounds.size.height - layout.canvas_y - layout.canvas_height,
                   layout.canvas_width, layout.canvas_height);
    std::vector<bool> ignored(static_cast<size_t>(shape.columns) * shape.rows,
                              false);
    const double cellWidth = canvas.size.width / shape.columns;
    const double cellHeight = canvas.size.height / shape.rows;
    for (NSView* subview in self.sourceMonitorView.subviews) {
        if (subview.hidden) continue;
        for (int row = 0; row < shape.rows; ++row)
            for (int column = 0; column < shape.columns; ++column)
                if (NSIntersectsRect(
                        NSMakeRect(NSMinX(canvas) + (column * cellWidth),
                                   NSMaxY(canvas) - ((row + 1) * cellHeight),
                                   cellWidth, cellHeight),
                        subview.frame))
                    ignored[(static_cast<size_t>(row) * shape.columns) +
                            column] = true;
    }
    std::vector<float> source;
    if (!UiQaGrid(gUiQaOutputDirectory / "81b-r6-source.png",
                  [self uiQaCaptureRect:canvas inView:self.sourceMonitorView],
                  shape, source)) {
        UiSmokeCheck(false, "VQA R6: the Source capture is read");
        return;
    }
    UiQaExpectSame("R6 the Source monitor shows the rush without the look: "
                   "the bare shot in the file",
                   source, bare, ignored);
    UiQaExpectDifferent("R6 Source on screen, the looked shot in the file",
                        source, looked, ignored);
}

// R6: the package copied elsewhere and read from there gives the same
// picture: the look travels inside the package, by digest, not by path.
- (void)uiQaCheckCopiedPackage:(const std::vector<float>&)looked
                          bare:(const std::vector<float>&)bare {
    [self drainProjectCommits];
    const std::filesystem::path project(self.documentPath.UTF8String ?: "");
    const std::filesystem::path copy =
        gUiSmokeRoot / "copie" / project.parent_path().filename();
    std::error_code copyError;
    std::filesystem::remove_all(copy.parent_path(), copyError);
    std::filesystem::create_directories(copy.parent_path(), copyError);
    std::filesystem::copy(project.parent_path(), copy,
                          std::filesystem::copy_options::recursive, copyError);
    const std::filesystem::path copiedProject = copy / project.filename();
    Project copied;
    std::string error;
    const bool loaded =
        !copyError && LoadStoredProject(copiedProject.string(), copied, error);
    const std::filesystem::path movie =
        gUiQaOutputDirectory / "81c-r6-copied.mp4";
    std::vector<float> grid;
    const bool exported =
        loaded &&
        UiQaExportMovie(copied.MakeDocument(gUiSmokeLookTimelineId),
                        copiedProject.string(), movie, error) &&
        [self uiQaFileGrid:"81c-r6-copied" movie:movie frame:12 grid:grid];
    UiSmokeCheck(exported, ("VQA R6: the package copied to another path "
                            "opens and exports" +
                            (error.empty() ? "" : ": " + error))
                               .c_str());
    if (exported) {
        const std::vector<bool> none(grid.size() / 3, false);
        UiQaExpectSame("R6 the copied package gives the same looked picture",
                       grid, looked, none);
        UiQaExpectDifferent("R6 the copied package's picture, the bare shot",
                            grid, bare, none);
    }
    [self uiQaDiscard:movie];
    std::filesystem::remove_all(copy.parent_path(), copyError);
}

// R6: the look's .cube removed from the package by hand. The export is
// refused and names it; the monitor, once it reads the look again (as at
// the next opening), shows the film without it -- the known gap.
- (void)uiQaCheckMissingCube:(const std::vector<float>&)bare
                     measure:
                         (const std::function<bool(
                              const std::string&, const std::filesystem::path&,
                              std::vector<float>&, std::vector<float>&)>&)
                             measure {
    const SequenceLut look = self.state->document.sequence.look_lut;
    const std::filesystem::path package =
        std::filesystem::path(self.documentPath.UTF8String ?: "").parent_path();
    const std::filesystem::path cube =
        package / LookLutPackagePath(look.digest);
    const std::filesystem::path aside = cube.string() + ".aside";
    std::error_code moveError;
    std::filesystem::rename(cube, aside, moveError);
    UiSmokeCheck(!moveError, "VQA R6: the look's .cube is taken out of the "
                             "package");
    if (moveError) return;
    std::string error;
    const std::filesystem::path movie =
        gUiQaOutputDirectory / "83b-r6-missing.mp4";
    const bool exported = UiQaExportMovie(
        self.state->document, self.documentPath.UTF8String ?: "", movie, error);
    const std::string file = LookLutPackagePath(look.digest);
    UiSmokeCheck(!exported && (error.find(file) != std::string::npos ||
                               error.find(look.name) != std::string::npos),
                 ("VQA R6: without its .cube the export is refused and names "
                  "it: " +
                  error)
                     .c_str());
    [self uiQaDiscard:movie];
    [self uiQaApply:SetSequenceLutOperation {}
              label:"R6: the look is cleared, as a reopening would find it"];
    // A tick of the display, as between two edits made by hand: Record
    // reads the look again only when it sees its digest change.
    self.state->overlayDirty = true;
    [self presentNearestFrameAtDeadline:NO];
    [self uiQaApply:SetSequenceLutOperation{look}
              label:"R6: and set again while its .cube is missing"];
    std::vector<float> monitor;
    std::vector<float> unused;
    if (measure("83c-r6-missing", {}, monitor, unused))
        UiQaExpectSame("R6 missing .cube: Record shows the film without its "
                       "look",
                       monitor, bare,
                       std::vector<bool>(bare.size() / 3, false));
    std::filesystem::rename(aside, cube, moveError);
    self.state->lookLut = LoadedLookLut{};
    self.state->overlayDirty = true;
}

// R6: under colour management, a look authored in ACEScct -- a contrast
// around mid grey -- acts on the log values the same way in Record and in
// the file, down to the blacks and up to the whites.
- (void)uiQaCheckAcescctLook:
            (const std::function<
                bool(const std::string&, const std::filesystem::path&,
                     std::vector<float>&, std::vector<float>&)>&)measure
                        bare:(const std::vector<float>&)bare {
    (void)bare;
    SetColorManagementOperation managed;
    managed.settings.enabled = true;
    if (![self uiQaApply:managed label:"R6: colour management on"]) return;
    // The shot under colour management without a look: the reference the
    // look is told apart from.
    const std::filesystem::path managedBare =
        [self uiQaExport:"85a-r6-managed-bare"];
    std::vector<float> managedBareMonitor;
    std::vector<float> managedBareFile;
    const bool measuredManaged = measure("85a-r6-managed-bare", managedBare,
                                         managedBareMonitor, managedBareFile);
    [self uiQaDiscard:managedBare];
    if (measuredManaged)
        UiQaExpectSame("R6 colour management without a look: Record shows "
                       "what the file holds",
                       managedBareMonitor, managedBareFile,
                       std::vector<bool>(managedBareFile.size() / 3, false));
    const std::filesystem::path cube = gUiSmokeRoot / "contraste-acescct.cube";
    const double gains[3] = {2.5, 2.5, 2.5};
    const double offsets[3] = {0.0, 0.0, 0.0};
    SequenceLut lut;
    std::string error;
    const bool imported =
        UiQaWriteLookCube(cube, gains, offsets) &&
        ImportLookLutIntoPackage(
            self.documentPath.UTF8String ?: "", cube.string(), "acescct",
            self.state->document.color_management, lut, error);
    UiSmokeCheck(imported, ("VQA R6: an ACEScct look is imported under colour "
                            "management" +
                            (imported ? "" : ": " + error))
                               .c_str());
    if (imported &&
        [self uiQaApply:SetSequenceLutOperation{lut}
                  label:"R6: the ACEScct look becomes the sequence look"]) {
        const std::filesystem::path movie = [self uiQaExport:"85-r6-acescct"];
        std::vector<float> monitor;
        std::vector<float> file;
        if (measure("85-r6-acescct", movie, monitor, file)) {
            const std::vector<bool> none(monitor.size() / 3, false);
            // Known gap, measured on 23 September 2026: under colour
            // management Record and the file already differ by 0.022
            // without any look (0.005 without colour management), and a
            // strong log contrast amplifies it. Asserted against that
            // measured bound, and reported, rather than hidden in the
            // 0.03 every other parity uses.
            UiQaExpectValue("R6 ACEScct look: Record against the file, "
                            "colour management's own gap amplified by the "
                            "contrast",
                            UiQaDistance(monitor, file, none), 0.0, 0.04);
            if (measuredManaged)
                UiQaExpectDifferent("R6 ACEScct look on screen, the same shot "
                                    "under colour management without it",
                                    monitor, managedBareFile, none);
            const auto brightest = [](const std::vector<float>& grid) {
                return grid.empty()
                           ? 0.0
                           : *std::max_element(grid.begin(), grid.end());
            };
            char buffer[160];
            std::snprintf(buffer, sizeof buffer,
                          " (blacks %.3f on screen, %.3f in the file; whites "
                          "%.3f and %.3f)",
                          UiQaMinimum(monitor, none), UiQaMinimum(file, none),
                          brightest(monitor), brightest(file));
            UiSmokeCheck(
                std::abs(UiQaMinimum(monitor, none) -
                         UiQaMinimum(file, none)) <= 0.02 &&
                    std::abs(brightest(monitor) - brightest(file)) <= 0.02,
                ("VQA R6 ACEScct look: Record crushes and clips no more than "
                 "the file" +
                 std::string(buffer))
                    .c_str());
        }
        [self uiQaDiscard:movie];
        [self menuUndo:nil];
    }
    [self menuUndo:nil];
}

- (void)runUiQaLookScenario {
    if (![self uiQaOpen:gUiSmokeLookTimelineId label:"R6"]) return;
    const DocumentClip* shot =
        self.state->document.FindClip(gUiSmokeLookClipId);
    if (shot == nullptr) return;
    const Ulid sourceId = shot->source_id;
    constexpr int64_t kFrame = 12;
    std::vector<bool> ignored;
    const auto measure =
        [&](const std::string& name, const std::filesystem::path& movie,
            std::vector<float>& monitor, std::vector<float>& file) {
            return [self uiQaMeasure:name
                               frame:kFrame
                               movie:movie
                              source:sourceId
                         sourceFrame:kFrame
                             ignored:ignored
                             monitor:monitor
                                file:file];
        };
    const auto importLook = [&](const char* name, const double gains[3],
                                const double offsets[3]) {
        const std::filesystem::path cube = gUiSmokeRoot / name;
        SequenceLut lut;
        std::string error;
        const bool imported =
            UiQaWriteLookCube(cube, gains, offsets) &&
            ImportLookLutIntoPackage(
                self.documentPath.UTF8String ?: "", cube.string(), "",
                self.state->document.color_management, lut, error);
        UiSmokeCheck(imported, ("VQA R6: " + std::string(name) +
                                " is imported into the package" +
                                (imported ? "" : ": " + error))
                                   .c_str());
        return imported && [self uiQaApply:SetSequenceLutOperation{lut}
                                     label:"R6: " + std::string(name) +
                                           " becomes the sequence look"];
    };

    const std::filesystem::path bare = [self uiQaExport:"80-r6-bare"];
    std::vector<float> bareMonitor;
    std::vector<float> bareFile;
    const bool measuredBare =
        measure("80-r6-bare", bare, bareMonitor, bareFile);
    // The RGB parade over Record, as the scope menu turns it on.
    const auto scope = [&](const std::string& name, std::vector<float>& grid) {
        self.state->programVideoScope = VideoScopeMode::ParadeRgb;
        self.state->overlayDirty = true;
        std::vector<float> unused;
        const bool shown = measure(name, {}, grid, unused);
        self.state->programVideoScope = VideoScopeMode::Off;
        self.state->overlayDirty = true;
        return shown;
    };
    std::vector<float> bareScope;
    const bool scopedBare = scope("80b-r6-scope-bare", bareScope);

    const double warmGains[3] = {1.15, 1.0, 0.85};
    const double warmOffsets[3] = {0.2, 0.04, -0.2};
    if (measuredBare && importLook("chaud.cube", warmGains, warmOffsets)) {
        const std::filesystem::path movie = [self uiQaExport:"81-r6-look"];
        std::vector<float> monitor;
        std::vector<float> file;
        if (measure("81-r6-look", movie, monitor, file)) {
            UiQaExpectSame("R6 look: Record shows what the file holds", monitor,
                           file, ignored);
            UiQaExpectDifferent("R6 the look reaches Record at once: look on "
                                "screen, bare shot in the file",
                                monitor, bareFile, ignored);
            std::vector<float> lookScope;
            if (scopedBare && scope("81e-r6-scope-look", lookScope))
                UiQaExpectDifferent("R6 the RGB parade with the look, the "
                                    "parade of the bare shot",
                                    lookScope, bareScope, ignored);
            [self uiQaCheckSourceWithoutLook:sourceId
                                       frame:kFrame
                                        bare:bareFile
                                      looked:file];
            [self uiQaCheckCopiedPackage:file bare:bareFile];
        }

        // A shot graded under the look: the grade first, the look after.
        SetClipEffectsOperation grade;
        grade.clip_id = gUiSmokeLookClipId;
        // Four stops down: a grade a grid of mean colours cannot miss,
        // where a warmer white balance moved it by 0.026.
        ClipEffect darker;
        darker.type = "color.exposure";
        darker.params["amount"] = {-4, 1};
        grade.effects = {darker};
        if ([self uiQaApply:grade
                      label:"R6: the shot is graded under the look"]) {
            const std::filesystem::path graded =
                [self uiQaExport:"82-r6-graded-look"];
            std::vector<float> gradedMonitor;
            std::vector<float> gradedFile;
            if (measure("82-r6-graded-look", graded, gradedMonitor,
                        gradedFile)) {
                UiQaExpectSame("R6 grade under the look: Record shows what "
                               "the file holds",
                               gradedMonitor, gradedFile, ignored);
                UiQaExpectDifferent("R6 graded shot on screen, ungraded one "
                                    "in the file",
                                    gradedMonitor, file, ignored);
            }
            [self uiQaDiscard:graded];
            [self menuUndo:nil];
        }

        const double coolGains[3] = {0.9, 1.0, 1.1};
        const double coolOffsets[3] = {-0.2, 0.06, 0.2};
        if (importLook("froid.cube", coolGains, coolOffsets)) {
            const std::filesystem::path cool = [self uiQaExport:"83-r6-other"];
            std::vector<float> coolMonitor;
            std::vector<float> coolFile;
            if (measure("83-r6-other", cool, coolMonitor, coolFile)) {
                UiQaExpectSame("R6 another look: Record shows what the file "
                               "holds",
                               coolMonitor, coolFile, ignored);
                UiQaExpectDifferent("R6 Record follows the new look: new "
                                    "look on screen, old one in the file",
                                    coolMonitor, file, ignored);
            }
            [self uiQaDiscard:cool];
            [self uiQaCheckMissingCube:bareFile measure:measure];
        }
        if ([self uiQaApply:SetSequenceLutOperation {}
                      label:"R6: the look is cleared"]) {
            std::vector<float> clearedMonitor;
            std::vector<float> unused;
            if (measure("84-r6-cleared", {}, clearedMonitor, unused))
                UiQaExpectSame("R6 cleared: Record shows the bare file, no "
                               "look held",
                               clearedMonitor, bareFile, ignored);
        }
        [self uiQaCheckAcescctLook:measure bare:bareFile];
        [self uiQaDiscard:movie];
    }
    [self uiQaDiscard:bare];
}

// PRD24 -- ROADMAP.md. What a clip carries, read back from the display list
// the window draws. A timeline of ordinary shots is given a speed, a
// framing, an animation, a grade, a motion clip and a multicam group through
// the edit log, and each rectangle must then say so: the right words, inside
// the rectangle the pointer hits (VisibleTimelineClips), above the clip's
// name and clear of the sync badge -- while a shot nothing was done to says
// nothing, and its commands are the ones it had.
//
// Runs in both modes. With CUTMACHINE_UI_QA_DIR it also keeps two captures
// of the window, for whoever judges what the list looks like. The timeline
// and the view it found are put back.
- (void)runUiSmokeClipSignageScenario {
    const Ulid previousTimeline = self.state->activeTimelineId;
    const TimelineViewport previousViewport = self.state->viewport;
    const auto activate = [self](const Ulid& timelineId) {
        return [self
            activateTimelineIdentifier:[NSString
                                           stringWithUTF8String:timelineId
                                                                    .c_str()]];
    };
    const BOOL opened = activate(gUiSmokeSignageTimelineId);
    UiSmokeCheck(opened, "PRD24: the signage timeline opens");
    if (!opened) return;
    [self requestTimelinePosition:{0, 25}];

    const double top = [self videoHeight];
    const auto sameColor = [](const ui::theme::Color& left,
                              const ui::theme::Color& right) {
        return left.r == right.r && left.g == right.g && left.b == right.b &&
               left.a == right.a;
    };
    const auto sameCommand = [&](const MetalDrawCommand& left,
                                 const MetalDrawCommand& right) {
        if (left.index() != right.index()) return false;
        if (const MetalRect* rect = std::get_if<MetalRect>(&left)) {
            const MetalRect& other = std::get<MetalRect>(right);
            return rect->x == other.x && rect->y == other.y &&
                   rect->width == other.width && rect->height == other.height &&
                   rect->red == other.red && rect->green == other.green &&
                   rect->blue == other.blue && rect->alpha == other.alpha &&
                   rect->bottom_red == other.bottom_red &&
                   rect->bottom_green == other.bottom_green &&
                   rect->bottom_blue == other.bottom_blue &&
                   rect->bottom_alpha == other.bottom_alpha;
        }
        if (const MetalText* text = std::get_if<MetalText>(&left)) {
            const MetalText& other = std::get<MetalText>(right);
            return text->x == other.x && text->y == other.y &&
                   text->max_width == other.max_width &&
                   text->point_size == other.point_size &&
                   text->text == other.text &&
                   sameColor(text->color, other.color) &&
                   text->face == other.face && text->bold == other.bold;
        }
        const MetalIcon& icon = std::get<MetalIcon>(left);
        const MetalIcon& other = std::get<MetalIcon>(right);
        return icon.x == other.x && icon.y == other.y &&
               icon.width == other.width && icon.height == other.height &&
               icon.name == other.name && sameColor(icon.color, other.color);
    };
    // The display list as the window draws it, and as it was before this
    // ticket: without the badges and the veil.
    const auto build = [&](bool signage) {
        gUiSmokeClipSignageOff = !signage;
        TimelineRenderData data = [self timelineRenderData];
        gUiSmokeClipSignageOff = false;
        return data;
    };

    // The whole timeline in view at a scale the fixture was written for,
    // whatever the window: the widths below do not depend on it.
    const auto frame = [&]() {
        [self fitTimelineToViewportWidth];
        self.state->viewport.pixels_per_second = 40.0;
        self.state->overlayDirty = true;
    };
    // A timeline that carries none of it: the sync badge of the late sound
    // is there, as it always was, and not one command more.
    frame();
    {
        const TimelineRenderData drawn = build(true);
        const TimelineRenderData before = build(false);
        bool identical = drawn.overlays.size() == before.overlays.size();
        for (size_t index = 0; identical && index < drawn.overlays.size();
             ++index)
            identical =
                sameCommand(drawn.overlays[index], before.overlays[index]);
        const bool driftSaid = std::any_of(
            drawn.overlays.begin(), drawn.overlays.end(),
            [](const MetalDrawCommand& command) {
                const MetalText* text = std::get_if<MetalText>(&command);
                return text && text->text == "+3f";
            });
        UiSmokeCheck(identical && driftSaid,
                     ("PRD24: a timeline whose clips carry nothing has the "
                      "display list it had, command for command, sync badge "
                      "included (" +
                      std::to_string(drawn.overlays.size()) + " against " +
                      std::to_string(before.overlays.size()) + ")")
                         .c_str());
    }

    const auto apply = [&](Operation operation, const std::string& label) {
        EditError editError = EditError::None;
        std::string message;
        const BOOL applied =
            [self applyAndPersistTimelineOperation:std::move(operation)
                                             error:editError
                                           message:message];
        UiSmokeCheck(
            applied,
            ("PRD24: " + label + (applied ? "" : ": " + message)).c_str());
        return static_cast<bool>(applied);
    };
    const auto speed = [](const Ulid& clipId, EffectParamValue value) {
        SetClipSpeedOperation operation;
        operation.clip_id = clipId;
        operation.speed = value;
        return operation;
    };
    ClipFraming fill;
    fill.mode = FramingMode::Fill;
    fill.scale = {11, 10};
    const auto fade = []() {
        ClipParameterAnimation animation;
        animation.parameter = ClipAnimatedParameter::Opacity;
        ClipKeyframe start;
        start.id = GenerateUlid();
        start.time = {0, 25};
        start.value = {1, 2};
        ClipKeyframe end;
        end.id = GenerateUlid();
        end.time = {4, 25};
        end.value = {1, 1};
        animation.keyframes = {start, end};
        return std::vector<ClipParameterAnimation>{animation};
    };
    const auto grade = [](const Ulid& clipId) {
        SetClipEffectsOperation operation;
        operation.clip_id = clipId;
        ClipEffect exposure;
        exposure.type = "color.exposure";
        exposure.params["amount"] = {1, 3};
        operation.effects = {exposure};
        return operation;
    };
    const Ulid loaded[] = {gUiSmokeSignageLoadedClipId,
                           gUiSmokeSignageTightClipId,
                           gUiSmokeSignageSliverClipId};
    bool edited =
        apply(speed(gUiSmokeSignageFastClipId, {2, 1}),
              "a linked shot and its sound are set to 2 through the edit "
              "log") &&
        apply(speed(gUiSmokeSignageSlowClipId, {1, 2}),
              "a shot is set to 1/2") &&
        apply(SetClipFramingOperation{{{gUiSmokeSignageFramedClipId, fill}}},
              "a shot is framed") &&
        apply(grade(gUiSmokeSignageGradedClipId), "a shot is graded") &&
        apply(SetClipAnimationOperation{{{gUiSmokeSignageAnimatedClipId,
                                          fade()}}},
              "a shot is animated");
    for (const Ulid& clipId : loaded)
        edited = edited &&
                 apply(speed(clipId, {1, 2}),
                       "a shot that will carry everything is set to 1/2") &&
                 apply(grade(clipId), "and graded");
    edited = edited &&
             apply(
                 SetClipFramingOperation{
                     {{loaded[0], fill}, {loaded[1], fill}, {loaded[2], fill}}},
                 "the three of them are framed in one edit") &&
             apply(SetClipAnimationOperation{{{loaded[0], fade()},
                                              {loaded[1], fade()},
                                              {loaded[2], fade()}}},
                   "and animated in one edit");
    Ulid motionClipId;
    if (const DocumentTrack* picture =
            self.state->document.FindTrackForClip(gUiSmokeSignagePlainClipId)) {
        InsertMotionClipOperation insert;
        insert.track_id = picture->id;
        insert.composition_id = gUiSmokeMotionCompositionId;
        insert.source_in = {0, 25};
        insert.duration = {40, 25};
        insert.timeline_in = {415, 25};
        insert.clip_id = GenerateUlid();
        motionClipId = insert.clip_id;
        edited = edited && apply(insert, "a motion clip ends the track");
    } else {
        edited = false;
    }
    {
        AddMulticamGroupOperation group;
        EditError groupError = EditError::None;
        std::string groupMessage;
        const bool built = BuildAddMulticamGroup(
            self.state->document, "QA",
            {{gUiSmokeSignageActiveAngleClipId, "Large"},
             {gUiSmokeSignageHiddenAngleClipId, "Serré"}},
            gUiSmokeSignageActiveAngleClipId, group, groupError, groupMessage);
        edited = edited && built &&
                 apply(group, "two stacked shots become the angles of a "
                              "group, the lower one active");
    }
    if (!edited) {
        UiSmokeCheck(false, "PRD24: the signage timeline is edited");
        activate(previousTimeline);
        self.state->viewport = previousViewport;
        self.state->overlayDirty = true;
        return;
    }
    [self refreshTimelineAfterEditFromPosition:{0, 25}];
    frame();

    const double width = self.metalView.bounds.size.width;
    const double trackHeight = self.state->viewport.track_height;
    char widthText[32];
    std::snprintf(widthText, sizeof widthText, "%.0f", width);
    UiSmokeCheck(self.state->viewport.TimeToX(self.state->duration) <= width,
                 (std::string("PRD24: the timeline panel shows the whole "
                              "signage timeline at 40 points a second (") +
                  widthText + " points wide)")
                     .c_str());
    const TimelineRenderData data = build(true);
    const TimelineRenderData bare = build(false);
    const std::vector<TimelineClipRect> rects = VisibleTimelineClips(
        self.state->document, self.state->viewport, width, {}, std::nullopt);

    // What shows of a clip, from the geometry the pointer is tested against.
    struct Span {
        bool found = false;
        double left = 0.0;
        double right = 0.0;
        double top = 0.0;
    };
    const auto spanOf = [&](const Ulid& clipId) {
        Span span;
        for (const TimelineClipRect& rect : rects) {
            if (rect.clip_id != clipId) continue;
            span.found = true;
            span.left = std::max(std::min(rect.x, rect.x + rect.width),
                                 self.state->viewport.header_width);
            span.right = std::min(std::max(rect.x, rect.x + rect.width), width);
            span.top = top + rect.y;
        }
        return span;
    };
    const auto onTrackOf = [&](const Span& span, const MetalRect& rect) {
        return rect.y >= span.top && rect.y < span.top + trackHeight - 3.0;
    };
    // A badge is a plate of the badge colour and the label drawn right
    // after it. Every plate that touches the clip's span is the clip's, so
    // that one sticking out of it is caught below rather than left out.
    struct DrawnBadge {
        MetalRect plate;
        MetalText label;
    };
    const auto badgesOf = [&](const TimelineRenderData& list,
                              const Span& span) {
        std::vector<DrawnBadge> badges;
        const ui::theme::Color fill = ui::theme::kClipBadgeFill;
        for (size_t index = 0; index + 1 < list.overlays.size(); ++index) {
            const MetalRect* rect =
                std::get_if<MetalRect>(&list.overlays[index]);
            const MetalText* text =
                std::get_if<MetalText>(&list.overlays[index + 1]);
            if (!rect || !text || !span.found || !onTrackOf(span, *rect))
                continue;
            if (rect->red != fill.r || rect->green != fill.g ||
                rect->blue != fill.b || rect->alpha != fill.a)
                continue;
            if (rect->x + rect->width <= span.left || rect->x >= span.right)
                continue;
            badges.push_back({*rect, *text});
        }
        return badges;
    };
    const auto reads = [&](const Ulid& clipId) {
        std::string joined;
        for (const DrawnBadge& badge : badgesOf(data, spanOf(clipId)))
            joined += (joined.empty() ? "" : " ") + badge.label.text;
        return joined;
    };
    const auto expectReads = [&](const Ulid& clipId, const std::string& what,
                                 const std::string& expected) {
        const Span span = spanOf(clipId);
        const std::string read = reads(clipId);
        char spanText[32];
        std::snprintf(spanText, sizeof spanText, "%.1f",
                      span.right - span.left);
        UiSmokeCheck(span.found && read == expected,
                     ("PRD24: " + what + " reads \"" + expected +
                      "\" on its rectangle (\"" + read + "\", " + spanText +
                      " points wide)")
                         .c_str());
    };
    const std::string gradeLabel = ClipBadgeLabel(ClipBadgeKind::Grade);
    expectReads(gUiSmokeSignagePlainClipId, "a shot nothing was done to", "");
    expectReads(gUiSmokeSignageFastClipId, "an accelerated shot", "2x");
    expectReads(gUiSmokeSignageSoundClipId, "its linked sound", "2x");
    expectReads(gUiSmokeSignageSlowClipId, "a slowed shot", "50%");
    expectReads(gUiSmokeSignageFramedClipId, "a framed shot", "CADRE");
    expectReads(gUiSmokeSignageGradedClipId, "a graded shot", gradeLabel);
    expectReads(gUiSmokeSignageAnimatedClipId, "an animated shot", "ANIM");
    expectReads(motionClipId, "a motion clip", "MOTION");
    expectReads(gUiSmokeSignageLoadedClipId,
                "a shot that carries all four, wide enough for them",
                "50% ANIM CADRE " + gradeLabel);
    expectReads(gUiSmokeSignageTightClipId,
                "the same on a narrow shot: the speed outlasts the rest",
                "50%");
    expectReads(gUiSmokeSignageSliverClipId,
                "the same on a shot too narrow for any badge", "");
    expectReads(gUiSmokeSignageActiveAngleClipId, "the active angle", "");
    expectReads(gUiSmokeSignageHiddenAngleClipId, "the hidden angle", "");

    // The face the atlas rasterises a label in (MetalUiAtlas.mm: twice the
    // size, two pixels of padding round each cell), measured here so that
    // the tokens the layout was written with are held to the real font.
    NSFont* atlasFace =
        [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeCaption * 2.0
                                    weight:NSFontWeightBold];
    const double advance =
        [@"0000000000" sizeWithAttributes:@{NSFontAttributeName : atlasFace}]
            .width /
        20.0;
    const double cellHeight =
        (std::ceil(atlasFace.ascender - atlasFace.descender +
                   atlasFace.leading) +
         4.0) /
        2.0;
    const double baseline = cellHeight - ((2.0 - atlasFace.descender) / 2.0);
    const double capitalsTop = baseline - (atlasFace.capHeight / 2.0);
    char advanceText[32];
    std::snprintf(advanceText, sizeof advanceText, "%.3f", advance);
    UiSmokeCheck(
        ui::theme::kTimelineClipBadgeGlyphAdvance >= advance &&
            ui::theme::kTimelineClipBadgeGlyphAdvance - advance <= 0.05,
        (std::string("PRD24: the advance a badge is laid out with is the "
                     "caption face's (") +
         advanceText + " points measured)")
            .c_str());

    const Ulid carrying[] = {
        gUiSmokeSignageFastClipId,       gUiSmokeSignageSoundClipId,
        gUiSmokeSignageSlowClipId,       gUiSmokeSignageFramedClipId,
        gUiSmokeSignageGradedClipId,     gUiSmokeSignageAnimatedClipId,
        gUiSmokeSignageLoadedClipId,     gUiSmokeSignageTightClipId,
        gUiSmokeSignageSliverClipId,     motionClipId,
        gUiSmokeSignagePlainClipId,      gUiSmokeSignageActiveAngleClipId,
        gUiSmokeSignageHiddenAngleClipId};
    bool inside = true;
    bool aboveName = true;
    bool apart = true;
    bool typeset = true;
    bool centred = true;
    bool clickThrough = true;
    size_t badgeCount = 0;
    for (const Ulid& clipId : carrying) {
        const Span span = spanOf(clipId);
        // The strip the clip's name is written on, as it is drawn.
        double nameTop = -1.0;
        for (const MetalDrawCommand& command : data.overlays) {
            const MetalRect* rect = std::get_if<MetalRect>(&command);
            if (rect && onTrackOf(span, *rect) &&
                std::abs(rect->x - span.left) < 0.01 &&
                std::abs(rect->height - 14.0) < 0.01 && rect->red == 0.0f &&
                rect->alpha == 0.40f)
                nameTop = rect->y;
        }
        double previousRight = span.left;
        for (const DrawnBadge& badge : badgesOf(data, span)) {
            ++badgeCount;
            const MetalRect& plate = badge.plate;
            const MetalText& label = badge.label;
            inside = inside && plate.x > span.left &&
                     plate.x + plate.width < span.right && plate.y > span.top &&
                     plate.y + plate.height < span.top + trackHeight - 3.0;
            aboveName = aboveName && nameTop >= 0.0 &&
                        plate.y + plate.height <= nameTop;
            apart = apart && plate.x > previousRight;
            previousRight = plate.x + plate.width;
            const double glyphs = ClipBadgeGlyphCount(label.text);
            // The pen starts one point inside the label's quad.
            const double penStart = label.x + 1.0;
            const double penEnd = penStart + (glyphs * advance);
            typeset = typeset && label.face == MetalFontFace::Mono &&
                      label.bold &&
                      label.point_size == ui::theme::kFontSizeCaption &&
                      sameColor(label.color, ui::theme::kClipBadgeText) &&
                      label.max_width >= glyphs * advance &&
                      penStart - plate.x >= 1.0 &&
                      plate.x + plate.width - penEnd >= 1.0 &&
                      std::abs((penStart - plate.x) -
                               (plate.x + plate.width - penEnd)) <= 1.0;
            const double over = label.y + capitalsTop - plate.y;
            const double under = plate.y + plate.height - (label.y + baseline);
            centred = centred && over >= 1.0 && under >= 1.0 &&
                      std::abs(over - under) <= 1.0;
            // A badge is not a control: the pointer on it hits the clip, as
            // it does at the same abscissa on the clip's name.
            const double x = plate.x + (plate.width / 2.0);
            const auto onBadge =
                HitTestTimeline(self.state->document, self.state->viewport, x,
                                plate.y + (plate.height / 2.0) - top, width);
            const auto onName =
                HitTestTimeline(self.state->document, self.state->viewport, x,
                                nameTop + 7.0 - top, width);
            clickThrough = clickThrough && onBadge && onName &&
                           onBadge->clip_id == clipId &&
                           onName->clip_id == clipId &&
                           onBadge->edge == onName->edge;
        }
    }
    // Twelve: one each on eight clips, and the four of the loaded shot.
    UiSmokeCheck(badgeCount == 12 && inside,
                 ("PRD24: every badge lies inside the rectangle the pointer "
                  "hits for its clip (" +
                  std::to_string(badgeCount) + " badges drawn)")
                     .c_str());
    UiSmokeCheck(aboveName,
                 "PRD24: no badge reaches the strip of its clip's name");
    UiSmokeCheck(apart, "PRD24: badges follow each other without touching");
    UiSmokeCheck(typeset, "PRD24: a label is the caption face in the badge "
                          "colour, its glyphs centred across the plate");
    UiSmokeCheck(centred, "PRD24: a label's capitals sit in the middle of "
                          "the plate's height");
    UiSmokeCheck(clickThrough,
                 "PRD24: the pointer on a badge hits the clip under it, as "
                 "it does anywhere else on the clip");

    // The sync badge of the late sound, and its speed after it.
    {
        const Span span = spanOf(gUiSmokeSignageSoundClipId);
        const MetalRect* drift = nullptr;
        for (const MetalDrawCommand& command : data.overlays) {
            const MetalRect* rect = std::get_if<MetalRect>(&command);
            if (rect && onTrackOf(span, *rect) && rect->x >= span.left &&
                rect->x < span.right && rect->red == 0.66f &&
                rect->alpha == 0.94f)
                drift = rect;
        }
        const bool driftSaid = std::any_of(
            data.overlays.begin(), data.overlays.end(),
            [](const MetalDrawCommand& command) {
                const MetalText* text = std::get_if<MetalText>(&command);
                return text && text->text == "+3f";
            });
        const std::vector<DrawnBadge> badges = badgesOf(data, span);
        UiSmokeCheck(
            drift && driftSaid && badges.size() == 1 &&
                badges.front().plate.x > drift->x + drift->width + 1.0 &&
                badges.front().plate.y == drift->y &&
                badges.front().plate.height == drift->height,
            "PRD24: on a drifted sound, the speed is written on "
            "the sync badge's row, after it, without touching it");
        // PRD25 -- its label used to be drawn four points too low, the
        // capitals running over the plate's lower edge onto the waveform:
        // it now sits in its plate as a badge's label does, both ways.
        const MetalText* driftLabel = nullptr;
        for (const MetalDrawCommand& command : data.overlays) {
            const MetalText* label = std::get_if<MetalText>(&command);
            if (label && label->text == "+3f") driftLabel = label;
        }
        char driftText[96] = "no sync badge";
        bool driftCentred = false;
        if (drift && driftLabel) {
            const double over = driftLabel->y + capitalsTop - drift->y;
            const double under =
                drift->y + drift->height - (driftLabel->y + baseline);
            const double penStart = driftLabel->x + 1.0;
            const double penEnd =
                penStart + (ClipBadgeGlyphCount(driftLabel->text) * advance);
            const double before = penStart - drift->x;
            const double after = drift->x + drift->width - penEnd;
            std::snprintf(driftText, sizeof driftText,
                          "%.2f above, %.2f under, %.2f before, %.2f after",
                          over, under, before, after);
            driftCentred = over >= 1.0 && under >= 1.0 &&
                           std::abs(over - under) <= 1.0 && before >= 1.0 &&
                           after >= 1.0 && std::abs(before - after) <= 1.0 &&
                           driftLabel->max_width >=
                               ClipBadgeGlyphCount(driftLabel->text) * advance;
        }
        UiSmokeCheck(driftCentred,
                     (std::string("PRD25: the sync badge's label sits in the "
                                  "middle of its plate, both ways (") +
                      driftText + ")")
                         .c_str());
    }

    // The hidden angle: one veil over the whole body and over the glyph
    // cells of its name, which reach under the body, drawn after the name
    // and before the clip's edges; the active angle has none.
    {
        const ui::theme::Color veil = ui::theme::kClipHiddenAngleVeil;
        // The cell the atlas draws a glyph of the name's face in, less the
        // point of padding under it: where a descender can end.
        NSFont* nameFace =
            [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeSmall * 2.0
                                        weight:NSFontWeightBold];
        const double nameInk =
            ((std::ceil(nameFace.ascender - nameFace.descender +
                        nameFace.leading) +
              4.0) /
             2.0) -
            1.0;
        const auto veilsOver = [&](const Ulid& clipId, bool& covers) {
            const Span span = spanOf(clipId);
            size_t veils = 0;
            covers = false;
            const MetalRect* body = nullptr;
            double nameBottom = -1.0;
            for (const MetalDrawCommand& command : data.overlays) {
                if (const MetalText* text = std::get_if<MetalText>(&command)) {
                    if (span.found && text->y >= span.top &&
                        text->y < span.top + trackHeight &&
                        std::abs(text->x - (span.left + 5.0)) < 0.01 &&
                        text->point_size == ui::theme::kFontSizeSmall)
                        nameBottom = text->y + nameInk;
                    continue;
                }
                const MetalRect* rect = std::get_if<MetalRect>(&command);
                if (!rect || !span.found || !onTrackOf(span, *rect) ||
                    std::abs(rect->x - span.left) > 0.01)
                    continue;
                if (!body && std::abs(rect->height - 40.0) < 0.01) body = rect;
                if (rect->red != veil.r || rect->green != veil.g ||
                    rect->blue != veil.b || rect->alpha != veil.a)
                    continue;
                ++veils;
                covers =
                    body && nameBottom >= 0.0 && rect->y == body->y &&
                    rect->width == body->width &&
                    rect->y + rect->height >= body->y + body->height &&
                    rect->y + rect->height >= nameBottom &&
                    // Short of a clip's body on the track below.
                    rect->y + rect->height < span.top + trackHeight + 2.0 &&
                    std::abs(rect->x + rect->width - span.right) < 0.01;
            }
            return veils;
        };
        bool hiddenCovered = false;
        bool activeCovered = false;
        const size_t hiddenVeils =
            veilsOver(gUiSmokeSignageHiddenAngleClipId, hiddenCovered);
        const size_t activeVeils =
            veilsOver(gUiSmokeSignageActiveAngleClipId, activeCovered);
        UiSmokeCheck(hiddenVeils == 1 && hiddenCovered && activeVeils == 0,
                     "PRD24: the angle its group keeps off screen is drawn "
                     "under a veil that covers its whole body and its name "
                     "down to the descenders, and the active angle is not");
    }

    // Nothing else moved: the list without the pass is the list with it,
    // less two commands a badge and one a veil.
    {
        size_t kept = 0;
        size_t added = 0;
        for (const MetalDrawCommand& command : data.overlays) {
            if (kept < bare.overlays.size() &&
                sameCommand(command, bare.overlays[kept]))
                ++kept;
            else
                ++added;
        }
        UiSmokeCheck(
            kept == bare.overlays.size() && added == (2 * badgeCount) + 1,
            ("PRD24: the badges and the veil are added to the "
             "display list, and nothing in it is changed (" +
             std::to_string(added) + " commands added to " +
             std::to_string(bare.overlays.size()) + ")")
                .c_str());
    }

    if (UiQaEnabled()) {
        const auto capture = [&](const std::string& name) {
            self.state->overlayDirty = true;
            [self presentNearestFrameAtDeadline:NO];
            [NSRunLoop.currentRunLoop
                   runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
            std::string captureError;
            const bool captured =
                CaptureUiQaWindow(self.window, name, captureError);
            UiSmokeCheck(
                captured,
                ("VQA captured " + name + (captured ? "" : ": " + captureError))
                    .c_str(),
                ui_smoke::Needs::kVisibleWindow);
        };
        capture("f0-prd24-signaletique");
        // The same with the hidden angle selected: its outline must read
        // over the veil.
        self.state->interaction->SelectClip(gUiSmokeSignageHiddenAngleClipId);
        capture("f1-prd24-angle-masque-selectionne");
        self.state->interaction->SelectClips({});
    }

    activate(previousTimeline);
    self.state->viewport = previousViewport;
    self.state->overlayDirty = true;
}

// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). An agent relayed to the
// window, as Claude Code is: through the window's real endpoint, on its
// socket, from another thread, one call at a time -- an edit, an
// intervention of three steps with a label, a refusal, a read. The Agent
// panel, fed while another tab of the dock is shown, must then hold one line
// for the edit, one for the intervention and one for the refusal, word for
// word, and none for the read; the clips the intervention touched carry the
// agent's mark in the display list, and a click of the person in the
// timeline takes the marks away.
//
// Played from the main run loop rather than from the run's main-queue block
// (-runUiSmokeTests): the endpoint's backend hands each call to the main
// queue, which this pump serves. Runs in both modes; with
// CUTMACHINE_UI_QA_DIR it keeps a capture of the window with the panel's
// three lines. The timeline, the view and the dock are put back, and the
// agent's edits undone.
- (void)runUiSmokeAgentJournalScenario {
    const auto pump = [](NSTimeInterval seconds) {
        NSEvent* event = [NSApp
            nextEventMatchingMask:NSEventMaskAny
                        untilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]
                           inMode:NSDefaultRunLoopMode
                          dequeue:YES];
        if (event) [NSApp sendEvent:event];
    };
    // What earlier scenarios left on the main queue, which could not run
    // while the run held it, runs now, before anything is measured.
    [self spinRunLoopFor:0.3];
    if (!self.agentEndpoint) [self startAgentEndpoint];
    const bool listening =
        self.agentEndpoint != nullptr && self.agentEndpoint->IsRunning();
    UiSmokeCheck(listening, "PRD25: the window's endpoint listens");
    if (!listening) return;

    const Ulid previousTimeline = self.state->activeTimelineId;
    const TimelineViewport previousViewport = self.state->viewport;
    const ui::PanelSlot dockBefore = [self.rightDockPanel selectedSlot];
    const auto activate = [self](const Ulid& timelineId) {
        return [self
            activateTimelineIdentifier:[NSString
                                           stringWithUTF8String:timelineId
                                                                    .c_str()]];
    };
    const BOOL opened = activate(gUiSmokeJournalTimelineId);
    UiSmokeCheck(opened, "PRD25: the agent's timeline opens");
    if (!opened) return;
    [self requestTimelinePosition:{0, 25}];
    [self fitTimelineToViewportWidth];
    self.state->viewport.pixels_per_second = 80.0;
    self.state->overlayDirty = true;
    [self.window makeFirstResponder:self.metalView];
    // The panel is not shown while the agent works: it fills all the same.
    // ON3 -- its window hidden, the toggle unmarked.
    [self showAgentWindow:NO focus:NO];
    self.agentUnread = NO;
    const std::string documentBefore = self.state->document.SaveToString();
    gUiSmokeAgentJournalTime = "10:00:00";
    const auto rows = [self]() {
        std::vector<std::string> found;
        UiSmokeAgentJournalRows(self.chatPanelView, found);
        return found;
    };
    const size_t rowsBefore = rows().size();
    const auto restore = [&]() {
        gUiSmokeAgentJournalTime.clear();
        activate(previousTimeline);
        self.state->viewport = previousViewport;
        self.state->interaction->SelectClips({});
        [self.rightDockPanel selectSlot:dockBefore];
        self.state->overlayDirty = true;
    };

    UiSmokeAgentLink agent;
    const bool connected = agent.Connect(self.agentEndpoint->SocketPath());
    UiSmokeCheck(connected, "PRD25: an agent connects to the window's socket");
    if (!connected) {
        restore();
        return;
    }
    int expectedEvents = gUiSmokeAgentEvents;
    // One line sent from another thread, its answer awaited here while the
    // main queue is served.
    const auto send = [&](const std::string& line) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto answer = std::make_shared<std::string>();
        UiSmokeAgentLink* link = &agent;
        std::thread worker([link, line, done, answer] {
            std::string received;
            if (link->Send(line) && link->Receive(received)) *answer = received;
            done->store(true);
        });
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:20.0];
        while (!done->load() && deadline.timeIntervalSinceNow > 0) pump(0.01);
        if (!done->load()) agent.Close();
        worker.join();
        return *answer;
    };
    // The window takes an event once its answer is written: wait for it.
    const auto settle = [&](int events) {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
        while (gUiSmokeAgentEvents < events &&
               deadline.timeIntervalSinceNow > 0)
            pump(0.01);
        return gUiSmokeAgentEvents >= events;
    };
    int requestId = 100;
    // A tools/call, and the tool's own answer in it.
    const auto call = [&](const std::string& tool, const std::string& arguments,
                          bool& isError) {
        const std::string line =
            send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++requestId) +
                 R"(,"method":"tools/call","params":{"name":")" + tool +
                 R"(","arguments":)" + arguments + "}}");
        settle(++expectedEvents);
        mcp_json::Value root;
        mcp_json::Value payload;
        std::string parseError;
        isError = true;
        if (mcp_json::Value::Parse(line, root, parseError) &&
            root.Find("result") != nullptr) {
            const mcp_json::Value& result = *root.Find("result");
            const mcp_json::Value* flag = result.Find("isError");
            isError = flag != nullptr && flag->AsBool();
            const mcp_json::Value* content = result.Find("content");
            if (content != nullptr && !content->AsArray().empty() &&
                content->AsArray()[0].Find("text") != nullptr)
                mcp_json::Value::Parse(
                    content->AsArray()[0].Find("text")->AsString(), payload,
                    parseError);
        }
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const char* key) {
        const mcp_json::Value* found = value.Find(key);
        return found != nullptr && found->IsString() ? found->AsString()
                                                     : std::string();
    };
    const auto joined = [](const std::vector<std::string>& lines) {
        std::string all;
        for (const std::string& line : lines) all += " | " + line;
        return all;
    };

    const std::string initialized =
        send(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
             R"({"protocolVersion":"2025-06-18","capabilities":{},)"
             R"("clientInfo":{"name":"ui-smoke","version":"1"}}})");
    settle(++expectedEvents);
    agent.Send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    UiSmokeCheck(initialized.find("\"protocolVersion\"") != std::string::npos,
                 "PRD25: the window's endpoint answers the agent's handshake");

    const std::string timeline =
        R"("timeline_id":")" + gUiSmokeJournalTimelineId + R"(",)";
    const auto opacity = [&](const Ulid& clipId, int num, int den) {
        return "{" + timeline + R"("clip_id":")" + clipId +
               R"(","opacity":{"num":)" + std::to_string(num) + R"(,"den":)" +
               std::to_string(den) + "}}";
    };
    const Ulid& first = gUiSmokeJournalClipIds[0];
    const Ulid& second = gUiSmokeJournalClipIds[1];
    const Ulid& third = gUiSmokeJournalClipIds[2];
    const std::string edited = "10:00:00 Régler l'opacité — 1 plan modifié";
    const std::string folded =
        "10:00:00 « Trois pas de l'agent » — 2 plans modifiés, 1 créé";
    bool isError = false;

    // An edit.
    call("set_clip_opacity", opacity(first, 1, 2), isError);
    UiSmokeCheck(
        !isError && rows().size() == rowsBefore + 1 && rows().back() == edited,
        ("PRD25: an edit of the agent is one line of the Agent "
         "panel, filled while another tab is shown:" +
         joined(rows()))
            .c_str());
    // An intervention of three steps, shown as they come, then folded.
    const mcp_json::Value began =
        call("begin_intervention",
             "{" + timeline.substr(0, timeline.size() - 1) + "}", isError);
    const std::string token = text(began, "intervention");
    UiSmokeCheck(!isError && !token.empty() && rows().size() == rowsBefore + 1,
                 "PRD25: beginning an intervention writes no line");
    call("set_clip_opacity", opacity(second, 1, 2), isError);
    const mcp_json::Value cut =
        call("split_clip",
             "{" + timeline + R"("clip_id":")" + third +
                 R"(","timeline_position":{"value":125,"rate":25}})",
             isError);
    Ulid half;
    if (const mcp_json::Value* delta = cut.Find("delta"))
        if (const mcp_json::Value* clips = delta->Find("clips"))
            for (const mcp_json::Value& clip : clips->AsArray())
                if (const mcp_json::Value* created = clip.Find("created");
                    created != nullptr && created->AsBool())
                    half = text(clip, "clip_id");
    call("set_clip_opacity", opacity(second, 3, 4), isError);
    const std::vector<std::string> steps = rows();
    UiSmokeCheck(!half.empty() && steps.size() == rowsBefore + 4 &&
                     steps[rowsBefore + 1] == edited &&
                     steps[rowsBefore + 2] ==
                         "10:00:00 Couper un plan — 1 plan modifié, 1 créé" &&
                     steps[rowsBefore + 3] == edited,
                 ("PRD25: each step of an intervention shows as it comes:" +
                  joined(steps))
                     .c_str());
    call(
        "end_intervention",
        R"({"intervention":")" + token + R"(","label":"Trois pas de l'agent"})",
        isError);
    UiSmokeCheck(!isError && rows().size() == rowsBefore + 2 &&
                     rows()[rowsBefore] == edited &&
                     rows()[rowsBefore + 1] == folded,
                 ("PRD25: ended, the intervention is one line with its "
                  "label and what it did as a whole:" +
                  joined(rows()))
                     .c_str());
    // A refusal.
    const mcp_json::Value refused = call(
        "remove_clip",
        "{" + timeline + R"("clip_id":"01K3ZZZZZZZZZZZZZZZZZZZZZZ"})", isError);
    const std::string refusal =
        "10:00:00 Refusé : supprimer un plan (" + text(refused, "error") + ")";
    UiSmokeCheck(isError && !text(refused, "error").empty() &&
                     rows().size() == rowsBefore + 3 &&
                     rows().back() == refusal,
                 ("PRD25: a refused edit reads as a refusal, with the "
                  "error's name:" +
                  joined(rows()))
                     .c_str());
    // A read.
    call("get_editor_view", "{}", isError);
    const std::vector<std::string> lines = rows();
    UiSmokeCheck(!isError && lines.size() == rowsBefore + 3 &&
                     lines[rowsBefore] == edited &&
                     lines[rowsBefore + 1] == folded &&
                     lines[rowsBefore + 2] == refusal,
                 ("PRD25: a read writes no line; the panel holds the edit, the "
                  "intervention and the refusal:" +
                  joined(lines))
                     .c_str());
    UiSmokeCheck(
        [self.infoLabel.stringValue isEqualToString:@"Agent : get_editor_view"],
        ("PRD25: the status line still says the last call: " +
         std::string(self.infoLabel.stringValue.UTF8String ?: ""))
            .c_str());

    // VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The window's picture,
    // as an agent asks it: an MCP image the size the text says, the window
    // at a pixel per point, Record cut out at two.
    const auto picture = [&](const std::string& arguments, int64_t& width,
                             int64_t& height, mcp_json::Value& described) {
        const std::string line =
            send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++requestId) +
                 R"(,"method":"tools/call","params":{"name":)"
                 R"("capture_editor_window","arguments":)" +
                 arguments + "}}");
        settle(++expectedEvents);
        width = height = 0;
        mcp_json::Value root;
        std::string parseError;
        if (!mcp_json::Value::Parse(line, root, parseError) ||
            root.Find("result") == nullptr)
            return;
        const mcp_json::Value* content = root.Find("result")->Find("content");
        if (content == nullptr || content->AsArray().size() != 2) return;
        mcp_json::Value::Parse(content->AsArray()[0].Find("text")->AsString(),
                               described, parseError);
        const mcp_json::Value* data = content->AsArray()[1].Find("data");
        NSData* jpeg =
            data != nullptr
                ? [[NSData alloc]
                      initWithBase64EncodedString:@(data->AsString().c_str())
                                          options:0]
                : nil;
        NSBitmapImageRep* image =
            jpeg != nil ? [NSBitmapImageRep imageRepWithData:jpeg] : nil;
        width = image.pixelsWide;
        height = image.pixelsHigh;
    };
    const auto number = [](const mcp_json::Value& object, const char* key) {
        const mcp_json::Value* found = object.Find(key);
        int64_t value = -1;
        if (found != nullptr) found->AsInt64(value);
        return value;
    };
    int64_t pictureWidth = 0;
    int64_t pictureHeight = 0;
    mcp_json::Value windowPicture;
    picture("{}", pictureWidth, pictureHeight, windowPicture);
    bool surfacesNamed = false;
    if (const mcp_json::Value* regions = windowPicture.Find("regions")) {
        bool timelineNamed = false;
        bool recordNamed = false;
        for (const mcp_json::Value& region : regions->AsArray()) {
            timelineNamed |= text(region, "name") == "timeline";
            recordNamed |= text(region, "name") == "record";
        }
        surfacesNamed = timelineNamed && recordNamed;
    }
    UiSmokeCheck(
        pictureWidth == std::lround(self.window.frame.size.width) &&
            pictureHeight == std::lround(self.window.frame.size.height) &&
            number(windowPicture, "width") == pictureWidth &&
            number(windowPicture, "height") == pictureHeight && surfacesNamed,
        ("VQA5: an agent gets the window's picture through its "
         "endpoint, a pixel per point, the surfaces placed: " +
         windowPicture.Dump())
            .c_str());
    mcp_json::Value record;
    picture(R"({"region":"record","scale":2})", pictureWidth, pictureHeight,
            record);
    UiSmokeCheck(
        pictureWidth ==
                std::lround(self.programMonitorView.bounds.size.width * 2) &&
            pictureHeight ==
                std::lround(self.programMonitorView.bounds.size.height * 2) &&
            number(record, "width") == pictureWidth,
        ("VQA5: and Record alone at two pixels per point: " + record.Dump())
            .c_str());

    // The marks: on what the intervention touched, and only there.
    self.state->overlayDirty = true;
    const double width = self.metalView.bounds.size.width;
    const double top = [self videoHeight];
    const auto marksOn = [&](const Ulid& clipId) {
        const TimelineRenderData data = [self timelineRenderData];
        const std::vector<TimelineClipRect> rects =
            VisibleTimelineClips(self.state->document, self.state->viewport,
                                 width, {}, std::nullopt);
        int marks = 0;
        for (const TimelineClipRect& rect : rects) {
            if (rect.clip_id != clipId) continue;
            const double left = std::max(std::min(rect.x, rect.x + rect.width),
                                         self.state->viewport.header_width);
            const double right =
                std::min(std::max(rect.x, rect.x + rect.width), width);
            const double rowTop = top + rect.y;
            for (size_t index = 0; index + 1 < data.overlays.size(); ++index) {
                const MetalRect* plate =
                    std::get_if<MetalRect>(&data.overlays[index]);
                const MetalText* label =
                    std::get_if<MetalText>(&data.overlays[index + 1]);
                const ui::theme::Color mark = ui::theme::kAgentMark;
                if (plate && label && plate->red == mark.r &&
                    plate->green == mark.g && plate->blue == mark.b &&
                    plate->alpha == mark.a && plate->x > left &&
                    plate->x + plate->width < right && plate->y > rowTop &&
                    plate->y < rowTop + self.state->viewport.track_height &&
                    label->text == ClipBadgeLabel(ClipBadgeKind::Agent))
                    ++marks;
            }
        }
        return marks;
    };
    const int onFirst = marksOn(first);
    const int onSecond = marksOn(second);
    const int onThird = marksOn(third);
    const int onHalf = half.empty() ? -1 : marksOn(half);
    char counts[96];
    std::snprintf(counts, sizeof counts, "%d, %d, %d, %d", onFirst, onSecond,
                  onThird, onHalf);
    UiSmokeCheck(onFirst == 0 && onSecond == 1 && onThird == 1 && onHalf == 1,
                 (std::string("PRD25: the clips the agent's last entry touched "
                              "carry its mark in the display list, the "
                              "others do not (") +
                  counts + ")")
                     .c_str());

    // ON3 -- docs/roadmap/ONGLETS-2026-10.md. Written while the Agent's
    // window was hidden, the entries put a dot on its toggle; shown, the
    // window takes it off.
    NSButton* agentToggle = self.workspaceBar.agentToggle;
    UiSmokeCheck(
        self.agentUnread && [agentToggle.title isEqualToString:@"Agent •"],
        "ON3: the relayed agent's entries, written while the Agent's "
        "window was hidden, put a dot on its toggle");
    [self showAgentWindow:YES focus:NO];
    UiSmokeCheck(!self.agentUnread &&
                     [agentToggle.title isEqualToString:@"Agent"] &&
                     agentToggle.state == NSControlStateValueOn,
                 "ON3: shown, the window takes the dot off its toggle");
    if (UiQaEnabled()) {
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
        // The window, with the marks on the timeline: what WindowServer
        // composites.
        std::string captureError;
        const bool captured = CaptureUiQaWindow(
            self.window, "g0-prd25-journal-agent", captureError);
        UiSmokeCheck(captured,
                     ("VQA captured g0-prd25-journal-agent" +
                      (captured ? "" : ": " + captureError))
                         .c_str(),
                     ui_smoke::Needs::kVisibleWindow);
        // The panel alone, drawn off screen: AppKit's views only, so it
        // needs no screen, and it is what the person reads.
        std::string panelError;
        const bool drawn = WriteUiQaViewCapture(
            self.chatPanelView, CMThemeColor(ui::theme::kSurfacePanel),
            "g1-prd25-panneau-agent", panelError);
        UiSmokeCheck(drawn, ("PRD25: g1-prd25-panneau-agent is kept for QA" +
                             (drawn ? "" : ": " + panelError))
                                .c_str());
    }
    [self showAgentWindow:NO focus:NO];

    // The person clicks a shot the agent left alone: the marks go.
    Ulid aimed;
    for (const TimelineClipRect& rect :
         VisibleTimelineClips(self.state->document, self.state->viewport, width,
                              {}, std::nullopt)) {
        if (rect.clip_id != first) continue;
        const double x = rect.x + (rect.width / 2.0);
        const double y = rect.y + 22.0;
        if (const auto hit = HitTestTimeline(self.state->document,
                                             self.state->viewport, x, y, width))
            aimed = hit->clip_id;
        SendTimelineMouseGesture(
            self.metalView,
            NSMakePoint(x, self.metalView.bounds.size.height - top - y),
            NSMakePoint(x, self.metalView.bounds.size.height - top - y));
        break;
    }
    UiSmokeCheck(aimed == first &&
                     self.state->agentJournal.MarkedClips().empty() &&
                     marksOn(second) == 0 && marksOn(third) == 0 &&
                     (half.empty() || marksOn(half) == 0),
                 "PRD25: a click of the person in the timeline takes the "
                 "agent's marks away, and leaves the journal");
    UiSmokeCheck(rows().size() == rowsBefore + 3,
                 "PRD25: the journal stays after the click");

    agent.Close();
    settle(++expectedEvents);
    [self menuUndo:nil];
    [self menuUndo:nil];
    UiSmokeCheck(self.state->document.SaveToString() == documentBefore,
                 "PRD25: two undos take the agent's edit and its "
                 "intervention back");
    restore();
}

// PRD22 -- ROADMAP.md. The "Moteur IA" dialog's form, built the way
// -showConfigurationWindow builds it (ai_engine::DraftsFromStored and a
// FormContext), its protocol menu moved through every protocol the way a
// person moves it: the item selected, then the menu's action. After each
// move every field is active or greyed and suggests what AiEngineForm.h
// gives for that protocol, a field the protocol does not use is empty, and
// what a field held comes back with its protocol. No check opened this
// dialog before, and its modal loop is not run here: it would hold the smoke
// run until someone answered it.
//
// Runs in both modes. With CUTMACHINE_UI_QA_DIR the alert is also drawn off
// screen and kept as a PNG, once per protocol.
- (void)runUiSmokeAiEngineFormScenario {
    using ai_engine::EngineProtocol;
    struct Controls {
        NSPopUpButton* menu = nil;
        NSTextField* command = nil;
        NSTextField* model = nil;
        NSTextField* baseUrl = nil;
        NSTextField* apiKey = nil;
        NSButton* removeKey = nil;
    };
    const auto findIn = [](NSView* root, NSString* identifier) -> NSView* {
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:root];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view.identifier isEqualToString:identifier]) return view;
            [pending addObjectsFromArray:view.subviews];
        }
        return nil;
    };
    const auto controlsOf = [&findIn](CMAiEngineFormView* form) {
        const auto find = [&findIn, form](NSString* identifier) {
            return findIn(form, identifier);
        };
        Controls controls;
        NSView* menu = find(@"ai_engine.provider");
        NSView* command = find(@"ai_engine.acp_command");
        NSView* model = find(@"ai_engine.model");
        NSView* baseUrl = find(@"ai_engine.base_url");
        NSView* apiKey = find(@"ai_engine.api_key");
        NSView* removeKey = find(@"ai_engine.remove_key");
        if ([menu isKindOfClass:NSPopUpButton.class])
            controls.menu = (NSPopUpButton*)menu;
        if ([command isKindOfClass:NSTextField.class])
            controls.command = (NSTextField*)command;
        if ([model isKindOfClass:NSTextField.class])
            controls.model = (NSTextField*)model;
        if ([baseUrl isKindOfClass:NSTextField.class])
            controls.baseUrl = (NSTextField*)baseUrl;
        if ([apiKey isKindOfClass:NSSecureTextField.class])
            controls.apiKey = (NSTextField*)apiKey;
        if ([removeKey isKindOfClass:NSButton.class])
            controls.removeKey = (NSButton*)removeKey;
        return controls;
    };
    const auto complete = [](const Controls& controls) {
        return controls.menu && controls.command && controls.model &&
               controls.baseUrl && controls.apiKey && controls.removeKey;
    };
    const auto nameOf = [](EngineProtocol protocol) -> std::string {
        switch (protocol) {
            case EngineProtocol::AnthropicMessages:
                return "Anthropic";
            case EngineProtocol::Ollama:
                return "Ollama";
            case EngineProtocol::Acp:
                return "the coding agent";
        }
        return "?";
    };
    const auto choose = [](const Controls& controls, EngineProtocol protocol) {
        [controls.menu selectItemAtIndex:ai_engine::MenuIndexOf(protocol)];
        [controls.menu sendAction:controls.menu.action to:controls.menu.target];
    };
    // What the form must show for `protocol`: the policy's rule for every
    // field, and `texts` in the fields (empty where the protocol does not
    // use them).
    const auto expect = [&](CMAiEngineFormView* form, const Controls& controls,
                            EngineProtocol protocol,
                            const ai_engine::FormContext& context,
                            const ai_engine::FormTexts& texts,
                            const std::string& when) {
        const ai_engine::FormPolicy policy =
            ai_engine::PolicyFor(protocol, context);
        UiSmokeCheck(controls.menu.indexOfSelectedItem ==
                             ai_engine::MenuIndexOf(protocol) &&
                         form.selectedProtocol == protocol,
                     ("PRD22: " + when + ", the menu and the form are on " +
                      nameOf(protocol))
                         .c_str());
        const auto field = [&](NSTextField* control,
                               const ai_engine::FieldPolicy& rule,
                               const std::string& text, const char* label) {
            const bool suggests = [control.placeholderString
                isEqualToString:@(rule.placeholder.c_str())];
            const bool holds =
                [control.stringValue isEqualToString:@(text.c_str())];
            // Its label dims with it: a disabled field alone looks empty.
            NSView* title = findIn(
                form, [control.identifier stringByAppendingString:@".label"]);
            const bool titled =
                [title isKindOfClass:NSTextField.class] &&
                [((NSTextField*)title).textColor
                    isEqual:rule.enabled ? NSColor.labelColor
                                         : NSColor.disabledControlTextColor];
            UiSmokeCheck(
                control.enabled == rule.enabled && suggests && holds && titled,
                ("PRD22: " + when + ", " + label + " is " +
                 (rule.enabled ? "active," : "greyed, its label dimmed,") +
                 " suggests \"" + rule.placeholder + "\" and holds \"" + text +
                 "\"")
                    .c_str());
        };
        field(controls.command, policy.acp_command, texts.acp_command,
              "Commande ACP");
        field(controls.model, policy.model, texts.model, "Modèle");
        field(controls.baseUrl, policy.base_url, texts.base_url, "URL de base");
        field(controls.apiKey, policy.api_key, texts.api_key, "Clé API");
        UiSmokeCheck(controls.removeKey.enabled == policy.remove_key_enabled &&
                         controls.removeKey.state == NSControlStateValueOff,
                     ("PRD22: " + when + ", the key deletion box is " +
                      (policy.remove_key_enabled ? "active" : "greyed") +
                      " and unticked")
                         .c_str());
    };

    // As the panel opens it with Anthropic chosen, a model stored but no
    // URL, a key in effect and a command stored for the agent: the stored
    // model in its field, the default URL in grey, nothing for Ollama.
    ai_engine::FormContext context;
    context.anthropic_key_stored = true;
    CMAiEngineFormView* form = [[CMAiEngineFormView alloc]
        initWithProtocol:EngineProtocol::AnthropicMessages
                  drafts:ai_engine::DraftsFromStored(
                             EngineProtocol::AnthropicMessages,
                             "claude-sonnet-4-5", "",
                             ai_engine::kCodexAcpCommandExample)
                 context:context];
    const Controls controls = controlsOf(form);
    UiSmokeCheck(complete(controls),
                 "PRD22: the engine form has its six controls under their "
                 "identifiers");
    if (!complete(controls)) return;
    bool listed = controls.menu.numberOfItems ==
                  static_cast<NSInteger>(ai_engine::kEngineProtocols.size());
    for (const EngineProtocol protocol : ai_engine::kEngineProtocols)
        listed =
            listed &&
            [[controls.menu itemTitleAtIndex:ai_engine::MenuIndexOf(protocol)]
                isEqualToString:@(ai_engine::MenuTitle(protocol))];
    UiSmokeCheck(listed, "PRD22: the protocol menu lists Anthropic, Ollama "
                         "and the coding agent, in that order");

    ai_engine::FormTexts stored;
    stored.model = "claude-sonnet-4-5";
    ai_engine::FormTexts agent;
    agent.acp_command = ai_engine::kCodexAcpCommandExample;
    const auto step = [&](EngineProtocol protocol,
                          const ai_engine::FormTexts& texts,
                          const std::string& when) {
        expect(form, controls, protocol, context, texts, when);
    };
    step(EngineProtocol::AnthropicMessages, stored, "at opening");
    UiSmokeCheck(
        [controls.model.placeholderString
            isEqualToString:@(chat::kDefaultAnthropicModel)] &&
            [controls.baseUrl.placeholderString
                isEqualToString:@(chat::kDefaultAnthropicBaseUrl)],
        "PRD22: on Anthropic, Modèle suggests the default model and URL de "
        "base api.anthropic.com");
    choose(controls, EngineProtocol::Ollama);
    step(EngineProtocol::Ollama, {}, "on Ollama");
    // Typed for Ollama, then the menu moves on and back.
    controls.model.stringValue = @"llama3.2";
    controls.apiKey.stringValue = @"cle-ollama";
    controls.removeKey.state = NSControlStateValueOn;
    choose(controls, EngineProtocol::Acp);
    step(EngineProtocol::Acp, agent, "on the coding agent");
    choose(controls, EngineProtocol::AnthropicMessages);
    step(EngineProtocol::AnthropicMessages, stored, "back on Anthropic");
    ai_engine::FormTexts typed;
    typed.model = "llama3.2";
    typed.api_key = "cle-ollama";
    choose(controls, EngineProtocol::Ollama);
    step(EngineProtocol::Ollama, typed, "back on Ollama");
    const ai_engine::FormTexts read = form.texts;
    UiSmokeCheck(read.model == "llama3.2" && read.base_url.empty() &&
                     read.api_key == "cle-ollama" && read.acp_command.empty() &&
                     !form.removesStoredKey,
                 "PRD22: what the dialog saves is what Ollama's fields hold, "
                 "the box ticked before the moves left unticked");

    // As the panel opens it with the coding agent chosen and no command
    // stored -- a model and a URL an earlier engine left behind -- on a Mac
    // whose environment names another Anthropic model and endpoint
    // (CUTMACHINE_ANTHROPIC_MODEL, CUTMACHINE_ANTHROPIC_URL), no key.
    ai_engine::FormContext environment;
    environment.anthropic_model = "claude-sonnet-4-6";
    environment.anthropic_base_url = "https://proxy.example";
    CMAiEngineFormView* agentForm = [[CMAiEngineFormView alloc]
        initWithProtocol:EngineProtocol::Acp
                  drafts:ai_engine::DraftsFromStored(
                             EngineProtocol::Acp, "claude-opus-5",
                             "https://leftover.example", "")
                 context:environment];
    const Controls agentControls = controlsOf(agentForm);
    if (complete(agentControls)) {
        expect(agentForm, agentControls, EngineProtocol::Acp, environment, {},
               "opened on the coding agent");
        UiSmokeCheck([agentControls.command.placeholderString
                         isEqualToString:@(ai_engine::kDefaultAcpCommand)],
                     "PRD22: an empty command suggests the pinned Claude "
                     "adapter");
        choose(agentControls, EngineProtocol::AnthropicMessages);
        expect(agentForm, agentControls, EngineProtocol::AnthropicMessages,
               environment, {}, "then on Anthropic, nothing stored");
        UiSmokeCheck([agentControls.model.placeholderString
                         isEqualToString:@"claude-sonnet-4-6"] &&
                         [agentControls.baseUrl.placeholderString
                             isEqualToString:@"https://proxy.example"],
                     "PRD22: an empty Modèle or URL de base suggests what the "
                     "environment names, the leftovers shown nowhere");
    } else {
        UiSmokeCheck(false, "PRD22: the form opened on the coding agent has "
                            "its six controls");
    }

    // The panel the dialog shows holds the form, its help text naming the
    // pinned Codex adapter. Built, never shown. PRD26 -- the window's form
    // panel, which was an alert.
    CMFormPanel* panel = [form makePanel];
    [panel layoutForm];
    UiSmokeCheck([panel.messageLabel.stringValue
                     containsString:@(ai_engine::kCodexAcpCommandExample)],
                 "PRD22: the dialog's help text cites the pinned Codex "
                 "adapter");
    const std::string problems = UiSmokeFormPanelProblems(panel);
    UiSmokeCheck(
        panel.form == form && problems.empty() &&
            [panel.titleLabel.stringValue isEqualToString:@"Moteur IA"],
        ("PRD26: the engine dialog is a form panel holding the form, "
         "laid out whole" +
         (problems.empty() ? std::string() : " --" + problems))
            .c_str());

    if (!UiQaEnabled()) return;
    // The panel as the dialog shows it, drawn off screen. Its window's
    // background is not drawn there, and in the dark appearance its labels
    // are white and its control bezels translucent: on a transparent
    // picture they vanished. Laid over the panel's background, they read.
    NSView* content = panel.contentView;
    for (const EngineProtocol protocol : ai_engine::kEngineProtocols) {
        choose(controls, protocol);
        NSBitmapImageRep* drawn =
            content
                ? [content bitmapImageRepForCachingDisplayInRect:content.bounds]
                : nil;
        if (drawn)
            [content cacheDisplayInRect:content.bounds toBitmapImageRep:drawn];
        NSBitmapImageRep* bitmap = nil;
        if (drawn) {
            bitmap = [[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:nullptr
                              pixelsWide:drawn.pixelsWide
                              pixelsHigh:drawn.pixelsHigh
                           bitsPerSample:8
                         samplesPerPixel:4
                                hasAlpha:YES
                                isPlanar:NO
                          colorSpaceName:NSDeviceRGBColorSpace
                             bytesPerRow:0
                            bitsPerPixel:0];
            bitmap.size = drawn.size;
            const NSRect bounds =
                NSMakeRect(0, 0, drawn.size.width, drawn.size.height);
            [NSGraphicsContext saveGraphicsState];
            NSGraphicsContext.currentContext =
                [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
            [content.effectiveAppearance performAsCurrentDrawingAppearance:^{
              [panel.backgroundColor setFill];
              NSRectFill(bounds);
            }];
            // Over the background: a plain -drawInRect: copies the picture,
            // its transparency included.
            [drawn drawInRect:bounds
                      fromRect:NSZeroRect
                     operation:NSCompositingOperationSourceOver
                      fraction:1.0
                respectFlipped:YES
                         hints:nil];
            [NSGraphicsContext restoreGraphicsState];
        }
        NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                           properties:@{}];
        const std::string name =
            "prd22-moteur-ia-" +
            std::to_string(ai_engine::MenuIndexOf(protocol)) + "-" +
            (protocol == EngineProtocol::AnthropicMessages ? "anthropic"
             : protocol == EngineProtocol::Ollama          ? "ollama"
                                                           : "acp");
        const std::filesystem::path path =
            gUiQaOutputDirectory / (name + ".png");
        const bool wrote = png != nil && [png writeToFile:@(path.c_str())
                                               atomically:YES];
        if (wrote)
            gUiQaCaptures.push_back({name, path.filename().string(),
                                     static_cast<int64_t>(bitmap.pixelsWide),
                                     static_cast<int64_t>(bitmap.pixelsHigh)});
        UiSmokeCheck(wrote, ("PRD22: the dialog on " + nameOf(protocol) +
                             " is kept for QA as " + name + ".png")
                                .c_str());
    }
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. What is wrong with a form
// panel as laid out, or nothing: every part inside it, no two overlapping,
// every text and every button title shown whole.
static std::string UiSmokeFormPanelProblems(CMFormPanel* panel) {
    std::string problems;
    NSView* content = panel.contentView;
    NSMutableArray<NSView*>* parts = [NSMutableArray
        arrayWithObjects:panel.titleLabel, panel.form, panel.errorLabel, nil];
    if (!panel.messageLabel.hidden) [parts addObject:panel.messageLabel];
    if (!panel.errorIcon.hidden) [parts addObject:panel.errorIcon];
    [parts addObjectsFromArray:panel.buttons];
    const auto nameOf = [panel](NSView* view) -> std::string {
        if ([view isKindOfClass:NSButton.class])
            return "« " +
                   std::string(((NSButton*)view).title.UTF8String ?: "") + " »";
        if (view == panel.form) return "the form";
        return view.identifier.UTF8String ?: "a view";
    };
    for (NSView* part in parts)
        if (!NSContainsRect(content.bounds, part.frame))
            problems += " " + nameOf(part) + " is outside;";
    for (NSUInteger left = 0; left < parts.count; ++left)
        for (NSUInteger right = left + 1; right < parts.count; ++right)
            if (NSIntersectsRect(parts[left].frame, parts[right].frame))
                problems += " " + nameOf(parts[left]) + " overlaps " +
                            nameOf(parts[right]) + ";";
    const auto whole = [&](NSTextField* field) {
        if (field.hidden || field.stringValue.length == 0) return;
        const NSSize needed =
            [field.cell cellSizeForBounds:NSMakeRect(0, 0, NSWidth(field.frame),
                                                     CGFLOAT_MAX)];
        if (needed.height > NSHeight(field.frame) + 0.5 ||
            (field == panel.titleLabel &&
             field.cell.cellSize.width > NSWidth(field.frame) + 0.5))
            problems += " " + nameOf(field) + " is cut;";
    };
    whole(panel.titleLabel);
    whole(panel.messageLabel);
    whole(panel.errorLabel);
    for (NSButton* button in panel.buttons)
        if (button.cell.cellSize.width > NSWidth(button.frame) + 0.5)
            problems += " " + nameOf(button) + " is cut;";
    return problems;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The window's forms on their
// panel (FormPanel.h), opened by the actions the menus send: each comes as a
// sheet on the editor window -- which needs neither the focus nor the
// screen, and runs no modal loop that would hold the run -- with its fields
// under their identifiers, its texts whole, its buttons where they go,
// Return on the accepting one and Escape on the cancelling one. A refused
// form stays open and says why on its error line. Every panel is closed
// before the next opens: a sheet left on the window would make the next
// form modal, and hold the run.
//
// Runs in both modes. With CUTMACHINE_UI_QA_DIR, each panel is also drawn
// off screen and kept as a PNG.
- (void)runUiSmokeFormPanelScenario {
    const auto waitFor = [](const std::function<bool()>& done, double seconds) {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
        while (!done() && deadline.timeIntervalSinceNow > 0) {
            NSEvent* event = [NSApp
                nextEventMatchingMask:NSEventMaskAny
                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                               inMode:NSDefaultRunLoopMode
                              dequeue:YES];
            if (event) [NSApp sendEvent:event];
        }
        return done();
    };
    const auto findIn = [](NSView* root, NSString* identifier) -> NSView* {
        if (root == nil) return nil;
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:root];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view.identifier isEqualToString:identifier]) return view;
            [pending addObjectsFromArray:view.subviews];
        }
        return nil;
    };
    const auto buttonOf = [](CMFormPanel* panel, NSString* title) -> NSButton* {
        for (NSButton* button in panel.buttons)
            if ([button.title isEqualToString:title]) return button;
        return nil;
    };
    // The form `open` shows, as a sheet on the editor window.
    const auto openPanel = [&](const std::string& what,
                               void (^open)(void)) -> CMFormPanel* {
        if (self.window.attachedSheet != nil) {
            UiSmokeCheck(false, ("PRD26: no sheet is left on the window "
                                 "before " +
                                 what + " opens")
                                    .c_str());
            [self.window endSheet:self.window.attachedSheet];
            waitFor([&] { return self.window.attachedSheet == nil; }, 3.0);
        }
        open();
        waitFor(
            [&] {
                return
                    [self.window.attachedSheet isKindOfClass:CMFormPanel.class];
            },
            3.0);
        NSWindow* sheet = self.window.attachedSheet;
        CMFormPanel* panel =
            [sheet isKindOfClass:CMFormPanel.class] ? (CMFormPanel*)sheet : nil;
        UiSmokeCheck(panel != nil && panel.presented,
                     ("PRD26: " + what +
                      " opens as a sheet on the editor window, not an alert")
                         .c_str());
        return panel;
    };
    // Its title, its parts laid out, Return on `accept`, rightmost, and
    // Escape on `cancel` beside it.
    const auto expectPanel = [&](CMFormPanel* panel, const std::string& what,
                                 NSString* title, NSString* accept,
                                 NSString* cancel) {
        if (panel == nil) return;
        [panel layoutForm];
        NSButton* accepting = buttonOf(panel, accept);
        NSButton* cancelling = cancel ? buttonOf(panel, cancel) : nil;
        double rightmost = 0.0;
        for (NSButton* button in panel.buttons)
            rightmost = std::max(rightmost, (double)NSMaxX(button.frame));
        std::string problems = UiSmokeFormPanelProblems(panel);
        if (![panel.titleLabel.stringValue isEqualToString:title])
            problems +=
                " titled « " +
                std::string(panel.titleLabel.stringValue.UTF8String ?: "") +
                " »;";
        // The default button cell: AppKit holds its key back while the panel
        // is not key, which it never is on a Mac in use.
        if (!accepting || panel.defaultButtonCell != accepting.cell)
            problems += " Return is not on the accepting button;";
        else if (NSMaxX(accepting.frame) != rightmost)
            problems += " the accepting button is not rightmost;";
        if (cancel != nil && (!cancelling || ![cancelling.keyEquivalent
                                                 isEqualToString:@"\x1b"]))
            problems += " Escape is not on the cancelling button;";
        else if (cancel != nil && accepting &&
                 NSMaxX(cancelling.frame) >= NSMinX(accepting.frame))
            problems += " the cancelling button is not left of it;";
        UiSmokeCheck(
            problems.empty(),
            ("PRD26: " + what + " is titled, laid out whole, Return on « " +
             std::string(accept.UTF8String) + " »" +
             (cancel ? ", Escape on « " + std::string(cancel.UTF8String) + " »"
                     : std::string()) +
             (problems.empty() ? "" : " --" + problems))
                .c_str());
    };
    const auto keep = [&](CMFormPanel* panel, const std::string& name) {
        if (!UiQaEnabled() || panel == nil) return;
        [panel layoutForm];
        std::string error;
        const bool kept = WriteUiQaViewCapture(
            panel.contentView, panel.backgroundColor, name, error);
        UiSmokeCheck(kept, ("PRD26: " + name + " is kept for QA" +
                            (kept ? "" : ": " + error))
                               .c_str());
    };
    // Closed by its cancelling button, as Escape closes it.
    const auto dismiss = [&](CMFormPanel* panel, NSString* cancel,
                             const std::string& what) {
        if (panel == nil) return;
        NSButton* cancelling = buttonOf(panel, cancel);
        if (cancelling)
            [cancelling performClick:nil];
        else
            [panel endWithResponse:NSModalResponseCancel];
        const bool gone =
            waitFor([&] { return self.window.attachedSheet == nil; }, 3.0);
        UiSmokeCheck(
            gone && !panel.presented,
            ("PRD26: « " + std::string(cancel.UTF8String) + " » closes " + what)
                .c_str());
    };

    // « Nouvelle timeline »: refused empty, then created, then undone.
    {
        const size_t timelines = self.state->project.timelines.size();
        const Ulid previous = self.state->activeTimelineId;
        CMFormPanel* panel = openPanel("« Nouvelle timeline »", ^{
          [self createTimelinePressed:nil];
        });
        NSView* found = findIn(panel.form, @"timeline.new.name");
        NSTextField* name =
            [found isKindOfClass:NSTextField.class] ? (NSTextField*)found : nil;
        NSString* proposed = [NSString
            stringWithFormat:@"Timeline %lu", (unsigned long)timelines + 1];
        UiSmokeCheck(name && [name.stringValue isEqualToString:proposed] &&
                         panel.initialFirstResponder == name,
                     "PRD26: its name field proposes the next timeline's "
                     "name and takes the keyboard");
        expectPanel(panel, "« Nouvelle timeline »", @"Nouvelle timeline",
                    @"Créer", @"Annuler");
        keep(panel, "prd26-nouvelle-timeline");
        NSButton* create = buttonOf(panel, @"Créer");
        // Return itself, typed, once the sheet has the keys: only when the
        // editor window became key. A blank name, so that Return shows the
        // refusal and the panel stays for what follows.
        if (panel && name && gUiSmokeScreen.key) {
            waitFor([&] { return panel.isKeyWindow; }, 2.0);
            name.stringValue = @" ";
            // Posted, so that AppKit dequeues it as the current event
            // (PostApplicationKey): sent straight to -sendEvent:, it never
            // reached « Créer ».
            if (panel.isKeyWindow) {
                [panel makeFirstResponder:name];
                PostApplicationKey(panel, @"\r", 36);
                waitFor([&] { return panel.errorLabel.stringValue.length > 0; },
                        2.0);
            }
        }
        UiSmokeCheck(panel.isKeyWindow && panel.presented &&
                         [panel.errorLabel.stringValue
                             isEqualToString:@"Donnez un nom à la timeline."],
                     "PRD26: the sheet takes the keys, and Return presses "
                     "« Créer »",
                     ui_smoke::Needs::kKeyWindow);
        if (panel) [panel showError:nil];
        if (panel && name && create) {
            name.stringValue = @"   ";
            [create performClick:nil];
            const bool kept =
                self.window.attachedSheet == panel && panel.presented &&
                [panel.errorLabel.stringValue
                    isEqualToString:@"Donnez un nom à la timeline."] &&
                !panel.errorIcon.hidden &&
                self.state->project.timelines.size() == timelines;
            UiSmokeCheck(kept, "PRD26: an empty name keeps the panel open "
                               "and says why on its error line, nothing "
                               "created");
            expectPanel(panel, "« Nouvelle timeline » refused",
                        @"Nouvelle timeline", @"Créer", @"Annuler");
            keep(panel, "prd26-nouvelle-timeline-refus");

            name.stringValue = @"  Timeline PRD26 ";
            self.infoLabel.stringValue = @"";
            [create performClick:nil];
            waitFor([&] { return self.window.attachedSheet == nil; }, 3.0);
            const bool closed =
                self.window.attachedSheet == nil && !panel.presented;
            const bool created =
                closed &&
                self.state->project.timelines.size() == timelines + 1 &&
                self.state->document.sequence.name == "Timeline PRD26";
            // The window's commit of a project with one timeline more than
            // it held edit logs for was refused from 20 August to PRD26
            // ("recovery envelope must contain exactly one edit log per
            // timeline"): this check is the creation's regression test.
            NSString* status = self.infoLabel.stringValue;
            UiSmokeCheck(created,
                         ("PRD26: « Créer » closes the panel and the window "
                          "opens the new « Timeline PRD26 »" +
                          std::string(created ? "" : " -- status: ") +
                          (created ? "" : status.UTF8String ?: ""))
                             .c_str());
            if (created) [self menuUndo:nil];
        } else if (panel) {
            dismiss(panel, @"Annuler", "« Nouvelle timeline »");
        }
        [self activateTimelineIdentifier:CMStringFromUtf8(previous)];
        UiSmokeCheck(self.state->project.timelines.size() == timelines &&
                         self.state->activeTimelineId == previous &&
                         self.window.attachedSheet == nil,
                     "PRD26: the project ends with the timelines it had (a "
                     "created one undone), the window on the timeline it "
                     "showed");
    }

    // A second form asked while one is open is refused: the menus stay
    // live under a sheet.
    {
        CMFormPanel* panel = openPanel("« Réglages de séquence »", ^{
          [self configureSequence:nil];
        });
        NSView* found = findIn(panel.form, @"sequence.name");
        NSTextField* name =
            [found isKindOfClass:NSTextField.class] ? (NSTextField*)found : nil;
        UiSmokeCheck(
            name &&
                [name.stringValue
                    isEqualToString:CMStringFromUtf8(
                                        self.state->document.sequence.name)] &&
                [findIn(panel.form, @"sequence.format")
                    isKindOfClass:NSPopUpButton.class] &&
                [findIn(panel.form, @"sequence.rate")
                    isKindOfClass:NSPopUpButton.class],
            "PRD26: the sequence form holds the timeline's name, its "
            "dimensions and its rate");
        expectPanel(panel, "« Réglages de séquence »", @"Réglages de séquence",
                    @"Appliquer", @"Annuler");
        keep(panel, "prd26-reglages-sequence");
        if (panel) {
            const std::string before = self.state->document.SaveToString();
            [self configureColorManagement:nil];
            const bool alone = self.window.sheets.count == 1;
            // The shortcuts' form is refused before it is built: building it
            // would take the keyboard's fields over.
            NSDictionary* fields = self.keyboardEditingFields;
            [self editKeyboardShortcuts:nil];
            UiSmokeCheck(alone && self.window.sheets.count == 1 &&
                             self.window.attachedSheet == panel &&
                             panel.presented &&
                             self.keyboardEditingFields == fields &&
                             self.state->document.SaveToString() == before,
                         "PRD26: a second form asked meanwhile is refused, "
                         "the first left as it was, the keyboard's fields "
                         "untouched");
        }
        dismiss(panel, @"Annuler", "« Réglages de séquence »");
    }

    // « Gestion colorimétrique du projet »: HLG out of Rec.2020 refused on
    // the panel, nothing applied.
    {
        const std::string before = self.state->document.SaveToString();
        CMFormPanel* panel = openPanel("« Gestion colorimétrique »", ^{
          [self configureColorManagement:nil];
        });
        expectPanel(panel, "« Gestion colorimétrique »",
                    @"Gestion colorimétrique du projet", @"Appliquer",
                    @"Annuler");
        keep(panel, "prd26-gestion-colorimetrique");
        NSView* gamut = findIn(panel.form, @"color.output_gamut");
        NSView* transfer = findIn(panel.form, @"color.output_transfer");
        NSButton* apply = buttonOf(panel, @"Appliquer");
        if ([gamut isKindOfClass:NSPopUpButton.class] &&
            [transfer isKindOfClass:NSPopUpButton.class] && apply) {
            NSPopUpButton* gamutMenu = (NSPopUpButton*)gamut;
            NSPopUpButton* transferMenu = (NSPopUpButton*)transfer;
            [gamutMenu
                selectItemAtIndex:
                    [gamutMenu indexOfItemWithRepresentedObject:@"rec709"]];
            [transferMenu
                selectItemAtIndex:[transferMenu
                                      indexOfItemWithRepresentedObject:@"hlg"]];
            [apply performClick:nil];
            UiSmokeCheck(
                self.window.attachedSheet == panel && panel.presented &&
                    [panel.errorLabel.stringValue
                        isEqualToString:@"La sortie HLG doit utiliser le gamut "
                                        @"Rec.2020."] &&
                    self.state->document.SaveToString() == before,
                "PRD26: HLG out of Rec.2020 is refused on the panel's error "
                "line, the panel open, nothing applied");
            expectPanel(panel, "« Gestion colorimétrique » refused",
                        @"Gestion colorimétrique du projet", @"Appliquer",
                        @"Annuler");
            keep(panel, "prd26-gestion-colorimetrique-refus");
        } else {
            UiSmokeCheck(false, "PRD26: the colour form has its output menus "
                                "under their identifiers");
        }
        dismiss(panel, @"Annuler", "« Gestion colorimétrique »");
        UiSmokeCheck(self.state->document.SaveToString() == before,
                     "PRD26: « Annuler » leaves the colour settings as they "
                     "were");
    }

    // « Exporter la vidéo finale »: on the engine's default preset (PRD16).
    {
        CMFormPanel* panel = openPanel("« Exporter la vidéo finale »", ^{
          [self exportFinalVideo:nil];
        });
        NSView* found = findIn(panel.form, @"export.preset");
        NSPopUpButton* preset = [found isKindOfClass:NSPopUpButton.class]
                                    ? (NSPopUpButton*)found
                                    : nil;
        UiSmokeCheck(
            preset &&
                preset.indexOfSelectedItem ==
                    (NSInteger)Exporter::PresetIndex(
                        Exporter::DefaultPreset(self.state->document)) &&
                findIn(panel.form, @"export.container") &&
                findIn(panel.form, @"export.resolution") &&
                findIn(panel.form, @"export.rate") &&
                findIn(panel.form, @"export.encoder"),
            "PRD26: the export form opens on the engine's default "
            "preset, its four overrides beside it");
        expectPanel(panel, "« Exporter la vidéo finale »",
                    @"Exporter la vidéo finale", @"Choisir la destination…",
                    @"Annuler");
        keep(panel, "prd26-exporter");
        dismiss(panel, @"Annuler", "« Exporter la vidéo finale »");
        UiSmokeCheck(!self.state->exportRunning,
                     "PRD26: and « Annuler » starts no export");
    }

    // « Raccourcis clavier »: a wrong combination refused on the panel,
    // « Valeurs par défaut » in place, nothing stored.
    {
        NSDictionary* stored = [NSUserDefaults.standardUserDefaults
            dictionaryForKey:@"KeyboardShortcuts.v1"];
        NSDictionary* bindings = [self.shortcutBindings copy];
        CMFormPanel* panel = openPanel("« Raccourcis clavier »", ^{
          [self editKeyboardShortcuts:nil];
        });
        expectPanel(panel, "« Raccourcis clavier »", @"Raccourcis clavier",
                    @"Enregistrer", @"Annuler");
        keep(panel, "prd26-raccourcis");
        NSDictionary* first = [self shortcutDefinitions].firstObject;
        NSView* found = findIn(
            panel.form, [@"shortcut." stringByAppendingString:first[@"id"]]);
        NSTextField* field =
            [found isKindOfClass:NSTextField.class] ? (NSTextField*)found : nil;
        NSButton* defaults = buttonOf(panel, @"Valeurs par défaut");
        NSButton* save = buttonOf(panel, @"Enregistrer");
        UiSmokeCheck(field && defaults && save &&
                         (panel.styleMask & NSWindowStyleMaskResizable) != 0 &&
                         NSMinX(defaults.frame) <
                             NSMinX(buttonOf(panel, @"Annuler").frame),
                     "PRD26: the shortcuts panel is resizable, its fields "
                     "under their identifiers, « Valeurs par défaut » apart "
                     "at the left");
        if (field && defaults && save) {
            [defaults performClick:nil];
            field.stringValue = @"A+B";
            [save performClick:nil];
            NSString* parseError = nil;
            ShortcutSignature(@"A+B", &parseError);
            NSString* expected = [NSString
                stringWithFormat:@"%@ : %@", first[@"title"], parseError];
            UiSmokeCheck(
                self.window.attachedSheet == panel && panel.presented &&
                    [panel.errorLabel.stringValue isEqualToString:expected],
                ("PRD26: a combination of two keys is refused on the panel's "
                 "error line, the panel open (« " +
                 std::string(panel.errorLabel.stringValue.UTF8String ?: "") +
                 " »)")
                    .c_str());
            expectPanel(panel, "« Raccourcis clavier » refused",
                        @"Raccourcis clavier", @"Enregistrer", @"Annuler");
            keep(panel, "prd26-raccourcis-refus");
            [defaults performClick:nil];
            UiSmokeCheck(
                self.window.attachedSheet == panel && panel.presented &&
                    [field.stringValue isEqualToString:first[@"default"]] &&
                    panel.errorLabel.stringValue.length == 0,
                "PRD26: « Valeurs par défaut » fills the fields in "
                "place and clears the error line");
        }
        dismiss(panel, @"Annuler", "« Raccourcis clavier »");
        NSDictionary* storedAfter = [NSUserDefaults.standardUserDefaults
            dictionaryForKey:@"KeyboardShortcuts.v1"];
        UiSmokeCheck((stored == storedAfter ||
                      [stored isEqualToDictionary:storedAfter]) &&
                         [bindings isEqualToDictionary:self.shortcutBindings],
                     "PRD26: « Annuler » stores no shortcut");
    }

    // « Moteur IA »: a URL that is not HTTP(S) refused on the panel; nothing
    // stored, the Keychain not touched.
    {
        CMFormPanel* panel = openPanel("« Moteur IA »", ^{
          [self.chatPanelView showConfigurationWindow];
        });
        expectPanel(panel, "« Moteur IA »", @"Moteur IA", @"Enregistrer",
                    @"Annuler");
        NSView* menu = findIn(panel.form, @"ai_engine.provider");
        NSView* url = findIn(panel.form, @"ai_engine.base_url");
        NSButton* save = buttonOf(panel, @"Enregistrer");
        if ([menu isKindOfClass:NSPopUpButton.class] &&
            [url isKindOfClass:NSTextField.class] && save) {
            NSPopUpButton* protocols = (NSPopUpButton*)menu;
            [protocols selectItemAtIndex:
                           ai_engine::MenuIndexOf(
                               ai_engine::EngineProtocol::AnthropicMessages)];
            [protocols sendAction:protocols.action to:protocols.target];
            ((NSTextField*)url).stringValue = @"ftp://exemple";
            [save performClick:nil];
            UiSmokeCheck(self.window.attachedSheet == panel &&
                             panel.presented &&
                             [panel.errorLabel.stringValue
                                 hasPrefix:@"Renseignez une URL HTTP(S) "
                                           @"complète"],
                         "PRD26: a URL that is not HTTP(S) is refused on the "
                         "engine panel's error line, the panel open");
            expectPanel(panel, "« Moteur IA » refused", @"Moteur IA",
                        @"Enregistrer", @"Annuler");
            keep(panel, "prd26-moteur-ia-refus");
        } else {
            UiSmokeCheck(false, "PRD26: the engine panel holds the form's "
                                "menu and URL field");
        }
        dismiss(panel, @"Annuler", "« Moteur IA »");
    }

    // « Déplacer le rush »: the bins, the rush's own selected.
    {
        NSString* mediaId = nil;
        const LibraryMedia* media = nullptr;
        for (const LibraryMedia& candidate : self.state->document.library)
            if (!candidate.id.empty()) {
                media = &candidate;
                mediaId = CMStringFromUtf8(candidate.id);
                break;
            }
        if (media) {
            const std::string bin = media->bin_id;
            CMFormPanel* panel = openPanel("« Déplacer le rush »", ^{
              [self moveMediaIdsToBin:@[ mediaId ]];
            });
            NSView* found = findIn(panel.form, @"media.move.destination");
            NSPopUpButton* destinations =
                [found isKindOfClass:NSPopUpButton.class]
                    ? (NSPopUpButton*)found
                    : nil;
            NSString* chosen =
                destinations.selectedItem.representedObject ?: @"";
            UiSmokeCheck(
                destinations &&
                    destinations.numberOfItems ==
                        (NSInteger)self.state->document.bins.size() + 1 &&
                    [chosen isEqualToString:CMStringFromUtf8(bin)],
                "PRD26: the move form lists « Sans chutier » and "
                "every bin, the rush's own chosen");
            expectPanel(panel, "« Déplacer le rush »", @"Déplacer le rush",
                        @"Déplacer", @"Annuler");
            keep(panel, "prd26-deplacer-rush");
            // Escape itself, typed, when the sheet has the keys.
            if (panel && destinations && gUiSmokeScreen.key) {
                waitFor([&] { return panel.isKeyWindow; }, 2.0);
                if (panel.isKeyWindow)
                    SendKeyThroughApplication(destinations, @"\x1b", 53);
                waitFor([&] { return self.window.attachedSheet == nil; }, 3.0);
            }
            const LibraryMedia* after =
                self.state->document.FindLibraryMedia(mediaId.UTF8String ?: "");
            UiSmokeCheck(!panel.presented && self.window.attachedSheet == nil &&
                             after != nullptr && after->bin_id == bin,
                         "PRD26: Escape closes « Déplacer le rush », nothing "
                         "moved",
                         ui_smoke::Needs::kKeyWindow);
            if (panel.presented)
                dismiss(panel, @"Annuler", "« Déplacer le rush »");
        } else {
            UiSmokeCheck(false, "PRD26: the smoke project has a rush to "
                                "move");
        }
    }

    // « Nouvelle timeline avec la sélection »: one rush selected in the
    // media browser, list or grid, the selection put back.
    {
        const BOOL grid = self.mediaViewToggle.selectedSegment == 1;
        NSIndexSet* selection = self.mediaTable.selectedRowIndexes;
        NSSet<NSIndexPath*>* selectedPaths =
            self.mediaCollection.selectionIndexPaths;
        NSInteger row = -1;
        for (NSUInteger index = 0; index < self.visibleMediaIds.count; ++index)
            if (self.state->document.FindLibraryMedia(
                    self.visibleMediaIds[index].UTF8String ?: "")) {
                row = (NSInteger)index;
                break;
            }
        const auto restore = [&] {
            if (grid)
                self.mediaCollection.selectionIndexPaths = selectedPaths;
            else
                [self.mediaTable selectRowIndexes:selection
                             byExtendingSelection:NO];
        };
        if (row >= 0) {
            if (grid)
                self.mediaCollection.selectionIndexPaths =
                    [NSSet setWithObject:[NSIndexPath indexPathForItem:row
                                                             inSection:0]];
            else
                [self.mediaTable
                        selectRowIndexes:[NSIndexSet
                                             indexSetWithIndex:(NSUInteger)row]
                    byExtendingSelection:NO];
            CMFormPanel* panel =
                openPanel("« Nouvelle timeline avec la sélection »", ^{
                  [self createTimelineFromSelectedMedia:nil];
                });
            UiSmokeCheck(
                [findIn(panel.form, @"timeline.from_selection.name")
                    isKindOfClass:NSTextField.class] &&
                    [panel.messageLabel.stringValue
                        isEqualToString:@"Les 1 rushes seront placés bout à "
                                        @"bout."],
                "PRD26: the selection's timeline form says how many rushes "
                "it lays end to end");
            expectPanel(panel, "« Nouvelle timeline avec la sélection »",
                        @"Nouvelle timeline avec la sélection", @"Créer",
                        @"Annuler");
            keep(panel, "prd26-timeline-selection");
            dismiss(panel, @"Annuler",
                    "« Nouvelle timeline avec la sélection »");
            restore();
        } else {
            UiSmokeCheck(false,
                         ("PRD26: the media browser shows a rush to select (" +
                          std::to_string(self.visibleMediaIds.count) +
                          " items shown, none a rush, in the " +
                          (grid ? "grid" : "list") + ")")
                             .c_str());
        }
    }

    // A marker's note: « Supprimer le marqueur » apart at the left, its own
    // answer; nothing applied by the panel itself.
    {
        __block NSModalResponse answered = 0;
        __block NSString* typed = nil;
        CMFormPanel* panel = openPanel("a marker's note", ^{
          [self askMarkerNote:@"Reprendre le cadre"
                         time:RationalTime{25, 25}
                     duration:RationalTime{0, 25}
                    removable:YES
                       answer:^(NSModalResponse response, NSString* text) {
                         answered = response;
                         typed = text;
                       }];
        });
        NSView* found = findIn(panel.form, @"marker.note");
        UiSmokeCheck([found isKindOfClass:NSTextField.class] &&
                         [((NSTextField*)found).stringValue
                             isEqualToString:@"Reprendre le cadre"] &&
                         panel.initialFirstResponder == found,
                     "PRD26: the note's field holds the note and takes the "
                     "keyboard");
        expectPanel(panel, "a marker's note", panel.titleLabel.stringValue,
                    @"Enregistrer", @"Annuler");
        NSButton* remove = buttonOf(panel, @"Supprimer le marqueur");
        UiSmokeCheck(remove && remove.keyEquivalent.length == 0 &&
                         NSMinX(remove.frame) <
                             NSMinX(buttonOf(panel, @"Annuler").frame),
                     "PRD26: « Supprimer le marqueur » answers no key and "
                     "sits apart at the left");
        keep(panel, "prd26-note-marqueur");
        [remove performClick:nil];
        waitFor([&] { return self.window.attachedSheet == nil; }, 3.0);
        UiSmokeCheck(answered == NSAlertThirdButtonReturn &&
                         [typed isEqualToString:@"Reprendre le cadre"] &&
                         self.window.attachedSheet == nil,
                     "PRD26: « Supprimer le marqueur » closes the note with "
                     "its own answer, the text typed with it");
    }

    // Built and not shown: the new project's name (modal, from the welcome
    // screen) and the two progress panels.
    {
        CMFormPanel* panel = [self newProjectNamePanel];
        NSView* found = findIn(panel.form, @"project.new.name");
        UiSmokeCheck([found isKindOfClass:NSTextField.class] &&
                         [((NSTextField*)found).stringValue
                             isEqualToString:@"Nouveau projet"],
                     "PRD26: the new project's name proposes « Nouveau "
                     "projet »");
        expectPanel(panel, "« Créer un nouveau projet »",
                    @"Créer un nouveau projet", @"Continuer", @"Annuler");
        keep(panel, "prd26-nouveau-projet");
        for (const bool h264 : {true, false}) {
            CMFormPanel* progress = [self exportProgressPanel:h264];
            NSString* title =
                h264 ? @"Export H.264 en cours" : @"Export HEVC en cours";
            const std::string problems = UiSmokeFormPanelProblems(progress);
            NSButton* cancel = buttonOf(progress, @"Annuler");
            UiSmokeCheck(
                problems.empty() && cancel &&
                    [cancel.keyEquivalent isEqualToString:@"\x1b"] &&
                    progress.buttons.count == 1 &&
                    [progress.titleLabel.stringValue isEqualToString:title],
                ("PRD26: the export's progress is titled « " +
                 std::string(title.UTF8String) +
                 " », laid out whole, « Annuler » on Escape" +
                 (problems.empty() ? std::string() : " --" + problems))
                    .c_str());
            if (h264) keep(progress, "prd26-progression-export");
        }
        CMFormPanel* collect = [self collectProgressPanel];
        const std::string problems = UiSmokeFormPanelProblems(collect);
        UiSmokeCheck(problems.empty() && buttonOf(collect, @"Masquer") &&
                         collect.buttons.count == 1,
                     ("PRD26: the collection's progress is laid out whole, "
                      "« Masquer » its one button" +
                      (problems.empty() ? std::string() : " --" + problems))
                         .c_str());
        keep(collect, "prd26-progression-collecte");
    }
    UiSmokeCheck(self.window.attachedSheet == nil,
                 "PRD26: no sheet is left on the window");
}

// DEC39 -- docs/roadmap/PHASES-2026-08.md. « Fichier › Nouveau projet… » and
// « Ouvrir un projet… »: the two items, and what a choice leads to up to
// the reopening, which the window test never does -- in its build,
// -switchToProjectAtPath: leaves the path in pendingProjectSwitchPath and
// quits nothing. The open and save panels run out of process and are not
// shown; the form panel of the name is PRD26's. Needs neither the screen
// nor the focus.
- (void)runUiSmokeProjectSwitchScenario {
    NSMenu* fileMenu = [NSApp.mainMenu itemWithTitle:@"Fichier"].submenu;
    NSMenuItem* create = [fileMenu itemWithTitle:@"Nouveau projet…"];
    NSMenuItem* open = [fileMenu itemWithTitle:@"Ouvrir un projet…"];
    UiSmokeCheck(create.action == @selector(newProjectFromEditor:) &&
                     open.action == @selector(openProjectFromEditor:) &&
                     [fileMenu indexOfItem:create] == 0 &&
                     [fileMenu indexOfItem:open] == 1,
                 "DEC39: the Fichier menu opens on « Nouveau projet… » and "
                 "« Ouvrir un projet… »");
    // An import or an analysis still running holds the window: wait for
    // the ones the run started.
    const auto quiet = [&] {
        if (self.state->exportRunning) return false;
        if (!self.state->mediaTasks) return true;
        for (const MediaTaskSnapshot& task : self.state->mediaTasks->Snapshot())
            if (task.state == MediaTaskState::Running ||
                task.state == MediaTaskState::Queued)
                return false;
        return true;
    };
    UiSmokeWaitUntil(quiet, 60.0);
    UiSmokeCheck(quiet(), "DEC39: no task runs before the project changes");

    self.pendingProjectSwitchPath = nil;
    UiSmokeCheck(![self switchToProjectAtPath:self.documentPath] &&
                     self.pendingProjectSwitchPath == nil &&
                     [self.infoLabel.stringValue
                         isEqualToString:@"Ce projet est déjà ouvert"] &&
                     self.window.attachedSheet == nil,
                 "DEC39: the window's own project is said to be open, and "
                 "nothing reopens");

    NSString* failure = nil;
    const std::filesystem::path otherFolder =
        gUiSmokeRoot / "QA autre projet.cutmachine-project";
    NSString* other =
        [self createProjectNamed:@"QA autre projet"
                              at:[NSURL fileURLWithPath:@(otherFolder.c_str())]
                           error:&failure];
    const BOOL switched = other != nil && [self switchToProjectAtPath:other];
    UiSmokeCheck(switched &&
                     [self.pendingProjectSwitchPath isEqualToString:other] &&
                     self.window.attachedSheet == nil,
                 ("DEC39: a project made as « Nouveau projet… » makes it is "
                  "checked, this one written, and the editor would reopen "
                  "on it (" +
                  std::string(other.UTF8String ?: failure.UTF8String ?: "") +
                  ")")
                     .c_str());
    self.pendingProjectSwitchPath = nil;

    const std::filesystem::path notProject = gUiSmokeRoot / "QA pas un projet";
    std::filesystem::create_directories(notProject);
    const BOOL refused = ![self switchToProjectAtPath:@(notProject.c_str())];
    NSWindow* sheet = self.window.attachedSheet;
    UiSmokeCheck(refused && self.pendingProjectSwitchPath == nil &&
                     sheet != nil &&
                     [self.infoLabel.stringValue
                         containsString:@"project.cutmachine.json"],
                 ("DEC39: a folder holding no project is refused on a sheet "
                  "that says why: " +
                  std::string(self.infoLabel.stringValue.UTF8String ?: ""))
                     .c_str());
    if (sheet) [self.window endSheet:sheet];
    UiSmokeWaitUntil([&] { return self.window.attachedSheet == nil; }, 2.0);
    UiSmokeCheck(self.window.attachedSheet == nil &&
                     self.pendingProjectSwitchPath == nil,
                 "DEC39: no sheet is left, and nothing to reopen");
}

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. The welcome screen,
// built as -showStartupWindow builds it, its modal session not run. Without
// FFmpeg -- CUTMACHINE_FFMPEG naming nothing, which the resolver reads again
// as soon as the variable changes (FfmpegBinary.h) -- it carries the line
// and « Diagnostic… » once the report is back from its thread; with the
// FFmpeg CMake found, nothing. Then the menu's item, and the dialog it opens,
// built from a report gathered the way the menu gathers one: its text names
// the five items, and « Copier » carries the engine's report. No media task
// runs while FFmpeg is made to be missing, and the variable is put back as
// soon as the reports are in.
//
// Runs in both modes, and needs neither the screen nor the focus: nothing is
// put in front. With CUTMACHINE_UI_QA_DIR, the welcome screen is drawn off
// screen and kept in both states, and the dialog without FFmpeg.
- (void)runUiSmokeSetupNoticeScenario {
    const auto findIn = [](NSView* root, NSString* identifier) -> NSView* {
        if (root == nil) return nil;
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:root];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view.identifier isEqualToString:identifier]) return view;
            [pending addObjectsFromArray:view.subviews];
        }
        return nil;
    };
    const auto labelIn = [&findIn](NSView* root,
                                   NSString* identifier) -> NSTextField* {
        NSView* view = findIn(root, identifier);
        return [view isKindOfClass:NSTextField.class] ? (NSTextField*)view
                                                      : nil;
    };
    const auto waitUntil = [](const std::function<bool()>& done,
                              double seconds) {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
        while (!done() && deadline.timeIntervalSinceNow > 0)
            [NSRunLoop.currentRunLoop
                   runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        return done();
    };
    // Drawn off screen: nothing here needs the screen, and the welcome
    // screen is never put in front of whoever uses the Mac.
    const auto capture = [](NSWindow* window, NSColor* background,
                            const std::string& name) {
        if (!UiQaEnabled()) return;
        std::string error;
        const bool captured =
            WriteUiQaViewCapture(window.contentView, background, name, error);
        UiSmokeCheck(captured, ("PRD21: " + name + " is kept for QA" +
                                (captured ? "" : ": " + error))
                                   .c_str());
    };

    if (self.state && self.state->mediaTasks)
        self.state->mediaTasks->WaitForIdle(30000);
    const char* variable = ffmpeg::kBinaryVariable;
    const char* previous = std::getenv(variable);
    const std::optional<std::string> saved =
        previous != nullptr ? std::optional<std::string>(previous)
                            : std::nullopt;
    const auto restore = [&saved, variable] {
        if (saved)
            setenv(variable, saved->c_str(), 1);
        else
            unsetenv(variable);
    };

    // Without FFmpeg: the welcome screen's report and the menu's, at once.
    setenv(variable, (gUiSmokeRoot / "sans-ffmpeg" / "introuvable").c_str(), 1);
    [self buildStartupWindow];
    NSWindow* welcome = self.startupWindow;
    auto menuReport = std::make_shared<std::optional<SetupNoticeResult>>();
    [self gatherSetupNotice:^(const SetupNoticeResult& result) {
      *menuReport = result;
    }];
    const bool missingChecked = waitUntil(
        [&] {
            return self.startupSetupNoticeChecked && menuReport->has_value();
        },
        30.0);
    restore();
    UiSmokeCheck(missingChecked,
                 "PRD21: without FFmpeg, the welcome screen's report and the "
                 "menu's come back from their thread");
    NSView* content = welcome.contentView;
    NSView* row = findIn(content, @"startup.setup_notice");
    NSTextField* line = labelIn(content, @"startup.setup_notice.text");
    NSView* button = findIn(content, @"startup.setup_notice.diagnose");
    NSButton* diagnose =
        [button isKindOfClass:NSButton.class] ? (NSButton*)button : nil;
    const std::string expected =
        menuReport->has_value()
            ? setup_notice::WelcomeLine((*menuReport)->report)
            : std::string();
    UiSmokeCheck(
        row && !row.hidden && line && !expected.empty() &&
            expected.rfind("FFmpeg introuvable : ", 0) == 0 &&
            [line.stringValue isEqualToString:CMStringFromUtf8(expected)],
        ("PRD21: without FFmpeg, the welcome screen says \"" + expected + "\"")
            .c_str());
    UiSmokeCheck(diagnose && !diagnose.hidden && diagnose.enabled &&
                     [diagnose.title isEqualToString:@"Diagnostic…"] &&
                     diagnose.target == self &&
                     diagnose.action == @selector(showSetupDiagnostic:),
                 "PRD21: and offers « Diagnostic… », which opens the "
                 "diagnosis");
    NSButton* quit = nil;
    for (NSView* view in content.subviews)
        if ([view isKindOfClass:NSButton.class] &&
            [((NSButton*)view).title isEqualToString:@"Quitter"])
            quit = (NSButton*)view;
    if (row && line && diagnose && quit) {
        const NSRect lineRect = [line convertRect:line.bounds toView:content];
        const NSRect buttonRect = [diagnose convertRect:diagnose.bounds
                                                 toView:content];
        const NSSize needed =
            [line.cell cellSizeForBounds:NSMakeRect(0, 0, NSWidth(line.frame),
                                                    CGFLOAT_MAX)];
        UiSmokeCheck(
            NSContainsRect(content.bounds, row.frame) &&
                !NSIntersectsRect(row.frame, quit.frame) &&
                !NSIntersectsRect(lineRect, buttonRect) &&
                needed.height <= NSHeight(line.frame) + 0.5 &&
                [diagnose.cell cellSize].width <= NSWidth(diagnose.frame) + 0.5,
            "PRD21: the line sits inside the screen, clear of "
            "« Quitter » and of its button, and is shown whole");
    } else {
        UiSmokeCheck(false, "PRD21: the welcome screen has its notice row, "
                            "its line, its button and « Quitter »");
    }
    capture(welcome, CMThemeColor(ui::theme::kSurfacePanel),
            "prd21-accueil-sans-ffmpeg");

    NSMenuItem* item = nil;
    for (NSMenuItem* candidate in NSApp.mainMenu.itemArray.firstObject.submenu
             .itemArray)
        if ([candidate.title isEqualToString:@"Diagnostic de l'installation…"])
            item = candidate;
    UiSmokeCheck(item && item.target == self &&
                     item.action == @selector(showSetupDiagnostic:) &&
                     [self validateMenuItem:item],
                 "PRD21: CUTMACHINE ▸ Diagnostic de l'installation… is in "
                 "the menu, enabled");
    if (menuReport->has_value()) {
        const SetupNoticeResult& result = **menuReport;
        // PRD26 -- on the window's form panel now, built and not shown.
        CMFormPanel* panel = [self setupDiagnosticPanel:result];
        [panel layoutForm];
        NSView* textView = findIn(panel.form, @"setup.diagnostic.text");
        NSString* shown = [textView isKindOfClass:NSTextView.class]
                              ? ((NSTextView*)textView).string
                              : @"";
        bool named = true;
        for (const char* heading : {
                 "FFmpeg : absent\n",
                 "Modèle Whisper : ",
                 "MFA : ",
                 "Clé de l'agent : ",
             })
            named = named && [shown containsString:CMStringFromUtf8(heading)];
        UiSmokeCheck(
            named &&
                [shown isEqualToString:CMStringFromUtf8(
                                           setup_notice::DiagnosticText(
                                               result.report, result.facts))],
            "PRD21: the menu's dialog shows the diagnosis, which names the "
            "five items, FFmpeg absent");
        NSButton* copy = nil;
        NSButton* close = nil;
        for (NSButton* candidate in panel.buttons) {
            if ([candidate.identifier isEqualToString:@"setup.diagnostic.copy"])
                copy = candidate;
            if ([candidate.title isEqualToString:@"Fermer"]) close = candidate;
        }
        UiSmokeCheck(copy && copy.target == self &&
                         copy.action == @selector(copySetupDiagnostic:),
                     "PRD21: « Copier le diagnostic » has its own action: "
                     "the dialog stays open");
        const std::string problems = UiSmokeFormPanelProblems(panel);
        UiSmokeCheck(
            problems.empty() && close &&
                [close.keyEquivalent isEqualToString:@"\r"] &&
                [panel.titleLabel.stringValue
                    isEqualToString:@"Diagnostic de l'installation"] &&
                (panel.styleMask & NSWindowStyleMaskResizable) != 0 &&
                NSMinX(copy.frame) < NSMinX(close.frame),
            ("PRD26: the diagnosis is a resizable form panel, laid out "
             "whole, Return on « Fermer », « Copier le diagnostic » apart "
             "at the left" +
             (problems.empty() ? std::string() : " --" + problems))
                .c_str());
        // A pasteboard of its own: the one of whoever uses this Mac is left
        // alone.
        NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
        const BOOL copied = [self writeSetupDiagnosticToPasteboard:board];
        NSString* pasted = [board stringForType:NSPasteboardTypeString];
        UiSmokeCheck(copied && shown.length > 0 && [pasted hasPrefix:shown] &&
                         [pasted containsString:@"cutmachine --check-setup"] &&
                         [pasted containsString:@"\"items\":["],
                     "PRD21: what it copies is the text, then the engine's "
                     "report");
        [board releaseGlobally];
        capture(panel, panel.backgroundColor, "prd21-diagnostic-sans-ffmpeg");
        self.setupDiagnosticClipboard = nil;
    }

    // With the FFmpeg CMake found: nothing on the screen.
    setenv(variable, FFMPEG_EXECUTABLE, 1);
    [self buildStartupWindow];
    welcome = self.startupWindow;
    const bool presentChecked = waitUntil(
        [&] { return static_cast<bool>(self.startupSetupNoticeChecked); },
        60.0);
    restore();
    row = findIn(welcome.contentView, @"startup.setup_notice");
    line = labelIn(welcome.contentView, @"startup.setup_notice.text");
    UiSmokeCheck(presentChecked && row && row.hidden && line &&
                     line.stringValue.length == 0,
                 "PRD21: with FFmpeg, the welcome screen carries no line");
    capture(welcome, CMThemeColor(ui::theme::kSurfacePanel),
            "prd21-accueil-avec-ffmpeg");
    self.startupWindow = nil;
    self.startupSetupNotice = nil;
    self.startupSetupNoticeLabel = nil;
}

// PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The window's own import, run for
// real: the background probe task, the engine's ingest (Ingest.h), and what
// the window then says. Every other check drops a rush the library already
// holds, so this path was compiled and never executed.
//
// Called last: it adds rushes to the library, and nothing after it counts
// them. The task manager's thread reads the files; displayTick is what
// commits a finished batch in the app, and `finish` does the same here once
// the manager is idle, because the smoke run does not let the timer fire.
- (void)runUiSmokeImportScenario {
    const std::string rushName = "import-sonore.mov";
    const std::filesystem::path rush = gUiSmokeRoot / rushName;
    const std::string generate =
        UiQaQuote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y "
        "-f lavfi -i 'color=c=gray:s=64x64:r=25:d=2' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' "
        "-shortest -c:v mpeg4 -c:a pcm_s16le " +
        UiQaQuote(rush.string());
    if (std::system(generate.c_str()) != 0) {
        UiSmokeCheck(false, "import: the sounding rush is created");
        return;
    }
    const auto fileUrl = [](const std::filesystem::path& path) {
        return [NSURL
            fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    };
    const auto finish = [self]() {
        const bool idle = self.state->mediaTasks->WaitForIdle(30000);
        [self processCompletedMediaImports];
        return idle && self.state->pendingImports.empty();
    };
    // Looked up again after every import: the library is a vector, and an
    // import may move it.
    const auto imported = [self, rushName]() -> const LibraryMedia* {
        for (const LibraryMedia& media : self.state->document.library)
            if (media.filename == rushName) return &media;
        return nullptr;
    };
    const auto said = [self]() {
        return std::string(self.binSummaryLabel.stringValue.UTF8String ?: "");
    };

    const size_t before = self.state->document.library.size();
    const bool queued = [self importMediaURLs:@[ fileUrl(rush) ] intoBin:nil];
    UiSmokeCheck(queued && self.state->pendingImports.size() == 1 &&
                     self.state->document.library.size() == before,
                 "import: a file is read by a background task, and nothing "
                 "enters the library before it ends");
    UiSmokeCheck(finish(), "import: the task ends and its batch is committed");
    const LibraryMedia* media = imported();
    UiSmokeCheck(media != nullptr &&
                     self.state->document.library.size() == before + 1 &&
                     media->metadata_complete && media->has_audio &&
                     media->bin_id.empty() &&
                     self.state->document.FindSource(media->id) != nullptr,
                 "import: the rush is in the library, at the root, read, "
                 "with its source");
    const int64_t level = media != nullptr ? media->audio_level : 0;
    UiSmokeCheck(media != nullptr && media->audio_level_measured && level > 0,
                 ("import: its audio level is measured, as --ingest measures "
                  "it: " +
                  std::to_string(level))
                     .c_str());
    UiSmokeCheck(
        said() == "1 rush importé",
        ("import: the window says one rush came in: " + said()).c_str());
    [self drainProjectCommits];
    std::ifstream stored(self.documentPath.UTF8String ?: "");
    const std::string project((std::istreambuf_iterator<char>(stored)),
                              std::istreambuf_iterator<char>());
    UiSmokeCheck(project.find(rushName) != std::string::npos,
                 "import: the project file on disk names the rush");

    [self importMediaURLs:@[ fileUrl(rush) ] intoBin:nil];
    UiSmokeCheck(
        finish() && self.state->document.library.size() == before + 1 &&
            said() == "Aucun changement",
        ("import: the same file again changes nothing: " + said()).c_str());

    Ulid rushesBin;
    for (const DocumentBin& bin : self.state->document.bins)
        if (bin.name == "1_RUSHES") rushesBin = bin.id;
    [self importMediaURLs:@[ fileUrl(rush) ]
                  intoBin:[NSString stringWithUTF8String:rushesBin.c_str()]];
    const bool moved = !rushesBin.empty() && finish();
    media = imported();
    UiSmokeCheck(moved && media != nullptr && media->bin_id == rushesBin &&
                     self.state->document.library.size() == before + 1 &&
                     media->audio_level == level && said() == "1 rush déplacé",
                 ("import: the same file into another bin moves the rush "
                  "there: " +
                  said())
                     .c_str());

    const std::filesystem::path stray = gUiSmokeRoot / "pas-un-media.txt";
    std::ofstream(stray) << "not a media file\n";
    [self importMediaURLs:@[ fileUrl(stray) ] intoBin:nil];
    UiSmokeCheck(
        finish() && self.state->document.library.size() == before + 1 &&
            said() == "1 refusé",
        ("import: a file that is not a media is refused: " + said()).c_str());

    // Cancelling is heard between two files: a batch of twelve cannot have
    // been read through by the time the next line cancels it.
    NSMutableArray<NSURL*>* batch = [NSMutableArray array];
    for (int index = 0; index < 12; ++index) {
        const std::filesystem::path copy =
            gUiSmokeRoot / ("import-lot-" + std::to_string(index) + ".mov");
        std::error_code copyError;
        std::filesystem::copy_file(rush, copy, copyError);
        if (!copyError) [batch addObject:fileUrl(copy)];
    }
    [self importMediaURLs:batch intoBin:nil];
    const bool cancelSent = batch.count == 12 &&
                            self.state->pendingImports.size() == 1 &&
                            self.state->mediaTasks->Cancel(
                                self.state->pendingImports.begin()->first);
    UiSmokeCheck(
        cancelSent && finish() &&
            self.state->document.library.size() == before + 1 &&
            said() == "Import média annulé.",
        ("import: a cancelled batch leaves the library as it was: " + said())
            .c_str());
    // The waveform and the thumbnail queued for the imported rush.
    self.state->mediaTasks->WaitForIdle(30000);

    // PRD37 -- ROADMAP.md. The line under the browser says what the last
    // task did in French, once: the engine's own word for it ("Complete",
    // "Cancelled") stays out, and a task over leaves no bar behind (QA of 3
    // October 2026: « terminée · Th…ov · Complete »).
    [self refreshMediaTaskStatus];
    const std::vector<MediaTaskSnapshot> settled =
        self.state->mediaTasks->Snapshot();
    const ui::media_panel::MediaTaskLine expected =
        ui::media_panel::DescribeMediaTask(
            ui::media_panel::ShownMediaTask(settled));
    NSString* line = self.mediaTaskLabel.stringValue ?: @"";
    bool english = false;
    for (NSString* word in
         @[ @"Complete", @"Cancelled", @"Failed", @"Starting" ])
        english = english || [line containsString:word];
    UiSmokeCheck(!settled.empty() && !english &&
                     [line isEqualToString:CMStringFromUtf8(expected.text)] &&
                     self.mediaTaskProgress.hidden == !expected.shows_progress,
                 ("PRD37: the media task line says the last task's state in "
                  "French, once, and leaves no bar behind: " +
                  std::string(line.UTF8String ?: "") +
                  (self.mediaTaskProgress.hidden ? " (no bar)" : " (a bar)"))
                     .c_str());
}

// F2.4 -- ROADMAP.md. ingest_media from an agent of the window: refused
// until 19 October 2026, it now ends in the window's own import. Played
// through the window's real endpoint, as a Claude Code relayed to it or the
// panel's coding agent (ACP) calls it, then through the adapter the built-in
// loop calls, from a worker thread: the rush must reach the library and the
// browser, measured, the line under the browser must say it, and the
// project on disk must name it. create_project and open_project stay
// refused, with the way to another project. Needs no focus.
//
// Called after the window's own import, for the same reason: it adds rushes
// to the library, and nothing after it counts them.
- (void)runUiSmokeAgentIngestScenario {
    const auto pump = [](NSTimeInterval seconds) {
        NSEvent* event = [NSApp
            nextEventMatchingMask:NSEventMaskAny
                        untilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]
                           inMode:NSDefaultRunLoopMode
                          dequeue:YES];
        if (event) [NSApp sendEvent:event];
    };
    // A worker's call, waited for here while the main queue is served: the
    // adapter hands the window's part to it with dispatch_sync. Past the
    // wait, `unblock` frees a worker stuck on its socket before the join.
    const auto served = [&pump](const std::function<void()>& work,
                                const std::function<void()>& unblock) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([work, done] {
            work();
            done->store(true);
        });
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:60.0];
        while (!done->load() && deadline.timeIntervalSinceNow > 0) pump(0.01);
        const bool finished = done->load();
        if (!finished && unblock) unblock();
        worker.join();
        return finished;
    };
    const auto rushIn = [](const std::filesystem::path& folder,
                           const std::string& name) {
        std::filesystem::create_directories(folder);
        const std::string generate =
            UiQaQuote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -y "
            "-f lavfi -i 'color=c=gray:s=64x64:r=25:d=2' "
            "-f lavfi -i 'sine=frequency=550:sample_rate=48000:duration=2' "
            "-shortest -c:v mpeg4 -c:a pcm_s16le " +
            UiQaQuote((folder / name).string());
        return std::system(generate.c_str()) == 0;
    };
    const auto imported =
        [self](const std::string& name) -> const LibraryMedia* {
        for (const LibraryMedia& media : self.state->document.library)
            if (media.filename == name) return &media;
        return nullptr;
    };
    const auto said = [self]() {
        return std::string(self.binSummaryLabel.stringValue.UTF8String ?: "");
    };
    const auto onDisk = [self](const std::string& name) {
        [self drainProjectCommits];
        std::ifstream stored(self.documentPath.UTF8String ?: "");
        const std::string project((std::istreambuf_iterator<char>(stored)),
                                  std::istreambuf_iterator<char>());
        return project.find(name) != std::string::npos;
    };
    const auto listed = [self](const LibraryMedia* media) {
        return media != nullptr &&
               [self.visibleMediaIds
                   containsObject:[NSString
                                      stringWithUTF8String:media->id.c_str()]];
    };

    const std::filesystem::path relayed = gUiSmokeRoot / "agent-rushes";
    const std::filesystem::path panel = gUiSmokeRoot / "chat-rushes";
    if (!rushIn(relayed, "agent-import.mov") ||
        !rushIn(panel, "chat-import.mov")) {
        UiSmokeCheck(false, "F2.4: the agent's rushes are created");
        return;
    }
    [self spinRunLoopFor:0.3];
    if (!self.agentEndpoint) [self startAgentEndpoint];
    if (self.agentEndpoint == nullptr || !self.agentEndpoint->IsRunning() ||
        self.agentEndpointBackend == nullptr) {
        UiSmokeCheck(false, "F2.4: the window's endpoint listens");
        return;
    }
    const auto rows = [self]() {
        std::vector<std::string> found;
        UiSmokeAgentJournalRows(self.chatPanelView, found);
        return found;
    };
    gUiSmokeAgentJournalTime = "10:00:00";

    // Through the endpoint, as a relayed agent.
    UiSmokeAgentLink agent;
    if (!agent.Connect(self.agentEndpoint->SocketPath())) {
        gUiSmokeAgentJournalTime.clear();
        UiSmokeCheck(false, "F2.4: an agent connects to the window's socket");
        return;
    }
    int expectedEvents = gUiSmokeAgentEvents;
    const auto settle = [&](int events) {
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
        while (gUiSmokeAgentEvents < events &&
               deadline.timeIntervalSinceNow > 0)
            pump(0.01);
    };
    const auto send = [&](const std::string& line) {
        auto answer = std::make_shared<std::string>();
        UiSmokeAgentLink* link = &agent;
        served(
            [link, line, answer] {
                std::string received;
                if (link->Send(line) && link->Receive(received))
                    *answer = received;
            },
            [link] { link->Close(); });
        settle(++expectedEvents);
        return *answer;
    };
    int requestId = 200;
    const auto call = [&](const std::string& tool, const std::string& arguments,
                          bool& isError) {
        const std::string line =
            send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++requestId) +
                 R"(,"method":"tools/call","params":{"name":")" + tool +
                 R"(","arguments":)" + arguments + "}}");
        mcp_json::Value root;
        mcp_json::Value payload;
        std::string parseError;
        isError = true;
        if (mcp_json::Value::Parse(line, root, parseError) &&
            root.Find("result") != nullptr) {
            const mcp_json::Value& result = *root.Find("result");
            const mcp_json::Value* flag = result.Find("isError");
            isError = flag != nullptr && flag->AsBool();
            const mcp_json::Value* content = result.Find("content");
            if (content != nullptr && !content->AsArray().empty() &&
                content->AsArray()[0].Find("text") != nullptr)
                mcp_json::Value::Parse(
                    content->AsArray()[0].Find("text")->AsString(), payload,
                    parseError);
        }
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const char* key) {
        const mcp_json::Value* found = value.Find(key);
        return found != nullptr && found->IsString() ? found->AsString()
                                                     : std::string();
    };
    send(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
         R"({"protocolVersion":"2025-06-18","capabilities":{},)"
         R"("clientInfo":{"name":"ui-smoke-f24","version":"1"}}})");
    agent.Send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");

    const size_t rowsBefore = rows().size();
    bool isError = true;
    const mcp_json::Value answer = call(
        "ingest_media",
        R"({"path":")" + relayed.string() + R"(","recursive":true})", isError);
    int64_t added = 0;
    const mcp_json::Value* addedField = answer.Find("added");
    const bool counted = addedField != nullptr && addedField->AsInt64(added);
    const LibraryMedia* media = imported("agent-import.mov");
    UiSmokeCheck(!isError && counted && added == 1 && media != nullptr &&
                     media->metadata_complete && media->has_audio &&
                     media->audio_level_measured && media->bin_id.empty() &&
                     self.state->document.FindSource(media->id) != nullptr,
                 ("F2.4: ingest_media from a relayed agent adds the rush to "
                  "the window's library, read and measured: " +
                  answer.Dump())
                     .c_str());
    UiSmokeCheck(said() == "1 rush importé" && listed(media) &&
                     onDisk("agent-import.mov"),
                 ("F2.4: the window shows the agent's rush in the browser, "
                  "says it came in, and writes it to the project: " +
                  said())
                     .c_str());
    const auto lastRow = [&rows]() {
        const std::vector<std::string> found = rows();
        return found.empty() ? std::string() : found.back();
    };
    UiSmokeCheck(rows().size() == rowsBefore + 1 &&
                     lastRow() == "10:00:00 Importer des rushes",
                 ("F2.4: the Agent panel's journal says the agent imported "
                  "rushes: " +
                  lastRow())
                     .c_str());

    // create_project and open_project of another project: refused, with
    // the project the window holds and the way to another.
    const mcp_json::Value created = call(
        "create_project",
        R"({"path":")" + (gUiSmokeRoot / "Autre.cutmachine-project").string() +
            R"(","name":"Autre"})",
        isError);
    const std::string createdDetail = text(created, "detail");
    UiSmokeCheck(
        isError && text(created, "error") == "UnsupportedOperation" &&
            createdDetail.find("« Nouveau projet… »") != std::string::npos &&
            !std::filesystem::exists(gUiSmokeRoot / "Autre.cutmachine-project"),
        ("F2.4: create_project from the window's agent is refused "
         "with the way to another project: " +
         createdDetail)
            .c_str());
    UiSmokeCheck(
        lastRow() == "10:00:00 Refusé : créer un projet (UnsupportedOperation)",
        ("F2.4: the journal shows the refusal: " + lastRow()).c_str());
    agent.Close();
    settle(++expectedEvents);

    // Through the adapter the built-in loop calls (MainThreadBackend.h), from
    // a worker thread, as ChatSession does.
    McpToolCallOutcome fromPanel;
    MainThreadBackend* adapter = self.agentEndpointBackend;
    const std::string panelPath = panel.string();
    const bool answered = served(
        [adapter, panelPath, &fromPanel] {
            const McpToolRegistry registry;
            mcp_json::Value arguments = mcp_json::Value::MakeObject();
            arguments.Set("path", mcp_json::Value::MakeString(panelPath));
            fromPanel = registry.Call(*adapter, "ingest_media", arguments);
        },
        nullptr);
    const LibraryMedia* chatMedia = imported("chat-import.mov");
    UiSmokeCheck(
        answered && fromPanel.ok &&
            fromPanel.result_json.find("\"added\":1") != std::string::npos &&
            chatMedia != nullptr && chatMedia->audio_level_measured &&
            listed(chatMedia) && said() == "1 rush importé" &&
            onDisk("chat-import.mov"),
        ("F2.4: ingest_media from the panel's own loop imports the "
         "rush the same way: " +
         fromPanel.result_json)
            .c_str());
    gUiSmokeAgentJournalTime.clear();
    // The waveforms and thumbnails queued for the agent's rushes.
    self.state->mediaTasks->WaitForIdle(30000);
}

- (void)runUiSmokeTests {
    // This is an in-process end-to-end smoke test: the real AppDelegate has
    // loaded and locked a disposable project, built the actual AppKit tree,
    // initialized Metal, and wired every target/action. It requires a macOS
    // WindowServer, but no Accessibility permission or synthetic global input.
    UiSmokeCheck(self.window != nil && self.state != nullptr,
                 "application launches a real editor window");

    // ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. Each page opens its
    // panes, and the panes it opens are wide; then the run's own layout,
    // every panel open, comes back. Nothing is written into the editor's
    // preferences (rememberWorkspace is NO for the whole run).
    {
        NSSplitView* split = self.workspaceSplitView;
        const auto panes = [&]() -> std::string {
            std::string names;
            for (NSView* pane in split.arrangedSubviews)
                names += pane == self.mediaPanel        ? "M"
                         : pane == self.rightDockPanel  ? "I"
                         : pane == self.editorSplitView ? "E"
                                                        : "?";
            return names;
        };
        UiSmokeCheck(!self.rememberWorkspace,
                     "ON1: the smoke run writes no page into the editor's "
                     "preferences");
        // The pages' own widths, for this scenario only.
        self.workspaceWidthsFixed = NO;
        [self applyWorkspacePage:ui::workspace::Page::Edit
                          panels:ui::workspace::DefaultPanels(
                                     ui::workspace::Page::Edit)
                        remember:NO];
        UiSmokeCheck(panes() == "E" && !self.state->sourceMonitorVisible,
                     ("ON2: Montage shows the timeline and Record alone, "
                      "without Source (" +
                      panes() + ")")
                         .c_str());
        UiSmokeCheck([self.monitorSplitView.arrangedSubviews
                         containsObject:self.workspaceActivityScroll] &&
                         [self.workspaceActivityText.string
                             containsString:@"MARQUEURS"] &&
                         [self.workspaceActivityText.string
                             containsString:@"MODIFICATIONS RÉCENTES"],
                     "ON7: Montage keeps the markers and durable edit trace "
                     "beside Record");
        [self keepFilmcraftDrawing:"on7-edit-page"];
        [self toggleWorkspacePanel:ui::workspace::Side::Inspector];
        const ui::workspace::PanelWidths edit = ui::workspace::SidePanelWidths(
            ui::workspace::Page::Edit, {false, true, false},
            split.bounds.size.width, split.dividerThickness);
        UiSmokeCheck(
            panes() == "EI" && std::abs(self.rightDockPanel.frame.size.width -
                                        edit.inspector) <= 2.0,
            ("ON2: the Inspector called on Montage opens at its "
             "share of the window (" +
             std::to_string(self.rightDockPanel.frame.size.width) + " for " +
             std::to_string(edit.inspector) + ")")
                .c_str());

        [self toggleWorkspacePanel:ui::workspace::Side::Inspector];
        UiSmokeCheck(panes() == "E", "ON2: and is dismissed");
        [self applyWorkspacePage:ui::workspace::Page::Media
                          panels:ui::workspace::DefaultPanels(
                                     ui::workspace::Page::Media)
                        remember:NO];
        UiSmokeCheck(
            panes() == "ME" && self.state->sourceMonitorVisible &&
                self.mediaPanel.frame.size.width >= 420.0,
            ("ON1: Médias opens the media panel, wide, and Source (" + panes() +
             ", " + std::to_string(self.mediaPanel.frame.size.width) + ")")
                .c_str());
        UiSmokeCheck(self.editorSplitView.arrangedSubviews.count == 1 &&
                         self.monitorSplitView.arrangedSubviews.count == 1 &&
                         self.monitorSplitView.arrangedSubviews.firstObject ==
                             self.sourceMonitorPanel &&
                         self.binSidebarScroll.frame.size.width >= 160.0 &&
                         self.mediaIconScroll.frame.size.width >= 400.0,
                     "ON5: Media gives Source the whole height and the browser "
                     "readable bins and a grid");
        [self keepFilmcraftDrawing:"on5-media-page"];
        [self applyWorkspacePage:ui::workspace::Page::Color
                          panels:ui::workspace::DefaultPanels(
                                     ui::workspace::Page::Color)
                        remember:NO];
        UiSmokeCheck(
            panes() == "EI" && !self.state->sourceMonitorVisible &&
                self.rightDockPanel.selectedSlot == ui::PanelSlot::Inspector,
            "ON1: Couleur opens the Inspector, without Source");
        UiSmokeCheck(self.inspectorView.colorWorkspace &&
                         self.editorSplitView.arrangedSubviews.count == 2 &&
                         ![self.monitorSplitView.arrangedSubviews
                             containsObject:self.workspaceActivityScroll],
                     "ON4: Color restores the timeline and opens dedicated "
                     "grade controls");
        if (UiQaEnabled()) {
            const Ulid selected =
                self.state->interaction->PrimarySelectedClipId();
            for (const DocumentTrack& track :
                 self.state->document.sequence.tracks) {
                if (track.kind != "video" || track.clips.empty()) {
                    continue;
                }
                self.state->interaction->SelectClip(track.clips.front().id);
                break;
            }
            [self updateSelectionInfo];
            [self keepFilmcraftDrawing:"on4-color-page"];
            self.state->interaction->SelectClip(selected);
            [self updateSelectionInfo];
        }
        self.workspaceWidthsFixed = YES;
        [self applyWorkspacePage:ui::workspace::Page::Edit
                          panels:ui::workspace::PanelSet{true, true, true}
                        remember:NO];
        UiSmokeCheck(
            panes() == "MEI" && self.state->sourceMonitorVisible &&
                std::abs(self.mediaPanel.frame.size.width - 320.0) <= 2.0 &&
                std::abs(self.rightDockPanel.frame.size.width - 300.0) <= 2.0,
            "ON1: the run's layout, every panel open at its width "
            "from before pages, is back");
        UiSmokeCheck(!self.inspectorView.colorWorkspace &&
                         ![self.monitorSplitView.arrangedSubviews
                             containsObject:self.workspaceActivityScroll],
                     "ON4/ON7: the reference layout restores the compact "
                     "Inspector and monitor surfaces");

        // #248 -- ROADMAP.md, « Dans la fenêtre ». The QA of 19 September
        // saw a search magnifier lying over the others in a dock, the day
        // the Audio tab came back with a search field at the Media tab's
        // height. Every panel open, whatever the media tab, the window
        // shows one search field at most, and none lies over another. The
        // view tree alone says it: no focus, no capture.
        const NSInteger shownTab = self.mediaTabStrip.selectedIndex;
        const auto& tabs = ui::media_panel::AllTabs();
        for (NSInteger index = 0; index < static_cast<NSInteger>(tabs.size());
             ++index) {
            [self.mediaTabStrip selectIndex:index];
            NSMutableArray<NSView*>* fields = [NSMutableArray array];
            NSMutableArray<NSView*>* pending =
                [NSMutableArray arrayWithObject:self.window.contentView];
            while (pending.count > 0) {
                NSView* view = pending.lastObject;
                [pending removeLastObject];
                if (view.isHidden) {
                    continue;
                }
                if ([view isKindOfClass:NSSearchField.class] &&
                    !NSIsEmptyRect(view.visibleRect)) {
                    [fields addObject:view];
                }
                [pending addObjectsFromArray:view.subviews];
            }
            bool apart = true;
            for (NSUInteger a = 0; a < fields.count; ++a) {
                const NSRect first = [fields[a] convertRect:fields[a].bounds
                                                     toView:nil];
                for (NSUInteger b = a + 1; b < fields.count; ++b) {
                    const NSRect second =
                        [fields[b] convertRect:fields[b].bounds toView:nil];
                    if (NSIntersectsRect(first, second)) {
                        apart = false;
                    }
                }
            }
            UiSmokeCheck(
                fields.count <= 1 && apart,
                ("#248: the " +
                 std::string(ui::media_panel::TabIdentifier(tabs[index])) +
                 " tab leaves one search field at most in the window, "
                 "none over another (" +
                 std::to_string(fields.count) + " shown)")
                    .c_str());
        }
        [self.mediaTabStrip selectIndex:shownTab];
    }

    // ON3 -- docs/roadmap/ONGLETS-2026-10.md. The Agent in a window of its
    // own, out of the right dock: hidden at first, shown and hidden from
    // every page by the bar's toggle, from the Affichage menu and its key,
    // floating over the editor and opened where it was left. Nothing is
    // written into the editor's preferences.
    {
        NSPanel* agent = self.agentWindow;
        NSButton* toggle = self.workspaceBar.agentToggle;
        NSMenuItem* entry = nil;
        for (NSMenuItem* menu in NSApp.mainMenu.itemArray)
            for (NSMenuItem* item in menu.submenu.itemArray)
                if (item.action == @selector(toggleAgentWindow:)) entry = item;
        bool agentTab = false;
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:self.rightDockPanel];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view isKindOfClass:NSButton.class] &&
                [((NSButton*)view).title isEqualToString:@"Agent"])
                agentTab = true;
            [pending addObjectsFromArray:view.subviews];
        }
        UiSmokeCheck(
            agent != nil && self.chatPanelView.window == agent &&
                ![self.chatPanelView isDescendantOf:self.rightDockPanel] &&
                !agentTab && !agent.visible && !self.agentShown,
            "ON3: the Agent is a window of its own, the right dock "
            "has no Agent tab, and it is hidden at first");
        UiSmokeCheck(
            entry != nil && [entry.keyEquivalent isEqualToString:@"a"] &&
                entry.keyEquivalentModifierMask ==
                    (NSEventModifierFlagCommand | NSEventModifierFlagOption) &&
                [toggle.toolTip hasSuffix:@"⌥⌘A"] && !toggle.hidden &&
                NSContainsRect(self.workspaceBar.bounds, toggle.frame),
            "ON3: the Affichage menu's « Agent » carries ⌥⌘A, which the "
            "bar's toggle says");
        // At the editor's narrowest, the bar's three toggles, the pages and
        // the status line do not run into one another.
        {
            CMWorkspaceBarView* bar = self.workspaceBar;
            const NSSize wide = bar.frame.size;
            [bar setFrameSize:NSMakeSize(self.window.contentMinSize.width,
                                         wide.height)];
            NSMutableArray<NSView*>* parts = [NSMutableArray array];
            for (NSView* view in bar.subviews)
                if (!view.hidden) [parts addObject:view];
            bool apart = parts.count >= 5;
            for (NSUInteger a = 0; a < parts.count; ++a) {
                apart = apart && NSContainsRect(bar.bounds, parts[a].frame);
                for (NSUInteger b = a + 1; b < parts.count; ++b)
                    apart = apart &&
                            !NSIntersectsRect(parts[a].frame, parts[b].frame);
            }
            [bar setFrameSize:wide];
            UiSmokeCheck(apart,
                         "ON3: at the editor's narrowest, the bar's status "
                         "line, pages and three toggles do not overlap");
        }

        // From every page, the toggle shows it and hides it.
        self.workspaceWidthsFixed = NO;
        std::optional<ui::agent_window::Rect> first;
        std::string seen;
        bool everyPage = agent != nil && entry != nil;
        for (const ui::workspace::PageDescriptor& page :
             ui::workspace::Pages()) {
            [self applyWorkspacePage:page.page
                              panels:ui::workspace::DefaultPanels(page.page)
                            remember:NO];
            [toggle performClick:nil];
            if (!first)
                first = ui::agent_window::Rect{
                    agent.frame.origin.x, agent.frame.origin.y,
                    agent.frame.size.width, agent.frame.size.height};
            [self validateMenuItem:entry];
            const bool shown = agent.visible && self.agentShown &&
                               agent.isFloatingPanel &&
                               toggle.state == NSControlStateValueOn &&
                               entry.state == NSControlStateValueOn;
            [toggle performClick:nil];
            [self validateMenuItem:entry];
            const bool hidden = !agent.visible && !self.agentShown &&
                                toggle.state == NSControlStateValueOff &&
                                entry.state == NSControlStateValueOff;
            everyPage = everyPage && shown && hidden;
            seen += std::string(page.identifier) + (shown ? "+" : "!") +
                    (hidden ? "-" : "!") + " ";
        }
        UiSmokeCheck(everyPage,
                     ("ON3: on every page the bar's toggle shows the Agent's "
                      "window, floating, the toggle and the menu entry on, "
                      "and hides it (" +
                      seen + ")")
                         .c_str());
        const NSRect editor = self.window.frame;
        UiSmokeCheck(
            first && *first == ui::agent_window::DefaultPlace(
                                   {editor.origin.x, editor.origin.y,
                                    editor.size.width, editor.size.height}),
            "ON3: the first time, it opens at the editor's top right "
            "corner, under its title bar");

        // ⌥⌘A, through the menus: shown with the keyboard in its field,
        // hidden with the keyboard back to the editor.
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:self.metalView];
        NSEvent* chord = UiSmokeKeyEvent(
            self.window, @"å", @"a", 0,
            NSEventModifierFlagCommand | NSEventModifierFlagOption);
        const BOOL took = [NSApp.mainMenu performKeyEquivalent:chord];
        const bool typing =
            took && self.agentShown && agent.isKeyWindow &&
            [agent.firstResponder isKindOfClass:NSTextView.class];
        [NSApp.mainMenu performKeyEquivalent:chord];
        UiSmokeCheck(typing && !self.agentShown && !agent.visible &&
                         self.window.isKeyWindow,
                     "ON3: ⌥⌘A shows the Agent's window with the keyboard in "
                     "its field, and again hides it, the keyboard back to the "
                     "editor",
                     ui_smoke::Needs::kKeyWindow);

        // Moved by the hand, it opens there again; its close button hides
        // it as the toggle does.
        [self showAgentWindow:YES focus:NO];
        const NSRect moved = NSOffsetRect(agent.frame, -60.0, -40.0);
        [agent setFrame:moved display:NO];
        [self showAgentWindow:NO focus:NO];
        [self showAgentWindow:YES focus:NO];
        const bool kept = NSEqualRects(agent.frame, moved);
        [agent performClose:nil];
        UiSmokeCheck(kept && !agent.visible && !self.agentShown &&
                         toggle.state == NSControlStateValueOff,
                     "ON3: moved, it opens where it was left, and its close "
                     "button hides it");

        self.workspaceWidthsFixed = YES;
        [self applyWorkspacePage:ui::workspace::Page::Edit
                          panels:ui::workspace::PanelSet{true, true, true}
                        remember:NO];
        [self.window makeFirstResponder:self.metalView];
    }

    // CF1 -- native screen enumeration and the actual menu, even with one
    // display. No physical input or visible window is needed for these.
    {
        VideoOutputState output;
        std::string name;
        std::string message;
        NSView* const parent = self.programMonitorView.superview;
        UiSmokeCheck([self accessVideoOutput:std::nullopt
                                       state:output
                                   errorName:name
                                     message:message] &&
                         !output.screens.empty(),
                     "CF1: the live editor lists its connected screens");
        const auto screens = output.screens;
        for (const VideoOutputScreen& screen : screens) {
            if (!screen.editor_screen) continue;
            UiSmokeCheck(
                ![self accessVideoOutput:std::optional<std::string>{screen.id}
                                   state:output
                               errorName:name
                                 message:message] &&
                    name == "ValidationFailed" &&
                    self.videoOutputWindow == nil &&
                    self.programMonitorView.superview == parent,
                "CF1: the editor's screen is refused without moving Record");
            NSMenuItem* choice = nil;
            for (NSMenuItem* item in self.videoOutputMenu.itemArray)
                if ([item.representedObject
                        isEqual:CMStringFromUtf8(screen.id)])
                    choice = item;
            UiSmokeCheck(choice && !choice.enabled,
                         "CF1: the menu disables the editor's screen");
        }
        UiSmokeCheck([self accessVideoOutput:std::optional<std::string>{""}
                                       state:output
                                   errorName:name
                                     message:message] &&
                         output.selected_screen_id.empty() &&
                         output.active_screen_id.empty(),
                     "CF1: disabling leaves no output window or selection");
    }

    // Becoming key is a round trip through the WindowServer, and the
    // dispatch_async that got us here only promises the next main-queue turn
    // -- so applicationDidFinishLaunching's activateIgnoringOtherApps has
    // usually, but not always, landed. AppKit routes the first click on a
    // non-key window to activating it rather than to the view under it, so
    // losing that race made every selection-, rename- and drag-dependent
    // check below fail *together*, intermittently: measured at 1 run in 6 on
    // an otherwise idle Mac, and easily mistaken for a real regression by
    // whoever ran ctest next.
    //
    // Waiting is the fix rather than retrying a click: a click that arrives
    // before the window is key is a different gesture, not a lost one.
    //
    // PRD28 -- docs/roadmap/PRODUIT-2026-10b.md. CUTMACHINE_UI_SMOKE_SIMULATE
    // plays the run on a screen it does not have: "no-focus", the activation
    // is not attempted and the window counts as not key, whatever AppKit
    // says; "hidden", it counts as not visible either, and no capture of it
    // is attempted. That is how what a run reports on a Mac in use is checked
    // on a free one, the same way every time, without putting another
    // application in front of anyone.
    const char* simulation = std::getenv("CUTMACHINE_UI_SMOKE_SIMULATE");
    const std::string simulated = simulation != nullptr ? simulation : "";
    const bool simulatedHidden = simulated == "hidden";
    const bool simulatedNoFocus = simulatedHidden || simulated == "no-focus";
    if (!simulated.empty() && !simulatedNoFocus)
        UiSmokeCheck(false, ("CUTMACHINE_UI_SMOKE_SIMULATE is no-focus or "
                             "hidden, not \"" +
                             simulated + "\"")
                                .c_str());
    gUiSmokeEditorWindow = self.window;
    if (!simulatedNoFocus && !self.window.isKeyWindow) {
        [NSApp activateIgnoringOtherApps:YES];
        [self.window makeKeyAndOrderFront:nil];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
        while (!self.window.isKeyWindow && deadline.timeIntervalSinceNow > 0) {
            NSEvent* event = [NSApp
                nextEventMatchingMask:NSEventMaskAny
                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                               inMode:NSDefaultRunLoopMode
                              dequeue:YES];
            if (event) [NSApp sendEvent:event];
        }
    }
    // PRD28 -- and whether WindowServer composites it. AppKit may report a
    // window just shown as visible a moment late: it gets the two seconds a
    // capture gives it (CaptureUiQaWindow).
    const auto composited = [&] {
        return (self.window.occlusionState & NSWindowOcclusionStateVisible) !=
               0;
    };
    NSDate* visibleBy = [NSDate dateWithTimeIntervalSinceNow:2.0];
    while (!simulatedHidden && !composited() &&
           visibleBy.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    const std::string simulatedNote =
        " (simulated: CUTMACHINE_UI_SMOKE_SIMULATE=" + simulated + ")";
    gUiSmokeScreen.key = !simulatedNoFocus && self.window.isKeyWindow;
    gUiSmokeScreen.not_key =
        std::string("the editor window could not become key") +
        (simulatedNoFocus ? simulatedNote : "");
    gUiSmokeScreen.visible = !simulatedHidden && composited();
    gUiSmokeScreen.not_visible =
        kUiSmokeHiddenReason + (simulatedHidden ? simulatedNote : "");
    // Reported rather than assumed: on a machine with no usable WindowServer
    // session -- a locked screen, a headless runner -- or while someone uses
    // the Mac, the window never becomes key. PRD28 -- that is no failure of
    // the editor: this line and every check that needs the key window are
    // reported SKIP with that reason, and the run says how many it did not
    // play. One honest line naming the screen is worth more than seven that
    // blame the media panel.
    UiSmokeCheck(self.window.isKeyWindow,
                 "editor window becomes key before any input check",
                 ui_smoke::Needs::kKeyWindow);

    // PERF-2026-09. The decoders no longer open before the window exists --
    // that is what made opening a project on an external drive show nothing
    // for seconds. Behind the window, the media are checked for presence and
    // the decoders the playhead shows are opened (PERF10), so the state
    // every check below reads is only settled once both have landed. Waiting
    // here rather than in each check keeps the wait honest: if the
    // background work never completes, this is the assertion that says so.
    const auto decodersSettling = [&] {
        return self.state->checkingSourceAvailability ||
               !self.state->openingDecoders.empty();
    };
    if (decodersSettling()) {
        NSDate* workerDeadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
        while (decodersSettling() && workerDeadline.timeIntervalSinceNow > 0) {
            NSEvent* event = [NSApp
                nextEventMatchingMask:NSEventMaskAny
                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                               inMode:NSDefaultRunLoopMode
                              dequeue:YES];
            if (event) [NSApp sendEvent:event];
        }
    }
    UiSmokeCheck(gUiSmokeWorkersAtWindowShow == 0,
                 "no decoder is opened before the window is on screen");

    // QA pass of 8 October 2026 -- CUTMACHINE_UI_SMOKE_ONLY plays some
    // scenarios alone, where the whole run plays them: "ecran", the screen
    // pass, from the main run loop; "prd26", the forms, in this block. A
    // check of them is tried in a minute, not in the four the run takes.
    if (const char* only = std::getenv("CUTMACHINE_UI_SMOKE_ONLY");
        only != nullptr && only[0] != '\0') {
        // CH6 (review) -- the scenarios by name, each a whole token of the
        // list ("ecran,prd26"): "ch" no longer runs with any name that
        // holds those two letters.
        std::set<std::string> names;
        {
            std::string name;
            for (const char* at = only;; ++at) {
                if (*at == '\0' || *at == ',' || *at == ' ') {
                    if (!name.empty()) names.insert(name);
                    name.clear();
                    if (*at == '\0') break;
                } else {
                    name += *at;
                }
            }
        }
        const auto wants = [&names](const char* name) {
            return names.count(name) > 0;
        };
        if (wants("prd26")) [self runUiSmokeFormPanelScenario];
        // DEC39 -- the Fichier menu's change of project.
        if (wants("dec39")) [self runUiSmokeProjectSwitchScenario];
        // FC-fenêtre -- the magnet, the trim mode, the Trim Monitor, the
        // slide and the rate stretch, wired in the window.
        if (wants("fc")) [self runUiSmokeFilmcraftScenario];
        // SY2 -- Auto Align on the waveform, wired in the window.
        if (wants("sy2")) [self runUiSmokeSeparateSoundScenario];
        // F2.3 -- a carton's style in the Inspector.
        if (wants("f23")) [self runUiSmokeCaptionStyleScenario];
        const bool screen = wants("ecran");
        // CH6 -- the Charte panel, on a library of its own; from the main
        // run loop, as the agent journal's: its reads and writes come back
        // on the main queue.
        const bool charter = wants("ch");
        CFRunLoopRef mainLoop = CFRunLoopGetMain();
        CFRunLoopPerformBlock(mainLoop, kCFRunLoopDefaultMode, ^{
          if (charter) [self runUiSmokeCharterScenario];
          if (screen) [self runUiSmokeScreenPass];
          [self finishUiSmokeTests];
        });
        CFRunLoopWakeUp(mainLoop);
        return;
    }

    // PERF-2026-09. Media whose technical metadata the project already holds
    // must be read from the project, not re-probed off the drive: that probe
    // was about 110 ms per rush before the window could appear, and it was a
    // second answer to a question Export.cc and Operations.cc already
    // resolve from the library. 90 degrees is the fixture's stored value and
    // is not what the file itself reports.
    const auto storedMetadata =
        self.state->mediaMetadata.find(gUiSmokeStoredMetadataMediaId);
    UiSmokeCheck(storedMetadata != self.state->mediaMetadata.end() &&
                     storedMetadata->second.rotation_degrees == 90,
                 "media with complete metadata is read from the project "
                 "rather than probed off the drive");
    UiSmokeCheck(!decodersSettling() && !self.state->workers.empty(),
                 "the decoders opened behind the window are installed");
    // PERF10 -- DecoderPool.h. Opening a 169-rush 4K project used to open a
    // decoder per rush, 1,030 threads before the first frame, and playing it
    // through froze the machine. Only what the playhead shows is decoded --
    // PERF12: and what playback reaches next, as planned.
    {
        std::set<Ulid> wanted = self.state->plannedSourceIds;
        for (const ResolvedSlot& slot : self.state->requested)
            if (slot.active) wanted.insert(slot.sourceId);
        size_t videoSources = 0;
        for (const DocumentSource& source : self.state->document.sources)
            if (DecodeOpenFor(self.state->document, source,
                              [self decodeBasePath], false))
                ++videoSources;
        bool onlyWanted = true;
        for (const auto& worker : self.state->workers)
            onlyWanted &= wanted.count(worker.first) != 0;
        UiSmokeCheck(onlyWanted && self.state->workers.size() < videoSources,
                     "a project opens decoders only for the sources the "
                     "playhead shows or reaches next, not for every rush");
    }
    const auto captureQaState = [&](const char* name) {
        if (!UiQaEnabled()) return;
        std::string captureError;
        const bool captured =
            CaptureUiQaWindow(self.window, name, captureError);
        const std::string label = captured
                                      ? "VQA captured " + std::string(name)
                                      : "VQA capture " + std::string(name) +
                                            " failed: " + captureError;
        UiSmokeCheck(captured, label.c_str(), ui_smoke::Needs::kVisibleWindow);
    };
    // PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. The guard against drift:
    // a visible button of the editor window with no word on it and no
    // accessibility label fails the run. Swept as the editor opens, with a
    // picture's grade in the Inspector and with a motion layer in it.
    const auto expectNamedButtons = [&](const std::string& moment) {
        size_t seen = 0;
        std::vector<std::string> unnamed;
        CollectUnnamedButtons(self.window.contentView, seen, unnamed);
        std::string list;
        for (const std::string& entry : unnamed) list += " " + entry + ";";
        UiSmokeCheck(seen > 0 && unnamed.empty(),
                     ("PRD27: every visible button " + moment +
                      " says in words what it does, to VoiceOver too (" +
                      std::to_string(seen) + " buttons" +
                      (unnamed.empty() ? "" : "; unnamed:" + list) + ")")
                         .c_str());
    };
    // PRD37 -- ROADMAP.md. Opened, the timeline is fitted to the view it has
    // once laid out, the whole film in it (QA of 3 October 2026: fitted at
    // 971 points while the window was being built, shown at 917.5, the end
    // of the film out of view and the zoom bar short of its rail).
    {
        const double width = self.metalView.bounds.size.width;
        TimelineViewport fitted = self.state->viewport;
        const bool measurable = std::isfinite(width) &&
                                width > fitted.header_width &&
                                self.state->duration.value > 0;
        if (measurable) fitted.FitDuration(self.state->duration, width);
        const double shown = self.state->viewport.pixels_per_second;
        const double end = self.state->viewport.TimeToX(self.state->duration);
        char detail[192];
        std::snprintf(detail, sizeof detail,
                      " (%.2f points a second, %.2f fit a view %.1f points "
                      "wide; the film ends at %.1f)",
                      shown, fitted.pixels_per_second, width, end);
        UiSmokeCheck(measurable &&
                         std::abs(shown - fitted.pixels_per_second) <=
                             1e-6 * fitted.pixels_per_second &&
                         end <= width,
                     (std::string("PRD37: opened, the timeline is fitted to "
                                  "the view it has once laid out") +
                      detail)
                         .c_str());
    }
    // PRD37 -- ROADMAP.md. The media panel says what it says whole, and its
    // selected bin reads (QA of 3 October 2026: « Reche », « 1_RUSH » and
    // « Chutier re » cut without an ellipsis, a grey name on the blue of
    // the selection at 2.05:1).
    {
        NSSearchFieldCell* searchCell = self.mediaSearchField.cell;
        const NSRect searchText =
            [searchCell searchTextRectForBounds:self.mediaSearchField.bounds];
        // Nil is not none: AppKit then writes its own « Search ».
        NSString* placeholder =
            self.mediaSearchField.placeholderAttributedString.string
                ?: self.mediaSearchField.placeholderString;
        const double placeholderWidth =
            placeholder.length == 0
                ? 0.0
                : [placeholder sizeWithAttributes:@{
                      NSFontAttributeName : self.mediaSearchField.font
                  }]
                      .width;
        char detail[128];
        std::snprintf(
            detail, sizeof detail, " (« %s », %.1f points in %.1f)",
            placeholder != nil ? placeholder.UTF8String : "AppKit's own",
            placeholderWidth, NSWidth(searchText));
        UiSmokeCheck(
            placeholder != nil && placeholderWidth <= NSWidth(searchText),
            (std::string("PRD37: the media search field's prompt is "
                         "never cut") +
             detail)
                .c_str());

        // Where each name's text is drawn -- its frame less the cell's own
        // margins -- against the list's visible edge.
        NSClipView* binClip = self.binOutline.enclosingScrollView.contentView;
        std::string overflowing;
        double furthest = 0.0;
        for (NSInteger row = 0; row < self.binOutline.numberOfRows; ++row) {
            NSTableCellView* cell = [self.binOutline viewAtColumn:0
                                                              row:row
                                                  makeIfNecessary:YES];
            NSTextField* name = cell.textField;
            if (name == nil) continue;
            const NSRect drawn =
                [name convertRect:[name.cell drawingRectForBounds:name.bounds]
                           toView:binClip];
            furthest = std::max(furthest, NSMaxX(drawn));
            if (NSMaxX(drawn) > NSMaxX(binClip.bounds) + 0.5)
                overflowing +=
                    " " + std::string(name.stringValue.UTF8String ?: "");
        }
        char listDetail[160];
        std::snprintf(
            listDetail, sizeof listDetail,
            " (text up to %.1f points, list %.1f wide, its column "
            "%.1f%s%s)",
            furthest, NSWidth(binClip.bounds),
            self.binOutline.tableColumns.firstObject.width,
            overflowing.empty() ? "" : "; past it:", overflowing.c_str());
        UiSmokeCheck(self.binOutline.numberOfRows >= 4 && overflowing.empty(),
                     (std::string("PRD37: every bin's name ends inside the bin "
                                  "list, so that a long one is shortened with "
                                  "an ellipsis rather than cut by its edge") +
                      listDetail)
                         .c_str());

        const NSInteger selectedBin = self.binOutline.selectedRow;
        NSTableRowView* selectedRow =
            selectedBin < 0
                ? nil
                : [self.binOutline rowViewAtRow:selectedBin makeIfNecessary:NO];
        NSTableCellView* selectedCell =
            selectedBin < 0 ? nil
                            : [self.binOutline viewAtColumn:0
                                                        row:selectedBin
                                            makeIfNecessary:NO];
        NSColor* nameColor = [selectedCell.textField.textColor
            colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        NSColor* wanted = [CMThemeColor(ui::theme::ListRowLabel(true))
            colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        // The theme's row, and nothing of AppKit's drawn over it.
        const bool themed =
            [selectedRow isKindOfClass:CMIndustrialTableRowView.class] &&
            self.binOutline.selectionHighlightStyle ==
                NSTableViewSelectionHighlightStyleNone;
        const bool primary =
            nameColor != nil && wanted != nil &&
            std::abs(nameColor.redComponent - wanted.redComponent) < 0.01 &&
            std::abs(nameColor.greenComponent - wanted.greenComponent) < 0.01 &&
            std::abs(nameColor.blueComponent - wanted.blueComponent) < 0.01;
        UiSmokeCheck(selectedRow != nil && themed && primary,
                     (std::string("PRD37: the selected bin is drawn in the "
                                  "theme's selection, its name in the primary "
                                  "text (row ") +
                      (themed ? "themed" : "AppKit's") + ", name " +
                      (primary ? "primary" : "not primary") + ")")
                         .c_str());
    }
    captureQaState("00-initial");

    NSButton* toggle = self.sourceMonitorToggleButton;
    const NSPoint togglePoint =
        NSMakePoint(NSMidX(toggle.frame), NSMidY(toggle.frame));
    UiSmokeCheck([self.programMonitorView hitTest:togglePoint] == toggle,
                 "Source toggle is hit-testable above the Metal view");
    ClickControlThroughWindow(toggle);
    UiSmokeCheck(!self.state->sourceMonitorVisible &&
                     self.sourceMonitorPanel.superview == nil &&
                     self.programMonitorPanel.frame.size.width ==
                         self.monitorSplitView.bounds.size.width &&
                     [toggle.title isEqualToString:@"SOURCE OFF"],
                 "Source toggle collapses the Source monitor");
    ClickControlThroughWindow(toggle);
    const double viewerWidthDelta =
        std::abs(self.sourceMonitorPanel.frame.size.width -
                 self.programMonitorPanel.frame.size.width);
    const double viewerHeightDelta =
        std::abs(self.sourceMonitorPanel.frame.size.height -
                 self.programMonitorPanel.frame.size.height);
    UiSmokeCheck(
        self.state->sourceMonitorVisible &&
            self.sourceMonitorPanel.superview == self.monitorSplitView &&
            viewerWidthDelta < 1.0 && viewerHeightDelta < 1.0 &&
            [toggle.title isEqualToString:@"SOURCE ON"],
        "Source toggle restores two equally sized monitors");

    NSPopUpButton* scopePopup = self.programVideoScopePopup;
    const NSPoint scopePoint =
        NSMakePoint(NSMidX(scopePopup.frame), NSMidY(scopePopup.frame));
    UiSmokeCheck([self.programMonitorView hitTest:scopePoint] == scopePopup,
                 "Record scope selector is hit-testable above Metal");
    const VideoScopeMode initialScope = self.state->programVideoScope;
    [scopePopup
        selectItemWithTag:static_cast<NSInteger>(VideoScopeMode::Waveform)];
    [scopePopup sendAction:scopePopup.action to:scopePopup.target];
    UiSmokeCheck(
        self.state->programVideoScope == VideoScopeMode::Waveform &&
            [NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey] ==
                static_cast<NSInteger>(VideoScopeMode::Waveform),
        "Record waveform toggle updates and persists local monitor state");
    [self presentNearestFrameAtDeadline:NO];
    UiSmokeCheck(!self.state->overlayDirty,
                 "Record waveform executes the Metal scope render pass");
    // PRD37 -- ROADMAP.md. The panel the scope is drawn in covers none of
    // Record's controls (QA of 3 October 2026: the selector lay over the
    // darkest fifth of a parade).
    {
        const NSRect panel = [self programScopePanelFrame];
        std::string covered;
        for (NSView* control in self.programMonitorView.subviews)
            if ([control isKindOfClass:NSButton.class] && !control.hidden &&
                NSIntersectsRect(panel, control.frame))
                covered +=
                    " " +
                    std::string(((NSButton*)control).title.UTF8String ?: "?");
        char detail[160];
        std::snprintf(detail, sizeof detail,
                      " (panel from %.1f to %.1f points up, selector up to "
                      "%.1f;%s)",
                      NSMinY(panel), NSMaxY(panel), NSMaxY(scopePopup.frame),
                      covered.empty() ? " covers nothing" : covered.c_str());
        UiSmokeCheck(!NSIsEmptyRect(panel) && covered.empty(),
                     (std::string("PRD37: the scope panel stands clear of "
                                  "Record's controls") +
                      detail)
                         .c_str());
    }
    [scopePopup selectItemWithTag:static_cast<NSInteger>(initialScope)];
    [scopePopup sendAction:scopePopup.action to:scopePopup.target];

    // G6 -- docs/roadmap/GRADE-2026-09.md. The comparison's wiring: the
    // gesture flips and keeps the bypass, a grab keeps a still of the
    // sequence's size and splits Record, forgetting it unsplits. What the
    // eye sees there is VISUAL_QA_CHECKLIST.md's.
    {
        const MonitorCompare initialCompare = self.state->monitorCompare;
        [self toggleGradeBypass:nil];
        UiSmokeCheck(
            self.state->monitorCompare.bypass_grade !=
                    initialCompare.bypass_grade &&
                [CMUiPreferences.sharedPreferences monitorCompare]
                        .bypass_grade ==
                    self.state->monitorCompare.bypass_grade &&
                !self.programCompareBadge.hidden ==
                    self.state->monitorCompare.bypass_grade,
            "Ignoring the grade flips, persists and says so on Record");
        [self toggleGradeBypass:nil];
        [self grabReferenceStill:nil];
        [self presentNearestFrameAtDeadline:NO];
        UiSmokeCheck(
            !self.state->grabReferenceRequested && [self referenceStillFits] &&
                self.state->monitorCompare.split &&
                [NSFileManager.defaultManager
                    fileExistsAtPath:[CMUiPreferences.sharedPreferences
                                             referenceStillPath]],
            "A grab keeps a still of the sequence's size and splits Record");
        [self presentNearestFrameAtDeadline:NO];
        [self forgetReferenceStill:nil];
        UiSmokeCheck(
            !self.state->referenceStill && !self.state->monitorCompare.split &&
                [CMUiPreferences.sharedPreferences referenceStillPath] == nil,
            "Forgetting the still unsplits Record");
        self.state->monitorCompare = initialCompare;
        [self saveMonitorCompare];
    }

    std::optional<Ulid> initialTrackId;
    std::optional<Ulid> initialClipId;
    std::optional<Ulid> initialAudioTrackId;
    std::optional<Ulid> initialCaptionClipId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind == "video" && !track.clips.empty()) {
            initialTrackId = track.id;
            initialClipId = track.clips.front().id;
        }
        if (track.kind == "audio") initialAudioTrackId = track.id;
        if (track.kind == "caption" && !track.clips.empty())
            initialCaptionClipId = track.clips.front().id;
    }
    const auto displayTracks =
        TimelineTracksInDisplayOrder(self.state->document);
    const auto displayIndexFor = [&](const std::optional<Ulid>& trackId) {
        if (!trackId) return size_t{0};
        const auto found = std::find_if(
            displayTracks.begin(), displayTracks.end(),
            [&](const DocumentTrack* track) { return track->id == *trackId; });
        return found == displayTracks.end()
                   ? size_t{0}
                   : static_cast<size_t>(found - displayTracks.begin());
    };
    const size_t videoDisplayIndex = displayIndexFor(initialTrackId);
    const size_t audioDisplayIndex = displayIndexFor(initialAudioTrackId);
    UiSmokeCheck(initialTrackId.has_value() && initialClipId.has_value(),
                 "UI fixture exposes a selectable video clip");
    if (initialClipId) {
        const double rulerTimelineY = kTimelineToolbarHeight + 8.0;
        const NSPoint playheadPoint =
            NSMakePoint(self.state->viewport.TimeToX({37, 25}),
                        self.metalView.bounds.size.height - rulerTimelineY);
        const NSPoint contentPoint =
            [self.metalView convertPoint:playheadPoint
                                  toView:self.window.contentView];
        UiSmokeCheck(
            [self.window.contentView hitTest:contentPoint] == self.metalView,
            "Window hit-test routes timeline coordinates to Metal");
        SendTimelineMouseGesture(self.metalView, playheadPoint, playheadPoint);
        UiSmokeCheck(self.state->requestedPosition == RationalTime(37, 25),
                     "Timeline click moves the playhead through AppKit");

        // PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. What VoiceOver reads of
        // the three Metal views: a name each, and for the timeline where the
        // playhead stands and what is selected, current whenever it is read.
        {
            const auto named = [](NSView* view, const char* label) {
                return view.isAccessibilityElement &&
                       [view.accessibilityRole
                           isEqualToString:NSAccessibilityGroupRole] &&
                       [view.accessibilityLabel
                           isEqualToString:CMStringFromUtf8(label)];
            };
            UiSmokeCheck(
                named(self.metalView, ui::accessibility::kTimelineLabel) &&
                    named(self.sourceMonitorView,
                          ui::accessibility::kSourceMonitorLabel) &&
                    named(self.programMonitorView,
                          ui::accessibility::kRecordMonitorLabel),
                "PRD27: VoiceOver finds the timeline and the two monitors by "
                "their names");
            const auto read = [&] {
                id value = self.metalView.accessibilityValue;
                return [value isKindOfClass:NSString.class]
                           ? std::string(((NSString*)value).UTF8String ?: "")
                           : std::string();
            };
            const std::vector<Ulid> savedSelection =
                self.state->interaction->SelectedClipIds();
            self.state->interaction->SelectClips({});
            const std::string bare = read();
            self.state->interaction->SelectClip(*initialClipId);
            const DocumentClip* chosen =
                self.state->document.FindClip(*initialClipId);
            const std::string name =
                chosen ? TimelineDrawnClipName(self.state->document,
                                               self.state->project, chosen,
                                               chosen->source_id)
                       : std::string();
            const std::string selected = read();
            [self requestTimelinePosition:{50, 25}];
            const std::string moved = read();
            [self requestTimelinePosition:{37, 25}];
            self.state->interaction->SelectClips(savedSelection);
            UiSmokeCheck(
                bare == "Tête de lecture 00:00:01:12, aucun plan sélectionné" &&
                    !name.empty() &&
                    selected ==
                        "Tête de lecture 00:00:01:12, plan sélectionné : " +
                            name &&
                    moved ==
                        "Tête de lecture 00:00:02:00, plan sélectionné : " +
                            name,
                ("PRD27: the timeline's value follows the playhead and the "
                 "selection: « " +
                 bare + " », « " + selected + " », « " + moved + " »")
                    .c_str());
        }
        // PRD27 -- that the guard sees what it guards: a button carrying a
        // glyph alone, put in the window for the time of one sweep.
        {
            NSButton* probe = [NSButton buttonWithTitle:@"↺"
                                                 target:nil
                                                 action:nil];
            probe.frame = NSMakeRect(0.0, 0.0, 12.0, 12.0);
            [self.window.contentView addSubview:probe];
            size_t seen = 0;
            std::vector<std::string> withProbe;
            CollectUnnamedButtons(self.window.contentView, seen, withProbe);
            [probe removeFromSuperview];
            std::vector<std::string> without;
            CollectUnnamedButtons(self.window.contentView, seen, without);
            UiSmokeCheck(withProbe.size() == without.size() + 1,
                         "PRD27: the guard flags a button that carries only a "
                         "glyph");
        }
        expectNamedButtons("as the editor opens");

        const TimelineViewport savedViewport = self.state->viewport;
        self.state->viewport.view_start = {0, 25};
        self.state->viewport.pixels_per_second = 2000.0;
        self.state->viewport.ScrollByPixels(-10000.0, 25);
        UiSmokeCheck(self.state->viewport.view_start == RationalTime{0, 25},
                     "Timeline navigation cannot expose time before zero");
        TimelineZoomBarGeometry zoomGeometry = CalculateTimelineZoomBarGeometry(
            self.state->viewport, self.state->duration,
            self.metalView.bounds.size.width);
        const double zoomBarViewY = kTimelineZoomBarHeight * 0.5;
        const NSPoint thumbStart =
            NSMakePoint(zoomGeometry.thumb_x + zoomGeometry.thumb_width * 0.5,
                        zoomBarViewY);
        const double beforeBarZoom = self.state->viewport.pixels_per_second;
        SendTimelineMouseGesture(
            self.metalView, thumbStart,
            NSMakePoint(thumbStart.x + 40.0, thumbStart.y));
        UiSmokeCheck(self.state->viewport.view_start > RationalTime{0, 25} &&
                         std::abs(self.state->viewport.pixels_per_second -
                                  beforeBarZoom) < 0.001 &&
                         !self.state->zoomBarDrag,
                     "Dragging the bottom thumb pans the visible timeline");
        zoomGeometry = CalculateTimelineZoomBarGeometry(
            self.state->viewport, self.state->duration,
            self.metalView.bounds.size.width);
        const NSPoint rightHandle =
            NSMakePoint(zoomGeometry.thumb_x + zoomGeometry.thumb_width - 2.0,
                        zoomBarViewY);
        const double beforeHandleZoom = self.state->viewport.pixels_per_second;
        SendTimelineMouseGesture(
            self.metalView, rightHandle,
            NSMakePoint(rightHandle.x - 40.0, rightHandle.y));
        UiSmokeCheck(self.state->viewport.pixels_per_second > beforeHandleZoom,
                     "Dragging a bottom-bar handle changes timeline zoom");
        self.state->viewport = savedViewport;

        const DocumentClip* beforeMove =
            self.state->document.FindClip(*initialClipId);
        const RationalTime originalTimelineIn =
            beforeMove ? beforeMove->timeline_in : RationalTime{0, 25};
        const size_t beforeMoveEdits = self.state->editLog.AppliedCount();
        const double trackTimelineY =
            kTimelineRulerHeight +
            (videoDisplayIndex + 0.5) * self.state->viewport.track_height;
        const NSPoint dragStart =
            NSMakePoint(self.state->viewport.TimeToX({25, 25}),
                        self.metalView.bounds.size.height - trackTimelineY);
        const NSPoint dragEnd = NSMakePoint(dragStart.x + 60.0, dragStart.y);
        self.state->interaction->PointerDown(
            dragStart.x, trackTimelineY, self.metalView.bounds.size.width, 25);
        self.state->interaction->PointerDrag(dragEnd.x, trackTimelineY,
                                             self.metalView.bounds.size.width);
        const auto movingPreview = self.state->interaction->MovePreview();
        const TimelineRenderData movingData = [self timelineRenderData];
        const double movingX =
            movingPreview
                ? self.state->viewport.TimeToX(movingPreview->timeline_in)
                : -1.0;
        const double movingY = [self videoHeight] + kTimelineRulerHeight + 3.5;
        const double movingTrackY =
            movingY + videoDisplayIndex * self.state->viewport.track_height;
        const bool hasOpaqueMovingBody = std::any_of(
            movingData.overlays.begin(), movingData.overlays.end(),
            [&](const MetalDrawCommand& overlay) {
                const MetalRect* rect = std::get_if<MetalRect>(&overlay);
                return rect && std::abs(rect->x - movingX) < 0.01 &&
                       std::abs(rect->y - movingTrackY) < 0.01 &&
                       std::abs(rect->height - 40.0) < 0.01 &&
                       std::abs(rect->alpha - 1.0f) < 0.001f &&
                       std::abs(rect->bottom_alpha - 1.0f) < 0.001f;
            });
        UiSmokeCheck(
            movingPreview && movingPreview->valid && hasOpaqueMovingBody,
            "Timeline overwrite preview remains fully opaque");
        self.state->interaction->CancelDrag();
        SendTimelineMouseGesture(self.metalView, dragStart, dragEnd);
        const DocumentClip* afterMove =
            self.state->document.FindClip(*initialClipId);
        UiSmokeCheck(afterMove &&
                         afterMove->timeline_in != originalTimelineIn &&
                         self.state->editLog.AppliedCount() > beforeMoveEdits,
                     "Timeline drag moves a clip through EditLog");

        self.shortcutBindings[@"play.forward"] = @"L";
        self.shortcutBindings[@"play.reverse"] = @"J";
        self.shortcutBindings[@"play.stop"] = @"K";
        SendKeyThroughWindow(self.metalView, @"l", 37);
        SendKeyThroughWindow(self.metalView, @"l", 37);
        UiSmokeCheck(self.state->playbackDirection == 2,
                     "Second L press enables 2x forward shuttle");
        SendKeyThroughWindow(self.metalView, @"k", 40);
        SendKeyThroughWindow(self.metalView, @"j", 38);
        SendKeyThroughWindow(self.metalView, @"j", 38);
        UiSmokeCheck(self.state->playbackDirection == -2,
                     "Second J press enables 2x reverse shuttle");
        SendKeyThroughWindow(self.metalView, @"k", 40);
        UiSmokeCheck(self.state->playbackDirection == 0,
                     "K stops shuttle playback");
        [self requestTimelinePosition:{0, 25}];
        SendKeyThroughWindow(self.metalView, @"\uf701", 125);
        const RationalTime nextCut = self.state->requestedPosition;
        SendKeyThroughWindow(self.metalView, @"\uf700", 126);
        UiSmokeCheck(nextCut > RationalTime{0, 25} &&
                         self.state->requestedPosition == RationalTime{0, 25},
                     "Down/Up navigate to the next/previous exact cut");

        // PRD37 -- ROADMAP.md. A play right after a scrub, and again right
        // after a stop, starts at once: the sound output stays open between
        // them. Closed at each stop and opened again at the next play, it
        // held the window 130 to 200 ms while the playback clock ran -- the
        // first frames skipped, the sound that late behind the picture (QA
        // of 3 October 2026).
        if (self.state->audioPlayback) {
            [self requestTimelinePosition:{9, 25}];
            [self requestTimelinePosition:{10, 25}];
            const auto timed = [&](int direction) {
                const auto began = std::chrono::steady_clock::now();
                [self setPlaybackDirection:direction];
                return std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - began)
                    .count();
            };
            const uint64_t opened =
                self.state->audioPlayback->OutputStartCount();
            const double first = timed(1);
            timed(0);
            const double again = timed(1);
            timed(0);
            const uint64_t reopened =
                self.state->audioPlayback->OutputStartCount() - opened;
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          " (%.1f ms, then %.1f ms right after a stop; the "
                          "output opened %llu more times)",
                          first, again,
                          static_cast<unsigned long long>(reopened));
            UiSmokeCheck(first < 40.0 && again < 40.0 && reopened == 0 &&
                             self.state->audioPlayback->OutputOpen(),
                         (std::string("PRD37: a play starts at once after a "
                                      "scrub and after a stop") +
                          detail)
                             .c_str());
        }

        // D2 -- docs/roadmap/DIALOGUE-2026-10.md. A marker dropped by its
        // key, drawn where the pointer hits it, reached by navigation and by
        // a click on its flag, then removed.
        self.shortcutBindings[@"marker.add"] = @"M";
        [self requestTimelinePosition:nextCut];
        const size_t markersBefore =
            self.state->document.sequence.markers.size();
        const size_t editsBeforeMarker = self.state->editLog.AppliedCount();
        SendKeyThroughWindow(self.metalView, @"m", 46);
        const bool markerDropped =
            self.state->document.sequence.markers.size() == markersBefore + 1 &&
            self.state->document.sequence.markers.back().time == nextCut &&
            self.state->editLog.AppliedCount() == editsBeforeMarker + 1;
        UiSmokeCheck(markerDropped,
                     "M drops a marker at the playhead through EditLog");
        if (markerDropped) {
            const Ulid markerId =
                self.state->document.sequence.markers.back().id;
            const double timelineWidth = self.metalView.bounds.size.width;
            const auto flags = VisibleTimelineMarkers(
                self.state->document, self.state->viewport, timelineWidth);
            const auto flag = std::find_if(flags.begin(), flags.end(),
                                           [&](const TimelineMarkerFlag& it) {
                                               return it.marker_id == markerId;
                                           });
            const TimelineRenderData markerData = [self timelineRenderData];
            const double markerTop = [self videoHeight];
            const auto drawn = [&](double x, double y, double w, double h) {
                return std::any_of(
                    markerData.overlays.begin(), markerData.overlays.end(),
                    [&](const MetalDrawCommand& overlay) {
                        const MetalRect* rect =
                            std::get_if<MetalRect>(&overlay);
                        return rect && std::abs(rect->x - x) < 0.01 &&
                               std::abs(rect->y - (markerTop + y)) < 0.01 &&
                               std::abs(rect->width - w) < 0.01 &&
                               std::abs(rect->height - h) < 0.01;
                    });
            };
            bool flagDrawn = flag != flags.end();
            if (flagDrawn)
                for (const TimelineMarkerFlagRow& row :
                     TimelineMarkerFlagShape(*flag))
                    flagDrawn =
                        flagDrawn && drawn(row.x, row.y, row.width, row.height);
            // Visual QA of 1 October 2026 -- and no line from it down
            // through the tracks, which read as a second playhead.
            const bool noStem =
                flag != flags.end() &&
                std::none_of(
                    markerData.overlays.begin(), markerData.overlays.end(),
                    [&](const MetalDrawCommand& overlay) {
                        const MetalRect* rect =
                            std::get_if<MetalRect>(&overlay);
                        return rect && std::abs(rect->x - flag->x) < 0.01 &&
                               std::abs(rect->width - 1.0) < 0.01 &&
                               std::abs(rect->y - (markerTop + flag->flag_y +
                                                   kTimelineMarkerFlagHeight)) <
                                   0.01;
                    });
            UiSmokeCheck(
                noStem && self.state->document.sequence.markers.back().color ==
                              kTimelineMarkerEditorColor,
                "A marker dropped in the window is a blue tag on the "
                "ruler, with no line through the tracks");
            UiSmokeCheck(flagDrawn,
                         "A marker's flag is drawn where the pointer hits it");
            const bool noteDrawn = std::any_of(
                markerData.overlays.begin(), markerData.overlays.end(),
                [&](const MetalDrawCommand& overlay) {
                    const MetalText* text = std::get_if<MetalText>(&overlay);
                    return text && text->text == kTimelineMarkerDefaultName;
                });
            UiSmokeCheck(noteDrawn, "The paused playhead on a marker shows "
                                    "its note");
            [self requestTimelinePosition:{0, 25}];
            [self menuNextMarker:nil];
            UiSmokeCheck(self.state->requestedPosition == nextCut,
                         "Next marker sends the playhead to the marker");
            [self requestTimelinePosition:{0, 25}];
            if (flag != flags.end()) {
                const NSPoint onFlag = NSMakePoint(
                    flag->x,
                    self.metalView.bounds.size.height -
                        (flag->flag_y + kTimelineMarkerFlagHeight / 2));
                SendTimelineMouseGesture(self.metalView, onFlag, onFlag);
                UiSmokeCheck(self.state->requestedPosition == nextCut,
                             "A click on a marker's flag sends the playhead "
                             "to it");
            }
            // D6 -- Option-drag stretches the marker into a span: one edit,
            // drawn as a body running on to where it ends, and a single
            // Undo makes it a point again.
            size_t stretchEdits = 0;
            if (flag != flags.end()) {
                const double flagViewY =
                    self.metalView.bounds.size.height -
                    (flag->flag_y + kTimelineMarkerFlagHeight / 2);
                const double pulled = flag->x + 80.0;
                SendTimelineMouseGesture(
                    self.metalView, NSMakePoint(flag->x, flagViewY),
                    NSMakePoint(pulled, flagViewY), NSEventModifierFlagOption);
                const DocumentMarker* stretched =
                    self.state->document.FindMarker(markerId);
                const bool isSpan = stretched && stretched->time == nextCut &&
                                    stretched->duration.value > 0 &&
                                    self.state->editLog.AppliedCount() ==
                                        editsBeforeMarker + 2 &&
                                    !self.state->markerStretch;
                UiSmokeCheck(isSpan, "Option-drag on a marker stretches it "
                                     "into a span, in one edit");
                if (isSpan) {
                    const double endX = self.state->viewport.TimeToX(
                        stretched->time.add(stretched->duration));
                    const TimelineRenderData spanData =
                        [self timelineRenderData];
                    const bool bodyDrawn = std::any_of(
                        spanData.overlays.begin(), spanData.overlays.end(),
                        [&](const MetalDrawCommand& overlay) {
                            const MetalRect* rect =
                                std::get_if<MetalRect>(&overlay);
                            return rect &&
                                   std::abs(rect->x - flag->flag_x) < 0.01 &&
                                   std::abs(rect->x + rect->width - endX) <
                                       0.01 &&
                                   std::abs(rect->y -
                                            (markerTop + flag->flag_y)) < 0.01;
                        });
                    // The pointer landed within a frame of where it pulled.
                    UiSmokeCheck(
                        bodyDrawn &&
                            std::abs(endX - pulled) <=
                                self.state->viewport.pixels_per_second / 25.0,
                        "The span is drawn from the marker's time to where "
                        "the pointer let go");
                    [self requestTimelinePosition:nextCut];
                    [self menuUndo:nil];
                    const DocumentMarker* undone =
                        self.state->document.FindMarker(markerId);
                    UiSmokeCheck(undone && undone->duration.value == 0,
                                 "One Undo makes the span a point again");
                    [self menuRedo:nil];
                    stretchEdits = 1;
                }
            }
            [self requestTimelinePosition:nextCut];
            [self menuRemoveMarker:nil];
            UiSmokeCheck(
                self.state->document.sequence.markers.size() == markersBefore &&
                    self.state->editLog.AppliedCount() ==
                        editsBeforeMarker + 2 + stretchEdits,
                "Removing the marker under the playhead is one more "
                "edit");
        }
        [self requestTimelinePosition:{0, 25}];

        const auto clickTrackControl = [&](size_t trackIndex, double x) {
            const double timelineY =
                kTimelineRulerHeight +
                trackIndex * self.state->viewport.track_height +
                self.state->viewport.track_height * 0.5;
            const NSPoint point =
                NSMakePoint(x, self.metalView.bounds.size.height - timelineY);
            SendTimelineMouseGesture(self.metalView, point, point);
        };
        const DocumentTrack* videoTrack =
            self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(videoTrack && videoTrack->visible,
                     "Video tracks start visible");
        clickTrackControl(videoDisplayIndex, 48.0);
        videoTrack = self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(videoTrack && !videoTrack->visible,
                     "Video eye hides the track output");
        clickTrackControl(videoDisplayIndex, 48.0);
        clickTrackControl(videoDisplayIndex, 66.0);
        videoTrack = self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(videoTrack && videoTrack->visible && videoTrack->locked,
                     "Video lock rejects edits independently of visibility");
        const TimelineRenderData lockedData = [self timelineRenderData];
        const bool hasLockHatch = std::any_of(
            lockedData.overlays.begin(), lockedData.overlays.end(),
            [&](const MetalDrawCommand& overlay) {
                const MetalRect* rect = std::get_if<MetalRect>(&overlay);
                return rect && std::abs(rect->height - 1.5) < 0.001 &&
                       std::abs(rect->alpha - 0.42f) < 0.001f &&
                       rect->x >= self.state->viewport.header_width;
            });
        UiSmokeCheck(hasLockHatch,
                     "Locked video track draws a hatch above its clips");
        clickTrackControl(videoDisplayIndex, 66.0);

        if (initialAudioTrackId) {
            const DocumentTrack* audioTrack =
                self.state->document.FindTrack(*initialAudioTrackId);
            UiSmokeCheck(audioTrack && !audioTrack->solo && !audioTrack->muted,
                         "Audio tracks start with Solo and Mute disabled");
            clickTrackControl(audioDisplayIndex, 48.0);
            clickTrackControl(audioDisplayIndex, 66.0);
            clickTrackControl(audioDisplayIndex, 84.0);
            audioTrack = self.state->document.FindTrack(*initialAudioTrackId);
            UiSmokeCheck(audioTrack && audioTrack->solo && audioTrack->muted &&
                             audioTrack->locked,
                         "Audio S, M and lock controls persist their state");
            clickTrackControl(audioDisplayIndex, 48.0);
            clickTrackControl(audioDisplayIndex, 66.0);
            clickTrackControl(audioDisplayIndex, 84.0);
        }

        self.state->timelineIn = RationalTime{10, 25};
        self.state->timelineOut = RationalTime{30, 25};
        const TimelineRenderData rangeData = [self timelineRenderData];
        size_t clipCommand = rangeData.overlays.size();
        size_t rangeCommand = rangeData.overlays.size();
        for (size_t index = 0; index < rangeData.overlays.size(); ++index) {
            const MetalRect* rect =
                std::get_if<MetalRect>(&rangeData.overlays[index]);
            if (!rect) continue;
            if (clipCommand == rangeData.overlays.size() &&
                rect->height == 40.0 &&
                rect->x >= self.state->viewport.header_width)
                clipCommand = index;
            if (std::abs(rect->alpha - 0.10f) < 0.001f) rangeCommand = index;
        }
        UiSmokeCheck(clipCommand < rangeCommand &&
                         rangeCommand < rangeData.overlays.size(),
                     "In/Out overlay is drawn above timeline clips");
        // D5 -- docs/roadmap/DIALOGUE-2026-10.md. The same two marks are
        // what an agent reads with get_editor_view.
        {
            EditorView marked;
            std::string viewMessage;
            UiSmokeCheck(
                self.chatBackend &&
                    self.chatBackend->ReadEditorView(marked, viewMessage) &&
                    marked.mark_in && marked.mark_out &&
                    *marked.mark_in == RationalTime(10, 25) &&
                    *marked.mark_out == RationalTime(30, 25),
                "D5: the editor view carries the window's In and Out");
        }
        self.state->timelineIn.reset();
        self.state->timelineOut.reset();

        self.state->interaction->SelectClip(*initialClipId);
        [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
        [self updateSelectionInfo];
        NSMutableArray<NSSlider*>* sliders = [NSMutableArray array];
        CollectSliders(self.inspectorView, sliders);
        // The framing sliders and the carton's height are CMTrackedSliders,
        // which CollectSliders leaves out: the opacity, the blur (EF3) and
        // the grade remain. The blur's arrival had left this count one short
        // since 30 October 2026.
        UiSmokeCheck(
            sliders.count == ui::inspector::GradeControls().size() + 2,
            "Inspector exposes opacity, blur and every grading slider");
        NSSlider* opacitySlider = nil;
        NSSlider* exposure = nil;
        const ui::inspector::GradeControlSpec& exposureSpec =
            ui::inspector::GradeControls().front();
        for (NSSlider* slider in sliders) {
            if (std::abs(slider.minValue) < 0.001 &&
                std::abs(slider.maxValue - 1.0) < 0.001)
                opacitySlider = slider;
            if (std::abs(slider.minValue - exposureSpec.min_value) < 0.001 &&
                std::abs(slider.maxValue - exposureSpec.max_value) < 0.001)
                exposure = slider;
        }
        if (opacitySlider) {
            opacitySlider.floatValue = 0.42f;
            [opacitySlider sendAction:opacitySlider.action
                                   to:opacitySlider.target];
            // ON4 -- a concurrent build can occupy the display callback
            // past the debounce. Await the committed state, within a bound,
            // instead of assuming one fixed run-loop turn fired the timer.
            NSDate* opacityDeadline = [NSDate dateWithTimeIntervalSinceNow:1.0];
            while (opacityDeadline.timeIntervalSinceNow > 0.0) {
                const DocumentClip* changed =
                    self.state->document.FindClip(*initialClipId);
                if (changed && changed->opacity.num == 420 &&
                    changed->opacity.den == 1000) {
                    break;
                }
                [NSRunLoop.currentRunLoop
                       runMode:NSDefaultRunLoopMode
                    beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
            const DocumentClip* composited =
                self.state->document.FindClip(*initialClipId);
            UiSmokeCheck(composited && composited->opacity.num == 420 &&
                             composited->opacity.den == 1000,
                         "Inspector opacity commits through EditLog");
            // R2c -- docs/roadmap/REEL-2026-09.md. The opacity's diamond, as
            // in Resolve: a first key, a second where the slider is let go,
            // the arrow back to the first, and both removed -- the last one
            // holding 0.42, the clip is left as it was found.
            const DocumentClip* keyedClip = composited;
            const MediaRate keyRate = self.state->document.sequence.frame_rate;
            int64_t keyFrames = 0;
            std::string keyError;
            if (keyedClip &&
                ClipFrameCount(*keyedClip, keyRate, keyFrames, keyError) &&
                keyFrames > 6 && keyRate.num > 0) {
                const RationalTime head = keyedClip->timeline_in;
                const auto at = [&](int64_t frame) {
                    return head.add(
                        RationalTime(frame * keyRate.den, keyRate.num));
                };
                const auto opacityKeys = [&]() -> std::vector<ClipKeyframe> {
                    const DocumentClip* clip =
                        self.state->document.FindClip(*initialClipId);
                    const ClipParameterAnimation* entry =
                        clip ? FindClipAnimation(*clip,
                                                 ClipAnimatedParameter::Opacity)
                             : nullptr;
                    return entry ? entry->keyframes
                                 : std::vector<ClipKeyframe>{};
                };
                [self requestTimelinePosition:at(2)];
                NSButton* diamond =
                    FindButtonLabelled(self.inspectorView, @"Opacité : Poser");
                UiSmokeCheck(diamond != nil && diamond.enabled,
                             "R2c: the opacity row carries a diamond, free on "
                             "the clip's frames");
                [diamond performClick:nil];
                std::vector<ClipKeyframe> keys = opacityKeys();
                UiSmokeCheck(keys.size() == 1 && keys[0].value.num == 420 &&
                                 keys[0].value.den == 1000 &&
                                 ClipKeyframeFrame(keys[0], keyRate) == 2 &&
                                 [diamond.title isEqualToString:@"◆"],
                             "R2c: the diamond poses one key on the playhead's "
                             "frame, holding the opacity shown, and lights");
                [self requestTimelinePosition:at(6)];
                NSButton* previous = FindButtonLabelled(
                    self.inspectorView, @"Opacité : clé précédente");
                UiSmokeCheck([diamond.title isEqualToString:@"◇"] &&
                                 previous != nil && !previous.hidden &&
                                 previous.enabled,
                             "R2c: four frames on, the diamond is unlit and "
                             "the arrow back is offered");
                opacitySlider.floatValue = 0.2f;
                [opacitySlider sendAction:opacitySlider.action
                                       to:opacitySlider.target];
                NSDate* keyDeadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
                while (opacityKeys().size() < 2 &&
                       keyDeadline.timeIntervalSinceNow > 0.0)
                    [NSRunLoop.currentRunLoop
                           runMode:NSDefaultRunLoopMode
                        beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
                keys = opacityKeys();
                const DocumentClip* field =
                    self.state->document.FindClip(*initialClipId);
                UiSmokeCheck(keys.size() == 2 &&
                                 ClipKeyframeFrame(keys[1], keyRate) == 6 &&
                                 keys[1].value.num == 200 && field &&
                                 field->opacity.num == 420,
                             "R2c: the keyed slider keys the frame it is let "
                             "go on, the opacity field untouched");
                [previous performClick:nil];
                UiSmokeCheck(self.state->requestedPosition == at(2) &&
                                 [diamond.title isEqualToString:@"◆"],
                             "R2c: the arrow takes the playhead back to the "
                             "first key");
                // The second key first: the last one removed puts its value
                // in the field, and 0.42 is the field's already.
                [self requestTimelinePosition:at(6)];
                [diamond performClick:nil];
                [self requestTimelinePosition:at(2)];
                [diamond performClick:nil];
                field = self.state->document.FindClip(*initialClipId);
                UiSmokeCheck(field && field->animation.empty() &&
                                 field->opacity.num == 420 &&
                                 field->opacity.den == 1000,
                             "R2c: both keys removed, the clip is as it was");
            } else {
                UiSmokeCheck(false, "R2c: the smoke clip is long enough to "
                                    "key two frames");
            }
        } else {
            UiSmokeCheck(false, "Inspector exposes its opacity slider");
        }
        if (exposure) {
            exposure.floatValue = 0.5f;
            [exposure sendAction:exposure.action to:exposure.target];
            const auto exposureValue = [&] {
                const DocumentClip* clip =
                    self.state->document.FindClip(*initialClipId);
                return clip ? ui::inspector::CurrentGradeControlValue(
                                  clip->effects,
                                  ui::inspector::GradeControls().front())
                            : 0.0f;
            };
            // PRD28 -- the commit waits out the Inspector's debounce (0.12 s)
            // and lands on a later turn of the run loop. Unlike the opacity
            // above, this slider first previews the grade, and the frame the
            // preview asks for is drawn on one of those turns: with another
            // application in front, the commit landed after 1.06 s (measured
            // on 3 October 2026) -- about the second Core Animation may wait
            // for a drawable of a window it does not composite. The fixed
            // 0.2 s wait made this check, the cut below and the grade stages
            // after it fail whenever the Mac was in use, as if they needed
            // the key window. They need nothing: wait for the commit itself,
            // bounded, and no less than before for what follows.
            NSDate* settled = [NSDate dateWithTimeIntervalSinceNow:0.2];
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5.0];
            while ((settled.timeIntervalSinceNow > 0.0 ||
                    std::abs(exposureValue() - 0.5f) >= 0.001f) &&
                   deadline.timeIntervalSinceNow > 0.0)
                [NSRunLoop.currentRunLoop
                       runMode:NSDefaultRunLoopMode
                    beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
            const DocumentClip* graded =
                self.state->document.FindClip(*initialClipId);
            const float value = exposureValue();
            UiSmokeCheck(std::abs(value - 0.5f) < 0.001f,
                         "Inspector slider commits through EditLog");
            captureQaState("10-inspector-video");
            if (graded && initialTrackId && graded->duration.value > 1) {
                const RationalTime cut = graded->timeline_in.add(
                    {graded->duration.value / 2, graded->duration.rate});
                const DocumentTrack* beforeTrack =
                    self.state->document.FindTrack(*initialTrackId);
                const size_t beforeCount =
                    beforeTrack ? beforeTrack->clips.size() : 0;
                // B, the blade's default key since it follows Resolve's.
                SendKeyThroughApplication(exposure, @"b", 11);
                UiSmokeCheck([self effectiveTool] == TimelineTool::Cut,
                             "Inspector focus still accepts the Cut shortcut");
                const double timelineY = kTimelineRulerHeight +
                                         (videoDisplayIndex + 0.5) *
                                             self.state->viewport.track_height;
                const NSPoint cutPoint =
                    NSMakePoint(self.state->viewport.TimeToX(cut),
                                self.metalView.bounds.size.height - timelineY);
                SendTimelineMouseGesture(self.metalView, cutPoint, cutPoint);
                const DocumentTrack* afterTrack =
                    self.state->document.FindTrack(*initialTrackId);
                const DocumentClip* right = nullptr;
                if (afterTrack) {
                    for (const DocumentClip& candidate : afterTrack->clips) {
                        if (candidate.timeline_in == cut) {
                            right = &candidate;
                            break;
                        }
                    }
                }
                UiSmokeCheck(
                    afterTrack && afterTrack->clips.size() > beforeCount &&
                        right && right->effects.size() == 1 &&
                        right->opacity.num == 420 &&
                        right->opacity.den == 1000 &&
                        std::abs(ui::inspector::CurrentGradeControlValue(
                                     right->effects,
                                     ui::inspector::GradeControls().front()) -
                                 0.5f) < 0.001f,
                    "Cutting from Inspector focus preserves grading");
                [self setTimelineTool:TimelineTool::Select];
            }
        } else {
            UiSmokeCheck(false, "Inspector exposes its exposure slider");
        }

        // GR4, GR5 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The wheels and
        // the curve editor of the Inspector, by the events AppKit sends
        // them: every gesture is one edit, and all of them undo to the
        // document they started from.
        {
            using namespace ui::inspector;
            const Ulid gradedId = *initialClipId;
            self.state->interaction->SelectClip(gradedId);
            [self updateSelectionInfo];
            CMGradeWheelGridView* grid =
                (CMGradeWheelGridView*)FindSubviewOfClass(
                    self.inspectorView, CMGradeWheelGridView.class);
            CMGradeCurveView* curves = (CMGradeCurveView*)FindSubviewOfClass(
                self.inspectorView, CMGradeCurveView.class);
            // ON4 -- the Colour layout uses the very same controls; the
            // normal Inspector's order returns when that page is left.
            CMInspectorView* colorInspector = [[CMInspectorView alloc]
                initWithFrame:NSMakeRect(0.0, 0.0, 1000.0, 700.0)];
            [colorInspector reloadWithDocument:self.state->document
                                selectedClipId:gradedId];
            colorInspector.colorWorkspace = YES;
            NSView* colorWheels =
                FindSubviewOfClass(colorInspector, CMGradeWheelGridView.class);
            NSView* colorCurves =
                FindSubviewOfClass(colorInspector, CMGradeCurveView.class);
            UiSmokeCheck(
                colorWheels && colorCurves &&
                    NSMinX(colorCurves.frame) > NSMaxX(colorWheels.frame) &&
                    std::abs(NSMinY(colorWheels.frame) -
                             NSMinY(colorCurves.frame)) < 1.0,
                "ON4: Colour puts wheels and curves beside each other");
            colorInspector.colorWorkspace = NO;
            UiSmokeCheck(NSMinY(colorCurves.frame) > NSMaxY(colorWheels.frame),
                         "ON4: leaving Colour restores the Inspector order");
            colorInspector.colorWorkspace = YES;
            colorInspector.frame = NSMakeRect(0.0, 0.0, 300.0, 700.0);
            UiSmokeCheck(
                NSMinY(colorCurves.frame) > NSMaxY(colorWheels.frame) &&
                    colorCurves.frame.size.width > 250.0,
                "ON4: a narrow Colour panel stacks usable editors");

            // DEC42 -- the histogram actually paints beneath the curve,
            // and clearing it removes the preceding picture's bins.
            CMGradeCurveView* histogramView = [[CMGradeCurveView alloc]
                initWithFrame:NSMakeRect(0.0, 0.0, 300.0, 260.0)];
            const auto histogramGraph =
                ui::inspector::LayoutGradeCurve(300.0).graph;
            const NSRect histogramRect =
                NSMakeRect(histogramGraph.x, histogramGraph.y,
                           histogramGraph.width, histogramGraph.height);
            const auto histogramPixels = [&] {
                NSBitmapImageRep* bitmap = [histogramView
                    bitmapImageRepForCachingDisplayInRect:histogramRect];
                [histogramView cacheDisplayInRect:histogramRect
                                 toBitmapImageRep:bitmap];
                // Compare the graph's pixels, not TIFF metadata or native
                // buttons whose first capture can update their appearance.
                return [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}];
            };
            NSData* beforeHistogram = histogramPixels();
            VideoHistogram distribution{};
            distribution[0][64] = 100;
            [histogramView showHistogram:distribution];
            NSData* withHistogram = histogramPixels();
            std::string histogramCaptureError;
            UiSmokeCheck(
                WriteUiQaViewCapture(
                    histogramView, CMThemeColor(ui::theme::kSurfacePanel),
                    "12a-curve-histogram", histogramCaptureError),
                ("DEC42: histogram capture: " + histogramCaptureError).c_str());
            [histogramView showHistogram:VideoHistogram{}];
            NSData* clearedHistogram = histogramPixels();
            UiSmokeCheck(beforeHistogram && withHistogram &&
                             ![beforeHistogram isEqualToData:withHistogram] &&
                             [beforeHistogram isEqualToData:clearedHistogram],
                         "DEC42: histogram bins paint the graph and clear");
            UiSmokeCheck(
                grid != nil && grid.wheelViews.count == 4 && curves != nil,
                "GR4/GR5: the Inspector holds four wheels and a "
                "curve editor");
            expectNamedButtons("with a picture's grade in the Inspector");
            const std::string beforeGrade = self.state->document.SaveToString();
            const size_t beforeGradeEdits = self.state->editLog.AppliedCount();
            const auto stack = [&]() -> std::vector<ClipEffect> {
                const DocumentClip* clip =
                    self.state->document.FindClip(gradedId);
                return clip != nullptr ? clip->effects
                                       : std::vector<ClipEffect>{};
            };
            const auto edits = [&] {
                return self.state->editLog.AppliedCount() - beforeGradeEdits;
            };
            const auto types = [&] {
                std::string joined;
                for (const ClipEffect& effect : stack())
                    joined += effect.type + " ";
                return joined;
            };
            if (grid != nil && grid.wheelViews.count == 4) {
                CMGradeWheelView* liftView = grid.wheelViews[0];
                CMGradeWheelView* gainView = grid.wheelViews[2];
                const GradeWheelSpec& lift = GradeWheels()[0];
                const GradeWheelSpec& gain = GradeWheels()[2];
                [grid scrollRectToVisible:grid.bounds];
                const GradeWheelLayout layout =
                    LayoutGradeWheel(liftView.bounds.size.width);
                const double travel = WheelTravelRadius(layout.ring);
                const std::array<double, 2> centre =
                    WheelPointInRing(layout.ring, 0.0, 0.0);
                const NSPoint middle = NSMakePoint(centre[0], centre[1]);

                // A drag in the ring: the puck follows, geared down.
                SendGradeGesture(liftView, middle,
                                 NSMakePoint(middle.x + 40.0, middle.y - 20.0),
                                 0);
                GradeWheelState state = ReadGradeWheel(stack(), lift);
                UiSmokeCheck(
                    edits() == 1 &&
                        std::abs(state.x - WheelDragDelta(40.0, travel)) <
                            0.02 &&
                        std::abs(state.y - WheelDragDelta(20.0, travel)) <
                            0.02 &&
                        state.knobs[3] == 0.0 &&
                        types() == "color.exposure color.wheels ",
                    ("GR4: a drag in the lift ring moves its puck by a "
                     "quarter of the pointer's travel, in one edit, the "
                     "stage after the exposure: " +
                     types())
                        .c_str());
                captureQaState("11-inspector-wheels");

                // With Shift, the puck is under the pointer.
                const std::array<double, 2> target =
                    WheelPointInRing(layout.ring, -0.5, 0.25);
                SendGradeGesture(liftView, NSMakePoint(target[0], target[1]),
                                 NSMakePoint(target[0], target[1]),
                                 NSEventModifierFlagShift);
                state = ReadGradeWheel(stack(), lift);
                UiSmokeCheck(edits() == 2 && std::abs(state.x + 0.5) < 0.02 &&
                                 std::abs(state.y - 0.25) < 0.02,
                             "GR4: a Shift click puts the puck under the "
                             "pointer");

                // With Command, and on the bar, the master.
                SendGradeGesture(liftView, middle,
                                 NSMakePoint(middle.x + 30.0, middle.y),
                                 NSEventModifierFlagCommand);
                const double commanded = ReadGradeWheel(stack(), lift).knobs[3];
                const NSPoint bar =
                    NSMakePoint(layout.master.x + layout.master.width / 2.0,
                                layout.master.y + layout.master.height / 2.0);
                SendGradeGesture(liftView, bar,
                                 NSMakePoint(bar.x - 10.0, bar.y), 0);
                state = ReadGradeWheel(stack(), lift);
                UiSmokeCheck(
                    edits() == 4 &&
                        std::abs(commanded - 30.0 * lift.master_step) < 1e-9 &&
                        std::abs(state.knobs[3] - 20.0 * lift.master_step) <
                            1e-9 &&
                        std::abs(state.x + 0.5) < 0.02,
                    "GR4: a Command drag in the ring and a drag on the bar "
                    "move the master and leave the colour");

                // A double click resets the colour, not the master.
                SendGradeGesture(liftView, middle, middle, 0, 2);
                state = ReadGradeWheel(stack(), lift);
                UiSmokeCheck(edits() == 5 && state.reach == 0.0 &&
                                 std::abs(state.knobs[3] -
                                          20.0 * lift.master_step) < 1e-9,
                             "GR4: a double click in the ring resets the "
                             "colour and keeps the master");

                // A field: the gain reads as a factor.
                NSTextField* gainMaster = gainView.fields[3];
                gainMaster.stringValue = @"1,50";
                [gainMaster sendAction:gainMaster.action to:gainMaster.target];
                UiSmokeCheck(
                    edits() == 6 &&
                        std::abs(ReadGradeWheel(stack(), gain).knobs[3] -
                                 0.585) < 1e-9 &&
                        [gainMaster.stringValue isEqualToString:@"1.50"],
                    "GR4: a gain typed as a factor, decimal comma included, "
                    "is committed as its power of two");
                gainMaster.stringValue = @"zéro";
                [gainMaster sendAction:gainMaster.action to:gainMaster.target];
                UiSmokeCheck(
                    edits() == 6 &&
                        [gainMaster.stringValue isEqualToString:@"1.50"],
                    "GR4: a field that holds no number commits nothing and "
                    "goes back to its knob");

                // The reset control: the wheel, and only that wheel.
                [liftView.resetButton sendAction:liftView.resetButton.action
                                              to:liftView.resetButton.target];
                UiSmokeCheck(
                    edits() == 7 &&
                        ReadGradeWheel(stack(), lift).knobs ==
                            std::array<double, 4>{} &&
                        std::abs(ReadGradeWheel(stack(), gain).knobs[3] -
                                 0.585) < 1e-9,
                    "GR4: the reset control resets its wheel and leaves the "
                    "others");
                // A click that moves nothing is no edit.
                SendGradeGesture(liftView, middle, middle, 0);
                UiSmokeCheck(edits() == 7,
                             "GR4: a click in the ring that moves nothing "
                             "commits nothing");
            }
            if (curves != nil) {
                [curves scrollRectToVisible:curves.bounds];
                const size_t wheelEdits = edits();
                const GradeRect graph =
                    LayoutGradeCurve(curves.bounds.size.width).graph;
                const auto at = [&](double x, double y) {
                    const std::array<double, 2> point =
                        CurvePointInGraph(graph, x, y);
                    return NSMakePoint(point[0], point[1]);
                };
                const auto points = [&](const char* channel) {
                    return ShownCurvePoints(stack(), channel);
                };
                // A click adds a point where it lands; dragged, it moves.
                SendGradeGesture(curves, at(0.5, 0.5), at(0.5, 0.62), 0);
                std::vector<ColorCurvePoint> master = points("master");
                UiSmokeCheck(
                    edits() == wheelEdits + 1 && master.size() == 3 &&
                        std::abs(CurveCoordinate(master[1].x) - 0.5) < 0.005 &&
                        std::abs(CurveCoordinate(master[1].y) - 0.62) < 0.005 &&
                        types() == "color.exposure color.wheels color.curves ",
                    ("GR5: a click on the graph adds a point and a drag "
                     "moves it, in one edit, the stage after the wheels: " +
                     types())
                        .c_str());
                // With Shift, on the curve; with Option, onto the diagonal.
                const double onCurve = CurveValueAt(stack(), "master", 0.25);
                SendGradeGesture(curves, at(0.25, 0.9), at(0.25, 0.9),
                                 NSEventModifierFlagShift);
                master = points("master");
                UiSmokeCheck(edits() == wheelEdits + 2 && master.size() == 4 &&
                                 std::abs(CurveCoordinate(master[1].y) -
                                          onCurve) < 0.002,
                             "GR5: a Shift click adds its point on the curve");
                SendGradeGesture(curves, at(0.5, 0.62), at(0.6, 0.9),
                                 NSEventModifierFlagOption);
                master = points("master");
                UiSmokeCheck(
                    edits() == wheelEdits + 3 && master.size() == 4 &&
                        std::abs(CurveCoordinate(master[2].x) - 0.6) < 0.005 &&
                        master[2].x.num * master[2].y.den ==
                            master[2].y.num * master[2].x.den,
                    "GR5: an Option drag snaps the point to the diagonal");
                captureQaState("12-inspector-curves");
                // A right click removes the point; an end stays.
                const auto rightClick = [&](NSPoint point) {
                    [curves
                        rightMouseDown:
                            [NSEvent
                                mouseEventWithType:NSEventTypeRightMouseDown
                                          location:[curves convertPoint:point
                                                                 toView:nil]
                                     modifierFlags:0
                                         timestamp:NSProcessInfo.processInfo
                                                       .systemUptime
                                      windowNumber:self.window.windowNumber
                                           context:nil
                                       eventNumber:0
                                        clickCount:1
                                          pressure:1.0]];
                };
                rightClick(at(0.6, 0.6));
                rightClick(at(1.0, 1.0));
                UiSmokeCheck(edits() == wheelEdits + 4 &&
                                 points("master").size() == 3 &&
                                 curves.notice.length > 0,
                             "GR5: a right click removes a point; on an end "
                             "it is refused, and says why");
                // Another channel is edited alone.
                [curves selectChannel:"r"];
                SendGradeGesture(curves, at(0.75, 0.7), at(0.75, 0.7), 0);
                UiSmokeCheck(
                    edits() == wheelEdits + 5 && points("r").size() == 3 &&
                        points("master").size() == 3 && points("g").size() == 2,
                    "GR5: the red channel takes its own point and "
                    "leaves the others");
                [curves selectChannel:"master"];
            }
            // GR8 -- the hue curves.
            CMGradeHueCurveView* hues =
                (CMGradeHueCurveView*)FindSubviewOfClass(
                    self.inspectorView, CMGradeHueCurveView.class);
            UiSmokeCheck(hues != nil,
                         "GR8: the Inspector holds a hue curve editor");
            if (hues != nil) {
                [hues scrollRectToVisible:hues.bounds];
                const size_t before = edits();
                const GradeHueCurveLayout layout =
                    LayoutGradeHueCurve(hues.bounds.size.width);
                const auto at = [&](double x, double y) {
                    const std::array<double, 2> point =
                        CurvePointInGraph(layout.graph, x, y);
                    return NSMakePoint(point[0], point[1]);
                };
                const auto points = [&](const char* curve) {
                    return HueCurvePoints(stack(), curve);
                };
                // A click on the flat curve: its point between two anchors,
                // and dragged, it moves.
                SendGradeGesture(hues, at(0.3, 0.5), at(0.3, 0.2), 0);
                std::vector<ColorCurvePoint> sat = points("hue_sat");
                UiSmokeCheck(
                    edits() == before + 1 && hues.curve == "hue_sat" &&
                        sat.size() == 3 &&
                        std::abs(CurveCoordinate(sat[1].x) - 0.3) < 0.005 &&
                        std::abs(CurveCoordinate(sat[1].y) - 0.2) < 0.01 &&
                        sat[0].y.num * 2 == sat[0].y.den &&
                        sat[2].y.num * 2 == sat[2].y.den &&
                        types() == "color.exposure color.wheels "
                                   "color.curves color.hue_curves ",
                    ("GR8: a click on the hue against saturation curve "
                     "places its point between two anchors at neutral, in "
                     "one edit, the stage last: " +
                     types())
                        .c_str());
                // With Option, back to neutral.
                SendGradeGesture(hues, at(0.3, 0.2), at(0.3, 0.3),
                                 NSEventModifierFlagOption);
                sat = points("hue_sat");
                UiSmokeCheck(edits() == before + 2 && sat.size() == 3 &&
                                 sat[1].y.num * 2 == sat[1].y.den,
                             "GR8: an Option drag snaps the point to neutral");
                SendGradeGesture(hues, at(0.3, 0.5), at(0.3, 0.15), 0);
                captureQaState("13-inspector-hue-curves");
                // Another curve; a colour patch places three points on it.
                [hues selectCurve:"hue_hue"];
                const NSPoint patch = NSMakePoint(
                    layout.patches[4].x + layout.patches[4].width / 2.0,
                    layout.patches[4].y + layout.patches[4].height / 2.0);
                SendGradeGesture(hues, patch, patch, 0);
                // Blue stands just short of the seam: its upper anchor
                // comes back in at the other end, so blue is the last point.
                const std::vector<ColorCurvePoint> turned = points("hue_hue");
                UiSmokeCheck(
                    edits() == before + 4 && turned.size() == 3 &&
                        std::abs(CurveCoordinate(turned[2].x) -
                                 HueCurveVectors()[4].hue) < 0.002 &&
                        CurveCoordinate(turned[0].x) < 0.1 &&
                        points("hue_sat").size() == 3,
                    "GR8: the blue patch places three points around blue on "
                    "the hue against hue curve, and leaves the other curve");
                // Along the luma there is no patch, and a right click
                // removes a point.
                [hues selectCurve:"lum_sat"];
                SendGradeGesture(hues, patch, patch, 0);
                SendGradeGesture(hues, at(0.2, 0.5), at(0.2, 0.1), 0);
                UiSmokeCheck(
                    edits() == before + 5 && points("lum_sat").size() == 3,
                    "GR8: along the luma a patch does nothing, and "
                    "the first click is held by the two ends");
                [hues rightMouseDown:
                          [NSEvent
                              mouseEventWithType:NSEventTypeRightMouseDown
                                        location:[hues convertPoint:at(0.2, 0.1)
                                                             toView:nil]
                                   modifierFlags:0
                                       timestamp:NSProcessInfo.processInfo
                                                     .systemUptime
                                    windowNumber:self.window.windowNumber
                                         context:nil
                                     eventNumber:0
                                      clickCount:1
                                        pressure:1.0]];
                UiSmokeCheck(
                    edits() == before + 6 && points("lum_sat").size() == 2,
                    "GR8: a right click removes the point");
                [hues selectCurve:"hue_sat"];
            }
            // On a sound clip, nothing is grabbed.
            {
                std::optional<Ulid> sound;
                for (const DocumentTrack& track :
                     self.state->document.sequence.tracks)
                    for (const DocumentClip& clip : track.clips)
                        if (track.kind == "audio" && !sound) sound = clip.id;
                if (sound && grid != nil && curves != nil) {
                    const size_t before = self.state->editLog.AppliedCount();
                    self.state->interaction->SelectClip(*sound);
                    [self updateSelectionInfo];
                    CMGradeWheelView* wheel = grid.wheelViews[0];
                    const GradeWheelLayout layout =
                        LayoutGradeWheel(wheel.bounds.size.width);
                    const std::array<double, 2> centre =
                        WheelPointInRing(layout.ring, 0.0, 0.0);
                    SendGradeGesture(wheel, NSMakePoint(centre[0], centre[1]),
                                     NSMakePoint(centre[0] + 30.0, centre[1]),
                                     0);
                    const GradeRect graph =
                        LayoutGradeCurve(curves.bounds.size.width).graph;
                    const std::array<double, 2> spot =
                        CurvePointInGraph(graph, 0.5, 0.7);
                    SendGradeGesture(curves, NSMakePoint(spot[0], spot[1]),
                                     NSMakePoint(spot[0], spot[1]), 0);
                    UiSmokeCheck(
                        self.state->editLog.AppliedCount() == before &&
                            !wheel.resetButton.enabled &&
                            !wheel.fields[0].enabled,
                        "GR4/GR5: on a sound clip the wheels and the curve "
                        "take no gesture");
                    self.state->interaction->SelectClip(gradedId);
                    [self updateSelectionInfo];
                }
            }
            // One gesture, one edit, one undo: all of them back.
            const size_t made = edits();
            for (size_t index = 0; index < made; ++index) [self menuUndo:nil];
            UiSmokeCheck(made >= 12 &&
                             self.state->document.SaveToString() == beforeGrade,
                         "GR4/GR5: undoing every wheel and curve gesture "
                         "restores the document byte for byte");

            // PRD37 -- ROADMAP.md. After its own edits, the Inspector shows
            // the stack the document holds, with no other reload: the reset
            // button, the wheels and the sliders follow one another, and the
            // refresh drops no commit still pending (QA of 3 October 2026).
            // On the picture graded above: every picture of this timeline
            // carries a grade by now, so its grade goes first, by the reset
            // button pressed as a hand presses it.
            NSButton* resetGrade = FindButtonWithTitle(
                self.inspectorView, @"Réinitialiser l'étalonnage");
            UiSmokeCheck(
                resetGrade != nil && grid != nil && grid.wheelViews.count == 4,
                "PRD37: the Inspector has its reset button and its "
                "four wheels");
            if (resetGrade != nil && grid != nil &&
                grid.wheelViews.count == 4) {
                [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
                const size_t beforeBlock = self.state->editLog.AppliedCount();
                const std::string beforeBlockDocument =
                    self.state->document.SaveToString();
                const Ulid plainId = gradedId;
                self.state->interaction->SelectClip(plainId);
                [self updateSelectionInfo];
                const auto gradeStages = [&] {
                    const DocumentClip* clip =
                        self.state->document.FindClip(plainId);
                    return clip == nullptr
                               ? size_t{0}
                               : clip->effects.size() -
                                     WithoutGrade(clip->effects).size();
                };
                const bool onWhenGraded =
                    resetGrade.enabled && gradeStages() > 0;
                [resetGrade performClick:nil];
                UiSmokeCheck(
                    onWhenGraded && gradeStages() == 0 &&
                        self.state->editLog.AppliedCount() == beforeBlock + 1,
                    "PRD37: on a graded picture « Réinitialiser "
                    "l'étalonnage » takes a click, and takes the "
                    "grade off in one edit");
                // A plain picture from here, read as a new selection reads
                // it: what follows is the panel's own edits alone.
                [self updateSelectionInfo];
                const size_t beforePlain = self.state->editLog.AppliedCount();
                NSMutableArray<NSSlider*>* all = [NSMutableArray array];
                CollectSliders(self.inspectorView, all);
                const SEL gradeAction =
                    NSSelectorFromString(@"cmGradeSliderChanged:");
                const auto sliderFor =
                    [&](const GradeControlSpec* spec) -> NSSlider* {
                    if (spec == nullptr) return nil;
                    const NSInteger index = spec - GradeControls().data();
                    for (NSSlider* candidate in all)
                        if (candidate.action == gradeAction &&
                            candidate.tag == index)
                            return candidate;
                    return nil;
                };
                const auto plainStack = [&]() -> std::vector<ClipEffect> {
                    const DocumentClip* clip =
                        self.state->document.FindClip(plainId);
                    return clip != nullptr ? clip->effects
                                           : std::vector<ClipEffect>{};
                };
                const auto settle = [] {
                    NSDate* deadline =
                        [NSDate dateWithTimeIntervalSinceNow:0.3];
                    while (deadline.timeIntervalSinceNow > 0.0)
                        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                               beforeDate:deadline];
                };
                const GradeControlSpec* contrastSpec =
                    FindGradeControlSpec("color.contrast");
                const GradeControlSpec* saturationSpec =
                    FindGradeControlSpec("color.saturation");
                const GradeControlSpec* gainMasterSpec =
                    FindGradeControlSpec("color.wheels", "gain_master");
                const GradeControlSpec* liftRedSpec =
                    FindGradeControlSpec("color.wheels", "lift_r");
                NSSlider* contrast = sliderFor(contrastSpec);
                NSSlider* saturation = sliderFor(saturationSpec);
                NSSlider* gainMasterSlider = sliderFor(gainMasterSpec);
                NSSlider* liftRed = sliderFor(liftRedSpec);
                CMGradeWheelView* liftView = grid.wheelViews[0];
                CMGradeWheelView* gainView = grid.wheelViews[2];
                const bool offWhenPlain = !resetGrade.enabled;
                if (contrast && saturation && gainMasterSlider && liftRed) {
                    // Two sliders moved within the debounce: two commits,
                    // the second still pending when the first refreshes.
                    contrast.doubleValue = 0.25;
                    [contrast sendAction:contrast.action to:contrast.target];
                    saturation.doubleValue = -0.4;
                    [saturation sendAction:saturation.action
                                        to:saturation.target];
                    settle();
                    const std::vector<ClipEffect> both = plainStack();
                    UiSmokeCheck(offWhenPlain && resetGrade.enabled,
                                 "PRD37: a slider's edit turns « Réinitialiser "
                                 "l'étalonnage » on, with no other reload");
                    UiSmokeCheck(
                        std::abs(CurrentGradeControlValue(both, *contrastSpec) -
                                 0.25f) < 1e-4f &&
                            std::abs(CurrentGradeControlValue(both,
                                                              *saturationSpec) +
                                     0.4f) < 1e-4f &&
                            self.state->editLog.AppliedCount() ==
                                beforePlain + 2,
                        "PRD37: two sliders moved within the debounce both "
                        "commit: the refresh after the first drops nothing");

                    // A value typed in a wheel's field: the wheel is
                    // redrawn, its master bar with it, and its slider moves.
                    // Each wheel's drawings are counted, after a few turns
                    // of the run loop -- where AppKit draws.
                    UiSmokeCountWheelDraws();
                    const auto drawsSettled = [&](NSView* view) {
                        [view scrollRectToVisible:view.bounds];
                        for (int turn = 0; turn < 3; ++turn) {
                            [self.window displayIfNeeded];
                            [NSRunLoop.currentRunLoop
                                runUntilDate:
                                    [NSDate dateWithTimeIntervalSinceNow:0.03]];
                        }
                        return UiSmokeWheelDrawCount(view);
                    };
                    // That the count says something: a wheel asked to draw
                    // is drawn, and one left alone is not drawn again.
                    const NSInteger beforeAsked = drawsSettled(gainView);
                    gainView.needsDisplay = YES;
                    const NSInteger asked = drawsSettled(gainView);
                    const NSInteger leftAlone = drawsSettled(gainView);
                    UiSmokeCheck(
                        asked > beforeAsked && leftAlone == asked,
                        ("PRD37: a wheel is drawn when asked to, and only "
                         "then (" +
                         std::to_string(beforeAsked) + ", " +
                         std::to_string(asked) + ", " +
                         std::to_string(leftAlone) + " drawings)")
                            .c_str());
                    const NSInteger gainBefore = leftAlone;
                    NSTextField* gainMasterField = gainView.fields[3];
                    gainMasterField.stringValue = @"1,50";
                    [gainMasterField sendAction:gainMasterField.action
                                             to:gainMasterField.target];
                    const NSInteger gainAfter = drawsSettled(gainView);
                    // 1.50 is half a stop and a bit: the master knob at
                    // log2(1.5), 0.585 at its step.
                    const double typedGain =
                        CurrentGradeControlValue(plainStack(), *gainMasterSpec);
                    UiSmokeCheck(
                        gainAfter > gainBefore &&
                            std::abs(typedGain - 0.585) < 1e-3 &&
                            std::abs(gainMasterSlider.doubleValue - typedGain) <
                                1e-6,
                        ("PRD37: a gain typed in its wheel redraws the wheel "
                         "and moves the « Gain maître » slider (drawn " +
                         std::to_string(gainAfter - gainBefore) +
                         " more times, knob at " + std::to_string(typedGain) +
                         ", slider at " +
                         std::to_string(gainMasterSlider.doubleValue) + ")")
                            .c_str());

                    // A slider moves its wheel.
                    liftRed.doubleValue = 0.1;
                    [liftRed sendAction:liftRed.action to:liftRed.target];
                    settle();
                    UiSmokeCheck(
                        [liftView.fields[0].stringValue
                            isEqualToString:@"0.10"],
                        ("PRD37: a wheel follows its per-channel slider "
                         "(field at " +
                         std::string(liftView.fields[0].stringValue.UTF8String
                                         ?: "") +
                         ")")
                            .c_str());

                    // The reset control and a double click, without a
                    // gesture after them.
                    const NSInteger liftBefore = drawsSettled(liftView);
                    [liftView.resetButton performClick:nil];
                    const NSInteger liftAfter = drawsSettled(liftView);
                    UiSmokeCheck(
                        liftAfter > liftBefore &&
                            [liftView.fields[0].stringValue
                                isEqualToString:@"0.00"] &&
                            std::abs(liftRed.doubleValue) < 1e-9,
                        ("PRD37: the reset control redraws its wheel, its "
                         "fields and its sliders at once (drawn " +
                         std::to_string(liftAfter - liftBefore) +
                         " more times)")
                            .c_str());
                    const GradeWheelLayout gainLayout =
                        LayoutGradeWheel(gainView.bounds.size.width);
                    const NSPoint gainBar = NSMakePoint(
                        gainLayout.master.x + gainLayout.master.width / 2.0,
                        gainLayout.master.y + gainLayout.master.height / 2.0);
                    const NSInteger barBefore = drawsSettled(gainView);
                    SendGradeGesture(gainView, gainBar, gainBar, 0, 2);
                    const NSInteger barAfter = drawsSettled(gainView);
                    UiSmokeCheck(
                        barAfter > barBefore &&
                            [gainMasterField.stringValue
                                isEqualToString:@"1.00"] &&
                            std::abs(gainMasterSlider.doubleValue) < 1e-9,
                        ("PRD37: a double click on the master bar redraws the "
                         "wheel, its field and its slider at once (drawn " +
                         std::to_string(barAfter - barBefore) + " more times)")
                            .c_str());
                }
                // Back to the document this block found, its reset undone
                // too.
                for (int guard = 0;
                     guard < 24 &&
                     self.state->editLog.AppliedCount() > beforeBlock;
                     ++guard)
                    [self menuUndo:nil];
                UiSmokeCheck(
                    self.state->document.SaveToString() == beforeBlockDocument,
                    "PRD37: undoing these edits gives the document back byte "
                    "for byte");
                self.state->interaction->SelectClip(gradedId);
                [self updateSelectionInfo];
            }
        }

        const DocumentTrack* multiTrack =
            self.state->document.FindTrack(*initialTrackId);
        if (multiTrack && multiTrack->clips.size() >= 2) {
            const Ulid firstSelected = multiTrack->clips[0].id;
            const Ulid secondSelected = multiTrack->clips[1].id;
            const std::string beforeDelete =
                self.state->document.SaveToString();
            const size_t beforeDeleteEdits = self.state->editLog.AppliedCount();
            self.state->interaction->SelectClips(
                {firstSelected, secondSelected});
            SendKeyThroughWindow(self.metalView, @"\x7f", 51);
            UiSmokeCheck(
                !self.state->document.FindClip(firstSelected) &&
                    !self.state->document.FindClip(secondSelected) &&
                    self.state->editLog.AppliedCount() ==
                        beforeDeleteEdits + 1 &&
                    std::holds_alternative<ClearClipsOperation>(
                        self.state->editLog.AppliedEntries().back().op),
                "Delete clears an arbitrary multi-clip selection atomically");
            [self menuUndo:nil];
            UiSmokeCheck(self.state->document.SaveToString() == beforeDelete,
                         "Undo restores a multi-clip delete byte-for-byte");
        }
    }

    const Ulid sourceId = self.state->document.sources.front().id;
    NSString* sourceIdentifier =
        [NSString stringWithUTF8String:sourceId.c_str()];
    NSString* timelineIdentifier =
        [NSString stringWithUTF8String:self.state->activeTimelineId.c_str()];
    const NSUInteger timelineIconIndex =
        [self.visibleMediaIds indexOfObject:timelineIdentifier];
    id<NSPasteboardWriting> timelineWriter =
        timelineIconIndex == NSNotFound
            ? nil
            : [self collectionView:self.mediaCollection
                  pasteboardWriterForItemAtIndexPath:
                      [NSIndexPath indexPathForItem:timelineIconIndex
                                          inSection:0]];
    UiSmokeCheck([(NSPasteboardItem*)timelineWriter
                     stringForType:kCutmachineTimelinePasteboardType] != nil,
                 "Timeline icon provides a bin-placement drag payload");

    NSString* mediaBinIdentifier = [NSString
        stringWithUTF8String:self.state->document.bins.back().id.c_str()];
    [self refreshBinControlsSelecting:mediaBinIdentifier];
    const NSUInteger iconIndex =
        [self.visibleMediaIds indexOfObject:sourceIdentifier];
    UiSmokeCheck(iconIndex == 0 && self.visibleMediaIds.count >= 7,
                 "Nested 1_RUSHES fixture exposes its first rush at index 0");
    id<NSPasteboardWriting> iconWriter =
        iconIndex == NSNotFound
            ? nil
            : [self collectionView:self.mediaCollection
                  pasteboardWriterForItemAtIndexPath:
                      [NSIndexPath indexPathForItem:iconIndex inSection:0]];
    NSSet<NSIndexPath*>* iconPaths =
        iconIndex == NSNotFound
            ? [NSSet set]
            : [NSSet setWithObject:[NSIndexPath indexPathForItem:iconIndex
                                                       inSection:0]];
    NSEvent* iconDragEvent =
        [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                           location:NSZeroPoint
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:self.window.windowNumber
                            context:nil
                        eventNumber:0
                         clickCount:1
                           pressure:1.0];
    UiSmokeCheck([(NSPasteboardItem*)iconWriter
                     stringForType:kCutmachineMediaPasteboardType] != nil,
                 "Icon-mode media item provides a timeline drag payload");
    UiSmokeCheck([self collectionView:self.mediaCollection
                     canDragItemsAtIndexPaths:iconPaths
                                    withEvent:iconDragEvent] &&
                     self.mediaCollection.gestureRecognizers.count == 0,
                 "Icon-mode drag is enabled without a competing click "
                 "recognizer");
    UiSmokeCheck((kMediaLocalDragOperations & NSDragOperationCopy) != 0 &&
                     (kMediaLocalDragOperations & NSDragOperationMove) != 0,
                 "Media drag supports timeline copy and bin move destinations");
    UiSmokeCheck([self.mediaCollection.registeredDraggedTypes
                     containsObject:kCutmachineMediaPasteboardType] &&
                     [self.mediaCollection.registeredDraggedTypes
                         containsObject:kCutmachineBinPasteboardType] &&
                     [self.mediaCollection.registeredDraggedTypes
                         containsObject:kCutmachineTimelinePasteboardType] &&
                     [self.mediaCollection.registeredDraggedTypes
                         containsObject:NSPasteboardTypeFileURL],
                 "Icon grid accepts media, timeline, bin, and Finder drops");
    if (iconIndex != NSNotFound && initialTrackId) {
        NSIndexPath* iconPath = [NSIndexPath indexPathForItem:iconIndex
                                                    inSection:0];
        [self.mediaCollection
            scrollToItemsAtIndexPaths:[NSSet setWithObject:iconPath]
                       scrollPosition:
                           NSCollectionViewScrollPositionNearestHorizontalEdge |
                           NSCollectionViewScrollPositionNearestVerticalEdge];
        [self.mediaCollection layoutSubtreeIfNeeded];
        NSCollectionViewItem* iconItem =
            [self.mediaCollection itemAtIndexPath:iconPath];
        NSCollectionViewLayoutAttributes* iconAttributes =
            [self.mediaCollection.collectionViewLayout
                layoutAttributesForItemAtIndexPath:iconPath];
        const NSRect visualIconFrame =
            iconItem ? [iconItem.view convertRect:iconItem.view.bounds
                                           toView:self.mediaCollection]
                     : NSZeroRect;
        const double timelineY =
            kTimelineRulerHeight +
            (videoDisplayIndex + 0.5) * self.state->viewport.track_height;
        const NSPoint destination =
            NSMakePoint(self.state->viewport.TimeToX({60, 25}),
                        self.metalView.bounds.size.height - timelineY);
        NSIndexPath* resolvedIconPath =
            iconItem
                ? [(ContextCollectionView*)self.mediaCollection
                      iconIndexPathAtPoint:NSMakePoint(NSMidX(visualIconFrame),
                                                       NSMidY(visualIconFrame))]
                : nil;
        UiSmokeCheck(resolvedIconPath.item == iconPath.item,
                     "Icon hit testing preserves top-to-bottom coordinates");
        bool gridSpansMultipleRows = false;
        for (NSCollectionViewItem* visibleItem in self.mediaCollection
                 .visibleItems) {
            const NSRect visibleFrame =
                [visibleItem.view convertRect:visibleItem.view.bounds
                                       toView:self.mediaCollection];
            if (std::abs(NSMidY(visibleFrame) - NSMidY(visualIconFrame)) >
                NSHeight(visualIconFrame) * 0.5) {
                gridSpansMultipleRows = true;
                break;
            }
        }
        UiSmokeCheck(gridSpansMultipleRows,
                     "Icon click fixture covers multiple visual rows");
        if (iconItem) {
            // Reproduce AppKit cell reuse after entering a bin: even if a
            // tile forwards an obsolete lower index, current visual geometry
            // must win.
            ((MediaIconView*)iconItem.view).indexPath =
                [NSIndexPath indexPathForItem:self.visibleMediaIds.count - 1
                                    inSection:0];
            self.mediaCollection.selectionIndexPaths = [NSSet set];
            // PRD28 -- a click through the window reaches a tile only when
            // the window is key: on another, AppKit spends it activating the
            // window. These clicks, the name click, the drag below and what
            // they leave behind need the key window.
            SendWindowClick(
                self.mediaCollection,
                NSMakePoint(NSMidX(visualIconFrame), NSMidY(visualIconFrame)));
            UiSmokeCheck([self.mediaCollection.selectionIndexPaths
                             containsObject:iconPath],
                         "A physical click selects the first rush tile",
                         ui_smoke::Needs::kKeyWindow);
            NSIndexPath* shiftedPath =
                [NSIndexPath indexPathForItem:iconPath.item + 1 inSection:0];
            NSCollectionViewItem* shiftedItem =
                [self.mediaCollection itemAtIndexPath:shiftedPath];
            if (shiftedItem) {
                const NSRect shiftedFrame =
                    [shiftedItem.view convertRect:shiftedItem.view.bounds
                                           toView:self.mediaCollection];
                SendWindowClick(
                    self.mediaCollection,
                    NSMakePoint(NSMidX(shiftedFrame), NSMidY(shiftedFrame)),
                    NSEventModifierFlagShift);
            }
            UiSmokeCheck(shiftedItem &&
                             [self.mediaCollection.selectionIndexPaths
                                 containsObject:iconPath] &&
                             [self.mediaCollection.selectionIndexPaths
                                 containsObject:shiftedPath],
                         "Shift-click extends the icon-mode media selection",
                         ui_smoke::Needs::kKeyWindow);
        }
        if (iconItem) {
            const NSPoint labelCenterInCollection = [iconItem.textField
                convertPoint:NSMakePoint(NSMidX(iconItem.textField.bounds),
                                         NSMidY(iconItem.textField.bounds))
                      toView:self.mediaCollection];
            NSIndexPath* labelPath =
                [(ContextCollectionView*)self.mediaCollection
                    iconIndexPathAtPoint:labelCenterInCollection];
            const NSPoint labelCenter = [iconItem.view
                convertPoint:NSMakePoint(NSMidX(iconItem.textField.bounds),
                                         NSMidY(iconItem.textField.bounds))
                    fromView:iconItem.textField];
            UiSmokeCheck(
                labelPath.item == iconPath.item &&
                    [iconItem.view hitTest:labelCenter] == iconItem.view &&
                    iconItem.textField.editable,
                "The first rush label resolves to its own icon item");
            const NSPoint labelWindowPoint =
                [self.mediaCollection convertPoint:labelCenterInCollection
                                            toView:nil];
            NSEvent* rightClick = [NSEvent
                mouseEventWithType:NSEventTypeRightMouseDown
                          location:labelWindowPoint
                     modifierFlags:0
                         timestamp:NSProcessInfo.processInfo.systemUptime
                      windowNumber:self.window.windowNumber
                           context:nil
                       eventNumber:0
                        clickCount:1
                          pressure:1.0];
            NSView* content = self.window.contentView;
            NSView* physicalHit = [content
                hitTest:[content convertPoint:labelWindowPoint fromView:nil]];
            UiSmokeCheck(
                [physicalHit isKindOfClass:MediaIconView.class] ||
                    physicalHit == self.mediaCollection,
                "Window hit-test routes the icon label to the unified tile "
                "handler");
            NSMenu* physicalMenu = [physicalHit menuForEvent:rightClick];
            UiSmokeCheck(physicalMenu == self.mediaCollection.menu,
                         "A physical right-click reaches the icon context "
                         "menu");
            UiSmokeCheck([self.mediaCollection.selectionIndexPaths
                             containsObject:iconPath],
                         "A physical right-click selects its icon item");
            self.mediaCollection.selectionIndexPaths = [NSSet set];
            SendWindowClick(self.mediaCollection, labelCenterInCollection);
            UiSmokeCheck([self.mediaCollection.selectionIndexPaths
                             containsObject:iconPath] &&
                             iconItem.textField.currentEditor != nil,
                         "A physical name click starts inline editing "
                         "immediately on the first rush",
                         ui_smoke::Needs::kKeyWindow);
            NSTextView* editor = (NSTextView*)iconItem.textField.currentEditor;
            if (editor) {
                editor.string = @"Rush renommé par clic";
                [self.window makeFirstResponder:self.mediaCollection];
            }
            const ProjectBinMetadata* clickedRename =
                self.state->project.FindBinMetadata(sourceId);
            UiSmokeCheck(clickedRename && clickedRename->display_name ==
                                              "Rush renommé par clic",
                         "Physical icon rename persists through the project "
                         "edit log",
                         ui_smoke::Needs::kKeyWindow);
            // PERF-2026-09. Reloading the decoders used to stop and reopen
            // every one of them, an avformat_open_input plus
            // avformat_find_stream_info per rush -- hundreds of milliseconds
            // each on the external drives this project is cut from, paid on
            // the main thread after every project mutation. A second reload
            // that changes no media must now open nothing at all, and keep
            // every decoded frame it had cached.
            const int opensBeforeIdleReload = gUiSmokeDecodeOpens;
            const size_t workersBeforeIdleReload = self.state->workers.size();
            [self reloadDecodeWorkers];
            UiSmokeCheck(
                workersBeforeIdleReload > 0 &&
                    self.state->workers.size() == workersBeforeIdleReload &&
                    gUiSmokeDecodeOpens == opensBeforeIdleReload,
                "Reloading decoders without a media change reopens "
                "no media");

            const int reloadsBeforeRenameHistory = gUiSmokeDecodeReloads;
            [self menuUndo:nil];
            const ProjectBinMetadata* undoneRename =
                self.state->project.FindBinMetadata(sourceId);
            // Without the rename, this undo takes back whatever edit came
            // last, and passes on it: it needs the key window too.
            UiSmokeCheck(
                (!undoneRename ||
                 undoneRename->display_name != "Rush renommé par clic") &&
                    gUiSmokeDecodeReloads == reloadsBeforeRenameHistory,
                "Undoing a rename does not restart media decoders",
                ui_smoke::Needs::kKeyWindow);
            [self menuRedo:nil];
            const ProjectBinMetadata* redoneRename =
                self.state->project.FindBinMetadata(sourceId);
            UiSmokeCheck(
                redoneRename &&
                    redoneRename->display_name == "Rush renommé par clic" &&
                    gUiSmokeDecodeReloads == reloadsBeforeRenameHistory,
                "Redoing a rename stays on the lightweight refresh path",
                ui_smoke::Needs::kKeyWindow);
        }
        [self.mediaCollection layoutSubtreeIfNeeded];
        iconItem = [self.mediaCollection itemAtIndexPath:iconPath];
        iconAttributes = [self.mediaCollection.collectionViewLayout
            layoutAttributesForItemAtIndexPath:iconPath];
        if (iconItem) {
            NSIndexPath* secondPath =
                [NSIndexPath indexPathForItem:iconPath.item + 1 inSection:0];
            self.mediaCollection.selectionIndexPaths =
                [NSSet setWithObjects:iconPath, secondPath, nil];
            const NSRect dragFrame =
                [iconItem.view convertRect:iconItem.view.bounds
                                    toView:self.mediaCollection];
            SendWindowDragGesture(
                self.mediaCollection,
                NSMakePoint(NSMidX(dragFrame), NSMidY(dragFrame)),
                self.metalView, destination);
        }
        // The press reaches the tile on any window; the drag session that
        // follows it starts on the key window only.
        UiSmokeCheck(gUiSmokeIconMouseDown,
                     "Physical icon drag reaches the collection mouseDown");
        UiSmokeCheck(gUiSmokeIconDragSession,
                     "Physical icon gesture starts an AppKit drag session",
                     ui_smoke::Needs::kKeyWindow);
        UiSmokeCheck(gUiSmokeMediaDragCount == 2,
                     "Icon drag payload preserves both selected rushes",
                     ui_smoke::Needs::kKeyWindow);
        // PRD8 -- the session runs once the run loop turns, here rather
        // than in whatever scenario spins it next, and it ends.
        NSDate* dragDeadline = [NSDate dateWithTimeIntervalSinceNow:3.0];
        while (gUiSmokeIconDragSession && !gUiSmokeIconDragEnded &&
               dragDeadline.timeIntervalSinceNow > 0)
            [self spinRunLoopFor:0.05];
        UiSmokeCheck(gUiSmokeIconDragEnded, "Physical icon drag session ends",
                     ui_smoke::Needs::kKeyWindow);
        UiSmokeCheck(iconItem && iconAttributes,
                     "Physical icon drag uses a visible collection item");
    }
    NSTableColumn* nameColumn =
        [self.mediaTable tableColumnWithIdentifier:@"name"];
    BOOL everyListNameIsEditable = nameColumn != nil;
    for (NSInteger row = 0;
         row < (NSInteger)self.visibleMediaIds.count && everyListNameIsEditable;
         ++row) {
        NSTableCellView* cell =
            (NSTableCellView*)[self tableView:self.mediaTable
                           viewForTableColumn:nameColumn
                                          row:row];
        everyListNameIsEditable =
            [cell.textField isKindOfClass:BrowserRenameTextField.class] &&
            cell.textField.editable &&
            [cell.textField.identifier hasPrefix:@"browser:"];
    }
    UiSmokeCheck(everyListNameIsEditable,
                 "List-mode names are inline-editable for every item kind");

    const auto commitInlineName = [&](NSString* identifier, NSString* name) {
        NSTextField* field = [NSTextField textFieldWithString:name];
        field.identifier = [@"browser:" stringByAppendingString:identifier];
        NSNotification* ended = [NSNotification
            notificationWithName:NSControlTextDidEndEditingNotification
                          object:field
                        userInfo:@{
                            NSTextMovementUserInfoKey : @(NSReturnTextMovement)
                        }];
        [self controlTextDidEndEditing:ended];
    };
    NSString* smokeBinIdentifier = [NSString
        stringWithUTF8String:self.state->document.bins.front().id.c_str()];
    commitInlineName(smokeBinIdentifier, @"Chutier renommé");
    commitInlineName(timelineIdentifier, @"Timeline renommée");
    commitInlineName(sourceIdentifier, @"Rush renommé");
    const ProjectBinMetadata* renamedMetadata =
        self.state->project.FindBinMetadata(sourceId);
    UiSmokeCheck(
        self.state->document.bins.front().name == "Chutier renommé" &&
            self.state->project.FindTimeline(self.state->activeTimelineId)
                    ->name == "Timeline renommée" &&
            renamedMetadata &&
            renamedMetadata->display_name == "Rush renommé" &&
            self.state->document.FindLibraryMedia(sourceId)->filename ==
                "00-fixture.mov",
        "Inline names persist for bins, timelines, and rushes without "
        "renaming the source file");
    UiSmokeCheck(
        [self.programMonitorTitleLabel.stringValue
            isEqualToString:@"RECORD — Timeline renommée"] &&
            [self.window.title hasSuffix:@"— Timeline renommée"],
        "UTF-8 timeline names remain intact in the window and Record titles");
    self.state->sourceMonitor = true;
    self.state->sourceMonitorActive = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceIn.reset();
    self.state->sourceOut.reset();
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->requestedPosition = {0, 25};
    NSButton* insert = FindButtonWithTitle(self.sourceMonitorView, @"INSÉRER");
    UiSmokeCheck(insert != nil, "Source Insert button exists");
    if (insert && initialTrackId) {
        const NSPoint insertPoint =
            NSMakePoint(NSMidX(insert.frame), NSMidY(insert.frame));
        UiSmokeCheck([self.sourceMonitorView hitTest:insertPoint] == insert,
                     "Source Insert button is hit-testable above Metal");
        const DocumentTrack* beforeTrack =
            self.state->document.FindTrack(*initialTrackId);
        const size_t before = beforeTrack ? beforeTrack->clips.size() : 0;
        [insert performClick:nil];
        const DocumentTrack* edited =
            self.state->document.FindTrack(*initialTrackId);
        UiSmokeCheck(edited && edited->clips.size() > before,
                     "Source Insert button performs a persisted edit");
    }

    NSButton* overwrite =
        FindButtonWithTitle(self.sourceMonitorView, @"ÉCRASER");
    UiSmokeCheck(overwrite != nil, "Source Overwrite button exists");
    if (overwrite) {
        const NSPoint overwritePoint =
            NSMakePoint(NSMidX(overwrite.frame), NSMidY(overwrite.frame));
        UiSmokeCheck(
            [self.sourceMonitorView hitTest:overwritePoint] == overwrite,
            "Source Overwrite button is hit-testable above Metal");
        const size_t before = self.state->editLog.AppliedCount();
        [overwrite performClick:nil];
        UiSmokeCheck(self.state->editLog.AppliedCount() > before &&
                         [self.infoLabel.stringValue containsString:@"écrasé"],
                     "Source Overwrite button performs a persisted edit");
    }

    if (initialTrackId && self.state->document.sources.size() >= 2) {
        const size_t before = self.state->editLog.AppliedCount();
        const Ulid secondSourceId = self.state->document.sources[1].id;
        const double timelineY =
            kTimelineRulerHeight +
            (videoDisplayIndex + 0.5) * self.state->viewport.track_height;
        const NSPoint point =
            NSMakePoint(self.state->viewport.TimeToX({75, 25}),
                        self.metalView.bounds.size.height - timelineY);
        const BOOL linkedSelection = self.state->linkedSelection;
        self.state->linkedSelection = false;
        const BOOL accepted = [self timelineDropMediaIds:@[
            [NSString stringWithUTF8String:sourceId.c_str()],
            [NSString stringWithUTF8String:secondSourceId.c_str()]
        ]
                                             atViewPoint:point];
        const std::vector<Ulid> dropped =
            self.state->interaction->SelectedClipIds();
        const DocumentClip* firstDropped =
            dropped.empty() ? nullptr
                            : self.state->document.FindClip(dropped[0]);
        const DocumentClip* secondDropped =
            dropped.size() < 2 ? nullptr
                               : self.state->document.FindClip(dropped[1]);
        UiSmokeCheck(
            accepted && self.state->editLog.AppliedCount() == before + 1 &&
                dropped.size() == 2 && firstDropped && secondDropped &&
                firstDropped->source_id == sourceId &&
                secondDropped->source_id == secondSourceId &&
                secondDropped->timeline_in ==
                    firstDropped->timeline_in.add(firstDropped->duration),
            "Multi-media drop edits the selected rushes in order, in one step");

        // Out of lot -- 29 October 2026. Dropped in the blank below the
        // tracks, a rush gets a track of its own, and one Undo takes the
        // track back with the clip.
        const size_t tracksBefore = self.state->document.sequence.tracks.size();
        const size_t entriesBefore = self.state->editLog.AppliedCount();
        const double blankY =
            kTimelineRulerHeight +
            (tracksBefore + 0.5) * self.state->viewport.track_height;
        const BOOL blankAccepted = [self
            timelineDropMediaIds:@[ [NSString
                                     stringWithUTF8String:sourceId.c_str()] ]
                     atViewPoint:NSMakePoint(
                                     self.state->viewport.TimeToX({75, 25}),
                                     self.metalView.bounds.size.height -
                                         blankY)];
        const std::vector<Ulid> blankDropped =
            self.state->interaction->SelectedClipIds();
        bool onNewTrack = false;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (!blankDropped.empty() && clip.id == blankDropped[0])
                    onNewTrack =
                        track.kind == "video" && track.clips.size() == 1;
        UiSmokeCheck(
            blankAccepted &&
                self.state->document.sequence.tracks.size() > tracksBefore &&
                self.state->editLog.AppliedCount() == entriesBefore + 1 &&
                onNewTrack,
            "a drop in the blank below the tracks creates the "
            "track it lands on, in one step");
        [self menuUndo:nil];
        UiSmokeCheck(
            self.state->document.sequence.tracks.size() == tracksBefore,
            "one Undo takes the drop back, its new tracks with it");
        self.state->linkedSelection = linkedSelection;
        self.state->interaction->SetLinkedSelectionEnabled(linkedSelection);
    }

    CAMetalLayer* sourceLayer = (CAMetalLayer*)self.sourceMonitorView.layer;
    CAMetalLayer* programLayer = (CAMetalLayer*)self.programMonitorView.layer;
    UiSmokeCheck(sourceLayer.pixelFormat == programLayer.pixelFormat &&
                     sourceLayer.colorspace != nullptr &&
                     programLayer.colorspace != nullptr &&
                     CFEqual(sourceLayer.colorspace, programLayer.colorspace) &&
                     sourceLayer.wantsExtendedDynamicRangeContent ==
                         programLayer.wantsExtendedDynamicRangeContent,
                 "Source and Record use the same Metal presentation policy");

    // F2.3 -- the Media panel's tabs, back since the visual QA of 19
    // September 2026. Each shows its own container, the Audio tab lists what
    // FilterAudioSources gives for the bin chosen in the Media tab, and the
    // Captions tab's buttons make and remove a style in one edit each.
    {
        [self.mediaTabStrip selectIndex:1];
        UiSmokeCheck(self.mediaTabContentMedia.hidden &&
                         !self.mediaTabContentAudio.hidden &&
                         self.mediaTabContentCaptions.hidden,
                     "the Audio tab shows its own container");
        const std::string bin(self.selectedBinId.UTF8String ?: "__all__");
        const size_t audible =
            ui::media_panel::FilterAudioSources(
                self.state->document.library, bin == "__all__",
                bin == "__root__",
                bin == "__all__" || bin == "__root__" ? Ulid{} : bin, "")
                .size();
        UiSmokeCheck(audible > 0 &&
                         self.audioTable.numberOfRows ==
                             static_cast<NSInteger>(audible) &&
                         self.audioSummaryLabel.stringValue.length > 0,
                     "the Audio tab lists the audible media of the chosen bin");
        captureQaState("20-media-audio");
        // F2.3 (VQA3) -- the Audio tab's search and a change of bin, played
        // rather than read: what the table shows against FilterAudioSources.
        {
            const auto audibleFor = [&](const std::string& chosen,
                                        const std::string& search) {
                return ui::media_panel::FilterAudioSources(
                           self.state->document.library, chosen == "__all__",
                           chosen == "__root__",
                           chosen == "__all__" || chosen == "__root__" ? Ulid{}
                                                                       : chosen,
                           search)
                    .size();
            };
            self.audioSearchField.stringValue = @"tone";
            [self audioSearchChanged:self.audioSearchField];
            const size_t tone = audibleFor(bin, "tone");
            UiSmokeCheck(tone > 0 && self.audioTable.numberOfRows ==
                                         static_cast<NSInteger>(tone),
                         ("VQA F2.3: searching the Audio tab for \"tone\" "
                          "keeps " +
                          std::to_string(tone) + " of " +
                          std::to_string(audible) + " rows")
                             .c_str());
            self.audioSearchField.stringValue = @"zzz-rien";
            [self audioSearchChanged:self.audioSearchField];
            UiSmokeCheck(self.audioTable.numberOfRows == 0,
                         "VQA F2.3: a search matching nothing empties the "
                         "Audio tab");
            self.audioSearchField.stringValue = @"";
            [self audioSearchChanged:self.audioSearchField];
            // Another bin: the QA bin holds the patterns, none audible.
            NSString* previousBin = self.selectedBinId;
            std::string qaBin;
            for (const DocumentBin& candidate : self.state->document.bins)
                if (candidate.name == "QA") qaBin = candidate.id;
            [self refreshBinControlsSelecting:
                      [NSString stringWithUTF8String:qaBin.c_str()]];
            UiSmokeCheck(
                !qaBin.empty() &&
                    self.audioTable.numberOfRows ==
                        static_cast<NSInteger>(audibleFor(qaBin, "")) &&
                    [self.audioSummaryLabel.stringValue containsString:@"QA"],
                ("VQA F2.3: choosing the QA bin in the Media tab "
                 "refilters the Audio tab and its summary names the "
                 "bin (\"" +
                 std::string(self.audioSummaryLabel.stringValue.UTF8String
                                 ?: "") +
                 "\")")
                    .c_str());
            [self refreshBinControlsSelecting:previousBin];
            UiSmokeCheck(
                self.audioTable.numberOfRows == static_cast<NSInteger>(audible),
                "VQA F2.3: the bin chosen before comes back");

            // Selection and scroll survive a round trip through the other
            // tabs.
            [self.audioTable selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                         byExtendingSelection:NO];
            [self.mediaTabStrip selectIndex:0];
            NSClipView* mediaClip =
                self.mediaCollection.enclosingScrollView.contentView;
            const NSPoint scrolled = NSMakePoint(
                0.0, std::max(0.0, NSHeight(self.mediaCollection.frame) -
                                       NSHeight(mediaClip.bounds)) /
                         2.0);
            [mediaClip scrollToPoint:scrolled];
            [self.mediaCollection.enclosingScrollView
                reflectScrolledClipView:mediaClip];
            const NSPoint mediaOrigin = mediaClip.bounds.origin;
            NSIndexSet* mediaSelection = self.mediaCollection.selectionIndexes;
            [self.mediaTabStrip selectIndex:2];
            [self.mediaTabStrip selectIndex:1];
            UiSmokeCheck(self.audioTable.selectedRow == 0,
                         ("VQA F2.3: the Audio tab's selection survives a "
                          "visit to the Captions tab (row " +
                          std::to_string(self.audioTable.selectedRow) + ")")
                             .c_str());
            [self.mediaTabStrip selectIndex:0];
            UiSmokeCheck(
                std::abs(mediaClip.bounds.origin.y - mediaOrigin.y) < 0.5 &&
                    [self.mediaCollection.selectionIndexes
                        isEqualToIndexSet:mediaSelection],
                ("VQA F2.3: the Media tab keeps its scroll (" +
                 std::to_string(mediaOrigin.y) + " -> " +
                 std::to_string(mediaClip.bounds.origin.y) +
                 " pt) and its selection after the other two tabs")
                    .c_str());
            // The Media tab's content starts right under the tab strip.
            UiSmokeCheck(
                std::abs(NSMaxY(self.mediaTabContentMedia.frame) -
                         NSMinY(self.mediaTabStrip.frame)) < 0.5,
                "VQA F2.3: the Media tab's content meets the tab strip, "
                "neither under it nor below a gap");
            // The Captions summary follows the selection while hidden.
            if (initialCaptionClipId) {
                self.state->interaction->SelectClip("");
                [self updateSelectionInfo];
                self.state->interaction->SelectClip(*initialCaptionClipId);
                [self updateSelectionInfo];
                UiSmokeCheck(
                    self.mediaTabContentCaptions.hidden &&
                        [self.captionSelectionLabel.stringValue
                            containsString:@"1 carton sélectionné"],
                    ("VQA F2.3: the Captions summary is current while its "
                     "tab is hidden (\"" +
                     std::string(
                         self.captionSelectionLabel.stringValue.UTF8String
                             ?: "") +
                     "\")")
                        .c_str());
                self.state->interaction->SelectClip("");
                [self updateSelectionInfo];
            }
            [self.mediaTabStrip selectIndex:1];
        }
        [self.mediaTabStrip selectIndex:2];
        UiSmokeCheck(self.mediaTabContentMedia.hidden &&
                         self.mediaTabContentAudio.hidden &&
                         !self.mediaTabContentCaptions.hidden,
                     "the Captions tab shows its own container");
        UiSmokeCheck([[CMUiPreferences.sharedPreferences lastMediaPanelTab]
                         isEqualToString:@"captions"],
                     "the tab shown is remembered as a local preference");
        NSTableColumn* captionCountColumn =
            [self.captionStyleTable tableColumnWithIdentifier:@"clips"];
        const NSInteger captionCountColumnIndex =
            [self.captionStyleTable columnWithIdentifier:@"clips"];
        const NSRect captionCountColumnRect =
            captionCountColumnIndex == -1
                ? NSZeroRect
                : [self.captionStyleTable rectOfColumn:captionCountColumnIndex];
        UiSmokeCheck(
            captionCountColumn != nil &&
                captionCountColumn.width >=
                    std::ceil(captionCountColumn.headerCell.cellSize.width) +
                        8.0,
            "the Cartons column is wide enough for its header");
        UiSmokeCheck(captionCountColumnIndex != -1 &&
                         NSMinX(captionCountColumnRect) >=
                             NSMinX(self.captionStyleTable.visibleRect) - 0.5 &&
                         NSMaxX(captionCountColumnRect) <=
                             NSMaxX(self.captionStyleTable.visibleRect) + 0.5,
                     "the Cartons column stays inside the visible table");
        UiSmokeCheck(
            NSWidth(self.captionStyleTable.frame) <=
                NSWidth(self.captionStyleScroll.contentView.bounds) + 0.5,
            "the caption style table follows its clip view width");
        const size_t styles =
            self.state->document.sequence.caption_styles.size();
        UiSmokeCheck(initialCaptionClipId.has_value() && styles > 0,
                     "the Captions fixture contains a styled carton");
        if (initialCaptionClipId && styles > 0) {
            self.state->interaction->SelectClip(*initialCaptionClipId);
            [self.captionStyleTable
                    selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                byExtendingSelection:NO];
            [self updateSelectionInfo];
            const DocumentClip* selectedCaption =
                self.state->document.FindClip(*initialCaptionClipId);
            if (selectedCaption) {
                [self requestTimelinePosition:selectedCaption->timeline_in.add(
                                                  {1, 25})];
                [self presentNearestFrameAtDeadline:NO];
            }
            // LG5 -- the carton is the caption renderer's picture, laid
            // over the frame Record shows.
            CMCaptionView* captionView = self.programCaptionView;
            UiSmokeCheck(
                [captionView.captionText
                    isEqualToString:@"Carton QA : lisible sur l’image"] &&
                    !captionView.hidden && captionView.layer.contents != nil,
                "the Record caption keeps its complete UTF-8 text, drawn as "
                "the export draws it");
            const ui::caption_overlay::Layout shownCanvas =
                ui::caption_overlay::Compute(
                    self.state->document.sequence.width,
                    self.state->document.sequence.height,
                    NSWidth(self.programMonitorView.bounds),
                    NSHeight(self.programMonitorView.bounds),
                    self.state->programMonitorZoom > 0.0
                        ? self.state->programMonitorZoom /
                              self.programMonitorView.layer.contentsScale
                        : 0.0);
            UiSmokeCheck(std::abs(NSWidth(captionView.frame) -
                                  shownCanvas.canvas_width) < 0.5 &&
                             std::abs(NSHeight(captionView.frame) -
                                      shownCanvas.canvas_height) < 0.5,
                         "the Record caption's picture covers the frame shown");
            UiSmokeCheck(
                selectedCaption &&
                    selectedCaption->caption_group_id ==
                        self.state->document.sequence.caption_styles[0].id &&
                    self.clearCaptionButton.enabled &&
                    [self.captionSelectionLabel.stringValue
                        containsString:@"1 carton sélectionné"],
                "the Captions tab identifies the selected styled carton");
        }
        captureQaState("21-media-captions");
        const size_t applied = self.state->editLog.AppliedCount();
        [self.addCaptionStyleButton
            sendAction:self.addCaptionStyleButton.action
                    to:self.addCaptionStyleButton.target];
        UiSmokeCheck(
            self.state->document.sequence.caption_styles.size() == styles + 1 &&
                self.state->editLog.AppliedCount() == applied + 1 &&
                self.captionStyleTable.selectedRow ==
                    static_cast<NSInteger>(styles) &&
                self.removeCaptionStyleButton.enabled,
            "+ STYLE adds one style in one edit and selects it");
        // F2.3 -- the look of the selected style, from the tab: one edit per
        // control, every other setting kept, one undo each.
        {
            const Ulid lookStyle =
                self.state->document.sequence.caption_styles.back().id;
            const auto lookOf = [&]() {
                const CaptionStyle* style =
                    self.state->document.FindCaptionStyle(lookStyle);
                return style ? *style : CaptionStyle{};
            };
            const CaptionStyle before = lookOf();
            UiSmokeCheck(
                self.captionFontPopup.enabled &&
                    self.captionSizeField.integerValue == before.font_size &&
                    [self.captionFontPopup.selectedItem.representedObject
                        isEqual:CMStringFromUtf8(before.font_family)],
                "the look controls show the selected style");
            const size_t lookEdits = self.state->editLog.AppliedCount();
            [self.captionSizeField sendAction:self.captionSizeField.action
                                           to:self.captionSizeField.target];
            UiSmokeCheck(self.state->editLog.AppliedCount() == lookEdits,
                         "an unchanged size makes no edit");
            self.captionSizeField.integerValue = 72;
            [self.captionSizeField sendAction:self.captionSizeField.action
                                           to:self.captionSizeField.target];
            UiSmokeCheck(
                lookOf().font_size == 72 && lookOf().color == before.color &&
                    lookOf().font_family == before.font_family &&
                    self.state->editLog.AppliedCount() == lookEdits + 1,
                "Taille sets the style's size in one edit");
            self.captionColorWell.color = [NSColor colorWithSRGBRed:1.0
                                                              green:0.0
                                                               blue:0.0
                                                              alpha:1.0];
            [self.captionColorWell sendAction:self.captionColorWell.action
                                           to:self.captionColorWell.target];
            UiSmokeCheck(
                lookOf().color == "#ff0000" && lookOf().font_size == 72,
                "Couleur sets the text colour, the size kept");
            NSInteger family = -1;
            for (NSInteger index = 0;
                 index < self.captionFontPopup.numberOfItems; ++index)
                if ([[self.captionFontPopup itemAtIndex:index].representedObject
                        isEqual:@"Helvetica"])
                    family = index;
            if (family >= 0) {
                [self.captionFontPopup selectItemAtIndex:family];
                [self.captionFontPopup sendAction:self.captionFontPopup.action
                                               to:self.captionFontPopup.target];
                UiSmokeCheck(lookOf().font_family == "Helvetica" &&
                                 lookOf().color == "#ff0000",
                             "Police sets the family, the colour kept");
                [self menuUndo:nil];
            }
            [self menuUndo:nil];
            [self menuUndo:nil];
            const CaptionStyle after = lookOf();
            UiSmokeCheck(
                after.font_size == before.font_size &&
                    after.color == before.color &&
                    after.font_family == before.font_family &&
                    self.captionSizeField.integerValue == before.font_size,
                "each look edit undoes on its own, and the controls "
                "follow");
        }
        if (initialCaptionClipId) {
            const Ulid addedStyle =
                self.state->document.sequence.caption_styles.back().id;
            [self.applyCaptionStyleButton
                sendAction:self.applyCaptionStyleButton.action
                        to:self.applyCaptionStyleButton.target];
            const DocumentClip* restyled =
                self.state->document.FindClip(*initialCaptionClipId);
            UiSmokeCheck(
                restyled && restyled->caption_group_id == addedStyle &&
                    restyled->caption_text == "Carton QA : lisible sur l’image",
                "APPLIQUER LE STYLE preserves the caption text");
            [self.clearCaptionButton sendAction:self.clearCaptionButton.action
                                             to:self.clearCaptionButton.target];
            const DocumentClip* cleared =
                self.state->document.FindClip(*initialCaptionClipId);
            UiSmokeCheck(
                cleared && cleared->caption_group_id.empty() &&
                    cleared->caption_text == "Carton QA : lisible sur l’image",
                "STYLE PAR DÉFAUT preserves the caption text");
            [self menuUndo:nil];
            [self menuUndo:nil];
        }
        // D7 -- docs/roadmap/DIALOGUE-2026-10.md. The words of a carton,
        // corrected in the Inspector: Return on the selected carton and a
        // double click on it put the keyboard in its field; there Return
        // breaks the line, Record shows what is typed, and leaving the field
        // makes one edit that keeps the style and undoes alone. What the
        // field looks like is the checklist's.
        if (initialCaptionClipId) {
            const Ulid cartonId = *initialCaptionClipId;
            const auto carton = [&] {
                return self.state->document.FindClip(cartonId);
            };
            const std::string wordsBefore = carton()->caption_text;
            const Ulid styleBefore = carton()->caption_group_id;
            NSTextField* field = self.inspectorView.captionTextField;
            const auto typing = [&] { return field.currentEditor != nil; };
            // Typed, then committed as the loss of focus commits.
            const auto type = [&](NSString* text) {
                field.stringValue = text;
                [field sendAction:field.action to:field.target];
            };
            const auto fieldShows = [&](const std::string& text) {
                return
                    [field.stringValue isEqualToString:CMStringFromUtf8(text)];
            };
            const ui::PanelSlot dockBefore = [self.rightDockPanel selectedSlot];
            [self setTimelineTool:TimelineTool::Select];
            NSMenuItem* textItem =
                [self menuItem:@"Modifier le texte du carton (↩)"
                        action:@selector(menuEditCartonText:)
                           key:@""];
            if (initialClipId) {
                self.state->interaction->SelectClip(*initialClipId);
                [self updateSelectionInfo];
                SendKeyThroughWindow(self.metalView, @"\r", 36);
                UiSmokeCheck(![self validateMenuItem:textItem] &&
                                 field.hidden && !typing(),
                             "a picture has no carton text: no field in the "
                             "Inspector, the menu entry is off and Return "
                             "does nothing");
            }
            // The Agent's window up -- the dock's other tab, in front of the
            // Inspector, until ON3: Return still brings the Inspector's text.
            [self showAgentWindow:YES focus:NO];
            self.state->interaction->SelectClip(cartonId);
            [self updateSelectionInfo];
            UiSmokeCheck([self validateMenuItem:textItem] && !field.hidden &&
                             fieldShows(wordsBefore),
                         "a selected carton shows its words in the "
                         "Inspector's field and enables the menu entry");
            const size_t editsBefore = self.state->editLog.AppliedCount();
            SendKeyThroughWindow(self.metalView, @"\r", 36);
            UiSmokeCheck(
                [self.rightDockPanel selectedSlot] ==
                        ui::PanelSlot::Inspector &&
                    typing() &&
                    self.state->editLog.AppliedCount() == editsBefore,
                "Return on a selected carton, the Agent's window up, puts "
                "the keyboard in the Inspector's text, without an edit");
            [self showAgentWindow:NO focus:NO];
            // PRD37 -- ROADMAP.md. The carton's track is named whole in the
            // Inspector (QA of 3 October 2026: « Sous-titres… »).
            {
                NSTextField* trackValue = (NSTextField*)UiQaFindView(
                    self.inspectorView, ^BOOL(NSView* view) {
                      return [view isKindOfClass:NSTextField.class] &&
                             [((NSTextField*)view).stringValue
                                 hasPrefix:@"Sous-titres · "];
                    });
                const double needed =
                    trackValue != nil ? trackValue.cell.cellSize.width : 0.0;
                const double room =
                    trackValue != nil ? NSWidth(trackValue.frame) : 0.0;
                char detail[96];
                std::snprintf(detail, sizeof detail,
                              " (%.1f points of text in %.1f)", needed, room);
                UiSmokeCheck(trackValue != nil && needed <= room + 0.5,
                             (std::string("PRD37: a carton's track is named "
                                          "whole in the Inspector") +
                              detail)
                                 .c_str());
            }
            captureQaState("22-inspector-carton-focus");
            type(@"Carton corrigé\r\n sur deux lignes ");
            UiSmokeCheck(
                carton()->caption_text == "Carton corrigé\nsur deux lignes" &&
                    carton()->caption_group_id == styleBefore &&
                    self.state->editLog.AppliedCount() == editsBefore + 1 &&
                    fieldShows("Carton corrigé\nsur deux lignes") &&
                    [self.infoLabel.stringValue
                        containsString:@"Carton corrigé / sur deux lignes"],
                "the typed text is one edit that keeps the carton's style; "
                "the field and the status line show it as the document "
                "holds it");
            [self requestTimelinePosition:carton()->timeline_in.add({1, 25})];
            [self presentNearestFrameAtDeadline:NO];
            UiSmokeCheck(
                [self.programCaptionView.captionText
                    isEqualToString:@"Carton corrigé\nsur deux lignes"],
                "Record shows the corrected words, on their two lines");
            captureQaState("23-inspector-carton-two-lines");
            type(@"Carton corrigé\nsur deux lignes ");
            UiSmokeCheck(
                self.state->editLog.AppliedCount() == editsBefore + 1 &&
                    fieldShows("Carton corrigé\nsur deux lignes"),
                "the same words typed again make no edit, and the "
                "field shows the document's text again");
            type(@" \n ");
            UiSmokeCheck(
                self.state->editLog.AppliedCount() == editsBefore + 1 &&
                    carton()->caption_text ==
                        "Carton corrigé\nsur deux lignes" &&
                    fieldShows("Carton corrigé\nsur deux lignes") &&
                    [self.infoLabel.stringValue containsString:@"vide"],
                "a carton is not emptied: the field takes its words back "
                "and the status line says why");
            [self menuUndo:nil];
            UiSmokeCheck(carton()->caption_text == wordsBefore &&
                             carton()->caption_group_id == styleBefore &&
                             fieldShows(wordsBefore),
                         "one undo gives the carton its words back, and the "
                         "field follows");

            // A key held with Cmd, sent the way the window receives one.
            const auto command = [&](NSString* key, unsigned short code) {
                [NSApp
                    sendEvent:[NSEvent keyEventWithType:NSEventTypeKeyDown
                                                     location:NSZeroPoint
                                                modifierFlags:
                                                    NSEventModifierFlagCommand
                                                    timestamp:NSProcessInfo
                                                                  .processInfo
                                                                  .systemUptime
                                                 windowNumber:self.window
                                                                  .windowNumber
                                                      context:nil
                                                   characters:key
                                  charactersIgnoringModifiers:key
                                                    isARepeat:NO
                                                      keyCode:code]];
            };
            // Typed as a hand types: the editor's text changes and says so.
            const auto keyIn = [&](NSString* text) {
                NSTextView* editor = (NSTextView*)field.currentEditor;
                editor.string = text;
                editor.selectedRange = NSMakeRange(text.length, 0);
                [editor didChangeText];
            };
            const auto recordShows = [&](const std::string& text) {
                [self
                    requestTimelinePosition:carton()->timeline_in.add({1, 25})];
                [self presentNearestFrameAtDeadline:NO];
                return [self.programCaptionView.captionText
                    isEqualToString:CMStringFromUtf8(text)];
            };

            // The text keys are the field's while it is typed in. Checked
            // by Select All and Undo: Copy and Paste take the same road, and
            // would write over the clipboard of whoever runs this.
            SendKeyThroughWindow(self.metalView, @"\r", 36);
            if (typing()) {
                field.currentEditor.selectedRange = NSMakeRange(0, 0);
                command(@"a", 0);
                const NSUInteger selected =
                    field.currentEditor.selectedRange.length;
                command(@"z", 6);
                UiSmokeCheck(
                    selected == field.stringValue.length && selected > 0 &&
                        typing() &&
                        self.state->editLog.AppliedCount() == editsBefore &&
                        carton()->caption_text == wordsBefore,
                    "in the field, Cmd+A selects its text and Cmd+Z leaves "
                    "the montage alone");
            }

            // As in Resolve: Record shows what is typed before anything is
            // committed, Return breaks the line, and Escape gives it all up
            // and hands the keys back to the timeline.
            SendKeyThroughWindow(self.metalView, @"\r", 36);
            if (typing()) {
                keyIn(@"Aperçu en direct");
                UiSmokeCheck(
                    recordShows("Aperçu en direct") &&
                        carton()->caption_text == wordsBefore &&
                        self.state->editLog.AppliedCount() == editsBefore,
                    "Record shows what is typed in the field, before any "
                    "edit");
                [(id<NSTextFieldDelegate>)field.delegate
                                control:field
                               textView:(NSTextView*)field.currentEditor
                    doCommandBySelector:@selector(insertNewline:)];
                UiSmokeCheck(
                    typing() &&
                        [field.currentEditor.string
                            isEqualToString:@"Aperçu en direct\n"] &&
                        self.state->editLog.AppliedCount() == editsBefore,
                    "Return breaks the line in the field and commits "
                    "nothing");
                [(id<NSTextFieldDelegate>)field.delegate
                                control:field
                               textView:(NSTextView*)field.currentEditor
                    doCommandBySelector:@selector(cancelOperation:)];
            }
            UiSmokeCheck(
                !typing() && fieldShows(wordsBefore) &&
                    self.window.firstResponder == self.metalView &&
                    self.state->editLog.AppliedCount() == editsBefore &&
                    recordShows(wordsBefore),
                "Escape gives up what was typed, on Record too, and hands "
                "the keys back to the timeline, without an edit");

            // Cmd+Return: done. The field is left, which commits it.
            SendKeyThroughWindow(self.metalView, @"\r", 36);
            if (typing()) {
                keyIn(@"Validé\nen deux lignes");
                command(@"\r", 36);
            }
            UiSmokeCheck(
                !typing() && self.window.firstResponder == self.metalView &&
                    carton()->caption_text == "Validé\nen deux lignes" &&
                    carton()->caption_group_id == styleBefore &&
                    self.state->editLog.AppliedCount() == editsBefore + 1 &&
                    recordShows("Validé\nen deux lignes"),
                "Cmd+Return commits what was typed as one edit and hands "
                "the keys back to the timeline");
            [self menuUndo:nil];
            UiSmokeCheck(carton()->caption_text == wordsBefore &&
                             fieldShows(wordsBefore) &&
                             recordShows(wordsBefore),
                         "one undo gives the words back, in the field and "
                         "on Record");

            // The double click, aimed by the geometry the pointer hits. The
            // earlier checks left the view zoomed in on the head of the
            // timeline: it is scrolled to the carton, and put back after.
            const TimelineViewport viewBefore = self.state->viewport;
            const auto rows =
                TimelineTracksInDisplayOrder(self.state->document);
            const DocumentTrack* cartonTrack =
                self.state->document.FindTrackForClip(cartonId);
            const auto row = std::find(rows.begin(), rows.end(), cartonTrack);
            const auto cartonMiddle = [&] {
                return (self.state->viewport.TimeToX(carton()->timeline_in) +
                        self.state->viewport.TimeToX(
                            carton()->timeline_in.add(carton()->duration))) /
                       2.0;
            };
            self.state->viewport.ScrollByPixels(
                cartonMiddle() - (self.state->viewport.header_width +
                                  self.metalView.bounds.size.width) /
                                     2.0,
                kTimelineViewRate);
            const double cartonX = cartonMiddle();
            const double cartonY = kTimelineRulerHeight +
                                   static_cast<double>(row - rows.begin()) *
                                       self.state->viewport.track_height +
                                   self.state->viewport.track_height * 0.5;
            const auto aimed = HitTestTimeline(
                self.state->document, self.state->viewport, cartonX, cartonY,
                self.metalView.bounds.size.width);
            UiSmokeCheck(aimed && aimed->clip_id == cartonId,
                         "the carton is under the point the run aims at");
            if (aimed && aimed->clip_id == cartonId) {
                if (initialClipId) {
                    self.state->interaction->SelectClip(*initialClipId);
                    [self updateSelectionInfo];
                }
                SendTimelineDoubleClick(
                    self.metalView,
                    NSMakePoint(cartonX, self.metalView.bounds.size.height -
                                             [self videoHeight] - cartonY));
                UiSmokeCheck(
                    self.state->interaction->SelectedClipId() == cartonId &&
                        typing() && fieldShows(wordsBefore) &&
                        !self.state->editDragging &&
                        self.state->editLog.AppliedCount() == editsBefore,
                    "a double click on a carton selects it and puts the "
                    "keyboard in its text, with no drag and no edit");
                type(@"Corrigé au double clic");
                UiSmokeCheck(
                    carton()->caption_text == "Corrigé au double clic" &&
                        carton()->caption_group_id == styleBefore &&
                        self.state->editLog.AppliedCount() == editsBefore + 1,
                    "the text typed after a double click is one edit");
                [self menuUndo:nil];
                UiSmokeCheck(carton()->caption_text == wordsBefore,
                             "one undo gives the words back again");
            }
            self.state->viewport = viewBefore;
            self.state->overlayDirty = true;

            // A locked track: its cartons are read, not written.
            const DocumentTrack* lockedTrack =
                self.state->document.FindTrackForClip(cartonId);
            EditError lockError = EditError::None;
            std::string lockMessage;
            if (lockedTrack &&
                [self
                    applyAndPersistTimelineOperation:SetTrackLockOperation{
                                                         lockedTrack->id, true}
                                               error:lockError
                                             message:lockMessage]) {
                [self.window makeFirstResponder:self.metalView];
                self.state->interaction->SelectClip(cartonId);
                [self updateSelectionInfo];
                const size_t locked = self.state->editLog.AppliedCount();
                SendKeyThroughWindow(self.metalView, @"\r", 36);
                UiSmokeCheck(
                    !typing() && !field.editable &&
                        self.state->editLog.AppliedCount() == locked &&
                        [self.infoLabel.stringValue
                            containsString:@"verrouillée"],
                    "on a locked caption track the field does not take the "
                    "keys, and the status line says why");
                [self menuUndo:nil];
                [self updateSelectionInfo];
                UiSmokeCheck(field.editable,
                             "unlocked again, the field is editable");
            }
            [self.window makeFirstResponder:self.metalView];
            [self.rightDockPanel selectSlot:dockBefore];
            self.state->interaction->SelectClip(cartonId);
            [self updateSelectionInfo];
        }
        // F2.3 -- a picture carrying the style (set_clip_caption, or a
        // project from before #12) neither counts as a carton nor keeps the
        // style from being deleted: it loses it in the same edit.
        std::optional<Ulid> styledPicture;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            if (track.kind == "video" && !track.clips.empty() && !styledPicture)
                styledPicture = track.clips.front().id;
        const Ulid strayStyle =
            self.state->document.sequence.caption_styles.back().id;
        if (styledPicture) {
            EditError strayError = EditError::None;
            std::string strayMessage;
            UiSmokeCheck([self applyAndPersistTimelineOperation:
                                   ui::media_panel::JoinClipToCaptionStyle(
                                       *styledPicture, strayStyle, "")
                                                          error:strayError
                                                        message:strayMessage],
                         "a picture takes the style, as set_clip_caption does");
            const size_t beforeRemoval = self.state->editLog.AppliedCount();
            [self.removeCaptionStyleButton
                sendAction:self.removeCaptionStyleButton.action
                        to:self.removeCaptionStyleButton.target];
            const DocumentClip* picture =
                self.state->document.FindClip(*styledPicture);
            UiSmokeCheck(
                !self.state->document.FindCaptionStyle(strayStyle) && picture &&
                    picture->caption_group_id.empty() &&
                    self.state->editLog.AppliedCount() == beforeRemoval + 1 &&
                    [self.captionSelectionLabel.stringValue
                        containsString:@"retiré de 1 plan"],
                "- STYLE takes a style off the picture that carries it, in "
                "one edit, and says so");
            [self menuUndo:nil];
            picture = self.state->document.FindClip(*styledPicture);
            UiSmokeCheck(self.state->document.FindCaptionStyle(strayStyle) &&
                             picture && picture->caption_group_id == strayStyle,
                         "one undo gives the style and the picture back");
            const NSUInteger row = [self.visibleCaptionStyleIds
                indexOfObject:CMStringFromUtf8(strayStyle)];
            if (row != NSNotFound)
                [self.captionStyleTable
                        selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                    byExtendingSelection:NO];
        }
        [self.removeCaptionStyleButton
            sendAction:self.removeCaptionStyleButton.action
                    to:self.removeCaptionStyleButton.target];
        UiSmokeCheck(
            self.state->document.sequence.caption_styles.size() == styles,
            "- STYLE removes a style no carton wears");
        [self.mediaTabStrip selectIndex:0];
        UiSmokeCheck(!self.mediaTabContentMedia.hidden &&
                         self.mediaTabContentAudio.hidden &&
                         self.mediaTabContentCaptions.hidden,
                     "the Media tab comes back");
    }

    // Visual QA of 19 September 2026 -- a motion clip reaching the Record
    // monitor freed its frame twice and corrupted the heap: the app crashed
    // as soon as a title was shown. Placed last, so no check above sees the
    // clip: on the first video track, past everything the run edited.
    std::optional<Ulid> firstVideoTrackId;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && !firstVideoTrackId)
            firstVideoTrackId = track.id;
    if (firstVideoTrackId && !self.state->project.compositions.empty()) {
        InsertMotionClipOperation insert;
        insert.track_id = *firstVideoTrackId;
        insert.composition_id = self.state->project.compositions.front().id;
        insert.source_in = {0, 25};
        insert.duration = {20, 25};
        insert.timeline_in = {1000, 25};
        EditError insertError = EditError::None;
        std::string insertMessage;
        const BOOL inserted =
            [self applyAndPersistTimelineOperation:insert
                                             error:insertError
                                           message:insertMessage];
        UiSmokeCheck(inserted, "a motion clip is placed on the timeline");
        self.state->viewport.view_start = {985, 25};
        [self refreshTimelineAfterEditFromPosition:{1005, 25}];
        [self requestTimelinePosition:{1005, 25}];
        for (int tick = 0; tick < 6; ++tick)
            [self presentNearestFrameAtDeadline:NO];
        UiSmokeCheck(self.state->motionPreviews->Count() > 0,
                     "the Record monitor draws a motion clip's frame, and "
                     "the app survives it");
        // CF1 -- exercise the passive output window on this screen in the
        // harness, below the public API's second-screen validation. This
        // checks window focus and the real Metal present even on a laptop;
        // it does not validate a physical second display or reconnection.
        {
            NSView* const parent = self.programMonitorView.superview;
            NSResponder* const responder = self.window.firstResponder;
            NSWindow* const key = NSApp.keyWindow;
            std::string error;
            const BOOL opened = [self showVideoOutputOnScreen:self.window.screen
                                                      message:error];
            UiSmokeCheck(opened && self.videoOutputWindow &&
                             !self.videoOutputWindow.canBecomeKeyWindow &&
                             !self.videoOutputWindow.canBecomeMainWindow &&
                             self.videoOutputWindow.ignoresMouseEvents &&
                             self.programMonitorView.superview == parent,
                         "CF1: the clean feed is a passive, separate surface");
            UiSmokeCheck(NSApp.keyWindow == key &&
                             self.window.firstResponder == responder,
                         "CF1: opening the output leaves the editor's focus",
                         ui_smoke::Needs::kKeyWindow);
            if (opened && self.state->videoOutputRenderer) {
                self.state->videoOutputRenderer->CaptureNextPresent();
                [self presentNearestFrameAtDeadline:NO];
                CGImageRef picture =
                    self.state->videoOutputRenderer->TakePresentCapture();
                UiSmokeCheck(
                    picture && CGImageGetWidth(picture) > 0 &&
                        CGImageGetHeight(picture) > 0,
                    "CF1: the clean-feed window presents the motion frame",
                    ui_smoke::Needs::kVisibleWindow);
                if (picture) CGImageRelease(picture);
                UiSmokeCheck(
                    self.videoOutputView.subviews.count == 1 &&
                        self.videoOutputView.subviews.firstObject ==
                            self.videoOutputCaptionView,
                    "CF1: the output has captions and no editor controls");
            }
            [self stopVideoOutput];
            UiSmokeCheck(self.videoOutputWindow == nil &&
                             !self.state->videoOutputRenderer &&
                             self.programMonitorView.superview == parent,
                         "CF1: stopping output preserves the Record monitor");
        }
        captureQaState("30-motion-record");

        // REMOTION-PARITY-2026-09 REM5 -- the visual editor, driven through
        // the methods the mouse drives (no physical event: this session's
        // window may not take them). The title's text is selected on
        // Record, the Inspector turns to it, a drag commits one operation,
        // and one undo takes it back.
        const BOOL prepared = [self motionCanvasPrepare];
        UiSmokeCheck(prepared, "REM5: Record knows the motion clip it shows");
        if (prepared && !self.state->motionCanvas.frame.root.children.empty()) {
            MotionCanvasState& canvas = self.state->motionCanvas;
            const MotionEvaluatedNode text = canvas.frame.root.children.front();
            MotionHit hit;
            const bool hitText =
                MotionHitTest(canvas.frame, text.box.x + text.box.width / 2.0,
                              text.box.y + text.box.height / 2.0, false, hit);
            UiSmokeCheck(hitText && hit.uid == text.uid,
                         "REM5: a click on the title selects its text");
            [self motionCanvasSelect:hit.uid];
            UiSmokeCheck(self.motionInspectorShown && !canvas.fields.empty(),
                         "REM5: the Inspector turns to the node selected");
            // MA6 -- its layers are listed above, named as After names
            // them; choosing one in the list selects it.
            UiSmokeCheck(self.motionInspectorView.layerCount > 0 &&
                             !canvas.outline.empty(),
                         "MA6: the Inspector lists the composition's layers");
            expectNamedButtons("with a motion layer in the Inspector");
            captureQaState("31-motion-canvas-selected");
            [self motionInspector:self.motionInspectorView
                       selectNode:CMStringFromUtf8(canvas.outline.front().uid)];
            UiSmokeCheck(canvas.selected == canvas.outline.front().uid &&
                             self.motionInspectorShown,
                         "MA6: a layer chosen in the list is selected");
            [self motionCanvasSelect:hit.uid];
            // In full screen the whole screen is Record: the overlay drew
            // its outlines across the film, at the place they had in the
            // panel. Nothing is drawn there, the mouse outlines nothing,
            // and the selection is back with the window.
            const BOOL outlined = !self.motionCanvasView.hidden;
            // Leaving full screen hands the keyboard to Record; the checks
            // below expect it where it was.
            NSResponder* const keyboard = self.window.firstResponder;
            [self toggleFullscreen:nil];
            const BOOL clearedInFullscreen =
                self.videoFullscreenActive && self.motionCanvasView.hidden;
            canvas.hovered = canvas.outline.front().uid;
            [self motionCanvasRedraw];
            const BOOL hoverIgnored = self.motionCanvasView.hidden;
            [self requestTimelinePosition:self.state->requestedPosition];
            const BOOL stillClear = self.motionCanvasView.hidden;
            // ON5 -- a presentation surface may be Record's fullscreen
            // window as well as the editor, unlike a detached Media pane.
            self.state->programRenderer->CaptureNextPresent();
            self.state->overlayDirty = true;
            [self presentNearestFrameAtDeadline:NO];
            CGImageRef fullscreenFrame =
                self.state->programRenderer->TakePresentCapture();
            UiSmokeCheck(fullscreenFrame != nullptr,
                         "ON5: Record still presents in its fullscreen window",
                         ui_smoke::Needs::kVisibleWindow);
            if (fullscreenFrame) CGImageRelease(fullscreenFrame);
            [self toggleFullscreen:nil];
            [self.window makeFirstResponder:keyboard];
            UiSmokeCheck(
                outlined && clearedInFullscreen && hoverIgnored && stillClear,
                "REM5: full screen shows the film without the "
                "editor's outlines");
            UiSmokeCheck(
                !self.videoFullscreenActive && !self.motionCanvasView.hidden &&
                    canvas.selected == hit.uid && canvas.hovered.empty(),
                "REM5: back in the window, the selection is "
                "outlined again");
            const std::string before =
                self.state->document.FindComposition(canvas.composition_id)
                    ->definition.Dump();
            const size_t steps = self.state->editLog.AppliedCount();
            canvas.gesture = 1;
            canvas.delta_x = 5.0;
            canvas.delta_y = 3.0;
            [self motionCanvasMouseUp:nil];
            const std::string moved =
                self.state->document.FindComposition(canvas.composition_id)
                    ->definition.Dump();
            UiSmokeCheck(self.state->editLog.AppliedCount() == steps + 1 &&
                             moved.find("\"translateX\":5,\"translateY\":3") !=
                                 std::string::npos,
                         "REM5: a drag commits one operation, the text moved "
                         "by it");
            for (int tick = 0; tick < 4; ++tick)
                [self presentNearestFrameAtDeadline:NO];
            captureQaState("32-motion-canvas-moved");
            [self menuUndo:nil];
            UiSmokeCheck(
                self.state->document.FindComposition(canvas.composition_id)
                        ->definition.Dump() == before,
                "REM5: one undo takes the drag back");
            [self motionCanvasSelect:std::string()];
            UiSmokeCheck(!self.motionInspectorShown,
                         "REM5: without a selection, the clip Inspector is "
                         "back");
        }

        // REM5 -- the M4 usage gap: the dissolve menu did nothing at a cut
        // between two motion clips. The title cut in two halves, each with
        // the other's frames as handles, takes one; two undos restore the
        // project.
        const DocumentTrack* motionTrack =
            self.state->document.FindTrack(*firstVideoTrackId);
        Ulid titleId;
        if (motionTrack != nullptr)
            for (const DocumentClip& clip : motionTrack->clips)
                if (clip.timeline_in == RationalTime{1000, 25})
                    titleId = clip.id;
        const std::string beforeCut = self.state->document.SaveToString();
        EditError cutError = EditError::None;
        std::string cutMessage;
        const BOOL cut =
            !titleId.empty() &&
            [self applyAndPersistTimelineOperation:SplitClipOperation {
                titleId, {1010, 25}, {}, {}
            }
                                             error:cutError
                                           message:cutMessage];
        UiSmokeCheck(cut,
                     ("REM5: the title is cut in two: " + cutMessage).c_str());
        if (cut) {
            const size_t transitions =
                self.state->document.sequence.transitions.size();
            self.state->interaction->SelectClip(titleId);
            [self requestTimelinePosition:{1010, 25}];
            [self menuAddCrossDissolve:nil];
            const bool dissolved =
                self.state->document.sequence.transitions.size() ==
                    transitions + 1 &&
                self.state->document.sequence.transitions.back().left_clip_id ==
                    titleId;
            UiSmokeCheck(
                dissolved,
                (std::string("REM5: the dissolve menu joins two motion "
                             "clips: ") +
                 (self.infoLabel.stringValue.UTF8String ?: ""))
                    .c_str());
            if (dissolved) [self menuUndo:nil];
            [self menuUndo:nil];
            UiSmokeCheck(self.state->document.SaveToString() == beforeCut,
                         "REM5: the undos give the title back whole");
        }
    } else {
        UiSmokeCheck(false, "the smoke project carries its composition");
    }
    [self runUiSmokeTimelineViewScenario];
    [self runUiSmokeDecodersAheadScenario];
    [self runUiSmokeHiddenLayersScenario];
    [self runUiSmokeOffscreenCaptureScenario];
    [self runUiSmokeFilmcraftScenario];
    [self runUiSmokeSeparateSoundScenario];
    [self runUiSmokeCaptionStyleScenario];

    if (UiQaEnabled()) {
        [self runUiQaInspectorScenario];
        [self runUiQaMotionScenario];
        [self runUiQaMotionTemplateScenario];
        [self runUiQaMulticamScenario];
        [self runUiQaRollScenario];
        [self runUiQaFramingScenario];
        [self runUiQaSpeedScenario];
        [self runUiQaCadenceScenario];
        [self runUiQaNtscScenario];
        [self runUiQaCaptionScenario];
        [self runUiQaTranscriptCaptionScenario];
        [self runUiQaResolveScenario];
        [self runUiQaRenderCostScenario];
        [self runUiQaAnimationScenario];
        [self runUiQaDipScenario];
        [self runUiQaShakeScenario];
        [self runUiQaLookScenario];
        [self runUiQaHlgScenario];
        [self runUiQaPlaybackScenario];
    }
    // PRD24 -- after the scenarios above, which expect the timelines and
    // the view the main run left them; it puts both back.
    [self runUiSmokeClipSignageScenario];
    // PRD22 -- the engine dialog's form; touches no document.
    [self runUiSmokeAiEngineFormScenario];
    // PRD26 -- the forms on their panel; puts back what it creates.
    [self runUiSmokeFormPanelScenario];
    // PRD21 (window) -- the welcome screen's line and the diagnosis dialog;
    // touches no document either.
    [self runUiSmokeSetupNoticeScenario];
    // DEC39 -- the Fichier menu's change of project, up to the reopening,
    // which the test build never does.
    [self runUiSmokeProjectSwitchScenario];

    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The rest of the run goes
    // on from the main run loop, out of this main-queue block: the agent
    // journal's scenario calls the window's real endpoint, whose backend
    // hands each call to the main queue with dispatch_sync
    // (MainThreadBackend.h), and the main queue runs nothing while this
    // block holds it. Nothing after this point needed to run inside it.
    CFRunLoopRef mainLoop = CFRunLoopGetMain();
    CFRunLoopPerformBlock(mainLoop, kCFRunLoopDefaultMode, ^{
      [self runUiSmokeAgentJournalScenario];
      // CH6 -- the Charte panel; touches no document, its library is its
      // own. Here for the same reason: its reads, writes and renders come
      // back on the main queue.
      [self runUiSmokeCharterScenario];
      [self runUiSmokeScreenPass];
      [self finishUiSmokeTests];
    });
    CFRunLoopWakeUp(mainLoop);
}

// QA pass of 8 October 2026 -- docs/qa/2026-10-08-ui-agentique.md. What
// the open cases of VISUAL_QA_CHECKLIST.md the window test can close still
// lacked: the key really typed, the case at the end of the track, the
// capture to read. Played from the main run loop (UiSmokeWaitUntil).
- (void)runUiSmokeScreenPass {
    // An exception in one scenario is that scenario's failure, named: out of
    // this block it ended the run's work without a word, and the window
    // test waited for a finish that never came.
    const auto guarded = [&](const char* name, void (^scenario)(void)) {
        @try {
            scenario();
        } @catch (NSException* exception) {
            UiSmokeCheck(false, (std::string("the screen pass's ") + name +
                                 " scenario raised " +
                                 (exception.name.UTF8String ?: "") + ": " +
                                 (exception.reason.UTF8String ?: ""))
                                    .c_str());
            while (NSWindow* sheet = self.window.attachedSheet)
                [self.window endSheet:sheet];
        }
    };
    guarded("markers", ^{
      [self runUiSmokeScreenMarkerScenario];
    });
    guarded("forms", ^{
      [self runUiSmokeScreenFormScenario];
    });
    guarded("cartons", ^{
      [self runUiSmokeScreenCartonScenario];
    });
    guarded("zoom", ^{
      [self runUiSmokeScreenZoomScenario];
    });
}

// A QA capture of the editor window, its timeline drawn as it stands.
- (void)uiQaScreenCapture:(const std::string&)name {
    if (!UiQaEnabled()) return;
    self.state->overlayDirty = true;
    UiSmokeWaitUntil([] { return false; }, 0.2);
    std::string error;
    const bool captured = CaptureUiQaWindow(self.window, name, error);
    UiSmokeCheck(
        captured,
        ("VQA captured " + name + (captured ? "" : ": " + error)).c_str(),
        ui_smoke::Needs::kVisibleWindow);
}

// D2, D6, D4 -- docs/roadmap/DIALOGUE-2026-10.md: the markers of the
// window, by the keys and the mouse, as section 5 septdecies and 5
// sexvicies of the checklist play them.
- (void)runUiSmokeScreenMarkerScenario {
    const Ulid previousTimeline = self.state->activeTimelineId;
    if (![self activateTimelineIdentifier:CMStringFromUtf8(
                                              gUiSmokeScreenTimelineId)]) {
        UiSmokeCheck(false, "D2: the screen pass's timeline opens");
        return;
    }
    NSDictionary* bindings = [self.shortcutBindings copy];
    self.shortcutBindings[@"marker.add"] = @"M";
    self.shortcutBindings[@"marker.note"] = @"Shift+M";
    self.shortcutBindings[@"playhead.resolution"] = @"Alt+M";
    self.shortcutBindings[@"mark.in"] = @"I";
    self.shortcutBindings[@"mark.out"] = @"O";
    self.shortcutBindings[@"mark.clear"] = @"Alt+X";
    [self setTimelineTool:TimelineTool::Select];
    [self setPlaybackDirection:0];
    [self fitTimelineToViewportWidth];
    self.state->playheadResolution = PlayheadResolution::Frame;
    const TimelineViewport fitted = self.state->viewport;
    const auto at = [](int64_t frame) { return RationalTime{frame, 25}; };
    const auto markers = [&]() -> const std::vector<DocumentMarker>& {
        return self.state->document.sequence.markers;
    };
    const auto edits = [&] { return self.state->editLog.AppliedCount(); };
    const auto find = [&](const RationalTime& time) -> const DocumentMarker* {
        for (const DocumentMarker& marker : markers())
            if (marker.time == time) return &marker;
        return nullptr;
    };
    // A key to the timeline, as the window hands it over.
    const auto key = [&](NSString* characters, unsigned short code,
                         NSEventModifierFlags modifiers = 0,
                         NSString* unmodified = nil) {
        SendViewKey(self.metalView, characters,
                    unmodified ?: characters.lowercaseString, code, modifiers);
        self.state->overlayDirty = true;
    };
    const auto undo = [&] { key(@"z", 6, NSEventModifierFlagCommand); };
    const auto status = [&] {
        return std::string(self.infoLabel.stringValue.UTF8String ?: "");
    };
    const auto timecode = [&](int64_t frame) {
        return std::string(
            TimelineTimecode(at(frame), [self playheadFrameRate]).UTF8String);
    };
    const auto flagOf =
        [&](const Ulid& markerId) -> std::optional<TimelineMarkerFlag> {
        for (const TimelineMarkerFlag& flag :
             VisibleTimelineMarkers(self.state->document, self.state->viewport,
                                    self.metalView.bounds.size.width))
            if (flag.marker_id == markerId) return flag;
        return std::nullopt;
    };
    // Where the pointer sits on a flag, and on the ruler's row at a frame.
    const double flagY = [&] {
        DocumentMarker probe;
        probe.time = at(0);
        Document document = self.state->document;
        document.sequence.markers = {probe};
        const auto flags = VisibleTimelineMarkers(
            document, self.state->viewport, self.metalView.bounds.size.width);
        return flags.empty()
                   ? 40.0
                   : flags.front().flag_y + kTimelineMarkerFlagHeight / 2.0;
    }();
    const auto rowPoint = [&](double x) {
        return NSMakePoint(x, self.metalView.bounds.size.height - flagY);
    };
    // A third of a frame into `frame`: the pointer lands on it whether the
    // window rounds or truncates.
    const auto frameX = [&](int64_t frame) {
        return self.state->viewport.TimeToX(at(frame)) +
               self.state->viewport.pixels_per_second / 75.0;
    };
    const auto sheet = [&]() -> CMFormPanel* {
        NSWindow* attached = self.window.attachedSheet;
        return [attached isKindOfClass:CMFormPanel.class]
                   ? (CMFormPanel*)attached
                   : nil;
    };
    const auto waitSheet = [&] {
        UiSmokeWaitUntil([&] { return sheet() != nil; }, 3.0);
        return sheet();
    };
    const auto waitNoSheet = [&] {
        return UiSmokeWaitUntil(
            [&] { return self.window.attachedSheet == nil; }, 3.0);
    };
    const auto noteField = [&](CMFormPanel* panel) -> NSTextField* {
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:panel.form ?: [NSView new]];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view.identifier isEqualToString:@"marker.note"] &&
                [view isKindOfClass:NSTextField.class])
                return (NSTextField*)view;
            [pending addObjectsFromArray:view.subviews];
        }
        return nil;
    };
    const auto button = [&](CMFormPanel* panel, NSString* title) -> NSButton* {
        for (NSButton* candidate in panel.buttons)
            if ([candidate.title isEqualToString:title]) return candidate;
        return nil;
    };
    // The text a sheet's field shows, typed or not: its field editor's
    // while it is being edited.
    const auto typeIn = [&](NSTextField* field, NSString* text) {
        if (NSText* editor = field.currentEditor) {
            editor.string = text;
            editor.selectedRange = NSMakeRange(text.length, 0);
            [(NSTextView*)editor didChangeText];
        } else {
            field.stringValue = text;
        }
    };
    const double top = [self videoHeight];

    // 5 septdecies, first case: M at rest.
    [self requestTimelinePosition:at(20)];
    size_t before = edits();
    key(@"m", 46);
    const DocumentMarker* dropped = find(at(20));
    UiSmokeCheck(
        markers().size() == 1 && dropped &&
            dropped->color == kTimelineMarkerEditorColor &&
            dropped->name == kTimelineMarkerDefaultName &&
            dropped->duration.value == 0 && edits() == before + 1 &&
            status().find("M à nouveau pour l’annoter") != std::string::npos,
        "D2: M at rest drops a blue marker at the playhead in one "
        "edit, and the status line says M again annotates it");
    if (dropped) {
        const Ulid droppedId = dropped->id;
        const auto flag = flagOf(droppedId);
        const TimelineRenderData data = [self timelineRenderData];
        bool tag = flag.has_value();
        if (flag)
            for (const TimelineMarkerFlagRow& row :
                 TimelineMarkerFlagShape(*flag))
                tag =
                    tag && UiSmokeDrawsRect(
                               data, row.x, top + row.y, row.width, row.height,
                               UiSmokeHexColor(kTimelineMarkerEditorColor));
        const bool stem =
            flag &&
            std::any_of(data.overlays.begin(), data.overlays.end(),
                        [&](const MetalDrawCommand& command) {
                            const MetalRect* rect =
                                std::get_if<MetalRect>(&command);
                            return rect && std::abs(rect->x - flag->x) < 0.6 &&
                                   rect->width <= 1.5 &&
                                   rect->y >= top + kTimelineRulerHeight;
                        });
        const MetalText* note =
            UiSmokeDrawnText(data, kTimelineMarkerDefaultName);
        UiSmokeCheck(
            tag && !stem && note && note->y >= top + kTimelineRulerHeight,
            "D2: its tag is drawn on the ruler in the editor's "
            "blue, nothing of it runs down the tracks, and "
            "« Marqueur » is written under the ruler");
        [self uiQaScreenCapture:"s0-d2-m-a-l-arret"];
        undo();
        UiSmokeCheck(markers().empty() && edits() == before,
                     "D2: Cmd+Z takes the marker back");
    }

    // M on a marker, or a double click on its flag, opens its note; Return
    // saves it.
    key(@"m", 46);
    const DocumentMarker* noted = find(at(20));
    const Ulid notedId = noted ? noted->id : Ulid{};
    before = edits();
    key(@"m", 46);
    CMFormPanel* panel = waitSheet();
    NSTextField* field = panel ? noteField(panel) : nil;
    UiSmokeCheck(panel && field && field.stringValue.length == 0 &&
                     [panel.titleLabel.stringValue
                         isEqualToString:CMStringFromUtf8("Marqueur à " +
                                                          timecode(20))] &&
                     panel.initialFirstResponder == field && edits() == before,
                 "D2: M on a marker opens its note on a sheet titled with "
                 "its time, the empty field taking the keys");
    NSString* const typed = @"Plan à revoir : l’œil… cadré ?";
    if (panel && field) {
        UiSmokeWaitUntil([&] { return panel.isKeyWindow; }, 2.0);
        typeIn(field, typed);
        if (panel.isKeyWindow) PostApplicationKey(panel, @"\r", 36);
        const bool saved = waitNoSheet();
        const DocumentMarker* named = self.state->document.FindMarker(notedId);
        UiSmokeCheck(
            saved && named &&
                named->name == TimelineMarkerNameFromNote(typed.UTF8String) &&
                named->name == std::string(typed.UTF8String) &&
                edits() == before + 1,
            "D2: Return saves the note as typed, accents and "
            "punctuation kept, in one edit",
            ui_smoke::Needs::kKeyWindow);
        if (!saved) {
            [button(panel, @"Enregistrer") performClick:nil];
            waitNoSheet();
        }
        const TimelineRenderData data = [self timelineRenderData];
        const MetalText* note = UiSmokeDrawnText(data, typed.UTF8String);
        UiSmokeCheck(note && note->y >= top + kTimelineRulerHeight,
                     "D2: the paused playhead on it writes the note under "
                     "the ruler");
        [self uiQaScreenCapture:"s1-d2-note-sous-la-regle"];
    }
    if (const auto flag = flagOf(notedId)) {
        before = edits();
        [self requestTimelinePosition:at(0)];
        SendTimelineDoubleClick(self.metalView, rowPoint(flag->x));
        CMFormPanel* opened = waitSheet();
        NSTextField* openedField = opened ? noteField(opened) : nil;
        UiSmokeCheck(opened && openedField &&
                         [openedField.stringValue isEqualToString:typed] &&
                         self.state->requestedPosition == at(20),
                     "D2: a double click on the flag sends the playhead to "
                     "it and opens its note, the note in the field");
        if (opened) {
            UiSmokeWaitUntil([&] { return opened.isKeyWindow; }, 2.0);
            typeIn(openedField, @"Abandonné");
            if (opened.isKeyWindow) PostApplicationKey(opened, @"\x1b", 53);
            const bool closed = waitNoSheet();
            const DocumentMarker* kept =
                self.state->document.FindMarker(notedId);
            UiSmokeCheck(closed && kept &&
                             kept->name == std::string(typed.UTF8String) &&
                             edits() == before,
                         "D2: Escape closes the note and keeps the one "
                         "saved",
                         ui_smoke::Needs::kKeyWindow);
            if (!closed) {
                [button(opened, @"Annuler") performClick:nil];
                waitNoSheet();
            }
        }
    }

    // Shift+M off a marker: the note first; Annuler adds nothing, and
    // Enregistrer adds the marker and its note as one undo.
    [self requestTimelinePosition:at(70)];
    before = edits();
    const size_t count = markers().size();
    key(@"M", 46, NSEventModifierFlagShift, @"m");
    panel = waitSheet();
    UiSmokeCheck(panel && noteField(panel) &&
                     !button(panel, @"Supprimer le marqueur") &&
                     [panel.titleLabel.stringValue
                         isEqualToString:CMStringFromUtf8("Marqueur à " +
                                                          timecode(70))] &&
                     markers().size() == count && edits() == before,
                 "D2: Shift+M off a marker asks its note first, on the same "
                 "sheet without « Supprimer le marqueur », nothing added yet");
    if (panel) {
        [button(panel, @"Annuler") performClick:nil];
        waitNoSheet();
    }
    UiSmokeCheck(markers().size() == count && edits() == before &&
                     self.window.attachedSheet == nil,
                 "D2: Annuler adds no marker");
    key(@"M", 46, NSEventModifierFlagShift, @"m");
    panel = waitSheet();
    if (panel) {
        typeIn(noteField(panel), @"Reprise du son");
        [button(panel, @"Enregistrer") performClick:nil];
        waitNoSheet();
    }
    const DocumentMarker* annotated = find(at(70));
    UiSmokeCheck(annotated && annotated->name == "Reprise du son" &&
                     markers().size() == count + 1 && edits() == before + 1,
                 "D2: Enregistrer adds the marker with its note in one edit");
    undo();
    UiSmokeCheck(
        !find(at(70)) && markers().size() == count && edits() == before,
        "D2: one Cmd+Z takes the marker and its note back");
    key(@"z", 6, NSEventModifierFlagCommand | NSEventModifierFlagShift);
    const Ulid laterId = find(at(70)) ? find(at(70))->id : Ulid{};

    // Shift+arrows go from marker to marker and stop at both ends; the
    // arrows alone still go from cut to cut.
    {
        [self requestTimelinePosition:at(0)];
        std::vector<int64_t> reached;
        const auto press = [&](unsigned short code, NSEventModifierFlags held) {
            key(code == 125 ? @"\uf701" : @"\uf700", code,
                held | NSEventModifierFlagFunction |
                    NSEventModifierFlagNumericPad);
            reached.push_back(self.state->requestedPosition.to_frames(25, 1));
        };
        press(125, NSEventModifierFlagShift);
        press(125, NSEventModifierFlagShift);
        press(125, NSEventModifierFlagShift);
        const std::string afterLast = status();
        press(126, NSEventModifierFlagShift);
        press(126, NSEventModifierFlagShift);
        const std::string beforeFirst = status();
        press(125, 0);
        press(125, 0);
        press(126, 0);
        const std::vector<int64_t> expected = {20, 70, 70, 20, 20, 50, 100, 50};
        std::string seen;
        for (const int64_t frame : reached) seen += " " + std::to_string(frame);
        UiSmokeCheck(
            reached == expected &&
                afterLast.find("Pas de marqueur après") != std::string::npos &&
                beforeFirst.find("Pas de marqueur avant") != std::string::npos,
            ("D2: Shift+Down and Shift+Up go from marker to marker "
             "and stop at the last and the first, saying so; Down "
             "and Up alone go from cut to cut (frames" +
             seen + ")")
                .c_str());
    }

    // A click on a flag with each of the five tools; a click beside it, on
    // the ruler, is the timeline's as before.
    {
        const std::array<std::pair<TimelineTool, const char*>, 5> tools = {
            std::pair{TimelineTool::Select, "Sélection"},
            std::pair{TimelineTool::Hand, "Main"},
            std::pair{TimelineTool::Zoom, "Zoom"},
            std::pair{TimelineTool::Cut, "Lame"},
            std::pair{TimelineTool::Trim, "Trim"}};
        std::string missed;
        for (const auto& [tool, name] : tools) {
            [self setTimelineTool:tool];
            self.state->viewport = fitted;
            [self requestTimelinePosition:at(0)];
            const auto flag = flagOf(laterId);
            if (flag) {
                const NSPoint point = rowPoint(flag->x);
                SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDown,
                                  point, 0);
                SendTimelineMouse(self.metalView, NSEventTypeLeftMouseUp, point,
                                  0);
            }
            if (!flag || self.state->requestedPosition != at(70))
                missed += std::string(" ") + name;
        }
        UiSmokeCheck(missed.empty(),
                     ("D2: a click on a flag sends the playhead to its "
                      "marker with each of the five tools" +
                      (missed.empty() ? "" : " -- missed with" + missed))
                         .c_str());
        [self setTimelineTool:TimelineTool::Select];
        self.state->viewport = fitted;
        [self requestTimelinePosition:at(0)];
        before = edits();
        const double besideX = frameX(35);
        const bool clear = !HitTestTimelineMarker(
            self.state->document, self.state->viewport, besideX, flagY,
            self.metalView.bounds.size.width);
        SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDown,
                          rowPoint(besideX), 0);
        SendTimelineMouse(self.metalView, NSEventTypeLeftMouseUp,
                          rowPoint(besideX), 0);
        UiSmokeCheck(clear &&
                         self.state->requestedPosition.to_frames(25, 1) == 35 &&
                         edits() == before && self.window.attachedSheet == nil,
                     "D2: a click on the ruler beside the flags moves the "
                     "playhead there, as before the markers");
    }

    // The flag under the pointer lights up and shows its note; by the
    // right edge, the note goes to the left of the tag.
    if (const auto flag = flagOf(notedId)) {
        self.state->viewport = fitted;
        [self requestTimelinePosition:at(0)];
        SendTimelineMouse(self.metalView, NSEventTypeMouseMoved,
                          rowPoint(flag->x), 0);
        const TimelineRenderData data = [self timelineRenderData];
        const auto shape = TimelineMarkerFlagShape(*flag);
        const bool lit =
            !shape.empty() &&
            UiSmokeDrawsRect(data, shape.front().x, top + shape.front().y,
                             shape.front().width, 2.0, ui::theme::kTextPrimary);
        const MetalText* note = UiSmokeDrawnText(data, typed.UTF8String);
        UiSmokeCheck(self.state->hoveredMarkerId == notedId && lit && note &&
                         note->x > flag->x,
                     "D2: the flag under the pointer lights up with a clear "
                     "line and shows its note, right of the tag");
        [self uiQaScreenCapture:"s2-d2-survol"];
    }
    {
        // Two frames from the end of the film, at the right of the view.
        DocumentMarker edge;
        edge.time = at(148);
        edge.name = "Une note posée tout près du bord droit";
        edge.color = kTimelineMarkerEditorColor;
        EditError error = EditError::None;
        std::string message;
        [self applyAndPersistTimelineOperation:AddMarkerOperation{edge, -1}
                                         error:error
                                       message:message];
        const auto flag = flagOf(edge.id);
        if (flag)
            SendTimelineMouse(self.metalView, NSEventTypeMouseMoved,
                              rowPoint(flag->x), 0);
        const MetalText* note =
            UiSmokeDrawnText([self timelineRenderData], edge.name);
        UiSmokeCheck(
            flag && self.state->hoveredMarkerId == edge.id && note &&
                note->x < flag->x &&
                note->x + note->max_width <= self.metalView.bounds.size.width,
            "D2: by the right edge, the note of the flag under the "
            "pointer goes to the left of the tag, whole");
        [self uiQaScreenCapture:"s3-d2-survol-bord-droit"];
        SendTimelineMouse(self.metalView, NSEventTypeMouseMoved,
                          NSMakePoint(frameX(5), 20.0), 0);
        UiSmokeCheck(self.state->hoveredMarkerId.empty(),
                     "D2: the pointer gone, no flag stays lit");
        undo();
    }

    // « Supprimer le marqueur », from the Timeline menu and from the note:
    // Cmd+Z brings the marker back with its note.
    {
        NSMenuItem* item = nil;
        for (NSMenuItem* menu in NSApp.mainMenu.itemArray)
            for (NSMenuItem* entry in menu.submenu.itemArray)
                if (entry.action == @selector(menuRemoveMarker:)) item = entry;
        [self requestTimelinePosition:at(20)];
        before = edits();
        if (item) [NSApp sendAction:item.action to:item.target from:item];
        const bool removed = !self.state->document.FindMarker(notedId) &&
                             edits() == before + 1 &&
                             status() == "Marqueur supprimé";
        undo();
        const DocumentMarker* back = self.state->document.FindMarker(notedId);
        UiSmokeCheck(
            item && [item.title isEqualToString:@"Supprimer le marqueur"] &&
                removed && back && back->name == std::string(typed.UTF8String),
            "D2: « Supprimer le marqueur » of the menu removes the "
            "marker under the playhead; Cmd+Z brings it back with "
            "its note");
        before = edits();
        key(@"m", 46);
        panel = waitSheet();
        if (NSButton* remove = button(panel, @"Supprimer le marqueur"))
            [remove performClick:nil];
        waitNoSheet();
        const bool gone =
            !self.state->document.FindMarker(notedId) && edits() == before + 1;
        undo();
        back = self.state->document.FindMarker(notedId);
        UiSmokeCheck(
            gone && back && back->name == std::string(typed.UTF8String),
            "D2: the note's « Supprimer le marqueur » removes it; "
            "Cmd+Z brings it back with its note");
    }

    // Alt+M: the playhead between frame and sample, said on the status
    // line; the three keys are set in the keyboard panel.
    {
        [self requestTimelinePosition:at(0)];
        [self.window makeFirstResponder:self.metalView];
        key(@"µ", 46, NSEventModifierFlagOption, @"m");
        const bool sample =
            self.state->playheadResolution == PlayheadResolution::Sample &&
            status().find("Échantillon 48 kHz (⌥M)") != std::string::npos;
        key(@"µ", 46, NSEventModifierFlagOption, @"m");
        const bool frame =
            self.state->playheadResolution == PlayheadResolution::Frame &&
            status().find("Image (⌥M)") != std::string::npos;
        UiSmokeCheck(sample && frame,
                     "D2: Alt+M switches the playhead to the sample and back "
                     "to the frame, and the status line says which");
        [self editKeyboardShortcuts:nil];
        CMFormPanel* keys = waitSheet();
        NSDictionary* fields = self.keyboardEditingFields;
        const auto holds = [&](NSString* identifier, NSString* value) {
            NSTextField* shortcut = fields[identifier];
            return shortcut && [shortcut.stringValue isEqualToString:value];
        };
        UiSmokeCheck(keys && holds(@"marker.add", @"M") &&
                         holds(@"marker.note", @"Shift+M") &&
                         holds(@"playhead.resolution", @"Alt+M"),
                     "D2: the keyboard panel lists the three keys, each in "
                     "its field");
        if (keys) {
            [button(keys, @"Annuler") performClick:nil];
            waitNoSheet();
        }
    }

    // D6 -- Option-drag on a point stretches it, right then left; Escape
    // gives the gesture up.
    if (const auto flag = flagOf(notedId)) {
        const NSPoint grip = rowPoint(flag->x);
        const auto stretch = [&](double toX, bool escape) {
            SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDown, grip,
                              NSEventModifierFlagOption);
            SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDragged,
                              rowPoint(toX), NSEventModifierFlagOption);
            const std::string during = status();
            const TimelineRenderData drawn = [self timelineRenderData];
            if (escape) key(@"\x1b", 53);
            SendTimelineMouse(self.metalView, NSEventTypeLeftMouseUp,
                              rowPoint(toX), NSEventModifierFlagOption);
            return std::pair{during, drawn};
        };
        before = edits();
        const auto [rightStatus, rightDrawn] = stretch(frameX(35), false);
        const DocumentMarker* right = self.state->document.FindMarker(notedId);
        const bool bodyFollowed = std::any_of(
            rightDrawn.overlays.begin(), rightDrawn.overlays.end(),
            [&](const MetalDrawCommand& command) {
                const MetalRect* rect = std::get_if<MetalRect>(&command);
                return rect && std::abs(rect->x - flag->flag_x) < 0.01 &&
                       std::abs(rect->x + rect->width -
                                self.state->viewport.TimeToX(at(35))) < 0.01;
            });
        UiSmokeCheck(
            bodyFollowed &&
                rightStatus ==
                    "MARQUEUR  " + timecode(20) + " → " + timecode(35) &&
                right && right->time == at(20) && right->duration == at(15) &&
                edits() == before + 1,
            "D6: Option-drag to the right stretches the tag to the pointer "
            "during the gesture, the status line giving both timecodes, and "
            "makes a span of one edit");
        undo();
        const DocumentMarker* point = self.state->document.FindMarker(notedId);
        UiSmokeCheck(
            point && point->time == at(20) && point->duration.value == 0,
            "D6: one Cmd+Z makes it a point again");
        before = edits();
        stretch(frameX(10), false);
        const DocumentMarker* left = self.state->document.FindMarker(notedId);
        UiSmokeCheck(left && left->time == at(10) && left->duration == at(10) &&
                         edits() == before + 1,
                     "D6: Option-drag to the left makes the span run from "
                     "the pointer to the marker");
        undo();
        before = edits();
        stretch(frameX(30), true);
        const DocumentMarker* given = self.state->document.FindMarker(notedId);
        UiSmokeCheck(given && given->time == at(20) &&
                         given->duration.value == 0 && edits() == before &&
                         !self.state->markerStretch,
                     "D6: Escape during the gesture gives it up");

        // Its ends: the end, then the start, then one onto the other.
        const auto drag = [&](double fromX, double toX) {
            SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDown,
                              rowPoint(fromX), NSEventModifierFlagOption);
            SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDragged,
                              rowPoint(toX), NSEventModifierFlagOption);
            SendTimelineMouse(self.metalView, NSEventTypeLeftMouseUp,
                              rowPoint(toX), NSEventModifierFlagOption);
            return self.state->document.FindMarker(notedId);
        };
        before = edits();
        drag(flag->x, frameX(40));
        const auto spanned = flagOf(notedId);
        const DocumentMarker* moved =
            spanned ? drag(spanned->end_x - 1.0, frameX(45)) : nullptr;
        const bool endMoved =
            moved && moved->time == at(20) && moved->duration == at(25);
        moved = drag(flag->x, frameX(15));
        const bool startMoved =
            moved && moved->time == at(15) && moved->duration == at(30);
        const auto whole = flagOf(notedId);
        moved = whole ? drag(whole->end_x - 1.0, frameX(15)) : nullptr;
        const bool collapsed =
            moved && moved->time == at(15) && moved->duration.value == 0;
        UiSmokeCheck(
            endMoved && startMoved && collapsed && edits() == before + 4,
            "D6: Option-drag on a span's end moves its end, on its "
            "start its start, and brought onto the other end it is "
            "a point again, one edit each");
        for (int step = 0; step < 4; ++step) undo();
    }

    // Inside a span, the paused playhead shows its note, and M drops a point
    // there which its own tag picks.
    if (const auto flag = flagOf(notedId)) {
        SendTimelineMouse(self.metalView, NSEventTypeMouseMoved,
                          NSMakePoint(frameX(5), 20.0), 0);
        SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDown,
                          rowPoint(flag->x), NSEventModifierFlagOption);
        SendTimelineMouse(self.metalView, NSEventTypeLeftMouseDragged,
                          rowPoint(frameX(45)), NSEventModifierFlagOption);
        SendTimelineMouse(self.metalView, NSEventTypeLeftMouseUp,
                          rowPoint(frameX(45)), NSEventModifierFlagOption);
        [self requestTimelinePosition:at(30)];
        const bool noteShown = UiSmokeDrawnText([self timelineRenderData],
                                                typed.UTF8String) != nullptr;
        before = edits();
        key(@"m", 46);
        const DocumentMarker* inside = find(at(30));
        const Ulid insideId = inside ? inside->id : Ulid{};
        const auto insideFlag = flagOf(insideId);
        const auto picked =
            insideFlag
                ? HitTestTimelineMarker(self.state->document,
                                        self.state->viewport, insideFlag->x,
                                        flagY, self.metalView.bounds.size.width)
                : std::nullopt;
        UiSmokeCheck(noteShown && inside && inside->duration.value == 0 &&
                         edits() == before + 1 &&
                         self.window.attachedSheet == nil && picked &&
                         *picked == insideId,
                     "D6: the paused playhead inside a span shows its note; "
                     "M there drops a point marker, which its own tag picks");
        [self uiQaScreenCapture:"s4-d6-passage-et-point"];
        undo();
        undo();
    }

    // I and O, then Shift+M: the note is about that passage.
    {
        [self requestTimelinePosition:at(60)];
        key(@"i", 34);
        [self requestTimelinePosition:at(80)];
        key(@"o", 31);
        const bool ranged = [self hasValidTimelineRange];
        const RationalTime in = ranged ? *self.state->timelineIn : at(0);
        const RationalTime out = ranged ? *self.state->timelineOut : at(0);
        before = edits();
        key(@"M", 46, NSEventModifierFlagShift, @"m");
        panel = waitSheet();
        const std::string title =
            "Marqueur de " +
            std::string(
                TimelineTimecode(in, [self playheadFrameRate]).UTF8String) +
            " à " +
            std::string(
                TimelineTimecode(out, [self playheadFrameRate]).UTF8String);
        const bool titled =
            panel && [panel.titleLabel.stringValue
                         isEqualToString:CMStringFromUtf8(title)];
        if (panel) {
            typeIn(noteField(panel), @"Passage entre I et O");
            [button(panel, @"Enregistrer") performClick:nil];
            waitNoSheet();
        }
        const DocumentMarker* passage = find(in);
        UiSmokeCheck(ranged && in == at(60) && titled && passage &&
                         passage->duration == out.sub(in) &&
                         passage->name == "Passage entre I et O" &&
                         edits() == before + 1,
                     ("D6: I, O, then Shift+M: the note is announced « " +
                      title + " » and the marker covers the range")
                         .c_str());
        if (passage) undo();
        key(@"≈", 7, NSEventModifierFlagOption, @"x");
    }

    // D4 -- the agent's question in orange, beside the editor's blue and an
    // agent's ordinary yellow; answered in its note, resolved by removing
    // it, and the agent reads both back.
    {
        const McpToolRegistry registry;
        const auto call = [&](const char* tool, const std::string& json) {
            mcp_json::Value arguments;
            std::string error;
            mcp_json::Value::Parse(json, arguments, error);
            return registry.Call(*self.chatBackend, tool, arguments);
        };
        {
            // AX1 -- what the agent reads is the timeline the window shows
            // and its edits change, whichever the project opened on.
            const McpToolCallOutcome whole = call("describe", "{}");
            mcp_json::Value read;
            std::string error;
            const mcp_json::Value* sequence =
                whole.ok &&
                        mcp_json::Value::Parse(whole.result_json, read, error)
                    ? read.Find("sequence")
                    : nullptr;
            const mcp_json::Value* id =
                sequence ? sequence->Find("id") : nullptr;
            bool listedActive = false;
            if (const mcp_json::Value* timelines = read.Find("timelines");
                timelines && timelines->IsArray())
                for (const mcp_json::Value& timeline : timelines->AsArray()) {
                    const mcp_json::Value* entry = timeline.Find("id");
                    const mcp_json::Value* active = timeline.Find("active");
                    if (entry && entry->IsString() && active &&
                        active->IsBool() && active->AsBool())
                        listedActive =
                            entry->AsString() == gUiSmokeScreenTimelineId;
                }
            UiSmokeCheck(id && id->IsString() &&
                             id->AsString() == gUiSmokeScreenTimelineId &&
                             listedActive,
                         "AX1: describe, from the window, describes the "
                         "timeline it shows and lists it as the active one");
        }
        const McpToolCallOutcome question = call(
            "add_marker",
            R"({"name":"Pourquoi ce plan ici ?","time":{"value":100,"rate":25},)"
            R"("category":"question"})");
        const McpToolCallOutcome ordinary = call(
            "add_marker",
            R"({"name":"Repère de l'agent","time":{"value":120,"rate":25}})");
        const DocumentMarker* asked = find(at(100));
        const DocumentMarker* plain = find(at(120));
        const DocumentMarker* editor = self.state->document.FindMarker(notedId);
        const Ulid askedId = asked ? asked->id : Ulid{};
        self.state->viewport = fitted;
        [self requestTimelinePosition:at(0)];
        const TimelineRenderData data = [self timelineRenderData];
        const auto drawnIn = [&](const DocumentMarker* marker,
                                 const ui::theme::Color& color) {
            const auto flag = marker ? flagOf(marker->id) : std::nullopt;
            if (!flag) return false;
            for (const TimelineMarkerFlagRow& row :
                 TimelineMarkerFlagShape(*flag))
                if (!UiSmokeDrawsRect(data, row.x, top + row.y, row.width,
                                      row.height, color))
                    return false;
            return true;
        };
        UiSmokeCheck(
            question.ok && ordinary.ok && asked &&
                asked->color == kTimelineMarkerAgentColor &&
                asked->category == kTimelineMarkerAgentCategory &&
                drawnIn(asked, UiSmokeHexColor(kTimelineMarkerAgentColor)) &&
                drawnIn(editor, UiSmokeHexColor(kTimelineMarkerEditorColor)) &&
                drawnIn(plain, ui::theme::kMarker),
            "D4: the agent's question is drawn in its orange, the editor's "
            "marker in blue and an agent's ordinary marker in yellow");
        [self uiQaScreenCapture:"s5-d4-trois-marqueurs"];

        [self requestTimelinePosition:at(100)];
        before = edits();
        key(@"M", 46, NSEventModifierFlagShift, @"m");
        panel = waitSheet();
        NSTextField* answer = panel ? noteField(panel) : nil;
        const bool asking =
            answer &&
            [answer.stringValue isEqualToString:@"Pourquoi ce plan ici ?"];
        if (panel) {
            typeIn(answer, @"Réponse : on le garde pour le raccord");
            [button(panel, @"Enregistrer") performClick:nil];
            waitNoSheet();
        }
        const DocumentMarker* answered =
            self.state->document.FindMarker(askedId);
        const auto describes = [&](const std::string& key,
                                   const std::string& text) {
            const McpToolCallOutcome read =
                call("describe", R"({"only":["markers"]})");
            mcp_json::Value parsed;
            std::string error;
            const bool holds =
                read.ok &&
                mcp_json::Value::Parse(read.result_json, parsed, error) &&
                UiSmokeJsonHolds(parsed, key, text);
            return holds;
        };
        UiSmokeCheck(
            asking && answered &&
                answered->name == "Réponse : on le garde pour le raccord" &&
                answered->color == kTimelineMarkerAgentColor &&
                answered->category == kTimelineMarkerAgentCategory &&
                edits() == before + 1 &&
                describes("name", "Réponse : on le garde pour le raccord"),
            "D4: Shift+M on the question opens its note; the answer "
            "is written there, the marker keeps its colour, and the "
            "agent reads it in describe");
        [self menuRemoveMarker:nil];
        const auto flag = flagOf(askedId);
        UiSmokeCheck(!self.state->document.FindMarker(askedId) && !flag &&
                         !describes("id", askedId) && describes("id", notedId),
                     "D4: removed, the question leaves the ruler and "
                     "describe");
        undo();
        undo();
        undo();
    }

    // What the pass found it leaves: no marker, no range, the bindings and
    // the timeline it was on.
    while (!markers().empty()) {
        EditError error = EditError::None;
        std::string message;
        if (![self applyAndPersistTimelineOperation:RemoveMarkerOperation{
                                                        markers().front().id}
                                              error:error
                                            message:message])
            break;
    }
    self.shortcutBindings = [bindings mutableCopy];
    self.state->viewport = fitted;
    [self activateTimelineIdentifier:CMStringFromUtf8(previousTimeline)];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md, and the decision of 8 October
// 2026 on caption fonts (AX6, section 5 quinvicies). The forms the main run
// builds and closes, played to their end: a real export through its save
// panel, the font alert before it, its progress and its « Annuler »; the
// keys a person presses on the other forms.
- (void)runUiSmokeScreenFormScenario {
    const Ulid previousTimeline = self.state->activeTimelineId;
    if (![self activateTimelineIdentifier:CMStringFromUtf8(
                                              gUiSmokeScreenTimelineId)]) {
        UiSmokeCheck(false, "PRD26: the screen pass's timeline opens");
        return;
    }
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:{0, 25}];
    const std::filesystem::path exports = gUiSmokeRoot / "qa-exports";
    std::error_code directoryError;
    std::filesystem::create_directories(exports, directoryError);
    const auto titled = [&](NSString* title) -> CMFormPanel* {
        for (NSWindow* sheet in self.window.sheets)
            if ([sheet isKindOfClass:CMFormPanel.class] &&
                ((CMFormPanel*)sheet).presented &&
                [((CMFormPanel*)sheet).titleLabel.stringValue
                    isEqualToString:title])
                return (CMFormPanel*)sheet;
        return nil;
    };
    const auto waitTitled = [&](NSString* title, double seconds) {
        UiSmokeWaitUntil([&] { return titled(title) != nil; }, seconds);
        return titled(title);
    };
    const auto waitGone = [&](NSWindow* sheet, double seconds) {
        return UiSmokeWaitUntil(
            [&] {
                return sheet == nil ||
                       ![self.window.sheets containsObject:sheet];
            },
            seconds);
    };
    const auto buttonOf = [](CMFormPanel* panel, NSString* title) -> NSButton* {
        for (NSButton* button in panel.buttons)
            if ([button.title isEqualToString:title]) return button;
        return nil;
    };
    const auto findIn = [](NSView* root, NSString* identifier) -> NSView* {
        if (root == nil) return nil;
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:root];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view.identifier isEqualToString:identifier]) return view;
            [pending addObjectsFromArray:view.subviews];
        }
        return nil;
    };
    const auto keep = [&](NSWindow* sheet, const std::string& name) {
        if (!UiQaEnabled() || sheet == nil) return;
        std::string error;
        const bool kept = WriteUiQaViewCapture(
            sheet.contentView, sheet.backgroundColor, name, error);
        UiSmokeCheck(kept,
                     ("VQA kept " + name + (kept ? "" : ": " + error)).c_str());
    };
    // The window's sheet that is neither a form nor a save panel: an alert.
    const auto alertSheet = [&]() -> NSWindow* {
        NSWindow* attached = self.window.attachedSheet;
        return attached && ![attached isKindOfClass:CMFormPanel.class] &&
                       ![attached isKindOfClass:NSSavePanel.class]
                   ? attached
                   : nil;
    };
    const auto waitAlert = [&](double seconds) {
        UiSmokeWaitUntil([&] { return alertSheet() != nil; }, seconds);
        return alertSheet();
    };
    const auto closeAlert = [&](NSWindow* alert) {
        if (alert == nil) return;
        if (NSButton* ok = UiSmokeButtonIn(alert.contentView, @"OK"))
            [ok performClick:nil];
        else
            [self.window endSheet:alert returnCode:NSAlertFirstButtonReturn];
        waitGone(alert, 3.0);
    };
    const auto styleFamily = [&](const std::string& family) {
        if (self.state->document.sequence.caption_styles.empty()) return false;
        CaptionStyle style =
            self.state->document.sequence.caption_styles.front();
        style.font_family = family;
        EditError error = EditError::None;
        std::string message;
        return static_cast<bool>([self
            applyAndPersistTimelineOperation:UpdateCaptionStyleOperation{style}
                                       error:error
                                     message:message]);
    };
    // The export's form answered, then its save panel: `resolution` and
    // `encoder` are rows of the override menus, 0 keeping the preset's.
    const auto exportTo = [&](const std::string& file, NSInteger resolution,
                              NSInteger encoder) {
        [self exportFinalVideo:nil];
        CMFormPanel* form = waitTitled(@"Exporter la vidéo finale", 3.0);
        if (form == nil) return false;
        NSView* sizes = findIn(form.form, @"export.resolution");
        NSView* encoders = findIn(form.form, @"export.encoder");
        if ([sizes isKindOfClass:NSPopUpButton.class])
            [(NSPopUpButton*)sizes selectItemAtIndex:resolution];
        if ([encoders isKindOfClass:NSPopUpButton.class])
            [(NSPopUpButton*)encoders selectItemAtIndex:encoder];
        [buttonOf(form, @"Choisir la destination…") performClick:nil];
        waitGone(form, 3.0);
        if (std::getenv("CUTMACHINE_UI_SMOKE_TRACE"))
            std::fprintf(stdout, "NOTE: export form closed, sheet %s\n",
                         NSStringFromClass([self.window.attachedSheet class])
                             .UTF8String);
        const bool answered =
            UiSmokeAnswerSavePanel(self.window, exports / file);
        if (std::getenv("CUTMACHINE_UI_SMOKE_TRACE"))
            std::fprintf(stdout, "NOTE: save panel answered %d, sheet %s\n",
                         answered ? 1 : 0,
                         NSStringFromClass([self.window.attachedSheet class])
                             .UTF8String);
        return answered;
    };
    const auto exportedSize = [&](const std::string& file) {
        std::error_code error;
        const auto size = std::filesystem::file_size(exports / file, error);
        return error ? std::uintmax_t{0} : size;
    };

    // 5 quinvicies -- cartons in a family this Mac does not have.
    const std::string missing = "QA Police Absente";
    const std::vector<CaptionFontSubstitution> substitutions =
        CheckCaptionFonts({missing});
    const CaptionFontNotice notice = CaptionFontWarning(substitutions);
    const std::string exportedAs =
        substitutions.empty() ? "" : substitutions.front().exported_as;
    const bool absent = !substitutions.empty() && styleFamily(missing);
    UiSmokeCheck(absent, "AX6: the cartons are set in a family the export "
                         "does not find on this Mac");
    if (absent) {
        const bool chosen = exportTo("police-absente-annulee.mp4", 0, 0);
        NSWindow* alert = chosen ? waitAlert(15.0) : nil;
        const std::string texts =
            alert ? UiSmokeViewTexts(alert.contentView) : "";
        const std::string said =
            "« " + missing + " » sortira en « " + exportedAs + " »";
        UiSmokeCheck(
            chosen && alert && texts.find(notice.title) != std::string::npos &&
                notice.title == "Une police de sous-titres manque à l’export" &&
                texts.find(notice.text) != std::string::npos &&
                texts.find(said) != std::string::npos &&
                UiSmokeButtonIn(alert.contentView, @"Exporter quand même") &&
                UiSmokeButtonIn(alert.contentView, @"Annuler") &&
                !self.state->exportRunning,
            ("AX6: the destination chosen, an alert says « " + notice.title +
             " » and " + said +
             ", its explanation whole, nothing exported "
             "yet")
                .c_str());
        keep(alert, "s6-ax6-alerte-police");
        if (alert) {
            UiSmokeWaitUntil([&] { return alert.isKeyWindow; }, 2.0);
            if (alert.isKeyWindow) PostApplicationKey(alert, @"\x1b", 53);
            const bool closed = waitGone(alert, 3.0);
            UiSmokeWaitUntil([&] { return self.window.attachedSheet == nil; },
                             2.0);
            UiSmokeCheck(closed && !self.state->exportRunning &&
                             self.window.attachedSheet == nil &&
                             exportedSize("police-absente-annulee.mp4") == 0,
                         "AX6: Escape answers « Annuler »: the alert goes and "
                         "nothing is exported",
                         ui_smoke::Needs::kKeyWindow);
            if (!closed) {
                [self.window endSheet:alert
                           returnCode:NSAlertSecondButtonReturn];
                waitGone(alert, 3.0);
            }
        }
        const bool again = exportTo("police-absente.mp4", 0, 0);
        alert = again ? waitAlert(15.0) : nil;
        if (NSButton* go = alert ? UiSmokeButtonIn(alert.contentView,
                                                   @"Exporter quand même")
                                 : nil)
            [go performClick:nil];
        CMFormPanel* progress = waitTitled(@"Export H.264 en cours", 5.0);
        UiSmokeCheck(alert && progress,
                     "AX6: « Exporter quand même » starts the export, under "
                     "its progress panel");
        const bool counted =
            progress && UiSmokeWaitUntil(
                            [&] {
                                NSString* line =
                                    progress.messageLabel.stringValue;
                                return [line containsString:@" images — "] &&
                                       [line hasSuffix:@"x"];
                            },
                            30.0);
        NSProgressIndicator* bar =
            [progress.form isKindOfClass:NSProgressIndicator.class]
                ? (NSProgressIndicator*)progress.form
                : nil;
        UiSmokeCheck(
            counted && bar && bar.doubleValue > 0.0,
            ("PRD26: « Export H.264 en cours » fills its bar and "
             "says « n / N images — x » (" +
             std::string(progress.messageLabel.stringValue.UTF8String ?: "") +
             ")")
                .c_str());
        keep(progress, "s7-prd26-progression-export");
        const bool done =
            UiSmokeWaitUntil([&] { return !self.state->exportRunning; }, 120.0);
        NSWindow* finished = waitAlert(5.0);
        const std::string finishedTexts =
            finished ? UiSmokeViewTexts(finished.contentView) : "";
        UiSmokeCheck(
            done && finishedTexts.find("Export terminé") != std::string::npos &&
                exportedSize("police-absente.mp4") > 0,
            "AX6: the export ends with its file and says « Export "
            "terminé »");
        closeAlert(finished);
    }

    // Cartons in Helvetica Neue: no alert. A long export, for the settings
    // shown over its progress and for « Annuler ».
    if (styleFamily("system")) {
        const bool chosen = exportTo("annule-uhd.mp4", 3, 2);
        CMFormPanel* progress = waitTitled(@"Export H.264 en cours", 5.0);
        UiSmokeCheck(chosen && progress && alertSheet() == nil &&
                         self.state->exportRunning,
                     "AX6: cartons in Helvetica Neue (« system »): no alert, "
                     "the export starts at once");
        if (progress) {
            [self configureSequence:nil];
            CMFormPanel* settings = waitTitled(@"Réglages de séquence", 3.0);
            const bool over = settings && progress.presented &&
                              self.state->exportRunning &&
                              self.window.attachedSheet == settings;
            keep(settings, "s8-prd26-reglages-pendant-export");
            if (settings) [buttonOf(settings, @"Annuler") performClick:nil];
            waitGone(settings, 3.0);
            NSString* line = [progress.messageLabel.stringValue copy];
            const bool resumed =
                self.window.attachedSheet == progress && progress.presented &&
                UiSmokeWaitUntil(
                    [&] {
                        return ![progress.messageLabel.stringValue
                            isEqualToString:line];
                    },
                    15.0);
            UiSmokeCheck(over && resumed,
                         "PRD26: during an export, « Réglages de séquence » "
                         "shows over its progress, which goes on once it "
                         "is closed");
            const bool running = self.state->exportRunning;
            [buttonOf(progress, @"Annuler") performClick:nil];
            const bool stopped = UiSmokeWaitUntil(
                [&] { return !self.state->exportRunning; }, 30.0);
            waitGone(progress, 3.0);
            UiSmokeWaitUntil([&] { return alertSheet() != nil; }, 1.0);
            UiSmokeCheck(running && stopped && alertSheet() == nil &&
                             self.window.attachedSheet == nil,
                         "PRD26: « Annuler » on the progress stops the "
                         "export, and no « Export terminé » follows");
            closeAlert(alertSheet());
        }
    }

    // « Nouvelle timeline »: the proposed name selected, Escape creates
    // nothing; an emptied name and Return keep the sheet where it is.
    {
        const size_t timelines = self.state->project.timelines.size();
        [self createTimelinePressed:nil];
        CMFormPanel* panel = waitTitled(@"Nouvelle timeline", 3.0);
        UiSmokeWaitUntil([&] { return panel.isKeyWindow; }, 2.0);
        NSView* found = findIn(panel.form, @"timeline.new.name");
        NSTextField* name =
            [found isKindOfClass:NSTextField.class] ? (NSTextField*)found : nil;
        NSText* editor = name.currentEditor;
        const bool selected =
            editor && name.stringValue.length > 0 &&
            editor.selectedRange.location == 0 &&
            editor.selectedRange.length == name.stringValue.length;
        if (panel.isKeyWindow) PostApplicationKey(panel, @"\x1b", 53);
        const bool closed = waitGone(panel, 3.0);
        UiSmokeCheck(panel && selected && closed &&
                         self.state->project.timelines.size() == timelines,
                     "PRD26: « Nouvelle timeline » opens with the proposed "
                     "name selected, and Escape closes it creating nothing",
                     ui_smoke::Needs::kKeyWindow);
        if (!closed && panel) {
            [buttonOf(panel, @"Annuler") performClick:nil];
            waitGone(panel, 3.0);
        }

        [self createTimelinePressed:nil];
        panel = waitTitled(@"Nouvelle timeline", 3.0);
        UiSmokeWaitUntil([&] { return panel.isKeyWindow; }, 2.0);
        found = findIn(panel.form, @"timeline.new.name");
        name =
            [found isKindOfClass:NSTextField.class] ? (NSTextField*)found : nil;
        const NSRect frame = panel.frame;
        if (NSText* typing = name.currentEditor) typing.string = @"";
        if (panel.isKeyWindow) PostApplicationKey(panel, @"\r", 36);
        UiSmokeWaitUntil(
            [&] { return panel.errorLabel.stringValue.length > 0; }, 2.0);
        UiSmokeCheck(
            panel && panel.presented && self.window.attachedSheet == panel &&
                self.window.sheets.count == 1 &&
                [panel.errorLabel.stringValue
                    isEqualToString:@"Donnez un nom à la timeline."] &&
                !panel.errorIcon.hidden && NSEqualRects(panel.frame, frame) &&
                self.state->project.timelines.size() == timelines,
            "PRD26: an emptied name and Return keep the sheet where it was, "
            "« Donnez un nom à la timeline. » under the field, no second "
            "alert",
            ui_smoke::Needs::kKeyWindow);
        if (panel) {
            [buttonOf(panel, @"Annuler") performClick:nil];
            waitGone(panel, 3.0);
        }
    }

    // « Nouvelle timeline avec la sélection »: the same rule, then the
    // timeline, the selected rush laid in it.
    {
        [self refreshBinControlsSelecting:@"__all__"];
        const BOOL grid = self.mediaViewToggle.selectedSegment == 1;
        NSInteger row = -1;
        for (NSUInteger index = 0; index < self.visibleMediaIds.count; ++index)
            if (self.state->document.FindLibraryMedia(
                    self.visibleMediaIds[index].UTF8String ?: "")) {
                row = static_cast<NSInteger>(index);
                break;
            }
        const std::string rush =
            row >= 0
                ? std::string(self.visibleMediaIds[static_cast<NSUInteger>(row)]
                                      .UTF8String
                                  ?: "")
                : std::string();
        if (row >= 0) {
            if (grid)
                self.mediaCollection.selectionIndexPaths =
                    [NSSet setWithObject:[NSIndexPath indexPathForItem:row
                                                             inSection:0]];
            else
                [self.mediaTable
                        selectRowIndexes:[NSIndexSet
                                             indexSetWithIndex:static_cast<
                                                                   NSUInteger>(
                                                                   row)]
                    byExtendingSelection:NO];
        }
        const size_t timelines = self.state->project.timelines.size();
        [self createTimelineFromSelectedMedia:nil];
        CMFormPanel* panel =
            waitTitled(@"Nouvelle timeline avec la sélection", 3.0);
        NSView* found = findIn(panel.form, @"timeline.from_selection.name");
        NSTextField* name =
            [found isKindOfClass:NSTextField.class] ? (NSTextField*)found : nil;
        UiSmokeSetFieldText(name, @"   ");
        [buttonOf(panel, @"Créer") performClick:nil];
        const bool refused = panel.presented &&
                             panel.errorLabel.stringValue.length > 0 &&
                             self.state->project.timelines.size() == timelines;
        UiSmokeSetFieldText(name, @"QA depuis la sélection");
        const std::string typedName =
            std::string("; the field held « ") +
            (name.stringValue.UTF8String ?: "") + " », " +
            (name.currentEditor ? "edited" : "not edited") +
            (panel.presented ? ", the sheet up" : ", the sheet gone");
        [buttonOf(panel, @"Créer") performClick:nil];
        waitGone(panel, 3.0);
        // The timeline is made once the sheet has gone.
        UiSmokeWaitUntil(
            [&] { return self.state->project.timelines.size() > timelines; },
            3.0);
        size_t laid = 0;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (clip.source_id == rush) ++laid;
        const bool created =
            self.state->project.timelines.size() == timelines + 1 &&
            self.state->document.sequence.name == "QA depuis la sélection" &&
            laid > 0;
        const std::string shown =
            std::to_string(self.state->project.timelines.size() - timelines) +
            " more timeline(s), the window on « " +
            self.state->document.sequence.name + " », " + std::to_string(laid) +
            " clip(s) of the rush, error « " +
            (panel.errorLabel.stringValue.UTF8String ?: "") + " »" + typedName +
            (panel.presented ? "; after « Créer », the sheet up"
                             : "; after « Créer », the sheet gone") +
            ", the bin line « " +
            (self.binSummaryLabel.stringValue.UTF8String ?: "") + " »";
        UiSmokeCheck(row >= 0 && panel && refused && created,
                     ("PRD26: « Nouvelle timeline avec la sélection » refuses "
                      "an empty name on its error line, then creates the "
                      "timeline with the rush in it (rush " +
                      std::string(row >= 0 ? "shown" : "not shown") +
                      ", panel " + (panel ? "open" : "absent") + ", refused " +
                      (refused ? "yes" : "no") + ", created " +
                      (created ? "yes" : "no") + "; " + shown + ")")
                         .c_str());
        if (created) [self menuUndo:nil];
        [self activateTimelineIdentifier:CMStringFromUtf8(
                                             gUiSmokeScreenTimelineId)];
    }

    // « Réglages de séquence »: « Appliquer » closes, Record takes the
    // format; a refusal of the engine stays on the sheet.
    {
        const DocumentSequence before = self.state->document.sequence;
        [self configureSequence:nil];
        CMFormPanel* panel = waitTitled(@"Réglages de séquence", 3.0);
        NSView* format = findIn(panel.form, @"sequence.format");
        if ([format isKindOfClass:NSPopUpButton.class])
            [(NSPopUpButton*)format selectItemAtIndex:3];
        [buttonOf(panel, @"Appliquer") performClick:nil];
        const bool closed = waitGone(panel, 3.0);
        [self requestTimelinePosition:{25, 25}];
        UiSmokeCheck(panel && closed &&
                         self.state->document.sequence.width == 1080 &&
                         self.state->document.sequence.height == 1920,
                     "PRD26: « Appliquer » closes « Réglages de séquence » "
                     "and the timeline takes the vertical format");
        for (int tick = 0; tick < 6; ++tick)
            [self presentNearestFrameAtDeadline:NO];
        [self uiQaScreenCapture:"s9-prd26-sequence-verticale"];
        if (closed) [self menuUndo:nil];
        [self configureSequence:nil];
        panel = waitTitled(@"Réglages de séquence", 3.0);
        NSView* found = findIn(panel.form, @"sequence.name");
        if ([found isKindOfClass:NSTextField.class])
            UiSmokeSetFieldText((NSTextField*)found, @"");
        [buttonOf(panel, @"Appliquer") performClick:nil];
        const bool kept = panel.presented &&
                          self.window.attachedSheet == panel &&
                          panel.errorLabel.stringValue.length > 0;
        keep(panel, "s10-prd26-reglages-refus");
        UiSmokeCheck(
            kept && self.state->document.sequence.width == before.width &&
                self.state->document.sequence.name == before.name,
            ("PRD26: a refusal of the engine stays on « Réglages de "
             "séquence », on its error line (« " +
             std::string(panel.errorLabel.stringValue.UTF8String ?: "") + " »)")
                .c_str());
        if (panel) {
            [buttonOf(panel, @"Annuler") performClick:nil];
            waitGone(panel, 3.0);
        }
    }

    // A sheet open, Cmd+, opens nothing else.
    {
        [self configureSequence:nil];
        CMFormPanel* panel = waitTitled(@"Réglages de séquence", 3.0);
        UiSmokeWaitUntil([&] { return panel.isKeyWindow; }, 2.0);
        NSDictionary* fields = self.keyboardEditingFields;
        if (panel.isKeyWindow)
            PostApplicationKey(panel, @",", 43, kCGEventFlagMaskCommand);
        UiSmokeWaitUntil([] { return false; }, 0.5);
        std::string sheets;
        for (NSWindow* sheet in self.window.sheets)
            sheets += std::string(" ") +
                      NSStringFromClass(sheet.class).UTF8String +
                      ([sheet isKindOfClass:CMFormPanel.class]
                           ? std::string(" « ") +
                                 (((CMFormPanel*)sheet)
                                          .titleLabel.stringValue.UTF8String
                                      ?: "") +
                                 " »"
                           : std::string());
        UiSmokeCheck(panel && self.window.sheets.count == 1 &&
                         self.window.attachedSheet == panel &&
                         self.keyboardEditingFields == fields &&
                         titled(@"Raccourcis clavier") == nil,
                     ("PRD26: with a sheet open, Cmd+, opens nothing else "
                      "(sheets:" +
                      sheets + ")")
                         .c_str(),
                     ui_smoke::Needs::kKeyWindow);
        if (panel) {
            [buttonOf(panel, @"Annuler") performClick:nil];
            waitGone(panel, 3.0);
        }
    }

    // « Raccourcis clavier » made taller: the list grows, the keyboard stays
    // at the top.
    {
        [self editKeyboardShortcuts:nil];
        CMFormPanel* panel = waitTitled(@"Raccourcis clavier", 3.0);
        NSScrollView* list = nil;
        for (NSView* view in panel.form.subviews)
            if ([view isKindOfClass:NSScrollView.class])
                list = (NSScrollView*)view;
        NSButton* space = UiSmokeButtonIn(panel.form, @"Espace");
        NSView* keyboard = space.superview;
        const auto keyboardTop = [&] {
            const NSRect inForm = [keyboard convertRect:keyboard.bounds
                                                 toView:panel.form];
            return NSMaxY(panel.form.bounds) - NSMaxY(inForm);
        };
        const double listBefore = list.frame.size.height;
        const double topBefore = keyboard ? keyboardTop() : -1.0;
        NSRect frame = panel.frame;
        frame.size.height += 200.0;
        frame.origin.y -= 200.0;
        [panel setFrame:frame display:YES];
        UiSmokeWaitUntil([] { return false; }, 0.2);
        const double grown = list.frame.size.height - listBefore;
        // The sheet's own margins take their share of the height.
        UiSmokeCheck(list && keyboard && grown >= 180.0 && grown <= 200.0 &&
                         std::abs(keyboardTop() - topBefore) < 1.0,
                     ("PRD26: « Raccourcis clavier » made 200 points taller: "
                      "its list grows by " +
                      std::to_string(static_cast<int>(grown)) +
                      ", the keyboard stays at the top")
                         .c_str());
        keep(panel, "s11-prd26-raccourcis-agrandi");
        if (panel) {
            [buttonOf(panel, @"Annuler") performClick:nil];
            waitGone(panel, 3.0);
        }
    }

    // « Moteur IA » with a good URL: it closes, the Agent panel reloads its
    // engine, and the agent journal keeps its lines.
    {
        NSUserDefaults* stored = NSUserDefaults.standardUserDefaults;
        NSArray<NSString*>* keys = @[
            @"AiEngine.Provider.v1", @"AiEngine.Model.v1",
            @"AiEngine.BaseUrl.v1", @"AiEngine.AcpCommand.v1"
        ];
        NSMutableDictionary* saved = [NSMutableDictionary dictionary];
        for (NSString* key in keys)
            if (id value = [stored objectForKey:key]) saved[key] = value;
        std::vector<std::string> before;
        UiSmokeAgentJournalRows(self.chatPanelView, before);
        if (before.empty()) {
            // Played alone, the pass has no agent behind it: a line of the
            // journal as an agent's edit leaves one.
            chat::ChatTranscriptEntry line;
            line.kind = chat::ChatEntryKind::AgentJournal;
            line.text = "10:00:00 Régler l'opacité — 1 plan modifié";
            line.tool_ok = true;
            line.journal_id = 1;
            [self.chatPanelView showAgentJournal:{line}];
            UiSmokeAgentJournalRows(self.chatPanelView, before);
        }
        [self.chatPanelView showConfigurationWindow];
        CMFormPanel* panel = waitTitled(@"Moteur IA", 3.0);
        NSView* menu = findIn(panel.form, @"ai_engine.provider");
        NSView* url = findIn(panel.form, @"ai_engine.base_url");
        if ([menu isKindOfClass:NSPopUpButton.class] &&
            [url isKindOfClass:NSTextField.class]) {
            NSPopUpButton* protocols = (NSPopUpButton*)menu;
            [protocols
                selectItemAtIndex:ai_engine::MenuIndexOf(
                                      ai_engine::EngineProtocol::Ollama)];
            [protocols sendAction:protocols.action to:protocols.target];
            UiSmokeSetFieldText((NSTextField*)url, @"http://127.0.0.1:11434");
        }
        [buttonOf(panel, @"Enregistrer") performClick:nil];
        const bool closed = waitGone(panel, 3.0);
        std::vector<std::string> after;
        UiSmokeAgentJournalRows(self.chatPanelView, after);
        UiSmokeCheck(
            panel && closed &&
                [[stored stringForKey:@"AiEngine.Provider.v1"]
                    isEqualToString:@"ollama"] &&
                [[stored stringForKey:@"AiEngine.BaseUrl.v1"]
                    isEqualToString:@"http://127.0.0.1:11434"] &&
                !before.empty() && after == before,
            ("PRD26: « Moteur IA » with a good URL closes, the engine is "
             "the one saved, and the agent journal keeps its " +
             std::to_string(before.size()) + " lines (closed " +
             (closed ? "yes" : "no") + ", provider « " +
             ([stored stringForKey:@"AiEngine.Provider.v1"].UTF8String ?: "") +
             " », URL « " +
             ([stored stringForKey:@"AiEngine.BaseUrl.v1"].UTF8String ?: "") +
             " », " + std::to_string(after.size()) + " lines after)")
                .c_str());
        for (NSString* key in keys) {
            if (saved[key])
                [stored setObject:saved[key] forKey:key];
            else
                [stored removeObjectForKey:key];
        }
    }

    // « Diagnostic de l'installation »: Return and Escape close it.
    {
        bool closedBoth = true;
        std::string how;
        for (NSString* key in @[ @"\r", @"\x1b" ]) {
            [self showSetupDiagnostic:nil];
            CMFormPanel* panel =
                waitTitled(@"Diagnostic de l'installation", 20.0);
            UiSmokeWaitUntil([&] { return panel.isKeyWindow; }, 2.0);
            const bool keyed = panel.isKeyWindow;
            if (panel.isKeyWindow)
                PostApplicationKey(panel, key,
                                   [key isEqualToString:@"\r"] ? 36 : 53);
            const bool closed = panel && waitGone(panel, 3.0);
            how += std::string(" ") +
                   ([key isEqualToString:@"\r"] ? "Return" : "Escape") + ": " +
                   (panel ? "shown" : "never shown") +
                   (keyed ? ", key" : ", not key") +
                   (closed ? ", closed;" : ", still open;");
            closedBoth = closedBoth && closed;
            if (!closed && panel) {
                [panel endWithResponse:NSModalResponseCancel];
                waitGone(panel, 3.0);
            }
        }
        UiSmokeCheck(closedBoth,
                     ("PRD26: Return and Escape close « Diagnostic de "
                      "l'installation » (" +
                      how + ")")
                         .c_str(),
                     ui_smoke::Needs::kKeyWindow);
    }

    [self activateTimelineIdentifier:CMStringFromUtf8(previousTimeline)];
}

// D7 -- docs/roadmap/DIALOGUE-2026-10.md, section 5 octodecies: the text of
// a carton typed in the Inspector, with the keys a person presses --
// posted as the keyboard posts them, so these checks need the window that
// has the keys.
- (void)runUiSmokeScreenCartonScenario {
    const Ulid previousTimeline = self.state->activeTimelineId;
    if (![self activateTimelineIdentifier:CMStringFromUtf8(
                                              gUiSmokeScreenTimelineId)]) {
        UiSmokeCheck(false, "D7: the screen pass's timeline opens");
        return;
    }
    [self setPlaybackDirection:0];
    [self setTimelineTool:TimelineTool::Select];
    [self fitTimelineToViewportWidth];
    NSDictionary* bindings = [self.shortcutBindings copy];
    self.shortcutBindings[@"marker.add"] = @"M";
    self.shortcutBindings[@"mark.in"] = @"I";
    self.shortcutBindings[@"mark.out"] = @"O";
    self.shortcutBindings[@"play.forward"] = @"L";
    self.shortcutBindings[@"play.reverse"] = @"J";
    self.shortcutBindings[@"play.stop"] = @"K";
    const auto needs = ui_smoke::Needs::kKeyWindow;
    NSTextField* field = self.inspectorView.captionTextField;
    const auto carton = [&](size_t index) {
        return self.state->document.FindClip(gUiSmokeScreenCartonIds[index]);
    };
    const auto edits = [&] { return self.state->editLog.AppliedCount(); };
    const auto typing = [&] {
        return field.currentEditor != nil &&
               self.window.firstResponder == field.currentEditor;
    };
    const auto pump = [](double seconds) {
        UiSmokeWaitUntil([] { return false; }, seconds);
    };
    // A key as the keyboard posts it, to the window that has the keys.
    const auto press = [&](NSString* characters, unsigned short code,
                           CGEventFlags flags = 0) {
        PostApplicationKey(self.window, characters, code, flags);
        pump(0.06);
    };
    const auto type = [&](NSString* text) {
        [text
            enumerateSubstringsInRange:NSMakeRange(0, text.length)
                               options:
                                   NSStringEnumerationByComposedCharacterSequences
                            usingBlock:^(NSString* letter, NSRange, NSRange,
                                         BOOL*) {
                              PostApplicationKey(
                                  self.window, letter,
                                  [letter isEqualToString:@" "] ? 49 : 0);
                            }];
        pump(0.2);
    };
    const auto recordShows = [&](NSString* text) {
        for (int tick = 0; tick < 4; ++tick)
            [self presentNearestFrameAtDeadline:NO];
        return [self.programCaptionView.captionText isEqualToString:text];
    };
    const auto drawnOnTimeline = [&](const std::string& text) {
        return UiSmokeDrawnText([self timelineRenderData], text) != nullptr;
    };
    const auto select = [&](size_t index) {
        self.state->interaction->SelectClip(gUiSmokeScreenCartonIds[index]);
        [self requestTimelinePosition:carton(index)->timeline_in.add({5, 25})];
        [self updateSelectionInfo];
        [self.window makeFirstResponder:self.metalView];
    };
    const auto keys = std::string(
        "CARTON · Entrée passe à la ligne · ⌘ Entrée enregistre · Tab carton "
        "suivant · Échap annule");
    const auto status = [&] {
        return std::string(self.infoLabel.stringValue.UTF8String ?: "");
    };
    // In the Inspector, which is in front, the field taking the keys with
    // its whole text selected, and the status line giving the keys.
    const auto focused = [&](size_t index) {
        NSText* editor = field.currentEditor;
        return self.rightDockPanel.selectedSlot == ui::PanelSlot::Inspector &&
               typing() &&
               [field.stringValue
                   isEqualToString:CMStringFromUtf8(
                                       carton(index)->caption_text)] &&
               editor.selectedRange.location == 0 &&
               editor.selectedRange.length == field.stringValue.length &&
               status() == keys;
    };
    const auto leave = [&] {
        if (typing()) press(@"\x1b", 53);
        [self.window makeFirstResponder:self.metalView];
    };
    std::array<std::string, 3> words;
    for (size_t index = 0; index < words.size(); ++index)
        words[index] = carton(index)->caption_text;
    // The carton row on the timeline, aimed the way the pointer hits it.
    const auto cartonPoint = [&](size_t index) {
        const auto rows = TimelineTracksInDisplayOrder(self.state->document);
        const DocumentTrack* track = self.state->document.FindTrackForClip(
            gUiSmokeScreenCartonIds[index]);
        const auto row = std::find(rows.begin(), rows.end(), track);
        const DocumentClip* clip = carton(index);
        const double x = (self.state->viewport.TimeToX(clip->timeline_in) +
                          self.state->viewport.TimeToX(
                              clip->timeline_in.add(clip->duration))) /
                         2.0;
        const double y = kTimelineRulerHeight +
                         static_cast<double>(row - rows.begin()) *
                             self.state->viewport.track_height +
                         self.state->viewport.track_height * 0.5;
        return NSMakePoint(x, self.metalView.bounds.size.height - y);
    };

    // Four ways in, the Agent's window up: a double click on a carton of one
    // word, Return, the Timeline menu, the carton's own menu. The Agent was
    // the dock's other tab, in front of the Inspector, until ON3 gave it a
    // window of its own; the keys still go to the Inspector's field, in the
    // editor's window.
    {
        [self showAgentWindow:YES focus:NO];
        const DocumentClip before = *carton(2);
        const size_t was = edits();
        SendTimelineDoubleClick(self.metalView, cartonPoint(2));
        pump(0.2);
        const DocumentClip* after = carton(2);
        UiSmokeCheck(focused(2) && after->timeline_in == before.timeline_in &&
                         after->duration == before.duration && edits() == was,
                     "D7: a double click on a carton of one word, the Agent's "
                     "window up, puts the keys in the Inspector's field, its "
                     "text selected and the keys said on the status line; "
                     "the carton neither moved nor was trimmed",
                     needs);
        [self uiQaScreenCapture:"s12-d7-double-clic"];
        leave();

        select(0);
        press(@"\r", 36);
        UiSmokeCheck(focused(0),
                     "D7: Return on the selected carton does the "
                     "same",
                     needs);
        leave();

        NSMenuItem* timelineEntry = nil;
        for (NSMenuItem* menu in NSApp.mainMenu.itemArray)
            for (NSMenuItem* entry in menu.submenu.itemArray)
                if (entry.action == @selector(menuEditCartonText:))
                    timelineEntry = entry;
        select(1);
        if (timelineEntry)
            [NSApp sendAction:timelineEntry.action
                           to:timelineEntry.target
                         from:timelineEntry];
        UiSmokeCheck(timelineEntry && focused(1),
                     "D7: « Modifier le texte du carton » of the Timeline "
                     "menu does the same",
                     needs);
        leave();

        select(1);
        const NSPoint point = cartonPoint(1);
        NSEvent* rightClick = [NSEvent
            mouseEventWithType:NSEventTypeRightMouseDown
                      location:[self.metalView convertPoint:point toView:nil]
                 modifierFlags:0
                     timestamp:NSProcessInfo.processInfo.systemUptime
                  windowNumber:self.window.windowNumber
                       context:nil
                   eventNumber:0
                    clickCount:1
                      pressure:1.0];
        NSMenu* menu = [self.metalView menuForEvent:rightClick];
        NSMenuItem* own = nil;
        for (NSMenuItem* entry in menu.itemArray)
            if (entry.action == @selector(menuEditCartonText:)) own = entry;
        if (own) [NSApp sendAction:own.action to:own.target from:own];
        UiSmokeCheck(own && [own.title isEqualToString:@"Modifier le texte"] &&
                         focused(1),
                     "D7: « Modifier le texte » of the carton's own menu does "
                     "the same",
                     needs);
        leave();
        [self showAgentWindow:NO focus:NO];
    }

    // Typed: Record follows each key in the carton's style, Return breaks the
    // line on the picture, and the timeline keeps the old words until saved.
    {
        select(0);
        press(@"\r", 36);
        const size_t was = edits();
        bool followed = typing();
        NSString* typed = @"";
        for (NSString* letter in
             @[ @"B", @"o", @"n", @"j", @"o", @"u", @"r" ]) {
            PostApplicationKey(self.window, letter, 0);
            pump(0.06);
            typed = [typed stringByAppendingString:letter];
            followed = followed && recordShows(typed);
        }
        press(@"\r", 36);
        type(@"à tous");
        const bool broken =
            [field.currentEditor.string isEqualToString:@"Bonjour\nà tous"] &&
            recordShows(@"Bonjour\nà tous");
        UiSmokeCheck(followed && broken && edits() == was &&
                         carton(0)->caption_text == words[0] &&
                         drawnOnTimeline(words[0]),
                     "D7: each key shows at once on Record; Return breaks the "
                     "line there; the timeline keeps the old words, nothing "
                     "saved",
                     needs);
        [self uiQaScreenCapture:"s13-d7-frappe-deux-lignes"];

        // Escape gives it up; the keys are the timeline's again.
        press(@"\x1b", 53);
        const bool given =
            !typing() &&
            [field.stringValue isEqualToString:CMStringFromUtf8(words[0])] &&
            recordShows(CMStringFromUtf8(words[0])) &&
            self.window.firstResponder == self.metalView && edits() == was;
        const std::string after =
            std::string("field « ") + (field.stringValue.UTF8String ?: "") +
            " », Record « " +
            (self.programCaptionView.captionText.UTF8String ?: "") +
            " », typing " + (typing() ? "yes" : "no") + ", keys to " +
            NSStringFromClass(self.window.firstResponder.class).UTF8String;
        press(@" ", 49);
        const bool playing = self.state->playbackDirection != 0;
        [self setPlaybackDirection:0];
        UiSmokeCheck(given && playing,
                     ("D7: Escape gives up what was typed, on Record too, and "
                      "the space bar plays the timeline again (" +
                      after + (playing ? ", played" : ", not played") + ")")
                         .c_str(),
                     needs);
    }

    // While typing, the keys the timeline uses are letters of the text.
    {
        select(0);
        press(@"\r", 36);
        const size_t markers = self.state->document.sequence.markers.size();
        const auto in = self.state->timelineIn;
        type(@" jklmio");
        const bool written =
            [field.currentEditor.string hasSuffix:@" jklmio"] && typing();
        UiSmokeCheck(
            written && self.state->playbackDirection == 0 &&
                self.state->document.sequence.markers.size() == markers &&
                self.state->timelineIn == in,
            "D7: while typing, space, J, K, L, M, I and O are "
            "written in the field and drive neither playback nor "
            "markers nor In",
            needs);
        leave();
    }

    // Cmd+Return or a click on the timeline saves; the style stays; one
    // Cmd+Z gives the old words back; a long text stays reachable.
    {
        select(0);
        const CaptionStyle style =
            self.state->document.sequence.caption_styles.front();
        const Ulid group = carton(0)->caption_group_id;
        press(@"\r", 36);
        size_t was = edits();
        type(@"Texte enregistré");
        press(@"\r", 36, kCGEventFlagMaskCommand);
        const bool saved =
            !typing() && self.window.firstResponder == self.metalView &&
            carton(0)->caption_text == "Texte enregistré" &&
            carton(0)->caption_group_id == group &&
            self.state->document.sequence.caption_styles.front().font_family ==
                style.font_family &&
            self.state->document.sequence.caption_styles.front().font_size ==
                style.font_size &&
            self.state->document.sequence.caption_styles.front().color ==
                style.color &&
            edits() == was + 1 && drawnOnTimeline("Texte enregistré") &&
            status().find("Texte enregistré") != std::string::npos;
        SendViewKey(self.metalView, @"z", @"z", 6, NSEventModifierFlagCommand);
        const bool back =
            carton(0)->caption_text == words[0] &&
            [field.stringValue isEqualToString:CMStringFromUtf8(words[0])];
        UiSmokeCheck(saved && back,
                     "D7: Cmd+Return saves the text as one edit, shown on the "
                     "timeline and the status line, the style untouched; one "
                     "Cmd+Z gives the old words back, the field following",
                     needs);

        select(1);
        press(@"\r", 36);
        was = edits();
        type(@"Enregistré au clic");
        const NSPoint empty = NSMakePoint(
            self.state->viewport.TimeToX({140, 25}),
            self.metalView.bounds.size.height -
                (kTimelineRulerHeight +
                 static_cast<double>(
                     TimelineTracksInDisplayOrder(self.state->document)
                         .size()) *
                     self.state->viewport.track_height +
                 4.0));
        SendTimelineMouseGesture(self.metalView, empty, empty);
        pump(0.2);
        UiSmokeCheck(!typing() &&
                         carton(1)->caption_text == "Enregistré au clic" &&
                         edits() == was + 1,
                     "D7: a click on the timeline saves what was typed", needs);
        if (carton(1)->caption_text != words[1]) [self menuUndo:nil];

        select(1);
        press(@"\r", 36);
        type(@"une");
        for (NSString* line in
             @[ @"deux", @"trois", @"quatre", @"cinq", @"six" ]) {
            press(@"\r", 36);
            type(line);
        }
        NSTextView* editor = (NSTextView*)field.currentEditor;
        bool reachable = false;
        if (editor && editor.layoutManager) {
            const NSRange glyphs = [editor.layoutManager
                glyphRangeForCharacterRange:NSMakeRange(
                                                editor.string.length - 1, 1)
                       actualCharacterRange:nullptr];
            NSRect last = [editor.layoutManager
                boundingRectForGlyphRange:glyphs
                          inTextContainer:editor.textContainer];
            last = NSOffsetRect(last, editor.textContainerOrigin.x,
                                editor.textContainerOrigin.y);
            reachable =
                NSContainsRect(NSInsetRect(editor.visibleRect, -1, -1), last);
        }
        UiSmokeCheck(reachable && [editor.string hasSuffix:@"\nsix"],
                     "D7: a text of six lines keeps its last line in view "
                     "in the field",
                     needs);
        [self uiQaScreenCapture:"s14-d7-six-lignes"];
        leave();
    }

    // Tab saves and walks on, Shift+Tab back; at the end of the track Tab
    // stays and says so. The buttons walk the same way, greyed at the ends,
    // and save first.
    {
        select(0);
        press(@"\r", 36);
        size_t was = edits();
        type(@"Changé par Tab");
        press(@"\t", 48);
        const bool walked =
            carton(0)->caption_text == "Changé par Tab" &&
            self.state->interaction->SelectedClipId() ==
                gUiSmokeScreenCartonIds[1] &&
            self.state->requestedPosition == carton(1)->timeline_in &&
            typing() &&
            [field.stringValue isEqualToString:CMStringFromUtf8(words[1])] &&
            edits() == was + 1;
        press(@"\t", 48, kCGEventFlagMaskShift);
        const bool backed = self.state->interaction->SelectedClipId() ==
                                gUiSmokeScreenCartonIds[0] &&
                            typing();
        press(@"\t", 48);
        press(@"\t", 48);
        press(@"\t", 48);
        const bool stayed =
            self.state->interaction->SelectedClipId() ==
                gUiSmokeScreenCartonIds[2] &&
            typing() &&
            status().find("Dernier carton de la piste") != std::string::npos;
        UiSmokeCheck(walked && backed && stayed,
                     "D7: Tab saves and goes to the next carton, the playhead "
                     "on its first frame and the keys in its text; Shift+Tab "
                     "comes back; at the end of the track Tab stays and the "
                     "status line says so",
                     needs);
        leave();
        if (carton(0)->caption_text != words[0]) [self menuUndo:nil];

        NSButton* previous =
            UiSmokeButtonIn(self.inspectorView, @"◀ Précédent");
        NSButton* next = UiSmokeButtonIn(self.inspectorView, @"Suivant ▶");
        select(0);
        [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
        const bool firstEnds =
            previous && next && !previous.enabled && next.enabled;
        [next performClick:nil];
        const bool onSecond =
            self.state->interaction->SelectedClipId() ==
                gUiSmokeScreenCartonIds[1] &&
            self.state->requestedPosition == carton(1)->timeline_in;
        [next performClick:nil];
        const bool lastEnds = self.state->interaction->SelectedClipId() ==
                                  gUiSmokeScreenCartonIds[2] &&
                              previous.enabled && !next.enabled;
        select(1);
        press(@"\r", 36);
        was = edits();
        type(@"Avant le bouton");
        [previous performClick:nil];
        pump(0.1);
        const bool savedFirst = carton(1)->caption_text == "Avant le bouton" &&
                                self.state->interaction->SelectedClipId() ==
                                    gUiSmokeScreenCartonIds[0] &&
                                typing() && edits() == was + 1;
        UiSmokeCheck(firstEnds && onSecond && lastEnds && savedFirst,
                     "D7: « ◀ Précédent » and « Suivant ▶ » walk the same "
                     "way, greyed at the two ends; pressed while typing, they "
                     "save first",
                     needs);
        leave();
        if (carton(1)->caption_text != words[1]) [self menuUndo:nil];
    }

    // Cmd+A, C, X, V and Z are the field's: a word copied, cut and pasted,
    // no clip copied or pasted, and Cmd+Z does not reach the montage. The
    // clipboard of whoever runs this is put back after.
    {
        NSPasteboard* board = NSPasteboard.generalPasteboard;
        NSMutableArray<NSPasteboardItem*>* kept = [NSMutableArray array];
        for (NSPasteboardItem* item in board.pasteboardItems) {
            NSPasteboardItem* copy = [NSPasteboardItem new];
            for (NSPasteboardType kind in item.types)
                if (NSData* data = [item dataForType:kind])
                    [copy setData:data forType:kind];
            [kept addObject:copy];
        }
        select(0);
        press(@"\r", 36);
        const size_t was = edits();
        const auto clips = [&] {
            size_t count = 0;
            for (const DocumentTrack& track :
                 self.state->document.sequence.tracks)
                count += track.clips.size();
            return count;
        };
        const size_t clipsBefore = clips();
        NSText* editor = field.currentEditor;
        editor.selectedRange = NSMakeRange(0, 7);  // « Premier »
        press(@"c", 8, kCGEventFlagMaskCommand);
        const bool copied =
            [[board stringForType:NSPasteboardTypeString]
                isEqualToString:@"Premier"] &&
            ![board.types containsObject:kCutmachineTimelinePasteboardType];
        press(@"x", 7, kCGEventFlagMaskCommand);
        const bool cut =
            [field.currentEditor.string isEqualToString:@" carton"];
        editor = field.currentEditor;
        editor.selectedRange = NSMakeRange(editor.string.length, 0);
        pump(0.3);
        press(@"v", 9, kCGEventFlagMaskCommand);
        const bool pasted =
            [field.currentEditor.string isEqualToString:@" cartonPremier"];
        NSTextView* pasting = (NSTextView*)field.currentEditor;
        const std::string undoState =
            std::string(" -- undo: ") +
            (pasting.undoManager ? "a manager" : "no manager") +
            (pasting.allowsUndo ? ", allowed" : ", not allowed") +
            (pasting.undoManager.canUndo ? ", can undo" : ", nothing to undo");
        pump(0.3);
        press(@"z", 6, kCGEventFlagMaskCommand);
        // Undone: the paste, or the cut with it when the two were typed
        // within one of the text's undo groups.
        const bool undone =
            [field.currentEditor.string isEqualToString:@" carton"] ||
            [field.currentEditor.string isEqualToString:@"Premier carton"];
        press(@"a", 0, kCGEventFlagMaskCommand);
        const bool all = field.currentEditor.selectedRange.length ==
                         field.currentEditor.string.length;
        const std::string seen =
            std::string(copied ? "" : " not copied") + (cut ? "" : " not cut") +
            (pasted ? "" : " not pasted") + (undone ? "" : " not undone") +
            (all ? "" : " not all selected") +
            (edits() == was ? "" : " the montage edited") +
            (clips() == clipsBefore ? "" : " a clip pasted") + " -- field « " +
            (field.currentEditor.string.UTF8String ?: "") + " », clipboard « " +
            ([board stringForType:NSPasteboardTypeString].UTF8String ?: "") +
            " »" + undoState;
        UiSmokeCheck(copied && cut && pasted && undone && all &&
                         edits() == was && clips() == clipsBefore,
                     ("D7: in the carton's field, Cmd+C, Cmd+X, Cmd+V, Cmd+Z "
                      "and Cmd+A work on its text; no clip is copied or "
                      "pasted, the montage is not undone (" +
                      seen + ")")
                         .c_str(),
                     needs);
        leave();

        // The same in the media search and in the agent's input.
        NSTextField* agentInput = nil;
        NSMutableArray<NSView*>* pending =
            [NSMutableArray arrayWithObject:self.chatPanelView];
        while (pending.count > 0) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if ([view isKindOfClass:NSTextField.class] &&
                [((NSTextField*)view).placeholderString
                    isEqualToString:@"Décrire une modification…"])
                agentInput = (NSTextField*)view;
            [pending addObjectsFromArray:view.subviews];
        }
        bool elsewhere = agentInput != nil && self.mediaSearchField != nil;
        NSMutableArray<NSTextField*>* others = [NSMutableArray array];
        if (self.mediaSearchField) [others addObject:self.mediaSearchField];
        if (agentInput) [others addObject:agentInput];
        for (NSTextField* other in others) {
            // ON3 -- the agent's input is in its own window, which takes
            // the keys while it is typed in.
            if (other == agentInput) [self showAgentWindow:YES focus:YES];
            [other.window makeFirstResponder:other];
            UiSmokeSetFieldText(other, @"");
            type(@"plan large");
            press(@"a", 0, kCGEventFlagMaskCommand);
            press(@"c", 8, kCGEventFlagMaskCommand);
            const bool copiedHere =
                [[board stringForType:NSPasteboardTypeString]
                    isEqualToString:@"plan large"];
            press(@"z", 6, kCGEventFlagMaskCommand);
            elsewhere = elsewhere && copiedHere && edits() == was &&
                        clips() == clipsBefore;
            UiSmokeSetFieldText(other, @"");
            if (other == agentInput) [self showAgentWindow:NO focus:NO];
            [self.window makeFirstResponder:self.metalView];
        }
        if (NSSearchField* search = self.mediaSearchField)
            [NSApp sendAction:search.action to:search.target from:search];
        UiSmokeCheck(elsewhere,
                     "D7: the same keys are the media search's and the agent "
                     "input's while they are typed in, the montage left alone",
                     needs);
        [board clearContents];
        if (kept.count > 0) [board writeObjects:kept];
        [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    }

    // « œ », « ’ », « … » and a no-break space before « ? »: exact in the
    // field, on Record, and in what the export draws.
    {
        select(2);
        press(@"\r", 36);
        NSString* special = @"Cœur d’acier… vraiment\u00a0?";
        type(special);
        press(@"\r", 36, kCGEventFlagMaskCommand);
        [self requestTimelinePosition:carton(2)->timeline_in.add({10, 25})];
        const bool exact =
            carton(2)->caption_text == std::string(special.UTF8String) &&
            [field.stringValue isEqualToString:special] && recordShows(special);
        std::string exportError;
        const std::filesystem::path movie = gUiSmokeRoot / "qa-d7-carton.mp4";
        const bool exported =
            UiQaEnabled() &&
            UiQaExportMovie(self.state->document, gUiSmokeProjectPath, movie,
                            exportError) &&
            UiQaExtractFrame(movie, 110,
                             gUiQaOutputDirectory / "s15-d7-carton-export.png");
        if (exported)
            gUiQaCaptures.push_back(
                {"s15-d7-carton-export", "s15-d7-carton-export.png", 0, 0});
        UiSmokeCheck(exact && (!UiQaEnabled() || exported),
                     ("D7: « œ », « ’ », « … » and a no-break space are kept "
                      "exactly in the field and on Record; the exported "
                      "frame is kept for reading" +
                      (exportError.empty() ? "" : ": " + exportError))
                         .c_str(),
                     needs);
        [self uiQaScreenCapture:"s16-d7-carton-record"];
        if (carton(2)->caption_text != words[2]) [self menuUndo:nil];
    }

    self.shortcutBindings = [bindings mutableCopy];
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    [self.window makeFirstResponder:self.metalView];
    [self activateTimelineIdentifier:CMStringFromUtf8(previousTimeline)];
}

// Visual QA of 1 October 2026 -- the zoom and scroll of the timeline, as
// the pass of that day left them to be seen: notches of a wheel sent to the
// timeline as macOS sends them (section « Passe du 1er octobre »).
- (void)runUiSmokeScreenZoomScenario {
    const Ulid previousTimeline = self.state->activeTimelineId;
    if (![self activateTimelineIdentifier:CMStringFromUtf8(
                                              gUiSmokeScreenTimelineId)]) {
        UiSmokeCheck(false, "zoom: the screen pass's timeline opens");
        return;
    }
    [self setPlaybackDirection:0];
    [self fitTimelineToViewportWidth];
    const TimelineViewport fitted = self.state->viewport;
    const double width = self.metalView.bounds.size.width;
    const double height = self.metalView.bounds.size.height;
    const double header = self.state->viewport.header_width;
    const RationalTime duration = self.state->duration;
    const auto at = [](int64_t frame) { return RationalTime{frame, 25}; };
    const auto x = [&](int64_t frame) {
        return self.state->viewport.TimeToX(at(frame));
    };
    const auto pps = [&] { return self.state->viewport.pixels_per_second; };
    const auto filmWidth = [&] {
        return self.state->viewport.TimeToX(duration) -
               self.state->viewport.TimeToX(at(0));
    };
    // The pointer over the first track, at the left or the right of it.
    const double trackY = height - (kTimelineRulerHeight + 10.0);
    const NSPoint left = NSMakePoint(header + 12.0, trackY);
    const NSPoint right = NSMakePoint(width - 12.0, trackY);
    // A notch: a line of a wheel, `lines` signed as the wheel turns.
    const auto notch = [&](NSPoint point, int32_t lines,
                           CGEventFlags flags = 0) {
        SendTimelineWheel(self.metalView, point, lines, flags);
        self.state->overlayDirty = true;
    };
    std::string trace;
    char text[160];

    // Which way of the wheel zooms in, read once.
    [self requestTimelinePosition:at(60)];
    const double fittedScale = pps();
    notch(left, 1);
    const int32_t in = pps() > fittedScale ? 1 : -1;
    if (in < 0) notch(left, -1);
    self.state->viewport = fitted;
    self.state->zoomAnchor.reset();

    // Pointer anywhere: the playhead keeps its place on screen.
    {
        [self requestTimelinePosition:at(60)];
        const double held = x(60);
        double scale = pps();
        bool kept = true;
        bool grew = true;
        for (const NSPoint& pointer : {left, right, left, right}) {
            notch(pointer, in);
            kept = kept && std::abs(x(60) - held) < 0.5;
            grew = grew && pps() > scale;
            scale = pps();
        }
        std::snprintf(text, sizeof text,
                      " (playhead at %.1f then %.1f points, %.0f to %.0f "
                      "points a second)",
                      held, x(60), fittedScale, pps());
        UiSmokeCheck(kept && grew,
                     ("zoom: wheel notches zoom in around the playhead, which "
                      "keeps its place on screen wherever the pointer is" +
                      std::string(text))
                         .c_str());
        // Panned with Shift until it is out of sight, the first notch
        // brings it to the middle.
        int32_t way = 3;
        for (int pan = 0; pan < 400 && x(60) >= header && x(60) <= width;
             ++pan) {
            const double before = x(60);
            notch(left, way, kCGEventFlagMaskShift);
            // Stopped at the start of the film: the other way.
            if (std::abs(x(60) - before) < 0.01) way = -way;
        }
        const bool hidden = x(60) < header || x(60) > width;
        const double away = x(60);
        notch(right, in);
        const double middle = (header + width) / 2.0;
        std::snprintf(
            text, sizeof text,
            " (out of sight at %.1f, then at %.1f, the middle at "
            "%.1f, the view from %.3f s at %.0f points a second)",
            away, x(60), middle,
            static_cast<double>(self.state->viewport.view_start.value) /
                self.state->viewport.view_start.rate,
            pps());
        UiSmokeCheck(hidden && std::abs(x(60) - middle) < 1.0,
                     ("zoom: with the playhead out of sight, the first notch "
                      "brings it to the middle of the view" +
                      std::string(text))
                         .c_str());
    }

    // Out to the floor and back: the playhead returns to its place; at the
    // floor the film begins at the left edge and fills half the view.
    {
        self.state->viewport = fitted;
        self.state->zoomAnchor.reset();
        [self requestTimelinePosition:at(60)];
        for (int step = 0; step < 4; ++step) notch(right, in);
        const double startX = x(60);
        int out = 0;
        double scale = pps();
        for (; out < 60; ++out) {
            notch(right, -in);
            if (std::abs(pps() - scale) < 1e-9) break;
            scale = pps();
        }
        const double floorScale = pps();
        const double floorStart = x(0);
        const double floorFilm = filmWidth();
        const double half = (width - header) / 2.0;
        for (int step = 0; step < out; ++step) notch(right, in);
        std::snprintf(text, sizeof text,
                      " (%d notches out, the film %.1f points wide for half "
                      "a view of %.1f, starting at %.1f; back at %.0f points "
                      "a second, the playhead at %.1f for %.1f)",
                      out, floorFilm, half, floorStart, pps(), x(60), startX);
        trace = text;
        UiSmokeCheck(out > 0 && out < 60 &&
                         std::abs(floorStart - header) < 0.5 &&
                         std::abs(floorFilm - half) < 2.0,
                     ("zoom: zoomed out to the end, the wheel stops when the "
                      "film fills half the view, its start at the left edge" +
                      trace)
                         .c_str());
        UiSmokeCheck(std::abs(x(60) - startX) < 1.0,
                     ("zoom: zoomed back in as many notches, the playhead is "
                      "back where it was" +
                      trace)
                         .c_str());
        (void)floorScale;
    }

    // The bar's handle stops at the same floor; zoomed in from there with
    // the playhead at the start, the film still begins at the left edge.
    {
        self.state->viewport = fitted;
        self.state->zoomAnchor.reset();
        const TimelineZoomBarGeometry bar = CalculateTimelineZoomBarGeometry(
            self.state->viewport, duration, width);
        const double barY = kTimelineZoomBarHeight / 2.0;
        const NSPoint handle = NSMakePoint(
            bar.thumb_x + bar.thumb_width - bar.handle_width / 2.0, barY);
        SendTimelineMouseGesture(self.metalView, handle,
                                 NSMakePoint(header + 4.0, barY));
        const double half = (width - header) / 2.0;
        const double barFilm = filmWidth();
        [self requestTimelinePosition:at(0)];
        notch(left, in);
        std::snprintf(text, sizeof text,
                      " (the film %.1f points wide by the bar, for half a "
                      "view of %.1f; after a notch in it starts at %.1f)",
                      barFilm, half, x(0));
        UiSmokeCheck(barFilm >= half - 2.0 && std::abs(x(0) - header) < 0.5,
                     ("zoom: the bar's handle stops at the same floor, and a "
                      "notch in from there, the playhead at the start, keeps "
                      "the film's start at the left edge" +
                      std::string(text))
                         .c_str());
    }

    self.state->viewport = fitted;
    self.state->zoomAnchor.reset();
    self.state->overlayDirty = true;
    [self activateTimelineIdentifier:CMStringFromUtf8(previousTimeline)];
}

- (void)finishUiSmokeTests {
    if (UiQaEnabled()) {
        std::string manifestError;
        const bool wroteManifest =
            WriteUiQaManifest(self.window, self.state->document, manifestError);
        if (!wroteManifest) {
            UiSmokeCheck(false,
                         ("VQA manifest failed: " + manifestError).c_str());
        }
    }
    [self runUiSmokeImportScenario];
    // F2.4 -- the same import, asked by an agent of the window.
    [self runUiSmokeAgentIngestScenario];
    // PRD28 -- "UI smoke: 0 failure(s)" as ever, or with the checks the
    // screen did not let the run play, counted, and why.
    std::fprintf(
        stdout, "%s\n",
        ui_smoke::SummaryLine(gUiSmokeFailures, gUiSmokeNotRun).c_str());
    std::fflush(stdout);
    // -terminate: exits the process before main can return the failure count.
    // Stop the run loop and wake it with a harmless application event so
    // ctest receives a non-zero status for every failed smoke assertion.
    [NSApp stop:nil];
    NSEvent* wake =
        [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                           location:NSZeroPoint
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:self.window.windowNumber
                            context:nil
                            subtype:0
                              data1:0
                              data2:0];
    [NSApp postEvent:wake atStart:NO];
}

@end
