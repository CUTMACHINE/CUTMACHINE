// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The settings
// a person reaches from the window: the AI engine, the Whisper model and
// the hesitations' weights, the setup diagnostic, the sequence and its
// colour management. SetupCheck.h and SetupNotice.h decide what is said.
// Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (Settings)

- (void)configureAiEngine:(id)sender {
    (void)sender;
    [self.chatPanelView showConfigurationWindow];
}

// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. The model chosen here is
// the engine's setting, written in the settings file: the agents and the
// command line use it too. The status line says so, or why it is used for
// this transcription only (WhisperModelSetting.h).
- (NSString*)requestWhisperModelPath {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Choisir un modèle Whisper local";
    panel.message =
        @"Sélectionnez un modèle whisper.cpp au format GGML (.bin). "
         "Ce choix est écrit dans ~/.config/cutmachine/.env : les agents et "
         "la ligne de commande s'en servent aussi.";
    panel.prompt = @"Utiliser ce modèle";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.resolvesAliases = YES;
    panel.allowedContentTypes = @[ UTTypeData ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return nil;
    NSString* modelPath = panel.URL.path;
    if (modelPath.length == 0) return nil;
    const whisper_model_setting::Choice choice =
        whisper_model_setting::SaveChosenModel(modelPath.UTF8String ?: "");
    self.binSummaryLabel.stringValue =
        CMStringFromUtf8(whisper_model_setting::ChoiceStatus(choice));
    return modelPath;
}

- (void)chooseWhisperModel:(id)sender {
    (void)sender;
    [self requestWhisperModelPath];
}

// DEC23 -- the model the transcription menus use: the engine's, resolved as
// every tool resolves it; the panel when there is none, or none there.
- (NSString*)transcriptionModelPath {
    std::string path;
    std::string reason;
    if (ResolveConfiguredWhisperModel(path, reason))
        return CMStringFromUtf8(path);
    return [self requestWhisperModelPath];
}

// DEC23 -- the window's old preference, migrated once into the settings
// file, then removed; kept when the file cannot be written, for the next
// launch to try again.
- (void)migrateWhisperModelPreference {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults objectForKey:kWhisperModelPathDefaultsKey] == nil) return;
    NSString* preference = [defaults stringForKey:kWhisperModelPathDefaultsKey];
    const whisper_model_setting::Migration migration =
        whisper_model_setting::MigrateWindowPreference(preference.UTF8String
                                                           ?: "");
    if (migration.forget_preference)
        [defaults removeObjectForKey:kWhisperModelPathDefaultsKey];
}

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. The report read again
// (the window keeps none), off the main thread. `completion` runs on the
// main run loop, scheduled there rather than on the main queue, in the modes
// of the editor and of a modal session: it must land while the welcome
// screen's session runs, and while the smoke run holds the main queue (see
// -checkSourceAvailabilityInBackground). Not during event tracking: the
// dialog it may open waits for a menu to close.
- (void)gatherSetupNotice:
    (void (^)(const SetupNoticeResult& result))completion {
    void (^done)(const SetupNoticeResult&) = [completion copy];
    static NSArray* const modes =
        @[ NSDefaultRunLoopMode, NSModalPanelRunLoopMode ];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      auto result = std::make_shared<SetupNoticeResult>(GatherSetupNotice());
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, (__bridge CFTypeRef)modes, ^{
        done(*result);
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

// « CUTMACHINE ▸ Diagnostic de l'installation… », and « Diagnostic… » on the
// welcome screen: the whole report, in French (SetupNotice.h).
- (void)showSetupDiagnostic:(id)sender {
    (void)sender;
    if (self.setupDiagnosticPending) return;
    // PRD26 -- a form already on the window refuses this one, which would
    // first replace what « Copier le diagnostic » copies.
    if ([CMFormPanel formShownInWindow:self.window]) {
        NSBeep();
        return;
    }
    self.setupDiagnosticPending = YES;
    __weak AppDelegate* weakSelf = self;
    [self gatherSetupNotice:^(const SetupNoticeResult& result) {
      AppDelegate* strongSelf = weakSelf;
      if (!strongSelf) return;
      strongSelf.setupDiagnosticPending = NO;
      // PRD26 -- a sheet on the editor window; modal on the welcome screen,
      // which has none. A form opened while the report was gathered keeps
      // the window.
      if ([CMFormPanel formShownInWindow:strongSelf.window]) {
          NSBeep();
          return;
      }
      __weak AppDelegate* weakAgain = strongSelf;
      [[strongSelf setupDiagnosticPanel:result]
          presentInWindow:strongSelf.window
               completion:^(NSModalResponse response) {
                 (void)response;
                 weakAgain.setupDiagnosticClipboard = nil;
               }];
    }];
}

// The dialog, built and not shown: the window test reads it the same way.
// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The window's form panel in
// place of an alert, resizable: the text takes the room. Its colours are the
// theme's now, the panel's appearance dark to match (FormPanel.h).
- (CMFormPanel*)setupDiagnosticPanel:(const SetupNoticeResult&)result {
    using namespace ui::theme;
    const NSRect frame = NSMakeRect(0, 0, 560, 320);
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    const NSSize content = [NSScrollView
        contentSizeForFrameSize:frame.size
        horizontalScrollerClass:nil
          verticalScrollerClass:NSScroller.class
                     borderType:NSBezelBorder
                    controlSize:NSControlSizeRegular
                  scrollerStyle:NSScroller.preferredScrollerStyle];
    NSTextView* text = [[NSTextView alloc]
        initWithFrame:NSMakeRect(0, 0, content.width, content.height)];
    text.identifier = @"setup.diagnostic.text";
    text.editable = NO;
    text.selectable = YES;
    text.drawsBackground = NO;
    text.textContainerInset = NSMakeSize(kSpaceS, kSpaceS);
    text.verticallyResizable = YES;
    text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    NSDictionary* plain = @{
        NSFontAttributeName : CMFont(kFontSizeBody, NSFontWeightRegular),
        NSForegroundColorAttributeName : CMTextPrimaryColor()
    };
    NSDictionary* heading = @{
        NSFontAttributeName : CMFont(kFontSizeSection, NSFontWeightSemibold),
        NSForegroundColorAttributeName : CMTextPrimaryColor()
    };
    NSMutableAttributedString* body = [NSMutableAttributedString new];
    for (const setup_notice::Run& run :
         setup_notice::DiagnosticRuns(result.report, result.facts))
        [body
            appendAttributedString:[[NSAttributedString alloc]
                                       initWithString:CMStringFromUtf8(run.text)
                                           attributes:run.heading ? heading
                                                                  : plain]];
    [text.textStorage setAttributedString:body];
    scroll.documentView = text;
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Diagnostic de l'installation"
              message:@"Ce dont CUTMACHINE dispose sur cette machine, relu à "
                      @"l'instant. « Copier le diagnostic » met ce texte dans "
                      @"le presse-papiers, suivi du rapport du moteur "
                      @"(cutmachine --check-setup)."
                 form:scroll];
    [panel addButtonWithTitle:@"Fermer" role:CMFormButtonRoleAccept];
    NSButton* copy = [panel addButtonWithTitle:@"Copier le diagnostic"
                                          role:CMFormButtonRoleAction];
    copy.identifier = @"setup.diagnostic.copy";
    // Its own action, not the panel's: the dialog stays open once copied.
    copy.target = self;
    copy.action = @selector(copySetupDiagnostic:);
    panel.formResizable = YES;
    self.setupDiagnosticClipboard = CMStringFromUtf8(
        setup_notice::ClipboardText(result.report, result.facts));
    return panel;
}

- (void)copySetupDiagnostic:(id)sender {
    if (![self writeSetupDiagnosticToPasteboard:NSPasteboard.generalPasteboard])
        return;
    if ([sender isKindOfClass:NSButton.class])
        ((NSButton*)sender).title = @"Diagnostic copié";
}

- (BOOL)writeSetupDiagnosticToPasteboard:(NSPasteboard*)pasteboard {
    NSString* text = self.setupDiagnosticClipboard;
    if (text.length == 0) return NO;
    [pasteboard clearContents];
    return [pasteboard setString:text forType:NSPasteboardTypeString];
}

- (BOOL)commitColorSettings:(const ColorManagementSettings&)settings {
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            SetColorManagementOperation { settings }
        }
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Unable to apply color settings (%s): %s\n",
                     EditErrorName(error), message.c_str());
        NSBeep();
        return NO;
    }
    self.state->overlayDirty = true;
    return YES;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel.
