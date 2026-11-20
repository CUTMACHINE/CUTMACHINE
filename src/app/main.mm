// PRD8 -- docs/roadmap/PRODUIT-2026-10.md. The includes, the window's state
// and the interfaces are in AppDelegatePrivate.h, which the window test
// harness (tests/ui_smoke/UiSmokeHarness.mm) shares; the code is here.
// STRUCTURE-2026-11 ST1 -- compiled once: the window test links these very
// objects, and reaches them through AppTestHooks.h. The executables' entry
// is AppMain.mm.
#include "AppDelegatePrivate.h"
#include "CliMain.h"
#include "Generation.h"
#include "LinkGroupRepair.h"
#include "PersonMatte.h"
#include "ProcessLauncher.h"
#include "ProjectSwitch.h"
#include "WorkspaceActivity.h"

@implementation AppDelegate

// What a save panel shown as a sheet answered. In the window test, the
// destination the test set and nothing without one: AppKit's panel runs out
// of process, a test can only end its sheet, and the panel then answers the
// last folder it was used in -- on 8 October 2026, a bench project's
// package, which a test export wrote into.
NSURL* CMSaveSheetAnswer(NSSavePanel* save, NSModalResponse response) {
    if (response != NSModalResponseOK) return nil;
    if (AppTestHooks* hooks = TestHooks(); hooks && hooks->AnswersSaveSheets())
        return hooks->TakeSaveSheetAnswer();
    return save.URL;
}

// DEC39 -- docs/roadmap/PHASES-2026-08.md. « Fichier › Nouveau projet… » and
// « Fichier › Ouvrir un projet… » in the editor, as Resolve's Project
// Manager changes project without leaving the program. The window does not
// swap its project in place: it checks the other one (ProjectSwitch.h),
// writes what it holds, and quits, and -applicationWillTerminate: starts a
// new editor on the other project once the lock of this one is released.
// The welcome window is not shown in between.

- (instancetype)initWithDocumentPath:(NSString*)documentPath {
    if ((self = [super init])) {
        _documentPath = [documentPath copy];
        _state = new AppState();
        _state->automaticProxiesEnabled = [NSUserDefaults.standardUserDefaults
            boolForKey:kAutomaticProxyGenerationDefaultsKey];
        _state->programVideoScope = VideoScopeModeFromPreference(
            static_cast<int32_t>([NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey]));
        [self loadMonitorCompare];
        _mediaThumbnails = [NSMutableDictionary dictionary];
    }
    return self;
}

- (void)refreshTimelineChrome {
    if (!self.metalView || !self.state) return;
    for (NSTextField* label in self.trackHeaderLabels)
        [label removeFromSuperview];
    [self.trackHeaderLabels removeAllObjects];

    if (self.offlineMediaLabel)
        self.offlineMediaLabel.frame =
            NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                       self.programMonitorView.bounds.size.height);
    // Labels now live in the ordered Metal atlas display list. Keeping the
    // former AppKit overlays hidden avoids two coordinate systems drifting
    // apart when the timeline is resized.
    self.timelineTimecodeLabel.hidden = YES;
}

- (NSString*)selectedBrowserObjectId {
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSIndexPath* path = self.mediaCollection.selectionIndexPaths.anyObject;
        return path && path.item < self.visibleMediaIds.count
                   ? self.visibleMediaIds[path.item]
                   : nil;
    }
    const NSInteger row = self.mediaTable.selectedRow;
    return row >= 0 && row < (NSInteger)self.visibleMediaIds.count
               ? self.visibleMediaIds[row]
               : nil;
}

// ---- F2.3 Media panel: Média/Audio/Légendes tab switching -----------------
// See MediaPanelModel.h -- ui::media_panel::Tab is the pure-C++ source of
// truth for which tab exists and in what order; this method only toggles
// AppKit visibility and lazily rebuilds whichever tab's content just became
// visible (nothing here mutates the Document).

// ---- F2.3 Media panel: Audio tab -------------------------------------------
// A filtered view onto the same document.library the Media tab browses --
// see MediaPanelModel.h's file comment on why "audio-capable"
// (LibraryMedia::has_audio) stands in for "audio-only source" here. No
// ingest logic is duplicated: this tab reads the library Ingest.cc already
// populated and mutates it only through the SetMediaBinOperation the Media
// tab applies. It filters by the bin chosen in the Media tab, whose list is
// not on screen here, so its summary names that bin.

