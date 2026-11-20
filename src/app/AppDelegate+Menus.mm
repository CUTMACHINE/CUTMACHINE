// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The menu bar:
// its construction, the validation of each item, what each item does, and
// the keyboard shortcuts a person can rebind (KeyboardShortcuts.h decides
// the bindings). Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (Menus)

- (NSArray<NSDictionary<NSString*, NSString*>*>*)shortcutDefinitions {
    NSMutableArray<NSDictionary<NSString*, NSString*>*>* definitions =
        [NSMutableArray array];
    for (const ui::shortcuts::Definition& definition :
         ui::shortcuts::Definitions()) {
        [definitions addObject:@{
            @"id" : @(definition.id),
            @"title" : @(definition.title),
            @"default" : @(definition.default_spec)
        }];
    }
    return definitions;
}

- (void)loadShortcutBindings {
    self.shortcutBindings = [NSMutableDictionary dictionary];
    NSDictionary* stored = [NSUserDefaults.standardUserDefaults
        dictionaryForKey:@"KeyboardShortcuts.v1"];
    for (NSDictionary* definition in [self shortcutDefinitions]) {
        NSString* identifier = definition[@"id"];
        // DEC40 -- a key stored under a command's former name still
        // belongs to it (KeyboardShortcuts.h, RenamedCommands).
        std::string spec;
        const bool found = ui::shortcuts::StoredSpecFor(
            identifier.UTF8String,
            [stored](const std::string& name, std::string& value) {
                id entry = stored[@(name.c_str())];
                if (![entry isKindOfClass:NSString.class]) return false;
                value = static_cast<NSString*>(entry).UTF8String;
                return true;
            },
            spec);
        NSString* value =
            found ? CMStringFromUtf8(spec) : definition[@"default"];
        self.shortcutBindings[identifier] = value;
    }
    self.shortcutMenuItems = [NSMutableDictionary dictionary];
}

- (BOOL)event:(NSEvent*)event matchesShortcut:(NSString*)identifier {
    NSString* expectedKey = nil;
    NSEventModifierFlags expectedModifiers = 0;
    if (!ParseShortcutSpec(self.shortcutBindings[identifier], &expectedKey,
                           &expectedModifiers, nullptr) ||
        expectedKey.length == 0)
        return NO;
    ui::shortcuts::Chord expected;
    expected.key = expectedKey.UTF8String ?: "";
    NSString* eventKey = event.charactersIgnoringModifiers.lowercaseString;
    NSEventModifierFlags actual = event.modifierFlags & kShortcutModifierMask;
    if (event.keyCode == 51) {
        eventKey = @"delete";
    }
    if (event.keyCode == 117) {  // Forward Delete
        NSString* forwardKey = nil;
        NSEventModifierFlags forwardModifiers = 0;
        ParseShortcutSpec(@(ui::shortcuts::kForwardDeleteMeans), &forwardKey,
                          &forwardModifiers, nullptr);
        eventKey = forwardKey;
        actual |= forwardModifiers;
    }
    if ([eventKey isEqualToString:@" "]) {
        eventKey = @"space";
    }
    if (eventKey.length == 1) {
        const unichar character = [eventKey characterAtIndex:0];
        if (character >= NSF1FunctionKey && character <= NSF19FunctionKey) {
            eventKey = [NSString
                stringWithFormat:@"f%d", character - NSF1FunctionKey + 1];
        }
    }
    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. A Shift the character
    // needed is not a modifier of it: the period is Shift and ";" on the
    // French layout (KeyboardShortcuts.h, TypedKeyMatches).
    NSString* unshifted = eventKey;
    if ((actual & NSEventModifierFlagShift) != 0 && eventKey.length == 1 &&
        event.keyCode != 51 && event.keyCode != 117) {
        NSString* bare = [event charactersByApplyingModifiers:0];
        if (bare.length == 1) unshifted = bare.lowercaseString;
    }
    const auto bit = [&](NSEventModifierFlags flags, NSEventModifierFlags flag,
                         ui::shortcuts::Modifier modifier) {
        return (flags & flag) != 0 ? static_cast<std::uint32_t>(modifier) : 0u;
    };
    const auto bits = [&](NSEventModifierFlags flags) {
        return bit(flags, NSEventModifierFlagCommand, ui::shortcuts::kCommand) |
               bit(flags, NSEventModifierFlagOption, ui::shortcuts::kOption) |
               bit(flags, NSEventModifierFlagControl, ui::shortcuts::kControl) |
               bit(flags, NSEventModifierFlagShift, ui::shortcuts::kShift);
    };
    expected.modifiers = bits(expectedModifiers);
    return ui::shortcuts::TypedKeyMatches(expected, eventKey.UTF8String ?: "",
                                          unshifted.UTF8String ?: "",
                                          bits(actual));
}

- (void)applyShortcutBindingsToMenus {
    [self.shortcutMenuItems
        enumerateKeysAndObjectsUsingBlock:^(NSString* identifier,
                                            NSMenuItem* item, BOOL* stop) {
          (void)stop;
          NSString* key = nil;
          NSEventModifierFlags modifiers = 0;
          ParseShortcutSpec(self.shortcutBindings[identifier], &key, &modifiers,
                            nullptr);
          item.keyEquivalent = MenuKeyEquivalentForShortcut(key);
          item.keyEquivalentModifierMask = modifiers;
        }];
    // ON3 -- the Agent's toggle says its key.
    [self refreshAgentToggle];
}

- (NSMenuItem*)shortcutMenuItem:(NSString*)title
                         action:(SEL)action
                        command:(NSString*)identifier {
    NSMenuItem* item = [self menuItem:title action:action key:@""];
    self.shortcutMenuItems[identifier] = item;
    return item;
}

