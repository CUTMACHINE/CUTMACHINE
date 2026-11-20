// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The
// Inspector's delegate (F2.2): each commit and preview of a clip's effects,
// framing, keys, opacity, blur, sound and carton text, the caption style,
// the grade copied and pasted, a media's colour input, and the sequence's
// look. InspectorView.mm draws; these methods turn each gesture into an
// operation. Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (Inspector)

- (void)applySonyColorPreset:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    settings.input_ycbcr_matrix = "bt709";
    settings.input_range = "full";
    settings.working_gamut = "acescct";
    settings.output_gamut = "rec2020";
    settings.output_transfer = "hlg";
    [self commitColorSettings:settings];
}

// G3 -- docs/roadmap/GRADE-2026-09.md. One undo step for every selected
// media that has a picture and reads otherwise.
- (void)applyMediaColorInputs:
    (const std::vector<std::pair<Ulid, MediaColorInput>>&)inputs {
    const std::optional<Operation> operation =
        ui::media_panel::BuildSetMediaColorInputs(self.state->document, inputs);
    if (!operation) return;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:*operation
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Entrée couleur refusée : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
        return;
    }
    self.state->overlayDirty = true;
}

- (void)setSelectedMediaColorInput:(NSMenuItem*)sender {
    const auto& choices = ui::media_panel::ColorInputChoices();
    if (sender.tag < 0 || static_cast<size_t>(sender.tag) >= choices.size())
        return;
    std::vector<std::pair<Ulid, MediaColorInput>> inputs;
    for (NSString* identifier in [self selectedMediaIds])
        inputs.emplace_back(identifier.UTF8String ?: "",
                            choices[static_cast<size_t>(sender.tag)].input);
    [self applyMediaColorInputs:inputs];
}

- (void)rereadSelectedMediaColorInput:(id)sender {
    (void)sender;
    const std::filesystem::path projectDirectory =
        std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""))
            .parent_path();
    std::vector<std::pair<Ulid, MediaColorInput>> inputs;
    for (NSString* identifier in [self selectedMediaIds]) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
        if (!media || (media->metadata_complete && !media->has_video)) continue;
        std::filesystem::path path(media->path);
        if (path.is_relative()) path = projectDirectory / path;
        MediaFileColor color;
        std::string reason;
        if (!ReadFileColorFacts(path.lexically_normal().string(), color,
                                reason)) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Lecture impossible : %@",
                                           CMStringFromUtf8(reason)];
            NSBeep();
            return;
        }
        // A file that declares nothing leaves the media as it is, as
        // set_media_color_input's from_file does.
        if (DeclaresColorInput(color.input))
            inputs.emplace_back(media->id, color.input);
    }
    [self applyMediaColorInputs:inputs];
}