// ---- F2.3 Media panel: Captions tab ----------------------------------------
// Lists sequence.caption_styles with the number of cartons in each, creates
// and deletes styles, and dresses the cartons selected on the timeline in
// one of them, or gives them back the default look. A style dresses only
// cartons, the clips of caption tracks: nothing else is drawn as a caption,
// by the monitor or the export (MediaPanelModel.h, DescribeCaptionSelection
// and RestyleCartons). Every change goes through EditLog::Apply, a restyle
// as one BatchOperation: one undo gives every carton back.

- (NSString*)iconGridDropTargetAtIndexPath:(NSIndexPath*)indexPath
                             dropOperation:
                                 (NSCollectionViewDropOperation)dropOperation {
    if (dropOperation == NSCollectionViewDropOn && indexPath &&
        indexPath.item < self.visibleMediaIds.count) {
        NSString* identifier = self.visibleMediaIds[indexPath.item];
        if (self.state->document.FindBin(identifier.UTF8String ?: ""))
            return identifier;
    }
    if (self.state->document.FindBin(self.selectedBinId.UTF8String ?: ""))
        return self.selectedBinId;
    return @"";
}

- (void)openIconItem:(id)sender {
    NSIndexPath* path = nil;
    if ([sender isKindOfClass:NSGestureRecognizer.class]) {
        NSGestureRecognizer* recognizer = sender;
        if (recognizer.state != NSGestureRecognizerStateEnded) return;
        const NSPoint point = [recognizer locationInView:self.mediaCollection];
        path = [self.mediaCollection indexPathForItemAtPoint:point];
    } else {
        path = self.mediaCollection.selectionIndexPaths.anyObject;
    }
    if (!path || path.item >= self.visibleMediaIds.count) return;
    NSString* identifier = self.visibleMediaIds[path.item];
    if (self.state->document.FindBin(identifier.UTF8String ?: "")) return;
    self.mediaCollection.selectionIndexPaths = [NSSet setWithObject:path];
    [self openSelectedMediaInSourceMonitor:sender];
}

- (BOOL)activateTimelineIdentifier:(NSString*)identifier {
    const Ulid timelineId(identifier.UTF8String ?: "");
    if (!self.state->project.FindTimeline(timelineId)) return NO;
    [self setPlaybackDirection:0];
    // FC4 -- edit points belong to the timeline they were selected on.
    if (timelineId != self.state->activeTimelineId) {
        [self leaveTrimMode];
        self.state->dynamicTrimMode = false;
        self.state->retimeClipIds.clear();
    }
    self.state->sourceMonitor = false;
    self.state->sourceMonitorActive = false;
    self.state->sourceIn.reset();
    self.state->sourceOut.reset();
    if (timelineId == self.state->activeTimelineId) {
        [self requestResolvedPosition:self.state->requestedPosition];
        return YES;
    }

    const HistoryDomain historyBeforeNavigation = self.state->lastHistoryDomain;
    std::string message;
    if (![self persistEdits:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Changement de timeline impossible : %@",
                             CMStringFromUtf8(message)];
        return NO;
    }
    self.state->lastHistoryDomain = historyBeforeNavigation;
    const Ulid previousId = self.state->activeTimelineId;
    self.state->timelinePositions[previousId] = self.state->requestedPosition;
    self.state->timelineEditLogs[previousId] = self.state->editLog;
    self.state->timelineTargetTracks[previousId] = self.state->targetedTrackIds;
    // PRD37 -- ROADMAP.md. Each timeline keeps its own view, as it keeps
    // its playhead (TimelineView.h, OpenTimelineView).
    self.state->timelineViews[previousId] =
        RememberTimelineView(self.state->viewport, self.state->timelineFit);

    EditLog nextLog;
    const auto storedLog = self.state->timelineEditLogs.find(timelineId);
    if (storedLog != self.state->timelineEditLogs.end())
        nextLog = storedLog->second;
    self.state->activeTimelineId = timelineId;
    self.state->document = self.state->project.MakeDocument(timelineId);
    self.state->editLog = std::move(nextLog);
    self.state->timelineEditLogs[timelineId] = self.state->editLog;
    self.state->targetedTrackIds.clear();
    const auto storedTargets =
        self.state->timelineTargetTracks.find(timelineId);
    if (storedTargets != self.state->timelineTargetTracks.end()) {
        for (const Ulid& id : storedTargets->second)
            if (self.state->document.FindTrack(id))
                self.state->targetedTrackIds.insert(id);
    } else {
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            self.state->targetedTrackIds.insert(track.id);
    }
    self.state->timelineTargetTracks[timelineId] = self.state->targetedTrackIds;
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->sourceRendered = {};
    self.state->rendered.clear();
    RationalTime position{0, self.state->document.sequence.frame_rate.num};
    const auto storedPosition = self.state->timelinePositions.find(timelineId);
    if (storedPosition != self.state->timelinePositions.end())
        position = storedPosition->second;
    // PRD37 -- the view it was left with, or all of it the first time.
    const auto storedView = self.state->timelineViews.find(timelineId);
    self.state->timelineFit = OpenTimelineView(
        self.state->viewport,
        storedView != self.state->timelineViews.end() ? &storedView->second
                                                      : nullptr,
        self.state->timeline->Duration(), self.metalView.bounds.size.width);
    self.programMonitorTitleLabel.stringValue =
        CMRecordTitle(self.state->document.sequence.name);
    [self refreshTimelineAfterEditFromPosition:position];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.window.title = CMWindowTitle(self.state->project.name,
                                      self.state->document.sequence.name);
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Timeline active : %@",
                         CMStringFromUtf8(self.state->document.sequence.name)];
    self.state->overlayDirty = true;
    [self splitMergedLinkGroups];
    return YES;
}