- (void)editKeyboardShortcuts:(id)sender {
    (void)sender;
    // PRD26 -- a form already on the window refuses this one, which would
    // first take the keyboard over from it (-keyboardViewForDefinitions:
    // keeps the fields it builds).
    if ([CMFormPanel formShownInWindow:self.window]) {
        NSBeep();
        return;
    }
    NSArray* definitions = [self shortcutDefinitions];
    NSMutableDictionary<NSString*, NSTextField*>* fields =
        [NSMutableDictionary dictionary];
    const CGFloat rowHeight = 32.0;
    const CGFloat documentHeight = rowHeight * definitions.count;
    NSView* document =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, documentHeight)];
    for (NSUInteger index = 0; index < definitions.count; ++index) {
        NSDictionary* definition = definitions[index];
        const CGFloat y = documentHeight - (index + 1) * rowHeight + 4.0;
        NSTextField* label = [NSTextField labelWithString:definition[@"title"]];
        label.frame = NSMakeRect(8, y + 3, 370, 22);
        NSTextField* field =
            [[NSTextField alloc] initWithFrame:NSMakeRect(390, y, 245, 25)];
        field.identifier =
            [@"shortcut." stringByAppendingString:definition[@"id"]];
        field.stringValue = self.shortcutBindings[definition[@"id"]] ?: @"";
        field.placeholderString = @"Non assigné";
        field.toolTip = @"Exemples : A, Space, F9, Alt+X, Cmd+Shift+L";
        fields[definition[@"id"]] = field;
        [document addSubview:label];
        [document addSubview:field];
    }
    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 650, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.documentView = document;

    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel,
    // resizable: the list takes the room, the keyboard stays on top. The
    // check runs before the panel closes, where a second alert, « Raccourci
    // invalide », reopened the form; « Valeurs par défaut » fills the
    // fields in place, as it did by reopening it.
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 565)];
    NSView* keyboard = [self keyboardViewForDefinitions:definitions
                                                 fields:fields];
    keyboard.frame = NSMakeRect(0, 310, 650, 255);
    keyboard.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    scroll.frame = NSMakeRect(0, 0, 650, 300);
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [accessory addSubview:keyboard];
    [accessory addSubview:scroll];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Raccourcis clavier"
              message:@"Cliquez sur le clavier ou saisissez directement une "
                      @"combinaison. Une valeur vide désactive la commande."
                 form:accessory];
    [panel addButtonWithTitle:@"Enregistrer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    NSButton* defaults = [panel addButtonWithTitle:@"Valeurs par défaut"
                                              role:CMFormButtonRoleAction];
    defaults.target = self;
    defaults.action = @selector(resetKeyboardShortcutFields:);
    panel.formResizable = YES;
    __block NSMutableDictionary<NSString*, NSString*>* accepted = nil;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      NSMutableDictionary<NSString*, NSString*>* candidate =
          [NSMutableDictionary dictionary];
      NSMutableDictionary<NSString*, NSString*>* owners =
          [NSMutableDictionary dictionary];
      for (const ui::shortcuts::ReservedChord& reserved :
           ui::shortcuts::ReservedChords()) {
          owners[ShortcutSignature(@(reserved.spec), nullptr)] =
              @(reserved.title);
      }
      NSString* validationError = nil;
      for (NSDictionary* definition in definitions) {
          NSString* identifier = definition[@"id"];
          NSString* spec = [fields[identifier].stringValue
              stringByTrimmingCharactersInSet:NSCharacterSet
                                                  .whitespaceCharacterSet];
          NSString* parseError = nil;
          NSString* signature = ShortcutSignature(spec, &parseError);
          if (!signature) {
              validationError =
                  [NSString stringWithFormat:@"%@ : %@", definition[@"title"],
                                             parseError];
              break;
          }
          if (signature.length > 0 && owners[signature]) {
              validationError = [NSString
                  stringWithFormat:@"Conflit entre « %@ » et « %@ ».",
                                   owners[signature], definition[@"title"]];
              break;
          }
          if (signature.length > 0) owners[signature] = definition[@"title"];
          candidate[identifier] = spec;
      }
      if (validationError) return validationError;
      accepted = candidate;
      return nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn || !accepted) return;
                  self.shortcutBindings = accepted;
                  [NSUserDefaults.standardUserDefaults
                      setObject:accepted
                         forKey:@"KeyboardShortcuts.v1"];
                  [self applyShortcutBindingsToMenus];
                  self.infoLabel.stringValue = @"Raccourcis clavier mis à jour";
                }];
}

// « Valeurs par défaut » of the shortcuts' panel: every field back to its
// default, the panel left open and its error line cleared.
- (void)resetKeyboardShortcutFields:(id)sender {
    for (NSDictionary* definition in [self shortcutDefinitions])
        self.keyboardEditingFields[definition[@"id"]].stringValue =
            definition[@"default"];
    if ([sender isKindOfClass:NSView.class] &&
        [((NSView*)sender).window isKindOfClass:CMFormPanel.class])
        [(CMFormPanel*)((NSView*)sender).window showError:nil];
}

- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key ?: @""];
    item.target = self;
    return item;
}

