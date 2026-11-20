// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The editor's
// life: the application's and the window's delegate, the launch that builds
// the window and loads the project under its lock, the files opened from
// the Finder, full screen, the window drawn off screen, and the end, which
// writes what is held and releases the lock. Moved out of main.mm
// unchanged.

#include "AppDelegatePrivate.h"
#include "ProjectSwitch.h"

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.

@implementation AppDelegate (Lifecycle)

- (void)toggleFullscreen:(id)sender {
    (void)sender;
    if (self.videoFullscreenActive) {
        self.videoFullscreenActive = NO;
        [self.programMonitorView removeFromSuperview];
        [self.videoFullscreenWindow orderOut:nil];
        self.programMonitorView.frame = self.programMonitorPanel.bounds;
        [self.programMonitorPanel addSubview:self.programMonitorView];
        self.programMonitorTitleLabel.hidden = NO;
        self.programMonitorZoomPopup.hidden = NO;
        self.sourceMonitorToggleButton.hidden = NO;
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:self.programMonitorView];
    } else {
        NSScreen* screen = self.window.screen ?: NSScreen.mainScreen;
        if (!screen) return;
        [self.programMonitorView removeFromSuperview];
        if (!self.videoFullscreenWindow) {
            self.videoFullscreenWindow = [[VideoFullscreenWindow alloc]
                initWithContentRect:screen.frame
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO
                             screen:screen];
            self.videoFullscreenWindow.backgroundColor = NSColor.blackColor;
            self.videoFullscreenWindow.opaque = YES;
            self.videoFullscreenWindow.level = NSMainMenuWindowLevel + 1;
            self.videoFullscreenWindow.animationBehavior =
                NSWindowAnimationBehaviorNone;
            self.videoFullscreenWindow.releasedWhenClosed = NO;
            self.videoFullscreenWindow.collectionBehavior =
                NSWindowCollectionBehaviorFullScreenAuxiliary |
                NSWindowCollectionBehaviorStationary;
        } else {
            [self.videoFullscreenWindow setFrame:screen.frame display:NO];
        }
        self.videoFullscreenActive = YES;
        self.programMonitorView.frame =
            self.videoFullscreenWindow.contentView.bounds;
        [self.videoFullscreenWindow.contentView
            addSubview:self.programMonitorView];
        self.programMonitorTitleLabel.hidden = YES;
        self.programMonitorZoomPopup.hidden = YES;
        self.sourceMonitorToggleButton.hidden = YES;
        self.state->sourceMonitorActive = false;
        [self.videoFullscreenWindow makeKeyAndOrderFront:nil];
        [self.videoFullscreenWindow makeFirstResponder:self.programMonitorView];
    }
    self.state->overlayDirty = true;
    // The motion overlay places its outlines for the size Record had
    // (motionCanvasPrepare): entering, it is cleared, and leaving, the
    // selection is placed again for the panel. The node under the mouse is
    // forgotten either way -- the mouse is not where it was.
    self.state->motionCanvas.hovered.clear();
    [self motionCanvasRefresh];
}

- (BOOL)loadDocumentAndSources {
    if ([self loadDocumentAndSourcesHoldingLock]) return YES;
    // A load that fails must not keep the project locked. Several callers
    // react to NO with -[NSApp terminate:], which tears the process down
    // without unwinding C++ destructors, so ProjectSessionLock's own
    // destructor never runs and the .lock directory outlives the process.
    // Acquire() does reclaim a lock whose owning pid is gone, so this only
    // ever leaked litter rather than wedging the next open -- but the litter
    // is confusing next to a project that failed to open for another reason.
    if (self.state) self.state->projectLock.reset();
    return NO;
}

- (BOOL)loadDocumentAndSourcesHoldingLock {
    const std::string documentPath(self.documentPath.UTF8String ?: "");
    std::string error;
    self.lastProjectLoadError = nil;
    self.projectArchiveWarningShown = NO;
    self.state->projectLock = std::make_unique<ProjectSessionLock>();
    std::string lockOwner;
    if (!self.state->projectLock->Acquire(documentPath, lockOwner, error)) {
        self.lastProjectLoadError =
            [NSString stringWithUTF8String:error.c_str()];
        return NO;
    }
    if (!RecoverTextArtifactTransaction(
            std::filesystem::path(documentPath).parent_path().string(),
            error)) {
        self.lastProjectLoadError =
            [NSString stringWithFormat:@"Transaction projet irrécupérable : %@",
                                       CMStringFromUtf8(error)];
        return NO;
    }
    // SAVING_ROADMAP.md, the editor killed. A temporary of an editor killed
    // inside WriteAutosave: this one holds the lock, the only autosave
    // writer, so what is left is a dead writer's. Not fatal.
    size_t autosaveLeftovers = 0;
    if (!ProjectRecovery::DiscardAutosaveLeftovers(documentPath,
                                                   autosaveLeftovers, error))
        std::fprintf(stderr, "%s\n", error.c_str());
    bool recoveredAutosave = false;
    const ProjectRecoveryInfo recovery = ProjectRecovery::Inspect(documentPath);
    if (recovery.state == ProjectRecoveryState::Available) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"Récupération disponible";
        // FI11 -- docs/roadmap/FIABILITE-2026-10.md. Beside an unreadable
        // project file the autosave is all that is left: declining keeps it
        // for the next opening instead of discarding it.
        alert.informativeText =
            recovery.primary_unreadable
                ? @"Le fichier du projet est illisible. La sauvegarde "
                  @"automatique est le seul état récupérable : elle est "
                  @"gardée si vous ne récupérez pas maintenant."
                : @"Une sauvegarde automatique valide est disponible pour "
                  @"récupérer le projet.";
        [alert addButtonWithTitle:@"Récupérer"];
        [alert addButtonWithTitle:recovery.primary_unreadable ? @"Plus tard"
                                                              : @"Ignorer"];
        // SAVING_ROADMAP.md, the editor killed. The forced-stop bench
        // (scripts/editor_forced_stop_bench.py) relaunches the editor
        // screen locked, where this alert would wait for a click that
        // cannot come. CUTMACHINE_RECOVERY_ANSWER=recover|decline answers
        // it as the first or second button would, and says so on stderr;
        // unset, or any other value, the person answers.
        NSModalResponse answer = NSAlertSecondButtonReturn;
        const char* scripted = std::getenv("CUTMACHINE_RECOVERY_ANSWER");
        const std::string scriptedAnswer = scripted != nullptr ? scripted : "";
        if (scriptedAnswer == "recover" || scriptedAnswer == "decline") {
            answer = scriptedAnswer == "recover" ? NSAlertFirstButtonReturn
                                                 : NSAlertSecondButtonReturn;
            std::fprintf(stderr,
                         "Recovery alert answered by "
                         "CUTMACHINE_RECOVERY_ANSWER: %s\n",
                         scriptedAnswer.c_str());
        } else {
            answer = [alert runModal];
        }
        if (answer == NSAlertFirstButtonReturn) {
            Project recovered;
            std::map<std::string, EditLog> recoveredLogs;
            ProjectEditLog recoveredProjectLog;
            if (!ProjectRecovery::LoadAutosave(documentPath, recovered,
                                               recoveredLogs,
                                               recoveredProjectLog, error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            if (!CommitStoredProjectAndLogs(documentPath, recovered,
                                            recoveredLogs, recoveredProjectLog,
                                            error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            self.state->project = std::move(recovered);
            recoveredAutosave = true;
        }
        if ((recoveredAutosave || !recovery.primary_unreadable) &&
            !ProjectRecovery::DiscardAutosave(documentPath, error))
            std::fprintf(stderr, "Unable to discard autosave: %s\n",
                         error.c_str());
    } else if (recovery.state == ProjectRecoveryState::Invalid) {
        std::fprintf(stderr, "Invalid autosave ignored: %s\n",
                     recovery.error.c_str());
    }
    // Recovery must run before package validation: an interrupted commit can
    // leave the main generation incomplete while its autosave is still the
    // only valid representation of the project.
    CollectionIntegrityReport integrity;
    if (!VerifyPortableProject(documentPath, integrity, error)) {
        self.lastProjectLoadError =
            [NSString stringWithFormat:@"Paquet projet invalide : %@",
                                       CMStringFromUtf8(error)];
        return NO;
    }
    if (!integrity.missing_media.empty() || !integrity.modified_media.empty() ||
        !integrity.damaged_looks.empty()) {
        NSAlert* warning = [NSAlert new];
        warning.alertStyle = NSAlertStyleCritical;
        warning.messageText = @"Intégrité du projet compromise";
        NSString* media = [NSString
            stringWithFormat:@"%lu média%@ manquant%@ et %lu modifié%@. "
                             @"Le montage peut être ouvert, mais son rendu "
                              "n’est plus garanti identique.",
                             (unsigned long)integrity.missing_media.size(),
                             integrity.missing_media.size() == 1 ? @"" : @"s",
                             integrity.missing_media.size() == 1 ? @"" : @"s",
                             (unsigned long)integrity.modified_media.size(),
                             integrity.modified_media.size() == 1 ? @"" : @"s"];
        // FI13 -- docs/roadmap/FIABILITE-2026-10.md. A damaged look opens
        // with the project; its timeline's renders refuse until the .cube
        // is imported again, which repairs the package's copy.
        if (!integrity.damaged_looks.empty()) {
            media = [media
                stringByAppendingFormat:
                    @"\n\nLook absent ou modifié sur %lu timeline%@. Ses "
                    @"rendus sont refusés jusqu’à ce que le fichier .cube "
                    @"soit réimporté.",
                    (unsigned long)integrity.damaged_looks.size(),
                    integrity.damaged_looks.size() == 1 ? @"" : @"s"];
        }
        warning.informativeText = media;
        [warning addButtonWithTitle:@"Ouvrir quand même"];
        [warning addButtonWithTitle:@"Annuler"];
        if ([warning runModal] != NSAlertFirstButtonReturn) {
            self.lastProjectLoadError =
                @"Ouverture annulée après contrôle d’intégrité.";
            return NO;
        }
    }
    if (!recoveredAutosave &&
        !LoadStoredProject(documentPath, self.state->project, error)) {
        std::fprintf(stderr, "Unable to load project: %s\n", error.c_str());
        return NO;
    }
    self.state->activeTimelineId = self.state->project.active_timeline_id;
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : self.state->project.timelines) {
        const std::string logPath =
            TimelineEditLogPathForProject(documentPath, timeline.id);
        std::error_code logExistsError;
        if (std::filesystem::exists(logPath, logExistsError)) {
            EditLog log;
            if (!EditLog::Load(logPath, log, editError, error)) {
                std::fprintf(stderr, "Unable to load timeline edit log: %s\n",
                             error.c_str());
                return NO;
            }
            self.state->timelineEditLogs[timeline.id] = std::move(log);
        }
        if (logExistsError) {
            std::fprintf(stderr, "Unable to inspect timeline edit log: %s\n",
                         logExistsError.message().c_str());
            return NO;
        }
    }
    const auto activeLog =
        self.state->timelineEditLogs.find(self.state->activeTimelineId);
    self.state->editLog = activeLog == self.state->timelineEditLogs.end()
                              ? EditLog{}
                              : activeLog->second;
    // DEC24 -- SAVING_ROADMAP.md. A journal written before S6 is migrated
    // here, under the lock this window holds, instead of being converted
    // again at every opening; one that cannot be written back still opens.
    ProjectEditLogMigration projectLogMigration;
    if (!LoadProjectEditLogMigrating(documentPath, self.state->projectEditLog,
                                     projectLogMigration, error)) {
        std::fprintf(stderr, "Unable to load project edit log: %s\n",
                     error.c_str());
        return NO;
    }
    if (!projectLogMigration.rewrite_error.empty()) {
        std::fprintf(stderr, "Project edit log not migrated: %s\n",
                     projectLogMigration.rewrite_error.c_str());
    }
    self.state->timelineEditLogs[self.state->activeTimelineId] =
        self.state->editLog;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        self.state->targetedTrackIds.insert(track.id);
    self.state->timelineTargetTracks[self.state->activeTimelineId] =
        self.state->targetedTrackIds;
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 100.0;
    self.state->viewport.track_height = ui::theme::kTimelineTrackHeight;
    self.state->viewport.header_width = ui::theme::kTimelineTrackHeaderWidth;
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    try {
        self.state->duration = self.state->timeline->Duration();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Invalid timeline duration: %s\n",
                     exception.what());
        return NO;
    }
    self.state->frameCache = std::make_unique<FrameCache>(kGlobalCacheBudget);
    self.state->performanceMetrics = std::make_unique<PerformanceMetrics>();
    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(std::filesystem::path(documentPath))
            .parent_path();
    // PERF-2026-09. This used to re-probe every rush on every open -- an
    // avformat_open_input plus avformat_find_stream_info per source,
    // measured at about 110 ms for a 256 MB camera file -- to fill a map the
    // interface reads for rotation, colour and channel counts.
    //
    // LibraryMedia already carries every one of those fields, and
    // `metadata_complete` already says whether they were probed. The rest of
    // the project trusts exactly that: Operations.cc validates inserts
    // against it, Export.cc resolves the rendered deliverable from it,
    // Relink and TimelineSheets read it. Only this path went back to the
    // file, which meant the monitor could show a rotation the export would
    // not apply -- two answers to one question, and the slow one was not the
    // authoritative one.
    //
    // So the store answers, and the drive is only consulted for the entries
    // it cannot answer for: version-1 media promoted without technical
    // metadata (`metadata_complete == false`, see Document.cc), and sources
    // with no library entry at all. In a project ingested by any current
    // version that is nothing, and opening it costs no media I/O here.
    //
    // The trade is that a rush swapped on disk behind the project's back no
    // longer changes the monitor until it is re-ingested or relinked. It
    // never changed the export, so this makes the two agree instead of
    // letting them drift; a swap that matters is still caught when the
    // decoder opens, by DecodeWorkerMatchesSource.
    const std::vector<DocumentSource>& openSources =
        self.state->document.sources;
    struct ProbedSource {
        bool needs_probe = false;
        bool probed = false;
        LibraryMedia detected;
        std::string media_path;
        std::string probe_error;
    };
    std::vector<ProbedSource> probes(openSources.size());
    for (size_t index = 0; index < openSources.size(); ++index) {
        const DocumentSource& source = openSources[index];
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.id);
        if (media && media->metadata_complete) {
            self.state->mediaMetadata[source.id] = *media;
            continue;
        }
        ProbedSource& probe = probes[index];
        probe.needs_probe = true;
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) mediaPath = baseDirectory / mediaPath;
        probe.media_path = mediaPath.lexically_normal().string();
        probe.detected.id = source.id;
        probe.detected.path = media ? media->path : source.path;
        probe.detected.filename = mediaPath.filename().string();
    }
    // Whatever is left is legacy, and rare, so it still goes to the drive --
    // together rather than one at a time. See ParallelForEachIndex above.
    ParallelForEachIndex(probes.size(), [&](size_t index) {
        ProbedSource& probe = probes[index];
        if (!probe.needs_probe) return;
        probe.probed = ProbeMediaMetadata(probe.media_path, probe.detected,
                                          probe.probe_error);
    });
    for (size_t index = 0; index < openSources.size(); ++index) {
        const DocumentSource& source = openSources[index];
        const ProbedSource& probe = probes[index];
        if (!probe.needs_probe) continue;
        if (probe.probed) {
            const LibraryMedia& detected = probe.detected;
            self.state->mediaMetadata[source.id] = detected;
            std::fprintf(
                stderr,
                "Media %s: %dx%d, %s/%s, range=%s matrix=%s transfer=%s "
                "primaries=%s, rotation %d degrees, %s\n",
                detected.filename.c_str(), detected.width, detected.height,
                detected.codec.c_str(), detected.pixel_format.c_str(),
                detected.color_range.c_str(), detected.color_space.c_str(),
                detected.color_transfer.c_str(),
                detected.color_primaries.c_str(), detected.rotation_degrees,
                detected.orientation.c_str());
        } else {
            std::fprintf(stderr, "Metadata probe failed for %s: %s\n",
                         probe.media_path.c_str(), probe.probe_error.c_str());
        }
    }

    if (![self separateEmbeddedAudioByDefault:error]) {
        std::fprintf(stderr, "Unable to separate embedded audio: %s\n",
                     error.c_str());
        return NO;
    }
    self.state->audioPlayback = std::make_unique<AudioPlayback>();
    if (!self.state->audioPlayback->Open(self.state->document,
                                         baseDirectory.string(), error)) {
        std::fprintf(stderr, "Unable to initialize audio: %s\n", error.c_str());
        return NO;
    }
    // PERF-2026-09. The decoders are deliberately not opened here. Each one
    // is an avformat_open_input plus avformat_find_stream_info against the
    // media itself, and doing them before the window exists is what made
    // opening a project on an external drive show nothing at all for
    // seconds. Since PERF10 they open when the monitor first asks for their
    // source (-requestFrame:ofSource:), and
    // -checkSourceAvailabilityInBackground only checks, once the editor is on
    // screen, that each file is there.
    [self rebuildVideoTrackIds];
    [self reportPendingProjectArchive];
    return YES;
}

