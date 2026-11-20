// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Playback and
// the monitors: the transport, what each tick asks of the decoders and
// reads ahead (PERF-2026-09, PERF13 to PERF17), the decoders opened,
// installed and closed, the frame presented at the display's deadline, the
// Source monitor's own playback, and the cartons drawn over Record.
// Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (Playback)

// PERF13 -- MonitorOcclusion.h's PictureLookup, answered from the frame
// cache as -presentNearestFrameAtDeadline: will read it: the picture
// GetNearest hands back, turned by the rotation the renderer is given.
// Each source is described once per lookup.
- (monitor_occlusion::PictureLookup)occlusionPictures {
    AppState* state = self.state;
    auto described = std::make_shared<
        std::map<Ulid, std::optional<monitor_occlusion::Picture>>>();
    return [state, described](
               const Ulid& sourceId,
               int64_t frame) -> std::optional<monitor_occlusion::Picture> {
        const auto known = described->find(sourceId);
        if (known != described->end()) return known->second;
        std::optional<monitor_occlusion::Picture> picture;
        int width = 0;
        int height = 0;
        bool alpha = false;
        if (state->frameCache && state->offlineSourceIds.count(sourceId) == 0 &&
            state->frameCache->DescribeNearest(sourceId, frame, width, height,
                                               alpha)) {
            const auto media = state->mediaMetadata.find(sourceId);
            const int32_t degrees = media != state->mediaMetadata.end() &&
                                            media->second.metadata_complete
                                        ? media->second.rotation_degrees
                                        : 0;
            picture = monitor_occlusion::UprightPicture(width, height, degrees,
                                                        alpha);
        }
        described->emplace(sourceId, picture);
        return picture;
    };
}

- (void)requestResolvedPosition:(RationalTime)position {
    self.state->requestedPosition = position;
    if (self.state->playbackReader.Running())
        self.state->playbackReader.SetPosition(position);
    self.timelineTimecodeLabel.stringValue =
        TimelineTimecode(position, [self playheadFrameRate]);
    self.state->requested.clear();
    self.state->hiddenSlots.clear();
    self.state->readSourceIds.clear();
    // FC4 -- in trim mode, at rest, Record is the Trim Monitor.
    if ([self resolveTrimMonitorSlots]) {
        self.offlineMediaLabel.hidden = YES;
        return;
    }
    // PERF13 -- MonitorOcclusion.h. In playback, a track an opaque shot
    // above hides here is neither composited nor decoded: under a cutaway,
    // the interview cost the external drive a second 4K stream nobody saw.
    // At rest everything is decoded as before -- a few frames, and a step
    // across the cutaway's end finds the interview's frame ready.
    self.state->hiddenTrackIds.clear();
    bool hide = true;
    if (AppTestHooks* hooks = TestHooks())
        hide = hooks->Playback().hide_occluded;
    if (self.state->playbackDirection != 0 && hide)
        self.state->hiddenTrackIds = monitor_occlusion::HiddenTracks(
            self.state->document, *self.state->timeline, position,
            [self occlusionPictures], self.state->programMonitorZoom > 0.0);
    for (const Ulid& trackId : self.state->videoTrackIds) {
        const bool trackHidden = self.state->hiddenTrackIds.count(trackId) != 0;
        for (const ResolvedLayer& layer :
             self.state->timeline->ResolveTrackLayers(trackId, position)) {
            const ResolvedSlot slot{true, layer.frame.source_id,
                                    layer.frame.source_frame, layer.opacity,
                                    layer.frame.clip_id};
            // A motion layer costs no decoder, and the motion canvas picks
            // what it edits among the layers composited: it stays.
            const DocumentClip* clip =
                trackHidden ? self.state->document.FindClip(layer.frame.clip_id)
                            : nullptr;
            if (clip != nullptr && !IsMotionClip(*clip)) {
                self.state->hiddenSlots.push_back(slot);
                continue;
            }
            self.state->requested.push_back(slot);
            self.state->readSourceIds.insert(layer.frame.source_id);
            [self requestFrame:layer.frame.source_frame
                      ofSource:layer.frame.source_id
                          plan:[self readPlanForClip:layer.frame.clip_id
                                             onTrack:trackId
                                                  at:position
                                            onScreen:YES]];
        }
    }
    const auto topmost = std::find_if(
        self.state->requested.rbegin(), self.state->requested.rend(),
        [](const ResolvedSlot& slot) { return slot.active; });
    self.offlineMediaLabel.hidden =
        topmost == self.state->requested.rend() ||
        self.state->offlineSourceIds.count(topmost->sourceId) == 0;
    // REM5 -- a selection follows the frame shown.
    if (!self.state->motionCanvas.selected.empty()) {
        [self motionCanvasRefresh];
    }
    // R2c -- and the Inspector's diamonds the playhead's frame.
    [self.inspectorView showPlayheadAt:position];
    [self warmUpcomingDecoders:position];
}

// PERF-2026-09. Playback crosses cuts into sources whose decoder has never
// been asked for anything, and the first request arrives one tick before the
// frame is due -- far too late for a seek and a decode. The layer then has no
// frame at all, and until PlaybackPresentation.h's hold the compositor
// uncovered the track underneath for those ticks. Ask the incoming decoder
// while the outgoing clip is still on screen instead, and only for sources no
// visible layer is using: a worker holds one target frame, so warming a
// source that is being shown right now would steal it from the current image.
- (void)warmUpcomingDecoders:(RationalTime)position {
    self.state->upcomingSourceIds.clear();
    if (self.state->playbackDirection != 0 && self.state->timeline) {
        const int speed = std::abs(self.state->playbackDirection);
        // One second of timeline at nominal speed, and proportionally
        // further ahead when playing faster -- the decoder gets the same
        // wall-clock warning either way.
        int64_t cutSeconds = read_ahead::kCutLookaheadSeconds;
        if (AppTestHooks* hooks = TestHooks()) {
            const PlaybackTuning tuning = hooks->Playback();
            cutSeconds = tuning.read_ahead ? tuning.cut_seconds : 1;
        }
        const RationalTime lookahead{speed * cutSeconds, 1};
        const auto onScreen = [&](const Ulid& sourceId) {
            return std::any_of(
                self.state->requested.begin(), self.state->requested.end(),
                [&](const ResolvedSlot& slot) {
                    return slot.active && slot.sourceId == sourceId;
                });
        };
        const monitor_occlusion::PictureLookup pictures =
            [self occlusionPictures];
        const bool zoomed = self.state->programMonitorZoom > 0.0;
        for (const ResolvedFrame& upcoming :
             self.state->timeline->ResolveUpcoming(
                 position, self.state->playbackDirection, lookahead)) {
            if (onScreen(upcoming.source_id)) continue;
            // PERF13 -- a cut on a track hidden now that is still hidden
            // when playback reaches it shows nothing: the frame the track
            // is shown again at is asked for below instead.
            if (self.state->playbackDirection > 0 &&
                !self.state->hiddenTrackIds.empty()) {
                const DocumentTrack* track =
                    self.state->document.FindTrackForClip(upcoming.clip_id);
                const DocumentClip* clip =
                    self.state->document.FindClip(upcoming.clip_id);
                if (track != nullptr && clip != nullptr &&
                    self.state->hiddenTrackIds.count(track->id) != 0 &&
                    monitor_occlusion::HiddenTracks(
                        self.state->document, *self.state->timeline,
                        clip->timeline_in, pictures, zoomed)
                            .count(track->id) != 0)
                    continue;
            }
            // PERF10 -- a source not open yet is opened now, a second
            // ahead. PERF12 -- the last resort: openDecodersAhead has
            // usually opened it long before.
            self.state->upcomingSourceIds.insert(upcoming.source_id);
            self.state->readSourceIds.insert(upcoming.source_id);
            DecodeWorker::ReadPlan plan;
            if (const DocumentClip* entered =
                    self.state->document.FindClip(upcoming.clip_id)) {
                const DocumentTrack* track =
                    self.state->document.FindTrackForClip(upcoming.clip_id);
                const RationalTime entry =
                    self.state->playbackDirection > 0
                        ? entered->timeline_in
                        : entered->timeline_in.add(entered->duration)
                              .sub(RationalTime{
                                  self.state->document.sequence.frame_rate.den,
                                  self.state->document.sequence.frame_rate
                                      .num});
                if (track != nullptr)
                    plan = [self readPlanForClip:upcoming.clip_id
                                         onTrack:track->id
                                              at:entry
                                        onScreen:NO];
            }
            [self requestFrame:upcoming.source_frame
                      ofSource:upcoming.source_id
                          plan:plan];
        }
        // PERF13 -- a hidden track shown again within the same window: its
        // frame there is asked for now, as a cut's is. Asked only once
        // shown, it would be a whole seek and decode late, and the monitor
        // would show the picture its decoder last held, from before the
        // cutaway, meanwhile. PERF14 -- and earlier than a cut, since the
        // cutaway it comes back from reads the same disk until its end.
        int64_t reappearSeconds = read_ahead::kReappearanceLookaheadSeconds;
        if (AppTestHooks* hooks = TestHooks()) {
            const PlaybackTuning tuning = hooks->Playback();
            reappearSeconds = tuning.read_ahead ? tuning.reappear_seconds : 1;
        }
        const RationalTime reappearance{speed * reappearSeconds, 1};
        for (const auto& [trackId, shownAt] : monitor_occlusion::NextShown(
                 self.state->document, *self.state->timeline, position,
                 self.state->playbackDirection, reappearance,
                 self.state->hiddenTrackIds, pictures, zoomed)) {
            for (const ResolvedLayer& layer :
                 self.state->timeline->ResolveTrackLayers(trackId, shownAt)) {
                const DocumentClip* clip =
                    self.state->document.FindClip(layer.frame.clip_id);
                if (clip == nullptr || IsMotionClip(*clip) ||
                    onScreen(layer.frame.source_id))
                    continue;
                self.state->upcomingSourceIds.insert(layer.frame.source_id);
                self.state->readSourceIds.insert(layer.frame.source_id);
                [self requestFrame:layer.frame.source_frame
                          ofSource:layer.frame.source_id
                              plan:[self readPlanForClip:layer.frame.clip_id
                                                 onTrack:trackId
                                                      at:shownAt
                                                onScreen:NO]];
            }
        }
    }
    // PERF14 -- the window of decoded pictures is shared among the sources
    // read, in playback; at rest, among the decoders open, as before.
    if (self.state->frameCache) {
        bool share = self.state->playbackDirection != 0;
        if (AppTestHooks* hooks = TestHooks())
            share = share && hooks->Playback().read_ahead;
        self.state->frameCache->SetReadSources(share ? self.state->readSourceIds
                                                     : std::set<Ulid>{});
    }
    [self openDecodersAhead:position];
}

