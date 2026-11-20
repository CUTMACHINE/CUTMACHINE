// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Trim mode
// (FILMCRAFT FC4, ONGLETS ON8 and ON9): the Trim Monitor's two frames and
// their gestures, the trim bar, the side and the nudges, dynamic trim, play
// around, and their menu items. TrimSession.h decides; these methods relay.
// Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

// ON9 -- the Trim Monitor under the hand, as Resolve's Trim Editor
// (manual 21.1, p. 1067) and Avid's two-up: a press on the left image takes
// A, on the right image B, on the divide between them a roll; the drag
// moves it, six points a frame, previewed on the monitor and the timeline,
// and the release writes it in one edit. Past what the clips can give, it
// holds at the last frame that applies.
static constexpr CGFloat kTrimMonitorDivide = 24.0;
static constexpr CGFloat kTrimMonitorPointsPerFrame = 6.0;

@implementation AppDelegate (Trim)

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The Trim Monitor (Resolve's
// two-up, manual p. 1065-1067): while the trim mode holds points and nothing
// plays, Record shows the last frame of A and the first frame of B side by
// side, each in the pane LayoutTrimMonitor gives it, through the same
// textures and the same compositing as the program. What each side shows,
// its counters and its handle are TrimSession.h's; this asks the frames
// and keeps the panes. NO, and nothing kept, when Record is the program.
// FC4 -- the Trim Monitor's frames and words, on Record's display list:
// rectangles of the theme and text of the atlas, nothing else. Over each
// pane, its side and the frames trimmed since the point was selected (in
// the trim green once they moved); under it, what the trim can still take.
- (void)layOutTrimMonitor:(TimelineRenderData&)programData {
    const std::vector<trim::TrimMonitorPane>& panes =
        self.state->trimMonitorPanes;
    for (size_t slot = 0; slot < panes.size(); ++slot) {
        const trim::TrimMonitorPane& pane = panes[slot];
        programData.video_framings[slot] = MonitorFraming{
            0.0f, 0.0f, 1.0f, 1.0f, pane.x, pane.y, pane.width, pane.height};
        programData.video_opacities[slot] = 1.0f;
        programData.video_veils[slot] = MonitorVeil{};
        // EF3 -- and sharp: the cut is judged on the frames themselves.
        if (slot < programData.video_blurs.size())
            programData.video_blurs[slot] = 0.0f;
        // PA8 -- and whole, for the same reason.
        if (slot < programData.video_masks.size())
            programData.video_masks[slot] = MonitorMask{};
    }
    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    const NSRect bounds = self.programMonitorView.bounds;
    const double layerScale = self.programMonitorView.layer.contentsScale;
    const double zoomPoints =
        self.state->programMonitorZoom > 0.0 && layerScale > 0.0
            ? self.state->programMonitorZoom / layerScale
            : 0.0;
    const ui::caption_overlay::Layout canvas = ui::caption_overlay::Compute(
        sequenceWidth, sequenceHeight, bounds.size.width, bounds.size.height,
        zoomPoints);
    const trim::TrimMonitorText text =
        trim::DescribeTrimMonitorText(self.state->trimMonitorView);
    const double band = ui::theme::kFontSizeSmall + (2.0 * ui::theme::kSpaceXs);
    const ui::theme::Color shade =
        ui::theme::WithAlpha(ui::theme::kSurfaceSunken, 0.78f);
    // The two halves of the sequence frame, whatever the panes' pictures.
    const double half = canvas.canvas_width / 2.0;
    programData.overlays.emplace_back(
        MetalRect{canvas.canvas_x + half - 0.5, canvas.canvas_y, 1.0,
                  canvas.canvas_height, ui::theme::kBorderStrong,
                  ui::theme::kBorderStrong});
    const auto words = [&](double x, double y, double width,
                           const std::string& line, ui::theme::Color color) {
        if (line.empty() || width <= 0.0) return;
        programData.overlays.emplace_back(
            MetalText{x, y, width, ui::theme::kFontSizeSmall, line, color,
                      MetalFontFace::Mono, true});
    };
    const std::array<const trim::TrimMonitorSide*, 2> sides = {
        &self.state->trimMonitorView.outgoing,
        &self.state->trimMonitorView.incoming};
    const std::array<int64_t, 2> shifts = {
        self.state->trimMonitorView.out_shift_frames,
        self.state->trimMonitorView.in_shift_frames};
    const std::array<trim::TrimMonitorPane, 2> sidePanes = {
        self.state->trimMonitorLayout.outgoing,
        self.state->trimMonitorLayout.incoming};
    for (size_t index = 0; index < 2; ++index) {
        const double x = canvas.canvas_x + (index * half);
        // The words hug the picture, above and under it, within the frame;
        // a side without a picture has them at the frame's edges.
        const trim::TrimMonitorPane& pane = sidePanes[index];
        const double frameBottom = canvas.canvas_y + canvas.canvas_height;
        const double paneTop = pane.empty()
                                   ? canvas.canvas_y
                                   : canvas.canvas_y + (pane.y * canvas.scale);
        const double paneBottom =
            pane.empty() ? frameBottom : paneTop + (pane.height * canvas.scale);
        const double top = std::max(canvas.canvas_y, paneTop - band);
        const double bottom = std::min(frameBottom - band, paneBottom);
        programData.overlays.emplace_back(
            MetalRect{x, top, half, band, shade, shade});
        programData.overlays.emplace_back(
            MetalRect{x, bottom, half, band, shade, shade});
        const double inset = ui::theme::kSpaceS;
        const std::string& title =
            index == 0 ? text.outgoing_title : text.incoming_title;
        const std::string& shift =
            index == 0 ? text.outgoing_shift : text.incoming_shift;
        const std::string& handle =
            index == 0 ? text.outgoing_handle : text.incoming_handle;
        const double titleWidth = std::max(0.0, half * 0.6 - inset);
        words(x + inset, top + ui::theme::kSpaceXs, titleWidth, title,
              ui::theme::kTextPrimary);
        words(x + (half * 0.6), top + ui::theme::kSpaceXs,
              std::max(0.0, (half * 0.4) - inset), shift,
              shifts[index] != 0 ? ui::theme::kTrimPoint
                                 : ui::theme::kTextSecondary);
        if (sides[index]->present)
            words(x + inset, bottom + ui::theme::kSpaceXs,
                  std::max(0.0, half - (2.0 * inset)), handle,
                  ui::theme::kTextSecondary);
    }
}

- (BOOL)resolveTrimMonitorSlots {
    self.state->trimMonitorPanes.clear();
    self.state->trimMonitorView = {};
    self.state->trimMonitorLayout = {};
    if (!self.state->trimSession.Active() || self.state->playbackDirection != 0)
        return NO;
    const Document& shown = [self trimShownDocument];
    const trim::TrimMonitorView view =
        trim::DescribeTrimMonitor(shown, self.state->trimSession);
    if (!view.active) return NO;
    const int32_t sequenceWidth = shown.sequence.width;
    const int32_t sequenceHeight = shown.sequence.height;
    const std::array<const trim::TrimMonitorSide*, 2> sides = {&view.outgoing,
                                                               &view.incoming};
    std::array<const DocumentClip*, 2> clips = {nullptr, nullptr};
    std::array<std::pair<int64_t, int64_t>, 2> sizes = {};
    for (size_t index = 0; index < sides.size(); ++index) {
        const trim::TrimMonitorSide& side = *sides[index];
        if (!side.present || !side.has_source || side.source_frame < 0)
            continue;
        const DocumentClip* clip = shown.FindClip(side.clip_id);
        // A motion clip is drawn, not decoded, from its composition's own
        // frames: the monitor leaves its pane empty rather than guess.
        if (clip == nullptr || IsMotionClip(*clip)) continue;
        int64_t width = 0;
        int64_t height = 0;
        std::string error;
        // A rush never probed has no known size: fitted as the sequence.
        if (!ClipDisplayedSize(shown, *clip, width, height, error)) {
            width = sequenceWidth;
            height = sequenceHeight;
        }
        clips[index] = clip;
        sizes[index] = {width, height};
    }
    const trim::TrimMonitorLayout layout = trim::LayoutTrimMonitor(
        sequenceWidth, sequenceHeight, sizes[0].first, sizes[0].second,
        sizes[1].first, sizes[1].second);
    const std::array<trim::TrimMonitorPane, 2> panes = {layout.outgoing,
                                                        layout.incoming};
    for (size_t index = 0; index < sides.size(); ++index) {
        if (clips[index] == nullptr || panes[index].empty()) continue;
        const DocumentClip& clip = *clips[index];
        self.state->requested.push_back(
            {true, clip.source_id, sides[index]->source_frame, 1.0f, clip.id});
        self.state->trimMonitorPanes.push_back(panes[index]);
        self.state->readSourceIds.insert(clip.source_id);
        [self requestFrame:sides[index]->source_frame ofSource:clip.source_id];
    }
    self.state->trimMonitorView = view;
    self.state->trimMonitorLayout = layout;
    self.state->overlayDirty = true;
    return YES;
}

// Selection (A) resizes, Trim (T) ripples (Resolve 21.1 manual, p.
// 1061-1062); linked partners follow while the linked selection is on.
- (void)syncTrimStyle {
    self.state->trimSession.SetStyle([self effectiveTool] == TimelineTool::Trim
                                         ? trim::TrimStyle::Ripple
                                         : trim::TrimStyle::Trim);
    self.state->trimSession.SetFollowLinks(self.state->linkedSelection);
}

- (const Document&)trimShownDocument {
    return self.state->trimPreview ? *self.state->trimPreview
                                   : self.state->document;
}

- (void)showTrimStatus {
    const trim::TrimMonitorView view = trim::DescribeTrimMonitor(
        [self trimShownDocument], self.state->trimSession);
    if (!view.active) return;
    const trim::TrimMonitorText text = trim::DescribeTrimMonitorText(view);
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ · A %@ · B %@%@", CMStringFromUtf8(text.status),
                         CMStringFromUtf8(text.outgoing_shift),
                         CMStringFromUtf8(text.incoming_shift),
                         self.state->dynamicTrimMode ? @" · W" : @""];
    [self refreshTrimBar];
}

