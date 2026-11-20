#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The settings
// a person reaches from the window: the AI engine, the Whisper model and
// the hesitations' weights, the setup diagnostic, the sequence and its
// colour management. SetupCheck.h and SetupNotice.h decide what is said.
// Moved out of main.mm unchanged.

@interface AppDelegate (Settings)
- (void)configureAiEngine:(id)sender;
- (NSString*)requestWhisperModelPath;
- (void)chooseWhisperModel:(id)sender;
- (NSString*)transcriptionModelPath;
- (void)migrateWhisperModelPreference;
- (void)gatherSetupNotice:(void (^)(const SetupNoticeResult& result))completion;
- (void)showSetupDiagnostic:(id)sender;
- (CMFormPanel*)setupDiagnosticPanel:(const SetupNoticeResult&)result;
- (void)copySetupDiagnostic:(id)sender;
- (BOOL)writeSetupDiagnosticToPasteboard:(NSPasteboard*)pasteboard;
- (BOOL)commitColorSettings:(const ColorManagementSettings&)settings;
- (void)configureSequence:(id)sender;
- (void)disableColorManagement:(id)sender;
- (void)configureColorManagement:(id)sender;
- (BOOL)hesitationsWeightsAllowed;
- (void)showHesitationsRefusal:(const std::string&)detail;
@end
