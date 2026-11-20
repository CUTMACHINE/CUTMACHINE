// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Edits of the
// timeline asked from the window: paste and delete (ripple or not), the
// range, markers and their notes, a carton's text, the Source insert,
// multicam angles, the waveform alignment of a sound, retiming, tracks
// added and removed, the context menu's actions, and the effects dropped
// on a cut. Each is an operation (Operations.h). Moved out of main.mm
// unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (TimelineEdits)

// EF1 -- docs/roadmap/EFFETS-2026-10.md. Where an effect over the timeline
// would land: the track and the time under the pointer, handed to
// EffectsLibrary.h, which picks the cut.
- (ui::effects::DropTarget)effectDropTargetForId:(NSString*)effectId
                                     atViewPoint:(NSPoint)point {
    ui::effects::DropTarget target;
    const ui::effects::Effect* effect =
        ui::effects::FindEffect(effectId.UTF8String ?: "");
    if (effect == nullptr || !self.state) {
        target.refusal = "Effet inconnu";
        return target;
    }
    const double timelineY =
        self.metalView.bounds.size.height - point.y - [self videoHeight];
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger trackIndex =
        timelineY < kTimelineRulerHeight
            ? -1
            : static_cast<NSInteger>((timelineY - kTimelineRulerHeight) /
                                     self.state->viewport.track_height);
    if (point.x < self.state->viewport.header_width || trackIndex < 0 ||
        trackIndex >= static_cast<NSInteger>(tracks.size())) {
        target.refusal = "Déposez l’effet sur une piste";
        return target;
    }
    try {
        const MediaRate rate = self.state->document.sequence.frame_rate;
        RationalTime time = self.state->viewport.XToTime(point.x, rate.num);
        if (time.value < 0) time = {0, time.rate};
        return ui::effects::FindDropTarget(self.state->document, *effect,
                                           tracks[trackIndex]->id, time);
    } catch (const std::exception& exception) {
        target.refusal = exception.what();
        return target;
    }
}

// One drop, one operation, one undo: the one EffectsLibrary.h builds.
- (BOOL)applyEffectWithId:(NSString*)effectId
                 toTarget:(const ui::effects::DropTarget&)target {
    const ui::effects::Effect* effect =
        ui::effects::FindEffect(effectId.UTF8String ?: "");
    if (effect == nullptr) return NO;
    ui::effects::DropEdit edit =
        ui::effects::EditForDrop(self.state->document, *effect, target);
    if (!edit.operation) {
        self.infoLabel.stringValue = CMStringFromUtf8(edit.refusal);
        return NO;
    }
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(*edit.operation)
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@ refusé : %@", CMStringFromUtf8(effect->title),
                             CMStringFromUtf8(message)];
        return NO;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    // EF3 -- the Inspector shows what the drop set: a blurred clip's
    // « Flou », a cut's « Fondu ».
    [self updateSelectionInfo];
    self.infoLabel.stringValue = CMStringFromUtf8(edit.summary);
    self.state->overlayDirty = true;
    return YES;
}

// EF1 -- a double click in the list: the cut nearest the playhead, on the
// selected clip's track when it takes the effect, otherwise on the first
// track that has a clip there and does.
- (void)effectsPanel:(CMEffectsPanelView*)panel
    applyEffectWithId:(NSString*)effectId {
    (void)panel;
    const ui::effects::Effect* effect =
        ui::effects::FindEffect(effectId.UTF8String ?: "");
    if (effect == nullptr || !self.state) return;
    const RationalTime playhead = self.state->requestedPosition;
    std::vector<Ulid> candidates;
    const Ulid selected = self.state->interaction->SelectedClipId();
    // EF3 -- a clip effect goes on the selected clip when it takes it,
    // wherever the playhead is; otherwise on the clip under the playhead,
    // the topmost picture track first (below).
    if (ui::effects::IsClipEffect(*effect) && !selected.empty()) {
        const ui::effects::DropTarget target = ui::effects::ClipEffectTarget(
            self.state->document, *effect, selected);
        if (target.valid) {
            [self applyEffectWithId:effectId toTarget:target];
            return;
        }
    }
    if (const DocumentTrack* track =
            selected.empty() ? nullptr
                             : self.state->document.FindTrackForClip(selected))
        candidates.push_back(track->id);
    for (const DocumentTrack* track :
         TimelineTracksInDisplayOrder(self.state->document))
        candidates.push_back(track->id);
    std::string refusal = "Aucun raccord sous la tête de lecture";
    for (const Ulid& trackId : candidates) {
        const ui::effects::DropTarget target = ui::effects::FindDropTarget(
            self.state->document, *effect, trackId, playhead);
        if (target.valid) {
            [self applyEffectWithId:effectId toTarget:target];
            return;
        }
        if (trackId == candidates.front()) refusal = target.refusal;
    }
    self.infoLabel.stringValue = CMStringFromUtf8(refusal);
}