- (void)showTrimRefusal:(trim::TrimRefusal)refusal {
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Trim refusé : %@",
                                   CMStringFromUtf8(
                                       trim::TrimRefusalDescription(refusal))];
}

- (void)leaveTrimMode {
    if (!self.state->trimSession.Active() && !self.state->trimPreview) return;
    self.state->trimSession.Clear();
    self.state->trimPreview.reset();
    self.state->overlayDirty = true;
    // Record is the program again.
    [self requestResolvedPosition:self.state->requestedPosition];
    [self refreshTrimBar];
}

// ON9 -- docs/roadmap/ONGLETS-2026-10.md. The cursor of an edge, Resolve's:
// "]" on the outgoing clip's tail, "[" on the incoming clip's head, "]["
// where the cut rolls. A stretch keeps AppKit's double arrow.
- (NSCursor*)edgeCursorFor:(const TimelinePointerTarget&)target {
    if (TimelineTrimModeStretches(target.trim_mode))
        return NSCursor.resizeLeftRightCursor;
    if (target.trim_mode == TimelineTrimMode::Roll)
        return CMTrimCursor(CMTrimCursorRoll);
    return CMTrimCursor(target.edge == TimelineHitEdge::Tail ? CMTrimCursorOut
                                                             : CMTrimCursorIn);
}