- (void)installApplicationMenus {
    [self loadShortcutBindings];
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@"Main"];
    NSMenuItem* appRoot = [[NSMenuItem alloc] initWithTitle:@"CUTMACHINE"
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"CUTMACHINE"];
    [app addItemWithTitle:@"À propos de CUTMACHINE"
                   action:@selector(orderFrontStandardAboutPanel:)
            keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItem:[self menuItem:@"Raccourcis clavier…"
                         action:@selector(editKeyboardShortcuts:)
                            key:@","]];
    [app addItem:[self menuItem:@"Moteur IA…"
                         action:@selector(configureAiEngine:)
                            key:@""]];
    [app addItem:[self menuItem:@"Modèle Whisper…"
                         action:@selector(chooseWhisperModel:)
                            key:@""]];
    // PRD21 (window) -- what this machine lacks, read again at each opening.
    [app addItem:[self menuItem:@"Diagnostic de l'installation…"
                         action:@selector(showSetupDiagnostic:)
                            key:@""]];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quitter CUTMACHINE"
                   action:@selector(terminate:)
            keyEquivalent:@"q"];
    appRoot.submenu = app;
    [bar addItem:appRoot];

    NSMenuItem* fileRoot = [[NSMenuItem alloc] initWithTitle:@"Fichier"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* file = [[NSMenu alloc] initWithTitle:@"Fichier"];
    // DEC39 -- change project without the welcome window.
    [file addItem:[self menuItem:@"Nouveau projet…"
                          action:@selector(newProjectFromEditor:)
                             key:@""]];
    [file addItem:[self menuItem:@"Ouvrir un projet…"
                          action:@selector(openProjectFromEditor:)
                             key:@""]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Importer des rushes…"
                          action:@selector(importMediaPressed:)
                             key:@"i"]];
    [file addItem:[self menuItem:@"Importer des sous-titres SRT…"
                          action:@selector(importSubtitlesPressed:)
                             key:@""]];
    [file addItem:[self menuItem:@"Reconnecter les médias offline…"
                          action:@selector(batchRelinkOfflineMedia:)
                             key:@""]];
    [file addItem:[self menuItem:@"Collecter le projet…"
                          action:@selector(collectPortableProject:)
                             key:@""]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Enregistrer le projet"
                          action:@selector(saveProjectPressed:)
                             key:@"s"]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Exporter la vidéo finale…"
                          action:@selector(exportFinalVideo:)
                             key:@"e"]];
    [file addItem:[self menuItem:@"Exporter les sous-titres SRT…"
                          action:@selector(exportSubtitlesPressed:)
                             key:@""]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Envoyer dans Resolve"
                          action:@selector(sendToResolvePressed:)
                             key:@""]];
    fileRoot.submenu = file;
    [bar addItem:fileRoot];

    NSMenuItem* editRoot = [[NSMenuItem alloc] initWithTitle:@"Édition"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Édition"];
    [edit addItem:[self menuItem:@"Annuler"
                          action:@selector(menuUndo:)
                             key:@"z"]];
    NSMenuItem* redo = [self menuItem:@"Rétablir"
                               action:@selector(menuRedo:)
                                  key:@"z"];
    redo.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [edit addItem:redo];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self shortcutMenuItem:@"Copier les clips"
                                  action:@selector(menuCopyTimelineClips:)
                                 command:@"edit.copy"]];
    [edit addItem:[self shortcutMenuItem:@"Coller les clips"
                                  action:@selector(menuPasteTimelineClips:)
                                 command:@"edit.paste"]];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self shortcutMenuItem:@"Supprimer la sélection"
                                  action:@selector(menuDeleteSelection:)
                                 command:@"edit.delete"]];
    [edit addItem:[self shortcutMenuItem:@"Suppression ripple"
                                  action:@selector(menuRippleDeleteSelection:)
                                 command:@"edit.ripple"]];
    editRoot.submenu = edit;
    [bar addItem:editRoot];

    NSMenuItem* clipRoot = [[NSMenuItem alloc] initWithTitle:@"Clip"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* clip = [[NSMenu alloc] initWithTitle:@"Clip"];
    [clip addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                          action:@selector(openSelectedMediaInSourceMonitor:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Couper au playhead"
                          action:@selector(menuCutSelectedAtPlayhead:)
                             key:@""]];
    // Out of lot -- the author, 29 October 2026: Resolve's Clip > Enable Clip
    // (D).
    [clip addItem:[self shortcutMenuItem:@"Activer / désactiver le plan"
                                  action:@selector(menuToggleClipsEnabled:)
                                 command:@"clip.enable"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self shortcutMenuItem:@"Définir le point d’entrée"
                                  action:@selector(menuMarkTimelineIn:)
                                 command:@"mark.in"]];
    [clip addItem:[self shortcutMenuItem:@"Définir le point de sortie"
                                  action:@selector(menuMarkTimelineOut:)
                                 command:@"mark.out"]];
    [clip addItem:[self shortcutMenuItem:@"Effacer les points In/Out"
                                  action:@selector(menuClearTimelineRange:)
                                 command:@"mark.clear"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self shortcutMenuItem:@"Insérer depuis le moniteur source"
                                  action:@selector(menuInsertSource:)
                                 command:@"source.insert"]];
    [clip addItem:[self shortcutMenuItem:@"Écraser depuis le moniteur source"
                                  action:@selector(menuOverwriteSource:)
                                 command:@"source.overwrite"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self menuItem:@"Activer l’angle multicam du clip"
                          action:@selector(menuActivateMulticamAngle:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Afficher tous les angles multicam"
                          action:@selector(menuClearMulticamAngle:)
                             key:@""]];
    // SY2 -- docs/roadmap/SYNCHRO-2026-10.md. Resolve's Auto Align Clips.
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self autoAlignMenuItem]];
    clipRoot.submenu = clip;
    [bar addItem:clipRoot];

    NSMenuItem* colorRoot = [[NSMenuItem alloc] initWithTitle:@"Couleur"
                                                       action:nil
                                                keyEquivalent:@""];
    NSMenu* color = [[NSMenu alloc] initWithTitle:@"Couleur"];
    [color addItem:[self menuItem:@"Preset Sony S-Log3 → Rec.2020 HLG"
                           action:@selector(applySonyColorPreset:)
                              key:@""]];
    [color addItem:[self menuItem:@"Configurer la gestion colorimétrique…"
                           action:@selector(configureColorManagement:)
                              key:@""]];
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self menuItem:@"Désactiver la gestion colorimétrique"
                           action:@selector(disableColorManagement:)
                              key:@""]];
    // G6 -- docs/roadmap/GRADE-2026-09.md. Record without its grade, and a
    // reference beside it: what the monitor shows, never the document.
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self shortcutMenuItem:@"Ignorer l’étalonnage"
                                   action:@selector(toggleGradeBypass:)
                                  command:@"color.bypass"]];
    [color addItem:[self shortcutMenuItem:@"Prendre l’image de référence"
                                   action:@selector(grabReferenceStill:)
                                  command:@"color.reference.grab"]];
    [color addItem:[self shortcutMenuItem:@"Comparer à l’image de référence"
                                   action:@selector(toggleReferenceSplit:)
                                  command:@"color.reference.split"]];
    [color addItem:[self menuItem:@"Oublier l’image de référence"
                           action:@selector(forgetReferenceStill:)
                              key:@""]];
    // GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. A grade copied
    // from one shot and pasted on the selection (ClipGradeCopy.h), as on
    // Resolve's Color page; paste_clip_grade builds the same step.
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self menuItem:@"Copier l’étalonnage du plan"
                           action:@selector(copyClipGrade:)
                              key:@""]];
    [color addItem:[self menuItem:@"Coller l’étalonnage sur la sélection"
                           action:@selector(pasteClipGrade:)
                              key:@""]];
    colorRoot.submenu = color;
    [bar addItem:colorRoot];

    NSMenuItem* timelineRoot = [[NSMenuItem alloc] initWithTitle:@"Timeline"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* timeline = [[NSMenu alloc] initWithTitle:@"Timeline"];
    NSMenuItem* newTimeline = [self menuItem:@"Nouvelle timeline…"
                                      action:@selector(createTimelinePressed:)
                                         key:@"n"];
    newTimeline.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [timeline addItem:newTimeline];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Outil Sélection"
                                      action:@selector(menuSelectTool:)
                                     command:@"tool.select"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Main"
                                      action:@selector(menuHandTool:)
                                     command:@"tool.hand"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Zoom"
                                      action:@selector(menuZoomTool:)
                                     command:@"tool.zoom"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Lame"
                                      action:@selector(menuCutTool:)
                                     command:@"tool.cut"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Trim"
                                      action:@selector(menuTrimTool:)
                                     command:@"tool.trim"]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Magnétisme"
                                      action:@selector(menuToggleSnapping:)
                                     command:@"timeline.snapping"]];
    [timeline
        addItem:[self shortcutMenuItem:@"Sélection liée"
                                action:@selector(menuToggleLinkedSelection:)
                               command:@"timeline.linked"]];
    [timeline addItem:[self shortcutMenuItem:@"Tout sélectionner après la "
                                             @"tête de lecture"
                                      action:@selector(menuSelectClipsAfter:)
                                     command:@"select.after"]];
    [timeline addItem:[self shortcutMenuItem:@"Ajouter un fondu enchaîné"
                                      action:@selector(menuAddCrossDissolve:)
                                     command:@"transition.add"]];
    [timeline addItem:[self menuItem:@"Supprimer le fondu au raccord"
                              action:@selector(menuRemoveCrossDissolve:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Utiliser les proxies"
                              action:@selector(menuToggleProxies:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Générer automatiquement les proxies"
                              action:@selector(menuToggleAutomaticProxies:)
                                 key:@""]];
    [timeline addItem:[self shortcutMenuItem:@"Cadrer toute la timeline"
                                      action:@selector(menuFitTimeline:)
                                     command:@"timeline.fit"]];
    [timeline addItem:[self menuItem:@"Réglages de séquence…"
                              action:@selector(configureSequence:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Transcrire la timeline avec Whisper…"
                              action:@selector(transcribeTimeline:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Transcrire les hésitations (français)…"
                              action:@selector(transcribeTimelineHesitations:)
                                 key:@""]];
    // D2 -- docs/roadmap/DIALOGUE-2026-10.md.
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Poser un marqueur"
                                      action:@selector(menuAddMarker:)
                                     command:@"marker.add"]];
    [timeline addItem:[self shortcutMenuItem:@"Annoter le marqueur…"
                                      action:@selector(menuAnnotateMarker:)
                                     command:@"marker.note"]];
    [timeline addItem:[self menuItem:@"Supprimer le marqueur"
                              action:@selector(menuRemoveMarker:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Marqueur précédent (⇧↑)"
                              action:@selector(menuPreviousMarker:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Marqueur suivant (⇧↓)"
                              action:@selector(menuNextMarker:)
                                 key:@""]];
    // D7 -- docs/roadmap/DIALOGUE-2026-10.md.
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Modifier le texte du carton (↩)"
                              action:@selector(menuEditCartonText:)
                                 key:@""]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Ajouter une piste vidéo"
                              action:@selector(menuAddVideoTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Ajouter une piste audio"
                              action:@selector(menuAddAudioTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
    timelineRoot.submenu = timeline;
    [bar addItem:timelineRoot];

    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. Resolve's Trim menu, for the
    // commands of the trim mode (Resolve 21.1 manual, p. 4340).
    NSMenuItem* trimRoot = [[NSMenuItem alloc] initWithTitle:@"Trim"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* trimMenu = [[NSMenu alloc] initWithTitle:@"Trim"];
    [trimMenu addItem:[self shortcutMenuItem:@"Point de montage le plus proche"
                                      action:@selector(menuTrimSelectNearest:)
                                     command:@"trim.select"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Côté du point (sortie, roll, "
                                             @"entrée)"
                                      action:@selector(menuTrimCycleSide:)
                                     command:@"trim.side"]];
    [trimMenu addItem:NSMenuItem.separatorItem];
    [trimMenu addItem:[self shortcutMenuItem:@"Une image en arrière"
                                      action:@selector(menuTrimNudgeBack:)
                                     command:@"trim.nudge.back"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Une image en avant"
                                      action:@selector(menuTrimNudgeForward:)
                                     command:@"trim.nudge.forward"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Cinq images en arrière"
                                      action:@selector(menuTrimNudgeBackMany:)
                                     command:@"trim.nudge.back.many"]];
    [trimMenu
        addItem:[self shortcutMenuItem:@"Cinq images en avant"
                                action:@selector(menuTrimNudgeForwardMany:)
                               command:@"trim.nudge.forward.many"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Étendre à la tête de lecture"
                                      action:@selector(menuTrimExtend:)
                                     command:@"trim.extend"]];
    [trimMenu addItem:NSMenuItem.separatorItem];
    [trimMenu addItem:[self shortcutMenuItem:@"Trim dynamique (J K L)"
                                      action:@selector(menuTrimDynamic:)
                                     command:@"trim.dynamic"]];
    [trimMenu addItem:[self menuItem:@"Quitter le mode trim (Échap)"
                              action:@selector(menuLeaveTrimMode:)
                                 key:@""]];
    [trimMenu addItem:NSMenuItem.separatorItem];
    // FC3 -- Resolve's Clip > Retime Controls (manual, p. 1218).
    [trimMenu addItem:[self shortcutMenuItem:@"Contrôles de vitesse"
                                      action:@selector(menuRetimeControls:)
                                     command:@"clip.retime"]];
    trimRoot.submenu = trimMenu;
    [bar addItem:trimRoot];

    NSMenuItem* playbackRoot = [[NSMenuItem alloc] initWithTitle:@"Lecture"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* playback = [[NSMenu alloc] initWithTitle:@"Lecture"];
    [playback addItem:[self shortcutMenuItem:@"Lecture / Pause"
                                      action:@selector(menuPlayPause:)
                                     command:@"play.toggle"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture arrière"
                                      action:@selector(menuPlayReverse:)
                                     command:@"play.reverse"]];
    [playback addItem:[self shortcutMenuItem:@"Arrêt"
                                      action:@selector(menuStop:)
                                     command:@"play.stop"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture avant"
                                      action:@selector(menuPlayForward:)
                                     command:@"play.forward"]];
    [playback addItem:NSMenuItem.separatorItem];
    // ON9 -- Resolve's Play Around Current Selection and its loop.
    [playback addItem:[self shortcutMenuItem:@"Lire autour de la sélection"
                                      action:@selector(menuPlayAround:)
                                     command:@"play.around"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture en boucle"
                                      action:@selector(menuToggleLoop:)
                                     command:@"play.loop"]];
    playbackRoot.submenu = playback;
    [bar addItem:playbackRoot];

    NSMenuItem* viewRoot = [[NSMenuItem alloc] initWithTitle:@"Affichage"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* view = [[NSMenu alloc] initWithTitle:@"Affichage"];
    // ON1, ON2 -- the pages, and the panels a page can call.
    [view addItem:[self shortcutMenuItem:@"Onglet Médias"
                                  action:@selector(showMediaPage:)
                                 command:@"page.media"]];
    [view addItem:[self shortcutMenuItem:@"Onglet Montage"
                                  action:@selector(showEditPage:)
                                 command:@"page.edit"]];
    [view addItem:[self shortcutMenuItem:@"Onglet Couleur"
                                  action:@selector(showColorPage:)
                                 command:@"page.color"]];
    [view addItem:[NSMenuItem separatorItem]];
    [view addItem:[self shortcutMenuItem:@"Médiathèque"
                                  action:@selector(toggleMediaPanel:)
                                 command:@"panel.media"]];
    [view addItem:[self shortcutMenuItem:@"Inspecteur"
                                  action:@selector(toggleInspectorPanel:)
                                 command:@"panel.inspector"]];
    // ON3 -- the Agent's own window, from any page.
    [view addItem:[self shortcutMenuItem:@"Agent"
                                  action:@selector(toggleAgentWindow:)
                                 command:@"panel.agent"]];
    // CH6 -- the Charte panel, floating, on demand only (DEC63).
    [view addItem:[self shortcutMenuItem:@"Charte…"
                                  action:@selector(toggleCharterPanel:)
                                 command:@"panel.charter"]];
    [view addItem:[NSMenuItem separatorItem]];
    [view addItem:[self shortcutMenuItem:@"Vidéo plein écran"
                                  action:@selector(toggleFullscreen:)
                                 command:@"view.fullscreen"]];
    NSMenuItem* output =
        [[NSMenuItem alloc] initWithTitle:@"Sortie vidéo (clean feed)"
                                   action:nil
                            keyEquivalent:@""];
    self.videoOutputMenu = [[NSMenu alloc] initWithTitle:output.title];
    self.videoOutputMenu.autoenablesItems = NO;
    output.submenu = self.videoOutputMenu;
    [view addItem:output];
    [self refreshVideoOutputMenu];
    viewRoot.submenu = view;
    [bar addItem:viewRoot];
    NSApp.mainMenu = bar;
    [self applyShortcutBindingsToMenus];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    const SEL action = item.action;
    // ON9 -- the loop, checked while on.
    if (action == @selector(menuToggleLoop:)) {
        item.state =
            self.loopPlayback ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    // ON1, ON2 -- the page shown and the panels open on it, checked.
    if (action == @selector(showMediaPage:) ||
        action == @selector(showEditPage:) ||
        action == @selector(showColorPage:)) {
        const ui::workspace::Page page =
            action == @selector(showMediaPage:)  ? ui::workspace::Page::Media
            : action == @selector(showEditPage:) ? ui::workspace::Page::Edit
                                                 : ui::workspace::Page::Color;
        item.state = self.workspacePage == page ? NSControlStateValueOn
                                                : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(toggleMediaPanel:) || action == @selector
                                                      (toggleInspectorPanel:)) {
        const ui::workspace::PanelSet panels = self.workspacePanels;
        item.state = (action == @selector(toggleMediaPanel:) ? panels.media
                                                             : panels.inspector)
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(toggleAgentWindow:)) {
        item.state =
            self.agentShown ? NSControlStateValueOn : NSControlStateValueOff;
        return self.agentWindow != nil;
    }
    if (action == @selector(toggleCharterPanel:)) {
        item.state = self.charterPanel.shown ? NSControlStateValueOn
                                             : NSControlStateValueOff;
        return self.window != nil;
    }
    // CH6 -- the Charte panel key: its undo is the charter's versions,
    // never the montage's.
    if ((action == @selector(menuUndo:) || action == @selector(menuRedo:)) &&
        [self.charterPanel ownsUndo])
        return action == @selector(menuUndo:) ? [self.charterPanel canUndo]
                                              : [self.charterPanel canRedo];
    // CH6 (review) -- and the montage's commands are not its own: with the
    // preview or a list holding the keyboard, Cmd+V pasted clips into the
    // timeline, which the panel's Cmd+Z could not take back.
    if (IsMontageMenuAction(action) && IsMenuBarItem(item) &&
        [self.charterPanel holdsKeyboard])
        return NO;
    // G6 -- checked when on; the split and the forgetting need a still.
    if (action == @selector(toggleGradeBypass:)) {
        if (!self.state) return NO;
        item.state = self.state->monitorCompare.bypass_grade
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(toggleReferenceSplit:)) {
        if (!self.state) return NO;
        item.state = self.state->monitorCompare.split ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
        return [self referenceStillFits];
    }
    if (action == @selector(forgetReferenceStill:))
        return self.state && self.state->referenceStill != nullptr;
    // GR11 -- one graded shot to copy from; a copied grade and a selection
    // to paste on.
    if (action == @selector(copyClipGrade:)) {
        if (!self.state || !self.state->interaction) return NO;
        const std::vector<Ulid> selected =
            self.state->interaction->SelectedClipIds();
        if (selected.size() != 1) return NO;
        const DocumentClip* clip =
            self.state->document.FindClip(selected.front());
        return clip != nullptr &&
               ClipGradeRefusal(self.state->document, *clip).empty();
    }
    if (action == @selector(pasteClipGrade:))
        return self.state && self.state->interaction &&
               self.state->gradeClipboard.has_value() &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(grabReferenceStill:))
        return self.state && self.state->programRenderer != nullptr;
    // G3 -- a choice is checked when every selected picture reads so, and
    // shown mixed when some do.
    if (action == @selector(setSelectedMediaColorInput:) ||
        action == @selector(rereadSelectedMediaColorInput:)) {
        if (!self.state) return NO;
        size_t pictures = 0;
        size_t matching = 0;
        for (NSString* identifier in [self selectedMediaIds]) {
            const LibraryMedia* media = self.state->document.FindLibraryMedia(
                identifier.UTF8String ?: "");
            if (!media || (media->metadata_complete && !media->has_video))
                continue;
            ++pictures;
            if (static_cast<NSInteger>(
                    ui::media_panel::ColorInputChoiceOf(*media)) == item.tag)
                ++matching;
        }
        if (action == @selector(setSelectedMediaColorInput:))
            item.state = matching == 0          ? NSControlStateValueOff
                         : matching == pictures ? NSControlStateValueOn
                                                : NSControlStateValueMixed;
        return pictures > 0;
    }
    if (action == @selector(exportFinalVideo:))
        return self.state && !self.state->exportRunning;
    if (action == @selector(menuUndo:))
        return self.state && (self.state->editLog.AppliedCount() > 0 ||
                              self.state->projectEditLog.AppliedCount() > 0);
    if (action == @selector(menuRedo:))
        return self.state && (self.state->editLog.UndoneCount() > 0 ||
                              self.state->projectEditLog.UndoneCount() > 0);
    if (action == @selector(menuCopyTimelineClips:))
        return self.state && self.state->interaction &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(menuToggleClipsEnabled:))
        return self.state && self.state->interaction &&
               !ToggleClipsEnabled(self.state->document,
                                   self.state->interaction->SelectedClipIds())
                    .clips.empty();
    if (action == @selector(menuPasteTimelineClips:))
        return self.state && !self.state->timelineClipboard.empty();
    // D7 -- one carton selected, and nothing else.
    if (action == @selector(menuEditCartonText:))
        return self.state && self.state->interaction &&
               ui::media_panel::SelectedCartonForText(
                   self.state->document,
                   self.state->interaction->SelectedClipIds())
                   .has_value();
    if (action == @selector(menuAddCrossDissolve:))
        return self.state && self.state->interaction;
    if (action == @selector(menuRemoveCrossDissolve:))
        return self.state && !self.state->document.sequence.transitions.empty();
    // SY2 -- actionable whenever something is selected: the handler says,
    // in words, what the selection lacks, which a greyed item cannot.
    if (action == @selector(menuAutoAlignByWaveform:))
        return self.state && self.state->interaction &&
               !self.state->soundAlignmentRunning &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(menuActivateMulticamAngle:) ||
        action == @selector(menuClearMulticamAngle:)) {
        const BOOL clear = action == @selector(menuClearMulticamAngle:);
        SetMulticamActiveAngleOperation operation;
        return [self multicamSwitchForSelection:clear operation:operation];
    }
    if (action == @selector(menuInsertSource:) || action == @selector
                                                      (menuOverwriteSource:))
        return self.state && self.state->sourceMonitor &&
               self.state->document.FindSource(self.state->sourceMonitorId) !=
                   nullptr;
    if (action == @selector(menuToggleSnapping:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->interaction->SnappingEnabled()
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleLinkedSelection:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->linkedSelection ? NSControlStateValueOn
                                                 : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleProxies:)) {
        if (!self.state) return NO;
        item.state = self.state->proxiesEnabled ? NSControlStateValueOn
                                                : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleAutomaticProxies:)) {
        if (!self.state) return NO;
        item.state = self.state->automaticProxiesEnabled
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(renameBinPressed:) || action == @selector
                                                      (deleteBinPressed:))
        return self.state &&
               self.state->document.FindBin(self.selectedBinId.UTF8String
                                                ?: "") != nullptr;
    if (action == @selector(assignMediaToBinPressed:))
        return self.state &&
               self.state->document.FindLibraryMedia(
                   [self selectedMediaId].UTF8String ?: "") != nullptr;
    if (action == @selector(relinkSelectedMedia:))
        return self.state && self.state->document.FindLibraryMedia(
                                 [self selectedMediaId].UTF8String ?: "");
    if (action == @selector(batchRelinkOfflineMedia:))
        return self.state && !self.state->offlineSourceIds.empty() &&
               self.state->pendingBatchRelinks.empty();
    if (action == @selector(transcribeTimeline:) ||
        action == @selector(transcribeTimelineHesitations:)) {
        if (!self.state) return NO;
        for (const auto& batch : self.state->pendingTimelineTranscriptions)
            if (batch.second.timeline_id == self.state->activeTimelineId)
                return NO;
        // Keep the command actionable. The handler can explain an empty or
        // muted timeline; a disabled menu item cannot explain a false
        // negative caused by stale library probe metadata.
        return YES;
    }
    if (action == @selector(transcribeSelectedMedia:) ||
        action == @selector(transcribeSelectedMediaHesitations:)) {
        const LibraryMedia* media =
            self.state ? self.state->document.FindLibraryMedia(
                             [self selectedMediaId].UTF8String ?: "")
                       : nullptr;
        return media && media->has_audio;
    }
    if (action == @selector(generateSelectedMediaProxy:) ||
        action == @selector(regenerateSelectedMediaThumbnail:))
        return self.state && self.state->document.FindLibraryMedia(
                                 [self selectedMediaId].UTF8String ?: "");
    if (action == @selector(removeSelectedMediaProxy:)) {
        const LibraryMedia* media =
            self.state ? self.state->document.FindLibraryMedia(
                             [self selectedMediaId].UTF8String ?: "")
                       : nullptr;
        return media && !media->proxy_path.empty();
    }
    if (action == @selector(removeContextTrackPressed:))
        return self.state &&
               self.state->document.FindTrack(self.state->contextTrackId);
    if (action == @selector(removeEmptyTracksPressed:))
        return self.state &&
               std::any_of(self.state->document.sequence.tracks.begin(),
                           self.state->document.sequence.tracks.end(),
                           [](const DocumentTrack& track) {
                               return track.clips.empty();
                           });
    if (action == @selector(menuCutSelectedAtPlayhead:)) {
        if (!self.state || !self.state->interaction) return NO;
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        return clip && self.state->requestedPosition > clip->timeline_in &&
               self.state->requestedPosition <
                   clip->timeline_in.add(clip->duration);
    }
    return YES;
}

- (void)menuToggleProxies:(id)sender {
    (void)sender;
    self.state->proxiesEnabled = !self.state->proxiesEnabled;
    [self reloadDecodeWorkers];
    self.infoLabel.stringValue =
        self.state->proxiesEnabled ? @"Proxies activés" : @"Originaux activés";
}

- (void)menuToggleAutomaticProxies:(id)sender {
    (void)sender;
    self.state->automaticProxiesEnabled = !self.state->automaticProxiesEnabled;
    [NSUserDefaults.standardUserDefaults
        setBool:self.state->automaticProxiesEnabled
         forKey:kAutomaticProxyGenerationDefaultsKey];
    self.infoLabel.stringValue =
        self.state->automaticProxiesEnabled
            ? @"Génération automatique des proxies activée"
            : @"Génération automatique des proxies désactivée";
}

- (void)menuCopyTimelineClips:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    std::vector<Ulid> clipIds = self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection)
        clipIds = ExpandLinkedClipSelection(self.state->document, clipIds);
    self.state->timelineClipboard =
        CopyTimelineClips(self.state->document, clipIds);
    if (self.state->timelineClipboard.empty()) {
        self.infoLabel.stringValue = @"Aucun clip à copier";
        return;
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%lu clip%@ copié%@",
                         (unsigned long)self.state->timelineClipboard.size(),
                         self.state->timelineClipboard.size() == 1 ? @"" : @"s",
                         self.state->timelineClipboard.size() == 1 ? @""
                                                                   : @"s"];
}

- (void)menuPasteTimelineClips:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    const auto operation = PasteTimelineClipboardAt(
        self.state->document, self.state->timelineClipboard,
        self.state->requestedPosition);
    if (!operation) {
        self.infoLabel.stringValue =
            @"Collage impossible : aucune piste compatible";
        return;
    }
    PasteClipsOperation overwrite = *operation;
    overwrite.overwrite = true;
    [self applyTimelinePaste:std::move(overwrite)
                       label:@"Clips collés en overwrite"];
}

- (void)menuUndo:(id)sender {
    (void)sender;
    // CH6 -- docs/roadmap/CHARTE-2026-11.md (DEC61). The Charte panel key:
    // its last gesture is undone, as a version of the charter; the
    // montage's EditLog is not touched.
    if ([self.charterPanel ownsUndo]) {
        [self.charterPanel undo];
        return;
    }
    [self dismissAgentMarks];  // PRD25
    // FC4 -- the counters of the trim mode say what was trimmed since its
    // points were selected; an undo would make them lie. Left, as Escape.
    [self leaveTrimMode];
    EditError error = EditError::None;
    std::string message;
    const bool projectHistory =
        (self.state->lastHistoryDomain == HistoryDomain::Project &&
         self.state->projectEditLog.AppliedCount() > 0) ||
        (self.state->editLog.AppliedCount() == 0 &&
         self.state->projectEditLog.AppliedCount() > 0);
    if (projectHistory) {
        const bool renameOnly =
            std::holds_alternative<RenameProjectItemOperation>(
                self.state->projectEditLog.AppliedEntries().back().op);
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Undo(candidate, error, message)) {
            if (error != EditError::EmptyUndo)
                std::fprintf(stderr, "Project undo failed (%s): %s\n",
                             EditErrorName(error), message.c_str());
            return;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            std::fprintf(stderr, "Unable to persist project undo: %s\n",
                         message.c_str());
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (renameOnly)
            [self refreshAfterProjectRename];
        else
            [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (stagedLog.Undo(stagedDocument, error, message)) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message]) {
            std::fprintf(stderr, "Unable to persist undo: %s\n",
                         message.c_str());
            return;
        }
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else if (error != EditError::EmptyUndo) {
        std::fprintf(stderr, "Undo failed (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
}

- (void)menuRedo:(id)sender {
    (void)sender;
    if ([self.charterPanel ownsUndo]) {  // CH6, as for an undo.
        [self.charterPanel redo];
        return;
    }
    [self dismissAgentMarks];  // PRD25
    [self leaveTrimMode];      // FC4, as for an undo.
    EditError error = EditError::None;
    std::string message;
    const bool projectHistory =
        (self.state->lastHistoryDomain == HistoryDomain::Project &&
         self.state->projectEditLog.UndoneCount() > 0) ||
        (self.state->editLog.UndoneCount() == 0 &&
         self.state->projectEditLog.UndoneCount() > 0);
    if (projectHistory) {
        const bool renameOnly =
            std::holds_alternative<RenameProjectItemOperation>(
                self.state->projectEditLog.UndoneEntries().back().op);
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Redo(candidate, error, message)) {
            if (error != EditError::EmptyRedo)
                std::fprintf(stderr, "Project redo failed (%s): %s\n",
                             EditErrorName(error), message.c_str());
            return;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            std::fprintf(stderr, "Unable to persist project redo: %s\n",
                         message.c_str());
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (renameOnly)
            [self refreshAfterProjectRename];
        else
            [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (stagedLog.Redo(stagedDocument, error, message)) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message]) {
            std::fprintf(stderr, "Unable to persist redo: %s\n",
                         message.c_str());
            return;
        }
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else if (error != EditError::EmptyRedo) {
        std::fprintf(stderr, "Redo failed (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
}

- (void)menuDeleteSelection:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (self.window.firstResponder == self.binOutline)
        [self deleteBinPressed:nil];
    else
        [self deleteCurrentTimelineSelection];
}

- (void)menuRippleDeleteSelection:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self deleteCurrentTimelineSelectionRipple:YES];
}

- (void)menuMarkTimelineIn:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        self.state->sourceIn = self.state->sourceMonitorPosition;
        [self updateSourceZoneLabel];
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"SOURCE IN  %@",
                                       TimelineTimecode(
                                           *self.state->sourceIn,
                                           source ? source->rate
                                                  : [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineIn = self.state->requestedPosition;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"IN  %@", TimelineTimecode(*self.state->timelineIn,
                                                     [self playheadFrameRate])];
    self.state->overlayDirty = true;
}

- (void)menuMarkTimelineOut:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        self.state->sourceOut = self.state->sourceMonitorPosition;
        if (source) {
            self.state->sourceOut = self.state->sourceOut->add(
                {source->rate.den, source->rate.num});
            if (*self.state->sourceOut > source->duration)
                self.state->sourceOut = source->duration;
        }
        [self updateSourceZoneLabel];
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"SOURCE OUT %@",
                                       TimelineTimecode(
                                           *self.state->sourceOut,
                                           source ? source->rate
                                                  : [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineOut = self.state->requestedPosition;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"OUT %@", TimelineTimecode(*self.state->timelineOut,
                                                     [self playheadFrameRate])];
    self.state->overlayDirty = true;
}

- (void)menuClearTimelineRange:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        self.state->sourceIn.reset();
        self.state->sourceOut.reset();
        [self updateSourceZoneLabel];
        self.infoLabel.stringValue = @"Points Source In/Out effacés";
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.infoLabel.stringValue = @"Points In/Out effacés";
    self.state->overlayDirty = true;
}

// M: a marker where the playhead is, at once and without a question, so
// that it can be dropped while watching. On a marker, M opens its note.
- (void)menuAddMarker:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (![self markersBelongToTimeline]) return;
    const DocumentMarker* existing = TimelineMarkerAtPlayhead(
        self.state->document, self.state->requestedPosition,
        [self playheadFrameRate]);
    if (existing) {
        const Ulid existingId = existing->id;
        [self editMarkerNote:existingId];
        return;
    }
    DocumentMarker marker;
    marker.name = kTimelineMarkerDefaultName;
    marker.color = kTimelineMarkerEditorColor;
    marker.time = self.state->requestedPosition;
    if ([self applyMarkerOperation:AddMarkerOperation{marker, -1}])
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MARQUEUR  %@    M à nouveau pour l’annoter",
                             TimelineTimecode(marker.time,
                                              [self playheadFrameRate])];
}

