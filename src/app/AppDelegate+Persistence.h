#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. How an edit
// made in the window reaches the project on disk: applied, staged,
// committed on a background queue, reported, and the window refreshed
// after it (ProjectStorage.h). Moved out of main.mm unchanged.

@interface AppDelegate (Persistence)
- (BOOL)persistEdits:(std::string&)message;
- (BOOL)persistStagedDocument:(const Document&)document
                      editLog:(const EditLog&)editLog
                      message:(std::string&)message;
- (BOOL)persistStagedDocumentNow:(const Document&)document
                         editLog:(const EditLog&)editLog
                         message:(std::string&)message;
- (BOOL)applyAndPersistTimelineOperation:(Operation)operation
                                   error:(EditError&)error
                                 message:(std::string&)message;
- (void)reportPendingProjectArchive;
- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                       message:(std::string&)message;
- (std::map<std::string, EditLog>)timelineLogsForProject:(const Project&)project
                                                 editLog:
                                                     (const EditLog&)editLog;
- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                    projectLog:(const ProjectEditLog&)projectLog
                       message:(std::string&)message;
- (dispatch_queue_t)commitQueue;
- (void)drainProjectCommits;
- (BOOL)persistEditsInBackground:(std::string&)message;
- (void)projectCommitFinished:(BOOL)committed message:(NSString*)message;
- (void)reportPendingProjectArchiveThrottled;
- (void)refreshAfterProjectRename;
- (void)refreshAfterProjectMutation;
@end
