// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The media
// panel's « Audio » and « Légendes » (captions) tabs (F2.3): the tab switch,
// the audio list and its search, and the caption styles' list, controls and
// buttons. Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (MediaTabs)

- (void)mediaTabChanged:(CMTabStripView*)sender {
    const NSInteger index = sender.selectedIndex;
    const auto& tabs = ui::media_panel::AllTabs();
    if (index < 0 || index >= static_cast<NSInteger>(tabs.size())) return;
    const ui::media_panel::Tab tab = tabs[index];
    self.mediaTabContentMedia.hidden = tab != ui::media_panel::Tab::Media;
    self.mediaTabContentAudio.hidden = tab != ui::media_panel::Tab::Audio;
    self.mediaTabContentCaptions.hidden = tab != ui::media_panel::Tab::Captions;
    self.effectsPanelView.hidden = tab != ui::media_panel::Tab::Effects;
    [CMUiPreferences.sharedPreferences
        setLastMediaPanelTab:
            [NSString
                stringWithUTF8String:ui::media_panel::TabIdentifier(tab)]];
    switch (tab) {
        case ui::media_panel::Tab::Media:
            [self rebuildMediaList];
            break;
        case ui::media_panel::Tab::Audio:
            [self rebuildAudioList];
            break;
        case ui::media_panel::Tab::Captions:
            [self rebuildCaptionStylesList];
            break;
        case ui::media_panel::Tab::Effects:
            break;
    }
}

- (void)buildAudioTabContentWithWidth:(double)width height:(double)height {
    self.mediaTabContentAudio =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    self.mediaTabContentAudio.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentAudio];

    NSTextField* audioTitle = CMMakeSectionHeader(@"SOURCES AUDIO");
    audioTitle.frame = NSMakeRect(14.0, height - 34.0, width - 28.0, 18.0);
    audioTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentAudio addSubview:audioTitle];

    self.audioSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(12.0, height - 70.0, width - 24.0, 26.0)];
    self.audioSearchField.placeholderString = @"Rechercher nom, codec…";
    self.audioSearchField.target = self;
    self.audioSearchField.action = @selector(audioSearchChanged:);
    self.audioSearchField.continuous = YES;
    self.audioSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentAudio addSubview:self.audioSearchField];

    self.audioTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"140" ], @[ @"format", @"Format", @"90" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.audioTable addTableColumn:column];
    }
    self.audioTable.dataSource = self;
    self.audioTable.delegate = self;
    self.audioTable.allowsMultipleSelection = YES;
    self.audioTable.rowHeight = 26.0;
    self.audioTable.usesAlternatingRowBackgroundColors = NO;
    self.audioTable.backgroundColor = CMSurfacePanelColor();
    self.audioTable.target = self;
    self.audioTable.doubleAction = @selector(openSelectedAudioInSourceMonitor:);
    self.audioScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 84.0, width - 24.0, height - 164.0)];
    self.audioScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.audioScroll.documentView = self.audioTable;
    self.audioScroll.hasVerticalScroller = YES;
    self.audioScroll.hasHorizontalScroller = YES;
    self.audioScroll.borderType = NSNoBorder;
    self.audioScroll.drawsBackground = YES;
    self.audioScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentAudio addSubview:self.audioScroll];

    NSButton* openAudioButton = CMMakeStyledButton(
        @"SOURCE", self, @selector(openSelectedAudioInSourceMonitor:));
    openAudioButton.frame = NSMakeRect(12.0, 48.0, 140.0, 28.0);
    openAudioButton.autoresizingMask = NSViewMaxYMargin;
    openAudioButton.toolTip = @"Ouvrir la source dans le moniteur Source";
    [self.mediaTabContentAudio addSubview:openAudioButton];

    NSButton* assignAudioButton = CMMakeStyledButton(
        @"DÉPLACER…", self, @selector(assignSelectedAudioToBinPressed:));
    assignAudioButton.frame = NSMakeRect(width - 152.0, 48.0, 140.0, 28.0);
    assignAudioButton.autoresizingMask = NSViewMaxYMargin | NSViewMinXMargin;
    assignAudioButton.toolTip = @"Déplacer les sources vers un chutier";
    [self.mediaTabContentAudio addSubview:assignAudioButton];

    self.audioSummaryLabel = [NSTextField labelWithString:@""];
    self.audioSummaryLabel.frame = NSMakeRect(14.0, 8.0, width - 28.0, 32.0);
    self.audioSummaryLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.audioSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.audioSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.audioSummaryLabel.maximumNumberOfLines = 2;
    self.audioSummaryLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [self.mediaTabContentAudio addSubview:self.audioSummaryLabel];
}