// Out of lot -- the author, 4 November 2026: « J'ai encore le problème des
// blocs fusionnés que je peux pas bouger ». A timeline cut before #213 binds a
// block's stretches under one link, so one click took the whole block
// (LinkGroupRepair.h). Split as the timeline opens, through the journal: one
// undo step (two past 256 links), said on the status line.
- (void)splitMergedLinkGroups {
    const LinkGroupRepair repair = PlanLinkGroupRepair(
        self.state->document, [] { return GenerateUlid(); });
    if (repair.Empty()) return;
    size_t steps = 0;
    for (BatchOperation& batch : LinkGroupRepairBatches(repair)) {
        EditError error = EditError::None;
        std::string message;
        if (![self applyAndPersistTimelineOperation:Operation{std::move(batch)}
                                              error:error
                                            message:message]) {
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Liens fusionnés non séparés : %@",
                                           CMStringFromUtf8(message)];
            break;
        }
        ++steps;
    }
    if (steps == 0) return;
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Liens séparés : %lu bloc(s) monté(s) avant le 30 "
                         @"octobre, chaque morceau garde sa paire image-son "
                         @"(⌘Z pour annuler).",
                         static_cast<unsigned long>(repair.groups)];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. A timeline's name, asked on
// the window's form panel (FormPanel.h). An empty name closed the alert and
// created nothing; it now keeps the panel open and says so.
- (void)createTimelinePressed:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    NSTextField* nameField =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    nameField.identifier = @"timeline.new.name";
    nameField.placeholderString = @"Nom de la timeline";
    nameField.stringValue = [NSString
        stringWithFormat:@"Timeline %lu",
                         (unsigned long)self.state->project.timelines.size() +
                             1];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Nouvelle timeline"
              message:@"La nouvelle timeline utilisera le format de la "
                      @"timeline active."
                 form:nameField];
    [panel addButtonWithTitle:@"Créer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = nameField;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      return CMTrimmedName(nameField.stringValue).length == 0
                 ? kTimelineNameMissing
                 : nil;
    };
    [panel
        presentInWindow:self.window
             completion:^(NSModalResponse response) {
               if (response != NSAlertFirstButtonReturn) return;
               [self createTimelineNamed:CMTrimmedName(nameField.stringValue)];
             }];
}

