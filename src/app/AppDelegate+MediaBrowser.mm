// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The media
// browser: the bins' outline, the media list and icon grid as data sources
// and delegates, their drags and drops, the selection, renaming, moving
// into bins, and opening a media in the Source monitor or as a timeline.
// MediaPanelModel.h decides what is shown; these methods relay. Moved out
// of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (MediaBrowser)

- (void)refreshBinControlsSelecting:(NSString*)selectedBinId {
    if (!self.binOutline || !self.mediaTable) return;
    NSString* requested = selectedBinId ?: self.selectedBinId ?: @"__all__";
    self.selectedBinId = requested;
    self.updatingBinControls = YES;
    [self.binOutline reloadData];
    [self.binOutline expandItem:nil expandChildren:YES];
    const NSInteger row = [self.binOutline rowForItem:requested];
    if (row >= 0)
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
    self.updatingBinControls = NO;
    [self rebuildMediaList];
    // Bin selection/membership is shared between the Media and Audio tabs --
    // both filter the same document.library (see MediaPanelModel.h) -- so
    // keep the Audio tab's list current from this one central place.
    if (self.audioTable) [self rebuildAudioList];
}

- (void)binSelectionChanged:(id)sender {
    (void)sender;
    if (self.updatingBinControls) return;
    const NSInteger row = self.binOutline.selectedRow;
    if (row < 0) return;
    NSString* selected = [self.binOutline itemAtRow:row];
    if ([selected isEqualToString:self.selectedBinId]) return;
    self.selectedBinId = selected;
    [self rebuildMediaList];
}

- (NSArray<NSString*>*)childBinIds:(NSString*)parentId {
    NSMutableArray<NSString*>* result = [NSMutableArray array];
    const std::string parent(parentId.UTF8String ?: "");
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        if (bin.parent_id == parent) bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins)
        [result addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    return result;
}

- (NSString*)dropTargetBinForItem:(id)item {
    NSString* identifier = [item isKindOfClass:NSString.class] ? item : nil;
    if (!identifier || [identifier isEqualToString:@"__all__"] ||
        [identifier isEqualToString:@"__root__"])
        return @"";
    return self.state->document.FindBin(identifier.UTF8String ?: "")
               ? identifier
               : nil;
}

- (BOOL)bin:(NSString*)binId canMoveInto:(NSString*)parentId {
    if ([binId isEqualToString:parentId]) return NO;
    const DocumentBin* cursor =
        self.state->document.FindBin(parentId.UTF8String ?: "");
    while (cursor) {
        if (cursor->id == (binId.UTF8String ?: "")) return NO;
        cursor = self.state->document.FindBin(cursor->parent_id);
    }
    return YES;
}

- (void)rebuildMediaList {
    self.visibleMediaIds = [NSMutableArray array];
    NSString* query = self.mediaSearchField.stringValue.lowercaseString ?: @"";
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    if (selected != "__all__") {
        const Ulid parent = selected == "__root__" ? Ulid{} : selected;
        const auto childBins = ui::media_panel::DirectChildBins(
            self.state->document.bins, parent, query.UTF8String ?: "");
        for (const DocumentBin* bin : childBins)
            [self.visibleMediaIds
                addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    }
    for (const DocumentSequence& sequence : self.state->project.timelines) {
        const auto placement =
            self.state->project.timeline_bin_ids.find(sequence.id);
        const Ulid binId =
            placement == self.state->project.timeline_bin_ids.end()
                ? Ulid{}
                : placement->second;
        if (selected != "__all__" &&
            ((selected == "__root__" && !binId.empty()) ||
             (selected != "__root__" && binId != selected)))
            continue;
        NSString* sequenceName =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        if (query.length == 0 ||
            [sequenceName.lowercaseString rangeOfString:query].location !=
                NSNotFound) {
            [self.visibleMediaIds
                addObject:[NSString stringWithUTF8String:sequence.id.c_str()]];
        }
    }
    std::vector<const LibraryMedia*> mediaItems;
    for (const LibraryMedia& media : self.state->document.library) {
        if (selected != "__all__" &&
            ((selected == "__root__" && !media.bin_id.empty()) ||
             (selected != "__root__" && media.bin_id != selected)))
            continue;
        const std::string displayName =
            RushDisplayName(self.state->project, media);
        NSString* searchable =
            [NSString stringWithFormat:@"%@ %@ %@ %@ %@ %dx%d",
                                       CMStringFromUtf8(displayName),
                                       CMStringFromUtf8(media.filename),
                                       CMStringFromUtf8(media.path),
                                       CMStringFromUtf8(media.codec),
                                       CMStringFromUtf8(media.orientation),
                                       media.width, media.height];
        if (query.length > 0 &&
            [searchable.lowercaseString rangeOfString:query].location ==
                NSNotFound)
            continue;
        mediaItems.push_back(&media);
    }
    const Project* project = &self.state->project;
    std::stable_sort(
        mediaItems.begin(), mediaItems.end(),
        [project](const LibraryMedia* left, const LibraryMedia* right) {
            return RushDisplayName(*project, *left) <
                   RushDisplayName(*project, *right);
        });
    for (const LibraryMedia* media : mediaItems)
        [self.visibleMediaIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.mediaTable reloadData];
    [self.mediaCollection reloadData];
    const NSUInteger count = self.visibleMediaIds.count;
    self.binSummaryLabel.stringValue = [NSString
        stringWithFormat:@"%lu objet%@ affiché%@", (unsigned long)count,
                         count == 1 ? @"" : @"s", count == 1 ? @"" : @"s"];
    self.assignMediaButton.enabled = YES;
}

- (void)mediaViewChanged:(id)sender {
    (void)sender;
    const BOOL icons = self.mediaViewToggle.selectedSegment == 1;
    self.mediaListScroll.hidden = icons;
    self.mediaIconScroll.hidden = !icons;
    [self rebuildMediaList];
}

- (NSString*)selectedMediaId {
    return [self selectedMediaIds].firstObject;
}

- (NSArray<NSString*>*)selectedMediaIds {
    NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSArray<NSIndexPath*>* paths =
            [self.mediaCollection.selectionIndexPaths.allObjects
                sortedArrayUsingComparator:^NSComparisonResult(
                    NSIndexPath* left, NSIndexPath* right) {
                  if (left.item < right.item) return NSOrderedAscending;
                  if (left.item > right.item) return NSOrderedDescending;
                  return NSOrderedSame;
                }];
        for (NSIndexPath* path in paths) {
            if (path.item >= self.visibleMediaIds.count) continue;
            NSString* identifier = self.visibleMediaIds[path.item];
            if (!self.state->document.FindBin(identifier.UTF8String ?: ""))
                [identifiers addObject:identifier];
        }
    } else {
        [self.mediaTable.selectedRowIndexes
            enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
              (void)stop;
              if (row >= self.visibleMediaIds.count) return;
              NSString* identifier = self.visibleMediaIds[row];
              if (!self.state->document.FindBin(identifier.UTF8String ?: ""))
                  [identifiers addObject:identifier];
            }];
    }
    return identifiers;
}

