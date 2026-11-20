#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The
// Inspector's delegate (F2.2): each commit and preview of a clip's effects,
// framing, keys, opacity, blur, sound and carton text, the caption style,
// the grade copied and pasted, a media's colour input, and the sequence's
// look. InspectorView.mm draws; these methods turn each gesture into an
// operation. Moved out of main.mm unchanged.

@interface AppDelegate (Inspector) <CMInspectorViewDelegate>
- (void)applySonyColorPreset:(id)sender;
- (void)applyMediaColorInputs:
    (const std::vector<std::pair<Ulid, MediaColorInput>>&)inputs;
- (void)setSelectedMediaColorInput:(NSMenuItem*)sender;
- (void)rereadSelectedMediaColorInput:(id)sender;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipEffects:(SetClipEffectsOperation)operation;
- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipEffects:(const std::vector<ClipEffect>&)effects
               forClip:(const Ulid&)clipId;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipFraming:(SetClipFramingOperation)operation;
- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipFraming:(const ClipFraming&)framing
               forClip:(const Ulid&)clipId;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipKeys:(Operation)operation
                label:(NSString*)label;
- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipAnimation:(const std::vector<ClipParameterAnimation>&)animation
                 forClip:(const Ulid&)clipId;
- (void)inspectorView:(CMInspectorView*)inspectorView
          moveToFrame:(int64_t)localFrame
               ofClip:(const Ulid&)clipId;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipOpacity:(SetClipOpacityOperation)operation;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipBlur:(SetClipBlurOperation)operation;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipAudio:(SetClipAudioOperation)operation;
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitCaptionStyle:(UpdateCaptionStyleOperation)operation;
- (void)copyClipGrade:(id)sender;
- (void)pasteClipGrade:(id)sender;
- (BOOL)inspectorView:(CMInspectorView*)inspectorView
    didCommitCartonText:(const std::string&)typed
                forClip:(const Ulid&)clipId;
- (BOOL)inspectorView:(CMInspectorView*)inspectorView
    selectCartonAdjacentTo:(const Ulid&)clipId
                   forward:(BOOL)forward;
- (void)inspectorView:(CMInspectorView*)inspectorView
    previewCartonText:(const std::string&)typed
              forClip:(const Ulid&)clipId;
- (void)inspectorViewDidLeaveCartonText:(CMInspectorView*)inspectorView;
- (void)refreshSequenceLookLut;
@end