- (void)audioSearchChanged:(id)sender {
    (void)sender;
    [self rebuildAudioList];
}

- (void)rebuildAudioList {
    if (!self.state) return;
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    const bool anyBin = selected == "__all__";
    const bool wantRoot = selected == "__root__";
    const Ulid wantBinId = anyBin || wantRoot ? Ulid{} : selected;
    const std::string search(self.audioSearchField.stringValue.UTF8String
                                 ?: "");
    const auto matches = ui::media_panel::FilterAudioSources(
        self.state->document.library, anyBin, wantRoot, wantBinId, search);
    // F2.3 -- every tab change rebuilds this list, and reloadData dropped
    // the selection: a source chosen here was gone after a look at the
    // Captions tab (agentic QA of 23 September 2026). Kept by identity, so
    // a source the new filter hides simply leaves the selection.
    NSSet<NSString*>* previouslySelected =
        [NSSet setWithArray:[self selectedAudioIds]];
    self.visibleAudioIds = [NSMutableArray array];
    for (const LibraryMedia* media : matches)
        [self.visibleAudioIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.audioTable reloadData];
    NSMutableIndexSet* reselected = [NSMutableIndexSet indexSet];
    for (NSUInteger row = 0; row < self.visibleAudioIds.count; ++row)
        if ([previouslySelected containsObject:self.visibleAudioIds[row]])
            [reselected addIndex:row];
    if (reselected.count > 0)
        [self.audioTable selectRowIndexes:reselected byExtendingSelection:NO];
    NSString* scope = @"tous les chutiers";
    if (wantRoot) {
        scope = @"la racine du projet";
    } else if (!anyBin) {
        const DocumentBin* bin = self.state->document.FindBin(wantBinId);
        // Not %s: it reads the name in the system C-string encoding, not
        // UTF-8, and garbles every accented bin name.
        NSString* name = bin != nullptr
                             ? [NSString stringWithUTF8String:bin->name.c_str()]
                             : nil;
        scope = name != nil
                    ? [NSString stringWithFormat:@"le chutier « %@ »", name]
                    : @"un chutier";
    }
    const NSUInteger count = self.visibleAudioIds.count;
    self.audioSummaryLabel.stringValue = [NSString
        stringWithFormat:@"%lu source%@ audio dans %@ (chutier choisi dans "
                         @"l’onglet Média)",
                         (unsigned long)count, count == 1 ? @"" : @"s", scope];
}

- (NSString*)selectedAudioId {
    return [self selectedAudioIds].firstObject;
}

- (NSArray<NSString*>*)selectedAudioIds {
    NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
    [self.audioTable.selectedRowIndexes
        enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
          (void)stop;
          if (row < self.visibleAudioIds.count)
              [identifiers addObject:self.visibleAudioIds[row]];
        }];
    return identifiers;
}

- (void)openSelectedAudioInSourceMonitor:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedAudioId];
    if (!identifier) return;
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (void)assignSelectedAudioToBinPressed:(id)sender {
    (void)sender;
    [self moveMediaIdsToBin:[self selectedAudioIds]];
}

