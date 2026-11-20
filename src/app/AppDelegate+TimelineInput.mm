// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The timeline
// under the hand (TimelineEventTarget): the mouse, the scroll and the
// pinch, the keys, the drops of media and effects, the context menu, and
// the tool and its cursor. TimelineView.h says what a point hits; these
// methods turn each gesture into an edit or a move of the playhead. Moved
// out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (TimelineInput)

- (NSPoint)timelinePointForEvent:(NSEvent*)event {
    const NSPoint point = [self.metalView convertPoint:event.locationInWindow
                                              fromView:nil];
    return NSMakePoint(point.x, self.metalView.bounds.size.height - point.y -
                                    [self videoHeight]);
}

- (TimelineTool)effectiveTool {
    return self.state->spaceHand ? TimelineTool::Hand : self.state->tool;
}

// FC2 -- docs/roadmap/FILMCRAFT-2026-10.md. What is under the pointer, for
// the hover and the click alike (TimelineHitAt, TimelineView.h).
- (TimelinePointerTarget)timelineTargetAt:(NSPoint)point event:(NSEvent*)event {
    TimelinePointerTool tool = TimelinePointerTool::Select;
    switch ([self effectiveTool]) {
        case TimelineTool::Select:
            tool = TimelinePointerTool::Select;
            break;
        case TimelineTool::Hand:
            tool = TimelinePointerTool::Hand;
            break;
        case TimelineTool::Zoom:
            tool = TimelinePointerTool::Zoom;
            break;
        case TimelineTool::Cut:
            tool = TimelinePointerTool::Cut;
            break;
        case TimelineTool::Trim:
            tool = TimelinePointerTool::Trim;
            break;
    }
    const NSEventModifierFlags flags = event.modifierFlags;
    const TimelinePointerModifiers modifiers{
        (flags & NSEventModifierFlagShift) != 0,
        (flags & NSEventModifierFlagOption) != 0,
        (flags & NSEventModifierFlagCommand) != 0};
    return TimelineHitAt(self.state->document, self.state->viewport, point.x,
                         point.y, self.metalView.bounds.size.width,
                         [self timelineHeight], self.state->duration, tool,
                         modifiers, self.state->retimeClipIds);
}

- (void)applyToolCursor {
    switch ([self effectiveTool]) {
        case TimelineTool::Select:
            [NSCursor.arrowCursor set];
            break;
        case TimelineTool::Hand:
            [(self.state->navigationDragging ? NSCursor.closedHandCursor
                                             : NSCursor.openHandCursor) set];
            break;
        case TimelineTool::Zoom:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Cut:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Trim:
            [NSCursor.resizeLeftRightCursor set];
            break;
    }
}

- (void)setTimelineTool:(TimelineTool)tool {
    self.state->tool = tool;
    for (NSInteger index = 0; index < self.timelineToolIcons.count; ++index) {
        self.timelineToolIcons[index].contentTintColor =
            index == static_cast<NSInteger>(tool) ? CMAccentColor()
                                                  : NSColor.secondaryLabelColor;
    }
    self.state->interaction->CancelDrag();
    self.state->navigationDragging = false;
    self.state->scrubDragging = false;
    self.state->editDragging = false;
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    // FC4 -- the trim mode's style follows the tool: Selection resizes,
    // Trim ripples. A dynamic trim keeps the style it started with.
    if (!self.state->trimSession.Dynamic().active) [self syncTrimStyle];
    [self applyToolCursor];
    [self refreshTrimBar];
    [self updateSelectionInfo];
    if (self.state->trimSession.Active()) [self showTrimStatus];
    self.state->overlayDirty = true;
}

// Moves the playhead by `amount` frames (or samples, in Sample resolution)
// from its current position. Shared by the Left/Right-arrow keyboard
// shortcut and the Transport panel's frame-step buttons (F2.5) so both
// paths compute the same delta and land on the same
// -requestTimelinePosition: -> QuantizePlayheadPosition machinery every
// other playhead move already uses.
- (void)zoomTimelineAtPlayheadOut:(BOOL)zoomOut {
    self.state->zoomAnchor = ZoomTimelineAtPlayhead(
        self.state->viewport, self.state->requestedPosition,
        zoomOut ? (1.0 / 1.5) : 1.5, self.state->duration,
        self.metalView.bounds.size.width, self.state->zoomAnchor);
    self.state->overlayDirty = true;
}

// Shift+Left/Right, as in Resolve (21.1 manual, p. 870): one timecode second
// of frames, or 48,000 samples when the playhead is on the sample grid.
- (void)stepPlayheadSeconds:(int64_t)direction {
    const int64_t amount =
        self.state->playheadResolution == PlayheadResolution::Sample
            ? 48000
            : NominalTimecodeFps([self playheadFrameRate]);
    [self stepPlayheadFrames:direction * amount];
}

- (void)stepPlayheadFrames:(int64_t)amount {
    const RationalTime delta =
        self.state->playheadResolution == PlayheadResolution::Sample
            ? RationalTime{amount, 48000}
            : RationalTime{amount * [self playheadFrameRate].den,
                           [self playheadFrameRate].num};
    [self requestTimelinePosition:self.state->requestedPosition.add(delta)];
}

- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point {
    return [self timelineDropMediaIds:mediaId ? @[ mediaId ] : @[]
                          atViewPoint:point];
}

- (NSDragOperation)timelineDragEffectId:(NSString*)effectId
                            atViewPoint:(NSPoint)point {
    const ui::effects::DropTarget target = [self effectDropTargetForId:effectId
                                                           atViewPoint:point];
    self.state->effectDropTarget = target;
    self.state->overlayDirty = true;
    if (!target.valid) {
        self.infoLabel.stringValue = CMStringFromUtf8(target.refusal);
        return NSDragOperationNone;
    }
    return NSDragOperationCopy;
}

- (void)timelineEffectDragEnded {
    if (!self.state || !self.state->effectDropTarget) return;
    self.state->effectDropTarget.reset();
    self.state->overlayDirty = true;
}

- (BOOL)timelineDropEffectId:(NSString*)effectId atViewPoint:(NSPoint)point {
    const ui::effects::DropTarget target = [self effectDropTargetForId:effectId
                                                           atViewPoint:point];
    [self timelineEffectDragEnded];
    if (!target.valid) {
        self.infoLabel.stringValue = CMStringFromUtf8(target.refusal);
        return NO;
    }
    return [self applyEffectWithId:effectId toTarget:target];
}