// ON8 -- docs/roadmap/ONGLETS-2026-10.md. Between the transport letters
// (J K L end at 294 points) and the timecode (102 points from the right
// edge), in the tool row along the top of the timeline.
- (void)layOutTrimBar {
    if (!self.trimBarView || !self.metalView) return;
    const NSRect bounds = self.metalView.bounds;
    constexpr CGFloat kLeft = 304.0;
    constexpr CGFloat kRight = 108.0;
    self.trimBarView.frame =
        NSMakeRect(kLeft, bounds.size.height - kTimelineToolbarHeight,
                   std::max<CGFloat>(0.0, bounds.size.width - kLeft - kRight),
                   kTimelineToolbarHeight);
}

// What the bar shows now: the trim mode's state, with the keys bound to its
// commands as the menus write them.
- (void)refreshTrimBar {
    if (!self.trimBarView || !self.state) return;
    NSDictionary<NSString*, NSString*>* bindings = self.shortcutBindings;
    const trim::TrimBarModel model = trim::DescribeTrimBar(
        self.state->trimSession, self.state->document,
        [self effectiveTool] == TimelineTool::Trim, self.state->dynamicTrimMode,
        [bindings](const std::string& command) {
            NSString* spec = bindings[@(command.c_str())];
            return spec ? ui::shortcuts::DisplaySpec(spec.UTF8String ?: "")
                        : std::string();
        });
    [self.trimBarView showModel:model];
}

