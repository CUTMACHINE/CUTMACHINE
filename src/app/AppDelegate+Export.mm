// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Delivery from
// the window: the final export and its presets, its progress sheet and its
// errors, the portable collect, the subtitles file, and the send to
// Resolve. Export.h, ResolveSend.h and Subtitles.h do the work; these
// methods ask, show and report. Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"
#include "ResolveSend.h"

@implementation AppDelegate (Export)

- (void)showExportError:(const std::string&)message {
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"Export impossible";
    alert.informativeText =
        [NSString stringWithUTF8String:message.c_str()] ?: @"Erreur inconnue";
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The settings on the window's
// form panel, the destination in the save panel, the render under a
// progress panel: three sheets on the window, one after the other. The
// window stays blocked while the export runs, as under the alert it
// replaces; the menus stay live, as they were (ExportPlan is a copy: an
// edit made meanwhile does not reach the file being written).
- (void)exportFinalVideo:(id)sender {
    (void)sender;
    if (!self.state || self.state->exportRunning) return;

    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 172)];
    auto popup = [&](NSString* labelText, NSArray<NSString*>* choices,
                     CGFloat y) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3, 132, 22);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* menu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(144, y, 364, 26)
                                       pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };

    NSMutableArray<NSString*>* presetNames = [NSMutableArray array];
    for (const ExportPresetDescriptor& preset : Exporter::Presets())
        [presetNames addObject:[NSString stringWithUTF8String:preset.name]];
    NSPopUpButton* preset = popup(@"Preset :", presetNames, 140);
    // PRD16 -- ROADMAP.md. The dialog opens on the engine's default for this
    // timeline, the file --export and export_timeline write without a codec.
    [preset
        selectItemAtIndex:(NSInteger)Exporter::PresetIndex(
                              Exporter::DefaultPreset(self.state->document))];
    // VQA2 -- the SDR tag rides with the container: Rec.709-A exists only
    // in a .mov (Export.h), so the menu offers no pair the engine refuses.
    NSPopUpButton* container =
        popup(@"Conteneur :",
              @[
                  @"MP4 · Rec.709", @"QuickTime MOV · Rec.709",
                  @"QuickTime MOV · Rec.709-A (gamma 2,4, comme le moniteur)"
              ],
              108);
    NSPopUpButton* resolution =
        popup(@"Résolution :",
              @[
                  @"Selon le preset", @"Format de la séquence", @"1920 × 1080",
                  @"3840 × 2160"
              ],
              76);
    NSPopUpButton* cadence =
        popup(@"Cadence :",
              @[
                  @"Selon le preset", @"Cadence de la séquence", @"23,976 i/s",
                  @"24 i/s", @"25 i/s", @"29,97 i/s", @"50 i/s", @"59,94 i/s"
              ],
              44);
    NSPopUpButton* encoder =
        popup(@"Encodage :",
              @[
                  @"Selon le preset", @"VideoToolbox — rapide",
                  @"Logiciel — qualité constante"
              ],
              12);
    preset.identifier = @"export.preset";
    container.identifier = @"export.container";
    resolution.identifier = @"export.resolution";
    cadence.identifier = @"export.rate";
    encoder.identifier = @"export.encoder";
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Exporter la vidéo finale"
              message:@"Choisissez un preset. Les autres menus servent "
                      @"uniquement d’override ponctuel."
                 form:accessory];
    [panel addButtonWithTitle:@"Choisir la destination…"
                         role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = preset;
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self exportWithPreset:preset
                               container:container
                              resolution:resolution
                                 cadence:cadence
                                 encoder:encoder];
                }];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The export's progress, a panel
// with its « Annuler »: the bar is its form. Its title says the codec the
// preset gives (PRD16). Built apart, so the window test reads it.
- (CMFormPanel*)exportProgressPanel:(BOOL)h264 {
    NSProgressIndicator* indicator =
        [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 420, 20)];
    indicator.identifier = @"export.progress";
    indicator.indeterminate = NO;
    indicator.minValue = 0.0;
    indicator.maxValue = 100.0;
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:h264 ? @"Export H.264 en cours" : @"Export HEVC en cours"
              message:@"Préparation du rendu…"
                 form:indicator];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    return panel;
}