- (void)createTimelineNamed:(NSString*)trimmed {
    std::string message;
    if (![self persistEdits:message]) return;
    Project candidate = self.state->project;
    ProjectEditLog projectLog = self.state->projectEditLog;
    const DocumentSequence& current = self.state->document.sequence;
    EditError editError = EditError::None;
    ProjectOperation operation = AddProjectTimelineOperation{
        trimmed.UTF8String ?: "New Timeline", current.width, current.height,
        current.frame_rate};
    if (!projectLog.Apply(candidate, std::move(operation), editError,
                          message)) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée (%@) : %@",
                             CMStringFromUtf8(EditErrorName(editError)),
                             CMStringFromUtf8(message)];
        return;
    }
    const auto& applied = std::get<AddProjectTimelineOperation>(
        projectLog.AppliedEntries().back().op);
    const Ulid createdId = applied.timeline_id;
    if (![self commitProjectCandidate:candidate
                              editLog:self.state->editLog
                           projectLog:projectLog
                              message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Création impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->project = std::move(candidate);
    self.state->projectEditLog = std::move(projectLog);
    self.state->lastHistoryDomain = HistoryDomain::Project;
    [self rebuildMediaList];
    [self
        activateTimelineIdentifier:[NSString
                                       stringWithUTF8String:createdId.c_str()]];
}

- (void)toggleSourceMonitor:(NSButton*)sender {
    if (!self.applyingWorkspace &&
        self.workspacePage == ui::workspace::Page::Media &&
        self.state->sourceMonitorVisible) {
        return;
    }
    // Removing the pane from the split is deterministic. Merely hiding an
    // arranged subview leaves NSSplitView free to restore its autosaved
    // divider on the next layout pass, which made SOURCE OFF appear inert.
    const bool makeVisible = !self.state->sourceMonitorVisible;
    if (!makeVisible) {
        self.state->sourceMonitorPanelWidth =
            self.sourceMonitorPanel.frame.size.width;
        self.state->sourceMonitorVisible = false;
        self.state->sourceMonitorActive = false;
        [self.monitorSplitView removeArrangedSubview:self.sourceMonitorPanel];
        [self.sourceMonitorPanel removeFromSuperview];
        sender.state = NSControlStateValueOff;
        sender.title = @"SOURCE OFF";
        sender.image =
            SystemSymbol(@"rectangle", @"Moniteur Record seul", 11.0);
    } else {
        self.state->sourceMonitorVisible = true;
        [self.monitorSplitView insertArrangedSubview:self.sourceMonitorPanel
                                             atIndex:0];
        [self.monitorSplitView adjustSubviews];
        const double available = self.monitorSplitView.bounds.size.width -
                                 self.monitorSplitView.dividerThickness;
        [self.monitorSplitView setPosition:available * 0.5 ofDividerAtIndex:0];
        self.state->sourceMonitorActive = self.state->sourceMonitor;
        sender.state = NSControlStateValueOn;
        sender.title = @"SOURCE ON";
        sender.image = SystemSymbol(@"rectangle.split.2x1",
                                    @"Afficher le moniteur Source", 11.0);
    }
    [self.monitorSplitView adjustSubviews];
    self.state->overlayDirty = true;
    // ON2 -- the page keeps what the hand chose for Source.
    if (!self.applyingWorkspace && self.workspaceBar) {
        ui::workspace::PanelSet panels = self.workspacePanels;
        panels.source = self.state->sourceMonitorVisible;
        self.workspacePanels = panels;
        if (self.rememberWorkspace)
            [CMUiPreferences.sharedPreferences setPanels:panels
                                                 forPage:self.workspacePage];
        [self layOutPageContents];
    }
}

#pragma mark - ON1, ON2 pages

// --- G6 -- docs/roadmap/GRADE-2026-09.md: comparing on Record -------------

- (MediaRate)playheadFrameRate {
    return PlayheadFrameRate(self.state->document);
}

// PRD27 -- what VoiceOver reads as the timeline's value (UiAccessibility.h):
// the playhead at the rate its timecode is shown, the selected clips by the
// names the timeline draws on them, the clicked one first.
- (NSString*)timelineAccessibilityValue {
    if (!self.state || !self.state->interaction) return nil;
    std::vector<ui::accessibility::SelectedClip> clips;
    const auto add = [&](const Ulid& clipId) {
        const DocumentClip* clip = self.state->document.FindClip(clipId);
        if (clip == nullptr) return;
        clips.push_back(
            {TimelineDrawnClipName(self.state->document, self.state->project,
                                   clip, clip->source_id),
             clip->link_group_id});
    };
    const Ulid& primary = self.state->interaction->PrimarySelectedClipId();
    if (!primary.empty()) add(primary);
    for (const Ulid& clipId : self.state->interaction->SelectedClipIds())
        if (clipId != primary) add(clipId);
    return CMStringFromUtf8(ui::accessibility::TimelineValue(
        self.state->requestedPosition, [self playheadFrameRate],
        ui::accessibility::SelectedShots(clips)));
}

- (int32_t)playheadInputRate {
    return self.state->playheadResolution == PlayheadResolution::Sample
               ? 48000
               : [self playheadFrameRate].num;
}

- (double)timelineHeight {
    return self.metalView.bounds.size.height;
}

- (double)videoHeight {
    return 0.0;
}

- (void)requestTimelinePosition:(RationalTime)position {
    if (position < RationalTime{0, 1}) position = {0, position.rate};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) {
        if (self.state->playheadResolution == PlayheadResolution::Sample) {
            quantized = {self.state->duration.to_frames(48000), 48000};
        } else {
            const MediaRate rate = [self playheadFrameRate];
            quantized = {
                self.state->duration.to_frames(rate.num, rate.den) * rate.den,
                rate.num};
        }
    }
    [self requestResolvedPosition:quantized];
    if (self.state->playbackDirection == 0 && self.state->audioPlayback) {
        std::string error;
        if (!self.state->audioPlayback->ScrubAt(quantized, error))
            std::fprintf(stderr, "Audio scrub failed: %s\n", error.c_str());
    }
    self.state->overlayDirty = true;
}