- (void)requestSourcePosition:(RationalTime)position {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source) return;
    if (position < RationalTime{0, source->duration.rate})
        position = {0, source->duration.rate};
    if (position > source->duration) position = source->duration;
    const int64_t totalFrames =
        source->duration.to_frames(source->rate.num, source->rate.den);
    const int64_t frame = std::clamp<int64_t>(
        position.to_frames(source->rate.num, source->rate.den), 0,
        std::max<int64_t>(0, totalFrames - 1));
    position = {frame * static_cast<int64_t>(source->rate.den),
                source->rate.num};
    if (position > source->duration) position = source->duration;
    self.state->sourceMonitorPosition = position;
    [self requestFrame:frame ofSource:source->id];
    self.state->overlayDirty = true;
}

- (void)updateSourceZoneLabel {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    const MediaRate rate = source ? source->rate : [self playheadFrameRate];
    NSString* inText = self.state->sourceIn
                           ? TimelineTimecode(*self.state->sourceIn, rate)
                           : @"—";
    NSString* outText = self.state->sourceOut
                            ? TimelineTimecode(*self.state->sourceOut, rate)
                            : @"—";
    self.sourceMonitorZoneLabel.stringValue =
        [NSString stringWithFormat:@"IN %@    OUT %@", inText, outText];
}

- (void)scrubSourceMonitorEvent:(NSEvent*)event {
    // Out of lot, 29 October 2026 -- a scrub takes the playhead from the
    // player.
    if (self.state->sourcePlaybackDirection != 0)
        [self setSourcePlaybackDirection:0];
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || self.sourceMonitorView.bounds.size.width <= 0.0) return;
    const NSPoint point =
        [self.sourceMonitorView convertPoint:event.locationInWindow
                                    fromView:nil];
    const long double fraction = std::clamp<long double>(
        point.x / self.sourceMonitorView.bounds.size.width, 0.0L, 1.0L);
    const int64_t frames =
        source->duration.to_frames(source->rate.num, source->rate.den);
    [self requestSourcePosition:{static_cast<int64_t>(
                                     std::llround(fraction * frames)) *
                                     static_cast<int64_t>(source->rate.den),
                                 source->rate.num}];
}

- (void)monitorZoomChanged:(NSPopUpButton*)sender {
    const double zoom =
        sender.selectedItem.tag <= 0 ? 0.0 : sender.selectedItem.tag / 100.0;
    if (sender == self.sourceMonitorZoomPopup)
        self.state->sourceMonitorZoom = zoom;
    else if (sender == self.programMonitorZoomPopup)
        self.state->programMonitorZoom = zoom;
    else
        return;
    // Zoomed, the monitor hides nothing, and the reader reads every layer.
    if (sender == self.programMonitorZoomPopup) [self syncPlaybackReader];
    self.state->overlayDirty = true;
}

- (NSString*)transportStatus {
    if (self.state->playbackDirection > 0)
        return [NSString
            stringWithFormat:@"Lecture ▶ ×%d", self.state->playbackDirection];
    if (self.state->playbackDirection < 0)
        return [NSString
            stringWithFormat:@"Lecture ◀ ×%d", -self.state->playbackDirection];
    return @"Pause";
}

- (NSString*)playheadResolutionStatus {
    NSString* name = self.state->playheadResolution == PlayheadResolution::Frame
                         ? @"Image"
                         : @"Échantillon 48 kHz";
    const std::string key = ui::shortcuts::DisplaySpec(std::string(
        self.shortcutBindings[@"playhead.resolution"].UTF8String ?: ""));
    if (key.empty()) {
        return name;
    }
    return
        [NSString stringWithFormat:@"%@ (%@)", name,
                                   [NSString stringWithUTF8String:key.c_str()]];
}

- (void)setPlaybackDirection:(int)direction {
    // ON9 -- any playback command but « Lire autour » itself ends it.
    self.playAroundActive = NO;
    // Out of lot, 29 October 2026 -- one player at a time: the timeline's
    // stops the source monitor's.
    if (direction != 0 && self.state->sourcePlaybackDirection != 0)
        [self setSourcePlaybackDirection:0];
    const bool wasPlaying = self.state->playbackDirection != 0;
    self.state->playbackDirection = std::clamp(direction, -4, 4);
    self.state->playbackAnchor = self.state->requestedPosition;
    // PERF10. Playback's read-ahead is for playback: at rest the decoders
    // fill a few frames around the playhead, and what playback cached past
    // kRestingCacheBytes goes, least recently shown first -- the frames at
    // the playhead stay.
    const bool playing = self.state->playbackDirection != 0;
    for (auto& worker : self.state->workers) worker.second->SetPlaying(playing);
    [self syncPlaybackReader];
    if (wasPlaying && !playing && self.state->frameCache)
        self.state->frameCache->TrimTo(kRestingCacheBytes);
    // PRD37 -- ROADMAP.md. A play goes straight to PlayFrom, which sets the
    // cursor, the speed and the direction itself. Stopping first closed the
    // sound output, and opening it again at once blocked here 130 to 200 ms
    // (measured on 3 October 2026) with the playback clock already running:
    // three to five frames skipped at every start, and the sound that late
    // behind the picture for the whole play. A stop silences the output and
    // leaves it open (AudioPlayback.h).
    if (self.state->audioPlayback) {
        if (!playing) {
            self.state->audioPlayback->Stop();
        } else {
            std::string error;
            if (!self.state->audioPlayback->PlayFrom(
                    self.state->playbackAnchor, self.state->playbackDirection,
                    error))
                std::fprintf(stderr, "Audio playback failed: %s\n",
                             error.c_str());
        }
    }
    // FC4 -- stopped in trim mode, Record is the Trim Monitor again.
    if (!playing && wasPlaying && self.state->trimSession.Active())
        [self requestResolvedPosition:self.state->requestedPosition];
    // The clock starts once the sound has: an output that still had to open
    // (after its idle grace) holds the first image that long, sound and
    // picture together, instead of skipping the frames it took.
    self.state->playbackStarted = std::chrono::steady_clock::now();
    [self updateSelectionInfo];
}

- (void)advancePlayback {
    if (self.state->playbackDirection == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const int64_t elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - self.state->playbackStarted)
            .count();
    if (elapsedUs <= 0) return;
    const RationalTime delta{
        elapsedUs * static_cast<int64_t>(self.state->playbackDirection),
        1000000};
    const RationalTime next = self.state->playbackAnchor.add(delta);
    if (next <= RationalTime{0, 1}) {
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        [self setPlaybackDirection:0];
    } else if (next >= self.state->duration) {
        [self requestTimelinePosition:self.state->duration];
        [self setPlaybackDirection:0];
    } else {
        [self requestTimelinePosition:next];
    }
}

// Out of lot -- the author, 29 October 2026: "I need to play in the source
// monitor, picture and sound, with an indicator of the player". The shown
// rush alone, on an audio track of its own, for the source monitor's player:
// the timeline's mix is never rebuilt for it.
- (void)prepareSourceAudioForSource:(const DocumentSource&)source {
    if (self.state->sourceAudioPlayback &&
        self.state->sourceAudioMediaId == source.id)
        return;
    Document rush;
    rush.sources = {source};
    rush.sequence.frame_rate = source.rate;
    DocumentClip clip;
    clip.id = GenerateUlid();
    clip.source_id = source.id;
    clip.source_in = {0, source.duration.rate};
    clip.duration = source.duration;
    clip.timeline_in = {0, source.duration.rate};
    DocumentTrack track;
    track.id = GenerateUlid();
    track.kind = "audio";
    track.clips = {clip};
    rush.sequence.tracks = {track};
    if (!self.state->sourceAudioPlayback) {
        auto player = std::make_unique<AudioPlayback>();
        const std::filesystem::path base =
            std::filesystem::absolute(
                std::filesystem::path(self.documentPath.UTF8String ?: ""))
                .parent_path();
        std::string error;
        if (!player->Open(rush, base.string(), error)) {
            std::fprintf(stderr, "Source audio unavailable: %s\n",
                         error.c_str());
            return;
        }
        self.state->sourceAudioPlayback = std::move(player);
    } else {
        self.state->sourceAudioPlayback->RebuildTimeline(rush);
    }
    self.state->sourceAudioMediaId = source.id;
}