// The collection's, which cannot be cancelled: the alert's own « OK » let
// the person put it away and go on editing -- the collection copies the
// project as saved when it started -- and « Masquer » keeps that, named for
// what it does.
- (CMFormPanel*)collectProgressPanel {
    NSProgressIndicator* indicator =
        [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 420, 18)];
    indicator.identifier = @"collect.progress";
    indicator.indeterminate = YES;
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Collecte du projet en cours"
              message:@"Copie et vérification des médias originaux…"
                 form:indicator];
    [panel addButtonWithTitle:@"Masquer" role:CMFormButtonRoleCancel];
    return panel;
}

// What the export form chose, read once it has closed: the destination,
// then the render.
- (void)exportWithPreset:(NSPopUpButton*)preset
               container:(NSPopUpButton*)container
              resolution:(NSPopUpButton*)resolution
                 cadence:(NSPopUpButton*)cadence
                 encoder:(NSPopUpButton*)encoder {
    if (!self.state || self.state->exportRunning) return;
    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    const MediaRate sequenceRate = self.state->document.sequence.frame_rate;

    const auto& presets = Exporter::Presets();
    const size_t presetIndex = std::min<size_t>(
        static_cast<size_t>(preset.indexOfSelectedItem), presets.size() - 1);
    ExportSettings selectedSettings =
        Exporter::SettingsForPreset(presets[presetIndex].id, "", sequenceWidth,
                                    sequenceHeight, sequenceRate);
    switch (resolution.indexOfSelectedItem) {
        case 1:
            selectedSettings.width = sequenceWidth + sequenceWidth % 2;
            selectedSettings.height = sequenceHeight + sequenceHeight % 2;
            break;
        case 2:
            selectedSettings.width = 1920;
            selectedSettings.height = 1080;
            break;
        case 3:
            selectedSettings.width = 3840;
            selectedSettings.height = 2160;
            break;
        default:
            break;
    }
    switch (cadence.indexOfSelectedItem) {
        case 1:
            selectedSettings.frame_rate = sequenceRate;
            break;
        case 2:
            selectedSettings.frame_rate = {24000, 1001};
            break;
        case 3:
            selectedSettings.frame_rate = {24, 1};
            break;
        case 4:
            selectedSettings.frame_rate = {25, 1};
            break;
        case 5:
            selectedSettings.frame_rate = {30000, 1001};
            break;
        case 6:
            selectedSettings.frame_rate = {50, 1};
            break;
        case 7:
            selectedSettings.frame_rate = {60000, 1001};
            break;
        default:
            break;
    }
    // PRD16 -- the override changes how the preset's codec is encoded, never
    // the codec: with the H.264 preset it used to force an HEVC encoder.
    const bool h264 = Exporter::IsH264(selectedSettings.encoder);
    if (encoder.indexOfSelectedItem == 1)
        selectedSettings.encoder = h264 ? ExportEncoder::H264VideoToolbox
                                        : ExportEncoder::HevcVideoToolbox;
    else if (encoder.indexOfSelectedItem == 2)
        selectedSettings.encoder =
            h264 ? ExportEncoder::H264Software : ExportEncoder::HevcSoftware;

    const BOOL mov = container.indexOfSelectedItem >= 1;
    if (container.indexOfSelectedItem == 2)
        selectedSettings.sdr_tag = ExportSdrTag::Rec709A;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Destination de l’export";
    save.nameFieldStringValue = mov ? @"export.mov" : @"export.mp4";
    save.allowedContentTypes =
        @[ [UTType typeWithFilenameExtension:(mov ? @"mov" : @"mp4")] ];
    [save
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 NSURL* chosen = CMSaveSheetAnswer(save, response);
                 if (!chosen) return;

                 ExportSettings settings = selectedSettings;
                 settings.output_path = chosen.fileSystemRepresentation ?: "";
                 settings.overwrite =
                     true;  // NSSavePanel already confirmed this.

                 ExportPlan plan;
                 std::string error;
                 if (!Exporter::BuildPlan(self.state->document,
                                          self.documentPath.UTF8String ?: "",
                                          settings, plan, error)) {
                     [self showExportError:error];
                     return;
                 }

                 void (^startExport)(void) = ^{
                   // PRD26 -- a progress panel with its « Annuler », in
                   // place of an alert.
                   CMFormPanel* progressPanel = [self
                       exportProgressPanel:Exporter::IsH264(settings.encoder)];
                   NSProgressIndicator* indicator =
                       (NSProgressIndicator*)progressPanel.form;
                   self.state->exportCancel.store(false);
                   self.state->exportRunning = true;
                   [progressPanel
                       presentInWindow:self.window
                            completion:^(NSModalResponse) {
                              if (self.state && self.state->exportRunning)
                                  self.state->exportCancel.store(true);
                            }];

                   dispatch_async(
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                       ^{
                         std::string renderError;
                         const bool ok = Exporter::Run(
                             plan,
                             [&](const ExportProgress& progress) {
                                 dispatch_async(dispatch_get_main_queue(), ^{
                                   indicator.doubleValue =
                                       progress.fraction * 100.0;
                                   [progressPanel
                                       setMessage:
                                           [NSString
                                               stringWithFormat:
                                                   @"%lld / %lld images — "
                                                   @"%.2fx",
                                                   static_cast<long long>(
                                                       progress
                                                           .rendered_frames),
                                                   static_cast<long long>(
                                                       progress.total_frames),
                                                   progress.speed]];
                                 });
                             },
                             &self.state->exportCancel, renderError);
                         dispatch_async(dispatch_get_main_queue(), ^{
                           self.state->exportRunning = false;
                           [progressPanel endWithResponse:NSModalResponseStop];
                           if (!ok) {
                               if (renderError != "export cancelled")
                                   [self showExportError:renderError];
                               return;
                           }
                           NSAlert* complete = [NSAlert new];
                           complete.messageText = @"Export terminé";
                           complete.informativeText = chosen.path;
                           [complete beginSheetModalForWindow:self.window
                                            completionHandler:nil];
                         });
                       });
                 };
                 // Decided on 8 October 2026 (ROADMAP.md): a caption
                 // family the caption renderer lacks is warned about,
                 // not refused (FontCatalog.h).
                 if (plan.caption_font_substitutions.empty()) {
                     startExport();
                     return;
                 }
                 const CaptionFontNotice notice =
                     CaptionFontWarning(plan.caption_font_substitutions);
                 NSAlert* warning = [NSAlert new];
                 warning.alertStyle = NSAlertStyleWarning;
                 warning.messageText =
                     [NSString stringWithUTF8String:notice.title.c_str()];
                 warning.informativeText =
                     [NSString stringWithUTF8String:notice.text.c_str()];
                 [warning addButtonWithTitle:@"Exporter quand même"];
                 // Escape is given by NSAlert to a button titled with the
                 // "Cancel" of the language macOS lends the app; said here,
                 // it does not depend on it (QA pass of 8 October 2026).
                 [warning addButtonWithTitle:@"Annuler"].keyEquivalent =
                     @"\033";
                 [warning beginSheetModalForWindow:self.window
                                 completionHandler:^(NSModalResponse choice) {
                                   if (choice == NSAlertFirstButtonReturn)
                                       startExport();
                                 }];
               }];
}