- (void)updateSelectionInfo {
    [self refreshWorkspaceActivity];
    // F2.2 -- this is the one hook already fired after every selection
    // change and every edit that could touch the selected clip (see this
    // method's call sites), so it doubles as the Inspector's refresh
    // trigger.
    //
    // PrimarySelectedClipId(), not SelectedClipId(): the latter is empty
    // whenever more than one clip is selected, and clicking a clip that is
    // A/V-linked -- which every clip imported from a video with sound is --
    // selects its partner too. Keying the Inspector off it left the panel
    // blank for exactly the footage people actually edit.
    // A reload shows the document again: whatever a drag was previewing is
    // either committed by now or given up.
    if (self.state->gradePreview || self.state->framingPreview) {
        self.state->gradePreview.reset();
        self.state->framingPreview.reset();
        self.state->overlayDirty = true;
    }
    [self.inspectorView showPlayheadAt:self.state->requestedPosition];
    [self refreshTrimBar];
    [self.inspectorView
        reloadWithDocument:self.state->document
            selectedClipId:self.state->interaction->PrimarySelectedClipId()];
    // F2.3 -- keep the Captions tab's style list and its summary of the
    // selected cartons live even when that tab is not on screen: an edit
    // from the chat or an undo changes them too. Cheap -- a few styles and
    // one label.
    if (self.captionStyleTable) [self rebuildCaptionStylesList];
    const size_t selectionCount =
        self.state->interaction->SelectedClipIds().size();
    if (selectionCount > 1) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    %zu clips "
                             @"sélectionnés    Liens %@",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool, self.shortcutBindings),
                             selectionCount,
                             self.state->linkedSelection ? @"ON" : @"OFF"];
        return;
    }
    const DocumentClip* clip = self.state->document.FindClip(
        self.state->interaction->SelectedClipId());
    if (!clip) {
        if (const auto& gap = self.state->interaction->SelectedGap()) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@  "
                                 @"  Trou sélectionné    piste %@    début %@  "
                                 @"  durée %@    Delete pour raccorder",
                                 [self transportStatus],
                                 [self playheadResolutionStatus],
                                 ToolName(self.state->tool,
                                          self.shortcutBindings),
                                 self.state->interaction->SnappingEnabled()
                                     ? @"ON (N)"
                                     : @"OFF (N)",
                                 CMStringFromUtf8(gap->track_id),
                                 TimeString(gap->start),
                                 TimeString(gap->duration)];
            return;
        }
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@    "
                             @"Aucun clip sélectionné",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool, self.shortcutBindings),
                             self.state->interaction->SnappingEnabled()
                                 ? @"ON (N)"
                                 : @"OFF (N)"];
        return;
    }
    const DocumentSource* source =
        self.state->document.FindSource(clip->source_id);
    const DocumentTrack* selectedTrack =
        self.state->document.FindTrackForClip(clip->id);
    const auto metadata = self.state->mediaMetadata.find(clip->source_id);
    const LibraryMedia* media = metadata == self.state->mediaMetadata.end()
                                    ? nullptr
                                    : &metadata->second;
    NSString* sourceText =
        source ? [NSString stringWithFormat:@"%@ (%@)",
                                            CMStringFromUtf8(source->path),
                                            CMStringFromUtf8(source->id)]
               : [NSString stringWithUTF8String:clip->source_id.c_str()];
    NSString* metadataText = @"métadonnées indisponibles";
    if (media && media->metadata_complete) {
        metadataText = [NSString
            stringWithFormat:@"%dx%d %@  %@/%@  range %@  matrix %@  rot %d°  "
                             @"%d/%d fps  audio %@",
                             media->width, media->height,
                             [NSString stringWithUTF8String:media->orientation
                                                                .c_str()],
                             CMStringFromUtf8(media->pixel_format),
                             CMStringFromUtf8(media->color_transfer),
                             CMStringFromUtf8(media->color_range),
                             CMStringFromUtf8(media->color_space),
                             media -> rotation_degrees, media -> rate.num,
                             media->rate.den,
                             media->has_audio ? @"oui" : @"non"];
    }
    NSString* roleText = @"clip";
    if (selectedTrack && selectedTrack->kind == "audio")
        roleText = @"audio séparé";
    else if (selectedTrack && selectedTrack->kind == "video")
        roleText =
            clip->sync_anchor_clip_id.empty() ? @"vidéo muette" : @"vidéo liée";
    NSString* syncText = @"";
    if (selectedTrack && selectedTrack->kind == "audio") {
        const auto drift = ClipSyncDrift(self.state->document, clip->id);
        if (drift && drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*drift, [self playheadFrameRate]);
            syncText = [NSString stringWithFormat:@"    Décalage sync %@",
                                                  CMStringFromUtf8(label)];
        }
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:
            @"%@    Playhead %@    Outil %@    Aimant %@    %@%@    source %@  "
            @"  %@    source_in %@    durée %@    timeline_in %@",
            [self transportStatus], [self playheadResolutionStatus],
            ToolName(self.state->tool, self.shortcutBindings),
            self.state->interaction->SnappingEnabled() ? @"ON (N)" : @"OFF (N)",
            roleText, syncText, sourceText, metadataText,
            TimeString(clip->source_in), TimeString(clip->duration),
            TimeString(clip->timeline_in)];
}