// VQA1 -- keeps the caption style table exactly as wide as its clip view:
// the count column keeps the width its "Cartons" header needs and the style
// name takes what is left. The automatic table style resolves to the inset
// style, whose side padding -tile adds to the frame, so the padding is
// measured after a first tile instead of being assumed.
- (void)layoutCaptionStyleColumns {
    NSTableView* table = self.captionStyleTable;
    if (!table) return;
    NSTableColumn* style = [table tableColumnWithIdentifier:@"style"];
    NSTableColumn* clips = [table tableColumnWithIdentifier:@"clips"];
    const double available =
        NSWidth(self.captionStyleScroll.contentView.bounds);
    const double spacing = 2.0 * table.intercellSpacing.width;
    style.width = std::max(40.0, available - clips.width - spacing);
    [table tile];
    const double overflow = NSWidth(table.frame) - available;
    if (overflow > 0.0) {
        style.width = std::max(40.0, style.width - overflow);
        [table tile];
    }
}

- (void)buildCaptionsTabContentWithWidth:(double)width height:(double)height {
    self.mediaTabContentCaptions =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    self.mediaTabContentCaptions.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentCaptions];

    NSTextField* stylesTitle = CMMakeSectionHeader(@"STYLES DE LÉGENDE");
    stylesTitle.frame = NSMakeRect(14.0, height - 34.0, width - 28.0, 18.0);
    stylesTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:stylesTitle];

    // F2.3 -- 140 pt rather than 220 since the style's look is set below:
    // everything still fits over the window's minimum height.
    self.captionStyleTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 140.0)];
    self.captionStyleTable.autoresizingMask = NSViewWidthSizable;
    // VQA1 -- uniform resizing shrank the count column below its header and
    // clipped "Cartons"; with any AppKit policy the columns were stretched
    // while the pane was still wide and the table never shrank back with
    // its clip view, leaving the column past the visible edge. Columns are
    // therefore laid out by -layoutCaptionStyleColumns, not by AppKit.
    self.captionStyleTable.columnAutoresizingStyle =
        NSTableViewNoColumnAutoresizing;
    for (NSArray<NSString*>* definition in @[
             @[ @"style", @"Style", @"196" ], @[ @"clips", @"Cartons", @"76" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        column.resizingMask = NSTableColumnNoResizing;
        [self.captionStyleTable addTableColumn:column];
    }
    self.captionStyleTable.dataSource = self;
    self.captionStyleTable.delegate = self;
    self.captionStyleTable.rowHeight = 26.0;
    self.captionStyleTable.usesAlternatingRowBackgroundColors = NO;
    self.captionStyleTable.backgroundColor = CMSurfacePanelColor();
    self.captionStyleScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, height - 184.0, width - 24.0, 140.0)];
    self.captionStyleScroll.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionStyleScroll.documentView = self.captionStyleTable;
    self.captionStyleScroll.hasVerticalScroller = YES;
    self.captionStyleScroll.borderType = NSNoBorder;
    self.captionStyleScroll.drawsBackground = YES;
    self.captionStyleScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentCaptions addSubview:self.captionStyleScroll];
    NSClipView* captionStyleClip = self.captionStyleScroll.contentView;
    captionStyleClip.postsFrameChangedNotifications = YES;
    __weak AppDelegate* weakSelf = self;
    [NSNotificationCenter.defaultCenter
        addObserverForName:NSViewFrameDidChangeNotification
                    object:captionStyleClip
                     queue:nil
                usingBlock:^(NSNotification*) {
                  [weakSelf layoutCaptionStyleColumns];
                }];
    [self layoutCaptionStyleColumns];

    self.addCaptionStyleButton = CMMakeStyledButton(
        @"+ STYLE", self, @selector(createCaptionStylePressed:));
    self.addCaptionStyleButton.frame =
        NSMakeRect(12.0, height - 220.0, 140.0, 28.0);
    self.addCaptionStyleButton.autoresizingMask = NSViewMinYMargin;
    self.addCaptionStyleButton.toolTip = @"Nouveau style de légende";
    [self.mediaTabContentCaptions addSubview:self.addCaptionStyleButton];

    self.removeCaptionStyleButton = CMMakeStyledButton(
        @"− STYLE", self, @selector(deleteCaptionStylePressed:));
    self.removeCaptionStyleButton.frame =
        NSMakeRect(width - 152.0, height - 220.0, 140.0, 28.0);
    self.removeCaptionStyleButton.autoresizingMask =
        NSViewMinYMargin | NSViewMinXMargin;
    self.removeCaptionStyleButton.toolTip =
        @"Supprimer le style sélectionné, s’il n’habille aucun carton ; un "
        @"plan vidéo qui le porte sans l’afficher le perd";
    [self.mediaTabContentCaptions addSubview:self.removeCaptionStyleButton];

    // F2.3 -- the look of the selected style: font, size, text colour. Each
    // change is one UpdateCaptionStyleOperation (RestyleCaptionStyle), so
    // every carton wearing the style follows, and one undo takes it back.
    NSTextField* lookTitle = CMMakeSectionHeader(@"ASPECT DU STYLE");
    lookTitle.frame = NSMakeRect(14.0, height - 254.0, width - 28.0, 18.0);
    lookTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:lookTitle];
    const auto lookLabel = [&](NSString* title, double x, double y) {
        NSTextField* label = [NSTextField labelWithString:title];
        label.frame = NSMakeRect(x, y + 3.0, 52.0, 18.0);
        label.autoresizingMask = NSViewMinYMargin;
        label.font = [NSFont systemFontOfSize:11.0];
        label.textColor = NSColor.secondaryLabelColor;
        [self.mediaTabContentCaptions addSubview:label];
    };
    lookLabel(@"Police", 14.0, height - 286.0);
    self.captionFontPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(68.0, height - 288.0, width - 80.0, 26.0)
            pullsDown:NO];
    self.captionFontPopup.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionFontPopup.controlSize = NSControlSizeSmall;
    self.captionFontPopup.font = [NSFont systemFontOfSize:11.0];
    // "system" is what the monitor and the export both draw in Helvetica
    // Neue (CaptionFontName); every other entry is an installed family.
    CMFillCaptionFontPopup(self.captionFontPopup);
    self.captionFontPopup.target = self;
    self.captionFontPopup.action = @selector(captionLookChanged:);
    self.captionFontPopup.toolTip = @"Police des cartons de ce style";
    [self.mediaTabContentCaptions addSubview:self.captionFontPopup];

    lookLabel(@"Taille", 14.0, height - 318.0);
    self.captionSizeField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(68.0, height - 316.0, 52.0, 22.0)];
    self.captionSizeField.autoresizingMask = NSViewMinYMargin;
    self.captionSizeField.font =
        [NSFont monospacedDigitSystemFontOfSize:11.0
                                         weight:NSFontWeightRegular];
    NSNumberFormatter* sizeFormat = [[NSNumberFormatter alloc] init];
    sizeFormat.numberStyle = NSNumberFormatterNoStyle;
    sizeFormat.allowsFloats = NO;
    sizeFormat.minimum = @(ui::media_panel::kCaptionTabMinimumFontSize);
    sizeFormat.maximum = @(ui::media_panel::kCaptionTabMaximumFontSize);
    self.captionSizeField.formatter = sizeFormat;
    self.captionSizeField.target = self;
    self.captionSizeField.action = @selector(captionLookChanged:);
    self.captionSizeField.toolTip =
        @"Taille en pixels à la résolution de la séquence";
    [self.mediaTabContentCaptions addSubview:self.captionSizeField];
    self.captionSizeStepper = [[NSStepper alloc]
        initWithFrame:NSMakeRect(122.0, height - 318.0, 19.0, 26.0)];
    self.captionSizeStepper.autoresizingMask = NSViewMinYMargin;
    self.captionSizeStepper.minValue =
        ui::media_panel::kCaptionTabMinimumFontSize;
    self.captionSizeStepper.maxValue =
        ui::media_panel::kCaptionTabMaximumFontSize;
    self.captionSizeStepper.increment = 2.0;
    self.captionSizeStepper.valueWraps = NO;
    self.captionSizeStepper.target = self;
    self.captionSizeStepper.action = @selector(captionLookChanged:);
    [self.mediaTabContentCaptions addSubview:self.captionSizeStepper];

    lookLabel(@"Couleur", 160.0, height - 318.0);
    self.captionColorWell = [[NSColorWell alloc]
        initWithFrame:NSMakeRect(214.0, height - 318.0, 44.0, 26.0)];
    self.captionColorWell.autoresizingMask = NSViewMinYMargin;
    // One edit when the colour is chosen, not one per step of the drag.
    self.captionColorWell.continuous = NO;
    self.captionColorWell.target = self;
    self.captionColorWell.action = @selector(captionLookChanged:);
    self.captionColorWell.toolTip =
        @"Couleur du texte ; l’opacité enregistrée est gardée";
    [self.mediaTabContentCaptions addSubview:self.captionColorWell];

    NSTextField* selectionTitle = CMMakeSectionHeader(@"CARTONS SÉLECTIONNÉS");
    selectionTitle.frame = NSMakeRect(14.0, height - 352.0, width - 28.0, 18.0);
    selectionTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:selectionTitle];

    self.captionSelectionLabel = [NSTextField labelWithString:@""];
    self.captionSelectionLabel.frame =
        NSMakeRect(14.0, height - 404.0, width - 28.0, 48.0);
    self.captionSelectionLabel.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionSelectionLabel.font = [NSFont systemFontOfSize:11.0];
    self.captionSelectionLabel.textColor = NSColor.secondaryLabelColor;
    self.captionSelectionLabel.maximumNumberOfLines = 3;
    self.captionSelectionLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [self.mediaTabContentCaptions addSubview:self.captionSelectionLabel];

    self.applyCaptionStyleButton = CMMakeStyledButton(
        @"APPLIQUER LE STYLE", self, @selector(applyCaptionStylePressed:));
    self.applyCaptionStyleButton.frame =
        NSMakeRect(12.0, height - 442.0, 140.0, 28.0);
    self.applyCaptionStyleButton.autoresizingMask = NSViewMinYMargin;
    self.applyCaptionStyleButton.toolTip =
        @"Habiller les cartons sélectionnés du style choisi, texte conservé";
    [self.mediaTabContentCaptions addSubview:self.applyCaptionStyleButton];

    self.clearCaptionButton = CMMakeStyledButton(
        @"STYLE PAR DÉFAUT", self, @selector(clearCaptionPressed:));
    self.clearCaptionButton.frame =
        NSMakeRect(width - 152.0, height - 442.0, 140.0, 28.0);
    self.clearCaptionButton.autoresizingMask =
        NSViewMinYMargin | NSViewMinXMargin;
    self.clearCaptionButton.toolTip =
        @"Rendre aux cartons sélectionnés l’aspect par défaut, texte conservé";
    [self.mediaTabContentCaptions addSubview:self.clearCaptionButton];
}