// Shift+M: the note first. Given up, nothing was added -- one marker and
// its note are one undo step, not two.
- (void)menuAnnotateMarker:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (![self markersBelongToTimeline]) return;
    const DocumentMarker* existing = TimelineMarkerAtPlayhead(
        self.state->document, self.state->requestedPosition,
        [self playheadFrameRate]);
    if (existing) {
        const Ulid existingId = existing->id;
        [self editMarkerNote:existingId];
        return;
    }
    DocumentMarker marker;
    marker.color = kTimelineMarkerEditorColor;
    marker.time = self.state->requestedPosition;
    // D6 -- with an In/Out range set on the timeline, the note is about
    // that passage: the marker spans it. The keyboard's way to what
    // Option-drag does with the mouse.
    if ([self hasValidTimelineRange]) {
        marker.time = *self.state->timelineIn;
        marker.duration = self.state->timelineOut->sub(*self.state->timelineIn);
    }
    [self
        askMarkerNote:@""
                 time:marker.time
             duration:marker.duration
            removable:NO
               answer:^(NSModalResponse response, NSString* typed) {
                 if (response != NSAlertFirstButtonReturn) return;
                 DocumentMarker added = marker;
                 added.name =
                     TimelineMarkerNameFromNote(typed.UTF8String ?: "");
                 if ([self applyMarkerOperation:AddMarkerOperation{added, -1}])
                     [self showMarkerInfo:added];
               }];
}