- (BOOL)timelineDropMediaIds:(NSArray<NSString*>*)mediaIds
                 atViewPoint:(NSPoint)point {
    if (mediaIds.count == 0) return NO;
    const double timelineY =
        self.metalView.bounds.size.height - point.y - [self videoHeight];
    if (point.x < self.state->viewport.header_width ||
        timelineY < kTimelineRulerHeight)
        return NO;
    // Out of lot -- the author, 29 October 2026. Resolve's drop (manual 21.1,
    // p. 891-893): an overwrite, Cmd+Shift an insert, Option the picture
    // alone, Shift the sound alone. It was always an insert, which opened
    // every sync-locked track: a cutaway dropped over an interview pushed
    // the interview along.
    const NSEventModifierFlags modifiers = NSEvent.modifierFlags;
    const bool insert = (modifiers & NSEventModifierFlagCommand) != 0 &&
                        (modifiers & NSEventModifierFlagShift) != 0;
    const bool pictureOnly = (modifiers & NSEventModifierFlagOption) != 0;
    const bool soundOnly =
        !insert && (modifiers & NSEventModifierFlagShift) != 0;
    // What each media brings: its picture unless it is known to have none,
    // its sound when it is known to have one.
    const auto partsOf = [&](const Ulid& sourceId) {
        const auto detected = self.state->mediaMetadata.find(sourceId);
        const LibraryMedia* media =
            detected == self.state->mediaMetadata.end()
                ? self.state->document.FindLibraryMedia(sourceId)
                : &detected->second;
        const bool known = media && media->metadata_complete;
        return std::make_pair(!(known && !media->has_video) && !soundOnly,
                              known && media->has_audio && !pictureOnly);
    };
    bool anyPicture = false;
    bool anySound = false;
    for (NSString* mediaId in mediaIds) {
        const auto [picture, sound] = partsOf(Ulid(mediaId.UTF8String ?: ""));
        anyPicture = anyPicture || picture;
        anySound = anySound || sound;
    }
    std::string refusal;
    const std::optional<TimelineDropTracks> targets =
        TimelineDropTracksAt(self.state->document, self.state->viewport,
                             timelineY, anyPicture, anySound, refusal);
    if (!targets) {
        if (!refusal.empty())
            self.binSummaryLabel.stringValue = CMStringFromUtf8(refusal);
        return NO;
    }
    const Ulid firstSourceId(mediaIds.firstObject.UTF8String ?: "");
    const DocumentSource* firstSource =
        self.state->document.FindSource(firstSourceId);
    if (!firstSource) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré avant son montage.";
        return NO;
    }
    RationalTime timelineIn;
    try {
        timelineIn =
            self.state->viewport.XToTime(point.x, firstSource->duration.rate);
        if (timelineIn.value < 0) timelineIn = {0, timelineIn.rate};
    } catch (const std::exception& exception) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Drop refusé : %@",
                                       CMStringFromUtf8(exception.what())];
        return NO;
    }
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    // The tracks the drop creates and its edits are one step: one Cmd+Z
    // takes the whole drop back, its new tracks with it.
    const InterventionMark mark =
        MarkIntervention(self.state->activeTimelineId, stagedLog);
    Ulid videoTrackId = targets->video_track_id;
    Ulid audioTrackId = targets->audio_track_id;
    std::vector<Ulid> createdTrackIds;
    const auto createTrack = [&](const std::string& kind, Ulid& trackId) {
        int32_t index = 0;
        for (const DocumentTrack& track : stagedDocument.sequence.tracks)
            index = std::max(index, track.index + 1);
        trackId = GenerateUlid();
        if (!stagedLog.Apply(stagedDocument,
                             Operation{AddTrackOperation{trackId, kind, index}},
                             error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Création de la piste refusée (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return false;
        }
        createdTrackIds.push_back(trackId);
        return true;
    };
    if (targets->create_video_track && !createTrack("video", videoTrackId))
        return NO;
    if (targets->create_audio_track && !createTrack("audio", audioTrackId))
        return NO;
    std::vector<Ulid> primaryClipIds;
    std::vector<Ulid> linkedClipIds;
    for (NSString* mediaId in mediaIds) {
        const Ulid sourceId(mediaId.UTF8String ?: "");
        const DocumentSource* source = stagedDocument.FindSource(sourceId);
        if (!source) {
            self.binSummaryLabel.stringValue =
                @"Un des médias doit être réingéré avant son montage.";
            return NO;
        }
        // EditLog::Apply replaces the staged Document with a validated copy.
        // Keep exact values, never pointers into it, across each application.
        const RationalTime sourceDuration = source->duration;
        const int32_t sourceRate = sourceDuration.rate;
        const auto [hasPicture, hasSound] = partsOf(sourceId);
        const bool picture = hasPicture && !videoTrackId.empty();
        const bool sound = hasSound && !audioTrackId.empty();
        // A still dropped with Shift, a sound with Option: nothing of this
        // one to edit, and no room left for it.
        if (!picture && !sound) continue;
        std::optional<DeleteGapOperation> sourceEdit =
            TimelineSourceEditOperation(
                stagedDocument, sourceId, {0, sourceRate}, sourceDuration,
                timelineIn, picture ? videoTrackId : audioTrackId, insert,
                picture && sound ? std::vector<Ulid>{audioTrackId}
                                 : std::vector<Ulid>{});
        if (!sourceEdit ||
            !stagedLog.Apply(stagedDocument, Operation{std::move(*sourceEdit)},
                             error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Montage refusé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
        // A plain selection takes the picture, or the sound edited alone.
        const Ulid primaryTrackId = picture ? videoTrackId : audioTrackId;
        for (const Ulid& trackId :
             {picture ? videoTrackId : Ulid{}, sound ? audioTrackId : Ulid{}}) {
            const DocumentTrack* track = stagedDocument.FindTrack(trackId);
            if (!track) continue;
            for (const DocumentClip& clip : track->clips) {
                if (clip.source_id == sourceId &&
                    clip.timeline_in == timelineIn &&
                    clip.source_in == RationalTime{0, sourceRate} &&
                    clip.duration == sourceDuration) {
                    if (trackId == primaryTrackId)
                        primaryClipIds.push_back(clip.id);
                    linkedClipIds.push_back(clip.id);
                    break;
                }
            }
        }
        timelineIn = timelineIn.add(sourceDuration);
    }
    if (linkedClipIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"Rien à monter : ces médias n'ont pas cette partie.";
        return NO;
    }
    if (stagedLog.AppliedCount() > mark.history_depth + 1) {
        size_t grouped = 0;
        if (!stagedLog.GroupSince(stagedDocument, mark, "Médias déposés",
                                  grouped, error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Montage refusé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
    }

    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    // A track the drop created is targeted, as one added by hand
    // (-addTrack:).
    for (const Ulid& trackId : createdTrackIds)
        self.state->targetedTrackIds.insert(trackId);
    if (!createdTrackIds.empty())
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
    self.state->interaction->SelectClips(
        self.state->linkedSelection ? linkedClipIds : primaryClipIds);
    const RationalTime playhead = self.state->requestedPosition;
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
    return YES;
}

- (void)timelineMouseDown:(NSEvent*)event {
    // Full screen is for watching. The whole screen is Record there, so the
    // mouse was always over some node of a motion clip and its outline was
    // drawn across the picture; and a click, which no monitor claimed, was
    // read in the timeline's coordinates and acted on clips nobody could
    // see. The mouse does nothing until the window is back.
    if (self.videoFullscreenActive) return;
    if (self.window.firstResponder == self.sourceMonitorView) {
        self.state->sourceMonitorActive = true;
        [self scrubSourceMonitorEvent:event];
        return;
    }
    if (self.window.firstResponder == self.programMonitorView) {
        self.state->sourceMonitorActive = false;
        // ON9 -- in trim mode Record is the Trim Monitor: a press trims.
        if ([self trimMonitorPressAt:[self.programMonitorView
                                         convertPoint:event.locationInWindow
                                             fromView:nil]])
            return;
        // REM5 -- a click on Record selects in the motion clip it shows.
        [self motionCanvasMouseDown:event];
        return;
    }
    self.state->sourceMonitorActive = false;
    self.state->duplicateDragging = false;
    self.state->duplicateDragClipboard.clear();
    self.state->zoomBarDrag.reset();
    self.state->fadeDrag.reset();
    const NSPoint point = [self timelinePointForEvent:event];
    const double timelineHeight = [self timelineHeight];
    if (point.y < 0.0 || point.y > timelineHeight) return;
    [self dismissAgentMarks];  // PRD25
    const TimelinePointerTarget target = [self timelineTargetAt:point
                                                          event:event];
    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. What the press took: a click
    // on an edge that moves nothing selects its point at the release. Any
    // other press in the tracks leaves trim mode, as a click elsewhere drops
    // Resolve's edit selection; Shift on an edge adds to it instead.
    self.state->pressTarget = target;
    self.state->pressShift =
        (event.modifierFlags & NSEventModifierFlagShift) != 0;
    if (self.state->trimSession.Active() && point.y >= kTimelineRulerHeight &&
        (target.kind == TimelinePointerTargetKind::ClipBody ||
         target.kind == TimelinePointerTargetKind::None ||
         target.kind == TimelinePointerTargetKind::FadeHandle ||
         (target.kind == TimelinePointerTargetKind::ClipEdge &&
          !self.state->pressShift)))
        [self leaveTrimMode];
    // The integrated strip shares its pure geometry with Metal rendering, so
    // the visible handles and the draggable hit areas cannot drift apart.
    if (target.kind == TimelinePointerTargetKind::ZoomBar) {
        const TimelineZoomBarGeometry geometry =
            CalculateTimelineZoomBarGeometry(self.state->viewport,
                                             self.state->duration,
                                             self.metalView.bounds.size.width);
        const TimelineZoomBarPart part = target.zoom_bar_part;
        if (part != TimelineZoomBarPart::None)
            self.state->zoomBarDrag = TimelineZoomBarDrag{
                part, point.x, self.state->viewport, geometry};
        if (NavTraceEnabled())
            std::fprintf(stderr,
                         "nav bar-down x=%.1f part=%s thumb=%.1f..%.1f "
                         "rail=%.1f..%.1f ruler=%.3fs pps=%.3f start=%.4fs\n",
                         point.x, NavTracePart(part), geometry.thumb_x,
                         geometry.thumb_x + geometry.thumb_width,
                         geometry.rail_x, geometry.rail_x + geometry.rail_width,
                         geometry.ruler_seconds,
                         self.state->viewport.pixels_per_second,
                         NavTraceStart(self.state->viewport));
        return;
    }
    const double tracksBottom =
        kTimelineRulerHeight + self.state->document.sequence.tracks.size() *
                                   self.state->viewport.track_height;
    if (point.y >= kTimelineRulerHeight && point.y < tracksBottom &&
        point.x >= 40.0 && point.x < 92.0) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t index =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (index < tracks.size()) {
            const DocumentTrack& track = *tracks[index];
            const int control = point.x < 56.0 ? 0 : point.x < 74.0 ? 1 : 2;
            EditError error = EditError::None;
            std::string message;
            const RationalTime playhead = self.state->requestedPosition;
            bool applies = true;
            Operation operation = SetTrackLockOperation{};
            if ((track.kind == "video" || track.kind == "caption") &&
                control == 0) {
                operation = SetTrackOutputOperation{track.id, !track.visible,
                                                    track.muted, track.solo};
            } else if (track.kind == "audio" && control == 0) {
                operation = SetTrackOutputOperation{track.id, track.visible,
                                                    track.muted, !track.solo};
            } else if (track.kind == "audio" && control == 1) {
                operation = SetTrackOutputOperation{track.id, track.visible,
                                                    !track.muted, track.solo};
            } else if (((track.kind == "video" || track.kind == "caption") &&
                        control == 1) ||
                       (track.kind == "audio" && control == 2)) {
                operation = SetTrackLockOperation{track.id, !track.locked};
            } else {
                applies = false;
            }
            if (applies &&
                [self applyAndPersistTimelineOperation:std::move(operation)
                                                 error:error
                                               message:message]) {
                [self refreshTimelineAfterEditFromPosition:playhead];
                self.state->overlayDirty = true;
            }
        }
        return;
    }
    if (point.y < kTimelineToolbarHeight && point.x >= 0.0) {
        constexpr double toolStart = 4.0;
        constexpr double toolWidth = 24.0;
        if (point.x >= toolStart && point.x < toolStart + toolWidth * 5.0) {
            const int index =
                static_cast<int>((point.x - toolStart) / toolWidth);
            [self setTimelineTool:static_cast<TimelineTool>(index)];
        } else if (point.x >= 128.0 && point.x < 166.0) {
            [self menuToggleSnapping:nil];
        } else if (point.x >= 170.0 && point.x < 216.0) {
            [self menuToggleLinkedSelection:nil];
        } else if (point.x >= 222.0 && point.x < 294.0) {
            const int index = static_cast<int>((point.x - 222.0) / 24.0);
            if (index == 0)
                [self menuPlayReverse:nil];
            else if (index == 1)
                [self menuStop:nil];
            else
                [self menuPlayForward:nil];
        }
        return;
    }
    // D2 -- docs/roadmap/DIALOGUE-2026-10.md. A click on a marker's flag
    // sends the playhead to its exact time, whatever the tool; a double
    // click opens its note. Hit by the geometry the flag is drawn from.
    if (target.kind == TimelinePointerTargetKind::Marker) {
        const std::optional<Ulid> markerId = target.marker_id;
        const DocumentMarker* marker =
            self.state->document.FindMarker(*markerId);
        if (!marker) return;
        // D6 -- with Option held, the marker is taken hold of to be
        // stretched into a span, as in the editor's other program; the
        // playhead stays where it is.
        if (target.marker_stretch) {
            [self setPlaybackDirection:0];
            self.state->markerStretch = AppState::MarkerStretch{
                *markerId,
                TimelineMarkerSpanAnchor(*marker, self.state->viewport,
                                         point.x),
                std::nullopt};
            self.infoLabel.stringValue =
                @"MARQUEUR · glisser pour marquer un passage";
            return;
        }
        [self setPlaybackDirection:0];
        [self requestTimelinePosition:marker->time];
        [self showMarkerInfo:*marker];
        self.state->overlayDirty = true;
        if (event.clickCount >= 2) {
            // After this event: the note is asked in a modal window, which
            // must not open inside the mouse-down it would swallow the
            // mouse-up of.
            const Ulid edited = *markerId;
            __weak AppDelegate* weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
              [weakSelf editMarkerNote:edited];
            });
        }
        return;
    }
    // GitHub #330 -- a fade handle is taken hold of; the selection stays as
    // it is, and the fade follows the pointer until the release, one edit.
    if (target.kind == TimelinePointerTargetKind::FadeHandle) {
        [self setPlaybackDirection:0];
        if (const std::optional<TimelineFadeDrag> press = BeginTimelineFadeDrag(
                self.state->document, target.clip_id, target.edge, point.x)) {
            self.state->fadeDrag = AppState::FadeDrag{*press, std::nullopt};
            [self showFadeReadout:press->start edge:target.edge];
        }
        self.state->overlayDirty = true;
        return;
    }
    const TimelineTool tool = [self effectiveTool];
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    if (tool == TimelineTool::Hand) {
        if (self.state->spaceHand) self.state->spaceUsedForPan = true;
        self.state->navigationDragging = true;
        self.state->navigationLastX = point.x;
        [self applyToolCursor];
        return;
    }
    if (tool == TimelineTool::Zoom) {
        const bool zoomOut =
            (event.modifierFlags & NSEventModifierFlagOption) != 0;
        ZoomTimelineAtPointer(self.state->viewport, point.x,
                              zoomOut ? 0.5 : 2.0, self.state->duration,
                              self.metalView.bounds.size.width, std::nullopt);
        self.state->overlayDirty = true;
        return;
    }
    if (tool == TimelineTool::Cut) {
        [self setPlaybackDirection:0];
        if (target.kind != TimelinePointerTargetKind::ClipBody) return;
        const Ulid hitClipId = target.clip_id;
        const DocumentClip* clip = self.state->document.FindClip(hitClipId);
        if (!clip) return;
        const bool linkedCut =
            self.state->linkedSelection &&
            (event.modifierFlags & NSEventModifierFlagOption) == 0;
        Operation operation = TimelineCutOperationForClip(
            self.state->document, *clip,
            self.state->viewport.XToTime(point.x, clip->duration.rate),
            linkedCut);
        EditError error = EditError::None;
        std::string message;
        const RationalTime playhead = self.state->requestedPosition;
        if ([self applyAndPersistTimelineOperation:std::move(operation)
                                             error:error
                                           message:message]) {
            if (linkedCut)
                self.state->interaction->SelectClips(ExpandLinkedClipSelection(
                    self.state->document, {hitClipId}));
            else
                self.state->interaction->SelectClip(hitClipId);
            [self refreshTimelineAfterEditFromPosition:playhead];
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::InvalidTimelineIn) {
            std::fprintf(stderr, "Cut rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Coupe refusée (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
        }
        return;
    }
    const bool selectionHit =
        target.kind == TimelinePointerTargetKind::ClipBody ||
        target.kind == TimelinePointerTargetKind::ClipEdge;
    // D7 -- docs/roadmap/DIALOGUE-2026-10.md. A double click on a carton
    // puts the keyboard in its text, in the Inspector. Its edges count as
    // the carton: one word long, it is little more than its two edges.
    // Nothing is dragged by this click -- the first one selected the carton.
    if (tool == TimelineTool::Select && event.clickCount >= 2 && selectionHit &&
        ui::media_panel::CartonTextAccess(self.state->document,
                                          target.clip_id) !=
            ui::media_panel::CartonTextOutcome::NotACarton) {
        [self setPlaybackDirection:0];
        const Ulid edited = target.clip_id;
        self.state->interaction->SelectClip(edited);
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        [self editCartonText:edited];
        return;
    }
    // ON9 -- a double click on a cut takes it as a roll and shows the Trim
    // Monitor, as Resolve opens its Trim Editor there (manual, p. 1067);
    // the first click took the side its zone gives.
    if ((tool == TimelineTool::Select || tool == TimelineTool::Trim) &&
        event.clickCount >= 2 &&
        target.kind == TimelinePointerTargetKind::ClipEdge &&
        !TimelineTrimModeStretches(target.trim_mode) &&
        !self.state->trimSession.Dynamic().active) {
        [self setPlaybackDirection:0];
        const TrimEdge edge = target.edge == TimelineHitEdge::Head
                                  ? TrimEdge::Head
                                  : TrimEdge::Tail;
        trim::TrimRefusal refusal = self.state->trimSession.SelectEdge(
            self.state->document, target.clip_id, edge, trim::TrimSide::Roll,
            false);
        if (refusal == trim::TrimRefusal::NoJunction)
            refusal = self.state->trimSession.SelectEdge(
                self.state->document, target.clip_id, edge,
                trim::TrimSideOfEdge(edge, false), false);
        self.state->trimPreview.reset();
        self.state->overlayDirty = true;
        if (refusal != trim::TrimRefusal::None) {
            [self showTrimRefusal:refusal];
            return;
        }
        [self syncTrimStyle];
        [self requestResolvedPosition:self.state->requestedPosition];
        [self showTrimStatus];
        return;
    }
    if (tool == TimelineTool::Select &&
        target.kind == TimelinePointerTargetKind::None &&
        point.x >= self.state->viewport.header_width &&
        !target.track_id.empty()) {
        self.state->lassoCandidate = true;
        self.state->lassoStartX = self.state->lassoCurrentX = point.x;
        self.state->lassoStartY = self.state->lassoCurrentY = point.y;
        self.state->lassoPlayhead = self.state->requestedPosition;
    }
    [self setPlaybackDirection:0];
    const bool optionGesture =
        (event.modifierFlags & NSEventModifierFlagOption) != 0;
    const bool commandGesture =
        (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    const bool duplicateGesture = target.duplicate;
    self.state->linkedSelectionGesture =
        self.state->linkedSelection && (!optionGesture || duplicateGesture);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelectionGesture);
    self.state->interaction->SetPlayheadSnapTarget(
        commandGesture
            ? std::optional<RationalTime>(self.state->requestedPosition)
            : std::nullopt);
    self.state->interaction->SetPlayhead(self.state->requestedPosition);
    // FC1 -- docs/roadmap/FILMCRAFT-2026-10.md. The timeline's In and Out
    // hold the magnet too; the window keeps them, never the document.
    self.state->interaction->SetInOutSnapTargets(self.state->timelineIn,
                                                 self.state->timelineOut);
    self.state->interaction->SetTrimMode(target.trim_mode);
    // Shift on a clip's body extends the selection, as it does in the media
    // panel; on an edge it stays the ripple trim (TimelineView.h).
    self.state->interaction->SetSelectionToggle(target.selection_toggle);
    // DEC40 -- Command on a body extends it, as in Resolve; the magnet
    // still takes the playhead for the drag that follows.
    self.state->interaction->SetSelectionExtend(target.selection_extend);
    self.state->interaction->PointerDown(point.x, point.y,
                                         self.metalView.bounds.size.width,
                                         [self playheadInputRate]);
    // PointerDown owns linked-selection expansion because it also knows which
    // linked members are protected by a locked track. Re-expanding here would
    // undo that safety decision before the first drag event.
    self.state->editDragging = self.state->interaction->HasActiveDrag();
    self.state->duplicateDragging =
        duplicateGesture && self.state->editDragging;
    if (self.state->duplicateDragging)
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    self.state->scrubDragging =
        self.state->interaction->RequestedPlayhead().has_value();
    if (self.state->interaction->RequestedPlayhead()) {
        RationalTime requested = *self.state->interaction->RequestedPlayhead();
        if (commandGesture) {
            const auto snapped = SnapTimelineTimeToEdit(
                self.state->document, self.state->viewport, requested);
            if (snapped) requested = *snapped;
        }
        [self requestTimelinePosition:requested];
        self.state->interaction->ClearRequestedPlayhead();
    }
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDragged:(NSEvent*)event {
    if (self.videoFullscreenActive) return;
    if (self.trimMonitorDragging) {
        [self trimMonitorDragTo:[self.programMonitorView
                                    convertPoint:event.locationInWindow
                                        fromView:nil]];
        return;
    }
    // REM5 -- a gesture begun on Record follows the mouse there.
    if (self.state->motionCanvas.gesture != 0) {
        [self motionCanvasMouseDragged:event];
        return;
    }
    if (self.state->sourceMonitorActive &&
        self.window.firstResponder == self.sourceMonitorView) {
        [self scrubSourceMonitorEvent:event];
        return;
    }
    const NSPoint point = [self timelinePointForEvent:event];
    if (self.state->fadeDrag) {
        // GitHub #330 -- the fade follows the pointer on whole frames,
        // never past the other one (TimelineView.h).
        const DocumentClip* faded =
            self.state->document.FindClip(self.state->fadeDrag->press.clip_id);
        if (faded == nullptr) {
            self.state->fadeDrag.reset();
            self.state->overlayDirty = true;
            return;
        }
        self.state->fadeDrag->reached =
            UpdateTimelineFadeDrag(self.state->document, self.state->viewport,
                                   self.state->fadeDrag->press, point.x);
        [self showFadeReadout:*self.state->fadeDrag->reached
                         edge:self.state->fadeDrag->press.edge];
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->markerStretch) {
        // D6 -- the span follows the pointer, on whole frames; the policy
        // (which side, a point again on the anchor) is TimelineView.h's.
        const DocumentMarker* marker = self.state->document.FindMarker(
            self.state->markerStretch->marker_id);
        if (!marker) {
            self.state->markerStretch.reset();
            return;
        }
        const double x = std::clamp(point.x, self.state->viewport.header_width,
                                    self.metalView.bounds.size.width);
        RationalTime pointer =
            self.state->viewport.XToTime(x, [self playheadFrameRate].num);
        if (pointer < RationalTime{0, 1}) pointer = {0, pointer.rate};
        pointer = QuantizePlayheadPosition(pointer, PlayheadResolution::Frame,
                                           [self playheadFrameRate]);
        self.state->markerStretch->reached = TimelineMarkerSpanUpdate(
            *marker, self.state->markerStretch->anchor, pointer);
        const UpdateMarkerOperation& reached =
            *self.state->markerStretch->reached;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MARQUEUR  %@ → %@",
                             TimelineTimecode(reached.time,
                                              [self playheadFrameRate]),
                             TimelineTimecode(
                                 reached.time.add(reached.duration),
                                 [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->zoomBarDrag) {
        UpdateTimelineZoomBarDrag(
            self.state->viewport, *self.state->zoomBarDrag, point.x,
            self.state->duration, self.metalView.bounds.size.width,
            kTimelineViewRate);
        if (NavTraceEnabled()) {
            const TimelineZoomBarGeometry held =
                CalculateTimelineZoomBarGeometry(
                    self.state->viewport, self.state->duration,
                    self.metalView.bounds.size.width,
                    self.state->zoomBarDrag->initial_geometry.ruler_seconds);
            std::fprintf(stderr,
                         "nav bar-drag x=%.1f part=%s thumb=%.1f..%.1f "
                         "pps=%.3f start=%.4fs\n",
                         point.x, NavTracePart(self.state->zoomBarDrag->part),
                         held.thumb_x, held.thumb_x + held.thumb_width,
                         self.state->viewport.pixels_per_second,
                         NavTraceStart(self.state->viewport));
        }
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->navigationDragging) {
        const double delta = self.state->navigationLastX - point.x;
        self.state->viewport.ScrollByPixels(delta, kTimelineViewRate);
        self.state->navigationLastX = point.x;
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->lassoCandidate) {
        self.state->lassoCurrentX = point.x;
        self.state->lassoCurrentY = point.y;
        const double deltaX = point.x - self.state->lassoStartX;
        const double deltaY = point.y - self.state->lassoStartY;
        if (!self.state->lassoDragging && std::hypot(deltaX, deltaY) >= 4.0) {
            self.state->lassoDragging = true;
            self.state->scrubDragging = false;
            // The press jumped the playhead to it before the gesture could
            // tell a lasso from a scrub; selecting does not move it.
            [self requestTimelinePosition:self.state->lassoPlayhead];
        }
        if (self.state->lassoDragging) {
            std::vector<Ulid> selected = LassoHitTestTimeline(
                self.state->document, self.state->viewport,
                self.state->lassoStartX, self.state->lassoStartY,
                self.state->lassoCurrentX, self.state->lassoCurrentY,
                self.metalView.bounds.size.width);
            if (self.state->linkedSelectionGesture)
                selected =
                    ExpandLinkedClipSelection(self.state->document, selected);
            self.state->interaction->SelectClips(selected);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->scrubDragging) {
        RationalTime requested =
            self.state->viewport.XToTime(point.x, [self playheadInputRate]);
        if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) {
            const auto snapped = SnapTimelineTimeToEdit(
                self.state->document, self.state->viewport, requested);
            if (snapped) requested = *snapped;
        }
        [self requestTimelinePosition:requested];
        return;
    }
    self.state->interaction->SetPlayheadSnapTarget(
        (event.modifierFlags & NSEventModifierFlagCommand) != 0
            ? std::optional<RationalTime>(self.state->requestedPosition)
            : std::nullopt);
    self.state->interaction->SetPlayhead(self.state->requestedPosition);
    // FC1 -- read again with each move, as the playhead target is.
    self.state->interaction->SetInOutSnapTargets(self.state->timelineIn,
                                                 self.state->timelineOut);
    self.state->interaction->PointerDrag(point.x, point.y,
                                         self.metalView.bounds.size.width);
    // AppKit may report Option only after the mouse-down event. Promote an
    // active body move to a duplicate as soon as the modifier appears, which
    // matches the interaction users expect from Premiere/Resolve.
    if (!self.state->duplicateDragging && self.state->editDragging &&
        [self effectiveTool] == TimelineTool::Select &&
        (event.modifierFlags & NSEventModifierFlagOption) != 0 &&
        self.state->interaction->MovePreview()) {
        self.state->duplicateDragging = true;
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    }
    self.state->overlayDirty = true;
}

- (void)timelineMouseMoved:(NSEvent*)event {
    if (self.videoFullscreenActive) return;
    // REM5 -- over Record, the node under the mouse is outlined.
    if ([self motionCanvasContainsEvent:event]) {
        // GitHub #330 -- and no clip of the timeline is under it.
        if (!self.state->hoveredClipId.empty()) {
            self.state->hoveredClipId.clear();
            self.state->overlayDirty = true;
        }
        [self motionCanvasHover:event];
        return;
    }
    [self applyToolCursor];
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    const NSPoint point = [self timelinePointForEvent:event];
    const int previousHoveredTool = self.state->hoveredTimelineTool;
    const Ulid previousHoveredTrack = self.state->hoveredTrackId;
    const int previousHoveredControl = self.state->hoveredTrackControl;
    self.state->hoveredTimelineTool = -1;
    self.state->hoveredTrackId.clear();
    self.state->hoveredTrackControl = -1;
    // FC2 -- what a press here would take hold of.
    const TimelinePointerTarget target = [self timelineTargetAt:point
                                                          event:event];
    // D2 -- the flag under the pointer, by the geometry it is drawn from.
    const Ulid previousHoveredMarker = self.state->hoveredMarkerId;
    self.state->hoveredMarkerId =
        target.kind == TimelinePointerTargetKind::Marker ? target.marker_id
                                                         : Ulid{};
    // GitHub #330 -- the clip under the pointer shows its fade handles, with
    // the tools that take them.
    const Ulid previousHoveredClip = self.state->hoveredClipId;
    const TimelineTool hoverTool = [self effectiveTool];
    self.state->hoveredClipId =
        (hoverTool == TimelineTool::Select ||
         hoverTool == TimelineTool::Trim) &&
                (target.kind == TimelinePointerTargetKind::ClipBody ||
                 target.kind == TimelinePointerTargetKind::ClipEdge ||
                 target.kind == TimelinePointerTargetKind::FadeHandle)
            ? target.clip_id
            : Ulid{};

    if (point.y >= 0.0 && point.y < kTimelineToolbarHeight && point.x >= 4.0 &&
        point.x < 124.0)
        self.state->hoveredTimelineTool =
            static_cast<int>((point.x - 4.0) / 24.0);
    if (point.y >= kTimelineRulerHeight) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t trackIndex =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (trackIndex < tracks.size() && point.x >= 40.0 && point.x < 92.0) {
            self.state->hoveredTrackId = tracks[trackIndex]->id;
            self.state->hoveredTrackControl = point.x < 56.0   ? 0
                                              : point.x < 74.0 ? 1
                                                               : 2;
        }
    }
    const bool chromeHoverChanged =
        previousHoveredTool != self.state->hoveredTimelineTool ||
        previousHoveredTrack != self.state->hoveredTrackId ||
        previousHoveredControl != self.state->hoveredTrackControl ||
        previousHoveredMarker != self.state->hoveredMarkerId ||
        previousHoveredClip != self.state->hoveredClipId;
    NSString* const fadeHint =
        target.kind == TimelinePointerTargetKind::FadeHandle
            ? (target.edge == TimelineHitEdge::Head
                   ? @"FONDU D’ENTRÉE · glisser pour l’allonger ou le "
                     @"raccourcir"
                   : @"FONDU DE SORTIE · glisser pour l’allonger ou le "
                     @"raccourcir")
            : nil;
    if ([self effectiveTool] == TimelineTool::Select) {
        if (fadeHint != nil) {
            [NSCursor.resizeLeftRightCursor set];
            self.infoLabel.stringValue = fadeHint;
        } else if (target.kind == TimelinePointerTargetKind::ClipEdge) {
            [[self edgeCursorFor:target] set];
            if (TimelineTrimModeStretches(target.trim_mode))
                self.infoLabel.stringValue =
                    @"VITESSE · glisser pour étirer le plan";
            else if (target.trim_mode == TimelineTrimMode::Roll)
                self.infoLabel.stringValue =
                    @"ROLL · glisser la coupe, les deux côtés à la fois";
            else if (target.trim_mode == TimelineTrimMode::Ripple)
                self.infoLabel.stringValue = @"RIPPLE TRIM · Shift-glisser";
            else
                self.infoLabel.stringValue = @"TRIM · glisser le raccord";
        } else if (target.duplicate) {
            self.infoLabel.stringValue = @"DUPLIQUER · Option-glisser le clip";
        }
        if (chromeHoverChanged) self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] == TimelineTool::Trim) {
        // FC4 -- the Trim tool: an edge ripples (Command rolls), the body
        // slips.
        // DEC40 -- its stretch ripples.
        if (fadeHint != nil)
            self.infoLabel.stringValue = fadeHint;
        else if (target.kind == TimelinePointerTargetKind::ClipEdge)
            self.infoLabel.stringValue =
                TimelineTrimModeStretches(target.trim_mode)
                    ? @"VITESSE · glisser pour étirer le plan, la suite suit"
                : target.trim_mode == TimelineTrimMode::Roll
                    ? @"ROLL · glisser la coupe, les deux côtés à la fois"
                    : @"RIPPLE TRIM · glisser le raccord";
        else if (target.kind == TimelinePointerTargetKind::ClipBody)
            self.infoLabel.stringValue =
                target.trim_mode == TimelineTrimMode::Slide
                    ? @"SLIDE · glisser le plan entre ses voisins"
                    : @"SLIP · glisser le contenu source";
        // The author, 31 October 2026: "des fois il est en mode trim normal,
        // des fois il bouge ce qu'il y a dans le plan". The tool's one cursor,
        // the double arrow, said nothing of which: six points from an edge
        // it ripples, past them it slips. Each gesture now has its own --
        // the double arrow on an edge, an open hand on the body that slips
        // the content, a pointing hand on the name bar that slides the clip
        // -- before the press, not only on the status line.
        if (fadeHint != nil)
            [NSCursor.resizeLeftRightCursor set];
        else if (target.kind == TimelinePointerTargetKind::ClipBody)
            [(target.trim_mode == TimelineTrimMode::Slide
                  ? NSCursor.pointingHandCursor
                  : NSCursor.openHandCursor) set];
        else if (target.kind == TimelinePointerTargetKind::ClipEdge)
            [[self edgeCursorFor:target] set];
        else
            [NSCursor.arrowCursor set];
        self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] != TimelineTool::Cut) {
        self.state->overlayDirty = true;
        return;
    }
    if (point.y < kTimelineRulerHeight || point.y > [self timelineHeight]) {
        self.state->overlayDirty = true;
        return;
    }
    const DocumentClip* clip =
        target.kind == TimelinePointerTargetKind::ClipBody
            ? self.state->document.FindClip(target.clip_id)
            : nullptr;
    if (clip) {
        const RationalTime cut =
            self.state->viewport.XToTime(point.x, clip->duration.rate);
        if (cut > clip->timeline_in &&
            cut < clip->timeline_in.add(clip->duration)) {
            self.state->cutPreviewX = self.state->viewport.TimeToX(cut);
            const auto tracks =
                TimelineTracksInDisplayOrder(self.state->document);
            const auto track = std::find_if(
                tracks.begin(), tracks.end(), [&](const DocumentTrack* value) {
                    return value->id == target.track_id;
                });
            if (track != tracks.end())
                self.state->cutPreviewY = kTimelineRulerHeight +
                                          std::distance(tracks.begin(), track) *
                                              self.state->viewport.track_height;
        }
    }
    self.state->overlayDirty = true;
}

- (void)timelineMouseUp:(NSEvent*)event {
    if (self.videoFullscreenActive) return;
    if (self.trimMonitorDragging) {
        [self trimMonitorRelease];
        return;
    }
    // REM5 -- and ends there, as one operation.
    if (self.state->motionCanvas.gesture != 0) {
        [self motionCanvasMouseUp:event];
        return;
    }
    if (self.state->sourceMonitorActive &&
        self.window.firstResponder == self.sourceMonitorView)
        return;
    (void)event;
    if (self.state->fadeDrag) {
        // GitHub #330 -- released: the fade the pointer reached is one edit.
        // A press that moved nothing changes nothing.
        const AppState::FadeDrag drag = *self.state->fadeDrag;
        self.state->fadeDrag.reset();
        self.state->overlayDirty = true;
        if (!drag.reached) return;
        const SetClipAudioOperation& reached = *drag.reached;
        if (reached.fade_in == drag.press.start.fade_in &&
            reached.fade_out == drag.press.start.fade_out)
            return;
        EditError error = EditError::None;
        std::string message;
        const RationalTime playhead = self.state->requestedPosition;
        if ([self applyAndPersistTimelineOperation:Operation{reached}
                                             error:error
                                           message:message]) {
            [self refreshTimelineAfterEditFromPosition:playhead];
            [self updateSelectionInfo];
            [self showFadeReadout:reached edge:drag.press.edge];
        } else {
            std::fprintf(stderr, "Fade rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Fondu refusé : %@",
                                           CMStringFromUtf8(message)];
        }
        return;
    }
    if (self.state->markerStretch) {
        // D6 -- released: the span the pointer reached is one edit. A press
        // with no drag changes nothing.
        const auto stretch = *self.state->markerStretch;
        self.state->markerStretch.reset();
        self.state->overlayDirty = true;
        const DocumentMarker* marker =
            self.state->document.FindMarker(stretch.marker_id);
        if (!marker || !stretch.reached ||
            (stretch.reached->time == marker->time &&
             stretch.reached->duration == marker->duration))
            return;
        const UpdateMarkerOperation reached = *stretch.reached;
        if ([self applyMarkerOperation:reached]) {
            if (const DocumentMarker* stretched =
                    self.state->document.FindMarker(reached.marker_id))
                [self showMarkerInfo:*stretched];
        }
        return;
    }
    if (self.state->zoomBarDrag) {
        self.state->zoomBarDrag.reset();
        if (NavTraceEnabled()) {
            const TimelineZoomBarGeometry released =
                CalculateTimelineZoomBarGeometry(
                    self.state->viewport, self.state->duration,
                    self.metalView.bounds.size.width);
            std::fprintf(
                stderr,
                "nav bar-up thumb=%.1f..%.1f ruler=%.3fs pps=%.3f "
                "start=%.4fs\n",
                released.thumb_x, released.thumb_x + released.thumb_width,
                released.ruler_seconds, self.state->viewport.pixels_per_second,
                NavTraceStart(self.state->viewport));
        }
        self.state->overlayDirty = true;
        return;
    }
    self.state->interaction->SetPlayheadSnapTarget(std::nullopt);
    self.state->interaction->SetInOutSnapTargets(std::nullopt, std::nullopt);
    if (self.state->lassoCandidate) {
        const bool completedLasso = self.state->lassoDragging;
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        if (completedLasso) {
            self.state->scrubDragging = false;
            self.state->interaction->SetLinkedSelectionEnabled(
                self.state->linkedSelection);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->navigationDragging || self.state->scrubDragging) {
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        [self applyToolCursor];
        return;
    }
    if (!self.state->editDragging) {
        self.state->duplicateDragging = false;
        self.state->duplicateDragClipboard.clear();
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        return;
    }
    self.state->editDragging = false;
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    trim::TrimRefusal edgeRefusal = trim::TrimRefusal::None;
    const Document documentBeforeEdit = self.state->document;
    const EditLog logBeforeEdit = self.state->editLog;
    if (self.state->duplicateDragging) {
        const auto preview = self.state->interaction->MovePreview();
        self.state->interaction->CancelDrag();
        self.state->duplicateDragging = false;
        const auto operation =
            preview ? PasteTimelineClipboardAtMoves(
                          self.state->duplicateDragClipboard, *preview)
                    : std::nullopt;
        self.state->duplicateDragClipboard.clear();
        if (operation)
            [self applyTimelinePaste:*operation label:@"Clips dupliqués"];
        else
            self.infoLabel.stringValue = @"Duplication annulée";
    } else if (self.state->interaction->PointerUp(error, message)) {
        if (![self persistEditsInBackground:message]) {
            self.state->document = documentBeforeEdit;
            self.state->editLog = logBeforeEdit;
            std::fprintf(stderr, "Unable to persist edit: %s\n",
                         message.c_str());
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Modification non enregistrée : %@",
                                           CMStringFromUtf8(message)];
        }
        [self refreshTimelineAfterEditFromPosition:playhead];
    } else if (error != EditError::None) {
        std::fprintf(stderr, "Edit rejected (%s): %s\n", EditErrorName(error),
                     message.c_str());
    } else {
        edgeRefusal = [self selectPressedEdgePoint];
    }
    self.state->pressTarget.reset();
    [self updateSelectionInfo];
    if (edgeRefusal != trim::TrimRefusal::None)
        [self showTrimRefusal:edgeRefusal];
    else if (self.state->trimSession.Active())
        [self showTrimStatus];
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->overlayDirty = true;
}

- (void)timelineScroll:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) return;
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    // Visual QA of 1 October 2026 -- what a scroll means and how far it
    // zooms are decided in TimelineView.h; AppKit only says whether the
    // deltas are precise and which modifiers are held. Option zooms on any
    // device: a trackpad's scroll was a pan and nothing else.
    const bool precise = event.hasPreciseScrollingDeltas;
    const TimelineScrollIntent intent = ResolveTimelineScrollIntent(
        event.scrollingDeltaX, event.scrollingDeltaY, precise,
        (event.modifierFlags & NSEventModifierFlagShift) != 0,
        (event.modifierFlags & NSEventModifierFlagOption) != 0,
        event.isDirectionInvertedFromDevice);
    if (std::abs(intent.delta) <= 0.0001) return;
    try {
        const TimelineViewport before = self.state->viewport;
        if (intent.action == TimelineScrollAction::Zoom) {
            // Around the playhead, wherever the pointer is: the zoom of
            // the editor's other program (TimelineView.h).
            self.state->zoomAnchor = ZoomTimelineAtPlayhead(
                self.state->viewport, self.state->requestedPosition,
                TimelineScrollZoomFactor(intent.delta, precise),
                self.state->duration, self.metalView.bounds.size.width,
                self.state->zoomAnchor);
        } else {
            self.state->viewport.ScrollByPixels(intent.delta,
                                                kTimelineViewRate);
        }
        if (NavTraceEnabled())
            std::fprintf(
                stderr,
                "nav scroll dx=%.3f dy=%.3f precise=%d inverted=%d shift=%d "
                "option=%d phase=%lu momentum=%lu x=%.1f -> %s %.3f | "
                "pps %.3f->%.3f start %.4fs->%.4fs | playhead %.3fs at "
                "x %.1f->%.1f\n",
                event.scrollingDeltaX, event.scrollingDeltaY, precise ? 1 : 0,
                event.isDirectionInvertedFromDevice ? 1 : 0,
                (event.modifierFlags & NSEventModifierFlagShift) != 0 ? 1 : 0,
                (event.modifierFlags & NSEventModifierFlagOption) != 0 ? 1 : 0,
                static_cast<unsigned long>(event.phase),
                static_cast<unsigned long>(event.momentumPhase), point.x,
                intent.action == TimelineScrollAction::Zoom ? "zoom" : "pan",
                intent.delta, before.pixels_per_second,
                self.state->viewport.pixels_per_second, NavTraceStart(before),
                NavTraceStart(self.state->viewport),
                static_cast<double>(self.state->requestedPosition.value) /
                    self.state->requestedPosition.rate,
                before.TimeToX(self.state->requestedPosition),
                self.state->viewport.TimeToX(self.state->requestedPosition));
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport update rejected: %s\n",
                     exception.what());
    }
}