- (void)rebuildCaptionStylesList {
    if (!self.state) return;
    const Ulid selectedStyle = [self selectedCaptionStyleId];
    const auto summaries =
        ui::media_panel::SummarizeCaptionStyles(self.state->document.sequence);
    self.visibleCaptionStyleIds = [NSMutableArray array];
    for (const auto& summary : summaries)
        [self.visibleCaptionStyleIds
            addObject:[NSString stringWithUTF8String:summary.style_id.c_str()]];
    [self.captionStyleTable reloadData];
    // A reload loses the table's selection; keep the style chosen, which the
    // buttons below act on.
    for (NSUInteger row = 0; row < self.visibleCaptionStyleIds.count; ++row)
        if (selectedStyle ==
            (self.visibleCaptionStyleIds[row].UTF8String ?: ""))
            [self.captionStyleTable
                    selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
    [self updateCaptionSelectionLabel];
}

// F2.3 -- the look controls show the selected style, and are off without
// one.
- (void)updateCaptionStyleControls {
    const Ulid styleId = [self selectedCaptionStyleId];
    const CaptionStyle* style =
        self.state && !styleId.empty()
            ? self.state->document.FindCaptionStyle(styleId)
            : nullptr;
    for (NSControl* control in @[
             self.captionFontPopup, self.captionSizeField,
             self.captionSizeStepper, self.captionColorWell
         ])
        control.enabled = style != nullptr;
    if (!style) {
        self.captionSizeField.stringValue = @"";
        return;
    }
    // A family set through MCP that this Mac does not have: shown as it is,
    // so choosing nothing changes nothing.
    CMSelectCaptionFont(self.captionFontPopup, style->font_family);
    self.captionSizeField.integerValue = style->font_size;
    self.captionSizeStepper.integerValue = style->font_size;
    self.captionColorWell.color =
        CMCaptionColor(style->color, NSColor.whiteColor);
}

- (void)captionLookChanged:(id)sender {
    if (!self.state) return;
    const Ulid styleId = [self selectedCaptionStyleId];
    const CaptionStyle* stored =
        styleId.empty() ? nullptr
                        : self.state->document.FindCaptionStyle(styleId);
    if (!stored) return;
    if (sender == self.captionSizeStepper)
        self.captionSizeField.integerValue =
            self.captionSizeStepper.integerValue;
    ui::media_panel::CaptionStyleLook look;
    NSString* family = self.captionFontPopup.selectedItem.representedObject;
    look.font_family = family.UTF8String ?: "";
    look.font_size = static_cast<int32_t>(self.captionSizeField.integerValue);
    look.color = stored->color;
    // The well shows the stored colour through NSColor, which rounds: only
    // a colour actually chosen replaces the stored spelling.
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    if (CMChosenCaptionChannels(self.captionColorWell.color, stored->color, red,
                                green, blue))
        look.color = ui::media_panel::CaptionColorFromWell(stored->color, red,
                                                           green, blue);
    std::string refusal;
    const auto update = ui::media_panel::RestyleCaptionStyle(
        self.state->document.sequence, styleId, look, refusal);
    if (!update) {
        if (!refusal.empty())
            self.captionSelectionLabel.stringValue =
                [NSString stringWithFormat:@"Aspect refusé : %@",
                                           CMStringFromUtf8(refusal)];
        [self updateCaptionStyleControls];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{*update}
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Aspect refusé : %@", CMStringFromUtf8(message)];
        [self updateCaptionStyleControls];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
}

- (void)updateCaptionSelectionLabel {
    const Ulid styleId = [self selectedCaptionStyleId];
    self.removeCaptionStyleButton.enabled = !styleId.empty();
    [self updateCaptionStyleControls];
    if (!self.state || !self.state->interaction) {
        self.captionSelectionLabel.stringValue = @"Aucune timeline chargée.";
        self.applyCaptionStyleButton.enabled = NO;
        self.clearCaptionButton.enabled = NO;
        return;
    }
    const ui::media_panel::CaptionSelection selection =
        ui::media_panel::DescribeCaptionSelection(
            self.state->document, self.state->interaction->SelectedClipIds(),
            styleId);
    self.applyCaptionStyleButton.enabled =
        !styleId.empty() && selection.in_style < selection.cartons.size();
    self.clearCaptionButton.enabled = !selection.cartons.empty();
    if (selection.cartons.empty()) {
        self.captionSelectionLabel.stringValue =
            selection.other_clips == 0
                ? @"Sélectionnez des cartons sur une piste de légendes."
                : @"Aucun carton dans la sélection : un style n’habille que "
                  @"les cartons d’une piste de légendes.";
        return;
    }
    const size_t cartons = selection.cartons.size();
    NSMutableString* text = [NSMutableString
        stringWithFormat:@"%zu carton%@ sélectionné%@", cartons,
                         cartons == 1 ? @"" : @"s", cartons == 1 ? @"" : @"s"];
    if (!styleId.empty())
        [text appendFormat:@" · %zu déjà dans ce style", selection.in_style];
    if (selection.other_clips > 0)
        [text appendFormat:@" · %zu autre%@ clip%@ ignoré%@",
                           selection.other_clips,
                           selection.other_clips == 1 ? @"" : @"s",
                           selection.other_clips == 1 ? @"" : @"s",
                           selection.other_clips == 1 ? @"" : @"s"];
    if (styleId.empty())
        [text appendString:@". Choisissez un style dans la liste."];
    self.captionSelectionLabel.stringValue = text;
}

- (Ulid)selectedCaptionStyleId {
    const NSInteger row = self.captionStyleTable.selectedRow;
    if (row < 0 ||
        row >= static_cast<NSInteger>(self.visibleCaptionStyleIds.count))
        return {};
    return Ulid(self.visibleCaptionStyleIds[row].UTF8String ?: "");
}

- (void)createCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state) return;
    const CaptionStyle style;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddCaptionStyleOperation { style, -1 }
        }
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue =
            [NSString stringWithFormat:@"Création refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    [self updateSelectionInfo];
    // The new style is the one the next "apply" is meant for.
    const NSUInteger row = [self.visibleCaptionStyleIds
        indexOfObject:[NSString stringWithUTF8String:style.id.c_str()]];
    if (row != NSNotFound)
        [self.captionStyleTable
                selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
}

- (void)deleteCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state) return;
    const Ulid styleId = [self selectedCaptionStyleId];
    if (styleId.empty()) return;
    int32_t cartons = 0;
    int32_t others = 0;
    for (const auto& summary :
         ui::media_panel::SummarizeCaptionStyles(self.state->document.sequence))
        if (summary.style_id == styleId) {
            cartons = summary.carton_count;
            others = summary.other_clip_count;
        }
    if (cartons > 0) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Ce style habille %d carton%@ : donnez-leur "
                             @"un autre style avant de le supprimer.",
                             cartons, cartons == 1 ? @"" : @"s"];
        return;
    }
    // F2.3 -- pictures that carry the style lose it in the same edit.
    const auto removal =
        ui::media_panel::CaptionStyleRemoval(self.state->document, styleId);
    if (!removal) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"%d plans portent ce style : trop pour une "
                             @"seule édition.",
                             others];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:*removal
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    [self updateSelectionInfo];
    if (others > 0)
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Style supprimé, et retiré de %d plan%@ qui le "
                             @"portai%@ sans l’afficher.",
                             others, others == 1 ? @"" : @"s",
                             others == 1 ? @"t" : @"ent"];
}

// Dresses the selected cartons in `styleId`, or in the default look when it
// is empty, as one undoable edit.
- (void)restyleSelectedCartons:(const Ulid&)styleId {
    if (!self.state || !self.state->interaction) return;
    const ui::media_panel::CaptionSelection selection =
        ui::media_panel::DescribeCaptionSelection(
            self.state->document, self.state->interaction->SelectedClipIds(),
            styleId);
    if (selection.cartons.size() > kMaxBatchOperations) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"%zu cartons sélectionnés : au plus %zu en une "
                             @"fois.",
                             selection.cartons.size(), kMaxBatchOperations];
        return;
    }
    const auto batch = ui::media_panel::RestyleCartons(
        self.state->document, selection.cartons, styleId);
    if (!batch) return;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{*batch}
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Style refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
}

- (void)applyCaptionStylePressed:(id)sender {
    (void)sender;
    const Ulid styleId = [self selectedCaptionStyleId];
    if (!styleId.empty()) [self restyleSelectedCartons:styleId];
}

- (void)clearCaptionPressed:(id)sender {
    (void)sender;
    [self restyleSelectedCartons:Ulid{}];
}

@end