- (void)menuRemoveMarker:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (![self markersBelongToTimeline]) return;
    const DocumentMarker* existing = TimelineMarkerAtPlayhead(
        self.state->document, self.state->requestedPosition,
        [self playheadFrameRate]);
    if (!existing) {
        self.infoLabel.stringValue = @"Aucun marqueur sous la tête de lecture";
        return;
    }
    if ([self applyMarkerOperation:RemoveMarkerOperation{existing->id}])
        self.infoLabel.stringValue = @"Marqueur supprimé";
}

- (void)menuNextMarker:(id)sender {
    (void)sender;
    [self goToAdjacentMarkerForward:YES];
}

- (void)menuPreviousMarker:(id)sender {
    (void)sender;
    [self goToAdjacentMarkerForward:NO];
}

- (void)menuEditCartonText:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (!self.state || !self.state->interaction) return;
    const auto carton = ui::media_panel::SelectedCartonForText(
        self.state->document, self.state->interaction->SelectedClipIds());
    if (!carton) {
        self.infoLabel.stringValue =
            @"Sélectionner un carton pour modifier son texte";
        return;
    }
    const Ulid edited = *carton;
    [self editCartonText:edited];
}

- (void)menuInsertSource:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self performSourceEditInsert:YES];
}

- (void)menuOverwriteSource:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self performSourceEditInsert:NO];
}