- (void)addTrack:(BOOL)audio {
    int32_t index = 0;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        index = std::max(index, track.index + 1);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    const Ulid trackId = GenerateUlid();
    Operation operation =
        AddTrackOperation{trackId, audio ? "audio" : "video", index};
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        self.state->targetedTrackIds.insert(trackId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else {
        std::fprintf(stderr, "Track creation rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    }
}

- (BOOL)applyTimelinePaste:(PasteClipsOperation)operation
                     label:(NSString*)label {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!stagedLog.Apply(stagedDocument, Operation{std::move(operation)}, error,
                         message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"%@ refusé (%@) : %@", label,
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    const auto& stored =
        std::get<PasteClipsOperation>(stagedLog.AppliedEntries().back().op);
    std::vector<Ulid> pastedIds;
    pastedIds.reserve(stored.clips.size());
    for (const PastedClip& clip : stored.clips)
        pastedIds.push_back(clip.clip_id);
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        std::fprintf(stderr, "Unable to persist paste: %s\n", message.c_str());
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"%@ impossible : %@", label,
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    self.state->interaction->SelectClips(pastedIds);
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%@ · %lu clip%@", label,
                                   (unsigned long)pastedIds.size(),
                                   pastedIds.size() == 1 ? @"" : @"s"];
    self.state->overlayDirty = true;
    return YES;
}

- (BOOL)hasValidTimelineRange {
    return self.state->timelineIn && self.state->timelineOut &&
           *self.state->timelineOut > *self.state->timelineIn;
}

- (BOOL)deleteTimelineRangeRipple:(BOOL)ripple {
    if (![self hasValidTimelineRange]) return NO;
    std::optional<DeleteGapOperation> rangeOperation =
        TimelineRangeDeleteOperation(
            self.state->document, *self.state->timelineIn,
            *self.state->timelineOut, ripple,
            std::vector<Ulid>(self.state->targetedTrackIds.begin(),
                              self.state->targetedTrackIds.end()));
    if (!rangeOperation) return NO;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = *self.state->timelineIn;
    if (![self applyAndPersistTimelineOperation:Operation{
                                                    std::move(*rangeOperation)}
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Range delete rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return NO;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.infoLabel.stringValue = ripple ? @"Zone In/Out extraite avec ripple"
                                        : @"Zone In/Out effacée (gap conservé)";
    self.state->overlayDirty = true;
    return YES;
}

- (BOOL)deleteCurrentTimelineSelectionRipple:(BOOL)ripple {
    if ([self hasValidTimelineRange])
        return [self deleteTimelineRangeRipple:ripple];
    const auto gap = self.state->interaction->SelectedGap();
    const std::vector<Ulid> ids = self.state->interaction->SelectedClipIds();
    if (!gap && ids.empty()) return NO;
    Operation operation =
        gap               ? Operation{GapDeleteOperationForSelection(
                  self.state->document, *gap, self.state->linkedSelection)}
        : ids.size() == 1 ? Operation{ClearClipOperation{ids.front(), {}}}
                          : Operation{ClearClipsOperation{ids, {}}};
    if (!gap && ripple) {
        const auto rippleOperation =
            BuildTimelineRippleDeleteSelection(self.state->document, ids);
        if (!rippleOperation) return NO;
        operation = *rippleOperation;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message])
        return NO;
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    return YES;
}

- (BOOL)deleteCurrentTimelineSelection {
    return [self deleteCurrentTimelineSelectionRipple:NO];
}

// D2 -- docs/roadmap/DIALOGUE-2026-10.md. Markers in the window: the editor
// leaves a note where the playhead is, an agent reads it (`describe`,
// `only: ["markers"]`), and what an agent marks shows up here. Every gesture
// below is one of the three marker operations the engine already had.
//
// A marker changes no clip, so applying one redraws the overlay and nothing
// else: -refreshTimelineAfterEditFromPosition: rebuilds the audio timeline,
// and a marker dropped during playback would cut the sound it annotates.
- (BOOL)applyMarkerOperation:(Operation)operation {
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Marqueur refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->overlayDirty = true;
    [self refreshWorkspaceActivity];
    return YES;
}

- (BOOL)markersBelongToTimeline {
    if (!self.state->sourceMonitorActive || !self.state->sourceMonitor)
        return YES;
    self.infoLabel.stringValue =
        @"Les marqueurs se posent sur la timeline, pas dans SOURCE";
    return NO;
}

- (void)showMarkerInfo:(const DocumentMarker&)marker {
    // D6 -- a span says where it ends.
    NSString* when = TimelineTimecode(marker.time, [self playheadFrameRate]);
    if (marker.duration.value > 0)
        when = [NSString
            stringWithFormat:@"%@ → %@", when,
                             TimelineTimecode(marker.time.add(marker.duration),
                                              [self playheadFrameRate])];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"MARQUEUR  %@    %@", when,
                                   CMStringFromUtf8(marker.name)];
}

// The note of one marker, typed. `note` is the text to start from;
// `answer` gets the button -- first to keep it, second to give up, third
// (only when `removable`) to delete the marker -- and the text typed.
// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel, a
// sheet: the answer comes when it closes, no longer before this returns.
- (void)askMarkerNote:(NSString*)note
                 time:(RationalTime)time
             duration:(RationalTime)duration
            removable:(BOOL)removable
               answer:
                   (void (^)(NSModalResponse response, NSString* typed))answer {
    [self setPlaybackDirection:0];
    // D6 -- a span is named by both its ends.
    NSString* title =
        duration.value > 0
            ? [NSString
                  stringWithFormat:@"Marqueur de %@ à %@",
                                   TimelineTimecode(time,
                                                    [self playheadFrameRate]),
                                   TimelineTimecode(time.add(duration),
                                                    [self playheadFrameRate])]
            : [NSString stringWithFormat:@"Marqueur à %@",
                                         TimelineTimecode(
                                             time, [self playheadFrameRate])];
    NSTextField* field =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 24)];
    field.identifier = @"marker.note";
    field.stringValue = note ?: @"";
    field.placeholderString = @"Ce qu’il y a à voir ou à reprendre ici";
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:title
              message:@"La note est lue par l’agent qui monte ce projet."
                 form:field];
    [panel addButtonWithTitle:@"Enregistrer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    if (removable)
        [panel addButtonWithTitle:@"Supprimer le marqueur"
                             role:CMFormButtonRoleAlternate];
    panel.initialFirstResponder = field;
    void (^answered)(NSModalResponse, NSString*) = [answer copy];
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  answered(response, field.stringValue);
                }];
}