// The source monitor's J/K/L: one direction at a time, the timeline's
// playback stopped while it plays (and the other way round,
// -setPlaybackDirection:).
- (void)setSourcePlaybackDirection:(int)direction {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || !self.state->sourceMonitor) direction = 0;
    direction = std::clamp(direction, -4, 4);
    if (direction != 0 && self.state->playbackDirection != 0)
        [self setPlaybackDirection:0];
    const bool wasPlaying = self.state->sourcePlaybackDirection != 0;
    self.state->sourcePlaybackDirection = direction;
    if (direction == 0) {
        if (self.state->sourceAudioPlayback)
            self.state->sourceAudioPlayback->Stop();
        if (wasPlaying) self.state->overlayDirty = true;
        return;
    }
    RationalTime anchor = self.state->sourceMonitorPosition;
    // From the end, a play starts the rush again; from the start, J plays
    // back from the end.
    const RationalTime lastFrame = source->duration.sub(
        {static_cast<int64_t>(source->rate.den), source->rate.num});
    if (direction > 0 && anchor >= lastFrame)
        anchor = {0, source->duration.rate};
    if (direction < 0 && anchor <= RationalTime{0, 1})
        anchor = source->duration;
    self.state->sourcePlaybackAnchor = anchor;
    [self prepareSourceAudioForSource:*source];
    if (self.state->sourceAudioPlayback) {
        std::string error;
        if (!self.state->sourceAudioPlayback->PlayFrom(anchor, direction,
                                                       error))
            std::fprintf(stderr, "Source audio playback failed: %s\n",
                         error.c_str());
    }
    // As for the timeline: the clock starts once the sound has.
    self.state->sourcePlaybackStarted = std::chrono::steady_clock::now();
    self.state->overlayDirty = true;
}

- (void)advanceSourcePlayback {
    if (self.state->sourcePlaybackDirection == 0) return;
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || !self.state->sourceMonitorVisible) {
        [self setSourcePlaybackDirection:0];
        return;
    }
    const int64_t elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() -
            self.state->sourcePlaybackStarted)
            .count();
    const SourcePlaybackStep step = AdvanceSourcePlayback(
        self.state->sourcePlaybackAnchor, self.state->sourcePlaybackDirection,
        elapsedUs, source->duration);
    [self requestSourcePosition:step.position];
    if (step.ended) [self setSourcePlaybackDirection:0];
}

- (void)displayTick:(NSTimer*)timer {
    (void)timer;
    [self advancePlayback];
    [self checkPlayAround];
    [self advanceSourcePlayback];
    [self processCompletedMediaImports];
    [self processCompletedMediaProxies];
    [self processCompletedMediaWaveforms];
    [self processCompletedMediaThumbnails];
    [self processCompletedMediaTranscriptions];
    [self processCompletedMediaRelinks];
    [self processCompletedBatchRelinks];
    [self refreshMediaTaskStatus];
    // PRD37 -- a stop leaves the sound output open for the next play; it
    // closes here once silent for long enough (PlaybackPresentation.h).
    if (self.state && self.state->audioPlayback)
        self.state->audioPlayback->ReleaseIdleOutput(
            std::chrono::steady_clock::now());
    if (self.state && self.state->sourceAudioPlayback)
        self.state->sourceAudioPlayback->ReleaseIdleOutput(
            std::chrono::steady_clock::now());
    // FC4 -- J or L running in trim mode: the offset follows the clock.
    if (self.state) [self refreshDynamicTrim];
    [self presentNearestFrameAtDeadline:YES];
}

- (std::filesystem::path)decodeBasePath {
    return std::filesystem::absolute(
               std::filesystem::path(self.documentPath.UTF8String ?: ""))
        .parent_path();
}

// PERF10 -- DecoderPool.h. Opening a project used to open a decoder for
// every video source, off the main thread but all at once; on a 169-rush 4K
// shoot that was 1,030 threads and 2 GB before the first frame, and the
// decoders then kept what they decoded. What opening a project needs to know
// up front is only which media are missing, so the relink flow and the
// offline badges are right: that is a stat per file, done here in the
// background. Decoders open when the monitor asks for a source
// (-requestFrame:ofSource:).
//
// The trade: a file that is there but does not match what the project
// declares (another frame rate, fewer frames) used to be marked offline at
// open; it is now marked offline the first time it is decoded.
- (void)checkSourceAvailabilityInBackground {
    if (!self.state || !self.state->frameCache) return;
    const std::filesystem::path base = [self decodeBasePath];
    auto pending = std::make_shared<std::vector<PendingDecodeOpen>>();
    for (const DocumentSource& source : self.state->document.sources) {
        if (auto item = DecodeOpenFor(self.state->document, source, base,
                                      self.state->proxiesEnabled))
            pending->push_back(std::move(*item));
    }
    self.state->checkingSourceAvailability = !pending->empty();
    if (pending->empty()) {
        // PERF12 -- nothing to check: what playback shows next may open.
        self.state->sourceAvailabilityKnown = true;
        [self openDecodersAhead:self.state->requestedPosition];
        return;
    }
    auto available = std::make_shared<std::vector<char>>(pending->size(), 0);
    AppState* state = self.state;
    const uint64_t generation = state->sourceAvailabilityGeneration;
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      ParallelForEachIndex(pending->size(), [&](size_t index) {
          (*available)[index] = DecodeMediaIsAvailable((*pending)[index]);
      });
      // Scheduled on the run loop rather than dispatched to the main queue.
      // The main queue is serial, so a block sent to it cannot run while
      // another one is still executing -- which is exactly the situation any
      // caller that spins the run loop waiting for this is in, the
      // in-process smoke test among them. Common modes additionally let the
      // install land during event tracking, the same reason the display
      // timer is registered there.
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || strongSelf.state != state ||
            state->sourceAvailabilityGeneration != generation)
            return;
        state->checkingSourceAvailability = false;
        bool changed = false;
        for (size_t index = 0; index < pending->size(); ++index) {
            const PendingDecodeOpen& item = (*pending)[index];
            // A decoder that opened meanwhile has already answered.
            if ((*available)[index] ||
                !state->document.FindSource(item.source_id) ||
                state->workers.count(item.source_id) != 0)
                continue;
            std::fprintf(stderr, "Source offline %s at %s\n",
                         item.source_id.c_str(), item.original.c_str());
            changed |= state->offlineSourceIds.insert(item.source_id).second;
        }
        // PERF12 -- the missing media are known: what playback shows next
        // may open, ahead of it (a refresh plans it too).
        state->sourceAvailabilityKnown = true;
        if (changed)
            [strongSelf refreshDecodedPositions];
        else
            [strongSelf openDecodersAhead:state->requestedPosition];
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

// PERF10. Every frame the monitors ask for goes through here. A source with
// a decoder gets the request and counts as used now; one without is opened
// in the background, and asked again once it is installed.
- (void)requestFrame:(int64_t)frame ofSource:(const Ulid&)sourceId {
    [self requestFrame:frame ofSource:sourceId plan:{}];
}

- (void)requestFrame:(int64_t)frame
            ofSource:(const Ulid&)sourceId
                plan:(const DecodeWorker::ReadPlan&)plan {
    const auto worker = self.state->workers.find(sourceId);
    if (worker != self.state->workers.end()) {
        self.state->decoderUsage.Touch(sourceId);
        worker->second->RequestFrame(frame, plan);
        return;
    }
    [self openDecoderForSource:sourceId];
}

// PERF15 -- PlaybackReader.h. The one reader reads ahead of forward
// playback, where the frames shown follow on in their files; played
// backward, or at rest, each decoder reads its own file as before. Started
// again on a copy of the document after an edit or a zoom, so that what it
// reads is what is shown.
- (void)syncPlaybackReader {
    AppState* state = self.state;
    bool wanted = state->playbackDirection > 0 && state->timeline != nullptr;
    if (AppTestHooks* hooks = TestHooks())
        wanted = wanted && hooks->Playback().reader;
    if (!wanted) {
        state->playbackReader.Stop();
        return;
    }
    state->playbackReader.Start(
        std::make_unique<Document>(state->document),
        state->programMonitorZoom > 0.0, state->requestedPosition,
        playback_reader::kReaderHorizonSeconds * state->playbackDirection);
}