// ON9 -- Resolve's Play Around Current Selection (manual 21.1, p. 1070):
// from two seconds before to two seconds after the selected edit point, the
// selected clips, or the playhead, then back to where it started -- the
// cut, judged again after each nudge. Resolve's pre- and post-roll are
// preferences; these two seconds are not yet.
- (void)playAroundSelection {
    if (!self.state || self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    const MediaRate rate = self.state->document.sequence.frame_rate;
    const RationalTime roll{2, 1};
    RationalTime from = self.state->requestedPosition;
    RationalTime to = from;
    NSString* what = @"la tête de lecture";
    const trim::TrimMonitorView view = trim::DescribeTrimMonitor(
        self.state->document, self.state->trimSession);
    if (view.active) {
        from = to = view.edit_time;
        what = @"le raccord";
    } else if (!self.state->interaction->SelectedClipIds().empty()) {
        bool first = true;
        for (const Ulid& id : self.state->interaction->SelectedClipIds()) {
            const DocumentClip* clip = self.state->document.FindClip(id);
            if (!clip) continue;
            const RationalTime end = clip->timeline_in.add(clip->duration);
            from = first || clip->timeline_in < from ? clip->timeline_in : from;
            to = first || end > to ? end : to;
            first = false;
        }
        what = @"la sélection";
    }
    RationalTime start = from.sub(roll);
    if (start < RationalTime{0, 1}) start = {0, rate.num > 0 ? rate.num : 1};
    RationalTime end = to.add(roll);
    if (end > self.state->duration) end = self.state->duration;
    self.playAroundReturn =
        view.active ? view.edit_time : self.state->requestedPosition;
    self.playAroundStart = start;
    self.playAroundEnd = end;
    [self requestTimelinePosition:start];
    [self setPlaybackDirection:1];
    self.playAroundActive = YES;
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Lecture autour de %@%@", what,
                                   self.loopPlayback ? @", en boucle" : @""];
}

// Past the window's end: again from its start in a loop, otherwise stopped
// and the playhead back where it was asked from.
- (void)checkPlayAround {
    if (!self.playAroundActive || !self.state ||
        self.state->playbackDirection <= 0 ||
        self.state->requestedPosition < self.playAroundEnd)
        return;
    if (self.loopPlayback) {
        const RationalTime start = self.playAroundStart;
        const RationalTime end = self.playAroundEnd;
        const RationalTime back = self.playAroundReturn;
        [self requestTimelinePosition:start];
        [self setPlaybackDirection:1];
        self.playAroundStart = start;
        self.playAroundEnd = end;
        self.playAroundReturn = back;
        self.playAroundActive = YES;
        return;
    }
    const RationalTime back = self.playAroundReturn;
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:back];
}

- (BOOL)trimMonitorPressAt:(NSPoint)point {
    if (!self.state || !self.state->trimSession.Active() ||
        self.state->trimMonitorPanes.empty() ||
        self.state->trimSession.Dynamic().active ||
        self.state->playbackDirection != 0)
        return NO;
    const CGFloat middle = NSMidX(self.programMonitorView.bounds);
    const trim::TrimSide side =
        point.x < middle - kTrimMonitorDivide   ? trim::TrimSide::A
        : point.x > middle + kTrimMonitorDivide ? trim::TrimSide::B
                                                : trim::TrimSide::Roll;
    if (self.state->trimSession.SetSide(self.state->document, side) !=
        trim::TrimRefusal::None) {
        [self showTrimRefusal:trim::TrimRefusal::NoJunction];
        return YES;
    }
    // The style the release will trim with, for the preview too.
    [self syncTrimStyle];
    self.trimMonitorDragging = YES;
    self.trimMonitorDragStartX = point.x;
    self.trimMonitorDragFrames = 0;
    self.state->trimPreview.reset();
    [self showTrimStatus];
    self.state->overlayDirty = true;
    return YES;
}