- (void)editMarkerNote:(const Ulid&)markerId {
    const DocumentMarker* found = self.state->document.FindMarker(markerId);
    if (!found) return;
    // Copied: applying an operation replaces the document `found` points in.
    const DocumentMarker marker = *found;
    const bool unnamed = marker.name == kTimelineMarkerDefaultName;
    NSString* note = unnamed ? @"" : CMStringFromUtf8(marker.name);
    [self askMarkerNote:note
                   time:marker.time
               duration:marker.duration
              removable:YES
                 answer:^(NSModalResponse response, NSString* typed) {
                   if (response == NSAlertThirdButtonReturn) {
                       if ([self applyMarkerOperation:RemoveMarkerOperation{
                                                          marker.id}])
                           self.infoLabel.stringValue = @"Marqueur supprimé";
                       return;
                   }
                   if (response != NSAlertFirstButtonReturn) return;
                   const std::string name =
                       TimelineMarkerNameFromNote(typed.UTF8String ?: "");
                   if (name == marker.name) return;
                   if ([self applyMarkerOperation:UpdateMarkerOperation{
                                                      marker.id, name,
                                                      marker.time, marker.color,
                                                      marker.category,
                                                      marker.duration}]) {
                       DocumentMarker renamed = marker;
                       renamed.name = name;
                       [self showMarkerInfo:renamed];
                   }
                 }];
}