- (void)menuCutSelectedAtPlayhead:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    const Ulid clipId = self.state->interaction->SelectedClipId();
    if (clipId.empty()) return;
    self.state->contextClipId = clipId;
    self.state->contextTime = self.state->requestedPosition;
    [self contextCutClip:nil];
}

- (void)menuAddCrossDissolve:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    const DocumentTrack* chosenTrack = nullptr;
    const DocumentClip* left = nullptr;
    const DocumentClip* right = nullptr;
    const Ulid selected = self.state->interaction->SelectedClipId();
    const auto choosePair = [&](const DocumentTrack& track, size_t index) {
        // EF2 -- a sound track takes a cross fade.
        if ((track.kind != "video" && track.kind != "audio") ||
            index + 1 >= track.clips.size())
            return false;
        const DocumentClip& candidateLeft = track.clips[index];
        const DocumentClip& candidateRight = track.clips[index + 1];
        if (candidateLeft.timeline_in.add(candidateLeft.duration) !=
            candidateRight.timeline_in)
            return false;
        chosenTrack = &track;
        left = &candidateLeft;
        right = &candidateRight;
        return true;
    };
    if (!selected.empty()) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            for (size_t index = 0; index < track.clips.size(); ++index) {
                if (track.clips[index].id != selected) continue;
                if (!choosePair(track, index) && index > 0)
                    choosePair(track, index - 1);
                break;
            }
            if (left) break;
        }
    }
    if (!left) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            for (size_t index = 0; index + 1 < track.clips.size(); ++index) {
                if (track.clips[index + 1].timeline_in ==
                        self.state->requestedPosition &&
                    choosePair(track, index))
                    break;
            }
            if (left) break;
        }
    }
    if (!left || !right || !chosenTrack) {
        self.infoLabel.stringValue = @"Placez le playhead sur un cut ou "
                                     @"sélectionnez un clip adjacent";
        return;
    }
    for (const DocumentTransition& existing :
         self.state->document.sequence.transitions) {
        if (existing.left_clip_id == left->id &&
            existing.right_clip_id == right->id) {
            self.infoLabel.stringValue = @"Ce cut possède déjà une transition";
            return;
        }
    }
    // REMOTION-PARITY-2026-09 REM5 -- what the left clip reads: its media,
    // or for a motion clip its composition (ClipSource, M4). A motion clip
    // used to have no source here and the menu did nothing (the M4 usage
    // gap of VISUAL_QA_CHECKLIST.md).
    const std::optional<DocumentSource> leftSource =
        ClipSource(self.state->document, *left);
    if (!leftSource) return;
    const MediaRate rate = self.state->document.sequence.frame_rate;
    const RationalTime tail =
        leftSource->duration.sub(left->source_in.add(left->duration));
    const int64_t leftHandle = tail.to_frames(rate.num, rate.den);
    const int64_t rightHandle = right->source_in.to_frames(rate.num, rate.den);
    const int64_t halfFrames = std::min<int64_t>({6, leftHandle, rightHandle});
    if (halfFrames < 1) {
        self.infoLabel.stringValue =
            @"Fondu impossible : il manque des poignées média de chaque côté";
        return;
    }
    DocumentTransition transition;
    transition.track_id = chosenTrack->id;
    transition.left_clip_id = left->id;
    transition.right_clip_id = right->id;
    transition.duration = {halfFrames * 2 * rate.den, rate.num};
    // EF2 -- on a sound track, the sound's transition.
    const bool crossFade = chosenTrack->kind == "audio";
    if (crossFade) transition.type = kTransitionCrossFade;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddTransitionOperation { transition }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Transition refusée (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ · %lld images",
                         crossFade ? @"Fondu croisé" : @"Fondu enchaîné",
                         static_cast<long long>(halfFrames * 2)];
    self.state->overlayDirty = true;
}