#pragma mark - REM5 motion canvas

// REMOTION-PARITY-2026-09 REM5 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// The visual editor of motion, on the Record monitor. Everything here
// relays: what a click selects, where a selection is drawn and which
// operation a gesture commits are MotionCanvas.h's answers (tested,
// portable); this turns events into its calls and its results into one
// operation through the same EditLog as every other edit. Not seen on
// screen yet (VISUAL_QA_CHECKLIST.md, section REM5).

#pragma mark REM5 menu

#pragma mark REM5 Inspector

- (void)setLinkedSelectionEnabled:(BOOL)enabled {
    self.state->linkedSelection = enabled;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection) {
        self.state->interaction->SelectClips(
            ExpandLinkedClipSelection(self.state->document, selected));
    } else if (selected.size() > 1) {
        self.state->interaction->SelectClip(selected.front());
    }
    [self.window makeFirstResponder:self.metalView];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

// GitHub #330 -- the fade a handle holds, on the status line: which one,
// and its length as the Inspector's « Son » section writes it.
- (void)showFadeReadout:(const SetClipAudioOperation&)settings
                   edge:(TimelineHitEdge)edge {
    const bool fadeIn = edge == TimelineHitEdge::Head;
    const std::string length = ui::inspector::FadeLengthText(
        fadeIn ? settings.fade_in : settings.fade_out,
        self.state->document.sequence.frame_rate);
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@  %@",
                         fadeIn ? @"FONDU D’ENTRÉE" : @"FONDU DE SORTIE",
                         CMStringFromUtf8(length)];
}

- (void)rebuildVideoTrackIds {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && track.visible) tracks.push_back(&track);
    std::sort(tracks.begin(), tracks.end(),
              [](const DocumentTrack* left, const DocumentTrack* right) {
                  return left->index < right->index;
              });
    self.state->videoTrackIds.clear();
    for (const DocumentTrack* track : tracks)
        self.state->videoTrackIds.push_back(track->id);
    self.state->requested.assign(tracks.size(), {});
    self.state->rendered.assign(tracks.size(), {});
}