- (void)goToAdjacentMarkerForward:(BOOL)forward {
    if (![self markersBelongToTimeline]) return;
    const auto time = AdjacentTimelineMarker(self.state->document,
                                             self.state->requestedPosition,
                                             [self playheadFrameRate], forward);
    if (!time) {
        self.infoLabel.stringValue =
            forward ? @"Pas de marqueur après" : @"Pas de marqueur avant";
        return;
    }
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:*time];
    if (const DocumentMarker* marker = TimelineMarkerAtPlayhead(
            self.state->document, self.state->requestedPosition,
            [self playheadFrameRate]))
        [self showMarkerInfo:*marker];
    self.state->overlayDirty = true;
}

// D7 -- docs/roadmap/DIALOGUE-2026-10.md. The words of a carton, corrected
// in the window: generated by an agent or read from an SRT, they could only
// be changed through set_clip_caption. They are typed in the Inspector, as
// in the editor the timelines go to (InspectorView.mm). What may be sent,
// and the operation that keeps the carton's style, are decided in
// MediaPanelModel.h (EditCartonText); this only applies and reports.
- (void)showCartonInfo:(const Ulid&)clipId {
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    if (!clip) return;
    // One line for a label that has one.
    NSString* text = [CMStringFromUtf8(clip->caption_text)
        stringByReplacingOccurrencesOfString:@"\n"
                                  withString:@" / "];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"CARTON  %@    %@",
                                   TimelineTimecode(clip->timeline_in,
                                                    [self playheadFrameRate]),
                                   text];
}

