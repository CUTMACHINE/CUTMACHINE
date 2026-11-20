// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. How an edit
// made in the window reaches the project on disk: applied, staged,
// committed on a background queue, reported, and the window refreshed
// after it (ProjectStorage.h). Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

// What every window commit does, on whichever thread runs it: the autosave
// envelope first (the recovery net if the package write fails), the package
// transaction, then the envelope discarded. StoredProjectIoMutex serialises
// it against any other storage access.
static bool RunProjectCommit(const ProjectCommitJob& job,
                             std::string& message) {
    if (!ProjectRecovery::WriteAutosave(
            job.path, job.project, job.timeline_logs, job.project_log, message))
        return false;
    if (!CommitStoredProjectAndLogs(job.path, job.project, job.timeline_logs,
                                    job.project_log, message))
        return false;
    std::string discardError;
    if (!ProjectRecovery::DiscardAutosave(job.path, discardError))
        std::fprintf(stderr, "Unable to discard committed autosave: %s\n",
                     discardError.c_str());
    return true;
}

@implementation AppDelegate (Persistence)

- (BOOL)persistEdits:(std::string&)message {
    const BOOL persisted = [self persistStagedDocumentNow:self.state->document
                                                  editLog:self.state->editLog
                                                  message:message];
    if (persisted) self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return persisted;
}

// Out of lot -- the author, 29 October 2026: "l'app est toujours lente dans les
// opérations". A window edit is saved as a gesture is: checked in memory
// here, written on the commit queue (-persistEditsInBackground:). Written
// here, the autosave envelope (encoded, then read back) and the package
// transaction held the main thread ~280 ms per edit on a 32-timeline
// project, once the validation itself had been cut from ~800 ms. An agent's
// edit is still written before it is answered (-persistStagedDocumentNow:).
- (BOOL)persistStagedDocument:(const Document&)document
                      editLog:(const EditLog&)editLog
                      message:(std::string&)message {
    Project candidate = self.state->project;
    if (!candidate.CommitDocument(self.state->activeTimelineId, document,
                                  message))
        return NO;
    auto job = std::make_shared<ProjectCommitJob>();
    job->path = self.documentPath.UTF8String ?: "";
    job->timeline_logs = [self timelineLogsForProject:candidate
                                              editLog:editLog];
    job->project_log = self.state->projectEditLog;
    job->project = candidate;
    self.state->project = std::move(candidate);
    self.state->timelineEditLogs[self.state->activeTimelineId] = editLog;
    {
        std::lock_guard<std::mutex> guard(self.state->pendingCommitMutex);
        self.state->pendingCommit = std::move(job);
    }
    AppState* state = self.state;
    __weak AppDelegate* weakSelf = self;
    dispatch_async([self commitQueue], ^{
      std::shared_ptr<ProjectCommitJob> latest;
      {
          std::lock_guard<std::mutex> guard(state->pendingCommitMutex);
          latest = std::move(state->pendingCommit);
      }
      // A later block of the same burst already wrote the newest state.
      if (!latest) return;
      std::string error;
      const bool committed = RunProjectCommit(*latest, error);
      dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf projectCommitFinished:committed
                                message:CMStringFromUtf8(error)];
      });
    });
    return YES;
}

// SAVING_ROADMAP.md, S3. Written before it returns: what an agent is
// answered, an explicit save, a project switch.
- (BOOL)persistStagedDocumentNow:(const Document&)document
                         editLog:(const EditLog&)editLog
                         message:(std::string&)message {
    Project candidate = self.state->project;
    if (!candidate.CommitDocument(self.state->activeTimelineId, document,
                                  message))
        return NO;
    if (![self commitProjectCandidate:candidate
                              editLog:editLog
                              message:message])
        return NO;
    self.state->project = std::move(candidate);
    self.state->timelineEditLogs[self.state->activeTimelineId] = editLog;
    return YES;
}

- (BOOL)applyAndPersistTimelineOperation:(Operation)operation
                                   error:(EditError&)error
                                 message:(std::string&)message {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (!stagedLog.Apply(stagedDocument, std::move(operation), error, message))
        return NO;
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message])
        return NO;
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return YES;
}

- (void)reportPendingProjectArchive {
    if (project_archive::DefaultDatabasePath().empty() || self.window == nil) {
        return;
    }
    const char* projectPath = self.documentPath.UTF8String;
    const auto report =
        StoredProjectArchiveStatus(projectPath != nullptr ? projectPath : "");
    if (report.pending == 0 && report.error_name.empty()) {
        self.projectArchiveWarningShown = NO;
        return;
    }
    if (self.projectArchiveWarningShown || self.window.attachedSheet != nil) {
        return;
    }
    self.projectArchiveWarningShown = YES;
    NSAlert* warning = [NSAlert new];
    warning.alertStyle = NSAlertStyleWarning;
    warning.messageText =
        NSLocalizedString(@"Projet enregistré, archive SQL en attente", nil);
    warning.informativeText = NSLocalizedString(
        @"Les captures restent conservées dans le projet. CUTMACHINE "
        @"réessaiera de les archiver à la prochaine sauvegarde ou ouverture. "
        @"Le montage et son historique sont enregistrés.",
        nil);
    [warning addButtonWithTitle:NSLocalizedString(@"Compris", nil)];
    [warning beginSheetModalForWindow:self.window completionHandler:nil];
}

- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                       message:(std::string&)message {
    return [self commitProjectCandidate:project
                                editLog:editLog
                             projectLog:self.state->projectEditLog
                                message:message];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. One log per timeline of the
// candidate, as the recovery envelope requires since 20 August: a timeline
// the candidate adds starts with an empty log (as a new package does,
// ProjectStorage.cc), and one it removes leaves its log behind. Copying the
// logs the window held refused every timeline creation.
- (std::map<std::string, EditLog>)timelineLogsForProject:(const Project&)project
                                                 editLog:
                                                     (const EditLog&)editLog {
    std::map<std::string, EditLog> timelineLogs;
    for (const DocumentSequence& timeline : project.timelines) {
        if (timeline.id == self.state->activeTimelineId) {
            timelineLogs.emplace(timeline.id, editLog);
            continue;
        }
        const auto held = self.state->timelineEditLogs.find(timeline.id);
        timelineLogs.emplace(timeline.id,
                             held != self.state->timelineEditLogs.end()
                                 ? held->second
                                 : EditLog{});
    }
    return timelineLogs;
}

- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                    projectLog:(const ProjectEditLog&)projectLog
                       message:(std::string&)message {
    // A gesture's save may still be on the queue: it lands first, so this
    // newer state is the one left on disk.
    [self drainProjectCommits];
    ProjectCommitJob job{self.documentPath.UTF8String ?: "", project,
                         [self timelineLogsForProject:project editLog:editLog],
                         projectLog};
    if (!RunProjectCommit(job, message)) return NO;
    self.backgroundCommitError = nil;
    [self reportPendingProjectArchiveThrottled];
    return YES;
}

// Out of lot -- the author, 29 October 2026. The window's commits, in order,
// off the main thread for a gesture's (persistEditsInBackground:).
- (dispatch_queue_t)commitQueue {
    if (!self.projectCommitQueue)
        self.projectCommitQueue = dispatch_queue_create(
            "com.cutmachine.project-commit", DISPATCH_QUEUE_SERIAL);
    return self.projectCommitQueue;
}

// Waits until every save already enqueued is on disk (or has failed).
- (void)drainProjectCommits {
    if (self.projectCommitQueue)
        dispatch_sync(self.projectCommitQueue, ^{
                      });
}

// Out of lot -- the author, 29 October 2026: "there is a delay when moving
// clips, it needs an optimistic update". A gesture's edit is applied, kept and
// drawn at once; its save (autosave, package, archive: ~240 ms on this
// window's main thread for a 23-timeline project) runs on the commit queue.
// Only the candidate's own check, in memory, can still refuse the edit here.
// A save that fails later cannot take the gesture back: the window says so,
// keeps the work in memory, and the next save -- which holds every edit --
// tries again.
- (BOOL)persistEditsInBackground:(std::string&)message {
    if (![self persistStagedDocument:self.state->document
                             editLog:self.state->editLog
                             message:message])
        return NO;
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return YES;
}

- (void)projectCommitFinished:(BOOL)committed message:(NSString*)message {
    if (!committed) {
        std::fprintf(stderr, "Unable to persist edit: %s\n",
                     message.UTF8String ?: "");
        self.backgroundCommitError = message;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Modification non enregistrée, nouvel essai "
                             @"à la prochaine modification : %@",
                             message];
        return;
    }
    if (self.backgroundCommitError) {
        self.backgroundCommitError = nil;
        self.infoLabel.stringValue = @"Projet enregistré";
    }
    [self reportPendingProjectArchiveThrottled];
}

- (void)reportPendingProjectArchiveThrottled {
    const NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    if (self.lastProjectArchiveCheck > 0 &&
        now - self.lastProjectArchiveCheck < 60.0)
        return;
    self.lastProjectArchiveCheck = now;
    [self reportPendingProjectArchive];
}

- (void)refreshAfterProjectRename {
    // A rename changes browser/title metadata only. Rehydrate the active
    // document so a later timeline save cannot restore its previous name,
    // while keeping decode workers and playback caches alive.
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    self.programMonitorTitleLabel.stringValue =
        CMRecordTitle(self.state->document.sequence.name);
    self.window.title = CMWindowTitle(self.state->project.name,
                                      self.state->document.sequence.name);
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)refreshAfterProjectMutation {
    if (!self.state->project.FindTimeline(self.state->activeTimelineId))
        self.state->activeTimelineId = self.state->project.active_timeline_id;
    if (!self.state->project.FindTimeline(self.state->activeTimelineId) &&
        !self.state->project.timelines.empty())
        self.state->activeTimelineId = self.state->project.timelines.front().id;
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    const auto storedLog =
        self.state->timelineEditLogs.find(self.state->activeTimelineId);
    self.state->editLog = storedLog == self.state->timelineEditLogs.end()
                              ? EditLog{}
                              : storedLog->second;
    self.state->targetedTrackIds.clear();
    const auto storedTargets =
        self.state->timelineTargetTracks.find(self.state->activeTimelineId);
    if (storedTargets != self.state->timelineTargetTracks.end()) {
        for (const Ulid& id : storedTargets->second)
            if (self.state->document.FindTrack(id))
                self.state->targetedTrackIds.insert(id);
    }
    if (self.state->targetedTrackIds.empty())
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            self.state->targetedTrackIds.insert(track.id);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The video tracks follow the
    // new document before the decoders reload: -reloadDecodeWorkers resolves
    // the decoded positions on them, and when the active timeline has just
    // gone (a creation undone) the old list named tracks the new timeline
    // does not have, which threw out of Timeline::ResolveTrack.
    [self rebuildVideoTrackIds];
    [self reloadDecodeWorkers];
    [self refreshTimelineAfterEditFromPosition:RationalTime{
                                                   0,
                                                   self.state->document.sequence
                                                       .frame_rate.num}];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

@end