- (void)menuRemoveCrossDissolve:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    const Ulid selected = self.state->interaction->SelectedClipId();
    const DocumentTransition* chosen = nullptr;
    for (const DocumentTransition& transition :
         self.state->document.sequence.transitions) {
        const DocumentClip* right =
            self.state->document.FindClip(transition.right_clip_id);
        const bool selectedCut =
            !selected.empty() && (transition.left_clip_id == selected ||
                                  transition.right_clip_id == selected);
        const bool playheadCut =
            right && right->timeline_in == self.state->requestedPosition;
        if (selectedCut || playheadCut) {
            chosen = &transition;
            break;
        }
    }
    if (!chosen) {
        self.infoLabel.stringValue =
            @"Aucun fondu au raccord ou au clip sélectionné";
        return;
    }
    const Ulid transitionId = chosen->id;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveTransitionOperation { transitionId }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = @"Fondu enchaîné supprimé";
    self.state->overlayDirty = true;
}

- (void)menuActivateMulticamAngle:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self applyMulticamSwitch:NO];
}

- (void)menuClearMulticamAngle:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self applyMulticamSwitch:YES];
}

- (void)menuAutoAlignByWaveform:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self alignSelectedSoundByWaveform];
}

- (void)menuSelectTool:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self setTimelineTool:TimelineTool::Select];
}