// One edit, or none and the reason in the status line. YES when the carton
// now holds the text typed.
- (BOOL)setCartonText:(const std::string&)typed forClip:(const Ulid&)clipId {
    using ui::media_panel::CartonTextOutcome;
    ui::media_panel::CartonTextEdit edit =
        ui::media_panel::EditCartonText(self.state->document, clipId, typed);
    switch (edit.outcome) {
        case CartonTextOutcome::NotACarton:
            self.infoLabel.stringValue =
                @"Le texte se modifie sur un carton de la piste de légendes";
            return NO;
        case CartonTextOutcome::Locked:
            self.infoLabel.stringValue =
                @"Piste de légendes verrouillée : le texte ne se modifie pas";
            return NO;
        case CartonTextOutcome::Empty:
            self.infoLabel.stringValue =
                @"Un carton ne reste pas vide : le supprimer pour le retirer";
            return NO;
        case CartonTextOutcome::Unchanged:
            return NO;
        case CartonTextOutcome::Edit:
            break;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{
                                                    std::move(edit.operation)}
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Texte refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    // As a restyle does: the monitor shows the carton under the playhead
    // again, with its new words.
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
    [self showCartonInfo:clipId];
    return YES;
}

// The keyboard goes to the carton's text in the Inspector, its dock turned
// to that panel. The carton is the selection by now: the Inspector shows
// the selected clip, and nothing else.
- (void)editCartonText:(const Ulid&)clipId {
    using ui::media_panel::CartonTextOutcome;
    // Refused here rather than by a field that will not take the keys:
    // the status line says why.
    if (ui::media_panel::CartonTextAccess(self.state->document, clipId) !=
        CartonTextOutcome::Edit) {
        [self setCartonText:"" forClip:clipId];
        return;
    }
    [self setPlaybackDirection:0];
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    if ([self.inspectorView focusCaptionTextForClip:clipId]) {
        self.infoLabel.stringValue =
            @"CARTON · Entrée passe à la ligne · ⌘ Entrée enregistre · Tab "
            @"carton suivant · Échap annule";
        return;
    }
    // The dock shows a motion layer, or the selection is not this carton.
    self.infoLabel.stringValue =
        @"Le texte d’un carton se modifie dans l’Inspector, carton sélectionné";
}

- (BOOL)performSourceEditInsert:(BOOL)insert {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!self.state->sourceMonitor || !source) {
        self.infoLabel.stringValue = @"Chargez d’abord un rush dans SOURCE";
        return NO;
    }
    const DocumentTrack* target = nullptr;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "video" || track.locked ||
            !self.state->targetedTrackIds.count(track.id))
            continue;
        if (!target || track.index < target->index) target = &track;
    }
    if (!target) {
        self.infoLabel.stringValue =
            @"Ciblez au moins une piste vidéo déverrouillée";
        return NO;
    }
    const LibraryMedia* media = nullptr;
    const auto probed = self.state->mediaMetadata.find(source->id);
    if (probed != self.state->mediaMetadata.end())
        media = &probed->second;
    else
        media = self.state->document.FindLibraryMedia(source->id);
    const bool sourceHasAudio =
        media && media->metadata_complete && media->has_audio;
    const DocumentTrack* audioTarget = nullptr;
    if (sourceHasAudio) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "audio" || track.locked ||
                !self.state->targetedTrackIds.count(track.id))
                continue;
            if (!audioTarget || track.index < audioTarget->index)
                audioTarget = &track;
        }
        if (!audioTarget) {
            for (const DocumentTrack& track :
                 self.state->document.sequence.tracks) {
                if (track.kind != "audio" || track.locked) continue;
                if (!audioTarget || track.index < audioTarget->index)
                    audioTarget = &track;
            }
        }
    }

    SourceRecordEditPoints points{self.state->sourceIn, self.state->sourceOut,
                                  self.state->timelineIn,
                                  self.state->timelineOut};
    if (!points.timeline_in && !points.timeline_out) {
        points.timeline_in = self.state->requestedPosition;
    }
    SourceRecordEditRange range;
    std::string rangeError;
    if (!ResolveSourceRecordEditRange(source->duration, points, range,
                                      rangeError)) {
        std::fprintf(stderr, "Source/Record range rejected: %s\n",
                     rangeError.c_str());
        self.infoLabel.stringValue = NSLocalizedString(
            @"Zone Source/Record invalide ou durées incompatibles",
            @"Invalid Source/Record edit points");
        return NO;
    }
    const RationalTime sourceIn = range.source_in;
    const RationalTime sourceOut = range.source_out;
    const RationalTime timelineIn = range.timeline_in;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    Ulid audioTargetId = audioTarget ? audioTarget->id : Ulid{};
    if (sourceHasAudio && audioTargetId.empty()) {
        int32_t index = 0;
        for (const DocumentTrack& track : stagedDocument.sequence.tracks)
            index = std::max(index, track.index + 1);
        audioTargetId = GenerateUlid();
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{AddTrackOperation{audioTargetId, "audio", index}},
                error, message)) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Création audio refusée (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
    }
    std::optional<DeleteGapOperation> operation = TimelineSourceEditOperation(
        stagedDocument, self.state->sourceMonitorId, sourceIn, sourceOut,
        timelineIn, target->id, insert,
        audioTargetId.empty() ? std::vector<Ulid>{}
                              : std::vector<Ulid>{audioTargetId});
    if (!operation) {
        self.infoLabel.stringValue =
            NSLocalizedString(@"Zone Source/Record invalide pour ce montage",
                              @"Invalid Source/Record edit target");
        return NO;
    }
    if (!stagedLog.Apply(stagedDocument, Operation{std::move(*operation)},
                         error, message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Montage refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    const RationalTime next = timelineIn.add(sourceOut.sub(sourceIn));
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    if (!audioTargetId.empty()) {
        self.state->targetedTrackIds.insert(audioTargetId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
    }
    [self refreshTimelineAfterEditFromPosition:next];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Rush %@ depuis SOURCE%@",
                         insert ? @"inséré" : @"écrasé",
                         !audioTargetId.empty() ? @" · audio séparé et lié"
                                                : @""];
    self.state->overlayDirty = true;
    return YES;
}

// F1.5 -- ROADMAP.md. The switch the selected clip stands for: make its angle
// the active one, or clear its group's choice. Multicam.h decides which group
// and angle a clip belongs to, as it does for the CLI and MCP; this only
// reads the selection and applies the one reversible operation.
- (BOOL)multicamSwitchForSelection:(BOOL)clear
                         operation:(SetMulticamActiveAngleOperation&)operation {
    if (self.state == nullptr || !self.state->interaction) {
        return NO;
    }
    const Ulid selected = self.state->interaction->SelectedClipId();
    EditError error = EditError::None;
    std::string message;
    SetMulticamActiveAngleOperation angle;
    if (selected.empty() ||
        !BuildSetMulticamActiveAngle(self.state->document, {}, selected, angle,
                                     error, message)) {
        return NO;
    }
    const MulticamGroup* group =
        self.state->document.FindMulticamGroup(angle.group_id);
    if (group == nullptr) {
        return NO;
    }
    if (clear) {
        if (group->active_angle_id.empty()) {
            return NO;
        }
        operation = {group->id, {}};
        return YES;
    }
    if (group->active_angle_id == angle.active_angle_id) {
        return NO;
    }
    operation = angle;
    return YES;
}

- (void)applyMulticamSwitch:(BOOL)clear {
    SetMulticamActiveAngleOperation operation;
    if (![self multicamSwitchForSelection:clear operation:operation]) {
        self.infoLabel.stringValue =
            clear ? @"Le clip sélectionné n’est l’angle d’aucun groupe "
                    @"multicam ayant un angle actif"
                  : @"Sélectionnez un angle inactif d’un groupe multicam";
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation{operation}
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Changement d’angle refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = clear ? @"Multicam : tous les angles visibles"
                                       : @"Multicam : angle du clip activé";
    self.state->overlayDirty = true;
}

// SY2 -- docs/roadmap/SYNCHRO-2026-10.md. Resolve's Auto Align Clips, Based
// on Waveform (DaVinci Resolve 21.1 manual, chapter 45, pages 955-956): a
// submenu, as Resolve has one, whose single entry is the waveform; the
// timecode is not read (decision of 8 October 2026).
- (NSMenuItem*)autoAlignMenuItem {
    NSMenuItem* root =
        [[NSMenuItem alloc] initWithTitle:@"Aligner automatiquement les plans"
                                   action:nil
                            keyEquivalent:@""];
    NSMenu* submenu =
        [[NSMenu alloc] initWithTitle:@"Aligner automatiquement les plans"];
    NSMenuItem* waveform = [self menuItem:@"Selon la forme d’onde"
                                   action:@selector(menuAutoAlignByWaveform:)
                                      key:@""];
    waveform.toolTip =
        @"Cale le son de l’enregistreur sélectionné sous le plan image "
        @"sélectionné, le lie au plan et coupe le son caméra ; une "
        @"annulation retire tout";
    [submenu addItem:waveform];
    root.submenu = submenu;
    return root;
}

// The two sounds are measured off the main thread, as
// --measure-audio-offset measures them, on the editor's task manager: the
// task bar shows it and cancels it. The edit is built again on the
// document as it is when the measure answers, and applied as one undo.
// YES when the measure was started.
- (BOOL)alignSelectedSoundByWaveform {
    if (!self.state || !self.state->interaction) return NO;
    const auto refuse = [&](const std::string& reason) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Synchro impossible : %@.",
                                       CMStringFromUtf8(reason)];
        return NO;
    };
    if (self.state->soundAlignmentRunning)
        return refuse("une synchro est déjà en cours");
    std::string reason;
    const std::optional<ui::separate_sound::SoundPair> pair =
        ui::separate_sound::PickSoundPair(
            self.state->document, self.state->interaction->SelectedClipIds(),
            reason);
    if (!pair) return refuse(reason);
    const SeparateSoundPairFault fault = CheckSeparateSoundPair(
        self.state->document, pair->picture_clip_id, pair->sound_clip_id);
    if (fault != SeparateSoundPairFault::None)
        return refuse(ui::separate_sound::PairFaultText(fault));
    const std::string projectPath = self.documentPath.UTF8String ?: "";
    if (projectPath.empty())
        return refuse(
            ui::separate_sound::MeasureRefusalText("InvalidDocument", ""));
    Project project = self.state->project;
    std::string message;
    if (!project.CommitDocument(self.state->activeTimelineId,
                                self.state->document, message))
        return refuse("le projet n’a pas pu être lu (" + message + ")");
    const Ulid pictureSource =
        self.state->document.FindClip(pair->picture_clip_id)->source_id;
    const Ulid soundSource =
        self.state->document.FindClip(pair->sound_clip_id)->source_id;
    MediaTaskManager* tasks = self.state->mediaTasks.get();
    const MediaTaskRunner runner = [tasks](MediaTaskKind kind,
                                           std::string label,
                                           MediaTaskManager::Work work) {
        return RunMediaTaskAndWait(*tasks, kind, std::move(label),
                                   std::move(work));
    };
    self.state->soundAlignmentRunning = true;
    self.infoLabel.stringValue = @"Synchro : mesure du décalage entre le son "
                                 @"du plan et celui de l’enregistreur…";
    const ui::separate_sound::SoundPair chosen = *pair;
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string output;
      MeasureAudioOffsetCommand(projectPath, pictureSource, soundSource, {},
                                output, runner, &project);
      // Back by the run loop, as the setup notice comes back: a run loop
      // nested in a block of the main queue still takes it.
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        [weakSelf finishSoundAlignment:chosen measured:output];
      });
      CFRunLoopWakeUp(mainLoop);
    });
    [self refreshMediaTaskStatus];
    return YES;
}