- (void)openIconBin:(NSClickGestureRecognizer*)recognizer {
    if (recognizer.state != NSGestureRecognizerStateEnded) return;
    const NSPoint point = [recognizer locationInView:self.mediaCollection];
    NSIndexPath* path = [self.mediaCollection indexPathForItemAtPoint:point];
    if (!path || path.item >= self.visibleMediaIds.count) return;
    NSString* identifier = self.visibleMediaIds[path.item];
    if (!self.state->document.FindBin(identifier.UTF8String ?: "")) return;
    self.mediaCollection.selectionIndexPaths = [NSSet setWithObject:path];
    self.selectedBinId = identifier;
    const NSInteger row = [self.binOutline rowForItem:identifier];
    if (row >= 0) {
        self.updatingBinControls = YES;
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        self.updatingBinControls = NO;
    }
    [self rebuildMediaList];
}

- (void)openSelectedMediaInSourceMonitor:(id)sender {
    (void)sender;
    NSString* browserObject = [self selectedBrowserObjectId];
    if (browserObject &&
        self.state->document.FindBin(browserObject.UTF8String ?: "")) {
        [self refreshBinControlsSelecting:browserObject];
        return;
    }
    NSString* identifier = [self selectedMediaId];
    if (!identifier) {
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        if (clip)
            identifier =
                [NSString stringWithUTF8String:clip->source_id.c_str()];
    }
    if (!identifier) return;
    if (self.state->project.FindTimeline(identifier.UTF8String ?: "")) {
        [self activateTimelineIdentifier:identifier];
        return;
    }
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (void)createTimelineFromSelectedMedia:(id)sender {
    (void)sender;
    std::vector<Ulid> mediaIds;
    for (NSString* identifier in [self selectedMediaIds]) {
        const Ulid mediaId(identifier.UTF8String ?: "");
        if (self.state->document.FindLibraryMedia(mediaId))
            mediaIds.push_back(mediaId);
    }
    if (mediaIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"Sélectionnez au moins un rush pour créer une timeline.";
        NSBeep();
        return;
    }

    // PRD26 -- on the form panel, as « Nouvelle timeline ».
    NSTextField* nameField =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    nameField.identifier = @"timeline.from_selection.name";
    nameField.stringValue = [NSString
        stringWithFormat:@"Timeline %lu",
                         (unsigned long)self.state->project.timelines.size() +
                             1];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Nouvelle timeline avec la sélection"
              message:[NSString
                          stringWithFormat:
                              @"Les %lu rushes seront placés bout à bout.",
                              (unsigned long)mediaIds.size()]
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
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self createTimelineNamed:CMTrimmedName(nameField.stringValue)
                                  fromMedia:mediaIds];
                }];
}