// The operation is applied before the panel closes: refused, the panel
// stays open with the engine's reason, where a second alert -- titled
// « Export impossible » -- used to say it once the form was gone.
- (void)configureSequence:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (!self.state) return;
    const DocumentSequence current = self.state->document.sequence;
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 108)];

    NSTextField* nameLabel = [NSTextField labelWithString:@"Nom :"];
    nameLabel.frame = NSMakeRect(0, 79, 120, 22);
    nameLabel.alignment = NSTextAlignmentRight;
    [accessory addSubview:nameLabel];
    NSTextField* name =
        [[NSTextField alloc] initWithFrame:NSMakeRect(132, 76, 316, 24)];
    name.identifier = @"sequence.name";
    name.stringValue = [NSString stringWithUTF8String:current.name.c_str()];
    [accessory addSubview:name];

    auto addPopup = [&](NSString* labelText, NSArray<NSString*>* choices,
                        CGFloat y) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3, 120, 22);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* menu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(132, y, 316, 26)
                                       pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };
    NSString* currentFormat = [NSString
        stringWithFormat:@"Actuel — %d × %d", current.width, current.height];
    NSPopUpButton* format =
        addPopup(@"Dimensions :",
                 @[
                     currentFormat, @"HD — 1920 × 1080", @"UHD — 3840 × 2160",
                     @"Vertical HD — 1080 × 1920", @"Carré — 1080 × 1080"
                 ],
                 44);
    NSString* currentRate = [NSString stringWithFormat:@"Actuelle — %d/%d i/s",
                                                       current.frame_rate.num,
                                                       current.frame_rate.den];
    NSPopUpButton* cadence =
        addPopup(@"Cadence :",
                 @[
                     currentRate, @"23,976 i/s", @"24 i/s", @"25 i/s",
                     @"29,97 i/s", @"50 i/s", @"59,94 i/s"
                 ],
                 12);
    format.identifier = @"sequence.format";
    cadence.identifier = @"sequence.rate";
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Réglages de séquence"
              message:@"Le format de séquence pilote le moniteur et les "
                      @"presets d’export."
                 form:accessory];
    [panel addButtonWithTitle:@"Appliquer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = name;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      UpdateSequenceOperation updated;
      updated.sequence_id = current.id;
      updated.name = name.stringValue.UTF8String ?: "Sequence 1";
      updated.width = current.width;
      updated.height = current.height;
      updated.frame_rate = current.frame_rate;
      switch (format.indexOfSelectedItem) {
          case 1:
              updated.width = 1920;
              updated.height = 1080;
              break;
          case 2:
              updated.width = 3840;
              updated.height = 2160;
              break;
          case 3:
              updated.width = 1080;
              updated.height = 1920;
              break;
          case 4:
              updated.width = 1080;
              updated.height = 1080;
              break;
          default:
              break;
      }
      switch (cadence.indexOfSelectedItem) {
          case 1:
              updated.frame_rate = {24000, 1001};
              break;
          case 2:
              updated.frame_rate = {24, 1};
              break;
          case 3:
              updated.frame_rate = {25, 1};
              break;
          case 4:
              updated.frame_rate = {30000, 1001};
              break;
          case 5:
              updated.frame_rate = {50, 1};
              break;
          case 6:
              updated.frame_rate = {60000, 1001};
              break;
          default:
              break;
      }
      EditError editError = EditError::None;
      std::string message;
      if (![self applyAndPersistTimelineOperation:Operation{std::move(updated)}
                                            error:editError
                                          message:message])
          return message.empty() ? @"Réglages refusés par le moteur."
                                 : CMStringFromUtf8(message);
      return nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self refreshTimelineAfterEditFromPosition:self.state->
                                                             requestedPosition];
                  [self rebuildMediaList];
                  self.state->overlayDirty = true;
                }];
}