- (void)finishSoundAlignment:(ui::separate_sound::SoundPair)pair
                    measured:(const std::string&)output {
    if (!self.state) return;
    self.state->soundAlignmentRunning = false;
    const auto refuse = [&](const std::string& reason) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Synchro refusée : %@. Rien n’a été posé.",
                             CMStringFromUtf8(reason)];
    };
    RationalTime offset;
    std::string code;
    std::string detail;
    if (!ui::separate_sound::ReadMeasuredOffset(output, offset, code, detail)) {
        refuse(ui::separate_sound::MeasureRefusalText(code, detail));
        return;
    }
    BatchOperation batch;
    SeparateSoundPlacement placement;
    EditError error = EditError::None;
    std::string message;
    if (!BuildSeparateSoundAlign(
            self.state->document,
            {pair.picture_clip_id, pair.sound_clip_id, offset, true}, batch,
            placement, error, message)) {
        refuse(ui::separate_sound::AlignRefusalText(error, message));
        return;
    }
    const std::string shift = ui::separate_sound::OffsetText(
        offset, self.state->document.sequence.frame_rate);
    if (batch.operations.empty()) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Le son est déjà calé sur le plan (décalage "
                             @"%@) et lié à lui : rien à faire.",
                             CMStringFromUtf8(shift)];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{batch}
                                          error:error
                                        message:message]) {
        refuse(ui::separate_sound::AlignRefusalText(error, message));
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    NSString* muted = @"";
    if (!placement.muted_track_id.empty())
        muted = [NSString stringWithFormat:@", piste %@ du son caméra coupée",
                                           CMStringFromUtf8(TrackDisplayName(
                                               self.state->document,
                                               placement.muted_track_id))];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Son de l’enregistreur calé sous le plan (décalage "
                         @"mesuré %@), lié%@. ⌘Z annule tout.",
                         CMStringFromUtf8(shift), muted];
    self.state->overlayDirty = true;
}

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. Command-R shows the selected
// clips' Retime controls, or hides them when all of them show theirs
// (Resolve 21.1 manual, p. 1218-1222): a band along the top of each clip
// with its speed, whose ends stretch it (RateStretchOperation).
- (void)toggleRetimeControls {
    std::vector<Ulid> selected = self.state->interaction->SelectedClipIds();
    if (selected.empty()) {
        self.infoLabel.stringValue =
            @"Contrôles de vitesse : sélectionnez un plan";
        return;
    }
    std::vector<Ulid>& shown = self.state->retimeClipIds;
    const bool all =
        std::all_of(selected.begin(), selected.end(), [&](const Ulid& id) {
            return std::find(shown.begin(), shown.end(), id) != shown.end();
        });
    if (all) {
        shown.erase(std::remove_if(shown.begin(), shown.end(),
                                   [&](const Ulid& id) {
                                       return std::find(selected.begin(),
                                                        selected.end(),
                                                        id) != selected.end();
                                   }),
                    shown.end());
        self.infoLabel.stringValue = @"Contrôles de vitesse masqués";
    } else {
        for (const Ulid& id : selected)
            if (std::find(shown.begin(), shown.end(), id) == shown.end())
                shown.push_back(id);
        self.infoLabel.stringValue =
            @"VITESSE · glisser un bout de la bande pour étirer le plan";
    }
    self.state->overlayDirty = true;
}