// F2.2 -- CMInspectorViewDelegate. Every grading slider commit arrives
// here as a ready-to-apply SetClipEffectsOperation; this is the only place
// that actually calls EditLog::Apply for it, matching every other edit in
// this file (e.g. -menuAddCrossDissolve: above) -- CMInspectorView itself
// never touches Document/DocumentClip directly (PHILOSOPHY.md principle
// 2/3). No refreshTimelineAfterEditFromPosition here: a grade change
// leaves every clip's position/duration untouched, it only changes what
// the next render composites (see main.mm's ResolveColorGrade call site),
// so marking overlayDirty is the only redraw trigger this edit needs.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipEffects:(SetClipEffectsOperation)operation {
    // Applied or refused, the monitor shows the document again.
    self.state->gradePreview.reset();
    self.state->overlayDirty = true;
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    if (!applied)
        std::fprintf(stderr, "Grading edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    // PRD37 -- ROADMAP.md. And so does the panel, which this edit came from:
    // its reset button, wheels, curves and sliders show the stack the
    // document holds now (QA of 3 October 2026: the reset left off on a
    // graded clip, a master bar left in the middle under a typed 1.50). Not
    // -updateSelectionInfo: its full reload drops the commits other sliders
    // still have pending.
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    self.state->overlayDirty = true;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipEffects:(const std::vector<ClipEffect>&)effects
               forClip:(const Ulid&)clipId {
    (void)inspectorView;
    self.state->gradePreview = ui::inspector::GradePreview{clipId, effects};
    self.state->overlayDirty = true;
}

// The author, 29 October 2026 -- zoom, position, fit or fill from the
// Inspector: one SetClipFramingOperation per released drag, the operation
// set_clip_framing applies.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipFraming:(SetClipFramingOperation)operation {
    self.state->framingPreview.reset();
    self.state->overlayDirty = true;
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Cadrage refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipFraming:(const ClipFraming&)framing
               forClip:(const Ulid&)clipId {
    (void)inspectorView;
    self.state->framingPreview =
        ui::inspector::FramingPreview{clipId, framing, std::nullopt};
    self.state->overlayDirty = true;
}

// R2c -- docs/roadmap/REEL-2026-09.md. A key posed or removed by the
// Inspector's diamond, or a keyed row's slider let go: SetClipAnimation, or
// with the last key a batch that keeps its value in the field. One undo.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipKeys:(Operation)operation
                label:(NSString*)label {
    self.state->framingPreview.reset();
    self.state->overlayDirty = true;
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Clé refusée : %@", CMStringFromUtf8(message)];
        return;
    }
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Inspecteur : %@", label];
    [self requestResolvedPosition:self.state->requestedPosition];
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipAnimation:(const std::vector<ClipParameterAnimation>&)animation
                 forClip:(const Ulid&)clipId {
    (void)inspectorView;
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    if (!clip) return;
    self.state->framingPreview =
        ui::inspector::FramingPreview{clipId, clip->framing, animation};
    self.state->overlayDirty = true;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
          moveToFrame:(int64_t)localFrame
               ofClip:(const Ulid&)clipId {
    (void)inspectorView;
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    const MediaRate rate = self.state->document.sequence.frame_rate;
    if (!clip || rate.num <= 0 || rate.den <= 0) return;
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:clip->timeline_in.add(RationalTime(
                                      localFrame * rate.den, rate.num))];
    self.state->overlayDirty = true;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipOpacity:(SetClipOpacityOperation)operation {
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    // PRD37 -- the panel shows what the document holds, applied or not.
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        std::fprintf(stderr, "Opacity edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. The Inspector's « Flou », applied
// as the opacity is: Record composites the blur on its next render.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipBlur:(SetClipBlurOperation)operation {
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        std::fprintf(stderr, "Blur edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Flou refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
}

// Out of lot -- the author, 18 November 2026 (GitHub #330). A gain or a fade
// set in the Inspector's « Son » section: one SetClipAudioOperation, one undo.
// The timeline refresh rebuilds what playback mixes, so the next frame heard
// is the new one, and redraws the clip's fades and its gain badge.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipAudio:(SetClipAudioOperation)operation {
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    if (applied) [self refreshTimelineAfterEditFromPosition:playhead];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        std::fprintf(stderr, "Clip audio edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Réglage du son refusé : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->overlayDirty = true;
}

// F2.3 -- ROADMAP.md. The style of a carton, set in the Inspector: one
// UpdateCaptionStyleOperation per gesture, built and checked by the panel's
// policy (RestyleCaptionStyleWhole). Every carton of the style follows, on
// Record and in the Captions tab's list; one undo takes it back.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitCaptionStyle:(UpdateCaptionStyleOperation)operation {
    (void)inspectorView;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                          error:error
                                        message:message]) {
        // The panel shows the style the document still holds.
        [self updateSelectionInfo];
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Style refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
    self.infoLabel.stringValue = @"Style de légende modifié : tous ses cartons "
                                 @"suivent. ⌘Z l’annule.";
}

// GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The selected
// shot's whole stack, held by the window until it is pasted.
- (void)copyClipGrade:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (!self.state || !self.state->interaction) return;
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    const DocumentClip* clip =
        selected.size() == 1 ? self.state->document.FindClip(selected.front())
                             : nullptr;
    if (clip == nullptr ||
        !ClipGradeRefusal(self.state->document, *clip).empty()) {
        NSBeep();
        return;
    }
    self.state->gradeClipboard = clip->effects;
    self.infoLabel.stringValue =
        clip->effects.empty()
            ? @"Étalonnage copié : aucun étage (le coller remet à zéro)"
            : [NSString
                  stringWithFormat:@"Étalonnage copié : %lu étage%@",
                                   (unsigned long)clip->effects.size(),
                                   clip->effects.size() == 1 ? @"" : @"s"];
}

// The copied stack on every selected shot that carries a grade, in one
// undo step; the others are left and counted.
- (void)pasteClipGrade:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (!self.state || !self.state->interaction ||
        !self.state->gradeClipboard) {
        NSBeep();
        return;
    }
    ClipGradePaste paste;
    std::string message;
    if (!BuildClipGradePasteOfStack(
            self.state->document, *self.state->gradeClipboard,
            self.state->interaction->SelectedClipIds(), paste, message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Étalonnage non collé : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
        return;
    }
    if (paste.operation) {
        EditError error = EditError::None;
        if (![self applyAndPersistTimelineOperation:*paste.operation
                                              error:error
                                            message:message]) {
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Étalonnage refusé : %@",
                                           CMStringFromUtf8(message)];
            NSBeep();
            return;
        }
        self.state->overlayDirty = true;
        [self updateSelectionInfo];
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:
            @"Étalonnage collé sur %lu plan%@%@",
            (unsigned long)paste.pasted.size(),
            paste.pasted.size() == 1 ? @"" : @"s",
            paste.skipped.empty()
                ? @""
                : [NSString
                      stringWithFormat:@" (%lu sans étalonnage possible)",
                                       (unsigned long)paste.skipped.size()]];
}