- (void)createTimelineNamed:(NSString*)trimmed
                  fromMedia:(const std::vector<Ulid>&)mediaIds {
    const DocumentSequence& current = self.state->document.sequence;
    Project finalProject = self.state->project;
    AddProjectTimelineOperation enriched{trimmed.UTF8String ?: "New Timeline",
                                         current.width, current.height,
                                         current.frame_rate};
    enriched.timeline_id = GenerateUlid();
    enriched.video_track_id = GenerateUlid();
    enriched.audio_track_id = GenerateUlid();
    DocumentSequence createdTimeline;
    createdTimeline.id = enriched.timeline_id;
    createdTimeline.name = enriched.name;
    createdTimeline.width = enriched.width;
    createdTimeline.height = enriched.height;
    createdTimeline.frame_rate = enriched.frame_rate;
    createdTimeline.tracks = {{enriched.video_track_id, "video", 0, {}},
                              {enriched.audio_track_id, "audio", 1, {}}};
    finalProject.timelines.push_back(std::move(createdTimeline));
    EditError editError = EditError::None;
    std::string message;
    DocumentSequence* timeline =
        finalProject.FindTimeline(enriched.timeline_id);
    if (!timeline) return;
    DocumentTrack* videoTrack = nullptr;
    DocumentTrack* audioTrack = nullptr;
    for (DocumentTrack& track : timeline->tracks) {
        if (track.id == enriched.video_track_id) videoTrack = &track;
        if (track.id == enriched.audio_track_id) audioTrack = &track;
    }
    if (!videoTrack || !audioTrack) return;

    RationalTime position{0, 1};
    for (const Ulid& mediaId : mediaIds) {
        const auto foundMedia = std::find_if(
            finalProject.rushes.begin(), finalProject.rushes.end(),
            [&](const LibraryMedia& value) { return value.id == mediaId; });
        const LibraryMedia* media =
            foundMedia == finalProject.rushes.end() ? nullptr : &*foundMedia;
        const DocumentSource* source = self.state->document.FindSource(mediaId);
        if (!media || !source) continue;
        const Ulid linkGroup = media->has_audio ? GenerateUlid() : Ulid{};
        DocumentClip video;
        video.source_id = mediaId;
        video.source_in = {0, source->duration.rate};
        video.duration = source->duration;
        video.timeline_in = position;
        video.include_audio = false;
        video.link_group_id = linkGroup;
        // A rush without sound is laid unlinked, without an anchor either:
        // Document::Validate refuses an anchor outside a link group, and the
        // timeline was refused whole (window test, 9 October 2026).
        if (!linkGroup.empty()) video.sync_anchor_clip_id = video.id;
        videoTrack->clips.push_back(video);
        if (media->has_audio) {
            DocumentClip audio;
            audio.source_id = mediaId;
            audio.source_in = video.source_in;
            audio.duration = video.duration;
            audio.timeline_in = video.timeline_in;
            audio.include_audio = true;
            audio.link_group_id = linkGroup;
            audio.sync_anchor_clip_id = video.id;
            audioTrack->clips.push_back(std::move(audio));
        }
        position = position.add(source->duration);
    }
    if (!finalProject.Validate(message)) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Timeline invalide : %@",
                                       CMStringFromUtf8(message)];
        return;
    }

    enriched.exact_project_result =
        ExactProjectState{finalProject.SaveToString()};
    Project candidate = self.state->project;
    ProjectEditLog projectLog = self.state->projectEditLog;
    if (!projectLog.Apply(candidate, ProjectOperation{std::move(enriched)},
                          editError, message) ||
        ![self commitProjectCandidate:candidate
                              editLog:self.state->editLog
                           projectLog:projectLog
                              message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Création impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->project = std::move(candidate);
    self.state->projectEditLog = std::move(projectLog);
    self.state->lastHistoryDomain = HistoryDomain::Project;
    [self rebuildMediaList];
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:timeline->id
                                                                  .c_str()]];
}

- (void)openMediaIdentifierInSourceMonitor:(NSString*)identifier {
    const Ulid sourceId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(sourceId);
    const LibraryMedia* media = self.state->document.FindLibraryMedia(sourceId);
    if (!source || !media) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré pour devenir une source montable.";
        return;
    }
    // ON2 -- Montage shows Source only once a rush is loaded into it.
    if (!self.state->sourceMonitorVisible && self.sourceMonitorToggleButton)
        [self toggleSourceMonitor:self.sourceMonitorToggleButton];
    [self setPlaybackDirection:0];
    [self setSourcePlaybackDirection:0];
    if (self.state->sourceMonitorId != sourceId) {
        self.state->sourceIn.reset();
        self.state->sourceOut.reset();
    }
    self.state->sourceMonitor = true;
    self.state->sourceMonitorActive = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceMonitorPosition = {0, source->duration.rate};
    self.state->sourceRendered = {};
    self.sourceMonitorTitleLabel.stringValue = [NSString
        stringWithFormat:@"SOURCE — %@", CMStringFromUtf8(media->filename)];
    [self updateSourceZoneLabel];
    [self.window makeFirstResponder:self.sourceMonitorView];
    if (self.state->offlineSourceIds.count(sourceId) != 0) {
        self.sourceOfflineMediaLabel.hidden = NO;
        self.state->overlayDirty = true;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MONITEUR SOURCE    %@    MÉDIA OFFLINE",
                             CMStringFromUtf8(media->filename)];
        return;
    }
    self.sourceOfflineMediaLabel.hidden = YES;
    [self requestFrame:0 ofSource:sourceId];
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"MONITEUR SOURCE    %@    durée %@    %d/%d fps",
                         CMStringFromUtf8(media->filename),
                         TimeString(source->duration), source->rate.num,
                         source->rate.den];
}

- (void)revealSelectedMediaInFinder:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return;
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path = std::filesystem::absolute(
                   std::filesystem::path(self.documentPath.UTF8String ?: ""))
                   .parent_path() /
               path;
    }
    NSURL* url = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:path.lexically_normal()
                                                           .c_str()]];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    if (tableView == self.mediaTable) return self.visibleMediaIds.count;
    if (tableView == self.audioTable) return self.visibleAudioIds.count;
    if (tableView == self.captionStyleTable)
        return self.visibleCaptionStyleIds.count;
    return 0;
}

- (void)mediaSearchChanged:(id)sender {
    (void)sender;
    [self rebuildMediaList];
}

- (void)createBinPressed:(id)sender {
    (void)sender;
    const std::string parent =
        self.state->document.FindBin(self.selectedBinId.UTF8String ?: "")
            ? std::string(self.selectedBinId.UTF8String ?: "")
            : std::string{};
    std::string name = "Nouveau chutier";
    for (int suffix = 2;; ++suffix) {
        const bool exists = std::any_of(
            self.state->document.bins.begin(), self.state->document.bins.end(),
            [&](const DocumentBin& bin) {
                return bin.parent_id == parent && bin.name == name;
            });
        if (!exists) break;
        name = "Nouveau chutier " + std::to_string(suffix);
    }
    const Ulid binId = GenerateUlid();
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddBinOperation { binId, name, parent }
        }
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Création refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    NSString* identifier = [NSString stringWithUTF8String:binId.c_str()];
    [self refreshBinControlsSelecting:identifier];
    [self beginEditingBin:identifier];
}