- (void)disableColorManagement:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    ColorManagementSettings settings = self.state->document.color_management;
    settings.enabled = false;
    [self commitColorSettings:settings];
}

- (void)configureColorManagement:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    const ColorManagementSettings& current =
        self.state->document.color_management;

    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 500, 264)];
    NSButton* enabled = [NSButton checkboxWithTitle:@"Activer pour ce projet"
                                             target:nil
                                             action:nil];
    enabled.identifier = @"color.enabled";
    enabled.frame = NSMakeRect(0, 236, 500, 24);
    enabled.state =
        current.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [accessory addSubview:enabled];

    CGFloat y = 202.0;
    auto addPopup = [&](NSString* labelText,
                        std::initializer_list<std::pair<NSString*, NSString*>>
                            choices,
                        const std::string& selected) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3.0, 175, 20);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* popup =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(188, y, 300, 26)
                                       pullsDown:NO];
        for (const auto& choice : choices) {
            [popup addItemWithTitle:choice.first];
            popup.lastItem.representedObject = choice.second;
            if ([choice.second
                    isEqualToString:[NSString
                                        stringWithUTF8String:selected.c_str()]])
                [popup selectItem:popup.lastItem];
        }
        [accessory addSubview:popup];
        y -= 32.0;
        return popup;
    };
    NSPopUpButton* inputGamut =
        addPopup(@"Gamut d’entrée :",
                 {{@"Sony S-Gamut3.Cine", @"sony_sgamut3_cine"},
                  {@"Sony S-Gamut3", @"sony_sgamut3"},
                  {@"Rec.709", @"rec709"},
                  {@"Rec.2020", @"rec2020"}},
                 current.input_gamut);
    NSPopUpButton* inputTransfer = addPopup(@"Courbe d’entrée :",
                                            {{@"Sony S-Log3", @"sony_slog3"},
                                             {@"Rec.709", @"rec709"},
                                             {@"HLG", @"hlg"},
                                             {@"Linéaire", @"linear"}},
                                            current.input_transfer);
    NSPopUpButton* ycbcr = addPopup(@"Matrice YCbCr :",
                                    {{@"Auto (métadonnées)", @"auto"},
                                     {@"BT.709", @"bt709"},
                                     {@"BT.2020 non constante", @"bt2020_ncl"}},
                                    current.input_ycbcr_matrix);
    NSPopUpButton* inputRange = addPopup(@"Plage du signal :",
                                         {{@"Auto (métadonnées)", @"auto"},
                                          {@"Full / Extended", @"full"},
                                          {@"Legal / Limited", @"limited"}},
                                         current.input_range);
    NSPopUpButton* working = addPopup(@"Espace de travail :",
                                      {{@"ACEScct (AP1)", @"acescct"},
                                       {@"Rec.2020 linéaire", @"rec2020"},
                                       {@"Rec.709 linéaire", @"rec709"}},
                                      current.working_gamut);
    NSPopUpButton* outputGamut =
        addPopup(@"Gamut de sortie :",
                 {{@"Rec.2020", @"rec2020"}, {@"Rec.709", @"rec709"}},
                 current.output_gamut);
    NSPopUpButton* outputTransfer = addPopup(
        @"Courbe de sortie :", {{@"HLG", @"hlg"}, {@"Rec.709", @"rec709"}},
        current.output_transfer);
    inputGamut.identifier = @"color.input_gamut";
    inputTransfer.identifier = @"color.input_transfer";
    ycbcr.identifier = @"color.ycbcr_matrix";
    inputRange.identifier = @"color.input_range";
    working.identifier = @"color.working_gamut";
    outputGamut.identifier = @"color.output_gamut";
    outputTransfer.identifier = @"color.output_transfer";

    const auto chosen = [=]() {
        const auto value = [](NSPopUpButton* popup) {
            NSString* string = popup.selectedItem.representedObject;
            return std::string(string.UTF8String ?: "");
        };
        ColorManagementSettings settings;
        settings.enabled = enabled.state == NSControlStateValueOn;
        settings.input_gamut = value(inputGamut);
        settings.input_transfer = value(inputTransfer);
        settings.input_ycbcr_matrix = value(ycbcr);
        settings.input_range = value(inputRange);
        settings.working_gamut = value(working);
        settings.output_gamut = value(outputGamut);
        settings.output_transfer = value(outputTransfer);
        return settings;
    };
    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel;
    // the HLG rule is checked before it closes, where a second alert,
    // « Configuration incompatible », said it once the form was gone.
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Gestion colorimétrique du projet"
              message:@"Les transformations sont calculées en linéaire dans "
                      @"l’espace de composition AP1, avec ACEScct pour le "
                      @"grading. Pour les rushes Sony habituels, utilisez "
                      @"S-Gamut3.Cine / S-Log3 en plage Full, puis Rec.2020 "
                      @"/ HLG."
                 form:accessory];
    [panel addButtonWithTitle:@"Appliquer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      const ColorManagementSettings settings = chosen();
      if (settings.output_transfer == "hlg" &&
          settings.output_gamut != "rec2020")
          return @"La sortie HLG doit utiliser le gamut Rec.2020.";
      return nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self commitColorSettings:chosen()];
                }];
}

// AL4 -- what the main thread can tell without reading the weights: unset,
// unreadable or known as other weights refuse here; weights not read yet in
// this process are read by the task, which refuses the same way.
- (BOOL)hesitationsWeightsAllowed {
    const whisper_model_setting::HesitationsFacts facts =
        whisper_model_setting::CurrentHesitationsFacts();
    const whisper_model_setting::HesitationsProblem problem =
        whisper_model_setting::HesitationsWeightsProblem(facts);
    if (problem == whisper_model_setting::HesitationsProblem::kNone ||
        problem == whisper_model_setting::HesitationsProblem::kUnverified)
        return YES;
    [self showHesitationsRefusal:whisper_model_setting::RefuseHesitations(
                                     problem, facts)
                                     .detail];
    return NO;
}

- (void)showHesitationsRefusal:(const std::string&)detail {
    self.binSummaryLabel.stringValue = CMStringFromUtf8(detail);
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Transcription des hésitations impossible";
    alert.informativeText = CMStringFromUtf8(detail);
    if (self.window.attachedSheet == nil)
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
    else
        NSBeep();
}

@end
