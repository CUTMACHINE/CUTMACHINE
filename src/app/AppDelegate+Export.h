#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Delivery from
// the window: the final export and its presets, its progress sheet and its
// errors, the portable collect, the subtitles file, and the send to
// Resolve. Export.h, ResolveSend.h and Subtitles.h do the work; these
// methods ask, show and report. Moved out of main.mm unchanged.

@interface AppDelegate (Export)
- (void)showExportError:(const std::string&)message;
- (void)exportFinalVideo:(id)sender;
- (CMFormPanel*)exportProgressPanel:(BOOL)h264;
- (CMFormPanel*)collectProgressPanel;
- (void)exportWithPreset:(NSPopUpButton*)preset
               container:(NSPopUpButton*)container
              resolution:(NSPopUpButton*)resolution
                 cadence:(NSPopUpButton*)cadence
                 encoder:(NSPopUpButton*)encoder;
- (void)collectPortableProject:(id)sender;
- (void)exportSubtitlesPressed:(id)sender;
- (void)sendToResolvePressed:(id)sender;
@end