- (void)deleteBinPressed:(id)sender {
    (void)sender;
    const std::string binId(self.selectedBinId.UTF8String ?: "");
    if (!self.state->document.FindBin(binId)) return;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveBinOperation { binId, "", "" }
        }
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshBinControlsSelecting:@"__all__"];
}

- (void)renameBinPressed:(id)sender {
    (void)sender;
    if (!self.state->document.FindBin(self.selectedBinId.UTF8String ?: ""))
        return;
    [self beginEditingBin:self.selectedBinId];
}

- (void)beginEditingBin:(NSString*)binId {
    dispatch_async(dispatch_get_main_queue(), ^{
      const NSInteger row = [self.binOutline rowForItem:binId];
      if (row < 0) return;
      [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                   byExtendingSelection:NO];
      [self.binOutline editColumn:0 row:row withEvent:nil select:YES];
    });
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    NSTextField* field = notification.object;
    if (![field isKindOfClass:NSTextField.class]) return;
    const BOOL sidebarRename = [field.identifier hasPrefix:@"bin:"];
    const BOOL browserRename = [field.identifier hasPrefix:@"browser:"];
    if (!sidebarRename && !browserRename) return;
    const NSUInteger prefixLength = sidebarRename ? 4 : 8;
    NSString* identifier = [field.identifier substringFromIndex:prefixLength];
    const NSInteger movement =
        [notification.userInfo[NSTextMovementUserInfoKey] integerValue];
    if (movement == NSCancelTextMovement) {
        [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
        return;
    }
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    NSString* name = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    const std::string currentName =
        bin        ? bin->name
        : timeline ? timeline->name
        : media    ? RushDisplayName(self.state->project, *media)
                   : std::string{};
    if (name.length == 0 || currentName.empty() ||
        currentName == (name.UTF8String ?: "")) {
        [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (bin) {
        if (![self applyAndPersistTimelineOperation:Operation {
                RenameBinOperation {
                    identifier.UTF8String ?: "", name.UTF8String ?: ""
                }
            }
                                              error:error
                                            message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Renommage refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
    } else {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Apply(
                candidate,
                ProjectOperation{RenameProjectItemOperation{
                    identifier.UTF8String ?: "", name.UTF8String ?: ""}},
                error, message) ||
            ![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Renommage refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (identifier.UTF8String &&
            self.state->activeTimelineId == identifier.UTF8String) {
            self.state->document =
                self.state->project.MakeDocument(self.state->activeTimelineId);
            self.programMonitorTitleLabel.stringValue =
                CMRecordTitle(self.state->document.sequence.name);
            self.window.title = CMWindowTitle(
                self.state->project.name, self.state->document.sequence.name);
        }
        self.state->overlayDirty = true;
    }
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
}

- (void)assignMediaToBinPressed:(id)sender {
    (void)sender;
    [self moveMediaIdsToBin:[self selectedMediaIds]];
}

// Shared "choose a destination bin, then apply SetMediaBinOperation to every
// id" flow. Factored out of -assignMediaToBinPressed so F2.3's Audio tab
// (ROADMAP.md) -- which lists a different filtered slice of the same
// document.library, see MediaPanelModel.h -- can offer the identical
// "Déplacer vers un chutier" gesture without a second alert/menu
// implementation.
- (void)moveMediaIdsToBin:(NSArray<NSString*>*)mediaIdentifiers {
    NSMutableArray<NSString*>* mediaIds = [NSMutableArray array];
    for (NSString* identifier in mediaIdentifiers)
        if (self.state->document.FindLibraryMedia(identifier.UTF8String ?: ""))
            [mediaIds addObject:identifier];
    if (mediaIds.count == 0) return;
    const LibraryMedia* selected = self.state->document.FindLibraryMedia(
        mediaIds.firstObject.UTF8String ?: "");

    NSPopUpButton* destinations =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26)];
    destinations.identifier = @"media.move.destination";
    [destinations addItemWithTitle:@"Sans chutier"];
    destinations.lastItem.representedObject = @"";
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins) {
        std::string path = bin->name;
        const DocumentBin* parent =
            self.state->document.FindBin(bin->parent_id);
        while (parent) {
            path = parent->name + " / " + path;
            parent = self.state->document.FindBin(parent->parent_id);
        }
        [destinations
            addItemWithTitle:[NSString stringWithUTF8String:path.c_str()]];
        destinations.lastItem.representedObject =
            [NSString stringWithUTF8String:bin->id.c_str()];
        if (bin->id == selected->bin_id)
            [destinations selectItem:destinations.lastItem];
    }
    if (selected->bin_id.empty()) [destinations selectItemAtIndex:0];
    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel.
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:mediaIds.count == 1 ? @"Déplacer le rush"
                                          : @"Déplacer les rushes"
              message:[NSString
                          stringWithFormat:@"Choisissez le chutier de "
                                           @"destination pour %lu "
                                           @"rush%@.",
                                           (unsigned long)mediaIds.count,
                                           mediaIds.count == 1 ? @"" : @"es"]
                 form:destinations];
    [panel addButtonWithTitle:@"Déplacer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = destinations;
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self moveMediaIds:mediaIds
                               toBin:destinations.selectedItem.representedObject
                                         ?: @""];
                }];
}

- (void)moveMediaIds:(NSArray<NSString*>*)mediaIds toBin:(NSString*)bin {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    for (NSString* media in mediaIds) {
        if (!stagedLog.Apply(stagedDocument,
                             Operation{SetMediaBinOperation{
                                 media.UTF8String ?: "", bin.UTF8String ?: ""}},
                             error, message)) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Classement refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
    }
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshBinControlsSelecting:bin.length == 0 ? @"__root__" : bin];
}