// The farthest of `wanted` frames, toward zero, the session's plan applies:
// a document showing it, or none at zero.
- (int64_t)trimMonitorPreviewFrames:(int64_t)wanted
                           document:(Document&)preview {
    for (int64_t frames = wanted; frames != 0; frames += wanted > 0 ? -1 : 1) {
        const trim::TrimEdit edit =
            self.state->trimSession.PlanNudge(self.state->document, frames);
        if (!edit.ok() || !edit.operation) continue;
        Document candidate = self.state->document;
        Operation operation = *edit.operation;
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        if (ApplyOperation(candidate, operation, inverse, error, message)) {
            preview = std::move(candidate);
            return frames;
        }
    }
    return 0;
}

- (void)trimMonitorDragTo:(NSPoint)point {
    if (!self.trimMonitorDragging || !self.state) return;
    const int64_t wanted = static_cast<int64_t>(std::lround(
        (point.x - self.trimMonitorDragStartX) / kTrimMonitorPointsPerFrame));
    Document preview;
    const int64_t frames = [self trimMonitorPreviewFrames:wanted
                                                 document:preview];
    if (frames == self.trimMonitorDragFrames) return;
    self.trimMonitorDragFrames = frames;
    if (frames == 0)
        self.state->trimPreview.reset();
    else
        self.state->trimPreview = std::move(preview);
    [self requestResolvedPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"TRIM MONITOR · %@ image%@ · relâcher pour écrire",
                         CMStringFromUtf8(trim::SignedFrames(frames)),
                         frames == 1 || frames == -1 ? @"" : @"s"];
    self.state->overlayDirty = true;
}

- (void)trimMonitorRelease {
    if (!self.trimMonitorDragging || !self.state) return;
    self.trimMonitorDragging = NO;
    const int64_t frames = self.trimMonitorDragFrames;
    self.trimMonitorDragFrames = 0;
    self.state->trimPreview.reset();
    if (frames != 0) {
        [self syncTrimStyle];
        [self applyTrimEdit:self.state->trimSession.PlanNudge(
                                self.state->document, frames)];
    } else {
        [self requestResolvedPosition:self.state->requestedPosition];
        [self showTrimStatus];
    }
    self.state->overlayDirty = true;
}

- (void)menuPlayAround:(id)sender {
    (void)sender;
    [self playAroundSelection];
}

- (void)menuToggleLoop:(id)sender {
    (void)sender;
    self.loopPlayback = !self.loopPlayback;
    self.infoLabel.stringValue = self.loopPlayback
                                     ? @"Lecture en boucle : activée"
                                     : @"Lecture en boucle : désactivée";
}

- (void)trimBar:(CMTrimBarView*)bar didPress:(NSString*)action {
    (void)bar;
    const std::string name = action.UTF8String ?: "";
    if (name == "select")
        [self trimSelectNearest];
    else if (name == "side.a")
        [self setTrimSide:trim::TrimSide::A];
    else if (name == "side.roll")
        [self setTrimSide:trim::TrimSide::Roll];
    else if (name == "side.b")
        [self setTrimSide:trim::TrimSide::B];
    else if (name == "nudge.-5")
        [self trimNudge:-trim::kLargeTrimFrames];
    else if (name == "nudge.-1")
        [self trimNudge:-1];
    else if (name == "nudge.+1")
        [self trimNudge:1];
    else if (name == "nudge.+5")
        [self trimNudge:trim::kLargeTrimFrames];
    else if (name == "extend")
        [self trimExtendToPlayhead];
    else if (name == "dynamic")
        [self toggleDynamicTrim];
    else if (name == "leave")
        [self leaveTrimMode];
    else if (name == "play.around")
        [self playAroundSelection];
    [self refreshTrimBar];
    // The keys go on reaching the timeline.
    [self.window makeFirstResponder:self.metalView];
}

// The bar's A, Roll and B: the side chosen outright, as U cycles to it.
- (void)setTrimSide:(trim::TrimSide)side {
    const trim::TrimRefusal refusal =
        self.state->trimSession.SetSide(self.state->document, side);
    self.state->overlayDirty = true;
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    [self showTrimStatus];
}