- (void)refreshTimelineAfterEditFromPosition:(RationalTime)position {
    self.state->interaction->ClearGapSelection();
    // FC4 -- points an edit took away leave trim mode.
    if (self.state->trimSession.Active() &&
        !self.state->trimSession.StillHolds(self.state->document))
        [self leaveTrimMode];
    [self rebuildVideoTrackIds];
    if (self.state->audioPlayback)
        self.state->audioPlayback->RebuildTimeline(self.state->document);
    self.state->duration = self.state->timeline->Duration();
    if (position < RationalTime{0, 1}) position = {0, 1};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) quantized = self.state->duration;
    [self refreshTimelineChrome];
    // PERF15 -- an edit while playing: the reader reads the new montage.
    if (self.state->playbackReader.Running()) [self syncPlaybackReader];
    [self requestResolvedPosition:quantized];
}

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode, wired. The
// policy is TrimSession.h's: which point, which side, what each gesture
// plans and refuses, what the counters say. This reads the window's state
// into it and applies what it plans through the timeline's edit log, one
// operation per gesture, so that one undo takes a gesture back.
- (std::vector<Ulid>)targetedTrackList {
    return std::vector<Ulid>(self.state->targetedTrackIds.begin(),
                             self.state->targetedTrackIds.end());
}

#pragma mark - ON9 trim gestures

#pragma mark - ON8 trim bar

// The width of the ruler's widest timecode, the one at the timeline's end.
- (double)rulerLabelWidth {
    NSFont* font =
        [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeCaption
                                    weight:NSFontWeightBold];
    NSString* label = CMStringFromUtf8(
        FormatTimecode(self.state->duration, [self playheadFrameRate]));
    return std::ceil(
        [label sizeWithAttributes:@{NSFontAttributeName : font}].width);
}

// D2 -- docs/roadmap/DIALOGUE-2026-10.md. A marker's note as the atlas draws
// it: its only regular sans face (MetalUiAtlas.mm), so the box behind the
// note is as wide as the note.
- (double)markerNoteWidth:(const std::string&)note {
    NSFont* font = [NSFont systemFontOfSize:ui::theme::kFontSizeSmall
                                     weight:NSFontWeightMedium];
    return std::ceil([CMStringFromUtf8(note)
                         sizeWithAttributes:@{NSFontAttributeName : font}]
                         .width);
}

- (TimelineRenderData)timelineRenderData {
    // The display list is built in portable C++ (TimelineDrawing.h), from
    // what the window holds; this only gathers it.
    TimelineDrawingInput input;
    input.document = &self.state->document;
    input.shown_document = &[self trimShownDocument];
    input.project = &self.state->project;
    input.viewport = &self.state->viewport;
    input.interaction = self.state->interaction.get();
    input.requested = &self.state->requested;
    input.offline_source_ids = &self.state->offlineSourceIds;
    input.waveforms = &self.state->waveforms;
    input.trim_session = &self.state->trimSession;
    input.agent_journal = &self.state->agentJournal;
    input.retime_clip_ids = &self.state->retimeClipIds;
    input.width = self.metalView.bounds.size.width;
    input.timeline_height = [self timelineHeight];
    input.video_height = [self videoHeight];
    input.ruler_label_width = [self rulerLabelWidth];
    // Called during BuildTimelineRenderData only: the lambda does not
    // outlive this method.
    input.marker_note_width = [self](const std::string& note) -> double {
        return [self markerNoteWidth:note];
    };
    input.timeline_has_focus = self.window.firstResponder == self.metalView;
    input.has_valid_timeline_range = [self hasValidTimelineRange];
    input.effective_tool = [self effectiveTool];
    input.playhead_rate = [self playheadFrameRate];
    input.duration = self.state->duration;
    input.requested_position = self.state->requestedPosition;
    input.timeline_in = self.state->timelineIn;
    input.timeline_out = self.state->timelineOut;
    input.playback_direction = self.state->playbackDirection;
    input.zoom_bar_drag = self.state->zoomBarDrag;
    input.linked_selection = self.state->linkedSelection;
    input.lasso_dragging = self.state->lassoDragging;
    input.lasso_start_x = self.state->lassoStartX;
    input.lasso_start_y = self.state->lassoStartY;
    input.lasso_current_x = self.state->lassoCurrentX;
    input.lasso_current_y = self.state->lassoCurrentY;
    input.cut_preview_x = self.state->cutPreviewX;
    input.cut_preview_y = self.state->cutPreviewY;
    input.hovered_timeline_tool = self.state->hoveredTimelineTool;
    input.hovered_track_control = self.state->hoveredTrackControl;
    input.hovered_track_id = self.state->hoveredTrackId;
    input.hovered_marker_id = self.state->hoveredMarkerId;
    input.hovered_clip_id = self.state->hoveredClipId;
    input.marker_stretch = self.state->markerStretch;
    input.fade_drag = self.state->fadeDrag;
    input.effect_drop_target = self.state->effectDropTarget;
    if (AppTestHooks* hooks = TestHooks())
        input.clip_signage = !hooks->HidesClipSignage();
    return BuildTimelineRenderData(input);
}