- (void)menuHandTool:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self setTimelineTool:TimelineTool::Hand];
}

- (void)menuZoomTool:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self setTimelineTool:TimelineTool::Zoom];
}

- (void)menuCutTool:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self setTimelineTool:TimelineTool::Cut];
}

- (void)menuRetimeControls:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self toggleRetimeControls];
}

- (void)menuToggleSnapping:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    self.state->interaction->SetSnappingEnabled(
        !self.state->interaction->SnappingEnabled());
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)menuToggleLinkedSelection:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self setLinkedSelectionEnabled:!self.state->linkedSelection];
}

// Out of lot -- the author, 29 October 2026 (Alt+Y): Resolve's Select All Clips
// Forward, from the playhead, on every unlocked track.
- (void)menuSelectClipsAfter:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (!self.state) return;
    self.state->interaction->SelectClips(TimelineClipsAfter(
        self.state->document, self.state->requestedPosition));
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

// Out of lot -- the author, 29 October 2026: "D on a selected shot disables it:
// greyed, no longer seen or heard". Resolve's toggle on the whole selection
// (ToggleClipsEnabled, ClipEnabled.h): any clip enabled, all are disabled;
// none, all are enabled. One operation, one undo step.
- (void)menuToggleClipsEnabled:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    if (!self.state || !self.state->interaction) return;
    const SetClipEnabledOperation operation = ToggleClipsEnabled(
        self.state->document, self.state->interaction->SelectedClipIds());
    if (operation.clips.empty()) {
        NSBeep();
        return;
    }
    const bool enabling = operation.clips.front().enabled;
    const size_t count = operation.clips.size();
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{operation}
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Plans non modifiés : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%lu plan%@ %@. ⌘Z l’annule.", (unsigned long)count,
                         count == 1 ? @"" : @"s",
                         enabling
                             ? (count == 1 ? @"activé" : @"activés")
                             : (count == 1 ? @"désactivé" : @"désactivés")];
}

- (void)menuFitTimeline:(id)sender {
    (void)sender;
    [self fitTimelineToViewportWidth];
}

- (void)menuAddVideoTrack:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self addTrack:NO];
}

- (void)menuAddAudioTrack:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self addTrack:YES];
}

- (void)menuPlayPause:(id)sender {
    (void)sender;
    [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1 : 0];
}

- (void)menuPlayReverse:(id)sender {
    (void)sender;
    if ([self dynamicTrimTransport:-1]) return;
    [self setPlaybackDirection:-1];
}

- (void)menuStop:(id)sender {
    (void)sender;
    // FC4 -- K commits a dynamic trim, as one edit.
    if ([self dynamicTrimTransport:0]) return;
    [self setPlaybackDirection:0];
}

- (void)menuPlayForward:(id)sender {
    (void)sender;
    if ([self dynamicTrimTransport:1]) return;
    [self setPlaybackDirection:1];
}

- (void)keyboardKeyPressed:(NSButton*)sender {
    NSString* identifier =
        self.keyboardCommandPopup.selectedItem.representedObject;
    NSTextField* field = self.keyboardEditingFields[identifier];
    if (!field || sender.identifier.length == 0) return;
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (self.keyboardCommandModifier.state == NSControlStateValueOn)
        [parts addObject:@"Cmd"];
    if (self.keyboardOptionModifier.state == NSControlStateValueOn)
        [parts addObject:@"Alt"];
    if (self.keyboardControlModifier.state == NSControlStateValueOn)
        [parts addObject:@"Ctrl"];
    if (self.keyboardShiftModifier.state == NSControlStateValueOn)
        [parts addObject:@"Shift"];
    [parts addObject:sender.identifier];
    field.stringValue = [parts componentsJoinedByString:@"+"];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ affecté à %@", field.stringValue,
                         self.keyboardCommandPopup.selectedItem.title];
}

- (NSButton*)keyboardModifierButton:(NSString*)title frame:(NSRect)frame {
    NSButton* button = [NSButton checkboxWithTitle:title target:nil action:nil];
    button.frame = frame;
    button.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightMedium];
    return button;
}

- (NSView*)keyboardViewForDefinitions:(NSArray*)definitions
                               fields:(NSMutableDictionary*)fields {
    NSView* keyboard =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 255)];
    NSTextField* instruction = [NSTextField
        labelWithString:@"Choisissez une commande, activez les modificateurs "
                        @"puis cliquez sur une touche."];
    instruction.frame = NSMakeRect(0, 229, 650, 20);
    instruction.textColor = NSColor.secondaryLabelColor;
    [keyboard addSubview:instruction];

    self.keyboardEditingFields = fields;
    self.keyboardCommandPopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 197, 315, 27)];
    for (NSDictionary* definition in definitions) {
        [self.keyboardCommandPopup addItemWithTitle:definition[@"title"]];
        self.keyboardCommandPopup.lastItem.representedObject =
            definition[@"id"];
    }
    [keyboard addSubview:self.keyboardCommandPopup];
    self.keyboardCommandModifier =
        [self keyboardModifierButton:@"⌘ Cmd"
                               frame:NSMakeRect(330, 199, 72, 24)];
    self.keyboardOptionModifier =
        [self keyboardModifierButton:@"⌥ Alt"
                               frame:NSMakeRect(402, 199, 68, 24)];
    self.keyboardControlModifier =
        [self keyboardModifierButton:@"⌃ Ctrl"
                               frame:NSMakeRect(470, 199, 72, 24)];
    self.keyboardShiftModifier =
        [self keyboardModifierButton:@"⇧ Shift"
                               frame:NSMakeRect(542, 199, 85, 24)];
    for (NSButton* modifier in @[
             self.keyboardCommandModifier, self.keyboardOptionModifier,
             self.keyboardControlModifier, self.keyboardShiftModifier
         ])
        [keyboard addSubview:modifier];

    NSArray<NSArray<NSString*>*>* rows = @[
        @[
            @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"0",
            @"Delete"
        ],
        @[ @"A", @"Z", @"E", @"R", @"T", @"Y", @"U", @"I", @"O", @"P" ],
        @[ @"Q", @"S", @"D", @"F", @"G", @"H", @"J", @"K", @"L", @"M" ],
        @[ @"W", @"X", @"C", @"V", @"B", @"N", @",", @".", @"Space" ]
    ];
    const CGFloat keyHeight = 35.0;
    const CGFloat gap = 5.0;
    for (NSUInteger row = 0; row < rows.count; ++row) {
        CGFloat x = row == 0 ? 0.0 : row * 12.0;
        const CGFloat y = 151.0 - row * (keyHeight + gap);
        for (NSString* key in rows[row]) {
            CGFloat width = [key isEqualToString:@"Space"]    ? 170.0
                            : [key isEqualToString:@"Delete"] ? 70.0
                                                              : 48.0;
            NSString* title = [key isEqualToString:@"Space"]    ? @"Espace"
                              : [key isEqualToString:@"Delete"] ? @"⌫"
                                                                : key;
            NSButton* button =
                [NSButton buttonWithTitle:title
                                   target:self
                                   action:@selector(keyboardKeyPressed:)];
            button.frame = NSMakeRect(x, y, width, keyHeight);
            button.identifier = key;
            button.bezelStyle = NSBezelStyleTexturedRounded;
            button.font = [NSFont systemFontOfSize:12.0
                                            weight:NSFontWeightMedium];
            [keyboard addSubview:button];
            x += width + gap;
        }
    }
    return keyboard;
}

@end