- (void)contextFindClipInMediaPool:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip) return;
    self.mediaSearchField.stringValue = @"";
    [self refreshBinControlsSelecting:@"__all__"];
    NSString* source = [NSString stringWithUTF8String:clip->source_id.c_str()];
    const NSUInteger row = [self.visibleMediaIds indexOfObject:source];
    if (row != NSNotFound) {
        self.mediaViewToggle.selectedSegment = 0;
        [self mediaViewChanged:nil];
        [self.mediaTable selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        [self.mediaTable scrollRowToVisible:row];
        [self.window makeFirstResponder:self.mediaTable];
    }
}

- (NSInteger)outlineView:(NSOutlineView*)outlineView
    numberOfChildrenOfItem:(id)item {
    (void)outlineView;
    if (!item) return 2;
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) return 0;
    if ([identifier isEqualToString:@"__root__"])
        return [self childBinIds:@""].count;
    return [self childBinIds:identifier].count;
}

- (id)outlineView:(NSOutlineView*)outlineView
            child:(NSInteger)index
           ofItem:(id)item {
    (void)outlineView;
    if (!item) {
        if (index == 0) return @"__all__";
        return @"__root__";
    }
    if ([item isEqualToString:@"__root__"])
        return [self childBinIds:@""][index];
    return [self childBinIds:item][index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) return NO;
    if ([identifier isEqualToString:@"__root__"])
        return [self childBinIds:@""].count > 0;
    return [self childBinIds:identifier].count > 0;
}

- (id<NSPasteboardWriting>)outlineView:(NSOutlineView*)outlineView
               pasteboardWriterForItem:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    if ([identifier hasPrefix:@"__"] ||
        !self.state->document.FindBin(identifier.UTF8String ?: ""))
        return nil;
    NSPasteboardItem* pasteboardItem = [[NSPasteboardItem alloc] init];
    [pasteboardItem setString:identifier forType:kCutmachineBinPasteboardType];
    return pasteboardItem;
}

- (NSDragOperation)outlineView:(NSOutlineView*)outlineView
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item
            proposedChildIndex:(NSInteger)index {
    NSString* target = [self dropTargetBinForItem:item];
    if (!target) return NSDragOperationNone;
    [outlineView setDropItem:item dropChildIndex:NSOutlineViewDropOnItemIndex];
    NSPasteboard* pasteboard = info.draggingPasteboard;
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (movingBin)
        return [self bin:movingBin canMoveInto:target] ? NSDragOperationMove
                                                       : NSDragOperationNone;
    if ([pasteboard stringForType:kCutmachineMediaPasteboardType])
        return NSDragOperationMove;
    if ([pasteboard stringForType:kCutmachineTimelinePasteboardType])
        return NSDragOperationMove;
    if ([pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]])
        return NSDragOperationCopy;
    (void)index;
    return NSDragOperationNone;
}