- (BOOL)separateEmbeddedAudioByDefault:(std::string&)message {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    std::vector<Ulid> videoClipIds;
    for (const DocumentTrack& track : stagedDocument.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (!clip.include_audio) continue;
            const auto detected =
                self.state->mediaMetadata.find(clip.source_id);
            const LibraryMedia* media =
                detected == self.state->mediaMetadata.end()
                    ? stagedDocument.FindLibraryMedia(clip.source_id)
                    : &detected->second;
            if (media && media->metadata_complete && media->has_audio)
                videoClipIds.push_back(clip.id);
        }
    }
    EditError error = EditError::None;
    for (const Ulid& videoClipId : videoClipIds) {
        const Ulid audioClipId = GenerateUlid();
        Ulid targetTrackId;
        for (const DocumentTrack* track :
             TimelineTracksInDisplayOrder(stagedDocument)) {
            if (track->kind != "audio") continue;
            Document candidate = stagedDocument;
            Operation probe =
                DetachAudioOperation{videoClipId, track->id, audioClipId, {}};
            Operation inverse = RemoveClipOperation{};
            EditError probeError = EditError::None;
            std::string probeMessage;
            if (ApplyOperation(candidate, probe, inverse, probeError,
                               probeMessage)) {
                targetTrackId = track->id;
                break;
            }
        }
        if (targetTrackId.empty()) {
            int32_t index = 0;
            for (const DocumentTrack& track : stagedDocument.sequence.tracks)
                index = std::max(index, track.index + 1);
            targetTrackId = GenerateUlid();
            if (!stagedLog.Apply(
                    stagedDocument,
                    Operation{AddTrackOperation{targetTrackId, "audio", index}},
                    error, message))
                return NO;
        }
        if (!stagedLog.Apply(stagedDocument,
                             Operation{DetachAudioOperation{
                                 videoClipId, targetTrackId, audioClipId, {}}},
                             error, message))
            return NO;
    }
    size_t migratedLinks = 0;
    std::vector<Ulid> claimedAudio;
    struct LegacyPair {
        Ulid video;
        Ulid audio;
        Ulid group;
    };
    std::vector<LegacyPair> legacyPairs;
    for (const DocumentTrack& videoTrack : stagedDocument.sequence.tracks) {
        if (videoTrack.kind != "video") continue;
        for (const DocumentClip& video : videoTrack.clips) {
            if (video.include_audio || !video.sync_anchor_clip_id.empty())
                continue;
            const DocumentClip* match = nullptr;
            for (const DocumentTrack& audioTrack :
                 stagedDocument.sequence.tracks) {
                if (audioTrack.kind != "audio") continue;
                for (const DocumentClip& audio : audioTrack.clips) {
                    if (!audio.sync_anchor_clip_id.empty() ||
                        std::find(claimedAudio.begin(), claimedAudio.end(),
                                  audio.id) != claimedAudio.end() ||
                        audio.source_id != video.source_id ||
                        audio.source_in != video.source_in ||
                        audio.duration != video.duration ||
                        audio.timeline_in != video.timeline_in)
                        continue;
                    match = &audio;
                    break;
                }
                if (match) break;
            }
            if (!match) continue;
            const Ulid audioId = match->id;
            const Ulid groupId =
                !video.link_group_id.empty()
                    ? video.link_group_id
                    : (!match->link_group_id.empty() ? match->link_group_id
                                                     : audioId);
            legacyPairs.push_back({video.id, audioId, groupId});
            claimedAudio.push_back(audioId);
        }
    }
    for (const auto& pair : legacyPairs) {
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{SetClipLinkOperation{pair.video, pair.audio,
                                               pair.group, pair.group}},
                error, message))
            return NO;
        ++migratedLinks;
    }
    if (!videoClipIds.empty() || migratedLinks > 0) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message])
            return NO;
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        std::fprintf(
            stderr,
            "Separated %zu embedded audio clip(s), migrated %zu A/V link(s)\n",
            videoClipIds.size(), migratedLinks);
    }
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    if (TestHooks() == nullptr) {
        // PRD35 -- ROADMAP.md. The app keeps the engine log; the window
        // test, which runs these objects, is a test binary and keeps none.
        engine_log::StartProcessLog("app", true);
        // DEC23 -- before the welcome screen reads the setup report. Not in
        // the window test: it would write the settings file of whoever runs
        // it.
        [self migrateWhisperModelPreference];
    }
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const BOOL pathWasProvided = self.documentPath.length > 0;
    while (true) {
        if (self.documentPath.length == 0 && ![self chooseStartupProject]) {
            [NSApp terminate:nil];
            return;
        }
        if ([self loadDocumentAndSources]) break;
        NSAlert* failure = [NSAlert new];
        failure.alertStyle = NSAlertStyleCritical;
        failure.messageText = @"Impossible d’ouvrir le projet";
        failure.informativeText =
            self.lastProjectLoadError
                ?: @"Le fichier n’est pas un projet CUTMACHINE valide ou il "
                   @"est inaccessible.";
        [failure runModal];
        if (pathWasProvided) {
            [NSApp terminate:nil];
            return;
        }
        self.documentPath = nil;
        delete self.state;
        self.state = new AppState();
        self.state->automaticProxiesEnabled =
            [NSUserDefaults.standardUserDefaults
                boolForKey:kAutomaticProxyGenerationDefaultsKey];
        self.state->programVideoScope = VideoScopeModeFromPreference(
            static_cast<int32_t>([NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey]));
        [self loadMonitorCompare];
        self.mediaThumbnails = [NSMutableDictionary dictionary];
    }
    [self recordRecentProject:self.documentPath];
    [self installApplicationMenus];

    const NSRect windowRect = NSMakeRect(0.0, 0.0, 1600.0, 960.0);
    self.window =
        [[NSWindow alloc] initWithContentRect:windowRect
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskResizable)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = CMWindowTitle(self.state->project.name,
                                      self.state->document.sequence.name);
    self.window.delegate = self;
    self.window.acceptsMouseMovedEvents = YES;
    self.window.contentMinSize = NSMakeSize(900.0, 560.0);

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    content.wantsLayer = YES;
    content.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfaceBase).CGColor;
    self.window.contentView = content;
    constexpr double mediaPanelWidth = 320.0;
    // F2.1 design system: fixed-width right dock (Inspector/Chat tabs).
    // Fixed, not user-resized-and-remembered, on purpose -- see
    // PanelHostView.h.
    constexpr double rightDockWidth = 300.0;
    // UI-2026-08 -- transport and zoom now belong to the timeline display
    // list. The only fixed bottom strip is the 42 pt status surface.
    const double workspaceHeight = windowRect.size.height - 42.0;
    self.workspaceSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 42.0, windowRect.size.width,
                                 workspaceHeight)];
    self.workspaceSplitView.vertical = YES;
    self.workspaceSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.workspaceSplitView.delegate = self;
    self.workspaceSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    // ON1 -- docs/roadmap/ONGLETS-2026-10.md. No autosaved geometry: the
    // panes a page shows, and how wide, come from the page and the widths
    // the hand gave it (UiPreferences.h), and the number of panes changes
    // from one page to the next. Autosaved geometry is positional, and a
    // two-pane frame restored onto three panes bricked the window once
    // (the old ".3pane" name retired it then).
    [content addSubview:self.workspaceSplitView];
    self.mediaPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth, workspaceHeight)];
    self.mediaPanel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.mediaPanel.wantsLayer = YES;
    // Same surface token as the new right dock (UiTheme.h kSurfacePanel) --
    // both are side panels of the same visual kind.
    self.mediaPanel.layer.backgroundColor = CMSurfacePanelColor().CGColor;
    [self.workspaceSplitView addArrangedSubview:self.mediaPanel];

    // F2.3 -- ROADMAP.md: the Média/Audio/Légendes tabs of the one
    // PanelSlot::Media (MediaPanelModel.h), a strip off the top and one
    // container per tab under it. UI-2026-08 had left the dock a media
    // browser alone (#12); they are back since the visual QA of 19 September
    // 2026, the caption tab dressing cartons only.
    const double mediaContentHeight =
        workspaceHeight - ui::theme::kTabStripHeight;
    NSMutableArray<NSString*>* mediaTabTitles = [NSMutableArray array];
    for (ui::media_panel::Tab tab : ui::media_panel::AllTabs())
        [mediaTabTitles
            addObject:[NSString
                          stringWithUTF8String:ui::media_panel::TabTitle(tab)]];
    self.mediaTabStrip = [[CMTabStripView alloc]
        initWithFrame:NSMakeRect(0.0, mediaContentHeight, mediaPanelWidth,
                                 ui::theme::kTabStripHeight)
               titles:mediaTabTitles];
    self.mediaTabStrip.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    self.mediaTabStrip.target = self;
    self.mediaTabStrip.action = @selector(mediaTabChanged:);
    [self.mediaPanel addSubview:self.mediaTabStrip];
    constexpr double binSidebarX = 8.0;
    constexpr double binSidebarWidth = 112.0;
    constexpr double mediaBrowserX = 128.0;
    constexpr double mediaBrowserWidth = mediaPanelWidth - mediaBrowserX - 8.0;

    self.mediaTabContentMedia =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                                 mediaContentHeight)];
    self.mediaTabContentMedia.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentMedia];

    NSTextField* libraryTitle =
        [NSTextField labelWithString:@"PROJET / CHUTIERS"];
    libraryTitle.frame = NSMakeRect(14.0, mediaContentHeight - 34.0,
                                    mediaPanelWidth - 28.0, 18.0);
    libraryTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    libraryTitle.font = [NSFont systemFontOfSize:11.0
                                          weight:NSFontWeightSemibold];
    libraryTitle.textColor = NSColor.secondaryLabelColor;
    [self.mediaTabContentMedia addSubview:libraryTitle];

    self.binOutline = [[ContextOutlineView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, binSidebarWidth,
                                 mediaContentHeight - 154.0)];
    NSTableColumn* binColumn =
        [[NSTableColumn alloc] initWithIdentifier:@"bin"];
    [self.binOutline addTableColumn:binColumn];
    self.binOutline.outlineTableColumn = binColumn;
    self.binOutline.headerView = nil;
    self.binOutline.dataSource = self;
    self.binOutline.delegate = self;
    self.binOutline.rowHeight = 26.0;
    self.binOutline.indentationPerLevel = 12.0;
    self.binOutline.backgroundColor = CMSurfacePanelColor();
    binColumn.width = binSidebarWidth;
    // PRD37 -- ROADMAP.md. A deep bin does not widen the column further: its
    // name ends in an ellipsis at the list's edge instead (« 1_RUSH »,
    // « Chutier re » cut there, QA of 3 October 2026; ContextOutlineView
    // keeps every cell inside the list).
    self.binOutline.autoresizesOutlineColumn = NO;
    // And a selected bin is drawn in the theme's row
    // (-outlineView:rowViewForItem:, UiTheme.h kListRowSelected), with
    // nothing of AppKit's over it: its blue, or its grey once the list is
    // not focused, covered that row and left the name at 2.05:1.
    self.binOutline.selectionHighlightStyle =
        NSTableViewSelectionHighlightStyleNone;
    [self.binOutline registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        kCutmachineTimelinePasteboardType, NSPasteboardTypeFileURL
    ]];
    [self.binOutline setDraggingSourceOperationMask:NSDragOperationMove
                                           forLocal:YES];
    NSScrollView* binScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(binSidebarX, 112.0, binSidebarWidth,
                                 mediaContentHeight - 154.0)];
    binScroll.autoresizingMask = NSViewHeightSizable | NSViewMaxXMargin;
    binScroll.documentView = self.binOutline;
    binScroll.hasVerticalScroller = YES;
    binScroll.borderType = NSNoBorder;
    binScroll.drawsBackground = YES;
    binScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentMedia addSubview:binScroll];
    self.binSidebarScroll = binScroll;

    self.mediaSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(mediaBrowserX, mediaContentHeight - 70.0,
                                 mediaBrowserWidth - 80.0, 26.0)];
    // PRD37 -- ROADMAP.md. No prompt in the field: forty points of text room
    // at the QA window's width held « Reche » of « Rechercher nom, codec,
    // format… », and a narrower window none of it (QA of 3 October 2026).
    // The magnifier says search, as in the editors the timelines go to; what
    // is searched is said where nothing cuts it. Empty, not nil: a nil
    // prompt is AppKit's own « Search », in English and cut as well.
    self.mediaSearchField.placeholderString = @"";
    self.mediaSearchField.toolTip = @"Rechercher par nom, codec ou format";
    self.mediaSearchField.accessibilityLabel =
        @"Rechercher par nom, codec ou format";
    self.mediaSearchField.target = self;
    self.mediaSearchField.action = @selector(mediaSearchChanged:);
    self.mediaSearchField.continuous = YES;
    self.mediaSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentMedia addSubview:self.mediaSearchField];

    self.mediaViewToggle = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 80.0,
                                 mediaContentHeight - 70.0, 72.0, 26.0)];
    self.mediaViewToggle.segmentCount = 2;
    [self.mediaViewToggle setImage:SystemSymbol(@"list.bullet", @"Vue liste")
                        forSegment:0];
    [self.mediaViewToggle
          setImage:SystemSymbol(@"square.grid.2x2", @"Vue grille")
        forSegment:1];
    // PRD27 -- the two segments carry only a symbol: VoiceOver reads the
    // description SystemSymbol gives each image, the pointer gets it too.
    [self.mediaViewToggle setToolTip:@"Vue liste" forSegment:0];
    [self.mediaViewToggle setToolTip:@"Vue grille" forSegment:1];
    self.mediaViewToggle.selectedSegment = 1;
    self.mediaViewToggle.target = self;
    self.mediaViewToggle.action = @selector(mediaViewChanged:);
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin;
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin | NSViewMinXMargin;
    [self.mediaTabContentMedia addSubview:self.mediaViewToggle];

    self.mediaTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"145" ], @[ @"format", @"Format", @"85" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.mediaTable addTableColumn:column];
    }
    self.mediaTable.dataSource = self;
    self.mediaTable.delegate = self;
    self.mediaTable.allowsMultipleSelection = YES;
    self.mediaTable.rowHeight = 44.0;
    self.mediaTable.usesAlternatingRowBackgroundColors = NO;
    self.mediaListScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(mediaBrowserX, 112.0, mediaBrowserWidth,
                                 mediaContentHeight - 190.0)];
    self.mediaListScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaListScroll.documentView = self.mediaTable;
    self.mediaListScroll.hasVerticalScroller = YES;
    self.mediaListScroll.hasHorizontalScroller = YES;
    self.mediaListScroll.borderType = NSBezelBorder;
    [self.mediaTabContentMedia addSubview:self.mediaListScroll];

    NSCollectionViewFlowLayout* iconLayout =
        [[NSCollectionViewFlowLayout alloc] init];
    iconLayout.itemSize = NSMakeSize(132.0, 112.0);
    iconLayout.minimumInteritemSpacing = 6.0;
    iconLayout.minimumLineSpacing = 8.0;
    iconLayout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);
    self.mediaCollection = [[ContextCollectionView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaBrowserWidth, 500.0)];
    self.mediaCollection.collectionViewLayout = iconLayout;
    self.mediaCollection.dataSource = self;
    self.mediaCollection.delegate = self;
    self.mediaCollection.selectable = YES;
    self.mediaCollection.allowsMultipleSelection = YES;
    [self.mediaCollection
        setDraggingSourceOperationMask:kMediaLocalDragOperations
                              forLocal:YES];
    [self.mediaCollection setDraggingSourceOperationMask:NSDragOperationCopy
                                                forLocal:NO];
    [self.mediaCollection registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        kCutmachineTimelinePasteboardType, NSPasteboardTypeFileURL
    ]];
    [self.mediaCollection registerClass:MediaIconItem.class
                  forItemWithIdentifier:@"media-icon"];
    self.mediaIconScroll =
        [[NSScrollView alloc] initWithFrame:self.mediaListScroll.frame];
    self.mediaIconScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaIconScroll.documentView = self.mediaCollection;
    self.mediaIconScroll.hasVerticalScroller = YES;
    self.mediaIconScroll.borderType = NSBezelBorder;
    self.mediaListScroll.hidden = YES;
    self.mediaIconScroll.hidden = NO;
    [self.mediaTabContentMedia addSubview:self.mediaIconScroll];

    self.mediaTable.target = self;
    self.mediaTable.doubleAction = @selector(openSelectedMediaInSourceMonitor:);
    ContextCollectionView* iconCollection =
        (ContextCollectionView*)self.mediaCollection;
    iconCollection.doubleClickTarget = self;
    iconCollection.doubleClickAction = @selector(openIconItem:);
    [self.mediaTable setDraggingSourceOperationMask:kMediaLocalDragOperations
                                           forLocal:YES];

    self.assignMediaButton = CMMakeStyledButton(@"IMPORTER DES RUSHES…", self,
                                                @selector(importMediaPressed:));
    self.assignMediaButton.frame = NSMakeRect(12.0, 76.0, 184.0, 28.0);
    // PRD37 -- it takes what the row has left of SOURCE, wider or narrower.
    self.assignMediaButton.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    self.assignMediaButton.image =
        SystemSymbol(@"square.and.arrow.down", @"Importer des rushes");
    self.assignMediaButton.imagePosition = NSImageLeading;
    [self.mediaTabContentMedia addSubview:self.assignMediaButton];

    self.sourceMonitorButton = CMMakeStyledButton(
        @"SOURCE", self, @selector(openSelectedMediaInSourceMonitor:));
    self.sourceMonitorButton.frame = NSMakeRect(202.0, 76.0, 106.0, 28.0);
    // PRD37 -- ROADMAP.md. Its mask was set twice, the second one kept: the
    // button followed the panel's right edge over a neighbour of fixed
    // width, and a narrower window put SOURCE on « IMPORTER DES RUSHES… »
    // (QA of 3 October 2026). It still follows that edge; the import button
    // gives up the room instead (its mask above).
    self.sourceMonitorButton.autoresizingMask =
        NSViewMinXMargin | NSViewMaxYMargin;
    self.sourceMonitorButton.image =
        SystemSymbol(@"rectangle.on.rectangle", @"Moniteur Source");
    self.sourceMonitorButton.imagePosition = NSImageLeading;
    self.sourceMonitorButton.toolTip =
        @"Ouvre le média sélectionné dans le moniteur source";
    [self.mediaTabContentMedia addSubview:self.sourceMonitorButton];

    NSMenu* binContext = [[NSMenu alloc] initWithTitle:@"Chutier"];
    [binContext addItem:[self menuItem:@"Nouveau chutier"
                                action:@selector(createBinPressed:)
                                   key:@""]];
    [binContext addItem:[self menuItem:@"Renommer"
                                action:@selector(renameBinPressed:)
                                   key:@""]];
    [binContext addItem:NSMenuItem.separatorItem];
    [binContext addItem:[self menuItem:@"Supprimer"
                                action:@selector(deleteBinPressed:)
                                   key:@""]];
    self.binOutline.menu = binContext;

    NSMenu* mediaContext = [[NSMenu alloc] initWithTitle:@"Média"];
    [mediaContext addItem:[self menuItem:@"Nouveau chutier"
                                  action:@selector(createBinPressed:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext
        addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                        action:@selector(openSelectedMediaInSourceMonitor:)
                           key:@""]];
    // PRD39 -- ROADMAP.md. It asks for the destination: it said it moved
    // into the selected bin.
    [mediaContext addItem:[self menuItem:@"Déplacer vers un chutier…"
                                  action:@selector(assignMediaToBinPressed:)
                                     key:@""]];
    [mediaContext
        addItem:[self menuItem:@"Nouvelle timeline avec la sélection"
                        action:@selector(createTimelineFromSelectedMedia:)
                           key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Révéler dans le Finder"
                                  action:@selector(revealSelectedMediaInFinder:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Reconnecter le média…"
                                  action:@selector(relinkSelectedMedia:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Reconnecter les médias offline…"
                                  action:@selector(batchRelinkOfflineMedia:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    // G3 -- docs/roadmap/GRADE-2026-09.md. The colour input the selected
    // media are read as: the choices MediaPanelModel.h lists, checked in
    // -validateMenuItem:, and the file read again as ingest reads it.
    NSMenu* colorInputMenu =
        [[NSMenu alloc] initWithTitle:@"Espace couleur d’entrée"];
    const auto& colorInputChoices = ui::media_panel::ColorInputChoices();
    for (size_t index = 0; index < colorInputChoices.size(); ++index) {
        NSMenuItem* choice =
            [self menuItem:CMStringFromUtf8(colorInputChoices[index].label)
                    action:@selector(setSelectedMediaColorInput:)
                       key:@""];
        choice.tag = static_cast<NSInteger>(index);
        [colorInputMenu addItem:choice];
        if (index == 0) [colorInputMenu addItem:NSMenuItem.separatorItem];
    }
    [colorInputMenu addItem:NSMenuItem.separatorItem];
    [colorInputMenu
        addItem:[self menuItem:@"Relire dans le fichier"
                        action:@selector(rereadSelectedMediaColorInput:)
                           key:@""]];
    NSMenuItem* colorInputItem =
        [[NSMenuItem alloc] initWithTitle:@"Espace couleur d’entrée"
                                   action:nil
                            keyEquivalent:@""];
    colorInputItem.submenu = colorInputMenu;
    [mediaContext addItem:colorInputItem];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Transcrire avec Whisper…"
                                  action:@selector(transcribeSelectedMedia:)
                                     key:@""]];
    [mediaContext
        addItem:[self menuItem:@"Transcrire les hésitations (français)…"
                        action:@selector(transcribeSelectedMediaHesitations:)
                           key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Générer / recréer le proxy"
                                  action:@selector(generateSelectedMediaProxy:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Supprimer le proxy"
                                  action:@selector(removeSelectedMediaProxy:)
                                     key:@""]];
    [mediaContext
        addItem:[self menuItem:@"Régénérer la vignette"
                        action:@selector(regenerateSelectedMediaThumbnail:)
                           key:@""]];
    self.mediaTable.menu = mediaContext;
    self.mediaCollection.menu = mediaContext;

    self.binSummaryLabel = [NSTextField labelWithString:@""];
    self.binSummaryLabel.frame =
        NSMakeRect(14.0, 42.0, mediaPanelWidth - 28.0, 34.0);
    self.binSummaryLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.binSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.binSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.binSummaryLabel.maximumNumberOfLines = 2;
    [self.mediaTabContentMedia addSubview:self.binSummaryLabel];
    self.mediaTaskLabel = [NSTextField labelWithString:@"Tâches média : prêt"];
    self.mediaTaskLabel.frame =
        NSMakeRect(14.0, 13.0, mediaPanelWidth - 152.0, 18.0);
    self.mediaTaskLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.mediaTaskLabel.font = [NSFont systemFontOfSize:10.0];
    self.mediaTaskLabel.textColor = NSColor.tertiaryLabelColor;
    self.mediaTaskLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.mediaTabContentMedia addSubview:self.mediaTaskLabel];
    self.mediaTaskProgress = [[NSProgressIndicator alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 132.0, 16.0, 88.0, 10.0)];
    self.mediaTaskProgress.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskProgress.minValue = 0.0;
    self.mediaTaskProgress.maxValue = 100.0;
    self.mediaTaskProgress.doubleValue = 0.0;
    self.mediaTaskProgress.indeterminate = NO;
    // PRD37 -- shown while a task waits or runs (-refreshMediaTaskStatus).
    self.mediaTaskProgress.hidden = YES;
    [self.mediaTabContentMedia addSubview:self.mediaTaskProgress];
    self.mediaTaskCancelButton =
        CMMakeStyledButton(@"×", self, @selector(cancelDisplayedMediaTask:));
    self.mediaTaskCancelButton.frame =
        NSMakeRect(mediaPanelWidth - 40.0, 7.0, 28.0, 26.0);
    self.mediaTaskCancelButton.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskCancelButton.toolTip = @"Annuler la tâche média active";
    // PRD27 -- VoiceOver would read « × ».
    self.mediaTaskCancelButton.accessibilityLabel =
        self.mediaTaskCancelButton.toolTip;
    self.mediaTaskCancelButton.enabled = NO;
    [self.mediaTabContentMedia addSubview:self.mediaTaskCancelButton];

    [self buildAudioTabContentWithWidth:mediaPanelWidth
                                 height:mediaContentHeight];
    [self buildCaptionsTabContentWithWidth:mediaPanelWidth
                                    height:mediaContentHeight];
    self.mediaTabContentAudio.hidden = YES;
    self.mediaTabContentCaptions.hidden = YES;
    // EF1 -- docs/roadmap/EFFETS-2026-10.md. The effects library.
    self.effectsPanelView = [[CMEffectsPanelView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                 mediaContentHeight)];
    self.effectsPanelView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.effectsPanelView.delegate = self;
    self.effectsPanelView.hidden = YES;
    [self.mediaPanel addSubview:self.effectsPanelView];
    NSString* rememberedMediaTab =
        [CMUiPreferences.sharedPreferences lastMediaPanelTab];
    if (const auto tab = ui::media_panel::TabFromIdentifier(
            rememberedMediaTab.UTF8String ?: "")) {
        const auto& tabs = ui::media_panel::AllTabs();
        const auto found = std::find(tabs.begin(), tabs.end(), *tab);
        if (found != tabs.begin() && found != tabs.end())
            [self.mediaTabStrip
                selectIndex:static_cast<NSInteger>(found - tabs.begin())];
    }

    const double editorWidth =
        windowRect.size.width - mediaPanelWidth - rightDockWidth - 2.0;
    self.editorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, workspaceHeight)];
    self.editorSplitView.vertical = NO;
    self.editorSplitView.arrangesAllSubviews = NO;
    self.editorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.editorSplitView.delegate = self;
    // v5 retires every earlier key: v4 and before were written while the panes
    // were inserted in the reverse of their visual order and their frames
    // rewritten afterwards, so the saved coordinates describe a composition
    // this build no longer produces.
    self.editorSplitView.autosaveName =
        @"CUTMACHINE.EditorSplit.MonitorsTop.v5";
    self.editorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;

    // NSSplitView is flipped: arranged subview 0 is the *top* pane of a
    // horizontal split and -setPosition: measures from the top edge. The
    // monitors therefore go in first and the timeline second -- the order
    // they appear on screen -- and AppKit needs no help holding them there.
    const double editorDivider = self.editorSplitView.dividerThickness;
    const double monitorPaneHeight = workspaceHeight * 0.44;
    const double timelinePaneTop = monitorPaneHeight + editorDivider;
    self.metalView = [[TimelineMetalView alloc]
        initWithFrame:NSMakeRect(0.0, timelinePaneTop, editorWidth,
                                 workspaceHeight - timelinePaneTop)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.metalView.eventTarget = self;
    self.metalView.resizeTarget = self;
    self.metalView.acceptsEffects = YES;
    // ON8 -- docs/roadmap/ONGLETS-2026-10.md. The trim bar, in the tool row.
    self.trimBarView = [[CMTrimBarView alloc] initWithFrame:NSZeroRect];
    self.trimBarView.delegate = self;
    [self.metalView addSubview:self.trimBarView];
    self.monitorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, monitorPaneHeight)];
    self.monitorSplitView.vertical = YES;
    self.monitorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.monitorSplitView.delegate = self;
    // The two viewers are one comparison surface: while Source is visible,
    // both panes always receive exactly half of the available width.
    self.monitorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorPanel =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, editorWidth * 0.5,
                                                 monitorPaneHeight)];
    self.programMonitorPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(editorWidth * 0.5, 0.0, editorWidth * 0.5,
                                 monitorPaneHeight)];
    for (NSView* panel in
         @[ self.sourceMonitorPanel, self.programMonitorPanel ]) {
        panel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        panel.wantsLayer = YES;
        panel.layer.backgroundColor = NSColor.blackColor.CGColor;
    }
    self.sourceMonitorView = [[TimelineMetalView alloc]
        initWithFrame:self.sourceMonitorPanel.bounds];
    self.sourceMonitorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorView.eventTarget = self;
    self.sourceMonitorView.resizeTarget = self;
    [self.sourceMonitorPanel addSubview:self.sourceMonitorView];
    self.programMonitorView = [[TimelineMetalView alloc]
        initWithFrame:self.programMonitorPanel.bounds];
    self.programMonitorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.programMonitorView.eventTarget = self;
    self.programMonitorView.resizeTarget = self;
    [self.programMonitorPanel addSubview:self.programMonitorView];
    // PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. The three Metal surfaces
    // were silent to VoiceOver. Each is a group named as the window names it
    // -- a group, so that the controls laid over a monitor stay inside it,
    // within reach -- and the timeline has a value, asked each time VoiceOver
    // reads it (UiAccessibility.h).
    const auto nameForVoiceOver = [](NSView* view, const char* label) {
        view.accessibilityElement = YES;
        view.accessibilityRole = NSAccessibilityGroupRole;
        view.accessibilityLabel = CMStringFromUtf8(label);
    };
    nameForVoiceOver(self.metalView, ui::accessibility::kTimelineLabel);
    nameForVoiceOver(self.sourceMonitorView,
                     ui::accessibility::kSourceMonitorLabel);
    nameForVoiceOver(self.programMonitorView,
                     ui::accessibility::kRecordMonitorLabel);
    __weak AppDelegate* valueOwner = self;
    self.metalView.accessibilityValueProvider = ^NSString* {
      return [valueOwner timelineAccessibilityValue];
    };
    // REM5 -- the motion selection, drawn over Record, taking no event.
    self.motionCanvasView = [[CMMotionCanvasView alloc]
        initWithFrame:self.programMonitorView.bounds];
    self.motionCanvasView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.motionCanvasView.hidden = YES;
    [self.programMonitorView addSubview:self.motionCanvasView];
    [self.monitorSplitView addArrangedSubview:self.sourceMonitorPanel];
    [self.monitorSplitView addArrangedSubview:self.programMonitorPanel];
    [self.monitorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                            forSubviewAtIndex:0];
    [self.monitorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                            forSubviewAtIndex:1];
    // -adjustSubviews before every -setPosition:ofDividerAtIndex: below, and
    // never the other way round. Freshly added arranged subviews all still
    // sit at x/y 0 -- the split view has not placed them relative to each
    // other yet -- and setting a divider position against overlapping frames
    // makes AppKit resolve the conflict by collapsing panes to zero width.
    // -adjustSubviews lays them out first, so each -setPosition: is then a
    // small nudge from a valid arrangement rather than a guess from garbage.
    [self.monitorSplitView adjustSubviews];
    [self.monitorSplitView setPosition:editorWidth * 0.5 ofDividerAtIndex:0];
    // Monitors above, timeline below -- see the flipped-coordinates note next
    // to editorSplitView's autosaveName. Divider 0 is then the boundary the
    // -constrain{Min,Max}Coordinate: pair below already describes: at least
    // 180 pt of monitors above it, at least 200 pt of timeline under it.
    [self.editorSplitView insertArrangedSubview:self.monitorSplitView
                                        atIndex:0];
    [self.editorSplitView insertArrangedSubview:self.metalView atIndex:1];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:0];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:1];
    [self.editorSplitView adjustSubviews];
    [self.editorSplitView setPosition:monitorPaneHeight ofDividerAtIndex:0];
    [self.workspaceSplitView addArrangedSubview:self.editorSplitView];

    // F2.1 design system: fixed right dock, tabs = FixedPanelLayout()'s
    // PanelDock::Right slots -- the Inspector alone since ON3 took the Agent
    // into its own window (-buildAgentWindow), so the host draws no tab
    // strip. F2.2 calls -setContentView:forSlot: once at startup to replace
    // its placeholder with real content; nothing else about this dock's
    // shape changes at runtime.
    self.rightDockPanel = [[CMPanelHostView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)
                 dock:ui::PanelDock::Right];
    self.rightDockPanel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.workspaceSplitView addArrangedSubview:self.rightDockPanel];

    // F2.2 -- Inspector: clip properties + the eight F1.3 color.* grading
    // knobs (ColorEffects.h), sliders built on CMControlRowView. Every
    // slider edit becomes a SetClipEffectsOperation, delivered here via
    // -inspectorView:didCommitClipEffects: (below) and applied through
    // self.state->editLog -- the same EditLog::Apply path CLI/MCP already
    // use for this operation (PHILOSOPHY.md principle 3, "aucune surface
    // n'est privilégiée"). See InspectorView.h/.mm for the view itself.
    self.inspectorView = [[CMInspectorView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.inspectorView.delegate = self;
    self.inspectorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.rightDockPanel setContentView:self.inspectorView
                                forSlot:ui::PanelSlot::Inspector];
    // REM5 -- swapped in while a motion node is selected on Record.
    self.motionInspectorView = [[CMMotionInspectorView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.motionInspectorView.delegate = self;
    self.motionInspectorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;

    // F2.4 -- ROADMAP.md: chat panel, wired to the exact same in-memory
    // Document/EditLog `self.state->editLog.Apply(self.state->document, ...)`
    // already mutates everywhere else in this file, through
    // McpLiveBackend -> McpToolRegistry::Call -- the identical dispatcher
    // McpServer.cc's `tools/call` uses for an external MCP client. No
    // second edit path: see McpLiveBackend.h/ChatSession.h for why. `state`
    // is allocated once, above, and never reallocated after the window is
    // built (only its fields are mutated in place by ordinary edits and
    // project loads), so the reference this backend holds stays valid for
    // the window's lifetime.
    __weak AppDelegate* weakSelf = self;
    self.chatBackend = new McpLiveBackend(
        self.state->document, self.state->editLog,
        [weakSelf]() {
            // Called once the edit is committed (SetEditCommitCallback
            // below) and the window's document and journal hold it.
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) return;
            const RationalTime playhead = strongSelf.state->requestedPosition;
            // PL1 -- ROADMAP.md. The agent's last step is a timeline
            // step: Cmd+Z must reach it, not a project step taken
            // before the turn (a rename, a timeline created earlier).
            strongSelf.state->lastHistoryDomain = HistoryDomain::Timeline;
            [strongSelf refreshTimelineAfterEditFromPosition:playhead];
            [strongSelf rebuildMediaList];
        },
        [weakSelf](ProjectOperation operation, std::vector<EntryMotif> motifs,
                   std::string& resultJson, std::string& errorName,
                   std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            if (![strongSelf persistEdits:message]) {
                errorName = "IoError";
                return false;
            }
            Project candidate = strongSelf.state->project;
            ProjectEditLog projectLog = strongSelf.state->projectEditLog;
            EditError editError = EditError::None;
            // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The agent's motif
            // rides on the project journal's entry.
            if (!projectLog.Apply(candidate, std::move(operation), editError,
                                  message, std::move(motifs))) {
                errorName = EditErrorName(editError);
                return false;
            }
            const Ulid createdId = candidate.active_timeline_id;
            if (![strongSelf commitProjectCandidate:candidate
                                            editLog:strongSelf.state->editLog
                                         projectLog:projectLog
                                            message:message]) {
                errorName = "IoError";
                return false;
            }
            strongSelf.state->project = std::move(candidate);
            strongSelf.state->projectEditLog = std::move(projectLog);
            strongSelf.state->lastHistoryDomain = HistoryDomain::Project;
            // AX1 -- docs/roadmap/AX-2026-09.md. A project edit may change
            // the timeline on screen (rename_timeline on the active one):
            // the window's document is rebuilt from the project, as the
            // Bin panel's own rename does. It was not, and the next
            // timeline edit wrote the old name back over the rename.
            if (strongSelf.state->project.FindTimeline(
                    strongSelf.state->activeTimelineId) != nullptr) {
                strongSelf.state->document =
                    strongSelf.state->project.MakeDocument(
                        strongSelf.state->activeTimelineId);
                strongSelf.programMonitorTitleLabel.stringValue =
                    CMRecordTitle(strongSelf.state->document.sequence.name);
                strongSelf.window.title =
                    CMWindowTitle(strongSelf.state->project.name,
                                  strongSelf.state->document.sequence.name);
            }
            [strongSelf rebuildMediaList];
            if (![strongSelf
                    activateTimelineIdentifier:
                        [NSString stringWithUTF8String:createdId.c_str()]]) {
                errorName = "UnknownSequence";
                message = "the created timeline could not be activated";
                return false;
            }
            resultJson = "{\"ok\":true,\"timeline_id\":\"" + createdId + "\"}";
            return true;
        },
        [weakSelf](std::string& json, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            // QA pass of 8 October 2026 -- the timeline the window shows,
            // which every edit of this backend changes (each is committed to
            // the project before the call returns). The project's own active
            // timeline is the one it was opened on: once another was chosen
            // in the window, an agent described the first and edited the
            // second.
            Project shown = strongSelf.state->project;
            shown.active_timeline_id = strongSelf.state->activeTimelineId;
            json = DescribeProject(shown);
            return true;
        },
        [weakSelf](std::string& json, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return DescribeTimelineTranscriptForAgent(
                strongSelf.state->document,
                projectPath.parent_path() / ".cutmachine" / "transcripts", json,
                message);
        },
        [weakSelf](const Ulid& sourceId, Transcript& transcript,
                   std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return LoadSourceWords(projectPath.parent_path() / ".cutmachine",
                                   sourceId, transcript, message);
        },
        [weakSelf](const Ulid& sourceId, ShotQualityReport& report) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                return ShotQualityCacheRead{
                    ShotQualityCacheState::Absent, 0,
                    "the application window is no longer available"};
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return ReadShotQualityCache(
                (projectPath.parent_path() / ".cutmachine" / "shotquality" /
                 (sourceId + ".json"))
                    .string(),
                report);
        },
        [weakSelf](const Ulid& sourceId, SpeechOnsetReport& report,
                   std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return LoadSpeechOnset((projectPath.parent_path() / ".cutmachine" /
                                    "speechonset" / (sourceId + ".json"))
                                       .string(),
                                   report, message);
        },
        [weakSelf](const Ulid& timelineId, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            if (![strongSelf
                    activateTimelineIdentifier:
                        [NSString stringWithUTF8String:timelineId.c_str()]]) {
                message = "unknown timeline_id '" + timelineId + "'";
                return false;
            }
            return true;
        },
        [[CMUiPreferences sharedPreferences]
            requiresExplicitTimelineForAgentEdits]);
    self.chatPanelView = [[CMChatPanelView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.chatBackend->SetProjectPath(self.documentPath.UTF8String ?: "");
    // SAVING_ROADMAP.md, a write refused under the editor (S3) -- 24 October
    // 2026. An edit of the Agent panel or of an agent relayed to the window
    // is committed as a mouse edit is (-applyAndPersistTimelineOperation:):
    // the staged document and journal first, the window's own only once
    // they are saved. A refused write leaves the window as it was, says so
    // under the timeline, and the caller is refused by name; it used to be
    // answered `ok` over an edit the window had just undone.
    self.chatBackend->SetEditCommitCallback([weakSelf](const Document& staged,
                                                       const EditLog& stagedLog,
                                                       std::string& message) {
        AppDelegate* strongSelf = weakSelf;
        if (strongSelf == nullptr || strongSelf.state == nullptr) {
            message = "the application window is no longer available";
            return false;
        }
        if (![strongSelf persistStagedDocumentNow:staged
                                          editLog:stagedLog
                                          message:message]) {
            std::fprintf(stderr, "Unable to persist an agent's edit: %s\n",
                         message.c_str());
            strongSelf.infoLabel.stringValue = [NSString
                stringWithFormat:@"Modification de l'agent refusée, rien n'a "
                                 @"changé : %@",
                                 CMStringFromUtf8(message)];
            return false;
        }
        return true;
    });
    // CHAT-PARITE -- ROADMAP.md F2.4 and P7. A transcription asked for from
    // the chat panel runs on the task manager the Media menu's uses: the
    // task bar shows its progress and cancels it, and quitting stops it. The
    // chat worker waits for it; the main thread never does. `state` and its
    // manager outlive the chat panel (see above). CHAT-ANALYSES -- so do the
    // shot quality, speech onset and beat analyses the chat asks for.
    MediaTaskManager* editorTasks = self.state->mediaTasks.get();
    // Visual QA of 19 September 2026 -- the tasks the chat started, so its
    // Stop button cancels them and nothing else the editor is running.
    struct ChatTasks {
        std::mutex mutex;
        std::set<Ulid> running;
    };
    const auto chatTasks = std::make_shared<ChatTasks>();
    self.chatBackend->SetMediaTaskRunner(
        [editorTasks, chatTasks](MediaTaskKind kind, std::string label,
                                 MediaTaskManager::Work work) {
            Ulid started;
            const MediaTaskSnapshot finished = RunMediaTaskAndWait(
                *editorTasks, kind, std::move(label), std::move(work),
                kMediaTaskWaitLimitMilliseconds, [&](const Ulid& id) {
                    started = id;
                    std::lock_guard<std::mutex> lock(chatTasks->mutex);
                    chatTasks->running.insert(id);
                });
            std::lock_guard<std::mutex> lock(chatTasks->mutex);
            chatTasks->running.erase(started);
            return finished;
        });
    // F2.4 -- ROADMAP.md. ingest_media, asked by the Agent panel or by an
    // agent relayed to the window, is read on that runner and ends in the
    // window's own import, as one from the File menu: the rushes reach the
    // browser with their thumbnails and waveforms, and the line under it
    // says what came in. No target bin, as for --ingest, unless the agent
    // names one (DEC32, PRD7).
    self.chatBackend->SetIngestCommitCallback(
        [weakSelf](const std::vector<ProbedIngestFile>& files,
                   const std::optional<Ulid>& targetBin, IngestOutcome& outcome,
                   std::string& errorName, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (strongSelf == nullptr || strongSelf.state == nullptr) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path documentPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return static_cast<bool>([strongSelf commitProbedMedia:files
                                                      documentPath:documentPath
                                                         targetBin:targetBin
                                                           outcome:outcome
                                                         errorName:errorName
                                                           message:message]);
        });
    self.chatBackend->SetProjectSnapshotCallback(
        [weakSelf](Project& project, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (strongSelf == nullptr) {
                message = "the application window is no longer available";
                return false;
            }
            project = strongSelf.state->project;
            return project.CommitDocument(strongSelf.state->activeTimelineId,
                                          strongSelf.state->document, message);
        });
    // OR2 -- publish the engine's returned state in the window only after
    // its edit, journals and receipt have committed together.
    self.chatBackend->SetRunStepCommitCallback(
        [weakSelf](const RunStepRequest& request, std::string& result,
                   std::string& errorName, std::string& message) {
            AppDelegate* editor = weakSelf;
            if (editor == nil || editor.state == nullptr ||
                editor.documentPath.length == 0) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            Project observed;
            if (!editor.chatBackend->SnapshotProject(observed, message)) {
                errorName = "IoError";
                return false;
            }
            Project committed;
            EditLog log;
            const auto storedLog =
                editor.state->timelineEditLogs.find(request.timeline_id);
            const EditLog* liveLog =
                request.timeline_id == editor.state->activeTimelineId
                    ? &editor.state->editLog
                    : (storedLog == editor.state->timelineEditLogs.end()
                           ? nullptr
                           : &storedLog->second);
            if (!CommitStoredRunStep(editor.documentPath.UTF8String, request,
                                     committed, log, result, errorName, message,
                                     &observed, liveLog,
                                     &editor.state->projectEditLog)) {
                return false;
            }
            const bool addedEntry =
                liveLog != nullptr &&
                log.AppliedCount() > liveLog->AppliedCount();
            editor.state->project = std::move(committed);
            editor.state->timelineEditLogs[request.timeline_id] =
                std::move(log);
            editor.state->document = editor.state->project.MakeDocument(
                editor.state->activeTimelineId);
            if (request.timeline_id == editor.state->activeTimelineId) {
                editor.state->editLog =
                    editor.state->timelineEditLogs.at(request.timeline_id);
                if (addedEntry) {
                    editor.state->lastHistoryDomain = HistoryDomain::Timeline;
                }
            }
            [editor refreshTimelineAfterEditFromPosition:editor.state->
                                                         requestedPosition];
            [editor rebuildMediaList];
            [editor reportPendingProjectArchive];
            return true;
        });
    // OR2 -- a stored receipt also has to match the unsaved window state.
    self.chatBackend->SetRunStepMatchCallback(
        [weakSelf](const RunStepReceipt& receipt) {
            AppDelegate* editor = weakSelf;
            if (editor == nil || editor.state == nullptr) {
                return false;
            }
            Project observed;
            std::string message;
            if (!editor.chatBackend->SnapshotProject(observed, message)) {
                return false;
            }
            const auto stored =
                editor.state->timelineEditLogs.find(receipt.timeline_id);
            const EditLog empty;
            const EditLog& log =
                receipt.timeline_id == editor.state->activeTimelineId
                    ? editor.state->editLog
                    : (stored == editor.state->timelineEditLogs.end()
                           ? empty
                           : stored->second);
            return RunStepMatchesState(receipt, observed, log,
                                       editor.state->projectEditLog);
        });
    // AX5 -- docs/roadmap/AX-2026-09.md. What this window shows, for an
    // agent asked about "this clip" or "here" (get_editor_view).
    self.chatBackend->SetEditorViewCallback(
        [weakSelf](EditorView& view, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (strongSelf == nullptr || strongSelf.state == nullptr) {
                message = "the application window is no longer available";
                return false;
            }
            view.timeline_id = strongSelf.state->activeTimelineId;
            view.playhead = strongSelf.state->requestedPosition;
            if (strongSelf.state->interaction)
                view.selected_clip_ids =
                    strongSelf.state->interaction->SelectedClipIds();
            // D5 -- docs/roadmap/DIALOGUE-2026-10.md.
            view.mark_in = strongSelf.state->timelineIn;
            view.mark_out = strongSelf.state->timelineOut;
            return true;
        });
    self.chatBackend->SetVideoOutputCallback(
        [weakSelf](const std::optional<std::string>& screen,
                   VideoOutputState& state, std::string& errorName,
                   std::string& message) {
            AppDelegate* editor = weakSelf;
            if (!editor || !editor.state) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            return static_cast<bool>([editor accessVideoOutput:screen
                                                         state:state
                                                     errorName:errorName
                                                       message:message]);
        });
    // VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. And the window itself,
    // as pixels (capture_editor_window).
    self.chatBackend->SetEditorCaptureCallback(
        [weakSelf](const EditorCaptureRequest& request, EditorCapture& capture,
                   std::string& errorName, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            errorName = "IoError";
            if (strongSelf == nullptr || strongSelf.state == nullptr) {
                message = "the application window is no longer available";
                return false;
            }
            // CH6 -- the Charte panel, a window of its own: drawn whole.
            if (request.region == "charter") {
                NSBitmapImageRep* panel =
                    [strongSelf drawCharterWindowOffscreen:message];
                if (panel == nil) {
                    errorName = "ValidationFailed";
                    return false;
                }
                const CGFloat panelBacking =
                    static_cast<CGFloat>(panel.pixelsWide) /
                    std::max<CGFloat>(
                        1.0, strongSelf.charterPanel.window.frame.size.width);
                const double panelFactor = request.scale / panelBacking;
                const auto panelWidth = static_cast<size_t>(
                    std::max(1.0, std::round(panel.pixelsWide * panelFactor)));
                const auto panelHeight = static_cast<size_t>(
                    std::max(1.0, std::round(panel.pixelsHigh * panelFactor)));
                CGImageRef image = WindowCaptureSrgb(
                    panel.CGImage,
                    CGRectMake(0, 0, panel.pixelsWide, panel.pixelsHigh),
                    panelWidth, panelHeight);
                capture.jpeg = EncodeWindowCapture(image, true);
                CGImageRelease(image);
                if (capture.jpeg.empty()) {
                    message = "the Charte panel was drawn but could not be "
                              "encoded";
                    return false;
                }
                capture.width = static_cast<int64_t>(panelWidth);
                capture.height = static_cast<int64_t>(panelHeight);
                capture.regions.push_back(
                    {"charter", 0, 0, capture.width, capture.height});
                return true;
            }
            NSBitmapImageRep* drawn =
                [strongSelf drawEditorWindowOffscreen:message];
            if (drawn == nil) return false;
            NSWindow* window = strongSelf.window;
            NSView* frameView =
                window.contentView.superview ?: window.contentView;
            const CGFloat backing =
                static_cast<CGFloat>(drawn.pixelsWide) /
                std::max<CGFloat>(1.0, frameView.bounds.size.width);
            const std::pair<const char*, NSView*> surfaces[] = {
                {"timeline", strongSelf.metalView},
                {"record", strongSelf.programMonitorView},
                {"source", strongSelf.sourceMonitorView},
            };
            const auto placed = [&](NSView* view, CGFloat pixelsPerPoint) {
                return view.window == window
                           ? WindowCaptureRect(view, pixelsPerPoint)
                           : CGRectNull;
            };
            CGRect crop = CGRectMake(0, 0, drawn.pixelsWide, drawn.pixelsHigh);
            if (request.region != "window") {
                for (const auto& [name, view] : surfaces)
                    if (request.region == name) crop = placed(view, backing);
                if (CGRectIsNull(crop) || CGRectIsEmpty(crop)) {
                    errorName = "ValidationFailed";
                    message = "the " + request.region +
                              " is not on show in the window";
                    return false;
                }
            }
            const double factor = request.scale / backing;
            const auto width = static_cast<size_t>(
                std::max(1.0, std::round(crop.size.width * factor)));
            const auto height = static_cast<size_t>(
                std::max(1.0, std::round(crop.size.height * factor)));
            CGImageRef image =
                WindowCaptureSrgb(drawn.CGImage, crop, width, height);
            capture.jpeg = EncodeWindowCapture(image, true);
            CGImageRelease(image);
            if (capture.jpeg.empty()) {
                message = "the window was drawn but could not be encoded";
                return false;
            }
            capture.width = static_cast<int64_t>(width);
            capture.height = static_cast<int64_t>(height);
            if (request.region != "window") {
                capture.regions.push_back(
                    {request.region, 0, 0, capture.width, capture.height});
                return true;
            }
            for (const auto& [name, view] : surfaces) {
                const CGRect rect = placed(view, request.scale);
                if (CGRectIsNull(rect) || CGRectIsEmpty(rect)) continue;
                capture.regions.push_back(
                    {name, static_cast<int64_t>(rect.origin.x),
                     static_cast<int64_t>(rect.origin.y),
                     static_cast<int64_t>(rect.size.width),
                     static_cast<int64_t>(rect.size.height)});
            }
            return true;
        });
    [self.chatPanelView configureWithBackend:*self.chatBackend];
    [self startAgentEndpoint];
    [self.chatPanelView setStopHandler:^{
      std::lock_guard<std::mutex> lock(chatTasks->mutex);
      for (const Ulid& id : chatTasks->running) editorTasks->Cancel(id);
    }];
    [self buildAgentWindow];

    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:0];
    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:2];
    [self.workspaceSplitView adjustSubviews];
    [self.workspaceSplitView setPosition:mediaPanelWidth ofDividerAtIndex:0];
    [self.workspaceSplitView setPosition:mediaPanelWidth + editorWidth
                        ofDividerAtIndex:1];

    self.infoLabel = [NSTextField labelWithString:@"Aucun clip sélectionné"];
    self.infoLabel.frame =
        NSMakeRect(20.0, 12.0, windowRect.size.width - 40.0, 18.0);
    self.infoLabel.autoresizingMask = NSViewWidthSizable;
    self.infoLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:11.0
                                         weight:NSFontWeightRegular];
    self.infoLabel.textColor = CMThemeColor(ui::theme::kTextSecondary);
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    // ON1 -- the bottom strip: the status line, the pages, the panels.
    self.workspaceBar = [[CMWorkspaceBarView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, windowRect.size.width, 42.0)];
    self.workspaceBar.autoresizingMask = NSViewWidthSizable;
    self.workspaceBar.delegate = self;
    self.workspaceBar.statusLabel = self.infoLabel;
    [content addSubview:self.workspaceBar];

    [self refreshBinControlsSelecting:@"__root__"];

    self.state->renderer = std::make_unique<Renderer>();
    if (!self.state->renderer->Initialize(self.metalView)) {
        std::fprintf(stderr, "Renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.state->sourceRenderer = std::make_unique<Renderer>();
    self.state->programRenderer = std::make_unique<Renderer>();
    if (!self.state->sourceRenderer->Initialize(self.sourceMonitorView) ||
        !self.state->programRenderer->Initialize(self.programMonitorView)) {
        std::fprintf(stderr, "Monitor renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.offlineMediaLabel = [NSTextField labelWithString:@"MÉDIA OFFLINE"];
    self.offlineMediaLabel.alignment = NSTextAlignmentCenter;
    self.offlineMediaLabel.font =
        [NSFont systemFontOfSize:24.0 weight:NSFontWeightSemibold];
    self.offlineMediaLabel.textColor = NSColor.secondaryLabelColor;
    self.offlineMediaLabel.hidden = YES;
    [self.programMonitorView addSubview:self.offlineMediaLabel];
    self.offlineMediaLabel.frame =
        NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                   self.programMonitorView.bounds.size.height);
    self.offlineMediaLabel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.programCaptionView = [[CMCaptionView alloc] initWithFrame:NSZeroRect];
    self.programCaptionView.hidden = YES;
    [self.programMonitorView addSubview:self.programCaptionView];
    self.sourceOfflineMediaLabel =
        [NSTextField labelWithString:@"MÉDIA OFFLINE"];
    self.sourceOfflineMediaLabel.alignment = NSTextAlignmentCenter;
    self.sourceOfflineMediaLabel.font =
        [NSFont systemFontOfSize:24.0 weight:NSFontWeightSemibold];
    self.sourceOfflineMediaLabel.textColor = NSColor.secondaryLabelColor;
    self.sourceOfflineMediaLabel.hidden = YES;
    self.sourceOfflineMediaLabel.frame = self.sourceMonitorView.bounds;
    self.sourceOfflineMediaLabel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.sourceMonitorView addSubview:self.sourceOfflineMediaLabel];

    self.sourceMonitorTitleLabel =
        [NSTextField labelWithString:@"SOURCE — aucun rush chargé"];
    self.programMonitorTitleLabel = [NSTextField
        labelWithString:CMRecordTitle(self.state->document.sequence.name)];
    for (NSTextField* label in
         @[ self.sourceMonitorTitleLabel, self.programMonitorTitleLabel ]) {
        TimelineMetalView* monitor = label == self.programMonitorTitleLabel
                                         ? self.programMonitorView
                                         : self.sourceMonitorView;
        label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightBold);
        label.drawsBackground = NO;
        label.frame = NSMakeRect(
            8.0,
            monitor.bounds.size.height - ui::theme::kPanelHeaderHeight - 4.0,
            monitor.bounds.size.width - 16.0, ui::theme::kPanelHeaderHeight);
        label.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    }
    self.sourceMonitorTitleLabel.textColor =
        CMThemeColor(ui::theme::kTextPrimary);
    self.programMonitorTitleLabel.textColor = NSColor.whiteColor;
    [self.sourceMonitorView addSubview:self.sourceMonitorTitleLabel];
    [self.programMonitorView addSubview:self.programMonitorTitleLabel];
    self.sourceMonitorZoneLabel =
        [NSTextField labelWithString:@"IN —    OUT —"];
    self.sourceMonitorZoneLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorZoneLabel.textColor =
        CMThemeColor(ui::theme::kTextSecondary);
    self.sourceMonitorZoneLabel.frame = NSMakeRect(10.0, 29.0, 300.0, 16.0);
    self.sourceMonitorZoneLabel.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    [self.sourceMonitorView addSubview:self.sourceMonitorZoneLabel];
    NSButton* sourceInsertButton =
        CMMakeStyledButton(@"INSÉRER", self, @selector(menuInsertSource:));
    NSButton* sourceOverwriteButton =
        CMMakeStyledButton(@"ÉCRASER", self, @selector(menuOverwriteSource:));
    sourceInsertButton.frame = NSMakeRect(10.0, 49.0, 78.0, 24.0);
    sourceOverwriteButton.frame = NSMakeRect(92.0, 49.0, 82.0, 24.0);
    sourceInsertButton.image =
        SystemSymbol(@"arrow.down.to.line", @"Insérer depuis Source", 10.0);
    sourceOverwriteButton.image =
        SystemSymbol(@"square.on.square", @"Écraser depuis Source", 10.0);
    for (NSButton* button in @[ sourceInsertButton, sourceOverwriteButton ]) {
        button.font = [NSFont systemFontOfSize:10.0];
        button.imagePosition = NSImageLeading;
        button.autoresizingMask = NSViewMaxYMargin;
        [self.sourceMonitorView addSubview:button];
    }
    self.sourceMonitorZoomPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(
                          self.sourceMonitorView.bounds.size.width - 92.0, 4.0,
                          84.0, 24.0)
            pullsDown:NO];
    self.programMonitorZoomPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(
                          self.programMonitorView.bounds.size.width - 92.0, 4.0,
                          84.0, 24.0)
            pullsDown:NO];
    for (NSPopUpButton* popup in
         @[ self.sourceMonitorZoomPopup, self.programMonitorZoomPopup ]) {
        [popup
            addItemsWithTitles:@[ @"Fit", @"25%", @"50%", @"100%", @"200%" ]];
        const NSInteger zoomTags[] = {0, 25, 50, 100, 200};
        for (NSInteger index = 0; index < popup.numberOfItems; ++index)
            [popup itemAtIndex:index].tag = zoomTags[index];
        popup.target = self;
        popup.action = @selector(monitorZoomChanged:);
        popup.controlSize = NSControlSizeSmall;
        popup.font =
            [NSFont monospacedDigitSystemFontOfSize:10.0
                                             weight:NSFontWeightMedium];
        popup.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        popup.toolTip = @"Zoom d’affichage du moniteur";
    }
    [self.sourceMonitorView addSubview:self.sourceMonitorZoomPopup];
    [self.programMonitorView addSubview:self.programMonitorZoomPopup];
    self.programVideoScopePopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(8.0, 4.0, 132.0, 24.0)
                                   pullsDown:NO];
    [self.programVideoScopePopup addItemsWithTitles:@[
        @"Aucun scope", @"Waveform", @"Waveform RGB", @"Parade RGB",
        @"Vectorscope", @"Fausses couleurs"
    ]];
    // Tags are the stored VideoScopeMode values; the RGB waveform was
    // appended to the enum but is listed beside the luma one.
    const NSInteger scopeTags[] = {
        static_cast<NSInteger>(VideoScopeMode::Off),
        static_cast<NSInteger>(VideoScopeMode::Waveform),
        static_cast<NSInteger>(VideoScopeMode::WaveformRgb),
        static_cast<NSInteger>(VideoScopeMode::ParadeRgb),
        static_cast<NSInteger>(VideoScopeMode::Vectorscope),
        static_cast<NSInteger>(VideoScopeMode::FalseColor)};
    for (NSInteger index = 0; index < self.programVideoScopePopup.numberOfItems;
         ++index)
        [self.programVideoScopePopup itemAtIndex:index].tag = scopeTags[index];
    [self.programVideoScopePopup
        selectItemWithTag:static_cast<NSInteger>(
                              self.state->programVideoScope)];
    self.programVideoScopePopup.target = self;
    self.programVideoScopePopup.action = @selector(programVideoScopeChanged:);
    self.programVideoScopePopup.controlSize = NSControlSizeSmall;
    self.programVideoScopePopup.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.programVideoScopePopup.autoresizingMask =
        NSViewMaxXMargin | NSViewMaxYMargin;
    self.programVideoScopePopup.toolTip =
        @"Afficher un scope de la sortie Record après grading";
    [self.programMonitorView addSubview:self.programVideoScopePopup];
    // G6 -- said on the monitor itself: a picture shown without its grade,
    // or half of it a reference, must never pass for the film.
    self.programCompareBadge = [NSTextField labelWithString:@""];
    self.programCompareBadge.frame = NSMakeRect(
        148.0, 8.0,
        std::max(0.0, self.programMonitorView.bounds.size.width - 248.0), 16.0);
    self.programCompareBadge.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    self.programCompareBadge.alignment = NSTextAlignmentCenter;
    self.programCompareBadge.font =
        [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeCaption
                                    weight:NSFontWeightBold];
    self.programCompareBadge.textColor = CMThemeColor(ui::theme::kAccent);
    self.programCompareBadge.hidden = YES;
    [self.programMonitorView addSubview:self.programCompareBadge];
    [self refreshCompareBadge];
    self.sourceMonitorToggleButton =
        CMMakeStyledButton(@"SOURCE ON", self, @selector(toggleSourceMonitor:));
    self.sourceMonitorToggleButton.state = NSControlStateValueOn;
    self.sourceMonitorToggleButton.frame = NSMakeRect(
        self.programMonitorView.bounds.size.width - 198.0, 4.0, 98.0, 24.0);
    self.sourceMonitorToggleButton.autoresizingMask =
        NSViewMinXMargin | NSViewMaxYMargin;
    self.sourceMonitorToggleButton.font =
        [NSFont systemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorToggleButton.image = SystemSymbol(
        @"rectangle.split.2x1", @"Afficher le moniteur Source", 11.0);
    self.sourceMonitorToggleButton.imagePosition = NSImageLeading;
    self.sourceMonitorToggleButton.toolTip =
        @"Afficher ou masquer le moniteur Source";
    [self.programMonitorView addSubview:self.sourceMonitorToggleButton];
    // ON7 -- the edit trace stays on Montage while the Agent is hidden.
    self.workspaceActivityScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(0, 0, ui::workspace::kActivityWidth, 400)];
    self.workspaceActivityScroll.hasVerticalScroller = YES;
    self.workspaceActivityScroll.borderType = NSNoBorder;
    self.workspaceActivityText =
        [[NSTextView alloc] initWithFrame:self.workspaceActivityScroll.bounds];
    self.workspaceActivityText.editable = NO;
    self.workspaceActivityText.selectable = YES;
    self.workspaceActivityText.font =
        CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
    self.workspaceActivityText.textColor =
        CMThemeColor(ui::theme::kTextPrimary);
    self.workspaceActivityText.backgroundColor = CMSurfacePanelColor();
    self.workspaceActivityText.textContainerInset =
        NSMakeSize(ui::theme::kSpaceS, ui::theme::kSpaceS);
    self.workspaceActivityText.minSize = NSZeroSize;
    self.workspaceActivityText.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    self.workspaceActivityText.verticallyResizable = YES;
    self.workspaceActivityText.horizontallyResizable = NO;
    self.workspaceActivityText.autoresizingMask = NSViewWidthSizable;
    self.workspaceActivityText.textContainer.widthTracksTextView = YES;
    self.workspaceActivityText.accessibilityLabel = NSLocalizedString(
        @"Marqueurs et modifications récentes du montage", nil);
    self.workspaceActivityScroll.documentView = self.workspaceActivityText;
    self.trackHeaderLabels = [NSMutableArray array];
    self.timelineTimecodeLabel = [NSTextField labelWithString:@"00:00:00:00"];
    self.timelineTimecodeLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:14.0
                                         weight:NSFontWeightSemibold];
    self.timelineTimecodeLabel.textColor = [NSColor colorWithRed:0.82
                                                           green:0.84
                                                            blue:0.87
                                                           alpha:1.0];
    self.timelineTimecodeLabel.alignment = NSTextAlignmentLeft;
    [self.metalView addSubview:self.timelineTimecodeLabel];
    self.timelineToolIcons = [NSMutableArray array];
    [self refreshTimelineChrome];
    [self fitTimelineToViewportWidth];

    __weak AppDelegate* weakShortcutTarget = self;
    self.timelineShortcutMonitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       AppDelegate* target = weakShortcutTarget;
                                       if (!target) return event;
                                       // ON3 -- the Agent's window: its
                                       // field keeps the text keys too.
                                       if ([target agentWindowTakesKey:event])
                                           return nil;
                                       // CH6 -- and the Charte panel's.
                                       if ([target charterWindowTakesKey:event])
                                           return nil;
                                       if (event.window != target.window)
                                           return event;
                                       NSResponder* responder =
                                           target.window.firstResponder;
                                       if (![responder
                                               isKindOfClass:NSView.class])
                                           return event;
                                       NSView* focusedView = (NSView*)responder;
                                       // D7 -- a field being typed in
                                       // keeps the text keys.
                                       if ([focusedView
                                               isKindOfClass:NSTextView
                                                                 .class] &&
                                           [target
                                               textEditKeyDown:event
                                                    inTextView:(NSTextView*)
                                                                   focusedView])
                                           return nil;
                                       if (![focusedView
                                               isDescendantOf:
                                                   target.inspectorView] ||
                                           [focusedView
                                               isKindOfClass:NSTextView.class])
                                           return event;
                                       // Preserve native Inspector control
                                       // navigation and adjustment. The
                                       // remaining keys use the same
                                       // configurable timeline dispatcher as
                                       // the Metal view, so this does not
                                       // create a second shortcut policy.
                                       switch (event.keyCode) {
                                           case 48:   // Tab
                                           case 53:   // Escape
                                           case 115:  // Home
                                           case 119:  // End
                                           case 123:  // Left
                                           case 124:  // Right
                                           case 125:  // Down
                                           case 126:  // Up
                                               return event;
                                           default:
                                               break;
                                       }
                                       return [target timelineKeyDown:event]
                                                  ? nil
                                                  : event;
                                     }];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self reportPendingProjectArchive];
    // The timeline it opens on, as -activateTimelineIdentifier: does for
    // the others (LinkGroupRepair.h).
    [self splitMergedLinkGroups];
    if (AppTestHooks* hooks = TestHooks())
        hooks->EditorWindowShown(self.state->workers.size());
    // The window is up; which media are missing is checked behind it.
    [self checkSourceAvailabilityInBackground];
    BOOL loadedThumbnail = NO;
    for (const DocumentSource& source : self.state->document.sources) {
        const auto detected = self.state->mediaMetadata.find(source.id);
        const LibraryMedia* media =
            detected == self.state->mediaMetadata.end()
                ? self.state->document.FindLibraryMedia(source.id)
                : &detected->second;
        if (media && media->has_audio &&
            self.state->offlineSourceIds.count(source.id) == 0)
            [self loadOrEnqueueWaveformForMediaIdentifier:
                      [NSString stringWithUTF8String:source.id.c_str()]];
        if (media && self.state->offlineSourceIds.count(source.id) == 0)
            loadedThumbnail |=
                [self stageThumbnailForMediaIdentifier:
                          [NSString stringWithUTF8String:source.id.c_str()]];
    }
    if (loadedThumbnail) [self.mediaCollection reloadData];
    // ON1 -- the page the editor left the window on, Montage the first time.
    // The smoke run lays the window out as it was before pages, every
    // panel open, and writes nothing into the editor's preferences.
    self.rememberWorkspace = YES;
    bool smokeLayout = false;
    if (AppTestHooks* hooks = TestHooks())
        smokeLayout = hooks->LaysOutEveryPanel();
    if (smokeLayout) {
        self.rememberWorkspace = NO;
        // 301, not 300: before pages the dock's divider was set at the
        // media panel's width plus the centre's, which left it one point
        // wider and the centre an even 977 points -- two viewers of equal
        // width, which "Source toggle restores two equally sized
        // monitors" holds to the point.
        self.workspaceFixedWidths = {320.0, 301.0};
        self.workspaceWidthsFixed = YES;
        [self applyWorkspacePage:ui::workspace::Page::Edit
                          panels:ui::workspace::PanelSet{true, true, true}
                        remember:NO];
    } else {
        CMUiPreferences* preferences = CMUiPreferences.sharedPreferences;
        const ui::workspace::Page page = [preferences workspacePage];
        [self applyWorkspacePage:page
                          panels:[preferences panelsForPage:page]
                        remember:NO];
        // ON3 -- the Agent's window, if it was left shown, without the
        // keyboard: the editor keeps it.
        if ([preferences agentWindowShown]) [self showAgentWindow:YES focus:NO];
    }
    [self refreshAgentToggle];
    [self requestResolvedPosition:{0, 1}];
    [self startVideoOutput];
    self.displayTimer = [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                                target:self
                                              selector:@selector(displayTick:)
                                              userInfo:nil
                                               repeats:YES];
    // Splitter dragging runs the AppKit event-tracking loop. Common modes keep
    // Metal presenting correctly letterboxed frames throughout the gesture.
    [NSRunLoop.mainRunLoop addTimer:self.displayTimer
                            forMode:NSRunLoopCommonModes];
    if (AppTestHooks* hooks = TestHooks()) hooks->EditorReady(self);
}

