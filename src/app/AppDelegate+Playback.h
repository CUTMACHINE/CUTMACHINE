#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Playback and
// the monitors: the transport, what each tick asks of the decoders and
// reads ahead (PERF-2026-09, PERF13 to PERF17), the decoders opened,
// installed and closed, the frame presented at the display's deadline, the
// Source monitor's own playback, and the cartons drawn over Record.
// Moved out of main.mm unchanged.

@interface AppDelegate (Playback)
- (monitor_occlusion::PictureLookup)occlusionPictures;
- (void)requestResolvedPosition:(RationalTime)position;
- (void)warmUpcomingDecoders:(RationalTime)position;
- (void)requestSourcePosition:(RationalTime)position;
- (void)updateSourceZoneLabel;
- (void)scrubSourceMonitorEvent:(NSEvent*)event;
- (void)monitorZoomChanged:(NSPopUpButton*)sender;
- (NSString*)transportStatus;
- (NSString*)playheadResolutionStatus;
- (void)setPlaybackDirection:(int)direction;
- (void)advancePlayback;
- (void)prepareSourceAudioForSource:(const DocumentSource&)source;
- (void)setSourcePlaybackDirection:(int)direction;
- (void)advanceSourcePlayback;
- (void)displayTick:(NSTimer*)timer;
- (std::filesystem::path)decodeBasePath;
- (void)checkSourceAvailabilityInBackground;
- (void)requestFrame:(int64_t)frame ofSource:(const Ulid&)sourceId;
- (void)requestFrame:(int64_t)frame
            ofSource:(const Ulid&)sourceId
                plan:(const DecodeWorker::ReadPlan&)plan;
- (void)syncPlaybackReader;
- (DecodeWorker::ReadPlan)readPlanForClip:(const Ulid&)clipId
                                  onTrack:(const Ulid&)trackId
                                       at:(RationalTime)position
                                 onScreen:(BOOL)onScreen;
- (BOOL)openDecoderForSource:(const Ulid&)sourceId;
- (void)installDecoder:(std::shared_ptr<std::unique_ptr<DecodeWorker>>)opened
               opening:(const PendingDecodeOpen&)pending;
- (void)openDecodersAhead:(RationalTime)position;
- (void)closeDecoderForSource:(const Ulid&)sourceId;
- (void)refreshDecodedPositions;
- (void)reloadDecodeWorkers;
- (MonitorMatte)personMatteForMedia:(const Ulid&)mediaId
                              frame:(int64_t)frame
                               slot:(size_t)slot
                            package:(const std::string&)package;
- (void)showProgramCaptions:(const std::vector<CaptionCarton>&)cartons
                       text:(NSString*)text;
- (void)showCaptions:(const std::vector<CaptionCarton>&)cartons
                text:(NSString*)text
                view:(CMCaptionView*)view
             monitor:(NSView*)monitor
                zoom:(double)zoom;
- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline;
@end