// PERF14 -- how playback reads a clip ahead (ReadAhead.h): within the
// source frames it shows, seen from `position` -- its out point, or the
// first frame a cutaway above hides --, and its bytes asked of the system
// ahead once it is on screen, its first second while it is still to come.
// Nothing at rest, where a decoder fills a few frames and nobody waits on
// the disk.
- (DecodeWorker::ReadPlan)readPlanForClip:(const Ulid&)clipId
                                  onTrack:(const Ulid&)trackId
                                       at:(RationalTime)position
                                 onScreen:(BOOL)onScreen {
    DecodeWorker::ReadPlan plan;
    const int direction = self.state->playbackDirection;
    if (direction == 0 || !self.state->timeline) return plan;
    int64_t byteSeconds = read_ahead::kByteHorizonSeconds;
    if (AppTestHooks* hooks = TestHooks()) {
        const PlaybackTuning tuning = hooks->Playback();
        if (!tuning.read_ahead) return plan;
        byteSeconds = tuning.byte_seconds;
    }
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    if (clip == nullptr) return plan;
    // PERF15 -- the playback reader reads this clip's bytes: asked of the
    // system a second time here, they would be read twice at once.
    if (self.state->playbackReader.Running() &&
        self.state->playbackReader.Covers(clip->source_id))
        byteSeconds = 0;
    const int64_t reach =
        std::max<int64_t>(byteSeconds, read_ahead::kByteChunkSeconds * 2) *
        std::abs(direction);
    const monitor_occlusion::PictureLookup pictures =
        playback_reads::PicturesFromLibrary(self.state->document);
    const bool zoomed = self.state->programMonitorZoom > 0.0;
    plan.span = playback_reads::VisibleReadSpan(
        self.state->document, *self.state->timeline, trackId, *clip, position,
        direction, RationalTime{reach, 1}, pictures, zoomed);
    // PERF17 -- the next piece of the same rush, decoded on into.
    bool nextPiece = true;
    if (AppTestHooks* hooks = TestHooks())
        nextPiece = hooks->Playback().next_piece;
    if (nextPiece)
        plan.next_piece = playback_reads::NextPieceFrame(
            self.state->document, *self.state->timeline, trackId, *clip,
            position, direction, RationalTime{reach, 1}, pictures, zoomed);
    if (byteSeconds <= 0) return plan;
    plan.byte_horizon = playback_reads::SourceFramesIn(
        self.state->document, *clip,
        onScreen ? byteSeconds : read_ahead::kByteChunkSeconds);
    plan.byte_chunk = playback_reads::SourceFramesIn(
        self.state->document, *clip, read_ahead::kByteChunkSeconds);
    return plan;
}

// Whether an open was started: none for a source already open or on its
// way, offline, closing (it reopens once closed), or with no video to decode.
- (BOOL)openDecoderForSource:(const Ulid&)sourceId {
    AppState* state = self.state;
    if (!state || !state->frameCache || state->workers.count(sourceId) != 0 ||
        state->openingDecoders.count(sourceId) != 0 ||
        state->offlineSourceIds.count(sourceId) != 0)
        return NO;
    if (state->closingDecoders.count(sourceId) != 0) {
        state->reopenAfterClose.insert(sourceId);
        return NO;
    }
    const DocumentSource* source = state->document.FindSource(sourceId);
    if (!source) return NO;
    const std::optional<PendingDecodeOpen> item =
        DecodeOpenFor(state->document, *source, [self decodeBasePath],
                      state -> proxiesEnabled);
    if (!item) return NO;
    if (AppTestHooks* hooks = TestHooks()) hooks->DecoderOpening(sourceId);
    state->openingDecoders.insert(sourceId);
    state->openingSince[sourceId] = std::chrono::steady_clock::now();
    // Both outlive this work and are internally locked, so the opening
    // thread may use them directly.
    FrameCache* cache = state->frameCache.get();
    PerformanceMetrics* metrics = state->performanceMetrics.get();
    const DocumentSource declared = *source;
    const PendingDecodeOpen pending = *item;
    // A unique_ptr cannot be captured by a block copied between queues; a
    // shared_ptr holding one can.
    auto opened = std::make_shared<std::unique_ptr<DecodeWorker>>();
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      auto worker =
          std::make_unique<DecodeWorker>(pending.source_id, *cache, *metrics);
      bool ready = worker->Open(pending.selected, 5);
      // A proxy that does not open, or is not the media the project
      // declares, falls back to the original, as it always has.
      if (pending.using_proxy &&
          (!ready || !DecodeWorkerMatchesSource(*worker, declared))) {
          worker = std::make_unique<DecodeWorker>(pending.source_id, *cache,
                                                  *metrics);
          ready = worker->Open(pending.original, 5);
      }
      if (ready) *opened = std::move(worker);
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || strongSelf.state != state) return;
        [strongSelf installDecoder:opened opening:pending];
      });
      CFRunLoopWakeUp(mainLoop);
    });
    return YES;
}

- (void)installDecoder:(std::shared_ptr<std::unique_ptr<DecodeWorker>>)opened
               opening:(const PendingDecodeOpen&)pending {
    AppState* state = self.state;
    const Ulid sourceId = pending.source_id;
    state->openingDecoders.erase(sourceId);
    if (AppTestHooks* hooks = TestHooks()) {
        // PERF12 -- what an open costs on the probed drive, at rest and
        // while playback streams.
        const auto openedSince = state->openingSince.find(sourceId);
        if (openedSince != state->openingSince.end())
            hooks->DecoderInstalled(
                std::filesystem::path(pending.selected).filename().string(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              openedSince->second)
                    .count(),
                state->playbackDirection != 0);
    }
    state->openingSince.erase(sourceId);
    std::unique_ptr<DecodeWorker> worker = std::move(*opened);
    const DocumentSource* source = state->document.FindSource(sourceId);
    const std::optional<PendingDecodeOpen> current =
        source ? DecodeOpenFor(state->document, *source, [self decodeBasePath],
                               state -> proxiesEnabled)
               : std::nullopt;
    // The source went away, or names other media than when the open started
    // (relinked, proxies toggled): the next request opens the right file.
    // PERF12 -- every path below plans the next open ahead, a refresh too.
    if (!current || current->selected != pending.selected ||
        state->workers.count(sourceId) != 0) {
        worker.reset();
        if (current)
            [self refreshDecodedPositions];
        else
            [self openDecodersAhead:state->requestedPosition];
        return;
    }
    if (!worker || !DecodeWorkerMatchesSource(*worker, *source)) {
        if (worker)
            std::fprintf(
                stderr, "Source %s declares rate %d/%d but media is %d/%d\n",
                sourceId.c_str(), source->rate.num, source->rate.den,
                worker->FrameRateNumerator(), worker->FrameRateDenominator());
        else
            std::fprintf(stderr, "Source offline %s at %s\n", sourceId.c_str(),
                         pending.original.c_str());
        worker.reset();
        state->offlineSourceIds.insert(sourceId);
        [self refreshDecodedPositions];
        return;
    }
    state->offlineSourceIds.erase(sourceId);
    worker->SetPlaying(state->playbackDirection != 0);
    // PERF15 -- where the playback reader finds this source's pictures.
    state->playbackReader.SetSource(sourceId, pending.selected,
                                    worker->SampleTable());
    worker->Start();
    state->workers.emplace(sourceId, std::move(worker));
    state->decoderUsage.Touch(sourceId);
    [self trimDecoderPool];
    [self refreshDecodedPositions];
}

// PERF12 -- DecoderPool.h. On the external drive projects are cut from, an
// open takes seconds while playback streams another rush from it, and the
// one-second warm-up above came far too late: every cut to a rush not yet
// open stopped the picture. So the rushes playback shows next -- forward of
// the playhead at rest, in the sense of travel in playback -- are opened
// before it gets there, soonest first, one at a time, within the capacity.
// Planned again at every position asked for, every install and every close;
// what is planned is held (-trimDecoderPool).
- (void)openDecodersAhead:(RationalTime)position {
    AppState* state = self.state;
    if (!state) return;
    state->plannedSourceIds.clear();
    if (!state->timeline || !state->frameCache) return;
    std::vector<decoder_pool::Need> needs = decoder_pool::UpcomingNeeds(
        state->document, position, state->playbackDirection,
        RationalTime{decoder_pool::kLookaheadSeconds, 1});
    if (state->sourceMonitor && !state->sourceMonitorId.empty())
        needs.insert(needs.begin(),
                     decoder_pool::Need{state->sourceMonitorId, {0, 1}});
    decoder_pool::PoolState pool;
    for (const auto& worker : state->workers) pool.open.insert(worker.first);
    const auto now = std::chrono::steady_clock::now();
    for (const Ulid& sourceId : state->openingDecoders) {
        const auto since = state->openingSince.find(sourceId);
        pool.opening[sourceId] = since == state->openingSince.end()
                                     ? std::chrono::steady_clock::duration{}
                                     : now - since->second;
    }
    pool.closing = state->closingDecoders;
    pool.offline = state->offlineSourceIds;
    const decoder_pool::Plan plan =
        decoder_pool::PlanOpens(needs, pool, state->decoderCapacity);
    state->plannedSourceIds.insert(plan.keep.begin(), plan.keep.end());
    if (!state->sourceAvailabilityKnown || state->checkingSourceAvailability)
        return;
    size_t started = 0;
    for (const Ulid& sourceId : plan.open) {
        if (started >= plan.budget) break;
        if ([self openDecoderForSource:sourceId]) ++started;
    }
}

// Stopping joins the decode thread, possibly mid-picture, and destroying the
// worker frees the codec and its threads: tens of milliseconds for a 4K
// decoder, which would be a hitch at the cut that made room for another, so
// it happens off the main thread. The frames it cached stay in the frame
// cache until its budget evicts them.
- (void)closeDecoderForSource:(const Ulid&)sourceId {
    AppState* state = self.state;
    auto node = state->workers.extract(sourceId);
    if (node.empty()) return;
    state->decoderUsage.Forget(sourceId);
    state->closingDecoders.insert(sourceId);
    auto closing = std::make_shared<std::unique_ptr<DecodeWorker>>(
        std::move(node.mapped()));
    const Ulid closed = sourceId;
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      closing->reset();
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || strongSelf.state != state) return;
        state->closingDecoders.erase(closed);
        if (state->reopenAfterClose.erase(closed) != 0)
            [strongSelf openDecoderForSource:closed];
        else
            [strongSelf openDecodersAhead:state->requestedPosition];
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