// Applies what the session planned; a refusal is said, nothing written.
- (BOOL)applyTrimEdit:(const trim::TrimEdit&)edit {
    if (!edit.ok()) {
        if (!edit.detail.empty())
            std::fprintf(stderr, "Trim refused (%s): %s\n",
                         trim::TrimRefusalName(edit.refusal),
                         edit.detail.c_str());
        [self showTrimRefusal:edit.refusal];
        return NO;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:*edit.operation
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Trim rejected (%s): %s\n", EditErrorName(error),
                     message.c_str());
        [self showTrimRefusal:trim::TrimRefusal::Rejected];
        return NO;
    }
    self.state->trimSession.RecordApplied(edit);
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    [self showTrimStatus];
    self.state->overlayDirty = true;
    return YES;
}

// V -- the edit point nearest the playhead on the targeted tracks, as a
// roll, and the playhead goes to it (manual, p. 1069).
- (void)trimSelectNearest {
    if (self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    [self setPlaybackDirection:0];
    RationalTime editTime;
    const trim::TrimRefusal refusal = self.state->trimSession.SelectNearestEdit(
        self.state->document, self.state->requestedPosition,
        [self targetedTrackList], trim::TrimSide::Roll, editTime);
    self.state->trimPreview.reset();
    self.state->overlayDirty = true;
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return;
    }
    [self syncTrimStyle];
    [self requestTimelinePosition:editTime];
    [self showTrimStatus];
}

// U -- outgoing, centred, incoming (manual, p. 1070).
- (void)trimCycleSide {
    const trim::TrimRefusal refusal =
        self.state->trimSession.CycleSide(self.state->document);
    self.state->overlayDirty = true;
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    [self showTrimStatus];
}

// Comma, period, and with Shift five frames (manual, p. 1069-1070).
- (void)trimNudge:(int64_t)frames {
    if (self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    [self syncTrimStyle];
    [self applyTrimEdit:self.state->trimSession.PlanNudge(self.state->document,
                                                          frames)];
}

// E -- the selected points to the playhead (manual, p. 1088-1089).
- (void)trimExtendToPlayhead {
    if (self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    [self syncTrimStyle];
    [self
        applyTrimEdit:self.state->trimSession.PlanExtendToPlayhead(
                          self.state->document, self.state->requestedPosition)];
}

// The clock a dynamic trim is counted on, as an exact time: microseconds
// of the monotonic clock. TrimSession reads no clock of its own.
- (RationalTime)trimClockNow {
    const int64_t microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return RationalTime{microseconds, 1000000};
}

// W -- Resolve's Dynamic Trim mode (manual, p. 1085-1086). On, with no
// point selected, the edit nearest the playhead is; off, a trim still
// running is dropped and nothing is written.
- (void)toggleDynamicTrim {
    if (self.state->dynamicTrimMode) {
        self.state->dynamicTrimMode = false;
        if (self.state->trimSession.CancelDynamic()) {
            self.state->trimPreview.reset();
            [self requestResolvedPosition:self.state->requestedPosition];
        }
        self.state->overlayDirty = true;
        if (self.state->trimSession.Active())
            [self showTrimStatus];
        else
            self.infoLabel.stringValue = @"Trim dynamique désactivé";
        return;
    }
    [self setPlaybackDirection:0];
    self.state->dynamicTrimMode = true;
    if (!self.state->trimSession.Active()) [self trimSelectNearest];
    if (self.state->trimSession.Active())
        [self showTrimStatus];
    else
        self.infoLabel.stringValue =
            @"Trim dynamique : aucun point de montage, J K L lisent";
    self.state->overlayDirty = true;
}

// J (-1), L (+1) and K (0) while the dynamic mode holds points: they trim
// instead of playing, and K commits what was reached as one edit. NO when
// the keys are the transport's.
- (BOOL)dynamicTrimTransport:(int)direction {
    if (!self.state->dynamicTrimMode || !self.state->trimSession.Active())
        return NO;
    const RationalTime now = [self trimClockNow];
    if (direction == 0) {
        if (!self.state->trimSession.Dynamic().active) return YES;
        const trim::TrimEdit edit =
            self.state->trimSession.StopDynamic(self.state->document, now);
        self.state->trimPreview.reset();
        self.state->overlayDirty = true;
        if (edit.refusal == trim::TrimRefusal::NoMovement) {
            [self requestResolvedPosition:self.state->requestedPosition];
            [self showTrimStatus];
            return YES;
        }
        [self applyTrimEdit:edit];
        return YES;
    }
    if (!self.state->trimSession.Dynamic().active) [self syncTrimStyle];
    if (self.state->playbackDirection != 0) [self setPlaybackDirection:0];
    const trim::TrimRefusal refusal =
        self.state->trimSession.Shuttle(self.state->document, direction, now);
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return YES;
    }
    [self refreshDynamicTrim];
    return YES;
}

// While J or L runs: the offset follows the clock, and the timeline and
// Record draw the document as it would be.
- (void)refreshDynamicTrim {
    if (!self.state->trimSession.Dynamic().active) return;
    self.state->trimSession.Tick(self.state->document, [self trimClockNow]);
    Document preview;
    if (self.state->trimSession.PreviewDocument(self.state->document, preview))
        self.state->trimPreview = std::move(preview);
    else
        self.state->trimPreview.reset();
    [self requestResolvedPosition:self.state->requestedPosition];
    [self showTrimStatus];
    self.state->overlayDirty = true;
}

// A click on a clip's edge that moved nothing: its point, on the side the
// edge gives (Command rolls), added to the others with Shift.
- (trim::TrimRefusal)selectPressedEdgePoint {
    if (!self.state->pressTarget ||
        self.state->pressTarget->kind != TimelinePointerTargetKind::ClipEdge ||
        TimelineTrimModeStretches(self.state->pressTarget->trim_mode))
        return trim::TrimRefusal::None;
    const TimelinePointerTarget target = *self.state->pressTarget;
    const TrimEdge edge =
        target.edge == TimelineHitEdge::Head ? TrimEdge::Head : TrimEdge::Tail;
    if (self.state->trimSession.Dynamic().active)
        return trim::TrimRefusal::Rejected;
    const trim::TrimRefusal refusal = self.state->trimSession.SelectEdge(
        self.state->document, target.clip_id, edge,
        trim::TrimSideOfEdge(edge, target.trim_mode == TimelineTrimMode::Roll),
        self.state->pressShift);
    self.state->trimPreview.reset();
    self.state->overlayDirty = true;
    if (refusal == trim::TrimRefusal::None) {
        [self syncTrimStyle];
        [self requestResolvedPosition:self.state->requestedPosition];
    }
    return refusal;
}

- (void)menuTrimTool:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self setTimelineTool:TimelineTool::Trim];
}