// FitDuration rejects a width at or below the track-header gutter, which is
// the right contract for it: below that there is no timeline left to scale
// into, so any answer it returned would be a lie. A view that AppKit has not
// laid out yet reports exactly such a width, though, and that is an ordinary
// transient state rather than a caller error -- so ask here instead of
// letting an uncaught C++ exception out of a Cocoa callback and abort the
// process. -timelineMetalViewDidResize: re-fits as soon as there is a real
// width, so nothing is lost by skipping.
- (void)fitTimelineToViewportWidth {
    if (!self.state) return;
    const double width = self.metalView.bounds.size.width;
    if (!std::isfinite(width) || width <= self.state->viewport.header_width)
        return;
    self.state->timelineFit =
        FitTimeline(self.state->viewport, self.state->duration, width);
    self.state->overlayDirty = true;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_displayTimer invalidate];
    if (_timelineShortcutMonitor)
        [NSEvent removeMonitor:_timelineShortcutMonitor];
    delete _state;
}

@end

namespace cutmachine_app {

namespace {

// STRUCTURE-2026-11 ST1 -- null in the product (AppTestHooks.h).
AppTestHooks* gAppTestHooks = nullptr;

}  // namespace

AppTestHooks* TestHooks() { return gAppTestHooks; }

void InstallTestHooks(AppTestHooks* hooks) { gAppTestHooks = hooks; }

int RunCutmachine(int argc, char* argv[], const char* editorProject) {
    // HEADLESS-2026-10 H1 -- docs/roadmap/HEADLESS-2026-10.md. The command
    // line is read by RunCliCommand (CliMain.h), the same reading as the
    // build without a window; this executable supplies what needs AppKit,
    // Core Text or Vision, and opens the editor when no command was given.
    CliHost host;
    // MOTION-2026-09 M3 -- every surface of the model that renders a motion
    // frame (the MCP tool, --call-tool, the app's agent) reaches Core Text
    // and Core Graphics through this renderer (MotionFrameRenderer.h).
    // AGENT-2026-09 A20 -- and a grade read on the face needs Vision.
    // GENERATIF-2026-10 GN1 -- and paid generation reaches fal.ai through
    // NSURLSession, its key through the Keychain (Generation.h).
    host.install_renderers = [] {
        InstallMotionFrameRenderer();
        InstallFaceFinder();
        // PA15 -- and the roto's matte is Vision's person segmentation.
        person_matte::InstallVisionSegmenter();
        generation::InstallGenerationTransport();
    };
    host.require_explicit_timeline = [] {
        return static_cast<bool>([[CMUiPreferences sharedPreferences]
            requiresExplicitTimelineForAgentEdits]);
    };
    host.motion_command = &MotionCommand;
    // The window test and the play probe open the editor on their own
    // project: their flags are not commands of the product, and their
    // executable reads them (tests/ui_smoke/UiSmokeMain.mm).
    if (editorProject == nullptr) {
        if (const std::optional<int> status = RunCliCommand(argc, argv, host)) {
            return *status;
        }
    }
    // Installed by RunCliCommand already, unless the command line was
    // skipped; installing sets two pointers, twice is harmless.
    host.install_renderers();

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* documentPath = nil;
        if (editorProject != nullptr) {
            documentPath = [NSString stringWithUTF8String:editorProject];
        } else {
            // A package folder opens the project it holds, as --help says
            // (ProjectFileOf); kept as is, it failed the package check.
            if (argc == 2) {
                const std::string projectFile =
                    project_switch::ProjectFileOf(argv[1]);
                documentPath =
                    [NSString stringWithUTF8String:projectFile.c_str()];
            }
        }
        AppDelegate* delegate =
            [[AppDelegate alloc] initWithDocumentPath:documentPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}

}  // namespace cutmachine_app
