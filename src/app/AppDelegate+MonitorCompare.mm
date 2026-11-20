// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. What the
// Record monitor shows beside the picture (GRADE-2026-09 G6, PRD37): the
// reference still and the comparison split, the grade bypass, and the video
// scope's panel. MonitorCompare.h and VideoScopes.h decide; these methods
// relay. Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (MonitorCompare)

- (void)programVideoScopeChanged:(NSPopUpButton*)sender {
    self.state->programVideoScope = VideoScopeModeFromPreference(
        static_cast<int32_t>(sender.selectedItem.tag));
    [NSUserDefaults.standardUserDefaults
        setInteger:static_cast<NSInteger>(self.state->programVideoScope)
            forKey:kProgramVideoScopeDefaultsKey];
    self.state->overlayDirty = true;
}

// PRD37 -- ROADMAP.md. The strip along Record's bottom edge that its
// controls cover (the scope selector, the zoom, SOURCE ON), in the
// drawable's pixels: the scope panel is laid out above it
// (VideoScopes.h, LayoutVideoScopePanel).
- (double)programScopeBottomInset {
    double top = 0.0;
    for (NSView* control in @[
             self.programVideoScopePopup, self.programMonitorZoomPopup,
             self.sourceMonitorToggleButton
         ])
        if (control != nil && !control.hidden)
            top = std::max(top, static_cast<double>(NSMaxY(control.frame)));
    const double scale = self.programMonitorView.layer.contentsScale > 0.0
                             ? self.programMonitorView.layer.contentsScale
                             : 1.0;
    return top * scale;
}

- (NSURL*)referenceStillURL {
    // The smoke run keeps its still in its own folder: it must never replace
    // the one a person grabbed.
    if (AppTestHooks* hooks = TestHooks())
        if (NSURL* still = hooks->ReferenceStillURL()) return still;
    NSURL* support = [NSFileManager.defaultManager
                         URLsForDirectory:NSApplicationSupportDirectory
                                inDomains:NSUserDomainMask]
                         .firstObject;
    return [[support URLByAppendingPathComponent:@"CUTMACHINE"]
        URLByAppendingPathComponent:@"Reference.png"];
}

// The preferences, and the still they name when its file still reads as
// one this build wrote: 8-bit RGBA, unmodified since.
- (void)loadMonitorCompare {
    self.state->monitorCompare =
        [CMUiPreferences.sharedPreferences monitorCompare];
    self.state->referenceStill = nullptr;
    NSString* path = [CMUiPreferences.sharedPreferences referenceStillPath];
    NSData* data = path ? [NSData dataWithContentsOfFile:path] : nil;
    NSBitmapImageRep* bitmap =
        data ? [NSBitmapImageRep imageRepWithData:data] : nil;
    if (bitmap && bitmap.bitsPerSample == 8 && bitmap.samplesPerPixel == 4 &&
        !bitmap.isPlanar && bitmap.bitmapData != nullptr &&
        bitmap.bytesPerRow == bitmap.pixelsWide * 4) {
        auto still = std::make_shared<ReferenceStill>();
        still->width = static_cast<int32_t>(bitmap.pixelsWide);
        still->height = static_cast<int32_t>(bitmap.pixelsHigh);
        still->rgba.assign(bitmap.bitmapData,
                           bitmap.bitmapData + still->ByteSize());
        self.state->referenceStill = std::move(still);
    }
    if (!self.state->referenceStill) self.state->monitorCompare.split = false;
}

- (void)saveMonitorCompare {
    [CMUiPreferences.sharedPreferences
        setMonitorCompare:self.state->monitorCompare];
    self.state->overlayDirty = true;
    [self refreshCompareBadge];
}

- (BOOL)referenceStillFits {
    return self.state && self.state->referenceStill &&
           ReferenceFits(*self.state->referenceStill,
                         self.state->document.sequence.width,
                         self.state->document.sequence.height);
}

- (void)refreshCompareBadge {
    if (!self.programCompareBadge || !self.state) return;
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (self.state->monitorCompare.bypass_grade)
        [parts addObject:@"SANS ÉTALONNAGE"];
    if (self.state->monitorCompare.split && [self referenceStillFits])
        [parts addObject:@"RÉFÉRENCE ◀ │ ▶ ACTUEL"];
    self.programCompareBadge.stringValue =
        [parts componentsJoinedByString:@"  ·  "];
    self.programCompareBadge.hidden = parts.count == 0;
}

- (void)toggleGradeBypass:(id)sender {
    (void)sender;
    if (!self.state) return;
    self.state->monitorCompare.bypass_grade =
        !self.state->monitorCompare.bypass_grade;
    [self saveMonitorCompare];
}

- (void)toggleReferenceSplit:(id)sender {
    (void)sender;
    if (!self.state) return;
    if (!self.state->monitorCompare.split && ![self referenceStillFits]) {
        NSBeep();
        return;
    }
    self.state->monitorCompare.split = !self.state->monitorCompare.split;
    [self saveMonitorCompare];
}

// The next Record render grabs itself (see -presentNearestFrameAtDeadline:).
- (void)grabReferenceStill:(id)sender {
    (void)sender;
    if (!self.state) return;
    self.state->grabReferenceRequested = true;
    self.state->overlayDirty = true;
}

- (void)forgetReferenceStill:(id)sender {
    (void)sender;
    if (!self.state) return;
    NSString* path = [CMUiPreferences.sharedPreferences referenceStillPath];
    if (path) [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    [CMUiPreferences.sharedPreferences setReferenceStillPath:nil];
    self.state->referenceStill = nullptr;
    self.state->monitorCompare.split = false;
    [self saveMonitorCompare];
}

// What Record just showed, kept as the reference: written where the
// preferences say, then shown split.
- (BOOL)keepReferenceStill:(ReferenceStill)still {
    NSBitmapImageRep* bitmap =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                pixelsWide:still.width
                                                pixelsHigh:still.height
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:still.width * 4
                                              bitsPerPixel:32];
    if (!bitmap || bitmap.bitmapData == nullptr) return NO;
    std::copy(still.rgba.begin(), still.rgba.end(), bitmap.bitmapData);
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                       properties:@{}];
    NSURL* url = [self referenceStillURL];
    [NSFileManager.defaultManager
               createDirectoryAtURL:url.URLByDeletingLastPathComponent
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
    if (!png || ![png writeToURL:url atomically:YES]) return NO;
    [CMUiPreferences.sharedPreferences setReferenceStillPath:url.path];
    self.state->referenceStill =
        std::make_shared<const ReferenceStill>(std::move(still));
    self.state->monitorCompare.split = true;
    [self saveMonitorCompare];
    return YES;
}

@end