// The Inspector's three calls. A text typed there is one edit; the walk
// from carton to carton selects and sends the playhead, so that Record
// shows the carton being read; Escape hands the keys back to the timeline.
- (BOOL)inspectorView:(CMInspectorView*)inspectorView
    didCommitCartonText:(const std::string&)typed
                forClip:(const Ulid&)clipId {
    (void)inspectorView;
    return [self setCartonText:typed forClip:clipId];
}

- (BOOL)inspectorView:(CMInspectorView*)inspectorView
    selectCartonAdjacentTo:(const Ulid&)clipId
                   forward:(BOOL)forward {
    (void)inspectorView;
    const auto neighbour =
        ui::media_panel::AdjacentCarton(self.state->document, clipId, forward);
    if (!neighbour) {
        self.infoLabel.stringValue = forward ? @"Dernier carton de la piste"
                                             : @"Premier carton de la piste";
        return NO;
    }
    const Ulid carton = *neighbour;
    const DocumentClip* clip = self.state->document.FindClip(carton);
    if (!clip) return NO;
    [self setPlaybackDirection:0];
    self.state->interaction->SelectClip(carton);
    [self requestTimelinePosition:clip->timeline_in];
    [self updateSelectionInfo];
    [self showCartonInfo:carton];
    self.state->overlayDirty = true;
    return YES;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewCartonText:(const std::string&)typed
              forClip:(const Ulid&)clipId {
    (void)inspectorView;
    if (clipId.empty())
        self.state->cartonPreview.reset();
    else
        self.state->cartonPreview =
            ui::media_panel::CartonTextPreview{clipId, typed};
    self.state->overlayDirty = true;
}

- (void)inspectorViewDidLeaveCartonText:(CMInspectorView*)inspectorView {
    (void)inspectorView;
    [self.window makeFirstResponder:self.metalView];
}

- (void)refreshSequenceLookLut {
    if (!self.state) return;
    const SequenceLut& look = self.state->document.sequence.look_lut;
    if (look.digest == self.state->lookLut.digest) return;
    self.state->lookLut = LoadedLookLut{};
    if (look.digest.empty()) return;
    const std::string projectDirectory =
        self.documentPath.length == 0
            ? std::string()
            : std::filesystem::path(self.documentPath.UTF8String)
                  .parent_path()
                  .string();
    LoadedLookLut loaded;
    std::string error;
    if (projectDirectory.empty() ||
        !LoadSequenceLookLut(self.state->document.sequence,
                             self.state->document.color_management,
                             projectDirectory, loaded, error)) {
        if (self.state->reportedLookLuts.insert(look.digest).second) {
            std::fprintf(stderr, "Unable to read the sequence look LUT: %s\n",
                         projectDirectory.empty()
                             ? "the project has no package on disk yet"
                             : error.c_str());
        }
        return;
    }
    self.state->lookLut = std::move(loaded);
}

@end
