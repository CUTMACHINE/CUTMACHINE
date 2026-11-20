#pragma once

// STRUCTURE-2026-11 ST1 -- docs/roadmap/STRUCTURE-2026-11.md. What only the
// window test needs, apart from what it shares with the editor
// (AppDelegatePrivate.h): ScreenCaptureKit, the sockets it speaks to the
// window's endpoint through, the run's state shared by the harness and its
// entry, and the methods of its category on AppDelegate. Until then this
// stood in AppDelegatePrivate.h under CUTMACHINE_UI_SMOKE_TEST.

#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <objc/runtime.h>
// PRD25 -- the run speaks to the window's endpoint as an agent does.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
// ScreenCaptureKit imports AVFoundation, whose Objective-C AVMediaType typedef
// collides with FFmpeg's C enum tag. This translation unit does not name that
// enum; changing only its local tag leaves every C symbol and ABI untouched.
// Included first, the FFmpeg headers are skipped when AppDelegatePrivate.h
// includes them again.
#define AVMediaType CutmachineFfmpegAVMediaType
#include <libavutil/frame.h>
#include <libavutil/parseutils.h>
#undef AVMediaType
}

#include "AppDelegatePrivate.h"
#include "SmokeReport.h"

namespace cutmachine_app {

// Defined in UiSmokeHarness.mm; its entry (UiSmokeMain.mm) and the hooks
// it installs read or set them.
extern bool gUiSmokeTesting;
extern int gUiSmokeFailures;
extern bool gUiSmokeIconMouseDown;
extern bool gUiSmokeIconDragSession;
extern bool gUiSmokeIconDragEnded;
extern NSUInteger gUiSmokeMediaDragCount;
extern int gUiSmokeDecodeReloads;
extern int gUiSmokeDecodeOpens;
extern size_t gUiSmokeWorkersAtWindowShow;
extern std::string gUiPlayProbeProject;
extern double gUiPlayProbeSeconds;
extern int gUiPlayProbeOpens;
extern int gUiPlayProbeReopens;
extern std::set<Ulid> gUiPlayProbeOpened;
extern int gUiPlayProbeTicks;
extern int gUiPlayProbeTopDrops;
extern bool gUiPlayProbeHide;
extern bool gUiPlayProbeReadAhead;
extern int64_t gUiPlayProbeByteSeconds;
extern int64_t gUiPlayProbeReappearSeconds;
extern int64_t gUiPlayProbeCutSeconds;
extern bool gUiPlayProbeReader;
extern bool gUiPlayProbeNextPiece;
extern double gUiPlayProbePerf7Seconds;
extern bool gUiSmokeClipSignageOff;
extern std::string gUiSmokeAgentJournalTime;
extern int gUiSmokeAgentEvents;
extern std::filesystem::path gUiSmokeRoot;
extern std::string gUiSmokeProjectPath;
extern std::filesystem::path gUiSmokeSaveAnswer;
extern ui_smoke::NotRun gUiSmokeNotRun;
bool PrepareUiSmokeProject(std::string& error);

}  // namespace cutmachine_app

// Implemented by UiSmokeHarness.mm; the hooks start them.
@interface AppDelegate (UiSmoke)
- (void)runUiSmokeTests;
- (void)runUiSmokeAgentJournalScenario;
- (void)finishUiSmokeTests;
- (void)runPlayProbe;
- (void)runPlayProbePerf7Phases;
// The scope panel as Record lays it out, in the monitor view's points (y
// up), for the run to hold against the controls it must not cover.
- (NSRect)programScopePanelFrame;
@end