- (void)removeContextTrackPressed:(id)sender {
    (void)sender;
    const DocumentTrack* track =
        self.state->document.FindTrack(self.state->contextTrackId);
    if (!track) return;
    if (!track->clips.empty()) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"Supprimer cette piste et tous ses clips ?";
        alert.informativeText = [NSString
            stringWithFormat:@"La piste contient %lu clip%@. Cette action "
                             @"reste annulable avec ⌘Z.",
                             (unsigned long)track->clips.size(),
                             track->clips.size() == 1 ? @"" : @"s"];
        [alert addButtonWithTitle:@"Supprimer la piste"];
        [alert addButtonWithTitle:@"Annuler"];
        if ([alert runModal] != NSAlertFirstButtonReturn) return;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveTrackOperation { track->id }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)removeEmptyTracksPressed:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    std::vector<Ulid> emptyTrackIds;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.clips.empty()) emptyTrackIds.push_back(track.id);
    if (emptyTrackIds.empty()) return;

    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    for (const Ulid& trackId : emptyTrackIds) {
        if (!stagedLog.Apply(stagedDocument,
                             Operation{RemoveTrackOperation{trackId}}, error,
                             message)) {
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Nettoyage refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
    }
    const RationalTime playhead = self.state->requestedPosition;
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%lu piste%@ vide%@ supprimée%@",
                                   (unsigned long)emptyTrackIds.size(),
                                   emptyTrackIds.size() == 1 ? @"" : @"s",
                                   emptyTrackIds.size() == 1 ? @"" : @"s",
                                   emptyTrackIds.size() == 1 ? @"" : @"s"];
    self.state->overlayDirty = true;
}

- (void)contextOpenClipSource:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (clip)
        [self openMediaIdentifierInSourceMonitor:
                  [NSString stringWithUTF8String:clip->source_id.c_str()]];
}

- (void)contextCutClip:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip || self.state->contextTime <= clip->timeline_in ||
        self.state->contextTime >= clip->timeline_in.add(clip->duration))
        return;
    Operation operation = TimelineCutOperationForClip(
        self.state->document, *clip, self.state->contextTime,
        self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self updateSelectionInfo];
    }
}

- (void)contextDeleteSelection:(id)sender {
    (void)sender;
    [self deleteCurrentTimelineSelection];
}

- (void)contextCloseGap:(id)sender {
    (void)sender;
    if (!self.state->contextGap) return;
    const TimelineGapSelection gap = *self.state->contextGap;
    Operation operation = GapDeleteOperationForSelection(
        self.state->document, gap, self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        self.state->interaction->SelectClip("");
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self updateSelectionInfo];
    }
}

@end