// Recomposite both monitors from what the decoders now hold, and ask the
// decoders for their current positions again.
- (void)refreshDecodedPositions {
    AppState* state = self.state;
    state->rendered.clear();
    state->sourceRendered = {};
    [self requestResolvedPosition:state->requestedPosition];
    if (state->sourceMonitor) {
        [self requestSourcePosition:state->sourceMonitorPosition];
        self.sourceOfflineMediaLabel.hidden =
            state->offlineSourceIds.count(state->sourceMonitorId) == 0;
    }
    state->overlayDirty = true;
}

// PERF-2026-09. This used to stop every decoder and reopen every one of
// them, on the main thread, on each call -- and it is called after every
// project mutation and every proxy toggle. Reopening is an
// avformat_open_input plus avformat_find_stream_info per rush, which on the
// external drives this project is cut from is hundreds of milliseconds each,
// so a project holding twenty rushes froze the editor for seconds every time
// a source list was touched at all. Clearing each source from the frame
// cache alongside threw away decoded pictures that were still valid, making
// the next few frames of playback re-decode as well.
//
// A worker already open on exactly the media the document now names is still
// correct, and so is everything it has cached, so it is kept as-is. Since
// PERF10 nothing else is opened here: availability is a stat per file, and
// the monitors open what they show when they ask for it.
//
// One case still reopens every time: a source whose proxy exists but is
// rejected as incompatible falls back to the original, so the worker's path
// never equals the proxy path the next call selects. That is the old
// behaviour, not a regression, and it stops as soon as the bad proxy is
// removed or regenerated.
- (void)reloadDecodeWorkers {
    if (AppTestHooks* hooks = TestHooks()) hooks->DecodeWorkersReloaded();
    if (!self.state->frameCache) return;
    AppState* state = self.state;
    // An availability check still in flight answers for the sources as they
    // were; this pass answers for them as they are.
    ++state->sourceAvailabilityGeneration;
    state->checkingSourceAvailability = false;
    const std::filesystem::path base = [self decodeBasePath];
    std::map<Ulid, std::unique_ptr<DecodeWorker>> retained;
    std::set<Ulid> offline;
    for (const DocumentSource& source : state->document.sources) {
        const std::optional<PendingDecodeOpen> item =
            DecodeOpenFor(state->document, source, base, state->proxiesEnabled);
        if (!item) {
            state->frameCache->ClearSource(source.id);
            continue;
        }
        if (!DecodeMediaIsAvailable(*item)) offline.insert(source.id);
        const auto existing = state->workers.find(source.id);
        if (existing != state->workers.end()) {
            if (existing->second->Path() == item->selected &&
                DecodeWorkerMatchesSource(*existing->second, source)) {
                retained.emplace(source.id, std::move(existing->second));
                state->workers.erase(existing);
                continue;
            }
            // Stopped before its cached pictures are cleared, so that none
            // of the old media lands in the cache afterwards.
            existing->second->Stop();
        }
        state->frameCache->ClearSource(source.id);
    }
    // Whatever is still here belongs to a source the document no longer has,
    // or to one whose media changed.
    std::vector<Ulid> stale;
    for (const auto& worker : state->workers) stale.push_back(worker.first);
    for (const Ulid& sourceId : stale) [self closeDecoderForSource:sourceId];
    state->workers = std::move(retained);
    state->offlineSourceIds = std::move(offline);
    // PERF12 -- this pass answered for every source, so what playback shows
    // next may open ahead even when the check it supersedes never lands.
    state->sourceAvailabilityKnown = true;
    [self refreshDecodedPositions];
}

// R6 -- docs/roadmap/REEL-2026-09.md. Reads the sequence's look out of the
// project package when the document names a digest the app does not hold
// yet, and drops it when the document names none. The read checks the file
// against that digest (LoadSequenceLookLut), so a .cube replaced under the
// same name is refused rather than shown.
//
// A read that fails leaves the monitor without the look, which is a picture
// that is not the film's: it is reported, once per digest, because there is
// nothing else the monitor can draw and silence would be the worse of the
// two.
// PA15 -- docs/roadmap/PARITE-2026-11.md. The matte of `frame` of a media
// for Record's slot `slot` (PersonMatte.h): the reader is kept open per
// media and opened again when its file is rewritten; the frame's bytes
// stay in matteFrames[slot] until the next render. Empty when the media
// has no usable matte, which is said once in the log.
- (MonitorMatte)personMatteForMedia:(const Ulid&)mediaId
                              frame:(int64_t)frame
                               slot:(size_t)slot
                            package:(const std::string&)package {
    MonitorMatte none;
    std::string path;
    person_matte::Info info;
    std::string error;
    if (!person_matte::MatteForMedia(self.state->document, package, mediaId,
                                     path, info, error)) {
        if (self.state->reportedMattes.insert(mediaId + error).second)
            std::fprintf(stderr, "Record: %s\n", error.c_str());
        self.state->personMattes.erase(mediaId);
        return none;
    }
    std::error_code timeError;
    const auto written = std::filesystem::last_write_time(path, timeError);
    auto& open = self.state->personMattes[mediaId];
    if (!open.reader || open.path != path || open.written != written) {
        open.reader = std::make_unique<person_matte::Reader>();
        open.path = path;
        open.written = written;
        if (!open.reader->Open(path, error)) {
            if (self.state->reportedMattes.insert(mediaId + error).second)
                std::fprintf(stderr, "Record: %s\n", error.c_str());
            self.state->personMattes.erase(mediaId);
            return none;
        }
    }
    if (slot >= self.state->matteFrames.size()) return none;
    std::vector<uint8_t>& bytes = self.state->matteFrames[slot];
    if (!open.reader->Frame(frame, bytes, error)) {
        if (self.state->reportedMattes.insert(mediaId + error).second)
            std::fprintf(stderr, "Record: %s\n", error.c_str());
        return none;
    }
    MonitorMatte matte;
    matte.bytes = bytes.data();
    matte.width = open.reader->info().width;
    matte.height = open.reader->info().height;
    return matte;
}

// LG5 -- docs/roadmap/LEGENDES-2026-10.md. Record shows the cartons as the
// export draws them: the caption renderer's picture of the frame, each
// carton in its own style, its lines broken where the file breaks them,
// drawn at the pixels the frame is shown at -- never more than the
// sequence's own: zoomed past one to one, it is enlarged with the video
// under it. Drawn again only when what it shows changes.
- (void)showProgramCaptions:(const std::vector<CaptionCarton>&)cartons
                       text:(NSString*)text {
    [self showCaptions:cartons
                  text:text
                  view:self.programCaptionView
               monitor:self.programMonitorView
                  zoom:self.state->programMonitorZoom];
}

- (void)showCaptions:(const std::vector<CaptionCarton>&)cartons
                text:(NSString*)text
                view:(CMCaptionView*)view
             monitor:(NSView*)monitor
                zoom:(double)zoom {
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    const DocumentSequence& sequence = self.state->document.sequence;
    const NSRect bounds = monitor.bounds;
    const double layerScale =
        monitor.layer.contentsScale > 0.0 ? monitor.layer.contentsScale : 1.0;
    // Zoomed, the renderer draws a sequence pixel as `zoom` of its layer's
    // pixels: that many over the layer's scale in points.
    const double zoomPoints = zoom > 0.0 ? zoom / layerScale : 0.0;
    const ui::caption_overlay::Layout canvas = ui::caption_overlay::Compute(
        sequence.width, sequence.height, bounds.size.width, bounds.size.height,
        zoomPoints);
    const auto hide = [&] {
        view.hidden = YES;
        view.captionText = @"";
    };
    if (cartons.empty() || renderer == nullptr || canvas.canvas_width <= 0.0 ||
        canvas.canvas_height <= 0.0) {
        hide();
        return;
    }
    // The screen's pixels per point, asked of the view.
    const double pixelScale = std::max(
        1.0, [monitor convertSizeToBacking:NSMakeSize(1.0, 1.0)].width);
    const int32_t width = std::clamp<int32_t>(
        static_cast<int32_t>(std::lround(canvas.canvas_width * pixelScale)), 1,
        sequence.width);
    const int32_t height = std::max<int32_t>(
        1, static_cast<int32_t>(std::lround(static_cast<double>(width) *
                                            sequence.height / sequence.width)));
    std::ostringstream key;
    key << width << 'x' << height << '/' << sequence.width;
    for (const CaptionCarton& carton : cartons) {
        const CaptionStyle& style = carton.style;
        key << '\x1e' << carton.text << '\x1f' << style.font_family << '\x1f'
            << style.font_size << '\x1f' << style.font_weight << '\x1f'
            << style.color << '\x1f' << style.position << '\x1f'
            << style.outline_width << '\x1f' << style.outline_color << '\x1f'
            << style.shadow_offset << '\x1f' << style.shadow_color << '\x1f'
            << style.background_color << '\x1f' << style.background_padding
            << '\x1f' << style.vertical_position.num << '/'
            << style.vertical_position.den << '\x1f' << carton.opacity.num
            << '/'
            << carton.opacity.den
            // PA4 -- a carton is redrawn when its next word starts.
            << '\x1f' << style.word_animation << '\x1f' << style.highlight_color
            << '\x1f' << carton.words_spoken;
    }
    NSString* drawnKey = CMStringFromUtf8(key.str());
    if (![view.drawnKey isEqualToString:drawnKey]) {
        std::vector<CaptionStyle> styles;
        std::vector<std::string> texts;
        for (const CaptionCarton& carton : cartons) {
            styles.push_back(carton.style);
            texts.push_back(carton.text);
        }
        std::string error;
        std::vector<CaptionCarton> delivered = cartons;
        CGImageRef picture = nullptr;
        CGRect ink = CGRectNull;
        if (WrapCaptionTextsForDelivery(styles, sequence, texts, error)) {
            for (size_t index = 0; index < delivered.size(); ++index)
                delivered[index].text = texts[index];
            CaptionDrawing drawing;
            drawing.width = width;
            drawing.height = height;
            drawing.sequence_width = sequence.width;
            renderer->draw(
                drawing, {delivered},
                [&](size_t, CaptionImage& image) {
                    picture = CMCaptionPicture(image);
                    for (const CaptionCartonLayout& layout : image.cartons) {
                        const caption_layout::Placement& placed =
                            layout.placement;
                        CGRect painted = CGRectMake(
                            placed.block.x - placed.outline,
                            placed.block.y - placed.outline,
                            placed.block.width + (2.0 * placed.outline) +
                                placed.shadow,
                            placed.block.height + (2.0 * placed.outline) +
                                placed.shadow);
                        if (placed.box) {
                            painted = CGRectUnion(
                                painted,
                                CGRectMake(placed.box_rect.x, placed.box_rect.y,
                                           placed.box_rect.width,
                                           placed.box_rect.height));
                        }
                        ink = CGRectIsNull(ink) ? painted
                                                : CGRectUnion(ink, painted);
                    }
                    return true;
                },
                error);
            if (!CGRectIsNull(ink)) {
                ink = CGRectMake(ink.origin.x / width, ink.origin.y / height,
                                 ink.size.width / width,
                                 ink.size.height / height);
            }
        }
        if (picture == nullptr) {
            hide();
            return;
        }
        view.layer.contents = (__bridge id)picture;
        CGImageRelease(picture);
        view.drawnKey = drawnKey;
        view.inkShare = ink;
    }
    view.frame =
        NSMakeRect(canvas.canvas_x,
                   bounds.size.height - canvas.canvas_y - canvas.canvas_height,
                   canvas.canvas_width, canvas.canvas_height);
    view.captionText = text;
    view.hidden = NO;
}

- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline {
    if (!self.state->frameCache || !self.state->renderer ||
        !self.state->sourceRenderer || !self.state->programRenderer) {
        return;
    }
    // PERF7 -- ROADMAP.md. The timeline's display list is timed further
    // down. The report is off unless CUTMACHINE_RENDER_COST says where to
    // write it (RenderCostMeter.h), and off costs this null check. The
    // signposts are always emitted and cost an enabled check until
    // Instruments records Points of Interest. Statics, because both describe
    // this process, never the project.
    static const std::unique_ptr<render_cost::Recorder> renderCost = [] {
        gRenderCostOriginNs = render_cost::NowNanoseconds();
        return render_cost::Recorder::Open(
            render_cost::ParseDestination(
                std::getenv(render_cost::kEnvironmentVariable)),
            gRenderCostOriginNs);
    }();
    static os_log_t const renderLog = os_log_create(
        "com.cutmachine.editor", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    if (renderCost && isDisplayDeadline) {
        renderCost->Tick(render_cost::NowNanoseconds());
    }

    std::vector<AVFrame*> frames(self.state->requested.size(), nullptr);
    std::vector<RenderedSlot> candidates(self.state->requested.size());
    std::vector<playback::LayerReadiness> readiness(
        self.state->requested.size());
    // MOTION-2026-09 M4 -- a motion clip's frame is drawn here, not decoded:
    // rendered once (0.5 ms at 1080x1920, M4), kept in motionPreviews, and
    // handed to Renderer.mm as a planar picture with alpha. Its AVFrame only
    // points into the cache (no buffer references), and, like every entry of
    // `frames`, is freed by the loop at the end of this method, after
    // RenderFrames has copied it. It must have no other owner: a second
    // av_frame_free of the same struct corrupted the heap as soon as a
    // motion clip reached the Record monitor (visual QA, 19 September 2026).
    std::vector<uint8_t> displayReferred(self.state->requested.size(), 0);
    const std::string motionDirectory =
        self.documentPath.length > 0
            ? std::filesystem::absolute(self.documentPath.UTF8String)
                  .parent_path()
                  .string()
            : std::string();
    bool missing = false;
    // PERF13 -- for the play probe (AppTestHooks::DisplayDeadline):
    // overwritten by each active layer, so the topmost one's is left.
    bool topmostLate = false;
    for (size_t slot = 0; slot < self.state->requested.size(); ++slot) {
        const ResolvedSlot& requested = self.state->requested[slot];
        if (!requested.active) {
            continue;
        }
        topmostLate = false;
        MotionPreviewRequest motion;
        if (MotionPreviewFor(
                self.state->document,
                {requested.sourceId, requested.frame, requested.clipId},
                motion)) {
            const MotionPreviewImage* image =
                self.state->motionPreviews->Find(motion.key, motion.frame);
            if (image == nullptr && self.state->failedMotionFrames.count(
                                        {motion.key, motion.frame}) == 0) {
                MotionPreviewImage rendered;
                MotionError error;
                if (RenderMotionPreview(motion, self.state->document,
                                        motionDirectory, rendered, error)) {
                    image = &self.state->motionPreviews->Insert(
                        motion.key, motion.frame, std::move(rendered));
                } else {
                    self.state->failedMotionFrames.insert(
                        {motion.key, motion.frame});
                    if (self.state->reportedMotionKeys.insert(motion.key)
                            .second)
                        std::fprintf(stderr, "Motion clip %s: %s: %s\n",
                                     requested.clipId.c_str(),
                                     error.code.c_str(), error.message.c_str());
                }
            }
            candidates[slot] = {true, motion.key, motion.frame,
                                requested.opacity, requested.clipId};
            readiness[slot] = {true, image != nullptr, true};
            AVFrame* frame = image != nullptr ? av_frame_alloc() : nullptr;
            if (frame != nullptr) {
                frame->format = AV_PIX_FMT_GBRAP;
                frame->width = image->width;
                frame->height = image->height;
                frame->color_range = AVCOL_RANGE_JPEG;
                uint8_t* planes = const_cast<uint8_t*>(image->planes.data());
                for (int plane = 0; plane < 4; ++plane) {
                    frame->data[plane] = planes + (image->PlaneSize() * plane);
                    frame->linesize[plane] = image->width;
                }
                frames[slot] = frame;
                displayReferred[slot] = 1;
            }
            topmostLate = frame == nullptr;
            continue;
        }
        int64_t cachedFrame = -1;
        frames[slot] = self.state->frameCache->GetNearest(
            requested.sourceId, requested.frame, cachedFrame);
        // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A media declared
        // a graphic is placed as a motion frame is: in the graphics view,
        // its code values being what it shows (Renderer.mm decodes its
        // YCbCr with its own matrix and range).
        if (self.state->document.IsGraphicsMedia(requested.sourceId))
            displayReferred[slot] = 1;
        candidates[slot] = {true, requested.sourceId, cachedFrame,
                            requested.opacity, requested.clipId};
        // PERF10 -- a source that is not offline has a decoder, or one on
        // its way: its layer is late, not missing.
        const bool decodable =
            self.state->offlineSourceIds.count(requested.sourceId) == 0;
        readiness[slot] = {true, frames[slot] != nullptr, decodable};
        if (!frames[slot] || cachedFrame != requested.frame) {
            missing = true;
            topmostLate = true;
        }
    }
    if (isDisplayDeadline && missing) {
        self.state->performanceMetrics->RecordDrop();
    }
    // FC4 -- the Trim Monitor's two frames often come from one rush, whose
    // decoder holds one target at a time: the first frame not in the cache
    // is asked again, until both are.
    if (!self.state->trimMonitorPanes.empty()) {
        for (size_t slot = 0; slot < self.state->requested.size(); ++slot) {
            const ResolvedSlot& requested = self.state->requested[slot];
            if (requested.active && candidates[slot].frame != requested.frame) {
                [self requestFrame:requested.frame ofSource:requested.sourceId];
                break;
            }
        }
    }
    if (AppTestHooks* hooks = TestHooks(); hooks && isDisplayDeadline)
        hooks->DisplayDeadline(topmostLate);
    // PERF-2026-09. A layer still waiting on its decoder must not composite:
    // Renderer.mm skips a null frame, so drawing now would punch a hole where
    // the edit says an image is and flash the track underneath. Keep the
    // previous image until the decode lands -- see PlaybackPresentation.h.
    const playback::HoldDecision holdProgram = playback::DecideProgramHold(
        readiness, self.state->programPresented, self.state->programHeldTicks);
    self.state->programHeldTicks = holdProgram.held_ticks;
    const bool programChanged = candidates != self.state->rendered;
    // ON5 -- Media detaches Record and the timeline. Source keeps rendering,
    // and a clean feed, when active, remains a program presentation surface.
    const bool recordVisible =
        self.programMonitorView.window != nil &&
        !self.programMonitorView.hiddenOrHasHiddenAncestor;
    const bool timelineVisible = self.metalView.window == self.window &&
                                 !self.metalView.hiddenOrHasHiddenAncestor;
    const bool programVisible =
        recordVisible || self.state->videoOutputRenderer;
    // FC4 -- the Trim Monitor shows two frames of the cut, not the
    // program's captions.
    const auto activeSubtitles =
        self.state->trimMonitorPanes.empty()
            ? ActiveSubtitles(self.state->document,
                              self.state->requestedPosition)
            : std::vector<const DocumentClip*>{};
    std::vector<CaptionCarton> cartons;
    NSMutableArray<NSString*>* subtitleLines = [NSMutableArray array];
    for (const DocumentClip* subtitle : activeSubtitles) {
        // D7 -- a carton being typed in the Inspector shows what is typed.
        CaptionCarton carton;
        carton.text = ui::media_panel::DisplayedCartonText(
            *subtitle, self.state->cartonPreview);
        if (carton.text.empty()) continue;
        if (const CaptionStyle* stored = self.state->document.FindCaptionStyle(
                subtitle->caption_group_id))
            carton.style = *stored;
        carton.opacity = subtitle->opacity;
        // PA4 -- docs/roadmap/PARITE-2026-11.md. The words started at the
        // playhead, as the export counts them at a frame's start. A carton
        // being typed is drawn whole: its times are those of the text it
        // had.
        if (!carton.style.word_animation.empty() &&
            !subtitle->caption_word_times.empty() &&
            carton.text == subtitle->caption_text)
            carton.words_spoken =
                CaptionWordsSpoken(*subtitle, self.state->requestedPosition);
        [subtitleLines addObject:CMStringFromUtf8(carton.text)];
        cartons.push_back(std::move(carton));
    }
    [self showProgramCaptions:cartons
                         text:[subtitleLines componentsJoinedByString:@"\n"]];
    // CF1 -- a gap can keep the same decoded frames while captions change.
    // They advance independently, except when the compositor holds a frame.
    if (self.state->videoOutputRenderer && !holdProgram.hold)
        [self showCaptions:cartons
                      text:[subtitleLines componentsJoinedByString:@"\n"]
                      view:self.videoOutputCaptionView
                   monitor:self.videoOutputView
                      zoom:0.0];
    if ((self.state->overlayDirty && (programVisible || timelineVisible)) ||
        (programChanged && programVisible) ||
        self.state->grabReferenceRequested ||
        (self.state->videoOutputNeedsPresent &&
         self.state->videoOutputRenderer)) {
        [self refreshSequenceLookLut];
        TimelineRenderData programData;
        programData.color_management = self.state->document.color_management;
        programData.display_sdr_preview = true;
        programData.sequence_width = self.state->document.sequence.width;
        programData.sequence_height = self.state->document.sequence.height;
        programData.video_height = self.programMonitorView.bounds.size.height;
        programData.video_zoom = self.state->programMonitorZoom;
        // DEC42 -- Record's zoom frames the sequence, as Resolve's viewer.
        programData.video_zoom_frames_sequence = true;
        programData.video_scope = self.state->programVideoScope;
        programData.video_scope_bottom_inset = [self programScopeBottomInset];
        programData.video_rotation_degrees.resize(candidates.size(), 0);
        programData.video_opacities.resize(candidates.size(), 1.0f);
        // Record is the canonical edited output: both monitors share the same
        // SDR presentation transform, while Record additionally displays the
        // clip's creative grade. Source remains the ungraded rush reference.
        programData.video_color_grades.resize(candidates.size());
        programData.video_display_referred = displayReferred;
        // G3 -- docs/roadmap/GRADE-2026-09.md. Each frame read with its
        // media's own input, as the export reads it.
        programData.video_color_inputs.resize(candidates.size());
        // R1 -- docs/roadmap/REEL-2026-09.md. One entry per composited
        // frame, resolved by ClipFraming.h and never here: Record shows the
        // clip at exactly the rectangle the export writes. A clip that
        // frames nothing leaves its entry empty and is fitted as before.
        programData.video_framings.resize(candidates.size());
        // R5 -- docs/roadmap/REEL-2026-09.md. One entry per composited
        // frame as well: the colour dip over the track that frame came
        // from, at the exact strength TransitionRender.h computes, which
        // is the same number the export hands FFmpeg. A track that dips
        // nowhere leaves its entry at zero and is drawn as before.
        programData.video_veils.resize(candidates.size());
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. One entry per composited
        // frame as well: its clip's blur, the sigma in sequence pixels the
        // export carries into the delivery's (ClipBlur.h). A sharp clip
        // leaves it at zero and is drawn as before.
        programData.video_blurs.resize(candidates.size(), 0.0f);
        // PA8 -- docs/roadmap/PARITE-2026-11.md. And its clip's mask.
        programData.video_masks.resize(candidates.size(), MonitorMask{});
        // PA15 -- and, for a clip cut out along its people, the matte of
        // the source frame shown.
        programData.video_mattes.resize(candidates.size(), MonitorMatte{});
        self.state->matteFrames.resize(candidates.size());
        // R6 -- docs/roadmap/REEL-2026-09.md. The look of the whole
        // sequence, read from the package and checked against its digest
        // by LoadSequenceLookLut: Record shows the film with the look the
        // export writes. Source stays the ungraded rush, as it stays
        // ungraded by the clip's own color.* stack.
        // G6 -- without the grade, the look goes too: it is the sequence's
        // grade.
        const bool bypassGrade = self.state->monitorCompare.bypass_grade;
        if (!bypassGrade) {
            programData.look_lut = self.state->lookLut.samples;
            programData.look_lut_digest = self.state->lookLut.digest;
            programData.look_lut_stage = self.state->lookLut.stage;
        }
        for (size_t slot = 0; slot < candidates.size(); ++slot) {
            if (!candidates[slot].active) continue;
            programData.video_opacities[slot] = candidates[slot].opacity;
            if (const DocumentTrack* veiledTrack =
                    self.state->document.FindTrackForClip(
                        candidates[slot].clipId)) {
                const std::optional<ResolvedColorVeil> veil =
                    self.state->timeline->ResolveTrackColorVeil(
                        veiledTrack->id, self.state->requestedPosition);
                if (veil) {
                    programData.video_veils[slot] = {
                        static_cast<float>(veil->color.red) / 255.0f,
                        static_cast<float>(veil->color.green) / 255.0f,
                        static_cast<float>(veil->color.blue) / 255.0f,
                        TransitionAlphaFloat(veil->alpha)};
                }
            }
            if (const DocumentClip* clip =
                    self.state->document.FindClip(candidates[slot].clipId)) {
                if (const LibraryMedia* media =
                        self.state->document.FindLibraryMedia(clip->source_id))
                    programData.video_color_inputs[slot] = media->color_input;
                if (ClipHasBlur(*clip) && clip->blur.den > 0)
                    programData.video_blurs[slot] =
                        static_cast<float>(clip->blur.num) /
                        static_cast<float>(clip->blur.den);
                programData.video_masks[slot] = MonitorMaskOf(clip->mask);
                // PA15 -- the matte of the frame this slot shows, read from
                // the package as the export reads it. A missing one leaves
                // the slot without a matte, and the clip shows nothing.
                if (ClipHasPersonMask(*clip) && candidates[slot].frame >= 0 &&
                    !motionDirectory.empty()) {
                    programData.video_mattes[slot] =
                        [self personMatteForMedia:clip->source_id
                                            frame:candidates[slot].frame
                                             slot:slot
                                          package:motionDirectory];
                }
                if (!bypassGrade)
                    programData.video_color_grades[slot] = ResolveColorGrade(
                        ui::inspector::DisplayedEffects(
                            *clip, self.state->gradePreview),
                        ColorGradeContextFor(self.state->document, *clip));
                // R2 -- docs/roadmap/REEL-2026-09.md. The values THIS
                // frame shows, sampled by ClipAnimation.h -- the same
                // function the export, the sheets and verify_export call,
                // so Record shows the zoom the file will hold. A clip that
                // animates nothing keeps its constants, and its pixels.
                // R2c -- the opacity is the slot's already: Timeline samples
                // it (ResolveTrackLayers), the key replacing the constant as
                // in the export. Multiplying it here again showed a keyed
                // or framing-keyed translucent clip darker than delivered.
                // While an Inspector drag moves a key, the clip is sampled
                // with the keys it would commit.
                const std::optional<DocumentClip> previewed =
                    ui::inspector::PreviewedClip(*clip,
                                                 self.state->framingPreview);
                const DocumentClip& shown = previewed ? *previewed : *clip;
                ClipSampledParameters sampled;
                sampled.opacity = shown.opacity;
                sampled.framing = shown.framing;
                if (ClipHasAnimation(shown)) {
                    std::string animationError;
                    const RationalTime offset =
                        self.state->requestedPosition.sub(shown.timeline_in);
                    const MediaRate rate =
                        self.state->document.sequence.frame_rate;
                    SampleClipParameters(shown,
                                         offset.to_frames(rate.num, rate.den),
                                         rate, sampled, animationError);
                }
                if (ClipHasFraming(shown) || ClipHasAnimation(shown)) {
                    ClipFramingGeometry geometry;
                    int64_t displayedWidth = 0;
                    int64_t displayedHeight = 0;
                    std::string framingError;
                    if (ClipDisplayedSize(self.state->document, shown,
                                          displayedWidth, displayedHeight,
                                          framingError) &&
                        ResolveClipFraming(sampled.framing, displayedWidth,
                                           displayedHeight,
                                           self.state->document.sequence.width,
                                           self.state->document.sequence.height,
                                           geometry, framingError)) {
                        programData.video_framings[slot] =
                            ClipHasAnimation(shown)
                                ? MonitorFramingOfWholePicture(
                                      geometry, displayedWidth, displayedHeight)
                                : MonitorFramingOf(geometry, displayedWidth,
                                                   displayedHeight);
                    }
                }
            }
            const auto media =
                self.state->mediaMetadata.find(candidates[slot].sourceId);
            if (media != self.state->mediaMetadata.end() &&
                media->second.metadata_complete)
                programData.video_rotation_degrees[slot] =
                    media->second.rotation_degrees;
        }
        // FC4 -- the Trim Monitor: each frame whole in its pane, opaque,
        // under no dip, and the counters over it.
        if (!self.state->trimMonitorPanes.empty() &&
            self.state->trimMonitorPanes.size() == candidates.size())
            [self layOutTrimMonitor:programData];
        // CF1 -- the same decoded composite on the chosen screen, fitted
        // independently of Record's zoom, without scopes or trim labels.
        // Captions use that screen's own pixel scale and stay with the
        // picture when a late decoder makes both monitors hold.
        if (self.state->videoOutputRenderer && !holdProgram.hold) {
            const TimelineRenderData feed = CleanFeedRenderData(
                programData, self.videoOutputView.bounds.size.height);
            const bool presented =
                self.state->videoOutputRenderer->RenderFrames(frames, feed);
            self.state->videoOutputNeedsPresent = !presented;
            if (presented && !recordVisible) {
                self.state->rendered = candidates;
                self.state->programPresented = true;
            }
        }
        // Building the display list while holding costs a few vectors and is
        // thrown away; what must not happen is the present itself, which is
        // where the hole would reach the screen.
        // G6 -- docs/roadmap/GRADE-2026-09.md. A grab takes Record as it is
        // about to be shown, at the sequence's size and without the scopes
        // or the reference; then the reference is laid over its half.
        if (self.state->grabReferenceRequested && !holdProgram.hold) {
            self.state->grabReferenceRequested = false;
            TimelineRenderData grab = programData;
            const int32_t width = self.state->document.sequence.width;
            const int32_t height = self.state->document.sequence.height;
            grab.video_height = height;
            grab.video_zoom = 0.0;
            grab.video_scope = VideoScopeMode::Off;
            grab.reference_still = nullptr;
            std::vector<float> pixels;
            if (width <= 0 || height <= 0 ||
                !self.state->programRenderer->RenderFramesToPixels(
                    frames, grab, static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height), pixels) ||
                ![self keepReferenceStill:ReferenceStillFromRgba(pixels, width,
                                                                 height)])
                NSBeep();
        }
        if (self.state->monitorCompare.split) {
            programData.reference_still = self.state->referenceStill;
            programData.reference_position =
                self.state->monitorCompare.split_position;
        }
        programData.curve_histogram =
            recordVisible && [self.inspectorView wantsCurveHistogram] &&
            self.state->trimMonitorPanes.empty();
        if (recordVisible && !holdProgram.hold &&
            self.state->programRenderer->RenderFrames(frames, programData)) {
            self.state->rendered = candidates;
            self.state->programPresented = true;
            [self.inspectorView showCurveHistogram:self.state->programRenderer
                                                       ->CurveHistogram()];
        }
        if (timelineVisible) {
            // PERF7 -- ROADMAP.md. Measured, not changed: the list is rebuilt
            // whenever the overlay is dirty or the Record composite moved,
            // which in playback is every tick. "Submit" is Renderer.mm's whole
            // call -- one draw call per quad, the wait for a drawable, then for
            // the GPU
            // -- i.e. how long the main thread is held, not only encoding.
            const bool timelineSignposts = os_signpost_enabled(renderLog);
            const bool measureTimeline = renderCost || timelineSignposts;
            const int64_t buildStart =
                measureTimeline ? render_cost::NowNanoseconds() : 0;
            os_signpost_interval_begin(renderLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                       "Timeline display list");
            TimelineRenderData timelineData = [self timelineRenderData];
            const int64_t buildEnd =
                measureTimeline ? render_cost::NowNanoseconds() : 0;
            render_cost::DisplayListCounts timelineCounts;
            if (measureTimeline) {
                for (const MetalDrawCommand& command : timelineData.overlays) {
                    if (std::holds_alternative<MetalRect>(command)) {
                        ++timelineCounts.rects;
                    } else if (const MetalText* text =
                                   std::get_if<MetalText>(&command)) {
                        ++timelineCounts.texts;
                        timelineCounts.text_bytes += text->text.size();
                    } else {
                        ++timelineCounts.icons;
                    }
                }
            }
            os_signpost_interval_end(
                renderLog, OS_SIGNPOST_ID_EXCLUSIVE, "Timeline display list",
                "%llu rects, %llu texts, %llu icons",
                static_cast<unsigned long long>(timelineCounts.rects),
                static_cast<unsigned long long>(timelineCounts.texts),
                static_cast<unsigned long long>(timelineCounts.icons));
            const int64_t submitStart =
                measureTimeline ? render_cost::NowNanoseconds() : 0;
            os_signpost_interval_begin(renderLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                       "Timeline submit");
            const std::vector<AVFrame*> noFrames;
            self.state->renderer->RenderFrames(noFrames, timelineData);
            os_signpost_interval_end(renderLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                     "Timeline submit");
            if (renderCost) {
                renderCost->Record({buildEnd - buildStart,
                                    render_cost::NowNanoseconds() - submitStart,
                                    timelineCounts});
            }
        }
    }

    if (self.state->sourceMonitorVisible && self.state->sourceMonitor &&
        !self.state->sourceMonitorId.empty()) {
        const int64_t requestedFrame =
            self.state->sourceMonitorPosition.to_frames(
                self.state->sourceMonitorPosition.rate);
        int64_t cachedFrame = -1;
        AVFrame* sourceFrame = self.state->frameCache->GetNearest(
            self.state->sourceMonitorId, requestedFrame, cachedFrame);
        const RenderedSlot sourceCandidate{true, self.state->sourceMonitorId,
                                           cachedFrame};
        if (self.state->overlayDirty ||
            !(sourceCandidate == self.state->sourceRendered)) {
            TimelineRenderData sourceData;
            sourceData.color_management = self.state->document.color_management;
            sourceData.display_sdr_preview = true;
            // Both viewers use the sequence canvas. Portrait media is then
            // letterboxed at the same scale in Source and Record.
            sourceData.sequence_width = self.state->document.sequence.width;
            sourceData.sequence_height = self.state->document.sequence.height;
            sourceData.video_height = self.sourceMonitorView.bounds.size.height;
            sourceData.video_zoom = self.state->sourceMonitorZoom;
            sourceData.video_rotation_degrees = {0};
            // G3 -- the rush read with its own declared input. DEC31 -- a
            // graphic is shown as Record places it.
            if (const LibraryMedia* media =
                    self.state->document.FindLibraryMedia(
                        self.state->sourceMonitorId)) {
                sourceData.video_color_inputs = {media->color_input};
                if (media->color_input.graphics)
                    sourceData.video_display_referred = {1};
            }
            const auto metadata =
                self.state->mediaMetadata.find(self.state->sourceMonitorId);
            if (metadata != self.state->mediaMetadata.end()) {
                if (metadata->second.metadata_complete) {
                    sourceData.video_rotation_degrees[0] =
                        metadata->second.rotation_degrees;
                }
            }
            // Out of lot -- the author, 29 October 2026: the source player's
            // indicator. The whole width is the whole rush, as the click that
            // scrubs it reads it; In/Out on it, the playhead across it
            // (LayoutSourceBar, TransportBar.h).
            if (const DocumentSource* shown = self.state->document.FindSource(
                    self.state->sourceMonitorId)) {
                try {
                    const SourceBarGeometry bar = LayoutSourceBar(
                        self.sourceMonitorView.bounds.size.width,
                        self.sourceMonitorView.bounds.size.height,
                        ScrubBarRange{shown->duration},
                        self.state->sourceMonitorPosition, self.state->sourceIn,
                        self.state->sourceOut);
                    const ui::theme::Color track =
                        ui::theme::WithAlpha(ui::theme::kSurfaceSunken, 0.85f);
                    const ui::theme::Color zone =
                        ui::theme::WithAlpha(ui::theme::kAccent, 0.45f);
                    sourceData.overlays.emplace_back(
                        MetalRect{bar.track_x, bar.track_y, bar.track_width,
                                  bar.track_height, track, track});
                    if (bar.zone_width > 0.0)
                        sourceData.overlays.emplace_back(
                            MetalRect{bar.zone_x, bar.track_y, bar.zone_width,
                                      bar.track_height, zone, zone});
                    sourceData.overlays.emplace_back(
                        MetalRect{bar.playhead_x, bar.playhead_y,
                                  bar.playhead_width, bar.playhead_height,
                                  ui::theme::kAccent, ui::theme::kAccent});
                } catch (const std::exception&) {
                }
            }
            const std::vector<AVFrame*> sourceFrames{sourceFrame};
            if (self.state->sourceRenderer->RenderFrames(sourceFrames,
                                                         sourceData))
                self.state->sourceRendered = sourceCandidate;
        }
        av_frame_free(&sourceFrame);
    } else if (self.state->sourceMonitorVisible && self.state->overlayDirty) {
        TimelineRenderData sourceData;
        sourceData.video_height = self.sourceMonitorView.bounds.size.height;
        const std::vector<AVFrame*> noFrames;
        self.state->sourceRenderer->RenderFrames(noFrames, sourceData);
    }
    self.state->overlayDirty = false;
    for (AVFrame*& frame : frames) av_frame_free(&frame);
}

@end
