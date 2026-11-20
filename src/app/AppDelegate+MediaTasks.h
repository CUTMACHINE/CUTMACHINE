#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The media
// library's work in the background (MediaTaskManager.h): imports and their
// probes, relinks, proxies, thumbnails, waveforms and transcriptions,
// asked from the window, then committed to the document when they finish.
// Moved out of main.mm unchanged.

@interface AppDelegate (MediaTasks)
- (void)relinkSelectedMedia:(id)sender;
- (void)batchRelinkOfflineMedia:(id)sender;
- (void)importMediaPressed:(id)sender;
- (void)importSubtitlesPressed:(id)sender;
- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId;
- (void)generateSelectedMediaProxy:(id)sender;
- (void)transcribeSelectedMedia:(id)sender;
- (void)transcribeSelectedMediaHesitations:(id)sender;
- (void)transcribeSelectedMediaFrenchVerbatim:(BOOL)frenchVerbatim;
- (void)transcribeTimeline:(id)sender;
- (void)transcribeTimelineHesitations:(id)sender;
- (void)transcribeTimelineFrenchVerbatim:(BOOL)frenchVerbatim;
- (Ulid)enqueueTranscriptionForMediaIdentifier:(NSString*)identifier
                                      settings:(const WhisperSettings&)settings
                                       batchId:(const Ulid&)batchId
                                    reuseCache:(BOOL)reuseCache
                                   hesitations:(BOOL)hesitations;
- (void)regenerateSelectedMediaThumbnail:(id)sender;
- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier;
- (void)removeSelectedMediaProxy:(id)sender;
- (void)refreshMediaTaskStatus;
- (void)cancelDisplayedMediaTask:(id)sender;
- (void)processCompletedMediaImports;
- (void)processCompletedMediaProxies;
- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)processCompletedMediaWaveforms;
- (void)processCompletedMediaTranscriptions;
- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier;
- (BOOL)stageThumbnailForMediaIdentifier:(NSString*)identifier;
- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier;
- (void)processCompletedMediaThumbnails;
- (void)processCompletedMediaRelinks;
- (void)processCompletedBatchRelinks;
- (void)commitMediaImportBatch:(ProbedImportBatch*)batch;
- (BOOL)commitProbedMedia:(const std::vector<ProbedIngestFile>&)files
             documentPath:(const std::filesystem::path&)documentPath
                targetBin:(const std::optional<Ulid>&)targetBin
                  outcome:(IngestOutcome&)outcome
                errorName:(std::string&)errorName
                  message:(std::string&)message;
@end