- (void)timelineMagnify:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) return;
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    try {
        const TimelineViewport before = self.state->viewport;
        self.state->zoomAnchor = ZoomTimelineAtPlayhead(
            self.state->viewport, self.state->requestedPosition,
            std::exp(std::clamp(static_cast<double>(event.magnification), -1.0,
                                1.0)),
            self.state->duration, self.metalView.bounds.size.width,
            self.state->zoomAnchor);
        if (NavTraceEnabled())
            std::fprintf(stderr,
                         "nav pinch magnification=%.4f x=%.1f | pps "
                         "%.3f->%.3f start %.4fs->%.4fs\n",
                         static_cast<double>(event.magnification), point.x,
                         before.pixels_per_second,
                         self.state->viewport.pixels_per_second,
                         NavTraceStart(before),
                         NavTraceStart(self.state->viewport));
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport magnification rejected: %s\n",
                     exception.what());
    }
}

- (BOOL)timelineKeyDown:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    // REM5 -- on Record, with a motion node selected: the arrows nudge it,
    // Delete removes it, Escape lets it go.
    if (self.window.firstResponder == self.programMonitorView &&
        !self.state->motionCanvas.selected.empty() &&
        [self motionCanvasKeyDown:event]) {
        return YES;
    }
    if (self.videoFullscreenActive && event.keyCode == 53) {
        [self toggleFullscreen:nil];
        return YES;
    }
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        // Out of lot, 29 October 2026 -- the source monitor plays: Space,
        // J, K and L act on it while it is the active viewer, as Resolve's
        // transport keys act on the viewer that has the focus.
        if (source && [self event:event matchesShortcut:@"play.toggle"]) {
            if (!event.isARepeat)
                [self setSourcePlaybackDirection:
                          self.state->sourcePlaybackDirection != 0 ? 0 : 1];
            return YES;
        }
        if (source && [self event:event matchesShortcut:@"play.forward"]) {
            [self setSourcePlaybackDirection:NextShuttleSpeed(
                                                 self.state
                                                     ->sourcePlaybackDirection,
                                                 1)];
            return YES;
        }
        if (source && [self event:event matchesShortcut:@"play.reverse"]) {
            [self setSourcePlaybackDirection:NextShuttleSpeed(
                                                 self.state
                                                     ->sourcePlaybackDirection,
                                                 -1)];
            return YES;
        }
        if (source && [self event:event matchesShortcut:@"play.stop"]) {
            [self setSourcePlaybackDirection:0];
            return YES;
        }
        // A step, a jump to either end: the player stops where asked.
        if (source && (event.keyCode == 123 || event.keyCode == 124 ||
                       event.keyCode == 115 || event.keyCode == 119))
            [self setSourcePlaybackDirection:0];
        if (source && (event.keyCode == 123 || event.keyCode == 124)) {
            // Shift moves by one timecode second, as in Resolve.
            const int64_t amount = (modifiers & NSEventModifierFlagShift) != 0
                                       ? NominalTimecodeFps(source->rate)
                                       : 1;
            const int64_t direction = event.keyCode == 123 ? -1 : 1;
            [self requestSourcePosition:self.state->sourceMonitorPosition.add(
                                            {direction * amount *
                                                 source->rate.den,
                                             source->rate.num})];
            return YES;
        }
        if (source && event.keyCode == 115) {
            [self requestSourcePosition:{0, source->duration.rate}];
            return YES;
        }
        if (source && event.keyCode == 119) {
            [self requestSourcePosition:source->duration];
            return YES;
        }
    }
    [self dismissAgentMarks];  // PRD25
    NSString* characters = event.charactersIgnoringModifiers.lowercaseString;
    if ([self event:event matchesShortcut:@"view.fullscreen"]) {
        [self toggleFullscreen:nil];
        return YES;
    }
    // G6 -- the gesture a colourist makes before trusting a grade.
    if ([self event:event matchesShortcut:@"color.bypass"]) {
        [self toggleGradeBypass:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"color.reference.grab"]) {
        [self grabReferenceStill:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"color.reference.split"]) {
        [self toggleReferenceSplit:nil];
        return YES;
    }
    // ON9 -- the slash plays around the selection, Command-slash loops.
    if ([self event:event matchesShortcut:@"play.loop"]) {
        if (!event.isARepeat) [self menuToggleLoop:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.around"]) {
        if (!event.isARepeat) [self playAroundSelection];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.toggle"]) {
        if ([characters isEqualToString:@" "]) {
            if (!event.isARepeat) self.state->spaceUsedForPan = false;
            self.state->spaceHand = true;
            [self applyToolCursor];
        } else if (!event.isARepeat) {
            [self menuPlayPause:nil];
        }
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.linked"]) {
        [self menuToggleLinkedSelection:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.copy"]) {
        [self menuCopyTimelineClips:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.paste"]) {
        [self menuPasteTimelineClips:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"transition.add"]) {
        [self menuAddCrossDissolve:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.ripple"]) {
        [self deleteCurrentTimelineSelectionRipple:YES];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.delete"]) {
        [self deleteCurrentTimelineSelectionRipple:NO];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.in"]) {
        [self menuMarkTimelineIn:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.out"]) {
        [self menuMarkTimelineOut:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.clear"]) {
        [self menuClearTimelineRange:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"source.insert"]) {
        [self menuInsertSource:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"source.overwrite"]) {
        [self menuOverwriteSource:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.select"]) {
        [self setTimelineTool:TimelineTool::Select];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.hand"]) {
        [self setTimelineTool:TimelineTool::Hand];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.zoom"]) {
        [self setTimelineTool:TimelineTool::Zoom];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.cut"] ||
        [self event:event matchesShortcut:@"tool.cut.alternate"]) {
        [self setTimelineTool:TimelineTool::Cut];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.trim"]) {
        [self setTimelineTool:TimelineTool::Trim];
        return YES;
    }
    // FC4 -- the trim mode's keys, Resolve's (KeyboardShortcuts.h).
    if ([self event:event matchesShortcut:@"trim.select"]) {
        if (!event.isARepeat) [self trimSelectNearest];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.side"]) {
        if (!event.isARepeat) [self trimCycleSide];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.back"]) {
        [self trimNudge:-1];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.forward"]) {
        [self trimNudge:1];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.back.many"]) {
        [self trimNudge:-trim::kLargeTrimFrames];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.forward.many"]) {
        [self trimNudge:trim::kLargeTrimFrames];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.extend"]) {
        if (!event.isARepeat) [self trimExtendToPlayhead];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.dynamic"]) {
        if (!event.isARepeat) [self toggleDynamicTrim];
        return YES;
    }
    // FC3 -- Command-R, before the Command keys below claim it.
    if ([self event:event matchesShortcut:@"clip.retime"]) {
        if (!event.isARepeat) [self toggleRetimeControls];
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.fit"]) {
        [self menuFitTimeline:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.snapping"]) {
        [self menuToggleSnapping:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"select.after"]) {
        [self menuSelectClipsAfter:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"clip.enable"]) {
        if (!event.isARepeat) [self menuToggleClipsEnabled:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.reverse"]) {
        if ([self dynamicTrimTransport:-1]) return YES;
        [self setPlaybackDirection:NextShuttleSpeed(
                                       self.state->playbackDirection, -1)];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.stop"]) {
        [self menuStop:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.forward"]) {
        if ([self dynamicTrimTransport:1]) return YES;
        [self setPlaybackDirection:NextShuttleSpeed(
                                       self.state->playbackDirection, 1)];
        return YES;
    }
    const bool zoomKey = [characters isEqualToString:@"+"] ||
                         [characters isEqualToString:@"="] ||
                         [characters isEqualToString:@"-"];
    if ((modifiers & NSEventModifierFlagCommand) != 0) {
        if ([characters isEqualToString:@"t"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            [self addTrack:(modifiers & NSEventModifierFlagOption) != 0];
            return YES;
        }
        // Resolve zooms the timeline with Cmd+= and Cmd+- (its + and - alone
        // start a relative timecode, which this window has not).
        if (zoomKey && (modifiers & (NSEventModifierFlagOption |
                                     NSEventModifierFlagControl)) == 0) {
            [self zoomTimelineAtPlayheadOut:[characters isEqualToString:@"-"]];
            return YES;
        }
        if (![characters isEqualToString:@"z"]) return NO;
        if ((modifiers & NSEventModifierFlagShift) != 0)
            [self menuRedo:nil];
        else
            [self menuUndo:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"marker.add"]) {
        if (!event.isARepeat) [self menuAddMarker:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"marker.note"]) {
        if (!event.isARepeat) [self menuAnnotateMarker:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"playhead.resolution"]) {
        self.state->playheadResolution =
            self.state->playheadResolution == PlayheadResolution::Frame
                ? PlayheadResolution::Sample
                : PlayheadResolution::Frame;
        if (self.state->playbackDirection == 0)
            [self requestTimelinePosition:self.state->requestedPosition];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        return YES;
    }
    if (zoomKey) {
        [self zoomTimelineAtPlayheadOut:[characters isEqualToString:@"-"]];
        return YES;
    }
    if (event.keyCode == 115) {  // Home
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        return YES;
    }
    if (event.keyCode == 119) {  // End
        [self requestTimelinePosition:self.state->duration];
        return YES;
    }
    if (event.keyCode == 126 || event.keyCode == 125) {  // Up / down
        const bool forward = event.keyCode == 125;
        // D2 -- with Shift, from marker to marker instead of cut to cut.
        if ((modifiers & NSEventModifierFlagShift) != 0) {
            [self goToAdjacentMarkerForward:forward];
            return YES;
        }
        [self setPlaybackDirection:0];
        const auto edit = AdjacentTimelineEdit(
            self.state->document, self.state->requestedPosition, forward);
        if (edit) [self requestTimelinePosition:*edit];
        return YES;
    }
    if (event.keyCode == 123 || event.keyCode == 124) {  // Left / right
        const int64_t direction = event.keyCode == 123 ? -1 : 1;
        if ((modifiers & NSEventModifierFlagShift) != 0) {
            [self stepPlayheadSeconds:direction];
        } else {
            [self stepPlayheadFrames:direction];
        }
        return YES;
    }
    // D7 -- docs/roadmap/DIALOGUE-2026-10.md. Return puts the keyboard in
    // the text of the one carton selected; on any other selection the key
    // stays unhandled.
    if ((event.keyCode == 36 || event.keyCode == 76) &&
        (modifiers & kShortcutModifierMask) == 0) {
        if (const auto carton = ui::media_panel::SelectedCartonForText(
                self.state->document,
                self.state->interaction->SelectedClipIds())) {
            const Ulid edited = *carton;
            if (!event.isARepeat) [self editCartonText:edited];
            return YES;
        }
    }
    if (event.keyCode == 53) {  // Escape
        // FC4 -- a dynamic trim running is dropped, nothing written; else
        // the trim mode is left.
        if (self.state->trimSession.CancelDynamic()) {
            self.state->trimPreview.reset();
            [self requestResolvedPosition:self.state->requestedPosition];
            [self showTrimStatus];
            self.state->overlayDirty = true;
            return YES;
        }
        if (self.state->trimSession.Active()) {
            [self leaveTrimMode];
            self.infoLabel.stringValue = @"Mode trim quitté";
        }
        // FC3 -- and the Retime controls close (Resolve 21.1 manual, p.
        // 1222).
        self.state->retimeClipIds.clear();
        self.state->interaction->CancelDrag();
        self.state->markerStretch.reset();
        self.state->fadeDrag.reset();
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->editDragging = false;
        self.state->duplicateDragging = false;
        self.state->duplicateDragClipboard.clear();
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        self.state->overlayDirty = true;
        return YES;
    }
    return NO;
}

- (void)timelineKeyUp:(NSEvent*)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@" "] &&
        [self event:event matchesShortcut:@"play.toggle"]) {
        self.state->spaceHand = false;
        [self applyToolCursor];
        if (!self.state->spaceUsedForPan) {
            [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1
                                                                          : 0];
        }
    }
}

- (BOOL)textEditKeyDown:(NSEvent*)event inTextView:(NSTextView*)text {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & kShortcutModifierMask;
    const BOOL shift = (modifiers & NSEventModifierFlagShift) != 0;
    if ((modifiers & ~NSEventModifierFlagShift) != NSEventModifierFlagCommand)
        return NO;
    // Cmd+Return in the carton's field: Return breaks the line there, so
    // this is the key that says "done". Leaving the field commits it.
    if (!shift && (event.keyCode == 36 || event.keyCode == 76) &&
        text.delegate == (id)self.inspectorView.captionTextField) {
        [self.window makeFirstResponder:self.metalView];
        return YES;
    }
    NSString* key = event.charactersIgnoringModifiers.lowercaseString;
    if ([key isEqualToString:@"z"]) {
        NSUndoManager* typed = text.undoManager;
        if (shift && typed.canRedo)
            [typed redo];
        else if (!shift && typed.canUndo)
            [typed undo];
        return YES;
    }
    if (shift) return NO;
    if ([key isEqualToString:@"c"])
        [text copy:nil];
    else if ([key isEqualToString:@"v"])
        [text paste:nil];
    else if ([key isEqualToString:@"x"])
        [text cut:nil];
    else if ([key isEqualToString:@"a"])
        [text selectAll:nil];
    else
        return NO;
    return YES;
}

- (NSMenu*)timelineMenuForEvent:(NSEvent*)event {
    if (self.videoFullscreenActive) return nil;
    // REM5 -- Record's menu builds the motion clip it shows.
    if ([self motionCanvasContainsEvent:event]) {
        return [self motionCanvasMenuForEvent:event];
    }
    const NSPoint viewPoint =
        [self.metalView convertPoint:event.locationInWindow fromView:nil];
    const double timelineY =
        self.metalView.bounds.size.height - viewPoint.y - [self videoHeight];
    self.state->contextClipId.clear();
    self.state->contextTrackId.clear();
    self.state->contextGap.reset();
    if (timelineY < 0.0 || timelineY > [self timelineHeight]) return nil;
    const RationalTime time =
        self.state->viewport.XToTime(viewPoint.x, self.state->duration.rate);
    self.state->contextTime = time;
    const auto hit =
        HitTestTimeline(self.state->document, self.state->viewport, viewPoint.x,
                        timelineY, self.metalView.bounds.size.width);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Timeline"];
    if (hit) {
        self.state->contextClipId = hit->clip_id;
        self.state->contextTrackId = hit->track_id;
        // SY2 -- as in Resolve and in every list of macOS, a right click on
        // a clip already selected keeps the selection: Auto Align takes the
        // picture and the sound selected together.
        const std::vector<Ulid>& selected =
            self.state->interaction->SelectedClipIds();
        if (std::find(selected.begin(), selected.end(), hit->clip_id) ==
            selected.end()) {
            std::vector<Ulid> selection{hit->clip_id};
            if (self.state->linkedSelection)
                selection =
                    ExpandLinkedClipSelection(self.state->document, selection);
            self.state->interaction->SelectClips(selection);
        }
        [self updateSelectionInfo];
        // D7 -- docs/roadmap/DIALOGUE-2026-10.md. On a carton, its text
        // first: it has no source to open.
        if (ui::media_panel::CartonTextAccess(self.state->document,
                                              hit->clip_id) !=
            ui::media_panel::CartonTextOutcome::NotACarton) {
            [menu addItem:[self menuItem:@"Modifier le texte"
                                  action:@selector(menuEditCartonText:)
                                     key:@""]];
            [menu addItem:NSMenuItem.separatorItem];
        }
        [menu addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                              action:@selector(contextOpenClipSource:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Retrouver dans la médiathèque"
                              action:@selector(contextFindClipInMediaPool:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Couper ici"
                              action:@selector(contextCutClip:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer"
                              action:@selector(contextDeleteSelection:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Activer / désactiver le plan"
                              action:@selector(menuToggleClipsEnabled:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self autoAlignMenuItem]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        return menu;
    }
    const auto gap = HitTestTimelineGap(
        self.state->document, self.state->viewport, viewPoint.x, timelineY,
        self.metalView.bounds.size.width, self.state->duration.rate);
    if (gap) {
        self.state->contextGap = gap;
        self.state->contextTrackId = gap->track_id;
        [menu addItem:[self menuItem:@"Fermer le gap"
                              action:@selector(contextCloseGap:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        return menu;
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger index = static_cast<NSInteger>(
        (timelineY - kTimelineRulerHeight) / self.state->viewport.track_height);
    if (index >= 0 && index < (NSInteger)tracks.size())
        self.state->contextTrackId = tracks[index]->id;
    if (!self.state->contextTrackId.empty()) {
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItem:[self menuItem:@"Ajouter une piste vidéo"
                          action:@selector(menuAddVideoTrack:)
                             key:@""]];
    [menu addItem:[self menuItem:@"Ajouter une piste audio"
                          action:@selector(menuAddAudioTrack:)
                             key:@""]];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:[self menuItem:@"Cadrer toute la timeline"
                          action:@selector(menuFitTimeline:)
                             key:@""]];
    return menu;
}

@end