- (void)menuTrimSelectNearest:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimSelectNearest];
}

- (void)menuTrimCycleSide:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimCycleSide];
}

- (void)menuTrimNudgeBack:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimNudge:-1];
}

- (void)menuTrimNudgeForward:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimNudge:1];
}

- (void)menuTrimNudgeBackMany:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimNudge:-trim::kLargeTrimFrames];
}

- (void)menuTrimNudgeForwardMany:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimNudge:trim::kLargeTrimFrames];
}

- (void)menuTrimExtend:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self trimExtendToPlayhead];
}

- (void)menuTrimDynamic:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self toggleDynamicTrim];
}

- (void)menuLeaveTrimMode:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self leaveTrimMode];
    self.infoLabel.stringValue = @"Mode trim quitté";
}

// PERF10 -- DecoderPool.h. Past the capacity, close the least recently used
// decoders that neither monitor shows and the next second of playback does
// not cross into. PERF12 -- nor one that playback reaches next, as last
// planned (-openDecodersAhead:).
- (void)trimDecoderPool {
    AppState* state = self.state;
    if (state->workers.size() <= state->decoderCapacity) return;
    std::set<std::string> needed(state->upcomingSourceIds.begin(),
                                 state->upcomingSourceIds.end());
    needed.insert(state->plannedSourceIds.begin(),
                  state->plannedSourceIds.end());
    for (const ResolvedSlot& slot : state->requested)
        if (slot.active) needed.insert(slot.sourceId);
    // PERF13 -- a layer hidden under an opaque shot is on screen all the
    // same: closed, it would reopen when the shot ends, seconds on this drive.
    for (const ResolvedSlot& slot : state->hiddenSlots)
        needed.insert(slot.sourceId);
    if (state->sourceMonitor && !state->sourceMonitorId.empty())
        needed.insert(state->sourceMonitorId);
    std::vector<std::string> open;
    open.reserve(state->workers.size());
    for (const auto& worker : state->workers) open.push_back(worker.first);
    for (const std::string& sourceId :
         state->decoderUsage.ToClose(open, needed, state->decoderCapacity))
        [self closeDecoderForSource:sourceId];
}

@end
