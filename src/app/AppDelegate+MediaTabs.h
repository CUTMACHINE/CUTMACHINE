#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The media
// panel's « Audio » and « Légendes » (captions) tabs (F2.3): the tab switch,
// the audio list and its search, and the caption styles' list, controls and
// buttons. Moved out of main.mm unchanged.

@interface AppDelegate (MediaTabs)
- (void)mediaTabChanged:(CMTabStripView*)sender;
- (void)buildAudioTabContentWithWidth:(double)width height:(double)height;
- (void)audioSearchChanged:(id)sender;
- (void)rebuildAudioList;
- (NSString*)selectedAudioId;
- (NSArray<NSString*>*)selectedAudioIds;
- (void)openSelectedAudioInSourceMonitor:(id)sender;
- (void)assignSelectedAudioToBinPressed:(id)sender;
- (void)layoutCaptionStyleColumns;
- (void)buildCaptionsTabContentWithWidth:(double)width height:(double)height;
- (void)rebuildCaptionStylesList;
- (void)updateCaptionStyleControls;
- (void)captionLookChanged:(id)sender;
- (void)updateCaptionSelectionLabel;
- (Ulid)selectedCaptionStyleId;
- (void)createCaptionStylePressed:(id)sender;
- (void)deleteCaptionStylePressed:(id)sender;
- (void)restyleSelectedCartons:(const Ulid&)styleId;
- (void)applyCaptionStylePressed:(id)sender;
- (void)clearCaptionPressed:(id)sender;
@end