- (void)collectPortableProject:(id)sender {
    (void)sender;
    if (!self.state || self.documentPath.length == 0) return;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Collecter le projet et ses médias";
    save.prompt = @"Collecter";
    save.canCreateDirectories = YES;
    NSString* projectName =
        [NSString stringWithUTF8String:self.state->project.name.c_str()];
    save.nameFieldStringValue =
        [projectName stringByAppendingString:@".cutmachine-project"];
    [save
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 NSURL* chosen = CMSaveSheetAnswer(save, response);
                 if (!chosen) return;
                 NSString* source = [self.documentPath copy];
                 NSString* destination = [chosen.path copy];
                 // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. A progress
                 // panel in place of an alert (-collectProgressPanel).
                 CMFormPanel* progress = [self collectProgressPanel];
                 [(NSProgressIndicator*)progress.form startAnimation:nil];
                 [progress presentInWindow:self.window completion:nil];

                 auto result = std::make_shared<PortableProjectResult>();
                 auto error = std::make_shared<std::string>();
                 dispatch_async(
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                       const bool success = CollectPortableProject(
                           source.fileSystemRepresentation ?: "",
                           destination.fileSystemRepresentation ?: "", *result,
                           *error);
                       dispatch_async(dispatch_get_main_queue(), ^{
                         [progress endWithResponse:NSModalResponseStop];
                         NSAlert* finished = [NSAlert new];
                         if (!success) {
                             finished.alertStyle = NSAlertStyleCritical;
                             finished.messageText = @"Collecte impossible";
                             finished.informativeText =
                                 [NSString stringWithUTF8String:error->c_str()];
                             [finished beginSheetModalForWindow:self.window
                                              completionHandler:nil];
                             return;
                         }
                         finished.messageText = @"Projet portable créé";
                         // FI12 -- docs/roadmap/FIABILITE-2026-10.md. A
                         // journal whose rewrite could not be proven is
                         // copied as it was: undoing there can reach the
                         // source's files, and the person is told.
                         const size_t verbatim =
                             result->verbatim_histories.size();
                         NSString* history =
                             verbatim == 0
                                 ? @""
                                 : [NSString
                                       stringWithFormat:
                                           @"\n%zu historique%@ copié%@ tel "
                                           @"quel : y annuler peut ramener "
                                           @"un fichier du projet source.",
                                           verbatim, verbatim == 1 ? @"" : @"s",
                                           verbatim == 1 ? @"" : @"s"];
                         finished.informativeText = [NSString
                             stringWithFormat:
                                 @"%llu média%@ · %.2f Go copiés\n%@%@",
                                 static_cast<unsigned long long>(
                                     result->media_count),
                                 result->media_count == 1 ? @"" : @"s",
                                 result->media_bytes / 1000000000.0,
                                 destination, history];
                         [finished
                             addButtonWithTitle:@"Révéler dans le Finder"];
                         [finished addButtonWithTitle:@"Fermer"];
                         [finished
                             beginSheetModalForWindow:self.window
                                    completionHandler:^(
                                        NSModalResponse choice) {
                                      if (choice == NSAlertFirstButtonReturn)
                                          [NSWorkspace.sharedWorkspace
                                              activateFileViewerSelectingURLs:@[
                                                  chosen
                                              ]];
                                    }];
                       });
                     });
               }];
}