- (BOOL)outlineView:(NSOutlineView*)outlineView
         acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item
         childIndex:(NSInteger)index {
    (void)outlineView;
    (void)index;
    NSString* target = [self dropTargetBinForItem:item];
    if (!target) return NO;
    NSPasteboard* pasteboard = info.draggingPasteboard;
    NSArray<NSURL*>* urls = [pasteboard
        readObjectsForClasses:@[ NSURL.class ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count > 0) return [self importMediaURLs:urls intoBin:target];

    NSString* media = [pasteboard stringForType:kCutmachineMediaPasteboardType];
    NSString* timeline =
        [pasteboard stringForType:kCutmachineTimelinePasteboardType];
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (timeline) {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        EditError error = EditError::None;
        std::string message;
        if (!projectLog.Apply(
                candidate,
                ProjectOperation{SetProjectTimelineBinOperation{
                    timeline.UTF8String ?: "", target.UTF8String ?: ""}},
                error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Déplacement refusé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                           CMStringFromUtf8(message)];
            return NO;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        NSString* selected = target.length == 0 ? @"__root__" : target;
        [self refreshBinControlsSelecting:selected];
        return YES;
    }
    Operation operation;
    if (media)
        operation = SetMediaBinOperation{media.UTF8String ?: "",
                                         target.UTF8String ?: ""};
    else if (movingBin && [self bin:movingBin canMoveInto:target])
        operation = MoveBinOperation{movingBin.UTF8String ?: "",
                                     target.UTF8String ?: ""};
    else
        return NO;

    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Déplacement impossible (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    NSString* selected = target.length == 0 ? @"__root__" : target;
    [self refreshBinControlsSelecting:selected];
    if (target.length > 0) [self.binOutline expandItem:target];
    return YES;
}

- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item {
    (void)outlineView;
    // PRD37 -- its name follows the row's selection (CMListCellView).
    CMListCellView* cell = [[CMListCellView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, tableColumn.width, 26.0)];
    NSImageView* icon =
        [[NSImageView alloc] initWithFrame:NSMakeRect(2.0, 6.0, 14.0, 14.0)];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    NSTextField* label = [NSTextField labelWithString:@""];
    label.frame = NSMakeRect(
        21.0, 4.0, std::max<CGFloat>(20.0, tableColumn.width - 23.0), 18.0);
    label.autoresizingMask = NSViewWidthSizable;
    label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
    label.textColor = CMThemeColor(ui::theme::ListRowLabel(false));
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) {
        label.stringValue = @"Tous";
        icon.image = SystemSymbol(@"square.grid.2x2", @"Tous les objets", 12.0);
        icon.contentTintColor = CMThemeColor(ui::theme::kTextTertiary);
    } else if ([identifier isEqualToString:@"__root__"]) {
        label.stringValue = @"Projet";
        icon.image = SystemSymbol(@"folder", @"Racine du projet", 12.0);
        icon.contentTintColor = CMAccentColor();
    } else {
        const DocumentBin* bin =
            self.state->document.FindBin(identifier.UTF8String ?: "");
        label.stringValue =
            bin ? [NSString stringWithUTF8String:bin->name.c_str()]
                : @"Chutier manquant";
        icon.image = SystemSymbol(@"folder", @"Chutier", 12.0);
        icon.contentTintColor = CMAccentColor();
        if (bin) {
            label.editable = YES;
            label.selectable = YES;
            label.delegate = self;
            label.identifier = [@"bin:" stringByAppendingString:identifier];
        }
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    cell.imageView = icon;
    cell.textField = label;
    [cell addSubview:icon];
    [cell addSubview:label];
    return cell;
}

// PRD37 -- ROADMAP.md. A bin is selected as a rush is, in the theme's row
// (UiTheme.h kListRowSelected), not AppKit's blue.
- (NSTableRowView*)outlineView:(NSOutlineView*)outlineView
                rowViewForItem:(id)item {
    (void)outlineView;
    (void)item;
    return [[CMIndustrialTableRowView alloc] initWithFrame:NSZeroRect];
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.binOutline)
        [self binSelectionChanged:self.binOutline];
}

- (NSInteger)collectionView:(NSCollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section {
    (void)collectionView;
    (void)section;
    return self.visibleMediaIds.count;
}

- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath {
    MediaIconItem* item = [collectionView makeItemWithIdentifier:@"media-icon"
                                                    forIndexPath:indexPath];
    if (indexPath.item >= self.visibleMediaIds.count) return item;
    NSString* identifier = self.visibleMediaIds[indexPath.item];
    item.representedObject = identifier;
    ((MediaIconView*)item.view).indexPath = indexPath;
    item.textField.editable = YES;
    item.textField.selectable = YES;
    item.textField.delegate = self;
    item.textField.identifier =
        [@"browser:" stringByAppendingString:identifier];
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if (bin) {
        item.textField.stringValue =
            [NSString stringWithUTF8String:bin->name.c_str()];
        item.imageView.image = SystemSymbol(@"folder.fill", @"Chutier", 38.0);
        item.imageView.contentTintColor = CMThemeColor(ui::theme::kAccent);
        item.imageView.layer.backgroundColor = NSColor.clearColor.CGColor;
        item.view.toolTip = @"Double-cliquer pour ouvrir ce chutier";
        return item;
    }
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    if (timeline) {
        const DocumentSequence& sequence = *timeline;
        item.textField.stringValue =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        item.imageView.image =
            SystemSymbol(@"rectangle.stack.fill", @"Séquence", 34.0);
        item.imageView.contentTintColor = CMAccentColor();
        item.imageView.layer.backgroundColor = NSColor.clearColor.CGColor;
        item.view.toolTip = [NSString
            stringWithFormat:@"Séquence · %dx%d · %d/%d fps · %lu pistes",
                             sequence.width, sequence.height,
                             sequence.frame_rate.num, sequence.frame_rate.den,
                             (unsigned long)sequence.tracks.size()];
        return item;
    }
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return item;
    item.textField.stringValue = [NSString
        stringWithUTF8String:RushDisplayName(self.state->project, *media)
                                 .c_str()];
    const bool offline = self.state->offlineSourceIds.count(media->id) != 0;
    NSImage* thumbnail = offline ? nil : self.mediaThumbnails[identifier];
    if (thumbnail) {
        item.imageView.contentTintColor = nil;
        item.imageView.image = thumbnail;
    } else {
        NSImage* symbol = [NSImage
            imageWithSystemSymbolName:(offline ? @"exclamationmark.triangle"
                                               : @"film")
             accessibilityDescription:(offline ? @"Média offline"
                                               : @"Média vidéo")];
        symbol.size = NSMakeSize(44.0, 44.0);
        item.imageView.image = symbol;
        item.imageView.contentTintColor =
            offline ? CMThemeColor(ui::theme::kError) : CMTextSecondaryColor();
    }
    item.imageView.layer.backgroundColor = CMSurfaceRaisedColor().CGColor;
    item.view.toolTip =
        offline ? [NSString stringWithFormat:@"%@ · MÉDIA OFFLINE",
                                             CMStringFromUtf8(media->filename)]
                : [NSString stringWithFormat:@"%@ · %@ · %dx%d · %@",
                                             CMStringFromUtf8(media->filename),
                                             CMStringFromUtf8(media->codec),
                                             media->width, media->height,
                                             TimeString(media->duration)];
    if (!offline && !media->proxy_path.empty())
        item.view.toolTip =
            [item.view.toolTip stringByAppendingString:@" · PROXY disponible"];
    return item;
}

- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath {
    (void)collectionView;
    if (indexPath.item >= self.visibleMediaIds.count) return nil;
    if (self.state->document.FindBin(
            self.visibleMediaIds[indexPath.item].UTF8String ?: ""))
        return nil;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    NSString* identifier = self.visibleMediaIds[indexPath.item];
    const NSPasteboardType type =
        self.state->project.FindTimeline(identifier.UTF8String ?: "")
            ? kCutmachineTimelinePasteboardType
            : kCutmachineMediaPasteboardType;
    [item setString:identifier forType:type];
    return item;
}

- (BOOL)collectionView:(NSCollectionView*)collectionView
    canDragItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths
                   withEvent:(NSEvent*)event {
    (void)collectionView;
    (void)event;
    for (NSIndexPath* path in indexPaths) {
        if (path.item >= self.visibleMediaIds.count) continue;
        const std::string identifier(self.visibleMediaIds[path.item].UTF8String
                                         ?: "");
        if (self.state->document.FindLibraryMedia(identifier) ||
            self.state->project.FindTimeline(identifier))
            return YES;
    }
    return NO;
}

- (NSDragOperation)collectionView:(NSCollectionView*)collectionView
                     validateDrop:(id<NSDraggingInfo>)draggingInfo
                proposedIndexPath:(NSIndexPath**)proposedDropIndexPath
                    dropOperation:
                        (NSCollectionViewDropOperation*)proposedDropOperation {
    if (collectionView != self.mediaCollection || !proposedDropIndexPath ||
        !*proposedDropIndexPath)
        return NSDragOperationNone;
    NSString* target =
        [self iconGridDropTargetAtIndexPath:*proposedDropIndexPath
                              dropOperation:*proposedDropOperation];

    NSPasteboard* pasteboard = draggingInfo.draggingPasteboard;
    NSString* movingMedia =
        [pasteboard stringForType:kCutmachineMediaPasteboardType];
    if (movingMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(movingMedia.UTF8String ?: "");
        if (!media || media->bin_id == (target.UTF8String ?: ""))
            return NSDragOperationNone;
        return NSDragOperationMove;
    }
    NSString* movingTimeline =
        [pasteboard stringForType:kCutmachineTimelinePasteboardType];
    if (movingTimeline) {
        const Ulid timelineId(movingTimeline.UTF8String ?: "");
        if (!self.state->project.FindTimeline(timelineId))
            return NSDragOperationNone;
        const auto placement =
            self.state->project.timeline_bin_ids.find(timelineId);
        const Ulid current =
            placement == self.state->project.timeline_bin_ids.end()
                ? Ulid{}
                : placement->second;
        return current == (target.UTF8String ?: "") ? NSDragOperationNone
                                                    : NSDragOperationMove;
    }
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (movingBin) {
        if (![self bin:movingBin canMoveInto:target])
            return NSDragOperationNone;
        return NSDragOperationMove;
    }
    if ([pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]])
        return NSDragOperationCopy;
    return NSDragOperationNone;
}

- (BOOL)collectionView:(NSCollectionView*)collectionView
            acceptDrop:(id<NSDraggingInfo>)draggingInfo
             indexPath:(NSIndexPath*)indexPath
         dropOperation:(NSCollectionViewDropOperation)dropOperation {
    if (collectionView != self.mediaCollection) return NO;
    NSString* target = [self iconGridDropTargetAtIndexPath:indexPath
                                             dropOperation:dropOperation];
    // Reuse the sidebar's operation-backed drop path: media classification,
    // bin nesting, Finder ingest, persistence, and refresh stay identical for
    // both representations of the same project architecture.
    return [self outlineView:self.binOutline
                  acceptDrop:draggingInfo
                        item:target.length > 0 ? target : @"__root__"
                  childIndex:NSOutlineViewDropOnItemIndex];
}

- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard {
    if (tableView != self.mediaTable || rowIndexes.count != 1) return NO;
    const NSUInteger row = rowIndexes.firstIndex;
    if (row >= self.visibleMediaIds.count) return NO;
    if (self.state->document.FindBin(self.visibleMediaIds[row].UTF8String
                                         ?: ""))
        return NO;
    NSString* identifier = self.visibleMediaIds[row];
    const NSPasteboardType type =
        self.state->project.FindTimeline(identifier.UTF8String ?: "")
            ? kCutmachineTimelinePasteboardType
            : kCutmachineMediaPasteboardType;
    [pasteboard declareTypes:@[ type ] owner:nil];
    return [pasteboard setString:identifier forType:type];
}

- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row {
    (void)tableView;
    (void)row;
    return [[CMIndustrialTableRowView alloc] initWithFrame:NSZeroRect];
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (tableView == self.audioTable) {
        if (row < 0 || row >= (NSInteger)self.visibleAudioIds.count) return nil;
        const LibraryMedia* media = self.state->document.FindLibraryMedia(
            self.visibleAudioIds[row].UTF8String ?: "");
        NSTextField* label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        if (!media) return label;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue =
                [NSString stringWithUTF8String:media->filename.c_str()];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue = [NSString
                stringWithFormat:@"%@ · %d ch", CMStringFromUtf8(media->codec),
                                 media->audio_channels];
        else
            label.stringValue = TimeString(media->duration);
        return label;
    }
    if (tableView == self.captionStyleTable) {
        if (row < 0 || row >= (NSInteger)self.visibleCaptionStyleIds.count)
            return nil;
        const CaptionStyle* style = self.state->document.FindCaptionStyle(
            self.visibleCaptionStyleIds[row].UTF8String ?: "");
        NSTextField* label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        if (!style) return label;
        if ([tableColumn.identifier isEqualToString:@"style"]) {
            label.stringValue = [NSString
                stringWithUTF8String:ui::media_panel::DescribeCaptionStyle(
                                         *style)
                                         .c_str()];
        } else {
            const auto summaries = ui::media_panel::SummarizeCaptionStyles(
                self.state->document.sequence);
            int32_t cartons = 0;
            int32_t others = 0;
            for (const auto& summary : summaries)
                if (summary.style_id == style->id) {
                    cartons = summary.carton_count;
                    others = summary.other_clip_count;
                }
            // F2.3 -- cartons only; pictures that carry the style without
            // showing it are counted apart.
            label.stringValue =
                others > 0
                    ? [NSString stringWithFormat:@"%d (+%d)", cartons, others]
                    : [NSString stringWithFormat:@"%d", cartons];
            label.toolTip =
                others > 0
                    ? [NSString
                          stringWithFormat:@"%d plan%@ vidéo porte%@ aussi ce "
                                           @"style, sans l’afficher",
                                           others, others == 1 ? @"" : @"s",
                                           others == 1 ? @"" : @"nt"]
                    : nil;
        }
        return label;
    }
    if (tableView != self.mediaTable || row < 0 ||
        row >= (NSInteger)self.visibleMediaIds.count)
        return nil;
    NSString* identifier = self.visibleMediaIds[row];
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if ([tableColumn.identifier isEqualToString:@"name"]) {
        NSTableCellView* cell = [[NSTableCellView alloc]
            initWithFrame:NSMakeRect(0, 0, tableColumn.width, 44.0)];
        NSImageView* image =
            [[NSImageView alloc] initWithFrame:NSMakeRect(4, 3, 60, 38)];
        image.imageScaling = NSImageScaleProportionallyUpOrDown;
        image.wantsLayer = YES;
        image.layer.cornerRadius = 0.0;
        image.layer.masksToBounds = YES;
        NSTextField* title = [[BrowserRenameTextField alloc]
            initWithFrame:NSMakeRect(72, 12,
                                     std::max(40.0, tableColumn.width - 76),
                                     20)];
        title.bezeled = NO;
        title.drawsBackground = NO;
        title.autoresizingMask = NSViewWidthSizable;
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        title.editable = YES;
        title.selectable = YES;
        title.delegate = self;
        title.identifier = [@"browser:" stringByAppendingString:identifier];
        cell.imageView = image;
        cell.textField = title;
        [cell addSubview:image];
        [cell addSubview:title];
        if (bin) {
            title.stringValue =
                [NSString stringWithUTF8String:bin->name.c_str()];
            image.image = SystemSymbol(@"folder.fill", @"Chutier", 24.0);
            image.contentTintColor = CMThemeColor(ui::theme::kAccent);
        } else if (media) {
            title.stringValue =
                [NSString stringWithUTF8String:RushDisplayName(
                                                   self.state->project, *media)
                                                   .c_str()];
            const bool offline =
                self.state->offlineSourceIds.count(media->id) != 0;
            image.image = offline ? nil : self.mediaThumbnails[identifier];
            if (image.image) {
                image.contentTintColor = nil;
            } else {
                image.image = SystemSymbol(offline ? @"exclamationmark.triangle"
                                           : media->has_video ? @"film"
                                                              : @"waveform",
                                           offline ? @"Média offline"
                                           : media->has_video ? @"Média vidéo"
                                                              : @"Média audio");
                image.contentTintColor = offline
                                             ? CMThemeColor(ui::theme::kError)
                                             : CMTextSecondaryColor();
            }
        } else if (const DocumentSequence* sequence =
                       self.state->project.FindTimeline(identifier.UTF8String
                                                            ?: "")) {
            title.stringValue =
                [NSString stringWithUTF8String:sequence->name.c_str()];
            image.image =
                SystemSymbol(@"rectangle.stack.fill", @"Séquence", 22.0);
            image.contentTintColor = CMAccentColor();
        }
        return cell;
    }
    NSTextField* label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (bin) {
        if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue = @"Chutier";
        else
            label.stringValue = @"—";
        return label;
    }
    if (!media) {
        const DocumentSequence* found =
            self.state->project.FindTimeline(identifier.UTF8String ?: "");
        if (!found) return label;
        const DocumentSequence& sequence = *found;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue = [NSString
                stringWithFormat:@"%@◫ %@",
                                 sequence.id == self.state->activeTimelineId
                                     ? @"● "
                                     : @"",
                                 CMStringFromUtf8(sequence.name)];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue =
                [NSString stringWithFormat:@"Séquence %dx%d", sequence.width,
                                           sequence.height];
        else {
            Document timelineDocument =
                self.state->project.MakeDocument(sequence.id);
            timelineDocument.sequence = sequence;  // document view, not an edit
            label.stringValue =
                TimeString(Timeline(timelineDocument).Duration());
        }
        return label;
    }
    if ([tableColumn.identifier isEqualToString:@"name"])
        label.stringValue = [NSString
            stringWithFormat:@"%@%@", media->proxy_path.empty() ? @"" : @"P · ",
                             CMStringFromUtf8(media->filename)];
    else if ([tableColumn.identifier isEqualToString:@"format"])
        label.stringValue =
            self.state->offlineSourceIds.count(media->id) ? @"Média offline"
            : media->metadata_complete && !media->has_video
                ? [NSString stringWithFormat:@"Audio · %@ · %d ch",
                                             CMStringFromUtf8(media->codec),
                                             media->audio_channels]
            : media->metadata_complete
                ? [NSString stringWithFormat:@"%@%@ %dx%d",
                                             media->proxy_path.empty()
                                                 ? @""
                                                 : @"Proxy · ",
                                             CMStringFromUtf8(media->codec),
                                             media->width, media->height]
                : @"—";
    else
        label.stringValue =
            [NSString stringWithFormat:@"%@", TimeString(media->duration)];
    return label;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.captionStyleTable) {
        [self updateCaptionSelectionLabel];
        return;
    }
    if (notification.object != self.mediaTable) return;
    (void)[self selectedMediaId];
}

- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths {
    (void)collectionView;
    NSIndexPath* path = indexPaths.anyObject;
    if (path && path.item < self.visibleMediaIds.count) {
        NSString* identifier = self.visibleMediaIds[path.item];
        if (self.state->document.FindBin(identifier.UTF8String ?: "")) {
            self.selectedBinId = identifier;
            const NSInteger row = [self.binOutline rowForItem:identifier];
            if (row >= 0) {
                self.updatingBinControls = YES;
                [self.binOutline
                        selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                    byExtendingSelection:NO];
                self.updatingBinControls = NO;
            }
            [self rebuildMediaList];
            return;
        }
    }
    (void)[self selectedMediaId];
}

@end