- (void)application:(NSApplication*)application
          openFiles:(NSArray<NSString*>*)filenames {
    (void)application;
    if (filenames.count == 0) return;
    NSString* selected = [self resolvedProjectPath:filenames.firstObject];
    if (!self.window && self.documentPath.length == 0) {
        self.documentPath = selected;
        [NSApp replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
        return;
    }
    [NSApp replyToOpenOrPrint:NSApplicationDelegateReplyCancel];
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Un projet est déjà ouvert";
    alert.informativeText =
        @"Fermez la fenêtre actuelle avant d’ouvrir un autre projet.";
    if (self.window)
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)windowDidMove:(NSNotification*)notification {
    if (notification.object == self.agentWindow)
        [self agentWindowDidChangeFrame];
}

// CH6 -- docs/roadmap/CHARTE-2026-11.md (DEC63). The Charte panel, built
// the first time it is asked for (CharterPanel.h): shown and hidden by
// Affichage > Charte... and its key, from any page.
- (void)toggleCharterPanel:(id)sender {
    (void)sender;
    if (self.window == nil) return;
    if (self.charterPanel == nil) {
        self.charterPanel =
            [[CMCharterPanel alloc] initWithEditorWindow:self.window];
        self.charterPanel.rememberFrame = self.rememberWorkspace;
    }
    [self.charterPanel toggle];
}

// The Charte panel as it stands, drawn without the screen
// (WindowCapture.h): capture_editor_window's "charter" region.
- (NSBitmapImageRep*)drawCharterWindowOffscreen:(std::string&)error {
    error.clear();
    NSPanel* panel = self.charterPanel.window;
    if (panel == nil || !self.charterPanel.shown) {
        error = "the Charte panel is not open (Affichage > Charte...)";
        return nil;
    }
    [panel displayIfNeeded];
    return DrawWindowOffscreen(panel, {}, error);
}

// Its close button hides it, as the toggle does; the editor closed, the
// application quits, the Agent's window with it, still remembered shown.
- (void)windowWillClose:(NSNotification*)notification {
    if (notification.object == self.agentWindow) {
        if (!self.agentShown) return;
        self.agentShown = NO;
        if (self.rememberWorkspace)
            [CMUiPreferences.sharedPreferences setAgentWindowShown:NO];
        [self refreshAgentToggle];
        return;
    }
    if (notification.object == self.window) {
        [self.agentWindow orderOut:nil];
        [self.charterPanel hidePanel];
    }
}

- (void)windowDidResize:(NSNotification*)notification {
    if (notification.object == self.agentWindow) {
        [self agentWindowDidChangeFrame];
        return;
    }
    if (self.state->renderer) {
        [self refreshTimelineChrome];
        self.state->overlayDirty = true;
    }
}

// VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The editor window as it
// stands, drawn without the screen (WindowCapture.h): each Metal surface on
// show presents once more and its renderer keeps a copy. Record keeps its
// previous picture, and has none to give, while a layer waits on its decoder
// (PlaybackPresentation.h): a few more ticks are tried, then the capture is
// refused rather than given with a hole where Record is.
- (NSBitmapImageRep*)drawEditorWindowOffscreen:(std::string&)error {
    error.clear();
    if (self.window == nil || !self.state || !self.state->renderer ||
        !self.state->sourceRenderer || !self.state->programRenderer) {
        error = "the editor window is not open";
        return nil;
    }
    struct Surface {
        TimelineMetalView* view;
        Renderer* renderer;
        const char* name;
    };
    const Surface all[] = {
        {self.metalView, self.state->renderer.get(), "the timeline"},
        {self.sourceMonitorView, self.state->sourceRenderer.get(), "Source"},
        {self.programMonitorView, self.state->programRenderer.get(), "Record"},
    };
    std::vector<const Surface*> waiting;
    for (const Surface& surface : all) {
        if (surface.view.window != self.window ||
            surface.view.isHiddenOrHasHiddenAncestor ||
            NSIsEmptyRect(surface.view.bounds))
            continue;
        surface.renderer->CaptureNextPresent();
        waiting.push_back(&surface);
    }
    std::vector<WindowCaptureSurface> surfaces;
    for (int attempt = 0; attempt < 10 && !waiting.empty(); ++attempt) {
        if (attempt > 0)
            [NSRunLoop.currentRunLoop
                   runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        for (auto it = waiting.begin(); it != waiting.end();) {
            CGImageRef image = (*it)->renderer->TakePresentCapture();
            if (image != nullptr) {
                surfaces.push_back({(*it)->view, image});
                it = waiting.erase(it);
            } else {
                (*it)->renderer->CaptureNextPresent();
                ++it;
            }
        }
    }
    NSBitmapImageRep* drawn = nil;
    if (waiting.empty()) {
        drawn = DrawWindowOffscreen(self.window, surfaces, error);
    } else {
        error = std::string(waiting.front()->name) +
                " did not present again: a layer still waits on its decoder";
        // Disarmed: the playback that follows draws each picture once.
        for (const Surface* surface : waiting)
            CGImageRelease(surface->renderer->TakePresentCapture());
    }
    for (const WindowCaptureSurface& surface : surfaces)
        CGImageRelease(surface.image);
    return drawn;
}

- (void)timelineMetalViewDidResize:(TimelineMetalView*)view {
    if (view == self.metalView) [self layOutTrimBar];
    if (!self.state) return;
    if (view == self.metalView && self.state->renderer) {
        self.state->renderer->Resize(view.bounds);
        // PRD37 -- ROADMAP.md. A fit made while the window was still being
        // laid out follows the width the view settles at (QA of 3 October
        // 2026: fitted at 971 points, shown at 917.5, the end of the film
        // out of view); once a zoom or a scroll has moved it, the scale is
        // the editor's and a resize keeps it.
        if (TimelineFitFollowsResize(self.state->timelineFit,
                                     self.state->viewport,
                                     view.bounds.size.width))
            [self fitTimelineToViewportWidth];
        [self refreshTimelineChrome];
    } else if (view == self.sourceMonitorView && self.state->sourceRenderer) {
        self.state->sourceRenderer->Resize(view.bounds);
    } else if (view == self.programMonitorView && self.state->programRenderer) {
        self.state->programRenderer->Resize(view.bounds);
    } else if (view == self.videoOutputView &&
               self.state->videoOutputRenderer) {
        self.state->videoOutputRenderer->Resize(view.bounds);
        self.state->videoOutputNeedsPresent = true;
    }
    self.state->overlayDirty = true;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    // A gesture's save still on the commit queue reaches the disk before
    // anything that writes the project stops and the lock goes.
    [self drainProjectCommits];
    [self stopVideoOutput];
    if (self.state) self.state->exportCancel.store(true);
    if (self.state && self.state->mediaTasks)
        self.state->mediaTasks->CancelAll();
    if (self.state->audioPlayback) self.state->audioPlayback->Stop();
    if (self.state->sourceAudioPlayback)
        self.state->sourceAudioPlayback->Stop();
    [self.displayTimer invalidate];
    self.displayTimer = nil;
    if (self.timelineShortcutMonitor) {
        [NSEvent removeMonitor:self.timelineShortcutMonitor];
        self.timelineShortcutMonitor = nil;
    }
    for (auto& worker : self.state->workers) {
        worker.second->Stop();
    }
    // AX1 -- no agent reaches the project once the lock is gone: the
    // announcement and the socket go first. Abandoned, not stopped: a
    // connection may be waiting on this very thread.
    if (self.agentEndpoint) self.agentEndpoint->Abandon();
    // Visual QA of 19 September 2026 -- -terminate: exits without running
    // C++ destructors, so ProjectSessionLock's never released the lock on a
    // plain Cmd+Q: its directory stayed next to the project, reclaimed only
    // because its pid was gone. Everything that writes the project has
    // stopped above; the lock goes last.
    if (self.state && self.state->projectLock)
        self.state->projectLock->Release();
    // Neither in the window test, which installs its hooks.
    if (TestHooks() != nullptr) return;
    // DEC39 -- the editor reopens on the project chosen in the Fichier
    // menu, once this one's lock is gone (ProjectSwitch.h).
    if (self.pendingProjectSwitchPath.length > 0) {
        const std::string projectPath =
            self.pendingProjectSwitchPath.fileSystemRepresentation ?: "";
        const process::Result started =
            process::StartDetached(project_switch::EditorArgumentsFor(
                NSBundle.mainBundle.executablePath.fileSystemRepresentation
                    ?: "",
                projectPath));
        if (!started.Started())
            std::fprintf(stderr,
                         "Project switch: the editor did not start again on "
                         "%s: %s\n",
                         projectPath.c_str(),
                         started.SystemErrorText().c_str());
    }
    engine_log::StopProcessLog();  // PRD35
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

@end