- (void)exportSubtitlesPressed:(id)sender {
    (void)sender;
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.title = @"Exporter les sous-titres SubRip";
    panel.prompt = @"Exporter";
    panel.nameFieldStringValue = @"sous-titres.srt";
    panel.allowedContentTypes = @[ [UTType typeWithFilenameExtension:@"srt"] ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    std::string message;
    if (!SaveSrt(self.state->document, panel.URL.fileSystemRepresentation ?: "",
                 message)) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Export SRT impossible";
        alert.informativeText = [NSString stringWithUTF8String:message.c_str()]
                                    ?: @"Erreur inconnue";
        [alert runModal];
        return;
    }
    self.infoLabel.stringValue = @"Sous-titres SRT exportés";
}

// PA16 -- docs/roadmap/PARITE-2026-11.md. « Fichier ▸ Envoyer dans
// Resolve » : send_to_resolve, the same engine call (ResolveSend.h), on the
// timeline on screen, into the project open in Resolve. Off the main thread:
// the bridge drives Resolve for a few seconds, and the window keeps
// answering. No name is asked: the bridge takes the sequence's, or the next
// free one. What it says comes from ResolveSendNoticeFor, in the status line,
// and in a sheet when something did not travel or the send was refused.
- (void)sendToResolvePressed:(id)sender {
    (void)sender;
    if (self.resolveSendPending || !self.state) {
        NSBeep();
        return;
    }
    self.resolveSendPending = YES;
    self.infoLabel.stringValue = @"Envoi dans Resolve…";
    auto document = std::make_shared<Document>(self.state->document);
    const std::string package =
        std::filesystem::path(self.documentPath.UTF8String ?: "")
            .parent_path()
            .string();
    __weak AppDelegate* weakSelf = self;
    static NSArray* const modes =
        @[ NSDefaultRunLoopMode, NSModalPanelRunLoopMode ];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string result;
      std::string errorName;
      std::string error;
      const bool sent = SendTimelineToResolve(
          *document, package, ResolveSendRequest{}, LocateResolveBridge(),
          result, errorName, error);
      auto notice = std::make_shared<ResolveSendNotice>(
          ResolveSendNoticeFor(sent, result, errorName, error));
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, (__bridge CFTypeRef)modes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf.resolveSendPending = NO;
        strongSelf.infoLabel.stringValue = CMStringFromUtf8(notice->status);
        if (notice->title.empty()) return;
        NSAlert* alert = [NSAlert new];
        alert.alertStyle =
            notice->failed ? NSAlertStyleCritical : NSAlertStyleInformational;
        alert.messageText = CMStringFromUtf8(notice->title);
        alert.informativeText = CMStringFromUtf8(notice->text);
        [alert beginSheetModalForWindow:strongSelf.window
                      completionHandler:nil];
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

@end
